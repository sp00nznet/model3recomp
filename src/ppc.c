/* model3recomp -- PowerPC 603e context.
 *
 * There is almost nothing here, and that is the point: lifted code mutates
 * m3_ctx directly through the macros in lift.h. This file owns the storage,
 * the reset state, and the few helpers that need a real function.
 */
#include "model3recomp/savestate.h"
#include "model3recomp/ppc.h"
#include "model3recomp/lift.h"
#include "model3recomp/model3recomp.h"
#include "model3recomp/irq.h"

#include <stdio.h>
#include <stdlib.h>

m3_ppc_t m3_ctx;

uint64_t m3_work;

/* The 603e comes out of reset with MSR[IP] set, so the vectors are the ones
 * at 0xFFF00000 in ROM. Games clear it once they have installed their own in
 * RAM -- see docs/technical/execution-model.md. */
#define MSR_IP 0x00000040u
#define MSR_ME 0x00001000u

void ppc_reset(void)
{
    unsigned i;
    for (i = 0; i < 32; i++) { m3_ctx.r[i] = 0; m3_ctx.f[i] = 0.0; }
    for (i = 0; i < 16; i++) m3_ctx.sr[i] = 0;
    for (i = 0; i < 1024; i++) m3_ctx.spr[i] = 0;
    m3_ctx.lr = m3_ctx.ctr = m3_ctx.xer = m3_ctx.cr = m3_ctx.fpscr = 0;
    m3_ctx.reserve_addr = 0;
    m3_ctx.reserve = 0;
    m3_ctx.msr = MSR_IP | MSR_ME;
}

/* The time base counts at the bus clock over four -- about 16.5 MHz on this
 * board, or some 275,000 ticks in a field.
 *
 * It used to advance one tick per read, which is not a clock: it makes the
 * value depend on how often a guest looks at it rather than on how much time
 * has passed. A guest that only wants monotonicity does not notice. One that
 * *measures* with it gets nonsense, and The Lost World measures with it --
 * it times a known interval and divides to get a decrementer period. Reading
 * one tick per read, it computed a period of -26 where the hardware gives
 * 26,926, so its decrementer never fired again and the main loop waited
 * forever on a counter only that interrupt writes.
 *
 * So it runs off m3_work, the same clock the field and the decrementer run
 * off, scaled to match: a field is M3_FIELD_WORK units and about 275,000
 * ticks, which is the 16 below, and the decrementer counts down at the same
 * rate. The two agree now, which is the part that matters -- a guest timing
 * one against the other gets a self-consistent answer. */
#define TB_PER_WORK 16u

static uint64_t g_tb_origin;    /* m3_work when the time base was last set */
static uint64_t g_tb_value;     /* what it was set to */

static uint64_t tb_now(void)
{
    return g_tb_value + (m3_work - g_tb_origin) * (uint64_t)TB_PER_WORK;
}

uint32_t m3_timebase(unsigned spr)
{
    uint64_t tb = tb_now();
    return (spr == 269) ? (uint32_t)(tb >> 32) : (uint32_t)tb;
}

/* Writing TBL or TBU re-bases the clock. A guest that times an interval does
 * it by zeroing the time base, waiting, and reading it back; drop the write
 * and the interval it measures is however long the machine has been on, which
 * is not what it asked for and not a number it can divide by. */
void m3_timebase_set(unsigned spr, uint32_t v)
{
    uint64_t tb = tb_now();
    if (spr == 285)
        tb = ((uint64_t)v << 32) | (uint32_t)tb;
    else
        tb = (tb & 0xFFFFFFFF00000000ull) | v;
    g_tb_value = tb;
    g_tb_origin = m3_work;
}

#ifdef M3_WATCH
/* Reported with the guest function that did the write, which is the part that
 * matters: the address alone says a word changed, not who changed it. */
uint32_t m3_watch_addr = 0xFFFFFFFFu;   /* set from M3_WATCH at init */
static uint64_t g_watch_from;           /* M3_WATCH_FROM: first field */
static int      g_watch_ring;           /* M3_WATCH_RING: call history */

