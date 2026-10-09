// ================================================================ several unit meshes in one draw call
// A captured battle frame draws ~7,000 meshes and every one of them is its own draw call, although the render
// list arrives sorted so that the same model sits in runs (3.7 on average, up to 59) inside one batch. The engine
// has a path that draws a run in ONE call - render list flush 0x00573BEB:
//
//     n = min(model->fxinfo->maxInstances, meshes left in the batch)
//     n <= 1 : draw(mesh, shader, 1)
//     n >  1 : collect the n mesh transforms, draw(first mesh, shader, n)        "Rendering %d meshes FXShader"
//
// and draw() multiplies the primitive and vertex counts by n. It is how the engine instances rigid props: the
// vertex buffer holds n copies of the mesh whose joint index is the copy number, and the shader's joint palette
// (semantic MeshToJointToWorld, quaternion + position per entry) holds one transform per copy. To the shader an
// instanced prop IS a skinned mesh with one joint per copy.
//
// Units never take that path. The FX mesh info builder (0x0058C05A) gives every SKINNED model a capacity of 1, and
// 93% of what a battle draws is skinned: each body part is a mesh bound to one or a few bones of its unit's
// skeleton, so its palette holds those bones instead. But the same trick fits: k one-bone meshes of the same model
// are, to the shader, one k-bone mesh. So for a run of k skinned meshes of one model
//
//     * the palette gets every mesh's bones, mesh after mesh           (k * B entries instead of B)
//     * the vertex buffer holds k copies whose joint index is offset   (copy c: index + c * B)
//
// and the engine's own multi-mesh path does everything else: world matrix, joints-per-vertex, commit, buffers,
// declaration, DrawIndexedPrimitive with k times the counts. Each vertex is still transformed by exactly the
// quaternion and position it was transformed by before, in the same order, so the pixels are the same.
//
// What is changed, all of it small:
//   1. 24 bytes in the flush where n is computed call mgDecide(), which may answer k for a run of skinned meshes
//      it has checked one by one (same model, same world-matrix case, own skeleton present).
//   2. The palette binding 0x0054D926 is hooked: for such a draw it writes the bones of all k meshes, read from
//      the same fields the engine reads (model bone list fxinfo+0x14, skeleton pivots + 0x30, 28 bytes each).
//   3. The buffers: the engine keeps binding its own vertex and index buffer. In our device hooks that binding
//      is swapped for a twin holding k copies. Copy 0 is the original byte for byte, so a single draw from the
//      twin is the single draw from the original - the swap is invisible to everything else.
//
// Nothing is merged until the palette code has reproduced the engine's own palette, byte for byte, for 3,000
// single draws; a twin is only built for a model whose buffers pass every check below; and any surprise turns
// merging off for the session. [Engine] MergeDraws = 0 leaves the engine alone; Ctrl+F11 switches it live.

#define MG_FLUSH_AT    0x00573D0C
#define MG_FLUSH_LEN   24
#define MG_PAL_FN      0x0054D926            // MeshToJointToWorld binding
#define MG_NJ_FN       0x0054D8C9            // NumJoints binding
#define MG_PALM_FN     0x0054DC62            // the same palette as 3x4 matrices (48 bytes an entry) - not merged
#define MG_PALV_FN     0x0054E04A            // and its third variant - not merged
#define MG_FXI_DTOR    0x0058BD88            // FX mesh info destructor
#define MG_DEVICE      0x00DD3474            // the engine's device pointer
#define MG_WRAP_VT     0x00BDCB44            // FX shader wrapper the flush calls
#define MG_INNER_VT    0x00BE9B84            // the effect object behind it
#define MG_PAL_ENGINE  0x00DD90F8            // the engine's palette scratch: 100 entries of 32 bytes
#define MG_PAL_MAX     100
#define MG_MAXK        16
#define MG_N           4096
#define MG_AN          8192

static const BYTE kMgFlushOrig[MG_FLUSH_LEN] = {
    0x8B,0x45,0x08,              // mov eax, [ebp+8]         next
    0x2B,0xC7,                   // sub eax, edi             - cur
    0xC1,0xF8,0x05,              // sar eax, 5               meshes left
    0x39,0x45,0xD4,              // cmp [ebp-2Ch], eax       maxInstances
    0x89,0x45,0xF0,              // mov [ebp-10h], eax
    0x8D,0x45,0xD4,              // lea eax, [ebp-2Ch]
    0x7C,0x03,                   // jl  +3
    0x8D,0x45,0xF0,              // lea eax, [ebp-10h]
    0x8B,0x08 };                 // mov ecx, [eax]           n
static const BYTE kMgPalOrig[11] = { 0x55, 0x8D,0x6C,0x24,0x90, 0x81,0xEC,0x9C,0x00,0x00,0x00 };
static const BYTE kMgNjOrig[9]   = { 0x55, 0x8B,0xEC, 0x8B,0x51,0x1C, 0x85,0xD2, 0x56 };
static const BYTE kMgDtorOrig[10] = { 0xB8,0x47,0xCA,0xB7,0x00, 0xE8,0x5E,0x11,0x4B,0x00 };
static const BYTE kMgPalMOrig[5] = { 0xB8,0xC6,0xAB,0xB7,0x00 }, kMgPalVOrig[5] = { 0xB8,0xDD,0xAB,0xB7,0x00 };

enum { MG_FREE = 0, MG_SEEN, MG_QUEUED, MG_READY, MG_REFUSED };
struct MgEnt {
    DWORD fxinfo, model;                     // key (fxinfo == 0: never used, 0xFFFFFFFF: was used)
    DWORD vb0, ib0, vbM, ibM;                // the engine's buffers and their multi-copy twins
    DWORD V, P, bytes, lastFrame;
    WORD  B, K, stride, idxOff, maxRun, runs;
    BYTE  state, why, idx32, pad;
    DWORD gen;                               // which use of this slot (a build answers only the one that asked)
};
struct MgAl { DWORD from, to; };
struct MgCtx { LONG armed, live, single; DWORD n, B, total, fxinfo; DWORD mesh[MG_MAXK]; };

static MgEnt  g_mgE[MG_N];
static MgAl   g_mgAlA[MG_AN], g_mgAlB[MG_AN];
static MgAl* volatile g_mgAl = g_mgAlA;
static MgCtx  g_mgCtx;
static float  g_mgPal[MG_PAL_MAX][8];        // zero filled once: the word behind each position stays 0, as the engine leaves it
static LONG   g_mgInstalled = 0;
static volatile LONG g_mgOn = 0, g_mgProven = 0;
static LONG   g_mgKMax = 8;
static DWORD  g_mgBudget = 48u << 20, g_mgBytes = 0;
static LONG   g_mgPend[256];
static LONG   g_mgPendCount = 0, g_mgJobsOut = 0;   // game thread: entries waiting for a build / builds on the render thread
static DWORD  g_mgGen = 0;
static BYTE*  g_mgTmp = NULL; static DWORD g_mgTmpCap = 0;
static struct { DWORD fx, h, elems; } g_mgFx[256];
static DWORD  g_mgBlock[256];                // effects seen using a matrix palette: never merged under
static volatile LONG g_mgProofOk = 0, g_mgProofBad = 0, g_mgFaults = 0, g_mgReady = 0, g_mgBuilt = 0, g_mgDropped = 0;
static volatile LONG g_mgGroups = 0, g_mgMeshes = 0, g_mgSingles = 0, g_mgRefusedN[20], g_mgStaleN = 0, g_mgToggles = 0;
static const char* g_mgOff = NULL;           // why merging was switched off, if it was

