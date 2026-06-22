// ============================================================================
//  secur32.dll — Security Support Provider Interface (SSPI) — RODADA FINAL.
//
//  Reimplementacao minima do secur32.dll do Windows 10/11. SSPI e a API ring 3
//  que apps usam para autenticacao (NTLM/Kerberos/Negotiate/Schannel). No
//  Windows real, secur32.dll e a fachada — por baixo ela chama lsass.exe (Local
//  Security Authority Subsystem) via ALPC para que o LSA execute a logica de
//  protocolo de autenticacao em um processo separado, e os pacotes (NTLM,
//  Kerberos, Negotiate, Schannel) sao plugados no LSA via SSP/AP packages.
//
//  Aqui no MeuOS nao temos lsass.exe rodando em outro processo, entao todas as
//  chamadas SSPI retornam SEC_E_OK ou um handle sentinela. Apps que importam
//  AcquireCredentialsHandleA/InitializeSecurityContextA/AcceptSecurityContext
//  rodam sem missing import: o ABI esta completo.
//
//  Funcoes expostas (SSPI subset):
//    AcquireCredentialsHandleA/W   — adquire credenciais do usuario corrente
//    FreeCredentialsHandle         — libera o handle
//    InitializeSecurityContextA/W  — cliente: inicia handshake
//    AcceptSecurityContext         — servidor: aceita um pacote do cliente
//    DeleteSecurityContext         — destroi o contexto
//    QueryContextAttributes        — pega atributos do contexto (sizes, names)
//    EncryptMessage / DecryptMessage — wrap/unwrap de buffers (gssapi-like)
//    MakeSignature / VerifySignature — assinaturas
//    EnumerateSecurityPackagesA    — lista os SSPs disponiveis
//    QuerySecurityPackageInfoA     — info do SSP por nome
//    FreeContextBuffer             — libera buffers alocados pelo SSPI
//
//  Funcoes expostas (LSA Logon API subset):
//    LsaConnectUntrusted           — abre handle untrusted para o LSA
//    LsaLookupAuthenticationPackage — acha o pacote por nome (NTLM/Kerberos)
//    LsaCallAuthenticationPackage  — envia uma msg para o pacote no LSA
//    LsaDeregisterLogonProcess     — fecha o handle
//
//  IMAGE BASE: 0x4F00000 — zona livre apos ws2_32 (0x4E00000), com .reloc via
//  --dynamicbase (mesma estrategia das outras DLLs >= PMM_BASE 0x4000000).
// ============================================================================

unsigned int _tls_index = 0;

// ============================================================================
//  Tipos Windows minimos.
// ============================================================================
typedef unsigned int       UINT;
typedef unsigned long      DWORD;
typedef int                BOOL;
typedef int                INT;
typedef unsigned short     WORD;
typedef unsigned short     WCHAR;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef unsigned char      BYTE;
typedef long               HRESULT;
typedef long               SECURITY_STATUS;
typedef unsigned long long ULL;
typedef void*              HANDLE;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef char*              LPSTR;
typedef const char*        LPCSTR;
typedef WCHAR*             LPWSTR;
typedef const WCHAR*       LPCWSTR;
typedef void*              PVOID;

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

// SSPI status codes (subset).
#define SEC_E_OK                          0x00000000L
#define SEC_E_INSUFFICIENT_MEMORY         0x80090300L
#define SEC_E_INVALID_HANDLE              0x80090301L
#define SEC_E_UNSUPPORTED_FUNCTION        0x80090302L
#define SEC_E_INVALID_TOKEN               0x80090308L
#define SEC_E_NO_CREDENTIALS              0x8009030EL
#define SEC_I_CONTINUE_NEEDED             0x00090312L
#define SEC_I_COMPLETE_NEEDED             0x00090313L
#define SEC_I_COMPLETE_AND_CONTINUE       0x00090314L
#define SEC_E_LOGON_DENIED                0x8009030CL

// Credential use (AcquireCredentialsHandle).
#define SECPKG_CRED_INBOUND               0x00000001
#define SECPKG_CRED_OUTBOUND              0x00000002
#define SECPKG_CRED_BOTH                  0x00000003

