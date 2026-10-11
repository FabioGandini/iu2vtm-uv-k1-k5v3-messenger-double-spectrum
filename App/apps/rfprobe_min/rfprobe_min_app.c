/* RF Probe MIN+CALIBRATE -- same working base, plus RF Probe's exact
 * calibrate() (ticks_ms() loop + raw SysTick reads), called ONCE at
 * startup, to isolate whether that specific function is what breaks
 * RF Probe (rssi_dbm() alone already tested fine). */
#include <stdint.h>
#include <stdbool.h>
#include "../app_api.h"

#define SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014u)
#define SYSTICK_VAL  (*(volatile uint32_t *)0xE000E018u)

static const app_api_t *A;
static uint32_t ticksPerMs;
static int16_t  noiseFloor;

/* Verbatim copy of RF Probe's calibrate() */
static void calibrate(void)
{
    ticksPerMs = SYSTICK_LOAD / 10u;
    if (ticksPerMs == 0) ticksPerMs = 1;

    int32_t sum = 0;
    uint32_t until = A->ticks_ms() + 100u;
    uint16_t n = 0;
    while (A->ticks_ms() < until && n < 200) {
        sum += A->rssi_dbm();
        n++;
    }
    noiseFloor = (int16_t)(sum / (n ? n : 1));
}

__attribute__((section(".text.entry"),used))
void app_main(const app_api_t *api)
{
    A = api;
    A->backlight_on();

    A->display_clear();
    A->print_normal("RFP MIN+CAL", 0, 127, 0);
    A->blit_full();

    calibrate();   /* <-- the one new thing, called once before the loop */

    {
        char buf[22];
        uint8_t i = 0;
        buf[i++] = 'n'; buf[i++] = 'f'; buf[i++] = ':';
        int32_t v = noiseFloor;
        if (v < 0) { buf[i++] = '-'; v = -v; }
        char tmp[6]; uint8_t t = 0;
        uint32_t uv = (uint32_t)v;
        do { tmp[t++] = (char)('0' + uv % 10); uv /= 10; } while (uv);
        while (t) buf[i++] = tmp[--t];
        buf[i++] = ' '; buf[i++] = 't'; buf[i++] = 'p'; buf[i++] = 'm'; buf[i++] = ':';
        uv = ticksPerMs; t = 0;
        do { tmp[t++] = (char)('0' + uv % 10); uv /= 10; } while (uv && t < 6);
        while (t) buf[i++] = tmp[--t];
        buf[i] = 0;
        A->print_normal(buf, 0, 127, 1);
        A->blit_full();
    }

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
        buf[i] = 0;

        A->print_normal(buf, 0, 127, 2);
        A->blit_full();

        A->delay_ms(20);
    }
}
