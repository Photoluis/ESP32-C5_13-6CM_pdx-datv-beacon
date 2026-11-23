/* ESP32-C5 Dual-Band DATV - IZ7PDX - GPL-3.0
 *
 * Ricezione con il motore di cattura della radio dell'ESP32-C5 (P2b, nessuna trasmissione).
 * Mappa dei registri ricavata dal disassemblato di librftest.a/libphy.a (ESP-IDF v6.1, esp32c5) e confrontata con il
 * progetto ESP-SDR di ESPARGOS (GPL-3.0, main/targets/esp32c5): adctrig(n-1, 0, 0, div*2, 0, 0, 0, 0, 0) con
 * sorgente 0 e senza tono, banco 0x40830000, proprietario 0x60095004 = 0x10200 durante la cattura.
 * Codice scritto da zero.
 */
#include "rf_c5.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_event.h"
#include "esp_heap_caps_init.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "heap_memory_layout.h"
#include "nvs_flash.h"

/* tutto il banco 1 (128 KB) va al motore radio durante la cattura: niente heap dentro */
SOC_RESERVE_MEMORY_REGION(0x40820000, 0x40840000, c5_rf_dump);

#define SENTINEL 0xA5A0055Au

extern void adctrig(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
extern void phy_set_chanfreq(unsigned mhz, unsigned mode);
extern void phy_set_freq(unsigned mhz, int khz);   /* phy_param+30 = kHz (stringa della libreria "freq: %d %dkhz %d") */
extern void phy_stop_tx_tone(unsigned);
extern void phy_pbus_workmode(void);
extern void phy_pbus_xpd_rx_on(unsigned);
extern void phy_pbus_xpd_tx_off(void);
extern void phy_set_rxclk_en(unsigned);

/* richiesta da librftest.a: nessuna shell dei test RF */
int cmd_parse(char *cmd, char *name, int *argc, char **argv)
{
    (void)cmd; (void)name; (void)argc; (void)argv;
    return -1;
}

static unsigned cur_mhz = 2412;
static uint32_t cur_khz = 2412000u, prog_khz = 2412000u;
static int32_t xtal_ppm_x100;

static inline uint32_t rd(uint32_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* 20/11: niente ESP_ERROR_CHECK qui. Un errore all'avvio (heap esaurito) faceva riavviare la scheda in continuo e la
 * porta USB si bloccava: ora l'errore si stampa e il prompt resta raggiungibile per riprogrammare. */
#define TRY(x) do { esp_err_t _e = (x); if (_e != ESP_OK) { printf("ERR avvio RF: %s = %s\n", #x, esp_err_to_name(_e)); return _e; } } while (0)

esp_err_t pdx_rf_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        TRY(nvs_flash_erase());
        e = nvs_flash_init();
    }
    TRY(e);
    TRY(esp_netif_init());
    TRY(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&cfg));
    TRY(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    TRY(esp_wifi_set_mode(WIFI_MODE_NULL));   /* nessuna trama trasmessa dal driver */
    TRY(esp_wifi_start());
    TRY(esp_wifi_set_ps(WIFI_PS_NONE));
    TRY(esp_wifi_set_promiscuous(true));
    TRY(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    /* 21/11 (B2): niente sintonia né cattura qui. L'access point della dashboard non funziona con la radio manipolata
     * (come sul beacon S3): la preparazione RF si fa alla prima pdx_rf_tune, solo quando serve davvero */
    return ESP_OK;
}

static void rx_idle(void)
{
    phy_stop_tx_tone(1);
    phy_pbus_workmode();
    phy_pbus_xpd_tx_off();
    phy_pbus_xpd_rx_on(1);
    phy_set_rxclk_en(1);
}

/* 21/11 (B2b): negli avvii senza TX (configurazione, pausa con la dashboard) il banco B del modulatore (48 KB) va al
 * heap: l'access point ha bisogno di RAM con DMA per i pacchetti TCP (con 2,8 KB liberi la pagina non arrivava).
 * Il prestito vale fino al riavvio: in questo avvio i comandi che usano il banco B vengono rifiutati. */
static bool bankB_lent;
bool pdx_rf_lend_bankB(void)
{
    if (bankB_lent) return true;
    if (heap_caps_add_region(0x40840000, 0x4084C000) != ESP_OK) return false;
    bankB_lent = true;
    printf("RF banco B (48 KB) prestato al heap fino al prossimo riavvio\n");
    return true;
}
bool pdx_rf_bankB_lent(void) { return bankB_lent; }
#define BANKB_GUARD(ret) do { if (bankB_lent) { printf("ERR banco B prestato al heap (dashboard): per trasmettere usare BEACON o START (riavvio)\n"); ret; } } while (0)

static bool rf_hw_ready;                     /* cattura di preparazione già fatta (blocco dump configurato) */
bool pdx_rf_dirty(void) { return rf_hw_ready; }

void pdx_rf_set_ppm(int32_t ppm_x100) { xtal_ppm_x100 = ppm_x100; }
uint32_t pdx_rf_prog_khz(void) { return prog_khz; }

bool pdx_rf_tune(unsigned mhz) { return pdx_rf_tune_khz(mhz * 1000u); }

bool pdx_rf_tune_khz(uint32_t khz)
{
    if (khz < 2300000u || (khz > 2500000u && khz < 4900000u) || khz > 5900000u) return false;
    const uint32_t pk = (uint32_t)((double)khz / (1.0 + (double)xtal_ppm_x100 * 1e-8) + 0.5);
    phy_set_freq(pk / 1000u, (int)(pk % 1000u));
    rx_idle();
    cur_khz = khz;
    prog_khz = pk;
    cur_mhz = khz / 1000u;
    if (!rf_hw_ready) {                          /* prima sintonia: cattura breve che configura il blocco dump */
        rf_hw_ready = true;
        uint32_t us;
        pdx_rf_capture(256, 0, &us);
    }
    return true;
}

unsigned pdx_rf_mhz(void) { return cur_mhz; }

/* ------------------------------------------------------------------ P2c: trasmissione di prova */
#include <math.h>
#include <string.h>
#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_heap_caps_init.h"
#include "esp_phy_cert_test.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern void force_txon(int on);

/* 21/11: il replay del DAC esce solo con il campo sorgente di 0x600A9008 (bit 17..20) = 0xF, che finora impostava di
 * nascosto la cattura RX (adctrig). Sopravvive ai reset software ma non allo spegnimento: si imposta a ogni TX. */
static bool tx_dry;                          /* prova a secco: motore del DAC in corsa ma senza force_txon (niente RF) */
static void dump_src_cfg(void) { wr(PDX_C5_DUMP_BASE + 8u, (rd(PDX_C5_DUMP_BASE + 8u) & ~0x1E0000u) | 0x1E0000u); }
static void tx_on(void) { dump_src_cfg(); if (!tx_dry) force_txon(1); }
static void tx_off(void) { if (!tx_dry) force_txon(0); }
extern void phy_txcal_work_mode(void);

void pdx_rf_ref_tone(unsigned backoff, unsigned ms)
{
    if (ms > PDX_TX_MS_MAX) ms = PDX_TX_MS_MAX;
    const unsigned mhz = cur_mhz;
    esp_phy_wifi_tx_tone(1, 1, backoff);          /* tono sul canale 1 ... */
    if (mhz != 2412) phy_set_chanfreq(mhz, 0);    /* ... poi PLL spostato sulla frequenza scelta */
    vTaskDelay(pdMS_TO_TICKS(ms));
    esp_phy_wifi_tx_tone(0, 1, 0);
    pdx_rf_tune(mhz);
}

#define TONE_LUT 256u
static uint32_t shared_tab[128];            /* tono di DACP (256 x int16) o tabella di STREAM (2 x 64 parole): mai insieme */
#define tone_cos ((int16_t *)shared_tab)

static IRAM_ATTR __attribute__((noinline)) uint32_t dac_tone_loop(uint32_t step, uint32_t period, uint32_t n)
{
    uint32_t ph = 0, s;
    uint32_t tn = esp_cpu_get_cycle_count() + 1000u;
    for (s = 0; s < n; s++) {
        const uint32_t idx = ph >> 24;
        const uint32_t w = pdx_dac_word(tone_cos[idx], tone_cos[(idx - TONE_LUT / 4u) & (TONE_LUT - 1u)]);
        ph += step;
        while ((int32_t)(esp_cpu_get_cycle_count() - tn) < 0) {}
        PDX_C5_BANK[0] = w;
        tn += period;
    }
    return s;
}

void pdx_rf_dac_probe(uint32_t off, uint32_t val, int amp, int tone_khz, unsigned fs_khz, unsigned ms, pdx_dacp_res_t *r)
{
    if (ms > PDX_TX_MS_MAX) ms = PDX_TX_MS_MAX;
    if (amp > PDX_DAC_AMP_MAX) amp = PDX_DAC_AMP_MAX;
    if (amp < -PDX_DAC_AMP_MAX) amp = -PDX_DAC_AMP_MAX;
    if (fs_khz < 100 || fs_khz > 4000) fs_khz = 1000;
    const uint32_t ctrl = PDX_C5_DUMP_BASE + (off & 0xFFCu);
    for (unsigned k = 0; k < TONE_LUT; k++) tone_cos[k] = (int16_t)lroundf(amp * cosf(6.2831853f * k / TONE_LUT));
    const uint32_t w0 = pdx_dac_word(amp, 0);
    for (unsigned j = 0; j < 16; j++) PDX_C5_BANK[j] = w0;

    r->owner_before = rd(PDX_C5_SRAM_OWNER);
    r->ctrl_before = rd(ctrl);
    tx_on();
    esp_rom_delay_us(3000);
    wr(PDX_C5_SRAM_OWNER, (r->owner_before & ~0xF00u) | 0x200u | 0x10000u);   /* banco 1 + 64 KB -> 0x40830000 */
    wr(ctrl, r->ctrl_before & ~(1u << 31));
    wr(ctrl, val & ~(1u << 31));
    wr(ctrl, val | (1u << 31));
    esp_rom_delay_us(50);
    r->ctrl_running = rd(ctrl);
    r->dump4 = rd(PDX_C5_DUMP_BASE + 4);
    r->dump8 = rd(PDX_C5_DUMP_BASE + 8);
    r->samples = 0;
    if (tone_khz) {
        const uint32_t step = (uint32_t)(int32_t)llround((double)tone_khz * 4294967296.0 / fs_khz);
        const uint32_t period = 240000u / fs_khz;
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        taskENTER_CRITICAL(&mux);                      /* un solo core: interrupt mascherati durante il tono */
        r->samples = dac_tone_loop(step, period, ms * fs_khz);
        taskEXIT_CRITICAL(&mux);
    } else {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
    for (unsigned j = 0; j < 16; j++) PDX_C5_BANK[j] = 0;
    wr(ctrl, rd(ctrl) & ~(1u << 31));
    wr(ctrl, r->ctrl_before);
    r->ctrl_after = rd(ctrl);
    wr(PDX_C5_SRAM_OWNER, r->owner_before);
    tx_off();
    phy_txcal_work_mode();
    pdx_rf_tune_khz(cur_khz);
}

static void replay_run(uint32_t off, uint32_t val, unsigned ms, pdx_dacp_res_t *r)
{
    const uint32_t ctrl = PDX_C5_DUMP_BASE + (off & 0xFFCu);
    r->owner_before = rd(PDX_C5_SRAM_OWNER);
    r->ctrl_before = rd(ctrl);
    tx_on();
    esp_rom_delay_us(3000);
    wr(PDX_C5_SRAM_OWNER, (r->owner_before & ~0xF00u) | 0x200u | 0x10000u);
    wr(ctrl, r->ctrl_before & ~(1u << 31));
    wr(ctrl, val & ~(1u << 31));
    wr(ctrl, val | (1u << 31));
    esp_rom_delay_us(50);
    r->ctrl_running = rd(ctrl);
    r->dump4 = rd(PDX_C5_DUMP_BASE + 4);
    r->dump8 = rd(PDX_C5_DUMP_BASE + 8);
    r->samples = 0;
    vTaskDelay(pdMS_TO_TICKS(ms));
    wr(ctrl, rd(ctrl) & ~(1u << 31));
    wr(ctrl, r->ctrl_before);
    r->ctrl_after = rd(ctrl);
    wr(PDX_C5_SRAM_OWNER, r->owner_before);
    tx_off();
    phy_txcal_work_mode();
    pdx_rf_tune_khz(cur_khz);
}

void pdx_rf_dac_pattern(uint32_t off, uint32_t val, int amp, unsigned n, int k, unsigned ms, pdx_dacp_res_t *r)
{
    if (ms > PDX_TX_MS_MAX) ms = PDX_TX_MS_MAX;
    if (amp > PDX_DAC_AMP_MAX) amp = PDX_DAC_AMP_MAX;
    if (n < 1) n = 1;
    if (n > PDX_C5_BANK_WORDS) n = PDX_C5_BANK_WORDS;
    for (unsigned j = 0; j < n; j++) {
        const float ph = 6.2831853f * (float)k * (float)j / (float)n;
        PDX_C5_BANK[j] = pdx_dac_word((int)lroundf(amp * cosf(ph)), (int)lroundf(amp * sinf(ph)));
    }
    for (unsigned j = n; j < n + 4 && j < PDX_C5_BANK_WORDS; j++) PDX_C5_BANK[j] = 0;
    replay_run(off, val, ms, r);
    for (unsigned j = 0; j < n; j++) PDX_C5_BANK[j] = 0;
}
void pdx_rf_owner_test(uint32_t out[4])
{
    const uint32_t owner = rd(PDX_C5_SRAM_OWNER);
    PDX_C5_BANK[0] = 0x11111111u;
    wr(PDX_C5_SRAM_OWNER, (owner & ~0xF00u) | 0x200u | 0x10000u);
    out[2] = PDX_C5_BANK[0];          /* lettura col banco al motore (prima di scrivere) */
    PDX_C5_BANK[0] = 0x22222222u;     /* scrittura col banco al motore */
    out[0] = PDX_C5_BANK[0];
    wr(PDX_C5_SRAM_OWNER, owner);
    out[1] = PDX_C5_BANK[0];          /* 0x22222222 = scrittura arrivata, 0x11111111 = persa */
    out[3] = rd(PDX_C5_SRAM_OWNER);
    PDX_C5_BANK[0] = 0;
}

/* ------------------------------------------------------------------ P2g: ricerca del contatore (senza TX) */
#define SCAN_N 16u
static uint32_t scan_a[SCAN_N], scan_b[SCAN_N], scan_t[16];

/* replay avviato SENZA force_txon: legge due volte 0x600A9000.. e 16 volte il registro idx_off a intervalli di dt cicli */
void pdx_rf_scan(uint32_t ctrlval, uint32_t idx_off, uint32_t dt, uint32_t **a, uint32_t **b, uint32_t **t)
{
    const uint32_t ctrl = PDX_C5_DUMP_BASE + 0x0Cu;
    const uint32_t owner0 = rd(PDX_C5_SRAM_OWNER), ctrl0 = rd(ctrl);
    for (unsigned j = 0; j < 1600; j++) PDX_C5_BANK[j] = 0;
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    for (unsigned i = 0; i < SCAN_N; i++) scan_a[i] = rd(PDX_C5_DUMP_BASE + 4u * i);   /* a riposo */
    wr(PDX_C5_SRAM_OWNER, (owner0 & ~0x10F00u) | 0x200u);
    wr(ctrl, ctrl0 & ~(1u << 31));
    wr(ctrl, ctrlval & ~(1u << 31));
    wr(ctrl, ctrlval | (1u << 31));
    for (volatile int d = 0; d < 200; d++) {}
    for (unsigned i = 0; i < SCAN_N; i++) scan_b[i] = rd(PDX_C5_DUMP_BASE + 4u * i);   /* in corsa */
    for (unsigned i = 0; i < 16; i++) {
        const uint32_t c0 = esp_cpu_get_cycle_count();
        scan_t[i] = rd(PDX_C5_DUMP_BASE + (idx_off & 0x1FCu));
        while (esp_cpu_get_cycle_count() - c0 < dt) {}
    }
    wr(ctrl, rd(ctrl) & ~(1u << 31));
    wr(ctrl, ctrl0);
    wr(PDX_C5_SRAM_OWNER, owner0);
    taskEXIT_CRITICAL(&mux);
    *a = scan_a; *b = scan_b; *t = scan_t;
}

/* replay una volta (senza force_txon): cicli CPU dall'avvio al bit18 del ctrl; -1 se non arriva entro max_cycles */
uint32_t pdx_rf_oneshot_cycles(uint32_t ctrlval, uint32_t max_cycles, uint32_t *ctrl_end)
{
    const uint32_t ctrl = PDX_C5_DUMP_BASE + 0x0Cu;
    const uint32_t owner0 = rd(PDX_C5_SRAM_OWNER), ctrl0 = rd(ctrl);
    uint32_t c = 0xFFFFFFFFu, v = 0;
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    wr(PDX_C5_SRAM_OWNER, (owner0 & ~0x10F00u) | 0x200u);
    wr(ctrl, ctrl0 & ~(1u << 31));
    wr(ctrl, ctrlval & ~(1u << 31));
    const uint32_t t0 = esp_cpu_get_cycle_count();
    wr(ctrl, ctrlval | (1u << 31));
    for (;;) {
        v = rd(ctrl);
        const uint32_t e = esp_cpu_get_cycle_count() - t0;
        if (v & (1u << 18)) { c = e; break; }
        if (e > max_cycles) break;
    }
    wr(ctrl, rd(ctrl) & ~(1u << 31));
    wr(ctrl, ctrl0);
    wr(PDX_C5_SRAM_OWNER, owner0);
    taskEXIT_CRITICAL(&mux);
    *ctrl_end = v;
    return c;
}

/* ------------------------------------------------------------------ P2f: ping-pong */
SOC_RESERVE_MEMORY_REGION(0x40840000, 0x4084C000, c5_rf_bank2_lo);   /* banco B: 12000 parole (48 KB) */

typedef struct {
    unsigned mode, ms, period_us, alloc;
    pdx_pp_res_t *r;
    TaskHandle_t caller;
    uint32_t n, l, lat;
    int amp;
    pdx_stream_res_t *sres;
} pp_req_t;
static pp_req_t pp_req;
static TaskHandle_t pp_task;
static StaticTask_t pp_tcb;
static StackType_t pp_stack[3072];          /* .bss nel banco 0: mai dentro i banchi dati alla radio */

static void fill_tone(volatile uint32_t *dst, unsigned n, int k, int amp)
{
    for (unsigned j = 0; j < n; j++) {
        const float ph = 6.2831853f * (float)k * (float)j / (float)n;
        dst[j] = pdx_dac_word((int)lroundf(amp * cosf(ph)), (int)lroundf(amp * sinf(ph)));
    }
}

/* alternanza del proprietario: solo registri e contatore di cicli (niente ROM, niente stack fuori dal banco 0) */
static IRAM_ATTR __attribute__((noinline)) uint32_t pp_loop(uint32_t oa, uint32_t ob, uint32_t mode, uint32_t period,
                                                            uint32_t total)
{
    volatile uint32_t *own = (volatile uint32_t *)PDX_C5_SRAM_OWNER;
    const uint32_t t0 = esp_cpu_get_cycle_count();
    uint32_t tn = t0 + period, sw = 0, cur = (mode == 1) ? ob : oa;
    *own = cur;
    while ((uint32_t)(esp_cpu_get_cycle_count() - t0) < total) {
        if (mode >= 2 && (int32_t)(esp_cpu_get_cycle_count() - tn) >= 0) {
            cur = (cur == oa) ? ob : oa;
            *own = cur;
            tn += period;
            sw++;
        }
    }
    return sw;
}

/* ---- P2h: streaming in tempo reale con ping-pong a 40 MS/s ----
 * Intervallo j (j >= -1) = campioni [jN + N/2, (j+1)N + N/2) del flusso, banco B se j pari, A se j dispari.
 * Contenuto del banco dell'intervallo j: indice i < N/2 -> campione (j+1)N + i; i >= N/2 -> campione jN + i.
 * Il flusso è un tono di periodo L campioni (tabella stream_tab), quindi campione s -> stream_tab[s mod L]. */
#define STREAM_L_MAX 64u
#define stream_tab shared_tab                       /* 2 * STREAM_L_MAX = 128 parole */
static pdx_stream_res_t stream_res;
static uint32_t stop_mask = 1u << 28;             /* ingressi che fermano la TX a livello basso: BOOT + GPIO STOP */
void pdx_rf_set_stop_gpio(int g) { stop_mask = (1u << 28) | (g >= 0 && g < 28 ? 1u << g : 0u); }

/* ---- CW (23/11): portante non modulata e manipolazione Morse (OOK) ----
 * Portante = replay in ciclo di CW_WORDS parole costanti (I = amp, Q = 0): riga esattamente sulla frequenza del PLL.
 * Il lavoro sta in un task (stack nel heap, fuori dal banco 1 che va al motore): la seriale resta libera per CW OFF.
 * Manipolazione: DAC = replay acceso/spento con la PA sempre accesa (resta la riga residua dell'LO, ~-28 dB);
 *                PA  = anche force_txon spento nelle pause (spegnimento completo). */
#define CW_WORDS 64u
#define CW_VAL (0x80000u | CW_WORDS)                 /* bit19 = ciclo, [13:0] = lunghezza */
static volatile bool cw_stop_req, cw_running;
static uint8_t cw_key_pa = 1;                 /* 23/11 in RF: PA = spento completo, stessa frequenza e livello a ogni tasto */
static int cw_amp = 300;
static uint32_t cw_owner0, cw_ctrl0;
static pdx_cw_job_t *cw_job;
static void (*cw_done)(const char *why);

static void cw_dac(bool on)
{
    const uint32_t ctrl = PDX_C5_DUMP_BASE + 0x0Cu;
    if (on) {
        wr(PDX_C5_SRAM_OWNER, (cw_owner0 & ~0xF00u) | 0x200u | 0x10000u);   /* banco 1 + 64 KB -> 0x40830000 */
        wr(ctrl, cw_ctrl0 & ~(1u << 31));
        wr(ctrl, CW_VAL);
        wr(ctrl, CW_VAL | (1u << 31));
    } else {
        wr(ctrl, rd(ctrl) & ~(1u << 31));
        wr(ctrl, cw_ctrl0);
        wr(PDX_C5_SRAM_OWNER, cw_owner0);
    }
}

static bool cw_key_state;
static void cw_key(bool on)
{
    if (on == cw_key_state) return;
    cw_key_state = on;
    if (cw_key_pa) {
        if (on) { tx_on(); esp_rom_delay_us(3000); cw_dac(true); }
        else { cw_dac(false); tx_off(); }
    } else cw_dac(on);
}

static const char *cw_why;
static int64_t cw_deadline;
static bool cw_check(void)                           /* false = fermare */
{
    if (cw_stop_req) { cw_why = "CW OFF"; return false; }
    if (~REG_READ(GPIO_IN_REG) & stop_mask) { cw_why = "BOOT o GPIO STOP"; return false; }
    if (esp_timer_get_time() > cw_deadline) { cw_why = "tempo massimo"; return false; }
    return true;
}

static bool cw_wait(TickType_t *t, uint32_t ms)      /* attesa a passi di 10 ms con i controlli */
{
    while (ms) {
        const uint32_t d = ms > 10u ? 10u : ms;
        vTaskDelayUntil(t, pdMS_TO_TICKS(d));
        ms -= d;
        if (!cw_check()) return false;
    }
    return true;
}

static const char *morse_of(char c)
{
    static const char *az[26] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..",
                                 "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-",
                                 "-.--", "--.."};
    static const char *d09[10] = {"-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..",
                                  "----."};
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return az[c - 'A'];
    if (c >= '0' && c <= '9') return d09[c - '0'];
    switch (c) {
    case '/': return "-..-.";
    case '?': return "..--..";
    case '.': return ".-.-.-";
    case ',': return "--..--";
    case '=': return "-...-";
    case '-': return "-....-";
    case '+': return ".-.-.";
    default: return NULL;
    }
}

static bool cw_send_text(TickType_t *t, const char *s, uint32_t dot)
{
    for (; *s; s++) {
        if (*s == ' ') { if (!cw_wait(t, 4u * dot)) return false; continue; }   /* 7 punti con i 3 di fine carattere */
        const char *m = morse_of(*s);
        if (!m) continue;
        for (; *m; m++) {
            cw_key(true);
            if (!cw_wait(t, *m == '-' ? 3u * dot : dot)) return false;
            cw_key(false);
            if (!cw_wait(t, dot)) return false;
        }
        if (!cw_wait(t, 2u * dot)) return false;                                /* 3 punti tra i caratteri */
    }
    return true;
}

static void cw_task_fn(void *arg)
{
    pdx_cw_job_t *j = (pdx_cw_job_t *)arg;
    const uint32_t dot = j->wpm ? 1200u / j->wpm : 60u;
    TickType_t t = xTaskGetTickCount();
    cw_why = "fine";
    cw_owner0 = rd(PDX_C5_SRAM_OWNER);
    cw_ctrl0 = rd(PDX_C5_DUMP_BASE + 0x0Cu);
    cw_key_state = false;
    tx_dry = j->dry != 0;
    tx_on();                                          /* PA accesa (anche con la manipolazione PA: poi segue il tasto) */
    esp_rom_delay_us(3000);
    if (cw_key_pa && j->text[0]) tx_off();
    if (!j->text[0]) {                                /* portante fissa */
        cw_key(true);
        while (cw_wait(&t, 100)) {}
    } else {
        for (;;) {
            if (!cw_send_text(&t, j->text, dot)) break;
            if (j->carrier_s) {
                if (!cw_wait(&t, 4u * dot)) break;
                cw_key(true);
                if (!cw_wait(&t, j->carrier_s * 1000u)) break;
                cw_key(false);
            }
            if (!cw_wait(&t, j->pause_s ? j->pause_s * 1000u : 7u * dot)) break;
        }
    }
    if (cw_key_state) { cw_key_state = false; cw_dac(false); }
    tx_off();
    tx_dry = false;
    phy_txcal_work_mode();
    pdx_rf_tune_khz(cur_khz);
    for (unsigned k = 0; k < CW_WORDS + 4u; k++) PDX_C5_BANK[k] = 0;
    cw_running = false;
    if (cw_done) cw_done(cw_why);
    cw_job = NULL;
    free(j);
    vTaskDelete(NULL);
}

void pdx_rf_cw_set_amp(int amp) { cw_amp = amp < 1 ? 1 : amp > PDX_CW_AMP_MAX ? PDX_CW_AMP_MAX : amp; }
int pdx_rf_cw_amp(void) { return cw_amp; }
void pdx_rf_cw_set_key_pa(bool pa) { cw_key_pa = pa; }
bool pdx_rf_cw_key_pa(void) { return cw_key_pa; }
bool pdx_rf_cw_active(void) { return cw_running; }
void pdx_rf_cw_stop(void) { cw_stop_req = true; }

const char *pdx_rf_cw_start(const pdx_cw_job_t *job, void (*done)(const char *why))
{
    if (cw_running) return "CW gia' in corso";
    if (bankB_lent) return "dashboard attiva in questo avvio: usare LAB";
    pdx_cw_job_t *j = malloc(sizeof *j);
    if (!j) return "memoria";
    *j = *job;
    j->text[sizeof j->text - 1] = 0;
    if (j->max_s < 1) j->max_s = 1;
    if (j->max_s > PDX_CW_MAX_S) j->max_s = PDX_CW_MAX_S;
    if (j->wpm && (j->wpm < 5 || j->wpm > 40)) j->wpm = 12;
    const uint32_t w = pdx_dac_word(cw_amp, 0);
    for (unsigned k = 0; k < CW_WORDS; k++) PDX_C5_BANK[k] = w;
    for (unsigned k = CW_WORDS; k < CW_WORDS + 4u; k++) PDX_C5_BANK[k] = 0;
    cw_stop_req = false;
    cw_deadline = esp_timer_get_time() + (int64_t)j->max_s * 1000000;
    cw_done = done;
    cw_job = j;
    cw_running = true;
    if (xTaskCreate(cw_task_fn, "pdx_cw", 3072, j, 5, NULL) != pdPASS) { cw_running = false; cw_job = NULL; free(j); return "task"; }
    return NULL;
}

/* copia a tratti contigui: la tabella è lunga 2L (tono ripetuto due volte), quindi da qualunque k si possono copiare
 * L parole senza controllare la fine; ciclo interno srotolato x4 senza volatile */
static IRAM_ATTR __attribute__((noinline)) void copy_run(uint32_t *d, const uint32_t *s, uint32_t cnt)
{
    while (cnt >= 4) {
        const uint32_t a = s[0], b = s[1], c = s[2], e = s[3];
        d[0] = a; d[1] = b; d[2] = c; d[3] = e;
        d += 4; s += 4; cnt -= 4;
    }
    while (cnt--) *d++ = *s++;
}

static IRAM_ATTR __attribute__((noinline)) void fill_part(uint32_t *d, uint32_t cnt, uint32_t k, uint32_t l)
{
    while (cnt) {
        const uint32_t run = cnt < l ? cnt : l;
        copy_run(d, &stream_tab[k], run);      /* k < l, run <= l: entro 2L */
        d += run;
        cnt -= run;
        k += run;
        if (k >= l) k -= l;
    }
}

static IRAM_ATTR __attribute__((noinline)) void stream_fill(volatile uint32_t *dst, int32_t j, uint32_t n, uint32_t l)
{
    const uint32_t h = n / 2u;
    uint32_t *d = (uint32_t *)dst;
    const int32_t sl = (int32_t)l;
    const int32_t k1 = (int32_t)(((j + 1) % sl) * (int32_t)(n % l) % sl);           /* (j+1)N mod L */
    const int32_t k2 = (int32_t)((((j % sl) * (int32_t)(n % l) + (int32_t)(h % l)) % sl + sl) % sl);  /* jN+N/2 mod L */
    fill_part(d, h, (uint32_t)((k1 + sl) % sl), l);
    fill_part(d + h, n - h, (uint32_t)k2, l);
}

/* ---- P3a: QPSK con RRC (roll-off 0,35, 8 simboli) generato in tempo reale ----
 * 15 campioni calcolati per simbolo, ciascuno ripetuto H volte a 40 MS/s: SR = 40e6 / (15 H) (H=8 -> 333,3 kS).
 * qpsk_t[fase][storia]: somma degli 8 simboli (+-1) pesati dal filtro, già nel formato a 10 bit (stessa tabella per I e Q).
 * Il flusso è sequenziale: per ogni banco si riempie prima la metà alta (campioni che suonano prima) poi la bassa. */
#define QPSK_SPS_MAX 16u
static uint16_t qpsk_t[QPSK_SPS_MAX][256];
static uint32_t qsps = 15;          /* fasi del filtro = campioni calcolati per simbolo */
static struct { uint32_t p, hi, hq, lfsr, hold; } qg;
static int stream_kind;                     /* 0 = tono, 1 = QPSK */

static float rrc_f(float t, float b)
{
    const float pi = 3.14159265f;
    if (fabsf(t) < 1e-6f) return 1.0f - b + 4.0f * b / pi;
    if (fabsf(fabsf(t) - 1.0f / (4.0f * b)) < 1e-4f)
        return b / 1.41421356f * ((1.0f + 2.0f / pi) * sinf(pi / (4.0f * b)) + (1.0f - 2.0f / pi) * cosf(pi / (4.0f * b)));
    return (sinf(pi * t * (1.0f - b)) + 4.0f * b * t * cosf(pi * t * (1.0f + b))) / (pi * t * (1.0f - 16.0f * b * b * t * t));
}

static float qpsk_sum(unsigned p, unsigned h)
{
    float s = 0;
    for (unsigned m = 0; m < 8; m++) {
        const float sym = (h >> m) & 1u ? -1.0f : 1.0f;             /* bit m = simbolo n-m */
        s += sym * rrc_f((float)p / (float)qsps + (float)m - 3.5f, 0.35f);
    }
    return s;
}

static void qpsk_build(int amp)                 /* due passate: niente tabella float da 15 KB nel banco 0 */
{
    float vmax = 0;
    for (unsigned p = 0; p < qsps; p++)
        for (unsigned h = 0; h < 256; h++) {
            const float s = fabsf(qpsk_sum(p, h));
            if (s > vmax) vmax = s;
        }
    for (unsigned p = 0; p < qsps; p++)
        for (unsigned h = 0; h < 256; h++) qpsk_t[p][h] = (uint16_t)((int)lroundf(qpsk_sum(p, h) / vmax * amp) & 0x3FF);
}

static IRAM_ATTR __attribute__((noinline)) void qpsk_gen(uint32_t *d, uint32_t cnt)
{
    uint32_t p = qg.p, hi = qg.hi, hq = qg.hq, lfsr = qg.lfsr;
    const uint32_t hold = qg.hold, ns = qsps;
    uint32_t *const end = d + cnt;
    while (d < end) {
        const uint32_t w = (uint32_t)qpsk_t[p][hi] | ((uint32_t)qpsk_t[p][hq] << 10);
        if (hold == 8) {
            d[0] = w; d[1] = w; d[2] = w; d[3] = w; d[4] = w; d[5] = w; d[6] = w; d[7] = w;
            d += 8;
        } else {
            for (uint32_t k = 0; k < hold; k++) *d++ = w;
        }
        if (++p == ns) {
            p = 0;
            /* PRBS 2^23-1 (x^23 + x^18 + 1): due bit per simbolo */
            uint32_t b0 = ((lfsr >> 22) ^ (lfsr >> 17)) & 1u;
            lfsr = ((lfsr << 1) | b0) & 0x7FFFFFu;
            uint32_t b1 = ((lfsr >> 22) ^ (lfsr >> 17)) & 1u;
            lfsr = ((lfsr << 1) | b1) & 0x7FFFFFu;
            hi = ((hi << 1) | b0) & 255u;
            hq = ((hq << 1) | b1) & 255u;
        }
    }
    qg.p = p; qg.hi = hi; qg.hq = hq; qg.lfsr = lfsr;
}

/* ---- P3b: simboli DVB-S2 dalla partizione "symbols" (formato PDXSYM01 dell'S3: 4 simboli per byte, primo nei bit
 * bassi, bit0 = I, bit1 = Q, 1 = +1). Nella tabella RRC il bit di storia 1 vale -1, quindi bit di storia = !bit. */
static const uint8_t *sym_data;
static uint32_t sym_n, sym_pos, sym_qinv;

/* SR frazionario: ogni simbolo dura ns*hold parole a 40 MS/s più, in media, fx.fr/fx.sr parole; l'accumulatore
 * aggiunge una copia in più del primo campione del simbolo quando supera fx.sr (333 kS: +1 parola ogni ~8,3 simboli,
 * salto di 1/120 di simbolo). fx.rem/fx.w = copie del campione rimaste da scrivere nel banco successivo. */
static struct { uint32_t acc, fr, sr, rem, w; } fx;

static IRAM_ATTR __attribute__((noinline)) void qpsk_gen_sym(uint32_t *d, uint32_t cnt)
{
    uint32_t p = qg.p, hi = qg.hi, hq = qg.hq, pos = sym_pos;
    const uint32_t n = sym_n, qx = sym_qinv, ns = qsps, hold = qg.hold;
    uint32_t acc = fx.acc, rem = fx.rem, w = fx.w;
    const uint32_t fr = fx.fr, sr = fx.sr;
    const uint8_t *src = sym_data;
    uint32_t *const end = d + cnt;
    while (rem && d < end) { *d++ = w; rem--; }
    while (d < end) {
        w = (uint32_t)qpsk_t[p][hi] | ((uint32_t)qpsk_t[p][hq] << 10);
        uint32_t c = hold;
        if (p == 0) {
            acc += fr;
            if (acc >= sr) { acc -= sr; c++; }
        }
        if (++p == ns) {
            p = 0;
            const uint32_t s = (src[pos >> 2] >> ((pos & 3u) * 2u)) & 3u;
            if (++pos == n) pos = 0;
            hi = ((hi << 1) | (~s & 1u)) & 255u;
            hq = ((hq << 1) | ((((~s) >> 1) ^ qx) & 1u)) & 255u;   /* qx = 1: Q negato (spettro invertito) */
        }
        const uint32_t room = (uint32_t)(end - d);
        if (c == 8 && room >= 8) {
            d[0] = w; d[1] = w; d[2] = w; d[3] = w; d[4] = w; d[5] = w; d[6] = w; d[7] = w;
            d += 8;
        } else if (c == 10 && room >= 10) {
            d[0] = w; d[1] = w; d[2] = w; d[3] = w; d[4] = w; d[5] = w; d[6] = w; d[7] = w; d[8] = w; d[9] = w;
            d += 10;
        } else {
            const uint32_t k = c < room ? c : room;
            for (uint32_t i = 0; i < k; i++) *d++ = w;
            rem = c - k;
        }
    }
    qg.p = p; qg.hi = hi; qg.hq = hq; sym_pos = pos;
    fx.acc = acc; fx.rem = rem; fx.w = w;
}

/* ---- P4b: TS continuo generato sulla C5 (mux dal monoscopio ES in flash -> DVB-S2 a passi -> mappatura pigra) ----
 * Doppio buffer delle parole di codice: il modulatore legge cw[cur], il produttore riempie cw[cur ^ 1] nei tempi morti
 * del ciclo di scambio dei banchi (stream_loop). Memoria: cw0 e righe LDPC nel banco 0, cw1 e tabella BCH nella RAM LP
 * (i banchi 1 e 2 sono della radio a turno durante la TX). Se il frame successivo non è pronto: frame DUMMY. */
#include "dvbs2_enc.h"
#include "ts_mux.h"

#define LIVE_ROWS_CAP 45u                    /* q massimo: rate da 3/4 in su (frame normali) */
#define LIVE_CW_BYTES 8100u                  /* 64800 bit */
static uint32_t live_rows[LIVE_ROWS_CAP][12];
static uint8_t live_cw0[LIVE_CW_BYTES];
static uint8_t *live_cw1;                    /* RAM LP */
static void *live_bch;                       /* RAM LP, 6144 B */
static pdx_s2_t live_s2;
static pdx_mux_t live_mux;
static uint8_t live_hdr_dummy[90];
static pdx_s2l_job_t live_job;
static pdx_s2l_map_t live_map;
static struct {
    uint8_t *cw[2];
    uint32_t cur, ready, pstage, fpos, pkt_pos, nb;
    uint16_t syncd;
    uint8_t prev_crc;
    uint8_t pkt[188];
    uint32_t frames, dummies, step_max, steps;
    uint32_t st_max[8], st_kcyc[8], st_n[8];   /* per fase: 0 nuovo campo, 1 pacchetti, 2+ job (scr, bch, ldpc, par, xor) */
} lv;

/* un passo del produttore; false se non c'è niente da fare (frame successivo già pronto) */
static __attribute__((noinline)) bool live_producer_step(void)   /* in flash: non critico al ciclo */
{
    if (lv.ready) return false;
    uint8_t *cw = lv.cw[lv.cur ^ 1u];
    if (lv.pstage == 0) {                                  /* nuovo DATA FIELD: SYNCD = bit fino al primo pacchetto */
        const uint32_t to_start = (188u - lv.pkt_pos) % 188u;
        lv.syncd = to_start < lv.nb ? (uint16_t)(to_start * 8u) : 0xFFFFu;
        lv.fpos = 0;
        lv.pstage = 1;
        return true;
    }
    if (lv.pstage == 1) {                                  /* un pacchetto TS per passo */
        for (int n = 0; n < 1 && lv.fpos < lv.nb; n++) {
            if (lv.pkt_pos == 188u) {
                uint8_t raw[188];
                pdx_mux_next(&live_mux, raw);
                lv.pkt[0] = lv.prev_crc;
                memcpy(lv.pkt + 1, raw + 1, 187);
                lv.prev_crc = pdx_s2_crc8(raw + 1, 187);
                lv.pkt_pos = 0;
            }
            uint32_t k = 188u - lv.pkt_pos;
            if (k > lv.nb - lv.fpos) k = lv.nb - lv.fpos;
            memcpy(cw + 10 + lv.fpos, lv.pkt + lv.pkt_pos, k);
            lv.fpos += k;
            lv.pkt_pos += k;
        }
        if (lv.fpos == lv.nb) {
            pdx_s2l_job_start(&live_job, &live_s2, cw, lv.syncd);
            lv.pstage = 2;
        }
        return true;
    }
    if (pdx_s2l_job_step(&live_job)) {
        lv.ready = 1;
        lv.pstage = 0;
        lv.frames++;
    }
    return true;
}

/* prossimo codice QPSK del flusso DVB-S2 (bit 0 = I, bit 1 = Q, 1 = +1) */
static IRAM_ATTR __attribute__((noinline)) uint32_t live_next_code(void)
{
    if (pdx_s2l_map_done(&live_map)) {
        if (lv.ready) {
            lv.cur ^= 1u;
            lv.ready = 0;
            pdx_s2l_map_start(&live_map, &live_s2, lv.cw[lv.cur], live_s2.hdr);
        } else {
            lv.dummies++;
            pdx_s2l_map_start(&live_map, &live_s2, NULL, live_hdr_dummy);
        }
    }
    return pdx_s2l_map_next(&live_map);
}

static IRAM_ATTR __attribute__((noinline)) void qpsk_gen_live(uint32_t *d, uint32_t cnt)
{
    uint32_t p = qg.p, hi = qg.hi, hq = qg.hq;
    const uint32_t qx = sym_qinv, ns = qsps, hold = qg.hold;
    uint32_t acc = fx.acc, rem = fx.rem, w = fx.w;
    const uint32_t fr = fx.fr, sr = fx.sr;
    uint32_t *const end = d + cnt;
    while (rem && d < end) { *d++ = w; rem--; }
    while (d < end) {
        w = (uint32_t)qpsk_t[p][hi] | ((uint32_t)qpsk_t[p][hq] << 10);
        uint32_t c = hold;
        if (p == 0) {
            acc += fr;
            if (acc >= sr) { acc -= sr; c++; }
        }
        if (++p == ns) {
            p = 0;
            const uint32_t s = live_next_code();
            hi = ((hi << 1) | (~s & 1u)) & 255u;
            hq = ((hq << 1) | ((((~s) >> 1) ^ qx) & 1u)) & 255u;
        }
        const uint32_t room = (uint32_t)(end - d);
        if (c == 8 && room >= 8) {
            d[0] = w; d[1] = w; d[2] = w; d[3] = w; d[4] = w; d[5] = w; d[6] = w; d[7] = w;
            d += 8;
        } else {
            const uint32_t k = c < room ? c : room;
            for (uint32_t i = 0; i < k; i++) *d++ = w;
            rem = c - k;
        }
    }
    qg.p = p; qg.hi = hi; qg.hq = hq;
    fx.acc = acc; fx.rem = rem; fx.w = w;
}

static const uint8_t *ovl_blob[PDX_OVL_FIELDS];          /* strisce della sovrimpressione (ovl_prepare) */
static uint32_t ovl_len[PDX_OVL_FIELDS];

/* prepara memoria, codificatore e mux; produce il primo frame (senza fretta). NULL se riuscito */
static const char *live_prepare(const uint8_t *es_file, uint32_t sr, const char *rate, const char *service,
                                const char *provider)
{
    if (!live_bch) live_bch = heap_caps_malloc(6144, MALLOC_CAP_RTCRAM);
    if (!live_cw1) live_cw1 = heap_caps_malloc(LIVE_CW_BYTES, MALLOC_CAP_RTCRAM);
    if (!live_bch || !live_cw1) return "memoria LP";
    const char *e = pdx_s2l_init(&live_s2, rate, false, true, live_bch, live_rows, LIVE_ROWS_CAP);
    if (e) return e;
    pdx_s2l_dummy_header(&live_s2, live_hdr_dummy);
    pdx_s2l_map_init();
    e = pdx_mux_init(&live_mux, es_file, pdx_s2_ts_bitrate(&live_s2, sr), service, provider, false);
    if (e) return e;
    pdx_mux_set_overlay(&live_mux, ovl_blob, ovl_len);           /* prima del primo IDR */
    memset(&lv, 0, sizeof(lv));
    lv.cw[0] = live_cw0;
    lv.cw[1] = live_cw1;
    lv.cur = 1;                                            /* il primo frame va in cw[0] */
    lv.pkt_pos = 188u;
    lv.nb = live_s2.dfl / 8u;
    while (!lv.ready) live_producer_step();
    memset(&live_map, 0, sizeof(live_map));                /* k = nsym = 0: al primo simbolo passa a cw[0] */
    lv.frames = 0;
    return NULL;
}

/* banco dell'intervallo j: prima la metà alta (suona prima), poi la bassa */
static IRAM_ATTR __attribute__((noinline)) void qpsk_fill(volatile uint32_t *dst, int32_t j, uint32_t n, uint32_t l)
{
    (void)j; (void)l;
    uint32_t *d = (uint32_t *)dst;
    if (stream_kind == 2) {
        qpsk_gen_sym(d + n / 2u, n - n / 2u);
        qpsk_gen_sym(d, n / 2u);
        return;
    }
    if (stream_kind == 3) {
        qpsk_gen_live(d + n / 2u, n - n / 2u);
        qpsk_gen_live(d, n / 2u);
        return;
    }
    qpsk_gen(d + n / 2u, n - n / 2u);
    qpsk_gen(d, n / 2u);
}

static IRAM_ATTR __attribute__((noinline)) void any_fill(volatile uint32_t *dst, int32_t j, uint32_t n, uint32_t l)
{
    if (stream_kind >= 1) qpsk_fill(dst, j, n, l);
    else stream_fill(dst, j, n, l);
}

static IRAM_ATTR __attribute__((noinline)) void stream_loop(uint32_t n, uint32_t l, uint32_t intervals, uint32_t lat,
                                                            uint32_t oa, uint32_t ob, uint32_t t0)
{
    volatile uint32_t *own = (volatile uint32_t *)PDX_C5_SRAM_OWNER;
    const uint32_t per = n * 6u;                                    /* 40 MS/s = 6 cicli per campione */
    uint32_t tsw = t0 + lat + (n / 2u) * 6u;                        /* primo scambio: intervallo 0 */
    for (uint32_t j = 0; j < intervals; j++) {
        if ((j & 255u) == 0 && (~REG_READ(GPIO_IN_REG) & stop_mask)) { stream_res.stopped = 1; intervals = j; break; }  /* BOOT o GPIO STOP */
        while ((int32_t)(esp_cpu_get_cycle_count() - tsw) < 0) {}
        const uint32_t late = esp_cpu_get_cycle_count() - tsw;
        *own = (j & 1u) ? oa : ob;                                  /* j pari -> B, dispari -> A */
        if (late > stream_res.late_max) stream_res.late_max = late;
        const uint32_t f0 = esp_cpu_get_cycle_count();
        any_fill((j & 1u) ? PDX_PP_B : PDX_PP_A, (int32_t)j + 1, n, l);      /* banco appena liberato: intervallo j+1 */
        const uint32_t fc = esp_cpu_get_cycle_count() - f0;
        if (fc > stream_res.fill_max) stream_res.fill_max = fc;
        if (stream_kind == 3) {                                   /* tempi morti: passi del produttore DVB-S2 */
            /* margine adattivo: un passo parte solo se prima dello scambio resta più del passo più lungo già visto
             * per la sua fase (+1500 cicli); all'inizio, senza misure, 12000 cicli */
            const uint32_t sw = tsw + per;
            for (;;) {
                const uint32_t ph = lv.pstage < 2u ? lv.pstage : 2u + (live_job.stage < 5u ? live_job.stage : 5u);
                const uint32_t mx = lv.st_max[ph] < 9000u ? lv.st_max[ph] : 9000u;   /* tetto: un picco a freddo non blocca la fase */
                const uint32_t need = mx ? mx + 1500u : 9000u;
                if ((int32_t)(sw - esp_cpu_get_cycle_count()) < (int32_t)need) break;
                const uint32_t s0 = esp_cpu_get_cycle_count();
                if (!live_producer_step()) break;
                const uint32_t sc = esp_cpu_get_cycle_count() - s0;
                if (sc > lv.step_max) lv.step_max = sc;
                if (sc > lv.st_max[ph]) lv.st_max[ph] = sc;
                lv.st_kcyc[ph] += (sc + 512u) >> 10;
                lv.st_n[ph]++;
                lv.steps++;
            }
        }
        if ((int32_t)(esp_cpu_get_cycle_count() - (tsw + per)) > 0) stream_res.overruns++;
        tsw += per;
    }
    stream_res.intervals = intervals;
}

static void stream_run(const pp_req_t *q)
{
    const uint32_t n = q->n & ~1u, l = q->l;
    const uint32_t ctrl = PDX_C5_DUMP_BASE + 0x0Cu;
    const uint32_t owner0 = rd(PDX_C5_SRAM_OWNER), ctrl0 = rd(ctrl);
    const uint32_t base = owner0 & ~0x10F00u, oa = base | 0x200u, ob = base | 0x400u;
    stream_res = (pdx_stream_res_t){0};
    for (uint32_t s = 0; s < l; s++) {
        const float ph = 6.2831853f * (float)s / (float)l;
        stream_tab[s] = pdx_dac_word((int)lroundf(q->amp * cosf(ph)), (int)lroundf(q->amp * sinf(ph)));
        stream_tab[s + l] = stream_tab[s];
    }
    if (stream_kind >= 1) {
        qpsk_build(q->amp);
        qg.p = 0; qg.hi = 0; qg.hq = 0; qg.lfsr = 0x5A5A5Au; qg.hold = l;   /* per il QPSK l = H */
        sym_pos = 0;
        if (stream_kind == 3) qpsk_gen_live((uint32_t *)PDX_PP_A, n / 2u);
        else if (stream_kind == 2) qpsk_gen_sym((uint32_t *)PDX_PP_A, n / 2u);
        else qpsk_gen((uint32_t *)PDX_PP_A, n / 2u);   /* intervallo -1: suona solo la metà bassa di A */
        qpsk_fill(PDX_PP_B, 0, n, l);
    } else {
        stream_fill(PDX_PP_A, -1, n, l);
        stream_fill(PDX_PP_B, 0, n, l);
    }
    const uint32_t intervals = (uint32_t)((uint64_t)q->ms * 40000u / n);
    tx_on();
    esp_rom_delay_us(3000);
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    wr(PDX_C5_SRAM_OWNER, oa);
    wr(ctrl, ctrl0 & ~(1u << 31));
    const uint32_t cv = 0x80000u | 0x20000u | n;                    /* ciclo, 40 MS/s, n parole */
    wr(ctrl, cv);
    const uint32_t t0 = esp_cpu_get_cycle_count();
    wr(ctrl, cv | (1u << 31));
    stream_loop(n, l, intervals, q->lat, oa, ob, t0);
    wr(ctrl, rd(ctrl) & ~(1u << 31));
    wr(ctrl, ctrl0);
    wr(PDX_C5_SRAM_OWNER, owner0);
    taskEXIT_CRITICAL(&mux);
    tx_off();
    phy_txcal_work_mode();
    pdx_rf_tune_khz(cur_khz);
}

static void pp_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        pp_req_t *q = &pp_req;
        if (q->mode == 10) {
            stream_run(q);
            *q->sres = stream_res;
            xTaskNotifyGive(q->caller);
            continue;
        }
        static pdx_pp_res_t res;   /* nel banco 0: il chiamante puo' avere lo stack nel banco 2 */
        pdx_pp_res_t *r = &res;
        const uint32_t ctrl = PDX_C5_DUMP_BASE + 0x0Cu;
        const uint32_t owner0 = rd(PDX_C5_SRAM_OWNER), ctrl0 = rd(ctrl);
        const uint32_t base = owner0 & ~0x10F00u;
        r->owner_a = base | 0x200u | (q->alloc ? 0x10000u : 0);
        r->owner_b = base | 0x400u | (q->alloc ? 0x10000u : 0);
        tx_on();
        esp_rom_delay_us(3000);
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        taskENTER_CRITICAL(&mux);
        wr(PDX_C5_SRAM_OWNER, q->mode == 1 ? r->owner_b : r->owner_a);
        wr(ctrl, ctrl0 & ~(1u << 31));
        wr(ctrl, 0x80000u | 1600u);
        wr(ctrl, 0x80000u | 1600u | (1u << 31));
        r->ctrl_running = rd(ctrl);
        r->switches = pp_loop(r->owner_a, r->owner_b, q->mode, q->period_us * 240u, q->ms * 240000u);
        r->ctrl_end = rd(ctrl);
        wr(ctrl, rd(ctrl) & ~(1u << 31));
        wr(ctrl, ctrl0);
        wr(PDX_C5_SRAM_OWNER, owner0);
        taskEXIT_CRITICAL(&mux);
        tx_off();
        phy_txcal_work_mode();
        pdx_rf_tune_khz(cur_khz);
        *q->r = res;
        xTaskNotifyGive(q->caller);
    }
}

