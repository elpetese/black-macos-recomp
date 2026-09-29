/**
 * Black (Xbox, 2006) - static recompilation host, POSIX (macOS / Linux).
 *
 * Same boot sequence as templates/new-game/src/main.c (the Windows host),
 * with the Win32-only pieces replaced:
 *   WinMain + MessageBox       -> main + stderr
 *   VEH + dbghelp              -> sigaction(SIGSEGV/SIGBUS) + dladdr
 *   Windows path separators    -> '/'
 *
 * XBE (from xbe_parser):
 *   Title: Black   Title ID: 0x45410083   XDK 5849
 *   Base 0x00010000   Entry 0x00025A3F   115 kernel imports
 *
 * Usage:  black-recomp [game_dir]
 *   game_dir holds default.xbe and the disc data (chars/, data/, levels/ ...).
 *   Default: $BLACK_GAME_DIR, else ./game
 */
#define _GNU_SOURCE        /* dladdr on glibc */
#include <signal.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach/mach.h>     /* arm_thread_state64_get_pc */
#endif

static void f7_signal_handler(int sig) {
    (void)sig;
    extern void f7_print_counts(void);
    f7_print_counts();
    _exit(0);
}

#include <xbox/xboxrecomp.h>
#include "recomp_icall_feedback.h"   /* compiles away unless the feature is on */
#include "mcpx_mmio.h"
#include "apu.h"
#include "ohci.h"
#include "xinput_xbox.h"
#include "video_player.h"
#include "keyboard_input.h"
#include "nv2a/nv2a_mmio_hook.h"

typedef struct NV2AState NV2AState;
uint64_t nv2a_mmio_read(NV2AState *d, uint64_t addr, unsigned int size);
void nv2a_mmio_write(NV2AState *d, uint64_t addr, uint64_t val, unsigned int size);

static uint64_t nv2a_region_read(void *ctx, uint32_t off, unsigned n)
{
    return nv2a_mmio_read((NV2AState *)ctx, (uint64_t)off, n);
}
static void nv2a_region_write(void *ctx, uint32_t off, uint64_t v, unsigned n)
{
    nv2a_mmio_write((NV2AState *)ctx, (uint64_t)off, v, n);
}

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;
extern uint32_t g_xbox_code_lo, g_xbox_code_hi;

#define BLACK_ENTRY_POINT 0x00025A3Fu

extern void xbe_entry_point(void);     /* generated (labelled by tools.disasm) */
extern int  recomp_dispatch_init(void); /* generated */

/* ── Crash handler ─────────────────────────────────────────── */

static uintptr_t host_pc(void *uctx)
{
    ucontext_t *uc = (ucontext_t *)uctx;
#if defined(__APPLE__) && defined(__aarch64__)
#  if defined(arm_thread_state64_get_pc)
    return (uintptr_t)arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
#  else
    return (uintptr_t)uc->uc_mcontext->__ss.__pc;
#  endif
#elif defined(__APPLE__) && defined(__x86_64__)
    return (uintptr_t)uc->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__aarch64__)
    return (uintptr_t)uc->uc_mcontext.pc;
#elif defined(__linux__) && defined(__x86_64__)
    return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
    (void)uc; return 0;
#endif
}

/* async-signal-safety is knowingly ignored: the process is about to die and
 * the diagnostics are worth more than the risk. */
