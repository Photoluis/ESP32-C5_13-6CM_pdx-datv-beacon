/* ESP32-C5 Dual-Band DATV - IZ7PDX - GPL-3.0
 * Dashboard (B2): access point "<QRZ>-BEACON" e pagina su http://192.168.4.1, attiva nelle pause del ciclo e in modo
 * configurazione. Durante la TX la radio Wi-Fi fa da modulatore: niente access point. */
#pragma once
#include <stdbool.h>
#include "beacon.h"

void pdx_web_init(pdx_bcn_cfg_t *cfg, const char *fw);
bool pdx_web_start(void);                  /* access point + server (se già attivi non fa nulla) */
bool pdx_web_running(void);
void pdx_web_set_state(const char *state); /* testo di stato mostrato nella pagina */
bool pdx_web_take_stop(void);              /* true una volta dopo STOP/CONFIG dalla pagina */
bool pdx_web_client_seen(void);            /* un telefono si è collegato all'access point */
