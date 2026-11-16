/* ESP32-C5 Dual-Band DATV - IZ7PDX - GPL-3.0
 * Firmware di prova P2c: identificazione, commutatore d'antenna, cattura RX e prove di trasmissione.
 * I comandi che trasmettono (REFTONE, DACP) funzionano solo dopo TXARM; ogni impulso dura al massimo 2 s;
 * TXSAFE (o il riavvio) li disabilita di nuovo.
 *   TXARM / TXSAFE     abilita / disabilita i comandi che trasmettono
 *   REFTONE <backoff> <ms>                    portante della libreria sulla frequenza FREQ (backoff 0,25 dB)
 *   DACP <off_hex> <val_hex> <amp> <tone_khz> <ms>   prova del replay del DAC (registro 0x600A9000+off)
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

#define FW_VERSION "C5-P2g-SCAN"

static bool tx_armed;

extern int _bss_end, _data_end, _iram_end;

static inline uint32_t rd(uint32_t a) { return *(volatile uint32_t *)a; }

static void cmd_info(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint8_t mac[8] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);
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
    pdx_rf_pp_init();
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
        else if (!strcmp(cmd, "TXARM")) { tx_armed = true; printf("OK txarm: i comandi REFTONE/DACP trasmettono\n"); }
        else if (!strcmp(cmd, "TXSAFE")) { tx_armed = false; printf("OK txsafe\n"); }
        else if (!strcmp(cmd, "SCAN")) {
            /* SCAN <ctrl_hex> <idx_off_hex> <dt_cicli>: replay senza force_txon (nessuna TX), registri prima/durante */
            unsigned cv, io, dt;
            if (sscanf(p, "%*s %x %x %u", &cv, &io, &dt) != 3) { printf("ERR uso: SCAN <ctrl_hex> <idx_off_hex> <dt>\n"); continue; }
            uint32_t *a, *b, *t;
            pdx_rf_scan(cv, io, dt, &a, &b, &t);
            for (unsigned i = 0; i < 128; i++)
                if (a[i] != b[i]) printf("DIFF +%03x riposo %08" PRIx32 " corsa %08" PRIx32 "\n", 4 * i, a[i], b[i]);
            printf("SERIE +%03x ogni %u cicli:", io & 0x1FC, dt);
            for (unsigned i = 0; i < 16; i++) printf(" %08" PRIx32, t[i]);
            printf("\nOK scan\n");
        }
        else if (!strcmp(cmd, "ONESHOT")) {
            /* ONESHOT <ctrl_hex>: replay una volta senza TX, cicli fino al bit18 (5 ripetizioni) */
            unsigned cv;
            if (sscanf(p, "%*s %x", &cv) != 1) { printf("ERR uso: ONESHOT <ctrl_hex>\n"); continue; }
            printf("ONESHOT ctrl %08x:", cv);
            uint32_t ce = 0;
            for (int i = 0; i < 5; i++) printf(" %" PRIu32, pdx_rf_oneshot_cycles(cv, 2400000u, &ce));
            printf("  ctrl finale %08" PRIx32 "\nOK oneshot\n", ce);
        }
        else if (!strcmp(cmd, "HEAPINFO")) {
            heap_caps_print_heap_info(MALLOC_CAP_8BIT);
            printf("OK heapinfo\n");
        }
        else if (!strcmp(cmd, "PP")) {
            /* PP <mode 0=A 1=B 2=alterna> <ms> <period_us> <alloc 0/1> */
            if (!tx_armed) { printf("ERR non armato (TXARM)\n"); continue; }
            unsigned m, ms, per, al;
            if (sscanf(p, "%*s %u %u %u %u", &m, &ms, &per, &al) != 4) { printf("ERR uso: PP <mode> <ms> <period_us> <alloc>\n"); continue; }
            printf("TX PP %u MHz mode %u ms %u periodo %u us alloc %u\n", pdx_rf_mhz(), m, ms, per, al);
            pdx_pp_res_t r;
            pdx_rf_pp(m, ms, per, al, &r);
            printf("owner A %08" PRIx32 " B %08" PRIx32 " ctrl in corsa %08" PRIx32 " fine %08" PRIx32 " scambi %" PRIu32 "\nOK pp\n",
                   r.owner_a, r.owner_b, r.ctrl_running, r.ctrl_end, r.switches);
        }
        else if (!strcmp(cmd, "DPAT")) {
            /* DPAT <reg_off_hex> <and_hex> <or_hex> <amp> <n> <k> <ms>: come DACPAT, con un registro del blocco
             * 0x600A9xxx modificato (v = (v & and) | or) solo durante l'impulso e poi ripristinato */
            if (!tx_armed) { printf("ERR non armato (TXARM)\n"); continue; }
            unsigned roff, andm, orm, n, ms;
            int amp, kk;
            if (sscanf(p, "%*s %x %x %x %d %u %d %u", &roff, &andm, &orm, &amp, &n, &kk, &ms) != 7 || roff > 0xFFC) {
                printf("ERR uso: DPAT <off_hex> <and_hex> <or_hex> <amp> <n> <k> <ms>\n");
                continue;
            }
            const uint32_t ra = PDX_C5_DUMP_BASE + (roff & 0xFFCu);
            const uint32_t old = rd(ra);
            *(volatile uint32_t *)ra = (old & andm) | orm;
            const uint32_t now = rd(ra);
            printf("TX DPAT %u MHz reg %08" PRIx32 " %08" PRIx32 " -> %08" PRIx32 " amp %d n %u k %d ms %u\n", pdx_rf_mhz(),
                   ra, old, now, amp, n, kk, ms);
            pdx_dacp_res_t r;
            pdx_rf_dac_pattern(0x0C, 0x80000u | (n & 0x3FFFu), amp, n, kk, ms, &r);
            *(volatile uint32_t *)ra = old;
            printf("ripristinato %08" PRIx32 "\nOK dpat\n", rd(ra));
        }
        else if (!strcmp(cmd, "OWNTEST")) {
            uint32_t o[4];
            pdx_rf_owner_test(o);
            printf("OK owntest letto_col_motore %08" PRIx32 " dopo_scrittura %08" PRIx32 " dopo_rilascio %08" PRIx32
                   " owner %08" PRIx32 " (22222222 = scrittura arrivata, 11111111 = persa)\n", o[2], o[0], o[1], o[3]);
        }
        else if (!strcmp(cmd, "DACPAT")) {
            if (!tx_armed) { printf("ERR non armato (TXARM)\n"); continue; }
            unsigned off, val, n, ms;
            int amp, kk;
            if (sscanf(p, "%*s %x %x %d %u %d %u", &off, &val, &amp, &n, &kk, &ms) != 6) {
                printf("ERR uso: DACPAT <off_hex> <val_hex> <amp> <n> <k> <ms>\n");
                continue;
            }
            printf("TX DACPAT %u MHz reg 0x%08x val 0x%08x amp %d n %u k %d ms %u\n", pdx_rf_mhz(),
                   (unsigned)(PDX_C5_DUMP_BASE + (off & 0xFFCu)), val, amp, n, kk, ms);
            pdx_dacp_res_t r;
            pdx_rf_dac_pattern(off, val, amp, n, kk, ms, &r);
            printf("ctrl prima %08" PRIx32 " in corsa %08" PRIx32 " dopo %08" PRIx32 " DUMP+4 %08" PRIx32 " +8 %08" PRIx32
                   "\nOK dacpat\n", r.ctrl_before, r.ctrl_running, r.ctrl_after, r.dump4, r.dump8);
        }
        else if (!strcmp(cmd, "REFTONE") || !strcmp(cmd, "DACP")) {
            if (!tx_armed) { printf("ERR non armato (TXARM)\n"); continue; }
            if (!strcmp(cmd, "REFTONE")) {
                if (k < 3) { printf("ERR uso: REFTONE <backoff> <ms>\n"); continue; }
                printf("TX REFTONE %u MHz backoff %u ms %u\n", pdx_rf_mhz(), a, b);
                pdx_rf_ref_tone(a, b);
                printf("OK reftone\n");
            } else {
                unsigned off, val, ms;
                int amp, tkhz;
                if (sscanf(p, "%*s %x %x %d %d %u", &off, &val, &amp, &tkhz, &ms) != 5) {
                    printf("ERR uso: DACP <off_hex> <val_hex> <amp> <tone_khz> <ms>\n");
                    continue;
                }
                printf("TX DACP %u MHz reg 0x%08x val 0x%08x amp %d tono %d kHz ms %u\n", pdx_rf_mhz(),
                       (unsigned)(PDX_C5_DUMP_BASE + (off & 0xFFCu)), val, amp, tkhz, ms);
                pdx_dacp_res_t r;
                pdx_rf_dac_probe(off, val, amp, tkhz, 1000, ms, &r);
                printf("owner prima %08" PRIx32 " ctrl prima %08" PRIx32 " in corsa %08" PRIx32 " dopo %08" PRIx32
                       " DUMP+4 %08" PRIx32 " +8 %08" PRIx32 " campioni %" PRIu32 "\nOK dacp\n",
                       r.owner_before, r.ctrl_before, r.ctrl_running, r.ctrl_after, r.dump4, r.dump8, r.samples);
            }
        }
        else if (!strcmp(cmd, "REG")) {
            uint32_t addr = (uint32_t)strtoul(p + 3, NULL, 16);
            if ((addr >> 20) == 0x600 || (addr >> 20) == 0x408) printf("OK reg %08" PRIx32 " = %08" PRIx32 "\n", addr, rd(addr & ~3u));
            else printf("ERR reg\n");
        } else printf("ERR comando\n");
    }
}
