/* model3recomp -- library entry points and frame pacing. */
#include "model3recomp/savestate.h"
#include "model3recomp/model3recomp.h"
#include "model3recomp/platform.h"
#include "model3recomp/ppc.h"
#include "model3recomp/tilegen.h"
#include "model3recomp/real3d.h"
#include "model3recomp/input.h"
#include "model3recomp/netplay.h"
#include "model3recomp/sound.h"

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

#ifdef M3_WATCH
    { void m3_watch_init(void); m3_watch_init(); }
#endif
    bus_init(&g_cfg.roms);
    func_table_init();
    irq_init();
    ppc_reset();

    m3_safepoint_pc = g_cfg.safepoint_pc ? g_cfg.safepoint_pc : 0xFFFFFFFFu;
    sound_init(g_cfg.roms.sndrom, g_cfg.roms.sndrom_size,
               g_cfg.roms.samples, g_cfg.roms.samples_size);
    if (g_cfg.nvram_path) {
        size_t n = 0;
        uint8_t *b = bus_backup(&n);
        FILE *f = fopen(g_cfg.nvram_path, "rb");
        if (f && b) {
            size_t got = fread(b, 1, n, f);
            /* The EEPROM follows the battery RAM in the same file. */
            got += fread(bus_eeprom(), 1, 128, f);
            fprintf(stderr, "[model3recomp] battery RAM: %u bytes from %s\n",
                    (unsigned)got, g_cfg.nvram_path);
        }
        if (f) fclose(f);
    }

    if (!platform_init(g_cfg.width, g_cfg.height,
                       g_cfg.title ? g_cfg.title : "model3recomp"))
        return 0;
    /* After the window, so a host waiting for its player 2 is visible. */
    if (!netplay_init_from_env())
        fprintf(stderr, "[model3recomp] netplay did not start; playing alone\n");

    g_last_field_us = m3_work;
    g_running = 1;
    return 1;
}

const m3_config_t *model3recomp_config(void) { return &g_cfg; }

void model3recomp_save_nvram(void)
{
    /* A joiner's battery RAM came from the host; keep the player's own. */
    if (g_cfg.nvram_path && netplay_role() != 2) {
        size_t n = 0;
        uint8_t *b = bus_backup(&n);
        FILE *f = b ? fopen(g_cfg.nvram_path, "wb") : NULL;
        if (f) { fwrite(b, 1, n, f); fwrite(bus_eeprom(), 1, 128, f); fclose(f); }
    }
}

void model3recomp_quit(int code)
{
    model3recomp_save_nvram();
    netplay_shutdown();
    model3recomp_shutdown();
    exit(code);
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

    /* Fields are paced by how much work the guest has done, not by the
     * wall clock, and the two are not interchangeable.
     *
     * This used to ask the platform layer for the time. Headless, that
     * returns m3_work and everything was fine. Against SDL it returns real
     * microseconds, so fields arrived sixty times a second whether or not
     * the guest had got anywhere -- and a recompiled 603e uploading three
     * megabytes of texture does not run at real time. The guest was handed
     * a fraction of the instructions per field that it needs and sat on the
     * Sega logo for thirty thousand fields, while the same build headless
     * walked into attract mode.
     *
     * A host that wants to run at the right speed throttles in
     * platform_present(); it does not get to decide when the guest's video
     * hardware retires a frame. */
    now = m3_work;
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

    /* Back to front: the tilemap layers the guest has put behind the 3D,
     * then the 3D, then the ones it has put in front. Which is which is a
     * field in the tilegen's register 0x20, and until it was read the sky
     * was drawn over the dinosaur. */
    {
        int fw = 0, fh = 0;
        uint32_t *fb = platform_framebuffer(&fw, &fh);
        if (fb) memset(fb, 0, (size_t)fw * (size_t)fh * sizeof(uint32_t));
    }
    {
        /* M3_PROFILE: where a field's time goes, printed every 300. */
        static int on = -1;
        static uint64_t t_guest, t_2d, t_3d, t_present, last, n;
        uint64_t t0, t1, t2, t3, t4;
        if (on < 0) on = getenv("M3_PROFILE") != NULL;
        t0 = on ? platform_ticks_us() : 0;
        tilegen_render_pass(0);
        t1 = on ? platform_ticks_us() : 0;
        real3d_render();
        t2 = on ? platform_ticks_us() : 0;
        tilegen_render_pass(1);
        t3 = on ? platform_ticks_us() : 0;
        platform_present();
        t4 = on ? platform_ticks_us() : 0;
        if (on) {
            if (last) t_guest += t0 - last;
            t_2d += (t1 - t0) + (t3 - t2); t_3d += t2 - t1; t_present += t4 - t3;
            last = t4;
            if (++n % 300 == 0) {
                extern uint64_t g_board_us, g_wait_us;
                fprintf(stderr, "[profile] per field: guest %.1f ms  2d %.1f  3d %.1f  present %.1f"
                                "  sound %.1f (guest waited %.1f)\n",
                        t_guest / 300000.0, t_2d / 300000.0, t_3d / 300000.0, t_present / 300000.0,
                        g_board_us / 300000.0, g_wait_us / 300000.0);
                t_guest = t_2d = t_3d = t_present = 0;
                g_board_us = g_wait_us = 0;
            }
        }
    }
    if (g_frame_hook)
        g_frame_hook();
    sound_run_field();
    if (!model3recomp_poll())
        model3recomp_quit(0);
    /* The next field's inputs, fixed now for every read the guest makes. */
    m3_input_latch();
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

void model3recomp_state(m3_state_t *st)
{
    M3_STATE_VAR(st, g_last_field_us);
    M3_STATE_VAR(st, g_frames);
}
