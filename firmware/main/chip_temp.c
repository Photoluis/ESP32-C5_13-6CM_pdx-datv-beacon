/* ESP32-C5_13-6CM_pdx-datv-beacon - IZ7PDX - GPL-3.0 (dal beacon ESP32-S3)
 * Temperatura del chip (vedi chip_temp.h). */
#include "chip_temp.h"

#include <math.h>
#include "driver/temperature_sensor.h"

static temperature_sensor_handle_t tsens;
static bool hot;                       /* scala 20..100 C invece di -10..80 C */

static void install(bool hi)
{
    if (tsens) { temperature_sensor_disable(tsens); temperature_sensor_uninstall(tsens); tsens = NULL; }
    const temperature_sensor_config_t c = hi ? (temperature_sensor_config_t)TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100)
                                             : (temperature_sensor_config_t)TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&c, &tsens) != ESP_OK || temperature_sensor_enable(tsens) != ESP_OK) tsens = NULL;
    hot = hi;
}

void pdx_temp_init(void)
{
    if (tsens) return;
    install(false);
}

/* 15/11 (DevKitC-1 a 79,8 C, al limite della scala -10..80): oltre 75 C si passa alla scala 20..100 C, sotto 25 C si
 * torna a quella bassa (traliccio d'inverno) */
float pdx_temp_read(void)
{
    float t;
    if (!tsens || temperature_sensor_get_celsius(tsens, &t) != ESP_OK) return NAN;
    if ((!hot && t > 75.0f) || (hot && t < 25.0f)) {
        install(!hot);
        if (!tsens || temperature_sensor_get_celsius(tsens, &t) != ESP_OK) return NAN;
    }
    return t;
}
