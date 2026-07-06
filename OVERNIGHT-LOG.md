# OVERNIGHT-LOG — Fundação Ke/HAL (branch `feat/kernel-foundation-irql-dpc`)

Trabalho autônomo enquanto o usuário dorme. Cada incremento: edita → `build.ps1` → `run.ps1` (pintok) → verifica **pintok ≥ baseline** + `proof_pillar4` → **commit + push**. Parada segura em qualquer falha (reverte, loga, para).

Referência do plano: `C:\Users\joao\.claude\plans\lovely-baking-whale.md`.

## Setup (concluído)
- Checkpoint do trabalho do usuário: commit `2e652e9` em `feat/unblock-desktop-mouse-irq12`, **pushed** para `origin`.
- Branch isolada criada: `feat/kernel-foundation-irql-dpc`.
- Build confirmado: `./build.ps1` → exit 0, `kernel.bin` OK (289628 bytes).
- Baseline dourado do pintok capturado: `./run.ps1 -Modules build/pintok.sys -Headless -TimeoutSec 40` (log completo em `$JOB/tmp/pintok_baseline.log`).

## Baseline dourado do pintok — assinatura estável (NÃO pode regredir)
```
[P1] ==== PROVA PASSOU ====
[P2] ==== PROVA PASSOU ====
[P3] ==== PROVA PASSOU ====   (P4 PAUSADA)
[boot] driver de kernel: pintok.sys
[io] chamando DriverEntry...
  [pintok.syslog] '1.17.18-11+20260212.201837 ; ...'
  [pintok.syslog] '10 ; 0 ; 26100.'            (Windows build 26100)
  [intercept] CPUID leaf=0/1/7 -> Intel i7-9700K   (x3)
[io] intercept totals: CPUID x3  RDTSC x~33  RDMSR x0  ANTIVM x0
[io] DriverEntry retornou status=0x00000000C0000365
```
- **Gates estáveis**: P1/P2/P3 PASSOU; "chamando DriverEntry"; CPUID x3; a sequência syslog; retorno C0000365.
- **RDTSC x33 varia** com timing (single-step) — não é gate rígido.
- **C0000365 é o teto atual do pintok** (ele falha internamente aí). Regressão = chegar MENOS fundo, sumir CPUID x3 / syslog, aparecer `Sistema parado` ou nova exceção não-recuperada. Ir além de C0000365 não é regressão (não é o objetivo, mas é aceitável).

## Incrementos
| # | Item | build | pintok≥baseline | proof P4 | probe | commit |
|---|------|-------|-----------------|----------|-------|--------|
| — | setup/baseline | ✅ | — (baseline) | pausada | — | 2e652e9 |
| 4 | Stall/PerfCounter (TSC) | ✅ | ✅ idêntico (CPUID x3, C0000365) | — | TSC=3.09GHz OK | 5557e5b |
| 1 | IRQL real (gs:[0x60]+CR8) | ✅ | ✅ idêntico | — | — (probe depois) | b11fa80 |
| 3 | Spinlocks reais + gating preempção | ✅ | ✅ idêntico (sem halt) | preempção viva (66 beats) | — | (este) |

Ordem noturna: **4 → 1 → 3 → [2] → parada segura**. Deferidos p/ supervisão (tocam contexto de troca ou trajetória do pintok): **0a+5** (reentrância do swap + waits — são uma unidade, testar juntas com você acordado), **6** (KTIMER), **7** (Ex), **trilha I/O** inteira. Motivo: mexer no caminho de context-switch (`ki_swap_context`) ou tornar `KeWait` bloqueante sem supervisão é o maior risco à trajetória do pintok.

| 2 | DPC (fila per-CPU + drena no KeLowerIrql) | ✅ | ✅ idêntico (CPUID x3, C0000365) | preempção viva (51 beats) | DPC disparou inline OK | 7acfb37 |
| 0a | Reentrância do swap (irq_save/restore) | ✅ | ✅ idêntico | preempção viva (60 beats) | — | 4aec7e6 |
| 5 | Waits bloqueantes reais (gated) + fix terminate-freeze | ✅ | ✅ idêntico (CPUID x3, C0000365) | preempção viva (48 beats) | **block+wake ACORDOU OK** | 9aacf68 |
| flag | `g_ke_legacy_mode` + retrofit waits | ✅ | ✅ (auto-legacy p/ pintok) | — | wait-test real OK | dfd7026 |
| 6 | KTIMER real (flag-gated) | ✅ | ✅ idêntico (CPUID x3, C0000365) | 45 beats | **timer EXPIROU→OK** | 112ba70 |
| 7 | Primitivos Ex (fast mutex/ERESOURCE/lookaside/interlocked, flag-gated) | ✅ | ✅ idêntico (CPUID x3, C0000365) | 54 beats | **fast mutex + lookaside OK** | (este) |