void pdx_rf_pp_init(void)
{
    pp_task = xTaskCreateStatic(pp_task_fn, "pp", sizeof pp_stack / sizeof pp_stack[0], NULL, 5, pp_stack, &pp_tcb);
}

void pdx_rf_pp(unsigned mode, unsigned ms, unsigned period_us, unsigned alloc, pdx_pp_res_t *r)
{
    *r = (pdx_pp_res_t){0};
    BANKB_GUARD(return);
    if (ms > PDX_TX_MS_MAX) ms = PDX_TX_MS_MAX;
    if (period_us < 20) period_us = 20;
    fill_tone(PDX_PP_A, 1600, 16, 200);
    fill_tone(PDX_C5_BANK, 1600, 32, 200);      /* 0x40830000 = A + 64 KB */
    fill_tone(PDX_PP_B, 1600, mode >= 3 ? 16 : -16, 200);   /* mode 3: stesso tono in fase nei due banchi */
    pp_req = (pp_req_t){.mode = mode, .ms = ms, .period_us = period_us, .alloc = alloc, .r = r,
                        .caller = xTaskGetCurrentTaskHandle()};
    esp_wifi_set_promiscuous(false);              /* niente DMA di ricezione nei buffer del banco 2 */
    vTaskDelay(pdMS_TO_TICKS(20));
    xTaskNotifyGive(pp_task);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_wifi_set_promiscuous(true);
}

