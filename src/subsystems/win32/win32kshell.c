// ============================================================================
//  win32kshell.c  —  Subsistema Win32 (lado kernel) — SHELL DESKTOP (estilo
//                    Windows 10).
//
//  Esta camada VIVE em cima do compose do win32kbase: depois que o BASE pintou
//  o desktop, as janelas (com chrome simples) e a barra de tarefas pequena, o
//  SHELL repinta por cima com o visual moderno do Windows 10:
//
//    * Wallpaper azul Windows-10 (#0078D7) sobre toda a area de trabalho.
//    * Taskbar 40px no rodape em cinza escuro (#202020).
//    * Botao "Iniciar" no canto esquerdo (logo de 4 quadradinhos azuis).
//    * Bandeja de aplicativos (1 botao por janela top-level visivel).
//    * Relogio HH:MM no canto direito.
//    * Start menu popup (400x500) quando aberto, com lista de apps.
//    * Window chrome estilo Windows 10: titulo + botoes [X][_][[]] no canto
//      superior direito de cada janela "normal".
//    * Drag de janelas: clique-segure na barra de titulo move a janela.
//    * Botoes da chrome:
//        - X (close)   -> posta WM_CLOSE para a janela.
//        - _ (min)     -> marca minimizada (skip no compose ate restaurar).
//        - [] (max)    -> alterna entre tamanho original e maximizado
//                          (cobre a tela menos a taskbar).
//
//  Inclusao: este arquivo NAO e compilado isolado. Igual ao win32kbase.c e
//  win32kfull.c, ele e #include-iado por win32k.c apos os dois primeiros, e
//  acaba dentro do MESMO translation unit. Assim acessa os tipos / s_wins /
//  helpers de desenho (w32k_*) e estado (s_focus_id, s_cursor_*) ja definidos.
// ============================================================================

// ---- Cores estilo Windows 10 (paleta default; serao mapeadas via FB_* p/
//      mode13h, e via palette_8_to_32 quando o backend e LFB 32 bpp).
//
// O backend mode13h so tem 16 cores nominais, entao usamos os indices mais
// proximos:
//   wallpaper azul Win10 -> FB_BLUE (mesmo azul DOS, suficiente em fallback)
//   taskbar     #202020  -> FB_DARK_GRAY (cinza escuro proximo)
//   start menu  #2D2D30  -> FB_DARK_GRAY tambem
//   accent      blue     -> FB_LIGHT_BLUE
//   close hover red      -> FB_RED
//
// Em LFB 32 bpp o w32k_fill_rect ja converte o indice para RGB32 — entao o
// resultado fica nominalmente correto nos dois backends. Em alta resolucao a
// paleta da palette_8_to_32 produz exatamente o azul/cinza esperados.
// ============================================================================
#define SH_WALLPAPER       FB_BLUE          // azul desktop (em LFB vira #0000A8)
#define SH_TASKBAR_BG      FB_DARK_GRAY     // taskbar escura
#define SH_TASKBAR_FG      FB_WHITE
#define SH_START_TILE      FB_LIGHT_BLUE    // ladrilhos do botao Iniciar
#define SH_START_HOVER     FB_CYAN
#define SH_CLOCK_FG        FB_WHITE
#define SH_MENU_BG         FB_DARK_GRAY
#define SH_MENU_FG         FB_WHITE
#define SH_MENU_TILE       FB_BLUE          // tile do start menu (acento)
#define SH_CHROME_TITLE    FB_BLUE          // barra de titulo (janela com foco)
#define SH_CHROME_INACT    FB_LIGHT_GRAY    // barra inativa
#define SH_CHROME_TEXT     FB_WHITE
#define SH_CHROME_BORDER   FB_BLACK
#define SH_BTN_CLOSE_HOVER FB_RED
#define SH_BTN_BG          FB_LIGHT_GRAY    // botao do app na taskbar
#define SH_BTN_ACTIVE_BG   FB_WHITE         // botao da janela com foco

// Dimensoes da shell Windows 10:
//   - taskbar 40px (no LFB 1024x768). Em mode13h (320x200) o draw_taskbar do
//     BASE ja gerou uma barra de 14px; aqui repintamos por cima a TASKBAR_H10.
//   - start button 40x40 (cabe na taskbar).
//   - botao de app na taskbar: 96x32 (cabem ~9 na largura).
//   - start menu popup: 400x500 (acima da taskbar).
#define SH_TASKBAR_H10     48
#define SH_START_BTN_W     48
#define SH_APP_BTN_W       160
#define SH_APP_BTN_H       40
#define SH_CLOCK_W         92
#define SH_MENU_W          320
#define SH_MENU_H          460

// Chrome de janela estilo Windows 10:
//   - title bar height 18 (em LFB; cabe a fonte 8x8 + padding).
//   - cada botao [_ [] X] = 18x18 no canto superior direito.
//
// NOTA: TITLE_H do BASE permanece 12 (compat com clipping de TextOut). O shell
// desenha a barra "espessa" de 18px POR CIMA — a area cliente comeca depois da
// barra original (TITLE_H=12), entao texto nao colide com nossa barra extra.
// (Visualmente: barra do BASE 12px + extensao 6px = 18px total da chrome.)
#define SH_TITLE_H         28
#define SH_BTN_W           46
#define SH_BTN_H           28

