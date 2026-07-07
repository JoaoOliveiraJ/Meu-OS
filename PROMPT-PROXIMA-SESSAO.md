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

## 🎯 A MISSÃO
Rodar o **binário REAL** `C:\Windows\explorer.exe` no MeuOS. NÃO escrever um explorer.
Detalhes: `RECON-EXPLORER.md` + memória `run-real-explorer-mission`.

## 📍 ESTADO — MARCO: o explorer NÃO CRASHA na init; encerra limpo no MRT
Carrega, resolve TODOS os imports, roda a init COMPLETA de COM + WinRT em ring-3 SEM
exceção, e encerra **LIMPO** via `ExitProcess(0)` (`[ps] pid=1 encerrou status=0x0`) ANTES
de criar janela. Memória: `explorer-com-winrt-clean-mrt-frontier`.

## ⇒ A PRÓXIMA FRONTEIRA = SISTEMA DE RECURSOS **MRT** (WinRT ResourceManager)
O explorer EXIGE o `Windows.ApplicationModel.Resources.Core.ResourceManager` p/ carregar
recursos de UI. **RE já feito** (com o SCAFFOLD abaixo) mapeou a sequência exata:
1. `CoCreateInstance {0000034B}`/`{0000015B}` = CLSID_GlobalOptions/IGlobalOptions (benigno, slots 3/4 → E_NOTIMPL ok).
2. `GetDC` (DPI ok) → `LoadLibrary("comctl32.dll")` falha (data-driven, **não-fatal**).
3. `CoCreateInstance {660B90C8-73A9-4B58-8CAE-355B7F55341B}`/`{BA5A92AE-...}` (classe COM de MRT).
4. `RoGetActivationFactory(ResourceManager)` factory-iid `{4A8EAC58-B652-459D-8DE1-239471E8B22B}`
   → o explorer chama a **factory slot 7** (get da ResourceManager). Devolvendo um objeto,
   ele chama **manager slot 6** (get MainResourceMap ~ `get_MainResourceMap` público é slot 6),
   depois **map slots 8 e 9**, e então **CRASHA em rip=0x464C89B** (RVA 0x33389B) num
   `mov rcx,[rdi+0x30]; mov rax,[rcx]` — a interface em `rdi+0x30` ficou LIXO porque o
   scaffold devolve objetos FALSOS (não seta os out-params certos). ⇒ precisa de objetos
   MRT REAIS (ResourceManager/ResourceMap/ResourceContext com métodos e DADOS reais).
   Import genuíno descoberto no caminho e JÁ IMPLEMENTADO: `SspiCli!GetUserNameExW` (advapi32).

⚠️ A factory-iid `{4A8EAC58}` NÃO está no SDK público (o header ABI
`Windows Kits/10/Include/10.0.26100.0/winrt/windows.applicationmodel.resources.core.h` tem
`IResourceManagerStatics`{get_Current slot6, IsResourceReference slot7} e `IResourceManager`
{`get_MainResourceMap` slot6, AllResourceMaps 7, DefaultContext 8, LoadPriFiles 9}). Logo o
explorer usa uma interface PRIVADA (windows.internal.*) — layout parecido mas confirmar por RE.

### Como atacar MRT (a fase longa)
- **SCAFFOLD de RE (já no código, gated)**: em `dll/win32/combase/combase.c` ponha
  `#define COMBASE_DBG 1`. Isso (a) loga CLSID/IID/classe-WinRT/slot na serial via int 0x80,
  e (b) faz `RoGetActivationFactory`/`RoActivateInstance` devolverem o objeto universal e os
  getters (slots 6-19) devolverem objetos — empurrando o explorer ALÉM do MRT p/ mapear os
  próximos muros. É DIAGNÓSTICO (objetos falsos → crasha ~4 slots adiante). Deixe em 0 no commit.
- **Implementação REAL**: criar objetos MRT de verdade em combase (ou numa dll `mrtcore`):
  `IResourceManager` real cujo `get_MainResourceMap` devolve um `IResourceMap` real; o map
  responde lookups. Provável necessidade de DADOS reais (o explorer busca recursos ESPECÍFICOS
  — um map vazio devolve "não encontrado" e ele pode sair/quebrar num recurso crítico). Talvez
  servir recursos do `.rsrc`/`.mui` clássico do próprio explorer. Iterar slot por slot com o RE.

## 🔁 O LOOP
1. Rodar o explorer (**12 módulos que FUNCIONAM** — NÃO passe de ~16, trava o boot):
   ```
   .\run.ps1 -Modules build\ntdll.dll,build\kernel32.dll,build\user32.dll,build\gdi32.dll,build\advapi32.dll,build\ucrtbase.dll,build\combase.dll,build\msvcp_win.dll,build\shell32.dll,build\shcore.dll,build\dxgi.dll,build\explorerreal.exe -Headless -TimeoutSec 45
   ```
   (Se `build\explorerreal.exe` sumiu: `cp C:\Windows\explorer.exe build\explorerreal.exe`.)
