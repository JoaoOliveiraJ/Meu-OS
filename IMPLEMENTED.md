# IMPLEMENTED — registro do que foi implementado por tarefa

## Resumo da rodada (consolidado)

Esta rodada entregou tres blocos, todos com **build verde e boot verde** (nada
revertido):

| Tarefa | Tema | Status |
|--------|------|--------|
| 1 | **Process Manager + espaco de enderecamento** (EPROCESS/ETHREAD como objetos, PML4 por processo + troca de CR3, `NtCreateProcess`/`NtCreateThread`/`NtTerminateProcess`/`NtWaitForSingleObject`, loader cria EPROCESS por `.exe`) | **Funciona** (isolamento de memoria **parcial**: PML4 copia o mapa do kernel; wait nao bloqueia) |
| 2 | **Loader PE32 (32-bit) + relocacoes + execucao em compat mode** (`pe_parse`/`pe_relocate`, `SEL_UCODE32`, `usermode_enter32`, `int 0x80` de 32 bits) | **Funciona** (sem ntdll/kernel32/user32 de 32 bits ainda: PE32 com imports Win32 = limitacao futura) |
| 3 | **Console I/O + mais syscalls/DLLs** (`NtWriteFile`/`NtReadFile` + "console device", `NtOpenKey`/`NtQueryValueKey` stubs, `GetModuleHandle`/`GetProcAddress` apoiados no loader real, IRP_MJ_WRITE/READ) | **Funciona** (registro = stub; `NtReadFile` no console le 0 bytes; caminho IRP de write/read pronto mas so o console e exercitado no boot) |

**Evidencia final consolidada** (`build.ps1 -Clean` + `run.ps1 -Headless
-TimeoutSec 8`, todos os modulos no MESMO boot; `qemu.err.log` vazio; 0
`syscall desconhecida`, 0 `import nao resolvido`, 0 `[EXCECAO]`/triple-fault):

```
[ok] Process Manager (EPROCESS/ETHREAD via Object Manager)
[boot] driver de kernel: ioctldriver.sys ... DriverEntry retornou status=0x0 (STATUS_SUCCESS)
[boot] driver de kernel: mydriver.sys     ... DriverEntry retornou status=0x0 (STATUS_SUCCESS)
[boot] aplicativo: test.exe
[ps] EPROCESS criado: pid=1 img=test.exe base=0x800000 cr3=0x10C000
[mm] PML4 por-processo @ 0x4003000 (kernel CR3 0x10C000)
[ps] EPROCESS criado: pid=2 img=worker.exe ... cr3=0x4003000   <- NtCreateProcess de DENTRO do ring 3, sob a PML4 do pai
  | Titulo: MeuOS  -  Pinball (teste do loader PE)              <- test.exe (64-bit) em ring 3 OK
[boot] aplicativo: ioctlapp.exe (pid=3)
  [DbgPrint] IoctlDrv: respondeu 0xCAFEBABE
  | Texto : 0xCAFEBABE                                          <- IOCTL 0xCAFEBABE ponta a ponta OK
[boot] aplicativo: conhello.exe (pid=4)
  [conhello] Ola do RING 3 via GetStdHandle + WriteFile!        <- WriteFile -> NtWriteFile -> console device
  [conhello] GetProcAddress(user32, MessageBoxA) OK.            <- GetModuleHandle/GetProcAddress via loader real
  | Texto : Chamado via GetProcAddress!
[boot] aplicativo: test32.exe
[ldr] PE32 (32-bit) detectado: machine=0x14C magic=0x10B ...
[ldr] PE32 mapeado @ 0x1700000 (delta=0x100000)
[ldr] relocacoes (.reloc) aplicadas: 7                          <- relocacoes HIGHLOW aplicadas
[ps] EPROCESS criado: pid=5 img=test32.exe base=0x1700000
  [PE32] Ola do RING 3 de 32 bits (compatibility mode)!         <- codigo de 32 bits via int 0x80
  | Texto : Sou um .EXE de 32 bits (PE32) rodando em compat mode no MeuOS!
Sistema no ar. Digite algo (teclado por interrupcao):
```

Os detalhes por tarefa (arquivos, decisoes, limitacoes) seguem abaixo.

## TAREFA 1 — Process Manager e espaco de enderecamento (estilo NT)

Status: **completo, build verde, boot verde.** Todas as partes (inclusive a
troca de CR3 por processo) foram implementadas SEM quebrar o boot.

### O que foi implementado
- **EPROCESS / ETHREAD como objetos do Object Manager**
  (`src/nt/process.c`, `src/nt/process.h`):
  - `EPROCESS` (OB_TYPE_PROCESS): `ProcessId`, `DirectoryTableBase` (CR3),
    `ImageBase`, `PML4`, `ThreadCount`, `ExitStatus`, `Terminated`, `ImageName`,
    ponteiro para a thread principal.
  - `ETHREAD` (OB_TYPE_THREAD): `ThreadId`, `Process`, `StartAddress` (entry),
    `StackBase`/`StackTop`, `ExitStatus`, `Terminated`.
  - API estilo "Ps": `PsCreateProcess`, `PsCreateThread`, `PsTerminateProcess`,
    `PsGetCurrentProcess`/`PsGetCurrentThread`/`ps_set_current`, `ps_init`.
  - PID/TID sequenciais; criados via `ObCreateObject(OB_TYPE_PROCESS/THREAD)`.
  - `ps_init()` chamado em `kmain` (linha [ok] "Process Manager ...").

- **PML4 por processo + troca de CR3** (`src/mm/paging.c`, `src/mm/paging.h`):
  - `mm_create_address_space()`: aloca PML4/PDPT/PD novos no PMM e **copia os
    mapeamentos do kernel** (identidade de 1 GiB, ja com os bits U/S setados
    pelos `mm_map_user` anteriores). Retorna o endereco fisico da nova PML4.
  - `mm_get_cr3()` / `mm_switch_cr3()`.
  - `usermode_enter` (`src/ke/usermode.c`) cria o espaco do processo, **troca o
    CR3 ao entrar em ring 3 e restaura o CR3 do kernel ao sair** (no retorno via
    longjmp). O kernel continua mapeado (identidade), entao IDT, `int 0x80`,
    pilha de kernel e drivers seguem validos sob a nova PML4. Se faltar RAM,
    cai de volta no espaco compartilhado (sem regressao).
  - Log: `[mm] PML4 por-processo @ 0x... (kernel CR3 0x...)`.

- **Syscalls de processo (SSDT em `src/ke/syscall.c`)** + stubs no ntdll:
  - 7 `SYS_CREATEPROCESS` -> `NtCreateProcess(out HANDLE*, image_name)`: cria
    EPROCESS + handle (herda CR3 atual; nao executa a imagem).
  - 8 `SYS_CREATETHREAD` -> `NtCreateThread(out HANDLE*, hProcess, start)`.
  - 9 `SYS_TERMINATEPROCESS` -> `NtTerminateProcessEx(hProcess, status)`
    (hProcess==0 encerra o processo corrente via longjmp, como o "exit").
  - 10 `SYS_WAITFORSINGLEOBJECT` -> `NtWaitForSingleObject(h, timeout)`
    (retorna STATUS_SUCCESS imediatamente — sem escalonador ainda).
  - `dll/ntdll.c`: exports `NtCreateProcess`, `NtCreateThread`,
    `NtTerminateProcessEx`, `NtWaitForSingleObject` (+ helper `sc3`).
  - `dll/kernel32.c`: `CreateProcessA` (cria EPROCESS + thread, devolve handles
    em `PROCESS_INFORMATION`) e `WaitForSingleObject`.

- **Loader cria EPROCESS** (`src/loader/loader.c`): `ldr_run(path, image)` cria
  o `EPROCESS` do `.exe` (nome, ImageBase, CR3 atual) e a `ETHREAD` principal
  (entry), marca como processo corrente, roda em ring 3 e, ao voltar, faz
  `PsTerminateProcess`. (Assinatura de `ldr_run` passou a receber o `path`;
  `kmain` atualizado.)

- **Demonstracao a partir de ring 3** (`examples/hello.c` -> `test.exe`):
  antes do MessageBox, chama `CreateProcessA("worker.exe")` + `WaitForSingleObject`,
  exercitando ring3 -> kernel32 -> ntdll -> `int 0x80` -> Process Manager.

### Arquivos alterados / criados
- Novos: `src/nt/process.h`, `src/nt/process.c`.
- Alterados: `src/mm/paging.c`, `src/mm/paging.h`, `src/ke/usermode.c`,
  `src/ke/syscall.c`, `src/loader/loader.c`, `src/loader/loader.h`,
  `src/kernel.c`, `src/nt/object.h` (comentarios nos tipos),
  `dll/ntdll.c`, `dll/kernel32.c`, `examples/hello.c`.

### Evidencia (run.ps1 -Headless)
- `[ok] Process Manager (EPROCESS/ETHREAD via Object Manager)`
- `[ps] EPROCESS criado: pid=1 img=test.exe base=0x800000 cr3=0x10B000`
- `[ps] ETHREAD criado: tid=1 pid=1 entry=0x801000`
- `[mm] PML4 por-processo @ 0x4003000 (kernel CR3 0x10B000)`
- `[ps] EPROCESS criado: pid=2 img=worker.exe ...` (criado de DENTRO do ring 3
  pela syscall `NtCreateProcess`, com cr3=0x4003000 = a PML4 do processo pai —
  prova que a syscall roda sob o espaco por-processo)
- test.exe e ioctlapp.exe continuam rodando em ring 3; IOCTL devolve `0xCAFEBABE`.
- Sistema chega em "Sistema no ar."

### Limitacoes / proximos passos (nao bloqueiam o boot)
- O isolamento ainda nao e total: a PML4 por-processo COPIA o mapa de identidade
  do kernel (mesmos PDPT/PD com U/S), entao processos enxergam as mesmas paginas
  baixas. O proximo passo e dar paginas privadas (imagem/pilha por processo em
  faixas distintas, sem U/S compartilhado) — a infra de troca de CR3 ja esta de pe.
- `NtWaitForSingleObject` retorna na hora (sem escalonador/bloqueio).
- `NtCreateProcess` cria o objeto mas nao carrega/roda a imagem (o loader e quem
  roda os `.exe` do boot); ligar os dois e trabalho futuro.

## TAREFA 2 — Suporte a PE32 (32-bit) no loader + execucao em compat mode

Status: **completo, build verde, boot verde.** O loader agora aceita PE32 alem
de PE32+, aplica relocacoes (.reloc) e EXECUTA codigo de 32 bits em ring 3
(compatibility mode), tudo sem quebrar o caminho 64-bit existente.

### O que foi implementado
- **Parsing de PE32 e PE32+ independente da bitness** (`src/loader/pe.c`,
  `src/loader/pe.h`):
  - Nova `pe_parse(image, pe_info_t*)`: detecta `magic` 0x20B (PE32+) vs 0x10B
    (PE32) e `machine` 0x8664 vs 0x14C, lendo os campos do optional header nos
    offsets corretos de cada formato (ImageBase 8 bytes @24 no PE32+, 4 bytes @28
    no PE32; array de data directories @112 no PE32+, @96 no PE32; `BaseOfData`
    so existe no PE32). Preenche `pe_info_t` (machine, magic, is64, nsec,
    entry_rva, preferred ImageBase, size_image/hdrs, ndirs, dir_off, sec_off).
  - `pe_map_at(image, base, &entry)`: mapeia headers+secoes numa base arbitraria
    (serve PE32 e PE32+). `pe_map` continua mapeando no ImageBase preferido
    (caminho 64-bit intacto).
  - `pe_bind_imports`: agora trata thunks de 8 bytes (PE32+, bit 63 = ordinal) E
    de 4 bytes (PE32, bit 31 = ordinal); a IAT de 32 bits e escrita truncada
    (DLLs/kernel vivem < 4 GiB, entao o truncamento e seguro).
  - `pe_get_export` e `pe_subsystem` reescritos sobre `get_dir()`, que le o array
    de data directories no offset certo conforme o magic.
- **Relocacoes (.reloc / base relocations)** (`pe_relocate(base, preferred)` em
  `src/loader/pe.c`):
  - Aplica fixups quando `base != preferred` (delta = base - preferred).
  - Trata `IMAGE_REL_BASED_HIGHLOW` (tipo 3, patch de 32 bits — PE32) e
    `IMAGE_REL_BASED_DIR64` (tipo 10, patch de 64 bits — PE32+); ignora
    `ABSOLUTE` (tipo 0, padding). Percorre todos os blocos ate consumir o size
    do diretorio. Retorna o numero de fixups.
- **Segmento de codigo 32-bit (compat mode) na GDT** (`src/ke/gdt.c`,
  `src/ke/gdt.h`):
  - GDT expandida de 7 para 8 entradas; nova `gdt[7]` = codigo ring 3 de 32 bits
    (L=0, D/B=1, G=1, DPL3), seletor `SEL_UCODE32 = 0x38`. A pilha/dados de 32
    bits reutilizam `SEL_UDATA` (0x20), que ja tem D/B=1. O TSS (0x28, 2 slots)
    nao se mexeu.
- **Entrada em ring 3 de 32 bits** (`src/ke/usermode.c`, `src/ke/usermode.h`):
  - `enter_ring3_32(entry32, esp32)`: monta o frame iretq (sempre 5 qwords em
    long mode); ao carregar CS=0x3B (UCODE32|3, L=0/D=1) a CPU passa a executar
    codigo de 32 bits. EIP/ESP usam so os 32 bits baixos (imagem e pilha < 4 GiB).
  - `usermode_enter32(entry32)`: espelha `usermode_enter` (mapeia a pilha de
    usuario, cria a PML4 por-processo, troca o CR3, arma o `setjmp`, entra em
    ring 3 de 32 bits) e, na saida (`int 0x80` -> `sys_exit` -> longjmp),
    restaura o CR3 do kernel e os segmentos de ring 0. Reaproveita a mesma
    pilha de usuario (6-7 MiB) e o mesmo `g_user_exit`.
- **Syscall vinda de 32 bits** (`src/ke/syscall.c`):
  - `syscall_dispatch` detecta o chamador de 32 bits pelo CS salvo (`0x3B`) e
    TRUNCA os argumentos (RAX/RDI/RSI/RDX/RCX) para 32 bits — em compat mode
    escrever EAX/EDI/... nao zera os 32 bits altos do registrador de 64 bits.
    Convencao 32-bit do MeuOS: EAX=numero; args em EDI, ESI, EDX, ECX. Os
    servicos existentes (sys_write/sys_messagebox/sys_exit) funcionam sem
    alteracao, pois ja tratam ponteiros/inteiros na faixa baixa.
- **Loader bitness-aware** (`src/loader/loader.c`):
  - `ldr_run` faz `pe_parse` e despacha: PE32+ segue o caminho 64-bit de sempre;
    PE32 vai para o novo `ldr_run32`.
  - `ldr_run32`: loga machine/magic/ImageBase/entryRVA; se a imagem tem `.reloc`,
    carrega numa base DESLOCADA da preferida (`0x1700000`) para EXERCITAR as
    relocacoes (senao usa o ImageBase preferido); chama `pe_relocate` e loga o
    numero de fixups; cria EPROCESS/ETHREAD; resolve imports (no-op se nao houver);
    entra em ring 3 de 32 bits via `usermode_enter32`; ao voltar, encerra o
    processo. Base de 32 bits separada da 64-bit (`LOAD32_BASE = 0x1600000`).
- **Exe de teste de 32 bits** (`examples/hello32.c` -> `build/test32.exe`):
  - Compilado com `-target x86-windows-gnu` (PE32, machine 0x14C, magic 0x10B),
    ImageBase 0x1600000, `--dynamicbase` para manter a `.reloc`. Autocontido (sem
    imports): faz `int 0x80` direto via assembly inline (sys_write, sys_messagebox,
    sys_exit). Wired em `build.ps1` e na lista default de modulos de `run.ps1`.

### Arquivos alterados / criados
- Novos: `examples/hello32.c`.
- Alterados: `src/loader/pe.c`, `src/loader/pe.h`, `src/loader/loader.c`,
  `src/ke/gdt.c`, `src/ke/gdt.h`, `src/ke/usermode.c`, `src/ke/usermode.h`,
  `src/ke/syscall.c`, `build.ps1`, `run.ps1`, `CHANGELOG.md`.

### Evidencia (run.ps1 -Headless)
- 64-bit intacto: `test.exe` em ring 3 -> MessageBox OK; `ioctlapp.exe` ->
  `[DbgPrint] IoctlDrv: respondeu 0xCAFEBABE` + MessageBox `0xCAFEBABE`.
- PE32 ponta a ponta:
  - `[ldr] PE32 (32-bit) detectado: machine=0x14C magic=0x10B ImageBase preferido=0x1600000 entryRVA=0x1000`
  - `[ldr] PE32 mapeado @ 0x1700000 (delta=0x100000)`
  - `[ldr] relocacoes (.reloc) aplicadas: 7`  (delta != 0 -> fixups HIGHLOW aplicados)
  - `[ldr] entrando no PE32 em RING 3 de 32 bits (compat mode)...`
  - `  [PE32] Ola do RING 3 de 32 bits (compatibility mode)!`  (sys_write de 32 bits)
  - MessageBox: `MeuOS  -  PE32 (loader 32-bit)` / `Sou um .EXE de 32 bits (PE32)...`
  - `[ldr] o PE32 (ring 3 32-bit) terminou; de volta ao kernel.` + `[ps] processo pid=4 encerrou`
- `Sistema no ar.` ao final; `qemu.err.log` vazio; sem `[EXCECAO]`, sem
  `syscall desconhecida`, sem `import nao resolvido`, sem triple-fault.

### Limitacoes / proximos passos (nao bloqueiam o boot)
- Nada foi revertido: o LOADER (parse + relocacoes) E a EXECUCAO de 32 bits
  ficaram de pe com build/boot verdes.
- O exe de 32 bits e autocontido (faz `int 0x80` direto). Ainda NAO ha
  ntdll/kernel32/user32 de 32 bits, entao um PE32 que importe Win32 cairia em
  "import nao resolvido" (as DLLs do sistema sao 64-bit). Proximo passo: compilar
  versoes 32-bit das DLLs do sistema (com export table) e um thunk de syscall de
  32 bits, para um PE32 com imports rodar como o de 64.
- A convencao de syscall de 32 bits cobre ate ~4 args (EDI/ESI/EDX/ECX). IOCTL
  (6 args) precisaria passar argumentos pela pilha de 32 bits no caso 32-bit;
  fica para quando houver um kernel32 de 32 bits.

## TAREFA 3 — Expandir syscalls e DLLs do sistema (console I/O)

Status: **completo, build verde, boot verde.** Novas syscalls de arquivo
(`NtWriteFile`/`NtReadFile`) com um "console device", stubs de registro
(`NtOpenKey`/`NtQueryValueKey`), `GetModuleHandle`/`GetProcAddress` apoiados no
loader real, e um novo `.exe` que imprime no console via `GetStdHandle` +
`WriteFile`. Tudo passa por ntdll -> `int 0x80` -> SSDT.

### O que foi implementado
- **Novas syscalls no SSDT** (`src/ke/syscall.c`), numeros casando com `dll/ntdll.c`:
  - 11 `SYS_WRITEFILE` -> `sys_writefile(handle, buf, len, *written)`.
  - 12 `SYS_READFILE`  -> `sys_readfile(handle, buf, len, *read)`.
  - 13 `SYS_OPENKEY`        -> `sys_openkey(out HANDLE*, path)` (stub de registro).
  - 14 `SYS_QUERYVALUEKEY`  -> `sys_queryvaluekey(key, name, buf, buflen, *outlen)`.
  - 15 `SYS_GETMODULEHANDLE` -> `sys_getmodulehandle(name)` -> base da imagem.
  - 16 `SYS_GETPROCADDRESS`  -> `sys_getprocaddress(base, fn)` -> endereco do export.
  - `NtClose` (6) reaproveitado para fechar handles (os pseudo-handles do console
    nao passam pela handle table — `CloseHandle` os ignora).

- **"Console device"** (`src/ke/syscall.c`): os handles padrao do Win32
  (`GetStdHandle`: STD_OUTPUT=-11, STD_ERROR=-12, STD_INPUT=-10) sao
  **pseudo-handles** (valores sentinela), como no NT. `sys_writefile` os
  reconhece (`is_console_handle`) e escreve byte a byte via `kputc` (VGA+serial);
  `sys_readfile` retorna 0 bytes (sem stdin bufferizado ainda) com STATUS_SUCCESS.

- **Caminho de WriteFile/ReadFile para handles de ARQUIVO** (driver via IRP):
  - `src/nt/io.c` + `io.h`: novas `io_build_write(dev, buf, len)` (IRP_MJ_WRITE,
    METHOD_BUFFERED — copia o buffer do usuario para o SystemBuffer) e
    `io_build_read(dev, buf, len)` (IRP_MJ_READ; o driver preenche o SystemBuffer
    e reporta os bytes em `IoStatus.Information`). `IO_STACK_LOCATION` ja tinha os
    campos `Parameters.Write`/`Parameters.Read` no `sdk/ntddk.h`.
  - `sys_writefile`/`sys_readfile`: se o handle NAO e console, resolvem o
    `FILE_OBJECT` (`ob_handle_to_object(.., OB_TYPE_FILE)`), montam o IRP, chamam
    `IoCallDriver` e (no read) copiam o SystemBuffer de volta. (Caminho pronto;
    exercitado pelo console no boot — um driver com Read/Write o usa direto.)

