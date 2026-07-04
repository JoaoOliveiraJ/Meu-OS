// ============================================================================
//  win32kbase.c  —  Subsistema Win32 (lado kernel) — LADO BASE (modelo NT 6.4).
//
//  A partir do Windows 8.1 (NT 6.4), o win32k.sys do Windows foi DIVIDIDO em
//  win32kbase.sys + win32kfull.sys + win32k.sys (shim). A divisao isola o que
//  e ESSENCIAL ao desktop (window manager, message queue, foco, mouse/keyboard
//  routing, compose simples) do que e PESADO (GDI complexo, BitBlt, printing,
//  formatos avancados de DIB). Este arquivo e o win32kbase.
//
//  ATENCAO — modelo de inclusao: para nao introduzir nova ABI de link no
//  build atual (que junta tudo num so kernel.elf), este arquivo NAO e
//  compilado isolado. Ele e INCLUIDO via #include de dentro de win32k.c
//  (shim). O efeito e o mesmo de um translation unit unico, mas as funcoes
//  ficam fisicamente separadas por tema, igual ao NT 6.4+.
//
//  Conteudo deste BASE:
//    - helpers de desenho de baixo nivel (w32k_clear/fill_rect/hline/vline/rect/
//      draw_char/draw_text + adaptador GPU LFB vs VGA mode13h)
//    - janelas (arvore HWND + z-order + foco + reaping)
//    - fila de mensagens (WM_*) + GetMessage/PostMessage/PostQuitMessage
//    - roteamento de teclado (win32k_on_key) + injecao sintetica (demo headless)
//    - composicao do desktop + barra de tarefas (draw_taskbar)
//    - GDI minimo do BASE: GetDC, GetStockObject, CreateSolidBrush
//
//  O GDI complexo (TextOut com cor, FillRect com brush real, DIB) fica em
//  win32kfull.c.
// ============================================================================

// ---- Cores (indices da paleta do mode 13h, ver video.h) ----
#define DESKTOP_COLOR    FB_BLUE
#define TITLE_ACTIVE     FB_BLUE
#define TITLE_INACTIVE   FB_DARK_GRAY
#define TITLE_TEXT       FB_WHITE
#define CLIENT_COLOR     FB_LIGHT_GRAY
#define FRAME_COLOR      FB_BLACK
#define TITLE_H          28          // altura da barra de titulo (caption Win10/11)

// ---- FASE 6: barra de tarefas (estilo Windows, no rodape do desktop) ----
#define TASKBAR_H        14          // altura da barra de tarefas
#define TASKBAR_COLOR    FB_DARK_GRAY
#define TASKBAR_TEXT     FB_WHITE
#define START_COLOR      FB_GREEN    // botao "Iniciar"
#define TBBTN_ACTIVE     FB_LIGHT_GRAY  // botao da janela com foco (afundado)
#define TBBTN_INACTIVE   FB_DARK_GRAY

// ============================================================================
//  FASE 12 — Forward declarations da SHELL Windows 10. As DEFINICOES vivem em
//  win32kshell.c (incluido por win32k.c apos win32kbase.c e win32kfull.c).
//  Como tudo termina no mesmo translation unit, basta declarar aqui.
// ============================================================================
void win32k_shell_compose(void);
int  win32k_shell_on_mouse(int x, int y, uint32_t buttons, uint32_t prev_buttons);

// ============================================================================
//  FASE 9.2 — Backend de pixels do win32k (GPU 32 bpp OU VGA mode13h 8 bpp).
//
//  O Window Manager mantem a mesma logica (HWND, z-order, fila de mensagens):
//  so o desenho efetivo dos pixels e que escolhe o backend em runtime. Se o
//  driver de GPU (Bochs VBE, drivers/video/gpu.c) inicializou (gpu_active()),
//  usamos gpu_* (LFB 32 bpp, ate 1024x768); senao caimos para o mode 13h
//  (fb_* em 320x200x8, paleta DOS).
//
//  Para nao quebrar o codigo existente, cada cor e passada como uint8_t (indice
//  de paleta) e CONVERTIDA via palette_8_to_32 antes de ir para gpu_*. Assim a
//  semantica visual e identica nos dois backends — "FB_BLUE" continua sendo o
//  mesmo azul nas duas telas.
//
//  ensure_screen() inicializa o backend correto sob demanda (lazy). Os getters
//  scr_width/scr_height refletem o backend ativo (1024/768 ou 320/200), o que
//  permite a janela "wallpaper" preencher a tela inteira na nova resolucao.
// ============================================================================
static int w32k_use_gpu(void) { return gpu_active(); }

static int scr_width(void)  { return w32k_use_gpu() ? (int)gpu_width()  : FB_WIDTH;  }
static int scr_height(void) { return w32k_use_gpu() ? (int)gpu_height() : FB_HEIGHT; }

// FASE 11 — Resolucao publica do backend ativo (usada pelo driver de mouse
// para inicializar o cursor no centro da tela).
int win32k_screen_width(void)  { return scr_width(); }
int win32k_screen_height(void) { return scr_height(); }

static void w32k_clear(uint8_t color8) {
    if (w32k_use_gpu()) gpu_clear(palette_8_to_32(color8));
    else                fb_clear(color8);
}
static void w32k_fill_rect(int x, int y, int w, int h, uint8_t color8) {
    if (w32k_use_gpu()) gpu_fill_rect(x, y, w, h, palette_8_to_32(color8));
    else                fb_fill_rect(x, y, w, h, color8);
}
static void w32k_hline(int x, int y, int w, uint8_t color8) {
    if (w32k_use_gpu()) gpu_fill_rect(x, y, w, 1, palette_8_to_32(color8));
    else                fb_hline(x, y, w, color8);
}
static void w32k_vline(int x, int y, int h, uint8_t color8) {
    if (w32k_use_gpu()) gpu_fill_rect(x, y, 1, h, palette_8_to_32(color8));
    else                fb_vline(x, y, h, color8);
}
static void w32k_rect(int x, int y, int w, int h, uint8_t color8) {
    if (w <= 0 || h <= 0) return;
    w32k_hline(x, y, w, color8);
    w32k_hline(x, y + h - 1, w, color8);
    w32k_vline(x, y, h, color8);
    w32k_vline(x + w - 1, y, h, color8);
}

// Desenha UM caractere 8x8 usando o glifo embutido. bg=0xFF => transparente.
// No backend GPU pintamos pixel a pixel via gpu_pixel (32 bpp). No backend
// mode13h delegamos ao fb_draw_char (mesma fonte, escrita no LFB de 8 bpp).
static void w32k_draw_char(int x, int y, char c, uint8_t fg8, uint8_t bg8) {
    if (!w32k_use_gpu()) { fb_draw_char(x, y, c, fg8, bg8); return; }

    unsigned char uc = (unsigned char)c;
    if (uc < 0x20 || uc > 0x7F) uc = '?';
    const uint8_t* glyph = g_font8x8[uc - 0x20];
    uint32_t fg = palette_8_to_32(fg8);
    uint32_t bg = palette_8_to_32(bg8);
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))      gpu_pixel(x + col, y + row, fg);
            else if (bg8 != 0xFF)           gpu_pixel(x + col, y + row, bg);
        }
    }
}
static void w32k_draw_text(int x, int y, const char* s, uint8_t fg8, uint8_t bg8) {
    if (!w32k_use_gpu()) { fb_draw_text(x, y, s, fg8, bg8); return; }

    int cx = x, cy = y;
    int W = scr_width();
    while (*s) {
        char c = *s++;
        if (c == '\n') { cx = x; cy += 8; continue; }
        w32k_draw_char(cx, cy, c, fg8, bg8);
        cx += 8;
        if (cx + 8 > W) { cx = x; cy += 8; }
    }
}