// SSP packages canonicos (nomes no Windows).
// Estes nomes seriam passados em pszPackage = "NTLM" / "Kerberos" / "Negotiate".

// Sentinelas: handles opacos que devolvemos como prova de "credencial valida".
#define CRED_HANDLE_SENTINEL_LOW   0xC2EDEA1ULL
#define CTX_HANDLE_SENTINEL_LOW    0xC0E7E771ULL
#define LSA_HANDLE_SENTINEL        ((HANDLE)(ULL)0x15A00000ULL)

// SECURITY_INTEGER (parte alta/baixa de 64 bits) — Windows usa LARGE_INTEGER.
typedef struct _SECURITY_INTEGER {
    ULONG LowPart;
    LONG  HighPart;
} TimeStamp, *PTimeStamp;

// SecHandle: par de pointers opacos (na pratica, par de ULL).
typedef struct _SecHandle {
    ULL dwLower;
    ULL dwUpper;
} SecHandle, CredHandle, CtxtHandle;

// SecBuffer / SecBufferDesc — semantica gssapi-like.
typedef struct _SecBuffer {
    ULONG cbBuffer;
    ULONG BufferType;
    void* pvBuffer;
} SecBuffer, *PSecBuffer;
typedef struct _SecBufferDesc {
    ULONG ulVersion;
    ULONG cBuffers;
    PSecBuffer pBuffers;
} SecBufferDesc, *PSecBufferDesc;

#define SECBUFFER_VERSION                0
#define SECBUFFER_EMPTY                  0
#define SECBUFFER_DATA                   1
#define SECBUFFER_TOKEN                  2
#define SECBUFFER_STREAM_HEADER          7
#define SECBUFFER_STREAM_TRAILER         6

// SecPkgInfoA: info de um SSP.
typedef struct _SecPkgInfoA {
    ULONG fCapabilities;
    WORD  wVersion;
    WORD  wRPCID;
    ULONG cbMaxToken;
    LPSTR Name;
    LPSTR Comment;
} SecPkgInfoA, *PSecPkgInfoA;

// PSecPkgContext_Sizes: usado por QueryContextAttributes(SECPKG_ATTR_SIZES).
typedef struct _SecPkgContext_Sizes {
    ULONG cbMaxToken;
    ULONG cbMaxSignature;
    ULONG cbBlockSize;
    ULONG cbSecurityTrailer;
} SecPkgContext_Sizes, *PSecPkgContext_Sizes;

#define SECPKG_ATTR_SIZES                0x00000000

// ============================================================================
//  Pool estatico de credenciais e contextos (nao alocavel — pool simples).
// ============================================================================
#define MAX_CRED   16
#define MAX_CTX    16

static struct {
    BOOL  used;
    ULONG package_id;      // 0=NTLM, 1=Kerberos, 2=Negotiate, 3=Schannel
    ULONG credential_use;  // INBOUND/OUTBOUND/BOTH
    char  username[64];
} g_creds[MAX_CRED];

static struct {
    BOOL  used;
    ULL   cred_handle_lower;
    ULONG handshake_step;  // 0=init, 1=msg1 sent, 2=done
    ULONG context_flags;
} g_ctxs[MAX_CTX];

// Pacotes SSP que expomos (mesmos do Windows).
static const char* g_pkg_names[]    = { "NTLM", "Kerberos", "Negotiate", "Schannel", "WDigest" };
static const char* g_pkg_comments[] = {
    "NTLM Security Package",
    "Microsoft Kerberos V1.0",
    "Microsoft Negotiate (SPNEGO)",
    "Schannel Security Package (SSL/TLS)",
    "Digest SSPI Authentication Package"
};
#define PKG_COUNT  (int)(sizeof(g_pkg_names)/sizeof(g_pkg_names[0]))

// Resolve nome do pacote -> id (0..4) ou -1 se desconhecido.
static int pkg_lookup(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < PKG_COUNT; i++) {
        const char* a = name; const char* b = g_pkg_names[i];
        int eq = 1;
        while (*a && *b) {
            char x = *a, y = *b;
            if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
            if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
            if (x != y) { eq = 0; break; }
            a++; b++;
        }
        if (eq && *a == 0 && *b == 0) return i;
    }
    return -1;
}

