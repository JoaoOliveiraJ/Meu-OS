#pragma once
#include <stdint.h>
// Tipos minimos compativeis com o DDK do Windows, layout x64.
// Usamos larguras fixas (stdint) porque o kernel e LP64 e o driver e LLP64;
// assim as structs tem EXATAMENTE o mesmo layout dos dois lados.
//
// Esta versao foi EXPANDIDA na FASE 7 (Driver Framework completo) para suportar
// drivers .sys que importem o conjunto completo da ntoskrnl/HAL: novos IRP_MJ_*,
// rotinas de suporte (IoCompleteRequest, ...), objetos de sincronizacao do KE
// (KEVENT/KSPIN_LOCK/KMUTEX), tipos de callback (Ps*/Ob*/Cm*), section objects,
// registro, e os enums de classes de NtQuery*Information mais usados.

typedef int32_t  NTSTATUS;
typedef int16_t  SHORT;
typedef uint16_t USHORT;
typedef int32_t  LONG;
typedef uint32_t ULONG;
typedef int64_t  LONGLONG;
typedef uint64_t ULONGLONG;
typedef uint64_t SIZE_T;
typedef uint8_t  BOOLEAN;
typedef uint8_t  UCHAR;
typedef void*    PVOID;
typedef uint16_t WCHAR;
typedef uint8_t  KIRQL;
typedef void*    HANDLE;
typedef int32_t  KPRIORITY;
typedef uint8_t  KPROCESSOR_MODE;
typedef uint64_t ACCESS_MASK;
typedef HANDLE*  PHANDLE;
typedef ULONG*   PULONG;
typedef NTSTATUS* PNTSTATUS;

#define IN
#define OUT
#define OPTIONAL
#define NTAPI            __attribute__((ms_abi))
#define DDKAPI           __attribute__((ms_abi))
#define PAGED_CODE()     ((void)0)
#define UNREFERENCED_PARAMETER(P) ((void)(P))

// ===== Status codes (subset). NT_SUCCESS macro p/ checagem rapida. =====
#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000)
#define STATUS_TIMEOUT                  ((NTSTATUS)0x00000102)
#define STATUS_PENDING                  ((NTSTATUS)0x00000103)
#define STATUS_BUFFER_OVERFLOW          ((NTSTATUS)0x80000005)
#define STATUS_NO_MORE_ENTRIES          ((NTSTATUS)0x8000001A)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xC0000002)
#define STATUS_INVALID_INFO_CLASS       ((NTSTATUS)0xC0000003)
#define STATUS_INFO_LENGTH_MISMATCH     ((NTSTATUS)0xC0000004)
#define STATUS_ACCESS_VIOLATION         ((NTSTATUS)0xC0000005)
#define STATUS_INVALID_HANDLE           ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000D)
#define STATUS_NO_SUCH_FILE             ((NTSTATUS)0xC000000F)
#define STATUS_END_OF_FILE              ((NTSTATUS)0xC0000011)
#define STATUS_NO_MEMORY                ((NTSTATUS)0xC0000017)
#define STATUS_ACCESS_DENIED            ((NTSTATUS)0xC0000022)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023)
#define STATUS_OBJECT_TYPE_MISMATCH     ((NTSTATUS)0xC0000024)
#define STATUS_OBJECT_NAME_NOT_FOUND    ((NTSTATUS)0xC0000034)
#define STATUS_OBJECT_NAME_COLLISION    ((NTSTATUS)0xC0000035)
#define STATUS_OBJECT_PATH_NOT_FOUND    ((NTSTATUS)0xC000003A)
#define STATUS_DEVICE_NOT_READY         ((NTSTATUS)0xC00000A3)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BB)
#define STATUS_DEBUGGER_INACTIVE        ((NTSTATUS)0xC0000354)
#define NT_SUCCESS(s)                   (((NTSTATUS)(s)) >= 0)

#define FILE_DEVICE_UNKNOWN             0x00000022
#define FILE_DEVICE_NETWORK             0x00000012
#define FILE_DEVICE_BEEP                0x00000001

// Atributos comuns p/ ObjectAttributes.
#define OBJ_INHERIT                     0x00000002
#define OBJ_PERMANENT                   0x00000010
#define OBJ_KERNEL_HANDLE               0x00000200
#define OBJ_CASE_INSENSITIVE            0x00000040

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    WCHAR* Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _STRING {
    USHORT Length;
    USHORT MaximumLength;
    char*  Buffer;
} ANSI_STRING, *PANSI_STRING, STRING, *PSTRING;

