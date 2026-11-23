# ESP32-C5_13-6CM_pdx-datv-beacon

**Farol DATV DVB-S2 de banda dupla (13 cm e 6 cm) num único ESP32-C5, que usa o transmissor Wi-Fi como modulador I/Q.**

[English](README.md) · [Italiano](README_it.md) · [Español](README_es.md) · [Deutsch](README_de.md) · **Português** · [العربية](README_ar.md) · [日本語](README_ja.md)

Painel em 11 línguas · Licença GPL-3.0

| 13 cm — 2375,000 MHz | 6 cm — 5750,000 MHz |
|---|---|
| ![DVB-S2 a 2375 MHz com indicativo e frequência sobrepostos, LOCK, MER 23,4 dB](docs/img/valigetta_touch_20261123_012733.jpg) | ![DVB-S2 a 5750 MHz com indicativo e frequência sobrepostos, LOCK, MER 22,8 dB](docs/img/pd4pdx_hdmi_20261123_012258.jpg) |

*Padrão de teste neutro com indicativo e frequência definidos no painel. 13 cm recebidos por um Portsdown 4 com Pluto (ecrã de 7", MER 23,4 dB), 6 cm pelo Pd4Pdx (Pluto + leandvb, MER 22,8 dB, VBER 0): QPSK 3/4, 333 kS, H.265 + AAC.*

<p align="center"><img src="docs/img/photo_esp32c5_zero_ipex.jpg" height="380" alt="ESP32-C5-Zero com cabo IPEX"> <img src="docs/img/photo_bench_c5_6cm.jpg" height="380" alt="Bancada de teste com o Pd4Pdx a receber 6 cm"></p>

*O ESP32-C5-Zero na bancada (LED azul = TX em 6 cm, antena de banda larga no IPEX) e o Pd4Pdx a recebê-lo em 5750 MHz.*

## O que faz

* Gera um sinal **DVB-S2 QPSK 3/4** (tramas normais, pilotos) a **125 / 250 / 333 / 500 kS**, com o transport stream MPEG (padrão de teste em vídeo H.265 + áudio AAC, SDT com o seu indicativo) **construído em tempo real no chip**.
* Transmite alternando **13 cm (2300–2450 MHz)** e **6 cm (5650–5850 MHz)** num ciclo de farol: TX 13 cm → pausa → TX 6 cm → pausa.
* Funciona numa **Waveshare ESP32-C5-Zero** (ESP32-C5, um só núcleo RISC-V, flash de 4 MB, sem PSRAM). Sem DAC externo e sem FPGA.
* **Painel web** no seu próprio ponto de acesso (`C5_13-6CM_BEACON`, http://192.168.4.1) em 11 línguas: indicativo, frequências, symbol rate, nível, antena por banda (PCB ou IPEX), GPIO para relés externos / PTT, tempos do ciclo, **correção do cristal em ppm** com calculadora.
* **Paragem sem botões:** duplo arranque, **GPIO STOP** à massa (interruptor, relé, comando remoto) ou STOP no painel.

| Painel (telemóvel) | | |
|---|---|---|
| ![Português](docs/img/dashboard_pt.png) | ![English](docs/img/dashboard_en.png) | ![Español](docs/img/dashboard_es.png) |

Todas as línguas: [IT](docs/img/dashboard_it.png) · [DE](docs/img/dashboard_de.png) · [FR](docs/img/dashboard_fr.png) · [NL](docs/img/dashboard_nl.png) · [PL](docs/img/dashboard_pl.png) · [EL](docs/img/dashboard_el.png) · [JA](docs/img/dashboard_ja.png) · [AR](docs/img/dashboard_ar.png)

## Como funciona (resumo)

O rádio do ESP32-C5 tem um motor interno que reproduz amostras (a Espressif usa-o nas calibrações). O seu código de transmissão já não está nas bibliotecas do fabricante, por isso os registos foram encontrados por ensaio: dois bancos de amostras a 40 MS/s tocados em ping-pong e preenchidos pela CPU com QPSK filtrada RRC enquanto o outro banco está no ar, com 333 000 S/s exatos graças a um acumulador fracionário. BCH, LDPC, baralhamento, trama PL e multiplexador do transport stream correm todos no único núcleo entre trocas de banco. Detalhes e todos os valores medidos: **[docs/MEASUREMENTS.md](docs/MEASUREMENTS.md)** (inglês).

## Hardware

| Item | Notas |
|---|---|
| Placa | Waveshare ESP32-C5-Zero (ESP32-C5HF4, flash de 4 MB) |
| Antena | cerâmica na placa ou **IPEX** (escolhida por GPIO26 no painel); nos testes, antena de banda larga para 2,4 e 5,7 GHz |
| GPIO | 26 comutador de antena · 27 LED de estado (amarelo = espera, vermelho = TX 13 cm, azul = TX 6 cm) · 28 BOOT · **10 STOP** (entrada, a GND) · saídas opcionais 0–10, 23–25 para relés de banda e PTT |
| Alimentação | USB-C |

O nível de saída é de poucos miliwatts: para uso real são necessários um filtro e um amplificador externos.

## Compilar e gravar

Requer **ESP-IDF v6.1** (target `esp32c5`).

```
cd firmware
idf.py set-target esp32c5
idf.py build
esptool --chip esp32c5 --port COMx -b 921600 write-flash 0x2000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/pdx_c5_probe.bin
```

O farol precisa também do padrão de teste neutro na flash em `0x290000` (um só ficheiro para as duas bandas, cerca de 760 KB). É gerado no PC com `tools/monoscope/make_es_c5.py` (ffmpeg com libx265 e a fonte livre DejaVu); o indicativo e a frequência **não** estão no ficheiro: o C5 sobrepõe-nos a partir das definições do painel.

```
cd tools/monoscope
python make_es_c5.py -o ../out/c5_mono_es_neutral.bin
esptool --chip esp32c5 --port COMx -b 921600 write-flash 0x290000 ../out/c5_mono_es_neutral.bin
```

## Primeira utilização

1. Alimente a placa: arranca em **configuração** (sem TX, LED amarelo fixo) porque o arranque automático está desligado.
2. Ligue o telemóvel ao Wi-Fi **`C5_13-6CM_BEACON`** (palavra-passe `pdxbeacon`) e abra **http://192.168.4.1**.
3. Defina indicativo, frequências, antena e correção do cristal, prima **Guardar** e depois **START**.
4. Durante a TX o Wi-Fi serve de modulador: o painel volta em cada pausa.

### Parar o farol sem botões

* **Duplo arranque:** ligue, retire a alimentação em menos de 5 s e volte a ligar → configuração, sem TX.
* **GPIO STOP:** GPIO10 a GND para a TX de imediato e reinicia em configuração com o Wi-Fi ligado; à massa ao ligar impede qualquer TX.
* **Painel:** STOP / CONFIG durante uma pausa.
* Com botões: BOOT para sempre, também em TX.

## Estado

Primeira fase do projeto concluída (novembro de 2026). Testado no ar: DVB-S2 em 13 e 6 cm, transport stream em tempo real, ciclo do farol, painel, GPIO STOP, correção do cristal.

Próximos passos:
* compensação por temperatura da deriva do cristal durante a TX (~1,6 ppm/min a frio);
* symbol rate de 1000 kS;
* ligação SPI a um ESP32-S3 (painel durante a TX, transponder de banda cruzada, streaming a partir do telemóvel).

## Nota legal

Este é um projeto de **radioamador**. Transmitir em 13 cm e 6 cm exige licença de radioamador e o respeito pelo plano de bandas e pelos limites de potência do seu país. O rádio Wi-Fi é usado fora dos seus canais e da sua modulação: é responsável pelo que transmite.

## Licença e créditos

GPL-3.0 — ver [LICENSE](LICENSE). © 2026 Luigi D'Arcangelo, IZ7PDX.
Baseado no codificador DVB-S2 e no transport stream do [ESP32-S3-CAM_PDX DATV Beacon](https://github.com/Photoluis/Esp32s3-cam-pdx-datv-beacon).
