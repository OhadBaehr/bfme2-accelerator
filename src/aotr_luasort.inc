// ================================================================ the script engine's event table (v55, 2026-10-07)
// WHERE THE FIRST SECOND OF A LOAD WENT (long-frame profile of 2026-10-07, slice 0.0-1.1 s of a 9.0 s save load:
// two thirds of the samples in the string lock, under 0x00733086 / 0x007332F6 / 0x00738E85 / 0x00738FE3):
// TheLuaScriptEngine (created at 0x0063C00F, 0xE0 bytes) keeps its event lists in a vector at +0xB0 of 20-byte
// records { AsciiString name; bool; vector<{ int; AsciiString; bool }> events } and a "sorted" flag at +0xAC. Every
// lookup by name (0x007396E8) sorts the whole vector first when the flag is down, and every record added
// (0x007397A3 and after) puts the flag down again - and looks up its parent list first. So a table of N lists is
// sorted N times. The sort is STLport's introsort (0x007396A2, only called from 0x0073972C) on records that are
// COPIED BY VALUE: every swap is three deep copies of a record, every pivot one, the insertion pass one per
// record - each of them a new vector and one locked reference count per event name in it.
//
// The sort's result does not need a single copy. With all names different (the adding code looks an existing name
// up and extends it instead), the sorted order is one fixed arrangement of the same records, whatever algorithm
// produces it. So the call at 0x0073972C goes to a sort that MOVES the 20-byte records as they are:
//   - the records before the first one out of order are left alone (after an add that is all but the last);
//     the ones that were in order when this sort last ran and have not changed since - same bytes, so the same
//     name buffers - are not compared again;
//   - every other record is put in its place by binary search with the engine's own compare (0x004065AA) and one
//     block move;
//   - if any two names compare equal, the order of equal records would depend on the algorithm: the vector is put
//     back exactly as it was found and the engine's own sort runs.
// No record is constructed, copied or destroyed; no reference count changes; the vector ends as the same records
// in the order the engine's sort leaves them. [Engine] ScriptTableSort (default 1) / AOTR_LUASORT=0.
#define LUS_EL 20
typedef int (__fastcall* tLusCmp)(void* a, void* edx, void* b);            // AsciiString::compare(const AsciiString&), thiscall
typedef void (__cdecl* tLusSort)(BYTE* first, BYTE* last, DWORD comp);
static tLusCmp g_lusCmp = (tLusCmp)(ULONG_PTR)0x004065AA;                     // (variables: the offline test puts stand-ins here)
static tLusSort g_lusOrig = (tLusSort)(ULONG_PTR)0x007396A2;
static volatile LONG g_lusOn = 0, g_lusBusy = 0;
static volatile LONG g_lusNSort = 0, g_lusNInOrder = 0, g_lusNTie = 0, g_lusNPlaced = 0, g_lusMaxN = 0, g_lusNKnown = 0, g_lusNOrigBusy = 0, g_lusNCheckBad = 0;
static volatile LONG64 g_lusTicks = 0, g_lusCmpN = 0;
static BYTE* g_lusBak = NULL; static DWORD g_lusBakCap = 0;                 // the vector as it was found (for the way back)
static BYTE* g_lusSeen = NULL; static DWORD g_lusSeenCap = 0, g_lusSeenN = 0;  // the records as this sort last left them, in order
static LONG g_lusCheckLeft = 64;                                           // the first results are checked record by record

