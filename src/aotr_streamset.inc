// ================================================================ the asset streamer's "everything these need" sets (v55, 2026-10-07)
// WHERE A QUARTER OF THE "97 %" PART OF A SAVE LOAD WENT (long-frame profile of 2026-10-07, slices 4.4-7.8 s:
// 10-14 % of the samples under the tree eraser 0x00468B12, 7-11 % under the node maker 0x0069C117, both inside
// the allocator and its lock): the asset streamer (the object whose lock is at +0x68) keeps the assets wanted in
// sets of ids (std::set at +0x198 and +0x1AC). Whenever one is added to (0x00A371B0, per object and model created)
// it rebuilds its queues (0x00A36500), and that starts by completing both sets with everything their members
// depend on (0x00A35510):
//     work = COPY of the set;  while work is not empty { cur = COPY of work; empty work;
//         for every id in cur: for every id it depends on that the set lacks: add it to work and to the set }
// A copy of a set is one allocation per member and its destruction one free per member; with a few thousand
// assets wanted that is tens of thousands of locked allocator calls - for every single asset added, to find, almost
// always, nothing new.
//
// The same walk without the copies: the members are read into a plain array once (the set is not changed while it
// is read), each generation is the array's next stretch, and what a generation finds is appended to it - sorted,
// because the engine walks its copy in ascending order. The set gets exactly the same additions in exactly the
// same order through the engine's own insert (0x004D3149), so it ends as the same tree. Nothing else is touched.
// The first calls are checked: the engine's own function runs behind ours and must find nothing left to add.
// The same function runs in play whenever something new is wanted (a unit type seen for the first time, new
// ground under a pan). [Engine] StreamerSets (default 1) / AOTR_STREAMSETS=0.
typedef void (__fastcall* tStcClosure)(void* self, void* edx, DWORD* set);
typedef DWORD* (__cdecl* tStcNext)(DWORD* node);
typedef void* (__fastcall* tStcInsert)(DWORD* set, void* edx, void* outPair, const DWORD* value);
static tStcClosure o_stcClosure = NULL;
static tStcNext g_stcNext = (tStcNext)(ULONG_PTR)0x00423EA0;               // (variables: the offline test puts stand-ins here)
static tStcInsert g_stcInsert = (tStcInsert)(ULONG_PTR)0x004D3149;
static volatile LONG g_stcOn = 0, g_stcBusy = 0;
static volatile LONG g_stcNCall = 0, g_stcNAdded = 0, g_stcNOrig = 0, g_stcMaxSet = 0, g_stcNCheckBad = 0, g_stcNGen = 0;
static volatile LONG64 g_stcTicks = 0, g_stcMembers = 0;
static LONG g_stcCheckLeft = 48;
static DWORD* g_stcBuf = NULL; static DWORD g_stcCap = 0;

