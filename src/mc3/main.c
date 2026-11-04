/* mc3 — PortMaster static-recompilation host for Midnight Club: L.A. Remix (USA).
 *
 * Per-game host that psprecomp expects a game repo to provide:
 *   1. load the decrypted ELF module (segments at their link addresses)
 *   2. map it, reset the CPU, register every recompiled function
 *   3. initialise the HLE firmware surface and a render backend
 *   4. run module_start as the boot thread (PREAL_THREADS)
 *   5. pump a host frame loop: yield to guest threads, present the framebuffer
 *
 * Nothing here touches game files: the decrypted EBOOT.ELF is supplied by the
 * user at run time (-e). Generated C (mc3_funcs_*.c, mc3_register.c,
 * mc3_imports.c) is produced by `allegrexrecomp emit` and compiled in.
 *
 * Build: see src/mc3/CMakeLists.txt  (build psprecomp with -DPPSPRECOMP_REAL_THREADS=ON)
 */

#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>
#include <psprecomp/render.h>
#include <psprecomp/ctors.h>
#include <psprecomp/recomp_rt.h>

#include <signal.h>
#include <unistd.h>

#include "render_gles.h"
#include "render_gles2.h"
#include "render_sdl.h"
#include "render_terminal.h"

#if defined(PSPRECOMP_HAVE_GLES)
extern const psp_render_backend psp_render_gles32;
extern const psp_render_backend psp_render_gles2;
#endif
extern const psp_render_backend psp_render_sdl2;
extern const psp_render_backend psp_render_terminal;

/* ---- SIGSEGV handler: report last guest function before crashing --------- */
static void on_segv(int sig) {
    (void)sig;
    fprintf(stderr, "\n=== SEGSEGV: last guest fn 0x%08X ===\n", psp_trace_last());
    _exit(1);
}

#include <elf.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <termios.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <errno.h>

#include "mc3_funcs.h"

extern void psp_recomp_register(void);   /* defined by mc3_register.c */

/* Stub for indirect-call targets that discovery missed. */
static void psp_stub_003F302C(void) { return; }
static void psp_stub_0037FB3C(void) { return; }
static void psp_stub_002CF4D0(void) { return; }

/* ---- runtime state ------------------------------------------------------ */

static uint8_t *g_module_image;
static uint32_t  g_module_size;
static sigjmp_buf g_watchdog_jmp;
static volatile int g_watchdog_fired = 0;

/* Direct framebuffer presentation via /dev/fb0 (no SDL dependency). */
static int   g_fb_fd = -1;
static void *g_fb_mem = NULL;
static uint32_t g_fb_w = 0, g_fb_h = 0;

/* ---- small ELF32 loader ------------------------------------------------- */

static int read_exact(int fd, void *buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t r = read(fd, (char *)buf + done, len - done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static uint8_t *load_segments(const char *path, uint32_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return NULL; }

    Elf32_Ehdr eh;
    if (read_exact(fd, &eh, sizeof eh) != 0) { perror("read ehdr"); close(fd); return NULL; }
    if (eh.e_ident[EI_MAG0] != ELFMAG0 || eh.e_ident[EI_MAG1] != ELFMAG1 ||
        eh.e_ident[EI_MAG2] != ELFMAG2 || eh.e_ident[EI_MAG3] != ELFMAG3) {
        fprintf(stderr, "mc3: not an ELF\n"); close(fd); return NULL;
    }
    if (eh.e_ident[EI_CLASS] != ELFCLASS32 || eh.e_ident[EI_DATA] != ELFDATA2LSB) {
        fprintf(stderr, "mc3: not ELF32 little-endian\n"); close(fd); return NULL;
    }
    if (eh.e_machine != EM_MIPS) {
        fprintf(stderr, "mc3: not MIPS (machine 0x%x)\n", eh.e_machine); close(fd); return NULL;
    }

    Elf32_Phdr *ph = calloc(eh.e_phnum, sizeof *ph);
    if (!ph) { perror("calloc"); close(fd); return NULL; }
    if (lseek(fd, eh.e_phoff, SEEK_SET) == (off_t)-1 ||
        read_exact(fd, ph, (size_t)eh.e_phnum * sizeof *ph) != 0) {
        perror("read phdr"); free(ph); close(fd); return NULL;
    }

    uint32_t total = 0;
    for (int i = 0; i < eh.e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > total)
            total = ph[i].p_vaddr + ph[i].p_memsz;

    uint8_t *image = (uint8_t *)calloc(1, total ? total : 1);
    if (!image) { perror("calloc image"); free(ph); close(fd); return NULL; }

    for (int i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (lseek(fd, ph[i].p_offset, SEEK_SET) == (off_t)-1) {
            perror("lseek seg"); free(image); free(ph); close(fd); return NULL;
        }
        uint8_t *dst = image + ph[i].p_vaddr;   /* module links at 0: vaddr == offset */
        if (read_exact(fd, dst, ph[i].p_filesz) != 0) {
            perror("read seg"); free(image); free(ph); close(fd); return NULL;
        }
        fprintf(stderr, "mc3: loaded PT_LOAD @ vaddr 0x%08x filesz 0x%x memsz 0x%x\n",
                ph[i].p_vaddr, ph[i].p_filesz, ph[i].p_memsz);
    }

    free(ph);
    close(fd);
    *out_size = total;
    fprintf(stderr, "mc3: module image 0x%08x bytes\n", total);
    return image;
}

/* ---- RSS ---------------------------------------------------------------- */

static uint64_t peak_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    uint64_t hwm = 0;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            unsigned long v = 0;
            if (sscanf(line + 6, " %lu", &v) == 1) hwm = v;
            break;
        }
    }
    fclose(f);
    return hwm;
}

