// ============================================================================
//  credui.dll — Credential UI helper (RODADA FINAL).
//
//  Reimplementacao minima do credui.dll do Windows 10/11. credui e a DLL que
//  apresenta a janela de dialogo de credencial padrao do Windows ("Conecte-se
//  a este servidor: digite usuario e senha"). Por baixo, ela usa o LSA
//  (atraves do secur32) para validar e/ou salvar as credenciais no
//  Credential Manager (vault de %appdata%\\Microsoft\\Credentials).
//
//  Apps RDP, IE, OneDrive, etc. usam CredUIPromptForCredentialsW p/ pedir
//  login do usuario sem ter que escrever sua propria caixa de dialogo.
//
//  Aqui no MeuOS:
//    - Nao temos vault em disco; salvamos credenciais num pool estatico
//      (max 16) que persiste enquanto a DLL estiver carregada.
//    - CredUIPromptForCredentialsA nao mostra dialogo (headless): se a app
//      passou bufs nao-vazios, devolve "user" e "pass" como exemplo; senao
//      retorna NO_ERROR. Apps que importam acham todos os simbolos.
//    - CredRead/CredWrite/CredEnumerate/CredDelete operam no pool em memoria.
//
//  IMAGE BASE: 0x5000000 — zona livre apos secur32 (0x4F00000), com .reloc via
//  --dynamicbase.
// ============================================================================

unsigned int _tls_index = 0;

// ============================================================================
//  Tipos Windows minimos.
// ============================================================================
typedef unsigned int       UINT;
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef unsigned short     WORD;
typedef unsigned short     WCHAR;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef unsigned char      BYTE;
typedef void*              HANDLE;
typedef void*              HWND;
typedef void*              LPVOID;
typedef char*              LPSTR;
typedef const char*        LPCSTR;
typedef WCHAR*             LPWSTR;
typedef const WCHAR*       LPCWSTR;
typedef void*              PVOID;
typedef unsigned long long ULL;
typedef long long          LL;

#define TRUE   1
#define FALSE  0
#define NULL   ((void*)0)

// Codigos de retorno (CREDUI_*).
#define NO_ERROR                            0
#define ERROR_INVALID_PARAMETER             87
#define ERROR_INSUFFICIENT_BUFFER           122
#define ERROR_NOT_FOUND                     1168
#define ERROR_CANCELLED                     1223
#define CREDUI_NO_PASSWORD                  1325

// Tipos de credencial.
#define CRED_TYPE_GENERIC                   1
#define CRED_TYPE_DOMAIN_PASSWORD           2
#define CRED_TYPE_DOMAIN_CERTIFICATE        3

// Flags de CredUIPromptForCredentials.
#define CREDUI_FLAGS_GENERIC_CREDENTIALS    0x00040000
#define CREDUI_FLAGS_DO_NOT_PERSIST         0x00000002
#define CREDUI_FLAGS_PERSIST                0x00001000
#define CREDUI_FLAGS_EXPECT_CONFIRMATION    0x00020000

#define CRED_MAX_USERNAME_LENGTH            (256 + 1)
#define CRED_MAX_CREDENTIAL_BLOB_SIZE       (5 * 512)
#define CRED_MAX_STRING_LENGTH              256
#define CREDUI_MAX_USERNAME_LENGTH          513
#define CREDUI_MAX_PASSWORD_LENGTH          256

// CREDUI_INFOA — struct passada para CredUIPromptForCredentialsA.
typedef struct _CREDUI_INFOA {
    DWORD cbSize;
    HWND  hwndParent;
    LPCSTR pszMessageText;
    LPCSTR pszCaptionText;
    HANDLE hbmBanner;
} CREDUI_INFOA, *PCREDUI_INFOA;

typedef struct _CREDUI_INFOW {
    DWORD cbSize;
    HWND  hwndParent;
    LPCWSTR pszMessageText;
    LPCWSTR pszCaptionText;
    HANDLE hbmBanner;
} CREDUI_INFOW, *PCREDUI_INFOW;

// CREDENTIALA — entrada do vault (Cred*).
typedef struct _CREDENTIAL_ATTRIBUTEA {
    LPSTR Keyword;
    DWORD Flags;
    DWORD ValueSize;
    BYTE* Value;
} CREDENTIAL_ATTRIBUTEA, *PCREDENTIAL_ATTRIBUTEA;

typedef struct _CREDENTIALA {
    DWORD Flags;
    DWORD Type;
    LPSTR TargetName;
    LPSTR Comment;
    LL    LastWritten;
    DWORD CredentialBlobSize;
    BYTE* CredentialBlob;
    DWORD Persist;
    DWORD AttributeCount;
    PCREDENTIAL_ATTRIBUTEA Attributes;
    LPSTR TargetAlias;
    LPSTR UserName;
} CREDENTIALA, *PCREDENTIALA;

