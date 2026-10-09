// ================================================================ the second pass at the device (v63, 2026-10-08)
// The in-between picture (aotr_tween.inc) is the frame's draw stream run a second time. Until now "run" meant that
// every effect call of the stream went to D3DX again. D3DX's effect code is the largest thing on the render thread -
// about half of everything it executes in a battle - and in the second pass most of it is a repetition: the pass
// state D3DX sets up for a batch (32 device calls for the game's main effect), the material it copies in from a
// parameter block, the shaders it picks, most of the preshader it runs come out the same the second time, because
// they do not depend on what the second pass changes. What the second pass changes is where things ARE: joint
// palettes and object transforms, and, when the camera moves, the camera.
//
// So the render thread listens while it executes the frame the first time (aotr_rt.inc, DvRec): for every effect
// call inside the marked stretch - SetTechnique, Begin, BeginPass, CommitChanges, EndPass, End - it keeps the device
// calls D3DX makes. The second pass issues those device calls itself, at the same places of the stream, and leaves
// D3DX alone: no parameter is written, no block applied, no pass opened. When the pass is over D3DX is exactly as the
// game left it (the old way had to write every blended value back).
//
// Where a blended value goes. A parameter the second pass holds a value of its own for (twPalette / twMatrix) is kept
// in a small table. When a recorded call uploads float constants, the registers of that upload that the value
// reaches are replaced. Which registers those are is read from the shader that is bound (aotr_shtab.inc; the reading
// is checked against D3DX's own disassembly for every shader and every preshader the game's effects bind):
//   * STRAIGHT FROM REGISTERS. The shader's constant table names every parameter it reads directly, and where. A
//     joint palette goes in as it is (what SetRawValue was given is what the registers get); a matrix goes register
//     by register in D3DX's layout (a column-major float4x3 written with SetMatrixTranspose: the first three rows of
//     what was passed; with SetMatrix: its first three columns - measured, src/fxprobe.cpp "image").
//   * THROUGH THE PRESHADER. A shader may carry a small program D3DX runs on the CPU, whose results go into further
//     registers. In the game's unit effects the camera goes that way: 28 instructions multiply View by Projection
//     into four registers. The program is in the shader's PRES block, with a table naming its inputs. Here it is
//     analysed per component: which outputs depend on a parameter that holds a blended value, and is everything on
//     the way to them a multiplication or an addition of values this code knows (parameters whose current value is in
//     the table, literals)? Then those outputs are computed - on the x87, as doubles, one operation at a time, the
//     way an interpreter compiled for the x87 does it and under the render thread's own control word - once for the
//     stream's own values and once for the blended ones.
// Two rules keep that honest:
//   * THE GUARD. Before anything is replaced, the bytes the first pass uploaded there are compared with what this
//     code derives from the stream's own value: the same registers of the value itself, or the preshader outputs
//     computed from it. They must be equal, bit for bit. That is the proof, for this very upload, that the registers
//     are where the tables say, laid out as assumed and (for a preshader) computed as D3DX computes them. If they
//     are not equal the pass is stopped.
//   * NOTHING ELSE. A blended parameter that reaches a preshader output through any other instruction, or together
//     with a value this code does not hold, stops the pass.
// A stopped pass is started again from the mark and made by D3DX as before; nothing of the attempt is left behind
// (it never touched D3DX, and the device is put back to the mark anyway).
//
// Proof.
//   * Offline: rt_harness.exe (a second pass made this way with nothing blended must be the game's frame, byte for
//     byte), tween_test.exe (blended pictures must be the pictures the test draws itself from the halfway values;
//     with deliberate faults switched in - AOTR_DEVPASS_BREAK - it must fail) and devpass_test.exe (the game's own
//     compiled effect, driven as the engine drives it: every picture must be the picture a run through D3DX makes).
//   * In the game, VERIFICATION: a frame's second pass is made by D3DX while the render thread listens again, and
//     at every draw the registers D3DX has given each blended parameter (and each preshader output that depends on
//     one) must be exactly the registers this code would have given it - computed from the value, and issued from
//     the recording - and the shaders bound must be the same. The first frames are verified before any is made the
//     new way; so are the first frames that blend anything, and the first with a moving camera; and one in every
//     DV_AUDIT after that. One difference switches the new way off for the session and says so in the log.
// [Engine] DevicePass (default 1), AOTR_DEVPASS; the "devpass:" line of the log.
#include "aotr_shtab.inc"

#define DV_NAMES      1024
#define DV_NAMEPOOL   32768
#define DV_SHN        2048
#define DV_VALN       512
#define DV_LIVE       128
#define DV_ARENA      (2u << 20)
#define DV_PREARENA   (3u << 20)
#define DV_FNMAX      (64u << 10)
#define DV_PREOUT     64
#define DV_PROVE      6                      // clean verified frames before the first one made the new way
#define DV_PROVE_REGS 256                    // blended registers verified before the first blended upload is issued
#define DV_PROVE_PRE  64                     // preshader outputs verified before the first computed one is issued
#define DV_AUDIT      512                    // after that: one frame in so many is verified
#define DV_UNKNOWN    0xFFFFFFFFu            // a shader binding from before the mark

enum { DV_AB_NONE = 0, DV_AB_CHUNK, DV_AB_CALL, DV_AB_PRES, DV_AB_TABLE, DV_AB_LAYOUT, DV_AB_GUARD, DV_AB_LIVE, DV_AB_UNPROVEN, DV_AB_N };
static const char* const kDvAb[DV_AB_N] = { "", "an effect call had no recording", "a recorded call cannot be issued", "a blended parameter feeds a preshader in a way that is not computed here",
    "a shader's tables could not be read", "a blended parameter is laid out in a way that is not known", "the recorded registers were not what the stream's own value gives",
    "too many blended parameters at once", "blended registers had not been verified yet" };

struct DvShC { WORD name, reg, cnt; BYTE cls, rows, cols, fl; };                     // fl: float registers
struct DvVal;
struct DvPreOut { WORD off; float orig, own; };                                    // an output component (register off / 4, component off % 4): from the stream's values, from this pass's
struct DvPre {                               // a shader's preshader
    WORD nIn, nIns, nLit, bad;
    DvShC* in; ShtIns* ins; double* lit;
    DvVal** inVal; DWORD* inSeq;             // the parameters' current values, as found for this pass
    DWORD rStamp, rMade, cStamp; BYTE cOk; WORD cN; DvPreOut cOut[DV_PREOUT];
};
struct DvSh { DWORD fx, ptr, seq; BYTE ok, hasPre; WORD nMain; DvShC main[SHT_MAXC]; DvPre* prog; };
struct DvVal { DWORD fx, h, stamp, wseq; WORD name; BYTE cls, slot, sub, live; const BYTE* orig; DWORD nb; BYTE* buf; DWORD cap;
               DWORD memoSeq[2]; const DvShC* memoC[2]; BYTE memoPre[2]; };
