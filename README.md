# MeuOS — sistema operacional 64-bit compatível com o NT, escrito do zero em C

Um kernel de verdade, **bootável**, escrito do zero em C + Assembly, que reimplementa
as mecânicas do **Windows NT** (ring 0/3, syscalls, Object/Process/IO Manager, win32k,
HAL, NTFS, SMP + scheduler preemptivo) e as **DLLs do sistema** (ntdll, kernel32,
user32, gdi32, combase/WinRT, shell32, shcore, …) como **PE reais** — no modelo do
**Wine/ReactOS**. Roda no **QEMU** (baixado automaticamente).

## 🎯 A missão atual: rodar o `explorer.exe` REAL do Windows 10

O norte deste projeto é **carregar e rodar o binário REAL da Microsoft**
`C:\Windows\explorer.exe` sobre o MeuOS — não um explorer caseiro. O explorer real
importa **1066 funções de 136 DLLs** (COM/WinRT + DWM + shell namespace + DirectUI +
ETW): é uma escalada estilo ReactOS/Wine, resolvida **diagnóstico-first**, um "muro"
por vez. Em paralelo, o MeuOS é validado contra um **driver de anti-cheat REAL**
(`pintok.sys` = Riot Vanguard), que exercita mecânicas de ring 0 profundas.

**Onde está hoje (resumo):** o explorer real **carrega, mapeia e reloca inteiro**, roda
o `wWinMain` em **modo shell (mode 3)**, cria a *Worker Window*, sobe o **worker do
taskbar** (Taskband Pin / jump lists) numa **thread ring-3 preemptiva** e encerra
limpo. A **fronteira** é a persistência do desktop: o worker do shell dá *FailFast* na
init de **DirectUI (dui70) + objetos COM de shell** — quando isso for implementado por
completo, o explorer entra no `DesktopExplorerHost` (host persistente) e não sai.

## Início rápido
```powershell
.\setup.ps1                                   # baixa Zig + QEMU (uma vez)
.\build.ps1                                   # compila -> build\kernel.bin + as DLLs/apps

# copia o explorer REAL da sua maquina p/ o boot (uma vez):
cp C:\Windows\explorer.exe build\explorerreal.exe

.\run.ps1                                     # PADRAO: sobe o explorer.exe REAL, LIMPO
.\run.ps1 -Headless -TimeoutSec 50            # sem janela; captura a serial e encerra
```

### Cenários (`-Scenario`)
| Cenário | O que sobe | Uso |
|---|---|---|
| *(padrão)* / `explorerreal` | os 15 módulos que o **explorer.exe REAL** exige + `explorerreal.exe` | a **missão** — boot limpo |
| `pintok` | só `pintok.sys` (Riot Vanguard) | baseline do **anti-cheat** (regra de ouro) |
| `full` | o **desfile completo** de apps de teste/demo + shell caseiro | exercitar todos os subsistemas |
| `desktop` | shell **caseiro** (csrss/winlogon/explorer caseiro) | referência de shell persistente |

```powershell
.\run.ps1 -Scenario pintok  -Headless -TimeoutSec 40   # baseline anti-cheat (dourada: P1/P2/P3 + C0000365)
.\run.ps1 -Scenario full    -Headless -TimeoutSec 25   # todos os apps de teste (polui log/tela; opt-in)
.\run.ps1 -Modules C:\caminho\app.exe                  # roda QUALQUER PE do Windows
```
> O `.\run.ps1` **padrão** foi limpo: antes carregava um desfile de apps de teste
> (cmd/guiapp/dx*/sysinfo/pipe*) + o shell caseiro, poluindo o log e a tela. Agora
> sobe só o explorer real; o desfile virou `-Scenario full`.

