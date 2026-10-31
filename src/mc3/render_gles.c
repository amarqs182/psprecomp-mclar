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
 * Display: the PSP's 480x272 (~16:9) is letterboxed into a 640x363 content
 * area within the 640x480 window (58px top/bottom bars), per the project's
 * letterbox-default rule. No stretch-to-fill.
 *
 * Build (host): gcc ... -lSDL2 -lGLESv2 -lEGL
 * Build (aarch64/RK3326): needs the target's GLES2/EGL headers and SDL2 in
 * the sysroot; PortMaster's runtime provides the shared libraries at run time.
 */

#include "render_gles.h"

#if defined(PSPRECOMP_HAVE_GLES)

#include <psprecomp/mem.h>

#include <SDL2/SDL.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- window / EGL state ------------------------------------------------- */

static SDL_Window *g_window = NULL;
static SDL_GLContext g_glcontext = NULL;
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLContext g_egl_context = EGL_NO_CONTEXT;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;

/* ---- shader / program --------------------------------------------------- */

static GLuint g_vertex_shader = 0;
static GLuint g_fragment_shader = 0;
static GLuint g_program = 0;
static GLint g_loc_pos = -1;
static GLint g_loc_color = -1;

/* ---- VBO ---------------------------------------------------------------- */

static GLuint g_vbo = 0;

/* ---- framebuffer target (GE_FBP/GE_FBW) --------------------------------- */

static uint32_t g_fb_addr = 0;
static uint32_t g_fb_stride = 0;

/* ---- window / content geometry ------------------------------------------ */

static int g_window_w = 640;
static int g_window_h = 480;

/* Letterbox: PSP 480x272 -> content 640x363 within 640x480.
 * Top/bottom bars: (480 - 363) / 2 = 58 (top), 59 (bottom). */
static int g_viewport_x = 0;
static int g_viewport_y = 58;
static int g_viewport_w = 640;
static int g_viewport_h = 363;

/* ---- shaders ------------------------------------------------------------ */

static const char *vertex_shader_src =
    "#version 300 es\n"
    "in vec2 a_pos;\n"
    "in vec4 a_color;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "    v_color = a_color;\n"
    "}\n";

static const char *fragment_shader_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 v_color;\n"
    "out vec4 frag_color;\n"
    "void main() {\n"
    "    frag_color = v_color;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "gles32: shader compile failed:\n%s\n", log);
    }
    return s;
}

/* ---- backend entry points ----------------------------------------------- */

static int gles_init(int w, int h) {
    g_window_w = w;
    g_window_h = h;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "gles32: SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    g_window = SDL_CreateWindow("Midnight Club: L.A. Remix",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                w, h,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!g_window) {
        fprintf(stderr, "gles32: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    g_glcontext = SDL_GL_CreateContext(g_window);
    if (!g_glcontext) {
        fprintf(stderr, "gles32: SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_GL_MakeCurrent(g_window, g_glcontext);

    g_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl_display == EGL_NO_DISPLAY) {
        fprintf(stderr, "gles32: eglGetDisplay failed\n");
        return -1;
    }
    EGLint major, minor;
    if (!eglInitialize(g_egl_display, &major, &minor)) {
        fprintf(stderr, "gles32: eglInitialize failed\n");
        return -1;
    }
    g_egl_context = eglGetCurrentContext();
    g_egl_surface = eglGetCurrentSurface(EGL_DRAW);

    /* Shaders */
    g_vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_src);
    g_fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_src);

    g_program = glCreateProgram();
    glAttachShader(g_program, g_vertex_shader);
    glAttachShader(g_program, g_fragment_shader);
    glLinkProgram(g_program);
    GLint link_ok = 0;
    glGetProgramiv(g_program, GL_LINK_STATUS, &link_ok);
    if (!link_ok) {
        char log[1024];
        glGetProgramInfoLog(g_program, sizeof(log), NULL, log);
        fprintf(stderr, "gles32: program link failed:\n%s\n", log);
        return -1;
    }

    g_loc_pos = glGetAttribLocation(g_program, "a_pos");
    g_loc_color = glGetAttribLocation(g_program, "a_color");

    glGenBuffers(1, &g_vbo);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glViewport(g_viewport_x, g_viewport_y, g_viewport_w, g_viewport_h);

    fprintf(stderr,
            "gles32: initialized %dx%d window, content %dx%d at (%d,%d), GLES %s\n",
            w, h, g_viewport_w, g_viewport_h, g_viewport_x, g_viewport_y,
            glGetString(GL_VERSION));
    return 0;
}

