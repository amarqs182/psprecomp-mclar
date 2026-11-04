/* mc3 — SDL2 software render backend for Midnight Club: L.A. Remix (USA).
 *
 * Implements the psp_render_backend interface using plain SDL2 (no EGL,
 * no GLES). The GE framebuffer lives in emulated PSP memory (guest VRAM).
 * Each frame we read it back and blit it to an SDL2 window, letterboxed
 * to preserve the PSP's 480x272 aspect ratio within the host window.
 *
 * Build: gcc ... -lSDL2
 */

#include "render_sdl.h"

#include <psprecomp/mem.h>
#include <psprecomp/hle.h>

#include <psprecomp/mem.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* include the global framebuffer state */
#include <psprecomp/render.h>

/* ---- window state ------------------------------------------------- */

static SDL_Window *g_window = NULL;
static SDL_Renderer *g_renderer = NULL;
static SDL_Texture *g_tex = NULL;

/* Window size (host) */
static int g_window_w = 1280;
static int g_window_h = 960;

/* Letterbox: PSP 480x272 content centered in the host window */
static int g_viewport_x = 0;
static int g_viewport_y = 0;
static int g_viewport_w = 960;
static int g_viewport_h = 540;

/* ---- shader-less drawing: we just blit the framebuffer texture ------ */

