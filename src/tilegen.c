/* model3recomp -- Model 3 tilemap generator.
 *
 * Four scrolling 8x8 tile layers over a 496x384 display. This draws the 2D
 * half of the picture: the service menu, the boot report, and the flat parts
 * of attract mode.
 *
 * VRAM is one block at 0xF1000000 and the layout below was read off a live
 * dump, then checked by rendering text whose wording is known in advance --
 * the boot report reads "THE LOST WORLD / Copyright (c) 1997 SEGA Enterprises
 * CO.,LTD". A decode that produces those words is right; one that produces
 * plausible-looking noise is not, and several did.
 *
 *     0x000000..0x0F5FFF   tile pattern data
 *     0x0F6000..0x0F7FFF   scroll tables
 *     0x0F8000..0x0FFFFF   four name tables, 0x2000 each
 *     0x100000..0x11FFFF   palette, 32768 entries of 32 bits
 *
 * The scroll tables are not name tables, which is what the previous version
 * of this file assumed. The boot fills 0x0F6000 with the entry 0x0080 and
 * 0x0F7000 with 0xFFFF -- visible as two of the memsets it issues -- so
 * reading them as a map draws one tile over the whole screen, forever.
 *
 * Everything the graphics side reads is little-endian, because the Real3D and
 * its tile generator sit on the PCI side of the board; the PowerPC is not.
 * That one fact explains all three of the orderings below, and getting any of
 * them wrong still produces a picture, just not the right one.
 */
#include "model3recomp/tilegen.h"
#include "model3recomp/bus.h"
#include "model3recomp/platform.h"
#include "model3recomp/model3recomp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define VRAM_PATTERN      0x000000u
#define VRAM_STENCIL      0x0F7000u
#define VRAM_NAME_BASE    0x0F8000u
#define VRAM_PALETTE      0x100000u
#define VRAM_END          0x120000u

#define LAYERS       4
#define LAYER_STRIDE 0x2000u          /* 64 x 64 entries of 16 bits */
#define MAP_W        64
#define TILE         8
#define TILE_BYTES   32u              /* 8x8 at 4bpp */

static int g_enable_layer[LAYERS] = {1, 1, 1, 1};
static unsigned g_layer_mask = 0xFu;

void tilegen_enable_layer(int layer, int on)
{
    if (layer >= 0 && layer < LAYERS)
        g_enable_layer[layer] = on;
}

/* The tilegen's registers are on the PCI side, so a word read back here is
 * reversed from the way the guest stored it -- 0x7F010000 written is 0x17F
 * seen, which is 383, the last scanline of a 384-line display. */
static uint32_t reg(unsigned idx)
{
    uint32_t v = bus_tilegen_regs()[idx / 4];
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) |
           ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}

/* A layer is off when the top bit of its scroll register is clear. */
static int layer_enabled(int layer)
{
    return (reg(0x60u + (unsigned)layer * 4u) >> 31) & 1u;
}

/* Register 0x20: a nibble of bit depths (set = four bits a pixel) above a
 * nibble of priorities (set = in front of the 3D), as MAME documents it.
 * It was the other way round here for a while: through most of attract both
 * nibbles read 3, so the two orders agree. They part at 0x3200, which Lost
 * World sets for its title logo and its credits roll -- read backwards,
 * layer 0 decodes as eight-bit and both screens are noise. */
static int layer_4bpp(int layer)
{
    return (reg(0x20) >> (12 + layer)) & 1u;
}

static int layer_above_3d(int layer)
{
    return (reg(0x20) >> (8 + layer)) & 1u;
}

/* A name-table entry. Two of them share each 32-bit word, and the whole word
 * is little-endian -- so the tile drawn first is the one in the second
 * halfword, and the halfword itself is byte-swapped too. Get the pair order
 * wrong and every two characters trade places: "THE LOST WORLD" comes out as
 * "T.EHL SO TOWLR.D", which is legible enough to look like a font problem.
 */
