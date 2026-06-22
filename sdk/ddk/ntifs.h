#pragma once
// ============================================================================
//  ntifs.h  —  Installable File Systems (drivers de filesystem).
//
//  No WDK real, ntifs.h estende ntddk.h com tipos privados do FILE_OBJECT,
//  FsRtl* (File System Runtime Library), CC_FILE_SIZES, e definicoes para
//  filter manager (FltMgr). No MeuOS, alias do ntddk.h consolidado por
//  enquanto — drivers de FS tipo NTFS importam isto pra clareza semantica.
// ============================================================================
#include "ntddk.h"