// ============================================================================
//  Estado da shell (escopo do TU win32k).
// ============================================================================
static int      s_shell_initialized;        // primeira compose ja configurou
static int      s_start_menu_open;          // popup do Iniciar aberto?
static uint32_t s_drag_hwnd;                // id da janela sendo arrastada (0=nenhuma)
static int      s_drag_offset_x;            // offset cursor->topo-esq da janela
static int      s_drag_offset_y;
// Estado por-janela (paralelo a s_wins[]): minimizada / maximizada + retangulo
// original para restaurar. Usamos arrays paralelos para nao mexer no struct WND
// do BASE (que ja esta estavel).
static int      s_minimized[MAX_WINDOWS];
static int      s_maximized[MAX_WINDOWS];
static int      s_saved_x[MAX_WINDOWS];
static int      s_saved_y[MAX_WINDOWS];
static int      s_saved_w[MAX_WINDOWS];
static int      s_saved_h[MAX_WINDOWS];
// Indice (em s_wins) cujos botoes [X][_][[]] ja foram clicados — usado para
// detectar o EDGE do clique no UP (mouse up apos down dentro do botao).
static int      s_pending_close_idx = -1;
static int      s_pending_min_idx   = -1;
static int      s_pending_max_idx   = -1;

// ----------------------------------------------------------------------------
//  Helpers: descobre em qual slot esta um id (s_wins eh array indexado por
//  slot, nao por id). Retorna -1 se nao encontrar.
// ----------------------------------------------------------------------------
static int shell_slot_of(uint32_t id) {
    if (!id) return -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (s_wins[i].used && s_wins[i].id == id) return i;
    return -1;
}

// ----------------------------------------------------------------------------
//  Helper: a barra de titulo do BASE (TITLE_H=12) NAO cobre toda a SH_TITLE_H
//  do Windows 10. Adicionamos uma "extensao" inferior que estende a barra para
//  baixo cobrindo 6px da area cliente. Como o conteudo via TextOut faz seu
//  clipping pelo TITLE_H+1, isso nao corta texto da app — so o cinza inicial.
// ----------------------------------------------------------------------------
static void shell_draw_chrome_extension(WND* w, int active) {
    int extra = SH_TITLE_H - TITLE_H;
    if (extra <= 0) return;
    uint8_t color = active ? SH_CHROME_TITLE : SH_CHROME_INACT;
    w32k_fill_rect(w->x + 1, w->y + TITLE_H, w->w - 2, extra, color);
}

// ----------------------------------------------------------------------------
//  Desenha os 3 botoes [_] [[]] [X] no canto superior direito da janela.
//  Ordem (esq -> dir): minimize, maximize, close. Cada um SH_BTN_W x SH_BTN_H.
// ----------------------------------------------------------------------------
static void shell_draw_window_buttons(WND* w, int slot) {
    if (w->flags & WNDF_DESKTOP) return;        // wallpaper sem botoes
    int btn_y = w->y;
    int x_close = w->x + w->w - SH_BTN_W - 1;
    int x_max   = x_close - SH_BTN_W;
    int x_min   = x_max   - SH_BTN_W;
    int hovered_close = (s_pending_close_idx == slot);
    int hovered_max   = (s_pending_max_idx   == slot);
    int hovered_min   = (s_pending_min_idx   == slot);

    // Botao X (close) — vermelho quando "armado" (LBUTTONDOWN sobre ele).
    uint8_t cbg = hovered_close ? SH_BTN_CLOSE_HOVER : SH_CHROME_TITLE;
    w32k_fill_rect(x_close, btn_y, SH_BTN_W, SH_BTN_H, cbg);
    // Desenha um 'X' simples: duas diagonais 8x8 no centro do botao.
    int cx = x_close + 5, cy = btn_y + 5;
    for (int k = 0; k < 8; k++) {
        w32k_fill_rect(cx + k, cy + k, 1, 1, FB_WHITE);
        w32k_fill_rect(cx + 7 - k, cy + k, 1, 1, FB_WHITE);
    }

    // Botao [] (maximize) — quadrado de contorno.
    uint8_t mbg = hovered_max ? FB_LIGHT_GRAY : SH_CHROME_TITLE;
    w32k_fill_rect(x_max, btn_y, SH_BTN_W, SH_BTN_H, mbg);
    w32k_rect(x_max + 4, btn_y + 4, 10, 10, FB_WHITE);

    // Botao _ (minimize) — barra horizontal no rodape do botao.
    uint8_t nbg = hovered_min ? FB_LIGHT_GRAY : SH_CHROME_TITLE;
    w32k_fill_rect(x_min, btn_y, SH_BTN_W, SH_BTN_H, nbg);
    w32k_fill_rect(x_min + 4, btn_y + 13, 10, 2, FB_WHITE);

    // Linhas pretas separando os botoes (estetica).
    w32k_vline(x_min, btn_y, SH_BTN_H, FB_BLACK);
    w32k_vline(x_max, btn_y, SH_BTN_H, FB_BLACK);
    w32k_vline(x_close, btn_y, SH_BTN_H, FB_BLACK);
}

// ----------------------------------------------------------------------------
//  Wallpaper: pinta a area de trabalho INTEIRA (menos a taskbar) com o azul
//  Windows 10. Vai por baixo das janelas — chamamos antes do replay das
//  janelas.
//  OBS: O BASE ja chamou w32k_clear(DESKTOP_COLOR) e draw_window_chrome de
//  cada janela. Nosso wallpaper aqui ja se sobrepoe ao azul do BASE (FB_BLUE
//  vs FB_BLUE — mesma cor; nao vai apagar nada).
// ----------------------------------------------------------------------------
static void shell_draw_wallpaper(void) {
    int W = scr_width();
    int H = scr_height() - SH_TASKBAR_H10;
    // O BASE ja limpou com DESKTOP_COLOR (=FB_BLUE). Nao precisamos repintar.
    // Mas se em algum momento mudarmos DESKTOP_COLOR, fazemos aqui.
    (void)W; (void)H;
}

