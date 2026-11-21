idf_component_register(SRCS "main.c" "rf_c5.c" "dvbs2_enc.c" "s2_test.c" "ts_mux.c" "led_status.c" "beacon.c" "web_c5.c"
                       EMBED_TXTFILES "web/index.html"
                       REQUIRES esp_phy esp_wifi esp_netif esp_event nvs_flash esp_rom esp_timer esp_hw_support esp_partition
                                esp_driver_gpio esp_driver_usb_serial_jtag esp_driver_rmt esp_http_server)

# controllo dei limiti del banco 0 (sram_guard.ld)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-T${CMAKE_CURRENT_SOURCE_DIR}/sram_guard.ld")
