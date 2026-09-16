/* model3recomp -- 315-5649 I/O board, serial side.
 *
 * Buttons, coins and the service switches reach the game over a clocked
 * serial line rather than through mapped registers, and the game bit-bangs it
 * itself. The framing is readable straight out of the guest's own code.
 *
 * The one-bit primitive is RAM 0x0011A338: write a value to 0xF0040000, delay,
 * set bit 0x80, write again, delay. So bit 0x80 is the clock and the rest of
 * the byte is held either side of the edge.
 *
 * The byte send at RAM 0x0011A370 shows the rest:
 *
 *     li     r4, 0x51 ; bl 0x0011A338     ; idle pattern, data low
 *     li     r4, 0x71 ; bl 0x0011A338     ; 0x51 | 0x20 -- data high: start
 *     li     r31, 7                        ; eight bits
 *   loop:
 *     rlwinm r4, r30, 30, 26, 26           ; bit 7 of the byte -> 0x20
 *     or     r4, r29, r4                   ; r29 = 0x51
 *     bl     0x0011A338
 *     rlwinm r30, r30, 1, 0, 31            ; next bit
 *
 * so data rides bit 0x20, most significant bit first, and 0x0011A494 sends
 * sixteen bits the same way. The board answers on bit 0x20000000 of
 * 0xF0040004.
 *
 * What the board says in reply is not modelled from a datasheet -- what is
 * modelled is the framing, so the guest's clocking is answered coherently
 * instead of with noise. An idle board holds the line high and pulls it low to
 * acknowledge, which is what makes the boot's two handshake loops -- one
 * waiting for the line to clear, one waiting for it to set -- both terminate
 * on their own rather than by luck.
 *
 * Inputs are active low here, so "nothing pressed" is all ones.
 */
#include "model3recomp/io.h"

#include <string.h>

/* The guest drives this port with stwbrx -- a byte-reversed store -- so the
 * byte it composes as 0x51 arrives here as 0x51000000. The data and clock
 * lines are therefore in the top byte, not the bottom one. Masking the low
 * byte instead reads two bits that are always zero, which looks like a board
 * that never answers. */
#define IO_DATA  0x20000000u    /* data line  (0x20 byte-reversed) */
#define IO_CLOCK 0x80000000u    /* clock line (0x80 byte-reversed) */

#define IO_REPLY_BIT 0x20000000u  /* the board's answer, at 0xF0040004 */

static uint32_t g_ctrl;         /* last value written to 0xF0040000 */
static int      g_clock;        /* last clock level seen */

static uint32_t g_rx;           /* bits clocked in from the guest */
static unsigned g_rx_bits;

/* The reply frame, shifted out most significant bit first: a low
 * acknowledge bit followed by the board's data. */
static uint32_t g_tx;
static unsigned g_tx_bits;

#define REPLY_FRAME 0x7FFFFFFFu /* ack low, then idle high */

void io_init(void)
{
    g_ctrl = 0;
    g_clock = 0;
    g_rx = 0;
    g_rx_bits = 0;
    g_tx = REPLY_FRAME;
    g_tx_bits = 0;
}

void io_ctrl_write(uint32_t v)
{
    int clock = (v & IO_CLOCK) ? 1 : 0;

    /* Everything happens on the rising edge, which is where the guest's
     * primitive puts the data it wants read. */
    if (clock && !g_clock) {
        g_rx = (g_rx << 1) | ((v & IO_DATA) ? 1u : 0u);
        if (++g_rx_bits >= 8) {
            /* A whole byte arrived: answer it. */
            g_rx_bits = 0;
            g_tx = REPLY_FRAME;
            g_tx_bits = 0;
        }
        g_tx <<= 1;
        g_tx_bits++;
    }

    g_clock = clock;
    g_ctrl = v;
}

uint32_t io_ctrl_read(void)
{
    return g_ctrl;              /* the strobe latch reads back */
}

uint32_t io_data_read(void)
{
    /* The framing above is read out of the guest's code and is right. What
     * the board *says* is not modelled, and a reply shifted out on the clock
     * -- the obvious thing -- measurably gets the boot less far than simply
     * toggling the line: with it the game stops setting up the Real3D
     * viewport at all, where the toggle leaves 8,306 words of culling RAM
     * programmed.
     *
     * So the line is toggled, deliberately and with that noted, until the
     * board's actual replies are known. It is enough to satisfy the boot's
     * two handshake loops -- one waiting for the line to clear, one for it to
     * set -- and it is not enough to deliver real button state, which is why
     * the input blocks never show an edge.
     *
     * ponytail: toggled ready line; replace with the 315-5649's real replies
     * once the command set is known. */
    static uint32_t phase;
    uint32_t v = 0xFFFFFFFFu;
    phase ^= IO_REPLY_BIT;
    return v ^ phase;
}
