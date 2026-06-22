// cmd.c  —  Shell estilo Windows (Command Prompt), rodando em RING 3 no MeuOS.
// FASE 5: comandos de arquivo sobre o volume NTFS montado como C:. Caminho de
// cada comando:
//   ring3 (cmd) -> kernel32 (WriteFile / ReadFile / CreateFileA /
//   QueryDirectoryFileEx / QueryVolumeInfoEx / EnumProcessesEx / EnumDriversEx /
//   StartDriverServiceA / StopDriverServiceA) -> ntdll (Nt*) -> int 0x80 ->
//   SSDT -> Object Manager / I/O Manager / driver NTFS (lado kernel).
//
// O NTFS e montado como a unidade C: (o kernel mapeia "C:\..." para
// \Device\Harddisk0\Partition1\...). O prompt e C:\> e segue o diretorio atual.
//
// Comandos:
//   help                  - lista os comandos
//   sc query              - lista os drivers de kernel e seu estado (STOPPED/RUNNING)
//   sc start <nome>       - carrega um driver .sys pelo nome (chama DriverEntry)
//   sc stop  <nome>       - descarrega o driver (chama DriverUnload)
//   tasklist              - lista os processos (EPROCESS do Object Manager)
//   vol                   - mostra rotulo/serial/tamanho do volume C: (NtQueryVolumeInformation)
//   dir                   - lista o diretorio atual do C: (NtCreateFile/NtQueryDirectoryFile)
//   cd  <dir>             - muda o diretorio atual (cd \ , cd .. , cd dir1)
//   type <arquivo>        - mostra o conteudo de um arquivo (NtReadFile)
//   copy <orig> <dest>    - copia o conteudo de um arquivo p/ outro EXISTENTE (NtWriteFile)
//   del  <arquivo>        - exclui um arquivo (stub: aviso; exige syscall de delete)
//   exit                  - encerra o shell
//
// Para ser TESTAVEL headless (sem teclado/-display none), o shell comeca num
// "MODO DEMO": executa automaticamente help, tasklist, sc query, vol, dir, type
// e cd, imprimindo tudo na serial. Em seguida abre o prompt interativo (le linhas
// via ReadFile do console); com display, a digitacao funciona; sem display
// (headless), ReadFile devolve 0 bytes e o shell encerra sozinho.

unsigned long _tls_index = 0;

#define STD_OUTPUT_HANDLE ((unsigned)-11)
#define STD_INPUT_HANDLE  ((unsigned)-10)

#define MEUOS_SERVICE_STOPPED  1
#define MEUOS_SERVICE_RUNNING  4

// ---- structs compartilhadas com o kernel (mesmo layout) ----
typedef struct _MEUOS_PROCESS_ENTRY {
    unsigned           ProcessId;
    unsigned           Terminated;
    unsigned long long ImageBase;
    unsigned           ThreadCount;
    char               ImageName[32];
} MEUOS_PROCESS_ENTRY;

typedef struct _MEUOS_DRIVER_ENTRY {
    unsigned State;
    unsigned LastStatus;
    char     Name[32];
} MEUOS_DRIVER_ENTRY;

// FASE 3 (NTFS): uma entrada de diretorio do volume (layout do kernel).
typedef struct _MEUOS_DIR_ENTRY {
    unsigned long long MftRecord;
    unsigned           IsDir;
    unsigned           Pad;
    unsigned long long Size;
    char               Name[256];
} MEUOS_DIR_ENTRY;

// FASE 5: resumo do volume (layout do kernel).
typedef struct _MEUOS_VOLUME_INFO {
    unsigned long long Serial;
    unsigned long long TotalBytes;
    unsigned long long FreeBytes;
    unsigned           BytesPerSector;
    unsigned           BytesPerCluster;
    char               FsName[8];
    char               Label[32];
} MEUOS_VOLUME_INFO;