### ✅ FUNDAÇÃO Ke/HAL COMPLETA
Todos os 4 self-tests passam num boot: `[dpc] PROVA OK`, `[ex-test] OK`, `[wait-test] ACORDOU`, `[timer-test] EXPIROU→OK`. Pintok intacto o tempo todo (auto-legacy). A flag `g_ke_legacy_mode=1` reverte tudo pro antigo.

## TRILHA DE DRIVERS I/O (em progresso)
Achado: `IO_STACK_LOCATION` já está NT-correto; correções de layout só melhoram o pintok (macros inline dele baked contra offsets MS). pintok **não cria device** no baseline → Fase 1 neutra.
| fase | o que | build | pintok | fundação | commit |
|------|-------|-------|--------|----------|--------|
| 1a | DEVICE_OBJECT → offsets NT (`DeviceExtension@0x40`, `DeviceType@0x48`, `sizeof 0xB8`) + `_Static_assert` + fix bug Flags/type | ✅ | ✅ idêntico | 4/4 | 3f5afe2 |
| 3 | Modelo de interrupção: `KINTERRUPT`/`IoConnectInterrupt`/`IoDisconnectInterrupt`/`KeSynchronizeExecution`/`HalGetInterruptVector` + `ioapic_set_irq_ex` (level/active-low/mask) + early-out no ISR (antes da cadeia legada) | ✅ | ✅ idêntico | 3/3 | 418707f |
| 2 | Device stacks: `IoAttachDeviceToDeviceStack(Safe)`/`IoDetachDevice`/`IoGetAttachedDevice(Reference)`/`IoGetLowerDeviceObject` (flag-gated; side-table p/ lower device) | ✅ | ✅ idêntico | 4/4 + devstack | afcc4e3 |
| 5 | HAL DMA: `HalGetDmaAdapter` + vtable `DMA_OPERATIONS` (AllocateCommonBuffer via `pmm_alloc_contiguous`, phys==virt); `HalAllocate/FreeCommonBuffer` flag-gated | ✅ | ✅ idêntico | 6/6 self-tests | 3983df7 |
| 1b | **IRP → layout NT x64** (`sizeof 0xC8`, `Tail.CurrentStackLocation@0xB8`, array traseiro de `IO_STACK_LOCATION`; `SystemBuffer`→`AssociatedIrp.SystemBuffer`) + macros inline `IoGetCurrent/Next/Skip/Copy` + `IoCallDriver` avança a stack. **72 sites em 8 arquivos** reescritos. `_Static_assert` de offsets | ✅ | ✅ idêntico | 7/7 self-tests | ad25c23 |
| 6 | Round-trip completo de IOCTL por um driver (mini-driver kernel: `MajorFunction[IRP_MJ_DEVICE_CONTROL]` → `IoCallDriver` → dispatch lê current → escreve SystemBuffer → `IoCompleteRequest`) | ✅ | ✅ idêntico | 8/8 self-tests | 2d94a56 |
| 4 | PnP mínimo: `pnp_start_function_driver` lê `DriverExtension.AddDevice`, cria PDO, chama AddDevice (driver cria FDO + anexa), envia `IRP_MJ_PNP`/`IRP_MN_START_DEVICE`. `DRIVER_EXTENSION` em ntddk.h (AddDevice@0x08). Legado (AddDevice=NULL) = no-op | ✅ | ✅ idêntico | 9/9 self-tests | (este) |

### 🎉 DRIVER WDM REAL RODANDO (`wdmdemo.sys`)
Escrito um driver WDM real (`apps/wdmdemo.c` → `wdmdemo.sys`, PE 7168 bytes, compilado pelo toolchain, **não** hardcoded). Carregado sozinho (`run.ps1 -Modules build\wdmdemo.sys`): `DriverEntry` roda, cria device+symlink, dispara system thread; o worker (modo real pós-DriverEntry) **bloqueia num `KeWaitForSingleObject` num KTIMER de 1s e é acordado de verdade**, usa fast mutex, enfileira DPC. Sem halt. É o objetivo original: um driver Windows real exercitando a fundação real. (DMA/KeStall ficaram fora do driver por não estarem na import lib do MinGW — já provados pelos kernel self-tests; import lib completa da ntoskrnl = follow-up.)

### ✅✅ TRILHA DE DRIVERS I/O COMPLETA (Fases 1a,1b,2,3,4,5,6)
**9/9 self-tests num boot**: int-test, devstack-test, dma-test, irp-test, drv-irp-test, pnp-test, ex-test, wait-test, timer-test. Pintok intacto o tempo todo. Um driver WDM genérico agora tem tudo: criar device (layout NT), **processar IRP** (layout NT), device stacks, `IoConnectInterrupt` (ISR em DIRQL), DMA, PnP (AddDevice+START_DEVICE), + fundação Ke completa. Wire de `pnp_start_function_driver`/START_DEVICE no caminho de load (driver.c) = passo futuro quando houver function driver real (driver.c fica intocado — pintok-safe).

