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
