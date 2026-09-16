/* model3recomp -- library entry points and frame pacing. */
#include "model3recomp/model3recomp.h"
#include "model3recomp/platform.h"
#include "model3recomp/tilegen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static m3_config_t g_cfg;
static int         g_running;
static uint64_t    g_last_field_us;

/* The board runs at 57.52 Hz, not 60 -- 496x384 at the Model 3's dot clock.
 * Using 60 here makes every attract-mode animation run 4% fast, which is the
 * sort of thing nobody notices until the music desyncs. */
#define FIELD_US 17385

int model3recomp_init(const m3_config_t *cfg)
{
    g_cfg = *cfg;
    if (!g_cfg.width)  g_cfg.width  = 496;
    if (!g_cfg.height) g_cfg.height = 384;

    bus_init(&g_cfg.roms);
    func_table_init();
    irq_init();
    ppc_reset();

    if (!platform_init(g_cfg.width, g_cfg.height,
                       g_cfg.title ? g_cfg.title : "model3recomp"))
        return 0;

    g_last_field_us = platform_ticks_us();
    g_running = 1;
    return 1;
}

void model3recomp_shutdown(void)
{
    platform_shutdown();
    bus_shutdown();
    g_running = 0;
}

int model3recomp_field_due(void)
{
    uint64_t now;
    /* irq_tick() runs from func_table_call(), which a test or a tool can
     * reach without ever bringing the board up. No window, no frames. */
    if (!g_running)
        return 0;
    now = platform_ticks_us();
    if (now - g_last_field_us < FIELD_US)
        return 0;
    g_last_field_us = now;
    return 1;
}

void model3recomp_begin_frame(void) { }

static uint64_t g_frames;
static void (*g_frame_hook)(void);

uint64_t model3recomp_frame_count(void) { return g_frames; }

void model3recomp_set_frame_hook(void (*hook)(void)) { g_frame_hook = hook; }

void model3recomp_end_frame(void)
{
    g_frames++;
    if (getenv("M3_TRACE_FRAMES"))
        fprintf(stderr, "[model3recomp] field %llu\n",
                (unsigned long long)g_frames);

    /* Real3D lands here when it is written; the tilemaps go under it. */
    tilegen_render();
    platform_present();
    if (g_frame_hook)
        g_frame_hook();
    if (!model3recomp_poll())
        g_running = 0;
}

int model3recomp_poll(void) { return platform_poll(); }

void model3recomp_run(void)
{
    /* Every Model 3 title starts at the PowerPC reset vector. Unlike the i960
     * on Model 2 there is no relocation hop to follow: the 603e simply fetches
     * from 0xFFF00100 with MSR[IP] set. */
    if (!func_table_call(0xFFF00100u)) {
        fprintf(stderr, "[model3recomp] reset vector 0xFFF00100 was not "
                        "lifted -- nothing to run\n");
        return;
    }
    /* Reached only if the guest's init returns, which a running game never
     * does. Keep presenting so the host does not appear hung. */
    while (g_running)
        model3recomp_end_frame();
}
