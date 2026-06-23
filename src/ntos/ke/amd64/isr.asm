; ============================================================================
;  isr.asm  —  Stubs de interrupcao (exceptions 0..31 e IRQs 32..47).
;
;  Cada stub empilha (err_code, int_no), salva os registradores, chama
;  isr_handler(struct regs*) em C e retorna com iretq.
; ============================================================================
extern isr_handler

section .text
bits 64

; Macro para vetores SEM codigo de erro: empurramos um 0 fake para uniformizar.
%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push 0
    push %1
    jmp isr_common
%endmacro

; Macro para vetores que JA tem codigo de erro empurrado pela CPU.
%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push %1
    jmp isr_common
%endmacro

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp            ; ponteiro para struct regs
    call isr_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16             ; descarta int_no e err_code
    iretq

; ---- Exceptions 0..31 (as com codigo de erro: 8,10,11,12,13,14,17,21) ----
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; ---- IRQs 32..47 (vetores PIC remapeados) ----
%assign irqn 32
%rep 16
    ISR_NOERR irqn
%assign irqn irqn+1
%endrep

; ---- Vetores 48..127 (sera usado por IO-APIC redirects, APIC timer etc.) ----
%assign irqn 48
%rep 80
    ISR_NOERR irqn
%assign irqn irqn+1
%endrep

; ---- Syscall: vetor 0x80 (128), invocavel do ring 3 (gate DPL=3) ----
ISR_NOERR 128

; ---- Vetores 129..255 (APIC timer 0xD1, IPI 0xE1, spurious 0xFF, etc.) ----
%assign irqn 129
%rep 127
    ISR_NOERR irqn
%assign irqn irqn+1
%endrep

; ---- Tabela com os 256 enderecos (usada pela idt.c) ----
section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep
