# PROMPT — próxima sessão: continuar rodando o `explorer.exe` REAL do Windows 10

Cole isto como prompt inicial. É AUTOSSUFICIENTE. É LONGO de propósito: tem o mapa de RE, o
estado, as ferramentas e os gotchas.

---

## ⚙️ MODO DE TRABALHO (LEIA ISTO)
**Trabalhe AUTÔNOMO. NÃO pare p/ perguntar "continua?".** Rode o loop de bring-up do explorer
real, derrubando **um muro por vez, DE VERDADE, POR COMPLETO** — o usuário é enfático: **NÃO
FAÇA STUB genérico catch-all; implemente o que precisa por completo.** Vá sem parar até ~600k de
tokens; então escreva um novo `PROMPT-PROXIMA-SESSAO.md`, faça o commit final e dê o balanço.
**Prefira IMPLEMENTAR a só diagnosticar** — cada sessão deve DERRUBAR muro(s) e commitar.

**COMENTÁRIOS GRANDES e MUITO EXPLICATIVOS (PT-BR)**: O QUE a função faz, POR QUE (mecânica do
NT/Windows) e o CONTEXTO do explorer (qual RVA/função exercita aquilo). Vale p/ kernel E DLLs.

**pintok DESTRAVADO** — pode mexer no kernel, mas rode o pintok a CADA mudança de kernel
(`.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`) e NÃO deixe quebrar (regra de ouro).

**COMMITS: NÃO use `Co-Authored-By: Claude`** (preferência do usuário). Mensagens humanas,
descrevendo o que foi feito — SEM "PROMPT sessão", numeração, emojis ou marketing. Tudo na `main`.
Disciplina: **build → rodar explorer → diagnosticar → implementar REAL → build → pintok →
commit + push**. Zig: `tools\zig-windows-x86_64-0.13.0\zig.exe`. Do bash rode PowerShell:
`powershell.exe -ExecutionPolicy Bypass -File run.ps1 -Scenario explorerreal -Headless -TimeoutSec 55`.

**BUILD RÁPIDO DO KERNEL (~7 s, sem build.ps1 inteiro)** — recompila só `win32k.c` (que inclui
win32kbase/full/shell) e re-linka os `build\*.o` já cacheados (salvo em `/tmp/kbuild.sh`):
```
ZIG=tools/zig-windows-x86_64-0.13.0/zig.exe
CF="-target x86_64-freestanding-none -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -Isrc -Isrc/ntos -Isrc/ntos/inc -Isrc/drivers -Isrc/subsystems -Isdk -Isdk/ddk -std=c11 -O2 -c"
$ZIG cc $CF src/subsystems/win32/win32k.c -o build/subsystems_win32_win32k_c.o
$ZIG cc -target x86_64-freestanding-none -nostdlib -static -no-pie -Wl,-T,linker.ld -Wl,--build-id=none -Wl,-z,noexecstack build/*.o -o build/kernel.elf
$ZIG objcopy -O binary build/kernel.elf build/kernel.bin
```
**BUILD RÁPIDO DE 1 DLL** — cada DLL é 1 comando zig. Ex.:
```
$ZIG cc -target x86_64-windows-gnu -shared -nostdlib -e DllMain -Wl,--image-base=0x5500000 -Wl,--dynamicbase -o build/combase.dll dll/win32/combase/combase.c
```
Bases: combase=0x5500000, shell32=0x5700000, shcore=0x5800000. ucrtbase=0x3300000 (linka
`build/libkernel32.a build/libntdll.a` + `-Wl,--out-implib,build/libucrtbase.a`). O `run.ps1`
copia os build\*.dll e roda; não precisa rebuildar tudo.

## 🎯 A MISSÃO
Rodar o **binário REAL** `C:\Windows\explorer.exe` no MeuOS (modelo Wine/ReactOS: implementar as
DLLs/mecânicas do NT que o binário exige). Ideal: **desktop PERSISTENTE e PINTADO — ✅ ATINGIDO
na sessão 9.**

## 📍 ESTADO — SESSÃO 9: o explorer.exe REAL **PERSISTE com o desktop PINTADO** 🎉
Commit `364e96e` (persistência + pintura) + limpeza de log + docs. **Marco da missão atingido.**

Screenshot (headless, `-Screendump`): papel de parede Win10 (gradiente azul) + taskbar com botão
Iniciar + system tray ("2 nucleos", relógio andando "00:00:38", "MeuOS 11"). Rodou **60 s
estável, 0 falhas, 0 saídas**. `pintok` VERDE. Steady-state limpo (após slot10: 3 DispatchMessage
+ 1 compose, depois só `gpu_present`).

