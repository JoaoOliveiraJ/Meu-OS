# RECON-EXPLORER.md — rodar o `explorer.exe` REAL do Windows 10 no MeuOS

> Objetivo (norte da GUI): **carregar e rodar o binário real da Microsoft**
> `C:\Windows\explorer.exe`, pelas mecânicas reais do NT — não um shell caseiro.
> Este doc é o mapa de degraus, baseado em EVIDÊNCIA (diagnóstico de 2026-07-07).

## O alvo (medido no binário real, 5,8 MB)
- Importa **1066 funções de 136 DLLs**. Base moderna: **WinRT + COM + DWM + shell
  namespace + ETW**. É uma escalada longa (estilo ReactOS/Wine).

## O que JÁ funciona (provado — degrau 0)
- Copiamos o real p/ `build\explorerreal.exe` e mandamos o kernel carregar
  (`.\run.ps1 -Modules ...,build\explorerreal.exe -Headless`).
- O loader do MeuOS (`ldr_run` → `pe_map`) **MAPEIA e RELOCA o explorer inteiro** sem
  crashar: `EPROCESS pid=1 img=explorerreal.exe base=0x0000000004319000`. → o nosso PE
  loader aguenta um binário moderno grande.
- **Caminha a import table inteira** (`pe_bind_imports`), reportando cada import.
- Já existe um **resolvedor de API Set embrionário**: `src/ntos/ldr/loader.c:111`
  redireciona `api-ms-win-crt-*` → `ucrtbase.dll`. É o padrão a estender.

## Onde ele PARA (o primeiro muro)
- Os imports **não resolvem** — faltam as DLLs implementadoras (**619** `[ldr] DLL nao
  registrada` / `import nao resolvido` no log). Não crashou; ficou **preso logando**
  (não terminou de bindar em 25s — a serial é lenta).
- Ranking do que falta, por nº de funções (empírico == estático):

  | Implementador | funções | temos? |
  |---|---|---|
  | `msvcp_win` (STL C++) | 97 | não |
  | `kernelbase` (via `api-ms-win-core-*`) | ~243 | não (temos `kernel32` stub) |
  | `shell32` (+ `api-ms-win-shell-*`) | ~91 | não |
  | COM/`combase`/WinRT (`api-ms-win-core-com`/`winrt`) | ~57 | não |
  | `shlwapi` | ~54 | não |
  | `shcore` | ~53 | não |
  | `uxtheme` | 28 | não |
  | `security` (`api-ms-win-security-*`) | 21 | parcial (`advapi32`) |
  | ETW (`api-ms-win-eventing-*`) | 17 | não (stub no-op serve) |
  | `dwmapi` | 16 | não |
  | `oleaut32` 11, `propsys` 9, `rpcrt4` 8 | | não |
  | temos stub: `user32`(150)/`gdi32`(28)/`ntdll`(73)/`ucrtbase`(82)/`kernel32` | ~334 | sim (finos) |

## Pintok-safe (regra de ouro)
- `ldr_resolve`/`pe_bind_imports` são o caminho de **USERLAND** (`.exe`/`.dll`, ring-3).
  O `pintok.sys` é **ring-0**, carregado por `driver_load` com resolução SEPARADA
  (stub genérico do `ntoskrnl`/`g_ntexports`). Mexer no loader userland **não toca** no
  pintok. (Confirmado: baseline dourada C0000365 intacta durante todo o diagnóstico.)

## A escada recomendada (SEM stub genérico — tudo implementado DE VERDADE)
- **Degrau 1 — redirects de API Set p/ as NOSSAS DLLs REAIS + medir o gap honesto:**
  estender o redirect (`loader.c`) das famílias `api-ms-win-*` p/ as DLLs reais que já
  temos (crt→ucrtbase, core→kernel32, *ntuser*→user32, security→advapi32). Isso resolve
  as funções que JÁ implementamos (de verdade) e deixa o resto NÃO resolvido — honesto:
  a lista de "nao resolvido" vira exatamente o que falta implementar. **Sem catch-all.**
- **Degrau 2+ — implementar as funções/DLLs que faltam, DE VERDADE, DLL por DLL**
  (kernelbase, msvcp_win, combase, shell32, shlwapi, shcore, uxtheme, propsys, oleaut32,
  rpcrt4, dwmapi…), na ordem que o explorer exige, rodando e vendo onde ele para:
  `kernelbase` (heap/synch/registry/processthreads/memory), `ntdll` (Rtl/heap/SRW/SEH
  unwind), `user32`+`gdi32` reais (janela + não-cliente `WM_NC*` + fonte/texto real),
  `combase` (CoInitialize/apartments/class factory), **registro real**, `shell32`
  namespace, `uxtheme`/`dwm`. Funções genuinamente no-op (ETW/telemetria/WER) viram
  stubs ESPECÍFICOS e nomeados na DLL certa — **nunca** um catch-all genérico.
- **Muros duros conhecidos:** WinRT (ativação/`CoreMessaging`/`twinapi`) e DWM.

## Como reproduzir o diagnóstico
```
cp C:\Windows\explorer.exe build\explorerreal.exe
.\run.ps1 -Modules @('build\ntdll.dll','build\kernel32.dll','build\user32.dll',`
  'build\gdi32.dll','build\advapi32.dll','build\ucrtbase.dll','build\explorerreal.exe') `
  -Headless -TimeoutSec 25
# ver build\serial.log:  "[ldr] DLL nao registrada" / "import nao resolvido"
```
