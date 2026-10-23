/* model3recomp -- browser platform layer (Emscripten).
 *
 * The guest runs on a pthread (link with -sPROXY_TO_PTHREAD): a Model 3
 * main loop never returns, and the rasteriser's band workers and the sound
 * board want threads of their own in any case. Nothing here touches the
 * DOM. The page's script, on the main thread, reads what this layer
 * publishes in shared wasm memory and writes the controls into it:
 *
 *   web_frame()        RGBA8 pixels of the last presented field, w*h*4
 *   web_frame_count()  bumped after every present; the page redraws on change
 *   web_input()        the controls, written by the page (web_input_t)
 *   web_audio_*()      44.1 kHz stereo s16 ring + monotonic write counter,
 *                      read by an AudioWorklet straight out of wasm memory
 *   web_quit()         asks the guest to stop at the next field
 *
 * Settings (ini), the menu bar and netplay prompts are the page's business;
 * this layer keeps nothing between runs.
 */
#include "model3recomp/platform.h"
#include "model3recomp/model3recomp.h"

#include <emscripten.h>
#include <emscripten/threading.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The controls, as the page writes them. Pointer positions are framebuffer
 * pixels (0..w-1, 0..h-1); the gun mapping clamps anything outside. */
typedef struct {
    int32_t  x, y;          /* player 1's pointer */
    uint32_t buttons;       /* WEB_BTN_* for player 1 and the cabinet */
    int32_t  x2, y2;        /* player 2's pointer */
    uint32_t buttons2;      /* WEB_BTN_TRIG / WEB_BTN_RELOAD for player 2 */
} web_input_t;

#define WEB_BTN_TRIG     0x001u  /* left button / Ctrl: pull the trigger */
#define WEB_BTN_RELOAD   0x002u  /* right button / Space / Shift: an off-screen shot */
#define WEB_BTN_COIN1    0x004u
#define WEB_BTN_START1   0x008u
#define WEB_BTN_COIN2    0x010u
#define WEB_BTN_START2   0x020u
#define WEB_BTN_TEST     0x040u
#define WEB_BTN_SERVICE  0x080u
#define WEB_BTN_FAST     0x100u  /* Tab: run flat out */

static volatile web_input_t g_in;

static uint32_t *g_fb;                 /* 0xAARRGGBB, what the renderers draw into */
static int       g_w, g_h;
static uint8_t  *g_present[2];         /* RGBA8 copies, one being shown, one being written */
static volatile int      g_front;      /* which of g_present the page should draw */
static volatile uint32_t g_frames;     /* fields presented */
static volatile int      g_quit;
static char      g_status[160];

#define AUDIO_RING 16384u              /* frames: ~370 ms at 44.1 kHz */
static int16_t           g_ring[AUDIO_RING * 2];
static volatile uint32_t g_ring_write; /* frames ever written */
static volatile int      g_mute;

/* ---- what the page sees --------------------------------------------- */
EMSCRIPTEN_KEEPALIVE web_input_t *web_input(void) { return (web_input_t *)&g_in; }
EMSCRIPTEN_KEEPALIVE uint8_t *web_frame(void) { return g_present[g_front]; }
EMSCRIPTEN_KEEPALIVE int web_frame_w(void) { return g_w; }
EMSCRIPTEN_KEEPALIVE int web_frame_h(void) { return g_h; }
EMSCRIPTEN_KEEPALIVE volatile uint32_t *web_frame_count(void) { return &g_frames; }
EMSCRIPTEN_KEEPALIVE int16_t *web_audio_ring(void) { return g_ring; }
EMSCRIPTEN_KEEPALIVE volatile uint32_t *web_audio_write(void) { return &g_ring_write; }
EMSCRIPTEN_KEEPALIVE uint32_t web_audio_size(void) { return AUDIO_RING; }
EMSCRIPTEN_KEEPALIVE void web_mute(int m) { g_mute = m; }
EMSCRIPTEN_KEEPALIVE void web_quit(void) { g_quit = 1; }
EMSCRIPTEN_KEEPALIVE const char *web_status(void) { return g_status; }

/* ---- the platform API ----------------------------------------------- */
int platform_init(int w, int h, const char *title)
{
    (void)title;
    g_w = w; g_h = h;
    g_fb = calloc((size_t)w * h, sizeof(uint32_t));
    g_present[0] = calloc((size_t)w * h, 4);
    g_present[1] = calloc((size_t)w * h, 4);
    return g_fb && g_present[0] && g_present[1];
}

void platform_shutdown(void)
{
    free(g_fb); g_fb = NULL;
}

uint32_t *platform_framebuffer(int *w, int *h)
{
    if (w) *w = g_w;
    if (h) *h = g_h;
    return g_fb;
}

int platform_poll(void) { return !g_quit; }

void platform_set_status(const char *s)
{
    snprintf(g_status, sizeof g_status, "%s", s ? s : "");
}

