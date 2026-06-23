// ============================================================================
//  mouse.c  —  Driver PS/2 do mouse (IRQ12).
//
//  Modelo do controlador i8042 (mesmas portas do teclado, mas com prefixo 0xD4):
//    - 0x60: data port (read/write para o dispositivo selecionado).
//    - 0x64: status (read) e command (write).
//
//  Comandos do controlador (escritos em 0x64):
//    0xA8 — Enable Aux Device (habilita a porta do mouse).
//    0x20 — Read Configuration Byte (le no 0x60 logo apos).
//    0x60 — Write Configuration Byte (escreve no 0x60 logo apos).
//    0xD4 — "Next byte vai para o dispositivo auxiliar (mouse)". Sem este
//           prefixo, escrever em 0x60 vai para o TECLADO.
//
//  Comandos do mouse (apos 0xD4, no 0x60):
//    0xF6 — Set Defaults (sample rate 100, resolution 4 counts/mm, scaling 1:1).
//    0xF4 — Enable Stream Mode (o mouse passa a enviar pacotes via IRQ12).
//
//  Pacote padrao (3 bytes):
//    byte 0: bit0=L bit1=R bit2=M bit3=1 (sempre) bit4=X_sign bit5=Y_sign
//            bit6=X_overflow bit7=Y_overflow
//    byte 1: deltaX (estendido com sinal a partir do bit4 do byte 0)
//    byte 2: deltaY (idem; positivo = pra CIMA no PS/2, INVERTIDO em relacao
//            a coordenada da tela)
//
//  IRQ12 (vector 44 apos pic_remap):
//    Cada IRQ entrega APENAS 1 byte. Mantemos uma maquina de estado (s_phase)
//    pra montar o pacote de 3 bytes. Se o byte 0 chega sem o bit3=1, a sequencia
//    esta desalinhada (host perdeu o sync); descartamos esse byte e tentamos
//    de novo. Isso e a recuperacao classica do i8042.
// ============================================================================
#include <stdint.h>
#include "mouse.h"
#include "io.h"
#include "ke/amd64/pic.h"
#include "ke/amd64/apic.h"
#include "win32/win32k.h"

extern void kputs(const char* s);
extern void kputc(char c);
extern void kput_hex(uint64_t v);
extern void kput_dec(uint64_t v);

// ----- portas do controlador i8042 (mesmo do teclado) -----
#define I8042_DATA    0x60
#define I8042_STATUS  0x64
#define I8042_CMD     0x64

// ----- bits do status (port 0x64) -----
#define I8042_ST_OUTPUT_FULL  0x01   // 1 = tem byte pronto para leitura em 0x60
#define I8042_ST_INPUT_FULL   0x02   // 1 = controller ainda ocupado com nosso byte
#define I8042_ST_AUX_DATA     0x20   // 1 = byte em 0x60 veio do MOUSE (aux), nao do teclado

// ----- comandos do controlador i8042 (port 0x64) -----
#define I8042_CMD_ENABLE_AUX     0xA8
#define I8042_CMD_READ_CONFIG    0x20
#define I8042_CMD_WRITE_CONFIG   0x60
#define I8042_CMD_WRITE_AUX      0xD4   // prefixo: proximo byte em 0x60 vai p/ o mouse

// ----- comandos do MOUSE (apos 0xD4, no port 0x60) -----
#define MOUSE_CMD_SET_DEFAULTS   0xF6
#define MOUSE_CMD_ENABLE_STREAM  0xF4

// ACK do dispositivo aux.
#define PS2_ACK                  0xFA

// ----- estado global -----
static int32_t  s_x = 0;
static int32_t  s_y = 0;
static uint32_t s_buttons = 0;
static uint64_t s_irq_count = 0;

// Maquina de estado da IRQ12 (cada IRQ entrega 1 byte).
static uint8_t  s_phase = 0;      // 0=esperando byte0, 1=esperando byte1, 2=esperando byte2
static uint8_t  s_pkt[3];

// ============================================================================
//  Helpers de espera para o controlador i8042. O i8042 e LENTO (sub-MHz); sem
//  isto a escrita do proximo comando atropela o byte anterior.
// ============================================================================

// Espera ate que o buffer de OUTPUT esteja cheio (tem byte para ler).
// timeout pequeno para nao travar caso o hardware nao responda.
static int wait_output_full(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(I8042_STATUS) & I8042_ST_OUTPUT_FULL) return 1;
    }
    return 0;
}

// Espera ate que o buffer de INPUT esteja vazio (podemos escrever).
static int wait_input_empty(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(I8042_STATUS) & I8042_ST_INPUT_FULL) == 0) return 1;
    }
    return 0;
}

