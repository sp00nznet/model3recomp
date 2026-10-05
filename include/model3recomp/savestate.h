/* model3recomp -- save states.
 *
 * A statically recompiled game keeps half its state on the host's call
 * stack, so a snapshot cannot be taken just anywhere: restore it somewhere
 * else and the C frames below no longer match the guest. It can be taken at
 * a safe point -- a guest address the game passes through once a field,
 * from the same place every time, with nothing interesting on the stack.
 * For The Lost World that is the head of its main loop.
 *
 * A request (menu, hotkey) is only noted; the snapshot or the restore
 * happens the next time the guest reaches the safe point, which the lifted
 * code reports through M3_FN (lift.h). Restoring there and carrying on
 * resumes the guest exactly where the snapshot left it.
 */
#ifndef MODEL3RECOMP_SAVESTATE_H
#define MODEL3RECOMP_SAVESTATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The guest address of the safe point, 0xFFFFFFFF for none (no states). */
extern uint32_t m3_safepoint_pc;
void m3_safepoint(void);

void m3_state_request_save(int slot);
void m3_state_request_load(int slot);
int  m3_state_exists(int slot);

/* Each board module hands its state to one of these. */
typedef struct m3_state m3_state_t;
void m3_state_io(m3_state_t *st, void *p, size_t n);
#define M3_STATE_VAR(st, v) m3_state_io((st), &(v), sizeof(v))

void bus_state(m3_state_t *st);
void irq_state(m3_state_t *st);
void scsi_state(m3_state_t *st);
void ppc_state(m3_state_t *st);
void real3d_state(m3_state_t *st);
void model3recomp_state(m3_state_t *st);
void input_state(m3_state_t *st);
void sound_state(m3_state_t *st);

#ifdef __cplusplus
}
#endif
#endif
