# MeuOS — sistema operacional 64-bit, escrito do zero em C

Um kernel de verdade, **bootável**, escrito do zero em C + Assembly. Liga em
modo **32 bits** (via Multiboot), monta paginação, transiciona para **long mode
(64 bits)**, instala interrupções e roda o kernel em C com drivers de vídeo,
serial e teclado. Testável no **QEMU**, que é baixado automaticamente.

## Status (verificado rodando no QEMU)
- [x] Boot Multiboot em modo protegido **32 bits**
- [x] Paginação **PAE** + transição para **LONG MODE (64 bits)** + GDT + SSE
- [x] Kernel em **C 64-bit** (`kmain`)
- [x] Driver de **vídeo VGA** texto + **serial COM1**
- [x] **IDT** (256 vetores) + tratamento de **exceções** (não dá triple-fault)
- [x] **PIC 8259** remapeado + **PIT** (timer IRQ0, 100 Hz)
- [x] **Teclado PS/2 por interrupção** (IRQ1), com eco
- [x] **Memória**: detecção de RAM (Multiboot) + **PMM** (frames) + **heap** (`kmalloc`/`kfree`)
- [x] **Carregador PE32+** + **Win32** própria: **um `.exe` do Windows roda** (resolve imports, chama entry, `MessageBoxA`/`ExitProcess`)
- [x] **Arquitetura NT**: executiva (`ntoskrnl`) + **I/O Manager** que **carrega drivers de kernel `.sys`** (`DriverEntry`, `DRIVER_OBJECT`, `IoCreateDevice`, `DriverUnload`)
- [x] **Modo kernel / modo usuário (ring 0 / ring 3)**: o **`.exe` roda em ring 3** e chama a Win32 via **syscall** (`int 0x80`, igual ao `int 2Eh` do NT); o driver `.sys` roda em ring 0. GDT + TSS.
- [x] **Object Manager**: objetos nomeados (namespace `\Device\`), **handles**, tipos (Device/Driver/File/Process/Thread/...)
- [x] **I/O Manager com IRPs**: `DRIVER_OBJECT.MajorFunction` (dispatch `IRP_MJ_CREATE`/`CLOSE`/`DEVICE_CONTROL`), `IoCreateDevice`, `IoCallDriver`, METHOD_BUFFERED
- [x] **SSDT + syscalls de arquivo/IOCTL**: `NtCreateFile`, `NtDeviceIoControlFile`, `NtClose`
- [x] **DeviceIoControl ponta a ponta** (Teste 2): app ring 3 → `CreateFileA`/`DeviceIoControl`/`CloseHandle` → driver responde a um IOCTL
- [x] **Process Manager (estilo NT)**: **EPROCESS/ETHREAD como objetos** do Object Manager (`Ps*`), **PML4 por processo com troca de CR3** ao entrar/sair do ring 3, syscalls `NtCreateProcess`/`NtCreateThread`/`NtTerminateProcess`/`NtWaitForSingleObject`, `CreateProcessA`/`WaitForSingleObject`; o loader cria um EPROCESS por `.exe`
- [x] **Loader PE32 (32-bit) + relocações + execução em compatibility mode**: `pe_parse` (PE32 e PE32+), `pe_relocate` (`.reloc`: HIGHLOW/DIR64), segmento de código 32-bit na GDT (`SEL_UCODE32`), `usermode_enter32` (ring 3 de 32 bits via `iretq`), `int 0x80` truncando args para 32 bits
- [x] **Console I/O + mais Win32**: `GetStdHandle`/`WriteFile` (→ `NtWriteFile` → "console device") ; `GetModuleHandleA`/`GetProcAddress` (apoiados no loader) ; `NtReadFile` (drena o stdin do teclado), `NtOpenKey`/`NtQueryValueKey` (stubs)
- [x] **Modo gráfico (framebuffer)**: driver de vídeo **VGA mode 13h (320x200x256)** programando os registradores direto, framebuffer em `0xA0000`, **GDI de baixo nível no kernel** (`fb_clear`/`fb_pixel`/`fb_fill_rect`/`fb_rect`/`fb_draw_text`) + fonte 8x8 (FASE 1)
- [x] **GUI estilo Windows (win32k + user32/gdi32)**: window manager (HWND/z-order/foco), **fila de mensagens** (`GetMessage`/`DispatchMessage` chama o WNDPROC em ring 3), objetos GDI (HDC/HBRUSH), syscalls `NtUser*`/`NtGdi*` (17..30, 41..43); `guiapp.exe` pinta no `WM_PAINT` e a **janela aparece no framebuffer** (FASE 2)
- [x] **Named Pipes (IPC)**: `OB_TYPE_PIPE` no Object Manager + namespace `\Pipe\Nome` + buffer 4096; `CreateNamedPipeA`/`ConnectNamedPipe`/`CreateFileA("\\.\pipe\Nome")`; demo servidor→cliente passa 44 bytes (FASE 3)
- [x] **Syscalls de informação + advapi32/SCM**: `NtQuerySystemInformation`/`NtQueryInformationProcess`/`NtRead`/`NtWriteVirtualMemory` (33..36); `advapi32.dll` (SCM stubs que não falham + registro via Native API); demo `sysinfo.exe` (FASE 4)
- [x] **Shell `cmd.exe` (ring 3)**: `help`/`tasklist`/`sc query`/`sc start`/`sc stop`/`dir`; `tasklist` enumera EPROCESS (Object Manager), `sc start`/`sc stop` carrega/descarrega `.sys` real (I/O Manager); syscalls 37..40 (FASE 5)
- [x] **Desktop + barra de tarefas + cmd numa janela**: papel de parede + taskbar no win32k, janelas de console rodando o shell, troca de foco, reaping de janelas por processo; demo `desktop.exe` (FASE 6)
- [x] **HAL (Hardware Abstraction Layer)** estilo NT (`src/hal/`): **I/O ports** (`HalReadPort*`/`HalWritePort*`), **MMIO** (`hal_map_mmio`, recusa faixas >1 GiB), **enumeração PCI** (config space 0xCF8/0xCFC) — `hal_init()` loga os dispositivos achados (host bridge, IDE/ATA, vídeo, e1000...)
- [x] **Disco IDE (ATA PIO, LBA28)** (`src/hal/disk.c`): `IDENTIFY DEVICE` + `HalReadSector`/`HalWriteSector` (canal primário master 0x1F0); cada setor lido/escrito é logado na serial
- [x] **Driver NTFS — LEITURA** (`src/drivers/ntfs.c`): monta o volume (BPB+`$MFT`), parse de registro MFT (`FILE`+fixups USA), `$DATA` residente/não-residente (data runs), listagem de diretório (`$INDEX_ROOT`/`$INDEX_ALLOCATION`), resolução de caminho; **camada de FS no I/O Manager** (`\Device\Harddisk0\Partition1`, IRP_MJ_CREATE/READ/DIRECTORY_CONTROL) + `NtQueryDirectoryFile`
- [x] **Driver NTFS — ESCRITA (subconjunto seguro)** (`src/drivers/ntfs.c`): sobrescrever/crescer/encurtar `$DATA` residente, criar e excluir arquivo/diretório (registro MFT novo + `$INDEX_ROOT` do pai), IRP_MJ_WRITE + `NtWriteFile` — validado por releitura do disco (o SHA-256 da imagem muda)
- [x] **Volume montado como C: + comandos de arquivo no `cmd`** (FASE 5): `NtQueryVolumeInformation`, caminhos `C:\...`; `dir`/`cd`/`type`/`copy`/`vol` ponta a ponta do ring 3 (`del` é stub)
- [~] **Isolamento por processo (parcial)**: a infra de troca de CR3 está de pé, mas a PML4 por processo ainda **copia o mapa de identidade do kernel** (páginas baixas compartilhadas); `NtWaitForSingleObject` não bloqueia (sem escalonador)
- [~] **NTFS escrita (parcial)**: sem **alocação de clusters** (`$Bitmap`) — só escreve onde já há espaço (cresce além do registro / cria `$DATA` grande é recusado com segurança); sem **journaling** (`$LogFile`)
- [ ] **Mouse PS/2 (IRQ12)** — a troca de foco é por `SetFocus`/teclado; botões da barra de tarefas são visuais (sem hit-testing)
- [ ] **Registro com hive real** — `NtOpenKey`/`NtQueryValueKey` são stubs (só `ProductName`/`CurrentVersion`); o SCM são stubs em `advapi32`

## Requisitos
- Windows 10/11 **x64**.
- **NASM** (`winget install -e --id NASM.NASM` se faltar).
- **Zig** e **QEMU** são baixados pelo `setup.ps1` (Zig sem admin; QEMU via winget).

## Início rápido
```powershell
.\setup.ps1                  # baixa Zig + QEMU (uma vez)
.\build.ps1                  # compila -> build\kernel.bin
.\run.ps1                    # roda no QEMU (janela grafica; mode 13h + eco na serial)
.\run.ps1 -Headless -TimeoutSec 10  # teste rapido por serial, encerra sozinho
.\run.ps1 -Headless -Screendump     # captura o framebuffer via QMP -> build\screen.ppm
.\run.ps1 -Headless -Disk -TimeoutSec 14  # anexa build\disk.img (IDE) e monta o NTFS como C:
```

Para o **NTFS** (HAL disco + driver NTFS), use `-Disk` para anexar uma imagem de
disco IDE. Sem `-Disk` o comportamento default (só `-kernel` + `-initrd`) fica
inalterado. Como criar a imagem de teste está em **"Sistema de arquivos NTFS"**
mais abaixo.

## Estrutura
```
src/
  arch/
    boot.asm ........ Multiboot + 32 bits -> long mode 64 bits (AOUT kludge)
    isr.asm ......... stubs de interrupcao (exceptions 0..31, IRQs 32..47)
  hal/  (Hardware Abstraction Layer, estilo HAL.DLL do NT)
    hal.c/.h ........ I/O ports (HalReadPort*/HalWritePort*) + MMIO (hal_map_mmio) + enumeracao PCI (0xCF8/0xCFC)
    disk.c/.h ....... disco IDE (ATA PIO, LBA28): IDENTIFY + HalReadSector/HalWriteSector (canal primario master)
  ke/   (Kernel core, estilo "Ke" do NT)
    gdt.c/.h ........ GDT (segmentos ring 0 e ring 3) + TSS
    syscall.c/.h .... despacho de syscalls (int 0x80); mapeia C: -> \Device\Harddisk0\Partition1
    usermode.c/.h ... entra em ring 3 (iretq) e roda codigo de modo usuario
  cpu/
    idt.c/.h ........ Interrupt Descriptor Table (IDT) + gate de syscall (DPL3)
    isr.c/.h ........ despacho de excecoes, IRQs e syscalls
    pic.c/.h ........ PIC 8259 (remap + EOI)
    pit.c/.h ........ timer PIT (IRQ0)
  drivers/
    vga.c/.h ........ video texto VGA (0xB8000)
    video.c/.h ...... framebuffer grafico VGA mode 13h (320x200x256) + GDI de baixo nivel (fb_*)
    font8x8.c ....... fonte bitmap 8x8 embutida (ASCII 0x20..0x7F)
    serial.c/.h ..... serial COM1
    keyboard.c/.h ... teclado PS/2 por IRQ1 (eco/console OU roteia p/ win32k; fila de stdin)
    ntfs.c/.h ....... driver NTFS: montar BPB+$MFT, parse de registro MFT (FILE+fixups), $DATA (residente/data runs), $INDEX (listar dir), resolver caminho; ESCRITA (sobrescrever/crescer/criar/excluir, sem alocar clusters)
    ntfs_fs.c ....... camada de File System ligada ao I/O Manager: \Device\Harddisk0\Partition1 + IRP_MJ_CREATE/READ/WRITE/DIRECTORY_CONTROL
  mm/
    pmm.c/.h ........ alocador de memoria fisica (frames de 4 KiB)
    heap.c/.h ....... heap do kernel (kmalloc/kfree)
    paging.c/.h ..... bit U/S nas paginas + PML4 por processo (mm_create_address_space, troca de CR3)
  loader/
    pe.c/.h ......... primitivas PE32+/PE32 (parse, mapear, imports, exports, relocacoes)
    loader.c/.h ..... LDR estilo Windows: registra modulos, carrega DLLs (recursivo), roda .exe (32/64-bit), ldr_get_module_bytes
  win32/
    win32.c/.h ...... win32k (base): servicos Win32 do lado kernel (atendem as syscalls)
    win32k.c/.h ..... window manager: arvore HWND/z-order/foco, fila de mensagens, compositing, desktop+barra de tarefas, objetos GDI (HDC/HBRUSH)
  nt/
    object.c/.h ..... Object Manager: objetos nomeados, handles, tipos, enumeracao por tipo (ob_enum_by_type)
    io.c/.h ......... I/O Manager: DEVICE_OBJECT, IRP, IoCreateDevice, IoCallDriver, write/read
    pipe.c/.h ....... Named Pipes: PIPE_OBJECT (buffer circular), namespace \Pipe\Nome, create/open/connect/read/write
    process.c/.h .... Process Manager: EPROCESS/ETHREAD (objetos), Ps* (create/terminate/current)
    ntexec.c/.h ..... executiva "ntoskrnl" (exports p/ drivers: DbgPrint, Io*, Rtl*)
    driver.c/.h ..... carrega .sys, monta DRIVER_OBJECT, chama DriverEntry; registro de drivers (sc start/stop/query)
  include/
    io.h ............ inb/outb (uso interno do kernel)
  kernel.c .......... kmain() + kputs/kput_hex/kput_dec/mem*

sdk/   <- headers que o OS EXPOE para binarios Windows (nao e o OS em si)
  ntddk.h ........... tipos do DDK (UNICODE_STRING, DRIVER_OBJECT, ...), layout x64

dll/   <- DLLs do sistema (reimplementadas), compiladas como PE de verdade
  ntdll.c ........... ntdll.dll: a UNICA camada que faz syscalls (int 0x80)
  kernel32.c ........ kernel32.dll: ExitProcess/CreateProcessA/WriteFile/GetProcAddress/CreateNamedPipeA/Enum*... (p/ ntdll)
  user32.c .......... user32.dll: MessageBoxA + janelas (RegisterClassA/CreateWindowExA/GetMessage/DispatchMessage/SetFocus...)
  gdi32.c ........... gdi32.dll: TextOutA/GetStockObject/CreateSolidBrush/SetTextColor (encaminha p/ win32k via ntdll)
  advapi32.c ........ advapi32.dll: SCM stubs (OpenSCManager/CreateService...) + registro (RegOpenKeyExA/RegQueryValueExA)

examples/   <- programas Windows de teste (NAO fazem parte do OS)
  hello.c ........... .EXE Windows PE32+ (x64): CreateProcessA + WaitForSingleObject + MessageBoxA
  hello32.c ......... .EXE Windows PE32 (32-bit): roda em ring 3 compat mode, int 0x80 direto
  conhello.c ........ .EXE de console: GetStdHandle/WriteFile + GetModuleHandleA/GetProcAddress
  ioctlapp.c ........ .EXE que faz CreateFileA + DeviceIoControl (IOCTL 0xCAFEBABE)
  guiapp.c .......... .EXE GUI: RegisterClassA + CreateWindowExA + loop de mensagens + WM_PAINT (win32k)
  pipeserver.c ...... .EXE servidor de Named Pipe: CreateNamedPipeA + ConnectNamedPipe + WriteFile
  pipeclient.c ...... .EXE cliente de Named Pipe: CreateFileA + ReadFile
  sysinfo.c ......... .EXE NtQuery* + advapi32 (versao do SO, PID/ImageBase, registro, SCM)
  cmd.c ............. .EXE shell (Command Prompt): help/tasklist/sc + dir/cd/type/copy/vol no volume C: (NTFS)
  desktop.c ......... .EXE desktop: papel de parede + barra de tarefas + 2 janelas de cmd (win32k)
  mydriver.c ........ um driver de kernel .sys (subsystem NATIVE)
  ioctldriver.c ..... driver .sys que responde a um IOCTL (0xCAFEBABE)
  make-ntfs-disk.ps1  gera build\disk.img: NTFS REAL (admin: New-VHD/Format-Volume) ou SINTETICO (sem admin, via Python)
  make-ntfs-image.py  builder NTFS sintetico em Python (sem admin): MBR + boot sector NTFS + $MFT minima

linker.ld ........... layout em 1 MiB + simbolos de carga p/ Multiboot
setup.ps1 / build.ps1 / run.ps1
```
`build.ps1` varre `src\` recursivamente (isso é o OS). `examples\` é compilado
**à parte**, com o **alvo Windows**, virando binários PE soltos em `build\` —
o kernel os carrega no boot, não os embute. `sdk\ntddk.h` é o nosso "DDK": os
tipos que o OS oferece a quem escreve drivers (igual o Windows fornece o WDK).

## Arquitetura (no estilo Windows NT)
- **DLLs do sistema** (`dll/`) — `ntdll.dll` / `kernel32.dll` / `user32.dll`
  reimplementadas como **PE reais com export table** (abordagem do ReactOS). O
  `.exe` importa delas; só o **ntdll** faz syscall. Nada de stub hardcoded.
- **Loader** (`loader/`) — estilo `LdrLoadDll`: mapeia o PE e, para cada import,
  **carrega a DLL real e resolve pela export table**, recursivamente (o `.exe`
  puxa `user32`, que puxa `ntdll`...).
- **win32k** (`win32/win32.c` + `win32/win32k.c`) — o lado **kernel** do subsistema
  Win32: os serviços que as syscalls (vindas do ntdll) executam em ring 0. O
  `win32k.c` é o **window manager** gráfico: árvore de janelas (HWND/z-order/foco),
  **fila de mensagens**, **compositing** sobre o framebuffer (mode 13h), desktop +
  barra de tarefas e objetos GDI (HDC/HBRUSH). O ciclo é igual ao do Windows: o
  kernel só **entrega a `MSG`**; o `DispatchMessage` (no `user32`, ring 3) **chama o
  WNDPROC** — o callback do app nunca roda em ring 0.
- **Executiva NT** (`nt/ntexec.c`) — o "ntoskrnl.exe": as APIs que os **drivers
  de kernel** importam (`DbgPrint`, `IoCreateDevice`, `RtlInitUnicodeString`...).
- **I/O Manager** (`nt/driver.c`) — carrega um `.sys`, monta o `DRIVER_OBJECT`
  (layout idêntico ao do NT) e chama `DriverEntry`, depois `DriverUnload`.
- **Modo kernel (ring 0) × modo usuário (ring 3)** (`ke/`) — GDT com segmentos
  de ring 0 e ring 3 + **TSS**. O kernel entra em ring 3 via `iretq`; o código de
  usuário **não toca o kernel direto** — fala só por **syscall** (`int 0x80`, o
  mesmo mecanismo do `int 2Eh` do NT clássico). A subida ring 3 → ring 0 usa a
  pilha `RSP0` do TSS, e o retorno volta pelo `iretq`/`longjmp`.
- Tudo que é binário Windows é chamado em **ABI Microsoft** (`ms_abi`); o kernel
  é compilado em System V.

```
   MODO USUARIO (ring 3)                 |   MODO KERNEL (ring 0)
   aplicativo .exe  <-- roda aqui        |   Executiva NT (ntoskrnl) ...... nt/ntexec
     -> user32 / gdi32 / kernel32        |   I/O Manager (drivers .sys) .... nt/driver
        / advapi32  (PE reais, dll/)     |   Object Manager / Process Mgr .. nt/object,process
     -> ntdll.dll  (PE real, dll/)       |   Named Pipes (IPC) ............. nt/pipe
     --- int 0x80 (syscall) ---------------> despacho .................... ke/syscall
   GUI: WNDPROC roda AQUI (ring 3),      |   win32k: window manager ........ win32/win32k
        chamado por DispatchMessage      |   File System NTFS (IRPs) ....... drivers/ntfs,ntfs_fs
                                         |   Memoria / IDT / GDT+TSS ....... mm/ cpu/ ke/
                                         |   HAL: I/O ports + PCI + disco ... hal/hal,hal/disk
                                         |   VGA texto, framebuffer, serial . drivers/
```

- **Process Manager (estilo NT)** (`nt/process.c`) — `EPROCESS`/`ETHREAD` são
  **objetos do Object Manager** (`Ps*`). Cada `.exe` ganha um EPROCESS (com o seu
  CR3) + a ETHREAD principal; o loader o cria, roda em ring 3 e o termina. Há
  uma **PML4 por processo** (`mm_create_address_space`) e a **troca de CR3**
  acontece ao entrar/sair do ring 3 (`usermode_enter`).
- **Subsistema gráfico (GUI)** (`drivers/video.c` + `win32/win32k.c`) — driver de
  vídeo em **framebuffer (VGA mode 13h, 320x200x256)** com GDI de baixo nível no
  kernel (`fb_*`) + window manager (HWND/z-order/foco, fila de mensagens,
  compositing, desktop + barra de tarefas). As DLLs `user32`/`gdi32` (ring 3)
  expõem `CreateWindowExA`/`GetMessage`/`DispatchMessage`/`TextOutA`/`FillRect`...;
  o ciclo de mensagens segue o modelo do Windows (o WNDPROC roda em ring 3).
- **IPC — Named Pipes** (`nt/pipe.c`) — `OB_TYPE_PIPE` no Object Manager
  (namespace `\Pipe\Nome`, buffer circular); um handle de pipe é um `FILE_OBJECT`
  roteado, então `CreateNamedPipeA`/`CreateFileA("\\.\pipe\Nome")`/`ReadFile`/
  `WriteFile` movem bytes entre processos no mesmo boot.
- **HAL (Hardware Abstraction Layer)** (`hal/hal.c` + `hal/disk.c`) — a camada de
  abstração estilo HAL.DLL do NT (funções `Hal*` em `ms_abi`): **portas de I/O**
  (`HalReadPort*`/`HalWritePort*`), **MMIO** (`hal_map_mmio`, que devolve ponteiro
  identidade para faixas < 1 GiB e **recusa** acima — caminho seguro, sem mexer nas
  page tables) e **enumeração PCI** pelo config space (0xCF8/0xCFC): `hal_init()`
  varre o barramento e loga cada dispositivo (host bridge, **IDE/ATA**, vídeo,
  e1000...). O **disco** (`hal/disk.c`) é IDE por **ATA PIO, LBA28** (canal primário
  master): `IDENTIFY DEVICE` + `HalReadSector`/`HalWriteSector` — a base do NTFS.
- **Sistema de arquivos NTFS** (`drivers/ntfs.c` + `drivers/ntfs_fs.c`) — um
  **File System Driver** de verdade ligado ao I/O Manager. `ntfs.c` monta o volume
  (BPB + `$MFT`), faz o parse de registros MFT (`FILE` + fixups USA), lê `$DATA`
  (residente e não-residente via data runs), lista diretórios (`$INDEX_ROOT` /
  `$INDEX_ALLOCATION`), resolve caminhos e **escreve** (sobrescrever/crescer `$DATA`
  residente, criar/excluir arquivo/diretório — sem alocar clusters). `ntfs_fs.c`
  registra `\FileSystem\Ntfs` e um device de volume `\Device\Harddisk0\Partition1`
  com `DRIVER_OBJECT.MajorFunction` para **IRP_MJ_CREATE/READ/WRITE/DIRECTORY_CONTROL**.
  O volume é exposto ao ring 3 como a unidade **C:** (`NtCreateFile`/`NtReadFile`/
  `NtWriteFile`/`NtQueryDirectoryFile`/`NtQueryVolumeInformation`); o `cmd.exe` faz
  `dir`/`cd`/`type`/`copy`/`vol` ponta a ponta. Detalhes e como criar a imagem de
  teste: seção **"Sistema de arquivos NTFS"** abaixo.
- **Falta para ser NT completo**: o isolamento por-processo é **parcial** — a
  PML4 por processo ainda **copia o mapa de identidade do kernel** (a pilha e as
  DLLs em páginas baixas seguem compartilhadas); o próximo passo é dar páginas
  privadas por processo (a infra de troca de CR3 já está de pé). Faltam ainda:
  **escalonador / preempção** (`NtWaitForSingleObject` não bloqueia; a fila de
  mensagens do win32k é global; a demo de pipe é sequencial); **driver de mouse
  PS/2** (a GUI tem teclado/foco, mas o clique na barra de tarefas é visual);
  **NTFS com alocação de clusters (`$Bitmap`) + journaling (`$LogFile`)** — hoje a
  escrita só usa espaço já alocado (criar arquivo grande exige o caminho de
  alocação) — e registro com hive real (o `cmd` já faz `dir`/`type`/`copy` num
  volume NTFS de verdade; o registro segue stub); e DLLs do sistema em 32-bit para
  o `pinball.exe` real (o loader já carrega/reloca/executa **PE32 em compat mode**,
  mas as DLLs do sistema ainda são 64-bit). A espinha (ring 0/3 + syscalls +
  **DLLs reais + loader recursivo (PE32 e PE32+)** + executiva + I/O Manager +
  **Process Manager** + **modo gráfico + GUI (win32k)** + **IPC** + **HAL (PCI +
  disco)** + **File System NTFS** + modelo de driver) já está de pé.

## Como o boot funciona
1. QEMU `-kernel` lê o cabeçalho **Multiboot** (nos primeiros 8 KiB). Como ele
   recusa ELF64, usamos o **AOUT kludge** + um **binário plano** (`kernel.bin`,
   via `zig objcopy`) com os endereços de carga no cabeçalho.
2. `boot.asm` (32 bits): valida CPUID/long-mode, monta PML4/PDPT/PD (identidade
   de 1 GiB), liga PAE + EFER.LME + paginação, e faz *far-jump* para 64 bits.
3. `boot.asm` (64 bits): habilita SSE e chama `kmain()`.
4. `kmain()`: VGA + serial + **IDT** + **PIC/PIT** + teclado por IRQ; `sti`.

## Sobre "rodar o Pinball" (importante)
O **Pinball original da Microsoft** (`PINBALL.EXE`, 3D Space Cadet) é um programa
**Windows (Win32)**: ele depende da API inteira do Windows (Win32/GDI/DirectX).
Rodar esse `.exe` aqui significaria **reimplementar o Windows** — é o que o Wine
e o ReactOS fazem, anos de trabalho. **Não dá** num kernel próprio.

Os caminhos reais, do zero, para ter Pinball **no MeuOS**:
- **(A) Pinball nativo nosso** — escrever um jogo de pinball direto sobre o
  kernel (framebuffer + física + flippers + bola, controle por teclado). É o
  caminho recomendado: vira jogável rápido e é 100% "do zero".
- **(B) Portar o Space Cadet open-source** (reimplementação `SpaceCadetPinball`,
  C++/SDL2) — dá o jogo "de verdade", mas exige antes construir uma base enorme
  no OS: libc/libc++, uma camada tipo SDL (vídeo/áudio/input), heap, sistema de
  arquivos para os dados do jogo. Meses de desenvolvimento.

**Modo gráfico — FEITO:** o MeuOS já sai do modo texto e tem **framebuffer**
(VGA *mode 13h*, 320x200, 256 cores, programando os registradores direto, desenho
em `0xA0000`) com GDI de baixo nível, um **window manager** (win32k) e um
**desktop com barra de tarefas** (FASES 1, 2 e 6). O próximo passo para o Pinball
é **resolução maior** (VBE/LFB linear) + áudio/input e física do jogo.

## Roadmap (meta: rodar um `.exe` do Windows, rumo ao PINBALL.EXE)
1. [x] IDT + exceções
2. [x] PIC + PIT + teclado por IRQ
3. [x] Memória: PMM (frames) + heap (`kmalloc`/`kfree`)
4. [x] **Carregador PE32+ e PE32** (parse, seções, imports, **relocações** + execução de 32-bit em compat mode)
5. [x] **Win32 própria** (resolve imports por nome; ABI `ms_abi`)
6. [x] **Rodar um `.exe` Windows nosso** (`MessageBoxA`/`ExitProcess`)
7. [x] **Arquitetura NT**: executiva `ntoskrnl` + I/O Manager + **carrega driver `.sys`**
8. [~] **Crescer Win32/ntoskrnl**: `GetStdHandle`/`WriteFile` (console device) + `GetModuleHandle`/`GetProcAddress` + IRP_MJ_WRITE/READ + `NtQuery*` + `advapi32`/SCM + Named Pipes feitos; falta `HeapAlloc`, mais Io*/Ke*
9. [~] **Process Manager (ring 3)**: EPROCESS/ETHREAD (objetos) + **PML4 por processo / troca de CR3** + `NtCreateProcess`/`NtCreateThread`/`NtTerminateProcess`/`NtWaitForSingleObject` feitos; falta isolamento total (páginas privadas) e escalonador (wait não bloqueia)
10. [x] **Modo gráfico** (framebuffer VGA mode 13h) + **GDI** + **win32k** (window manager) + **desktop/barra de tarefas** + shell `cmd` (FASES 1/2/5/6); falta **DirectDraw** e resolução maior (VBE/LFB) p/ o render do Pinball
11. [~] **HAL + armazenamento + sistema de arquivos**: **HAL** (PCI + I/O ports + MMIO) + **disco IDE (ATA PIO)** + **driver NTFS** (leitura + escrita residente) montado como **C:** feitos; falta **alocação de clusters (`$Bitmap`) + journaling (`$LogFile`)** e DLLs do sistema em 32-bit
12. [~] Áudio (**DirectSound**) + input + **registry**: input por teclado feito (a GUI roteia para a janela com foco); **registry é stub** (`ProductName`/`CurrentVersion`); falta áudio e mouse (IRQ12)
13. [ ] **PINBALL.EXE** rodando

> O `.exe` **já roda em ring 3** (cada um com seu EPROCESS + CR3) e o OS já tem
> **modo gráfico + GUI (win32k) + desktop + shell cmd** (passo 10). Falta o
> isolamento total por processo + escalonador (passo 9) e, para o Pinball,
> **resolução maior (VBE/LFB) + DirectDraw/áudio** — que é o que ele realmente exige.

## Sistema de arquivos NTFS (HAL disco + driver NTFS)
O MeuOS tem uma **HAL** (`src/hal/`) que enumera o PCI e dirige o disco IDE por
**ATA PIO (LBA28)**, e um **driver NTFS** (`src/drivers/ntfs.c` + `ntfs_fs.c`)
ligado ao I/O Manager. O volume NTFS é montado como a unidade **C:** e o `cmd.exe`
faz `dir`/`cd`/`type`/`copy`/`vol` de verdade. Tudo é logado na serial (`[hal]`,
`[disk]`, `[ntfs]`/`[ntfs.sys]`) para comprovar em headless.

O QEMU aqui usa `-kernel` + `-initrd` (sem disco por padrão); para o NTFS é preciso
**anexar uma imagem de disco** com `run.ps1 -Disk`.

### 1) Criar a imagem NTFS de teste
Duas formas (o script escolhe sozinho conforme você seja ou não Administrador):

```powershell
# (A) NTFS REAL do Windows  — precisa de PowerShell ELEVADO (Administrador) + Hyper-V:
#     New-VHD -> Mount-VHD -> New-Partition -MbrType IFS -> Format-Volume NTFS
#     -> copia \hello.txt e \dir1\file.txt -> qemu-img convert -O raw -> build\disk.img
.\examples\make-ntfs-disk.ps1

# (B) NTFS SINTETICO (sem admin) — constroi os bytes na mao (MBR + boot sector
#     "NTFS    " + $MFT minima). Suficiente para a leitura/escrita residente:
python .\examples\make-ntfs-image.py build\disk.img
```

Use o modo **(A)** (volume 100% autêntico, com `$LogFile`/`$UpCase`/`$INDEX_ALLOCATION`
reais) para exercitar os caminhos de `$DATA` **não-residente** e diretório grande
(`INDX`). O modo **(B)** roda sem privilégios e valida a montagem + a leitura/escrita
de `$DATA` residente. Se `build\disk.img` faltar (ex.: após `build.ps1 -Clean`), o
`run.ps1 -Disk` **gera a imagem sintética automaticamente**.

### 2) Rodar com o disco anexado
```powershell
.\build.ps1
.\run.ps1 -Headless -Disk -TimeoutSec 14    # anexa build\disk.img como IDE; monta o NTFS como C:
.\run.ps1 -DiskImage C:\caminho\meu.img     # usa uma imagem específica (implica -Disk)
# Depois de testar:  Get-Process qemu* | Stop-Process -Force
```

No boot com `-Disk` você verá: a HAL achar o controlador IDE (`8086:7010`), o
`IDENTIFY` do disco, a leitura do **MBR** + **boot sector NTFS** (`"NTFS    "` @3),
a montagem do volume (`$MFT` registro #0 com fixups), a listagem da raiz, a leitura
de `\hello.txt` e os testes de **escrita** (sobrescrever/crescer/criar/excluir — o
SHA-256 de `build\disk.img` muda após o boot, provando que persistiu). Sem `-Disk`,
o boot fica idêntico ao default (sem montar NTFS; o `cmd` avisa que não há volume).

> **Limitações atuais (honestas):** a escrita **não aloca clusters** (`$Bitmap`) nem
> tem **journaling** (`$LogFile`) — só escreve onde já há espaço (sobrescrever/crescer
> `$DATA` residente, overwrite de não-residente in-place, criar/excluir com índice no
> `$INDEX_ROOT` do pai); crescer além do registro MFT ou criar `$DATA` grande é
> **recusado com segurança** (sem corromper). O espaço livre do `vol` é estimativa.

## Como um binário Windows é carregado (qualquer um, não só os exemplos)
1. O binário (`.exe` ou `.sys`) é passado ao QEMU como **módulo de boot**
   (`-initrd`) — o `run.ps1` faz isso. **Nada é embutido no kernel.**
2. `kmain()` lê os módulos do Multiboot e, para cada um, olha o **Subsystem**
   do PE: NATIVE → driver (`nt/driver.c`); senão → app (`pe_run`, via `win32/`).
3. `loader/pe.c` faz o parsing (DOS/PE/seções), mapeia no ImageBase, percorre a
   **import table** e resolve cada import **pelo nome** (Win32 ou ntoskrnl),
   depois chama o entry em **ABI Microsoft** (`__attribute__((ms_abi))`).

Para rodar o SEU programa:
```powershell
.\run.ps1 -Modules C:\caminho\app.exe
```
O loader lista cada import; os que aparecem como **"NAO IMPLEMENTADO"** são as
APIs que faltam implementar em `win32/` ou `nt/` para aquele programa rodar.
Ou seja: o OS **não** é feito sob medida para os exemplos — ele roda qualquer
PE; o que cresce é a quantidade de APIs do Windows que já existem.

## Dicas de QEMU
- Serial no terminal: `-serial stdio`; sem janela: `-display none`.
- Debug com GDB: `-s -S` e `target remote :1234` usando `kernel.elf` (símbolos).
