/**
 * Manual function overrides and ICALL diagnostics for Black.
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

extern uint32_t g_eax;
extern ptrdiff_t g_xbox_mem_offset;

/* RECOMP_MENULOG=1: name every front-end action the title dispatches.
 * The menu registers its actions by name (push handler; mov ecx, "Name";
 * call sub_00112FC0); these are all 68 of them, read from the XBE. Menu text
 * is not drawn yet, so this is how to see what the menu is doing. */
#include <stdlib.h>
static const struct { uint32_t va; const char *name; } k_menu_actions[] = {
    { 0x00112860u, "DebugGoToTitleMenu" },
    { 0x001132A0u, "DemoMode_PostTitleScreen" },
    { 0x001132E0u, "DemoMode_StartDemo" },
    { 0x001136D0u, "LeaveDifficultyMenu" },
    { 0x00113710u, "SelectDifficulty" },
    { 0x00113720u, "ChangeDifficulty" },
    { 0x00113740u, "ConfirmCustom" },
    { 0x001138F0u, "DifficultyBack" },
    { 0x00113970u, "SucceedTestAndFinish" },
    { 0x001139C0u, "SucceedTest" },
    { 0x00118340u, "Unset60Hz" },
    { 0x00118370u, "Set60Hz" },
    { 0x00118390u, "Unset480pAndFinish" },
    { 0x001183E0u, "Unset480p" },
    { 0x00118410u, "Set480p" },
    { 0x00119970u, "SetGammaLevel" },
    { 0x00119A10u, "ChangePictureOutputType" },
    { 0x00119C00u, "ToggleVibration" },
    { 0x00119CF0u, "ToggleCrouchOnOff" },
    { 0x00119D60u, "ToggleInvertLook" },
    { 0x00119DD0u, "ToggleVibration" },
    { 0x00119EC0u, "ToggleCrouchOnOff" },
    { 0x00119F50u, "ToggleInvertLook" },
    { 0x00119FE0u, "SetMusicValue" },
    { 0x0011A080u, "SetSfxValue" },
    { 0x0011A280u, "Start60HzTest" },
    { 0x0011A2C0u, "Start480pTest" },
    { 0x0011A660u, "SetupConfirmWarning" },
    { 0x0011AFE0u, "ChangePlayMode" },
    { 0x0011B0D0u, "ChangeSoundtrack" },
    { 0x0011B730u, "ChangeObjectiveHint" },
    { 0x0011B7F0u, "UpdateDificulty" },
    { 0x0011B890u, "LevelSelect" },
    { 0x0011D130u, "SetupChallengeResultsFailed" },
    { 0x0011D270u, "SetupChallengeResultsSucceeded" },
    { 0x0011D4F0u, "SelectChallengeKillingTime" },
    { 0x0011D740u, "SelectChallengeGunRun" },
    { 0x0011D8B0u, "SelectChallengeHeadCount" },
    { 0x0011DC40u, "SetMusicValue" },
    { 0x0011DCF0u, "SetSfxValue" },
    { 0x0011DDA0u, "ChangeSoundOutputType" },
    { 0x0011E110u, "LaunchCustom" },
    { 0x0011E120u, "ChangeControllerType" },
    { 0x0011E210u, "SetControllerTypeToCustom" },
    { 0x0011E280u, "SetCurrentObjectives" },
    { 0x0011EA00u, "ChallengeLevelSelect" },
    { 0x0011ED00u, "SetupObjectives" },
    { 0x00120370u, "RevertControllerConfig" },
    { 0x00120390u, "ChangeButtonAction" },
    { 0x001231F0u, "CloseObjectivesScreen" },
    { 0x001234B0u, "ClosePauseMenu" },
    { 0x001235C0u, "RestartChallenge" },
    { 0x001237E0u, "StartChallenge" },
    { 0x001240D0u, "ContinueMission" },
    { 0x00124190u, "RestartMission" },
    { 0x00124250u, "RestartLevel" },
    { 0x001246E0u, "DemoMode_QuitDemo" },
    { 0x00124750u, "QuitFromMissionFailed" },
    { 0x00124850u, "QuitLevel" },
    { 0x00124CF0u, "MissionModeSelect_StartLevel" },
    { 0x00124F00u, "SelectedDifficulty" },
    { 0x001F8C10u, "StartVideo" },
    { 0x001F8DB0u, "StartVideoPage" },
    { 0x001F8E70u, "SkipIntroCredits" },
    { 0x001F8EE0u, "StartMainMenu" },
    { 0x001F8F00u, "StartMainMenuSkip" },
    { 0x0020DE00u, "StartGame" },
    { 0x0020DEF0u, "StartNewMission" },
};

static void menu_log(uint32_t va)
{
    for (size_t i = 0; i < sizeof k_menu_actions / sizeof k_menu_actions[0]; i++)
        if (k_menu_actions[i].va == va) {
            fprintf(stderr, "[MENU] %s (0x%08X)\n", k_menu_actions[i].name, va);
            fflush(stderr);
            return;
        }
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    static int menulog = -1;
    if (menulog < 0)
        menulog = getenv("RECOMP_MENULOG") != NULL;
    if (menulog)
        menu_log(xbox_va);
    (void)xbox_va;
    return (recomp_func_t)0;
}

void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s)\n",
            va, (unsigned long long)hits[i]);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. (From upstream's
 * templates/new-game/src/recomp_manual.c.) */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* ── NaN reaching an SSE compare (diagnostic) ────────────────────────────
 *
 * Called by a generated build whose comiss/ucomiss sites were instrumented
 * (sed over src/recomp/gen; not part of the normal generator output). Reports
 * the first NaN seen in each guest function and then every power-of-two count
 * of it, so a run can be killed at any time and still say where NaNs come
 * from and how many. */
#include <string.h>

void recomp_nan_seen(const char *fn)
{
    enum { SLOTS = 96 };
    static struct { char name[24]; unsigned long n; } t[SLOTS];
    static int used;
    static unsigned long total;
    int i;

    total++;
    for (i = 0; i < used; i++)
        if (strncmp(t[i].name, fn, sizeof t[i].name - 1) == 0)
            break;
    if (i == used) {
        if (used == SLOTS)
            return;
        strncpy(t[used].name, fn, sizeof t[used].name - 1);
        used++;
    }
    t[i].n++;
    if ((t[i].n & (t[i].n - 1)) == 0)
        fprintf(stderr, "[NAN] %s: %lu (all functions: %lu)\n", fn, t[i].n, total);
}
