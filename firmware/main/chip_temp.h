/* ESP32-C5_13-6CM_pdx-datv-beacon - IZ7PDX - GPL-3.0 (dal beacon ESP32-S3)
 * Temperatura del chip dal sensore interno dell'ESP32-C5 (indicativa: è la temperatura del silicio, non dell'aria).
 * Sull'S3 si legge fuori dalla TX (parte analogica condivisa con la radio); sulla C5 la lettura durante la CW è in prova. */
#pragma once

void pdx_temp_init(void);
float pdx_temp_read(void);      /* gradi Celsius; NAN se il sensore non è disponibile */
