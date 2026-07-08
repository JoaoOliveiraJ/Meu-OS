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

## 🎯 A MISSÃO
Rodar o **binário REAL** `C:\Windows\explorer.exe` no MeuOS (modelo Wine/ReactOS: implementar as
DLLs/mecânicas do NT que o binário exige). Ideal: desktop PERSISTENTE e PINTADO.

## 📍 ESTADO — MARCOS DA SESSÃO 7 (C++ EH x64 REAL + contenção de fault ring-3)

1. 🎉 **C++ EXCEPTION HANDLING x64 REAL** (o muro era: `throw` do explorer = terminate imediato).
   Diagnóstico: o dump antigo do sys_exit lia LIXO da pilha (a sessão 6 mapeou errado p/ dui70 —
   eram valores obsoletos). Reescrevi o dump p/ **simbolizar** (módulo+RVA + checar se é endereço
   de retorno real precedido por CALL). Revelou o alvo REAL: o worker do taskbar dá
   `THROW_IF_FAILED` → `_CxxThrowException` tipo `.?AVResultException@wil@@` (RA=explorer 0x23059E).
   - **ntdll**: implementei a ABI de unwind do NT — `RtlCaptureContext`/`RtlRestoreContext` (naked
     asm, layout CONTEXT exato), `RtlLookupFunctionEntry` (acha RUNTIME_FUNCTION no .pdata),
     `RtlVirtualUnwind` (interpreta UNWIND_CODEs + CHAININFO), `RtlDispatchException` (1ª passada),
     `RtlUnwindEx` (2ª passada), `RtlRaiseException`. Structs CONTEXT/DISPATCHER_CONTEXT/
     EXCEPTION_RECORD com offsets EXATOS do windows.h.
   - **ucrtbase**: `_CxxThrowException` monta o EXCEPTION_RECORD (0xE06D7363, magic 0x19930520,
     obj, ThrowInfo, imagebase) e chama `RtlRaiseException`. Decodifica o tipo lançado.
   - **kernel**: syscall **#52 `sys_module_for_pc`** (RtlLookupFunctionEntry resolve PC→base do
     módulo; fallback p/ a IMAGEM PRINCIPAL — o .exe NÃO fica em s_mods).
   - **PROVADO** (fase de BUSCA): o dispatch anda ntdll→ucrtbase→explorer e **CHAMA os
     `__CxxFrameHandler3` ESTÁTICOS do próprio explorer** (o explorer traz o seu; nós só damos os
     primitivos Rtl*). O explorer é quem casa tipo/faz catch.
   - ⚠️ A fase de **UNWIND/CATCH (`RtlUnwindEx`)** está implementada mas **NÃO EXERCITADA**: este
     throw específico NÃO casa nenhum catch na pilha (real Windows NUNCA lança aqui — só lança pq
     nossa combase devolve E_NOTIMPL onde o objeto real devolveria S_OK; ninguém escreveu catch p/
     um erro que não acontece). Quando algum throw REALMENTE for capturado, valide o RtlUnwindEx.

2. 🎉 **FAULT DE RING-3 NÃO HALTA MAIS O OS** (kernel). Um #PF/#GP/#UD irrecuperável num processo
   de usuário fazia "Sistema parado" (derrubava o KERNEL). Agora `ki_ring3_fault_contain(rsp)`
   (isr.c chama no lugar do hlt p/ fault de CPL=3): thread WORKER → mata só a thread; PRINCIPAL →
   encerra o processo. O OS sobrevive. (Mesma política do sys_exit, mas p/ faults de HW.)

3. **SHCreateThread agora SÍNCRONO** (shcore): espera (join) o worker terminar antes de retornar.
   Fidelidade (o Windows bloqueia até a init) + determinismo (fim da corrida principal-vs-worker).

