// ================================================================ camera scrolling at one speed (v54, 2026-10-07)
// The engine scrolls the camera by a fixed distance per rendered frame. Every scroll the player makes ends in
// W3DView::scrollBy (0x0048C774, vtable 0xBDD490 + 0x5C): the arrow keys (0x006A228C), the screen edge, the keys
// held in the look-at translator and the right-button drag (0x0083B5C9 .. 0x0083B8A4) - each builds its offset
// from speed factors in the global data and nothing else; no time is in it. With the stock 30 fps cap that is one
// speed. With the cap raised it is not: 60 frames a second in an empty corner of the map scroll twice as fast as
// stock, 25 in a battle slower than stock, and a pan across both changes speed as it goes.
//
// So the offset is scaled by (usual frame time) / (1/30 s) before the engine's function sees it: the camera moves
// at the speed the stock game has at its 30 fps, at any frame rate. The usual frame time is the mean of the last
// twelve frames (measured where the views are drawn) - the mean, so that the step is the same from frame to frame
// even when frames alternate between long and short (a logic frame, a frame without); with the pictures on a
// steady beat (aotr_pace.inc) equal steps are what makes a pan even. The scroll amount the view remembers
// (view + 0x23FC / + 0x2400, read elsewhere in the engine) is put back to what the caller passed.
// [Engine] SteadyScrollSpeed (default 1) / AOTR_SCROLL=0.

typedef DWORD (__fastcall* tScrScrollBy)(void* self, void* edx, float* delta);
static tScrScrollBy o_scrScrollBy = NULL;
static volatile LONG g_scrOn = 0, g_scrNCalls = 0, g_scrNScaled = 0, g_scrKMilli = 1000;
static float g_scrK = 1.0f;
static float g_scrMul = 1.0f;                                              // [Engine] ScrollSpeed: the speed held, as a multiple of the stock game's at 30 fps
static LONG64 g_scrDt[12]; static LONG g_scrN = 0, g_scrI = 0;

