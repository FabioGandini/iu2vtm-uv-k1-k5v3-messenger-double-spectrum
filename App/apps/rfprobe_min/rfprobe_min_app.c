/* RF Probe MIN -- absolute minimal test, copying breakout's exact idiom.
 * No SysTick reads, no rssi_dbm(), no calibrate(), no ticks_ms(). Only
 * get_key()+EXIT and a visible counter, to isolate whether ANYTHING works
 * on this firmware build at all. */
#include <stdint.h>
#include <stdbool.h>
#include "../app_api.h"

static const app_api_t *A;

__attribute__((section(".text.entry"),used))
void app_main(const app_api_t *api)
{
    A = api;
    A->backlight_on();

    A->display_clear();
    A->print_normal("RFP MIN ALIVE", 0, 127, 0);
    A->blit_full();

    uint16_t n = 0;
    for (;;) {
        uint8_t key = A->get_key();
        if (key == APP_KEY_EXIT) break;

        char buf[16];
        uint8_t i = 0;
        buf[i++] = 'n'; buf[i++] = ':';
        uint16_t v = ++n;
        char tmp[6]; uint8_t t = 0;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (t) buf[i++] = tmp[--t];
        buf[i++] = ' '; buf[i++] = 'k'; buf[i++] = ':';
        v = key;
        t = 0;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (t) buf[i++] = tmp[--t];
        buf[i] = 0;

        A->print_normal(buf, 0, 127, 2);
        A->blit_full();

        A->delay_ms(20);
    }
}
