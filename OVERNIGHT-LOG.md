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

| 2 | DPC (fila per-CPU + drena no KeLowerIrql) | ✅ | ✅ idêntico (CPUID x3, C0000365) | preempção viva (51 beats) | DPC disparou inline OK | 7acfb37 |
| 0a | Reentrância do swap (irq_save/restore) | ✅ | ✅ idêntico | preempção viva (60 beats) | — | (este) |

**Sessão retomada ("Continua").** pintok **não** usa `KeWaitForSingleObject`/`KeInitializeEvent` (verificado no baseline) → Item 5 (waits reais) será **gated por contexto** (só threads worker reais bloqueiam; contexto boot/idle, onde o pintok roda, mantém o auto-resolve). Próximos: Item 5 (waits), Item 6 (KTIMER), Item 7 (Ex).

---

## RESUMO PARA ACORDAR (parada segura)

**Seu trabalho está salvo:** checkpoint `2e652e9` na sua branch `feat/unblock-desktop-mouse-irq12`, **pushed** para o GitHub.

**Fundação construída (branch isolada `feat/kernel-foundation-irql-dpc`, tudo pushed):**
| commit | item | o que ficou real |
|--------|------|------------------|
| `5557e5b` | Item 4 | `KeStallExecutionProcessor` (busy-spin por TSC) + `KeQueryPerformanceCounter` (retorna contador real). TSC calibrado ~3.1 GHz. |
| `b11fa80` | Item 1 | IRQL real: `KeGetCurrentIrql` lê `gs:[0x60]`; raise/lower mantêm `gs:[0x60]`+CR8. Níveis 0–15. |
| `1ef56bc` | Item 3 | Spinlocks reais (atômico + raise IRQL) + gating de preempção no ISR do timer ("raise a DISPATCH bloqueia preempção"). |

**Cada incremento foi verificado:** build OK + **pintok baseline byte-idêntico** (P1/P2/P3 PROVA PASSOU, `chamando DriverEntry`, CPUID x3, retorno C0000365) + preempção viva (workers batendo) + sem `Sistema parado`. **O pintok está intocado.**

**Deferido para quando você estiver acordado (motivo: tocam o caminho de context-switch OU a trajetória do pintok — não faço sem supervisão):**
- **Item 2 (DPC)** — o pintok chama `KeInitializeDpc`/`KeGenericCallDpc`/`KeSetTargetProcessorDpc`/`KeSignalCallDpc*`; DPC real tem superfície de interação com ele. Design pronto no plano.
- **Item 0a + 5 (reentrância do swap + `KeWait` bloqueante)** — mexem em `ki_swap_context` e no "auto-resolve" que o pintok usa. São uma unidade.
- **Item 6 (KTIMER)**, **Item 7 (primitivos Ex: FAST_MUTEX/ERESOURCE/lookaside)** — dependem do Item 5.
- **Trilha I/O inteira** (IRP layout NT, device stacks, `IoConnectInterrupt`, PnP, DMA) + os drivers de teste (`tests/drivers/`).

**Como continuar:** branch isolada, cada item é um commit limpo (revertível), design completo em `C:\Users\joao\.claude\plans\lovely-baking-whale.md`. Nada está quebrado; pior caso, revisar/reverter por commit. Baseline dourado do pintok em `$JOB/tmp/pintok_baseline.log`.