Fluxo de IRP provado ponta-a-ponta (`[irp-test]`: build→advance→read, campos corretos). Drivers in-tree (ntfs/ioctldriver/calller) rodam `DriverEntry` completo. **Falta só**: Fase 4 (PnP: AddDevice + `IRP_MJ_PNP` START_DEVICE) e Fase 6 (drivers de teste WDM que processam IRP) — ambas agora desbloqueadas.

**Todas as peças I/O que NÃO precisam do IRP reescrito estão feitas.** Um driver WDM já pode: criar device (layout NT), anexar device stack, conectar IRQ (ISR em DIRQL), usar DMA common buffer, + toda a fundação Ke. **Falta só**: Fase 1b (reescrita IRP, 72 sites) → desbloqueia Fase 4 (PnP) e Fase 6 (drivers de teste que processam IRP).

`int-test` prova o chain `IoConnectInterrupt`→`int`→`isr_handler`→dispatch→ISR do driver **em DIRQL**. Nomes fora da lista do pintok → efeito zero. kbd/mouse/timer legado intocados. **Pendente**: Fase 1b (reescrita IRP, 72 sites), Fase 2 (device stacks), Fase 4 (PnP), Fase 5 (DMA), Fase 6 (drivers de teste WDM).

**Sessão retomada ("Continua").** pintok **não** usa `KeWaitForSingleObject`/`KeInitializeEvent` (baseline) → Item 5 gated por contexto (só threads worker reais bloqueiam; contexto boot/idle, onde o pintok roda, mantém auto-resolve). O auto-teste provou block+wake ponta-a-ponta, e de bônus corrigiu um **freeze latente de término de thread** (`cli;hlt` → `yield`).

**Flag `g_ke_legacy_mode`** (pedido do usuário): `0`=correto/real (default), `1`=volta pro antigo. `ke_legacy_active()` (em `ntoskrnl.c`) também liga o modo antigo **automaticamente** enquanto o pintok roda (`g_pintok_trace=1` no `DriverEntry`), então a trajetória do pintok é **garantidamente** preservada nos itens sensíveis (waits/timers/Ex) — sem setar nada. Waits (Item 5) já respeitam a flag. Isso desbloqueia Items 6/7 reais. Próximos: Item 6 (KTIMER) e Item 7 (Ex), ambos flag-gated.

---

## RESUMO PARA ACORDAR (parada segura)

**Seu trabalho está salvo:** checkpoint `2e652e9` na sua branch `feat/unblock-desktop-mouse-irq12`, **pushed** para o GitHub.

**Fundação construída (branch isolada `feat/kernel-foundation-irql-dpc`, tudo pushed):**
| commit | item | o que ficou real |
|--------|------|------------------|
| `5557e5b` | Item 4 | `KeStallExecutionProcessor` (busy-spin por TSC) + `KeQueryPerformanceCounter` (retorna contador real). TSC calibrado ~3.1 GHz. |
| `b11fa80` | Item 1 | IRQL real: `KeGetCurrentIrql` lê `gs:[0x60]`; raise/lower mantêm `gs:[0x60]`+CR8. Níveis 0–15. |
| `1ef56bc` | Item 3 | Spinlocks reais (atômico + raise IRQL) + gating de preempção no ISR do timer ("raise a DISPATCH bloqueia preempção"). |

**Cada incremento foi verificado:** build OK + **pintok baseline byte-idêntico** (P1/P2/P3 PROVA PASSOU, `chamando DriverEntry`, CPUID x3, retorno C0000365) + preempção viva (workers batendo) + sem `Sistema parado`. **O pintok está intocado.**

**Deferido para quando você estiver acordado (motivo: tocam o caminho de context-switch OU a trajetória do pintok — não faço sem supervisão):**
- **Item 2 (DPC)** — o pintok chama `KeInitializeDpc`/`KeGenericCallDpc`/`KeSetTargetProcessorDpc`/`KeSignalCallDpc*`; DPC real tem superfície de interação com ele. Design pronto no plano.
- **Item 0a + 5 (reentrância do swap + `KeWait` bloqueante)** — mexem em `ki_swap_context` e no "auto-resolve" que o pintok usa. São uma unidade.
- **Item 6 (KTIMER)**, **Item 7 (primitivos Ex: FAST_MUTEX/ERESOURCE/lookaside)** — dependem do Item 5.
- **Trilha I/O inteira** (IRP layout NT, device stacks, `IoConnectInterrupt`, PnP, DMA) + os drivers de teste (`tests/drivers/`).