// Le um byte do data port (depois de esperar OUTPUT_FULL).
static uint8_t read_data(void) {
    wait_output_full();
    return inb(I8042_DATA);
}

// Envia comando ao controlador (port 0x64).
static void send_cmd(uint8_t cmd) {
    wait_input_empty();
    outb(I8042_CMD, cmd);
}

// Envia byte ao controlador (port 0x60).
static void send_data(uint8_t data) {
    wait_input_empty();
    outb(I8042_DATA, data);
}

// Envia comando ao MOUSE (prefixa com 0xD4) e devolve o ACK (0xFA esperado).
static uint8_t mouse_send(uint8_t cmd) {
    send_cmd(I8042_CMD_WRITE_AUX);
    send_data(cmd);
    return read_data();
}

// ============================================================================
//  Desmascarar o IRQ12 — REGIME-AWARE (APIC vs PIC legado).
//
//  Regime APIC (o nosso, apos apic_init): o 8259 esta DESLIGADO (pic_disable
//  escreveu 0xFF/0xFF). Cutucar 0x21/0xA1 seria no vazio — era O BUG: o IRQ12
//  nunca chegava a CPU. No IO-APIC, o "unmask" e o mask bit (bit 16) da
//  redirection entry do IRQ12 — e essa entry JA foi programada UNMASKED por
//  apic_init (ioapic_set_irq(12, APIC_VECTOR_MOUSE, ...)). Logo, no regime APIC
//  nao ha nada a desmascarar aqui: o roteamento+unmask sao donos do HAL/apic_init
//  (modelo NT: o driver "conecta" a IRQ; o HAL programa o IO-APIC).
//
//  Regime PIC (legado, sem APIC ativo): mantem o caminho antigo — desmascara
//  IRQ12 no slave (bit 4 de 0xA1) + IRQ2 cascade no master (bit 2 de 0x21).
// ============================================================================
static void unmask_irq12(void) {
    if (apic_active()) {
        // IO-APIC ja roteia+desmascara o IRQ12 (apic_init). 8259 morto: nada a fazer.
        return;
    }
    // Master (0x21): mantem o que estava e LIMPA bit 2 (IRQ2 = cascade slave).
    uint8_t m = inb(0x21);
    m &= ~(uint8_t)(1 << 2);   // habilita IRQ2 (cascade)
    outb(0x21, m);
    // Slave (0xA1): mantem o que estava e LIMPA bit 4 (IRQ12 = mouse).
    uint8_t s = inb(0xA1);
    s &= ~(uint8_t)(1 << 4);   // habilita IRQ12
    outb(0xA1, s);
}

// ============================================================================
//  mouse_init — sequencia padrao de init do PS/2 mouse no i8042.
// ============================================================================
// Drena bytes pendentes do output buffer do i8042. Necessario porque um
// teclado/mouse "antigo" pode ter deixado bytes la antes do init do MeuOS.
static void i8042_drain(void) {
    for (int i = 0; i < 32; i++) {
        if ((inb(I8042_STATUS) & I8042_ST_OUTPUT_FULL) == 0) return;
        (void)inb(I8042_DATA);
    }
}

void mouse_init(void) {
    // 0) Drena bytes pendentes no output buffer do i8042 (do teclado ou
    //    mouse, deixados antes do nosso init).
    i8042_drain();

    // 1) Enable aux device (libera a porta do mouse no controlador).
    send_cmd(I8042_CMD_ENABLE_AUX);

    // 2) Le o config byte, liga bit1 (Aux Interrupt Enable) e desliga bit5
    //    (Aux Clock Disable). Sem isto a IRQ12 nunca chega ao CPU mesmo
    //    com o slave PIC desmascarado.
    send_cmd(I8042_CMD_READ_CONFIG);
    uint8_t cfg_before = read_data();
    uint8_t cfg = cfg_before;
    cfg |=  (1 << 1);   // bit1: Enable Aux Interrupt (IRQ12)
    cfg &= ~(1 << 5);   // bit5: Aux Clock — 0 = habilitado
    send_cmd(I8042_CMD_WRITE_CONFIG);
    send_data(cfg);

    // 3) Set defaults no mouse: sample rate 100, resolution 4, scaling 1:1.
    uint8_t a1 = mouse_send(MOUSE_CMD_SET_DEFAULTS);
    // 4) Enable stream mode (o mouse passa a enviar pacotes via IRQ12).
    uint8_t a2 = mouse_send(MOUSE_CMD_ENABLE_STREAM);

    // 5) Desmascara IRQ12 no PIC (slave + cascade no master).
    unmask_irq12();

    // 6) Posicao inicial do cursor: centro da tela. Pegamos a resolucao do
    //    win32k (que ja conhece o backend ativo: GPU LFB ou mode13h).
    s_x = win32k_screen_width()  / 2;
    s_y = win32k_screen_height() / 2;
    s_buttons = 0;
    s_phase = 0;
    s_irq_count = 0;

    kputs("[mouse] PS/2 init: aux enabled, IRQ12 unmasked, stream OK"
          " (set_defaults ack="); kput_hex(a1);
    kputs(", enable_stream ack="); kput_hex(a2);
    kputs(", cfg "); kput_hex(cfg_before); kputs("->"); kput_hex(cfg);
    kputs(", cursor inicial=("); kput_dec((uint64_t)s_x); kputc(',');
    kput_dec((uint64_t)s_y); kputs("))\n");
}