// ----------------------------------------------------------------------------
//  Logo do Start (4 quadradinhos azuis 2x2 num grid 2x2, igual ao do Win10).
//  Desenhado dentro do botao "Iniciar" (40x40 no canto inferior esquerdo).
// ----------------------------------------------------------------------------
static void shell_draw_start_logo(int bx, int by) {
    // Cada "azulejo" do logo: 7x7 pixels; gap de 2px entre os 4.
    int gap = 2;
    int tile = 7;
    int ox = bx + (SH_START_BTN_W - (2 * tile + gap)) / 2;
    int oy = by + (SH_TASKBAR_H10 - (2 * tile + gap)) / 2;
    uint8_t c = SH_START_TILE;
    w32k_fill_rect(ox,             oy,             tile, tile, c);
    w32k_fill_rect(ox + tile + gap,oy,             tile, tile, c);
    w32k_fill_rect(ox,             oy + tile + gap,tile, tile, c);
    w32k_fill_rect(ox + tile + gap,oy + tile + gap,tile, tile, c);
}

// ----------------------------------------------------------------------------
//  Taskbar Windows 10: pinta a barra inteira de cinza escuro + Start button +
//  botoes de apps + relogio.
//
//  Hover/active states: nesta primeira versao "headless", os botoes nao tem
//  hover real (depende do mouse parado em cima). Apenas mostramos o estado
//  "ativo" (janela com foco) destacando o botao do app correspondente.
// ----------------------------------------------------------------------------
static void shell_draw_taskbar(void) {
    int W  = scr_width();
    int ty = scr_height() - SH_TASKBAR_H10;
    // Tampa a taskbar pequena do BASE com a barra grande do Windows 10.
    w32k_fill_rect(0, ty, W, SH_TASKBAR_H10, SH_TASKBAR_BG);
    w32k_hline(0, ty, W, FB_LIGHT_GRAY);            // borda superior 1px (separador)

    // Botao Iniciar (40x40 no canto inferior esquerdo).
    w32k_fill_rect(0, ty, SH_START_BTN_W, SH_TASKBAR_H10,
                   s_start_menu_open ? FB_DARK_GRAY : SH_TASKBAR_BG);
    shell_draw_start_logo(0, ty);

    // Bandeja de apps: para cada janela top-level visivel, um botao de app.
    int bx = SH_START_BTN_W + 4;
    int max_x = W - SH_CLOCK_W - 4;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        WND* w = &s_wins[i];
        if (!w->used || !w->visible || (w->flags & WNDF_DESKTOP)) continue;
        if (bx + SH_APP_BTN_W > max_x) break;

        int active = (w->id == s_focus_id);
        uint8_t bg = active ? SH_BTN_ACTIVE_BG : SH_TASKBAR_BG;
        w32k_fill_rect(bx, ty + 4, SH_APP_BTN_W, SH_APP_BTN_H, bg);
        // Linha do indicador (debaixo do botao, igual Win10).
        w32k_fill_rect(bx, ty + SH_TASKBAR_H10 - 2,
                       SH_APP_BTN_W, 2, active ? FB_LIGHT_BLUE : FB_DARK_GRAY);
        // Titulo abreviado (~10 chars na fonte 8x8 em 96px).
        char lbl[12]; int k = 0;
        for (; k < 11 && w->title[k]; k++) lbl[k] = w->title[k];
        lbl[k] = 0;
        w32k_draw_text(bx + 6, ty + (SH_TASKBAR_H10 - 8) / 2 - 2, lbl,
                       active ? FB_BLACK : SH_TASKBAR_FG, 0xFF);
        // Indicador "minimizada" — botao apagado.
        if (s_minimized[i]) {
            w32k_fill_rect(bx + SH_APP_BTN_W - 6, ty + 6, 4, 4, FB_LIGHT_GRAY);
        }
        bx += SH_APP_BTN_W + 2;
    }

    // Relogio HH:MM no canto direito. g_ticks @ 100 Hz -> segundos = g_ticks/100.
    // "Hora" desde o boot, mod 24:00 (suficiente p/ feedback visual em headless).
    uint64_t total_secs = (uint64_t)g_ticks / 100u;
    uint32_t minutes = (uint32_t)((total_secs / 60u) % 60u);
    uint32_t hours   = (uint32_t)((total_secs / 3600u) % 24u);
    char clk[8];
    clk[0] = '0' + ((hours / 10) % 10);
    clk[1] = '0' + (hours % 10);
    clk[2] = ':';
    clk[3] = '0' + ((minutes / 10) % 10);
    clk[4] = '0' + (minutes % 10);
    clk[5] = 0;
    int clock_x = W - SH_CLOCK_W + 8;
    int clock_y = ty + (SH_TASKBAR_H10 - 8) / 2;
    w32k_draw_text(clock_x, clock_y, clk, SH_CLOCK_FG, 0xFF);
}

