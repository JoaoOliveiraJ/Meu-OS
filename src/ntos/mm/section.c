// ============================================================================
//  FASE 7 — Section objects (NtCreateSection/NtMapViewOfSection) + helpers Mm.
//  Identidade-mapeada: o mesmo ponteiro vale p/ kernel e ring 3.
// ============================================================================
#include "mm/section.h"
#include "ob/object.h"
#include "mm/heap.h"
#include "hal/hal.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// uniq -> ASCII curto (usado em logs).
static void uni_to_ascii(PUNICODE_STRING u, char* out, int max) {
    int n = (u && u->Buffer) ? (u->Length / 2) : 0;
    int i = 0;
    for (; i < n && i < max - 1; i++) out[i] = (char)(u->Buffer[i] & 0xFF);
    out[i] = 0;
}

PSECTION_OBJECT section_create_anon(uint64_t size, const char* name_opt) {
    if (size == 0) return 0;
    PSECTION_OBJECT s = (PSECTION_OBJECT)ObCreateObject(/*OB_TYPE_SECTION*/9, sizeof(SECTION_OBJECT),
                                                       name_opt);
    if (!s) return 0;
    s->Size    = size;
    s->Base    = kmalloc((size_t)size);
    s->Protect = PAGE_READWRITE;
    s->Inherited = ViewShare;
    int i = 0; if (name_opt) while (name_opt[i] && i < (int)sizeof(s->Name) - 1) { s->Name[i] = name_opt[i]; i++; }
    s->Name[i] = 0;
    if (!s->Base) return 0;
    // Zero-init.
    for (uint64_t k = 0; k < size; k++) ((uint8_t*)s->Base)[k] = 0;
    return s;
}
void section_destroy(PSECTION_OBJECT s) {
    if (!s) return;
    if (s->Base) kfree(s->Base);
    s->Base = 0;
}