// LARGE_INTEGER classico do NT (union para acesso a 64 ou aos halves).
typedef union _LARGE_INTEGER {
    struct { ULONG LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct { ULONG LowPart; ULONG HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

// Endereco fisico (idem o WDK).
typedef LARGE_INTEGER PHYSICAL_ADDRESS;

// LIST_ENTRY do NT — usada em varias estruturas (DPCs/IRPs/etc.). Layout fixo.
typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY* Flink;
    struct _LIST_ENTRY* Blink;
} LIST_ENTRY, *PLIST_ENTRY;

// Dispatcher header — base de todos os objetos de sincronizacao do KE.
typedef struct _DISPATCHER_HEADER {
    UCHAR  Type;          // 0=Notification, 1=Synchronization, etc.
    UCHAR  Absolute;
    UCHAR  Size;
    UCHAR  Inserted;
    LONG   SignalState;   // 0=not signaled, !=0=signaled
    LIST_ENTRY WaitListHead;
} DISPATCHER_HEADER, *PDISPATCHER_HEADER;

// Objetos de sincronizacao do KE (layout suficiente p/ drivers nao quebrarem).
typedef struct _KEVENT     { DISPATCHER_HEADER Header; }      KEVENT, *PKEVENT;
typedef struct _KSEMAPHORE { DISPATCHER_HEADER Header; LONG Limit; } KSEMAPHORE, *PKSEMAPHORE;
typedef struct _KMUTEX     { DISPATCHER_HEADER Header; LIST_ENTRY MutantListEntry; PVOID OwnerThread; UCHAR Abandoned; UCHAR ApcDisable; } KMUTEX, *PKMUTEX;
typedef ULONGLONG KSPIN_LOCK, *PKSPIN_LOCK;
// FASE FUNDACAO (Item 3) — handle de queued spinlock (in-stack).
typedef struct _KLOCK_QUEUE_HANDLE { PKSPIN_LOCK LockPtr; KIRQL OldIrql; } KLOCK_QUEUE_HANDLE, *PKLOCK_QUEUE_HANDLE;

// FASE FUNDACAO (Item 2) — KDPC (Deferred Procedure Call), layout NT x64 (0x40).
typedef struct _KDPC {
    UCHAR  Type;                // +0x00
    UCHAR  Importance;          // +0x01
    USHORT Number;              // +0x02  (0xFFFF = nao enfileirado)
    LIST_ENTRY DpcListEntry;    // +0x08
    PVOID  DeferredRoutine;     // +0x18
    PVOID  DeferredContext;     // +0x20
    PVOID  SystemArgument1;     // +0x28
    PVOID  SystemArgument2;     // +0x30
    PVOID  DpcData;             // +0x38
} KDPC, *PKDPC;
typedef void (NTAPI *PKDEFERRED_ROUTINE)(PKDPC Dpc, PVOID Ctx, PVOID Arg1, PVOID Arg2);

// FASE FUNDACAO (Item 6) — KTIMER. Dispatcher object; ao expirar sinaliza
// (acorda waiters) e opcionalmente enfileira um DPC. Header.Type: 8=notificacao
// (fica sinalizado), 9=sincronizacao (auto-reset).
typedef struct _KTIMER {
    DISPATCHER_HEADER Header;         // +0x00
    LARGE_INTEGER     DueTime;        // +0x18 — reproposito: tick alvo (g_ticks)
    LIST_ENTRY        TimerListEntry; // +0x20  (Flink==0 => fora da lista)
    PVOID             Dpc;            // +0x30  (KDPC*, opcional)
    LONG              Period;         // +0x38  (ms; 0 = one-shot)
    LONG              _pad;           // +0x3C
} KTIMER, *PKTIMER;
typedef enum _TIMER_TYPE { NotificationTimer = 0, SynchronizationTimer = 1 } TIMER_TYPE;

// FASE FUNDACAO (Item 7) — primitivos Ex (layout simplificado; nossos drivers
// compilam contra ISTO. pintok os usa via modo legado (no-op) -> layout nao
// importa p/ ele).
typedef struct _FAST_MUTEX {
    LONG   Count;          // 1=livre, <=0 preso/contencao
    PVOID  Owner;
    ULONG  Contention;
    ULONG  _pad;
    KEVENT Event;
    ULONG  OldIrql;
} FAST_MUTEX, *PFAST_MUTEX;

typedef struct _ERESOURCE {
    LONG       ActiveCount;      // >0 = N leitores (shared); -1 = exclusivo
    LONG       ExclusiveWaiters;
    LONG       SharedWaiters;
    PVOID      OwnerThread;
    KEVENT     ExclusiveEvent;
    KSEMAPHORE SharedSem;
} ERESOURCE, *PERESOURCE;

typedef struct _NPAGED_LOOKASIDE_LIST {
    void*      ListHead;   // LIFO: o primeiro ptr de cada bloco livre aponta o proximo
    KSPIN_LOCK Lock;
    uint32_t   Size;
    uint32_t   Tag;
    uint32_t   Depth;
    uint32_t   _pad;
} NPAGED_LOOKASIDE_LIST, *PNPAGED_LOOKASIDE_LIST;
typedef NPAGED_LOOKASIDE_LIST PAGED_LOOKASIDE_LIST, *PPAGED_LOOKASIDE_LIST;

// FASE FUNDACAO (trilha I/O, Fase 3) — modelo de interrupcao. KINTERRUPT e opaco
// p/ drivers (so acessado via IoConnectInterrupt/KeSynchronizeExecution). Os
// prototipos publicos precisam bater com o WDK (ms_abi resolve registradores).
typedef struct _KINTERRUPT KINTERRUPT, *PKINTERRUPT;
typedef BOOLEAN (NTAPI *PKSERVICE_ROUTINE)(PKINTERRUPT Interrupt, PVOID ServiceContext);
typedef BOOLEAN (NTAPI *PKSYNCHRONIZE_ROUTINE)(PVOID SynchronizeContext);
typedef enum _KINTERRUPT_MODE { LevelSensitive = 0, Latched = 1 } KINTERRUPT_MODE;
typedef uint64_t KAFFINITY;
#define DIRQL_DEFAULT_DEVICE  11

// FASE FUNDACAO (trilha I/O, Fase 5) — HAL DMA. DMA_OPERATIONS e uma vtable que
// o driver deref (adapter->DmaOperations->AllocateCommonBuffer(...)); a ORDEM dos
// membros e ABI (WDK). So Allocate/FreeCommonBuffer + GetDmaAlignment tem corpo
// real; o resto sao stubs seguros (0). phys==virt (identity map).
typedef PHYSICAL_ADDRESS *PPHYSICAL_ADDRESS;
typedef struct _DMA_ADAPTER DMA_ADAPTER, *PDMA_ADAPTER;
typedef PVOID (NTAPI *PALLOCATE_COMMON_BUFFER)(PDMA_ADAPTER, ULONG Length, PPHYSICAL_ADDRESS LogicalAddress, BOOLEAN CacheEnabled);
typedef void  (NTAPI *PFREE_COMMON_BUFFER)(PDMA_ADAPTER, ULONG Length, PHYSICAL_ADDRESS LogicalAddress, PVOID VirtualAddress, BOOLEAN CacheEnabled);
typedef ULONG (NTAPI *PGET_DMA_ALIGNMENT)(PDMA_ADAPTER);
typedef struct _DMA_OPERATIONS {
    ULONG Size;                                 // +0x00
    PVOID PutDmaAdapter;                         // +0x08
    PALLOCATE_COMMON_BUFFER AllocateCommonBuffer;// +0x10
    PFREE_COMMON_BUFFER     FreeCommonBuffer;    // +0x18
    PVOID AllocateAdapterChannel;                // +0x20
    PVOID FlushAdapterBuffers;                   // +0x28
    PVOID FreeAdapterChannel;                    // +0x30
    PVOID FreeMapRegisters;                      // +0x38
    PVOID MapTransfer;                           // +0x40
    PGET_DMA_ALIGNMENT GetDmaAlignment;          // +0x48
    PVOID ReadDmaCounter;                        // +0x50
    PVOID GetScatterGatherList;                  // +0x58
    PVOID PutScatterGatherList;                  // +0x60
    PVOID CalculateScatterGatherListSize;        // +0x68
    PVOID BuildScatterGatherList;                // +0x70
    PVOID BuildMdlFromScatterGatherList;         // +0x78
} DMA_OPERATIONS, *PDMA_OPERATIONS;
typedef struct _DMA_ADAPTER {
    USHORT Version;
    USHORT Size;
    PDMA_OPERATIONS DmaOperations;
} DMA_ADAPTER;

// Argumentos do KeWaitForSingleObject / KeDelayExecutionThread.
typedef enum _KWAIT_REASON {
    Executive = 0, FreePage, PageIn, PoolAllocation, DelayExecution, Suspended,
    UserRequest, WrExecutive, WrFreePage, WrPageIn, WrPoolAllocation, WrDelayExecution
} KWAIT_REASON;

typedef enum _EVENT_TYPE {
    NotificationEvent = 0, SynchronizationEvent
} EVENT_TYPE;

typedef enum _WAIT_TYPE { WaitAll = 0, WaitAny } WAIT_TYPE;

typedef enum _POOL_TYPE {
    NonPagedPool = 0, PagedPool = 1, NonPagedPoolMustSucceed = 2,
    DontUseThisType = 3, NonPagedPoolCacheAligned = 4,
    PagedPoolCacheAligned = 5, NonPagedPoolCacheAlignedMustS = 6,
    MaxPoolType = 7
} POOL_TYPE;

// MEMORY_CACHING_TYPE p/ MmMapIoSpace.
typedef enum _MEMORY_CACHING_TYPE {
    MmNonCached = 0, MmCached = 1, MmWriteCombined = 2, MmHardwareCoherentCached = 3,
    MmNonCachedUnordered = 4, MmUSWCCached = 5, MmMaximumCacheType = 6
} MEMORY_CACHING_TYPE;

// MODE: KernelMode = 0, UserMode = 1.
#define KernelMode 0
#define UserMode   1

// OBJECT_ATTRIBUTES — usado por NtCreate*/NtOpen* (NtCreateKey, NtCreateFile, etc.).
typedef struct _OBJECT_ATTRIBUTES {
    ULONG          Length;
    HANDLE         RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG          Attributes;
    PVOID          SecurityDescriptor;
    PVOID          SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

#define InitializeObjectAttributes(p,n,a,r,s) do {            \
    (p)->Length = (ULONG)sizeof(OBJECT_ATTRIBUTES);            \
    (p)->RootDirectory = (r);                                  \
    (p)->ObjectName = (n);                                     \
    (p)->Attributes = (a);                                     \
    (p)->SecurityDescriptor = (s);                             \
    (p)->SecurityQualityOfService = 0;                         \
} while (0)

// CLIENT_ID — pares (ProcessId, ThreadId) usados em PsLookup* e callbacks.
typedef struct _CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} CLIENT_ID, *PCLIENT_ID;

// Layout casa com os offsets reais do DRIVER_OBJECT do NT (x64).
typedef struct _DRIVER_OBJECT {
    SHORT          Type;
    SHORT          Size;
    PVOID          DeviceObject;        // 0x08
    ULONG          Flags;               // 0x10
    PVOID          DriverStart;         // 0x18
    ULONG          DriverSize;          // 0x20
    PVOID          DriverSection;       // 0x28
    PVOID          DriverExtension;     // 0x30
    UNICODE_STRING DriverName;          // 0x38
    PVOID          HardwareDatabase;    // 0x48
    PVOID          FastIoDispatch;      // 0x50
    PVOID          DriverInit;          // 0x58
    PVOID          DriverStartIo;       // 0x60
    PVOID          DriverUnload;        // 0x68
    PVOID          MajorFunction[28];   // 0x70
} DRIVER_OBJECT, *PDRIVER_OBJECT;

// FASE FUNDACAO (trilha I/O, Fase 1a) — DEVICE_OBJECT com offsets NT x64 reais.
// DeviceExtension@0x40 (era 0x30) e DeviceType@0x48 (antes 'Flags' era usado como
// tipo — bug). Drivers reais acessam esses campos por offset baked-in; aproximar
// do NT so aumenta compatibilidade. Campos internos (Timer/Vpb/Queue/Dpc/...) sao
// opacos aqui (nenhum driver in-tree os acessa por offset). Vpb sempre 0.
typedef struct _VPB *PVPB;   // opaco
typedef struct _DEVICE_OBJECT {
    SHORT   Type;                    // 0x00
    SHORT   Size;                    // 0x02
    int32_t ReferenceCount;          // 0x04
    PVOID   DriverObject;            // 0x08
    PVOID   NextDevice;              // 0x10
    PVOID   AttachedDevice;          // 0x18
    PVOID   CurrentIrp;              // 0x20
    PVOID   Timer;                   // 0x28
    ULONG   Flags;                   // 0x30
    ULONG   Characteristics;         // 0x34
    PVPB    Vpb;                     // 0x38
    PVOID   DeviceExtension;         // 0x40  <- bate com NT
    ULONG   DeviceType;              // 0x48
    signed char StackSize;           // 0x4C  (CCHAR)
    uint8_t _tail[0xB8 - 0x4D];      // 0x4D..0xB7 (Queue/Dpc/SecurityDescriptor/etc.)
} DEVICE_OBJECT, *PDEVICE_OBJECT;
_Static_assert(__builtin_offsetof(DEVICE_OBJECT, DeviceExtension) == 0x40, "DeviceExtension deve estar em 0x40 (NT x64)");
_Static_assert(__builtin_offsetof(DEVICE_OBJECT, DeviceType)      == 0x48, "DeviceType deve estar em 0x48 (NT x64)");
_Static_assert(sizeof(DEVICE_OBJECT) == 0xB8, "sizeof(DEVICE_OBJECT) deve ser 0xB8 (NT x64)");

// ===================== I/O Manager (IRPs, IOCTL) =====================
// IRP majors completas conforme o WDM (uso os mesmos numeros do Windows).
#define IRP_MJ_CREATE                   0x00
#define IRP_MJ_CREATE_NAMED_PIPE        0x01
#define IRP_MJ_CLOSE                    0x02
#define IRP_MJ_READ                     0x03
#define IRP_MJ_WRITE                    0x04
#define IRP_MJ_QUERY_INFORMATION        0x05
#define IRP_MJ_SET_INFORMATION          0x06
#define IRP_MJ_QUERY_EA                 0x07
#define IRP_MJ_SET_EA                   0x08
#define IRP_MJ_FLUSH_BUFFERS            0x09
#define IRP_MJ_QUERY_VOLUME_INFORMATION 0x0A
#define IRP_MJ_SET_VOLUME_INFORMATION   0x0B
#define IRP_MJ_DIRECTORY_CONTROL        0x0C
#define IRP_MJ_FILE_SYSTEM_CONTROL      0x0D
#define IRP_MJ_DEVICE_CONTROL           0x0E
#define IRP_MJ_INTERNAL_DEVICE_CONTROL  0x0F
#define IRP_MJ_SHUTDOWN                 0x10
#define IRP_MJ_LOCK_CONTROL             0x11
#define IRP_MJ_CLEANUP                  0x12
#define IRP_MJ_CREATE_MAILSLOT          0x13
#define IRP_MJ_QUERY_SECURITY           0x14
#define IRP_MJ_SET_SECURITY             0x15
#define IRP_MJ_POWER                    0x16
#define IRP_MJ_SYSTEM_CONTROL           0x17
#define IRP_MJ_DEVICE_CHANGE            0x18
#define IRP_MJ_QUERY_QUOTA              0x19
#define IRP_MJ_SET_QUOTA                0x1A
#define IRP_MJ_PNP                      0x1B
#define IRP_MJ_MAXIMUM_FUNCTION         0x1B

#define METHOD_BUFFERED   0
#define METHOD_IN_DIRECT  1
#define METHOD_OUT_DIRECT 2
#define METHOD_NEITHER    3

#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((ULONG)(DeviceType) << 16) | ((ULONG)(Access) << 14) | \
     ((ULONG)(Function) << 2) | (ULONG)(Method))

