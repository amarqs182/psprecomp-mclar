/* mc3 — Terminal render backend for Midnight Club: L.A. Remix (USA).
 *
 * Draws the PSP framebuffer as ANSI-colored block characters in the terminal.
 * Guaranteed to work on any terminal (no X11/Wayland needed).
 */

#include "render_terminal.h"

#include <psprecomp/mem.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- terminal state ------------------------------------------------- */

static uint32_t g_fb_addr = 0;
static uint32_t g_fb_stride = 512;

/* Write a string directly to stdout fd 1, bypassing stdio buffering. */
static void stdout_write(const char *s) {
    size_t len = strlen(s);
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(STDOUT_FILENO, s + off, len - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
}

/* ANSI 256-color palette lookup from RGBA8888 */
static int rgba_to_ansi256(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (a < 128) return 232;  /* transparent -> black */
    /* Grayscale ramp */
    if (r == g && g == b) {
        if (r < 8) return 16;
        if (r > 247) return 231;
        return 232 + (r - 8) / 10;
    }
    /* Color cube: 6x6x6 = 216 colors, indices 16..231 */
    int r6 = r * 6 / 256;
    int g6 = g * 6 / 256;
    int b6 = b * 6 / 256;
    return 16 + r6 * 36 + g6 * 6 + b6;
}

/* ---- draw the full framebuffer as ANSI blocks ---------------------- */

static void draw_framebuffer(void) {
    static int draw_count = 0;
    draw_count++;

    const int cols = 60;
    const int rows = 34;
    const int cell_w = 480 / cols;
    const int cell_h = 272 / rows;

    uint8_t row_buf[512 * 4];
    char line[cols * 12 + 1];

    /* Move cursor to top-left */
    stdout_write("\033[H");

    /* Linha de status: número de desenhos e endereço do framebuffer */
    int status_len = snprintf(line, sizeof(line),
        "\033[48;5;232m \033[0m \033[1mMC3 terminal render: frame %d fb=0x%08X\033[0m",
        draw_count, g_fb_addr);
    /* Preencher o restante da linha */
    for (int i = status_len; i < cols * 11; i++) line[i] = ' ';
    line[cols * 11] = '\0';
    stdout_write(line);
    stdout_write("\n");

    for (int ry = 0; ry < rows - 1; ry++) {
        int line_len = 0;
        for (int cx = 0; cx < cols; cx++) {
            int sx = cx * cell_w + cell_w / 2;
            int sy = ry * cell_h + cell_h / 2;

            uint8_t r = 0, g = 0, b = 0, a = 255;

            if (g_fb_addr) {
                psp_mem_read_block(row_buf, g_fb_addr + (uint32_t)sy * g_fb_stride * 4 + (uint32_t)sx * 4, 4);
                r = row_buf[0];
                g = row_buf[1];
                b = row_buf[2];
                a = row_buf[3];
            } else {
                /* Game hasn't set a framebuffer yet — draw a test gradient */
                r = (uint8_t)(sx * 255 / 480);
                g = (uint8_t)(sy * 255 / 272);
                b = 128;
                a = 255;
            }

            int ansi = rgba_to_ansi256(r, g, b, a);
            line_len += snprintf(line + line_len, sizeof(line) - line_len,
                                 "\033[48;5;%dm \033[0m", ansi);
        }
        line[line_len] = '\0';
        stdout_write(line);
        stdout_write("\n");
    }
    stdout_write("\033[0m\n");
}

static int terminal_init(int w, int h) {
    (void)w; (void)h;
    /* Hide cursor, switch to alternate screen */
    fprintf(stdout, "\033[?25l");   /* hide cursor */
    fprintf(stdout, "\033[2J");     /* clear screen */
    fprintf(stdout, "\033[?1049h"); /* alternate screen buffer */
    fflush(stdout);

    /* Draw a test gradient immediately so the user sees something right away */
    draw_framebuffer();

    return 0;
}

static void terminal_shutdown(void) {
    /* Restore cursor and main screen */
    fprintf(stdout, "\033[?25h");   /* show cursor */
    fprintf(stdout, "\033[?1049l"); /* main screen buffer */
    fprintf(stdout, "\033[0m");     /* reset attributes */
    fflush(stdout);
}

static void terminal_set_target(uint32_t addr, uint32_t stride, int fmt) {
    g_fb_addr = addr;
    g_fb_stride = stride;
    (void)fmt;
}

/* ---- draw: no-op after initial frame -------------------------------- */

static void terminal_draw(int prim, const psp_vertex *v, int count) {
    (void)prim;
    (void)v;
    (void)count;

    /* Draw at most ~10 times per second so the terminal isn't flooded.
     * The game calls terminal_draw thousands of times per second via the
     * GE draw path, so we throttle by call count. */
    static int call_counter = 0;
    call_counter++;
    if ((call_counter % 500) != 0)
        return;

    draw_framebuffer();
}

static void terminal_finish(void) {
    /* Nothing to flush */
}

static void terminal_present(void) {
    /* Nothing */
}

/* ---- backend registration ------------------------------------------- */

const psp_render_backend psp_render_terminal = {
    "terminal",
    terminal_init,
    terminal_shutdown,
    terminal_set_target,
    terminal_draw,
    terminal_finish,
    terminal_present,
};