typedef void (__fastcall* tMgBind)(void* self, void* edx, void* fx, DWORD h);
typedef void (__fastcall* tMgDtor)(void* self, void* edx);
static tMgBind o_mgPal = NULL, o_mgNj = NULL, o_mgPalM = NULL, o_mgPalV = NULL;
static tMgDtor o_mgDtor = NULL;

static void mgSwitchOff(const char* why) {
    if (InterlockedExchange(&g_mgOn, 0)) { g_mgOff = why; logf("merge: OFF for this session - %s. Every mesh is its own draw again.", why); }
}

// ---------------------------------------------------------------- the buffer swap
static __forceinline DWORD mgAlHash(DWORD b) { return (b * 2654435761u) >> 19; }
static DWORD mgAlias(DWORD b) {
    const MgAl* t = g_mgAl;
    DWORD h = mgAlHash(b);
    for (int i = 0; i < 16; ++i) {
        const MgAl& e = t[(h + i) & (MG_AN - 1)];
        if (!e.from) return b;
        if (e.from == b) return e.to;
    }
    return b;
}
static void mgAliasRebuild() {                               // game thread; readers keep the old table until the swap
    MgAl* t = (g_mgAl == g_mgAlA) ? g_mgAlB : g_mgAlA;
    memset(t, 0, sizeof(g_mgAlA));
    LONG live = 0;
    for (int i = 0; i < MG_N; ++i) {
        MgEnt& e = g_mgE[i];
        if (e.state != MG_READY) continue;
        const DWORD pair[2][2] = { { e.vb0, e.vbM }, { e.ib0, e.ibM } };
        for (int p = 0; p < 2; ++p) {
            DWORD h = mgAlHash(pair[p][0]);
            for (int k = 0; k < 16; ++k) {
                MgAl& a = t[(h + k) & (MG_AN - 1)];
                if (!a.from) { a.to = pair[p][1]; a.from = pair[p][0]; break; }
            }
        }
        ++live;
    }
    g_mgAl = t;
    g_mgReady = live;
    InterlockedExchange(&g_mgAny, live ? 1 : 0);
}
static MgEnt* mgFind(DWORD fxinfo, DWORD model, bool create) {
    DWORD h = (fxinfo * 2654435761u) >> 20;
    MgEnt* spare = NULL;
    for (int i = 0; i < 32; ++i) {
        MgEnt& e = g_mgE[(h + i) & (MG_N - 1)];
        if (e.fxinfo == fxinfo && e.state != MG_FREE) return &e;
        if (e.state == MG_FREE) { if (!spare) spare = &e; if (!e.fxinfo) break; }
    }
    if (!create || !spare) return NULL;
    memset(spare, 0, sizeof(*spare));
    spare->fxinfo = fxinfo; spare->model = model; spare->state = MG_SEEN; spare->gen = ++g_mgGen;
    return spare;
}
static void mgRelease(MgEnt& e) {                            // game thread: hand the twins back through the queue
    if (e.vbM) rt_vb_2((void*)(ULONG_PTR)e.vbM);
    if (e.ibM) rt_ib_2((void*)(ULONG_PTR)e.ibM);
    if (g_mgBytes >= e.bytes) g_mgBytes -= e.bytes; else g_mgBytes = 0;
    e.vbM = e.ibM = 0; e.bytes = 0;
}
static void mgForget(DWORD fxinfo) {                         // the engine is destroying this FX mesh info
    DWORD h = (fxinfo * 2654435761u) >> 20;
    for (int i = 0; i < 32; ++i) {
        MgEnt& e = g_mgE[(h + i) & (MG_N - 1)];
        if (!e.fxinfo) return;
        if (e.fxinfo != fxinfo || e.state == MG_FREE) continue;
        bool was = e.state == MG_READY;
        if (g_mgCtx.fxinfo == fxinfo) { g_mgCtx.armed = 0; g_mgCtx.live = 0; g_mgCtx.single = 0; }
        e.state = MG_FREE; e.fxinfo = 0xFFFFFFFF;
        if (was) { mgAliasRebuild(); mgRelease(e); g_mgDropped++; }      // the swap goes first, then the twins
        return;
    }
}
static void mgJobsSettle();
static void mgDropAll(const char* why) {                     // game thread: device reset
    LONG n = 0;
    mgJobsSettle();                                          // twins still being built must not outlive the reset either
    for (int i = 0; i < MG_N; ++i) if (g_mgE[i].state == MG_READY) { g_mgE[i].state = MG_SEEN; g_mgE[i].runs = 0; ++n; }
    g_mgCtx.armed = 0; g_mgCtx.live = 0;
    mgAliasRebuild();
    for (int i = 0; i < MG_N; ++i) if (g_mgE[i].vbM || g_mgE[i].ibM) mgRelease(g_mgE[i]);
    if (n) logf("merge: %d multi-copy buffers released (%s); they are rebuilt as the models are drawn again.", (int)n, why);
}
static void mgOnWriteLock(DWORD buf) {                       // the engine is about to rewrite a buffer that has a twin
    if (mgAlias(buf) == buf) return;
    if (__readfsdword(0x24) != g_rtMainTid) { mgSwitchOff("a swapped buffer was locked from another thread"); return; }
    for (int i = 0; i < MG_N; ++i) {
        MgEnt& e = g_mgE[i];
        if (e.state != MG_READY || (e.vb0 != buf && e.ib0 != buf)) continue;
        e.state = MG_REFUSED; e.why = 16; g_mgRefusedN[16]++; g_mgStaleN++;
        if (g_mgCtx.fxinfo == e.fxinfo) { g_mgCtx.armed = 0; g_mgCtx.live = 0; }
        mgAliasRebuild(); mgRelease(e);
    }
}

