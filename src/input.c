/* model3recomp -- the per-field input record. See input.h. */
#include "model3recomp/savestate.h"
#include "model3recomp/input.h"
#include "model3recomp/model3recomp.h"
#include "model3recomp/platform.h"
#include "model3recomp/netplay.h"

#include <stdlib.h>
#include <string.h>

static m3_input_t g_cur;            /* what the guest reads this field */
static uint32_t   g_cheats_local, g_cheat_actions, g_cheat_pulse;
static const char *g_cheat_label[32];
static uint32_t   g_options_local, g_options_saved;   /* saved: which slots came from settings */
static struct { const char *label; const char *const *choices; unsigned n; } g_opt[8];

const m3_input_t *m3_input(void) { return &g_cur; }

void m3_gun_from_screen(int px, int py, int w, int h, uint16_t *gx, uint16_t *gy)
{
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > w - 1) px = w - 1;
    if (py > h - 1) py = h - 1;
    *gx = (uint16_t)(M3_GUN_X0 + px * (M3_GUN_X1 - M3_GUN_X0) / (w > 1 ? w - 1 : 1));
    *gy = (uint16_t)(M3_GUN_Y0 + py * (M3_GUN_Y1 - M3_GUN_Y0) / (h > 1 ? h - 1 : 1));
}

void m3_cheat_add(unsigned bit, const char *label)
{
    if (bit < 32u) g_cheat_label[bit] = label;
}
const char *m3_cheat_label(unsigned bit) { return bit < 32u ? g_cheat_label[bit] : NULL; }
uint32_t m3_cheats_local(void) { return g_cheats_local; }
void m3_cheats_set_local(uint32_t bits) { g_cheats_local = bits & ~g_cheat_actions; }

void m3_cheat_add_action(unsigned bit, const char *label)
{
    if (bit < 32u) { g_cheat_label[bit] = label; g_cheat_actions |= 1u << bit; }
}
int m3_cheat_is_action(unsigned bit) { return bit < 32u && ((g_cheat_actions >> bit) & 1u); }
void m3_cheat_pulse(unsigned bit) { if (bit < 32u) g_cheat_pulse |= 1u << bit; }

static unsigned getopt4(uint32_t all, unsigned slot) { return (all >> (4u * slot)) & 15u; }
static uint32_t setopt4(uint32_t all, unsigned slot, unsigned v)
{
    return (all & ~(15u << (4u * slot))) | ((v & 15u) << (4u * slot));
}

void m3_option_add(unsigned slot, const char *label, const char *const *choices,
                   unsigned n, unsigned dflt)
{
    if (slot >= 8u) return;
    g_opt[slot].label = label; g_opt[slot].choices = choices; g_opt[slot].n = n;
    /* Settings loaded before the game registered win; else the default. */
    if (!((g_options_saved >> slot) & 1u) || getopt4(g_options_local, slot) >= n)
        g_options_local = setopt4(g_options_local, slot, dflt);
}
const char *m3_option_label(unsigned slot) { return slot < 8u ? g_opt[slot].label : NULL; }
const char *m3_option_choice(unsigned slot, unsigned i)
{
    return (slot < 8u && i < g_opt[slot].n) ? g_opt[slot].choices[i] : NULL;
}
unsigned m3_option_count(unsigned slot) { return slot < 8u ? g_opt[slot].n : 0; }
unsigned m3_option_local(unsigned slot) { return slot < 8u ? getopt4(g_options_local, slot) : 0; }
void m3_option_set_local(unsigned slot, unsigned v)
{
    if (slot < 8u) g_options_local = setopt4(g_options_local, slot, v);
}
unsigned m3_option(unsigned slot) { return slot < 8u ? getopt4(g_cur.options, slot) : 0; }
uint32_t m3_options_local_all(void) { return g_options_local; }
void m3_options_set_local_all(uint32_t v) { g_options_local = v; g_options_saved = 0xFFu; }

static unsigned long env_ul(const char *name, unsigned long dflt)
{
    const char *e = getenv(name);
    return e ? strtoul(e, NULL, 0) : dflt;
}

