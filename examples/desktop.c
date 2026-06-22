// desktop.c  —  DESKTOP estilo Windows (FASE 6), rodando em RING 3 no MeuOS.
//
// Compoe um ambiente grafico completo sobre o win32k (FASE 2):
//   * uma janela de FUNDO (papel de parede / wallpaper) que cobre a tela;
//   * uma BARRA DE TAREFAS (desenhada pelo win32k no compose) com o botao
//     "Iniciar" + um botao por janela aberta (a com foco fica destacada);
//   * uma ou mais janelas de CONSOLE rodando o shell cmd DENTRO da janela: o
//     texto do shell e desenhado via gdi32 (TextOutA, branco sobre fundo preto)
//     na area cliente, e o teclado e roteado pela FILA DE MENSAGENS da janela
//     com foco (WM_CHAR). Suporta abrir MULTIPLAS janelas (2 cmd no demo).
//
// Caminho de cada operacao (igual ao Windows):
//   ring3 (desktop) -> user32/gdi32 -> ntdll -> int 0x80 -> win32k (kernel) ->
//   framebuffer (drivers/video.c). Cada passo loga na serial.
//
// MODO DEMO (headless-testavel): apos subir o desktop e abrir 2 janelas de cmd,
// "digita" comandos em cada janela (via PostKeyToWindow, determinismo sem
// teclado real) e troca o foco entre elas (SetFocus), terminando com
// PostQuitMessage. Com -display, o teclado real (IRQ1) digita na janela com foco.

unsigned long _tls_index = 0;

typedef unsigned long long ULL;

// ============================================================================
//  Imports (kernel32 / user32 / gdi32) — ABI Microsoft, resolvidos pelo loader.
// ============================================================================
// ---- console/serial (loga o que o desktop faz, alem dos logs do kernel) ----
#define STD_OUTPUT_HANDLE ((unsigned)-11)
__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len, unsigned* wrote, void* ov);
__declspec(dllimport) void  ExitProcess(unsigned code);
// FASE 5 — usadas pelos comandos do shell dentro da janela:
#define MEUOS_SERVICE_STOPPED  1
#define MEUOS_SERVICE_RUNNING  4
typedef struct _MEUOS_PROCESS_ENTRY {
    unsigned           ProcessId;
    unsigned           Terminated;
    unsigned long long ImageBase;
    unsigned           ThreadCount;
    char               ImageName[32];
} MEUOS_PROCESS_ENTRY;
typedef struct _MEUOS_DRIVER_ENTRY {
    unsigned State; unsigned LastStatus; char Name[32];
} MEUOS_DRIVER_ENTRY;
__declspec(dllimport) int EnumProcessesEx(unsigned index, MEUOS_PROCESS_ENTRY* out);
__declspec(dllimport) int EnumDriversEx(unsigned index, MEUOS_DRIVER_ENTRY* out);
__declspec(dllimport) int StartDriverServiceA(const char* name);
__declspec(dllimport) int StopDriverServiceA(const char* name);

// ---- user32 ----
#define WM_CREATE  0x0001
#define WM_DESTROY 0x0002
#define WM_PAINT   0x000F
#define WM_KEYDOWN 0x0100
#define WM_CHAR    0x0102
#define SW_SHOW    5

typedef struct { int x, y; } POINT;
typedef struct { void* hwnd; unsigned message; ULL wParam; ULL lParam; unsigned time; POINT pt; } MSG;
typedef long long (*WNDPROC)(void*, unsigned, ULL, ULL);
typedef struct {
    unsigned style; WNDPROC lpfnWndProc; int cbClsExtra; int cbWndExtra;
    void* hInstance; void* hIcon; void* hCursor; void* hbrBackground;
    const char* lpszMenuName; const char* lpszClassName;
} WNDCLASSA;
typedef struct { int left, top, right, bottom; } RECT;
typedef struct { void* hdc; int fErase; int rc[4]; int fRestore; int fIncUpdate; char rgb[32]; } PAINTSTRUCT;

__declspec(dllimport) unsigned short RegisterClassA(const WNDCLASSA*);
__declspec(dllimport) void* CreateWindowExA(unsigned, const char*, const char*, unsigned,
        int, int, int, int, void*, void*, void*, void*);
// FASE 6 — janela de fundo (wallpaper) e janela de console (cmd):
__declspec(dllimport) void* CreateDesktopWindowA(const char* cls, const char* title,
        unsigned bgColor, WNDPROC unusedProc);
