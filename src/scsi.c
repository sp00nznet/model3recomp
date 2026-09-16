/* model3recomp -- NCR 53C810 SCSI controller, DMA engine only.
 *
 * Step 1.x boards shift bulk data with the SCSI controller's SCRIPTS
 * processor rather than the host bridge. No actual SCSI bus is involved and
 * no device is attached: the game uses it as a memory-to-memory DMA engine.
 *
 * Only what The Lost World's boot exercises is modelled -- memory move,
 * jump, and interrupt. Block moves would need a real SCSI target and never
 * happen here.
 *
 * The chip is on the PCI side and is little-endian, so every 32-bit quantity
 * is byte-reversed relative to the PowerPC. This is visible in the game's own
 * traffic: it writes DSP = 0x4C2C1A00, which is the address 0x001A2C4C.
 */
#include "model3recomp/scsi.h"
#include "model3recomp/bus.h"
#include "model3recomp/irq.h"

#include <stdio.h>
#include <string.h>

/* Register offsets that matter. */
#define R_DSTAT  0x0C
#define R_ISTAT  0x14
#define R_DSP    0x2C
#define R_DMODE  0x38
#define R_DCNTL  0x3B

#define DMODE_MAN 0x01      /* manual start: DSP writes do not launch */
#define DCNTL_STD 0x04      /* start DMA */

#define ISTAT_DIP 0x01      /* DMA interrupt pending */
#define DSTAT_SIR 0x04      /* SCRIPTS interrupt instruction */

static uint8_t g_reg[0x40];

static uint32_t swap32(uint32_t v)
{
    return ((v >> 24) & 0xFFu) | ((v >> 8) & 0xFF00u) |
           ((v << 8) & 0xFF0000u) | ((v << 24) & 0xFF000000u);
}

static uint32_t reg32(unsigned off)
{
    return ((uint32_t)g_reg[off] << 24) | ((uint32_t)g_reg[off + 1] << 16) |
           ((uint32_t)g_reg[off + 2] << 8) | g_reg[off + 3];
}

void scsi_init(void)
{
    memset(g_reg, 0, sizeof g_reg);
}

/* Run a SCRIPTS program. Bounded: a malformed script must not wedge the host,
 * and a real one is a handful of instructions. */
/* Somewhere sane to move data to or from. A malformed script must not be able
 * to walk the whole address space a byte at a time -- that showed up as
 * twenty million device accesses to consecutive unmapped addresses. */
static int mapped(uint32_t a, uint32_t len)
{
    uint32_t end = a + len;
    if (end < a) return 0;                       /* wrapped */
    if (end <= M3_RAM_SIZE) return 1;            /* work RAM */
    if (a >= 0xFF000000u) return 1;              /* CROM */
    if (a >= 0x8C000000u && end <= 0x8C400000u) return 1;   /* culling lo  */
    if (a >= 0x8E000000u && end <= 0x8E100000u) return 1;   /* culling hi  */
    if (a >= 0x98000000u && end <= 0x98400000u) return 1;   /* polygon RAM */
    if (a >= 0xF1000000u && end <= 0xF1120000u) return 1;   /* tilegen     */
    /* The Real3D command and texture ports. Leaving these out is not a
     * hypothetical: the game's very first real SCRIPTS program is
     *
     *     C000000C  memory move, 12 bytes
     *     001BAA50  from work RAM
     *     9C000000  to the texture port
     *     98080000  INT
     *
     * Rejecting it meant the completion interrupt never fired and the guest
     * polled ISTAT twenty million times. */
    if (a >= 0x88000000u && end <= 0x88000100u) return 1;   /* R3D trigger */
    /* The game DMAs here too, from RAM 0x0010FA94 through the same helper.
     * Omitting it meant those uploads were rejected as out of range. */
    if (a >= 0x90000000u && end <= 0x90100000u) return 1;   /* R3D port 2  */
    if (a >= 0x9C000000u && end <= 0x9D000000u) return 1;   /* R3D texture */
    return 0;
}