static int gles_init(int w, int h) {
    g_window_w = w;
    g_window_h = h;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "sdl2: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Give the compositor a moment to be ready before creating the window,
     * so it is mapped and visible. */
    usleep(500000);  /* 500 ms */

    g_window = SDL_CreateWindow(
        "MIDNIGHT CLUB: L.A. REMIX - PSP Static Recomp",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        g_window_w, g_window_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        fprintf(stderr, "sdl2: SDL_CreateWindow failed: %s\n", SDL_GetError());
        fprintf(stderr, "sdl2: Try SDL_VIDEODRIVER=x11 if on Wayland\n");
        return -1;
    }

    /* Force window to the front on Wayland/X11 */
    SDL_RaiseWindow(g_window);
    SDL_SetWindowInputFocus(g_window);
    SDL_ShowWindow(g_window);

    fprintf(stderr, "sdl2: window raised, focused and shown\n");

    g_renderer = SDL_CreateRenderer(g_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) {
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
        if (!g_renderer) {
            fprintf(stderr, "sdl2: SDL_CreateRenderer failed: %s\n", SDL_GetError());
            return -1;
        }
    }

    /* Create texture matching PSP framebuffer: 480x272 RGBA8888 */
    g_tex = SDL_CreateTexture(g_renderer,
        SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING,
        480, 272);
    if (!g_tex) {
        fprintf(stderr, "sdl2: SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }

    /* Compute letterbox for 480x272 content in window */
    float content_aspect = 480.0f / 272.0f;
    float window_aspect = (float)g_window_w / (float)g_window_h;

    if (window_aspect > content_aspect) {
        /* Window is wider than content: letterbox left/right */
        g_viewport_h = g_window_h;
        g_viewport_w = (int)(g_viewport_h * content_aspect);
        g_viewport_x = (g_window_w - g_viewport_w) / 2;
        g_viewport_y = 0;
    } else {
        /* Window is taller than content: letterbox top/bottom */
        g_viewport_w = g_window_w;
        g_viewport_h = (int)(g_viewport_w / content_aspect);
        g_viewport_x = 0;
        g_viewport_y = (g_window_h - g_viewport_h) / 2;
    }

    fprintf(stderr,
        "sdl2: initialized %dx%d window, content %dx%d at (%d,%d)\n",
        g_window_w, g_window_h, g_viewport_w, g_viewport_h,
        g_viewport_x, g_viewport_y);

    /* Draw an immediate test gradient so the user sees SOMETHING on screen
     * right away, before the game starts submitting GE commands. */
    {
        void *pixels = NULL;
        int pitch = 0;
        if (SDL_LockTexture(g_tex, NULL, &pixels, &pitch) == 0) {
            uint8_t *dst = (uint8_t *)pixels;
            for (int y = 0; y < 272; y++) {
                for (int x = 0; x < 480; x++) {
                    dst[(y * 480 + x) * 4 + 0] = (uint8_t)(x * 255 / 480);   /* R */
                    dst[(y * 480 + x) * 4 + 1] = (uint8_t)(y * 255 / 272);   /* G */
                    dst[(y * 480 + x) * 4 + 2] = 128;                          /* B */
                    dst[(y * 480 + x) * 4 + 3] = 255;                          /* A */
                }
            }
            SDL_UnlockTexture(g_tex);
            SDL_RenderClear(g_renderer);
            SDL_Rect dst_rect = { g_viewport_x, g_viewport_y, g_viewport_w, g_viewport_h };
            SDL_RenderCopy(g_renderer, g_tex, NULL, &dst_rect);
            SDL_RenderPresent(g_renderer);
            fprintf(stderr, "sdl2: test gradient drawn to window\n");

    /* Move window to current workspace and focus it on Hyprland,
     * so the user actually sees it. Silently ignored on non-Hyprland. */
    system("/home/amarqs/mc3proj/build-native/mc3_hyprctl.sh 2>/dev/null");
        }
    }
    return 0;
}

static void gles_shutdown(void) {
    if (g_tex) { SDL_DestroyTexture(g_tex); g_tex = NULL; }
    if (g_renderer) { SDL_DestroyRenderer(g_renderer); g_renderer = NULL; }
    if (g_window) { SDL_DestroyWindow(g_window); g_window = NULL; }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void gles_set_target(uint32_t addr, uint32_t stride, int fmt) {
    /* Update the global framebuffer state so main.c can read it */
    psp_render_set_target(addr, stride);
    (void)fmt;
}

/* ---- draw: copy PSP framebuffer to SDL texture --------------------- */

static void gles_draw(int prim, const psp_vertex *v, int count) {
    if (!g_tex || !g_renderer)
        return;

    uint32_t fb_addr = psp_render_get_fb_addr();
    uint32_t fb_stride = psp_render_get_fb_stride();

    /* Rasterize sprites into guest VRAM so the SDL2 blit has something
     * to show. Without this the VRAM stays blank and the screen is black.
     *
     * Sprites (type=6) submit one vertex per sprite: position + colour.
     * Draw each vertex as a small filled square so the game's actual
     * geometry is visible on screen. */
    if (fb_addr && fb_stride && prim == PSP_PRIM_SPRITES) {
        for (int i = 0; i < count; i++) {
            int x0 = v[i].x - 2;
            int y0 = v[i].y - 2;
            int x1 = v[i].x + 2;
            int y1 = v[i].y + 2;
            uint32_t rgba = v[i].rgba;

            /* DEBUG: game sends sprites at (0,0) with rgba=0 (loading screen).
             * Make them visible: move (0,0) sprites to screen centre and
             * enlarge them so the user can see the game is actually drawing.
             * Remove this once the game sends real geometry. */
            if (v[i].x == 0 && v[i].y == 0) {
                x0 = 240 - 8; y0 = 136 - 8;
                x1 = 240 + 8; y1 = 136 + 8;
                if (rgba == 0x00000000u) rgba = 0xFFFF0000u; /* red opaque */
            }

            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > 480) x1 = 480;
            if (y1 > 272) y1 = 272;

            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    psp_write32(fb_addr + (uint32_t)(y * (int)fb_stride + x) * 4, rgba);
                }
            }
        }
    }

    /* Lock texture */
    void *pixels = NULL;
    int pitch = 0;
    if (SDL_LockTexture(g_tex, NULL, &pixels, &pitch) != 0) {
        fprintf(stderr, "sdl2: SDL_LockTexture failed: %s\n", SDL_GetError());
        return;
    }

    if (!psp_render_get_fb_addr()) {
        /* Game hasn't set a framebuffer yet — draw a test gradient so the
         * user can see the window is working. */
        uint8_t *dst = (uint8_t *)pixels;
        for (int y = 0; y < 272; y++) {
            for (int x = 0; x < 480; x++) {
                dst[(y * 480 + x) * 4 + 0] = (uint8_t)(x * 255 / 480);   /* R gradient */
                dst[(y * 480 + x) * 4 + 1] = (uint8_t)(y * 255 / 272);   /* G gradient */
                dst[(y * 480 + x) * 4 + 2] = 128;                          /* B constant */
                dst[(y * 480 + x) * 4 + 3] = 255;                          /* A opaque */
            }
        }
    } else {
        /* Copy real PSP framebuffer from guest VRAM */
        uint8_t row[512 * 4];
        uint8_t *dst = (uint8_t *)pixels;
        for (int y = 0; y < 272; y++) {
            psp_mem_read_block(row, psp_render_get_fb_addr() + (uint32_t)y * psp_render_get_fb_stride() * 4, 512 * 4);
            for (int x = 0; x < 480; x++) {
                dst[(y * 480 + x) * 4 + 0] = row[x * 4 + 0];  /* R */
                dst[(y * 480 + x) * 4 + 1] = row[x * 4 + 1];  /* G */
                dst[(y * 480 + x) * 4 + 2] = row[x * 4 + 2];  /* B */
                dst[(y * 480 + x) * 4 + 3] = row[x * 4 + 3];  /* A */
            }
        }
    }

    SDL_UnlockTexture(g_tex);

    /* Render */
    SDL_RenderClear(g_renderer);
    SDL_Rect dst_rect = {
        g_viewport_x,
        g_viewport_y,
        g_viewport_w,
        g_viewport_h
    };
    SDL_RenderCopy(g_renderer, g_tex, NULL, &dst_rect);
    SDL_RenderPresent(g_renderer);
}