// ----------------------------------------------------------------------------
//  Popup do Start menu. Exibido quando s_start_menu_open=1; cobre 400x500 acima
//  da taskbar, com cor escura e uma lista simbolica de apps.
//  Sem callbacks reais — so visual.
// ----------------------------------------------------------------------------
static void shell_draw_start_menu(void) {
    if (!s_start_menu_open) return;
    int mx = 0;
    int my = scr_height() - SH_TASKBAR_H10 - SH_MENU_H;
    if (my < 0) { my = 0; }   // se a tela for muito pequena, encosta no topo

    // Em mode13h (320x200) o menu nao cabe — limita para nao estourar a tela.
    int mw = SH_MENU_W;
    int mh = SH_MENU_H;
    if (mx + mw > scr_width())  mw = scr_width() - mx;
    if (my + mh > scr_height() - SH_TASKBAR_H10) mh = scr_height() - SH_TASKBAR_H10 - my;
    if (mw <= 0 || mh <= 0) return;

    w32k_fill_rect(mx, my, mw, mh, SH_MENU_BG);
    w32k_rect(mx, my, mw, mh, FB_LIGHT_GRAY);

    // Cabecalho (barra fina com o usuario).
    w32k_fill_rect(mx, my, mw, 24, FB_BLUE);
    w32k_draw_text(mx + 8, my + 8, "Usuario", FB_WHITE, 0xFF);

    // Lista de tiles/apps (uma linha cada).
    static const char* items[] = {
        "Calculator", "Notepad", "Run", "Settings", "Shut Down"
    };
    int y = my + 36;
    for (int i = 0; i < 5; i++) {
        // tile do icone (apenas um quadrado azul 24x24 -> simula icone).
        w32k_fill_rect(mx + 12, y, 24, 24, SH_MENU_TILE);
        w32k_draw_text(mx + 44, y + 8, items[i], SH_MENU_FG, 0xFF);
        y += 32;
        if (y + 32 > my + mh) break;
    }
}

// ============================================================================
//  ====================  COMPOSE TRUE-COLOR (Windows 10/11)  ==================
//
//  Caminho usado quando o backend e o LFB 32 bpp (gpu_active). Desenha o
//  desktop INTEIRO com cores XRGB reais: wallpaper com gradiente, janelas com
//  drop-shadow + caption em gradiente de acento + botoes [_ [] X], taskbar
//  acrilica com botao Iniciar (logo de 4 ladrilhos), botoes de app e relogio.
//
//  As funcoes _tc (true-color) sao paralelas as de paleta acima (usadas so no
//  fallback mode13h). win32k_shell_compose_full e' chamada por win32k_compose.
// ============================================================================

// SMP: status dos cores para a system tray (prova visual de multinucleo).
extern int      smp_cpu_count(void);
extern int      smp_ap_working(void);
extern uint64_t smp_ap_heartbeat(void);

// uint64 -> string decimal (helper local; kput_dec e' so serial).
static int shell_u64(char* buf, uint64_t v) {
    char tmp[24]; int n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return 1; }
    while (v) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0; return n;
}
static void shell_strcat(char* dst, int* p, const char* s) {
    while (*s) dst[(*p)++] = *s++;
    dst[*p] = 0;
}

// Largura reservada na direita da taskbar para a system tray de CPU.
#define SH_TRAY_W  150

// Logo do Iniciar: 4 ladrilhos azuis (bandeira do Windows) centrados no botao.
static void shell_logo4_tc(int bx, int by, int box_w) {
    int tile = 9, gap = 3;
    int ox = bx + (box_w - (2 * tile + gap)) / 2;
    int oy = by + (SH_TASKBAR_H10 - (2 * tile + gap)) / 2;
    uint32_t c = UI_START_TILE;
    w32k_rgb_rect(ox,             oy,             tile, tile, c);
    w32k_rgb_rect(ox + tile + gap, oy,            tile, tile, c);
    w32k_rgb_rect(ox,             oy + tile + gap, tile, tile, c);
    w32k_rgb_rect(ox + tile + gap, oy + tile + gap, tile, tile, c);
}

// Botoes da caption [_] [[]] [X] no canto superior direito. Geometria IDENTICA
// a do hit-test em win32k_shell_on_mouse (x_close = x+w-SH_BTN_W-1).
static void shell_caption_buttons_tc(WND* w, int slot) {
    int bh = TITLE_H;
    int by = w->y;
    int x_close = w->x + w->w - SH_BTN_W - 1;
    int x_max   = x_close - SH_BTN_W;
    int x_min   = x_max   - SH_BTN_W;
    if (x_min < w->x) return;                 // janela estreita demais p/ botoes

    int hov_close = (s_pending_close_idx == slot);
    int hov_max   = (s_pending_max_idx   == slot);
    int hov_min   = (s_pending_min_idx   == slot);

    // Minimize: barra horizontal.
    if (hov_min) w32k_rgb_rect(x_min, by, SH_BTN_W, bh, UI_BTN_HOVER);
    w32k_rgb_rect(x_min + SH_BTN_W / 2 - 6, by + bh / 2 + 3, 12, 2, UI_TEXT_LT);

    // Maximize: quadrado de contorno (2px de espessura visual).
    if (hov_max) w32k_rgb_rect(x_max, by, SH_BTN_W, bh, UI_BTN_HOVER);
    w32k_rgb_frame(x_max + SH_BTN_W / 2 - 6, by + bh / 2 - 6, 12, 12, UI_TEXT_LT);
    w32k_rgb_frame(x_max + SH_BTN_W / 2 - 5, by + bh / 2 - 5, 10, 10, UI_TEXT_LT);

    // Close: X (fundo vermelho no hover). Duas diagonais de 2px.
    if (hov_close) w32k_rgb_rect(x_close, by, SH_BTN_W, bh, UI_CLOSE_HOVER);
    int cx = x_close + SH_BTN_W / 2 - 4, cy = by + bh / 2 - 4;
    for (int k = 0; k < 9; k++) {
        gpu_pixel(cx + k,     cy + k, UI_TEXT_LT);
        gpu_pixel(cx + k + 1, cy + k, UI_TEXT_LT);
        gpu_pixel(cx + 8 - k, cy + k, UI_TEXT_LT);
        gpu_pixel(cx + 8 - k + 1, cy + k, UI_TEXT_LT);
    }
}

