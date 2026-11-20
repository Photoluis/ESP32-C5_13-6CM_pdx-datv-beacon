idf_component_register(SRCS "main.c" "rf_c5.c" "dvbs2_enc.c" "s2_test.c"
                       REQUIRES esp_phy esp_wifi esp_netif esp_event nvs_flash esp_rom esp_timer esp_hw_support esp_partition
                                esp_driver_gpio esp_driver_usb_serial_jtag)
