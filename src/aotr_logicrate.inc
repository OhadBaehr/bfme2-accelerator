// ---------------------------------------------------------------- logic rate, separated from the frame rate
// Raising the frame cap sped the SIMULATION up (measured 1.97x at 60 fps) because the engine advances one
// logic sub-frame per rendered frame. The per-frame scheduler is 0x006325A0:
//
//     call [0xDEF548]->vtbl[0x28]          ; per-frame subsystem
//     call 0x604189([0xDE3BAC])            ; asks the external DLL's CanAppContinue, answer -> [0xDE3B9C]
//     bl = 0x603452([0xDE3BAC])            ; "render this frame WITHOUT advancing the simulation?"
//     call this->vtbl[0x9C]                ; 0x632409 - input, client, net: runs on both paths
//     if (bl) { [[0xDE4388]+0xC8] = 0; [0xDC62C0]->vtbl[0x94](); goto epilogue; }   <- render-only frame
//     [[0xDE4388]+0xC8] = 1;
//     n = ++[esi+0x34]; 0x6251A3(GameLogic[0x40]*10 + n - 1);
//     if (n <= 6) dispatcher(n); else { n = 0; dispatcher(0); }                     <- GameLogic::update(n)
//
// So the engine ALREADY has a render-only frame path, and 0x603452 is the switch. Stock answers 0 every time
// only because the answer comes from an optional external DLL (export "CanAppContinue") and none is loaded.
// Answering 1 on the surplus frames therefore holds the simulation at a fixed rate using the engine's own
// code: the logic sequence, its order and its sub-frame numbering are not touched at all.
//
// This replaces the earlier approach of pacing GameLogic::update from our own clock. That was the wrong layer
// and it is why animation broke: the scheduler's sub-frame counter (GameEngine+0x34) and the global index it
// derives (GameLogic[0x40]*10 + n - 1) advance once per RENDERED frame above the hook, so skipping the call
// underneath left the engine's own notion of sub-frame racing ahead of the logic that was actually run.
//
// Only the scheduler's call site is redirected, not 0x603452 itself: the other caller (0x00441E23) asks the
// same predicate for a different purpose and must keep its stock answer.
//
// 0x603452's own first test is `byte[this+0x1A5D9] != 0 -> never skip`, and that byte is **1 for the whole run**:
// the only instruction in the binary that writes it is `mov byte [esi+0x1A5D9], 1` at 0x0060809A, in the object's
// constructor, and nothing ever clears it. So it is not a game-state interlock, it is a constant that makes the
// CanAppContinue path dead code in a shipped build - which is exactly why the first attempt answered 0 on every
// frame (measured: "0 logic frames and 0 frames rendered only" every second, game back at 2x speed). We answer
// the question ourselves and do not consult it; 0x00441E23 still calls the real predicate and still honours it.

#define LR_CALL_AT   0x006325C4            // the "call 0x603452" inside the per-frame scheduler
#define LR_PRED      0x00603452
#define LR_GUARD_OFF 0x1A5D9               // the engine's own "never skip" guard byte, kept exactly

static ULONG64 g_lrTicks = 0;              // rdtsc ticks between logic frames; 0 = off
static ULONG64 g_lrNext  = 0;
static LONG    g_lrHz = 0, g_lrLive = 0;
static volatile LONG g_lrRan = 0, g_lrSkipped = 0, g_lrNudged = 0;