- **Registro (stubs simples)** (`src/ke/syscall.c`): sem hive real ainda.
  `sys_openkey` aceita qualquer caminho e devolve uma raiz fixa
  (`REG_ROOT_HANDLE = 'REGK'`); `sys_queryvaluekey` responde valores conhecidos
  como string ASCII (`ProductName`="MeuOS", `CurrentVersion`="0.1"), senao
  `STATUS_OBJECT_NAME_NOT_FOUND` (0xC0000034).

- **`GetModuleHandle`/`GetProcAddress` apoiados no loader real** (sem stub):
  - Kernel: `sys_getmodulehandle` chama `ldr_load(name)` (loader estilo LdrLoadDll,
    `src/loader/loader.c`) -> base da imagem ja carregada; `sys_getprocaddress`
    chama `pe_get_export(base, fn)` (caminha a export table, `src/loader/pe.c`).
  - `src/ke/syscall.c` passou a incluir `loader/loader.h` e `loader/pe.h`.

- **ntdll** (`dll/ntdll.c`): novos helpers `sc4`/`sc5` (4 e 5 args: rdi, rsi,
  rdx, r10, r8) e os exports `NtWriteFile`, `NtReadFile`, `NtOpenKey`,
  `NtQueryValueKey`, `LdrGetModuleHandle`, `LdrGetProcAddress` (a unica camada que
  faz `int 0x80`).

- **kernel32** (`dll/kernel32.c`): novos exports `GetStdHandle` (sign-extend do
  valor sentinela), `WriteFile` (via `NtWriteFile`), `ReadFile` (via `NtReadFile`),
  `GetModuleHandleA` (via `LdrGetModuleHandle`), `GetProcAddress` (via
  `LdrGetProcAddress`). `CloseHandle` agora ignora pseudo-handles do console.
  `user32` segue com `MessageBoxA` (inalterado).

- **Novo exe de exemplo** (`examples/conhello.c` -> `build/conhello.exe`,
  PE32+ x64, ImageBase **0x1800000** — livre): `GetStdHandle(STD_OUTPUT_HANDLE)`
  + `WriteFile` para imprimir 2 linhas no console; depois
  `GetModuleHandleA("user32.dll")` + `GetProcAddress(.., "MessageBoxA")` e chama o
  `MessageBoxA` pelo ponteiro obtido (igual ao Windows com LoadLibrary/GetProcAddress).
  Importa kernel32 + user32. Wired em `build.ps1` e na lista default de `run.ps1`.

### Arquivos alterados / criados
- Novos: `examples/conhello.c`.
- Alterados: `src/ke/syscall.c`, `src/nt/io.c`, `src/nt/io.h`,
  `dll/ntdll.c`, `dll/kernel32.c`, `build.ps1`, `run.ps1`,
  `CHANGELOG.md`, `README.md`.

### Evidencia (run.ps1 -Headless)
- Tudo o que ja funcionava continua: ambos os drivers `DriverEntry status=0x0`;
  `test.exe` (64-bit) -> MessageBox; `ioctlapp.exe` -> `[DbgPrint] IoctlDrv:
  respondeu 0xCAFEBABE` + MessageBox `0xCAFEBABE`; `test32.exe` (PE32) em compat
  mode com 7 relocacoes -> MessageBox.
- NOVO `conhello.exe` ponta a ponta (ring3 -> kernel32 -> ntdll -> int 0x80 ->
  console device):
  - `  [conhello] Ola do RING 3 via GetStdHandle + WriteFile!`
  - `  [conhello] Esta linha foi escrita por NtWriteFile (console device).`
  - `  [conhello] GetProcAddress(user32, MessageBoxA) OK.`  (loader/export table reais)
  - MessageBox `MeuOS  -  WriteFile + GetProcAddress` / `Chamado via GetProcAddress!`
  - `[ps] processo pid=4 encerrou (status=0x0)`.
- `Sistema no ar.` ao final; `qemu.err.log` vazio; sem `syscall desconhecida`,
  sem `import nao resolvido`, sem `[EXCECAO]`/triple-fault.

### Limitacoes / proximos passos (nao bloqueiam o boot)
- `NtReadFile` no console retorna sempre 0 bytes (sem stdin bufferizado): o
  teclado por IRQ existe, mas falta um buffer de linha/fila ligada ao console
  device. O caminho de read para handles de arquivo (IRP_MJ_READ) ja esta pronto.
- O caminho WriteFile/ReadFile por IRP (handles de arquivo/dispositivo) compila e
  esta wired, mas no boot so o console e exercitado; um driver que implemente
  IRP_MJ_WRITE/READ o usa sem mudancas.
- Registro e stub (sem hive em disco/memoria persistente): `NtOpenKey` devolve uma
  raiz fixa e `NtQueryValueKey` so conhece `ProductName`/`CurrentVersion`.
- `GetModuleHandleA(NULL)` devolve NULL aqui (o NT devolveria a base do .exe
  corrente); como o EPROCESS ja guarda o ImageBase, da para ligar no futuro.

## TAREFA 4 — Consolidacao da documentacao

Status: **concluido, build verde, boot verde.** Esta etapa nao mexeu em codigo;
verificou o estado do repo, conferiu que o codigo bate com o que foi descrito e
consolidou a documentacao.

### O que foi feito
- Adicionado no topo deste `IMPLEMENTED.md` um **Resumo da rodada (consolidado)**:
  tabela por tarefa (tema + status funciona/parcial) e a **evidencia serial final
  com todos os modulos no MESMO boot** (test.exe + ioctlapp.exe + conhello.exe +
  test32.exe + 2 drivers).
- `CHANGELOG.md` (secao **[Nao lancado]**): acrescentado o bloco do **Process
  Manager + espaco de enderecamento por processo** (TAREFA 1), que faltava; as
  TAREFAS 2 (PE32) e 3 (console I/O) ja estavam registradas. Limitacoes marcadas
  explicitamente como *parcial*.
- `README.md`:
  - **Status (checklist)**: novos itens `[x]` para Process Manager (EPROCESS/
    ETHREAD + PML4 por processo + syscalls de processo) e Loader PE32/compat mode;
    novo item `[~]` para "isolamento por processo (parcial)".
  - **Estrutura**: adicionado `nt/process.c/.h`; `paging.c` e `loader/` com
    descricoes atualizadas (PML4 por processo, PE32, relocacoes); listas de
    `examples/` (hello32, conhello, ioctlapp, ioctldriver) e `dll/` atualizadas.
  - **Arquitetura**: novo paragrafo do Process Manager; o "Falta para ser NT
    completo" reescrito (isolamento por-processo agora **parcial**, nao ausente).
  - **Roadmap**: novo passo 9 `[~]` "Process Manager (ring 3)"; corrigida a nota
    que dizia "hoje o .exe roda em ring 0" (ja roda em **ring 3**, com EPROCESS+CR3).
- Rodado `build.ps1 -Clean` + `run.ps1 -Headless -TimeoutSec 8` uma ultima vez:
  build OK; boot OK; todas as linhas `[ok]`; ambos os drivers `status=0x0`;
  os 4 `.exe` rodam em ring 3 (incluindo o PE32 em compat mode com 7 relocacoes,
  o IOCTL `0xCAFEBABE` e o `conhello.exe` via console device); `qemu.err.log`
  vazio; sem `[EXCECAO]`/triple-fault/`syscall desconhecida`/`import nao resolvido`.

### Arquivos alterados
- `IMPLEMENTED.md` (este arquivo — resumo consolidado + esta secao),
  `CHANGELOG.md`, `README.md`. Nenhum arquivo de codigo foi tocado.

## FASE 1 — Framebuffer grafico (VGA mode 13h) + GDI de baixo nivel

Status: **completo, build verde, boot verde.** Pre-requisito da GUI: o OS sai do
modo texto e ganha um framebuffer grafico com uma API de desenho no kernel, sem
quebrar nada do que ja funcionava (test.exe, IOCTL 0xCAFEBABE, conhello, test32).

### O que foi implementado
- **Driver de video em framebuffer — VGA mode 13h (320x200x256)**
  (`src/drivers/video.c` + `src/drivers/video.h`):
  - `fb_init()`: programa os blocos de registradores da VGA **direto** (sem BIOS,
    que nao existe em long mode): Miscellaneous Output (0x3C2), Sequencer
    (0x3C4/0x3C5), CRT Controller (0x3D4/0x3D5, destravando o bit de protecao do
    CRTC[0x11]), Graphics Controller (0x3CE/0x3CF) e Attribute Controller (0x3C0,
    resetando o flip-flop via leitura de 0x3DA e religando a tela com o bit PAS).
    Carrega a paleta no DAC (0x3C8/0x3C9): 16 cores nomeadas (EGA/CGA, convertidas
    para os 6 bits do DAC) + rampa de cinza nos indices 16..255. Framebuffer
    linear em **0xA0000** (1 byte = 1 pixel = indice na paleta), ja dentro da
    identidade de 1 GiB (primeira pagina de 2 MiB), entao **nao precisou mexer nas
    tabelas de pagina** — o caminho seguro pedido no enunciado.
  - **API GDI de baixo nivel**: `fb_clear(cor)`, `fb_pixel(x,y,cor)`,
    `fb_get_pixel(x,y)` (leitura — usada para verificar o que foi escrito),
    `fb_fill_rect(x,y,w,h,cor)` (com clipping de borda), `fb_rect` (contorno 1px),
    `fb_hline`/`fb_vline`, `fb_draw_char(x,y,c,fg,bg)` e `fb_draw_text(x,y,str,fg,bg)`
    (bg=0xFF = fundo transparente; trata `\n` e quebra de linha automatica).
    Todas as operacoes sao no-op seguro se `fb_active()` for falso.
- **Fonte bitmap 8x8 embutida** (`src/drivers/font8x8.c`): `g_font8x8[96][8]`
  cobrindo o ASCII imprimivel (0x20..0x7F), 8 bytes por glifo (1 linha/byte, bit 7
  = pixel a esquerda). Conjunto de dominio publico no estilo da fonte 8x8 do PC.
- **Demo no boot** (`src/kernel.c`, funcao `fb_demo()`): roda DEPOIS de todos os
  `[ok]` e dos testes de binario (para preservar os logs no VGA texto ate ali, ja
  que o mode 13h para de exibir o texto). Sequencia: `fb_init` -> `fb_clear`
  (desktop azul) -> `fb_rect` (borda branca) -> uma "janela" estilo NT
  (`fb_fill_rect` do corpo cinza + barra de titulo azul + `fb_rect` da moldura) ->
  `fb_draw_text` (titulo "MeuOS - mode 13h" + 3 linhas de corpo em cores
  diferentes) -> 4 `fb_pixel` nos cantos -> 16 swatches da paleta nomeada ->
  `fb_get_pixel` na barra de titulo conferindo que leu **cor=1 (azul)** = o que
  foi escrito. **Cada operacao e logada na serial** (`[fb] ...`) para comprovar a
  logica em headless, conforme a regra 3.
- **A serial continua sendo o canal de log**: `kputc` (VGA texto + serial) nao foi
  tocado; os logs de boot e o eco de teclado seguem na serial mesmo apos o mode
  13h tomar a tela grafica.
- **Captura opcional do framebuffer via QMP** (`run.ps1`): novo switch
  `-Screendump` (e `-QmpPort`, default 4444) que adiciona `-qmp tcp:...` ao QEMU,
  conecta via TCP, faz o handshake QMP (`qmp_capabilities`) e executa `screendump`
  para `build\screen.ppm`. Sem o switch, o comportamento default (teste headless)
  fica **inalterado** — nao arrisca o boot verde.

### Arquivos alterados / criados
- Novos: `src/drivers/video.h`, `src/drivers/video.c`, `src/drivers/font8x8.c`.
- Alterados: `src/kernel.c` (include de `video.h`, funcao `fb_demo()` e a chamada
  no fim de `kmain`, antes do loop ocioso), `run.ps1` (switch `-Screendump`/`-QmpPort`
  + captura QMP), `IMPLEMENTED.md` (esta secao).
- **Estrutura de pastas inalterada**: tudo dentro de `src/drivers/` (pasta ja
  existente). 64-bit e todos os testes anteriores intactos.

### Evidencia (run.ps1 -Headless -TimeoutSec 10)
- Tudo o que ja funcionava continua: 12 linhas `[ok]`; ambos os drivers
  `DriverEntry status=0x0`; `test.exe` (64-bit) MessageBox; `ioctlapp.exe` ->
  `[DbgPrint] IoctlDrv: respondeu 0xCAFEBABE` + MessageBox `0xCAFEBABE`;
  `conhello.exe` via console device; `test32.exe` (PE32) compat mode com 7
  relocacoes. `qemu.err.log` vazio; 0 `[EXCECAO]`/triple-fault/`syscall
  desconhecida`/`import nao resolvido`; chega em `Sistema no ar.`
- Framebuffer (logs `[fb]` na serial):
  ```
  --- Framebuffer grafico (VGA mode 13h, 320x200x256) ---
  [fb] fb_init(): programando registradores VGA (CRTC/SEQ/GC/AC) + paleta DAC...
  [fb] mode 13h ativo: framebuffer linear em 0xA0000 (320x200, 8bpp). ...
  [fb] fb_clear(cor=1 azul): pintando 320x200 = 64000 pixels.
  [fb] fb_rect(0,0,320,200, cor=15 branco): contorno do desktop.
  [fb] fb_fill_rect(40,40,240,110, cor=7 cinza): corpo da janela.
  [fb] fb_fill_rect(40,40,240,12, cor=1 azul): barra de titulo.
  [fb] fb_draw_text(48,42,"MeuOS", fg=15): titulo na barra.
  [fb] fb_get_pixel(barra de titulo) = 1 (esperado 1 = azul) -> OK
  [fb] demo concluida: desktop + 1 janela (titulo+corpo+texto) desenhados.
  ```
- **Screendump confirmado** (`run.ps1 -Headless -Screendump`): `build\screen.ppm`
  gerado (P6 640x400 — o QEMU dobra a resolucao do mode 13h na exibicao), 768015
  bytes. Convertido para PNG e inspecionado: mostra o desktop azul, a janela cinza
  com barra de titulo azul e moldura preta, o titulo "MeuOS - mode 13h" e 3 linhas
  de corpo (preto/vermelho/azul) na fonte 8x8, e as 16 swatches da paleta nomeada
  embaixo — **bate exatamente com o desenhado**, provando os pixels (nao so o log).

### Limitacoes / proximos passos (nao bloqueiam o boot)
- Ficamos no **mode 13h** (caminho seguro): 320x200x256, framebuffer ja mapeado em
  0xA0000. O STRETCH (VBE bochs-DISPI nas portas 0x1CE/0x1CF para resolucao maior
  com LFB linear) NAO foi feito, porque exigiria mapear a faixa fisica do LFB nas
  tabelas de pagina — preferi a opcao segura, como o enunciado permite.
- Ao entrar no mode 13h o VGA texto (0xB8000) deixa de ser exibido; a serial segue
  como canal de log/eco. Nao ha "modo texto sobre grafico" ainda (renderizar o
  console na tela grafica via `fb_draw_text` e um proximo passo natural).
- A GDI aqui e do lado **kernel** (`fb_*`), base para a Fase 2 (window manager +
  user32/gdi32 em ring 3 -> ntdll -> int 0x80 -> win32k). Ainda nao ha
  `CreateWindowEx`/`BitBlt`/`TextOutA` expostos a binarios Windows; isto e o
  alicerce sobre o qual a GUI sera construida.

## FASE 2 — Win32k (janelas + mensagens) + user32/gdi32

Status: **completo, build verde, boot verde.** Subsistema grafico estilo Windows
sobre o framebuffer da Fase 1: window manager (HWND/z-order/foco), filas de
mensagens por thread, objetos GDI, syscalls win32k (>=17), DLLs `user32.dll`
(janelas) + `gdi32.dll` (GDI) como PE reais, e uma demo `guiapp.exe` (ring 3) que
cria janela, faz o loop de mensagens e no `WM_PAINT` desenha — a janela **aparece
no framebuffer**. Nada do que ja funcionava regrediu.

### O que foi implementado
- **Window manager (lado KERNEL, win32k.sys)** (`src/win32/win32k.c` +
  `src/win32/win32k.h`):
  - **Arvore de janelas (HWND)**: `WND` com retangulo (x,y,w,h), titulo, classe,
    `visible`, `z` (z-order) e o ponteiro do WNDPROC (guardado p/ debug). HWND = id
    pequeno (1,2,3,...). Tabela de ate 16 janelas + 16 classes. **Janela com foco**
    (`s_focus_id`) recebe o teclado; nova janela toma o foco; ao destruir, o foco
    volta para a de maior z.
  - **Compositing simples** (`win32k_compose`): pinta o desktop (azul) e desenha as
    janelas em **ordem de z crescente** (as de z maior por cima). Cada janela:
    corpo cinza (area cliente) + barra de titulo (azul se ativa, cinza-escuro se
    nao) + titulo + moldura preta. Loga cada janela (rect, z, [FOCO]).
  - **Filas de mensagens**: fila circular (cap 64). `NtUserGetMessage` **BLOQUEIA**
    com `sti; hlt` ate haver mensagem (o teclado/timer por IRQ enchem a fila);
    devolve 1=normal, 0=WM_QUIT, -1=erro; se nao ha mais janelas, encerra o loop.
    `WM_CREATE`/`WM_PAINT`/`WM_KEYDOWN`/`WM_CHAR`/`WM_DESTROY`/`WM_QUIT`.
  - **Roteamento de teclado** (`win32k_on_key`, chamado pela IRQ1): posta
    `WM_KEYDOWN` + `WM_CHAR` para a **janela com foco**. `keyboard_irq`
    (`drivers/keyboard.c`) passou a rotear para o win32k quando ha janelas (senao
    mantem o eco no console). **Injecao sintetica** (`win32k_inject_demo_input`)
    posta teclas + `WM_QUIT` na 1a janela mostrada, para o teste headless ser
    deterministico (sem digitacao) — um teclado real (IRQ1) tambem roteia.
  - **Objetos GDI no Object Manager**: `HDC` (`W32_DC`, liga a janela dona) e
    `HBRUSH` (`W32_BRUSH`, cor de paleta) sao objetos (`ObCreateObject`). Brushes
    stock pre-criados (`GetStockObject`). `GetDC` devolve um HANDLE; `TextOut`/
    `FillRect` resolvem o HDC -> janela e desenham na **area cliente** (coords
    relativas convertidas p/ o desktop) via `fb_draw_text`/`fb_fill_rect`.
  - O **framebuffer e ligado sob demanda** (`ensure_fb` -> `fb_init`, idempotente)
    no 1o desenho da GUI, preservando os logs de boot no VGA texto ate ali.
- **Syscalls win32k (SSDT em `src/ke/syscall.c`)**, numeros novos **17..30**,
  sincronizados com `dll/ntdll.c`: `NtUserRegisterClass`(17),
  `NtUserCreateWindowEx`(18), `NtUserDestroyWindow`(19), `NtUserShowWindow`(20),
  `NtUserGetMessage`(21), `NtUserDispatchMessage`(22), `NtUserPostMessage`(23),
  `NtUserPostQuitMessage`(24), `NtUserGetDC`(25), `NtUserInvalidate`(26),
  `NtGdiGetStockObject`(27), `NtGdiCreateSolidBrush`(28), `NtGdiTextOut`(29),
  `NtGdiFillRect`(30). Cada handler le os args de `struct regs` (rdi,rsi,rdx,r10,
  r8,r9) e chama a funcao do win32k.
- **ntdll** (`dll/ntdll.c`): exports `Nt*` para as 14 syscalls acima (a unica camada
  que faz `int 0x80`); reaproveita `sc1..sc6`.
- **user32.dll** (`dll/user32.c`) — API de janelas em **ring 3**: `RegisterClassA`,
  `CreateWindowExA`, `ShowWindow`, `DestroyWindow`, `GetMessageA`,
  `TranslateMessage`, `DispatchMessageA`, `DefWindowProcA`, `PostQuitMessage`,
  `PostMessageA`, `InvalidateRect`, `GetDC`/`ReleaseDC`, `BeginPaint`/`EndPaint`,
  `FillRect` (no Windows real e funcao do USER) e `MessageBoxA`. **Ponto-chave
  (igual ao Windows): `DispatchMessageA` CHAMA o WNDPROC em ring 3** — o kernel so
  entrega a `MSG`; o callback do app nunca roda no kernel. O user32 mantem a tabela
  classe->wndproc e hwnd->wndproc localmente.
- **gdi32.dll** (`dll/gdi32.c`, novo) — `TextOutA`, `GetStockObject`,
  `CreateSolidBrush`, `DeleteObject` (encaminham p/ o win32k via ntdll). Adicionada
  ao `build.ps1` (ImageBase **0x1A00000**) e ao `run.ps1`.
- **Demo `examples/guiapp.c`** (`build\guiapp.exe`, PE32+ x64, ImageBase
  **0x1C00000**): `RegisterClassA` + `CreateWindowExA` + `ShowWindow` + loop
  `GetMessage`/`TranslateMessage`/`DispatchMessage`. O WNDPROC trata `WM_CREATE`,
  `WM_PAINT` (`CreateSolidBrush` + `FillRect` de um retangulo vermelho + 3
  `TextOutA`), `WM_KEYDOWN`/`WM_CHAR` (loga a tecla) e `WM_DESTROY`. Importa
  kernel32 + user32 + gdi32. Loga cada etapa na serial.
