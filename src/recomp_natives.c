/**
 * Guest functions implemented natively instead of translated.
 *
 * Each sub_ defined here is skipped by the generator
 * (tools.recomp --exclude-manual src/recomp_natives.c), so this definition is
 * the one that links, both for direct calls and for the dispatch table.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp_types.h"

/*
 * sub_000A24E0 -- the XDK CRT memcpy (MSVC's, which handles overlap, so it is
 * memmove in all but name). cdecl: dst, src, count on the stack; returns dst.
 *
 * Native because the translation cannot be complete: the routine dispatches
 * its head and tail bytes through jump tables embedded in the code
 * (0x000A2540, 0x000A25C0, 0x000A262C, 0x000A263C, 0x000A26D0, 0x000A27C8),
 * and the instructions after them are never decoded. Copies whose alignment
 * lands on those arms jumped to addresses with no translation --
 * "[ICALL] Failed to resolve VA 0x000A2550 / 0x000A26DC / 0x000A2700" -- and
 * returned with the copy unfinished; a few seconds later the title crashed
 * on a stack full of 0x30303030. The working backup build had the same gaps.
 * No other function uses those tables, so replacing the whole routine covers
 * every path.
 */
void sub_000A24E0(void)
{
    uint32_t dst = MEM32(g_esp + 4);
    uint32_t src = MEM32(g_esp + 8);
    uint32_t n   = MEM32(g_esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), n);
    g_eax = dst;
    g_esp += 4;   /* ret: the caller cleans the arguments (cdecl) */
}

/* ── XInput (keyboard as the pad, no USB) ─────────────────────────────────
 *
 * The same approach as the Critical Hour port: the title's XPP entry points
 * are answered natively and the USB/OHCI stack underneath is never started.
 * Port 0 always holds a gamepad fed by the keyboard and mouse of the game
 * window (and an SDL pad if one is attached), through the runtime's
 * xbox_InputGetState.
 *
 * Found by instruction-for-instruction match against the Critical Hour XBE
 * (same XDK, 5849) and by the calls the title makes into the XPP section:
 *   0x0027DD24 XInitDevices(count, types)          stdcall, 2 args
 *   0x0027DD29 XGetDeviceChanges(type, *in, *out)  stdcall, 3 args
 *   0x0027DD96 XInputOpen(type, port, slot, poll)  stdcall, 4 args
 *   0x0027DDEC XInputClose(handle)                 stdcall, 1 arg
 *   0x0027DDF8 XInputGetState(handle, *state)      stdcall, 2 args
 *   0x0027DE6B XInputSetState(handle, *feedback)   stdcall, 2 args
 * XDEVICE_TYPE_GAMEPAD is 0x0027CF4C (pushed by sub_000C22B0 for both
 * XGetDeviceChanges and XInputOpen); its words are current, pending change
 * and previous connection masks.
 */
#define XIN_TYPE_GAMEPAD  0x0027CF4Cu
#define XIN_HANDLE        0xD0DE0000u
#define XIN_NOT_CONNECTED 0x48Fu            /* ERROR_DEVICE_NOT_CONNECTED */

typedef struct {
    uint32_t dwPacketNumber;
    uint16_t wButtons;
    uint8_t  bAnalogButtons[8];
    int16_t  sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} xin_state_t;                               /* = XBOX_INPUT_STATE */
extern uint32_t xbox_InputGetState(uint32_t port, xin_state_t *state);

#include <time.h>
static double xin_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static int xin_log(void)
{
    static int v = -1;
    if (v < 0) v = getenv("RECOMP_XINLOG") != NULL;
    return v;
}

static uint32_t xin_arg(int i) { return MEM32(g_esp + 4u + 4u * (uint32_t)i); }
static void xin_ret(uint32_t nargs, uint32_t value)
{
    g_eax = value;
    g_esp += 4u + 4u * nargs;                 /* ret 4*nargs (stdcall) */
}

/* XInitDevices */
void sub_0027DD24(void)
{
    MEM32(XIN_TYPE_GAMEPAD + 0) = 1;          /* port 0 connected */
    MEM32(XIN_TYPE_GAMEPAD + 4) = 1;          /* change pending */
    MEM32(XIN_TYPE_GAMEPAD + 8) = 0;          /* previously: nothing */
    xin_ret(2, 0);
}

