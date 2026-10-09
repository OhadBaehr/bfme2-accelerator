// aotr_texjobs.inc - texture decoding off the game thread (v51).
//
// WHERE LOADING TIME WENT (measured, 2026-09-18 log and texjob_test.exe on the mod's own files): about 5,500 of the
// mod's 6,750 .dds textures ship with ONE mip level, and the engine asks D3DX for a complete chain. D3DX then
// box-filters and DXT-compresses every missing level - 18 ms for a 1024x1024, 50-70 ms for a 2048x2048 - inside
// D3DXCreateTextureFromFileInMemoryEx, on the game thread, one texture after another. The terrain code does the same
// through D3DXLoadSurfaceFromMemory + D3DXFilterTexture (10 ms per 256x256 tile atlas into DXT), and plain .tga
// files through D3DXFilterTexture. In one match start that was 10.5 s of the load screen and most of the 2-3 fps
// "warm-up" after it (the engine's asset streamer finishes every texture its reader thread delivered, with no time
// budget, at the start of each frame).
//
// WHAT THIS DOES: those D3DX calls now run on worker threads, on memory textures (aotr_texmem.inc), several at once:
//   - the game thread asks D3DX (a probe: D3DX's own code, a device stand-in whose CreateTexture notes the arguments
//     and fails) what texture it would create, creates that texture for real - empty - and returns it at once;
//   - a worker runs the SAME d3dx9_27 function with the SAME arguments and the game thread's own floating-point
//     control word (the output depends on the x87 precision: measured) into a memory texture of that shape;
//   - an "upload" record in the render queue, at the place of the original call, waits for that worker and copies
//     the finished levels into the real texture. Everything queued after it - every draw that can sample the
//     texture - therefore sees exactly the bytes D3DX would have written, exactly when it would have.
// A lock or any other direct use of such a texture by the game finds it named by a queued record and takes the
// existing wait-for-the-queue path, so it also sees the finished content.
//
// PROOF (texjob_test.exe, all 11,911 texture files of the game and the mod): every mip level produced on a memory
// texture is byte-identical to what D3DX leaves in a real DXVK texture, in both x87 precisions, with eight threads
// at once, and after the copy into a real texture. [Engine] TextureCheck = N repeats that comparison in the game
// for the first N textures (D3DX run again on a real scratch texture, on the render thread) and logs the result.
//
// What stays where it was: cube and volume textures, files with a separate alpha file, anything given a palette
// or a partial destination rectangle, textures outside the managed pool, and every call made while the render
// thread is off - they take the old exclusive path, which is also the fallback for anything that fails here.
#define TXJ_KIND       11
#define TXJ_OP         507
#define TXJ_MAXSTEPS   4
#define TXJ_LIVE       64
#define TXJ_MAXTHREADS 16

struct TxjStep {
    LONG kind;                    // 1 whole file, 2 surface from memory, 3 mip filter, 4 surface from a file image
    BYTE* src; DWORD srcSize; LONG srcVirtual;
    DWORD a[10];
    WORD cw; DWORD mxcsr;         // the game thread's floating-point control at the call
    DWORD hr; LONG failed;
    LONG64 ticks;
    DWORD mask;                   // the levels this step writes
    volatile LONG uploaded;       // its levels are in the real texture (set under the exec lock by whoever put them there)
    LONG store, fromDisk;         // worker: keep the result on disk / it came from there
    TxcKey key;
};
struct TxjChain {
    volatile LONG ref;
    TxMem* mem;
    DWORD tex;                    // the real texture
    LONG nSteps, nStarted, queued, running;       // under g_txjCs
    volatile LONG nDone;
    volatile LONG failed;         // a step failed: from there on the real texture is filled the old way, in order
    volatile LONG uploads;        // upload records queued and not executed yet
    volatile LONG closed;         // takes no more steps (its last upload ran)
    DWORD wrote;                  // game thread: levels its steps write
    DWORD bytes;
    LONG check;
    DWORD lastSeq;                // game thread: the sequence number of its newest upload record
    LONG born;                    // the texture was made in the call that made this chain: nothing older can use it
    TxjChain* next;
    TxjStep step[TXJ_MAXSTEPS];
};
struct TxjLive { DWORD tex; TxjChain* c; };

static void* g_txjDev = NULL;
static LONG g_txjInfoStale = 0;
static CRITICAL_SECTION g_txjCs;
static HANDLE g_txjSem = NULL, g_txjRtEvt = NULL, g_txjGtEvt = NULL;
static TxjChain* g_txjHead = NULL, *g_txjTail = NULL;
static TxjLive g_txjLive[TXJ_LIVE]; static LONG g_txjLiveAt = 0;
static LONG g_txjThreads = 0, g_txjWantThreads = 0;
static volatile LONG64 g_txjInflight = 0;
static LONG64 g_txjCap = 96ll << 20;
static volatile LONG g_txjCheckLeft = 0, g_txjChkSame = 0, g_txjChkDiff = 0, g_txjChkDone = 0;
static LONG g_txjCheckAll = 0, g_txjChainNo = 0;       // check every chain (tests) / chains made so far (game thread)
// statistics
static volatile LONG g_txjN[8];                         // chains by first step kind
static volatile LONG g_txjNStep[8], g_txjNFail = 0, g_txjNClassicRedo = 0, g_txjNCreate = 0, g_txjNCheckReq = 0, g_txjNBudget = 0, g_txjNoop = 0;
static volatile LONG64 g_txjWorkTicks = 0, g_txjGameTicks = 0, g_txjRtWaitTicks = 0, g_txjRtCopyTicks = 0, g_txjBudgetTicks = 0, g_txjBytesUp = 0;
static volatile LONG g_txjNEarly = 0, g_txjNEarlyStep = 0;       // chains whose levels the game thread put into the texture itself (it wanted to lock it), and their steps
static volatile LONG64 g_txjEarlyTicks = 0, g_txjEarlyWaitTicks = 0;
static void* g_txjImgInfo = NULL;                            // D3DXGetImageInfoFromFileInMemory
#define TXJ_WHY 12
static volatile LONG g_txjWhy[TXJ_WHY];
static const char* const kTxjWhy[TXJ_WHY] = { "", "arguments it does not take", "D3DX did not reach CreateTexture", "a format it does not take", "no memory", "the texture is still named by queued calls",
    "the surface is not a level of a 2D texture", "not a managed-pool texture", "the texture already has work in flight on those levels", "the real CreateTexture failed", "the terrain atlas mirror handles it", "level count mismatch" };

static __forceinline WORD txjCw() { WORD cw; __asm { fnstcw cw } return cw; }
static __forceinline void txjSetCw(WORD cw) { __asm { fldcw cw } }
static __forceinline bool txjMain() { return g_txjOn && g_rtActive && __readfsdword(0x24) == g_rtMainTid && !g_rtDirectScope && g_txjDev; }

static BYTE* txjSrcCopy(const void* p, DWORD n, LONG* virt) {
    *virt = n >= (256u << 10) ? 1 : 0;
    BYTE* d = *virt ? (BYTE*)VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : (BYTE*)HeapAlloc(g_txHeap, 0, n ? n : 1);
    if (d && n) memcpy(d, p, n);
    return d;
}
static void txjSrcFree(TxjStep* s) {
    if (!s->src) return;
    if (s->srcVirtual) VirtualFree(s->src, 0, MEM_RELEASE); else HeapFree(g_txHeap, 0, s->src);
    s->src = NULL;
    InterlockedExchangeAdd64(&g_txjInflight, -(LONG64)s->srcSize);
}
static void txjChainRelease(TxjChain* c) {
    if (InterlockedDecrement(&c->ref)) return;
    for (LONG i = 0; i < c->nSteps; ++i) txjSrcFree(&c->step[i]);
    if (c->mem) tx_tex_Release(c->mem);
    InterlockedExchangeAdd64(&g_txjInflight, -(LONG64)c->bytes);
    HeapFree(g_txHeap, 0, c);
    SetEvent(g_txjGtEvt);
}

