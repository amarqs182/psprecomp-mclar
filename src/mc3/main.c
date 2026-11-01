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

#if defined(PSPRECOMP_HAVE_GLES)
extern const psp_render_backend psp_render_gles32;
#endif

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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mc3_funcs.h"

extern void psp_recomp_register(void);   /* defined by mc3_register.c */

/* Stub for indirect-call targets that discovery missed. */
static void psp_stub_003F302C(void) { return; }

/* ---- runtime state ------------------------------------------------------ */

static uint8_t *g_module_image;
static uint32_t  g_module_size;
static sigjmp_buf g_watchdog_jmp;
static volatile int g_watchdog_fired = 0;

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

    /* Register indirect-call targets that discovery missed. 0x003F302C is
     * called via jalr from somewhere in the boot path; it's not a function
     * entry discovered by psprecomp, so register a stub that just returns. */
    psp_register(0x003F302Cu, psp_stub_003F302C);
    psp_register(0x003886A8u, psp_stub_003F302C);

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

    /* 5. render backend. Register the GLES 3.2 backend (when available), then
     * select by name. Default is software (reference). A real port uses
     * gles32 on the RK3326. */
#if defined(PSPRECOMP_HAVE_GLES)
    psp_render_register_backend(&psp_render_gles32);
#endif
    psp_render_select(render_name);
    if (psp_render_current()->init(640, 480) != 0) {
        fprintf(stderr, "mc3: render init failed (%s)\n", render_name); return 1;
    }
    fprintf(stderr, "mc3: render backend: %s\n", psp_render_current()->name);

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
    uint64_t max_frames = 300;
    const char *frames_env = getenv("MC3_FRAMES");
    if (frames_env && *frames_env) {
        long v = atol(frames_env);
        if (v > 0) max_frames = (uint64_t)v;
    }
    while (!psp_exit_requested() && !g_watchdog_fired && frames < max_frames) {
        psp_threadman_yield();                    /* let guest threads run */

        /* If the game hasn't submitted any GE commands yet, draw a dummy
         * triangle so we can verify the render backend works. */
        if (psp_ge_command_count() == 0 && !drew_dummy) {
            static const psp_vertex tri[3] = {
                { .x = 160, .y = 60,  .rgba = 0xFFFF0000u }, /* red   */
                { .x = 320, .y = 200, .rgba = 0xFF00FF00u }, /* green */
                { .x = 320, .y = 60,  .rgba = 0xFF0000FFu }, /* blue  */
            };
            const psp_render_backend *be = psp_render_current();
            be->set_target(0x04000000u, 512, 0);   /* VRAM base, 512 stride, 8888 */
            be->draw(PSP_PRIM_TRIANGLES, tri, 3);
            be->finish();
            drew_dummy = 1;
        }

        psp_render_current()->present();          /* show the framebuffer */
        frames++;
    }
    if (frames > 0)
        fprintf(stderr, "mc3: host frame loop ran %llu frames\n",
                (unsigned long long)frames);

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

    psp_render_current()->shutdown();
    psp_mem_free();
    free(image);
    return g_watchdog_fired ? 1 : 0;
}
