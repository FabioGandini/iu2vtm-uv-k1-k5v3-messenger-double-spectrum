/* RF Probe -- overlay app (experimental).
 *
 * Diagnostic/experiment for capturing fixed-code 433.92 MHz OOK keyfobs
 * (FAAC RC and similar 12-bit protocols: ~300-700 us pulses) on hardware
 * that was never designed for this: the BK4819 has no OOK envelope/edge
 * capture path, so this polls rssi_dbm() as fast as possible and
 * timestamps each sample.
 *
 * Coarse timing (the capture window, noise-floor averaging) uses the
 * resident api->ticks_ms() (API level 2, 10 ms resolution). That is too
 * coarse to resolve individual OOK pulses, so fine edge-to-edge timing
 * still reads the Cortex-M0+ SysTick down-counter directly (no MPU on this
 * chip, so an overlay app can); ticks-per-ms for that conversion comes
 * straight from SysTick's own reload value (it free-runs at 10 ms per
 * period by firmware design), so no calibration loop is needed any more.
 *
 * Tune the radio's main VFO to the keyfob's frequency (433.9200 MHz for
 * most Italian fixed-code gate remotes) in AM/narrow mode BEFORE opening
 * this app: it has no way to change frequency itself, and merely reads
 * whatever the resident receiver is already tuned to.
 *
 * Keys: PTT = arm a ~2.5 s capture window (press the real keyfob during
 * it) · EXIT = quit.
 *
 * Row 2 also shows a live raw key code and a free-running frame counter --
 * diagnostics kept from the build that tracked down an earlier freeze (a
 * write past the 7-row framebuffer). Still useful: it is the fastest way
 * to see, on the very first test of this firmware sync, whether the
 * overlay-app key-read path is healthy.
 *
 * Step one: capture + raw diagnostics, NOT a decoder or transmitter.
 * Whether the numbers it reports are good enough to go further can only be
 * judged from a real capture on real hardware.
 */

#include <stdint.h>
#include <stdbool.h>
#include "../app_api.h"

/* Cortex-M0+ SysTick, architecturally fixed address on every chip of this
 * family. */
#define SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014u)
#define SYSTICK_VAL  (*(volatile uint32_t *)0xE000E018u)

#define MAX_EDGES     256u
#define CAPTURE_MS   2500u   /* capture window length                   */
#define SHOW_EDGES      7u   /* how many of the last widths to display  */

static const app_api_t *A;

static uint16_t edgeUs[MAX_EDGES];
static uint16_t edgeCount;
static uint32_t sampleCount;
static uint32_t ticksPerMs;     /* SysTick ticks per 1 ms, from LOAD/10   */
static int16_t  noiseFloor;     /* calibrated quiescent RSSI, dBm         */
static bool     haveResult;
static char     str[22];

/* Down-counting timer: elapsed ticks since 'prev', valid for any single
 * gap shorter than one full reload period (10 ms) -- the capture loop
 * samples far faster than that. */
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
    /* Timebase: SysTick free-runs at a fixed 10 ms reload period (firmware
     * design, see App/driver/systick.c), so ticks-per-ms falls straight
     * out of LOAD -- no delay-loop measurement needed any more. */
    ticksPerMs = SYSTICK_LOAD / 10u;
    if (ticksPerMs == 0) ticksPerMs = 1;

    /* Noise floor: average RSSI with no keyfob pressed. */
    int32_t sum = 0;
    uint32_t until = A->ticks_ms() + 100u;
    uint16_t n = 0;
    while (A->ticks_ms() < until && n < 200) {
        sum += A->rssi_dbm();
        n++;
    }
    noiseFloor = (int16_t)(sum / (n ? n : 1));
}

static void runCapture(void)
{
    uint32_t load = SYSTICK_LOAD;
    uint32_t until = A->ticks_ms() + CAPTURE_MS;
    uint32_t prevT = SYSTICK_VAL;
    uint32_t edgeT = prevT;
    /* Hysteresis around the calibrated floor so we don't double-trigger
     * on noise sitting right at the threshold. */
    int16_t  thrHi = (int16_t)(noiseFloor + 6);
    int16_t  thrLo = (int16_t)(noiseFloor + 3);
    bool     state = false; /* false = quiet, true = carrier present   */

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

static void show(void)
{
    char *o;
    A->display_clear();

    line(0, "RF PROBE 433 v2.7");

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

static uint16_t frameCounter;

static void showKey(uint8_t key)
{
    /* Diagnostic: live raw key code + a free-running frame counter, so a
     * stuck app shows two different things independently: whether the loop
     * is iterating AT ALL (frameCounter moves even with no key pressed) and
     * whether get_key() specifically is live (key changes when a button is
     * pressed). Row 2 is the only one show() leaves free -- FRAME_LINES is
     * 7, valid rows are 0..6. */
    char *o = str;
    o = put(o, "k:");
    o = putu(o, key);
    o = put(o, " f:");
    o = putu(o, ++frameCounter);
    o = put(o, "   ");
    *o = 0;
    A->print_normal(str, 0, 127, 2);
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