// Uma janela "normal" completa: sombra + cliente + caption + titulo + botoes + borda.
static void shell_window_tc(WND* w, int slot) {
    int active = (w->id == s_focus_id);
    int x = w->x, y = w->y, ww = w->w, hh = w->h;

    w32k_rgb_shadow(x, y, ww, hh);                       // drop shadow

    // Corpo (area cliente) abaixo da caption — branco Win10.
    if (hh > TITLE_H)
        w32k_rgb_rect(x, y + TITLE_H, ww, hh - TITLE_H, UI_CLIENT);

    // Caption com gradiente vertical (acento se focada, cinza se nao).
    if (active) gpu_gradient_v(x, y, ww, TITLE_H, UI_ACCENT_LT, UI_ACCENT_DK);
    else        gpu_gradient_v(x, y, ww, TITLE_H, UI_TITLE_INACT1, UI_TITLE_INACT2);

    // Titulo em 2x (16px) com leve sombra, centrado na altura da caption.
    int ty = y + (TITLE_H - 16) / 2;
    w32k_rgb_text_sh(x + 12, ty, w->title,
                     active ? UI_TEXT_LT : UI_TEXT_DIM, 0x00102038, 2);

    shell_caption_buttons_tc(w, slot);

    // Borda externa 1px (acento quando focada).
    w32k_rgb_frame(x, y, ww, hh, active ? UI_ACCENT_DK : 0x00303337);
}

// Taskbar acrilica: fundo escuro translucido + Iniciar + botoes de app + relogio.
static void shell_taskbar_tc(void) {
    int W  = scr_width();
    int ty = scr_height() - SH_TASKBAR_H10;

    gpu_blend_rect(0, ty, W, SH_TASKBAR_H10, UI_TASKBAR, 236);   // "acrilico"
    w32k_rgb_rect(0, ty, W, 1, UI_TASKBAR_HI);                    // realce superior

    // Botao Iniciar (48px). Fundo levemente mais claro quando o menu esta aberto.
    if (s_start_menu_open) w32k_rgb_rect(0, ty, SH_START_BTN_W, SH_TASKBAR_H10, 0x002E3136);
    shell_logo4_tc(0, ty, SH_START_BTN_W);

    // Bandeja de apps: um botao por janela top-level visivel.
    int bx = SH_START_BTN_W + 8;
    int max_x = W - SH_CLOCK_W - SH_TRAY_W - 8;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        WND* w = &s_wins[i];
        if (!w->used || !w->visible || (w->flags & WNDF_DESKTOP)) continue;
        if (bx + SH_APP_BTN_W > max_x) break;
        int active = (w->id == s_focus_id) && !s_minimized[i];

        w32k_rgb_rect(bx, ty + 3, SH_APP_BTN_W, SH_TASKBAR_H10 - 3,
                      active ? 0x002D2F34 : UI_TASKBAR);
        // Realce (underline) de acento embaixo do botao ativo.
        if (active)
            w32k_rgb_rect(bx, ty + SH_TASKBAR_H10 - 3, SH_APP_BTN_W, 3, UI_ACCENT);
        else if (!s_minimized[i])
            w32k_rgb_rect(bx + 6, ty + SH_TASKBAR_H10 - 3, SH_APP_BTN_W - 12, 2, 0x00555A61);

        // Icone (quadradinho de acento) + titulo abreviado.
        w32k_rgb_rect(bx + 10, ty + SH_TASKBAR_H10 / 2 - 6, 12, 12, UI_ACCENT_LT);
        char lbl[18]; int k = 0;
        for (; k < 17 && w->title[k]; k++) lbl[k] = w->title[k];
        lbl[k] = 0;
        w32k_rgb_text(bx + 30, ty + SH_TASKBAR_H10 / 2 - 4, lbl, UI_TEXT_LT, 1);
        bx += SH_APP_BTN_W + 4;
    }

    // System tray — status dos NUCLEOS (prova visual de SMP no desktop).
    // Dois "chips": CPU0 (BSP) sempre aceso; CPU1 (AP) aceso se o 2o core esta no
    // worker loop. Abaixo, o heartbeat do AP (em milhoes) — so avanca se o AP esta
    // executando instrucoes AGORA, em paralelo ao BSP que desenha esta taskbar.
    {
        int tx = W - SH_CLOCK_W - SH_TRAY_W;
        int ap_on = smp_ap_working();
        w32k_rgb_rect(tx,      ty + 9, 9, 9, UI_ACCENT_LT);                         // CPU0
        w32k_rgb_rect(tx + 12, ty + 9, 9, 9, ap_on ? UI_START_TILE : 0x00505357);  // CPU1
        char top[32]; int p = 0;
        shell_u64(top, (uint64_t)smp_cpu_count());
        p = (int)0; while (top[p]) p++;
        shell_strcat(top, &p, " nucleos");
        w32k_rgb_text(tx + 28, ty + 8, top, UI_TEXT_LT, 1);
        char bot[40]; p = 0;
        shell_strcat(bot, &p, "AP ");
        shell_u64(bot + p, smp_ap_heartbeat() / 1000000ULL);
        while (bot[p]) p++;
        shell_strcat(bot, &p, "M");
        w32k_rgb_text(tx + 28, ty + SH_TASKBAR_H10 - 16, bot,
                      ap_on ? UI_TEXT_DIM : 0x00707070, 1);
    }

    // Relogio HH:MM:SS a direita. g_ticks @100 Hz -> segundos. Com o timer do
    // scheduler ativo, ele avanca em tempo real (prova visual do multitarefa).
    uint64_t secs = (uint64_t)g_ticks / 100u;
    uint32_t ss = (uint32_t)(secs % 60u);
    uint32_t mm = (uint32_t)((secs / 60u) % 60u);
    uint32_t hh = (uint32_t)((secs / 3600u) % 24u);
    char clk[9];
    clk[0] = (char)('0' + (hh / 10) % 10); clk[1] = (char)('0' + hh % 10);
    clk[2] = ':';
    clk[3] = (char)('0' + (mm / 10) % 10); clk[4] = (char)('0' + mm % 10);
    clk[5] = ':';
    clk[6] = (char)('0' + (ss / 10) % 10); clk[7] = (char)('0' + ss % 10);
    clk[8] = 0;
    int cw = w32k_text_w(clk, 1);
    int cxr = W - 14 - cw;
    w32k_rgb_text(cxr, ty + 10, clk, UI_TEXT_LT, 1);
    const char* label = "MeuOS 11";
    w32k_rgb_text(W - 14 - w32k_text_w(label, 1), ty + SH_TASKBAR_H10 - 16,
                  label, UI_TEXT_DIM, 1);
}

