# PROMPT — próxima sessão: continuar rodando o `explorer.exe` REAL do Windows 10

Cole isto como prompt inicial. É AUTOSSUFICIENTE — comece a trabalhar imediatamente. É LONGO
de propósito: tem TODO o mapa de RE, o plano de threads, as ferramentas e os gotchas.

---

## ⚙️ MODO DE TRABALHO (o mais importante — LEIA ISTO)

**Trabalhe de forma AUTÔNOMA. NÃO pare para me perguntar "continua?".** Rode o loop de
bring-up do explorer real, implementando **um muro por vez, de verdade**, sem parar, até
usar ~600k de tokens de contexto. Só então: escreva um novo `PROMPT-PROXIMA-SESSAO.md`
atualizado, faça o commit final, e me dê o balanço. Antes disso, siga implementando e
commitando sozinho. **NUNCA um stub genérico catch-all** — só stubs ESPECÍFICOS e nomeados.
**Prefira IMPLEMENTAR a só diagnosticar** — o diagnóstico é meio, não fim; cada sessão deve
DERRUBAR muro(s) de verdade e commitar. Rode `pintok` a cada mudança de kernel.

**COMENTÁRIOS: escreva comentários GRANDES e MUITO EXPLICATIVOS** no código (em PT-BR), como já
é o estilo do projeto — explique O QUE a função faz, POR QUE (a mecânica do NT/Windows por trás),
e o CONTEXTO do explorer real (qual RVA/função do explorer exercita aquilo). Cada stub/ordinal/
função nova leva um bloco de comentário dizendo de onde o explorer chama e por que o retorno é
aquele. Nada de comentário de 1 linha seco — o próximo eu (e o usuário) precisam entender tudo
sem reabrir o diagnóstico. Vale para .c do kernel E das DLLs.

**pintok: DESTRAVADO pelo usuário nesta sessão** — não deixe o pintok te BLOQUEAR. Pode mexer no
kernel (threads/scheduler) à vontade; se precisar mexer no intercept de CPUID/RDTSC, deixe
comentado e reative quando for testar o pintok. Ainda assim, rode o pintok de vez em quando p/
saber onde está (`.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`), mas NÃO pare por ele.

Disciplina: **build → rodar o explorer → diagnosticar → implementar REAL (com comentário GRANDE)
→ build → commit + push**. Builds/QEMU em background (~1 min).
Zig: `tools\zig-windows-x86_64-0.13.0\zig.exe`. **PowerShell**: aspas nos args `-Wl,...`
(a vírgula vira operador de array): `& $zig cc ... "-Wl,--image-base=0x5900000" ...`.
Rebuild rápido de UMA dll (ex.: user32, que importa do ntdll — **inclui o .def dos ordinais!**):
```
& $zig cc -target x86_64-windows-gnu -shared -nostdlib -e DllMain "-Wl,--image-base=0xE00000" "-Wl,--out-implib,build\libuser32.a" -o build\user32.dll dll\win32\user32\user32.c dll\win32\user32\user32.def build\libntdll.a
```
Rebuild do KERNEL inteiro (quando mexer em `src/ntos/...`): `.\build.ps1` (gera `build\kernel.bin`;
~1 min). Bases das DLLs (image-base): ntdll 0xA00000, kernel32 0xC00000, user32 0xE00000,
gdi32 0x1A00000, advapi32 0x3200000, ucrtbase 0x3300000, dxgi 0x3F00000. As grandes (combase/
shell32/shcore/uxtheme/comctl32/dui70/msvcp_win) carregam por PMM+reloc (base preferida alta).

## 🎯 A MISSÃO
Rodar o **binário REAL** `C:\Windows\explorer.exe` no MeuOS. NÃO escrever um explorer.
Detalhes: `RECON-EXPLORER.md` + memória `run-real-explorer-mission`. Modelo estilo Wine/ReactOS:
implementar as DLLs/mecânicas do NT que o binário real precisa, uma por uma, diagnóstico-first.