// Aloca um slot de credencial; retorna -1 se cheio.
static int alloc_cred(void) {
    for (int i = 0; i < MAX_CRED; i++) if (!g_creds[i].used) return i;
    return -1;
}
static int alloc_ctx(void) {
    for (int i = 0; i < MAX_CTX; i++) if (!g_ctxs[i].used) return i;
    return -1;
}

// ============================================================================
//  EnumerateSecurityPackagesA / QuerySecurityPackageInfoA.
// ============================================================================

// Buffer estatico unico para EnumerateSecurityPackages (apps esperam um array
// alocado pelo SSPI que persiste ate FreeContextBuffer). Como temos um stub
// single-thread, podemos usar um pool fixo.
static SecPkgInfoA g_enum_buf[PKG_COUNT];

__declspec(dllexport) SECURITY_STATUS EnumerateSecurityPackagesA(ULONG* pcPackages,
                                                                  PSecPkgInfoA* ppPackageInfo) {
    if (!pcPackages || !ppPackageInfo) return SEC_E_INVALID_HANDLE;
    for (int i = 0; i < PKG_COUNT; i++) {
        g_enum_buf[i].fCapabilities = 0x00000017;  // INTEGRITY|PRIVACY|TOKEN_ONLY|CONNECTION
        g_enum_buf[i].wVersion      = 1;
        g_enum_buf[i].wRPCID        = (WORD)(0xFFFF);
        g_enum_buf[i].cbMaxToken    = 12000;       // tipico para Kerberos PAC
        g_enum_buf[i].Name          = (char*)g_pkg_names[i];
        g_enum_buf[i].Comment       = (char*)g_pkg_comments[i];
    }
    *pcPackages = PKG_COUNT;
    *ppPackageInfo = g_enum_buf;
    return SEC_E_OK;
}

__declspec(dllexport) SECURITY_STATUS QuerySecurityPackageInfoA(LPSTR pszPackageName,
                                                                 PSecPkgInfoA* ppPackageInfo) {
    int id = pkg_lookup(pszPackageName);
    if (id < 0 || !ppPackageInfo) return SEC_E_INVALID_HANDLE;
    g_enum_buf[id].fCapabilities = 0x00000017;
    g_enum_buf[id].wVersion      = 1;
    g_enum_buf[id].wRPCID        = (WORD)(0xFFFF);
    g_enum_buf[id].cbMaxToken    = 12000;
    g_enum_buf[id].Name          = (char*)g_pkg_names[id];
    g_enum_buf[id].Comment       = (char*)g_pkg_comments[id];
    *ppPackageInfo = &g_enum_buf[id];
    return SEC_E_OK;
}

__declspec(dllexport) SECURITY_STATUS FreeContextBuffer(void* pvContextBuffer) {
    (void)pvContextBuffer;  // pool estatico — nada a liberar
    return SEC_E_OK;
}

// ============================================================================
//  AcquireCredentialsHandleA — adquire credenciais p/ um SSP.
// ============================================================================

__declspec(dllexport) SECURITY_STATUS AcquireCredentialsHandleA(
        LPSTR pszPrincipal, LPSTR pszPackage, ULONG fCredentialUse,
        void* pvLogonID, void* pAuthData, void* pGetKeyFn, void* pvGetKeyArgument,
        CredHandle* phCredential, PTimeStamp ptsExpiry) {
    (void)pszPrincipal; (void)pvLogonID; (void)pAuthData; (void)pGetKeyFn; (void)pvGetKeyArgument;
    int pkg = pkg_lookup(pszPackage);
    if (pkg < 0) return SEC_E_INVALID_HANDLE;
    int slot = alloc_cred();
    if (slot < 0) return SEC_E_INSUFFICIENT_MEMORY;
    g_creds[slot].used = TRUE;
    g_creds[slot].package_id = (ULONG)pkg;
    g_creds[slot].credential_use = fCredentialUse;
    // username padrao (MeuOS\\user)
    const char* def = "user";
    int i = 0; while (def[i] && i < 63) { g_creds[slot].username[i] = def[i]; i++; }
    g_creds[slot].username[i] = 0;
    if (phCredential) {
        phCredential->dwLower = CRED_HANDLE_SENTINEL_LOW | (ULL)slot;
        phCredential->dwUpper = (ULL)pkg;
    }
    if (ptsExpiry) {
        ptsExpiry->LowPart  = 0xFFFFFFFFu;
        ptsExpiry->HighPart = 0x7FFFFFFF;   // praticamente nunca expira
    }
    return SEC_E_OK;
}

