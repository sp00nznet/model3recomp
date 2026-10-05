/* model3recomp -- Model 3 memory bus.
 *
 * Every guest load and store lands here. Two routings in this file are
 * load-bearing enough to call out, because getting either wrong hangs the
 * game rather than glitching it:
 *
 *   read  0xF0100018  ->  irq_status_read()   the frame boundary
 *   write 0xF1180010  ->  irq_ack()           the acknowledge the guest spins on
 *
 * See docs/technical/execution-model.md.
 */
#include "model3recomp/savestate.h"
#include "model3recomp/bus.h"
#include "model3recomp/real3d.h"
#include "model3recomp/platform.h"
#include "model3recomp/input.h"
#include "model3recomp/sound.h"
#define M3_NL "\n"
#include "model3recomp/irq.h"
#include "model3recomp/scsi.h"
#include "model3recomp/model3recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t  *g_ram;
uint8_t *m3_ram_base;           /* mirrors g_ram, for lift.h's fast path */
static m3_roms_t g_roms;
static uint8_t  *g_backup;          /* 128 KB battery-backed SRAM */
static uint32_t  g_crom_bank;       /* byte offset of the window into the image */
static uint32_t  g_crom_bank_reg;   /* what was written, which reads back verbatim */

/* I/O board control register at 0xF0040000. It is a latch: the boot strobes a
 * bit, reads the register back, and waits for the bit to mirror -- first for
 * it to set, then for it to clear:
 *
 *     stw r10, 0(r11)   ; r11 = 0xF0040000, r10 = 0x01000000
 *     lwz r0,  0(r11)
 *     andis. r9, r0, 0x100
 *     beq  -0xC                    ; spin until it SETS
 *     ...
 *     stw r10, 0(r11)   ; r10 = 0
 *     lwz r0,  0(r11)
 *     andis. r9, r0, 0x100
 *     bne  -0xC                    ; spin until it CLEARS
 *
 * Return a constant here -- 0 or 0xFFFFFFFF -- and one of those two loops
 * never exits. */
static uint32_t  g_io_ctrl;
static uint32_t  g_io_ready;        /* see the serial-handshake note below */

/* The 93C46 serial EEPROM: 64 words of 16 bits, bit-banged through the I/O
 * board's register 0x00 (bit 6 chip select, bit 7 clock, bit 5 data in) and
 * read back on bit 5 of register 0x04 in bank 1. Games keep their settings
 * in it -- The Lost World its country, which decides whether it plays in
 * Japanese or English.
 *
 * The protocol is the part's datasheet's. With chip select high, data in is
 * sampled on each rising clock: a start bit (the first 1), a two-bit opcode
 * and a six-bit address, then for a write sixteen data bits, most
 * significant first. 10 is READ, 01 WRITE, 11 ERASE; opcode 00 takes its
 * meaning from the top two address bits -- 11 write enable, 00 write
 * disable, 10 erase all, 01 write all. A read answers a 0 as the address
 * completes and then the word, a bit a clock, and runs on into the next
 * word for as long as the clock does. Writes are refused until enabled.
 * Dropping chip select ends a command; raised again after a write, data out
 * reads busy (0) for a moment and then ready (1). */
static struct {
    uint16_t regs[64];
    unsigned clk, dout;
    unsigned nbits;                 /* bits taken since the start bit */
    uint32_t cmd;                   /* opcode, address, data, as they came */
    unsigned reading, addr, bit;    /* a READ is shifting out */
    unsigned busy;                  /* reads of status still busy */
    int      writable;
} g_ee;

static void eeprom_reset(void)
{
    memset(&g_ee, 0, sizeof g_ee);
    memset(g_ee.regs, 0xFF, sizeof g_ee.regs);
}

static void ee_program(unsigned op, unsigned a, uint16_t data)
{
    unsigned i;
    if (!g_ee.writable) return;
    if (op == 1u) g_ee.regs[a] = data;                         /* WRITE */
    else if (op == 3u) g_ee.regs[a] = 0xFFFFu;                 /* ERASE */
    else if ((a >> 4) == 2u) for (i = 0; i < 64; i++) g_ee.regs[i] = 0xFFFFu;   /* ERAL */
    else if ((a >> 4) == 1u) for (i = 0; i < 64; i++) g_ee.regs[i] = data;      /* WRAL */
    g_ee.busy = 5;
}

static void eeprom_write(unsigned cs, unsigned clk, unsigned di)
{
    unsigned rising = clk && !g_ee.clk;
    g_ee.clk = clk;
    if (!cs) {                      /* deselected: whatever was going is over */
        g_ee.nbits = 0; g_ee.cmd = 0; g_ee.reading = 0;
        return;
    }
    if (!rising) return;

    if (g_ee.reading) {
        g_ee.dout = (g_ee.regs[g_ee.addr] >> (15u - g_ee.bit)) & 1u;
        if (++g_ee.bit == 16u) { g_ee.bit = 0; g_ee.addr = (g_ee.addr + 1u) & 63u; }
        return;
    }
    if (g_ee.nbits == 0 && !di) return;     /* waiting for the start bit */
    if (g_ee.nbits++ == 0) return;          /* that was it */
    g_ee.cmd = (g_ee.cmd << 1) | (di & 1u);

    if (g_ee.nbits == 9u) {                 /* start + opcode + address */
        unsigned op = (g_ee.cmd >> 6) & 3u, a = g_ee.cmd & 63u;
        if (op == 2u) {                     /* READ */
            g_ee.reading = 1; g_ee.addr = a; g_ee.bit = 0; g_ee.dout = 0;
        } else if (op == 3u) {
            ee_program(op, a, 0);
            g_ee.nbits = 0; g_ee.cmd = 0;
        } else if (op == 0u && (a >> 4) == 3u) {
            g_ee.writable = 1; g_ee.nbits = 0; g_ee.cmd = 0;
        } else if (op == 0u && (a >> 4) == 0u) {
            g_ee.writable = 0; g_ee.nbits = 0; g_ee.cmd = 0;
        } else if (op == 0u && (a >> 4) == 2u) {
            ee_program(op, a, 0);
            g_ee.nbits = 0; g_ee.cmd = 0;
        }
        /* WRITE and WRAL go on for sixteen bits of data. */
    } else if (g_ee.nbits == 25u) {
        unsigned op = (g_ee.cmd >> 22) & 3u, a = (g_ee.cmd >> 16) & 63u;
        if (getenv("M3_EE_TRACE"))
            fprintf(stderr, "[eeprom] field %llu op %u addr %2u = %04X%s" M3_NL,
                    (unsigned long long)model3recomp_frame_count(), op, a,
                    (unsigned)(g_ee.cmd & 0xFFFFu), g_ee.writable ? "" : " (locked)");
        ee_program(op, a, (uint16_t)g_ee.cmd);
        g_ee.nbits = 0; g_ee.cmd = 0;
    }
}

