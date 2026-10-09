// ================================================================ in-between frames (v50, 2026-10-06)
// A heavy battle is bound by the game thread: one frame of game work is ~38 ms and no amount of re-arranging gets a
// second copy of the engine's own code running beside it. The render thread, though, is busy only half the time, and
// the GPU is barely loaded. So the picture rate is taken off the game thread altogether: between every two frames
// the game produces, the render thread draws one more - the same draw stream, run a second time, with every moving
// thing (bones, object transforms, the camera) set halfway between the previous frame and this one - and shows it
// first. The game's own frame follows half a frame period later. Real geometry, drawn by the game's own shaders:
// nothing is warped or guessed from pixels.
//
// How a frame's draw stream is run twice:
//   * MARK. The game thread queues a mark where the frame's drawing into the back buffer begins (the engine's
//     Begin_Render call; everything before it - the shadow map, texture uploads - is done once and shared).
//     When the render thread reaches the mark it captures the whole device state into a state block, plus the
//     render targets, and from there on does not hand the ring space of the records it executes back to the
//     producer: the records stay where they are, pointers into their own payload and all.
//   * RELEASES WAIT. A Release or DeleteParameterBlock inside the marked stretch is put aside and executed after
//     the second pass, in order, so everything the stretch names is still alive when it runs again.
//   * REPLAY. At Present the device state is put back as it was at the mark and the records are executed again.
//     What must happen once is skipped: Present, the releases, the recording of a parameter block (it exists
//     already), mip-chain rebuilds, queries, our own build jobs - and every write into a TEXTURE or surface
//     (pixels, rows, UpdateSurface, UpdateTexture). A texture is written and then drawn with, so when the stretch
//     ends it holds what its draws saw; writing the queued bytes a second time would only be redundant - and
//     wrong where the game has since changed the same texture through a direct lock, which is not in the stream
//     (the first version of this did exactly that to the harness's radar texture). Vertex and index buffer writes
//     ARE repeated: a dynamic buffer is refilled several times a frame and each draw needs its own fill; a direct
//     write lock of a buffer cancels the stretch instead. An ApplyParameterBlock uses the handle its first
//     execution resolved (kept in the record), because a proxy index can be deleted and handed out again inside
//     one frame. The game thread forgets what the effects hold at the mark, so the first write of every effect
//     parameter in the stretch is a real record and the second pass is not missing a write that the value cache
//     dropped because an EARLIER stretch had already set it.
//   * THE BACK BUFFER STANDS IN. The game's frame is drawn into one of a few off-screen targets shaped like the
//     back buffer (every call that names the back buffer is redirected - the device's "original" methods are
//     wrapped, so queued and direct calls alike go through it). The second pass draws into another of them. Both
//     pictures then wait for their place in time - a steady beat, see aotr_pace.inc - and are copied to the real
//     back buffer and presented when it comes, while the render thread is already executing the next frame into
//     a target that is free. (Until v52 the second pass drew into the real back buffer and was presented the
//     moment it was finished, so every uneven frame showed as a jerk.)
//   * NOT EVERY FRAME. A call that runs directly on the game thread and changes device or effect state (Begin of a
//     technique seen for the first time, releasing an effect, a device reset...) is not in the stream, so that
//     frame's stretch is not run again: its releases are executed at once and the frame is simply shown in its
//     slot. If the producer runs out of ring space the stretch is given back the same way. The frame AFTER such a
//     frame still gets its in-between picture: what was kept of the frame before the missed one is two frames old
//     then, so the blend is taken three quarters of the way instead of half (v53).
//
// What is set halfway in the second pass, and how the two frames' values are matched:
//   * BONES. The engine's mesh draw (0x0054A1F0) is hooked and writes a tag - the mesh pointers - into the effect
//     parameter stream ahead of the draw's own parameters. The palette that follows (the shaders' WorldBones:
//     quaternion + position per joint, written with SetRawValue) is kept per (mesh, effect, handle); in the second
//     pass each joint is the normalised blend of last frame's quaternion and this one's, and the blend of the two
//     positions. A mesh that moved further than any unit can in one frame is left as it is (a pointer handed to a
//     new object, a teleport).
//   * RIGID MESHES. The same tag keys the World matrix of a mesh that is not skinned.
//   * THE CAMERA. The shaders take it as View / Projection / ViewI (matched by parameter semantic, per effect and
//     handle, against the nearest of last frame's values - a cut to another place is further away than any camera
//     move and is not blended) and as WorldViewProjection / WorldView / WorldViewI products for the terrain, trees,
//     water and particles. A product is not matched with anything: it is multiplied by the matrix that takes this
//     frame's camera to the halfway camera (built from the device's own VIEW and PROJECTION transforms, which are
//     blended the same way for everything drawn without a shader).
//   * CAMERA-SPACE GEOMETRY. Late in the 3D pass the engine draws things whose vertices it has already moved into
//     this frame's camera space (it sets the device's VIEW to the identity, keeps the camera's projection, draws,
//     and sets the camera back - once per object). Left alone they would stay where this frame's camera saw them
//     while everything around them follows the halfway camera. So an identity VIEW under a perspective projection
//     is replaced by V^-1 V' - "from this frame's camera space to the halfway camera's" - for as long as it lasts.
//   * FLAT THINGS PINNED TO THE WORLD. Health bars and unit icons are flat quads the engine has already placed on
//     screen for this frame's camera, drawn at the end of the 3D pass with identity VIEW and PROJECTION. They sit
//     close to the ground, so where they belong under the halfway camera is the plane-to-plane mapping of the
//     ground (at the height the frame's units stand at) between the two cameras; that 3x3 mapping, as a
//     projection matrix, replaces the identity while they are drawn. The game thread marks where the 3D pass ends
//     - the interface proper, drawn after it, keeps the identity.
//   * Everything else is this frame's: textures, the interface, particle vertices, the shadow map and the matrix
//     that looks things up in it. When the second pass is done every blended parameter is written back to the
//     stream's own last value, so the next frame starts from exactly what the game set.
//
// Proof. src/rt_harness.cpp ("tween", "tweenbad", "tweenlate"): every frame's stretch is run again with nothing
// changed and the picture in the back buffer must be the off-screen frame, byte for byte; the frames themselves
// must still hash to the references made without any of this; a control that leaves draws out must fail.
// src/tween_test.cpp: a scene built on the game's parameter layout (palette, World, View/Projection/ViewI, product
// matrices, fixed-function transforms) is run with blending on; each in-between picture must be, byte for byte, the
// picture the test draws itself from the halfway values, and each of the game's frames must be untouched.
// [Engine] InBetweenFrames (default 0); Ctrl+F10 switches it live. When it is on it is on for every frame the game
// draws, at any frame rate: nothing measures the render thread's load and backs off. If the render thread cannot fit
// the second pass into the game's frame, that shows - in the "tween:" and "budget:" lines of the log - and is the
// thing to fix. The one thing that switches it off by itself is a fault inside the second pass.

#define TW_KIND      10                      // record kind: frame mark / stretch cancelled
#define TW_OP        508
#define TW_DEFER_N   16384
#define TW_BEGIN_RENDER_AT 0x00449EA5        // W3DDisplay::draw: call WW3D::Begin_Render (0x00517B30)
#define TW_BEGIN_RENDER_FN 0x00517B30
#define TW_DRAW_FN         0x0054A1F0        // mesh draw (mesh, effect wrapper, count)
#define TW_KEYS      32768
#define TW_ARENA     (6u << 20)
#define TW_CAMS      512
#define TW_CLS_N     8192
#define TW_UNDO_N    2048
#define TW_NSURF     6                       // off-screen frames: one being drawn into, one for the in-between frame, three waiting for their place in time, and the one on screen while it may be shown again (aotr_panwarp.inc)
#define TW_QN        8

typedef DWORD (__stdcall* TwF1)(DWORD);
typedef DWORD (__stdcall* TwF2)(DWORD, DWORD);
typedef DWORD (__stdcall* TwF3)(DWORD, DWORD, DWORD);
typedef DWORD (__stdcall* TwF4)(DWORD, DWORD, DWORD, DWORD);
typedef DWORD (__stdcall* TwF5)(DWORD, DWORD, DWORD, DWORD, DWORD);
typedef DWORD (__stdcall* TwF6)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
typedef DWORD (__stdcall* TwF7)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
typedef DWORD (__stdcall* TwF9)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);

enum { TWC_NONE = 0, TWC_PALETTE, TWC_WORLD, TWC_VIEW, TWC_PROJ, TWC_VIEWI, TWC_WVP, TWC_WV, TWC_WVI };
struct TwDef { void* fn; DWORD a0, a1; DWORD two; };
struct TwPresentArgs { int viaSc; DWORD self, flags; BYTE haveSrc, haveDst; LONG src[4], dst[4]; DWORD wnd; };
struct TwPic { DWORD surf, seq; LONG ib, pan; LONG64 due, beat, made; DWORD gt; TwPresentArgs args;
               LONG camOk; DWORD ct, mt, scn; float V[16], P[16], gh; };   // (v59) the camera it was drawn from, when its frame's scroll step was taken and when the frame was marked, its scene before the interface   // a finished picture waiting for its place in time (gt: when the game thread handed its frame over)
struct TwKey { DWORD mesh, fx, h, off; WORD occ, nb; DWORD stamp; };
struct TwCam { DWORD fx, h; float v[16]; };              // fx == 0: a device transform, h is its type
struct TwGen { TwKey* key; BYTE* arena; TwCam* cam; DWORD used, stamp; LONG nCam, valid, over; };
struct TwCls { DWORD fx, h; LONG gen; DWORD cls; };
struct TwUndo { DWORD fx, h; const DWORD* e; DWORD sub; DWORD stamp; };

// Everything below belongs to whoever is executing device work: the render thread, or the game thread while it
// holds the exec lock for a direct call.
__declspec(align(64)) struct TwW {
    DWORD dev;                               // device seen by the wrapped methods
    DWORD resDev;                            // device the objects below were made for
    DWORD bb;                                // its back buffer (not referenced: it lives until the device is reset)
    DWORD off[TW_NSURF]; LONG nOff;          // render targets shaped like it
    DWORD sb;                                // state block, D3DSBT_ALL
    DWORD chkRT, chkSys[2];                  // self-check: resolve target and two system-memory surfaces
    DWORD w, h, fmt, ms, msq;
    LONG  resFail;
    DWORD standIn;                           // what "the back buffer" means in the calls being executed (0: itself)
    LONG  inScene;
    // the stretch of the frame being executed
    LONG  cancel;                            // (open: g_twStretch)
    RtRec* markRec;
    DWORD markRT[4]; BYTE markRTOk[4]; DWORD markDS; BYTE markDSOk;
    LONG  markInScene;
    float markV[16], markP[16]; LONG markVOk, markPOk;
    LONG  replaying; DWORD keepStandIn;
    TwDef* defer; LONG nDefer;
    // finished pictures waiting for their place in time, in the order they go on screen (an in-between frame, then the game's frame it leads to)
    TwPic q[TW_QN]; LONG qn;
    DWORD ibSurf;                            // what the second pass draws into
    DWORD seqIn, seqOut;                     // proof of order: pictures queued so far / the last one shown
    DWORD lastHr;
    // pacing (aotr_pace.inc)
    TwPace pace;
    LONG64 lastShow, replayCost;             // when the last picture went on screen
    LONG  prevAge;                           // how many frames back gen[1 - cur] is (1: the frame before this one)
    LONG  wantOff;                           // draw the next frame off screen
    LONG  active;                            // in-between frames are being made (it is switched on and can run)
    LONG  walkN, lastTags;                   // battle-sized stretches that could be run again so far; meshes tagged in the last stretch read
    // two frames' worth of what moves
    TwGen gen[2]; int cur;                   // gen[cur] is being filled, gen[1 - cur] is the frame before
    TwCls* cls; TwUndo* undo; DWORD undoStamp;
};
static TwW g_tw;
// pan pictures made on the GPU (aotr_panwarp.inc, included further down)
struct PwCam { LONG ok; DWORD ct, mt, scn; float V[16], P[16], gh; };
static PwCam g_pwPend[2];                    // what the two pictures of the frame at hand show the world from (0: the game's frame, 1: the in-between one)
static volatile LONG g_pwGridOn = 1, g_pwNGrid = 0, g_pwNGridNew = 0, g_pwGridGapMax = 0;   // pictures on the screen's beat while the camera scrolls: the switch ([Engine] PanRefreshBeat), re-shown pictures, game pictures, longest gap (us)
static volatile LONG g_pwNPassed = 0, g_pwNSlotClock = 0, g_pwNSlotOwn = 0, g_pwNTail = 0, g_pwNHeld = 0;   // (v72) pictures a newer one went up instead of; refreshes served by the screen's own clock / by a beat of our own; refreshes of a scroll's end played out; pictures kept back until then
static volatile LONG g_pwEvenN = 0, g_pwEvenOff5 = 0, g_pwEvenOff20 = 0, g_pwEvenWorstPm = 0;      // (aotr_panwarp.inc) how even the scroll on screen is
static volatile LONG g_pwHandN = 0, g_pwHandMaxUs = 0, g_pwHandOver1 = 0; static volatile LONG64 g_pwHandSumUs = 0;   // on the screen's beat: pictures handed over, how long after the planned moment (us), how many more than a millisecond after
static LONG g_pwLeadUs = 0;                  // [Engine] PanLeadMs: how long before a refresh a picture is handed over (0: 2 ms)
static LONG twUsFwd(LONG64 ticks);                    // (twUs, defined with the pictures' statistics below)
static volatile LONG g_pwOn = 0, g_pwTest = 0;        // the switch; test bits: 1 every picture goes through the rectangle with the camera it has and is compared, 2 the camera given by the test
static volatile DWORD g_panCt = 0;           // game thread: when this frame's scroll step was taken (QPC, low 32 bits, never 0); 0: the camera was not scrolled
static volatile LONG g_panWarpActive = 0;    // pictures can be shown from where the camera is at that moment: the scroll hook steps by the clock
static DWORD g_pwLastSurf = 0;               // the picture on screen, kept while it may be shown again from further along the pan
struct PwPath { DWORD t; float V[16], P[16]; LONG moving; };       // a frame's camera and the moment (QPC, low 32 bits) it stands for
#define PW_PATH 8
struct PwW {
    DWORD dev, texA, surfA, scnTex[TW_NSURF], scn[TW_NSURF], ps, sb;
    BYTE scnValid[TW_NSURF];
    LONG ready, fail;
    PwPath path[PW_PATH]; LONG pathN;
    DWORD frameCt, frameMt;                  // the frame being executed: when its scroll step was taken (0: none), when it was marked
    TwPic last; LONG lastOk;                 // the picture on screen
    LONG lagMark, lagOdd;                    // from a frame's mark to its own picture going up, usual (ticks)
    LONG64 refresh;                          // the screen's refresh interval (ticks)
    LONG64 slotVb;                           // (v72) the refresh the last picture on the screen's beat was handed over for
    LONG lead;                               // ... how long before a refresh a picture is handed over (ticks)
    LONG lagGrid;                            // ... how far behind a refresh the camera's moment is while pictures go up on the screen's beat (ticks)
    LONG ctOff;                              // from a frame's scroll step to its mark, usual (ticks)
    LONG shifted;                            // the picture on screen went up from another place than it was drawn from
    LONG walkLag, walkOdd;                   // from a frame's scroll step to the render thread having read its camera, usual (ticks)
    DWORD endT, tauLast; LONG endSet;        // the moment of the newest frame the camera was scrolled in; the moment of the scroll the screen last showed
};
static PwW g_pw;
// How long before a refresh a picture is handed over: 2 ms (or [Engine] PanLeadMs) when DXVK puts it on screen the moment the GPU has it (it then lands at the
// refresh, give or take); 4 ms when DXVK shows it at the refresh (mailbox, vsync): the GPU has to be done with it before the refresh, not at it.
static __forceinline LONG pwLead() { return (g_pwLeadUs || g_dxvkMode < 2) ? g_pw.lead : g_pw.lead * 2; }
static LONG64 g_pwHandAt = 0;                // the moment the refresh being served asked for its picture (pwSlot)
static __forceinline void pwHandNote() {               // right before the Present of a picture that goes up on the screen's beat
    LONG64 late = qpcNow() - g_pwHandAt; if (late < 0 || !g_pwHandAt) late = 0;
    LONG us = (LONG)(late * 1000000 / (g_pqpf.QuadPart ? g_pqpf.QuadPart : 1));
    g_pwHandN++; g_pwHandSumUs += us; if (us > g_pwHandMaxUs) g_pwHandMaxUs = us; if (us > 1000) g_pwHandOver1++;
}
// While pictures go up on the screen's beat: how long since the one before - within a quarter of a refresh of one
// refresh, up to 1.75 refreshes (late), up to 2.5 (a refresh without a picture), longer.
static volatile LONG g_pwGridGapN[4];
static void pwGridGapNote(LONG64 gap) {
    LONG us = twUsFwd(gap);
    if (us >= 500000 || gap <= 0 || !g_pw.refresh) return;      // (not across a pause)
    if (us > g_pwGridGapMax) g_pwGridGapMax = us;
    LONG64 q4 = gap * 4 / g_pw.refresh;
    g_pwGridGapN[q4 < 5 ? 0 : q4 < 7 ? 1 : q4 < 10 ? 2 : 3]++;
}
// The camera has stopped, but the screen has not shown the end of its way yet (on the screen's beat the camera's moment runs a
// steady time behind the refresh - pwGridTau): the scroll is played out to its end, for a quarter of a second at most.
static __forceinline LONG pwTail() {
    const PwW& q = g_pw;
    return (g_pwGridOn && q.endSet && q.lastOk && (LONG)(q.endT - q.tauLast) > 0 && (LONG)((DWORD)qpcNow() - q.endT) < (LONG)(g_pqpf.QuadPart / 4)) ? 1 : 0;
}
// a scroll is on (or being played out)
static __forceinline LONG pwScrolling() { return (g_pwOn && g_pw.pathN > 0 && (g_pw.path[g_pw.pathN - 1].moving || pwTail())) ? 1 : 0; }
// the idle render thread has something to look out for: the camera is scrolling and the picture on screen can be shown again further along
static __forceinline LONG pwBusy() { return (g_pw.lastOk && pwScrolling()) ? 1 : 0; }
static bool pwTryShow(const TwPic& p, LONG64 now, bool grid);
static void pwSlot(LONG64 now, LONG64* at, LONG64* vb);
static void pwShown(const TwPic& p, LONG64 t);
static void pwFillerTick();
static LONG64 pwGrid();                      // the screen's refresh interval while pictures go up on that beat (a scroll is on); 0 otherwise
static void pwSceneCopy(DWORD surf);
static void pwFree();
static bool pwEnsure();
static void pwTestCompare(const TwPic& p);
// across threads
static volatile LONG g_twOn = 0;             // the feature (ini, Ctrl+F10, test)
static volatile LONG g_twTest = 0;           // test bits: 1 second pass unchanged + compare, 2 leave draws out, 4 always off screen, 8 every other frame shown late, 16 blend + keep the picture, 32 as in a slow game (blend, real pacing), 64 a waiting picture goes up in the middle of the next second pass
static volatile LONG g_twEver = 0;
static volatile LONG g_twWantMark = 0;       // render thread: frames are slow enough (or a test wants them marked)
// (g_twHold / g_twHoldTail / g_twGiveUp / g_twBusy / g_twStretch / g_twPresentRec / g_twOpClass / g_twFxGen are defined in aotr_rt.inc, which uses them)
static LONG g_twCheckLeft = 0;               // in game: this many more stretches are run again unchanged and compared (a diagnostic)
static LONG g_twDumpLeft = 0, g_twDumpAfter = 0, g_twDumpN = 0;   // diagnostic: write this many in-between pictures (and their game frames) to files, after so many
static float g_twT = 0.5f;
static volatile LONG g_twBeatOn = 1;         // pictures on a steady beat (aotr_pace.inc); 0: each as soon as it is finished, the game's frame half a period later
static float g_twMaxMove = 60.0f, g_twMaxCamMove = 400.0f;
static volatile LONG g_twNPan = 0, g_twPanSum16 = 0;       // in-between frames drawn while the camera moved, and by how much (1/16 units, summed)
static LONG g_twDumpPan = 0, g_twDumpGap = 0;            // picture dumps wait for a moving camera; frames to leave between two of them
// the translation part of a view matrix differs by this much (either layout: one of the two triples is the translation, the other is zero in both)
static float twViewShift(const float* a, const float* b) {
    float d1 = (a[12] - b[12]) * (a[12] - b[12]) + (a[13] - b[13]) * (a[13] - b[13]) + (a[14] - b[14]) * (a[14] - b[14]);
    float d2 = (a[3] - b[3]) * (a[3] - b[3]) + (a[7] - b[7]) * (a[7] - b[7]) + (a[11] - b[11]) * (a[11] - b[11]);
    return (float)sqrt((double)(d1 > d2 ? d1 : d2));
}
static LONG g_twGroundOk = 0;                               // the last stretch walked had the end-of-3D mark
static float g_twGroundZ = 0.0f, g_twBarLift = 15.0f;       // where units stood in the last frame that had any; how far above their feet their bars float
// game thread
static struct { LONG open, cancel; } g_twGt;
static BYTE g_twSyncOk[512];                 // a direct call of this kind leaves the stretch repeatable
// counters
static volatile LONG g_twNMark = 0, g_twNReplay = 0, g_twNSame = 0, g_twNDiff = 0, g_twNCancel = 0, g_twNNoVerdict = 0, g_twNGiveUp = 0, g_twNDeferMax = 0,
                     g_twNDeferOver = 0, g_twNLate = 0, g_twNOnTime = 0, g_twNShown = 0, g_twNSlotOnly = 0, g_twNCheckFail = 0, g_twNDirectPresent = 0,
                     g_twNLost = 0, g_twNTween = 0, g_twNCollect = 0, g_twNNoPrev = 0;