// ============================================================================
//  Camada TRUE-COLOR (32 bpp) — a base do visual moderno (Windows 10/11).
//
//  As funcoes w32k_* acima falam em indices de paleta (16 cores do mode13h);
//  boas para compat, pobres para um desktop bonito. Quando o backend e o LFB
//  32 bpp (gpu_active), a shell usa ESTAS funcoes: cores XRGB reais, gradientes,
//  sombras (alpha-blend) e texto escalado/com sombra. Sem efeito no mode13h
//  (a shell cai no caminho de paleta antigo nesse fallback).
// ============================================================================

// ---- Paleta Windows 10/11 (0x00RRGGBB) ----
#define UI_WALL_TOP     0x00123A6Eu   // topo do wallpaper (azul profundo)
#define UI_WALL_MID     0x001E5FA8u   // meio (brilho)
#define UI_WALL_BOT     0x000C2748u   // base (mais escuro)
#define UI_TASKBAR      0x00202124u   // barra de tarefas (quase preto Win10)
#define UI_TASKBAR_HI   0x003A3D42u   // realce superior da taskbar (1px)
#define UI_ACCENT       0x000078D7u   // azul de acento do Windows 10
#define UI_ACCENT_LT    0x002B88E0u   // acento claro (topo do gradiente)
#define UI_ACCENT_DK    0x00005A9Eu   // acento escuro (base/borda)
#define UI_TITLE_INACT1 0x004A4D52u   // caption inativa (topo)
#define UI_TITLE_INACT2 0x003A3D42u   // caption inativa (base)
#define UI_CLIENT       0x00F3F3F3u   // area cliente (branco Win10)
#define UI_TEXT_LT      0x00FFFFFFu   // texto claro
#define UI_TEXT_DK      0x00202020u   // texto escuro
#define UI_TEXT_DIM     0x00C8C8C8u   // texto esmaecido
#define UI_SHADOW       0x00000000u   // sombra (alpha-blend preto)
#define UI_CLOSE_HOVER  0x00E81123u   // X hover (vermelho Win10)
#define UI_BTN_HOVER    0x00505357u   // botao caption hover (cinza)
#define UI_START_TILE   0x0037A0EBu   // ladrilhos do logo Iniciar

// Um glifo 8x8 da fonte embutida, desenhado em cor XRGB e escala inteira.
static void w32k_rgb_glyph(int x, int y, char c, uint32_t fg, int scale) {
    unsigned char uc = (unsigned char)c;
    if (uc < 0x20 || uc > 0x7F) uc = '?';
    const uint8_t* glyph = g_font8x8[uc - 0x20];
    if (scale <= 1) {
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col)) gpu_pixel(x + col, y + row, fg);
        }
        return;
    }
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++)
            if (bits & (0x80 >> col))
                gpu_fill_rect(x + col * scale, y + row * scale, scale, scale, fg);
    }
}

// Texto XRGB numa escala inteira (1 = 8px, 2 = 16px, ...).
static void w32k_rgb_text(int x, int y, const char* s, uint32_t fg, int scale) {
    int cx = x;
    while (*s) {
        char c = *s++;
        if (c == '\n') { cx = x; y += 8 * scale; continue; }
        w32k_rgb_glyph(cx, y, c, fg, scale);
        cx += 8 * scale;
    }
}

// Texto XRGB com sombra 1px deslocada (legibilidade sobre gradientes/caption).
static void w32k_rgb_text_sh(int x, int y, const char* s, uint32_t fg, uint32_t shadow, int scale) {
    w32k_rgb_text(x + 1, y + 1, s, shadow, scale);
    w32k_rgb_text(x,     y,     s, fg,     scale);
}

// Largura em pixels de uma string na fonte 8x8 escalada.
static int w32k_text_w(const char* s, int scale) {
    int n = 0; while (s[n]) n++;
    return n * 8 * scale;
}

// Retangulo XRGB solido (atalho tipado p/ a shell).
static void w32k_rgb_rect(int x, int y, int w, int h, uint32_t rgb) {
    gpu_fill_rect(x, y, w, h, rgb);
}
// Contorno XRGB de 1px.
static void w32k_rgb_frame(int x, int y, int w, int h, uint32_t rgb) {
    if (w <= 0 || h <= 0) return;
    gpu_fill_rect(x, y, w, 1, rgb);
    gpu_fill_rect(x, y + h - 1, w, 1, rgb);
    gpu_fill_rect(x, y, 1, h, rgb);
    gpu_fill_rect(x + w - 1, y, 1, h, rgb);
}
// Sombra suave (drop shadow) atras de uma janela: aneis de preto translucido
// que crescem para fora, deslocados um pouco para baixo (luz vinda de cima).
static void w32k_rgb_shadow(int x, int y, int w, int h) {
    const int N = 7;                 // espessura da sombra
    for (int i = N; i >= 1; i--) {
        uint8_t a = (uint8_t)(6 + (N - i) * 5);   // mais forte perto da janela
        int off = i;
        gpu_blend_rect(x - off, y - off + 3, w + 2 * off, h + 2 * off, UI_SHADOW, a);
    }
}

// ============================================================================
//  Janelas (arvore + z-order)
// ============================================================================
#define MAX_WINDOWS 16
#define MAX_CLASSES 16

typedef struct _WND {
    int      used;
    uint32_t id;            // HWND = (void*)(uintptr_t)id  (1, 2, 3, ...)
    int      x, y, w, h;    // retangulo da janela (coordenadas do desktop)
    char     title[48];
    char     className[32];
    void*    wndProc;       // WNDPROC (ring 3) — guardado p/ depuracao
    int      visible;
    int      z;             // ordem de empilhamento (maior = mais ao topo)
    uint8_t  bgColor;       // cor da area cliente (indice de paleta)
    uint32_t flags;         // WNDF_* (FASE 6): WNDF_DESKTOP / WNDF_CONSOLE
    void*    owner;         // EPROCESS dono (p/ reaping ao processo encerrar)
    // "Pintura pendente" da area cliente, desenhada pelo wndproc no WM_PAINT.
    // Guardamos o ultimo texto/preenchimento para o compositing redesenhar.
} WND;

static WND      s_wins[MAX_WINDOWS];
static int      s_nwin;
static uint32_t s_next_id = 1;
static int      s_next_z  = 1;
static uint32_t s_focus_id;        // janela com foco (recebe o teclado)
static int      s_was_active;      // 1 se a GUI ja compos a tela alguma vez
                                   //   (mantem o framebuffer no estado da GUI)

// FASE 11 — Cursor do mouse. Atualizado por win32k_on_mouse_event (chamado
// pela IRQ12 do PS/2). NtUserGetCursorPos/SetCursorPos leem/escrevem aqui.
static int32_t  s_cursor_x = 160;
static int32_t  s_cursor_y = 100;
static uint32_t s_cursor_buttons = 0;  // estado anterior dos botoes (p/ detectar mudancas)
static int      s_cursor_initialized = 0;

// Guard que serializa TODA escrita composta no framebuffer (compose, refresh da
// taskbar e move do cursor de software). test-and-set: um pedido concorrente e'
// descartado. Declarado cedo pois o cursor de software (acima do compose) o usa.
static volatile int s_composing = 0;

// Classes registradas (so o nome; o wndproc real fica no user32 em ring 3).
typedef struct _WNDCLASS_K { int used; char name[32]; void* wndProc; } WNDCLASS_K;
static WNDCLASS_K s_classes[MAX_CLASSES];

