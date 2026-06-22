// ============================================================================
//  ws2_32.dll — Winsock 2.2 (FASE 12 do MeuOS).
//
//  Reimplementacao minima da API Winsock 2.2 do Windows. ws2_32 e a DLL ring 3
//  que apps de rede usam (curl/winhttp/IE/...). Aqui no MeuOS nao temos stack
//  IP real (rede vazia): socket() devolve um handle fake, send() aceita os
//  bytes sem transmitir, recv() devolve 0 (peer fechou). Estado em pool
//  estatico de 32 sockets.
//
//  Comportamento ABI: zig cc -target x86_64-windows-gnu gera mingw __cdecl;
//  os exports tem __stdcall fake porque so retornam (sem callbacks). Sem
//  dependencia ABI cruzada — funcao isolada.
//
//  IMAGE BASE: 0x4E00000 — zona livre apos winmm (0x4D00000), com .reloc via
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
typedef short              SHORT;
typedef unsigned short     USHORT;
typedef unsigned short     WORD;
typedef long               LONG;
typedef unsigned long      ULONG;
typedef unsigned char      BYTE;
typedef unsigned long long ULL;
typedef void*              HANDLE;
typedef void*              LPVOID;
typedef const void*        LPCVOID;
typedef char*              LPSTR;
typedef const char*        LPCSTR;
typedef void*              HWND;

// SOCKET handle: pelo NT/Windows, e um UINT_PTR (64-bit). Aqui mascaramos
// como ULL.
typedef ULL SOCKET;

#define INVALID_SOCKET     ((SOCKET)(~0))
#define SOCKET_ERROR       (-1)

// Familias.
#define AF_UNSPEC          0
#define AF_INET            2
#define AF_INET6           23

// Tipos de socket.
#define SOCK_STREAM        1
#define SOCK_DGRAM         2
#define SOCK_RAW           3

// Protocolos.
#define IPPROTO_TCP        6
#define IPPROTO_UDP        17
#define IPPROTO_IP         0
#define IPPROTO_IPV6       41
#define IPPROTO_ICMP       1

// Erros Winsock (subset).
#define WSAEINVAL          10022
#define WSAEFAULT          10014
#define WSAEMFILE          10024
#define WSAEWOULDBLOCK     10035
#define WSAENOTSOCK        10038
#define WSAEINTR           10004
#define WSAEACCES          10013
#define WSAEADDRINUSE      10048
#define WSAECONNRESET      10054
#define WSAECONNREFUSED    10061
#define WSAEHOSTUNREACH    10065
#define WSAEHOSTDOWN       10064
#define WSAENETDOWN        10050

// FIONBIO (set/get nonblocking).
#define FIONBIO            0x8004667E
#define FIONREAD           0x4004667F
#define SIOCATMARK         0x40047307

// shutdown how.
#define SD_RECEIVE         0
#define SD_SEND            1
#define SD_BOTH            2

// ============================================================================
//  WSADATA — struct devolvida por WSAStartup.
// ============================================================================
#define WSADESCRIPTION_LEN 256
#define WSASYS_STATUS_LEN  128

typedef struct WSAData {
    WORD wVersion;
    WORD wHighVersion;
    char szDescription[WSADESCRIPTION_LEN + 1];
    char szSystemStatus[WSASYS_STATUS_LEN + 1];
    USHORT iMaxSockets;
    USHORT iMaxUdpDg;
    char* lpVendorInfo;
} WSADATA, *LPWSADATA;

// ============================================================================
//  Estruturas de endereco.
// ============================================================================
struct in_addr {
    union {
        struct { BYTE s_b1, s_b2, s_b3, s_b4; } S_un_b;
        struct { USHORT s_w1, s_w2; } S_un_w;
        ULONG  S_addr;
    } S_un;
#define s_addr S_un.S_addr
};

struct sockaddr_in {
    SHORT  sin_family;
    USHORT sin_port;
    struct in_addr sin_addr;
    char   sin_zero[8];
};

struct sockaddr {
    USHORT sa_family;
    char   sa_data[14];
};

struct hostent {
    char*  h_name;
    char** h_aliases;
    SHORT  h_addrtype;
    SHORT  h_length;
    char** h_addr_list;
};

// Linger struct (para setsockopt SO_LINGER).
struct linger {
    USHORT l_onoff;
    USHORT l_linger;
};