void sdl2_draw_frame(void) {
    if (!g_tex || !g_renderer)
        return;

    /* Process SDL events — detect window close (SDL_QUIT) so the game
     * can exit cleanly when the user closes the window. */
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            psp_exit_requested_set(1);
            return;
        }
    }

    /* Lock texture */
    void *pixels = NULL;
    int pitch = 0;
    if (SDL_LockTexture(g_tex, NULL, &pixels, &pitch) != 0) {
        return;
    }

    if (!psp_render_get_fb_addr()) {
        /* Game hasn't set a framebuffer yet — draw a test gradient so the
         * user can see the window is working. */
        uint8_t *dst = (uint8_t *)pixels;
        for (int y = 0; y < 272; y++) {
            for (int x = 0; x < 480; x++) {
                dst[(y * 480 + x) * 4 + 0] = (uint8_t)(x * 255 / 480);   /* R gradient */
                dst[(y * 480 + x) * 4 + 1] = (uint8_t)(y * 255 / 272);   /* G gradient */
                dst[(y * 480 + x) * 4 + 2] = 128;                          /* B constant */
                dst[(y * 480 + x) * 4 + 3] = 255;                          /* A opaque */
            }
        }
    } else {
        /* Framebuffer definido pelo jogo — copiar a VRAM real do PSP.
         * Antes mostrávamos o gradiente quando psp_render_pixels()==0, mas o
         * backend SDL2 não conta pixels escritos na VRAM; use fb_addr != 0
         * como sinal de que o jogo já configurou o framebuffer. */
        uint32_t fb_addr = psp_render_get_fb_addr();
        uint32_t fb_stride = psp_render_get_fb_stride();
        /* Copy real PSP framebuffer from guest VRAM */
        uint8_t row[512 * 4];
        uint8_t *dst = (uint8_t *)pixels;
        for (int y = 0; y < 272; y++) {
            psp_mem_read_block(row, fb_addr + (uint32_t)y * fb_stride * 4, 512 * 4);
            for (int x = 0; x < 480; x++) {
                dst[(y * 480 + x) * 4 + 0] = row[x * 4 + 0];  /* R */
                dst[(y * 480 + x) * 4 + 1] = row[x * 4 + 1];  /* G */
                dst[(y * 480 + x) * 4 + 2] = row[x * 4 + 2];  /* B */
                dst[(y * 480 + x) * 4 + 3] = row[x * 4 + 3];  /* A */
            }
        }
    }

    SDL_UnlockTexture(g_tex);

    /* Render */
    SDL_RenderClear(g_renderer);
    SDL_Rect dst_rect = {
        g_viewport_x,
        g_viewport_y,
        g_viewport_w,
        g_viewport_h
    };
    SDL_RenderCopy(g_renderer, g_tex, NULL, &dst_rect);
    SDL_RenderPresent(g_renderer);
}

static void gles_finish(void) {
    /* Nothing to flush - we draw immediately */
}

static void gles_present(void) {
    /* Already presented in gles_draw */
}

/* ---- backend registration --------------------------------------------- */

const psp_render_backend psp_render_sdl2 = {
    "sdl2",
    gles_init,
    gles_shutdown,
    gles_set_target,
    gles_draw,
    gles_finish,
    gles_present,
};
