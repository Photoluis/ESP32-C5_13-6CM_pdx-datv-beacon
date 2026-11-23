/* ESP32-C5 Dual-Band DATV - IZ7PDX - GPL-3.0
 * Accesso al blocco RF dell'ESP32-C5: P2 = solo ricezione (motore di cattura "dump"), nessuna trasmissione. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define PDX_C5_SRAM_OWNER  0x60095004u   /* HP_SYSTEM_SRAM_USAGE_CONF_REG: [11:8] SRAM_USAGE, bit16 MAC_DUMP_ALLOC */
#define PDX_C5_DUMP_BASE   0x600A9000u   /* blocco del motore di cattura (MODEM0 + 0x9000) */
#define PDX_C5_BANK        ((volatile uint32_t *)0x40830000u)
#define PDX_C5_BANK_WORDS  16380u
#define PDX_C5_ANT_GPIO    26            /* commutatore d'antenna della ESP32-C5-Zero (ceramica / IPEX) */

esp_err_t pdx_rf_init(void);                  /* Wi-Fi in modo NULL: accende e calibra la PHY */
bool pdx_rf_tune(unsigned mhz);              /* = pdx_rf_tune_khz(mhz * 1000) */
/* frequenza VERA in kHz: il PLL si programma a f / (1 + ppm) con passo 1 kHz (phy_set_freq(MHz, kHz)) */
bool pdx_rf_tune_khz(uint32_t khz);
void pdx_rf_set_ppm(int32_t ppm_x100);       /* errore del quarzo della scheda in centesimi di ppm (+ = esce alta) */
uint32_t pdx_rf_prog_khz(void);              /* frequenza programmata nel PLL (dopo la correzione) */
bool pdx_rf_lend_bankB(void);            /* avvii senza TX: banco B al heap per l'access point */
bool pdx_rf_bankB_lent(void);
bool pdx_rf_dirty(void);                 /* la radio è stata manipolata (sintonia/TX): l'access point vuole un riavvio */          /* PLL sulla frequenza (MHz interi) */
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
/* ---- CW (23/11): portante non modulata e Morse OOK sulla frequenza del PLL, in un task (comando CW OFF) ----
 * text vuoto = portante fissa; altrimenti testo a wpm, poi carrier_s di portante, poi pause_s spento, in ciclo.
 * Si ferma con pdx_rf_cw_stop, BOOT/GPIO STOP o dopo max_s; done() è chiamata dal task alla fine. */
#define PDX_CW_AMP_MAX 480
#define PDX_CW_MAX_S 3600u
typedef struct { unsigned max_s, wpm, pause_s, carrier_s, dry; char text[48]; } pdx_cw_job_t;   /* dry: motore senza RF */
const char *pdx_rf_cw_start(const pdx_cw_job_t *job, void (*done)(const char *why));   /* NULL = partito */
void pdx_rf_cw_stop(void);
/* compensazione termica del quarzo (per ora applicata solo durante CW/Morse) */
void pdx_xc_update(float tc);                /* temperatura del chip in C (NaN = ignorata) */
int32_t pdx_xc_ppb(void);
void pdx_xc_set(bool on);
void pdx_xc_set_ref(float t);
void pdx_xc_set_k(float c1, float c2, float c3, float tau1, float tau2);
void pdx_xc_print(void);
void pdx_rf_cw_set_guard(unsigned g);      /* 0 = no, 1 = ripristino se i registri cambiano, 2 = force_txon ogni 100 ms */
void pdx_rf_cw_set_tlog(unsigned s);       /* temperatura nelle righe CWT ogni s secondi (0 = non leggere) */
void pdx_rf_cw_retune(uint32_t khz);         /* durante la CW: nuova frequenza vera (prova del PLL in TX) */
bool pdx_rf_cw_active(void);
void pdx_rf_cw_set_amp(int amp);           /* livello del DAC 1..480 (predefinito 300) */
int pdx_rf_cw_amp(void);
void pdx_rf_cw_set_key_pa(bool pa);        /* manipolazione: false = DAC (PA sempre accesa), true = anche PA spenta */
bool pdx_rf_cw_key_pa(void);
/* senza TX: la CPU legge/scrive il banco mentre è assegnato al motore? out[0] letto col motore, out[1] dopo */
void pdx_rf_owner_test(uint32_t out[4]);