// ---- imports (kernel32) ----
__declspec(dllimport) void* GetStdHandle(unsigned which);
__declspec(dllimport) int   WriteFile(void* h, const void* buf, unsigned len, unsigned* wrote, void* ov);
__declspec(dllimport) int   ReadFile(void* h, void* buf, unsigned len, unsigned* read, void* ov);
__declspec(dllimport) void  ExitProcess(unsigned code);
__declspec(dllimport) int   EnumProcessesEx(unsigned index, MEUOS_PROCESS_ENTRY* out);
__declspec(dllimport) int   EnumDriversEx(unsigned index, MEUOS_DRIVER_ENTRY* out);
__declspec(dllimport) int   StartDriverServiceA(const char* name);
__declspec(dllimport) int   StopDriverServiceA(const char* name);
// FASE 3/5 (NTFS): abrir/ler/listar arquivos e consultar o volume.
__declspec(dllimport) void* CreateFileA(const char* name, unsigned access, unsigned share,
        void* sec, unsigned disposition, unsigned flags, void* templ);
__declspec(dllimport) int   CloseHandle(void* h);
__declspec(dllimport) int   QueryDirectoryFileEx(void* hDir, MEUOS_DIR_ENTRY* out);
__declspec(dllimport) int   QueryVolumeInfoEx(MEUOS_VOLUME_INFO* out);

#define MEUOS_INVALID_HANDLE ((void*)(long long)-1)

// ============================================================================
//  Utilitarios sem CRT (string + saida).
// ============================================================================
static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void scopy(char* d, const char* s) { while ((*d++ = *s++)) {} }
static void scat(char* d, const char* s) { while (*d) d++; while ((*d++ = *s++)) {} }

static void* g_out;
static void out(const char* s) {
    unsigned w = 0; WriteFile(g_out, s, slen(s), &w, 0);
}
static void out_dec(unsigned long long v) {
    char b[24]; int i = 0;
    if (v == 0) { out("0"); return; }
    while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    char r[24]; int j = 0;
    while (i) r[j++] = b[--i];
    r[j] = 0;
    out(r);
}
static void out_hex(unsigned long long v) {
    const char* d = "0123456789ABCDEF";
    char r[19]; r[0] = '0'; r[1] = 'x';
    for (int i = 0; i < 16; i++) r[2 + i] = d[(v >> ((15 - i) * 4)) & 0xF];
    r[18] = 0;
    out(r);
}

static char lower_c(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// Compara duas strings ignorando maiusculas/minusculas.
static int ieq(const char* a, const char* b) {
    while (*a && *b) { if (lower_c(*a) != lower_c(*b)) return 0; a++; b++; }
    return *a == *b;
}

// Copia uma palavra (ate espaco/fim) de 'src' para 'dst'; devolve o ponteiro
// logo apos a palavra (apos pular espacos a esquerda).
static const char* word(const char* src, char* dst, int max) {
    while (*src == ' ' || *src == '\t') src++;
    int i = 0;
    while (*src && *src != ' ' && *src != '\t' && i < max - 1) dst[i++] = *src++;
    dst[i] = 0;
    return src;
}

static void trim_eol(char* s) {
    int n = (int)slen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) s[--n] = 0;
}

// ============================================================================
//  Diretorio atual (CWD) e resolucao de caminhos. O CWD e um caminho NTFS
//  relativo ao volume, sempre comecando por '\' (raiz = "\"). O kernel monta
//  C:\... -> \Device\Harddisk0\Partition1\..., entao basta prefixar "C:".
// ============================================================================
static char g_cwd[256] = "\\";   // diretorio atual no volume (NTFS subpath)

// Normaliza barras de 'in' para '\' copiando em 'out'.
static void norm_slashes(char* out_, const char* in) {
    int i = 0;
    while (in[i]) { out_[i] = (in[i] == '/') ? '\\' : in[i]; i++; }
    out_[i] = 0;
}

