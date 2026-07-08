# PROMPT — próxima sessão: continuar rodando o `explorer.exe` REAL do Windows 10

Cole isto como prompt inicial. É AUTOSSUFICIENTE — comece a trabalhar imediatamente. É LONGO
de propósito: tem TODO o mapa de RE, o estado das threads, as ferramentas e os gotchas.

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
(o bash come as barras e o PowerShell não faz o comma-split via `-File`) — USE O CENÁRIO:
`.\run.ps1 -Scenario explorerreal -Headless -TimeoutSec 50` (adicionado ao run.ps1 nesta sessão).

## 🎯 A MISSÃO
Rodar o **binário REAL** `C:\Windows\explorer.exe` no MeuOS. NÃO escrever um explorer.
Modelo estilo Wine/ReactOS: implementar as DLLs/mecânicas do NT que o binário real precisa,
uma por uma, diagnóstico-first. Ideal final: desktop PERSISTENTE e PINTADO.

## 📍 ESTADO — MARCOS DA SESSÃO 5 (threads ring-3 PREEMPTIVAS)
0. 🎉🎉 **THREADS RING-3 PREEMPTIVAS DE VERDADE (commit `4485e28`).** Substituí o modelo
   COOPERATIVO da sessão 4 (que rodava a threadproc só no WaitForSingleObject, congelando a
   thread principal) por threads ring-3 PREEMPTIVAS: **cada CreateThread do explorer vira um
   KTHREAD próprio que roda a threadproc em RING 3, escalonado pelo timer LADO A LADO com a
   thread principal.** É o que o explorer real exige.
1. 🎉 **DESCOBERTA que corrige o diagnóstico da sessão 4:** a threadproc da Worker Window
   (RVA **0x72270**, runtime 0x0438B270) **NÃO trava num import não resolvido**. Ela é uma
   rotina de **marshaling/serialização** que **RODA e RETORNA com rax=0 (STATUS_SUCCESS)**.
   O `[EXCECAO] rip=0` da sessão 4 era o `ret` final pulando p/ [stack_top-8]=0 (nunca puséramos
   endereço de retorno na pilha do worker). Provado pelo dump dos regs no #PF: rbp/r12..r15 no
   fault == valores de ENTRADA restaurados por um epílogo `pop`+`ret`.
2. 🎉 **O explorer roda o CICLO DE VIDA COMPLETO, correto, e ENCERRA (status=0):**
   `RegisterClass 'Worker Window'` → `CreateWindowEx HWND #1` → `CreateThread` → **thread ring-3
   preemptiva roda CONCORRENTE e retorna rax=0** → `WaitForSingleObject`→WAIT_OBJECT_0 →
   `DestroyWindow #1` → `CoUninitialize` → `processo pid=1 encerrou (status=0x0)`. SEM crash,
   SEM halt. (Regressão do cenário `desktop` OK; pintok VERDE.)

### ⇒ POR QUE O EXPLORER ENCERRA (o muro ATUAL, e é GRANDE)
O `wWinMain` (RVA **0x23350**) **faz init pesado (COM) e RETORNA** — e o CRT chama ExitProcess.
NÃO há loop de mensagem persistente no caminho que nosso ambiente dispara. A persistência do
shell real depende de **objetos COM de shell** que hoje são **stubs** (o `combase` devolve um
"objeto universal" cujos métodos são no-op/E_NOTIMPL). Detalhe do fim do wWinMain (disassemblado):
- `0x23e41 call 0x92598` — cria a Worker Window + thread (sub_92598).
- `0x23ec2 test rdi; je 0x24035` — se rdi==0 pula DIRETO p/ o teardown.
- `0x23f31 CoCreateInstance` (CLSID em rva 0x3BB460) → chama métodos virtuais (slots 3,3,3,5,10)
  no objeto → `Release`. **Nenhum é um "Run" que bloqueia** — usa o objeto e solta.
- `0x2403c call 0x2fe434 = ~WorkerWindow` → `WaitForSingleObject(thread)` + `DestroyWindow #1`.
- `0x24058 CoUninitialize` → retorna → ExitProcess.

