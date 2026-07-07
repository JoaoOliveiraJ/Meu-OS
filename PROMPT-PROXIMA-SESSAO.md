# PROMPT — próxima sessão: continuar rodando o `explorer.exe` REAL do Windows 10

Cole isto como prompt inicial. É AUTOSSUFICIENTE — comece a trabalhar imediatamente.

---

## ⚙️ MODO DE TRABALHO (o mais importante — LEIA ISTO)

**Trabalhe de forma AUTÔNOMA. NÃO pare para me perguntar "continua?".** Rode o loop de
bring-up do explorer real, implementando **um muro por vez, de verdade**, sem parar, até
usar ~600k de tokens de contexto. Só então: escreva um novo `PROMPT-PROXIMA-SESSAO.md`
atualizado, faça o commit final, e me dê o balanço. Antes disso, siga implementando e
commitando sozinho. **NUNCA um stub genérico catch-all** — só stubs ESPECÍFICOS e nomeados.

Disciplina: **build → rodar o explorer → diagnosticar → implementar REAL → build → pintok
(SE mexeu no kernel `src/ntos/...`) → commit + push**. Builds/QEMU em background (~1 min).
Zig: `tools\zig-windows-x86_64-0.13.0\zig.exe`. **PowerShell**: aspas nos args `-Wl,...`
(a vírgula vira operador de array): `& $zig cc ... "-Wl,--image-base=0x5900000" ...`.
Rebuild rápido de UMA dll (ex.: gdi32, que importa do ntdll):
`& $zig cc -target x86_64-windows-gnu -shared -nostdlib -e DllMain "-Wl,--image-base=0x1A00000" -o build\gdi32.dll dll\win32\gdi32\gdi32.c build\libntdll.a`

## 🎯 A MISSÃO
Rodar o **binário REAL** `C:\Windows\explorer.exe` no MeuOS. NÃO escrever um explorer.
Detalhes: `RECON-EXPLORER.md` + memória `run-real-explorer-mission`.

## 📍 ESTADO — MARCO: o explorer REGISTRA sua 1ª CLASSE DE JANELA ("Worker Window")
Enorme avanço vs. a sessão anterior (que bilava em `LoadLibrary("dui70")`). Agora o explorer:
carrega tudo, resolve COM+WinRT+MRT, **passa a init de Feature Staging (era um LOOP INFINITO)**,
carrega **comctl32 + dui70**, monta recursos de desenho (GDI), e **chama `RegisterClassW` p/
registrar a classe `"Worker Window"`** — a 1ª criação de classe de janela do explorer real aqui.
Serial: `[win32k] RegisterClass 'Worker Window' wndproc=0x...`. Base do explorer: **0x04319000**.

## ⇒ A PRÓXIMA FRONTEIRA = por que o explorer SAI logo após `RegisterClass("Worker Window")`
Ele registra a classe e **SAI limpo (status=0x0) SEM chamar `CreateWindowExW`** (0 logs
`[win32k] CreateWindowEx -> HWND`). O `wWinMain` **RETORNA 0** (→ CRT → `ExitProcess(0)`),
SEM criar janela nem thread. O bail está **ENTRE** o register e o create. NÃO é função
faltando (não há `[gpa] MISS` depois do RegisterClass): é uma **decisão interna** do wWinMain.
**CONFIRMADO: NÃO há crash** — o `isr.c` logaria `[bringup] excecao em RING-3` + `Sistema
parado` numa falha; não aparece. Logo o explorer NÃO está chamando CreateWindowExW e derefando
lixo — ele decide retornar 0 de propósito. (Descarta a hipótese do ATOM-crash; ver suspeito a.)

### Como atacar (o que investigar primeiro)
1. **RE do bail pós-RegisterClass("Worker Window").** "Worker Window" = a WorkerW do DESKTOP
   (papel de parede), não a taskbar. O explorer registra a classe do desktop e desiste antes
   de criar. Descubra o que ele checa logo após: prováveis suspeitos —
   - **ATOM (bug latente, NÃO o muro confirmado):** `RegisterClassW` devolve ATOM=1 fixo;
     `u32_wpool` trata className<0x10000 como ATOM cru → `find_wndproc` derefaria endereço
     pequeno. Como NÃO há crash, o explorer NÃO chega a CreateWindowExW — então isto é bug
     latente, corrija (ATOM único 0xC000+slot + resolva ATOM→nome), mas NÃO espere que mova
     o muro. O muro é ANTES: a decisão do wWinMain de não criar a janela.
   - Um objeto COM/serviço do desktop (ex.: `IShellDesktopTray`, `SHDesktopMessageLoop`) que
     falha e faz o wWinMain retornar 0.
   - **DWM/composição** (`dwmapi` tem 16 imports NÃO resolvidos — ver abaixo).
