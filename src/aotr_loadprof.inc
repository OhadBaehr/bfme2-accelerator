// aotr_loadprof.inc - where the game thread is while a frame takes far too long (load screens, the slow frames
// after them, hitches).
//
// The battle profiler (gsThread) looks at frames of 30..250 ms and only in diagnostic builds. Loading was never
// profiled at all until v51: a load screen is one engine frame of 20-30 s. This sampler is asleep in normal play
// (it looks at the engine's frame counter every 20 ms) and wakes only while the current frame is already longer
// than 80 ms; then it takes about a thousand samples a second of the game thread (suspend, read the context and
// the stack, resume) until the frame ends. Every sample is kept (time, where, the innermost game.dat callers and
// the outermost ones); when no long frame has been seen for three seconds the episode is written to the log:
//   - every frame of 2.5 s or more ON ITS OWN (a load screen), with a timeline: the frame cut into eight slices in
//     time order, each with what the thread was mostly doing and under which functions - "what is it doing at 97 %";
//   - all shorter slow frames together (the seconds after a load);
//   - for each: where the thread was by module, how much of it was waiting for the render queue, the functions it
//     was in, the game.dat code that called out of game.dat, and the commonest call paths.
// [Engine] LoadProfile = 1 switches it on. It needs the engine frame counter, so it runs on the verified build only.
#define LP_TAB   65536
#define LP_MAXS  160000
#define LP_ROOTS 10
struct LpS { DWORD t, eip, frame, ret0; BYTE waitQ, scope, nroot, nleaf; DWORD leaf[4]; DWORD root[LP_ROOTS]; };   // 72 bytes
struct LpPath { DWORD k[4]; DWORD n; };
#define LP_PATHS 16384
static LpS* g_lpLog = NULL; static LONG g_lpN = 0, g_lpCommitted = 0;
static LeafEnt* g_lpSelf = NULL, *g_lpIncl = NULL, *g_lpBound = NULL, *g_lpTmp = NULL, *g_lpAgg = NULL;
static LpPath* g_lpPath = NULL, *g_lpPathTmp = NULL;
static LONG g_lpMod[PMODS + 1];
static LONG g_lpDrop = 0, g_lpEpisodes = 0;
static const char* g_lpTag = "load";                       // whose lines the shared report functions are writing
static DWORD g_lpSelfLo = 0, g_lpSelfHi = 0;

