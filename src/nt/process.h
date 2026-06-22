#pragma once
#include <stdint.h>
#include "ntddk.h"      // BOOLEAN
#include "nt/object.h"

// ============================================================================
//  FASE 7.3 — Expansao de EPROCESS/ETHREAD para o LAYOUT NT 10 x64.
//
//  ESTRATEGIA DE COMPAT (sem regredir nada existente):
//   - Os campos "antigos" (ProcessId, ThreadCount, DirectoryTableBase, ImageBase,
//     PML4, ExitStatus, Terminated, ImageName, Thread) FICAM no inicio da struct,
//     com OS MESMOS TIPOS. Todo o codigo atual (syscall.c, loader.c, systhread.c,
//     usermode.c, callbacks.c) continua compilando e rodando 1:1.
//   - Em seguida adicionamos um "espelho" do EPROCESS/ETHREAD do Windows 10 x64,
//     posicionado a partir do offset 0x100 (alem dos campos antigos). Drivers
//     que andem os offsets do NT (gs:[..], EPROCESS+0x440, ETHREAD+0x650, etc.)
//     encontram dados validos NO LUGAR CERTO.
//   - Isso e feito com 'char _padX[N]' calculados para chegar nos offsets exatos.
//     Static asserts (no .c) confirmam cada offset critico.
//   - O sizeof(EPROCESS) cresce p/ ~2 KiB e o sizeof(ETHREAD) p/ ~2 KiB. O Object
//     Manager (ObCreateObject) aceita qualquer tamanho de corpo.
//
//  OBS sobre offsets: o NT real varia 1-2 bytes entre builds. Esses valores aqui
//  correspondem ao Windows 10 1809+ (build 17763+), que e o que driver de teste
//  (pintok.sys) espera. Se algum offset divergir, ajustar o respectivo _padX.
// ============================================================================

struct _ETHREAD;

// ----------------------------------------------------------------------------
//  Sub-estruturas espelhadas do NT (so os campos que algum driver le).
// ----------------------------------------------------------------------------

// LIST_ENTRY do NT (DOUBLE-linked, igual ao Windows).
typedef struct _NT_LIST_ENTRY {
    struct _NT_LIST_ENTRY* Flink;
    struct _NT_LIST_ENTRY* Blink;
} NT_LIST_ENTRY;

// KPROCESS embutido (offsets relativos ao inicio do KPROCESS).
// O EPROCESS tem KProcess em +0x000 (o KPROCESS e o "header KE" do EPROCESS).
//   +0x028 DirectoryTableBase  (CR3)
//   +0x050 ThreadListHead       (LIST_ENTRY)
typedef struct __attribute__((packed)) _NT_KPROCESS {
    uint8_t        _pad_000[0x28];
    uint64_t       DirectoryTableBase;   // +0x028
    uint8_t        _pad_030[0x50 - 0x30];
    NT_LIST_ENTRY  ThreadListHead;       // +0x050
    uint8_t        _pad_060[0x180 - 0x60]; // espaco ate +0x180 (PageFaultsCount mora la)
} NT_KPROCESS;

// KTHREAD embutido (TCB do ETHREAD). Offsets relativos ao TCB:
//   +0x180 InitialStack  (PVOID)
//   +0x188 StackLimit    (PVOID)
//   +0x190 KernelStack   (PVOID)
//   +0x1F8 TrapFrame     (PVOID)
typedef struct __attribute__((packed)) _NT_KTHREAD {
    uint8_t  _pad_000[0x180];
    void*    InitialStack;       // +0x180
    void*    StackLimit;         // +0x188
    void*    KernelStack;        // +0x190
    uint8_t  _pad_198[0x1F8 - 0x198];
    void*    TrapFrame;          // +0x1F8
    uint8_t  _pad_200[0x420 - 0x200];   // ate o inicio do ETHREAD body (CreateTime @ 0x420)
} NT_KTHREAD;