void m3_watch_init(void)
{
    const char *e = getenv("M3_WATCH");
    m3_watch_addr = e ? (uint32_t)strtoul(e, NULL, 0) : 0xFFFFFFFFu;
    e = getenv("M3_WATCH_FROM");
    g_watch_from = e ? strtoull(e, NULL, 0) : 0;
    g_watch_ring = getenv("M3_WATCH_RING") != NULL;
    { void m3_fn_args_init(void); m3_fn_args_init(); }
}

void m3_watch_hit(uint32_t addr, uint32_t val, unsigned size)
{
    uint32_t k;
    if (model3recomp_frame_count() < g_watch_from) return;
    fprintf(stderr, "[model3recomp] watch %08X = %08X (size %u) from guest "
                    "function %08X, field %llu\n",
            addr, val, size, m3_fn_now,
            (unsigned long long)model3recomp_frame_count());
    /* m3_fn_now is where lifted code last *entered* a function, which is
     * not where it is now: nothing restores it on return, so a write a
     * caller makes after its callee returned is reported against the
     * callee. The last entries in order say which caller that was. */
    if (!g_watch_ring) { fflush(stderr); return; }
    fprintf(stderr, "    r3=%08X r4=%08X r5=%08X r31=%08X\n",
            PPC_R(3), PPC_R(4), PPC_R(5), PPC_R(31));
    fprintf(stderr, "    last functions entered:");
    for (k = m3_fn_ring_n > M3_FN_RING ? m3_fn_ring_n - M3_FN_RING : 0;
         k < m3_fn_ring_n; k++)
        fprintf(stderr, " %08X", m3_fn_ring[k & (M3_FN_RING - 1u)]);
    fprintf(stderr, "\n");
    fflush(stderr);
}
#endif

/* See M3_LOOPED in lift.h. Credits the guest with the work its loop did and
 * lets the field clock run, so a wait on an interrupt-set RAM word ends. */
uint32_t m3_loop_n;

void m3_loop_tick(void)
{
    m3_work += M3_LOOP_CREDIT;
    irq_tick();
}

/* Where lifted code last entered a guest function, and the guard that reports
 * it. Only compiled into a build that defines M3_LOOP_GUARD; see lift.h. */
#ifdef M3_LOOP_GUARD
uint32_t m3_fn_now;
uint32_t m3_fn_prev;

/* Which guest functions ran, and how often.
 *
 * M3_TRACE_CALLS only sees indirect dispatch, so a frame task's whole call
 * tree -- every direct call the lifter resolved statically -- is invisible to
 * it. This counts every function entry instead, which is the difference
 * between "the frame task ran" and "the frame task ran and here is what it
 * did". Open addressing, no allocation, no ordering: it runs on every call.
 */
#define FN_SLOTS 8192u
static uint32_t g_fn_addr[FN_SLOTS];
static uint32_t g_fn_hits[FN_SLOTS];

/* The last few function entries, in order. See m3_watch_hit(). */
uint32_t m3_fn_ring[M3_FN_RING];
uint32_t m3_fn_ring_n;

/* M3_FN_ARGS=<guest addr>: print the argument registers every time that
 * function is entered, from field M3_WATCH_FROM. A watch on memory says
 * what a routine wrote; this says what it was told. Two builds of the same
 * scene -- one that renders and one that does not -- diverge somewhere in
 * these numbers long before they diverge in RAM. */
static uint32_t g_fn_args = 0xFFFFFFFFu;
static uint64_t g_fn_args_from;
static uint32_t g_fn_mat = 0xFFFFFFFFu;
void m3_fn_args_init(void)
{
    const char *e = getenv("M3_FN_ARGS");
    g_fn_args = e ? (uint32_t)strtoul(e, NULL, 0) : 0xFFFFFFFFu;
    e = getenv("M3_WATCH_FROM");
    g_fn_args_from = e ? strtoull(e, NULL, 0) : 0;
    e = getenv("M3_FN_MAT");
    g_fn_mat = e ? (uint32_t)strtoul(e, NULL, 0) : 0xFFFFFFFFu;
}