// ---------------------------------------------------------------- the palette of one mesh, as the engine builds it
// 0x0054D926, bone mode: for each 16-bit index in the model's bone list, copy 7 dwords from skeleton pivot + 0x30.
static __forceinline DWORD mgHTreeOf(DWORD mesh) {
    DWORD cont = *(DWORD*)(ULONG_PTR)(mesh + 0x7C);
    if (!cont) return 0;
    typedef DWORD (__fastcall* tGet)(void*, void*);
    return ((tGet)(*(void***)(ULONG_PTR)cont)[0xE8 / 4])((void*)(ULONG_PTR)cont, NULL);     // 0x0059E740 for a model: [cont+0xF8]
}
static bool mgPalOf(DWORD mesh, DWORD lb, DWORD B, float* out) {
    DWORD ht = mgHTreeOf(mesh);
    if (!ht) return false;
    LONG npiv = *(LONG*)(ULONG_PTR)(ht + 0x10);
    DWORD piv = *(DWORD*)(ULONG_PTR)(ht + 0x14);
    if (!piv) return false;
    for (DWORD j = 0; j < B; ++j) {
        LONG idx = *(short*)(ULONG_PTR)(lb + j * 2);
        if (idx < 0 || idx >= npiv) return false;
        const DWORD* s = (const DWORD*)(ULONG_PTR)(piv + (DWORD)idx * 0x58 + 0x30);
        DWORD* d = (DWORD*)(out + j * 8);
        d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; d[4] = s[4]; d[5] = s[5]; d[6] = s[6];
    }
    return true;
}
static void mgLearnPalette(void* fx, DWORD h) {              // how many entries this effect's palette array has
    DWORD k = (((DWORD)(ULONG_PTR)fx) >> 4) & 255;
    if (g_mgFx[k].fx == (DWORD)(ULONG_PTR)fx && g_mgFx[k].h == h) return;
    BYTE desc[0x40]; memset(desc, 0, sizeof(desc));
    typedef LONG (__stdcall* tDesc)(void*, DWORD, BYTE*);
    if (((tDesc)(*(void***)fx)[4])(fx, h, desc) < 0) return;
    DWORD el = *(DWORD*)(desc + 0x18);
    g_mgFx[k].elems = el > MG_PAL_MAX ? 0 : el;              // the engine writes nothing into an array above 100
    g_mgFx[k].h = h; g_mgFx[k].fx = (DWORD)(ULONG_PTR)fx;
}
static DWORD mgPaletteSize(DWORD shader) {                   // the effect behind the flush's shader, without touching it
    if (*(DWORD*)(ULONG_PTR)shader != MG_WRAP_VT) return 0;
    DWORD inner = *(DWORD*)(ULONG_PTR)(shader + 8);
    if (!inner || *(DWORD*)(ULONG_PTR)inner != MG_INNER_VT) return 0;
    DWORD obj = *(DWORD*)(ULONG_PTR)(inner + 8);
    if (!obj) return 0;
    DWORD x = *(DWORD*)(ULONG_PTR)(obj + 0x14);
    if (!x) return 0;
    DWORD fx = *(DWORD*)(ULONG_PTR)(x + 4);
    if (!fx) return 0;
    DWORD k = (fx >> 4) & 255;
    if (g_mgBlock[k] == fx) return 0;                        // it also feeds a matrix palette, which is not written here
    return g_mgFx[k].fx == fx ? g_mgFx[k].elems : 0;
}
// 1 = written (or the engine would have written nothing either), 0 = let the engine's own code run
static int mgPalMerged(DWORD self, void* fx, DWORD h) {
    MgCtx& c = g_mgCtx;
    __try {
        DWORD vec = *(DWORD*)(ULONG_PTR)(self + 0x1C);
        if (!vec) return 0;
        DWORD cnt = (*(DWORD*)(ULONG_PTR)(vec + 4) - *(DWORD*)(ULONG_PTR)vec) / 0x30;
        DWORD list = *(DWORD*)(ULONG_PTR)(self + 0x14);
        if (cnt != c.n || !list || list != c.fxinfo + 0x14) { mgSwitchOff("the draw that followed a merge decision was not that group"); return 0; }
        DWORD lb = *(DWORD*)(ULONG_PTR)list, le = *(DWORD*)(ULONG_PTR)(list + 4);
        if (((le - lb) >> 1) != c.B) { mgSwitchOff("a model's bone list changed size"); return 0; }
        BYTE desc[0x40]; memset(desc, 0, sizeof(desc));
        typedef LONG (__stdcall* tDesc)(void*, DWORD, BYTE*);
        if (((tDesc)(*(void***)fx)[4])(fx, h, desc) < 0) return 1;
        DWORD el = *(DWORD*)(desc + 0x18);
        if (!el || el > MG_PAL_MAX) return 1;
        if (c.total > el) { mgSwitchOff("a shader's palette turned out smaller than recorded"); return 0; }
        for (DWORD i = 0; i < c.n; ++i)
            if (!mgPalOf(c.mesh[i], lb, c.B, g_mgPal[i * c.B])) { mgSwitchOff("a mesh in a merged group lost its skeleton"); return 0; }
        typedef LONG (__stdcall* tRaw)(void*, DWORD, const void*, DWORD, DWORD);
        ((tRaw)(*(void***)fx)[78])(fx, h, g_mgPal, 0, c.total * 32);
        g_mgGroups++; g_mgMeshes += (LONG)c.n;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_mgFaults++; mgSwitchOff("the palette writer faulted"); return 0; }
}
// The proof: a single draw of a skinned mesh, the engine has just written its palette - ours must be the same bytes.
static void mgProve(DWORD self, DWORD mesh, DWORD fxinfo) {
    __try {
        if (*(DWORD*)(ULONG_PTR)(self + 0x1C) || !*(DWORD*)(ULONG_PTR)(self + 0x10)) return;      // not bone mode
        DWORD list = *(DWORD*)(ULONG_PTR)(self + 0x14);
        if (list != fxinfo + 0x14) return;                                                         // not this mesh's draw
        DWORD lb = *(DWORD*)(ULONG_PTR)list, le = *(DWORD*)(ULONG_PTR)(list + 4);
        DWORD B = (le - lb) >> 1;
        if (!B || B > MG_PAL_MAX) return;
        if (!mgPalOf(mesh, lb, B, g_mgPal[0])) return;
        if (memcmp(g_mgPal, (const void*)MG_PAL_ENGINE, B * 32) == 0) {
            if (InterlockedIncrement(&g_mgProofOk) == 3000 && !g_mgProofBad) {
                InterlockedExchange(&g_mgProven, 1);
                logf("merge: proven - our palette matched the engine's own for 3,000 single draws, byte for byte. Runs of the same skinned model now share a draw call.");
            }
        } else if (InterlockedIncrement(&g_mgProofBad) == 1) {
            logf("merge: a palette of ours did not match the engine's (mesh %08X, %u bones) - merging stays off.", mesh, B);
            mgSwitchOff("the palette proof failed");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_mgFaults++; }
}
static void mgLearnGuarded(void* fx, DWORD h) {
    __try { mgLearnPalette(fx, h); } __except (EXCEPTION_EXECUTE_HANDLER) { g_mgFaults++; }
}
static void __fastcall mgPalHook(void* self, void* edx, void* fx, DWORD h) {
    MgCtx& c = g_mgCtx;
    if (c.armed) {
        c.armed = 0;
        if (mgPalMerged((DWORD)(ULONG_PTR)self, fx, h)) return;
    }
    o_mgPal(self, edx, fx, h);
    if (c.single) {
        c.single = 0;
        if (g_mgOn && !g_mgProven && !g_mgProofBad) mgProve((DWORD)(ULONG_PTR)self, c.mesh[0], c.fxinfo);
    }
    if (g_mgInstalled) mgLearnGuarded(fx, h);
}
static void __fastcall mgNjHook(void* self, void* edx, void* fx, DWORD h) {      // NumJoints: the palette entries in use
    MgCtx& c = g_mgCtx;
    if (c.live && *(DWORD*)((BYTE*)self + 0x1C)) {
        typedef LONG (__stdcall* tInt)(void*, DWORD, DWORD);
        ((tInt)(*(void***)fx)[26])(fx, h, c.total);
        return;
    }
    o_mgNj(self, edx, fx, h);
}
// The other two palette bindings build 48-byte matrix entries. A merged draw is only correct with the quaternion
// palette above, so an effect that uses either of these is remembered and never merged under; should a merged draw
// reach one anyway, merging ends for the session.
static __forceinline void mgOtherPalette(void* fx) {
    g_mgBlock[(((DWORD)(ULONG_PTR)fx) >> 4) & 255] = (DWORD)(ULONG_PTR)fx;
    if (g_mgCtx.armed) { g_mgCtx.armed = 0; g_mgCtx.live = 0; mgSwitchOff("a shared draw reached a shader with a matrix palette"); }   // armed = planned and not yet written
}
static void __fastcall mgPalMHook(void* self, void* edx, void* fx, DWORD h) { mgOtherPalette(fx); o_mgPalM(self, edx, fx, h); }
static void __fastcall mgPalVHook(void* self, void* edx, void* fx, DWORD h) { mgOtherPalette(fx); o_mgPalV(self, edx, fx, h); }
static void __fastcall mgDtorHook(void* self, void* edx) {
    if (g_mgInstalled) mgForget((DWORD)(ULONG_PTR)self);
    o_mgDtor(self, edx);
}

// ---------------------------------------------------------------- may this mesh share a draw with the run's first?
// Everything 0x0054A1F0 reads from the MESH on the skinned path: the model, the "relative to another object"
// pointer at +0x310 (only the identity case is merged), the index base at +0x300, the container and its skeleton.
static __forceinline bool mgMeshOk(DWORD mesh, DWORD model) {
    if (*(DWORD*)(ULONG_PTR)(mesh + 0xC4) != model) return false;
    DWORD o = *(DWORD*)(ULONG_PTR)(mesh + 0x310);
    if (o) { DWORD t = *(DWORD*)(ULONG_PTR)o; if (t && t != mesh) return false; }
    if (*(WORD*)(ULONG_PTR)(mesh + 0x300) != 0) return false;
    return mgHTreeOf(mesh) != 0;
}
static void mgQueue(MgEnt* e) {
    if (g_mgPendCount >= 256) return;
    e->state = MG_QUEUED;
    g_mgPend[g_mgPendCount++] = (LONG)(e - g_mgE);
    InterlockedExchange(&g_mgPendN, g_mgPendCount + g_mgJobsOut);
}
static DWORD mgPlan(DWORD remaining, DWORD cur, DWORD shader) {
    MgCtx& c = g_mgCtx;
    DWORD mesh0 = *(DWORD*)(ULONG_PTR)cur;
    DWORD model = *(DWORD*)(ULONG_PTR)(mesh0 + 0xC4);
    if (!model || !(*(BYTE*)(ULONG_PTR)(model + 0x19) & 4)) return 1;              // not a skinned model
    DWORD fxi = *(DWORD*)(ULONG_PTR)(model + 0xBC);
    if (!fxi || *(BYTE*)(ULONG_PTR)fxi) return 1;                                  // skinned on the CPU: its own buffer
    c.mesh[0] = mesh0; c.fxinfo = fxi; c.single = 1;                               // (the palette hook proves on single draws)
    if (remaining < 2 || !g_mgProven) return 1;
    if (!mgMeshOk(mesh0, model)) return 1;
    MgEnt* e = mgFind(fxi, model, true);
    if (!e || e->state == MG_REFUSED) return 1;
    DWORD capK = e->state == MG_READY ? e->K : (DWORD)g_mgKMax;
    DWORD r = 1;
    while (r < remaining && r < capK) {
        DWORD m = *(DWORD*)(ULONG_PTR)(cur + r * 0x20);
        if (!mgMeshOk(m, model)) break;
        c.mesh[r++] = m;
    }
    if (r < 2) return 1;
    if (r > e->maxRun) e->maxRun = (WORD)r;
    if (!g_rtInstalled) return 1;                                                  // the buffer swap lives in the device hooks
    if (e->state == MG_SEEN) { if (++e->runs >= 3) mgQueue(e); return 1; }         // seen in a run three times: worth a twin
    if (e->state != MG_READY) return 1;
    // the engine's buffers must still be the ones the twins were made from
    DWORD vbo = *(DWORD*)(ULONG_PTR)(fxi + 0xC), ibo = *(DWORD*)(ULONG_PTR)(fxi + 0x10);
    if (!vbo || !ibo || *(DWORD*)(ULONG_PTR)(vbo + 0x1C) != e->vb0 || *(DWORD*)(ULONG_PTR)(ibo + 0x14) != e->ib0 ||
        *(DWORD*)(ULONG_PTR)(model + 0x28) != e->V || *(DWORD*)(ULONG_PTR)(model + 0x24) != e->P) {
        e->state = MG_REFUSED; e->why = 17; g_mgRefusedN[17]++;
        mgAliasRebuild(); mgRelease(*e);
        return 1;
    }
    DWORD elems = mgPaletteSize(shader);
    DWORD k = elems / e->B;
    if (k > r) k = r;
    if (k < 2) return 1;
    c.n = k; c.B = e->B; c.total = k * e->B;
    c.single = 0; c.live = 1; c.armed = 1;
    e->lastFrame = (DWORD)g_mkFrames;
    return k;
}
// From the flush, in place of n = min(maxInstances, meshes left).
static DWORD __stdcall mgDecide(DWORD maxInst, DWORD next, DWORD cur, DWORD shader) {
    LONG left = (LONG)(next - cur) >> 5;
    DWORD n = ((LONG)maxInst < left) ? maxInst : (DWORD)left;                      // the engine's own answer
    MgCtx& c = g_mgCtx;
    c.armed = 0; c.live = 0; c.single = 0;
    if (!g_mgOn || maxInst != 1 || left < 1 || __readfsdword(0x24) != g_rtMainTid) return n;
    __try { n = mgPlan((DWORD)left, cur, shader); }
    __except (EXCEPTION_EXECUTE_HANDLER) { c.armed = 0; c.live = 0; c.single = 0; n = 1; if (InterlockedIncrement(&g_mgFaults) > 8) mgSwitchOff("the group planner kept faulting"); }
    if (n == 1) g_mgSingles++;
    return n;
}

// ---------------------------------------------------------------- building a twin
// The game thread only gathers what it reads from the engine's own objects and queues ONE record; the render
// thread does the Direct3D part when that record's turn comes (read the engine's buffers, create the twins, fill
// them). Nobody waits: the game thread picks the result up at a later Present. The engine's buffers cannot be
// freed in between, because their Release goes through the same queue, behind our record.
static void mgRefuse(MgEnt* e, int why) { e->state = MG_REFUSED; e->why = (BYTE)why; g_mgRefusedN[why]++; }
struct MgJob {
    volatile LONG state;                     // 0 free, 1 queued, 2 built, 3 refused
    DWORD entry, fxinfo, gen;
    void* vb0; void* ib0; void* decl; void* dev;
    DWORD V, P, B, stride, K, jpv;           // read on the game thread (jpv: joints per vertex, 1 or 2)
    DWORD vbM, ibM, idxOff, bytes, isz, why; // answered by the render thread
};
#define MG_JOBS 8
static MgJob g_mgJob[MG_JOBS];
static bool mgTmp(DWORD n) {                 // render thread only
    if (g_mgTmpCap >= n) return true;
    if (g_mgTmp) VirtualFree(g_mgTmp, 0, MEM_RELEASE);
    DWORD cap = (n + 0xFFFF) & ~0xFFFFu;
    g_mgTmp = (BYTE*)VirtualAlloc(NULL, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_mgTmpCap = g_mgTmp ? cap : 0;
    return g_mgTmp != NULL;
}
typedef LONG (__stdcall* tMgLock)(void*, DWORD, DWORD, void**, DWORD);
typedef LONG (__stdcall* tMgUnlock)(void*);
typedef LONG (__stdcall* tMgDesc)(void*, BYTE*);
typedef LONG (__stdcall* tMgCreate)(void*, DWORD, DWORD, DWORD, DWORD, void**, void*);
typedef LONG (__stdcall* tMgDecl)(void*, BYTE*, UINT*);
static int mgJobBuild(MgJob* j) {            // render thread, in queue order; returns 0 or the reason it was refused
    void* vbM = NULL; void* ibM = NULL;
    const DWORD V = j->V, P = j->P, B = j->B, stride = j->stride;
    BYTE vd[32], id[32]; memset(vd, 0, sizeof(vd)); memset(id, 0, sizeof(id));
    if (((tMgDesc)g_rtO_vb[13])(j->vb0, vd) < 0 || ((tMgDesc)g_rtO_ib[13])(j->ib0, id) < 0) return 7;
    DWORD vUsage = *(DWORD*)(vd + 8), vPool = *(DWORD*)(vd + 12), vSize = *(DWORD*)(vd + 16), vFvf = *(DWORD*)(vd + 20);
    DWORD iFmt = *(DWORD*)id, iUsage = *(DWORD*)(id + 8), iPool = *(DWORD*)(id + 12), iSize = *(DWORD*)(id + 16);
    if ((vUsage | iUsage) & 0x200) return 8;                                                  // dynamic: rewritten every frame
    DWORD isz = iFmt == 101 ? 2 : iFmt == 102 ? 4 : 0;
    if (!isz) return 9;
    if (vSize < V * stride || iSize < P * 3 * isz) return 10;
    // where the joint index sits in a vertex: the declaration's BLENDINDICES element of stream 0
    BYTE el[8 * 80]; UINT nel = 0; memset(el, 0, sizeof(el));
    if (((tMgDecl)(*(void***)j->decl)[4])(j->decl, el, &nel) < 0 || nel > 70) return 11;
    int idxOff = -1;
    for (UINT i = 0; i < nel; ++i) {
        const BYTE* x = el + i * 8;
        WORD stream = *(const WORD*)x, off = *(const WORD*)(x + 2);
        if (stream == 0xFF) break;
        if (stream == 0 && x[6] == 2 && x[7] == 0) {                                          // D3DDECLUSAGE_BLENDINDICES, index 0
            if (x[4] != 4 && x[4] != 5) return 12;                                            // D3DCOLOR or UBYTE4: one byte per joint
            idxOff = off;
        }
    }
    if (idxOff < 0 || (DWORD)idxOff + 4 > stride) return 12;
    DWORD K = j->K;
    if (isz == 2 && K > 65535 / V) K = 65535 / V;
    if (K < 2) return 13;
    DWORD vBytes = V * stride, iBytes = P * 3 * isz;
    if (!mgTmp(vBytes + iBytes)) return 15;
    // the engine's bytes
    void* p = NULL;
    if (((tMgLock)g_rtO_vb[11])(j->vb0, 0, 0, &p, 0x10) < 0 || !p) return 7;
    memcpy(g_mgTmp, p, vBytes);
    ((tMgUnlock)g_rtO_vb[12])(j->vb0);
    p = NULL;
    if (((tMgLock)g_rtO_ib[11])(j->ib0, 0, 0, &p, 0x10) < 0 || !p) return 7;
    memcpy(g_mgTmp + vBytes, p, iBytes);
    ((tMgUnlock)g_rtO_ib[12])(j->ib0);
    // The joint index is a DWORD the builder at 0x0058C05A wrote: the palette slot for one joint per vertex,
    // slot2 << 8 | slot1 for two. Every slot has to be below B and nothing else may be in there.
    const DWORD jpv = j->jpv;
    for (DWORD v = 0; v < V; ++v) {
        DWORD d = *(DWORD*)(g_mgTmp + v * stride + idxOff);
        if (jpv == 1) { if (d >= B) return 19; }
        else if ((d & 0xFF) >= B || ((d >> 8) & 0xFF) >= B || (d >> 16)) return 19;
    }
    const DWORD step = jpv == 1 ? B : B * 0x101u;                                             // what one copy adds to that DWORD
    const BYTE* ix = g_mgTmp + vBytes;
    for (DWORD i = 0; i < P * 3; ++i) {
        DWORD a = isz == 2 ? *(const WORD*)(ix + i * 2) : *(const DWORD*)(ix + i * 4);
        if (a >= V) return 19;
    }
    // the twins: same usage, format and pool as the engine's own
    if (((tMgCreate)g_rtO_dev[26])(j->dev, K * vBytes, vUsage, vFvf, vPool, &vbM, NULL) < 0 || !vbM) return 15;
    if (((tMgCreate)g_rtO_dev[27])(j->dev, K * iBytes, iUsage, iFmt, iPool, &ibM, NULL) < 0 || !ibM) { ((tMgUnlock)g_rtO_vb[2])(vbM); return 15; }
    bool ok = false;
    p = NULL;
    if (((tMgLock)g_rtO_vb[11])(vbM, 0, 0, &p, 0) >= 0 && p) {
        for (DWORD c = 0; c < K; ++c) {
            BYTE* d = (BYTE*)p + c * vBytes;
            memcpy(d, g_mgTmp, vBytes);
            if (c) for (DWORD v = 0; v < V; ++v) *(DWORD*)(d + v * stride + idxOff) += c * step;   // copy c reads palette entries c*B .. c*B+B-1
        }
        ((tMgUnlock)g_rtO_vb[12])(vbM);
        p = NULL;
        if (((tMgLock)g_rtO_ib[11])(ibM, 0, 0, &p, 0) >= 0 && p) {
            for (DWORD c = 0; c < K; ++c) {
                BYTE* d = (BYTE*)p + c * iBytes;
                if (!c) { memcpy(d, ix, iBytes); continue; }
                if (isz == 2) for (DWORD i = 0; i < P * 3; ++i) ((WORD*)d)[i] = (WORD)(((const WORD*)ix)[i] + c * V);
                else          for (DWORD i = 0; i < P * 3; ++i) ((DWORD*)d)[i] = ((const DWORD*)ix)[i] + c * V;
            }
            ((tMgUnlock)g_rtO_ib[12])(ibM);
            ok = true;
        }
    }
    if (!ok) { ((tMgUnlock)g_rtO_vb[2])(vbM); ((tMgUnlock)g_rtO_ib[2])(ibM); return 15; }
    j->vbM = (DWORD)(ULONG_PTR)vbM; j->ibM = (DWORD)(ULONG_PTR)ibM;
    j->K = K; j->idxOff = (DWORD)idxOff; j->isz = isz; j->bytes = K * (vBytes + iBytes);
    return 0;
}
static DWORD __stdcall mgJobExec(DWORD index) {               // the queued record lands here, on the render thread
    MgJob* j = &g_mgJob[index & (MG_JOBS - 1)];
    int why = 15;
    __try { why = mgJobBuild(j); } __except (EXCEPTION_EXECUTE_HANDLER) { why = 15; g_mgFaults++; }
    j->why = (DWORD)why;
    InterlockedExchange(&j->state, why ? 3 : 2);
    return 0;
}
// game thread: what the build needs from the engine's objects, checked as far as it can be without Direct3D
static int mgJobPrepare(MgEnt* e, MgJob* j) {
    __try {
        DWORD fxi = e->fxinfo, model = e->model;
        if (*(BYTE*)(ULONG_PTR)fxi) return 1;
        DWORD decl = *(DWORD*)(ULONG_PTR)(fxi + 8), vbo = *(DWORD*)(ULONG_PTR)(fxi + 0xC), ibo = *(DWORD*)(ULONG_PTR)(fxi + 0x10);
        if (!decl || !vbo || !ibo) return 2;
        DWORD lb = *(DWORD*)(ULONG_PTR)(fxi + 0x14), le = *(DWORD*)(ULONG_PTR)(fxi + 0x18);
        DWORD B = (le - lb) >> 1;
        DWORD jpv = *(DWORD*)(ULONG_PTR)(fxi + 0x20);
        if ((jpv != 1 && jpv != 2) || !lb || B < 1 || B > MG_PAL_MAX / 2) return 3;                   // one or two joints per vertex
        DWORD V = *(DWORD*)(ULONG_PTR)(model + 0x28), P = *(DWORD*)(ULONG_PTR)(model + 0x24);
        if (!V || !P || V > 32767 || P > 65535) return 4;
        if (!*(BYTE*)(ULONG_PTR)(vbo + 0x18)) return 5;                                       // described by an FVF, not a declaration
        DWORD fvi = *(DWORD*)(ULONG_PTR)(vbo + 0x14);
        DWORD stride = fvi ? *(DWORD*)(ULONG_PTR)(fvi + 0xC) : 0;
        void* vb0 = *(void**)(ULONG_PTR)(vbo + 0x1C); void* ib0 = *(void**)(ULONG_PTR)(ibo + 0x14);
        if (!vb0 || !ib0 || stride < 16 || stride > 256) return 6;
        if (mgAlias((DWORD)(ULONG_PTR)vb0) != (DWORD)(ULONG_PTR)vb0 || mgAlias((DWORD)(ULONG_PTR)ib0) != (DWORD)(ULONG_PTR)ib0) return 18;   // another model owns these buffers
        DWORD K = (DWORD)g_mgKMax;
        if (K > MG_PAL_MAX / B) K = MG_PAL_MAX / B;
        if (K > MG_MAXK) K = MG_MAXK;
        if (K < 2) return 13;
        if (g_mgBytes + K * (V * stride + P * 6) > g_mgBudget) return 14;
        void* dev = *(void**)MG_DEVICE;
        if (!dev) return 7;
        j->entry = (DWORD)(e - g_mgE); j->fxinfo = fxi; j->gen = e->gen;
        j->vb0 = vb0; j->ib0 = ib0; j->decl = (void*)(ULONG_PTR)decl; j->dev = dev;
        j->V = V; j->P = P; j->B = B; j->stride = stride; j->K = K; j->jpv = jpv;
        j->vbM = j->ibM = 0; j->idxOff = j->bytes = j->isz = j->why = 0;
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_mgFaults++; return 15; }
}
static void mgJobsSettle() {                                 // game thread, ahead of a device reset
    if (!g_mgJobsOut) return;
    rtDrain();                                               // every build that was queued has run now
    for (int i = 0; i < MG_JOBS; ++i) {
        MgJob& j = g_mgJob[i];
        LONG st = j.state;
        if (st != 2 && st != 3) continue;
        if (st == 2) { rt_vb_2((void*)(ULONG_PTR)j.vbM); rt_ib_2((void*)(ULONG_PTR)j.ibM); }
        MgEnt* e = &g_mgE[j.entry & (MG_N - 1)];
        if (e->state == MG_QUEUED && e->fxinfo == j.fxinfo && e->gen == j.gen) { e->state = MG_SEEN; e->runs = 0; }
        InterlockedExchange(&j.state, 0);
        g_mgJobsOut--;
    }
    InterlockedExchange(&g_mgPendN, g_mgPendCount + g_mgJobsOut);
}
static void mgFrameTick() {                                  // game thread, at Present
    static LONG lastFrame = -1;
    if (!g_mgInstalled || !g_rtInstalled || __readfsdword(0x24) != g_rtMainTid) return;
    if (lastFrame == g_mkFrames) return;
    lastFrame = g_mkFrames;
    bool changed = false;
    // 1. twins the render thread has finished
    for (int i = 0; i < MG_JOBS; ++i) {
        MgJob& j = g_mgJob[i];
        LONG st = j.state;
        if (st != 2 && st != 3) continue;
        MgEnt* e = &g_mgE[j.entry & (MG_N - 1)];
        bool mine = e->state == MG_QUEUED && e->fxinfo == j.fxinfo && e->gen == j.gen;
        if (st == 2) {
            if (mine && g_mgBytes + j.bytes <= g_mgBudget) {
                e->vb0 = (DWORD)(ULONG_PTR)j.vb0; e->ib0 = (DWORD)(ULONG_PTR)j.ib0; e->vbM = j.vbM; e->ibM = j.ibM;
                e->V = j.V; e->P = j.P; e->B = (WORD)j.B; e->K = (WORD)j.K; e->stride = (WORD)j.stride; e->idxOff = (WORD)j.idxOff; e->idx32 = j.isz == 4;
                e->bytes = j.bytes; g_mgBytes += j.bytes; e->lastFrame = (DWORD)g_mkFrames;
                e->state = MG_READY; g_mgBuilt++; changed = true;
            } else {                                         // the model went away meanwhile, or the budget filled up
                rt_vb_2((void*)(ULONG_PTR)j.vbM); rt_ib_2((void*)(ULONG_PTR)j.ibM);
                if (mine) mgRefuse(e, 14);
            }
        } else if (mine) mgRefuse(e, (int)j.why);
        InterlockedExchange(&j.state, 0);
        g_mgJobsOut--;
    }
    if (changed) mgAliasRebuild();
    // 2. new ones, while the queue is running and there is a free slot (a few a frame)
    int started = 0;
    while (g_mgOn && g_mgPendCount > 0 && started < 4 && g_rtActive && !g_rtDirectScope) {
        int slot = -1;
        for (int i = 0; i < MG_JOBS; ++i) if (!g_mgJob[i].state) { slot = i; break; }
        if (slot < 0) break;
        MgEnt* e = &g_mgE[g_mgPend[--g_mgPendCount] & (MG_N - 1)];
        if (e->state != MG_QUEUED) continue;
        MgJob& j = g_mgJob[slot];
        int why = mgJobPrepare(e, &j);
        if (why) { mgRefuse(e, why); continue; }
        InterlockedExchange(&j.state, 1);
        g_mgJobsOut++;
        RtRec* r = rtAlloc(1, 0); r->fn = (void*)&mgJobExec; r->op = 341;
        rtArgs(r)[0] = (DWORD)slot;
        rtCommit(r, 0);
        ++started;
    }
    InterlockedExchange(&g_mgPendN, g_mgPendCount + g_mgJobsOut);
}
// ---------------------------------------------------------------- what a frame costs, in the ordinary log
// The stage and phase timers run in every build; only their report was a diagnostic. One line every five seconds
// says where the game thread's frame goes and how busy the render thread is, so the effect of a change is read in
// milliseconds from a normal battle. The mesh flush is timed here (two clock reads per render list).
#define MG_FLUSH_FN 0x00573BEB
static const BYTE kMgFlushFnOrig[5] = { 0xB8,0x8C,0xBF,0xB7,0x00 };              // mov eax, 0x00B7BF8C (EH prolog argument)
typedef void (__fastcall* tMgFlush)(void* self, void* edx, DWORD begin, DWORD end);
static tMgFlush o_mgFlush = NULL;
static volatile LONG64 g_mgFlushTicks = 0;
static volatile LONG g_mgFlushN = 0, g_mgFlushElems = 0;
static void __fastcall mgFlushHook(void* self, void* edx, DWORD begin, DWORD end) {
    LONG64 t0 = qpcNow();
    o_mgFlush(self, edx, begin, end);
    g_mgFlushTicks += qpcNow() - t0;
    g_mgFlushN++; g_mgFlushElems += (LONG)((end - begin) >> 5);
}
static void mgMs(LONG64 ticks, LONG frames, char* out) {                         // ticks over frames -> "12.3" (ms a frame)
    LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
    LONG64 x10 = frames > 0 ? (ticks * 10000) / (f * frames) : 0;
    wsprintfA(out, "%d.%d", (int)(x10 / 10), (int)(x10 % 10));
}
static DWORD WINAPI mgBudget(LPVOID) {
    LONG lF = 0, lFl = 0, lEl = 0; LONG64 l[8]; memset(l, 0, sizeof(l));
    for (;;) {
        Sleep(5000);
        LONG f = g_mkFrames, df = f - lF; lF = f;
        LONG64 c[8] = { rd64(&g_phTicks[PH_LOGIC]), rd64(&g_phTicks[PH_CLIENT]), rd64(&g_stTicks[0]), rd64(&g_stTicks[1]), rd64(&g_stTicks[2]),
                        rd64(&g_mgFlushTicks), rd64(&g_rtBusyTicks), rd64(&g_rtDrainTicks) };
        LONG64 d[8]; for (int i = 0; i < 8; ++i) { d[i] = c[i] - l[i]; l[i] = c[i]; }
        LONG fl = g_mgFlushN, el = g_mgFlushElems, dfl = fl - lFl, del = el - lEl; lFl = fl; lEl = el;
        if (df < 50) continue;                                                    // menus, loading
        LONG64 clientOwn = d[1] - d[2] - d[3] - d[4]; if (clientOwn < 0) clientOwn = 0;
        char a[8][16];
        mgMs(d[0], df, a[0]); mgMs(clientOwn, df, a[1]); mgMs(d[2], df, a[2]); mgMs(d[3], df, a[3]); mgMs(d[4], df, a[4]);
        mgMs(d[5], df, a[5]); mgMs(d[6], df, a[6]); mgMs(d[7], df, a[7]);
        char tot[16]; mgMs(d[0] + d[1], df, tot);
        logf("budget: %d.%d fps | game thread %s ms a frame = logic %s + client %s + shadow pass %s + views %s + ui %s | of the passes, the mesh flush is %s "
             "(%d meshes in %d lists a frame) | render thread busy %s, game thread waited on it %s | shared draws %s",
             (int)(df / 5), (int)((df % 5) * 2), tot, a[0], a[1], a[2], a[3], a[4], a[5], (int)(del / df), (int)(dfl / df), a[6], a[7],
             g_mgInstalled ? (g_mgOn ? (g_mgProven ? "ON" : "proving") : "off") : "not installed");
        // how even the frames were: the same five seconds by frame length (the edges are the ones the frame timer has always used)
        { static LONG lFh[9]; LONG fh[9]; LONG tot = 0;
          for (int k = 0; k < 9; ++k) { LONG v = g_frHist[k]; fh[k] = v - lFh[k]; lFh[k] = v; tot += fh[k]; }
          LONG mx = InterlockedExchange(&g_frMaxTenths, 0);
          logf("frames: %d in these 5 s by length - under 20 ms %d, 20-25 %d, 25-33 %d, 33-40 %d, 40-50 %d, 50-67 %d, 67-100 %d, 100-200 %d, over 200 %d | the longest %d.%d ms",
               (int)tot, (int)fh[0], (int)fh[1], (int)fh[2], (int)fh[3], (int)fh[4], (int)fh[5], (int)fh[6], (int)fh[7], (int)fh[8], (int)(mx / 10), (int)(mx % 10)); }
        // every 30 s of play: the logic share split by module type (those timers run in every build as well)
        static LONG acc = 0, ticks = 0;
        acc += df;
        if (++ticks >= 6) { if (acc >= 300) mtReport(acc); acc = 0; ticks = 0; }
    }
}
static void installBudget() {
    __try {
        if (!memcmp((void*)MG_FLUSH_FN, kMgFlushFnOrig, sizeof(kMgFlushFnOrig))) {
            o_mgFlush = (tMgFlush)makeTrampoline((BYTE*)MG_FLUSH_FN, 5);
            if (o_mgFlush) {
                HANDLE fz[256]; int nf = 0;
                suspendOthers(fz, &nf, 256);
                patchJmp((BYTE*)MG_FLUSH_FN, (void*)&mgFlushHook, 5);
                resumeAll(fz, nf);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { o_mgFlush = NULL; }
    CreateThread(NULL, 0, mgBudget, NULL, 0, NULL);
    logf("budget: one line every five seconds of play says where the game thread's frame goes%s.", o_mgFlush ? ", the mesh flush included" : " (the flush itself is not timed - its entry differs)");
}
static void mgToggle() {                                     // key thread
    if (!g_mgInstalled) return;
    if (g_mgOff) { logf("merge: Ctrl+F11 - it switched itself off earlier (%s) and stays off this session.", g_mgOff); return; }
    LONG was = InterlockedExchange(&g_mgOn, g_mgOn ? 0 : 1);
    g_mgToggles++;
    logf("merge: Ctrl+F11 - shared draw calls %s.", was ? "OFF (every mesh its own draw, as the engine ships)" : "ON");
}
static DWORD WINAPI mgReport(LPVOID) {
    LONG lG = 0, lM = 0, lS = 0, lF = 0;
    static const char* kWhy[20] = { "", "cpu-skinned", "no buffers", "joints per vertex", "size", "FVF buffer", "odd buffer", "D3D refused",
        "dynamic", "index format", "buffer too small", "declaration", "no joint index", "one copy only", "memory budget", "allocation", "engine rewrote it",
        "engine replaced it", "buffer shared", "contents" };
    for (;;) {
        Sleep(5000);
        LONG g = g_mgGroups, m = g_mgMeshes, s = g_mgSingles, f = g_mkFrames;
        LONG dg = g - lG, dm = m - lM, ds = s - lS, df = f - lF; lG = g; lM = m; lS = s; lF = f;
        if (df <= 0 || (!dg && !ds)) continue;
        char why[400]; int q = 0; why[0] = 0;
        for (int i = 1; i < 20; ++i) if (g_mgRefusedN[i]) q += wsprintfA(why + q, " %s %d", kWhy[i], (int)g_mgRefusedN[i]);
        logf("merge: %s | per frame: %d shared draws carrying %d meshes, %d single draws -> %d draw calls instead of %d | models with multi-copy buffers %d (%d KB), "
             "built %d, dropped %d | proof %d matched, %d wrong | faults %d |%s%s",
             g_mgOn ? (g_mgProven ? "ON" : "proving") : "off", (int)(dg / df), (int)(dm / df), (int)(ds / df), (int)((dg + ds) / df), (int)((dm + ds) / df),
             (int)g_mgReady, (int)(g_mgBytes >> 10), (int)g_mgBuilt, (int)g_mgDropped, (int)g_mgProofOk, (int)g_mgProofBad, (int)g_mgFaults,
             q ? " models left alone:" : "", why);
    }
}

static void installMerge() {
    char ini[MAX_PATH];
    aotrPath(ini, "bfme2_accel.ini");
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;
    if (GetPrivateProfileIntA("Engine", "MergeDraws", 0, ini) <= 0) return;
    int k = (int)GetPrivateProfileIntA("Engine", "MergeMaxMeshes", 8, ini);
    if (k < 2) k = 2;
    if (k > MG_MAXK) k = MG_MAXK;
    int mb = (int)GetPrivateProfileIntA("Engine", "MergeBudgetMB", 48, ini);
    if (mb < 4) mb = 4;
    if (mb > 256) mb = 256;
    __try {
        if (memcmp((void*)MG_FLUSH_AT, kMgFlushOrig, MG_FLUSH_LEN) || memcmp((void*)MG_PAL_FN, kMgPalOrig, sizeof(kMgPalOrig)) ||
            memcmp((void*)MG_NJ_FN, kMgNjOrig, sizeof(kMgNjOrig)) || memcmp((void*)MG_FXI_DTOR, kMgDtorOrig, sizeof(kMgDtorOrig)) ||
            memcmp((void*)MG_PALM_FN, kMgPalMOrig, sizeof(kMgPalMOrig)) || memcmp((void*)MG_PALV_FN, kMgPalVOrig, sizeof(kMgPalVOrig))) {
            logf("merge: the render list flush does not look like the build this was written for - off.");
            return;
        }
        o_mgPal = (tMgBind)makeTrampoline((BYTE*)MG_PAL_FN, 5);
        o_mgNj = (tMgBind)makeTrampoline((BYTE*)MG_NJ_FN, 6);
        o_mgDtor = (tMgDtor)makeTrampoline((BYTE*)MG_FXI_DTOR, 5);
        o_mgPalM = (tMgBind)makeTrampoline((BYTE*)MG_PALM_FN, 5);
        o_mgPalV = (tMgBind)makeTrampoline((BYTE*)MG_PALV_FN, 5);
        if (!o_mgPal || !o_mgNj || !o_mgDtor || !o_mgPalM || !o_mgPalV) { logf("merge: no trampolines - off."); return; }
        BYTE patch[MG_FLUSH_LEN];
        memset(patch, 0x90, sizeof(patch));
        patch[0] = 0x56;                                             // push esi            the batch's shader
        patch[1] = 0x57;                                             // push edi            first mesh of the group
        patch[2] = 0xFF; patch[3] = 0x75; patch[4] = 0x08;           // push [ebp+8]        end of the batch
        patch[5] = 0xFF; patch[6] = 0x75; patch[7] = 0xD4;           // push [ebp-2Ch]      the model's own capacity
        patch[8] = 0xE8;                                             // call mgDecide
        *(DWORD*)(patch + 9) = (DWORD)(ULONG_PTR)&mgDecide - (MG_FLUSH_AT + 13);
        patch[13] = 0x8B; patch[14] = 0xC8;                          // mov ecx, eax        n, where the engine expects it
        g_mgKMax = k; g_mgBudget = (DWORD)mb << 20;
        HANDLE fz[256]; int nf = 0;
        suspendOthers(fz, &nf, 256);
        DWORD op = 0;
        bool ok = VirtualProtect((void*)MG_FLUSH_AT, MG_FLUSH_LEN, PAGE_EXECUTE_READWRITE, &op) != 0;
        if (ok) {
            patchJmp((BYTE*)MG_PAL_FN, (void*)&mgPalHook, 5);
            patchJmp((BYTE*)MG_NJ_FN, (void*)&mgNjHook, 6);
            patchJmp((BYTE*)MG_FXI_DTOR, (void*)&mgDtorHook, 5);
            patchJmp((BYTE*)MG_PALM_FN, (void*)&mgPalMHook, 5);
            patchJmp((BYTE*)MG_PALV_FN, (void*)&mgPalVHook, 5);
            memcpy((void*)MG_FLUSH_AT, patch, MG_FLUSH_LEN);
            VirtualProtect((void*)MG_FLUSH_AT, MG_FLUSH_LEN, op, &op);
            FlushInstructionCache(GetCurrentProcess(), (void*)MG_FLUSH_AT, MG_FLUSH_LEN);
        }
        resumeAll(fz, nf);
        if (!ok) { logf("merge: the flush is not writable - off."); return; }
        g_mgInstalled = 1; InterlockedExchange(&g_mgOn, 1);
        logf("merge: installed - runs of the same skinned model inside a batch will share a draw call (up to %d meshes, %d MB of multi-copy buffers). "
             "First our palette has to match the engine's own on 3,000 single draws; Ctrl+F11 switches it live.", k, mb);
        CreateThread(NULL, 0, mgReport, NULL, 0, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf("merge: install faulted - off.");
    }
}

// ---------------------------------------------------------------- offline proof (src/merge_test.cpp)
// Builds a twin for buffers the test made, by the same queued record the game uses, and registers the swap, so the
// test can draw k meshes one by one and then as ONE draw and compare the two pictures pixel for pixel on the game's
// own Direct3D. Returns the number of copies, or minus the reason the build was refused.
extern "C" __declspec(dllexport) int __cdecl AotrMgTestBuild(void* dev, void* vb0, void* ib0, void* decl, DWORD V, DWORD P, DWORD B, DWORD stride, DWORD K, DWORD key, DWORD jpv) {
    MgJob& j = g_mgJob[0];
    memset(&j, 0, sizeof(j));
    j.vb0 = vb0; j.ib0 = ib0; j.decl = decl; j.dev = dev; j.V = V; j.P = P; j.B = B; j.stride = stride; j.K = K; j.jpv = jpv;
    InterlockedExchange(&j.state, 1);
    if (g_rtActive) {
        RtRec* r = rtAlloc(1, 0); r->fn = (void*)&mgJobExec; r->op = 341;
        rtArgs(r)[0] = 0;
        rtCommit(r, 0);
        rtDrain();
    } else mgJobExec(0);
    if (j.state != 2) { int why = (int)j.why; InterlockedExchange(&j.state, 0); return -why; }
    MgEnt* e = mgFind(key, 0, true);
    if (!e) { InterlockedExchange(&j.state, 0); return -100; }
    e->vb0 = (DWORD)(ULONG_PTR)vb0; e->ib0 = (DWORD)(ULONG_PTR)ib0; e->vbM = j.vbM; e->ibM = j.ibM;
    e->V = V; e->P = P; e->B = (WORD)B; e->K = (WORD)j.K; e->stride = (WORD)stride; e->idxOff = (WORD)j.idxOff; e->bytes = j.bytes;
    e->state = MG_READY;
    mgAliasRebuild();
    InterlockedExchange(&j.state, 0);
    return (int)j.K;
}
extern "C" __declspec(dllexport) void __cdecl AotrMgTestSwap(int on) {           // the device hooks swap the buffers / leave them alone
    if (on) mgAliasRebuild(); else InterlockedExchange(&g_mgAny, 0);
}
