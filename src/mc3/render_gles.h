/* mc3 — GLES 3.2 render backend for Midnight Club: L.A. Remix (USA).
 *
 * Implements the psp_render_backend interface from psprecomp using SDL2 +
 * EGL + OpenGL ES 3.2. Primitives arrive assembled (screen-space position +
 * colour) and are drawn directly with GLES. No Vulkan, no desktop GL — this
 * backend targets GLES 3.2 exclusively, as required by the Mali-G31 on the
 * RK3326.
 *
 * The vertex format (psp_vertex) carries position and colour only; there are
 * no texture coordinates in the current interface, so textured primitives are
 * drawn as solid colour. That matches the GE's through-mode output, which is
 * all the software rasterizer currently produces.
 *
 * Build (host): gcc ... -lSDL2 -lGLESv2 -lEGL
 * Build (aarch64/RK3326): needs the target's GLES2/EGL headers and SDL2 in
 * the sysroot; PortMaster's runtime provides the shared libraries.
 */

#ifndef RENDER_GLES_H
#define RENDER_GLES_H

#include <psprecomp/render.h>

/* The GLES 3.2 backend. Registered by the game repo before
 * psp_render_select("gles32") is called. */
extern const psp_render_backend psp_render_gles32;

#endif /* RENDER_GLES_H */
