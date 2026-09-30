/**
 * Differential check of the recompiled code against the original x86.
 *
 * Built only with -DBLACK_SHADOW=ON, where the generator's output is wrapped
 * (tools/shadow_wrap.py): every translated function sub_XXXXXXXX becomes
 *
 *     void sub_XXXXXXXX(void) { recomp_shadow_call(0xXXXXXXXX, sub_XXXXXXXX_impl); }
 *
 * On the first few calls of each function:
 *   1. the ORIGINAL x86 bytes of the function run in Unicorn, from a copy of
 *      guest RAM and the caller's registers, and what they wrote is kept as a
 *      short list of (address, old, new) words;
 *   2. the recompiled function then runs, for real, as it always does;
 *   3. each word the original wrote is looked up in live memory. Where the
 *      translation left something else, or the result registers or the
 *      callee-saved ones disagree, it is a translation error and is logged
 *      with its guest address.
 * The emulation happens BEFORE the real run, so a function that never returns
 * (the game's main loop) just costs one timed-out emulation, and the checks
 * nest: a function called from inside another gets its turn too.
 *
 * Functions that call the kernel, touch hardware registers or run away cannot
 * be followed by the emulator: those are counted, not judged.
 *
 * RECOMP_SHADOW=1          enable
 * RECOMP_SHADOW_CALLS=n    calls to check per function (default 2)
 * RECOMP_SHADOW_RETRY=n    extra calls to try when a check was skipped (default 6)
 * RECOMP_SHADOW_ONLY=hex   check only this function (e.g. 001D3790)
 * RECOMP_SHADOW_VERBOSE=1  one line per check
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <setjmp.h>
#include <unicorn/unicorn.h>
#include "recomp_types.h"

extern ptrdiff_t g_xbox_mem_offset;

#define MAIN_LO   0x00010000u
#define MAIN_HI   0x04000000u
#define CONT_LO   0x80000000u
#define CONT_HI   0x84000000u
#define MAIN_SIZE (MAIN_HI - MAIN_LO)
#define CONT_SIZE (CONT_HI - CONT_LO)
#define NPAGE_MAIN (MAIN_SIZE / 4096)
#define NPAGE_CONT (CONT_SIZE / 4096)

static int         s_on = -1;
static uc_engine  *s_uc;
static uint8_t    *s_uc_main, *s_uc_cont;       /* the emulator's RAM */
static uint8_t    *s_ok_main, *s_ok_cont;       /* page readable? one byte per 4 KB */
static uint8_t    *s_dirty_main, *s_dirty_cont; /* pages the emulated code wrote */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned s_calls_max = 2, s_retry_max = 6;
static int      s_only = -1;
static int      s_verbose;

typedef struct { uint32_t va; uint16_t tries, done; uint8_t diffed; } Slot;
#define NSLOT 32768
static Slot s_slot[NSLOT];

static unsigned long s_n_ok, s_n_diff, s_n_skip_mem, s_n_skip_other, s_n_calls;
static unsigned s_skip_reasons[16];

typedef struct { uint32_t addr, oldv, newv; } Delta;

static sigjmp_buf s_probe_jmp;
static void probe_handler(int sig) { (void)sig; siglongjmp(s_probe_jmp, 1); }

static uint8_t *host_ptr(uint32_t va) { return (uint8_t *)(g_xbox_mem_offset + (ptrdiff_t)va); }

/* Which 4 KB pages of a guest range can be read at all. */
static void probe_pages(uint32_t lo, uint32_t size, uint8_t *ok)
{
    struct sigaction sa, old_segv, old_bus;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS, &sa, &old_bus);
    for (uint32_t p = 0; p < size / 4096; p++) {
        volatile uint8_t v;
        if (sigsetjmp(s_probe_jmp, 1) == 0) {
            v = *(volatile uint8_t *)host_ptr(lo + p * 4096);
            (void)v;
            ok[p] = 1;
        } else {
            ok[p] = 0;
        }
    }
    sigaction(SIGSEGV, &old_segv, NULL);
    sigaction(SIGBUS, &old_bus, NULL);
}

static void copy_readable(uint8_t *dst, uint32_t lo, uint32_t size, const uint8_t *ok)
{
    uint32_t np = size / 4096;
    for (uint32_t p = 0; p < np;) {
        uint32_t q = p;
        if (!ok[p]) { memset(dst + (size_t)p * 4096, 0, 4096); p++; continue; }
        while (q < np && ok[q]) q++;
        memcpy(dst + (size_t)p * 4096, host_ptr(lo + p * 4096), (size_t)(q - p) * 4096);
        p = q;
    }
}