/* ---- P2f: ping-pong tra due banchi (senza offset +64 KB): A = 0x40820000 (banco 1), B = 0x40840000 (banco 2) ----
 * A riempito con k=+16, B con k=-16 (n=1600 parole); 0x40830000 con k=+32 (diagnosi dell'indirizzo).
 * mode 0: proprietario sempre A; 1: sempre B; 2: alterna A/B ogni period_us. ALLOC = bit16 di SRAM_USAGE (0 o 1). */
#define PDX_PP_A ((volatile uint32_t *)0x40820000u)
#define PDX_PP_B ((volatile uint32_t *)0x40840000u)
typedef struct { uint32_t switches, ctrl_running, ctrl_end, owner_a, owner_b; } pdx_pp_res_t;
void pdx_rf_scan(uint32_t ctrlval, uint32_t idx_off, uint32_t dt, uint32_t **a, uint32_t **b, uint32_t **t);
/* P2h: tono di periodo l campioni generato in tempo reale nei due banchi (n parole, 40 MS/s), scambio a metà banco */
typedef struct { uint32_t intervals, fill_max, late_max, overruns, stopped; } pdx_stream_res_t;
/* P3b: DVB-S2 dai simboli della partizione "symbols" (333,3 kS, RRC 0,35), fermata col tasto BOOT (GPIO28) */
#define PDX_S2_MS_MAX 120000u
uint32_t pdx_rf_sym_open(uint32_t *sr, char label[41]);
void pdx_rf_s2play(int amp, unsigned ms, uint32_t lat, unsigned qinv, pdx_stream_res_t *r);
void pdx_rf_stream(uint32_t n, uint32_t l, int amp, unsigned ms, uint32_t lat, pdx_stream_res_t *r);
/* P3a: QPSK RRC 0,35 con simboli PRBS, SR = 40e6 / (15 * hold) (hold 4/8/16 -> 667/333/167 kS) */
void pdx_rf_qpsk(uint32_t n, uint32_t hold, int amp, unsigned ms, uint32_t lat, pdx_stream_res_t *r);
/* P4b: TS continuo generato sulla C5 dal monoscopio ES (partizione symbols + 0x180000), DVB-S2 QPSK 3/4 normal + pilot */
typedef struct {
    uint32_t frames, dummies, step_max, steps, null_pk, video_pk, audio_pk;
    uint32_t st_max[8], st_kcyc[8], st_n[8];   /* per fase del produttore */
    double ts_rate;
} pdx_live_stats_t;
/* dry = 1: prova a secco, motore del DAC in corsa ma senza force_txon (nessuna RF) */
const char *pdx_rf_s2live(uint32_t sr, int amp, unsigned ms, uint32_t lat, unsigned qinv, unsigned dry,
                          pdx_stream_res_t *r);
const char *pdx_rf_s2ltest(uint32_t nframes, uint32_t *ok, uint32_t *bad_sym, uint32_t *steps_max_cyc);
void pdx_rf_live_stats(pdx_live_stats_t *st);
/* GPIO STOP (a massa = ferma la TX come il tasto BOOT), -1 = solo BOOT */
void pdx_rf_set_stop_gpio(int g);
/* monoscopio (0 = 6 cm, 1 = 13 cm) e nomi della SDT per le prossime S2LIVE */
void pdx_rf_live_source(int es, const char *service, const char *provider);
/* testi della sovrimpressione del monoscopio neutro (PDXES002): nominativo e frequenza ("" = dalla sintonia) */
void pdx_rf_live_overlay(const char *call, const char *freq);
/* prova senza TX: compone in flash le strisce del monoscopio k e stampa lunghezze e CRC32 */
const char *pdx_rf_ovl_test(int k, const char *call, const char *freq);
uint32_t pdx_rf_oneshot_cycles(uint32_t ctrlval, uint32_t max_cycles, uint32_t *ctrl_end);
void pdx_rf_pp_init(void);   /* crea il task con lo stack statico nel banco 0 */
void pdx_rf_pp(unsigned mode, unsigned ms, unsigned period_us, unsigned alloc, pdx_pp_res_t *r);
