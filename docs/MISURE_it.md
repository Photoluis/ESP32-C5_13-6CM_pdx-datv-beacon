# Misure e parametri — beacon DATV 13/6 cm con ESP32-C5

[English](MEASUREMENTS.md) · [日本語](MEASUREMENTS_ja.md)

Tutti i valori sono stati misurati su hardware reale durante lo sviluppo (15–22 novembre 2026).
Ricevitori: il **Pd4Pdx** (ADALM-Pluto + leandvb per la decodifica DVB-S2; due unità: stazione principale e "Valigetta") e **MiniTiouner**.
Spettro: visualizzazione basata sulla Pluto, span 10 MHz. Livelli relativi (dBFS della Pluto), non tarati.

## 1. Hardware in prova

| Voce | Valore |
|---|---|
| Scheda | Waveshare **ESP32-C5-Zero** (modulo ESP32-C5HF4) |
| Chip | ESP32-C5 rev 1.0, Wi-Fi 6 doppia banda, 1 core RISC-V HP a 240 MHz + core LP |
| Flash / PSRAM | 4 MB nel package / **assente** |
| Quarzo | 48 MHz |
| Uscite RF | ANT_2G + ANT_5G → diplexer → commutatore RF su **GPIO26**: basso = antenna ceramica (PCB), **alto = IPEX** |
| LED | WS2812 su GPIO27 · tasto BOOT GPIO28 |
| Antenna usata | antenna larga banda sull'IPEX (copre 2,4 e 5,7 GHz) |
| Ambiente | ESP-IDF v6.1 |

## 2. Come la C5 trasmette l'I/Q (trovato per prova)

Le librerie del costruttore per la C5 non contengono più il codice di replay in TX (`dactrig` è vuota): ogni registro qui sotto è stato trovato con le prove.

| Parametro | Valore |
|---|---|
| Controllo del replay del DAC | `0x600A900C`: [13:0] lunghezza, bit19 ciclo, bit31 avvio, bit18 fine |
| Parola del campione | I = bit 0–9, Q = bit 10–19 (10 bit con segno); spettro **non invertito** |
| Frequenza di campionamento | 80 MS/s (3,000 cicli CPU/campione); **bit17 = 40 MS/s** (6,000 cicli/campione) |
| Campo sorgente | `0x600A9008` bit 17–20 = `0xF` (si perde allo spegnimento, impostato prima di ogni TX) |
| Banchi di memoria | registro proprietario `0x60095004`: 0x200 → banco 0x40820000, 0x400 → 0x40840000; il motore segue il proprietario al volo (ping-pong provato fino a 100 000 scambi in 2 s) |
| Latenza di partenza | ~45–50 cicli CPU, dispersione ±10 |
| Sintonia del PLL | `phy_set_freq(MHz, kHz)` — MHz interi + kHz (passo 1 kHz) |
| Frequenze accettate | 2320, 2375, 5660, 5760, 5800 MHz provate senza errori |

## 3. Risultati in RF

### 3.1 Primi segnali (16/11/2026)

| Prova | Frequenza | Risultato |
|---|---|---|
| Tono della libreria (REFTONE) | 2375 MHz | riga −5,6 dB |
| Replay del DAC, ampiezza 0 (solo portante) | 2375 MHz | −33,4 dB |
| Replay del DAC, ampiezza 200 | 2375 MHz | −5,0 dB (+28 dB) |
| Tono k=±16 a 80 MS/s | 2375 MHz | righe a 2375,80 / 2374,20 MHz, immagine e spurie ≥ 55 dB sotto, LO residuo ~−45 dBc |
| Tono k=±16 | 5750 MHz | righe a 5750,82 / 5749,21 MHz, −25 dB, S/N 28 dB |
| Streaming in tempo reale (due banchi, 40 MS/s) | 5750 MHz | 4 cicli/parola, 0 sforamenti su 80 000 scambi, riga unica pulita |
| QPSK RRC 0,35, 333 kS (PRBS) | 5750 MHz | campana piatta larga 0,40 MHz, spalle ~39 dB sotto, repliche H=8 a ±5 MHz ~34 dB sotto |

