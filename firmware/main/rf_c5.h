/* ESP32-C5 Dual-Band DATV - IZ7PDX - GPL-3.0
 * Accesso al blocco RF dell'ESP32-C5: P2 = solo ricezione (motore di cattura "dump"), nessuna trasmissione. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PDX_C5_SRAM_OWNER  0x60095004u   /* HP_SYSTEM_SRAM_USAGE_CONF_REG: [11:8] SRAM_USAGE, bit16 MAC_DUMP_ALLOC */
#define PDX_C5_DUMP_BASE   0x600A9000u   /* blocco del motore di cattura (MODEM0 + 0x9000) */
#define PDX_C5_BANK        ((volatile uint32_t *)0x40830000u)
#define PDX_C5_BANK_WORDS  16380u
#define PDX_C5_ANT_GPIO    26            /* commutatore d'antenna della ESP32-C5-Zero (ceramica / IPEX) */

void pdx_rf_init(void);                  /* Wi-Fi in modo NULL: accende e calibra la PHY */
bool pdx_rf_tune(unsigned mhz);          /* PLL sulla frequenza (MHz interi) */
unsigned pdx_rf_mhz(void);
/* cattura n parole (coppie I/Q) nel banco; div = divisore della frequenza di campionamento (0 = massima);
 * restituisce il numero di parole scritte (sentinella sovrascritta) e la durata in us */
unsigned pdx_rf_capture(unsigned n, unsigned div, uint32_t *us);