**Como continuar:** branch isolada, cada item é um commit limpo (revertível), design completo em `C:\Users\joao\.claude\plans\lovely-baking-whale.md`. Nada está quebrado; pior caso, revisar/reverter por commit. Baseline dourado do pintok em `$JOB/tmp/pintok_baseline.log`.

---

## MARCO: driver REAL da Microsoft PROCESSANDO I/O (não só carregando)

Fundação + trilha I/O concluídas (IRP/DEVICE_OBJECT layout NT, device stacks, interrupção, DMA, PnP — 9 self-tests de boot passam). Depois disso, o teste que importa: pegar um **driver Windows REAL** e provar que ele **processa IRP**.

**`null.sys` (Microsoft, 7680 bytes, `\Device\Null`):**
1. Carregado: relocado de ImageBase `0x1C0000000` → `0x4319000` (.reloc aplicado, 6 relocações).
2. `DriverEntry` → criou `\Device\Null` (`RtlInitUnicodeString '\Device\Null'`) → retornou `STATUS_SUCCESS`.
3. **Exercício de I/O real** (`KiExerciseDriverIO`, em `io.c`, chamado de `driver.c` entre o DriverEntry OK e o Unload, com o device ainda vivo e o kernel em modo real `g_pintok_trace=0`):
   - **WRITE 8 bytes → `STATUS_SUCCESS`, Information=8** — consumiu/descartou os 8 bytes (semântica exata do `\Device\Null`). ✓
   - **READ 8 bytes → `STATUS_END_OF_FILE` (0xC0000011), Information=0** — leitura do null = EOF. ✓

   Isto é o comportamento **byte-a-byte correto** do `\Device\Null` do Windows real. O driver rodou o dispatch de WRITE e READ, setou `IoStatus.Status/Information` e completou os IRPs.

**Como funciona o teste (genérico, vale p/ qualquer driver):** `KiExerciseDriverIO(drv)` pega `drv->DeviceObject` (cabeça da lista de devices do driver, igual ao NT), e só manda WRITE/READ se o driver implementa esses MajorFunction (senão evita dispatch nulo). Ligado em `driver.c` **dentro do ramo `st==STATUS_SUCCESS`**.

**Segurança do pintok:** o pintok retorna `C0000365` (≠ SUCCESS), então **nunca entra** nesse ramo → intocado. Regressão re-verificada: `[P1]/[P2]/[P3] ==== PROVA PASSOU ====`, `chamando DriverEntry`, `DriverEntry retornou status=0xC0000365`, **sem `Sistema parado`**. Idêntico ao baseline dourado.

Logs: `$JOB/tmp/serial_null.log` (null.sys) e `$JOB/tmp/serial_pintok.log` (regressão).

---

## MARCO 2: IofCompleteRequest REAL (conclusão de IRP genuína) — Frente 1 completa

Sessão seguinte (branch `feat/kernel-foundation-irql-dpc`, 7 commits, todos pushed). Alvo:
robustez p/ drivers reais mais pesados. Descoberta-chave: o `null.sys` só funcionou porque
seta `IoStatus` na mão; a rotina que drivers WDK reais usam p/ concluir I/O
(**`IofCompleteRequest`**, p/ onde o macro `IoCompleteRequest()` do `wdm.h` expande) era um
**no-op** (`generic_zero_stub`). Reforma, em incrementos pequenos com regressão do pintok a cada:

- **INC 1** (`ntddk.h`): constantes `SL_INVOKE_ON_SUCCESS/ERROR/CANCEL`, `SL_PENDING_RETURNED`,
  `STATUS_MORE_PROCESSING_REQUIRED`; `PIO_COMPLETION_ROUTINE` passa de `void` p/ `NTSTATUS`.
- **INC 2** (`ntddk.h`): `IO_STACK_LOCATION` de 0x40 → **0x48 real** (união `Parameters` ganha
  `Others{Argument1..4}`), com `_Static_assert` de offsets (`CompletionRoutine@0x38`). Agora um
  driver de filtro WDK real interopera com o nosso completador.
- **INC 3** (`io.c`): `IoCompleteRequest_k` reescrito como **walk real** (igual ao
  `IopfCompleteRequest` do NT): percorre a pilha de baixo p/ cima, chama a rotina de conclusão
  de cada nível conforme `Control` × resultado, honra `STATUS_MORE_PROCESSING_REQUIRED`
  (interrompe/retoma), escreve `UserIosb`, sinaliza `UserEvent`. `IoSetCompletionRoutine_k`
  passa a gravar na **próxima** localização + `Control`. **Ungated** (pintok não importa
  nenhuma dessas APIs — verificado no import table dele).
- **INC 4** (`io.c`+`main.c`): `KiCompletionSelfTest` — pilha real de 2 drivers (filtro+função)
  prova rotina no nível certo + `SL_INVOKE_ON_*` + `MORE_PROCESSING_REQUIRED` + write-back.
  `[cmpl-test] ... OK` no boot.
