// rlsort_test: maps game.dat's image at its base address (no imports, nothing started) and runs the stock render list
// sort 0x5740D0 (and its budgeted core 0x573F23 + 0x5738FD, to reach the heap sort fallback) against the replacement
// in aotr_rlsort_algo.h: identical bytes, identical comparator call sequence, reference counts back where they started.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "aotr_rlsort_algo.h"

typedef void (__cdecl* tStockSort)(RlEl*, RlEl*, tRlPred);
typedef void (__cdecl* tStockCore)(RlEl*, RlEl*, int, int, tRlPred);
typedef void (__cdecl* tStockFinal)(RlEl*, RlEl*, tRlPred);

// The test process's own heaps and mapped files already sit at 0x400000 when main runs, so the parent creates a suspended
// child (this exe, image based at 0x70000000), writes game.dat's headers and sections at the image base before the child's
// loader creates anything there, and lets the child run the tests.
static int runChildWithImage(const char* path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("cannot open %s\n", path); return 2; }
    DWORD size = GetFileSize(f, NULL), got = 0;
    BYTE* d = (BYTE*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ReadFile(f, d, size, &got, NULL); CloseHandle(f);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(d + ((IMAGE_DOS_HEADER*)d)->e_lfanew);
    DWORD base = nt->OptionalHeader.ImageBase, img = nt->OptionalHeader.SizeOfImage;
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    char cmd[MAX_PATH + 16]; sprintf_s(cmd, "\"%s\" child", exe);
    STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) { printf("CreateProcess failed %u\n", GetLastError()); return 2; }
    void* m = VirtualAllocEx(pi.hProcess, (void*)(ULONG_PTR)base, img, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (m != (void*)(ULONG_PTR)base) { printf("cannot map the image at %08X (size %08X) in the child: error %u\n", base, img, GetLastError()); TerminateProcess(pi.hProcess, 2); return 2; }
    SIZE_T wr = 0;
    WriteProcessMemory(pi.hProcess, m, d, nt->OptionalHeader.SizeOfHeaders, &wr);
    IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        DWORD n = sh[i].SizeOfRawData; if (sh[i].Misc.VirtualSize && sh[i].Misc.VirtualSize < n) n = sh[i].Misc.VirtualSize;
        if (n) WriteProcessMemory(pi.hProcess, (BYTE*)m + sh[i].VirtualAddress, d + sh[i].PointerToRawData, n, &wr);
    }
    printf("mapped game.dat at %08X in the test child (%u sections)\n", base, nt->FileHeader.NumberOfSections);
    fflush(stdout);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2; GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}

struct FakeObj { DWORD vt; LONG refs; BYTE pad[0xC4 - 8]; DWORD model; BYTE pad2[0x310 - 0xC8]; DWORD inst; };
static LONG g_released = 0;
static void __fastcall fakeDelete(void*, void*) { g_released++; }
static DWORD g_fakeVt[4];
static unsigned __int64 g_rng = 0x9E3779B97F4A7C15ull;
static DWORD rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (DWORD)(g_rng >> 16); }

// comparator that records the call sequence by element id (w[1]); ids survive copies into temporaries
static DWORD* g_seq = NULL; static size_t g_seqN = 0, g_seqCap = 0; static bool g_seqOverflow = false;
static bool __cdecl recLess(const RlEl* a, const RlEl* b) {
    if (g_seqN + 2 <= g_seqCap) { g_seq[g_seqN++] = a->w[1]; g_seq[g_seqN++] = b->w[1]; } else g_seqOverflow = true;
    DWORD ma = *(DWORD*)(ULONG_PTR)(a->w[0] + 0xC4), mb = *(DWORD*)(ULONG_PTR)(b->w[0] + 0xC4);
    return ma < mb;
}