## 📍 ESTADO — MARCOS DESTA SESSÃO (sessão 4)
0. 🎉🎉 **O explorer real PERSISTE (NÃO SAI MAIS) — implementei THREADS RING-3 cooperativas
   (commit `d591fab`).** A thread da Worker Window ENTRA EM RING 3 e roda código real; o
   `wWinMain` fica preso no `WaitForSingleObject(worker)` do `~WorkerWindow` → NÃO chega ao
   `DestroyWindow`/`ExitProcess`. Serial: `[thread] worker ring-3: start=... teb=...`, SEM
   `DestroyWindow #1`, SEM `processo encerrou`. → ver "PRÓXIMA FRONTEIRA" (muro dentro da threadproc).
1. 🎉 **O explorer CRIA sua Worker Window (HWND #1).** Serial:
   `[win32k] RegisterClass 'Worker Window'` → `[win32k] CreateWindowEx -> HWND #1 classe='Worker Window'`.
2. 🎉 **A Worker Window associa seu objeto C++ (GWLP_USERDATA) e roda o handler real** de
   `WM_USER+7` SEM crashar (via WM_NCCREATE inline + SetWindowLongPtrW/GetWindowLongPtrW reais).
Base do explorer em runtime: **0x04319000** (ImageBase preferida 0x140000000; RVA = runtime − 0x04319000).

### O que era o 1º muro (RESOLVIDO) — NÃO era ATOM, nem COM, nem DWM
O `wWinMain` chamava **`CreateWindowInBand`** (import PRIVADO `api-ms-win-rtcore-ntuser-private!
CreateWindowInBand`, redirecionado p/ user32), que era **stub → 0**; vendo `hwnd==NULL`
(`test rax,rax; je 0x2290e0`), o explorer DESISTIA. Fix (tudo em **user32, userland → pintok intacto**):
- `CreateWindowInBand`/`CreateWindowInBandEx` **criam a janela de verdade** (mesmo caminho do `CreateWindowExW`).
- **ATOM único**: `RegisterClass*` devolve `0xC000+slot` (era `1` fixo) e `class_ref_to_name_w/a`
  resolve atom→nome de classe em TODO `CreateWindow*` (antes derefava o atom pequeno como string → PF).
- **`user32.def`** exporta os 6 ordinais PRIVADOS noname que o explorer importa POR ORDINAL:
  `#2005/#2521/#2522/#2573/#2574/#2611`. `#2522(hwnd)` era slot=0 e **crashava (rip=0)** logo após
  criar a janela. `.def` é **aditivo** aos `__declspec` (padrão do `uxtheme.def`; `build.ps1` já passa `$user32Def`).

### O que era o 2º "muro" (RESOLVIDO, prereq p/ threads)
`Set/GetWindowLongPtrW` eram no-op → o wndproc real da Worker Window (RVA 0x72e00) NUNCA achava
seu objeto (cai em DefWindowProc). Fix (user32): armazenamento REAL de Window Long Ptr por
`(hwnd,idx)` + **enviar `WM_NCCREATE` INLINE no `create_window_impl`** (com CREATESTRUCTW: o
wndproc lê `lpCreateParams` no offset 0 e faz `SetWindowLongPtrW(hwnd,-21,obj)`). DefWindowProc*
agora devolve TRUE p/ WM_NCCREATE. Threei o `lpParam` por todos os `CreateWindow*`.

## ⇒ A PRÓXIMA FRONTEIRA = import NÃO resolvido DENTRO da threadproc da Worker Window
As threads ring-3 JÁ FUNCIONAM (commit `d591fab`, cooperativas — ver "COMO AS THREADS FUNCIONAM"
abaixo). Agora a threadproc da Worker Window (**RVA 0x72270**, runtime 0x0438B270) roda: faz o
prólogo + stack-cookie, `GetProcessHeap`+`HeapAlloc` (resolvem), e então **chama um import NÃO
RESOLVIDO (IAT=0 → `call [0]` → rip=0 → Page Fault)**. Serial:
`[EXCECAO] ... rip=0x0 ...` + `[bringup] ring-3 chamou import NAO resolvido (IAT=0)`.

**Como atacar:** `python disrange.py 0x72270 200` e ache o 1º `call [rip+...]` cujo alvo é um
import de DLL que NÃO temos (SHLWAPI/OLEAUT32/PROPSYS/RPCRT4/ole32/CoreMessaging/etc.) OU um
apiset que não redireciona. Implemente essa função (na DLL certa, com comentário GRANDE) ou crie
a DLL + adicione aos `-Modules`. Provável: init de TLS/thread, ou um objeto do desktop. Ligue o
`U32_TRACE` e/ou o `[bringup]` já dá o rip; use `whocalls.py`/`ripref.py` p/ mapear. Depois que a
threadproc passar, o loop de mensagem da Worker Window (`sub_7e8e0`, `MsgWaitForMultipleObjectsEx`)
deve rodar — aí falta o win32k ENTREGAR mensagens (WM_PAINT do wallpaper) p/ a Worker Window
desenhar o desktop. (Nosso `MsgWaitForMultipleObjectsEx`→WAIT_TIMEOUT 0x102; o loop provavelmente
faz PeekMessage — verifique o que ele chama e implemente a entrega real.)

## ⚙️ COMO AS THREADS RING-3 FUNCIONAM (implementado nesta sessão — commit `d591fab`)
Modelo COOPERATIVO que reaproveita a preempção de UMA thread ring-3 (o explorer já rodava sobre o
contexto da idle/boot, e o timer já preemptava/retomava esse ring-3):
- `kernel32 CreateThread(start,param)` → `NtCreateThread` (agora **4 args**, `param` em r10) →
  `sys_createthread` (syscall.c) REGISTRA o worker pendente em `g_ring3_threads[]` e devolve o
  handle (não roda ainda).
- `sys_waitforsingleobject(handle)`: se `handle` é de um worker pendente, chama
  `usermode_run_worker(start,param)` (usermode.c) → aloca **pilha 256 KiB (pmm_alloc_contiguous +
  mm_map_user) + TEB próprio** (PEB compartilhado) e **IRETQ p/ a threadproc(param)** (rcx=param,
  gs.base=TEB). A thread principal que chamou (o `WaitForSingleObject(worker)` do `~WorkerWindow`)
  é ABANDONADA aqui → a threadproc VIRA a thread ring-3 corrente. O processo PERSISTE.
- Limitações conhecidas (p/ evoluir): (a) é 1 worker por vez de fato ativo (o main fica parado);
  se o explorer criar N threads e esperar TODAS, só a 1ª roda — evoluir p/ threads ring-3
  PREEMPTIVAS de verdade (TSS rsp0 + gs.base por thread no `ki_quantum_end`; ver notas abaixo).
  (b) se a threadproc RETORNA (não deveria — é loop), cai em rip=[stack] indefinido.
- **Evolução p/ preemptivas** (quando precisar de várias threads simultâneas): dar a cada thread
  ring-3 um KTHREAD (`ki_create_thread`) cujo entry (ring0) faz `enter_ring3_arg`; no
  `ki_quantum_end` (sched.c) setar `tss_set_rsp0(next->kstack_top)` + `HalWriteMsr(GS_BASE,
  next->user_teb)` a cada troca (guardar `kstack_top`/`user_teb`/`is_ring3` no `ki_thread_t`).
  TSS é único (só CPU0 roda ring-3). Sem swapgs no kernel (gs.base tolerado em qualquer valor).

### Diagnóstico HISTÓRICO (por que saía antes — RVAs p/ `disrange.py`; base 0x04319000)
- **`wWinMain` (RVA 0x23350)** faz TODO o setup do shell e **RETORNA** — **NÃO há loop de mensagem
  no corpo do wWinMain**. O main NÃO importa GetMessage/PeekMessage/DispatchMessage. Usa
  `MsgWaitForMultipleObjectsEx`, mas isso está em `sub_7e8e0` (RVA 0x7e8e0), na THREAD da janela.
  (`sub_7e40c`, chamado em 0x23e93, é getter, não loop.)
- A Worker Window é objeto LOCAL na pilha do wWinMain (`[rbp+0x170]`). **`sub_92598`** (call @0x23e41)
  inicializa: `CreateWindowInBand`→HWND `[rdi+0x28]`; `#2522(hwnd)`; `CreateThreadpoolWork`+
  `SubmitThreadpoolWork` (callback RVA **0x9b230** = `SendMessageW(hwnd, WM_USER+7)`); `CreateEventW`
  →`[rdi+0x120]`; **`sub_926b8`** = `CreateThread`(threadproc RVA **0x72470**, ctx=rdi)→`[rdi+0x118]`
  + `CreateThreadpoolWait`.
- Nosso **`CreateThread` (kernel32.c:701) é NO-OP** (retorna handle fake, NEM chama NtCreateThread);
  e `PsCreateThread` (ps/process.c:122) só cria o OBJETO ETHREAD — comentário no código: "sem
  scheduler proprio". Logo a threadproc 0x72470 (o loop de msg da Worker Window) **NUNCA roda**.
- Sem thread e sem loop no main, o `wWinMain` chega ao **epílogo** (RVA ~0x24004+): ReleaseMutex
  (mutex de instância `[rbp+0x2c0]`), CoUninitialize, UnhookWinEvent, e o **destrutor `~WorkerWindow`
  (RVA 0x2fe434, call @0x24035; variante parcial 0x2fe3c4 @0x240d7)**: `WaitForSingleObject`(thread)+
  **`DestroyWindow`(HWND #1)** (`[rbx+0x28]`, call @0x2fe526)+`CloseHandle`(event)+`UnregisterClassW`.
  Depois retorna → CRT → `ExitProcess(0)`.
- **Prova no serial:** `CreateWindowEx -> HWND #1 'Worker Window'` → `DestroyWindow #1` → `encerrou (0x0)`, SEM crash.
- **Por que NÃO há atalho por waits:** nossos waits são stubs (`MsgWaitForMultipleObjectsEx`→
  WAIT_TIMEOUT 0x102; `WaitForSingleObjectEx`/`WaitForMultipleObjectsEx`→0). Loop de msg com
  WAIT_TIMEOUT SPINARIA (não sai) — então o exit NÃO é por wait curto-circuitado. É porque o
  wWinMain simplesmente TERMINA o setup; a persistência do explorer real depende da THREAD.

### PLANO DETALHADO — implementar threads ring-3 (a única forma do processo persistir)
O modelo de ring-3 hoje (`src/ntos/ke/amd64/usermode.c`): `usermode_enter(entry)` mapeia UMA pilha
de usuário `[0x600000,0x700000)`, monta UM TEB/PEB no fundo dela, cria CR3 por-processo, faz
`setjmp(g_user_exit)` e `enter_ring3` (IRETQ p/ ring-3 com SS/RSP/RFLAGS/CS/RIP e `gs.base=TEB`).
A saída é por `int 0x80`→`sys_exit`→`longjmp(g_user_exit)`. **NÃO há troca de contexto ring-3.**

Para threads ring-3 PREEMPTIVAS (a Worker Window precisa rodar CONCORRENTE com o wWinMain, ANTES
do epílogo destruir a janela — não há ponto de yield cooperativo, então tem que ser preempção):
1. **`CreateThread` real** (kernel32 → novo/ajustado NtCreateThread): alocar pilha de usuário
   PRÓPRIA por thread (não a única `[0x600000,..)`; ex.: faixas `0x600000 + N*0x100000`, ou via
   `NtVirtualAlloc`) + **TEB próprio por thread** (cada um com StackBase/Limit e Self em gs:[0x30]).
2. **Contexto por ETHREAD**: guardar o estado ring-3 completo (todos os GPRs, RIP, RSP, RFLAGS,
   segmentos) no ETHREAD. Inicial = threadproc no RIP, arg (rcx) = ctx, RSP = topo da pilha nova.
3. **Troca de contexto no TIMER** (`src/ntos/ke/amd64/isr.c`, vetor do APIC timer 0xD1): quando o
   IRQ do timer chega em RING-3, salvar o contexto da thread corrente (do frame de interrupção +
   GPRs), escolher a próxima thread ring-3 runnable (rotação no scheduler `sched.c`), restaurar o
   contexto dela e IRETQ. Cuidar de `gs.base` (TEB da thread), TSS `rsp0` (pilha de kernel por
   thread p/ o próximo trap), e o CR3 (mesmo processo → mesmo CR3).
4. **Integrar com o scheduler** (`src/ntos/ke/sched.c` + `sched_asm.asm`): ele já troca KTHREADS
   (kernel). Ver como ele salva/restaura e ADICIONAR as threads ring-3 do processo à rotação.
5. **`ExitThread`/`ExitProcess`**: thread que retorna da threadproc → termina só ela; ExitProcess
   termina o processo. Enquanto houver thread ring-3 viva, o processo NÃO sai (o wWinMain retorna,
   mas a thread da Worker Window segue rodando o loop → o desktop/wallpaper aparece).
⚠️ **É a área MAIS SENSÍVEL ao pintok** (timer/IDT/TSS/syscall/timing — o pintok intercepta
CPUID/RDTSC e mede timing). **Aditivo, incremental, e `pintok` VERDE a cada passo** (regra de
ouro C0000365 — ver [[pintok-safe-kernel-expansion]]). Ver `smp-scheduler-tcg-limit` (o AP já roda
em paralelo mas SEM timer local no TCG do QEMU — pode limitar onde escalonar; talvez tudo no BSP).

## 🗺️ MAPA DE RE DO EXPLORER (RVAs no binário real; base disassembler 0x140000000)
- `wWinMain` = **0x23350**. modo-3 shell/desktop começa em **0x239cd**.
- Worker Window: registrar da classe `sub_92780` (WNDCLASSEXW cbSize=0x50, style=3, wndproc 0x72e00,
  `GetStockObject(5=NULL_BRUSH)`, `LoadCursorW`, className "Worker Window" @rva 0x3ac868, atom→[rbx]).
- `sub_92598` (init da Worker Window, ÚNICO caller 0x925aa dentro de wWinMain @0x23e41): cria janela
  (`CreateWindowInBand`), threadpool work (callback 0x9b230 = `SendMessageW WM_USER+7`), evento, thread.
- `sub_926b8` (cria a THREAD): `CreateThread(threadproc 0x72470)` + `CreateThreadpoolWait`.
- wndproc da Worker Window = **0x72e00**: em WM_NCCREATE(0x81) faz `SetWindowLongPtrW(hwnd,-21,obj)`
  (obj=lpCreateParams) e retorna 1; nas outras msgs faz `GetWindowLongPtrW(hwnd,-21)` e, se !=0,
  despacha p/ `sub_72e84` (método do objeto). Índice GWLP_USERDATA=-21 (0xffffffeb).
- `~WorkerWindow` = **0x2fe434** (completo) e **0x2fe3c4** (parcial). DestroyWindow em 0x2fe526.
- Loop de msg (na thread): `sub_7e8e0` faz `MsgWaitForMultipleObjectsEx` (0x7e912).
- Ordinais privados USER32 que o explorer usa: #2005(ptr,ret ignorado), #2521(GetProcessUIContext
  Information), #2522(hwnd,ret ignorado), #2573/#2574(gate de GetWindowBand)/#2611.

## 🔁 O LOOP — comando de run (16 módulos, com dwmapi; NÃO passe de ~16)
```
.\run.ps1 -Modules build\ntdll.dll,build\kernel32.dll,build\user32.dll,build\gdi32.dll,build\advapi32.dll,build\ucrtbase.dll,build\combase.dll,build\msvcp_win.dll,build\shell32.dll,build\shcore.dll,build\dxgi.dll,build\uxtheme.dll,build\comctl32.dll,build\dui70.dll,build\dwmapi.dll,build\explorerreal.exe -Headless -TimeoutSec 60
```
(Se `build\explorerreal.exe` sumiu: `cp C:\Windows\explorer.exe build\explorerreal.exe`.)
Ver `build\serial.log`. Marco atual: `CreateWindowEx -> HWND #1 'Worker Window'` → `DestroyWindow #1` → sai.
Regressão: `.\run.ps1 -Scenario desktop -Headless -TimeoutSec 25` (login→shell caseiro, tem que continuar OK).

## 🧭 SEQUÊNCIA RING-3 HOJE (em ordem)
`desk.cpl`(falha opc) → `GetDC(NULL)` → carrega `comctl32` → probe GDI/locale (downlevel opc) →
`RegisterClass("Worker Window")` (atom 0xC000) → **`CreateWindowInBand`→HWND #1** (envia WM_NCCREATE
inline → wndproc associa objeto) → `#2522(hwnd)` → `CreateThreadpoolWork`+`SubmitThreadpoolWork`
(→`SendMessage WM_USER+7` inline → wndproc → método do objeto, roda OK) → `CreateEventW` →
`CreateThread`(**no-op**) → setup do shell (mode-3 0x239cd: CoCreateInstance, WinEvent hooks) →
**epílogo: `~WorkerWindow`→`DestroyWindow #1`** → `ExitProcess(0)`.