## Status (verificado rodando no QEMU)
Fundação NT + boot:
- [x] Boot **Multiboot** (32 bits) → **PAE + long mode (64 bits)** + GDT + TSS + SSE
- [x] **IDT** (256 vetores) + exceções + **PIC/PIT** + **teclado PS/2 (IRQ1)** + **mouse PS/2 (IRQ12)**
- [x] **Memória**: PMM (frames) + heap (`kmalloc`) + **paginação dinâmica** (MMIO arena, PML4 por processo, troca de CR3)
- [x] **APIC** (Local APIC + IO-APIC) substituindo o 8259/PIT; **timer LVT** (relógio do sistema)
- [x] **SMP**: MADT + INIT-SIPI-SIPI (2 cores) + **scheduler MP preemptivo** (KTHREAD, quantum, KiSwapContext)
- [x] **Threads ring-3 PREEMPTIVAS**: cada `CreateThread`/`SHCreateThread` vira um KTHREAD que roda em ring 3, escalonado pelo timer lado a lado com a thread principal
- [x] Fundação NT: **DPC**, **KTIMER**, **fast mutex/lookaside**, modelo de interrupção, **KUSER_SHARED_DATA**, **KPCR/GS_BASE**, instrução **SYSCALL** (EFER.SCE), CR4/XCR0 (SMEP/AVX gateados por CPUID)

Executiva NT + Win32:
- [x] **Object Manager** (objetos nomeados, handles, tipos), **Process Manager** (EPROCESS/ETHREAD, PspCidTable), **I/O Manager** (DRIVER_OBJECT, IRPs, `IoCallDriver`)
- [x] **Ring 0 / ring 3** com syscalls (`int 0x80` + `SYSCALL`); **carrega drivers `.sys`** (`DriverEntry`/`DriverUnload`) e roda **`.exe`** em ring 3
- [x] **Loader PE32+/PE32** com **relocações**, **imports por nome e ordinal**, **exports**, **delay-imports**, e **redirect de API Sets** (`api-ms-win-*` → a DLL real correspondente, estilo NT); DLLs de base alta carregam por **PMM + reloc**
- [x] **win32k** (window manager: HWND/z-order/foco, fila de mensagens, compositing, GDI) + **framebuffer** (virtio-gpu 1024x768x32 ou Bochs VBE; fallback VGA mode 13h)
- [x] **HAL** (I/O ports + MMIO + enumeração PCI) + **disco IDE (ATA PIO)** + **driver NTFS** (leitura + escrita residente) montável como **C:**
- [x] Subsistemas espelhando o Win10/11 (stubs de deteção/ABI): **DX** (dxgkrnl + dxgmms), **áudio** (HD Audio), **rede** (NDIS + TCP/IP + E1000), **USB** (usbport + usbhub + xHCI), **ACPI**, **PnP**, **FltMgr**, **virtio-tablet** (cursor absoluto)

DLLs do sistema (PE reais, em `dll/`):
- [x] **ntdll** (única camada de syscall), **kernel32/kernelbase**, **user32**, **gdi32**, **advapi32**, **ucrtbase** (CRT)
- [x] **combase** (COM base + **WinRT/HSTRING/Ro***), **msvcp_win** (STL), **shell32**, **shcore** (+shlwapi), **uxtheme**, **comctl32**, **dui70**, **dwmapi**, **dxgi**, **secur32/credui**, **áudio/rede** (mmdevapi/Audioses/dsound/winmm/ws2_32)

Explorer REAL (a missão) e anti-cheat:
- [x] `explorer.exe` REAL **mapeia+reloca inteiro**, resolve a import table, roda `wWinMain` em **mode 3 (shell)**, cria a Worker Window + o **worker do taskbar** (thread ring-3) e encerra limpo
- [x] `pintok.sys` (**Riot Vanguard**): baseline dourada estável — `[P1]/[P2]/[P3] PROVA PASSOU`, `intercept CPUID/RDTSC/RDMSR/ANTIVM`, `DriverEntry → C0000365` (teto atual), sem "Sistema parado"
- [~] **Persistência do desktop** (fronteira): o worker do shell dá FailFast na init de **DirectUI (dui70) + COM de shell** (objetos ainda genéricos) — implementar por completo destrava o `DesktopExplorerHost`
- [~] **Isolamento por processo parcial** (PML4 copia o mapa do kernel); NTFS **sem alocação de clusters/journaling**; **registro** é stub