// fd_set (select).
#define FD_SETSIZE 64
typedef struct fd_set {
    UINT   fd_count;
    SOCKET fd_array[FD_SETSIZE];
} fd_set;

struct timeval {
    LONG tv_sec;
    LONG tv_usec;
};

// ============================================================================
//  Estado interno — pool estatico de 32 SOCKETs.
// ============================================================================
#define MAX_SOCKETS 32

typedef struct socket_state {
    int      used;
    int      family;
    int      type;
    int      protocol;
    int      bound;
    int      listening;
    int      connected;
    int      shutdown_flags;       // SD_*
    int      nonblocking;
    USHORT   local_port;
    USHORT   remote_port;
    ULONG    local_ip;
    ULONG    remote_ip;
} socket_state_t;

static socket_state_t g_sockets[MAX_SOCKETS];
static int g_wsa_started = 0;
static int g_wsa_last_error = 0;

static void zero_mem(void* p, unsigned n) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned i = 0; i < n; i++) b[i] = 0;
}

static int alloc_socket_slot(void) {
    for (int i = 1; i < MAX_SOCKETS; i++) {     // i=0 reservado (=INVALID)
        if (!g_sockets[i].used) {
            zero_mem(&g_sockets[i], sizeof(g_sockets[i]));
            g_sockets[i].used = 1;
            return i;
        }
    }
    return -1;
}

static socket_state_t* sock_to_state(SOCKET s) {
    if (s == INVALID_SOCKET) return 0;
    ULL v = (ULL)s;
    // Handle encodado: bits altos = 0x5AC0_0000, baixos = indice (1..MAX_SOCKETS-1).
    if ((v & 0xFFFFFFFFULL) >> 16 != 0x5AC0) return 0;
    int idx = (int)(v & 0xFFFF);
    if (idx <= 0 || idx >= MAX_SOCKETS) return 0;
    if (!g_sockets[idx].used) return 0;
    return &g_sockets[idx];
}

static SOCKET slot_to_socket(int idx) {
    return (SOCKET)((ULL)0x5AC00000ULL | (ULL)(idx & 0xFFFF));
}

// ============================================================================
//  WSAStartup / WSACleanup.
// ============================================================================
__declspec(dllexport) int __stdcall WSAStartup(WORD version, LPWSADATA data) {
    if (!data) return WSAEFAULT;
    zero_mem(data, sizeof(*data));
    data->wVersion     = version;
    data->wHighVersion = 0x0202;   // 2.2
    const char desc[] = "MeuOS Winsock 2.2 stub";
    int i;
    for (i = 0; desc[i] && i < WSADESCRIPTION_LEN; i++) data->szDescription[i] = desc[i];
    data->szDescription[i] = 0;
    const char st[] = "Running (no real network)";
    for (i = 0; st[i] && i < WSASYS_STATUS_LEN; i++) data->szSystemStatus[i] = st[i];
    data->szSystemStatus[i] = 0;
    data->iMaxSockets = MAX_SOCKETS - 1;
    data->iMaxUdpDg   = 65507;
    data->lpVendorInfo = 0;
    g_wsa_started = 1;
    return 0;   // sucesso
}

__declspec(dllexport) int __stdcall WSACleanup(void) {
    g_wsa_started = 0;
    for (int i = 1; i < MAX_SOCKETS; i++) g_sockets[i].used = 0;
    return 0;
}

__declspec(dllexport) int __stdcall WSAGetLastError(void) {
    return g_wsa_last_error;
}

__declspec(dllexport) void __stdcall WSASetLastError(int err) {
    g_wsa_last_error = err;
}

// ============================================================================
//  socket / closesocket / shutdown.
// ============================================================================
__declspec(dllexport) SOCKET __stdcall socket(int af, int type, int proto) {
    if (!g_wsa_started) { g_wsa_last_error = WSAENETDOWN; return INVALID_SOCKET; }
    int idx = alloc_socket_slot();
    if (idx < 0) { g_wsa_last_error = WSAEMFILE; return INVALID_SOCKET; }
    g_sockets[idx].family   = af;
    g_sockets[idx].type     = type;
    g_sockets[idx].protocol = proto;
    return slot_to_socket(idx);
}

