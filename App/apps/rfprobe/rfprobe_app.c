/* RF Probe — overlay app (experimental).
 *
 * Diagnostic/experiment for capturing fixed-code 433.92 MHz OOK keyfobs
 * (FAAC RC and similar 12-bit protocols: ~300-700 us pulses) on hardware
 * that was never designed for this: the BK4819 has no OOK envelope/edge
 * capture path, so this polls rssi_dbm() as fast as possible and time-
 * stamps each sample with the Cortex-M0+ SysTick down-counter, read
 * directly (no firmware symbols: NOCROSSREFS forbids linking against
 * them, and nothing in app_api exposes microsecond timing).
 *
 * Tune the radio's main VFO to the keyfob's frequency (433.9200 MHz for
 * most Italian fixed-code gate remotes) in AM/narrow mode BEFORE opening
 * this app: it has no way to change frequency itself, and merely reads
 * whatever the resident receiver is already tuned to.
 *
 * Keys: PTT = arm a ~2.5 s capture window (press the real keyfob during
 * it) · EXIT = quit.
 *
 * This is step one: capture + raw diagnostics, NOT a decoder or
 * transmitter. Whether the numbers it reports are good enough to go
 * further can only be judged from a real capture on real hardware.
 */

#include <stdint.h>
#include <stdbool.h>
#include "../app_api.h"

/* Cortex-M0+ SysTick, architecturally fixed address on every chip of this
 * family. No MPU on this MCU, so an overlay app can read it directly. */
#define SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014u)
#define SYSTICK_VAL  (*(volatile uint32_t *)0xE000E018u)

#define MAX_EDGES     256u
#define CAL_STEPS     200u   /* 200 x 1 ms = 200 ms calibration window   */
#define CAPTURE_MS   2500u   /* capture window length                   */
#define SHOW_EDGES      7u   /* how many of the last widths to display  */

static const app_api_t *A;

static uint16_t edgeUs[MAX_EDGES];
static uint16_t edgeCount;
static uint32_t sampleCount;
static uint32_t ticksPerMs;     /* calibrated: SysTick ticks per 1 ms     */
static int16_t  noiseFloor;     /* calibrated quiescent RSSI, dBm         */
static bool     haveResult;
static char     str[22];

/* Down-counting timer: elapsed ticks since 'prev', valid for any single
 * gap shorter than one full reload period (the capture loop samples far
 * faster than that, so each step is safe; the calibration loop below
 * only ever waits across one delay_ms(1) step at a time for the same
 * reason). */
static uint32_t tickDelta(uint32_t prev, uint32_t cur, uint32_t load)
{
    return (cur <= prev) ? (prev - cur) : (prev + (load - cur) + 1u);
}

static char *putu(char *o, uint32_t v)
{
    char t[10]; int8_t n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n--) *o++ = t[n];
    return o;
}
static char *puti(char *o, int32_t v)
{
    if (v < 0) { *o++ = '-'; v = -v; }
    return putu(o, (uint32_t)v);
}
static char *put(char *o, const char *s) { while (*s) *o++ = *s++; return o; }

static void line(uint8_t row, const char *s)
{
    A->print_normal(s, 0, 127, row);
}

static void calibrate(void)
{
    /* Timebase: sum SysTick deltas across CAL_STEPS known 1 ms waits. */
    uint32_t load = SYSTICK_LOAD;
    uint32_t total = 0;
    for (uint16_t i = 0; i < CAL_STEPS; i++) {
        uint32_t t0 = SYSTICK_VAL;
        A->delay_ms(1);
        uint32_t t1 = SYSTICK_VAL;
        total += tickDelta(t0, t1, load);
    }
    ticksPerMs = total / CAL_STEPS;
    if (ticksPerMs == 0) ticksPerMs = 1; /* never divide by zero below */

    /* Noise floor: average RSSI over the same style of window, no keyfob
     * pressed. Quick and crude (no real averaging needed at dBm scale). */
    int32_t sum = 0;
    for (uint16_t i = 0; i < 64; i++) sum += A->rssi_dbm();
    noiseFloor = (int16_t)(sum / 64);
}

