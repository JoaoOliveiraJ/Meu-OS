# Changelog

Todas as mudanças relevantes do projeto. Formato baseado em
[Keep a Changelog](https://keepachangelog.com/pt-BR/1.0.0/) e
[Versionamento Semântico](https://semver.org/lang/pt-BR/).

## [0.11.0] - 2026-07-04 — GUI moderna (Windows 10/11) + multitarefa preemptiva + SMP

### Adicionado — Interface gráfica true-color

- **Camada de desenho 32 bpp** no win32k (antes tudo passava por uma paleta de
  16 cores do mode13h). Novas primitivas em `gpu.c`:
  - `gpu_gradient_v` — gradiente vertical (uma cor por linha; barato em tela cheia)
  - `gpu_blend_rect` — alpha-blend (base de sombras / "acrílico" da taskbar)
  - `gpu_get_pixel` — leitura de pixel para composição
- **Compositor Windows 10/11** (`win32kshell.c`, caminho `win32k_shell_compose_full`):
  - Wallpaper com gradiente azul (duas metades, brilho no meio)
  - Janelas com **drop-shadow**, caption em **gradiente de acento** (foco) e
    título 2× nítido, área cliente branca (#F3F3F3), borda de acento
  - Botões de legenda **[_] [ ] [X]** (X com hover vermelho #E81123)
  - **Taskbar acrílica** (blend escuro), botão Iniciar (logo de 4 ladrilhos),
    botões de app com realce de acento, **relógio HH:MM:SS ao vivo**
  - **System tray de SMP**: chips dos 2 núcleos + heartbeat do AP ao vivo
- Caption bar subiu para 28px (`TITLE_H`); título desenhado UMA vez (fim do
  texto duplicado). App content agora é publicado (present) quando o message
  loop fica ocioso, e **re-invalidado (WM_PAINT)** após cada compose.

### Corrigido — Cursor do mouse invisível

- Trocado o cursor de HARDWARE do virtio-gpu por um **cursor de SOFTWARE** com
  save-under (`win32kbase.c`: `cursor_show`/`cursor_hide`/`win32k_cursor_repaint`).
  Motivo: o cursor de HW do virtio-gpu só aparece com `-display gtk,gl=on` no host
  Windows; sem isso ele fica **invisível**. O cursor de software é desenhado no
  próprio framebuffer, então aparece em QUALQUER modo de display.
- Fluidez preservada: `gpu_present_rect` / `virtio_gpu_present_rect` publicam SÓ o
  retângulo do cursor a cada movimento (nada de recompor a tela inteira). Logs de
  transfer/flush ficam silenciosos nesse caminho (não floodam a serial).

### Adicionado — Multitarefa preemptiva (scheduler MP RETOMADO)

- **Pilar 4 religado**: o LVT timer do BSP volta a rodar (relógio/`g_ticks` ao
  vivo) e a preempção (`ki_quantum_end`) é ativada com `g_p4_active=1`.
- **KTHREADs de kernel reais** escalonadas por quantum via `KiSwapContext`:
  3 workers de demonstração + 1 thread de **refresh do desktop** (relógio ao
  vivo, ~2 Hz) + o fluxo do boot/desktop (idle thread).
- Correção no `sched.c`: a thread idle (que hospeda o fluxo do boot/desktop)
  agora participa do rodízio — antes seria inanida pelos workers e o desktop
  nunca subiria.
- Preempção é ligada **só quando o shell persistente (explorer.exe) sobe**, então
  todo o carregamento pesado de drivers/apps roda a toda velocidade.

### Adicionado — SMP (2º núcleo em paralelo REAL)

- O **AP (CPU 1)** roda um worker loop dedicado que executa trabalho de ALU real
  (hash LCG) e publica um **heartbeat** em memória compartilhada. O BSP lê e
  mostra na taskbar — o número só cresce se o 2º núcleo está executando ao mesmo
  tempo. Prova de SMP genuíno (dois cores simultâneos).
- **Limitação documentada**: sob QEMU TCG (sem WHPX/KVM neste host), desmascarar
  um SEGUNDO LAPIC timer periódico congela deterministicamente o BSP no boot
  (confirmado por teste). Por isso o AP não usa o timer local — não há preempção
  por-core no 2º núcleo. Em hardware real / WHPX bastaria `apic_unmask_timer_local()`
  em `ap_entry` (o `ki_quantum_end` já roda em qualquer CPU).

## [0.10.0] - 2026-06-26 — pintok.sys DECIFRADO

 (``/`pintok.sys`) decifrado
byte-a-byte por emulação isolada com Unicorn**. Não é mais "decoy / parede de
design" — pintok.sys é uma pintok.sys funcional COMPLETA com callbacks ativos,
inspeção de processos, indexação por BootIdentifier GUID, persistence via
registry (`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\BuildGUIDEx`).

### Adicionado

- **`pintok.sys-GATE-CHAIN.md`** — Gate do DriverEntry desmontado byte-a-byte:
  - Input em `[rsp+0x560] = 0x8298d4856579546b`
  - Cadeia: `~ROL(input, 19)` → mux MBA `(A | x) & (B | ~x)` → `~esi[31:0]` = accumulator
  - Accumulator `0xa7db5aad` (bit 2 = 1) → status `0xC0000022 ACCESS_DENIED`
  - Imul gate: `0x140c3e6de imul rax, rdx` onde rdx = bit2 flag derivada
  - **FLAG_ZERO injection**: `rdx ← 0` no imul → RAX=0 SUCCESS limpo
  - Lifecycle completo (DriverEntry + Egg + DriverUnload) emulado: 6.83M instrs

- **`pintok.sys-pintok.sys-MAP.md`** — Mapa completo do pintok.sys:
  - 4 callbacks registrados (`PsSetCreate{Process,Thread,LoadImage}Notify*`)
  - ProcessNotifyEx: 786K instrs com inspeção real (PsGetProcess*, ZwOpenKey,
    RtlRandomEx × 64, ZwSetValueKey, `ZwQuerySystemInformation(0x5A)` =
    SystemBootEnvironmentInformation, KeStackAttachProcess, KeUnstackDetachProcess)
  - **DESCOBERTA CRUCIAL**: pintok.sys indexa state por `BootIdentifier GUID` —
    explica anti-VM (snapshot reverte = mesmo GUID = replay detection)
  - **DESCOBERTA CRUCIAL**: pintok.sys USURPA `HKLM\SOFTWARE\Microsoft\Windows NT\
    CurrentVersion\BuildGUIDEx` para state criptografado (REG_BINARY)
  - Inner Thread handler `0x1400d3493` (CLEAN code, sem VMProtect!) decifrado:
    XOR-decryption + range checks + detection bit BTS em `[rip - 0x270fb]`
  - Encrypted fn ptr tables em `IB+0xb65f0/0xb62d0/0xb6668` (populated runtime)
  - 64 chamadas RtlRandomEx = 256 bytes entropy per process create

- **`EMU-INJECTION-COOKBOOK.md`** — Reference completo do `scratchpad/emu.py`:
  - 30+ env vars documentadas (FAKE_PROCESS_NAME, FAKE_BOOT_GUID, FAKE_REG_DATA, etc)
  - Stages do CONTINUE_PAST_DE + INVOKE_CALLBACKS (7-state machine)
  - HLE side-effects: KEVENT, KTIMER, KDPC, ExCreateCallback, etc.
  - Hooks PSCALL_REGISTER (flip rdx low byte → pintok.sys REGISTRA), REGISTER_VIA_REMOVE,
    INVOKE_BUGCHECK
  - Receita ready-to-go pra reproduzir todos os achados

- **`MEUOS-pintok.sys-READY-RECIPE.md`** — Checklist pro MeuOS:
  - 69 APIs ntoskrnl via IAT
  - 217 APIs resolved-by-name via export table
  - 5 hard walls (anti-VM timing, self-hash, BootIdentifier, registry, EPROCESS)
  - Lista de callbacks que pintok.sys vai registrar
  - Sinais de validação no log

- **`scratchpad/emu.py`** v30+: ~600 linhas adicionais com:
  - Side-effects reais (Ps*Notify*, Cm*, Ob*, Io*, Ke*, Ex*)
  - PSCALL_WATCH + PSCALL_REGISTER (flipa rdx pra REGISTER, não Remove)
  - REGISTER_VIA_REMOVE (PsRemove* re-interpretado como Register no HLE)
  - CONTINUE_PAST_DE (DriverEntry → Egg → DriverUnload pipeline)
  - INVOKE_CALLBACKS (4 callbacks invocados com synthetic args)
  - INVOKE_BUGCHECK (5º callback invocado)
  - HLE realista de PsGetProcessImageFileName/SessionId/SectionBaseAddress
  - HLE de ZwOpenKey/QueryValueKey/SetValueKey (loga key + value paths)
  - HLE de ZwQuerySystemInformation class 0x5A (BootIdentifier GUID)
  - FAKE_REG_DATA injection (2-stage size query + buffer read)
  - DriverObject post-mortem dump (DriverUnload addr, MajorFunction, KTIMER state)
  - 233-ptr resolved-fn scan + nearest-export mapping

### Confirmado

- **pintok.sys não precisa de vgc.exe** pra fazer detection (intuição inicial do LO confirmada)
- pintok.sys em **isolation registra 4 callbacks** + executa toda a inspeção real,
  mas não TOMA ações (ZwTerminate, BugCheck) porque whitelists/state encrypted
  ficam zeradas — requer Riot-signed blob no registry
- O caminho pro MeuOS é claro: implementar APIs + GUID estável + registry +
  EPROCESS realista; ou injetar FLAG_ZERO via hook (cheating mas funcional)

### Limitações honestas

- Em isolação **pintok.sys não chama** `ObRegisterCallbacks`, `CmRegisterCallback`,
  `IoCreateDevice` (estão atrás de um gate upstream que requer Riot provisioning)
- Process name (svchost.exe, Valorant.exe, Cheat Engine.exe) **não muda
  comportamento** em isolation — tabela de comparação é zerada
- Egg export é near-noop em isolation (10.9K instrs, retorna 0)
- BCryptVerifySignature NUNCA é chamado em isolation (verification blob path
  unreachable)

## [0.9.0] - 2026-06-22

Nona versão. **Stack Windows 10/11 essencialmente completo** — 6 sub-fases
sequenciais via workflow entregando mouse PS/2, audio, networking, USB+ACPI+
PnP+FltMgr, security/logon/subsistema, e app de teste final. **18 DLLs
Win10/11** funcionais + 11 drivers novos + 3 apps (csrss, winlogon, win10ui).
Build verde, boot verde, 24 `[ok]`, calller.sys 100% intacto.

**Nota:** A Fase 11.2 (Desktop Win10 cosmético — window chrome com
close/min/max buttons, mouse drag, Start menu popup) **falhou com API 500**
durante o workflow. Outras 5 fases entregues OK. O Desktop visual está
funcional via `winlogon.exe` (tela de logon Aero) + `win10ui.exe` (janela
Win10 mock); polish cosmético pode ser feito em rodada futura.

### Adicionado

- **FASE 11 — Mouse PS/2 + integração win32k**
  *(`src/drivers/input/mouse/{mouse.c,h}`)*:
  - State machine 3-byte (byte0 buttons+signs+overflow, byte1 dX, byte2 dY).
  - IRQ 12 handler (vetor 32+12=44 apos pic_remap), drain do i8042.
  - Setup: outb(0x64, 0xA8) Aux Enable, cmd 0xD4 + 0xF6 Set Defaults + 0xF4
    Enable Stream (ACK 0xFA). Unmask IRQ12 no slave PIC + IRQ2 cascade no master.
  - `src/ntos/ke/amd64/isr.c`: dispatch IRQ12 -> `mouse_irq()`.
  - `win32k_on_mouse_event(dx, dy, buttons)`: atualiza `s_cursor_x/y` com clamp
    1024x768, hit-test HWND, post `WM_MOUSEMOVE` + `WM_LBUTTONDOWN/UP` + RBUTTON
    + MBUTTON pra janela alvo, foco em click L.
  - Cursor sprite 12x16 (seta clássica NT preto/branco) desenhado em
    `win32k_compose()` apos taskbar.
  - Syscalls **46 (NtUserGetCursorPos)** + **47 (NtUserSetCursorPos)** com
    `POINT {int x, y}` layout. Sincronizadas em `ntdll.{c,def}` e
    `user32.{c,def}` (`GetCursorPos`/`SetCursorPos`).
  - *Limitação honesta*: QEMU headless (`-display none`) não injeta IRQ12 via
    QMP `input-send-event` nem `mouse_move`. Mouse foi PROVADO ponta-a-ponta
    via smoke test sintetico (4 chamadas a `win32k_on_mouse_event` -> cursor
    se move + sprite renderizado pixel-a-pixel no screen.ppm em (542, 414)).
    Com `-display gtk/sdl` interativo, IRQ12 fluiria normalmente.

- **FASE 11 (audio) — DirectSound + WASAPI + MM**
  *(4 novas DLLs ring 3 + driver HDAudio kernel)*:
  - `dll/win32/mmdevapi/{mmdevapi.c,def}`: `IMMDeviceEnumerator` Vista+ COM.
    Exports: `CoCreateInstance`, `MMDevApiCreateEnumerator`,
    `ActivateAudioInterfaceAsync`.
  - `dll/win32/Audioses/{audioses.c,def}`: WASAPI `IAudioClient/RenderClient/
    CaptureClient`. Exports: `AudiosesCreateAudioClient`, `DllGetClassObject`.
  - `dll/win32/dsound/{dsound.c,def}`: `IDirectSound8/Buffer`. Exports:
    `DirectSoundCreate/Create8`, `DirectSoundCaptureCreate8`,
    `DirectSoundEnumerateA`.
  - `dll/win32/winmm/{winmm.c,def}`: legacy MM. Exports: `PlaySoundA/W`,
    `waveOutOpen/Write/Close`, `timeGetTime`, `mciSendStringA`, `midiOutOpen`.
  - `src/drivers/audio/HDAudio/{HDAudio.c,h}`: detect Intel HD Audio via PCI
    `class=0x04 sub=0x03`. QEMU achou: `vendor=0x8086 device=0x2668
    (intel-hda) BAR0=0xFEBF0000`.

- **FASE 12 — Networking stack (NDIS + TCP/IP + Winsock + e1000)**
  *(`src/drivers/network/{ndis,tcpip,e1000}/` + `dll/win32/ws2_32/`)*:
  - **NDIS framework** (`ndis.{c,h}`): pools estaticos 8 miniports + 4 protocolos.
    10 APIs ms_abi expostas pelo ntoskrnl: `NdisInitializeWrapper`,
    `NdisMRegisterMiniportDriver`, `NdisRegisterProtocolDriver`,
    `NdisAllocate/FreeNetBufferList`, `NdisMSendNetBufferListsComplete`,
    `NdisFree/AllocateMemoryWithTag`, `NdisMResetComplete`, `NdisGetVersion`.
  - **TCP/IP** (`tcpip.{c,h}`): cria `\Device\{Tcp,Udp,Ip,RawIp}` (TDI/WSK
    stub) + registra como protocolo NDIS via `ndis_register_protocol("Tcpip")`.
  - **e1000 driver** (`e1000.{c,h}`): detect Intel 82540EM-A via PCI
    `vendor=0x8086 device=0x100E` em bus 0:5.0. Registra na NDIS como miniport.
  - **Winsock 2.2** (`ws2_32.dll`, ~40 exports): `WSAStartup/Cleanup`,
    `socket/bind/listen/accept/connect`, `send/recv/sendto/recvfrom`, `select`,
    `getsockname/getpeername`, `getsockopt/setsockopt/ioctlsocket`,
    `htons/ntohs/htonl/ntohl`, `inet_addr/ntoa`, `gethostbyname/byaddr/hostname`,
    `getaddrinfo/freeaddrinfo`, `WSARecv/Send`, `WSA*Event`. Pool 32 sockets.
  - QEMU agora roda com `-netdev user,id=net0 -device e1000,netdev=net0`.

- **FASE 13 — USB + ACPI + PnP Manager + Filter Manager**
  *(4 subsistemas kernel adicionais)*:
  - **USB stack** (`src/drivers/usb/`):
    - `usbport/{c,h}`: framework com tabela 8 HCDs, `usbport_register_hcd`.
    - `usbhub/{c,h}`: hub class driver.
    - `xhci/{c,h}`: detect PCI `class=0x0C sub=0x03 prog_if=0x30` (prefere xHCI >
      EHCI > OHCI > UHCI). QEMU achou `qemu-xhci` em PCI 0:6.0
      `vendor=0x1B36 device=0x000D BAR0=0xFEBF4000`.
  - **ACPI** (`src/drivers/acpi/{acpi.c,h}`): scan BIOS area `[0xE0000,0xFFFFF]`
    procurando `"RSD PTR "`. Achou em `0xF52C0`, `OEM='BOCHS '`, `RSDT=0xFFE22DB`.
    Sem AML interpreter (stub).
  - **PnP Manager** (`src/ntos/io/{pnp.c,h}`): 23 sub-códigos `IRP_MN_*`
    (START_DEVICE..SURPRISE_REMOVAL), `pnp_dispatch` safe path retorna SUCCESS.
    Exports: `IoInvalidateDeviceState`, `IoReportDeviceObject`.
  - **Filter Manager** (`src/drivers/fltmgr/{c,h}`): pool 8 filtros + 16 ports.
    APIs ms_abi: `FltRegisterFilter`, `FltStartFiltering`, `FltUnregisterFilter`,
    `FltCreateCommunicationPort`, `FltSendMessage`, `FltSetCallbackDataDirty`.
  - QEMU agora roda com `-device qemu-xhci,id=xhci0` (USB 3.0 visivel no PCI).

- **FASE 14 (RODADA FINAL) — Security + winlogon + Csrss + win10ui**
  *(2 DLLs + 3 apps)*:
  - **secur32.dll** (30 exports SSPI+LSA): `AcquireCredentialsHandleA/W`,
    `InitializeSecurityContextA/W`, `AcceptSecurityContext`,
    `DeleteSecurityContext`, `QueryContextAttributesA/W`, `EncryptMessage/
    DecryptMessage`, `MakeSignature/VerifySignature`, `CompleteAuthToken`,
    `ApplyControlToken`, `ImpersonateSecurityContext`, `RevertSecurityContext`,
    `LsaConnectUntrusted`, `LsaRegisterLogonProcess`, `LsaLookupAuthenticationPackage`,
    `LsaCallAuthenticationPackage`, `LsaLogonUser`, etc.
  - **credui.dll** (14 exports): `CredUIPromptForCredentialsA/W`,
    `CredUIConfirmCredentialsA/W`, `CredUIParseUserNameA/W`,
    `CredUIStoreSSOCredW`, `CredReadA/WriteA/DeleteA/EnumerateA/Free`,
    `CredIsMarshaledCredentialA`.
  - **csrss.exe**: stub que loga init e exit (Client/Server Runtime Subsystem).
  - **winlogon.exe**: app que desenha tela de logon Aero (fundo azul + caixa
    branca + avatar + campos usuario/senha + botão Entrar + rodapé). Importa
    secur32 + credui + user32 + gdi32.
  - **win10ui.exe**: app teste FINAL exercitando todo o stack — janela Win10
    mock + `dsound` (Audio OK) + `ws2_32` (Network OK) + `secur32` (SSPI OK) +
    `credui` (CredUI OK) + `fltmgr` (FltMgr registered).

### Estrutura final v0.9.0

```
Drivers (12 novos nesta rodada):
  src/drivers/input/mouse/                PS/2 mouse driver
  src/drivers/audio/HDAudio/              Intel HD Audio
  src/drivers/network/{ndis,tcpip,e1000}/ NDIS framework + TCP/IP + e1000
  src/drivers/usb/{usbport,usbhub,xhci}/  USB stack
  src/drivers/acpi/                       ACPI scan
  src/drivers/fltmgr/                     Filter Manager

DLLs (5 novas Win10/11 + 2 security = 7 nesta rodada):
  dll/win32/mmdevapi/   IMMDevice (Vista+)
  dll/win32/Audioses/   WASAPI (Win7+)
  dll/win32/dsound/     DirectSound
  dll/win32/winmm/      Multimedia legacy
  dll/win32/ws2_32/     Winsock 2.2
  dll/win32/secur32/    SSPI + LSA
  dll/win32/credui/     Credential UI

Apps (3 novos):
  apps/csrss/        Client/Server Runtime Subsystem stub
  apps/winlogon/     Logon screen (Aero style)
  apps/win10ui/      Stack test final
```

**Evidência consolidada** (run.ps1 -Headless -TimeoutSec 20):
- **24 `[ok]`** (era 20 — +4 das novas fases)
- 0 `[EXCECAO]`, 0 stub generico, 0 import nao resolvido
- 3x `0xCAFEBABE` (IOCTL intacto desde v0.1)
- calller.sys 100% (DriverEntry + 3 ticks + DriverUnload)
- **18 DLLs Win10/11** funcionais (era 13 — +5 nesta rodada)
- Logs: 6 mouse + 7 HDA audio + 10 network + 15 USB+ACPI+PnP+FltMgr +
  47 security/winlogon/csrss/win10ui
- `Sistema no ar.`

### Pendência (rodada futura)

- **Fase 11.2 Desktop Win10 cosmetico**: API 500 do servidor durante workflow.
  Pendente: window chrome com botoes close/min/max, mouse drag, Start menu
  popup. Atualmente o desktop tem wallpaper + taskbar basica (da v0.6/v0.7) +
  cursor (v0.9 fase 11.1) + tela de logon Aero (`winlogon.exe`) + janela Win10
  mock (`win10ui.exe`). Pode ser fechado numa rodada rapida.

## [0.8.0] - 2026-06-22

Oitava versão. **virtio-gpu 2D driver COMPLETO** — substitui o Bochs VBE
como backend primário do `gpu_*` API. Implementação real do protocolo
virtio 1.1: PCI capabilities walking, MMIO mapping, status protocol,
feature negotiation, virtqueue (descriptor table + avail/used rings),
notification via MMIO, polling do used ring, e os 7 comandos 2D essenciais.
Bochs VBE permanece como fallback. **Screendump prova rendering real via
GPU virtual: 97.78% azul (desktop) + barra de tarefas cinza + janelas +
texto branco — tudo apresentado pelo host via virtio-gpu.**

### Adicionado

- **FASE 10 — virtio-gpu 2D driver COMPLETO**
  *(4 sub-fases via workflow; build verde, boot verde, screendump validando)*:

  **10.1 — PCI + transport + status protocol**:
  - `src/drivers/display/VirtioGpu/{VirtioGpu.h, VirtioGpu.c}` *(novos)*.
  - `virtio_gpu_detect()` chamado em `kmain` apos `hal_init()`.
  - Detecta PCI vendor=`0x1AF4` device=`0x1050` em bus `0:4.0` (modern virtio).
  - Walk de PCI capabilities (offset `0x34` -> cap list): cada cap com
    `vndr=0x09` (vendor-specific) decoded para `cfg_type 1..5`:
    `COMMON_CFG bar=4 off=0x0`, `NOTIFY_CFG off=0x3000 mult=4`,
    `ISR_CFG off=0x1000`, `DEVICE_CFG off=0x2000`, `PCI_CFG`.
  - Mapeia 4 MMIO regions em `0xFFFFC100...` (phys=`0xFE000000`) via
    `mm_map_phys_range` com flag PCD.
  - Status protocol: `reset (0) -> ACK (1) -> DRIVER (3) -> FEATURES_OK (11)`.
  - Feature negotiation: le `device_feature` em 2 selects (0=lo, 1=hi).
    Aceita `VIRTIO_F_VERSION_1` (bit 32). Resultado:
    `features lo=0x30000002 hi=0x101` -> `VERSION_1` negociado.
  - run.ps1 ganhou `-device virtio-gpu-pci,id=vgpu0`.
  - Logs `[vgpu]` x14 no boot.

  **10.2 — Virtqueue + notify + DRIVER_OK**:
  - Structs packed com `static_assert` de layout:
    `vq_desc` (16B), `vq_avail` (4+VQ_SIZE*2+2), `vq_used` (4+VQ_SIZE*8+2).
  - `VIRTIO_QUEUE` struct: ponteiros desc/avail/used + phys, size,
    `last_used_idx`, `notify_off`, `notify_reg`, free-list
    (`next_free_desc`, `num_free`).
  - `virtio_queue_init(idx, q)`: queue_select, clamp size=`min(64, dev)`,
    aloca 3 paginas via `pmm_alloc_contiguous(1)`, zera, monta free-list
    (`desc[i].next = i+1`, ultimo `0xFFFF`), grava phys em
    `queue_desc/driver/device`, `queue_enable=1`.
  - Helpers: `virtio_queue_alloc_desc` (pop free-list), `free_desc_chain`
    (segue `VIRTQ_DESC_F_NEXT`), `submit` (avail.ring[idx]=head, mb,
    avail.idx++, mb, *notify_reg=queue_idx), `wait_response` (polling
    de `used.idx` com PAUSE).
  - 2 queues: `controlq idx=0 size=64`, `cursorq idx=1 size=16`.
  - DRIVER_OK -> `status=0x0F` (ACK|DRIVER|FEATURES_OK|DRIVER_OK).

  **10.3 — Os 7 comandos 2D essenciais**:
  - `GET_DISPLAY_INFO (0x0100)` -> scanout 0: **1280x800 enabled=1** (default QEMU).
  - `RESOURCE_CREATE_2D (0x0101)`: cria resource_id, formato `B8G8R8A8_UNORM`.
  - `RESOURCE_UNREF (0x0102)`.
  - `RESOURCE_ATTACH_BACKING (0x0106)`: associa lista de `mem_entry`
    (phys+length) a um resource — usa o framebuffer DMA kmalloc'd.
  - `SET_SCANOUT (0x0103)`: marca resource como display output (scanout_id=0).
  - `TRANSFER_TO_HOST_2D (0x0105)`: host re-le o backing.
  - `RESOURCE_FLUSH (0x0104)`: host re-apresenta o scanout.
  - Cada comando: aloca 2 desc (header NEXT + response WRITE), submete via
    controlq, polling em used, le resp_hdr.type, libera chain.
  - Smoke test soft em main.c: 64x64 BGRA kmalloc'd, CREATE_2D +
    ATTACH_BACKING + UNREF (sem SET_SCANOUT). Logs todos `OK_NODATA`.

  **10.4 — Backend primario + screendump validando**:
  - `virtio_gpu_init_display(1024, 768)`: aloca FB DMA contiguo
    `1024*768*4 = 3 MiB` via `pmm_alloc_contiguous(768 pages)`,
    `CREATE_2D resource_id=1`, `ATTACH_BACKING phys=framebuffer`,
    `SET_SCANOUT scanout=0 resource_id=1`. AGORA virtio-gpu controla a tela.
  - `virtio_gpu_present()`: envia `TRANSFER_TO_HOST_2D` (full rect) +
    `RESOURCE_FLUSH` — host re-le o backing e re-apresenta.
  - `gpu_init()` em `src/drivers/display/BasicDisplay/gpu.c` agora tenta
    virtio-gpu PRIMEIRO; Bochs VBE so fallback. `gpu_pixel/fill_rect/
    copy_rect/clear` escrevem direto no FB virtio quando ativo.
  - `win32k_compose()` chama `gpu_present()` ao final pra commitar o frame.
  - **Evidencia visual**: `build\screen.ppm` (P6 1024x768) capturado via
    QMP screendump mostra **97.78% azul (desktop)** + **barra cinza**
    (taskbar) + **janelas** (compose) + **texto branco**. Cada pixel veio
    pelo caminho:
    `win32k.compose -> gpu_* -> framebuffer DMA -> virtio_gpu_present
    -> controlq virtqueue -> notify MMIO -> host QEMU -> screendump`.

  ### Stack final pos-v0.8.0

  ```
  src/drivers/display/
  ├── BasicDisplay/{BasicDisplay,gpu}.{c,h}    Bochs VBE (fallback)
  └── VirtioGpu/{VirtioGpu}.{c,h}              virtio-gpu 2D (primario)

  Arch GPU final:
  APP ring 3 -> ddraw/d3d11/d3d12/d2d1 (DLLs Win10/11)
              -> dxgkrnl + dxgmms (DirectX kernel)
              -> gpu_* DDI (api unificada)
              -> virtio-gpu (primario) ou Bochs VBE (fallback)
              -> QEMU host
  ```

  **Evidencia consolidada** (run.ps1 -Headless -TimeoutSec 14):
  - **20 `[ok]`** (era 18 — +1 dxgkrnl/dxgmms da v0.7, +1 virtio-gpu init)
  - 0 `[EXCECAO]`, 0 import nao resolvido
  - 3x `0xCAFEBABE` (IOCTL intacto)
  - 65 linhas `[vgpu]` (detect + caps + features + virtqueue + 7 commands + present cycle)
  - 2 linhas `[dxgk]` + `[dxgmms]`
  - calller.sys 100% lifecycle
  - guiapp/dxdemo/d3d11demo/desktop todos rodando
  - screendump 1024x768 BGRA com 97.78% azul (desktop renderizado pelo host)
  - `Sistema no ar.`

## [0.7.0] - 2026-06-22

Sétima versão. **Migração arquitetural de XDDM para WDDM 2.x** — modelo
Windows 10/11. Implementa o stack DirectX moderno completo: `dxgkrnl.sys` +
`dxgmms.sys` no kernel, 6 novas DLLs ring 3 (`dxgi.dll`, `d3d11.dll`,
`d3d12.dll`, `d2d1.dll`, `dwrite.dll`, `dxcore.dll`), `ddraw.dll` vira shim
que delega para `d3d11.dll` (igual Windows 10 real), `win32k` split em
`win32kbase.c` + `win32kfull.c` (modelo NT 6.4 = Win 8.1+), e
`d3d11demo.exe` exercitando o stack ponta a ponta. Build verde, boot verde,
zero regressão. **13 DLLs Win10/11** funcionais no MeuOS.

### Adicionado

- **FASE 9.7 — `dxgkrnl.sys` + `dxgmms.sys` (DirectX kernel-mode WDDM)**
  *(`src/subsystems/dx/dxgkrnl/`, `src/subsystems/dx/dxgmms/`)*:
  - `dxgkrnl` espelha o `dxgkrnl.sys` real do Win10/11: dispatcher central
    do DirectX em kernel-mode. TODAS as chamadas D3D/DXGI vindas das DLLs
    passam por aqui (no Windows real via syscalls; no MeuOS via direct
    call por enquanto).
  - APIs publicas (15 novos exports em `ntoskrnl.c`):
    `DxgkInitialize/Shutdown`, `DxgkOpenAdapter`, `DxgkCreateDevice/Context/Allocation`,
    `DxgkPresent`, `DxgkPresentDisplayOnly`, `DxgkSubmitCommand`.
  - Pools estaticos: 4 adapters / 16 devices / 32 contexts / 64 allocations.
    Cada estrutura segue layout NT (DXGK_ADAPTER, DXGK_DEVICE, DXGK_CONTEXT,
    DXGK_ALLOCATION com kmd_context, refcount, width/height/bpp/pitch).
  - `DxgkInitialize` le `gpu_active/width/height/bpp/pitch` do BasicDisplay
    e prepara o adapter primario. `DxgkPresent` delega para `gpu_fill_rect`;
    `DxgkPresentDisplayOnly` faz blit pixel-a-pixel via `gpu_pixel`.
  - `dxgmms` espelha `DxgMms2.sys`: GPU memory manager. Pool fixo de 64
    DXGMMS_RESIDENCY (estados NONE/IN_RAM/IN_VRAM/LOCKED), backed por
    `kmalloc`. Exports: `DxgMmsInitialize/Shutdown/Allocate/Free/Lock/Unlock`.
  - `kmain` chama `DxgkInitialize()` + `DxgMmsInitialize()` apos `gpu_init()`.
  - Log: `[dxgk] DxgkInitialize: adapter primario 1024x768x32 pitch=4096 (gpu backend OK)`
    e `[dxgmms] init OK (pool=64 descritores, backend=heap kernel)`.

- **FASE 9.8 — `dxgi.dll` (DirectX Graphics Infrastructure)**
  *(`dll/win32/dxgi/{dxgi.c, dxgi.def}`)*:
  - PE32+ ImageBase `0x3F00000` (entre ddraw e PMM_BASE).
  - ABI COM completo de `IDXGIFactory`/`Factory1`/`Factory2`, `IDXGIAdapter`/
    `Adapter1`, `IDXGIOutput`, `IDXGISwapChain`. Pools estaticos 4/4/4/8.
  - `IDXGIAdapter::GetDesc/GetDesc1` retornam **VendorId=0x1234, DeviceId=0x1111**
    (Bochs VBE real!), Description=`L"MeuOS Bochs Display Driver"`,
    DedicatedVideoMemory=64 MiB.
  - `IDXGIOutput::GetDisplayModeList` expoe `1024x768@60Hz BGRA`.
  - `IDXGISwapChain::Present` delega para hook `DxgkPresentDisplayOnly` via
    `DXGISetPresentBackend()` (plugavel pra UMD/dxgkrnl no futuro).
  - Exports: `CreateDXGIFactory`, `CreateDXGIFactory1`, `CreateDXGIFactory2`,
    `DXGISetPresentBackend`.

- **FASE 9.9 — `d3d11.dll` + `d3d12.dll`**
  *(APIs primarias Windows 10 e 11)*:
  - `dll/win32/d3d11/d3d11.c` (~45 KB, PE32+, ImageBase `0x4500000` +
    `--dynamicbase` por estar >= PMM_BASE):
    `ID3D11Device` com **27 metodos** (Create Buffer/Tex1D/2D/3D/SRV/UAV/RTV/
    DSV/InputLayout/VS/GS/GSStreamOut/PS/HS/DS/CS/ClassLinkage/Blend/
    DepthStencil/Rasterizer/Sampler/Query/Predicate/Counter/DeferredContext +
    GetImmediateContext/FeatureLevel/CreationFlags/RemovedReason) +
    `ID3D11DeviceContext` com **32 metodos** (VS/PS/IA/RS/OM/Clear/Draw/
    Map/Unmap) + `ID3D11Resource` generico (BUFFER/TEX2D/RTV/DSV/SRV/
    shaders/InputLayout/RasterState/BlendState/DepthStencil/Sampler/Query).
  - Pools estaticos: 4 devices / 16 contexts / 64 resources.
  - Exports: `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain` (este ultimo
    aloca um IDXGISwapChain ABI-compativel com vtable propria — fix do bug
    inicial que causava #UD ao chamar Present).
  - `dll/win32/d3d12/d3d12.c` (~71 KB, ImageBase `0x4600000` + `--dynamicbase`):
    `ID3D12Device` com **40+ metodos** (CommandQueue/Allocator/List/
    PipelineState/RootSignature/DescriptorHeap/Fence/CommittedResource/
    PlacedResource/ReservedResource/Heap/QueryHeap/CommandSignature +
    GetDescriptorHandleIncrementSize/GetAdapterLuid),
    `ID3D12CommandQueue` (ExecuteCommandLists/Signal/Wait/GetTimestampFrequency),
    `ID3D12CommandList` (Close/Reset + DrawInstanced/DrawIndexedInstanced/
    Dispatch/Copy*/ResourceBarrier/Clear*/RSSet*/OMSet*/SetPipelineState/
    SetRoot*/SetDescriptorHeaps/IA*), `ID3D12Fence`, `ID3D12Resource`.
  - Pools: 4 devices / 8 queues / 16 allocators / 32 lists / 16 fences /
    64 resources / 16 pipelines / 16 rootsigs / 16 heaps / 16 blobs.
  - Exports: `D3D12CreateDevice`, `D3D12GetDebugInterface`,
    `D3D12SerializeRootSignature`.

- **FASE 9.10 — `d2d1.dll` + `dwrite.dll` + `dxcore.dll`**
  *(Direct2D moderno + DirectWrite + DirectX Core Win11)*:
  - `dll/win32/d2d1/d2d1.c` (~38 KB, ImageBase `0x4700000`): `ID2D1Factory`
    (CreateRectangle/RoundedRect/Ellipse/PathGeometry/HwndRenderTarget/
    DxgiSurfaceRT/DCRT/WicBitmapRT/StrokeStyle/DrawingStateBlock +
    GetDesktopDpi/ReloadSystemMetrics), `ID2D1RenderTarget` com **52 metodos**
    (BeginDraw/EndDraw/Clear/Draw|FillRectangle/RoundedRect/Ellipse/
    Geometry/Bitmap/Text/TextLayout/GlyphRun + CreateBrush/Bitmap +
    SetTransform/Antialias/Layer/Clip + Flush/Save/RestoreDrawingState +
    GetSize/PixelSize/MaxBitmapSize/IsSupported), `ID2D1Brush`, `ID2D1Bitmap`,
    `ID2D1Geometry`. Pools: 4/4/16/16/16.
  - `dll/win32/dwrite/dwrite.c` (~43 KB, ImageBase `0x4800000`):
    `IDWriteFactory` (25 metodos), `IDWriteTextFormat` (28 metodos),
    `IDWriteTextLayout` (19 metodos com metricas estimadas baseado em fontSize),
    `IDWriteFontCollection`, `IDWriteFontFamily`, `IDWriteFont` (metricas
    coerentes 2048 EM Segoe UI-like).
  - `dll/win32/dxcore/dxcore.c` (~20 KB, ImageBase `0x4900000`):
    `IDXCoreAdapterFactory`, `IDXCoreAdapterList`, `IDXCoreAdapter` com 18
    propriedades (InstanceLuid, DriverVersion, DriverDescription=
    `"BasicDisplay (MeuOS)"`, HardwareID `0x1234:0x1111`,
    KmdModelVersion=WDDM 3.0, DedicatedAdapterMemory=16 MiB,
    SharedSystemMemory=128 MiB, IsHardware=0, IsIntegrated=1, etc.).
  - Exports: `D2D1CreateFactory`, `DWriteCreateFactory`,
    `DXCoreCreateAdapterFactory`.
  - Bug fix: `UINT16`/`INT16` typedefs precisavam vir ANTES de
    `DWRITE_FONT_METRICS` (resolveu 10 erros de compilacao).

- **FASE 9.11 — `ddraw` shim + `win32k` split + `d3d11demo.exe`**:
  - **ddraw vira shim que delega para d3d11** (igual Win10 real): `ddraw.c`
    `IDirectDraw7::CreateSurface` chama `D3D11CreateDevice` via `libd3d11.a`
    na 1a invocacao; `IDirectDrawSurface7::Blt` marca flag de compat;
    novo export `DdrawCompatModeActive()`. `build.ps1` re-linka `ddraw.dll`
    apos `d3d11.dll` ser construida.
  - **win32k split** (modelo NT 6.4 = Win 8.1+):
    `src/subsystems/win32/win32kbase.c` = HWND mgmt, message queue, focus,
    compose, taskbar, GDI minimo (NtUserGet*Message, NtUserPostMessage,
    NtUserSetFocus, win32k_compose, draw_taskbar);
    `src/subsystems/win32/win32kfull.c` = TextOut com cor, FillRect avancado,
    DIB, NtGdi* avancados;
    `src/subsystems/win32/win32k.c` = SHIM que #include's os dois (mantem um
    unico .o sem fragmentar symbols).
    Log: `[win32k] base + full carregados (Windows 8.1+ split model)`.
  - **`apps/d3d11demo/d3d11demo.c`** (PE32+, ImageBase escolhido pelo agente
    pra evitar PMM): exerce stack WDDM completo —
    `CreateWindowExA` 1024x768 -> `CreateDXGIFactory` -> S_OK ->
    `EnumAdapters(0)` -> Vendor=0x1234 Device=0x1111 ->
    `D3D11CreateDeviceAndSwapChain` -> S_OK ->
    `ClearRenderTargetView(0.2, 0.4, 0.7, 1.0)` ->
    `IDXGISwapChain::Present(1, 0)` -> `GetMessage` loop com `WM_PAINT`
    refazendo Clear+Present (frame 2) -> `PostQuitMessage` -> Release
    dos 4 objetos COM -> `ExitProcess(0)`.
  - Bug fix em `d3d11.c`: `D3D11CreateDeviceAndSwapChain` agora aloca um
    IDXGISwapChain ABI-compativel com vtable propria (slot Present correto)
    em vez de devolver o device como swap chain — corrige #UD em Present.
  - `run.ps1` carrega `d3d11demo.exe` apos `dxdemo.exe` na lista default.

### Estrutura final do projeto v0.7.0

```
src/
├── boot/                                 multiboot
├── ntos/                                 ntoskrnl (executive)
├── hal/                                  HAL
├── drivers/
│   ├── input/keyboard
│   ├── display/BasicDisplay/             KMD WDDM-style (Bochs VBE)
│   ├── video/{vga, video, font8x8}       legado XDDM (Win XP)
│   ├── serial/serial
│   └── filesystems/ntfs/...
└── subsystems/
    ├── win32/{win32, win32k, win32kbase, win32kfull}   NT 6.4 split
    └── dx/                                              NOVO (WDDM 2.x)
        ├── dxgkrnl/                      DirectX kernel dispatcher
        └── dxgmms/                        GPU memory manager

dll/win32/
├── kernel32, user32, gdi32, advapi32     classics
├── ddraw                                  shim -> d3d11 (Win10+ behavior)
├── d3d9                                   D3D7/8/9 stubs (legado)
├── dxgi                                   Win10 — Graphics Infrastructure
├── d3d11                                  Win10 primary API
├── d3d12                                  Win11 primary API
├── d2d1                                   Direct2D
├── dwrite                                 DirectWrite
└── dxcore                                 Win11 — alternativa ao DXGI

apps/
├── dxdemo/dxdemo.c                        XDDM stack (ddraw + d3d9)
└── d3d11demo/d3d11demo.c                  WDDM stack (dxgi + d3d11)
```

**Evidencia consolidada** (run.ps1 -Headless -TimeoutSec 16):
- **18 `[ok]`** (era 17 — +1 do DX subsystem)
- 0 `[EXCECAO]`, 0 Page Fault, 0 stub generico, 0 import nao resolvido
- 3x `0xCAFEBABE` (IOCTL intacto)
- calller.sys 100% (DriverEntry + 3x thread tick + thread exit + DriverUnload)
- 1x `[dxgk] DxgkInitialize: adapter primario 1024x768x32`
- 1x `[dxgmms] init OK (pool=64, backend=heap kernel)`
- 1x `[win32k] base + full carregados (Windows 8.1+ split model)`
- 13 DLLs no build: `ntdll, kernel32, user32, gdi32, advapi32, ddraw, d3d9,
  dxgi, d3d11, d3d12, d2d1, dwrite, dxcore`
- `dxdemo.exe` roda (XDDM compat) + `d3d11demo.exe` roda (WDDM novo)
- screendump 1024x768x32 capturado via QMP
- `Sistema no ar.`

## [0.6.0] - 2026-06-22

Sexta versão. **GPU stack XDDM completo** (modelo Windows XP/2000):
driver Bochs VBE no kernel + GDI 32-bit no win32k + DirectDraw 7 + Direct3D 7
stubs + `dxdemo.exe` exercitando o stack ponta-a-ponta. Build verde, boot
verde, **screendump 1024x768x32** validando os pixels. Próxima rodada
(v0.7.0) migra para **WDDM 2.x (Windows 10+)** adicionando `dxgkrnl.sys`,
`dxgmms.sys`, `dxgi.dll`, `d3d11.dll`, `d3d12.dll`, `d2d1.dll`, `dwrite.dll`.

### Adicionado

- **FASE 9 — GPU + GDI 32-bit + DirectX 7 stubs (XDDM)**
  *(5 sub-fases via workflow; 17 arquivos novos/modificados; build verde, boot verde)*:

  **9.1 — `mm_map_phys_range` + driver Bochs VBE**:
  - `src/ntos/mm/paging.{c,h}`: nova `mm_map_phys_range(virt, phys, size, flags)`
    walk PML4→PDPT→PD→PT com `ensure_table`, instala um PTE por pagina 4 KiB,
    flush TLB; recusa ranges sobre huge pages. `mm_map_zero_page` refatorada
    para reusar `mm_map_phys_range` (com fast-path sem vazar frame).
  - `src/drivers/display/BasicDisplay/BasicDisplay.{c,h}` (renomeado de
    `src/drivers/video/bochsvbe.{c,h}` pra padrão Windows real
    `BasicDisplay.sys`): detecta PCI vendor=0x1234 device=0x1111
    (QEMU std-vga = Bochs Display Interface), le BAR0=0xFD000000, habilita
    Memory+BusMaster, mapeia 3072 KiB do LFB em virt=0xFFFFC000FD000000 com
    PCD via mm_map_phys_range, valida `VBE_DISPI_INDEX_ID=0xB0C5` via portas
    0x1CE/0x1CF, programa 1024x768x32 (disable -> set XRES/YRES/BPP -> enable
    com VBE_DISPI_ENABLED|VBE_DISPI_LFB_ENABLED|VBE_DISPI_8BIT_DAC).
  - `src/drivers/display/BasicDisplay/gpu.{c,h}` (renomeado de
    `src/drivers/video/gpu.{c,h}`): API DDI-like exposta pro kernel:
    `gpu_init/active/width/height/pitch/bpp/clear/pixel/fill_rect/copy_rect/present`
    sobre o LFB 32 bpp (RGBA `0xAARRGGBB`).
  - `kmain` chama `gpu_init(1024, 768)` apos `hal_init()`.
  - Log: `[bvbe] PCI 1234:1111 BAR0=phys=0xFD000000 virt=0xFFFFC000FD000000 id=0xB0C5 mode=1024x768x32`
    e `[gpu] init OK WxHxBPP, LFB=...`.

  **9.2 — GDI 32-bit no win32k**:
  - `src/drivers/video/video.h` ganhou `RGB32(r,g,b)` macro + constantes
    XRGB888 (`RGB32_BLACK/WHITE/BLUE/...`) + `palette_8_to_32(idx)`.
  - `src/subsystems/win32/win32k.c`: helpers internos
    `w32k_clear/fill_rect/hline/vline/rect/draw_char/draw_text + scr_width/height`
    escolhem o backend em runtime: gpu_* (32 bpp XRGB888) se `gpu_active()`,
    fallback fb_* (mode 13h 8 bpp). `draw_window_chrome/draw_taskbar/
    win32k_compose/NtGdiTextOut_k/NtGdiFillRect_k` migrados.
  - `ensure_fb()` pula mode13h se a GPU ja esta ativa.
  - **SSDT (NtUser*/NtGdi*) intacta** — aplicacoes ring 3 nao mudam.
  - DIB minimo: `NtGdiCreateDIBSection_k` aloca buffer XRGB888 no heap do
    kernel. Wallpaper agora ocupa 1024x768 (era 320x200).

  **9.3 — DirectDraw 7 stubs (DLL ring 3)**:
  - `dll/win32/ddraw/{ddraw.c, ddraw.def}` (PE32+ x64, ImageBase 0x3E00000):
    `IDirectDraw7` e `IDirectDrawSurface7` com vtables COM-ABI completas.
  - Metodos IDD7: `QueryInterface/AddRef/Release/SetCooperativeLevel/
    SetDisplayMode/CreateSurface/GetDisplayMode/CreatePalette/CreateClipper/
    WaitForVerticalBlank/GetCaps/RestoreDisplayMode/FlipToGDISurface/Compact`.
  - Metodos IDDS7: `Blt/Flip/Lock/Unlock/GetDC/ReleaseDC/Restore/IsLost/
    SetClipper/SetPalette/GetSurfaceDesc`.
  - Todos devolvem `DD_OK`. Pool estatico (4 IDD7 + 16 surfaces).
  - DLL autocontida (sem dependencia de ntdll).
  - Exports: `DirectDrawCreate`, `DirectDrawCreateEx`, `DirectDrawEnumerateA`,
    `DirectDrawEnumerateExA`.
  - `DDSURFACEDESC2` com `#pragma pack(1)` para ABI binario.

  **9.4 — Direct3D 7 stubs (`d3d9.dll`)**:
  - `dll/win32/d3d9/{d3d9.c, d3d9.def}` (renomeado de `dll/win32/d3d/`,
    ImageBase 0x3C00000 — `0x4000000` evitado pq e `PMM_BASE`):
    `IDirect3D7` e `IDirect3DDevice7` com vtables COM-ABI completas.
  - Metodos ID3D7: `EnumDevices/CreateDevice/CreateVertexBuffer/
    EnumZBufferFormats/EvictManagedTextures`.
  - Metodos ID3DD7: `GetCaps/BeginScene/EndScene/Clear/SetTransform/
    GetTransform/SetViewport/SetMaterial/SetLight/LightEnable/
    SetRenderState/GetRenderState/DrawPrimitive/DrawIndexedPrimitive/
    SetTexture/SetTextureStageState/SetRenderTarget` + state-blocks.
  - Todos devolvem `D3D_OK`. Pools estaticos (4 IDirect3D7 + 4 devices).
  - Exports: `Direct3DCreate7`, `Direct3DCreate8`, `Direct3DCreate9`.

  **9.5 — `apps/dxdemo/dxdemo.c` (renomeado de `gpuapp.c`)**:
  - PE32+ x64, ImageBase `0x400000` (4 MiB, slot pd[2] livre entre kernel e
    USTACK 0x600000; 0x4200000 sugerido evitado pq >= PMM_BASE colide com
    frames gerenciados).
  - Importa kernel32/user32/gdi32/ntdll/ddraw/d3d9.
  - WinMain: `RegisterClassA -> CreateWindowExA -> DirectDrawCreateEx ->
    SetCooperativeLevel -> SetDisplayMode -> CreateSurface(primary) ->
    Direct3DCreate7 -> CreateDevice -> WindowProc WM_PAINT (FillRect
    vermelho/amarelo + TextOut + ciclo BeginScene/Clear/SetViewport/
    SetRenderState/DrawPrimitive/EndScene) -> PostQuitMessage ->
    Release dos 4 objetos COM -> ExitProcess(0)`.
  - `run.ps1` ganhou `-Screendump`/`-QmpPort` (estavam documentados mas
    faltavam o handshake QMP). Screendump valida `build\screen.ppm`
    (P6 1024x768) com a janela + retangulos coloridos renderizados.
  - **Bug colateral corrigido**: `say_hex` em `ntoskrnl.c` usava buffer
    local de 64 bytes que estourava com prefixos longos do dxdemo
    (53+ chars + "0x" + 16 digitos + "\\n\\0" = 73 bytes), causando #UD
    (vector 6). Buffer expandido para 160 bytes.

  ### Polish — padronizacao de nomenclatura Windows real:

  | Workflow gerou | Renomeado para |
  |----------------|----------------|
  | `src/drivers/video/bochsvbe.{c,h}` | `src/drivers/display/BasicDisplay/BasicDisplay.{c,h}` |
  | `src/drivers/video/gpu.{c,h}`      | `src/drivers/display/BasicDisplay/gpu.{c,h}` |
  | `dll/win32/d3d/d3d.{c,def}`        | `dll/win32/d3d9/d3d9.{c,def}` (no Win10 real, D3D7 vem dentro do ddraw.dll; D3D9 e que tem DLL propria) |
  | `apps/gpuapp.c`                    | `apps/dxdemo/dxdemo.c` (estilo `dxdiag` do Windows — subpasta por app) |
  | binario `d3d.dll` / `libd3d.a`     | `d3d9.dll` / `libd3d9.a` |
  | binario `gpuapp.exe`               | `dxdemo.exe` |

  `src/drivers/video/` mantido com `vga.c/video.c/font8x8.c` como **legado
  XDDM** (modelo Windows XP), enquanto `src/drivers/display/BasicDisplay/`
  e o caminho **moderno WDDM-style** (Windows 8+).

  **Evidencia consolidada** (run.ps1 -Headless -TimeoutSec 14):
  - 17 `[ok]` (era 16 — +1 da GPU)
  - 0 `[EXCECAO]`, 0 Page Fault, 0 stub generico, 0 import nao resolvido
  - 3x `0xCAFEBABE` (IOCTL anterior intacto)
  - calller.sys 100% (Ps/Ob/Cm callbacks + KEVENT + ExAllocate + thread ticks + DriverUnload)
  - 7 linhas `[bvbe]` + `[gpu]` (Bochs VBE detect/map/programa + gpu_init OK)
  - dxdemo.exe roda DirectDrawCreate7 + Direct3DCreate7 + WM_PAINT
  - **screendump 1024x768x32** captura pixels: janela "MeuOS GPU App"
    com FillRect vermelho/amarelo + TextOut "GDI + DirectDraw + D3D"
  - `Sistema no ar.` x1

## [0.5.0] - 2026-06-22

Quinta versão. **Polish arquitetural** completando a reorganização v0.4.0:
SDK dividido em `sdk/ddk/{wdm.h, ntddk.h, ntifs.h}` (estilo WDK real),
`examples/` renomeado para `apps/`, placeholder `src/ntos/ke/i386/` para
futura porta x86 32-bit, e arquivos `.def` informativos em cada DLL
documentando o contrato de exports. Build verde, boot verde, zero
regressão.

### Alterado

- **FASE 8.1 — Refinamento final estilo WDK/ReactOS**:
  - **`sdk/ddk/{wdm.h, ntddk.h, ntifs.h}`**: o monolitico `sdk/ntddk.h`
    foi movido para `sdk/ddk/ntddk.h` (preserva historico via `git mv`).
    Criados `sdk/ddk/wdm.h` e `sdk/ddk/ntifs.h` como wrappers que fazem
    `#include "ntddk.h"`. No WDK real, `wdm.h` contem o subconjunto base
    (tipos primitivos, IRP, KEVENT/KSPIN_LOCK, basic Io/Rtl) que TODOS
    os drivers usam, `ntddk.h` estende com APIs do executive (Cc/Cm/Ex/
    Mm/Ob/Ps/Se), e `ntifs.h` estende com FILE_OBJECT internals para
    drivers de filesystem. No MeuOS comecam como aliases — podem ser
    divididos em conteudo nas rodadas futuras quando a separacao de
    niveis importar. `build.ps1` ganhou `-I sdk/ddk` adicional para
    resolver `<ntddk.h>` direto da nova localizacao.
  - **`examples/` → `apps/`**: renomeado via `git mv` (15 arquivos),
    `__pycache__` removido. Casa com a convencao Windows/ReactOS onde
    "apps" sao binarios de teste/demo distintos do kernel. Atualizadas
    referencias em `build.ps1` (variavel `$ex` aponta para `apps/`,
    mensagens de log mostram `apps\<nome>.c`).
  - **`src/ntos/ke/i386/`**: pasta criada com `.gitkeep` documentando o
    plano de futura porta 32-bit. Estrutura espelharia
    `src/ntos/ke/amd64/` (isr.asm/c, idt, pic, pit, gdt, kpcr 32-bit,
    syscall via sysenter, syscall_entry.asm, usermode). `build.ps1`
    selecionaria a pasta arch por target (`-target x86-*` -> `i386/`).
  - **Arquivos `.def` informativos** em cada DLL:
    - `dll/ntdll/ntdll.def` — lista os ~40 exports `Nt*` (NtCreateFile,
      NtOpenKey, NtCreateSection, NtUserCreateWindowEx, etc.) +
      `LdrGetModuleHandle`/`LdrGetProcAddress`.
    - `dll/win32/kernel32/kernel32.def` — `GetStdHandle`, `WriteFile`,
      `CreateProcessA`, `EnumProcessesEx`, `QueryDirectoryFileEx`, etc.
    - `dll/win32/user32/user32.def` — `MessageBoxA`, `CreateWindowExA`,
      loop de mensagens (`GetMessage`/`DispatchMessage`), `BeginPaint`,
      `FillRect`, `SetFocus`, `CreateDesktopWindowA`,
      `CreateConsoleWindowA`.
    - `dll/win32/gdi32/gdi32.def` — `TextOutA`, `GetStockObject`,
      `CreateSolidBrush`, `DeleteObject`, `SetTextColor`.
    - `dll/win32/advapi32/advapi32.def` — SCM stubs
      (`StartServiceCtrlDispatcherA`, `OpenSCManagerA`,
      `CreateServiceA`, etc.) + registro (`RegOpenKeyExA`,
      `RegQueryValueExA`, `RegCloseKey`).
    Os `.def` sao **documentacionais** — o Zig cc gera os exports via
    `__declspec(dllexport)` nos `.c`. Replicam o contrato publico no
    estilo NT/WDK real (cada DLL tem um `.def` listando o contrato).

  **Evidencia** (build + boot): 16 `[ok]`, 0 `[EXCECAO]`, 3x
  `0xCAFEBABE`, `Sistema no ar`. Mesma evidencia da v0.4.0 — refinamento
  arquitetural sem alterar comportamento.

## [0.4.0] - 2026-06-22

Quarta versão. **Reorganização arquitetural total**: estrutura de pastas
agora espelha o **Windows Research Kit (WRK) / ReactOS** com hierarquia
`src/ntos/{ke,ex,mm,ob,io,ps,cm,lpc,ldr,init,inc}`, separacao
arquitetural (`src/ntos/ke/amd64/` para tudo x86_64-especifico),
diretorios por subsistema (`src/drivers/{input,video,serial,filesystems}`,
`src/subsystems/win32/`) e DLLs em subpastas (`dll/ntdll/`,
`dll/win32/{kernel32,user32,gdi32,advapi32}/`). 88 arquivos movidos via
`git mv` (preservam historico). Build verde, boot verde, zero regressao.

### Alterado

- **FASE 8 — Reorganizacao de pastas estilo NT (WRK/ReactOS)**
  *(88 arquivos movidos, ~31 `#include`s reescritos, `build.ps1` ajustado)*:

  Estrutura nova:
  ```
  src/
  ├── boot/                       ← multiboot stub (era src/arch/boot.asm)
  │   └── boot.asm
  ├── ntos/                       ← ntoskrnl (executive)
  │   ├── ntoskrnl.c              ← era nt/ntexec.c (export table)
  │   ├── inc/io.h                ← era src/include/io.h
  │   ├── init/main.c             ← era src/kernel.c
  │   ├── ke/                     ← Kernel (sync/dispatcher)
  │   │   ├── sync.c              ← KEVENT/KSPIN_LOCK/KMUTEX
  │   │   ├── cpu_features.c      ← CR4/XCR0
  │   │   └── amd64/              ← arquitetura-especifico
  │   │       ├── isr.asm, idt.c, pic.c, pit.c, isr.c
  │   │       ├── gdt.c, kpcr.c, msr_init.c
  │   │       ├── syscall.c, syscall_entry.asm
  │   │       └── usermode.c
  │   ├── ex/                     ← Executive
  │   │   ├── pool.c              ← ExAllocatePool (era ke/pool.c)
  │   │   ├── callbacks.c         ← Ps/Ob/Cm callbacks (era nt/callbacks.c)
  │   │   └── seh.c               ← __C_specific_handler (era nt/seh.c)
  │   ├── mm/                     ← Memory Manager
  │   │   ├── pmm.c, heap.c, paging.c, mdl.c
  │   │   ├── section.c           ← era nt/section.c
  │   │   └── virtmem.c           ← era mm/virtual_memory.c (renomeado)
  │   ├── ob/object.c             ← Object Manager (era nt/object.c)
  │   ├── io/                     ← I/O Manager
  │   │   ├── io.c                ← IRPs (era nt/io.c)
  │   │   └── driver.c            ← Driver loading (era nt/driver.c)
  │   ├── ps/                     ← Process Manager
  │   │   ├── process.c           ← EPROCESS/ETHREAD (era nt/process.c)
  │   │   ├── cid_table.c         ← PspCidTable (era nt/cid_table.c)
  │   │   └── systhread.c         ← PsCreateSystemThread (era ke/systhread.c)
  │   ├── cm/registry.c           ← Configuration Manager (era nt/registry.c)
  │   ├── lpc/pipe.c              ← Local Procedure Call (era nt/pipe.c)
  │   └── ldr/                    ← PE Loader (era src/loader/)
  │       ├── loader.c, pe.c
  ├── hal/                        ← HAL (mantido)
  ├── drivers/                    ← built-in drivers
  │   ├── input/keyboard.{c,h}
  │   ├── video/{vga,video,font8x8}
  │   ├── serial/serial.{c,h}
  │   └── filesystems/ntfs/{ntfs.c,ntfs.h,ntfs_fs.c}
  └── subsystems/win32/{win32,win32k}

  dll/
  ├── ntdll/ntdll.c
  └── win32/{kernel32,user32,gdi32,advapi32}/<name>.c
  ```

  Mapeamento de `#include`s (substituidos em 31 arquivos por `-replace`):
  - `nt/object.h` → `ob/object.h`
  - `nt/io.h` → `io/io.h`
  - `nt/driver.h` → `io/driver.h`
  - `nt/process.h` → `ps/process.h`
  - `nt/cid_table.h` → `ps/cid_table.h`
  - `nt/callbacks.h` → `ex/callbacks.h`
  - `nt/section.h` → `mm/section.h`
  - `nt/registry.h` → `cm/registry.h`
  - `nt/pipe.h` → `lpc/pipe.h`
  - `nt/ntexec.h` → `ntoskrnl.h`
  - `ke/pool.h` → `ex/pool.h`
  - `ke/systhread.h` → `ps/systhread.h`
  - `ke/{gdt,kpcr,syscall,usermode}.h` → `ke/amd64/...`
  - `cpu/{idt,isr,pic,pit}.h` → `ke/amd64/...`
  - `mm/virtual_memory.h` → `mm/virtmem.h`
  - `loader/{loader,pe}.h` → `ldr/...`
  - `drivers/keyboard.h` → `input/keyboard.h`
  - `drivers/serial.h` → `serial/serial.h`
  - `drivers/{vga,video}.h` → `video/...`
  - `drivers/ntfs.h` → `filesystems/ntfs/ntfs.h`

  Ajustes em `build.ps1`:
  - Novos `-I` paths: `-I src/ntos`, `-I src/ntos/inc`, `-I src/drivers`,
    `-I src/subsystems` (alem de `-I src` e `-I sdk` ja existentes). Isso faz
    `#include "ke/sync.h"` resolver em `src/ntos/ke/sync.h` automaticamente,
    sem precisar prefixar `ntos/` nos `#include`s.
  - Paths das DLLs atualizados: `dll/ntdll.c` → `dll/ntdll/ntdll.c`, etc.
  - **HOTFIX**: `Get-ObjName` agora inclui a extensao no nome do `.o`
    (`ntos_ke_amd64_isr_asm.o` vs `ntos_ke_amd64_isr_c.o`). Antes, com
    `isr.asm` e `isr.c` na MESMA pasta `src/ntos/ke/amd64/`, ambos
    gravavam o mesmo `ntos_ke_amd64_isr.o` — o `.c` sobrescrevia o `.asm`
    e o link falhava com `undefined symbol: isr_stub_table`.

  **Evidencia** (build + run.ps1 -Headless): build verde com 47 .o
  (4 asm + 43 c), link OK, `kernel.bin` gerado. Boot: 16 `[ok]`, 0
  `[EXCECAO]`, 3x `DriverEntry status=0x0`, 3x `0xCAFEBABE` (IOCTL
  intacto), `Sistema no ar`. **calller.sys, mydriver.sys, ioctldriver.sys,
  guiapp.exe, desktop.exe, todos os apps anteriores rodam 1:1.**

## [0.3.0] - 2026-06-22

Terceira versão. Foco em **mini-hipervisor de software puro via Trap Flag**:
interceptação por single-step de CPUID/RDTSC/RDMSR em ring 0, sem VMX/SVM.
Build verde, boot verde, zero regressão.

**Resultado experimental:** o `pintok.sys` (driver real packed por VM
protector estilo VMProtect) faz exatamente **3 CPUIDs + 1 RDTSC + 1 RDMSR**
durante o `DriverEntry`, TODOS interceptados/mascarados com sucesso pelo
nosso handler. Vendor TCG escondido (leaves `0x40000000..0x4FFFFFFF` zeradas),
HypervisorPresent bit (CPUID.1.ECX[31]) limpo, TSC com deltas pequenos
constantes (1000 "ciclos" por leitura), MSRs lidos retornam zero. Apesar
disso, o pintok continua bailing com `STATUS_NOT_FOUND` — confirma de vez
que o check de bail dele NAO e via fingerprint de CPU/hypervisor. E logica
interna do packer baseada em outro estado (provavelmente concatenacao de
RegistryPath ou inspecao de algum campo do `DRIVER_OBJECT`).

### Adicionado

- **FASE 7.13 — Mini-hipervisor de software via Trap Flag**
  *(`src/cpu/isr.c`, `src/nt/driver.c`)*:
  - **Single-stepping em ring 0 via RFLAGS.TF=1**: antes de chamar
    `DriverEntry` o `driver.c` modifica a RFLAGS empilhada com
    `pushfq; orq $0x100, (%rsp); popfq` (TF bit 8 = 1). A CPU passa a
    disparar `#DB` (int 1) DEPOIS de cada instrucao executada pelo driver,
    sem que o driver perceba. Apos retornar do DriverEntry, TF e limpo
    com `andq $-257, (%rsp); popfq`.
  - **Handler `#DB` em `src/cpu/isr.c::try_intercept(r)`**: quando
    `g_intercept_cpuid=1` e `r->int_no==1`, le os bytes em `(rip-2)` (ou
    `rip-3` para RDTSCP) e identifica a instrucao que acabou de rodar:
    - **CPUID (0F A2)**: a CPU ja executou; rax/rbx/rcx/rdx contem os
      valores REAIS do TCG. Reescrevemos no `struct regs` salvo. Se
      `leaf >= 0x40000000 && leaf <= 0x4FFFFFFF` zera todos (esconde
      vendor `'TCGTCGTCGTCG'` e leaves de hypervisor). Se `leaf == 1`
      limpa bit 31 de ECX (HypervisorPresent). IRETQ restaura os
      registradores fake — driver ve "ambiente limpo".
    - **RDTSC (0F 31)**: ignora o TSC real (que ficaria EnORME devido
      ao overhead de single-step e revelaria que estamos sendo
      monitorados) e devolve `s_fake_tsc += 1000` — deltas pequenos
      consistentes que parecem hardware normal.
    - **RDTSCP (0F 01 F9)**: idem RDTSC + zera ECX (TSC_AUX).
    - **RDMSR (0F 32)**: loga o MSR pedido (1a vez de cada, p/ debug) e
      devolve EAX=0/EDX=0 (MSR "nao implementado").
  - **Anti-flooding de log**: arrays estaticos `s_seen[16]` (CPUID leaves
    ja logadas) e `s_msr_seen[32]` (MSRs ja logados) — primeira ocorrencia
    de cada loga, repeticoes ficam silenciosas. Contadores totais
    (`g_cpuid_intercepts`, `g_rdtsc_intercepts`, `g_rdmsr_intercepts`) na
    var global, expostos via `cpuid_intercept_count()` / `rdtsc_intercept_count()`
    / `rdmsr_intercept_count()`.
  - **Resumo logado em driver.c apos o DriverEntry**:
    `[io] intercept totals: CPUID xN  RDTSC xM  RDMSR xK`.
  - **Custo**: ~100-200 ciclos por instrucao do driver (overhead do #DB
    + handler + IRETQ). Para os ~50k de instrucoes do DriverEntry tipico,
    sao ~5-10ms de overhead — invisivel. Nao indicado para drivers que
    rodem horas (overhead total proibitivo), so para inspecionar a fase
    de inicializacao.
  - **Funciona em QEMU TCG puro**, sem VMX/SVM/KVM — TF e feature padrao
    do x86 desde os anos 80, disponivel em qualquer CPU.
  - **Evidencia (boot verde sem pintok)**: 16 `[ok]`, 0 `[EXCECAO]`,
    `Sistema no ar`, `calller.sys` intercepta 1 CPUID (o `calller_HalCpuid`
    inline em `DriverEntry`), todos os outros drivers passam sem intercept.
  - **Evidencia (boot com pintok.sys)**: 495 relocacoes aplicadas,
    DriverEntry executa, tracer captura 11 chamadas a APIs do ntoskrnl
    (RtlDuplicateUnicodeString, 5x ExAllocatePoolWithTag, 4x ExFreePool,
    RtlInitUnicodeString) **+ 3 CPUIDs interceptados + 1 RDTSC + 1 RDMSR
    mascarados**, e ainda assim retorna `STATUS_NOT_FOUND`. Sistema
    continua para "Sistema no ar" sem crash.
  - *Conclusao definitiva sobre pintok.sys*: o bail DEFINITIVAMENTE
    nao e via fingerprint de hypervisor/CPU. As 3 leaves de CPUID que
    o pintok le foram todas mascaradas (vendor zero, HypervisorPresent=0),
    o TSC e linear/fake, MSR retorna 0 — e mesmo assim ele decide nao
    rodar. O bail e logica interna do packer baseada em ALGO ELSE
    (provavelmente concatenacao de RegistryPath nao satisfaz padrao
    esperado, OU inspecao de campo do DRIVER_OBJECT que ainda nao
    preenchemos, OU validacao de bytes da propria imagem carregada).
    Sem desempacotar a VM `.grfn1`, nao temos como saber qual.
    **Mini-hipervisor de software funciona perfeitamente** — eu provei
    com os contadores. Para outros drivers reais nao-packados, esta
    infra esconde o ambiente TCG/QEMU automaticamente.

## [0.2.0] - 2026-06-22

Segunda versão pública. Foco em **compatibilidade com drivers de kernel reais
do Windows NT**: framework completo (FASE 7) + camadas de compat NT 10
(FASES 7.2..7.12). Build verde, boot verde, **zero regressão** vs v0.1.0.

**Marco arquitetural:** o test driver `calller.sys` (NT-style real, escrito
contra este framework) roda 100% — exercita callbacks Ps/Ob/Cm, KEVENT/
SpinLock/Pool/SystemThread/CPUID/MSR/Section/IRP via `IoCompleteRequest`.

**Resumo numérico:** 9 novos subsistemas (KPCR, CPU features, hypervisor
detect, cid table, MDL real, virtual memory, SEH, pool refinado, SYSCALL
MSRs); ~150 exports `ntoskrnl.exe` na tabela `ntkrnl_resolve`; ~25 arquivos
novos em `src/{ke,hal,mm,nt}/`; CR4/XCR0 gated por CPUID; KUSER_SHARED_DATA
em 0xFFFFF780_00000000; PspCidTable exposta; `__C_specific_handler` real +
page-fault map-zero recovery.

### Adicionado

- **FASE 7.12 — Debug session: rastreamento do `pintok.sys` (driver real packed) +
  hotfixes** *(build verde, boot verde, calller.sys 100%)*:
  - **Tracer global de chamadas a APIs do ntoskrnl** (`g_pintok_trace` em
    `src/nt/ntexec.c`, ligado em `src/nt/driver.c` apenas durante o `DriverEntry` de
    cada `.sys`). Funcoes instrumentadas com log condicional: `HalCpuid`, `HalReadMsr`,
    `HalWriteMsr` (em `src/hal/cpu.c`), `MmGetSystemRoutineAddress`,
    `ObReferenceObjectByHandle`, `ZwQuerySystemInformation`, `RtlGetVersion`,
    `RtlInitUnicodeString`, `RtlDuplicateUnicodeString`, `generic_zero_stub` (em
    `src/nt/ntexec.c`), `NtOpenKey_k` (em `src/nt/registry.c`), `KeInitializeEvent_k`,
    `KeInitializeSpinLock_k` (em `src/ke/sync.c`), `ExAllocatePoolWithTag_k`,
    `ExFreePoolWithTag_k` (em `src/ke/pool.c`). Cada chamada loga argumentos
    relevantes (path, MSR, leaf, tag, tamanho) — assim o ultimo `[trace]` antes do
    bail revela exatamente o que o driver acessou.
  - **HIPOTESE A: pre-criar a chave do servico no registro** — nova funcao
    `registry_create_driver_service(name)` em `src/nt/registry.c` cria
    `\Registry\Machine\System\CurrentControlSet\Services\<NomeSemSys>` com os
    valores padrao do NT: `Type=1` (SERVICE_KERNEL_DRIVER), `Start=0`
    (SERVICE_BOOT_START), `ErrorControl=1` (NORMAL), `ImagePath=\??\C:\Windows\
    System32\drivers\<nome>.sys` e subchave `Parameters` vazia. Chamada em
    `driver_build_and_entry` antes do `DriverEntry`. Drivers reais que abrem essa
    chave durante o `DriverEntry` agora encontram o caminho valido em vez de
    `STATUS_OBJECT_NAME_NOT_FOUND`.
  - **HIPOTESE B: preencher mais campos do `DRIVER_OBJECT`** — `src/nt/driver.c`:
    `DriverName` UNICODE_STRING populado com `\Driver\<NomeSemSys>`,
    `DriverSize` setado para `pi.size_image`, `DriverExtension` aponta para uma
    `DRIVER_EXTENSION` minima (DriverObject back-ref + ServiceKeyName = DriverName).
    No NT real esses campos sao inspecionados por algumas APIs (incluindo
    `IoGetDriverObjectExtension`).
  - **HIPOTESE C: disassembly + analise estrutural do `pintok.sys`** — bytes do
    entry point e listagem das sections mostram que e um driver **packed por
    protector comercial estilo VM**: section `.grfn1` de **42 MiB** (`0x282F2C4`)
    `R-X` com bytecode virtualizado, `.stub0` de 285 KiB com decryption stub,
    `_ENTROPY` (seed de randomizacao), entry point comeca com `E9 DE 18 19 00`
    (jmp +0x1918DE) saltando para dentro de `.grfn1` em `0x1DD26B`, seguido de
    bytes aleatorios (codigo encriptado). Disassembly do destino mostra prologo
    `mov [rsp+8], rcx; push rbx; sub rsp, 32; mov rbx, rdx; jmp +0x50; call +0x12C;
    loop -0x50; ...` — **`loop` em prologo + saltos quase-aleatorios sao assinatura
    de VM interpreter packer**. Constante `STATUS_NOT_FOUND` (0xC0000258) NAO existe
    literal no binario (`B8 58 02 00 C0` = 0 ocorrencias) — e construida em
    runtime pela VM via aritmetica obfuscada.
  - **DESCOBERTA: o bail NAO chama nenhuma API NT** — o tracer capturou apenas 11
    operacoes durante o `DriverEntry`: 1x `RtlDuplicateUnicodeString` (duplicando
    nosso RegistryPath), 5x `ExAllocatePoolWithTag` (118, 58, 124, 2048, 40 bytes),
    1x `RtlInitUnicodeString(NULL)` e 4x `ExFreePoolWithTag`. Pintok aloca,
    manipula buffers internos, libera tudo e retorna `STATUS_NOT_FOUND` **sem
    chamar `NtOpenKey`, `MmGetSystemRoutineAddress`, `ObReferenceObjectByHandle`,
    `HalCpuid`, `HalReadMsr` ou qualquer outra API instrumentada**. O check do
    bail e interno a VM do packer, baseado em estado computado a partir dos bytes
    ofuscados — nao em consulta ao kernel.
  - **HOTFIX da `run.ps1`**: o restore do git apos um regex broken (Set-Content
    com `$null`) trouxe a versao do "first commit" que tinha apenas
    `'test.exe', 'mydriver.sys'` na lista default. Restaurado para a lista
    completa de 18 modulos (DLLs + drivers + apps das FASES 1..6 + calller.sys).
    Sem essa restauracao, `run.ps1 -Headless` sem `-Modules` carregava test.exe
    sem as DLLs do sistema e batia Page Fault em ring 3 (rip=0/cr2=0 = call to
    NULL via IAT nao resolvida).
  - **Flag `-cpu qemu64,-hypervisor,vendor=GenuineIntel`** adicionada ao QEMU em
    `run.ps1`. Efeito parcial: vendor da CPU passa de `'AuthenticAMD'` para
    `'GenuineIntel'`, `CPUID.1.ECX bit 31` (HypervisorPresent) passa de **1 para
    0**. Porem as leaves `CPUID 0x40000000..0x40000005` continuam expostas pelo
    TCG do QEMU com vendor `'TCGTCGTCGTCG'` (hardcoded em `target/i386/cpu.c` do
    QEMU; nao desligavel por flag). Pintok continua bailing com `STATUS_NOT_FOUND`
    porque seu check inclui as leaves alem do bit 31.
  - *Limitacoes honestas:* o `pintok.sys` especifico nao pode ser destravado sem
    (a) desempacotar a VM `.grfn1` (engenharia reversa massiva), (b) implementar
    **single-step via Trap Flag** (`RFLAGS.TF=1` + handler `#DB` que sobrescreve
    EAX/EBX/ECX/EDX apos cada `cpuid` — software hypervisor primitivo, ~1us/inst
    de overhead, viavel mas nao implementado) ou (c) escrever um mini-hypervisor
    VMX/SVM (`vmxon`/`vmlaunch` + VMCS + EPT — semanas de trabalho). O framework
    NT do MeuOS esta correto — `calller.sys` (driver NT-style real) valida todas
    as APIs com sucesso. Drivers reais nao-packados que dependem das APIs publicas
    do ntoskrnl/HAL devem rodar; pintok bate num check de VM-detector dentro da
    propria VM do packer, nao em falta de API.
  - **Evidencia final** (boot sem pintok): 16 `[ok]`, 0 `[EXCECAO]`, 5x
    `DriverEntry status=0x0`, 3x `0xCAFEBABE` (IOCTL intacto), 0 `import nao
    resolvido`, `qemu.err.log` = 0 bytes, `Sistema no ar` x1. Boot com pintok:
    driver carrega + 495 relocacoes + DriverEntry executa as 11 operacoes acima
    + retorna `STATUS_NOT_FOUND` + driver descartado sem chamar DriverUnload +
    sistema chega em `Sistema no ar` (sem crash).

- **FASE 7.11 — Driver lifecycle correto + RegistryPath valido**
  *(`src/nt/driver.c`)*:
  - **`driver_call_unload` agora valida o ponteiro antes de chamar**: rejeita
    `NULL` ou `< 0x1000` (faixa onde so cabe lixo/page0). NT real tambem nunca
    pula direto para um endereco invalido — falha cedo e silenciosa.
  - **`driver_load` so chama `DriverUnload` se `DriverEntry` retornou
    `STATUS_SUCCESS`** — corrige o crash em `rip=0x3` quando o driver
    retornava `STATUS_NOT_FOUND` e nosso codigo chamava um `DriverUnload`
    invalido (`p->DriverUnload` poderia ser 0 ou um ponteiro que o packer setou
    para um endereco quase-NULL).
  - **`driver_build_and_entry` recebe `drv_name` e constroi um
    `UNICODE_STRING reg`** com o `RegistryPath` no formato do NT
    (`\Registry\Machine\System\CurrentControlSet\Services\<NomeSemSys>`) antes
    de chamar o `DriverEntry`. Antes passavamos `{0, 0, 0}` (Buffer=NULL),
    o que faz alguns drivers bailarem imediatamente.
  - **Tabela `nt_status_desc`** com 16 NTSTATUS comuns para o log ficar
    legivel: `STATUS_SUCCESS / STATUS_UNSUCCESSFUL / STATUS_NOT_IMPLEMENTED /
    STATUS_ACCESS_VIOLATION / STATUS_INVALID_PARAMETER / STATUS_NO_MEMORY /
    STATUS_ACCESS_DENIED / STATUS_OBJECT_NAME_NOT_FOUND /
    STATUS_INSUFFICIENT_RESOURCES / STATUS_NOT_SUPPORTED /
    STATUS_ENTRYPOINT_NOT_FOUND / STATUS_DLL_INIT_FAILED / STATUS_NOT_FOUND
    (0xC0000225 e 0xC0000258) / STATUS_PROCESS_IS_TERMINATING`.

- **FASE 7.10 — Page Fault map-zero recovery + SEH minimo**
  *(`src/cpu/isr.c`, `src/mm/paging.c`, `src/nt/seh.c`, `src/nt/ntexec.c`)*:
  - **`mm_map_zero_page(va)`** em `src/mm/paging.c/.h`: walk PML4 → PDPT → PD →
    PT com `ensure_table` (aloca tabela intermediaria se faltar), aloca um frame
    de 4 KiB, zera, mapeia `PRESENT|RW|USER`, flush TLB. Trata caso `PS=1`
    (entrada de 2 MiB no PD) e re-presence (pagina ja mapeada -> retorna OK).
  - **Page Fault handler com recuperacao automatica** (`src/cpu/isr.c`):
    extendido com `try_pagefault_recover(r, cr2)`. Heuristica: se `rip` esta na
    faixa de drivers carregados (`[16 MiB, 1 GiB)`) **e** `cr2` esta em
    `[16 MiB, ...)` (nao no kernel-nucleo) **e** o fault NAO foi I-fetch (bit 4
    do err_code), mapeia uma pagina zerada em `cr2` e RETORNA sem halt. A CPU
    re-executa a instrucao que faultou (agora com a pagina presente).
    **Anti-loop**: historico circular de 4 slots `(rip, cr2)`; se o mesmo par
    repetir 3 vezes, abandona a recuperacao e halt — evita boot looping
    infinito recuperando.
  - **`src/nt/seh.c`** *(novo)*: `__C_specific_handler` real (era stub trivial)
    — `ms_abi`, detecta `EXCEPTION_UNWINDING`, loga 1x, retorna
    `ExceptionContinueSearch` (1) — propaga para o frame externo. **Drivers que
    usam `__try`/`__except`** agora tem um handler que nao crasha (mesmo sem
    scope-table walking completo). `NT_ProbeForRead_real`/`NT_ProbeForWrite_real`
    validam alinhamento + range (`USER_PROBE_LIMIT=0x7FFFFFFE_FFFF`); enderecos
    invalidos sao logados (sem raise de exception por enquanto — TODO quando o
    SEH completo).
  - Exports atualizados em `ntexec.c`: `__C_specific_handler`, `ProbeForRead`,
    `ProbeForWrite` apontam para as implementacoes reais.

- **FASE 7.9 — `Mm{Allocate,Free,Protect}VirtualMemory` + pool refinado**
  *(`src/mm/virtual_memory.c/.h`, `src/ke/pool.c/.h`, `src/nt/ntexec.c`)*:
  - **`src/mm/virtual_memory.c/.h`** *(novo)*: `MmAllocateVirtualMemory_k`
    (pmm_alloc_contiguous + identidade-mapeada, zero-init, retorna
    `STATUS_INSUFFICIENT_RESOURCES` em OOM sem crash); `MmFreeVirtualMemory_k`
    (`MEM_RELEASE` libera frames, `MEM_DECOMMIT` preserva); `MmProtectVirtualMemory_k`
    (no-op + `OldProtect=PAGE_READWRITE`; TODO PTE walking quando MMU per-processo
    amadurecer).
  - **`src/ke/pool.c/.h`** expandido: cabecalho `POOL_HDR` agora guarda `PoolType`
    (1 byte) alem de `Tag+Size+Magic`, com contadores separados
    `s_bytes_paged/s_bytes_nonpag` e API `pool_bytes_paged/_nonpaged` para
    diferenciar `PagedPool` vs `NonPagedPool` em `pool_dump`.
  - **9 novos exports em ntexec.c**: `MmAllocateVirtualMemory`,
    `MmFreeVirtualMemory`, `MmProtectVirtualMemory`, `NtAllocateVirtualMemory`,
    `ZwAllocateVirtualMemory`, `NtFreeVirtualMemory`, `ZwFreeVirtualMemory`,
    `NtProtectVirtualMemory`, `ZwProtectVirtualMemory`.

- **FASE 7.8 — Hypervisor detection + `HalCpuidEx`**
  *(`src/hal/hypervisor.c/.h`, `src/nt/ntexec.c`)*:
  - `hv_detect_init()` le `CPUID.1.ECX` bit 31 (HypervisorPresent) + leaves
    `0x40000000..0x40000005` e loga tudo na serial — passivo, sem modificar
    estado. Achado em QEMU TCG no Windows: HypervisorPresent=1,
    vendor='TCGTCGTCGTCG' (EBX=0x54474354/ECX=0x43544743/EDX=0x47435447),
    max_leaf=0x40000001, leaf 0x40000001 = zeros. Documentado que sem VMX/SVM
    nao podemos forcar leaves customizadas (`cpuid` nao gera #UD em leaves
    invalidas — retorna lixo, nao excecao, entao SIGILL handler nao ajudaria).
  - Adicionados `HalReadMsr`/`HalWriteMsr`/`HalCpuid`/`HalCpuidEx` a tabela de
    exports do ntoskrnl (faltavam).

- **FASE 7.7 — `cpu_features_init` (CR4 + XCR0)**
  *(`src/ke/cpu_features.c/.h`, `src/kernel.c`)*:
  - Le `CPUID.1.ECX` (XSAVE/AVX/PCID), `CPUID.7.0.EBX/ECX` (SMEP/SMAP/UMIP) e
    `CPUID.D.0.EAX` (mascara XCR0 valida). Cada bit so e setado em CR4 se o
    CPUID expoe — gates eliminam `#GP` por bit reservado.
  - **Ordem segura**: `OSXSAVE` primeiro (bit 18), depois `xsetbv XCR0=X87|SSE
    (|AVX se suportado)`, depois `SMEP|UMIP|PCIDE`. `SMAP` fica detectado MAS
    NAO setado (kernel ainda copia para paginas user no PE loader sem
    `STAC/CLAC`; setar SMAP gera #PF imediato la).
  - Em QEMU TCG `AuthenticAMD` nenhum bit novo foi exposto pelo CPUID, entao
    nada extra foi ligado (caminho seguro). Em HW real ou QEMU com `-cpu host`,
    OSXSAVE/SMEP/UMIP/PCIDE serao ligados automaticamente e XCR0=0x7 sera
    escrito.

- **FASE 7.6 — MDL real com layout NT**
  *(`src/mm/mdl.c/.h`, `sdk/ntddk.h`, `src/nt/ntexec.c`)*:
  - `sdk/ntddk.h` ganhou typedef `MDL` com campos corretos (Next +0x00 /
    Size +0x08 / MdlFlags +0x0A / Process +0x10 / MappedSystemVa +0x18 /
    StartVa +0x20 / ByteCount +0x28 / ByteOffset +0x2C + array de
    `PFN_NUMBER` apos +0x30); flags `MDL_*` completas, `LOCK_OPERATION`,
    macros `BYTES_TO_PAGES`/`ADDRESS_AND_SIZE_TO_SPAN_PAGES`.
  - `src/mm/mdl.c/.h` *(novo)*: `IoAllocateMdl_k` aloca cabecalho + PFN[] via
    pool com tag 'MdlR' e preenche PFNs em identidade (PFN=vaddr>>12 valido no
    nosso 1o GiB identidade-mapeado). `MmProbeAndLockPages_k` marca
    `MDL_PAGES_LOCKED | MDL_WRITE_OPERATION`. `MmMapLockedPagesSpecifyCache_k`
    mapeia identidade (`StartVa+ByteOffset`) e marca `MDL_MAPPED_TO_SYSTEM_VA`.
    `MmUnlockPages_k`, `MmUnmapLockedPages_k`, `IoFreeMdl_k`,
    `MmBuildMdlForNonPagedPool_k` tambem implementados.
  - Stubs antigos de 64 bytes opacos em `ntexec.c` REMOVIDOS; novos exports:
    `MmProbeAndLockPages`, `MmMapLockedPages`, `MmMapLockedPagesSpecifyCache`,
    `MmUnlockPages`, `MmUnmapLockedPages`, `MmBuildMdlForNonPagedPool`,
    `MmGetSystemAddressForMdlSafe`, `MmGetMdlVirtualAddress`,
    `MmGetMdlByteCount`, `MmGetMdlByteOffset`, `MmSizeOfMdl`, `IoAllocateMdl`,
    `IoFreeMdl`.

- **FASE 7.5 — `PspCidTable` estilo NT**
  *(`src/nt/cid_table.c/.h`, `src/nt/process.c`, `src/ke/systhread.c`,
   `src/nt/ntexec.c`, `src/kernel.c`)*:
  - Array linear de 1024 entradas `(cid, ptr)` indexando PIDs/TIDs para
    `EPROCESS`/`ETHREAD`. `PsCreateProcess`/`PsCreateThread` inserem;
    `PsTerminateProcess` remove.
  - `PsLookupProcessByProcessId_k`/`PsLookupThreadByThreadId_k` agora usam
    caminho rapido via CID table; caem no fallback `ob_enum_by_type` se nao
    acharem (rede de seguranca para compat).
  - **Variavel global `g_pspcid_table`** (struct `CID_HANDLE_TABLE` com
    `TableCode` 0x00, `QuotaProcess` 0x08, `UniqueProcessId` 0x10, `HandleLock`
    0x18, `HandleTableList` 0x20, `HandleCount` 0x54) exposta como `PspCidTable`
    via `ntexec.c` — drivers que fazem reach-around direto ao CID table agora
    encontram a estrutura.
  - `cid_init()` chamada em `kmain` antes de `ps_init`.

- **FASE 7.4 — SYSCALL/SYSENTER MSRs**
  *(`src/ke/syscall_entry.asm`, `src/ke/msr_init.c`, `src/kernel.c`)*:
  - `src/ke/syscall_entry.asm` *(novo)*: handler ring 0 chamado pela CPU em
    `syscall`, monta o quadro `struct regs` identico ao `isr_common` e desvia
    para `isr_handler` com `int_no=0x80`, retornando via `IRETQ`.
  - `src/ke/msr_init.c` *(novo)*: `syscall_msr_init()` programa `EFER.SCE=1`,
    `STAR=0x0010000800000000` (STAR.kernel=0x08=SEL_KCODE, STAR.user=0x10
    neutro), `LSTAR=&syscall_entry`, `CSTAR=&syscall_entry`, `SFMASK=0x4700`
    (DF, IF, TF clear ao entrar).
  - `kmain` chama `syscall_msr_init()` apos `kpcr_init()`. `int 0x80` INTOCADO;
    o dispatch C (`syscall_dispatch` em `ke/syscall.c`) e o mesmo para AMBOS os
    caminhos. Drivers que fazem syscalls diretos via instrucao `syscall` agora
    funcionam.
  - Logs no boot confirmam: `EFER=0x0000000000000501 STAR=0x0010000800000000
    LSTAR=0x0000000000100360 SFMASK=0x0000000000004700` + `SCE habilitado`.

- **FASE 7.3 — EPROCESS/ETHREAD layout NT 10 x64**
  *(`src/nt/process.h/.c`)*:
  - Expansao das structs para ~2 KiB cada. Campos legacy (ProcessId,
    ThreadCount, DirectoryTableBase, ImageBase, PML4, ExitStatus, Terminated,
    ImageName, Thread / ThreadId, Process, StartAddress, StackBase, StackTop)
    PRESERVADOS no topo de cada struct — todo codigo existente (syscall.c,
    loader.c, systhread.c, usermode.c, callbacks.c, win32k.c) compila e roda
    1:1.
  - **Espelho NT em offsets exatos**:
    EPROCESS: +0x180 `PageFaultsCount`, +0x440 `UniqueProcessId`, +0x448
    `ActiveProcessLinks` (lista circular inicializada Flink=Blink=self),
    +0x4B8 `InheritedFromUniqueProcessId`, +0x550 `Peb`, +0x568
    `Wow64Process`, +0x5A8 `ImageFileName[15]`, +0x650 `ActiveThreads`,
    +0x710 `ObjectTable`.
    ETHREAD: +0x180 `InitialStack` / +0x188 `StackLimit` / +0x190
    `KernelStack`, +0x1F8 `TrapFrame`, +0x420 `CreateTime`, +0x650
    `Cid_UniqueProcess` / +0x658 `Cid_UniqueThread`, +0x6B0
    `ImpersonationInfo`, +0x7C8 `ThreadName`.
  - `PsCreateProcess`/`PsCreateThread` preenchem AMBOS (legacy + espelho NT).
    `_Static_assert` no `.c` trava cada offset critico. `ObCreateObject` ja
    zera o corpo, entao `_padX[]` todos ficam 0 (caminho seguro).

- **FASE 7.2 — KPCR/KPRCB no `GS_BASE`**
  *(`src/ke/kpcr.c/.h`, `src/kernel.c`, `src/nt/ntexec.c`)*:
  - `src/ke/kpcr.h`: layout completo KPCR (4096 bytes) + KPRCB inline a +0x180:
    `+0x000 GdtBase`, `+0x008 TssBase`, `+0x010 UserRsp`, `+0x018 Self` (KPCR*),
    `+0x020 CurrentPrcb` (=&this->Prcb), `+0x030 Used_Self`, `+0x038 IdtBase`,
    `+0x0F4/0x0F6 MajorVersion/MinorVersion=1/1`, `+0x100
    StallScaleFactor=100`, `+0x180 Prcb` (KPRCB inline).
  - `kpcr_init` aloca via `kmalloc(sizeof(KPCR))`, zera, preenche Self/Used_Self/
    CurrentPrcb (auto-referenciado), Major/Minor/Stall, e KPRCB.CurrentThread
    (ponteiro KTHREAD fake de 512 B). Programa `IA32_GS_BASE` (0xC0000101) e
    `IA32_KERNEL_GS_BASE` (0xC0000102) com o endereco do KPCR — drivers podem
    ler `gs:[0x18]` (Self), `gs:[0x190]` (CurrentThread), `gs:[0x200]`
    (ProcessorNumber).
  - Novos exports `KeGetCurrentProcessorNumber` / `KeGetCurrentProcessorNumberEx`
    que leem `gs:[0x200]` inline.
  - `kpcr_init()` chamado em `kmain` apos `hal_cpu_init` e antes de
    `callbacks_init`. Log: `[kpcr] KPCR @ 0x2001088 | Self=0x2001088 |
    Prcb=0x2001208 (=+0x180) | gs_base=0x2001088`.

- **FASE 7 — Driver Framework completo (NT-compatible)** *(build verde, boot verde, nada regrediu)*:
  - **`sdk/ntddk.h` massivamente expandido**: tipos completos do WDM (PHANDLE, ACCESS_MASK,
    LARGE_INTEGER, OBJECT_ATTRIBUTES, CLIENT_ID, KEVENT/KMUTEX/KSEMAPHORE/KSPIN_LOCK com
    DISPATCHER_HEADER, POOL_TYPE/MEMORY_CACHING_TYPE, SECTION_INHERIT, todas as IRP_MJ_*
    (CREATE/CLOSE/READ/WRITE/QUERY/SET/EA/FLUSH/VOLUME/DIR/FSCONTROL/IOCTL/INTERNAL/SHUTDOWN/
    LOCK/CLEANUP/MAILSLOT/SECURITY/POWER/SYSTEM/CHANGE/QUOTA/PNP), prototipos de callbacks
    (PCREATE_PROCESS_NOTIFY_ROUTINE/Ex, PCREATE_THREAD_NOTIFY_ROUTINE, PLOAD_IMAGE_NOTIFY,
    POB_OPERATION_CALLBACK, PEX_CALLBACK_FUNCTION), tabela de status codes (NT_SUCCESS macro),
    KdDebuggerEnabled/KdDebuggerNotPresent como `extern BOOLEAN`, PsProcessType/PsThreadType/
    IoFileObjectType como ponteiros publicos, IRQL (PASSIVE/APC/DISPATCH).
  - **`src/nt/callbacks.{c,h}`** *(novo)*: tabelas de registro para Ps/Ob/Cm/Ex callbacks
    (cap 16 por tipo), com `PsSetCreate*NotifyRoutine`/`PsRemoveCreate*NotifyRoutine`/
    `PsSetLoadImageNotifyRoutine` (+ remove), `ObRegisterCallbacks`/`ObUnRegisterCallbacks`,
    `CmRegisterCallback`/`CmRegisterCallbackEx`/`CmUnRegisterCallback`, `ExRegisterCallback`/
    `ExUnregisterCallback`. Disparadores `callbacks_fire_*` (kernel-side) integrados ao
    process/thread/image lifecycle e ao registro. Cada registro/disparo logado na serial.
  - **`src/ke/sync.{c,h}`** *(novo)*: sincronizacao do KE: `KeInitializeEvent`/`KeSetEvent`/
    `KeResetEvent`/`KeClearEvent`/`KeReadStateEvent`, `KeWaitForSingleObject`/
    `KeWaitForMultipleObjects` (auto-resolve sem scheduler — caminho seguro headless),
    `KeDelayExecutionThread` (usa PIT 100 Hz para delay real), spin locks
    (`KeInitializeSpinLock`/`KeAcquireSpinLockRaiseToDpc`/`KeReleaseSpinLock`/dpc level),
    IRQL (`KeGetCurrentIrql`/`KeRaiseIrql`/`KeLowerIrql`), mutex/semaphore,
    `KeQuerySystemTime`/`KeQueryInterruptTime`/`KeQueryTimeIncrement`.
  - **`src/ke/pool.{c,h}`** *(novo)*: `Ex*Pool*` em cima do kmalloc com cabecalho oculto
    (Tag + Size + Magic 'POOL'); `ExAllocatePool`/`ExAllocatePoolWithTag`/
    `ExAllocatePool2`/`ExAllocatePool3`/`ExAllocatePoolUninitialized`, `ExFreePool`/
    `ExFreePoolWithTag`. Stats (`pool_total_allocs`/`pool_bytes_outstanding`).
  - **`src/ke/systhread.{c,h}`** *(novo)*: `PsCreateSystemThread` (execucao INLINE sem
    scheduler — chama a entry imediatamente; KeWait* auto-resolvem), `PsTerminateSystemThread`.
    Inspecao de processo: `PsGetCurrentProcessId`/`PsGetCurrentProcess`/`PsGetCurrentThreadId`/
    `PsGetCurrentThread`/`PsGetProcessId`/`PsGetProcessImageFileName`/`PsGetProcessPeb`/
    `PsGetProcessWow64Process`/`PsGetProcessCreateTimeQuadPart`/`PsGetProcessExitStatus`/
    `PsIsProtectedProcess`/`PsIsProtectedProcessLight`/`PsLookupProcessByProcessId`/
    `PsLookupThreadByThreadId`.
  - **`src/nt/section.{c,h}`** *(novo)*: Section objects (`NtCreateSection`/`NtOpenSection`/
    `NtMapViewOfSection`/`NtUnmapViewOfSection`) + Mm* helpers (`MmGetPhysicalAddress`/
    `MmMapIoSpace`/`MmUnmapIoSpace`/`MmAllocateContiguousMemory`/`MmAllocateNonCachedMemory`/
    `MmIsAddressValid`/`MmProtectMdlSystemAddress`). Identidade-mapeada: a section
    devolve o proprio buffer base — kernel e ring 3 veem os MESMOS bytes.
  - **`src/nt/registry.{c,h}`** *(novo)*: hive em memoria com arvore de keys + values;
    `NtCreateKey`/`NtOpenKey`/`NtCloseKey`/`NtDeleteKey`/`NtEnumerateKey`/
    `NtEnumerateValueKey`/`NtSetValueKey`/`NtQueryValueKey`/`NtDeleteValueKey`/`NtFlushKey`
    (todos com Zw* aliasados). Namespace `\Registry\Machine\Software\MeuOS` pre-criado
    com `ProductName`/`CurrentVersion`/`BuildNumber`. Cada operacao dispara o callback Cm.
  - **`src/hal/cpu.{c,h}`** *(novo)*: `HalReadMsr`/`HalWriteMsr` (rdmsr/wrmsr), `HalCpuid`,
    `hal_cpu_init()` (le vendor/family/model/stepping), `hal_rdtsc`,
    `KeQueryActiveProcessorCount`/`KeQueryActiveProcessors`/`KeQueryPerformanceCounter`.
    Logada uma linha `[cpu] vendor='...' family=... model=... stepping=...` no boot.
  - **I/O Manager expandido** (`src/nt/io.{c,h}`): rotinas de suporte completas —
    `IoAllocateIrp`/`IoFreeIrp`/`IoInitializeIrp`/`IoGetCurrentIrpStackLocation`/
    `IoGetNextIrpStackLocation`/`IoSkipCurrentIrpStackLocation`/`IoCopyCurrentIrpStackLocationToNext`/
    `IoSetCompletionRoutine` (dispara no `IoCompleteRequest`)/`IoCompleteRequest`/
    `IoCancelIrp`/`IoBuildAsynchronousFsdRequest`/`IoBuildDeviceIoControlRequest`/
    `IoCreateSymbolicLink` (com tabela interna `\DosDevices\X` -> `\Device\X`)/
    `IoDeleteSymbolicLink`/`IoGetDeviceObjectPointer`/`IoReleaseRemoveLockAndWait`.
  - **`src/nt/ntexec.c` MASSIVAMENTE expandido**: tabela `ntkrnl_resolve` com ~150 exports
    cobrindo Debug (DbgPrint/DbgPrintEx/DbgPrompt/KdDebuggerEnabled/KeBugCheck),
    Rtl (UnicodeString/AnsiString conversoes, CompareMemory/CopyMemory/MoveMemory/ZeroMemory/
    FillMemory, RtlGetVersion -> Windows 10 build 26100), Io (todas as rotinas de suporte
    + IoCreateDevice/IoDeleteDevice + symbolic links + IoFileObjectType),
    Callbacks (Ps/Ob/Cm/Ex + PsProcessType/PsThreadType), System threads (PsCreateSystemThread
    + Ps* de inspecao), Sincronizacao (Ke*: events/spinlocks/mutex/semaphore/wait/delay/
    IRQL/system time/performance counter/active processors), Pool (Ex*Pool* incl. Pool2/3),
    Memoria (Mm* contiguous/noncached/physical/MdlSystemAddress/IsAddressValid/
    MmGetSystemRoutineAddress), Section objects, Registry (Cm/Nt/Zw todos), NtQuerySystemInformation
    (SystemBasicInformation/CodeIntegrity/SecureBootPolicy/VsmProtection/FirmwareTable),
    HAL ports, Cache Manager stubs (Cc*), Security stubs (Se*), Bcrypt/Ci stubs
    (sempre STATUS_SUCCESS — caminho seguro). **Fallback generic_zero_stub**: exports
    nao listados nao quebram o load — sao redirecionados para um stub que retorna 0 e
    logam 1x ("[ntex] stub generico p/ 'nome'"). Imports nao resolvidos viram **ZERO** ocorrencias.
  - **Test driver `examples/calller.c` -> `calller.sys`** (PE NATIVE, ImageBase 0x3A00000):
    exercita TODO o framework. DriverEntry: cria `\Device\Calller` + symbolic link
    `\DosDevices\Calller`, registra dispatch para IRP_MJ_CREATE/CLOSE/CLEANUP/DEVICE_CONTROL
    (com IOCTLs ECHO 0x900 + SECTION 0x901), inicializa KEVENT + KSPIN_LOCK, faz
    `ExAllocatePoolWithTag(256)`+free, CPUID+MSR (inline pois sao intrinsics no WDK),
    registra 3 notify routines Ps* (process/thread/image) + 1 Cm (registry), cria uma
    section de 4 KiB com `NtCreateSection`, cria uma system thread que faz 3 ticks
    e sai (KeWaitForSingleObject auto-resolve), e por fim DriverUnload (deleta o symlink
    + device). Build VERDE, carrega VERDE, executa VERDE.
  - **`src/kernel.c`** ganha `hal_cpu_init()` + `callbacks_init()` + `registry_init()`
    apos `hal_init()` e antes do Process Manager. Nova linha `[ok] FASE 7: Callbacks
    (Ps/Ob/Cm/Ex) + Registro em memoria (\Registry\)`.
  - **`build.ps1`** compila `calller.c` apos os outros drivers; **`run.ps1`** carrega
    `calller.sys` na lista default antes dos `.exe`.
  - *Limitacoes honestas (relatadas; NAO bloqueiam o boot):* (1) **Sem escalonador**:
    `KeWaitForSingleObject`/`KeDelayExecutionThread` retornam STATUS_SUCCESS imediato
    se nao sinalizados (caminho seguro). System threads executam INLINE — drivers que
    fazem "while(1) wait" ficariam pendurados; oferecemos limite de iteracoes opcional.
    (2) **Sem hive persistente**: o registro fica em RAM e some no reboot. TODO:
    serializar para NTFS (a infra de escrita ja existe — FASE 4 NTFS). (3) **Crypto
    e CI sao stubs que sempre retornam SUCCESS** — pelo caminho seguro pedido, "for
    testing" como diz o spec; checagens de integridade nao sao realmente feitas.
    (4) **Pool nao distingue paged/nonpaged**: kmalloc unico (identidade); drivers
    que dependem de comportamento de paginacao podem ter surpresas. (5) **PsCreateSystemThread
    inline** = a thread roda na mesma pilha do chamador no momento da criacao; resultado:
    o "loop" do driver executa N iteracoes e sai (definido por `systhread_set_max_iterations`).
    (6) **IRP_MJ_PNP/POWER/SHUTDOWN**: defines adicionados e drivers podem registrar
    handlers (cabe nos 28 slots de DRIVER_OBJECT.MajorFunction), mas o kernel nunca
    envia esses IRPs por si — so se um driver chamar `IoCallDriver` com eles. (7) Build
    do calller.sys faz **inline de KeInitializeSpinLock/HalReadMsr/HalCpuid** porque
    Zig's libntoskrnl.a (mingw-w64) nao tem stubs deles; no NT real eles tambem sao
    macros/intrinsics no header. **Evidencia final**: 16 `[ok]`, `qemu.err.log` = 0 bytes,
    0 `[EXCECAO]`/Page Fault/triple-fault/`syscall desconhecida`/`import nao resolvido`/
    `stub generico`. 5x `DriverEntry status=0x0` (era 4, +1 do calller.sys). 3x `0xCAFEBABE`
    (IOCTL anterior intacto). Linha por linha: KEVENT/SpinLock + ExAllocatePool + CPUID/MSR
    + 3 Ps* notify + 1 Cm callback + NtCreateSection(4096) + system thread (3 ticks)
    + DriverEntry concluido + DriverUnload — TUDO logado na serial.

- **RODADA HAL/NTFS — HAL (PCI/disco) + driver NTFS (leitura/escrita) + volume C:**
  (5 fases; build verde, boot verde, nada anterior regrediu):
  - **HAL core** (`src/hal/hal.{c,h}`, pasta nova): camada estilo HAL.DLL do NT —
    **I/O ports** (`HalReadPortUchar/Ushort/Ulong` + `HalWritePort*`, `ms_abi`),
    **MMIO** (`hal_map_mmio` devolve ponteiro identidade para faixas < 1 GiB e
    **recusa** faixas acima, sem mexer nas page tables; `HalReadMmio*`/`HalWriteMmio*`)
    e **enumeracao PCI** via mecanismo de configuracao #1 (portas **0xCF8/0xCFC**):
    `HalPciReadConfig*`/`WriteConfigUlong`; `hal_pci_enumerate()` varre bus/device/
    function, le vendor/device/class/subclass/header-type + as 6 BARs (I/O vs MMIO),
    trata multifuncao; helpers `hal_pci_count/get/find_class`. `hal_init()` em `kmain`
    (apos heap/PMM, antes do Object Manager) loga cada dispositivo (regra 4) —
    destaca o controlador IDE/ATA, a video e o host bridge.
  - **HAL disco** (`src/hal/disk.{c,h}`, novos): driver IDE por **ATA PIO, LBA28**,
    canal primario master (0x1F0-0x1F7 + 0x3F6). `hal_disk_init()` faz **IDENTIFY
    DEVICE** (modelo + nº de setores); `HalReadSector`/`HalWriteSector` (READ/WRITE
    SECTORS + CACHE FLUSH, `ms_abi`, com timeout e checagem de ERR/DRQ). Cada setor
    lido/escrito logado na serial (regra 4).
  - **Imagem NTFS de teste**: `examples/make-ntfs-disk.ps1` (gera `build\disk.img` —
    modo **REAL** com admin: `New-VHD` -> `Mount-VHD` -> `New-Partition -MbrType IFS`
    -> `Format-Volume NTFS` -> copia arquivos -> `qemu-img convert -O raw`; ou modo
    **SINTETICO** sem admin via Python) e `examples/make-ntfs-image.py` (constroi os
    bytes na mao: MBR particao 0x07 @LBA 2048, boot sector `NTFS    ` + BPB + `$MFT`
    minima com `\hello.txt` residente e `\dir1\file.txt`, com fixups USA).
  - **`run.ps1 -Disk` / `-DiskImage`**: anexa `build\disk.img` (ou imagem indicada)
    como **IDE primario master** (`-drive ...,format=raw,if=ide,index=0,media=disk`);
    auto-gera a imagem sintetica se faltar (apos `-Clean`). Sem `-Disk`, o
    comportamento default (so `-kernel` + `-initrd`) fica **inalterado**.
  - **NTFS LEITURA** (`src/drivers/ntfs.{c,h}`, `src/drivers/ntfs_fs.c`, novos):
    `ntfs_mount` (valida `NTFS    ` @3, faz o parse do BPB — bytes/setor, setores/
    cluster, `$MFT` LCN, tam. de registro); leitura de registro MFT com **assinatura
    `FILE` + fixups (USA)**; iteracao de atributos **residentes E nao-residentes
    (data runs / mapping pairs)**; `$STANDARD_INFORMATION`/`$FILE_NAME` (UTF-16->ASCII,
    namespace Win32); leitura de `$DATA`; **listagem de diretorio** (`$INDEX_ROOT`
    residente + `$INDEX_ALLOCATION`/blocos `INDX` nao-residentes); **resolucao de
    caminho** (`\`, `\hello.txt`, `\dir1\file.txt`). **Camada de FS ligada ao I/O
    Manager** (`ntfs_fs.c`): `FsMountVolume` cria um `DRIVER_OBJECT`
    (`\FileSystem\Ntfs`) e um `DEVICE_OBJECT` de volume (`\Device\Harddisk0\Partition1`)
    com `DRIVER_OBJECT.MajorFunction` (`ms_abi`) para **IRP_MJ_CREATE/CLOSE/READ/
    DIRECTORY_CONTROL** (este devolve UMA entrada por IRP, estilo `NtQueryDirectoryFile`).
    `IRP_MJ_DIRECTORY_CONTROL`=0x0C adicionado ao `sdk/ntddk.h`.
  - **NTFS ESCRITA (subconjunto seguro)** (`src/drivers/ntfs.c`, `ntfs_fs.c`):
    `ntfs_write_mft_record` (inverso da leitura — **reaplica os fixups na escrita**);
    `ntfs_write_file` (sobrescrever/**crescer**/encurtar `$DATA` **residente**, com
    *validate-before-write* e atualizacao de `$FILE_NAME` + entrada do `$INDEX_ROOT`
    do pai; overwrite de `$DATA` **nao-residente** in-place nos clusters ja alocados);
    `ntfs_create_file` (registro MFT livre + `FILE`/`$STD_INFO`/`$FILE_NAME`/`$DATA`
    ou `$INDEX_ROOT` vazio + insere no `$INDEX` do pai) e `ntfs_delete_file` (remove
    do `$INDEX` do pai + marca o registro nao-em-uso). Handler **IRP_MJ_WRITE** no
    device de volume.
  - **Syscalls / ring 3**: `NtQueryDirectoryFile` (**44**), `NtQueryVolumeInformation`
    (**45**), sincronizadas em `dll/ntdll.c`. `NtCreateFile`/`NtReadFile`/`NtWriteFile`/
    `NtQueryDirectoryFile` reconhecem o caminho do volume — **forma NT crua**
    (`\Device\Harddisk0\Partition1[\...]`) **e forma DOS `C:`** (`C:\hello.txt`, `C:`),
    montando os IRPs com `Key`=registro MFT + `ByteOffset` corrente. `FILE_OBJECT`
    (`src/nt/io.h`) ganhou `FsContext`/`CurrentByteOffset`/`IsDirectory`.
  - **DLLs**: `ntdll` exporta `NtQueryDirectoryFile`/`NtQueryVolumeInformation`;
    `kernel32` exporta `QueryDirectoryFileEx` (+`MEUOS_DIR_ENTRY`) e
    `QueryVolumeInfoEx` (+`MEUOS_VOLUME_INFO`).
  - **`cmd.exe`** (`examples/cmd.c`): comandos de arquivo sobre o **C:** em ring 3 —
    `dir [caminho]` (lista via `QueryDirectoryFileEx`), `cd` (CWD + prompt que segue
    o diretorio), `type` (via `ReadFile`), `copy` (sobrescreve destino existente via
    `WriteFile` -> escrita NTFS), `vol` (via `QueryVolumeInfoEx`), `del` (stub
    honesto). Modo DEMO roda esses comandos automaticamente p/ headless. `sc`/
    `tasklist`/`help` mantidos.
  - **Testes no boot** (`src/kernel.c`): `disk_test()` (MBR + boot sector NTFS, write/
    readback nao destrutivo) e `ntfs_test()` (monta o volume, lista a raiz, le
    `\hello.txt` == texto conhecido, exercita IRPs CREATE/READ/DIRECTORY_CONTROL, e
    a ESCRITA: sobrescrita/grow/criar/excluir + IRP_MJ_WRITE), so com `-Disk`.
  - *Limitacoes (relatadas, NAO bloqueiam):* sem **alocacao de clusters** (`$Bitmap`)
    — escrita so onde ja ha espaco; sem **journaling** (`$LogFile`); `$ATTRIBUTE_LIST`
    e `$INDEX_ALLOCATION` na escrita fora do escopo seguro; `$MFT:$BITMAP` nao
    atualizado ao criar; espaco livre do `vol` e estimativa; APIC/HPET NAO feitos
    (PIC/PIT mantidos). Imagem de teste **sintetica** (sem admin) — para exercitar os
    caminhos nao-residente/INDX, gere um NTFS autentico com `make-ntfs-disk.ps1` como
    Administrador. Build verde, boot verde; nada anterior regrediu.
- **FASE 6 — Desktop + barra de tarefas + `cmd` numa janela (integração GUI+CMD)**:
  - **win32k (kernel)** (`src/win32/win32k.c`+`.h`): **papel de parede** (janela de
    fundo, flag `WNDF_DESKTOP`: cobre a tela, sem chrome, fora do z-order de topo,
    não toma foco) + **janela de console** (flag `WNDF_CONSOLE`: área cliente
    escura, com chrome) + **barra de tarefas** desenhada no `compose` (botão
    "Iniciar" + um botão por janela, com a janela em foco destacada). `W32_CREATE`
    ganhou `bgColor`+`flags` (layout compatível; `CreateWindowExA` antigo intacto).
    **Reaping** das janelas de um processo quando ele encerra
    (`win32k_reap_process_windows`, chamado no `ldr_run`) + `win32k_was_active()`
    (mantém o framebuffer no estado da GUI). **Coalescing de `WM_PAINT`** + fila de
    mensagens ampliada para **256**. `NtUserSetFocus` (foco/Alt+Tab), `NtUserPostKey`
    (injeta tecla numa janela específica) e `NtGdiTextOutEx` (TextOut com cor de
    texto + clipping à área cliente).
  - **Syscalls novas** (`ke/syscall.c`), números **41..43**, sincronizadas com
    `dll/ntdll.c`: `NtUserSetFocus` (41), `NtUserPostKey` (42), `NtGdiTextOutEx` (43).
  - **DLLs** (ring 3): `user32` ganha `SetFocus`, `PostKeyToWindow`,
    `CreateDesktopWindowA`, `CreateConsoleWindowA`; `gdi32` ganha `SetTextColor`
    (cor por HDC) e roteia `TextOutA` por `NtGdiTextOutEx` quando há cor; `ntdll`
    exporta os 3 `Nt*` novos.
  - **`examples/desktop.c`** (`desktop.exe`, PE32+ x64, ImageBase 0x3800000): sobe o
    desktop (papel de parede + barra de tarefas), abre **2 janelas de cmd**, "digita"
    comandos em cada uma (via `PostKeyToWindow`, determinístico headless) alternando
    o foco (`SetFocus`), e roda o loop `GetMessage`/`Dispatch`. O WNDPROC do console
    desenha a saída do shell na janela via `TextOutA` (branco sobre fundo escuro) e
    o `WM_CHAR` digita/executa. Os comandos `help`/`ver`/`tasklist`/`sc query`/`sc
    start`/`sc stop` reusam as syscalls da FASE 5 — `tasklist` enumera EPROCESS reais
    e `sc start` chama o `DriverEntry` real, com a saída desenhada **na janela** e
    ecoada na serial (`[win] ...`).
  - **kmain** (`src/kernel.c`): decide o estado final por `win32k_was_active()` (o
    desktop permanece na tela para o screendump mesmo após as janelas serem reaped).
  - **build.ps1/run.ps1**: `desktop.exe` compilado depois das DLLs (linkado contra
    `libkernel32`/`libuser32`/`libgdi32`/`libntdll`) e carregado por **último**.
  - *Limitações (não bloqueiam):* uma fila de mensagens global (um processo GUI por
    vez); sem mouse (foco por `SetFocus`/teclado, botões da barra de tarefas são
    visuais); compositing sem clipping entre janelas/sem repaint automático pós-
    recompose (o conteúdo é redesenhado no `WM_PAINT`); console = grade de texto
    simples (~36x22, fonte 8x8, rola 1 linha); resolução 320x200 (mode 13h). Build
    verde, boot verde; nada anterior regrediu.
- **FASE 5 — Shell `cmd.exe` (estilo Windows, ring 3)**:
  - **`examples/cmd.c`** (`cmd.exe`, PE32+ x64, ImageBase 0x3600000): Command
    Prompt em ring 3 com `help`, `tasklist` (lista os EPROCESS), `sc query`
    (lista os drivers de kernel e o estado STOPPED/RUNNING), `sc start <nome>` /
    `sc stop <nome>` (carrega/descarrega um driver `.sys` pelo nome), `dir` (stub,
    sem FS) e `exit`. Começa em **modo demo** (executa `help`/`tasklist`/`sc query`/
    `sc start`/`sc stop`/`dir` automaticamente, testável headless na serial) e depois
    abre um **prompt interativo** (lê linhas via `ReadFile`; com display a digitação
    funciona, headless encerra sozinho).
  - **Syscalls novas** (`ke/syscall.c`), números **37..40**, sincronizadas com
    `dll/ntdll.c`: `NtEnumProcesses` (37, enumera EPROCESS do Object Manager),
    `NtEnumDrivers` (38, enumera o registro de drivers), `NtLoadDriver` (39, `sc
    start`: carrega o `.sys` pelo nome via I/O Manager + loader e o deixa RUNNING),
    `NtUnloadDriver` (40, `sc stop`: chama `DriverUnload` e marca STOPPED).
  - **Object Manager** (`nt/object.c`): lista global de objetos +
    `ob_enum_by_type`/`ob_count_by_type` (enumeram objetos por tipo, inclusive os
    sem nome como EPROCESS e DRIVER_OBJECT).
  - **I/O Manager** (`nt/driver.c`): "registro de drivers" por nome (estado
    STOPPED/RUNNING + DRIVER_OBJECT vivo); `driver_load(name, image)` (boot:
    registra como STOPPED), `driver_load_by_name`/`driver_unload_by_name` (start/
    stop reais), `driver_enum` (para `sc query`). `loader/loader.c`:
    `ldr_get_module_bytes(name)` (bytes brutos de um `.sys` registrado).
  - **Entrada interativa pelo console** (`drivers/keyboard.c`): fila de stdin
    alimentada pela IRQ1 quando NÃO há janelas GUI; `NtReadFile` no "console
    device" drena essa fila (`kbd_stdin_read`), sem interferir no roteamento de
    teclado do win32k.
  - **kernel32** (`dll/kernel32.c`): exports `EnumProcessesEx`/`EnumDriversEx`/
    `StartDriverServiceA`/`StopDriverServiceA` (encaminham para o ntdll). cmd.exe
    compilado depois das DLLs e linkado contra a nossa `libkernel32.a`.
  - *Parcial:* `sc start`/`sc stop` cobrem drivers de kernel (`.sys`) — não há
    `services.exe` nem serviços de usuário; `dir` é stub (sem FS); a digitação
    interativa só funciona com display (headless o shell roda em modo demo e
    encerra). Build verde, boot verde; nada anterior regrediu.
- **FASE 3 — Named Pipes (IPC)**:
  - **Object Manager** (`nt/object.h`): novo tipo `OB_TYPE_PIPE` (o pipe é objeto
    nomeado de verdade, vive no namespace global e persiste entre processos).
  - **Named Pipe (kernel)** (`nt/pipe.c`+`.h`, novos): `PIPE_OBJECT` com **buffer
    circular de 4096 bytes** e estados DISCONNECTED/LISTENING/CONNECTED;
    `pipe_normalize_name` (aceita `\\.\pipe\Nome`, `\Device\NamedPipe\Nome`,
    `\Pipe\Nome` ou só `Nome` → `\Pipe\Nome`); `pipe_create`/`pipe_open`/
    `pipe_connect`/`pipe_write`/`pipe_read`. `FILE_OBJECT` (`nt/io.h`) ganhou
    `PipeObject` — um handle de pipe é um file object com `DeviceObject=0` e
    `PipeObject!=0` (modelo do npfs do Windows).
  - **SSDT** (`ke/syscall.c`), números **31..32**: `NtCreateNamedPipeFile` (31),
    `NtConnectNamedPipe` (32). `NtCreateFile` passou a detectar nomes de pipe e
    abrir pelo lado cliente; `NtWriteFile`/`NtReadFile` roteiam para o buffer do
    pipe quando `FILE_OBJECT.PipeObject != 0`.
  - **kernel32** (`dll/kernel32.c`): `CreateNamedPipeA`/`ConnectNamedPipe` (e o
    `CreateFileA`/`ReadFile`/`WriteFile`/`CloseHandle` já existentes funcionam para
    pipes); **ntdll** exporta os dois `Nt*` novos.
  - Demo sequencial no mesmo boot: `pipeserver.exe` (base 0x1E00000) cria
    `\\.\pipe\MeuOSPipe`, `ConnectNamedPipe` e `WriteFile` 44 bytes; `pipeclient.exe`
    (base 0x3000000) `CreateFileA` + `ReadFile` recebe os 44 bytes idênticos.
  - *Limitações (não bloqueiam):* demo sequencial (sem escalonador,
    `connect`/`open` não bloqueiam); buffer FIFO único de 4096 bytes (não
    full-duplex); `pipe_create` falha se o nome já existir (sem unlink do
    namespace); sem overlapped/assíncrono nem múltiplas instâncias. Build verde,
    boot verde; nada anterior regrediu.
- **FASE 2 — Win32k (janelas + mensagens) + user32/gdi32**:
  - **Window manager (kernel, win32k)** (`src/win32/win32k.c`+`.h`): árvore de
    janelas (`WND` com rect/título/classe/visible/z), **HWND** = id pequeno, foco
    (a janela com foco recebe o teclado), **compositing** em z-order (desktop azul +
    janelas com corpo cinza + barra de título azul/inativa + moldura preta), **fila
    de mensagens** circular (cap 64) com `NtUserGetMessage` que **bloqueia** (`sti;
    hlt`) até haver mensagem, e **objetos GDI** no Object Manager (`HDC`/`HBRUSH`).
    Roteamento de teclado: a IRQ1 (`drivers/keyboard.c`) posta `WM_KEYDOWN`/`WM_CHAR`
    para a janela com foco quando há janelas; injeção sintética
    (`win32k_inject_demo_input`) torna o teste headless determinístico.
  - **SSDT** (`ke/syscall.c`), números **17..30**, sincronizados com `dll/ntdll.c`:
    `NtUserRegisterClass`(17), `NtUserCreateWindowEx`(18), `NtUserDestroyWindow`(19),
    `NtUserShowWindow`(20), `NtUserGetMessage`(21), `NtUserDispatchMessage`(22),
    `NtUserPostMessage`(23), `NtUserPostQuitMessage`(24), `NtUserGetDC`(25),
    `NtUserInvalidate`(26), `NtGdiGetStockObject`(27), `NtGdiCreateSolidBrush`(28),
    `NtGdiTextOut`(29), `NtGdiFillRect`(30).
  - **DLLs** (ring 3): **`user32.dll`** ganha a API de janelas (`RegisterClassA`,
    `CreateWindowExA`, `ShowWindow`, `GetMessageA`, `TranslateMessage`,
    `DispatchMessageA` — que **chama o WNDPROC em ring 3**, igual ao Windows —,
    `DefWindowProcA`, `PostQuitMessage`, `InvalidateRect`, `GetDC`/`BeginPaint`/
    `EndPaint`, `FillRect`); **`gdi32.dll`** (novo, ImageBase 0x1A00000):
    `TextOutA`, `GetStockObject`, `CreateSolidBrush`, `DeleteObject`; **ntdll**
    exporta as 14 syscalls win32k.
  - Demo `examples/guiapp.c` (`guiapp.exe`, ImageBase 0x1C00000): `RegisterClassA` +
    `CreateWindowExA` + `ShowWindow` + loop `GetMessage`/`Dispatch`; o WNDPROC no
    `WM_PAINT` faz `CreateSolidBrush` + `FillRect` (retângulo vermelho) + 3
    `TextOutA` — a janela **aparece no framebuffer**.
  - **kmain**: `win32k_init()` após `ps_init()`; se a GUI deixou janela na tela, não
    roda a `fb_demo()` da Fase 1 (deixa o que o `guiapp` pintou para o screendump).
  - *Limitações (não bloqueiam):* uma fila de mensagens global (uma thread GUI por
    vez); sem mouse (caminho de `WM_*BUTTON*` definido, falta driver PS/2 IRQ12);
    compositing sem clipping entre janelas; resolução 320x200 (mode 13h). Build
    verde, boot verde; nada anterior regrediu.
- **FASE 1 — Framebuffer gráfico (VGA mode 13h) + GDI de baixo nível no kernel**:
  - **Driver de vídeo** (`src/drivers/video.c`+`.h`): `fb_init()` programa os
    registradores VGA **direto** (Misc Output 0x3C2, Sequencer 0x3C4/5, CRTC 0x3D4/5
    destravando o bit de proteção, Graphics Controller 0x3CE/F, Attribute Controller
    0x3C0, paleta no DAC 0x3C8/9) para **mode 13h (320x200x256)**; framebuffer linear
    em **0xA0000** (já mapeado na identidade de 1 GiB — não foi preciso mexer nas page
    tables). **GDI de baixo nível**: `fb_clear`, `fb_pixel`, `fb_get_pixel`,
    `fb_fill_rect` (com clipping), `fb_rect`, `fb_hline`/`fb_vline`, `fb_draw_char`/
    `fb_draw_text` (bg transparente, trata `\n` e wrap) — no-op seguro se
    `!fb_active()`.
  - **Fonte bitmap 8x8** (`src/drivers/font8x8.c`): `g_font8x8[96][8]` cobrindo o
    ASCII imprimível (0x20..0x7F).
  - **Demo no boot** (`src/kernel.c`, `fb_demo()`): roda **depois** dos `[ok]` e dos
    testes de binário; desenha desktop azul + janela estilo NT + título + 3 linhas +
    16 swatches da paleta, com `fb_get_pixel` conferindo a cor escrita. Cada operação
    é **logada na serial** (`[fb] ...`).
  - **Captura via QMP** (`run.ps1`): novo switch **`-Screendump`** (+`-QmpPort`,
    default 4444) que adiciona `-qmp tcp:...`, faz o handshake e executa `screendump`
    para `build\screen.ppm`. O comportamento default (teste headless) fica inalterado.
  - *Limitações (não bloqueiam):* ficou no **mode 13h** (caminho seguro); o stretch
    VBE/LFB (resolução maior com LFB linear) exigiria mapear a faixa física do LFB.
    Ao entrar no mode 13h o VGA texto (0xB8000) deixa de exibir; a serial segue como
    canal de log. Build verde, boot verde; nada anterior regrediu.
- **FASE 4 — SCM stubs (advapi32.dll) + syscalls de informação (NtQuery*)**:
  - **SSDT** (`ke/syscall.c`), números **33..36**: `NtQuerySystemInformation` (33,
    classes `SystemBasicInformation` → nº de processadores=1, page size, páginas
    físicas via `pmm_total_frames`; e classe própria de versão → "MeuOS 0.1 ..."),
    `NtQueryInformationProcess` (34, `ProcessBasicInformation` → PID + ImageBase do
    EPROCESS), `NtReadVirtualMemory` (35) / `NtWriteVirtualMemory` (36) simples na
    faixa identidade-mapeada. `mm/pmm.c`+`pmm.h` ganham `pmm_total_frames()`.
  - **advapi32.dll** (`dll/advapi32.c`, PE real com export table): stubs do
    **Service Control Manager** que NÃO falham (`StartServiceCtrlDispatcherA`,
    `RegisterServiceCtrlHandlerA`, `SetServiceStatus`, `OpenSCManagerA`,
    `CreateServiceA`/`OpenServiceA`/`StartServiceA`, `CloseServiceHandle`) +
    **registro** apoiado na Native API (`RegOpenKeyExA`→`NtOpenKey`,
    `RegQueryValueExA`→`NtQueryValueKey`, `RegCloseKey`). Vive em ring 3; só o
    ntdll faz syscall. ImageBase 0x3200000.
  - **ntdll** (`dll/ntdll.c`): exports `NtQuerySystemInformation`/
    `NtQueryInformationProcess`/`NtReadVirtualMemory`/`NtWriteVirtualMemory`.
  - Demo `examples/sysinfo.c` (`sysinfo.exe`, base 0x3400000): imprime a versão do
    SO, nº de processadores, PID/ImageBase do processo e os valores de registro
    (ProductName/CurrentVersion), e exercita o SCM (OpenSCManager/CreateService).
    Importa kernel32 + ntdll + advapi32; wired em `build.ps1`/`run.ps1`.
  - *Parcial:* SCM sem `services.exe` real (stubs não falham; ServiceMain chamada
    inline); `NtQuerySystemInformation`/`NtQueryInformationProcess` cobrem 1 classe
    cada; registro segue stub (sem hive); `NtRead/WriteVirtualMemory` assumem o
    espaço identidade compartilhado (não trocam CR3 para o `hProcess` alvo).
- **Process Manager (estilo "Ps" do NT) + espaço de endereçamento por processo**:
  - **EPROCESS / ETHREAD como objetos do Object Manager** (`nt/process.c`,
    `nt/process.h`): tipos `OB_TYPE_PROCESS`/`OB_TYPE_THREAD` (cabeçalho+corpo,
    refcount). `EPROCESS` guarda `ProcessId`, `DirectoryTableBase` (CR3),
    `ImageBase`, `ExitStatus`, `ImageName`; `ETHREAD` guarda `ThreadId`,
    `StartAddress` (entry), pilha. API `PsCreateProcess`/`PsCreateThread`/
    `PsTerminateProcess`/`PsGetCurrentProcess`/`PsGetCurrentThread`/`ps_init`
    (PID/TID sequenciais; `ps_init()` em `kmain`).
  - **PML4 por processo + troca de CR3** (`mm/paging.c`, `mm/paging.h`):
    `mm_create_address_space()` aloca PML4/PDPT/PD no PMM e **copia o mapa do
    kernel** (identidade de 1 GiB com bits U/S); `mm_get_cr3`/`mm_switch_cr3`.
    `usermode_enter` (`ke/usermode.c`) cria o espaço do processo, **troca o CR3
    ao entrar em ring 3 e restaura o do kernel ao sair** (via longjmp). Fallback
    seguro para o espaço compartilhado se faltar RAM.
  - **SSDT** (`ke/syscall.c`): `NtCreateProcess` (7), `NtCreateThread` (8),
    `NtTerminateProcess` (9), `NtWaitForSingleObject` (10, retorna na hora — sem
    escalonador ainda). Stubs no `ntdll` (`NtCreateProcess`/`NtCreateThread`/
    `NtTerminateProcessEx`/`NtWaitForSingleObject`) e `CreateProcessA`/
    `WaitForSingleObject` no `kernel32` (preenchem `PROCESS_INFORMATION`).
  - **Loader cria EPROCESS por `.exe`** (`loader/loader.c`): `ldr_run` cria o
    EPROCESS + ETHREAD principal do `.exe`, marca como processo corrente, roda em
    ring 3 e faz `PsTerminateProcess` ao voltar.
  - `examples/hello.c` (test.exe) demonstra a partir do ring 3: `CreateProcessA`
    + `WaitForSingleObject` antes do `MessageBox` (ring3 → kernel32 → ntdll →
    `int 0x80` → Process Manager).
  - *Parcial:* a PML4 por processo copia o mapa de identidade do kernel (mesmos
    PDPT/PD com U/S) — o isolamento ainda não é total (páginas baixas
    compartilhadas); `NtWaitForSingleObject` não bloqueia; `NtCreateProcess` cria
    o objeto mas não carrega/executa a imagem (o loader é quem roda os `.exe`).
- **Separação modo kernel / modo usuário (ring 0 / ring 3)** — camada `ke/`:
  - GDT com segmentos de ring 0 e ring 3 + **TSS** (`ke/gdt.c`).
  - Entrada em ring 3 via `iretq` e código de modo usuário (`ke/usermode.c`).
  - **Syscalls** por `int 0x80` (mesmo mecanismo do `int 2Eh` do NT), com gate
    DPL 3 na IDT e despacho em `ke/syscall.c` (`sys_write`, `sys_exit`).
  - `mm/paging.c`: marca páginas como acessíveis ao usuário (bit U/S).
- **O `.exe` do Windows roda em RING 3 importando de DLLs REAIS** (sem stubs):
  - `ntdll.dll` / `kernel32.dll` / `user32.dll` reimplementadas como **PE de
    verdade com export table** (`dll/`, abordagem do ReactOS). Só o `ntdll` faz
    syscall (`int 0x80`); kernel32/user32 encaminham pra ele.
  - **Loader estilo `LdrLoadDll`** (`loader/loader.c`): registra os módulos do
    boot por nome e, para cada import, **carrega a DLL e resolve pela export
    table, recursivamente** (`.exe` → `user32` → `ntdll`).
  - `loader/pe.c` virou primitivas (`pe_map`, `pe_bind_imports`, `pe_get_export`).
  - `win32/win32.c` é o **win32k** (serviços do lado kernel). O driver `.sys`
    segue em ring 0. Removido o antigo `user/win32u.c` (stubs hardcoded).
- Carregamento de binários Windows como **módulos de boot** (`-initrd`), com
  detecção automática `.exe` vs `.sys` pelo Subsystem do PE.
- **Object Manager + I/O Manager com IRPs (estilo NT)**:
  - `nt/object.c`: objetos (header+corpo, refcount), **namespace** (`\Device\`),
    **handle table**, tipos (Device/Driver/File/Process/Thread/Event).
  - `nt/io.c`: `DEVICE_OBJECT`/`IRP`/`IO_STACK_LOCATION`, `IoCreateDevice` (cria
    objeto nomeado), `IoCallDriver`, IRPs de CREATE/CLOSE/IOCTL (METHOD_BUFFERED).
  - **SSDT** em `ke/syscall.c` + `NtCreateFile`, `NtDeviceIoControlFile`, `NtClose`;
    stubs no `ntdll` e `CreateFileA`/`DeviceIoControl`/`CloseHandle` no `kernel32`.
  - Driver model: `DRIVER_OBJECT.MajorFunction` (dispatch `IRP_MJ_CREATE`/`CLOSE`/
    `DEVICE_CONTROL`); cada driver com seu próprio `DRIVER_OBJECT`.
  - **Teste 2 OK**: `examples/ioctldriver.c` + `examples/ioctlapp.c` —
    `DeviceIoControl` devolve `0xCAFEBABE` do driver ao app em ring 3.

- **Console I/O + mais syscalls/DLLs do sistema** (rumo ao passo 8 do roadmap):
  - **"Console device"**: `GetStdHandle(STD_OUTPUT/ERROR/INPUT_HANDLE)` devolve
    pseudo-handles (valores sentinela -11/-12/-10, como no NT); `NtWriteFile` para
    um destes escreve na saída do kernel (VGA+serial), `NtReadFile` lê 0 bytes
    (sem stdin bufferizado ainda).
  - **SSDT** (`ke/syscall.c`): `NtWriteFile` (11), `NtReadFile` (12),
    `NtOpenKey` (13) + `NtQueryValueKey` (14) como **stubs de registro**
    (`ProductName`/`CurrentVersion`), `GetModuleHandle` (15) + `GetProcAddress`
    (16) **apoiados no loader real** (`ldr_load` + `pe_get_export`).
  - **I/O Manager** (`nt/io.c`): `io_build_write`/`io_build_read` (IRP_MJ_WRITE/
    READ, METHOD_BUFFERED) — `NtWriteFile`/`NtReadFile` encaminham ao driver
    quando o handle é de arquivo (não-console).
  - **DLLs**: `ntdll` ganha `NtWriteFile`/`NtReadFile`/`NtOpenKey`/
    `NtQueryValueKey`/`LdrGetModuleHandle`/`LdrGetProcAddress` (+ helpers `sc4`/`sc5`);
    `kernel32` ganha `GetStdHandle`, `WriteFile`, `ReadFile`, `GetModuleHandleA`,
    `GetProcAddress`. `user32` mantém `MessageBoxA`.
  - Exe de teste (`examples/conhello.c` -> `conhello.exe`, base 0x1800000):
    `GetStdHandle` + `WriteFile` imprimem no console e `GetModuleHandleA` +
    `GetProcAddress` resolvem `MessageBoxA` em user32, chamando-o pelo ponteiro.

- **Suporte a PE32 (32-bit) no loader + execução em compatibility mode**:
  - `loader/pe.c` virou bitness-aware: `pe_parse` detecta `IMAGE_NT_HEADERS32`
    (magic 0x10B, machine 0x14C) e `IMAGE_NT_HEADERS64` (0x20B/0x8664), lendo os
    campos nos offsets certos de cada formato. `pe_map_at` mapeia numa base
    arbitrária; `pe_bind_imports` trata thunks de 4 bytes (PE32) e 8 bytes (PE32+).
  - **Relocações** (`pe_relocate`): aplica base relocations (.reloc) quando a base
    efetiva difere da preferida — `HIGHLOW` (32-bit) e `DIR64` (64-bit).
  - **Execução de 32 bits em ring 3**: novo segmento de código 32-bit (compat
    mode, L=0/D=1) na GDT (`SEL_UCODE32`, `ke/gdt.c`); `usermode_enter32` entra em
    ring 3 de 32 bits via `iretq`; `syscall_dispatch` reconhece o chamador de 32
    bits (pelo CS salvo) e trunca os argumentos para 32 bits (`int 0x80` com
    EAX=número; args em EDI/ESI/...).
  - Exe de teste PE32 (`examples/hello32.c` -> `test32.exe`, alvo
    `x86-windows-gnu`, base 0x1600000) roda em ring 3 de 32 bits e chama
    `sys_write`/`sys_messagebox`/`sys_exit`; o loader carrega numa base deslocada
    e aplica as relocações.

### Planejado
- DLLs do sistema em versão 32-bit (ntdll/kernel32/user32 PE32) + thunk de
  syscall de 32 bits, para um PE32 com imports rodar como o de 64 (rumo ao
  pinball.exe real).
- Mais objetos/serviços: eventos (`NtCreateEvent`/`NtSetEvent`), registro com
  hive real (hoje só `ProductName`/`CurrentVersion` como stub).
- **Escalonador / preempção** (hoje os `.exe` rodam em sequência;
  `NtWaitForSingleObject` não bloqueia, fila de mensagens do win32k é global,
  demo de pipe é sequencial).
- **Driver de mouse PS/2 (IRQ12)** + hit-testing dos botões da barra de tarefas /
  troca de foco por clique (hoje só por `SetFocus`/teclado).
- **Resolução maior** (VBE/LFB linear acima do mode 13h 320x200) — exige mapear a
  faixa física do LFB nas page tables.
- **NTFS: alocação de clusters (`$Bitmap`) + journaling (`$LogFile`)** — hoje a
  escrita só usa espaço já alocado; criar arquivo grande / converter `$DATA`
  residente→não-residente exige o caminho de alocação. `services.exe` real (o SCM
  ainda são stubs em `advapi32`).
- DLLs do sistema em 32-bit + thunk de syscall de 32 bits, para um PE32 que
  importe Win32 rodar como o de 64 (rumo ao pinball.exe real).

> **Feito nesta rodada (antes "planejado"):** modo gráfico/framebuffer (FASE 1),
> `CreateWindowEx`/`TextOutA`/`gdi32` + window manager (FASE 2), IPC por Named
> Pipes (FASE 3), `NtQuery*` + `advapi32`/SCM (FASE 4), shell `cmd.exe` com stdin
> bufferizado no console (FASE 5), desktop + barra de tarefas + cmd numa janela
> (FASE 6) e a **RODADA HAL/NTFS**: HAL (PCI + disco IDE ATA PIO), **driver NTFS
> de leitura + escrita** (subconjunto seguro) ligado ao I/O Manager e o volume
> montado como **C:** (`dir`/`cd`/`type`/`copy`/`vol` no `cmd`). O **sistema de
> arquivos** e o **driver de disco** deixaram de ser stub.

## [0.1.0] - 2026-06-22
Primeira versão pública. Kernel 64-bit, do zero, que carrega e roda binários
do Windows (PE32+) com arquitetura no estilo NT.

### Adicionado
- **Boot**: Multiboot em 32 bits → long mode 64 bits (AOUT kludge + binário plano).
- **CPU/memória baixa**: paginação PAE, GDT de 64 bits, SSE.
- **Interrupções**: IDT (256 vetores) + exceções; PIC 8259 remapeado; PIT (timer
  IRQ0); teclado PS/2 por IRQ1.
- **Memória**: detecção de RAM (Multiboot), PMM (frames de 4 KiB), heap
  (`kmalloc`/`kfree` com split + coalescing).
- **Carregador PE32+**: parse de DOS/PE/seções e resolução de imports por nome.
- **Subsistema Win32**: `MessageBoxA`, `ExitProcess` — roda um `.exe` do Windows.
- **Executiva NT** (`ntoskrnl`): `DbgPrint`, `IoCreateDevice`, `RtlInitUnicodeString`.
- **I/O Manager**: carrega drivers de kernel `.sys` (`DriverEntry`, `DRIVER_OBJECT`
  com layout x64 idêntico ao do NT, `DriverUnload`).
- **Carregamento por módulos de boot**: o kernel roda **qualquer** PE passado via
  `-initrd`, detectando `.exe` vs `.sys` pelo Subsystem. Nada embutido no kernel.
- **Toolchain portátil no Windows**: Zig (`zig cc`) + NASM + QEMU, baixados pelo
  `setup.ps1` (sem precisar de SDK).

### Corrigido
- `cpuid` destruía `EBX` (ponteiro do Multiboot) — `EBX` passou a ser salvo no
  primeiro instante do boot, antes de qualquer `cpuid`.

[Não lançado]: https://github.com/JoaoOliveiraJ/Meu-OS/compare/v0.9.0...HEAD
[0.9.0]: https://github.com/JoaoOliveiraJ/Meu-OS/releases/tag/v0.9.0
[0.8.0]: https://github.com/JoaoOliveiraJ/Meu-OS/releases/tag/v0.8.0
[0.7.0]: https://github.com/JoaoOliveiraJ/Meu-OS/releases/tag/v0.7.0
[0.6.0]: https://github.com/JoaoOliveiraJ/Meu-OS/releases/tag/v0.6.0
[0.5.0]: https://github.com/JoaoOliveiraJ/Meu-OS/releases/tag/v0.5.0
[0.4.0]: https://github.com/JoaoOliveiraJ/Meu-OS/releases/tag/v0.4.0
[0.3.0]: https://github.com/JoaoOliveiraJ/Meu-OS/releases/tag/v0.3.0
[0.2.0]: https://github.com/JoaoOliveiraJ/Meu-OS/releases/tag/v0.2.0
[0.1.0]: https://github.com/JoaoOliveiraJ/Meu-OS/releases/tag/v0.1.0
