/* model3recomp -- the sound board: a 68000, two SCSPs, and the UART that
 * joins it to the PowerPC.
 *
 * The 68000 is interpreted (Musashi), not recompiled: it is a separate
 * 11.3 MHz processor running a 512 KB driver, and an interpreter costs a
 * few per cent of one core. The board runs once a field, in step with the
 * guest -- a field's worth of 44.1 kHz samples, 256 68000 cycles each -- so
 * it is as deterministic as the rest of the machine: what the PowerPC reads
 * back from it depends only on the field count and what it was sent.
 */
#ifndef MODEL3RECOMP_SOUND_H
#define MODEL3RECOMP_SOUND_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* prog: the 68000 program (big-endian, 512 KB); samples: the wave ROM
 * (big-endian, a multiple of 8 MB). Either may be NULL: no sound board. */
void     sound_init(const uint8_t *prog, size_t prog_size,
                    const uint8_t *samples, size_t samples_size);
int      sound_present(void);

/* The PowerPC side: 0xF0080000 data, 0xF0080004 status / control. */
uint8_t  sound_uart_read(unsigned reg);
void     sound_uart_write(unsigned reg, uint8_t v);

/* Run the board for one field and hand its audio to the platform. */
void     sound_run_field(void);

#ifdef __cplusplus
}
#endif
#endif