struct DvS {                                 // render thread: what lasts longer than a pass
    LONG ready, initFailed, gen; DWORD shSeq, wseq, valMade;
    char* namePool; DWORD nameUsed; WORD* nameIdx;
    DvSh* sh; DvVal* val; BYTE* arena; DWORD arenaUsed; BYTE* preArena; DWORD preUsed;
    BYTE* fn; ShtTab* tab;
    float* xVsF; float* xPsF; float* dVsF; float* dPsF; BYTE* xSet;       // verification: the float registers as D3DX's second pass leaves them, and as the recording would
    BYTE* fl[3]; double* tb[3][2];           // preshader evaluation: per component of the input / temporary / output table its flags, and its value for the stream's parameters [0] and this pass's [1]
    LONG off;                                // a verification failed: not used again in this session
    LONG proven, subProven, preProven;       // clean verified frames; blended registers verified; computed preshader outputs verified
    LONG verifyNext;                         // this many of the next frames are verified
    LONG skip, skipLen;                      // after a stop: frames made by D3DX before the next attempt
    LONG camHold;                            // the last pass was stopped by the camera and the camera is still moving
    DWORD frameN;
    LONG test;                               // 0 the game; 2 always at the device; 3 always verified
    LONG failLogged;
};
struct DvPass {
    int mode, abort, stoppedFor; DWORD stamp;
    DWORD vs, ps, passFx;                    // what the stream has bound so far; the effect whose pass is open
    DvVal* live[DV_LIVE]; LONG nLive;
    LONG nChunks, nCalls, nPatched, nPre;
    DWORD shFx, shPtr[2]; const DvSh* shTab[2];       // the shader tables looked up last
    DWORD xvs, xps; LONG vDead, vWould, vDraws, vRegs, vPre, vUnset, vFail;
    BYTE scratch[1024 * 16];
};
static DvS g_dvs;
static DvPass g_dvp;
static volatile LONG g_dvNLive = 0, g_dvNVerify = 0, g_dvNVerifyClean = 0, g_dvNCam = 0, g_dvNSkip = 0, g_dvNNoRec = 0, g_dvNBadCall = 0, g_dvNBadFull = 0, g_dvNBadStray = 0, g_dvNOff = 0,
                     g_dvNFail = 0, g_dvNChunks = 0, g_dvNCalls = 0, g_dvNPatched = 0, g_dvNPre = 0, g_dvNRecFrames = 0, g_dvVDraws = 0, g_dvVRegs = 0, g_dvVPre = 0, g_dvVUnset = 0, g_dvLastBadSlot = 0;
static volatile LONG g_dvNStop[DV_AB_N], g_dvNWould[DV_AB_N];
static volatile LONG64 g_dvRecBytes = 0;
static void twExecAgain(RtRec* r);
#ifndef AOTR_PROD
static LONG g_dvBreak = 0;                   // test controls (AOTR_DEVPASS_BREAK): 1 a palette one register too far, 2 matrices the other way round and no guard, 3 nothing put into registers, 4 preshader outputs computed with the SSE unit instead of the x87 and no guard
#define DV_BREAK(n) (g_dvBreak == (n))
#else
#define DV_BREAK(n) 0
#endif