// ---------------------------------------------------------------- workers
typedef DWORD (__stdcall* TxjFileF)(void*, const void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, void*, void*, void**);
typedef DWORD (__stdcall* TxjCreateF)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, void**);
typedef DWORD (__stdcall* TxjReqF)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
typedef DWORD (__stdcall* TxjLoadF)(void*, const void*, const TxRECT*, const void*, DWORD, DWORD, const void*, const TxRECT*, DWORD, DWORD);
typedef DWORD (__stdcall* TxjFilterF)(void*, const void*, DWORD, DWORD);
typedef DWORD (__stdcall* TxjLoadFileF)(void*, const void*, const TxRECT*, const void*, DWORD, const TxRECT*, DWORD, DWORD, void*);
// One step with D3DX, on `tex` (a memory texture on a worker; a real one for the old way and the self-check).
// `dev` is the device D3DX is given for a whole-file step.
static DWORD txjCall(TxjStep* s, void* dev, void* tex, void* levelSurf, void** made) {
    switch (s->kind) {
    case 1: return ((TxjFileF)g_rtXO[3])(dev, s->src, s->srcSize, s->a[0], s->a[1], s->a[2], s->a[3], s->a[4], s->a[5], s->a[6], s->a[7], s->a[8], NULL, NULL, made);
    case 2: { TxRECT rc = { (LONG)s->a[3], (LONG)s->a[4], (LONG)s->a[5], (LONG)s->a[6] };
              return ((TxjLoadF)g_rtXO[10])(levelSurf, NULL, NULL, s->src, s->a[1], s->a[2], NULL, &rc, s->a[7], s->a[8]); }
    case 3: return ((TxjFilterF)g_rtXO[12])(tex, NULL, s->a[0], s->a[1]);
    case 4: return ((TxjLoadFileF)g_rtXO[9])(levelSurf, NULL, NULL, s->src, s->srcSize, NULL, s->a[1], s->a[2], NULL);
    }
    return 0x80004005u;
}
static void txjRunBody(TxjChain* c, TxjStep* s, DWORD* hr, LONG* bad) {
    __try {
        if (s->kind == 1) {
            TxDev jd; txDevInit(&jd, 0, c->mem); void* out = NULL;
            *hr = txjCall(s, &jd, NULL, NULL, &out);
            c->mem->dev = &g_txDevStatic;
            if (out) tx_tex_Release((TxMem*)out);
            if (out != (void*)c->mem || jd.nMismatch || jd.nCreate != 1) *bad = 2;      // D3DX settled on another shape than its probe did
        } else *hr = txjCall(s, NULL, c->mem, (s->kind == 2 || s->kind == 4) ? (void*)&c->mem->sf[s->a[0]] : NULL, NULL);
    } __except (EXCEPTION_EXECUTE_HANDLER) { *bad = 3; *hr = GetExceptionCode(); }
}
static void txjRun(TxjChain* c, TxjStep* s) {
    LONG64 t0 = qpcNow();
    DWORD hr = 0x80004005u; LONG bad = 0;
    bool keyed = false;
    if (g_txcOn && !c->failed && c->mem) {                            // what D3DX made of exactly this before is on disk (aotr_texcache.inc)
        DWORD src = s->a[0] == 0xFFFFFFFFu ? 0 : s->a[0];
        txcKeyOf(&s->key, (DWORD)s->kind, s->a, s->cw, s->mxcsr, c->mem, s->kind == 3 ? NULL : s->src, s->kind == 3 ? 0 : s->srcSize, s->kind == 3 ? src : 0);
        keyed = true;
        if (txcLoad(&s->key, c->mem, s->mask, (DWORD)s->kind, &hr)) s->fromDisk = 1;
    }
    if (!s->fromDisk) {
        WORD cw0 = txjCw(); DWORD mx0 = _mm_getcsr();
        txjSetCw(s->cw); _mm_setcsr(s->mxcsr);
        if (!c->failed) txjRunBody(c, s, &hr, &bad); else bad = 1;    // an earlier step of this texture failed: what this one would read is not there
        if (bad == 3) { __asm { fninit } }
        txjSetCw(cw0); _mm_setcsr(mx0);
        if (!bad && (LONG)hr < 0) bad = 4;
    }
    s->hr = hr;
    if (bad) { s->failed = bad; if (!InterlockedExchange(&c->failed, 1)) InterlockedIncrement(&g_txjNFail); }
    s->ticks = qpcNow() - t0;
    InterlockedExchangeAdd64(&g_txjWorkTicks, s->ticks);
    if (keyed && !s->fromDisk && !bad) {
        if (s->ticks * 1000000 >= g_pqpf.QuadPart * TXC_MIN_US) s->store = 1; else InterlockedIncrement(&g_txcNSmall);
    }
}
static DWORD WINAPI txjWorker(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_txjSem, INFINITE);
        EnterCriticalSection(&g_txjCs);
        TxjChain* c = g_txjHead;
        if (!c) { LeaveCriticalSection(&g_txjCs); continue; }
        g_txjHead = c->next; if (!g_txjHead) g_txjTail = NULL;
        c->next = NULL; c->queued = 0; c->running = 1;
        for (;;) {
            LONG k = c->nStarted;
            if (k >= c->nSteps) { c->running = 0; break; }
            c->nStarted = k + 1;
            LeaveCriticalSection(&g_txjCs);
            TxjStep* s = &c->step[k];
            txjRun(c, s);
            TxMem* keep = NULL;                                 // the result goes to disk AFTER it is handed on, on the writer thread; the memory texture must outlive that
            EnterCriticalSection(&g_txjCs);                     // (the levels a step wrote are no later step's: reading them while the chain goes on is safe)
            if (s->store && c->mem) { keep = c->mem; InterlockedIncrement(&keep->ref); }
            c->nDone = k + 1;
            SetEvent(g_txjRtEvt); SetEvent(g_txjGtEvt);
            if (keep && !txcStoreLater(&s->key, keep, s->mask, (DWORD)s->kind, s->hr, (DWORD)(s->ticks * 1000000 / g_pqpf.QuadPart))) {
                LeaveCriticalSection(&g_txjCs);
                tx_tex_Release(keep);
                EnterCriticalSection(&g_txjCs);
            }
        }
        LeaveCriticalSection(&g_txjCs);
        txjChainRelease(c);                                 // the queue's reference
    }
}
static void txjStartWorkers() {
    if (g_txjThreads) return;
    LONG n = g_txjWantThreads;
    if (n <= 0) { SYSTEM_INFO si; GetSystemInfo(&si); n = (LONG)si.dwNumberOfProcessors / 2 - 2; if (n < 2) n = 2; if (n > 8) n = 8; }
    if (n > TXJ_MAXTHREADS) n = TXJ_MAXTHREADS;
    for (LONG i = 0; i < n; ++i) {
        HANDLE th = CreateThread(NULL, 0, txjWorker, NULL, 0, NULL);
        if (!th) break;
        SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(th); g_txjThreads++;
    }
    logf("texjobs: %d worker threads started.", (int)g_txjThreads);
}
// game thread: make the chain's pending steps runnable
static void txjKick(TxjChain* c) {                         // g_txjCs held
    if (c->queued || c->running) return;
    c->queued = 1; c->next = NULL; InterlockedIncrement(&c->ref);
    if (g_txjTail) g_txjTail->next = c; else g_txjHead = c;
    g_txjTail = c;
    ReleaseSemaphore(g_txjSem, 1, NULL);
}