- **INC 5** (`io.c`): `IoBuild{DeviceIoControl,AsynchronousFsd}Request_k` guardam
  `UserIosb`/`UserEvent` (era cópia p/ dentro do IRP) → write-back real p/ IOCTL síncrono.
- **INC 6** (`ntoskrnl.c`): exporta `IofCompleteRequest`/`IofCallDriver` (append-only). Checagem
  crítica: mesmo a export image sintética do pintok resolvendo `IofCompleteRequest` p/ código
  real, o baseline se manteve **C0000365**.
- **INC 7** (`ntoskrnl.c`): `MmPageEntireDriver` (no-op seguro). **`null.sys` roda SOZINHO** e
  resolve TODOS os imports (nada mais no `generic_zero_stub`): `DriverEntry`→SUCCESS,
  `WRITE 8→info=8`, `READ 8→0xC0000011` — agora pela **via REAL do `IofCompleteRequest`**.

Baseline pintok re-verificado após CADA incremento: `P1/P2/P3 PROVA PASSOU`, `C0000365`, sem
`Sistema parado`.

### beep.sys — diagnóstico (Frente 2 esticada, DEFERIDA)

Copiei `C:\Windows\System32\drivers\beep.sys` (10 KB) p/ `build\` e carreguei sozinho. Este
`beep.sys` do Win10 **não** é o "beep simples": usa o modelo **StartIo + KDEVICE_QUEUE +
cancel-safe-queue (Csq) + cancel-spinlock** (imports: `IoCsqInitialize/InsertIrp/RemoveNextIrp`,
`IoStartPacket/StartNextPacket`, `KeRemove(Entry)DeviceQueue`, `IoAcquire/ReleaseCancelSpinLock`,
`MmLockPagableDataSection`, `IoGetRequestorSessionId`, `IoSetStartIoAttributes`). Com essas
APIs ainda como stub, o `DriverEntry` do beep **page-faulta cedo** através de ponteiros
selvagens (cr2=0x2B99…, 0x875F…) e cascateia p/ opcode inválido → `Sistema parado`. Ou seja,
habilitar o beep é um **subsistema real** (com dimensão de layout de struct do IO_CSQ, estilo
o trabalho do pintok), não só "adicionar 13 APIs".

**Decisão:** Frente 1 (máquina de conclusão) + Frente 2 núcleo (null.sys pela via real) estão
**completas e provadas**. O beep é esticada opcional (o plano já previa null.sys como suficiente
p/ a Frente 2). Piv­ot p/ **Frente 3 (rodar .exe REAL do Windows)** — a outra metade do objetivo,
intocada. beep.sys fica documentado aqui p/ retomar depois (começar por safe-stubs que devolvem
valores sensatos: `MmLockPagableDataSection`→devolve o arg, etc., e o IO_CSQ real).

---

## MARCO 3: Frente 3 (rodar .exe REAL) — Fase 3a COMPLETA + escopo da Fase 3b observado

Branch `feat/kernel-foundation-irql-dpc` (mesma leva). Depois de Frente 1+2, abrimos a
Frente 3 (rodar executaveis .exe reais do Windows). A Fase 3a (fundacao) esta **completa e
provada**; a Fase 3b (rodar um binario com CRT real) foi **diagnosticada** (escopo exato).

### Fase 3a — COMPLETA (2 commits)
- **Relocacao no caminho de usuario** (`ldr_run`, loader.c): espelha a relocacao do caminho
  de driver. Um .exe de ImageBase alto (>= 1 GiB, ex. 0x140000000 = default MSVC) e realocado
  p/ RAM baixa via PMM + `.reloc`. Prova: `apps/hihello.c` (ImageBase 0x140000000, .reloc
  forcado) realocado p/ 0x4319000 e **rodando em ring 3** (`relocacoes aplicadas: 1`;
  imprimiu; saiu limpo). Apps de base baixa seguem no caminho else — sem regressao.
- **TEB/PEB + gs-base** (`usermode.c`): o processo ring-3 tem TEB+PEB minimos (no fundo da
  janela de pilha, 0x600000/0x601000). `enter_ring3` seta `IA32_GS_BASE=TEB` apos o load do
  seletor de gs e antes do iretq; na volta ao kernel restaura `IA32_GS_BASE=KPCR` (conserta um
  bug latente — antes o kernel ficava com gs-base 0 depois que uma app rodava). Prova
  (`apps/tebtest.c`): em ring 3, `gs:[0x30]=0x600000` (TEB.Self), `gs:[0x60]=0x601000` (PEB),
  `PEB->ImageBaseAddress=0x1900000` (base real do tebtest). conhello roda DEPOIS do tebtest
  (KPCR restaurado entre apps). E' o acesso exato que o CRT de um .exe real faz no arranque.

### Fase 3b — DIAGNOSTICADA (escopo do "primeiro .exe real")
`apps/crthello.c` = `printf("...")` compilado com o CRT REAL do mingw:
`zig cc -target x86_64-windows-gnu apps/crthello.c -o build/crthello.exe` (SEM -nostdlib).
Resultado (152 KB, ImageBase **0x400000** — zig-mingw poe EXE baixo, entao ele NAO precisa
de relocacao): ao carregar, o loader lista os imports que faltam (a superficie da Fase 3b):
- **UCRT via apisets `api-ms-win-crt-*.dll`** (~40 funcs): heap (`malloc/free/calloc/_set_new_mode`),
  runtime (`_initterm`, `_set_app_type`, `__p___argc/argv/__wargv`, `_cexit`, `exit`,
  `_configure_narrow/wide_argv`, `_initialize_narrow/wide_environment`, `_crt_atexit`,
  `_crt_at_quick_exit`, `abort`, `signal`, `_set_invalid_parameter_handler`), stdio
  (`__acrt_iob_func`, `__stdio_common_vfprintf/vfwprintf`, `fwrite`, `__p__commode`,
  `__p__fmode`), private (`__C_specific_handler`), string (`strlen`, `strncmp`), math
  (`__setusermatherr`), environment (`__p__environ/_wenviron`), time (`__daylight`,
  `__timezone`, `__tzname`, `_tzset`).
- **KERNEL32.dll** (~10): `Initialize/Enter/Leave/DeleteCriticalSection`, `TlsGetValue`,
  `VirtualProtect`, `VirtualQuery`, `SetUnhandledExceptionFilter`, `Sleep`, `GetLastError`.
Com IAT=0 (tudo sem resolver), o startup do CRT chamou por um slot nulo -> `rip=0 cr2=0` ->
`Sistema parado`.

**Plano da Fase 3b (proxima sessao — heavy, ~2-4 sessoes):** (a) resolver os apisets
`api-ms-win-crt-*` — ou implementar essas DLLs, ou fazer o `pe_get_export`/loader resolver
forwarders/apiset p/ uma msvcrt/ucrt nossa; agent B recomendou mirar o msvcrt CLASSICO (mais
leve) em vez do UCRT — avaliar retargetar o link p/ msvcrt.dll; (b) crescer kernel32 (~10
funcs acima; CriticalSection podem ser no-ops em UP, Tls* real, VirtualProtect/Query
plausiveis, GetLastError via TEB LastError); (c) init de loader (processar TLS dir, chamar
DllMain, registrar .pdata); (d) `__C_specific_handler`/SetUnhandledExceptionFilter stubs.
Alvo: `crthello.exe` imprime e sai 0. Comecar por WriteConsole/WriteFile (evitar stdio do CRT
no inicio). Fase 3a ja entrega tudo que o startup do CRT le por gs (TEB/PEB).

### Fase 3b — FEITA: um .exe com CRT REAL do Windows roda printf() no MeuOS

Implementada (mesma leva). Pecas: **dll/win32/ucrtbase/ucrtbase.c** (NOVA) = CRT minimo
(~35 exports cobrindo os apisets api-ms-win-crt-*): malloc/strlen/strncmp reais; argv/env
accessors; `_initterm` no-op (seguro p/ hello C); exit/abort->ExitProcess; `__C_specific_handler`
->ExceptionContinueSearch; e **printf REAL** — `__stdio_common_vfprintf` formata o va_list
(x64: char* com slots de 8 bytes; %s %c %d %u %x %p %l/%ll + largura/zero-pad) e escreve via
WriteFile->NtWriteFile->console. **loader.c** `ldr_resolve` redireciona `api-ms-win-crt-*.dll`
-> `ucrtbase.dll` (igual aos apisets do Windows). **kernel32.c** +10 funcs do startup
(CriticalSection no-op, TlsGetValue, VirtualProtect/Query, SetUnhandledExceptionFilter, Sleep,
GetLastError). Alvo `apps/crthello.c` (printf).

**Prova:** `zig cc -target x86_64-windows-gnu apps/crthello.c` (CRT real), rodado com
`run.ps1 -Modules ntdll,kernel32,user32,ucrtbase,crthello`:
```
  [crthello] Ola de um .exe com CRT REAL (mingw) rodando no MeuOS!
  [crthello] printf de verdade: 2+2=4, hex=0xff, str=MeuOS, char=!
