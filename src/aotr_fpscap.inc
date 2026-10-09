// ---------------------------------------------------------------- frame rate cap, raised in memory
// The engine caps the render loop with a wall-clock spin at 0x0063A18E:
//
//     cmp  byte [0xDE4320], 0        ; UseFPSLimit
//     je   skip
//     call timeGetTime               ; edi = now
//     fild dword [esi+0x0C]          ; FramesPerSecondLimit
//     fmul [0xD9F498] / fdivr [0xBD4388] / ftol    ; esi = milliseconds per frame
//     ...
//  spin:
//     push 0 / call Sleep            ; Sleep(0) until now - lastFrame >= esi
//
// Nothing in there touches the simulation: it only decides how long the loop waits. The sixteen bytes that turn
// the INI's FramesPerSecondLimit into a millisecond budget are replaced with a constant of our own, so the cap can
// be raised without editing gamedata.ini - no file in the game folder, and it applies to whatever mod is loaded.
//
//     DB 46 0C           fild dword [esi+0x0C]        ->   8B F8      mov edi, eax   (keep timeGetTime's result)
//     8B F8              mov edi, eax                 ->   BE ms      mov esi, ms
//     D8 0D 98 F4 D9 00  fmul  [0xD9F498]             ->   90 x17     nop
//     D8 3D 88 43 BD 00  fdivr [0xBD4388]
//     E8 F2 2D 40 00     call  ftol
//     8B F0              mov esi, eax
//
// The original pushes one x87 value and pops it again in ftol; the replacement touches the stack not at all, so it
// stays balanced either way.
//
// Whether the SIMULATION also speeds up is the open question, and this does not assume an answer: with the cap
// raised it logs render frames and logic steps per second once a second, which settles it in one battle. One logic
// step is six GameLogic::update(n) calls and the engine's own dispatcher decides how many of those run per rendered
// frame, so the engine is built to keep the two apart - but built to and does are different claims.

#define FPS_PATCH_AT   0x0063A19C
#define FPS_PATCH_LEN  24
#define FPS_USELIMIT   0x00DE4320        // the UseFPSLimit byte the loop tests first

static LONG  g_fpsWanted = 0;            // from the ini; 0 = leave the engine alone
static LONG  g_fpsMs = 0;                // what we wrote as the per-frame budget
static LONG  g_fpsLive = 0;

static const BYTE kFpsOrig[FPS_PATCH_LEN] = {
    0xDB, 0x46, 0x0C,                    // fild  dword ptr [esi+0x0C]
    0x8B, 0xF8,                          // mov   edi, eax
    0xD8, 0x0D, 0x98, 0xF4, 0xD9, 0x00,  // fmul  dword ptr [0x00D9F498]
    0xD8, 0x3D, 0x88, 0x43, 0xBD, 0x00,  // fdivr dword ptr [0x00BD4388]
    0xE8, 0xF2, 0x2D, 0x40, 0x00,        // call  0x00A3CFA4   (ftol)
    0x8B, 0xF0                           // mov   esi, eax
};