static unsigned eeprom_read(void)
{
    if (g_ee.reading) return g_ee.dout;
    if (g_ee.busy) { g_ee.busy--; return 0; }
    return 1;
}

uint16_t *bus_eeprom(void) { return g_ee.regs; }
static uint32_t  g_pci_addr;        /* PCI CONFIG_ADDRESS latch            */
static uint32_t  g_r3d_ping;        /* Real3D status, bit 1 is the ping    */

/* Tilegen VRAM is 0xF1000000..0xF111FFFF: 1 MB of tile/name data followed by
 * 128 KB of palette. One allocation, since the guest addresses it as one. */
#define TILEGEN_VRAM_SIZE 0x120000u
static uint64_t g_dev_reads[256];

/* The light gun board, reached through a two-byte serial window.
 *
 * The guest writes a command to register 0x24 and data to 0x28, and reads
 * the answers back from 0x2C and 0x30. Command 0x00 latches which of the
 * gun's own registers to look at; command 0x87 fetches it. Register 0x34
 * reports whether a gun board is fitted at all.
 *
 * The Lost World asks the player to shoot rather than to press start, so
 * none of this is optional: without it the attract panel telling you to
 * shoot stays up forever, which is exactly where this got stuck. */
static uint32_t g_serial1, g_serial2, g_gun_reg;

/* Buttons and guns come from the record latched for this field (input.c),
 * never from the host directly: every read in a field must agree, and on a
 * netplay session both machines must see the same thing. */
static unsigned buttons_now(void) { return m3_input()->buttons; }
static uint8_t *g_vram;

/* The tilegen's own register file, 0xF1180000..0xF11800FF. Only the ack at
 * 0x10 has a side effect; the rest is state the renderer reads back -- layer
 * enables and the per-layer scroll. */
static uint32_t g_tilegen_regs[64];

const uint32_t *bus_tilegen_regs(void) { return g_tilegen_regs; }

/* The Real3D's upload FIFO at 0x94000000.
 *
 * The guest streams a thousand bytes a frame into this one address, and until
 * it was named the DMA guard threw every transfer away as out of range -- 464
 * of them in a two-thousand-field run, which is most of what the guest sends
 * the graphics processor. Kept in arrival order, because a FIFO has no
 * addresses. */
#define TEXFIFO_CAP 0x400000u
static uint8_t *g_texfifo;
static uint32_t g_vromtex[4];    /* the VROM texture port at 0x90000000 */
static size_t   g_texfifo_used;

/* Real3D culling and polygon RAM. Not yet consumed by a renderer, but the
 * guest writes them constantly and reads back what it wrote. */
#define CULL_LO_SIZE 0x400000u
#define CULL_HI_SIZE 0x100000u
#define POLY_SIZE    0x400000u
static uint8_t *g_cull_lo, *g_cull_hi, *g_poly;

void bus_init(const m3_roms_t *roms)
{
    g_roms = *roms;
    g_ram     = calloc(1, M3_RAM_SIZE);
    g_backup  = calloc(1, 0x20000);
    /* Battery-backed settings. A machine that has been switched on before has
     * these; a freshly allocated block of zeros is a machine that never has,
     * and a game that checks them for its own signature behaves accordingly.
     * M3_BACKUP loads a saved image, which is what the battery is for. */
    if (g_backup) {
        const char *bk = getenv("M3_BACKUP");
        if (bk) {
            FILE *bf = fopen(bk, "rb");
            if (bf) {
                size_t got = fread(g_backup, 1, 0x20000, bf);
                fclose(bf);
                fprintf(stderr, "[model3recomp] backup RAM: %u bytes from %s\n",
                        (unsigned)got, bk);
            }
        }
    }
    g_vram    = calloc(1, TILEGEN_VRAM_SIZE);
    g_texfifo = calloc(1, TEXFIFO_CAP);
    g_cull_lo = calloc(1, CULL_LO_SIZE);
    g_cull_hi = calloc(1, CULL_HI_SIZE);
    g_poly    = calloc(1, POLY_SIZE);
    if (!g_ram || !g_backup || !g_vram || !g_cull_lo || !g_cull_hi || !g_poly) {
        fprintf(stderr, "[model3recomp] out of memory allocating the board\n");
        abort();
    }
    m3_ram_base = g_ram;
    g_crom_bank = 0;
    g_crom_bank_reg = 0;
    g_io_ctrl = 0;
    g_io_ready = 0;
    eeprom_reset();
    g_pci_addr = 0;
    g_r3d_ping = 0;
    scsi_init();
}

void bus_shutdown(void)
{
    free(g_ram); free(g_backup); free(g_vram); free(g_texfifo);
    free(g_cull_lo); free(g_cull_hi); free(g_poly);
    g_ram = g_backup = g_vram = g_cull_lo = g_cull_hi = g_poly = NULL;
    g_texfifo = NULL; g_texfifo_used = 0;
    m3_ram_base = NULL;
}

uint8_t *bus_backup(size_t *size)
{
    if (size) *size = g_backup ? 0x20000u : 0u;
    return g_backup;
}

uint8_t *bus_ram(void) { return g_ram; }

uint8_t *bus_texfifo(size_t *used, size_t *capacity)
{
    if (used) *used = g_texfifo_used;
    if (capacity) *capacity = TEXFIFO_CAP;
    return g_texfifo;
}

void bus_texfifo_reset(void) { g_texfifo_used = 0; }

uint8_t *bus_vram(size_t *size)
{
    if (size) *size = TILEGEN_VRAM_SIZE;
    return g_vram;
}

uint8_t *bus_cull_lo(size_t *size)
{
    if (size) *size = CULL_LO_SIZE;
    return g_cull_lo;
}

uint8_t *bus_cull_hi(size_t *size)
{
    if (size) *size = CULL_HI_SIZE;
    return g_cull_hi;
}

