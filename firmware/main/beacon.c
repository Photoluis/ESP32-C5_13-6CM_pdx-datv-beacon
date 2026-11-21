/* ESP32-C5 Dual-Band DATV - IZ7PDX - GPL-3.0
 * Beacon autonomo (B1): vedi beacon.h. */
#include "beacon.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_status.h"
#include "nvs.h"
#include "rf_c5.h"
#include "esp_attr.h"
#include "esp_system.h"
#include "web_c5.h"

#define CFG_VER  4u                  /* 2: antenna e GPIO per banda; 3: password dell'access point; 4: GPIO PTT */
#define NVS_NS   "bcn"
#define BOOT_GPIO 28

void pdx_bcn_defaults(pdx_bcn_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->ver = CFG_VER;
    snprintf(c->call, sizeof c->call, "IZ7PDX");
    c->f13_khz = 2375000u;
    c->f6_khz = 5750000u;
    c->sr = 333000u;
    c->amp = 250;
    c->tx_s = 60;
    c->pause_s = 30;
    c->wait_s = 30;
    c->bands = 3;
    c->auto_tx = 0;          /* sicurezza: la partenza automatica si abilita a mano (SET auto 1 + SAVE) */
    c->ant13 = 1;
    c->ant6 = 1;
    c->gpio13 = -1;
    c->gpio6 = -1;
    c->gpio_pol = 1;
    c->gpio_ptt = -1;
    snprintf(c->ap_pass, sizeof c->ap_pass, "pdxbeacon");
}

void pdx_bcn_load(pdx_bcn_cfg_t *c)
{
    pdx_bcn_defaults(c);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    pdx_bcn_cfg_t t;
    size_t n = sizeof t;
    if (nvs_get_blob(h, "cfg", &t, &n) == ESP_OK && n == sizeof t && t.ver == CFG_VER) *c = t;
    nvs_close(h);
}

