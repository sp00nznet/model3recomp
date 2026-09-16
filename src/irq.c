/* model3recomp -- interrupt controller and the frame boundary.
 *
 * Read docs/technical/execution-model.md first.
 *
 * The original design here hung the frame off the guest's read of the
 * interrupt status register at 0xF0100018, the way model2recomp hangs it off
 * a video-status poll. That was wrong, and the interpreter proved it: The
 * Lost World's main loop never reads 0xF0100018 at all. It looks like this:
 *
 *     0x00118284  lwz  r0, -0x126C(r29)    ; handler pointer in RAM
 *                 mtlr r0 ; blrl           ; call the service routine
 *     0x00118290  lbz  r0, 1(r30)          ; flag byte at RAM 0x6ED
 *                 cmpwi cr1, r0, 0
 *     0x001182A0  bc   0x00118284          ; spin until the flag changes
 *
 * Only the VBlank handler sets that flag. So interrupts have to be delivered
 * on a timer, not in response to a poll -- which is what the hardware does
 * anyway. The runtime gets control at every func_table_call, and a main loop
 * that waits on an interrupt-set flag necessarily dispatches through one, so
 * that is where the field advances.
 */
#include "model3recomp/irq.h"
#include "model3recomp/func_table.h"
#include "model3recomp/bus.h"
#include "model3recomp/model3recomp.h"

#include <stdio.h>

#define MSR_EE 0x00008000u
#define MSR_IP 0x00000040u

static uint32_t g_pending;
static uint32_t g_enable;
static uint64_t g_field;
static int      g_in_dispatch;

uint64_t m3_next_tick;

/* How many guest instructions between checks. A field is ~1.15 M, so this is
 * fine-grained enough to place a field accurately and coarse enough that the
 * check costs nothing. */
#define TICK_QUANTUM 65536u

void m3_tick(void)
{
    m3_next_tick = m3_work + TICK_QUANTUM;
    irq_tick();
}

void irq_init(void)
{
    m3_next_tick = TICK_QUANTUM;
    g_pending = 0;
    g_enable  = 0;
    g_field   = 0;
    g_in_dispatch = 0;
}

void irq_raise(uint32_t bits)      { g_pending |= bits; }
void irq_ack(uint32_t bits)        { g_pending &= ~bits; }
uint32_t irq_pending(void)         { return g_pending; }
uint32_t irq_enable_read(void)     { return g_enable; }
void irq_enable_write(uint32_t v)  { g_enable = v; }

/* Advance the field if one is due, and deliver the interrupt.
 *
 * Safe to call from anywhere lifted code reaches the runtime. Dispatching
 * from inside a call looks alarming but is exactly what the hardware does --
 * an interrupt lands between two instructions -- and the guest's handler
 * saves and restores every register itself (stmw/lmw around the body), so
 * there is nothing of the interrupted function's state for us to preserve.
 * Our own lifted code keeps its temporaries in C locals on the host stack,
 * which the guest cannot touch.
 */
void irq_tick(void)
{
    /* Watchdog. A recompiled game that stops making progress gives no stack
     * and no program counter, so the runtime reports the little it knows:
     * whether it is still being reached at all, whether the guest is inside
     * its own interrupt handler and not coming out, and whether the clock is
     * moving. Each cause looks identical from outside without this. */
    {
        static uint64_t calls, last_work, quiet;
        calls++;
        if (m3_work != last_work) { last_work = m3_work; quiet = 0; }
        else if (++quiet == 20000000ull)
            fprintf(stderr, "[model3recomp] %llu ticks, clock frozen at %llu, "
                            "in_dispatch=%d\n",
                    (unsigned long long)calls, (unsigned long long)m3_work,
                    g_in_dispatch);
    }

    if (g_in_dispatch)
        return;

    if (model3recomp_field_due()) {
        g_field++;
        model3recomp_end_frame();      /* render and present */

        /* Only the start-of-VBlank line is raised. Raising the end line in
         * the same tick as well -- which this did at first -- leaves the
         * guest seeing 0x03000000 pending where the hardware would show
         * 0x02000000, because the two are separate events at opposite ends of
         * the blanking interval. The differential trace caught that one. */
        irq_raise(M3_IRQ_VBLANK_START);
        /* The guest's enable mask is 0x2E000000, so 0x20000000 and
         * 0x08000000 are enabled too and this runtime never asserts them.
         * Asserting them with the field is wrong, though: the guest does not
         * acknowledge them, so they stay pending and the handler re-enters
         * 212,812 times across 600 fields instead of 482. It is not waiting
         * for them. */
        model3recomp_begin_frame();
    }

    /* Try to deliver on every tick, not only when a field has just elapsed.
     * A pending, enabled interrupt fires as soon as the guest permits it --
     * that is what a level-triggered line does. Dispatching only at the field
     * boundary silently drops the interrupt whenever the guest happened to
     * have MSR[EE] clear at that instant, and the handler then ran 462 times
     * across 600 fields instead of once per field. */
    irq_dispatch();
}

/* Kept as the routing for a read of 0xF0100018: a guest that *does* poll the
 * controller still gets a field out of it. */
uint32_t irq_status_read(void)
{
    irq_tick();
    return g_pending;
}

/* Walk the guest's own path. The vector base follows MSR[IP], which the game
 * clears once it has installed handlers in RAM, so the address is derived
 * rather than cached -- a game that moves its handlers keeps working, and one
 * the lifter missed shows up as a named func-table miss. */
int irq_dispatch(void)
{
    uint32_t vec_base, saved_msr;
    int ran = 0;

    if (g_in_dispatch)
        return 0;
    if (!(g_pending & g_enable))
        return 0;
    if (!(m3_ctx.msr & MSR_EE))        /* guest has interrupts masked */
        return 0;

    vec_base = (m3_ctx.msr & MSR_IP) ? 0xFFF00000u : 0x00000000u;

    /* What the processor hands the handler. It reads these immediately
     * (mfspr r31,SRR0 / mfspr r30,SRR1) and restores them before its rfi. */
    saved_msr = m3_ctx.msr;
    m3_ctx.spr[26] = 0;                /* SRR0: no meaningful resume address */
    m3_ctx.spr[27] = saved_msr;        /* SRR1 */
    m3_ctx.msr &= ~MSR_EE;             /* EE off for the duration */

    g_in_dispatch = 1;
    if (!func_table_call(vec_base + 0x500))
        fprintf(stderr, "[model3recomp] no handler lifted at %08X "
                        "(external interrupt)\n", vec_base + 0x500);
    else
        ran++;
    g_in_dispatch = 0;

    m3_ctx.msr = saved_msr;            /* the guest's rfi became a return */
    return ran;
}