__declspec(dllimport) void* CreateConsoleWindowA(const char* cls, const char* title,
        int x, int y, int w, int h, unsigned bgColor);
__declspec(dllimport) int   ShowWindow(void*, int);
__declspec(dllimport) int   GetMessageA(MSG*, void*, unsigned, unsigned);
__declspec(dllimport) int   TranslateMessage(const MSG*);
__declspec(dllimport) long long DispatchMessageA(const MSG*);
__declspec(dllimport) long long DefWindowProcA(void*, unsigned, ULL, ULL);
__declspec(dllimport) void* BeginPaint(void*, PAINTSTRUCT*);
__declspec(dllimport) int   EndPaint(void*, const PAINTSTRUCT*);
__declspec(dllimport) void  PostQuitMessage(int);
__declspec(dllimport) int   InvalidateRect(void*, void*, int);
__declspec(dllimport) void* SetFocus(void*);
__declspec(dllimport) int   PostKeyToWindow(void*, int ascii);

// ---- gdi32 ----
__declspec(dllimport) int   TextOutA(void*, int, int, const char*, int);
__declspec(dllimport) unsigned SetTextColor(void*, unsigned color);

// Cores (indices da paleta do mode 13h).
#define COL_BLACK 0
#define COL_GREEN 2
#define COL_CYAN  3
#define COL_WHITE 15
#define COL_NAVY  1
#define COL_GRAY  7
#define COL_DKGRAY 8

// ============================================================================
//  Utilitarios sem CRT.
// ============================================================================
static unsigned slen(const char* s) { unsigned n = 0; if (s) while (s[n]) n++; return n; }
static void* g_out;   // STDOUT (serial) — espelha o que sai nas janelas
static void say(const char* s) { unsigned w = 0; WriteFile(g_out, s, slen(s), &w, 0); }

