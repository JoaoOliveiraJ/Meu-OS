; ============================================================================
;  boot.asm  —  Ponto de entrada do kernel.
;
;  Fluxo:  Multiboot (32 bits) -> habilita paginacao/PAE -> entra em LONG MODE
;          (64 bits) -> chama kmain() em C.
;
;  Montado com:  nasm -f elf64 boot.asm -o boot.o
; ============================================================================

global _start
extern kmain

; simbolos definidos pelo linker (enderecos de carga)
extern __load_start
extern __load_end
extern __bss_end

; ---- Cabecalho Multiboot 1 ----
; Usamos o "AOUT kludge" (bit 16): o QEMU "-kernel" so aceita ELF de 32 bits,
; entao entregamos um BINARIO PLANO e dizemos a ele os enderecos de carga aqui.
MB_ALIGN   equ 1 << 0                 ; alinhar modulos em 4 KiB
MB_MEMINFO equ 1 << 1                 ; pedir mapa de memoria
MB_KLUDGE  equ 1 << 16                ; usar os campos de endereco abaixo
MBFLAGS    equ MB_ALIGN | MB_MEMINFO | MB_KLUDGE
MAGIC      equ 0x1BADB002
CHECKSUM   equ -(MAGIC + MBFLAGS)

section .multiboot
align 4
mb_header:
    dd MAGIC
    dd MBFLAGS
    dd CHECKSUM
    dd __load_start                  ; header_addr
    dd __load_start                  ; load_addr
    dd __load_end                    ; load_end_addr
    dd __bss_end                     ; bss_end_addr
    dd _start                        ; entry_addr

; ---- Memoria reservada: pilha e tabelas de paginas ----
section .bss
alignb 16
stack_bottom:
    resb 16384                       ; 16 KiB de pilha
stack_top:

mb_magic:    resd 1                  ; EAX salvo no boot (magico Multiboot)
mb_info_ptr: resd 1                  ; EBX salvo no boot (ponteiro do Multiboot info)

alignb 4096
p4_table:    resb 4096               ; PML4
p3_table:    resb 4096               ; PDPT
p2_table:    resb 4096               ; Page Directory (paginas de 2 MiB)

; ---- GDT de 64 bits ----
section .rodata
gdt64:
    dq 0                                                     ; descritor nulo
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)                 ; codigo: exec, present, long mode
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

; ============================================================================
section .text
bits 32
_start:
    mov esp, stack_top                ; pilha provisoria
    mov [mb_magic], eax               ; salva ANTES de qualquer cpuid (que destroi EBX)
    mov [mb_info_ptr], ebx

    call check_multiboot
    call check_cpuid
    call check_long_mode

    call setup_page_tables
    call enable_paging

    lgdt [gdt64.pointer]              ; carrega GDT de 64 bits
    jmp gdt64.code:long_mode_start    ; far-jump -> entra em 64 bits

; --------------------------------------------------------------------------
; Verificacoes (em caso de falha, imprime "ERR:x" e trava)
; --------------------------------------------------------------------------
check_multiboot:
    cmp eax, 0x2BADB002              ; magico que o bootloader deixa em EAX
    jne .fail
    ret
.fail:
    mov al, '0'
    jmp error

check_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21                 ; tenta inverter o bit ID (21) do EFLAGS
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .fail
    ret
.fail:
    mov al, '1'
    jmp error

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .fail
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29               ; bit LM (long mode) em CPUID.80000001:EDX
    jz .fail
    ret
.fail:
    mov al, '2'
    jmp error

; --------------------------------------------------------------------------
; Monta tabelas: identidade dos primeiros 1 GiB com paginas de 2 MiB.
; (escrevemos tambem os dwords altos = 0 para nao depender do .bss zerado)
; --------------------------------------------------------------------------
setup_page_tables:
    mov eax, p3_table
    or  eax, 0b11                    ; present + writable
    mov [p4_table], eax
    mov dword [p4_table + 4], 0

    mov eax, p2_table
    or  eax, 0b11
    mov [p3_table], eax
    mov dword [p3_table + 4], 0

    mov ecx, 0
.map:
    mov eax, 0x200000               ; 2 MiB
    mul ecx                         ; eax = 2MiB * ecx
    or  eax, 0b10000011             ; present + writable + huge(2MiB)
    mov [p2_table + ecx*8], eax
    mov dword [p2_table + ecx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne .map
    ret

enable_paging:
    mov eax, p4_table
    mov cr3, eax                    ; CR3 -> PML4

    mov eax, cr4
    or  eax, 1 << 5                 ; CR4.PAE
    mov cr4, eax

    mov ecx, 0xC0000080            ; MSR EFER
    rdmsr
    or  eax, 1 << 8                ; EFER.LME (habilita long mode)
    wrmsr

    mov eax, cr0
    or  eax, 1 << 31               ; CR0.PG (liga paginacao)
    mov cr0, eax
    ret

; imprime "ERR:x" (x = AL) no canto da tela VGA e trava
error:
    mov dword [0xb8000], 0x4f524f45 ; "ER"
    mov dword [0xb8004], 0x4f3a4f52 ; "R:"
    mov byte  [0xb8008], al
    mov byte  [0xb8009], 0x4f
.halt:
    hlt
    jmp .halt

; ============================================================================
bits 64
long_mode_start:
    mov ax, 0                       ; em long mode os seletores de dados sao 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov rsp, stack_top

    ; habilita SSE (o compilador C pode emitir instrucoes SSE)
    mov rax, cr0
    and ax, 0xFFFB                  ; limpa CR0.EM
    or  ax, 0x2                     ; seta CR0.MP
    mov cr0, rax
    mov rax, cr4
    or  rax, (1 << 9) | (1 << 10)   ; CR4.OSFXSR | CR4.OSXMMEXCPT
    mov cr4, rax

    mov edi, [rel mb_info_ptr]      ; 1o arg de kmain: ponteiro do Multiboot info (salvo no boot)
    call kmain                      ; -> codigo C de 64 bits
.hang:
    cli
    hlt
    jmp .hang
