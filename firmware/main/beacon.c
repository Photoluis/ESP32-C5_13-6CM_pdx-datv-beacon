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

#define CFG_VER  1u
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
    c->ant = 1;
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
    printf("CONFIG tx %u s  pause %u s  wait %u s  bands %s%s  auto %s  ant %s\n", c->tx_s, c->pause_s, c->wait_s,
           c->bands & 1 ? "13cm " : "", c->bands & 2 ? "6cm" : "", c->auto_tx ? "ON" : "OFF", c->ant ? "IPEX" : "ceramica");
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
    if (!strcmp(key, "sr") && num(val, 125000, 500000, &x)) { c->sr = (uint32_t)x; return true; }
    if (!strcmp(key, "amp") && num(val, 20, 300, &x)) { c->amp = (int32_t)x; return true; }
    if (!strcmp(key, "tx") && num(val, 10, 600, &x)) { c->tx_s = (uint16_t)x; return true; }
    if (!strcmp(key, "pause") && num(val, 5, 3600, &x)) { c->pause_s = (uint16_t)x; return true; }
    if (!strcmp(key, "wait") && num(val, 5, 300, &x)) { c->wait_s = (uint16_t)x; return true; }
    if (!strcmp(key, "bands") && num(val, 1, 3, &x)) { c->bands = (uint8_t)x; return true; }
    if (!strcmp(key, "auto") && num(val, 0, 1, &x)) { c->auto_tx = (uint8_t)x; return true; }
    if (!strcmp(key, "ant") && num(val, 0, 1, &x)) { c->ant = (uint8_t)x; return true; }
    return false;
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
    printf("BEACON pausa %u s (un tasto sulla seriale o BOOT ferma il beacon)\n", s);
    for (unsigned k = 0; k < s * 10u; k++)
        if (stop_requested(pdMS_TO_TICKS(100))) return false;
    return true;
}

void pdx_bcn_run(const pdx_bcn_cfg_t *c)
{
    pdx_bcn_quick_clear();
    gpio_set_level(PDX_C5_ANT_GPIO, c->ant ? 1 : 0);
    const double lvl = 20.0 * log10((double)c->amp / 480.0);
    for (uint32_t cycle = 1;; cycle++) {
        for (int b = 0; b < 2; b++) {
            if (!(c->bands & (1u << b))) continue;
            const uint32_t fk = b == 0 ? c->f13_khz : c->f6_khz;
            char prov[40];
            snprintf(prov, sizeof prov, "BEACON %" PRIu32 ".%03" PRIu32 " MHz", fk / 1000u, fk % 1000u);
            pdx_rf_live_source(b == 0 ? 1 : 0, c->call, prov);
            if (!pdx_rf_tune(fk / 1000u)) { printf("BEACON ERR frequenza %" PRIu32 " kHz\n", fk); return; }
            printf("BEACON ciclo %" PRIu32 " TX %s %" PRIu32 ".%03" PRIu32 " MHz %u s (BOOT ferma)\n", cycle,
                   b == 0 ? "13cm" : "6cm", fk / 1000u, fk % 1000u, c->tx_s);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(50));
            pdx_led_tx(lvl);
            pdx_stream_res_t r;
            const char *e = pdx_rf_s2live(c->sr, c->amp, c->tx_s * 1000u, 50, 0, 0, &r);
            pdx_led_idle();
            pdx_live_stats_t st;
            pdx_rf_live_stats(&st);
            if (e) { printf("BEACON ERR %s\n", e); return; }
            printf("BEACON fine TX: frame %" PRIu32 " DUMMY %" PRIu32 " ritardo max %" PRIu32 " sforamenti %" PRIu32 "\n",
                   st.frames, st.dummies, r.late_max, r.overruns);
            if (r.stopped) { printf("BEACON fermato con BOOT\n"); return; }
            if (!pause_with_stop(c->pause_s)) { printf("BEACON fermato: modo configurazione\n"); return; }
        }
    }
}
