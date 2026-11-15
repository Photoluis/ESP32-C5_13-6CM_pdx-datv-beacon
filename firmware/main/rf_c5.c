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