// ---- logic first (v55) ---------------------------------------------------------------------------------
// A frame without logic is only free when it fits. The rule above starts one whenever the next logic frame is
// not due YET - also when it is due in two milliseconds and the frame will take fifteen. Then the logic frame
// starts thirteen milliseconds late, the next one is counted from there, and the simulation runs below its
// rate although the machine could hold it: the log of 2026-10-07 has seconds with "36 rendered frames, 26 logic
// frames, 10 rendered only" - the game at 26 Hz with ten extra frames squeezed in, and frames of 31 and 19 ms in
// no order (which is what a pan shows). It happens exactly where a frame with logic takes between half a logic
// interval and a whole one: 30 to 60 frames a second, the seconds after a load, a middling battle.
// So a frame without logic is started only if it will be DONE before the next logic frame is due; otherwise the
// thread waits the few milliseconds for that logic frame. What such a frame costs is known without running one:
// the scheduler (0x006325A0) draws first and runs the logic update AFTER the Present, so the time from here to
// the Present is the cost of a frame without logic whether this frame has logic or not. (v55 subtracted the logic
// time from it, which is not in it: the cost came out at 10 ms instead of 23 in a battle, and frames that did not
// fit were started after all - "38 rendered, 25 logic, 13 rendered only" in the log of 2026-10-07 23:50.)
// And no frame starts sooner than the frame cap's period after the one before it (the engine's limiter at
// 0x0063A18E, when UseFPSLimit is on), so a frame without logic takes at least that long however little it draws.
// Logic stays at its rate and frames come at one interval: 60 a second while two frames fit into a logic
// interval, an even 30 when they do not. [Engine] LogicFirst (default 1) / AOTR_LOGICFIRST=0.
static volatile ULONG64 g_lrPresentTsc = 0;            // game thread: when the frame's Present was queued (aotr_rt.inc)
static ULONG64 g_lrLastDecide = 0, g_lrLogicSeen = 0;
static LONG g_lrLastWasR = 0, g_lrFirst = 1;
static volatile LONG g_lrHeld = 0;                     // frames without logic not started: the next logic frame was due sooner than they take
static volatile LONG64 g_lrHeldTicks = 0;              // ... and how long the thread waited for those logic frames (rdtsc ticks)
static volatile LONG g_lrModeFlips = 0;
static LONG g_lrCapMs = 0;                             // the frame cap we set, in ms (aotr_fpscap.inc tells); the engine's UseFPSLimit byte says whether it is in force
// What is known about the frames, smoothed: `work` = from the scheduler's question to the Present (the cost of a
// frame without logic - the logic update runs after the Present), `logic` = the logic update. Under a frame cap
// the choice is between two cadences and is made from these, not frame by frame: `two` = a frame with logic leaves
// room for a second frame before the next logic frame (cap + cap <= interval). It is left when a frame with logic
// costs more than (interval - cap), and taken up again only when it costs 2 ms less than that.
struct LrState { ULONG64 work, logic; LONG two, holding; };
static LrState g_lrS = { 0, 0, 1, 0 };
static __forceinline ULONG64 lrEma(ULONG64 est, ULONG64 x) { return est ? (est * 3 + x) / 4 : x; }
// the frame that just ended (pure: the offline test calls it too)
static void lrLearn(LrState& s, ULONG64 work, ULONG64 logic, int wasR, ULONG64 interval) {
    if (work && work <= interval * 4) s.work = lrEma(s.work, work > interval ? interval : work);     // a stall or a load screen says nothing about a frame
    if (!wasR && logic && logic <= interval * 4) s.logic = lrEma(s.logic, logic);
}
// may a frame without logic start now, `gap` before the next logic frame is due? (pure) cap: the frame cap's period,
// 0 when no limiter is in force; ms: one millisecond in the same unit.
static bool lrRoom(LrState& s, ULONG64 gap, ULONG64 cap, ULONG64 interval, ULONG64 ms, int lastWasR, LONG* flips) {
    if (!s.work) return true;                            // nothing known yet: as before
    if (cap) {
        ULONG64 lcost = s.work + s.logic, room = interval > cap ? interval - cap : 0;
        if (s.two) { if (lcost > room || s.work > cap + ms) { s.two = 0; if (flips) ++*flips; } }
        else if (lcost + 2 * ms <= room && s.work <= cap) { s.two = 1; if (flips) ++*flips; }
        return s.two && gap + ms >= cap;                 // (after the second frame the logic frame is a millisecond away: that is waited for)
    }
    ULONG64 need = s.work + (s.holding ? s.work / 4 : s.work / 16) + interval / 64;      // no limiter: its usual time, a margin, half a millisecond
    bool ok = gap >= need;
    if (ok) s.holding = 0; else if (!lastWasR) s.holding = 1;
    return ok;
}
// In the two-frame cadence the frame without logic starts at the MIDDLE of the logic interval, not as soon as the
// engine's limiter lets it (16 ms after the frame before - a whole number of milliseconds, 1000 / 60 cut off):
// frames then come 16.67 / 16.67 ms apart and not 16 / 17.33, which on a 60 Hz screen is one frame per refresh.
// How long to hold a frame without logic that has room, `gap` before the next logic frame is due. (pure)
static ULONG64 lrMidHold(const LrState& s, ULONG64 gap, ULONG64 cap, ULONG64 interval) {
    return (cap && s.two && s.work && gap > interval / 2) ? gap - interval / 2 : 0;
}