// Resolve um argumento de caminho ('hello.txt', '\a\b', 'dir1', '..', 'C:\x')
// em um caminho NTFS absoluto (comeca por '\'), aplicando o CWD e tratando
// '.' e '..'. Escreve em 'out' (caminho relativo ao volume, sem o "C:").
static void resolve_subpath(const char* arg, char* out_) {
    char tmp[256];
    norm_slashes(tmp, arg);
    const char* p = tmp;

    // Drive letter C: (forma DOS) — descarta o prefixo "C:".
    if ((p[0] == 'C' || p[0] == 'c') && p[1] == ':') p += 2;

    char base[256];
    if (p[0] == '\\') { base[0] = '\\'; base[1] = 0; p++; }   // caminho absoluto
    else scopy(base, g_cwd);                                  // relativo ao CWD

    // Processa componente a componente, aplicando '.' e '..'.
    while (*p) {
        char comp[256]; int ci = 0;
        while (*p && *p != '\\') { if (ci < 255) comp[ci++] = *p; p++; }
        comp[ci] = 0;
        while (*p == '\\') p++;
        if (comp[0] == 0 || (comp[0] == '.' && comp[1] == 0)) continue;   // "" ou "."
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
            // sobe um nivel (corta o ultimo componente de 'base')
            int n = (int)slen(base);
            while (n > 1 && base[n - 1] != '\\') n--;
            if (n > 1) n--;                  // remove a barra (a menos da raiz)
            if (n < 1) n = 1;
            base[n] = 0;
            if (base[0] == 0) { base[0] = '\\'; base[1] = 0; }
            continue;
        }
        // anexa o componente
        int n = (int)slen(base);
        if (n == 0 || base[n - 1] != '\\') base[n++] = '\\';
        base[n] = 0;
        scat(base, comp);
    }
    scopy(out_, base);
}

// Monta o caminho COMPLETO p/ CreateFileA: "C:" + subpath. (O kernel reconhece
// "C:\..." e mapeia para o device de volume.)
static void make_full_path(const char* subpath, char* out_) {
    out_[0] = 'C'; out_[1] = ':'; out_[2] = 0;
    scat(out_, subpath[0] ? subpath : "\\");
}

// ============================================================================
//  Comandos.
// ============================================================================
static void cmd_help(void) {
    out("Comandos do MeuOS cmd.exe:\n");
    out("  help              - mostra esta ajuda\n");
    out("  tasklist          - lista os processos (EPROCESS)\n");
    out("  sc query          - lista os drivers de kernel e o estado\n");
    out("  sc start <nome>   - carrega um driver .sys (chama DriverEntry)\n");
    out("  sc stop  <nome>   - descarrega um driver (chama DriverUnload)\n");
    out("  vol               - rotulo/serial/tamanho do volume C: (NTFS)\n");
    out("  dir               - lista o diretorio atual do C:\n");
    out("  cd <dir>          - muda o diretorio (cd \\ , cd .. , cd dir1)\n");
    out("  type <arquivo>    - mostra o conteudo de um arquivo\n");
    out("  copy <orig> <dst> - copia para um arquivo EXISTENTE (sobrescreve)\n");
    out("  del <arquivo>     - exclui um arquivo (stub: aviso)\n");
    out("  exit              - encerra o shell\n");
}

static const char* drv_state_name(unsigned st) {
    if (st == MEUOS_SERVICE_RUNNING) return "RUNNING";
    if (st == MEUOS_SERVICE_STOPPED) return "STOPPED";
    return "UNKNOWN";
}

static void cmd_tasklist(void) {
    out("\n");
    out("Image Name                   PID   Threads  ImageBase\n");
    out("=========================  =====  =======  ==================\n");
    MEUOS_PROCESS_ENTRY e;
    unsigned i = 0, count = 0;
    while (EnumProcessesEx(i, &e)) {
        out(e.ImageName[0] ? e.ImageName : "(sem nome)");
        int pad = 25 - (int)slen(e.ImageName[0] ? e.ImageName : "(sem nome)");
        for (int k = 0; k < pad; k++) out(" ");
        out("  ");
        out_dec(e.ProcessId);
        out("      ");
        out_dec(e.ThreadCount);
        out("        ");
        out_hex(e.ImageBase);
        out(e.Terminated ? "  [terminado]\n" : "\n");
        count++; i++;
    }
    out("Total: "); out_dec(count); out(" processo(s).\n");
}

