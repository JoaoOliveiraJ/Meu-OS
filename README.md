# MeuOS — sistema operacional 64-bit, escrito do zero em C

Um kernel de verdade, **bootável**, escrito do zero em C + Assembly. Liga em
modo **32 bits** (via Multiboot), monta paginação, transiciona para **long mode
(64 bits)**, instala interrupções e roda o kernel em C com drivers de vídeo,
serial e teclado. Testável no **QEMU**, que é baixado automaticamente.

## Status (verificado rodando no QEMU)
- [x] Boot Multiboot em modo protegido **32 bits**
- [x] Paginação **PAE** + transição para **LONG MODE (64 bits)** + GDT + SSE
- [x] Kernel em **C 64-bit** (`kmain`)
- [x] Driver de **vídeo VGA** texto + **serial COM1**
- [x] **IDT** (256 vetores) + tratamento de **exceções** (não dá triple-fault)
- [x] **PIC 8259** remapeado + **PIT** (timer IRQ0, 100 Hz)
- [x] **Teclado PS/2 por interrupção** (IRQ1), com eco
- [x] **Memória**: detecção de RAM (Multiboot) + **PMM** (frames) + **heap** (`kmalloc`/`kfree`)
- [x] **Carregador PE32+** + **Win32** própria: **um `.exe` do Windows roda** (resolve imports, chama entry, `MessageBoxA`/`ExitProcess`)
- [x] **Arquitetura NT**: executiva (`ntoskrnl`) + **I/O Manager** que **carrega drivers de kernel `.sys`** (`DriverEntry`, `DRIVER_OBJECT`, `IoCreateDevice`, `DriverUnload`)

## Requisitos
- Windows 10/11 **x64**.
- **NASM** (`winget install -e --id NASM.NASM` se faltar).
- **Zig** e **QEMU** são baixados pelo `setup.ps1` (Zig sem admin; QEMU via winget).

## Início rápido
```powershell
.\setup.ps1                  # baixa Zig + QEMU (uma vez)
.\build.ps1                  # compila -> build\kernel.bin
.\run.ps1                    # roda no QEMU (janela; digite e veja o eco)
.\run.ps1 -Headless -TimeoutSec 8   # teste rápido por serial, encerra sozinho
```