// ---- animation between logic frames ----------------------------------------------------------------
// Poses are not computed by the logic; they are computed while rendering, every frame, from the millisecond
// counter at 0x00DD1E0C (the engine keeps the previous value beside it at 0x00DD1E10 and works in deltas -
// 0x005A1D60 branches on `cur - prev`). On a render-only frame every skeleton is therefore re-evaluated
// against a timestamp that has not moved, which is what makes 30 Hz animation under a 60 fps render.
//
// So on a render-only frame we move the counter forward by the fraction of the logic interval that has really
// elapsed, and put the engine's own committed value back before the next logic frame runs. Logic never reads
// a value we wrote: the write happens after the decision to skip, and the restore happens before we let a
// logic frame through, so `prev = cur; cur = cur + step` in the setter at 0x00516E20 sees exactly what it
// would have seen. The step is MEASURED from the engine (the difference between consecutive committed
// values, normally 33 ms) rather than assumed, and smoothing is skipped for an interval whose step looks
// wrong. Off with [Engine] SmoothAnimation = 0.
#define LR_ANIMTIME 0x00DD1E0C
static LONG  g_lrSmooth = 0;
static DWORD g_lrBase = 0, g_lrPrevBase = 0, g_lrStep = 0;
static int   g_lrHaveBase = 0;