// ============================================================================
//  Pool estatico (vault em memoria).
// ============================================================================
#define MAX_CREDS  16
#define CRED_BLOB  256
#define CRED_NAME  64

static struct {
    BOOL  used;
    DWORD type;
    DWORD persist;
    char  target[CRED_NAME];
    char  username[CRED_NAME];
    BYTE  blob[CRED_BLOB];
    DWORD blob_size;
} g_vault[MAX_CREDS];

static int slen(const char* s) { int n = 0; if (s) while (s[n]) n++; return n; }
static void scopy(char* d, const char* s, int max) {
    int i = 0; if (s) for (; i < max - 1 && s[i]; i++) d[i] = s[i]; d[i] = 0;
}
static int seq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

// Acha pelo target. -1 se nao encontrar.
static int vault_find(const char* target) {
    if (!target) return -1;
    for (int i = 0; i < MAX_CREDS; i++)
        if (g_vault[i].used && seq(g_vault[i].target, target)) return i;
    return -1;
}
static int vault_alloc(void) {
    for (int i = 0; i < MAX_CREDS; i++) if (!g_vault[i].used) return i;
    return -1;
}

// ============================================================================
//  CredUIPromptForCredentialsA — dialogo de credenciais.
//
//  Sem display real (estamos rodando em ring 3 do MeuOS headless), nao
//  podemos mostrar uma janela de input. Mas o ABI precisa estar 100%: se a
//  app passou strings vazias nos buffers de saida (pszUserName/pszPassword),
//  preenchemos com valores padrao "user" e "pass" e retornamos NO_ERROR
//  (como se o usuario tivesse digitado e clicado OK). Em uma versao com UI,
//  bastaria substituir esta funcao por uma que abre uma janela win32k.
// ============================================================================
__declspec(dllexport) DWORD CredUIPromptForCredentialsA(PCREDUI_INFOA pUiInfo,
                                                        LPCSTR pszTargetName,
                                                        void* Reserved,
                                                        DWORD dwAuthError,
                                                        LPSTR pszUserName,
                                                        ULONG ulUserNameBufferSize,
                                                        LPSTR pszPassword,
                                                        ULONG ulPasswordBufferSize,
                                                        BOOL* pfSave,
                                                        DWORD dwFlags) {
    (void)pUiInfo; (void)Reserved; (void)dwAuthError; (void)dwFlags;

    if (!pszUserName || !pszPassword) return ERROR_INVALID_PARAMETER;
    if (ulUserNameBufferSize == 0 || ulPasswordBufferSize == 0)
        return ERROR_INSUFFICIENT_BUFFER;

    // Se a app ja preencheu UserName, mantemos. Senao usamos "user".
    if (pszUserName[0] == 0) {
        const char* def = "user";
        ULONG i = 0; while (def[i] && i < ulUserNameBufferSize - 1) { pszUserName[i] = def[i]; i++; }
        pszUserName[i] = 0;
    }
    // Se a app ja preencheu Password, mantemos. Senao usamos "pass".
    if (pszPassword[0] == 0) {
        const char* def = "pass";
        ULONG i = 0; while (def[i] && i < ulPasswordBufferSize - 1) { pszPassword[i] = def[i]; i++; }
        pszPassword[i] = 0;
    }

    // Auto-salvar no vault se a app pediu PERSIST.
    if (pszTargetName && (dwFlags & CREDUI_FLAGS_PERSIST)) {
        int idx = vault_find(pszTargetName);
        if (idx < 0) idx = vault_alloc();
        if (idx >= 0) {
            g_vault[idx].used = TRUE;
            g_vault[idx].type = (dwFlags & CREDUI_FLAGS_GENERIC_CREDENTIALS)
                                 ? CRED_TYPE_GENERIC : CRED_TYPE_DOMAIN_PASSWORD;
            g_vault[idx].persist = 3;   // CRED_PERSIST_ENTERPRISE
            scopy(g_vault[idx].target,   pszTargetName, CRED_NAME);
            scopy(g_vault[idx].username, pszUserName,   CRED_NAME);
            int n = slen(pszPassword);
            if (n > CRED_BLOB - 1) n = CRED_BLOB - 1;
            for (int i = 0; i < n; i++) g_vault[idx].blob[i] = (BYTE)pszPassword[i];
            g_vault[idx].blob[n] = 0;
            g_vault[idx].blob_size = (DWORD)n;
        }
    }

    if (pfSave) *pfSave = TRUE;
    return NO_ERROR;
}

