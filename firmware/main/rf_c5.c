/* ESP32-C5 Dual-Band DATV - IZ7PDX - GPL-3.0
 *
 * Ricezione con il motore di cattura della radio dell'ESP32-C5 (P2b, nessuna trasmissione).
 * Mappa dei registri ricavata dal disassemblato di librftest.a/libphy.a (ESP-IDF v6.1, esp32c5) e confrontata con il
 * progetto ESP-SDR di ESPARGOS (GPL-3.0, main/targets/esp32c5): adctrig(n-1, 0, 0, div*2, 0, 0, 0, 0, 0) con
 * sorgente 0 e senza tono, banco 0x40830000, proprietario 0x60095004 = 0x10200 durante la cattura.
 * Codice scritto da zero.
 */
#include "rf_c5.h"

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

void pdx_rf_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));   /* nessuna trama trasmessa dal driver */
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    pdx_rf_tune(2412);
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
#include "esp_cpu.h"
#include "esp_phy_cert_test.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern void force_txon(int on);
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
static int16_t tone_cos[TONE_LUT];

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
    force_txon(1);
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
    force_txon(0);
    phy_txcal_work_mode();
    pdx_rf_tune(cur_mhz);
}

static void replay_run(uint32_t off, uint32_t val, unsigned ms, pdx_dacp_res_t *r)
{
    const uint32_t ctrl = PDX_C5_DUMP_BASE + (off & 0xFFCu);
    r->owner_before = rd(PDX_C5_SRAM_OWNER);
    r->ctrl_before = rd(ctrl);
    force_txon(1);
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
    force_txon(0);
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
#define SCAN_N 128u
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
SOC_RESERVE_MEMORY_REGION(0x40840000, 0x40850000, c5_rf_bank2_lo);

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
#define STREAM_L_MAX 256u
static uint32_t stream_tab[2 * STREAM_L_MAX];
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
#define QPSK_SPS 15u
static uint16_t qpsk_t[QPSK_SPS][256];
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
        s += sym * rrc_f((float)p / QPSK_SPS + (float)m - 3.5f, 0.35f);
    }
    return s;
}

static void qpsk_build(int amp)                 /* due passate: niente tabella float da 15 KB nel banco 0 */
{
    float vmax = 0;
    for (unsigned p = 0; p < QPSK_SPS; p++)
        for (unsigned h = 0; h < 256; h++) {
            const float s = fabsf(qpsk_sum(p, h));
            if (s > vmax) vmax = s;
        }
    for (unsigned p = 0; p < QPSK_SPS; p++)
        for (unsigned h = 0; h < 256; h++) qpsk_t[p][h] = (uint16_t)((int)lroundf(qpsk_sum(p, h) / vmax * amp) & 0x3FF);
}

static IRAM_ATTR __attribute__((noinline)) void qpsk_gen(uint32_t *d, uint32_t cnt)
{
    uint32_t p = qg.p, hi = qg.hi, hq = qg.hq, lfsr = qg.lfsr;
    const uint32_t hold = qg.hold;
    uint32_t *const end = d + cnt;
    while (d < end) {
        const uint32_t w = (uint32_t)qpsk_t[p][hi] | ((uint32_t)qpsk_t[p][hq] << 10);
        if (hold == 8) {
            d[0] = w; d[1] = w; d[2] = w; d[3] = w; d[4] = w; d[5] = w; d[6] = w; d[7] = w;
            d += 8;
        } else {
            for (uint32_t k = 0; k < hold; k++) *d++ = w;
        }
        if (++p == QPSK_SPS) {
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

/* banco dell'intervallo j: prima la metà alta (suona prima), poi la bassa */
static IRAM_ATTR __attribute__((noinline)) void qpsk_fill(volatile uint32_t *dst, int32_t j, uint32_t n, uint32_t l)
{
    (void)j; (void)l;
    uint32_t *d = (uint32_t *)dst;
    qpsk_gen(d + n / 2u, n - n / 2u);
    qpsk_gen(d, n / 2u);
}

static IRAM_ATTR __attribute__((noinline)) void any_fill(volatile uint32_t *dst, int32_t j, uint32_t n, uint32_t l)
{
    if (stream_kind == 1) qpsk_fill(dst, j, n, l);
    else stream_fill(dst, j, n, l);
}

static IRAM_ATTR __attribute__((noinline)) void stream_loop(uint32_t n, uint32_t l, uint32_t intervals, uint32_t lat,
                                                            uint32_t oa, uint32_t ob, uint32_t t0)
{
    volatile uint32_t *own = (volatile uint32_t *)PDX_C5_SRAM_OWNER;
    const uint32_t per = n * 6u;                                    /* 40 MS/s = 6 cicli per campione */
    uint32_t tsw = t0 + lat + (n / 2u) * 6u;                        /* primo scambio: intervallo 0 */
    for (uint32_t j = 0; j < intervals; j++) {
        while ((int32_t)(esp_cpu_get_cycle_count() - tsw) < 0) {}
        const uint32_t late = esp_cpu_get_cycle_count() - tsw;
        *own = (j & 1u) ? oa : ob;                                  /* j pari -> B, dispari -> A */
        if (late > stream_res.late_max) stream_res.late_max = late;
        const uint32_t f0 = esp_cpu_get_cycle_count();
        any_fill((j & 1u) ? PDX_PP_B : PDX_PP_A, (int32_t)j + 1, n, l);      /* banco appena liberato: intervallo j+1 */
        const uint32_t fc = esp_cpu_get_cycle_count() - f0;
        if (fc > stream_res.fill_max) stream_res.fill_max = fc;
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
    if (stream_kind == 1) {
        qpsk_build(q->amp);
        qg.p = 0; qg.hi = 0; qg.hq = 0; qg.lfsr = 0x5A5A5Au; qg.hold = l;   /* per il QPSK l = H */
        qpsk_gen((uint32_t *)PDX_PP_A, n / 2u);       /* intervallo -1: suona solo la metà bassa di A */
        qpsk_fill(PDX_PP_B, 0, n, l);
    } else {
        stream_fill(PDX_PP_A, -1, n, l);
        stream_fill(PDX_PP_B, 0, n, l);
    }
    const uint32_t intervals = (uint32_t)((uint64_t)q->ms * 40000u / n);
    force_txon(1);
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
    force_txon(0);
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
        force_txon(1);
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
        force_txon(0);
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
    if (n > 16000) n = 16000;
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
    if (n > 16000) n = 16000;
    stream_kind = 1;
    pp_req = (pp_req_t){.mode = 10, .ms = ms, .n = n, .l = hold, .lat = lat, .amp = amp, .sres = r,
                        .caller = xTaskGetCurrentTaskHandle()};
    esp_wifi_set_promiscuous(false);
    vTaskDelay(pdMS_TO_TICKS(20));
    xTaskNotifyGive(pp_task);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_wifi_set_promiscuous(true);
    stream_kind = 0;
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