// ---------------------------------------------------------------- game thread: chains
static TxjChain* txjLiveFind(DWORD tex) {
    for (int i = 0; i < TXJ_LIVE; ++i) {
        if (g_txjLive[i].tex != tex || !g_txjLive[i].c) continue;
        TxjChain* c = g_txjLive[i].c;
        if (!c->closed) return c;
        g_txjLive[i].tex = 0; g_txjLive[i].c = NULL; txjChainRelease(c);
    }
    return NULL;
}
static void txjLiveDrop(DWORD tex) {
    for (int i = 0; i < TXJ_LIVE; ++i) if (g_txjLive[i].tex == tex && g_txjLive[i].c) { TxjChain* c = g_txjLive[i].c; g_txjLive[i].tex = 0; g_txjLive[i].c = NULL; txjChainRelease(c); }
}
static void txjLiveAdd(DWORD tex, TxjChain* c) {
    txjLiveDrop(tex);
    TxjLive& e = g_txjLive[(g_txjLiveAt++) & (TXJ_LIVE - 1)];
    if (e.c) txjChainRelease(e.c);
    e.tex = tex; e.c = c; InterlockedIncrement(&c->ref);
}
static void txjBudget(DWORD add) {                         // game thread, no lock held
    if (rd64(&g_txjInflight) + (LONG64)add <= g_txjCap || rd64(&g_txjInflight) <= 0) return;
    if (g_rtFxRec) rtFxClose();
    rtPublish();                                           // the records that free this memory have to be visible to the render thread
    LONG64 t0 = qpcNow();
    { RtStuck st; rtStuckBegin(st);                      // (it is the render thread that frees this memory)
      while (rd64(&g_txjInflight) + (LONG64)add > g_txjCap && rd64(&g_txjInflight) > 0) rtWaitH(g_txjGtEvt, 1, rtStuckNow(st)); }
    g_txjBudgetTicks += qpcNow() - t0; g_txjNBudget++;
}
static TxjChain* txjChainNew(TxMem* m, DWORD tex) {
    TxjChain* c = (TxjChain*)HeapAlloc(g_txHeap, HEAP_ZERO_MEMORY, sizeof(TxjChain));
    if (!c) return NULL;
    c->ref = 1; c->mem = m; c->tex = tex; c->bytes = m->memSize;
    InterlockedExchangeAdd64(&g_txjInflight, (LONG64)c->bytes);
    LONG no = g_txjChainNo++;
    if (g_txjCheckLeft > 0 && (g_txjCheckAll || no < 8 || (no & 15) == 0)) { InterlockedDecrement(&g_txjCheckLeft); c->check = 1; }
    return c;
}
// append a step and queue its upload record. The caller has filled `s` (src already copied). false: the chain is
// closed or full - nothing was changed.
static bool txjAddStep(TxjChain* c, const TxjStep* s, DWORD tex, DWORD mask) {
    EnterCriticalSection(&g_txjCs);
    if (c->closed || c->nSteps >= TXJ_MAXSTEPS) { LeaveCriticalSection(&g_txjCs); return false; }
    LONG k = c->nSteps;
    c->step[k] = *s; c->step[k].cw = txjCw(); c->step[k].mxcsr = _mm_getcsr(); c->step[k].mask = mask;
    c->nSteps = k + 1;
    InterlockedIncrement(&c->uploads);
    txjKick(c);
    LeaveCriticalSection(&g_txjCs);
    InterlockedExchangeAdd64(&g_txjInflight, (LONG64)s->srcSize);
    c->wrote |= mask;
    InterlockedIncrement(&g_txjNStep[s->kind & 7]);
    InterlockedIncrement(&c->ref);                         // the record's reference
    RtRec* r = rtAlloc(4, 0); r->fn = NULL; r->op = TXJ_OP;
    DWORD* A = rtArgs(r);
    // The record names the texture like any other (a lock finds it pending) - but a texture nothing ELSE has named
    // yet stays in the "new" set: only its own uploads are on their way to it, and those the game thread can bring
    // forward itself when it wants to lock it (txjFlushNow).
    A[0] = rtObjKeepNew(r, 0, tex); A[1] = (DWORD)(ULONG_PTR)c; A[2] = (DWORD)(k + 1); A[3] = mask;
    r->kind = TXJ_KIND;
    rtCommit(r, 0);
    c->lastSeq = r->seq;
    return true;
}
// is this object one of the 2D textures / surfaces whose methods we hold?
static __forceinline bool txjIsTex(DWORD t) { return t && (*(void***)(ULONG_PTR)t)[19] == g_rtH_tex[19] && ((DWORD(__stdcall*)(DWORD))g_rtO_tex[10])(t) == 3; }
static __forceinline bool txjIsSurf(DWORD s) { return s && (*(void***)(ULONG_PTR)s)[13] == g_rtH_surf[13]; }
static bool txjShape(DWORD tex, TxDESC* d, DWORD* levels) {
    if ((LONG)((DWORD(__stdcall*)(DWORD, DWORD, TxDESC*))g_rtO_tex[17])(tex, 0, d) < 0) return false;
    *levels = ((DWORD(__stdcall*)(DWORD))g_rtO_tex[13])(tex);
    return *levels > 0 && *levels <= TX_MAXLEVELS;
}
static void txjInfoRefresh() {                             // game thread
    if (!g_txjInfoStale) return;
    g_txjInfoStale = 0;
    rtExecLock(); g_rtDirectScope++;
    txForgetFormats(); txInfoFill(g_txjDev, g_rtO_dev);
    g_rtDirectScope--; LeaveCriticalSection(&g_rtExecCs);
}