static void on_write(uc_engine *uc, uc_mem_type type, uint64_t addr, int size, int64_t value, void *ud)
{
    uint64_t end = addr + (size > 0 ? (uint64_t)size : 1) - 1;
    (void)uc; (void)type; (void)value; (void)ud;
    for (uint64_t a = addr & ~4095ull; a <= end; a += 4096) {
        if (a >= MAIN_LO && a < MAIN_HI) s_dirty_main[(a - MAIN_LO) / 4096] = 1;
        else if (a >= CONT_LO && a < CONT_HI) s_dirty_cont[(a - CONT_LO) / 4096] = 1;
    }
}

static int shadow_init(void)
{
    const char *e;
    uc_err err;
    uc_hook hh;
    unsigned nm = 0, nc = 0;

    if ((e = getenv("RECOMP_SHADOW_CALLS")) && atoi(e) > 0) s_calls_max = (unsigned)atoi(e);
    if ((e = getenv("RECOMP_SHADOW_RETRY")) && atoi(e) >= 0) s_retry_max = (unsigned)atoi(e);
    if ((e = getenv("RECOMP_SHADOW_ONLY"))) s_only = (int)strtoul(e, NULL, 16);
    s_verbose = getenv("RECOMP_SHADOW_VERBOSE") != NULL;

    s_uc_main = malloc(MAIN_SIZE);  s_uc_cont = malloc(CONT_SIZE);
    s_ok_main = calloc(NPAGE_MAIN, 1);  s_ok_cont = calloc(NPAGE_CONT, 1);
    s_dirty_main = calloc(NPAGE_MAIN, 1);  s_dirty_cont = calloc(NPAGE_CONT, 1);
    if (!s_uc_main || !s_uc_cont || !s_ok_main || !s_ok_cont || !s_dirty_main || !s_dirty_cont) {
        fprintf(stderr, "[SHADOW] out of memory\n");
        return 0;
    }
    probe_pages(MAIN_LO, MAIN_SIZE, s_ok_main);
    probe_pages(CONT_LO, CONT_SIZE, s_ok_cont);
    for (uint32_t i = 0; i < NPAGE_MAIN; i++) nm += s_ok_main[i];
    for (uint32_t i = 0; i < NPAGE_CONT; i++) nc += s_ok_cont[i];

    err = uc_open(UC_ARCH_X86, UC_MODE_32, &s_uc);
    if (err != UC_ERR_OK) { fprintf(stderr, "[SHADOW] uc_open: %s\n", uc_strerror(err)); return 0; }
    /* Code and data in main RAM; the contiguous window is data only, so a
     * jump into a kernel thunk (0x8000xxxx) stops the emulation. */
    err = uc_mem_map_ptr(s_uc, MAIN_LO, MAIN_SIZE, UC_PROT_ALL, s_uc_main);
    if (err == UC_ERR_OK)
        err = uc_mem_map_ptr(s_uc, CONT_LO, CONT_SIZE, UC_PROT_READ | UC_PROT_WRITE, s_uc_cont);
    if (err == UC_ERR_OK)
        err = uc_hook_add(s_uc, &hh, UC_HOOK_MEM_WRITE, (void *)on_write, NULL, 1, 0);
    if (err != UC_ERR_OK) { fprintf(stderr, "[SHADOW] unicorn setup: %s\n", uc_strerror(err)); return 0; }

    fprintf(stderr, "[SHADOW] on: %u call(s) per function; readable pages: main %u of %u, contiguous %u of %u\n",
            s_calls_max, nm, (unsigned)NPAGE_MAIN, nc, (unsigned)NPAGE_CONT);
    return 1;
}

static void report_summary(void)
{
    fprintf(stderr, "[SHADOW] summary: %lu calls seen; %lu checks identical, %lu DIFFERENT, %lu skipped (memory/kernel), %lu skipped (other)\n",
            s_n_calls, s_n_ok, s_n_diff, s_n_skip_mem, s_n_skip_other);
    for (unsigned i = 0; i < NSLOT; i++)
        if (s_slot[i].va && s_slot[i].diffed)
            fprintf(stderr, "[SHADOW]   differs: sub_%08X\n", s_slot[i].va);
}

static Slot *slot_for(uint32_t va)
{
    unsigned h = (unsigned)(((uint64_t)va * 2654435761u) >> 17) & (NSLOT - 1);
    for (unsigned n = 0; n < NSLOT; n++, h = (h + 1) & (NSLOT - 1)) {
        if (s_slot[h].va == va) return &s_slot[h];
        if (!s_slot[h].va) { s_slot[h].va = va; return &s_slot[h]; }
    }
    return NULL;
}