uint8_t *bus_poly(size_t *size)
{
    if (size) *size = POLY_SIZE;
    return g_poly;
}

const uint8_t *bus_vrom(size_t *size)
{
    if (size) *size = g_roms.vrom_size;
    return g_roms.vrom;
}

/* ---- big-endian accessors over a host buffer --------------------------- */
static inline uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static inline uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint32_t)p[0] << 8) | p[1]);
}
static inline void wbe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static inline void wbe16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

/* ---- region resolution --------------------------------------------------
 * Returns a host pointer for the plain-memory regions, or NULL when the
 * address belongs to a device and has to go through the switch below.
 */
static uint8_t *direct(uint32_t a, uint32_t size, int write)
{
    if (a < M3_RAM_SIZE)
        return g_ram + a;

    if (a >= M3_R3D_CULL_LO && a < M3_R3D_CULL_LO + CULL_LO_SIZE)
        return g_cull_lo + (a - M3_R3D_CULL_LO);
    if (a >= M3_R3D_CULL_HI && a < M3_R3D_CULL_HI + CULL_HI_SIZE)
        return g_cull_hi + (a - M3_R3D_CULL_HI);
    if (a >= M3_R3D_POLY && a < M3_R3D_POLY + POLY_SIZE)
        return g_poly + (a - M3_R3D_POLY);

    if (a >= M3_TILEGEN_VRAM && a < M3_TILEGEN_VRAM + TILEGEN_VRAM_SIZE)
        return g_vram + (a - M3_TILEGEN_VRAM);

    if (a >= M3_BACKUP_BASE && a < M3_BACKUP_BASE + 0x20000)
        return g_backup + (a - M3_BACKUP_BASE);

    if (!write) {
        /* Fixed program CROM occupies the top of the map. A ROM smaller than
         * the 8 MB window sits at the END of it, which is why the reset
         * vector at 0xFFF00100 lands where it does. */
        if (a >= M3_CROM_FIXED && g_roms.crom) {
            uint32_t top = 0xFFFFFFFFu - (uint32_t)g_roms.crom_size + 1u;
            if (a >= top)
                return (uint8_t *)g_roms.crom + (a - top);
            /* Below the program and above 0xFF800000 is the data ROM, and it
             * is not banked: the whole span is the front of the image, which
             * is where a game's tables and models actually live.
             *
             * This used to read as nothing at all, and a guest asking for its
             * own data got zeros -- The Lost World reads 0xFFA18DEC, where
             * the image has an "M3"-tagged record, and found none. Checked
             * against MAME: 0xFF800000 is offset 0 of this image, 0xFFC00000
             * is 0x400000 and so on, one for one to 0xFFDFFFFF, and driving
             * the bank register through all 256 values moves none of it. */
            if (g_roms.crom_bank) {
                uint32_t off = a - M3_CROM_FIXED;
                if (off < g_roms.crom_bank_size)
                    return (uint8_t *)g_roms.crom_bank + off;
            }
            return NULL;
        }
        /* Banked CROM window. */
        if (a >= M3_CROM_BANKED && a < M3_CROM_FIXED && g_roms.crom_bank) {
#ifdef M3_LOOP_GUARD
            /* Which window offsets the guest reads, under which bank value.
             * Pinning the bank mapping down needs the guest's own access
             * pattern, not a guess at what the register bits mean. */
            {
                static int probed, on;
                static uint32_t seen[64];
                static unsigned nseen;
                if (!probed) { probed = 1; on = getenv("M3_TRACE_BANK") != NULL; }
                if (on) {
                    uint32_t key = (g_crom_bank_reg << 24) |
                                   ((a - M3_CROM_BANKED) >> 16);
                    unsigned i;
                    for (i = 0; i < nseen; i++) if (seen[i] == key) break;
                    if (i == nseen && nseen < 64) {
                        seen[nseen++] = key;
                        fprintf(stderr, "[bank] reg=%02X window+0x%06X\n",
                                g_crom_bank_reg, (a - M3_CROM_BANKED) & ~0xFFFFu);
                    }
                }
            }
#endif
            uint32_t off = (a - M3_CROM_BANKED) + g_crom_bank;
            if (off < g_roms.crom_bank_size)
                return (uint8_t *)g_roms.crom_bank + off;
            return NULL;
        }
    }
    return NULL;
}

/* ---- spin detection -----------------------------------------------------
 * A recompiled game that stops making progress is almost always polling one
 * hardware register forever. There is no PC to inspect -- lifted code is
 * native code -- so the bus counts what it is asked for and names the culprit
 * itself once the traffic is obviously pathological.
 */
/* M3_TRACE_IO=<path> records every device access in order. The interpreter in
 * tools/ppc_interp.py writes the same format, so the two can be diffed: the
 * first line that differs is where the lifted code and the interpreter stop
 * agreeing. That is the whole point of keeping an interpreter around. */
static FILE *g_io_log;
static long  g_io_budget;

static void io_log_init(void)
{
    static int done;
    const char *path, *lim;
    if (done) return;
    done = 1;
    path = getenv("M3_TRACE_IO");
    if (!path) return;
    g_io_log = fopen(path, "w");
    lim = getenv("M3_TRACE_IO_MAX");
    g_io_budget = lim ? strtol(lim, NULL, 0) : 20000;
}

static void io_log(char rw, uint32_t a, unsigned size, uint32_t v)
{
    io_log_init();
    if (!g_io_log || g_io_budget <= 0) return;
    g_io_budget--;
    fprintf(g_io_log, "%c %08X %u %08X\n", rw, a, size, v);
    if (g_io_budget == 0) { fclose(g_io_log); g_io_log = NULL; }
}

enum { SPIN_SLOTS = 24, SPIN_REPORT = 20000000u };
static struct { uint32_t addr, n; } g_spin[SPIN_SLOTS];
static uint32_t g_dev_accesses;

