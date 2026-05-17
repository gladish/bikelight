/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "driver/gpio.h"
#include "sk9822_leds.hpp"
#include "led_patterns.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "sdkconfig.h"
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#define BUTTON_GPIO GPIO_NUM_2
#define BUTTON_DEBOUNCE_MS 30
#define BUTTON_LONG_PRESS_MS 1500

#define LED_SPI_CLK_PIN GPIO_NUM_8
#define LED_SPI_MOSI_PIN GPIO_NUM_10
#define LED_RENDER_PERIOD_MS 33
#define LED_MODE_COUNT 6
#define BUTTON_TASK_STACK_SIZE 3072
#define RENDER_TASK_STACK_SIZE 3072

static const char *TAG = "main";
static TaskHandle_t button_task_handle;
static TaskHandle_t render_task_handle;
static TimerHandle_t render_timer_handle;
static portMUX_TYPE state_lock = portMUX_INITIALIZER_UNLOCKED;


struct LightApplicationState
{
    bool PowerOn;
    uint8_t LedStyle;
};

static SK9822_LedStrip<8> led_strip;

static LightApplicationState CreateLightApplicationState(void)
{
    LightApplicationState state = {};
    state.PowerOn = true;
    state.LedStyle = 1;
    return state;
}

static LightApplicationState light_app_state = CreateLightApplicationState();

static LightApplicationState LightApplicationState_GetCurrentStyle()
{
    LightApplicationState state;

    portENTER_CRITICAL(&state_lock);
    state = light_app_state;
    portEXIT_CRITICAL(&state_lock);

    return state;
}

static LightApplicationState LightApplicationState_AdvanceLedStyle()
{
    LightApplicationState state;

    portENTER_CRITICAL(&state_lock);
    light_app_state.LedStyle = (light_app_state.LedStyle % LED_MODE_COUNT) + 1U;
    state = light_app_state;
    portEXIT_CRITICAL(&state_lock);

    return state;
}

static void LightApplicationState_SetPower(bool power_on)
{
    portENTER_CRITICAL(&state_lock);
    light_app_state.PowerOn = power_on;
    portEXIT_CRITICAL(&state_lock);
}


static void LightApplicationState_RenderLeds(LightApplicationState& state)
{
    static OffPattern off_pattern;
    static SolidRedPattern solid_red_pattern;
    static ChasePattern chase_pattern;
    static PulsePattern pulse_pattern;
    static StrobePattern strobe_pattern;
    static TwinklePattern twinkle_pattern;
    static const SK9822_Led kFireColors[] = {
        {20, 150, 60, 255},
        {20, 220, 170,  0},
    };
    static RandomPattern random_pattern(kFireColors, 2, 100);

    uint32_t const now = (uint32_t)(esp_timer_get_time() / 1000ULL);

    if (!state.PowerOn)
    {
        led_strip.Apply(off_pattern, now);
        led_strip.Render();
        return;
    }

    LightApplicationState current_state = LightApplicationState_GetCurrentStyle();

    switch (current_state.LedStyle)
    {
        case 1: led_strip.Apply(solid_red_pattern, now); break;
        case 2: led_strip.Apply(chase_pattern, now); break;
        case 3: led_strip.Apply(pulse_pattern, now); break;
        case 4: led_strip.Apply(strobe_pattern, now); break;
        case 5: led_strip.Apply(twinkle_pattern, now); break;
        case 6: led_strip.Apply(random_pattern, now); break;
        default: led_strip.Apply(solid_red_pattern, now); break;
    }

    led_strip.Render();
}

static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "Entering deep sleep; press button to wake");
    LightApplicationState_SetPower(false);

    if (render_timer_handle != NULL)
    {
        xTimerStop(render_timer_handle, portMAX_DELAY);
    }

    ESP_ERROR_CHECK(gpio_wakeup_enable(BUTTON_GPIO, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());

    esp_deep_sleep_start();
}

static void render_timer_callback(TimerHandle_t __unused(timer))
{
    // notify render task to update LEDs
    if (render_task_handle != NULL)
    {
        xTaskNotifyGive(render_task_handle);
    }
}

static void render_task(void *__unused(argp))
{
    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        LightApplicationState_RenderLeds( light_app_state );
    }
}

static void button_task(void *__unused(arg))
{
    int last_level = gpio_get_level(BUTTON_GPIO);
    TickType_t pressed_at = 0;

    // ESP_LOGI(TAG, "Monitoring button on GPIO %d", BUTTON_GPIO);
    // ESP_LOGI(TAG, "Button %s", last_level == 0 ? "pressed" : "released");

    while (1)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));

        const int level = gpio_get_level(BUTTON_GPIO);
        if (level == last_level)
        {
            continue;
        }

        last_level = level;
        ESP_LOGI(TAG, "Button %s", level == 0 ? "pressed" : "released");

        if (level == 0)
        {
            pressed_at = xTaskGetTickCount();
            continue;
        }

        const TickType_t held_ticks = xTaskGetTickCount() - pressed_at;
        const uint32_t held_ms = held_ticks * portTICK_PERIOD_MS;

        if (held_ms >= BUTTON_LONG_PRESS_MS)
        {
            enter_deep_sleep();
            continue;
        }

        const LightApplicationState new_state = LightApplicationState_AdvanceLedStyle();
        ESP_LOGI(TAG, "Active LED mode: %u", new_state.LedStyle);
    }
}

static void IRAM_ATTR button_isr_handler(void *__unused(arg))
{
    BaseType_t task_woken = pdFALSE;

    vTaskNotifyGiveFromISR(button_task_handle, &task_woken);
    if (task_woken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

extern "C" void app_main(void)
{
    const gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    ESP_ERROR_CHECK(led_strip.InitSPI(LED_SPI_CLK_PIN, LED_SPI_MOSI_PIN));

    ESP_ERROR_CHECK(gpio_config(&button_config));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    render_timer_handle =
        xTimerCreate("render_timer", pdMS_TO_TICKS(LED_RENDER_PERIOD_MS), pdTRUE, NULL, render_timer_callback);
    assert(render_timer_handle != NULL);

    BaseType_t task_created =
        xTaskCreate(button_task, "button_task", BUTTON_TASK_STACK_SIZE, NULL, 10, &button_task_handle);
    assert(task_created == pdPASS);

    task_created = xTaskCreate(render_task, "render_task", RENDER_TASK_STACK_SIZE, NULL, 9, &render_task_handle);
    assert(task_created == pdPASS);

    ESP_ERROR_CHECK(gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL));
    BaseType_t timer_started = xTimerStart(render_timer_handle, portMAX_DELAY);
    assert(timer_started == pdPASS);
}