static int str_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
static void str_cpy(char* d, const char* s, int max) {
    int i = 0;
    if (s) while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static WND* wnd_from_id(uint32_t id) {
    if (!id) return 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (s_wins[i].used && s_wins[i].id == id) return &s_wins[i];
    return 0;
}
static WND* wnd_from_handle(void* h) { return wnd_from_id((uint32_t)(uintptr_t)h); }

int win32k_has_windows(void) { return s_nwin > 0; }

// Garante que ALGUM backend grafico esta ligado antes de desenhar. Se a GPU
// (Bochs VBE / LFB 32 bpp) ja foi inicializada pelo kmain, usa ela; senao
// liga o mode 13h (320x200x8) sob demanda. Idempotente.
static void ensure_fb(void) {
    if (w32k_use_gpu()) return;          // GPU ja ativa: nada a fazer
    if (!fb_active()) fb_init();         // fallback: VGA mode13h
}

// ============================================================================
//  Fila de mensagens (uma fila global — uma thread GUI por enquanto)
// ============================================================================
// Capacidade 256: a FASE 6 injeta varias dezenas de teclas (WM_KEYDOWN+WM_CHAR)
// para 2 janelas de cmd ANTES de drenar a fila no loop; 64 estourava e perdia o
// WM_QUIT. 256 acomoda a rajada do demo com folga.
#define MSG_QUEUE_CAP 256
static W32_MSG s_queue[MSG_QUEUE_CAP];
static volatile int s_q_head;   // proximo a remover
static volatile int s_q_tail;   // proximo a inserir
static volatile int s_q_count;

// Coalescing de WM_PAINT (igual ao Windows: WM_PAINT e "nivelado" — varias
// invalidacoes viram UMA so pintura pendente). Sem isto, cada WM_CHAR posta um
// WM_PAINT e a fila enche de pinturas redundantes (e nunca esvazia). Verdadeiro
// se ja ha um WM_PAINT pendente para 'hwnd' na fila.
static int paint_already_queued(void* hwnd) {
    for (int k = 0, i = s_q_head; k < s_q_count; k++, i = (i + 1) % MSG_QUEUE_CAP)
        if (s_queue[i].message == WM_PAINT && s_queue[i].hwnd == hwnd) return 1;
    return 0;
}

static void queue_post(void* hwnd, uint32_t msg, uint64_t wParam, uint64_t lParam) {
    if (s_q_count >= MSG_QUEUE_CAP) return;          // fila cheia: descarta
    if (msg == WM_PAINT && paint_already_queued(hwnd)) return;   // coalesce WM_PAINT
    W32_MSG* m = &s_queue[s_q_tail];
    m->hwnd = hwnd; m->message = msg; m->wParam = wParam; m->lParam = lParam;
    m->time = (uint32_t)g_ticks; m->pt.x = 0; m->pt.y = 0;
    s_q_tail = (s_q_tail + 1) % MSG_QUEUE_CAP;
    s_q_count++;
}
static int queue_pop(W32_MSG* out) {
    if (s_q_count <= 0) return 0;
    *out = s_queue[s_q_head];
    s_q_head = (s_q_head + 1) % MSG_QUEUE_CAP;
    s_q_count--;
    return 1;
}

// ============================================================================
//  GDI: HDC e HBRUSH como objetos do Object Manager
// ============================================================================
typedef struct _W32_DC {
    uint32_t windowId;     // janela dona (0 = tela toda / desktop)
    uint32_t reserved;
} W32_DC;
typedef struct _W32_BRUSH {
    uint32_t color;        // indice de paleta
    uint32_t isStock;
} W32_BRUSH;

// HBRUSH stock pre-criados (GetStockObject). Mapeia o indice -> cor de paleta.
static void* s_stock_brush[6];

static uint8_t stock_color(int index) {
    switch (index) {
        case WHITE_BRUSH:  return FB_WHITE;
        case LTGRAY_BRUSH: return FB_LIGHT_GRAY;
        case GRAY_BRUSH:   return FB_DARK_GRAY;
        case DKGRAY_BRUSH: return FB_DARK_GRAY;
        case BLACK_BRUSH:  return FB_BLACK;
        default:           return FB_BLACK;
    }
}

// ============================================================================
//  Init (lado BASE — o shim win32k.c chama isto e em seguida loga o "split").
// ============================================================================
void win32kbase_init(void) {
    s_nwin = 0; s_next_id = 1; s_next_z = 1; s_focus_id = 0;
    s_q_head = s_q_tail = s_q_count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) s_wins[i].used = 0;
    for (int i = 0; i < MAX_CLASSES; i++) s_classes[i].used = 0;

    // Cria os HBRUSH stock como objetos GDI (uma vez).
    for (int i = 0; i < 6; i++) {
        W32_BRUSH* b = (W32_BRUSH*)ObCreateObject(OB_TYPE_EVENT, sizeof(W32_BRUSH), 0);
        if (b) { b->color = stock_color(i); b->isStock = 1; }
        s_stock_brush[i] = b;
    }
    kputs("[win32k] window manager iniciado (arvore HWND, fila de msg, GDI).\n");
}

// ============================================================================
//  Compositing: pinta o desktop + as janelas (z-order) no framebuffer
// ============================================================================
// Desenha a "moldura" de UMA janela (barra de titulo + corpo cliente + borda).
// O CONTEUDO da area cliente (texto/retangulos) e desenhado pelo wndproc via
// NtGdiTextOut/NtGdiFillRect (durante o WM_PAINT), entao aqui so pintamos o
// chrome e limpamos a area cliente.
static void draw_window_chrome(WND* wptr) {
    // Janela de fundo (papel de parede): so a area cliente, sem barra/moldura.
    if (wptr->flags & WNDF_DESKTOP) {
        w32k_fill_rect(wptr->x, wptr->y, wptr->w, wptr->h, wptr->bgColor);
        return;
    }
    int active = (wptr->id == s_focus_id);
    // corpo (area cliente) — cor da janela (cinza por padrao; preto p/ console)
    w32k_fill_rect(wptr->x, wptr->y, wptr->w, wptr->h, wptr->bgColor);
    // barra de titulo
    w32k_fill_rect(wptr->x, wptr->y, wptr->w, TITLE_H, active ? TITLE_ACTIVE : TITLE_INACTIVE);
    // texto do titulo
    w32k_draw_text(wptr->x + 3, wptr->y + 2, wptr->title, TITLE_TEXT, 0xFF);
    // moldura preta
    w32k_rect(wptr->x, wptr->y, wptr->w, wptr->h, FRAME_COLOR);
    // separador da barra de titulo
    w32k_hline(wptr->x, wptr->y + TITLE_H, wptr->w, FRAME_COLOR);
}

// Conta as janelas top-level visiveis (exclui o papel de parede). Usado pela
// barra de tarefas (um botao por janela "normal").
static int count_taskbar_windows(void) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (s_wins[i].used && s_wins[i].visible && !(s_wins[i].flags & WNDF_DESKTOP)) n++;
    return n;
}