uint64_t platform_ticks_us(void)
{
    return (uint64_t)(emscripten_get_now() * 1000.0);
}

/* Reload is a shot off the screen. The reload control does it in one go:
 * the gun reads off screen from the press for as long as it is held, and
 * never less than RELOAD_FIELDS, and the trigger is pulled for the middle
 * of that, so the game has seen the gun leave the screen before the shot
 * and sees the trigger let go before the gun comes back. (As platform_sdl.) */
#define RELOAD_FIELDS 8u
static unsigned reload(int player, int held)
{
    static unsigned left[2], age[2];
    unsigned b = 0;
    if (held && !left[player] && !age[player]) left[player] = RELOAD_FIELDS;
    if (!held && !left[player]) age[player] = 0;
    if (left[player] || held) {
        unsigned n = age[player]++;
        b |= player ? M3_BTN_OFFSCR2 : M3_BTN_OFFSCR1;
        if (n >= 2u && n < RELOAD_FIELDS - 2u) b |= player ? M3_BTN_TRIG2 : M3_BTN_TRIG1;
        if (left[player]) left[player]--;
    }
    return b;
}

void platform_sample(m3_input_t *in)
{
    uint32_t b = g_in.buttons, b2 = g_in.buttons2;

    if (b & WEB_BTN_COIN1)   in->buttons |= M3_BTN_COIN1;
    if (b & WEB_BTN_COIN2)   in->buttons |= M3_BTN_COIN2;
    if (b & WEB_BTN_START1)  in->buttons |= M3_BTN_START1;
    if (b & WEB_BTN_START2)  in->buttons |= M3_BTN_START2;
    if (b & WEB_BTN_TEST)    in->buttons |= M3_BTN_TEST;
    if (b & WEB_BTN_SERVICE) in->buttons |= M3_BTN_SERVICE;

    m3_gun_from_screen(g_in.x, g_in.y, g_w, g_h, &in->gun_x[0], &in->gun_y[0]);
    if (b & WEB_BTN_TRIG) in->buttons |= M3_BTN_TRIG1;
    in->buttons |= reload(0, (b & WEB_BTN_RELOAD) != 0);

    if (b2) {
        m3_gun_from_screen(g_in.x2, g_in.y2, g_w, g_h, &in->gun_x[1], &in->gun_y[1]);
        if (b2 & WEB_BTN_TRIG) in->buttons |= M3_BTN_TRIG2;
    }
    in->buttons |= reload(1, (b2 & WEB_BTN_RELOAD) != 0);
}

/* Hold the display to the board's 57.524 Hz. Fields are paced by guest
 * work, not the clock, so without this the game runs as fast as the host
 * can draw it. M3_NOTHROTTLE turns it off; holding Tab does the same for
 * a moment. The audio ring is kept in step by the worklet's own rate
 * control, not by nudging the period as the SDL layer does. */
static void throttle(void)
{
    static int probed, off;
    static double next;
    double now;
    if (!probed) { probed = 1; off = getenv("M3_NOTHROTTLE") != NULL; }
    if (off || (g_in.buttons & WEB_BTN_FAST)) { next = 0; return; }
    now = emscripten_get_now();
    if (!next || now > next + 100.0)   /* first field, or fell far behind */
        next = now;
    next += 1000.0 / 57.524;
    if (next - now > 1.0)
        emscripten_thread_sleep(next - now - 0.5);   /* ms; the guest is on a pthread */
    while (emscripten_get_now() < next) { }
}

void platform_present(void)
{
    int back = g_front ^ 1;
    const uint32_t *s = g_fb;
    uint8_t *d = g_present[back];
    size_t i, n = (size_t)g_w * g_h;
    for (i = 0; i < n; i++) {
        uint32_t p = s[i];
        d[4 * i]     = (uint8_t)(p >> 16);
        d[4 * i + 1] = (uint8_t)(p >> 8);
        d[4 * i + 2] = (uint8_t)p;
        d[4 * i + 3] = 255;
    }
    __atomic_store_n(&g_front, back, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_frames, 1u, __ATOMIC_RELEASE);
    throttle();
}

/* A field's audio goes into the ring; the worklet on the audio thread
 * reads it out and keeps its distance from the write counter. */
void platform_audio(const int16_t *lr, int frames)
{
    uint32_t w = g_ring_write;
    int i;
    if (frames <= 0) return;
    for (i = 0; i < frames; i++) {
        size_t k = (size_t)((w + (uint32_t)i) % AUDIO_RING) * 2;
        g_ring[k]     = g_mute ? 0 : lr[2 * i];
        g_ring[k + 1] = g_mute ? 0 : lr[2 * i + 1];
    }
    __atomic_store_n(&g_ring_write, w + (uint32_t)frames, __ATOMIC_RELEASE);
}
