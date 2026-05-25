/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <sdkconfig.h>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <nvs_flash.h>

#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gatts_api.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <math.h>

#include <button_gpio.h>
#include <iot_button.h>

#include "LedPattern.h"

#include <array>

// constants
static constexpr uint8_t kLedStripLength = 8;
static constexpr uint32_t kRenderTimerPeriodMs = 33;
// static constexpr uint32_t kStartupFlashMs = 250;
static constexpr uint32_t kButtonDebounceMs = 30;
static constexpr uint32_t kButtonShortPressMs = 180;
static constexpr uint32_t kButtonLongPressMs = 1500;
static constexpr gpio_num_t kGpioButton = GPIO_NUM_2;
static constexpr gpio_num_t kGpioLedStripClock = GPIO_NUM_8;
static constexpr gpio_num_t kGpioLedStripData = GPIO_NUM_10;  // mosi
static constexpr uint32_t kRenderTaskStackSize = 3072;

// globals
static const char* TAG = "main";
static TaskHandle_t render_task_handle;
static TaskHandle_t render_stop_waiter_handle;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static LedPattern current_led_pattern = LedPattern::kSolidRed;
static bool stop_render_requested = false;
static bool in_setup_mode = false;

// deep sleep saved
RTC_DATA_ATTR LedPattern saved_led_pattern = LedPattern::kSolidRed;

static void InitializeBluetooth()
{
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK( nvs_flash_erase() );
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK( err );

  // ESP32-C3 supports BLE only, so release classic BT memory.
  ESP_ERROR_CHECK( esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT) );

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK( esp_bt_controller_init(&bt_cfg) );
  ESP_ERROR_CHECK( esp_bt_controller_enable(ESP_BT_MODE_BLE) );
  ESP_ERROR_CHECK( esp_bluedroid_init() );
  ESP_ERROR_CHECK( esp_bluedroid_enable() );

  ESP_LOGI(TAG, "Bluetooth BLE stack initialized");
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

  // Hold SPI pins low through deep sleep so SK9822 LEDs don't glitch
  std::array<gpio_num_t, 2> spi_pins = { kGpioLedStripData, kGpioLedStripClock };
  for (gpio_num_t pin : spi_pins) {
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
    gpio_hold_en(pin);
  }
  gpio_deep_sleep_hold_en();

  // set interrupt to wake up on button press and go into deep sleep
  esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(BIT(kGpioButton), ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

static void render_task(void* __unused(argp))
{
  LedPattern pattern = LedPattern::kSolidRed;
  PatternContext pattern_context;

  static spi_device_handle_t device_handler = nullptr;

  led_strip_spi_t led_strip = {
    .buf = NULL,
    .length = kLedStripLength,
    .host_device = SPI2_HOST,
    .mosi_io_num = kGpioLedStripData,
    .sclk_io_num = kGpioLedStripClock,
    .max_transfer_sz = LED_STRIP_SPI_BUFFER_SIZE(kLedStripLength),
    .clock_speed_hz = 1 * 1000 * 1000,
    .queue_size = 1,
    .device_handle = device_handler,
    .dma_chan = LED_STRIP_SPI_DEFAULT_DMA_CHAN,
    .transaction = {},
  };

  ESP_ERROR_CHECK( led_strip_spi_init(&led_strip) );

  bool should_stop = false;
  bool setup_mode = false;

  while (true) {
    LedPattern next_pattern = pattern;

    portENTER_CRITICAL(&state_lock);
    next_pattern = current_led_pattern;
    should_stop = stop_render_requested;
    setup_mode = in_setup_mode;
    portEXIT_CRITICAL(&state_lock);

    if (next_pattern != pattern) {
      pattern = next_pattern;
      pattern_context.reset();
    }

    if (should_stop) {
      break;
    }

    pattern_context.clock.now_us = esp_timer_get_time();

    if (!setup_mode) {
      RenderLedPattern(&led_strip, kLedStripLength, pattern, &pattern_context);
    }
    else {
      static bool stated = false;
      if (!stated) {
        ESP_LOGI(TAG, "In setup mode, showing solid blue pattern");
        stated = true;
      }
      // In setup mode, just show a blue light to indicate bluetooth mode.
      ESP_ERROR_CHECK( RenderSolidColor(&led_strip,
        kLedStripLength, rgb_t{ .r = 0, .g = 0, .b = 255 }, 20)
      );
      ESP_ERROR_CHECK( led_strip_spi_flush(&led_strip) );
    }

    // Sleep until next frame, but allow immediate wake-up when stop is requested.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kRenderTimerPeriodMs));
  }

  // Ensure LEDs are physically driven to off before stopping the task.
  ESP_ERROR_CHECK( RenderSolidColor(&led_strip, kLedStripLength, rgb_t{ .r = 0, .g = 0, .b = 0 }, 0) );
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
  PatternDefinition::InitializePatternConfigs();

  gpio_deep_sleep_hold_dis();

  ESP_ERROR_CHECK( gpio_hold_dis(kGpioLedStripData) );
  ESP_ERROR_CHECK( gpio_hold_dis(kGpioLedStripClock) );
  ESP_ERROR_CHECK( led_strip_spi_install() );

  button_config_t const button_config = {
      .long_press_time = kButtonLongPressMs,
      .short_press_time = kButtonShortPressMs,
  };

  button_gpio_config_t const button_gpio_config = {
      .gpio_num = kGpioButton,
      .active_level = 0,
      .enable_power_save = false,
      .disable_pull = false,
  };

  button_handle_t button = nullptr;
  ESP_ERROR_CHECK( iot_button_new_gpio_device(&button_config, &button_gpio_config, &button) );
  ESP_ERROR_CHECK( iot_button_register_cb(button, BUTTON_SINGLE_CLICK, nullptr,
    [](void* __unused(button), void* __unused(user_data))
    {
      portENTER_CRITICAL(&state_lock);
      if (!in_setup_mode) {
        current_led_pattern = GetNextLedPattern(current_led_pattern);
      }
      portEXIT_CRITICAL(&state_lock);
    }, nullptr));

  button_event_args_t button_long_press_event_args = {
    .long_press = {
      .press_time = kButtonLongPressMs,
    },
  };

  ESP_ERROR_CHECK( iot_button_register_cb(button, BUTTON_LONG_PRESS_START, &button_long_press_event_args,
    [](void* button_handle, void* __unused(user_data))
    {
      button_handle_t b = reinterpret_cast<button_handle_t>(button_handle);
      enter_deep_sleep_mode(b);
    }, nullptr));

  button_event_args_t button_multiple_click_event_args = {
    .multiple_clicks = {
      .clicks = 3,
    },
  };

  ESP_ERROR_CHECK( iot_button_register_cb(button, BUTTON_MULTIPLE_CLICK, &button_multiple_click_event_args,
    [](void* __unused(button), void* __unused(user_data))
    {
      bool setup_mode = false;

      portENTER_CRITICAL(&state_lock);
      setup_mode = in_setup_mode;
      if (!setup_mode) in_setup_mode = true;
      portEXIT_CRITICAL(&state_lock);

      if (!setup_mode) {
        InitializeBluetooth();
      }
    }, nullptr));

  auto const wakeup_causes = esp_sleep_get_wakeup_causes();
  if (wakeup_causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) {
    current_led_pattern = saved_led_pattern;
  }

  auto const task_created = xTaskCreate(render_task, "render_task", kRenderTaskStackSize, NULL, 9, &render_task_handle);
  if (task_created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create render task");
    return;
  }
}