static volatile LONG g_twNTag = 0, g_twNPal = 0, g_twNPalNew = 0, g_twNPalFar = 0, g_twNWorld = 0, g_twNWorldNew = 0, g_twNCam = 0, g_twNCamCut = 0, g_twNFix = 0, g_twNKeyOver = 0, g_twNDevCam = 0, g_twNCamSpace = 0, g_twNGround = 0;
static volatile LONG g_twCancelOp[8];        // the first few direct calls that cancelled a stretch (op + 1)
static volatile LONG64 g_twReplayTicks = 0;
static volatile LONG g_twFaults = 0;
static DWORD g_twFirstDiff[4];               // second pass, x, y, pixels different
static BYTE* g_twGrabBuf = NULL; static volatile LONG g_twGrabSeq = 0; static DWORD g_twGrabBytes = 0;
static char g_twNames[16][40]; static volatile LONG g_twNNames = 0;      // parameter names classified so far (log)

static void dvMarkBegin();
static void dvMarkEnd();
static void* g_twO_SetRT = NULL, *g_twO_GetRT = NULL, *g_twO_Stretch = NULL, *g_twO_Fill = NULL, *g_twO_GetRTData = NULL, *g_twO_UpdSurf = NULL,
           * g_twO_BeginScene = NULL, *g_twO_EndScene = NULL, *g_twO_Present = NULL, *g_twO_ScPresent = NULL;

static __forceinline DWORD twSurfCall1(DWORD surf, int slot) { void* f = rtSurfOrig(surf, slot); return f ? ((TwF1)f)(surf) : 0x80004005u; }
static __forceinline void twRel(DWORD* p) { if (*p) { twSurfCall1(*p, 2); *p = 0; } }

// ---------------------------------------------------------------- objects
static __forceinline bool twIsOff(DWORD s) { const TwW& w = g_tw; for (LONG i = 0; i < w.nOff; ++i) if (w.off[i] == s) return true; return false; }
static void twFreeObjects() {                             // exec lock held
    TwW& w = g_tw;
    pwFree();
    for (int i = 0; i < TW_NSURF; ++i) twRel(&w.off[i]);
    w.nOff = 0; twRel(&w.chkRT); twRel(&w.chkSys[0]); twRel(&w.chkSys[1]);
    if (w.sb) { ((TwF1)g_rtO_sb[2])(w.sb); w.sb = 0; }
    w.bb = 0; w.resDev = 0;
}
static bool twEnsure(DWORD dev) {                         // exec lock held
    TwW& w = g_tw;
    if (!dev || !g_twO_SetRT) return false;
    if (w.resDev == dev) return true;
    if (w.resFail) return false;
    if (w.resDev) twFreeObjects();
    void* bb = NULL;
    if ((LONG)((TwF5)g_rtO_dev[18])(dev, 0, 0, 0, (DWORD)(ULONG_PTR)&bb) < 0 || !bb) { w.resFail = 1; logf("tween: the device gave no back buffer - in-between frames stay off."); return false; }
    BYTE desc[32]; memset(desc, 0, sizeof(desc));
    void* gd = rtSurfOrig((DWORD)(ULONG_PTR)bb, 12);
    LONG hr = gd ? (LONG)((TwF2)gd)((DWORD)(ULONG_PTR)bb, (DWORD)(ULONG_PTR)desc) : -1;
    twSurfCall1((DWORD)(ULONG_PTR)bb, 2);                 // the pointer is all that is kept
    const DWORD* d = (const DWORD*)desc;                  // Format Type Usage Pool MultiSampleType MultiSampleQuality Width Height
    if (hr < 0 || !d[6] || !d[7]) { w.resFail = 1; logf("tween: the back buffer would not describe itself - in-between frames stay off."); return false; }
    w.fmt = d[0]; w.ms = d[4]; w.msq = d[5]; w.w = d[6]; w.h = d[7];
    typedef DWORD (__stdcall* tCRT)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
    w.nOff = 0;
    for (int i = 0; i < TW_NSURF; ++i) {
        void* s = NULL;
        if ((LONG)((tCRT)g_rtO_dev[28])(dev, w.w, w.h, w.fmt, w.ms, w.msq, 0, (DWORD)(ULONG_PTR)&s, 0) < 0 || !s) break;
        w.off[w.nOff++] = (DWORD)(ULONG_PTR)s;
    }
    if (w.nOff < 3) {                                      // one to draw into, one for the in-between frame, one waiting: with fewer nothing can be shown in order
        logf("tween: no render targets of the back buffer's shape (%ux%u format %u, multisample %u) - in-between frames stay off.", w.w, w.h, w.fmt, w.ms);
        for (int i = 0; i < TW_NSURF; ++i) twRel(&w.off[i]);
        w.nOff = 0; w.resFail = 1; return false;
    }
    void* sb = NULL;
    if ((LONG)((TwF3)g_rtO_dev[59])(dev, 1, (DWORD)(ULONG_PTR)&sb) < 0 || !sb) { for (int i = 0; i < TW_NSURF; ++i) twRel(&w.off[i]); w.nOff = 0; w.resFail = 1; logf("tween: no state block - in-between frames stay off."); return false; }
    w.sb = (DWORD)(ULONG_PTR)sb; w.bb = (DWORD)(ULONG_PTR)bb; w.resDev = dev;
    logf("tween: back buffer %ux%u format %u%s - %d off-screen frames and a state block ready.", w.w, w.h, w.fmt, w.ms ? ", multisampled" : "", (int)w.nOff);
    if (g_pwOn) pwEnsure();
    return true;
}

// ---------------------------------------------------------------- the back buffer's stand-in (wrapped device originals)
static __forceinline DWORD twSub(DWORD surf) { TwW& w = g_tw; return (surf && surf == w.bb && w.standIn) ? w.standIn : surf; }
static DWORD __stdcall tw_SetRenderTarget(DWORD self, DWORD idx, DWORD surf) {
    g_tw.dev = self;
    return ((TwF3)g_twO_SetRT)(self, idx, idx == 0 ? twSub(surf) : surf);
}
static DWORD __stdcall tw_GetRenderTarget(DWORD self, DWORD idx, DWORD pp) {
    DWORD hr = ((TwF3)g_twO_GetRT)(self, idx, pp);
    TwW& w = g_tw;
    if ((LONG)hr >= 0 && pp && w.bb) {
        DWORD s = *(DWORD*)(ULONG_PTR)pp;
        if (s && twIsOff(s)) {                            // the caller asked for "the render target": that is the back buffer
            twSurfCall1(s, 2); twSurfCall1(w.bb, 1);
            *(DWORD*)(ULONG_PTR)pp = w.bb;
        }
    }
    return hr;
}
static DWORD __stdcall tw_StretchRect(DWORD self, DWORD src, DWORD sr, DWORD dst, DWORD dr, DWORD filter) { return ((TwF6)g_twO_Stretch)(self, twSub(src), sr, twSub(dst), dr, filter); }
static DWORD __stdcall tw_ColorFill(DWORD self, DWORD surf, DWORD rc, DWORD color) { return ((TwF4)g_twO_Fill)(self, twSub(surf), rc, color); }
static DWORD __stdcall tw_GetRTData(DWORD self, DWORD rt, DWORD dst) { return ((TwF3)g_twO_GetRTData)(self, twSub(rt), dst); }
static DWORD __stdcall tw_UpdateSurface(DWORD self, DWORD src, DWORD sr, DWORD dst, DWORD dp) { return ((TwF5)g_twO_UpdSurf)(self, src, sr, twSub(dst), dp); }
static DWORD __stdcall tw_BeginScene(DWORD self) { DWORD hr = ((TwF1)g_twO_BeginScene)(self); if ((LONG)hr >= 0) g_tw.inScene = 1; return hr; }
static DWORD __stdcall tw_EndScene(DWORD self) { DWORD hr = ((TwF1)g_twO_EndScene)(self); if ((LONG)hr >= 0) g_tw.inScene = 0; return hr; }
static DWORD __stdcall tw_Present(DWORD self, DWORD a0, DWORD a1, DWORD a2, DWORD a3);
static DWORD __stdcall tw_ScPresent(DWORD self, DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4);
static void twWrap() {                                    // after the device and swap chain tables are hooked
    struct { void** slot; void** keep; void* shim; } k[] = {
        { &g_rtO_dev[37], &g_twO_SetRT, (void*)&tw_SetRenderTarget }, { &g_rtO_dev[38], &g_twO_GetRT, (void*)&tw_GetRenderTarget },
        { &g_rtO_dev[34], &g_twO_Stretch, (void*)&tw_StretchRect },  { &g_rtO_dev[35], &g_twO_Fill, (void*)&tw_ColorFill },
        { &g_rtO_dev[32], &g_twO_GetRTData, (void*)&tw_GetRTData },  { &g_rtO_dev[30], &g_twO_UpdSurf, (void*)&tw_UpdateSurface },
        { &g_rtO_dev[41], &g_twO_BeginScene, (void*)&tw_BeginScene }, { &g_rtO_dev[42], &g_twO_EndScene, (void*)&tw_EndScene },
        { &g_rtO_dev[17], &g_twO_Present, (void*)&tw_Present },      { &g_rtO_sc[3], &g_twO_ScPresent, (void*)&tw_ScPresent },
    };
    for (int i = 0; i < (int)(sizeof(k) / sizeof(k[0])); ++i)
        if (*k[i].slot && *k[i].slot != k[i].shim) { *k[i].keep = *k[i].slot; *k[i].slot = k[i].shim; }
}

// ---------------------------------------------------------------- what a direct call or a queued record means for a stretch
static void twTablesInit() {
    for (int op = 0; op < 512; ++op) {
        const char* n = rtOpName(op);
        const char* dot = n ? strchr(n, '.') : NULL;
        const char* m = dot ? dot + 1 : "";
        bool ok = !strncmp(m, "Get", 3) || strstr(m, "Lock") != NULL || !strncmp(m, "Is", 2) || !strncmp(m, "QueryInterface", 14) || !strncmp(m, "Create", 6) ||
                  !strncmp(m, "SetCursor", 9) || !strncmp(m, "ShowCursor", 10) || !strncmp(m, "TestCooperativeLevel", 20) || !strncmp(m, "ValidateDevice", 14) ||
                  !strncmp(m, "ValidateTechnique", 17) || !strncmp(m, "FindNextValidTechnique", 22) || strstr(m, "PrivateData") != NULL ||
                  !strncmp(m, "SetPriority", 11) || !strncmp(m, "SetLOD", 6) || !strncmp(m, "SetAutoGenFilterType", 20) || !strncmp(m, "AddRef", 6);
        if (op == 59) ok = true;                           // CreateStateBlock: an object, nothing recorded
        if (op == 60 || op == 61) ok = false;              // Begin/EndStateBlock change what the device does with every call
        g_twSyncOk[op] = ok ? 1 : 0;
        if (!strcmp(m, "Release")) g_twOpClass[op] = 1;
        if (!strcmp(m, "PreLoad") || !strcmp(m, "GenerateMipSubLevels") || !strcmp(m, "AddDirtyRect") || !strcmp(m, "Issue") || !strcmp(m, "EvictManagedResources") ||
            !strcmp(m, "SetGammaRamp") || !strcmp(m, "UnlockRect") || !strcmp(m, "Unlock") || !strcmp(m, "UnlockBox")) g_twOpClass[op] = 2;
    }
    g_twSyncOk[511] = 1;                                   // internal waits (ring space, buffer mirrors)
    g_twOpClass[196] = 3;                                  // fx.DeleteParameterBlock with a real handle: waits like a release
    g_twOpClass[509] = 2;                                  // queued D3DXFilterTexture
    g_twOpClass[17] = g_twOpClass[393] = 2;                // Present
    g_twOpClass[30] = g_twOpClass[31] = 2;                 // UpdateSurface / UpdateTexture: texture contents
}
static void twQueueCtl(DWORD what, DWORD dev, DWORD x = 0, DWORD y = 0) {           // game thread
    RtRec* r = rtAlloc(4, 0); r->kind = TW_KIND; r->op = TW_OP;
    DWORD* A = rtArgs(r); A[0] = what; A[1] = dev; A[2] = x; A[3] = y;
    rtCommit(r, 0);
}
// Game thread: the frame's drawing into the back buffer starts here.
static bool twStretchOpenGt() { return g_twGt.open && !g_twGt.cancel; }
static void twMark(DWORD dev) {
    g_twGt.open = 0; g_twGt.cancel = 0; g_twGtSerial++;
    DWORD ct = (DWORD)InterlockedExchange((volatile LONG*)&g_panCt, 0);       // this frame's scroll step, if the camera was scrolled (aotr_scroll.inc)
    if (!g_twOn || !g_twWantMark || !g_rtActive || g_rtDirectScope || g_rtObjRefs || __readfsdword(0x24) != g_rtMainTid) return;
    rtFxInvalAll(3);                                       // every effect parameter the stretch relies on is written inside it
    twQueueCtl(0, dev, ct, (DWORD)qpcNow() | 1);
    g_twGt.open = 1;
    InterlockedExchange(&g_twEver, 1);
}
// Game thread: a call is about to run directly instead of through the queue.
static void twOnSync(int op) {
    if (!g_twGt.open || g_twGt.cancel || g_twSyncOk[op & 511]) return;
    g_twGt.cancel = op + 1;
    for (int i = 0; i < 8; ++i) if (!g_twCancelOp[i] || g_twCancelOp[i] == op + 1) { g_twCancelOp[i] = op + 1; break; }
    if (g_rtActive && !g_rtDirectScope && __readfsdword(0x24) == g_rtMainTid) twQueueCtl(1, 0);   // its releases must not wait behind this call
}
static void twOnDirectWrite(int op) {                     // game thread: a buffer is about to be written outside the queue
    BYTE keep = g_twSyncOk[op & 511]; g_twSyncOk[op & 511] = 0;
    twOnSync(op);
    g_twSyncOk[op & 511] = keep;
}
static void twViewsEnd() {                                // game thread: the views are drawn, what follows is the interface
    if (g_twGt.open && g_rtActive && !g_rtDirectScope && __readfsdword(0x24) == g_rtMainTid) twQueueCtl(2, 0);
}
// Game thread, queueing Present: may this frame's stretch be run again?
static BYTE twFrameEnd() {
    BYTE f = (g_twGt.open && !g_twGt.cancel) ? 2 : 0;
    g_twGt.open = 0; g_twGt.cancel = 0;
    return f;
}
// Game thread, from the engine's mesh draw: the parameters that follow belong to these meshes.
static void __stdcall twTagDraw(DWORD mesh, DWORD count) {
    if (!g_twGt.open || !g_rtFxBatchOn || !g_rtActive || g_rtDirectScope || __readfsdword(0x24) != g_rtMainTid) return;
    DWORD k = 1; const DWORD* list = &mesh;
    if (count > 1) {                                       // a group: ours (aotr_merge.inc) names its meshes, the engine's own prop instancing is not blended
        if (g_mgCtx.live && g_mgCtx.n == count && g_mgCtx.mesh[0] == mesh && count <= MG_MAXK) { k = count; list = g_mgCtx.mesh; }
        else k = 0;
    }
    DWORD* e = rtFxRoom(4 + 4 * k);
    e[0] = 0xF0 | (k << 8);
    for (DWORD i = 0; i < k; ++i) e[1 + i] = list[i];
    g_rtFxPos += 4 + 4 * k; g_rtFxN++;
}

// ---------------------------------------------------------------- releases that wait
static void twRunDeferred() {
    TwW& w = g_tw;
    if (w.nDefer > g_twNDeferMax) g_twNDeferMax = w.nDefer;
    for (LONG i = 0; i < w.nDefer; ++i) {
        const TwDef& d = w.defer[i];
        if (d.two) ((TwF2)d.fn)(d.a0, d.a1); else ((TwF1)d.fn)(d.a0);
    }
    w.nDefer = 0;
}
static bool twDefer(void* fn, DWORD a0, DWORD a1, DWORD two) {
    TwW& w = g_tw;
    if (!w.defer || w.nDefer >= TW_DEFER_N) { w.cancel = 1; g_twNDeferOver++; return false; }   // no room: it runs now, and the stretch is not repeated
    TwDef& d = w.defer[w.nDefer++]; d.fn = fn; d.a0 = a0; d.a1 = a1; d.two = two;
    return true;
}
static void twClose() {                                   // the stretch is over (or given up): nothing waits any more
    TwW& w = g_tw;
    twRunDeferred();
    for (int i = 0; i < 4; ++i) { if (w.markRTOk[i]) twRel(&w.markRT[i]); w.markRT[i] = 0; w.markRTOk[i] = 0; }
    if (w.markDSOk) twRel(&w.markDS);
    w.markDS = 0; w.markDSOk = 0;
    g_twStretch = 0; w.cancel = 0; w.markRec = NULL;
    dvMarkEnd();
    InterlockedExchange(&g_twHold, 0);
}
static void twExecCtl(RtRec* r) {                         // render thread: a mark, or "this stretch will not be repeated"
    TwW& w = g_tw;
    DWORD what = rtArgs(r)[0], dev = rtArgs(r)[1];
    if (what == 2) { pwSceneCopy(w.standIn); return; }     // end of the 3D pass: the second pass reads it; the scene as it is now is kept aside for a scrolling camera
    if (what == 1) { if (g_twStretch) { g_twNCancel++; twClose(); } return; }
    if (g_twStretch) twClose();
    if (!dev) dev = w.dev;
    g_twNMark++;
    g_pw.frameCt = 0; g_pw.frameMt = 0;
    if (!twEnsure(dev)) return;
    w.dev = dev;
    if (r->nargs >= 4) { g_pw.frameCt = rtArgs(r)[2]; g_pw.frameMt = rtArgs(r)[3]; }
    for (int i = 0; i < 4; ++i) {
        void* s = NULL;
        DWORD hr = ((TwF3)g_twO_GetRT)(dev, (DWORD)i, (DWORD)(ULONG_PTR)&s);
        w.markRTOk[i] = ((LONG)hr >= 0 || hr == RT_D3DERR_NOTFOUND) ? 1 : 0;
        w.markRT[i] = (LONG)hr >= 0 ? (DWORD)(ULONG_PTR)s : 0;
    }
    { void* s = NULL;
      DWORD hr = ((TwF2)g_rtO_dev[40])(dev, (DWORD)(ULONG_PTR)&s);
      w.markDSOk = ((LONG)hr >= 0 || hr == RT_D3DERR_NOTFOUND) ? 1 : 0;
      w.markDS = (LONG)hr >= 0 ? (DWORD)(ULONG_PTR)s : 0; }
    ((TwF1)g_rtO_sb[4])(w.sb);                             // Capture
    w.markVOk = (LONG)((TwF3)g_rtO_dev[45])(dev, 2, (DWORD)(ULONG_PTR)w.markV) >= 0;      // the camera the stretch starts with
    w.markPOk = (LONG)((TwF3)g_rtO_dev[45])(dev, 3, (DWORD)(ULONG_PTR)w.markP) >= 0;
    w.markInScene = w.inScene;
    w.markRec = r; w.cancel = 0; g_twStretch = 1;
    g_twHoldTail = (DWORD)((BYTE*)r - g_rtBuf);
    InterlockedExchange(&g_twHold, 1);
    dvMarkBegin();                                         // from here the device calls inside effect calls are kept (aotr_devpass.inc)
}
// Render thread, first execution of a record inside an open stretch. true: put aside, do not execute now.
static bool twHoldBack(RtRec* r) {
    DWORD* a = rtArgs(r);
    BYTE c = g_twOpClass[r->op & 511];
    if (c == 1 && r->nargs == 1) return twDefer(r->fn, a[0], 0, 0);
    if (c == 3 && r->nargs == 2) return twDefer(r->fn, a[0], a[1], 1);
    return false;
}

// ---------------------------------------------------------------- showing frames
// Every picture - an in-between frame or one of the game's - is drawn into an off-screen target and waits there for
// its place in time; when that comes it is copied to the back buffer and presented. The pictures wait in a queue,
// in the order they go on screen, and that order is never left. The targets are a small pool: one is being drawn
// into, the others hold what waits; when none is free the oldest waiting picture is shown early.
static DWORD twRealPresent(const TwPresentArgs& p) {
    DWORD src = p.haveSrc ? (DWORD)(ULONG_PTR)p.src : 0, dst = p.haveDst ? (DWORD)(ULONG_PTR)p.dst : 0;
    return p.viaSc ? ((TwF6)g_twO_ScPresent)(p.self, src, dst, p.wnd, 0, p.flags) : ((TwF5)g_twO_Present)(p.self, src, dst, p.wnd, 0);
}
static void twCopyToBackBuffer(DWORD surf) {
    TwW& w = g_tw;
    ((TwF6)g_twO_Stretch)(w.resDev, surf, 0, w.bb, 0, 0);
}
// what the eye gets: every picture shown, the time since the one before it (microseconds where a maximum is kept)
static volatile LONG g_twNIbHeld = 0, g_twNOrderBad = 0, g_twNForced = 0, g_twNPulled = 0, g_twNBeatLate = 0, g_twBeatLateMax = 0, g_twNPicShown = 0, g_twNGap15 = 0, g_twNGap25 = 0, g_twGapMax = 0,
                     g_twNPanPic = 0, g_twNPanGap15 = 0, g_twNPanGap25 = 0, g_twPanGapMax = 0, g_twShowLateMax = 0, g_twNAged = 0, g_twNInHand = 0;