## 🚧 MUROS/IMPORTS CONHECIDOS (em OUTROS caminhos — o explorer sai antes de chegar neles)
- **Threads ring-3** (o muro ATUAL — ver plano acima).
- ✅ `dwmapi.dll` (16 imports) FEITO nesta sessão (composição OFF; DwmIsCompositionEnabled→FALSE).
- Imports estáticos NÃO resolvidos → **slot=0** (crasham SE chamados):
  `OLEAUT32`(ordinais), `PROPSYS`(9), `RPCRT4`(8), `SHLWAPI`(nomes+ordinais), `ole32`, `urlmon`,
  `WININET`, `CoreMessaging`, `TWINAPI`, `WTSAPI32`, `IPHLPAPI`, vários `api-ms-win-*` e `ext-ms-win-*`.
  Delay imports NÃO resolvidos → `LdrpNullStub` (retorna 0, NÃO crasha). Implemente por demanda.
- Redirects: `src/ntos/ldr/loader.c` `apiset_redirect` (crt→ucrtbase, core→kernel32, *ntuser*→user32,
  shell-*→shell32, shcore-*→shcore, gdi-*→gdi32, kernelbase→kernel32, com/winrt→combase, security/
  eventing→advapi32). **DLL nova** = crie a DLL + adicione aos `-Modules`. **Ordinal privado de DLL
  que já temos** = exporte no `.def` (`nome @N NONAME`; resolve via `pe_get_export_by_ordinal`).