// ---------------------------------------------------------------- game thread: the five D3DX entry points
// Each returns true when it has done the call's work (hr is what the caller returns), false for the old path.
static bool txjCheckReq(DWORD* A, DWORD* hr) {             // D3DXCheckTextureRequirements(device, &w, &h, &levels, usage, &format, pool)
    if (!txjMain() || (void*)(ULONG_PTR)A[0] != g_txjDev) return false;
    txjInfoRefresh();
    *hr = ((TxjReqF)g_rtXO[8])(&g_txDevStatic, A[1], A[2], A[3], A[4], A[5], A[6]);     // the same questions, answered from memory
    g_txjNCheckReq++;
    return true;
}
static bool txjCreateTexture(DWORD* A, DWORD* hr) {        // D3DXCreateTexture(device, w, h, levels, usage, format, pool, &tex)
    if (!txjMain() || (void*)(ULONG_PTR)A[0] != g_txjDev || !A[7]) return false;
    txjInfoRefresh();
    TxDev pd; txDevInit(&pd, 1, NULL); void* dummy = NULL;
    ((TxjCreateF)g_rtXO[2])(&pd, A[1], A[2], A[3], A[4], A[5], A[6], &dummy);
    if (pd.nCreate != 1) { g_txjWhy[2]++; return false; }
    DWORD r = rt_dev_23(g_txjDev, pd.cw, pd.ch, pd.clevels, pd.cusage, pd.cfmt, pd.cpool, A[7], 0);
    if ((LONG)r < 0) { g_txjWhy[9]++; return false; }      // let D3DX itself meet the failure
    g_txjNCreate++;
    *hr = 0; return true;
}
static bool txjCreateFromFile(DWORD* A, DWORD* hr) {       // D3DXCreateTextureFromFileInMemoryEx(device, data, size, w, h, levels, usage, format, pool, filter, mipfilter, colorkey, info, palette, &tex)
    if (!txjMain() || (void*)(ULONG_PTR)A[0] != g_txjDev) return false;
    if (!A[1] || !A[2] || !A[14] || A[12] || A[13] || A[6] != 0 || A[8] != 1) { g_txjWhy[1]++; return false; }
    LONG64 t0 = qpcNow();
    txjInfoRefresh();
    TxDev pd; txDevInit(&pd, 1, NULL); void* dummy = NULL;
    ((TxjFileF)g_rtXO[3])(&pd, (const void*)(ULONG_PTR)A[1], A[2], A[3], A[4], A[5], A[6], A[7], A[8], A[9], A[10], A[11], NULL, NULL, &dummy);
    DWORD bytes, block;
    if (pd.nCreate != 1) { g_txjWhy[2]++; return false; }
    if (pd.cpool != 1 || pd.cusage != 0) { g_txjWhy[7]++; return false; }
    if (!txFmtInfo(pd.cfmt, &bytes, &block)) { g_txjWhy[3]++; return false; }
    txjStartWorkers();
    TxMem* m = txMemCreate(pd.cw, pd.ch, pd.clevels, 0, pd.cfmt, 1);
    if (!m) { g_txjWhy[4]++; return false; }
    txjBudget(m->memSize + A[2]);
    DWORD tex = 0;
    DWORD r = rt_dev_23(g_txjDev, pd.cw, pd.ch, pd.clevels, 0, pd.cfmt, 1, (DWORD)(ULONG_PTR)&tex, 0);
    if ((LONG)r < 0 || !tex) { tx_tex_Release(m); g_txjWhy[9]++; return false; }
    if (((DWORD(__stdcall*)(DWORD))g_rtO_tex[13])(tex) != m->levels) {                     // the device made another level count than D3DX asked for
        ((DWORD(__stdcall*)(DWORD))g_rtO_tex[2])(tex); tx_tex_Release(m); g_txjWhy[11]++; return false; }
    TxjStep s; memset(&s, 0, sizeof(s)); s.kind = 1;
    s.src = txjSrcCopy((const void*)(ULONG_PTR)A[1], A[2], &s.srcVirtual); s.srcSize = A[2];
    TxjChain* c = s.src ? txjChainNew(m, tex) : NULL;
    if (!c) { if (s.src) { if (s.srcVirtual) VirtualFree(s.src, 0, MEM_RELEASE); else HeapFree(g_txHeap, 0, s.src); }
              ((DWORD(__stdcall*)(DWORD))g_rtO_tex[2])(tex); tx_tex_Release(m); g_txjWhy[4]++; return false; }
    for (int i = 0; i < 9; ++i) s.a[i] = A[3 + i];         // w, h, levels, usage, format, pool, filter, mipfilter, colorkey
    c->born = 1;
    txjAddStep(c, &s, tex, (1u << m->levels) - 1);
    txjLiveAdd(tex, c);
    txjChainRelease(c);
    *(DWORD*)(ULONG_PTR)A[14] = tex;
    InterlockedIncrement(&g_txjN[1]);
    g_txjGameTicks += qpcNow() - t0;
    *hr = 0; return true;
}
static bool txjLoadSurface(DWORD* A, DWORD* hr) {          // D3DXLoadSurfaceFromMemory(surface, destPalette, destRect, src, srcFormat, srcPitch, srcPalette, srcRect, filter, colorkey)
    if (!txjMain()) return false;
    DWORD surf = A[0];
    if (!surf || A[1] || A[2] || !A[3] || A[6] || !A[7] || !txjIsSurf(surf)) { g_txjWhy[1]++; return false; }
    LONG64 t0 = qpcNow();
    const TxRECT* rc = (const TxRECT*)(ULONG_PTR)A[7];
    DWORD sb, sblock;
    if (!txFmtInfo(A[4], &sb, &sblock) || rc->l < 0 || rc->t < 0 || rc->r <= rc->l || rc->b <= rc->t || rc->r > 16384 || rc->b > 16384 || !A[5] || (LONG)A[5] < 0) { g_txjWhy[1]++; return false; }
    DWORD need = sblock ? (((DWORD)rc->b + 3) / 4) * A[5] : ((DWORD)rc->b - 1) * A[5] + (DWORD)rc->r * sb;     // what D3DX can read of the source
    void* cont = NULL;
    if ((LONG)((DWORD(__stdcall*)(DWORD, const GUID*, void**))g_rtO_surf[11])(surf, &kTxIID_Texture, &cont) < 0 || !cont) { g_txjWhy[6]++; return false; }
    DWORD tex = (DWORD)(ULONG_PTR)cont; bool ok = false; TxDESC d; DWORD levels = 0, level = 0;
    if (txjIsTex(tex) && txjShape(tex, &d, &levels)) {
        for (DWORD i = 0; i < levels && !ok; ++i) {
            void* ls = NULL; ((DWORD(__stdcall*)(DWORD, DWORD, void**))g_rtO_tex[18])(tex, i, &ls);
            if (ls) ((DWORD(__stdcall*)(void*))g_rtO_surf[2])(ls);
            if ((DWORD)(ULONG_PTR)ls == surf) { level = i; ok = true; }
        }
    }
    ((DWORD(__stdcall*)(DWORD))g_rtO_tex[2])(tex);         // GetContainer's reference (the surface's caller holds the texture)
    if (!ok) { g_txjWhy[6]++; return false; }
    DWORD bytes, block;
    if (d.Pool != 1 || d.Usage != 0) { g_txjWhy[7]++; return false; }
    if (!txFmtInfo(d.Format, &bytes, &block)) { g_txjWhy[3]++; return false; }
    DWORD bit = 1u << level;
    TxjChain* c = txjLiveFind(tex);
    if (c && ((c->wrote & bit) || c->nSteps >= TXJ_MAXSTEPS)) { g_txjWhy[8]++; return false; }   // those levels are still on their way: the old path waits for them
    txjStartWorkers();
    bool fresh = !c;
    if (!c) {
        TxMem* m = txMemCreate(d.Width, d.Height, levels, 0, d.Format, 1);
        if (!m || m->levels != levels) { if (m) tx_tex_Release(m); g_txjWhy[4]++; return false; }
        txjBudget(m->memSize + need);
        c = txjChainNew(m, tex);
        if (!c) { tx_tex_Release(m); g_txjWhy[4]++; return false; }
        c->born = rtFreshHas(tex) ? 1 : 0;
    }
    TxjStep s; memset(&s, 0, sizeof(s)); s.kind = 2;
    s.src = txjSrcCopy((const void*)(ULONG_PTR)A[3], need, &s.srcVirtual); s.srcSize = need;
    if (!s.src) { if (fresh) txjChainRelease(c); g_txjWhy[4]++; return false; }
    s.a[0] = level; s.a[1] = A[4]; s.a[2] = A[5]; s.a[3] = (DWORD)rc->l; s.a[4] = (DWORD)rc->t; s.a[5] = (DWORD)rc->r; s.a[6] = (DWORD)rc->b; s.a[7] = A[8]; s.a[8] = A[9];
    if (g_rtTMAny) { rtTMKill(tex); rtTMKill(surf); }      // a mirror of this level would no longer be the level
    if (!txjAddStep(c, &s, tex, bit)) {
        if (s.srcVirtual) VirtualFree(s.src, 0, MEM_RELEASE); else HeapFree(g_txHeap, 0, s.src);
        if (fresh) txjChainRelease(c);
        g_txjWhy[8]++; return false;
    }
    if (fresh) { txjLiveAdd(tex, c); txjChainRelease(c); InterlockedIncrement(&g_txjN[2]); }
    g_txjGameTicks += qpcNow() - t0;
    *hr = 0; return true;
}
// D3DXLoadSurfaceFromFileInMemory(surface, destPalette, destRect, data, size, srcRect, filter, colorkey, info) into a
// whole level of a managed texture: the engine's path for a colour file that has its alpha in a second file
// (0x00530E48: D3DXCreateTexture, this, then the alpha merged in by hand) - 117 ms a call in the load of 2026-10-07.
// D3DX is asked first whether it knows the file at all (its own header check): if not, the call is D3DX's to fail.
static bool txjLoadSurfaceFile(DWORD* A, DWORD* hr) {
    if (!txjMain() || !g_rtXO[9] || !g_txjImgInfo) return false;
    DWORD surf = A[0];
    if (!surf || A[1] || A[2] || !A[3] || !A[4] || A[5] || A[8] || !txjIsSurf(surf)) { g_txjWhy[1]++; return false; }
    LONG64 t0 = qpcNow();
    { BYTE info[64]; if ((LONG)((DWORD(__stdcall*)(const void*, DWORD, void*))g_txjImgInfo)((const void*)(ULONG_PTR)A[3], A[4], info) < 0) { g_txjWhy[1]++; return false; } }
    void* cont = NULL;
    if ((LONG)((DWORD(__stdcall*)(DWORD, const GUID*, void**))g_rtO_surf[11])(surf, &kTxIID_Texture, &cont) < 0 || !cont) { g_txjWhy[6]++; return false; }
    DWORD tex = (DWORD)(ULONG_PTR)cont; bool ok = false; TxDESC d; DWORD levels = 0, level = 0;
    if (txjIsTex(tex) && txjShape(tex, &d, &levels)) {
        for (DWORD i = 0; i < levels && !ok; ++i) {
            void* ls = NULL; ((DWORD(__stdcall*)(DWORD, DWORD, void**))g_rtO_tex[18])(tex, i, &ls);
            if (ls) ((DWORD(__stdcall*)(void*))g_rtO_surf[2])(ls);
            if ((DWORD)(ULONG_PTR)ls == surf) { level = i; ok = true; }
        }
    }
    ((DWORD(__stdcall*)(DWORD))g_rtO_tex[2])(tex);
    if (!ok) { g_txjWhy[6]++; return false; }
    DWORD bytes, block;
    if (d.Pool != 1 || d.Usage != 0) { g_txjWhy[7]++; return false; }
    if (!txFmtInfo(d.Format, &bytes, &block)) { g_txjWhy[3]++; return false; }
    DWORD bit = 1u << level;
    TxjChain* c = txjLiveFind(tex);
    if (c && ((c->wrote & bit) || c->nSteps >= TXJ_MAXSTEPS)) { g_txjWhy[8]++; return false; }
    txjStartWorkers();
    bool fresh = !c;
    if (!c) {
        TxMem* m = txMemCreate(d.Width, d.Height, levels, 0, d.Format, 1);
        if (!m || m->levels != levels) { if (m) tx_tex_Release(m); g_txjWhy[4]++; return false; }
        txjBudget(m->memSize + A[4]);
        c = txjChainNew(m, tex);
        if (!c) { tx_tex_Release(m); g_txjWhy[4]++; return false; }
        c->born = rtFreshHas(tex) ? 1 : 0;
    }
    TxjStep s; memset(&s, 0, sizeof(s)); s.kind = 4;
    s.src = txjSrcCopy((const void*)(ULONG_PTR)A[3], A[4], &s.srcVirtual); s.srcSize = A[4];
    if (!s.src) { if (fresh) txjChainRelease(c); g_txjWhy[4]++; return false; }
    s.a[0] = level; s.a[1] = A[6]; s.a[2] = A[7];
    if (g_rtTMAny) { rtTMKill(tex); rtTMKill(surf); }
    if (!txjAddStep(c, &s, tex, bit)) {
        if (s.srcVirtual) VirtualFree(s.src, 0, MEM_RELEASE); else HeapFree(g_txHeap, 0, s.src);
        if (fresh) txjChainRelease(c);
        g_txjWhy[8]++; return false;
    }
    if (fresh) { txjLiveAdd(tex, c); txjChainRelease(c); InterlockedIncrement(&g_txjN[4]); }
    g_txjGameTicks += qpcNow() - t0;
    *hr = 0; return true;
}
static bool txjFilter(DWORD* A, DWORD* hr) {               // D3DXFilterTexture(texture, palette, sourceLevel, filter)
    if (!txjMain()) return false;
    DWORD tex = A[0], src = A[2] == 0xFFFFFFFFu ? 0 : A[2];
    if (!tex || A[1] || !txjIsTex(tex)) { g_txjWhy[1]++; return false; }
    LONG64 t0 = qpcNow();
    TxDESC d; DWORD levels = 0, bytes, block;
    if (!txjShape(tex, &d, &levels) || src >= levels) { g_txjWhy[1]++; return false; }
    if (d.Pool != 1 || d.Usage != 0) { g_txjWhy[7]++; return false; }
    if (!txFmtInfo(d.Format, &bytes, &block)) { g_txjWhy[3]++; return false; }
    DWORD mask = ((1u << levels) - 1) & ~((2u << src) - 1);                          // the levels below the source level
    TxjChain* c = txjLiveFind(tex);
    if (c) {
        if (!(c->wrote & (1u << src)) || (c->wrote & mask) || c->nSteps >= TXJ_MAXSTEPS) { g_txjWhy[8]++; return false; }
        if (!mask) { *hr = 0; g_txjNoop++; return true; }
    } else {
        if (g_rtTMAny && rtTMIsMirroredTex(tex)) { g_txjWhy[10]++; return false; }
        // the source level has to be read now: only when nothing queued can still change it
        if (!mask) { *hr = 0; g_txjNoop++; return true; }   // one level: nothing to build, and D3DX would only have said so
        TxMem* m = txMemCreate(d.Width, d.Height, levels, 0, d.Format, 1);
        if (!m || m->levels != levels) { if (m) tx_tex_Release(m); g_txjWhy[4]++; return false; }
        txjStartWorkers();
        txjBudget(m->memSize);
        rtExecLock(); g_rtDirectScope++;
        void* ls = NULL; ((DWORD(__stdcall*)(DWORD, DWORD, void**))g_rtO_tex[18])(tex, src, &ls);
        if (ls) ((DWORD(__stdcall*)(void*))g_rtO_surf[2])(ls);
        bool pend = rtRefPending(tex) || (ls && rtRefPending((DWORD)(ULONG_PTR)ls));
        bool got = !pend && txDownload(m, (void*)(ULONG_PTR)tex, g_rtO_tex[19], g_rtO_tex[20], src);
        g_rtDirectScope--; LeaveCriticalSection(&g_rtExecCs);
        if (!got) { tx_tex_Release(m); g_txjWhy[pend ? 5 : 4]++; return false; }
        c = txjChainNew(m, tex);
        if (!c) { tx_tex_Release(m); g_txjWhy[4]++; return false; }
        c->wrote = 1u << src;                              // known, not written: nothing to upload for it
        txjLiveAdd(tex, c); txjChainRelease(c);
        InterlockedIncrement(&g_txjN[3]);
    }
    TxjStep s; memset(&s, 0, sizeof(s)); s.kind = 3; s.a[0] = A[2]; s.a[1] = A[3];
    if (!txjAddStep(c, &s, tex, mask)) { g_txjWhy[8]++; txjLiveDrop(tex); return false; }    // closed in this very moment: its content is in the real texture now
    g_txjGameTicks += qpcNow() - t0;
    *hr = 0; return true;
}

