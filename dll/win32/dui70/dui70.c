// dui70.dll — DirectUI (framework de UI PRIVADO do shell do Windows). O explorer real
// carrega esta DLL em runtime (LoadLibraryW(L"dui70")) e faz GetProcAddress dos pontos de
// entrada do DirectUI p/ montar a taskbar/menu-iniciar.
//
// REALIDADE HONESTA: a dui70.dll real tem 4321 exports — construtores/metodos C++
// mangled de DirectUI (Button, ScrollBar, Browser, DUIXmlParser, ...). Reimplementar o
// DirectUI e' de escala ReactOS, fora do escopo de um incremento. Esta DLL e' um PONTO DE
// APOIO MINIMO cujo objetivo e' duplo e concreto:
//   (1) resolver o UNICO import (delay) que o explorer tem de dui70 — SkipDLLUnloadInitChecks
//       — com um no-op nomeado e correto (ele so diz ao loader p/ pular checagens de unload);
//   (2) permitir que LoadLibraryW(L"dui70") tenha SUCESSO (base != NULL), p/ o explorer
//       AVANCAR e revelar via GetProcAddress EXATAMENTE quais entradas do DirectUI ele pede
//       (mapeadas pelo log de GetProcAddress gated no sys_getprocaddress). Isso transforma o
//       muro "dui70 e' uma caixa-preta" numa LISTA NOMEADA — o proximo alvo de implementacao.
// NAO e' um catch-all: exporta so o que o explorer referencia por nome hoje. Autocontido.

typedef unsigned long long ULL_;
unsigned int _tls_index = 0;

#define DUI70_DBG 0
#if DUI70_DBG
static void dbg_puts(const char* s) { ULL_ ret; __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(1ULL), "D"(s) : "memory", "rcx", "r11"); }
#endif

// SkipDLLUnloadInitChecks — na dui70 real, marca a DLL p/ o loader NAO rodar certas
// checagens de init no unload (evita reentrancia no teardown). Sem estado real aqui: no-op.
// (Delay-import do explorer; assinatura sem retorno significativo.)
__declspec(dllexport) void SkipDLLUnloadInitChecks(void) {
#if DUI70_DBG
    dbg_puts("[dui70] SkipDLLUnloadInitChecks (no-op)\n");
#endif
}

// DllMain — DirectUI real inicializa TLS/estado do processo aqui; p/ o ponto de apoio,
// so devolvemos sucesso p/ o LoadLibrary completar.
__declspec(dllexport) int DllMain(void* h, unsigned reason, void* v) {
    (void)h; (void)reason; (void)v;
#if DUI70_DBG
    dbg_puts("[dui70] DllMain -> TRUE\n");
#endif
    return 1;
}
