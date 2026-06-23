; ============================================================================
;  ap_trampoline.asm  —  Trampoline de boot dos Application Processors (Pilar 3).
;
;  Este blob e' COPIADO em runtime para o endereco fisico 0x8000 (vetor SIPI 0x08).
;  Depois o BSP envia INIT-SIPI-SIPI para cada AP, e o AP comeca a executar AQUI
;  em REAL MODE com CS:IP = 0x0800:0x0000 (= phys 0x8000).
;
;  Sequencia da trampoline:
;    16-bit real mode -> 32-bit protected mode -> 64-bit long mode
;    -> carrega RSP do AP, programa GS_BASE/KERNEL_GS_BASE com o KPCR do AP,
;       chama ap_entry(): C funcao definida em smp.c.
;
;  TODOS os enderecos absolutos sao escritos como "0x8000 + (label - blob_start)"
;  porque a trampoline e' position-independent: o assembler nao sabe onde vai
;  rodar, mas sabe que sera em phys 0x8000 (vetor SIPI fixo). O 'ap_trampoline_*_off'
;  exporta os offsets dos slots de dados pra smp.c patchear antes da SIPI.
;
;  Referencia: Intel SDM Vol 3 8.4 (Multiple-Processor Initialization).
; ============================================================================

%define BASE 0x8000

section .data
global ap_trampoline_blob
global ap_trampoline_blob_end
global ap_trampoline_size
global ap_trampoline_stack_off
global ap_trampoline_kpcr_off
global ap_trampoline_entry_off
global ap_trampoline_pml4_off

ap_trampoline_blob:

; --------------------------------------------------------------------------
;  16-bit real mode (entry from SIPI)
;
;  IMPORTANTE: em real mode, [imm16] e' INTERPRETADO como offset RELATIVO ao
;  segmento DS. CS = 0x0800 (set pelo SIPI page 0x08), entao DS = CS = 0x0800
;  -> phys = DS*16 + offset = 0x8000 + offset. Logo o operand do lgdt deve ser
;  APENAS o offset (rotulo - blob_start), SEM somar BASE — somar BASE somaria
;  duas vezes e a CPU leria phys 0x100XX (lixo) em vez de 0x80XX (nosso GDT).
;
;  Ja o far-jmp imm32:imm16 (com prefixo 0x66) carrega EIP = imm32 LINEAR
;  (CS-base = 0 na entry 0x08 da GDT), entao para o far-jmp usamos
;  BASE + (label - blob_start), que IS o endereco linear apos PE=1.
; --------------------------------------------------------------------------
bits 16
    cli
    cld

    ; Inicializa segmentos para o CS atual (= 0x0800).
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Carrega GDT de 32 bits. Operand = OFFSET dentro do segmento DS=0x0800,
    ; NAO endereco linear. gdt32_ptr - blob_start e' a posicao do struct.
    lgdt [gdt32_ptr - ap_trampoline_blob]

    ; CR0.PE = 1
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Far jump para 32-bit. operand-size 0x66 prefix faz imm32:imm16:
    ; imm32 = endereco LINEAR (PE=1, CS=0x08 com base 0); imm16 = seletor.
    jmp dword 0x08:(BASE + (pm_start - ap_trampoline_blob))

; --------------------------------------------------------------------------
;  32-bit protected mode (transito)
; --------------------------------------------------------------------------
bits 32
pm_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; CR4.PAE = 1 (obrigatorio em LM)
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; CR3 = PML4 do kernel (mesmo do BSP). Slot 'ap_tramp_pml4' patchado.
    mov eax, [BASE + (ap_tramp_pml4 - ap_trampoline_blob)]
    mov cr3, eax

    ; EFER.LME = 1 (long mode enable)
    mov ecx, 0xC0000080
    rdmsr
    or eax, (1 << 8)
    wrmsr

    ; CR0.PG = 1 -> entra em long mode
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    ; Far jump para 64-bit code (selector 0x18 com L=1 na nossa GDT).
    jmp dword 0x18:(BASE + (lm_start - ap_trampoline_blob))

; --------------------------------------------------------------------------
;  64-bit long mode — primeira instrucao em modo 64-bit do AP
; --------------------------------------------------------------------------
bits 64
lm_start:
    ; Limpa segmentos data com selector data 64-bit.
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; RSP = stack do AP (slot 'ap_tramp_stack' patchado pelo BSP).
    mov rax, [BASE + (ap_tramp_stack - ap_trampoline_blob)]
    mov rsp, rax

    ; Programa GS_BASE = ap_kpcr. MSR 0xC0000101 = IA32_GS_BASE.
    mov rax, [BASE + (ap_tramp_kpcr - ap_trampoline_blob)]
    mov rdx, rax
    shr rdx, 32
    mov ecx, 0xC0000101
    wrmsr
    ; KERNEL_GS_BASE igual (sem swapgs assimetrico no kernel single-ring).
    mov ecx, 0xC0000102
    wrmsr

    ; Chama ap_entry() — definido em smp.c. RAX = ponteiro absoluto.
    mov rax, [BASE + (ap_tramp_entry - ap_trampoline_blob)]
    call rax

    ; ap_entry nao deve retornar; mas se retornar, halt eterno.
.hang:
    cli
    hlt
    jmp .hang

; --------------------------------------------------------------------------
;  GDT temporaria (32-bit + 64-bit code/data)
; --------------------------------------------------------------------------
align 8
gdt32:
    dq 0                                   ; null
    dq 0x00CF9A000000FFFF                  ; 0x08: 32-bit code (D/B=1, L=0, RX)
    dq 0x00CF92000000FFFF                  ; 0x10: 32-bit data
    dq 0x00AF9A000000FFFF                  ; 0x18: 64-bit code (L=1)
    dq 0x00AF92000000FFFF                  ; 0x20: 64-bit data
gdt32_end:

gdt32_ptr:
    dw (gdt32_end - gdt32) - 1
    dd BASE + (gdt32 - ap_trampoline_blob)

; --------------------------------------------------------------------------
;  Slots patchados pelo BSP em smp.c antes da SIPI
; --------------------------------------------------------------------------
align 8
ap_tramp_stack:  dq 0
ap_tramp_kpcr:   dq 0
ap_tramp_entry:  dq 0
ap_tramp_pml4:   dq 0
ap_trampoline_blob_end:

; --------------------------------------------------------------------------
;  Tamanho do blob + offsets dos slots (constantes link-time exportadas)
; --------------------------------------------------------------------------
ap_trampoline_size:      dq (ap_trampoline_blob_end - ap_trampoline_blob)
ap_trampoline_stack_off: dq (ap_tramp_stack - ap_trampoline_blob)
ap_trampoline_kpcr_off:  dq (ap_tramp_kpcr  - ap_trampoline_blob)
ap_trampoline_entry_off: dq (ap_tramp_entry - ap_trampoline_blob)
ap_trampoline_pml4_off:  dq (ap_tramp_pml4  - ap_trampoline_blob)
