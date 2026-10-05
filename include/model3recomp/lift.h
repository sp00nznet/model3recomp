/* model3recomp -- the surface lifted code is compiled against.
 *
 * Generated C includes this and nothing else from the runtime. Keeping the
 * spelling short matters: a lifted Model 3 game is on the order of a hundred
 * thousand statements, and every one of them goes through these macros.
 */
#ifndef MODEL3RECOMP_LIFT_H
#define MODEL3RECOMP_LIFT_H

#include <math.h>

#include "model3recomp/ppc.h"
#include "model3recomp/bus.h"
#include "model3recomp/func_table.h"

/* ---- Memory -------------------------------------------------------------
 * Nearly every guest access is plain work RAM, so it is handled inline here
 * rather than as a call into the bus with a chain of range checks. The bus is
 * still the single place devices are decoded -- this only short-circuits the
 * common case, and the byte order matches what bus.c stores (big-endian, as
 * the guest sees it).
 */
static inline uint32_t m3_ld32(uint32_t a)
{
    if (a < M3_RAM_SIZE - 3u) {
        const uint8_t *p = m3_ram_base + a;
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | p[3];
    }
    return bus_read32(a);
}

static inline uint32_t m3_ld16(uint32_t a)
{
    if (a < M3_RAM_SIZE - 1u) {
        const uint8_t *p = m3_ram_base + a;
        return ((uint32_t)p[0] << 8) | p[1];
    }
    return bus_read16(a);
}

static inline uint32_t m3_ld8(uint32_t a)
{
    if (a < M3_RAM_SIZE)
        return m3_ram_base[a];
    return bus_read8(a);
}

/* Watching a guest RAM address.
 *
 * tools/ppc_interp.py has --watch, and the runtime had no equivalent: lifted
 * code writes work RAM through the inline path below without the bus ever
 * seeing it, so "what wrote this word?" had no answer on the native side.
 * Build with MODEL3RECOMP_WATCH to compile the check in, then set M3_WATCH
 * in the environment to the address: every write that *covers* it is
 * reported with the guest function that did it. The address is a runtime
 * variable rather than a macro because moving the watch is most of the
 * work -- one buffer names the next -- and a rebuild for each hop is
 * twenty minutes. A byte is most often cleared by a word store two bytes
 * below it, which an equality test misses, so the test is containment.
 * Compiles to nothing without the build flag. */
#ifdef M3_WATCH
void m3_watch_hit(uint32_t addr, uint32_t val, unsigned size);
extern uint32_t m3_watch_addr;
#define M3_WATCH_NOTE(a, v, n)                                              do { if (m3_watch_addr - (uint32_t)(a) < (uint32_t)(n))                          m3_watch_hit((uint32_t)(a), (uint32_t)(v), (n)); } while (0)
#else
#define M3_WATCH_NOTE(a, v, n) ((void)0)
#endif

static inline void m3_st32(uint32_t a, uint32_t v)
{
    M3_WATCH_NOTE(a, v, 4);
    if (a < M3_RAM_SIZE - 3u) {
        uint8_t *p = m3_ram_base + a;
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
        return;
    }
    bus_write32(a, v);
}

static inline void m3_st16(uint32_t a, uint32_t v)
{
    M3_WATCH_NOTE(a, v, 2);
    if (a < M3_RAM_SIZE - 1u) {
        uint8_t *p = m3_ram_base + a;
        p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
        return;
    }
    bus_write16(a, (uint16_t)v);
}

static inline void m3_st8(uint32_t a, uint32_t v)
{
    M3_WATCH_NOTE(a, v, 1);
    if (a < M3_RAM_SIZE) { m3_ram_base[a] = (uint8_t)v; return; }
    bus_write8(a, (uint8_t)v);
}

#define MEM_R8(a)     m3_ld8 ((uint32_t)(a))
#define MEM_R16(a)    m3_ld16((uint32_t)(a))
#define MEM_R32(a)    m3_ld32((uint32_t)(a))
#define MEM_R64(a)    bus_read64((uint32_t)(a))
#define MEM_W8(a,v)   m3_st8 ((uint32_t)(a),(uint32_t)(v))
#define MEM_W16(a,v)  m3_st16((uint32_t)(a),(uint32_t)(v))
#define MEM_W32(a,v)  m3_st32((uint32_t)(a),(uint32_t)(v))
#define MEM_W64(a,v)  bus_write64((uint32_t)(a),(uint64_t)(v))

/* Condition / flag updates */
#define CR0(v)        m3_cr0(&m3_ctx,(int32_t)(v))
#define CRF(f,v)      m3_cr_set(&m3_ctx,(f),(v))
#define CMPS(f,a,b)   m3_cmp_s(&m3_ctx,(f),(int32_t)(a),(int32_t)(b))
#define CMPU(f,a,b)   m3_cmp_u(&m3_ctx,(f),(uint32_t)(a),(uint32_t)(b))
#define FCMP(f,a,b)   m3_fcmp(&m3_ctx,(f),(a),(b))
#define CRB(b)        m3_crbit(&m3_ctx,(b))
#define CRB_SET(b,v)  m3_crbit_set(&m3_ctx,(b),(v))
#define CA            m3_xer_ca(&m3_ctx)
#define SET_CA(c)     m3_set_ca(&m3_ctx,(c))
#define SET_OV(o)     m3_set_ov(&m3_ctx,(o))

