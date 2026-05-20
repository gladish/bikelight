/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <sdkconfig.h>

#include <esp_log.h>
#include <esp_sleep.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <button_gpio.h>
#include <iot_button.h>
#include <led_strip_spi.h>

// constants
static constexpr uint8_t kLedModeCount = 6;
static constexpr uint8_t kLedStripLength = 8;
static constexpr uint32_t kRenderTimerPeriodMs = 33;
static constexpr uint32_t kStartupFlashMs = 250;
static constexpr uint32_t kButtonDebounceMs = 30;
static constexpr uint32_t kButtonShortPressMs = 180;
static constexpr uint32_t kButtonLongPressMs = 1500;
static constexpr gpio_num_t kGpioButton = GPIO_NUM_2;
static constexpr gpio_num_t kGpioLedStringClock = GPIO_NUM_8;
static constexpr gpio_num_t kGpioLedStripData = GPIO_NUM_10;  // mosi
static constexpr uint32_t kButtonTaskStackSize = 3072;
static constexpr uint32_t kRenderTaskStackSize = 3072;

// globals
static const char* TAG = "main";
static TaskHandle_t render_task_handle;
static TaskHandle_t render_stop_waiter_handle;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t current_led_pattern = 1;
static bool in_setup_mode = false;
static bool stop_render_requested = false;

// deep sleep saved
RTC_DATA_ATTR int saved_led_pattern = 0;

static void render_solid_color_pattern(led_strip_spi_t* leds, uint8_t n, rgb_t color, uint8_t brightness)
{
  for (int i = 0; i < n; i++) {
    ESP_ERROR_CHECK( led_strip_spi_set_pixel_brightness(leds, i, color, brightness) );
  }
}

static void render_led_pattern(led_strip_spi_t* leds, uint8_t n, uint8_t pattern)
{
  static rgb_t const kColorRed = rgb_t{
    .r = 255,
    .g = 0,
    .b = 0
  };

  static rgb_t const kColorGreen = rgb_t{
    .r = 0,
    .g = 255,
    .b = 0
  };

  static rgb_t const kColorBlue = rgb_t{
    .r = 0,
    .g = 0,
    .b = 255
  };

  static rgb_t const kColorYellow = rgb_t{
    .r = 255,
    .g = 255,
    .b = 0
  };

  static rgb_t const kColorCyan = rgb_t{
    .r = 0,
    .g = 255,
    .b = 255
  };

  static rgb_t const kColorMagenta = rgb_t{
    .r = 255,
    .g = 0,
    .b = 255
  };


  static uint8_t default_brightness = 50;

  switch (pattern) {
    case 1:
      render_solid_color_pattern(leds, n, kColorRed, default_brightness);
      break;
    case 2:
      render_solid_color_pattern(leds, n, kColorGreen, default_brightness);
      break;
    case 3:
      render_solid_color_pattern(leds, n, kColorBlue, default_brightness);
      break;
    case 4:
      render_solid_color_pattern(leds, n, kColorYellow, default_brightness);
      break;
    case 5:
      render_solid_color_pattern(leds, n, kColorCyan, default_brightness);
      break;
    case 6:
      render_solid_color_pattern(leds, n, kColorMagenta, default_brightness);
      break;
    case 7:
      render_solid_color_pattern(leds, n, kColorRed, default_brightness);
      break;
    default:
      render_solid_color_pattern(leds, n, kColorGreen, default_brightness);
      break;
  }

  ESP_ERROR_CHECK( led_strip_spi_flush(leds) );
}

