#include "led_patterns.h"
#include "esp_random.h"

void TwinklePattern::Apply(SK9822_Led *leds, int count, uint32_t now)
{
    if (now - last_update_ < interval_)
        return;

    last_update_ = now;

    for (int i = 0; i < count; i++)
    {
        const bool lit = (esp_random() & 1U) == 0;
        leds[i].Brightness = lit ? Brightness() : 0;
        leds[i].Red        = lit ? 255 : 0;
        leds[i].Green      = 0;
        leds[i].Blue       = 0;
    }
}


void StrobePattern::Apply(SK9822_Led *leds, int count, uint32_t now)
{
    uint32_t elapsed = now - last_update_;
    uint32_t period = on_ ? on_ms_ : off_ms_;

    if (elapsed >= period)
    {
        on_ = !on_;
        last_update_ = now;
    }

    for (int i = 0; i < count; i++)
    {
        leds[i].Brightness = on_ ? Brightness() : 0;
        leds[i].Red = on_ ? 255 : 0;
        leds[i].Green = 0;
        leds[i].Blue = 0;
    }
}


void PulsePattern::Apply(SK9822_Led *leds, int count, uint32_t now)
{
     uint8_t brightness = (uint8_t)(frame_ & 0x1FU);

    if (frame_ & 0x20U)
    {
        brightness = 31U - brightness;
    }

    for (int i = 0; i < count; i++)
    {
        leds[i].Brightness = brightness;
        leds[i].Red = 255;
        leds[i].Green = 0;
        leds[i].Blue = 0;
    }

    frame_++;
}


void OffPattern::Apply(SK9822_Led *leds, int count, uint32_t now)
{
    SK9822_Led_Zero(leds, count);
}


void SolidRedPattern::Apply(SK9822_Led *leds, int count, uint32_t now)
{
    for (int i = 0; i < count; i++)
    {
        leds[i].Brightness = Brightness();
        leds[i].Red = 255;
        leds[i].Green = 0;
        leds[i].Blue = 0;
    }
}


void ChasePattern::Apply(SK9822_Led *leds, int count, uint32_t now)
{
    if (now - last_update_ < interval_)
    {
        return;
    }

    last_update_ = now;

    SK9822_Led_Zero(leds, count);

    leds[index_].Brightness = Brightness();
    leds[index_].Red = 255;
    leds[index_].Green = 0;
    leds[index_].Blue = 0;

    if (forward_)
    {
        if (++index_ > count - 1)
        {
            index_ = count - 2;
            forward_ = false;
        }
    }
    else
    {
        if (--index_ < 0)
        {
            index_ = 1;
            forward_ = true;
        }
    }
}

void RandomPattern::Apply(SK9822_Led *leds, int count, uint32_t now)
{
    if (now - last_update_ < interval_)
        return;

    last_update_ = now;

    for (int i = 0; i < count; i++)
    {
        leds[i] = colors_[esp_random() % (uint32_t)num_colors_];
    }
}
