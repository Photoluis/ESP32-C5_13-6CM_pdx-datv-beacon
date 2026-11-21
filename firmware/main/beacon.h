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
    uint8_t ant;            /* 1 = IPEX, 0 = antenna ceramica */
    uint8_t rsv[3];
} pdx_bcn_cfg_t;

void pdx_bcn_defaults(pdx_bcn_cfg_t *c);
void pdx_bcn_load(pdx_bcn_cfg_t *c);              /* NVS, altrimenti valori predefiniti */
bool pdx_bcn_save(const pdx_bcn_cfg_t *c);
void pdx_bcn_print(const pdx_bcn_cfg_t *c);
bool pdx_bcn_set(pdx_bcn_cfg_t *c, const char *key, const char *val);   /* false se chiave o valore non validi */

/* doppia accensione: true se l'avvio precedente è stato interrotto entro 5 s (=> configurazione, niente TX) */
bool pdx_bcn_quick_boot(void);
void pdx_bcn_quick_clear(void);

/* attesa annullabile (true = scaduta senza annullamento: si parte) */
bool pdx_bcn_wait(unsigned s);
/* ciclo del beacon fino allo STOP (tasto BOOT durante la TX, carattere seriale o BOOT nelle pause) */
void pdx_bcn_run(const pdx_bcn_cfg_t *c);
