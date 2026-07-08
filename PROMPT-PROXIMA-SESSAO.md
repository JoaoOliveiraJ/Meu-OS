# PROMPT — próxima sessão: continuar rodando o `explorer.exe` REAL do Windows 10

Cole isto como prompt inicial. É AUTOSSUFICIENTE — comece a trabalhar imediatamente. É LONGO
de propósito: tem TODO o mapa de RE, o estado, as ferramentas e os gotchas.

---

## ⚙️ MODO DE TRABALHO (o mais importante — LEIA ISTO)

**Trabalhe de forma AUTÔNOMA. NÃO pare para me perguntar "continua?".** Rode o loop de
bring-up do explorer real, implementando **um muro por vez, DE VERDADE, POR COMPLETO** (o
usuário foi enfático: **NÃO FAÇA STUB — implemente o que precisa por completo**), sem parar,
até usar ~600k de tokens de contexto. Só então: escreva um novo `PROMPT-PROXIMA-SESSAO.md`
atualizado, faça o commit final, e me dê o balanço. **NUNCA um stub genérico catch-all** — só
stubs ESPECÍFICOS e nomeados, e prefira a implementação REAL ao stub. **Prefira IMPLEMENTAR a
só diagnosticar** — cada sessão deve DERRUBAR muro(s) de verdade e commitar.

**COMENTÁRIOS: escreva comentários GRANDES e MUITO EXPLICATIVOS** no código (em PT-BR) — O QUE
a função faz, POR QUE (a mecânica do NT/Windows), e o CONTEXTO do explorer (qual RVA/função
exercita aquilo). Vale p/ .c do kernel E das DLLs.

**pintok: DESTRAVADO pelo usuário** — pode mexer no kernel (threads/scheduler) à vontade, mas
rode o pintok a CADA mudança de kernel (`.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`)
e NÃO deixe quebrar (regra de ouro — ver seção pintok).

Disciplina: **build → rodar o explorer → diagnosticar → implementar REAL (comentário GRANDE)
→ build → pintok → commit + push**. Builds/QEMU em background (~1 min).
Zig: `tools\zig-windows-x86_64-0.13.0\zig.exe`. **Bash NÃO passa `-Modules build\a.dll,...`**
(o comma-split só ocorre no parse do PowerShell) — USE O CENÁRIO:
`.\run.ps1 -Scenario explorerreal -Headless -TimeoutSec 50`.

## 🎯 A MISSÃO
Rodar o **binário REAL** `C:\Windows\explorer.exe` no MeuOS. NÃO escrever um explorer.
Modelo estilo Wine/ReactOS: implementar as DLLs/mecânicas do NT que o binário real precisa,
uma por uma, diagnóstico-first. Ideal final: desktop PERSISTENTE e PINTADO.

## 📍 ESTADO — MARCOS DA SESSÃO 6 (SHCreateThread real → worker do taskbar; muro = COM de shell/DirectUI)

0. 🎉 **DIAGNÓSTICO DEFINITIVO DA PERSISTÊNCIA.** O explorer roda em **MODE 3 = desktop shell**
   (provado): o `wWinMain` chama o parser de cmdline `sub_0xaa63c`, que devolve modo=3 quando o
   explorer **adquire o mutex do shell** (`CreateMutexW`+`WaitForSingleObject`, sem shell prévio) —
   é o caminho de "eu sou o shell". Estamos NO caminho certo. A persistência real viria de
   `CLSID_ExplorerHostCreator` → `CLSID_DesktopExplorerHost` (o HOST persistente do desktop),
   mas ele é **PULADO** por `rdi==0` no `wWinMain` (branch `0x23ec2: test rdi; je 0x24035`).

