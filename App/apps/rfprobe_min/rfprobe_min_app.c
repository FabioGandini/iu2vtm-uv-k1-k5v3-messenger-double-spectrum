/* RF Probe MIN+RSSI -- same as the minimal test, plus ONE rssi_dbm() call
 * per frame, to isolate whether touching the BK4819 is what breaks RF
 * Probe (its only other real difference from this working minimal app). */
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
    A->print_normal("RFP MIN+RSSI", 0, 127, 0);
    A->blit_full();

    uint16_t n = 0;
    for (;;) {
        uint8_t key = A->get_key();
        if (key == APP_KEY_EXIT) break;

        int16_t rssi = A->rssi_dbm();   /* <-- the one new thing */

        char buf[20];
        uint8_t i = 0;
        buf[i++] = 'n'; buf[i++] = ':';
        uint16_t v = ++n;
        char tmp[6]; uint8_t t = 0;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (t) buf[i++] = tmp[--t];
        buf[i++] = ' '; buf[i++] = 'r'; buf[i++] = ':';
        int32_t rv = rssi;
        if (rv < 0) { buf[i++] = '-'; rv = -rv; }
        t = 0;
        v = (uint16_t)rv;
        do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (t) buf[i++] = tmp[--t];
        buf[i] = 0;

        A->print_normal(buf, 0, 127, 2);
        A->blit_full();

        A->delay_ms(20);
    }
}