// Menu Iniciar (painel escuro acima do botao) com lista de apps.
static void shell_start_menu_tc(void) {
    if (!s_start_menu_open) return;
    int mh = SH_MENU_H;
    int mw = SH_MENU_W;
    int mx = 0;
    int my = scr_height() - SH_TASKBAR_H10 - mh;
    if (my < 0) { my = 0; mh = scr_height() - SH_TASKBAR_H10; }

    gpu_blend_rect(mx, my, mw, mh, 0x00202124, 244);
    w32k_rgb_frame(mx, my, mw, mh, 0x00343841);

    // Cabecalho com "usuario".
    gpu_gradient_v(mx, my, mw, 56, UI_ACCENT_LT, UI_ACCENT_DK);
    w32k_rgb_rect(mx + 16, my + 16, 24, 24, 0x00FFFFFF);        // avatar
    w32k_rgb_text(mx + 52, my + 24, "MeuOS User", UI_TEXT_LT, 1);

    static const char* items[] = {
        "Documentos", "Imagens", "Configuracoes",
        "Explorador", "Prompt de Comando", "Energia"
    };
    int y = my + 72;
    for (int i = 0; i < 6; i++) {
        w32k_rgb_rect(mx + 16, y, 22, 22, UI_ACCENT);          // "icone"
        w32k_rgb_text(mx + 50, y + 7, items[i], UI_TEXT_LT, 1);
        y += 40;
        if (y + 30 > my + mh) break;
    }
}

// Compose completo (chamado por win32k_compose no backend LFB 32 bpp).
void win32k_shell_compose_full(void) {
    if (!s_shell_initialized) {
        s_shell_initialized = 1;
        kputs("[shell] compose TRUE-COLOR (Windows 10/11) ativo.\n");
    }
    int W = scr_width();
    int H = scr_height();
    int half = H / 2;

    // 1) Wallpaper: gradiente vertical em duas metades (brilho no meio).
    gpu_gradient_v(0, 0,    W, half,     UI_WALL_TOP, UI_WALL_MID);
    gpu_gradient_v(0, half, W, H - half, UI_WALL_MID, UI_WALL_BOT);

    // 2) Janelas normais, em ordem de z crescente (topo por cima).
    for (int pass = 1; pass <= s_next_z; pass++) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            WND* w = &s_wins[i];
            if (!w->used || !w->visible || w->z != pass) continue;
            if (w->flags & WNDF_DESKTOP) continue;   // wallpaper ja desenhado
            if (s_minimized[i]) continue;            // minimizada -> so na taskbar
            shell_window_tc(w, i);
        }
    }

    // 3) Taskbar + menu Iniciar por cima de tudo.
    shell_taskbar_tc();
    shell_start_menu_tc();

    kputs("[shell] compose(true-color): wallpaper+janelas+taskbar");
    if (s_start_menu_open) kputs("+menu");
    kputc('\n');
}

// Refresh LEVE: repinta SO a faixa da taskbar (relogio ao vivo) sem tocar nas
// janelas — assim o conteudo das apps NAO e' apagado a cada tique. Chamado pela
// thread kdesktop_refresh ~2x/s. Serializado pelo mesmo guard s_composing.
void win32k_refresh_taskbar(void) {
    if (!w32k_use_gpu()) return;
    if (!s_shell_initialized) return;                 // nada composto ainda
    if (__atomic_test_and_set(&s_composing, __ATOMIC_ACQUIRE)) return;
    cursor_hide();                                    // tira a setinha (save-under)
    int W  = scr_width();
    int ty = scr_height() - SH_TASKBAR_H10;
    // Base fresca sob a taskbar (p/ o "acrilico" nao driftar ao reblender).
    w32k_rgb_rect(0, ty, W, SH_TASKBAR_H10, UI_WALL_BOT);
    shell_taskbar_tc();
    cursor_show();                                    // redesenha a setinha por cima
    gpu_present();
    __atomic_clear(&s_composing, __ATOMIC_RELEASE);
}

