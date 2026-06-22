// ============================================================================
//  src/nt/seh.c — FASE 7.9: SEH minimo.
//
//  CONTEXTO
//  --------
//  Drivers Windows reais sao compilados com __try/__except. Em x64, o suporte
//  e tabela-based (.pdata/.xdata, NAO frame-based como x86): quando uma
//  excecao (Page Fault, GP, etc.) acontece, o kernel chama o "language
//  specific handler" — para MSVC C/C++ esse handler e __C_specific_handler.
//
//  IMPLEMENTACAO PRAGMATICA NESTA FASE
//  -----------------------------------
//  Nao temos dispatcher de excecao stack-walking ainda. Entao o nosso
//  __C_specific_handler simplesmente:
//
//    - se "is_unwind" (IS_UNWINDING no ExceptionRecord) -> retorna
//      ExceptionContinueSearch (1) para deixar o NT-runtime nao quebrar;
//    - caso contrario -> retorna ExceptionContinueSearch (1) tambem (propaga
//      ao frame externo — o nosso isr.c trata a recuperacao real, mapeando
//      pagina ZERADA quando o fault esta no espaco de driver).
//
//  Ou seja, do ponto de vista do CHAMADOR este e um handler "transparente":
//  ele nao FILTRA, mas tambem nao quebra. O EFEITO util de SEH vem do
//  recovery no isr.c (map-zero) — o driver continua a partir do RIP que
//  falhou (CPU re-executa a instrucao que faultou, agora encontrando a
//  pagina mapeada).
//
//  REGRAS NT QUE PRECISAMOS PRESERVAR
//  ----------------------------------
//  - ABI ms_abi (este e o ABI Microsoft x64; drivers chamam com este conv).
//  - Retorna inteiro 32-bit (EXCEPTION_DISPOSITION): 0 ExceptionContinueExecution,
//    1 ExceptionContinueSearch, 2 ExceptionNestedException, 3 ExceptionCollidedUnwind.
//
//  PROBE FOR READ/WRITE
//  --------------------
//  Validacoes basicas: alinhamento, "Length>0", e que o endereco NAO esta
//  na faixa do KERNEL (acima de MmUserProbeAddress; usamos 0x7FFFFFFF0000
//  como heuristica simples). Se invalido, NAO faz raise (sem SEH real);
//  apenas retorna sem fazer nada — o caller que confiou na area lera lixo
//  ou cairá no Page Fault handler (que recupera mapeando zero-page).
// ============================================================================
#include <stdint.h>
#include <stddef.h>
#include "ntddk.h"

extern void kputs(const char* s);
extern void kput_hex(uint64_t v);

// EXCEPTION_DISPOSITION (valores do NT)
#define ExceptionContinueExecution  0
#define ExceptionContinueSearch     1
#define ExceptionNestedException    2
#define ExceptionCollidedUnwind     3

// EXCEPTION_RECORD layout minimo (so ler ExceptionFlags / ExceptionCode).
typedef struct _EXCEPTION_RECORD_LITE {
    ULONG ExceptionCode;
    ULONG ExceptionFlags;
    struct _EXCEPTION_RECORD_LITE* ExceptionRecord;
    PVOID ExceptionAddress;
    ULONG NumberParameters;
    uint64_t ExceptionInformation[15];
} EXCEPTION_RECORD_LITE;

#define EXCEPTION_NONCONTINUABLE   0x00000001
#define EXCEPTION_UNWINDING        0x00000002
#define EXCEPTION_EXIT_UNWIND      0x00000004
#define EXCEPTION_STACK_INVALID    0x00000008
#define EXCEPTION_NESTED_CALL      0x00000010
#define EXCEPTION_TARGET_UNWIND    0x00000020
#define EXCEPTION_COLLIDED_UNWIND  0x00000040

// MmUserProbeAddress heuristico (igual ao Windows: HIGHEST_USER_ADDRESS - 0x10000).
#define USER_PROBE_LIMIT  0x000007FFFFFEFFFFULL