### Como a persistência foi RESOLVIDA (corrige o modelo da s7/s8)
Na escada `rdi!=0` do `wWinMain` (0x23ec2..0x24035), o objeto criado em **0x23d6d** por
`CoCreateInstance(AppResolver {660B90C8-73A9-4B58-8CAE-355B7F55341B}, IID {BA5A92AE-...})` é
**RETIDO em `[rsp+0x58]`** até o teardown. Varredura de TODOS os acessos a `[rsp+0x58]` no
wWinMain: entre a criação (0x23d6d) e a leitura-porteira (0x23fdb) **NÃO há escrita** — o
"released→0" anotado na s8 estava ERRADO. No fim da escada o explorer chama, nesse objeto,
`slot5` (vtbl+0x28, "pre-run") e **`slot10` (vtbl+0x50) = o "RUN" / loop de mensagens**. Tudo
depois de 0x23ffe é teardown (Release/CoUninitialize/ExitProcess) — só alcançado quando o loop
retorna. **NÃO era o DesktopExplorerHost** (a escada ExplorerHostCreator→Create não preenche
`[rsp+0x58]`; Create só recebe rcx+rdx). E **0x23edd** é só `PostMessageW(GLOBAL_A [0x433980]=hwnd,
0x590, 1, 0)` via delay-import (`.didat` slot 0x45D600), NÃO um loop.

**Fix (combase, `specific_object_for`):** CLSID_AppResolver devolve um objeto cujo `slot10` roda
`run_message_loop()` (GetMessage/DispatchMessage do win32k, nunca retorna). Antes `[rsp+0x58]`
recebia o objeto UNIVERSAL (slot10 = univ_fill, retorno imediato) → wWinMain caía no ExitProcess.

**Fix (win32kbase, `NtUserGetMessage_k` idle):** com o explorer parado no loop de msg, a thread
PRINCIPAL vive ociosa ali. Passou a ser o lugar de manter o desktop PINTADO: 1ª volta ociosa faz
`win32k_compose()` completo (papel de parede + taskbar); depois ~2x/s (throttle por `g_ticks`
@100Hz) faz `win32k_refresh_taskbar()` (relógio ao vivo) sem repintar janelas.

## 🧭 PRÓXIMAS FRONTEIRAS (mapeadas na s9; todas DEEP — o visível já está pronto via compose)
O desktop VISÍVEL é o **compose sintético** do win32k (não os pixels do explorer). As frentes
abaixo aumentam a FIDELIDADE ("o explorer rodando de verdade por completo") mas quase não mudam a
tela — por isso são deep/low-visible-payoff. Escolha com esse trade-off em mente.

1. **Threadproc do taskbar (RVA 0x7A880, tid=3, param pData=RVA 0x434820 → 0x474D820).** É o loop
   de msg do `Shell_TrayWnd`. Hoje está **STARVED** (nunca escalonada). Se rodasse: `sub_0x7a8f0`
   (0x7A930) faz `PeekMessageW(&msg,0,0,0,PM_REMOVE)` (NÃO-bloqueante, slot 0x45D750; nosso user32
   devolve 0) → se 0, deref **`[pData+0x320]`** (0x7A962) p/ chamar slot5 (idle, vtbl+0x28) / slot7
   (dispatch, vtbl+0x38). **`[pData+0x320]` é sempre 0** (único write é o zero-init em 0x15DF4;
   `sub_0xb7b58` chamado em 0x372CE é só NULL-CHECK, não criador; a criação real está numa cadeia
   COM/DUI da callback 0x2C5BC que NÃO exercitamos e que scans estáticos NÃO localizam — precisa de
   write-watch em runtime). Para RODAR LIMPO precisa de DOIS fixes acoplados: (a) **fairness de
   scheduler** — a PRINCIPAL bloqueia em `NtUserGetMessage_k` com `sti;hlt` DENTRO do syscall
   (ring0); o timer não preempta ring0 → nenhuma outra thread roda. Fix faithful: ceder ao
   scheduler quando o GetMessage bloqueia. (b) **popular `[pData+0x320]`** com objeto cujo slot5
   BLOQUEIE (senão busy-spin com PeekMessage não-bloqueante). SEM (b), acordar a thread só causa
   #PF contido em 0x7A969 (null deref) — inofensivo mas suja o log. AVISO: injetar em pData+0x320
   pelo shcore é HACK frágil (viola o "sem stub"); o caminho faithful é fazer a criação COM/DUI da
   callback suceder.

2. **Renderização própria da UI do explorer (DirectUI/dui70).** Massivo. O explorer desenha
   taskbar/Start via DUI em `Shell_TrayWnd`; nós desenhamos um Win10 sintético. Reproduzir isso é
   projeto grande (theming, GDI+, DUI). Baixa prioridade vs. esforço.

