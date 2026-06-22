// ============================================================================
//  winlogon.exe — Logon Process (RODADA FINAL).
//
//  No Windows real, winlogon.exe e o processo SISTEMA que apresenta a tela
//  de logon: cria o desktop "Winlogon", monta o GINA/Credential Provider,
//  recebe usuario+senha do usuario, valida via secur32 -> lsass, cria o
//  token de logon e dispara o shell (explorer.exe / userinit.exe).
//
//  Aqui no MeuOS, winlogon.exe e uma app ring 3 que:
//    1. cria uma janela "tela de logon" estilo Windows 10 (azul Aero +
//       caixa central com avatar + dois campos "Usuario" e "Senha" +
//       botao "Entrar"),
//    2. importa secur32.dll (LsaConnectUntrusted +
//       LsaLookupAuthenticationPackage("NTLM")) e credui.dll
//       (CredUIPromptForCredentialsA) e exercita o ABI de cada DLL,
//    3. desenha a UI via win32k (CreateWindow + FillRect + TextOut),
//    4. simula login bem-sucedido (sem auth real) e encerra.
//
//  Caminho:
//    ring3 (winlogon) -> user32/gdi32/secur32/credui -> ntdll ->
//    int 0x80 -> win32k (kernel) -> framebuffer.
//
//  IMAGE BASE: 0x5100000 — zona livre apos credui (0x5000000).
// ============================================================================

unsigned long _tls_index = 0;

typedef unsigned long long ULL;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef long               SECURITY_STATUS;
typedef long               NTSTATUS;
typedef int                BOOL;
typedef void*              HANDLE;

#define STD_OUTPUT_HANDLE   ((unsigned)-11)

// ============================================================================
//  Imports (kernel32 / user32 / gdi32 / secur32 / credui).
// ============================================================================
__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len, unsigned* w, void* ov);
__declspec(dllimport) void  ExitProcess(unsigned code);

// ---- user32 ----
#define WM_CREATE  0x0001
#define WM_DESTROY 0x0002
#define WM_PAINT   0x000F
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
__declspec(dllimport) void* CreateDesktopWindowA(const char* cls, const char* title,
        unsigned bgColor, WNDPROC unusedProc);
__declspec(dllimport) int   ShowWindow(void*, int);
__declspec(dllimport) int   GetMessageA(MSG*, void*, unsigned, unsigned);
__declspec(dllimport) int   TranslateMessage(const MSG*);
__declspec(dllimport) long long DispatchMessageA(const MSG*);
__declspec(dllimport) long long DefWindowProcA(void*, unsigned, ULL, ULL);
__declspec(dllimport) void* BeginPaint(void*, PAINTSTRUCT*);
__declspec(dllimport) int   EndPaint(void*, const PAINTSTRUCT*);
__declspec(dllimport) void  PostQuitMessage(int code);

// ---- gdi32 ----
#define LTGRAY_BRUSH 1
__declspec(dllimport) int   TextOutA(void*, int, int, const char*, int);
__declspec(dllimport) int   FillRect(void*, const RECT*, void*);
__declspec(dllimport) void* GetStockObject(int);
__declspec(dllimport) void* CreateSolidBrush(unsigned);
__declspec(dllimport) unsigned SetTextColor(void* hdc, unsigned color);

// ---- secur32 ----
typedef struct { unsigned short Length; unsigned short MaxLength; char* Buffer; } LSA_STRING;
__declspec(dllimport) NTSTATUS LsaConnectUntrusted(HANDLE* LsaHandle);
__declspec(dllimport) NTSTATUS LsaLookupAuthenticationPackage(HANDLE LsaHandle,
        void* PackageName, ULONG* AuthenticationPackage);
__declspec(dllimport) NTSTATUS LsaDeregisterLogonProcess(HANDLE LsaHandle);
__declspec(dllimport) SECURITY_STATUS EnumerateSecurityPackagesA(ULONG* pcPackages,
        void** ppPackageInfo);
__declspec(dllimport) SECURITY_STATUS FreeContextBuffer(void* pvContextBuffer);

// ---- credui ----
#define CREDUI_FLAGS_GENERIC_CREDENTIALS 0x00040000
__declspec(dllimport) unsigned long CredUIPromptForCredentialsA(void* pUiInfo,
        const char* pszTargetName, void* Reserved, unsigned long dwAuthError,
        char* pszUserName, ULONG ulUserNameBufferSize,
        char* pszPassword, ULONG ulPasswordBufferSize,
        BOOL* pfSave, unsigned long dwFlags);