### ⇒ RUNTIME ATUAL (explorerreal, limpo)
Worker do taskbar lança → callback dá `throw wil::ResultException` (slot 3 E_NOTIMPL) → C++ EH
faz a busca (nenhum catch casa) → unhandled → `ExitProcess(3)` → **sys_exit CONTÉM** (mata só a
worker) → a PRINCIPAL segue (Worker Window → threadproc → ExitProcess) → `status=0`. SEM crash,
SEM hang, SEM "Sistema parado". pintok VERDE. Desktop caseiro (`-Scenario desktop`) sem regressão.

## 🧭 PRÓXIMA FRONTEIRA — persistência (CADEIA DE DEPENDÊNCIA TOTALMENTE MAPEADA)
O `wWinMain` (mode 3) em **0x23dd7** chama `sub_0x87568` → **rdi**. **0x23ec2** `test rdi; je
0x24035` (rdi==0 → TEARDOWN → ExitProcess). Para PERSISTIR precisa **rdi != 0**, e:

- `sub_0x87568` devolve !=0 SÓ SE: **`sub_0x87628` != 0** E **GLOBAL_B (`[rip+0x3ac3e0]`) == 0**,
  e ENTÃO **`SHELL32!ord#200` != 0** (esse retorno vira rbx→rdi; a chamada é em 0x875cd).
- `sub_0x87628` (0x87628) devolve !=0 SÓ SE `SHCreateThread` != 0 (ok, devolve 1) **E
  GLOBAL_C (`[rip+0x3ad16c]`) != 0**. **GLOBAL_C é setado pela INIT DO CALLBACK do worker.**
- ⇒ **O CRUX**: o callback do worker (RVA **0x2C5B0** → corpo real ~**0x2C828**, tem
  `__CxxFrameHandler3`) precisa **CONCLUIR a init** (setar GLOBAL_C). Hoje ele LANÇA no
  `CoCreateInstance(Taskband Pin {90AA3A4E}, iid {0DD79AE2})` → **slot 3 devolve E_NOTIMPL** →
  `THROW_IF_FAILED` → throw. Init não conclui → GLOBAL_C=0 → sub_0x87628=0 → rdi=0 → teardown.

**⇒ Muro a derrubar (session 8): fazer a init do callback do worker CONCLUIR de VERDADE** —
implementar os objetos COM de shell que o callback cria/usa, com métodos que devolvem S_OK e
out-params VÁLIDOS (não E_NOTIMPL, não lixo). Ordem observada de `CoCreateInstance` no worker
(com COMBASE_DBG=1): `{660B90C8}` appresolver · `{90AA3A4E}` **Taskband Pin** (1º throw: slot 3) ·
`{77F10CF0}` jump lists · `{DD313E04}` User Assist · `{F0AE1542}` · etc. **Comece pelo Taskband
Pin slot 3** (desmonte o corpo do callback ~0x2C828 no explorer p/ ver o que ele faz com o
retorno de cada método — implemente o objeto/método ESPECÍFICO, não catch-all).

Cuidado (aprendido na s7): o objeto UNIVERSAL da combase devolvendo S_OK+preenche-com-ele-mesmo
em TODOS os slots é o **catch-all proibido** — empurra mais fundo mas depois o explorer derefa um
FIELD (não método) do objeto e dá #PF (cr2=0x10, explorer 0x29F5B2). O jeito REAL é objeto com
DADOS válidos, por CLSID/interface.

**Depois de GLOBAL_C != 0** (rdi vira o retorno de `shell32!ord#200`): implemente **`shell32
ord#200`** (real: aloca/constrói e devolve objeto NÃO-NULO; hoje é `shell_ord_stub`→0, ver
`shell32.def` `_ord200`) e a escada final que o `wWinMain` roda quando rdi!=0 (JÁ DESMONTADA,
0x23ec2..0x24004): `CoCreateInstance(CLSID_ExplorerHostCreator {AB0B37EC})` → **slot 3**
(vtbl+0x18, "Create") preenche um `DesktopExplorerHost {682159D9}` em `[rsp+0x58]` → **slot 5
(vtbl+0x28)** e **slot 10 (vtbl+0x50)** do DesktopExplorerHost — **UM deles tem que rodar um LOOP
DE MENSAGENS BLOQUEANTE** (nossa win32k tem GetMessage/DispatchMessage por syscall; o desktop
caseiro já persiste assim) → o `wWinMain` NÃO retorna → **explorer PERSISTE**. Implemente
ExplorerHostCreator + DesktopExplorerHost como objetos COM ESPECÍFICOS na combase (por CLSID),
não pelo objeto universal.