static void crash_handler(int sig, siginfo_t *si, void *uctx)
{
    uintptr_t fault = (uintptr_t)si->si_addr;
    uintptr_t pc    = host_pc(uctx);
    uintptr_t off   = (uintptr_t)g_xbox_mem_offset;

    /* Intercepted MCPX registers fault on purpose; resume once emulated. */
    if (xbox_mcpx_mmio_fault(uctx, fault))
        return;

    xbox_KeyboardRestoreTerminal();      /* leave the shell usable */
    {
        extern void xbox_FramebufferReleaseMouse(void) __attribute__((weak));
        if (xbox_FramebufferReleaseMouse)
            xbox_FramebufferReleaseMouse();   /* and the cursor free */
    }
    fprintf(stderr, "\n[CRASH] %s at host pc=%p, fault addr=%p\n",
            sig == SIGBUS ? "SIGBUS" : "SIGSEGV", (void *)pc, (void *)fault);
    if (off && fault >= off && fault - off <= 0xFFFFFFFFu)
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n", (uint32_t)(fault - off));
    else
        fprintf(stderr, "  fault is outside the guest address space (host pointer bug)\n");
    fprintf(stderr, "  Xbox regs: eax=%08X ecx=%08X edx=%08X esp=%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
    fprintf(stderr, "  Xbox regs: ebx=%08X esi=%08X edi=%08X\n", g_ebx, g_esi, g_edi);

    Dl_info di;
    if (pc && dladdr((void *)pc, &di) && di.dli_sname)
        fprintf(stderr, "  in %s+0x%lX\n", di.dli_sname,
                (unsigned long)(pc - (uintptr_t)di.dli_saddr));

    if (off && g_esp) {
        const uint32_t *sp = (const uint32_t *)(off + g_esp);
        int shown = 0;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (int i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* RenderWare's memory interface: sub_00086590 tail-jumps through
         * [0x2C28C4], so a failing allocator shows up here as the function it
         * actually dispatches to. */
        fprintf(stderr, "  RW memory fns: malloc=[0x2C28C4]=0x%08X free=[0x2C28C0]=0x%08X"
                        " realloc=[0x2C28C8]=0x%08X calloc=[0x2C28CC]=0x%08X\n",
                *(const uint32_t *)(off + 0x2C28C4), *(const uint32_t *)(off + 0x2C28C0),
                *(const uint32_t *)(off + 0x2C28C8), *(const uint32_t *)(off + 0x2C28CC));
        /* [0x2C023C] is RenderWare's "registry bootstrap in progress" flag.
         * While it is set, sub_00081B70 skips substituting the default parent
         * and therefore links nothing into the type registry. It must be 0
         * once sub_00081C90 has finished. */
        fprintf(stderr, "  RW registry: bootstrap_flag[0x2C023C]=0x%08X default_parent[0x5754DC]=0x%08X"
                        " head[0x2C01A0]=0x%08X\n",
                *(const uint32_t *)(off + 0x2C023C), *(const uint32_t *)(off + 0x5754DC),
                *(const uint32_t *)(off + 0x2C01A0));
        /* The factory sub_000830D0 calls the type's constructor callback at
         * type+0x28 and returns NULL when it does. Type 0x2C2BB8 is the first
         * one that fails, and everything downstream is collateral. */
        fprintf(stderr, "  RW type 0x2C2BB8: ctor[+0x28]=0x%08X  field[+0x3C]=0x%08X"
                        "  field[+0x40]=0x%08X field[+0x48]=0x%08X\n",
                *(const uint32_t *)(off + 0x2C2BB8 + 0x28),
                *(const uint32_t *)(off + 0x2C2BB8 + 0x3C),
                *(const uint32_t *)(off + 0x2C2BB8 + 0x40),
                *(const uint32_t *)(off + 0x2C2BB8 + 0x48));
        /* DirectSound's internal pool object. sub_0021B65A returns NULL for
         * every allocation while this is 0, which is where the whole null
         * chain that ends in sub_00084F90 begins. */
        fprintf(stderr, "  DSOUND pool: heap[0x224C44]=0x%08X freelist[0x224CD0]=0x%08X\n",
                *(const uint32_t *)(off + 0x224C44), *(const uint32_t *)(off + 0x224CD0));
        /* sub_0013E100 is a bump allocator: [0x2F3C80] is the cursor and
         * [0x2F3C7C] the limit. It returns NULL the moment cursor+size passes
         * the limit, which is indistinguishable from any other null. */
        fprintf(stderr, "  RW bump alloc: cursor=[0x2F3C80]=0x%08X limit=[0x2F3C7C]=0x%08X\n",
                *(const uint32_t *)(off + 0x2F3C80), *(const uint32_t *)(off + 0x2F3C7C));
        /* The filtered view above hides everything that is not a code address,
         * which is exactly where the arguments are. A null field reached
         * through an argument is unreadable without them. */
        fprintf(stderr, "  raw stack:\n");
        for (int i = 0; i < 24; i += 4)
            fprintf(stderr, "    [esp+%-3d] %08X %08X %08X %08X\n",
                    i * 4, sp[i], sp[i + 1], sp[i + 2], sp[i + 3]);
    }
    /* A title that dies mid-boot is exactly the one whose indirect-branch
     * targets are worth keeping, so dump before re-raising. */
    RECOMP_ICALL_FEEDBACK_DUMP();
    fflush(stderr);
    signal(sig, SIG_DFL);   /* re-raise so lldb / the OS sees the real fault */
    raise(sig);
}

static void install_crash_handler(void)
{
    /* Separate stack: a guest stack overflow must still be reportable. */
    static char altstack[256 * 1024];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);   /* macOS reports many bad accesses as SIGBUS */
}

/* ── MCPX APU ──────────────────────────────────────────────── */

/* DirectSound hands stream and buffer packets to the APU's voice processor
 * and completes them from the APU interrupt. Black's DirectSound connects
 * that ISR at vector 5 (routine 0x0021D042); with nothing behind the VP
 * registers no packet ever completed, the logo soundtrack never advanced,
 * and the intro video, which takes its clock from it, never moved. The
 * APU core registers occupy the first 0x20000 and the voice processor's
 * method space the next 0x10000 (Play writes 0xFE8202F8.. and polls
 * PIO_FREE at 0xFE820010); GP/EP DSP space above stays plain memory for the
 * runtime's DSP stubs. */
#define APU_VA          0xFE800000u
#define APU_VP_SIZE     0x00030000u
#define APU_VECTOR      5u
#define APU_ISTS        0x1000u
#define APU_ISTS_GINTS  0x1u

static uint64_t apu_region_read(void *ctx, uint32_t off, unsigned size)
{
    return mcpx_apu_mmio_read((MCPXAPUState *)ctx, off, size);
}

static void apu_region_write(void *ctx, uint32_t off, uint64_t val, unsigned size)
{
    mcpx_apu_mmio_write((MCPXAPUState *)ctx, off, val, size);
}

static int apu_irq_pending(void *ctx)
{
    return (mcpx_apu_mmio_read((MCPXAPUState *)ctx, APU_ISTS, 4) & APU_ISTS_GINTS) != 0;
}

static void apu_init(void)
{
    MCPXAPUState *apu;

    if (!getenv("RECOMP_AC97_READY"))
        return;
    apu = mcpx_apu_init_standalone((uint8_t *)(uintptr_t)xbox_GetMemoryOffset());
    if (!apu)
        return;
    if (!xbox_mmio_intercept(APU_VA, APU_VP_SIZE, apu_region_read, apu_region_write,
                             apu, "APU"))
        return;
    xbox_RegisterInterruptSource(APU_VECTOR, apu_irq_pending, apu);
    if (getenv("RECOMP_AUDIO_TEST"))
        mcpx_apu_play_test_tone(apu);
}

/* ── Inline-vertex probe (temporary, for the black menu panel) ─ */

static void panel_probe(uint32_t prim, uint32_t *w, uint32_t count)
{
    static struct timespec t0;
    static int started, lines;
    struct timespec now;
    double after = atof(getenv("RECOMP_PANEL_PROBE"));

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!started) { t0 = now; started = 1; }
    if ((now.tv_sec - t0.tv_sec) < after || lines > 400 || count > 96)
        return;
    lines++;
    fprintf(stderr, "[PANEL] prim=%u count=%u:", prim, count);
    for (uint32_t i = 0; i < count; i++) {
        float f; memcpy(&f, &w[i], 4);
        if (w[i] >= 0x3000000u && w[i] < 0x7F000000u && f > -4096.f && f < 4096.f)
            fprintf(stderr, " %.1f", f);
        else
            fprintf(stderr, " %08X", w[i]);
    }
    fprintf(stderr, "\n");
}