// Desenha a barra de tarefas no rodape: botao "Iniciar" + um botao por janela
// (o da janela com foco fica destacado). Loga cada botao na serial.
static void draw_taskbar(void) {
    int W  = scr_width();
    int ty = scr_height() - TASKBAR_H;
    w32k_fill_rect(0, ty, W, TASKBAR_H, TASKBAR_COLOR);
    w32k_hline(0, ty, W, FB_WHITE);          // borda superior (relevo)

    // Botao "Iniciar".
    w32k_fill_rect(2, ty + 2, 40, TASKBAR_H - 4, START_COLOR);
    w32k_rect(2, ty + 2, 40, TASKBAR_H - 4, FRAME_COLOR);
    w32k_draw_text(6, ty + 3, "Iniciar", FB_BLACK, 0xFF);
    kputs("[win32k] taskbar: [Iniciar]");

    // Um botao por janela top-level (cabem mais botoes na nova resolucao).
    int bx = 46, bw = 52;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        WND* w = &s_wins[i];
        if (!w->used || !w->visible || (w->flags & WNDF_DESKTOP)) continue;
        if (bx + bw > W - 2) break;
        int active = (w->id == s_focus_id);
        w32k_fill_rect(bx, ty + 2, bw, TASKBAR_H - 4, active ? TBBTN_ACTIVE : TBBTN_INACTIVE);
        w32k_rect(bx, ty + 2, bw, TASKBAR_H - 4, FRAME_COLOR);
        // Titulo abreviado (cabe ~6 chars na fonte 8x8).
        char lbl[8]; int k = 0;
        for (; k < 6 && w->title[k]; k++) lbl[k] = w->title[k];
        lbl[k] = 0;
        w32k_draw_text(bx + 2, ty + 3, lbl, active ? FB_BLACK : FB_WHITE, 0xFF);
        kputs(" ["); kputs(lbl);
        if (active) kputs("*");        // * = janela com foco
        kputs("]");
        bx += bw + 2;
    }
    kputc('\n');
}

// ============================================================================
//  FASE 11 — Sprite do cursor (12x16 px, seta classica do NT).
//
//  Bitmap 12x16: 'X' = preto (corpo da seta), '.' = branco (borda), ' ' = nao
//  desenhar (transparente). A seta aponta para cima/esquerda (canto sup-esq),
//  como nos cursores classicos do Windows. O hotspot e (0,0).
// ============================================================================
static const char* s_cursor_bitmap[16] = {
    "X           ",
    "XX          ",
    "X.X         ",
    "X..X        ",
    "X...X       ",
    "X....X      ",
    "X.....X     ",
    "X......X    ",
    "X.......X   ",
    "X........X  ",
    "X.....XXXXX ",
    "X..X..X     ",
    "X.X X..X    ",
    "XX  X..X    ",
    "X    X..X   ",
    "      XX    "
};

static void draw_cursor_sprite(void) {
    int cx = (int)s_cursor_x;
    int cy = (int)s_cursor_y;
    if (cx < 0 || cy < 0) return;
    int SW = scr_width();
    int SH = scr_height();
    if (cx >= SW || cy >= SH) return;

    // Cores: preto p/ corpo, branco p/ borda. Funciona nos dois backends (a
    // funcao w32k_fill_rect com 1x1 px equivale a um pixel via paleta -> 32bpp).
    for (int row = 0; row < 16; row++) {
        const char* line = s_cursor_bitmap[row];
        for (int col = 0; col < 12; col++) {
            char c = line[col];
            if (c == ' ') continue;
            int px = cx + col;
            int py = cy + row;
            if (px < 0 || py < 0 || px >= SW || py >= SH) continue;
            uint8_t color = (c == 'X') ? FB_BLACK : FB_WHITE;
            // Pixel unico via w32k_fill_rect 1x1 (uniforme nos dois backends).
            w32k_fill_rect(px, py, 1, 1, color);
        }
    }
}

// ============================================================================
//  Cursor de HARDWARE (virtio-gpu cursor queue). Quando o backend o tem, o HOST
//  compoe o cursor sobre o scanout: mover o cursor NAO recompoe o desktop. O
//  sprite de software (draw_cursor_sprite) vira fallback (ex.: Bochs VBE).
//
//  A imagem 64x64 BGRA sai do MESMO s_cursor_bitmap 12x16: 'X' = preto opaco
//  (0xFF000000), '.' = branco opaco (0xFFFFFFFF), ' ' = transparente (alpha=0).
//  Hotspot (0,0) = ponta da seta no canto sup-esq (igual ao sprite SW).
// ============================================================================
// ============================================================================
//  Cursor de SOFTWARE com save-under.
//
//  Historico: usavamos o cursor de HARDWARE do virtio-gpu (cursor queue), mas ele
//  SO aparece quando o host renderiza o overlay — no QEMU 2D em Windows isso exige
//  `-display gtk,gl=on`. Sem isso, o cursor fica INVISIVEL. Solucao robusta: uma
//  setinha desenhada DIRETO no framebuffer (aparece em qualquer modo de display).
//
//  Para ficar FLUIDO sem recompor a tela a cada movimento, guardamos os pixels
//  sob o cursor (save-under): ao mover, restauramos o fundo antigo, salvamos o
//  novo, desenhamos a setinha e publicamos SO o retangulo afetado (gpu_present_rect).
// ============================================================================
static int s_hw_cursor = 0;         // mantido 0: cursor de HW desativado (ver acima)

#define CUR_W 12
#define CUR_H 16
static uint32_t s_cur_back[CUR_W * CUR_H];   // pixels salvos sob a setinha
static int      s_cur_bx = 0, s_cur_by = 0;  // onde o save-under foi capturado
static int      s_cur_back_valid = 0;

// Remove a setinha: repoe os pixels salvos sob ela.
static void cursor_hide(void) {
    if (!s_cur_back_valid) return;
    for (int r = 0; r < CUR_H; r++)
        for (int c = 0; c < CUR_W; c++)
            gpu_pixel(s_cur_bx + c, s_cur_by + r, s_cur_back[r * CUR_W + c]);
    s_cur_back_valid = 0;
}

// Desenha a setinha na posicao atual: salva o fundo e pinta o sprite por cima.
// Assume que NAO ha setinha desenhada agora (chame cursor_hide antes, ou apos um
// compose que ja redesenhou tudo).
static void cursor_show(void) {
    if (!s_cursor_initialized || !w32k_use_gpu()) return;
    int x = (int)s_cursor_x, y = (int)s_cursor_y;
    for (int r = 0; r < CUR_H; r++)
        for (int c = 0; c < CUR_W; c++)
            s_cur_back[r * CUR_W + c] = gpu_get_pixel(x + c, y + r);
    s_cur_bx = x; s_cur_by = y; s_cur_back_valid = 1;
    for (int r = 0; r < 16; r++) {
        const char* line = s_cursor_bitmap[r];
        for (int col = 0; col < 12; col++) {
            char ch = line[col];
            if (ch == ' ') continue;
            gpu_pixel(x + col, y + r, (ch == 'X') ? 0x00000000u    // contorno preto
                                                  : 0x00FFFFFFu);  // interior branco
        }
    }
}

// Move a setinha para (s_cursor_x, s_cursor_y) e publica SO a regiao afetada.
// Fluido: nao recompoe a tela. Chamado a cada evento de mouse (movimento).
// Serializado pelo guard s_composing: a thread de refresh e as apps tambem mexem
// no framebuffer/save-under; um compose/refresh em andamento faz este pedido ser
// descartado (a setinha reaparece no proximo compose/refresh usando s_cursor_x/y).
void win32k_cursor_repaint(void) {
    if (!w32k_use_gpu()) { if (s_was_active) win32k_compose(); return; }
    if (__atomic_test_and_set(&s_composing, __ATOMIC_ACQUIRE)) return;
    int ox = s_cur_bx, oy = s_cur_by, had = s_cur_back_valid;
    cursor_hide();
    cursor_show();
    int minx = s_cur_bx, miny = s_cur_by;
    int maxx = s_cur_bx + CUR_W, maxy = s_cur_by + CUR_H;
    if (had) {                                   // uniao do retangulo velho + novo
        if (ox < minx) minx = ox;
        if (oy < miny) miny = oy;
        if (ox + CUR_W > maxx) maxx = ox + CUR_W;
        if (oy + CUR_H > maxy) maxy = oy + CUR_H;
    }
    gpu_present_rect(minx, miny, maxx - minx, maxy - miny);
    __atomic_clear(&s_composing, __ATOMIC_RELEASE);
}