static bool dvInit() {                                    // render thread
    DvS& S = g_dvs; DvRec& d = g_dvr;
    if (S.ready) return true;
    if (S.initFailed) return false;
    #define DV_ALLOC(n) VirtualAlloc(NULL, (n), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
    d.a = (BYTE*)DV_ALLOC(DV_A_BYTES); d.x = (BYTE*)DV_ALLOC(DV_X_BYTES); d.stray = (BYTE*)DV_ALLOC(8192); d.chunk = (DvChunk*)DV_ALLOC(DV_CHUNKS * sizeof(DvChunk));
    S.namePool = (char*)DV_ALLOC(DV_NAMEPOOL); S.nameIdx = (WORD*)DV_ALLOC(DV_NAMES * sizeof(WORD));
    S.sh = (DvSh*)DV_ALLOC(DV_SHN * sizeof(DvSh)); S.val = (DvVal*)DV_ALLOC(DV_VALN * sizeof(DvVal)); S.arena = (BYTE*)DV_ALLOC(DV_ARENA); S.preArena = (BYTE*)DV_ALLOC(DV_PREARENA);
    S.fn = (BYTE*)DV_ALLOC(DV_FNMAX); S.tab = (ShtTab*)DV_ALLOC(sizeof(ShtTab));
    S.xVsF = (float*)DV_ALLOC(4 * 256 * 16); S.xSet = (BYTE*)DV_ALLOC(4096);
    S.fl[0] = (BYTE*)DV_ALLOC(3 * SHT_COMPS); S.tb[0][0] = (double*)DV_ALLOC(6 * SHT_COMPS * sizeof(double));
    #undef DV_ALLOC
    if (!d.a || !d.x || !d.stray || !d.chunk || !S.namePool || !S.nameIdx || !S.sh || !S.val || !S.arena || !S.preArena || !S.fn || !S.tab || !S.xVsF || !S.xSet || !S.fl[0] || !S.tb[0][0]) {
        S.initFailed = 1; InterlockedExchange(&g_dvOn, 0);
        logf("devpass: no memory for the recording - the second pass stays with D3DX.");
        return false;
    }
    S.xPsF = S.xVsF + 256 * 4; S.dVsF = S.xVsF + 512 * 4; S.dPsF = S.xVsF + 768 * 4;
    S.fl[1] = S.fl[0] + SHT_COMPS; S.fl[2] = S.fl[0] + 2 * SHT_COMPS;
    for (int t = 0; t < 3; ++t) for (int e = 0; e < 2; ++e) S.tb[t][e] = S.tb[0][0] + (t * 2 + e) * SHT_COMPS;
    S.nameUsed = 1; S.gen = g_twFxGen; S.ready = 1;
    return true;
}
// Everything that names an effect, a handle or a shader is forgotten when an effect is released: its address, its
// handles and its shaders' addresses may mean something else afterwards.
static void dvForget() {
    DvS& S = g_dvs; DvPass& P = g_dvp;
    memset(S.sh, 0, DV_SHN * sizeof(DvSh)); memset(S.val, 0, DV_VALN * sizeof(DvVal));
    S.arenaUsed = 0; S.preUsed = 0; P.nLive = 0; S.gen = g_twFxGen; S.valMade++;
}
static WORD dvName(const char* s) {                       // a parameter name -> its number (0: none)
    DvS& S = g_dvs;
    if (!s || !s[0]) return 0;
    DWORD h = 0x811C9DC5u, len = 0;
    for (const BYTE* p = (const BYTE*)s; *p && len < 200; ++p, ++len) h = (h ^ *p) * 0x01000193u;
    if (len >= 200) return 0;
    DWORD i = h & (DV_NAMES - 1);
    for (int k = 0; k < 64; ++k, i = (i + 1) & (DV_NAMES - 1)) {
        WORD o = S.nameIdx[i];
        if (!o) {
            if (S.nameUsed + len + 1 > DV_NAMEPOOL) return 0;
            o = (WORD)S.nameUsed; memcpy(S.namePool + o, s, len + 1); S.nameUsed += len + 1; S.nameIdx[i] = o;
            return o;
        }
        if (!strcmp(S.namePool + o, s)) return o;
    }
    return 0;
}
static WORD dvParamName(DWORD fx, DWORD h) {
    BYTE desc[64]; memset(desc, 0, sizeof(desc));
    __try {
        if ((LONG)((RtF3)g_rtO_fx[4])(fx, h, (DWORD)(ULONG_PTR)desc) < 0) return 0;
        return dvName(*(const char**)desc);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static bool dvConstCopy(const ShtConst& c, DvShC& o) {
    o.name = dvName(c.name);
    if (!o.name || c.rows > 255 || c.cols > 255 || c.cls > 255) return false;
    o.reg = c.reg; o.cnt = c.cnt; o.cls = (BYTE)c.cls; o.rows = (BYTE)c.rows; o.cols = (BYTE)c.cols; o.fl = c.set == 2 ? 1 : 0;
    return true;
}
// A shader's tables, read once from the function the device gives back for it. Keyed by the effect that bound it
// as well: an effect's shaders live exactly as long as the effect, so the pair cannot come to mean another shader
// without an effect having been released (dvForget).
static bool dvShaderFill(DvSh& s) {
    DvS& S = g_dvs;
    __try {
        typedef DWORD (__stdcall* GF)(DWORD, void*, UINT*);
        void** v = *(void***)(ULONG_PTR)s.ptr; UINT n = 0;
        if ((LONG)((GF)v[4])(s.ptr, NULL, &n) < 0 || n < 8 || n > DV_FNMAX || (LONG)((GF)v[4])(s.ptr, S.fn, &n) < 0) return false;
        ShtTab* t = S.tab;
        if (!shtParse((const DWORD*)S.fn, n, t)) return false;
        for (int i = 0; i < t->nMain; ++i) if (!dvConstCopy(t->main[i], s.main[i])) return false;
        s.nMain = (WORD)t->nMain; s.hasPre = t->hasPre ? 1 : 0; s.prog = NULL;
        if (t->hasPre) {
            const ShtProg& g = t->prog;
            DWORD nIns = g.ok ? (DWORD)g.nIns : 0, nLit = g.ok ? (DWORD)g.nLit : 0;
            DWORD need = sizeof(DvPre) + 8 + (DWORD)t->nPre * (sizeof(DvShC) + sizeof(DvVal*) + sizeof(DWORD)) + nIns * sizeof(ShtIns) + 8 + nLit * sizeof(double);
            need = (need + 15) & ~15u;
            if (S.preUsed + need > DV_PREARENA) return false;
            BYTE* m = S.preArena + S.preUsed; S.preUsed += need; memset(m, 0, need);
            DvPre* p = (DvPre*)m; m += (sizeof(DvPre) + 7) & ~7u;
            p->lit = (double*)m; m += nLit * sizeof(double);       // (the arena and the header are multiples of 8: the doubles are aligned)
            p->in = (DvShC*)m; m += (DWORD)t->nPre * sizeof(DvShC);
            p->inVal = (DvVal**)m; m += (DWORD)t->nPre * sizeof(DvVal*);
            p->inSeq = (DWORD*)m; m += (DWORD)t->nPre * sizeof(DWORD);
            p->ins = (ShtIns*)m;
            for (int i = 0; i < t->nPre; ++i) if (!dvConstCopy(t->pre[i], p->in[i])) return false;
            p->nIn = (WORD)t->nPre; p->nIns = (WORD)nIns; p->nLit = (WORD)nLit; p->bad = g.ok ? 0 : 1;
            if (nIns) memcpy(p->ins, g.ins, nIns * sizeof(ShtIns));
            if (nLit) memcpy(p->lit, g.lit, nLit * sizeof(double));
            s.prog = p;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static const DvSh* dvShader(DWORD fx, DWORD ptr) {
    DvS& S = g_dvs;
    DWORD i = (((ptr >> 4) * 2654435761u) ^ (fx * 2246822519u)) >> 21;
    for (int k = 0; k < 16; ++k, i = (i + 1) & (DV_SHN - 1)) {
        DvSh& s = S.sh[i];
        if (s.ptr == ptr && s.fx == fx) return &s;
        if (!s.ptr) { s.fx = fx; s.ptr = ptr; s.seq = ++S.shSeq; s.nMain = 0; s.hasPre = 0; s.prog = NULL; s.ok = dvShaderFill(s) ? 1 : 0; return &s; }
    }
    return NULL;
}
static __forceinline const DvSh* dvShaderAt(DWORD fx, int w, DWORD ptr) {    // the same, for the shader bound to stage w - usually the one asked about last
    DvPass& P = g_dvp;
    if (P.shFx == fx && P.shPtr[w] == ptr && P.shTab[w]) return P.shTab[w];
    if (P.shFx != fx) { P.shFx = fx; P.shPtr[0] = P.shPtr[1] = 0; P.shTab[0] = P.shTab[1] = NULL; }
    P.shPtr[w] = ptr; P.shTab[w] = dvShader(fx, ptr);
    return P.shTab[w];
}
static DvVal* dvValFind(DWORD fx, DWORD h, bool make) {
    DvS& S = g_dvs;
    DWORD i = (((fx >> 4) * 2654435761u) ^ (h * 2246822519u)) >> 23;
    for (int k = 0; k < 16; ++k, i = (i + 1) & (DV_VALN - 1)) {
        DvVal& v = S.val[i];
        if (v.fx == fx && v.h == h) return &v;
        if (!v.fx) { if (!make) return NULL; memset(&v, 0, sizeof(v)); v.fx = fx; v.h = h; v.name = dvParamName(fx, h); S.valMade++; return &v; }
    }
    return NULL;
}
// The table entry of a parameter the stream is writing, with room for this pass's own value. NULL: no room.
static DvVal* dvValFor(DWORD fx, DWORD h, DWORD cls, DWORD slot, DWORD nb) {
    DvS& S = g_dvs;
    DvVal* v = dvValFind(fx, h, true);
    DWORD need = cls == TWC_PALETTE ? 3200 : 64;
    if (!v || nb > need) return NULL;
    if (v->cap < need) { if (S.arenaUsed + need > DV_ARENA) return NULL; v->buf = S.arena + S.arenaUsed; v->cap = need; S.arenaUsed += need; }
    v->cls = (BYTE)cls; v->slot = (BYTE)slot;
    return v;
}
static void dvValSet(DvVal* v, const void* orig, DWORD nb, bool sub) {       // the stream wrote the parameter; sub: this pass holds another value for it (in v->buf)
    DvPass& P = g_dvp;
    if (!v) { if (sub && !P.abort) P.abort = DV_AB_LIVE; return; }
    v->orig = (const BYTE*)orig; v->nb = nb; v->sub = sub ? 1 : 0; v->stamp = P.stamp; v->wseq = ++g_dvs.wseq;
    if (sub && !v->live) { if (P.nLive < DV_LIVE) { P.live[P.nLive++] = v; v->live = 1; } else { v->sub = 0; if (!P.abort) P.abort = DV_AB_LIVE; } }
}
static void dvValPlain(DWORD fx, DWORD h, DWORD partial) {                  // the stream wrote the parameter some way that is not read here: its own value from now on, whatever that is
    DvPass& P = g_dvp;
    DvVal* v = dvValFind(fx, h, false);
    if (!v || v->stamp != P.stamp) return;
    if (v->sub && partial && !P.abort) P.abort = DV_AB_LAYOUT;              // part of a blended value replaced: what the registers hold now is a mix
    v->sub = 0; v->stamp = 0; v->wseq = ++g_dvs.wseq;                       // (no longer a value this code knows)
}
static __forceinline void dvResolve(DvVal* v, const DvSh* s, int w) {     // where shader s reads parameter v, and whether (as which input) its preshader does
    if (v->memoSeq[w] == s->seq) return;
    v->memoSeq[w] = s->seq; v->memoC[w] = NULL; v->memoPre[w] = 0;
    for (WORD i = 0; i < s->nMain; ++i) if (s->main[i].name == v->name) { v->memoC[w] = &s->main[i]; break; }
    if (s->prog) for (WORD i = 0; i < s->prog->nIn && i < 255; ++i) if (s->prog->in[i].name == v->name) { v->memoPre[w] = (BYTE)(i + 1); break; }
}
static __forceinline bool dvMatrixOk(const DvVal* v, const DvShC* c) {
    return c->fl && (c->cls == 2 || c->cls == 3) && c->rows >= 1 && c->rows <= 4 && c->cols >= 1 && c->cols <= 4 && v->nb == 64 && (v->slot == 38 || v->slot == 44);
}
static __forceinline bool dvLayoutOk(const DvVal* v, const DvShC* c) { return v->cls == TWC_PALETTE ? c->fl != 0 : dvMatrixOk(v, c); }
static __forceinline DWORD dvValRegs(const DvVal* v, const DvShC* c) {     // registers of the constant the value written last fills
    DWORD n = v->cls == TWC_PALETTE ? v->nb / 16 : (c->cls == 3 ? c->cols : c->rows);
    return n < c->cnt ? n : c->cnt;
}
// Register i of a matrix constant, for the 16 floats m as they were passed to SetMatrix (38) or SetMatrixTranspose
// (44). D3DX stores the matrix (transposed for 44) and gives a column-major constant its columns, a row-major one its
// rows, one register each; only as many components as the constant is declared with are the value's.
static __forceinline DWORD dvMatReg(const DvShC* c, DWORD slot, const float* m, DWORD i, float* out) {
    bool colMajor = c->cls == 3; DWORD comps = colMajor ? c->rows : c->cols;
    if (DV_BREAK(2)) slot = slot == 44 ? 38 : 44;
    for (DWORD k = 0; k < comps; ++k) {
        DWORD r = colMajor ? k : i, cc = colMajor ? i : k;                 // element (r, cc) of the stored matrix
        out[k] = slot == 44 ? m[cc * 4 + r] : m[r * 4 + cc];
    }
    return comps;
}

// ---------------------------------------------------------------- preshader outputs that depend on a blended parameter
// One operation, the way x87 code does it: operands loaded as doubles, the result rounded by the thread's own
// precision control and stored as a double. (The render thread runs under the game thread's control word - D3D
// sets single precision when the device is created - and so does D3DX's interpreter when it runs there.)
#pragma warning(push)
#pragma warning(disable: 4725 4740)
static double dvX87(WORD op, double a, double b) {
    double r;
#ifndef AOTR_PROD
    if (DV_BREAK(4)) return op == 0x205 ? a * b : a + b;
#endif
    if (op == 0x205) { __asm { fld qword ptr [a]
                               fmul qword ptr [b]
                               fstp qword ptr [r] } }
    else             { __asm { fld qword ptr [a]
                               fadd qword ptr [b]
                               fstp qword ptr [r] } }
    return r;
}
static float dvX87F(double a) { float r; __asm { fld qword ptr [a]
                                                 fstp dword ptr [r] }
                                return r; }
#pragma warning(pop)
// What shader s's preshader makes of the parameters of effect fx as they are right now. true: every output that
// depends on a blended parameter has been computed (g->cOut: from the stream's values and from this pass's).
static bool dvPreEval(const DvSh* s, DWORD fx) {
    DvPass& P = g_dvp; DvS& S = g_dvs; DvPre* g = s->prog;
    bool changed = false;
    if (g->rStamp != P.stamp || g->rMade != S.valMade) {                    // which table entries are its inputs (entries are made as the stream first writes a parameter)
        for (WORD i = 0; i < g->nIn; ++i) g->inVal[i] = NULL;
        for (DWORD k = 0; k < DV_VALN; ++k) { DvVal* v = &S.val[k];
            if (v->fx != fx || !v->name) continue;
            for (WORD i = 0; i < g->nIn; ++i) if (g->in[i].name == v->name && (!g->inVal[i] || v->wseq > g->inVal[i]->wseq)) g->inVal[i] = v; }
        g->rStamp = P.stamp; g->rMade = S.valMade; changed = true;
    }
    for (WORD i = 0; i < g->nIn; ++i) { DvVal* v = g->inVal[i]; DWORD q = (v && v->stamp == P.stamp) ? v->wseq : 0; if (q != g->inSeq[i]) { g->inSeq[i] = q; changed = true; } }
    if (!changed && g->cStamp == P.stamp) return g->cOk != 0;
    g->cStamp = P.stamp; g->cOk = 0; g->cN = 0;
    if (g->bad || !g->nIns) return false;
    // per component: bit 0 - depends on a parameter this pass holds another value for; bit 1 - not computed here
    BYTE* fc = S.fl[0]; BYTE* ft = S.fl[1]; BYTE* fo = S.fl[2];
    memset(fc, 2, SHT_COMPS); memset(ft, 2, SHT_COMPS); memset(fo, 0, SHT_COMPS);
    for (WORD i = 0; i < g->nIn; ++i) {
        DvVal* v = g->inVal[i]; const DvShC* c = &g->in[i];
        if (!v || v->stamp != P.stamp || v->cls < TWC_WORLD || !dvMatrixOk(v, c)) continue;                 // not a value this code holds: its components stay "not computed"
        DWORD n = dvValRegs(v, c);
        for (DWORD r = 0; r < n; ++r) {
            float a[4], b[4]; DWORD comps = dvMatReg(c, v->slot, (const float*)v->orig, r, a), off = ((DWORD)c->reg + r) * 4;
            dvMatReg(c, v->slot, (const float*)(v->sub ? v->buf : v->orig), r, b);
            if (off + 4 > SHT_COMPS) break;
            for (DWORD k = 0; k < comps; ++k) { fc[off + k] = v->sub ? 1 : 0; S.tb[0][0][off + k] = (double)a[k]; S.tb[0][1][off + k] = (double)b[k]; }
        }
    }
    bool lost = false;                                                     // a bool / int output depends on a blended parameter
    for (WORD n = 0; n < g->nIns; ++n) {
        const ShtIns& q = g->ins[n];
        bool sup = (q.op == 0x204 || q.op == 0x205) && q.nin == 2;
        for (WORD k = 0; k < q.ncomp; ++k) {
            BYTE f = sup ? 0 : 2; double a[2][2] = { { 0, 0 }, { 0, 0 } };
            for (BYTE j = 0; j < q.nin; ++j) {
                const ShtArg& g_ = q.in[j]; DWORD off = (DWORD)g_.off + ((q.scalar && j == 0) ? 0 : k);
                if (g_.idx || off >= SHT_COMPS) { f |= 2; continue; }
                if (g_.tab == 1) { if (off < g->nLit) { if (j < 2) a[j][0] = a[j][1] = g->lit[off]; } else f |= 2; }
                else if (g_.tab == 2) { f |= fc[off]; if (j < 2) { a[j][0] = S.tb[0][0][off]; a[j][1] = S.tb[0][1][off]; } }
                else if (g_.tab == 7) { f |= ft[off]; if (j < 2) { a[j][0] = S.tb[1][0][off]; a[j][1] = S.tb[1][1][off]; } }
                else f |= 2;
            }
            DWORD o = (DWORD)q.out.off + k;
            if (q.out.idx || o >= SHT_COMPS) { if (f & 1) lost = true; continue; }
            double r0 = 0, r1 = 0;
            if (!(f & 2)) { r0 = dvX87(q.op, a[0][0], a[1][0]); r1 = (f & 1) ? dvX87(q.op, a[0][1], a[1][1]) : r0; }
            if (q.out.tab == 7) { ft[o] = f; S.tb[1][0][o] = r0; S.tb[1][1][o] = r1; }
            else if (q.out.tab == 4) { fo[o] = f; S.tb[2][0][o] = r0; S.tb[2][1][o] = r1; }
            else if (f & 1) lost = true;
        }
    }
    if (lost) return false;
    for (DWORD o = 0; o < SHT_COMPS; ++o) {
        if (!(fo[o] & 1)) continue;
        if ((fo[o] & 2) || g->cN >= DV_PREOUT) { g->cN = 0; return false; }
        DvPreOut& x = g->cOut[g->cN++]; x.off = (WORD)o; x.orig = dvX87F(S.tb[2][0][o]); x.own = dvX87F(S.tb[2][1][o]);
    }
    g->cOk = 1;
    return true;
}

// ---------------------------------------------------------------- a recorded upload, as this pass must issue it
static __forceinline bool dvUnproven(LONG have, LONG need) {
    if (g_dvp.mode == 1 && !g_dvs.test && have < need) { g_dvp.abort = DV_AB_UNPROVEN; return true; }
    return false;
}
// Parameters read straight from registers. rec: what the first pass uploaded for registers start .. start + count.
// false: the pass stops (g_dvp.abort says why). *copied: the upload is now in g_dvp.scratch, with replacements.
static bool dvPatch(DvVal** vals, int nv, int w, DWORD start, DWORD count, const BYTE* rec, bool* copied) {
    DvPass& P = g_dvp; DvS& S = g_dvs;
    for (int i = 0; i < nv; ++i) {
        DvVal* v = vals[i]; const DvShC* c = v->memoC[w];
        if (!c) continue;
        DWORD n = dvValRegs(v, c), lo = start > c->reg ? start : c->reg, hi = start + count < (DWORD)c->reg + n ? start + count : (DWORD)c->reg + n;
        if (lo >= hi) continue;
        if (count > 1024) { P.abort = DV_AB_CALL; return false; }
        if (dvUnproven(S.subProven, DV_PROVE_REGS)) return false;
        if (DV_BREAK(3)) continue;
        if (!*copied) { memcpy(P.scratch, rec, count * 16); *copied = true; }
        const BYTE* src = rec + (lo - start) * 16; BYTE* dst = P.scratch + (lo - start) * 16;
        if (v->cls == TWC_PALETTE) {
            DWORD off = (lo - c->reg) * 16, len = (hi - lo) * 16;
            if (memcmp(src, v->orig + off, len)) { P.abort = DV_AB_GUARD; return false; }
            if (DV_BREAK(1)) { if (len > 16) memcpy(dst + 16, v->buf + off, len - 16); } else
            memcpy(dst, v->buf + off, len);
        } else {
            for (DWORD r = lo; r < hi; ++r, src += 16, dst += 16) {
                float a[4], b[4];
                DWORD comps = dvMatReg(c, v->slot, (const float*)v->orig, r - c->reg, a);
                dvMatReg(c, v->slot, (const float*)v->buf, r - c->reg, b);
                if (!DV_BREAK(2) && memcmp(src, a, comps * 4)) { P.abort = DV_AB_GUARD; return false; }
                memcpy(dst, b, comps * 4);
            }
        }
        P.nPatched++;
    }
    return true;
}
// An upload that is nothing but (the start of) one joint palette this pass holds its own value for - what every
// CommitChanges of a skinned mesh is: D3DX uploads the whole constant, 180 registers for the game's units, whatever
// part of it the mesh's palette fills. It is issued straight from the pass's value, for the registers that value
// fills, and nothing is copied. The registers behind it are not uploaded at all. They are the joints the mesh does
// not have; in the first pass they hold what earlier meshes left there, and in a second pass made by D3DX they hold
// what earlier meshes of THAT pass left there - another content already, and one the pictures have never depended
// on (the harness, tween_test and the in-game self-check compare them). The guard is asked first, as everywhere.
// 1: issued this way (*data, *cnt); 0: not such an upload; -1: the pass stops.
static int dvPaletteOnly(DvVal** vals, int nv, int w, DWORD start, DWORD count, const BYTE* rec, const void** data, DWORD* cnt) {
    DvPass& P = g_dvp; DvS& S = g_dvs;
    for (int i = 0; i < nv; ++i) {
        DvVal* v = vals[i]; const DvShC* c = v->memoC[w];
        if (!c || v->cls != TWC_PALETTE || start != c->reg || start + count > (DWORD)c->reg + c->cnt) continue;
        DWORD n = dvValRegs(v, c); if (n > count) n = count;
        if (!n || DV_BREAK(1) || DV_BREAK(3)) return 0;
        if (dvUnproven(S.subProven, DV_PROVE_REGS)) return -1;
        if (memcmp(rec, v->orig, n * 16)) { P.abort = DV_AB_GUARD; return -1; }
        *data = v->buf; *cnt = n; P.nPatched++;
        return 1;
    }
    return 0;
}
// Preshader outputs that depend on a blended parameter (g->cOut, from dvPreEval).
static bool dvPatchPre(const DvPre* g, DWORD start, DWORD count, const BYTE* rec, bool* copied) {
    DvPass& P = g_dvp; DvS& S = g_dvs;
    bool any = false;
    for (WORD i = 0; i < g->cN; ++i) {
        const DvPreOut& o = g->cOut[i]; DWORD reg = o.off >> 2, at = (reg - start) * 16 + (o.off & 3) * 4;
        if (reg < start || reg >= start + count) continue;
        if (count > 1024) { P.abort = DV_AB_CALL; return false; }
        if (dvUnproven(S.preProven, DV_PROVE_PRE)) return false;
        if (!DV_BREAK(4) && memcmp(rec + at, &o.orig, 4)) { P.abort = DV_AB_GUARD; return false; }       // computed from the stream's own values, it must be what D3DX uploaded
        if (DV_BREAK(3)) continue;
        if (!*copied) { memcpy(P.scratch, rec, count * 16); *copied = true; }
        memcpy(P.scratch + at, &o.own, 4);
        any = true;
    }
    if (any) P.nPre++;
    return true;
}
static __forceinline void dvShadowF(float* reg, BYTE* set, DWORD start, DWORD count, const void* data) {
    if (start >= 256 || count > 256 - start) { g_dvp.vDead = 1; return; }
    memcpy(reg + start * 4, data, count * 16);
    if (set) memset(set + start, 1, count);
}
// The device calls the first pass recorded for effect call r, as this pass must make them: issued (shadow false),
// or only applied to the register shadow that verification compares with D3DX's. false: the pass stops.
static bool dvRun(RtRec* r, DWORD dev, bool shadow) {
    DvRec& d = g_dvr; DvPass& P = g_dvp; DvS& S = g_dvs;
    DWORD k = r->pad - 1;
    if (k >= (DWORD)d.nChunk || d.chunk[k].rec != r) { P.abort = DV_AB_CHUNK; return false; }
    const DWORD* p = (const DWORD*)(d.a + d.chunk[k].off); const DWORD* e = (const DWORD*)((const BYTE*)p + d.chunk[k].bytes);
    DWORD fx = rtArgs(r)[0];
    if (r->op == 184) P.passFx = fx; else if (r->op == 186 || r->op == 187) P.passFx = 0;
    if (p == e) { P.nChunks++; return true; }
    DWORD vs = P.vs, ps = P.ps;
    for (const DWORD* q = p; q < e; q += q[0] >> 16) {                      // the shaders these calls leave bound
        DWORD s = q[0] & 0xFFFF;
        if (!(q[0] >> 16)) { P.abort = DV_AB_CALL; return false; }
        if (s == 92) vs = q[1]; else if (s == 107) ps = q[1];
    }
    DvVal* vals[24]; int nv = 0;
    for (LONG i = 0; i < P.nLive; ++i) { DvVal* v = P.live[i];
        if (!v->sub || v->fx != fx || v->stamp != P.stamp) continue;
        if (nv == 24) { P.abort = DV_AB_LIVE; return false; }
        vals[nv++] = v; }
    const DvSh* sh[2] = { NULL, NULL }; bool fed[2] = { false, false };
    if (nv) {                                                              // a blended value of this effect: what do the shaders make of it?
        DWORD ptr[2] = { vs, ps };
        for (int w = 0; w < 2; ++w) if (ptr[w] && ptr[w] != DV_UNKNOWN) { sh[w] = dvShaderAt(fx, w, ptr[w]); if (!sh[w] || !sh[w]->ok) { P.abort = DV_AB_TABLE; return false; } }
        for (int i = 0; i < nv; ++i) { DvVal* v = vals[i];
            if (!v->name) { P.abort = DV_AB_TABLE; return false; }
            for (int w = 0; w < 2; ++w) {
                if (!sh[w]) { v->memoSeq[w] = 0; v->memoC[w] = NULL; v->memoPre[w] = 0; continue; }
                dvResolve(v, sh[w], w);
                if (v->memoPre[w]) fed[w] = true;
                if (v->memoC[w] && !dvLayoutOk(v, v->memoC[w])) { P.abort = DV_AB_LAYOUT; return false; }
            } }
        for (int w = 0; w < 2; ++w) if (fed[w] && !dvPreEval(sh[w], fx)) { P.abort = DV_AB_PRES; return false; }
    }
    void** O = (void**)g_rtO_dev;
    for (; p < e; p += p[0] >> 16) {
        DWORD slot = p[0] & 0xFFFF;
        switch (slot) {
        case 94: case 109: {
            int w = slot == 94 ? 0 : 1; const BYTE* rec = (const BYTE*)(p + 3); bool copied = false;
            const void* data = rec; DWORD cnt = p[2];
            if (nv) {
                if ((w ? ps : vs) == DV_UNKNOWN) { P.abort = DV_AB_TABLE; return false; }       // constants for a shader bound before the mark: what reads them is not known
                if (sh[w]) {
                    int pal = dvPaletteOnly(vals, nv, w, p[1], p[2], rec, &data, &cnt);
                    if (pal < 0) return false;
                    if (!pal) { if (!dvPatch(vals, nv, w, p[1], p[2], rec, &copied)) return false;
                                if (fed[w] && !dvPatchPre(sh[w]->prog, p[1], p[2], rec, &copied)) return false;
                                if (copied) data = P.scratch; }
                }
            }
            if (shadow) dvShadowF(w ? S.dPsF : S.dVsF, NULL, p[1], cnt, data);
            else ((RtF4)O[slot])(dev, p[1], (DWORD)(ULONG_PTR)data, cnt);
            break; }
        case 0x200 | 94: {                                                   // the bytes SetRawValue was given, as CommitChanges uploaded them (see dvNoteC)
            DWORD start = p[1], count = p[2], n = p[4] / 16; const BYTE* pay = (const BYTE*)(ULONG_PTR)p[3];
            if (!vs || vs == DV_UNKNOWN) { P.abort = DV_AB_TABLE; return false; }
            const DvSh* s = sh[0] ? sh[0] : (sh[0] = dvShaderAt(fx, 0, vs));
            if (!s || !s->ok) { P.abort = DV_AB_TABLE; return false; }
            // The upload has to be one constant's registers and nothing else: then what lies behind the bytes written is
            // the same constant's - the joints this mesh does not have (see dvPaletteOnly) - and is left as it is.
            const DvShC* c = NULL;
            for (WORD i = 0; i < s->nMain; ++i) if (s->main[i].fl && s->main[i].reg == start && start + count <= (DWORD)s->main[i].reg + s->main[i].cnt) { c = &s->main[i]; break; }
            if (!c) { P.abort = DV_AB_CALL; return false; }
            if (n > count) n = count;
            const void* data = pay; DWORD at = start;
            for (int i = 0; i < nv; ++i) { DvVal* v = vals[i];                 // a value of this pass's own for that constant?
                if (v->memoC[0] != c) continue;
                if (v->cls != TWC_PALETTE || v->orig != pay || v->nb != p[4]) { P.abort = DV_AB_GUARD; return false; }      // the stream's value of the parameter is not what the first pass uploaded here
                if (dvUnproven(S.subProven, DV_PROVE_REGS)) return false;
                if (DV_BREAK(3)) break;
                data = v->buf; P.nPatched++;
                if (DV_BREAK(1) && n > 1) { at = start + 1; --n; }
                break; }
            if (shadow) dvShadowF(S.dVsF, NULL, at, n, data);
            else ((RtF4)O[94])(dev, at, (DWORD)(ULONG_PTR)data, n);
            break; }
        case 57: case 65: if (!shadow) ((RtF3)O[slot])(dev, p[1], p[2]); break;
        case 67: case 69: if (!shadow) ((RtF4)O[slot])(dev, p[1], p[2], p[3]); break;
        case 92: case 107: if (!shadow) ((RtF2)O[slot])(dev, p[1]); break;
        case 96: case 98: case 111: case 113: if (!shadow) ((RtF4)O[slot])(dev, p[1], (DWORD)(ULONG_PTR)(p + 3), p[2]); break;
        case 0x104: case 0x105: if (!shadow) ((RtF1)g_rtO_sb[slot & 0xFF])(p[1]); break;
        default: P.abort = DV_AB_CALL; return false;
        }
        P.nCalls++;
    }
    P.vs = vs; P.ps = ps; P.nChunks++;
    return true;
}
static __forceinline void dvShaderSet(DWORD op, DWORD ptr) {              // the stream itself binds a shader
    DvPass& P = g_dvp;
    if (op == 92) { P.vs = ptr; P.xvs = ptr; } else { P.ps = ptr; P.xps = ptr; }
}

// ---------------------------------------------------------------- verification
static void dvFail(int kind, const DvVal* v, const DvSh* s, DWORD reg) {
    DvPass& P = g_dvp; DvS& S = g_dvs;
    P.vFail++; P.vDead = 1;
    InterlockedIncrement(&g_dvNFail);
    if (!S.test) S.off = 1;
    if (S.failLogged++ >= 4) return;
    static const char* const kWhat[6] = { "", "the shaders bound at a draw are not the ones D3DX bound", "D3DX gave a blended parameter other registers than computed here", "the recording, issued again, leaves other registers than D3DX did",
                                          "D3DX's preshader gave another result than computed here", "the recording, issued again, leaves other preshader results than D3DX did" };
    logf("devpass: VERIFICATION FAILED - %s (parameter %s, written with method %d, %u bytes; register c%u of shader %08X; effect %08X; vertex shaders %08X / %08X, pixel shaders %08X / %08X).%s",
         kWhat[kind < 6 ? kind : 0], v && v->name ? S.namePool + v->name : "-", v ? (int)v->slot : 0, v ? v->nb : 0, reg, s ? s->ptr : 0, v ? v->fx : P.passFx, P.vs, P.xvs, P.ps, P.xps,
         S.test ? "" : " The second pass at the device is OFF for this session; in-between pictures are made through D3DX as before.");
}
// Mode 2: D3DX executes effect call r while its device calls are listened to; then the first pass's recording of
// the same call goes through dvRun into a shadow of its own.
static void dvVerifyCall(RtRec* r, DWORD dev) {
    DvRec& d = g_dvr; DvPass& P = g_dvp; DvS& S = g_dvs;
    LONG bad0 = d.bad;
    d.cur = d.x; d.end = d.x + DV_X_BYTES; d.curOp = 0;                     // (every upload with its data: nothing is kept by reference here)
    twExecAgain(r);
    const DWORD* p = (const DWORD*)d.x; const DWORD* e = (const DWORD*)d.cur;
    d.cur = NULL;
    if (d.bad != bad0) { d.bad = bad0; P.vDead = 1; return; }               // something that cannot be kept: this frame's verification says nothing
    if (P.vDead) return;
    for (; p < e; p += p[0] >> 16) {
        DWORD slot = p[0] & 0xFFFF;
        if (!(p[0] >> 16)) { P.vDead = 1; return; }
        if (slot == 92) P.xvs = p[1]; else if (slot == 107) P.xps = p[1];
        else if (slot == 94) dvShadowF(S.xVsF, S.xSet, p[1], p[2], p + 3);
        else if (slot == 109) dvShadowF(S.xPsF, S.xSet + 256, p[1], p[2], p + 3);
    }
    if (!dvRun(r, dev, true)) { P.vWould = P.abort; P.abort = 0; P.vDead = 1; }      // the pass would have been stopped here: nothing more to compare in this frame
}
static void dvVerifyDraw() {                              // mode 2, at a draw: what the draw reads of the blended parameters
    DvPass& P = g_dvp; DvS& S = g_dvs;
    if (P.vDead || !P.passFx) return;
    if (P.xvs != P.vs || P.xps != P.ps) { dvFail(1, NULL, NULL, 0); return; }
    P.vDraws++;
    DWORD fx = P.passFx;
    for (int w = 0; w < 2; ++w) {
        DWORD ptr = w ? P.xps : P.xvs;
        if (!ptr || ptr == DV_UNKNOWN) continue;
        const DvSh* s = NULL; bool fed = false;
        const float* X = w ? S.xPsF : S.xVsF; const float* D = w ? S.dPsF : S.dVsF; const BYTE* set = S.xSet + (w ? 256 : 0);
        for (LONG i = 0; i < P.nLive; ++i) {
            DvVal* v = P.live[i];
            if (!v->sub || v->fx != fx || v->stamp != P.stamp) continue;
            if (!s) { s = dvShader(fx, ptr); if (!s || !s->ok) { P.vDead = 1; P.vWould = DV_AB_TABLE; return; } }
            if (!v->name) { P.vDead = 1; P.vWould = DV_AB_TABLE; return; }
            dvResolve(v, s, w);
            if (v->memoPre[w]) fed = true;
            const DvShC* c = v->memoC[w];
            if (!c) continue;
            if (!dvLayoutOk(v, c)) { P.vDead = 1; P.vWould = DV_AB_LAYOUT; return; }
            DWORD n = dvValRegs(v, c);
            for (DWORD r = 0; r < n; ++r) {
                DWORD reg = (DWORD)c->reg + r;
                if (reg >= 256) break;
                if (!set[reg]) { P.vUnset++; continue; }
                float img[4]; DWORD comps = 4; const void* want = v->buf + r * 16;
                if (v->cls != TWC_PALETTE) { comps = dvMatReg(c, v->slot, (const float*)v->buf, r, img); want = img; }
                if (memcmp(X + reg * 4, want, comps * 4)) { dvFail(2, v, s, reg); return; }
                if (memcmp(D + reg * 4, X + reg * 4, 16)) { dvFail(3, v, s, reg); return; }
                P.vRegs++;
            }
        }
        if (!fed) continue;
        if (!dvPreEval(s, fx)) { P.vDead = 1; P.vWould = DV_AB_PRES; return; }
        for (WORD i = 0; i < s->prog->cN; ++i) {
            const DvPreOut& o = s->prog->cOut[i]; DWORD reg = o.off >> 2, k = o.off & 3;
            if (reg >= 256) continue;
            if (!set[reg]) { P.vUnset++; continue; }
            if (memcmp(X + reg * 4 + k, &o.own, 4)) { dvFail(4, NULL, s, reg); return; }
            if (memcmp(D + reg * 4 + k, X + reg * 4 + k, 4)) { dvFail(5, NULL, s, reg); return; }
            P.vPre++;
        }
    }
}

// ---------------------------------------------------------------- a frame's pass, from the walk's side
static void dvMarkBegin() {                               // render thread, a stretch opens: listen from here
    DvRec& d = g_dvr;
    d.rec = 0; d.cur = NULL;
    if (!g_dvOn || !dvInit()) return;
    d.usedA = 0; d.nChunk = 0; d.bad = 0; d.badSlot = 0; d.rawPtr = NULL; d.rawBytes = 0; d.curOp = 0; d.rec = 1;
}
static void dvMarkEnd() {
    DvRec& d = g_dvr;
    if (d.rec) { g_dvRecBytes += d.usedA; g_dvNRecFrames++; }
    d.rec = 0; d.cur = NULL;
}
static int dvPassMode() {                                 // how the second pass of the frame at hand is made
    DvRec& d = g_dvr; DvS& S = g_dvs;
    if (!g_dvOn || !S.ready) return 0;
    if (!d.rec) { g_dvNNoRec++; return 0; }
    if (d.bad) { if (d.bad & DV_BAD_FULL) g_dvNBadFull++; else if (d.bad & DV_BAD_STRAY) g_dvNBadStray++; else { g_dvNBadCall++; g_dvLastBadSlot = (LONG)d.badSlot + 1; } return 0; }
    if (S.test == 2) return 1;
    if (S.test == 3) return 2;
    if (S.off) { g_dvNOff++; return 0; }
    if (S.camHold) { g_dvNCam++; return 0; }
    if (S.skip > 0) { S.skip--; g_dvNSkip++; return 0; }
    if (S.proven < DV_PROVE || S.verifyNext > 0) return 2;
    if ((++S.frameN % DV_AUDIT) == 0) return 2;
    return 1;
}
static void dvPassBegin(int mode) {
    DvPass& P = g_dvp; DvS& S = g_dvs;
    for (LONG i = 0; i < P.nLive; ++i) P.live[i]->live = 0;
    P.nLive = 0;
    if (S.gen != g_twFxGen) dvForget();
    if (++P.stamp == 0) { for (DWORD i = 0; i < DV_VALN; ++i) S.val[i].stamp = 0; P.stamp = 1; }
    P.mode = mode; P.abort = 0; P.vs = P.ps = P.xvs = P.xps = DV_UNKNOWN; P.passFx = 0;
    P.nChunks = P.nCalls = P.nPatched = P.nPre = 0;
    P.shFx = 0; P.shPtr[0] = P.shPtr[1] = 0; P.shTab[0] = P.shTab[1] = NULL;
    P.vDead = P.vWould = P.vDraws = P.vRegs = P.vPre = P.vUnset = P.vFail = 0;
    if (mode == 2) memset(S.xSet, 0, 512);
}
static void dvPassStopped() {                             // mode 1 could not go on: the pass is made again from the mark, by D3DX
    DvPass& P = g_dvp; DvS& S = g_dvs;
    InterlockedIncrement(&g_dvNStop[P.abort < DV_AB_N ? P.abort : 0]);
    P.stoppedFor = P.abort;
    if (P.abort == DV_AB_UNPROVEN) S.verifyNext = 3;      // the next frames are verified: that is where blended registers get their proof
    else { S.skipLen = S.skipLen ? (S.skipLen < 128 ? S.skipLen * 2 : 256) : 1; S.skip = S.skipLen; }
    for (LONG i = 0; i < P.nLive; ++i) P.live[i]->live = 0;
    P.nLive = 0; P.abort = 0;
}
static void dvPassDone(int mode, bool camMoved) {         // a second pass is complete (mode: how it was made in the end)
    DvPass& P = g_dvp; DvS& S = g_dvs;
    LONG stoppedFor = P.stoppedFor; P.stoppedFor = 0;
    // a camera this code cannot follow (a preshader it does not compute): frames go to D3DX until the camera stands again
    if (mode == 1) S.camHold = 0;
    else if (mode == 0) S.camHold = (camMoved && (S.camHold || stoppedFor == DV_AB_PRES)) ? 1 : 0;
    else S.camHold = (camMoved && P.vWould == DV_AB_PRES) ? 1 : 0;
    if (S.camHold) { S.skip = 0; S.skipLen = 0; }
    if (mode == 1) {
        g_dvNLive++; g_dvNChunks += P.nChunks; g_dvNCalls += P.nCalls; g_dvNPatched += P.nPatched; g_dvNPre += P.nPre;
        S.skipLen = 0;
    } else if (mode == 2) {
        g_dvNVerify++; g_dvVDraws += P.vDraws; g_dvVRegs += P.vRegs; g_dvVPre += P.vPre; g_dvVUnset += P.vUnset;
        if (P.vWould) InterlockedIncrement(&g_dvNWould[P.vWould < DV_AB_N ? P.vWould : 0]);
        if (!P.vDead && !P.vFail) { g_dvNVerifyClean++; S.proven++; S.subProven += P.vRegs; S.preProven += P.vPre; }
        if (S.verifyNext > 0 && !P.vFail) S.verifyNext--;
    }
}
static void dvFaulted() { g_dvr.cur = NULL; g_dvp.abort = 0; }
static void dvReport() {                                  // report thread, every 5 s
    static LONG lLive = 0, lVer = 0, lClean = 0, lCam = 0, lSkip = 0, lNoRec = 0, lBadCall = 0, lBadFull = 0, lBadStray = 0, lOff = 0, lChunks = 0, lCalls = 0, lPatched = 0, lPre = 0, lRecF = 0, lVD = 0, lVR = 0, lVP = 0, lVU = 0, lStop[DV_AB_N], lWould[DV_AB_N], said = 0;
    static LONG64 lBytes = 0;
    if (!g_dvOn) return;
    LONG live = g_dvNLive, ver = g_dvNVerify, clean = g_dvNVerifyClean, cam = g_dvNCam, skip = g_dvNSkip, noRec = g_dvNNoRec, badCall = g_dvNBadCall, badFull = g_dvNBadFull, badStray = g_dvNBadStray, off = g_dvNOff,
         chunks = g_dvNChunks, calls = g_dvNCalls, patched = g_dvNPatched, pre = g_dvNPre, recF = g_dvNRecFrames, vd = g_dvVDraws, vr = g_dvVRegs, vp = g_dvVPre, vu = g_dvVUnset;
    LONG64 bytes = rd64(&g_dvRecBytes);
    LONG dLive = live - lLive, dVer = ver - lVer, dCam = cam - lCam, dSkip = skip - lSkip, dBad = (noRec - lNoRec) + (badCall - lBadCall) + (badFull - lBadFull) + (badStray - lBadStray), dOff = off - lOff, dRecF = recF - lRecF;
    char stops[400]; int q = 0; stops[0] = 0; LONG nStop = 0;
    for (int i = 1; i < DV_AB_N; ++i) { LONG s = g_dvNStop[i], w = g_dvNWould[i]; LONG ds = s - lStop[i], dw = w - lWould[i]; lStop[i] = s; lWould[i] = w; nStop += ds;
        if ((ds || dw) && q < 300) q += wsprintfA(stops + q, "%s%d x %s", q ? "; " : "", (int)(ds + dw), kDvAb[i]); }
    if (dLive || dVer || dCam || dSkip || dBad || dOff || nStop || !said) {
        said = 1;
        logf("devpass: last 5 s - in-between pictures made at the device: %d (each: %d effect calls answered by %d device calls, %d uploads carrying blended registers, %d carrying a camera product computed here) | made by D3DX: %d to verify (%d clean; %d draws, "
             "%d blended registers and %d computed preshader results compared, %d not uploaded), %d while a camera moved that cannot be followed, %d after a stop, %d because the frame's recording could not be used (%d not recorded, "
             "%d held a call that cannot be issued again%s, %d did not fit, %d had a device call inside a parameter write), %d switched off | stops: %s | recording: %d KB a frame | so far %d clean verified frames, %d blended registers and "
             "%d preshader results verified, %d differences%s",
             (int)dLive, (int)(dLive ? (chunks - lChunks) / dLive : 0), (int)(dLive ? (calls - lCalls) / dLive : 0), (int)(dLive ? (patched - lPatched) / dLive : 0), (int)(dLive ? (pre - lPre) / dLive : 0),
             (int)dVer, (int)(clean - lClean), (int)(vd - lVD), (int)(vr - lVR), (int)(vp - lVP), (int)(vu - lVU), (int)dCam, (int)dSkip, (int)dBad, (int)(noRec - lNoRec), (int)(badCall - lBadCall),
             g_dvLastBadSlot ? rtOpName(g_dvLastBadSlot - 1) : "", (int)(badFull - lBadFull), (int)(badStray - lBadStray), (int)dOff, q ? stops : "none",
             (int)(dRecF > 0 ? (bytes - lBytes) / dRecF / 1024 : 0), (int)g_dvs.proven, (int)g_dvs.subProven, (int)g_dvs.preProven, (int)g_dvNFail, g_dvs.off ? " - OFF for this session" : "");
    }
    lLive = live; lVer = ver; lClean = clean; lCam = cam; lSkip = skip; lNoRec = noRec; lBadCall = badCall; lBadFull = badFull; lBadStray = badStray; lOff = off; lChunks = chunks; lCalls = calls; lPatched = patched; lPre = pre;
    lRecF = recF; lVD = vd; lVR = vr; lVP = vp; lVU = vu; lBytes = bytes;
}
static void dvSwitchEnv() {                               // AOTR_DEVPASS: 0 off, 1 on, 2 every pass at the device (tests), 3 every pass verified (tests)
    char env[16]; env[0] = 0;
    if (!GetEnvironmentVariableA("AOTR_DEVPASS", env, sizeof(env)) || !env[0]) return;
    int v = env[0] - '0';
    InterlockedExchange(&g_dvOn, v > 0 ? 1 : 0);
    g_dvs.test = v >= 2 ? v : 0;
#ifndef AOTR_PROD
    env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_DEVPASS_BREAK", env, sizeof(env)) && env[0]) g_dvBreak = env[0] - '0';
#endif
}