**Prova empírica (COMBASE_DBG=1, agora OFF):** o explorer pede **~40 objetos COM**, todos servidos
pelo stub universal, e ENCERRA igual. CLSIDs pedidos incluem:
- `{0000034B-…}`/iid `{0000015B-…}` = **IGlobalOptions** (setup COM padrão).
- WinRT `Windows.ApplicationModel.Resources.Core.ResourceManager` (**MRT** — resources/ícones/strings).
- `{30D49246-D217-465F-B00B-AC9DDD652EB7}`, `{C980E4C2-C178-4572-935D-A8A429884806}` (pedido 4x!),
  `{49A832D7-C7B3-4556-8716-5BDC745C3C1B}`, `{660B90C8-73A9-4B58-8CAE-355B7F55341B}`.

### 🧭 PRÓXIMA FRONTEIRA — fazer o explorer PERSISTIR/PINTAR (shell COM stack)
Isto é grande e provavelmente multi-sessão. Opções (escolha e implemente POR COMPLETO):
1. **Identificar e implementar o(s) objeto(s) COM de shell** que o wWinMain usa (o `{C980E4C2…}`
   pedido 4x é o suspeito nº1). Se um deles tiver um método "Run"/loop que em Windows real
   BLOQUEIA (roda o pump do shell até shutdown), implementá-lo p/ realmente rodar um loop de
   mensagens faz o wWinMain NÃO retornar → explorer PERSISTE. Descubra a interface: ache a
   CoCreateInstance (0x23f31) e desmonte os métodos virtuais chamados (slots 5=vtbl+0x28,
   10=vtbl+0x50 no objeto [rsp+0x58]). Ligue COMBASE_DBG=1 (dll/win32/combase/combase.c:41) p/
   ver CLSID/IID em runtime.
2. **Investigar se o explorer deveria entrar num MODO diferente** (shell/desktop completo) que
   NÃO cai no teardown. Desmonte a decisão de modo no início do wWinMain (0x23350) e o ramo p/
   `0x239cd` (prompt da sessão 3 chamava de "modo-3"). Veja o que seleciona o caminho persistente.
3. **DLLs estáticas não resolvidas** que o shell completo vai exigir (hoje slot=0 → crasham SE
   chamadas; o caminho atual NÃO as chama, mas um caminho mais fundo vai): `SHLWAPI` (paths/strings
   — bem definido, implementável POR COMPLETO), `RPCRT4` (8 fns — o shell usa RPC), `PROPSYS`,
   `OLEAUT32`, `urlmon`, `WININET`, `WTSAPI32`, `CoreMessaging` (o pump moderno!), `USERENV`,
   `IPHLPAPI`, `SspiCli`, `AEPIC`, `TWINAPI`. **O explorer NÃO importa GetMessage/PeekMessage/
   DispatchMessage** — o pump dele é `MsgWaitForMultipleObjectsEx` + provavelmente **CoreMessaging**.

## ⚙️ COMO AS THREADS RING-3 PREEMPTIVAS FUNCIONAM (implementado na sessão 5)
Só o **CPU 0** roda ring 3 (TSS único). Gate global `g_ring3_active` (sched.c) liga a lógica; no
cenário pintok fica 0 → `ki_quantum_end` idêntico ao baseline (regra de ouro respeitada).
- `ki_thread_t` (sched.h) ganhou: `is_ring3, user_teb, kstack_top, user_start, user_param,
  user_stack_top`.
- **`ki_quantum_end` (sched.c):** ao escalonar (só CPU0 + g_ring3_active), programa
  `tss_set_rsp0(next->kstack_top)` (cada thread ring-3 trapa na SUA pilha de kernel) e
  `gs.base = next->is_ring3 ? next->user_teb : KPCR` (cada uma com seu TEB / KPCR).
