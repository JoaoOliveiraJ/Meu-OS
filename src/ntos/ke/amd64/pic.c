#include "ke/amd64/pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

void pic_remap(void) {
    // Sequencia de inicializacao (ICW1..ICW4)
    outb(PIC1_CMD, 0x11); io_wait();   // inicia, espera ICW4
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();  // master: vetores comecam em 32
    outb(PIC2_DATA, 0x28); io_wait();  // slave:  vetores comecam em 40
    outb(PIC1_DATA, 0x04); io_wait();  // master: slave ligado na IRQ2
    outb(PIC2_DATA, 0x02); io_wait();  // slave:  identidade em cascata
    outb(PIC1_DATA, 0x01); io_wait();  // modo 8086
    outb(PIC2_DATA, 0x01); io_wait();

    // Mascaras: habilita apenas IRQ0 (timer) e IRQ1 (teclado).
    outb(PIC1_DATA, 0xFC);             // 1111 1100
    outb(PIC2_DATA, 0xFF);             // tudo mascarado no slave
}

void pic_eoi(int irq) {
    if (irq >= 8) outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}
