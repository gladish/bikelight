idf_component_register(
  SRCS "sk9822_leds.cpp" "main.cpp" "led_patterns.cpp"
  PRIV_REQUIRES "spi_flash" "esp_driver_gpio" "esp_driver_spi" "esp_timer"
  INCLUDE_DIRS "")