/* ---- watchdog ----------------------------------------------------------- */

static void on_alarm(int sig) {
    (void)sig;
    g_watchdog_fired = 1;
    siglongjmp(g_watchdog_jmp, 1);
}

/* ---- teclado como controle PSP (para testes via QEMU) ------------------- */

static uint32_t g_kbd_buttons = 0;

static void *ctrl_reader(void *arg) {
    (void)arg;
    struct termios old, new;
    tcgetattr(STDIN_FILENO, &old);
    new = old;
    new.c_lflag &= ~(ICANON | ECHO);
    new.c_cc[VMIN] = 0;
    new.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &new);

    fprintf(stderr,
        "mc3: controle de teclado ativo.\n"
        "  X=cross   C=circle   S=square   T=triangle\n"
        "  Enter=start   Espaço=select   setas=d-pad\n"
        "  q=sair\n"
        "Segure uma tecla para pressionar o botão, solte para liberar.\n");
    fflush(stderr);

    while (!psp_exit_requested()) {
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        if (poll(&pfd, 1, 50) > 0) {
            int c = getchar();
            if (c == 'q' || c == 'Q') break;
            uint32_t btn = 0;
            switch (c) {
                case 'x': case 'X': btn = 0x4000u; break; /* CROSS */
                case 'c': case 'C': btn = 0x2000u; break; /* CIRCLE */
                case 's': case 'S': btn = 0x8000u; break; /* SQUARE */
                case 't': case 'T': btn = 0x1000u; break; /* TRIANGLE */
                case '\n': case '\r': btn = 0x0008u; break; /* START */
                case ' ':         btn = 0x0001u; break; /* SELECT */
                case 'A': case 'a': btn = 0x0080u; break; /* LEFT  */
                case 'D': case 'd': btn = 0x0020u; break; /* RIGHT */
                case 'W': case 'w': btn = 0x0010u; break; /* UP    */
                case 'Z': case 'z': btn = 0x0040u; break; /* DOWN  */
                default:          btn = 0u;           break; /* soltar */
            }
            psp_ctrl_set(btn, 128, 128);
        }
        /* Segura o botão por ~50ms para o jogo conseguir ler */
        usleep(50000);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &old);
    return NULL;
}