static void spin_note(uint32_t a, int write)
{
    unsigned i, weakest = 0;
    m3_work++;                     /* device traffic counts as progress too */
    uint32_t key = (a & ~3u) | (write ? 1u : 0u);
    for (i = 0; i < SPIN_SLOTS; i++) {
        if (g_spin[i].addr == key && g_spin[i].n) {
            g_spin[i].n++;
            goto counted;
        }
        if (g_spin[i].n < g_spin[weakest].n)
            weakest = i;
    }
    /* Table full: evict the least-used slot rather than keeping whichever
     * addresses happened to come first. A boot writes dozens of registers
     * once each, and those would otherwise crowd out the one address the
     * guest is spinning on -- which is the only one worth seeing. */
    g_spin[weakest].addr = key;
    g_spin[weakest].n = 1;
counted:
    if (++g_dev_accesses < SPIN_REPORT)
        return;
    g_dev_accesses = 0;
    fprintf(stderr, "[model3recomp] %u device accesses with no field "
                    "advance -- the guest is probably spinning:\n", SPIN_REPORT);
    for (i = 0; i < SPIN_SLOTS && g_spin[i].n; i++)
        fprintf(stderr, "    %c %08X x%u\n",
                (g_spin[i].addr & 1) ? 'W' : 'R',
                g_spin[i].addr & ~3u, g_spin[i].n);
    for (i = 0; i < SPIN_SLOTS; i++) { g_spin[i].addr = 0; g_spin[i].n = 0; }
}

/* ---- devices ------------------------------------------------------------
 * Registers are defined as 32-bit words, but the guest reaches several of
 * them with lbz/stb -- the CROM bank register at 0xF0100008 is a byte
 * register the boot code writes and then reads back to confirm. So every
 * device access is funnelled through an aligned word accessor and the byte
 * lane is extracted here, once, rather than each device guessing.
 *
 * Getting this wrong does not glitch anything: the boot writes a bank, reads
 * back a value that never matches, and spins on 0xF0100008 forever.
 */
/* ---- PCI configuration space -------------------------------------------
 *
 * The game will not finish booting until it finds two devices by ID. It walks
 * them through the MPC105's CONFIG_ADDRESS/CONFIG_DATA port pair:
 *
 *     addr = 0x80000000 | (device << 11) | offset
 *     stwbrx addr -> 0xF0800CF8 ; lwbrx  data <- 0xF0C00CFC
 *
 * and compares dword 0 against a literal. Device 13 must answer as the Real3D
 * and device 14 as the 53C810; miss either and the guest spins on a readiness
 * flag that the matching arm is what sets. Both already live at fixed
 * addresses in this board's memory map, so the BAR writes that follow are
 * dropped -- there is nothing here for the guest to relocate.
 */
#define PCI_ID_REAL3D  0x16C311DBu   /* vendor 0x11DB Sega, device 0x16C3 */
#define PCI_ID_53C810  0x00011000u   /* vendor 0x1000 LSI,  device 0x0001 */

static uint32_t bswap32(uint32_t v)
{
    return (v >> 24) | ((v >> 8) & 0xFF00u) |
           ((v << 8) & 0xFF0000u) | (v << 24);
}

static uint32_t pci_config(uint32_t addr)
{
    uint32_t dev;

    if (!(addr & 0x80000000u))
        return 0;
    if ((addr & 0xFCu) != 0)
        return 0;                    /* every other register reads zero */

    dev = (addr >> 11) & 0x1Fu;
    if (dev == 13) return PCI_ID_REAL3D;
    if (dev == 14) return PCI_ID_53C810;
    return 0;                        /* absent */
}

static uint32_t dev_read_word(uint32_t w, int side_effects);