static bool stcRoom(DWORD need) {
    if (g_stcCap >= need) return true;
    DWORD want = need * 2 + 4096;
    DWORD* q = (DWORD*)VirtualAlloc(NULL, want * sizeof(DWORD), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!q) return false;
    if (g_stcBuf) { memcpy(q, g_stcBuf, g_stcCap * sizeof(DWORD)); VirtualFree(g_stcBuf, 0, MEM_RELEASE); }
    g_stcBuf = q; g_stcCap = want;
    return true;
}
static __forceinline bool stcHas(const DWORD* set, DWORD id) {             // the engine's own find (0x00A35650): lower bound, then equal?
    const DWORD* hdr = (const DWORD*)(ULONG_PTR)set[0];
    const DWORD* x = (const DWORD*)(ULONG_PTR)hdr[1]; const DWORD* y = hdr;
    while (x) { if (x[4] < id) x = (const DWORD*)(ULONG_PTR)x[3]; else { y = x; x = (const DWORD*)(ULONG_PTR)x[2]; } }
    return y != hdr && !(id < y[4]);
}
static void stcSortU32(DWORD* a, DWORD n) {                                // ascending; a generation's finds are few
    if (n < 2) return;
    if (n <= 24) { for (DWORD i = 1; i < n; ++i) { DWORD x = a[i], j = i; while (j > 0 && a[j - 1] > x) { a[j] = a[j - 1]; --j; } a[j] = x; } return; }
    for (DWORD gap = n / 2; gap > 0; gap = gap == 2 ? 1 : (DWORD)(gap / 2.2)) {
        for (DWORD i = gap; i < n; ++i) { DWORD x = a[i], j = i; while (j >= gap && a[j - gap] > x) { a[j] = a[j - gap]; j -= gap; } a[j] = x; }
    }
}
// The walk. false: nothing was done (no room, or a table the engine itself would trip over) - the caller runs the
// engine's function.
static bool stcClosure(BYTE* self, DWORD* set, LONG* added, LONG* gens) {
    DWORD count = set[1];
    if (!count) return true;                                                // the engine copies an empty set and leaves
    DWORD* hdr = (DWORD*)(ULONG_PTR)set[0];
    if (!hdr) return false;
    { DWORD* b0 = *(DWORD**)(self + 0x50); DWORD* b1 = *(DWORD**)(self + 0x54); if (!b0 || b1 <= b0) return false; }
    if (!stcRoom(count + 1024)) return false;
    DWORD n = 0;
    for (DWORD* node = (DWORD*)(ULONG_PTR)hdr[2]; node != hdr && n < count; node = g_stcNext(node)) g_stcBuf[n++] = node[4];
    if (n != count) return false;                                           // a set whose count and tree disagree: not ours to walk
    DWORD g0 = 0, g1 = n;
    while (g0 < g1) {
        DWORD end = g1;
        for (DWORD i = g0; i < g1; ++i) {
            DWORD id = g_stcBuf[i];
            if (!id) continue;
            DWORD* b0 = *(DWORD**)(self + 0x50); DWORD* b1 = *(DWORD**)(self + 0x54);
            DWORD* e = (DWORD*)(ULONG_PTR)b0[id % (DWORD)(b1 - b0)];
            while (e && e[1] != id) e = (DWORD*)(ULONG_PTR)e[0];
            if (!e) continue;
            DWORD* rec = (DWORD*)(ULONG_PTR)e[2];
            DWORD* deps = (DWORD*)(ULONG_PTR)rec[3];
            if (!deps || !deps[0]) continue;
            for (DWORD k = 0;; ++k) {
                DWORD d = deps[k];
                if (!stcHas(set, d)) {
                    if (end >= g_stcCap && !stcRoom(end + 1)) return false;   // (cannot happen short of no memory; the set is still a part of what the engine would make, and its own function finishes it)
                    g_stcBuf[end++] = d;
                    BYTE pair[8]; DWORD v = d;
                    g_stcInsert(set, NULL, pair, &v);
                    ++*added;
                }
                deps = (DWORD*)(ULONG_PTR)rec[3];                           // the engine reads the list's address again for every entry
                if (!deps[k + 1]) break;
            }
        }
        stcSortU32(g_stcBuf + g1, end - g1);                                // the next generation, in the order the engine's copy has it
        g0 = g1; g1 = end; ++*gens;
    }
    return true;
}
static void __fastcall hkStcClosure(void* self, void* edx, DWORD* set) {
    if (!g_stcOn || !set || InterlockedExchange(&g_stcBusy, 1)) { g_stcNOrig++; o_stcClosure(self, edx, set); return; }
    LONG64 t0 = qpcNow();
    LONG added = 0, gens = 0; bool ok = false;
    DWORD before = set[1];
    __try { ok = stcClosure((BYTE*)self, set, &added, &gens); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok) { g_stcNOrig++; o_stcClosure(self, edx, set); }
    else if (g_stcCheckLeft > 0) {                                          // the engine's own walk behind ours: it must find the set complete
        --g_stcCheckLeft;
        DWORD mine = set[1];
        o_stcClosure(self, edx, set);
        if (set[1] != mine) { g_stcNCheckBad++; InterlockedExchange(&g_stcOn, 0);
            logf("streamsets: CHECK FAILED - the engine's own walk added %u assets to a set of %u that ours had called complete; the engine's function is back in place.", (unsigned)(set[1] - mine), (unsigned)mine); }
    }
    g_stcNCall++; g_stcNAdded += added; g_stcNGen += gens; g_stcMembers += before;
    if ((LONG)set[1] > g_stcMaxSet) g_stcMaxSet = (LONG)set[1];
    g_stcTicks += qpcNow() - t0;
    InterlockedExchange(&g_stcBusy, 0);
}
static DWORD WINAPI stcReport(LPVOID) {
    LONG lCall = 0; LONG64 lTicks = 0, lMem = 0;
    for (;;) {
        Sleep(5000);
        LONG c = g_stcNCall; LONG64 t = rd64(&g_stcTicks), mem = rd64(&g_stcMembers);
        if (c - lCall < 40) continue;
        char m[16];
        logf("streamsets: last 5 s - the streamer completed its wanted-asset sets %d times (%d members a time on average, the largest set so far %d) in %s ms; the engine's way copies and frees each set twice for that, one allocation a member. "
             "So far %d assets added by it, %d calls left to the engine's function%s.",
             (int)(c - lCall), (int)((mem - lMem) / (c - lCall)), (int)g_stcMaxSet, msStr(t - lTicks, 1, m), (int)g_stcNAdded, (int)g_stcNOrig, g_stcCheckLeft > 0 ? "" : "; the first 48 results were checked against the engine's own walk");
        lCall = c; lTicks = t; lMem = mem;
    }
}
static void installStreamSets() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    int want = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES ? (int)GetPrivateProfileIntA("Engine", "StreamerSets", 1, ini) : 1;
    char env[8]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_STREAMSETS", env, sizeof(env)) && env[0]) want = env[0] - '0';
    if (want <= 0) { logf("streamsets: off - the asset streamer completes its sets the engine's way."); return; }
    static const BYTE kHead[13] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x6A, 0xFF, 0x68, 0x10, 0xC9, 0xBB, 0x00 };    // push ebp; mov ebp,esp; and esp,-8; push -1; push handler
    static const BYTE kId[5] = { 0x8B, 0x47, 0x10, 0x3B, 0xC3 };                                                       // mov eax,[edi+10]; cmp eax,ebx  (a member; zero is skipped)
    static const BYTE kHash[16] = { 0x8B, 0x51, 0x50, 0x8B, 0x59, 0x54, 0x83, 0xC1, 0x4C, 0x2B, 0xDA, 0x8B, 0xF0, 0xC1, 0xFB, 0x02 };   // buckets at this+50 / +54
    static const BYTE kChain[9] = { 0x39, 0x70, 0x04, 0x74, 0x06, 0x8B, 0x00, 0x85, 0xC0 };                            // cmp [eax+4],esi; je; mov eax,[eax]; test eax,eax
    static const BYTE kDeps[13] = { 0x8B, 0x58, 0x08, 0x8B, 0x43, 0x0C, 0x85, 0xC0, 0x74, 0x78, 0x83, 0x38, 0x00 };     // mov ebx,[eax+8]; mov eax,[ebx+C]; test; je; cmp dword [eax],0
    static const BYTE kFind[11] = { 0x39, 0x70, 0x10, 0x72, 0x07, 0x8B, 0xD0, 0x8B, 0x40, 0x08, 0xEB };                 // cmp [eax+10],esi; jb; mov edx,eax; mov eax,[eax+8]; jmp
    static const BYTE kNextDep[9] = { 0x8B, 0x43, 0x0C, 0x8B, 0x4C, 0x07, 0x04, 0x83, 0xC7 };                           // mov eax,[ebx+C]; mov ecx,[edi+eax+4]; add edi,4
    static const BYTE kRet[3] = { 0xC2, 0x04, 0x00 };
    static const BYTE kInsert[8] = { 0x55, 0x8B, 0xEC, 0x53, 0x8B, 0xD9, 0x8B, 0x0B };                                  // push ebp; mov ebp,esp; push ebx; mov ebx,ecx; mov ecx,[ebx]
    static const BYTE kNext[8] = { 0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x45, 0x08, 0x83 };
    bool ok = false; int bad = 0;
    __try {
        ok = true;
        if (memcmp((void*)0x00A35510, kHead, sizeof(kHead))) { ok = false; bad = 1; }
        else if (!rtCallTo(0x00A3553D, 0x00A34A80) || !rtCallTo(0x00A35569, 0x00A34A80)) { ok = false; bad = 2; }
        else if (memcmp((void*)0x00A355F0, kId, sizeof(kId)) || memcmp((void*)0x00A355FF, kHash, sizeof(kHash)) || memcmp((void*)0x00A35620, kChain, sizeof(kChain))) { ok = false; bad = 3; }
        else if (memcmp((void*)0x00A35637, kDeps, sizeof(kDeps)) || memcmp((void*)0x00A35660, kFind, sizeof(kFind)) || memcmp((void*)0x00A356A7, kNextDep, sizeof(kNextDep))) { ok = false; bad = 4; }
        else if (!rtCallTo(0x00A35690, 0x004D3149) || !rtCallTo(0x00A356A2, 0x004D3149) || !rtCallTo(0x00A356BA, 0x00423EA0) || memcmp((void*)0x00A3570E, kRet, sizeof(kRet))) { ok = false; bad = 5; }
        else if (memcmp((void*)0x004D3149, kInsert, sizeof(kInsert)) || memcmp((void*)0x00423EA0, kNext, sizeof(kNext))) { ok = false; bad = 6; }
        else if (!rtCallTo(0x00A36551, 0x00A35510) || !rtCallTo(0x00A3655F, 0x00A35510)) { ok = false; bad = 7; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; bad = 9; }
    if (!ok) { logf("streamsets: the asset streamer's code is not what this build has (check %d) - nothing changed.", bad); return; }
    o_stcClosure = (tStcClosure)makeTrampoline((BYTE*)0x00A35510, 6);
    if (!o_stcClosure || !stcRoom(16384)) { logf("streamsets: no memory - nothing changed."); return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)0x00A35510, (void*)&hkStcClosure, 6);
    resumeAll(fz, nf);
    InterlockedExchange(&g_stcOn, 1);
    CreateThread(NULL, 0, stcReport, NULL, 0, NULL);
    logf("streamsets: installed - the asset streamer completes its wanted-asset sets without copying them (the engine copies each set twice, an allocation and a free per member, every time one asset is added: a quarter of the time a save's objects take to restore).");
}

