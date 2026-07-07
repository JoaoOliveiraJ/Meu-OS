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
Zig: `tools\zig-windows-x86_64-0.13.0\zig.exe`. Rebuild rápido de UMA dll (ex.: combase):
`& $zig cc -target x86_64-windows-gnu -shared -nostdlib -e DllMain -Wl,--image-base=0x5500000 -Wl,--dynamicbase -o build\combase.dll dll\win32\combase\combase.c`

## 🎯 A MISSÃO
Rodar o **binário REAL** `C:\Windows\explorer.exe` no MeuOS. NÃO escrever um explorer.
Detalhes: `RECON-EXPLORER.md` + memória `run-real-explorer-mission`.

## 📍 ESTADO — MARCO: o explorer entra no caminho do SHELL/DESKTOP (modo 3) e roda a init da taskbar
Ele carrega, resolve os imports, roda a init COMPLETA de COM+WinRT+**MRT** SEM crashar,
entra no `wWinMain`, escolhe o **modo 3 (shell/desktop)** e roda a init REAL da taskbar/menu
(lê ShellServiceObjects, StartPage, SettingSync, SearchboxTaskbarMode/Width, StartColorMenu,
Accent...). Encerra **LIMPO** via `ExitProcess(0)` na fase de **construção da UI da taskbar**.
Base do explorer: **0x04319000** (determinística). Memória: `explorer-com-winrt-clean-mrt-frontier`.

Diagnóstico PROVOU o ponto de saída: o explorer chama **`ExitProcess(0)` EXPLÍCITO** (retorno
0 do wWinMain → CRT exit) e **NUNCA chama `CreateThread`** → não é problema de thread ainda;
ele bila na construção da UI porque falta o framework de UI (dui70/DirectUI) + tema (UxTheme).

## ⇒ A PRÓXIMA FRONTEIRA = UI da TASKBAR/DESKTOP (dui70/DirectUI + UxTheme)
O explorer em modo 3 lê toda a config do shell e então tenta montar a taskbar/menu-iniciar,
que dependem de:
- **`dui70.dll` (DirectUI)** — framework da UI do shell (PRIVADO/undocumented, exports por
  ordinal; GRANDE). É o bloqueador PROVÁVEL da saída (LoadLibrary("dui70") falha logo antes).
- **`UxTheme.dll`** — ~25 imports NÃO resolvidos (OpenThemeData, DrawThemeBackground,
  GetThemeColor/Font/Metric/Margins, IsThemeActive, DrawThemeTextEx, ...). DLL TRATÁVEL e
  documentada — bom alvo p/ uma DLL nova real. (delay-load: ausência é tolerada, mas o tema
  fica sem estilo.)
- Depois: **RegisterClassExW/CreateWindowExW** (o user32 só tem variantes **A** hoje; as **W**
  NÃO são exportadas — o explorer usa as W p/ Shell_TrayWnd/Progman), criação de janela real,
  escalonamento de **threads ring-3** (CreateThread é stub que NÃO roda a thread), e **DWM**.

### Como atacar (o loop de RE já montado)
1. **Ligue os gates de diagnóstico** conforme o alvo:
   - `REG_TRACE 1` em `src/ntos/ke/amd64/syscall.c` → loga cada `NtOpenKey`/`NtQueryValueKey`
     (mapeia o que o shell lê no registro). **Kernel → re-verifique pintok.**
   - `COMBASE_DBG 1` em `dll/win32/combase/combase.c` → loga CLSID/IID/classe-WinRT/slot.
   - Para achar o ponto de saída: instrumente kernel32 `ExitProcess`/`CreateThread` com um
     logger int 0x80 (ex.: `static void k32dbg(const char*s){unsigned long long r;__asm__ volatile("int $0x80":"=a"(r):"a"(1ULL),"D"(s):"memory","rcx","r11");}`).
     ⚠️ NÃO use `__builtin_return_address`+loop de hex sem testar — deu **opcode inválido**
     em C01517 na última sessão; prefira logar um marcador fixo e casar por disassembly.
2. **Diagnosticar** (scripts na raiz + scratchpad):
   - `python nextwall.py` — nomeia import NULO. `python disat.py 0x<rip>` — desmonta no crash.
   - Scratchpad desta sessão (recrie se sumiram): `disrange.py <rva> <n>` (desmonta faixa por
     RVA, nomeia CALL[IAT] e detecta call-virtual via CFG `call [rip+CFG]`), `callers.py
     <rva>` (acha os CALL rel32 que chamam um RVA), `rdstr.py <rva>` (lê string). Base do
     explorer no serial: `img=explorerreal.exe base=0x...`.
3. **Implementar a DLL/função REAL.** DLL nova: `dll/win32/<nome>/<nome>.c` (+`.def` se
   ordinais/mangled), bloco no `build.ps1` (ImageBase livre ≥0x5900000, o loader RELOCA) +
   `-Modules` (≤~16). UxTheme é autocontida (int 0x80 direto p/ log, sem ntdll) como a combase.

## 🔁 O LOOP — comando de run (12 módulos; NÃO passe de ~16, trava o boot)
```
.\run.ps1 -Modules build\ntdll.dll,build\kernel32.dll,build\user32.dll,build\gdi32.dll,build\advapi32.dll,build\ucrtbase.dll,build\combase.dll,build\msvcp_win.dll,build\shell32.dll,build\shcore.dll,build\dxgi.dll,build\explorerreal.exe -Headless -TimeoutSec 45
```
(Se `build\explorerreal.exe` sumiu: `cp C:\Windows\explorer.exe build\explorerreal.exe`.)
Ver `build\serial.log`. Marco esperado hoje: chega a `dui70` e sai `status=0x0` LIMPO.

