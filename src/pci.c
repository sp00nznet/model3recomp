/* model3recomp -- PCI configuration space.
 *
 * The board is a PowerPC behind an MPC105 host bridge with devices on PCI, and
 * the game enumerates them the ordinary way before it configures anything. Its
 * accessor sits at RAM 0x00118940:
 *
 *     rlwinm r3, r3, 11, ...        ; bus/device/function/register
 *     oris   r4, r4, 0x8000         ; the CONFIG_ADDRESS enable bit
 *     or     r3, r3, r4
 *     lis    r0, 0xF080 ; ori 0xCF8
 *     stwbrx r3, r0                 ; CONFIG_ADDRESS
 *     lis    r0, 0xF0C0 ; ori 0xCFC
 *     lwbrx  r3, r0                 ; CONFIG_DATA
 *
 * The 0xCF8/0xCFC offsets are the standard config ports, and the stwbrx/lwbrx
 * say what the SCSI controller already told us: the PCI side of this board is
 * little-endian, so every value is byte-reversed relative to the CPU.
 *
 * A config read that answers zero means "no device" as surely as one that
 * answers 0xFFFFFFFF, and a game that finds no graphics hardware does not set
 * any up. That is worth being precise about rather than guessing IDs, so this
 * logs what is asked for under M3_TRACE_PCI before it answers.
 */
#include "model3recomp/pci.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_config_addr;

/* The board's PCI devices, as the game asks for them. It probes exactly one:
 * bus 0, device 14, register 0 -- the vendor/device ID of the 53C810 SCSI
 * controller, which is how it decides the DMA engine is there to be used.
 * Answering "no device" is what a bare bus does, and the game believes it.
 *
 * Config space is kept per device and is writable, because enumeration is not
 * just reading: the game assigns base addresses and enables bus mastering, and
 * expects to read back what it wrote. */
#define PCI_DEV_SCSI 14
/* The game writes device 11's command register and a vendor register without
 * probing for it first, which is what software does when it knows a device is
 * soldered down. On this board that is the graphics side. */
#define PCI_DEV_REAL3D 11

static uint8_t g_cfg[32][256];
static uint8_t g_present[32];

static void put32(unsigned dev, unsigned reg, uint32_t v)
{
    g_cfg[dev][reg + 0] = (uint8_t)v;
    g_cfg[dev][reg + 1] = (uint8_t)(v >> 8);
    g_cfg[dev][reg + 2] = (uint8_t)(v >> 16);
    g_cfg[dev][reg + 3] = (uint8_t)(v >> 24);
}

static uint32_t get32(unsigned dev, unsigned reg)
{
    return (uint32_t)g_cfg[dev][reg] |
           ((uint32_t)g_cfg[dev][reg + 1] << 8) |
           ((uint32_t)g_cfg[dev][reg + 2] << 16) |
           ((uint32_t)g_cfg[dev][reg + 3] << 24);
}

/* Absent devices read as all-ones on real hardware; software tests for that. */
#define PCI_NO_DEVICE 0xFFFFFFFFu

static uint32_t swap32(uint32_t v)
{
    return ((v >> 24) & 0xFFu) | ((v >> 8) & 0xFF00u) |
           ((v << 8) & 0xFF0000u) | ((v << 24) & 0xFF000000u);
}

static int tracing(void)
{
    static int on = -1;
    if (on < 0) on = getenv("M3_TRACE_PCI") ? 1 : 0;
    return on;
}

void pci_init(void)
{
    g_config_addr = 0;
    memset(g_cfg, 0, sizeof g_cfg);
    memset(g_present, 0, sizeof g_present);

    /* NCR/Symbios 53C810: vendor 0x1000, device 0x0001, class 01 00 00
     * (mass storage, SCSI). The board maps it where the game already talks to
     * it, so the base address register reads back that address. */
    g_present[PCI_DEV_SCSI] = 1;
    put32(PCI_DEV_SCSI, 0x00, 0x00011000u);   /* device << 16 | vendor */
    put32(PCI_DEV_SCSI, 0x04, 0x02800006u);   /* status | command       */
    put32(PCI_DEV_SCSI, 0x08, 0x01000001u);   /* class 01/00/00, rev 1  */
    put32(PCI_DEV_SCSI, 0x0C, 0x00000000u);
    put32(PCI_DEV_SCSI, 0x10, 0xC0000001u);   /* BAR0, I/O space        */
    put32(PCI_DEV_SCSI, 0x14, 0xC1000000u);   /* BAR1, memory space     */
    put32(PCI_DEV_SCSI, 0x3C, 0x00000100u);   /* interrupt pin A        */

    /* Real3D. The identifiers here are not known from a datasheet -- the game
     * never reads them, it only writes the command register and one vendor
     * register -- so what matters is that the writes land and read back. */
    g_present[PCI_DEV_REAL3D] = 1;
    put32(PCI_DEV_REAL3D, 0x00, 0x178611DBu);  /* placeholder identifiers */
    put32(PCI_DEV_REAL3D, 0x04, 0x02800000u);
    put32(PCI_DEV_REAL3D, 0x08, 0x03000001u);  /* class 03, display       */
}

/* CONFIG_ADDRESS: 31 enable, 23:16 bus, 15:11 device, 10:8 function,
 * 7:2 register. */
void pci_config_address_write(uint32_t v)
{
    g_config_addr = swap32(v);      /* the guest wrote it byte-reversed */
}

uint32_t pci_config_data_read(void)
{
    unsigned bus, dev, fn, reg;
    uint32_t v = PCI_NO_DEVICE;

    if (!(g_config_addr & 0x80000000u))
        return swap32(v);

    bus = (g_config_addr >> 16) & 0xFFu;
    dev = (g_config_addr >> 11) & 0x1Fu;
    fn  = (g_config_addr >> 8) & 0x07u;
    reg = g_config_addr & 0xFCu;

    if (bus == 0 && fn == 0 && dev < 32 && g_present[dev])
        v = get32(dev, reg);

    if (tracing())
        fprintf(stderr, "[pci] read  bus %u dev %2u fn %u reg 0x%02X -> %08X\n",
                bus, dev, fn, reg, v);

    return swap32(v);
}

void pci_config_data_write(uint32_t v)
{
    unsigned bus, dev, fn, reg;

    if (!(g_config_addr & 0x80000000u))
        return;

    bus = (g_config_addr >> 16) & 0xFFu;
    dev = (g_config_addr >> 11) & 0x1Fu;
    fn  = (g_config_addr >> 8) & 0x07u;
    reg = g_config_addr & 0xFCu;
    v   = swap32(v);                /* the guest wrote it byte-reversed */

    if (tracing())
        fprintf(stderr, "[pci] write bus %u dev %2u fn %u reg 0x%02X <- %08X\n",
                bus, dev, fn, reg, v);

    /* Identifiers and class code are read-only; everything else the game
     * writes it expects to read back. */
    if (bus == 0 && fn == 0 && dev < 32 && g_present[dev] &&
        reg >= 0x04 && reg != 0x08)
        put32(dev, reg, v);
}