void m3_fn_count(uint32_t addr)
{
    m3_fn_ring[m3_fn_ring_n++ & (M3_FN_RING - 1u)] = addr;
    if (addr == g_fn_args && model3recomp_frame_count() >= g_fn_args_from) {
        fprintf(stderr, "[args] %08X f1=%.6g f2=%.6g f3=%.6g"
                        "  r3=%08X r4=%08X field %llu\n",
                addr, PPC_F(1), PPC_F(2), PPC_F(3), PPC_R(3), PPC_R(4),
                (unsigned long long)model3recomp_frame_count());
        if (g_fn_mat != 0xFFFFFFFFu) {
            /* A value under 32 means "the matrix this register points at",
             * which is how you follow an argument rather than a fixed
             * buffer. */
            uint32_t at = g_fn_mat < 32u ? PPC_R(g_fn_mat) : g_fn_mat;
            unsigned k;
            fprintf(stderr, "       mat@%08X", at);
            for (k = 0; k < 12; k++)
                fprintf(stderr, " %.5g",
                        (double)m3_bits_to_f32(m3_ld32(at + k * 4u)));
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
    uint32_t i = (addr * 2654435761u) & (FN_SLOTS - 1u);
    uint32_t probes = 0;
    for (; probes < 64u; probes++) {
        if (g_fn_addr[i] == addr) { g_fn_hits[i]++; return; }
        if (!g_fn_hits[i]) { g_fn_addr[i] = addr; g_fn_hits[i] = 1; return; }
        i = (i + 1u) & (FN_SLOTS - 1u);
    }
}

void m3_fn_report(unsigned top)
{
    unsigned n;
    fprintf(stderr, "[model3recomp] guest functions entered, most first:\n");
    for (n = 0; n < top; n++) {
        uint32_t best = 0, bi = FN_SLOTS;
        uint32_t i;
        for (i = 0; i < FN_SLOTS; i++)
            if (g_fn_hits[i] > best) { best = g_fn_hits[i]; bi = i; }
        if (bi == FN_SLOTS) break;
        fprintf(stderr, "    %08X  x%u\n", g_fn_addr[bi], g_fn_hits[bi]);
        g_fn_hits[bi] = 0;
    }
    fflush(stderr);
}

void m3_loop_guard(uint32_t branch_at)
{
    static uint64_t spins;
    static uint64_t seen_frame;
    uint64_t now = model3recomp_frame_count();

    if (now != seen_frame) {          /* progress: the guest is fine */
        seen_frame = now;
        spins = 0;
        return;
    }
    if (++spins < 500000000ull)
        return;
    fprintf(stderr, "[model3recomp] %llu backward branches and no field "
                    "advance; the guest is looping at %08X, in the function "
                    "entered at %08X\n",
            (unsigned long long)spins, branch_at, m3_fn_now);
    fprintf(stderr, "    in_dispatch=%d  pending=%08X  enable=%08X  field=%llu\n",
            irq_in_dispatch(), irq_pending(), irq_enable_read(),
            (unsigned long long)now);
    fflush(stderr);
    spins = 0;
}
#endif

/* An instruction the lifter did not translate. Loud once per site, then
 * silent -- a hot loop containing one would otherwise bury everything else. */
void m3_unimplemented(uint32_t addr, uint32_t raw, const char *mn)
{
    enum { SEEN_MAX = 64 };
    static uint32_t seen[SEEN_MAX];
    static unsigned n;
    unsigned i;
    for (i = 0; i < n; i++)
        if (seen[i] == addr) return;
    if (n < SEEN_MAX) seen[n++] = addr;
    fprintf(stderr, "[model3recomp] unimplemented %s at %08X (%08X)\n",
            mn ? mn : "?", addr, raw);
}

void ppc_state(m3_state_t *st)
{
    M3_STATE_VAR(st, m3_ctx);
    M3_STATE_VAR(st, m3_work);
    M3_STATE_VAR(st, g_tb_origin);
    M3_STATE_VAR(st, g_tb_value);
}