// ============================================================================
//  EPROCESS — expandido para ~2 KiB com offsets NT 10 x64.
//
//  Campos antigos (legacy, intocados):
//    [0]   ProcessId         (u32)         <-- usado por todo lugar
//    [4]   ThreadCount       (u32)
//    [8]   DirectoryTableBase(u64)
//    [16]  ImageBase         (u64)
//    [24]  PML4              (void*)
//    [32]  ExitStatus        (u32)
//    [36]  Terminated        (BOOLEAN)
//    [37]  ImageName[32]     (char[32])
//    [69]  Thread            (PETHREAD)
//  Espelho NT (em offsets ABSOLUTOS dentro do EPROCESS, contados desde p):
//    +0x000  union { legacy ; NT_KPROCESS KProcess; }  <-- compartilha header
//    +0x028  KProcess.DirectoryTableBase  (mas tambem em [8] antigo — preenchemos OS DOIS)
//    +0x180  PageFaultsCount   (u32)
//    +0x440  UniqueProcessId   (HANDLE)
//    +0x448  ActiveProcessLinks (LIST_ENTRY)
//    +0x4B8  InheritedFromUniqueProcessId (HANDLE)
//    +0x550  Peb              (PVOID)
//    +0x568  Wow64Process     (PVOID)
//    +0x5A8  ImageFileName[15] (UCHAR[15])
//    +0x650  ActiveThreads    (ULONG)
//    +0x710  ObjectTable      (PVOID)
//
//  Importante: como o NT_KPROCESS embute DirectoryTableBase em +0x28 e nos
//  temos um campo legacy 'DirectoryTableBase' em [8], NAO podemos sobrepor
//  estes. Solucao: o KProcess fica como ESPELHO em offset 0 (compartilhando
//  os bytes dos campos legacy), e PsCreateProcess preenche os dois lugares.
//
//  Para conseguir isso COM offsets exatos, usamos uma uniao no topo:
//    union {
//      struct legacy { ProcessId/ThreadCount/... };
//      NT_KPROCESS  KProcess;   // sobrepoe os MESMOS bytes
//    } header;
//  O acesso 'p->ProcessId' continua valido pois e o primeiro membro da union.
//  Mas o legacy ocupa apenas ~69 bytes — o KProcess vai ate +0x180.
//
//  No fim, mantemos dois "ponteiros logicos" para a mesma area:
//    - p->ProcessId / p->ThreadCount / p->DirectoryTableBase / p->ImageBase
//      / p->PML4 / p->ExitStatus / p->Terminated / p->ImageName / p->Thread
//    - p->KProcess.DirectoryTableBase / p->KProcess.ThreadListHead
//      (espelhados pelo PsCreateProcess)
// ============================================================================

typedef struct _EPROCESS {
    // ----- HEADER COMPARTILHADO (0x000 ate 0x180) ------------------------
    // Mantemos os campos antigos intocados; eles ocupam 0..~80 bytes.
    // O resto ate +0x180 e padding (zerado), seguindo a area do KProcess.
    uint32_t  ProcessId;               // +0x000  PID (1, 2, 3, ...)
    uint32_t  ThreadCount;             // +0x004  threads vivas neste processo
    uint64_t  DirectoryTableBase;      // +0x008  CR3 do processo (PML4 fisica)
    uint64_t  ImageBase;               // +0x010  ImageBase do .exe carregado
    void*     PML4;                    // +0x018  ponteiro virtual p/ a PML4
    uint32_t  ExitStatus;              // +0x020  status de saida
    BOOLEAN   Terminated;              // +0x024  1 quando o processo encerrou
    uint8_t   _pad_025[3];             // +0x025  align
    char      ImageName[32];           // +0x028  nome curto da imagem (legacy)
    struct _ETHREAD* Thread;           // +0x048  thread principal (legacy)

    // Padding ate +0x180 (PageFaultsCount). Tudo zerado por ObCreateObject.
    uint8_t   _pad_050[0x180 - 0x50];

    // ----- ESPELHO NT 10 x64 (a partir de +0x180) ------------------------
    uint32_t  PageFaultsCount;         // +0x180
    uint8_t   _pad_184[0x440 - 0x184];

    HANDLE    UniqueProcessId;         // +0x440  (HANDLE)(pid)
    NT_LIST_ENTRY ActiveProcessLinks;  // +0x448  lista circular de EPROCESS (vazia=>aponta p/ si)
    uint8_t   _pad_458[0x4B8 - 0x458];

    HANDLE    InheritedFromUniqueProcessId; // +0x4B8  parent pid (HANDLE)
    uint8_t   _pad_4C0[0x550 - 0x4C0];

    void*     Peb;                     // +0x550  PEB do processo (ring3) — opcional
    uint8_t   _pad_558[0x568 - 0x558];

    void*     Wow64Process;            // +0x568  NULL p/ processos nativos x64
    uint8_t   _pad_570[0x5A8 - 0x570];

    // ImageFileName: 15 bytes (UCHAR[15]) — nome curto da imagem no offset NT.
    uint8_t   ImageFileName[15];       // +0x5A8
    uint8_t   _pad_5B7[0x650 - 0x5B7];

    uint32_t  ActiveThreads;           // +0x650
    uint8_t   _pad_654[0x710 - 0x654];

    void*     ObjectTable;             // +0x710  PHANDLE_TABLE (no nosso kernel = NULL/global)
    uint8_t   _pad_718[0x800 - 0x718]; // total ~2 KiB
} EPROCESS, *PEPROCESS;