/* ── Menu black panel (workaround) ─────────────────────────── */
/*
 * The title and menus draw an opaque black panel over the attract video:
 * inline XYZRHW vertices, 5 words each (x, y, u, v, diffuse), as a triangle
 * list whose every vertex is diffuse 0xFF000000, followed by a grey line
 * outline of the same rectangle. Measured on the title screen:
 *   prim 5, 30 words: rect 116.6..523.5 x 117.6..362.5, all FF000000
 *   prim 2, 40 words: the same corners, all FF7F7F7F
 * The title itself asks for it; the root cause is in its menu logic (see
 * BLACK_PORT_ESTADO 3.4). Until that is found the panel and its outline are
 * collapsed to a point so nothing is drawn -- the same workaround the working
 * build carried (black-recomp 38a81c0, lost with that repository).
 * RECOMP_KEEP_BLACK_PANEL=1 draws it.
 */
#define PANEL_STRIDE 5
static float s_panel_x0, s_panel_x1, s_panel_y0, s_panel_y1;
static int   s_panel_valid;

static float word_f(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }

static int panel_bounds(const uint32_t *w, uint32_t n, float *x0, float *x1,
                        float *y0, float *y1)
{
    *x0 = *y0 = 1e9f; *x1 = *y1 = -1e9f;
    for (uint32_t i = 0; i < n; i += PANEL_STRIDE) {
        float x = word_f(w[i]), y = word_f(w[i + 1]);
        if (!(x >= 0.f && x <= 640.f && y >= 0.f && y <= 480.f))
            return 0;
        if (x < *x0) *x0 = x;
        if (x > *x1) *x1 = x;
        if (y < *y0) *y0 = y;
        if (y > *y1) *y1 = y;
    }
    /* Every vertex on a corner: an axis-aligned rectangle. */
    for (uint32_t i = 0; i < n; i += PANEL_STRIDE) {
        float x = word_f(w[i]), y = word_f(w[i + 1]);
        if ((x != *x0 && x != *x1) || (y != *y0 && y != *y1))
            return 0;
    }
    return *x1 > *x0 && *y1 > *y0;
}

