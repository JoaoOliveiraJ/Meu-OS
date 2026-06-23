PAUSADO: Fundação NT (APIC/SMP/Scheduler) — branch feat/nt-foundation-apic-smp.

Pilares 1–3 (paginação dinâmica, APIC, SMP bring-up) PASSARAM com provas comportamentais, sólidos. Pilar 4 (scheduler MP) travou por ambiente, não arquitetura: o TCG do QEMU não emula SMP de forma confiável no host Windows (timers phase-locked, AP timer não entregue, IPI cross-CPU trava). WHPX rejeitado (hypervisor não carregado no boot; usuário não quer rebootar). Teste WSL-TCG não chegou a ser feito.

O único bug de CÓDIGO real é a swap A→B (thread B nunca roda; suspeita de re-entrância do timer no KiSwapContext — provável correção: elevar a DISPATCH_LEVEL e mascarar interrupção durante todo o swap, modelo NT). Esse bug é diagnosticável até em single-core lógico.

Para retomar: decidir ambiente de prova SMP (WSL-TCG / KVM-Linux real / CI), depois consertar a swap A→B, depois (C) per-PRCB lock e (D) KeWait/KeSetEvent com bloqueio real. Workarounds de TCG aplicados (AP a 10 Hz, IPI cross-CPU desligado) precisam ser REMOVIDOS no ambiente são — eles corromperam a prova MP.

---

## ATUALIZAÇÃO 2026-06-23 (rodada "destravar desktop") — proof_pillar4 DESACOPLADA do kmain

A prova do Pilar 4 (`proof_pillar4_scheduler`) ESTAVA sendo CHAMADA no kmain como
gate fatal (`if (!proof_pillar4_scheduler()) { halt }`, ~main.c:969). Foi
**DESACOPLADA do boot** nesta rodada. Estado atual em `src/ntos/init/main.c`:
uma guarda explícita `static const int p4_proof_enabled = 0;` envolve a chamada;
com 0 a prova NÃO é chamada (loga `[P4] ... PAUSADA (desacoplada do boot)`).
**NÃO reative (p4_proof_enabled = 1) sem antes consertar o swap A→B (KiSwapContext).**

Por que desacoplar e não só tornar o gate não-fatal: a prova **NUNCA RETORNA** (não
é "falha e retorna 0"). Ela ativa a preempção MP (`g_p4_active=1` + ready das threads
A/B); o APIC timer troca o BSP para a thread A que, com o swap A→B quebrado, nunca é
preemptada de volta — A monopoliza o BSP e a serial trava em `[P4] A counter=...`.
Tornar o `if` não-fatal não adianta: o problema é o não-retorno, não o `halt`.

### ACHADO NOVO — SEGUNDO hang atrás do P4 (bloqueia o desktop)

Desacoplar o P4 revelou um **segundo hang** que o P4 escondia. Com o P4 fora do
caminho, o boot passa do P4 mas **trava em `mm_map_kuser_shared_data()`**
(~main.c:1016, FASE 7.1), **antes** de `gpu_init`/win32k/desktop. Esse trecho roda
pela 1ª vez com o **APIC timer ativo (P2)** e o **AP vivo (P3)** — a 00:30
(pré-pilares) rodava limpo. O ponto de travamento **jittera** entre boots (ora em
`[hv] resumo`, ora dentro do `mm_map`); `cli/sti` em volta das chamadas suspeitas
NÃO estabilizou → é uma **race do timer/SMP**, não um deadlock único. `qemu.err.log`
vazio = hang, não triple-fault. Provável mesma raiz do swap A→B: **re-entrância do
APIC timer ISR** (o ISR chama `mm_kuser_tick()` a cada tick — isr.c:299) colidindo
com código de boot que toca o mesmo estado/lock, sem máscara de IRQL.

**Conclusão da rodada:** pausar o P4 é **necessário mas NÃO suficiente** para
destravar o desktop. A fundação APIC/SMP (timer + AP ativos) desestabiliza todo o
boot pós-P4 de forma não-determinística. O desktop (provado a 00:30, pré-fundação)
não volta só desacoplando o P4. Caminhos possíveis (decisão de arquitetura pendente
com o dono — rodada PARADA aqui, sem mexer no mouse):
  1. **Quiescer a fundação durante o boot:** mascarar o LVT timer / parar o AP após
     as provas, mantendo o **IO-APIC** ligado para rotear o IRQ12 do mouse. Desktop
     volta; mouse via IO-APIC continua viável. Mexe em apic em sentido "desligar".
  2. **Basear desktop/mouse no commit pré-fundação** (antes de P1): desktop boota
     limpo no regime PIC, mas o fix do mouse seria PIC (não o IO-APIC redirect).
  3. **Fix-forward** os hangs pós-P4 (timer re-entrância / IRQL) — é justamente a
     fundação PAUSADA; whack-a-mole provável (mm_map, depois gpu_init, win32k...).

### RESOLUÇÃO desta rodada — escolhida a Opção 1 (QUIESCE do APIC timer)

O **APIC timer foi QUIESCIDO como parte de COMPLETAR a pausa da fundação SMP/
scheduler.** Com o Pilar 4 desacoplado, o timer não serve a ninguém (não há
preempção MP) e a sua ISR (mm_kuser_tick) era o que travava o boot pós-P3. Mudanças:
  - **BSP:** `apic_mask_timer_local()` (novo, em `apic.c`) é chamado no `kmain`
    logo após as provas P1–P3 (e o bloco P4 desacoplado). Mascara o LVT timer do
    BSP (bit 16). Loga `[apic] LVT timer do BSP MASCARADO`.
  - **AP:** `ap_entry` (`smp.c`) **não chama mais** `apic_unmask_timer_local()` — o
    timer do AP fica mascarado (como `apic_enable_local` já o deixou) e o AP dorme
    de fato em `hlt` (sem ISR de timer, sem contenção de LAPIC timers no TCG).
  - O **IO-APIC permanece ATIVO** — o IRQ12 do mouse continua roteável (necessário
    para o Passo 2 desta rodada).

**Efeito colateral aceito:** `g_ticks` congela (relógio da taskbar fica estático em
00:00). Nada no caminho do boot/desktop depende de `g_ticks` avançar
(`KeDelayExecutionThread_k` não tem caller no boot), então não há novo hang.

**Provado:** o desktop volta a 1024×768 (wallpaper + 2 janelas cmd + taskbar +
cursor) e é **estável em 3 boots consecutivos** — serial chega a
`[win32k] estado final: desktop` com 1116 linhas idênticas, e os 3 screendumps são
**byte-a-byte idênticos** (vs. os hangs pré-quiesce que jitteravam em 99/111 linhas).

> **REATIVAR o timer (BSP `apic_mask_timer_local` removido + AP
> `apic_unmask_timer_local` de volta) FAZ PARTE DE RETOMAR O PILAR 4 (scheduler MP).
> Não reative o timer isoladamente — ele só existe, hoje, para a preempção MP que
> está pausada. Reativá-lo sem o swap A→B consertado traz de volta o hang.**