static uint32_t dev_read_word(uint32_t w, int side_effects)
{
    g_dev_reads[(w >> 16) & 0xFFu]++;
    /* The VROM texture port reads back what was written, so a byte store
     * assembles instead of clearing the other three. */
    if ((w & 0xFF000000u) == 0x90000000u)
        return g_vromtex[(w >> 2) & 3u];
    /* System controller / interrupt controller. */
    if ((w & 0xFFFFFFC0u) == M3_SYSCTL_BASE) {
        switch (w & 0x3C) {
        case 0x14: return irq_enable_read();
        /* THE frame boundary. Answering this read is what makes a field
         * elapse; see docs/technical/execution-model.md. Skipped when this
         * is the read half of a read-modify-write on a byte store. */
        case 0x18: return side_effects ? irq_status_read() : irq_pending();
        /* Byte register in the most-significant lane of its word. */
        /* Reads back exactly what was written. The boot writes a bank and
         * spins here until it matches, so this cannot be derived from the
         * offset below -- the moment the mapping is anything but a shift,
         * the two stop agreeing and the guest never leaves the loop. */
        case 0x08: return g_crom_bank_reg << 24;
        default:   return 0;
        }
    }

    /* Real3D status block. The boot copies nine dwords of this into RAM with
     * stwbrx -- the chip is on the PCI side, so the cached copy is
     * byte-reversed -- and then waits for bit 0x02000000 of that copy to
     * flip. Byte-reversed, that is bit 1 of the register here.
     *
     * ponytail: the ping is toggled per read rather than driven by the
     * renderer, which is all the wait needs. Upgrade path: flip it when the
     * Real3D actually retires a frame, once there is a renderer to retire
     * one. */
    if ((w & 0xFF000000u) == M3_R3D_STATUS) {
        if (w != M3_R3D_STATUS)
            return 0;
        /* One flip per field, because a guest measures time with this.
         *
         * Toggling it per read satisfies a guest that only waits for the bit
         * to change, and that is what this did. But The Lost World times the
         * interval between two flips against the time base and divides to get
         * its decrementer period: flipping on demand makes that interval
         * nothing, and it computed a period of -26 where the hardware gives
         * 26,926. Its decrementer then never fired, its main loop waited
         * forever on a counter only that interrupt writes, and it never ran a
         * single one of its own state handlers.
         *
         * So the ping is what it is on hardware -- a frame flag -- and the
         * wait for it ends when a field does. */
        return (model3recomp_frame_count() & 1u) ? 0x00000002u : 0u;
    }

    /* PCI CONFIG_DATA. The guest reads it with lwbrx, so hand it back
     * byte-reversed. */
    if (w == M3_PCI_CONFIG_DATA)
        return bswap32(pci_config(g_pci_addr));

    /* Sound board handshake. The guest reads this with lwbrx and tests two
     * bits of the byte-reversed word: bit 0 is "ready for a command" and bit
     * 1 is "a reply byte is waiting". Byte-reversed those are 0x01000000 and
     * 0x02000000 here.
     *
     * The command loop at 0x00118854 spins until ready, and it runs inside
     * the VBlank handler -- so a board that is never ready does not merely
     * lose sound, it wedges the machine, because irq_tick() will not re-enter
     * while a dispatch is in progress and no further field ever falls due.
     *
     * ponytail: with no 68000 to be busy, always ready and never answering is
     * the honest model -- the guest's commands go nowhere and it never waits
     * on a reply. Upgrade path: a real 68000 and SCSPs, at which point both
     * bits come from the board instead. */
    /* The sound board's UART: 0x00 data, 0x04 status (sound.c). The
     * guest reads these a byte at a time, from the top of the word. */
    if ((w & 0xFFFF0000u) == M3_SOUND_BASE)
        return (uint32_t)(side_effects ? sound_uart_read((w >> 2) & 1u)
                                       : sound_uart_read(1)) << 24;

    if ((w & 0xFFFFFFC0u) == M3_INPUTS_BASE) {
        if (getenv("M3_IO_TRACE") &&
            model3recomp_frame_count() >= 3000u) {
            static unsigned n;
            if (n++ < 6000)
                fprintf(stderr, "[io] read %08X (reg %02X) ctrl=%08X" M3_NL,
                        w, w & 0x3Cu, g_io_ctrl);
        }
        if ((w & 0x3Cu) == 0x00u)
            return g_io_ctrl;        /* the strobe latch reads back */
        if ((w & 0x3Cu) == 0x2Cu) return g_serial1 << 24;
        if ((w & 0x3Cu) == 0x30u) return g_serial2 << 24;
        if ((w & 0x3Cu) == 0x34u) return 0x0Cu << 24;  /* a gun board is fitted */
        if ((w & 0x3Cu) == 0x08u || (w & 0x3Cu) == 0x0Cu) {
            /* Game inputs, active low in the top byte. On a gun cabinet
             * bit 0 is that player's trigger: 0x08 is player one and 0x0C
             * player two. The game asks you to shoot rather than to press
             * start, so this is what actually begins a credit. */
            unsigned btn = buttons_now();
            uint32_t v = 0xFFu;
            unsigned want = ((w & 0x3Cu) == 0x08u) ? M3_BTN_TRIG1 : M3_BTN_TRIG2;
            if (btn & want) v &= ~0x01u;
            return (v << 24) | 0x00FFFFFFu;
        }
        if ((w & 0x3Cu) == 0x3Cu) {
            /* The ADC. The game reads this more than any other register in
             * the block. M3_ADC picks what it reads back while what the
             * guest wants from it is still being worked out; all ones is a
             * stuck-high converter, which is what it used to get. */
            static int probed;
            static uint32_t adc = 0xFFFFFFFFu;
            if (!probed) {
                const char *e = getenv("M3_ADC");
                probed = 1;
                if (e) adc = (uint32_t)strtoul(e, NULL, 0);
            }
            return adc;
        }
        if ((w & 0x3Cu) == 0x04u) {
            /* Register 0x04 carries the cabinet buttons, active low, in the
             * top byte -- the board is big-endian and the guest reads this
             * a byte at a time.
             *
             *   bit 0 coin 1   bit 2 test      bit 4 start 1
             *   bit 1 coin 2   bit 3 service   bit 5 see below
             *
             * Bit 5 is left alone. The boot's EEPROM handshake watches it
             * and waits for it to change, so driving it from a button that
             * nobody is pressing hangs the machine on the Sega logo before
             * it ever reaches attract mode. That is what happened the first
             * time this was wired up. The toggle below is the old
             * behaviour, kept underneath the buttons rather than replaced
             * by them. */
            /* Only bank 0 carries the buttons. Bank 1 is the EEPROM and
             * the service straps, and laying button bits over it feeds the
             * guest nonsense whenever a key is held. */
            /* Bit 5 is start 2 in bank 0 and the EEPROM's data line in
             * bank 1 (MAME: port IN1 bit 5). The toggle belongs to bank 1
             * only: laid over bank 0 it made start 2 flicker on every
             * read, and a second player could never join. */
            int bank1 = (int)((g_io_ctrl >> 24) & 1u);
            unsigned btn = (getenv("M3_NO_BUTTONS") || bank1) ? 0u : buttons_now();
            uint32_t v = 0xFFFFFFFFu;
            if (bank1 && !eeprom_read())
                v &= ~0x20000000u;
            if (btn & M3_BTN_COIN1)   v &= ~0x01000000u;
            if (btn & M3_BTN_COIN2)   v &= ~0x02000000u;
            if (btn & M3_BTN_TEST)    v &= ~0x04000000u;
            if (btn & M3_BTN_SERVICE) v &= ~0x08000000u;
            if (btn & M3_BTN_START1)  v &= ~0x10000000u;
            if (btn & M3_BTN_START2)  v &= ~0x20000000u;
            return v;
        }
        if (0) {

            /* ponytail: the I/O board's serial ready line is toggled rather
             * than driven by a real protocol. The boot bit-bangs a byte to
             * 0xF0040000 and then waits on bit 0x20000000 here -- first for it
             * to clear, then for it to set:
             *
             *     li r4, 0x51 ; bl <bit-bang>
             *     lwz r0, 0(r30)        ; r30 = 0xF0040004
             *     andis. r9, r0, 0x2000
             *     bne -0x14             ; wait for CLEAR
             *     ... then the same loop waiting for SET
             *
             * A constant hangs one loop or the other. Toggling satisfies both
             * and gets the boot past I/O init; the data the game reads back is
             * not meaningful. Upgrade path: model the 315-5649 I/O board's
             * serial protocol properly, which is also what buttons and coins
             * need. */
            g_io_ready ^= 0x20000000u;
            return 0xFFFFFFFFu ^ g_io_ready;
        }
        /* Inputs are active low, so all ones is "nothing pressed". A game
         * also reads cabinet straps and DIP switches through here, and for
         * those all ones is a value like any other -- M3_INPUTS overrides it
         * so what the guest does with a different one can be asked. */
        {
            static int probed;
            static uint32_t forced;
            static int have;
            if (!probed) {
                const char *e = getenv("M3_INPUTS");
                probed = 1;
                if (e) { forced = (uint32_t)strtoul(e, NULL, 0); have = 1; }
            }
            if (have)
                return forced;
        }
        return 0xFFFFFFFFu;          /* inputs are active low: nothing pressed */
    }

    return 0;
}

/* The Real3D "draw this" trigger.
 *
 * The scene in culling and polygon RAM is written by the guest over many
 * thousands of instructions, and the runtime's field boundary is synthetic
 * -- it comes off m3_work, not off a video clock -- so rendering on the
 * field boundary means rendering whatever happens to be in the buffers at
 * that moment, which can be half of the frame the guest is building and half
 * of the one before it. Counting the trigger lets the renderer tell a
 * finished scene from a torn one.
 */