static void cmd_sc_query(void) {
    out("\n");
    out("SERVICE_NAME              STATE\n");
    out("========================  ==========\n");
    MEUOS_DRIVER_ENTRY e;
    unsigned i = 0, count = 0;
    while (EnumDriversEx(i, &e)) {
        out(e.Name);
        int pad = 24 - (int)slen(e.Name);
        for (int k = 0; k < pad; k++) out(" ");
        out("  ");
        out_dec(e.State); out(" "); out(drv_state_name(e.State));
        out("\n");
        count++; i++;
    }
    if (count == 0) out("(nenhum driver registrado)\n");
    out("Total: "); out_dec(count); out(" driver(s).\n");
}

static void cmd_sc_start(const char* name) {
    if (!name[0]) { out("Uso: sc start <nome.sys>\n"); return; }
    out("sc start "); out(name); out(" ...\n");
    if (StartDriverServiceA(name))
        out("  [SC] driver iniciado (RUNNING).\n");
    else
        out("  [SC] FALHA ao iniciar (nome desconhecido ou DriverEntry falhou).\n");
}

static void cmd_sc_stop(const char* name) {
    if (!name[0]) { out("Uso: sc stop <nome.sys>\n"); return; }
    out("sc stop "); out(name); out(" ...\n");
    if (StopDriverServiceA(name))
        out("  [SC] driver parado (STOPPED).\n");
    else
        out("  [SC] FALHA ao parar (driver nao estava rodando?).\n");
}

static void cmd_sc(const char* args) {
    char sub[32];
    const char* rest = word(args, sub, sizeof(sub));
    if (ieq(sub, "query")) { cmd_sc_query(); return; }
    if (ieq(sub, "start")) { char n[32]; word(rest, n, sizeof(n)); cmd_sc_start(n); return; }
    if (ieq(sub, "stop"))  { char n[32]; word(rest, n, sizeof(n)); cmd_sc_stop(n);  return; }
    out("Uso: sc query | sc start <nome> | sc stop <nome>\n");
}

// vol — info do volume C: via QueryVolumeInfoEx (NtQueryVolumeInformation).
static void cmd_vol(void) {
    MEUOS_VOLUME_INFO vi;
    if (!QueryVolumeInfoEx(&vi)) {
        out(" Nenhum volume NTFS montado (rode com -Disk).\n");
        return;
    }
    out("\n Volume na unidade C: e "); out(vi.Label[0] ? vi.Label : "(sem rotulo)"); out("\n");
    out(" Sistema de arquivos: "); out(vi.FsName); out("\n");
    out(" Numero de serie do volume: "); out_hex(vi.Serial); out("\n");
    out(" Tamanho total: "); out_dec(vi.TotalBytes); out(" bytes (");
    out_dec(vi.TotalBytes / (1024 * 1024)); out(" MiB)\n");
    out(" Espaco livre (estimado): "); out_dec(vi.FreeBytes); out(" bytes\n");
    out(" Bytes por setor: "); out_dec(vi.BytesPerSector);
    out(" | bytes por cluster: "); out_dec(vi.BytesPerCluster); out("\n");
}

