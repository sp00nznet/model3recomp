/* model3recomp -- Real3D scene memory capture.
 *
 * The renderer is written against what a game actually puts in these buffers,
 * not against a guess, so there has to be a way to get them off the machine.
 * Same reasoning as screenshot.c: called from the frame hook, because a
 * Model 3 main loop never returns.
 */
#include "model3recomp/model3recomp.h"
#include "model3recomp/bus.h"

#include <stdio.h>
#include <string.h>

static int dump_one(const char *prefix, const char *what,
                    const uint8_t *p, size_t n)
{
    char path[512];
    FILE *f;
    if (!p || !n)
        return 0;
    snprintf(path, sizeof path, "%s_%s.bin", prefix, what);
    f = fopen(path, "wb");
    if (!f)
        return 0;
    fwrite(p, 1, n, f);
    fclose(f);
    return 1;
}

int model3recomp_dump_scene(const char *prefix)
{
    size_t n;
    uint8_t *p;
    int ok = 1;

    p = bus_cull_lo(&n); ok &= dump_one(prefix, "cull_lo", p, n);
    p = bus_cull_hi(&n); ok &= dump_one(prefix, "cull_hi", p, n);
    p = bus_poly(&n);    ok &= dump_one(prefix, "poly",    p, n);
    p = bus_vram(&n);    ok &= dump_one(prefix, "vram",    p, n);
    p = bus_ram();       ok &= dump_one(prefix, "ram", p, 0x00800000u);
    { size_t used = 0; p = bus_texfifo(&used, NULL);
      if (used) ok &= dump_one(prefix, "texfifo", p, used); }
    return ok;
}
