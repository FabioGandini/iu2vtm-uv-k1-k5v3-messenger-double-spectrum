/* RF Probe MIN+CAPTURE -- working base + calibrate() (already confirmed
 * fine) + RF Probe's exact runCapture(), triggered by PTT, to isolate it
 * as the last untested piece. */
#include <stdint.h>
#include <stdbool.h>
#include "../app_api.h"

#define SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014u)
#define SYSTICK_VAL  (*(volatile uint32_t *)0xE000E018u)

#define MAX_EDGES 256u
#define CAPTURE_MS 2500u

static const app_api_t *A;
static uint32_t ticksPerMs;
static int16_t  noiseFloor;
static uint16_t edgeUs[MAX_EDGES];
static uint16_t edgeCount;
static uint32_t sampleCount;
static bool     haveResult;

static uint32_t tickDelta(uint32_t prev, uint32_t cur, uint32_t load)
{
    return (cur <= prev) ? (prev - cur) : (prev + (load - cur) + 1u);
}

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

/* Verbatim copy of RF Probe's runCapture() */
static void runCapture(void)
{
    uint32_t load = SYSTICK_LOAD;
    uint32_t until = A->ticks_ms() + CAPTURE_MS;
    uint32_t prevT = SYSTICK_VAL;
    uint32_t edgeT = prevT;
    int16_t  thrHi = (int16_t)(noiseFloor + 6);
    int16_t  thrLo = (int16_t)(noiseFloor + 3);
    bool     state = false;

    edgeCount = 0;
    sampleCount = 0;

    while (A->ticks_ms() < until && edgeCount < MAX_EDGES) {
        int16_t rssi = A->rssi_dbm();
        uint32_t now = SYSTICK_VAL;
        prevT = now;
        sampleCount++;

        bool newState = state ? (rssi > thrLo) : (rssi > thrHi);
        if (newState != state) {
            uint32_t width = tickDelta(edgeT, now, load);
            uint32_t us = (width * 1000u) / ticksPerMs;
            edgeUs[edgeCount++] = (uint16_t)(us > 65535u ? 65535u : us);
            edgeT = now;
            state = newState;
        }
    }
    (void)prevT;
    haveResult = true;
}

static char *putu(char *o, uint32_t v)
{
    char t[10]; int8_t n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n--) *o++ = t[n];
    return o;
}
static char *put(char *o, const char *s) { while (*s) *o++ = *s++; return o; }

static void showResult(void)
{
    char str[22];
    A->display_clear();   /* <-- called a second time, like show() does
                            * after a capture */
    A->print_normal("RFP MIN+CAP", 0, 127, 0);
    A->print_normal("PTT = capture 2.5s", 0, 127, 1);
    char *o = str;
    o = put(o, "edges:"); o = putu(o, edgeCount);
    o = put(o, " rate:");
    o = putu(o, sampleCount * 1000u / CAPTURE_MS);
    *o = 0;
    A->print_normal(str, 0, 127, 3);
    A->blit_full();
}

__attribute__((section(".text.entry"),used))
void app_main(const app_api_t *api)
{
    A = api;
    A->backlight_on();

    A->display_clear();   /* <-- the one never-tested call */
    A->print_normal("RFP MIN+CAP", 0, 127, 0);
    A->print_normal("PTT = capture 2.5s", 0, 127, 1);
    A->blit_full();

    calibrate();

    bool pttHeld = false;
    uint16_t n = 0;
    for (;;) {
        uint8_t key = A->get_key();
        if (key == APP_KEY_EXIT) break;

        bool pttNow = (key == APP_KEY_PTT);
        if (pttNow && !pttHeld) {
            A->print_normal("Capturing...        ", 0, 127, 3);
            A->blit_full();
            runCapture();        /* <-- the one new thing, on PTT */
            showResult();
        }
        pttHeld = pttNow;

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
        A->backlight_update();   /* <-- the one never-tested call */
    }
}