[ldr] o .exe (ring 3) terminou; de volta ao kernel.
```
TODOS os imports resolvidos (zero "nao resolvido"); startup do CRT chega em main(); printf
formata certo; sai limpo. Apoiado na Fase 3a (TEB/PEB/gs) + relocacao. pintok C0000365 mantido.

**FRENTE 3 entregue no basico: rodamos um EXECUTAVEL com o CRT REAL do Windows.** Proximos
passos possiveis (nao criticos): stdio real de FILE (fopen/fread p/ arquivos), mais superficie
kernel32 conforme apps maiores pedirem, e — p/ binarios MSVC/System32 — resolucao de
forwarders/apiset generico + ucrtbase clássico. beep.sys (Frente 2 esticada) segue deferido.

---

## MARCO 4 — Frente 3 Fase 3c: ENTRADA de teclado no CRT (scanf/getchar) FEITA

Depois de printf (saida, Fase 3b), agora um .exe com CRT REAL LE do teclado via `scanf()`
DE VERDADE. Caminho da ENTRADA: `main -> scanf -> __stdio_common_vfscanf (ucrtbase) ->
ReadFile (kernel32) -> NtReadFile -> fila de stdin do teclado (IRQ1, keyboard.c)`.

Pecas (todas ring-3 / harness — NENHUMA mudanca no kernel):
- **dll/win32/ucrtbase/ucrtbase.c**: +entrada — `getchar/fgetc/_fgetc_nolock/getc/ungetc/fgets/
  fread` e o **`__stdio_common_vfscanf` real** (subset `%d %i %u %x/%X %c %s`, com `*`, largura,
  `l`/`h`), espelhando o formatador de saida `ucrt_vfmt`. Pushback de 1 char p/ o scanf "espiar".
- **BLOQUEIO em ring 3 (nao no kernel):** o `sys_readfile` do console segue NAO-bloqueante de
  proposito — o `cmd.exe` depende disso (ele desiste apos ~2M tentativas ociosas em headless; se
  o kernel bloqueasse, o boot padrao travaria). Entao o "esperar digitar" e' feito na ucrtbase:
  `ucrt_getc_raw` gira chamando ReadFile ate a tecla chegar (IRQ1 enfileira em paralelo). Assim
  getchar/scanf ESPERAM a entrada como no Windows, SEM tocar no kernel.
- **apps/echoin.c** (NOVO): `scanf("%d %s", &n, palavra)`.
- **run.ps1** `-SendKeys "..."` (NOVO): injeta teclas via QMP `send-key` apos o boot — prova de
  entrada de teclado SEM display (mesmo caminho do PS/2 real -> IRQ1). Aditivo: so ativa com o
  switch; o baseline do pintok (que nao usa) fica intacto.

**Prova:** `run.ps1 -Modules ntdll,kernel32,user32,ucrtbase,echoin -Headless -TimeoutSec 25
-SendKeys "4 2 spc m e u o s ret"`:
```
  [echoin] digite: <numero> <palavra> e Enter
  [echoin] scanf casou 2 campos
  [echoin] numero=42 (dobro=84), palavra=meuos