static void enter_deep_sleep_mode(button_handle_t mode_button)
{
  TaskHandle_t render_task = nullptr;

  portENTER_CRITICAL(&state_lock);
  saved_led_pattern = current_led_pattern;
  stop_render_requested = true;
  render_task = render_task_handle;
  render_stop_waiter_handle = xTaskGetCurrentTaskHandle();
  portEXIT_CRITICAL(&state_lock);

  if (render_task != nullptr) {
    // Wake render task immediately so it can process stop request and exit.
    xTaskNotifyGive(render_task);

    // Wait for render task to exit and flush all-zero frame.
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0) {
      ESP_LOGW(TAG, "Timed out waiting for render task to stop");
    }
  }

  // wait for user to release button
  while (iot_button_get_key_level(mode_button) == 1) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  // set interrupt to wake up on button press and go into deep sleep
  esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(BIT(kGpioButton), ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

static void render_task(void* __unused(argp))
{
  uint8_t pattern = 1;

  static spi_device_handle_t device_handler = nullptr;

  led_strip_spi_t led_strip = {
    .buf = NULL,
    .length = kLedStripLength,
    .host_device = SPI2_HOST,
    .mosi_io_num = kGpioLedStripData,
    .sclk_io_num = kGpioLedStringClock,
    .max_transfer_sz = LED_STRIP_SPI_BUFFER_SIZE(kLedStripLength),
    .clock_speed_hz = 1 * 1000 * 1000,
    .queue_size = 1,
    .device_handle = device_handler,
    .dma_chan = LED_STRIP_SPI_DEFAULT_DMA_CHAN,
    .transaction = {},
  };

  ESP_ERROR_CHECK( led_strip_spi_init(&led_strip) );

  while (true) {
    bool should_stop = false;

    portENTER_CRITICAL(&state_lock);
    pattern = current_led_pattern;
    should_stop = stop_render_requested;
    portEXIT_CRITICAL(&state_lock);

    if (should_stop) {
      break;
    }

    render_led_pattern(&led_strip, kLedStripLength, pattern);

    // Sleep until next frame, but allow immediate wake-up when stop is requested.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kRenderTimerPeriodMs));
  }

  // Ensure LEDs are physically driven to off before stopping the task.
  render_solid_color_pattern(&led_strip, kLedStripLength, rgb_t{ .r = 0, .g = 0, .b = 0 }, 0);
  ESP_ERROR_CHECK( led_strip_spi_flush(&led_strip) );

  TaskHandle_t waiter = nullptr;
  portENTER_CRITICAL(&state_lock);
  waiter = render_stop_waiter_handle;
  render_task_handle = nullptr;
  render_stop_waiter_handle = nullptr;
  portEXIT_CRITICAL(&state_lock);

  if (waiter != nullptr) {
    xTaskNotifyGive(waiter);
  }

  vTaskDelete(nullptr);
}


extern "C" void app_main(void)
{
  esp_err_t err;

  ESP_ERROR_CHECK( led_strip_spi_install() );

  const button_config_t button_config = {
      .long_press_time = kButtonLongPressMs,
      .short_press_time = kButtonShortPressMs,
  };
  const button_gpio_config_t button_gpio_config = {
      .gpio_num = kGpioButton,
      .active_level = 0,
      .enable_power_save = false,
      .disable_pull = false,
  };

  button_handle_t button = nullptr;
  ESP_ERROR_CHECK( iot_button_new_gpio_device(&button_config, &button_gpio_config, &button) );

  err = iot_button_register_cb(button, BUTTON_SINGLE_CLICK, nullptr, [](void* __unused(button), void* __unused(user_data)) {
    uint8_t new_pattern = 1;
    portENTER_CRITICAL(&state_lock);
    current_led_pattern = (current_led_pattern % kLedModeCount) + 1;
    new_pattern = current_led_pattern;
    portEXIT_CRITICAL(&state_lock);

    ESP_LOGI(TAG, "Button single click, current pattern: %u", new_pattern);
  }, nullptr);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register single-click callback: %s", esp_err_to_name(err));
    return;
  }

  button_event_args_t button_long_press_event_args = {
    .long_press = {
      .press_time = kButtonLongPressMs,
    },
  };

  err = iot_button_register_cb(button, BUTTON_LONG_PRESS_START, &button_long_press_event_args, [](void* button_handle, void* __unused(user_data)) {
    enter_deep_sleep_mode( (button_handle_t) button_handle );
  }, nullptr);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register long-press callback: %s", esp_err_to_name(err));
    return;
  }

  button_event_args_t button_multiple_click_event_args = {
    .multiple_clicks = {
      .clicks = 3,
    },
  };

  err = iot_button_register_cb(button, BUTTON_MULTIPLE_CLICK, &button_multiple_click_event_args, [](void* __unused(button), void* __unused(user_data)) {
    ESP_LOGI(TAG, "Button multiple click");
  }, nullptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register multiple-click callback: %s", esp_err_to_name(err));
    return;
  }

  auto wakeup_causes = esp_sleep_get_wakeup_causes();

  if (wakeup_causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) {
    ESP_LOGI(TAG, "Woke up by EXT0 GPIO");
    current_led_pattern = saved_led_pattern;
  }

  auto task_created = xTaskCreate(render_task, "render_task", kRenderTaskStackSize, NULL, 9, &render_task_handle);
  if (task_created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create render task");
    return;
  }
}
