#pragma once
#include <stdint.h>

// ============================================================================
//  virtio_input.h — Driver virtio-input PCI (modern transport, virtio 1.1).
//
//  Motivo: o mouse PS/2 (relativo) numa janela do QEMU nao tem "mouse
//  integration": o host captura (grab) o ponteiro pra entregar deltas e, durante
//  o grab, ESCONDE o cursor (entao o cursor de HW do virtio-gpu fica invisivel);
//  alem disso o eixo X satura (dx=-127 fixo, cursor preso na borda). A cura — a
//  mesma que uma VM Windows real usa no QEMU — e' ler um dispositivo ABSOLUTO.
//
//  Este driver dirige um `virtio-tablet-pci` (vendor 0x1AF4, device 0x1052):
//  ao chegar em DRIVER_OK, o QEMU passa a tratar a tablet como ponteiro ATIVO
//  (modo absoluto) -> PARA de capturar o mouse -> o cursor de HW aparece e segue
//  o ponteiro; e o guest le coordenadas absolutas (eventq) -> sabe onde o cursor
//  esta -> hit-test/cliques corretos.
//
//  Entrega de eventos: POLLING (sem IRQ). A eventq (queue 0) e' populada com
//  buffers graváveis pelo device; virtio_input_poll() drena o used ring,
//  traduz os eventos Linux (EV_ABS/EV_KEY/EV_SYN) e chama win32k_on_mouse_abs.
//  Chamado nos loops ociosos (kmain idle + NtUserGetMessage), acordados pelo
//  APIC timer (100 Hz). O cursor visual e' renderizado pelo HOST na posicao do
//  ponteiro, entao 100 Hz de poll so afeta hit-test (suave o bastante).
// ============================================================================

// Detecta o virtio-tablet no PCI, faz o setup virtio modern (ACK->DRIVER->
// FEATURES_OK->eventq->DRIVER_OK) e popula a eventq. No-op se nao houver device.
void virtio_input_init(void);

// Drena os eventos pendentes na eventq e roteia ao win32k (absoluto). Barato
// quando nao ha eventos. Chamado nos idle loops.
void virtio_input_poll(void);

// 1 se a tablet esta ativa (DRIVER_OK + eventq pronta).
int  virtio_input_active(void);