// ---------------------------------------------------------------- render thread: the upload record
struct TxjSnapLevel { DWORD rows, rowBytes; };
// compare levels of a real texture with the memory texture's; returns the first level that differs or -1
static int txjCompare(TxMem* m, DWORD tex, DWORD mask, DWORD* at) {
    for (DWORD i = 0; i < m->levels; ++i) {
        if (!((mask >> i) & 1)) continue;
        TxLOCKED lr = { 0, NULL };
        if ((LONG)((DWORD(__stdcall*)(DWORD, DWORD, TxLOCKED*, const TxRECT*, DWORD))g_rtO_tex[19])(tex, i, &lr, NULL, 0x10) < 0 || !lr.bits) { *at = 0xFFFFFFFFu; return (int)i; }
        const TxLevel& L = m->lv[i]; int bad = -1;
        for (DWORD y = 0; y < L.rows && bad < 0; ++y)
            if (memcmp((const BYTE*)lr.bits + (INT)y * lr.pitch, L.bits + y * L.pitch, L.rowBytes)) {
                const BYTE* p = (const BYTE*)lr.bits + (INT)y * lr.pitch; const BYTE* q = L.bits + y * L.pitch; DWORD x = 0; while (p[x] == q[x]) ++x;
                *at = y * L.rowBytes + x; bad = (int)i; }
        ((DWORD(__stdcall*)(DWORD, DWORD))g_rtO_tex[20])(tex, i);
        if (bad >= 0) return bad;
    }
    return -1;
}
// the step once more, by D3DX on a REAL texture, and the two results compared (render thread, exec lock held)
static void txjSelfCheck(TxjChain* c, TxjStep* s, DWORD mask) {
    TxMem* m = c->mem; DWORD scratch = 0; void* made = NULL; DWORD hr = 0;
    WORD cw0 = txjCw(); DWORD mx0 = _mm_getcsr();
    txjSetCw(s->cw); _mm_setcsr(s->mxcsr);
    if (s->kind == 1) { hr = txjCall(s, g_txjDev, NULL, NULL, &made); scratch = (DWORD)(ULONG_PTR)made; }
    else {
        void* t = NULL;
        ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, void**, void*))g_rtO_dev[23])(g_txjDev, m->w, m->h, m->levels, 0, m->fmt, 1, &t, NULL);
        scratch = (DWORD)(ULONG_PTR)t;
        if (scratch && (s->kind == 2 || s->kind == 4)) {
            void* ls = NULL; ((DWORD(__stdcall*)(DWORD, DWORD, void**))g_rtO_tex[18])(scratch, s->a[0], &ls);
            hr = ls ? txjCall(s, NULL, NULL, ls, NULL) : 0x80004005u;
            if (ls) ((DWORD(__stdcall*)(void*))g_rtO_surf[2])(ls);
        } else if (scratch) {
            DWORD src = s->a[0] == 0xFFFFFFFFu ? 0 : s->a[0];
            txUpload(m, (void*)(ULONG_PTR)scratch, g_rtO_tex[19], g_rtO_tex[20], 1u << src);
            hr = txjCall(s, NULL, (void*)(ULONG_PTR)scratch, NULL, NULL);
        }
    }
    txjSetCw(cw0); _mm_setcsr(mx0);
    DWORD at = 0; int bad = -2;
    if (scratch && (LONG)hr >= 0) {
        TxDESC d; DWORD levels = 0;
        if (!txjShape(scratch, &d, &levels) || levels != m->levels || d.Format != m->fmt || d.Width != m->w || d.Height != m->h) bad = 100;
        else bad = txjCompare(m, scratch, mask & (DWORD)m->written, &at);
    }
    if (scratch) ((DWORD(__stdcall*)(DWORD))g_rtO_tex[2])(scratch);
    static const char* const kind[8] = { "", "whole file", "surface from memory", "mip filter", "surface from a file", "", "", "" };
    if (bad == -1) InterlockedIncrement(&g_txjChkSame);
    else { InterlockedIncrement(&g_txjChkDiff);
           logf("texjobs: SELF-CHECK DIFFERENCE - %s%s, %ux%u format %08X, %u levels: %s (level %d, byte %u; D3DX on the real texture returned %08X).", kind[s->kind & 7], s->fromDisk ? " READ FROM DISK" : "", m->w, m->h, m->fmt, m->levels,
                bad == 100 ? "the real texture has another shape" : bad == -2 ? "the comparison run failed" : "the bytes differ", bad, at, hr); }
    InterlockedIncrement(&g_txjChkDone);
}
// a step that failed on its memory texture: done the old way on the real one, here, in its place in the queue
static void txjRedo(TxjChain* c, TxjStep* s, DWORD tex) {
    InterlockedIncrement(&g_txjNClassicRedo);
    if (s->kind == 1) { logf("texjobs: D3DX failed on a texture file it had accepted (%08X, %ux%u format %08X) - the texture keeps what was decoded.", s->hr, c->mem->w, c->mem->h, c->mem->fmt);
                        txUpload(c->mem, (void*)(ULONG_PTR)tex, g_rtO_tex[19], g_rtO_tex[20], (DWORD)c->mem->written); return; }
    WORD cw0 = txjCw(); DWORD mx0 = _mm_getcsr();
    txjSetCw(s->cw); _mm_setcsr(s->mxcsr);
    if (s->kind == 2 || s->kind == 4) {
        void* ls = NULL; ((DWORD(__stdcall*)(DWORD, DWORD, void**))g_rtO_tex[18])(tex, s->a[0], &ls);
        if (ls) { txjCall(s, NULL, NULL, ls, NULL); ((DWORD(__stdcall*)(void*))g_rtO_surf[2])(ls); }
    } else txjCall(s, NULL, (void*)(ULONG_PTR)tex, NULL, NULL);
    txjSetCw(cw0); _mm_setcsr(mx0);
}
static void txjExecUpload(RtRec* r) {                      // render thread, exec lock held
    DWORD* a = rtArgs(r);
    DWORD tex = a[0]; TxjChain* c = (TxjChain*)(ULONG_PTR)a[1]; LONG need = (LONG)a[2]; DWORD mask = a[3];
    if (c->nDone < need && !c->step[need - 1].uploaded) {   // not decoded yet: wait for it, with the lock open for the game thread's direct calls
        LONG64 t0 = qpcNow();
        g_rtExecSeq = r->seq - 1;                          // everything before this record is done: a call waiting for one of those need not wait for this
        if (g_rtDrainWaiting && g_rtMainSleeping && rtSeqReached(g_rtExecSeq, g_rtDrainSeq)) SetEvent(g_rtDrained);
        LeaveCriticalSection(&g_rtExecCs);
        while (c->nDone < need) {
            DWORD ms = 2;
            g_rtWH.beat++;                                 // (alive: waiting for a decoding thread, not for the game thread)
            if (g_twBusy) {
                if (!g_rtLK.want) { EnterCriticalSection(&g_rtExecCs); twService(); LeaveCriticalSection(&g_rtExecCs); }
                DWORD t = twIdleWaitMs(); if (t < ms) ms = t;
            }
            WaitForSingleObject(g_txjRtEvt, ms);
        }
        EnterCriticalSection(&g_rtExecCs);
        LONG64 dt = qpcNow() - t0;
        g_txjRtWaitTicks += dt; g_rtBusyTicks -= dt;         // waiting is not work
    }
    TxjStep* s = &c->step[need - 1];
    LONG64 t1 = qpcNow();
    if (s->uploaded) { }                                   // the game thread wanted the texture and put the levels in itself (txjFlushNow)
    else if (c->failed) { txjRedo(c, s, tex); s->uploaded = 1; }
    else if (c->mem) {
        DWORD up = mask & (DWORD)c->mem->written;
        txUpload(c->mem, (void*)(ULONG_PTR)tex, g_rtO_tex[19], g_rtO_tex[20], up);
        for (DWORD i = 0; i < c->mem->levels; ++i) if ((up >> i) & 1) g_txjBytesUp += c->mem->lv[i].size;
        if (c->check) txjSelfCheck(c, s, mask);
        s->uploaded = 1;
    }
    g_txjRtCopyTicks += qpcNow() - t1;
    txjSrcFree(s);
    if (r->objMask & 1) ((DWORD(__stdcall*)(DWORD))(*(DWORD**)(ULONG_PTR)tex)[2])(tex);
    TxMem* gone = NULL; DWORD goneBytes = 0;
    EnterCriticalSection(&g_txjCs);
    if (InterlockedDecrement(&c->uploads) == 0 && !c->closed) { c->closed = 1; gone = c->mem; c->mem = NULL; goneBytes = c->bytes; c->bytes = 0; }   // closed: no step is pending and none will be taken
    LeaveCriticalSection(&g_txjCs);
    if (gone) { tx_tex_Release(gone); InterlockedExchangeAdd64(&g_txjInflight, -(LONG64)goneBytes); SetEvent(g_txjGtEvt); }
    txjChainRelease(c);
}