// ----------------------------------------------------------------------------
//  shell_compose: chamado por win32k_compose ao final (apos draw_taskbar e
//  antes do cursor). Repinta a area de trabalho com o visual Windows 10.
// ----------------------------------------------------------------------------
void win32k_shell_compose(void) {
    if (!w32k_use_gpu() && !fb_active()) return;   // nada para pintar
    if (!s_shell_initialized) {
        s_shell_initialized = 1;
        kputs("[shell] Win10-style desktop compose ativo.\n");
    }
    shell_draw_wallpaper();

    // Redesenha a chrome estilo Win10 para cada janela top-level visivel.
    // O BASE ja desenhou a chrome simples; aqui ampliamos a barra com
    // a extensao + botoes no canto superior direito.
    for (int i = 0; i < MAX_WINDOWS; i++) {
        WND* w = &s_wins[i];
        if (!w->used || !w->visible) continue;
        if (w->flags & WNDF_DESKTOP) continue;
        if (s_minimized[i]) {
            // Janela "minimizada" — apaga a janela do desktop (deixa wallpaper).
            // (No proximo compose o BASE nao redesenhara: tornamos a janela
            // ainda visivel pro hit-test, mas pintamos por cima do wallpaper.)
            w32k_fill_rect(w->x, w->y, w->w, w->h, SH_WALLPAPER);
            continue;
        }
        int active = (w->id == s_focus_id);
        shell_draw_chrome_extension(w, active);
        shell_draw_window_buttons(w, i);
        // Re-desenha o titulo por cima (ja foi escrito pelo BASE, mas o
        // espacamento mudou — escrevemos centralizado verticalmente na barra
        // de SH_TITLE_H px).
        w32k_draw_text(w->x + 6, w->y + 5, w->title, SH_CHROME_TEXT, 0xFF);
        // Borda azul de 1px ao redor da janela (toque Windows 10).
        w32k_rect(w->x, w->y, w->w, w->h,
                  active ? FB_LIGHT_BLUE : FB_DARK_GRAY);
    }

    shell_draw_taskbar();
    shell_draw_start_menu();
    kputs("[shell] compose: wallpaper+taskbar+start_btn+clock");
    if (s_start_menu_open) kputs("+start_menu");
    if (s_drag_hwnd) { kputs("+dragging#"); kput_dec((uint64_t)s_drag_hwnd); }
    kputc('\n');
}