## 🗺️ MAPA DE RE (base disassembler = 0x140000000; runtime base logada por run;
`explorerreal` recente = **0x04319000**; RVA = runtime − base). ⚠️ **GOTCHA: aritmética de
`lea [rip±disp]`** — recompute `rip_APÓS_a_instr ± disp` com cuidado (errei 2x nesta sessão).
- `wWinMain`=0x23350. Parser de modo `sub_0xaa63c` (modo 3 = shell, via mutex).
- `sub_0x87568` (→rdi): testa GLOBAL_A `[rip+0x3ac3fa]`, chama `sub_0x87628`, testa GLOBAL_B
  `[rip+0x3ac3e0]`, chama `SHELL32!ord#200` em **0x875cd** (rbx→rdi).
- `sub_0x87628`: chama **SHCreateThread**(threadproc=RVA **0x7A880**, pData=**0x434820**,
  flags, callback=RVA **0x2C5B0**) e testa GLOBAL_C `[rip+0x3ad16c]` (setado pela init).
- callback **0x2C5B0** (thunk → corpo ~0x2C828, tem `__CxxFrameHandler3`): init pesada do shell.
- threadproc **0x7A880** (tem try/catch): trabalho principal (roda DEPOIS do callback).
- Teardown/host: **0x23ec2** `test rdi; je 0x24035` · **0x23f31** `CoCreateInstance(ExplorerHost
  Creator)` · slot3 (0x23f56) · slot5 (0x23fec, vtbl+0x28) · slot10 (0x23ffe, vtbl+0x50) ·
  **0x24035** `~WorkerWindow` (0x2fe434) → CoUninitialize → ExitProcess.
- `SHELL32!ord#200` (real shell32 RVA 0xa8380): aloca 0x2c0 bytes, constrói, devolve ponteiro.
- CLSIDs (COMBASE_DBG=1): `{0000034B}` IGlobalOptions · `{660B90C8}` appresolver · `{30D49246}`
  IDStore · `{C980E4C2}` AppReadiness · `{49A832D7}` SettingSyncCore · `{90AA3A4E}` Taskband Pin ·
  `{77F10CF0}` jump lists · `{DD313E04}` User Assist · `{AB0B37EC}` CLSID_ExplorerHostCreator ·
  `{682159D9}` CLSID_DesktopExplorerHost. Identifique GUID: `reg query "HKCR\CLSID\{GUID}" //ve`.

## 🔩 GATES DE DIAGNÓSTICO (deixe em 0 nos commits)
- **`EH_DBG`** em `dll/ntdll/ntdll.c` (=1): loga cada frame do dispatch (rip/base/handler) — usa
  p/ ver o unwind andar. Rebuild: `.\build.ps1`.
- **`COMBASE_DBG`** em `dll/win32/combase/combase.c:41` (=1): loga CLSID/IID de cada
  CoCreateInstance/RoGetActivationFactory + a THREAD (MAIN vs WORKER) + slots do obj universal.
- **ucrtbase `_CxxThrowException`** (sempre ativo, baixo ruído): loga o TIPO C++ lançado + RA.
- **`[sys_exit] worker call-chain SIMBOLIZADA`** (`src/ntos/ke/amd64/syscall.c`, sempre ativo):
  cadeia de retorno módulo+RVA quando um worker é contido.

## 🔁 O LOOP
`.\run.ps1 -Scenario explorerreal -Headless -TimeoutSec 55` (15 módulos num token só). Ver
`build\serial.log`. Regressão: `.\run.ps1 -Scenario desktop -Headless -TimeoutSec 25` (shell
caseiro tem que persistir no loop de msg). pintok: `.\run.ps1 -Scenario pintok -Headless
-TimeoutSec 40` (dourado: P1/P2/P3 PASSOU, `intercept totals ... ANTIVM x0`, DriverEntry C0000365).