static void gles_shutdown(void) {
    if (g_vbo) { glDeleteBuffers(1, &g_vbo); g_vbo = 0; }
    if (g_program) { glDeleteProgram(g_program); g_program = 0; }
    if (g_fragment_shader) { glDeleteShader(g_fragment_shader); g_fragment_shader = 0; }
    if (g_vertex_shader) { glDeleteShader(g_vertex_shader); g_vertex_shader = 0; }
    if (g_glcontext) { SDL_GL_DeleteContext(g_glcontext); g_glcontext = NULL; }
    if (g_window) { SDL_DestroyWindow(g_window); g_window = NULL; }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

/* Map one PSP screen-space vertex to NDC. X: [0,480]->[-1,1]. Y: [0,272]->[-1,1] flipped. */
static void map_vertex(int x, int y, float *ox, float *oy) {
    *ox = (x / 480.0f) * 2.0f - 1.0f;
    *oy = -((y / 272.0f) * 2.0f - 1.0f);
}

static void rgba_to_float(uint32_t rgba, float c[4]) {
    c[0] = ((rgba >> 16) & 0xFF) / 255.0f;   /* R */
    c[1] = ((rgba >> 8)  & 0xFF) / 255.0f;   /* G */
    c[2] = ( rgba        & 0xFF) / 255.0f;   /* B */
    c[3] = ((rgba >> 24) & 0xFF) / 255.0f;   /* A */
}

/* Max vertices we might emit: sprites expand 2 verts -> 6 verts. */
#define MAX_DRAW_VERT 4096

static void gles_draw(int prim, const psp_vertex *v, int count) {
    if (!v || count < 1) return;

    glUseProgram(g_program);
    glEnableVertexAttribArray((GLuint)g_loc_pos);
    glEnableVertexAttribArray((GLuint)g_loc_color);

    /* Expand into a flat vertex buffer. Max: sprites expand 2 verts -> 6 verts. */
    float pos[MAX_DRAW_VERT * 2];
    float col[MAX_DRAW_VERT * 4];
    int n = 0;

    if (prim == PSP_PRIM_SPRITES) {
        for (int i = 0; i + 1 < count; i += 2) {
            const psp_vertex *a = &v[i];
            const psp_vertex *b = &v[i + 1];
            int x0 = a->x < b->x ? a->x : b->x;
            int x1 = a->x > b->x ? a->x : b->x;
            int y0 = a->y < b->y ? a->y : b->y;
            int y1 = a->y > b->y ? a->y : b->y;
            uint32_t rgba = b->rgba;   /* software backend uses b->rgba for whole sprite */

            float c[4];
            rgba_to_float(rgba, c);

            /* Two triangles: (x0,y0),(x1,y0),(x0,y1) and (x1,y0),(x1,y1),(x0,y1) */
            const float sv[6][2] = {
                {(float)x0, (float)y0}, {(float)x1, (float)y0}, {(float)x0, (float)y1},
                {(float)x1, (float)y0}, {(float)x1, (float)y1}, {(float)x0, (float)y1},
            };
            for (int j = 0; j < 6 && n < MAX_DRAW_VERT; j++) {
                map_vertex((int)sv[j][0], (int)sv[j][1], &pos[n*2], &pos[n*2+1]);
                col[n*4] = c[0]; col[n*4+1] = c[1]; col[n*4+2] = c[2]; col[n*4+3] = c[3];
                n++;
            }
        }
    } else {
        for (int i = 0; i < count && n < MAX_DRAW_VERT; i++) {
            map_vertex(v[i].x, v[i].y, &pos[n*2], &pos[n*2+1]);
            float c[4];
            rgba_to_float(v[i].rgba, c);
            col[n*4] = c[0]; col[n*4+1] = c[1]; col[n*4+2] = c[2]; col[n*4+3] = c[3];
            n++;
        }
    }

    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, (size_t)(n * 2 * sizeof(float) + n * 4 * sizeof(float)),
                 NULL, GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (size_t)(n * 2 * sizeof(float)), pos);
    glBufferSubData(GL_ARRAY_BUFFER, (size_t)(n * 2 * sizeof(float)),
                    (size_t)(n * 4 * sizeof(float)), col);

    glVertexAttribPointer((GLuint)g_loc_pos, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glVertexAttribPointer((GLuint)g_loc_color, 4, GL_FLOAT, GL_FALSE, 0,
                          (void *)(size_t)(n * 2 * sizeof(float)));

    GLenum mode;
    switch (prim) {
    case PSP_PRIM_TRIANGLES:      mode = GL_TRIANGLES; break;
    case PSP_PRIM_TRIANGLE_STRIP: mode = GL_TRIANGLE_STRIP; break;
    case PSP_PRIM_TRIANGLE_FAN:   mode = GL_TRIANGLE_FAN; break;
    case PSP_PRIM_POINTS:         mode = GL_POINTS; break;
    case PSP_PRIM_LINES:          mode = GL_LINES; break;
    case PSP_PRIM_LINE_STRIP:     mode = GL_LINE_STRIP; break;
    case PSP_PRIM_SPRITES:        mode = GL_TRIANGLES; break; /* already expanded */
    default:                      mode = GL_TRIANGLES; break;
    }

    glDrawArrays(mode, 0, n);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisableVertexAttribArray((GLuint)g_loc_pos);
    glDisableVertexAttribArray((GLuint)g_loc_color);
    glUseProgram(0);
}

static void gles_finish(void) {
    glFlush();
}

static void gles_present(void) {
    SDL_GL_SwapWindow(g_window);
}

static void gles_set_target(uint32_t addr, uint32_t stride, int fmt) {
    g_fb_addr = addr;
    g_fb_stride = stride;
    (void)fmt;
}

const psp_render_backend psp_render_gles32 = {
    "gles32",
    gles_init,
    gles_shutdown,
    gles_set_target,
    gles_draw,
    gles_finish,
    gles_present,
};

#else /* !PSPRECOMP_HAVE_GLES */

/* Stub backend when SDL2/EGL/GLESv2 are not available. Init always fails so
 * the game falls back to the software backend. */
#include <psprecomp/render.h>
#include <stdio.h>

static int gles_stub_init(int w, int h) { (void)w; (void)h; return -1; }
static void gles_stub_shutdown(void) { }
static void gles_stub_set_target(uint32_t a, uint32_t s, int f) { (void)a; (void)s; (void)f; }
static void gles_stub_draw(int p, const psp_vertex *v, int n) { (void)p; (void)v; (void)n; }
static void gles_stub_finish(void) { }
static void gles_stub_present(void) { }

const psp_render_backend psp_render_gles32 = {
    "gles32",
    gles_stub_init,
    gles_stub_shutdown,
    gles_stub_set_target,
    gles_stub_draw,
    gles_stub_finish,
    gles_stub_present,
};

#endif /* PSPRECOMP_HAVE_GLES */