void pdx_rf_stream(uint32_t n, uint32_t l, int amp, unsigned ms, uint32_t lat, pdx_stream_res_t *r)
{
    *r = (pdx_stream_res_t){0};
    BANKB_GUARD(return);
    if (ms > PDX_TX_MS_MAX) ms = PDX_TX_MS_MAX;
    if (amp > PDX_DAC_AMP_MAX) amp = PDX_DAC_AMP_MAX;
    if (n < 1000) n = 1000;
    if (n > 12000) n = 12000;
    if (l < 2) l = 2;
    if (l > STREAM_L_MAX) l = STREAM_L_MAX;
    stream_kind = 0;
    pp_req = (pp_req_t){.mode = 10, .ms = ms, .n = n, .l = l, .lat = lat, .amp = amp, .sres = r,
                        .caller = xTaskGetCurrentTaskHandle()};
    esp_wifi_set_promiscuous(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    xTaskNotifyGive(pp_task);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_wifi_set_promiscuous(true);
}

void pdx_rf_qpsk(uint32_t n, uint32_t hold, int amp, unsigned ms, uint32_t lat, pdx_stream_res_t *r)
{
    *r = (pdx_stream_res_t){0};
    BANKB_GUARD(return);
    if (ms > PDX_TX_MS_MAX) ms = PDX_TX_MS_MAX;
    if (amp > PDX_DAC_AMP_MAX) amp = PDX_DAC_AMP_MAX;
    if (hold != 4 && hold != 8 && hold != 16) hold = 8;
    n = n / (2u * hold) * (2u * hold);          /* metà banco multipla di H */
    if (n < 1600) n = 1600;
    if (n > 12000) n = 12000;
    stream_kind = 1;
    qsps = 15;
    pp_req = (pp_req_t){.mode = 10, .ms = ms, .n = n, .l = hold, .lat = lat, .amp = amp, .sres = r,
                        .caller = xTaskGetCurrentTaskHandle()};
    esp_wifi_set_promiscuous(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    xTaskNotifyGive(pp_task);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_wifi_set_promiscuous(true);
    stream_kind = 0;
}

#include "esp_partition.h"
#include "esp_rom_crc.h"
#include <inttypes.h>
#include "soc/gpio_reg.h"

static esp_partition_mmap_handle_t sym_mh;

/* mappa la partizione "symbols" e controlla l'intestazione PDXSYM01; restituisce il numero di simboli (0 = errore) */
uint32_t pdx_rf_sym_open(uint32_t *sr, char label[41])
{
    const esp_partition_t *pt = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "symbols");
    if (!pt) return 0;
    if (!sym_data) {
        const void *ptr;
        if (esp_partition_mmap(pt, 0, pt->size, ESP_PARTITION_MMAP_DATA, &ptr, &sym_mh) != ESP_OK) return 0;
        sym_data = (const uint8_t *)ptr;
    }
    const uint8_t *h = sym_data;
    if (memcmp(h, "PDXSYM01", 8) != 0) return 0;
    uint32_t n;
    memcpy(sr, h + 8, 4);
    memcpy(&n, h + 12, 4);
    memcpy(label, h + 24, 40);
    label[40] = 0;
    if (n == 0 || n / 4u + 64u > pt->size) return 0;
    return n;
}

void pdx_rf_s2play(int amp, unsigned ms, uint32_t lat, unsigned qinv, pdx_stream_res_t *r)
{
    *r = (pdx_stream_res_t){0};
    BANKB_GUARD(return);
    if (ms > PDX_S2_MS_MAX) ms = PDX_S2_MS_MAX;
    if (amp > PDX_DAC_AMP_MAX) amp = PDX_DAC_AMP_MAX;
    uint32_t sr;
    char lab[41];
    const uint32_t n = pdx_rf_sym_open(&sr, lab);
    *r = (pdx_stream_res_t){0};
    if (!n) return;
    const uint8_t *base = sym_data;
    sym_data = base + 64;                        /* simboli dopo l'intestazione */
    sym_n = n / 4u * 4u;
    sym_qinv = qinv ? 1u : 0u;
    const uint32_t tot = 40000000u / sr;                     /* parole intere a 40 MS/s per simbolo */
    if (sr < 50000u || sr > 2000000u) { sym_data = base; return; }
    qsps = (tot % 16u == 0) ? 16u : (tot % 15u == 0) ? 15u : 8u;
    const uint32_t hold = tot / qsps;
    fx.sr = sr;
    fx.fr = 40000000u - qsps * hold * sr;                    /* resto per simbolo (333 kS: 40000) */
    fx.acc = 0; fx.rem = 0; fx.w = 0;
    if (fx.fr >= sr) { sym_data = base; return; }             /* al massimo una parola in più per simbolo */
    stream_kind = 2;
    pp_req = (pp_req_t){.mode = 10, .ms = ms, .n = 12000, .l = hold, .lat = lat, .amp = amp, .sres = r,
                        .caller = xTaskGetCurrentTaskHandle()};
    esp_wifi_set_promiscuous(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    xTaskNotifyGive(pp_task);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_wifi_set_promiscuous(true);
    stream_kind = 0;
    sym_data = base;
}

/* ---- P4b: comandi del TS continuo ---- */
/* monoscopi nella partizione "symbols" (da 0x190000), area assoluta 0x290000..0x3F0000:
 *   PDXES002 neutro (23/11): uno solo per le due bande, con nominativo e frequenza in sovrimpressione;
 *   oppure i due PDXES001 di prima: 6 cm a 0x290000, 13 cm a 0x320000. */
#define LIVE_ES_AREA_OFF 0x100000u
#define LIVE_ES_AREA_LEN 0x160000u
#define LIVE_ES13_OFF    0x090000u
#define OVL_PART_OFF     0x260000u                   /* assoluto 0x3F0000: strisce composte, 0x8000 per banda */
#define OVL_SLOT         0x8000u
#define OVL_BUF          1600u                       /* una slice con intestazione ed emulation prevention */
static const uint8_t *live_area;
static esp_partition_mmap_handle_t live_area_mh;
static const uint8_t *live_es[2];
static int live_src;                                 /* monoscopio scelto per la prossima S2LIVE */
static char live_service[24] = "NOCALL", live_provider[40] = "BEACON C5";
static char live_call[16] = "NOCALL", live_freq[16];  /* testi della sovrimpressione (freq vuota = dalla sintonia) */
static const uint8_t *ovl_map;
static esp_partition_mmap_handle_t ovl_mh;
static const char *live_es_open(int k)
{
    if (k < 0 || k > 1) return "monoscopio";
    if (live_es[k]) return NULL;
    const esp_partition_t *pt = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "symbols");
    if (!pt) return "partizione symbols";
    if (!live_area) {
        const void *ptr;
        if (esp_partition_mmap(pt, LIVE_ES_AREA_OFF, LIVE_ES_AREA_LEN, ESP_PARTITION_MMAP_DATA, &ptr, &live_area_mh) !=
            ESP_OK)
            return "mmap ES";
        live_area = ptr;
    }
    const uint8_t *e = !memcmp(live_area, "PDXES002", 8) ? live_area : live_area + (k ? LIVE_ES13_OFF : 0);
    if (memcmp(e, "PDXES001", 8) && memcmp(e, "PDXES002", 8)) return "file ES assente";
    live_es[k] = e;
    return NULL;
}

/* ---- sovrimpressione: strisce del nominativo e della frequenza composte in flash dalle slice dei caratteri ---- */
typedef struct { uint8_t b[16]; uint32_t n; } ovl_bw_t;
static void bw_u(ovl_bw_t *w, uint32_t bits, uint32_t v)
{
    while (bits--) {
        if ((v >> bits) & 1u) w->b[w->n >> 3] |= (uint8_t)(0x80u >> (w->n & 7u));
        w->n++;
    }
}
static void bw_ue(ovl_bw_t *w, uint32_t v)
{
    v++;
    const uint32_t bits = 32u - (uint32_t)__builtin_clz(v);
    bw_u(w, bits - 1u, 0);
    bw_u(w, bits, v);
}

typedef struct { uint8_t *b; uint32_t n, zeros; } ovl_esc_t;
static void esc_put(ovl_esc_t *e, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (e->zeros >= 2 && p[i] <= 3) { e->b[e->n++] = 3; e->zeros = 0; }
        e->b[e->n++] = p[i];
        e->zeros = p[i] ? 0 : e->zeros + 1;
    }
}