static int __cdecl lpCmp(const void* a, const void* b) { DWORD x = ((const LeafEnt*)a)->count, y = ((const LeafEnt*)b)->count; return x < y ? 1 : (x > y ? -1 : 0); }
static int __cdecl lpPathCmp(const void* a, const void* b) { DWORD x = ((const LpPath*)a)->n, y = ((const LpPath*)b)->n; return x < y ? 1 : (x > y ? -1 : 0); }
static DWORD lpSymKey(DWORD a) {                              // whole function in game.dat, export (or a piece) elsewhere
    if (a >= g_gameBase && a < g_gameEnd) { DWORD f = gsFunc(a); return f ? f : a; }
    if (a >= g_lpSelfLo && a < g_lpSelfHi) return a & ~0x3Fu;
    int mi = pmOf(a);
    if (mi < 0) return a & ~0xFFFu;
    DWORD rva = a - g_pm[mi].base;
    ExpMod* m = expFor(g_pm[mi].base);
    if (m && m->n) {
        int lo = 0, hi = m->n - 1, best = -1;
        while (lo <= hi) { int mid = (lo + hi) >> 1; if (m->syms[mid].rva <= rva) { best = mid; lo = mid + 1; } else hi = mid - 1; }
        if (best >= 0 && rva - m->syms[best].rva < 0x200) return g_pm[mi].base + m->syms[best].rva;
        return a & ~0xFFu;                                    // an internal routine: a 256-byte piece of it
    }
    return g_pm[mi].base;
}
static void lpName(DWORD key, char* sym) {
    symName(key, sym);
    char* plus = strrchr(sym, '+'); if (plus && strchr(sym, '!')) *plus = 0;           // exports: name only
}
static void lpDump(LeafEnt* tab, const char* what, int count, LONG denom) {
    int m = 0;
    for (int i = 0; i < LP_TAB; ++i) if (tab[i].key) g_lpTmp[m++] = tab[i];
    qsort(g_lpTmp, m, sizeof(LeafEnt), lpCmp);
    char line[1000]; int lp = 0;
    for (int i = 0; i < m && i < count; ++i) {
        if (g_lpTmp[i].count * 500 < (DWORD)denom && i > 8) { if (lp) logf("%s:   %s%s", g_lpTag, what, line); lp = 0; break; }   // below 0.2 %: the rest is dust
        char sym[160]; lpName(g_lpTmp[i].key, sym);
        char pc[16]; pctStr(g_lpTmp[i].count, denom, pc);
        lp += wsprintfA(line + lp, " %s=%s", sym, pc);
        if (lp > 800 || i + 1 == count || i + 1 == m) { logf("%s:   %s%s", g_lpTag, what, line); lp = 0; }
    }
}
static void lpPathAdd(DWORD a, DWORD b, DWORD c, DWORD d) {
    DWORD h = ((a * 2654435761u) ^ (b * 2246822519u) ^ (c * 3266489917u) ^ (d * 668265263u)) >> 18;
    for (int i = 0; i < 64; ++i) {
        LpPath* e = &g_lpPath[(h + i) & (LP_PATHS - 1)];
        if (e->n && e->k[0] == a && e->k[1] == b && e->k[2] == c && e->k[3] == d) { e->n++; return; }
        if (!e->n) { e->k[0] = a; e->k[1] = b; e->k[2] = c; e->k[3] = d; e->n = 1; return; }
    }
}
// one group of samples [idx[0..n)): the tables and the lines
static void lpGroup(const LONG* idx, LONG n, const char* title) {
    if (n < 40) return;
    memset(g_lpSelf, 0, LP_TAB * sizeof(LeafEnt)); memset(g_lpIncl, 0, LP_TAB * sizeof(LeafEnt)); memset(g_lpBound, 0, LP_TAB * sizeof(LeafEnt));
    memset(g_lpPath, 0, LP_PATHS * sizeof(LpPath)); memset(g_lpMod, 0, sizeof(g_lpMod));
    LONG waitQ = 0, scope = 0, drop = 0;
    for (LONG q = 0; q < n; ++q) {
        const LpS& s = g_lpLog[idx[q]];
        if (s.waitQ) waitQ++;
        if (s.scope) scope++;
        { int mi = pmOf(s.eip); g_lpMod[mi < 0 ? PMODS : mi]++; }
        DWORD leafKey = lpSymKey(s.eip);
        tabAdd(g_lpSelf, LP_TAB, leafKey, 1, &drop);
        bool inGame = s.eip >= g_gameBase && s.eip < g_gameEnd;
        if (!inGame && s.ret0) tabAdd(g_lpBound, LP_TAB, s.ret0, 1, &drop);
        DWORD seen[4 + LP_ROOTS]; int ns = 0;
        for (int k = 0; k < s.nleaf; ++k) { bool dup = false; for (int j = 0; j < ns; ++j) if (seen[j] == s.leaf[k]) dup = true; if (!dup) seen[ns++] = s.leaf[k]; }
        for (int k = 0; k < s.nroot; ++k) { bool dup = false; for (int j = 0; j < ns; ++j) if (seen[j] == s.root[k]) dup = true; if (!dup) seen[ns++] = s.root[k]; }
        for (int j = 0; j < ns; ++j) tabAdd(g_lpIncl, LP_TAB, seen[j], 1, &drop);
        // the path: where it is, and the game.dat functions that got it there (innermost first; a leaf in game.dat is its own first entry)
        int o = (inGame && s.nleaf && s.leaf[0] == leafKey) ? 1 : 0;
        lpPathAdd(leafKey, s.nleaf > o ? s.leaf[o] : 0, s.nleaf > o + 1 ? s.leaf[o + 1] : 0, s.nleaf > o + 2 ? s.leaf[o + 2] : 0);
    }
    char mods[700]; int q = 0; mods[0] = 0;
    for (int pass = 0; pass < 8; ++pass) {
        int best = -1; for (int i = 0; i <= PMODS; ++i) if (g_lpMod[i] > 0 && (best < 0 || g_lpMod[i] > g_lpMod[best])) best = i;
        if (best < 0) break;
        char pc[16]; pctStr(g_lpMod[best], n, pc);
        q += wsprintfA(mods + q, " %s %s%%,", best == PMODS ? "(no module)" : g_pm[best].name, pc);
        g_lpMod[best] = 0;
    }
    char w1[16], w2[16]; pctStr(waitQ, n, w1); pctStr(scope, n, w2);
    logf("%s: -- %s: %d samples | by module:%s | waiting for the render queue %s%%, inside a D3DX call still on the game thread %s%%", g_lpTag, title, (int)n, mods, w1, w2);
    lpDump(g_lpSelf, "in:", 48, n);
    lpDump(g_lpBound, "called out of game.dat from:", 30, n);
    lpDump(g_lpIncl, "under (innermost 4 and outermost 10 game.dat functions of each sample):", 60, n);
    int m = 0;
    for (int i = 0; i < LP_PATHS; ++i) if (g_lpPath[i].n) g_lpPathTmp[m++] = g_lpPath[i];
    qsort(g_lpPathTmp, m, sizeof(LpPath), lpPathCmp);
    for (int i = 0; i < m && i < 22; ++i) {
        if (g_lpPathTmp[i].n * 200 < (DWORD)n) break;                                   // below 0.5 %
        char s0[160], s1[160], s2[160], s3[160], pc[16];
        lpName(g_lpPathTmp[i].k[0], s0);
        if (g_lpPathTmp[i].k[1]) lpName(g_lpPathTmp[i].k[1], s1); else lstrcpyA(s1, "?");
        if (g_lpPathTmp[i].k[2]) lpName(g_lpPathTmp[i].k[2], s2); else lstrcpyA(s2, "?");
        if (g_lpPathTmp[i].k[3]) lpName(g_lpPathTmp[i].k[3], s3); else lstrcpyA(s3, "?");
        pctStr(g_lpPathTmp[i].n, n, pc);
        logf("%s:   path %s%%  %s < %s < %s < %s", g_lpTag, pc, s0, s1, s2, s3);
    }
}
// a long frame in time order: eight slices, each with its commonest leaf, caller out, and the functions most samples were under
static void lpTimeline(const LONG* idx, LONG n) {
    if (n < 400) return;
    DWORD t0 = g_lpLog[idx[0]].t, t1 = g_lpLog[idx[n - 1]].t;
    if (t1 <= t0) return;
    for (int sl = 0; sl < 8; ++sl) {
        DWORD a = t0 + (DWORD)((ULONGLONG)(t1 - t0) * sl / 8), b = t0 + (DWORD)((ULONGLONG)(t1 - t0) * (sl + 1) / 8);
        memset(g_lpSelf, 0, LP_TAB * sizeof(LeafEnt)); memset(g_lpIncl, 0, LP_TAB * sizeof(LeafEnt)); memset(g_lpBound, 0, LP_TAB * sizeof(LeafEnt));
        LONG cnt = 0, waitQ = 0, drop = 0;
        for (LONG q = 0; q < n; ++q) {
            const LpS& s = g_lpLog[idx[q]];
            if (s.t < a || (s.t >= b && sl < 7)) continue;
            cnt++; if (s.waitQ) waitQ++;
            tabAdd(g_lpSelf, LP_TAB, lpSymKey(s.eip), 1, &drop);
            if (s.ret0 && !(s.eip >= g_gameBase && s.eip < g_gameEnd)) tabAdd(g_lpBound, LP_TAB, s.ret0, 1, &drop);
            DWORD seen[4 + LP_ROOTS]; int ns = 0;
            for (int k = 0; k < s.nleaf; ++k) { bool dup = false; for (int j = 0; j < ns; ++j) if (seen[j] == s.leaf[k]) dup = true; if (!dup) seen[ns++] = s.leaf[k]; }
            for (int k = 0; k < s.nroot; ++k) { bool dup = false; for (int j = 0; j < ns; ++j) if (seen[j] == s.root[k]) dup = true; if (!dup) seen[ns++] = s.root[k]; }
            for (int j = 0; j < ns; ++j) tabAdd(g_lpIncl, LP_TAB, seen[j], 1, &drop);
        }
        if (cnt < 8) { logf("load:   %2u.%u-%2u.%u s: (hardly sampled)", (unsigned)((a - t0) / 1000), (unsigned)((a - t0) % 1000 / 100), (unsigned)((b - t0) / 1000), (unsigned)((b - t0) % 1000 / 100)); continue; }
        char line[900]; int lp = 0; char pc[16];
        lp += wsprintfA(line + lp, "%2u.%u-%2u.%u s: waiting for the queue %s%% | in", (unsigned)((a - t0) / 1000), (unsigned)((a - t0) % 1000 / 100), (unsigned)((b - t0) / 1000), (unsigned)((b - t0) % 1000 / 100), pctStr(waitQ, cnt, pc));
        for (int pass = 0; pass < 3; ++pass) {                 // three tables, the top few of each
            LeafEnt* tab = pass == 0 ? g_lpSelf : pass == 1 ? g_lpBound : g_lpIncl; int top = pass == 2 ? 14 : 4;
            if (pass == 1) lp += wsprintfA(line + lp, " | called out from");
            if (pass == 2) lp += wsprintfA(line + lp, " | under");
            int m = 0; for (int i = 0; i < LP_TAB; ++i) if (tab[i].key) g_lpTmp[m++] = tab[i];
            qsort(g_lpTmp, m, sizeof(LeafEnt), lpCmp);
            int shown = 0;
            for (int i = 0; i < m && shown < top && lp < 800; ++i) {
                if (pass == 2 && g_lpTmp[i].count * 100 >= (DWORD)cnt * 97) continue;      // the frames every sample has say nothing about this slice
                if (g_lpTmp[i].count * 20 < (DWORD)cnt) break;                             // below 5 %
                char sym[160]; lpName(g_lpTmp[i].key, sym);
                lp += wsprintfA(line + lp, " %s=%s", sym, pctStr(g_lpTmp[i].count, cnt, pc)); shown++;
            }
        }
        logf("load:   %s", line);
    }
}
static void lpReport() {
    LONG n = g_lpN;
    if (n < 150) { g_lpN = 0; return; }
    profRefreshModules();
    static LONG idx[LP_MAXS];
    // the frames: samples carry the engine frame number they were taken in
    DWORD total = 0, longest = 0; LONG frames = 0, big = 0;
    for (LONG i = 0; i < n; ) {
        LONG j = i; while (j < n && g_lpLog[j].frame == g_lpLog[i].frame) ++j;
        DWORD d = g_lpLog[j - 1].t - g_lpLog[i].t + 80;
        total += d; frames++; if (d > longest) longest = d; if (d >= 2500) big++;
        i = j;
    }
    logf("load: ==== long-frame profile #%d: %u.%u s spent in %d frames longer than 80 ms (%d of them 2.5 s or longer, the longest %u.%u s), %d samples of the game thread%s ====", (int)++g_lpEpisodes,
         (unsigned)(total / 1000), (unsigned)(total % 1000 / 100), (int)frames, (int)big, (unsigned)(longest / 1000), (unsigned)(longest % 1000 / 100), (int)n, n >= LP_MAXS ? " (the sample store was full: the end is missing)" : "");
    LONG nb = 0, m = 0;
    for (LONG i = 0; i < n; ) {
        LONG j = i; while (j < n && g_lpLog[j].frame == g_lpLog[i].frame) ++j;
        DWORD d = g_lpLog[j - 1].t - g_lpLog[i].t + 80;
        if (d >= 2500) {
            LONG k = 0; for (LONG q = i; q < j; ++q) idx[k++] = q;
            char title[96]; wsprintfA(title, "one frame of %u.%u s (long frame %d of %d)", (unsigned)(d / 1000), (unsigned)(d % 1000 / 100), (int)++nb, (int)big);
            lpGroup(idx, k, title);
            logf("load:   the same frame in time order, eight slices:");
            lpTimeline(idx, k);
        }
        i = j;
    }
    for (LONG i = 0; i < n; ) {                                // everything shorter, together
        LONG j = i; while (j < n && g_lpLog[j].frame == g_lpLog[i].frame) ++j;
        if (g_lpLog[j - 1].t - g_lpLog[i].t + 80 < 2500) for (LONG q = i; q < j; ++q) idx[m++] = q;
        i = j;
    }
    if (m) { char title[96]; wsprintfA(title, "the %d shorter slow frames together", (int)(frames - big)); lpGroup(idx, m, title); }
    g_lpN = 0;
}
// ---------------------------------------------------------------- v55: frames while the camera moves, and the render thread
// PAN PROFILE. "Panning hangs a bit" cannot be read from frames longer than 80 ms, and a sampler that wakes at a
// frame's 80th millisecond sees only what the frame happened to be doing last. So while the camera is being
// scrolled (aotr_scroll.inc stamps g_lpPanTick), the game thread is sampled about 500 times a second through WHOLE
// frames, each sample carrying its frame's number; the game thread itself notes every frame's length. Three seconds
// after the camera stops, the frames are split into the slow ones (a third longer than the usual frame of that pan)
// and the rest, and each group gets the same tables as a load: where the thread was, under which functions, the
// commonest paths. What the slow frames have and the others do not is what makes a pan hitch.
// [Engine] PanProfile = 1. Cost while the camera moves: 500 suspensions a second of ~20 microseconds.
//
// RENDER THREAD PROFILE. What does a picture cost the render thread, and where: in D3DX's effect code, in the
// Direct3D layer, in our own walk? The render thread notes the queued call it is executing (two plain stores a
// record); this thread reads its instruction pointer a hundred times a second and files each sample under that
// call and the module it was in - the first pass and the second pass (the in-between picture) apart. One report
// every thirty seconds of play. [Engine] RenderProfile = 1.
#define LP_PANMAX 60000
#define LP_RTMAX  6000
#define LP_RCLS   96
struct LpR { DWORD eip; WORD op; BYTE kind, pass; };
static LpS* g_lpPan = NULL; static LONG g_lpPanN = 0, g_lpPanCommitted = 0;
static LpR* g_lpRt = NULL; static LONG g_lpRtN = 0;
static volatile LONG g_lpFrNo = 0;
static WORD g_lpFrDur[4096];                               // a frame's length in tenths of a millisecond, by frame number
static LONG g_lpPanOn = 0, g_lpRtOn = 0, g_lpLoadOn = 0, g_lpPanReports = 0;
static void lpOnFrame(LONG64 dt) {                         // game thread, where the views are drawn: the frame that just ended
    LONG64 f = g_pqpf.QuadPart, v = f > 0 ? dt * 10000 / f : 0;
    g_lpFrDur[g_lpFrNo & 4095] = (WORD)(v > 65535 ? 65535 : v);
    InterlockedIncrement(&g_lpFrNo);
}
// one sample of the game thread into *s (everything but its time). false: none taken.
static bool lpTake(HANDLE h, NT_TIB* tib, BYTE* stk, DWORD stkCap, LpS* ps, DWORD* panFrame) {
    if (SuspendThread(h) == (DWORD)-1) return false;
    CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
    DWORD eip = 0, esp = 0, n = 0;
    if (GetThreadContext(h, &ctx)) {
        eip = ctx.Eip; esp = ctx.Esp;
        DWORD sb = (DWORD)(ULONG_PTR)tib->StackBase, sl = (DWORD)(ULONG_PTR)tib->StackLimit;
        if (esp >= sl && esp < sb) { n = (sb - esp) & ~3u; if (n > stkCap) n = stkCap; if (!gsCopyStack(stk, esp, n)) n = 0; }
    }
    LONG waitQ = g_rtDrainWaiting, scope = g_rtDirectScope, frameNo = g_mkFrames;
    if (panFrame) *panFrame = (DWORD)g_lpFrNo;
    ResumeThread(h);
    if (!eip) return false;
    LpS& s = *ps;
    s.eip = eip; s.frame = (DWORD)frameNo; s.ret0 = 0; s.waitQ = waitQ ? 1 : 0; s.scope = scope > 0 ? 1 : 0; s.nroot = 0; s.nleaf = 0;
    if (pmOf(eip) < 0) profRefreshModules();
    DWORD seen[192]; int ns = 0;
    DWORD f0 = (eip >= g_gameBase && eip < g_gameEnd) ? gsFunc(eip) : 0;
    if (f0) seen[ns++] = f0;
    DWORD cur = f0; int miss = 0;                          // the function the next return address must have called
    for (DWORD o = 0; o + 4 <= n && ns < 192; o += 4) {
        DWORD v = *(DWORD*)(stk + o);
        if (v < g_textLo + 8 || v >= g_textHi || !gsIsRet(v)) continue;
        const BYTE* rp = (const BYTE*)(ULONG_PTR)v;
        if (cur && rp[-5] == 0xE8) {                       // a direct call must target the current function (or a jmp to it)
            DWORD t = v + *(const LONG*)(rp - 4);
            bool ok = t == cur;
            if (!ok && t >= g_textLo && t + 5 <= g_textHi && ((const BYTE*)(ULONG_PTR)t)[0] == 0xE9) ok = t + 5 + *(const LONG*)((const BYTE*)(ULONG_PTR)t + 1) == cur;
            if (!ok && ++miss <= 3) continue;              // a stale return address; after three, resynchronize
        }
        miss = 0;
        if (!s.ret0) s.ret0 = v;
        DWORD f = gsFunc(v);
        cur = f;
        if (!f) continue;
        bool dup = false; for (int k = 0; k < ns; ++k) if (seen[k] == f) { dup = true; break; }
        if (!dup) seen[ns++] = f;
    }
    for (int k = 0; k < ns && k < 4; ++k) s.leaf[s.nleaf++] = seen[k];
    for (int k = 0; k < LP_ROOTS && ns - 1 - k >= 4; ++k) s.root[s.nroot++] = seen[ns - 1 - k];       // from the outermost inwards, without what is in leaf[]
    return true;
}
static int __cdecl lpWordCmp(const void* a, const void* b) { WORD x = *(const WORD*)a, y = *(const WORD*)b; return x < y ? -1 : (x > y ? 1 : 0); }
static void lpPanReport() {
    LONG n = g_lpPanN;
    if (n < 300) { g_lpPanN = 0; return; }
    profRefreshModules();
    static LONG slow[LP_PANMAX], rest[LP_PANMAX]; static WORD durs[4096];
    LONG cur = g_lpFrNo; LONG nf = 0;
    // the frames these samples are from (in time order), each with its length when it is still on record
    for (LONG i = 0; i < n && nf < 4096; ) {
        LONG j = i; while (j < n && g_lpPan[j].frame == g_lpPan[i].frame) ++j;
        LONG no = (LONG)g_lpPan[i].frame;
        if (no < cur && cur - no < 4000 && g_lpFrDur[no & 4095]) durs[nf++] = g_lpFrDur[no & 4095];
        i = j;
    }
    if (nf < 12) { g_lpPanN = 0; return; }
    static WORD sorted[4096]; memcpy(sorted, durs, nf * sizeof(WORD)); qsort(sorted, nf, sizeof(WORD), lpWordCmp);
    DWORD med = sorted[nf / 2], thr = med * 135 / 100; if (thr < med + 60) thr = med + 60;                  // a third longer than usual, and at least 6 ms longer
    LONG ns = 0, nr = 0, fSlow = 0, fRest = 0; DWORD longest = 0, over = 0;
    for (LONG i = 0; i < n; ) {
        LONG j = i; while (j < n && g_lpPan[j].frame == g_lpPan[i].frame) ++j;
        LONG no = (LONG)g_lpPan[i].frame; DWORD d = (no < cur && cur - no < 4000) ? g_lpFrDur[no & 4095] : 0;
        if (d) {
            if (d > thr) { for (LONG q = i; q < j; ++q) slow[ns++] = q; fSlow++; over += d - med; if (d > longest) longest = d; }
            else { for (LONG q = i; q < j; ++q) rest[nr++] = q; fRest++; }
        }
        i = j;
    }
    logf("pan: ==== camera moving, profile #%d: %d frames sampled through and through (%d samples of the game thread) | a frame usually took %u.%u ms; %d frames took more than %u.%u ms "
         "(the longest %u.%u ms; together %u ms over the usual) ====", (int)++g_lpPanReports, (int)(fSlow + fRest), (int)n, (unsigned)(med / 10), (unsigned)(med % 10), (int)fSlow, (unsigned)(thr / 10), (unsigned)(thr % 10),
         (unsigned)(longest / 10), (unsigned)(longest % 10), (unsigned)(over / 10));
    LpS* keep = g_lpLog; const char* keepTag = g_lpTag;
    g_lpLog = g_lpPan; g_lpTag = "pan";
    { char title[96]; wsprintfA(title, "the %d slow frames", (int)fSlow); lpGroup(slow, ns, title); }
    { char title[96]; wsprintfA(title, "the other %d frames", (int)fRest); lpGroup(rest, nr, title); }
    g_lpLog = keep; g_lpTag = keepTag;
    g_lpPanN = 0;
}
static const char* lpRClassName(DWORD key, char* tmp) {
    if (key == 0xFFFF) return "(between records: waiting for work or for a picture's time)";
    if (key & 0x8000) {
        switch (key & 0xFF) { case 3: return "buffer write"; case 5: case 6: return "parameter block"; case 7: return "texture rows"; case 8: return "effect parameter runs"; case 9: return "surface rows";
                              case 10: return "frame mark"; case 11: return "texture upload (with the wait for its decoding)"; }
        wsprintfA(tmp, "kind %u", (unsigned)(key & 0xFF)); return tmp;
    }
    const char* nm = rtOpName((int)key); return nm ? nm : "?";
}
static void lpRtReport(DWORD spanMs) {
    LONG n = g_lpRtN;
    if (n < 600) { g_lpRtN = 0; return; }
    profRefreshModules();
    static DWORD cKey[LP_RCLS]; static LONG cN[LP_RCLS], cN2[LP_RCLS]; static WORD cMod[LP_RCLS][PMODS + 1];
    memset(cKey, 0, sizeof(cKey)); memset(cN, 0, sizeof(cN)); memset(cN2, 0, sizeof(cN2)); memset(cMod, 0, sizeof(cMod));
    memset(g_lpSelf, 0, LP_TAB * sizeof(LeafEnt)); memset(g_lpMod, 0, sizeof(g_lpMod));
    LONG nCls = 0, p1 = 0, p2 = 0, idle = 0, drop = 0;
    for (LONG i = 0; i < n; ++i) {
        const LpR& r = g_lpRt[i];
        DWORD key = r.kind == 0xFF ? 0xFFFF : (r.kind == 1 ? (DWORD)(r.op & 511) : (0x8000u | r.kind));
        int mi = pmOf(r.eip); if (mi < 0) mi = PMODS;
        if (r.kind == 0xFF) idle++; else if (r.pass) p2++; else p1++;
        LONG c = 0; while (c < nCls && cKey[c] != key + 1) ++c;
        if (c == nCls) { if (nCls >= LP_RCLS) continue; cKey[nCls++] = key + 1; }
        cN[c]++; if (r.pass && r.kind != 0xFF) cN2[c]++; if (cMod[c][mi] < 65535) cMod[c][mi]++;
        if (r.kind != 0xFF) { g_lpMod[mi]++; tabAdd(g_lpSelf, LP_TAB, lpSymKey(r.eip), 1, &drop); }
    }
    LONG busy = p1 + p2; if (busy < 1) busy = 1;
    char mods[500]; int q = 0; mods[0] = 0;
    for (int pass = 0; pass < 7; ++pass) {
        int best = -1; for (int i = 0; i <= PMODS; ++i) if (g_lpMod[i] > 0 && (best < 0 || g_lpMod[i] > g_lpMod[best])) best = i;
        if (best < 0) break;
        char pc[16]; q += wsprintfA(mods + q, " %s %s%%,", best == PMODS ? "(no module)" : g_pm[best].name, pctStr(g_lpMod[best], busy, pc));
        g_lpMod[best] = 0;
    }
    char a1[16], a2[16], a3[16];
    logf("rtprof: ==== render thread, %d samples over %u s: executing first-pass records %s%%, the in-between picture's second pass %s%%, between records %s%% | while executing, by module:%s ====",
         (int)n, (unsigned)(spanMs / 1000), pctStr(p1, n, a1), pctStr(p2, n, a2), pctStr(idle, n, a3), mods);
    for (int which = 0; which < 2; ++which) {                // all executing samples, then the second pass alone
        LONG* cnt = which ? cN2 : cN; LONG denom = which ? (p2 > 0 ? p2 : 1) : busy;
        static LONG used[LP_RCLS]; memset(used, 0, sizeof(used));
        char line[1100]; int lp = 0;
        for (int k = 0; k < 14; ++k) {
            LONG best = -1; for (LONG c = 0; c < nCls; ++c) if (!used[c] && cKey[c] != 0x10000 && cnt[c] > 0 && (best < 0 || cnt[c] > cnt[best])) best = c;
            if (best < 0 || cnt[best] * 100 < denom) break;                                    // below 1 %
            used[best] = 1;
            char tmp[32], pc[16]; lp += wsprintfA(line + lp, " %s %s%%", lpRClassName(cKey[best] - 1, tmp), pctStr(cnt[best], denom, pc));
            if (!which) {                                    // where it was while executing that call (the two largest modules)
                int m1 = -1, m2 = -1;
                for (int i = 0; i <= PMODS; ++i) { if (!cMod[best][i]) continue; if (m1 < 0 || cMod[best][i] > cMod[best][m1]) { m2 = m1; m1 = i; } else if (m2 < 0 || cMod[best][i] > cMod[best][m2]) m2 = i; }
                if (m1 >= 0) { char p1s[16]; lp += wsprintfA(line + lp, " (%s %s", m1 == PMODS ? "?" : g_pm[m1].name, pctStr(cMod[best][m1], cN[best], p1s));
                    if (m2 >= 0) { char p2s[16]; lp += wsprintfA(line + lp, ", %s %s", m2 == PMODS ? "?" : g_pm[m2].name, pctStr(cMod[best][m2], cN[best], p2s)); }
                    lp += wsprintfA(line + lp, ")"); }
            }
            lp += wsprintfA(line + lp, " |");
            if (lp > 950) break;
        }
        if (lp) logf("rtprof:   %s:%s", which ? "the second pass alone, by queued call" : "by queued call (share of the executing samples; the modules it was in)", line);
    }
    { const char* keepTag = g_lpTag; g_lpTag = "rtprof"; lpDump(g_lpSelf, "in:", 36, busy); g_lpTag = keepTag; }
    g_lpRtN = 0;
}
static DWORD WINAPI lpThread(LPVOID) {
    while (!g_mkTid) Sleep(500);                               // the engine's render thread, known from the first frame on
    DWORD tid = g_mkTid;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h || !p_NtQIT) { logf("load: cannot open the game thread - the long-frame profiler is off."); return 0; }
    PROF_TBI tbi; if (p_NtQIT(h, 0, &tbi, sizeof(tbi), NULL) != 0 || !tbi.TebBaseAddress) { logf("load: no TEB - the long-frame profiler is off."); CloseHandle(h); return 0; }
    NT_TIB* tib = (NT_TIB*)tbi.TebBaseAddress;
    static BYTE stk[128 * 1024];
    { HMODULE hs = NULL; if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&lpSymKey, &hs) && hs) {
        IMAGE_NT_HEADERS* nh = (IMAGE_NT_HEADERS*)((BYTE*)hs + ((IMAGE_DOS_HEADER*)hs)->e_lfanew); g_lpSelfLo = (DWORD)(ULONG_PTR)hs; g_lpSelfHi = g_lpSelfLo + nh->OptionalHeader.SizeOfImage; } }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    if (g_lpLoadOn) logf("load: long-frame profiler armed (game thread %u): asleep in normal play, about a thousand samples a second while a frame is longer than 80 ms; a report follows every loading episode.", tid);
    if (g_lpPanOn) logf("pan: profiler armed - while the camera is being scrolled the game thread is sampled through whole frames (about 500 times a second); three seconds after the camera stops, the slow frames are set against the others.");
    if (g_lpRtOn) logf("rtprof: armed - the render thread is sampled a hundred times a second and each sample filed under the queued call it was executing; a report every thirty seconds of play.");
    LONG lastFr = g_mkFrames; DWORD lastChange = GetTickCount(), lastSample = 0, epStart = 0;
    DWORD lastPan = 0, panStart = 0, lastRt = 0, rtStart = 0; HANDLE hRt = NULL; DWORD rtTid = 0;
    profRefreshModules();
    for (;;) {
        DWORD now = GetTickCount();
        LONG fr = g_mkFrames;
        if (fr != lastFr) { lastFr = fr; lastChange = now; }
        // the render thread, a hundred times a second while frames are being drawn
        if (g_lpRtOn && g_rtActive && g_rtWorkerTid && now - lastRt >= 10 && now - lastChange < 200) {
            if (rtTid != g_rtWorkerTid) { if (hRt) CloseHandle(hRt); rtTid = g_rtWorkerTid; hRt = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, rtTid); g_rtDg.on = hRt ? 1 : 0; }
            lastRt = now;
            if (hRt && g_lpRtN < LP_RTMAX && SuspendThread(hRt) != (DWORD)-1) {
                CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
                LpR r; r.eip = GetThreadContext(hRt, &ctx) ? ctx.Eip : 0; r.op = g_rtDg.op; r.kind = g_rtDg.kind; r.pass = g_rtDg.pass;
                ResumeThread(hRt);
                if (r.eip) { if (!g_lpRtN) rtStart = now; if (pmOf(r.eip) < 0) profRefreshModules(); g_lpRt[g_lpRtN++] = r; }
            }
            if (g_lpRtN >= LP_RTMAX || (g_lpRtN >= 600 && now - rtStart >= 30000)) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); lpRtReport(now - rtStart); SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); }
        }
        // the camera is being moved: whole frames
        if (g_lpPanOn && g_lpPanTick && (LONG)(now - g_lpPanTick) < 150) {
            if (!g_lpPanN) panStart = now;
            lastPan = now;
            if (g_lpPanN >= g_lpPanCommitted && g_lpPanCommitted < LP_PANMAX) {
                LONG more = 4096; if (g_lpPanCommitted + more > LP_PANMAX) more = LP_PANMAX - g_lpPanCommitted;
                if (VirtualAlloc(g_lpPan + g_lpPanCommitted, more * sizeof(LpS), MEM_COMMIT, PAGE_READWRITE)) g_lpPanCommitted += more;
            }
            if (g_lpPanN < g_lpPanCommitted) {
                DWORD pf = 0; LpS& s = g_lpPan[g_lpPanN];
                if (lpTake(h, tib, stk, sizeof(stk), &s, &pf)) { s.t = GetTickCount() - panStart; s.frame = pf; g_lpPanN++; }
            }
            Sleep(2);
            continue;
        }
        if (g_lpPanN && now - lastPan > 3000) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); lpPanReport(); SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); }
        if (!g_lpLoadOn) { Sleep(5); continue; }
        bool stuck = now - lastChange >= 80;
        if (!stuck) {
            if (g_lpN && now - lastSample > 3000) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); lpReport(); SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); epStart = 0; }
            Sleep((g_lpN || g_lpPanOn || g_lpRtOn) ? 5 : 20);
            continue;
        }
        if (!epStart) epStart = now;
        if (g_lpN >= LP_MAXS || now - epStart > 600000) {       // the store is full, or ten minutes in one "frame" (a dialog, the window away): report what there is and wait for frames
            if (g_lpN) { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); lpReport(); SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST); }
            while (g_mkFrames == lastFr) Sleep(200);
            epStart = 0;
            continue;
        }
        Sleep(1);
        if (g_lpN >= g_lpCommitted) {                           // the store grows as it is used
            LONG more = 8192; if (g_lpCommitted + more > LP_MAXS) more = LP_MAXS - g_lpCommitted;
            if (more <= 0 || !VirtualAlloc(g_lpLog + g_lpCommitted, more * sizeof(LpS), MEM_COMMIT, PAGE_READWRITE)) { Sleep(50); continue; }
            g_lpCommitted += more;
        }
        LpS& s = g_lpLog[g_lpN];
        if (!lpTake(h, tib, stk, sizeof(stk), &s, NULL)) continue;
        lastSample = GetTickCount();
        s.t = lastSample - epStart;
        g_lpN++;
    }
}
static void installLoadProf() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    bool have = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
    int want = have ? (int)GetPrivateProfileIntA("Engine", "LoadProfile", 0, ini) : 0;
    int pan = have ? (int)GetPrivateProfileIntA("Engine", "PanProfile", 0, ini) : 0;
    int rtp = have ? (int)GetPrivateProfileIntA("Engine", "RenderProfile", 0, ini) : 0;
    char env[16]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_LOADPROF", env, sizeof(env)) && env[0]) want = env[0] - '0';
    env[0] = 0; if (GetEnvironmentVariableA("AOTR_PANPROF", env, sizeof(env)) && env[0]) pan = env[0] - '0';
    env[0] = 0; if (GetEnvironmentVariableA("AOTR_RTPROF", env, sizeof(env)) && env[0]) rtp = env[0] - '0';
    if (want <= 0 && pan <= 0 && rtp <= 0) return;
    g_lpLog = (LpS*)VirtualAlloc(NULL, LP_MAXS * sizeof(LpS), MEM_RESERVE, PAGE_READWRITE);
    g_lpPan = (LpS*)VirtualAlloc(NULL, LP_PANMAX * sizeof(LpS), MEM_RESERVE, PAGE_READWRITE);
    g_lpRt = (LpR*)VirtualAlloc(NULL, LP_RTMAX * sizeof(LpR), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_lpSelf = (LeafEnt*)VirtualAlloc(NULL, LP_TAB * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_lpIncl = (LeafEnt*)VirtualAlloc(NULL, LP_TAB * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_lpBound = (LeafEnt*)VirtualAlloc(NULL, LP_TAB * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_lpTmp = (LeafEnt*)VirtualAlloc(NULL, LP_TAB * sizeof(LeafEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_lpPath = (LpPath*)VirtualAlloc(NULL, LP_PATHS * sizeof(LpPath), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_lpPathTmp = (LpPath*)VirtualAlloc(NULL, LP_PATHS * sizeof(LpPath), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_lpLog || !g_lpPan || !g_lpRt || !g_lpSelf || !g_lpIncl || !g_lpBound || !g_lpTmp || !g_lpPath || !g_lpPathTmp) { logf("load: no memory for the tables - the profilers are off."); return; }
    g_lpLoadOn = want > 0; g_lpPanOn = pan > 0; g_lpRtOn = rtp > 0;
    if (g_lpPanOn) g_lpFrame = &lpOnFrame;
    CreateThread(NULL, 0, lpThread, NULL, 0, NULL);
}