// ----------------------------------------------------------------------------
//  __C_specific_handler — handler "transparente": sempre devolve CONTINUE_SEARCH
//
//  ASSINATURA (ABI ms_abi):
//    EXCEPTION_DISPOSITION __cdecl __C_specific_handler(
//        IN PEXCEPTION_RECORD ExceptionRecord,
//        IN PVOID             EstablisherFrame,
//        IN OUT PCONTEXT      ContextRecord,
//        IN OUT PDISPATCHER_CONTEXT DispatcherContext);
//
//  Em modo unwind, ALGUNS handlers MSVC esperam que voltemos sem tocar nada.
//  Continuar a busca ate o frame externo e o comportamento "default safe".
// ----------------------------------------------------------------------------
__attribute__((ms_abi)) int __C_specific_handler(
        EXCEPTION_RECORD_LITE* ExceptionRecord,
        void*                  EstablisherFrame,
        void*                  ContextRecord,
        void*                  DispatcherContext) {
    (void)EstablisherFrame; (void)ContextRecord; (void)DispatcherContext;

    if (ExceptionRecord) {
        if (ExceptionRecord->ExceptionFlags &
            (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
            // Estamos em fase de unwind: nada a fazer aqui (sem scope walk
            // real). Volta CONTINUE_SEARCH para nao quebrar nested handlers.
            return ExceptionContinueSearch;
        }
        // Loga 1x p/ saber que algum codigo de driver pediu o handler.
        kputs("[seh] __C_specific_handler chamado: code=");
        kput_hex(ExceptionRecord->ExceptionCode);
        kputs(" flags=");
        kput_hex(ExceptionRecord->ExceptionFlags);
        kputs("\n");
    }

    // Sem dispatcher real: propaga p/ frame externo. O fallback eh o nosso
    // isr.c, que recupera page-fault de driver mapeando zero-page.
    return ExceptionContinueSearch;
}

// ----------------------------------------------------------------------------
//  ProbeForRead / ProbeForWrite — validacao basica.
//
//  Especificacao NT:
//    void ProbeForRead (CONST VOID* Address, SIZE_T Length, ULONG Alignment);
//    void ProbeForWrite(PVOID        Address, SIZE_T Length, ULONG Alignment);
//
//  Levantam STATUS_ACCESS_VIOLATION (excecao) se:
//    - Length == 0  -> NAO (nao faz nada)
//    - Address mal alinhado
//    - Address + Length cruza o limite de user
//
//  Como nao temos raise-exception real, apenas LOGAMOS e retornamos. O codigo
//  do driver que confiar em ProbeForRead/Write como "garantia" eventualmente
//  cai num page-fault que e tratado pelo nosso isr.c.
// ----------------------------------------------------------------------------
__attribute__((ms_abi)) void NT_ProbeForRead_real(const void* Address, SIZE_T Length, ULONG Alignment) {
    if (Length == 0) return;
    uint64_t a = (uint64_t)Address;

    // Alinhamento.
    if (Alignment && (a & (Alignment - 1))) {
        kputs("[probe] ProbeForRead: ALINHAMENTO invalido addr=");
        kput_hex(a); kputs(" align="); kput_hex(Alignment); kputs("\n");
        return;
    }
    // Range usuario.
    uint64_t end = a + (uint64_t)Length;
    if (a > USER_PROBE_LIMIT || end > USER_PROBE_LIMIT || end < a) {
        kputs("[probe] ProbeForRead: FORA do range user addr=");
        kput_hex(a); kputs(" len="); kput_hex((uint64_t)Length); kputs("\n");
        return;
    }
    // Validacao OK: nao faz acesso real (drivers reais nao fazem tambem).
}

__attribute__((ms_abi)) void NT_ProbeForWrite_real(void* Address, SIZE_T Length, ULONG Alignment) {
    if (Length == 0) return;
    uint64_t a = (uint64_t)Address;

    if (Alignment && (a & (Alignment - 1))) {
        kputs("[probe] ProbeForWrite: ALINHAMENTO invalido addr=");
        kput_hex(a); kputs(" align="); kput_hex(Alignment); kputs("\n");
        return;
    }
    uint64_t end = a + (uint64_t)Length;
    if (a > USER_PROBE_LIMIT || end > USER_PROBE_LIMIT || end < a) {
        kputs("[probe] ProbeForWrite: FORA do range user addr=");
        kput_hex(a); kputs(" len="); kput_hex((uint64_t)Length); kputs("\n");
        return;
    }
}