__declspec(dllexport) DWORD CredUIPromptForCredentialsW(PCREDUI_INFOW pUiInfo,
                                                        LPCWSTR pszTargetName,
                                                        void* Reserved,
                                                        DWORD dwAuthError,
                                                        LPWSTR pszUserName,
                                                        ULONG ulUserNameBufferSize,
                                                        LPWSTR pszPassword,
                                                        ULONG ulPasswordBufferSize,
                                                        BOOL* pfSave,
                                                        DWORD dwFlags) {
    (void)pUiInfo; (void)Reserved; (void)dwAuthError; (void)pszTargetName;
    (void)dwFlags;
    if (!pszUserName || !pszPassword) return ERROR_INVALID_PARAMETER;
    if (ulUserNameBufferSize == 0 || ulPasswordBufferSize == 0)
        return ERROR_INSUFFICIENT_BUFFER;

    if (pszUserName[0] == 0) {
        const WCHAR def[] = { 'u','s','e','r',0 };
        ULONG i = 0; while (def[i] && i < ulUserNameBufferSize - 1) { pszUserName[i] = def[i]; i++; }
        pszUserName[i] = 0;
    }
    if (pszPassword[0] == 0) {
        const WCHAR def[] = { 'p','a','s','s',0 };
        ULONG i = 0; while (def[i] && i < ulPasswordBufferSize - 1) { pszPassword[i] = def[i]; i++; }
        pszPassword[i] = 0;
    }
    if (pfSave) *pfSave = TRUE;
    return NO_ERROR;
}

// CredUIConfirmCredentialsA — chamada depois que a app testou as credenciais.
// Se bConfirm=TRUE, garantimos que o vault contem; se FALSE, removemos.
__declspec(dllexport) DWORD CredUIConfirmCredentialsA(LPCSTR pszTargetName,
                                                     BOOL bConfirm) {
    if (!pszTargetName) return ERROR_INVALID_PARAMETER;
    int idx = vault_find(pszTargetName);
    if (bConfirm) return NO_ERROR;  // ja foi salvo no prompt
    if (idx >= 0) g_vault[idx].used = FALSE;
    return NO_ERROR;
}

__declspec(dllexport) DWORD CredUIConfirmCredentialsW(LPCWSTR pszTargetName, BOOL bConfirm) {
    (void)pszTargetName;
    return bConfirm ? NO_ERROR : NO_ERROR;
}

// CredUIParseUserNameA — separa "DOMAIN\\user" em DOMAIN e user.
__declspec(dllexport) DWORD CredUIParseUserNameA(LPCSTR userName,
                                                  LPSTR user, ULONG userBufferSize,
                                                  LPSTR domain, ULONG domainBufferSize) {
    if (!userName || !user || !domain) return ERROR_INVALID_PARAMETER;
    // Procura o '\\'.
    const char* slash = NULL;
    for (const char* p = userName; *p; p++) if (*p == '\\') { slash = p; break; }
    if (slash) {
        ULONG dn = (ULONG)(slash - userName);
        if (dn >= domainBufferSize) return ERROR_INSUFFICIENT_BUFFER;
        for (ULONG i = 0; i < dn; i++) domain[i] = userName[i];
        domain[dn] = 0;
        // user
        const char* up = slash + 1;
        ULONG un = (ULONG)slen(up);
        if (un >= userBufferSize) return ERROR_INSUFFICIENT_BUFFER;
        for (ULONG i = 0; i < un; i++) user[i] = up[i];
        user[un] = 0;
    } else {
        // Sem dominio.
        ULONG un = (ULONG)slen(userName);
        if (un >= userBufferSize) return ERROR_INSUFFICIENT_BUFFER;
        for (ULONG i = 0; i < un; i++) user[i] = userName[i];
        user[un] = 0;
        if (domainBufferSize > 0) domain[0] = 0;
    }
    return NO_ERROR;
}

__declspec(dllexport) DWORD CredUIParseUserNameW(LPCWSTR userName,
                                                  LPWSTR user, ULONG userBufferSize,
                                                  LPWSTR domain, ULONG domainBufferSize) {
    (void)userName; (void)user; (void)userBufferSize; (void)domain; (void)domainBufferSize;
    if (userBufferSize > 0 && user) user[0] = 0;
    if (domainBufferSize > 0 && domain) domain[0] = 0;
    return NO_ERROR;
}

// CredUIStoreSSOCredW / CredUIReadSSOCredW — single sign-on (stub).
__declspec(dllexport) DWORD CredUIStoreSSOCredW(LPCWSTR pszRealm, LPCWSTR pszUsername,
                                                  LPCWSTR pszPassword, BOOL bPersist) {
    (void)pszRealm; (void)pszUsername; (void)pszPassword; (void)bPersist;
    return NO_ERROR;
}

__declspec(dllexport) DWORD CredUIReadSSOCredW(LPCWSTR pszRealm, LPWSTR* ppszUsername) {
    (void)pszRealm;
    if (ppszUsername) *ppszUsername = NULL;
    return ERROR_NOT_FOUND;
}

// ============================================================================
//  Cred* — APIs Win32 do Credential Manager (advapi32/credui no Windows).
// ============================================================================

