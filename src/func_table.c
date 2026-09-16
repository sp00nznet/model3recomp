/* model3recomp -- guest address -> native function dispatch.
 *
 * Open-addressed, power-of-two, linear probing. A lifted Model 3 game is a
 * couple of thousand functions; this never grows past a few tens of KB and
 * every indirect call in the game goes through it, so it stays simple and
 * branch-light rather than clever.
 */
#include "model3recomp/func_table.h"
#include "model3recomp/irq.h"
#include "model3recomp/model3recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t addr; m3_func_t fn; } slot_t;

static slot_t  *g_tab;
static uint32_t g_mask;
static uint32_t g_count;

/* Knuth multiplicative. Guest addresses are 4-byte aligned and heavily
 * clustered, so the low bits alone would collide constantly. */
static inline uint32_t hash(uint32_t a)
{
    return (a >> 2) * 2654435761u;
}

static void grow(void);

void func_table_init(void)
{
    free(g_tab);
    g_mask  = 1023;
    g_count = 0;
    g_tab   = calloc(g_mask + 1, sizeof(slot_t));
    if (!g_tab) { fprintf(stderr, "[model3recomp] func table alloc failed\n"); abort(); }
}

void func_table_register(uint32_t addr, m3_func_t fn)
{
    uint32_t i;
    if (!g_tab) func_table_init();
    /* Keep it under three-quarters full; linear probing degrades fast above. */
    if ((g_count + 1) * 4 > (g_mask + 1) * 3) grow();

    i = hash(addr) & g_mask;
    for (;;) {
        if (!g_tab[i].fn) {
            g_tab[i].addr = addr;
            g_tab[i].fn   = fn;
            g_count++;
            return;
        }
        if (g_tab[i].addr == addr) { g_tab[i].fn = fn; return; }
        i = (i + 1) & g_mask;
    }
}

static void grow(void)
{
    slot_t  *old  = g_tab;
    uint32_t oldn = g_mask + 1, i;
    g_mask = g_mask * 2 + 1;
    g_count = 0;
    g_tab = calloc(g_mask + 1, sizeof(slot_t));
    if (!g_tab) { fprintf(stderr, "[model3recomp] func table grow failed\n"); abort(); }
    for (i = 0; i < oldn; i++)
        if (old[i].fn) func_table_register(old[i].addr, old[i].fn);
    free(old);
}

m3_func_t func_table_lookup(uint32_t addr)
{
    uint32_t i;
    if (!g_tab) return NULL;
    i = hash(addr) & g_mask;
    for (;;) {
        if (!g_tab[i].fn) return NULL;
        if (g_tab[i].addr == addr) return g_tab[i].fn;
        i = (i + 1) & g_mask;
    }
}

/* A miss is a lifter coverage hole, not a game bug, and naming it is the most
 * useful thing this library can do when a port stops making progress. Each
 * address is reported once; an indirect call in a hot loop would otherwise
 * drown out everything else. */
/* M3_TRACE_CALLS=N prints the first N dispatched addresses. A recompiled game
 * has no program counter to inspect, so this is the equivalent of watching
 * where it goes. */
static long trace_budget(void)
{
    static long n = -1;
    if (n < 0) {
        const char *e = getenv("M3_TRACE_CALLS");
        n = e ? strtol(e, NULL, 0) : 0;
    }
    return n;
}

/* Which guest addresses are dispatched most. A recompiled game gives no stack
 * and no program counter, so when it stops making progress this is the closest
 * thing to "where is it": the routines its idle loop keeps calling. */
enum { HOT_SLOTS = 64 };
static struct { uint32_t addr; uint64_t n; } g_hot[HOT_SLOTS];

static void hot_note(uint32_t addr)
{
    unsigned i, weakest = 0;
    for (i = 0; i < HOT_SLOTS; i++) {
        if (g_hot[i].addr == addr && g_hot[i].n) { g_hot[i].n++; return; }
        if (g_hot[i].n < g_hot[weakest].n) weakest = i;
    }
    g_hot[weakest].addr = addr;
    g_hot[weakest].n = 1;
}

void func_table_dump_hot(void)
{
    unsigned i, j;
    fprintf(stderr, "[model3recomp] most-dispatched guest addresses:\n");
    for (j = 0; j < 12; j++) {
        unsigned best = 0;
        for (i = 0; i < HOT_SLOTS; i++)
            if (g_hot[i].n > g_hot[best].n) best = i;
        if (!g_hot[best].n) break;
        fprintf(stderr, "    %08X  x%llu\n", g_hot[best].addr,
                (unsigned long long)g_hot[best].n);
        g_hot[best].n = 0;
    }
}

int func_table_call(uint32_t addr)
{
    hot_note(addr);

    enum { MISS_MAX = 64 };
    static uint32_t miss[MISS_MAX];
    static unsigned nmiss;
    static long traced;
    unsigned k;

    m3_work += 16;                 /* a dispatched call is real work */

    if (traced < trace_budget()) {
        traced++;
        fprintf(stderr, "[call %ld] %08X%s\n", traced, addr,
                func_table_lookup(addr) ? "" : "   <-- NOT LIFTED");
    }

    m3_func_t fn = func_table_lookup(addr);

    /* The frame boundary. A Model 3 main loop waits on a RAM flag that only
     * the VBlank handler sets, so nothing advances unless interrupts are
     * delivered on a timer -- and this is where the runtime gets control.
     * See docs/technical/execution-model.md. */
    irq_tick();

    if (fn) { fn(); return 1; }

    for (k = 0; k < nmiss; k++)
        if (miss[k] == addr) return 0;
    if (nmiss < MISS_MAX) miss[nmiss++] = addr;
    fprintf(stderr, "[model3recomp] no function lifted at %08X\n", addr);
    return 0;
}
