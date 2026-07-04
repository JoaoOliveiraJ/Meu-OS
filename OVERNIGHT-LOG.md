# OVERNIGHT-LOG — Fundação Ke/HAL (branch `feat/kernel-foundation-irql-dpc`)

Trabalho autônomo enquanto o usuário dorme. Cada incremento: edita → `build.ps1` → `run.ps1` (pintok) → verifica **pintok ≥ baseline** + `proof_pillar4` → **commit + push**. Parada segura em qualquer falha (reverte, loga, para).

Referência do plano: `C:\Users\joao\.claude\plans\lovely-baking-whale.md`.

## Setup (concluído)
- Checkpoint do trabalho do usuário: commit `2e652e9` em `feat/unblock-desktop-mouse-irq12`, **pushed** para `origin`.
- Branch isolada criada: `feat/kernel-foundation-irql-dpc`.
- Build confirmado: `./build.ps1` → exit 0, `kernel.bin` OK (289628 bytes).
- Baseline dourado do pintok capturado: `./run.ps1 -Modules build/pintok.sys -Headless -TimeoutSec 40` (log completo em `$JOB/tmp/pintok_baseline.log`).

## Baseline dourado do pintok — assinatura estável (NÃO pode regredir)
```
[P1] ==== PROVA PASSOU ====
[P2] ==== PROVA PASSOU ====
[P3] ==== PROVA PASSOU ====   (P4 PAUSADA)
[boot] driver de kernel: pintok.sys
[io] chamando DriverEntry...
  [pintok.syslog] '1.17.18-11+20260212.201837 ; ...'
  [pintok.syslog] '10 ; 0 ; 26100.'            (Windows build 26100)
  [intercept] CPUID leaf=0/1/7 -> Intel i7-9700K   (x3)
[io] intercept totals: CPUID x3  RDTSC x~33  RDMSR x0  ANTIVM x0
[io] DriverEntry retornou status=0x00000000C0000365
```
- **Gates estáveis**: P1/P2/P3 PASSOU; "chamando DriverEntry"; CPUID x3; a sequência syslog; retorno C0000365.
- **RDTSC x33 varia** com timing (single-step) — não é gate rígido.
- **C0000365 é o teto atual do pintok** (ele falha internamente aí). Regressão = chegar MENOS fundo, sumir CPUID x3 / syslog, aparecer `Sistema parado` ou nova exceção não-recuperada. Ir além de C0000365 não é regressão (não é o objetivo, mas é aceitável).

## Incrementos
| # | Item | build | pintok≥baseline | proof P4 | probe | commit |
|---|------|-------|-----------------|----------|-------|--------|
| — | setup/baseline | ✅ | — (baseline) | pausada | — | 2e652e9 |
| 4 | Stall/PerfCounter (TSC) | ✅ | ✅ idêntico (CPUID x3, C0000365) | — | TSC=3.09GHz OK | 5557e5b |
| 1 | IRQL real (gs:[0x60]+CR8) | ✅ | ✅ idêntico | — | — (probe depois) | b11fa80 |
| 3 | Spinlocks reais + gating preempção | ✅ | ✅ idêntico (sem halt) | preempção viva (66 beats) | — | (este) |

Ordem noturna: **4 → 1 → 3 → [2] → parada segura**. Deferidos p/ supervisão (tocam contexto de troca ou trajetória do pintok): **0a+5** (reentrância do swap + waits — são uma unidade, testar juntas com você acordado), **6** (KTIMER), **7** (Ex), **trilha I/O** inteira. Motivo: mexer no caminho de context-switch (`ki_swap_context`) ou tornar `KeWait` bloqueante sem supervisão é o maior risco à trajetória do pintok.

(em progresso — Item 2: DPC)