#define FILE_ANY_ACCESS    0
#define FILE_READ_ACCESS   1
#define FILE_WRITE_ACCESS  2

typedef struct _IO_STATUS_BLOCK {
    NTSTATUS Status;
    uint64_t Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef struct _IO_STACK_LOCATION {
    uint8_t MajorFunction;
    uint8_t MinorFunction;
    uint8_t Flags;
    uint8_t Control;
    union {
        struct {
            ULONG OutputBufferLength;
            ULONG InputBufferLength;
            ULONG IoControlCode;
            PVOID Type3InputBuffer;
        } DeviceIoControl;
        struct { ULONG Length; ULONG Key; uint64_t ByteOffset; } Read;
        struct { ULONG Length; ULONG Key; uint64_t ByteOffset; } Write;
        struct { ULONG Length; PUNICODE_STRING FileName; uint32_t FileInformationClass; ULONG FileIndex; } QueryDirectory;
        struct { uint8_t Minor; uint8_t Flags; ULONG SystemContext; } Pnp;
    } Parameters;
    PDEVICE_OBJECT DeviceObject;
    PVOID FileObject;
    void* CompletionRoutine;
    PVOID Context;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;

typedef struct _IRP {
    PVOID              SystemBuffer;     // AssociatedIrp.SystemBuffer (METHOD_BUFFERED)
    IO_STATUS_BLOCK    IoStatus;
    PVOID              UserBuffer;       // buffer de saida do usuario
    PIO_STACK_LOCATION CurrentStack;     // -> StackLocation
    IO_STACK_LOCATION  StackLocation;    // 1 nivel (simplificado)
    UCHAR              Cancel;
    UCHAR              StackCount;       // expostos p/ drivers que checam
    UCHAR              CurrentLocation;
} IRP, *PIRP;

typedef NTSTATUS (NTAPI *PDRIVER_DISPATCH)(PDEVICE_OBJECT, PIRP);
typedef void     (NTAPI *PDRIVER_UNLOAD)(PDRIVER_OBJECT);
typedef NTSTATUS (NTAPI *PDRIVER_INITIALIZE)(PDRIVER_OBJECT, PUNICODE_STRING);
typedef void     (NTAPI *PIO_COMPLETION_ROUTINE)(PDEVICE_OBJECT, PIRP, PVOID);
typedef void     (NTAPI *PKSTART_ROUTINE)(PVOID Context);

// ====================== Callbacks de Ps/Ob/Cm ======================
// Process / Thread / Image notification (PsSetCreate*NotifyRoutine).
typedef void (NTAPI *PCREATE_PROCESS_NOTIFY_ROUTINE)(HANDLE ParentId, HANDLE ProcessId, BOOLEAN Create);
typedef void (NTAPI *PCREATE_THREAD_NOTIFY_ROUTINE) (HANDLE ProcessId, HANDLE ThreadId, BOOLEAN Create);
typedef void (NTAPI *PLOAD_IMAGE_NOTIFY_ROUTINE)    (PUNICODE_STRING FullImageName, HANDLE ProcessId, PVOID ImageInfo);

typedef struct _PS_CREATE_NOTIFY_INFO {
    SIZE_T          Size;
    union { ULONG Flags; struct { ULONG FileOpenNameAvailable : 1; ULONG Reserved : 31; }; };
    HANDLE          ParentProcessId;
    CLIENT_ID       CreatingThreadId;
    PVOID           FileObject;
    PUNICODE_STRING ImageFileName;
    PUNICODE_STRING CommandLine;
    NTSTATUS        CreationStatus;
} PS_CREATE_NOTIFY_INFO, *PPS_CREATE_NOTIFY_INFO;

typedef void (NTAPI *PCREATE_PROCESS_NOTIFY_ROUTINE_EX)(PVOID Process, HANDLE ProcessId, PPS_CREATE_NOTIFY_INFO CreateInfo);

// Ob callbacks (ObRegisterCallbacks).
typedef enum _OB_OPERATION { OB_OPERATION_HANDLE_CREATE = 1, OB_OPERATION_HANDLE_DUPLICATE = 2 } OB_OPERATION;

typedef struct _OB_PRE_OPERATION_INFORMATION  OB_PRE_OPERATION_INFORMATION,  *POB_PRE_OPERATION_INFORMATION;
typedef struct _OB_POST_OPERATION_INFORMATION OB_POST_OPERATION_INFORMATION, *POB_POST_OPERATION_INFORMATION;

typedef ULONG (NTAPI *POB_PRE_OPERATION_CALLBACK) (PVOID, POB_PRE_OPERATION_INFORMATION);
typedef void  (NTAPI *POB_POST_OPERATION_CALLBACK)(PVOID, POB_POST_OPERATION_INFORMATION);

typedef struct _OB_OPERATION_REGISTRATION {
    PVOID*                      ObjectType;     // *PsProcessType, *PsThreadType, ...
    ULONG                       Operations;
    POB_PRE_OPERATION_CALLBACK  PreOperation;
    POB_POST_OPERATION_CALLBACK PostOperation;
} OB_OPERATION_REGISTRATION, *POB_OPERATION_REGISTRATION;

typedef struct _OB_CALLBACK_REGISTRATION {
    USHORT                     Version;
    USHORT                     OperationRegistrationCount;
    UNICODE_STRING             Altitude;
    PVOID                      RegistrationContext;
    POB_OPERATION_REGISTRATION OperationRegistration;
} OB_CALLBACK_REGISTRATION, *POB_CALLBACK_REGISTRATION;

// Cm callbacks (CmRegisterCallback / Ex).
typedef enum _REG_NOTIFY_CLASS {
    RegNtPreDeleteKey = 0, RegNtPreSetValueKey, RegNtPreDeleteValueKey,
    RegNtPreSetInformationKey, RegNtPreRenameKey, RegNtPreEnumerateKey,
    RegNtPreEnumerateValueKey, RegNtPreQueryKey, RegNtPreQueryValueKey,
    RegNtPreQueryMultipleValueKey, RegNtPreCreateKey, RegNtPostCreateKey,
    RegNtPreOpenKey, RegNtPostOpenKey, RegNtPreKeyHandleClose, MaxRegNtNotifyClass
} REG_NOTIFY_CLASS;
typedef NTSTATUS (NTAPI *PEX_CALLBACK_FUNCTION)(PVOID CallbackContext, PVOID Argument1, PVOID Argument2);

// SECTION_INHERIT / page protection (NtCreateSection).
typedef enum _SECTION_INHERIT { ViewShare = 1, ViewUnmap = 2 } SECTION_INHERIT;

#define PAGE_NOACCESS          0x01
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04
#define PAGE_WRITECOPY         0x08
#define PAGE_EXECUTE           0x10
#define PAGE_EXECUTE_READ      0x20
#define PAGE_EXECUTE_READWRITE 0x40

#define SEC_FILE      0x00800000
#define SEC_IMAGE     0x01000000
#define SEC_RESERVE   0x04000000
#define SEC_COMMIT    0x08000000
#define SEC_NOCACHE   0x10000000

// SYSTEM_INFORMATION_CLASS (subset usado).
#define SystemBasicInformation                0
#define SystemProcessorInformation            1
#define SystemPerformanceInformation          2
#define SystemTimeOfDayInformation            3
#define SystemProcessInformation              5
#define SystemModuleInformation               11
#define SystemHandleInformation               16
#define SystemFirmwareTableInformation        76
#define SystemModuleInformationEx             77   /* 0x4D — RTL_PROCESS_MODULE_INFORMATION_EX[] */
#define SystemCodeIntegrityInformation        103
#define SystemSecureBootPolicyInformation     144
#define SystemVsmProtectionInformation        152
#define MeuOsVersionInformation               0x1000

// MEMORY_INFORMATION_CLASS / FILE_INFORMATION_CLASS (subset).
#define FileDirectoryInformation              1
#define FileBothDirectoryInformation          3
#define FileFullDirectoryInformation          2
#define FileIdBothDirectoryInformation        37

// PsThreadType / PsProcessType (publicados como ponteiros pelo NT).
extern PVOID *PsProcessType;
extern PVOID *PsThreadType;
extern PVOID *IoFileObjectType;

// KdDebuggerEnabled / KdDebuggerNotPresent (exports globais do NT).
extern BOOLEAN KdDebuggerEnabled;
extern BOOLEAN KdDebuggerNotPresent;

// IRQL x64 real (0-15). CR8/TPR mapeia 1:1 (IRQL == priority class = vetor>>4):
// PASSIVE=0, APC=1, DISPATCH=2, DIRQL de dispositivo=3..11, CLOCK=13 (vetor
// 0xD1), IPI=14 (vetor 0xE1), HIGH=15. KIRQL e uint8_t (cabe 0-15).
#define PASSIVE_LEVEL    0
#define APC_LEVEL        1
#define DISPATCH_LEVEL   2
#define DEVICE_LEVEL_MIN 3
#define DEVICE_LEVEL_MAX 12
#define CLOCK_LEVEL      13
#define IPI_LEVEL        14
#define HIGH_LEVEL       15

// ============================================================================
//  FASE 7.6 — Memory Descriptor List (MDL) — layout NT x64 real.
//
//  Antes desta fase, IoAllocateMdl no ntexec.c devolvia 64 bytes opacos com
//  campos chutados via indice de PVOID*. Drivers reais como pintok.sys que
//  fazem MmGetSystemAddressForMdlSafe / MmProbeAndLockPages / acessos por
//  campo (Mdl->ByteCount, Mdl->StartVa, Mdl->MdlFlags) liam lixo e quebravam.
//
//  Layout abaixo casa com o WDK x64 (verificado em ntddk.h da Microsoft):
//    +0x00  Next               PMDL
//    +0x08  Size               SHORT  (tamanho total da MDL+PFN[])
//    +0x0A  MdlFlags           SHORT
//    +0x10  Process            PEPROCESS (dono — 0 = pool sistema)
//    +0x18  MappedSystemVa     PVOID   (preenchido por MmMapLockedPagesSpecifyCache)
//    +0x20  StartVa            PVOID   (alinhado em pagina; base do mapping)
//    +0x28  ByteCount          ULONG
//    +0x2C  ByteOffset         ULONG
//    +0x30  Pages[]            PFN_NUMBER[] (variavel; sizeof(uintptr_t) cada)
//
//  Apos o cabecalho de 0x30 bytes vem o array de PFNs (paginas fisicas).
//  Cada PFN_NUMBER tem o tamanho de uma palavra do host (uintptr_t).
// ============================================================================

typedef uintptr_t PFN_NUMBER, *PPFN_NUMBER;

typedef struct _MDL {
    struct _MDL* Next;             // +0x00  proximo MDL na cadeia (chained MDLs)
    SHORT        Size;             // +0x08  tamanho total alocado p/ MDL+PFN[]
    SHORT        MdlFlags;         // +0x0A  bits MDL_*
    PVOID        Process;          // +0x10  EPROCESS dono (0 = pool de sistema)
    PVOID        MappedSystemVa;   // +0x18  endereco virtual de sistema mapeado
    PVOID        StartVa;          // +0x20  endereco virtual base (alinhado em pagina)
    ULONG        ByteCount;        // +0x28  tamanho em bytes do buffer descrito
    ULONG        ByteOffset;       // +0x2C  offset dentro da primeira pagina
    // Apos +0x30: PFN_NUMBER Pages[]  (variavel; nao declarado aqui)
} MDL, *PMDL;

// Flags do MDL (subset que drivers reais checam).
#define MDL_MAPPED_TO_SYSTEM_VA      0x0001
#define MDL_PAGES_LOCKED             0x0002
#define MDL_SOURCE_IS_NONPAGED_POOL  0x0004
#define MDL_ALLOCATED_FIXED_SIZE     0x0008
#define MDL_PARTIAL                  0x0010
#define MDL_PARTIAL_HAS_BEEN_MAPPED  0x0020
#define MDL_IO_PAGE_READ             0x0040
#define MDL_WRITE_OPERATION          0x0080
#define MDL_PARENT_MAPPED_SYSTEM_VA  0x0100
#define MDL_LOCK_HELD                0x0200
#define MDL_PHYSICAL_VIEW            0x0400
#define MDL_IO_SPACE                 0x0800
#define MDL_NETWORK_HEADER           0x1000
#define MDL_MAPPING_CAN_FAIL         0x2000
#define MDL_ALLOCATED_MUST_SUCCEED   0x4000

// LockOperation enum p/ MmProbeAndLockPages.
typedef enum _LOCK_OPERATION {
    IoReadAccess = 0, IoWriteAccess = 1, IoModifyAccess = 2
} LOCK_OPERATION;

// MM_PAGE_PRIORITY — drivers passam (LowPagePriority, NormalPagePriority, etc.)
// como ULONG. Nao precisamos do enum exato, basta receber/ignorar.

// ===== Acessores estilo macro do WDK =====
//
// Sem MMU virtualizando-paginas-por-processo, nosso ambiente e quase identidade:
// StartVa + ByteOffset = endereco original. MappedSystemVa, quando setado, ja
// aponta para o buffer pronto p/ leitura/escrita do kernel.
//
// MmGetSystemAddressForMdlSafe: se ja mapeado, devolve MappedSystemVa; senao
// chama MmMapLockedPagesSpecifyCache (no nosso caso, retorna o mesmo endereco
// identidade do StartVa+ByteOffset).

PVOID NTAPI MmMapLockedPagesSpecifyCache(PMDL Mdl, KPROCESSOR_MODE AccessMode,
                                          MEMORY_CACHING_TYPE CacheType,
                                          PVOID BaseAddress, ULONG BugCheckOnFailure,
                                          ULONG Priority);

static inline PVOID MmGetMdlVirtualAddress(PMDL Mdl) {
    return Mdl ? (PVOID)((uintptr_t)Mdl->StartVa + Mdl->ByteOffset) : 0;
}
static inline ULONG MmGetMdlByteCount(PMDL Mdl)  { return Mdl ? Mdl->ByteCount  : 0; }
static inline ULONG MmGetMdlByteOffset(PMDL Mdl) { return Mdl ? Mdl->ByteOffset : 0; }
static inline PPFN_NUMBER MmGetMdlPfnArray(PMDL Mdl) {
    // Array de PFNs vive logo apos o cabecalho (offset 0x30).
    return Mdl ? (PPFN_NUMBER)((uint8_t*)Mdl + 0x30) : 0;
}

static inline PVOID MmGetSystemAddressForMdlSafe(PMDL Mdl, ULONG Priority) {
    if (!Mdl) return 0;
    if (Mdl->MdlFlags & MDL_MAPPED_TO_SYSTEM_VA) return Mdl->MappedSystemVa;
    if (Mdl->MdlFlags & MDL_SOURCE_IS_NONPAGED_POOL)
        return (PVOID)((uintptr_t)Mdl->StartVa + Mdl->ByteOffset);
    return MmMapLockedPagesSpecifyCache(Mdl, KernelMode, MmCached, 0, 0, Priority);
}

// Macro estilo WDK ADDRESS_AND_SIZE_TO_SPAN_PAGES — quantas paginas o buffer atravessa.
#define BYTES_TO_PAGES(b)             (((SIZE_T)(b) + 4095) >> 12)
#define ADDRESS_AND_SIZE_TO_SPAN_PAGES(addr, size) \
    ((ULONG)((((uintptr_t)(addr) & 0xFFFULL) + (size) + 4095) >> 12))
