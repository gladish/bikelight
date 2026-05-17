/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "led_patterns.h"
#include "sdkconfig.h"
#include "sk9822_leds.hpp"

// constants
static constexpr uint8_t kLedModeCount = 6;
static constexpr uint8_t kLedStripLength = 8;
static constexpr uint32_t kRenderTimerPeriodMs = 33;
static constexpr uint32_t kButtonDebounceMs = 30;
static constexpr uint32_t kButtonLongPressMs = 1500;
static constexpr gpio_num_t kGpioButton = GPIO_NUM_2;
static constexpr gpio_num_t kGpioLedStringClock = GPIO_NUM_8;
static constexpr gpio_num_t kGpioLedStripData = GPIO_NUM_10;  // mosi
static constexpr uint32_t kButtonTaskStackSize = 3072;
static constexpr uint32_t kRenderTaskStackSize = 3072;

// globals
static const char* TAG = "main";
static TaskHandle_t button_task_handle;
static TaskHandle_t render_task_handle;
static TimerHandle_t render_timer_handle;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t current_led_design = 1;
static SK9822_LedStrip<kLedStripLength> led_strip;

// deep sleep saved
RTC_DATA_ATTR int saved_led_design = 0;


static void LightApplicationState_RenderLeds()
{
  static OffPattern off_pattern;
  static SolidRedPattern solid_red_pattern;
  static ChasePattern chase_pattern;
  static PulsePattern pulse_pattern;
  static StrobePattern strobe_pattern;
  static TwinklePattern twinkle_pattern;

  static const SK9822_Led kFireColors[] = {
      {20, 150, 60, 255},
      {20, 220, 170, 0},
  };
  static RandomPattern random_pattern(kFireColors, 2, 100);

  uint8_t design = -1;

  portENTER_CRITICAL(&state_lock);
  design = current_led_design;
  portEXIT_CRITICAL(&state_lock);

  uint32_t const now = (uint32_t)(esp_timer_get_time() / 1000ULL);

  switch (design) {
    case 1:
      led_strip.ApplyDesign(solid_red_pattern, now);
      break;
    case 2:
      led_strip.ApplyDesign(chase_pattern, now);
      break;
    case 3:
      led_strip.ApplyDesign(pulse_pattern, now);
      break;
    case 4:
      led_strip.ApplyDesign(strobe_pattern, now);
      break;
    case 5:
      led_strip.ApplyDesign(twinkle_pattern, now);
      break;
    case 6:
      led_strip.ApplyDesign(random_pattern, now);
      break;
    default:
      led_strip.ApplyDesign(solid_red_pattern, now);
      break;
  }

  led_strip.Render();
}

static void enter_deep_sleep(void)
{
  ESP_LOGI(TAG, "Entering deep sleep; press button to wake");

  if (render_timer_handle != NULL) {
    xTimerStop(render_timer_handle, portMAX_DELAY);
  }

  led_strip.Clear();
  led_strip.Render();

  while (gpio_get_level(kGpioButton) == 0) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  vTaskDelay(pdMS_TO_TICKS(kButtonDebounceMs));
  saved_led_design = current_led_design;

  esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(BIT(kGpioButton), ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

static void render_timer_callback(TimerHandle_t __unused(timer))
{
  // notify render task to update LEDs
  if (render_task_handle != NULL) {
    xTaskNotifyGive(render_task_handle);
  }
}

static void render_task(void* __unused(argp))
{
  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    LightApplicationState_RenderLeds();
  }
}

static void button_task(void* __unused(arg))
{
  int last_level = gpio_get_level(kGpioButton);
  TickType_t pressed_at = 0;

  bool long_press_handled = false;

  // ESP_LOGI(TAG, "Monitoring button on GPIO %d", kGpioButton);
  // ESP_LOGI(TAG, "Button %s", last_level == 0 ? "pressed" : "released");

  while (1) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(kButtonDebounceMs));

    const int level = gpio_get_level(kGpioButton);
    if (level == last_level) {
      continue;
    }

    last_level = level;
    ESP_LOGI(TAG, "Button %s", level == 0 ? "pressed" : "released");

    if (level == 0) {
      pressed_at = xTaskGetTickCount();
      long_press_handled = false;

      while (gpio_get_level(kGpioButton) == 0) {
        uint32_t const held_ms = (xTaskGetTickCount() - pressed_at) * portTICK_PERIOD_MS;

        if (!long_press_handled && held_ms >= kButtonLongPressMs) {
          long_press_handled = true;
          ESP_LOGI(TAG, "Long press detected (%" PRIu32 " ms)", held_ms);
          enter_deep_sleep();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
      }

      continue;
    }

    if (long_press_handled) {
      continue;
    }

    portENTER_CRITICAL(&state_lock);
    current_led_design = (current_led_design % kLedModeCount) + 1U;
    portEXIT_CRITICAL(&state_lock);

    ESP_LOGI(TAG, "Active LED mode: %u", current_led_design);
  }
}

static void IRAM_ATTR button_isr_handler(void* __unused(arg))
{
  BaseType_t task_woken = pdFALSE;

  vTaskNotifyGiveFromISR(button_task_handle, &task_woken);
  if (task_woken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

extern "C" void app_main(void)
{
  const gpio_config_t button_config = {
      .pin_bit_mask = 1ULL << kGpioButton,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_ANYEDGE,
  };

  if (saved_led_design != 0) {
    current_led_design = saved_led_design;
  }

  ESP_ERROR_CHECK(led_strip.InitSPI(kGpioLedStringClock, kGpioLedStripData));
  ESP_ERROR_CHECK(gpio_config(&button_config));
  ESP_ERROR_CHECK(gpio_install_isr_service(0));

  render_timer_handle =
      xTimerCreate("render_timer", pdMS_TO_TICKS(kRenderTimerPeriodMs), pdTRUE, NULL, render_timer_callback);
  assert(render_timer_handle != NULL);

  BaseType_t task_created =
      xTaskCreate(button_task, "button_task", kButtonTaskStackSize, NULL, 10, &button_task_handle);
  assert(task_created == pdPASS);

  task_created = xTaskCreate(render_task, "render_task", kRenderTaskStackSize, NULL, 9, &render_task_handle);
  assert(task_created == pdPASS);

  ESP_ERROR_CHECK(gpio_isr_handler_add(kGpioButton, button_isr_handler, NULL));
  BaseType_t timer_started = xTimerStart(render_timer_handle, portMAX_DELAY);
  assert(timer_started == pdPASS);
}