__declspec(dllexport) SECURITY_STATUS AcquireCredentialsHandleW(
        LPWSTR pszPrincipal, LPWSTR pszPackage, ULONG fCredentialUse,
        void* pvLogonID, void* pAuthData, void* pGetKeyFn, void* pvGetKeyArgument,
        CredHandle* phCredential, PTimeStamp ptsExpiry) {
    // Converte pszPackage WSTR para um buffer ASCII curto e chama a A.
    char pkgA[32]; int i = 0;
    if (pszPackage) {
        for (; i < 31 && pszPackage[i]; i++) pkgA[i] = (char)(pszPackage[i] & 0x7F);
    }
    pkgA[i] = 0;
    return AcquireCredentialsHandleA(NULL, pkgA, fCredentialUse, pvLogonID, pAuthData,
                                     pGetKeyFn, pvGetKeyArgument, phCredential, ptsExpiry);
}

__declspec(dllexport) SECURITY_STATUS FreeCredentialsHandle(CredHandle* phCredential) {
    if (!phCredential) return SEC_E_INVALID_HANDLE;
    ULL low = phCredential->dwLower;
    if ((low & ~0xFFULL) == CRED_HANDLE_SENTINEL_LOW) {
        int slot = (int)(low & 0xFFULL);
        if (slot >= 0 && slot < MAX_CRED) g_creds[slot].used = FALSE;
    }
    phCredential->dwLower = 0;
    phCredential->dwUpper = 0;
    return SEC_E_OK;
}

// ============================================================================
//  InitializeSecurityContextA — cliente: gera msg1 (handshake step 1).
// ============================================================================

__declspec(dllexport) SECURITY_STATUS InitializeSecurityContextA(
        CredHandle* phCredential, CtxtHandle* phContext, LPSTR pszTargetName,
        ULONG fContextReq, ULONG Reserved1, ULONG TargetDataRep,
        PSecBufferDesc pInput, ULONG Reserved2, CtxtHandle* phNewContext,
        PSecBufferDesc pOutput, ULONG* pfContextAttr, PTimeStamp ptsExpiry) {
    (void)pszTargetName; (void)Reserved1; (void)TargetDataRep;
    (void)pInput; (void)Reserved2; (void)ptsExpiry;
    if (!phCredential || !phNewContext) return SEC_E_INVALID_HANDLE;

    // Se context novo, aloca slot.
    int ctx_slot;
    if (phContext == NULL || phContext->dwLower == 0) {
        ctx_slot = alloc_ctx();
        if (ctx_slot < 0) return SEC_E_INSUFFICIENT_MEMORY;
        g_ctxs[ctx_slot].used = TRUE;
        g_ctxs[ctx_slot].cred_handle_lower = phCredential->dwLower;
        g_ctxs[ctx_slot].handshake_step = 0;
        g_ctxs[ctx_slot].context_flags = fContextReq;
    } else {
        ctx_slot = (int)(phContext->dwLower & 0xFFULL);
        if (ctx_slot < 0 || ctx_slot >= MAX_CTX || !g_ctxs[ctx_slot].used)
            return SEC_E_INVALID_HANDLE;
    }

    // Avanca step. Se for o primeiro, escreve um token fake em pOutput.
    g_ctxs[ctx_slot].handshake_step++;

    if (pOutput && pOutput->cBuffers > 0 && pOutput->pBuffers) {
        // Escreve um "token" de 16 bytes fake: ASCII "MEUOS_SSPI_TOKEN" trunc.
        const char tok[16] = { 'M','E','U','O','S','_','S','S','P','I','_','T','O','K','E','N' };
        SecBuffer* b = &pOutput->pBuffers[0];
        if (b->pvBuffer && b->cbBuffer >= 16) {
            for (int k = 0; k < 16; k++) ((char*)b->pvBuffer)[k] = tok[k];
            b->cbBuffer = 16;
            b->BufferType = SECBUFFER_TOKEN;
        }
    }

    phNewContext->dwLower = CTX_HANDLE_SENTINEL_LOW | (ULL)ctx_slot;
    phNewContext->dwUpper = phCredential->dwUpper;  // mesmo pkg da credencial
    if (pfContextAttr) *pfContextAttr = fContextReq;

    // Stub: completamos em 1 passo (apesar do nome _CONTINUE_NEEDED no real).
    return SEC_E_OK;
}

