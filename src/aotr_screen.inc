// ================================================================ the screen's own beat (v72, 2026-10-08)
// When does the screen start its next refresh? Direct3D 9 on DXVK cannot say (GetRasterStatus is made up there), and
// a beat of our own "one refresh interval apart" drifts across the screen's: with vsync off a picture then goes up
// somewhere in the middle of a refresh, at a place that wanders.
//
// Windows itself knows: D3DKMTWaitForVerticalBlankEvent (gdi32) returns at the start of each vertical blank of the
// monitor a window is on, for any process, without touching the game's device. One small thread waits on it and
// notes the time; from the times it keeps the refresh interval (averaged) and the phase (followed by an eighth of
// each time's error, so a late wake-up of the thread moves it very little). Readers get "the first refresh at or
// after this moment". If the calls are missing or fail, or no refresh has been seen for a quarter of a second (the
// screen is off, the game is minimised on some systems), nothing is known and the callers use their own beat.
// Pure observation: nothing is changed anywhere.

struct ScrClk {
    volatile LONG seq, ok, started;
    LONG64 vb, per;                          // a recent refresh (QPC ticks, filtered) and the refresh interval
    LONG64 lastRaw; LONG n;
    HWND wnd;
};
static ScrClk g_scr;
static volatile LONG g_scrNSeen = 0, g_scrNFail = 0, g_scrJitterMaxUs = 0;