// Asked once per rendered frame. 1 = render only, 0 = also advance one logic sub-frame.
static __declspec(noinline) int __cdecl lrDecide(void) {
    ULONG64 now = __rdtsc();
    if (g_lrFirst && g_lrLastDecide) {                 // the frame that just ended: its drawing (to the Present) and its logic update (after it)
        ULONG64 pres = g_lrPresentTsc, lg = g_lgTscSum;
        lrLearn(g_lrS, pres > g_lrLastDecide ? pres - g_lrLastDecide : 0, lg - g_lrLogicSeen, g_lrLastWasR, g_lrTicks);
        g_lrLogicSeen = lg;
    }
    if (!g_lrNext) { g_lrNext = now + g_lrTicks; g_lrRan++; g_lrLastDecide = now; g_lrLastWasR = 0; g_lrLogicSeen = g_lgTscSum; return 0; }
    ULONG64 perMs = g_lrTicks * (ULONG64)g_lrHz / 1000; if (!perMs) perMs = 1;
    ULONG64 cap = (g_lrCapMs > 0 && *(volatile BYTE*)(ULONG_PTR)0x00DE4320) ? perMs * (ULONG64)g_lrCapMs : 0;      // UseFPSLimit: is the engine's limiter in force right now?
    if (now < g_lrNext && g_lrFirst) {
        ULONG64 t0 = now, until = 0;
        if (!lrRoom(g_lrS, g_lrNext - now, cap, g_lrTicks, perMs, g_lrLastWasR, (LONG*)&g_lrModeFlips)) until = g_lrNext;     // no room: wait for the logic frame instead of pushing it back
        else until = now + lrMidHold(g_lrS, g_lrNext - now, cap, g_lrTicks);                                                  // room: at the middle of the interval
        while (now < until) {                              // sleep while more than ~2 ms remain, then spin
            if (until - now > perMs * 2) Sleep(1); else _mm_pause();
            now = __rdtsc();
        }
        // counted: a frame without logic that was not started. (In the two-frame cadence the logic frame is always a
        // millisecond away after the second frame - that wait is the cadence, not a frame given up.)
        if (until == g_lrNext && (!(cap && g_lrS.two) || now - t0 > perMs * 3)) { g_lrHeld++; g_lrHeldTicks += (LONG64)(now - t0); }
    }
    if (g_lrFirst && cap) {                            // which of the two cadences it is, said when it changes (not more than every two seconds)
        static LONG saidTwo = -1; static DWORD saidAt = 0;
        if (g_lrS.two != saidTwo) { DWORD tk = GetTickCount();
            if (saidTwo < 0 || tk - saidAt > 2000) { saidTwo = g_lrS.two; saidAt = tk;
                logf("logicrate: %s (a frame's drawing takes %d.%d ms, its logic update %d.%d ms).", g_lrS.two ? "two frames to a logic frame - one with logic, one without, evenly" : "one frame to a logic frame - a second one would not be done in time",
                     (int)(g_lrS.work / perMs), (int)((g_lrS.work * 10 / perMs) % 10), (int)(g_lrS.logic / perMs), (int)((g_lrS.logic * 10 / perMs) % 10)); } }
    }
    if (now < g_lrNext) {                              // render-only frame
        g_lrLastDecide = now; g_lrLastWasR = 1;
        if (g_lrSmooth) {
            DWORD* cur = (DWORD*)LR_ANIMTIME;
            if (!g_lrHaveBase) {                       // first surplus frame of this interval: the engine's own value
                g_lrPrevBase = g_lrBase; g_lrBase = *cur; g_lrHaveBase = 1;
                DWORD d = g_lrBase - g_lrPrevBase;
                g_lrStep = (d >= 1 && d <= 200) ? d : 0;      // the engine's measured increment, or leave it alone
            }
            if (g_lrStep) {
                ULONG64 into = g_lrTicks - (g_lrNext - now);  // ticks since the logic frame, always < g_lrTicks
                DWORD add = (DWORD)(((ULONG64)g_lrStep * into) / g_lrTicks);
                if (add > g_lrStep - 1) add = g_lrStep - 1;   // never reach the next logic frame's own value
                *cur = g_lrBase + add;
                g_lrNudged++;
            }
        }
        g_lrSkipped++;
        return 1;
    }
    if (g_lrHaveBase) { *(DWORD*)LR_ANIMTIME = g_lrBase; g_lrHaveBase = 0; }   // hand the engine back its own clock
    g_lrNext += g_lrTicks;
    if (g_lrNext <= now) g_lrNext = now + g_lrTicks;   // after a stall: resync, never a burst of logic frames
    g_lrRan++;
    g_lrLastDecide = now; g_lrLastWasR = 0;
    return 0;
}

static __declspec(naked) void lrStub() {               // thiscall: ecx is ignored, answer in al
    __asm {
        call lrDecide
        ret
    }
}