// ---------------------------------------------------------------- game thread: a new texture's levels, now
// The engine reads some textures back right after loading them (0x00531C77 keeps the pixels of A8R8G8B8 / A4R4G4B4
// textures in its own memory; 0x00530E48 merges a second file's alpha in). The texture is named by its upload
// records, so that lock used to wait for the WHOLE queue - every draw queued before it and every other texture's
// decoding. But a texture that was made in the very call that started its chain, and that nothing but its own
// upload records has named since, is used by no queued draw: its levels can go into it at any moment, in any
// order relative to the rest of the queue. So the game thread waits for THIS texture's workers only, copies the
// levels in itself, and closes the chain; the upload records find their work done when the render thread gets to
// them. true: the texture now holds everything that was on its way to it (and is no longer pending).
static bool txjFlushNow(DWORD tex) {
    TxjChain* c = txjLiveFind(tex);
    if (!c || !c->born) return false;
    LONG xi = rtXFind(tex);
    if (xi < 0 || g_rtX[xi].seq != c->lastSeq) return false;      // something else has named it since (or its entry is a guess): the queue decides
    InterlockedIncrement(&c->ref);
    LONG64 t0 = qpcNow();
    while (c->nDone < c->nSteps) rtWaitH(g_txjGtEvt, 1, false);      // the decoding threads: nothing of theirs waits for a window
    LONG64 t1 = qpcNow();
    rtExecLock(); g_rtDirectScope++;
    LONG did = 0;
    for (LONG k = 0; k < c->nSteps; ++k) {
        TxjStep* s = &c->step[k];
        if (s->uploaded) continue;
        if (c->failed) txjRedo(c, s, tex);
        else if (c->mem) {
            DWORD up = s->mask & (DWORD)c->mem->written;
            txUpload(c->mem, (void*)(ULONG_PTR)tex, g_rtO_tex[19], g_rtO_tex[20], up);
            for (DWORD i = 0; i < c->mem->levels; ++i) if ((up >> i) & 1) g_txjBytesUp += c->mem->lv[i].size;
        }
        s->uploaded = 1; ++did;
    }
    xi = rtXFind(tex);                                     // (looked up again: the wait above lets window messages through)
    if (xi >= 0) g_rtX[xi].seq = g_rtExecSeq;              // its queued records are empty now: nothing is on its way to it any more
    g_rtDirectScope--; LeaveCriticalSection(&g_rtExecCs);
    TxMem* gone = NULL; DWORD goneBytes = 0;
    EnterCriticalSection(&g_txjCs);
    if (!c->closed) { c->closed = 1; gone = c->mem; c->mem = NULL; goneBytes = c->bytes; c->bytes = 0; }
    LeaveCriticalSection(&g_txjCs);
    if (gone) { tx_tex_Release(gone); InterlockedExchangeAdd64(&g_txjInflight, -(LONG64)goneBytes); }
    txjChainRelease(c);
    g_txjNEarly++; g_txjNEarlyStep += did;
    g_txjEarlyWaitTicks += t1 - t0; g_txjEarlyTicks += qpcNow() - t0;
    return true;
}