// ============================================================================
//  mouse_irq — tratador da IRQ12. Cada chamada le UM byte do port 0x60 e
//  agrega no pacote de 3 bytes. Pacote completo -> decodifica e posta evento.
// ============================================================================
void mouse_irq(void) {
    // Confirma que o byte disponivel veio do mouse (AUX_DATA setado). Em
    // alguns casos a IRQ12 pode disparar sem dado aux pronto; nesse caso
    // saimos sem ler para nao desincronizar a maquina de estado.
    uint8_t st = inb(I8042_STATUS);
    if ((st & I8042_ST_OUTPUT_FULL) == 0) return;
    if ((st & I8042_ST_AUX_DATA)    == 0) {
        // Byte e do teclado; deixa o handler IRQ1 cuidar (nao consome).
        return;
    }

    uint8_t b = inb(I8042_DATA);
    s_pkt[s_phase] = b;

    // Sync check no byte 0: bit3 DEVE ser 1. Se nao for, o stream esta
    // desalinhado — descartamos e ficamos em phase=0.
    if (s_phase == 0 && (b & 0x08) == 0) {
        // Desalinhado; tenta resync no proximo byte.
        return;
    }

    s_phase++;
    if (s_phase < 3) return;     // ainda nao temos o pacote completo

    // ----- Pacote completo: decodifica -----
    s_phase = 0;
    uint8_t b0 = s_pkt[0];
    int16_t dx =  (int16_t)(uint16_t)s_pkt[1];
    int16_t dy =  (int16_t)(uint16_t)s_pkt[2];

    // Estende sinal a partir dos bits 4/5 do byte 0.
    if (b0 & 0x10) dx |= (int16_t)0xFF00;   // X_sign
    if (b0 & 0x20) dy |= (int16_t)0xFF00;   // Y_sign

    // Descarta pacote com overflow (bits 6/7 do byte 0) — dado nao confiavel.
    if (b0 & 0xC0) {
        s_irq_count++;
        return;
    }

    // PS/2 reporta Y positivo para CIMA; tela tem Y crescente para BAIXO.
    // Invertemos para a coordenada da tela ficar natural.
    int32_t delta_x = (int32_t)dx;
    int32_t delta_y = -(int32_t)dy;

    uint32_t btns = 0;
    if (b0 & 0x01) btns |= MOUSE_BTN_LEFT;
    if (b0 & 0x02) btns |= MOUSE_BTN_RIGHT;
    if (b0 & 0x04) btns |= MOUSE_BTN_MIDDLE;

    s_buttons = btns;
    s_irq_count++;

    // Loga apenas algumas IRQs (cada 1: util pro headless; o mouse fica
    // quieto sem movimento, entao nao floodaria).
    kputs("[mouse] IRQ12 #"); kput_dec(s_irq_count);
    kputs(": dx="); if (delta_x < 0) kputc('-');
    kput_dec((uint64_t)(delta_x < 0 ? -delta_x : delta_x));
    kputs(" dy="); if (delta_y < 0) kputc('-');
    kput_dec((uint64_t)(delta_y < 0 ? -delta_y : delta_y));
    kputs(" buttons="); kput_hex((uint64_t)btns); kputc('\n');

    // Posta o evento ao win32k. Ele atualiza a posicao do cursor (clamp),
    // faz hit-test e roteia WM_MOUSE* para a janela alvo.
    win32k_on_mouse_event(delta_x, delta_y, btns);

    // Espelha aqui a posicao final do cursor para que NtUserGetCursorPos
    // (que pode ser chamada antes de o win32k publicar) tenha um valor.
    s_x = win32k_cursor_x();
    s_y = win32k_cursor_y();
}

// ============================================================================
//  Getters publicos.
// ============================================================================
int32_t  mouse_x(void)       { return s_x; }
int32_t  mouse_y(void)       { return s_y; }
uint32_t mouse_buttons(void) { return s_buttons; }
