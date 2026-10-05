/* model3recomp -- host platform layer (window, input, timing, presentation). */
#ifndef MODEL3RECOMP_PLATFORM_H
#define MODEL3RECOMP_PLATFORM_H

#include <stdint.h>
#include "model3recomp/input.h"

#ifdef __cplusplus
extern "C" {
#endif

int      platform_init(int w, int h, const char *title);
void     platform_shutdown(void);
int      platform_poll(void);          /* 0 when the user wants to quit */
void     platform_present(void);
uint64_t platform_ticks_us(void);

/* Sample the host's own controls into a record (input.h): buttons for
 * player 1 and, with a second device, player 2, and the guns in board
 * coordinates. The record arrives with the guns centred and nothing
 * pressed; a platform with no controls leaves it alone. Called once per
 * field. */
void platform_sample(m3_input_t *in);

/* A field's audio: frames of interleaved stereo 16-bit at 44.1 kHz. */
void platform_audio(const int16_t *lr, int frames);

/* A line of status -- netplay, save states -- for the title bar. */
void platform_set_status(const char *s);

/* The framebuffer renderers draw into: w*h pixels, 0xAARRGGBB. */
uint32_t *platform_framebuffer(int *w, int *h);

#ifdef __cplusplus
}
#endif
#endif