2. Diagnosticar:
   - `python nextwall.py` — nomeia o import NULO (rip=0). "Sem caller" = crash de lógica
     (veja `[bringup] excecao em RING-3: rip=...`) ou saída limpa (MRT).
   - `python disat.py 0x<rip>` — desmonta o explorer no crash. `python strxref.py "<str>"` — xref de string.
   - Ative `COMBASE_DBG 1` p/ ver a dança de COM/WinRT (CLSID/IID/slot) na serial.
3. Implementar a função/interface REAL. Rebuild. Repetir.

## 🧭 REDIRECTS (`src/ntos/ldr/loader.c` `apiset_redirect`)
`api-ms-win-crt-*`→ucrtbase · `core-com*`→combase · `core-winrt*`→combase · `core-registry`→advapi32
· `security-*`/`eventing-*`→advapi32 · `core-*`→kernel32 · `*ntuser*`→user32 · `shell-*`→shell32
· `shcore-*`→shcore · `storage-*`→shell32 · `ext-ms-win-*`→(user32/gdi32/shell32/advapi32/kernel32)
· diretas: `userenv`→advapi32, `sspicli`→advapi32. **DLL direta nova**: criar
`dll/win32/<nome>/<nome>.c` (+`.def` se ordinais/mangled), bloco no `build.ps1` (ImageBase livre
≥0x5900000, o loader RELOCA — PMM-safe) + `-Modules` (≤~16); OU redirecionar p/ host existente.

## 🛠️ FERRAMENTAS (raiz, versionadas)
`nextwall.py` (import nulo) · `disat.py <hex>...` (desmonta) · `strxref.py <substr>` (xref
string ASCII/UTF-16 → call-sites) · `redirgap.py [dll]`/`gap.py` (gap por DLL, espelha o loader)
· `dumpimports.py`/`dumpexports.py` · `gen_msvcp.py` (gerador c/ .def).

## 🔨 PRONTO (gap 0)
Kernel loader (NOME/ORDINAL/DELAY-LOAD, PMM+reloc, redirects, diag ring-3). DLLs: ntdll,
ucrtbase, kernel32(248), user32(140 GUI), gdi32(+GetDeviceCaps), advapi32(seg+SHReg+GetProfileType+
DeriveAppContainerSid+**GetUserNameExW**), shell32(IL*+SHGetFolderPathEx/KnownFolderIDList+Get/Set
ThreadFlags), shcore(DPI+IUnknown_*), msvcp_win(97), dxgi. **combase(35 COM + 22 WinRT)**: IMalloc,
IStream real, GUID/string, objeto UNIVERSAL (CoCreateInstance degrada), marshaling identidade,
HSTRING real, Ro*(ativação→REGDB_E_CLASSNOTREG honesto), winrt-error. SCAFFOLD de RE do MRT gated.

## ⛔ REGRA DE OURO — pintok.sys. NÃO QUEBRAR.
Após CADA incremento no KERNEL (`src/ntos/...`): `.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`.
Dourado: `[P1]/[P2]/[P3] ==== PROVA PASSOU ====`; `intercept totals: CPUID x3 RDTSC x33 RDMSR x0
ANTIVM x0`; `DriverEntry ... C0000365`; SEM "Sistema parado". DLLs de userland NÃO afetam o pintok.
Syscalls novos: **append no FIM** do enum + `s_ssdt[]` em `src/ntos/ke/amd64/syscall.c` (último
`SYS_VIRTUALALLOC=50`; próximo 51).

## 📜 COMMITS DESTA SESSÃO (todos pintok-verde)
`2b84e88` lote 1 (combase COM real + gdi GetDeviceCaps + storage + userenv) · `e07b335` lote 2
(WinRT na combase; explorer roda init COM+WinRT sem crashar) · `ea04818` lote 3 (ativação WinRT
honesta REGDB + COMBASE_DBG gated) · lote 4 (SspiCli!GetUserNameExW + RE do MRT documentado).
Mensagens terminam com `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`; branch
`feat/kernel-foundation-irql-dpc`; `git push` a cada lote.

## 📌 NOTAS
- Boot aguenta ~16 módulos. Mantenha o set de 12.
- SDK do Windows na máquina: `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\`
  (headers ABI winrt em `.../winrt/`). MRT usa interfaces privadas (não no SDK público).
- Heap combase: arena bump 2 MiB (sem free). combase autocontida (int 0x80 direto p/ log).

**Agora: o próximo grande degrau é MRT REAL (ResourceManager/ResourceMap com dados). Use o
SCAFFOLD (`COMBASE_DBG 1`) p/ mapear os slots e muros, implemente objetos MRT de verdade slot
por slot, e veja o explorer avançar rumo à criação de janela. Vá, sem parar, até ~600k.**