__declspec(dllexport) SOCKET __stdcall WSASocketA(int af, int type, int proto,
        void* lpProtocolInfo, UINT g, DWORD dwFlags) {
    (void)lpProtocolInfo; (void)g; (void)dwFlags;
    return socket(af, type, proto);
}

__declspec(dllexport) SOCKET __stdcall WSASocketW(int af, int type, int proto,
        void* lpProtocolInfo, UINT g, DWORD dwFlags) {
    (void)lpProtocolInfo; (void)g; (void)dwFlags;
    return socket(af, type, proto);
}

__declspec(dllexport) int __stdcall closesocket(SOCKET s) {
    socket_state_t* st = sock_to_state(s);
    if (!st) { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    st->used = 0;
    return 0;
}

__declspec(dllexport) int __stdcall shutdown(SOCKET s, int how) {
    socket_state_t* st = sock_to_state(s);
    if (!st) { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    st->shutdown_flags |= (1 << how);
    return 0;
}

// ============================================================================
//  bind / listen / accept / connect.
// ============================================================================
__declspec(dllexport) int __stdcall bind(SOCKET s, const struct sockaddr* name, int namelen) {
    socket_state_t* st = sock_to_state(s);
    if (!st)  { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    if (!name || namelen < (int)sizeof(struct sockaddr_in)) {
        g_wsa_last_error = WSAEFAULT; return SOCKET_ERROR;
    }
    const struct sockaddr_in* sin = (const struct sockaddr_in*)name;
    st->local_port = sin->sin_port;
    st->local_ip   = sin->sin_addr.s_addr;
    st->bound      = 1;
    return 0;
}

__declspec(dllexport) int __stdcall listen(SOCKET s, int backlog) {
    (void)backlog;
    socket_state_t* st = sock_to_state(s);
    if (!st)        { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    if (!st->bound) { g_wsa_last_error = WSAEINVAL;   return SOCKET_ERROR; }
    st->listening = 1;
    return 0;
}

__declspec(dllexport) SOCKET __stdcall accept(SOCKET s, struct sockaddr* addr, int* addrlen) {
    socket_state_t* st = sock_to_state(s);
    if (!st || !st->listening) { g_wsa_last_error = WSAENOTSOCK; return INVALID_SOCKET; }
    // Sem rede real -> WOULDBLOCK (apps geralmente fazem select primeiro).
    (void)addr; (void)addrlen;
    g_wsa_last_error = WSAEWOULDBLOCK;
    return INVALID_SOCKET;
}

__declspec(dllexport) int __stdcall connect(SOCKET s, const struct sockaddr* name, int namelen) {
    socket_state_t* st = sock_to_state(s);
    if (!st) { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    if (!name || namelen < (int)sizeof(struct sockaddr_in)) {
        g_wsa_last_error = WSAEFAULT; return SOCKET_ERROR;
    }
    const struct sockaddr_in* sin = (const struct sockaddr_in*)name;
    st->remote_port = sin->sin_port;
    st->remote_ip   = sin->sin_addr.s_addr;
    // Sem rede real: simulamos sucesso de conexao (apps que checam connect()
    // entram no laco de send/recv, recebem 0 bytes e fecham gentilmente).
    st->connected = 1;
    return 0;
}

// ============================================================================
//  send / recv / sendto / recvfrom.
// ============================================================================
__declspec(dllexport) int __stdcall send(SOCKET s, const char* buf, int len, int flags) {
    (void)buf; (void)flags;
    socket_state_t* st = sock_to_state(s);
    if (!st) { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    // "Envia" todos os bytes (rede vazia: vai pro vazio).
    return len > 0 ? len : 0;
}

__declspec(dllexport) int __stdcall recv(SOCKET s, char* buf, int len, int flags) {
    (void)buf; (void)len; (void)flags;
    socket_state_t* st = sock_to_state(s);
    if (!st) { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    // Sem dados: devolve 0 (peer fechou gentilmente).
    return 0;
}

__declspec(dllexport) int __stdcall sendto(SOCKET s, const char* buf, int len, int flags,
        const struct sockaddr* to, int tolen) {
    (void)buf; (void)flags; (void)to; (void)tolen;
    socket_state_t* st = sock_to_state(s);
    if (!st) { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    return len > 0 ? len : 0;
}

__declspec(dllexport) int __stdcall recvfrom(SOCKET s, char* buf, int len, int flags,
        struct sockaddr* from, int* fromlen) {
    (void)buf; (void)len; (void)flags; (void)from; (void)fromlen;
    socket_state_t* st = sock_to_state(s);
    if (!st) { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    return 0;
}

// ============================================================================
//  select.
// ============================================================================
__declspec(dllexport) int __stdcall select(int nfds, fd_set* rd, fd_set* wr, fd_set* ex,
        const struct timeval* tv) {
    (void)nfds; (void)rd; (void)wr; (void)ex; (void)tv;
    // Rede vazia: nenhum descritor pronto -> timeout.
    if (rd) rd->fd_count = 0;
    if (wr) wr->fd_count = 0;
    if (ex) ex->fd_count = 0;
    return 0;
}

// ============================================================================
//  getsockname / getpeername.
// ============================================================================
__declspec(dllexport) int __stdcall getsockname(SOCKET s, struct sockaddr* name, int* namelen) {
    socket_state_t* st = sock_to_state(s);
    if (!st)               { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    if (!name || !namelen) { g_wsa_last_error = WSAEFAULT;   return SOCKET_ERROR; }
    if (*namelen < (int)sizeof(struct sockaddr_in)) {
        g_wsa_last_error = WSAEFAULT; return SOCKET_ERROR;
    }
    struct sockaddr_in* sin = (struct sockaddr_in*)name;
    zero_mem(sin, sizeof(*sin));
    sin->sin_family       = AF_INET;
    sin->sin_port         = st->local_port;
    sin->sin_addr.s_addr  = st->local_ip;
    *namelen = sizeof(struct sockaddr_in);
    return 0;
}

__declspec(dllexport) int __stdcall getpeername(SOCKET s, struct sockaddr* name, int* namelen) {
    socket_state_t* st = sock_to_state(s);
    if (!st)               { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    if (!name || !namelen) { g_wsa_last_error = WSAEFAULT;   return SOCKET_ERROR; }
    struct sockaddr_in* sin = (struct sockaddr_in*)name;
    zero_mem(sin, sizeof(*sin));
    sin->sin_family       = AF_INET;
    sin->sin_port         = st->remote_port;
    sin->sin_addr.s_addr  = st->remote_ip;
    *namelen = sizeof(struct sockaddr_in);
    return 0;
}

// ============================================================================
//  getsockopt / setsockopt / ioctlsocket.
// ============================================================================
__declspec(dllexport) int __stdcall getsockopt(SOCKET s, int level, int optname,
        char* optval, int* optlen) {
    (void)level; (void)optname; (void)optval; (void)optlen;
    socket_state_t* st = sock_to_state(s);
    if (!st) { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    // Sempre devolve sucesso com optval inalterado.
    return 0;
}

__declspec(dllexport) int __stdcall setsockopt(SOCKET s, int level, int optname,
        const char* optval, int optlen) {
    (void)level; (void)optname; (void)optval; (void)optlen;
    socket_state_t* st = sock_to_state(s);
    if (!st) { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    return 0;
}

__declspec(dllexport) int __stdcall ioctlsocket(SOCKET s, LONG cmd, ULONG* argp) {
    socket_state_t* st = sock_to_state(s);
    if (!st) { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    if (cmd == FIONBIO) {
        if (argp) st->nonblocking = (*argp != 0);
        return 0;
    }
    if (cmd == FIONREAD) {
        if (argp) *argp = 0;   // sem bytes em recv
        return 0;
    }
    return 0;
}

// ============================================================================
//  Conversoes / DNS.
// ============================================================================
__declspec(dllexport) USHORT __stdcall htons(USHORT host) {
    return (USHORT)(((host & 0xFF) << 8) | ((host & 0xFF00) >> 8));
}

__declspec(dllexport) USHORT __stdcall ntohs(USHORT net) {
    return htons(net);   // simetrico
}

__declspec(dllexport) ULONG __stdcall htonl(ULONG host) {
    return ((host & 0x000000FFu) << 24) |
           ((host & 0x0000FF00u) << 8)  |
           ((host & 0x00FF0000u) >> 8)  |
           ((host & 0xFF000000u) >> 24);
}

__declspec(dllexport) ULONG __stdcall ntohl(ULONG net) {
    return htonl(net);
}

// inet_addr: converte "a.b.c.d" para ULONG (BE). 0xFFFFFFFF em erro.
__declspec(dllexport) ULONG __stdcall inet_addr(const char* cp) {
    if (!cp) return 0xFFFFFFFFu;
    ULONG parts[4] = {0,0,0,0};
    int part = 0;
    const char* p = cp;
    while (*p && part < 4) {
        if (*p < '0' || *p > '9') {
            if (*p == '.') { part++; p++; continue; }
            return 0xFFFFFFFFu;
        }
        ULONG v = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (ULONG)(*p - '0');
            if (v > 255) return 0xFFFFFFFFu;
            p++;
        }
        parts[part] = v;
        if (*p == '.') { part++; p++; }
        else break;
    }
    if (part != 3) return 0xFFFFFFFFu;
    // Network byte order (big endian): a.b.c.d -> a em LSB do BE...
    return  (parts[0]      ) |
            (parts[1] <<  8) |
            (parts[2] << 16) |
            (parts[3] << 24);
}

// inet_ntoa: devolve string fixa em buffer estatico (apps reais nao reentrante).
static char g_inet_ntoa_buf[32];
static void utoa10(ULONG v, char* dst, int* idx) {
    char tmp[12]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int j = n - 1; j >= 0; j--) dst[(*idx)++] = tmp[j];
}

__declspec(dllexport) char* __stdcall inet_ntoa(struct in_addr addr) {
    int idx = 0;
    ULONG v = addr.S_un.S_addr;
    utoa10((v      ) & 0xFF, g_inet_ntoa_buf, &idx); g_inet_ntoa_buf[idx++] = '.';
    utoa10((v >>  8) & 0xFF, g_inet_ntoa_buf, &idx); g_inet_ntoa_buf[idx++] = '.';
    utoa10((v >> 16) & 0xFF, g_inet_ntoa_buf, &idx); g_inet_ntoa_buf[idx++] = '.';
    utoa10((v >> 24) & 0xFF, g_inet_ntoa_buf, &idx);
    g_inet_ntoa_buf[idx] = 0;
    return g_inet_ntoa_buf;
}

// gethostbyname: devolve sempre 127.0.0.1 (caminho seguro). Estrutura estatica.
static char g_host_name_buf[64];
static char g_host_addr[4] = { 127, 0, 0, 1 };
static char* g_host_addr_list[2] = { g_host_addr, 0 };
static char* g_host_aliases[1]   = { 0 };
static struct hostent g_hostent;

__declspec(dllexport) struct hostent* __stdcall gethostbyname(const char* name) {
    int n = 0;
    if (name) {
        while (name[n] && n < 63) { g_host_name_buf[n] = name[n]; n++; }
    }
    g_host_name_buf[n] = 0;
    g_hostent.h_name = g_host_name_buf;
    g_hostent.h_aliases = g_host_aliases;
    g_hostent.h_addrtype = AF_INET;
    g_hostent.h_length = 4;
    g_hostent.h_addr_list = g_host_addr_list;
    return &g_hostent;
}

__declspec(dllexport) struct hostent* __stdcall gethostbyaddr(const char* addr, int len, int type) {
    (void)addr; (void)len; (void)type;
    return gethostbyname("localhost");
}

__declspec(dllexport) int __stdcall gethostname(char* name, int namelen) {
    if (!name || namelen < 1) { g_wsa_last_error = WSAEFAULT; return SOCKET_ERROR; }
    const char nm[] = "MeuOS";
    int i;
    for (i = 0; nm[i] && i < namelen - 1; i++) name[i] = nm[i];
    name[i] = 0;
    return 0;
}

// getaddrinfo simples (Win Vista+): para nao quebrar apps modernos.
typedef struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    ULL ai_addrlen;
    char* ai_canonname;
    struct sockaddr* ai_addr;
    struct addrinfo* ai_next;
} ADDRINFOA;

static struct sockaddr_in g_ai_sa;
static ADDRINFOA g_ai_result;

__declspec(dllexport) int __stdcall getaddrinfo(const char* node, const char* service,
        const ADDRINFOA* hints, ADDRINFOA** res) {
    (void)hints;
    if (!res) return WSAEFAULT;
    zero_mem(&g_ai_sa, sizeof(g_ai_sa));
    g_ai_sa.sin_family = AF_INET;
    g_ai_sa.sin_addr.s_addr = 0x0100007F;   // 127.0.0.1 em NBO (le-se 7F.00.00.01)
    int port = 0;
    if (service) {
        // Aceita "80", "443", numero ascii.
        for (int i = 0; service[i]; i++) {
            if (service[i] >= '0' && service[i] <= '9') port = port * 10 + (service[i] - '0');
            else { port = 0; break; }
        }
    }
    g_ai_sa.sin_port = htons((USHORT)port);
    (void)node;
    zero_mem(&g_ai_result, sizeof(g_ai_result));
    g_ai_result.ai_family   = AF_INET;
    g_ai_result.ai_socktype = SOCK_STREAM;
    g_ai_result.ai_protocol = IPPROTO_TCP;
    g_ai_result.ai_addrlen  = sizeof(g_ai_sa);
    g_ai_result.ai_addr     = (struct sockaddr*)&g_ai_sa;
    g_ai_result.ai_next     = 0;
    *res = &g_ai_result;
    return 0;
}

__declspec(dllexport) void __stdcall freeaddrinfo(ADDRINFOA* info) {
    (void)info;   // estatico — no-op
}

// ============================================================================
//  WSAFD* helpers (FD_SET/FD_CLR/FD_ISSET sao macros no header, mas exportamos
//  como wrappers usaveis por apps que linkam dinamicamente).
// ============================================================================
__declspec(dllexport) int __stdcall __WSAFDIsSet(SOCKET s, fd_set* set) {
    if (!set) return 0;
    for (UINT i = 0; i < set->fd_count; i++) {
        if (set->fd_array[i] == s) return 1;
    }
    return 0;
}

// ============================================================================
//  WSARecv / WSASend (overlapped — devolvem sucesso "imediato").
// ============================================================================
typedef struct WSABUF {
    ULONG  len;
    char*  buf;
} WSABUF, *LPWSABUF;

__declspec(dllexport) int __stdcall WSARecv(SOCKET s, LPWSABUF buffers, DWORD bufcount,
        DWORD* received, DWORD* flags, void* ov, void* completion) {
    (void)buffers; (void)bufcount; (void)flags; (void)ov; (void)completion;
    socket_state_t* st = sock_to_state(s);
    if (!st) { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    if (received) *received = 0;
    return 0;
}

__declspec(dllexport) int __stdcall WSASend(SOCKET s, LPWSABUF buffers, DWORD bufcount,
        DWORD* sent, DWORD flags, void* ov, void* completion) {
    (void)flags; (void)ov; (void)completion;
    socket_state_t* st = sock_to_state(s);
    if (!st) { g_wsa_last_error = WSAENOTSOCK; return SOCKET_ERROR; }
    DWORD total = 0;
    if (buffers) for (DWORD i = 0; i < bufcount; i++) total += buffers[i].len;
    if (sent) *sent = total;
    return 0;
}

__declspec(dllexport) int __stdcall WSAEventSelect(SOCKET s, HANDLE ev, LONG events) {
    (void)s; (void)ev; (void)events;
    return 0;
}

__declspec(dllexport) HANDLE __stdcall WSACreateEvent(void) {
    return (HANDLE)1;
}

__declspec(dllexport) BOOL __stdcall WSACloseEvent(HANDLE ev) {
    (void)ev; return 1;
}

__declspec(dllexport) BOOL __stdcall WSAResetEvent(HANDLE ev) {
    (void)ev; return 1;
}

__declspec(dllexport) BOOL __stdcall WSASetEvent(HANDLE ev) {
    (void)ev; return 1;
}

__declspec(dllexport) DWORD __stdcall WSAWaitForMultipleEvents(DWORD ne, const HANDLE* evs,
        BOOL waitAll, DWORD timeout, BOOL alertable) {
    (void)ne; (void)evs; (void)waitAll; (void)timeout; (void)alertable;
    return 0;   // WSA_WAIT_EVENT_0
}

__declspec(dllexport) int __stdcall WSAAsyncSelect(SOCKET s, HWND hwnd, UINT msg, LONG events) {
    (void)s; (void)hwnd; (void)msg; (void)events;
    return 0;
}

// ============================================================================
//  DllMain.
// ============================================================================
int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reserved;
    if (reason == 1) {   // DLL_PROCESS_ATTACH
        for (int i = 0; i < MAX_SOCKETS; i++) g_sockets[i].used = 0;
        g_wsa_started = 0;
        g_wsa_last_error = 0;
    }
    return 1;
}