## 🛠️ FERRAMENTAS (raiz, versionadas)
`disrange.py <rva> <n>` (desmonta explorer.exe por RVA, nomeia CALL[IAT]) · **`disdll.py <pe>
<rva> [n]`** (idem p/ QUALQUER PE — dui70/shell32/ntdll/nossos build\*.dll) · **`pdata.py <pe>
<rva>`** (mostra EHANDLER/UHANDLER + handler + FuncInfo RVA de uma função) · **`funcinfo.py <pe>
<rva>`** (decodifica try-blocks + tipos de catch — SÓ __CxxFrameHandler3 v3; explorer usa v4 em
algumas funções → lixo ali) · `callers.py` (só E8 diretos) · `dumpimports.py`/`dumpexports.py` ·
`whocalls.py`. **`ripref.py` BUGADO.** Para outras DLLs use o disdll.py.

## ⛔ REGRA DE OURO — pintok.sys. NÃO QUEBRAR.
Após CADA incremento no KERNEL: `.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`. As
mudanças de EH/contenção são **pintok-verdes** porque tudo é ring-3 (o pintok é ring-0, CPL=0,
`g_ring3_active=0` no cenário pintok, nunca entra em sys_exit/ki_ring3_fault_contain). DLLs de
userland (ntdll/ucrtbase/shcore/combase/shell32) NÃO afetam o pintok. Syscalls novos: append no
FIM do `s_ssdt[]` (último = **#52 sys_module_for_pc**).

## 📌 NOTAS / GOTCHAS
- **Toolchain gnu** (`-target x86_64-windows-gnu`): NÃO tem SEH `__try/__except` nem C++ try/catch
  compatível com MSVC. Ou seja, NÃO dá p/ pôr um `catch` nas NOSSAS DLLs (shcore etc.) p/ capturar
  o throw do explorer. O explorer traz o PRÓPRIO `__CxxFrameHandler3`/funclets — nós só damos os
  primitivos Rtl* da ntdll. O jeito de "não morrer" no throw é (a) a init suceder (sem throw), ou
  (b) a contenção (sys_exit/fault) — ambos já existem.
- ucrtbase agora linka `libntdll.a` (p/ `RtlRaiseException`) — ver build.ps1 (~linha 430).
- Threads ring-3 (CreateThread/SHCreateThread) têm pilha/TEB na faixa do PMM (0x04B/0x05Cxxxxx); a
  PRINCIPAL em [0x600000,0x700000). É assim que sys_exit e ki_ring3_fault_contain distinguem.
- O objeto UNIVERSAL da combase (slots 6-63 = univ_fill S_OK) é SCAFFOLD; NÃO estenda p/ slots 3-5
  (vira catch-all proibido e leva a #PF por deref de field, não de método).
- Imports ainda não resolvidos (só crasham SE chamados): `RPCRT4`, `WININET`, `OLEAUT32`,
  `PROPSYS`, `ole32`, `CoreMessaging`, `api-ms-win-appmodel-*`.
- `run.ps1` roda `-smp 2 -accel tcg,thread=multi`. SDK em
  `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\`.

## 📜 COMMITS DA SESSÃO 7 (branch `main`; SEM Co-Authored-By)
- `6ecfce2` feat(ntdll+kernel+shell): C++ EH x64 REAL — o throw do explorer não mata mais o processo.
- `a6bad5a` feat(kernel): fault de RING-3 não halta mais o OS — contenção por thread/processo.

**Agora: derrube o CRUX — faça a init do callback do worker (0x2C5B0/0x2C828) CONCLUIR
implementando os objetos COM de shell REAIS (comece pelo Taskband Pin {90AA3A4E} slot 3), setando
GLOBAL_C `[rip+0x3ad16c]` → rdi!=0 → escada ExplorerHostCreator/DesktopExplorerHost com LOOP DE
MENSAGENS → explorer PERSISTE. Vá, sem parar, até ~600k.**