1. 🎉 **SHCreateThread (shcore) era um STUB MENTIROSO** (`return 1` sem criar thread nem rodar
   init). Agora é **REAL**: lança a threadproc como **thread ring-3 PREEMPTIVA** (infra da sessão 5,
   via `int 0x80` #8 direto — shcore autocontido) e o callback de init roda na thread nova.
   **RESULTADO:** o explorer agora sobe o **WORKER do taskbar** e roda a init de:
   `Taskband Pin {90AA3A4E}` (itens fixados), `Custom Destination List {77F10CF0}` (jump lists),
   `User Assist {DD313E04}` — init de subsistemas de shell que NUNCA rodava antes (provado por
   thread-attribution: 9 CoCreateInstance na PRINCIPAL, depois ~12 na WORKER).

2. 🎉 **SHPinDllOfCLSID (crash rip=0) DERRUBADO.** Era import não resolvido de
   `api-ms-win-shlwapi-winrt-storage-l1-1-1.dll` (slot=0 → `CALL[0]` → #PF rip=0). Implementei 9
   funções NOMEADAS desse contrato na **shcore** (`SHPinDllOfCLSID`, `StrRetToBufW`/`StrRetToStrW`,
   `PathRemoveArgsW`, `SHIsChildOrSelf`, `IUnknown_GetWindow`, `ShellMessageBoxW`,
   `AssocQueryStringW`, `SHCreateWorkerWindowW` real via syscall) + redirect
   `api-ms-win-shlwapi-` → shcore.dll no `src/ntos/ldr/loader.c`.

3. 🎉 **sys_exit: CONTÉM o FailFast de worker ring-3.** Um worker (thread não-principal, pilha na
   faixa do PMM, fora de `[0x600000,0x700000)`) que chama `ExitProcess` agora **termina só a
   própria thread** (`ki_ring3_thread_exit`), não o processo. Corrige UB (o `longjmp(g_user_exit)`
   era o buffer da PRINCIPAL) E é bring-up-friendly (um FailFast de subsistema de shell incompleto
   não derruba o desktop). Só o `ExitProcess` da PRINCIPAL encerra o processo.

### ⇒ ESTADO ATUAL DE RUNTIME (explorerreal, limpo)
- **Thread PRINCIPAL:** ciclo baseline (`RegisterClass 'Worker Window'` → `CreateWindowEx HWND #1`
  → `CreateThread` da worker 0x72270 → `WaitForSingleObject`→WAIT_OBJECT_0 → `DestroyWindow #1` →
  `ExitProcess da PRINCIPAL`). Encerra `status=0`.
- **Thread WORKER (do SHCreateThread):** roda a init do taskbar CONCORRENTE e **dá FailFast**
  (`ExitProcess`) na init de **DirectUI/COM de shell** — CONTIDA (não derruba o processo).
- SEM crash, SEM hang, SEM UB. pintok VERDE, desktop (shell caseiro) sem regressão.

### 🧭 PRÓXIMA FRONTEIRA — fazer a init do worker CONCLUIR (muro de COM de shell / DirectUI)
A persistência depende de o worker do taskbar **NÃO** dar FailFast: se a init concluir,
`sub_0x87628` tem sucesso → `sub_0x87568` devolve !=0 → **rdi!=0** no `wWinMain (0x23ec2)` → o
explorer cria `CLSID_ExplorerHostCreator` → `CLSID_DesktopExplorerHost` (o host persistente do
desktop) em vez de cair no teardown. É a escada:
- **ALVO do FailFast (capturado por `[sys_exit] worker stack`, ImageBase=base de carga logada):**
  a cadeia de retorno tem **explorer RVA 0x2304AF / 0x3A6CA0** → **`dui70.dll` (DirectUI, ~RVA
  0xD677)** → `kernel32!ExitProcess`. Ou seja: o worker aborta na init de **DirectUI (dui70)** +
  objetos COM de shell (hoje o objeto UNIVERSAL da combase é insuficiente p/ Taskband/jump lists).
  → DESMONTE `dui70.dll` em ~RVA 0xD677 (é outro binário — adapte um disassembler p/ dui70) e o
  explorer em 0x2304AF/0x3A6CA0 p/ achar QUAL método COM/DirectUI checa e aborta. Implemente esse
  objeto/método ESPECÍFICO (não universal).
- **Como reexercitar o FailFast p/ estudar:** temporariamente faça a `SHCreateThread` ESPERAR o
  worker (loop `while(!done) pause` com teto), p/ o worker rodar até o FailFast e cair no
  `[sys_exit] worker stack`. (Na versão commitada a `SHCreateThread` é lança-e-retorna: o worker
  ainda alcança o FailFast durante o `WaitForSingleObject` da principal, mas nem sempre.)
- **Alternativa:** implementar `CLSID_ExplorerHostCreator {AB0B37EC-56F6-4A0E-A8FD-7A8BF7C2DA96}`
  (IID `IExplorerHostCreator {C4DE032A-…}`) como objeto COM ESPECÍFICO na combase, cujo `Create`
  (slot 3) devolve um `CLSID_DesktopExplorerHost {682159D9-…}` cujo `slot 5 (vtbl+0x28)` ou
  `slot 10 (vtbl+0x50)` roda um LOOP DE MENSAGENS bloqueante (nossa win32k tem GetMessage/
  DispatchMessage por syscall) → o `wWinMain` NÃO retorna → explorer PERSISTE. **Mas** isso exige
  primeiro `rdi!=0` (ver acima). O caminho limpo é destravar a init do worker.

## 🗺️ MAPA DE RE DO EXPLORER (RVAs; base do disassembler = 0x140000000; **runtime base = logada
por run** em `[ps] EPROCESS ... base=` e `PEB->ImageBase=`; nas rodadas explorerreal recentes =
0x04319000. **RVA = valor_runtime − base_de_carga**.)
- `wWinMain` = **0x23350**. Parser de modo `sub_0xaa63c` (esi=modo = `[rbp+0x2b4]`; modo 3 = shell,
  via mutex). Branch de modo: `0x236d4: cmp esi,3; je 0x239cd` (bloco do shell).
- **Bloco mode-3** = 0x239cd..0x23e41. No fim: `0x23dd7: call sub_0x87568` → `rdi = eax` (0x23ddc).
  `sub_0x87568` chama `sub_0x87628` (que chama **SHCreateThread**(threadproc=RVA 0x7A880,
  callback=RVA 0x2C5B0, flags=0x282a)) e testa um GLOBAL (RVA 0x424828) que a init preenche.
- **Teardown** (0x23e41..0x24058): `0x23e41 call 0x92598` (Worker Window) → `0x23ec2 test rdi;
  je 0x24035` (rdi==0 PULA o host) → `0x23f31 CoCreateInstance(ExplorerHostCreator)` → métodos
  virtuais (slots 3,3,3,5,10) → `0x2403c ~WorkerWindow` (WaitForSingleObject+DestroyWindow) →
  `0x24058 CoUninitialize` → retorna → ExitProcess.
- **Worker Window:** classe registrada por sub_92780 (wndproc **0x72e00**), criada por sub_92598.
  threadproc = **0x72270** (marshaling; RODA e RETORNA rax=0 — NÃO é loop). `~WorkerWindow` = 0x2fe434.
- **Pump moderno:** o explorer NÃO importa GetMessage/PeekMessage/DispatchMessage. O único import
  de pump é **`CoreMessaging.dll!CreateDispatcherQueueController`** (chamado em RVA 0x145ebf, dentro
  de uma init que só CRIA o DispatcherQueueController THREAD_CURRENT e guarda em this+0x40 — NÃO
  bloqueia ali). `CoreMessaging.dll` NÃO está no cenário (só crasha SE chamado — hoje não é).

### CLSIDs que o explorer pede (via COMBASE_DBG; todos servidos pelo objeto UNIVERSAL hoje)
Identifique qualquer GUID na hora com: `reg query "HKEY_CLASSES_ROOT\CLSID\{GUID}" //ve` e
`...\InProcServer32 //ve` (ESTE Windows tem tudo registrado). Já identificados:
`{0000034B}` IGlobalOptions · `{660B90C8}` Start menu cache (appresolver) · `{30D49246}` Identity
Store (IDStore) · `{C980E4C2}` AppReadiness · `{49A832D7}` BackupSettings (SettingSyncCore) ·
`{90AA3A4E}` **Taskband Pin** (shell32) · `{77F10CF0}` **Custom Destination List / jump lists**
(windows.storage) · `{DD313E04}` User Assist (shell32) · `{AB0B37EC}` **CLSID_ExplorerHostCreator**
(explorerframe) · `{682159D9}` **CLSID_DesktopExplorerHost**.

## 🔩 GATES DE DIAGNÓSTICO (deixe em 0 nos commits)
- **`COMBASE_DBG`** em `dll/win32/combase/combase.c:41` (=1): loga CLSID/IID de todo CoCreateInstance/
  RoGetActivationFactory + a THREAD (MAIN vs WORKER pela faixa da pilha) + faz getters do objeto
  universal preencherem out-params. Rebuild: `.\build.ps1`.
- **`[sys_exit] worker stack`** (`src/ntos/ke/amd64/syscall.c`, sempre ativo): quando um worker
  ring-3 é CONTIDO no ExitProcess, dumpa a cadeia de retorno da pilha do worker (achar o FailFast).
- **`U32_TRACE`** em `dll/win32/user32/user32.c` (loga via int 0x80; ra ring-3 → RVA = ra − base).

## 🔁 O LOOP — comando de run (cenário 'explorerreal' = 15 módulos num token só)
```
.\run.ps1 -Scenario explorerreal -Headless -TimeoutSec 55
```
Ver `build\serial.log`. Marco atual: `SHCreateThread REAL: lanca worker ring-3` → `RegisterClass
'Worker Window'` → (worker: `[sys_exit] ... CONTIDO`) → `ExitProcess da thread PRINCIPAL` →
`processo pid=1 encerrou (status=0x0)`. Regressão: `.\run.ps1 -Scenario desktop -Headless
-TimeoutSec 25` (login→shell caseiro PERSISTE no loop de msg; tem que continuar OK).
(Se `build\explorerreal.exe` sumiu: `cp C:\Windows\explorer.exe build\explorerreal.exe`.)

## 🛠️ FERRAMENTAS (raiz, versionadas)
`disrange.py <rva> <n>` (desmonta N instr do explorer.exe por RVA, nomeia CALL[IAT]) · `disat.py` ·
`callers.py <rva>` (CALL E8 diretos; NÃO acha delay/indiretos) · `dumpimports.py [filtro]` ·
`dumpexports.py` · `whocalls.py <import>` (slot IAT + call-sites `call [rip]`; NÃO pega
`mov rax,[iat];call rax` nem delay) · `elfdis.py <símbolo>` (desmonta símbolo do `build\kernel.elf`) ·
`strxref.py`/`rdstr.py`/`nextwall.py`/`gap.py`. **`ripref.py` está BUGADO** (0 refs). Para achar
refs a um slot IAT, escreva um scan `python3` ad-hoc (funcionou p/ CreateDispatcherQueueController).
**GOTCHA:** confira a aritmética de RVA de `lea [rip±disp]` (errei por 0x1000/0x10000 numa hora —
recompute `rip_após_instr ± disp` com cuidado). Para desmontar OUTRAS DLLs (dui70, shell32,
explorerframe), adapte `disrange.py` trocando `EXE = r'C:\Windows\explorer.exe'` pelo caminho da DLL.

## ⛔ REGRA DE OURO — pintok.sys. NÃO QUEBRAR.
Após CADA incremento no KERNEL (`src/ntos/**`): `.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`.
Dourado: `[P1]/[P2]/[P3] ==== PROVA PASSOU ====`; `intercept totals: CPUID x3 RDTSC x33 RDMSR x0
ANTIVM x0`; `DriverEntry … C0000365`; SEM "Sistema parado". **As threads/containment ring-3 são
pintok-verdes porque `g_ring3_active` fica 0 no cenário pintok (só explorer/desktop ligam) e o
pintok é ring-0 (não chama sys_exit).** DLLs de userland (shcore/combase) NÃO afetam o pintok.
Syscalls novos: **append no FIM** do `s_ssdt[]` (último = #51 sys_thread_exit).

## 📜 COMMITS DA SESSÃO 6 (branch `feat/kernel-foundation-irql-dpc`; push a cada lote)
- `07402b4` feat(shell): SHCreateThread REAL + shlwapi-winrt-storage — worker do taskbar em thread ring-3.
- `4a1e610` feat(kernel+shell): contém FailFast de worker ring-3 (sys_exit) + SHCreateThread honesto.
- `c55c656` diag(kernel): dump da pilha do worker no FailFast — mapeia o muro (dui70/DirectUI + COM de shell).
Mensagens terminam com `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

## 📌 NOTAS / GOTCHAS
- Boot aguenta ~16 módulos; explorerreal usa 15 (ntdll, kernel32, user32, gdi32, advapi32, ucrtbase,
  combase, msvcp_win, shell32, shcore, dxgi, uxtheme, comctl32, dui70, dwmapi + explorerreal.exe).
  Bases (image-base) baixas: ntdll 0xA00000, kernel32 0xC00000, user32 0xE00000, ucrtbase 0x3300000.
  Grandes (combase/shell32/shcore/uxtheme/comctl32/dui70/msvcp_win) carregam por PMM+reloc (base alta).
- Threads ring-3 do CreateThread/SHCreateThread têm pilha/TEB na faixa do PMM (ex.: 0x04Bxxxxx/
  0x05Cxxxxx). A thread PRINCIPAL tem pilha/TEB/PEB em [0x600000,0x700000). É assim que o
  containment do sys_exit distingue worker de principal.
- ⚠️ O `[EXCECAO] vetor=0x0e … expected-trap` no início do serial é PROVA de paginação, NÃO crash.
- `mm_map_user` mapeia em 2 MiB granular e SEM NX → região do worker é user+EXEC.
- Muitos imports ainda NÃO resolvidos (só crasham SE chamados): `RPCRT4` (8 fns), `WININET`,
  `WTSAPI32`, `SHLWAPI.dll` direto (#163 IUnknown_QueryStatus, #164 IUnknown_Exec, #467
  SHRunIndirectRegClientCommand, AssocCreate/AssocQueryKeyW/ChrCmpIW/PathIsDirectoryW/PathIsRelativeW),
  `OLEAUT32`, `PROPSYS`, `ole32`, `CoreMessaging`, `api-ms-win-appmodel-*`. O caminho mais fundo
  (destravar o worker/host) vai exigir vários deles — implemente DE VERDADE, DLL por DLL.
- `run.ps1` roda com `-smp 2 -accel tcg,thread=multi`; SDK em
  `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\`.

**Agora: ataque a init do worker do taskbar que dá FailFast (dui70/DirectUI + COM de shell) —
desmonte o alvo (explorer 0x2304AF/0x3A6CA0 + dui70 ~0xD677), implemente POR COMPLETO o objeto
COM/DirectUI específico que ele exige, e faça a init CONCLUIR → rdi!=0 → DesktopExplorerHost →
persistência. Vá, sem parar, até ~600k.**
