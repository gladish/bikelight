#pragma once

#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"

// ---------------------------------------------------------------------------
// SK9822 single LED pixel
// ---------------------------------------------------------------------------

struct SK9822_Led {
  uint8_t Brightness;  // 0-31 (5 bits; top 3 bits of the wire frame are always 111)
  uint8_t Blue;
  uint8_t Green;
  uint8_t Red;
};

// Byte length of the SPI tx buffer for a strip of N LEDs.
#define SK9822_TX_BUFFER_SIZE(n) (4 + ((n) * 4) + 4)

// ---------------------------------------------------------------------------
// Driver init
// ---------------------------------------------------------------------------

// Initialise the SPI bus and attach the SK9822 device.
// Must be called once before any Render call.
esp_err_t SK9822_Leds_Init(gpio_num_t clk_pin, gpio_num_t mosi_pin);

// ---------------------------------------------------------------------------
// Low-level free functions  (implemented in sk9822_leds.cpp)
// These operate on raw arrays and are the building blocks for SK9822_LedStrip.
// ---------------------------------------------------------------------------

void SK9822_Led_Zero(SK9822_Led* strip, int led_count);
void SK9822_Led_Render(const SK9822_Led* leds, uint8_t* tx_buffer, int count);

// ---------------------------------------------------------------------------
// SK9822_LedStrip<N>
//
// LED count is a compile-time template parameter. The LED array and SPI tx
// buffer are embedded members — no heap allocation, no external pointers.
// ---------------------------------------------------------------------------

class LedDesign
{
public:
  virtual ~LedDesign() = default;

  virtual void Apply(SK9822_Led* leds, int count, uint32_t now) = 0;

  // brightness is 0-31 (percent).
  void SetBrightness(uint8_t brightness)
  {
    brightness_ = brightness > 31 ? 31 : brightness;
  }

  uint8_t Brightness() const
  {
    return brightness_;
  }

protected:
  uint8_t brightness_ = 10;
};

template <int N>
class SK9822_LedStrip
{
public:
  SK9822_LedStrip() = default;

  static constexpr int kLedCount = N;

  esp_err_t InitSPI(gpio_num_t clk_pin, gpio_num_t mosi_pin)
  {
    spi_transaction_ = {};
    spi_transaction_.length = 8 * (size_t)SK9822_TX_BUFFER_SIZE(kLedCount);
    spi_transaction_.tx_buffer = tx_buffer_;

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = mosi_pin;
    buscfg.sclk_io_num = clk_pin;
    buscfg.max_transfer_sz = 4096;

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 1 * 1000 * 1000;  // 1 MHz
    devcfg.mode = 0;
    devcfg.spics_io_num = -1;  // SK9822 uses start/end frames instead of CS
    devcfg.queue_size = 1;

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
      ESP_LOGE("SK9822_LedStrip", "spi_bus_initialize failed: %s", esp_err_to_name(err));
      return err;
    }

    err = spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle_);
    if (err != ESP_OK) {
      ESP_LOGE("SK9822_LedStrip", "spi_bus_add_device failed: %s", esp_err_to_name(err));
      return err;
    }

    return ESP_OK;
  }

  void Clear()
  {
    SK9822_Led_Zero(leds_, kLedCount);
  }

  void ApplyDesign(LedDesign& design, uint32_t now)
  {
    design.Apply(leds_, kLedCount, now);
  }

  void Render()
  {
    // SK9822 protocol: 4-byte start frame, one 4-byte frame per LED, 4-byte end frame
    int idx = 0;

    tx_buffer_[idx++] = 0x00;
    tx_buffer_[idx++] = 0x00;
    tx_buffer_[idx++] = 0x00;
    tx_buffer_[idx++] = 0x00;

    for (int i = 0; i < kLedCount; i++) {
      tx_buffer_[idx++] = 0xE0U | (leds_[i].Brightness & 0x1FU);
      tx_buffer_[idx++] = leds_[i].Blue;
      tx_buffer_[idx++] = leds_[i].Green;
      tx_buffer_[idx++] = leds_[i].Red;
    }

    tx_buffer_[idx++] = 0xFF;
    tx_buffer_[idx++] = 0xFF;
    tx_buffer_[idx++] = 0xFF;
    tx_buffer_[idx++] = 0xFF;

    spi_transaction_ = {};
    spi_transaction_.length = 8 * (size_t)SK9822_TX_BUFFER_SIZE(kLedCount);
    spi_transaction_.tx_buffer = tx_buffer_;

    spi_device_transmit(spi_handle_, &spi_transaction_);
  }

private:
  SK9822_Led leds_[kLedCount];
  uint8_t tx_buffer_[SK9822_TX_BUFFER_SIZE(kLedCount)];

  spi_device_handle_t spi_handle_;
  spi_transaction_t spi_transaction_;
};
