# PROMPT — próxima sessão: continuar rodando o `explorer.exe` REAL do Windows 10

Cole isto como prompt inicial. É AUTOSSUFICIENTE — comece a trabalhar imediatamente.

---

## ⚙️ MODO DE TRABALHO (o mais importante — LEIA ISTO)

**Trabalhe de forma AUTÔNOMA. NÃO pare para me perguntar "continua?" nem "quer que eu siga?".**
Rode o loop de bring-up do explorer real, implementando **um muro por vez, de verdade**, sem parar,
**até usar ~600k de tokens de contexto** (~60% de 1M). Só então: escreva um novo
`PROMPT-PROXIMA-SESSAO.md` atualizado, faça o commit final, e me dê o balanço. Antes disso, siga
implementando e commitando sozinho. Eu quero ver o explorer avançar, não perguntas.

Disciplina a cada muro/lote: **build → rodar o explorer → `nextwall.py` → implementar a função REAL
→ build → regressão do pintok (se mexeu no kernel) → commit + push**. Commite por fase (a cada 3–6
muros ou quando fechar uma DLL), não a cada função. Mantenha o pintok VERDE sempre.

---

## 🎯 A MISSÃO
Rodar o **binário REAL** `C:\Windows\explorer.exe` da Microsoft no MeuOS (SO estilo NT feito do zero,
C via `zig cc` + NASM, QEMU). NÃO escrever um explorer do zero. Rodar o real é um **driver de
conformidade do SO inteiro**: cada função que falta revela uma lacuna em usermode (DLL), kernel
(syscall) ou HAL — e a gente implementa **de verdade**. **NUNCA um stub genérico catch-all.** Só
stubs específicos e nomeados onde a função genuinamente não faz nada aqui (ETW/telemetria,
apartamentos COM). Detalhes e a escada: `RECON-EXPLORER.md` (raiz) + a memória do projeto
`run-real-explorer-mission`.

## 📍 ONDE O EXPLORER ESTÁ AGORA (ponto mais avançado)
Ele já MAPEIA/RELOCA (5,8 MB), entra em ring-3, binda os imports e **executa ~2 MB dentro do próprio
código**, COM UM HEAP REAL, atravessando:
`SEH → cookie → init do CRT (~60 fns UCRT) → SList → startup → synch → COM (CoTaskMemAlloc) →
registro (Reg*) → HEAP REAL (VirtualAlloc) → mem* → versão do SO → ETW → construtores C++ estáticos`.
**Próximo muro: `msvcp_win.dll!_Mtx_init_in_situ`** (a STL do C++).

## 🔁 O LOOP (a ferramenta que torna tudo automático)
1. Rodar o explorer real:
   ```
   .\run.ps1 -Modules @('build\ntdll.dll','build\kernel32.dll','build\user32.dll','build\gdi32.dll','build\advapi32.dll','build\ucrtbase.dll','build\combase.dll','build\explorerreal.exe') -Headless -TimeoutSec 40
   ```
   (Se `build\explorerreal.exe` sumiu: `cp C:\Windows\explorer.exe build\explorerreal.exe`.)