// Compat: os caminhos de input chamam isto na 1a vez. Com cursor de SOFTWARE nao
// ha nada a inicializar no host — so registramos a escolha uma vez.
static void win32k_hw_cursor_try_init(void) {
    static int logged = 0;
    if (logged) return;
    logged = 1;
    s_hw_cursor = 0;
    kputs("[win32k] cursor de SOFTWARE (save-under; visivel em qualquer display)\n");
}

// Compose TRUE-COLOR completo (LFB 32 bpp): definido em win32kshell.c. Desenha
// wallpaper com gradiente + janelas (sombra/caption/borda) + taskbar + menu.
void win32k_shell_compose_full(void);

// (s_composing declarado la em cima, junto do estado do cursor, pois o cursor de
// software — definido antes do compose — tambem precisa serializar por ele.)

// Apos um compose completo, a area cliente de cada janela foi repintada com a
// cor de fundo (o conteudo desenhado pela app foi coberto). Pedimos que cada
// janela visivel repinte seu conteudo (WM_PAINT, coalescido) — igual ao Windows,
// que envia WM_PAINT quando uma regiao e' invalidada. A app repinta no proximo
// giro do seu message loop e o present-on-idle publica o resultado.
static void win32k_invalidate_all(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        WND* w = &s_wins[i];
        if (w->used && w->visible && !(w->flags & WNDF_DESKTOP))
            queue_post((void*)(uintptr_t)w->id, WM_PAINT, 0, 0);
    }
}

void win32k_compose(void) {
    if (__atomic_test_and_set(&s_composing, __ATOMIC_ACQUIRE)) return;

    ensure_fb();
    // Backend GPU OU mode13h precisa estar ativo.
    if (!w32k_use_gpu() && !fb_active()) { __atomic_clear(&s_composing, __ATOMIC_RELEASE); return; }
    s_was_active = 1;                      // a GUI tomou a tela ao menos uma vez

    // -------- Caminho TRUE-COLOR (Windows 10/11) --------
    // No backend LFB 32 bpp a shell e' a dona do compose inteiro: desenha o
    // wallpaper com gradiente, cada janela com sombra + caption moderna, a
    // taskbar acrilica e o menu Iniciar. O caminho de paleta abaixo (mode13h)
    // fica so como fallback quando nao ha GPU.
    if (w32k_use_gpu()) {
        win32k_shell_compose_full();
        // A tela foi redesenhada: o save-under antigo esta invalido. Redesenha a
        // setinha de software DENTRO deste frame (capturando o fundo novo).
        s_cur_back_valid = 0;
        cursor_show();
        gpu_present();
        __atomic_clear(&s_composing, __ATOMIC_RELEASE);
        win32k_invalidate_all();   // apps repintam seu conteudo apos o compose
        return;
    }

    kputs("[win32k] compose: desktop + ");
    kput_dec((uint64_t)s_nwin); kputs(" janela(s) (z-order) + barra de tarefas");
    kputs(w32k_use_gpu() ? " [backend: gpu/LFB 32bpp]\n" : " [backend: vga mode13h 8bpp]\n");

    w32k_clear(DESKTOP_COLOR);             // fundo (caso nao haja papel de parede)

    // Desenha em ordem de z crescente (as de z maior por cima). Selection sort
    // simples por z (poucas janelas). O papel de parede (z=1) fica no fundo.
    for (int pass = 1; pass <= s_next_z; pass++) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            WND* w = &s_wins[i];
            if (w->used && w->visible && w->z == pass) {
                draw_window_chrome(w);
                kputs("[win32k]   janela #"); kput_dec(w->id);
                kputs(" '"); kputs(w->title); kputs("' rect=(");
                kput_dec((uint64_t)w->x); kputc(','); kput_dec((uint64_t)w->y);
                kputc(','); kput_dec((uint64_t)w->w); kputc(',');
                kput_dec((uint64_t)w->h); kputs(") z="); kput_dec((uint64_t)w->z);
                if (w->flags & WNDF_DESKTOP) kputs(" [wallpaper]");
                if (w->id == s_focus_id)     kputs(" [FOCO]");
                kputc('\n');
            }
        }
    }

    // Barra de tarefas SEMPRE por cima de tudo (se ha alguma janela "normal").
    if (count_taskbar_windows() > 0) draw_taskbar();

    // FASE 12 — SHELL Windows 10: repinta wallpaper Win10, chrome com
    // [_][[]][X], taskbar 40px, start button, clock, popup do start menu.
    // Vem APOS draw_taskbar (sobrescreve o cinza do BASE) e ANTES do cursor.
    win32k_shell_compose();

    // FASE 11 — Sprite do cursor por cima de tudo (apos taskbar+shell).
    // Desenhado apenas se ja houve algum evento de mouse (cursor inicializado),
    // para nao poluir as composicoes anteriores ao init do PS/2. Com cursor de
    // HARDWARE ativo, o HOST desenha o cursor — NAO desenhamos o sprite SW aqui.
    if (s_cursor_initialized && !s_hw_cursor) draw_cursor_sprite();

    // FASE 10.4 — Apos cada compose, "apresenta" o frame ao display. Em
    // virtio-gpu isso envia TRANSFER_TO_HOST_2D + RESOURCE_FLUSH para o host
    // re-ler nosso framebuffer e atualizar a tela. Em Bochs VBE e no-op (LFB
    // MMIO ja propaga as escritas). Sem este flush, em virtio-gpu o desktop
    // SO apareceria no proximo evento que chamasse explicitamente gpu_present.
    if (w32k_use_gpu()) gpu_present();
    __atomic_clear(&s_composing, __ATOMIC_RELEASE);
}

// ============================================================================
//  Servicos: classes e janelas
// ============================================================================
uintptr_t NtUserRegisterClass_k(const char* className, void* wndProc) {
    for (int i = 0; i < MAX_CLASSES; i++) {
        if (s_classes[i].used && str_eq(s_classes[i].name, className)) {
            s_classes[i].wndProc = wndProc;   // re-registro: atualiza
            return 1;
        }
    }
    for (int i = 0; i < MAX_CLASSES; i++) {
        if (!s_classes[i].used) {
            s_classes[i].used = 1;
            str_cpy(s_classes[i].name, className, 32);
            s_classes[i].wndProc = wndProc;
            kputs("[win32k] RegisterClass '"); kputs(s_classes[i].name);
            kputs("' wndproc="); kput_hex((uint64_t)(uintptr_t)wndProc); kputc('\n');
            return 1;
        }
    }
    kputs("[win32k] RegisterClass: tabela cheia\n");
    return 0;
}

