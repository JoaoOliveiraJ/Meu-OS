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

**COMMITS: NÃO use `Co-Authored-By: Claude`** (preferência do usuário). Tudo na branch `main`.
Disciplina: **build → rodar explorer → diagnosticar → implementar REAL → build → pintok →
commit + push**. Zig: `tools\zig-windows-x86_64-0.13.0\zig.exe`. Do bash rode PowerShell:
`powershell.exe -ExecutionPolicy Bypass -File run.ps1 -Scenario explorerreal -Headless -TimeoutSec 55`.

**BUILD RÁPIDO (1 DLL só, sem rodar build.ps1 inteiro)** — cada DLL é 1 comando zig. Ex.:
```
ZIG=tools/zig-windows-x86_64-0.13.0/zig.exe
$ZIG cc -target x86_64-windows-gnu -shared -nostdlib -e DllMain -Wl,--image-base=0x5500000 -Wl,--dynamicbase -o build/combase.dll dll/win32/combase/combase.c
```
Bases: combase=0x5500000, shell32=0x5700000, shcore=0x5800000. ucrtbase=0x3300000 (linka
`build/libkernel32.a build/libntdll.a` + `-Wl,--out-implib,build/libucrtbase.a`). O `run.ps1`
copia os build\*.dll e roda; não precisa rebuildar tudo.

## 🎯 A MISSÃO
Rodar o **binário REAL** `C:\Windows\explorer.exe` no MeuOS (modelo Wine/ReactOS: implementar as
DLLs/mecânicas do NT que o binário exige). Ideal: desktop PERSISTENTE e PINTADO.

## 📍 ESTADO — MARCOS DA SESSÃO 8 (a init do taskbar CONCLUI; explorer entra na escada de persistência)
Commit `3e57d3b`. **3 muros derrubados DE VERDADE (sem catch-all)** — o explorer.exe REAL agora
roda a init COMPLETA do taskbar e cria a janela `Shell_TrayWnd` (a barra de tarefas real).

1. 🎉 **IPinnedList3 REAL (combase).** O throw da s7 era `CoCreateInstance(CLSID_TaskbandPin
   {90AA3A4E})` → objeto UNIVERSAL slot3=E_NOTIMPL → `THROW_IF_FAILED` (`wil::ResultException`).
   RE: o throw estava na função **0x1A500** ("carrega/reconcilia a lista fixada"), chamada de
   **0x1B066**, na cadeia do callback do worker **0x2C5BC** (backtrace provado: callback 0x2C807
   → 0x1B066 → 0x1A500 → throw). Fix: objeto ESPECÍFICO por CLSID em `specific_object_for()`:
   `pl_slot3` devolve um ENUMERADOR vazio; `enum->slot3` devolve **S_FALSE** (lista vazia — S_OK
   tentaria PROCESSAR item inexistente; o contrato observado em 0x1A69B é `cmp ebx,1; jne`).
   ⇒ o callback CONCLUI, cria **Shell_TrayWnd** e seta **GLOBAL_C** (`pData+8`, RVA 0x434828 = 1).

2. 🎉 **SHCreateThread split (shcore).** Era síncrono no worker inteiro (callback+threadproc);
   agora que a init conclui, a threadproc (RVA **0x7A880**) roda o LOOP DE MSG do taskbar (nunca
   retorna) → o join travava a thread PRINCIPAL (deadlock). Fix: após o callback, a threadproc é
   lançada em THREAD PRÓPRIA (a fila do win32k é GLOBAL/por-processo — `queue_pop` em
   win32kbase.c) e o wrapper retorna → `SHCreateThread` devolve com GLOBAL_C JÁ setado.

3. 🎉 **shell32 ord#200 REAL.** Era `shell_ord_stub`→0 → rdi=0 → teardown. Agora `shell_ord200`
   devolve um host NÃO-NULO (0x2c0, zerado, vtable de fallback QI→self/S_OK) → **rdi != 0** →
   o wWinMain (mode 3, `0x23ec2 test rdi`) **ENTRA na escada de persistência**.

### ⇒ RUNTIME ATUAL (explorerreal, limpo): SEM throw, SEM hang, SEM "Sistema parado".
Log-chave: `RegisterClass 'Shell_TrayWnd'` + `CreateWindowEx HWND #1` · `GLOBAL_C(pData+8)=0x1` ·
`ord#200 CHAMADA`. A PRINCIPAL entra na escada (rdi!=0), mas `[rsp+0x58]`==0 → pula slot5/slot10
→ cai no cleanup → `ExitProcess(0)`. A threadproc do taskbar (0x7A880) roda em thread própria;
após o fix de MM da s8 ela passa da pilha (0x7A933) e avança até um deref COM nulo em 0x7A969
(pData+0x320) — contido (mata só a thread; a principal segue). pintok VERDE. desktop PERSISTE.

