#pragma once
// ============================================================================
//  wdm.h  —  Windows Driver Model (drivers de kernel padrao).
//
//  No WDK real, wdm.h contem o subconjunto base (tipos primitivos, DRIVER_OBJECT,
//  IRP, KEVENT/KSPIN_LOCK, basic Rtl*, basic Io*) que TODOS os drivers usam.
//  ntddk.h estende com APIs do executive (Cc/Cm/Ex/Mm/Ob/Ps/Se), e ntifs.h
//  estende com FILE_OBJECT internals / FsRtl* (so para filesystem drivers).
//
//  No MeuOS, FASE 8: comeca como alias do ntddk.h consolidado. Pode ser
//  dividido em rodadas futuras se a separacao de niveis virar importante.
// ============================================================================
#include "ntddk.h"