bool pdx_bcn_save(const pdx_bcn_cfg_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    const bool ok = nvs_set_blob(h, "cfg", c, sizeof *c) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void pdx_bcn_print(const pdx_bcn_cfg_t *c)
{
    printf("CONFIG call %s  f13 %" PRIu32 ".%03" PRIu32 " MHz  f6 %" PRIu32 ".%03" PRIu32 " MHz  sr %" PRIu32
           "  amp %" PRIi32 "\n", c->call, c->f13_khz / 1000u, c->f13_khz % 1000u, c->f6_khz / 1000u, c->f6_khz % 1000u,
           c->sr, c->amp);
    printf("CONFIG tx %u s  pause %u s  wait %u s  bands %s%s  auto %s\n", c->tx_s, c->pause_s, c->wait_s,
           c->bands & 1 ? "13cm " : "", c->bands & 2 ? "6cm" : "", c->auto_tx ? "ON" : "OFF");
    char g13[12] = "", g6[12] = "", gp[12] = "nessuno";
    if (c->gpio13 >= 0) snprintf(g13, sizeof g13, " + GPIO%d", c->gpio13);
    if (c->gpio6 >= 0) snprintf(g6, sizeof g6, " + GPIO%d", c->gpio6);
    if (c->gpio_ptt >= 0) snprintf(gp, sizeof gp, "GPIO%d", c->gpio_ptt);
    printf("CONFIG antenna 13cm %s%s  6cm %s%s  PTT %s  GPIO esterni attivi %s\n", c->ant13 ? "IPEX" : "PCB", g13,
           c->ant6 ? "IPEX" : "PCB", g6, gp, c->gpio_pol ? "alti" : "bassi");
}

static bool num(const char *v, long lo, long hi, long *out)
{
    char *e;
    const long x = strtol(v, &e, 10);
    if (e == v || *e || x < lo || x > hi) return false;
    *out = x;
    return true;
}

/* frequenza in MHz con fino a 3 decimali -> kHz */
static bool freq_khz(const char *v, uint32_t lo_mhz, uint32_t hi_mhz, uint32_t *out)
{
    const double f = strtod(v, NULL);
    if (f < lo_mhz || f > hi_mhz) return false;
    *out = (uint32_t)(f * 1000.0 + 0.5);
    return true;
}

bool pdx_bcn_set(pdx_bcn_cfg_t *c, const char *key, const char *val)
{
    long x;
    if (!strcmp(key, "call")) {
        if (!*val || strlen(val) >= sizeof c->call) return false;
        snprintf(c->call, sizeof c->call, "%s", val);
        for (char *p = c->call; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        return true;
    }
    if (!strcmp(key, "f13")) return freq_khz(val, 2300, 2450, &c->f13_khz);
    if (!strcmp(key, "f6")) return freq_khz(val, 5650, 5850, &c->f6_khz);
    if (!strcmp(key, "sr") && num(val, 125000, 1000000, &x) &&
        (x == 125000 || x == 250000 || x == 333000 || x == 500000 || x == 1000000)) { c->sr = (uint32_t)x; return true; }
    if (!strcmp(key, "amp") && num(val, 20, 300, &x)) { c->amp = (int32_t)x; return true; }
    if (!strcmp(key, "tx") && num(val, 10, 600, &x)) { c->tx_s = (uint16_t)x; return true; }
    if (!strcmp(key, "pause") && num(val, 5, 3600, &x)) { c->pause_s = (uint16_t)x; return true; }
    if (!strcmp(key, "wait") && num(val, 5, 300, &x)) { c->wait_s = (uint16_t)x; return true; }
    if (!strcmp(key, "bands") && num(val, 1, 3, &x)) { c->bands = (uint8_t)x; return true; }
    if (!strcmp(key, "auto") && num(val, 0, 1, &x)) { c->auto_tx = (uint8_t)x; return true; }
    if (!strcmp(key, "ant13") && num(val, 0, 1, &x)) { c->ant13 = (uint8_t)x; return true; }
    if (!strcmp(key, "ant6") && num(val, 0, 1, &x)) { c->ant6 = (uint8_t)x; return true; }
    if ((!strcmp(key, "gpio13") || !strcmp(key, "gpio6") || !strcmp(key, "gpioptt")) && num(val, -1, 28, &x)) {
        if (x >= 0 && !pdx_bcn_gpio_ok((int)x)) return false;
        if (!strcmp(key, "gpio13")) c->gpio13 = (int8_t)x;
        else if (!strcmp(key, "gpio6")) c->gpio6 = (int8_t)x;
        else c->gpio_ptt = (int8_t)x;
        return true;
    }
    if (!strcmp(key, "gpiopol") && num(val, 0, 1, &x)) { c->gpio_pol = (uint8_t)x; return true; }
    if (!strcmp(key, "appass")) {
        const size_t n = strlen(val);
        if (n < 8 || n >= sizeof c->ap_pass) return false;
        snprintf(c->ap_pass, sizeof c->ap_pass, "%s", val);
        return true;
    }
    return false;
}

/* GPIO esterni consentiti: connettore 0..10 e piazzole 23/24/25 (non 13/14 USB, 26 antenna, 27 LED, 28 BOOT) */
bool pdx_bcn_gpio_ok(int g) { return (g >= 0 && g <= 10) || g == 23 || g == 24 || g == 25; }

static void gpio_out(int g, bool active, uint8_t pol)
{
    if (g < 0) return;
    gpio_reset_pin((gpio_num_t)g);
    gpio_set_direction((gpio_num_t)g, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)g, active ? pol : !pol);
}

/* uscita della banda b (0 = 13 cm, 1 = 6 cm): commutatore interno e GPIO esterni; tx = false: tutti a riposo */
static void antenna_route(const pdx_bcn_cfg_t *c, int b, bool tx)
{
    gpio_set_level(PDX_C5_ANT_GPIO, (b == 0 ? c->ant13 : c->ant6) ? 1 : 0);
    gpio_out(c->gpio13, tx && b == 0, c->gpio_pol);
    gpio_out(c->gpio6, tx && b == 1, c->gpio_pol);
    gpio_out(c->gpio_ptt, tx, c->gpio_pol);                  /* TX/standby (PTT): attivo durante qualunque TX */
    if (tx && ((b == 0 ? c->gpio13 : c->gpio6) >= 0 || c->gpio_ptt >= 0)) vTaskDelay(pdMS_TO_TICKS(50));   /* relè */
}

/* ---- doppia accensione: flag in NVS acceso all'avvio e spento dopo 5 s ---- */
static void quick_set(uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "quick", v);
    nvs_commit(h);
    nvs_close(h);
}

static esp_timer_handle_t quick_tmr;
static void quick_cb(void *arg) { (void)arg; quick_set(0); }

bool pdx_bcn_quick_boot(void)
{
    uint8_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "quick", &v);
        nvs_close(h);
    }
    quick_set(1);
    const esp_timer_create_args_t a = {.callback = quick_cb, .name = "quick"};
    if (esp_timer_create(&a, &quick_tmr) == ESP_OK) esp_timer_start_once(quick_tmr, 5000000);
    return v == 1;
}

void pdx_bcn_quick_clear(void)
{
    if (quick_tmr) esp_timer_stop(quick_tmr);
    quick_set(0);
}

/* ---- stato del ciclo attraverso i riavvii (memoria RTC: sopravvive a esp_restart, non allo spegnimento) ---- */
typedef struct { uint32_t magic, cycle; int32_t band, mode; } bcn_rtc_t;
static RTC_NOINIT_ATTR bcn_rtc_t rtc_state;
#define RTC_MAGIC 0x50445843u                               /* "PDXC" */

