#pragma once
#include <stdint.h>

// FASE 7.1 — KUSER_SHARED_DATA em 0xFFFFF78000000000 (drivers leem direto).
void mm_map_kuser_shared_data(void);
void mm_kuser_tick(void);

// FASE 7.9 (SEH minimo) — mapeia 1 pagina (4 KiB) ZERADA em "va" (alinha por
// baixo a multiplo de 4 KiB). Usado pelo handler de page-fault como caminho de
// recuperacao quando um driver acessa endereco nao mapeado fora do range do
// kernel. Retorna 1 = sucesso (pagina presente apos a chamada), 0 = falha
// (sem RAM ou parametro invalido). Faz flush local do TLB no fim.
int mm_map_zero_page(uint64_t va);


// Marca o intervalo [addr, addr+size) como acessivel em modo usuario (ring 3),
// setando o bit U/S na cadeia de paginas (PML4[0], PDPT[0] e as paginas de 2 MiB).
void mm_map_user(uint64_t addr, uint64_t size);

// Cria um espaco de enderecamento NOVO para um processo: aloca uma PML4/PDPT/PD
// proprias e COPIA os mapeamentos do kernel (identidade de 1 GiB, com os bits
// U/S ja setados). Retorna o endereco FISICO da nova PML4 (para carregar em CR3),
// ou 0 se nao houver RAM (o chamador entao continua no espaco compartilhado).
uint64_t mm_create_address_space(void);

// Le/escreve o CR3 (raiz da tabela de paginas do processo corrente).
uint64_t mm_get_cr3(void);
void     mm_switch_cr3(uint64_t cr3);   // troca de espaco de enderecamento