/* XGetDeviceChanges */
void sub_0027DD29(void)
{
    static int reported;
    uint32_t type = xin_arg(0), ins = xin_arg(1), rem = xin_arg(2);
    uint32_t in = 0;

    if (type == XIN_TYPE_GAMEPAD && !reported) {
        reported = 1;
        in = 1;                               /* the pad arrives once, on port 0 */
        MEM32(XIN_TYPE_GAMEPAD + 4) = 0;
        MEM32(XIN_TYPE_GAMEPAD + 8) = 1;
    }
    if (xin_log() && in)
        fprintf(stderr, "[XIN] XGetDeviceChanges type=%08X -> in=%u\n", type, in);
    if (ins) MEM32(ins) = in;
    if (rem) MEM32(rem) = 0;
    xin_ret(3, in != 0);
}

/* XInputOpen */
void sub_0027DD96(void)
{
    uint32_t type = xin_arg(0), port = xin_arg(1);
    if (xin_log())
        fprintf(stderr, "[XIN] XInputOpen type=%08X port=%u\n", type, port);
    xin_ret(4, (type == XIN_TYPE_GAMEPAD && port == 0) ? XIN_HANDLE : 0);
}

/* XInputClose */
void sub_0027DDEC(void)
{
    xin_ret(1, 0);
}

/* XInputGetState */
void sub_0027DDF8(void)
{
    static uint32_t packet;
    uint32_t h = xin_arg(0), out = xin_arg(1);
    xin_state_t s;

    if (h != XIN_HANDLE || !out) {
        xin_ret(2, XIN_NOT_CONNECTED);
        return;
    }
    memset(&s, 0, sizeof s);
    /* Not connected until the first key or click: an idle pad, not a
     * missing one, so the title does not ask for a controller. */
    xbox_InputGetState(0, &s);
    if (xin_log()) {   /* one line per CHANGE of what the pad reports */
        static uint16_t lb; static uint8_t la[8]; static int16_t llx, lly, lrx, lry;
        if (s.wButtons != lb || memcmp(s.bAnalogButtons, la, 8) != 0 ||
            (s.sThumbLX != 0) != (llx != 0) || (s.sThumbLY != 0) != (lly != 0) ||
            (s.sThumbRX != 0) != (lrx != 0) || (s.sThumbRY != 0) != (lry != 0)) {
            fprintf(stderr, "[XIN] t=%.3f pad: buttons=%04X A=%u B=%u X=%u Y=%u blk=%u wht=%u LT=%u RT=%u"
                            " L=(%d,%d) R=(%d,%d)\n", xin_now(), s.wButtons,
                    s.bAnalogButtons[0], s.bAnalogButtons[1], s.bAnalogButtons[2],
                    s.bAnalogButtons[3], s.bAnalogButtons[4], s.bAnalogButtons[5],
                    s.bAnalogButtons[6], s.bAnalogButtons[7],
                    s.sThumbLX, s.sThumbLY, s.sThumbRX, s.sThumbRY);
            lb = s.wButtons; memcpy(la, s.bAnalogButtons, 8);
            llx = s.sThumbLX; lly = s.sThumbLY; lrx = s.sThumbRX; lry = s.sThumbRY;
        }
    }
    MEM32(out) = ++packet;
    MEM16(out + 4) = s.wButtons;
    for (int i = 0; i < 8; i++)
        MEM8(out + 6 + i) = s.bAnalogButtons[i];
    MEM16(out + 14) = (uint16_t)s.sThumbLX;
    MEM16(out + 16) = (uint16_t)s.sThumbLY;
    MEM16(out + 18) = (uint16_t)s.sThumbRX;
    MEM16(out + 20) = (uint16_t)s.sThumbRY;
    xin_ret(2, 0);
}

/* XInputSetState (rumble) */
void sub_0027DE6B(void)
{
    uint32_t fb = xin_arg(1);
    if (fb)
        MEM32(fb) = 0;                        /* header.dwStatus: done */
    xin_ret(2, 0);
}
