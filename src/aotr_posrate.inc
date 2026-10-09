// ---------------------------------------------------------------- how often does a unit actually move?
// Raising the frame cap was measured to speed the simulation up: the engine runs exactly one GameLogic::update(n)
// per rendered frame, six of those to a logic step, so logic steps = fps / 6 (measured 6.05 frames a step, 1.97x
// game speed at 60). Rendering 60 fps honestly therefore means running logic on only half the frames.
//
// Animation survives that for free - it is a pure function of wall-clock milliseconds ([0x00DD1E0C], written from
// an accumulated timeGetTime delta). POSITION is the open question, and it decides how big the job is:
//
//   ~30 moves a second per object  ->  an object moves on every update(n). Skip logic on surplus frames and the
//                                      extra frames still show new positions. Pacing the calls is the whole job.
//   ~5 moves a second per object   ->  an object moves once per whole logic step. Surplus frames would show
//                                      smoothly animated units sliding in visible steps, and the job also needs
//                                      a position interpolation layer - far more work.
//
// So: each rendered frame, hash the world transform of up to 256 render objects the scene is about to draw and
// count how many changed since the frame before. Per second that gives moves-per-object-per-second directly.
// Measurement only, off unless [Engine] MeasurePositions is set, and it never writes to the game.

#define PR_TRACK   512                       // open-addressed, keyed on the render object pointer
#define PR_SAMPLE  256                       // objects looked at per frame
#define PR_XFORM   0x18                      // RenderObjClass world transform, 48 bytes (12 floats)

struct PrEnt { DWORD obj, hash; };
static PrEnt  g_prT[PR_TRACK];
static LONG   g_prOn = 0;
static DWORD  g_prFrame = 0xFFFFFFFF;        // the render frame already sampled (passes repeat within a frame)
static volatile LONG g_prSeen = 0, g_prMoved = 0, g_prFrames = 0;
// The millisecond counter animation reads (0x00DD1E0C, stamped into each anim at +0x114). Whether it advances per
// RENDERED frame or per logic call decides whether pacing the logic can make animation stutter - and a counter
// that ever goes backwards would make animations snap, which is what "jumping all over the place" looks like.
#define PR_ANIMTIME 0x00DD1E0C
static DWORD g_prLastT = 0;
static volatile LONG g_prTChanged = 0, g_prTBack = 0, g_prTMin = 0x7FFFFFFF, g_prTMax = 0, g_prTSum = 0;

static __forceinline DWORD prHash(const DWORD* w) {
    DWORD h = 2166136261u;
    for (int i = 0; i < 12; ++i) { h ^= w[i]; h *= 16777619u; }
    return h;
}
static void prSample(void* scene) {
    if (!g_prOn) return;
    DWORD fr = (DWORD)g_mkFrames;
    if (fr == g_prFrame) return;             // the shadow pass and the view pass are the same frame
    g_prFrame = fr;
    __try {
        void** arr = *(void***)((char*)scene + 0x800);
        int count  = *(int*)   ((char*)scene + 0x80C);
        if (!arr || count <= 0) return;
        if (count > PR_SAMPLE) count = PR_SAMPLE;
        int seen = 0, moved = 0;
        for (int i = 0; i < count; ++i) {
            void* o = arr[i];
            if (!o) continue;
            DWORD vt = *(DWORD*)o;
            if (vt < 0x00BD0000 || vt >= 0x00D89000) continue;      // the same sanity test the pose warm-up uses
            DWORD h = prHash((const DWORD*)((char*)o + PR_XFORM));
            DWORD key = (DWORD)(ULONG_PTR)o;
            DWORD slot = ((key >> 4) * 2654435761u) >> 23;
            PrEnt* e = NULL;
            for (int k = 0; k < 8; ++k) {
                PrEnt& c = g_prT[(slot + k) & (PR_TRACK - 1)];
                if (c.obj == key || !c.obj) { e = &c; break; }
            }
            if (!e) continue;                                        // table crowded: skip rather than evict
            ++seen;
            if (e->obj == key && e->hash != h) ++moved;               // known object whose transform changed
            e->obj = key; e->hash = h;
        }
        g_prSeen += seen; g_prMoved += moved; g_prFrames++;
        DWORD t = *(DWORD*)PR_ANIMTIME;                      // sampled once per rendered frame
        if (g_prLastT && t != g_prLastT) {
            LONG d = (LONG)(t - g_prLastT);
            g_prTChanged++;
            if (d < 0) g_prTBack++;
            else { if (d < g_prTMin) g_prTMin = d; if (d > g_prTMax) g_prTMax = d; g_prTSum += d; }
        }
        g_prLastT = t;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_prOn = 0; }
}

static DWORD WINAPI prReport(LPVOID) {
    LONG lastFrames = 0, lastSteps = 0, lastSeen = 0, lastMoved = 0, lastSampled = 0;
    LONG lastTC = 0, lastTB = 0, lastTS = 0;
    for (;;) {
        Sleep(1000);
        LONG f = g_mkFrames,  df = f - lastFrames;  lastFrames = f;
        LONG s = g_lsSteps + g_lsStockSteps, ds = s - lastSteps; lastSteps = s;
        LONG sn = g_prSeen,   dn = sn - lastSeen;   lastSeen = sn;
        LONG mv = g_prMoved,  dm = mv - lastMoved;  lastMoved = mv;
        LONG sf = g_prFrames, dsf = sf - lastSampled; lastSampled = sf;
        if (dsf <= 0 || dn <= 0) continue;                            // nothing was drawn this second
        // Integer maths throughout: logf goes through wsprintfA, which has no %f at all - floats print as the
        // literal letter and the first measurement run was lost to exactly that.
        LONG perFrame   = dn / dsf;                                   // objects looked at per sampled frame
        LONG movedPerMil = (LONG)(((LONG64)dm * 1000) / dn);          // share of them that moved, per mille
        LONG movesX10    = (LONG)(((LONG64)dm * 10 * dsf) / dn);      // moves per object per second, x10
        LONG tc = g_prTChanged, dtc = tc - lastTC; lastTC = tc;
        LONG tb = g_prTBack,    dtb = tb - lastTB; lastTB = tb;
        LONG ts = g_prTSum,     dts = ts - lastTS; lastTS = ts;
        logf("posrate: %d frames, %d logic steps | %d objects a frame, %d.%d%% moved between frames -> about "
             "%d.%d moves per object per second.",
             (int)df, (int)ds, (int)perFrame, (int)(movedPerMil / 10), (int)(movedPerMil % 10),
             (int)(movesX10 / 10), (int)(movesX10 % 10));
        logf("animtime: advanced on %d of %d rendered frames, %d of them BACKWARDS | step ms min %d max %d avg %d "
             "| total %d ms of animation time for %d frames of wall clock",
             (int)dtc, (int)dsf, (int)dtb, (int)g_prTMin, (int)g_prTMax,
             dtc ? (int)(dts / dtc) : 0, (int)dts, (int)df);
        g_prTMin = 0x7FFFFFFF; g_prTMax = 0;
    }
}

static void installPosRate() {
    char ini[MAX_PATH];
    aotrPath(ini, "bfme2_accel.ini");
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;
    if (GetPrivateProfileIntA("Engine", "MeasurePositions", 0, ini) <= 0) return;
    g_prOn = 1;
    logf("posrate: measuring how often a drawn object's transform changes. Nothing is written to the game; this "
         "only reads transforms the scene is about to draw.");
    CreateThread(NULL, 0, prReport, NULL, 0, NULL);
}