// ============================================================================
//  shell_on_mouse: chamado por win32k_on_mouse_event AO INICIO. Trata cliques
//  na taskbar / start button / window chrome / drag.
//  Recebe a posicao ABSOLUTA do cursor + estado novo dos botoes + estado
//  anterior. Retorno: 1 se a shell consumiu o evento (e o BASE deve PULAR o
//  roteamento normal de WM_MOUSE*); 0 caso contrario.
// ============================================================================
int win32k_shell_on_mouse(int x, int y, uint32_t buttons, uint32_t prev_buttons) {
    uint32_t edge_down = buttons & ~prev_buttons;  // bits que ACABARAM de ligar
    uint32_t edge_up   = prev_buttons & ~buttons;  // bits que ACABARAM de desligar

    int ty = scr_height() - SH_TASKBAR_H10;
    int in_taskbar = (y >= ty);

    // ---------- DRAG: se estamos arrastando, atualiza a posicao da janela ----
    if (s_drag_hwnd) {
        int slot = shell_slot_of(s_drag_hwnd);
        if (slot >= 0) {
            WND* w = &s_wins[slot];
            w->x = x - s_drag_offset_x;
            w->y = y - s_drag_offset_y;
            // Clipa ao desktop (impede arrastar pra fora completamente).
            int SW = scr_width();
            if (w->x < 0) w->x = 0;
            if (w->y < 0) w->y = 0;
            if (w->x > SW - 40) w->x = SW - 40;       // 40px sempre visiveis
            if (w->y > ty - 4)  w->y = ty - 4;
            if (edge_up & 0x01) {                       // soltou o botao esquerdo
                kputs("[shell] drag end HWND #");
                kput_dec((uint64_t)s_drag_hwnd); kputc('\n');
                s_drag_hwnd = 0;
            }
            return 1;                                    // consumimos o evento
        }
        s_drag_hwnd = 0;                                 // janela sumiu
    }

    // ---------- CLIQUE no botao Iniciar -----------------------------------
    if (in_taskbar && x < SH_START_BTN_W && (edge_down & 0x01)) {
        s_start_menu_open = !s_start_menu_open;
        kputs("[shell] Start menu ");
        kputs(s_start_menu_open ? "ABERTO\n" : "FECHADO\n");
        if (s_was_active) win32k_compose();
        return 1;
    }

    // ---------- CLIQUE em algum botao de app da taskbar --------------------
    if (in_taskbar && x >= SH_START_BTN_W && (edge_down & 0x01)) {
        int bx = SH_START_BTN_W + 4;
        int max_x = scr_width() - SH_CLOCK_W - 4;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            WND* w = &s_wins[i];
            if (!w->used || !w->visible || (w->flags & WNDF_DESKTOP)) continue;
            if (bx + SH_APP_BTN_W > max_x) break;
            if (x >= bx && x < bx + SH_APP_BTN_W) {
                // Clique no botao do app: alterna minimizada / foco.
                if (s_minimized[i]) {
                    s_minimized[i] = 0;
                    s_focus_id = w->id;
                    kputs("[shell] restaurando janela #"); kput_dec((uint64_t)w->id);
                    kputc('\n');
                } else if (w->id == s_focus_id) {
                    s_minimized[i] = 1;
                    kputs("[shell] minimizando janela #"); kput_dec((uint64_t)w->id);
                    kputc('\n');
                } else {
                    s_focus_id = w->id;
                    kputs("[shell] foco -> janela #"); kput_dec((uint64_t)w->id);
                    kputc('\n');
                }
                if (s_was_active) win32k_compose();
                return 1;
            }
            bx += SH_APP_BTN_W + 2;
        }
    }

    // ---------- FECHA o start menu se clicar fora ------------------------
    if (s_start_menu_open && (edge_down & 0x01)) {
        // Esta dentro do retangulo do menu?
        int my = scr_height() - SH_TASKBAR_H10 - SH_MENU_H;
        if (my < 0) my = 0;
        int inside_menu = (x >= 0 && x < SH_MENU_W &&
                           y >= my && y < my + SH_MENU_H);
        int on_start_btn = (in_taskbar && x < SH_START_BTN_W);
        if (!inside_menu && !on_start_btn) {
            s_start_menu_open = 0;
            kputs("[shell] Start menu fechado (clique fora).\n");
            if (s_was_active) win32k_compose();
            return 1;
        }
    }

    // ---------- WINDOW CHROME: hit-test em [_] [[]] [X] -------------------
    // Encontra a janela "normal" debaixo do cursor (do topo do z-order p/ baixo).
    int hit_slot = -1; int hit_z = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        WND* w = &s_wins[i];
        if (!w->used || !w->visible || (w->flags & WNDF_DESKTOP)) continue;
        if (s_minimized[i]) continue;
        if (x < w->x || x >= w->x + w->w) continue;
        if (y < w->y || y >= w->y + w->h) continue;
        if (w->z > hit_z) { hit_z = w->z; hit_slot = i; }
    }

    if (hit_slot >= 0) {
        WND* w = &s_wins[hit_slot];
        int x_close = w->x + w->w - SH_BTN_W - 1;
        int x_max   = x_close - SH_BTN_W;
        int x_min   = x_max   - SH_BTN_W;
        int on_close = (x >= x_close && x < x_close + SH_BTN_W &&
                        y >= w->y    && y < w->y + SH_BTN_H);
        int on_max   = (x >= x_max   && x < x_max   + SH_BTN_W &&
                        y >= w->y    && y < w->y + SH_BTN_H);
        int on_min   = (x >= x_min   && x < x_min   + SH_BTN_W &&
                        y >= w->y    && y < w->y + SH_BTN_H);
        int on_title = (y >= w->y && y < w->y + SH_TITLE_H &&
                        !on_close && !on_max && !on_min);

        // -- LBUTTONDOWN sobre um botao da chrome -> "arma" o clique --
        if (edge_down & 0x01) {
            if (on_close) {
                s_pending_close_idx = hit_slot;
                if (s_was_active) win32k_compose();
                return 1;
            }
            if (on_max) {
                s_pending_max_idx = hit_slot;
                if (s_was_active) win32k_compose();
                return 1;
            }
            if (on_min) {
                s_pending_min_idx = hit_slot;
                if (s_was_active) win32k_compose();
                return 1;
            }
            if (on_title) {
                // Inicia drag: armazena offset cursor->topo-esq da janela.
                s_drag_hwnd     = w->id;
                s_drag_offset_x = x - w->x;
                s_drag_offset_y = y - w->y;
                s_focus_id      = w->id;        // janela arrastada recebe foco
                kputs("[shell] drag start HWND #"); kput_dec((uint64_t)w->id);
                kputs(" offset=(");
                kput_dec((uint64_t)s_drag_offset_x); kputc(',');
                kput_dec((uint64_t)s_drag_offset_y); kputs(")\n");
                if (s_was_active) win32k_compose();
                return 1;
            }
        }

        // -- LBUTTONUP sobre um botao "armado" -> executa a acao --
        if (edge_up & 0x01) {
            if (s_pending_close_idx == hit_slot && on_close) {
                kputs("[shell] close HWND #"); kput_dec((uint64_t)w->id); kputc('\n');
                queue_post((void*)(uintptr_t)w->id, WM_CLOSE, 0, 0);
                s_pending_close_idx = -1;
                if (s_was_active) win32k_compose();
                return 1;
            }
            if (s_pending_min_idx == hit_slot && on_min) {
                kputs("[shell] minimize HWND #"); kput_dec((uint64_t)w->id); kputc('\n');
                s_minimized[hit_slot] = 1;
                if (s_focus_id == w->id) s_focus_id = 0;
                s_pending_min_idx = -1;
                if (s_was_active) win32k_compose();
                return 1;
            }
            if (s_pending_max_idx == hit_slot && on_max) {
                kputs("[shell] maximize toggle HWND #");
                kput_dec((uint64_t)w->id); kputc('\n');
                if (s_maximized[hit_slot]) {
                    // Restaura tamanho original.
                    w->x = s_saved_x[hit_slot];
                    w->y = s_saved_y[hit_slot];
                    w->w = s_saved_w[hit_slot];
                    w->h = s_saved_h[hit_slot];
                    s_maximized[hit_slot] = 0;
                } else {
                    // Salva e maximiza (tela menos taskbar).
                    s_saved_x[hit_slot] = w->x;
                    s_saved_y[hit_slot] = w->y;
                    s_saved_w[hit_slot] = w->w;
                    s_saved_h[hit_slot] = w->h;
                    w->x = 0; w->y = 0;
                    w->w = scr_width();
                    w->h = scr_height() - SH_TASKBAR_H10;
                    s_maximized[hit_slot] = 1;
                }
                s_pending_max_idx = -1;
                if (s_was_active) win32k_compose();
                return 1;
            }
        }
    }

    // Qualquer LBUTTONUP limpa pendingens (cancela o clique fora do botao).
    if (edge_up & 0x01) {
        s_pending_close_idx = -1;
        s_pending_max_idx   = -1;
        s_pending_min_idx   = -1;
    }

    return 0;                                  // nao consumimos -> BASE rota WM_*
}

// ----------------------------------------------------------------------------
//  Init publico da shell. Hoje so zera o estado por-janela; a primeira compose
//  faz o log de ativacao.
// ----------------------------------------------------------------------------
void win32k_shell_init(void) {
    s_start_menu_open = 0;
    s_drag_hwnd = 0;
    s_drag_offset_x = 0;
    s_drag_offset_y = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        s_minimized[i] = 0;
        s_maximized[i] = 0;
    }
    s_pending_close_idx = -1;
    s_pending_min_idx   = -1;
    s_pending_max_idx   = -1;
    kputs("[shell] win32k_shell_init: estado zerado (start_menu=fechado).\n");
}
