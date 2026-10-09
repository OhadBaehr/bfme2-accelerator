// ================================================================ the game's files opened ahead of it (v58, 2026-10-08)
// WHAT WAS SLOW. Log of 2026-10-08, a saved game loaded: the 11.1 s load frame spent 44 % of its time inside
// NtCreateFile, and the 73 slow frames after it (19.6 s at 2 to 6 frames a second) 75 %. Measured on this machine
// (scratchpad/opentime.py, openpar.py, opencpu.py): the FIRST time a file is opened since the virus scanner last
// forgot it (a restart, a signature update), the open takes about 6 ms - Windows Defender's real-time protection
// looking at the file - and every open after that 0.035 ms. The game and the mod are 52,885 loose files (8,920
// models, 8,700 textures, 3,052 ini files, 26,366 sounds ...), the engine opens each one it needs on the game
// thread, one at a time: a few thousand at 6 ms apiece is the bar standing at 97 %, the first seconds of a match
// at 6 frames a second, and a hitch whenever a pan or a battle brings in something not seen yet in this session.
//
// WHAT THIS DOES. The scanner's look is per file, not per opener, and it takes them side by side: 8 threads open
// 1,300 to 1,500 new files a second (6 ms of processor time each, half of this machine for as long as it lasts),
// 1 thread 168. So from the moment the game starts, a few threads of ours open every file the game can ask for -
// read access, every kind of sharing allowed, closed at once, nothing read, nothing written:
//   1. the files the game opened in earlier sessions, in the order it opened them (kept in filewarm.lst next to
//      this DLL: the import slots of _open / _wopen are ours already, aotr_fdbuf.inc, and every read-only open is
//      noted there with how long it took);
//   2. then everything else under the folders the game runs from - the folder of its exe, its current folder,
//      the folder given with -mod, and (not their subfolders) the folders of archives it opened before - what a
//      start and a load need first (ini, maps, models, textures), sounds and films last;
//   3. the texture cache's own files.
// When the game then opens a file, the scanner has seen it and the open takes its 0.035 ms. Nothing the game
// reads or gets is different; a file is never opened for the game, only before it.
// While frames are being drawn the number of threads drops (the scanning is real processor work); before the
// first frame and through loading screens all of them run. The whole tree is done in about 40 s on this machine.
// [Engine] FileWarm (1 / 0), FileWarmThreads (8) / AOTR_FILEWARM=0. The log's `files:` lines say what the game
// still had to wait for.

#define FW_MAXENT  (1 << 17)
#define FW_TABLE   (1 << 18)
#define FW_ARENA   (16u << 20)
#define FW_MAXROOT 24
struct FwEnt { DWORD off; DWORD hash; volatile LONG flags; };              // flags: 1 the game opened it this session, 2 opened ahead by us, 4 on the saved list, 8 queued
static FwEnt* g_fwEnt = NULL; static volatile LONG g_fwN = 0;
static DWORD* g_fwTab = NULL;
static char* g_fwArena = NULL; static DWORD g_fwArenaUsed = 0, g_fwArenaCommit = 0;
static CRITICAL_SECTION g_fwCs;
static LONG* g_fwOrder = NULL; static volatile LONG g_fwNOrder = 0, g_fwNOrderSaved = 0;
static LONG* g_fwWork = NULL; static volatile LONG g_fwWorkN = 0, g_fwNext = 0;
static volatile LONG g_fwOn = 0, g_fwThreads = 8, g_fwThreadsPlay = 3, g_fwFlowing = 0, g_fwWalkDone = 0;
struct FwRoot { char p[600]; int len; int recursive; };
static FwRoot g_fwRoot[FW_MAXROOT]; static int g_fwNRoot = 0;
static volatile LONG g_fwNWarm = 0, g_fwNWarmSlow = 0, g_fwNWarmFail = 0;       // opened ahead; of them slow (the scanner's first look); could not be opened
static volatile LONG64 g_fwWarmTicks = 0;
static volatile LONG g_fwNGame = 0, g_fwNGameSlow = 0, g_fwNGameAhead = 0, g_fwGameMaxUs = 0;
static volatile LONG64 g_fwGameSlowTicks = 0;
static char g_fwSlowName[300];
static char g_fwListPath[MAX_PATH];