## 🛠️ FERRAMENTAS (raiz, versionadas)
`disrange.py <rva> <n>` (desmonta N instr a partir do RVA, nomeia CALL[IAT]) · `disat.py <hex>` ·
`callers.py <rva>` (acha CALL E8 diretos p/ o rva) · `strxref.py <substr>` · `rdstr.py <rva>` ·
`dumpimports.py [filtro]` (imports estáticos) · `dumpexports.py` · `nextwall.py` · `gap.py`/`redirgap.py`.
**RECRIE do histórico (scratchpad da sessão 4 — foram MUITO úteis; ver git/journal se sumiram):**
- `ripref.py <rva...>` — brute-force de `LEA/MOV reg,[rip+disp]` que apontam p/ um RVA (acha refs que
  o disasm linear PERDE por desalinhamento). Foi como achei o registrar da "Worker Window".
- `whocalls.py <nome_import>` — IAT slot + TODAS as call-sites `call [rip]` de um import NOMEADO.
- `ordsites.py` — call-sites dos ordinais privados + como o retorno é usado (p/ escolher o stub).
- p/ decidir se um slot é import estático vs delay vs ntuser-private: varra dir 1 (imports) e dir 13
  (delay) da import table (offset 0x3C→PE, opt+0x70/0x0F8). Ex.: `CreateWindowInBand`, `DestroyWindow`,
  `SendMessageW`, `Set/GetWindowLongPtrW` são DELAY de `ext-ms-win-rtcore-ntuser-window-ext`→user32.

