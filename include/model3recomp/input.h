/* model3recomp -- the cabinet's inputs, one record per field.
 *
 * The board reads its buttons and light guns many times in a field. They are
 * latched once, at the field boundary, and every read in the next field sees
 * the same record. That is what a lockstep netplay session needs -- both
 * machines must hand the guest identical inputs on identical fields -- and it
 * also makes a scripted run independent of how often the guest happens to
 * look.
 */
#ifndef MODEL3RECOMP_INPUT_H
#define MODEL3RECOMP_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cabinet buttons, both players. A set bit means pressed; the board's
 * registers are active low and bus.c inverts. */
#define M3_BTN_COIN1    0x0001u
#define M3_BTN_COIN2    0x0002u
#define M3_BTN_TEST     0x0004u
#define M3_BTN_SERVICE  0x0008u
#define M3_BTN_START1   0x0010u
#define M3_BTN_START2   0x0020u
#define M3_BTN_TRIG1    0x0040u   /* light gun trigger                    */
#define M3_BTN_TRIG2    0x0080u
#define M3_BTN_OFFSCR1  0x0100u   /* pointed off screen, which is reload  */
#define M3_BTN_OFFSCR2  0x0200u

/* The light gun board reports ten bits a coordinate. Where the visible
 * screen falls in them was measured from where The Lost World draws its
 * crosshair: pinned to X 200/250/400/550/600 it lands at pixel 52, 101,
 * 248, 395, 444, and to Y 120/150/300/400/420 at 41, 70, 220, 319, 339 --
 * straight lines that put the screen's edges at X 147..652, Y 79..464. */
#define M3_GUN_X0 147
#define M3_GUN_X1 652
#define M3_GUN_Y0 79
#define M3_GUN_Y1 464

typedef struct m3_input {
    uint32_t buttons;           /* M3_BTN_* */
    uint16_t gun_x[2], gun_y[2];/* board units, players 1 and 2 */
    uint32_t cheats;            /* game-defined bits, see m3_cheat_* */
    uint32_t options;           /* game options, four bits each, see m3_option_* */
} m3_input_t;

/* The record the guest sees this field. */
const m3_input_t *m3_input(void);

/* Latch the next field's record: sample the host, add scripted input, and
 * trade with a netplay peer if there is one. model3recomp_end_frame() calls
 * this; nothing else should. */
void m3_input_latch(void);

/* Screen pixel (0..w-1, 0..h-1) to gun coordinates. */
void m3_gun_from_screen(int px, int py, int w, int h,
                        uint16_t *gx, uint16_t *gy);

/* Cheats a game offers. The host side of a netplay session decides them, so
 * they travel in the input record and both machines apply the same ones. A
 * game registers its cheats with m3_cheat_add() and reads them back from
 * m3_input()->cheats. */
void        m3_cheat_add(unsigned bit, const char *label);
const char *m3_cheat_label(unsigned bit);   /* NULL if not registered */
uint32_t    m3_cheats_local(void);
void        m3_cheats_set_local(uint32_t bits);

/* A one-shot cheat -- "add credits" rather than "infinite health": the
 * menu item sets its bit for a single field. */
void        m3_cheat_add_action(unsigned bit, const char *label);
int         m3_cheat_is_action(unsigned bit);
void        m3_cheat_pulse(unsigned bit);

/* Game options: a choice per slot (0..7), four bits each in the record,
 * so on a netplay session the host's choice holds on both machines. The
 * window saves them with its other settings. A game reads its choice with
 * m3_option(slot). */
void        m3_option_add(unsigned slot, const char *label,
                          const char *const *choices, unsigned n, unsigned dflt);
const char *m3_option_label(unsigned slot);  /* NULL if not registered */
const char *m3_option_choice(unsigned slot, unsigned i);
unsigned    m3_option_count(unsigned slot);
unsigned    m3_option_local(unsigned slot);
void        m3_option_set_local(unsigned slot, unsigned v);
unsigned    m3_option(unsigned slot);         /* as the guest sees it this field */
uint32_t    m3_options_local_all(void);
void        m3_options_set_local_all(uint32_t v); /* from saved settings */

#ifdef __cplusplus
}
#endif
#endif