// The usual frame time of these frames: the mean of those that are not far from the median. (v54 took the plain
// mean of the last twelve. One slow frame in a pan - new ground costs a frame 100 ms now and then - raised that
// mean by a quarter for the twelve frames after it: the camera stood for a moment and then ran 25 % faster for
// half a second, every time. A frame that is far from the usual says nothing about the next one. Frames that
// alternate between two lengths, or come three long and one short, are all near the median and all counted, so
// the step stays the mean's - which is what keeps the speed right for those.)
static double scrUsual(const LONG64* dt, LONG n) {
    LONG64 v[12]; LONG m = n > 12 ? 12 : n;
    for (LONG i = 0; i < m; ++i) { LONG64 x = dt[i]; LONG j = i; while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; --j; } v[j] = x; }
    double med = (m & 1) ? (double)v[m / 2] : ((double)v[m / 2 - 1] + (double)v[m / 2]) * 0.5;
    double sum = 0; LONG k = 0;
    for (LONG i = 0; i < m; ++i) if ((double)v[i] >= med * 0.55 && (double)v[i] <= med * 1.5) { sum += (double)v[i]; ++k; }
    return k ? sum / k : med;
}
static float scrScale(const LONG64* dt, LONG n, LONG64 f) {                // the factor for these frame times (pure: the test calls it too)
    if (n < 4 || f <= 0) return 1.0f;                                     // not known yet: as the engine has it
    double k = scrUsual(dt, n) * 30.0 / (double)f;
    if (k < 0.2) k = 0.2; else if (k > 3.0) k = 3.0;                      // 150 frames a second .. 10
    return (float)k;
}
// (v59) With pan pictures on (aotr_panwarp.inc) the camera runs on the clock: the first step of a frame is as long as
// the time that really passed since the step of the frame before (a quarter of a second at most), so that where the
// camera is, is a straight line over time and a picture can be shown from where the camera is at any moment. The
// first step of a scroll, and any step when the frame before had none, is the usual frame time long as before.
static volatile LONG g_scrFrameNo = 0;
static LONG g_scrStepFrame = -2; static LONG64 g_scrStepQpc = 0; static float g_scrKClock = 1.0f;
static volatile LONG g_scrNClock = 0;
static float scrClockK(LONG64 dt, LONG64 f, bool follows, float usual) {   // pure: the factor for a step taken dt after the one before
    if (!follows || f <= 0 || dt <= f / 500) return usual;
    if (dt > f / 4) dt = f / 4;
    return (float)((double)dt * 30.0 / (double)f);
}
static void scrOnFrame(LONG64 dt) {                                        // game thread, once a rendered frame: the time since the frame before
    LONG64 f = g_pqpf.QuadPart;
    g_scrFrameNo++;
    if (dt <= f / 500 || dt >= f / 4) return;                             // a pause or a stall is not the frame rate
    g_scrDt[g_scrI] = dt; g_scrI = (g_scrI + 1) % 12; if (g_scrN < 12) g_scrN++;
    g_scrK = scrScale(g_scrDt, g_scrN, f);
    g_scrKMilli = (LONG)(g_scrK * 1000.0f);
}
// How the camera is being scrolled, counted per pan for the log: the look-at translator's one call (0x0083B8A4,
// its `this` still in esi there) carries the kind at translator + 0x148 - 1 the right-button drag, 2 held keys,
// 3 the screen edge; every other caller (the arrow keys at 0x006A23CA, script nudges) counts as 0.
static BYTE* g_scrCallerEsi = NULL;
static volatile LONG g_scrKindN[4], g_scrPanSteps = 0;
static volatile DWORD g_scrPanFirst = 0, g_scrPanLast = 0;
static DWORD __fastcall hkScrScrollBy(void* self, void* edx, float* delta);
static __declspec(naked) void hkScrScrollByStub() {
    __asm {
        mov g_scrCallerEsi, esi
        jmp hkScrScrollBy
    }
}
static __declspec(noinline) void scrCount(void* ra) {
    DWORD kind = 0;
    if (ra == (void*)(ULONG_PTR)0x0083B8A7) {
        __try { kind = *(DWORD*)(g_scrCallerEsi + 0x148); } __except (EXCEPTION_EXECUTE_HANDLER) { kind = 0; }
        if (kind > 3) kind = 0;
    }
    DWORD tk = GetTickCount();
    g_scrKindN[kind]++;
    if (!g_scrPanSteps++) g_scrPanFirst = tk;
    g_scrPanLast = tk;
    g_lpPanTick = tk;                                                      // the camera is being moved (the pan profiler looks at these frames)
}
static DWORD __fastcall hkScrScrollBy(void* self, void* edx, float* delta) {
    float k = g_scrK * g_scrMul;
    g_scrNCalls++;
    if (delta && (delta[0] != 0.0f || delta[1] != 0.0f)) {
        scrCount(_ReturnAddress());
        if (g_pwOn && g_scrOn) {                                           // pan pictures: the render thread is told when this frame's step was taken ...
            LONG fno = g_scrFrameNo;
            if (g_scrStepFrame != fno) {                                   // the first step of this frame
                LONG64 now = qpcNow();
                // ... and while pictures really are being shown from where the camera is, the camera runs on the clock
                g_scrKClock = g_panWarpActive ? scrClockK(now - g_scrStepQpc, g_pqpf.QuadPart, g_scrStepFrame == fno - 1 && g_scrStepQpc != 0, g_scrK) : g_scrK;
                g_scrStepFrame = fno; g_scrStepQpc = now;
                g_panCt = (DWORD)now | 1;                                  // the frame's mark carries it to the render thread
                if (g_panWarpActive) g_scrNClock++;
            }
            k = g_scrKClock * g_scrMul;
        }
    }
    if (!delta || !g_scrOn || k == 1.0f) return o_scrScrollBy(self, edx, delta);
    float d[2] = { delta[0] * k, delta[1] * k };
    float was[2] = { *(float*)((BYTE*)self + 0x23FC), *(float*)((BYTE*)self + 0x2400) };
    DWORD r = o_scrScrollBy(self, edx, d);
    float* kept = (float*)((BYTE*)self + 0x23FC);
    if (kept[0] == d[0] && kept[1] == d[1]) { kept[0] = delta[0]; kept[1] = delta[1]; }      // the engine took it: what it remembers is the caller's amount
    else { (void)was; }                                                                      // (it refused the scroll: nothing was stored)
    g_scrNScaled++;
    return r;
}
// ---------------------------------------------------------------- the edge-scroll ramp and the pictures' lag (v56)
// Scrolling by pushing the mouse against the screen edge does not start at full speed: the look-at translator
// (update at 0x0083B471, edge case at 0x0083B515) scales it by the time since the scroll began -
//     elapsed = timeGetTime() - start (translator + 0x14C, set at 0x0083AA43);  step *= min(elapsed / ramp, 1)
// with ramp = ScreenEdgeScrollRampTime from gamedata.ini (GlobalData + 0xAA8, in ms; the mod has 0.25 s). Keyboard
// and right-button scrolling have no ramp.
// With in-between frames, what is on screen runs behind the game by a steady amount (the in-between picture needs
// the frame after it, and pictures wait for their beat): measured, about 50 ms in a 25 fps battle. On top of a
// quarter-second ramp that is felt as "nothing happens for 0.3 s" (reported 2026-10-08). The ramp is the one thing
// in a scroll that the engine times by the clock, so it can be made up for exactly: the ramp is read as if the
// scroll had begun that lag earlier. On screen the camera then gathers speed when it would in the stock game;
// only the very first moment (the lag itself) cannot be had back.
// [Engine] ScrollRampLead = 1 (default) / 0. [Engine] EdgeScrollRampMs = -1 keeps the game's own ramp time; a
// number replaces it (0 = full speed at once) - in memory, gamedata.ini is not touched.
static LONG g_scrRampLead = 1, g_scrRampMs = -1;
static volatile LONG g_scrNRamp = 0, g_scrRampLagMs = 0;
static volatile DWORD g_scrRampStart = 0;                                  // the edge scroll now running (its start time, as the translator has it)
static volatile LONG g_scrRampStarts = 0, g_scrRampFirstRaw = -1, g_scrRampFirstGiven = -1, g_scrRampFullAt = -1, g_scrRampOf = 0;
static DWORD (__stdcall* g_scrTime)(void) = NULL;                          // timeGetTime (the test puts a stand-in here)
static DWORD scrRampElapsed(DWORD el, DWORD lagMs, LONG rampWanted, DWORD rampStock) {      // pure: the elapsed time the engine's ramp is given
    if (el > 0x0FFFFFFFu) return el;                                      // nothing sane to add to
    if (lagMs > 150) lagMs = 150;
    el += lagMs;
    if (rampWanted == 0) return 0x0FFFFFFFu;                              // no ramp: full speed at once
    if (rampWanted > 0 && rampStock > 0 && rampStock < 100000u) {
        ULONG64 e2 = (ULONG64)el * rampStock / (DWORD)rampWanted;         // the engine divides by its own ramp time: scale so that it comes out as ours
        el = e2 > 0x0FFFFFFFu ? 0x0FFFFFFFu : (DWORD)e2;
    }
    return el;
}
static DWORD __cdecl scrRampTime(BYTE* xlat) {                            // in place of timeGetTime() at 0x0083B515; the engine subtracts the scroll's start from the result
    DWORD now = g_scrTime();
    DWORD start = *(DWORD*)(xlat + 0x14C);
    LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
    DWORD lagMs = g_scrRampLead ? (DWORD)((LONG64)g_twLagTicks * 1000 / f) : 0;
    DWORD stock = 0;
    __try { stock = *(DWORD*)(*(BYTE**)(ULONG_PTR)0x00DE4364 + 0xAA8); } __except (EXCEPTION_EXECUTE_HANDLER) { stock = 0; }
    g_scrRampLagMs = (LONG)lagMs; g_scrNRamp++;
    DWORD given = scrRampElapsed(now - start, lagMs, g_scrRampMs, stock);
    if (start != g_scrRampStart) {                                        // a new edge scroll: what its first frame was given
        g_scrRampStart = start; g_scrRampStarts++;
        g_scrRampFirstRaw = (LONG)(now - start); g_scrRampFirstGiven = (LONG)(given > 100000u ? 100000u : given);
        g_scrRampFullAt = -1; g_scrRampOf = (LONG)stock;
    }
    if (g_scrRampFullAt < 0 && given >= stock) g_scrRampFullAt = (LONG)(now - start);      // full speed from this frame on
    return start + given;
}
static __declspec(naked) void scrRampStub() {
    __asm {
        push esi                                           // the translator
        call scrRampTime
        add esp, 4
        ret
    }
}
static DWORD WINAPI scrReport(LPVOID) {                    // a line per pan; and one when the factor has moved by a tenth: what speed the camera is being held to
    LONG last = 0, tick = 0, pans = 0;
    for (;;) {
        Sleep(500);
        if (g_scrPanSteps > 0 && GetTickCount() - g_scrPanLast > 1500) {  // the pan is over: how it was made
            LONG steps = InterlockedExchange(&g_scrPanSteps, 0), kn[4];
            for (int i = 0; i < 4; ++i) kn[i] = InterlockedExchange(&g_scrKindN[i], 0);
            LONG starts = InterlockedExchange(&g_scrRampStarts, 0);
            DWORD ms = g_scrPanLast - g_scrPanFirst;
            if (++pans <= 400) {
                char edge[320]; edge[0] = 0;
                if (starts > 0) {
                    LONG full = g_scrRampFullAt;
                    char fullS[96]; if (full >= 0) sprintf(fullS, "full speed %d ms after it began", (int)full); else sprintf(fullS, "it ended before full speed");
                    sprintf(edge, " | edge scrolls begun: %d; the last one: its first frame came %d ms after the mouse reached the edge and was taken as %d ms into the %d ms the game ramps over (the pictures ran %d ms behind), %s",
                            (int)starts, (int)g_scrRampFirstRaw, (int)g_scrRampFirstGiven, (int)(g_scrRampMs >= 0 ? g_scrRampMs : g_scrRampOf), (int)g_scrRampLagMs, fullS);
                }
                logf("scroll: pan #%d - %u.%u s, %d camera steps: %d by the screen edge, %d by held keys, %d by the right-button drag, %d other (arrow keys, scripts)%s",
                     (int)pans, (unsigned)(ms / 1000), (unsigned)((ms % 1000) / 100), (int)steps, (int)kn[3], (int)kn[2], (int)kn[1], (int)kn[0], edge);
            }
        }
        if (++tick % 10) continue;
        LONG k = g_scrKMilli, n = g_scrNScaled;
        LONG d = k - last; if (d < 0) d = -d;
        if (n > 0 && d >= 100) { last = k;
            int mul = (int)(g_scrMul * 100.0f + 0.5f);
            logf("scroll: the camera's step is now %d.%02d of the engine's own per frame (frames come at about %d a second; the scrolling speed is %d.%02d times the stock game's at 30 fps).",
                 (int)((k * mul / 100) / 1000), (int)(((k * mul / 100) % 1000) / 10), (int)(k > 0 ? 30000 / k : 0), mul / 100, mul % 100); }
    }
}
static void installSteadyScroll() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    int want = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES ? (int)GetPrivateProfileIntA("Engine", "SteadyScrollSpeed", 1, ini) : 1;
    char env[8]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_SCROLL", env, sizeof(env)) && env[0]) want = env[0] - '0';
    if (want <= 0) { logf("scroll: off - the camera scrolls a fixed distance per frame, so its speed follows the frame rate."); return; }
    static const BYTE kScrollBy[19] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x64, 0xA1, 0x88, 0x43, 0xDE, 0x00, 0x80, 0xB8, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x53 };
    static const BYTE kStore[12] = { 0x8B, 0x06, 0x89, 0x83, 0xFC, 0x23, 0x00, 0x00, 0x8B, 0x46, 0x04, 0x89 };          // mov eax,[esi]; mov [ebx+23FC],eax; mov eax,[esi+4]; mov [ebx+2400],eax
    static const BYTE kRet[4] = { 0xC9, 0xC2, 0x04, 0x00 };                                                             // leave; ret 4
    bool ok = false;
    __try {
        ok = !memcmp((void*)0x0048C774, kScrollBy, sizeof(kScrollBy)) && !memcmp((void*)0x0048C7D3, kStore, sizeof(kStore)) && !memcmp((void*)0x0048CA76, kRet, sizeof(kRet)) &&
             *(DWORD*)0x00BDD4EC == 0x0048C774 && *(float*)0x00BDD764 == 250.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok) { logf("scroll: the view's scroll function is not what this build has - the camera scrolls as the engine has it."); return; }
    o_scrScrollBy = (tScrScrollBy)makeTrampoline((BYTE*)0x0048C774, 6);
    if (!o_scrScrollBy) { logf("scroll: no trampoline - off."); return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)0x0048C774, (void*)&hkScrScrollByStub, 6);
    resumeAll(fz, nf);
    g_scrFrame = &scrOnFrame;
    { char b[32]; b[0] = 0; GetPrivateProfileStringA("Engine", "ScrollSpeed", "1.0", b, sizeof(b), ini);
      double m = atof(b); if (m < 0.25 || m > 4.0) m = 1.0; g_scrMul = (float)m; }
    InterlockedExchange(&g_scrOn, 1);
    CreateThread(NULL, 0, scrReport, NULL, 0, NULL);
    { int mul = (int)(g_scrMul * 100.0f + 0.5f);
      logf("scroll: installed - the camera scrolls at %d.%02d times the stock game's 30 fps speed at any frame rate (the engine moves it a fixed distance per rendered frame; that distance is scaled by the usual frame time - a single slow frame does not change it).", mul / 100, mul % 100); }
    // the edge-scroll ramp, read ahead by the pictures' lag
    g_scrRampLead = (int)GetPrivateProfileIntA("Engine", "ScrollRampLead", 1, ini) > 0 ? 1 : 0;
    g_scrRampMs = (LONG)(int)GetPrivateProfileIntA("Engine", "EdgeScrollRampMs", -1, ini);
    if (g_scrRampMs > 5000) g_scrRampMs = -1;
    static const BYTE kRamp[32] = { 0xFF, 0x15, 0x20, 0x09, 0xBD, 0x00, 0x8B, 0x0D, 0x64, 0x43, 0xDE, 0x00, 0x2B, 0x86, 0x4C, 0x01, 0x00, 0x00, 0x8B, 0x89, 0xA8, 0x0A, 0x00, 0x00, 0x3B, 0xC1, 0x7E, 0x05, 0x6A, 0x64, 0x5B, 0xEB };
    static const BYTE kStart[12] = { 0xFF, 0x15, 0x20, 0x09, 0xBD, 0x00, 0x89, 0x86, 0x4C, 0x01, 0x00, 0x00 };          // call timeGetTime; mov [esi+14C],eax
    bool rampOk = false;
    __try { rampOk = !memcmp((void*)0x0083B515, kRamp, sizeof(kRamp)) && !memcmp((void*)0x0083AA43, kStart, sizeof(kStart)); } __except (EXCEPTION_EXECUTE_HANDLER) { rampOk = false; }
    HMODULE hw = GetModuleHandleA("winmm.dll");
    g_scrTime = hw ? (DWORD(__stdcall*)(void))GetProcAddress(hw, "timeGetTime") : NULL;
    if (!rampOk || !g_scrTime) { logf("scroll: the edge-scroll ramp is not where this build has it - it runs by the game's own clock."); return; }
    if (!g_scrRampLead && g_scrRampMs < 0) { logf("scroll: the edge-scroll ramp is left to the game's own clock (ScrollRampLead = 0)."); return; }
    { DWORD op = 0; BYTE* q = (BYTE*)0x0083B515;
      HANDLE fz2[256]; int nf2 = 0;
      suspendOthers(fz2, &nf2, 256);
      if (VirtualProtect(q, 6, PAGE_EXECUTE_READWRITE, &op)) {
          q[0] = 0xE8; *(DWORD*)(q + 1) = (DWORD)(ULONG_PTR)&scrRampStub - (0x0083B515 + 5); q[5] = 0x90;
          VirtualProtect(q, 6, op, &op); FlushInstructionCache(GetCurrentProcess(), q, 6);
      }
      resumeAll(fz2, nf2); }
    if (g_scrRampMs >= 0) logf("scroll: edge scrolling gathers speed over %d ms instead of the game's own ramp time (EdgeScrollRampMs)%s.", (int)g_scrRampMs, g_scrRampLead ? ", read ahead by how far the pictures run behind the game" : "");
    else logf("scroll: edge scrolling's ramp (the game's own ScreenEdgeScrollRampTime) is read ahead by how far the pictures on screen run behind the game, so on screen the camera gathers speed when it would in the stock game.");
}

