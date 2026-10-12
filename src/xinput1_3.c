/*
 * xinput1_3.dll proxy for Prototype 2 (32-bit) - "dual input": gamepad + gyro
 * (Steam Input "gyro as mouse" arrives as a normal mouse).
 *
 * Prototype 2: prototype2.exe is a tiny launcher. The ENGINE DLL
 * (prototype2engine.dll, preferred base 0x10000000, no ASLR - everything is still
 * computed from GetModuleHandleA at runtime) imports XINPUT1_3.dll statically BY
 * ORDINAL: #2 XInputGetState, #3 XInputSetState, #4 XInputGetCapabilities.
 *
 * PAD SIDE. Steam Input (gameoverlayrenderer.dll) detours the exported
 * XInputGetState of the xinput1_3.dll the game loaded - this one - so code inside
 * the export may never run. Every export is therefore a plain pass-through to the
 * real System32/SysWOW64 DLL, and the engine DLL's IAT slots are repointed at
 * wrappers (MixedGetState etc.) which call the (possibly Steam-detoured) export.
 *
 * MOUSE SIDE. The engine reads mouse/keyboard through DirectInput8 created with
 * CoCreateInstance (no dinput8 import, so a dinput8.dll proxy cannot work). All
 * IDirectInputDevice8A objects of the system dinput8.dll share one vtable, so we
 * create our own mouse device, take its vtable and patch GetDeviceState (9) and
 * GetDeviceData (10). The hooks identify mouse devices via GetCapabilities, take
 * X/Y deltas out of what the game sees, and MixedGetState feeds them into the
 * right stick (velocity smoothing, sensitivity, curve, dead-zone compensation, real
 * stick override - same math as the Call of Juarez proxy, MouseMode 0).
 *
 * Menu detection (engine_base + offsets, read under __try):
 *   FE cursor visible : u8 at +0xCE9CC6 (1 in the front end, 0 in gameplay)
 *   paused (candidate): *(u8*)(*(u32*)(+0xDFA2BC) + 0xA8)
 * menu = cursor != 0 || paused != 0.  Never writes game memory.
 */
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <xinput.h>
#include <dinput.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

typedef DWORD (WINAPI *PFN_GetState)(DWORD, XINPUT_STATE *);
typedef DWORD (WINAPI *PFN_SetState)(DWORD, XINPUT_VIBRATION *);
typedef DWORD (WINAPI *PFN_GetCaps)(DWORD, DWORD, XINPUT_CAPABILITIES *);
typedef void  (WINAPI *PFN_Enable)(BOOL);
typedef DWORD (WINAPI *PFN_Guids)(DWORD, LPWSTR, UINT *, LPWSTR, UINT *);
typedef DWORD (WINAPI *PFN_Battery)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION *);
typedef DWORD (WINAPI *PFN_Keystroke)(DWORD, DWORD, XINPUT_KEYSTROKE *);

static HMODULE g_real, g_self;
static char g_log[MAX_PATH];
static volatile LONG g_installed;

/* ---- settings (xinput_mouse.ini next to the DLL) ---- */
static char  g_ini[MAX_PATH];
static int   g_enabled    = 1;
static float g_sens       = 0.3f;
static float g_sensY      = 1.0f;
static int   g_invertY    = 0;
static float g_curve      = 1.0f;
static float g_deadComp   = 0.10f;
static float g_windowMs   = 20.0f;   /* SmoothingMs: window length (mode 0) or time constant tau (mode 1) */
static int   g_smoothMode = 1;       /* 0 = windowed sum, 1 = per-poll exponential smoothing */
static float g_override   = 0.27f;
static int   g_menuMouse  = 0;
static int   g_buttons    = 0;
static int   g_usePause   = 1;
static int   g_stripStart = 1;       /* StripMouseOnStart: strip all mouse data while the controller is not yet locked (press-start screen) */
static int   g_toggleVk, g_slowerVk, g_fasterVk, g_modeVk;
static int   g_mouseMode  = 1;       /* 1 = native mouse look (engine patch P1), 0 = mouse -> right stick mixing */
static float g_nativeSens = 1.0f;
static float g_natTauMs   = 35.0f;   /* native-mode distance-preserving smoothing time constant, 0 = off */
static float g_natMaxSpeed = 1500.0f;  /* counts/s; >0: full smoothing below, none above 2x (0 = smooth at all speeds) */
static int   g_natLocked  = 1;       /* NativeFrameLocked: release per nominal frame instead of measured dt */
static float g_natFrameMs = 16.667f; /* NativeFrameMs: nominal frame time for the locked release */
static volatile LONG g_natDups;      /* locked mode: polls <1 ms after the previous one, treated as same frame (summary) */
static volatile LONG g_natReset;     /* ask the native smoother to drop pending/carry/speed */
static int   g_linearMouse = 1;      /* P2, read once at install */
static int   g_stickLinear = 0;      /* P3, read once at install */
static int   g_patchesDone, g_p1ok;  /* patches applied (once); P1 (mouse gate) applied */
static volatile LONG g_stickBusy;    /* real right stick deflected past StickOverride (mode 1) */
static volatile LONG g_natPassed, g_natStripped;   /* native-mode X/Y event/state counts (summary only) */
static int   g_logLevel   = 1;   /* 0 minimal, 1 diagnostics/transitions, 2 + 5 s summary, 3 + xinput_trace.csv */
static char  g_trace[MAX_PATH];
static char  g_ntrace[MAX_PATH];     /* native-mode smoothing trace (LogLevel 3) */
static FILETIME g_iniTime;           /* last-write time of the ini at the last (re)load */

static void Log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    if (!g_log[0] || !(f = fopen(g_log, "a"))) return;
    fprintf(f, "%lu ms: ", GetTickCount());
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* pass-through targets: the real system DLL */
static PFN_GetState  p_GetState;
static PFN_SetState  p_SetState;
static PFN_GetCaps   p_GetCaps;
static PFN_Enable    p_Enable;
static PFN_Guids     p_Guids;
static PFN_Battery   p_Battery;
static PFN_Keystroke p_Keystroke;

static void StampIni(void)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (g_ini[0] && GetFileAttributesExA(g_ini, GetFileExInfoStandard, &fa)) g_iniTime = fa.ftLastWriteTime;
}

static void SetMouseMode(int m);

/* Reads every setting from the ini. initial=0 on a hot reload: the Enabled key only
   overrides the runtime toggle (hotkey) when its value in the file actually changed. */