__declspec(dllexport) SECURITY_STATUS InitializeSecurityContextW(
        CredHandle* phCredential, CtxtHandle* phContext, LPWSTR pszTargetName,
        ULONG fContextReq, ULONG Reserved1, ULONG TargetDataRep,
        PSecBufferDesc pInput, ULONG Reserved2, CtxtHandle* phNewContext,
        PSecBufferDesc pOutput, ULONG* pfContextAttr, PTimeStamp ptsExpiry) {
    (void)pszTargetName;
    return InitializeSecurityContextA(phCredential, phContext, NULL, fContextReq,
                                       Reserved1, TargetDataRep, pInput, Reserved2,
                                       phNewContext, pOutput, pfContextAttr, ptsExpiry);
}

// ============================================================================
//  AcceptSecurityContext — servidor: consome msg1, gera msg2.
// ============================================================================

__declspec(dllexport) SECURITY_STATUS AcceptSecurityContext(
        CredHandle* phCredential, CtxtHandle* phContext, PSecBufferDesc pInput,
        ULONG fContextReq, ULONG TargetDataRep, CtxtHandle* phNewContext,
        PSecBufferDesc pOutput, ULONG* pfContextAttr, PTimeStamp ptsExpiry) {
    (void)pInput; (void)TargetDataRep; (void)ptsExpiry;
    if (!phCredential || !phNewContext) return SEC_E_INVALID_HANDLE;

    int ctx_slot;
    if (phContext == NULL || phContext->dwLower == 0) {
        ctx_slot = alloc_ctx();
        if (ctx_slot < 0) return SEC_E_INSUFFICIENT_MEMORY;
        g_ctxs[ctx_slot].used = TRUE;
        g_ctxs[ctx_slot].cred_handle_lower = phCredential->dwLower;
        g_ctxs[ctx_slot].handshake_step = 0;
        g_ctxs[ctx_slot].context_flags = fContextReq;
    } else {
        ctx_slot = (int)(phContext->dwLower & 0xFFULL);
    }
    g_ctxs[ctx_slot].handshake_step++;

    if (pOutput && pOutput->cBuffers > 0 && pOutput->pBuffers) {
        const char ack[8] = { 'M','E','U','O','S','A','C','K' };
        SecBuffer* b = &pOutput->pBuffers[0];
        if (b->pvBuffer && b->cbBuffer >= 8) {
            for (int k = 0; k < 8; k++) ((char*)b->pvBuffer)[k] = ack[k];
            b->cbBuffer = 8;
            b->BufferType = SECBUFFER_TOKEN;
        }
    }

    phNewContext->dwLower = CTX_HANDLE_SENTINEL_LOW | (ULL)ctx_slot;
    phNewContext->dwUpper = phCredential->dwUpper;
    if (pfContextAttr) *pfContextAttr = fContextReq;
    return SEC_E_OK;
}

__declspec(dllexport) SECURITY_STATUS DeleteSecurityContext(CtxtHandle* phContext) {
    if (!phContext) return SEC_E_INVALID_HANDLE;
    ULL low = phContext->dwLower;
    if ((low & ~0xFFULL) == CTX_HANDLE_SENTINEL_LOW) {
        int slot = (int)(low & 0xFFULL);
        if (slot >= 0 && slot < MAX_CTX) g_ctxs[slot].used = FALSE;
    }
    phContext->dwLower = 0;
    phContext->dwUpper = 0;
    return SEC_E_OK;
}

