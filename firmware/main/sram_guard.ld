/* ESP32-C5 Dual-Band DATV - IZ7PDX - GPL-3.0
 * Il banco 0x40820000-0x4083FFFF (e 0x40840000-0x4084BFFF) va alla radio durante la TX: codice e dati statici devono
 * finire sotto 0x40820000, altrimenti la compilazione si ferma (idea da ESP-SDR di ESPARGOS, main/targets/esp32c5). */
ASSERT(_iram_end <= 0x40820000, "C5: IRAM dentro il banco della radio (0x40820000)")
ASSERT(_data_end <= 0x40820000, "C5: .data dentro il banco della radio (0x40820000)")
ASSERT(_bss_end <= 0x40820000, "C5: .bss dentro il banco della radio (0x40820000)")
