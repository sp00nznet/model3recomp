/* model3recomp -- framebuffer capture.
 *
 * A recompiled game never returns, so the only way to see what it drew is to
 * grab the framebuffer from the frame hook. PPM because it is eight lines of
 * code and every tool reads it; no image library is worth a dependency here.
 */
#include "model3recomp/model3recomp.h"
#include "model3recomp/platform.h"

#include <stdio.h>

int model3recomp_screenshot(const char *path)
{
    int w = 0, h = 0, x, y;
    uint32_t *fb = platform_framebuffer(&w, &h);
    FILE *f;

    if (!fb || w <= 0 || h <= 0)
        return 0;
    f = fopen(path, "wb");
    if (!f)
        return 0;

    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint32_t p = fb[y * w + x];
            fputc((int)((p >> 16) & 0xFF), f);
            fputc((int)((p >> 8) & 0xFF), f);
            fputc((int)(p & 0xFF), f);
        }
    }
    fclose(f);
    return 1;
}