/* ---- entry point -------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *elf_path = NULL;
    const char *render_name = "software";
    const char *data_dir = ".";
    int opt;
    while ((opt = getopt(argc, argv, "e:r:d:")) != -1) {
        if (opt == 'e') elf_path = optarg;
        else if (opt == 'r') render_name = optarg;
        else if (opt == 'd') data_dir = optarg;
        else {
            fprintf(stderr, "usage: mc3 -e <EBOOT.ELF> [-r software|gles32|null] [-d <data_dir>]\n");
            return 2;
        }
    }
    if (!elf_path) {
        fprintf(stderr, "mc3: -e <EBOOT.ELF> required\n");
        return 2;
    }

    /* 1. memory */
    if (psp_mem_init() != 0) { fprintf(stderr, "mc3: mem init failed\n"); return 1; }

    uint8_t *image = load_segments(elf_path, &g_module_size);
    if (!image) return 1;
    g_module_image = image;
    if (psp_mem_map_module(0, g_module_size) != 0) {
        fprintf(stderr, "mc3: map module failed\n"); return 1;
    }

    /* 2. HLE firmware surface */
    psp_hle_init();
    /* The guest sees the disc as "disc0:/". The host root holds one
     * subdirectory per device prefix (disc/, ms/, flash/, host/), so the
     * game finds disc0:/PSP_GAME/USRDIR/PCODE.TXT under <data_dir>/disc/.
     * Without this the game spins in a retry loop waiting for a file that
     * is not there. */
    psp_io_set_root(data_dir);

    /* 3. register every recompiled function (+ interior labels) */
    psp_recomp_register();

    /* Register indirect-call targets that discovery missed. */
    psp_register(0x003F302Cu, psp_stub_003F302C);
    psp_register(0x003886A8u, psp_stub_003F302C);
    /* These functions are called via jalr from discovered code but were not
     * themselves discovered as function entries. They are real functions in
     * the ELF, not jump-table entries -- see allegrexrecomp dis output. */
    psp_register(0x0037FB3Cu, psp_stub_0037FB3C);   /* Thread 2 entry */
    psp_register(0x002CF4D0u, psp_stub_002CF4D0);   /* Thread 5 entry */

    /* 3b. Register the boot-to-GE chain for hit counting. The static call
     * graph says module_start reaches the GE/Display init through
     * 0x5A4 -> 0x6AC -> 0x8 -> 0x2DAE78 -> 0x1EC -> 0x137538 -> 0x2EFE48
     * -> 0x2B0378 -> 0x2A8C50, but a static graph cannot say which link
     * fails at runtime. Counting each link shows exactly where it stops.
     *
     * The chain actually starts in the game's own thread: module_start calls
     * sceKernelCreateThread with entry 0x000006B8 and starts it. */
    {
        static const uint32_t watch[] = {
            0x000005A4u, 0x000006B8u, 0x00000718u, 0x00000748u, 0x00000788u,
            0x000007C0u, 0x00000008u, 0x002DAE78u, 0x000001ECu, 0x000002E8u,
            0x00137538u, 0x002EFE48u, 0x002B0378u, 0x002A8C50u, 0x002A8B64u,
            0x002A8E14u, 0x002A8F04u, 0x002A918Cu, 0x003806D0u, 0x003807ACu,
        };
        psp_trace_watch_multi(watch, (int)(sizeof(watch) / sizeof(watch[0])));
    }

    /* 4. CPU — set initial stack pointer into user RAM (the module links at
     * address 0 and expects $sp to be set by the kernel; our loader doesn't
     * run the kernel, so we seed it here). gp comes from the module info
     * (.rodata.sceModuleInfo reports gp=0x7FF0). */
    psp_cpu_reset();
    psp_cpu.r[PSP_REG_SP] = 0x09FFF000u;   /* initial stack, in user RAM */
    psp_cpu.r[PSP_REG_GP] = 0x00007FF0u;   /* module gp_value */

    /* 5. render backend. Register the GLES backends (when available), then
     * select by name. Default is software (reference). A real port uses
     * gles2 on the RK3326 if GLES 3.2 is unavailable. */
#if defined(PSPRECOMP_HAVE_GLES)
    psp_render_register_backend(&psp_render_gles32);
    psp_render_register_backend(&psp_render_gles2);