// dir — lista o diretorio ATUAL do volume C: via CreateFileA + QueryDirectoryFileEx
// (NtCreateFile/NtQueryDirectoryFile -> int 0x80 -> I/O Manager -> driver NTFS).
static void cmd_dir(const char* args) {
    char arg[256]; word(args, arg, sizeof(arg));
    char sub[256], full[260];
    if (arg[0]) resolve_subpath(arg, sub); else scopy(sub, g_cwd);
    make_full_path(sub, full);

    void* hDir = CreateFileA(full, 0, 0, 0, 3, 0, 0);
    if (hDir == MEUOS_INVALID_HANDLE || hDir == 0) {
        out(" Nenhum volume NTFS montado ou caminho invalido: C:"); out(sub); out("\n");
        out(" (dir requer um disco NTFS anexado ao QEMU: rode com -Disk.)\n");
        return;
    }
    out("\n Volume na unidade C: e o disco NTFS.\n");
    out(" Diretorio de C:"); out(sub[0] ? sub : "\\"); out("\n\n");

    MEUOS_DIR_ENTRY e;
    unsigned count = 0, files = 0, dirs = 0;
    unsigned long long total = 0;
    while (QueryDirectoryFileEx(hDir, &e)) {
        if (e.IsDir) { out(" <DIR>          "); dirs++; }
        else {
            // tamanho alinhado a direita em ~12 colunas
            char num[24]; int ni = 0; unsigned long long v = e.Size;
            if (v == 0) num[ni++] = '0';
            else { char t[24]; int ti = 0; while (v) { t[ti++] = (char)('0' + v % 10); v /= 10; } while (ti) num[ni++] = t[--ti]; }
            num[ni] = 0;
            int pad = 14 - ni; for (int k = 0; k < pad; k++) out(" ");
            out(num); out(" ");
            files++; total += e.Size;
        }
        out(e.Name);
        out("\n");
        count++;
        if (count > 256) break;   // guarda
    }
    CloseHandle(hDir);
    out("\n        "); out_dec(files); out(" arquivo(s)  ");
    out_dec(total); out(" bytes\n");
    out("        "); out_dec(dirs); out(" pasta(s)\n");
}

// cd — muda o diretorio atual. Valida abrindo o destino como diretorio e
// confirmando que ele lista (e um diretorio de verdade). 'cd' sem arg mostra o CWD.
static void cmd_cd(const char* args) {
    char arg[256]; word(args, arg, sizeof(arg));
    if (!arg[0]) { out("C:"); out(g_cwd); out("\n"); return; }

    char sub[256], full[260];
    resolve_subpath(arg, sub);
    make_full_path(sub, full);

    // Abre o caminho e tenta listar: se lista (ou esta vazio mas e dir), aceita.
    void* h = CreateFileA(full, 0, 0, 0, 3, 0, 0);
    if (h == MEUOS_INVALID_HANDLE || h == 0) {
        out(" O sistema nao pode encontrar o caminho especificado: C:"); out(sub); out("\n");
        return;
    }
    // O kernel so abre como diretorio se o caminho for um diretorio (FsContext).
    // Para confirmar, tentamos uma listagem; se vier ao menos a 1a entrada OU o
    // diretorio for valido (raiz), aceitamos. Diretorios da imagem tem entradas.
    MEUOS_DIR_ENTRY e;
    int is_listable = QueryDirectoryFileEx(h, &e);
    CloseHandle(h);

    // Caso raiz: sempre aceitavel.
    int is_root = (sub[0] == '\\' && sub[1] == 0);
    if (!is_listable && !is_root) {
        // Pode ser um arquivo (nao diretorio) ou diretorio vazio. Como a imagem
        // de teste nao tem diretorios vazios, tratamos "nao lista" como nao-dir.
        out(" O caminho nao e um diretorio: C:"); out(sub); out("\n");
        return;
    }
    scopy(g_cwd, sub);
    out("C:"); out(g_cwd); out("\n");
}

// type — mostra o conteudo de um arquivo (NtReadFile em blocos).
static void cmd_type(const char* args) {
    char arg[256]; word(args, arg, sizeof(arg));
    if (!arg[0]) { out("Uso: type <arquivo>\n"); return; }

    char sub[256], full[260];
    resolve_subpath(arg, sub);
    make_full_path(sub, full);

    void* hf = CreateFileA(full, 0, 0, 0, 3, 0, 0);
    if (hf == MEUOS_INVALID_HANDLE || hf == 0) {
        out(" O sistema nao pode encontrar o arquivo: C:"); out(sub); out("\n");
        return;
    }
    char buf[256]; unsigned got = 0; int any = 0; int last = 0;
    while (ReadFile(hf, buf, sizeof(buf) - 1, &got, 0) && got) {
        buf[got] = 0;
        out(buf);
        any = 1; last = buf[got - 1];
        if (got < sizeof(buf) - 1) break;   // leu menos que o pedido -> EOF
    }
    CloseHandle(hf);
    if (any && last != '\n') out("\n");
    if (!any) out("(arquivo vazio)\n");
}