// ---------------------------------------------------------------- offline test
// Stand-ins for the engine's pieces: a set is a plain (unbalanced) search tree with the engine's node layout
// { colour, parent, left, right, id } and header { -, root, leftmost, rightmost }; the reference is the engine's
// algorithm written out with its copies. Both must make the same inserts in the same order and end with the same
// members.
struct StcTSet { DWORD hdr, count; };
static DWORD g_stcTLog[2][65536]; static DWORD g_stcTLogN[2]; static int g_stcTWhich = 0;
static DWORD* stcTNode(DWORD id) { DWORD* n = (DWORD*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 20); n[4] = id; return n; }
static void stcTInit(StcTSet* s) { DWORD* h = stcTNode(0); h[1] = 0; h[2] = (DWORD)(ULONG_PTR)h; h[3] = (DWORD)(ULONG_PTR)h; s->hdr = (DWORD)(ULONG_PTR)h; s->count = 0; }
static bool stcTInsertRaw(StcTSet* s, DWORD id) {
    DWORD* h = (DWORD*)(ULONG_PTR)s->hdr; DWORD* x = (DWORD*)(ULONG_PTR)h[1]; DWORD* y = h; bool left = true;
    while (x) { y = x; if (id == x[4]) return false; left = id < x[4]; x = (DWORD*)(ULONG_PTR)(left ? x[2] : x[3]); }
    DWORD* n = stcTNode(id); n[1] = (DWORD)(ULONG_PTR)y;
    if (y == h) { h[1] = (DWORD)(ULONG_PTR)n; h[2] = (DWORD)(ULONG_PTR)n; h[3] = (DWORD)(ULONG_PTR)n; }
    else { if (left) y[2] = (DWORD)(ULONG_PTR)n; else y[3] = (DWORD)(ULONG_PTR)n;
           if (id < ((DWORD*)(ULONG_PTR)h[2])[4]) h[2] = (DWORD)(ULONG_PTR)n;
           if (id > ((DWORD*)(ULONG_PTR)h[3])[4]) h[3] = (DWORD)(ULONG_PTR)n; }
    s->count++;
    return true;
}
static void* __fastcall stcTInsert(DWORD* set, void*, void* outPair, const DWORD* value) {
    if (g_stcTLogN[g_stcTWhich] < 65536) g_stcTLog[g_stcTWhich][g_stcTLogN[g_stcTWhich]++] = *value;
    stcTInsertRaw((StcTSet*)set, *value);
    return outPair;
}
static DWORD* __cdecl stcTNext(DWORD* n) {                                 // in-order successor; the header closes the ring
    if (n[3]) { n = (DWORD*)(ULONG_PTR)n[3]; while (n[2]) n = (DWORD*)(ULONG_PTR)n[2]; return n; }
    DWORD* y = (DWORD*)(ULONG_PTR)n[1];
    while (n == (DWORD*)(ULONG_PTR)y[3]) { n = y; y = (DWORD*)(ULONG_PTR)y[1]; }
    if ((DWORD*)(ULONG_PTR)n[3] != y) n = y;                                // (the header's "parent" is the root and its "right" the last member: climbing out of the tree ends at the header)
    return n;
}
static DWORD stcTList(const StcTSet* s, DWORD* out, DWORD cap) {
    DWORD* h = (DWORD*)(ULONG_PTR)s->hdr; DWORD n = 0;
    if (!s->count) return 0;
    for (DWORD* x = (DWORD*)(ULONG_PTR)h[2]; x != h && n < cap; x = stcTNext(x)) out[n++] = x[4];
    return n;
}
// the engine's function as it is written, copies and all, on the stand-ins
static void stcTReference(BYTE* self, StcTSet* S) {
    static DWORD work[65536], cur[65536]; DWORD nWork = stcTList(S, work, 65536);
    while (nWork) {
        DWORD nCur = nWork; memcpy(cur, work, nCur * sizeof(DWORD)); nWork = 0;
        StcTSet w; stcTInit(&w);                                            // "work" is a set: its members come out in ascending order, each once
        for (DWORD i = 0; i < nCur; ++i) {
            DWORD id = cur[i]; if (!id) continue;
            DWORD* b0 = *(DWORD**)(self + 0x50); DWORD* b1 = *(DWORD**)(self + 0x54);
            DWORD* e = (DWORD*)(ULONG_PTR)b0[id % (DWORD)(b1 - b0)];
            while (e && e[1] != id) e = (DWORD*)(ULONG_PTR)e[0];
            if (!e) continue;
            DWORD* rec = (DWORD*)(ULONG_PTR)e[2]; DWORD* deps = (DWORD*)(ULONG_PTR)rec[3];
            if (!deps || !deps[0]) continue;
            for (DWORD k = 0;; ++k) {
                DWORD d = deps[k];
                if (!stcHas((const DWORD*)S, d)) { stcTInsertRaw(&w, d); BYTE pair[8]; DWORD v = d; stcTInsert((DWORD*)S, NULL, pair, &v); }
                if (!deps[k + 1]) break;
            }
        }
        nWork = stcTList(&w, work, 65536);
    }
}
extern "C" __declspec(dllexport) int __cdecl AotrStreamSetTest(char* out, int nOut) {
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    int fail = 0;
    tStcNext keepN = g_stcNext; tStcInsert keepI = g_stcInsert;
    g_stcNext = &stcTNext; g_stcInsert = &stcTInsert;
    const DWORD NA = 6000, NB = 1021;                                       // assets, hash buckets
    static BYTE self[0x60];
    DWORD* buckets = (DWORD*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, NB * 4);
    *(DWORD**)(self + 0x50) = buckets; *(DWORD**)(self + 0x54) = buckets + NB;
    DWORD seed = 777;
    #define STC_RND() (seed = seed * 1664525u + 1013904223u, seed >> 8)
    DWORD* ids = (DWORD*)HeapAlloc(GetProcessHeap(), 0, NA * 4);
    for (DWORD i = 0; i < NA; ++i) ids[i] = 0x10000000u + i * 2654435761u % 0x0FFFFFFFu + i;           // scattered, unique enough
    for (DWORD i = 0; i < NA; ++i) {                                         // record: [3] = dependency list (0-terminated), or none
        DWORD* rec = (DWORD*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 16);
        DWORD kind = STC_RND() % 10;
        if (kind >= 3) { DWORD nd = kind == 9 ? 12 : 1 + STC_RND() % 4; DWORD* d = (DWORD*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (nd + 1) * 4);
            for (DWORD k = 0; k < nd; ++k) d[k] = ids[STC_RND() % NA];
            if (kind == 4) d[0] = 0;                                         // a list that starts with the end mark: nothing
            if (kind == 5) d[nd - 1] = 0x7F000000u + i;                      // an id the table does not know
            rec[3] = (DWORD)(ULONG_PTR)d; }
        if ((i % 97) == 5) continue;                                         // a member the table does not know
        DWORD* e = (DWORD*)HeapAlloc(GetProcessHeap(), 0, 12); DWORD b = ids[i] % NB;
        e[0] = buckets[b]; e[1] = ids[i]; e[2] = (DWORD)(ULONG_PTR)rec; buckets[b] = (DWORD)(ULONG_PTR)e;
    }
    int rounds = 0; LONG64 tMine = 0; LONG totalAdded = 0;
    static DWORD la[65536], lb[65536];
    for (int round = 0; round < 60 && !fail; ++round) {
        StcTSet A, B; stcTInit(&A); stcTInit(&B);
        DWORD start = round == 0 ? 0 : round == 1 ? 1 : 1 + STC_RND() % (round < 30 ? 40 : 1500);
        for (DWORD k = 0; k < start; ++k) { DWORD id = (round == 2 && k == 0) ? 0 : ids[STC_RND() % NA]; stcTInsertRaw(&A, id); stcTInsertRaw(&B, id); }
        // several calls with a few members added in between, as the streamer does
        for (int step = 0; step < 6 && !fail; ++step) {
            g_stcTLogN[0] = g_stcTLogN[1] = 0;
            g_stcTWhich = 0; LONG added = 0, gens = 0; LONG64 t0 = qpcNow();
            bool ok = stcClosure(self, (DWORD*)&A, &added, &gens);
            tMine += qpcNow() - t0; totalAdded += added;
            g_stcTWhich = 1; stcTReference(self, &B);
            if (!ok && A.count) fail |= 1;
            if (g_stcTLogN[0] != g_stcTLogN[1] || memcmp(g_stcTLog[0], g_stcTLog[1], g_stcTLogN[0] * 4)) fail |= 2;      // the same inserts, in the same order
            DWORD na = stcTList(&A, la, 65536), nb = stcTList(&B, lb, 65536);
            if (na != nb || na != A.count || memcmp(la, lb, na * 4)) fail |= 4;
            g_stcTLogN[0] = 0; g_stcTWhich = 0; added = 0;                    // complete: a second walk adds nothing
            stcClosure(self, (DWORD*)&A, &added, &gens); if (added || g_stcTLogN[0]) fail |= 8;
            for (int k = 0; k < 3; ++k) { DWORD id = ids[STC_RND() % NA]; stcTInsertRaw(&A, id); stcTInsertRaw(&B, id); }
            ++rounds;
        }
    }
    char m1[16];
    if (out && nOut > 300) sprintf(out, "streamer sets: %s (flags %04X) - %d walks over sets of up to %u assets against the engine's algorithm written out with its copies: the same inserts in the same order and the same members every time (%d assets added in all, ours %s ms)",
                                   fail ? "FAILED" : "all cases as they must be", fail, rounds, (unsigned)NA, (int)totalAdded, msStr(tMine, 1, m1));
    g_stcNext = keepN; g_stcInsert = keepI;
    #undef STC_RND
    return fail;
}