static int is_float_close(uint32_t a, uint32_t b)
{
    float fa, fb;
    memcpy(&fa, &a, 4); memcpy(&fb, &b, 4);
    if (fa != fa && fb != fb) return 1;                 /* both NaN */
    if (!(fabsf(fa) < 1e30f) || !(fabsf(fb) < 1e30f)) return 0;
    float m = fmaxf(fmaxf(fabsf(fa), fabsf(fb)), 1e-6f);
    return fabsf(fa - fb) <= 2e-5f * m;
}

#define UCW(reg, val) do { uint32_t _v = (val); uc_reg_write(s_uc, (reg), &_v); } while (0)

typedef struct {
    int      ok;                      /* the emulation reached the return address */
    uc_err   err;
    uint32_t eax, esp, ebx, esi, edi;
    Delta   *delta;
    unsigned ndelta;
} Original;

/* Run the original bytes of `va` from live memory as it is now. */
static void run_original(uint32_t va, Original *o)
{
    uint32_t eax0 = g_eax, ecx0 = g_ecx, edx0 = g_edx, ebx0 = g_ebx, esi0 = g_esi, edi0 = g_edi;
    uint32_t ebp0 = g_ebp, esp0 = g_esp;
    uint32_t retaddr = *(uint32_t *)host_ptr(esp0);
    RecompXmm x[8] = { g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7 };
    uint16_t cw = g_fp_control_word;
    uint32_t eip = 0;
    unsigned cap = 0;

    memset(o, 0, sizeof *o);
    copy_readable(s_uc_main, MAIN_LO, MAIN_SIZE, s_ok_main);
    copy_readable(s_uc_cont, CONT_LO, CONT_SIZE, s_ok_cont);
    memset(s_dirty_main, 0, NPAGE_MAIN);
    memset(s_dirty_cont, 0, NPAGE_CONT);

    UCW(UC_X86_REG_EAX, eax0); UCW(UC_X86_REG_ECX, ecx0); UCW(UC_X86_REG_EDX, edx0);
    UCW(UC_X86_REG_EBX, ebx0); UCW(UC_X86_REG_ESI, esi0); UCW(UC_X86_REG_EDI, edi0);
    UCW(UC_X86_REG_EBP, ebp0); UCW(UC_X86_REG_ESP, esp0);
    UCW(UC_X86_REG_EFLAGS, 0x202u | (g_df ? 0x400u : 0u));
    uc_reg_write(s_uc, UC_X86_REG_FPCW, &cw);
    for (int i = 0; i < 8; i++) uc_reg_write(s_uc, UC_X86_REG_XMM0 + i, &x[i]);

    o->err = uc_emu_start(s_uc, va, retaddr, 3000000, 4000000);
    uc_reg_read(s_uc, UC_X86_REG_EIP, &eip);
    uc_reg_read(s_uc, UC_X86_REG_EAX, &o->eax);
    uc_reg_read(s_uc, UC_X86_REG_ESP, &o->esp);
    uc_reg_read(s_uc, UC_X86_REG_EBX, &o->ebx);
    uc_reg_read(s_uc, UC_X86_REG_ESI, &o->esi);
    uc_reg_read(s_uc, UC_X86_REG_EDI, &o->edi);
    o->ok = (o->err == UC_ERR_OK && eip == retaddr);
    if (!o->ok) return;

    /* the words the original changed, from the pages it wrote to */
    for (int region = 0; region < 2; region++) {
        uint32_t lo = region ? CONT_LO : MAIN_LO, np = region ? NPAGE_CONT : NPAGE_MAIN;
        const uint8_t *dirty = region ? s_dirty_cont : s_dirty_main;
        const uint8_t *emu = region ? s_uc_cont : s_uc_main;
        const uint8_t *ok = region ? s_ok_cont : s_ok_main;
        for (uint32_t p = 0; p < np; p++) {
            if (!dirty[p] || !ok[p]) continue;
            for (uint32_t w = 0; w < 4096; w += 4) {
                uint32_t off = p * 4096 + w, u, s;
                memcpy(&u, emu + off, 4);
                memcpy(&s, host_ptr(lo + off), 4);
                if (u == s) continue;
                if (o->ndelta == cap) {
                    cap = cap ? cap * 2 : 256;
                    o->delta = realloc(o->delta, cap * sizeof(Delta));
                }
                o->delta[o->ndelta++] = (Delta){ lo + off, s, u };
            }
        }
    }
}