## 🔩 DIAGNÓSTICO — GATES (deixe todos em 0 nos commits)
- **`U32_TRACE`** em `dll/win32/user32/user32.c` (=1 liga): loga via `sys_write` (int 0x80, rax=1,
  rdi=string) — `u32:create hwnd/proc`, `u32:SendMsgW msg/ra`, `u32:DestroyWindow ra`. **ra =
  return-address ring-3**; RVA = ra − 0x04319000 → joga no `disrange.py` p/ achar o CHAMADOR real.
  (Foi assim que localizei o `~WorkerWindow`/DestroyWindow e a call-site do SendMessage.)
- `GPA_TRACE`/`REG_TRACE` em `src/ntos/ke/amd64/syscall.c` (kernel → re-verifique pintok).
- `COMBASE_DBG` em `dll/win32/combase/combase.c`.

## ⛔ REGRA DE OURO — pintok.sys. NÃO QUEBRAR.
Após CADA incremento no KERNEL (`src/ntos/...`): `.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`.
Dourado: `[P1]/[P2]/[P3] ==== PROVA PASSOU ====`; `intercept totals: CPUID x3 RDTSC x33 RDMSR x0
ANTIVM x0`; `DriverEntry ... C0000365`; SEM "Sistema parado". **DLLs de userland NÃO afetam o
pintok** (confirmado nesta sessão: TODOS os fixes foram user32 e o pintok seguiu verde). Syscalls
novos: **append no FIM** do enum + `s_ssdt[]` em `syscall.c`. Threads ring-3 VÃO mexer no kernel —
rode o pintok a CADA passo e reverta na hora se `ANTIVM`/`C0000365` mudar.

