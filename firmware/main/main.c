/* ESP32-C5 Dual-Band DATV - IZ7PDX - GPL-3.0
 * Firmware di prova P2b: identificazione, commutatore d'antenna e cattura RX. NESSUN comando trasmette.
 *
 * Comandi (USB Serial/JTAG, una riga ciascuno):
 *   INFO               chip, memoria, registri del motore di cattura
 *   ANT <0|1>          livello di GPIO26 (commutatore ceramica/IPEX) e rilettura
 *   FREQ <MHz>         sintonia del PLL (2300..2500, 4900..5900)
 *   RXCAP <n> [div]    cattura n parole nel banco 0x40830000 e stampa il riepilogo
 *   RXDUMP <n>         stampa le prime n parole dell'ultima cattura in esadecimale (righe "D ...", poi "OK")
 *   REG <hex>          legge un registro a 32 bit (solo 0x600xxxxx e 0x408xxxxx)
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rf_c5.h"

#define FW_VERSION "C5-P2b-RX"

extern int _bss_end, _data_end, _iram_end;

static inline uint32_t rd(uint32_t a) { return *(volatile uint32_t *)a; }

static void cmd_info(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    printf("FW %s  chip model %d rev %d cores %d  MAC %02x:%02x:%02x:%02x:%02x:%02x\n", FW_VERSION, ci.model,
           ci.revision, ci.cores, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("heap libero %u  min %u  _iram_end %p  _data_end %p  _bss_end %p  (limite 0x40820000)\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
           (void *)&_iram_end, (void *)&_data_end, (void *)&_bss_end);
    printf("SRAM_OWNER 0x%08" PRIx32 "\n", rd(PDX_C5_SRAM_OWNER));
    for (uint32_t o = 0; o <= 0x1C; o += 4) printf("DUMP+%02" PRIx32 " 0x%08" PRIx32 "\n", o, rd(PDX_C5_DUMP_BASE + o));
    printf("FREQ %u MHz  ANT(GPIO26) %d\n", pdx_rf_mhz(), gpio_get_level(PDX_C5_ANT_GPIO));
    printf("OK info\n");
}

static void cmd_ant(int v)
{
    gpio_set_level(PDX_C5_ANT_GPIO, v ? 1 : 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    printf("OK ant %d letto %d\n", v ? 1 : 0, gpio_get_level(PDX_C5_ANT_GPIO));
}

static unsigned last_n;

static void cmd_rxcap(unsigned n, unsigned div)
{
    uint32_t us;
    const unsigned got = pdx_rf_capture(n, div, &us);
    last_n = n > PDX_C5_BANK_WORDS ? PDX_C5_BANK_WORDS : n;
    /* riepilogo grezzo: il formato della parola si ricava sul PC; qui due ipotesi (16+16 e 10+10 bit) */
    int64_t s16i = 0, s16q = 0, p16 = 0;
    for (unsigned j = 0; j < last_n; j++) {
        const uint32_t w = PDX_C5_BANK[j];
        const int16_t i = (int16_t)(w & 0xFFFF), q = (int16_t)(w >> 16);
        s16i += i; s16q += q; p16 += (int32_t)i * i + (int32_t)q * q;
    }
    printf("RX %u MHz  n %u  scritte %u  durata %" PRIu32 " us  owner 0x%08" PRIx32 "\n", pdx_rf_mhz(), last_n, got, us,
           rd(PDX_C5_SRAM_OWNER));
    printf("medie16 I %lld Q %lld  potenza16 %lld\n", s16i / (int64_t)last_n, s16q / (int64_t)last_n, p16 / (int64_t)last_n);
    printf("prime parole:");
    for (unsigned j = 0; j < 8; j++) printf(" %08" PRIx32, PDX_C5_BANK[j]);
    printf("\nOK rxcap\n");
}

static void cmd_rxdump(unsigned n)
{
    if (n > last_n) n = last_n;
    for (unsigned j = 0; j < n; j += 8) {
        printf("D");
        for (unsigned k = j; k < j + 8 && k < n; k++) printf(" %08" PRIx32, PDX_C5_BANK[k]);
        printf("\n");
        if ((j & 0x3FF) == 0) vTaskDelay(1);
    }
    printf("OK rxdump %u\n", n);
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t uc = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&uc));
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);

    gpio_config_t g = {.pin_bit_mask = 1ULL << PDX_C5_ANT_GPIO, .mode = GPIO_MODE_INPUT_OUTPUT};
    gpio_config(&g);
    gpio_set_level(PDX_C5_ANT_GPIO, 0);   /* stato di riposo della scheda (pull-down R17) */

    pdx_rf_init();
    printf("\nPDX ESP32-C5 %s pronto (nessun comando trasmette)\n", FW_VERSION);

    char line[96];
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        char *p = line + strspn(line, " \r\n");
        char cmd[16] = {0};
        unsigned a = 0, b = 0;
        const int k = sscanf(p, "%15s %u %u", cmd, &a, &b);
        if (k < 1) continue;
        for (char *c = cmd; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        if (!strcmp(cmd, "INFO")) cmd_info();
        else if (!strcmp(cmd, "ANT") && k >= 2) cmd_ant((int)a);
        else if (!strcmp(cmd, "FREQ") && k >= 2) printf(pdx_rf_tune(a) ? "OK freq %u\n" : "ERR freq %u\n", a);
        else if (!strcmp(cmd, "RXCAP") && k >= 2) cmd_rxcap(a, k >= 3 ? b : 0);
        else if (!strcmp(cmd, "RXDUMP") && k >= 2) cmd_rxdump(a);
        else if (!strcmp(cmd, "REG")) {
            uint32_t addr = (uint32_t)strtoul(p + 3, NULL, 16);
            if ((addr >> 20) == 0x600 || (addr >> 20) == 0x408) printf("OK reg %08" PRIx32 " = %08" PRIx32 "\n", addr, rd(addr & ~3u));
            else printf("ERR reg\n");
        } else printf("ERR comando\n");
    }
}