static volatile LONG64 g_twBeatLateSum = 0, g_twGapSum = 0, g_twShowLateSum = 0, g_twInHandSum = 0;
// How far what is on screen lags the game: from the game thread queuing a frame's Present to that frame's own picture
// going up (the in-between picture ahead of it shows the halfway point half a frame period earlier, so the motion on
// screen runs behind the game's by this much throughout). Measured per picture; the camera code makes up for it where
// the engine times something by the clock (aotr_scroll.inc: the edge-scroll ramp).
static volatile LONG g_twLagTicks = 0, g_twLagN = 0, g_twLagIbN = 0;     // smoothed lag in QPC ticks; pictures measured
static volatile LONG64 g_twLagSum = 0, g_twLagIbSum = 0;
static void twLagNote(DWORD gt, LONG64 t, LONG ib) {
    if (!gt) return;
    DWORD lag = (DWORD)t - gt;
    if (lag >= (DWORD)(g_pqpf.QuadPart / 2)) return;        // across a pause or a load: not a lag
    if (ib) { g_twLagIbSum += lag; g_twLagIbN++; return; }
    g_twLagSum += lag; g_twLagN++;
    LONG cur = g_twLagTicks; g_twLagTicks = cur ? (cur * 7 + (LONG)lag) / 8 : (LONG)lag;
}
static __forceinline LONG twUs(LONG64 ticks) { LONG64 us = ticks * 1000000 / (g_pqpf.QuadPart ? g_pqpf.QuadPart : 1); return us > 0x7FFFFFFF ? 0x7FFFFFFF : us < 0 ? 0 : (LONG)us; }
static LONG twUsFwd(LONG64 ticks) { return twUs(ticks); }
static bool twSurfBusy(DWORD s) {
    const TwW& w = g_tw;
    if (s == w.standIn || s == w.ibSurf || (w.replaying && s == w.keepStandIn) || s == g_pwLastSurf) return true;
    for (LONG i = 0; i < w.qn; ++i) if (w.q[i].surf == s) return true;
    return false;
}
// The oldest waiting picture goes on screen. why: 0 its time has come, 1 its target is needed, 2 nothing may wait any longer.
// at: on the screen's beat, the refresh it is handed over for (0: none - it is shown for this moment).
static void twShowHead(int why, LONG64 at = 0) {
    TwW& w = g_tw;
    if (w.qn <= 0) return;
    TwPic p = w.q[0];
    for (LONG i = 1; i < w.qn; ++i) w.q[i - 1] = w.q[i];
    w.qn--;
    bool scene = w.inScene != 0;
    if (scene) ((TwF1)g_twO_EndScene)(w.resDev);
    bool moved = pwTryShow(p, at ? at : qpcNow(), at != 0);       // from where the camera is at that moment (aotr_panwarp.inc), or as it is
    if (!moved) twCopyToBackBuffer(p.surf);
    else if (g_pwTest & 1) pwTestCompare(p);
    g_pw.shifted = moved ? 1 : 0;
    if (at) pwHandNote();
    DWORD hr = twRealPresent(p.args);
    if (scene) ((TwF1)g_twO_BeginScene)(w.resDev);
    LONG64 t = qpcNow();
    pwShown(p, t);
    twLagNote(p.gt, t, p.ib);
    if (p.seq != w.seqOut + 1) InterlockedIncrement(&g_twNOrderBad);          // never out of the order they were made in
    w.seqOut = p.seq;
    if (w.lastShow && p.beat > 0) {
        LONG64 gap = t - w.lastShow;
        if (gap > 0 && gap < p.beat * 20) {                // (not across a pause)
            LONG us = twUs(gap);
            bool g15 = gap * 2 > p.beat * 3, g25 = gap * 2 > p.beat * 5;
            g_twNPicShown++; g_twGapSum += gap;
            if (us > g_twGapMax) g_twGapMax = us;
            if (g15) g_twNGap15++;
            if (g25) g_twNGap25++;
            if (p.pan) { g_twNPanPic++; if (g15) g_twNPanGap15++; if (g25) g_twNPanGap25++; if (us > g_twPanGapMax) g_twPanGapMax = us; }
        }
    }
    if (at) { g_pwNGridNew++; if (w.lastShow) pwGridGapNote(t - w.lastShow); g_pw.slotVb = at; }
    if (why == 0) { LONG64 late = t - p.due; if (late > 0) { g_twShowLateSum += late; LONG us = twUs(late); if (us > g_twShowLateMax) g_twShowLateMax = us; } g_twNOnTime++; }
    else if (why == 1) { g_twNForced++; g_twNLate++; }
    w.lastShow = t; w.lastHr = hr;
    if (!p.ib) g_twNShown++;
    InterlockedExchange(&g_rtPresentHR, (LONG)hr);
}
// A waiting picture is not shown: a newer one goes up at this refresh (or a scroll's end is being played out).
static void twPassOver() {
    TwW& w = g_tw;
    if (w.qn <= 0) return;
    TwPic p = w.q[0];
    for (LONG i = 1; i < w.qn; ++i) w.q[i - 1] = w.q[i];
    w.qn--;
    if (p.seq != w.seqOut + 1) InterlockedIncrement(&g_twNOrderBad);
    w.seqOut = p.seq;
}
static void twShowDue() {                                 // exec lock held: whatever is due goes on screen - but never two pictures in the same instant
    TwW& w = g_tw;
    while (w.qn > 0) {
        LONG64 now = qpcNow();
        if (pwGrid()) {                                    // while it scrolls: one picture for each refresh of the screen (aotr_panwarp.inc), the newest that is due by then
            LONG64 at, vb; pwSlot(now, &at, &vb);
            if (now < at) break;
            LONG n = 0; while (n < w.qn && w.q[n].due <= vb) ++n;
            if (!n) break;                                 // none: the one on screen goes up again from further along (pwFillerTick)
            const TwPic& last = w.q[n - 1];
            if (!g_pw.path[g_pw.pathN - 1].moving && !(last.camOk && last.scn)) {      // the scroll's end is being played out and this is a picture from after it: it cannot be shown from
                while (n-- > 0) { twPassOver(); g_pwNHeld++; }                         // an earlier place - it waits for nothing; the one on screen is shown again until the camera has arrived
                break;
            }
            while (n-- > 1) { twPassOver(); g_pwNPassed++; }
            g_pwHandAt = at;
            twShowHead(0, vb);
            break;
        }
        if (now < w.q[0].due || now < w.lastShow + w.q[0].beat / 2) break;      // otherwise only never two in the same instant
        twShowHead(0);
    }
}
static void twFlush() { while (g_tw.qn > 0) twShowHead(2); }
static DWORD twFreeSurf() {                               // a target nothing waits in; if there is none, the oldest waiting picture is shown now
    TwW& w = g_tw;
    for (;;) {
        for (LONG i = 0; i < w.nOff; ++i) if (!twSurfBusy(w.off[i])) return w.off[i];
        if (w.qn <= 0) return 0;
        twShowHead(1);
    }
}
static void twQueue(DWORD surf, LONG64 due, LONG64 beat, LONG64 made, const TwPresentArgs& pa, LONG ib, LONG pan, DWORD gt) {
    TwW& w = g_tw;
    if (w.qn >= TW_QN) twShowHead(1);
    TwPic& p = w.q[w.qn++];
    p.surf = surf; p.due = due; p.beat = beat; p.made = made; p.args = pa; p.ib = ib; p.pan = pan; p.gt = gt; p.seq = ++w.seqIn;
    { const PwCam& c = g_pwPend[ib ? 1 : 0];
      p.camOk = c.ok; p.ct = c.ct; p.mt = c.mt; p.gh = c.gh; p.scn = c.scn;
      if (c.ok) { memcpy(p.V, c.V, 64); memcpy(p.P, c.P, 64); } }
}
// Both of a frame's pictures exist at a (only the game's frame when ibSurf is 0): when do they go on screen?
static void twSchedule(LONG64 a, DWORD ibSurf, DWORD gSurf, const TwPresentArgs& pa, LONG test, LONG pan, DWORD gt) {
    TwW& w = g_tw;
    LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1, x = a, y = a, beat = 0;
    if (test & (1 | 16)) {                                 // tests: at once (and, every other one, not before its target is needed)
        if ((test & 8) && (g_twNReplay & 1)) y = a + f * 60;
        w.pace.lastDue = a;
    } else if (!g_twBeatOn) {                              // as each is finished: the in-between frame now, the game's frame half a usual period later
        beat = w.pace.period / 2;
        y = a + beat + (ibSurf ? 0 : w.replayCost);
        w.pace.lastDue = y;
    } else {
        twpPlace(w.pace, a, ibSurf != 0, &x, &y, &beat);   // on the beat (aotr_pace.inc)
        if (ibSurf) {
            if (w.pace.late) { LONG us = twUs(w.pace.miss); g_twNBeatLate++; g_twBeatLateSum += w.pace.miss; if (us > g_twBeatLateMax) g_twBeatLateMax = us; }
            if (w.pace.held) g_twNIbHeld++;
            if (w.pace.pulled) g_twNPulled++;
            g_twInHandSum += w.pace.inHand; g_twNInHand++;
        }
    }
    if (ibSurf) twQueue(ibSurf, x, beat, a, pa, 1, pan, gt);
    twQueue(gSurf, y, beat, a, pa, 0, pan, gt);
}
// Render thread, between two records of a batch: only what the clock asks for - a picture whose moment has come.
static void twServicePictures() {
    TwW& w = g_tw;
    if (w.qn > 0) twShowDue();
    pwFillerTick();
    if (g_pwSlotDue && !pwGrid()) g_pwSlotDue = 0;
}
// Render thread, between records and while idle (exec lock held).
static void twService() {
    TwW& w = g_tw;
    if (g_twGiveUp) {
        if (g_twStretch) { g_twNGiveUp++; w.cancel = 1; twRunDeferred(); InterlockedExchange(&g_twHold, 0); }   // the records may be overwritten from here on
        InterlockedExchange(&g_twGiveUp, 0);
    }
    if (w.qn > 0) twShowDue();
    pwFillerTick();                                        // the beat has come and no picture is there: the one on screen, from further along the pan
    if (g_pwSlotDue && !pwGrid()) g_pwSlotDue = 0;
    InterlockedExchange(&g_twBusy, (w.qn > 0 || g_twGiveUp || pwBusy()) ? 1 : 0);
}
static DWORD twIdleWaitMs() {                             // how long the idle render thread may sleep
    TwW& w = g_tw;
    LONG64 now = qpcNow(), due; bool grid = false;
    if (pwGrid()) { LONG64 vb; pwSlot(now, &due, &vb); grid = true; }   // the screen's next refresh, less the lead
    else if (w.qn <= 0) return (g_pwOn && g_pw.lastOk) ? 1 : 2;
    else { due = w.q[0].due; LONG64 soonest = w.lastShow + w.q[0].beat / 2; if (soonest > due) due = soonest; }
    LONG64 left = due - now;
    if (left <= 0) return 0;
    LONG64 ms = left * 1000 / (g_pqpf.QuadPart ? g_pqpf.QuadPart : 1);
    if (grid) return ms >= 5 ? 2 : (ms >= 4 ? 1 : 0);      // (a sleep of 1 ms may take 2 and a wake-up its time: the last 3 ms before a refresh's picture are not slept through)
    return ms >= 3 ? 2 : (ms >= 2 ? 1 : 0);
}
// Game thread: no ring space while the render thread sits on a stretch.
static void twSpacePressure() {
    if (!g_twHold) return;
    InterlockedExchange(&g_twGiveUp, 1); InterlockedExchange(&g_twBusy, 1);
    if (g_rtWorkerSleeping) SetEvent(g_rtWake);
    for (int spins = 0; g_twHold && spins < 2000000; ++spins) { if (spins < 4000) _mm_pause(); else Sleep(0); }
}

// ---------------------------------------------------------------- what moves: two frames' values
static bool twGenAlloc() {
    TwW& w = g_tw;
    for (int i = 0; i < 2; ++i) {
        TwGen& g = w.gen[i];
        if (!g.key) g.key = (TwKey*)VirtualAlloc(NULL, TW_KEYS * sizeof(TwKey), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g.arena) g.arena = (BYTE*)VirtualAlloc(NULL, TW_ARENA, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g.cam) g.cam = (TwCam*)VirtualAlloc(NULL, TW_CAMS * sizeof(TwCam), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g.key || !g.arena || !g.cam) return false;
        if (!g.stamp) g.stamp = 1;
    }
    if (!w.cls) w.cls = (TwCls*)VirtualAlloc(NULL, TW_CLS_N * sizeof(TwCls), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!w.undo) w.undo = (TwUndo*)VirtualAlloc(NULL, TW_UNDO_N * sizeof(TwUndo), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return w.cls && w.undo;
}
static void twGenBegin(TwGen& g) {
    if (++g.stamp == 0) { memset(g.key, 0, TW_KEYS * sizeof(TwKey)); g.stamp = 1; }
    g.used = 0; g.nCam = 0; g.valid = 0; g.over = 0;
}
static __forceinline DWORD twKeyHash(DWORD mesh, DWORD fx, DWORD h, DWORD occ) {
    DWORD x = mesh * 2654435761u; x ^= (h + occ * 0x9E3779B9u) * 2246822519u; x ^= fx >> 3;
    return (x ^ (x >> 15)) & (TW_KEYS - 1);
}
static const TwKey* twKeyFind(const TwGen& g, DWORD mesh, DWORD fx, DWORD h, DWORD occ) {
    DWORD i = twKeyHash(mesh, fx, h, occ);
    for (int k = 0; k < 16; ++k, i = (i + 1) & (TW_KEYS - 1)) {
        const TwKey& e = g.key[i];
        if (e.stamp != g.stamp) return NULL;
        if (e.mesh == mesh && e.h == h && e.fx == fx && e.occ == (WORD)occ) return &e;
    }
    return NULL;
}
static void twKeyPut(TwGen& g, DWORD mesh, DWORD fx, DWORD h, DWORD occ, const void* data, DWORD nb) {
    if (g.used + nb > TW_ARENA) { g.over++; g_twNKeyOver++; return; }
    DWORD i = twKeyHash(mesh, fx, h, occ);
    for (int k = 0; k < 16; ++k, i = (i + 1) & (TW_KEYS - 1)) {
        TwKey& e = g.key[i];
        if (e.stamp == g.stamp) continue;
        memcpy(g.arena + g.used, data, nb);
        e.mesh = mesh; e.fx = fx; e.h = h; e.off = g.used; e.occ = (WORD)occ; e.nb = (WORD)nb; e.stamp = g.stamp;
        g.used += (nb + 3) & ~3u;
        return;
    }
    g.over++; g_twNKeyOver++;
}
static void twCamPut(TwGen& g, DWORD fx, DWORD h, const float* v) {
    for (LONG i = 0; i < g.nCam; ++i) if (g.cam[i].fx == fx && g.cam[i].h == h && !memcmp(g.cam[i].v, v, 64)) return;
    if (g.nCam >= TW_CAMS) return;
    TwCam& c = g.cam[g.nCam++]; c.fx = fx; c.h = h; memcpy(c.v, v, 64);
}
static const float* twCamNearest(const TwGen& g, DWORD fx, DWORD h, const float* v) {
    const float* best = NULL; double bd = 0;
    for (LONG i = 0; i < g.nCam; ++i) {
        const TwCam& c = g.cam[i];
        if (c.fx != fx || c.h != h) continue;
        double d = 0; for (int k = 0; k < 16; ++k) { double x = (double)c.v[k] - v[k]; d += x * x; }
        if (!best || d < bd) { best = c.v; bd = d; }
    }
    return best;
}
// effect parameter -> what it is, by the semantic (or the name) the shader gives it
static DWORD twClassify(DWORD fx, DWORD h) {
    BYTE desc[64]; memset(desc, 0, sizeof(desc));
    DWORD cls = TWC_NONE;
    __try {
        if ((LONG)((TwF3)g_rtO_fx[4])(fx, h, (DWORD)(ULONG_PTR)desc) < 0) return TWC_NONE;
        const char* name = *(const char**)desc; const char* sem = *(const char**)(desc + 4);
        DWORD pc = *(DWORD*)(desc + 8), pt = *(DWORD*)(desc + 12), elems = *(DWORD*)(desc + 24), bytes = *(DWORD*)(desc + 40);
        if (!name) return TWC_NONE;
        if (!strncmp(name, "WorldBones", 10)) { if (pc == 5 && elems && bytes == elems * 32) cls = TWC_PALETTE; }      // an array of { quaternion, position } structs, nothing else
        else if ((pc == 2 || pc == 3) && pt == 3) {
            static const struct { const char* sem; const char* name; DWORD cls; } k[] = {
                { "WORLD", "World", TWC_WORLD }, { "VIEW", "View", TWC_VIEW }, { "PROJECTION", "Projection", TWC_PROJ }, { "VIEWINVERSE", "ViewI", TWC_VIEWI },
                { "WORLDVIEWPROJECTION", "WorldViewProjection", TWC_WVP }, { "WORLDVIEW", "WorldView", TWC_WV }, { "WORLDVIEWINVERSE", "WorldViewI", TWC_WVI } };
            for (int i = 0; i < 7 && !cls; ++i) if ((sem && sem[0]) ? !lstrcmpiA(sem, k[i].sem) : !strcmp(name, k[i].name)) cls = k[i].cls;
        }
        if (cls) {
            LONG n = g_twNNames; bool seen = false;
            for (LONG i = 0; i < n && i < 16; ++i) if (!strncmp(g_twNames[i], name, 39)) seen = true;
            if (!seen && n < 16) { lstrcpynA(g_twNames[n], name, 40); InterlockedIncrement(&g_twNNames); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return TWC_NONE; }
    return cls;
}
static __forceinline DWORD twClass(DWORD fx, DWORD h) {
    TwCls* t = g_tw.cls; LONG gen = g_twFxGen;
    DWORD i = (((fx >> 4) * 2654435761u) ^ (h * 2246822519u)) >> 19;
    for (int k = 0; k < 8; ++k, i = (i + 1) & (TW_CLS_N - 1)) {
        TwCls& c = t[i];
        if (c.fx == fx && c.h == h && c.gen == gen) return c.cls;
        if (c.gen != gen || !c.fx) { c.cls = twClassify(fx, h); c.fx = fx; c.h = h; c.gen = gen; return c.cls; }
    }
    return twClassify(fx, h);
}

// ---------------------------------------------------------------- the blends
// A joint palette: quaternion (x y z w) + position (x y z) + one unused word, 32 bytes a joint. false: leave it.
// (twLerpPaletteRef is the definition, one joint at a time. twLerpPalette below does the same arithmetic four joints
// at a time and is what runs; AotrTweenTestLerp compares the two bit for bit.)
static __forceinline bool twLerpJoint(const float* q0, const float* q1, float t, float far2, float* o) {       // false: this joint says "leave the palette"
    float dx = q1[4] - q0[4], dy = q1[5] - q0[5], dz = q1[6] - q0[6];
    if (!(dx*dx + dy*dy + dz*dz <= far2)) return false;                                 // further than anything moves in one frame (or not a number)
    float la = q0[0]*q0[0] + q0[1]*q0[1] + q0[2]*q0[2] + q0[3]*q0[3];
    float lb = q1[0]*q1[0] + q1[1]*q1[1] + q1[2]*q1[2] + q1[3]*q1[3];
    if (!(la > 0.8f && la < 1.25f && lb > 0.8f && lb < 1.25f)) return false;             // not two rotations: whatever this is, it is left alone
    float d = q0[0]*q1[0] + q0[1]*q1[1] + q0[2]*q1[2] + q0[3]*q1[3];
    float s = d < 0 ? -1.0f : 1.0f;
    float o4 = q0[4] + (q1[4] - q0[4]) * t, o5 = q0[5] + (q1[5] - q0[5]) * t, o6 = q0[6] + (q1[6] - q0[6]) * t; DWORD o7 = ((const DWORD*)q1)[7];
    if (d * s < 0.3f) { o[0] = q1[0]; o[1] = q1[1]; o[2] = q1[2]; o[3] = q1[3]; }          // turned too far in one frame to be one motion
    else {
        float w0 = (1.0f - t) * s, w1 = t;
        float x = q0[0]*w0 + q1[0]*w1, y = q0[1]*w0 + q1[1]*w1, z = q0[2]*w0 + q1[2]*w1, ww = q0[3]*w0 + q1[3]*w1;
        float l = x*x + y*y + z*z + ww*ww;
        float inv = l > 1e-12f ? 1.0f / (float)sqrt(l) : 1.0f;
        o[0] = x * inv; o[1] = y * inv; o[2] = z * inv; o[3] = ww * inv;
    }
    o[4] = o4; o[5] = o5; o[6] = o6; ((DWORD*)o)[7] = o7;
    return true;
}
static bool twLerpPaletteRef(const float* a, const float* b, DWORD joints, float t, float* out) {
    float far2 = g_twMaxMove * g_twMaxMove;
    for (DWORD i = 0; i < joints; ++i) {
        float dx = b[i*8+4] - a[i*8+4], dy = b[i*8+5] - a[i*8+5], dz = b[i*8+6] - a[i*8+6];
        if (!(dx*dx + dy*dy + dz*dz <= far2)) return false;
        float la = a[i*8]*a[i*8] + a[i*8+1]*a[i*8+1] + a[i*8+2]*a[i*8+2] + a[i*8+3]*a[i*8+3];
        float lb = b[i*8]*b[i*8] + b[i*8+1]*b[i*8+1] + b[i*8+2]*b[i*8+2] + b[i*8+3]*b[i*8+3];
        if (!(la > 0.8f && la < 1.25f && lb > 0.8f && lb < 1.25f)) return false;
    }
    for (DWORD i = 0; i < joints; ++i) {
        const float* q0 = a + i*8; const float* q1 = b + i*8; float* o = out + i*8;
        float d = q0[0]*q1[0] + q0[1]*q1[1] + q0[2]*q1[2] + q0[3]*q1[3];
        float s = d < 0 ? -1.0f : 1.0f;
        if (d * s < 0.3f) { o[0] = q1[0]; o[1] = q1[1]; o[2] = q1[2]; o[3] = q1[3]; }
        else {
            float w0 = (1.0f - t) * s, w1 = t;
            float x = q0[0]*w0 + q1[0]*w1, y = q0[1]*w0 + q1[1]*w1, z = q0[2]*w0 + q1[2]*w1, ww = q0[3]*w0 + q1[3]*w1;
            float l = x*x + y*y + z*z + ww*ww;
            float inv = l > 1e-12f ? 1.0f / (float)sqrt(l) : 1.0f;
            o[0] = x * inv; o[1] = y * inv; o[2] = z * inv; o[3] = ww * inv;
        }
        o[4] = q0[4] + (q1[4] - q0[4]) * t; o[5] = q0[5] + (q1[5] - q0[5]) * t; o[6] = q0[6] + (q1[6] - q0[6]) * t;
        ((DWORD*)o)[7] = ((const DWORD*)q1)[7];
    }
    return true;
}
// Four joints at a time. Every operation is the one-joint code's own, in its order, on single-precision lanes: the
// sums are added left to right, the square root is the correctly rounded one (which is what (float)sqrt(double)
// gives), comparisons are the ordered ones (false for a NaN, as in C). So the result is the same in every bit - and
// so is the verdict. When the verdict is "leave it" the output holds unfinished values; both callers then overwrite
// it with the palette as it is. The unused eighth word is kept out of the arithmetic (it may be anything).
static bool twLerpPalette(const float* a, const float* b, DWORD joints, float t, float* out) {
    const float far2 = g_twMaxMove * g_twMaxMove;
    const __m128 vFar = _mm_set1_ps(far2), vLo = _mm_set1_ps(0.8f), vHi = _mm_set1_ps(1.25f), vOne = _mm_set1_ps(1.0f), vMinus = _mm_set1_ps(-1.0f),
                 vT = _mm_set1_ps(t), vOmT = _mm_set1_ps(1.0f - t), vTurn = _mm_set1_ps(0.3f), vTiny = _mm_set1_ps(1e-12f), vZero = _mm_setzero_ps();
    const __m128 mXyz = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
    __m128 bad = _mm_setzero_ps();
    DWORD i = 0;
    for (; i + 4 <= joints; i += 4) {
        const float* pa = a + i * 8; const float* pb = b + i * 8; float* po = out + i * 8;
        __m128 ax = _mm_loadu_ps(pa), ay = _mm_loadu_ps(pa + 8), az = _mm_loadu_ps(pa + 16), aw = _mm_loadu_ps(pa + 24);      // four quaternions, one a row ...
        __m128 bx = _mm_loadu_ps(pb), by = _mm_loadu_ps(pb + 8), bz = _mm_loadu_ps(pb + 16), bw = _mm_loadu_ps(pb + 24);
        _MM_TRANSPOSE4_PS(ax, ay, az, aw);                                                                                    // ... now one component a row
        _MM_TRANSPOSE4_PS(bx, by, bz, bw);
        __m128 pa0 = _mm_loadu_ps(pa + 4), pa1 = _mm_loadu_ps(pa + 12), pa2 = _mm_loadu_ps(pa + 20), pa3 = _mm_loadu_ps(pa + 28);
        __m128 pb0 = _mm_loadu_ps(pb + 4), pb1 = _mm_loadu_ps(pb + 12), pb2 = _mm_loadu_ps(pb + 20), pb3 = _mm_loadu_ps(pb + 28);
        __m128 u0 = _mm_andnot_ps(mXyz, pb0), u1 = _mm_andnot_ps(mXyz, pb1), u2 = _mm_andnot_ps(mXyz, pb2), u3 = _mm_andnot_ps(mXyz, pb3);   // the eighth words, as they are
        pa0 = _mm_and_ps(pa0, mXyz); pa1 = _mm_and_ps(pa1, mXyz); pa2 = _mm_and_ps(pa2, mXyz); pa3 = _mm_and_ps(pa3, mXyz);
        pb0 = _mm_and_ps(pb0, mXyz); pb1 = _mm_and_ps(pb1, mXyz); pb2 = _mm_and_ps(pb2, mXyz); pb3 = _mm_and_ps(pb3, mXyz);
        __m128 d0 = _mm_sub_ps(pb0, pa0), d1 = _mm_sub_ps(pb1, pa1), d2 = _mm_sub_ps(pb2, pa2), d3 = _mm_sub_ps(pb3, pa3);
        // positions: q0 + (q1 - q0) * t, the eighth word from q1
        _mm_storeu_ps(po + 4,  _mm_or_ps(_mm_add_ps(pa0, _mm_mul_ps(d0, vT)), u0));
        _mm_storeu_ps(po + 12, _mm_or_ps(_mm_add_ps(pa1, _mm_mul_ps(d1, vT)), u1));
        _mm_storeu_ps(po + 20, _mm_or_ps(_mm_add_ps(pa2, _mm_mul_ps(d2, vT)), u2));
        _mm_storeu_ps(po + 28, _mm_or_ps(_mm_add_ps(pa3, _mm_mul_ps(d3, vT)), u3));
        // how far each joint moved, and whether both are rotations
        _MM_TRANSPOSE4_PS(d0, d1, d2, d3);
        __m128 dist = _mm_add_ps(_mm_add_ps(_mm_mul_ps(d0, d0), _mm_mul_ps(d1, d1)), _mm_mul_ps(d2, d2));
        bad = _mm_or_ps(bad, _mm_cmpnle_ps(dist, vFar));
        __m128 la = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(ax, ax), _mm_mul_ps(ay, ay)), _mm_mul_ps(az, az)), _mm_mul_ps(aw, aw));
        __m128 lb = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(bx, bx), _mm_mul_ps(by, by)), _mm_mul_ps(bz, bz)), _mm_mul_ps(bw, bw));
        __m128 ok = _mm_and_ps(_mm_and_ps(_mm_cmpgt_ps(la, vLo), _mm_cmplt_ps(la, vHi)), _mm_and_ps(_mm_cmpgt_ps(lb, vLo), _mm_cmplt_ps(lb, vHi)));
        bad = _mm_or_ps(bad, _mm_cmpeq_ps(ok, vZero));                                                                        // (a lane that is not all ones is +0)
        // the rotations
        __m128 d = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(ax, bx), _mm_mul_ps(ay, by)), _mm_mul_ps(az, bz)), _mm_mul_ps(aw, bw));
        __m128 neg = _mm_cmplt_ps(d, vZero);
        __m128 s = _mm_or_ps(_mm_and_ps(neg, vMinus), _mm_andnot_ps(neg, vOne));
        __m128 turned = _mm_cmplt_ps(_mm_mul_ps(d, s), vTurn);
        __m128 w0 = _mm_mul_ps(vOmT, s);
        __m128 x = _mm_add_ps(_mm_mul_ps(ax, w0), _mm_mul_ps(bx, vT)), y = _mm_add_ps(_mm_mul_ps(ay, w0), _mm_mul_ps(by, vT)),
               z = _mm_add_ps(_mm_mul_ps(az, w0), _mm_mul_ps(bz, vT)), w = _mm_add_ps(_mm_mul_ps(aw, w0), _mm_mul_ps(bw, vT));
        __m128 l = _mm_add_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(x, x), _mm_mul_ps(y, y)), _mm_mul_ps(z, z)), _mm_mul_ps(w, w));
        __m128 big = _mm_cmpgt_ps(l, vTiny);
        __m128 inv = _mm_or_ps(_mm_and_ps(big, _mm_div_ps(vOne, _mm_sqrt_ps(l))), _mm_andnot_ps(big, vOne));
        x = _mm_or_ps(_mm_and_ps(turned, bx), _mm_andnot_ps(turned, _mm_mul_ps(x, inv)));
        y = _mm_or_ps(_mm_and_ps(turned, by), _mm_andnot_ps(turned, _mm_mul_ps(y, inv)));
        z = _mm_or_ps(_mm_and_ps(turned, bz), _mm_andnot_ps(turned, _mm_mul_ps(z, inv)));
        w = _mm_or_ps(_mm_and_ps(turned, bw), _mm_andnot_ps(turned, _mm_mul_ps(w, inv)));
        _MM_TRANSPOSE4_PS(x, y, z, w);
        _mm_storeu_ps(po, x); _mm_storeu_ps(po + 8, y); _mm_storeu_ps(po + 16, z); _mm_storeu_ps(po + 24, w);
    }
    if (_mm_movemask_ps(bad)) return false;
    for (; i < joints; ++i) if (!twLerpJoint(a + i * 8, b + i * 8, t, far2, out + i * 8)) return false;
    return true;
}
static __forceinline void twLerpMat(const float* a, const float* b, float t, float* out) { for (int i = 0; i < 16; ++i) out[i] = a[i] + (b[i] - a[i]) * t; }
// May these two matrices be one motion? d3dLayout: translation in elements 12..14 (else 3, 7, 11 - the engine's
// transposed matrices). proj: no translation, every element compared relative to its size.
static bool twMatNear(const float* a, const float* b, bool d3dLayout, bool proj, float maxMove) {
    for (int i = 0; i < 16; ++i) {
        float d = b[i] - a[i]; if (d < 0) d = -d;
        if (proj) { float m = a[i] < 0 ? -a[i] : a[i], m2 = b[i] < 0 ? -b[i] : b[i]; if (m2 > m) m = m2; if (d > 0.3f * m + 1e-4f) return false; continue; }
        bool tr = d3dLayout ? (i >= 12 && i < 15) : (i == 3 || i == 7 || i == 11);
        if (d > (tr ? maxMove : 0.35f)) return false;
    }
    return true;
}
static void twMul4d(const double* a, const double* b, double* o) {
    double t[16];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) t[r*4+c] = a[r*4]*b[c] + a[r*4+1]*b[4+c] + a[r*4+2]*b[8+c] + a[r*4+3]*b[12+c];
    memcpy(o, t, sizeof(t));
}
static bool twInv4d(const double* m, double* o) {
    double inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det > -1e-30 && det < 1e-30) return false;
    det = 1.0 / det;
    for (int i = 0; i < 16; ++i) o[i] = inv[i] * det;
    return true;
}
// The matrices that take this frame's camera to the halfway camera, in Direct3D's row-vector layout:
//   C  = (V P)^-1 (V' P')     W V P   ->  W V' P'   is  X C
//   Cv = V^-1 V'              W V     ->  W V'      is  X Cv
//   Ci = V'^-1 V              (W V)^-1 -> (W V')^-1 is  Ci X
struct TwCamFix { double C[16], Cv[16], Ci[16]; };
static bool twCamFixBuild(const float* V, const float* P, const float* V2, const float* P2, TwCamFix* f) {
    double v[16], p[16], v2[16], p2[16], vp[16], vp2[16], iv[16], ivp[16], iv2[16];
    for (int i = 0; i < 16; ++i) { v[i] = V[i]; p[i] = P[i]; v2[i] = V2[i]; p2[i] = P2[i]; }
    twMul4d(v, p, vp); twMul4d(v2, p2, vp2);
    if (!twInv4d(vp, ivp) || !twInv4d(v, iv) || !twInv4d(v2, iv2)) return false;
    twMul4d(ivp, vp2, f->C); twMul4d(iv, v2, f->Cv); twMul4d(iv2, v, f->Ci);
    return true;
}
// X: 16 floats as the game passed them. transposed: through SetMatrixTranspose (the engine's layout).
static void twCamFixApply(const TwCamFix& f, DWORD cls, bool transposed, const float* X, float* out) {
    double x[16], r[16];
    if (transposed) for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) x[i*4+j] = X[j*4+i];
    else for (int i = 0; i < 16; ++i) x[i] = X[i];
    if (cls == TWC_WVP) twMul4d(x, f.C, r); else if (cls == TWC_WV) twMul4d(x, f.Cv, r); else twMul4d(f.Ci, x, r);
    if (transposed) for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out[j*4+i] = (float)r[i*4+j];
    else for (int i = 0; i < 16; ++i) out[i] = (float)r[i];
}