uintptr_t NtUserCreateWindowEx_k(W32_CREATE* c) {
    if (!c) return 0;
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) if (!s_wins[i].used) { slot = i; break; }
    if (slot < 0) { kputs("[win32k] CreateWindowEx: limite de janelas\n"); return 0; }

    WND* w = &s_wins[slot];
    w->used = 1;
    w->id = s_next_id++;
    w->flags = c->flags;
    w->owner = (void*)PsGetCurrentProcess();   // EPROCESS dono (p/ reaping)
    // Cor da area cliente: WND_BG_DEFAULT -> cinza padrao (compat. Fase 2).
    w->bgColor = (c->bgColor == WND_BG_DEFAULT) ? CLIENT_COLOR : (uint8_t)c->bgColor;

    int SW = scr_width(), SH = scr_height();
    if (w->flags & WNDF_DESKTOP) {
        // Janela de fundo (papel de parede): cobre a tela inteira, sem chrome.
        // Usa a resolucao ATIVA (1024x768 com GPU, 320x200 no fallback mode13h).
        w->x = 0; w->y = 0; w->w = SW; w->h = SH;
    } else {
        w->x = c->x; w->y = c->y;
        w->w = (c->w > 0) ? c->w : 120;
        w->h = (c->h > 0) ? c->h : 80;
        // Clipa ao desktop (na resolucao do backend ativo) p/ ficar visivel.
        if (w->w > SW) w->w = SW;
        if (w->h > SH) w->h = SH;
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
        if (w->x + w->w > SW) w->x = SW - w->w;
        if (w->y + w->h > SH) w->y = SH - w->h;
    }
    str_cpy(w->title, c->windowName, 48);
    str_cpy(w->className, c->className, 32);
    w->wndProc = c->wndProc;
    w->visible = 0;                 // so visivel apos ShowWindow (como no Windows)
    w->z = s_next_z++;
    s_nwin++;
    // A janela de fundo NAO toma o foco (o teclado vai para as janelas de cima).
    if (!(w->flags & WNDF_DESKTOP)) s_focus_id = w->id;

    kputs("[win32k] CreateWindowEx -> HWND #"); kput_dec(w->id);
    kputs(" classe='"); kputs(w->className); kputs("' titulo='"); kputs(w->title);
    kputs("' x="); kput_dec((uint64_t)w->x); kputs(" y="); kput_dec((uint64_t)w->y);
    kputs(" w="); kput_dec((uint64_t)w->w); kputs(" h="); kput_dec((uint64_t)w->h);
    if (w->flags & WNDF_DESKTOP) kputs(" [DESKTOP/wallpaper]");
    if (w->flags & WNDF_CONSOLE) kputs(" [CONSOLE]");
    kputc('\n');

    void* hwnd = (void*)(uintptr_t)w->id;
    // WM_CREATE entra na fila (a app o processa no loop).
    queue_post(hwnd, WM_CREATE, 0, 0);
    return (uintptr_t)w->id;
}

uintptr_t NtUserShowWindow_k(void* hwnd, int cmdShow) {
    WND* w = wnd_from_handle(hwnd);
    if (!w) return 0;
    w->visible = (cmdShow != 0);
    kputs("[win32k] ShowWindow #"); kput_dec(w->id);
    kputs(w->visible ? " -> VISIVEL\n" : " -> OCULTA\n");
    if (w->visible) {
        win32k_compose();                       // desenha o chrome ja
        queue_post(hwnd, WM_PAINT, 0, 0);        // pede a pintura do cliente
        // Teste headless (FASE 2, guiapp): na 1a janela SIMPLES mostrada, injeta
        // entrada sintetica + WM_QUIT para o loop ser deterministico (sem
        // digitacao). Janelas de DESKTOP/CONSOLE (FASE 6) controlam a propria
        // entrada via NtUserPostKey/PostMessage — nao usam este auto-inject.
        static int s_injected = 0;
        if (!s_injected && !(w->flags & (WNDF_DESKTOP | WNDF_CONSOLE))) {
            s_injected = 1; win32k_inject_demo_input("Hi");
        }
    }
    return 1;
}

// Injeta entrada SINTETICA na janela com foco (mesmo caminho do teclado por
// IRQ1): WM_KEYDOWN+WM_CHAR para cada char de 's', e ao final um WM_QUIT.
// Serve para o teste headless ser deterministico (sem digitacao real) E para
// comprovar o roteamento de teclas; um teclado de verdade (IRQ1) tambem rota.
void win32k_inject_demo_input(const char* s) {
    if (s_focus_id == 0) return;
    void* hwnd = (void*)(uintptr_t)s_focus_id;
    kputs("[win32k] (demo) injetando teclas sinteticas p/ HWND #");
    kput_dec((uint64_t)s_focus_id); kputs(": \""); kputs(s); kputs("\"\n");
    while (*s) {
        char c = *s++;
        queue_post(hwnd, WM_KEYDOWN, (uint64_t)(uint8_t)c, 0);
        queue_post(hwnd, WM_CHAR,    (uint64_t)(uint8_t)c, 0);
    }
    queue_post(0, WM_QUIT, 0, 0);     // encerra o loop de mensagens do app
}

// FASE 6: da o foco a uma janela (clique simulado / Alt+Tab entre janelas).
// O teclado passa a ir para ela; recompoe para a barra de titulo refletir.
uintptr_t NtUserSetFocus_k(void* hwnd) {
    WND* w = wnd_from_handle(hwnd);
    if (!w || (w->flags & WNDF_DESKTOP)) return 0;   // o papel de parede nao recebe foco
    if (s_focus_id == w->id) return 1;
    s_focus_id = w->id;
    kputs("[win32k] SetFocus -> HWND #"); kput_dec(w->id);
    kputs(" ('"); kputs(w->title); kputs("')\n");
    win32k_compose();
    return 1;
}

// FASE 6: posta WM_KEYDOWN(+WM_CHAR) para UMA janela especifica, independente do
// foco. Usado pelo modo demo para enviar entrada deterministica a janelas
// distintas (varias janelas de cmd) em headless. Um teclado real (IRQ1) usa o
// caminho do foco (win32k_on_key).
uintptr_t NtUserPostKey_k(void* hwnd, char ascii, uint8_t scancode) {
    WND* w = wnd_from_handle(hwnd);
    if (!w) return 0;
    queue_post(hwnd, WM_KEYDOWN, (uint64_t)scancode, 0);
    if (ascii) queue_post(hwnd, WM_CHAR, (uint64_t)(uint8_t)ascii, 0);
    return 1;
}

uintptr_t NtUserDestroyWindow_k(void* hwnd) {
    WND* w = wnd_from_handle(hwnd);
    if (!w) return 0;
    kputs("[win32k] DestroyWindow #"); kput_dec(w->id); kputc('\n');
    queue_post(hwnd, WM_DESTROY, 0, 0);
    w->used = 0;
    s_nwin--;
    if (s_focus_id == w->id) {
        // foco vai para a janela "normal" de maior z restante (nunca o papel de
        // parede, que tem WNDF_DESKTOP).
        s_focus_id = 0; int best = -1;
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (s_wins[i].used && !(s_wins[i].flags & WNDF_DESKTOP) && s_wins[i].z > best) {
                best = s_wins[i].z; s_focus_id = s_wins[i].id;
            }
    }
    win32k_compose();
    return 1;
}

uintptr_t NtUserInvalidate_k(void* hwnd) {
    WND* w = wnd_from_handle(hwnd);
    if (!w) return 0;
    queue_post(hwnd, WM_PAINT, 0, 0);
    kputs("[win32k] InvalidateRect #"); kput_dec(w->id); kputs(" -> WM_PAINT enfileirado\n");
    return 1;
}

// ============================================================================
//  Fila de mensagens
// ============================================================================
uintptr_t NtUserPostMessage_k(void* hwnd, uint32_t msg, uint64_t wParam, uint64_t lParam) {
    queue_post(hwnd, msg, wParam, lParam);
    return 1;
}