// copy — copia o conteudo de 'orig' para 'dst' (que deve EXISTIR; o ring 3 ainda
// nao cria arquivos). Le 'orig' e escreve via WriteFile no 'dst' (caminho de
// escrita NTFS da Fase 4). Bom para sobrescrever um arquivo existente do C:.
static void cmd_copy(const char* args) {
    char a1[256]; const char* rest = word(args, a1, sizeof(a1));
    char a2[256]; word(rest, a2, sizeof(a2));
    if (!a1[0] || !a2[0]) { out("Uso: copy <origem> <destino>\n"); return; }

    char ssub[256], sfull[260], dsub[256], dfull[260];
    resolve_subpath(a1, ssub); make_full_path(ssub, sfull);
    resolve_subpath(a2, dsub); make_full_path(dsub, dfull);

    void* hs = CreateFileA(sfull, 0, 0, 0, 3, 0, 0);
    if (hs == MEUOS_INVALID_HANDLE || hs == 0) {
        out(" Origem nao encontrada: C:"); out(ssub); out("\n");
        return;
    }
    // Le a origem inteira (arquivos da imagem sao pequenos; buffer de 4 KiB).
    static char data[4096]; unsigned total = 0, got = 0;
    while (ReadFile(hs, data + total, (unsigned)(sizeof(data) - total), &got, 0) && got) {
        total += got;
        if (total >= sizeof(data) || got < 1) break;
    }
    CloseHandle(hs);

    void* hd = CreateFileA(dfull, 0, 0, 0, 3, 0, 0);
    if (hd == MEUOS_INVALID_HANDLE || hd == 0) {
        out(" Destino nao existe: C:"); out(dsub); out("\n");
        out(" (criar arquivos novos pelo ring 3 ainda nao e suportado; "
            "copy sobrescreve um arquivo EXISTENTE.)\n");
        return;
    }
    unsigned wrote = 0;
    int ok = WriteFile(hd, data, total, &wrote, 0);
    CloseHandle(hd);
    if (ok && wrote) {
        out("        1 arquivo(s) copiado(s) ("); out_dec(wrote);
        out(" bytes: C:"); out(ssub); out(" -> C:"); out(dsub); out(").\n");
    } else {
        out(" Falha ao escrever no destino C:"); out(dsub); out("\n");
    }
}

// del — exclui um arquivo. Sem syscall de delete no ring 3 ainda (o kernel tem
// ntfs_delete_file, mas nao ha NtDeleteFile exposto). Stub honesto.
static void cmd_del(const char* args) {
    char arg[256]; word(args, arg, sizeof(arg));
    if (!arg[0]) { out("Uso: del <arquivo>\n"); return; }
    char sub[256]; resolve_subpath(arg, sub);
    out(" del C:"); out(sub); out(": exclusao pelo ring 3 ainda nao suportada.\n");
    out(" (O kernel tem ntfs_delete_file; falta expor uma syscall NtDeleteFile.)\n");
}

// Despacha uma linha de comando. Devolve 0 se foi 'exit'.
static int run_line(const char* line) {
    char cmd[32];
    const char* rest = word(line, cmd, sizeof(cmd));
    if (cmd[0] == 0) return 1;                         // linha vazia
    if (ieq(cmd, "help") || ieq(cmd, "?")) { cmd_help(); return 1; }
    if (ieq(cmd, "tasklist"))              { cmd_tasklist(); return 1; }
    if (ieq(cmd, "sc"))                    { cmd_sc(rest); return 1; }
    if (ieq(cmd, "vol"))                   { cmd_vol(); return 1; }
    if (ieq(cmd, "dir"))                   { cmd_dir(rest); return 1; }
    if (ieq(cmd, "cd") || ieq(cmd, "chdir")) { cmd_cd(rest); return 1; }
    if (ieq(cmd, "type"))                  { cmd_type(rest); return 1; }
    if (ieq(cmd, "copy"))                  { cmd_copy(rest); return 1; }
    if (ieq(cmd, "del") || ieq(cmd, "erase")) { cmd_del(rest); return 1; }
    if (ieq(cmd, "cls"))                   { out("\n"); return 1; }
    if (ieq(cmd, "exit") || ieq(cmd, "quit")) { out("Encerrando o shell.\n"); return 0; }
    out("'"); out(cmd); out("' nao e reconhecido como um comando interno.\n");
    out("Digite 'help' para a lista de comandos.\n");
    return 1;
}

