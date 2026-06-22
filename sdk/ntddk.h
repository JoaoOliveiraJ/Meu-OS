#pragma once
#include <stdint.h>
// Tipos minimos compativeis com o DDK do Windows, layout x64.
// Usamos larguras fixas (stdint) porque o kernel e LP64 e o driver e LLP64;
// assim as structs tem EXATAMENTE o mesmo layout dos dois lados.

typedef int32_t  NTSTATUS;
typedef int16_t  SHORT;
typedef uint16_t USHORT;
typedef uint32_t ULONG;
typedef uint8_t  BOOLEAN;
typedef void*    PVOID;
typedef uint16_t WCHAR;

#define STATUS_SUCCESS         ((NTSTATUS)0x00000000)
#define STATUS_UNSUCCESSFUL    ((NTSTATUS)0xC0000001)
#define FILE_DEVICE_UNKNOWN    0x00000022

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    WCHAR* Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

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

typedef struct _DEVICE_OBJECT {
    SHORT  Type;
    SHORT  Size;
    int32_t ReferenceCount;
    PVOID  DriverObject;
    PVOID  NextDevice;
    PVOID  AttachedDevice;
    PVOID  CurrentIrp;
    ULONG  Flags;
    PVOID  DeviceExtension;
} DEVICE_OBJECT, *PDEVICE_OBJECT;
