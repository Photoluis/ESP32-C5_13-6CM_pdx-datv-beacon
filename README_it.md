# ESP32-C5_13-6CM_pdx-datv-beacon

**Beacon DATV DVB-S2 a doppia banda (13 cm e 6 cm) su una sola ESP32-C5, che usa il trasmettitore Wi-Fi come modulatore I/Q.**

[English](README.md) · [日本語](README_ja.md) · Dashboard in 11 lingue · Licenza GPL-3.0

| 13 cm — 2375,000 MHz | 6 cm — 5750,000 MHz |
|---|---|
| ![DVB-S2 a 2375 MHz ricevuto con LOCK, MER 20,9 dB](docs/img/pd4pdx_hdmi_20261122_230937.jpg) | ![DVB-S2 a 5750 MHz ricevuto con LOCK, MER 20,8 dB](docs/img/pd4pdx_hdmi_20261122_231119.jpg) |

*Le due bande ricevute da una stazione Pluto + leandvb: QPSK 3/4, 333 kS, monoscopio H.265 + AAC, MER ~21 dB, VBER 0.*

## Cosa fa

* Genera un segnale **DVB-S2 QPSK 3/4** (frame normali, pilot) a **125 / 250 / 333 / 500 kS**, con il transport stream MPEG (monoscopio video H.265 + audio AAC, SDT con il tuo nominativo) **costruito in tempo reale sul chip**.
* Trasmette alternando **13 cm (2300–2450 MHz)** e **6 cm (5650–5850 MHz)** in un ciclo da beacon: TX 13 cm → pausa → TX 6 cm → pausa.
* Gira su una **Waveshare ESP32-C5-Zero** (ESP32-C5, un solo core RISC-V, flash 4 MB, niente PSRAM). Nessun DAC esterno, nessuna FPGA.
* **Dashboard web** sul suo access point (`C5_13-6CM_BEACON`, http://192.168.4.1) in 11 lingue: nominativo, frequenze, symbol rate, livello, antenna per banda (PCB o IPEX), GPIO per relè esterni / PTT, tempi del ciclo, **correzione del quarzo in ppm** con calcolatore.
* **Stop senza tasti:** doppia accensione, **GPIO STOP** a massa (interruttore, relè, telecomando) oppure STOP dalla dashboard.

| Dashboard (telefono) | | |
|---|---|---|
| ![Italiano](docs/img/dashboard_it.png) | ![English](docs/img/dashboard_en.png) | ![日本語](docs/img/dashboard_ja.png) |

Tutte le lingue: [DE](docs/img/dashboard_de.png) · [ES](docs/img/dashboard_es.png) · [FR](docs/img/dashboard_fr.png) · [PT](docs/img/dashboard_pt.png) · [NL](docs/img/dashboard_nl.png) · [PL](docs/img/dashboard_pl.png) · [EL](docs/img/dashboard_el.png) · [AR](docs/img/dashboard_ar.png) — pagina intera: [IT](docs/img/dashboard_full_it.png) · [EN](docs/img/dashboard_full_en.png) · [JA](docs/img/dashboard_full_ja.png)

## Come funziona (in breve)

La radio della ESP32-C5 ha un motore interno che riproduce campioni (Espressif lo usa per le calibrazioni). Il suo codice di trasmissione non c'è più nelle librerie del costruttore, quindi i registri sono stati trovati per prova: due banchi di campioni a 40 MS/s suonati a ping-pong e riempiti dalla CPU con la QPSK filtrata RRC mentre l'altro banco è in onda, con 333 000 S/s esatti grazie a un accumulatore frazionario. BCH, LDPC, scrambling, trama PL e multiplexer del transport stream girano tutti sull'unico core negli intervalli tra uno scambio e l'altro. Dettagli e tutti i valori misurati: **[docs/MISURE_it.md](docs/MISURE_it.md)**.

## Hardware

| Voce | Note |
|---|---|
| Scheda | Waveshare ESP32-C5-Zero (ESP32-C5HF4, flash 4 MB) |
| Antenna | ceramica sulla scheda o **IPEX** (scelta con GPIO26 dalla dashboard); nelle prove un'antenna larga banda per 2,4 e 5,7 GHz |
| GPIO | 26 commutatore d'antenna · 27 LED di stato (giallo = attesa, rosso = TX 13 cm, blu = TX 6 cm) · 28 BOOT · **10 STOP** (ingresso, a GND) · uscite facoltative 0–10, 23–25 per relè di banda e PTT |
| Alimentazione | USB-C |

Il livello d'uscita è di pochi milliwatt: per l'uso reale servono un filtro e un amplificatore esterni.

## Compilare e caricare

Serve **ESP-IDF v6.1** (target `esp32c5`).

```
cd firmware
idf.py set-target esp32c5
idf.py build
esptool --chip esp32c5 --port COMx -b 921600 write-flash 0x2000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/pdx_c5_probe.bin
```

Il beacon ha bisogno anche dei due monoscopi già codificati in flash (6 cm a `0x290000`, 13 cm a `0x320000`). **I monoscopi neutri e un'immagine pronta da caricare saranno pubblicati con la prima release**; si generano con lo strumento `make_es` del progetto [ESP32-S3-CAM_PDX DATV Beacon](https://github.com/Photoluis/Esp32s3-cam-pdx-datv-beacon).

## Primo utilizzo

1. Alimenta la scheda: parte in **configurazione** (nessuna TX, LED giallo fisso) perché la partenza automatica è spenta.
2. Collega il telefono al Wi-Fi **`C5_13-6CM_BEACON`** (password `pdxbeacon`) e apri **http://192.168.4.1**.
3. Imposta nominativo, frequenze, antenna e correzione del quarzo, premi **Salva**, poi **START**.
4. Durante la TX il Wi-Fi fa da modulatore: la dashboard torna a ogni pausa.

### Fermare il beacon senza tasti

* **Doppia accensione:** accendi, togli l'alimentazione entro 5 s e riaccendi → configurazione, nessuna TX.
* **GPIO STOP:** GPIO10 a GND ferma subito la TX e riavvia in configurazione con il Wi-Fi acceso; tenuto a massa all'accensione impedisce ogni TX.
* **Dashboard:** STOP / CONFIG durante una pausa.
* Con i tasti: BOOT ferma sempre, anche in TX.

## Stato

Prima fase del progetto completata (novembre 2026). Provato in aria: DVB-S2 a 13 e 6 cm, transport stream in tempo reale, ciclo del beacon, dashboard, GPIO STOP, correzione del quarzo.

Prossimi passi:
* compensazione con la temperatura della deriva del quarzo durante la TX (~1,6 ppm/min da freddo);
* monoscopio con il nominativo in sovrimpressione dalla dashboard;
* symbol rate 1000 kS;
* collegamento SPI con una ESP32-S3 (dashboard durante la TX, transponder in banda incrociata, streaming dal telefono).

## Nota legale

È un progetto **radioamatoriale**. Trasmettere a 13 cm e 6 cm richiede la licenza di radioamatore e il rispetto del piano di banda e dei limiti di potenza nazionali. La radio Wi-Fi viene usata fuori dai suoi canali e dalla sua modulazione: sei responsabile di ciò che trasmetti.

## Licenza e crediti

GPL-3.0 — vedi [LICENSE](LICENSE). © 2026 Luigi D'Arcangelo, IZ7PDX.
Basato sul codificatore DVB-S2 e sul transport stream dell'[ESP32-S3-CAM_PDX DATV Beacon](https://github.com/Photoluis/Esp32s3-cam-pdx-datv-beacon).