static void scrFeed(LONG64 t) {
    ScrClk& c = g_scr; const LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
    LONG64 vb = c.vb, per = c.per; LONG n = c.n;
    LONG64 d = t - c.lastRaw; c.lastRaw = t;
    if (!n || d <= 0 || d > f / 4) { n = 1; vb = t; if (per < f / 500 || per > f / 20) per = f / 60; }      // the first one, or the first after a gap
    else {
        LONG64 k = (d + per / 2) / per; if (k < 1) k = 1;
        if (k == 1 && d > f / 500 && d < f / 20) {                     // one refresh later: the interval itself
            if (n < 4) per = d; else { LONG64 e = d - per; if (e < per / 8 && e > -per / 8) per += e / (n < 64 ? 8 : 64); }
        }
        LONG64 pred = vb + k * per, err = t - pred;
        if (err > per / 3 || err < -per / 3) { vb = t; n = 1; }        // out of step: start again from here
        else { vb = pred + err / 8; if (n < 100000) ++n;
               if (n > 64) { LONG us = (LONG)((err < 0 ? -err : err) * 1000000 / f); if (us > g_scrJitterMaxUs) g_scrJitterMaxUs = us; } }
    }
    InterlockedIncrement(&c.seq);
    c.vb = vb; c.per = per; c.n = n;
    InterlockedIncrement(&c.seq);
    InterlockedExchange(&c.ok, n >= 16 ? 1 : 0);
    g_scrNSeen++;
}
// The first refresh of the screen at or after `after`. false: the screen's beat is not known (now).
static bool scrNext(LONG64 after, LONG64* vb, LONG64* per) {
    ScrClk& c = g_scr;
    if (!c.ok) return false;
    LONG64 v, p; LONG s;
    do { s = c.seq; v = c.vb; p = c.per; } while ((s & 1) || s != c.seq);
    const LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
    LONG64 d = after - v;
    if (p <= 0 || d > f / 4 || d < -f / 4) return false;               // not seen for a quarter of a second
    LONG64 k = d > 0 ? (d + p - 1) / p : -((-d) / p);
    *vb = v + k * p; *per = p;
    return true;
}
struct ScrOpen { HDC hDc; UINT hAdapter; LUID luid; UINT source; };
struct ScrWait { UINT hAdapter, hDevice, source; };
struct ScrClose { UINT hAdapter; };
static DWORD WINAPI scrThread(LPVOID) {
    ScrClk& c = g_scr;
    HMODULE g = LoadLibraryA("gdi32.dll");
    typedef LONG (WINAPI* OpenF)(ScrOpen*); typedef LONG (WINAPI* WaitF)(const ScrWait*); typedef LONG (WINAPI* CloseF)(const ScrClose*);
    typedef HDC (WINAPI* CreateDcF)(LPCSTR, LPCSTR, LPCSTR, const void*); typedef BOOL (WINAPI* DeleteDcF)(HDC);
    OpenF open = g ? (OpenF)GetProcAddress(g, "D3DKMTOpenAdapterFromHdc") : NULL;
    WaitF wait = g ? (WaitF)GetProcAddress(g, "D3DKMTWaitForVerticalBlankEvent") : NULL;
    CloseF close = g ? (CloseF)GetProcAddress(g, "D3DKMTCloseAdapter") : NULL;
    CreateDcF createDc = g ? (CreateDcF)GetProcAddress(g, "CreateDCA") : NULL; DeleteDcF deleteDc = g ? (DeleteDcF)GetProcAddress(g, "DeleteDC") : NULL;
    if (!open || !wait || !close || !createDc || !deleteDc) { logf("screen: this Windows does not offer the calls that tell when the screen refreshes - pictures keep a beat of their own."); return 0; }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    UINT adapter = 0, source = 0; LONG told = 0, fast = 0, fastEpisodes = 0; LONG64 lastT = 0; char dev[40]; dev[0] = 0;
    for (;;) {
        if (!adapter) {
            MONITORINFOEXA mi; memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi);
            HMONITOR hm = MonitorFromWindow(c.wnd, MONITOR_DEFAULTTOPRIMARY);
            if (hm && GetMonitorInfoA(hm, (MONITORINFO*)&mi)) {
                HDC dc = createDc(NULL, mi.szDevice, NULL, NULL);
                if (dc) { ScrOpen oa; memset(&oa, 0, sizeof(oa)); oa.hDc = dc; if (open(&oa) >= 0 && oa.hAdapter) { adapter = oa.hAdapter; source = oa.source; lstrcpynA(dev, mi.szDevice, sizeof(dev)); } deleteDc(dc); }
            }
            if (!adapter) { InterlockedExchange(&c.ok, 0); if (InterlockedIncrement(&g_scrNFail) == 1) logf("screen: the monitor's adapter could not be opened - pictures keep a beat of their own (trying again every second)."); Sleep(1000); continue; }
        }
        ScrWait w = { adapter, 0, source };
        LONG st = wait(&w);
        LONG64 t = qpcNow();
        if (st < 0) {                                                  // the screen is off or the adapter went away: nothing is known until it answers again
            InterlockedExchange(&c.ok, 0); c.n = 0;
            ScrClose cl = { adapter }; close(&cl); adapter = 0;
            if (InterlockedIncrement(&g_scrNFail) <= 2) logf("screen: waiting for the screen's refresh failed (0x%08X) - pictures keep a beat of their own until it answers again.", (DWORD)st);
            Sleep(250); continue;
        }
        // A wait that returns at once is not a refresh. This thread runs at time-critical priority: it must never spin.
        if (lastT && t - lastT < g_pqpf.QuadPart / 2500) {
            if (++fast >= 8) {
                InterlockedExchange(&c.ok, 0); c.n = 0; fast = 0;
                if (++fastEpisodes == 1) logf("screen: the wait for the screen's refresh answers at once instead of at a refresh - not used; pictures keep a beat of their own.");
                if (fastEpisodes >= 20) { ScrClose cl = { adapter }; close(&cl); return 0; }
                Sleep(500);
            }
            lastT = t; continue;
        }
        fast = 0; lastT = t;
        scrFeed(t);
        if (!told && c.ok && c.n >= 120) { told = 1; const LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
            logf("screen: %s refreshes every %d.%03d ms (%d.%02d Hz), measured over %d refreshes - while the camera scrolls, pictures are handed over a fixed time ahead of each refresh.", dev,
                 (int)(c.per * 1000 / f), (int)((c.per * 1000000 / f) % 1000), (int)(f / c.per), (int)((f * 100 / c.per) % 100), (int)c.n); }
    }
}
static volatile LONG g_scrFake = 0;                                     // test: refreshes made up by a thread of ours, this many microseconds apart
static void scrStart(HWND wnd) {                                       // once; the window tells which monitor
    ScrClk& c = g_scr;
    if (g_scrFake || InterlockedExchange(&c.started, 1)) return;
    c.wnd = wnd;
    HANDLE th = CreateThread(NULL, 0, scrThread, NULL, 0, NULL);
    if (th) CloseHandle(th);
}
// test: the clock as it is now
extern "C" __declspec(dllexport) int __cdecl AotrScreenClockTest(HWND wnd, char* out, int nOut) {
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    scrStart(wnd);
    const LONG64 f = g_pqpf.QuadPart; LONG64 vb = 0, per = 0; bool ok = scrNext(qpcNow(), &vb, &per);
    if (out && nOut > 200) sprintf(out, "screen clock %s: %d refreshes seen, interval %d.%03d ms, the next refresh is %d us away, the largest error of one reading against the beat %d us, %d failures",
                                   ok ? "known" : "NOT known", (int)g_scrNSeen, ok ? (int)(per * 1000 / f) : 0, ok ? (int)((per * 1000000 / f) % 1000) : 0, ok ? (int)((vb - qpcNow()) * 1000000 / f) : 0, (int)g_scrJitterMaxUs, (int)g_scrNFail);
    return ok ? 1 : 0;
}
// test: a stand-in for the screen (the real one cannot be waited for while the monitor sleeps): a thread that "sees" a
// refresh every usPer microseconds, each up to 200 us late like a real wake-up
static DWORD WINAPI scrFakeThread(LPVOID) {
    const LONG64 f = g_pqpf.QuadPart; LONG64 per = f * g_scrFake / 1000000, next = qpcNow() + per; DWORD seed = 12345;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        LONG64 left = next - qpcNow();
        if (left > f / 400) Sleep((DWORD)(left * 1000 / f) - 2);
        seed = seed * 1664525u + 1013904223u; LONG64 late = (LONG64)((seed >> 16) % 200) * f / 1000000;
        while (qpcNow() < next + late) _mm_pause();
        scrFeed(qpcNow());
        next += per;
    }
}
extern "C" __declspec(dllexport) void __cdecl AotrScreenClockFake(int usPer) {
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    if (InterlockedExchange(&g_scrFake, usPer > 0 ? usPer : 16667)) return;
    { HMODULE mm = LoadLibraryA("winmm.dll"); typedef UINT (WINAPI* tTBP)(UINT); tTBP tbp = mm ? (tTBP)GetProcAddress(mm, "timeBeginPeriod") : NULL; if (tbp) tbp(1); }
    HANDLE th = CreateThread(NULL, 0, scrFakeThread, NULL, 0, NULL); if (th) CloseHandle(th);
}
// test: the arithmetic alone, over made-up times. 0: as it must be.
extern "C" __declspec(dllexport) int __cdecl AotrScreenClockSelfTest(char* out, int nOut) {
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    ScrClk keep = g_scr; LONG keepSeen = g_scrNSeen, keepJit = g_scrJitterMaxUs; int fail = 0;
    const LONG64 f = g_pqpf.QuadPart; DWORD seed = 777;
    memset(&g_scr, 0, sizeof(g_scr));
    // a 59.94 Hz screen, each refresh seen up to 250 us late, every 50th not seen at all
    const double perT = (double)f / 59.94; double t0 = (double)qpcNow(), tt = t0; int n = 0; double worst = 0;
    for (int i = 0; i < 600; ++i) {
        tt = t0 + perT * i;
        if (i % 50 == 49) continue;
        seed = seed * 1664525u + 1013904223u; double late = (double)((seed >> 16) % 250) * (double)f / 1e6;
        scrFeed((LONG64)(tt + late)); ++n;
        if (i > 300) { LONG64 vb = 0, per = 0;                           // the first refresh after the middle of this interval must be the true next one (seen 125 us late on average: within 300 us)
            if (!scrNext((LONG64)(tt + perT * 0.5), &vb, &per)) fail |= 1;
            else { double e = (double)vb - (t0 + perT * (i + 1)); if (e < 0) e = -e; if (e > worst) worst = e; if (e > 300.0 * (double)f / 1e6) fail |= 2; } }
    }
    double perErr = (double)g_scr.per - perT; if (perErr < 0) perErr = -perErr;
    if (perErr > 25.0 * (double)f / 1e6) fail |= 4;                      // the interval to 25 us (a thousandth and a half of it; the phase is followed at every refresh)
    // a refresh that is half an interval off the beat: out of step, start again - nothing is known until sixteen more were seen
    scrFeed((LONG64)(tt + perT * 1.5));
    { LONG64 vb, per; if (scrNext((LONG64)(tt + perT * 1.5) + 1, &vb, &per)) fail |= 8; }
    // nothing seen for a quarter of a second: not known
    memset(&g_scr, 0, sizeof(g_scr)); double u0 = tt + perT * 40;
    for (int i = 0; i < 40; ++i) scrFeed((LONG64)(u0 + perT * i));
    { LONG64 vb, per; if (!scrNext((LONG64)(u0 + perT * 39.5), &vb, &per)) fail |= 16; if (scrNext((LONG64)(u0 + perT * 39) + f / 2, &vb, &per)) fail |= 32;
      if (scrNext((LONG64)(u0 + perT * 39.2), &vb, &per)) { double e = (double)vb - (u0 + perT * 40); if (e < 0) e = -e; if (e > 50.0 * (double)f / 1e6) fail |= 64; } else fail |= 64; }
    if (out && nOut > 200) sprintf(out, "screen clock arithmetic: %s (flags %02X) - over 600 refreshes of a 59.94 Hz screen seen up to 250 us late, some not at all: the interval is known to %d.%d us, the next refresh to %d us at worst; out of step and after a gap nothing is claimed",
                                   fail ? "FAILED" : "as it must be", fail, (int)(perErr * 1e6 / (double)f), (int)(perErr * 1e7 / (double)f) % 10, (int)(worst * 1e6 / (double)f));
    g_scr = keep; g_scrNSeen = keepSeen; g_scrJitterMaxUs = keepJit;
    return fail;
}