const int kObj = 700;
static FakeObj* objs;
static void fill(RlEl* a, int n, int pattern) {
    DWORD models = pattern == 0 ? 1 : pattern == 1 ? 2 : pattern == 2 ? 5 : pattern == 3 ? 40 : pattern == 4 ? 400 : 64;
    for (int i = 0; i < kObj; ++i) { objs[i].model = 0x1000 + (rnd() % models) * 0x40; objs[i].inst = rnd() % (pattern == 5 ? 2 : 50); }
    for (int i = 0; i < n; ++i) {
        int k;
        switch (pattern) {
        case 6: k = (int)((__int64)i * kObj / (n ? n : 1)); break;                      // ascending
        case 7: k = kObj - 1 - (int)((__int64)i * kObj / (n ? n : 1)); break;           // descending
        case 8: k = (i & 1) ? (int)(rnd() % kObj) : (int)((__int64)i * kObj / (n ? n : 1)); break;   // half sorted
        case 9: k = (int)(rnd() % 8) + ((i % 64) < 32 ? 0 : kObj - 8); break;           // blocks
        default: k = (int)(rnd() % kObj); break;
        }
        if (pattern >= 6 && pattern <= 8) objs[k].model = 0x1000 + (DWORD)k * 0x10;
        a[i].w[0] = (DWORD)(ULONG_PTR)&objs[k];
        a[i].w[1] = (DWORD)i;
        for (int j = 2; j < 8; ++j) a[i].w[j] = (rnd() & 1) ? (DWORD)(ULONG_PTR)&objs[rnd() % kObj] : 0;
    }
}
static int g_fail = 0;
static bool refsOk() { for (int i = 0; i < kObj; ++i) if (objs[i].refs != 1000000) return false; return g_released == 0; }

static int* s_logA; static int s_nA; static DWORD* s_rA; static DWORD* s_rPA;