#include "aotr_devpass.inc"

// ---------------------------------------------------------------- the second pass
struct TwWalk {
    int exec, blend; float t;
    int fxExec, dv, dvTrack;                                // D3DX gets this pass's parameter writes; how the pass is made (aotr_devpass.inc: 0 by D3DX, 1 at the device, 2 by D3DX and compared); blended values are kept for the device
    LONG camFxN;                                            // camera matrices of shaders that got a blended value in this pass
    TwGen* prev; TwGen* cur; DWORD dev;
    DWORD tagN; const DWORD* tag;
    float devV[16], devP[16], twV[16], twP[16]; bool haveV, haveP, subV, subP, fixOk, fixDirty;   // the stream's VIEW / PROJECTION, and what the device holds in this pass
    float blV[16], blP[16]; bool blVok, blPok;              // the stream's value blended with last frame's (when it could be)
    float camV[16], camTw[16], camP[16], camPTw[16]; bool camSub, camPHave;      // the last real camera and perspective the stream set, and their blends
    float endV[16], endP[16], endTwV[16], endTwP[16]; bool endOk, camVHave;      // (v59) the camera the 3D pass was drawn with, and its blend, known where the pass ends
    // (v60) Which camera that is: the VIEW under which most of the pass's draws were made. Not the last one set - right
    // before the interface the engine sets its 2D camera (translation 0,0,-1; capture_3.txt line 194284), and a view
    // inside the interface has another. A camera's run lasts until a different non-identity VIEW is set (the identity
    // VIEWs of camera-space geometry and of the bars do not end it).
    float epV[16], epP[16], epTwV[16], epTwP[16]; bool epHave, epPHave; LONG epDraws, bestDraws;
    bool in3D, sawEnd;                                      // still inside the 3D pass (the game thread marks its end); this stretch has that mark
    float sumZ; LONG nZ;                                    // where this frame's units stand (first joint of every tagged palette)
    LONG nTag;                                              // meshes tagged in this stretch
    float camMove;                                          // how far the view moved from the frame before (world units), 0 when it stood still
    TwCamFix fix;
    float scratch[100 * 8 + 16];
};
static TwWalk g_twWalk;
static __forceinline TwUndo* twUndoSlot(DWORD fx, DWORD h) {
    TwW& w = g_tw;
    DWORD i = (((fx >> 4) * 2654435761u) ^ (h * 2246822519u)) >> 21;
    for (int k = 0; k < 8; ++k, i = (i + 1) & (TW_UNDO_N - 1)) {
        TwUndo& u = w.undo[i];
        if (u.stamp == w.undoStamp && u.fx == fx && u.h == h) return &u;
        if (u.stamp != w.undoStamp) { u.stamp = w.undoStamp; u.fx = fx; u.h = h; u.e = NULL; u.sub = 0; return &u; }
    }
    return NULL;
}
static __forceinline void twUndoNote(DWORD fx, DWORD h, const DWORD* e, bool substituted) {
    TwUndo* u = twUndoSlot(fx, h);
    if (u) { u->e = e; u->sub = substituted ? 1u : 0u; }
}
// A palette write that follows a tag. Returns the bytes to write (the blend, or the stream's own).
static const void* twPalette(TwWalk& W, DWORD fx, DWORD h, const DWORD* e, const void* data, DWORD nb) {
    DWORD n = W.tagN;
    if (!n || nb > 3200 || (nb % (32 * n))) { if (W.dvTrack) dvValPlain(fx, h, 0); return data; }
    DWORD per = nb / n;
    bool any = false;
    BYTE* base = (BYTE*)W.scratch; DvVal* dv = NULL;
    if (W.dvTrack) { dv = dvValFor(fx, h, TWC_PALETTE, 78, nb); if (dv) base = dv->buf; }
    for (DWORD m = 0; m < n; ++m) {
        DWORD mesh = W.tag[m];
        const float* cur = (const float*)((const BYTE*)data + m * per);
        DWORD occ = 0;
        while (occ < 6 && twKeyFind(*W.cur, mesh, fx, h, occ)) ++occ;
        const TwKey* pk = (W.prev && W.blend) ? twKeyFind(*W.prev, mesh, fx, h, occ) : NULL;
        twKeyPut(*W.cur, mesh, fx, h, occ, cur, per);
        if (cur[4] != 0.0f || cur[5] != 0.0f || cur[6] != 0.0f) { W.sumZ += cur[6]; W.nZ++; }      // (a rigid mesh's identity palette is nowhere)
        if (!W.blend) continue;
        float* out = (float*)(base + m * per);
        if (pk && pk->nb == per) {
            const float* prev = (const float*)(W.prev->arena + pk->off);
            if (!memcmp(prev, cur, per)) { memcpy(out, cur, per); continue; }
            if (twLerpPalette(prev, cur, per / 32, W.t, out)) { any = true; g_twNPal++; continue; }
            g_twNPalFar++;
        } else g_twNPalNew++;
        memcpy(out, cur, per);
    }
    if (W.blend) twUndoNote(fx, h, e, any);
    if (W.dvTrack) dvValSet(dv, data, nb, any);
    return any ? (const void*)base : data;
}
static const void* twMatrix(TwWalk& W, DWORD fx, DWORD h, DWORD slot, const DWORD* e, const void* data) {
    DWORD cls = twClass(fx, h);
    if (cls < TWC_WORLD) return data;
    const float* cur = (const float*)data;
    float* out = W.scratch;
    bool sub = false;
    if (cls == TWC_WORLD) {
        static const float kIdent[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        if (W.tagN == 1 && memcmp(cur, kIdent, 64)) {
            DWORD mesh = W.tag[0], occ = 0;
            while (occ < 6 && twKeyFind(*W.cur, mesh, fx, h, occ)) ++occ;
            const TwKey* pk = (W.prev && W.blend) ? twKeyFind(*W.prev, mesh, fx, h, occ) : NULL;
            twKeyPut(*W.cur, mesh, fx, h, occ, cur, 64);
            if (W.blend) {
                if (pk && pk->nb == 64) {
                    const float* prev = (const float*)(W.prev->arena + pk->off);
                    if (memcmp(prev, cur, 64)) {
                        if (twMatNear(prev, cur, slot != 44, false, g_twMaxMove)) { twLerpMat(prev, cur, W.t, out); sub = true; g_twNWorld++; }
                        else g_twNPalFar++;
                    }
                } else g_twNWorldNew++;
            }
        }
    } else if (cls == TWC_VIEW || cls == TWC_PROJ || cls == TWC_VIEWI) {
        twCamPut(*W.cur, fx, h, cur);
        const float* prev = (W.blend && W.prev) ? twCamNearest(*W.prev, fx, h, cur) : NULL;
        if (prev && memcmp(prev, cur, 64)) {
            if (twMatNear(prev, cur, slot != 44, cls == TWC_PROJ, g_twMaxCamMove)) { twLerpMat(prev, cur, W.t, out); sub = true; g_twNCam++;
                if (cls == TWC_VIEW) { float d = twViewShift(prev, cur); if (d > W.camMove) W.camMove = d; } }
            else g_twNCamCut++;
        }
    } else if (W.blend && (W.subV || W.subP) && W.haveV && W.haveP) {     // a product with the camera in it: this frame's value, re-aimed
        if (W.fixDirty) { W.fixOk = twCamFixBuild(W.devV, W.devP, W.twV, W.twP, &W.fix); W.fixDirty = false; }
        if (W.fixOk) { twCamFixApply(W.fix, cls, slot == 44, cur, out); sub = true; g_twNFix++; }
    }
    if (W.blend) twUndoNote(fx, h, e, sub);                 // blended or not: the stream's last write is what stays
    if (sub && cls >= TWC_VIEW) W.camFxN++;
    if (W.dvTrack) {                                        // blended or not, it is a value the device pass knows from here on (a preshader may read it)
        DvVal* dv = dvValFor(fx, h, cls, slot, 64);
        if (dv && sub) memcpy(dv->buf, out, 64);
        dvValSet(dv, data, 64, sub);
    }
    return sub ? (const void*)out : data;
}
static void twWalkFx(TwWalk& W, RtRec* r) {               // a run of effect parameter writes
    const DWORD* e = (const DWORD*)rtPayload(r);
    void** O = (void**)r->fn;
    for (DWORD i = r->pad; i; --i) {
        DWORD w0 = e[0], slot = w0 & 0xFF, nb = (w0 >> 8) & 0x7FFFFF;
        if (slot == 78) {
            const void* data = (w0 & 0x80000000u) ? (const void*)(e + 5) : NULL;
            const void* use = data;
            if (data && twClass(e[1], e[2]) == TWC_PALETTE) {
                if (W.tagN && !e[3] && e[4] == nb) use = twPalette(W, e[1], e[2], e, data, nb);
                else { if (W.blend) twUndoNote(e[1], e[2], e, false);                       // a palette nobody tagged (a batch's default)
                       if (W.dvTrack) dvValPlain(e[1], e[2], e[3]); }
            }
            if (W.fxExec) ((RtF5)O[78])(e[1], e[2], (DWORD)(ULONG_PTR)use, e[3], e[4]);
            e = (const DWORD*)((const BYTE*)e + 20 + ((nb + 3) & ~3u));
        } else if (slot == 34 || slot == 38 || slot == 44) {
            const void* data = (w0 & 0x80000000u) ? (const void*)(e + 3) : NULL;
            const void* use = data;
            if (data && nb == 64 && slot != 34) use = twMatrix(W, e[1], e[2], slot, e, data);
            if (W.fxExec) ((RtF3)O[slot])(e[1], e[2], (DWORD)(ULONG_PTR)use);
            e = (const DWORD*)((const BYTE*)e + 12 + ((nb + 3) & ~3u));
        } else if (slot == 0xF0) {
            DWORD n = (w0 >> 8) & 0xFFFF;
            W.tagN = n; W.tag = e + 1; g_twNTag++; W.nTag += (LONG)n;
            e += 1 + n;
        } else {
            if (W.fxExec) ((RtF3)O[slot])(e[1], e[2], e[3]);
            e += 4;
        }
    }
}
// From this frame's camera space to the blended camera's: V^-1 V' (row vectors). false: V cannot be inverted.
static bool twCamSpaceView(const float* V, const float* V2, float* out) {
    double v[16], v2[16], iv[16], r[16];
    for (int i = 0; i < 16; ++i) { v[i] = V[i]; v2[i] = V2[i]; }
    if (!twInv4d(v, iv)) return false;
    twMul4d(iv, v2, r);
    for (int i = 0; i < 16; ++i) out[i] = (float)r[i];
    return true;
}
static __forceinline bool twIsPerspective(const float* p) { return p[15] == 0.0f && (p[11] == 1.0f || p[11] == -1.0f); }
// Flat geometry already placed on screen for this frame's camera (health bars, icons) belongs to things that
// stand on the ground: under the blended camera it is where the ground plane at height h maps to. Plane point
// (X, Y, 1) -> world (X, Y, h, 1) -> clip x, y, w is a 3x3 matrix per camera; M^-1 M' takes this frame's screen
// position to the blended camera's. Written as a projection matrix (row vectors): x, y and w mix, z stays.
static bool twInv3d(const double* m, double* o) {
    double c0 = m[4]*m[8] - m[5]*m[7], c1 = m[5]*m[6] - m[3]*m[8], c2 = m[3]*m[7] - m[4]*m[6];
    double det = m[0]*c0 + m[1]*c1 + m[2]*c2;
    if (det > -1e-30 && det < 1e-30) return false;
    det = 1.0 / det;
    o[0] = c0 * det; o[1] = (m[2]*m[7] - m[1]*m[8]) * det; o[2] = (m[1]*m[5] - m[2]*m[4]) * det;
    o[3] = c1 * det; o[4] = (m[0]*m[8] - m[2]*m[6]) * det; o[5] = (m[2]*m[3] - m[0]*m[5]) * det;
    o[6] = c2 * det; o[7] = (m[1]*m[6] - m[0]*m[7]) * det; o[8] = (m[0]*m[4] - m[1]*m[3]) * det;
    return true;
}
static bool twGroundFix(const float* V, const float* P, const float* V2, const float* P2, float h, float* out) {
    double v[16], p[16], v2[16], p2[16], vp[16], vp2[16], M[9], M2[9], iM[9], H[9];
    for (int i = 0; i < 16; ++i) { v[i] = V[i]; p[i] = P[i]; v2[i] = V2[i]; p2[i] = P2[i]; }
    twMul4d(v, p, vp); twMul4d(v2, p2, vp2);
    for (int c = 0; c < 3; ++c) {
        int col = c == 2 ? 3 : c;
        M[c] = vp[col]; M[3 + c] = vp[4 + col]; M[6 + c] = (double)h * vp[8 + col] + vp[12 + col];
        M2[c] = vp2[col]; M2[3 + c] = vp2[4 + col]; M2[6 + c] = (double)h * vp2[8 + col] + vp2[12 + col];
    }
    if (!twInv3d(M, iM)) return false;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) H[r*3+c] = iM[r*3]*M2[c] + iM[r*3+1]*M2[3+c] + iM[r*3+2]*M2[6+c];
    if (H[8] > -1e-12 && H[8] < 1e-12) return false;
    double s = 1.0 / H[8];
    float t[16] = { (float)(H[0]*s), (float)(H[1]*s), 0, (float)(H[2]*s),
                    (float)(H[3]*s), (float)(H[4]*s), 0, (float)(H[5]*s),
                    0, 0, 1, 0,
                    (float)(H[6]*s), (float)(H[7]*s), 0, 1.0f };
    for (int i = 0; i < 16; ++i) if (!(t[i] > -1e6f && t[i] < 1e6f)) return false;
    memcpy(out, t, 64);
    return true;
}
static __forceinline bool twIsAffine2D(const float* p) { return p[3] == 0.0f && p[7] == 0.0f && p[11] == 0.0f && p[15] == 1.0f; }
static const float kTwIdent[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
static __forceinline float twGroundHeight(const TwWalk& W) { return (W.nZ > 0 ? W.sumZ / (float)W.nZ : g_twGroundZ) + g_twBarLift; }
// What the device's VIEW and PROJECTION are in this pass, given what the stream has set:
//   a camera                                   -> its blend with last frame's
//   identity VIEW under a perspective          -> camera-space geometry: V^-1 V'
//   identity VIEW, flat PROJECTION, in the 3D pass -> world-pinned flat geometry: the ground mapping, as the projection
//   anything else                              -> as the stream has it
static void twDevEffective(TwWalk& W, float* eV, float* eP, int* kind) {
    bool vI = !memcmp(W.devV, kTwIdent, 64);
    *kind = 0;
    if (!vI) memcpy(eV, W.blVok ? W.blV : W.devV, 64);
    else if (W.blend && W.camSub && W.haveP && twIsPerspective(W.devP) && twCamSpaceView(W.camV, W.camTw, eV)) *kind = 1;
    else memcpy(eV, kTwIdent, 64);
    memcpy(eP, W.blPok ? W.blP : W.devP, 64);
    if (vI && W.haveV && W.haveP && W.blend && W.in3D && g_twGroundOk && W.camSub && W.camPHave && twIsAffine2D(W.devP)) {
        float g[16];
        if (twGroundFix(W.camV, W.camP, W.camTw, W.camPTw, twGroundHeight(W), g)) {
            if (!memcmp(W.devP, kTwIdent, 64)) memcpy(eP, g, 64);
            else { double a[16], b[16], r[16]; for (int i = 0; i < 16; ++i) { a[i] = W.devP[i]; b[i] = g[i]; } twMul4d(a, b, r); for (int i = 0; i < 16; ++i) eP[i] = (float)r[i]; }
            *kind = 2;
        }
    }
}
static void twDevApply(TwWalk& W, DWORD setType) {         // setType: the transform the stream is setting right now (0: none)
    float eV[16], eP[16]; int kind = 0;
    twDevEffective(W, eV, eP, &kind);
    bool chV = W.haveV && memcmp(eV, W.twV, 64) != 0, chP = W.haveP && memcmp(eP, W.twP, 64) != 0;
    if (W.haveV) memcpy(W.twV, eV, 64);
    if (W.haveP) memcpy(W.twP, eP, 64);
    W.subV = W.haveV && memcmp(W.twV, W.devV, 64) != 0; W.subP = W.haveP && memcmp(W.twP, W.devP, 64) != 0;
    W.fixDirty = true;
    if (kind == 1 && chV) g_twNCamSpace++; else if (kind == 2 && chP) g_twNGround++;
    if (!W.exec) return;
    if (W.haveV && (setType == 2 || chV)) ((TwF3)g_rtO_dev[44])(W.dev, 2, (DWORD)(ULONG_PTR)W.twV);
    if (W.haveP && (setType == 3 || chP)) ((TwF3)g_rtO_dev[44])(W.dev, 3, (DWORD)(ULONG_PTR)W.twP);
}
// (v60) The run of draws under one camera is over: was it the longest so far?
static void twCamRunClose(TwWalk& W) {
    if (W.epHave && W.epPHave && W.epDraws > W.bestDraws) {
        W.bestDraws = W.epDraws;
        memcpy(W.endV, W.epV, 64); memcpy(W.endP, W.epP, 64); memcpy(W.endTwV, W.epTwV, 64); memcpy(W.endTwP, W.epTwP, 64);
        W.endOk = true;
    }
}
// The stream sets the device's VIEW (2) or PROJECTION (3).
static void twDevSet(TwWalk& W, DWORD type, const float* m) {
    bool view = type == 2;
    memcpy(view ? W.devV : W.devP, m, 64);
    (view ? W.haveV : W.haveP) = true;
    bool& blOk = view ? W.blVok : W.blPok; float* bl = view ? W.blV : W.blP;
    blOk = false;
    if (memcmp(m, kTwIdent, 64)) {
        twCamPut(*W.cur, 0, type, m);
        if (W.blend) {
            const float* prev = W.prev ? twCamNearest(*W.prev, 0, type, m) : NULL;
            if (prev && memcmp(prev, m, 64)) {
                if (twMatNear(prev, m, true, !view, g_twMaxCamMove)) { twLerpMat(prev, m, W.t, bl); blOk = true; g_twNDevCam++;
                    if (view) { float d = twViewShift(prev, m); if (d > W.camMove) W.camMove = d; } }
                else g_twNCamCut++;
            }
        }
        if (view) { memcpy(W.camV, m, 64); memcpy(W.camTw, blOk ? bl : m, 64); W.camSub = blOk; W.camVHave = true;
            if (W.in3D && (!W.epHave || memcmp(W.epV, m, 64))) {            // another camera: the run of the one before ends here
                twCamRunClose(W);
                memcpy(W.epV, m, 64); memcpy(W.epTwV, W.camTw, 64); W.epHave = true; W.epDraws = 0;
                W.epPHave = W.camPHave; if (W.camPHave) { memcpy(W.epP, W.camP, 64); memcpy(W.epTwP, W.camPTw, 64); }
            } }
        else if (twIsPerspective(m)) { memcpy(W.camP, m, 64); memcpy(W.camPTw, blOk ? bl : m, 64); W.camPHave = true;
            if (W.in3D && W.epHave && !W.epPHave) { memcpy(W.epP, W.camP, 64); memcpy(W.epTwP, W.camPTw, 64); W.epPHave = true; } }      // (set after its VIEW: it is that camera's)
    }
    twDevApply(W, type);
}
static void twExecAgain(RtRec* r) {                       // one record, without anything that may only happen once
    DWORD* a = rtArgs(r);
    switch (r->kind) {
    case 1: break;
    case 3: {                                             // staged buffer write: the same bytes into the same range
        void* mapped = NULL;
        if ((LONG)((DWORD(__stdcall*)(DWORD, DWORD, DWORD, void**, DWORD))r->fn)(a[0], a[1], a[2], &mapped, a[3]) >= 0 && mapped) memcpy(mapped, (const void*)(ULONG_PTR)a[5], a[2]);
        ((TwF1)(ULONG_PTR)a[4])(a[0]);
        return; }
    case 6: if (!a[2]) ((RtF2)r->fn)(a[0], r->pad); return;   // ApplyParameterBlock: the handle its first execution resolved
    default: return;                                      // 4 / 7 / 9 texture contents, 5 EndParameterBlock, 8 (twWalkFx), 10 marks
    }
    switch (r->nargs) {
    case 1:  ((RtF1)r->fn)(a[0]); break;
    case 2:  ((RtF2)r->fn)(a[0], a[1]); break;
    case 3:  ((RtF3)r->fn)(a[0], a[1], a[2]); break;
    case 4:  ((RtF4)r->fn)(a[0], a[1], a[2], a[3]); break;
    case 5:  ((RtF5)r->fn)(a[0], a[1], a[2], a[3], a[4]); break;
    case 6:  ((RtF6)r->fn)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
    case 7:  ((RtF7)r->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
    case 8:  ((RtF8)r->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
    case 9:  ((RtF9)r->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]); break;
    case 10: ((RtF10)r->fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;
    }
}
static void twPutBack() {
    TwW& w = g_tw; TwWalk& W = g_twWalk;
    if (!W.blend) return;
    if (W.fxExec) for (int i = 0; i < TW_UNDO_N; ++i) {
        TwUndo& u = w.undo[i];
        if (u.stamp != w.undoStamp || !u.sub || !u.e) continue;
        const DWORD* e = u.e; DWORD w0 = e[0], slot = w0 & 0xFF;
        void** O = (void**)g_rtO_fx;
        u.sub = 0;
        if (slot == 78) ((RtF5)O[78])(e[1], e[2], (DWORD)(ULONG_PTR)(e + 5), e[3], e[4]);
        else ((RtF3)O[slot])(e[1], e[2], (DWORD)(ULONG_PTR)(e + 3));
    }
    if (W.subV && W.haveV) { W.subV = false; ((TwF3)g_rtO_dev[44])(W.dev, 2, (DWORD)(ULONG_PTR)W.devV); }
    if (W.subP && W.haveP) { W.subP = false; ((TwF3)g_rtO_dev[44])(W.dev, 3, (DWORD)(ULONG_PTR)W.devP); }
}
// Render thread, at the frame's Present record. exec: put the device back to the mark and run the stretch again,
// into the real back buffer; blend: with what moves set between the previous frame and this one. Without exec the
// stretch is only read, so the next frame has this one to blend from.
#ifndef AOTR_PROD
static volatile LONG64 g_twSplit[5];          // second passes: cycles in parameter runs, effect calls, the stream's device calls, everything (and the passes counted)
#define TW_SPLIT(i, code) { unsigned __int64 c0_ = __rdtsc(); code; g_twSplit[i] += (LONG64)(__rdtsc() - c0_); }
#else
#define TW_SPLIT(i, code) { code; }
#endif
static void twWalkStretch(RtRec* end, int exec, int blend, float t) {
    TwW& w = g_tw;
    TwWalk& W = g_twWalk;
    DWORD dev = w.resDev;
    LONG64 t0 = qpcNow();
#ifndef AOTR_PROD
    unsigned __int64 c00 = __rdtsc();
#endif
    w.keepStandIn = w.standIn;
    int dv = exec ? dvPassMode() : 0;                      // how this pass is made: by D3DX (0), at the device (1), by D3DX with its device calls compared (2)
    for (;;) {                                             // (a pass at the device that has to stop is made again from the mark, by D3DX)
    bool stopped = false;
    W.exec = exec; W.blend = (exec && blend) ? 1 : 0; W.t = t; W.dev = dev;
    W.dv = dv; W.fxExec = (exec && dv != 1) ? 1 : 0; W.dvTrack = (dv && W.blend) ? 1 : 0; W.camFxN = 0;
    W.cur = &w.gen[w.cur]; W.prev = w.gen[1 - w.cur].valid ? &w.gen[1 - w.cur] : NULL;
    W.tagN = 0; W.tag = NULL; W.haveV = W.haveP = W.subV = W.subP = W.fixOk = false; W.fixDirty = true; W.camSub = false; W.camPHave = false; W.blVok = W.blPok = false; W.in3D = true; W.sawEnd = false; W.sumZ = 0; W.nZ = 0; W.nTag = 0; W.camMove = 0;
    W.endOk = false; W.camVHave = false; W.epHave = false; W.epPHave = false; W.epDraws = 0; W.bestDraws = 0;
    twGenBegin(*W.cur);
    if (++w.undoStamp == 0) { memset(w.undo, 0, TW_UNDO_N * sizeof(TwUndo)); w.undoStamp = 1; }
    if (dv) dvPassBegin(dv);
    if (exec) {
        g_rtDg.pass = 1;
        w.replaying = 1; w.standIn = w.ibSurf;             // in this pass "the back buffer" is the in-between frame's own target
        for (int i = 0; i < 4; ++i) {
            if (!w.markRTOk[i]) continue;
            DWORD s = w.markRT[i];
            if (i == 0 && twIsOff(s)) s = w.ibSurf;
            if (i == 0 && !s) continue;
            ((TwF3)g_twO_SetRT)(dev, (DWORD)i, s);
        }
        if (w.markDSOk) ((TwF2)g_rtO_dev[39])(dev, w.markDS);
        ((TwF1)g_rtO_sb[5])(w.sb);                         // Apply: viewport and scissor included, after the targets
        if (w.markInScene && !w.inScene) tw_BeginScene(dev); else if (!w.markInScene && w.inScene) tw_EndScene(dev);
    }
    // what the device holds at the mark is whatever the previous frame left there - not this frame's camera, so it
    // is neither kept for the next frame nor blended; it only says what a product would be re-aimed from
    memcpy(W.devV, kTwIdent, 64); memcpy(W.devP, kTwIdent, 64); memcpy(W.twV, kTwIdent, 64); memcpy(W.twP, kTwIdent, 64);
    if (w.markVOk) { memcpy(W.devV, w.markV, 64); memcpy(W.twV, w.markV, 64); W.haveV = true; }
    if (w.markPOk) { memcpy(W.devP, w.markP, 64); memcpy(W.twP, w.markP, 64); W.haveP = true; }
    bool inBlock = false;
    LONG n = 0, draws = 0; DWORD walked = 0;
    for (BYTE* p = (BYTE*)w.markRec; ; ) {
        RtRec* r = (RtRec*)p;
        if (r == end) break;
        if (r->kind == 2) { p = g_rtBuf; continue; }
        if (!r->size || (walked += r->size) > RT_BUF_SIZE / 2) { g_twNLost++; break; }      // cannot happen while the ring space is held
        p += r->size;
        if (exec && (++n & 127) == 0) {
            g_rtWH.beat++;                                 // (alive: the whole pass is one record to the queue)
            if (g_rtLK.want) {                             // a direct call that needs no queue (it creates or locks something): let it in
                LeaveCriticalSection(&g_rtExecCs);
                for (int k = 0; g_rtLK.want && k < 65536; ++k) _mm_pause();
                EnterCriticalSection(&g_rtExecCs);
            }
            if (w.qn > 0) twShowDue();                     // a waiting picture's time has come while this one is being drawn
            if (pwGrid()) pwFillerTick();                  // ... or, while the camera scrolls, the screen's next refresh has
        }
        if (exec && n == 16 && (g_twTest & 64) && w.qn > 0) twShowHead(0);      // test: one goes up in the middle of this pass whatever the time
        if (exec && g_rtDg.on) { g_rtDg.op = r->op; g_rtDg.kind = (BYTE)r->kind; }
        if (inBlock) { if (r->kind == 5) inBlock = false; continue; }      // a parameter block being recorded: it exists already
        if (r->kind == 8) { TW_SPLIT(0, twWalkFx(W, r)); if (dv == 1 && g_dvp.abort) { stopped = true; break; } continue; }
        if (r->kind == TW_KIND) {                          // the 3D pass ends here: flat geometry from now on is the interface
            if (rtArgs(r)[0] == 2 && W.in3D) {
                twCamRunClose(W);                          // the camera most of the pass was drawn with is the frame's
                W.in3D = false; W.sawEnd = true;
                if (exec) pwSceneCopy(w.ibSurf);           // the in-between picture's scene, before its interface
                twDevApply(W, 0);
            }
            continue;
        }
        if (r->kind == 1) {
            WORD op = r->op;
            if (op == 193) { inBlock = true; continue; }
            if (g_twOpClass[op & 511]) continue;
            if (r->fn == (void*)&mgJobExec || r->fn == (void*)&rtAsyncFilterRun) continue;
            if (op == 44 && r->nargs == 3) {               // SetTransform
                DWORD* a = rtArgs(r);
                if ((a[1] == 2 || a[1] == 3) && a[2]) { twDevSet(W, a[1], (const float*)(ULONG_PTR)a[2]); continue; }
            } else if (op >= 81 && op <= 84) {             // a draw: the tag before it has been used
                W.tagN = 0;
                if (W.in3D) W.epDraws++;
                if (exec && (g_twTest & 2) && op == 82 && (draws++ & 3) == 0) continue;     // control: a picture with draws missing must not pass
                if (dv == 2) dvVerifyDraw();               // what this draw reads of the blended parameters: D3DX's registers against the recording's
            } else if (dv && (WORD)(op - 120) < 80) {      // an effect call
                if (W.dvTrack && ((op >= 140 && op < 178) || op == 198) && r->nargs >= 2) dvValPlain(rtArgs(r)[0], rtArgs(r)[1], 0);     // a parameter written outside a run: the stream's own value from here on
                bool ok_ = true;
                if (dv == 1) { TW_SPLIT(1, ok_ = dvRun(r, dev, false)); if (!ok_) { stopped = true; break; } }      // the device calls D3DX made for it the first time, with this pass's values in their registers
                else dvVerifyCall(r, dev);
                continue;
            } else if (dv && (op == 92 || op == 107) && r->nargs == 2) dvShaderSet(op, rtArgs(r)[1]);
        } else if (r->kind == 6 && dv == 1) continue;      // a parameter block: D3DX is asked nothing in this pass
        if (exec) { if (r->kind == 6 || (r->kind == 1 && (WORD)(r->op - 120) < 80)) { TW_SPLIT(1, twExecAgain(r)); } else { TW_SPLIT(2, twExecAgain(r)); } }
    }
    if (!stopped) break;
    dvPassStopped(); dv = 0;
    }
    if (exec) {
        twPutBack();                                       // every blended value goes back to what the stream itself set last
        w.standIn = w.keepStandIn; w.replaying = 0; g_rtDg.pass = 0;
        LONG64 dt = qpcNow() - t0;
        g_twReplayTicks += dt; g_twNReplay++;
        w.replayCost = w.replayCost ? (w.replayCost * 3 + dt) / 4 : dt;
        dvPassDone(dv, W.camFxN > 0);
#ifndef AOTR_PROD
        g_twSplit[4]++; g_twSplit[3] += (LONG64)(__rdtsc() - c00);
#endif
    } else g_twNCollect++;
    if (W.nZ > 0) g_twGroundZ = W.sumZ / (float)W.nZ;
    w.lastTags = W.nTag;
    g_twGroundOk = W.sawEnd ? 1 : 0;                       // without the end-of-3D mark the interface would be taken for world-pinned geometry: then nothing flat is moved
    W.cur->valid = 1;
    w.cur = 1 - w.cur; w.prevAge = 1;                      // this frame is now "the frame before"
}

// The second pass executes records a second time; should one of them fault (something it names is gone after all),
// the game must not go down with it: the fault is logged, what was blended is put back as far as it can be, and
// in-between frames are switched off for the session. The game's own frame is untouched either way.
static DWORD g_twFaultCode = 0, g_twFaultAddr = 0;
static int twFaultFilter(EXCEPTION_POINTERS* ep) {
    g_twFaultCode = ep->ExceptionRecord->ExceptionCode; g_twFaultAddr = (DWORD)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
    return (g_twFaultCode & 0xC0000000u) == 0xC0000000u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}
static void twPutBackGuarded() { __try { twPutBack(); } __except (EXCEPTION_EXECUTE_HANDLER) { } }
static bool twWalkGuarded(RtRec* end, int exec, int blend, float t) {
    for (int attempt = 0; ; ++attempt) {
        __try { twWalkStretch(end, exec, blend, t); return true; }
        __except (twFaultFilter(GetExceptionInformation())) {
            TwW& w = g_tw;
            int dv = g_twWalk.dv;
            dvFaulted();
            if (exec) { twPutBackGuarded(); w.standIn = w.keepStandIn; w.replaying = 0; g_rtDg.pass = 0; }
            if (dv && !attempt) {                           // it happened in a pass made at the device (or compared with one): that is switched off, and the picture is made again the old way
                InterlockedExchange(&g_dvOn, 0); g_dvs.off = 1;
                logf("devpass: a second pass made at the device faulted (0x%08X at %08X) - that is OFF for this session; the picture is made again through D3DX, in-between frames stay on.", g_twFaultCode, g_twFaultAddr);
                continue;
            }
            InterlockedIncrement(&g_twFaults);
            w.gen[0].valid = 0; w.gen[1].valid = 0; w.prevAge = 0;
            InterlockedExchange(&g_twOn, 0);
            logf("tween: the second pass faulted (0x%08X at %08X, %s) - in-between frames are OFF for this session; the game's own frames are not affected.",
                 g_twFaultCode, g_twFaultAddr, exec ? (blend ? "blending" : "replaying") : "reading");
            return false;
        }
    }
}

// ---------------------------------------------------------------- self-check: is the second pass the same picture?
static bool twReadBack(DWORD surf, int which, INT* pitch, const BYTE** bits) {
    TwW& w = g_tw;
    DWORD dev = w.resDev, src = surf;
    typedef DWORD (__stdcall* tCRT)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
    typedef DWORD (__stdcall* tCOPS)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
    if (!w.chkSys[which]) { void* s = NULL; ((tCOPS)g_rtO_dev[36])(dev, w.w, w.h, w.fmt, 2, (DWORD)(ULONG_PTR)&s, 0); w.chkSys[which] = (DWORD)(ULONG_PTR)s; }
    if (!w.chkSys[which]) return false;
    if (w.ms) {                                            // a multisampled target is resolved first
        if (!w.chkRT) { void* s = NULL; ((tCRT)g_rtO_dev[28])(dev, w.w, w.h, w.fmt, 0, 0, 0, (DWORD)(ULONG_PTR)&s, 0); w.chkRT = (DWORD)(ULONG_PTR)s; }
        if (!w.chkRT || (LONG)((TwF6)g_twO_Stretch)(dev, surf, 0, w.chkRT, 0, 0) < 0) return false;
        src = w.chkRT;
    }
    if ((LONG)((TwF3)g_twO_GetRTData)(dev, src, w.chkSys[which]) < 0) return false;
    struct { INT pitch; void* bits; } lr = { 0, NULL };
    void* lk = rtSurfOrig(w.chkSys[which], 13);
    if (!lk || (LONG)((TwF4)lk)(w.chkSys[which], (DWORD)(ULONG_PTR)&lr, 0, 0x10) < 0 || !lr.bits) return false;
    *pitch = lr.pitch; *bits = (const BYTE*)lr.bits;
    return true;
}
static void twWriteBmp(const char* name, const BYTE* bits, INT pitch, DWORD w, DWORD h, DWORD bpp, DWORD step) {
    char path[MAX_PATH]; aotrPath(path, name);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD ow = w / step, oh = h / step, row = (ow * 3 + 3) & ~3u, wr = 0;
    BITMAPFILEHEADER fh; ZeroMemory(&fh, sizeof(fh)); fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER); fh.bfSize = fh.bfOffBits + row * oh;
    BITMAPINFOHEADER ih; ZeroMemory(&ih, sizeof(ih)); ih.biSize = sizeof(ih); ih.biWidth = (LONG)ow; ih.biHeight = -(LONG)oh; ih.biPlanes = 1; ih.biBitCount = 24;
    WriteFile(f, &fh, sizeof(fh), &wr, NULL); WriteFile(f, &ih, sizeof(ih), &wr, NULL);
    BYTE* line = (BYTE*)VirtualAlloc(NULL, row, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (line) {
        for (DWORD y = 0; y < oh; ++y) {
            const BYTE* s = bits + (INT)(y * step) * pitch;
            for (DWORD x = 0; x < ow; ++x) { const BYTE* q = s + x * step * bpp; line[x*3] = q[0]; line[x*3+1] = q[1]; line[x*3+2] = q[2]; }
            WriteFile(f, line, row, &wr, NULL);
        }
        VirtualFree(line, 0, MEM_RELEASE);
    }
    CloseHandle(f);
}
static void twCompare(DWORD frame) {                       // the game's frame against the second pass (each in its off-screen target)
    TwW& w = g_tw;
    INT pa = 0, pb = 0; const BYTE* a = NULL; const BYTE* b = NULL;
    DWORD bpp = rtFmtBytes(w.fmt);
    bool ra = bpp && twReadBack(w.standIn, 0, &pa, &a), rb = ra && twReadBack(w.ibSurf, 1, &pb, &b);
    if (ra && rb) {
        DWORD diff = 0, fx = 0, fy = 0;
        for (DWORD y = 0; y < w.h; ++y) {
            const BYTE* x0 = a + (INT)y * pa; const BYTE* x1 = b + (INT)y * pb;
            if (!memcmp(x0, x1, w.w * bpp)) continue;
            for (DWORD x = 0; x < w.w; ++x) if (memcmp(x0 + x * bpp, x1 + x * bpp, bpp == 4 && w.fmt == 22 ? 3 : bpp)) { if (!diff) { fx = x; fy = y; } ++diff; }
        }
        if (diff) {
            if (InterlockedIncrement(&g_twNDiff) == 1) {
                g_twFirstDiff[0] = frame; g_twFirstDiff[1] = fx; g_twFirstDiff[2] = fy; g_twFirstDiff[3] = diff;
                if (!g_twTest || (g_twTest & 32)) {            // in the game: both pictures, whole, to look at
                    twWriteBmp("tween_check_game.bmp", a, pa, w.w, w.h, bpp, 1); twWriteBmp("tween_check_second_pass.bmp", b, pb, w.w, w.h, bpp, 1);
                    logf("tween: self-check - a second pass differs from the game's frame in %u pixels, the first at (%u,%u); both pictures are next to the DLL (tween_check_*.bmp).", diff, fx, fy);
                }
            }
        } else InterlockedIncrement(&g_twNSame);
    } else InterlockedIncrement(&g_twNCheckFail);
    if (ra) twSurfCall1(w.chkSys[0], 14);
    if (rb) twSurfCall1(w.chkSys[1], 14);
}
static void twGrab() {                                     // test: keep the in-between picture for the test to fetch
    TwW& w = g_tw;
    INT pitch = 0; const BYTE* bits = NULL;
    DWORD bpp = rtFmtBytes(w.fmt), bytes = w.w * w.h * bpp;
    if (!bpp || !twReadBack(w.ibSurf, 1, &pitch, &bits)) return;
    if (g_twGrabBytes < bytes) { if (g_twGrabBuf) VirtualFree(g_twGrabBuf, 0, MEM_RELEASE); g_twGrabBuf = (BYTE*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); g_twGrabBytes = g_twGrabBuf ? bytes : 0; }
    if (g_twGrabBuf) { for (DWORD y = 0; y < w.h; ++y) memcpy(g_twGrabBuf + y * w.w * bpp, bits + (INT)y * pitch, w.w * bpp); InterlockedIncrement(&g_twGrabSeq); }
    twSurfCall1(w.chkSys[1], 14);
}

// A part of a picture with every pixel in it (the half-size picture of a 4K frame loses what one looks for in a
// shadow's edge): the middle 1920x1080 of the frame.
static void twWriteBmpMiddle(const char* name, const BYTE* bits, INT pitch, DWORD w, DWORD h, DWORD bpp) {
    DWORD cw = w < 1920 ? w : 1920, ch = h < 1080 ? h : 1080, x0 = (w - cw) / 2, y0 = (h - ch) / 2, row = (cw * 3 + 3) & ~3u, wr = 0;
    char path[MAX_PATH]; aotrPath(path, name);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    BITMAPFILEHEADER fh; ZeroMemory(&fh, sizeof(fh)); fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER); fh.bfSize = fh.bfOffBits + row * ch;
    BITMAPINFOHEADER ih; ZeroMemory(&ih, sizeof(ih)); ih.biSize = sizeof(ih); ih.biWidth = (LONG)cw; ih.biHeight = -(LONG)ch; ih.biPlanes = 1; ih.biBitCount = 24;
    WriteFile(f, &fh, sizeof(fh), &wr, NULL); WriteFile(f, &ih, sizeof(ih), &wr, NULL);
    BYTE* buf = (BYTE*)VirtualAlloc(NULL, row * ch, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (buf) {
        for (DWORD y = 0; y < ch; ++y) { const BYTE* s = bits + (INT)(y0 + y) * pitch + x0 * bpp; BYTE* o = buf + y * row;
            for (DWORD x = 0; x < cw; ++x) { o[x * 3] = s[x * bpp]; o[x * 3 + 1] = s[x * bpp + 1]; o[x * 3 + 2] = s[x * bpp + 2]; } }
        WriteFile(f, buf, row * ch, &wr, NULL);
        VirtualFree(buf, 0, MEM_RELEASE);
    }
    CloseHandle(f);
}
static LONG g_twDumpAny = 0;                               // test: any frame will do (not only a battle's), and the middle is written for a small frame too
static LONG g_twDumpOnce = 0;                              // armed from the ini: once, and not again until the ini is written anew (picture_dump_done.txt says it was done)
static volatile LONG g_pwDumpLeft = 0;                     // ... and this many pictures shown from another camera (aotr_panwarp.inc), with the picture as it was drawn
static void twDumpDone() {
    char path[MAX_PATH]; aotrPath(path, "picture_dump_done.txt");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) { static const char t[] = "The pictures asked for by InBetweenDump were written. They are written again only after bfme2_accel.ini is saved anew.\r\n"; DWORD wr = 0; WriteFile(f, t, sizeof(t) - 1, &wr, NULL); CloseHandle(f); }
}
static void twDump() {                                     // diagnostic: the in-between picture and the game's frame (each in its off-screen target), as files
    TwW& w = g_tw;
    INT pa = 0, pb = 0; const BYTE* a = NULL; const BYTE* b = NULL;
    DWORD bpp = rtFmtBytes(w.fmt), step = w.w > 2000 ? 2 : 1;                 // a 4K frame is written at half size
    if (bpp < 3) return;
    char name[64];
    if (twReadBack(w.ibSurf, 1, &pb, &b)) { wsprintfA(name, "tween_dump_%02d_a_between.bmp", (int)g_twDumpN); twWriteBmp(name, b, pb, w.w, w.h, bpp, step);
        if (step > 1 || g_twDumpAny) { wsprintfA(name, "tween_dump_%02d_a_between_middle.bmp", (int)g_twDumpN); twWriteBmpMiddle(name, b, pb, w.w, w.h, bpp); }
        twSurfCall1(w.chkSys[1], 14); }
    if (twReadBack(w.standIn, 0, &pa, &a)) { wsprintfA(name, "tween_dump_%02d_b_game.bmp", (int)g_twDumpN); twWriteBmp(name, a, pa, w.w, w.h, bpp, step);
        if (step > 1 || g_twDumpAny) { wsprintfA(name, "tween_dump_%02d_b_game_middle.bmp", (int)g_twDumpN); twWriteBmpMiddle(name, a, pa, w.w, w.h, bpp); }
        twSurfCall1(w.chkSys[0], 14); }
    g_twDumpN++;
}

#include "aotr_panwarp.inc"

// ---------------------------------------------------------------- Present
// Every Present there is ends up here, on whichever thread executes it.
static void twNextTarget(DWORD dev) {                     // what stands for the back buffer in the next frame
    TwW& w = g_tw;
    DWORD next = 0;
    if (w.wantOff && twEnsure(dev)) next = twFreeSurf();
    if (w.resDev == dev && w.bb) {
        void* cur = NULL;
        if ((LONG)((TwF3)g_twO_GetRT)(dev, 0, (DWORD)(ULONG_PTR)&cur) >= 0 && cur) {
            DWORD c = (DWORD)(ULONG_PTR)cur, want = next ? next : w.bb;
            twSurfCall1(c, 2);
            if ((c == w.bb || twIsOff(c)) && c != want) {       // the stream believes the back buffer is bound: keep that true
                BYTE vp[24]; LONG sc[4];
                bool haveVp = (LONG)((TwF2)g_rtO_dev[48])(dev, (DWORD)(ULONG_PTR)vp) >= 0, haveSc = (LONG)((TwF2)g_rtO_dev[76])(dev, (DWORD)(ULONG_PTR)sc) >= 0;
                ((TwF3)g_twO_SetRT)(dev, 0, want);
                if (haveVp) ((TwF2)g_rtO_dev[47])(dev, (DWORD)(ULONG_PTR)vp);
                if (haveSc) ((TwF2)g_rtO_dev[75])(dev, (DWORD)(ULONG_PTR)sc);
            }
        }
    }
    w.standIn = next;
}
// Whether in-between frames can be made at all. Switched on means on: for every frame, at any frame rate.
static void twPace(bool worker) {
    TwW& w = g_tw;
    bool can = g_twOn && g_rtActive && worker && !g_rtObjRefs && !w.resFail;
    LONG test = g_twTest;
    if (test) { w.active = (test & (16 | 32)) ? 1 : 0; w.wantOff = (can && (test & (4 | 16 | 32))) ? 1 : 0; InterlockedExchange(&g_twWantMark, can ? 1 : 0); return; }
    if (can != (w.active != 0)) {
        if (can) logf("tween: running - every frame the game draws gets an in-between frame ahead of it.");
        else if (g_twOn) logf("tween: cannot run right now (%s).", !g_rtActive ? "the render thread is switched off" : g_rtObjRefs ? "this Direct3D does not let every object be released through the queue" :
                              w.resFail ? "no off-screen frames could be made" : "this Present did not come through the render thread");
    }
    w.active = can ? 1 : 0;
    w.wantOff = w.active;
    InterlockedExchange(&g_twWantMark, w.active);
    InterlockedExchange(&g_panWarpActive, (g_pwOn && w.active && g_pw.ready && !g_pwTest && g_pwBadRun < 5 && !g_pwBroken) ? 1 : 0);
}
static void twGenMiss() {                                  // this frame's values could not be read: what is kept is one frame older now
    TwW& w = g_tw;
    if (w.gen[1 - w.cur].valid && w.prevAge == 1) w.prevAge = 2;
    else { w.gen[0].valid = 0; w.gen[1].valid = 0; w.prevAge = 0; }
}
static DWORD twPresentCore(const TwPresentArgs& pa, DWORD dev) {
    TwW& w = g_tw;
    bool worker = __readfsdword(0x24) == g_rtWorkerTid;
    if (!g_twOn && !g_twEver && !w.standIn && w.qn <= 0) return twRealPresent(pa);  // never engaged: nothing here exists
    if (!worker) EnterCriticalSection(&g_rtExecCs);        // a direct Present (render thread off, or a dirty region): the idle worker must not show a frame meanwhile
    RtRec* rec = worker ? g_twPresentRec : NULL;
    g_twPresentRec = NULL;
    LONG64 now = qpcNow();
    if (!w.pace.f) twpReset(w.pace, g_pqpf.QuadPart);
    if (rec) twpArrive(w.pace, now);                       // this frame's own duration, and the usual one
    else g_twNDirectPresent++;
    DWORD S = w.standIn, hr = w.lastHr;
    LONG test = g_twTest;
    bool walkable = rec && (rec->flags & 2) && g_twStretch && !w.cancel && w.markRec && g_twHold && twGenAlloc();
    if (rec && g_twStretch && !(rec->flags & 2)) g_twNNoVerdict++;
    bool battle = w.lastTags >= 300;                        // the first-run diagnostics wait for a frame worth looking at
    if (walkable && S && battle) w.walkN++;
    bool plain = walkable && S && ((test & 1) || (g_twCheckLeft > 0 && battle && w.walkN > 150 && (w.walkN % 60) == 0));    // run again unchanged and compare
    LONG age = w.prevAge;                                   // what is kept from before: the frame before this one (1), or the one before that when the last frame could not be read (2)
    bool blend = walkable && S && !plain && w.active && w.gen[1 - w.cur].valid && (age == 1 || age == 2);
    if (walkable && S && !plain && w.active && !blend) g_twNNoPrev++;
    DWORD I = (plain || blend) ? twFreeSurf() : 0;         // where the second pass draws
    if (!I) { plain = false; blend = false; }
    bool did = false, walked = false; LONG pan = 0;
    w.ibSurf = I;
    if (plain && twWalkGuarded(rec, 1, 0, 1.0f)) {
        did = true; walked = true;
        twCompare((DWORD)g_twNReplay);
        if (g_twCheckLeft > 0 && --g_twCheckLeft == 0) logf("tween: self-check done - %d second passes identical to the game's frame, %d different, %d could not be read.", (int)g_twNSame, (int)g_twNDiff, (int)g_twNCheckFail);
    } else if (blend && twWalkGuarded(rec, 1, 1, 1.0f - (1.0f - g_twT) / (float)age)) {     // halfway to this frame from the one before it; three quarters of the way when what is kept is two frames old
        did = true; walked = true;
        if (test & 16) twGrab();
        if (age == 2) g_twNAged++;
        { float mv = g_twWalk.camMove / (float)age;
          if (mv >= 1.0f) { pan = 1; InterlockedIncrement(&g_twNPan); InterlockedExchangeAdd(&g_twPanSum16, (LONG)(mv * 16.0f)); }
          if (g_twDumpGap > 0) g_twDumpGap--;
          if (g_twDumpLeft > 0 && (battle || g_twDumpAny)) {
              if (g_twDumpAfter > 0) g_twDumpAfter--;
              else if (!g_twDumpGap && (!g_twDumpPan || mv >= 3.0f || (g_twDumpOnce && !(g_twDumpN & 1)))) {     // (the once-only set: every other pair whatever the camera does)
                  twDump(); g_twDumpLeft--; g_twDumpGap = 45;
                  logf("tween: pictures %02d written (the in-between frame and the game's frame after it); the camera moved %d.%d units between the two game frames.", (int)g_twDumpN - 1, (int)mv, (int)(mv * 10.0f) % 10);
                  if (g_twDumpOnce && g_twDumpLeft == 0) twDumpDone(); } } }
        g_twNTween++;
    }
    if (!did) {
        if (walkable && !plain && !blend) walked = twWalkGuarded(rec, 0, 0, 1.0f);      // only read: the next frame blends from this one
        else twGenMiss();                                  // not even that: the next frame blends from the one before, if there is one
        I = 0;
    }
    w.ibSurf = 0;
    LONG64 a = qpcNow();
    pwAfterWalk(walked, did, I, S, a);                     // what each of the two pictures shows the world from (aotr_panwarp.inc)
    if (S && (did || (w.active && !(test & ~32)))) {       // its pictures take their places in time (a frame without an in-between picture keeps its own)
        twSchedule(a, I, S, pa, test, pan, rec ? rec->pad : 0);
        if (!did) g_twNSlotOnly++;
        hr = w.lastHr;
    } else {
        twFlush();                                         // nothing made before this frame may be shown after it
        if (S) { twCopyToBackBuffer(S); g_twNSlotOnly++; }
        g_pw.lastOk = 0; g_pwLastSurf = 0;                 // what is on screen now is not a picture that could be shown again
        hr = twRealPresent(pa);
        w.lastShow = qpcNow(); w.pace.lastDue = 0;
        if (rec) twLagNote(rec->pad, w.lastShow, 0);
    }
    if (g_twStretch) twClose();
    w.lastHr = hr;
    twPace(worker);
    if (!w.wantOff) twFlush();                             // the next frame is drawn straight into the back buffer: nothing may be copied over it later
    twNextTarget(dev);
    InterlockedExchange(&g_twBusy, (w.qn > 0 || g_twGiveUp || pwBusy()) ? 1 : 0);
    if (!worker) LeaveCriticalSection(&g_rtExecCs);
    return hr;
}
static void twFillArgs(TwPresentArgs& p, int viaSc, DWORD self, DWORD a0, DWORD a1, DWORD a2, DWORD flags) {
    p.viaSc = viaSc; p.self = self; p.flags = flags; p.wnd = a2;
    p.haveSrc = a0 ? 1 : 0; p.haveDst = a1 ? 1 : 0;
    if (a0) memcpy(p.src, (const void*)(ULONG_PTR)a0, 16);
    if (a1) memcpy(p.dst, (const void*)(ULONG_PTR)a1, 16);
}
static DWORD __stdcall tw_Present(DWORD self, DWORD a0, DWORD a1, DWORD a2, DWORD a3) {
    if (a3) return ((TwF5)g_twO_Present)(self, a0, a1, a2, a3);       // a dirty region: only ever with a copy swap effect, never seen
    TwPresentArgs p; twFillArgs(p, 0, self, a0, a1, a2, 0);
    g_tw.dev = self;
    return twPresentCore(p, self);
}
static DWORD __stdcall tw_ScPresent(DWORD self, DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4) {
    if (a3) return ((TwF6)g_twO_ScPresent)(self, a0, a1, a2, a3, a4);
    TwPresentArgs p; twFillArgs(p, 1, self, a0, a1, a2, a4);
    return twPresentCore(p, g_tw.dev);
}

// ---------------------------------------------------------------- device reset, switches, report
static LONG g_twResetNoScope = 0;                          // test (AOTR_RESETNOSCOPE=1, not in production builds): free everything at a reset outside a direct scope, as v60-v65 did
static void twDropAll() {                                  // exec lock held, queue drained
    TwW& w = g_tw;
    if (g_twStretch) twClose();
    w.qn = 0; w.seqOut = w.seqIn; w.ibSurf = 0; w.standIn = 0; w.inScene = 0; g_twPresentRec = NULL;
    twFreeObjects();
    w.resFail = 0; w.lastShow = 0; w.gen[0].valid = 0; w.gen[1].valid = 0; w.prevAge = 0;
    twpReset(w.pace, g_pqpf.QuadPart);
    InterlockedExchange(&g_twHold, 0); InterlockedExchange(&g_twBusy, 0);
}
static void twOnReset() {                                  // game thread, before the device is reset: nothing of ours may be left on it
    if (!g_tw.resDev && !g_twStretch) return;
    g_twGt.open = 0; g_twGt.cancel = 0;
    DWORD tid = __readfsdword(0x24);
    // Everything is freed here with direct calls, on the game thread, under the render lock - and that has to be a
    // direct scope, because DXVK calls hooked methods from inside its own: a level surface hands its Release to its
    // texture, and the last reference of any object releases the device. Outside a direct scope the hooks take those
    // for the game's calls: the texture's Release is queued, and the device's Release waits for the queue - which
    // can never run, the lock being ours. That was v60 to v65 on every device reset once pan pictures had made their
    // textures (the game resets the device whenever it loses or regains the foreground: minimise, alt-tab).
    if (g_rtActive && tid == g_rtMainTid && !g_rtDirectScope) {
        LONG sc = g_twResetNoScope ? 0 : 1;                // (test: without the scope, as it was - the queue's own guard must then keep the reset from hanging)
        rtDrain(); EnterCriticalSection(&g_rtExecCs); g_rtDirectScope += sc; twDropAll(); g_rtDirectScope -= sc; LeaveCriticalSection(&g_rtExecCs);
    }
    else if (g_rtActive && tid != g_rtWorkerTid && tid != g_rtMainTid) { EnterCriticalSection(&g_rtExecCs); twDropAll(); LeaveCriticalSection(&g_rtExecCs); }
    else twDropAll();
}
// The device refused a reset (game thread). With DXVK that means something that has to go before a reset is still
// alive: a default-pool buffer or texture, a render target, a state block. What of ours could be, for the log.
static void twResetRefused(DWORD hr) {
    LONG bm = 0, bmDead = 0, tm = 0;
    for (int i = 0; i < RT_BMIR; ++i) if (g_rtBM[i].live) { ++bm; if (g_rtBM[i].dead) ++bmDead; }
    for (int i = 0; i < RT_TMIR; ++i) if (g_rtTM[i].live) ++tm;
    logf("rt: the device REFUSED the reset (0x%08X) - one of its default-pool objects, render targets or state blocks is still alive. Of ours: %d buffer copies hold their buffer (%d of them given up but not yet given back), %d staged writes are open, "
         "%d texture copies hold their texture, in-between frames %s, pan pictures %s, shared-draw buffers %s. (The game asks again every frame and ends itself after fifty refusals.)",
         hr, (int)bm, (int)bmDead, (int)g_rtNStage, (int)tm, g_tw.resDev ? "still hold their targets" : "hold nothing", g_pw.dev ? "still hold their textures" : "hold nothing", g_mgAny ? "exist" : "none");
}
static void twToggle() {                                   // key thread
    if (!g_twO_SetRT) return;
    LONG was = InterlockedExchange(&g_twOn, g_twOn ? 0 : 1);
    logf("tween: Ctrl+F10 - in-between frames %s.", was ? "OFF" : "ON");
}
static void twBeatToggle() {                               // key thread
    if (!g_twO_SetRT) return;
    LONG was = InterlockedExchange(&g_twBeatOn, g_twBeatOn ? 0 : 1);
    logf("tween: Ctrl+F9 - pictures are now %s.", was ? "shown as each is finished (the game's frame half a period after its in-between frame)" : "put on a steady beat");
}
static void twInitOnce() {
    static LONG done = 0;
    if (InterlockedExchange(&done, 1)) return;
    twTablesInit();
    g_tw.defer = (TwDef*)VirtualAlloc(NULL, TW_DEFER_N * sizeof(TwDef), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    { HMODULE mm = LoadLibraryA("winmm.dll"); typedef UINT (WINAPI* tTBP)(UINT); tTBP tbp = mm ? (tTBP)GetProcAddress(mm, "timeBeginPeriod") : NULL; if (tbp) tbp(1); }   // pictures are paced to the millisecond: the timer has to be that fine
    dvSwitchEnv();
    { char env[16]; env[0] = 0; if (GetEnvironmentVariableA("AOTR_PANGRID", env, sizeof(env)) && env[0]) InterlockedExchange(&g_pwGridOn, env[0] == '0' ? 0 : (env[0] == '2' ? 2 : 1)); }
    { char env[16]; env[0] = 0; if (GetEnvironmentVariableA("AOTR_PANSHARP", env, sizeof(env)) && env[0]) InterlockedExchange(&g_pwSharp, env[0] != '0' ? 1 : 0); }
#ifndef AOTR_PROD
    { char env[16]; env[0] = 0; if (GetEnvironmentVariableA("AOTR_RESETNOSCOPE", env, sizeof(env)) && env[0] == '1') g_twResetNoScope = 1; }
#endif
}
static DWORD WINAPI twReport(LPVOID) {
    LONG dxvkAsk = 0;
    LONG lTween = 0, lMark = 0, lCancel = 0, lPal = 0, lNew = 0, lFar = 0, lWorld = 0, lCam = 0, lCut = 0, lFix = 0, lTag = 0, lReplay = 0, lNoPrev = 0, lF = 0, lDev = 0, lCs = 0, lGr = 0;
    LONG64 lTicks = 0, lBusy = rd64(&g_rtBusyTicks); LONG namesLogged = 0, lPan = 0, lPanSum = 0, lGiveUp = 0, lAged = 0;
    LONG lPic = 0, lG15 = 0, lG25 = 0, lPanPic = 0, lPanG15 = 0, lPanG25 = 0, lBeatLate = 0, lForced = 0, lPulled = 0, lOnTime = 0, lInHandN = 0;
    LONG64 lGapSum = 0, lBeatLateSum = 0, lShowLateSum = 0, lInHandSum = 0;
    for (;;) {
        Sleep(5000);
        if (g_pw.ready && !g_dxvkMode && (++dxvkAsk % 2) == 1) scrDxvkModeCheck(g_pwLeadUs ? g_pwLeadUs : 2000);      // (which mode DXVK presents in: from its own log, once pictures are shown)
        LONG64 busyNow = rd64(&g_rtBusyTicks);
        LONG pan = g_twNPan, panSum = g_twPanSum16, dPan = pan - lPan, dPanSum = panSum - lPanSum; lPan = pan; lPanSum = panSum;
        LONG mark = g_twNMark, dMark = mark - lMark; lMark = mark;
        LONG fr = g_mkFrames, dF = fr - lF; lF = fr;
        LONG tween = g_twNTween, cancel = g_twNCancel, pal = g_twNPal, nw = g_twNPalNew + g_twNWorldNew, far_ = g_twNPalFar,
             world = g_twNWorld, cam = g_twNCam, cut = g_twNCamCut, fix = g_twNFix, tag = g_twNTag, rp = g_twNReplay, np = g_twNNoPrev, dev = g_twNDevCam, cs = g_twNCamSpace, gr = g_twNGround,
             giveUp = g_twNGiveUp, aged = g_twNAged;
        LONG64 ticks = rd64(&g_twReplayTicks);
        LONG dTween = tween - lTween, dRp = rp - lReplay;
        // what went on screen, and how evenly
        LONG pic = g_twNPicShown, g15 = g_twNGap15, g25 = g_twNGap25, panPic = g_twNPanPic, panG15 = g_twNPanGap15, panG25 = g_twNPanGap25, beatLate = g_twNBeatLate, forced = g_twNForced, pulled = g_twNPulled,
             onTime = g_twNOnTime, inHandN = g_twNInHand;
        LONG64 gapSum = rd64(&g_twGapSum), beatLateSum = rd64(&g_twBeatLateSum), showLateSum = rd64(&g_twShowLateSum), inHandSum = rd64(&g_twInHandSum);
        LONG gapMax = InterlockedExchange(&g_twGapMax, 0), panGapMax = InterlockedExchange(&g_twPanGapMax, 0), beatLateMax = InterlockedExchange(&g_twBeatLateMax, 0), showLateMax = InterlockedExchange(&g_twShowLateMax, 0);
        if (dMark > 0 && dF > 0) {
            char ms[16];
            LONG64 qf = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
            int busyPct = (int)(((busyNow - lBusy) * 100) / (qf * 5));
            logf("tween: %s | per second: %d game frames + %d in-between = %d shown | render thread busy %d%% of the time, an in-between frame takes it %s ms | per in-between frame: %d meshes tagged, %d joint palettes and %d object "
                 "transforms blended, %d new, %d too far to blend, camera %d shader + %d device matrices blended, %d re-aimed | in these 5 s: %d camera-space views, %d flat batches moved with the ground (units at height %d), %d cuts, "
                 "camera moving in %d in-between frames (%d.%d units a frame) | frames without one: %d cancelled, %d had no frame before, %d given up for queue space | blended across a frame that could not be read: %d",
                 g_tw.active ? "ON" : (g_twOn ? "on, but it cannot run right now" : "off"), (int)(dF / 5), (int)(dTween / 5), (int)((dF + dTween) / 5), busyPct, msStr(ticks - lTicks, dRp > 0 ? dRp : 1, ms),
                 (int)((tag - lTag) / (dRp > 0 ? dRp : 1)), (int)((pal - lPal) / (dTween > 0 ? dTween : 1)), (int)((world - lWorld) / (dTween > 0 ? dTween : 1)), (int)((nw - lNew) / (dTween > 0 ? dTween : 1)),
                 (int)((far_ - lFar) / (dTween > 0 ? dTween : 1)), (int)((cam - lCam) / (dTween > 0 ? dTween : 1)), (int)((dev - lDev) / (dTween > 0 ? dTween : 1)), (int)((fix - lFix) / (dTween > 0 ? dTween : 1)), (int)(cs - lCs), (int)(gr - lGr), (int)g_twGroundZ, (int)(cut - lCut),
                 (int)dPan, (int)(dPan > 0 ? dPanSum / dPan / 16 : 0), (int)(dPan > 0 ? (dPanSum * 10 / dPan / 16) % 10 : 0),
                 (int)(cancel - lCancel), (int)(np - lNoPrev), (int)(giveUp - lGiveUp), (int)(aged - lAged));
            LONG dPic = pic - lPic;
            if (dPic > 0) {
                char m1[16], m2[16], m3[16], m4[16];
                LONG dLate = beatLate - lBeatLate, dOn = onTime - lOnTime, dHand = inHandN - lInHandN;
                logf("tween: pacing, pictures %s | %d shown, one every %s ms | gaps longer than 1.5 times that: %d, longer than 2.5 times: %d, the longest %d.%d ms | while the camera moved: %d pictures, %d and %d such gaps, the longest %d.%d ms | "
                     "in-between frames finished after their beat: %d (%s ms after it on average, %d.%d at most) | a finished picture waits %s ms for its beat on average | shown early: %d for a free target, %d pulled in | "
                     "pictures go up %s ms after their time on average, %d.%d at most",
                     g_twBeatOn ? "on a steady beat" : "shown as each is finished (Ctrl+F9)", (int)dPic, msStr(gapSum - lGapSum, dPic, m1), (int)(g15 - lG15), (int)(g25 - lG25), (int)(gapMax / 1000), (int)((gapMax / 100) % 10),
                     (int)(panPic - lPanPic), (int)(panG15 - lPanG15), (int)(panG25 - lPanG25), (int)(panGapMax / 1000), (int)((panGapMax / 100) % 10),
                     (int)dLate, msStr(beatLateSum - lBeatLateSum, dLate > 0 ? dLate : 1, m2), (int)(beatLateMax / 1000), (int)((beatLateMax / 100) % 10),
                     msStr(inHandSum - lInHandSum, dHand > 0 ? dHand : 1, m3), (int)(forced - lForced), (int)(pulled - lPulled),
                     msStr(showLateSum - lShowLateSum, dOn > 0 ? dOn : 1, m4), (int)(showLateMax / 1000), (int)((showLateMax / 100) % 10));
                { static LONG lLagN = 0, lLagIbN = 0; static LONG64 lLagSum = 0, lLagIbSum = 0;        // how far the screen runs behind the game
                  LONG ln = g_twLagN, lin = g_twLagIbN; LONG64 ls = rd64(&g_twLagSum), lis = rd64(&g_twLagIbSum);
                  if (ln > lLagN) { char q1[16], q2[16];
                      logf("tween: lag - a frame's own picture is on screen %s ms after the game thread finished that frame; the in-between picture ahead of it (halfway to that frame) %s ms after",
                           msStr(ls - lLagSum, ln - lLagN, q1), msStr(lis - lLagIbSum, lin > lLagIbN ? lin - lLagIbN : 1, q2)); }
                  lLagN = ln; lLagIbN = lin; lLagSum = ls; lLagIbSum = lis; }
            }
            { static LONG lShift = 0, lFill = 0, lAsIs = 0, lNoScene = 0, lFar = 0, lFail = 0, lCopies = 0, lScr = 0, lStood = 0; static LONG64 lPx = 0;      // pan pictures (aotr_panwarp.inc)
              LONG sh = g_pwNShift, fi = g_pwNFill, as = g_pwNAsIs, ns = g_pwNNoScene, tf = g_pwNTooFar, df = g_pwNDrawFail, cp = g_pwNCopies; LONG64 pxs = rd64(&g_pwPxSum16);
              LONG mx = InterlockedExchange(&g_pwMaxPx16, 0), fm = InterlockedExchange(&g_pwFillMaxMs, 0);
              if (sh != lShift || fi != lFill || as != lAsIs || ns != lNoScene) {
                  LONG dSh = sh - lShift;
                  logf("panpictures: last 5 s - %d pictures shown from where the camera was at that moment (moved %d.%d px on average, %d.%d at most), %d as they were (within %d.%02d px); %d times the picture on screen was shown again further along the pan while none came (%d ms ahead of the game's last frame at most) | "
                       "%d scene copies; not shifted: %d had no scene copy, %d were further off than a shift may go, %d could not be drawn | a picture usually goes up %d ms after its frame was begun | of %d scrolled frames the camera read from the frame had not moved in %d",
                       (int)dSh, dSh ? (int)((pxs - lPx) / dSh / 16) : 0, dSh ? (int)(((pxs - lPx) * 10 / dSh / 16) % 10) : 0, (int)(mx / 16), (int)((mx * 10 / 16) % 10), (int)(as - lAsIs), (int)g_pwDeadPx, (int)(g_pwDeadPx * 100.0f) % 100,
                       (int)(fi - lFill), (int)fm, (int)(cp - lCopies), (int)(ns - lNoScene), (int)(tf - lFar), (int)(df - lFail), (int)((LONG64)g_pw.lagMark * 1000 / qf),
                       (int)(g_pwNScrolled - lScr), (int)(g_pwNStood - lStood));
              }
              lScr = g_pwNScrolled; lStood = g_pwNStood;
              { static LONG lGrid = 0, lGridNew = 0, lGap[4] = { 0, 0, 0, 0 }, lPassed = 0, lClock = 0, lOwn = 0, lTail = 0, lHeld = 0, lHn = 0, lH1 = 0; static LONG64 lHs = 0;
                LONG hn = g_pwHandN, h1 = g_pwHandOver1, hm = InterlockedExchange(&g_pwHandMaxUs, 0); LONG64 hs = rd64(&g_pwHandSumUs);
                LONG gr = g_pwNGrid, gn = g_pwNGridNew, gp[4] = { g_pwGridGapN[0], g_pwGridGapN[1], g_pwGridGapN[2], g_pwGridGapN[3] }, ps = g_pwNPassed, ck = g_pwNSlotClock, ow = g_pwNSlotOwn, tl = g_pwNTail, hd = g_pwNHeld;
                if (gr != lGrid || gn != lGridNew)
                  { static LONG lEv = 0, lE5 = 0, lE20 = 0; LONG ev = g_pwEvenN, e5 = g_pwEvenOff5, e20 = g_pwEvenOff20, wp = InterlockedExchange(&g_pwEvenWorstPm, 0);
                    if (ev > lEv) logf("panpictures: last 5 s, how even the scroll was on screen - the camera's speed from one refresh to the next, against the eight before: %d refreshes measured, %d more than 5 %% off, %d more than 20 %% off, the worst %d.%d %% (a steady scroll is 0; the first refreshes of a scroll, while it gathers speed, are not counted)",
                                    (int)(ev - lEv), (int)(e5 - lE5), (int)(e20 - lE20), (int)(wp / 10), (int)(wp % 10));
                    lEv = ev; lE5 = e5; lE20 = e20; }
                    logf("panpictures: last 5 s, one picture for each refresh of the screen while the camera scrolled (every %d.%d ms; %s) - at %d refreshes one of the game's pictures went up (%d more were passed over for a newer one), at %d the one on screen again from further along | "
                         "time since the picture before: one refresh %d times, up to 1.75 refreshes %d, up to 2.5 %d, longer %d (the longest this session %d ms) | handed over %d.%d ms ahead of the refresh (in fact %d us after that moment on average, %d at most, %d times more than a millisecond after); the camera's moment on screen runs %d ms behind it; the end of a scroll was played out over %d refreshes (%d pictures from after it kept back)",
                         (int)(g_pw.refresh * 1000 / qf), (int)((g_pw.refresh * 10000 / qf) % 10), ck - lClock >= ow - lOwn ? "timed by the screen's own refresh" : "on a beat of our own - the screen's refresh could not be read",
                         (int)(gn - lGridNew), (int)(ps - lPassed), (int)(gr - lGrid),
                         (int)(gp[0] - lGap[0]), (int)(gp[1] - lGap[1]), (int)(gp[2] - lGap[2]), (int)(gp[3] - lGap[3]), (int)(g_pwGridGapMax / 1000),
                         (int)((LONG64)pwLead() * 1000 / qf), (int)(((LONG64)pwLead() * 10000 / qf) % 10), hn > lHn ? (int)((hs - lHs) / (hn - lHn)) : 0, (int)hm, (int)(h1 - lH1),
                         (int)((LONG64)g_pw.lagGrid * 1000 / qf), (int)(tl - lTail), (int)(hd - lHeld));
                lGrid = gr; lGridNew = gn; for (int i = 0; i < 4; ++i) lGap[i] = gp[i]; lPassed = ps; lClock = ck; lOwn = ow; lTail = tl; lHeld = hd; lHn = hn; lH1 = h1; lHs = hs; }
              lShift = sh; lFill = fi; lAsIs = as; lNoScene = ns; lFar = tf; lFail = df; lCopies = cp; lPx = pxs; }
            dvReport();
            LONG nn = g_twNNames;
            if (nn > namesLogged) { char b[720]; int q = 0; b[0] = 0; for (LONG i = 0; i < nn && i < 16; ++i) q += wsprintfA(b + q, "%s%s", i ? ", " : "", g_twNames[i]); namesLogged = nn;
                logf("tween: shader parameters recognised so far: %s.", b); }
            if (cancel - lCancel > 0) { char b[400]; int q = 0; b[0] = 0; for (int i = 0; i < 8 && g_twCancelOp[i]; ++i) q += wsprintfA(b + q, "%s%s", i ? ", " : "", rtOpName(g_twCancelOp[i] - 1));
                logf("tween: frames are cancelled by direct calls of: %s.", b); }
        }
        lTween = tween; lCancel = cancel; lPal = pal; lNew = nw; lFar = far_; lWorld = world; lCam = cam; lCut = cut; lFix = fix; lTag = tag; lReplay = rp; lNoPrev = np; lTicks = ticks; lDev = dev; lCs = cs; lGr = gr; lBusy = busyNow;
        lGiveUp = giveUp; lAged = aged; lPic = pic; lG15 = g15; lG25 = g25; lPanPic = panPic; lPanG15 = panG15; lPanG25 = panG25; lBeatLate = beatLate; lForced = forced; lPulled = pulled; lOnTime = onTime; lInHandN = inHandN;
        lGapSum = gapSum; lBeatLateSum = beatLateSum; lShowLateSum = showLateSum; lInHandSum = inHandSum;
    }
}

// ---------------------------------------------------------------- the engine side
typedef void (__fastcall* tTwDraw)(void* self, void* edx, DWORD effect, DWORD count);
static tTwDraw o_twDraw = NULL;
static void __fastcall hkTwDraw(void* self, void* edx, DWORD effect, DWORD count) {
    if (g_twGt.open) twTagDraw((DWORD)(ULONG_PTR)self, count);
    o_twDraw(self, edx, effect, count);
}
static DWORD g_twBeginRenderNext = TW_BEGIN_RENDER_FN;     // what the call site called when we got there
static DWORD* g_twEngineDev = (DWORD*)(ULONG_PTR)MG_DEVICE;   // where the engine keeps its device
static void __cdecl twMarkEngine() { twMark(*g_twEngineDev); }
static __declspec(naked) void twBeginRenderStub() {       // in place of the call to WW3D::Begin_Render: mark, then the call itself
    __asm {
        pushad
        call twMarkEngine
        popad
        jmp dword ptr [g_twBeginRenderNext]
    }
}
static void installTween() {
    char ini[MAX_PATH];
    aotrPath(ini, "bfme2_accel.ini");
    int want = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES ? (int)GetPrivateProfileIntA("Engine", "InBetweenFrames", 0, ini) : 0;
    char env[16]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_TWEEN", env, sizeof(env)) && env[0]) want = env[0] - '0';
    if (want <= 0) return;
    env[0] = 0;
    int check = (GetEnvironmentVariableA("AOTR_TWEEN_CHECK", env, sizeof(env)) && env[0]) ? atoi(env) : (int)GetPrivateProfileIntA("Engine", "InBetweenCheck", 0, ini);
    env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_TWEEN_DUMP", env, sizeof(env)) && env[0]) { g_twDumpLeft = atoi(env); const char* c = strchr(env, ','); g_twDumpAfter = c ? atoi(c + 1) : 300; }
    else { g_twDumpLeft = (LONG)GetPrivateProfileIntA("Engine", "InBetweenDump", 0, ini); g_twDumpAfter = 120; g_twDumpPan = 1;      // from the ini: pictures of a moving camera
        if (g_twDumpLeft > 0) {                             // ... once: not in every session until someone remembers to take the key out again
            char done[MAX_PATH]; aotrPath(done, "picture_dump_done.txt");
            WIN32_FILE_ATTRIBUTE_DATA fd, fi;
            if (GetFileAttributesExA(done, GetFileExInfoStandard, &fd) && GetFileAttributesExA(ini, GetFileExInfoStandard, &fi) && CompareFileTime(&fi.ftLastWriteTime, &fd.ftLastWriteTime) <= 0) g_twDumpLeft = 0;
            else { g_twDumpOnce = 1; InterlockedExchange(&g_pwDumpLeft, 2);
                   logf("tween: InBetweenDump = %d - once a match is under way, that many pairs of pictures (the in-between frame and the game's frame after it) and two pictures shown from another camera are written next to the DLL (*_dump_*.bmp); each is a hitch of a tenth of a second. Once only: not again until the ini is saved anew.", (int)g_twDumpLeft); } } }
    { int beat = (int)GetPrivateProfileIntA("Engine", "PictureBeat", 1, ini); env[0] = 0;
      if (GetEnvironmentVariableA("AOTR_BEAT", env, sizeof(env)) && env[0]) beat = env[0] - '0';
      InterlockedExchange(&g_twBeatOn, beat ? 1 : 0); }
    static const BYTE kDraw[7] = { 0x6A,0xFF,0x68,0x58,0xA9,0xB7,0x00 };
    __try {
        { const BYTE* q = (const BYTE*)TW_BEGIN_RENDER_AT;                       // a diagnostic build has put its timer on this call: go through it
          DWORD target = q[0] == 0xE8 ? TW_BEGIN_RENDER_AT + 5 + *(const DWORD*)(q + 1) : 0;
          if (target != TW_BEGIN_RENDER_FN && !(g_diag && target)) { logf("tween: the frame's Begin_Render call is not where this build has it - in-between frames off."); return; }
          g_twBeginRenderNext = target; }
        bool ownDraw = !memcmp((void*)TW_DRAW_FN, kDraw, sizeof(kDraw));
        if (ownDraw) { o_twDraw = (tTwDraw)makeTrampoline((BYTE*)TW_DRAW_FN, 7); if (!o_twDraw) { logf("tween: no trampoline - in-between frames off."); return; } }
        HANDLE fz[256]; int nf = 0;
        suspendOthers(fz, &nf, 256);
        rtSetCall(TW_BEGIN_RENDER_AT, (void*)&twBeginRenderStub);
        if (ownDraw) patchJmp((BYTE*)TW_DRAW_FN, (void*)&hkTwDraw, 7);
        resumeAll(fz, nf);
        g_twDrawTag = &twTagDraw;                           // a diagnostic build has its own hook on the mesh draw: it calls this
        g_twViewsEnd = &twViewsEnd;                         // called where the stage timer sees the views end
        if (check > 0) g_twCheckLeft = check;
        InterlockedExchange(&g_dvOn, GetPrivateProfileIntA("Engine", "DevicePass", 1, ini) ? 1 : 0);
        twInitOnce();
        logf("devpass: %s", g_dvOn ? "ON - an in-between picture's effect calls are answered with the device calls D3DX made for the frame itself, blended bones and transforms put straight into their registers; frames in which the camera moved, "
                                     "and anything that does not check out, are made through D3DX as before. The first frames are verified against D3DX before any is made this way."
                                   : "off ([Engine] DevicePass = 0) - every in-between picture is made through D3DX.");
        { HMODULE mm = LoadLibraryA("winmm.dll"); typedef UINT (WINAPI* tTBP)(UINT);       // the idle render thread sleeps up to 2 ms before a picture's time: the timer has to be that fine (the game asks for the same)
          tTBP tbp = mm ? (tTBP)GetProcAddress(mm, "timeBeginPeriod") : NULL; if (tbp) tbp(1); }
        { int pw = (int)GetPrivateProfileIntA("Engine", "PanPictures", 0, ini); env[0] = 0;
          if (GetEnvironmentVariableA("AOTR_PANWARP", env, sizeof(env)) && env[0]) pw = env[0] - '0';
          InterlockedExchange(&g_pwOn, pw > 0 ? 1 : 0);
          { int gb = (int)GetPrivateProfileIntA("Engine", "PanRefreshBeat", 1, ini); char e3[16]; e3[0] = 0;
            if (!(GetEnvironmentVariableA("AOTR_PANGRID", e3, sizeof(e3)) && e3[0])) InterlockedExchange(&g_pwGridOn, gb <= 0 ? 0 : (gb == 2 ? 2 : 1));
            char lb[32]; lb[0] = 0; GetPrivateProfileStringA("Engine", "PanLeadMs", "", lb, sizeof(lb), ini); double lm = lb[0] ? atof(lb) : 0.0; e3[0] = 0;
            if (GetEnvironmentVariableA("AOTR_PANLEAD", e3, sizeof(e3)) && e3[0]) lm = atof(e3);
            if (lm >= 0.2 && lm <= 12.0) g_pwLeadUs = (LONG)(lm * 1000.0); }
          { char e2[16]; e2[0] = 0; if (!(GetEnvironmentVariableA("AOTR_PANSHARP", e2, sizeof(e2)) && e2[0])) InterlockedExchange(&g_pwSharp, GetPrivateProfileIntA("Engine", "PanSharp", 1, ini) ? 1 : 0); }
          logf("panpictures: %s", pw > 0 ? "ON - while the camera scrolls, each picture is shown from where the camera is at that moment (one rectangle on the GPU), and the picture on screen is shown again from further along when the next one is late. Ctrl+F8 switches it live."
                                         : "off ([Engine] PanPictures = 0) - every picture is shown as it was drawn. Ctrl+F8 switches it on."); }
        InterlockedExchange(&g_twOn, 1);
        CreateThread(NULL, 0, twReport, NULL, 0, NULL);
        logf("tween: installed - the render thread draws a frame between every two of the game's (bones, objects and camera halfway), for every frame at any frame rate. Pictures go on screen %s (Ctrl+F9 switches that live, "
             "Ctrl+F10 the in-between frames).%s", g_twBeatOn ? "on a steady beat" : "as each is finished", check > 0 ? " Self-check: in the first battle a few stretches are run again unchanged and compared." : "");
    } __except (EXCEPTION_EXECUTE_HANDLER) { logf("tween: install faulted - in-between frames off."); }
}

// ---------------------------------------------------------------- test entry points (offline harness, tween_test)
extern "C" __declspec(dllexport) void __cdecl AotrTweenTestMode(int bits) {      // see g_twTest
    twInitOnce();
    InterlockedExchange(&g_twTest, bits);
    InterlockedExchange(&g_twOn, bits ? 1 : 0);
    InterlockedExchange(&g_twWantMark, bits ? 1 : 0);
}
extern "C" __declspec(dllexport) void __cdecl AotrTweenTestMark(void* dev) { twMark((DWORD)(ULONG_PTR)dev); }
// the second pass at the device (aotr_devpass.inc): 0 off, 1 as in the game (verified first), 2 every pass at the device, 3 every pass by D3DX and compared
extern "C" __declspec(dllexport) void __cdecl AotrDevPassTest(int mode) { g_dvs.test = mode >= 2 ? mode : 0; InterlockedExchange(&g_dvOn, mode > 0 ? 1 : 0); }
extern "C" __declspec(dllexport) void __cdecl AotrDevPassStats(char* out, int n) {
    char b[1200]; int q = 0; LONG stops = 0, would = 0;
    for (int i = 1; i < DV_AB_N; ++i) { stops += g_dvNStop[i]; would += g_dvNWould[i]; }
    q += wsprintfA(b + q, "devpass: on=%d test=%d at_device=%d (effect_calls=%d device_calls=%d blended_uploads=%d camera_products=%d) verified=%d clean=%d draws=%d blended_registers=%d preshader_results=%d not_uploaded=%d differences=%d stops=%d would_stop=%d ",
                   (int)g_dvOn, (int)g_dvs.test, (int)g_dvNLive, (int)g_dvNChunks, (int)g_dvNCalls, (int)g_dvNPatched, (int)g_dvNPre, (int)g_dvNVerify, (int)g_dvNVerifyClean, (int)g_dvVDraws, (int)g_dvVRegs, (int)g_dvVPre, (int)g_dvVUnset, (int)g_dvNFail, (int)stops, (int)would);
    for (int i = 1; i < DV_AB_N; ++i) if (g_dvNStop[i] || g_dvNWould[i]) q += wsprintfA(b + q, "[%d+%d %s] ", (int)g_dvNStop[i], (int)g_dvNWould[i], kDvAb[i]);
    q += wsprintfA(b + q, "unusable_recordings: not_recorded=%d call=%d%s%s full=%d stray=%d | camera_frames=%d after_stop=%d off=%d recorded_frames=%d kb_a_frame=%d",
                   (int)g_dvNNoRec, (int)g_dvNBadCall, g_dvLastBadSlot ? " " : "", g_dvLastBadSlot ? rtOpName(g_dvLastBadSlot - 1) : "", (int)g_dvNBadFull, (int)g_dvNBadStray, (int)g_dvNCam, (int)g_dvNSkip, (int)g_dvNOff,
                   (int)g_dvNRecFrames, (int)(g_dvNRecFrames ? rd64(&g_dvRecBytes) / g_dvNRecFrames / 1024 : 0));
#ifndef AOTR_PROD
    { LONG64 all = g_twSplit[3] ? g_twSplit[3] : 1, np = g_twSplit[4] ? g_twSplit[4] : 1;
      q += wsprintfA(b + q, " | a second pass: %d kilocycles = parameter runs %d%% + effect calls %d%% + the stream's device calls %d%% + the rest %d%%", (int)(all / np / 1000), (int)(g_twSplit[0] * 100 / all), (int)(g_twSplit[1] * 100 / all),
                     (int)(g_twSplit[2] * 100 / all), (int)((all - g_twSplit[0] - g_twSplit[1] - g_twSplit[2]) * 100 / all)); }
#endif
    lstrcpynA(out, b, n);
}
// pan pictures: bits as g_pwTest (0: off). 1: every picture goes through the rectangle with its own camera and the
// back buffer is compared with the picture.
extern "C" __declspec(dllexport) void __cdecl AotrPanWarpTest(int bits) { InterlockedExchange(&g_pwTest, bits); InterlockedExchange(&g_pwOn, bits ? 1 : 0); }
extern "C" __declspec(dllexport) void __cdecl AotrPanWarpForce(const float* V, const float* P, float gh) {      // test bit 2: the camera pictures are shown from, the ground's height
    if (!V || !P) { InterlockedExchange(&g_pwForceSet, 0); return; }
    memcpy(g_pwForceV, V, 64); memcpy(g_pwForceP, P, 64); g_pwForceGh = gh; InterlockedExchange(&g_pwForceSet, 1);
}
extern "C" __declspec(dllexport) int __cdecl AotrPanWarpGrab(BYTE* out, int bytes) {       // the back buffer after the last picture shown that way; returns how many so far
    if (out && g_pwGrabBuf && (DWORD)bytes <= g_pwGrabBytes) memcpy(out, g_pwGrabBuf, bytes);
    return (int)g_pwGrabSeq;
}
extern "C" __declspec(dllexport) void __cdecl AotrPanWarpTestScroll() { g_panCt = (DWORD)qpcNow() | 1; }       // as the scroll hook: this frame's camera step was taken now
extern "C" __declspec(dllexport) void __cdecl AotrPanWarpStats(char* out, int n) {
    if (out && n > 600) {
        int q = wsprintfA(out, "pan pictures: %d through the rectangle with their own camera - %d came out the same, %d different, %d could not be read | scene copies %d, draws failed %d | first difference at (%u,%u), %u pixels",
                          (int)g_pwNTestDraw, (int)g_pwNTestSame, (int)g_pwNTestDiff, (int)g_pwNTestFail, (int)g_pwNCopies, (int)g_pwNDrawFail, g_pwTestFirst[0], g_pwTestFirst[1], g_pwTestFirst[2]);
        LONG sh = g_pwNShift; LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
        wsprintfA(out + q, " | live: %d shown from where the camera was (moved %d.%d px on average, %d at most), %d as they were, %d shown again further along (%d ms ahead at most), %d without a scene copy, %d too far, a picture goes up %d ms after its frame's mark",
                  (int)sh, sh ? (int)(g_pwPxSum16 / sh / 16) : 0, sh ? (int)((g_pwPxSum16 * 10 / sh / 16) % 10) : 0, (int)(g_pwMaxPx16 / 16), (int)g_pwNAsIs, (int)g_pwNFill, (int)g_pwFillMaxMs, (int)g_pwNNoScene, (int)g_pwNTooFar,
                  (int)((LONG64)g_pw.lagMark * 1000 / f));
        q = lstrlenA(out);
        if (q < n - 900) wsprintfA(out + q, " | on the screen's beat (%d.%d ms, switch %d): %d of the game's pictures (%d passed over for a newer one), %d times the one on screen again in between; time since the picture before: one refresh %d times, up to 1.75 refreshes %d, up to 2.5 %d, longer %d, the longest %d.%d ms"
                                            " | refreshes timed by the screen's own clock %d, by a beat of our own %d; lead %d us; the camera's moment %d ms behind the refresh; a scroll's end played out over %d refreshes, %d pictures kept back"
                                            " | the camera's speed on screen from one refresh to the next: %d measured, %d more than 5 %% off the eight before, %d more than 20 %% off, the worst %d per mille"
                                            " | handed over %d us after the planned moment on average, %d at most, %d of %d more than a millisecond after",
                                   (int)(g_pw.refresh * 1000 / f), (int)((g_pw.refresh * 10000 / f) % 10), (int)g_pwGridOn, (int)g_pwNGridNew, (int)g_pwNPassed, (int)g_pwNGrid,
                                   (int)g_pwGridGapN[0], (int)g_pwGridGapN[1], (int)g_pwGridGapN[2], (int)g_pwGridGapN[3], (int)(g_pwGridGapMax / 1000), (int)((g_pwGridGapMax / 100) % 10),
                                   (int)g_pwNSlotClock, (int)g_pwNSlotOwn, (int)((LONG64)pwLead() * 1000000 / f), (int)((LONG64)g_pw.lagGrid * 1000 / f), (int)g_pwNTail, (int)g_pwNHeld,
                                   (int)g_pwEvenN, (int)g_pwEvenOff5, (int)g_pwEvenOff20, (int)g_pwEvenWorstPm,
                                   g_pwHandN ? (int)(g_pwHandSumUs / g_pwHandN) : 0, (int)g_pwHandMaxUs, (int)g_pwHandOver1, (int)g_pwHandN);
    }
}
extern "C" __declspec(dllexport) void __cdecl AotrTweenTestViewsEnd() { twViewsEnd(); }
extern "C" __declspec(dllexport) void __cdecl AotrTweenTestDump(int pairs, int panPictures) { g_twDumpAny = 1; g_twDumpAfter = 20; g_twDumpPan = 0; g_twDumpOnce = 1; g_twDumpLeft = pairs; InterlockedExchange(&g_pwDumpLeft, panPictures); }
extern "C" __declspec(dllexport) float __cdecl AotrTweenTestBarLift() { return g_twBarLift; }
extern "C" __declspec(dllexport) void __cdecl AotrTweenTestPressure() {          // as if the producer had run out of ring space in the middle of a stretch
    if (g_rtFxRec) rtFxClose();
    rtPublish();
    for (int i = 0; i < 200 && !g_twHold; ++i) Sleep(0);                          // let the render thread reach the mark first
    twSpacePressure();
}
extern "C" __declspec(dllexport) void __cdecl AotrTweenTestTag(void* mesh, int count) { twTagDraw((DWORD)(ULONG_PTR)mesh, (DWORD)count); }
extern "C" __declspec(dllexport) void __cdecl AotrTweenTestTagN(const DWORD* meshes, int n) {       // a group that shares one draw
    if (!g_twGt.open || !g_rtFxBatchOn || !g_rtActive || g_rtDirectScope || __readfsdword(0x24) != g_rtMainTid || n < 0 || n > 16) return;
    DWORD* e = rtFxRoom(4 + 4 * (DWORD)n);
    e[0] = 0xF0 | ((DWORD)n << 8);
    for (int i = 0; i < n; ++i) e[1 + i] = meshes[i];
    g_rtFxPos += 4 + 4 * (DWORD)n; g_rtFxN++;
}
extern "C" __declspec(dllexport) int __cdecl AotrTweenTestGrab(BYTE* out, int bytes) {   // the last in-between picture; returns how many have been kept so far
    if (out && g_twGrabBuf && (DWORD)bytes <= g_twGrabBytes) memcpy(out, g_twGrabBuf, bytes);
    return (int)g_twGrabSeq;
}
// The blends themselves, with their limits, for a test that draws the halfway picture on its own. kind: 0 joint
// palette (n joints), 1 object World (engine layout), 2 camera matrix in the engine's layout (View, ViewI),
// 3 projection, 4 the device's VIEW. Returns 1 when out is a blend, 0 when it is b unchanged.
extern "C" __declspec(dllexport) int __cdecl AotrTweenTestBlend(int kind, const float* a, const float* b, int n, float t, float* out) {
    if (kind == 0) { if (!memcmp(a, b, n * 32) || !twLerpPalette(a, b, (DWORD)n, t, out)) { memcpy(out, b, n * 32); return 0; } return 1; }
    bool near_ = kind == 1 ? twMatNear(a, b, false, false, g_twMaxMove) : kind == 2 ? twMatNear(a, b, false, false, g_twMaxCamMove)
               : kind == 3 ? twMatNear(a, b, true, true, g_twMaxCamMove) : twMatNear(a, b, true, false, g_twMaxCamMove);
    if (!memcmp(a, b, 64) || !near_) { memcpy(out, b, 64); return 0; }
    twLerpMat(a, b, t, out); return 1;
}
// The palette blend both ways: returns bit 0 the four-at-a-time verdict, bit 1 the one-joint verdict; the outputs go
// to out and outRef (each untouched-or-unfinished when its verdict is "leave it").
extern "C" __declspec(dllexport) int __cdecl AotrTweenTestLerp(const float* a, const float* b, int n, float t, float* out, float* outRef) {
    return (twLerpPalette(a, b, (DWORD)n, t, out) ? 1 : 0) | (twLerpPaletteRef(a, b, (DWORD)n, t, outRef) ? 2 : 0);
}
// a camera product re-aimed: cls 6 WorldViewProjection, 7 WorldView, 8 WorldViewInverse; cls 1: the VIEW that
// stands in for the identity while camera-space geometry is drawn
extern "C" __declspec(dllexport) int __cdecl AotrTweenTestReaim(int cls, int transposed, const float* X, const float* V, const float* P, const float* V2, const float* P2, float* out) {
    if (cls == 1) return twCamSpaceView(V, V2, out) ? 1 : 0;
    if (cls == 2) return twGroundFix(V, P, V2, P2, X[0], out) ? 1 : 0;       // flat world-pinned geometry: X[0] is the ground height
    TwCamFix f;
    if (!twCamFixBuild(V, P, V2, P2, &f)) return 0;
    twCamFixApply(f, (DWORD)cls, transposed != 0, X, out);
    return 1;
}
// The two engine-side stubs, run against stand-ins for the engine's functions: the Begin_Render call must arrive
// with its four arguments and hand its result back, the mesh draw with its mesh and both arguments.
extern "C" __declspec(dllexport) DWORD __cdecl AotrTweenTestStubs(void* beginRender, void* meshDraw, DWORD a, DWORD b, DWORD c, DWORD d) {
    DWORD keepNext = g_twBeginRenderNext; tTwDraw keepDraw = o_twDraw; DWORD* keepDev = g_twEngineDev; static DWORD noDevice = 0;
    g_twBeginRenderNext = (DWORD)(ULONG_PTR)beginRender; o_twDraw = (tTwDraw)meshDraw; g_twEngineDev = &noDevice;
    DWORD r = ((DWORD(__cdecl*)(DWORD, DWORD, DWORD, DWORD))(void*)&twBeginRenderStub)(a, b, c, d);
    hkTwDraw((void*)(ULONG_PTR)(a ^ 0x5A5A5A5Au), NULL, b, c);
    g_twBeginRenderNext = keepNext; o_twDraw = keepDraw; g_twEngineDev = keepDev;
    return r;
}
extern "C" __declspec(dllexport) void __cdecl AotrTweenTestStats(char* out, int n) {
    if (!out || n < 1024) return;
    char ms[16];
    int q = wsprintfA(out, "marks=%d second_passes=%d same=%d different=%d check_failed=%d cancelled=%d no_verdict=%d gave_up=%d shown_from_offscreen=%d (late %d, on time %d) "
              "copied_only=%d direct_presents=%d waiting_releases_max=%d overflow=%d lost=%d second_pass_ms=%s",
              (int)g_twNMark, (int)g_twNReplay, (int)g_twNSame, (int)g_twNDiff, (int)g_twNCheckFail, (int)g_twNCancel, (int)g_twNNoVerdict, (int)g_twNGiveUp,
              (int)g_twNShown, (int)g_twNLate, (int)g_twNOnTime, (int)g_twNSlotOnly, (int)g_twNDirectPresent, (int)g_twNDeferMax, (int)g_twNDeferOver, (int)g_twNLost,
              msStr(rd64(&g_twReplayTicks), g_twNReplay > 0 ? g_twNReplay : 1, ms));
    q += wsprintfA(out + q, " | held_for_their_place=%d out_of_order=%d after_their_beat=%d pulled_in=%d still_waiting=%d across_a_missed_frame=%d targets=%d",
              (int)g_twNIbHeld, (int)g_twNOrderBad, (int)g_twNBeatLate, (int)g_twNPulled, (int)g_tw.qn, (int)g_twNAged, (int)g_tw.nOff);
    q += wsprintfA(out + q, " | faults=%d in_between=%d read_only=%d no_frame_before=%d tags=%d palettes=%d (new %d, too far %d) worlds=%d (new %d) camera: shader %d device %d re-aimed %d camera_space %d ground %d cuts %d store_overflow=%d",
              (int)g_twFaults, (int)g_twNTween, (int)g_twNCollect, (int)g_twNNoPrev, (int)g_twNTag, (int)g_twNPal, (int)g_twNPalNew, (int)g_twNPalFar, (int)g_twNWorld, (int)g_twNWorldNew,
              (int)g_twNCam, (int)g_twNDevCam, (int)g_twNFix, (int)g_twNCamSpace, (int)g_twNGround, (int)g_twNCamCut, (int)g_twNKeyOver);
    if (g_twNDiff) q += wsprintfA(out + q, " | first difference: second pass %u, pixel (%u,%u), %u pixels", g_twFirstDiff[0], g_twFirstDiff[1], g_twFirstDiff[2], g_twFirstDiff[3]);
    for (int i = 0; i < 8 && g_twCancelOp[i]; ++i) q += wsprintfA(out + q, "%s%s", i ? ", " : " | cancelled by: ", rtOpName(g_twCancelOp[i] - 1));
    if (g_twNNames) { q += wsprintfA(out + q, " | parameters: "); for (LONG i = 0; i < g_twNNames && i < 16; ++i) q += wsprintfA(out + q, "%s%s", i ? "," : "", g_twNames[i]); }
}