// ---------------------------------------------------------------- how DXVK puts pictures on screen, and the tear-free option
// With vsync off DXVK presents in "immediate" mode: a picture is on screen the moment the GPU has it, and where the
// screen's refresh happens to be at that moment the old and the new picture meet in a line (a tear). DXVK has a second
// mode for vsync off, "mailbox" (dxvk.tearFree = True): the newest finished picture goes up at the screen's next
// refresh - nothing waits, nothing is limited, and there is no line. Which of the two is used is DXVK's
// configuration, read once when Direct3D is started: the user's own (the DXVK_CONFIG environment variable, dxvk.conf).
// [Engine] TearFree = 1 (default 0) adds "dxvk.tearFree = True" to THIS PROCESS's copy of DXVK_CONFIG when the
// accelerator starts - before the game starts Direct3D - unless the user's text already names dxvk.tearFree. Nothing
// outside the process is changed. Checked on DXVK 2.6.2 with dxvk_mode_probe.exe: a configuration with
// vsync off gives VK_PRESENT_MODE_IMMEDIATE_KHR, the same with the addition VK_PRESENT_MODE_MAILBOX_KHR.
// What DXVK really chose is read from its own log (<exe>_d3d9.log) once pictures are shown, and said in ours.
static volatile LONG g_scrTearFreeAsked = 0, g_dxvkMode = 0, g_dxvkModeTries = 0;      // mode: 0 not known, 1 immediate, 2 mailbox, 3 vsync
static void installTearFree() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    int on = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES ? (int)GetPrivateProfileIntA("Engine", "TearFree", 0, ini) : 0;
    char env[16]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_TEARFREE", env, sizeof(env)) && env[0]) on = atoi(env);
    if (on <= 0) return;
    static char cfg[4096]; cfg[0] = 0;
    DWORD n = GetEnvironmentVariableA("DXVK_CONFIG", cfg, sizeof(cfg) - 64);
    if (n >= sizeof(cfg) - 64) { logf("screen: TearFree - DXVK_CONFIG is too long to add to; it is left as it is."); return; }
    cfg[n] = 0;
    for (const char* p = cfg; *p; ++p) if (!_strnicmp(p, "dxvk.tearFree", 13)) {
        logf("screen: TearFree - your DXVK_CONFIG already says what dxvk.tearFree is; it is left as it is.");
        InterlockedExchange(&g_scrTearFreeAsked, 2); return; }
    lstrcatA(cfg, n ? "; dxvk.tearFree = True" : "dxvk.tearFree = True");
    if (!SetEnvironmentVariableA("DXVK_CONFIG", cfg)) { logf("screen: TearFree - this process's DXVK_CONFIG could not be set (%lu).", GetLastError()); return; }
    InterlockedExchange(&g_scrTearFreeAsked, 1);
    logf("screen: TearFree - inside this process DXVK is asked for its tear-free mode (\"dxvk.tearFree = True\" added to this process's copy of DXVK_CONFIG; your own setting is not changed): with vsync off a picture then goes up at the screen's next refresh "
         "instead of in the middle of one - no tear line, and nothing waits for the screen. Which mode DXVK really took is said once pictures are shown.");
}
// Which mode DXVK presents in, from its own log; asked now and then until it is known.
static void scrDxvkModeCheck(LONG leadUs) {
    if (g_dxvkMode || g_dxvkModeTries >= 8) return;
    g_dxvkModeTries++;
    char exe[MAX_PATH], path[MAX_PATH * 2 + 32], dir[MAX_PATH]; exe[0] = 0; dir[0] = 0;
    GetModuleFileNameA(NULL, exe, MAX_PATH); const char* b = strrchr(exe, '\\'); b = b ? b + 1 : exe;
    DWORD n = GetEnvironmentVariableA("DXVK_LOG_PATH", dir, sizeof(dir));
    if (n && n < sizeof(dir)) { if (!lstrcmpiA(dir, "none")) { g_dxvkModeTries = 8; return; } wsprintfA(path, "%s\\%s_d3d9.log", dir, b); }
    else wsprintfA(path, "%s_d3d9.log", b);                            // (DXVK writes it into the current directory)
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME wr, cr, ex, kt, ut; bool fresh = GetFileTime(h, NULL, NULL, &wr) && GetProcessTimes(GetCurrentProcess(), &cr, &ex, &kt, &ut) && CompareFileTime(&wr, &cr) >= 0;
    DWORD size = GetFileSize(h, NULL), take = size > 512 * 1024 ? 512 * 1024 : size, got = 0;
    char* buf = fresh ? (char*)malloc(take + 1) : NULL;
    if (buf) { SetFilePointer(h, (LONG)(size - take), NULL, FILE_BEGIN); if (!ReadFile(h, buf, take, &got, NULL)) got = 0; buf[got] = 0; }
    CloseHandle(h);
    if (!buf) return;                                                  // (a log of an earlier session says nothing about this one)
    const char* last = NULL; for (const char* p = buf; (p = strstr(p, "Present mode: VK_PRESENT_MODE_")) != NULL; p += 20) last = p;
    LONG mode = 0;
    if (last) { last += 30; mode = !strncmp(last, "IMMEDIATE", 9) ? 1 : !strncmp(last, "MAILBOX", 7) ? 2 : !strncmp(last, "FIFO", 4) ? 3 : 0; }
    free(buf);
    if (!mode) return;
    InterlockedExchange(&g_dxvkMode, mode);
    if (mode == 1) logf("screen: DXVK presents in immediate mode (vsync off): a picture is on screen the moment the GPU has it, and where the screen's refresh is at that moment the old picture and the new one meet in a line. While the camera scrolls, "
                        "pictures are handed over %d.%d ms ahead of each refresh, which puts that line at the top or bottom edge instead of anywhere.%s", (int)(leadUs / 1000), (int)(leadUs / 100 % 10),
                        g_scrTearFreeAsked == 1 ? " TearFree was asked for, but DXVK had read its configuration before the accelerator was loaded - start the game through the accelerator's launcher for it to count." :
                                                  " (No line at all needs DXVK's mailbox mode: [Engine] TearFree = 1.)");
    else if (mode == 2) logf("screen: DXVK presents in mailbox mode: with vsync off, the newest finished picture goes up at the screen's next refresh - no tear line, nothing waits%s. While the camera scrolls, pictures are handed over %d.%d ms ahead of each refresh, so that the GPU has them done by then.",
                             g_scrTearFreeAsked == 1 ? " (asked for by [Engine] TearFree)" : "", (int)(leadUs * 2 / 1000), (int)(leadUs * 2 / 100 % 10));
    else logf("screen: DXVK presents with vsync on: every picture waits for the screen's next refresh.");
}
// test: the option's work on this process's DXVK_CONFIG (the caller sets AOTR_TEARFREE and DXVK_CONFIG first)
extern "C" __declspec(dllexport) int __cdecl AotrTearFreeTest(char* out, int nOut) {
    InterlockedExchange(&g_scrTearFreeAsked, 0);
    installTearFree();
    if (out && nOut > 0) { out[0] = 0; GetEnvironmentVariableA("DXVK_CONFIG", out, (DWORD)nOut); }
    return (int)g_scrTearFreeAsked;
}