## Arquitetura (no estilo Windows NT)
```
   MODO USUARIO (ring 3)                    |   MODO KERNEL (ring 0)
   .exe do Windows (REAL ou nosso)          |   Executiva NT (ntoskrnl) ......... src/ntos/{io,ob,ps,ex,cm,lpc}
     -> user32/gdi32/kernel32/advapi32      |   Loader PE + API Sets ............ src/ntos/ldr
        combase(COM/WinRT)/shell32/shcore   |   Scheduler MP + threads .......... src/ntos/ke (+amd64)
        uxtheme/comctl32/dui70/dxgi/...      |   Memoria (PMM/heap/paging) ....... src/ntos/mm
     -> ntdll.dll  (unica que faz syscall)  |   win32k (window manager) ......... src/subsystems/win32
     --- int 0x80 / SYSCALL ------------------> despacho (SSDT) ................. src/ntos/ke/amd64/syscall.c
   GUI: WNDPROC roda AQUI (ring 3)          |   HAL (PCI/portas/MMIO/disco) ..... src/hal
                                            |   Drivers (video/input/net/usb/fs) . src/drivers/*
                                            |   DX (dxgkrnl/dxgmms) ............. src/subsystems/dx
```
- **DLLs do sistema** (`dll/`) — reimplementadas como **PE reais com export table** (modelo ReactOS). O `.exe` importa delas; só o **ntdll** faz syscall. Redirect de **API Sets**: `api-ms-win-core-*`→kernel32, `api-ms-win-core-com/winrt`→combase, `api-ms-win-shcore/shlwapi-*`→shcore, `api-ms-win-shell-*`→shell32, etc.
- **Loader** (`src/ntos/ldr`) — estilo `LdrLoadDll`: mapeia o PE, resolve cada import (nome/ordinal/delay) pela export table da DLL real, recursivamente; DLLs de ImageBase alta são realocadas via PMM.
- **Scheduler** (`src/ntos/ke`) — KTHREAD com quantum + `KiSwapContext`; o timer APIC (0xD1) chama `ki_quantum_end`. **Threads ring-3 preemptivas**: `CreateThread` do app vira um KTHREAD que trapa na sua própria pilha de kernel (TSS.rsp0) e roda a threadproc em ring 3.
- **win32k** (`src/subsystems/win32`) — o lado kernel do Win32: árvore de janelas, fila de mensagens, compositing sobre o framebuffer. O `DispatchMessage` (no user32, ring 3) chama o WNDPROC — o callback nunca roda em ring 0.