// ---------------------------------------------------------------- install, reset, report
static void txjOnReset() { if (g_txjDev) g_txjInfoStale = 1; }
static void txjOnInstall(void* dev) {                      // game thread, device methods still original (g_rtO_dev holds them)
    if (!g_txjOn) return;
    if (!txMemInitOnce() || !txInfoFill(dev, g_rtO_dev)) { InterlockedExchange(&g_txjOn, 0); logf("texjobs: the device would not answer - textures are decoded on the game thread as before."); return; }
    { static const int need[6] = { 2, 3, 8, 9, 10, 12 }; HMODULE hx = GetModuleHandleA("d3dx9_27.dll");     // a function the game does not import is simply never called through us
      for (int i = 0; i < 6; ++i) if (!g_rtXO[need[i]] && hx) g_rtXO[need[i]] = (void*)GetProcAddress(hx, kRtX[need[i]].name);
      if (hx) g_txjImgInfo = (void*)GetProcAddress(hx, "D3DXGetImageInfoFromFileInMemory"); }
    if (!g_rtXO[2] || !g_rtXO[3] || !g_rtXO[8] || !g_rtXO[10] || !g_rtXO[12]) { InterlockedExchange(&g_txjOn, 0); logf("texjobs: this game does not import the D3DX texture functions - off."); return; }
    g_txjDev = dev;
    logf("texjobs: ready - D3DX texture decoding (whole files, surface loads, mip chains) runs on worker threads and reaches the textures through the render queue.%s",
         g_txjCheckLeft > 0 ? " Self-check: the first results are compared with D3DX working on a real texture." : "");
}
static DWORD WINAPI txjReport(LPVOID) {
    LONG lN[8] = { 0, 0, 0, 0, 0, 0, 0, 0 }, lStep[8] = { 0, 0, 0, 0, 0, 0, 0, 0 }, lWhy[TXJ_WHY], lXc[16], lCreate = 0, lReq = 0, lBud = 0, lFail = 0, lRedo = 0, lChk = 0, lNoop = 0;
    LONG lEarly = 0, lNoDrain = 0, lDrainTo = 0, lBmBlank = 0, lBmKept = 0; LONG64 lEarlyT = 0, lEarlyW = 0;
    LONG64 lWork = 0, lGame = 0, lWait = 0, lCopy = 0, lBudT = 0, lXt[16], lUp = 0, lDrain = 0;
    LONG lFbAdd = 0, lFbLock = 0, lBmNoWait = 0, lBmAdopt = 0, lFbStretch = 0, lBeginDesc = 0;
    static LONG64 lOpT[512];
    memset(lWhy, 0, sizeof(lWhy)); memset(lXc, 0, sizeof(lXc)); memset(lXt, 0, sizeof(lXt));
    bool said = false;
    for (;;) {
        Sleep(5000);
        if (!g_txjOn) continue;
        LONG n[8], st[8]; LONG dn = 0;
        for (int i = 1; i < 5; ++i) { n[i] = g_txjN[i]; st[i] = g_txjNStep[i]; dn += st[i] - lStep[i]; }
        LONG64 work = rd64(&g_txjWorkTicks), game = rd64(&g_txjGameTicks), wait = rd64(&g_txjRtWaitTicks), copy = rd64(&g_txjRtCopyTicks), budT = rd64(&g_txjBudgetTicks), up = rd64(&g_txjBytesUp);
        LONG create = g_txjNCreate, req = g_txjNCheckReq, bud = g_txjNBudget, fail = g_txjNFail, redo = g_txjNClassicRedo, noop = g_txjNoop;
        char still[700]; int q = 0; still[0] = 0; LONG64 stillT = 0;
        for (int k = 0; k < 14; ++k) {
            LONG cN = g_rtXCalls[k], dN = cN - lXc[k]; LONG64 tN = rd64(&g_rtXTicks[k]), dT = tN - lXt[k]; lXc[k] = cN; lXt[k] = tN;
            if (dN <= 0) continue;
            stillT += dT;
            char m1[16]; if (q < 380) q += wsprintfA(still + q, " %s %d (%s ms)", kRtX[k].name + 4, (int)dN, msStr(dT, 1, m1));
        }
        LONG64 drain = rd64(&g_rtDrainTicks), dDrain = drain - lDrain; lDrain = drain;
        char dr[400]; int dq = 0; dr[0] = 0;
        { int top[3] = { -1, -1, -1 }; LONG64 dOp[512];
          for (int i = 0; i < 512; ++i) { LONG64 v = rd64(&g_rtDrainOpTicks[i]); dOp[i] = v - lOpT[i]; lOpT[i] = v; }
          for (int k = 0; k < 3; ++k) { int b = -1; for (int i = 0; i < 512; ++i) { if (i == top[0] || i == top[1]) continue; if (dOp[i] > 0 && (b < 0 || dOp[i] > dOp[b])) b = i; } top[k] = b; }
          for (int k = 0; k < 3; ++k) if (top[k] >= 0 && dOp[top[k]] > g_pqpf.QuadPart / 500) { char m0[16]; dq += wsprintfA(dr + dq, " %s %s ms,", top[k] == 511 ? "D3DX calls and locks" : rtOpName(top[k]), msStr(dOp[top[k]], 1, m0)); } }
        char why[600]; int w = 0; why[0] = 0;
        for (int k = 1; k < TXJ_WHY; ++k) { LONG v = g_txjWhy[k], dv = v - lWhy[k]; lWhy[k] = v; if (dv > 0 && w < 330) w += wsprintfA(why + w, " %d: %s;", (int)dv, kTxjWhy[k]); }
        LONG fbAdd = g_rtNFBAdd, fbLock = g_rtNFBLock, bmNoWait = g_rtNBMNoWait, bmAdopt = g_rtNBMAdopt, fbStretch = g_rtNFBStretch, beginDesc = g_rtNBeginDesc;
        if (dn > 0 || stillT > g_pqpf.QuadPart / 20 || fbLock - lFbLock >= 50 || fbStretch - lFbStretch >= 10 || dDrain > g_pqpf.QuadPart / 10) {
            char m1[16], m2[16], m3[16], m4[16], m5[16], m6[16];
            logf("texjobs: last 5 s - off the game thread: %d whole files, %d surface loads, %d mip chains, %d surfaces from files (%d textures created, %d format checks answered without the device) | D3DX work on %d workers %s ms | "
                 "game thread spent %s ms handing them over%s%s | render thread waited %s ms for results, copied %d KB in %s ms%s%s",
                 (int)(st[1] - lStep[1]), (int)(st[2] - lStep[2]), (int)(st[3] - lStep[3]), (int)(st[4] - lStep[4]), (int)(create - lCreate), (int)(req - lReq), (int)g_txjThreads, msStr(work - lWork, 1, m1),
                 msStr(game - lGame, 1, m2), bud > lBud ? ", waited for memory " : "", bud > lBud ? msStr(budT - lBudT, 1, m5) : "", msStr(wait - lWait, 1, m3), (int)((up - lUp) >> 10), msStr(copy - lCopy, 1, m4),
                 fail > lFail ? " | STEPS FAILED on memory and were redone the old way" : "", redo > lRedo ? " (see above)" : "");
            logf("texjobs: still on the game thread - D3DX:%s | waited %s ms for the render queue:%s%s%s",
                 still[0] ? still : " nothing", msStr(dDrain, 1, m6), dr[0] ? dr : " -", why[0] ? " | left on the old path because -" : "", why);
            logf("buffers: last 5 s - %d vertex/index buffers created, %d locks of new buffers served at once, %d new buffers written through the queue because their frame is drawn twice; "
                 "buffers taken into a mirror %d, of them without waiting for the queue %d | techniques begun for the first time without waiting %d",
                 (int)(fbAdd - lFbAdd), (int)(fbLock - lFbLock), (int)(fbStretch - lFbStretch), (int)(bmAdopt - lBmAdopt), (int)(bmNoWait - lBmNoWait), (int)(beginDesc - lBeginDesc));
            { LONG early = g_txjNEarly, noDrain = g_rtNScopeNoDrain, drainTo = g_rtNDrainTo, bmBlank = g_rtNBMBlank, bmKept = g_rtNBMKept; LONG64 earlyT = rd64(&g_txjEarlyTicks), earlyW = rd64(&g_txjEarlyWaitTicks);
              char e1[16], e2[16];
              logf("waits: last 5 s - %d new textures the game wanted to lock at once got their levels from the game thread itself (%s ms, of that %s ms waiting for their own decoding - not for the queue); "
                   "%d D3DX calls on new objects ran without waiting for the queue; %d waits were for one object's records instead of the whole queue; %d new buffers got their mirror from their first write, %d mirrors kept across a whole-buffer rewrite",
                   (int)(early - lEarly), msStr(earlyT - lEarlyT, 1, e1), msStr(earlyW - lEarlyW, 1, e2), (int)(noDrain - lNoDrain), (int)(drainTo - lDrainTo), (int)(bmBlank - lBmBlank), (int)(bmKept - lBmKept));
              lEarly = early; lNoDrain = noDrain; lDrainTo = drainTo; lBmBlank = bmBlank; lBmKept = bmKept; lEarlyT = earlyT; lEarlyW = earlyW; }
            said = true;
        }
        if (g_txjChkDone > lChk) {
            lChk = g_txjChkDone;
            logf("texjobs: self-check so far - %d results compared with D3DX doing the same work on a real texture: %d identical, %d DIFFERENT%s.", (int)g_txjChkDone, (int)g_txjChkSame, (int)g_txjChkDiff,
                 g_txjCheckLeft > 0 ? "" : " (that was the last of them)");
        }
        for (int i = 1; i < 5; ++i) { lN[i] = n[i]; lStep[i] = st[i]; }
        lFbAdd = fbAdd; lFbLock = fbLock; lBmNoWait = bmNoWait; lBmAdopt = bmAdopt; lFbStretch = fbStretch; lBeginDesc = beginDesc;
        lWork = work; lGame = game; lWait = wait; lCopy = copy; lBudT = budT; lCreate = create; lReq = req; lBud = bud; lFail = fail; lRedo = redo; lUp = up; lNoop = noop;
        (void)said; (void)lNoop;
    }
}
static void txjSetup(int threads, int check, int capMb, const char* how) {
    if (g_txjOn) return;
    if (capMb < 8) capMb = 8;
    if (capMb > 1024) capMb = 1024;
    if (threads == 0) { logf("texjobs: off ([Engine] TextureThreads = 0) - textures are decoded on the game thread."); return; }
    InitializeCriticalSection(&g_txjCs);
    g_txjSem = CreateSemaphoreA(NULL, 0, 0x7FFFFFFF, NULL); g_txjRtEvt = CreateEventA(NULL, FALSE, FALSE, NULL); g_txjGtEvt = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_txjSem || !g_txjRtEvt || !g_txjGtEvt || !txMemInitOnce()) { logf("texjobs: could not set up - off."); return; }
    g_txjWantThreads = threads > 0 ? threads : 0;
    g_txjCap = (LONG64)capMb << 20;
    if (check > 0) g_txjCheckLeft = check;
    g_txjCheckAll = how[0] ? 1 : 0;                        // the offline tests check every result
    InterlockedExchange(&g_txjOn, 1);
    CreateThread(NULL, 0, txjReport, NULL, 0, NULL);
    logf("texjobs: installed%s - the D3DX texture work of loading (mip chains built and compressed for every single-level .dds, terrain atlases, .tga mip chains) leaves the game thread once the render thread is live. "
         "%s worker threads, at most %d MB in flight.", how, threads > 0 ? "configured" : "automatic", capMb);
}
static void installTexJobs() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    bool have = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
    int threads = have ? (int)GetPrivateProfileIntA("Engine", "TextureThreads", -1, ini) : -1;
    char env[32]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_TEXTHREADS", env, sizeof(env)) && env[0]) threads = atoi(env);
    int check = have ? (int)GetPrivateProfileIntA("Engine", "TextureCheck", 0, ini) : 0;
    env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_TEXCHECK", env, sizeof(env)) && env[0]) check = atoi(env);
    int capMb = have ? (int)GetPrivateProfileIntA("Engine", "TextureWorkMB", 96, ini) : 96;
    env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_TEXWORKMB", env, sizeof(env)) && env[0]) capMb = atoi(env);
    txjSetup(threads, check, capMb, "");
}
extern "C" __declspec(dllexport) void __cdecl AotrTexTestSetup(int threads, int check, int capMb) { if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf); txjSetup(threads, check, capMb, " (test)"); }   // before AotrRtTestInstall
extern "C" __declspec(dllexport) void __cdecl AotrTexTestStats(char* out, int n) {
    char m1[16], m2[16], m3[16], m4[16], why[400]; int w = 0; why[0] = 0;
    for (int k = 1; k < TXJ_WHY; ++k) if (g_txjWhy[k] && w < 320) w += wsprintfA(why + w, " %d %s;", (int)g_txjWhy[k], kTxjWhy[k]);
    char buf[2200];
    wsprintfA(buf, "texjobs %s: %d whole files, %d surface loads, %d mip chains, %d surfaces from files on %d workers (%s ms of D3DX work); %d textures created and %d format checks without the device; %d filters with nothing to do; "
                   "game thread %s ms handing over, %d waits for memory (%s ms); render thread waited %s ms; steps failed %d, redone the old way %d; self-check %d identical, %d DIFFERENT; in flight now %d KB, peak memory textures %d KB; old path:%s",
              g_txjOn ? "on" : "OFF", (int)g_txjNStep[1], (int)g_txjNStep[2], (int)g_txjNStep[3], (int)g_txjNStep[4], (int)g_txjThreads, msStr(rd64(&g_txjWorkTicks), 1, m1), (int)g_txjNCreate, (int)g_txjNCheckReq, (int)g_txjNoop,
              msStr(rd64(&g_txjGameTicks), 1, m2), (int)g_txjNBudget, msStr(rd64(&g_txjBudgetTicks), 1, m3), msStr(rd64(&g_txjRtWaitTicks), 1, m4), (int)g_txjNFail, (int)g_txjNClassicRedo,
              (int)g_txjChkSame, (int)g_txjChkDiff, (int)(rd64(&g_txjInflight) >> 10), (int)(g_txMemPeak >> 10), why[0] ? why : " none");
    wsprintfA(buf + lstrlenA(buf), " | buffers: %d created, %d locks of new ones served at once, %d written through the queue because their frame is drawn twice, %d taken into a mirror (%d without waiting) | techniques begun for the first time without waiting: %d",
              (int)g_rtNFBAdd, (int)g_rtNFBLock, (int)g_rtNFBStretch, (int)g_rtNBMAdopt, (int)g_rtNBMNoWait, (int)g_rtNBeginDesc);
    wsprintfA(buf + lstrlenA(buf), " | waits: %d new textures given their levels by the calling thread (%d steps), %d D3DX calls on new objects without waiting for the queue, %d waits for one object's records, %d mirrors from a first write, %d kept across a rewrite",
              (int)g_txjNEarly, (int)g_txjNEarlyStep, (int)g_rtNScopeNoDrain, (int)g_rtNDrainTo, (int)g_rtNBMBlank, (int)g_rtNBMKept);
    lstrcpynA(out, buf, n);
}