## 🧠 O MAPA DO wWinMain (RE feito — não precisa refazer)
- Entry 0xA2940 → CRT `__scrt_common_main_seh` (0xA27C0) → `wWinMain` (RVA **0x23350**).
- wWinMain: init (ETW/AppUserModelID/priority) → `GetCommandLineW`/`PathGetArgsW` → parse do
  modo em **0xAA63C** → `esi` = modo. **esi==3 → 0x239CD (shell/desktop)**; ==5 → ShellExecute;
  ==2 → janela de pasta (sai sem pasta).
- Gate do modo 3 (0xAA63C): com args vazios, cai em 0xAA6F9 → **0xAA8DC** ("sou o shell?" —
  lê `HKLM\...\Winlogon\AlternateShells\AvailableShells`; ausência → fallback lê valor `Shell`
  =explorer.exe → TRUE) E **0xAA950** (`FindWindow "Progman"`/"Proxy Desktop" == NULL) → então
  cria/adquire o mutex do shell → **edi=3**. JÁ DESBLOQUEADO (fix do AvailableShells).

## 🧭 REDIRECTS (`src/ntos/ldr/loader.c` `apiset_redirect`)
`api-ms-win-crt-*`→ucrtbase · `core-com*`/`core-winrt*`→combase · `core-registry`→advapi32 ·
`security-*`/`eventing-*`→advapi32 · `core-*`→kernel32 · `*ntuser*`→user32 · `shell-*`→shell32 ·
`shcore-*`→shcore · `storage-*`→shell32 · `ext-ms-win-*`→(user32/gdi32/shell32/advapi32/kernel32)
· diretas: `userenv`→advapi32, `sspicli`→advapi32. **DLL nova**: adicione o redirect/host aqui.
UxTheme: o explorer importa `UxTheme.dll` direto → basta criar a DLL e adicioná-la aos módulos
(nome direto, sem redirect necessário se o import é "UxTheme.dll").

## 🛠️ FERRAMENTAS (raiz, versionadas)
`nextwall.py` · `disat.py <hex>...` · `strxref.py <substr>` · `redirgap.py [dll]`/`gap.py` ·
`dumpimports.py`/`dumpexports.py` · `gen_msvcp.py`. (E os do scratchpad — veja o LOOP acima.)

## 🔨 PRONTO (gap 0)
Kernel loader (NOME/ORDINAL/DELAY-LOAD, PMM+reloc, redirects, diag ring-3). Registro:
`sys_openkey`/`sys_queryvaluekey` (stub + AvailableShells honesto + gate REG_TRACE). DLLs:
ntdll, ucrtbase, kernel32(248), user32(140 GUI, só variantes A), gdi32, advapi32(seg+SHReg+
GetUserNameExW), shell32(IL*/KnownFolder), shcore(DPI), msvcp_win(97), dxgi.
**combase(35 COM+22 WinRT)**: IMalloc, IStream real, GUID/string, HSTRING real, objeto
UNIVERSAL com **`univ_fill` (preenche o out-param REAL em a2..a5)** → MRT passa; Ro* devolvem
o universal (S_OK). SCAFFOLD `COMBASE_DBG` gated.

## ⛔ REGRA DE OURO — pintok.sys. NÃO QUEBRAR.
Após CADA incremento no KERNEL (`src/ntos/...`): `.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`.
Dourado: `[P1]/[P2]/[P3] ==== PROVA PASSOU ====`; `intercept totals: CPUID x3 RDTSC x33 RDMSR x0
ANTIVM x0`; `DriverEntry ... C0000365`; SEM "Sistema parado". DLLs de userland NÃO afetam o pintok.
(Confirmado nesta sessão: o fix do registro em syscall.c passou pintok intacto.) Syscalls novos:
**append no FIM** do enum + `s_ssdt[]` em `src/ntos/ke/amd64/syscall.c`.

## 📜 COMMITS DESTA SESSÃO (branch `feat/kernel-foundation-irql-dpc`; push a cada lote)
`a9d6397` lote 1 (combase, userland) — MRT HONESTO: `univ_fill` preenche o out-param real →
explorer roda a init de recursos completa e chega ao wWinMain. `b84a9a3` lote 2 (kernel,
pintok-verde) — registro honesto p/ AvailableShells → explorer entra no modo 3 (shell/desktop)
e roda a init da taskbar. Mensagens terminam com `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## 📌 NOTAS
- Boot aguenta ~16 módulos. Mantenha o set de 12 (adicione UxTheme quando existir).
- SDK do Windows: `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\` (UxTheme:
  `um\uxtheme.h` — API documentada, ao contrário do dui70). MRT/dui70 = interfaces privadas.
- Heap combase: arena bump 2 MiB (sem free). combase/uxtheme autocontidas (int 0x80 p/ log).

**Agora: o próximo grande degrau é a UI da TASKBAR. Comece por UxTheme (DLL real, documentada,
resolve ~25 imports), depois ataque dui70/DirectUI e a criação de janela (RegisterClassExW/
CreateWindowExW no user32). Use os gates (REG_TRACE/COMBASE_DBG) e o disassembly p/ ver onde o
explorer para e por quê. Vá, sem parar, até ~600k.**