NTSTATUS NTAPI NtCreateSection_k(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                                 POBJECT_ATTRIBUTES ObjectAttributes,
                                 PLARGE_INTEGER MaximumSize, ULONG SectionPageProtection,
                                 ULONG AllocationAttributes, HANDLE FileHandle) {
    (void)DesiredAccess; (void)AllocationAttributes; (void)FileHandle;
    if (!SectionHandle || !MaximumSize) return STATUS_INVALID_PARAMETER;

    char name[64] = {0};
    if (ObjectAttributes && ObjectAttributes->ObjectName) uni_to_ascii(ObjectAttributes->ObjectName, name, sizeof(name));

    PSECTION_OBJECT s = section_create_anon((uint64_t)MaximumSize->QuadPart, name[0] ? name : 0);
    if (!s) return STATUS_NO_MEMORY;
    s->Protect = SectionPageProtection;

    HANDLE h = ob_create_handle(s);
    *SectionHandle = h;
    kputs("[sec] NtCreateSection size="); kput_dec(s->Size);
    if (name[0]) { kputs(" name='"); kputs(name); kputc('\''); }
    kputs(" base="); kput_hex((uint64_t)(uintptr_t)s->Base); kputc('\n');
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtOpenSection_k(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess,
                               POBJECT_ATTRIBUTES ObjectAttributes) {
    (void)DesiredAccess;
    if (!SectionHandle || !ObjectAttributes || !ObjectAttributes->ObjectName) return STATUS_INVALID_PARAMETER;
    char name[64];
    uni_to_ascii(ObjectAttributes->ObjectName, name, sizeof(name));
    void* body = ObLookupObject(name);
    if (!body) return STATUS_OBJECT_NAME_NOT_FOUND;
    *SectionHandle = ob_create_handle(body);
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtMapViewOfSection_k(HANDLE SectionHandle, HANDLE ProcessHandle, PVOID* BaseAddress,
                                    uint64_t ZeroBits, uint64_t CommitSize, PLARGE_INTEGER SectionOffset,
                                    uint64_t* ViewSize, SECTION_INHERIT InheritDisposition,
                                    ULONG AllocationType, ULONG Win32Protect) {
    (void)ProcessHandle; (void)ZeroBits; (void)CommitSize; (void)InheritDisposition;
    (void)AllocationType; (void)Win32Protect;
    if (!BaseAddress) return STATUS_INVALID_PARAMETER;

    PSECTION_OBJECT s = (PSECTION_OBJECT)ob_handle_to_object(SectionHandle, /*OB_TYPE_SECTION*/9);
    if (!s) return STATUS_INVALID_HANDLE;
    uint64_t off = SectionOffset ? (uint64_t)SectionOffset->QuadPart : 0;
    uint64_t vs  = ViewSize ? (*ViewSize ? *ViewSize : s->Size - off) : s->Size - off;
    if (off >= s->Size) return STATUS_INVALID_PARAMETER;
    if (off + vs > s->Size) vs = s->Size - off;
    *BaseAddress = (PVOID)((uint8_t*)s->Base + off);
    if (ViewSize) *ViewSize = vs;
    kputs("[sec] NtMapViewOfSection -> base="); kput_hex((uint64_t)(uintptr_t)*BaseAddress);
    kputs(" view="); kput_dec(vs); kputc('\n');
    return STATUS_SUCCESS;
}
NTSTATUS NTAPI NtUnmapViewOfSection_k(HANDLE ProcessHandle, PVOID BaseAddress) {
    (void)ProcessHandle; (void)BaseAddress;
    // Identidade-mapeada: nada a fazer (a section continua valida via handle).
    return STATUS_SUCCESS;
}

// ===== Mm* helpers =====
PHYSICAL_ADDRESS NTAPI MmGetPhysicalAddress_k(PVOID BaseAddress) {
    PHYSICAL_ADDRESS pa; pa.QuadPart = (LONGLONG)(uintptr_t)BaseAddress;   // identidade
    return pa;
}
PVOID NTAPI MmMapIoSpace_k(PHYSICAL_ADDRESS PhysicalAddress, SIZE_T NumberOfBytes, MEMORY_CACHING_TYPE Caching) {
    (void)NumberOfBytes; (void)Caching;
    return (PVOID)hal_map_mmio((uint64_t)PhysicalAddress.QuadPart, (uint64_t)NumberOfBytes);
}
void NTAPI MmUnmapIoSpace_k(PVOID BaseAddress, SIZE_T NumberOfBytes) {
    (void)BaseAddress; (void)NumberOfBytes;
}
PVOID NTAPI MmAllocateContiguousMemory_k(SIZE_T NumberOfBytes, PHYSICAL_ADDRESS HighestAcceptableAddress) {
    (void)HighestAcceptableAddress;
    void* p = kmalloc(NumberOfBytes);
    return p;   // kmalloc devolve faixa baixa (identidade); contiguo logico/fisico iguais.
}
void NTAPI MmFreeContiguousMemory_k(PVOID BaseAddress) {
    if (BaseAddress) kfree(BaseAddress);
}
PVOID NTAPI MmAllocateNonCachedMemory_k(SIZE_T NumberOfBytes) {
    return kmalloc(NumberOfBytes);
}
void NTAPI MmFreeNonCachedMemory_k(PVOID BaseAddress, SIZE_T NumberOfBytes) {
    (void)NumberOfBytes; if (BaseAddress) kfree(BaseAddress);
}
NTSTATUS NTAPI MmProtectMdlSystemAddress_k(PVOID Mdl, ULONG NewProtect) {
    (void)Mdl; (void)NewProtect; return STATUS_SUCCESS;
}
BOOLEAN NTAPI MmIsAddressValid_k(PVOID VirtualAddress) {
    uintptr_t v = (uintptr_t)VirtualAddress;
    return (v != 0 && v < 0x40000000ULL) ? 1 : 0;   // 1 GiB identidade
}