static void ReadSettings(int initial)
{
    char buf[64];
    static int prevEnabledIni = 1, prevModeIni = 1;
    int en, md;
#define GETF(key, var, def) do { GetPrivateProfileStringA("Mouse", key, def, buf, sizeof buf, g_ini); var = (float)atof(buf); } while (0)
    GETF("Sensitivity", g_sens, "0.3");
    GETF("SensitivityY", g_sensY, "1.0");
    GETF("DeadzoneComp", g_deadComp, "0.10");
    GETF("SmoothingMs", g_windowMs, "20");
    GETF("Curve", g_curve, "1.0");
    GETF("StickOverride", g_override, "0.27");
    GETF("NativeSensitivity", g_nativeSens, "1.0");
    GETF("NativeSmoothingMs", g_natTauMs, "35");
    GETF("NativeFrameMs", g_natFrameMs, "16.667");
    g_natLocked = GetPrivateProfileIntA("Mouse", "NativeFrameLocked", 1, g_ini) != 0;
    if (g_natFrameMs < 4.0f) g_natFrameMs = 4.0f;
    if (g_natFrameMs > 50.0f) g_natFrameMs = 50.0f;
    GETF("NativeSmoothingMaxSpeed", g_natMaxSpeed, "1500");
    if (g_natTauMs < 0.0f) g_natTauMs = 0.0f;
    if (g_natTauMs > 500.0f) g_natTauMs = 500.0f;
    if (g_natMaxSpeed < 0.0f) g_natMaxSpeed = 0.0f;
    InterlockedExchange(&g_natReset, 1);
#undef GETF
    g_smoothMode = GetPrivateProfileIntA("Mouse", "SmoothingMode", 1, g_ini) != 0;
    if (g_curve <= 0.05f) g_curve = 1.0f;
    if (g_windowMs < 1.0f) g_windowMs = 1.0f;
    if (g_windowMs > 500.0f) g_windowMs = 500.0f;
    g_invertY   = GetPrivateProfileIntA("Mouse", "InvertY", 0, g_ini);
    en          = GetPrivateProfileIntA("Mouse", "Enabled", 1, g_ini);
    if (initial || en != prevEnabledIni) g_enabled = en;
    prevEnabledIni = en;
    if (g_nativeSens < 0.01f) g_nativeSens = 0.01f;
    if (g_nativeSens > 20.0f) g_nativeSens = 20.0f;
    /* LinearMouse / StickLinear: applied once at startup only (hot reload never re-applies) */
    g_linearMouse = GetPrivateProfileIntA("Mouse", "LinearMouse", 1, g_ini);
    g_stickLinear = GetPrivateProfileIntA("Mouse", "StickLinear", 0, g_ini);
    md = GetPrivateProfileIntA("Mouse", "MouseMode", 1, g_ini) != 0;
    if (initial || md != prevModeIni) SetMouseMode(md);
    prevModeIni = md;
    g_modeVk    = GetPrivateProfileIntA("Mouse", "ModeKey", 0, g_ini);
    g_menuMouse = GetPrivateProfileIntA("Mouse", "MenuMouse", 0, g_ini);
    g_buttons   = GetPrivateProfileIntA("Mouse", "MouseButtons", 0, g_ini);
    g_usePause  = GetPrivateProfileIntA("Mouse", "UsePauseFlag", 1, g_ini);
    g_stripStart = GetPrivateProfileIntA("Mouse", "StripMouseOnStart", 1, g_ini) != 0;
    g_toggleVk  = GetPrivateProfileIntA("Mouse", "ToggleKey", 0, g_ini);
    g_slowerVk  = GetPrivateProfileIntA("Mouse", "SlowerKey", 0, g_ini);
    g_fasterVk  = GetPrivateProfileIntA("Mouse", "FasterKey", 0, g_ini);
    g_logLevel  = GetPrivateProfileIntA("Mouse", "LogLevel", 1, g_ini);
    StampIni();
}

static void LoadSettings(HMODULE self)
{
    char *slash;
    GetModuleFileNameA(self, g_ini, MAX_PATH);
    slash = strrchr(g_ini, '\\');
    if (!slash) { g_ini[0] = 0; return; }
    strcpy(slash + 1, "xinput_mouse.ini");
    strcpy(g_trace, g_ini);
    strcpy(g_trace + (slash - g_ini) + 1, "xinput_trace.csv");
    strcpy(g_ntrace, g_ini);
    strcpy(g_ntrace + (slash - g_ini) + 1, "xinput_native.csv");
    ReadSettings(1);
}

static void SettingsSummary(char *out, size_t n)
{
    _snprintf(out, n, "mode=%d nativeSens=%.3f natSmooth=%.1fms/%.0f locked=%d/%.3fms linear=%d stickLinear=%d modeKey=%d enabled=%d loglevel=%d sens=%.3f sensY=%.2f invertY=%d curve=%.2f deadComp=%.2f smoothMode=%d smoothMs=%.1f override=%.2f menuMouse=%d buttons=%d usePause=%d stripStart=%d keys=%d/%d/%d",
        g_mouseMode, g_nativeSens, g_natTauMs, g_natMaxSpeed, g_natLocked, g_natFrameMs, g_linearMouse, g_stickLinear, g_modeVk,
        g_enabled, g_logLevel, g_sens, g_sensY, g_invertY, g_curve, g_deadComp, g_smoothMode, g_windowMs,
        g_override, g_menuMouse, g_buttons, g_usePause, g_stripStart, g_toggleVk, g_slowerVk, g_fasterVk);
    out[n - 1] = 0;
}

/* called from the user-0 GetState path; cheap: one tick compare per call, one stat per second */
static void CheckReload(void)
{
    static DWORD last;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    DWORD now = GetTickCount();
    if (!g_ini[0] || now - last < 1000) return;
    last = now;
    if (!GetFileAttributesExA(g_ini, GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &g_iniTime) == 0) return;
    ReadSettings(0);
    {
        char sum[600];
        SettingsSummary(sum, sizeof sum);
        Log("settings reloaded: %s", sum);
    }
}

static void SaveSetting(const char *key, const char *fmt, double v)
{
    char buf[64];
    if (!g_ini[0]) return;
    sprintf(buf, fmt, v);
    WritePrivateProfileStringA("Mouse", key, buf, g_ini);
    StampIni();                      /* our own write is not a reload trigger */
}

/* ---- shared state ---- */
static HMODULE g_engine;                       /* prototype2engine.dll */
static volatile LONG g_dx, g_dy;               /* mouse counts taken from the game, awaiting the pad poll */
static volatile LONG g_btn[3];                 /* mouse buttons 0-2 held (when MouseButtons=1) */
static volatile LONG g_padOk;                  /* pad 0 answered the last XInputGetState */
static volatile LONG g_resetReq;               /* ask the pad thread to drop its smoothing history */
static volatile LONG g_menuPacked = -1;        /* cursor | paused << 8, for transition logging */
static volatile LONG g_menu;                   /* 1 while a menu is up */
static volatile LONG g_lockPacked = -2;        /* controller-lock flag for transition logging */
static volatile LONG g_unlocked;               /* 1 while the controller-lock flag reads 0 (press-start screen) */
static volatile LONG g_stolenX, g_stolenY;     /* running totals of counts removed from the game (summary only) */
static volatile LONG g_hookGds, g_hookGdd;     /* mouse hook calls (summary only) */

/* return address as an engine RVA (or raw address when outside the engine) */
static void RetStr(void *ra, char *out, size_t n)
{
    BYTE *eng = (BYTE *)g_engine;
    if (eng && (BYTE *)ra >= eng && (BYTE *)ra < eng + 0x4000000)
        _snprintf(out, n, "engine+0x%X", (unsigned)((BYTE *)ra - eng));
    else
        _snprintf(out, n, "0x%p (outside engine)", ra);
    out[n - 1] = 0;
}

