/* ESP32-C5 Dual-Band DATV - IZ7PDX - GPL-3.0
 * Accesso al blocco RF dell'ESP32-C5: P2 = solo ricezione (motore di cattura "dump"), nessuna trasmissione. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PDX_C5_SRAM_OWNER  0x60095004u   /* HP_SYSTEM_SRAM_USAGE_CONF_REG: [11:8] SRAM_USAGE, bit16 MAC_DUMP_ALLOC */
#define PDX_C5_DUMP_BASE   0x600A9000u   /* blocco del motore di cattura (MODEM0 + 0x9000) */
#define PDX_C5_BANK        ((volatile uint32_t *)0x40830000u)
#define PDX_C5_BANK_WORDS  16380u
#define PDX_C5_ANT_GPIO    26            /* commutatore d'antenna della ESP32-C5-Zero (ceramica / IPEX) */

void pdx_rf_init(void);                  /* Wi-Fi in modo NULL: accende e calibra la PHY */
bool pdx_rf_tune(unsigned mhz);          /* PLL sulla frequenza (MHz interi) */
unsigned pdx_rf_mhz(void);
/* cattura n parole (coppie I/Q) nel banco; div = divisore della frequenza di campionamento (0 = massima);
 * restituisce il numero di parole scritte (sentinella sovrascritta) e la durata in us */
unsigned pdx_rf_capture(unsigned n, unsigned div, uint32_t *us);

/* ---- P2c: prove di trasmissione (solo con TXARM, impulsi <= 2 s) ---- */
#define PDX_TX_MS_MAX 2000u
#define PDX_DAC_AMP_MAX 300
/* parola DAC come S3/C3 (da confermare sul C5): I bit 0..9, Q bit 10..19, complemento a due */
static inline uint32_t pdx_dac_word(int i, int q) { return ((uint32_t)i & 0x3FFu) | (((uint32_t)q & 0x3FFu) << 10); }
/* portante di riferimento: generatore di tono della libreria (esp_phy_wifi_tx_tone), backoff in 0,25 dB */
void pdx_rf_ref_tone(unsigned backoff, unsigned ms);
/* prova del replay: registro di controllo candidato (DUMP_BASE + off), valore di avvio val (bit31 aggiunto),
 * parola I=amp (tone_khz = 0) oppure tono complesso a +tone_khz scritto dalla CPU a fs_khz. Riporta le riletture. */
typedef struct { uint32_t owner_before, ctrl_before, ctrl_running, ctrl_after, samples, dump4, dump8; } pdx_dacp_res_t;
void pdx_rf_dac_probe(uint32_t off, uint32_t val, int amp, int tone_khz, unsigned fs_khz, unsigned ms, pdx_dacp_res_t *r);
/* banco precaricato con n parole = k periodi di un tono complesso (k > 0: sopra l'LO), poi replay come sopra */
void pdx_rf_dac_pattern(uint32_t off, uint32_t val, int amp, unsigned n, int k, unsigned ms, pdx_dacp_res_t *r);
/* senza TX: la CPU legge/scrive il banco mentre è assegnato al motore? out[0] letto col motore, out[1] dopo */
void pdx_rf_owner_test(uint32_t out[4]);

/* ---- P2f: ping-pong tra due banchi (senza offset +64 KB): A = 0x40820000 (banco 1), B = 0x40840000 (banco 2) ----
 * A riempito con k=+16, B con k=-16 (n=1600 parole); 0x40830000 con k=+32 (diagnosi dell'indirizzo).
 * mode 0: proprietario sempre A; 1: sempre B; 2: alterna A/B ogni period_us. ALLOC = bit16 di SRAM_USAGE (0 o 1). */
#define PDX_PP_A ((volatile uint32_t *)0x40820000u)
#define PDX_PP_B ((volatile uint32_t *)0x40840000u)
typedef struct { uint32_t switches, ctrl_running, ctrl_end, owner_a, owner_b; } pdx_pp_res_t;
void pdx_rf_scan(uint32_t ctrlval, uint32_t idx_off, uint32_t dt, uint32_t **a, uint32_t **b, uint32_t **t);
void pdx_rf_pp_init(void);   /* crea il task con lo stack statico nel banco 0 */
void pdx_rf_pp(unsigned mode, unsigned ms, unsigned period_us, unsigned alloc, pdx_pp_res_t *r);