static const pdx_ovl_field_t *ovl_field(const pdx_ovl_header_t *o, uint32_t f)
{
    return (const pdx_ovl_field_t *)((const uint8_t *)o + sizeof(*o)) + f;
}

/* voce del carattere (chiave di 1 o 2 caratteri) codificato sopra lo sfondo della colonna col, NULL se assente */
static const uint8_t *ovl_glyph(const pdx_ovl_header_t *o, const pdx_ovl_field_t *fd, char c0, char c1, uint32_t col)
{
    const uint8_t *map = (const uint8_t *)o + fd->glyph_off;
    const uint8_t grp = map[col < o->cols ? col : 0];
    const uint32_t esz = 4u + 8u * fd->rows;
    const uint8_t *e = map + ((o->cols + 3u) & ~3u);
    for (uint32_t i = 0; i < fd->nglyph; i++, e += esz)
        if (e[0] == (uint8_t)c0 && e[1] == (uint8_t)(fd->cell == 2 ? c1 : 0) && e[2] == grp) return e;
    return NULL;
}

/* compone la striscia f per il testo dato e la scrive nella partizione da woff; lunghezza o 0 se errore */
static uint32_t ovl_compose(const esp_partition_t *pt, uint32_t woff, const pdx_ovl_header_t *o, uint32_t f,
                            const char *text, uint8_t *buf, uint32_t *crc)
{
    const pdx_ovl_field_t *fd = ovl_field(o, f);
    char t[2 * 20 + 2];
    uint32_t n = 0;
    const bool call = !strncmp(fd->name, "call", 4);
    for (const char *p = text; *p && n < sizeof t - 2; p++) {
        char c = call && *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
        if (!ovl_glyph(o, fd, c, ' ', 0)) c = ' ';    /* carattere non disponibile: spazio */
        t[n++] = c;
    }
    const uint32_t cell = fd->cell ? fd->cell : 1u;
    while (n % cell) t[n++] = ' ';
    uint32_t ncell = n / cell;
    if (ncell > o->cols) ncell = o->cols;
    const uint32_t c0 = (o->cols - ncell) / 2u;
    uint8_t nalh[2];
    memcpy(nalh, (const uint8_t *)o + o->idr_off + 4, 2);
    uint32_t total = 0;
    for (uint32_t r = 0; r < fd->rows; r++) {
        for (uint32_t col = 0; col < o->cols; col++) {
            const bool in = col >= c0 && col < c0 + ncell;
            const char a = in ? t[(col - c0) * cell] : ' ', b = in && cell == 2 ? t[(col - c0) * cell + 1] : ' ';
            const uint8_t *g = ovl_glyph(o, fd, a, b, col);
            if (!g) g = ovl_glyph(o, fd, ' ', ' ', col);
            if (!g) return 0;
            uint32_t doff;
            uint16_t dlen;
            memcpy(&doff, g + 4 + 8 * r, 4);
            memcpy(&dlen, g + 8 + 8 * r, 2);
            const uint8_t qp = g[10 + 8 * r], sao = g[11 + 8 * r];
            ovl_bw_t w;
            memset(&w, 0, sizeof w);
            bw_u(&w, 1, 0);                                   /* first_slice_segment_in_pic_flag */
            bw_u(&w, 1, 0);                                   /* no_output_of_prior_pics_flag */
            bw_ue(&w, 0);                                     /* slice_pic_parameter_set_id */
            bw_u(&w, o->abits, (fd->row0 + r) * o->cols + col);   /* slice_segment_address */
            bw_ue(&w, 2);                                     /* slice_type I */
            if (o->flags & 1u) { bw_u(&w, 1, sao & 1u); bw_u(&w, 1, (sao >> 1) & 1u); }
            const int32_t dq = (int32_t)qp - 26 - o->init_qp_minus26;
            bw_ue(&w, dq > 0 ? 2u * (uint32_t)dq - 1u : (uint32_t)(-2 * dq));   /* slice_qp_delta */
            if (o->flags & 2u) bw_ue(&w, 0);                  /* num_entry_point_offsets */
            bw_u(&w, 1, 1);                                   /* byte_alignment */
            while (w.n & 7u) bw_u(&w, 1, 0);
            if (4u + 2u * (2u + w.n / 8u + dlen) > OVL_BUF) return 0;
            ovl_esc_t e = {.b = buf};
            static const uint8_t sc[4] = {0, 0, 0, 1};
            memcpy(buf, sc, 4);
            e.n = 4;
            esc_put(&e, nalh, 2);
            esc_put(&e, w.b, w.n / 8u);
            esc_put(&e, (const uint8_t *)o + doff, dlen);
            if (total + e.n > OVL_SLOT - 64u) return 0;
            if (esp_partition_write(pt, woff + total, buf, e.n) != ESP_OK) return 0;
            *crc = esp_rom_crc32_le(*crc, buf, e.n);
            total += e.n;
        }
    }
    return total;
}