/* ---- menu detection ---- */
static int ReadMenuRaw(int *cursor, int *paused, int *lock)
{
    BYTE *b = (BYTE *)g_engine;
    *cursor = *paused = 0; *lock = -1;
    if (!b) return 0;
    __try { *cursor = *(volatile BYTE *)(b + 0xCE9CC6); } __except (EXCEPTION_EXECUTE_HANDLER) { *cursor = -1; }
    __try {
        DWORD p = *(volatile DWORD *)(b + 0xDFA2BC);
        *paused = p ? *(volatile BYTE *)(p + 0xA8) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { *paused = -1; }
    /* engine VA 0x10E19741: controller locked flag, 0 on press-start screen, 1 once a pad is locked */
    __try { *lock = *(volatile BYTE *)(b + 0xE19741); } __except (EXCEPTION_EXECUTE_HANDLER) { *lock = -1; }
    return 1;
}

/* Recomputes g_menu from the engine; logs transitions of either raw value. */
static int UpdateMenu(void)
{
    int cur, pau, menu, lock;
    LONG packed;
    if (!ReadMenuRaw(&cur, &pau, &lock)) return 0;
    if (InterlockedExchange(&g_lockPacked, lock) != lock && g_logLevel >= 1)
        Log("controller lock: %s (flag=%d)", lock == 0 ? "unlocked (press-start)" : lock > 0 ? "locked" : "unreadable", lock);
    InterlockedExchange(&g_unlocked, lock == 0);
    menu = (cur > 0) || (g_usePause && pau > 0);
    packed = (LONG)((cur & 0xFF) | ((pau & 0xFF) << 8));
    if (InterlockedExchange(&g_menuPacked, packed) != packed) {
        if (g_logLevel >= 1)
            Log("menu state: %s (FEcursor=%d paused=%d)", menu ? "MENU" : "gameplay", cur, pau);
        /* drop held buttons / stale motion across the switch */
        InterlockedExchange(&g_btn[0], 0); InterlockedExchange(&g_btn[1], 0); InterlockedExchange(&g_btn[2], 0);
        InterlockedExchange(&g_dx, 0); InterlockedExchange(&g_dy, 0);
        InterlockedExchange(&g_resetReq, 1);
        InterlockedExchange(&g_natReset, 1);
    }
    InterlockedExchange(&g_menu, menu);
    return menu;
}

/* ---- velocity estimation ----
   Mode 1 (default): per-poll exponential smoothing with time constant tau = SmoothingMs,
     v += (counts/dt - v) * (1 - exp(-dt/tau)).  Counts that arrive between polls stay in
     g_dx/g_dy (Interlocked) until the next user-0 poll consumes them, so none are lost.
   Mode 0: sum of counts over the last SmoothingMs (ported from the CoJ proxy). */
#define NSAMP 128
static struct { double t; LONG dx, dy; } g_samp[NSAMP];
static int g_head, g_count;
static LARGE_INTEGER g_freq;
static double g_lastT;            /* QPC seconds of the previous consuming poll, 0 = none since reset */
static float g_velX, g_velY;
static LONG  g_rawDx, g_rawDy;    /* counts consumed by the latest poll (trace) */
static double g_pollDt;           /* seconds between the last two polls (trace) */
static short g_injX, g_injY;      /* last injected stick values (summary only) */

static void ResetMouse(void)
{
    InterlockedExchange(&g_dx, 0);
    InterlockedExchange(&g_dy, 0);
    InterlockedExchange(&g_resetReq, 1);
    InterlockedExchange(&g_natReset, 1);
}

static float g_remX, g_remY;      /* native-mode sub-count remainders */

static void SetMouseMode(int m)
{
    if (m == 1 && g_patchesDone && !g_p1ok) {
        Log("MouseMode=1 refused: patch P1 (mouse gate) not applied, staying in mode 0");
        m = 0;
    }
    if (m != g_mouseMode) Log("MouseMode -> %d (%s)", m, m ? "native" : "stick");
    g_mouseMode = m;
    g_remX = g_remY = 0.0f;
    InterlockedExchange(&g_stickBusy, 0);
    ResetMouse();
}

/* Called only from the user-0 XInputGetState path (the sole consumer of g_dx/g_dy). */
static void UpdateMouseState(void)
{
    LARGE_INTEGER now;
    double t, win;
    LONG dx, dy, sx = 0, sy = 0;
    int i, kept = 0;
    if (InterlockedExchange(&g_resetReq, 0)) { g_count = 0; g_velX = g_velY = 0.0f; g_lastT = 0.0; }
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&now);
    t = (double)now.QuadPart / (double)g_freq.QuadPart;
    g_rawDx = g_rawDy = 0;

    if (g_smoothMode) {
        double dt, tau, a;
        if (g_lastT == 0.0) {                 /* first poll after a reset: just start the clock */
            g_lastT = t; g_pollDt = 0.0;
            return;
        }
        dt = t - g_lastT;
        if (dt < 0.0005) return;              /* polled twice in <0.5 ms: leave the counts for the next poll */
        g_lastT = t;
        g_pollDt = dt;
        if (dt > 0.1) dt = 0.1;
        dx = InterlockedExchange(&g_dx, 0);
        dy = InterlockedExchange(&g_dy, 0);
        g_rawDx = dx; g_rawDy = dy;
        tau = g_windowMs / 1000.0;
        if (tau < 0.001) tau = 0.001;
        a = 1.0 - exp(-dt / tau);
        g_velX = (float)(g_velX + ((double)dx / dt - g_velX) * a);
        g_velY = (float)(g_velY + ((double)dy / dt - g_velY) * a);
        return;
    }

    win = g_windowMs / 1000.0;
    if (win < 0.005) win = 0.005;
    if (win > 0.2) win = 0.2;
    g_pollDt = g_lastT != 0.0 ? t - g_lastT : 0.0;
    g_lastT = t;
    dx = InterlockedExchange(&g_dx, 0);
    dy = InterlockedExchange(&g_dy, 0);
    g_rawDx = dx; g_rawDy = dy;
    if (dx || dy) {
        g_samp[g_head].t = t; g_samp[g_head].dx = dx; g_samp[g_head].dy = dy;
        g_head = (g_head + 1) % NSAMP;
        if (g_count < NSAMP) g_count++;
    }
    for (i = 0; i < g_count; i++) {
        int k = (g_head - 1 - i + NSAMP) % NSAMP;
        if (t - g_samp[k].t > win) break;
        sx += g_samp[k].dx; sy += g_samp[k].dy; kept++;
    }
    g_count = kept;
    g_velX = (float)(sx / win);
    g_velY = (float)(sy / win);
}

/* ---- native-mode smoothing: distance-preserving exponential release ----
   Per axis: pending += counts; each poll releases pending * (1 - exp(-dt/tau)); the
   integer part is emitted, the fraction is carried. pending + carry + emitted is conserved
   (nothing is lost, only delayed ~tau). With NativeSmoothingMaxSpeed > 0, tau is scaled
   down linearly from full (speed <= max) to zero (speed >= 2*max) using an EMA of
   |counts|/dt, so fast flicks pass instantly. */
typedef struct { double pending, carry, lastT, speed, dt, accIn; } NATAX;
static NATAX g_nat[2];
static double g_natPollT, g_natPollDt;   /* locked mode: QPC of the last non-duplicate poll; measured dt of the current poll */
static int    g_natDupNow;               /* current poll is a duplicate within the same frame */

#define NTRACE_RING 8192
typedef struct { float t_ms, dt_ms, pendX, pendY; LONG inX, inY, outX, outY; int locked; } NREC;
static NREC g_ntr[NTRACE_RING];
static volatile LONG g_ntHead, g_ntTail;
static double g_ntT0;

static int NatOn(void) { return g_natTauMs > 0.0f; }

static double QpcNow(void)
{
    LARGE_INTEGER q;
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&q);
    return (double)q.QuadPart / (double)g_freq.QuadPart;
}

static void NatCheckReset(void)
{
    if (InterlockedExchange(&g_natReset, 0)) { memset(g_nat, 0, sizeof g_nat); g_natPollT = 0.0; g_natPollDt = 0.0; g_natDupNow = 0; }
}

/* once per mouse poll (before the axes): frame-lock bookkeeping */
static void NatPoll(double t)
{
    g_natDupNow = 0;
    g_natPollDt = g_natPollT > 0.0 ? t - g_natPollT : 0.0;
    if (!g_natLocked) { g_natPollT = t; return; }
    if (g_natPollT > 0.0 && g_natPollDt < 0.001) {
        g_natDupNow = 1;                         /* second poll in the same frame: keep the first poll time */
        InterlockedIncrement(&g_natDups);
        return;
    }
    g_natPollT = t;
}

static LONG NatAxis(int a, double t, LONG in)
{
    NATAX *n = &g_nat[a];
    double dt, tau, f, out, total;
    LONG emit;
    n->pending += (double)in;
    if (g_natLocked) {
        /* release per displayed frame: nominal frame time, not the jittery poll moment */
        if (g_natDupNow) { n->accIn += (double)in; return 0; }   /* same frame: input waits, nothing emitted */
        dt = g_natFrameMs / 1000.0;
        n->dt = g_natPollDt > 0.0 ? g_natPollDt : dt;
        n->accIn += (double)in;                  /* speed below: counts of this whole frame */
    } else {
        dt = n->lastT > 0.0 ? t - n->lastT : 0.016;
        n->lastT = t;
        if (dt < 0.001) dt = 0.001;
        if (dt > 0.1) dt = 0.1;
        n->dt = dt;
    }
    tau = g_natTauMs / 1000.0;
    if (g_natMaxSpeed > 0.0f) {
        double k, cnt = g_natLocked ? fabs(n->accIn) : fabs((double)in);
        n->speed += ((cnt / dt) - n->speed) * (1.0 - exp(-dt / 0.05));
        k = (n->speed - g_natMaxSpeed) / g_natMaxSpeed;
        if (k < 0.0) k = 0.0;
        if (k > 1.0) k = 1.0;
        tau *= (1.0 - k);
    }
    n->accIn = 0.0;
    f = tau > 1e-5 ? 1.0 - exp(-dt / tau) : 1.0;
    out = n->pending * f;
    total = out + n->carry;
    emit = (LONG)total;                 /* toward zero */
    n->carry = total - (double)emit;
    n->pending -= out;
    return emit;
}

