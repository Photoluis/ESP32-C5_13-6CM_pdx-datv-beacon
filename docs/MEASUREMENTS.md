# Measurements and parameters — ESP32-C5 13/6 cm DATV beacon

[Italiano](MISURE_it.md) · [日本語](MEASUREMENTS_ja.md)

Every number below was measured on real hardware during development (15–22 November 2026).
Receivers: the **Pd4Pdx** (ADALM-Pluto + leandvb DVB-S2 decoder; two units: main station and "Valigetta") and a **MiniTiouner**.
Spectrum views: Pluto-based spectrum display, 10 MHz span. Levels are relative (Pluto dBFS), not calibrated.

## 1. Hardware under test

| Item | Value |
|---|---|
| Board | Waveshare **ESP32-C5-Zero** (module ESP32-C5HF4) |
| Chip | ESP32-C5 rev 1.0, Wi-Fi 6 dual band, 1 RISC-V HP core @ 240 MHz + LP core |
| Flash / PSRAM | 4 MB in package / **none** |
| Crystal | 48 MHz |
| RF outputs | ANT_2G + ANT_5G → diplexer → RF switch on **GPIO26**: low = ceramic (PCB) antenna, **high = IPEX** |
| LED | WS2812 on GPIO27 · BOOT button GPIO28 |
| Antenna used | wide-band antenna on IPEX (covers 2.4 and 5.7 GHz) |
| Toolchain | ESP-IDF v6.1 |

## 2. How the C5 transmits I/Q (found by experiment)

The vendor libraries of the C5 no longer contain the TX replay code (`dactrig` is empty), so every register below was found by test.

| Parameter | Value |
|---|---|
| DAC replay control | `0x600A900C`: [13:0] length, bit19 loop, bit31 start, bit18 end |
| Sample word | I = bits 0–9, Q = bits 10–19 (10-bit signed); spectrum **not inverted** |
| Sample rate | 80 MS/s (3.000 CPU cycles/sample); **bit17 = 40 MS/s** (6.000 cycles/sample) |
| Source field | `0x600A9008` bits 17–20 must be `0xF` (lost at power-off, set before every TX) |
| Memory banks | owner register `0x60095004`: 0x200 → bank 0x40820000, 0x400 → 0x40840000; the engine follows the owner on the fly (ping-pong tested up to 100 000 swaps/2 s) |
| Start latency | ~45–50 CPU cycles, jitter ±10 |
| PLL tuning | `phy_set_freq(MHz, kHz)` — integer MHz + kHz offset (1 kHz step) |
| Frequencies accepted | 2320, 2375, 5660, 5760, 5800 MHz tested without error |

## 3. RF results

### 3.1 First signals (16/11/2026)

| Test | Frequency | Result |
|---|---|---|
| Library tone (REFTONE) | 2375 MHz | line −5.6 dB |
| DAC replay, amplitude 0 (carrier only) | 2375 MHz | −33.4 dB |
| DAC replay, amplitude 200 | 2375 MHz | −5.0 dB (+28 dB) |
| Tone k=±16 @ 80 MS/s | 2375 MHz | lines at 2375.80 / 2374.20 MHz, image and spurs ≥ 55 dB down, LO leak ~−45 dBc |
| Tone k=±16 | 5750 MHz | lines at 5750.82 / 5749.21 MHz, −25 dB, S/N 28 dB |
| Real-time streaming (two banks, 40 MS/s) | 5750 MHz | 4 cycles/word, 0 overruns in 80 000 swaps, single clean line |
| QPSK RRC 0.35, 333 kS (PRBS) | 5750 MHz | flat 0.40 MHz wide, shoulders ~39 dB down, H=8 repeats at ±5 MHz ~34 dB down |

### 3.2 DVB-S2 (QPSK 3/4, normal frames, pilots, H.265 + AAC test card)