2. **Ligue os gates de diagnóstico:**
   - `GPA_TRACE 1` em `src/ntos/ke/amd64/syscall.c` (linha `#define GPA_TRACE`) → loga cada
     `GetProcAddress` que FALHA. **Kernel → re-verifique pintok.** (Commitado em 0.)
   - `REG_TRACE 1` (mesmo arquivo) → loga `NtOpenKey`/`NtQueryValueKey`. Vê o que o desktop lê.
   - `COMBASE_DBG 1` em `dll/win32/combase/combase.c` → CLSID/IID/classe-WinRT/slot.
3. **Instrumente o win32k** (`src/subsystems/win32/win32kbase.c`): `NtUserCreateWindowEx_k`
   (~linha 725) e `NtUserRegisterClass_k` (~704) JÁ logam. Pra ver a decisão de saída,
   instrumente `sys_exit` (syscall.c:96, faz `__builtin_longjmp`) com o RIP do chamador.
   ⚠️ NÃO use `__builtin_return_address`+loop-hex sem testar (deu opcode inválido antes).
4. **RE estático** (base explorer 0x140000000 no disassembler; RVA é o que os scripts usam):
   `python disrange.py 0x<rva> <n>` (desmonta, nomeia CALL[IAT]). wWinMain RVA **0x23350**;
   modo-3 (shell/desktop) em **0x239CD**. `strxref.py "Worker Window"` acha a string mas NÃO há
   LEA rip-relative p/ ela (é referenciada via ponteiro em .data — a WNDCLASSEXW).

## 🔁 O LOOP — comando de run (15 módulos; +uxtheme +comctl32 +dui70; NÃO passe de ~16)
```
.\run.ps1 -Modules build\ntdll.dll,build\kernel32.dll,build\user32.dll,build\gdi32.dll,build\advapi32.dll,build\ucrtbase.dll,build\combase.dll,build\msvcp_win.dll,build\shell32.dll,build\shcore.dll,build\dxgi.dll,build\uxtheme.dll,build\comctl32.dll,build\dui70.dll,build\explorerreal.exe -Headless -TimeoutSec 60
```
(Se `build\explorerreal.exe` sumiu: `cp C:\Windows\explorer.exe build\explorerreal.exe`.)
Ver `build\serial.log`. Marco atual: `[win32k] RegisterClass 'Worker Window'` e sai status=0x0.

## 🧭 SEQUÊNCIA DO RING-3 HOJE (o que o explorer faz, em ordem, antes de sair)
`TestCreate`(GetProcAddr miss, opcional) → `LoadLibrary("desk.cpl")`(falha, opcional) →
`GetDC(NULL)` → carrega `comctl32` → `#20`(miss ordinal, opcional) → probe de GDI/locale
(BitBlt/CreateCompatibleBitmap/GdiAlphaBlend/GetUserPreferredUILanguages/LCIDToLocaleName —
tudo **downlevel-probe OPCIONAL**, o explorer trata NULL e segue) → **`RegisterClass("Worker
Window")`** → **SAI (status 0)**. Os probes de GetProcAddress NÃO são o muro (são detecção de
API downlevel, cacheada como NULL). O muro é a decisão do wWinMain de retornar 0 aqui.

## 🚧 MUROS CONHECIDOS AINDA NÃO RESOLVIDOS (imports NULOS na IAT do explorer)
- **dwmapi.dll (16)**: `DwmSetWindowAttribute`, `DwmGetWindowAttribute`, `DwmIsCompositionEnabled`,
  `DwmEnableBlurBehindWindow`, `Dwm*Thumbnail*`, ordinais #113/114/124/138/139/140/141/159.
  DLL nova tratável (autocontida, int 0x80). Composição/atributos de janela — provável na init.
