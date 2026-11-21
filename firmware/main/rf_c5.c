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

#include "esp_event.h"
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
    pdx_rf_tune(2412);
    uint32_t us;
    pdx_rf_capture(256, 0, &us);              /* configura il blocco dump (serve anche al replay del DAC) */
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

bool pdx_rf_tune(unsigned mhz)
{
    if (mhz < 2300 || (mhz > 2500 && mhz < 4900) || mhz > 5900) return false;
    phy_set_chanfreq(mhz, 0);
    rx_idle();
    cur_mhz = mhz;
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
    pdx_rf_tune(cur_mhz);
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
    pdx_rf_tune(cur_mhz);
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
static IRAM_ATTR __attribute__((noinline)) bool live_producer_step(void)
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
        if ((j & 255u) == 0 && !(REG_READ(GPIO_IN_REG) & (1u << 28))) { stream_res.stopped = 1; intervals = j; break; }  /* BOOT */
        while ((int32_t)(esp_cpu_get_cycle_count() - tsw) < 0) {}
        const uint32_t late = esp_cpu_get_cycle_count() - tsw;
        *own = (j & 1u) ? oa : ob;                                  /* j pari -> B, dispari -> A */
        if (late > stream_res.late_max) stream_res.late_max = late;
        const uint32_t f0 = esp_cpu_get_cycle_count();
        any_fill((j & 1u) ? PDX_PP_B : PDX_PP_A, (int32_t)j + 1, n, l);      /* banco appena liberato: intervallo j+1 */
        const uint32_t fc = esp_cpu_get_cycle_count() - f0;
        if (fc > stream_res.fill_max) stream_res.fill_max = fc;
        if (stream_kind == 3) {                                   /* tempi morti: passi del produttore DVB-S2 */
            const uint32_t dl = tsw + per - 14000u;               /* margine 58 us prima dello scambio */
            while ((int32_t)(esp_cpu_get_cycle_count() - dl) < 0) {
                const uint32_t ph = lv.pstage < 2u ? lv.pstage : 2u + (live_job.stage < 5u ? live_job.stage : 5u);
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
    pdx_rf_tune(cur_mhz);
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
        pdx_rf_tune(cur_mhz);
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
#define LIVE_ES_PART_OFF 0x180000u            /* file PDXES001 nella partizione "symbols" (assoluto 0x290000) */
#define LIVE_ES_MAP      0x170000u
static const uint8_t *live_es;
static esp_partition_mmap_handle_t live_es_mh;

static const char *live_es_open(void)
{
    if (live_es) return NULL;
    const esp_partition_t *pt = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "symbols");
    if (!pt) return "partizione symbols";
    const void *ptr;
    if (esp_partition_mmap(pt, LIVE_ES_PART_OFF, LIVE_ES_MAP, ESP_PARTITION_MMAP_DATA, &ptr, &live_es_mh) != ESP_OK)
        return "mmap ES";
    if (memcmp(ptr, "PDXES001", 8) != 0) { esp_partition_munmap(live_es_mh); return "file ES assente"; }
    live_es = ptr;
    return NULL;
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
    if (ms > PDX_S2_MS_MAX) ms = PDX_S2_MS_MAX;
    if (amp > PDX_DAC_AMP_MAX) amp = PDX_DAC_AMP_MAX;
    *r = (pdx_stream_res_t){0};
    if (sr < 100000u || sr > 1000000u) return "SR";
    const char *e = live_es_open();
    if (e) return e;
    e = live_prepare(live_es, sr, "3/4", "IZ7PDX", "IZ7PDX BEACON C5");
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
    const char *e = live_es_open();
    if (e) return e;
    e = live_prepare(live_es, 333000u, "3/4", "IZ7PDX", "IZ7PDX BEACON C5");
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
