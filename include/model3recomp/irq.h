/* model3recomp -- interrupt controller and the frame boundary.
 *
 * READ docs/technical/execution-model.md BEFORE touching this. On Model 3 the
 * frame boundary is not a video-status poll the way it is on Model 2; it is a
 * real interrupt, and the acknowledge path is a busy-wait that will hang the
 * game forever if this file is wrong.
 *
 * The bit assignments and the ack path below are not guesses -- they were read
 * out of The Lost World's own interrupt library at 0xFFF37BD0..0xFFF38100:
 *
 *     lwz   r0, 0x18(r9)      ; r9 = 0xF0100000   -- read pending
 *     andis. r9, r0, 0x100                        -- test, in the HIGH half
 *     ...
 *     stw   r8, 0(r11)        ; r11 = 0xF1180010  -- ACK by writing the bit
 *     lwz   r0, 0x18(r10)                         -- re-read pending
 *     andis. r9, r0, 0x100
 *     bne   -0xC                                  -- SPIN until it clears
 *
 * So: status lives in the upper 16 bits, and an ack written to the tilegen at
 * 0xF1180010 must clear the corresponding bit in 0xF0100018 before the next
 * read, or the spin never exits.
 */
#ifndef MODEL3RECOMP_IRQ_H
#define MODEL3RECOMP_IRQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pending/enable bits, as the game tests them (upper 16 bits of the word). */
#define M3_IRQ_VBLANK_START  0x02000000u  /* tilegen, start of vblank  */
#define M3_IRQ_VBLANK_END    0x01000000u  /* tilegen, end of vblank    */
#define M3_IRQ_SCSI          0x04000000u
#define M3_IRQ_SOUND         0x08000000u
#define M3_IRQ_NET           0x10000000u

void     irq_init(void);

/* Raise / acknowledge. ack is what 0xF1180010 routes to. */
void     irq_raise(uint32_t bits);
void     irq_ack(uint32_t bits);

uint32_t irq_pending(void);          /* raw pending, before enable mask */
uint32_t irq_enable_read(void);
void     irq_enable_write(uint32_t v);

/* ---- The frame boundary ------------------------------------------------
 * Routed from a read of 0xF0100018. Advances the field when one is due,
 * renders, raises vblank, and dispatches the guest's handler -- all nested
 * inside the guest's own poll, because there is nowhere else to stand.
 */
uint32_t irq_status_read(void);

/* Advance the field if one is due and deliver the interrupt. Called from
 * func_table_call(), because a main loop that waits on an interrupt-set RAM
 * flag still dispatches its service routine through a pointer every
 * iteration -- that is the only hook the runtime reliably gets. */
void     irq_tick(void);

/* Walks the guest's own dispatch path rather than hardcoding a handler:
 * the vector the game installed is read out of guest RAM and called through
 * the func table. Returns the number of handlers run. */
int      irq_dispatch(void);

#ifdef __cplusplus
}
#endif
#endif /* MODEL3RECOMP_IRQ_H */