2. Achar o próximo muro (nomeia a função exata que falta):
   ```
   python nextwall.py
   ```
   Ele lê `build\serial.log`, pega o `[bringup] caller[rsp]=0x...` que o KERNEL loga na falha `rip=0`
   (import não resolvido → IAT=0 → CALL[0]), e desmonta o explorer nesse endereço.
   O diagnóstico do kernel está em `src/ntos/ke/amd64/isr.c` (caminho de halt do #PF; pintok-safe).
3. Implementar a função **de verdade** na DLL/camada certa (ver "onde implementar" abaixo).
4. `.\build.ps1` → rodar o explorer de novo → `python nextwall.py` → repetir.

## 🔨 PRÓXIMO TRABALHO IMEDIATO — a STL do C++ (`msvcp_win.dll`)
`msvcp_win` é import DIRETO (não `api-ms-win-*`), então: **crie a DLL + adicione ao build + inclua no
`-Modules`** (não precisa de redirect no loader). O explorer importa **97 funções** dela.
- Criar `dll/win32/msvcp_win/msvcp_win.c` (precisa de `unsigned int _tls_index = 0;` como as outras).
- `build.ps1`: bloco de build (ImageBase livre, ex.: `0x5600000` — depois da combase 0x5500000);
  siga o padrão da combase (procure "combase.dll" no build.ps1).
- Adicionar `build\msvcp_win.dll` ao `-Modules` do comando do explorer (e no `nextwall.py` a base do
  explorer pode mudar; ele já lê a base do log).
- Dump das 97 funções que o explorer pede da msvcp_win (para saber o que implementar): rode um Python
  que parseia a import table do `C:\Windows\explorer.exe` filtrando `dll=='msvcp_win.dll'` (o padrão
  do parser está em `nextwall.py` e em `RECON-EXPLORER.md`).
- Implementar: `_Mtx_init_in_situ/_Mtx_lock/_Mtx_unlock/_Mtx_destroy_in_situ` (mutex STL → no-op
  correto single-threaded), `_Xlength_error/_Xout_of_range/_Xbad_alloc` (→ terminate/ExitProcess),
  e os internos de `std::string`/`locale`/`iostream` que aparecerem. Deixe os que não forem chamados
  na init como stubs específicos e vá tornando reais conforme o loop apontar.

**Depois da STL:** o **shell COM profundo** — `shell32`/`IShellFolder`/class factories via
`CoCreateInstance` com OBJETOS REAIS (a `combase.dll` hoje só tem CoTaskMemAlloc + Co* que retornam
E_NOTIMPL). Essa é a fase longa (o coração do shell). Provavelmente vai precisar de: uma shell32.dll,
objetos COM de verdade (vtables), e talvez propsys/oleaut32.

## 🧭 ONDE IMPLEMENTAR (o redirect de API Set já existe em `src/ntos/ldr/loader.c` `apiset_redirect`)
- `api-ms-win-crt-*` → `ucrtbase.dll` · `api-ms-win-core-com*` → `combase.dll`
- `api-ms-win-core-registry` → `advapi32.dll` · `api-ms-win-security-*` → `advapi32.dll`
- `api-ms-win-eventing-*` → `advapi32.dll` (ETW no-op) · `api-ms-win-*ntuser*` → `user32.dll`
- `api-ms-win-core-*` (resto) → `kernel32.dll` (o nosso "kernelbase")
- DLLs diretas (msvcp_win, shell32, shlwapi, shcore, propsys, oleaut32, rpcrt4, uxtheme, dwmapi...):
  criar a DLL + build + `-Modules`. Se for `api-ms-win-*` de família nova, adicionar um redirect.

## ⛔ REGRA DE OURO — pintok.sys (Riot Vanguard). NÃO QUEBRAR.
Depois de CADA incremento que mexa no KERNEL (`src/ntos/...`), rode:
```
.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40
```
e confira em `build\serial.log` a baseline dourada: `[P1]/[P2]/[P3] ==== PROVA PASSOU ====`;
`[intercept] CPUID ... Intel i7-9700K` (x3); `[io] intercept totals: CPUID x3 RDTSC x33 RDMSR x0
ANTIVM x0`; `[io] DriverEntry retornou status=0x00000000C0000365`; **SEM** "Sistema parado".
(Mudanças só em DLLs de userland NÃO afetam o pintok — o cenário pintok nem as carrega — mas rode
mesmo assim se mexeu no kernel.) Syscalls novos: **append no FIM** do enum + do `s_ssdt[]` em
`src/ntos/ke/amd64/syscall.c` (pintok é ring-0, não usa SSDT → pintok-safe). O último syscall foi
`SYS_VIRTUALALLOC=50`; o próximo livre é 51.

## 🛠️ COMANDOS
- Build: `.\build.ps1` (~1–2 min, rode em background).
- Explorer (loop): o comando `-Modules` acima + `python nextwall.py`.
- Desktop (prova visual, não regride): `.\run.ps1 -Scenario desktop -Screendump -TimeoutSec 20`
  → `build\screen.ppm` (PPM→PNG: `python -c "from PIL import Image; Image.open('build/screen.ppm').save('build/screen.png')"`).
- Pintok (regressão): `.\run.ps1 -Scenario pintok -Headless -TimeoutSec 40`.
- Commit: mensagens terminam com `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`; branch
  atual `feat/kernel-foundation-irql-dpc`; `git push` a cada lote.

## 📜 O QUE JÁ FOI FEITO (commits desta sessão, todos pintok-verde)
`5751909` cenários no run.ps1 (boot limpo `-Scenario desktop|pintok`) · `68e4890` RECON-EXPLORER.md +
diagnóstico degrau 0 · `6d2908b` redirect de API Set p/ DLLs reais · `5aa829d` kernelbase lote 1
(explorer entra em ring-3) · `dd7df9f` cookie/entry + **diagnóstico de bring-up no isr.c** · `91b6d52`
prólogo do CRT (UCRT+SList+startup) · `31ebf6a` synch + **combase (COM)** + entrada no registro ·
`3a4cc2e` registro (Reg* wide) + **HEAP REAL (SYS_VIRTUALALLOC)** + mem* + versão + ETW.

Arquivos-chave tocados: `dll/win32/{kernel32,ucrtbase,advapi32,combase}/*.c`, `dll/ntdll/ntdll.c`,
`src/ntos/ke/amd64/syscall.c`, `src/ntos/ldr/loader.c`, `src/ntos/ke/amd64/isr.c` (diagnóstico),
`build.ps1` (combase), `run.ps1` (cenários), `nextwall.py` (ferramenta do loop), `RECON-EXPLORER.md`.

## 📌 NOTAS/PENDÊNCIAS
- Heap: hoje é bump com header backado por VirtualAlloc (sem free real; `HeapFree` é no-op). Suficiente
  por ora; free-list real depois se esgotar.
- Objetos de synch e apartamentos COM: pseudo-handles / no-op (corretos single-threaded); viram reais
  quando houver threads de ring-3.
- Frentes A/E ainda pendentes (menor prioridade que o explorer): gate dos self-tests (`g_run_selftests`
  em `main.c`), tabela de módulos no build.ps1, remover entulho morto (`pintok.sys` de 43 MB na raiz,
  `src/ke/` vazio, etc.). Tarefas #3, #5, #6.

**Agora: recrie/pegue o `nextwall.py`, rode o explorer, e siga o loop implementando a `msvcp_win`
(STL) e além, SEM parar para perguntar, até ~600k de contexto. Vá.**