static void runCapture(void)
{
    uint32_t load = SYSTICK_LOAD;
    uint32_t budgetTicks = ticksPerMs * CAPTURE_MS;
    uint32_t elapsed = 0;
    uint32_t prevT = SYSTICK_VAL;
    uint32_t edgeT = prevT;
    /* Hysteresis around the calibrated floor so we don't double-trigger
     * on noise sitting right at the threshold. */
    int16_t  thrHi = (int16_t)(noiseFloor + 6);
    int16_t  thrLo = (int16_t)(noiseFloor + 3);
    bool     state = false; /* false = quiet, true = carrier present   */

    edgeCount = 0;
    sampleCount = 0;

    while (elapsed < budgetTicks && edgeCount < MAX_EDGES) {
        int16_t rssi = A->rssi_dbm();
        uint32_t now = SYSTICK_VAL;
        uint32_t d = tickDelta(prevT, now, load);
        elapsed += d;
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
    haveResult = true;
}

static void show(void)
{
    char *o;
    A->display_clear();

    line(0, "RF PROBE 433 (exp.)");

    o = str;
    o = put(o, "VFO ");
    uint32_t f = A->rx_freq(); /* x10 Hz */
    o = putu(o, f / 100000u);
    *o++ = '.';
    uint32_t frac = (f / 10u) % 10000u;
    for (int8_t d = 3; d >= 0; d--) {
        uint32_t p = 1; for (int8_t k = 0; k < d; k++) p *= 10;
        *o++ = (char)('0' + (frac / p) % 10);
    }
    o = put(o, "M");
    *o = 0;
    line(1, str);

    if (!haveResult) {
        line(3, "PTT = arm 2.5s capture");
        line(4, "Tune AM to keyfob freq");
        line(5, "then press keyfob while");
        line(6, "PTT is held down.");
    } else {
        o = str;
        o = put(o, "edges:"); o = putu(o, edgeCount);
        o = put(o, " nf:");   o = puti(o, noiseFloor);
        *o = 0;
        line(3, str);

        uint32_t rate = sampleCount * 1000u / CAPTURE_MS;
        o = str;
        o = put(o, "rate:"); o = putu(o, rate); o = put(o, "/s");
        *o = 0;
        line(4, str);

        uint8_t first = (edgeCount > SHOW_EDGES) ? (uint8_t)(edgeCount - SHOW_EDGES) : 0;
        o = str;
        for (uint8_t i = first; i < edgeCount && i < first + SHOW_EDGES; i++) {
            o = putu(o, edgeUs[i]);
            *o++ = ' ';
        }
        *o = 0;
        line(5, str);
        line(6, "PTT = capture again");
    }

    A->blit_full();
}

static void showKey(uint8_t key)
{
    /* Diagnostic: live raw key code, so a stuck app shows whether get_key()
     * is still being read at all (this updates) or appears frozen (it
     * doesn't) -- two very different problems. */
    char *o = str;
    o = put(o, "key:");
    o = putu(o, key);
    o = put(o, "   ");
    *o = 0;
    A->print_normal(str, 0, 127, 2); /* row 2: the only one show() leaves free
                                       * (FRAME_LINES is 7: valid rows 0..6 --
                                       * row 7 doesn't exist, writes straight
                                       * past the framebuffer into whatever
                                       * RAM follows it) */
    A->blit_full();
}

__attribute__((section(".text.entry"),used))
void app_main(const app_api_t *api)
{
    A = api;
    haveResult = false;
    edgeCount = 0;

    A->backlight_on();

    calibrate();
    show();

    bool pttHeld = false;
    for (;;) {
        uint8_t key = A->get_key();
        showKey(key);
        if (key == APP_KEY_EXIT) break;

        bool pttNow = (key == APP_KEY_PTT);
        if (pttNow && !pttHeld) {
            line(3, "Capturing...");
            A->blit_full();
            runCapture();
            show();
        }
        pttHeld = pttNow;

        A->delay_ms(20);
        A->backlight_update();
    }
}
