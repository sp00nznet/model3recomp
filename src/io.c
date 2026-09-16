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
    /* 0xF0040004 is multiplexed: what it returns depends on what was last
     * written to 0xF0040000. Two callers want two different things from it,
     * and serving either one alone breaks the other.
     *
     * The input path at RAM 0x00117890 strobes and reads:
     *
     *     stw  r10, 0(r11)      ; r10 = 0x01000000 -- clock bit LOW
     *     lwz  r0,  0(r11)      ; handshake is on the control latch
     *     andis. r9, r0, 0x100
     *     beq  -0xC
     *     lwz  r0, 4(r9)        ; <- wants button state here
     *
     * The serial path at RAM 0x0011A650 bit-bangs a byte first, and its
     * primitive at 0x0011A338 leaves the clock bit HIGH:
     *
     *     stwbrx r4, ...        ; 0x51 -> 0x51000000
     *     ori    r4, r4, 0x80
     *     stwbrx r4, ...        ; 0xD1 -> 0xD1000000, clock HIGH
     *     lwz    r0, 0(r30)     ; <- wants the board's reply here
     *
     * So the clock bit tells them apart. Returning the reply line
     * unconditionally injects a phantom button press on every other read --
     * which is what put the game into its own service menu instead of attract
     * mode. Returning button state unconditionally hangs the serial
     * handshake, which waits for the line to change.
     *
     * Inputs are active low, so nothing pressed is all ones. */
    if (g_ctrl & IO_CLOCK) {
        /* ponytail: the reply is toggled, not driven by the real 315-5649
         * protocol -- enough for the handshake, not real board data. */
        static uint32_t phase;
        phase ^= IO_REPLY_BIT;
        return 0xFFFFFFFFu ^ phase;
    }
    return 0xFFFFFFFFu;
}