uintptr_t NtUserPostQuitMessage_k(int exitCode) {
    queue_post(0, WM_QUIT, (uint64_t)(uint32_t)exitCode, 0);
    kputs("[win32k] PostQuitMessage("); kput_dec((uint64_t)(uint32_t)exitCode);
    kputs(") -> WM_QUIT enfileirado\n");
    return 1;
}

// Driver da tablet absoluta (virtio-input): poll dos eventos no idle (sem IRQ).
extern void virtio_input_poll(void);

// Tira a proxima mensagem. BLOQUEIA (hlt) ate haver algo: o teclado (IRQ1) e o
// timer (IRQ0) seguem rodando e o win32k_on_key/etc enchem a fila. Devolve:
//   1 = mensagem normal; 0 = WM_QUIT (a app encerra o loop); -1 = sem janelas.
int NtUserGetMessage_k(W32_MSG* out) {
    if (!out) return -1;
    for (;;) {
        if (queue_pop(out)) {
            if (out->message == WM_QUIT) {
                kputs("[win32k] GetMessage -> WM_QUIT (fim do loop)\n");
                return 0;
            }
            return 1;
        }
        // Fila vazia. Se nao ha mais janelas vivas, encerra para nao travar.
        if (s_nwin <= 0) {
            out->hwnd = 0; out->message = WM_QUIT; out->wParam = 0; out->lParam = 0;
            return 0;
        }
        // Fila vazia = a app terminou de processar (inclusive o WM_PAINT). No
        // backend virtio-gpu as escritas do WNDPROC (TextOut/FillRect) so vao
        // pra tela apos um present; fazemos UM present por volta ociosa do loop
        // (barato — nao por chamada GDI) para o conteudo da janela aparecer.
        if (w32k_use_gpu()) gpu_present();
        // Espera por uma interrupcao (teclado/timer) que possa postar algo.
        __asm__ volatile ("sti; hlt");
        // Apos acordar (timer @100Hz), drena a tablet absoluta (virtio-input):
        // ela nao tem IRQ wired, entao fazemos poll aqui. Os eventos viram
        // WM_MOUSE* na fila e o loop acima os entrega na proxima volta.
        virtio_input_poll();
    }
}

uintptr_t NtUserDispatchMessage_k(W32_MSG* msg) {
    // O despacho real (chamar o WNDPROC) acontece no user32, em ring 3. Aqui so
    // logamos para comprovar o roteamento.
    if (!msg) return 0;
    kputs("[win32k] DispatchMessage: hwnd=#");
    kput_dec((uint64_t)(uintptr_t)msg->hwnd);
    kputs(" msg="); kput_hex(msg->message); kputc('\n');
    return 0;
}

// ============================================================================
//  Entrada: teclado (IRQ1) -> janela com foco
// ============================================================================
void win32k_on_key(char ascii, uint8_t scancode) {
    if (s_focus_id == 0) return;                 // sem janela com foco
    void* hwnd = (void*)(uintptr_t)s_focus_id;
    queue_post(hwnd, WM_KEYDOWN, (uint64_t)scancode, 0);
    if (ascii) queue_post(hwnd, WM_CHAR, (uint64_t)(uint8_t)ascii, 0);
    kputs("[win32k] tecla roteada p/ HWND #"); kput_dec((uint64_t)s_focus_id);
    kputs(": WM_KEYDOWN(sc="); kput_hex(scancode); kputs(")");
    if (ascii) { kputs(" + WM_CHAR('"); kputc(ascii); kputs("')"); }
    kputc('\n');
}

// ============================================================================
//  FASE 11 — Entrada: mouse (IRQ12) -> janela debaixo do cursor.
//
//  Hit-test: percorre as janelas do topo do z-order pra baixo e devolve a
//  primeira que contem o ponto. Janelas WNDF_DESKTOP entram no hit-test
//  (papel de parede recebe WM_MOUSEMOVE quando o cursor passa por cima dele).
// ============================================================================
static WND* hit_test(int x, int y) {
    WND* best = 0;
    int best_z = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        WND* w = &s_wins[i];
        if (!w->used || !w->visible) continue;
        if (x < w->x || x >= w->x + w->w) continue;
        if (y < w->y || y >= w->y + w->h) continue;
        if (w->z > best_z) { best = w; best_z = w->z; }
    }
    return best;
}

// Empacota POINT em lParam (low=x, high=y, 16 bits cada) — convencao Win32.
static uint64_t pack_lparam_point(int x, int y) {
    uint32_t lo = (uint32_t)(uint16_t)(int16_t)x;
    uint32_t hi = (uint32_t)(uint16_t)(int16_t)y;
    return (uint64_t)((hi << 16) | lo);
}

// Roteamento compartilhado entre o mouse RELATIVO (PS/2) e o ABSOLUTO (virtio-
// tablet): assume s_cursor_x/y JA atualizados. A shell ve o evento primeiro;
// senao faz hit-test e posta WM_MOUSE*/botoes; recompoe so em mudanca de
// conteudo (clique). NAO mexe na posicao do cursor — cada chamador ja fez isso.
static void win32k_route_mouse(uint32_t buttons) {
    // FASE 12 — A SHELL Windows 10 ve o evento PRIMEIRO. Se ela consumir
    // (clique no Start button, no botao de uma janela na taskbar, em
    // [_][[]][X] da chrome, ou drag em andamento), o BASE NAO posta os
    // WM_MOUSE* — caso contrario o app receberia ambos os efeitos.
    {
        uint32_t prev = s_cursor_buttons;
        if (win32k_shell_on_mouse((int)s_cursor_x, (int)s_cursor_y, buttons, prev)) {
            s_cursor_buttons = buttons;       // atualiza antes do return
            return;                            // (sem log por-evento: floodaria a serial)
        }
    }

    // Hit-test: que janela esta debaixo do cursor agora?
    WND* w = hit_test((int)s_cursor_x, (int)s_cursor_y);
    void* hwnd = w ? (void*)(uintptr_t)w->id : 0;

    // WM_MOUSEMOVE: posta para a janela do hit (se houver). wParam = botoes
    // (estilo Win32: MK_LBUTTON=0x0001 MK_RBUTTON=0x0002 MK_MBUTTON=0x0010);
    // lParam = POINT empacotado em coords da JANELA (cliente).
    if (hwnd) {
        int cx = (int)s_cursor_x - w->x;
        int cy = (int)s_cursor_y - w->y;
        uint64_t mk = 0;
        if (buttons & 0x01) mk |= 0x0001;   // MK_LBUTTON
        if (buttons & 0x02) mk |= 0x0002;   // MK_RBUTTON
        if (buttons & 0x04) mk |= 0x0010;   // MK_MBUTTON
        queue_post(hwnd, WM_MOUSEMOVE, mk, pack_lparam_point(cx, cy));
    }

    // Mudanca de botoes -> postar DOWN/UP. Comparamos com o estado anterior.
    uint32_t changed = buttons ^ s_cursor_buttons;
    if (changed & 0x01) {                          // botao esquerdo mudou
        uint32_t msg = (buttons & 0x01) ? WM_LBUTTONDOWN : WM_LBUTTONUP;
        if (hwnd) {
            int cx = (int)s_cursor_x - w->x;
            int cy = (int)s_cursor_y - w->y;
            queue_post(hwnd, msg, 0, pack_lparam_point(cx, cy));
            // Clique no botao esquerdo tambem da o foco a janela (NT-style).
            if (msg == WM_LBUTTONDOWN && !(w->flags & WNDF_DESKTOP)) {
                s_focus_id = w->id;
            }
        }
    }
    if (changed & 0x02) {                          // botao direito
        uint32_t msg = (buttons & 0x02) ? WM_RBUTTONDOWN : WM_RBUTTONUP;
        if (hwnd) {
            int cx = (int)s_cursor_x - w->x;
            int cy = (int)s_cursor_y - w->y;
            queue_post(hwnd, msg, 0, pack_lparam_point(cx, cy));
        }
    }
    if (changed & 0x04) {                          // botao meio
        uint32_t msg = (buttons & 0x04) ? WM_MBUTTONDOWN : WM_MBUTTONUP;
        if (hwnd) {
            int cx = (int)s_cursor_x - w->x;
            int cy = (int)s_cursor_y - w->y;
            queue_post(hwnd, msg, 0, pack_lparam_point(cx, cy));
        }
    }
    s_cursor_buttons = buttons;

    // Recompoe o desktop SO quando o conteudo pode ter mudado (clique = mudanca de
    // foco/estado). Movimento puro do mouse NAO recompoe — a setinha de software e'
    // atualizada de forma barata (present parcial) pelo chamador via
    // win32k_cursor_repaint(). (Sem log por-evento: floodaria a serial.)
    int content_changed = (changed != 0);
    if (s_was_active && content_changed) {
        kputs("[win32k] clique roteado em (");
        kput_dec((uint64_t)s_cursor_x); kputc(','); kput_dec((uint64_t)s_cursor_y);
        if (hwnd) { kputs(") sobre HWND #"); kput_dec((uint64_t)w->id); } else kputc(')');
        kputc('\n');
        win32k_compose();
    }
}