#define ROTL32(v,n)   m3_rotl32((uint32_t)(v),(n))
#define MASK(mb,me)   m3_mask((mb),(me))
#define CNTLZW(v)     m3_cntlzw((uint32_t)(v))

/* Single-precision ops round through float; they do not truncate. */
#define F32(d)        ((double)(float)(d))

static inline double m3_fabs(double d)  { return fabs(d); }
static inline double m3_sqrt(double d)  { return sqrt(d); }
/* fctiw honours FPSCR[RN]; every Model 3 title leaves it at round-to-nearest,
 * which is what nearbyint gives under the default host mode. */
static inline double m3_round(double d) { return nearbyint(d); }

uint32_t m3_timebase(unsigned spr);
void     m3_timebase_set(unsigned spr, uint32_t v);

/* Indirect control transfer. Every target the lifter could not resolve
 * statically lands here and is looked up at runtime. */
#define CALL(a)       func_table_call((uint32_t)(a))

/* Finding a guest that is looping.
 *
 * A recompiled game has no program counter to inspect: once lifted code is
 * running, nothing outside it knows where "there" is. When a guest wedges in
 * a loop that touches no device and dispatches through no pointer, even the
 * bus-level spin report sees nothing, because nothing reaches the runtime at
 * all.
 *
 * So the lifter marks two things: the guest address of every function it
 * enters, and every backward branch. Both compile to nothing unless the build
 * defines M3_LOOP_GUARD, so a normal build is unchanged; with it, a guest
 * that spins says which of its own functions it is spinning in.
 */
/* Every lifted function opens with M3_FN. The one at the game's safe point
 * (savestate.h) is where save states are taken and restored; for every other
 * function this is one compare against a constant. */
#include "model3recomp/savestate.h"
#define M3_SAFEPOINT(a) ((uint32_t)(a) == m3_safepoint_pc ? m3_safepoint() : (void)0)

#ifdef M3_LOOP_GUARD
extern uint32_t m3_fn_now;
void m3_loop_guard(uint32_t branch_at);
void m3_fn_count(uint32_t addr);
void m3_fn_report(unsigned top);
extern uint32_t m3_fn_prev;
/* The last few function entries in order, for m3_watch_hit(). */
#define M3_FN_RING 32u
extern uint32_t m3_fn_ring[M3_FN_RING];
extern uint32_t m3_fn_ring_n;
#define M3_FN(a)        (m3_fn_prev = m3_fn_now, m3_fn_now = (uint32_t)(a), m3_fn_count((uint32_t)(a)), M3_SAFEPOINT(a))
#define M3_LOOP_NOTE(a) m3_loop_guard((uint32_t)(a))
#else
#define M3_FN(a)        M3_SAFEPOINT(a)
#define M3_LOOP_NOTE(a) ((void)0)
#endif

/* The third place the runtime gets control.
 *
 * func_table_call() and dev_read() were the other two, and between them they
 * miss the case that matters most: a guest waiting on a RAM word that only an
 * interrupt handler writes. That loop dispatches through no pointer and reads
 * no device, so the field clock stops, so the interrupt never arrives, so the
 * word never changes. The Lost World waits exactly like that at 0x0011837C
 * and would sit there forever.
 *
 * A backward branch is one iteration of a loop, so counting those is counting
 * guest work. Doing it every iteration would cost more than it is worth, so
 * the runtime gets a turn every M3_LOOP_PERIOD of them and is credited for
 * all of them at once.
 *
 * The credit is a calibration knob and the numbers behind it are these: a
 * 66 MHz 603e retires about 1.1 M instructions in a field, and a field is
 * 17385 units, so a unit is roughly 64 instructions. A tight wait loop is a
 * handful of instructions per iteration, so crediting one unit per iteration
 * -- the rate a device access gets -- ages a spinning guest about sixteen
 * times too fast, and the symptom is not subtle: fields tick by in their
 * thousands, the guest takes an interrupt on each, and its main loop never
 * runs between them.
 *
 * ponytail: one unit per sixteen iterations, flat. Upgrade path: weight it by
 * the size of the loop body, which the lifter knows and this does not. */
#define M3_LOOP_PERIOD 1024u
#define M3_LOOP_CREDIT (M3_LOOP_PERIOD / 16u)
extern uint32_t m3_loop_n;
void m3_loop_tick(void);
#define M3_LOOPED(a)                                                       do {                                                                       M3_LOOP_NOTE(a); M3_SAFEPOINT(a);                                                       if (!(++m3_loop_n & (M3_LOOP_PERIOD - 1u))) m3_loop_tick();        } while (0)

/* An instruction the lifter does not translate. Greppable and countable --
 * never a silent wrong answer. See tools/ppc_lifter.py --stats. */
void m3_unimplemented(uint32_t addr, uint32_t raw, const char *mn);
#define UNIMPL(a,r,m) m3_unimplemented((a),(r),(m))

#endif /* MODEL3RECOMP_LIFT_H */