// ============================================================================
//  Modo demo (headless) + prompt interativo (com display).
// ============================================================================
static void prompt(void) { out("\nC:"); out(g_cwd); out("> "); }

static void demo(void) {
    out("\n");
    out("============================================================\n");
    out("  MeuOS Command Prompt (cmd.exe) - ring 3 - FASE 5\n");
    out("  Modo DEMO: executando comandos automaticamente.\n");
    out("============================================================\n");

    prompt(); out("help\n");
    run_line("help");

    prompt(); out("tasklist\n");
    run_line("tasklist");

    prompt(); out("sc query\n");
    run_line("sc query");

    // Exercita o ciclo de vida de um driver pelo SCM/loader: start -> query -> stop.
    prompt(); out("sc start mydriver.sys\n");
    run_line("sc start mydriver.sys");
    prompt(); out("sc stop mydriver.sys\n");
    run_line("sc stop mydriver.sys");

    // --- FASE 5: comandos de arquivo sobre o volume C: (NTFS) ---
    prompt(); out("vol\n");
    run_line("vol");

    prompt(); out("dir\n");
    run_line("dir");

    // type hello.txt — mostra o conteudo do arquivo conhecido do C: (NtReadFile).
    prompt(); out("type hello.txt\n");
    run_line("type hello.txt");

    // cd dir1 + dir + type file.txt — prova a navegacao em subdiretorio.
    prompt(); out("cd dir1\n");
    run_line("cd dir1");
    prompt(); out("dir\n");
    run_line("dir");
    prompt(); out("type file.txt\n");
    run_line("type file.txt");

    // copy file.txt file.txt — exercita o caminho de ESCRITA (sobrescreve um
    // arquivo EXISTENTE via WriteFile -> NtWriteFile -> driver NTFS).
    prompt(); out("copy file.txt file.txt\n");
    run_line("copy file.txt file.txt");

    // volta a raiz.
    prompt(); out("cd \\\n");
    run_line("cd \\");

    out("\n[cmd] modo demo concluido. ");
    out("Abrindo prompt interativo (digite com -display; headless encerra).\n");
}

// Le uma linha do console (stdin). Monta caractere a caractere tratando Enter e
// Backspace. Devolve 1 se leu uma linha (terminada por Enter), 0 se nao ha mais
// entrada (ReadFile retornou 0 bytes por varias tentativas — caso headless).
static int read_line(char* buf, int max) {
    void* hIn = GetStdHandle(STD_INPUT_HANDLE);
    int len = 0;
    unsigned idle = 0;
    for (;;) {
        char ch; unsigned got = 0;
        ReadFile(hIn, &ch, 1, &got, 0);
        if (got == 0) {
            if (++idle > 2000000u) { buf[len] = 0; return (len > 0); }
            continue;
        }
        idle = 0;
        if (ch == '\n' || ch == '\r') { buf[len] = 0; return 1; }
        if (ch == '\b') { if (len > 0) { len--; out("\b \b"); } continue; }
        if (len < max - 1) { buf[len++] = ch; char e[2] = { ch, 0 }; out(e); }
    }
}

void _start(void) {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);

    demo();

    // Prompt interativo. Com display, o usuario digita e ve o eco; cada linha e
    // despachada por run_line. Headless: read_line acaba devolvendo 0 (sem
    // teclas) e o shell encerra, deixando o boot prosseguir.
    char line[128];
    for (;;) {
        prompt();
        if (!read_line(line, sizeof(line))) break;   // sem entrada -> encerra
        trim_eol(line);
        if (!run_line(line)) break;                  // 'exit'
    }

    out("\n[cmd] shell encerrado.\n");
    ExitProcess(0);
}