static void collapse(uint32_t *w, uint32_t n)
{
    for (uint32_t i = PANEL_STRIDE; i < n; i += PANEL_STRIDE) {
        w[i] = w[0];
        w[i + 1] = w[1];
    }
}

static void black_panel_filter(uint32_t prim, uint32_t *w, uint32_t count)
{
    float x0, x1, y0, y1;

    if (count < 2 * PANEL_STRIDE || count % PANEL_STRIDE)
        return;
    if (prim == 5) {                          /* triangles: the panel */
        for (uint32_t i = 4; i < count; i += PANEL_STRIDE)
            if (w[i] != 0xFF000000u)
                return;
        if (!panel_bounds(w, count, &x0, &x1, &y0, &y1))
            return;
        if (x1 - x0 >= 639.f && y1 - y0 >= 479.f)
            return;                           /* a full-screen fade is not it */
        s_panel_x0 = x0; s_panel_x1 = x1; s_panel_y0 = y0; s_panel_y1 = y1;
        s_panel_valid = 1;
        collapse(w, count);
    } else if (prim == 2 && s_panel_valid) {  /* lines: its outline */
        if (!panel_bounds(w, count, &x0, &x1, &y0, &y1))
            return;
        if (x0 == s_panel_x0 && x1 == s_panel_x1
                && y0 == s_panel_y0 && y1 == s_panel_y1)
            collapse(w, count);
    }
}

/* ── XBE loading ───────────────────────────────────────────── */

static int load_file(const char *path, void **out, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = n > 0 ? malloc((size_t)n) : NULL;
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return 0;
    }
    fclose(f);
    *out = buf; *out_size = (size_t)n;
    return 1;
}

/* ── Guest thread (macOS) ──────────────────────────────────── */
/*
 * xbe_entry_point() is the recompiled Xbox binary: it runs the whole guest
 * CPU loop and does not return until the game exits. It cannot run on the
 * main thread when a Cocoa window is open: NSWindow/CAMetalLayer need the
 * main thread to keep servicing NSApplication's run loop so WindowServer
 * can finish wiring up the window's Core Animation layer. If the main
 * thread never returns to Cocoa, the render thread can keep calling
 * nextDrawable/commit/presentDrawable successfully forever while the
 * window itself is still shown through to the desktop (transparent),
 * because the window's own surface was never promoted by WindowServer.
 *
 * g_esp (and the other guest registers) are RECOMP_TLS, i.e. thread-local,
 * so they must be initialized on whichever thread actually calls
 * xbe_entry_point() - setting g_esp on the main thread does not carry over
 * to a pthread's own TLS slot.
 */