typedef struct { char magic[8]; uint32_t key, nf, len[2], crc[2]; } ovl_slot_hdr_t;

/* strisce del monoscopio k per live_call / live_freq: riusa la flash se già composte, altrimenti le riscrive.
 * Senza sovrimpressione (PDXES001) non fa nulla. */
static const char *ovl_prepare(int k, const uint8_t *es, bool verbose)
{
    memset(ovl_blob, 0, sizeof ovl_blob);
    memset(ovl_len, 0, sizeof ovl_len);
    const pdx_ovl_header_t *o = pdx_es_overlay(es);
    if (!o) return NULL;
    char freq[16];
    if (live_freq[0]) snprintf(freq, sizeof freq, "%s", live_freq);
    else snprintf(freq, sizeof freq, "%u.%03u MHz", (unsigned)(cur_khz / 1000u), (unsigned)(cur_khz % 1000u));
    const char *txt[PDX_OVL_FIELDS] = {live_call, freq};
    uint32_t key = esp_rom_crc32_le(0, es, 64);
    for (int f = 0; f < PDX_OVL_FIELDS; f++) key = esp_rom_crc32_le(key, (const uint8_t *)txt[f], strlen(txt[f]) + 1);
    const esp_partition_t *pt = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "symbols");
    if (!pt) return "partizione symbols";
    const uint32_t slot = OVL_PART_OFF + (uint32_t)k * OVL_SLOT;
    ovl_slot_hdr_t h;
    if (esp_partition_read(pt, slot, &h, sizeof h) != ESP_OK) return "lettura strisce";
    const bool same = !memcmp(h.magic, "PDXBLB01", 8) && h.key == key && h.nf == o->nfields;
    if (!same) {
        if (ovl_map) { esp_partition_munmap(ovl_mh); ovl_map = NULL; }   /* niente dati vecchi in cache */
        uint8_t *buf = heap_caps_malloc(OVL_BUF, MALLOC_CAP_8BIT);
        if (!buf) return "memoria strisce";
        if (esp_partition_erase_range(pt, slot, OVL_SLOT) != ESP_OK) { free(buf); return "cancellazione strisce"; }
        memset(&h, 0, sizeof h);
        uint32_t off = 64;
        for (uint32_t f = 0; f < o->nfields && f < PDX_OVL_FIELDS; f++) {
            h.crc[f] = 0;
            h.len[f] = ovl_compose(pt, slot + off, o, f, txt[f], buf, &h.crc[f]);
            if (!h.len[f]) { free(buf); return "composizione strisce"; }
            off += (h.len[f] + 3u) & ~3u;
        }
        free(buf);
        memcpy(h.magic, "PDXBLB01", 8);
        h.key = key;
        h.nf = o->nfields;
        if (esp_partition_write(pt, slot, &h, sizeof h) != ESP_OK) return "scrittura strisce";
    }
    if (!ovl_map) {
        const void *ptr;
        if (esp_partition_mmap(pt, OVL_PART_OFF, 2u * OVL_SLOT, ESP_PARTITION_MMAP_DATA, &ptr, &ovl_mh) != ESP_OK)
            return "mmap strisce";
        ovl_map = ptr;
    }
    uint32_t off = 64;
    for (uint32_t f = 0; f < o->nfields && f < PDX_OVL_FIELDS; f++) {
        ovl_blob[f] = ovl_map + (uint32_t)k * OVL_SLOT + off;
        ovl_len[f] = h.len[f];
        off += (h.len[f] + 3u) & ~3u;
        if (verbose) printf("OVL striscia %s \"%s\": %" PRIu32 " byte CRC32 %08" PRIx32 "%s\n", ovl_field(o, f)->name,
                            txt[f], h.len[f], h.crc[f], same ? " (gia' in flash)" : " (scritta)");
    }
    return NULL;
}