static void NatTrace(double t, LONG inX, LONG inY, LONG outX, LONG outY)
{
    LONG h = g_ntHead;
    NREC *r;
    if (g_logLevel < 3 || h - g_ntTail >= NTRACE_RING) return;
    if (g_ntT0 == 0.0) g_ntT0 = t;
    r = &g_ntr[h % NTRACE_RING];
    r->t_ms = (float)((t - g_ntT0) * 1000.0);
    r->dt_ms = (float)((g_natLocked ? g_natPollDt : g_nat[0].dt) * 1000.0);
    r->locked = g_natLocked;
    r->inX = inX; r->inY = inY; r->outX = outX; r->outY = outY;
    r->pendX = (float)g_nat[0].pending; r->pendY = (float)g_nat[1].pending;
    MemoryBarrier();
    g_ntHead = h + 1;
}

/* ---- gameplay trace (LogLevel 3): xinput_trace.csv ----
   The pad thread pushes fixed records into a ring; a low-priority thread formats and
   writes them in blocks (file opened once). Records are dropped when the ring is full. */
#define TRACE_RING 8192
#define TRACE_MAX_BYTES (20u * 1024u * 1024u)
typedef struct { float t_ms, dt_ms, velX, velY; LONG rdx, rdy; short realRX, realRY, outRX, outRY; int menu; } TREC;
static TREC g_tr[TRACE_RING];
static volatile LONG g_trHead, g_trTail;     /* producer / consumer counters */
static double g_trT0;

static void TracePush(double t, short realRX, short realRY, short outRX, short outRY)
{
    LONG h = g_trHead;
    TREC *r;
    if (h - g_trTail >= TRACE_RING) return;   /* full: drop */
    if (g_trT0 == 0.0) g_trT0 = t;
    r = &g_tr[h % TRACE_RING];
    r->t_ms = (float)((t - g_trT0) * 1000.0);
    r->dt_ms = (float)(g_pollDt * 1000.0);
    r->rdx = g_rawDx; r->rdy = g_rawDy;
    r->velX = g_velX; r->velY = g_velY;
    r->realRX = realRX; r->realRY = realRY; r->outRX = outRX; r->outRY = outRY;
    r->menu = (int)g_menu;
    MemoryBarrier();
    g_trHead = h + 1;
}

static DWORD WINAPI TraceThread(LPVOID unused)
{
    FILE *f = NULL;
    unsigned long bytes = 0;
    int capped = 0;
    static char buf[64 * 1024];
    FILE *nf = NULL;
    unsigned long nbytes = 0;
    int ncapped = 0;
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        LONG t, h;
        Sleep(500);
        {   /* native-mode smoothing records -> xinput_native.csv (opened once) */
            LONG nt = g_ntTail, nh = g_ntHead;
            if (g_logLevel < 3 || !g_ntrace[0] || ncapped) { g_ntTail = nh; }
            else {
                while (nt != nh) {
                    int n = 0;
                    while (nt != nh && n < (int)sizeof buf - 200) {
                        NREC *r = &g_ntr[nt % NTRACE_RING];
                        n += sprintf(buf + n, "%.1f,%.1f,%ld,%ld,%ld,%ld,%.2f,%.2f,%d\n",
                            r->t_ms, r->dt_ms, r->inX, r->inY, r->outX, r->outY, r->pendX, r->pendY, r->locked);
                        nt++;
                    }
                    if (!nf) {
                        nf = fopen(g_ntrace, "wb");
                        if (!nf) { ncapped = 1; break; }
                        nbytes = (unsigned long)fprintf(nf, "t_ms,dt_ms,in_dx,in_dy,out_dx,out_dy,pend_x,pend_y,frame_locked\n");
                    }
                    fwrite(buf, 1, n, nf);
                    nbytes += n;
                    if (nbytes >= TRACE_MAX_BYTES) { ncapped = 1; Log("native trace file reached %lu bytes, tracing stopped", nbytes); break; }
                }
                if (nf) fflush(nf);
                g_ntTail = ncapped ? g_ntHead : nt;
            }
        }
        t = g_trTail; h = g_trHead;
        if (g_logLevel < 3 || !g_trace[0] || capped) { g_trTail = h; continue; }   /* discard */
        while (t != h) {
            int n = 0;
            while (t != h && n < (int)sizeof buf - 160) {
                TREC *r = &g_tr[t % TRACE_RING];
                n += sprintf(buf + n, "%.3f,%.3f,%ld,%ld,%.1f,%.1f,%d,%d,%d,%d,%d\n",
                    r->t_ms, r->dt_ms, r->rdx, r->rdy, r->velX, r->velY,
                    r->realRX, r->realRY, r->outRX, r->outRY, r->menu);
                t++;
            }
            if (!f) {
                f = fopen(g_trace, "wb");
                if (!f) { capped = 1; break; }
                bytes = (unsigned long)fprintf(f, "t_ms,dt_ms,raw_dx,raw_dy,velX,velY,realRX,realRY,outRX,outRY,menu\n");
            }
            fwrite(buf, 1, n, f);
            bytes += n;
            if (bytes >= TRACE_MAX_BYTES) { capped = 1; Log("trace file reached %lu bytes, tracing stopped", bytes); break; }
        }
        if (f) fflush(f);
        g_trTail = capped ? g_trHead : t;
    }
    return 0;
}

static short ClampS(float v)
{
    if (v > 32767.0f) return 32767;
    if (v < -32767.0f) return -32767;
    return (short)v;
}

static void HandleHotkeys(void)
{
    static int prev[4];
    int vk[4], i, focused;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    vk[0] = g_toggleVk; vk[1] = g_slowerVk; vk[2] = g_fasterVk; vk[3] = g_modeVk;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    focused = pid == GetCurrentProcessId();
    for (i = 0; i < 4; i++) {
        int down = focused && vk[i] && (GetAsyncKeyState(vk[i]) & 0x8000) != 0;
        if (down && !prev[i]) {
            switch (i) {
            case 0: g_enabled = !g_enabled; ResetMouse();
                    if (g_logLevel >= 1) Log("hotkey: mouse mixing %s", g_enabled ? "ON" : "OFF"); break;
            case 1: g_sens *= 0.9f; if (g_sens < 0.02f) g_sens = 0.02f;
                    SaveSetting("Sensitivity", "%.3f", g_sens);
                    if (g_logLevel >= 1) Log("hotkey: sensitivity %.3f", g_sens); break;
            case 2: g_sens *= 1.1f; if (g_sens > 10.0f) g_sens = 10.0f;
                    SaveSetting("Sensitivity", "%.3f", g_sens);
                    if (g_logLevel >= 1) Log("hotkey: sensitivity %.3f", g_sens); break;
            case 3: SetMouseMode(!g_mouseMode);
                    if (g_logLevel >= 1) Log("hotkey: MouseMode now %d", g_mouseMode); break;
            }
        }
        prev[i] = down;
    }
}

/* ---- XInput diagnostics + mixing ---- */
static PFN_GetState p_PadState;                /* our export, as seen through Steam's detour */
static PFN_SetState p_PadSet;
static PFN_GetCaps  p_PadCaps;