static __forceinline DWORD fwHash(const char* p, int n) { DWORD h = 0x811C9DC5u; for (int i = 0; i < n; ++i) h = (h ^ (BYTE)p[i]) * 0x01000193u; return h; }
// a path as the table keeps it: full, lower case, UTF-8. Returns its length or 0.
static int fwNormW(const wchar_t* in, char* out, int cap) {
    wchar_t full[1024];
    DWORD n = GetFullPathNameW(in, 1024, full, NULL);
    if (!n || n >= 1024) return 0;
    CharLowerBuffW(full, n);
    int k = WideCharToMultiByte(CP_UTF8, 0, full, (int)n, out, cap - 1, NULL, NULL);
    if (k <= 0) return 0;
    out[k] = 0;
    return k;
}
static int fwNormA(const char* in, char* out, int cap) {
    wchar_t w[1024];
    int n = MultiByteToWideChar(CP_ACP, 0, in, -1, w, 1024);
    if (n <= 1) return 0;
    return fwNormW(w, out, cap);
}
// the entry of a normalised path (made if it is not there); -1 when the table is full. Caller holds g_fwCs.
static LONG fwIntern(const char* p, int n, bool* isNew) {
    DWORD h = fwHash(p, n);
    for (DWORD i = h & (FW_TABLE - 1), probes = 0; probes < FW_TABLE; i = (i + 1) & (FW_TABLE - 1), ++probes) {
        DWORD v = g_fwTab[i];
        if (!v) {
            if (g_fwN >= FW_MAXENT || g_fwArenaUsed + (DWORD)n + 1 > FW_ARENA) return -1;
            if (g_fwArenaUsed + (DWORD)n + 1 > g_fwArenaCommit) {
                DWORD more = 1u << 20;
                if (!VirtualAlloc(g_fwArena + g_fwArenaCommit, more, MEM_COMMIT, PAGE_READWRITE)) return -1;
                g_fwArenaCommit += more;
            }
            LONG e = g_fwN;
            g_fwEnt[e].off = g_fwArenaUsed; g_fwEnt[e].hash = h; g_fwEnt[e].flags = 0;
            memcpy(g_fwArena + g_fwArenaUsed, p, (size_t)n + 1); g_fwArenaUsed += (DWORD)n + 1;
            g_fwTab[i] = (DWORD)e + 1; g_fwN = e + 1;
            if (isNew) *isNew = true;
            return e;
        }
        const FwEnt& x = g_fwEnt[v - 1];
        if (x.hash == h && !strcmp(g_fwArena + x.off, p)) { if (isNew) *isNew = false; return (LONG)(v - 1); }
    }
    return -1;
}
static bool fwUnderRoot(const char* p, int n) {
    for (int r = 0; r < g_fwNRoot; ++r) {
        const FwRoot& R = g_fwRoot[r];
        if (n > R.len && !memcmp(p, R.p, (size_t)R.len)) { if (R.recursive) return true; if (!strchr(p + R.len, '\\')) return true; }
    }
    return false;
}
static void fwAddRoot(const wchar_t* dir, int recursive) {
    if (g_fwNRoot >= FW_MAXROOT || !dir || !dir[0]) return;
    char b[600]; int n = fwNormW(dir, b, sizeof(b) - 2);
    if (n < 4) return;                                                     // never a whole drive
    if (b[n - 1] != '\\') { b[n++] = '\\'; b[n] = 0; }
    DWORD at = GetFileAttributesW(dir);
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) return;
    for (int r = 0; r < g_fwNRoot; ++r) {
        if (g_fwRoot[r].len <= n && !memcmp(g_fwRoot[r].p, b, (size_t)g_fwRoot[r].len) && g_fwRoot[r].recursive) return;       // inside one that is walked whole
        if (g_fwRoot[r].len == n && !memcmp(g_fwRoot[r].p, b, (size_t)n)) { if (recursive) g_fwRoot[r].recursive = 1; return; }
    }
    FwRoot& R = g_fwRoot[g_fwNRoot++]; memcpy(R.p, b, (size_t)n + 1); R.len = n; R.recursive = recursive;
}

// ---------------------------------------------------------------- the game's own opens (from aotr_fdbuf.inc's hooks)
static void fwNoteOpen(const char* a, const wchar_t* w, int oflag, LONG64 ticks) {
    if (!g_fwOn || ticks < 0 || (oflag & 0x0303)) return;                  // read-only opens that create nothing
    char p[2100]; int n = a ? fwNormA(a, p, sizeof(p)) : fwNormW(w, p, sizeof(p));
    if (!n) return;
    LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
    LONG us = (LONG)(ticks * 1000000 / f);
    EnterCriticalSection(&g_fwCs);
    LONG e = fwIntern(p, n, NULL);
    LONG was = 0;
    if (e >= 0) {
        was = InterlockedOr(&g_fwEnt[e].flags, 1);
        if (!(was & 1) && g_fwNOrder < FW_MAXENT) g_fwOrder[g_fwNOrder++] = e;
    }
    LeaveCriticalSection(&g_fwCs);
    g_fwNGame++;
    if (was & 2) g_fwNGameAhead++;
    if (us > 1000) {
        g_fwNGameSlow++; g_fwGameSlowTicks += ticks;
        if (us > g_fwGameMaxUs) { g_fwGameMaxUs = us; int k = n < (int)sizeof(g_fwSlowName) - 1 ? n : (int)sizeof(g_fwSlowName) - 1; memcpy(g_fwSlowName, p + (n - k), (size_t)k); g_fwSlowName[k] = 0; }
    }
}

