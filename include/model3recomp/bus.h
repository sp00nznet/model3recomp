/* model3recomp -- Model 3 memory bus.
 *
 * Every load and store the lifted game performs comes through here. The map
 * below was confirmed against The Lost World's own code: the boot ROM at
 * 0xFFF00100 does `lis r3,0xf004; lbz r4,0x18(r3)`, and the system-control
 * block at 0xF0100000 is the busiest region in the ROM by a wide margin.
 */
#ifndef MODEL3RECOMP_BUS_H
#define MODEL3RECOMP_BUS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Address map (Step 1.x) -------------------------------------------- */
#define M3_RAM_BASE        0x00000000u
#define M3_RAM_SIZE        0x00800000u   /* 8 MB */

#define M3_R3D_STATUS      0x84000000u   /* Real3D status / ID              */
#define M3_R3D_TRIGGER     0x88000000u   /* Real3D command trigger          */
#define M3_R3D_CULL_LO     0x8C000000u   /* culling RAM low   (4 MB)        */
#define M3_R3D_CULL_HI     0x8E000000u   /* culling RAM high  (1 MB)        */
#define M3_R3D_POLY        0x98000000u   /* polygon RAM       (4 MB)        */
#define M3_R3D_TEXPORT     0x9C000000u   /* texture upload FIFO             */

#define M3_SCSI_BASE       0xC0000000u   /* 53C810, Step 1.x                */
#define M3_SCSI_MIRROR     0xC1000000u

#define M3_INPUTS_BASE     0xF0040000u   /* controls / coins / DIPs         */
#define M3_SOUND_BASE      0xF0080000u   /* 68000 sound board comms         */
#define M3_BACKUP_BASE     0xF00C0000u   /* battery-backed SRAM (128 KB)    */
#define M3_SYSCTL_BASE     0xF0100000u   /* system control + IRQ            */
#define M3_RTC_BASE        0xF0140000u   /* 72421 real-time clock           */
#define M3_SECURITY_BASE   0xF0180000u   /* security board                  */
#define M3_TILEGEN_VRAM    0xF1000000u   /* tilemap VRAM + palette (1.2 MB) */
#define M3_TILEGEN_REGS    0xF1180000u   /* tilemap registers               */
#define M3_BRIDGE_BASE     0xF8FFF000u   /* MPC105/106 host bridge config   */

#define M3_CROM_BANKED     0xFF000000u   /* 8 MB window into CROM0..3       */
#define M3_CROM_FIXED      0xFF800000u   /* 8 MB fixed program CROM         */

/* The 0xFE... aliases are the same devices seen through a second BAT. */
#define M3_MIRROR_MASK     0xFE000000u

/* ---- ROM images the runtime is handed at init -------------------------- */
typedef struct m3_roms {
    const uint8_t *crom;      size_t crom_size;      /* fixed program CROM  */
    const uint8_t *crom_bank; size_t crom_bank_size; /* CROM0..3 banked     */
    const uint8_t *vrom;      size_t vrom_size;      /* texture / model ROM */
    const uint8_t *sndrom;    size_t sndrom_size;    /* 68000 program       */
    const uint8_t *samples;   size_t samples_size;   /* SCSP wave ROM       */
} m3_roms_t;

void     bus_init(const m3_roms_t *roms);
void     bus_shutdown(void);

uint8_t  bus_read8 (uint32_t addr);
uint16_t bus_read16(uint32_t addr);
uint32_t bus_read32(uint32_t addr);
uint64_t bus_read64(uint32_t addr);

void     bus_write8 (uint32_t addr, uint8_t  v);
void     bus_write16(uint32_t addr, uint16_t v);
void     bus_write32(uint32_t addr, uint32_t v);
void     bus_write64(uint32_t addr, uint64_t v);

/* Direct RAM window, for DMA engines and the debugger. Bounds-checked. */
uint8_t *bus_ram(void);

/* The work-RAM base, for the inline fast path in lift.h. Valid between
 * bus_init() and bus_shutdown(); NULL outside that. */
extern uint8_t *m3_ram_base;

/* Tilemap VRAM: 1 MB of pattern and name data followed by 128 KB of palette,
 * addressed by the guest as one block at 0xF1000000. */
uint8_t *bus_vram(size_t *size);

/* Real3D scene memory. Nothing renders these yet, but the guest fills them
 * every frame, so they are the evidence that it is drawing at all. */
uint8_t *bus_cull_lo(size_t *size);
uint8_t *bus_cull_hi(size_t *size);
uint8_t *bus_poly(size_t *size);

/* The VROM holds the Real3D's models and textures. It is deliberately not in
 * the CPU's address map -- on hardware only the graphics processor reads it --
 * so the renderer gets at it through here. */
const uint8_t *bus_vrom(size_t *size);
void     bus_dma_copy(uint32_t dst, uint32_t src, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif /* MODEL3RECOMP_BUS_H */