## 📜 COMMITS DA SESSÃO 4 (branch `feat/kernel-foundation-irql-dpc`; push a cada lote)
- `a480768` feat(user32): explorer CRIA sua Worker Window — CreateWindowInBand real + ATOM único
  0xC000+slot + 6 ordinais privados (.def) + build.ps1 passa o .def.
- `f09b51e` feat(user32): GWLP_USERDATA real + WM_NCCREATE inline → a Worker Window associa
  seu objeto e roda o handler real de WM_USER+7 (sem crash) + gate `U32_TRACE` (diagnóstico, off).
- `0259593` feat(dwmapi): dwmapi.dll nova (16 imports resolvidos; composição OFF) + build.ps1 + 16º módulo.
- `d591fab` feat(kernel): **THREADS RING-3 cooperativas** (usermode.c/h + syscall.c + ntdll + kernel32)
  → o explorer PERSISTE (não sai mais); a threadproc entra em ring-3 e roda até um import não resolvido.
Mensagens terminam com `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## 📌 NOTAS / GOTCHAS
- Boot aguenta ~16 módulos. Set atual = 15.
- ⚠️ **BSS grande (>1 MiB) em DLL de base baixa TRAVA o boot** (o loader mapeia só 2 MiB p/ a DLL)
  — use `NtVirtualAlloc` (ex.: bits do CreateDIBSection).
- ⚠️ O `[EXCECAO] vetor=0x0e ... rip=0x132752 ... expected-trap` no início do serial é a PROVA de
  paginação (Pilar 1), NÃO um crash. Crash real de ring-3 = `[bringup] excecao em RING-3` + `Sistema parado`.
- SDK do Windows: `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\` (winuser.h,
  dwmapi.h, etc.). dui70/DirectUI = privado (4321 exports C++ mangled).
- `run.ps1` roda com `-smp 2 -accel tcg,thread=multi`; TEB/PEB no fundo da pilha `[0x600000,0x601fff]`.

**Agora: ataque as THREADS RING-3 (o muro atual) — é o que falta p/ o explorer PERSISTIR (rodar o
loop da Worker Window e desenhar o desktop). Comece pelo `CreateThread` real (pilha+TEB por thread)
e a troca de contexto no timer, ADITIVO e pintok-verde a cada passo. Vá, sem parar, até ~600k.**