static LONG  g_gsCalls[4];
static LONG  g_gsLast[4] = { -1, -1, -1, -1 };
static LONG  g_gsRetSeen[4];
static LONG  g_gcCalls[4];
static LONG  g_gcLast[4] = { -1, -1, -1, -1 };
static LONG  g_gcRetSeen[4];
static BYTE  g_gcType[4] = { 0xFF, 0xFF, 0xFF, 0xFF }, g_gcSub[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
static LONG  g_ssCalls;
#define FEW 5

static DWORD WINAPI MixedGetState(DWORD user, XINPUT_STATE *st)
{
    void *ra = _ReturnAddress();
    DWORD r = p_PadState ? p_PadState(user, st) : ERROR_DEVICE_NOT_CONNECTED;
    LONG n = 0;
    if (user < 4) {
        n = InterlockedIncrement(&g_gsCalls[user]);
        if (g_logLevel >= 1) {
            if (!InterlockedExchange(&g_gsRetSeen[user], 1)) {
                char rs[64]; RetStr(ra, rs, sizeof rs);
                Log("GetState user %lu first call from %s", user, rs);
            }
            if (g_gsLast[user] != (LONG)r) {
                Log("GetState user %lu: %s (rc=%lu, call #%ld)", user,
                    r == ERROR_SUCCESS ? "CONNECTED" : r == ERROR_DEVICE_NOT_CONNECTED ? "disconnected" : "error", r, n);
                g_gsLast[user] = (LONG)r;
            }
        }
    }
    if (user != 0) return r;

    /* user 0 only: everything below (incl. consuming g_dx/g_dy) runs once per poll of pad 0 */
    InterlockedExchange(&g_padOk, r == ERROR_SUCCESS);
    CheckReload();
    UpdateMenu();
    HandleHotkeys();

    if (r == ERROR_SUCCESS && st && g_enabled && !g_menu && g_mouseMode == 1) {
        /* native mode: the stick is never modified; only track whether the real stick wins */
        float sx = st->Gamepad.sThumbRX / 32767.0f, sy = st->Gamepad.sThumbRY / 32767.0f;
        LONG busy = sqrtf(sx * sx + sy * sy) > g_override;
        if (busy && !g_stickBusy) { g_remX = g_remY = 0.0f; InterlockedExchange(&g_natReset, 1); }
        InterlockedExchange(&g_stickBusy, busy);
        g_injX = g_injY = 0;
    } else if (r == ERROR_SUCCESS && st && g_enabled && !g_menu) {
        float sx, sy;
        int busy;
        short realRX = st->Gamepad.sThumbRX, realRY = st->Gamepad.sThumbRY;
        UpdateMouseState();
        sx = st->Gamepad.sThumbRX / 32767.0f;
        sy = st->Gamepad.sThumbRY / 32767.0f;
        busy = sqrtf(sx * sx + sy * sy) > g_override;
        if (busy) {
            /* the real right stick wins; drop mouse motion made meanwhile so it
               doesn't jump in after the stick is released */
            ResetMouse();
            g_velX = g_velY = 0.0f; g_lastT = 0.0; g_count = 0;
            g_injX = g_injY = 0;
        } else {
            float mx = g_velX / 1000.0f * g_sens;
            float my = g_velY / 1000.0f * g_sens * g_sensY * (g_invertY ? 1.0f : -1.0f);
            float mm = sqrtf(mx * mx + my * my);
            g_injX = g_injY = 0;
            if (mm > 0.01f) {
                float m = mm;
                if (g_curve != 1.0f) m = powf(m, g_curve);
                m = m * (1.0f - g_deadComp) + g_deadComp;
                if (m > 1.0f) m = 1.0f;
                g_injX = st->Gamepad.sThumbRX = ClampS(mx * m / mm * 32767.0f);
                g_injY = st->Gamepad.sThumbRY = ClampS(my * m / mm * 32767.0f);
            }
        }
        if (g_logLevel >= 3) {
            LARGE_INTEGER q; QueryPerformanceCounter(&q);
            TracePush((double)q.QuadPart / (double)g_freq.QuadPart, realRX, realRY, st->Gamepad.sThumbRX, st->Gamepad.sThumbRY);
        }
        if (g_buttons) {
            if (g_btn[0]) st->Gamepad.bRightTrigger = 255;
            if (g_btn[1]) st->Gamepad.bLeftTrigger = 255;
            if (g_btn[2]) st->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        }
    } else {
        ResetMouse();
        g_velX = g_velY = 0.0f;
        g_injX = g_injY = 0;
        InterlockedExchange(&g_stickBusy, 0);
    }

    if (g_logLevel >= 2) {
        static DWORD last;
        DWORD now = GetTickCount();
        if (now - last >= 5000) {
            last = now;
            if (r == ERROR_SUCCESS && st)
                Log("summary: mode=%d native passed=%ld stripped=%ld dups=%ld stickBusy=%ld | pad connected btn=0x%04X LT=%u RT=%u L=(%d,%d) R=(%d,%d) | menu=%ld lock=%ld mouse vel=(%.0f,%.0f)/s inj=(%d,%d) stolen=(%ld,%ld) hooks GDS=%ld GDD=%ld",
                    g_mouseMode, g_natPassed, g_natStripped, g_natDups, g_stickBusy,
                    st->Gamepad.wButtons, st->Gamepad.bLeftTrigger, st->Gamepad.bRightTrigger,
                    st->Gamepad.sThumbLX, st->Gamepad.sThumbLY, st->Gamepad.sThumbRX, st->Gamepad.sThumbRY,
                    g_menu, g_lockPacked, g_velX, g_velY, g_injX, g_injY, g_stolenX, g_stolenY, g_hookGds, g_hookGdd);
            else
                Log("summary: mode=%d native passed=%ld stripped=%ld dups=%ld stickBusy=%ld | pad not connected (rc=%lu) menu=%ld lock=%ld hooks GDS=%ld GDD=%ld", g_mouseMode, g_natPassed, g_natStripped, g_natDups, g_stickBusy, r, g_menu, g_lockPacked, g_hookGds, g_hookGdd);
        }
    }
    return r;
}

static DWORD WINAPI LoggedGetCaps(DWORD user, DWORD flags, XINPUT_CAPABILITIES *c)
{
    void *ra = _ReturnAddress();
    DWORD r = p_PadCaps ? p_PadCaps(user, flags, c) : ERROR_DEVICE_NOT_CONNECTED;
    if (g_logLevel >= 1 && user < 4) {
        LONG n = InterlockedIncrement(&g_gcCalls[user]);
        BYTE type = (r == ERROR_SUCCESS && c) ? c->Type : 0xFF;
        BYTE sub  = (r == ERROR_SUCCESS && c) ? c->SubType : 0xFF;
        if (!InterlockedExchange(&g_gcRetSeen[user], 1)) {
            char rs[64]; RetStr(ra, rs, sizeof rs);
            Log("GetCapabilities user %lu first call from %s", user, rs);
        }
        if (n <= FEW || g_gcLast[user] != (LONG)r || g_gcType[user] != type || g_gcSub[user] != sub) {
            Log("GetCapabilities user %lu flags=0x%lX rc=%lu type=%u subtype=%u (call #%ld)", user, flags, r, type, sub, n);
            g_gcLast[user] = (LONG)r; g_gcType[user] = type; g_gcSub[user] = sub;
        }
    }
    return r;
}

static DWORD WINAPI LoggedSetState(DWORD user, XINPUT_VIBRATION *v)
{
    DWORD r = p_PadSet ? p_PadSet(user, v) : ERROR_DEVICE_NOT_CONNECTED;
    LONG n = InterlockedIncrement(&g_ssCalls);
    if (g_logLevel >= 1 && n <= FEW)
        Log("SetState #%ld user %lu L=%u R=%u rc=%lu", n, user, v ? v->wLeftMotorSpeed : 0, v ? v->wRightMotorSpeed : 0, r);
    return r;
}

/* ---- DirectInput mouse hooks ---- */

typedef HRESULT (STDMETHODCALLTYPE *PFN_GDS)(IDirectInputDevice8A *, DWORD, LPVOID);
typedef HRESULT (STDMETHODCALLTYPE *PFN_GDD)(IDirectInputDevice8A *, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
static PFN_GDS o_GDS;
static PFN_GDD o_GDD;
static volatile LONG g_diHooked;
static LONG g_firstGds, g_firstGdd;

/* small per-pointer classification cache, revalidated every second (a released
   device's address may be reused by another device) */
typedef struct { void *p; int mouse; DWORD tick; int logged; } DEVC;
static DEVC g_dc[16];
static volatile LONG g_dcN;

static int IsMouse(IDirectInputDevice8A *self)
{
    DWORD now = GetTickCount();
    DEVC *e = NULL;
    LONG n = g_dcN, i;
    DIDEVCAPS caps;
    int mouse;
    HRESULT hr;
    if (n > 16) n = 16;
    for (i = 0; i < n; i++) if (g_dc[i].p == (void *)self) { e = &g_dc[i]; break; }
    if (e && now - e->tick < 1000) return e->mouse;

    memset(&caps, 0, sizeof caps);
    caps.dwSize = sizeof caps;
    hr = self->lpVtbl->GetCapabilities(self, &caps);
    mouse = SUCCEEDED(hr) && ((caps.dwDevType & 0xFF) == DI8DEVTYPE_MOUSE);
    if (!e && g_dcN < 16) {
        LONG idx = InterlockedIncrement(&g_dcN) - 1;
        if (idx < 16) { e = &g_dc[idx]; e->logged = 0; e->p = (void *)self; }
    }
    if (e) {
        e->mouse = mouse; e->tick = now;
        if (!e->logged) {
            e->logged = 1;
            if (g_logLevel >= 1)
                Log("DI device %p: GetCapabilities hr=0x%08lX devtype=0x%lX (%s) flags=0x%lX axes=%lu buttons=%lu povs=%lu",
                    self, (unsigned long)hr, caps.dwDevType, mouse ? "MOUSE" : "other", caps.dwFlags,
                    caps.dwAxes, caps.dwButtons, caps.dwPOVs);
        }
    }
    return mouse;
}

/* 5 = press-start screen (controller not locked): strip ALL mouse data (X/Y/Z/buttons)
   0 = pass through, 1 = gameplay (steal X/Y into the stick), 2 = menu, cursor frozen,
   3 = native gameplay (pass, scaled by NativeSensitivity), 4 = native gameplay, real stick wins (strip) */
static int XyMode(void)
{
    if (!g_enabled || !g_padOk) return 0;
    {
        int menu = UpdateMenu();
        if (g_stripStart && g_unlocked) return 5;
        if (menu) return g_menuMouse ? 0 : 2;
    }
    if (g_mouseMode == 1) return g_stickBusy ? 4 : 3;
    return 1;
}

/* scale one axis, carrying the fractional remainder (commit=0: peek, remainder untouched) */
static LONG ScaleAxis(LONG v, float *rem, int commit)
{
    float f;
    LONG r;
    if (g_nativeSens == 1.0f) return v;
    f = (float)v * g_nativeSens + *rem;
    r = (LONG)f;                       /* truncate toward zero */
    if (commit) *rem = f - (float)r;
    return r;
}

static int ButtonsMapped(void)
{
    return g_buttons && g_enabled && g_padOk && !g_menu;
}

static HRESULT STDMETHODCALLTYPE Hook_GetDeviceState(IDirectInputDevice8A *self, DWORD cb, LPVOID data)
{
    void *ra = _ReturnAddress();
    HRESULT hr = o_GDS(self, cb, data);
    int mode;
    LONG *xy;
    if (FAILED(hr) || !data || cb < 12 || !IsMouse(self)) return hr;
    InterlockedIncrement(&g_hookGds);
    if (!InterlockedExchange(&g_firstGds, 1) && g_logLevel >= 1) {
        char rs[64]; RetStr(ra, rs, sizeof rs);
        Log("mouse GetDeviceState first call from %s (cb=%lu)", rs, cb);
    }
    xy = (LONG *)data;               /* DIMOUSESTATE/2: lX at +0, lY at +4 */
    mode = XyMode();
    if (mode != 3) InterlockedExchange(&g_natReset, 1);
    if (mode == 5) {
        memset(data, 0, cb);         /* X, Y, wheel and every button */
        return hr;
    }
    if (mode == 1) {
        InterlockedExchangeAdd(&g_dx, xy[0]);
        InterlockedExchangeAdd(&g_dy, xy[1]);
        InterlockedExchangeAdd(&g_stolenX, xy[0]);
        InterlockedExchangeAdd(&g_stolenY, xy[1]);
    }
    if (mode == 3) {
        LONG ox = xy[0], oy = xy[1];
        xy[0] = ScaleAxis(ox, &g_remX, 1);
        xy[1] = ScaleAxis(oy, &g_remY, 1);
        if (g_invertY) xy[1] = -xy[1];
        if (NatOn()) {
            double t = QpcNow();
            LONG ix = xy[0], iy = xy[1];
            NatCheckReset();
            NatPoll(t);
            xy[0] = NatAxis(0, t, ix);
            xy[1] = NatAxis(1, t, iy);
            NatTrace(t, ix, iy, xy[0], xy[1]);
        }
        if (ox || oy || xy[0] || xy[1]) InterlockedIncrement(&g_natPassed);
    } else if (mode == 4) {
        if (xy[0] || xy[1]) InterlockedIncrement(&g_natStripped);
        g_remX = g_remY = 0.0f;
        xy[0] = 0; xy[1] = 0;
    } else if (mode) { xy[0] = 0; xy[1] = 0; }
    if (ButtonsMapped()) {
        BYTE *b = (BYTE *)data + 12;
        DWORD i, nb = cb - 12 < 3 ? cb - 12 : 3;
        for (i = 0; i < nb; i++) {
            InterlockedExchange(&g_btn[i], (b[i] & 0x80) != 0);
            b[i] = 0;
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hook_GetDeviceData(IDirectInputDevice8A *self, DWORD cb,
                                                    LPDIDEVICEOBJECTDATA rg, LPDWORD inout, DWORD flags)
{
    void *ra = _ReturnAddress();
    HRESULT hr = o_GDD(self, cb, rg, inout, flags);
    int mode, mapBtn;
    BYTE *base;
    DWORD n, out = 0, i;
    int natSm = 0;
    LONG lastIdx[2] = { -1, -1 }, natOut[2] = { 0, 0 };
    if (FAILED(hr) || !rg || !inout || cb < 16 || !IsMouse(self)) return hr;
    InterlockedIncrement(&g_hookGdd);
    if (!InterlockedExchange(&g_firstGdd, 1) && g_logLevel >= 1) {
        char rs[64]; RetStr(ra, rs, sizeof rs);
        Log("mouse GetDeviceData first call from %s (cb=%lu flags=0x%lX count=%lu)", rs, cb, flags, *inout);
    }
    mode = XyMode();
    if (mode != 3) InterlockedExchange(&g_natReset, 1);
    if (mode == 5) { *inout = 0; return hr; }   /* drop every record of the mouse */
    mapBtn = ButtonsMapped();
    if (!mode && !mapBtn) return hr;
    base = (BYTE *)rg;
    n = *inout;
    if (mode == 3 && NatOn() && !(flags & DIGDD_PEEK)) {
        /* scale + invert every X/Y record, sum per axis, run the smoother once for the batch;
           the released amount goes into the LAST record of each axis (its timestamp/sequence) */
        double t;
        LONG insum[2] = { 0, 0 };
        natSm = 1;
        for (i = 0; i < n; i++) {
            DIDEVICEOBJECTDATA *e = (DIDEVICEOBJECTDATA *)(base + i * cb);
            if (e->dwOfs == DIMOFS_X || e->dwOfs == DIMOFS_Y) {
                int a = e->dwOfs == DIMOFS_Y;
                LONG raw = (LONG)e->dwData, v = ScaleAxis(raw, a ? &g_remY : &g_remX, 1);
                if (a && g_invertY) v = -v;
                if (raw) InterlockedIncrement(&g_natPassed);
                e->dwData = (DWORD)v;
                insum[a] += v;
                lastIdx[a] = (LONG)i;
            }
        }
        t = QpcNow();
        NatCheckReset();
        if (lastIdx[0] >= 0 || lastIdx[1] >= 0) NatPoll(t);
        for (i = 0; i < 2; i++)
            if (lastIdx[i] >= 0) natOut[i] = NatAxis((int)i, t, insum[i]);
        if (lastIdx[0] >= 0 || lastIdx[1] >= 0)
            NatTrace(t, insum[0], insum[1], natOut[0], natOut[1]);
    }
    for (i = 0; i < n; i++) {
        DIDEVICEOBJECTDATA *e = (DIDEVICEOBJECTDATA *)(base + i * cb);
        int keep = 1;
        switch (e->dwOfs) {
        case DIMOFS_X:
        case DIMOFS_Y:
            if (natSm) {
                int a = e->dwOfs == DIMOFS_Y;
                if ((LONG)i == lastIdx[a]) { e->dwData = (DWORD)natOut[a]; if (natOut[a] == 0) keep = 0; }
                else keep = 0;
            } else if (mode == 3) {
                LONG v = (LONG)e->dwData, o;
                int isx = e->dwOfs == DIMOFS_X;
                o = ScaleAxis(v, isx ? &g_remX : &g_remY, !(flags & DIGDD_PEEK));
                if (!isx && g_invertY) o = -o;
                e->dwData = (DWORD)o;
                if (v) InterlockedIncrement(&g_natPassed);
                if (o == 0) keep = 0;
            } else if (mode == 4) {
                if (!(flags & DIGDD_PEEK)) { InterlockedIncrement(&g_natStripped); g_remX = g_remY = 0.0f; }
                keep = 0;
            } else if (mode) {
                if (mode == 1 && !(flags & DIGDD_PEEK)) {   /* a peeked record is read again later */
                    if (e->dwOfs == DIMOFS_X) { InterlockedExchangeAdd(&g_dx, (LONG)e->dwData); InterlockedExchangeAdd(&g_stolenX, (LONG)e->dwData); }
                    else                      { InterlockedExchangeAdd(&g_dy, (LONG)e->dwData); InterlockedExchangeAdd(&g_stolenY, (LONG)e->dwData); }
                }
                keep = 0;
            }
            break;
        case DIMOFS_BUTTON0: case DIMOFS_BUTTON1: case DIMOFS_BUTTON2:
            if (mapBtn) {
                if (!(flags & DIGDD_PEEK))
                    InterlockedExchange(&g_btn[e->dwOfs - DIMOFS_BUTTON0], (e->dwData & 0x80) != 0);
                keep = 0;
            }
            break;
        }
        if (keep) {
            if (out != i) memmove(base + out * cb, e, cb);
            out++;
        }
    }
    *inout = out;
    return hr;
}

/* Create a throw-away mouse device from the system dinput8.dll, patch its (shared)
   vtable, release it. */
static int InstallDIHook(void)
{
    typedef HRESULT (WINAPI *PFN_Create)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    HMODULE h = GetModuleHandleA("dinput8.dll");
    PFN_Create create;
    IDirectInput8A *di = NULL;
    IDirectInputDevice8A *dev = NULL;
    void **vt;
    HRESULT hr;
    DWORD old;
    int already = h != NULL;
    if (g_diHooked) return 1;
    if (!h) {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        strcat(sys, "\\dinput8.dll");
        h = LoadLibraryA(sys);
        if (!h) return 0;
    }
    create = (PFN_Create)GetProcAddress(h, "DirectInput8Create");
    if (!create) return 0;
    hr = create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (LPVOID *)&di, NULL);
    if (FAILED(hr) || !di) { Log("DirectInput8Create failed hr=0x%08lX", (unsigned long)hr); return 0; }
    hr = di->lpVtbl->CreateDevice(di, &GUID_SysMouse, &dev, NULL);
    if (FAILED(hr) || !dev) {
        Log("CreateDevice(SysMouse) failed hr=0x%08lX", (unsigned long)hr);
        di->lpVtbl->Release(di);
        return 0;
    }
    vt = *(void ***)dev;
    if (vt[9] != (void *)Hook_GetDeviceState && vt[10] != (void *)Hook_GetDeviceData &&
        VirtualProtect(&vt[9], 2 * sizeof(void *), PAGE_READWRITE, &old)) {
        o_GDS = (PFN_GDS)vt[9];
        o_GDD = (PFN_GDD)vt[10];
        vt[9]  = (void *)Hook_GetDeviceState;
        vt[10] = (void *)Hook_GetDeviceData;
        VirtualProtect(&vt[9], 2 * sizeof(void *), old, &old);
        InterlockedExchange(&g_diHooked, 1);
        Log("dinput8 mouse vtable %p hooked (dinput8.dll %s at %p): GetDeviceState %p -> hook, GetDeviceData %p -> hook",
            vt, already ? "was already loaded" : "loaded by us", h, o_GDS, o_GDD);
    } else {
        Log("dinput8 vtable patch failed (vtable %p)", vt);
    }
    dev->lpVtbl->Release(dev);
    di->lpVtbl->Release(di);
    return g_diHooked != 0;
}

/* ---- exports: plain pass-throughs (Steam may detour these, see header) ---- */

DWORD WINAPI XInputGetState(DWORD user, XINPUT_STATE *st)
{
    return p_GetState ? p_GetState(user, st) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputSetState(DWORD user, XINPUT_VIBRATION *v)
{
    return p_SetState ? p_SetState(user, v) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES *c)
{
    return p_GetCaps ? p_GetCaps(user, flags, c) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI XInputEnable(BOOL e)
{
    if (p_Enable) p_Enable(e);
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD user, LPWSTR a, UINT *an, LPWSTR b, UINT *bn)
{
    return p_Guids ? p_Guids(user, a, an, b, bn) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD user, BYTE type, XINPUT_BATTERY_INFORMATION *b)
{
    return p_Battery ? p_Battery(user, type, b) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD user, DWORD reserved, XINPUT_KEYSTROKE *k)
{
    return p_Keystroke ? p_Keystroke(user, reserved, k) : ERROR_DEVICE_NOT_CONNECTED;
}

/* ---- engine code/data patches (see re/FINDINGS.md "Camera look path") ---- */

#define ENG_VA(va) ((BYTE *)g_engine + ((DWORD)(va) - 0x10000000u))

static int ReadEng(DWORD va, void *out, size_t n)
{
    __try { memcpy(out, ENG_VA(va), n); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

static int WriteEng(DWORD va, const void *src, size_t n, int code)
{
    DWORD old, tmp;
    BYTE *p = ENG_VA(va);
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(p, src, n);
    VirtualProtect(p, n, old, &tmp);
    if (code) FlushInstructionCache(GetCurrentProcess(), p, n);
    return 1;
}

static void HexStr(const BYTE *b, size_t n, char *out)
{
    size_t i;
    for (i = 0; i < n; i++) sprintf(out + i * 3, "%02X ", b[i]);
    out[n * 3] = 0;
}

typedef struct { DWORD va; BYTE orig[8], repl[8]; size_t n; } BPATCH;

/* verifies every site first; all-or-nothing per patch group */
static int ApplyBytePatches(const char *name, const BPATCH *pp, int cnt)
{
    int i;
    for (i = 0; i < cnt; i++) {
        BYTE cur[8]; char hx[40];
        if (!ReadEng(pp[i].va, cur, pp[i].n)) { Log("patch %s: unexpected bytes (unreadable at 0x%08lX), skipped", name, pp[i].va); return 0; }
        if (memcmp(cur, pp[i].orig, pp[i].n) != 0) {
            HexStr(cur, pp[i].n, hx);
            Log("patch %s: unexpected bytes %sat 0x%08lX, skipped", name, hx, pp[i].va);
            return 0;
        }
    }
    for (i = 0; i < cnt; i++)
        if (!WriteEng(pp[i].va, pp[i].repl, pp[i].n, 1)) { Log("patch %s: VirtualProtect/write failed at 0x%08lX", name, pp[i].va); return 0; }
    Log("patch %s: applied (%d site%s)", name, cnt, cnt == 1 ? "" : "s");
    return 1;
}

static int FloatNear(DWORD va, float want, float tol, float *got)
{
    if (!ReadEng(va, got, 4)) return 0;
    return fabsf(*got - want) <= tol;
}

static void ApplyPatches(void)
{
    const float one = 1.0f, zero = 0.0f;
    if (g_patchesDone || !g_engine) return;
    g_patchesDone = 1;

    {   /* P1 MouseGate */
        static const BPATCH p1[] = { { 0x1010555C, { 0x0F,0x84,0xA1,0,0,0 }, { 0x90,0x90,0x90,0x90,0x90,0x90 }, 6 } };
        g_p1ok = ApplyBytePatches("P1 MouseGate", p1, 1);
        if (!g_p1ok && g_mouseMode == 1) {
            Log("MouseMode=1 needs P1; falling back to MouseMode 0");
            SetMouseMode(0);
        }
    }
    if (g_linearMouse) {   /* P2 LinearMouse */
        float a = 0, b = 0;
        int okA = FloatNear(0x109fc880, 1520.0f, 0.5f, &a), okB = FloatNear(0x109fc87c, 653.33f, 0.05f, &b);
        if (!okA || !okB) {
            Log("patch P2 LinearMouse: unexpected bytes (floats %g / %g at 0x109fc880 / 0x109fc87c), skipped", a, b);
        } else {
            BYTE guard = 0;
            int ok = ReadEng(0x10dd0884, &guard, 1);
            Log("patch P2 LinearMouse: guard byte 0x10dd0884 = 0x%02X%s", guard, ok ? "" : " (unreadable)");
            if (WriteEng(0x109fc880, &one, 4, 0) && WriteEng(0x109fc87c, &one, 4, 0)) {
                if (ok && (guard & 1)) {
                    BYTE g2 = (BYTE)(guard & ~2);
                    WriteEng(0x10dd087c, &one, 4, 0);
                    WriteEng(0x10dd0880, &one, 4, 0);
                    WriteEng(0x10dd0884, &g2, 1, 0);
                    Log("patch P2 LinearMouse: applied; spline statics already initialised, overwrote and cleared guard bit 0x2 (guard now 0x%02X)", g2);
                } else {
                    Log("patch P2 LinearMouse: applied (spline not yet initialised)");
                }
            } else {
                Log("patch P2 LinearMouse: write failed");
            }
        }
    }
    if (g_stickLinear) {   /* P3 StickLinear: float + 4 code sites, all-or-nothing */
        static const BPATCH p3[] = {
            { 0x1010b062, { 0xF3,0x0F,0x59,0x44,0x24,0x48 }, { 0xF3,0x0F,0x10,0x44,0x24,0x48 }, 6 },
            { 0x1010b068, { 0xD9,0xC0 }, { 0xD9,0xE8 }, 2 },
            { 0x1010b054, { 0x76,0x04 }, { 0xEB,0x04 }, 2 },
            { 0x1010f1d5, { 0x6A,0x01 }, { 0x6A,0x00 }, 2 },
        };
        float dz = 0;
        if (!FloatNear(0x109fc884, 0.3f, 0.001f, &dz)) {
            Log("patch P3 StickLinear: unexpected bytes (float %g at 0x109fc884), skipped", dz);
        } else if (ApplyBytePatches("P3 StickLinear code", p3, 4)) {
            if (WriteEng(0x109fc884, &zero, 4, 0)) Log("patch P3 StickLinear: dead zone 0.3 -> 0.0 applied");
            else Log("patch P3 StickLinear: dead zone write failed");
        }
    }
}

/* ---- IAT patching ---- */

static int PatchSlot(IMAGE_THUNK_DATA *t, void *wrapper)
{
    DWORD old;
    if (!VirtualProtect(&t->u1.Function, sizeof t->u1.Function, PAGE_READWRITE, &old)) return 0;
    t->u1.Function = (DWORD_PTR)wrapper;
    VirtualProtect(&t->u1.Function, sizeof t->u1.Function, old, &old);
    return 1;
}

/* Walk prototype2engine.dll's imports, find XINPUT1_3.dll, patch slots for
   GetState / GetCapabilities / SetState (identified by value == our export, or by
   ordinal when the import name table is present). */
int WINAPI MI_Install(void)
{
    BYTE *eng;
    void *expGs, *expSs, *expGc;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    int patched = 0, found = 0;
    IMAGE_IMPORT_DESCRIPTOR *d;

    if (g_installed) return 1;
    eng = (BYTE *)GetModuleHandleA("prototype2engine.dll");
    if (!eng) return 0;
    g_engine = (HMODULE)eng;
    expGs = (void *)GetProcAddress(g_self, "XInputGetState");
    expSs = (void *)GetProcAddress(g_self, "XInputSetState");
    expGc = (void *)GetProcAddress(g_self, "XInputGetCapabilities");
    if (!expGs || !expSs || !expGc) return 0;
    p_PadState = (PFN_GetState)expGs;
    p_PadSet   = (PFN_SetState)expSs;
    p_PadCaps  = (PFN_GetCaps)expGc;

    nt = (IMAGE_NT_HEADERS *)(eng + ((IMAGE_DOS_HEADER *)eng)->e_lfanew);
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress) return 0;
    for (d = (IMAGE_IMPORT_DESCRIPTOR *)(eng + dir->VirtualAddress); d->Name; d++) {
        IMAGE_THUNK_DATA *t, *oft;
        if (_stricmp((char *)(eng + d->Name), "xinput1_3.dll") != 0) continue;
        oft = d->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(eng + d->OriginalFirstThunk) : NULL;
        if (g_logLevel >= 1)
            Log("engine base %p, XINPUT1_3 import descriptor found, IAT at %p (RVA 0x%X)", eng,
                eng + d->FirstThunk, (unsigned)d->FirstThunk);
        for (t = (IMAGE_THUNK_DATA *)(eng + d->FirstThunk); t->u1.Function; t++, oft = oft ? oft + 1 : NULL) {
            void *wrapper = NULL;
            const char *name = "?";
            DWORD ord = 0;
            if (oft && (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG32)) ord = (DWORD)IMAGE_ORDINAL32(oft->u1.Ordinal);
            found++;
            if ((void *)t->u1.Function == expGs || ord == 2)      { wrapper = MixedGetState;  name = "XInputGetState"; }
            else if ((void *)t->u1.Function == expSs || ord == 3) { wrapper = LoggedSetState; name = "XInputSetState"; }
            else if ((void *)t->u1.Function == expGc || ord == 4) { wrapper = LoggedGetCaps;  name = "XInputGetCapabilities"; }
            if (g_logLevel >= 1)
                Log("  IAT slot RVA 0x%X: value %p ordinal %lu -> %s", (unsigned)((BYTE *)t - eng), (void *)t->u1.Function, ord, wrapper ? name : "(untouched)");
            if (wrapper && PatchSlot(t, wrapper)) patched++;
        }
    }
    if (patched) {
        BYTE *code = (BYTE *)expGs;
        InterlockedExchange(&g_installed, 1);
        ApplyPatches();
        Log("installed: %d of %d xinput import slot(s) patched; export XInputGetState starts with %02x (%s)",
            patched, found, code[0], code[0] == 0xE9 ? "detoured, likely Steam Input" : "not detoured");
    }
    return patched != 0;
}

static DWORD WINAPI InstallThread(LPVOID unused)
{
    int i;
    (void)unused;
    /* DirectInput hook first: it needs no engine, only dinput8.dll (loaded by us if
       the game has not yet); retried for ~30 s. */
    for (i = 0; i < 300 && !g_diHooked; i++) {
        if (i) Sleep(100);
        InstallDIHook();
    }
    if (!g_diHooked) Log("DirectInput mouse hook FAILED after retries; mouse stays native");
    for (i = 0; i < 100 && !g_installed; i++) {
        Sleep(100);
        MI_Install();
    }
    if (!g_installed)
        Log("install failed: engine %s, XINPUT1_3 import not patched",
            GetModuleHandleA("prototype2engine.dll") ? "loaded" : "NOT loaded");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char sys[MAX_PATH];
        DisableThreadLibraryCalls(inst);
        GetSystemDirectoryA(sys, MAX_PATH);          /* SysWOW64 for a 32-bit process */
        strcat(sys, "\\xinput1_3.dll");
        g_real = LoadLibraryA(sys);
        if (g_real) {
            p_GetState  = (PFN_GetState)GetProcAddress(g_real, "XInputGetState");
            p_SetState  = (PFN_SetState)GetProcAddress(g_real, "XInputSetState");
            p_GetCaps   = (PFN_GetCaps)GetProcAddress(g_real, "XInputGetCapabilities");
            p_Enable    = (PFN_Enable)GetProcAddress(g_real, "XInputEnable");
            p_Guids     = (PFN_Guids)GetProcAddress(g_real, "XInputGetDSoundAudioDeviceGuids");
            p_Battery   = (PFN_Battery)GetProcAddress(g_real, "XInputGetBatteryInformation");
            p_Keystroke = (PFN_Keystroke)GetProcAddress(g_real, "XInputGetKeystroke");
        }
        g_self = inst;
        LoadSettings(inst);
        if (g_ini[0]) {
            strcpy(g_log, g_ini);
            strcpy(strrchr(g_log, '.'), ".log");
            DeleteFileA(g_log);
        }
        Log("xinput_mouse (Prototype 2 dual input) loaded: mode=%d nativeSens=%.3f natSmooth=%.1fms/%.0f locked=%d/%.3fms linear=%d stickLinear=%d enabled=%d loglevel=%d sens=%.3f sensY=%.2f invertY=%d curve=%.2f deadComp=%.2f smoothMode=%d smooth=%.1fms override=%.2f menuMouse=%d buttons=%d",
            g_mouseMode, g_nativeSens, g_natTauMs, g_natMaxSpeed, g_natLocked, g_natFrameMs, g_linearMouse, g_stickLinear,
            g_enabled, g_logLevel, g_sens, g_sensY, g_invertY, g_curve, g_deadComp, g_smoothMode, g_windowMs, g_override, g_menuMouse, g_buttons);
        Log("real dll: %s -> %s", sys, g_real ? "loaded" : "FAILED to load");
        CloseHandle(CreateThread(NULL, 0, InstallThread, NULL, 0, NULL));
        CloseHandle(CreateThread(NULL, 0, TraceThread, NULL, 0, NULL));
    }
    return TRUE;
}