// ---- push / erase: fake models, meshes and lights; Delete_This records the release order
struct FakeModel { BYTE pad[0x19]; BYTE flags; BYTE pad2[0xBC - 0x1A]; DWORD mat; };
static int g_delLog[200000]; static int g_delN = 0;
static FakeObj* g_pool = NULL; static const int kPool = 300;
static void __fastcall poolDelete(void* self, void*) { if (g_delN < 200000) g_delLog[g_delN++] = (int)(((FakeObj*)self) - g_pool); }
static DWORD g_poolVt[4];
typedef void (__fastcall* tStockPush)(DWORD*, void*, DWORD, DWORD*);
typedef DWORD (__fastcall* tStockErase)(DWORD*, void*, DWORD, DWORD);
static int testPushErase() {
    tStockPush stockPush = (tStockPush)0x0057430D;
    tStockErase stockErase = (tStockErase)0x00573816;
    g_pool = (FakeObj*)VirtualAlloc(NULL, kPool * sizeof(FakeObj), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    FakeModel* models = (FakeModel*)VirtualAlloc(NULL, 16 * sizeof(FakeModel), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    BYTE matYes = 1, matNo = 0;
    g_poolVt[0] = (DWORD)(ULONG_PTR)&poolDelete;
    for (int m = 0; m < 16; ++m) { models[m].flags = (BYTE)((m & 1) ? 4 : 0) | (BYTE)((m & 4) ? 0x08 : 0); models[m].mat = (DWORD)(ULONG_PTR)((m & 2) ? &matYes : &matNo); }
    const int kCap = 4000;
    static RlEl stA[3][4000], stB[3][4000];
    static LONG refs0[300], refsA[300];
    int fails = 0, rounds = 0; __int64 pushes = 0, deletes = 0;
    for (int round = 0; round < 400; ++round) {
        // initial reference counts: some objects start at 1-3 so the clear releases them to zero
        for (int i = 0; i < kPool; ++i) {
            g_pool[i].vt = (DWORD)(ULONG_PTR)g_poolVt; g_pool[i].model = (DWORD)(ULONG_PTR)&models[rnd() % 16];
            refs0[i] = (rnd() % 4 == 0) ? 1 + (LONG)(rnd() % 3) : 100000 + (LONG)(rnd() % 1000);
        }
        int np = (int)(rnd() % 1200) + (round % 7 == 0 ? 2500 : 0);
        DWORD* ops = new DWORD[np * 8];
        for (int k = 0; k < np; ++k) {
            ops[k * 8] = rnd() % kPool;
            DWORD nl = rnd() % 9; if (rnd() % 3) nl = rnd() % 3;         // mostly 0-2 lights, sometimes 7-8 (stock early-out)
            ops[k * 8 + 1] = nl;
            for (int j = 0; j < 6; ++j) ops[k * 8 + 2 + j] = (rnd() % 5 == 0) ? 0xFFFFFFFF : rnd() % kPool;   // 0xFFFFFFFF = null light
        }
        static DWORD lightArr[8];
        int smallList = round % 11 == 3 ? (int)(rnd() % 3) : -1, smallCap = (int)(rnd() % 40);
        for (int pass = 0; pass < 2; ++pass) {                       // pass 0 stock, pass 1 replacement
            for (int i = 0; i < kPool; ++i) g_pool[i].refs = refs0[i];
            RlEl (*st)[4000] = pass ? stB : stA;
            DWORD r[9];
            int capv[3] = { kCap, kCap, kCap };
            if (smallList >= 0) capv[smallList] = smallCap;             // a small list: pushes past capacity are skipped in both runs
            for (int l = 0; l < 3; ++l) { r[l * 3] = r[l * 3 + 1] = (DWORD)(ULONG_PTR)st[l]; r[l * 3 + 2] = (DWORD)(ULONG_PTR)(st[l] + capv[l]); }
            memset(st, 0xEE, sizeof(stA));
            g_delN = 0;
            for (int k = 0; k < np; ++k) {
                DWORD mesh = (DWORD)(ULONG_PTR)&g_pool[ops[k * 8]];
                DWORD nl = ops[k * 8 + 1];
                DWORD cnt = nl > 6 ? 6 : nl;
                for (DWORD j = 0; j < 8; ++j) { DWORD li = ops[k * 8 + 2 + (j < 6 ? j : 5)]; lightArr[j] = li == 0xFFFFFFFF ? 0 : (DWORD)(ULONG_PTR)&g_pool[li]; }
                DWORD lv[3] = { (DWORD)(ULONG_PTR)lightArr, (DWORD)(ULONG_PTR)(lightArr + nl), (DWORD)(ULONG_PTR)(lightArr + 8) };
                (void)cnt;
                ++g_pool[ops[k * 8]].refs;                              // the caller's by-value copy
                // a list at capacity would make the stock code reallocate through the game allocator: skip that push in both runs
                DWORD model = g_pool[ops[k * 8]].model;
                DWORD* v = r;
                if (((FakeModel*)(ULONG_PTR)model)->flags & 4) v = *(BYTE*)(ULONG_PTR)((FakeModel*)(ULONG_PTR)model)->mat ? r + 3 : r + 6;
                if (nl <= 6 && v[2] == v[1]) { --g_pool[ops[k * 8]].refs; continue; }
                if (pass == 0) stockPush(r, NULL, mesh, lv);
                else if (!rlPushFast(r, mesh, lv)) stockPush(r, NULL, mesh, lv);
                ++pushes;
            }
            DWORD rAfterPush[9]; memcpy(rAfterPush, r, sizeof(r));
            // objects that started with 1-3 references lose those outside references now, so the lists hold the last ones
            for (int i = 0; i < kPool; ++i) if (refs0[i] < 100) g_pool[i].refs -= refs0[i];
            // clear the three lists the way the flush does (erase(begin, end)), plus a middle erase that stays stock
            for (int l = 0; l < 3; ++l) {
                DWORD* v = r + l * 3;
                if (pass == 0) stockErase(v, NULL, v[0], v[1]);
                else if (!rlEraseTailFast(v, v[0], v[1])) stockErase(v, NULL, v[0], v[1]);
            }
            if (pass == 0) {
                memcpy(refsA, refs0, 0); for (int i = 0; i < kPool; ++i) refsA[i] = g_pool[i].refs;
                static int logA[200000]; static int nA; memcpy(logA, g_delLog, g_delN * sizeof(int)); nA = g_delN;
                static DWORD rA[9]; memcpy(rA, r, sizeof(r));
                static DWORD rPA[9]; memcpy(rPA, rAfterPush, sizeof(r));
                // keep for the comparison below
                s_logA = logA; s_nA = nA; s_rA = rA; s_rPA = rPA;
            } else {
                bool bad = false;
                for (int i = 0; i < kPool; ++i) if (g_pool[i].refs != refsA[i]) bad = true;
                if (g_delN != s_nA || memcmp(g_delLog, s_logA, g_delN * sizeof(int))) bad = true;
                for (int l = 0; l < 3; ++l) {
                    if (r[l * 3 + 1] - r[l * 3] != s_rA[l * 3 + 1] - s_rA[l * 3]) bad = true;
                    DWORD usedA = (s_rPA[l * 3 + 1] - s_rPA[l * 3]) , usedB = (rAfterPush[l * 3 + 1] - rAfterPush[l * 3]);
                    if (usedA != usedB || memcmp(stA[l], stB[l], usedA)) bad = true;
                }
                deletes += g_delN;
                if (bad && fails++ < 5) printf("PUSH/ERASE MISMATCH round %d (pushes %d, deletes stock %d replacement %d)\n", round, np, s_nA, g_delN);
            }
        }
        delete[] ops;
        ++rounds;
    }
    printf("push/erase: %d rounds, %lld pushes, %lld releases to zero, %s\n", rounds, pushes / 2, deletes, fails ? "MISMATCH" : "identical (list bytes, reference counts, release order)");
    return fails;
}

int main(int argc, char** argv) {
    // argv[1] is the path to game.dat when it is not the default install; "child" is the re-launch into the
    // mapped image.
    if (argc < 2 || strcmp(argv[1], "child"))
        return runChildWithImage(argc > 1 ? argv[1] : "C:\\AgeoftheRing\\rotwk\\game.dat");
    static const BYTE kSort[] = {0x56,0x8B,0x74,0x24,0x08,0x57,0x8B,0x7C,0x24,0x10,0x3B,0xF7,0x74,0x32};
    if (memcmp((void*)0x005740D0, kSort, sizeof(kSort))) { printf("sort bytes differ\n"); return 2; }
    tStockSort stock = (tStockSort)0x005740D0;
    tStockCore core = (tStockCore)0x00573F23;
    tStockFinal fin = (tStockFinal)0x005738FD;
    objs = (FakeObj*)VirtualAlloc(NULL, kObj * sizeof(FakeObj), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fakeVt[0] = (DWORD)(ULONG_PTR)&fakeDelete;
    for (int i = 0; i < kObj; ++i) { objs[i].vt = (DWORD)(ULONG_PTR)g_fakeVt; objs[i].refs = 1000000; }
    const int kMax = 20000;
    RlEl* A = new RlEl[kMax], *B = new RlEl[kMax], *C = new RlEl[kMax], *D = new RlEl[kMax];
    g_seqCap = 64 << 20; g_seq = (DWORD*)VirtualAlloc(NULL, g_seqCap * 2 * sizeof(DWORD), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD* seq2 = g_seq + g_seqCap;
    const tRlPred preds[2] = { (tRlPred)(ULONG_PTR)kRlLessModel, (tRlPred)(ULONG_PTR)kRlLessModelInst };
    int lists = 0; __int64 elements = 0;
    // 1) whole sort: stock vs inlined comparator vs comparator through its pointer
    for (int round = 0; round < 3; ++round) {
        for (int n = 0; n <= 400; n += (n < 70 ? 1 : 7)) {
            for (int pattern = 0; pattern < 10; ++pattern) {
                fill(A, n, pattern);
                for (int pc = 0; pc < 2; ++pc) {
                    memcpy(B, A, n * sizeof(RlEl)); memcpy(C, A, n * sizeof(RlEl)); memcpy(D, A, n * sizeof(RlEl));
                    stock(D, D + n, preds[pc]);
                    rlFastSort(B, B + n, preds[pc]);
                    RlLessCall call = { preds[pc] };
                    if (n) rlSortWith(C, C + n, call, preds[pc]);
                    if (memcmp(D, B, n * sizeof(RlEl)) || memcmp(D, C, n * sizeof(RlEl))) { if (g_fail++ < 10) printf("MISMATCH whole sort n=%d pattern=%d comparator=%d\n", n, pattern, pc); }
                    if (!refsOk()) { if (g_fail++ < 10) printf("REFCOUNT drift n=%d pattern=%d\n", n, pattern); }
                    ++lists; elements += n;
                }
            }
        }
    }
    static const int kBig[] = { 1000, 2047, 2048, 2049, 4096, 8191, 19999 };
    for (int bi = 0; bi < 7; ++bi) {
        int n = kBig[bi];
        for (int pattern = 0; pattern < 10; ++pattern) {
            fill(A, n, pattern);
            for (int pc = 0; pc < 2; ++pc) {
                memcpy(B, A, n * sizeof(RlEl)); memcpy(D, A, n * sizeof(RlEl));
                stock(D, D + n, preds[pc]);
                rlFastSort(B, B + n, preds[pc]);
                if (memcmp(D, B, n * sizeof(RlEl))) { if (g_fail++ < 10) printf("MISMATCH big n=%d pattern=%d comparator=%d\n", n, pattern, pc); }
                if (!refsOk()) { if (g_fail++ < 10) printf("REFCOUNT drift big n=%d\n", n); }
                ++lists; elements += n;
            }
        }
    }
    printf("whole sorts: %d lists, %lld elements compared\n", lists, elements);
    // 2) comparator call sequence, recorded by element id
    int seqLists = 0;
    for (int n = 0; n <= 3000; n += (n < 100 ? 3 : 97)) {
        for (int pattern = 0; pattern < 10; ++pattern) {
            fill(A, n, pattern);
            memcpy(B, A, n * sizeof(RlEl)); memcpy(D, A, n * sizeof(RlEl));
            g_seqN = 0; g_seqOverflow = false;
            stock(D, D + n, &recLess);
            size_t n1 = g_seqN; memcpy(seq2, g_seq, n1 * sizeof(DWORD));
            g_seqN = 0;
            if (n) rlFastSort(B, B + n, &recLess);
            size_t n2 = g_seqN;
            if (g_seqOverflow || n1 != n2 || memcmp(seq2, g_seq, n1 * sizeof(DWORD)) || memcmp(D, B, n * sizeof(RlEl))) {
                if (g_fail++ < 10) printf("CALL SEQUENCE differs n=%d pattern=%d (%u vs %u calls)\n", n, pattern, (unsigned)(n1 / 2), (unsigned)(n2 / 2));
            }
            ++seqLists;
        }
    }
    printf("comparator call sequences: %d lists identical\n", seqLists - (g_fail ? 1 : 0));
    // 3) budgeted core with small budgets, so the heap sort fallback runs inside the sort
    int heapLists = 0; LONG hf0 = g_rlHeapFalls;
    for (int n = 17; n <= 1500; n += 37) {
        for (int pattern = 0; pattern < 10; ++pattern) {
            for (int budget = 0; budget <= 4; ++budget) {
                fill(A, n, pattern);
                for (int pc = 0; pc < 2; ++pc) {
                    memcpy(B, A, n * sizeof(RlEl)); memcpy(D, A, n * sizeof(RlEl));
                    core(D, D + n, 0, budget, preds[pc]); fin(D, D + n, preds[pc]);
                    if (pc) rlSortBudget(B, B + n, budget, RlLessModelInst(), preds[pc]); else rlSortBudget(B, B + n, budget, RlLessModel(), preds[pc]);
                    if (memcmp(D, B, n * sizeof(RlEl))) { if (g_fail++ < 10) printf("MISMATCH budget %d n=%d pattern=%d comparator=%d\n", budget, n, pattern, pc); }
                    if (!refsOk()) { if (g_fail++ < 10) printf("REFCOUNT drift budget n=%d\n", n); }
                    ++heapLists;
                }
            }
        }
    }
    printf("budgeted sorts: %d lists, heap sort fallbacks %d\n", heapLists, (int)(g_rlHeapFalls - hf0));
    // 4) speed on render-list-like data: 2500 meshes over 120 models, instances of a model mostly adjacent already
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    for (int pc = 0; pc < 2; ++pc) {
        const int n = 2500, reps = 200;
        __int64 tS = 0, tF = 0;
        for (int r = 0; r < reps; ++r) {
            for (int i = 0; i < kObj; ++i) { objs[i].model = 0x1000 + (rnd() % 120) * 0x40; objs[i].inst = rnd() % 8; }
            for (int i = 0; i < n; ++i) { A[i].w[0] = (DWORD)(ULONG_PTR)&objs[(i / 4 + (rnd() % 3)) % kObj]; A[i].w[1] = i; for (int j = 2; j < 8; ++j) A[i].w[j] = (j < 4) ? (DWORD)(ULONG_PTR)&objs[rnd() % kObj] : 0; }
            memcpy(B, A, n * sizeof(RlEl)); memcpy(D, A, n * sizeof(RlEl));
            LARGE_INTEGER t0, t1, t2; QueryPerformanceCounter(&t0);
            stock(D, D + n, preds[pc]);
            QueryPerformanceCounter(&t1);
            rlFastSort(B, B + n, preds[pc]);
            QueryPerformanceCounter(&t2);
            tS += t1.QuadPart - t0.QuadPart; tF += t2.QuadPart - t1.QuadPart;
            if (memcmp(D, B, n * sizeof(RlEl))) { if (g_fail++ < 10) printf("MISMATCH speed data\n"); }
        }
        printf("speed comparator %d: list of %d, stock %.3f ms, replacement %.3f ms (%.1fx)\n", pc, n,
               tS * 1000.0 / qf.QuadPart / reps, tF * 1000.0 / qf.QuadPart / reps, tF ? (double)tS / (double)tF : 0.0);
    }
    g_fail += testPushErase();
    printf(g_fail ? "FAILED (%d)\n" : "ALL IDENTICAL\n", g_fail);
    return g_fail ? 1 : 0;
}
