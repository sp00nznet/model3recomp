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
#include "model3recomp/bus.h"
#include "model3recomp/irq.h"
#include "model3recomp/scsi.h"
#include "model3recomp/model3recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t  *g_ram;
static m3_roms_t g_roms;
static uint8_t  *g_backup;          /* 128 KB battery-backed SRAM */
static uint32_t  g_crom_bank;       /* which 8 MB of CROM0..3 is at 0xFF000000 */

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

/* Tilegen VRAM is 0xF1000000..0xF111FFFF: 1 MB of tile/name data followed by
 * 128 KB of palette. One allocation, since the guest addresses it as one. */
#define TILEGEN_VRAM_SIZE 0x120000u
static uint8_t *g_vram;

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
    g_vram    = calloc(1, TILEGEN_VRAM_SIZE);
    g_cull_lo = calloc(1, CULL_LO_SIZE);
    g_cull_hi = calloc(1, CULL_HI_SIZE);
    g_poly    = calloc(1, POLY_SIZE);
    if (!g_ram || !g_backup || !g_vram || !g_cull_lo || !g_cull_hi || !g_poly) {
        fprintf(stderr, "[model3recomp] out of memory allocating the board\n");
        abort();
    }
    g_crom_bank = 0;
    g_io_ctrl = 0;
    g_io_ready = 0;
    scsi_init();
}

void bus_shutdown(void)
{
    free(g_ram); free(g_backup); free(g_vram);
    free(g_cull_lo); free(g_cull_hi); free(g_poly);
    g_ram = g_backup = g_vram = g_cull_lo = g_cull_hi = g_poly = NULL;
}

uint8_t *bus_ram(void) { return g_ram; }

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
            return NULL;
        }
        /* Banked CROM window. */
        if (a >= M3_CROM_BANKED && a < M3_CROM_FIXED && g_roms.crom_bank) {
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
static uint32_t dev_read_word(uint32_t w, int side_effects);

static uint32_t dev_read_word(uint32_t w, int side_effects)
{
    /* System controller / interrupt controller. */
    if ((w & 0xFFFFFFC0u) == M3_SYSCTL_BASE) {
        switch (w & 0x3C) {
        case 0x14: return irq_enable_read();
        /* THE frame boundary. Answering this read is what makes a field
         * elapse; see docs/technical/execution-model.md. Skipped when this
         * is the read half of a read-modify-write on a byte store. */
        case 0x18: return side_effects ? irq_status_read() : irq_pending();
        /* Byte register in the most-significant lane of its word. */
        case 0x08: return (g_crom_bank >> 20) << 24;
        default:   return 0;
        }
    }

    /* Real3D status. Bit 1 low means "not busy"; the boot code polls it. */
    /* Real3D status. Bit 1 low means "not busy"; the boot code polls it. */
    if ((w & 0xFF000000u) == M3_R3D_STATUS)
        return 0;

    if ((w & 0xFFFFFFC0u) == M3_INPUTS_BASE) {
        if ((w & 0x3Cu) == 0x00u)
            return g_io_ctrl;        /* the strobe latch reads back */
        if ((w & 0x3Cu) == 0x04u) {
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
        return 0xFFFFFFFFu;          /* inputs are active low: nothing pressed */
    }

    return 0;
}

static void dev_write_word(uint32_t w, uint32_t v)
{
    if ((w & 0xFFFFFFC0u) == M3_SYSCTL_BASE) {
        switch (w & 0x3C) {
        case 0x14: irq_enable_write(v); return;
        case 0x08: g_crom_bank = ((v >> 24) & 0xFFu) << 20; return;
        default: return;
        }
    }

    if ((w & 0xFFFFFFC0u) == M3_INPUTS_BASE) {
        if ((w & 0x3Cu) == 0x00u) g_io_ctrl = v;
        return;
    }

    /* Interrupt acknowledge. The guest writes the bit here and then spins on
     * 0xF0100018 until it clears -- miss this and the game never leaves its
     * interrupt handler. */
    if ((w & 0xFFFF0000u) == (M3_TILEGEN_REGS & 0xFFFF0000u)) {
        if ((w & 0xFC) == 0x10) irq_ack(v);
        return;
    }
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
    if ((a & 0xFE000000u) == M3_SCSI_BASE) {
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

static void dev_write(uint32_t a, uint32_t v, unsigned size)
{
    uint32_t w = a & ~3u, cur, mask;
    unsigned sh;
    spin_note(a, 1);
    io_log('W', a, size, v);
    if ((a & 0xFE000000u) == M3_SCSI_BASE) { scsi_write(a, v, size); return; }
    if (size == 4) { dev_write_word(w, v); return; }
    /* Read-modify-write without side effects: a byte store must not be able
     * to advance a field by accidentally reading the status register. */
    cur  = dev_read_word(w, 0);
    sh   = 8u * (4u - size - (a & 3u));
    mask = ((1u << (8u * size)) - 1u) << sh;
    dev_write_word(w, (cur & ~mask) | ((v << sh) & mask));
}

/* ---- public accessors ---------------------------------------------------- */
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
    uint8_t *p = direct(a, 1, 1);
    if (p) { *p = v; return; }
    dev_write(a, v, 1);
}

void bus_write16(uint32_t a, uint16_t v)
{
    uint8_t *p = direct(a, 2, 1);
    if (p) { wbe16(p, v); return; }
    dev_write(a, v, 2);
}

void bus_write32(uint32_t a, uint32_t v)
{
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