[ldr] o .exe (ring 3) terminou; de volta ao kernel.
```
scanf casou os 2 campos (42 e "meuos"); zero "import nao resolvido"; saida limpa. Regressao do
pintok conferida: [P1]/[P2]/[P3] PROVA PASSOU, CPUID x3 -> i7-9700K, syslog `10 ; 0 ; 26100.`,
`DriverEntry retornou status=0x00000000C0000365`, SEM "Sistema parado". Baseline MANTIDO.

---

## MARCO 5 — Frente 3 Fase 3d: ARQUIVOS reais no NTFS via CRT (fopen/fread/fwrite) FEITA

Um .exe com CRT REAL agora le e escreve ARQUIVOS REAIS no disco NTFS via fopen/fread/fwrite/
fclose. Caminho: `fopen -> CreateFileA -> NtCreateFile -> volume NTFS (\Device\Harddisk0\
Partition1)`; `fread -> ReadFile -> IRP_MJ_READ -> ntfs_read_file`; `fwrite -> WriteFile ->
IRP_MJ_WRITE -> ntfs_write_file`.

**Diagnostico:** quase tudo ja existia — o kernel ja tinha um teste completo de NTFS no boot
(main.c FASE 2/3/4: IDENTIFY, mount, leitura de \hello.txt, ESCRITA que cresce o arquivo) e a
via usuario->IRP (sys_createfile/read/write) ja resolvia C:\... no volume. **O unico elo
faltando era o `-Disk` no run.ps1** (o hal/disk.c ja pedia "Rode com -Disk"; make-ntfs-disk.ps1
ja gera build\disk.img). Logo o INC 2 nao precisou de NENHUMA mudanca no kernel.

Pecas (ring-3 / harness):
- **run.ps1** `-Disk` (NOVO): anexa build\disk.img como IDE primario master
  (`-drive file=disk.img,format=raw,if=ide,index=0,media=disk`) — o canal 0x1F0 que o hal/disk.c
  le por ATA PIO. Gated: sem o switch, nada muda (baseline do pintok intacto).
- **dll/win32/ucrtbase/ucrtbase.c**: modelo FILE estendido (console fd 0/1/2 OU arquivo fd=-1 +
  HANDLE do CreateFileA); `fopen/fclose/feof/ferror/fflush`; `fread/fwrite/fgetc/fgets` ramificam
  arquivo (ReadFile/WriteFile no handle) vs console. `fseek/ftell` stub (sem syscall de seek; p/
  reler, reabrir). v1: abre EXISTENTE ("w"/"wb" sobrescreve do offset 0); criar arquivo novo (MFT)
  fica deferido.
- **apps/filecat.c** (NOVO): le C:\hello.txt e faz round-trip de escrita em C:\dir1\file.txt.

**Prova:** `make-ntfs-disk.ps1` gera o disco; `run.ps1 -Disk -Modules ntdll,kernel32,user32,
ucrtbase,filecat -Headless`:
```
  [filecat] li 159 bytes de C:\hello.txt:
    "MeuOS FASE 4: arquivo NTFS reescrito E AUMENTADO no lugar, resident grow dentro do ..."
  [filecat] escrevi 19 B "MEUOS-FILEIO-OK-123"; reli 19 B "MEUOS-FILEIO-OK-123"