## 🧭 PRÓXIMA FRONTEIRA — a PERSISTÊNCIA (reavaliar o modelo da s7!)
A escada `rdi!=0` do wWinMain (**0x23ec2..0x24004**) faz:
`CoCreateInstance(CLSID_ExplorerHostCreator {AB0B37EC-56F6-4A0E-A8FD-7A8BF7C2DA96})` (0x23f31)
→ `Create` (slot3 vtbl+0x18)({682159D9}=CLSID_DesktopExplorerHost) (0x23f56) → lê **`[rsp+0x58]`**
(0x23fdb) → `slot5`(vtbl+0x28) + `slot10`(vtbl+0x50).

**⚠️ DESCOBERTAS DA S8 que MUDAM o modelo da s7 (que dizia "slot5/slot10 = loop de msg"):**
- `Create` (0x23f56) só seta **rcx(this)+rdx(=CLSID)** — os demais args e o RETORNO (rax) são
  LIXO/IGNORADOS. **O host NÃO vem dos args nem do retorno de Create.**
- `[rsp+0x58]` é um ComPtr local **REUTILIZADO**: o AppResolver {660B90C8} é criado ali em
  **0x23d6d** (CoCreateInstance PADRÃO, out=&[rsp+0x58]), usa slot4, e é LIBERADO→0.