static bool lusRoom(BYTE** p, DWORD* cap, DWORD need) {
    if (*cap >= need) return true;
    DWORD want = need + need / 2 + 4096;
    BYTE* q = (BYTE*)VirtualAlloc(NULL, want, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!q) return false;
    if (*p) VirtualFree(*p, 0, MEM_RELEASE);
    *p = q; *cap = want;
    return true;
}
// 0: was in order already, 1: put in order, 2: two equal names (the vector is as it was found). `known`: so many
// leading records are known to be in order. `bak` has room for the whole vector.
static int lusSortCore(BYTE* first, DWORD n, DWORD known, tLusCmp cmp, BYTE* bak, LONG64* nCmp, LONG* nPlaced) {
    DWORD p = known > 1 ? known : 1;
    LONG64 c_ = 0;
    for (; p < n; ++p) { int c = cmp(first + (p - 1) * LUS_EL, NULL, first + p * LUS_EL); ++c_; if (c == 0) { *nCmp += c_; return 2; } if (c > 0) break; }
    if (p >= n) { *nCmp += c_; return 0; }
    memcpy(bak, first, n * LUS_EL);
    for (DWORD i = p; i < n; ++i) {
        BYTE x[LUS_EL]; memcpy(x, first + i * LUS_EL, LUS_EL);
        DWORD lo = 0, hi = i;                                               // the first record that is not below x
        while (lo < hi) {
            DWORD mid = (lo + hi) / 2;
            int c = cmp(first + mid * LUS_EL, NULL, x); ++c_;
            if (c == 0) { memcpy(first, bak, n * LUS_EL); *nCmp += c_; return 2; }
            if (c < 0) lo = mid + 1; else hi = mid;
        }
        if (lo < i) { memmove(first + (lo + 1) * LUS_EL, first + lo * LUS_EL, (i - lo) * LUS_EL); memcpy(first + lo * LUS_EL, x, LUS_EL); ++*nPlaced; }
    }
    *nCmp += c_;
    return 1;
}
static void __cdecl hkLusSort(BYTE* first, BYTE* last, DWORD comp) {
    if (!g_lusOn || !first || last <= first || ((DWORD)(last - first) % LUS_EL) != 0) { g_lusOrig(first, last, comp); return; }
    DWORD n = (DWORD)(last - first) / LUS_EL;
    if (n < 2) return;                                                      // nothing to sort (the engine's sort does nothing either)
    if (InterlockedExchange(&g_lusBusy, 1)) { g_lusNOrigBusy++; g_lusOrig(first, last, comp); return; }
    LONG64 t0 = qpcNow();
    int how = 2; LONG64 nCmp = 0; LONG placed = 0;
    if (lusRoom(&g_lusBak, &g_lusBakCap, n * LUS_EL) && lusRoom(&g_lusSeen, &g_lusSeenCap, n * LUS_EL)) {
        DWORD known = 0, lim = g_lusSeenN < n ? g_lusSeenN : n;               // leading records that are the ones last left in order, byte for byte
        while (known < lim && !memcmp(first + known * LUS_EL, g_lusSeen + known * LUS_EL, LUS_EL)) ++known;
        if (known > 1) g_lusNKnown++;
        how = lusSortCore(first, n, known, g_lusCmp, g_lusBak, &nCmp, &placed);
        if (how != 2 && g_lusCheckLeft > 0) {                                // the first results, pair by pair with the engine's compare
            --g_lusCheckLeft;
            for (DWORD i = 1; i < n; ++i) if (g_lusCmp(first + (i - 1) * LUS_EL, NULL, first + i * LUS_EL) >= 0) { how = 3; break; }
            if (how == 3) { g_lusNCheckBad++; if (placed) memcpy(first, g_lusBak, n * LUS_EL); }
        }
    }
    if (how >= 2) {                                                         // equal names, no memory, or a result that did not hold: the engine's own sort
        g_lusSeenN = 0;
        if (how == 3) { InterlockedExchange(&g_lusOn, 0); logf("luasort: a sorted table failed its check - the engine's own sort is back in place."); }
        else g_lusNTie++;
        g_lusOrig(first, last, comp);
    } else {
        memcpy(g_lusSeen, first, n * LUS_EL); g_lusSeenN = n;
        if (how == 0) g_lusNInOrder++;
    }
    g_lusNSort++; g_lusNPlaced += placed; g_lusCmpN += nCmp; if ((LONG)n > g_lusMaxN) g_lusMaxN = (LONG)n;
    g_lusTicks += qpcNow() - t0;
    InterlockedExchange(&g_lusBusy, 0);
}
static DWORD WINAPI lusReport(LPVOID) {
    LONG lSort = 0;
    for (;;) {
        Sleep(5000);
        LONG s = g_lusNSort;
        if (s - lSort < 20) continue;
        char m[16];
        logf("luasort: %d sorts of the script engine's event table since the last line (%d in all, the table has up to %d lists): %d found it in order, %d records put in place, %d times two lists had the same name and the engine's own sort ran; "
             "%s ms in all of them so far.", (int)(s - lSort), (int)s, (int)g_lusMaxN, (int)g_lusNInOrder, (int)g_lusNPlaced, (int)g_lusNTie, msStr(rd64(&g_lusTicks), 1, m));
        lSort = s;
    }
}
static void installLuaSort() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    int want = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES ? (int)GetPrivateProfileIntA("Engine", "ScriptTableSort", 1, ini) : 1;
    char env[8]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_LUASORT", env, sizeof(env)) && env[0]) want = env[0] - '0';
    if (want <= 0) { logf("luasort: off - the script engine sorts its event table with the engine's own sort."); return; }
    static const BYTE kLookup[11] = { 0x80, 0xBE, 0xAC, 0x00, 0x00, 0x00, 0x00, 0x57, 0x75, 0x26, 0x8B };              // cmp byte [esi+AC],0; push edi; jne +26; mov ecx,[esi+B0]
    static const BYTE kAfter[10] = { 0x83, 0xC4, 0x0C, 0xC6, 0x86, 0xAC, 0x00, 0x00, 0x00, 0x01 };                     // add esp,0C; mov byte [esi+AC],1
    static const BYTE kSort[8] = { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x8B, 0x7C };                                   // push esi; mov esi,[esp+8]; push edi; mov edi,[esp+10]
    static const BYTE kSize[3] = { 0x6A, 0x14, 0x99 };                                                                 // push 14h; cdq  (records of 20 bytes)
    static const BYTE kCmp[6] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x00 };                                                // mov eax,[esp+4]; mov eax,[eax]
    bool ok = false;
    __try {
        ok = !memcmp((void*)0x0073970B, kLookup, sizeof(kLookup)) && rtCallTo(0x0073972C, 0x007396A2) && !memcmp((void*)0x00739731, kAfter, sizeof(kAfter)) &&
             !memcmp((void*)0x007396A2, kSort, sizeof(kSort)) && !memcmp((void*)0x007396B4, kSize, sizeof(kSize)) && rtCallTo(0x007396D2, 0x0073961A) && rtCallTo(0x007396DD, 0x00739507) &&
             !memcmp((void*)0x004065AA, kCmp, sizeof(kCmp)) && rtCallTo(0x004065CC, 0x00406307) && rtCallTo(0x00738F6F, 0x004065AA);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok) { logf("luasort: the script engine's lookup is not what this build has - its table is sorted as the engine has it."); return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    rtSetCall(0x0073972C, (void*)&hkLusSort);
    resumeAll(fz, nf);
    InterlockedExchange(&g_lusOn, 1);
    CreateThread(NULL, 0, lusReport, NULL, 0, NULL);
    logf("luasort: installed - the script engine's event table (sorted again after every list added to it, by a sort that copies each record with all its event names at every step) is put in order by moving the records as they are.");
}