- **CreateWindowExW com ATOM** (ver frontier #1). Corrigir o ATOM em user32.
- Mais tarde: escalonamento de **threads ring-3** (o explorer ainda NÃO chama CreateThread),
  loop de mensagem real (hoje `GetMessageW`→`NtUserGetMessage`), DWM/composição, .mui (recursos).

## 🧭 REDIRECTS (`src/ntos/ldr/loader.c` `apiset_redirect`)
`api-ms-win-crt-*`→ucrtbase · `core-com*`/`core-winrt*`→combase · `core-registry`→advapi32 ·
`security-*`/`eventing-*`→advapi32 · `core-*`→kernel32 · **`kernelbase`→kernel32** · `*ntuser*`
/`rtcore-ntuser-*`/`ext-ms-win-*ntuser*`→user32 · `shell-*`/`storage-*`→shell32 · `shcore-*`→
shcore · `ext-ms-win-gdi-*`→gdi32 · diretas: `userenv`/`sspicli`→advapi32. **DLL nova**: se o
import é o nome direto (ex.: `dui70.dll`, `comctl32.dll`, `uxtheme.dll`), basta criar a DLL +
adicioná-la aos `-Modules` (sem redirect). O `ldr_load_runtime` anexa `.dll` a nomes sem ext.

## 🔩 RUNTIME LOADER (NOVO nesta sessão — `loader.c`/`syscall.c`)
`LoadLibraryW` (kernel32) agora FUNCIONA (era stub→0). `sys_loadlibrary`→`ldr_load_runtime`
(aplica `apiset_redirect` + anexa `.dll`). `sys_getmodulehandle`→`ldr_find_runtime` (idem, sem
carregar). `sys_getprocaddress` trata **ordinal** (fn<0x10000) + gate `GPA_TRACE`.

## 🛠️ FERRAMENTAS (raiz, versionadas)
`nextwall.py` · `disat.py <hex>` · `disrange.py <rva> <n>` · `callers.py <rva>` · `rdstr.py
<rva>` · `strxref.py <substr>` · `dumpimports.py [dll]` (imports ESTÁTICOS; usa
`C:\Windows\explorer.exe`, arg = filtro) · `dumpexports.py`. **Delay imports** (DUI70/comctl32
são delay): parser em `scratchpad/delayimp.py` desta sessão — recrie (parseia dir 1 + dir 13
da import table; útil p/ achar de onde vêm RegisterClassExW/CreateWindowExW etc.).

## 🔨 PRONTO (DLLs e cobertura)
Kernel loader (nome/ordinal/delay, PMM+reloc, redirects, runtime LoadLibrary/GetModuleHandle/
GetProcAddress). DLLs: ntdll (+Feature Config: `RtlQueryFeatureConfiguration`/`Register...
ChangeNotification`/`RtlDllShutdownInProgress`/`RtlDisownModuleHeapAllocation`), ucrtbase,
kernel32 (+`LoadLibraryW` real, condition variables, MUI `Get*PreferredUILanguages`),
user32 (~200: **W-variants de janela** RegisterClassW/ExW+CreateWindowExW+loop+geometria+props+
timers+hooks, além das A e das 116 GUI), **gdi32 (~50: 28 imports do explorer + bitmaps/DIB +
blend/gradient/transparente + clip/DC/pen)**, advapi32, shell32, shcore, msvcp_win, dxgi,
**combase (35 COM+22 WinRT, univ_fill p/ MRT)**, **uxtheme (28, tema clássico)**, **comctl32
(v6 nomeadas)**, **dui70 (SkipDLLUnloadInitChecks — só p/ LoadLibrary("dui70") ter sucesso)**.

## ⛔ REGRA DE OURO — pintok.sys. NÃO QUEBRAR.
Após CADA incremento no KERNEL (`src/ntos/...`): `.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`.
Dourado: `[P1]/[P2]/[P3] ==== PROVA PASSOU ====`; `intercept totals: CPUID x3 RDTSC x33 RDMSR x0
ANTIVM x0`; `DriverEntry ... C0000365`; SEM "Sistema parado". DLLs de userland NÃO afetam o
pintok. (Confirmado nesta sessão: os fixes de loader/syscall passaram pintok intactos 2x.)
Syscalls novos: **append no FIM** do enum + `s_ssdt[]` em `src/ntos/ke/amd64/syscall.c`.

## 📜 COMMITS DESTA SESSÃO (branch `feat/kernel-foundation-irql-dpc`; push a cada lote)
lote 1 `uxtheme` (28 imports, tema clássico) · lote 2 `kernel+kernel32` (LoadLibraryW real +
runtime module resolution) · lote 3 `comctl32+dui70+kernel` (carregam + GetProcAddress ordinal
+ mapa) · lote 4 `ntdll+kernel32` (QUEBRA o loop infinito de Feature Config + condition vars) ·
lote 5 `gdi32` (6→28 + BitBlt) · lote 6 `gdi32+kernel32` (blend/raster/DIB + MUI/locale) ·
lote 7 `user32` (W-variants → explorer REGISTRA "Worker Window"). Mensagens terminam com
`Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## 📌 NOTAS
- Boot aguenta ~16 módulos. Set atual = 15 (adicione dwmapi quando existir → 16, no limite).
- ⚠️ **BSS grande em DLL de base baixa TRAVA o boot**: o loader mapeia só 2 MiB p/ a DLL. NÃO
  use arrays estáticos grandes (>1 MiB) — use `NtVirtualAlloc` (ex.: bits do CreateDIBSection).
- SDK do Windows: `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\` (uxtheme.h,
  winuser.h, dwmapi.h documentados). dui70/DirectUI = privado (4321 exports C++ mangled).

**Agora: descubra por que o wWinMain RETORNA 0 logo após `RegisterClass("Worker Window")`
(sem `CreateWindowExW`). Comece pelo fix do ATOM em user32 (CreateWindowExW com atom), depois
`dwmapi.dll` (16 imports, DLL nova tratável), depois RE do bail com GPA_TRACE/REG_TRACE +
disrange. Vá, sem parar, até ~600k.**