static inline unsigned name_entry(const uint8_t *v, uint32_t base, unsigned i)
{
    uint32_t off = base + (uint32_t)(i ^ 1u) * 2u;
    return ((unsigned)v[off + 1] << 8) | v[off];
}

/* Palette entry: a little-endian 32-bit word whose low 16 bits are the
 * colour, 1-5-5-5 with bit 15 meaning transparent. */
static uint32_t palette_argb(const uint8_t *v, unsigned index, int *transparent)
{
    uint32_t off = VRAM_PALETTE + index * 4u;
    unsigned c;
    uint32_t r, g, b;

    *transparent = 1;
    if (off + 4u > VRAM_END)
        return 0;

    /* Bytes 0 and 1 of the big-endian word are the low half once reversed. */
    c = ((unsigned)v[off + 1] << 8) | v[off];
    if (c & 0x8000u)
        return 0;
    *transparent = 0;

    /* Blue is the top field and red the bottom, not the other way round. A
     * palette read the wrong way round still produces a picture in plausible
     * colours -- it only ever swaps red and blue, so greys and the boot
     * report's flat white come out identical either way, and the attract
     * screen's orange text comes out as the exact same blue every time.
     * 0x00A5FF reversed is 0xFFA500, which is how this was caught. */
    b = (c >> 10) & 0x1Fu;
    g = (c >> 5) & 0x1Fu;
    r = c & 0x1Fu;
    /* 5 bits to 8, replicating the high bits so 0x1F maps to 0xFF. */
    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* The palette index of one pixel of a layer, scroll already applied: (sx, sy)
 * is a position in the layer's 512x512 map. A 4bpp row is four bytes read as
 * one little-endian word with the leftmost pixel in the top nibble, so the
 * byte the leftmost pixel lives in is the last of the four -- read the other
 * way round every glyph comes out mirrored, invisible on T, H, O and X. An
 * 8bpp row is two such words, a byte a pixel, same reversal.
 *
 * Name entries: in 4bpp, bits 14..1 are the tile number and bit 15 its low
 * bit, and bits 14..4 are also the top of the colour index. In 8bpp the low
 * fourteen bits are the tile (64 bytes each) and bits 14..8 the palette. The
 * palette field is why the boot report is the wrong thing to check a decode
 * against: all its entries carry palette 0x20. */
static int layer_pixel(const uint8_t *v, int layer, int four_bit,
                       unsigned sx, unsigned sy)
{
    uint32_t nt = VRAM_NAME_BASE + (uint32_t)layer * LAYER_STRIDE;
    unsigned e = name_entry(v, nt, (sy / TILE) * MAP_W + sx / TILE);
    unsigned row = sy & 7u, col = sx & 7u;
    uint32_t pattern;

    if (!e)
        return -1;
    if (four_bit) {
        uint8_t byte;
        pattern = (((uint32_t)(e & 0x3FFFu) << 1) | ((e >> 15) & 1u))
                * TILE_BYTES;
        byte = v[pattern + row * 4u + (3u - (col >> 1))];
        return (int)((e & 0x7FF0u) | ((col & 1u) ? (byte & 0x0Fu) : (byte >> 4)));
    }
    pattern = (uint32_t)(e & 0x3FFFu) * (TILE_BYTES * 2u);
    return (int)((e & 0x7F00u) | v[pattern + row * 8u + (col ^ 3u)]);
}

static uint32_t le32(const uint8_t *v, uint32_t off)
{
    return (uint32_t)v[off] | ((uint32_t)v[off + 1] << 8) |
           ((uint32_t)v[off + 2] << 16) | ((uint32_t)v[off + 3] << 24);
}

/* Registers 0x40 (layers 0/1) and 0x44 (2/3), red, green and blue a byte
 * each below an empty top byte (MAME documents them, with a question mark,
 * as colour modulation). What they do comes from what the game writes: the
 * SEGA logo fades in with 0x81 stepping up to 0xFF over forty fields and
 * then 0, and the attract screens fade out the same way down from 0xF4. As
 * signed bytes that is -127 to -1 -- so a byte is an offset added to the
 * channel, and for -127 to be black it moves the channel two steps a unit.
 * Positive values brighten: the lightning in attract is 0x7F. */
static uint32_t offset_colour(uint32_t argb, uint32_t r)
{
    uint32_t out = 0xFF000000u;
    int sh;
    for (sh = 0; sh < 24; sh += 8) {
        int v = (int)((argb >> sh) & 0xFFu) + 2 * (int8_t)((r >> sh) & 0xFFu);
        out |= (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v) << sh;
    }
    return out;
}

#ifdef M3_TILEGEN_DUMP
/* The registers and the layer occupancy, printed whenever register 0x20
 * changes. Printing it per field is useless -- it changes four times in an
 * attract cycle, and it was reading it on change that showed the enable bits
 * do not move at all. */
static void dump_state(const uint8_t *v)
{
    const uint32_t *rg = bus_tilegen_regs();
    unsigned i, L;
    fprintf(stderr, "[tilegen] f=%llu r20=%08X r40=%08X r44=%08X "
                    "scroll=%08X/%08X/%08X/%08X",
            (unsigned long long)model3recomp_frame_count(),
            rg[0x20/4], rg[0x40/4], rg[0x44/4],
            rg[0x60/4], rg[0x64/4], rg[0x68/4], rg[0x6C/4]);
    (void)i;
    for (L = 0; L < LAYERS; L++) {
        uint32_t b = VRAM_NAME_BASE + L * LAYER_STRIDE;
        unsigned nz = 0, first = 0xFFFFu, last = 0;
        for (i = 0; i < LAYER_STRIDE / 2u; i++)
            if (name_entry(v, b, i)) {
                nz++; if (first == 0xFFFFu) first = i; last = i;
            }
        if (nz)
            fprintf(stderr, "  L%u=%u(r%u-%u)", L, nz, first / MAP_W, last / MAP_W);
    }
    fprintf(stderr, "%s", "\n");
    fflush(stderr);
}
#endif

void tilegen_render_pass(int above)
{
    int fbw = 0, fbh = 0;
    uint32_t *fb = platform_framebuffer(&fbw, &fbh);
    const uint8_t *v = bus_vram(NULL);
    int x, y;

    if (!fb || !v)
        return;

    /* M3_NO_2D: draw the 3D on its own. The tilemaps are opaque over most of
     * the screen and sit above the 3D during a round, so this is the only
     * way to see what the Real3D actually produced. */
    if (getenv("M3_NO_2D"))
        return;

    /* No clear here: real3d_render() has already drawn the 3D scene into
     * this buffer and cleared what it did not cover. The tilemaps go over
     * it, which is why they are drawn second. */

#ifdef M3_TILEGEN_DUMP
    {
        static uint32_t last = 0xFFFFFFFFu;
        uint32_t r20 = bus_tilegen_regs()[0x20 / 4];
        if (r20 != last) { last = r20; dump_state(v); }
    }
#endif


#ifdef M3_LOOP_GUARD
    /* What the renderer is actually handed, every so often. A black screen is
     * either an empty map or a decode that drops everything, and the two look
     * identical from the outside. */
    {
        static unsigned n;
        if ((n++ & 0x3Fu) == 0) {
            int L;
            fprintf(stderr, "[model3recomp] tilegen:");
            for (L = 0; L < LAYERS; L++) {
                uint32_t b = VRAM_NAME_BASE + (uint32_t)L * LAYER_STRIDE;
                unsigned nz = 0, i;
                for (i = 0; i < LAYER_STRIDE / 2u; i++)
                    if (name_entry(v, b, i)) nz++;
                fprintf(stderr, "  L%d=%u", L, nz);
            }
            fprintf(stderr, "\n");
        }
    }
#endif

    /* Back to front. Transparency comes from the palette rather than from
     * "index 0 on any layer but the bottom", so the bottom layer is not a
     * special case and a screen nothing wrote to stays black. */
    if (getenv("M3_TILEGEN_REGS")) {
        static uint32_t last = 0xFFFFFFFFu;
        const uint32_t *rg = bus_tilegen_regs();
        if (rg[0x20/4] != last) {
            last = rg[0x20/4];
            fprintf(stderr, "[tilegen] field %llu r20=%08X (swapped %08X)\n",
                    (unsigned long long)model3recomp_frame_count(),
                    rg[0x20/4],
                    ((rg[0x20/4] & 0xFFu) << 24) | ((rg[0x20/4] & 0xFF00u) << 8) |
                    ((rg[0x20/4] >> 8) & 0xFF00u) | ((rg[0x20/4] >> 24) & 0xFFu));
            { int L;
              for (L = 0; L < LAYERS; L++)
                  fprintf(stderr, "          L%d scroll %08X  enabled %d"
                                  "  4bpp %d  above3d %d\n",
                          L, reg(0x60u + (unsigned)L * 4u),
                          layer_enabled(L), layer_4bpp(L),
                          layer_above_3d(L)); }
            fflush(stderr);
        }
    }

    /* M3_LAYER_MASK=<bits>: draw only the layers whose bit is set. */
    {
        static int probed;
        if (!probed) { const char *e = getenv("M3_LAYER_MASK");
                       probed = 1;
                       if (e) g_layer_mask = (unsigned)strtoul(e, NULL, 0); }
    }

    /* The layers come in pairs, A/A' (0/1) and B/B' (2/3), and per pixel
     * only one of a pair shows (MAME's draw_layer). The stencil at 0x0F7000
     * picks which, a word a screen line: its high half for pair A, its low
     * for B, a bit per 32 pixels of the layer, set for the first of the
     * pair. The Lost World only ever writes whole halves of 0xFFFF or 0,
     * a line at a time, to wipe between screens. Each layer scrolls by its
     * register: Y in bits 16..24, X in 0..8. Pair B is drawn behind A.
     *
     * Not done: the per-line scroll table at 0x0F6000. The Lost World never
     * turns it on. */
    for (y = 0; y < fbh && y < 384; y++) {
        uint32_t stencil = le32(v, VRAM_STENCIL + (uint32_t)y * 4u);
        int pair;
        for (pair = 1; pair >= 0; pair--) {
            unsigned half = pair ? (stencil & 0xFFFFu) : (stencil >> 16);
            uint32_t coff = reg(0x40u + (unsigned)pair * 4u);
            int sx[2], sy[2], on[2], four[2], k;
            for (k = 0; k < 2; k++) {
                int L = pair * 2 + k;
                uint32_t sr = reg(0x60u + (unsigned)L * 4u);
                on[k] = g_enable_layer[L] && layer_enabled(L) && ((g_layer_mask >> L) & 1u)
                     && layer_above_3d(L) == (above != 0);
                four[k] = layer_4bpp(L);
                sx[k] = (int)(sr & 0x1FFu);
                sy[k] = (int)((sr >> 16) & 0x1FFu);
            }
            if (!on[0] && !on[1])
                continue;
            for (x = 0; x < fbw && x < 496; x++) {
                int idx, clear;
                uint32_t argb;
                unsigned lx0 = (unsigned)(x + sx[0]) & 511u;
                k = ((half >> (lx0 >> 5)) & 1u) ? 0 : 1;
                if (!on[k])
                    continue;
                idx = layer_pixel(v, pair * 2 + k, four[k], (unsigned)(x + sx[k]) & 511u,
                                  (unsigned)(y + sy[k]) & 511u);
                if (idx < 0)
                    continue;
                argb = palette_argb(v, (unsigned)idx, &clear);
                if (!clear)
                    fb[y * fbw + x] = coff ? offset_colour(argb, coff) : argb;
            }
        }
    }
}
