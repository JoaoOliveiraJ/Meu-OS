; ============================================================================
;  sched_asm.asm — KiSwapContext + ki_thread_startup_asm (Pilar 4).
;
;  void ki_swap_context(uint64_t* save_rsp, uint64_t next_rsp);
;
;  Salva os callee-saved (RBX, RBP, R12-R15) na stack atual + grava RSP em
;  *save_rsp. Carrega RSP de next_rsp; pop dos callee-saved; ret. O ret vai
;  ao endereco no topo da nova stack — que e' ou:
;    (a) o ret address de um KiSwapContext anterior (thread ja rodou); OU
;    (b) ki_thread_startup_asm (thread NOVA, montada por ki_create_thread).
;
;  Convencao System V x86_64:
;    RDI = save_rsp (ponteiro para gravar)
;    RSI = next_rsp (valor a carregar)
;    Callee-saved que precisamos preservar entre chamadas: RBX, RBP, R12-R15.
;    (R10/R11 sao caller-saved no SysV — nao precisam salvar aqui.)
; ============================================================================

extern ki_thread_startup           ; C function

section .text
bits 64

global ki_swap_context
ki_swap_context:
    ; Salva callee-saved na stack atual.
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    ; Salva RSP de cur em *save_rsp (rdi).
    mov [rdi], rsp

    ; Carrega RSP de next.
    mov rsp, rsi

    ; Pop callee-saved da nova stack.
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp

    ; ret: leva ao topo da nova stack (= ret addr salvo pelo KiSwapContext
    ; anterior, OU = ki_thread_startup_asm para thread nova).
    ret


; ---------------------------------------------------------------------------
;  ki_thread_startup_asm: wrapper em asm sobre ki_thread_startup (C).
;
;  Quando KiSwapContext "ret" em uma thread NOVA, a primeira execucao cai
;  aqui (montado por ki_create_thread como o ret-address inicial). Aqui:
;   1) ja saimos do contexto de KiSwapContext; RSP aponta para o topo limpo
;      da nova stack.
;   2) Chama ki_thread_startup (C), que faz sti + chama entry(arg) + termina.
; ---------------------------------------------------------------------------
global ki_thread_startup_asm
ki_thread_startup_asm:
    call ki_thread_startup
    ; Se ki_thread_startup retornar (nao deveria), halts forever.
.hang:
    cli
    hlt
    jmp .hang