static void scsi_run(uint32_t dsp)
{
    unsigned steps;
    uint64_t moved = 0;
    static uint64_t runs;

    if ((++runs % 1000000ull) == 0)
        fprintf(stderr, "[model3recomp] %llu SCRIPTS programs run\n",
                (unsigned long long)runs);

    for (steps = 0; steps < 4096; steps++) {
        uint32_t d0 = swap32(bus_read32(dsp));
        uint32_t d1 = swap32(bus_read32(dsp + 4));
        uint8_t  cmd = (uint8_t)(d0 >> 24);

        if ((cmd & 0xC0u) == 0xC0u) {              /* memory move */
            uint32_t cnt = d0 & 0x00FFFFFFu;
            uint32_t src = d1;
            uint32_t dst = swap32(bus_read32(dsp + 8));
            dsp += 12;
            if (!cnt || cnt > 0x00400000u)
                continue;
            if (!mapped(src, cnt) || !mapped(dst, cnt)) {
                /* Reported once. The boot's register test writes a pattern
                 * over DSP, so this fires whenever that pattern happens to
                 * point at something; it is noise, not an event. */
                static int told;
                if (!told++)
                    fprintf(stderr, "[model3recomp] SCRIPTS move out of "
                                    "range: %08X -> %08X, %u bytes "
                                    "(further reports silenced)\n",
                            src, dst, cnt);
                /* Skip the move but let the program run on, so its INT
                 * still signals completion and the guest is not left
                 * polling ISTAT forever. */
                continue;
            }
            moved += cnt;
            if (moved > 0x01000000u) {
                fprintf(stderr, "[model3recomp] SCRIPTS moved over 16 MB; "
                                "stopping\n");
                return;
            }
            bus_dma_copy(dst, src, cnt);
            continue;
        }
        if ((cmd & 0xC0u) == 0x80u) {              /* transfer control */
            unsigned op = (cmd >> 3) & 7u;
            if (op == 0) { dsp = d1; continue; }   /* JUMP */
            if (op == 3) {                          /* INT */
                g_reg[R_ISTAT] |= ISTAT_DIP;
                g_reg[R_DSTAT] |= DSTAT_SIR;
                /* A SCRIPTS interrupt does not just set a status bit: the
                 * chip asserts its PCI interrupt line, and the system
                 * controller turns that into IRQ 0x04000000. Setting the
                 * status alone leaves a guest that sequences its loading on
                 * that interrupt waiting for something that has already
                 * happened. */
                irq_raise(M3_IRQ_SCSI);
                return;
            }
            return;                                 /* CALL/RETURN: unused */
        }
        return;                                     /* block move: no target */
    }
    fprintf(stderr, "[model3recomp] SCRIPTS program did not terminate\n");
}

uint32_t scsi_read(uint32_t addr, unsigned size)
{
    unsigned off = addr & 0x3Fu;
    uint32_t v = 0;
    unsigned i;
    for (i = 0; i < size && off + i < sizeof g_reg; i++)
        v = (v << 8) | g_reg[off + i];

    /* Reading DSTAT clears the DMA interrupt, as the real chip does; the game
     * polls ISTAT and then reads DSTAT to find out why. */
    if (off == R_DSTAT) {
        g_reg[R_DSTAT] = 0;
        g_reg[R_ISTAT] &= (uint8_t)~ISTAT_DIP;
    }
    return v;
}

void scsi_write(uint32_t addr, uint32_t v, unsigned size)
{
    unsigned off = addr & 0x3Fu;
    unsigned i;
    for (i = 0; i < size && off + i < sizeof g_reg; i++)
        g_reg[off + i] = (uint8_t)(v >> (8 * (size - 1 - i)));

    /* Writing DSP starts the program it points at -- unless the chip is in
     * manual-start mode, in which case it waits for DCNTL[STD].
     *
     * This is not pedantry. The boot sweeps the whole register file with a
     * test pattern, DSP included, and without MAN every one of those writes
     * launches a "SCRIPTS program" that is really whatever PowerPC code
     * happens to live at the address. The giveaway was a move from 1A00603D,
     * which byte-reversed is 3D60001A -- `lis r11, 0x1A`. */
    if (off <= R_DSP && R_DSP < off + size) {
        if (!(g_reg[R_DMODE] & DMODE_MAN)) {
            uint32_t dsp = swap32(reg32(R_DSP));
            if (dsp)
                scsi_run(dsp);
        }
        return;
    }
    if (off <= R_DCNTL && R_DCNTL < off + size && (g_reg[R_DCNTL] & DCNTL_STD)) {
        uint32_t dsp = swap32(reg32(R_DSP));
        g_reg[R_DCNTL] &= (uint8_t)~DCNTL_STD;
        if (dsp)
            scsi_run(dsp);
    }
}