static uint64_t g_r3d_triggers;
static uint64_t g_dropped_writes[256];

uint64_t bus_r3d_trigger_count(void) { return g_r3d_triggers; }

static void dev_write_word(uint32_t w, uint32_t v)
{
    if ((w & 0xFFFFFFC0u) == M3_SYSCTL_BASE) {
        switch (w & 0x3C) {
        case 0x14: irq_enable_write(v); return;
        case 0x08: {
            /* CROM bank select. The banked window is 8 MB of a 32 MB image,
             * so only a couple of bits of this can be the bank; the rest is
             * something else. M3_CROM_BANK picks the reading while it is
             * being pinned down against what the guest then finds there. */
            static int probed;
            static unsigned mode;
            uint32_t b = (v >> 24) & 0xFFu;
            g_crom_bank_reg = b;
            if (getenv("M3_BANK_TRACE")) {
                static uint8_t seen[256];
                if (!seen[b]) { seen[b] = 1;
                    fprintf(stderr, "[bank] guest wrote %02X" M3_NL, b); }
            }
            if (!probed) {
                const char *e = getenv("M3_CROM_BANK");
                probed = 1;
                mode = e ? (unsigned)strtoul(e, NULL, 0) : 0u;
            }
            switch (mode) {
            case 1:  g_crom_bank = (b & 3u) * 0x800000u; break;
            case 2:  g_crom_bank = ((~b) & 3u) * 0x800000u; break;
            case 3:  g_crom_bank = (b & 7u) * 0x400000u; break;
            case 4:  g_crom_bank = ((~b) & 7u) * 0x400000u; break;
            case 5:  g_crom_bank = ((b >> 4) & 3u) * 0x800000u; break;
            case 6:  g_crom_bank = (b & 7u) * 0x800000u; break;
            case 7:  g_crom_bank = ((~b) & 7u) * 0x800000u; break;
            case 8:  g_crom_bank = ((b >> 1) & 7u) * 0x800000u; break;
            case 9:  g_crom_bank = ((b >> 1) & 3u) * 0x800000u; break;
            case 10: g_crom_bank = (((~b) >> 1) & 7u) * 0x800000u; break;
            case 11: {   /* sweep: bank = ((b >> S) ^ X) & 7 */
                const char *es = getenv("M3_BANK_S"), *ex = getenv("M3_BANK_X");
                unsigned sh = es ? (unsigned)strtoul(es, NULL, 0) : 0u;
                unsigned xr = ex ? (unsigned)strtoul(ex, NULL, 0) : 0u;
                g_crom_bank = (((b >> sh) ^ xr) & 7u) * 0x800000u;
                break;
            }
            /* What this has always done, kept as the default until the
             * right reading is known: the value shifted into a byte offset.
             * For most of the values The Lost World writes -- 0xF7 above all
             * -- that lands far outside the image, every read through the
             * window returns zero, and the guest quietly gets nothing. */
            /* Active-low, four bits, eight megabytes a bank. The banked
             * image is the 64 MB that follows the fixed program CROM, so
             * the index is used directly against it. */
            case 12: g_crom_bank = ((~b) & 0xFu) * 0x800000u; break;
            case 13: g_crom_bank = ((~b) & 0x7u) * 0x800000u; break;
            /* The bank select is active low: the index is the written
             * byte inverted, 8 MB a bank (MAME's model3_ctrl_w takes four
             * bits; three are kept here because this game's banked image
             * is 64 MB).
             *
             * This used to shift the byte into a byte offset, and the
             * comment here used to note that for the values The Lost World
             * writes -- 0xF7 above all -- that lands outside the image and
             * the guest quietly gets nothing. That was correct and it sat
             * there, because a game reading zeros where its data should be
             * does not crash. It draws less.
             *
             * It drew a great deal less. With the window returning zeros
             * the guest streamed 321 KB to the Real3D in a whole run; with
             * it working it streams 3.4 MB. That is the difference between
             * a scene with no textures in it and one with. */
            case 99: g_crom_bank = b << 20; break;   /* the old, broken one */
            default: g_crom_bank = ((~b) & 0x7u) * 0x800000u; break;
            }
            return;
        }
        default: return;
        }
    }

    if ((w & 0xFFFFFFC0u) == M3_INPUTS_BASE) {
        if ((w & 0x3Cu) == 0x00u) {
            unsigned d = (v >> 24) & 0xFFu;
            g_io_ctrl = v;
            eeprom_write((d >> 6) & 1u, (d >> 7) & 1u, (d >> 5) & 1u);
            return;
        }
        if ((w & 0x3Cu) == 0x28u) { g_serial2 = (v >> 24) & 0xFFu; return; }
        if ((w & 0x3Cu) == 0x24u) {
            unsigned cmd = (v >> 24) & 0xFFu;
            if (getenv("M3_GUN_TRACE")) {
                static unsigned n;
                if (model3recomp_frame_count() >= 3000u && n++ < 2000)
                    fprintf(stderr, "[gun] cmd %02X reg %u fifo2 %02X" M3_NL,
                            cmd, g_gun_reg, g_serial2);
            }
            /* Command 0x00 latches which gun register to look at, from
             * whatever was written to 0x28; command 0x87 fetches it. In
             * the steady state this game sends exactly those two, a
             * thousand of each. It sends 0x01/0x07/0x81/0x83 as well, but
             * only during boot -- reading the boot phase as the protocol
             * and rebuilding around it was a wrong turn. */
            if (cmd == 0x00u) {
                g_gun_reg = g_serial2;
            } else if (cmd == 0x87u) {
                const m3_input_t *in = m3_input();
                unsigned btn = in->buttons;
                g_serial1 = 0;
                switch (g_gun_reg) {
                case 0: g_serial2 = in->gun_y[0] & 0xFFu; break;
                case 1: g_serial2 = (in->gun_y[0] >> 8) & 3u; break;
                case 2: g_serial2 = in->gun_x[0] & 0xFFu; break;
                case 3: g_serial2 = (in->gun_x[0] >> 8) & 3u; break;
                case 4: g_serial2 = in->gun_y[1] & 0xFFu; break;
                case 5: g_serial2 = (in->gun_y[1] >> 8) & 3u; break;
                case 6: g_serial2 = in->gun_x[1] & 0xFFu; break;
                case 7: g_serial2 = (in->gun_x[1] >> 8) & 3u; break;
                /* Offscreen, one bit per player: pointing away from the
                 * screen is how a player reloads. */
                case 8: g_serial2 = (btn & M3_BTN_OFFSCR2) ? 2u : 0u;
                        if (btn & M3_BTN_OFFSCR1) g_serial2 |= 1u;
                        break;
                default: g_serial2 = 0; break;
                }
                if (getenv("M3_GUN_TRACE2")) {
                    static unsigned m;
                    if (m++ < 20)
                        fprintf(stderr, "[gun] read reg %u -> %02X" M3_NL,
                                g_gun_reg, g_serial2);
                }
            }
            return;
        }
        return;
    }

    if ((w & 0xFF000000u) == M3_R3D_TRIGGER) {
        g_r3d_triggers++;
        if (getenv("M3_R3D_TRACE")) {
            static unsigned n;
            if (n++ < 400)
                fprintf(stderr, "[r3d] trigger %08X = %08X\n", w, v);
        }
        return;
    }

    /* PCI CONFIG_ADDRESS. Written with stwbrx, so un-reverse it. */
    if (w == M3_PCI_CONFIG_ADDR) {
        g_pci_addr = bswap32(v);
        return;
    }

    /* Interrupt acknowledge. The guest writes the bit here and then spins on
     * 0xF0100018 until it clears -- miss this and the game never leaves its
     * interrupt handler. */
    if ((w & 0xFFFF0000u) == (M3_TILEGEN_REGS & 0xFFFF0000u)) {
        g_tilegen_regs[(w >> 2) & 0x3Fu] = v;
        if ((w & 0xFC) == 0x10) irq_ack(v);
        return;
    }

    /* Anything that reaches here is a write the board does not model, and
     * it is thrown away. Count it by page, because a region the guest
     * writes megabytes to and we ignore looks exactly like a region the
     * guest never writes. */
    /* 0x90000000..0x9000000B: the Real3D VROM texture port. The game
     * names a texture already in VROM instead of pushing its pixels
     * through the FIFO, which is how a stage's scenery is loaded. */
    if ((w & 0xFF000000u) == 0x90000000u) {
        unsigned r = (w >> 2) & 3u;
        g_vromtex[r] = v;
        if (getenv("M3_VROMTEX_TRACE"))
            fprintf(stderr, "[vromtex] reg %u = %08X  (%08X %08X %08X)"
                    M3_NL, r * 4u, v,
                    g_vromtex[0], g_vromtex[1], g_vromtex[2]);
        if (r == 2)
            real3d_vrom_texture(g_vromtex[0], g_vromtex[1], g_vromtex[2]);
        return;
    }
    if ((w & 0xFF000000u) == 0x9C000000u) {
        if (getenv("M3_TEXPORT_TRACE"))
            fprintf(stderr, "[texport] %08X = %08X" M3_NL, w, v);
        g_dropped_writes[(w >> 24) & 0xFFu]++;
        return;
    }
    g_dropped_writes[(w >> 24) & 0xFFu]++;
    if (getenv("M3_DROP_TRACE")) {
        static unsigned n;
        if (n++ < 30)
            fprintf(stderr, "[drop] %08X = %08X" M3_NL, w, v);
    }
}