- **kmain** (`src/kernel.c`): `win32k_init()` apos `ps_init()` (13a linha `[ok]`).
  Ao final, se a GUI deixou janela(s) na tela, **NAO** roda a `fb_demo()` da Fase 1
  (que apagaria o conteudo) — deixa o framebuffer como o `guiapp` pintou (chrome +
  cliente) para o screendump; sem GUI, a `fb_demo()` roda normalmente.

### Arquivos alterados / criados
- Novos: `src/win32/win32k.h`, `src/win32/win32k.c`, `dll/gdi32.c`,
  `examples/guiapp.c`.
- Alterados: `src/ke/syscall.c` (syscalls 17..30 + handlers), `dll/ntdll.c`
  (exports `Nt*` win32k), `dll/user32.c` (API de janelas + FillRect),
  `src/drivers/keyboard.c` (roteia IRQ1 -> win32k), `src/kernel.c`
  (`win32k_init` + estado final da GUI), `build.ps1` (gdi32.dll + guiapp.exe),
  `run.ps1` (gdi32.dll + guiapp.exe na lista default), `IMPLEMENTED.md`.
- **Estrutura de pastas inalterada**: tudo em `src/win32/`, `dll/`, `examples/`
  (pastas ja existentes). 64-bit e todos os testes anteriores intactos.

### Evidencia (run.ps1 -Headless -TimeoutSec 12)
- Tudo o que ja funcionava continua: **13 linhas `[ok]`** (era 12, +1 do win32k);
  ambos os drivers `DriverEntry status=0x0` (7 ocorrencias do status); `test.exe`
  (64-bit) MessageBox "Pinball (teste do loader PE)"; `ioctlapp.exe` -> IOCTL
  `0xCAFEBABE` (3x); `conhello.exe` via console device; `test32.exe` (PE32) compat
  mode com 7 relocacoes. `qemu.err.log` **vazio (0 bytes)**; **0** `[EXCECAO]`,
  **0** `Page Fault`, **0** `syscall desconhecida`, **0** `import nao resolvido`;
  chega em `Sistema no ar.`
- GUI ponta a ponta (ring3 guiapp -> user32/gdi32 -> ntdll -> int 0x80 -> win32k
  -> framebuffer), logs da serial:
  ```
  [win32k] RegisterClass 'MeuOSWindowClass' wndproc=0x...01C012C0
  [win32k] CreateWindowEx -> HWND #1 classe='MeuOSWindowClass' titulo='MeuOS GUI' x=60 y=50 w=180 h=110
  [win32k] ShowWindow #1 -> VISIVEL
  [win32k] compose: desktop + 1 janela(s) (z-order)
  [win32k]   janela #1 'MeuOS GUI' rect=(60,50,180,110) z=1 [FOCO]
  [win32k] (demo) injetando teclas sinteticas p/ HWND #1: "Hi"
    [guiapp] WM_CREATE recebido.
    [guiapp] WM_PAINT: desenhando (FillRect + TextOut)...
  [gdi] GetDC(HWND #1) -> HDC 0x...0C
  [gdi] CreateSolidBrush(cor=4) -> HBRUSH 0x...
  [gdi] FillRect(HDC, x=8, y=24, w=112, h=20, cor=4) -> desktop(69,87)
  [gdi] TextOut(HDC, x=6, y=4, "Janela GUI do MeuOS") -> desktop(67,67)
  [gdi] TextOut(HDC, x=12, y=28, "WM_PAINT OK") -> desktop(73,91)
  [gdi] TextOut(HDC, x=6, y=52, "Fase 2: win32k") -> desktop(67,115)
    [guiapp] WM_KEYDOWN/WM_CHAR: 'H' ... 'i'   (tecla roteada p/ a janela com foco)
  [win32k] GetMessage -> WM_QUIT (fim do loop)
    [guiapp] saiu do loop (WM_QUIT). Fim.
  ```
- **Screendump confirmado** (`run.ps1 -Headless -Screendump`): `build\screen.ppm`
  (P6 640x400 — o QEMU dobra o mode 13h). Convertido para PNG e inspecionado com a
  ferramenta Read: **desktop azul**, uma **janela com barra de titulo azul** (ativa)
  escrito "MeuOS GUI", **area cliente cinza com moldura preta**, o texto "Janela GUI
  do MeuOS", um **retangulo vermelho** com "WM_PAINT OK" por cima, e "Fase 2:
  win32k" — **bate exatamente** com as operacoes logadas (prova os pixels, nao so o
  log).

### Limitacoes / proximos passos (nao bloqueiam o boot)
- Nada foi revertido: o window manager, as filas de mensagens, a GDI e as DLLs
  ficaram de pe com build/boot verdes.
- **Uma fila de mensagens global** (uma thread GUI por vez). Como o OS roda os
  `.exe` em sequencia (sem escalonador), basta; multiplas threads GUI exigem fila
  por-thread e um escalonador.
- **`DispatchMessage` chama o WNDPROC em ring 3** (no user32), que e exatamente o
  modelo do Windows; o kernel nao faz upcall para ring 3. Como so ha uma app GUI no
  boot, o user32 guarda o mapa classe/hwnd->wndproc localmente.
- **Sem mouse**: o roteamento de `WM_LBUTTONDOWN`/`WM_MOUSEMOVE` esta definido
  (constantes + caminho de fila), mas nao ha driver de mouse PS/2 (IRQ12) ainda; o
  teclado (IRQ1) ja roteia para a janela com foco. Adicionar o mouse e incremental.
- **Compositing sem clipping entre janelas / sem repaint pos-saida**: o `compose`
  redesenha tudo em z-order (poucas janelas); apos o app sair nao ha quem repinte o
  cliente, entao o estado final = o que o `guiapp` pintou (deixado intacto p/ o
  screendump). `InvalidateRect` ja enfileira `WM_PAINT`.
- **Resolucao**: seguimos no **mode 13h** (320x200) herdado da Fase 1; janelas
  maiores que a tela sao clipadas. VBE/LFB maior continua sendo o stretch da Fase 1.

## FASE 3 — Named Pipes (IPC)

Status: **completo, build verde, boot verde.** IPC estilo Windows: um novo tipo
de objeto `OB_TYPE_PIPE` no Object Manager com buffer e namespace `\Pipe\Nome`,
syscalls de criar/conectar pipe + roteamento de `NtReadFile`/`NtWriteFile` para o
buffer do pipe, API `CreateNamedPipeA`/`ConnectNamedPipe`/`CreateFileA("\\.\pipe\Nome")`
em `kernel32`, e uma demo sequencial (servidor escreve, cliente le os mesmos
bytes) — os dados atravessam servidor->cliente na serial. Nada regrediu.

### O que foi implementado
- **Tipo Pipe no Object Manager** (`src/nt/object.h`): novo `OB_TYPE_PIPE`. O pipe
  e um objeto nomeado de verdade (`ObCreateObject`/`ObLookupObject`), entao vive no
  namespace global do kernel e persiste entre processos (chave para a demo sem
  escalonador).
- **Named Pipe (lado kernel)** (`src/nt/pipe.c` + `src/nt/pipe.h`, novos):
  - `PIPE_OBJECT`: nome completo (`\Pipe\Nome`), estado
    (DISCONNECTED/LISTENING/CONNECTED), flags de servidor/cliente conectados e um
    **buffer circular** de 4096 bytes (Head/Tail/Count).
  - `pipe_normalize_name`: aceita `\\.\pipe\Nome` (forma Win32 do `CreateFile`),
    `\Device\NamedPipe\Nome` (forma NT), `\Pipe\Nome` ou so `Nome`, e normaliza
    tudo para `\Pipe\Nome` (o nome real no namespace).
  - `pipe_create` (servidor cria; falha se ja existir), `pipe_open` (cliente abre
    pelo nome; marca CONNECTED), `pipe_connect` (servidor sinaliza pronto; **sem
    escalonador nao bloqueia**), `pipe_write`/`pipe_read` (movem bytes no buffer
    circular e devolvem quantos moveram). Cada operacao loga na serial
    (`[pipe] ...`) para comprovar a logica em headless (regra 3).
- **FILE_OBJECT com ponteiro de pipe** (`src/nt/io.h`): `FILE_OBJECT` ganhou
  `void* PipeObject`. Um handle de pipe e um `FILE_OBJECT` (OB_TYPE_FILE) com
  `DeviceObject=0` e `PipeObject!=0` — exatamente como no Windows, em que um pipe
  e um file object que o npfs roteia para o buffer. Assim `NtReadFile`/`NtWriteFile`/
  `NtClose` ja existentes funcionam num handle de pipe.
