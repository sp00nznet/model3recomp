/* model3recomp -- host platform layer (window, input, timing, presentation). */
#ifndef MODEL3RECOMP_PLATFORM_H
#define MODEL3RECOMP_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int      platform_init(int w, int h, const char *title);
void     platform_shutdown(void);
int      platform_poll(void);          /* 0 when the user wants to quit */
void     platform_present(void);
uint64_t platform_ticks_us(void);

/* The framebuffer renderers draw into: w*h pixels, 0xAARRGGBB. */
uint32_t *platform_framebuffer(int *w, int *h);

#ifdef __cplusplus
}
#endif
#endif