// ============================================================================
//  FASE 14 — Entrada ABSOLUTA (virtio-tablet). Diferente do PS/2 (delta), aqui
//  recebemos a posicao ABSOLUTA ja em pixels da tela. Reposiciona o cursor de
//  HW via MOVE_CURSOR (gpu_cursor_move): no caminho de compose GL do QEMU
//  (-display gtk,gl=on) o cursor do virtio-gpu e' um OVERLAY posicionado pelo
//  guest — sem MOVE_CURSOR ele ficaria parado. Barato (cursor queue, sem
//  recompor). Depois atualiza a posicao interna (hit-test/cliques) e roteia WM_*.
// ============================================================================
void win32k_on_mouse_abs(int32_t x, int32_t y, uint32_t buttons) {
    if (!s_cursor_initialized) {
        s_cursor_initialized = 1;
        win32k_hw_cursor_try_init();   // garante o cursor de HW definido (1x)
    }
    int SW = scr_width(), SH = scr_height();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= SW) x = SW - 1;
    if (y >= SH) y = SH - 1;
    s_cursor_x = x;
    s_cursor_y = y;
    win32k_route_mouse(buttons);
    win32k_cursor_repaint();      // move a setinha de software (present parcial)
}

void win32k_on_mouse_event(int32_t dx, int32_t dy, uint32_t buttons) {
    // Marca o cursor como inicializado: agora o compose desenha o sprite.
    if (!s_cursor_initialized) {
        // Centro da tela na 1a chamada (a IRQ12 nao ve a tela ate o init).
        s_cursor_x = scr_width()  / 2;
        s_cursor_y = scr_height() / 2;
        s_cursor_initialized = 1;
        win32k_hw_cursor_try_init();   // tenta ligar o cursor de hardware (1x)
    }

    // Aplica delta com clamp aos limites da tela.
    int32_t nx = s_cursor_x + dx;
    int32_t ny = s_cursor_y + dy;
    int SW = scr_width(), SH = scr_height();
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx >= SW) nx = SW - 1;
    if (ny >= SH) ny = SH - 1;
    s_cursor_x = nx;
    s_cursor_y = ny;

    win32k_route_mouse(buttons);
    win32k_cursor_repaint();      // move a setinha de software (present parcial)
}

// ============================================================================
//  FASE 11 — Getters/Setters do cursor (usados pelas syscalls).
// ============================================================================
int32_t win32k_cursor_x(void) { return s_cursor_x; }
int32_t win32k_cursor_y(void) { return s_cursor_y; }

void win32k_set_cursor(int32_t x, int32_t y) {
    int SW = scr_width(), SH = scr_height();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= SW) x = SW - 1;
    if (y >= SH) y = SH - 1;
    s_cursor_x = x;
    s_cursor_y = y;
    s_cursor_initialized = 1;
    win32k_hw_cursor_try_init();
    kputs("[win32k] SetCursorPos -> (");
    kput_dec((uint64_t)s_cursor_x); kputc(',');
    kput_dec((uint64_t)s_cursor_y); kputs(")\n");
    win32k_cursor_repaint();      // move a setinha de software (present parcial)
}

// ============================================================================
//  GDI BASE: GetDC / GetStockObject / CreateSolidBrush.
//  O GDI complexo (TextOut com cor, FillRect com brush real, DIB) vai no FULL.
// ============================================================================
uintptr_t NtUserGetDC_k(void* hwnd) {
    WND* w = wnd_from_handle(hwnd);
    W32_DC* dc = (W32_DC*)ObCreateObject(OB_TYPE_EVENT, sizeof(W32_DC), 0);
    if (!dc) return 0;
    dc->windowId = w ? w->id : 0;
    HANDLE h = ob_create_handle(dc);
    kputs("[gdi] GetDC(HWND #"); kput_dec((uint64_t)(uintptr_t)hwnd);
    kputs(") -> HDC "); kput_hex((uint64_t)(uintptr_t)h); kputc('\n');
    return (uintptr_t)h;
}

uintptr_t NtGdiGetStockObject_k(int index) {
    if (index < 0 || index > 5) return 0;
    return (uintptr_t)s_stock_brush[index];   // HBRUSH = ponteiro p/ o objeto
}

uintptr_t NtGdiCreateSolidBrush_k(uint32_t color) {
    W32_BRUSH* b = (W32_BRUSH*)ObCreateObject(OB_TYPE_EVENT, sizeof(W32_BRUSH), 0);
    if (!b) return 0;
    b->color = color & 0xFF; b->isStock = 0;
    kputs("[gdi] CreateSolidBrush(cor="); kput_dec(color & 0xFF);
    kputs(") -> HBRUSH "); kput_hex((uint64_t)(uintptr_t)b); kputc('\n');
    return (uintptr_t)b;
}

// ============================================================================
//  FASE 6 — ciclo de vida das janelas por processo + estado da GUI
// ============================================================================
// Verdadeiro se a GUI ja tomou a tela alguma vez (a tela esta no estado grafico
// da GUI). O kmain usa isto para NAO rodar a fb_demo() da Fase 1 por cima do
// desktop, mesmo depois que as janelas foram reaped (tabela limpa).
int win32k_was_active(void) { return s_was_active; }

// Quando um processo (EPROCESS) encerra, libera TODAS as janelas dele (igual ao
// NT, que destroi as janelas de um processo ao terminar). Mantem o framebuffer
// como esta (nao repinta): assim a tela do ULTIMO app GUI permanece para o
// screendump, sem janelas "fantasma" de apps anteriores poluindo a tabela.
void win32k_reap_process_windows(void* eproc) {
    if (!eproc) return;
    int reaped = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (s_wins[i].used && s_wins[i].owner == eproc) {
            if (s_focus_id == s_wins[i].id) s_focus_id = 0;
            s_wins[i].used = 0;
            s_nwin--;
            reaped++;
        }
    }
    if (reaped) {
        kputs("[win32k] reaping: "); kput_dec((uint64_t)reaped);
        kputs(" janela(s) do processo que encerrou.\n");
    }
}