| Date | Freq | SR | Source | Lock | MER | Notes |
|---|---|---|---|---|---|---|
| 20/11 19:22 | 5750 MHz | 250 kS | pre-computed symbols | ~45 s | 22.5–23.2 dB | VBER 0, offset +48.5…+49.8 kHz, S/N 32–34 dB |
| 20/11 18:36 | 5750 MHz | "333 kS" = 333 333 S/s | pre-computed | **no lock** | — | +1000 ppm symbol-rate error vs 333 000 expected by the receiver |
| 20/11 19:31 | 5750 MHz | **333 000 S/s exact** | pre-computed | ~30 s, held 90 s | 20.2–22.1 dB | fractional resampler (120.12 words/symbol), simulated MER 42.9 dB |
| 21/11 11:24 | 5750 MHz | 333 kS | **TS generated live on the C5** | whole test (120 s) | 20.4–23.5 dB | VBER 0, 0 DUMMY frames, counter on screen without PCR jumps |
| 21/11 11:46 | 2375 MHz | 333 kS | live | < 30 s, held 120 s | 21.2–23.8 dB | VBER 0, S/N ~43.5 dB, offset +20…+22 kHz |
| 21/11 12:22 | 2375 + 5750 | 333 kS | **beacon cycle** 13 → 6 cm | ~20 s | 23.3 dB (6 cm) | 600 + 600 frames, 0 DUMMY; 13 cm also on MiniTiouner |
| 22/11 23:09 | 2375 MHz | 333 kS | beacon, +7 ppm | yes | 20.9 dB | offset **+7.5 kHz**, TS 450 kb/s, S/N 36.6 dB |
| 22/11 23:11 | 5750 MHz | 333 kS | beacon, +7 ppm | yes | 20.8 dB | offset **+10.8 kHz**, TS 386 kb/s, S/N 35.7 dB |
| 23/11 01:27 | 2375 MHz | 333 kS | **neutral test card, overlay** | yes | 23.4 dB | callsign and frequency from the dashboard; Pd4Pdx "Valigetta" (Pluto, 7" screen), offset −13.8 kHz on its scale, TS 515 kb/s (picture `img/pd4pdx_7in_20261123_012733.jpg`) |
| 23/11 01:22 | 5750 MHz | 333 kS | **neutral test card, overlay** | yes | 22.8 dB | Pd4Pdx, offset +10.2 kHz, TS 580 kb/s, VBER 0, S/N 32.7 dB (picture `img/pd4pdx_hdmi_20261123_012258.jpg`) |

Exact symbol rates at 40 MS/s: 125 kS (16 × 20), 250 kS (16 × 10), 500 kS (16 × 5), 1000 kS (8 × 5); 333 kS uses the fractional accumulator.
1000 kS is disabled in this version (dry test locks the C5, under investigation).

### 3.3 Crystal correction (21/11/2026, Pluto receiver, 60 s TX each, offset cold → warm)

| Test | Band | Correction | Receiver offset | Lock |
|---|---|---|---|---|
| 1 | 13 cm | 0 ppm | +26.6 → +22.7 kHz | continuous (in 5 s) |
| 2 | 13 cm | +9.26 ppm | +4.6 → +0.5 kHz | continuous |
| 3 | 6 cm | +9.26 ppm | +6.2 → −3.1 kHz | **intermittent** (signal crosses the Pluto DC at 0 Hz) |
| 4 | 6 cm | +6 ppm | +16.6 → +13.3 kHz | continuous |

* Shift between test 1 and 2 = −22.0 kHz = exactly the programmed correction → the kHz argument of `phy_set_freq` is confirmed.
* Thermal drift during TX from cold: ~1.6–1.7 ppm/minute (≈ 4 kHz/min at 13 cm, 9 kHz/min at 6 cm).
* Chosen setting **+7 ppm**: keeps the signal a few kHz above the receiver centre (Pluto DC) in both bands.

## 4. Encoder and timing (on the C5, single core)

| Item | Value |
|---|---|
| Reference check | 42/42 frames identical to the Python reference encoder; live path 8/8 frames, 0 symbol errors |
| Time per frame (CPU free) | BCH 2.72 + LDPC 2.34 + mapping 3.11 = **8.2 ms** (frame lasts 99.9 ms at 333 kS) |
| Live producer cost | ~1.66 M cycles/frame, step ≤ 9000 cycles (adaptive margin) |
| Bank fill (QPSK, H=8) | 60.3 k of 96 k cycles per swap |
| Swap delay | max 4954–6114 cycles in the 21/11 evening tests; 8–35 late fills per 60 s with 0 DUMMY frames (being investigated) |
| TS rate | 483 541 bit/s at 333 kS (video 8903, audio 2816, null 4743 packets per 60 s) |
| Stop latency (BOOT or STOP GPIO) | checked every 256 swaps ≈ 80 ms |

## 5. Memory map (why the design is shaped this way)

| Region | Use |
|---|---|
| 0x40800000–0x4081FFFF (bank 0) | code, data, stacks; `_bss_end` 0x4081FB10 (guard in `sram_guard.ld`) |
| 0x40820000–0x4083FFFF | DAC bank A / capture bank (owned by the radio during TX) |
| 0x40840000–0x4084BFFF | DAC bank B (12 000 words), lent to the heap when the access point runs |
| LP RAM 0x50000000 | BCH table |
| Flash 0x290000 | neutral test card PDXES002 for both bands (H.265 + AAC + overlay tables, 761 252 B) |
| Flash 0x3F0000 | callsign and frequency strips composed by the C5 (0x8000 per band, rewritten only when the text changes) |

## 6. Configuration (NVS, version 6)

| Key | Default | Range |
|---|---|---|
| call | NOCALL | 1–15 characters |
| f13 / f6 | 2375.000 / 5750.000 MHz | 2300–2450 / 5650–5850 MHz |
| sr | 333000 | 125000, 250000, 333000, 500000 |
| amp | 250 (−5.7 dB) | 20–300 |
| tx / pause / wait | 60 / 30 / 30 s | 10–600 / 5–3600 / 5–300 |
| bands | 3 (both) | 1 = 13 cm, 2 = 6 cm |
| auto | 0 | 0/1 |
| ant13 / ant6 | 1 (IPEX) | 0 = PCB, 1 = IPEX |
| gpio13 / gpio6 / gpioptt | −1 | 0–10, 23, 24, 25 |
| gpiostop | 10 | input, active to GND |
| gpiopol | 1 (active high) | 0/1 |
| ppm | 0 | −100…+100 (0.01 step) |
| appass | pdxbeacon | 8–15 characters |
| Wi-Fi | SSID `C5_13-6CM_BEACON`, http://192.168.4.1 | |

## 7. Known limits

* No PSRAM: the receiving "Parrot" of the ESP32-S3 project does not fit on the C5.
* During TX the Wi-Fi radio is the modulator: the dashboard is only reachable in the pauses and in configuration.
* The test card is a pre-encoded H.265 file: the C5 has no video encoder.
* Frequency drift while warming up (see 3.3): temperature compensation is the next step.