- **Syscalls (SSDT em `src/ke/syscall.c`)**, numeros novos **31..32**, sincronizados
  com `dll/ntdll.c`:
  - 31 `SYS_CREATENAMEDPIPE` -> `sys_createnamedpipe(out HANDLE*, name)`: chama
    `pipe_create` e devolve um handle de pipe (FILE_OBJECT com `PipeObject`).
  - 32 `SYS_CONNECTNAMEDPIPE` -> `sys_connectnamedpipe(HANDLE)`: resolve o
    FILE_OBJECT do pipe e chama `pipe_connect`.
  - `sys_createfile` (NtCreateFile, ja existente) passou a **detectar nomes de
    pipe** (`is_pipe_name`: `\\.\pipe\`, `\Pipe\`, `\Device\NamedPipe\`) e abrir o
    pipe pelo lado cliente (`pipe_open`) em vez de procurar um device — entao o
    `CreateFileA` classico funciona para pipes sem API nova no app.
  - `sys_writefile`/`sys_readfile`: se o `FILE_OBJECT.PipeObject != 0`, roteiam
    para `pipe_write`/`pipe_read` (o buffer do pipe) em vez do IRP do driver.
- **ntdll** (`dll/ntdll.c`): exports `NtCreateNamedPipeFile` (31) e
  `NtConnectNamedPipe` (32) — a unica camada que faz `int 0x80`.
- **kernel32** (`dll/kernel32.c`): `CreateNamedPipeA` (assinatura Win32 completa:
  openMode/pipeMode/maxInstances/tamanhos/timeout/sec; simplificada) -> servidor;
  `ConnectNamedPipe(h, overlapped)` -> BOOL; `CreateFileA("\\.\pipe\Nome")` ja
  existia e agora abre o pipe (o kernel reconhece o nome). `ReadFile`/`WriteFile`/
  `CloseHandle` reusados sem mudanca.
- **DEMO sequencial** (sem escalonador): dois `.exe` no MESMO boot:
  - `examples/pipeserver.c` -> `pipeserver.exe` (PE32+ x64, ImageBase **0x1E00000**):
    `CreateNamedPipeA("\\.\pipe\MeuOSPipe")` + `ConnectNamedPipe` + `WriteFile` da
    mensagem. Nao fecha o handle (deixa o pipe + bytes no namespace para o cliente).
  - `examples/pipeclient.c` -> `pipeclient.exe` (PE32+ x64, ImageBase **0x3000000**,
    na zona morta 48-64 MiB entre o heap e a regiao do PMM): `CreateFileA("\\.\pipe\MeuOSPipe")`
    + `ReadFile` + imprime os bytes recebidos. Roda DEPOIS do servidor.
  - Caminho: ring3 -> kernel32 -> ntdll -> `int 0x80` -> SSDT -> `pipe_*` (buffer no
    Object Manager). Importam kernel32 + user32 (GetStdHandle p/ logar).

### Arquivos alterados / criados
- Novos: `src/nt/pipe.h`, `src/nt/pipe.c`, `examples/pipeserver.c`,
  `examples/pipeclient.c`.
- Alterados: `src/nt/object.h` (OB_TYPE_PIPE), `src/nt/io.h` (FILE_OBJECT.PipeObject),
  `src/ke/syscall.c` (syscalls 31..32 + roteamento de pipe em createfile/writefile/
  readfile), `dll/ntdll.c` (exports Nt* de pipe), `dll/kernel32.c`
  (CreateNamedPipeA/ConnectNamedPipe), `build.ps1` (pipeserver.exe + pipeclient.exe),
  `run.ps1` (os dois exes na lista default, servidor antes do cliente),
  `IMPLEMENTED.md` (esta secao).
- **Estrutura de pastas inalterada**: tudo em `src/nt/`, `dll/`, `examples/` (pastas
  ja existentes). 64-bit e todos os testes anteriores intactos.

### Evidencia (run.ps1 -Headless -TimeoutSec 12)
- Tudo o que ja funcionava continua: **13 linhas `[ok]`**; ambos os drivers
  `DriverEntry retornou status=0x0` (2x); `test.exe` MessageBox "Pinball (teste do
  loader PE)"; `ioctlapp.exe` -> IOCTL `0xCAFEBABE` (3 ocorrencias); `conhello.exe`
  via console device; `test32.exe` (PE32) compat mode; `guiapp.exe` (win32k) pinta a
  janela. `qemu.err.log` **vazio (0 bytes)**; **0** `[EXCECAO]`/`Page Fault`/triple-fault,
  **0** `syscall desconhecida`, **0** `import nao resolvido`; chega em `Sistema no ar.`
- Named Pipe ponta a ponta (servidor -> cliente, no mesmo boot):
  ```
  [boot] aplicativo: pipeserver.exe
    [pipeserver] inicio (ring 3). Criando o Named Pipe...
  [pipe] pipe_create: '\Pipe\MeuOSPipe' criado (OB_TYPE_PIPE, buffer=4096 bytes)
    [pipeserver] CreateNamedPipeA(\\.\pipe\MeuOSPipe) OK.
  [pipe] pipe_connect: servidor de '\Pipe\MeuOSPipe' aguardando cliente (LISTENING)
  [pipe] pipe_write('\Pipe\MeuOSPipe'): 44 bytes -> buffer (ocupacao=44)
    [pipeserver] WriteFile -> pipe; escrevi a mensagem:
      "Ola do servidor via Named Pipe! (IPC FASE 3)"
  [boot] aplicativo: pipeclient.exe
    [pipeclient] inicio (ring 3). Abrindo o Named Pipe pelo nome...
  [pipe] pipe_open: cliente conectou em '\Pipe\MeuOSPipe' (estado=CONNECTED)
    [pipeclient] CreateFileA(\\.\pipe\MeuOSPipe) OK (conectado).
  [pipe] pipe_read('\Pipe\MeuOSPipe'): 44 bytes <- buffer (restam=0)
    [pipeclient] ReadFile <- pipe; recebi do servidor:
      "Ola do servidor via Named Pipe! (IPC FASE 3)"
    [pipeclient] IPC por Named Pipe OK: os bytes atravessaram servidor->cliente.
  ```
  Os 44 bytes escritos pelo servidor sao lidos identicos pelo cliente (buffer
  esvazia: ocupacao=44 -> restam=0). IPC comprovado na serial.

### Limitacoes / proximos passos (nao bloqueiam o boot)
- **Demo sequencial** (sem escalonador): o servidor escreve e termina; o cliente
  le depois. `pipe_connect`/`pipe_open` nao bloqueiam (o NT bloquearia o servidor
  ate um cliente chegar). O pipe e os bytes persistem porque o objeto vive no
  namespace do kernel — basta para o ida-e-volta no mesmo boot.
- **Buffer unico bidirecional** (FIFO simples de 4096 bytes): basta para a demo.
  Um pipe full-duplex real teria dois buffers (um por sentido) e watermarks.
- **ImageBase do cliente (0x3000000)** fica na faixa identidade-mapeada *entre* o
  heap (`[0x2000000,0x3000000)`) e a regiao do PMM (`>=0x4000000`); foi escolhida
  por nao colidir com nenhum alocador (a primeira tentativa, 0x2000000, batia no
  inicio do heap e causava page fault — corrigido).
- **`pipe_create` falha se o nome ja existir** e o objeto nunca e removido do
  namespace (nao ha `Ob` de unlink ainda); para a demo (um pipe, um boot) e o
  esperado.
- Sem suporte a `overlapped`/assincrono, mensagens vs byte-stream, ou multiplas
  instancias do pipe — todos extensoes naturais quando houver escalonador.

## FASE 4 — SCM stubs (advapi32) + syscalls de informacao (NtQuery*)

Status: **completo, build verde, boot verde.** Novas syscalls de informacao
estilo NT (`NtQuerySystemInformation`/`NtQueryInformationProcess` + opcionais
`NtRead`/`NtWriteVirtualMemory`), uma DLL `advapi32.dll` (PE real) com os stubs
do Service Control Manager que **NAO falham** + wrappers de registro apoiados em
`NtOpenKey`/`NtQueryValueKey`, e uma demo `sysinfo.exe` que imprime a versao do
SO, o num de processadores, o PID/ImageBase do processo e os valores de registro.
Nada do que ja funcionava regrediu.

### O que foi implementado
- **Syscalls de informacao (SSDT em `src/ke/syscall.c`)**, numeros novos
  **33..36**, sincronizados com `dll/ntdll.c`:
  - 33 `SYS_QUERYSYSTEMINFO` -> `sys_querysysteminfo(class, buf, buflen, *retlen)`.
    Responde duas classes: `SystemBasicInformation` (0) preenche um
    `SYSTEM_BASIC_INFORMATION` com **num de processadores** (`MEUOS_NUM_PROCESSORS=1`,
    single-CPU sem SMP/APIC ainda), `PageSize=4096`, `AllocationGranularity=4096`,
    `NumberOfPhysicalPages` (via `pmm_total_frames()`), faixa de endereco de
    usuario (0x10000..0x3FFFFFFF); e `MeuOsVersionInformation` (0x1000, classe
    propria) devolve a **string da versao do SO** ("MeuOS 0.1 (kernel 64-bit estilo
    NT)"). Classe desconhecida -> `STATUS_INVALID_INFO_CLASS` (0xC0000003);
    buffer pequeno -> `STATUS_INFO_LENGTH_MISMATCH` (0xC0000004), com `*retlen`
    preenchido (semantica do NT).
  - 34 `SYS_QUERYINFORMATIONPROCESS` -> `sys_queryinformationprocess(hProc, class,
    buf, buflen, *retlen)`. `hProc==0` = processo corrente (`PsGetCurrentProcess`);
    `ProcessBasicInformation` (0) preenche um `PROCESS_BASIC_INFORMATION` com o
    **PID** (`UniqueProcessId`), a **ImageBase** do processo (em `PebBaseAddress`,
    lida do `EPROCESS`), `ExitStatus`, afinidade e prioridade.
  - 35 `SYS_READVIRTUALMEMORY` / 36 `SYS_WRITEVIRTUALMEMORY` (opcionais): copia
    simples na faixa identidade-mapeada (< 1 GiB), com checagem de limite
    (`STATUS_ACCESS_VIOLATION` 0xC0000005 fora da faixa). Como o espaco e
    identidade compartilhada, le/escreve direto na faixa baixa.
  - Novo include `mm/pmm.h` em `syscall.c` (para `pmm_total_frames()`).
- **`pmm_total_frames()`** (`src/mm/pmm.c` + `src/mm/pmm.h`): expoe o total de
  frames geridos (topo da RAM detectada / 4 KiB), usado por
  `NtQuerySystemInformation` para reportar a memoria fisica.
- **ntdll** (`dll/ntdll.c`): exports `NtQuerySystemInformation`,
  `NtQueryInformationProcess`, `NtReadVirtualMemory`, `NtWriteVirtualMemory`
  (a unica camada que faz `int 0x80`; reaproveita `sc4`/`sc5`). Enum de syscalls
  estendido com 33..36.
- **advapi32.dll** (`dll/advapi32.c`, novo) — PE real com export table, igual
  ntdll/kernel32/user32. Vive em **ring 3**; so o ntdll faz syscall.
  - **Service Control Manager (stubs que NAO falham)**:
    `StartServiceCtrlDispatcherA` (sem `services.exe`, **chama a ServiceMain da
    1a entrada** diretamente e retorna TRUE, exercitando o entry do servico),
    `RegisterServiceCtrlHandlerA` (devolve um SERVICE_STATUS_HANDLE sentinela
    nao-nulo), `SetServiceStatus` (aceita e retorna TRUE), `OpenSCManagerA` /
    `CreateServiceA` / `OpenServiceA` (devolvem SC_HANDLE sentinela nao-nulo),
    `StartServiceA` (TRUE), `CloseServiceHandle` (TRUE). Handles sentinela
    ('SCM\0'/'SVR\0'/'SSH\0') tratados como opacos — nunca falham.
  - **Registro apoiado na Native API** (como o advapi32 real): `RegOpenKeyExA`
    (-> `NtOpenKey`), `RegQueryValueExA` (-> `NtQueryValueKey`, com a semantica
    Win32 de `*cbData` entrar com o tamanho do buffer e sair com os bytes; `type`
    recebe `REG_SZ`), `RegCloseKey`. Erro -> `ERROR_FILE_NOT_FOUND` (2); sucesso
    -> `ERROR_SUCCESS` (0).
- **Demo `examples/sysinfo.c`** (`build\sysinfo.exe`, PE32+ x64, ImageBase
  **0x3400000**): chama `NtQuerySystemInformation` (versao do SO + num de CPUs +
  paginas fisicas), `NtQueryInformationProcess` (PID + ImageBase do processo
  atual), o registro via advapi32 (`RegOpenKeyExA`+`RegQueryValueExA`+`RegCloseKey`
  lendo `ProductName`/`CurrentVersion`) e o SCM via advapi32 (`OpenSCManagerA`+
  `CreateServiceA`+`CloseServiceHandle`). Importa kernel32 + ntdll (Nt*) +
  advapi32; loga cada etapa na serial.

### Arquivos alterados / criados
- Novos: `dll/advapi32.c`, `examples/sysinfo.c`.
- Alterados: `src/ke/syscall.c` (syscalls 33..36 + handlers + include de pmm.h),
  `src/mm/pmm.c`, `src/mm/pmm.h` (`pmm_total_frames`), `dll/ntdll.c` (exports
  Nt* de informacao), `build.ps1` (advapi32.dll + sysinfo.exe — este compilado
  DEPOIS das DLLs para linkar contra as nossas import-libs), `run.ps1` (advapi32.dll
  + sysinfo.exe na lista default de modulos), `IMPLEMENTED.md` (esta secao).
- **Estrutura de pastas inalterada**: tudo em `src/ke/`, `src/mm/`, `dll/`,
  `examples/` (pastas ja existentes). 64-bit e todos os testes anteriores intactos.

### Decisao de ImageBase / link
- advapi32.dll em **0x3200000** e sysinfo.exe em **0x3400000**, ambos na **zona
  morta 48-64 MiB** (`[0x3000000,0x4000000)`), fora do heap (`[0x2000000,0x3000000)`)
  e da regiao do PMM (`>=0x4000000`) — sem colidir com nenhum modulo nem alocador.
- sysinfo.exe e compilado **depois** das DLLs (libntdll/libkernel32/libadvapi32
  ja existem) e linkado contra as **nossas import-libs**, garantindo que os nomes
  importados batam exatamente com os exports das nossas DLLs (o loader os resolve
  em runtime pela export table — `[ldr] carregando advapi32.dll @ 0x3200000`).

### Evidencia (run.ps1 -Headless -TimeoutSec 12)
- Tudo o que ja funcionava continua: **13 linhas `[ok]`**; ambos os drivers
  `DriverEntry retornou status=0x0` (10 ocorrencias do status no boot); `test.exe`
  MessageBox "Pinball (teste do loader PE)"; `ioctlapp.exe` -> IOCTL `0xCAFEBABE`
  (3 ocorrencias); `conhello.exe` via console device; `test32.exe` (PE32) compat
  mode com 7 relocacoes; named pipe 44 bytes servidor->cliente; `guiapp.exe`
  (win32k) pinta a janela. `qemu.err.log` **vazio (0 bytes)**; **0** `syscall
  desconhecida`, **0** `import nao resolvido`, **0** `[EXCECAO]`/`Page Fault`/
  triple-fault; chega em `Sistema no ar.`
- FASE 4 ponta a ponta (ring3 sysinfo -> ntdll/advapi32 -> int 0x80 -> SSDT):
  ```
  [boot] aplicativo: sysinfo.exe
  [ps] EPROCESS criado: pid=6 img=sysinfo.exe base=0x0000000003400000 cr3=...
  [ldr] carregando advapi32.dll @ 0x0000000003200000
    [sysinfo] inicio (ring 3). FASE 4: syscalls de informacao + advapi32.
    [sysinfo] versao do SO: "MeuOS 0.1 (kernel 64-bit estilo NT)"
    [sysinfo] processadores = 1 ; page size = 4096 ; paginas fisicas = 65504
    [sysinfo] processo atual: PID = 6 ; ImageBase = 0x0000000003400000
    [sysinfo] RegOpenKeyExA OK.
    [sysinfo] registro: ProductName = "MeuOS"
    [sysinfo] registro: CurrentVersion = "0.1"
    [sysinfo] RegCloseKey OK.
    [sysinfo] OpenSCManagerA OK (handle do SCM nao-nulo).
    [sysinfo] CreateServiceA OK (servico 'MeuOSSvc' registrado - stub).
    [sysinfo] FASE 4 OK: NtQuery* + advapi32 (SCM + registro) exercitados.
  [ps] processo pid=6 encerrou (status=0x0)
  ```
  O `ImageBase` reportado por `NtQueryInformationProcess` (0x3400000) bate com a
  base do EPROCESS; os valores de registro saem do stub do kernel via advapi32.

### Limitacoes / proximos passos (nao bloqueiam o boot)
- **SCM sem `services.exe` real**: os stubs nunca falham e
  `StartServiceCtrlDispatcherA` chama a ServiceMain inline (single-service, sem
  thread/loop de controle). Um SCM de verdade exigiria um processo services.exe,
  IPC (os Named Pipes da FASE 3 servem de base) e um banco de servicos persistente.
- **`NtQuerySystemInformation`** cobre `SystemBasicInformation` + a classe propria
  de versao; outras classes (process list, handle table, tempos) retornam
  `STATUS_INVALID_INFO_CLASS`. **`NtQueryInformationProcess`** cobre
  `ProcessBasicInformation`; `PebBaseAddress` reaproveita a ImageBase (nao ha PEB
  real ainda).
- **`NtRead`/`NtWriteVirtualMemory`** assumem o espaco identidade compartilhado
  (< 1 GiB) e nao trocam de CR3 para o `hProcess` alvo — basta enquanto os
  processos compartilham o mapa do kernel; com isolamento total por processo
  precisariam mapear/atravessar a PML4 do alvo.
- **Registro ainda e stub** (sem hive): `NtQueryValueKey` so conhece
  `ProductName`/`CurrentVersion`; o advapi32 expoe esse stub pela API Win32 fiel,
  mas um registro real (hive em memoria/disco + mais valores) e trabalho futuro.

## FASE 5 — Shell cmd.exe (Command Prompt estilo Windows, ring 3)

Status: **completo, build verde, boot verde.** Um Command Prompt rodando em
**ring 3** (`examples/cmd.c` -> `cmd.exe`) com os comandos `help`, `tasklist`,
`sc query`, `sc start <nome>`, `sc stop <nome>` e `dir`. Os comandos atravessam
o caminho real do SO: ring3 -> kernel32 -> ntdll -> `int 0x80` -> SSDT ->
Object Manager / I/O Manager / loader. `sc start`/`sc stop` carregam e
descarregam **drivers de kernel de verdade** (chamam `DriverEntry`/`DriverUnload`).
Nada do que ja funcionava regrediu.

### O que foi implementado
- **Enumeracao de objetos no Object Manager** (`src/nt/object.c` + `object.h`):
  o Object Manager passou a manter uma **lista global** de TODOS os objetos
  criados (`s_all[]`), alem do namespace por nome (`s_named[]`, so os nomeados).
  Novas `ob_enum_by_type(type, index)` (devolve o n-esimo objeto vivo do tipo) e
  `ob_count_by_type(type)`. Isso permite listar EPROCESS (tasklist) e
  DRIVER_OBJECT (sc query) mesmo sendo objetos **sem nome** (que nao entram no
  `s_named[]`). `ob_init` zera as duas listas.
- **Registro de drivers no I/O Manager** (`src/nt/driver.c` + `driver.h`):
  - Uma tabela `s_drivers[]` (ate 16) com `name`, `state`
    (`DRV_STATE_STOPPED`=1 / `DRV_STATE_RUNNING`=4, espelhando `SERVICE_*` do
    SCM), `laststatus` (status do ultimo `DriverEntry`) e o `DRIVER_OBJECT` vivo
    quando RUNNING. `drv_intern`/`drv_find` (case-insensitive, por basename).
  - `driver_build_and_entry` (helper): faz `pe_map` + `pe_bind_imports` +
    monta o `DRIVER_OBJECT` + chama `DriverEntry`. `driver_call_unload`: chama o
    `DriverUnload` se houver.
  - **`driver_load(name, image)`** (BOOT): carrega, chama `DriverEntry` e
    `DriverUnload` em seguida (como antes), e **registra o driver como STOPPED**
    (disponivel para `sc start`). A assinatura ganhou o `name` (a chamada em
    `kernel.c` passou a `driver_load(path, bytes)`).
  - **`driver_load_by_name(name)`** (`sc start`): acha os bytes do `.sys` pelo
    nome via `ldr_get_module_bytes`, monta o `DRIVER_OBJECT`, chama `DriverEntry`
    e **deixa o driver RODANDO** (NAO chama `DriverUnload`). Idempotente. Devolve
    o `NTSTATUS` do `DriverEntry`.
  - **`driver_unload_by_name(name)`** (`sc stop`): chama o `DriverUnload` do
    driver RUNNING e o marca STOPPED.
  - **`driver_enum(index, &name, &state, &laststatus)`**: enumera o registro
    para o `sc query`.
- **Loader** (`src/loader/loader.c` + `loader.h`): nova
  **`ldr_get_module_bytes(name)`** — bytes brutos de um modulo registrado pelo
  nome (usada pelo I/O Manager para carregar um `.sys` pelo nome no `sc start`).
- **Entrada interativa pelo console** (`src/drivers/keyboard.c` + `keyboard.h`):
  fila circular de **stdin** (256 bytes). Quando NAO ha janelas GUI, a IRQ1
  **ecoa E enfileira** o caractere; quando ha janelas, segue roteando para o
  win32k (sem mudanca). Nova `kbd_stdin_read(dst, max)` (nao bloqueia: copia o
  que houver). O **console device** do `sys_readfile` (`ke/syscall.c`) agora
  **drena essa fila** em vez de devolver sempre 0 — entao `NtReadFile` no stdin
  retorna as teclas digitadas (entrada interativa com display) e 0 bytes quando
  nada foi digitado (headless), sem nunca bloquear.
- **Syscalls novas (SSDT em `src/ke/syscall.c`)**, numeros **37..40**,
  sincronizados com `dll/ntdll.c`:
  - 37 `SYS_ENUMPROCESSES` -> `sys_enumprocesses(index, MEUOS_PROCESS_ENTRY* out)`:
    `ob_enum_by_type(OB_TYPE_PROCESS, index)` -> preenche PID, Terminated,
    ImageBase, ThreadCount, ImageName. RAX=1 enquanto houver, 0 no fim.
  - 38 `SYS_ENUMDRIVERS` -> `sys_enumdrivers(index, MEUOS_DRIVER_ENTRY* out)`:
    `driver_enum` -> State, LastStatus, Name. RAX=1/0.
  - 39 `SYS_LOADDRIVER` -> `sys_loaddriver(name)` = `driver_load_by_name`.
  - 40 `SYS_UNLOADDRIVER` -> `sys_unloaddriver(name)` = `driver_unload_by_name`.
  - As structs `MEUOS_PROCESS_ENTRY`/`MEUOS_DRIVER_ENTRY` so tem campos simples
    (sem ponteiros do kernel); o cmd.exe tem copias com o MESMO layout.
- **ntdll** (`dll/ntdll.c`): exports `NtEnumProcesses`, `NtEnumDrivers`,
  `NtLoadDriver`, `NtUnloadDriver` (a unica camada que faz `int 0x80`); enum
  estendido 37..40.
- **kernel32** (`dll/kernel32.c`): exports `EnumProcessesEx`/`EnumDriversEx`
  (preenchem as structs via ntdll), `StartDriverServiceA`/`StopDriverServiceA`
  (sc start/stop -> BOOL). As structs aqui tem o mesmo layout do kernel.
- **Shell `examples/cmd.c`** (`build\cmd.exe`, PE32+ x64, ImageBase
  **0x3600000** — zona morta 48-64 MiB, ao lado de advapi32/sysinfo, sem
  colidir): saida via `WriteFile` (STD_OUTPUT), helpers de string/numero sem CRT,
  parser de linha (`word`/`ieq`/`istarts`). Comandos: `help`, `tasklist`
  (itera `EnumProcessesEx` 0,1,2,... -> tabela Image/PID/Threads/ImageBase),
  `sc query` (itera `EnumDriversEx` -> SERVICE_NAME/STATE), `sc start <nome>`
  (`StartDriverServiceA`), `sc stop <nome>` (`StopDriverServiceA`), `dir` (stub),
  `cls`, `exit`. `run_line` despacha; `read_line` monta uma linha via `ReadFile`
  (trata Enter/Backspace). **Modo demo** (`demo()`): executa automaticamente
  `help` -> `tasklist` -> `sc query` -> `sc start mydriver.sys` -> `sc query`
  -> `sc stop mydriver.sys` -> `dir`, imprimindo tudo na serial (testavel
  headless). Depois abre o **prompt interativo**: com display a digitacao funciona
  (a IRQ1 enche o stdin); headless o `read_line` devolve 0 (sem teclas) e o shell
  encerra, deixando o boot prosseguir ate "Sistema no ar.".
- **`src/kernel.c`**: a chamada do boot passou a `driver_load(path, bytes)`.

### Arquivos alterados / criados
- Novos: `examples/cmd.c`.
- Alterados: `src/nt/object.c`, `src/nt/object.h` (enumeracao por tipo),
  `src/nt/driver.c`, `src/nt/driver.h` (registro + load/unload por nome + enum),
  `src/loader/loader.c`, `src/loader/loader.h` (`ldr_get_module_bytes`),
  `src/drivers/keyboard.c`, `src/drivers/keyboard.h` (fila de stdin),
  `src/ke/syscall.c` (syscalls 37..40 + `sys_readfile` drena o stdin do console),
  `src/kernel.c` (`driver_load(path, bytes)`), `dll/ntdll.c` (exports Nt* 37..40),
  `dll/kernel32.c` (Enum*/StartDriverService*/StopDriverService*), `build.ps1`
  (compila `cmd.exe` depois das DLLs, linkado contra `libkernel32`/`libntdll`),
  `run.ps1` (`cmd.exe` na lista default, antes do `guiapp.exe`),
  `CHANGELOG.md`, `IMPLEMENTED.md` (esta secao).
- **Estrutura de pastas inalterada**: tudo em `src/nt/`, `src/loader/`,
  `src/drivers/`, `src/ke/`, `dll/`, `examples/` (pastas ja existentes). 64-bit e
  todos os testes anteriores intactos.

### Evidencia (run.ps1 -Headless -TimeoutSec 9..12)
- Tudo o que ja funcionava continua: **13 linhas `[ok]`**; `test.exe` MessageBox
  "Pinball (teste do loader PE)"; `ioctlapp.exe` -> IOCTL `0xCAFEBABE` (3x);
  `conhello.exe` via console device; `test32.exe` (PE32) compat mode com 7
  relocacoes; named pipe 44 bytes servidor->cliente; `sysinfo.exe` FASE 4 OK;
  `guiapp.exe` (win32k) pinta a janela. `qemu.err.log` **vazio (0 bytes)**;
  **0** `syscall desconhecida`, **0** `import nao resolvido`, **0**
  `[EXCECAO]`/`Page Fault`/triple-fault; chega em `Sistema no ar.` O boot
  conclui com folga (cmd encerra ~47 linhas antes do fim) mesmo a 9s.
- FASE 5 ponta a ponta (ring3 cmd -> kernel32/ntdll -> int 0x80 -> SSDT ->
  Object Manager / I/O Manager / loader):
  ```
  [boot] aplicativo: cmd.exe
  [ps] EPROCESS criado: pid=9 img=cmd.exe base=0x0000000003600000 ...
  ============================================================
    MeuOS Command Prompt (cmd.exe) - ring 3 - FASE 5
    Modo DEMO: executando comandos automaticamente.
  ============================================================
  C:\> help
  Comandos do MeuOS cmd.exe:
    help              - mostra esta ajuda
    tasklist          - lista os processos (EPROCESS)
    sc query          - lista os drivers de kernel e o estado
    ...
  C:\> tasklist
  Image Name                   PID   Threads  ImageBase
  =========================  =====  =======  ==================
  test.exe                   1      1        0x0000000000800000  [terminado]
  ...
  cmd.exe                    9      1        0x0000000003600000
  Total: 9 processo(s).
  C:\> sc query
  SERVICE_NAME              STATE
  ========================  ==========
  ioctldriver.sys           1 STOPPED
  mydriver.sys              1 STOPPED
  Total: 2 driver(s).
  C:\> sc start mydriver.sys
  [io] sc start: carregando driver 'mydriver.sys'...
  [io] chamando DriverEntry...
    [DbgPrint] MeuDriver: DriverEntry — driver de kernel rodando no MeuOS!
    [DbgPrint] MeuDriver: IoCreateDevice OK — device object criado.
  [io] DriverEntry retornou status=0x...0  (STATUS_SUCCESS)
    [SC] driver iniciado (RUNNING).
  C:\> sc query
  mydriver.sys              4 RUNNING            <- estado mudou para RUNNING
  C:\> sc stop mydriver.sys
  [io] sc stop: descarregando driver 'mydriver.sys'...
  [io] chamando DriverUnload...
    [DbgPrint] MeuDriver: DriverUnload chamado — driver descarregado.
    [SC] driver parado (STOPPED).
  C:\> dir   ... (stub)
  [cmd] modo demo concluido. ...
  [cmd] shell encerrado.
  [ps] processo pid=9 encerrou (status=0x...0)
  ```
  `tasklist` listou os 9 EPROCESS (prova a enumeracao do Object Manager);
  `sc start`/`sc stop` chamaram o `DriverEntry`/`DriverUnload` REAIS do
  `mydriver.sys` (os `DbgPrint` do driver aparecem) e o `sc query` mostrou a
  transicao STOPPED -> RUNNING -> (STOPPED).

### Limitacoes / proximos passos (nao bloqueiam o boot)
- **`sc start`/`sc stop` cobrem drivers de kernel (`.sys`)**: nao ha
  `services.exe` nem servicos de usuario (Win32 services); o "registro de
  servicos" e o registro de drivers do I/O Manager. Os stubs do SCM em ring 3
  ficam em `advapi32.dll` (FASE 4).
- **`dir` e stub** (sem sistema de arquivos): lista entradas fixas e avisa.
- **Entrada interativa so com display**: headless o shell roda o modo demo e
  encerra (sem teclado, `ReadFile` devolve 0). O `read_line` desiste depois de
  muitas leituras vazias para o boot poder concluir; com display, cada tecla
  (IRQ1) reseta esse contador e a linha e lida normalmente. O stdin do console
  so e alimentado quando NAO ha janelas GUI (por isso o `cmd.exe` roda antes do
  `guiapp.exe`).
- **`sc start` remapeia o `.sys` no mesmo ImageBase** a cada start (sem unload do
  mapeamento anterior): basta para a demo; um gerenciador real liberaria as
  paginas no stop.

## FASE 6 — Desktop + barra de tarefas + cmd numa janela (integracao GUI+CMD)

Status: **completo, build verde, boot verde.** Compoe um ambiente grafico
estilo Windows sobre o win32k (FASE 2): **papel de parede** (janela de fundo) +
**barra de tarefas** + **uma ou mais janelas de CONSOLE** rodando o shell `cmd`
DENTRO da janela — o texto do shell e desenhado via `gdi32` (`TextOutA`) na area
cliente, e o teclado e roteado pela **fila de mensagens** da janela com foco
(`WM_CHAR`). Demo abre 2 janelas de cmd, executa comandos reais em cada uma
(`tasklist` via Object Manager, `sc query`/`sc start` via I/O Manager) e alterna
o foco. Nada do que ja funcionava regrediu (test.exe, IOCTL 0xCAFEBABE, conhello,
test32, named pipe, sysinfo, cmd FASE 5, guiapp FASE 2 — todos intactos).

### O que foi implementado
- **Desktop + barra de tarefas no win32k (lado KERNEL)** (`src/win32/win32k.c`
  + `win32k.h`):
  - **Janela de fundo (papel de parede)** — flag `WNDF_DESKTOP`: cobre a tela
    inteira (320x200), **sem chrome** (sem barra de titulo/moldura), cor de fundo
    propria, fica no **fundo do z-order** e **nao toma o foco** (o teclado vai
    para as janelas de cima). O `compose` a desenha primeiro.
  - **Janela de console** — flag `WNDF_CONSOLE`: area cliente escura (cor
    configuravel, azul-marinho na demo), com barra de titulo + moldura; recebe
    foco e teclado normalmente. Serve de "console window" para o shell.
  - **`W32_CREATE` ganhou `bgColor` + `flags`** (campos no FIM, layout compativel:
    o `CreateWindowExA` antigo preenche `bgColor=WND_BG_DEFAULT` => cinza da Fase 2
    e `flags=0`, entao o `guiapp` continua identico). O `WND` guarda `bgColor`,
    `flags` e o **`owner` (EPROCESS)**.
  - **`win32k_compose` desenha a BARRA DE TAREFAS** no rodape (sempre por cima):
    botao **"Iniciar"** (verde) + **um botao por janela top-level** visivel (o da
    janela com foco fica destacado/afundado). Loga `[win32k] taskbar: [Iniciar]
    [cmd #1] [cmd #2*]` (o `*` marca o foco). O papel de parede NAO ganha botao.
  - **Reaping de janelas por processo** (`win32k_reap_process_windows(eproc)`):
    quando um app GUI encerra, **todas as janelas dele sao liberadas** (igual ao
    NT), evitando janelas "fantasma" entre apps. Chamado no `ldr_run` ao app
    retornar. O **framebuffer permanece** como o app pintou (para o screendump).
  - **`win32k_was_active()`**: 1 se a GUI ja compos a tela alguma vez. O `kmain`
    passou a usar isto (em vez de `has_windows`) para NAO rodar a `fb_demo()` da
    Fase 1 por cima do desktop — mesmo depois das janelas serem reaped (a tela
    grafica permanece).
  - **Coalescing de `WM_PAINT`** (igual ao Windows: varias invalidacoes viram UMA
    pintura pendente) + **fila de mensagens ampliada para 256** (a demo injeta
    dezenas de teclas para 2 janelas ANTES de drenar a fila; 64 estourava e
    perdia o `WM_QUIT`).
  - **`NtUserSetFocus_k`** (da o foco a uma janela — clique/Alt+Tab; recompoe e a
    barra de titulo + o botao da barra de tarefas refletem) e **`NtUserPostKey_k`**
    (posta `WM_KEYDOWN`+`WM_CHAR` para UMA janela especifica, independente do foco
    — usado pela demo para alimentar varias janelas em headless de forma
    deterministica; o teclado real por IRQ1 usa o caminho do foco).
  - **`NtGdiTextOutEx_k`**: `TextOut` com **cor de texto explicita** (para o
    console: texto claro sobre fundo escuro), com **clipping vertical** a area
    cliente (o texto nao invade a barra de titulo nem ultrapassa a base da janela).
    O `NtGdiTextOut_k` (Fase 2, texto preto) virou um caso particular.
- **Syscalls novas (SSDT em `src/ke/syscall.c`)**, numeros **41..43**
  sincronizados com `dll/ntdll.c`: `NtUserSetFocus`(41), `NtUserPostKey`(42),
  `NtGdiTextOutEx`(43).
- **DLLs (ring 3)**:
  - `ntdll.dll`: exports `NtUserSetFocus`/`NtUserPostKey`/`NtGdiTextOutEx`
    (a unica camada que faz `int 0x80`).
  - `user32.dll`: `SetFocus`, `PostKeyToWindow` (injecao direcionada p/ a demo),
    `CreateDesktopWindowA` (papel de parede: `WNDF_DESKTOP`) e
    `CreateConsoleWindowA` (console: `WNDF_CONSOLE` + cor de fundo). O
    `CreateWindowExA` foi refatorado sobre um helper comum (preenche
    `bgColor`/`flags`); a API antiga e o `guiapp` ficam identicos.
  - `gdi32.dll`: `SetTextColor` (cor de texto **por HDC**, igual ao Windows) +
    `TextOutA` agora usa `NtGdiTextOutEx` quando ha cor definida (console), senao
    `NtGdiTextOut` (texto preto, compat. Fase 2).
- **Demo `examples/desktop.c`** (`build/desktop.exe`, PE32+ x64, ImageBase
  **0x3800000** — zona morta 48-64 MiB, ao lado do cmd, sem colidir). Linka contra
  as nossas import-libs (`libkernel32.a`/`libuser32.a`/`libgdi32.a`/`libntdll.a`).
  Fluxo: registra as classes (`MeuDesktop` + `MeuConsole`); cria o papel de parede
  (`CreateDesktopWindowA`) + 2 janelas de cmd (`CreateConsoleWindowA`, lado a
  lado); "digita" comandos em cada janela (via `PostKeyToWindow`, determinismo
  headless) alternando o foco (`SetFocus`); roda o **loop classico**
  `GetMessage`/`TranslateMessage`/`DispatchMessage`. O **WNDPROC do console**
  trata `WM_PAINT` (redesenha a grade de texto via `TextOutA` branco sobre o fundo
  escuro), `WM_CHAR` (monta a linha de comando; no Enter executa o comando e
  desenha a saida na janela), `WM_CREATE`/`WM_DESTROY`. Os comandos `help`/`ver`/
  `tasklist`/`sc query`/`sc start`/`sc stop`/`cls`/`exit` reusam as syscalls da
  FASE 5 (`EnumProcessesEx`/`EnumDriversEx`/`StartDriverServiceA`), entao
  `tasklist` enumera os EPROCESS de verdade e `sc start` chama o `DriverEntry`
  real. Cada saida e desenhada na janela **E** ecoada na serial (`[win] ...`) para
  comprovar a logica em headless. O `exit` (ultima tecla) repinta o papel de
  parede e posta `WM_QUIT` no fim da fila (quadro final completo e limpo).
- **kmain** (`src/kernel.c`): a decisao final passou a ser "se a GUI ja tomou a
  tela (`win32k_was_active`), mantem o framebuffer no estado da GUI; senao roda a
  `fb_demo()` da Fase 1". Assim o desktop (com as janelas reaped) permanece na
  tela para o screendump.
- **build.ps1 / run.ps1**: `desktop.exe` compilado depois das DLLs (linkado contra
  as import-libs proprias) e adicionado a lista default de modulos por **ULTIMO**
  (apos `guiapp.exe`), para o desktop ser o estado visual final. O `guiapp` roda
  antes e suas janelas sao reaped ao ele encerrar — o desktop sobe numa tabela de
  janelas limpa.

### Arquivos alterados / criados
- Novos: `examples/desktop.c`.
- Alterados: `src/win32/win32k.h`, `src/win32/win32k.c` (desktop/taskbar/console,
  reaping, was_active, coalescing, fila 256, SetFocus/PostKey/TextOutEx),
  `src/ke/syscall.c` (syscalls 41..43), `src/loader/loader.c` (reaping ao app
  encerrar), `src/kernel.c` (`win32k_was_active` na decisao final), `dll/ntdll.c`,
  `dll/user32.c`, `dll/gdi32.c`, `build.ps1`, `run.ps1`, `IMPLEMENTED.md`.
- **Estrutura de pastas inalterada**: tudo em `src/win32/`, `src/ke/`,
  `src/loader/`, `dll/`, `examples/` (pastas ja existentes). 64-bit e todos os
  testes anteriores intactos.

### Evidencia (run.ps1 -Headless -TimeoutSec 10 e 14)
- **GREEN**: `qemu.err.log` **0 bytes**; **13 linhas `[ok]`**; **`Sistema no ar`**
  alcancado; **0** `[EXCECAO]`, **0** `Page Fault`, **0** `syscall desconhecida`,
  **0** `import nao resolvido`. Reproduzivel tanto a 10s quanto a 14s.
- **Testes anteriores intactos (mesmo boot)**: test.exe MessageBox "Pinball (teste
  do loader PE)" (1); IOCTL `0xCAFEBABE` (3); conhello WriteFile "Ola do RING 3 via
  GetStdHandle" (1); test32 PE32 compat mode com 7 relocacoes (1); `DriverEntry
  status=0x0` (4); named pipe 44 bytes (2); sysinfo FASE 4 (3); cmd FASE 5 "shell
  encerrado" (1); guiapp FASE 2 WM_PAINT (1).
- **FASE 6 ponta a ponta** (ring3 desktop -> user32/gdi32 -> ntdll -> int 0x80 ->
  win32k -> framebuffer), logs da serial:
  ```
  [win32k] reaping: 1 janela(s) do processo que encerrou.   <- guiapp limpo antes do desktop
    MeuOS Desktop (FASE 6): wallpaper + barra de tarefas + cmd
  [win32k] CreateWindowEx -> HWND #2 ... titulo='wallpaper' x=0 y=0 w=320 h=200 [DESKTOP/wallpaper]
  [win32k] CreateWindowEx -> HWND #3 ... titulo='cmd #1' ... [CONSOLE]
  [win32k] CreateWindowEx -> HWND #4 ... titulo='cmd #2' ... [CONSOLE]
  [win32k] taskbar: [Iniciar] [cmd #1] [cmd #2*]            <- barra de tarefas, * = foco
  [win32k] SetFocus -> HWND #3 ('cmd #1')                   <- alterna o foco
  [win32k] SetFocus -> HWND #4 ('cmd #2')
    [desktop] (demo) digitando em HWND: "tasklist"          <- teclas pela fila de msg
    [win] PID  Imagem ...  [win]  1   test.exe ... [win] Total: 11   <- tasklist na janela (Object Manager)
    [win] sc start mydriver.sys / [io] chamando DriverEntry... /
    [DbgPrint] MeuDriver: DriverEntry — driver de kernel rodando no MeuOS! /
    [io] DriverEntry retornou status=0x0 / [win]   -> RUNNING        <- sc start na janela (I/O Manager)
  [gdi] TextOut(HDC, x=2, y=10, fg=15, " 1   test.exe") -> desktop(11,39)  <- texto branco DENTRO da janela #3
    [desktop] loop encerrado (WM_QUIT). Desktop montado; encerrando demo.
  [win32k] reaping: 3 janela(s) do processo que encerrou.   <- wallpaper + 2 consoles
  [win32k] estado final: desktop + barra de tarefas + janela(s) de cmd.
  ```
- **SCREENDUMP confirmado** (`run.ps1 -Headless -Screendump`): `build/screen.ppm`
  (P6 640x400, QEMU dobra o mode 13h). Convertido para PNG e inspecionado com a
  ferramenta Read: **desktop azul** com a marca d'agua "MeuOS Desktop - FASE 6"
  no canto; **janela 'cmd #1'** (barra de titulo ativa) com a saida do `tasklist`
  (os 11 processos + "Total: 11" + "encerrando..."); **janela 'cmd #2'** com a
  saida de `sc query` (ioctldriver.sys/mydriver.sys STOPPED) e `sc start` ("->
  RUNNING"); e a **barra de tarefas** no rodape com "[Iniciar] [cmd #1] [cmd #2]"
  — **bate exatamente** com os comandos logados (prova os pixels, nao so o log).

### Limitacoes / proximos passos (nao bloqueiam o boot)
- Nada foi revertido: o desktop, a barra de tarefas e o cmd-numa-janela ficaram de
  pe com build/boot verdes.
- **Uma fila de mensagens global** (uma thread GUI por vez): o `desktop.exe`
  hospeda TODAS as janelas (wallpaper + consoles) num unico processo e num unico
  loop de mensagens — `DispatchMessage` roteia cada `MSG` para o WNDPROC da janela
  alvo (em ring 3). Multiplos processos GUI concorrentes exigiriam escalonador +
  fila por-thread; aqui o OS roda os `.exe` em sequencia.
- **Sem mouse**: nao ha clique de verdade na barra de tarefas / nas janelas (falta
  driver de mouse PS/2, IRQ12). O foco e trocado por `SetFocus` (programatico) e a
  demo injeta teclas com `PostKeyToWindow`; um teclado real (IRQ1) digita na janela
  com foco. Botoes da barra de tarefas sao visuais (sem hit-testing de clique).
- **Compositing sem clipping entre janelas e sem repaint automatico pos-recompose**:
  o `compose` redesenha o chrome e LIMPA a area cliente; o conteudo (texto do shell)
  e redesenhado pelos `WM_PAINT` (a grade de texto persiste no proprio app). A
  marca d'agua do papel de parede e repintada por ultimo no encerramento para
  sobreviver ao ultimo compose. Sobreposicao parcial de janelas pode deixar
  artefatos ate o proximo `WM_PAINT` (modelo herdado da Fase 2).
- **Console "grade de texto" simples**: cada janela tem um backing store de
  ~36x22 celulas (fonte 8x8); rola 1 linha ao encher. Sem cursor piscante, sem
  selecao, sem cores por trecho (tudo branco; so o `SetTextColor` por HDC). Basta
  para desenhar a saida do shell.
- **Resolucao herdada da Fase 1** (mode 13h 320x200): as janelas sao pequenas
  (poucas colunas/linhas) e ficam clipadas ao desktop. VBE/LFB maior continua
  sendo o stretch da Fase 1.
- **Entrada da demo via `PostKeyToWindow`** (uma vez, no inicio): torna o teste
  headless deterministico (sem depender de digitacao). Com `-display`, o teclado
  real (IRQ1) roteia para a janela com foco normalmente; clicar para trocar de
  janela ainda depende do mouse (acima).

## Rodada GUI/CMD — resumo consolidado (FASES 1..6)

Status global: **build verde e boot verde, nada revertido.** Esta rodada levou o
MeuOS do "roda `.exe`/`.sys` em modo texto" para um **ambiente grafico estilo
Windows** (framebuffer + window manager + DLLs `user32`/`gdi32`/`advapi32`) com um
**shell `cmd`** e um **desktop com barra de tarefas**, mais **IPC por Named Pipes**
e **syscalls de informacao/SCM**. Tudo incremental: cada fase compilou e bootou
verde antes da seguinte; os testes anteriores seguem intactos no MESMO boot.

As secoes detalhadas de cada fase (o que/arquivos/decisoes/limitacoes/evidencia)
estao logo acima neste arquivo (FASE 1 .. FASE 6). Esta secao e o **mapa por
fase**: tema, arquivos-chave, status e a evidencia (serial/screendump).

### Tabela por fase

| Fase | Tema | Syscalls novas | Status | Evidencia |
|------|------|----------------|--------|-----------|
| 1 | **Framebuffer grafico** (VGA mode 13h 320x200x256) + **GDI de baixo nivel no kernel** (`fb_*`) + fonte 8x8 + demo no boot | — (so kernel) | **Funciona** | serial `[fb] ...` (mode 13h ativo, clear/rect/fill/text, `fb_get_pixel`=1) + **screendump** (desktop azul + janela + 16 swatches) |
| 2 | **Win32k** (HWND/z-order/foco, fila de mensagens, objetos GDI) + **`user32.dll`/`gdi32.dll`** reais + demo `guiapp.exe` (ring 3) que pinta no `WM_PAINT` | **17..30** (NtUser*/NtGdi*) | **Funciona** | serial (RegisterClass/CreateWindowEx/compose/`[gdi] FillRect`+`TextOut`/WM_CHAR/WM_QUIT) + **screendump** (janela "MeuOS GUI" + retangulo vermelho "WM_PAINT OK") |
| 3 | **Named Pipes (IPC)**: `OB_TYPE_PIPE` + namespace `\Pipe\Nome` + buffer 4096 + roteamento de `NtRead/WriteFile`/`NtCreateFile`; demo servidor->cliente | **31..32** (NtCreateNamedPipeFile/NtConnectNamedPipe) | **Funciona** (sequencial, sem escalonador) | serial `[pipe] ...` — 44 bytes escritos pelo servidor lidos identicos pelo cliente (ocupacao=44 -> restam=0) |
| 4 | **`advapi32.dll`** (SCM stubs que nao falham + registro via Native API) + **syscalls de informacao** (`NtQuerySystemInformation`/`NtQueryInformationProcess` + `NtRead/WriteVirtualMemory`); demo `sysinfo.exe` | **33..36** | **Funciona** (SCM = stub; 1 classe por NtQuery; registro = stub) | serial `[sysinfo]` (versao do SO, 1 CPU, 65504 paginas, PID/ImageBase, ProductName/CurrentVersion, OpenSCManager/CreateService OK) |
| 5 | **Shell `cmd.exe`** (ring 3): `help`/`tasklist`/`sc query`/`sc start`/`sc stop`/`dir` — `tasklist` enumera EPROCESS (Object Manager), `sc start/stop` carrega/descarrega `.sys` real (I/O Manager) | **37..40** (NtEnumProcesses/NtEnumDrivers/NtLoadDriver/NtUnloadDriver) | **Funciona** (interativo so com display; `dir` stub; sem services.exe) | serial — `tasklist` lista 9 procs; `sc start mydriver.sys` chama o `DriverEntry` real (DbgPrint do driver); `sc query` mostra STOPPED->RUNNING->STOPPED |
| 6 | **Desktop + barra de tarefas + `cmd` numa janela** (integra GUI+CMD): papel de parede + taskbar no win32k, 2 janelas de console rodando o shell, troca de foco, reaping por processo | **41..43** (NtUserSetFocus/NtUserPostKey/NtGdiTextOutEx) | **Funciona** (1 fila global, sem mouse, console = grade 36x22) | serial (reaping, CreateWindowEx DESKTOP/CONSOLE, `taskbar: [Iniciar] [cmd #1] [cmd #2*]`, SetFocus, `tasklist` 11 procs + `sc start`->RUNNING DENTRO da janela) + **screendump** (desktop + 2 janelas cmd + barra de tarefas) |

> Numeracao de syscalls **contigua 1..43** (1..16 = base anterior; **17..43 = esta
> rodada**), sincronizada entre o SSDT (`src/ke/syscall.c`) e o `enum` do
> `dll/ntdll.c`. Proximo numero livre = **44**.

### Arquivos-chave por fase (resumo)

- **Fase 1 (kernel/video):** `src/drivers/video.c`/`.h`, `src/drivers/font8x8.c`,
  `src/kernel.c` (`fb_demo`), `run.ps1` (`-Screendump`/`-QmpPort`).
- **Fase 2 (win32k + DLLs GUI):** `src/win32/win32k.c`/`.h`, `dll/user32.c`,
  `dll/gdi32.c` (novo), `dll/ntdll.c`, `src/drivers/keyboard.c` (roteia IRQ1 ->
  win32k), `examples/guiapp.c`, `src/ke/syscall.c` (17..30).
- **Fase 3 (IPC):** `src/nt/pipe.c`/`.h` (novos), `src/nt/object.h`
  (`OB_TYPE_PIPE`), `src/nt/io.h` (`FILE_OBJECT.PipeObject`), `dll/kernel32.c`,
  `dll/ntdll.c`, `examples/pipeserver.c`/`pipeclient.c`, `src/ke/syscall.c` (31..32).
- **Fase 4 (info + SCM):** `dll/advapi32.c` (novo), `src/mm/pmm.c`/`.h`
  (`pmm_total_frames`), `dll/ntdll.c`, `examples/sysinfo.c`, `src/ke/syscall.c`
  (33..36).
- **Fase 5 (shell cmd):** `examples/cmd.c` (novo), `src/nt/object.c`/`.h`
  (`ob_enum_by_type`), `src/nt/driver.c`/`.h` (registro + load/unload por nome),
  `src/loader/loader.c`/`.h` (`ldr_get_module_bytes`), `src/drivers/keyboard.c`
  (fila de stdin), `dll/kernel32.c`, `dll/ntdll.c`, `src/ke/syscall.c` (37..40).
- **Fase 6 (desktop):** `examples/desktop.c` (novo), `src/win32/win32k.c`/`.h`
  (desktop/taskbar/console, reaping, coalescing, fila 256), `src/loader/loader.c`
  (reaping ao app encerrar), `src/kernel.c` (`win32k_was_active`), `dll/user32.c`,
  `dll/gdi32.c`, `dll/ntdll.c`, `src/ke/syscall.c` (41..43).
- **Wiring (todas as fases):** `build.ps1` (compila as novas DLLs/exes, linkando
  os exes contra as nossas import-libs DEPOIS das DLLs) e `run.ps1` (modulos na
  lista default; ordem importa: `cmd.exe` antes do `guiapp.exe`, `desktop.exe`
  por ULTIMO).

### Evidencia consolidada do boot final (run.ps1 -Headless -TimeoutSec 10)

`qemu.err.log` **= 0 bytes**; **13 linhas `[ok]`**; chega em **`Sistema no ar.`**;
**0** `[EXCECAO]`, **0** `Page Fault`, **0** triple-fault, **0** `syscall
desconhecida`, **0** `import nao resolvido`, **0** `NAO IMPLEMENTADO`. Contadores
por feature no MESMO boot (verificados): `DriverEntry status=0x0` = 4;
`0xCAFEBABE` = 3; "Pinball (teste do loader PE)" = 1; PE32 `.reloc` = 7 = 1;
named pipe "44 bytes" = 2; "FASE 4 OK" = 1; cmd "shell encerrado" = 1; guiapp
`WM_PAINT` = 1; desktop "loop encerrado" = 1; "estado final: desktop + barra de
tarefas..." = 1.

### O que ficou pendente (transversal, nao bloqueia o boot)

- **Escalonador / preempcao:** nao ha. Os `.exe` rodam em **sequencia**; por isso a
  fila de mensagens do win32k e **global** (uma thread GUI por vez), o
  `NtWaitForSingleObject` nao bloqueia, e a demo de pipe e sequencial
  (servidor escreve e termina, cliente le depois).
- **Mouse:** sem driver PS/2 (IRQ12). O foco e trocado por `SetFocus`
  (programatico) e a demo injeta teclas com `PostKeyToWindow`; o teclado real
  (IRQ1) ja roteia para a janela com foco, mas os botoes da barra de tarefas sao
  **visuais** (sem hit-testing de clique).
- **Resolucao:** **mode 13h (320x200x256)** herdado da Fase 1 — janelas pequenas,
  consoles de ~36x22. O stretch VBE/LFB (resolucao maior com LFB linear) exigiria
  mapear a faixa fisica do LFB nas page tables; ficou de fora pelo caminho seguro.
- **Compositing:** sem clipping entre janelas e sem repaint automatico pos-
  recompose (o conteudo do shell e redesenhado nos `WM_PAINT`; sobreposicao parcial
  pode deixar artefato ate o proximo paint).
- **Sistema de arquivos:** nao ha — `dir` e stub; o registro e stub (sem hive,
  so `ProductName`/`CurrentVersion`); o SCM nao tem `services.exe` (stubs em
  `advapi32` que nunca falham).
- **DLLs do sistema em 32-bit:** so 64-bit; um PE32 que importe Win32 ainda cairia
  em "import nao resolvido" (o `test32.exe` e autocontido, faz `int 0x80` direto).

# ============================================================================
# RODADA HAL/NTFS — resumo consolidado (5 fases)
# ============================================================================

Esta rodada acrescentou uma **HAL** (Hardware Abstraction Layer) estilo Windows NT
e um **driver NTFS** (leitura + escrita do subconjunto seguro) ligado ao I/O Manager,
mais o disco IDE (ATA PIO) e a imagem NTFS de teste. **Tudo com build verde e boot
verde** (nada revertido); todos os testes anteriores seguem intactos no MESMO boot
(IOCTL `0xCAFEBABE`, ring3, PE32, named pipes, cmd `sc`/`tasklist`, win32k, desktop).

| Fase | Tema | Arquivos (principais) | Status | Evidencia serial |
|------|------|------------------------|--------|------------------|
| 1 | **HAL core** — I/O ports (`HalReadPort*`/`HalWritePort*`), MMIO (`hal_map_mmio`, recusa >1 GiB), **enumeracao PCI** (config #1, 0xCF8/0xCFC), `hal_init()` no boot | `src/hal/hal.h`, `src/hal/hal.c`, `src/kernel.c` | **Funciona** | `[ok] HAL`; `[hal]` x18; **9** linhas `vendor=0x` (PCI): IDE/ATA `8086:7010` (BAR4 I/O 0xC040), VIDEO `1234:1111`, host bridge `8086:1237`, ISA, e1000 |
| 2 | **HAL disco** — IDE ATA PIO LBA28 (`HalReadSector`/`HalWriteSector` + IDENTIFY), imagem NTFS de teste, `run.ps1 -Disk`, teste de boot le MBR + boot sector NTFS (`"NTFS    "` @3) | `src/hal/disk.{c,h}`, `run.ps1`, `examples/make-ntfs-disk.ps1`, `examples/make-ntfs-image.py`, `src/kernel.c` | **Funciona** | `[ok] HAL disco`; `IDENTIFY OK modelo='QEMU HARDDISK'`; `MBR ... 0x55AA OK`; `particao NTFS no LBA 2048`; `assinatura 'NTFS    ' confirmada -> OK`; write/readback nao destrutivo |
| 3 | **NTFS LEITURA** — montar BPB+`$MFT`, parse de registro MFT (FILE+fixups USA), atributos residentes/nao-residentes (data runs), ler `$DATA`, listar diretorio (`$INDEX_ROOT`/`$INDEX_ALLOCATION`), resolver caminho; **camada de FS no I/O Manager** (`\Device\Harddisk0\Partition1`, IRP_MJ_CREATE/READ/DIRECTORY_CONTROL) + `NtQueryDirectoryFile` (syscall 44) | `src/drivers/ntfs.{c,h}`, `src/drivers/ntfs_fs.c`, `src/ke/syscall.c`, `src/nt/io.h`, `sdk/ntddk.h`, `dll/ntdll.c`, `dll/kernel32.c`, `examples/cmd.c` | **Funciona** | `volume NTFS MONTADO`; raiz `[0] hello.txt`, `[1] <DIR> dir1`; `\hello.txt -> MFT #24` conteudo == texto conhecido; `\dir1\file.txt -> MFT #26`; IRPs reais devolvem os bytes; `cmd dir`/`type` em ring 3 |
| 4 | **NTFS ESCRITA (subconjunto seguro)** — reescrever registro MFT (`ntfs_write_mft_record`, fixups na escrita), sobrescrever/crescer/encurtar `$DATA` residente, overwrite de `$DATA` nao-residente in-place, **criar** arquivo/dir (registro novo + `$INDEX_ROOT` do pai) e **excluir**; IRP_MJ_WRITE no I/O Manager + `NtWriteFile` no volume | `src/drivers/ntfs.{c,h}`, `src/drivers/ntfs_fs.c`, `src/ke/syscall.c`, `src/kernel.c` | **Funciona (subconjunto seguro)** | SHA-256 de `disk.img` MUDA apos o boot (escrita persistiu); sobrescrita `\hello.txt`, grow 68->191B, criar `\novo.txt` (#27), excluir; releitura do disco confirma cada uma; IRP_MJ_WRITE em `\dir1\file.txt` |
| 5 | **FS no ring 3** — volume montado como **C:** (`C:\path`/`C:`), `NtQueryVolumeInformation` (syscall 45), `cmd.exe` com `dir`/`cd`/`type`/`copy`/`vol` (+`del` stub) | `src/ke/syscall.c`, `src/drivers/ntfs.{c,h}`, `dll/ntdll.c`, `dll/kernel32.c`, `examples/cmd.c`, `src/kernel.c` | **Funciona** | `[ok] NTFS: volume montado como C:`; `vol` -> `rotulo='MEUOS' fs=NTFS total=66060288 bytes`; `dir`/`type`/`cd dir1`/`copy` ponta a ponta do ring 3; **0** "comando nao reconhecido" |

**Evidencia final consolidada** (`build.ps1` + `run.ps1 -Headless -Disk -TimeoutSec 14`,
tudo no MESMO boot; `qemu.err.log` = **0 bytes**): **16** linhas `[ok]`; **0**
`[EXCECAO]`/`Page Fault`/triple-fault; **0** `syscall desconhecida`; **0** `import
nao resolvido`; **3x** `0xCAFEBABE`; `DriverEntry status=0x0`; `Sistema no ar.` x1.
Sem `-Disk` (default) o boot tambem fica verde com **15** `[ok]` (sem a linha do NTFS)
e os comandos de arquivo do `cmd` degradam com aviso gracioso — **sem regressao**.

**Blockers / pendencias honestas** (NAO quebram o boot):
- **Alocacao de clusters (`$Bitmap` do volume) nao implementada** — a escrita so usa
  espaco ja alocado: sobrescrever/crescer `$DATA` residente (dentro do registro MFT) e
  overwrite de `$DATA` nao-residente nos clusters existentes. Crescer alem do registro
  (residente -> nao-residente) ou criar `$DATA` grande **e recusado com seguranca**
  (loga + sobrescrita pura, sem corromper). Journaling (`$LogFile`) tambem nao foi feito.
- **Imagem de teste sintetica (sem admin):** o ambiente nao esta elevado (Administrador),
  entao a imagem foi gerada pelo modo **SINTETICO** (`make-ntfs-image.py`, Python, sem
  admin): NTFS auto-consistente com MBR + boot sector `"NTFS    "` + `$MFT` minima
  (`\hello.txt` residente, `\dir1\file.txt`, `$INDEX_ROOT`). Os caminhos de `$DATA`
  **nao-residente / data runs / `$INDEX_ALLOCATION`** (INDX) na leitura E na escrita
  estao implementados com protecao de limites, mas so seriam *exercitados* contra um
  **volume NTFS 100% autentico**. Para isso, gere com `examples\make-ntfs-disk.ps1`
  num **PowerShell ELEVADO** (`Format-Volume NTFS` exige admin) — ver "Como criar a
  imagem NTFS de teste" no README.
- **`$ATTRIBUTE_LIST` (registros de extensao) nao seguido**; **`$MFT:$BITMAP`** nao
  atualizado ao criar (o NOSSO leitor ve o arquivo; chkdsk do Windows exigiria marcar);
  **`$INDEX_ALLOCATION` na escrita** so mexe no `$INDEX_ROOT` residente. **Espaco livre
  do `vol` e ESTIMATIVA** (~7/8; total/serial/fs/bytes-por-setor sao exatos do BPB).
- **APIC/HPET / IRQ registravel** NAO feitos de proposito (stretch opcional permitido):
  mantidos PIC 8259 + PIT atuais. MMIO acima de 1 GiB e recusado por `hal_map_mmio`
  (caminho seguro; um LFB linear exigiria estender a paginacao).

Os detalhes por fase (decisoes, todos os arquivos, evidencia completa e limitacoes)
seguem nas secoes **FASE 1 (HAL)** .. **FASE 5** abaixo.

## FASE 1 (HAL) — Hardware Abstraction Layer (I/O ports + MMIO + enumeracao PCI)

Status: **completo, build verde, boot verde.** Nova pasta `src/hal/` com a camada
de abstracao de hardware no estilo da HAL.DLL do Windows NT: portas de I/O,
acesso MMIO e **enumeracao PCI** (config space via 0xCF8/0xCFC). `hal_init()` e
chamado no boot e loga cada dispositivo PCI achado na serial (regra 4). Nada do
que ja funcionava regrediu.

### O que foi implementado
- **I/O ports no estilo NT HAL** (`src/hal/hal.c` + `src/hal/hal.h`):
  `HalReadPortUchar`/`Ushort`/`Ulong` e `HalWritePortUchar`/`Ushort`/`Ulong`,
  todas `__attribute__((ms_abi))` (imitam a HAL do Windows; o config space do PCI
  precisa de acessos de 16/32 bits, entao definimos `inw`/`outw`/`inl`/`outl`
  ao lado das primitivas de 8 bits do `src/include/io.h`).
- **Acesso MMIO** (`hal_map_mmio` + `HalReadMmio*`/`HalWriteMmio*`): a identidade
  de 1 GiB do boot ja cobre `[0, 0x40000000)`, entao `hal_map_mmio(phys, size)`
  devolve o **proprio ponteiro identidade** para faixas dentro de 1 GiB (acesso
  direto — caso da VGA em 0xA0000/0xB8000 e de BARs MMIO baixas). Para faixas
  acima de 1 GiB **nao mapeadas**, devolve 0 e loga o aviso — **nao mexemos nas
  page tables** (o caminho seguro pedido no enunciado). As BARs de MMIO da video
  do QEMU ficam em 0xFD000000/0xFEBF0000 (acima de 1 GiB), entao sao **listadas**
  (vendor/device/base) mas nao desreferenciadas — coerente com a regra de seguranca.
- **Enumeracao PCI (mecanismo de configuracao #1, portas 0xCF8/0xCFC)**:
  - `HalPciReadConfigUlong`/`WriteConfigUlong`/`ReadConfigUshort`/`ReadConfigUchar`
    montam o endereco (bit31 enable | bus<<16 | dev<<11 | func<<8 | offset&0xFC)
    em 0xCF8 e leem/escrevem o dword em 0xCFC.
  - `hal_pci_enumerate()` percorre **bus 0..255, device 0..31, function 0..7**;
    pula slots vazios (vendor==0xFFFF); le **vendor/device** (offset 0x00),
    **class/subclass/prog-if/revision** (offset 0x08) e o **header type** (offset
    0x0E, bit 7 = multifuncao — so entao varre as funcoes 1..7); para header type 0
    le as **6 BARs** (0x10..0x24). Guarda cada dispositivo em `hal_pci_device_t`
    numa tabela interna (ate 64).
  - **Decodificacao de classe** (`pci_class_name`): nomes legiveis para as classes
    que interessam ao projeto (Host bridge, ISA bridge, **Mass Storage / IDE (ATA)**,
    **Display**, rede, USB, etc.).
  - **Log de cada BAR** (regra 4): distingue **I/O** (bit0=1 -> porta) de **MMIO**
    (bit0=0 -> base; sinaliza 64-bit e prefetch).
  - Helpers: `hal_pci_count`/`hal_pci_get` (acesso a tabela) e
    `hal_pci_find_class(class, subclass)` (localiza o **controlador IDE**, a
    **video**, o host bridge — base para o disco/NTFS das proximas fases).
- **`hal_init()`** (chamado em `src/kernel.c`, **logo apos** o heap/PMM ficarem
  operacionais e **antes** do Object Manager): dispara a enumeracao, loga o total e
  **destaca** o controlador de armazenamento (notando IDE/ATA -> portas
  **0x1F0-0x1F7**), a video e o host bridge. Nova linha de boot:
  `[ok] HAL: portas de I/O + MMIO + enumeracao PCI`.

### Arquivos alterados / criados
- **Novos:** `src/hal/hal.h`, `src/hal/hal.c` (pasta `src/hal/` criada — permitido
  pela regra 1).
- **Alterados:** `src/kernel.c` (include `hal/hal.h` + chamada `hal_init()` +
  linha `[ok]`), `IMPLEMENTED.md` (esta secao).
- **Sem mudancas em `build.ps1`/`run.ps1`**: o `build.ps1` varre `src/`
  recursivamente, entao `src/hal/hal.c` entra no build automaticamente (a
  `#include "io.h"` resolve por `-I src/include`, ja nos cflags). Estrutura de
  pastas preservada; 64-bit e todos os testes anteriores intactos.

### Evidencia (run.ps1 -Headless -TimeoutSec 12)
- `qemu.err.log` **= 0 bytes**; **14 linhas `[ok]`** (era 13, +1 da HAL); **0**
  `[EXCECAO]`/`Page Fault`/triple-fault, **0** `syscall desconhecida`, **0**
  `import nao resolvido`; chega em **`Sistema no ar.`** Tudo o que ja funcionava
  continua: ambos os drivers `DriverEntry status=0x0`; `test.exe` MessageBox;
  `ioctlapp.exe` -> **`0xCAFEBABE`** (3x); `conhello.exe`; `test32.exe` (PE32,
  7 relocacoes); named pipe 44 bytes; cmd `sc`/`tasklist`; guiapp/desktop (win32k).
- HAL (logs `[hal]` na serial — **6 dispositivos** no QEMU i440fx):
  ```
  --- HAL (Hardware Abstraction Layer) ---
  [hal] hal_init(): I/O ports + MMIO + enumeracao PCI.
  [hal] enumerando PCI (mecanismo #1, portas 0xCF8/0xCFC)...
  [hal] PCI: 0:0.0  vendor=0x8086 device=0x1237 class=0x06 sub=0x00  (Bridge / Host (CPU->PCI))
  [hal] PCI: 0:1.0  vendor=0x8086 device=0x7000 class=0x06 sub=0x01  (Bridge / ISA)
  [hal] PCI: 0:1.1  vendor=0x8086 device=0x7010 class=0x01 sub=0x01  (Mass Storage / IDE (ATA))
  [hal]   BAR4=I/O  porta=0xC040
  [hal] PCI: 0:1.3  vendor=0x8086 device=0x7113 class=0x06 sub=0x80  (Bridge)
  [hal] PCI: 0:2.0  vendor=0x1234 device=0x1111 class=0x03 sub=0x00  (Controlador de video (Display))
  [hal]   BAR0=MMIO base=0xFD000000 prefetch
  [hal]   BAR2=MMIO base=0xFEBF0000
  [hal] PCI: 0:3.0  vendor=0x8086 device=0x100E class=0x02 sub=0x00  (Controlador de rede)
  [hal]   BAR0=MMIO base=0xFEBC0000
  [hal]   BAR1=I/O  porta=0xC000
  [hal] PCI: 6 dispositivo(s) encontrado(s).
  [hal] controlador de armazenamento: vendor=0x8086 device=0x7010 subclass=0x01 (IDE/ATA - portas 0x1F0-0x1F7)
  [hal] controlador de video: vendor=0x1234 device=0x1111
  [hal] host bridge (CPU->PCI): vendor=0x8086 device=0x1237
  [hal] HAL pronta.
  ```
  O **controlador IDE (PIIX3, 8086:7010)** e a **placa de video (QEMU std VGA,
  1234:1111)** foram achados e logados, exatamente como pede o teste da FASE 1.

### Limitacoes / proximos passos (nao bloqueiam o boot)
- **Stretch nao feito (APIC/HPET):** mantivemos o **PIC 8259 + PIT** atuais; a
  infra de IRQ ainda nao e "registravel" como pedido no opcional. A HAL hoje cobre
  I/O ports + MMIO + PCI; o roteamento de IRQ por APIC fica para depois.
- **MMIO acima de 1 GiB nao e desreferenciado:** as BARs de MMIO da video
  (0xFD000000/0xFEBF0000) estao **fora** da identidade de 1 GiB; `hal_map_mmio`
  as recusa (retorna 0) em vez de mapear novas page tables — caminho seguro. Um
  driver que precise desse LFB linear (resolucao maior) tera antes de estender a
  paginacao para a faixa fisica do BAR.
- **Sem escrita de config / sem alocacao de BAR:** lemos o config space (e ha
  `HalPciWriteConfigUlong`), mas nao reprogramamos BARs nem habilitamos
  bus-mastering ainda; o controlador IDE sera dirigido por **ATA PIO** (portas
  0x1F0-0x1F7) na fase de disco, que nao depende disso.

## FASE 2 (HAL disco) — IDE ATA PIO (LBA28) + imagem de disco NTFS de teste

Status: **completo, build verde, boot verde.** Driver de disco IDE por PIO
(`src/hal/disk.c`) com IDENTIFY + `HalReadSector`/`HalWriteSector` (LBA28), o
`run.ps1` estendido com `-Disk` (anexa a imagem como IDE primario master), um
script que gera a imagem NTFS de teste (`examples/make-ntfs-disk.ps1` +
`examples/make-ntfs-image.py`) e um teste no boot que **le o MBR (setor 0) e o
boot sector da particao NTFS, confirmando a assinatura "NTFS    " no offset 3**.
Nada do que ja funcionava regrediu.

### O que foi implementado
- **Driver de disco IDE (ATA PIO, LBA28)** (`src/hal/disk.c` + `src/hal/disk.h`):
  - **Canal primario master**, portas **0x1F0-0x1F7** (dados/erro/seccount/LBA
    0..2/drive/status-command) + **0x3F6** (alt-status/controle), exatamente o que
    o `hal_init` (FASE 1) achou: IDE/ATA = PIIX3 `8086:7010`.
  - `hal_disk_init()`: faz **IDENTIFY DEVICE (0xEC)**, espera BSY cair, le os 256
    words do bloco, extrai o **modelo** (words 27..46, bytes trocados) e o **total
    de setores LBA28** (words 60..61). Trata "sem disco" (status 0/0xFF) e
    ATAPI/SATA (LBA1/LBA2 != 0) sem travar. Idempotente.
  - `HalReadSector(lba, buf)` (comando **READ SECTORS 0x20**) e
    `HalWriteSector(lba, buf)` (comando **WRITE SECTORS 0x30** + **CACHE FLUSH
    0xE7**): programam drive/LBA/seccount=1, esperam **DRQ** (com checagem do bit
    **ERR**) e transferem 256 words (512 bytes) pela porta de dados (`inw`/`outw`).
    Ambas `__attribute__((ms_abi))` (estilo HAL.DLL do NT) e com **timeout** nas
    esperas de BSY/DRQ (sem loop infinito). **Cada setor lido/escrito e logado na
    serial** com os 8 primeiros bytes (`[disk] READ/WRITE lba=...`), conforme a regra 4.
- **`run.ps1 -Disk` / `-DiskImage`** (extensao): anexa um disco IDE legado ao
  QEMU com `-drive file=...,format=raw,if=ide,index=0,media=disk` (primario
  master = 0x1F0/0x3F6). `-Disk` usa `build\disk.img`; `-DiskImage C:\x.img`
  aponta para outra imagem. **Sem `-Disk`, o comportamento default (so `-kernel`
  + `-initrd`) fica inalterado** — nao arrisca o boot verde.
- **Script da imagem NTFS de teste** (`examples/make-ntfs-disk.ps1`): gera
  `build\disk.img` (default 64 MiB) com **dois modos** escolhidos automaticamente:
  - **(A) REAL (admin + Hyper-V):** `New-VHD -Fixed` -> `Mount-VHD` ->
    `Initialize-Disk -PartitionStyle MBR` -> `New-Partition -MbrType IFS` (tipo de
    particao **0x07**) -> **`Format-Volume -FileSystem NTFS`** -> `Copy/Set-Content`
    dos arquivos -> **`qemu-img convert -f vpc -O raw`**. Produz um **NTFS autentico
    do Windows** (ideal para exercitar o leitor NTFS das proximas fases).
  - **(B) SINTETICO (sem admin):** `examples/make-ntfs-image.py` (Python 3)
    **constroi os bytes na mao** — MBR (1 particao NTFS type 0x07 em LBA 2048),
    **boot sector NTFS** com OEM `NTFS    ` @offset 3 + BPB coerente (bytes/setor
    512, setores/cluster 8, `$MFT` LCN, magic 0xAA55) e uma **`$MFT` minima** (os
    registros de metadados padrao `$MFT`/`$MFTMirr`/.../raiz + **`\hello.txt`** com
    `$DATA` **residente** de conteudo conhecido + **`\dir1\file.txt`**), com os
    **fixups (Update Sequence Array)** por registro. Suficiente para validar o boot
    sector e ja deixar a `$MFT` pronta para a leitura NTFS.
  - Ao final, o script **re-le a imagem** e confirma o MBR (type 0x07) + a
    assinatura `NTFS    ` no boot sector antes de declarar pronto.
- **Teste de disco no boot** (`src/kernel.c`, `disk_test()`): roda **logo apos
  `hal_init`** (e a linha `[ok] HAL disco`), antes de tocar o framebuffer:
  1. `hal_disk_init()` (IDENTIFY) — se nao ha disco (`-kernel` sem `-Disk`), loga
     e segue (boot continua verde).
  2. **Le o setor 0 (MBR)**, confere a magic **0x55AA** e percorre a tabela de
     particoes (4 entradas em 0x1BE), localizando a 1a particao **NTFS (type 0x07)**.
  3. **Le o boot sector (VBR)** dessa particao e **confirma `NTFS    ` no offset 3**;
     loga o BPB (bytes/setor, setores/cluster, total de setores, **`$MFT` LCN**).
  4. **Prova de `HalWriteSector` nao destrutiva:** le um setor de sobra (LBA 100000,
     area NTFS nao alocada), salva, escreve um padrao, **le de volta conferindo** e
     **restaura o original** — comprova a escrita PIO sem alterar a imagem (o hash
     SHA-256 da imagem fica identico antes/depois).
  Tambem trata o caso "superfloppy" (sem MBR; o setor 0 ja e o boot NTFS).

### Arquivos alterados / criados
- **Novos:** `src/hal/disk.h`, `src/hal/disk.c` (driver IDE ATA PIO),
  `examples/make-ntfs-disk.ps1` (gera a imagem; modo real/sintetico),
  `examples/make-ntfs-image.py` (builder NTFS sintetico em Python, sem admin).
- **Alterados:** `src/kernel.c` (include `hal/disk.h` + `disk_test()` + chamada e
  linha `[ok] HAL disco`), `run.ps1` (parametros `-Disk`/`-DiskImage` + `-drive`
  IDE), `IMPLEMENTED.md` (esta secao). O `build.ps1` varre `src/` recursivamente,
  entao `src/hal/disk.c` entra no build sozinho — **sem mudancas no build.ps1**.
- **Estrutura de pastas preservada**; 64-bit e todos os testes anteriores intactos.

### Evidencia (run.ps1 -Headless -Disk -TimeoutSec 14)
- **Sem regressao:** `qemu.err.log` **= 0 bytes**; **15 linhas `[ok]`** (era 14,
  +1 "HAL disco"); **0** `[EXCECAO]`/`Page Fault`/triple-fault, **0** `syscall
  desconhecida`, **0** `import nao resolvido`; **3x `0xCAFEBABE`** (IOCTL intacto);
  drivers `DriverEntry status=0x0`; `test.exe`/`conhello.exe`/`test32.exe` (7
  relocacoes)/named pipe 44 bytes/cmd/win32k todos OK; chega em **`Sistema no ar.`**
- **Disco (logs `[disk]`):**
  ```
  --- HAL disco (IDE ATA PIO, canal primario master 0x1F0) ---
  [disk] IDENTIFY OK. modelo='QEMU HARDDISK'
  [disk] setores LBA28 = 131072 (64 MiB, setor de 512 bytes)
  [disk] READ  lba=0 (512 bytes) primeiros: 0x00 0x00 ...
  [disk] MBR assinatura @510 = 0x55 0xAA  (0x55AA OK)
  [disk] particao 0: type=0x07 LBA=2048 nsec=129024
  [disk] particao NTFS no LBA 2048; lendo o boot sector (VBR)...
  [disk] READ  lba=2048 (512 bytes) primeiros: 0xEB 0x52 0x90 0x4E 0x54 0x46 0x53 0x20  <- "..NTFS "
  [disk] OEM @3 = 'NTFS    '
  [disk] NTFS BPB: bytes/setor=512 setores/cluster=8 total_setores=129024 MFT_LCN=4
  [disk] assinatura 'NTFS    ' confirmada no boot sector -> OK
  [disk] teste de ESCRITA (nao destrutivo) no setor de sobra 100000:
  [disk] WRITE lba=100000 (512 bytes) primeiros: 0xA5 0xA4 0xA7 0xA6 ...
  [disk] write/readback BATE (escrita PIO OK).
  [disk] WRITE lba=100000 ... 0x00 0x00 ...   <- restaura o original
  [disk] setor de sobra restaurado ao conteudo original.
  ```
  O SHA-256 de `build\disk.img` fica **identico antes/depois** do boot (a prova de
  escrita e nao destrutiva).
- **Sem `-Disk` (default):** o teste detecta "sem disco anexado" e segue; boot
  permanece verde (15 `[ok]`, `Sistema no ar.`).

### Como gerar a imagem e testar
```powershell
# 1) Gera build\disk.img (64 MiB). Sem admin -> NTFS sintetico; com admin -> NTFS real.
.\examples\make-ntfs-disk.ps1 -SizeMB 64
# (forcar o modo sem-admin:  .\examples\make-ntfs-disk.ps1 -Synthetic)

# 2) Boota com o disco anexado e confere os logs [disk] (MBR + boot NTFS).
.\run.ps1 -Headless -Disk -TimeoutSec 14
# Apos testar:  Get-Process qemu* | Stop-Process -Force
```

### Limitacoes / proximos passos (nao bloqueiam o boot)
- **Leitura NTFS ainda nao montada:** esta fase entrega o **acesso ao disco** (IDE
  PIO) e a **validacao do boot sector NTFS** + a **imagem de teste**. Montar
  BPB+MFT, percorrer atributos e ler o `$DATA` de `\hello.txt` / listar `\dir1` via
  `$INDEX_ROOT` e a **FASE 3 (NTFS leitura)** — a `$MFT` da imagem sintetica ja foi
  montada pensando nisso (registros + `$DATA` residente + `$INDEX_ROOT`).
- **PIO, 1 setor por vez, sem IRQ/DMA:** simples e suficiente para a imagem de 64
  MiB; um driver de alto desempenho usaria leitura multi-setor / bus-mastering DMA
  (a BAR4 de I/O do PIIX3 ja foi logada na FASE 1) e IRQ14 em vez de polling.
- **So o canal primario master (LBA28):** cobre ate 128 GiB; slave / canal
  secundario / LBA48 ficam para depois (nao necessarios aqui).
- **Imagem sintetica vs. NTFS real:** o builder Python e auto-consistente para o
  boot sector e leitura residente, mas **nao** reproduz todas as estruturas de um
  NTFS real (`$LogFile`, `$Secure`, `$UpCase` reais, `$INDEX_ALLOCATION` para
  diretorios grandes). Para validar o leitor NTFS contra um volume 100% autentico,
  rode `make-ntfs-disk.ps1` **como Administrador** (modo real via Format-Volume).

## FASE 3 (NTFS) — Driver NTFS (LEITURA) + camada de File System

Status: **completo, build verde, boot verde.** Sobre o acesso ao disco da FASE 2,
esta fase monta o volume NTFS (BPB + `$MFT`), faz o parse de registros MFT
(header FILE + fixups USA + atributos residentes/nao-residentes com data runs),
LE o `$DATA` de um arquivo, LISTA diretorios via `$INDEX_ROOT`/`$INDEX_ALLOCATION`,
resolve caminhos (`\dir\arquivo`), e expoe tudo por uma **camada de File System
ligada ao I/O Manager** (`\Device\Harddisk0\Partition1` com `DRIVER_OBJECT.MajorFunction`
atendendo `IRP_MJ_CREATE/READ/DIRECTORY_CONTROL`). Validado contra a imagem de teste:
le `\hello.txt` (== texto conhecido) e lista a raiz. Nada do que ja funcionava regrediu.

### O que foi implementado
- **Parser NTFS (lado kernel)** (`src/drivers/ntfs.c` + `src/drivers/ntfs.h`, novos):
  - **`ntfs_mount(part_lba)`**: le o boot sector pela HAL (`HalReadSector`), valida
    `'NTFS    '` @offset 3 e faz o **parse do BPB**: bytes/setor (`0x0B`),
    setores/cluster (`0x0D`), total de setores (`0x28`), **`$MFT` LCN** (`0x30`),
    `$MFTMirr` LCN (`0x38`), **tamanho do registro MFT** (`0x40`, com a convencao
    do valor com sinal: negativo => `2^|v|` bytes, ex.: `-10` => 1024) e tamanho do
    index record (`0x44`). Le o registro `#0` (`$MFT`) e, se o `$DATA` dele for
    nao-residente, usa o LCN do **1o data run** como autoridade para localizar a MFT.
  - **Leitura de um registro MFT** (`ntfs_read_mft_record`): localiza o registro por
    offset linear no `$DATA` do `$MFT` (`record_no * mft_record_size`), le os setores
    pela HAL, valida a assinatura **`FILE`** e **aplica os fixups (Update Sequence
    Array)** — confere o USN e restaura os 2 ultimos bytes de cada setor.
  - **Iteracao de atributos** (`ntfs_find_attr`): percorre os atributos do registro
    (a partir de `+0x14`, ate `0xFFFFFFFF`), com protecao de limites; acha a n-esima
    instancia de um tipo, opcionalmente filtrando pelo nome `$I30` (indices de dir).
  - **Atributos residentes** (`attr_resident_data`): conteudo em `+content_offset`,
    `content_length` em `+0x10`. **Nao-residentes**: detecta pela flag (`+0x08`),
    le o tamanho real (`+0x30`) e percorre os **data runs** (mapping pairs em `+0x20`):
    cada run = header (nibble baixo=tam. do `length`, nibble alto=tam. do `offset`),
    `length` clusters, `offset` = delta de LCN **com sinal** (run sparse = `offset_size`
    0). `ntfs_run_for_vcn` mapeia um VCN -> LCN; `ntfs_read_nonresident` le por
    cluster (runs sparse viram zeros).
  - **`$STANDARD_INFORMATION` / `$FILE_NAME`** (`ntfs_fill_info`): le o nome
    (UTF-16LE -> ASCII) preferindo o namespace Win32 (1/3) sobre DOS (2)/POSIX, a
    flag de diretorio e o tamanho real do `$DATA`.
  - **`$DATA`** (`ntfs_read_data_attr` / `ntfs_read_file`): le `len` bytes a partir
    de `offset`, tratando residente E nao-residente (data runs) de forma transparente.
  - **Diretorios** (`ntfs_list_dir`): le o **`$INDEX_ROOT`** (residente; node header
    em `+0x10`, entradas a partir de `entries_off`) e, se houver, o
    **`$INDEX_ALLOCATION`** (nao-residente; blocos `INDX` com fixups proprios, node
    header em `+0x18`). Cada **index entry**: ref MFT (`+0x00`, 6 bytes), entry length
    (`+0x08`), key length (`+0x0A`), flags (`+0x0C`); a key e um `$FILE_NAME` (nome,
    dir flag, size). Pula a entrada final (END), o `.` e o namespace DOS duplicado.
  - **Resolucao de caminho** (`ntfs_resolve_path`): comeca na raiz (registro 5, lido
    de verdade via `ntfs_fill_info`) e desce componente a componente (case-insensitive)
    listando cada diretorio ate o alvo. `\`, `\hello.txt`, `\dir1\file.txt`.
- **Camada de File System ligada ao I/O Manager** (`src/drivers/ntfs_fs.c`, novo):
  - **`FsMountVolume(part_lba)`**: chama `ntfs_mount`, cria um **`DRIVER_OBJECT`**
    (FS driver estilo `ntfs.sys`, nomeado `\FileSystem\Ntfs`) e um **`DEVICE_OBJECT`
    de volume** (`io_create_device`, nome **`\Device\Harddisk0\Partition1`**) com uma
    extensao de contexto (registro MFT alvo, offset de leitura, cursor de enumeracao).
  - **Dispatch por IRP** (`DRIVER_OBJECT.MajorFunction`, todos `ms_abi`):
    `IRP_MJ_CREATE` (abre o volume), `IRP_MJ_CLOSE`, **`IRP_MJ_READ`** (le o `$DATA`
    do registro MFT em `Parameters.Read.Key`, offset em `ByteOffset`, preenche o
    `SystemBuffer` METHOD_BUFFERED) e **`IRP_MJ_DIRECTORY_CONTROL`** (devolve UMA
    entrada do diretorio por IRP, estilo `NtQueryDirectoryFile` com ReturnSingleEntry).
  - `IRP_MJ_DIRECTORY_CONTROL` foi adicionado ao `sdk/ntddk.h` (`0x0C`).
- **Conexao com as syscalls (ring 3 -> I/O Manager)** (`src/ke/syscall.c`):
  - **`NtCreateFile`** reconhece o caminho do volume (`\Device\Harddisk0\Partition1`
    com sub-caminho): resolve o caminho NTFS -> registro MFT, cria um `FILE_OBJECT`
    ligado ao device de volume (com `FsContext` = MFT) e dispara `IRP_MJ_CREATE`.
  - **`NtReadFile`** num handle do volume monta um `IRP_MJ_READ` com `Key` = registro
    MFT e `ByteOffset` = posicao corrente (read sequencial; avanca o offset).
  - **`NtQueryDirectoryFile`** (syscall **44**, nova): monta `IRP_MJ_DIRECTORY_CONTROL`
    e devolve uma entrada de diretorio por chamada. `FILE_OBJECT` ganhou `FsContext`/
    `CurrentByteOffset`/`IsDirectory` em `src/nt/io.h`.
  - `dll/ntdll.c`: novo export `NtQueryDirectoryFile` (syscall 44). `dll/kernel32.c`:
    novo export `QueryDirectoryFileEx` (+ struct `MEUOS_DIR_ENTRY`); `CreateFileA`/
    `ReadFile`/`CloseHandle` ja serviam o volume.
- **`cmd.exe` (ring 3): `dir` agora le o volume NTFS de verdade** (`examples/cmd.c`):
  abre `\Device\Harddisk0\Partition1` com `CreateFileA`, **lista a raiz** com
  `QueryDirectoryFileEx` (-> `NtQueryDirectoryFile` -> IRP -> driver NTFS) e dá
  `type \hello.txt` lendo o conteudo com `CreateFileA`+`ReadFile`. Sem volume
  montado, exibe um aviso (nao falha).
- **Teste no boot** (`src/kernel.c`, `ntfs_test()`): apos `disk_test()` achar o boot
  sector NTFS e `ob_init()`, chama `FsMountVolume`, **LISTA a raiz**, **LE `\hello.txt`**
  (mostra o conteudo == texto conhecido) e `\dir1\file.txt`, e exercita a **camada de
  FS via I/O Manager** montando IRPs `CREATE`/`READ`/`DIRECTORY_CONTROL` e chamando
  `IoCallDriver` direto no device de volume. Roda so se houver volume (`-Disk`).
- **`run.ps1 -Disk` robusto**: como `build\disk.img` e um artefato (o `-Clean` apaga
  `build\`), se a imagem default faltar, o `run.ps1` a **gera na hora** com
  `examples/make-ntfs-image.py` (sem admin). `-DiskImage` continua exigindo a imagem.

### Arquivos alterados / criados
- Novos: `src/drivers/ntfs.h`, `src/drivers/ntfs.c`, `src/drivers/ntfs_fs.c`.
- Alterados: `src/kernel.c` (include + `ntfs_test()` + chamada apos `ob_init`),
  `src/ke/syscall.c` (rota do volume em `NtCreateFile`/`NtReadFile` + syscall 44
  `NtQueryDirectoryFile`), `src/nt/io.h` (campos no `FILE_OBJECT`),
  `sdk/ntddk.h` (`IRP_MJ_DIRECTORY_CONTROL`), `dll/ntdll.c` + `dll/kernel32.c`
  (exports `NtQueryDirectoryFile` / `QueryDirectoryFileEx`), `examples/cmd.c`
  (`dir` real), `run.ps1` (auto-gera `disk.img`), `IMPLEMENTED.md` (esta secao).
- **Estrutura de pastas inalterada**: tudo em `src/drivers/`, `src/nt/`, `src/ke/`,
  `dll/`, `examples/` (pastas ja existentes). 64-bit e todos os testes anteriores intactos.

### Evidencia (run.ps1 -Headless -Disk -TimeoutSec 24)
- Tudo o que ja funcionava continua: **16 linhas `[ok]`** (era 15 sem disco; +1 do
  NTFS); ambos os drivers `DriverEntry status=0x0`; `test.exe` (64-bit) MessageBox
  "Pinball"; `ioctlapp.exe` -> IOCTL `0xCAFEBABE` (3x); `conhello.exe`; `test32.exe`
  (PE32 compat mode); pipes; `sysinfo.exe`; `cmd.exe`; `guiapp.exe` (`WM_PAINT`);
  `desktop.exe`. `qemu.err.log` **vazio (0 bytes)**; **0** `[EXCECAO]`, **0** `Page
  Fault`, **0** triple-fault, **0** `syscall desconhecida`, **0** `import nao
  resolvido`; chega em `Sistema no ar.`
- Montagem + leitura NTFS (logs `[ntfs]`/`[ntfs.sys]` na serial):
  ```
  [ntfs] BPB: bytes/setor=512 setores/cluster=8 bytes/cluster=4096
  [ntfs] BPB: total_setores=129024 $MFT_LCN=4 $MFTMirr_LCN=8
  [ntfs] BPB: tam. registro MFT=1024 bytes; tam. index record=4096 bytes
  [ntfs] registro MFT #0 ($MFT) lido: assinatura 'FILE' OK, fixups aplicados.
  [ntfs] volume NTFS MONTADO com sucesso.
  [ntfs.sys] File System Driver registrado: device '\Device\Harddisk0\Partition1' no I/O Manager.
  [ntfs]   [0]        hello.txt   (68 bytes)  -> MFT #24
  [ntfs]   [1] <DIR>  dir1  -> MFT #25
  [ntfs] \hello.txt resolvido: MFT #24, tamanho 68 bytes, ARQUIVO
  [ntfs] $DATA residente, 68 bytes.   bytes lidos do $DATA: 68
  ---- conteudo de \hello.txt ----
  Hello from MeuOS NTFS! Este arquivo foi lido do disco via IDE PIO.
  [ntfs] \dir1\file.txt resolvido: MFT #26, 25 bytes -> "Arquivo dentro de dir1."
  ```
- Camada de FS via **I/O Manager** (IRPs reais no device de volume):
  ```
  [ntfs.sys] IRP_MJ_CREATE: volume aberto.
  [ntfs.sys] IRP_MJ_READ: MFT #24 offset=0 len=127
  [ntfs] IoCallDriver(READ) devolveu 68 bytes: "Hello from MeuOS NTFS! ..."
  [ntfs.sys] IRP_MJ_DIRECTORY_CONTROL: entrada [0] = hello.txt
  [ntfs.sys] IRP_MJ_DIRECTORY_CONTROL: entrada [1] = dir1
  ```
- Ponta a ponta a partir do **ring 3** (`cmd.exe` `dir`):
  ```
  [ntfs.sys] NtCreateFile: '\Device\Harddisk0\Partition1' -> MFT #5 (DIR)
  [ntfs.sys] IRP_MJ_DIRECTORY_CONTROL: entrada [0] = hello.txt   ->        68 bytes  hello.txt
  [ntfs.sys] IRP_MJ_DIRECTORY_CONTROL: entrada [1] = dir1        ->  <DIR>          dir1
  [ntfs.sys] NtCreateFile: '\Device\Harddisk0\Partition1\hello.txt' -> MFT #24 (FILE)
  [ntfs.sys] IRP_MJ_READ: MFT #24 offset=0 len=255
   type \hello.txt:  Hello from MeuOS NTFS! Este arquivo foi lido do disco via IDE PIO.
  ```
- **Sem `-Disk` (default):** nenhuma regressao — 15 `[ok]` (sem a linha do NTFS),
  `dir` do cmd avisa "Nenhum volume NTFS montado", `Sistema no ar.`, `qemu.err.log`
  vazio. O SHA-256 de `build\disk.img` fica identico antes/depois (leitura nao escreve).

### Como testar
```powershell
.\build.ps1
.\run.ps1 -Headless -Disk -TimeoutSec 24   # gera disk.img se faltar; monta NTFS, le \hello.txt
# Apos testar:  Get-Process qemu* | Stop-Process -Force
```

### Limitacoes / proximos passos (nao bloqueiam o boot)
- **Somente LEITURA** (como pede o enunciado): montar BPB+MFT, ler `$DATA`
  (residente e nao-residente via data runs) e listar diretorios (`$INDEX_ROOT`/
  `$INDEX_ALLOCATION`) estao feitos. **ESCRITA** (criar/modificar/excluir, alocacao de
  clusters via `$Bitmap`, atualizar `$INDEX`, journaling pelo `$LogFile`) **nao** foi
  feita — e o proximo passo; o caminho seguro seria comecar por sobrescrever um
  `$DATA` residente de tamanho fixo.
- **`$ATTRIBUTE_LIST` nao tratado:** registros cujos atributos transbordam para
  registros de extensao (arquivos muito fragmentados / nomes longos) nao sao seguidos;
  a imagem de teste e os arquivos pequenos cabem num unico registro.
- **Contexto de volume unico (sem escalonador):** a extensao do device guarda UM
  alvo de leitura / cursor de diretorio por vez; basta para a demo sequencial. Multiplos
  handles concorrentes (com escalonador) precisariam de contexto por-`FILE_OBJECT`
  no proprio IRP (o `FsContext` ja existe no `FILE_OBJECT` para isso).
- **`Parameters.Read.Key` carrega o nº do registro MFT (32 bits):** suficiente para
  qualquer volume realista; uma versao completa passaria o `FILE_OBJECT`/`FsContext`
  pelo `IO_STACK_LOCATION.FileObject`.
- **Imagem sintetica:** validado contra a `$MFT` minima gerada por
  `make-ntfs-image.py` (`\hello.txt` residente, `\dir1\file.txt`, `$INDEX_ROOT`). Os
  caminhos de **nao-residente / data runs / `$INDEX_ALLOCATION`** estao implementados e
  com protecao de limites, mas so seriam *exercitados* contra um NTFS real (arquivo
  grande / diretorio grande) — gere um volume autentico com `make-ntfs-disk.ps1` como
  Administrador para exercitar esses caminhos.

## FASE 4 — Driver NTFS: ESCRITA (subconjunto seguro, sem alocar clusters)

Status: **completo (subconjunto seguro), build verde, boot verde.** Sobre a
leitura da Fase 3, foi adicionada a **ESCRITA NTFS** que NAO precisa alocar
clusters novos nem mexer no `$Bitmap`/`$LogFile` (o caminho de risco). Tudo
reusa o que ja esta alocado e e validado por **round-trip**: escreve, RELE do
disco (recarregando o registro MFT) e confirma na serial. **Nada do que ja
funcionava regrediu** (IOCTL `0xCAFEBABE`, ring3, PE32, named pipes, cmd, win32k,
desktop intactos; 16 `[ok]`, `qemu.err.log` vazio).

### O que foi implementado
- **Reescrita de registro MFT no disco** (`ntfs_write_mft_record` em
  `src/drivers/ntfs.c`): o inverso exato da leitura — valida o header `FILE`,
  **reaplica os fixups na direcao da escrita** (`ntfs_apply_fixups_for_write`:
  incrementa o USN, grava-o no slot do USA e nos 2 ultimos bytes de CADA setor,
  salvando os bytes reais no array) e grava os setores via `HalWriteSector`
  (canal IDE primario, ja existente na HAL). Loga o registro, os setores e o LBA.
- **Sobrescrever / crescer / encurtar `$DATA` RESIDENTE** (`ntfs_write_file`,
  `set_eof`): copia `len` bytes em `offset`; se o novo fim couber no slack do
  registro de 1024 B **e** o `$DATA` for o ULTIMO atributo (END logo apos),
  AJUSTA o tamanho — atualizando o content-length (+0x10) e attr-length (+0x04)
  do `$DATA`, o `bytes used` (+0x18) e o marcador de fim do registro, todas as
  instancias de `$FILE_NAME` (real @0x30 / alloc @0x28) e a entrada de tamanho no
  `$INDEX_ROOT` do diretorio pai (`ntfs_index_update_size`). **validate-before-write:**
  toda a validacao (espaco, "e o ultimo atributo?") acontece ANTES de tocar nos
  bytes; se o resize nao for seguro, faz a **sobrescrita pura** (clampando `len`
  ao tamanho atual), nunca corrompendo o registro.
- **Sobrescrever `$DATA` NAO-RESIDENTE in-place**: grava nos clusters JA alocados
  (read-modify-write por cluster via `ntfs_run_for_vcn` + `HalWriteSector`),
  truncando para nao crescer (sem alocacao de clusters). Implementado com
  protecao de limites; a imagem sintetica so tem `$DATA` residente, mas o caminho
  esta pronto p/ um volume autentico.
- **CRIAR arquivo e diretorio** (`ntfs_create_file`): acha um **registro MFT
  livre** (`ntfs_find_free_record`, varre a partir de #27 — vao seguro do disco
  apos os 27 registros da `$MFT` da imagem, antes do `$MFTMirr`), monta um
  registro do zero (`FILE` header + USA + `$STANDARD_INFORMATION` +
  `$FILE_NAME` + `$DATA` residente, ou `$INDEX_ROOT` vazio p/ diretorio — via os
  builders `build_resident_attr`/`build_filename_content`/`build_stdinfo`/
  `build_empty_index_root`), grava-o, e **insere a entrada no `$INDEX_ROOT` do
  diretorio pai** (`ntfs_index_insert`: cresce o `$INDEX_ROOT` residente dentro
  da folga do registro, desloca a entrada END, atualiza o node header / attr /
  `bytes used`). Em falha de indexacao, reverte marcando o registro novo como
  nao-em-uso (sem orfao).
- **EXCLUIR arquivo e diretorio** (`ntfs_delete_file`): **remove a entrada do
  `$INDEX_ROOT` do pai** (`ntfs_index_remove`: compacta as entradas seguintes
  sobre a removida, encolhe node/attr/`bytes used`) e marca o registro do filho
  como **nao-em-uso** (limpa o bit `0x01` em +0x16). Sem liberar clusters
  (arquivos da imagem sao residentes).
- **Camada de FS / I/O Manager** (`src/drivers/ntfs_fs.c`): novo handler
  **`IRP_MJ_WRITE`** no `DRIVER_OBJECT.MajorFunction` do `\Device\Harddisk0\Partition1`
  (todos `ms_abi`): le o registro MFT alvo de `Parameters.Write.Key`, o offset de
  `ByteOffset` e os bytes do `SystemBuffer` (METHOD_BUFFERED) e chama
  `ntfs_write_file`. `Information` = bytes escritos.
- **Syscall layer** (`src/ke/syscall.c`): `sys_writefile` (NtWriteFile) agora,
  para um handle do volume NTFS, passa `Key`=registro MFT (`FsContext`) e o
  `ByteOffset` corrente ao montar o `IRP_MJ_WRITE` e avanca o cursor (escrita
  sequencial) — espelhando o que ja era feito na leitura.

### Arquivos alterados / criados
- Alterados: `src/drivers/ntfs.c` (+`ntfs_write_mft_record`,
  `ntfs_apply_fixups_for_write`, `ntfs_write_file`, `ntfs_index_update_size`,
  `ntfs_record_set_filename_size`, `ntfs_create_file`/`ntfs_delete_file` +
  `ntfs_index_insert`/`ntfs_index_remove`/`ntfs_find_free_record` + builders),
  `src/drivers/ntfs.h` (API de escrita/criar/excluir), `src/drivers/ntfs_fs.c`
  (`IRP_MJ_WRITE`), `src/ke/syscall.c` (`sys_writefile` no volume NTFS),
  `src/kernel.c` (`ntfs_test`: testes de escrita/grow/criar/excluir + IRP_MJ_WRITE),
  `IMPLEMENTED.md` (esta secao). **Estrutura de pastas inalterada.**

### Evidencia (run.ps1 -Headless -Disk -TimeoutSec 18; tudo no MESMO boot)
- **build.ps1 -Clean: VERDE** (`ntfs.c`/`ntfs_fs.c` 0 warnings; link + objcopy OK).
- **16 `[ok]`**, **0** `[EXCECAO]`/`Page Fault`/triple-fault/`syscall desconhecida`/
  `import nao resolvido`; **`qemu.err.log` = 0 bytes**; `Sistema no ar.` x1.
- **SHA-256 de `build\disk.img` MUDA** apos o boot (a escrita persistiu no meio):
  `D2DCFC17...` (recem-gerado) -> `24742E2C...` (apos o boot). Sem `-Disk`: o disk
  nao e tocado; 15 `[ok]`, 0 operacoes de escrita, sem regressao.
- **(1) Sobrescrita (mesmo tamanho)** de `\hello.txt` (MFT #24): apos gravar 32
  bytes + reler do disco -> `"OVERWRITTEN by MeuOS write path!ivo foi lido do
  disco via IDE PIO."` -> `(1) OK: sobrescrita PERSISTIU no disco`.
- **(2) Crescer (resident grow) 68 -> 191 bytes**: grava `$DATA` + `bytes used` +
  `$FILE_NAME` + entrada do `$INDEX_ROOT` da raiz (#5); RELE do disco -> os 191
  bytes novos; a **listagem da raiz reporta `\hello.txt` agora com 191 bytes (era
  68) -> OK** -> `(2) OK: arquivo CRESCEU e o novo conteudo persistiu`. (O `cmd.exe`
  em ring 3 faz `type \hello.txt` e mostra exatamente o conteudo de 191 bytes —
  prova end-to-end pelo I/O Manager + ring 3.)
- **Criar `\novo.txt`** (registro livre #27 + entrada no `$INDEX` da raiz): RELE
  do disco -> `"Arquivo CRIADO em runtime pelo MeuOS (...)"` (87 bytes) -> `OK:
  arquivo CRIADO e o conteudo persistiu`; a raiz passa a listar **3 entradas**
  (`hello.txt`, `dir1`, `novo.txt`).
- **Excluir `\novo.txt`**: removido do `$INDEX` da raiz + registro #27 marcado
  nao-em-uso; a raiz volta a **2 entradas** e `\novo.txt` **nao resolve mais** ->
  `OK: \novo.txt foi EXCLUIDO`.
- **IRP_MJ_WRITE pelo I/O Manager** em `\dir1\file.txt` (MFT #26): `IoCallDriver(WRITE)`
  -> `[ntfs.sys] IRP_MJ_WRITE` -> 8 bytes; `IRP_MJ_READ` RELE -> `"via IRP!dentro
  de dir1."` (8 bytes sobrescritos + resto intacto).

### Como testar
```powershell
.\build.ps1
# gera disk.img se faltar; monta NTFS, le \hello.txt, ESCREVE/CRESCE/CRIA/EXCLUI:
.\run.ps1 -Headless -Disk -TimeoutSec 18
# Apos testar:  Get-Process qemu* | Stop-Process -Force
# O SHA-256 de build\disk.img muda apos o boot (a escrita persiste no meio).
```

### Limitacoes / proximos passos (relatados como blockers; NAO quebram o boot)
- **Sem alocacao de clusters (`$Bitmap` do volume):** so escrevemos onde ja ha
  espaco — sobrescrita/grow de `$DATA` **residente** (dentro do registro MFT) e
  overwrite de `$DATA` **nao-residente** nos clusters existentes. Crescer um
  arquivo alem do registro (converter residente -> nao-residente) ou criar um
  arquivo com `$DATA` grande exigiria alocar clusters via o `$Bitmap` do volume —
  **nao implementado** (seria o caminho arriscado p/ o boot verde). O `ntfs_write_file`
  detecta e **recusa com seguranca** (loga e faz sobrescrita pura) nesse caso.
- **`$MFT:$BITMAP` e tamanho logico do `$DATA` do `$MFT` nao atualizados:** ao
  criar, escolhemos um registro livre por *scan* (FILE sem in-use ou zeros) e
  gravamos; o NOSSO leitor (offset linear + valida `FILE` + bit in-use + entrada
  no `$INDEX`) ve o arquivo, mas o **Windows real** exigiria marcar o registro no
  `$MFT:$BITMAP` e estender o `$DATA` do `$MFT`. Honesto: a criacao e completa p/
  o MeuOS, parcial p/ compatibilidade com chkdsk do Windows.
- **`$INDEX_ALLOCATION` (diretorio grande) na escrita:** inserir/remover so mexe
  no `$INDEX_ROOT` residente (que e o ULTIMO atributo nos diretorios da imagem).
  Diretorios grandes que transbordam p/ blocos `INDX` nao-residentes exigiriam
  alocar/atualizar `$INDEX_ALLOCATION` + `$BITMAP` do indice — fora do escopo
  seguro. A leitura de `INDX` (Fase 3) ja existe; a escrita de `INDX` nao.
- **Journaling (`$LogFile`) nao implementado** (era STRETCH): nao ha transacao/
  recuperacao — uma queda de energia no meio de uma escrita multi-setor poderia
  deixar o volume inconsistente (sem o journal do NTFS real). Para a demo
  sequencial headless e seguro; e o passo seguinte natural.
- **Para EXERCITAR os caminhos nao-residente/INDX na ESCRITA** e preciso um volume
  NTFS 100% autentico (arquivo grande / diretorio grande): gere com
  `examples\make-ntfs-disk.ps1` num PowerShell **Administrador** (`Format-Volume
  NTFS` exige elevacao; o ambiente atual nao esta elevado).

## FASE 5 — Syscalls de FS + comandos de arquivo no cmd.exe (volume montado como C:)

Status: **completo, build verde, boot verde.** Sobre a leitura+escrita NTFS das
Fases 3/4, esta fase liga o volume NTFS ao **ring 3 como a unidade C:** e adiciona
os **comandos de arquivo do cmd.exe** (`dir`/`cd`/`type`/`copy`/`del`/`vol`), alem
da syscall `NtQueryVolumeInformation`. Nada do que ja funcionava regrediu.

### O que foi implementado
- **Montagem automatica como C: (mapeamento de caminho no syscall layer)**
  (`src/ke/syscall.c`, `ntfs_volume_subpath`): o tradutor de caminho do volume
  agora reconhece **ambas** as formas e devolve o sub-caminho NTFS normalizado
  (`\...`, barras `/`->`\`):
  - **forma DOS `C:` (nova):** `C:\hello.txt`, `C:hello.txt`, `C:` (raiz), `C:\`;
  - forma NT crua `\Device\Harddisk0\Partition1[\...]` (ja existente).
  Com isso `NtCreateFile`/`NtReadFile`/`NtWriteFile`/`NtQueryDirectoryFile` no
  volume aceitam `C:\caminho` transparente (o kernel mapeia `C:\` ->
  `\Device\Harddisk0\Partition1`). O boot loga `volume disponivel como C:`.
- **NtQueryVolumeInformation (syscall 45, nova)** — sincronizada em `dll/ntdll.c`:
  - kernel (`src/ke/syscall.c`, `sys_queryvolumeinformation`): preenche um
    `MEUOS_VOLUME_INFO` (serial, total/livre em bytes, bytes/setor, bytes/cluster,
    fs name "NTFS", rotulo) do volume montado. RAX = STATUS_SUCCESS / 0xC0000004
    (buffer pequeno) / 0xC0000034 (sem volume). Loga rotulo/fs/total.
  - parser (`src/drivers/ntfs.c`, `ntfs_volume_info` + struct `NTFS_VOLUME_INFO`
    em `ntfs.h`): captura o **serial** do BPB (@0x48) na montagem; le o rotulo do
    registro `$Volume` (#3) pelo atributo `$VOLUME_NAME` (0x60, UTF-16->ASCII),
    com default "MEUOS"; tamanho TOTAL exato (`total_setores * bytes/setor`); o
    **free e ESTIMADO** (~7/8 do total — nao varremos o `$Bitmap`, honesto).
- **DLLs:** `dll/ntdll.c` exporta `NtQueryVolumeInformation` (sc3, syscall 45);
  `dll/kernel32.c` exporta `QueryVolumeInfoEx(MEUOS_VOLUME_INFO*)` (+ a struct,
  mesmo layout do kernel) para o `vol` do cmd.
- **cmd.exe (`examples/cmd.c`) — comandos de arquivo sobre o C:** (ring 3):
  - **CWD + resolucao de caminho:** um diretorio atual `g_cwd` (subpath NTFS,
    raiz = `\`); `resolve_subpath()` aplica o CWD e trata `.`/`..`/absoluto/
    relativo/`C:` e normaliza barras; `make_full_path()` prefixa `C:` para o
    `CreateFileA`. O **prompt segue o CWD** (`C:\>`, `C:\dir1>`).
  - **`dir [caminho]`:** abre o diretorio (CWD ou o argumento) com `CreateFileA`
    e lista via `QueryDirectoryFileEx` (-> `NtQueryDirectoryFile` -> IRP ->
    driver NTFS), com tamanho por arquivo e sumario (N arquivos / N pastas).
  - **`cd <dir>`** (`cd \`, `cd ..`, `cd dir1`): resolve, abre como diretorio e
    valida (lista a 1a entrada ou aceita a raiz) antes de fixar o `g_cwd`.
  - **`type <arquivo>`:** abre `C:\...` e imprime o conteudo em blocos via
    `ReadFile` (-> `NtReadFile` -> IRP_MJ_READ -> `$DATA`).
  - **`copy <orig> <dst>`:** le a origem e **escreve no destino EXISTENTE** via
    `WriteFile` (-> `NtWriteFile` -> IRP_MJ_WRITE -> `ntfs_write_file`,
    sobrescrita residente da Fase 4) — exercita a ESCRITA NTFS ponta a ponta do
    ring 3. Se o destino nao existe, **avisa** (criar arquivos novos pelo ring 3
    ainda nao tem syscall) sem falhar.
  - **`del <arquivo>`:** **stub honesto** — o kernel tem `ntfs_delete_file`
    (Fase 4), mas falta expor `NtDeleteFile`; o comando explica isso e nao quebra.
  - **`vol`:** mostra rotulo/serial/fs/tamanho/livre do C: via `QueryVolumeInfoEx`.
  - mantidos `help`/`tasklist`/`sc query`/`sc start`/`sc stop`; o **modo DEMO**
    (headless) agora executa automaticamente `vol`, `dir`, `type hello.txt`,
    `cd dir1` + `dir` + `type file.txt`, `copy file.txt file.txt` e `cd \`.

### Arquivos alterados / criados
- Alterados: `src/ke/syscall.c` (mapeamento C: + syscall 45 + tabela SSDT),
  `src/drivers/ntfs.c` (`ntfs_volume_info` + serial no mount), `src/drivers/ntfs.h`
  (`NTFS_VOLUME_INFO` + serial no `NTFS_VOLUME` + prototipo),
  `dll/ntdll.c` (export `NtQueryVolumeInformation`), `dll/kernel32.c`
  (import + `QueryVolumeInfoEx` + struct), `examples/cmd.c` (CWD + dir/cd/type/
  copy/del/vol + demo), `src/kernel.c` (log "montado como C:"), `IMPLEMENTED.md`.
- **Estrutura de pastas inalterada.** 64-bit e todos os testes anteriores intactos.

### Evidencia (run.ps1 -Headless -Disk -TimeoutSec 24)
- build.ps1: VERDE (0 erros/warnings; link + objcopy OK). Boot VERDE com disco:
  **16 linhas [ok]** (inclui "NTFS: volume montado como C: + \hello.txt lido");
  **0** [EXCECAO]/Page Fault/triple-fault/`syscall desconhecida`/`import nao
  resolvido`; **3x** 0xCAFEBABE (IOCTL intacto); 14x DriverEntry status=0x0;
  "Sistema no ar" x1; `qemu.err.log` = 0 bytes. Apps existentes intactos no mesmo
  boot: test.exe (Pinball MessageBox), conhello, test32 (PE32 compat), pipes
  (server->client, 26 linhas), sysinfo, guiapp (WM_PAINT x63), desktop, win32k.
- **PROVA DE ESCRITA PERSISTENTE:** SHA-256 do build\disk.img muda apos o boot
  (D2DCFC17... -> 3196709D...) = a escrita do `copy` (+ a Fase 4) foi ao disco.
- **C: ponta a ponta no cmd.exe (ring 3 -> kernel32 -> ntdll -> int 0x80 ->
  driver NTFS):**
  - `vol`: `[ntfs.sys] NtQueryVolumeInformation: rotulo='MEUOS' fs=NTFS
    total=66060288 bytes` -> "Volume na unidade C: e MEUOS / NTFS / serial
    0x1234567890ABCDEF / 63 MiB / bytes por setor 512 | cluster 4096".
  - `dir` na raiz: `NtCreateFile 'C:\' -> MFT #5 (DIR)`, lista `hello.txt` (191
    bytes) + `<DIR> dir1`, "1 arquivo(s) 191 bytes / 1 pasta(s)".
  - `type hello.txt`: `NtCreateFile 'C:\hello.txt' -> MFT #24 (FILE)`,
    `IRP_MJ_READ MFT #24` -> mostra o conteudo (`$DATA` residente, 191 bytes).
  - `cd dir1`: `NtCreateFile 'C:\dir1' -> MFT #25 (DIR)`; prompt vira `C:\dir1>`;
    `dir` lista `file.txt` (25 bytes); `type file.txt` -> `MFT #26` mostra o
    conteudo. `cd \` volta para `C:\>`.
  - `copy file.txt file.txt`: le 25 bytes e `IRP_MJ_WRITE MFT #26` ->
    `[ntfs] write: $DATA residente do registro #26 gravado (25 bytes)` ->
    "1 arquivo(s) copiado(s) (25 bytes: C:\dir1\file.txt -> C:\dir1\file.txt)".
  - **0** ocorrencias de "nao e reconhecido como um comando interno".
- **Sem regressao sem -Disk (default):** **15 [ok]** (sem a linha do NTFS),
  `vol`/`dir`/`type`/`cd`/`copy` degradam com aviso gracioso ("Nenhum volume NTFS
  montado" / "nao pode encontrar o arquivo") — sem hang/fault; `qemu.err` 0 bytes;
  "Sistema no ar".

### Limitacoes / proximos passos (nao bloqueiam o boot)
- **Espaco livre do `vol` e ESTIMATIVA** (~7/8 do total): nao varremos o `$Bitmap`
  do volume (caminho de alocacao). O tamanho TOTAL, serial, fs name, bytes/setor e
  bytes/cluster sao exatos (do BPB).
- **`copy` so sobrescreve um destino EXISTENTE** e **`del` e stub:** criar/excluir
  arquivo pelo ring 3 exigiria expor `NtCreateFile(disposition=CREATE)` e
  `NtDeleteFile` ligados ao `ntfs_create_file`/`ntfs_delete_file` (que JA existem
  no kernel, Fase 4). O caminho de criacao/alocacao e o proximo passo natural.
- **Contexto de volume unico (sem escalonador):** a extensao do device guarda um
  alvo/cursor por vez; cada `CreateFileA` reabre (IRP_MJ_CREATE) e **reinicia o
  cursor de enumeracao**, entao `cd`+`dir` do mesmo diretorio em handles separados
  listam do inicio corretamente. O `FsContext` ja existe no `FILE_OBJECT` para
  evoluir p/ contexto por-handle quando houver concorrencia.
- **`cd` valida diretorio listando a 1a entrada:** diretorios vazios (que a imagem
  de teste nao tem) seriam tratados como nao-diretorio; com `$INDEX` real isso se
  resolve checando o flag de diretorio do registro.
