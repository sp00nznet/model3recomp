/* model3recomp -- headless platform layer.
 *
 * Used when SDL2 is not available, and by the conformance harness, which
 * wants frames to tick as fast as possible with nothing on screen. The
 * framebuffer is real, so renderers can be tested against it by checksum
 * without a window.
 */
#include "model3recomp/platform.h"
#include "model3recomp/model3recomp.h"

#include <stdlib.h>
#include <time.h>

static uint32_t *g_fb;
static int g_w, g_h;
static uint64_t g_virtual_us;

/* Frames are not paced against a wall clock here: a headless run should go as
 * fast as it can, and a conformance run must be deterministic. Instead the
 * virtual clock advances a tick per query, so a field falls due after a fixed
 * amount of guest work.
 *
 * Returning "a field is always due" instead -- which this did at first -- is
 * not merely fast, it is broken: irq_tick() runs from func_table_call(), so
 * the guest takes an interrupt on every single indirect call, its main loop
 * never gets to run between them, and the game ticks frames forever without
 * making any progress. The symptom was 17,000 fields rendered and not one
 * byte written to tilemap VRAM. */
#define FIELD_US 17385

int platform_init(int w, int h, const char *title)
{
    (void)title;
    g_w = w; g_h = h;
    g_fb = calloc((size_t)w * h, sizeof(uint32_t));
    g_virtual_us = 0;
    return g_fb != NULL;
}

void platform_shutdown(void) { free(g_fb); g_fb = NULL; }

uint32_t *platform_framebuffer(int *w, int *h)
{
    if (w) *w = g_w;
    if (h) *h = g_h;
    return g_fb;
}

int platform_poll(void) { return 1; }

void platform_present(void) { }

/* Pacing off calls alone stalls: the guest spends long stretches polling
 * devices without dispatching through a pointer, so fields stop arriving
 * entirely. m3_work counts both. */
uint64_t platform_ticks_us(void)
{
    g_virtual_us = m3_work;
    return g_virtual_us;
}