void recomp_shadow_call(uint32_t va, void (*impl)(void))
{
    Slot *sl;
    Original o;
    uint32_t ebx0, esi0, edi0, esp0, retaddr;
    struct timespec t_a, t_b;

    if (s_on < 0) {
        const char *e = getenv("RECOMP_SHADOW");
        s_on = (e && *e && *e != '0');
        if (s_on) {
            pthread_mutex_lock(&s_lock);
            if (!s_uc && !shadow_init()) s_on = 0;
            else atexit(report_summary);
            pthread_mutex_unlock(&s_lock);
        }
    }
    if (!s_on || (s_only >= 0 && (uint32_t)s_only != va)) { impl(); return; }
    s_n_calls++;
    if (pthread_mutex_trylock(&s_lock) != 0) { impl(); return; }
    sl = slot_for(va);
    if (!sl || sl->done >= s_calls_max || sl->tries >= s_calls_max + s_retry_max) {
        pthread_mutex_unlock(&s_lock);
        impl();
        return;
    }
    sl->tries++;
    ebx0 = g_ebx; esi0 = g_esi; edi0 = g_edi; esp0 = g_esp;
    retaddr = *(uint32_t *)host_ptr(esp0);

    /* 1. the original, from the state the function starts from */
    clock_gettime(CLOCK_MONOTONIC, &t_a);
    run_original(va, &o);
    clock_gettime(CLOCK_MONOTONIC, &t_b);
    pthread_mutex_unlock(&s_lock);
    if (s_verbose)
        fprintf(stderr, "[SHADOW] sub_%08X original: %s, %u word(s) written, %.0f ms\n", va,
                o.ok ? "ok" : uc_strerror(o.err), o.ndelta,
                (t_b.tv_sec - t_a.tv_sec) * 1e3 + (t_b.tv_nsec - t_a.tv_nsec) / 1e6);

    /* 2. the translation, for real */
    impl();

    /* 3. compare */
    pthread_mutex_lock(&s_lock);
    if (!o.ok) {
        unsigned code = (unsigned)o.err < 16 ? (unsigned)o.err : 15;
        s_skip_reasons[code]++;
        if (o.err == UC_ERR_FETCH_PROT || o.err == UC_ERR_READ_UNMAPPED ||
            o.err == UC_ERR_WRITE_UNMAPPED || o.err == UC_ERR_FETCH_UNMAPPED ||
            o.err == UC_ERR_READ_PROT || o.err == UC_ERR_WRITE_PROT)
            s_n_skip_mem++;
        else
            s_n_skip_other++;
    } else {
        unsigned bad = 0, shown = 0, tol = 0;
        unsigned regdiff = 0;
        uint32_t espN = g_esp, eaxN = g_eax;
        uint32_t dead_lo = (espN < o.esp ? espN : o.esp) - 0x100000u, dead_hi = (espN > o.esp ? espN : o.esp);
        if (espN != o.esp) regdiff |= 1;
        if (g_esi != o.esi && o.esi == esi0) regdiff |= 2;     /* translation clobbered a callee-saved register */
        if (g_edi != o.edi && o.edi == edi0) regdiff |= 4;
        if (g_ebx != o.ebx && o.ebx == ebx0) regdiff |= 8;
        for (unsigned i = 0; i < o.ndelta; i++) {
            uint32_t a = o.delta[i].addr, r;
            if (a >= dead_lo && a < dead_hi) continue;        /* dead stack below the final esp */
            memcpy(&r, host_ptr(a), 4);
            if (r == o.delta[i].newv) continue;
            if (is_float_close(o.delta[i].newv, r)) { tol++; continue; }
            bad++;
            if (shown < 6) {
                shown++;
                fprintf(stderr, "[SHADOW]     @%08X before=%08X original wrote %08X translation left %08X\n",
                        a, o.delta[i].oldv, o.delta[i].newv, r);
            }
        }
        sl->done++;
        if (bad || regdiff || eaxN != o.eax) {
            sl->diffed = 1;
            s_n_diff++;
            fprintf(stderr, "[SHADOW] DIFF sub_%08X (call %u, returns to %08X): %u word(s) differ; eax original=%08X translation=%08X; esp original=%08X translation=%08X%s%s%s%s\n",
                    va, sl->done, retaddr, bad, o.eax, eaxN, o.esp, espN,
                    (regdiff & 2) ? " esi-clobbered" : "", (regdiff & 4) ? " edi-clobbered" : "",
                    (regdiff & 8) ? " ebx-clobbered" : "", (regdiff & 1) ? " esp-differs" : "");
        } else {
            s_n_ok++;
        }
        (void)tol;
    }
    free(o.delta);
    {
        static unsigned finished;
        if ((++finished % 200) == 0) {
            fprintf(stderr, "[SHADOW] progress: %u checks: %lu identical, %lu DIFFERENT, %lu skipped(memory/kernel) %lu skipped(other); uc errors:",
                    finished, s_n_ok, s_n_diff, s_n_skip_mem, s_n_skip_other);
            for (int i = 0; i < 16; i++) if (s_skip_reasons[i]) fprintf(stderr, " %d=%u", i, s_skip_reasons[i]);
            fprintf(stderr, "\n");
        }
    }
    pthread_mutex_unlock(&s_lock);
}
