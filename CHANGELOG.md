# Changelog

Todas as mudanças relevantes do projeto. Formato baseado em
[Keep a Changelog](https://keepachangelog.com/pt-BR/1.0.0/) e
[Versionamento Semântico](https://semver.org/lang/pt-BR/).

## [Não lançado]
### Planejado
- Modo gráfico (framebuffer) + GDI/DirectDraw.
- Crescer a superfície Win32 / ntoskrnl (mais APIs conforme os apps pedirem).
- Modo usuário (ring 3) + paginação por processo + syscalls (isolamento).
- Sistema de arquivos + driver de disco (carregar binários sem `-initrd`).

## [0.1.0] - 2026-06-22
Primeira versão pública. Kernel 64-bit, do zero, que carrega e roda binários
do Windows (PE32+) com arquitetura no estilo NT.

### Adicionado
- **Boot**: Multiboot em 32 bits → long mode 64 bits (AOUT kludge + binário plano).
- **CPU/memória baixa**: paginação PAE, GDT de 64 bits, SSE.
- **Interrupções**: IDT (256 vetores) + exceções; PIC 8259 remapeado; PIT (timer
  IRQ0); teclado PS/2 por IRQ1.
- **Memória**: detecção de RAM (Multiboot), PMM (frames de 4 KiB), heap
  (`kmalloc`/`kfree` com split + coalescing).
- **Carregador PE32+**: parse de DOS/PE/seções e resolução de imports por nome.
- **Subsistema Win32**: `MessageBoxA`, `ExitProcess` — roda um `.exe` do Windows.
- **Executiva NT** (`ntoskrnl`): `DbgPrint`, `IoCreateDevice`, `RtlInitUnicodeString`.
- **I/O Manager**: carrega drivers de kernel `.sys` (`DriverEntry`, `DRIVER_OBJECT`
  com layout x64 idêntico ao do NT, `DriverUnload`).
- **Carregamento por módulos de boot**: o kernel roda **qualquer** PE passado via
  `-initrd`, detectando `.exe` vs `.sys` pelo Subsystem. Nada embutido no kernel.
- **Toolchain portátil no Windows**: Zig (`zig cc`) + NASM + QEMU, baixados pelo
  `setup.ps1` (sem precisar de SDK).

### Corrigido
- `cpuid` destruía `EBX` (ponteiro do Multiboot) — `EBX` passou a ser salvo no
  primeiro instante do boot, antes de qualquer `cpuid`.

[Não lançado]: https://github.com/JoaoOliveiraJ/Meu-OS/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/JoaoOliveiraJ/Meu-OS/releases/tag/v0.1.0