void pdx_rf_live_overlay(const char *call, const char *freq)
{
    snprintf(live_call, sizeof live_call, "%s", call ? call : "");
    snprintf(live_freq, sizeof live_freq, "%s", freq ? freq : "");
}

const char *pdx_rf_ovl_test(int k, const char *call, const char *freq)
{
    pdx_rf_live_overlay(call, freq);
    const char *e = live_es_open(k);
    if (e) return e;
    if (!pdx_es_overlay(live_es[k])) return "monoscopio senza sovrimpressione (PDXES001)";
    return ovl_prepare(k, live_es[k], true);
}

void pdx_rf_live_source(int es, const char *service, const char *provider)
{
    live_src = es;
    snprintf(live_service, sizeof live_service, "%s", service);
    snprintf(live_provider, sizeof live_provider, "%s", provider);
}

void pdx_rf_live_stats(pdx_live_stats_t *st)
{
    st->frames = lv.frames;
    st->dummies = lv.dummies;
    st->step_max = lv.step_max;
    st->steps = lv.steps;
    st->ts_rate = live_mux.rate;
    st->null_pk = live_mux.stats_null;
    st->video_pk = live_mux.stats_video;
    st->audio_pk = live_mux.stats_audio;
    for (int i = 0; i < 8; i++) { st->st_max[i] = lv.st_max[i]; st->st_kcyc[i] = lv.st_kcyc[i]; st->st_n[i] = lv.st_n[i]; }
}

