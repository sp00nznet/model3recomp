/* model3recomp -- Model 3 tilemap generator.
 *
 * Four scrolling 8x8 tile layers over a 496x384 display. This draws the 2D
 * half of the picture: the service menu, the HUD, and the flat parts of
 * attract mode.
 *
 * VRAM is one block at 0xF1000000, and the split below was read off a live
 * dump of the game rather than taken from a document:
 *
 *     0x000000..0x0F5FFF   tile pattern data
 *     0x0F6000..0x0FFFFF   name tables
 *     0x100000..0x11FFFF   palette, 32768 entries of 32 bits
 *
 * The exact name-table base and the bit layout of a name-table entry are the
 * parts still being pinned down against real content -- see TILEGEN_NAME_BASE
 * below. Everything else here is structural and does not depend on that.
 */
#include "model3recomp/tilegen.h"
#include "model3recomp/bus.h"
#include "model3recomp/platform.h"

#include <string.h>

#define VRAM_PATTERN     0x000000u
/* Read off a live dump rather than assumed: the game's name-table writes land
 * at 0x0F6000, not the 0x0F8000 that four 0x2000 layers below 0x100000 would
 * suggest. One 64x64 layer of 16-bit entries is 0x2000 bytes. */
#define TILEGEN_NAME_BASE 0x0F6000u
#define VRAM_PALETTE     0x100000u

#define LAYERS      4
#define LAYER_STRIDE 0x2000u          /* 64 x 64 entries of 16 bits */
#define MAP_W       64
#define MAP_H       64
#define TILE        8

static int g_enable_layer[LAYERS] = {1, 1, 1, 1};

void tilegen_enable_layer(int layer, int on)
{
    if (layer >= 0 && layer < LAYERS)
        g_enable_layer[layer] = on;
}

static inline uint16_t vram16(const uint8_t *v, uint32_t off)
{
    return (uint16_t)(((uint32_t)v[off] << 8) | v[off + 1]);
}

/* Palette entries are 32 bits with the colour in the upper half, as the
 * game's own writes show: 0x0080 0000, 0x3967 0000, 0x9452 0000. The colour
 * itself is 1-5-5-5. */
static uint32_t palette_argb(const uint8_t *v, unsigned index)
{
    uint32_t off = VRAM_PALETTE + index * 4u;
    uint16_t c;
    uint32_t r, g, b;

    if (off + 4u > 0x120000u)
        return 0xFF000000u;
    c = vram16(v, off);

    r = (c >> 10) & 0x1Fu;
    g = (c >> 5) & 0x1Fu;
    b = c & 0x1Fu;
    /* 5 bits to 8, replicating the high bits so 0x1F maps to 0xFF. */
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* One 8x8 4bpp tile: 32 bytes, two pixels per byte. Colour index 0 is
 * transparent in every layer but the bottom one. */
static void draw_tile(uint32_t *fb, int fbw, int fbh,
                      const uint8_t *v, uint32_t pattern,
                      unsigned pal_base, int px, int py, int opaque)
{
    int row, col;

    if (pattern + 32u > TILEGEN_NAME_BASE)
        return;

    for (row = 0; row < TILE; row++) {
        int y = py + row;
        if (y < 0 || y >= fbh)
            continue;
        for (col = 0; col < TILE; col++) {
            int x = px + col;
            uint8_t byte;
            unsigned idx;
            if (x < 0 || x >= fbw)
                continue;
            byte = v[pattern + row * 4u + (unsigned)(col >> 1)];
            idx = (col & 1) ? (byte & 0x0Fu) : (byte >> 4);
            if (!idx && !opaque)
                continue;
            fb[y * fbw + x] = palette_argb(v, pal_base + idx);
        }
    }
}

void tilegen_render(void)
{
    int fbw = 0, fbh = 0;
    uint32_t *fb = platform_framebuffer(&fbw, &fbh);
    const uint8_t *v = bus_vram(NULL);
    int layer;

    if (!fb || !v)
        return;

    /* Layers draw back to front; the bottom one is opaque so the frame is
     * fully covered even where nothing wrote a tile. */
    memset(fb, 0, (size_t)fbw * (size_t)fbh * sizeof(uint32_t));

    for (layer = LAYERS - 1; layer >= 0; layer--) {
        uint32_t nt = TILEGEN_NAME_BASE + (uint32_t)layer * LAYER_STRIDE;
        int ty, tx;

        if (!g_enable_layer[layer])
            continue;

        for (ty = 0; ty < MAP_H; ty++) {
            if (ty * TILE >= fbh)
                break;
            for (tx = 0; tx < MAP_W; tx++) {
                uint16_t e;
                uint32_t pattern;
                unsigned pal;
                if (tx * TILE >= fbw)
                    break;
                e = vram16(v, nt + (uint32_t)(ty * MAP_W + tx) * 2u);
                if (!e)
                    continue;
                /* The pattern address is the entry times 16, not times 32:
                 * name entry 0x0080 in a live dump resolves to byte 0x800,
                 * where there is a real glyph, while 0x0080 * 32 = 0x1000 is
                 * blank. So the entry's low bit is an attribute and the tile
                 * number sits above it. */
                /* 15 bits of tile index: 0xFFFF * 16 would run off the end
                 * of the pattern area, and the game does fill unused rows
                 * with 0xFFFF. */
                pattern = ((uint32_t)e & 0x7FFFu) * 16u;
                pal = (unsigned)((e >> 14) & 0x3u) * 16u;
                draw_tile(fb, fbw, fbh, v, pattern, pal,
                          tx * TILE, ty * TILE, layer == LAYERS - 1);
            }
        }
    }
}
