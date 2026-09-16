/* model3recomp -- 315-5649 I/O board, serial side.
 *
 * Buttons, coins and the service switches arrive over a clocked serial line
 * the game bit-bangs itself. See src/io.c for the framing, which is read out
 * of the guest's own code rather than a datasheet.
 */
#ifndef MODEL3RECOMP_IO_H
#define MODEL3RECOMP_IO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     io_init(void);

/* 0xF0040000: the strobe/clock/data line the guest drives. */
void     io_ctrl_write(uint32_t v);
uint32_t io_ctrl_read(void);

/* 0xF0040004: input state, with the board's serial reply on bit 0x20000000. */
uint32_t io_data_read(void);

#ifdef __cplusplus
}
#endif
#endif