void pdx_bcn_reboot(int mode, int band, uint32_t cycle)
{
    rtc_state = (bcn_rtc_t){.magic = RTC_MAGIC, .cycle = cycle, .band = band, .mode = mode};
    static const char *nm[] = {"", "TX", "PAUSA", "CONFIG"};
    printf("BEACON riavvio -> %s (banda %s, ciclo %" PRIu32 ")\n", nm[mode & 3], band ? "6 cm" : "13 cm", cycle);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

void pdx_bcn_request_run(int start_band, uint32_t cycle) { pdx_bcn_reboot(PDX_BCN_BOOT_RUN, start_band, cycle); }

int pdx_bcn_boot_mode(int *band, uint32_t *cycle)
{
    int m = PDX_BCN_BOOT_NONE;
    if (rtc_state.magic == RTC_MAGIC && esp_reset_reason() == ESP_RST_SW) {
        m = (int)rtc_state.mode;
        *band = rtc_state.band;
        *cycle = rtc_state.cycle;
    }
    rtc_state.magic = 0;
    return m;
}

/* ---- attese con STOP: un carattere dalla seriale o il tasto BOOT ---- */
static bool stop_requested(TickType_t wait)
{
    uint8_t ch;
    if (usb_serial_jtag_read_bytes(&ch, 1, wait) > 0) return true;
    return gpio_get_level(BOOT_GPIO) == 0;
}

bool pdx_bcn_wait(unsigned s)
{
    for (unsigned t = s; t > 0; t--) {
        printf("BEACON parte tra %u s (un tasto sulla seriale o BOOT annulla)\n", t);
        for (int k = 0; k < 10; k++)
            if (stop_requested(pdMS_TO_TICKS(100))) { printf("BEACON annullato: modo configurazione\n"); return false; }
    }
    return true;
}

static bool pause_with_stop(unsigned s)
{
    printf("BEACON pausa %u s (un tasto sulla seriale, BOOT o STOP dalla dashboard ferma il beacon)\n", s);
    for (unsigned k = 0; k < s * 10u; k++)
        if (stop_requested(pdMS_TO_TICKS(100)) || pdx_web_take_stop()) return false;
    return true;
}

static void next_band(const pdx_bcn_cfg_t *c, int band, uint32_t cycle, int *nb, uint32_t *nc)
{
    if (band == 0 && (c->bands & 2)) { *nb = 1; *nc = cycle; return; }
    *nb = (c->bands & 1) ? 0 : 1;
    *nc = cycle + 1;
}

void pdx_bcn_segment(const pdx_bcn_cfg_t *c, int b, uint32_t cycle)
{
    pdx_bcn_quick_clear();
    if (!(c->bands & (1u << b))) { int nb; uint32_t nc; next_band(c, b, cycle, &nb, &nc); pdx_bcn_request_run(nb, nc); }
    const double lvl = 20.0 * log10((double)c->amp / 480.0);
    const uint32_t fk = b == 0 ? c->f13_khz : c->f6_khz;
    char prov[40];
    snprintf(prov, sizeof prov, "BEACON %" PRIu32 ".%03" PRIu32 " MHz", fk / 1000u, fk % 1000u);
    pdx_rf_live_source(b == 0 ? 1 : 0, c->call, prov);
    if (!pdx_rf_tune(fk / 1000u)) { printf("BEACON ERR frequenza %" PRIu32 " kHz\n", fk); pdx_bcn_reboot(PDX_BCN_BOOT_CONFIG, b, cycle); }
    printf("BEACON ciclo %" PRIu32 " TX %s %" PRIu32 ".%03" PRIu32 " MHz %u s (BOOT ferma)\n", cycle,
           b == 0 ? "13cm" : "6cm", fk / 1000u, fk % 1000u, c->tx_s);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    antenna_route(c, b, true);
    if (b == 0) pdx_led_tx(lvl); else pdx_led_tx6(lvl);
    pdx_stream_res_t r;
    const char *e = pdx_rf_s2live(c->sr, c->amp, c->tx_s * 1000u, 50, 0, 0, &r);
    pdx_led_idle();
    antenna_route(c, b, false);
    pdx_live_stats_t st;
    pdx_rf_live_stats(&st);
    if (e) { printf("BEACON ERR %s\n", e); pdx_bcn_reboot(PDX_BCN_BOOT_CONFIG, b, cycle); }
    printf("BEACON fine TX: frame %" PRIu32 " DUMMY %" PRIu32 " ritardo max %" PRIu32 " sforamenti %" PRIu32 "\n",
           st.frames, st.dummies, r.late_max, r.overruns);
    if (r.stopped) { printf("BEACON fermato con BOOT\n"); pdx_bcn_reboot(PDX_BCN_BOOT_CONFIG, b, cycle); }
    pdx_bcn_reboot(PDX_BCN_BOOT_PAUSE, b, cycle);
}

bool pdx_bcn_pause(const pdx_bcn_cfg_t *c, int b, uint32_t cycle)
{
    char stt[48];
    snprintf(stt, sizeof stt, "PAUSA dopo %s, ciclo %" PRIu32, b == 0 ? "13 cm" : "6 cm", cycle);
    pdx_web_set_state(stt);
    pdx_web_start();
    if (!pause_with_stop(c->pause_s)) {
        pdx_web_set_state("CONFIG (beacon fermato)");
        printf("BEACON fermato: modo configurazione\n");
        return false;
    }
    int nb;
    uint32_t nc;
    next_band(c, b, cycle, &nb, &nc);
    pdx_bcn_request_run(nb, nc);
    return true;
}