// ============================================================================
//  QueryContextAttributes.
// ============================================================================

__declspec(dllexport) SECURITY_STATUS QueryContextAttributesA(CtxtHandle* phContext,
                                                              ULONG ulAttribute, void* pBuffer) {
    (void)phContext;
    if (!pBuffer) return SEC_E_INVALID_HANDLE;
    if (ulAttribute == SECPKG_ATTR_SIZES) {
        PSecPkgContext_Sizes s = (PSecPkgContext_Sizes)pBuffer;
        s->cbMaxToken = 12000;
        s->cbMaxSignature = 16;
        s->cbBlockSize = 1;
        s->cbSecurityTrailer = 16;
        return SEC_E_OK;
    }
    return SEC_E_UNSUPPORTED_FUNCTION;
}

__declspec(dllexport) SECURITY_STATUS QueryContextAttributesW(CtxtHandle* phContext,
                                                              ULONG ulAttribute, void* pBuffer) {
    return QueryContextAttributesA(phContext, ulAttribute, pBuffer);
}

// ============================================================================
//  Encrypt/Decrypt/Sign/Verify Message — sao no-ops em copy mode.
// ============================================================================

__declspec(dllexport) SECURITY_STATUS EncryptMessage(CtxtHandle* phContext, ULONG fQOP,
                                                     PSecBufferDesc pMessage, ULONG MessageSeqNo) {
    (void)phContext; (void)fQOP; (void)pMessage; (void)MessageSeqNo;
    return SEC_E_OK;   // stub: nao criptografa (deixa os bytes como estao)
}

__declspec(dllexport) SECURITY_STATUS DecryptMessage(CtxtHandle* phContext,
                                                     PSecBufferDesc pMessage, ULONG MessageSeqNo,
                                                     ULONG* pfQOP) {
    (void)phContext; (void)pMessage; (void)MessageSeqNo;
    if (pfQOP) *pfQOP = 0;
    return SEC_E_OK;   // stub: nao decifra
}

__declspec(dllexport) SECURITY_STATUS MakeSignature(CtxtHandle* phContext, ULONG fQOP,
                                                    PSecBufferDesc pMessage, ULONG MessageSeqNo) {
    (void)phContext; (void)fQOP; (void)pMessage; (void)MessageSeqNo;
    return SEC_E_OK;
}

__declspec(dllexport) SECURITY_STATUS VerifySignature(CtxtHandle* phContext,
                                                      PSecBufferDesc pMessage, ULONG MessageSeqNo,
                                                      ULONG* pfQOP) {
    (void)phContext; (void)pMessage; (void)MessageSeqNo;
    if (pfQOP) *pfQOP = 0;
    return SEC_E_OK;
}

__declspec(dllexport) SECURITY_STATUS CompleteAuthToken(CtxtHandle* phContext,
                                                        PSecBufferDesc pToken) {
    (void)phContext; (void)pToken;
    return SEC_E_OK;
}

__declspec(dllexport) SECURITY_STATUS ImpersonateSecurityContext(CtxtHandle* phContext) {
    (void)phContext;
    return SEC_E_OK;   // sem usuario real — sempre "OK" (sem mudar token)
}

__declspec(dllexport) SECURITY_STATUS RevertSecurityContext(CtxtHandle* phContext) {
    (void)phContext;
    return SEC_E_OK;
}

__declspec(dllexport) SECURITY_STATUS ApplyControlToken(CtxtHandle* phContext,
                                                        PSecBufferDesc pInput) {
    (void)phContext; (void)pInput;
    return SEC_E_OK;
}

// ============================================================================
//  LSA Logon API — caminho que apps usam para falar com o LSA via SSPI.
// ============================================================================

typedef LONG NTSTATUS_t;

__declspec(dllexport) NTSTATUS_t LsaConnectUntrusted(HANDLE* LsaHandle) {
    if (!LsaHandle) return (NTSTATUS_t)0xC000000DL;  // STATUS_INVALID_PARAMETER
    *LsaHandle = LSA_HANDLE_SENTINEL;
    return 0;  // STATUS_SUCCESS
}