#if defined(__APPLE__)
extern void xbox_PumpMainRunLoop(void); /* implemented in fb_present_posix.m */
static volatile int s_guest_running = 1;

static void *guest_thread_main(void *unused)
{
    (void)unused;
    g_esp = XBOX_STACK_TOP;
    xbe_entry_point();
    s_guest_running = 0;
    return NULL;
}
#endif

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);

    const char *game_dir = argc > 1 ? argv[1]
                         : getenv("BLACK_GAME_DIR") ? getenv("BLACK_GAME_DIR")
                         : "game";
    char xbe_path[4096];
    snprintf(xbe_path, sizeof xbe_path, "%s/default.xbe", game_dir);

    printf("=== Black - Static Recompilation (POSIX host) ===\n");
    install_crash_handler();
    RECOMP_ICALL_FEEDBACK_INIT();

    void *xbe = NULL; size_t xbe_size = 0;
    if (!load_file(xbe_path, &xbe, &xbe_size)) return 1;
    printf("XBE loaded: %zu bytes from %s\n", xbe_size, xbe_path);

    /* Black's GPU memory is the contiguous window (0x80000000 + P), and D3D's
     * LockRect hands out the tiled alias 0xF0000000 + P. Aliasing the tiled
     * aperture to low RAM sent every CPU write to a locked surface -- XMV video
     * frames among them -- to whatever the title kept at VA P. Must precede
     * xbox_MemoryLayoutInit. (Runtime commit 710d99b.) */
    xbox_SetTiledAliasesContig(1);

    printf("Initializing Xbox memory layout...\n");
    if (!xbox_MemoryLayoutInit(xbe, xbe_size)) {
        fprintf(stderr, "Memory layout init failed.\n");
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();
    fprintf(stderr, "[BOOT] after xbox_kernel_init\n");
    fflush(stderr);
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        xbox_path_init(game_dir, NULL);
    }
    fprintf(stderr, "[BOOT] after xbox_path_init\n");
    fflush(stderr);
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();
    fprintf(stderr, "[BOOT] after xbox_kernel_bridge_init\n");
    fflush(stderr);
    apu_init();
    fprintf(stderr, "[BOOT] after apu_init\n");
    fflush(stderr);
    if (getenv("RECOMP_NV2A_MMIO")) {
        nv2a_hook_init(g_xbox_mem_offset);
        if (nv2a_hook_get_state()) {
            xbox_mmio_intercept(NV2A_MMIO_BASE, NV2A_MMIO_SIZE,
                                nv2a_region_read, nv2a_region_write,
                                nv2a_hook_get_state(), "NV2A");
        }
    }
    /* The pad: keyboard and mouse of the game window (plus an SDL pad if
     * one is attached), read by the native XInput in recomp_natives.c. */
    xbox_InputInit();
    if (getenv("RECOMP_USB")) {
        /* Emulated OHCI, only for experimenting with the title's own USB
         * stack: the native XInput above never starts it. */
        xbox_OhciInit();
    }
    fprintf(stderr, "[BOOT] after input/ohci init\n");
    fflush(stderr);

    g_esp = XBOX_STACK_TOP;

    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; using binary search\n");
    fprintf(stderr, "[BOOT] after recomp_dispatch_init\n");
    fflush(stderr);
    xbox_WatchdogStart();
    fprintf(stderr, "[BOOT] after xbox_WatchdogStart\n");
    fflush(stderr);

    /* D3D push-buffer completion fence (sub_0026CC10): device at [0x27BFF8],
     * submitted count at +0x2C, fence pointer at +0x30 (the 96-byte contiguous
     * block). Measured: submitted 13, fence stuck at 3. */
    xbox_Nv2aMirrorFence(0x0027BFF8u, 0x2Cu, 0x30u);

    /* D3DDevice_GetDisplayFieldStatus (sub_0026B610) reports the field from
     * [dev + 0x1DF8]. The vblank ISR only re-latches it when its DPC re-arms,
     * so the XMV player (sub_0023F45C), which starts its clock on the other
     * field, never started a video. Alternate it from the vblank clock. */
    xbox_Nv2aFieldParity(0x0027BFF8u, 0x1DF8u);

    {
        extern void xbox_PbSetInlineFilter(void (*fn)(uint32_t, uint32_t *, uint32_t));
        if (getenv("RECOMP_PANEL_PROBE"))
            xbox_PbSetInlineFilter(panel_probe);
        else if (!getenv("RECOMP_KEEP_BLACK_PANEL"))
            xbox_PbSetInlineFilter(black_panel_filter);
    }

