#include "sk9822_leds.hpp"

void SK9822_Led_Zero(SK9822_Led *strip, int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        strip[i].Brightness = 0;
        strip[i].Red = 0;
        strip[i].Green = 0;
        strip[i].Blue = 0;
    }
}