void bus_report_device_reads(void)
{
    unsigned i;
    fprintf(stderr, "[model3recomp] device reads by address, F0xx pages:" M3_NL);
    for (i = 0; i < 256; i++)
        if (g_dev_reads[i])
            fprintf(stderr, "    F0%02X0000: %llu" M3_NL, i,
                    (unsigned long long)g_dev_reads[i]);
    fflush(stderr);
}

void bus_report_dropped_writes(void)
{
    unsigned i, any = 0;
    for (i = 0; i < 256; i++) {
        if (!g_dropped_writes[i]) continue;
        if (!any++) fprintf(stderr, "[model3recomp] writes the board "
                                    "discarded, by page:\n");
        fprintf(stderr, "    %02X000000: %llu\n", i,
                (unsigned long long)g_dropped_writes[i]);
    }
    if (!any)
        fprintf(stderr, "[model3recomp] no writes were discarded\n");
    fflush(stderr);
}

/* Byte-lane extraction, big-endian: the byte at address a sits in lane
 * (a & 3) counted from the most-significant end of its word. */
static uint32_t dev_read(uint32_t a, unsigned size)
{
    uint32_t word, mask;
    unsigned sh;
    spin_note(a, 0);

    /* The other place the runtime gets control. func_table_call() alone is
     * not enough: the guest spends long stretches polling a device without
     * dispatching through a pointer, and during those the field never
     * advances and no interrupt is ever delivered -- so it polls forever.
     *
     * Delivering an interrupt from inside a load is what the hardware does
     * anyway; the guest's handler saves and restores every register, and our
     * lifted code keeps its temporaries in C locals the guest cannot reach.
     * irq_tick() is re-entrancy guarded. */
    irq_tick();
    if ((a & 0xFF000000u) == M3_SCSI_BASE) {
        uint32_t sv = scsi_read(a, size);
        io_log('R', a, size, sv);
        return sv;
    }
    if (size == 4) {
        word = dev_read_word(a & ~3u, 1);
        io_log('R', a, size, word);
        return word;
    }
    word = dev_read_word(a & ~3u, 1);
    sh   = 8u * (4u - size - (a & 3u));
    mask = (1u << (8u * size)) - 1u;
    word = (word >> sh) & mask;
    io_log('R', a, size, word);
    return word;
}

#ifdef M3_LOOP_GUARD
/* Which guest function wrote a device register. The RAM watch in lift.h
 * cannot see these: a device write leaves the inline path and comes here. */
extern uint32_t m3_fn_now;
extern uint32_t m3_fn_prev;
static void dev_watch(uint32_t a, uint32_t v, unsigned size)
{
    static int probed;
    static uint32_t want;
    if (!probed) {
        const char *e = getenv("M3_WATCH_DEV");
        probed = 1;
        want = e ? (uint32_t)strtoul(e, NULL, 0) : 0u;
    }
    if (want && (a & 0xFFFFF000u) == (want & 0xFFFFF000u))
        fprintf(stderr, "[model3recomp] dev W %08X = %08X (%u) from guest "
                        "function %08X (called from %08X)\n", a, v, size, m3_fn_now, m3_fn_prev);
}
#define DEV_WATCH(a, v, n) dev_watch((a), (v), (n))
#else
#define DEV_WATCH(a, v, n) ((void)0)
#endif

