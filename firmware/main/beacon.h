/* ESP32-C5 Dual-Band DATV - IZ7PDX - GPL-3.0
 * Beacon autonomo (B1): ciclo 13 cm / pausa / 6 cm / pausa con il TS continuo, configurazione in NVS, attesa annullabile
 * all'accensione, STOP senza USB e senza tasti con la doppia accensione (spegnere e riaccendere entro 5 s). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t ver;
    char call[16];          /* nominativo (servizio della SDT) */
    uint32_t f13_khz;       /* frequenza 13 cm */
    uint32_t f6_khz;        /* frequenza 6 cm */
    uint32_t sr;            /* symbol rate, S/s */
    int32_t amp;            /* ampiezza DAC (picco), <= 300 */
    uint16_t tx_s;          /* durata della TX per banda */
    uint16_t pause_s;       /* pausa dopo ogni banda */
    uint16_t wait_s;        /* attesa annullabile all'accensione */
    uint8_t bands;          /* bit 0 = 13 cm, bit 1 = 6 cm */
    uint8_t auto_tx;        /* 1 = il ciclo parte da solo all'accensione */
    uint8_t ant13, ant6;    /* uscita per banda: 1 = IPEX, 0 = antenna PCB/ceramica (commutatore su GPIO26) */
    int8_t gpio13, gpio6;   /* GPIO esterno attivo durante la TX della banda (relè, commutatore), -1 = nessuno */
    uint8_t gpio_pol;       /* 1 = attivo alto, 0 = attivo basso */
    int8_t gpio_ptt;        /* GPIO TX/standby (PTT) attivo durante qualunque TX, -1 = nessuno */
    int8_t gpio_stop;       /* GPIO STOP: ingresso con pull-up, a massa = beacon fermo e Wi-Fi acceso, -1 = nessuno */
    uint8_t rsv[1];
    char ap_pass[16];       /* password dell'access point della dashboard (8..15 caratteri) */
    /* --- v6 (blob v4/v5 = 68 byte, fin qui) --- */
    int16_t ppm_x100;       /* errore del quarzo in centesimi di ppm (+ = la scheda esce alta), -10000..10000 */
    uint8_t rsv2[2];
} pdx_bcn_cfg_t;

void pdx_bcn_defaults(pdx_bcn_cfg_t *c);
void pdx_bcn_load(pdx_bcn_cfg_t *c);              /* NVS, altrimenti valori predefiniti */
bool pdx_bcn_save(const pdx_bcn_cfg_t *c);
void pdx_bcn_print(const pdx_bcn_cfg_t *c);
bool pdx_bcn_set(pdx_bcn_cfg_t *c, const char *key, const char *val);   /* false se chiave o valore non validi */
bool pdx_bcn_gpio_ok(int g);                                            /* GPIO esterno consentito */
const char *pdx_bcn_check(const pdx_bcn_cfg_t *c);                      /* NULL = ok, altrimenti GPIO in conflitto */
/* GPIO STOP: configura l'ingresso (pull-up) e lo aggiunge al BOOT nel ciclo della TX; true = a massa adesso */
void pdx_bcn_stop_gpio_init(const pdx_bcn_cfg_t *c);
/* applica al volo le parti della configurazione che agiscono subito (GPIO STOP, correzione del quarzo) */
void pdx_bcn_apply(const pdx_bcn_cfg_t *c);
bool pdx_bcn_stop_gpio_active(void);

/* doppia accensione: true se l'avvio precedente è stato interrotto entro 5 s (=> configurazione, niente TX) */
bool pdx_bcn_quick_boot(void);
void pdx_bcn_quick_clear(void);

/* attesa annullabile (true = scaduta senza annullamento: si parte) */
bool pdx_bcn_wait(unsigned s);
/* Il ciclo è una sequenza di riavvii, ognuno con la radio pulita per il suo compito (stato in memoria RTC):
 *   RUN   TX di una banda, poi riavvio in PAUSE (o in CONFIG se fermato con BOOT)
 *   PAUSE access point e dashboard per la pausa, poi riavvio in RUN sulla banda seguente (o CONFIG se STOP)
 *   CONFIG / accensione: access point e dashboard, nessuna TX finché non arriva START (o la partenza automatica) */
enum { PDX_BCN_BOOT_NONE = 0, PDX_BCN_BOOT_RUN = 1, PDX_BCN_BOOT_PAUSE = 2, PDX_BCN_BOOT_CONFIG = 3, PDX_BCN_BOOT_LAB = 4 };  /* LAB: niente access point, comandi di prova disponibili */
void pdx_bcn_reboot(int mode, int band, uint32_t cycle);
int pdx_bcn_boot_mode(int *band, uint32_t *cycle);     /* modo del riavvio software (NONE dopo un'accensione) */
void pdx_bcn_request_run(int start_band, uint32_t cycle);   /* = pdx_bcn_reboot(RUN, ...) */
/* TX della banda band (riavvia sempre alla fine: PAUSE o CONFIG) */
void pdx_bcn_segment(const pdx_bcn_cfg_t *c, int band, uint32_t cycle);
/* uscita della banda (0 = 13 cm, 1 = 6 cm): commutatore d'antenna, GPIO di banda e PTT; tx = false: a riposo */
void pdx_bcn_route(const pdx_bcn_cfg_t *c, int band, bool tx);
/* pausa dopo la banda band: dashboard attiva; alla fine riavvia in RUN sulla banda seguente; false = STOP */
bool pdx_bcn_pause(const pdx_bcn_cfg_t *c, int band, uint32_t cycle);
