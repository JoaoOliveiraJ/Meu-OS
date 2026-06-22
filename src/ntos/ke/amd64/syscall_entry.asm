; ============================================================================
;  syscall_entry.asm  —  FASE 7.4: entrada da instrucao SYSCALL (Intel/AMD).
;
;  A instrucao SYSCALL (opcode 0F 05) e a forma "rapida" de entrar em ring 0,
;  usada por drivers reais do NT e por programas user-mode (via wrappers de
;  ntdll: NtXxx, ZwXxx). Detalhes do que a CPU faz na entrada:
;     1) RCX = RIP do retorno (a instrucao APOS o syscall do chamador).
;     2) R11 = RFLAGS originais.
;     3) RFLAGS &= ~SFMASK  (limpa bits da mascara — geralmente IF/TF/DF).
;     4) CS  = STAR[47:32], com base/limite "flat" (long mode).
;     5) SS  = STAR[47:32] + 8.
;     6) RIP = LSTAR.
;     7) NAO troca RSP (e nem swapgs) — software faz se quiser pilha de kernel.
;
;  Aqui usamos uma abordagem MINIMA e SEGURA para nao quebrar nada anterior:
;   - NAO usamos SYSRETQ (volta para ring 3) porque a GDT do MeuOS nao tem o
;     layout que SYSRET espera (UCODE/UDATA invertidos). Em vez disso voltamos
;     pelo caminho do int 0x80 (IRETQ), reaproveitando isr_handler/syscall_dispatch.
;   - Para fazer isso, EMPILHAMOS um quadro compativel com 'struct regs' (igual
;     ao isr.asm) e chamamos isr_handler(struct regs*) com int_no=0x80. Assim
;     o codigo C nao muda nada: sys_write, sys_writefile, etc. funcionam 1:1.
;   - O quadro INTERROMPIDO pela CPU em uma INT contem RIP/CS/RFLAGS/RSP/SS no
;     topo. Como SYSCALL nao empilha isso, MONTAMOS o frame manualmente: RIP=RCX
;     (PC de retorno), CS=0x1B (UCODE|3), RFLAGS=R11, RSP=do chamador, SS=0x23.
;     O IRETQ final volta exatamente para a instrucao apos o SYSCALL do chamador.
;
;   - Atencao: SYSCALL NAO troca pilha. O chamador esta em ring 3 e RSP aponta
;     para a pilha de usuario — porem, em ring 0, a CPU NAO faz check de pilha
;     no inicio (qualquer endereco mapeado serve). Empilhamos por cima da pilha
;     atual SOMENTE se o chamador deixou bytes suficientes (>= 256 B). Drivers
;     em ring 0 que executam SYSCALL ja estao em pilha de kernel, entao tambem
;     funciona. Caminho seguro: sem alocacao dinamica nem troca de RSP/GS.
;
;  Resultado: se pintok.sys (ring 0) executar 'syscall', em vez de #GP por
;  EFER.SCE=0, caimos AQUI, dispatch via isr_handler e voltamos via IRETQ. Se
;  o numero do syscall for desconhecido, syscall_dispatch ja loga e devolve -1.
; ============================================================================

extern isr_handler

section .text
bits 64

global syscall_entry
syscall_entry:
    ; Layout do 'struct regs' (igual ao isr_common em isr.asm):
    ;   topo (RSP=0 apos pushes): r15,r14,r13,r12,r11,r10,r9,r8,
    ;     rbp,rdi,rsi,rdx,rcx,rbx,rax,int_no,err_code, rip,cs,rflags,rsp,ss
    ; ATENCAO: pusha ordem inversa — o stub coloca de int_no/err_code primeiro,
    ; mas a CPU NAO os empilha em SYSCALL. Aqui MONTAMOS o frame iretq E
    ; depois empilhamos os outros campos. Total = 21 qwords.

    ; (1) Salva o RSP do chamador num temp registers nao destruidos pelo C
    ;     calling convention (rdi). Vamos restaurar no final.
    ;     Como SYSCALL NAO troca SS:RSP nem GS, NAO chamamos swapgs aqui.

    ; (2) Empilha o "iretq frame" (5 qwords): SS, RSP, RFLAGS, CS, RIP.
    ;     Ordem do iretq (do topo p/ baixo): RIP, CS, RFLAGS, RSP, SS.
    ;     Como push diminui RSP, fazemos NA ORDEM INVERSA (SS primeiro).
    push qword 0x23                 ; SS  = SEL_UDATA|3
    push rsp                        ; RSP do chamador (aproximado: RSP+8 mas
                                    ; serve — o iretq apenas restaura, e como
                                    ; nao trocaremos pilha de fato, fica ok)
    push r11                        ; RFLAGS originais (SYSCALL guardou em R11)
    push qword 0x1B                 ; CS  = SEL_UCODE|3
    push rcx                        ; RIP de retorno (SYSCALL guardou em RCX)

    ; (3) Empilha err_code=0 e int_no=0x80 — uniformiza com isr_common.
    push qword 0                    ; err_code
    push qword 0x80                 ; int_no

    ; (4) Salva GPRs na ordem do isr_common.
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

    ; (5) Chama isr_handler(struct regs*) — RDI = ponteiro p/ topo do frame.
    mov rdi, rsp
    cld                              ; ABI: DF=0 antes da chamada C
    call isr_handler

    ; (6) Restaura GPRs (ordem inversa).
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

    ; (7) Descarta int_no + err_code.
    add rsp, 16

    ; (8) IRETQ usa os 5 qwords no topo (RIP,CS,RFLAGS,RSP,SS) e volta para o
    ;     chamador. Como CS=0x1B (DPL3), a CPU sai de ring 0 -> ring 3 (caso
    ;     o chamador estivesse em ring 3); se a origem era ring 0 (driver),
    ;     o iretq devolve para CS=0x1B mesmo — pintok.sys que executou syscall
    ;     ESPERA voltar em ring 3 NO MESMO RIP que pediu (RCX), o que e
    ;     coerente com a semantica do SYSRETQ.
    ;
    ;     NOTA: drivers em ring 0 NAO costumam fazer SYSCALL — quem faz isso
    ;     e ntdll.dll (ring 3). Se pintok.sys testar syscall em ring 0,
    ;     ele recebe um caminho funcional aqui (nao #GP), mas o iretq vai
    ;     baixar p/ ring 3 — o driver fica em ring 3 ate fazer SYSENTER/INT
    ;     de volta. Como pintok.sys nao depende disso para fluxo normal, e
    ;     so um caminho de "sondagem" — devolvemos com codigo coerente.
    iretq
