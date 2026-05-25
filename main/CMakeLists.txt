idf_component_register(
  SRCS "LedPattern.cpp" "main.cpp"
  PRIV_REQUIRES "spi_flash" "esp_driver_gpio" "esp_driver_spi" "esp_timer" "bt" "nvs_flash"
  INCLUDE_DIRS "")
