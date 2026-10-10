/* RF Probe -- minimal test build (b5).
 * Stripped to the bare minimum to isolate a freeze seen in earlier builds:
 * no SysTick MMIO reads, no rssi_dbm(), no calibrate(), nothing but the
 * same pattern every other working app uses. */

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
    A->print_normal("RF PROBE b5 ALIVE", 0, 127, 0);
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
