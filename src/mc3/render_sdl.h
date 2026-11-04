#ifndef RENDER_SDL_H
#define RENDER_SDL_H

#include <psprecomp/render.h>

/* The SDL2 software render backend. Registered by the game repo before
 * psp_render_select("sdl2") is called. */
extern const psp_render_backend psp_render_sdl2;

/* Draw the current framebuffer (or a test gradient if the game hasn't set
 * one yet) to the SDL window. Called from the host frame loop so something
 * is always visible on screen. */
extern void sdl2_draw_frame(void);

/* Present loop: runs sdl2_draw_frame() at a fixed ~60 FPS until
 * psp_exit_requested() returns true. Intended to be called from a dedicated
 * present thread so presentation is independent of guest execution. */
extern void sdl2_present_loop(void);

#endif /* RENDER_SDL_H */