```
(O conteudo lido de hello.txt e' a mensagem do teste de ESCRITA do boot — o proprio boot cresce o
arquivo p/ 191 bytes ANTES do filecat; o fread devolve min(pedido,disponivel)=159, correto.) O
round-trip em dir1\file.txt (grava 19 B, reabre, rele 19 B identicos via IRP_MJ_WRITE/READ) prova
a ida-e-volta. Zero "import nao resolvido"; saida limpa. Regressao pintok OK (C0000365 mantido,
CPUID x3 -> i7-9700K, `10 ; 0 ; 26100.`, SEM "Sistema parado").

**Nota (fixtures):** disk.img e' artefato de build (gitignored); gere com `apps/make-ntfs-disk.ps1`
(modo sintetico Python sem admin, ou real com admin+Hyper-V) antes de `run.ps1 -Disk`.

---

## MARCO 6 — Frente 3 Fase 3e: GUI de TERCEIRO com CRT REAL (WinMain) no win32k FEITA

Uma app GRAFICA de estrutura PADRAO do Windows — `WinMain` + RegisterClassA + CreateWindowExA +
loop GetMessage/Translate/Dispatch + WM_PAINT desenhando — compilada com o **CRT REAL do mingw**
(subsistema WINDOWS: o startup `WinMainCRTStartup` chama WinMain) roda no win32k PROPRIO do MeuOS
e ABRE UMA JANELA que desenha. Diferente do `guiapp.c` (que usa `_start`/-nostdlib e declara tudo
a mao), este e' um binario Win32 "de livro" (inclui `<windows.h>`, usa a API padrao) — o alvo do
item 4 (rodar um binario de janela de terceiro).

**Diagnostico primeiro:** compilei `apps/guihello.c` e rodei — faltavam 4 imports (e ao chamar um
deles com IAT=0 ele saltava p/ rip=0 -> "Sistema parado"): `USER32!UpdateWindow`,
`KERNEL32!GetStartupInfoA`, `ucrtbase!__p__acmdln`, `ucrtbase!_ismbblead`. Adicionei os 4 (todos
ring-3, NENHUMA mudanca no kernel):
- **user32.c** `UpdateWindow` -> `NtUserInvalidate` (marca invalida -> WM_PAINT).
- **kernel32.c** `GetStartupInfoA` (zera STARTUPINFOA 104 B -> CRT usa SW_SHOWDEFAULT).
- **ucrtbase.c** `__p__acmdln` (ponteiro da linha de comando p/ o startup GUI) + `_ismbblead`
  (sem MBCS -> 0).

**Prova (screendump):** `run.ps1 -Screendump -Modules ntdll,kernel32,user32,gdi32,ucrtbase,
guihello`. Serial: `RegisterClass 'GuiHelloClass'` -> `CreateWindowEx -> HWND #1 titulo='GuiHello
(terceiro, CRT)' x=50 y=45 w=240 h=130` -> `InvalidateRect -> WM_PAINT` -> `FillRect(cor=200)` +
`TextOut("Janela de TERCEIRO (CRT real)")` + `TextOut("WinMain + WM_PAINT OK")`. O win32k tem um
modo demo que injeta "Hi" e manda WM_QUIT (o app sai limpo; ciclo de vida Win32 completo). A imagem
`build/screen_guihello.png` (1024x768) mostra a JANELA com barra de titulo azul "GuiHello
(terceiro, CRT)", o texto e o retangulo vermelho "WinMain + WM_PAINT OK", sobre o desktop
Win10/11 (wallpaper + taskbar + relogio). Zero "import nao resolvido". Regressao pintok OK
(C0000365 mantido, CPUID x3 -> i7-9700K, `10 ; 0 ; 26100.`, SEM "Sistema parado").
