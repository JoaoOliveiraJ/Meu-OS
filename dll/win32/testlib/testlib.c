// testlib.dll — DLL de teste da FASE 3f (LoadLibrary em runtime). Exporta 2 funcoes
// que o loadlib.exe resolve EM RUNTIME (LoadLibraryA + GetProcAddress) e chama POR
// PONTEIRO. Nao e' importada estaticamente por ninguem: so entra no processo quando
// LoadLibraryA a carrega. Compilada -nostdlib (sem CRT): so DllMain + 2 exports.
unsigned int _tls_index = 0;

__declspec(dllexport) int testlib_add(int a, int b) { return a + b; }
__declspec(dllexport) const char* testlib_name(void) {
    return "testlib.dll v1 (carregada em runtime)";
}

int DllMain(void* h, unsigned reason, void* reserved) {
    (void)h; (void)reason; (void)reserved; return 1;
}
