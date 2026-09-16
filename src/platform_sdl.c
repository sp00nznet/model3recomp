/* model3recomp -- SDL2 platform layer. */
#include "model3recomp/platform.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static uint32_t     *g_fb;
static int           g_w, g_h;

int platform_init(int w, int h, const char *title)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "[model3recomp] SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    g_w = w; g_h = h;
    g_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, w * 2, h * 2,
                             SDL_WINDOW_RESIZABLE);
    if (!g_win) return 0;
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren) return 0;
    /* Letterbox rather than stretch: the board's pixels are not square, but
     * they are not 2:1 either, and stretching hides aspect bugs. */
    SDL_RenderSetLogicalSize(g_ren, w, h);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g_tex) return 0;
    g_fb = calloc((size_t)w * h, sizeof(uint32_t));
    return g_fb != NULL;
}

void platform_shutdown(void)
{
    free(g_fb); g_fb = NULL;
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
}

uint32_t *platform_framebuffer(int *w, int *h)
{
    if (w) *w = g_w;
    if (h) *h = g_h;
    return g_fb;
}

int platform_poll(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) return 0;
    }
    return 1;
}

void platform_present(void)
{
    SDL_UpdateTexture(g_tex, NULL, g_fb, g_w * (int)sizeof(uint32_t));
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
}

uint64_t platform_ticks_us(void)
{
    /* SDL_GetPerformanceCounter, not SDL_GetTicks: a 17.4 ms field needs
     * better than millisecond resolution or the pacing judders. */
    static uint64_t freq;
    if (!freq) freq = SDL_GetPerformanceFrequency();
    return SDL_GetPerformanceCounter() * 1000000ull / freq;
}