// ---------------------------------------------------------------- opening ahead
static void fwQueue(LONG e) {                                               // caller holds g_fwCs
    if (g_fwEnt[e].flags & (1 | 2 | 8)) return;
    if (g_fwWorkN >= FW_MAXENT) return;
    InterlockedOr(&g_fwEnt[e].flags, 8);
    g_fwWork[g_fwWorkN] = e;
    g_fwWorkN = g_fwWorkN + 1;
}
static DWORD WINAPI fwWorker(LPVOID arg) {
    LONG me = (LONG)(LONG_PTR)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
    for (;;) {
        if (!g_fwOn) { Sleep(500); continue; }
        if (me >= (g_fwFlowing ? g_fwThreadsPlay : g_fwThreads)) { Sleep(40); continue; }      // fewer of us while frames are being drawn
        LONG i = g_fwNext;
        if (i >= g_fwWorkN) { Sleep(g_fwWalkDone ? 400 : 15); continue; }
        if (InterlockedCompareExchange(&g_fwNext, i + 1, i) != i) continue;      // another thread took that one
        LONG e = g_fwWork[i];
        if (g_fwEnt[e].flags & (1 | 2)) continue;                          // the game got there first
        wchar_t w[1024];
        if (MultiByteToWideChar(CP_UTF8, 0, g_fwArena + g_fwEnt[e].off, -1, w, 1024) <= 0) continue;
        LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
        HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        QueryPerformanceCounter(&t1);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h); else g_fwNWarmFail++;
        InterlockedOr(&g_fwEnt[e].flags, 2);
        LONG64 dt = t1.QuadPart - t0.QuadPart;
        InterlockedIncrement(&g_fwNWarm);
        if (dt * 1000 > f) { InterlockedIncrement(&g_fwNWarmSlow); InterlockedExchangeAdd64(&g_fwWarmTicks, dt); }
    }
}
// what a start and a load ask for first comes first; sounds and films last
static int fwClass(const wchar_t* name) {
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot) return 3;
    if (!_wcsicmp(dot, L".exe") || !_wcsicmp(dot, L".dll") || !_wcsicmp(dot, L".asi")) return -1;       // programs: the scanner looks at those every time, and the game does not read them as data
    if (!_wcsicmp(dot, L".tmp") || !_wcsicmp(dot, L".log") || !_wcsicmp(dot, L".dmp") || !_wcsicmp(dot, L".lst") || !_wcsicmp(dot, L".dxvk-cache") || !_wcsicmp(dot, L".lock")) return -1;   // files something writes while the game runs: left alone
    static const wchar_t* k0[] = { L".ini", L".inc", L".str", L".csf", L".big", L".bse", L".map", L".wnd", L".lua", L".scb", L".xml", L".txt", L".dat", L".ru", NULL };
    static const wchar_t* k1[] = { L".w3d", L".wak", L".skn", L".skl", NULL };
    static const wchar_t* k2[] = { L".dds", L".tga", L".jpg", L".bmp", L".txc", L".fx", L".fxo", NULL };
    static const wchar_t* k4[] = { L".wav", L".mp3", NULL };
    static const wchar_t* k5[] = { L".vp6", L".bik", L".mp4", L".avi", NULL };
    for (int i = 0; k0[i]; ++i) if (!_wcsicmp(dot, k0[i])) return 0;
    for (int i = 0; k1[i]; ++i) if (!_wcsicmp(dot, k1[i])) return 1;
    for (int i = 0; k2[i]; ++i) if (!_wcsicmp(dot, k2[i])) return 2;
    for (int i = 0; k4[i]; ++i) if (!_wcsicmp(dot, k4[i])) return 4;
    for (int i = 0; k5[i]; ++i) if (!_wcsicmp(dot, k5[i])) return 5;
    return 3;
}
struct FwFound { LONG ent; int cls; };
static FwFound* g_fwFound = NULL; static LONG g_fwNFound = 0;
static void fwWalk(const wchar_t* dir, const char* dirNorm, int dirNormLen, int recursive, int depth) {
    if (depth > 24 || g_fwNFound >= FW_MAXENT) return;
    wchar_t pat[1024]; int dl = (int)wcslen(dir);
    if (dl > 900) return;
    memcpy(pat, dir, (size_t)dl * sizeof(wchar_t)); pat[dl] = L'*'; pat[dl + 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' && (!fd.cFileName[1] || (fd.cFileName[1] == L'.' && !fd.cFileName[2]))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        int nl = (int)wcslen(fd.cFileName);
        if (dl + nl + 2 >= 1000) continue;
        wchar_t low[300]; if (nl >= 299) continue;
        memcpy(low, fd.cFileName, ((size_t)nl + 1) * sizeof(wchar_t)); CharLowerBuffW(low, (DWORD)nl);
        char p[2100];
        memcpy(p, dirNorm, (size_t)dirNormLen);
        int k = WideCharToMultiByte(CP_UTF8, 0, low, nl, p + dirNormLen, (int)sizeof(p) - dirNormLen - 3, NULL, NULL);
        if (k <= 0) continue;
        int n = dirNormLen + k; p[n] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!recursive) continue;
            wchar_t sub[1024]; memcpy(sub, dir, (size_t)dl * sizeof(wchar_t)); memcpy(sub + dl, fd.cFileName, (size_t)nl * sizeof(wchar_t)); sub[dl + nl] = L'\\'; sub[dl + nl + 1] = 0;
            p[n] = '\\'; p[n + 1] = 0;
            fwWalk(sub, p, n + 1, recursive, depth + 1);
            continue;
        }
        int cls = fwClass(fd.cFileName);
        if (cls < 0) continue;
        EnterCriticalSection(&g_fwCs);
        LONG e = fwIntern(p, n, NULL);
        if (e >= 0 && !(g_fwEnt[e].flags & (1 | 2 | 8)) && g_fwNFound < FW_MAXENT) { g_fwFound[g_fwNFound].ent = e; g_fwFound[g_fwNFound].cls = cls; g_fwNFound++; }
        LeaveCriticalSection(&g_fwCs);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static void fwQueueFound() {                                                // by class, in the order found
    for (int c = 0; c <= 5; ++c) {
        EnterCriticalSection(&g_fwCs);
        for (LONG i = 0; i < g_fwNFound; ++i) if (g_fwFound[i].cls == c) fwQueue(g_fwFound[i].ent);
        LeaveCriticalSection(&g_fwCs);
    }
    g_fwNFound = 0;
}
// the list of earlier sessions: one path a line, the order the game opened them
static LONG fwLoadList() {
    HANDLE h = CreateFileA(g_fwListPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD size = GetFileSize(h, NULL), got = 0; LONG n = 0;
    char* buf = (size && size < (64u << 20)) ? (char*)VirtualAlloc(NULL, size + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : NULL;
    if (buf && ReadFile(h, buf, size, &got, NULL) && got == size) {
        buf[size] = 0;
        // an archive opened before, outside the folders known so far: its folder (itself only) is the base game's
        for (char* q = buf; *q; ) {
            char* e = q; while (*e && *e != '\n' && *e != '\r') ++e;
            int len = (int)(e - q);
            if (len > 8 && len < 900 && !memcmp(e - 4, ".big", 4) && !fwUnderRoot(q, len)) {
                char d[1000]; memcpy(d, q, (size_t)len); d[len] = 0;
                char* sl = strrchr(d, '\\');
                if (sl) { sl[1] = 0; wchar_t w[1000]; if (MultiByteToWideChar(CP_UTF8, 0, d, -1, w, 1000) > 0) fwAddRoot(w, 0); }
            }
            q = e; while (*q == '\n' || *q == '\r') ++q;
        }
        char* p = buf;
        while (*p) {
            char* e = p; while (*e && *e != '\n' && *e != '\r') ++e;
            int len = (int)(e - p); char c = *e; *e = 0;
            if (len > 3 && len < 2000 && fwUnderRoot(p, len)) {
                EnterCriticalSection(&g_fwCs);
                bool isNew = false; LONG ent = fwIntern(p, len, &isNew);
                if (ent >= 0) { InterlockedOr(&g_fwEnt[ent].flags, 4); fwQueue(ent); ++n; }
                LeaveCriticalSection(&g_fwCs);
            }
            if (!c) break;
            p = e + 1; while (*p == '\n' || *p == '\r') ++p;
        }
    }
    if (buf) VirtualFree(buf, 0, MEM_RELEASE);
    CloseHandle(h);
    return n;
}
// written back: this session's opens in their order, then what the list had before that was not opened this time
static void fwSaveList() {
    LONG nOrder = g_fwNOrder, nEnt = g_fwN;
    if (nOrder == g_fwNOrderSaved) return;
    char tmp[MAX_PATH + 8]; sprintf(tmp, "%s.tmp", g_fwListPath);
    HANDLE h = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    static char out[1 << 16]; DWORD used = 0, wr = 0; LONG lines = 0; bool ok = true;
    for (int pass = 0; pass < 2 && ok; ++pass) {
        LONG cnt = pass ? nEnt : nOrder;
        for (LONG i = 0; i < cnt && lines < 90000 && ok; ++i) {
            LONG e = pass ? i : g_fwOrder[i];
            LONG fl = g_fwEnt[e].flags;
            if (pass && (!(fl & 4) || (fl & 1))) continue;                  // second pass: only what the old list had and this session did not open
            const char* p = g_fwArena + g_fwEnt[e].off; int n = (int)strlen(p);
            if (!fwUnderRoot(p, n)) continue;
            if (used + (DWORD)n + 2 > sizeof(out)) { ok = WriteFile(h, out, used, &wr, NULL) && wr == used; used = 0; }
            memcpy(out + used, p, (size_t)n); used += (DWORD)n; out[used++] = '\n'; ++lines;
        }
    }
    if (ok && used) ok = WriteFile(h, out, used, &wr, NULL) && wr == used;
    CloseHandle(h);
    if (ok && MoveFileExA(tmp, g_fwListPath, MOVEFILE_REPLACE_EXISTING)) g_fwNOrderSaved = nOrder; else DeleteFileA(tmp);
}
static DWORD WINAPI fwDirector(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1, q0 = qpcNow();
    LONG fromList = fwLoadList();
    logf("files: opening the game's files ahead of it on %d threads (%d while frames are drawn) - %d from earlier sessions first, then everything under %d folders.", (int)g_fwThreads, (int)g_fwThreadsPlay, (int)fromList, g_fwNRoot);
    g_fwFound = (FwFound*)VirtualAlloc(NULL, sizeof(FwFound) * FW_MAXENT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (g_fwFound) {
        for (int r = 0; r < g_fwNRoot; ++r) {
            wchar_t w[700];
            if (MultiByteToWideChar(CP_UTF8, 0, g_fwRoot[r].p, -1, w, 700) <= 0) continue;
            fwWalk(w, g_fwRoot[r].p, g_fwRoot[r].len, g_fwRoot[r].recursive, 0);
        }
        LONG found = g_fwNFound;
        fwQueueFound();
        VirtualFree(g_fwFound, 0, MEM_RELEASE); g_fwFound = NULL;
        logf("files: %d more files found under those folders in %d ms; %d queued in all.", (int)found, (int)((qpcNow() - q0) * 1000 / f), (int)g_fwWorkN);
    }
    g_fwWalkDone = 1;
    // service: fewer threads while frames flow, the list written back, the log
    LONG lastFrames = (LONG)g_mkFrames, lastGame = 0, lastSlow = 0, lastAhead = 0, saidDone = 0, tick = 0; LONG64 lastSlowT = 0;
    for (;;) {
        Sleep(250); ++tick;
        LONG fr = (LONG)g_mkFrames; g_fwFlowing = (fr - lastFrames) >= 5 ? 1 : 0; lastFrames = fr;
        if (!saidDone && g_fwNext >= g_fwWorkN) {
            saidDone = 1;
            logf("files: every file is opened - %d opened ahead of the game in %d.%d s, %d of them had not been looked at by the virus scanner yet (%d.%d s of its looking, which the game thread would have waited for one file at a time); %d could not be opened.",
                 (int)g_fwNWarm, (int)((qpcNow() - q0) / f), (int)(((qpcNow() - q0) * 10 / f) % 10), (int)g_fwNWarmSlow, (int)(g_fwWarmTicks / f), (int)((g_fwWarmTicks * 10 / f) % 10), (int)g_fwNWarmFail);
        }
        if (tick % 20 == 0) {                                               // every 5 s
            LONG g = g_fwNGame, s = g_fwNGameSlow, a = g_fwNGameAhead, wm = g_fwNWarm; LONG64 st = g_fwGameSlowTicks;
            if (g - lastGame >= 20 || s != lastSlow) {
                LONG mx = InterlockedExchange(&g_fwGameMaxUs, 0);
                char nm[300]; memcpy(nm, g_fwSlowName, sizeof(nm)); nm[sizeof(nm) - 1] = 0;
                logf("files: last 5 s - the game opened %d files, %d of them had been opened ahead; %d took more than a millisecond (%d ms in all) | opened ahead so far %d, still to go %d",
                     (int)(g - lastGame), (int)(a - lastAhead), (int)(s - lastSlow), (int)((st - lastSlowT) * 1000 / f), (int)wm, (int)(g_fwWorkN - (g_fwNext < g_fwWorkN ? g_fwNext : g_fwWorkN)));
                if (mx > 1000) logf("files:   the longest of those: %d.%d ms for ...%s", (int)(mx / 1000), (int)((mx % 1000) / 100), nm);
            }
            lastGame = g; lastSlow = s; lastAhead = a; lastSlowT = st;
        }
        if (tick % 60 == 0) fwSaveList();                                   // every 15 s
    }
}

// the argument after -mod on a command line (quoted or not); false when there is none
static bool fwModArg(const wchar_t* cl, wchar_t* out, int cap) {
    if (!cl) return false;
    for (const wchar_t* p = cl; *p; ++p) {
        if ((p[0] == L'-' || p[0] == L'/') && (p[1] == L'm' || p[1] == L'M') && (p[2] == L'o' || p[2] == L'O') && (p[3] == L'd' || p[3] == L'D') && (p[4] == L' ' || p[4] == L'\t') && (p == cl || p[-1] == L' ' || p[-1] == L'\t' || p[-1] == L'"')) {
            p += 5; while (*p == L' ' || *p == L'\t') ++p;
            int n = 0;
            if (*p == L'"') { ++p; while (*p && *p != L'"' && n < cap - 1) out[n++] = *p++; }
            else { while (*p && *p != L' ' && *p != L'\t' && n < cap - 1) out[n++] = *p++; }
            out[n] = 0;
            return n > 0;
        }
    }
    return false;
}
static void installFileWarm() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    bool have = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
    int want = have ? (int)GetPrivateProfileIntA("Engine", "FileWarm", 1, ini) : 1;
    int threads = have ? (int)GetPrivateProfileIntA("Engine", "FileWarmThreads", -1, ini) : -1;
    char env[8]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_FILEWARM", env, sizeof(env)) && env[0]) want = env[0] != '0';
    if (want <= 0) { logf("files: opening ahead is off ([Engine] FileWarm = 0) - the first open of every file waits for the virus scanner on the game thread."); return; }
    SYSTEM_INFO si; GetSystemInfo(&si);
    if (threads < 1) { threads = (int)si.dwNumberOfProcessors / 2; if (threads > 8) threads = 8; if (threads < 2) threads = 2; }       // on its own: half the processors, eight at most (more gains little: openpar.py)
    if (threads > 16) threads = 16;
    if (threads > (int)si.dwNumberOfProcessors) threads = (int)si.dwNumberOfProcessors;
    g_fwThreads = threads; g_fwThreadsPlay = threads >= 6 ? 3 : threads >= 3 ? 2 : 1;
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    InitializeCriticalSection(&g_fwCs);
    g_fwEnt = (FwEnt*)VirtualAlloc(NULL, sizeof(FwEnt) * FW_MAXENT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fwTab = (DWORD*)VirtualAlloc(NULL, sizeof(DWORD) * FW_TABLE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fwOrder = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * FW_MAXENT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fwWork = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * FW_MAXENT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fwArena = (char*)VirtualAlloc(NULL, FW_ARENA, MEM_RESERVE, PAGE_READWRITE);
    if (!g_fwEnt || !g_fwTab || !g_fwOrder || !g_fwWork || !g_fwArena) { logf("files: no memory for the list of files - opening ahead is off."); return; }
    aotrPath(g_fwListPath, "filewarm.lst");
    // the folders the game runs from
    { wchar_t w[1024]; DWORD n = GetModuleFileNameW(NULL, w, 1024);
      if (n && n < 1024) { wchar_t* s = wcsrchr(w, L'\\'); if (s) { s[1] = 0; fwAddRoot(w, 1); } }
      n = GetCurrentDirectoryW(1024, w); if (n && n < 1000) fwAddRoot(w, 1);
      // -mod <folder or archive>
      { wchar_t m[1024];
        if (fwModArg(GetCommandLineW(), m, 1024)) {
            DWORD at = GetFileAttributesW(m);
            if (at != INVALID_FILE_ATTRIBUTES) { if (at & FILE_ATTRIBUTE_DIRECTORY) fwAddRoot(m, 1);
                                                 else { wchar_t* sl = wcsrchr(m, L'\\'); if (sl) { sl[1] = 0; fwAddRoot(m, 0); } } } } }
      // our own texture cache (read by the texture workers; a first open there holds a texture up just the same)
      char tc[MAX_PATH]; aotrPath(tc, "texcache"); wchar_t tw[MAX_PATH]; if (MultiByteToWideChar(CP_ACP, 0, tc, -1, tw, MAX_PATH) > 0) fwAddRoot(tw, 1); }
    InterlockedExchange(&g_fwOn, 1);
    g_fdbNoteOpen = &fwNoteOpen;
    for (int i = 0; i < threads; ++i) { HANDLE t = CreateThread(NULL, 128 * 1024, fwWorker, (LPVOID)(LONG_PTR)i, 0, NULL); if (t) CloseHandle(t); }
    { HANDLE t = CreateThread(NULL, 0, fwDirector, NULL, 0, NULL); if (t) CloseHandle(t); }
    char roots[1400]; roots[0] = 0; int ro = 0;
    for (int r = 0; r < g_fwNRoot && ro < 1200; ++r) ro += sprintf(roots + ro, "%s%s%s", r ? ", " : "", g_fwRoot[r].p, g_fwRoot[r].recursive ? "" : " (itself only)");
    logf("files: opening ahead installed - folders: %s.", roots);
}

// ---------------------------------------------------------------- offline test
// A folder of made-up files: the walk finds every one once, in class order; the list is written and read back in
// the order of "the game's" opens; a file outside the folders is never queued; the workers open them all.
extern "C" __declspec(dllexport) int __cdecl AotrFileWarmTest(char* out, int nOut) {
    int fail = 0;
    if (g_fwOn) { if (out && nOut > 60) sprintf(out, "file warm: already running in this process - not tested here"); return 0; }
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    wchar_t tmp[MAX_PATH], dir[MAX_PATH], other[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    swprintf(dir, MAX_PATH, L"%sAotrFwTest_%u\\", tmp, GetCurrentProcessId()); swprintf(other, MAX_PATH, L"%sAotrFwOther_%u\\", tmp, GetCurrentProcessId());
    CreateDirectoryW(dir, NULL); CreateDirectoryW(other, NULL);
    wchar_t sub[MAX_PATH]; swprintf(sub, MAX_PATH, L"%sArt\\", dir); CreateDirectoryW(sub, NULL);
    static const wchar_t* names[] = { L"Art\\Tree.W3D", L"Art\\bark.dds", L"data.INI", L"Art\\song.wav", L"Art\\film.vp6", L"readme", L"Art\\Sub Dir.tga" };
    const int NF = 7;
    for (int i = 0; i < NF; ++i) { wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%s%s", dir, names[i]); HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); if (h != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(h, "x", 1, &wr, NULL); CloseHandle(h); } else fail |= 1; }
    { wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%soutside.ini", other); HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    InitializeCriticalSection(&g_fwCs);
    g_fwEnt = (FwEnt*)VirtualAlloc(NULL, sizeof(FwEnt) * FW_MAXENT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fwTab = (DWORD*)VirtualAlloc(NULL, sizeof(DWORD) * FW_TABLE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fwOrder = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * FW_MAXENT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fwWork = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * FW_MAXENT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fwArena = (char*)VirtualAlloc(NULL, FW_ARENA, MEM_RESERVE, PAGE_READWRITE);
    g_fwFound = (FwFound*)VirtualAlloc(NULL, sizeof(FwFound) * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_fwEnt || !g_fwTab || !g_fwOrder || !g_fwWork || !g_fwArena || !g_fwFound) return 99;
    char lp[MAX_PATH * 3]; WideCharToMultiByte(CP_ACP, 0, other, -1, lp, sizeof(lp), NULL, NULL); sprintf(g_fwListPath, "%sfilewarm.lst", lp);      // (the list lives outside the folders that are walked, as it does next to the DLL)
    fwAddRoot(dir, 1);
    if (g_fwNRoot != 1) fail |= 2;
    InterlockedExchange(&g_fwOn, 1);
    // "the game" opens two files (one through the ANSI form with other spelling, one wide), then a file outside the folders, then one for writing
    { char a[MAX_PATH * 3]; wchar_t p[MAX_PATH];
      swprintf(p, MAX_PATH, L"%sART/bark.DDS", dir); WideCharToMultiByte(CP_ACP, 0, p, -1, a, sizeof(a), NULL, NULL); fwNoteOpen(a, NULL, 0x8000, 10);
      swprintf(p, MAX_PATH, L"%sdata.ini", dir); fwNoteOpen(NULL, p, 0x8000, g_pqpf.QuadPart / 100);        // a 10 ms open: counted as slow
      swprintf(p, MAX_PATH, L"%soutside.ini", other); fwNoteOpen(NULL, p, 0x8000, 10);
      swprintf(p, MAX_PATH, L"%sreadme", dir); fwNoteOpen(NULL, p, 0x8001, 10); }                             // opened for writing: not noted
    if (g_fwNOrder != 3 || g_fwNGame != 3 || g_fwNGameSlow != 1) fail |= 4;
    fwSaveList();
    // the walk: everything but the two the game opened, by class
    { char rn[700]; int rl = fwNormW(dir, rn, sizeof(rn)); fwWalk(dir, rn, rl, 1, 0); }
    if (g_fwNFound != NF - 2) fail |= 8;
    fwQueueFound();
    if (g_fwWorkN != NF - 2) fail |= 16;
    { int lastCls = -1; for (LONG i = 0; i < g_fwWorkN; ++i) { const char* p = g_fwArena + g_fwEnt[g_fwWork[i]].off; wchar_t w[1024]; MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1024); int c = fwClass(w); if (c < lastCls) fail |= 32; lastCls = c; } }
    // the list read back into a fresh table: the two files under the folder, in the order they were opened; the outside one is not on it
    LONG nEntBefore = g_fwN;
    memset(g_fwTab, 0, sizeof(DWORD) * FW_TABLE); g_fwN = 0; g_fwArenaUsed = 0; g_fwWorkN = 0; g_fwNext = 0; g_fwNOrder = 0; g_fwNOrderSaved = 0;
    LONG fromList = fwLoadList();
    if (fromList != 2 || g_fwWorkN != 2) fail |= 64;
    else { const char* a = g_fwArena + g_fwEnt[g_fwWork[0]].off; const char* b = g_fwArena + g_fwEnt[g_fwWork[1]].off; if (!strstr(a, "art\\bark.dds") || !strstr(b, "data.ini")) fail |= 128; }
    // the workers: everything queued gets opened (walk again for the rest)
    { char rn[700]; int rl = fwNormW(dir, rn, sizeof(rn)); g_fwNFound = 0; fwWalk(dir, rn, rl, 1, 0); fwQueueFound(); }
    if (g_fwWorkN != NF) fail |= 256;
    g_fwThreads = 3; g_fwThreadsPlay = 1; g_fwWalkDone = 1;
    for (int i = 0; i < 3; ++i) { HANDLE t = CreateThread(NULL, 0, fwWorker, (LPVOID)(LONG_PTR)i, 0, NULL); if (t) CloseHandle(t); }
    for (int w = 0; w < 400 && g_fwNWarm < NF; ++w) Sleep(10);
    if (g_fwNWarm != NF || g_fwNWarmFail != 0) fail |= 512;
    InterlockedExchange(&g_fwOn, 0);
    Sleep(60);
    for (int i = 0; i < NF; ++i) { wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%s%s", dir, names[i]); DeleteFileW(p); }
    { wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%sfilewarm.lst", other); DeleteFileW(p); swprintf(p, MAX_PATH, L"%soutside.ini", other); DeleteFileW(p); }
    RemoveDirectoryW(sub); RemoveDirectoryW(dir); RemoveDirectoryW(other);
    { wchar_t m[64];
      if (!fwModArg(L"\"C:\\Game\\lotr.exe\" -xres 3840 -mod \"C:\\Age of the Ring\\aotr\" -noshellmap", m, 64) || wcscmp(m, L"C:\\Age of the Ring\\aotr")) fail |= 1024;
      if (!fwModArg(L"game.dat -MOD D:\\mods\\x.big", m, 64) || wcscmp(m, L"D:\\mods\\x.big")) fail |= 1024;
      if (fwModArg(L"game.dat -model 3 -modx y", m, 64)) fail |= 1024; }
    if (out && nOut > 300) sprintf(out, "file warm: %s (flags %03X) - %d made-up files: the walk found the %d the game had not opened, queued by kind; the list of the game's opens came back in order (%d lines, the file outside the folders and the one opened for writing not on it); %d opened by the workers, %d failed; %d paths known",
                                fail ? "FAILED" : "all as it must be", fail, NF, NF - 2, (int)fromList, (int)g_fwNWarm, (int)g_fwNWarmFail, (int)nEntBefore);
    return fail;
}

// The real thing over a real folder, from a test program: the walk, the queue, `threads` workers, until every file
// has been opened; how many there were, how long it took, how many of the opens were slow (the scanner's first look).
extern "C" __declspec(dllexport) int __cdecl AotrFileWarmRun(const wchar_t* root, int threads, char* out, int nOut) {
    if (g_fwOn || g_fwEnt) { if (out && nOut > 60) sprintf(out, "file warm: already set up in this process"); return 1; }
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    InitializeCriticalSection(&g_fwCs);
    g_fwEnt = (FwEnt*)VirtualAlloc(NULL, sizeof(FwEnt) * FW_MAXENT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fwTab = (DWORD*)VirtualAlloc(NULL, sizeof(DWORD) * FW_TABLE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fwOrder = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * FW_MAXENT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fwWork = (LONG*)VirtualAlloc(NULL, sizeof(LONG) * FW_MAXENT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_fwArena = (char*)VirtualAlloc(NULL, FW_ARENA, MEM_RESERVE, PAGE_READWRITE);
    g_fwFound = (FwFound*)VirtualAlloc(NULL, sizeof(FwFound) * FW_MAXENT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_fwEnt || !g_fwTab || !g_fwOrder || !g_fwWork || !g_fwArena || !g_fwFound) return 99;
    fwAddRoot(root, 1);
    if (g_fwNRoot != 1) { if (out && nOut > 60) sprintf(out, "file warm: not a folder"); return 2; }
    LONG64 f = g_pqpf.QuadPart, q0 = qpcNow();
    { wchar_t w[700]; MultiByteToWideChar(CP_UTF8, 0, g_fwRoot[0].p, -1, w, 700); fwWalk(w, g_fwRoot[0].p, g_fwRoot[0].len, 1, 0); }
    LONG found = g_fwNFound;
    fwQueueFound();
    LONG64 q1 = qpcNow();
    if (threads < 1) threads = 1; if (threads > 32) threads = 32;
    g_fwThreads = threads; g_fwThreadsPlay = threads; g_fwWalkDone = 1;
    InterlockedExchange(&g_fwOn, 1);
    for (int i = 0; i < threads; ++i) { HANDLE t = CreateThread(NULL, 0, fwWorker, (LPVOID)(LONG_PTR)i, 0, NULL); if (t) CloseHandle(t); }
    while (g_fwNWarm < g_fwWorkN) Sleep(5);
    LONG64 q2 = qpcNow();
    InterlockedExchange(&g_fwOn, 0);
    if (out && nOut > 300) sprintf(out, "file warm: %d files under the folder (%d paths, %d KB of names), found in %.0f ms; all opened on %d threads in %.2f s = %.0f files a second; %d of the opens took more than a millisecond (%.1f s of waiting in all, %.2f ms each); %d could not be opened",
                                (int)found, (int)g_fwN, (int)(g_fwArenaUsed >> 10), (q1 - q0) * 1000.0 / f, threads, (double)(q2 - q1) / f, g_fwWorkN / ((double)(q2 - q1) / f),
                                (int)g_fwNWarmSlow, (double)g_fwWarmTicks / f, g_fwNWarmSlow ? (double)g_fwWarmTicks * 1000.0 / f / g_fwNWarmSlow : 0.0, (int)g_fwNWarmFail);
    return 0;
}
