# PROMPT — próxima sessão: continuar rodando o `explorer.exe` REAL do Windows 10

Cole isto como prompt inicial. É AUTOSSUFICIENTE — comece a trabalhar imediatamente.

---

## ⚙️ MODO DE TRABALHO (o mais importante — LEIA ISTO)

**Trabalhe de forma AUTÔNOMA. NÃO pare para me perguntar "continua?" nem "quer que eu siga?".**
Rode o loop de bring-up do explorer real, implementando **um muro por vez, de verdade**, sem parar,
**até usar ~600k de tokens de contexto**. Só então: escreva um novo `PROMPT-PROXIMA-SESSAO.md`
atualizado, faça o commit final, e me dê o balanço. Antes disso, siga implementando e commitando
sozinho.

Disciplina a cada muro/lote: **build → rodar o explorer → `python nextwall.py` → implementar a função
REAL → build → regressão do pintok (SE mexeu no kernel `src/ntos/...`) → commit + push**. Commite por
fase (a cada 3–6 muros ou quando fechar uma DLL). Mantenha o pintok VERDE sempre. Rode builds/QEMU em
background (o build leva ~1 min). **NUNCA um stub genérico catch-all** — só stubs ESPECÍFICOS e
nomeados onde a função genuinamente é no-op aqui.

## 🎯 A MISSÃO
Rodar o **binário REAL** `C:\Windows\explorer.exe` da Microsoft no MeuOS (SO estilo NT do zero,
`zig cc` + NASM, QEMU). NÃO escrever um explorer do zero. Detalhes: `RECON-EXPLORER.md` + a memória
`run-real-explorer-mission`.

## 📍 ONDE O EXPLORER ESTÁ AGORA (MUITO avançado — leia com atenção)
**Ele resolve TODOS os imports** (regulares + **delay-load** + por **ordinal**) e **executa a
inicialização PROFUNDA da shell**. O crash não é mais "import faltando" — agora é **lógica interna
de COM**: o explorer chama `CoCreateInstance` (na nossa `combase.dll` → hoje `E_NOTIMPL`) e
**desreferencia o objeto NULL** (`mov rdx,[rax+8]` com `rax=0`, cr2=0x8).

⇒ **A PRÓXIMA FRONTEIRA É OBJETOS COM REAIS.** A `combase.dll` precisa de `CoCreateInstance`/
`CoGetClassObject` que devolvam **objetos de verdade com vtables** (no mínimo `IUnknown`:
QueryInterface/AddRef/Release), e provavelmente `IClassFactory`, e aos poucos `IShellFolder`,
`IShellItem`, etc. Isto é o **coração do shell** (fase longa). Estratégia sugerida: comece por um
`IUnknown`/`IClassFactory` genérico cujo `QueryInterface` devolve o próprio objeto e cujos métodos
devolvem `E_NOTIMPL` — assim o explorer recebe um ponteiro NÃO-NULO, lê a vtable, chama um método
(recebe E_NOTIMPL) e **degrada em vez de derefar NULL**. Depois torne reais as interfaces que o loop
apontar. Cuidado: cada `CLSID`/`IID` pode querer uma vtable específica.

Muros pequenos também aparecem intercalados (ex.: agora o próximo é
`api-ms-win-storage-exports-internal-l1-1-0.dll!SetThreadFlags` → precisa de redirect+impl; é um
no-op trivial). Trate-os pelo loop normal.

## 🔁 O LOOP (a ferramenta que torna tudo automático)
1. Rodar o explorer real (**conjunto de 12 módulos que FUNCIONA** — o boot aguenta ~16; NÃO
   adicione d3d11/d3d12/d2d1/dwrite/dxcore, o explorer não os importa, e passar de ~16 módulos
   trava o boot ANTES do `ldr_run`):
   ```
   .\run.ps1 -Modules build\ntdll.dll,build\kernel32.dll,build\user32.dll,build\gdi32.dll,build\advapi32.dll,build\ucrtbase.dll,build\combase.dll,build\msvcp_win.dll,build\shell32.dll,build\shcore.dll,build\dxgi.dll,build\explorerreal.exe -Headless -TimeoutSec 45
   ```
   (Se `build\explorerreal.exe` sumiu: `cp C:\Windows\explorer.exe build\explorerreal.exe`.)