### 3.2 DVB-S2 (QPSK 3/4, frame normali, pilot, monoscopio H.265 + AAC)

| Data | Freq | SR | Sorgente | Aggancio | MER | Note |
|---|---|---|---|---|---|---|
| 20/11 19:22 | 5750 MHz | 250 kS | simboli precalcolati | ~45 s | 22,5–23,2 dB | VBER 0, offset +48,5…+49,8 kHz, S/N 32–34 dB |
| 20/11 18:36 | 5750 MHz | "333 kS" = 333 333 S/s | precalcolati | **nessun aggancio** | — | errore di +1000 ppm rispetto ai 333 000 attesi dal ricevitore |
| 20/11 19:31 | 5750 MHz | **333 000 S/s esatti** | precalcolati | ~30 s, tenuto 90 s | 20,2–22,1 dB | ricampionatore frazionario (120,12 parole/simbolo), MER simulata 42,9 dB |
| 21/11 11:24 | 5750 MHz | 333 kS | **TS generato in tempo reale sulla C5** | tutta la prova (120 s) | 20,4–23,5 dB | VBER 0, 0 frame DUMMY, contatore a schermo senza salti del PCR |
| 21/11 11:46 | 2375 MHz | 333 kS | tempo reale | < 30 s, tenuto 120 s | 21,2–23,8 dB | VBER 0, S/N ~43,5 dB, offset +20…+22 kHz |
| 21/11 12:22 | 2375 + 5750 | 333 kS | **ciclo del beacon** 13 → 6 cm | ~20 s | 23,3 dB (6 cm) | 600 + 600 frame, 0 DUMMY; 13 cm anche sul MiniTiouner |
| 22/11 23:09 | 2375 MHz | 333 kS | beacon, +7 ppm | sì | 20,9 dB | offset **+7,5 kHz**, TS 450 kb/s, S/N 36,6 dB |
| 22/11 23:11 | 5750 MHz | 333 kS | beacon, +7 ppm | sì | 20,8 dB | offset **+10,8 kHz**, TS 386 kb/s, S/N 35,7 dB |
| 23/11 01:27 | 2375 MHz | 333 kS | **monoscopio neutro, sovrimpressione** | sì | 23,4 dB | nominativo e frequenza dalla dashboard; Pd4Pdx "Valigetta" (Pluto, schermo 7"), offset −13,8 kHz sulla sua scala, TS 515 kb/s (immagine `img/pd4pdx_7in_20261123_012733.jpg`) |
| 23/11 01:22 | 5750 MHz | 333 kS | **monoscopio neutro, sovrimpressione** | sì | 22,8 dB | Pd4Pdx, offset +10,2 kHz, TS 580 kb/s, VBER 0, S/N 32,7 dB (immagine `img/pd4pdx_hdmi_20261123_012258.jpg`) |

SR esatti a 40 MS/s: 125 kS (16 × 20), 250 kS (16 × 10), 500 kS (16 × 5), 1000 kS (8 × 5); 333 kS usa l'accumulatore frazionario.
1000 kS è disattivato in questa versione (la prova a secco blocca la C5, da indagare).

### 3.3 Correzione del quarzo (21/11/2026, ricevitore Pluto, TX di 60 s, offset da freddo a caldo)

| Prova | Banda | Correzione | Offset al ricevitore | Aggancio |
|---|---|---|---|---|
| 1 | 13 cm | 0 ppm | +26,6 → +22,7 kHz | continuo (in 5 s) |
| 2 | 13 cm | +9,26 ppm | +4,6 → +0,5 kHz | continuo |
| 3 | 6 cm | +9,26 ppm | +6,2 → −3,1 kHz | **a tratti** (il segnale attraversa la DC della Pluto a 0 Hz) |
| 4 | 6 cm | +6 ppm | +16,6 → +13,3 kHz | continuo |

* Spostamento tra prova 1 e 2 = −22,0 kHz = esattamente la correzione programmata → confermato che il secondo argomento di `phy_set_freq` è in kHz.
* Deriva termica in TX da freddo: ~1,6–1,7 ppm al minuto (≈ 4 kHz/min a 13 cm, 9 kHz/min a 6 cm).
* Valore scelto **+7 ppm**: il segnale resta qualche kHz sopra il centro del ricevitore (DC della Pluto) in tutte e due le bande.

## 4. Codificatore e tempi (sulla C5, un solo core)

| Voce | Valore |
|---|---|
| Verifica | 42/42 frame identici al codificatore di riferimento in Python; percorso in tempo reale 8/8 frame, 0 simboli diversi |
| Tempo per frame (CPU libera) | BCH 2,72 + LDPC 2,34 + mappatura 3,11 = **8,2 ms** (un frame dura 99,9 ms a 333 kS) |
| Costo del produttore in tempo reale | ~1,66 M cicli/frame, passo ≤ 9000 cicli (margine adattivo) |
| Riempimento del banco (QPSK, H=8) | 60,3 k su 96 k cicli per scambio |
| Ritardo dello scambio | max 4954–6114 cicli nelle prove del 21/11 sera; 8–35 riempimenti in ritardo ogni 60 s con 0 frame DUMMY (da indagare) |
| TS | 483 541 bit/s a 333 kS (in 60 s: 8903 pacchetti video, 2816 audio, 4743 nulli) |
| Latenza dello STOP (BOOT o GPIO STOP) | controllo ogni 256 scambi ≈ 80 ms |

## 5. Mappa della memoria (perché il progetto è fatto così)

| Zona | Uso |
|---|---|
| 0x40800000–0x4081FFFF (banco 0) | codice, dati, stack; `_bss_end` 0x4081FB10 (guardia in `sram_guard.ld`) |
| 0x40820000–0x4083FFFF | banco A del DAC / banco di cattura (della radio durante la TX) |
| 0x40840000–0x4084BFFF | banco B del DAC (12 000 parole), prestato al heap quando c'è l'access point |
| RAM LP 0x50000000 | tabella BCH |
| Flash 0x290000 | monoscopio neutro PDXES002 per le due bande (H.265 + AAC + tabelle della sovrimpressione, 761 252 B) |
| Flash 0x3F0000 | strisce del nominativo e della frequenza composte dalla C5 (0x8000 per banda, riscritte solo se il testo cambia) |

## 6. Configurazione (NVS, versione 6)

| Chiave | Predefinito | Intervallo |
|---|---|---|
| call | NOCALL | 1–15 caratteri |
| f13 / f6 | 2375,000 / 5750,000 MHz | 2300–2450 / 5650–5850 MHz |
| sr | 333000 | 125000, 250000, 333000, 500000 |
| amp | 250 (−5,7 dB) | 20–300 |
| tx / pause / wait | 60 / 30 / 30 s | 10–600 / 5–3600 / 5–300 |
| bands | 3 (entrambe) | 1 = 13 cm, 2 = 6 cm |
| auto | 0 | 0/1 |
| ant13 / ant6 | 1 (IPEX) | 0 = PCB, 1 = IPEX |
| gpio13 / gpio6 / gpioptt | −1 | 0–10, 23, 24, 25 |
| gpiostop | 10 | ingresso, attivo a GND |
| gpiopol | 1 (attivi alti) | 0/1 |
| ppm | 0 | −100…+100 (passo 0,01) |
| appass | pdxbeacon | 8–15 caratteri |
| Wi-Fi | SSID `C5_13-6CM_BEACON`, http://192.168.4.1 | |

## 7. Limiti noti

* Niente PSRAM: il "Parrot" in ricezione del progetto ESP32-S3 non sta sulla C5.
* Durante la TX la radio Wi-Fi fa da modulatore: la dashboard si raggiunge solo nelle pause e in configurazione.
* Il monoscopio è un file H.265 già codificato: la C5 non ha un codificatore video.
* Deriva di frequenza mentre la scheda si scalda (vedi 3.3): la compensazione con la temperatura è il prossimo passo.