// ---------------------------------------------------------------- offline test
static DWORD g_scrTestSeen[2]; static float g_scrTestKept[2];
static DWORD __fastcall scrTestOrig(void* self, void* edx, float* delta) {
    g_scrTestSeen[0] = *(DWORD*)&delta[0]; g_scrTestSeen[1] = *(DWORD*)&delta[1];
    *(float*)((BYTE*)self + 0x23FC) = delta[0]; *(float*)((BYTE*)self + 0x2400) = delta[1];
    return 0x1234;
}
extern "C" __declspec(dllexport) int __cdecl AotrScrollTest(char* out, int n) {
    int fail = 0; const LONG64 f = 10000000;
    LONG64 dt[12];
    for (int i = 0; i < 12; ++i) dt[i] = f / 30;                          // stock: 30 frames a second -> exactly the engine's step
    float k30 = scrScale(dt, 12, f); if (k30 < 0.9999f || k30 > 1.0001f) fail |= 1;
    for (int i = 0; i < 12; ++i) dt[i] = f / 60;
    float k60 = scrScale(dt, 12, f); if (k60 < 0.4999f || k60 > 0.5001f) fail |= 2;
    for (int i = 0; i < 12; ++i) dt[i] = f / 25;
    float k25 = scrScale(dt, 12, f); if (k25 < 1.1999f || k25 > 1.2001f) fail |= 4;
    for (int i = 0; i < 12; ++i) dt[i] = (i & 1) ? f * 20 / 1000 : f * 35 / 1000;          // a logic frame, a frame without: one step for both, the mean's
    float kMix = scrScale(dt, 12, f); if (kMix < 0.8249f || kMix > 0.8251f) fail |= 8;
    if (scrScale(dt, 3, f) != 1.0f) fail |= 16;                            // too few frames: untouched
    for (int i = 0; i < 12; ++i) dt[i] = f / 500;
    if (scrScale(dt, 12, f) != 0.2f) fail |= 32;
    for (int i = 0; i < 12; ++i) dt[i] = f / 5;
    if (scrScale(dt, 12, f) != 3.0f) fail |= 64;
    // distance covered in a second is the same at every rate: step * factor * frames
    double d30 = 25.0 * k30 * 30.0, d60 = 25.0 * k60 * 60.0, d25 = 25.0 * k25 * 25.0, dMix = 25.0 * kMix * (1000.0 / 27.5);
    if (fabs(d60 - d30) > 0.01 || fabs(d25 - d30) > 0.01 || fabs(dMix - d30) > 0.01) fail |= 128;
    // the hook: the engine's function gets the scaled offset, hands its result back, and the view remembers the caller's amount
    static BYTE view[0x2410]; float in[2] = { 3.5f, -7.25f };
    tScrScrollBy keep = o_scrScrollBy; LONG keepOn = g_scrOn; float keepK = g_scrK;
    o_scrScrollBy = &scrTestOrig; g_scrOn = 1; g_scrK = 0.5f;
    DWORD r = hkScrScrollBy(view, NULL, in);
    float s0, s1; memcpy(&s0, &g_scrTestSeen[0], 4); memcpy(&s1, &g_scrTestSeen[1], 4);
    if (r != 0x1234 || s0 != 1.75f || s1 != -3.625f) fail |= 256;
    if (*(float*)(view + 0x23FC) != 3.5f || *(float*)(view + 0x2400) != -7.25f || in[0] != 3.5f || in[1] != -7.25f) fail |= 512;
    g_scrK = 1.0f; r = hkScrScrollBy(view, NULL, in);                       // factor one: the caller's own pointer goes through
    memcpy(&s0, &g_scrTestSeen[0], 4); if (r != 0x1234 || s0 != 3.5f) fail |= 1024;
    g_scrOn = 0; g_scrK = 0.5f; r = hkScrScrollBy(view, NULL, in);           // switched off
    memcpy(&s0, &g_scrTestSeen[0], 4); if (s0 != 3.5f) fail |= 2048;
    o_scrScrollBy = keep; g_scrOn = keepOn; g_scrK = keepK;
    // one slow frame among usual ones changes nothing (v54's mean made the camera a quarter faster for the next twelve frames)
    for (int i = 0; i < 12; ++i) dt[i] = f * 37 / 1000; dt[5] = f * 150 / 1000;
    float kHitch = scrScale(dt, 12, f); if (kHitch < 1.1099f || kHitch > 1.1101f) fail |= 4096;
    for (int i = 0; i < 12; ++i) dt[i] = (i % 4 == 3) ? f * 20 / 1000 : f * 33 / 1000;       // three long, one short: the mean of all of them, as before
    float k31 = scrScale(dt, 12, f); if (k31 < 0.8924f || k31 > 0.8926f) fail |= 8192;
    // the camera on the clock: a step as long as the time since the one before, the usual one when there was none, a quarter of a second at most
    if (scrClockK(f / 30, f, true, 9.0f) < 0.9999f || scrClockK(f / 30, f, true, 9.0f) > 1.0001f || scrClockK(f / 20, f, true, 9.0f) < 1.4999f || scrClockK(f / 20, f, true, 9.0f) > 1.5001f) fail |= 0x20000;
    if (scrClockK(f / 30, f, false, 1.1f) != 1.1f || scrClockK(f / 1000, f, true, 1.1f) != 1.1f || scrClockK(f * 2, f, true, 1.1f) < 7.4999f || scrClockK(f * 2, f, true, 1.1f) > 7.5001f) fail |= 0x20000;
    // the edge-scroll ramp: read ahead by the lag, a ramp time of our own, none at all, and the engine's subtraction
    if (scrRampElapsed(100, 0, -1, 250) != 100 || scrRampElapsed(100, 48, -1, 250) != 148 || scrRampElapsed(100, 400, -1, 250) != 250) fail |= 16384;      // (a lag is never taken for more than 150 ms)
    if (scrRampElapsed(60, 0, 120, 250) != 125 || scrRampElapsed(60, 20, 0, 250) != 0x0FFFFFFFu || scrRampElapsed(0x20000000u, 50, 100, 250) != 0x20000000u) fail |= 32768;
    { static BYTE xl[0x150]; *(DWORD*)(xl + 0x14C) = 1000;
      struct T { static DWORD __stdcall now() { return 1100; } };
      DWORD (__stdcall* keepT)(void) = g_scrTime; LONG keepLead = g_scrRampLead, keepMs = g_scrRampMs, keepLag = g_twLagTicks;
      if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
      g_scrTime = &T::now; g_scrRampLead = 1; g_scrRampMs = -1; g_twLagTicks = (LONG)(g_pqpf.QuadPart * 48 / 1000);
      DWORD r1 = scrRampTime(xl);                                           // 100 ms into the scroll, pictures 48 ms behind: the engine sees 148 ms
      g_scrRampLead = 0; DWORD r2 = scrRampTime(xl);
      if (r1 - 1000 != 148 || r2 - 1000 != 100) fail |= 65536;
      g_scrTime = keepT; g_scrRampLead = keepLead; g_scrRampMs = keepMs; g_twLagTicks = keepLag; }
    if (out && n > 200) sprintf(out, "steady scroll: %s (flags %05X) - factor at 30 fps %.4f, 60 fps %.4f, 25 fps %.4f, mixed 35/20 ms %.4f, a 150 ms frame among 37 ms ones %.4f (the plain mean would say %.4f); units a second at a 25-unit step: %.2f / %.2f / %.2f / %.2f; edge-scroll ramp read ahead by the pictures' lag: as it must be",
                                fail ? "FAILED" : "all cases as they must be", fail, k30, k60, k25, kMix, kHitch, (11.0 * 37.0 + 150.0) / 12.0 * 0.03, d30, d60, d25, dMix);
    return fail;
}
