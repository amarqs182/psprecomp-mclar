/* mc3 — GLES 2.0 render backend for Midnight Club: L.A. Remix (USA).\n *
 * Implements the psp_render_backend interface from psprecomp using SDL2 +\n * EGL + OpenGL ES 2.0. Primitives arrive assembled (screen-space position +\n * colour) and are drawn directly with GLES 2.0. No Vulkan, no desktop GL.\n *
 * The vertex format (psp_vertex) carries position and colour only; there are\n * no texture coordinates in the current interface, so textured primitives are\n * drawn as solid colour. That matches the GE's through-mode output, which is\n * all the software rasterizer currently produces.\n *
 * Display: the PSP's 480x272 (~16:9) is letterboxed into a 640x363 content\n * area within the 640x480 window (58px top/bottom bars), per the project's\n * letterbox-default rule. No stretch-to-fill.\n *
 * Build (host): gcc ... -lSDL2 -lGLESv2 -lEGL\n * Build (aarch64/RK3326): needs the target's GLES2/EGL headers and SDL2 in\n * the sysroot; PortMaster's runtime provides the shared libraries at run time.\n */

#ifndef RENDER_GLES2_H
#define RENDER_GLES2_H

#include <psprecomp/render.h>

/* The GLES 2.0 backend. Registered by the game repo before
 * psp_render_select("gles2") is called. */
extern const psp_render_backend psp_render_gles2;

#endif /* RENDER_GLES2_H */
