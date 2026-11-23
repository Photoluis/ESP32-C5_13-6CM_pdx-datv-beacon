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
#include <math.h>
#include "beacon.h"
#include "web_c5.h"
#include "led_status.h"
#include "rf_c5.h"

#define FW_VERSION "C5-B4-CW"

static bool tx_armed;

/* fine della CW (dal task della radio): uscite a riposo, LED giallo */
static const pdx_bcn_cfg_t *cw_cfg;
static int cw_band;
static void cw_end(const char *why)
{
    if (cw_cfg) pdx_bcn_route(cw_cfg, cw_band, false);
    pdx_led_idle();
    printf("CW fine: %s\n", why);
}

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
    printf("FREQ %u MHz  PLL %" PRIu32 " kHz  ANT(GPIO26) %d\n", pdx_rf_mhz(), pdx_rf_prog_khz(), gpio_get_level(PDX_C5_ANT_GPIO));
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
    gpio_config_t gb = {.pin_bit_mask = 1ULL << 28, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&gb);                      /* tasto BOOT = stop della TX lunga */

    if (pdx_rf_init() != ESP_OK) printf("ATTENZIONE: radio non inizializzata, solo comandi di servizio\n");
    pdx_rf_pp_init();
    pdx_led_init();                          /* giallo fisso: in attesa */

    /* B1: beacon autonomo. Doppia accensione (spegnere e riaccendere entro 5 s) = modo configurazione, niente TX */
    static pdx_bcn_cfg_t bcfg;
    pdx_bcn_load(&bcfg);
    pdx_web_init(&bcfg, FW_VERSION);
    pdx_bcn_apply(&bcfg);                                /* GPIO STOP (a massa = beacon fermo) e correzione del quarzo */
    int rb = 0;
    uint32_t rc = 1;
    const int bm = pdx_bcn_boot_mode(&rb, &rc);
    if (bm == PDX_BCN_BOOT_RUN) {                        /* TX di una banda (radio pulita dal riavvio) */
        pdx_bcn_segment(&bcfg, rb, rc);                   /* non ritorna: riavvia in PAUSA o CONFIG */
    } else if (bm == PDX_BCN_BOOT_PAUSE) {               /* pausa con la dashboard */
        pdx_bcn_pause(&bcfg, rb, rc);                     /* ritorna solo se fermato: si resta in configurazione */
    } else if (bm == PDX_BCN_BOOT_CONFIG) {
        printf("BEACON modo configurazione\n");
    } else {
        const bool quick = pdx_bcn_quick_boot();
        pdx_bcn_print(&bcfg);
        if (quick) {
            printf("BEACON doppia accensione: modo configurazione (nessuna TX)\n");
            pdx_bcn_quick_clear();
        } else if (pdx_bcn_stop_gpio_active()) {
            printf("BEACON GPIO%d STOP a massa: modo configurazione (nessuna TX)\n", bcfg.gpio_stop);
        } else if (bcfg.auto_tx) {
            pdx_web_set_state("ATTESA prima del beacon");
            pdx_web_start();                              /* la dashboard c'è già durante l'attesa */
            if (pdx_bcn_wait(bcfg.wait_s) && !pdx_web_take_stop()) pdx_bcn_request_run(0, 1);
        } else {
            printf("BEACON partenza automatica OFF (SET auto 1, SAVE per abilitarla)\n");
        }
    }
    /* modo configurazione: dashboard sempre accesa (radio pulita: nessuna sintonia fatta in questo avvio) */
    if (bm == PDX_BCN_BOOT_LAB) printf("LAB: access point spento, comandi di prova disponibili (riavvio = dashboard)\n");
    else { pdx_web_set_state("CONFIG"); pdx_web_start(); }
    printf("\nPDX ESP32-C5 %s pronto (nessun comando trasmette)\n", FW_VERSION);

    char line[96];
    bool led_red = false;
    for (;;) {
        if (led_red) { pdx_led_idle(); led_red = false; }
        if (!fgets(line, sizeof line, stdin)) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        char *p = line + strspn(line, " \r\n");
        char cmd[16] = {0};
        unsigned a = 0, b = 0;
        const int k = sscanf(p, "%15s %u %u", cmd, &a, &b);
        if (k < 1) continue;
        for (char *c = cmd; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        {
            static const char *txc[] = {"REFTONE", "DACP", "DACPAT", "DPAT", "PP", "STREAM", "QPSK", "S2PLAY",
                                        "S2LIVE"};
            for (unsigned i = 0; i < sizeof txc / sizeof txc[0]; i++)
                if (tx_armed && !strcmp(cmd, txc[i])) {
                    int amp = 250;
                    sscanf(p, "%*s %*s %d", &amp);         /* il livello decide la velocita' del lampeggio */
                    if (!strcmp(cmd, "S2PLAY") || !strcmp(cmd, "QPSK") || !strcmp(cmd, "REFTONE")) amp = 250;
                    if (amp < 5) amp = 5;
                    pdx_led_tx(20.0 * log10((double)amp / 480.0));
                    led_red = true;
                }
        }
        if (pdx_rf_cw_active() && strcmp(cmd, "CW") && strcmp(cmd, "CWDRY") && strcmp(cmd, "INFO")) { printf("ERR cw in corso: CW OFF prima\n"); continue; }
        if (!strcmp(cmd, "INFO")) cmd_info();
        else if (!strcmp(cmd, "ANT") && k >= 2) cmd_ant((int)a);
        else if (!strcmp(cmd, "FREQ") && k >= 2) printf(pdx_rf_tune(a) ? "OK freq %u\n" : "ERR freq %u\n", a);
        else if (!strcmp(cmd, "FREQK") && k >= 2) {     /* FREQK <kHz>: frequenza vera, PLL corretto dal quarzo (nessuna TX) */
            if (pdx_rf_tune_khz(a)) printf("OK freqk %u PLL %" PRIu32 " kHz\n", a, pdx_rf_prog_khz());
            else printf("ERR freqk %u\n", a);
        }
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
            for (unsigned i = 0; i < 16; i++)
                if (a[i] != b[i]) printf("DIFF +%03x riposo %08" PRIx32 " corsa %08" PRIx32 "\n", 4 * i, a[i], b[i]);
            printf("SERIE +%03x ogni %u cicli:", io & 0x1FC, dt);
            for (unsigned i = 0; i < 16; i++) printf(" %08" PRIx32, t[i]);
            printf("\nOK scan\n");
        }
        else if (!strcmp(cmd, "STREAM")) {
            /* STREAM <n> <l> <amp> <ms> <lat_cicli>: tono fs/l generato in tempo reale, ping-pong a 40 MS/s */
            if (!tx_armed) { printf("ERR non armato (TXARM)\n"); continue; }
            unsigned n, l, ms, lat;
            int amp;
            if (sscanf(p, "%*s %u %u %d %u %u", &n, &l, &amp, &ms, &lat) != 5) { printf("ERR uso: STREAM <n> <l> <amp> <ms> <lat>\n"); continue; }
            printf("TX STREAM %u MHz n %u l %u amp %d ms %u lat %u\n", pdx_rf_mhz(), n, l, amp, ms, lat);
            pdx_stream_res_t r;
            pdx_rf_stream(n, l, amp, ms, lat, &r);
            printf("intervalli %" PRIu32 " riempimento max %" PRIu32 " cicli (budget %u) ritardo scambio max %" PRIu32
                   " sforamenti %" PRIu32 "\nOK stream\n", r.intervals, r.fill_max, (n & ~1u) * 6u, r.late_max, r.overruns);
        }
        else if (!strcmp(cmd, "WEBTEST")) { pdx_web_selftest(); printf("OK webtest\n"); }
        else if (!strcmp(cmd, "OVLTEST")) {
            /* OVLTEST <banda 0=6cm 1=13cm> <nominativo> <MHz>: compone in flash le strisce del monoscopio, nessuna TX */
            unsigned kb = 0;
            char call[16] = {0}, mhz[12] = {0}, ftxt[20];
            if (sscanf(p, "%*s %u %15s %11s", &kb, call, mhz) != 3) { printf("ERR uso: OVLTEST <0|1> <call> <MHz>\n"); continue; }
            snprintf(ftxt, sizeof ftxt, "%s MHz", mhz);
            const char *oe = pdx_rf_ovl_test((int)kb, call, ftxt);
            if (oe) printf("ERR ovltest: %s\n", oe); else printf("OK ovltest\n");
        }
        else if (!strcmp(cmd, "LAB")) pdx_bcn_reboot(PDX_BCN_BOOT_LAB, 0, 1);
        else if (!strcmp(cmd, "CONFIG")) { pdx_bcn_print(&bcfg); printf("OK config\n"); }
        else if (!strcmp(cmd, "SET")) {
            char key[16] = {0}, val[40] = {0};
            pdx_bcn_cfg_t t = bcfg;
            const char *cf = NULL;
            if (sscanf(p, "%*s %15s %39s", key, val) == 2 && pdx_bcn_set(&t, key, val) && !(cf = pdx_bcn_check(&t))) {
                bcfg = t;
                pdx_bcn_apply(&bcfg);
                pdx_bcn_print(&bcfg);
                printf("OK set (SAVE per salvare)\n");
            }
            else if (cf) printf("ERR %s: GPIO gia' usato da un'altra funzione\n", cf);
            else printf("ERR uso: SET call|appass|f13|f6|sr|amp|tx|pause|wait|bands|auto|ant13|ant6|gpio13|gpio6|gpioptt|gpiostop|gpiopol|ppm valore\n");
        }
        else if (!strcmp(cmd, "SAVE")) printf(pdx_bcn_save(&bcfg) ? "OK save\n" : "ERR save\n");
        else if (!strcmp(cmd, "BEACON")) {
            if (!tx_armed) { printf("ERR non armato (TXARM)\n"); continue; }
            pdx_bcn_request_run(0, 1);                    /* riavvio: TX con la radio pulita */
        }
        else if (!strcmp(cmd, "S2LTEST")) {
            /* S2LTEST <frame>: percorso a passi + mappatura pigra contro il codificatore di riferimento. Nessuna RF. */
            uint32_t ok, bad, smax;
            const char *e = pdx_rf_s2ltest(k >= 2 ? a : 5, &ok, &bad, &smax);
            if (e) printf("ERR s2ltest %s\n", e);
            else printf("OK s2ltest frame uguali %" PRIu32 "/%u simboli diversi %" PRIu32 " passo max %" PRIu32 " cicli\n",
                        ok, k >= 2 ? a : 5, bad, smax);
        }
        else if (!strcmp(cmd, "S2LIVE") || !strcmp(cmd, "S2LDRY")) {
            /* S2LIVE <sr> <amp> <ms> [lat] [qinv]: TS continuo generato sulla C5, stop col tasto BOOT.
             * S2LDRY: stessa cosa a secco (motore del DAC in corsa, nessuna RF), per misurare il produttore. */
            const bool dry = !strcmp(cmd, "S2LDRY");
            if (!dry && !tx_armed) { printf("ERR non armato (TXARM)\n"); continue; }
            unsigned sr, ms, lat = 50, qinv = 0;
            int amp;
            if (sscanf(p, "%*s %u %d %u %u %u", &sr, &amp, &ms, &lat, &qinv) < 3) { printf("ERR uso: S2LIVE <sr> <amp> <ms> [lat] [qinv]\n"); continue; }
            printf("%s S2LIVE %u MHz SR %u amp %d ms %u (stop: BOOT)\n", dry ? "A SECCO" : "TX", pdx_rf_mhz(), sr, amp, ms);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(50));
            pdx_stream_res_t r;
            const char *e = pdx_rf_s2live(sr, amp, ms, lat, qinv, dry, &r);
            if (e) { printf("ERR s2live %s\n", e); continue; }
            pdx_live_stats_t st;
            pdx_rf_live_stats(&st);
            printf("intervalli %" PRIu32 " riempimento max %" PRIu32 " ritardo max %" PRIu32 " sforamenti %" PRIu32
                   " fermato %" PRIu32 "\n", r.intervals, r.fill_max, r.late_max, r.overruns, r.stopped);
            printf("frame %" PRIu32 " DUMMY %" PRIu32 " passi %" PRIu32 " passo max %" PRIu32 " cicli TS %.0f bit/s "
                   "pacchetti nulli %" PRIu32 " video %" PRIu32 " audio %" PRIu32 "\n", st.frames, st.dummies,
                   st.steps, st.step_max, st.ts_rate, st.null_pk, st.video_pk, st.audio_pk);
            static const char *ph[8] = {"campo", "pacchetti", "scrambling", "BCH", "LDPC", "parita'", "xor", "-"};
            for (int i = 0; i < 7; i++)
                if (st.st_n[i]) printf("fase %-10s passi %7" PRIu32 " max %6" PRIu32 " cicli  totale %8" PRIu32 " kcicli\n",
                                       ph[i], st.st_n[i], st.st_max[i], st.st_kcyc[i]);
            printf("OK s2live\n");
        }
        else if (!strcmp(cmd, "S2TEST")) {
            /* codificatore DVB-S2 della C5 contro encoder.py (42 casi a 0x310000), con i tempi per frame. Nessuna RF. */
            extern int pdx_s2_selftest(void);
            pdx_s2_selftest();
            printf("OK s2test\n");
        }
        else if (!strcmp(cmd, "SYMINFO")) {
            uint32_t sr;
            char lab[41];
            const uint32_t n = pdx_rf_sym_open(&sr, lab);
            if (n) printf("OK syminfo %" PRIu32 " simboli SR %" PRIu32 " = %.2f s  \"%s\"\n", n, sr, (double)n / sr, lab);
            else printf("ERR syminfo: partizione symbols assente o senza intestazione PDXSYM01\n");
        }
        else if (!strcmp(cmd, "S2PLAY")) {
            /* S2PLAY <amp> <ms> [lat] [qinv]: DVB-S2 dalla flash, stop anche col tasto BOOT */
            if (!tx_armed) { printf("ERR non armato (TXARM)\n"); continue; }
            unsigned ms, lat = 50, qinv = 0;
            int amp;
            if (sscanf(p, "%*s %d %u %u %u", &amp, &ms, &lat, &qinv) < 2) { printf("ERR uso: S2PLAY <amp> <ms> [lat] [qinv]\n"); continue; }
            uint32_t sr;
            char lab[41];
            if (!pdx_rf_sym_open(&sr, lab)) { printf("ERR simboli assenti\n"); continue; }
            printf("TX S2PLAY %u MHz amp %d ms %u lat %u qinv %u \"%s\" (stop: BOOT)\n", pdx_rf_mhz(), amp, ms, lat, qinv, lab);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(50));
            pdx_stream_res_t r;
            pdx_rf_s2play(amp, ms, lat, qinv, &r);
            printf("intervalli %" PRIu32 " riempimento max %" PRIu32 " cicli ritardo scambio max %" PRIu32
                   " sforamenti %" PRIu32 " fermato %" PRIu32 "\nOK s2play\n", r.intervals, r.fill_max, r.late_max,
                   r.overruns, r.stopped);
        }
        else if (!strcmp(cmd, "QPSK")) {
            /* QPSK <hold 4/8/16> <amp> <ms> <lat> [n]: RRC 0,35, simboli PRBS, SR = 40e6/(15*hold) */
            if (!tx_armed) { printf("ERR non armato (TXARM)\n"); continue; }
            unsigned hold, ms, lat, n = 16000;
            int amp;
            if (sscanf(p, "%*s %u %d %u %u %u", &hold, &amp, &ms, &lat, &n) < 4) { printf("ERR uso: QPSK <hold> <amp> <ms> <lat> [n]\n"); continue; }
            printf("TX QPSK %u MHz SR %.1f kS amp %d ms %u lat %u n %u\n", pdx_rf_mhz(), 40000.0 / (15.0 * hold), amp, ms, lat, n);
            pdx_stream_res_t r;
            pdx_rf_qpsk(n, hold, amp, ms, lat, &r);
            printf("intervalli %" PRIu32 " riempimento max %" PRIu32 " cicli ritardo scambio max %" PRIu32
                   " sforamenti %" PRIu32 "\nOK qpsk\n", r.intervals, r.fill_max, r.late_max, r.overruns);
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
        else if (!strcmp(cmd, "CW") || !strcmp(cmd, "CWDRY")) {
            const bool cwdry = !strcmp(cmd, "CWDRY");   /* stessa cosa senza RF (force_txon spento) */
            /* CW ON <13|6> [amp] [s] | CW MORSE <13|6> <wpm> <pausa_s> <portante_s> <testo> | CW OFF |
             * CW AMP <1..480> | CW KEY DAC|PA | CW (stato). Frequenza, quarzo, antenna, GPIO e PTT dalla configurazione. */
            char sub[12] = {0}, rest[64] = {0};
            sscanf(p, "%*s %11s %63[^\r\n]", sub, rest);
            for (char *c = sub; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
            if (!sub[0]) {
                printf("OK cw %s amp %d manipolazione %s\n", pdx_rf_cw_active() ? "IN CORSO" : "fermo", pdx_rf_cw_amp(),
                       pdx_rf_cw_key_pa() ? "PA" : "DAC");
            } else if (!strcmp(sub, "OFF")) {
                if (pdx_rf_cw_active()) pdx_rf_cw_stop();
                printf("OK cw off\n");
            } else if (!strcmp(sub, "AMP")) {
                pdx_rf_cw_set_amp(atoi(rest));
                printf("OK cw amp %d (%.1f dB)\n", pdx_rf_cw_amp(), 20.0 * log10(pdx_rf_cw_amp() / 480.0));
            } else if (!strcmp(sub, "KEY")) {
                pdx_rf_cw_set_key_pa(rest[0] == 'p' || rest[0] == 'P');
                printf("OK cw key %s\n", pdx_rf_cw_key_pa() ? "PA" : "DAC");
            } else if (!strcmp(sub, "ON") || !strcmp(sub, "MORSE")) {
                if (!tx_armed && !cwdry) { printf("ERR non armato (TXARM)\n"); continue; }
                if (pdx_rf_cw_active()) { printf("ERR cw gia' in corso (CW OFF)\n"); continue; }
                if (pdx_bcn_stop_gpio_active()) { printf("ERR GPIO STOP a massa\n"); continue; }
                const bool morse = !strcmp(sub, "MORSE");
                unsigned band = 0, x1 = 0, x2 = 0, x3 = 0;
                int nch = 0;
                const int n = sscanf(rest, "%u %u %u %u %n", &band, &x1, &x2, &x3, &nch);
                if (n < 1 || (band != 13 && band != 6) || (morse && (n < 4 || !rest[nch]))) {
                    printf("ERR uso: CW ON <13|6> [amp] [s]  |  CW MORSE <13|6> <wpm> <pausa_s> <portante_s> <testo>\n");
                    continue;
                }
                pdx_cw_job_t job = {0};
                job.dry = cwdry;
                if (morse) {
                    job.wpm = x1; job.pause_s = x2; job.carrier_s = x3; job.max_s = PDX_CW_MAX_S;
                    snprintf(job.text, sizeof job.text, "%s", rest + nch);
                } else {
                    if (n >= 2 && x1) pdx_rf_cw_set_amp((int)x1);
                    job.max_s = n >= 3 && x2 ? x2 : 600;
                }
                const int b = band == 13 ? 0 : 1;
                const uint32_t fk = b == 0 ? bcfg.f13_khz : bcfg.f6_khz;
                pdx_rf_set_ppm(bcfg.ppm_x100);
                if (!pdx_rf_tune_khz(fk)) { printf("ERR frequenza %" PRIu32 " kHz\n", fk); continue; }
                if (!cwdry) pdx_bcn_route(&bcfg, b, true);
                cw_cfg = cwdry ? NULL : &bcfg; cw_band = b;
                const double lvl = 20.0 * log10((double)pdx_rf_cw_amp() / 480.0);
                if (b == 0) pdx_led_tx(lvl); else pdx_led_tx6(lvl);
                const char *e = pdx_rf_cw_start(&job, cw_end);
                if (e) { pdx_bcn_route(&bcfg, b, false); pdx_led_idle(); printf("ERR cw %s\n", e); continue; }
                if (morse)
                    printf("TX CW MORSE %s %" PRIu32 ".%03" PRIu32 " MHz (PLL %" PRIu32 " kHz) amp %d %u wpm pausa %u s portante %u s "
                           "manipolazione %s testo \"%s\" (stop: CW OFF, BOOT, GPIO STOP)\n", b ? "6cm" : "13cm", fk / 1000u,
                           fk % 1000u, pdx_rf_prog_khz(), pdx_rf_cw_amp(), job.wpm, job.pause_s, job.carrier_s,
                           pdx_rf_cw_key_pa() ? "PA" : "DAC", job.text);
                else
                    printf("TX CW portante %s %" PRIu32 ".%03" PRIu32 " MHz (PLL %" PRIu32 " kHz) amp %d (%.1f dB) max %u s "
                           "(stop: CW OFF, BOOT, GPIO STOP)\n", b ? "6cm" : "13cm", fk / 1000u, fk % 1000u,
                           pdx_rf_prog_khz(), pdx_rf_cw_amp(), lvl, job.max_s);
                if (cwdry) printf("(A SECCO: nessuna RF)\n");
                printf("OK cw\n");
            } else printf("ERR uso: CW [ON|MORSE|OFF|AMP|KEY]\n");
        }
        else if (!strcmp(cmd, "REG")) {
            uint32_t addr = (uint32_t)strtoul(p + 3, NULL, 16);
            if ((addr >> 20) == 0x600 || (addr >> 20) == 0x408) printf("OK reg %08" PRIx32 " = %08" PRIx32 "\n", addr, rd(addr & ~3u));
            else printf("ERR reg\n");
        } else printf("ERR comando\n");
    }
}