/* Scripted input for harnesses, the same whatever the platform:
 *
 *   M3_BUTTONS       a mask held for the whole run
 *   M3_COIN_AT       field at which to press coin 1 for a moment
 *   M3_START_AT      likewise start 1 (with the trigger: this game wants a shot)
 *   M3_PRESS_FIELDS  how long "a moment" is, 8 fields by default
 *   M3_GUN_X/Y       pin player 1's gun to raw board coordinates
 *   M3_FIRE_EVERY    pull player 1's trigger for 4 fields every N
 *   M3_PRESS         field:mask[,field:mask...] -- press buttons for a moment
 *                    at those fields, e.g. a walk through the test menu
 *   M3_CHEATS        a mask of the game's cheats to start with
 *   M3_OPTIONS       the game options, four bits a slot
 *
 * A coin slot counts an edge, not a level, which is why the timed ones
 * exist: a mask held from the first field may be seen once or not at all. */
static int g_scripted;   /* a script drives player 1: the host's own controls are ignored */

static void scripted(m3_input_t *in, uint64_t f)
{
    static int probed;
    static unsigned long held, coin_at, start_at, press, gx, gy, fire;
    static unsigned long at[64], mask[64];
    static int npress;
    int k;
    if (!probed) {
        probed = 1;
        held = env_ul("M3_BUTTONS", 0);
        coin_at = env_ul("M3_COIN_AT", 0);
        start_at = env_ul("M3_START_AT", 0);
        press = env_ul("M3_PRESS_FIELDS", 8);
        gx = env_ul("M3_GUN_X", 0);
        gy = env_ul("M3_GUN_Y", 0);
        fire = env_ul("M3_FIRE_EVERY", 0);
        {
            const char *e = getenv("M3_PRESS");
            while (e && *e && npress < 64) {
                char *end;
                at[npress] = strtoul(e, &end, 0);
                if (*end != ':') break;
                mask[npress++] = strtoul(end + 1, &end, 0);
                e = *end == ',' ? end + 1 : end;
            }
        }
        g_scripted = held || coin_at || start_at || gx || gy || fire || npress;
    }
    if (fire && f % fire < 4)
        in->buttons |= M3_BTN_TRIG1;
    in->buttons |= (uint32_t)held;
    if (coin_at && f >= coin_at && f < coin_at + press)
        in->buttons |= M3_BTN_COIN1;
    if (start_at && f >= start_at && f < start_at + press)
        in->buttons |= M3_BTN_START1 | M3_BTN_TRIG1;
    for (k = 0; k < npress; k++)
        if (f >= at[k] && f < at[k] + press)
            in->buttons |= (uint32_t)mask[k];
    if (gx) in->gun_x[0] = (uint16_t)(gx & 0x3FFu);
    if (gy) in->gun_y[0] = (uint16_t)(gy & 0x3FFu);
}

void m3_input_latch(void)
{
    m3_input_t local;
    uint64_t next = model3recomp_frame_count();   /* the field about to run */

    memset(&local, 0, sizeof local);
    local.gun_x[0] = local.gun_x[1] = (M3_GUN_X0 + M3_GUN_X1) / 2;
    local.gun_y[0] = local.gun_y[1] = (M3_GUN_Y0 + M3_GUN_Y1) / 2;
    /* A scripted run must not also read whoever is at the machine: a mouse
     * moved during a capture would aim the scripted shots, and two runs of
     * one script would part company. */
    scripted(&local, next);
    if (!g_scripted)
        platform_sample(&local);
    {
        static int probed;
        if (!probed) { probed = 1; g_cheats_local |= (uint32_t)env_ul("M3_CHEATS", 0); }
    }
    local.cheats = g_cheats_local | g_cheat_pulse;
    g_cheat_pulse = 0;
    {
        static int probed;
        if (!probed && getenv("M3_OPTIONS")) g_options_local = (uint32_t)env_ul("M3_OPTIONS", 0);
        probed = 1;
    }
    local.options = g_options_local;

    if (!netplay_exchange(&local, &g_cur, next))
        g_cur = local;
}

void input_state(m3_state_t *st)
{
    M3_STATE_VAR(st, g_cur);
}