## Estrutura
```
src/
  arch/
    boot.asm ........ Multiboot + 32 bits -> long mode 64 bits (AOUT kludge)
    isr.asm ......... stubs de interrupcao (exceptions 0..31, IRQs 32..47)
  cpu/
    idt.c/.h ........ Interrupt Descriptor Table (IDT)
    isr.c/.h ........ despacho de excecoes e IRQs (isr_handler, struct regs)
    pic.c/.h ........ PIC 8259 (remap + EOI)
    pit.c/.h ........ timer PIT (IRQ0)
  drivers/
    vga.c/.h ........ video texto VGA (0xB8000)
    serial.c/.h ..... serial COM1
    keyboard.c/.h ... teclado PS/2 por IRQ1 (scancode set 1 -> ASCII)
  mm/
    pmm.c/.h ........ alocador de memoria fisica (frames de 4 KiB)
    heap.c/.h ....... heap do kernel (kmalloc/kfree)
  loader/
    pe.c/.h ......... carregador PE32+ (parse, secoes, imports) — exe e .sys
  win32/
    win32.c/.h ...... subsistema Win32 (MessageBoxA, ExitProcess, ...)
  nt/
    ntexec.c/.h ..... executiva "ntoskrnl" (DbgPrint, IoCreateDevice, Rtl...)
    driver.c/.h ..... I/O Manager: carrega .sys, monta DRIVER_OBJECT, DriverEntry
  include/
    io.h ............ inb/outb (uso interno do kernel)
  kernel.c .......... kmain() + kputs/kput_hex/kput_dec/mem*

sdk/   <- headers que o OS EXPOE para binarios Windows (nao e o OS em si)
  ntddk.h ........... tipos do DDK (UNICODE_STRING, DRIVER_OBJECT, ...), layout x64

examples/   <- programas Windows de teste (NAO fazem parte do OS)
  hello.c ........... um .EXE Windows (compilado p/ PE32+, alvo Windows)
  mydriver.c ........ um driver de kernel .sys (subsystem NATIVE)

linker.ld ........... layout em 1 MiB + simbolos de carga p/ Multiboot
setup.ps1 / build.ps1 / run.ps1
```
`build.ps1` varre `src\` recursivamente (isso é o OS). `examples\` é compilado
**à parte**, com o **alvo Windows**, virando binários PE soltos em `build\` —
o kernel os carrega no boot, não os embute. `sdk\ntddk.h` é o nosso "DDK": os
tipos que o OS oferece a quem escreve drivers (igual o Windows fornece o WDK).

## Arquitetura (no estilo Windows NT)
- **Subsistema Win32** (`win32/`) — resolve os imports de um `.exe` (user32,
  kernel32, ...) para implementações nossas. É o papel de kernel32/user32.
- **Executiva NT** (`nt/ntexec.c`) — o "ntoskrnl.exe": as APIs que os **drivers
  de kernel** importam (`DbgPrint`, `IoCreateDevice`, `RtlInitUnicodeString`...).
- **I/O Manager** (`nt/driver.c`) — carrega um `.sys`, monta o `DRIVER_OBJECT`
  (layout idêntico ao do NT) e chama `DriverEntry(DriverObject, RegistryPath)`,
  depois `DriverUnload`.
- **Carregador PE** (`loader/pe.c`) — comum aos dois: faz parsing do PE32+,
  mapeia seções e resolve imports via um *resolver* (Win32 ou ntoskrnl).
- Tudo que é binário Windows é chamado em **ABI Microsoft** (`ms_abi`); o kernel
  é compilado em System V.
- **Ainda não** é NT completo: falta ring 3/`ntdll`+syscalls, Object Manager
  cheio, IRPs/PnP. A espinha (executiva + I/O Manager + modelo de driver) já está.

## Como o boot funciona
1. QEMU `-kernel` lê o cabeçalho **Multiboot** (nos primeiros 8 KiB). Como ele
   recusa ELF64, usamos o **AOUT kludge** + um **binário plano** (`kernel.bin`,
   via `zig objcopy`) com os endereços de carga no cabeçalho.
2. `boot.asm` (32 bits): valida CPUID/long-mode, monta PML4/PDPT/PD (identidade
   de 1 GiB), liga PAE + EFER.LME + paginação, e faz *far-jump* para 64 bits.
3. `boot.asm` (64 bits): habilita SSE e chama `kmain()`.
4. `kmain()`: VGA + serial + **IDT** + **PIC/PIT** + teclado por IRQ; `sti`.

## Sobre "rodar o Pinball" (importante)
O **Pinball original da Microsoft** (`PINBALL.EXE`, 3D Space Cadet) é um programa
**Windows (Win32)**: ele depende da API inteira do Windows (Win32/GDI/DirectX).
Rodar esse `.exe` aqui significaria **reimplementar o Windows** — é o que o Wine
e o ReactOS fazem, anos de trabalho. **Não dá** num kernel próprio.

Os caminhos reais, do zero, para ter Pinball **no MeuOS**:
- **(A) Pinball nativo nosso** — escrever um jogo de pinball direto sobre o
  kernel (framebuffer + física + flippers + bola, controle por teclado). É o
  caminho recomendado: vira jogável rápido e é 100% "do zero".
- **(B) Portar o Space Cadet open-source** (reimplementação `SpaceCadetPinball`,
  C++/SDL2) — dá o jogo "de verdade", mas exige antes construir uma base enorme
  no OS: libc/libc++, uma camada tipo SDL (vídeo/áudio/input), heap, sistema de
  arquivos para os dados do jogo. Meses de desenvolvimento.

**Próximo passo técnico (serve para os dois):** sair do modo texto e ter
**modo gráfico** (framebuffer). O plano é VGA *mode 13h* (320x200, 256 cores)
programando os registradores direto, com desenho em `0xA0000`.

## Roadmap (meta: rodar um `.exe` do Windows, rumo ao PINBALL.EXE)
1. [x] IDT + exceções
2. [x] PIC + PIT + teclado por IRQ
3. [x] Memória: PMM (frames) + heap (`kmalloc`/`kfree`)
4. [x] **Carregador PE32+** (parse, seções, tabela de imports)
5. [x] **Win32 própria** (resolve imports por nome; ABI `ms_abi`)
6. [x] **Rodar um `.exe` Windows nosso** (`MessageBoxA`/`ExitProcess`)
7. [x] **Arquitetura NT**: executiva `ntoskrnl` + I/O Manager + **carrega driver `.sys`**
8. [ ] **Crescer Win32/ntoskrnl**: `GetStdHandle`/`WriteFile`/`HeapAlloc`, IRPs, mais Io*/Ke*
9. [ ] **Modo gráfico** (framebuffer) + GDI/**DirectDraw** (render do Pinball)
10. [ ] Áudio (**DirectSound**) + input + **registry** (placar/opções)
11. [ ] **Modo usuário (ring 3)** + paginação por processo + syscalls (isolamento)
12. [ ] **PINBALL.EXE** rodando

> Hoje o `.exe` roda em ring 0 (no espaço do kernel). O isolamento em ring 3
> (passo 10) vem depois; primeiro ampliamos a superfície Win32 e o gráfico,
> que é o que o Pinball realmente exige.

## Como um binário Windows é carregado (qualquer um, não só os exemplos)
1. O binário (`.exe` ou `.sys`) é passado ao QEMU como **módulo de boot**
   (`-initrd`) — o `run.ps1` faz isso. **Nada é embutido no kernel.**
2. `kmain()` lê os módulos do Multiboot e, para cada um, olha o **Subsystem**
   do PE: NATIVE → driver (`nt/driver.c`); senão → app (`pe_run`, via `win32/`).
3. `loader/pe.c` faz o parsing (DOS/PE/seções), mapeia no ImageBase, percorre a
   **import table** e resolve cada import **pelo nome** (Win32 ou ntoskrnl),
   depois chama o entry em **ABI Microsoft** (`__attribute__((ms_abi))`).

Para rodar o SEU programa:
```powershell
.\run.ps1 -Modules C:\caminho\app.exe
```
O loader lista cada import; os que aparecem como **"NAO IMPLEMENTADO"** são as
APIs que faltam implementar em `win32/` ou `nt/` para aquele programa rodar.
Ou seja: o OS **não** é feito sob medida para os exemplos — ele roda qualquer
PE; o que cresce é a quantidade de APIs do Windows que já existem.

## Dicas de QEMU
- Serial no terminal: `-serial stdio`; sem janela: `-display none`.
- Debug com GDB: `-s -S` e `target remote :1234` usando `kernel.elf` (símbolos).
