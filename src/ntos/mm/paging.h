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

// Flags expostos para mm_map_phys_range (mesmos bits da arquitetura x86_64).
#define MM_FLAG_PRESENT  0x1ULL
#define MM_FLAG_RW       0x2ULL
#define MM_FLAG_USER     0x4ULL
#define MM_FLAG_PWT      0x8ULL   // page write-through (uncached recomendado p/ MMIO)
#define MM_FLAG_PCD      0x10ULL  // page cache disable
#define MM_FLAG_NX       (1ULL << 63)

// FASE GPU — Mapeia uma faixa fisica arbitraria [phys, phys+size) em virt
// (alinhamento por baixo em 4 KiB). Caminha PML4 -> PDPT -> PD -> PT criando
// tabelas sob demanda (ensure_table) e popula cada PTE para a pagina fisica
// correspondente com os 'flags' dados. 'flags' deve conter ao menos
// MM_FLAG_PRESENT|MM_FLAG_RW; para MMIO de dispositivo prefira tambem
// MM_FLAG_PCD (cache disable) para garantir ordem de escrita.
// Faz flush global do TLB ao final. Retorna 1 em sucesso, 0 em falha
// (sem RAM para as tabelas, range invalido, ou bater em paginas de 2 MiB ja
// mapeadas na faixa).
int mm_map_phys_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);

// Desmapeia [virt, virt+size). Caminha PML4 -> PDPT -> PD -> PT, zera os PTEs
// das paginas de 4 KiB cobertas e invalida o TLB de cada uma (INVLPG). Faixa
// que cair sobre uma pagina huge de 2 MiB (PS=1) e' recusada (devolve 0) pelo
// mesmo motivo de mm_map_phys_range — nao reformatamos sub-tabela aqui.
// Tabelas intermediarias NAO sao liberadas ao PMM (caminho seguro: outras
// paginas no mesmo PT podem ainda estar mapeadas). Retorna 1 em sucesso.
int mm_unmap_range(uint64_t virt, uint64_t size);

// MMIO arena (Pilar 1 — paginacao dinamica). Reserva 'size' bytes de espaco
// virtual KERNEL no arena MMIO e devolve o endereco virtual base alinhado em
// 4 KiB. A reserva NAO mapeia nada — chame mm_map_phys_range em cima do va
// retornado. Bump-allocator simples; sem reuso (caminho NT: MmAllocateMapping
// Address tambem nao tem reuso barato). Retorna 0 se o arena ficou cheio.
uint64_t mm_mmio_reserve(uint64_t size);

// Pre-aloca o PDPT do arena MMIO em PML4[arena_idx] no boot, ANTES de qualquer
// processo criar sua propria PML4 (mm_create_address_space copia entradas do
// PML4 por valor — entradas de PDPT sao por referencia, entao alteracoes
// abaixo do PML4 SAO compartilhadas; ja o PML4 nao). Idempotente.
void mm_mmio_init(void);

// ============================================================================
//  PF probe (prova de Pilar 1): faz um read de 4 bytes do VA e devolve 1 em
//  sucesso ou 0 se #PF disparou. Antes do load liga g_mm_pf_expect=1 e instala
//  g_mm_pf_resume_rip apontando para o label apos a instrucao; o handler de
//  #PF em isr.c, ao detectar g_mm_pf_expect, marca g_mm_pf_caught=1, reescreve
//  rip do trap frame para g_mm_pf_resume_rip e retorna — a CPU sai do IRETQ
//  em cima do label de resume e a execucao prossegue normalmente. Mecanismo
//  classico de "expected-trap" usado tambem em kernels reais (ex.: __ex_table
//  do Linux para acessos copy_from_user com PF tratado).
// ============================================================================
extern volatile int      g_mm_pf_expect;
extern volatile int      g_mm_pf_caught;
extern volatile uint64_t g_mm_pf_resume_rip;

int mm_probe_read_u32(volatile uint32_t* va, uint32_t* out_value);


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