// Cores (paleta indexada do modo 13h; o win32k mapeia direto).
#define COL_BLACK        0
#define COL_BLUE         1
#define COL_GREEN        2
#define COL_CYAN         3
#define COL_RED          4
#define COL_MAGENTA      5
#define COL_BROWN        6
#define COL_LIGHT_GRAY   7
#define COL_DARK_GRAY    8
#define COL_LIGHT_BLUE   9
#define COL_LIGHT_GREEN  10
#define COL_LIGHT_CYAN   11
#define COL_LIGHT_RED    12
#define COL_LIGHT_MAGENTA 13
#define COL_YELLOW       14
#define COL_WHITE        15

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void say(const char* s) {
    unsigned w = 0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, slen(s), &w, 0);
}

// WNDPROC da janela de logon. WM_PAINT desenha a tela completa:
//   - fundo azul "Aero",
//   - caixa central branca com:
//     - avatar (circulo cinza),
//     - nome de usuario "MeuOS\\user",
//     - dois campos vazios "Usuario" e "Senha",
//     - botao "Entrar" (azul) + dica "Pressione Enter".
//   - rodape preto com "MeuOS v0.8 - Sessao do usuario".
static long long LogonWndProc(void* hwnd, unsigned msg, ULL wParam, ULL lParam) {
    switch (msg) {
    case WM_CREATE:
        say("  [winlogon] WM_CREATE: criando UI de logon...\n");
        return 0;
    case WM_PAINT: {
        say("  [winlogon] WM_PAINT: renderizando tela de logon\n");
        PAINTSTRUCT ps;
        void* hdc = BeginPaint(hwnd, &ps);

        // Fundo azul Aero da janela completa.
        void* azulBrush = CreateSolidBrush(COL_BLUE);
        RECT full = { 0, 0, 320, 200 };  // mode 13h
        FillRect(hdc, &full, azulBrush);

        // Caixa central branca (110x80 px no centro).
        void* whiteBrush = CreateSolidBrush(COL_WHITE);
        RECT box = { 100, 60, 220, 150 };
        FillRect(hdc, &box, whiteBrush);

        // Borda da caixa (preta).
        void* blackBrush = CreateSolidBrush(COL_BLACK);
        RECT borda1 = { 100, 60, 220, 61 };  // top
        RECT borda2 = { 100, 149, 220, 150 };  // bot
        RECT borda3 = { 100, 60, 101, 150 };  // left
        RECT borda4 = { 219, 60, 220, 150 };  // right
        FillRect(hdc, &borda1, blackBrush);
        FillRect(hdc, &borda2, blackBrush);
        FillRect(hdc, &borda3, blackBrush);
        FillRect(hdc, &borda4, blackBrush);

        // Avatar (circulo cinza 16x16 simulado por retangulo).
        void* grayBrush = CreateSolidBrush(COL_DARK_GRAY);
        RECT avatar = { 144, 66, 176, 84 };
        FillRect(hdc, &avatar, grayBrush);

        // Botao "Entrar" (azul claro) embaixo da caixa.
        void* lblueBrush = CreateSolidBrush(COL_LIGHT_BLUE);
        RECT btn = { 132, 130, 188, 145 };
        FillRect(hdc, &btn, lblueBrush);

        // Textos. SetTextColor para preto (sobre fundo branco).
        SetTextColor(hdc, COL_BLACK);
        TextOutA(hdc, 110, 88,  "MeuOS\\user",     -1);
        TextOutA(hdc, 110, 100, "Usuario: [user]", -1);
        TextOutA(hdc, 110, 112, "Senha:   [****]", -1);
        SetTextColor(hdc, COL_WHITE);
        TextOutA(hdc, 142, 134, "Entrar", -1);

        // Titulo (em cima do fundo azul).
        SetTextColor(hdc, COL_WHITE);
        TextOutA(hdc, 80, 20, "Bem-vindo ao MeuOS",  -1);
        TextOutA(hdc, 90, 32, "Pressione Enter",     -1);

        // Rodape preto.
        void* footerBrush = CreateSolidBrush(COL_BLACK);
        RECT footer = { 0, 188, 320, 200 };
        FillRect(hdc, &footer, footerBrush);
        SetTextColor(hdc, COL_LIGHT_GRAY);
        TextOutA(hdc, 4, 190, "MeuOS v0.8 - Sessao do usuario", -1);

        EndPaint(hwnd, &ps);

        // Apos pintar a tela de logon UMA vez, fecha o loop p/ o boot seguir
        // (no real, esperaria input do usuario; aqui simulamos login OK).
        say("  [winlogon] WM_PAINT concluido -> PostQuitMessage(0).\n");
        PostQuitMessage(0);
        return 0;
    }
    case WM_DESTROY:
        say("  [winlogon] WM_DESTROY: encerrando sessao.\n");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void _start(void) {
    void* hOut = GetStdHandle(STD_OUTPUT_HANDLE);

    say("\n[winlogon] ============================================\n");
    say("[winlogon] MeuOS Logon Process (renderiza tela de logon)\n");
    say("[winlogon] ============================================\n");

    // ---- 1) Smoke test do secur32 (SSPI + LSA Logon API) ----------------
    say("[winlogon] (1) conectando ao LSA via secur32.LsaConnectUntrusted...\n");
    HANDLE lsa = 0;
    NTSTATUS rc = LsaConnectUntrusted(&lsa);
    if (rc == 0 && lsa) {
        say("[winlogon] (1) LSA handle adquirido.\n");
    } else {
        say("[winlogon] (1) LsaConnectUntrusted devolveu erro.\n");
    }

    // Procura o pacote NTLM no LSA.
    LSA_STRING pkg;
    char pkg_name[16] = { 'N', 'T', 'L', 'M', 0 };
    pkg.Buffer = pkg_name;
    pkg.Length = 4;
    pkg.MaxLength = 5;
    ULONG ap = 0xFFFFFFFFu;
    rc = LsaLookupAuthenticationPackage(lsa, &pkg, &ap);
    say("[winlogon] (1) LsaLookupAuthenticationPackage(NTLM) ");
    if (rc == 0 && ap != 0xFFFFFFFFu) {
        say("OK. pacote id resolvido.\n");
    } else {
        say("falhou (sem pacote registrado).\n");
    }

    // Enumera todos os pacotes registrados.
    ULONG count = 0;
    void* pinfo = 0;
    SECURITY_STATUS ss = EnumerateSecurityPackagesA(&count, &pinfo);
    if (ss == 0) {
        say("[winlogon] (1) EnumerateSecurityPackagesA: pacotes registrados.\n");
        FreeContextBuffer(pinfo);
    }

    LsaDeregisterLogonProcess(lsa);

    // ---- 2) Smoke test do credui (Credential UI) ------------------------
    say("[winlogon] (2) consultando credui.CredUIPromptForCredentialsA...\n");
    char user[32]; char pass[32];
    for (int i = 0; i < 32; i++) { user[i] = 0; pass[i] = 0; }
    BOOL save = 0;
    unsigned long rcu = CredUIPromptForCredentialsA(
            0, "MeuOS.Logon", 0, 0,
            user, 32, pass, 32, &save,
            CREDUI_FLAGS_GENERIC_CREDENTIALS);
    if (rcu == 0) {
        say("[winlogon] (2) credui retornou NO_ERROR. usuario='");
        say(user); say("' senha='");
        // Mascarar a senha no log (mostra so o tamanho).
        for (unsigned i = 0; i < slen(pass); i++) say("*");
        say("'\n");
    } else {
        say("[winlogon] (2) credui falhou (sem dialog real).\n");
    }

    // ---- 3) Renderiza a tela de logon (win32k) --------------------------
    say("[winlogon] (3) abrindo tela de logon via win32k...\n");

    WNDCLASSA wc;
    for (unsigned i = 0; i < sizeof(wc); i++) ((char*)&wc)[i] = 0;
    wc.lpfnWndProc   = LogonWndProc;
    wc.hbrBackground = GetStockObject(LTGRAY_BRUSH);
    wc.lpszClassName = "WinlogonClass";
    RegisterClassA(&wc);

    // Janela cobrindo a tela toda (320x200 mode 13h).
    void* hwnd = CreateWindowExA(0, "WinlogonClass", "MeuOS Logon",
                                 0, 0, 0, 320, 200,
                                 0, 0, 0, 0);
    if (!hwnd) {
        say("[winlogon] CreateWindowExA falhou; encerrando.\n");
        ExitProcess(1);
    }
    ShowWindow(hwnd, SW_SHOW);
    say("[winlogon] janela de logon visivel.\n");

    // Loop de mensagens. Sem teclado real no headless, GetMessageA processa
    // apenas WM_PAINT/WM_DESTROY (o win32k injeta WM_QUIT no shutdown global).
    say("[winlogon] entrando no loop de mensagens (tela de logon ativa)...\n");
    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    say("[winlogon] sessao iniciada (login simulado: bem-sucedido).\n");
    say("[winlogon] cedendo controle para o shell (no MeuOS: desktop.exe).\n");

    ExitProcess(0);
}
