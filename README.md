# ESP32-C5_13-6CM_pdx-datv-beacon — IZ7PDX

Trasmettitore/beacon DVB-S2 su ESP32-C5, che usa la parte TX del Wi-Fi come modulatore I/Q, in **13 cm** (2,4 GHz) e **6 cm** (5,7 GHz).
È l'evoluzione dell'ESP32-S3-CAM_PDX DATV Beacon.

## Stato

Fase P0: progetto appena creato, si stanno definendo i requisiti. Ancora nessun firmware.

## Fasi

1. P0 requisiti
2. P1 identificazione della scheda e backup della flash
3. P2 registri RF del C5, tono di prova a 2,4 GHz
4. P3 frequenze fuori dalla banda Wi-Fi (2375 MHz, 5,7 GHz)
5. P4 DVB-S2 dal monoscopio
6. P5 funzioni del beacon e scelta della banda 13/6 cm