const char *pdx_rf_s2live(uint32_t sr, int amp, unsigned ms, uint32_t lat, unsigned qinv, unsigned dry,
                          pdx_stream_res_t *r)
{
    if (bankB_lent) { *r = (pdx_stream_res_t){0}; return "banco B prestato al heap: usare BEACON o START (riavvio)"; }
    if (ms > PDX_S2_MS_MAX) ms = PDX_S2_MS_MAX;
    if (amp > PDX_DAC_AMP_MAX) amp = PDX_DAC_AMP_MAX;
    *r = (pdx_stream_res_t){0};
    if (sr < 100000u || sr > 1000000u) return "SR";
    const char *e = live_es_open(live_src);
    if (e) return e;
    e = ovl_prepare(live_src, live_es[live_src], true);          /* flash scritta solo se il testo è cambiato */
    if (e) return e;
    e = live_prepare(live_es[live_src], sr, "3/4", live_service, live_provider);
    if (e) return e;
    const uint32_t tot = 40000000u / sr;
    qsps = (tot % 16u == 0) ? 16u : (tot % 15u == 0) ? 15u : 8u;
    const uint32_t hold = tot / qsps;
    fx.sr = sr;
    fx.fr = 40000000u - qsps * hold * sr;
    fx.acc = 0; fx.rem = 0; fx.w = 0;
    if (fx.fr >= sr) return "SR non realizzabile";
    sym_qinv = qinv ? 1u : 0u;
    stream_kind = 3;
    tx_dry = dry != 0;
    pp_req = (pp_req_t){.mode = 10, .ms = ms, .n = 12000, .l = hold, .lat = lat, .amp = amp, .sres = r,
                        .caller = xTaskGetCurrentTaskHandle()};
    esp_wifi_set_promiscuous(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    xTaskNotifyGive(pp_task);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_wifi_set_promiscuous(true);
    stream_kind = 0;
    tx_dry = false;
    return NULL;
}

/* S2LTEST (senza RF): nframes frame del percorso a passi + mappatura pigra confrontati con pdx_s2_frame sullo stesso
 * DATA FIELD. Il riferimento lavora nel banco 1 (libero senza TX). */
const char *pdx_rf_s2ltest(uint32_t nframes, uint32_t *ok, uint32_t *bad_sym, uint32_t *steps_max_cyc)
{
    *ok = 0;
    *bad_sym = 0;
    *steps_max_cyc = 0;
    const char *e = live_es_open(0);
    if (e) return e;
    e = live_prepare(live_es[0], 333000u, "3/4", "IZ7PDX", "IZ7PDX BEACON C5");
    if (e) return e;
    /* il primo frame è già in cw[0]: rifà il percorso frame per frame salvando il DATA FIELD prima della codifica */
    uint8_t *field = (uint8_t *)0x40830000u;              /* banco 1, metà alta */
    uint8_t *ref = (uint8_t *)0x40832000u;
    for (uint32_t f = 0; f < nframes; f++) {
        lv.ready = 0;
        lv.cur ^= 1u;                                      /* il produttore scrive in cw[cur ^ 1] */
        uint8_t *cw = lv.cw[lv.cur ^ 1u];
        while (lv.pstage != 2) live_producer_step();       /* DATA FIELD pronto, job avviato */
        memcpy(field, cw + 10, lv.nb);
        const uint16_t syncd = lv.syncd;
        while (!lv.ready) {
            const uint32_t s0 = esp_cpu_get_cycle_count();
            live_producer_step();
            const uint32_t sc = esp_cpu_get_cycle_count() - s0;
            if (sc > *steps_max_cyc) *steps_max_cyc = sc;
        }
        /* riferimento: codificatore originale con lavoro nel banco 1 */
        pdx_s2_t rs;
        pdx_s2_work_reset();
        e = pdx_s2_init(&rs, "3/4", false, true);
        if (e) return e;
        memset(ref, 0, rs.nsym / 4u + 4u);
        pdx_s2_out_t out = {.buf = ref};
        pdx_s2_frame(&rs, field, syncd, &out);
        pdx_s2l_map_t m;
        pdx_s2l_map_start(&m, &live_s2, cw, live_s2.hdr);
        uint32_t bad = 0;
        for (uint32_t k = 0; k < rs.nsym; k++) {
            const uint32_t a = pdx_s2l_map_next(&m), b = (ref[k >> 2] >> ((k & 3u) * 2u)) & 3u;
            if (a != b) bad++;
        }
        if (!pdx_s2l_map_done(&m)) bad++;
        *bad_sym += bad;
        if (!bad) (*ok)++;
    }
    return NULL;
}

unsigned pdx_rf_capture(unsigned n, unsigned div, uint32_t *us)
{
    if (n < 16) n = 16;
    if (n > PDX_C5_BANK_WORDS) n = PDX_C5_BANK_WORDS;
    rx_idle();
    for (unsigned j = 0; j < n; j++) PDX_C5_BANK[j] = SENTINEL;
    const uint32_t owner = rd(PDX_C5_SRAM_OWNER);
    const int64_t t0 = esp_timer_get_time();
    adctrig(n - 1, 0, 0, div * 2, 0, 0, 0, 0, 0);       /* sorgente 0, nessun tono, nessuna trama */
    *us = (uint32_t)(esp_timer_get_time() - t0);
    wr(PDX_C5_SRAM_OWNER, owner);
    unsigned got = 0;
    for (unsigned j = 0; j < n; j++) got += PDX_C5_BANK[j] != SENTINEL;
    return got;
}