## Estrutura de diretórios
```
src/
  boot/ ............. Multiboot + 32 bits -> long mode (boot.asm) + ISR stubs
  ntos/ ............. a EXECUTIVA NT (o "ntoskrnl")
    ke/  (+amd64) ... scheduler MP, syscalls (SSDT), APIC, SMP, usermode (ring 3), DPC/timer/sync
    mm/ ............. PMM, heap, paginacao (MMIO arena, PML4 por processo), MDL, sections
    io/ ............. I/O Manager (IRPs, DRIVER_OBJECT), PnP
    ob/ ............. Object Manager (objetos nomeados, handles)
    ps/ ............. Process Manager (EPROCESS/ETHREAD, PspCidTable)
    ex/ cm/ lpc/ .... primitivas Ex (mutex/lookaside/callbacks), Registro, LPC
    ldr/ ............ loader PE (parse/map/reloc/imports/exports) + API Sets + ntoskrnl sintetico
  hal/ .............. HAL: I/O ports, MMIO, PCI, disco IDE (ATA PIO)
  subsystems/
    win32/ .......... win32k (window manager + GDI + filas de mensagem)
    dx/ ............. dxgkrnl + dxgmms (DirectX em kernel; adapter display-only)
  drivers/
    video/ display/ . framebuffer VGA + virtio-gpu / Bochs VBE (BasicDisplay)
    input/ .......... teclado PS/2, mouse PS/2, virtio-tablet (cursor absoluto)
    filesystems/ .... driver NTFS (BPB+$MFT, $DATA, $INDEX, leitura+escrita residente) + camada de FS
    network/ ........ NDIS + TCP/IP + E1000 (stub)
    usb/ acpi/ ...... usbport/usbhub/xhci ; ACPI (RSDP scan)
    fltmgr/ serial/ . Filter Manager ; serial COM1

dll/   <- DLLs do sistema (PE reais que o OS EXPOE aos binarios Windows)
  ntdll/ ............ ntdll.dll: a UNICA camada que faz syscalls
  win32/ ............ 30 DLLs: kernel32, user32, gdi32, advapi32, ucrtbase, combase (COM/WinRT),
                      msvcp_win, shell32, shcore, uxtheme, comctl32, dui70, dwmapi, dxgi, d3d*/d2d1/
                      dwrite/dxcore, dsound/mmdevapi/Audioses/winmm, ws2_32, secur32/credui, lsasrv

apps/  <- programas Windows de teste (NAO fazem parte do OS): cmd, guiapp, sysinfo, pipe*, desktop,
          o shell caseiro (csrss/winlogon/explorer), dx demos, drivers de exemplo (.sys), etc.
sdk/   <- headers que o OS EXPOE (o "DDK"): ntddk.h e afins
tools/ <- Zig + QEMU (baixados pelo setup.ps1)
build/ <- saida: kernel.bin/kernel.elf + as DLLs/apps compiladas
```
`build.ps1` varre `src\` recursivamente (compila só `*.c`/`*.asm` — isso é o OS). As
DLLs (`dll/`) e os apps (`apps/`) são compilados **à parte**, com o **alvo Windows**,
virando PEs soltos em `build\`; o kernel os carrega no boot (não os embute).

## O loop de bring-up do explorer real
Disciplina: **build → rodar → diagnosticar (qual import/objeto falta) → implementar de
verdade → repetir**. Ferramentas de RE na raiz (`disrange.py`, `dumpimports.py`,
`whocalls.py`, `callers.py`, …) desmontam o `explorer.exe` real por RVA e listam imports.
O estado detalhado, o mapa de RVAs, os CLSIDs identificados e o próximo muro ficam em
**`PROMPT-PROXIMA-SESSAO.md`**.

## Regra de ouro — `pintok.sys` (anti-cheat)
Toda mudança no **kernel** é validada contra o `pintok.sys` (Riot Vanguard):
```powershell
.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40
```
Baseline dourada: `[P1]/[P2]/[P3] ==== PROVA PASSOU ====`, `intercept totals: CPUID x3
RDTSC x33 RDMSR x0 ANTIVM x0`, `DriverEntry … C0000365`, **sem** "Sistema parado". As
provas P1/P2/P3 (paginação, APIC, SMP) rodam em todo boot e são parte dessa baseline.

## Sistema de arquivos NTFS (opcional, com `-Disk`)
O MeuOS tem HAL de disco (IDE ATA PIO) + driver NTFS (`src/drivers/filesystems`) ligado
ao I/O Manager; o volume é montado como **C:**. Sem `-Disk` o boot roda idêntico (sem NTFS).
```powershell
python .\apps\make-ntfs-image.py build\disk.img     # NTFS sintetico (sem admin)
.\run.ps1 -Headless -Disk -TimeoutSec 14            # anexa build\disk.img como IDE; monta C:
```
Limitações honestas: a escrita **não aloca clusters** (`$Bitmap`) nem tem **journaling**
(`$LogFile`) — só sobrescreve/cresce `$DATA` residente e cria/exclui com índice no pai.

## Requisitos
- Windows 10/11 **x64**. **NASM** (`winget install -e --id NASM.NASM`).
- **Zig** e **QEMU** são baixados pelo `setup.ps1` (Zig sem admin; QEMU via winget).

## Dicas de QEMU
- Serial no terminal: `-serial stdio`; sem janela: `-display none`. O `run.ps1` roda com
  `-smp 2 -accel tcg,thread=multi` e `virtio-gpu` + `virtio-tablet`.
- Debug com GDB: `-s -S` + `target remote :1234` usando `build\kernel.elf` (símbolos).