2. Achar o próximo muro:
   ```
   python nextwall.py       # nomeia a funcao do import NULO (rip=0). Le build\serial.log.
   ```
   - Se disser "Sem caller de bring-up": o crash NÃO é import nulo (é #UD/#GP/#PF de lógica). Veja
     `[bringup] excecao em RING-3: rip=... stack[rsp..]=...` no serial.log e desmonte com
     `python disat.py 0x<rip>` e `python disat.py 0x<endereco da pilha>`.
   - Se nextwall imprimir `?? 0x<slot>`: é import por **ORDINAL** ou **delay-load** (nextwall só
     nomeia por-nome). Parse a import/delay table do explorer p/ achar o `DLL!#ord` (o loader já
     resolve ordinais e delay-load — o muro é falta de **redirect** ou de **impl** no host).
3. Implementar a função **de verdade** na DLL/camada certa. Rebuild. Repetir.

## 🧭 ONDE IMPLEMENTAR (redirects em `src/ntos/ldr/loader.c` `apiset_redirect`)
- `api-ms-win-crt-*` → `ucrtbase` · `api-ms-win-core-com*` → `combase` · `api-ms-win-core-registry`
  → `advapi32` · `api-ms-win-security-*`/`eventing-*` → `advapi32` · `api-ms-win-core-*` (resto) →
  `kernel32` · `*ntuser*` → `user32` · `api-ms-win-shell-*` → `shell32` · `api-ms-win-shcore-*` →
  `shcore`.
- **Extension API sets** `ext-ms-win-*` (delay-load): `rtcore-ntuser`/`ntuser`/`session-winsta` →
  `user32`; `gdi` → `gdi32`; `shell32`/`shell` → `shell32`; `security` → `advapi32`; `core` →
  `kernel32`. **Família nova** (ex.: `api-ms-win-storage-*`) → adicionar um redirect + impl no host.
- **DLL direta nova** (propsys/oleaut32/rpcrt4/dwmapi/uxtheme...): criar `dll/win32/<nome>/<nome>.c`
  (+ `.def` se tiver ordinais/nomes mangled), bloco no `build.ps1` (ImageBase livre ≥ 0x5900000 —
  o loader RELOCA sozinho, é PMM-safe), e adicionar ao `-Modules` (respeitando o limite de ~16).

## 🛠️ FERRAMENTAS DO LOOP (na raiz, versionadas)
- `nextwall.py` — nomeia o import nulo (rip=0) desmontando o explorer no caller.
- `disat.py <hexaddr>...` — desmonta o explorer em endereços (crash/retorno), nomeia CALL [IAT].
- `gap.py <dll-substr> <build\alvo.dll>` — o que o explorer importa da DLL e o alvo NÃO exporta.
- `redirgap.py [dll]` — aplica os redirects e agrupa o gap por DLL implementadora (visão geral).
- `dumpimports.py [dll]` / `dumpexports.py <pe>` — import/export tables.
- `gen_msvcp.py` — regenera msvcp_win (padrão de gerador com `.def` p/ nomes mangled + ordinais).

## 🔨 O QUE JÁ ESTÁ PRONTO (gap 0 onde indicado)
- **Kernel (loader)**: mapeia/reloca o explorer (5,8 MB); **imports por NOME, ORDINAL e DELAY-LOAD**
  (`pe_get_export_by_ordinal`, `.didat` eager em `pe_bind_imports`); DLLs de base alta (≥64 MiB)
  vão p/ frames RESERVADOS do PMM + reloc (fix da colisão que corrompia msvcp/combase); delay não
  resolvido → `ntdll!LdrpNullStub` (no-op); diagnóstico de #UD/#GP/#PF de ring-3 no `isr.c`.
- **DLLs (gap 0)**: `ntdll` (Rtl/heap/strings/Zw/versao/SRW/WNF/Sqm + LdrpNullStub), `ucrtbase`
  (CRT + printf/scanf WIDE), `kernel32` (248/248 — threadpool, InitOnce c/ CONTEXTO, processo/
  thread, arquivo, NLS, path, recurso, mapeamento, Global/Local, job/actctx/power), `advapi32`
  (ETW controller + seguranca SID/ACL/token single-user + SHReg), `shell32` (IL* reais + namespace/
  ShellExecute/NotifyIcon + ordinais), `shcore` (DPI/stream/registro/thread/appid), `user32` (base +
  140 GUI: RECT/metrics/DPI/monitor/menu/icon/input/hook), `dxgi` (+DeclareAdapterRemovalSupport).
- **combase**: só `CoTaskMemAlloc` + `Co*` que devolvem `E_NOTIMPL`. ← **É AQUI QUE VOCÊ MEXE AGORA.**

## ⛔ REGRA DE OURO — pintok.sys (Riot Vanguard). NÃO QUEBRAR.
Depois de CADA incremento que mexa no KERNEL (`src/ntos/...`), rode:
```
.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40
```
Baseline dourada em `build\serial.log`: `[P1]/[P2]/[P3] ==== PROVA PASSOU ====`; `[intercept] CPUID
... Intel i7-9700K` (x3); `[io] intercept totals: CPUID x3 RDTSC x33 RDMSR x0 ANTIVM x0`;
`DriverEntry retornou status=0x00000000C0000365`; **SEM** "Sistema parado". (Mudancas so em DLLs de
userland NAO afetam o pintok — o cenario nem as carrega. Os caminhos de loader que mexi sao userland;
o pintok e ring-0 com resolucao separada — por isso continua verde.) Syscalls novos: **append no FIM**
do enum + do `s_ssdt[]` em `src/ntos/ke/amd64/syscall.c` (ultimo foi `SYS_VIRTUALALLOC=50`; proximo
livre 51).

## 📜 COMMITS DESTA SESSÃO (todos pintok-verde)
`5dd56f4` msvcp_win + fix colisao PMM + ntdll · `ceed2f9` kernel32/ucrtbase FECHADOS + dxgi ·
`2acb5eb` imports por ORDINAL + shell32 + shcore + advapi32 · `34f22a0` delay-load + ext-ms +
user32 GUI + InitOnce (contexto). Mensagens terminam com `Co-Authored-By: Claude Fable 5
<noreply@anthropic.com>`; branch `feat/kernel-foundation-irql-dpc`; `git push` a cada lote.

## 📌 NOTAS
- Boot aguenta ~16 modulos Multiboot; passar disso trava ANTES do `ldr_run` do explorer (o explorer
  e o ultimo modulo). Mantenha o set de 12. Se precisar de +DLLs, pode ser necessario investigar o
  limite (nao achei um cap explicito de 16 em `main.c`; parece layout de memoria do initrd).
- Heap: bump backado por VirtualAlloc (sem free real). Suficiente.
- COM/threads: apartamentos e threads de ring-3 ainda no-op/pseudo (corretos single-threaded). Viram
  reais quando o shell precisar (a fase COM vai forcar objetos reais; threads talvez depois).

**Agora: rode o explorer (12 modulos), ache o muro, e siga o loop. O grande degrau e `combase` com
objetos COM REAIS (class factory + vtables). Va, sem parar para perguntar, ate ~600k de contexto.**