3. **Cobertura COM/DLL** (`OLEAUT32`, `ole32`, `PROPSYS`, `RPCRT4` — hoje "import nao resolvido").
   O explorer PERSISTE sem elas (não estão no caminho crítico). Implementá-las habilita subsistemas
   mais profundos — mas nada visível hoje (a thread está parada no loop). Só vale p/ destravar um
   comportamento específico.

4. **Cenário `desktop` (regressão): explorer CASEIRO sai por WM_QUIT (PRE-EXISTENTE, não é do s9).**
   O `win32k_inject_demo_input` (crutch de teste headless) posta um WM_QUIT GLOBAL na 1ª janela
   simples (a de logon do winlogon, HWND #1); o winlogon consome UM WM_QUIT (o seu PostQuitMessage)
   e o do demo-inject VAZA p/ o explorer caseiro → ele sai. Fix limpo: escopar o demo-inject só p/
   o cenário `full` (ex.: kernel checa se guiapp.exe está nos módulos). `explorerreal` NÃO é
   afetado (não dispara demo-inject). OBS: `desktop` é LENTO p/ chegar ao shell — use `-TimeoutSec 45`.

## 🗺️ MAPA DE RE (base disassembler = 0x140000000; runtime base logada por run;
`explorerreal` recente = **0x04319000**; RVA = runtime − base). ⚠️ **GOTCHA `lea [rip±disp]`** —
recompute `rip_APÓS_a_instr ± disp`. ⚠️ **disrange/disdll fazem sweep LINEAR** e dessincronizam em
dados/padding — comece num boundary de instrução limpo.
- `wWinMain`=**0x23350** (prólogo: `rbp = rsp_entrada − 0x328`; após `sub rsp,0x400`, **rbp = rsp
  + 0x100**; logo `[rsp+0x58]` = rsp_entrada − 0x3D0, e `[rbp+0x30]` ≠ `[rsp+0x58]`).
- **0x23d6d**: `CoCreateInstance(AppResolver {660B90C8}, IID {BA5A92AE-BFD7-4916-854F-6B3A402B84A8},
  CLSCTX=3, out=&[rsp+0x58])` — o objeto da PERSISTÊNCIA. slot4 (vtbl+0x20) usado cedo em 0x23d89.
- Mode 3 começa em 0x23dd7 (`call sub_0x87568`; rdi=rax). `sub_0x87568`→rdi: testa GLOBAL_A
  [0x433980], `sub_0x87628` (SHCreateThread), GLOBAL_B [0x433978], **SHELL32!ord#200** (0x875cd).
- Escada `rdi!=0` (0x23ec2..0x24035): **0x23edd** `PostMessageW([0x433980],0x590,1,0)` (delay,
  slot 0x45D600) · 0x23ee9 call 0x2e33c · 0x23f31 CCI(ExplorerHostCreator{AB0B37EC}) · 0x23f56
  Create(slot3)({682159D9}=DesktopExplorerHost) [só rcx+rdx] · 0x23f95 SHELL32!ord#201 [CLEANUP]
  · 0x23fcf ord#206 [CLEANUP] · **0x23fdb** `rcx=[rsp+0x58]`(=AppResolver); if !=0: **slot5
  (vtbl+0x28) + slot10 (vtbl+0x50=LOOP)** · 0x24004+ teardown (Release/CoUninitialize/ExitProcess).
- Threadproc taskbar **0x7A880**: SHChangeNotifyRegisterThread → `sub_0x7b2b0` (LoadAccelerators→
  [pData+0x108], SetThreadPriority, ChangeWindowMessageFilterEx; retorna) → `sub_0x7a8f0` (loop
  0x7A930). Loop: PeekMessageW (0x7A945, slot 0x45D750) → deref [pData+0x320] (0x7A962 slot5 /
  0x7A9ED slot7). WM_QUIT=0x12 sai em 0x22259f.
- GUIDs: AppResolver `{660B90C8-73A9-4B58-8CAE-355B7F55341B}` (IID `{BA5A92AE-BFD7-4916-854F-6B3A402B84A8}`)
  · ExplorerHostCreator `{AB0B37EC-56F6-4A0E-A8FD-7A8BF7C2DA96}` (IID `{A3FD6B4C-B949-09B7-CB3C-D3F0473AEC8F}`)
  · DesktopExplorerHost `{682159D9-C321-47CA-B3F1-30E36B2EC8B9}` · TaskbandPin `{90AA3A4E-...}`.
- CFG: chamadas `48 FF 15 [rip→0x3A0288]` são `__guard_dispatch_icall` (rax=alvo virtual). Outros
  `[rip]` são IAT normal. disrange rotula "VIRT slot N (vtbl+0xNN)" pelo `mov rax,[rax+0xNN]` antes.