static char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int ieq(const char* a, const char* b) {
    while (*a && *b) { if (lower_c(*a) != lower_c(*b)) return 0; a++; b++; } return *a == *b;
}
static const char* word(const char* src, char* dst, int max) {
    while (*src == ' ' || *src == '\t') src++;
    int i = 0;
    while (*src && *src != ' ' && *src != '\t' && i < max - 1) dst[i++] = *src++;
    dst[i] = 0; return src;
}
static void utoa(unsigned v, char* b) {
    char t[12]; int i = 0;
    if (v == 0) { b[0] = '0'; b[1] = 0; return; }
    while (v) { t[i++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0; while (i) b[j++] = t[--i]; b[j] = 0;
}

// ============================================================================
//  Estado por janela de console (uma "console window"): grade de texto.
//  Cada celula = 1 char; desenhamos com a fonte 8x8 (8x8 px por celula).
// ============================================================================
#define MAX_COLS 36
#define MAX_ROWS 22
typedef struct {
    void*   hwnd;
    int     cols, rows;          // tamanho da grade (derivado do tamanho da janela)
    int     cur_row;             // linha do cursor (onde a proxima saida vai)
    char    line[128];           // linha de comando sendo digitada
    int     line_len;
    char    grid[MAX_ROWS][MAX_COLS + 1];   // backing store do texto (p/ repaint)
    int     used;
} CONSOLE;

#define MAX_CONSOLES 4
static CONSOLE g_cons[MAX_CONSOLES];
static int     g_ncons;
static void*   g_wall;     // HWND do papel de parede (p/ repintar no encerramento)

static CONSOLE* con_of(void* hwnd) {
    for (int i = 0; i < g_ncons; i++) if (g_cons[i].hwnd == hwnd) return &g_cons[i];
    return 0;
}

// Cor do texto por linha lembrada para o repaint (prompt/saida em cores).
// Simplificacao: tudo branco, exceto o prompt em verde (desenhado no repaint).

// Escreve UMA linha 's' na grade na linha cur_row e avanca; rola se encher.
// (so backing store; o desenho real e em con_redraw / con_paint_line.)
static void con_putline(CONSOLE* c, const char* s) {
    if (!c) return;
    if (c->cur_row >= c->rows) {
        // rola uma linha para cima
        for (int r = 1; r < c->rows; r++)
            for (int k = 0; k <= c->cols; k++) c->grid[r - 1][k] = c->grid[r][k];
        c->cur_row = c->rows - 1;
        c->grid[c->cur_row][0] = 0;
    }
    int k = 0; while (s[k] && k < c->cols) { c->grid[c->cur_row][k] = s[k]; k++; }
    c->grid[c->cur_row][k] = 0;
    c->cur_row++;
}

// Desenha UMA linha de texto na janela (TextOutA), na cor dada. y = row*8.
static void con_draw_row(CONSOLE* c, void* hdc, int row, const char* s, unsigned color) {
    SetTextColor(hdc, color);
    TextOutA(hdc, 2, 2 + row * 8, s, -1);
}

// Redesenha toda a area cliente da console (no WM_PAINT). Loga na serial.
static void con_paint(CONSOLE* c, void* hdc) {
    for (int r = 0; r < c->cur_row && r < c->rows; r++)
        con_draw_row(c, hdc, r, c->grid[r], COL_WHITE);
}

// ============================================================================
//  Comandos do shell — escrevem na grade da janela E na serial (prova headless).
// ============================================================================
static void emit(CONSOLE* c, const char* s) { con_putline(c, s); say("    [win] "); say(s); say("\n"); }

static const char* drv_state_name(unsigned st) {
    if (st == MEUOS_SERVICE_RUNNING) return "RUNNING";
    if (st == MEUOS_SERVICE_STOPPED) return "STOPPED";
    return "UNKNOWN";
}

static void cmd_help(CONSOLE* c) {
    emit(c, "Comandos:");
    emit(c, " help tasklist");
    emit(c, " sc query/start/stop");
    emit(c, " ver cls exit");
}
static void cmd_ver(CONSOLE* c) { emit(c, "MeuOS [versao 0.1]"); }

static void cmd_tasklist(CONSOLE* c) {
    emit(c, "PID  Imagem");
    MEUOS_PROCESS_ENTRY e; unsigned i = 0, n = 0; char buf[40];
    while (EnumProcessesEx(i, &e)) {
        char num[12]; utoa(e.ProcessId, num);
        int p = 0; buf[p++] = ' ';
        for (unsigned k = 0; num[k]; k++) buf[p++] = num[k];
        while (p < 5) buf[p++] = ' ';
        for (unsigned k = 0; e.ImageName[k] && p < 38; k++) buf[p++] = e.ImageName[k];
        buf[p] = 0; emit(c, buf); n++; i++;
    }
    char t[24]; char num[12]; utoa(n, num);
    int p = 0; const char* pre = "Total: ";
    for (unsigned k = 0; pre[k]; k++) t[p++] = pre[k];
    for (unsigned k = 0; num[k]; k++) t[p++] = num[k];
    t[p] = 0; emit(c, t);
}

static void cmd_sc_query(CONSOLE* c) {
    emit(c, "Servico        Estado");
    MEUOS_DRIVER_ENTRY e; unsigned i = 0; char buf[40];
    while (EnumDriversEx(i, &e)) {
        int p = 0;
        for (unsigned k = 0; e.Name[k] && p < 16; k++) buf[p++] = e.Name[k];
        while (p < 16) buf[p++] = ' ';
        const char* st = drv_state_name(e.State);
        for (unsigned k = 0; st[k] && p < 38; k++) buf[p++] = st[k];
        buf[p] = 0; emit(c, buf); i++;
    }
}
static void cmd_sc(CONSOLE* c, const char* args) {
    char sub[24]; const char* rest = word(args, sub, sizeof(sub));
    if (ieq(sub, "query")) { cmd_sc_query(c); return; }
    if (ieq(sub, "start")) {
        char n[32]; word(rest, n, sizeof(n));
        say("    [win] sc start "); say(n); say("\n");
        if (StartDriverServiceA(n)) emit(c, "  -> RUNNING"); else emit(c, "  -> falhou");
        return;
    }
    if (ieq(sub, "stop")) {
        char n[32]; word(rest, n, sizeof(n));
        say("    [win] sc stop "); say(n); say("\n");
        if (StopDriverServiceA(n)) emit(c, "  -> STOPPED"); else emit(c, "  -> falhou");
        return;
    }
    emit(c, "uso: sc query|start|stop");
}

// Executa uma linha de comando dentro da janela 'c'. Devolve 0 se foi 'exit'.
static int con_run(CONSOLE* c, const char* line) {
    char cmd[24]; const char* rest = word(line, cmd, sizeof(cmd));
    if (cmd[0] == 0) return 1;
    if (ieq(cmd, "help") || ieq(cmd, "?")) { cmd_help(c); return 1; }
    if (ieq(cmd, "ver"))      { cmd_ver(c); return 1; }
    if (ieq(cmd, "tasklist")) { cmd_tasklist(c); return 1; }
    if (ieq(cmd, "sc"))       { cmd_sc(c, rest); return 1; }
    if (ieq(cmd, "cls"))      { c->cur_row = 0; return 1; }
    if (ieq(cmd, "exit"))     { emit(c, "encerrando..."); return 0; }
    { char b[48]; int p = 0; const char* pre = "? ";
      for (unsigned k = 0; pre[k]; k++) b[p++] = pre[k];
      for (unsigned k = 0; cmd[k] && p < 46; k++) b[p++] = cmd[k];
      b[p] = 0; emit(c, b); }
    return 1;
}

// Mostra o prompt "C:\>" + a linha digitada na grade (sem avancar; o Enter avanca).
static void con_prompt(CONSOLE* c) {
    char b[140]; int p = 0; const char* pr = "C:\\> ";
    for (unsigned k = 0; pr[k]; k++) b[p++] = pr[k];
    for (int k = 0; k < c->line_len && p < 138; k++) b[p++] = c->line[k];
    b[p] = 0;
    // o prompt fica na linha corrente sem avancar cur_row (sera reescrito)
    int k = 0; while (b[k] && k < c->cols) { c->grid[c->cur_row][k] = b[k]; k++; }
    c->grid[c->cur_row][k] = 0;
}

// ============================================================================
//  WNDPROC do console: WM_PAINT desenha a grade; WM_CHAR digita/executa.
// ============================================================================
static long long ConsoleProc(void* hwnd, unsigned msg, ULL wParam, ULL lParam) {
    CONSOLE* c = con_of(hwnd);
    switch (msg) {
    case WM_CREATE:
        say("  [desktop] WM_CREATE de uma janela de console.\n");
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; void* hdc = BeginPaint(hwnd, &ps);
        if (c) {
            // garante o prompt na linha corrente antes de pintar
            con_prompt(c);
            con_paint(c, hdc);
            say("  [desktop] WM_PAINT: redesenhei a console (");
            { char n[12]; utoa((unsigned)c->cur_row + 1, n); say(n); }
            say(" linhas).\n");
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        return 0;
    case WM_CHAR: {
        if (!c) return 0;
        char ch = (char)(unsigned char)wParam;
        if (ch == '\n' || ch == '\r') {
            // termina a linha do prompt na grade, executa e mostra novo prompt
            con_prompt(c);              // fixa "C:\> <linha>" na linha corrente
            c->cur_row++;               // o prompt vira historico
            int quit = (con_run(c, c->line) == 0);   // 'exit' -> encerra o desktop
            c->line_len = 0; c->line[0] = 0;
            con_prompt(c);              // novo prompt vazio
            InvalidateRect(hwnd, 0, 0); // repinta ESTA janela (com a saida do cmd)
            if (quit) {
                // Encerramento (no FIM da fila, depois de todos os repaints ja
                // enfileirados): repinta o papel de parede (a marca d'agua
                // sobrevive) e posta WM_QUIT. Quadro final limpo e completo.
                if (g_wall) InvalidateRect(g_wall, 0, 0);
                PostQuitMessage(0);
            }
        } else if (ch == '\b') {
            if (c->line_len > 0) c->line_len--;
            c->line[c->line_len] = 0;
            con_prompt(c); InvalidateRect(hwnd, 0, 0);
        } else if (c->line_len < (int)sizeof(c->line) - 1) {
            c->line[c->line_len++] = ch; c->line[c->line_len] = 0;
            con_prompt(c); InvalidateRect(hwnd, 0, 0);
        }
        return 0;
    }
    case WM_DESTROY:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// WNDPROC do papel de parede: desenha um texto de marca d'agua no WM_PAINT.
static long long DesktopProc(void* hwnd, unsigned msg, ULL wParam, ULL lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; void* hdc = BeginPaint(hwnd, &ps);
        SetTextColor(hdc, COL_WHITE);
        // Marca d'agua no canto inferior-esquerdo, na faixa LIVRE entre as
        // janelas e a barra de tarefas (nao fica sob nenhuma janela).
        TextOutA(hdc, 6, 150, "MeuOS Desktop - FASE 6", -1);
        EndPaint(hwnd, &ps);
        say("  [desktop] WM_PAINT do papel de parede (marca d'agua).\n");
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// ============================================================================
//  Criacao + injecao de comandos (demo headless).
// ============================================================================
static CONSOLE* new_console(const char* title, int x, int y, int w, int h) {
    if (g_ncons >= MAX_CONSOLES) return 0;
    CONSOLE* c = &g_cons[g_ncons];
    void* hwnd = CreateConsoleWindowA("MeuConsole", title, x, y, w, h, COL_NAVY);
    if (!hwnd) { say("  [desktop] CreateConsoleWindow falhou.\n"); return 0; }
    c->hwnd = hwnd; c->used = 1;
    c->cols = (w - 4) / 8; if (c->cols > MAX_COLS) c->cols = MAX_COLS;
    c->rows = (h - 14 - 2) / 8; if (c->rows > MAX_ROWS) c->rows = MAX_ROWS;
    c->cur_row = 0; c->line_len = 0; c->line[0] = 0;
    for (int r = 0; r < MAX_ROWS; r++) c->grid[r][0] = 0;
    g_ncons++;
    // banner inicial
    con_putline(c, title);
    con_prompt(c);
    ShowWindow(hwnd, SW_SHOW);
    say("  [desktop] console aberta: "); say(title); say("\n");
    return c;
}

// "Digita" uma string + Enter numa janela especifica (PostKeyToWindow), igual a
// uma sequencia de WM_CHAR. Determinismo headless; o teclado real faz o mesmo.
static void type_cmd(void* hwnd, const char* s) {
    say("  [desktop] (demo) digitando em HWND: \""); say(s); say("\"\n");
    while (*s) PostKeyToWindow(hwnd, (unsigned char)*s++);
    PostKeyToWindow(hwnd, '\n');     // Enter -> executa
}

void _start(void) {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    say("\n============================================================\n");
    say("  MeuOS Desktop (FASE 6): wallpaper + barra de tarefas + cmd\n");
    say("============================================================\n");

    // 1) Registra as classes (wallpaper + console).
    WNDCLASSA wc;
    for (unsigned i = 0; i < sizeof(wc); i++) ((char*)&wc)[i] = 0;
    wc.lpfnWndProc = DesktopProc; wc.lpszClassName = "MeuDesktop";
    RegisterClassA(&wc);
    wc.lpfnWndProc = ConsoleProc; wc.lpszClassName = "MeuConsole";
    RegisterClassA(&wc);

    // 2) Papel de parede (cobre a tela; fundo azul-marinho).
    say("  [desktop] criando o papel de parede (wallpaper)...\n");
    void* wall = CreateDesktopWindowA("MeuDesktop", "wallpaper", COL_NAVY, DesktopProc);
    g_wall = wall;
    ShowWindow(wall, SW_SHOW);

    // 3) Duas janelas de cmd (multiplas janelas), lado a lado.
    CONSOLE* c1 = new_console("cmd #1", 8, 16, 150, 120);
    CONSOLE* c2 = new_console("cmd #2", 162, 30, 150, 110);

    // 4) DEMO: digita comandos em cada janela e troca o foco entre elas. As
    //    teclas vao para a FILA DE MENSAGENS da janela alvo (como o teclado real
    //    por IRQ1 faz com a janela com foco). O foco alterna entre as 2 janelas
    //    (SetFocus) — a barra de titulo e o botao da barra de tarefas refletem.
    say("\n  [desktop] --- modo demo: executando comandos nas janelas ---\n");
    if (c1) {
        SetFocus(c1->hwnd);
        type_cmd(c1->hwnd, "help");
        type_cmd(c1->hwnd, "ver");
        type_cmd(c1->hwnd, "tasklist");
    }
    if (c2) {
        SetFocus(c2->hwnd);          // foco vai p/ a 2a janela (barra de titulo azul)
        type_cmd(c2->hwnd, "sc query");
        type_cmd(c2->hwnd, "sc start mydriver.sys");
        // o 'exit' (na c1) encerra o desktop por ULTIMO; antes, devolve o foco
        // a c1 para o quadro final destacar a 1a janela (com o tasklist).
        SetFocus(c1->hwnd);
        type_cmd(c1->hwnd, "exit");  // 'exit' -> WNDPROC repinta wallpaper + WM_QUIT
    }

    // 5) Loop de mensagens (igual ao Windows). DispatchMessage chama o WNDPROC
    //    da janela alvo (em ring 3); o win32k so entrega a MSG e compoe a tela.
    //    O 'exit' (ultima tecla) dispara, no FIM da fila, o repaint do wallpaper
    //    e o WM_QUIT — entao o quadro final fica completo (todas as janelas
    //    repintadas + marca d'agua) antes de o loop terminar.
    say("\n  [desktop] entrando no loop de mensagens (GetMessage)...\n");
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    say("  [desktop] loop encerrado (WM_QUIT). Desktop montado; encerrando demo.\n");
    ExitProcess(0);
}