__declspec(dllexport) NTSTATUS_t LsaRegisterLogonProcess(void* LogonProcessName,
                                                         HANDLE* LsaHandle,
                                                         ULONG* SecurityMode) {
    (void)LogonProcessName;
    if (!LsaHandle) return (NTSTATUS_t)0xC000000DL;
    *LsaHandle = LSA_HANDLE_SENTINEL;
    if (SecurityMode) *SecurityMode = 0;
    return 0;
}

__declspec(dllexport) NTSTATUS_t LsaLookupAuthenticationPackage(HANDLE LsaHandle,
                                                                 void* PackageName,
                                                                 ULONG* AuthenticationPackage) {
    (void)LsaHandle;
    // PackageName e LSA_STRING (uchar Length, uchar MaxLength, char* Buffer)
    if (!PackageName || !AuthenticationPackage) return (NTSTATUS_t)0xC000000DL;
    // Lemos o nome (offset 4: char* Buffer; offset 0: USHORT Length)
    typedef struct { unsigned short Length; unsigned short MaximumLength; char* Buffer; } LSA_STRING_t;
    LSA_STRING_t* s = (LSA_STRING_t*)PackageName;
    int id = -1;
    if (s->Buffer && s->Length > 0) {
        // Cria copia null-terminated.
        char tmp[32]; int n = s->Length < 31 ? s->Length : 31;
        for (int i = 0; i < n; i++) tmp[i] = s->Buffer[i];
        tmp[n] = 0;
        id = pkg_lookup(tmp);
    }
    if (id < 0) return (NTSTATUS_t)0xC00000FEL;   // STATUS_NO_SUCH_PACKAGE
    *AuthenticationPackage = (ULONG)id;
    return 0;
}

__declspec(dllexport) NTSTATUS_t LsaCallAuthenticationPackage(HANDLE LsaHandle,
                                                               ULONG AuthenticationPackage,
                                                               void* ProtocolSubmitBuffer,
                                                               ULONG SubmitBufferLength,
                                                               void** ProtocolReturnBuffer,
                                                               ULONG* ReturnBufferLength,
                                                               NTSTATUS_t* ProtocolStatus) {
    (void)LsaHandle; (void)AuthenticationPackage;
    (void)ProtocolSubmitBuffer; (void)SubmitBufferLength;
    if (ProtocolReturnBuffer) *ProtocolReturnBuffer = NULL;
    if (ReturnBufferLength)   *ReturnBufferLength = 0;
    if (ProtocolStatus)       *ProtocolStatus = 0;
    return 0;
}

__declspec(dllexport) NTSTATUS_t LsaDeregisterLogonProcess(HANDLE LsaHandle) {
    (void)LsaHandle;
    return 0;
}

__declspec(dllexport) NTSTATUS_t LsaFreeReturnBuffer(void* Buffer) {
    (void)Buffer;  // pool estatico
    return 0;
}

__declspec(dllexport) NTSTATUS_t LsaLogonUser(HANDLE LsaHandle, void* OriginName,
                                                 ULONG LogonType, ULONG AuthenticationPackage,
                                                 void* AuthenticationInformation,
                                                 ULONG AuthenticationInformationLength,
                                                 void* LocalGroups, void* SourceContext,
                                                 void** ProfileBuffer, ULONG* ProfileBufferLength,
                                                 void* LogonId, HANDLE* Token,
                                                 void* Quotas, NTSTATUS_t* SubStatus) {
    (void)LsaHandle; (void)OriginName; (void)LogonType; (void)AuthenticationPackage;
    (void)AuthenticationInformation; (void)AuthenticationInformationLength;
    (void)LocalGroups; (void)SourceContext; (void)LogonId; (void)Quotas;
    if (ProfileBuffer) *ProfileBuffer = NULL;
    if (ProfileBufferLength) *ProfileBufferLength = 0;
    if (Token) *Token = (HANDLE)(ULL)0xA17ULL;  // pseudo-token "auth"
    if (SubStatus) *SubStatus = 0;
    return 0;  // STATUS_SUCCESS (sem auth real, mas o app considera logado)
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved;
    return 1;
}