#if defined(__APPLE__)
    /* xemu's PGRAPH (RECOMP_XEMU_GPU). Without xnv2a_init the pushbuffer
     * methods are dropped and PGRAPH registers read as 0 -- including
     * PATT_COLOR0 (0xFD400B10), D3D's fence marker. sub_0026CBA0 spins until
     * PATT_COLOR0 == [fence] << 2, so the title hung in its first
     * BlockOnFence right after loading FEAudio.awd. The GL contexts must be
     * created on the main thread on macOS; methods then arrive on the NV2A
     * ack thread, which becomes the renderer's.
     * vram = physical memory = the contiguous window; aperture = 0xFD000000
     * (RAMIN at +0x700000, PFIFO at +0x2000). */
    if (getenv("RECOMP_XEMU_GPU")) {
        extern void xnv2a_context_init_main_thread(void);
        extern void xnv2a_init(uint8_t *vram, size_t vram_size, uint8_t *aperture);
        xnv2a_context_init_main_thread();
        xnv2a_init((uint8_t *)(uintptr_t)(g_xbox_mem_offset + 0x80000000u),
                   64u * 1024u * 1024u,
                   (uint8_t *)(uintptr_t)(g_xbox_mem_offset + 0xFD000000u));
        fprintf(stderr, "[BOOT] xemu NV2A renderer initialised\n");
    }
#endif

    /* Catch whoever overwrites guest .text. A jump table at 0x000F7614 comes
     * back corrupted, and the store happens thousands of calls before the
     * crash it causes; a read-only page faults at the instruction itself. */
    if (getenv("RECOMP_WATCH_TEXT")) {
        long   ps = sysconf(_SC_PAGESIZE);
        uint32_t watch_va = (uint32_t)strtoul(getenv("RECOMP_WATCH_TEXT"), NULL, 0);
        if (watch_va < 0x1000) watch_va = 0x000F7614;
        uintptr_t page = (uintptr_t)(g_xbox_mem_offset + watch_va) & ~(uintptr_t)(ps - 1);
        if (mprotect((void *)page, (size_t)ps, PROT_READ) != 0)
            perror("[WATCH] mprotect");
        else
            fprintf(stderr, "[WATCH] guest VA 0x%08X: host page %p now read-only (%ld bytes)\n",
                    watch_va, (void *)page, ps);
    }

    printf("Entry point: 0x%08X  ESP: 0x%08X\nStarting game...\n",
           BLACK_ENTRY_POINT, g_esp);
    fprintf(stderr, "[BOOT] before xbe_entry_point\n");
    fflush(stderr);
#if defined(__APPLE__)
    int fb_window_active = getenv("RECOMP_FB_WINDOW") != NULL;
    if (fb_window_active)
        xbox_FramebufferWindowStart();

    if (fb_window_active) {
        pthread_t guest_thread;
        if (pthread_create(&guest_thread, NULL, guest_thread_main, NULL) != 0) {
            fprintf(stderr, "[BOOT] failed to start guest thread\n");
            return 1;
        }
        /* Main thread stays free for Cocoa: drain NSApp's event queue on
         * every iteration instead of blocking on xbe_entry_point(). */
        while (s_guest_running) {
            xbox_PumpMainRunLoop();
            struct timespec ts = { 0, 1000000 }; /* 1ms */
            nanosleep(&ts, NULL);
        }
        pthread_join(guest_thread, NULL);
    } else {
        xbe_entry_point();
    }

    if (fb_window_active)
        xbox_FramebufferWindowStop();
#else
    xbe_entry_point();
#endif

    printf("Game returned. Cleaning up...\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    extern void f7_print_counts(void);
    f7_print_counts();
    free(xbe);
    return 0;
}