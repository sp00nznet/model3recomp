/* model3recomp -- Sega Model 3 hardware runtime for static recompilation.
 *
 * Link a statically recompiled Model 3 game against this and you have a
 * board. Your game project supplies the lifted PowerPC functions and a
 * main(); this library supplies everything they talk to.
 *
 * Title-agnostic: nothing here knows what game it is running.
 */
#ifndef MODEL3RECOMP_H
#define MODEL3RECOMP_H

#include <stdint.h>

#include "model3recomp/ppc.h"
#include "model3recomp/bus.h"
#include "model3recomp/func_table.h"
#include "model3recomp/irq.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Model 3 came in Steps. They differ in CPU clock, graphics revision and how
 * data reaches the Real3D; Step 1.x uses the 53C810 SCSI DMA engine for it. */
typedef enum {
    M3_STEP_1_0 = 0x10,
    M3_STEP_1_5 = 0x15,
    M3_STEP_2_0 = 0x20,
    M3_STEP_2_1 = 0x21
} m3_step_t;

typedef struct m3_config {
    m3_step_t   step;
    m3_roms_t   roms;
    int         width, height;   /* 0,0 -> 496x384, the board's native mode */
    const char *title;
} m3_config_t;

int  model3recomp_init(const m3_config_t *cfg);
void model3recomp_shutdown(void);

/* Frame pacing. These exist and work, but for a game that owns its own loop
 * they are called BY irq_status_read(), not by your main(). Keep a loop over
 * them in main() only as a fallback for a guest whose init returns early. */
int  model3recomp_field_due(void);
void model3recomp_begin_frame(void);
void model3recomp_end_frame(void);

/* Pumps host events and returns 0 when the user has asked to quit. */
int  model3recomp_poll(void);

/* Fields presented since init. */
uint64_t model3recomp_frame_count(void);

/* Called after every field is presented. For screenshots, tracing and
 * conformance capture -- a Model 3 main loop never returns, so this is the
 * only place a host gets a look in. NULL clears it. */
void model3recomp_set_frame_hook(void (*hook)(void));

/* Write the current framebuffer to a binary PPM. Returns 0 on failure. */
int  model3recomp_screenshot(const char *path);

/* A monotonically increasing measure of how much work the guest has done:
 * dispatched calls and device accesses. The headless platform paces fields
 * off it, because there is no wall clock to pace against and lifted code has
 * no instruction count to read. */
extern uint64_t m3_work;

/* Boot. Resets the CPU and enters the guest at its reset vector, which for
 * every Model 3 title is 0xFFF00100. Does not return while the game runs --
 * Model 3 main loops never do. */
void model3recomp_run(void);

#ifdef __cplusplus
}
#endif
#endif /* MODEL3RECOMP_H */
