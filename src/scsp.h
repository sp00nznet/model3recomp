/* model3recomp -- Yamaha YMF292-F SCSP, two per sound board.
 *
 * A C port of MAME's scsp.cpp / scspdsp.cpp (BSD-3-Clause; ElSemi,
 * R. Belmont, kingshriek -- see the notice in scsp.c). The differences are
 * in the plumbing, not the sound: timers count output samples rather than
 * scheduler time, the MIDI ports are byte queues rather than bit-level
 * serial, and the noise source is a seeded generator -- so a chip run for
 * the same number of samples with the same inputs ends in the same state,
 * which netplay and save states depend on.
 */
#ifndef MODEL3RECOMP_SCSP_H
#define MODEL3RECOMP_SCSP_H

#include <stdint.h>
#include <stddef.h>

typedef struct scsp scsp_t;

/* ram: the chip's own sample RAM (big-endian words), shared with the
 * 68000. irq: raise (level, 1) or clear (level, 0) a 68000 interrupt line;
 * NULL for a chip whose interrupt is not wired, which also leaves its
 * timers off (MAME's model3 wires only the first). midi_out: a byte the
 * chip sends out of its MIDI port. */
scsp_t  *scsp_create(uint8_t *ram, size_t ram_size,
                     void (*irq)(int level, int state),
                     void (*midi_out)(uint8_t byte));
void     scsp_reset(scsp_t *s);

uint16_t scsp_read16(scsp_t *s, uint32_t offset);            /* byte offset */
void     scsp_write16(scsp_t *s, uint32_t offset, uint16_t data, uint16_t mask);

/* One 44.1 kHz sample, stereo, added into *l / *r (16-bit range). */
void     scsp_sample(scsp_t *s, int32_t *l, int32_t *r);

int      scsp_active(scsp_t *s);              /* voices playing */
int      scsp_midi_in(scsp_t *s, uint8_t byte);   /* 0 if its FIFO is full */

/* Save states: the chip as raw bytes, with its pointers fixed up after. */
size_t   scsp_state_size(void);
void     scsp_state_save(scsp_t *s, void *buf);
void     scsp_state_load(scsp_t *s, const void *buf);

#endif