// Buffer estatico para CredRead retornar (apps esperam buffer alocado pelo
// sistema que persiste ate CredFree).
static CREDENTIALA g_cred_read_buf;

__declspec(dllexport) BOOL CredReadA(LPCSTR TargetName, DWORD Type, DWORD Flags,
                                      PCREDENTIALA* Credential) {
    (void)Type; (void)Flags;
    if (!TargetName || !Credential) return FALSE;
    int idx = vault_find(TargetName);
    if (idx < 0) return FALSE;

    g_cred_read_buf.Flags = 0;
    g_cred_read_buf.Type = g_vault[idx].type;
    g_cred_read_buf.TargetName = g_vault[idx].target;
    g_cred_read_buf.Comment = (char*)"MeuOS vault";
    g_cred_read_buf.LastWritten = 0;
    g_cred_read_buf.CredentialBlobSize = g_vault[idx].blob_size;
    g_cred_read_buf.CredentialBlob = g_vault[idx].blob;
    g_cred_read_buf.Persist = g_vault[idx].persist;
    g_cred_read_buf.AttributeCount = 0;
    g_cred_read_buf.Attributes = NULL;
    g_cred_read_buf.TargetAlias = NULL;
    g_cred_read_buf.UserName = g_vault[idx].username;
    *Credential = &g_cred_read_buf;
    return TRUE;
}

__declspec(dllexport) BOOL CredWriteA(PCREDENTIALA Credential, DWORD Flags) {
    (void)Flags;
    if (!Credential || !Credential->TargetName) return FALSE;
    int idx = vault_find(Credential->TargetName);
    if (idx < 0) idx = vault_alloc();
    if (idx < 0) return FALSE;
    g_vault[idx].used = TRUE;
    g_vault[idx].type = Credential->Type ? Credential->Type : CRED_TYPE_GENERIC;
    g_vault[idx].persist = Credential->Persist ? Credential->Persist : 3;
    scopy(g_vault[idx].target, Credential->TargetName, CRED_NAME);
    scopy(g_vault[idx].username, Credential->UserName ? Credential->UserName : "", CRED_NAME);
    DWORD n = Credential->CredentialBlobSize;
    if (n > CRED_BLOB) n = CRED_BLOB;
    for (DWORD i = 0; i < n; i++)
        g_vault[idx].blob[i] = Credential->CredentialBlob ? Credential->CredentialBlob[i] : 0;
    g_vault[idx].blob_size = n;
    return TRUE;
}

__declspec(dllexport) BOOL CredDeleteA(LPCSTR TargetName, DWORD Type, DWORD Flags) {
    (void)Type; (void)Flags;
    if (!TargetName) return FALSE;
    int idx = vault_find(TargetName);
    if (idx < 0) return FALSE;
    g_vault[idx].used = FALSE;
    return TRUE;
}

// CredEnumerateA — enumera credenciais por filtro (Win32 espera array alocado).
// Limit p/ stub: devolve um array estatico de no max MAX_CREDS ponteiros.
static PCREDENTIALA g_enum_arr[MAX_CREDS];
static CREDENTIALA  g_enum_recs[MAX_CREDS];

__declspec(dllexport) BOOL CredEnumerateA(LPCSTR Filter, DWORD Flags,
                                           DWORD* Count, PCREDENTIALA** Credential) {
    (void)Filter; (void)Flags;
    if (!Count || !Credential) return FALSE;
    DWORD n = 0;
    for (int i = 0; i < MAX_CREDS; i++) {
        if (!g_vault[i].used) continue;
        g_enum_recs[n].Flags = 0;
        g_enum_recs[n].Type = g_vault[i].type;
        g_enum_recs[n].TargetName = g_vault[i].target;
        g_enum_recs[n].Comment = NULL;
        g_enum_recs[n].LastWritten = 0;
        g_enum_recs[n].CredentialBlobSize = g_vault[i].blob_size;
        g_enum_recs[n].CredentialBlob = g_vault[i].blob;
        g_enum_recs[n].Persist = g_vault[i].persist;
        g_enum_recs[n].AttributeCount = 0;
        g_enum_recs[n].Attributes = NULL;
        g_enum_recs[n].TargetAlias = NULL;
        g_enum_recs[n].UserName = g_vault[i].username;
        g_enum_arr[n] = &g_enum_recs[n];
        n++;
    }
    *Count = n;
    *Credential = g_enum_arr;
    if (n == 0) return FALSE;
    return TRUE;
}

__declspec(dllexport) void CredFree(PVOID Buffer) {
    (void)Buffer;  // pool estatico
}

__declspec(dllexport) BOOL CredIsMarshaledCredentialA(LPCSTR MarshaledCredential) {
    (void)MarshaledCredential;
    return FALSE;
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved;
    return 1;
}