- **`ki_launch_ring3_thread(start,param)` (usermode.c):** aloca pilha de usuário 256 KiB + TEB +
  **stub de retorno** (`usermode_alloc_ring3_stack`), cria um KTHREAD (pilha de kernel 16 KiB via
  `ki_create_thread`) cujo entry de kernel é `ring3_trampoline` → `enter_ring3_arg` (IRETQ p/ ring 3).
  Afinidade CPU 0. `sys_createthread` chama isto e guarda o par (handle, KTHREAD).
- **`usermode_enter` marca a thread principal ring-3** (`ki_mark_current_ring3(TEB_ADDR)`) — ela
  roda sobre a idle/boot; assim o scheduler restaura gs.base=TEB e TSS.rsp0=pilha do boot p/ ela.
- **Stub de retorno:** a threadproc entra via IRETQ (sem `call` → sem endereço de retorno). Pomos
  em `[stack_top-8]` o endereço de um stub ring-3 (`mov rdi,rax; mov eax,51; int 0x80; jmp $`).
  Quando a threadproc RETORNA, cai no stub → syscall **#51 `sys_thread_exit`** → `ki_ring3_thread_exit`
  (marca TERMINATED, acorda waiters, cede p/ sempre).
- **`sys_waitforsingleobject`:** se o handle é de uma thread ring-3 lançada, ESPERA
  COOPERATIVO (a idle/boot NÃO pode bloquear no dispatcher — `ki_can_block`=false): `while
  (!ki_thread_is_terminated(kt)) ki_yield_processor();`. Se a thread rodasse um pump INFINITO,
  a principal cederia p/ sempre → o processo PERSISTIRIA. Como a 0x72270 só faz init e retorna, o
  wait devolve e a principal segue (→ teardown → exit). **Semântica correta do WaitForSingleObject.**
- Limitação/evolução: threadpool (`SubmitThreadpoolWork`) roda o callback SÍNCRONO inline (não é
  stub — realmente executa; kernel32.c:664). Se algum dia o pump do shell for um callback de
  threadpool/CoreMessaging, dá p/ torná-los threads ring-3 reais reusando `ki_launch_ring3_thread`
  (precisaria passar rdx/r8 além de rcx — hoje só rcx=param).

## 🗺️ MAPA DE RE DO EXPLORER (RVAs; base do disassembler = 0x140000000; runtime base = 0x04319000)
- `wWinMain` = **0x23350**. Fim do wWinMain (teardown) = 0x23e41..0x24058 (ver "POR QUE ENCERRA").
- **Worker Window:** classe registrada em sub_92780 (wndproc **0x72e00**, className "Worker Window").
  Criada por sub_92598 (@0x23e41). wndproc em WM_NCCREATE faz SetWindowLongPtrW(-21, obj).
- **threadproc da Worker Window = 0x72270** (o CreateThread real). É MARSHALING: HeapAlloc(0xa0/8/0xc4),
  serializa struct com tags de tipo (8,0xc,0xa0) + checks de overflow (erros 0xC0000095/0x23/0x17/0x0d),
  epílogo comum em **0x20e00b** (libera buffers), RETORNA rax=0. NÃO é loop de mensagem.
