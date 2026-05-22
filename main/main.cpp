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

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <math.h>

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
static constexpr gpio_num_t kGpioLedStripClock = GPIO_NUM_8;
static constexpr gpio_num_t kGpioLedStripData = GPIO_NUM_10;  // mosi
static constexpr uint32_t kButtonTaskStackSize = 3072;
static constexpr uint32_t kRenderTaskStackSize = 3072;

// globals
static const char* TAG = "main";
static TaskHandle_t render_task_handle;
static TaskHandle_t render_stop_waiter_handle;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t current_led_pattern = 1;
static bool stop_render_requested = false;

// deep sleep saved
RTC_DATA_ATTR int saved_led_pattern = 0;

typedef struct
{
  int64_t last_update_us;
  int64_t now_us;
} animation_clock_t;

static void render_solid_color_pattern(led_strip_spi_t* leds, uint8_t n, rgb_t color, uint8_t brightness)
{
  ESP_ERROR_CHECK( led_strip_spi_set_pixels_brightness(leds, 0, n, color, brightness) );
}

static void render_chase_pattern(led_strip_spi_t* leds, uint8_t n, rgb_t color, uint8_t brightness, animation_clock_t* clock)
{
  static int index = 0;
  static bool forward = true;

  if (clock->now_us - clock->last_update_us >= 100 * 1000) {
    clock->last_update_us = clock->now_us;

    for (int i = 0; i < n; i++) {
      if (i == index) {
        ESP_ERROR_CHECK( led_strip_spi_set_pixel_brightness(leds, i, color, brightness) );
      } else {
        ESP_ERROR_CHECK( led_strip_spi_set_pixel_brightness(leds, i, color, 0) );
      }
    }

    if (forward) {
      index++;
      if (index >= n) {
        index = n - 2;
        forward = false;
      }
    } else {
      index--;
      if (index < 0) {
        index = 1;
        forward = true;
      }
    }
  }
}

static void render_pulse_pattern(led_strip_spi_t* leds, uint8_t n, rgb_t color, uint8_t max_brightness, animation_clock_t* clock)
{
  constexpr int64_t kPulsePeriodUs = 2'000'000;

  int64_t cycle_us = clock->now_us % kPulsePeriodUs;

  float phase = (float)cycle_us / (float)kPulsePeriodUs;

  float triangle;
  if (phase < 0.5f)
    triangle = phase * 2.0f;
  else
    triangle = 2.0f - (phase * 2.0f);

  uint8_t brightness = (uint8_t)(triangle * max_brightness);

  esp_err_t err = led_strip_spi_set_pixels_brightness(leds, 0, n, color, brightness);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set pixel color: %s", esp_err_to_name(err));
  }
}

static void render_strobe_pattern(led_strip_spi_t* leds, uint8_t n, rgb_t color, uint8_t brightness, animation_clock_t* clock)
{
  static bool on = false;
  static uint32_t on_duration_us = 50 * 1000;
  static uint32_t off_duration_us = 150 * 1000;

  if (on && (clock->now_us - clock->last_update_us >= on_duration_us)) {
    // turn off
    esp_err_t err = led_strip_spi_set_pixels_brightness(leds, 0, n, color, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to set pixel color: %s", esp_err_to_name(err));
    }
    on = false;
    clock->last_update_us = clock->now_us;
  } else if (!on && (clock->now_us - clock->last_update_us >= off_duration_us)) {
    // turn on
    esp_err_t err = led_strip_spi_set_pixels_brightness(leds, 0, n, color, brightness);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to set pixel color: %s", esp_err_to_name(err));
    }
    on = true;
    clock->last_update_us = clock->now_us;
  }
}

static void render_twinkle_pattern(led_strip_spi_t* leds, uint8_t n, rgb_t color, uint8_t brightness, animation_clock_t* clock)
{
  static uint32_t twinkle_interval_us = 200 * 1000;

  if (clock->now_us - clock->last_update_us >= twinkle_interval_us) {
    clock->last_update_us = clock->now_us;

    for (int i = 0; i < n; i++) {
      if (esp_random() % 2 == 0) {
        esp_err_t err = led_strip_spi_set_pixel_brightness(leds, i, color, brightness);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "Failed to set pixel color: %s", esp_err_to_name(err));
        }
      } else {
        esp_err_t err = led_strip_spi_set_pixel_brightness(leds, i, color, 0);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "Failed to set pixel color: %s", esp_err_to_name(err));
        }
      }
    }
  }
}

static void render_random_pattern(led_strip_spi_t* leds, uint8_t n, rgb_t const* colors, uint8_t color_count, uint8_t brightness, animation_clock_t* clock)
{
  static uint32_t change_interval_us = 500 * 1000;

  if (clock->now_us - clock->last_update_us >= change_interval_us) {
    clock->last_update_us = clock->now_us;

    for (int i = 0; i < n; i++) {
      rgb_t color = colors[esp_random() % color_count];
      esp_err_t err = led_strip_spi_set_pixel_brightness(leds, i, color, brightness);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set pixel color: %s", esp_err_to_name(err));
      }
    }
  }
}

static void render_led_pattern(led_strip_spi_t* leds, uint8_t n, uint8_t pattern, animation_clock_t* clock)
{
  static rgb_t const kColorRed = rgb_t{
    .r = 255,
    .g = 0,
    .b = 0
  };

  static rgb_t const kInstigatorColors[] = {
    {255, 60, 150},
    {0, 170, 220}
  };

  static uint8_t default_brightness = 40;

  switch (pattern) {
    case 1:
      render_solid_color_pattern(leds, n, kColorRed, default_brightness);
      break;
    case 2:
      render_chase_pattern(leds, n, kColorRed, default_brightness, clock);
      break;
    case 3:
      render_pulse_pattern(leds, n, kColorRed, default_brightness, clock);
      break;
    case 4:
      render_strobe_pattern(leds, n, kColorRed, default_brightness, clock);
      break;
    case 5:
      render_twinkle_pattern(leds, n, kColorRed, default_brightness, clock);
      break;
    case 6:
      render_random_pattern(leds, n, kInstigatorColors, 2, 30, clock);
      break;
    case 7:
      render_solid_color_pattern(leds, n, kColorRed, default_brightness);
      break;
    default:
      render_solid_color_pattern(leds, n, kColorRed, default_brightness);
      break;
  }

  esp_err_t err = led_strip_spi_flush(leds);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to flush LED strip: %s", esp_err_to_name(err));
  }
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
  gpio_num_t const spi_pins[] = { kGpioLedStripData, kGpioLedStripClock };
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
  uint8_t pattern = 1;

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

  animation_clock_t clock = {
    .last_update_us = 0,
    .now_us = 0,
  };

  while (true) {
    bool should_stop = false;

    portENTER_CRITICAL(&state_lock);
    if (current_led_pattern != pattern)
      clock.last_update_us = 0;
    pattern = current_led_pattern;
    should_stop = stop_render_requested;
    portEXIT_CRITICAL(&state_lock);

    if (should_stop) {
      break;
    }

    clock.now_us = esp_timer_get_time();
    render_led_pattern(&led_strip, kLedStripLength, pattern, &clock);

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
      current_led_pattern = (current_led_pattern % kLedModeCount) + 1;
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
      ESP_LOGI(TAG, "TODO: enter setup mode");
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