## 🔩 GATES DE DIAGNÓSTICO (deixe em 0/baixo-ruído nos commits — HOJE OK)
- **`COMBASE_DBG`** combase.c:51 (=0). **`EH_DBG`** ntdll.c:607 (=0).
- Sempre-ativos baixo ruído (mantenha): `[cb] AppResolver ...` (1x) + `AppResolver::slot4/5/10`
  (provam a escada de persistência) · `[shcore] SHCreateThread ... GLOBAL_C(pData+8)=` · `[shell32]
  ord#200 CHAMADA` · `[shell] compose(true-color)` · ucrtbase `_CxxThrowException` (só na exceção).

## 🔁 O LOOP / VALIDAÇÃO
`.\run.ps1 -Scenario explorerreal -Headless -TimeoutSec 55`. Ver `build\serial.log`. Prova de
persistência: `grep AppResolver::slot10` presente E **sem** `ExitProcess da thread PRINCIPAL`/
`processo pid=1 encerrou`. Prova visual: `-Screendump` → `build\screen.ppm` (converta com PIL:
`python -c "from PIL import Image; Image.open('build/screen.ppm').save('/tmp/s.png')"`). pintok:
`.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40` (P1/P2/P3 PASSOU, CPUID→Intel i7-9700K, vgk
DriverEntry fundo). Regressão shell caseiro: `.\run.ps1 -Scenario desktop -Headless -TimeoutSec 45`
(LENTO; ver muro 4).

## 🛠️ FERRAMENTAS (raiz, versionadas)
`disrange.py <rva> <n>` (desmonta explorer.exe por RVA, nomeia CALL[IAT]) · **`disdll.py <pe>
<rva> [n]`** (idem p/ QUALQUER PE) · **`pdata.py <pe> <rva>`** (EHANDLER/UHANDLER + FuncInfo) ·
**`funcinfo.py <pe> <rva>`** (try/catch, __CxxFrameHandler3 v3) · `whocalls.py`/`callers.py` (E8
diretos) · `dumpimports.py`/`dumpexports.py`. **`ripref.py` BUGADO** — p/ refs a endereço/GUID use
BYTE-SCAN em python (padrão `48/4C 8B/89/8D modrm` rip-rel; p/ +0xNNN: SIB=0x24, disp32=0xNNN).

## ⛔ REGRA DE OURO — pintok.sys. NÃO QUEBRAR.
Após CADA incremento no KERNEL: `.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`. Syscalls
novos: append no FIM do `s_ssdt[]` (último = **#52 sys_module_for_pc**). A mudança de kernel da s9
(compose/refresh no idle do GetMessage) NÃO afeta pintok (sem ring-3/janelas no cenário pintok).

## 📌 NOTAS / GOTCHAS
- **Objeto UNIVERSAL da combase** (slots 6-63 = univ_fill S_OK; 3-5 = E_NOTIMPL): SCAFFOLD. NÃO
  estenda p/ slots 3-5 (catch-all proibido). Implemente objetos ESPECÍFICOS por CLSID em
  `specific_object_for()` (já tem TaskbandPin, ExplorerHostCreator, **AppResolver**).
- **Toolchain gnu** (`-target x86_64-windows-gnu`): sem SEH/catch MSVC nas NOSSAS DLLs.
- Fila de msg do win32k é **GLOBAL** (por-processo). `NtUserGetMessage_k` bloqueia com `sti;hlt`
  enquanto `s_nwin>0` (Shell_TrayWnd mantém vivo) e faz compose/refresh/present por volta ociosa.
- Threads ring-3: pilha/TEB na faixa do PMM (0x04Bx/0x05Cxxxxx); a PRINCIPAL em [0x600000,0x700000).
  fault de ring-3 mata SÓ a thread (ki_ring3_fault_contain).
- `run.ps1` roda `-smp 2 -accel tcg,thread=multi`. AP sem timer local (bug TCG) → sem preempção no AP.
- SDK em `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\`.

## 📜 COMMITS DA SESSÃO 9 (branch `main`; SEM Co-Authored-By)
- `364e96e` explorer real persiste com desktop pintado (AppResolver::slot10 = loop de msg retido
  em [rsp+0x58]; win32kbase idle pinta o desktop + relógio).
- combase: reduz ruído do log do AppResolver (1x, não por consumidor).
- docs: README + PROMPT-PROXIMA-SESSAO atualizados p/ o marco.

**Agora: o marco (persistência + pintura) está feito. As próximas fronteiras são de FIDELIDADE
PROFUNDA (threadproc do taskbar + scheduler; DUI; COM) — deep e de baixo payoff visível. Se for na
threadproc do taskbar, faça FAITHFUL: rastreie em runtime onde a callback cria `[pData+0x320]`
(write-watch) e faça a cadeia COM/DUI suceder — NÃO injete no pData. Vá, sem parar, até ~600k.**
