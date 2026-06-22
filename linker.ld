/* Script de ligacao do kernel.
   Carregado em 1 MiB (0x100000), ELF64 lower-half (identidade).
   Geramos tambem um binario plano (kernel.bin) para o "-kernel" do QEMU,
   por isso exportamos os simbolos de carga usados no cabecalho Multiboot. */
ENTRY(_start)

SECTIONS
{
    . = 1M;
    __load_start = .;

    /* O cabecalho Multiboot precisa ficar no inicio da imagem. */
    .multiboot : { KEEP(*(.multiboot)) }

    .text   : { *(.text*)   }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*)   }

    __load_end = .;            /* fim do conteudo carregado (antes do .bss) */

    .bss    : { *(.bss*) *(COMMON) }

    __bss_end = .;             /* fim da memoria a zerar */

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