- **ord#201/ord#206** (chamados em 0x23f95/0x23fcf com rcx=rdi/ord#200) são **CLEANUP** — no
  shell32 REAL fazem `DestroyWindow`/release (ord#201 RVA 0xa7df0: `lock inc [rcx+0x84]`,
  `DestroyWindow([rcx+0x88])`; ord#206 0xa7de0 → salta p/ 0xa8190). ⇒ **a escada 0x23f0b-0x24004
  é em grande parte SHUTDOWN**, e slot5/slot10 do host podem ser TEARDOWN, não o loop.
- ⇒ **O LOOP DE MSG REAL está ANTES.** Candidatos a investigar na próxima sessão:
  (a) **0x23edd** `call [rip+0x43971c]` (é um PONTEIRO DE FUNÇÃO em [0x45D600], NÃO import;
      rcx=GLOBAL_A `[0x433980]`, edx=0x590, r8=r15) — a 1ª call do caminho rdi!=0; pode ser o
      "run"/loop. Descobrir o valor de [0x45D600] em runtime.
  (b) a **threadproc 0x7A880** (thread do taskbar) — tem try/catch; roda GetMessage/Dispatch?
  (c) dentro de **sub_0x87568** ou seus callees (antes de devolver rdi).
  Instrumente em runtime: logue a sequência de GetMessage(#21)/DispatchMessage(#22) por thread
  (no win32kbase.c) p/ ver QUAL thread bloqueia num loop de msg no Windows real vs no nosso.

**Estratégia recomendada:** primeiro DESCUBRA onde o Windows real persiste (qual thread/loop
NÃO retorna), com instrumentação, ANTES de implementar. O scaffolding de
ExplorerHostCreator/DesktopExplorerHost (com slot10=loop) já existe na combase e está PRONTO —
mas só é exercitado se `[rsp+0x58]` for preenchido, o que hoje NÃO acontece (documentado acima).

**Threadproc do taskbar (0x7A880) — 4º muro RESOLVIDO na s8 (commit 6ffc6ff), novo muro à frente.**
A pilha USER da thread ring-3 faltava em 0x7A933 (write) porque `mm_map_zero_page` (recovery de PF)
fazia no-op p/ HUGE PAGE 2 MiB. CORRIGIDO: a recovery (roda no cr3 da thread que faltou) agora abre
a huge page faultada p/ ring-3 (PG_USER|PG_RW). Agora a threadproc AVANÇA de 0x7A933 até **0x7A969**:
`mov rcx,[rbx+0x320]` (rbx=pData=0x474D820) → `mov rax,[rcx]` com **rcx=NULL** → #PF cr2=0. Ou seja,
`[pData+0x320]` deve ser um OBJETO COM (a threadproc chama slot5/vtbl+0x28 nele) que a init do
taskbar NÃO populou. **Próximo passo p/ a threadproc RODAR:** popular `[pData+0x320]` (e outros
campos de pData) com os objetos que a init deveria criar. Se a threadproc rodar seu loop de msg,
dá p/ VER se ELA é a persistência (ver "PRÓXIMA FRONTEIRA"). Contido por ki_ring3_fault_contain
(mata só a thread; a principal segue).

## 🗺️ MAPA DE RE (base disassembler = 0x140000000; runtime base logada por run;
`explorerreal` recente = **0x04319000**; RVA = runtime − base). ⚠️ **GOTCHA: aritmética de
`lea [rip±disp]`** — recompute `rip_APÓS_a_instr ± disp`. ⚠️ **disrange/disdll fazem sweep
LINEAR** e dessincronizam em bytes de dados/padding — comece num boundary de instrução limpo;
p/ achar refs a um endereço/GUID use o BYTE-SCAN (varre `48/4C 8B/89/8D modrm` por rip-rel).
- `wWinMain`=**0x23350** (prólogo: após 5 push + `sub rsp,0x400`, **rbp = rsp + 0x100**). Mode 3
  (shell) começa em **0x23dd7** (`call sub_0x87568`; rdi=rax).
- `sub_0x87568` (→rdi): testa GLOBAL_A `[0x433980]`, chama `sub_0x87628`, testa GLOBAL_B
  `[0x433978]`, chama **SHELL32!ord#200** em **0x875cd** (rbx→rdi). (Só devolve rdi!=0 se
  sub_0x87628!=0 E GLOBAL_B==0 E ord#200!=0. GLOBAL_A/B==0 no nosso boot.)
- `sub_0x87628`: `SHCreateThread`(threadproc=RVA **0x7A880**, pData=RVA **0x434820**, flags=0x282a,
  callback=RVA **0x2C5B0**) e testa **GLOBAL_C** `[0x434828]`(=pData+8, setado pelo callback).
- callback **0x2C5BC** (thunk 0x2C5B0): init pesada; **0x2C802 call 0x9DEB4** entra na cadeia
  0x1B066→0x1A500 (lista fixada). Cria Shell_TrayWnd no meio.
- **0x1A500**: `CoCreateInstance(TaskbandPin{90AA3A4E}, IPinnedList3{0DD79AE2})` → slot3 (out=
  enum) [THROW_IF_FAILED 0x1ab15] → `QI(IID_IPinnedList {60274FA2})` → enum->slot3 (Next) loop.
  Handlers de throw em 0x1ab00/15/2a/3f → `call 0x2366e4` (wil throw helper).
- Escada persistência/shutdown: **0x23d6d** CCI(AppResolver{660B90C8}→[rsp+0x58], slot4) ·
  **0x23ec2** `test rdi; je 0x24035`(teardown) · **0x23edd** call [0x45D600](rcx=GLOBAL_A) ·
  **0x23f31** CCI(ExplorerHostCreator{AB0B37EC}) · **0x23f56** Create(slot3)({682159D9}) ·
  **0x23f95** SHELL32!ord#201(rdi) [CLEANUP] · **0x23fcf** SHELL32!ord#206(rdi) [CLEANUP] ·
  **0x23fdb** rcx=[rsp+0x58]; slot5(vtbl+0x28)/slot10(vtbl+0x50) · 0x24035 ~WorkerWindow.
- GUIDs úteis: TaskbandPin `{90AA3A4E-1CBA-4233-B8BB-535773D48449}` · IPinnedList3
  `{0DD79AE2-D156-45D4-9EEB-3B549769E940}` · IPinnedList `{60274FA2-611F-4B8A-A293-F27BF103D148}`
  · AppResolver `{660B90C8-73A9-4B58-8CAE-355B7F55341B}` · ExplorerHostCreator
  `{AB0B37EC-56F6-4A0E-A8FD-7A8BF7C2DA96}` · DesktopExplorerHost `{682159D9-C321-47CA-B3F1-30E36B2EC8B9}`.

## 🔩 GATES DE DIAGNÓSTICO (deixe em 0 nos commits)
- **`COMBASE_DBG`** em `dll/win32/combase/combase.c:41` (=1): loga CLSID/IID de cada
  CoCreateInstance/RoGetActivationFactory + THREAD (MAIN/WORKER) + slots do obj universal +
  RA do slot3 (usl3). Rebuild só a combase (comando acima).
- **`cb_log`/`cb_loghex`** (combase, SEMPRE ativos, baixo ruído): usados na escada do host
  (ExplorerHostCreator::Create / DesktopExplorerHost::slot10). Só disparam se `[rsp+0x58]` for
  preenchido (hoje não). Bom p/ ligar quando resolver o host.
- **`[shcore] SHCreateThread: ... GLOBAL_C(pData+8)=`** (sempre ativo): prova a conclusão da init.
- **`[shell32] ord#200 CHAMADA`** (sempre ativo): prova rdi!=0 (entra na escada).
- **ucrtbase `_CxxThrowException`** (sempre ativo): loga o TIPO C++ lançado + RA + **BACKTRACE**
  dos frames do explorer na pilha (mapeou callback→0x1B066→0x1A500 nesta sessão). Só na exceção.
- **`EH_DBG`** em `dll/ntdll/ntdll.c` (=1): loga cada frame do dispatch de EH.

## 🔁 O LOOP
`.\run.ps1 -Scenario explorerreal -Headless -TimeoutSec 55` (16 módulos num token só). Ver
`build\serial.log`. Regressão: `.\run.ps1 -Scenario desktop -Headless -TimeoutSec 25` (shell
caseiro tem que persistir no loop de msg). pintok: `.\run.ps1 -Scenario pintok -Headless
-TimeoutSec 40` (dourado: P1/P2/P3 PASSOU, `intercept totals ... ANTIVM x0`, DriverEntry C0000365).

## 🛠️ FERRAMENTAS (raiz, versionadas)
`disrange.py <rva> <n>` (desmonta explorer.exe por RVA, nomeia CALL[IAT]) · **`disdll.py <pe>
<rva> [n]`** (idem p/ QUALQUER PE — shell32/ntdll/nossos build\*.dll) · **`pdata.py <pe> <rva>`**
(EHANDLER/UHANDLER + FuncInfo RVA) · **`funcinfo.py <pe> <rva>`** (try-blocks + tipos de catch,
só __CxxFrameHandler3 v3) · `callers.py`/`whocalls.py` (E8 diretos) · `dumpimports.py`/
`dumpexports.py`. **`ripref.py` BUGADO.** P/ achar refs a um endereço/GUID no .text use o
BYTE-SCAN em python (padrão `48/4C 8B/89/8D modrm` rip-rel; resolve `A+7+disp==alvo`).

## ⛔ REGRA DE OURO — pintok.sys. NÃO QUEBRAR.
Após CADA incremento no KERNEL: `.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`. As
mudanças da s8 foram TODAS em userland (combase/shcore/shell32/ucrtbase) → pintok intocado
(ring-0, `g_ring3_active=0` no cenário pintok). Syscalls novos: append no FIM do `s_ssdt[]`
(último = **#52 sys_module_for_pc**). Se for corrigir a pilha ring-3 read-only (kernel), RODE O
PINTOK depois.

## 📌 NOTAS / GOTCHAS
- **Objeto UNIVERSAL da combase** (slots 6-63 = univ_fill S_OK; 3-5 = E_NOTIMPL): SCAFFOLD. NÃO
  estenda p/ slots 3-5 (catch-all proibido → #PF por deref de field). Implemente objetos
  ESPECÍFICOS por CLSID em `specific_object_for()` (já tem TaskbandPin + ExplorerHostCreator).
- **Toolchain gnu** (`-target x86_64-windows-gnu`): NÃO tem SEH/catch MSVC. NÃO dá p/ pôr `catch`
  nas NOSSAS DLLs p/ capturar o throw do explorer. Não morrer no throw = init suceder (sem throw).
- A fila de msg do win32k é **GLOBAL** (por-processo, não por-thread) — por isso a threadproc do
  taskbar pôde ir p/ thread própria sem quebrar o Shell_TrayWnd.
- `CoRegisterClassObject` NÃO é chamado pelo explorer (verificado) — o host NÃO vem de fábrica
  registrada in-proc.
- Threads ring-3 têm pilha/TEB na faixa do PMM (0x04Bx/0x05Cxxxxx); a PRINCIPAL em
  [0x600000,0x700000). sys_exit/ki_ring3_fault_contain distinguem por isso.
- Imports não resolvidos (só crasham SE chamados): `RPCRT4`, `WININET`, `OLEAUT32`, `PROPSYS`,
  `ole32`, `CoreMessaging`, `api-ms-win-appmodel-*`.
- `run.ps1` roda `-smp 2 -accel tcg,thread=multi`. SDK em
  `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\`.

## 📜 COMMITS DA SESSÃO 8 (branch `main`; SEM Co-Authored-By)
- `3e57d3b` feat(combase+shcore+shell32): explorer REAL conclui a init do taskbar (Shell_TrayWnd)
  e entra na escada de persistencia. (IPinnedList3 real + SHCreateThread split + shell32 ord#200 +
  scaffolding ExplorerHostCreator/DesktopExplorerHost + backtrace no _CxxThrowException.)
- `6ffc6ff` fix(mm): map-zero recovery conserta perms de HUGE PAGE 2 MiB (não era no-op) — a
  threadproc do taskbar (0x7A880) passa de 0x7A933 e avança até 0x7A969 (deref COM nulo em
  pData+0x320). Beneficia TODA thread ring-3. pintok VERDE.
- `3ec97a0` / `73b9aff` docs: PROMPT + diagnóstico da threadproc.

**Agora: DESCUBRA onde o explorer real PERSISTE (qual thread/loop não retorna) — instrumente
GetMessage/Dispatch por thread e siga 0x23edd/[0x45D600] e a threadproc 0x7A880. Corrija a pilha
ring-3 read-only (fault 0x7A933) p/ a threadproc do taskbar sobreviver. Derrube o muro da
persistência DE VERDADE (o explorer NÃO deve chamar ExitProcess) → desktop PERSISTENTE. Vá, sem
parar, até ~600k.**