// steps and frames per second, so the first run with a raised cap says whether the simulation moved too
static DWORD WINAPI fpsReport(LPVOID) {
    LONG lastFrames = 0, lastSteps = 0, said = 0, lastLogic = 0, lastSkip = 0, lastNudge = 0, lastHeld = 0; LONG64 lastHeldT = 0;
    for (;;) {
        Sleep(1000);
        LONG frames = g_mkFrames, df = frames - lastFrames; lastFrames = frames;
        LONG steps = g_lsSteps + g_lsStockSteps, ds = steps - lastSteps; lastSteps = steps;
        if (!df && !ds) continue;                                   // menus and loading screens say nothing
        if (!said) { said = 1;                                      // read now, not at install: the game parses its
            logf("fpscap: UseFPSLimit is %s.", *(BYTE*)FPS_USELIMIT ? "on" : "off (the limiter is never reached)");
        }
        LONG lr = g_lrRan, dlr = lr - lastLogic; lastLogic = lr;
        LONG ls = g_lrSkipped, dls = ls - lastSkip; lastSkip = ls;
        LONG nu = g_lrNudged, dnu = nu - lastNudge; lastNudge = nu;
        if (g_lrLive) {
            LONG held = g_lrHeld, dHeld = held - lastHeld; lastHeld = held;
            LONG64 heldT = g_lrHeldTicks, dHeldT = heldT - lastHeldT; lastHeldT = heldT;
            int heldMs = (g_lrTicks && g_lrHz) ? (int)((ULONG64)dHeldT * 1000 / (g_lrTicks * (ULONG64)g_lrHz)) : 0;
            logf("fps: %d rendered frames, %d logic frames and %d logic steps in the last second (cap %d fps, "
                 "%d ms a frame; logic held at %d Hz, %d rendered only, %d of those with animation carried "
                 "forward; %d frames without logic not started because the next logic frame was due first - %d ms waited for those). "
                 "Logic frames must stay at %d however high the frame rate goes.", (int)df, (int)dlr,
                 (int)ds, (int)g_fpsWanted, (int)g_fpsMs, (int)g_lrHz, (int)dls, (int)dnu, (int)dHeld, heldMs, (int)g_lrHz);
        }
        else if (g_lsInstalled)
            logf("fps: %d rendered frames and %d logic steps in the last second (cap %d fps, %d ms a frame). "
                 "Logic steps must not rise with the frame rate.", (int)df, (int)ds, (int)g_fpsWanted, (int)g_fpsMs);
        else
            logf("fps: %d rendered frames in the last second (cap %d fps, %d ms a frame). Logic steps are not "
                 "counted - the slicer is not installed.", (int)df, (int)g_fpsWanted, (int)g_fpsMs);
    }
}

static void installFpsCap() {
    char ini[MAX_PATH];
    aotrPath(ini, "bfme2_accel.ini");
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;         // no ini: the engine keeps its own cap
    int want = (int)GetPrivateProfileIntA("Engine", "FpsLimit", 0, ini);
    if (want <= 0) return;
    if (want < 20 || want > 240) {
        logf("fpscap: [Engine] FpsLimit = %d is outside 20..240 - left alone.", want);
        return;
    }
    __try {
        if (memcmp((void*)FPS_PATCH_AT, kFpsOrig, FPS_PATCH_LEN) != 0) {
            logf("fpscap: the frame limiter does not look like the build this was written for - cap left alone.");
            return;
        }
        const LONG ms = 1000 / want;                                         // the engine truncates the same way

        BYTE patch[FPS_PATCH_LEN];
        memset(patch, 0x90, sizeof(patch));
        patch[0] = 0x8B; patch[1] = 0xF8;                                    // mov edi, eax
        patch[2] = 0xBE;                                                     // mov esi, imm32
        *(LONG*)(patch + 3) = ms;

        HANDLE susp[256]; int nf = 0;
        suspendOthers(susp, &nf, 256);
        DWORD op = 0;
        if (!VirtualProtect((void*)FPS_PATCH_AT, FPS_PATCH_LEN, PAGE_EXECUTE_READWRITE, &op)) {
            resumeAll(susp, nf);
            logf("fpscap: the limiter is not writable - cap left alone.");
            return;
        }
        memcpy((void*)FPS_PATCH_AT, patch, FPS_PATCH_LEN);
        VirtualProtect((void*)FPS_PATCH_AT, FPS_PATCH_LEN, op, &op);
        FlushInstructionCache(GetCurrentProcess(), (void*)FPS_PATCH_AT, FPS_PATCH_LEN);
        resumeAll(susp, nf);

        g_fpsWanted = want; g_fpsMs = ms; g_fpsLive = 1;
        g_lrCapMs = ms;                                                      // the logic limiter counts on it (aotr_logicrate.inc)
        logf("fpscap: frame cap raised to %d fps (%d ms a frame, %d threads frozen for the patch).", want, (int)ms, nf);
        logf("fpscap: counting rendered frames against logic steps once a second. The simulation is only safe if "
             "the logic step count does NOT rise with the frame rate.");
        CreateThread(NULL, 0, fpsReport, NULL, 0, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("fpscap: install faulted - cap left alone.");
    }
}