#endif
    psp_render_register_backend(&psp_render_sdl2);
    psp_render_register_backend(&psp_render_terminal);
    psp_render_select(render_name);
    if (psp_render_current()->init(640, 480) != 0) {
        fprintf(stderr, "mc3: render init failed (%s)\n", render_name); return 1;
    }
    fprintf(stderr, "mc3: render backend: %s\n", psp_render_current()->name);

    /* Open /dev/fb0 for direct framebuffer presentation (software backend only).
     * The software rasterizer draws into guest memory (g_ge.fbp); we copy
     * it to /dev/fb0 each host frame so the game appears on the R36S screen.
     * SDL2 backend does not use /dev/fb0 - it renders to an SDL window. */
    if (strcmp(psp_render_current()->name, "software") == 0) {
        g_fb_fd = open("/dev/fb0", O_RDWR);
        if (g_fb_fd < 0) {
            fprintf(stderr, "mc3: cannot open /dev/fb0: %s\n", strerror(errno));
            return 1;
        }
        struct fb_var_screeninfo vinfo;
        if (ioctl(g_fb_fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
            fprintf(stderr, "mc3: FBIOGET_VSCREENINFO failed: %s\n", strerror(errno));
            return 1;
        }
        g_fb_w = vinfo.xres;
        g_fb_h = vinfo.yres;
        size_t fb_size = vinfo.xres * vinfo.yres * (vinfo.bits_per_pixel / 8);
        g_fb_mem = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb_fd, 0);
        if (g_fb_mem == MAP_FAILED) {
            fprintf(stderr, "mc3: mmap /dev/fb0 failed: %s\n", strerror(errno));
            return 1;
        }
        fprintf(stderr, "mc3: fb0 %ux%u @ %p\n", g_fb_w, g_fb_h, g_fb_mem);
    }

    /* 6. threading: main becomes the boot thread and runs module entry.
     * Guest threads (created by sceKernelCreateThread) run when module entry
     * parks at a frame wait. Requires psprecomp built with
     * PPSPRECOMP_REAL_THREADS=ON. */
    signal(SIGSEGV, on_segv);
    psp_threadman_reset();

    /* 7. watchdog: 30 s of guest time, then longjmp back so we still report.
     * The bring-up runs need longer than that to get past the boot sequence
     * on the RK3326, so the timeout is overridable from the environment. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigaction(SIGALRM, &sa, NULL);
    int watchdog_secs = 30;
    const char *wd_env = getenv("MC3_WATCHDOG");
    if (wd_env && *wd_env) {
        int v = atoi(wd_env);
        if (v > 0) watchdog_secs = v;
    }
    alarm((unsigned)watchdog_secs);

    /* 8. run module_start (entry 0x000005A4) as the boot thread.
     * On a real PSP the kernel passes threadargs_size in $a0 and an arg
     * buffer pointer in $a1. The game's loader thread checks $a0 > 0 before
     * creating the main game threads, so we must pass non-zero $a0. */
    psp_cpu.r[PSP_REG_A0] = 4u;
    psp_cpu.r[PSP_REG_A1] = 0u;
    fprintf(stderr, "mc3: entering module_start @ 0x000005A4 (boot thread)\n");
    if (sigsetjmp(g_watchdog_jmp, 1) == 0) {
        psp_func_000005A4();
        fprintf(stderr, "mc3: module_start returned\n");
    } else {
        fprintf(stderr, "mc3: watchdog fired (module still running)\n");
    }
    alarm(0);

    /* 8.5. Create the game's main thread at entry 0x000000D8 (Part 2 of
     * function at vaddr 8 -- the delay loop that calls
     * sceKernelDelayThreadCB + sceCtrlPeekBufferPositive). The loader thread
     * created by module_start only does init and returns; it never calls
     * sceKernelCreateThread itself, so we do it here on the host side. */
    {
        psp_cpu.r[PSP_REG_A0] = 0u;           /* name */
        psp_cpu.r[PSP_REG_A1] = 0x000000D8u;  /* entry */
        psp_cpu.r[PSP_REG_A2] = 8u;           /* priority */
        psp_cpu.r[PSP_REG_A3] = 0x10000u;      /* stackSize */
        psp_write32(psp_cpu.r[PSP_REG_SP] - 16, 0u);  /* attr */
        psp_write32(psp_cpu.r[PSP_REG_SP] - 12, 0u);  /* option */
        psp_hle_call(0x446D8DE6u);             /* sceKernelCreateThread */
        uint32_t thid = psp_cpu.r[PSP_REG_V0];
        fprintf(stderr, "mc3: created main thread uid=%u\n", thid);

        psp_cpu.r[PSP_REG_A0] = thid;
        psp_cpu.r[PSP_REG_A1] = 0u;
        psp_cpu.r[PSP_REG_A2] = 0u;
        psp_hle_call(0xF475845Du);             /* sceKernelStartThread */
        fprintf(stderr, "mc3: started main thread\n");
    }

    /* 8.6. Create the remaining threads that the game creates via
     * sceKernelCreateThread. The loader thread never reaches those calls, so
     * we create them here on the host side. Entry points and parameters were
     * extracted from the disassembly of the sceKernelCreateThread call sites
     * in mc3_funcs_384.c, _393.c, _480.c, _482.c, _486.c. */
    {
        uint32_t thid;
        /* Thread 1: entry 0x003F302C, prio 16, stack 2048 */
        psp_cpu.r[PSP_REG_A0] = 0u;
        psp_cpu.r[PSP_REG_A1] = 0x003F302Cu;
        psp_cpu.r[PSP_REG_A2] = 16u;
        psp_cpu.r[PSP_REG_A3] = 2048u;
        psp_write32(psp_cpu.r[PSP_REG_SP] - 16, 0u);
        psp_write32(psp_cpu.r[PSP_REG_SP] - 12, 0u);
        psp_hle_call(0x446D8DE6u);
        thid = psp_cpu.r[PSP_REG_V0];
        fprintf(stderr, "mc3: created thread uid=%u entry=0x%08X prio=16 stack=2048\n", thid);
        psp_cpu.r[PSP_REG_A0] = thid;
        psp_cpu.r[PSP_REG_A1] = 0u;
        psp_cpu.r[PSP_REG_A2] = 0u;
        psp_hle_call(0xF475845Du);
        fprintf(stderr, "mc3: started thread uid=%u\n", thid);
    }
    {
        uint32_t thid;
        /* Thread 2: entry 0x0037FB3C, prio 16, stack 32768 */
        psp_cpu.r[PSP_REG_A0] = 0u;
        psp_cpu.r[PSP_REG_A1] = 0x0037FB3Cu;
        psp_cpu.r[PSP_REG_A2] = 16u;
        psp_cpu.r[PSP_REG_A3] = 32768u;
        psp_write32(psp_cpu.r[PSP_REG_SP] - 16, 0u);
        psp_write32(psp_cpu.r[PSP_REG_SP] - 12, 0u);
        psp_hle_call(0x446D8DE6u);
        thid = psp_cpu.r[PSP_REG_V0];
        fprintf(stderr, "mc3: created thread uid=%u entry=0x%08X prio=16 stack=32768\n", thid);
        psp_cpu.r[PSP_REG_A0] = thid;
        psp_cpu.r[PSP_REG_A1] = 0u;
        psp_cpu.r[PSP_REG_A2] = 0u;
        psp_hle_call(0xF475845Du);
        fprintf(stderr, "mc3: started thread uid=%u\n", thid);
    }
    {
        uint32_t thid;
        /* Thread 3: entry 0x00382110, prio 17, stack 8192 */
        psp_cpu.r[PSP_REG_A0] = 0u;
        psp_cpu.r[PSP_REG_A1] = 0x00382110u;
        psp_cpu.r[PSP_REG_A2] = 17u;
        psp_cpu.r[PSP_REG_A3] = 8192u;
        psp_write32(psp_cpu.r[PSP_REG_SP] - 16, 0u);
        psp_write32(psp_cpu.r[PSP_REG_SP] - 12, 0u);
        psp_hle_call(0x446D8DE6u);
        thid = psp_cpu.r[PSP_REG_V0];
        fprintf(stderr, "mc3: created thread uid=%u entry=0x%08X prio=17 stack=8192\n", thid);
        psp_cpu.r[PSP_REG_A0] = thid;
        psp_cpu.r[PSP_REG_A1] = 0u;
        psp_cpu.r[PSP_REG_A2] = 0u;
        psp_hle_call(0xF475845Du);
        fprintf(stderr, "mc3: started thread uid=%u\n", thid);
    }
    {
        uint32_t thid;
        /* Thread 4: entry 0x00382518, prio 18, stack 2048 */
        psp_cpu.r[PSP_REG_A0] = 0u;
        psp_cpu.r[PSP_REG_A1] = 0x00382518u;
        psp_cpu.r[PSP_REG_A2] = 18u;
        psp_cpu.r[PSP_REG_A3] = 2048u;
        psp_write32(psp_cpu.r[PSP_REG_SP] - 16, 0u);
        psp_write32(psp_cpu.r[PSP_REG_SP] - 12, 0u);
        psp_hle_call(0x446D8DE6u);
        thid = psp_cpu.r[PSP_REG_V0];
        fprintf(stderr, "mc3: created thread uid=%u entry=0x%08X prio=18 stack=2048\n", thid);
        psp_cpu.r[PSP_REG_A0] = thid;
        psp_cpu.r[PSP_REG_A1] = 0u;
        psp_cpu.r[PSP_REG_A2] = 0u;
        psp_hle_call(0xF475845Du);
        fprintf(stderr, "mc3: started thread uid=%u\n", thid);
    }
    {
        uint32_t thid;
        /* Thread 5: entry 0x003817D0, prio 16, stack 2048 */
        psp_cpu.r[PSP_REG_A0] = 0u;
        psp_cpu.r[PSP_REG_A1] = 0x003817D0u;
        psp_cpu.r[PSP_REG_A2] = 16u;
        psp_cpu.r[PSP_REG_A3] = 2048u;
        psp_write32(psp_cpu.r[PSP_REG_SP] - 16, 0u);
        psp_write32(psp_cpu.r[PSP_REG_SP] - 12, 0u);
        psp_hle_call(0x446D8DE6u);
        thid = psp_cpu.r[PSP_REG_V0];
        fprintf(stderr, "mc3: created thread uid=%u entry=0x%08X prio=16 stack=2048\n", thid);
        psp_cpu.r[PSP_REG_A0] = thid;
        psp_cpu.r[PSP_REG_A1] = 0u;
        psp_cpu.r[PSP_REG_A2] = 0u;
        psp_hle_call(0xF475845Du);
        fprintf(stderr, "mc3: started thread uid=%u\n", thid);
    }
    {
        uint32_t thid;
        /* Thread 6: entry 0x003886A8, prio 16, stack 4096 */
        psp_cpu.r[PSP_REG_A0] = 0u;
        psp_cpu.r[PSP_REG_A1] = 0x003886A8u;
        psp_cpu.r[PSP_REG_A2] = 16u;
        psp_cpu.r[PSP_REG_A3] = 4096u;
        psp_write32(psp_cpu.r[PSP_REG_SP] - 16, 0u);
        psp_write32(psp_cpu.r[PSP_REG_SP] - 12, 0u);
        psp_hle_call(0x446D8DE6u);
        thid = psp_cpu.r[PSP_REG_V0];
        fprintf(stderr, "mc3: created thread uid=%u entry=0x%08X prio=16 stack=4096\n", thid);
        psp_cpu.r[PSP_REG_A0] = thid;
        psp_cpu.r[PSP_REG_A1] = 0u;
        psp_cpu.r[PSP_REG_A2] = 0u;
        psp_hle_call(0xF475845Du);
        fprintf(stderr, "mc3: started thread uid=%u\n", thid);
    }

    /* 9. host frame loop — after module_start returns, keep pumping frames so
     * guest threads get CPU time and the GE output can be presented. Each
     * iteration yields to guest threads (they submit GE commands), then
     * presents the accumulated framebuffer. If the game never submits GE
     * commands (stuck in boot loop), draw a dummy coloured triangle so the
     * backend is exercised and something appears on screen. */
    uint64_t frames = 0;
    int      drew_dummy = 0;
    uint64_t max_frames = 100000000;
    uint64_t frames_since_ge = 0;
    const char *frames_env = getenv("MC3_FRAMES");
    if (frames_env && *frames_env) {
        long v = atol(frames_env);
        if (v > 0) max_frames = (uint64_t)v;
    }

    fprintf(stderr, "mc3: entering host frame loop\n");
    fflush(stderr);

    /* Thread de leitura de teclado para controle do jogo (QEMU).
     * Desabilitado temporariamente devido a SEGSEGV com threads reais.
    pthread_t ctrl_tid;
    fprintf(stderr, "mc3: before pthread_create(ctrl_reader)\n");
    fflush(stderr);
    pthread_create(&ctrl_tid, NULL, ctrl_reader, NULL);
    fprintf(stderr, "mc3: after pthread_create(ctrl_reader)\n");
    fflush(stderr);
    */
    fprintf(stderr, "mc3: ctrl_reader disabled\n");
    fflush(stderr);

    /* Auto-envio de CROSS periodicamente para desbloquear o jogo no QEMU /
     * R36s sem pad físico detectado via gptokeyb. Enviado a cada 60 frames
     * (~1s) para permitir que o jogo avance nos menus. */

    /* No automatic exit — the game runs until the user closes the window
     * or presses Ctrl+C. The GE stall watchdog was removed because the
     * game legitimately pauses between GE command bursts. */
    (void)frames_since_ge;

    /* Draw something every frame so the user always sees the window is
     * alive. For the SDL2 backend this shows a test gradient until the
     * game sets a framebuffer, then the real PSP framebuffer. For other
     * backends this falls back to their present(). */
    while (!psp_exit_requested() && !g_watchdog_fired && frames < max_frames) {
        psp_threadman_yield();                    /* let guest threads run */

        /* Yield to the OS periodically so signals (SIGALRM watchdog) are
         * delivered and the host frame loop can make progress even when a
         * guest thread spins busy without calling yield. */
        if ((frames % 100) == 0)
            usleep(1000);

        /* Saída de emergência: SELECT + START juntos encerra o jogo.
         * SELECT = 0x0001, START = 0x0008. */
        if ((psp_ctrl_get_buttons() & 0x0009u) == 0x0009u) {
            fprintf(stderr, "mc3: EXIT requested via SELECT+START\n");
            break;
        }

        /* Tecla ESC encerra o jogo imediatamente. */
        if (psp_exit_requested()) {
            fprintf(stderr, "mc3: EXIT requested\n");
            break;
        }

        /* Auto-envio de CROSS periodicamente depois do frame 120 para
         * desbloquear o jogo no QEMU / R36s sem pad físico. Enviado a cada
         * 60 frames (~1s) para permitir que o jogo avance nos menus. */
        if (frames > 120 && (frames % 60) == 0) {
            psp_ctrl_set(0x4000u, 128, 128);      /* CROSS held */
            fprintf(stderr, "mc3: auto-ctrl CROSS held at frame %llu\n",
                    (unsigned long long)frames);
        }

        /* Draw the current frame (test gradient or real framebuffer) every
         * frame so something is always visible on screen. */
        sdl2_draw_frame();

        /* Log GE command count changes for debugging */
        {
            static uint64_t last_ge_count = 0;
            uint64_t current_ge_count = psp_ge_command_count();
            if (current_ge_count != last_ge_count) {
                fprintf(stderr, "mc3: GE commands %llu -> %llu at frame %llu\n",
                        (unsigned long long)last_ge_count,
                        (unsigned long long)current_ge_count,
                        (unsigned long long)frames);
                last_ge_count = current_ge_count;
            }
        }

        /* Copy the GE framebuffer from guest memory to /dev/fb0. */
        {
            uint32_t fb_addr = psp_render_get_fb_addr();
            uint32_t fb_stride = psp_render_get_fb_stride();
            uint32_t row_bytes = 480 * 4;  /* 480 pixels * 4 bytes (RGBA8888) */
            if (fb_addr && g_fb_mem) {
                uint8_t *dst = (uint8_t *)g_fb_mem;
                for (int y = 0; y < 272 && y < (int)g_fb_h; y++) {
                    psp_mem_read_block(dst + (uint32_t)y * g_fb_w * 4,
                                       fb_addr + (uint32_t)y * fb_stride * 4,
                                       row_bytes);
                }
            }
        }

        frames++;
        if ((frames % 300) == 0) {
            fprintf(stderr, "mc3: frame %llu, GE commands=%llu, GE vertices=%llu, GE pixels=%llu\n",
                    (unsigned long long)frames,
                    (unsigned long long)psp_ge_command_count(),
                    (unsigned long long)psp_ge_vertex_count(),
                    (unsigned long long)psp_render_pixels());
        }
    }

    /* ---- report --------------------------------------------------------- */
    fprintf(stderr, "\n=== mc3 report ===\n");
    fprintf(stderr, "GE commands:   %llu\n", (unsigned long long)psp_ge_command_count());
    fprintf(stderr, "GE vertices:   %llu\n", (unsigned long long)psp_ge_vertex_count());
    fprintf(stderr, "GE pixels:     %llu  <-- >0 means the GE drew\n",
            (unsigned long long)psp_render_pixels());
    fprintf(stderr, "host frames:   %llu\n", (unsigned long long)frames);
    fprintf(stderr, "dispatch:      %u registered, %llu misses\n",
            psp_dispatch_count(), (unsigned long long)psp_dispatch_misses());
    fprintf(stderr, "HLE unimplemented calls: %llu (logged %llu)\n",
            (unsigned long long)psp_hle_unimplemented_calls(),
            (unsigned long long)psp_hle_unimplemented_logged());
    fprintf(stderr, "bad memory accesses: %llu\n",
            (unsigned long long)psp_mem_bad_access);
    fprintf(stderr, "threadman: switches=%llu parks=%llu created=%llu started=%llu\n",
            (unsigned long long)psp_threadman_switches(),
            (unsigned long long)psp_threadman_parks(),
            (unsigned long long)psp_threadman_created(),
            (unsigned long long)psp_threadman_started());
    fprintf(stderr, "loop back-edge: addr=0x%08x hits=%llu\n",
            psp_trace_loop_addr(), (unsigned long long)psp_trace_loop_hits());
    fprintf(stderr, "peak RSS:      %llu KiB\n",
            (unsigned long long)peak_rss_kb());
    fprintf(stderr, "=================\n");

    /* The boot-to-GE call chain, from module_start down to the function that
     * calls sceGeListEnqueue / sceDisplaySetFrameBuf. Each link is reported
     * with its hit count, so a run that stops early shows exactly which link
     * was not reached.
     *
     * The real chain runs in the *game's own thread*, not in module_start:
     * module_start creates a thread with entry 0x000006B8 (the arg-parsing
     * wrapper: $s3 = threadargs_size, $s2 = argp) and starts it. That entry
     * then walks 0x6B8 -> 0x718 -> 0x748 -> 0x788 -> 0x7C0 -> 0x8 -> ... The
     * static graph showed the chain but not that it starts at a thread entry. */
    {
        static const uint32_t chain[] = {
            0x000005A4u, /* module_start                                  */
            0x000006B8u, /* game main thread entry (created by module_start)*/
            0x00000718u, /* 0x6B8 -> 0x718  (arg parse loop)              */
            0x00000748u, /* 0x718 -> 0x748  (arg parse done)              */
            0x00000788u, /* 0x748 -> 0x788                                 */
            0x000007C0u, /* 0x788 -> 0x7C0                                 */
            0x00000008u, /* 0x7C0 -> 0x8  (thread body)                   */
            0x002DAE78u, /* 0x8 -> 0x2DAE78                               */
            0x000001ECu, /* 0x2DAE78 -> 0x1EC                             */
            0x000002E8u, /* 0x1EC -> 0x2E8                               */
            0x00137538u, /* 0x2E8 -> 0x137538                            */
            0x002EFE48u, /* 0x137538 -> 0x2EFE48                         */
            0x002B0378u, /* 0x2EFE48 -> 0x2B0378                         */
            0x002A8C50u, /* 0x2B0378 -> 0x2A8C50  (GE + Display init)    */
            0x002A8B64u, /* per-frame GE submit                          */
            0x002A8E14u, /* GE list enqueue                               */
            0x002A8F04u, /* GE list enqueue                               */
            0x002A918Cu, /* Display set frame buf                         */
            0x003806D0u, /* Display                                       */
            0x003807ACu, /* Display                                       */
        };
        fprintf(stderr, "\n=== boot -> GE chain hit counts ===\n");
        for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); i++)
            fprintf(stderr, "  %08X -> %llu\n", chain[i],
                    (unsigned long long)psp_trace_watch_count(chain[i]));
        /* The last N functions entered, in order. This shows the path the run
         * actually took -- the hit counts only say which links ran. */
        psp_trace_dump();
    }

    /* Framebuffer cleanup. */
    if (g_fb_mem) munmap(g_fb_mem, (size_t)g_fb_w * (size_t)g_fb_h * 4u);
    if (g_fb_fd >= 0) close(g_fb_fd);

    psp_render_current()->shutdown();
    psp_mem_free();
    free(image);
    return g_watchdog_fired ? 1 : 0;
}