// ---------------------------------------------------------------- offline test
// Stand-ins: a record's first field points at a "string buffer" { refs, len16, pad16, chars }; the compare is by
// those bytes. The reference is a stable sort of the same records by the same compare; with all names different
// the two must agree record for record, and with two equal names the vector must come back untouched.
static int __fastcall lusTestCmp(void* a, void*, void* b) {
    const BYTE* x = *(const BYTE**)a; const BYTE* y = *(const BYTE**)b;
    DWORD lx = x ? *(const WORD*)(x + 4) : 0, ly = y ? *(const WORD*)(y + 4) : 0, m = lx < ly ? lx : ly;
    int c = m ? memcmp(x + 8, y + 8, m) : 0;
    return c ? c : (lx < ly ? -1 : lx > ly ? 1 : 0);
}
static LONG g_lusTestOrigN = 0;
static void __cdecl lusTestOrig(BYTE* first, BYTE* last, DWORD) {          // "the engine's sort": a plain insertion sort (stable), counted
    g_lusTestOrigN++;
    DWORD n = (DWORD)(last - first) / LUS_EL;
    for (DWORD i = 1; i < n; ++i) { BYTE x[LUS_EL]; memcpy(x, first + i * LUS_EL, LUS_EL); DWORD j = i;
        while (j > 0 && lusTestCmp(first + (j - 1) * LUS_EL, NULL, x) > 0) { memcpy(first + j * LUS_EL, first + (j - 1) * LUS_EL, LUS_EL); --j; }
        memcpy(first + j * LUS_EL, x, LUS_EL); }
}
extern "C" __declspec(dllexport) int __cdecl AotrLuaSortTest(char* out, int nOut) {
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    int fail = 0; const DWORD N = 3000;
    tLusCmp keepC = g_lusCmp; tLusSort keepO = g_lusOrig; LONG keepOn = g_lusOn, keepCheck = g_lusCheckLeft;
    g_lusCmp = &lusTestCmp; g_lusOrig = &lusTestOrig; g_lusOn = 1; g_lusCheckLeft = 1 << 30; g_lusSeenN = 0;
    BYTE* names = (BYTE*)VirtualAlloc(NULL, (N + 8) * 40, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BYTE* v = (BYTE*)VirtualAlloc(NULL, (N + 8) * LUS_EL, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BYTE* ref = (BYTE*)VirtualAlloc(NULL, (N + 8) * LUS_EL, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!names || !v || !ref) return 1 << 30;
    DWORD seed = 12345;
    #define LUS_RND() (seed = seed * 1664525u + 1013904223u, seed >> 8)
    for (DWORD i = 0; i < N + 8; ++i) {                                     // name i: unique, of varying length, sharing prefixes
        BYTE* s = names + i * 40; int len = wsprintfA((char*)s + 8, "%s%05u%s", (LUS_RND() & 1) ? "ObjFunctions" : "Obj", (unsigned)((i * 7919u) % 100003u), (i & 3) == 0 ? "X" : "");
        *(DWORD*)s = 1; *(WORD*)(s + 4) = (WORD)len;
    }
    // 1. the engine's pattern: one record added, then a lookup that sorts - N times
    LONG origBefore = g_lusTestOrigN; LONG64 tMine = 0;
    for (DWORD n = 1; n <= N && !fail; ++n) {
        BYTE* e = v + (n - 1) * LUS_EL; memset(e, 0, LUS_EL);
        *(DWORD*)e = (DWORD)(ULONG_PTR)(names + (n - 1) * 40); e[4] = (BYTE)(n & 1); *(DWORD*)(e + 8) = 0x1000 + n; *(DWORD*)(e + 12) = 0x2000 + n; *(DWORD*)(e + 16) = 0x3000 + n;
        memcpy(ref, v, n * LUS_EL); lusTestOrig(ref, ref + n * LUS_EL, 0); g_lusTestOrigN--;
        LONG64 ta = qpcNow();
        hkLusSort(v, v + n * LUS_EL, 0);
        tMine += qpcNow() - ta;
        if (memcmp(v, ref, n * LUS_EL)) fail |= 1;
    }
    if (g_lusTestOrigN != origBefore) fail |= 2;                             // the engine's sort was never needed
    LONG64 cmpAdd = g_lusCmpN;
    // 2. a shuffled vector, sorted in one go (nothing known about it)
    for (DWORD i = N - 1; i > 0; --i) { DWORD j = LUS_RND() % (i + 1); BYTE t[LUS_EL]; memcpy(t, v + i * LUS_EL, LUS_EL); memcpy(v + i * LUS_EL, v + j * LUS_EL, LUS_EL); memcpy(v + j * LUS_EL, t, LUS_EL); }
    memcpy(ref, v, N * LUS_EL); lusTestOrig(ref, ref + N * LUS_EL, 0); g_lusTestOrigN--;
    hkLusSort(v, v + N * LUS_EL, 0);
    if (memcmp(v, ref, N * LUS_EL)) fail |= 4;
    // 3. the vector emptied and filled with other records at the same address: what was known must not be believed
    for (DWORD i = 0; i < 500; ++i) { BYTE* e = v + i * LUS_EL; *(DWORD*)e = (DWORD)(ULONG_PTR)(names + ((i * 37 + 11) % N) * 40); *(DWORD*)(e + 8) = 0x9000 + i; }
    memcpy(ref, v, 500 * LUS_EL); lusTestOrig(ref, ref + 500 * LUS_EL, 0); g_lusTestOrigN--;
    hkLusSort(v, v + 500 * LUS_EL, 0);
    if (memcmp(v, ref, 500 * LUS_EL)) fail |= 8;
    // 4. two records with the same name: back as found, then the engine's sort
    { BYTE* e = v + 500 * LUS_EL; memset(e, 0, LUS_EL); *(DWORD*)e = *(DWORD*)(v + 123 * LUS_EL); *(DWORD*)(e + 8) = 0x7777; }
    BYTE* e2 = v + 501 * LUS_EL; memset(e2, 0, LUS_EL); *(DWORD*)e2 = (DWORD)(ULONG_PTR)(names + (N + 1) * 40);
    memcpy(ref, v, 502 * LUS_EL); lusTestOrig(ref, ref + 502 * LUS_EL, 0); g_lusTestOrigN--;
    LONG ob = g_lusTestOrigN, tieBefore = g_lusNTie;
    hkLusSort(v, v + 502 * LUS_EL, 0);
    if (g_lusTestOrigN != ob + 1 || g_lusNTie != tieBefore + 1 || memcmp(v, ref, 502 * LUS_EL)) fail |= 16;
    // 5. in order already, twice: nothing moves, the second time nothing is compared but the new record
    hkLusSort(v, v + 502 * LUS_EL, 0);                                       // (equal names again: engine's sort, result unchanged)
    if (memcmp(v, ref, 502 * LUS_EL)) fail |= 32;
    // 6. one, zero records, and a length that is not whole records
    hkLusSort(v, v + LUS_EL, 0); hkLusSort(v, v, 0);
    ob = g_lusTestOrigN; hkLusSort(v, v + 30, 0); if (g_lusTestOrigN != ob + 1) fail |= 64;
    if (g_lusNCheckBad) fail |= 128;
    char m1[16];
    if (out && nOut > 300) sprintf(out, "script table sort: %s (flags %04X) - %u records added one at a time with a sort after each: every result the engine's order, %s ms and %d compares in all (the engine's sort copies ~%u records with their event lists for the same); "
                                        "shuffled, refilled, equal names (left to the engine's sort), odd sizes: as they must be",
                                   fail ? "FAILED" : "all cases as they must be", fail, (unsigned)N, msStr(tMine, 1, m1), (int)cmpAdd, (unsigned)(N * N / 2));
    g_lusCmp = keepC; g_lusOrig = keepO; g_lusOn = keepOn; g_lusCheckLeft = keepCheck; g_lusSeenN = 0;
    VirtualFree(names, 0, MEM_RELEASE); VirtualFree(v, 0, MEM_RELEASE); VirtualFree(ref, 0, MEM_RELEASE);
    #undef LUS_RND
    return fail;
}