- `~WorkerWindow` = **0x2fe434** (completo)/**0x2fe3c4** (parcial). DestroyWindow em 0x2fe526.

## 🔁 O LOOP — comando de run (cenário 'explorerreal' = 16 módulos num token só)
```
.\run.ps1 -Scenario explorerreal -Headless -TimeoutSec 50
```
Ver `build\serial.log`. Marco atual: `thread ring-3 preemptiva LANCADA` → `RETORNOU (rax=0)` →
`WaitForSingleObject … WAIT_OBJECT_0` → `DestroyWindow #1` → `processo pid=1 encerrou`.
Regressão: `.\run.ps1 -Scenario desktop -Headless -TimeoutSec 25` (login→shell caseiro, tem que continuar OK).
(Se `build\explorerreal.exe` sumiu: `cp C:\Windows\explorer.exe build\explorerreal.exe`.)

## 🛠️ FERRAMENTAS (raiz, versionadas)
`disrange.py <rva> <n>` (desmonta N instr do RVA, nomeia CALL[IAT]) · `disat.py <hex>` ·
`callers.py <rva>` (CALL E8 diretos p/ o rva; NÃO acha delay imports) · `strxref.py` · `rdstr.py` ·
`dumpimports.py [filtro]` · `dumpexports.py` · `nextwall.py` · `gap.py`/`redirgap.py`.
**Recriadas na sessão 5:** `whocalls.py <import>` (slot IAT + call-sites `call [rip]`; NÃO pega
delay imports — por isso não achou WaitForSingleObject/MsgWait) · `elfdis.py <símbolo>` (desmonta
símbolo do `build\kernel.elf` — foi como provei que `usermode_run_worker` estava correto) ·
`ripref.py <rva>` (LEA/MOV [rip]→rva — **está BUGADO, acha 0 refs mesmo p/ refs reais; conserte
ou não confie**).

## 🔩 DIAGNÓSTICO — GATES (deixe todos em 0 nos commits)
- **`COMBASE_DBG`** em `dll/win32/combase/combase.c:41` (=1): loga CLSID/IID de todo CoCreateInstance/
  RoGetActivationFactory + faz getters do objeto universal devolverem objetos. **Foi como listei os
  ~40 objetos COM que o explorer pede.** Rebuild: `.\build.ps1`.
- **`U32_TRACE`** em `dll/win32/user32/user32.c` (loga via int 0x80; ra ring-3 → RVA = ra − 0x04319000).
- `GPA_TRACE`/`REG_TRACE` em `src/ntos/ke/amd64/syscall.c`.

## ⛔ REGRA DE OURO — pintok.sys. NÃO QUEBRAR.
Após CADA incremento no KERNEL: `.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`.
Dourado: `[P1]/[P2]/[P3] ==== PROVA PASSOU ====`; `intercept totals: CPUID x3 RDTSC x33 RDMSR x0
ANTIVM x0`; `DriverEntry … C0000365`; SEM "Sistema parado". **As threads ring-3 são pintok-verdes
porque `g_ring3_active` fica 0 no cenário pintok (só o explorer/desktop ligam).** DLLs de userland
NÃO afetam o pintok. Syscalls novos: **append no FIM** do enum `s_ssdt[]` (último = #51 sys_thread_exit).

## 📜 COMMITS DA SESSÃO 5 (branch `feat/kernel-foundation-irql-dpc`; push a cada lote)
- `c9a9501` feat(kernel): worker threadproc RETORNA limpo (rax=0) — stub de retorno + park; fim do HALT rip=0.
- `4485e28` feat(kernel): **THREADS RING-3 PREEMPTIVAS** — CreateThread → KTHREAD ring-3 concorrente.
Mensagens terminam com `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`.

## 📌 NOTAS / GOTCHAS
- Boot aguenta ~16 módulos. Bases (image-base): ntdll 0xA00000, kernel32 0xC00000, user32 0xE00000,
  gdi32 0x1A00000, advapi32 0x3200000, ucrtbase 0x3300000, dxgi 0x3F00000. Grandes (combase/shell32/
  shcore/uxtheme/comctl32/dui70/msvcp_win) carregam por PMM+reloc (base preferida alta).
- `mm_map_user` mapeia em **2 MiB granular** e SEM NX → a região do worker vira user+EXEC (por isso
  dá p/ pôr o stub de retorno de ring-3 lá, e por isso escritas logo acima do stack_top não faltam).
- ⚠️ O `[EXCECAO] vetor=0x0e … expected-trap` no início do serial é PROVA de paginação, NÃO crash.
- SDK do Windows: `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\`.
- `run.ps1` roda com `-smp 2 -accel tcg,thread=multi`; TEB/PEB da principal em [0x600000,0x601fff].

**Agora: ataque a PERSISTÊNCIA do shell (o muro atual) — implemente POR COMPLETO o(s) objeto(s) COM
de shell / o pump que faz o wWinMain NÃO retornar, OU descubra o MODO shell que evita o teardown.
Threads ring-3 já são preemptivas e corretas. Vá, sem parar, até ~600k.**