// ============================================================================
//  ETHREAD — espelhado para o NT 10 x64 (~2 KiB).
//
//  Campos antigos:
//    [0]  ThreadId      (u32)
//    [8]  Process       (PEPROCESS)
//    [16] StartAddress  (u64)
//    [24] StackBase     (u64)
//    [32] StackTop      (u64)
//    [40] ExitStatus    (u32)
//    [44] Terminated    (BOOLEAN)
//  Espelho NT:
//    +0x000 Tcb (KTHREAD: InitialStack/StackLimit/KernelStack/TrapFrame)
//    +0x420 CreateTime (LARGE_INTEGER)
//    +0x650 Cid       (CLIENT_ID = {UniqueProcess, UniqueThread})
//    +0x6B0 ImpersonationInfo
//    +0x7C8 ThreadName (PUNICODE_STRING)
//
//  Os campos antigos cabem dentro do KTHREAD (que vai de 0..0x420). Como
//  ThreadId/Process/etc. ocupam ~48 bytes a partir de 0, eles sobrepoem o
//  inicio do KTHREAD (que e padding no NT). O ETHREAD body real comeca a
//  +0x420; e la que colocamos CreateTime, Cid e ThreadName.
// ============================================================================

typedef struct _ETHREAD {
    // ----- LEGACY (no inicio, primeira parte do KTHREAD padding) ---------
    uint32_t  ThreadId;                // +0x000  TID
    uint32_t  _pad_004;                // align
    PEPROCESS Process;                 // +0x008  processo dono
    uint64_t  StartAddress;            // +0x010  RIP inicial em ring 3
    uint64_t  StackBase;               // +0x018  base da pilha de usuario
    uint64_t  StackTop;                // +0x020  topo (RSP inicial)
    uint32_t  ExitStatus;              // +0x028
    BOOLEAN   Terminated;              // +0x02C
    uint8_t   _pad_02D[3];             // align

    // ----- KTHREAD continua (campos NT em offsets fixos) -----------------
    // Padding ate +0x180 (InitialStack do KTHREAD).
    uint8_t   _pad_030[0x180 - 0x30];

    void*     InitialStack;            // +0x180  (KTHREAD.InitialStack)
    void*     StackLimit;              // +0x188  (KTHREAD.StackLimit)
    void*     KernelStack;             // +0x190  (KTHREAD.KernelStack)
    uint8_t   _pad_198[0x1F8 - 0x198];
    void*     TrapFrame;               // +0x1F8

    uint8_t   _pad_200[0x420 - 0x200];

    // ----- ETHREAD body real (NT) ----------------------------------------
    int64_t   CreateTime;              // +0x420  LARGE_INTEGER.QuadPart
    uint8_t   _pad_428[0x650 - 0x428];

    // CLIENT_ID = { HANDLE UniqueProcess; HANDLE UniqueThread; }
    HANDLE    Cid_UniqueProcess;       // +0x650
    HANDLE    Cid_UniqueThread;        // +0x658
    uint8_t   _pad_660[0x6B0 - 0x660];

    void*     ImpersonationInfo;       // +0x6B0  (NULL na nossa implementacao)
    uint8_t   _pad_6B8[0x7C8 - 0x6B8];

    void*     ThreadName;              // +0x7C8  PUNICODE_STRING (NULL)
    uint8_t   _pad_7D0[0x800 - 0x7D0]; // total ~2 KiB
} ETHREAD, *PETHREAD;

void      ps_init(void);

// Cria um EPROCESS (objeto OB_TYPE_PROCESS). dir_base = CR3 do processo
// (0 = usar o CR3 atual/compartilhado). Devolve o corpo do objeto.
PEPROCESS PsCreateProcess(const char* image_name, uint64_t image_base, uint64_t dir_base);

// Cria um ETHREAD (objeto OB_TYPE_THREAD) ligado a 'proc'.
PETHREAD  PsCreateThread(PEPROCESS proc, uint64_t start, uint64_t stack_top, uint64_t stack_base);

// Marca o processo (e sua thread) como terminado e guarda o ExitStatus.
void      PsTerminateProcess(PEPROCESS proc, uint32_t status);

// Processo/thread "corrente" (o que esta rodando em ring 3 agora).
PEPROCESS PsGetCurrentProcess(void);
PETHREAD  PsGetCurrentThread(void);
void      ps_set_current(PEPROCESS proc, PETHREAD thr);   // uso interno do loader/usermode