static void dev_write(uint32_t a, uint32_t v, unsigned size)
{
    DEV_WATCH(a, v, size);

    if ((a & 0xFFFF0000u) == M3_SOUND_BASE) {
        io_log('W', a, size, v);
        sound_uart_write((a >> 2) & 1u, (uint8_t)(v >> (8u * (size - 1u))));
        return;
    }

    /* The upload FIFO. One port, so the address does not advance and what
     * matters is the order the bytes arrived in. */
    if ((a & 0xFF000000u) == M3_R3D_TEXFIFO) {
        unsigned i;
        for (i = 0; i < size; i++) {
            if (g_texfifo_used >= TEXFIFO_CAP)
                break;
            g_texfifo[g_texfifo_used++] =
                (uint8_t)(v >> (8u * (size - 1u - i)));
        }
        return;
    }
    uint32_t w = a & ~3u, cur, mask;
    unsigned sh;
    spin_note(a, 1);
    io_log('W', a, size, v);
    /* One 16 MB page, not two. The mask used to be 0xFE000000, which put
     * 0xC0000000 in the SCSI window as well -- and the game uploads 40 KB to
     * a device at 0xC0020000. Every one of those 10272 words landed on the
     * 53C810's 64-byte register file, DSP included, and each DSP write
     * launched a "SCRIPTS program" made of whatever the upload happened to
     * contain. One of them moved 262232 bytes from 0x68 to 0x00000000 and
     * took the low quarter-megabyte of RAM with it.
     *
     * The game says where the chip is: it writes DSP at 0xC100002C, and it
     * touches 0x0C, 0x14, 0x2C, 0x38 and 0x39 there and nowhere else. */
    if ((a & 0xFF000000u) == M3_SCSI_BASE) { scsi_write(a, v, size); return; }
    if (size == 4) { dev_write_word(w, v); return; }
    /* Read-modify-write without side effects: a byte store must not be able
     * to advance a field by accidentally reading the status register. */
    cur  = dev_read_word(w, 0);
    sh   = 8u * (4u - size - (a & 3u));
    mask = ((1u << (8u * size)) - 1u) << sh;
    dev_write_word(w, (cur & ~mask) | ((v << sh) & mask));
}

/* ---- public accessors ---------------------------------------------------- */

/* The watch in lift.h only sees stores from lifted code. Everything that
 * reaches RAM another way -- a SCRIPTS DMA, most of all -- goes through the
 * three writers below, and a byte quietly cleared by a DMA looks exactly
 * like a byte nothing ever wrote. */
#ifdef M3_WATCH
void m3_watch_hit(uint32_t addr, uint32_t val, unsigned size);
extern uint32_t m3_watch_addr;
#define BUS_WATCH(a, v, n)                                                  do { if (m3_watch_addr - (uint32_t)(a) < (uint32_t)(n))                          m3_watch_hit((uint32_t)(a), (uint32_t)(v), (n)); } while (0)
#else
#define BUS_WATCH(a, v, n) ((void)0)
#endif

uint8_t bus_read8(uint32_t a)
{
    uint8_t *p = direct(a, 1, 0);
    if (p) return *p;
    return (uint8_t)dev_read(a, 1);
}

uint16_t bus_read16(uint32_t a)
{
    uint8_t *p = direct(a, 2, 0);
    if (p) return be16(p);
    return (uint16_t)dev_read(a, 2);
}

uint32_t bus_read32(uint32_t a)
{
    uint8_t *p = direct(a, 4, 0);
    if (p) return be32(p);
    return dev_read(a, 4);
}

uint64_t bus_read64(uint32_t a)
{
    return ((uint64_t)bus_read32(a) << 32) | bus_read32(a + 4);
}

void bus_write8(uint32_t a, uint8_t v)
{
    BUS_WATCH(a, v, 1);
    uint8_t *p = direct(a, 1, 1);
    if (p) { *p = v; return; }
    dev_write(a, v, 1);
}

void bus_write16(uint32_t a, uint16_t v)
{
    BUS_WATCH(a, v, 2);
    uint8_t *p = direct(a, 2, 1);
    if (p) { wbe16(p, v); return; }
    dev_write(a, v, 2);
}

void bus_write32(uint32_t a, uint32_t v)
{
    BUS_WATCH(a, v, 4);
    uint8_t *p = direct(a, 4, 1);
    if (p) { wbe32(p, v); return; }
    dev_write(a, v, 4);
}

void bus_write64(uint32_t a, uint64_t v)
{
    bus_write32(a, (uint32_t)(v >> 32));
    bus_write32(a + 4, (uint32_t)v);
}

void bus_dma_copy(uint32_t dst, uint32_t src, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++)
        bus_write8(dst + i, bus_read8(src + i));
}

/* Everything the guest can see of the board, for save states. The ROMs are
 * not state; the texture FIFO is, up to what has been queued. */
void bus_state(m3_state_t *st)
{
    m3_state_io(st, g_ram, M3_RAM_SIZE);
    m3_state_io(st, g_backup, 0x20000u);
    m3_state_io(st, g_vram, TILEGEN_VRAM_SIZE);
    m3_state_io(st, g_cull_lo, CULL_LO_SIZE);
    m3_state_io(st, g_cull_hi, CULL_HI_SIZE);
    m3_state_io(st, g_poly, POLY_SIZE);
    M3_STATE_VAR(st, g_texfifo_used);
    if (g_texfifo_used <= TEXFIFO_CAP)
        m3_state_io(st, g_texfifo, g_texfifo_used);
    M3_STATE_VAR(st, g_crom_bank);
    M3_STATE_VAR(st, g_crom_bank_reg);
    M3_STATE_VAR(st, g_io_ctrl);
    M3_STATE_VAR(st, g_io_ready);
    M3_STATE_VAR(st, g_ee);
    M3_STATE_VAR(st, g_pci_addr);
    M3_STATE_VAR(st, g_r3d_ping);
    M3_STATE_VAR(st, g_serial1);
    M3_STATE_VAR(st, g_serial2);
    M3_STATE_VAR(st, g_gun_reg);
    M3_STATE_VAR(st, g_tilegen_regs);
    M3_STATE_VAR(st, g_vromtex);
}