static void installLogicRate() {
    char ini[MAX_PATH];
    aotrPath(ini, "bfme2_accel.ini");
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;        // no ini: logic stays tied to the frame rate
    int hz = (int)GetPrivateProfileIntA("Engine", "LogicHz", 0, ini);
    if (hz <= 0) return;
    if (hz < 5 || hz > 120) {
        logf("logicrate: [Engine] LogicHz = %d is outside 5..120 - logic stays tied to the frame rate.", hz);
        return;
    }
    double tps = g_tscPerQpc * (double)g_pqpf.QuadPart;                    // rdtsc ticks a second
    if (tps <= 0.0) { logf("logicrate: the timer is not calibrated - logic stays tied to the frame rate."); return; }
    __try {
        BYTE* at = (BYTE*)LR_CALL_AT;
        if (at[0] != 0xE8 || (DWORD)(ULONG_PTR)(at + 5 + *(LONG*)(at + 1)) != LR_PRED) {
            logf("logicrate: the frame scheduler does not look like the build this was written for - left alone.");
            return;
        }
        g_lrTicks = (ULONG64)(tps / (double)hz);
        HANDLE fz[256]; int nf = 0;
        suspendOthers(fz, &nf, 256);
        DWORD op = 0;
        if (!VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &op)) {
            resumeAll(fz, nf);
            logf("logicrate: the scheduler is not writable - logic stays tied to the frame rate.");
            return;
        }
        *(LONG*)(at + 1) = (LONG)((ULONG_PTR)&lrStub - (ULONG_PTR)(at + 5));
        VirtualProtect(at, 5, op, &op);
        FlushInstructionCache(GetCurrentProcess(), at, 5);
        resumeAll(fz, nf);
        g_lrSmooth = (GetPrivateProfileIntA("Engine", "SmoothAnimation", 1, ini) > 0) ? 1 : 0;
        g_lrFirst = (GetPrivateProfileIntA("Engine", "LogicFirst", 1, ini) > 0) ? 1 : 0;
        { char env[8]; env[0] = 0; if (GetEnvironmentVariableA("AOTR_LOGICFIRST", env, sizeof(env)) && env[0]) g_lrFirst = env[0] != '0'; }
        if (g_lrFirst) {                                   // its waits sleep a millisecond at a time: the timer has to tick that fast
            HMODULE mm = GetModuleHandleA("winmm.dll"); if (!mm) mm = LoadLibraryA("winmm.dll");
            typedef UINT (WINAPI* tTBP)(UINT); tTBP tbp = mm ? (tTBP)GetProcAddress(mm, "timeBeginPeriod") : NULL; if (tbp) tbp(1);
        }
        g_lrHz = hz; g_lrLive = 1;
        logf("logicrate: %s", g_lrFirst ? "logic first - a frame without logic is started only when it will be done before the next logic frame is due; otherwise the thread waits for that logic frame (the simulation keeps its rate and frames come evenly between 30 and 60 a second)."
                                        : "logic first is off - a frame without logic is started whenever the next logic frame is not due yet, also when that makes it late.");
        logf("logicrate: animation between logic frames is %s.", g_lrSmooth
             ? "ON - the millisecond counter the pose evaluation reads is carried forward on render-only frames "
               "and the engine's own value is restored before each logic frame"
             : "off - poses repeat on render-only frames");
        logf("logicrate: the simulation is held at %d logic frames a second (%I64u ticks apart) while the render "
             "loop runs free. The engine's own render-only frame path does it, so the logic sequence, its order "
             "and its sub-frame numbering are exactly as they ship.", hz, g_lrTicks);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("logicrate: install faulted - logic stays tied to the frame rate.");
    }
}

// ---------------------------------------------------------------- offline test: the rule, over frame patterns
// A game thread that draws a frame in D ms and runs a logic update in G ms (after the Present, as the scheduler
// does), under the engine's 16 ms frame cap, driven by the old rule and by the new one for ten seconds: logic
// frames a second, frames a second, how late logic frames start, and how uneven the frame intervals are. Then the
// same with the drawing time wandering around the edge between the two cadences: the cadence must not flip.
static void lrSim(int rule, double D, double G, double wobble, double* out4, LONG* flips) {
    const double interval = 1000.0 / 30.0, cap = 16.0;
    LrState st = { 0, 0, 1, 0 };
    double t = 0, next = 0, lastStart = -1, limExit = -1, sumDev = 0, prevDt = 0, lateSum = 0; int nL = 0, nF = 0, nDt = 0, lastWasR = 0; bool have = false;
    DWORD seed = 4242;
    while (t < 10000.0) {
        if (limExit >= 0 && t < limExit + cap) t = limExit + cap;            // the engine's limiter: 16 ms from its own last exit (0x0063A196 .. 0x0063A1F8)
        limExit = t;
        bool logic; double due = next;
        if (!have) { next = t + interval; have = true; logic = true; due = t; }
        else {
            if (rule == 1 && t < next) {
                if (!lrRoom(st, (ULONG64)((next - t) * 1000.0), (ULONG64)(cap * 1000.0), (ULONG64)(interval * 1000.0), 1000, lastWasR, flips)) t = next;
                else t += (double)lrMidHold(st, (ULONG64)((next - t) * 1000.0), (ULONG64)(cap * 1000.0), (ULONG64)(interval * 1000.0)) / 1000.0;
            }
            if (t < next) logic = false;
            else { logic = true; next += interval; if (next <= t) next = t + interval; }
        }
        if (lastStart >= 0) { double dt = t - lastStart; if (nDt++) sumDev += (dt > prevDt ? dt - prevDt : prevDt - dt); prevDt = dt; }
        lastStart = t;
        seed = seed * 1664525u + 1013904223u;
        double d = D + wobble * (((double)(seed >> 8) / 16777216.0) * 2.0 - 1.0);
        lrLearn(st, (ULONG64)(d * 1000.0), (ULONG64)(G * 1000.0), logic ? 0 : 1, (ULONG64)(interval * 1000.0));
        if (logic) { nL++; lateSum += t - due; }
        nF++; lastWasR = logic ? 0 : 1;
        t += d + (logic ? G : 0.0);
    }
    out4[0] = nL / 10.0; out4[1] = nF / 10.0; out4[2] = nDt > 1 ? sumDev / (nDt - 1) : 0; out4[3] = nL ? lateSum / nL : 0;
}
extern "C" __declspec(dllexport) int __cdecl AotrLogicFirstTest(char* out, int nOut) {
    int fail = 0; char* o = out; int left = nOut;
    static const double kD[7] = { 26.0, 19.0, 15.0, 12.0, 10.0, 8.0, 4.0 }, kG[7] = { 14.0, 12.0, 10.0, 8.0, 6.0, 4.0, 3.0 };
    const double interval = 1000.0 / 30.0;
    for (int c = 0; c < 7; ++c) {
        double res[2][4]; LONG flips = 0;
        lrSim(0, kD[c], kG[c], 0.0, res[0], NULL); lrSim(1, kD[c], kG[c], 0.0, res[1], &flips);
        double maxLogic = 1000.0 / (kD[c] + kG[c]); if (maxLogic > 30.0) maxLogic = 30.0;
        // the new rule: the logic rate the machine can hold, no fewer logic frames than before, frames no less even, logic frames on time where 30 can be held
        if (res[1][0] < maxLogic - 0.25 || res[1][0] < res[0][0] - 0.05 || res[1][0] > 30.15 || res[1][2] > res[0][2] + 0.05) fail |= 1 << c;
        if (kD[c] + kG[c] < interval && res[1][3] > 0.2) fail |= 1 << c;
        if (kD[c] + kG[c] <= 16.0 && res[1][1] < 59.0) fail |= 1 << c;                    // two frames fit: both are drawn
        if (flips > 1) fail |= 1 << c;
        if (o && left > 220) { int k = sprintf(o, "%s[draw %.0f + logic %.0f ms: was %.1f logic / %.1f frames a second, logic %.1f ms late, step %.1f ms uneven; now %.1f / %.1f, %.1f late, %.1f]",
                                               c ? " " : "", kD[c], kG[c], res[0][0], res[0][1], res[0][3], res[0][2], res[1][0], res[1][1], res[1][3], res[1][2]); o += k; left -= k; }
    }
    // at the edge between the two cadences (a frame with logic of 17.3 ms), the drawing time wandering by +-1.5 ms: one cadence, held
    { double r[4]; LONG flips = 0; lrSim(1, 10.3, 7.0, 1.5, r, &flips);
      if (flips > 3 || r[3] > 0.6) fail |= 128;
      if (o && left > 160) { int k = sprintf(o, " [at the edge, drawing 10.3 +-1.5 ms + logic 7: %.1f logic / %.1f frames a second, %.1f ms late, the cadence changed %d times in ten seconds]", r[0], r[1], r[3], (int)flips); o += k; left -= k; } }
    if (o && left > 60) sprintf(o, " - %s (flags %02X)", fail ? "FAILED" : "logic on time at the rate the machine can hold in every case", fail);
    return fail;
}
