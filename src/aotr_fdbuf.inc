// aotr_fdbuf.inc - read-ahead for the engine's file reads (v52).
//
// WHAT WAS SLOW (long-frame profile of a save load, 2026-10-07): a quarter of the whole load - 9 s - was the game
// thread inside NtReadFile, called from LocalFile::read (game.dat 0x00A19AF0) under the W3D chunk reader
// (0x00A28E70). The engine's file class sits directly on the C runtime's low-level _read / _lseek, which do no
// buffering at all: every 4-byte count, every 8-byte chunk header, every channel of every animation is its own
// system call (and its own pair of lock operations inside msvcr71).
//
// WHAT THIS DOES: the game's import slots for _read, _lseek, _write, _close, _open, _wopen and _get_osfhandle point
// here. A file opened read-only in binary mode gets read-ahead windows (4 KB, growing to 64 KB while it is read
// forwards; up to four per file so that streams interleaved on one archive handle keep theirs); small reads are
// copies out of a window and seeks inside the file are arithmetic. The position the program sees is tracked here,
// the real handle is moved only when a window is filled. Everything else - text mode, files open for writing,
// devices, pipes, a handle the program asked the OS handle of - goes straight to msvcr71 as before.
//
// SAME BYTES, SAME RETURN VALUES: the offline test (fdbuf_test.cpp) runs random reads, seeks (all three origins,
// before the start and past the end), skips and end-of-file reads against msvcr71's own functions on a second
// handle of the same file and compares every result and every byte, single- and multi-threaded.
//
// The includer supplies FDB_LOG(fmt, ...) or nothing.
#ifndef FDB_LOG
#define FDB_LOG(...) ((void)0)
#endif
#define FDB_MAXFD  2048
#define FDB_WINMAX (64u << 10)
#define FDB_WINMIN (4u << 10)
#define FDB_NWIN   4
#define FDB_DIRECT (32u << 10)          // a read this large goes straight into the caller's buffer

typedef int  (__cdecl* FdbReadF)(int, void*, unsigned);
typedef long (__cdecl* FdbSeekF)(int, long, int);
typedef int  (__cdecl* FdbWriteF)(int, const void*, unsigned);
typedef int  (__cdecl* FdbCloseF)(int);
typedef int  (__cdecl* FdbWOpenF)(const wchar_t*, int, int);
typedef int  (__cdecl* FdbOpenF)(const char*, int, int);
typedef INT_PTR (__cdecl* FdbOsfF)(int);
static FdbReadF fdb_o_read = NULL; static FdbSeekF fdb_o_lseek = NULL; static FdbWriteF fdb_o_write = NULL; static FdbCloseF fdb_o_close = NULL;
static FdbWOpenF fdb_o_wopen = NULL; static FdbOpenF fdb_o_open = NULL; static FdbOsfF fdb_o_osf = NULL;

struct FdbWin { BYTE* mem; LONG pos, len; DWORD used, grow; };          // file offset of the first byte, valid bytes; mem is FDB_WINMAX long
struct FdbFd {
    LONG state;                 // 0 never seen (opened before the hooks were in), 1 buffered, 2 opened through the hooks as a candidate, 3 left to msvcr71
    LONG pos, rpos, size;       // where the program is, where the real handle is, how long the file is
    DWORD clock;
    FdbWin win[FDB_NWIN];
};
static FdbFd g_fdb[FDB_MAXFD];
static CRITICAL_SECTION g_fdbCs[64];
static volatile LONG g_fdbOn = 0;
static BYTE* volatile g_fdbFree = NULL;                                 // spare window buffers (first pointer-size bytes link them)
static CRITICAL_SECTION g_fdbPoolCs;
static volatile LONG g_fdbNRead = 0, g_fdbNReal = 0, g_fdbNSeek = 0, g_fdbNSeekReal = 0, g_fdbNAdopt = 0, g_fdbNDirect = 0, g_fdbNRaw = 0, g_fdbNWin = 0, g_fdbNLate = 0;
static LONG g_fdbNPass = 0;                                             // reads left to msvcr71 (a plain count, not exact across threads)
// A handle opened before the hooks were in (the archives the engine opens at start) never passed through fdb_wopen, so
// its mode is not known from the open. msvcr71 keeps it in its handle table, which it exports (__pioinfo: 64 blocks of
// 32 entries {OS handle, flags, ...}); the layout assumed here is checked against two files opened for the purpose
// before anything relies on it.
static void** g_fdbPio = NULL;
#define FDB_PIO_SIZE 36
static __forceinline const BYTE* fdbPio(int fd) { const BYTE* blk = g_fdbPio ? (const BYTE*)g_fdbPio[fd >> 5] : NULL; return blk ? blk + (fd & 31) * FDB_PIO_SIZE : NULL; }
static bool fdbLooksBinaryFile(int fd) {                                // open, not text, not append, not a device, not a pipe
    const BYTE* e = fdbPio(fd);
    if (!e) return false;
    BYTE of = e[sizeof(INT_PTR)];
    return (of & 0x01) && !(of & (0x80 | 0x40 | 0x20 | 0x08));
}
static volatile LONG64 g_fdbBytes = 0;

typedef LONG (NTAPI* FdbNtQif)(HANDLE, void*, void*, ULONG, int);
static FdbNtQif fdb_NtQif = NULL;

static BYTE* fdbWinAlloc() {
    EnterCriticalSection(&g_fdbPoolCs);
    BYTE* p = g_fdbFree; if (p) g_fdbFree = *(BYTE**)p;
    LeaveCriticalSection(&g_fdbPoolCs);
    if (!p) { p = (BYTE*)VirtualAlloc(NULL, FDB_WINMAX, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); if (p) InterlockedIncrement(&g_fdbNWin); }
    return p;
}
static void fdbWinFree(BYTE* p) {
    if (!p) return;
    EnterCriticalSection(&g_fdbPoolCs); *(BYTE**)p = g_fdbFree; g_fdbFree = p; LeaveCriticalSection(&g_fdbPoolCs);
}
static void fdbDrop(FdbFd* f, LONG to) {                                // lock held: forget everything about this handle
    for (int i = 0; i < FDB_NWIN; ++i) { fdbWinFree(f->win[i].mem); f->win[i].mem = NULL; f->win[i].len = 0; f->win[i].pos = -1; f->win[i].grow = 0; }
    f->state = to;
}
// lock held, state 2: is it a plain disk file we may read ahead of? (binary and read-only were seen at the open)
static bool fdbAdopt(int fd, FdbFd* f) {
    f->state = 3;
    INT_PTR h = fdb_o_osf(fd);
    if (h == -1 || h == -2 || GetFileType((HANDLE)h) != FILE_TYPE_DISK) return false;
    if (fdb_NtQif) { struct { ULONG_PTR st; ULONG_PTR info; } iosb = { 0, 0 }; ULONG access = 0;
        if (fdb_NtQif((HANDLE)h, &iosb, &access, sizeof(access), 8) != 0 || (access & 0x6)) return false; }      // FileAccessInformation: no FILE_WRITE_DATA / FILE_APPEND_DATA
    long cur = fdb_o_lseek(fd, 0, 1); if (cur < 0) return false;
    long size = fdb_o_lseek(fd, 0, 2); if (size < 0) { fdb_o_lseek(fd, cur, 0); return false; }
    if (fdb_o_lseek(fd, cur, 0) != cur) return false;
    f->pos = f->rpos = cur; f->size = size; f->clock = 0;
    for (int i = 0; i < FDB_NWIN; ++i) { f->win[i].mem = NULL; f->win[i].len = 0; f->win[i].pos = -1; f->win[i].used = 0; f->win[i].grow = 0; }
    f->state = 1;
    InterlockedIncrement(&g_fdbNAdopt);
    return true;
}
static __forceinline void fdbSync(int fd, FdbFd* f) {                   // lock held: put the real handle where the program believes it is
    if (f->rpos != f->pos) { long r = fdb_o_lseek(fd, f->pos, 0); f->rpos = r < 0 ? -1 : r; InterlockedIncrement(&g_fdbNSeekReal); }
}
static BOOL fdbCopyOut(void* dst, const BYTE* src, DWORD n) { __try { memcpy(dst, src, n); return TRUE; } __except (EXCEPTION_EXECUTE_HANDLER) { return FALSE; } }

static int __cdecl fdb_read(int fd, void* buf, unsigned n) {
    if ((unsigned)fd >= FDB_MAXFD || g_fdb[fd].state == 3) { g_fdbNPass++; return fdb_o_read(fd, buf, n); }
    FdbFd* f = &g_fdb[fd]; CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
    EnterCriticalSection(cs);
    if (f->state == 0) { if (g_fdbOn && fdbLooksBinaryFile(fd) && fdbAdopt(fd, f)) InterlockedIncrement(&g_fdbNLate); else f->state = 3; }
    else if (f->state == 2) fdbAdopt(fd, f);
    if (f->state != 1) { LeaveCriticalSection(cs); g_fdbNPass++; return fdb_o_read(fd, buf, n); }
    if (!n || !buf || f->pos < 0 || n >= FDB_DIRECT || f->pos >= f->size) {
        // nothing to copy, a bad buffer, a large read or the end of the file: msvcr71 does it, from the right place
        fdbSync(fd, f);
        int r = fdb_o_read(fd, buf, n);
        if (r > 0) { f->pos += r; f->rpos = f->pos; if (f->pos > f->size) f->size = f->pos; InterlockedIncrement(&g_fdbNDirect); }
        else if (r < 0) { long c = fdb_o_lseek(fd, 0, 1); f->rpos = c; if (c >= 0) f->pos = c; }
        InterlockedIncrement(&g_fdbNReal);
        LeaveCriticalSection(cs);
        return r;
    }
    unsigned done = 0; BYTE* out = (BYTE*)buf;
    while (done < n) {
        LONG p = f->pos; FdbWin* w = NULL;
        for (int i = 0; i < FDB_NWIN; ++i) { FdbWin* c = &f->win[i]; if (c->mem && p >= c->pos && p < c->pos + c->len) { w = c; break; } }
        if (!w) {
            if (p >= f->size) break;                                    // the end of the file
            // a window for this place: the one that ended exactly here grows (read forwards), otherwise the least recently used starts small
            FdbWin* v = NULL; DWORD want = FDB_WINMIN;
            for (int i = 0; i < FDB_NWIN; ++i) { FdbWin* c = &f->win[i]; if (c->mem && c->len > 0 && c->pos + c->len == p) { v = c; want = c->grow >= FDB_WINMAX / 4 ? FDB_WINMAX : (c->grow ? c->grow * 4 : FDB_WINMIN * 4); break; } }
            if (!v) { for (int i = 0; i < FDB_NWIN; ++i) { FdbWin* c = &f->win[i]; if (!v || (!c->mem && v->mem) || (!!c->mem == !!v->mem && c->used < v->used)) v = c; } }
            if (!v->mem) { v->mem = fdbWinAlloc(); if (!v->mem) goto rest_direct; }
            if (want > FDB_WINMAX) want = FDB_WINMAX;
            if ((LONGLONG)p + want > f->size) want = (DWORD)(f->size - p);
            if (f->rpos != p) { long r = fdb_o_lseek(fd, p, 0); f->rpos = r < 0 ? -1 : r; InterlockedIncrement(&g_fdbNSeekReal); if (r != p) { v->len = 0; v->pos = -1; goto rest_direct; } }
            int got = fdb_o_read(fd, v->mem, want);
            InterlockedIncrement(&g_fdbNReal);
            if (got < 0) { v->len = 0; v->pos = -1; long c = fdb_o_lseek(fd, 0, 1); f->rpos = c; LeaveCriticalSection(cs); return done ? (int)done : -1; }
            if (got == 0) { v->len = 0; v->pos = -1; f->rpos = p; f->size = p; break; }
            f->rpos = p + got; v->pos = p; v->len = got; v->grow = want;
            if ((DWORD)got < want) f->size = p + got;                   // shorter than it said: that is the end
            w = v;
        }
        w->used = ++f->clock;
        DWORD off = (DWORD)(p - w->pos), take = (DWORD)w->len - off;
        if (take > n - done) take = n - done;
        if (!fdbCopyOut(out + done, w->mem + off, take)) { LeaveCriticalSection(cs); return done ? (int)done : -1; }
        done += take; f->pos += (LONG)take;
    }
    InterlockedIncrement(&g_fdbNRead); InterlockedExchangeAdd64(&g_fdbBytes, done);
    LeaveCriticalSection(cs);
    return (int)done;
rest_direct:                                                            // no window to be had: msvcr71 reads the rest itself
    {
        fdbSync(fd, f);
        int r = fdb_o_read(fd, out + done, n - done);
        InterlockedIncrement(&g_fdbNReal);
        if (r > 0) { f->pos += r; f->rpos = f->pos; done += (unsigned)r; }
        else { long c = fdb_o_lseek(fd, 0, 1); f->rpos = c; if (r < 0 && !done) { LeaveCriticalSection(cs); return -1; } }
        LeaveCriticalSection(cs);
        return (int)done;
    }
}
static long __cdecl fdb_lseek(int fd, long off, int whence) {
    if ((unsigned)fd >= FDB_MAXFD || g_fdb[fd].state != 1) return fdb_o_lseek(fd, off, whence);      // not read ahead of (yet): the real handle is the truth
    FdbFd* f = &g_fdb[fd]; CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
    EnterCriticalSection(cs);
    if (f->state != 1) { LeaveCriticalSection(cs); return fdb_o_lseek(fd, off, whence); }
    if ((unsigned)whence <= 2 && f->pos >= 0) {
        LONGLONG t = (LONGLONG)off + (whence == 0 ? 0 : whence == 1 ? f->pos : f->size);
        if (t >= 0 && t <= f->size) { f->pos = (LONG)t; InterlockedIncrement(&g_fdbNSeek); LeaveCriticalSection(cs); return (long)t; }   // inside the file: nothing to ask the system
    }
    fdbSync(fd, f);                                                     // outside it (or an origin msvcr71 will reject): its answer, from the right place
    long r = fdb_o_lseek(fd, off, whence);
    InterlockedIncrement(&g_fdbNSeekReal);
    if (r >= 0) { f->pos = f->rpos = r; }
    else { long c = fdb_o_lseek(fd, 0, 1); f->rpos = c; if (c >= 0) f->pos = c; }
    LeaveCriticalSection(cs);
    return r;
}
static void fdbGoRaw(int fd) {                                          // from here on msvcr71 alone: the real handle is where the program is
    if ((unsigned)fd >= FDB_MAXFD || g_fdb[fd].state == 3) return;
    FdbFd* f = &g_fdb[fd]; CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
    EnterCriticalSection(cs);
    if (f->state == 1) { fdbSync(fd, f); InterlockedIncrement(&g_fdbNRaw); }
    fdbDrop(f, 3);
    LeaveCriticalSection(cs);
}
static int __cdecl fdb_write(int fd, const void* buf, unsigned n) { fdbGoRaw(fd); return fdb_o_write(fd, buf, n); }
static INT_PTR __cdecl fdb_get_osfhandle(int fd) { fdbGoRaw(fd); return fdb_o_osf(fd); }
static int __cdecl fdb_close(int fd) {
    if ((unsigned)fd < FDB_MAXFD && g_fdb[fd].state) { CRITICAL_SECTION* cs = &g_fdbCs[fd & 63]; EnterCriticalSection(cs); fdbDrop(&g_fdb[fd], 3); LeaveCriticalSection(cs); }   // the number means nothing until the next open
    return fdb_o_close(fd);
}
// _O_RDONLY 0, _O_WRONLY 1, _O_RDWR 2, _O_APPEND 8, _O_TEMPORARY 0x40, _O_CREAT 0x100, _O_TRUNC 0x200, _O_EXCL 0x400, _O_TEXT 0x4000, _O_BINARY 0x8000
static void fdbOpened(int fd, int oflag) {
    if ((unsigned)fd >= FDB_MAXFD) return;
    CRITICAL_SECTION* cs = &g_fdbCs[fd & 63];
    EnterCriticalSection(cs);
    fdbDrop(&g_fdb[fd], 3);                                             // whatever this number meant before
    if (g_fdbOn && (oflag & 0x8000) && !(oflag & (0x1 | 0x2 | 0x8 | 0x40 | 0x100 | 0x200 | 0x4000))) g_fdb[fd].state = 2;
    LeaveCriticalSection(cs);
}
// (v58) every open is timed and told to whoever wants to know (aotr_filewarm.inc: which files the game opens, and how
// long the first open of each took - the virus scanner's look)
static void (*g_fdbNoteOpen)(const char* a, const wchar_t* w, int oflag, LONG64 ticks) = NULL;
static int __cdecl fdb_wopen(const wchar_t* name, int oflag, int pmode) {
    void (*note)(const char*, const wchar_t*, int, LONG64) = g_fdbNoteOpen;
    LARGE_INTEGER t0, t1; if (note) QueryPerformanceCounter(&t0);
    int fd = fdb_o_wopen(name, oflag, pmode);
    if (note && fd >= 0 && name) { QueryPerformanceCounter(&t1); note(NULL, name, oflag, t1.QuadPart - t0.QuadPart); }
    if (fd >= 0) fdbOpened(fd, oflag);
    return fd;
}
static int __cdecl fdb_open(const char* name, int oflag, int pmode) {
    void (*note)(const char*, const wchar_t*, int, LONG64) = g_fdbNoteOpen;
    LARGE_INTEGER t0, t1; if (note) QueryPerformanceCounter(&t0);
    int fd = fdb_o_open(name, oflag, pmode);
    if (note && fd >= 0 && name) { QueryPerformanceCounter(&t1); note(name, NULL, oflag, t1.QuadPart - t0.QuadPart); }
    if (fd >= 0) fdbOpened(fd, oflag);
    return fd;
}

static bool fdbInit(HMODULE crt) {
    static LONG done = 0;
    if (InterlockedExchange(&done, 1)) return fdb_o_read != NULL;
    for (int i = 0; i < 64; ++i) InitializeCriticalSection(&g_fdbCs[i]);
    InitializeCriticalSection(&g_fdbPoolCs);
    for (int i = 0; i < FDB_MAXFD; ++i) for (int k = 0; k < FDB_NWIN; ++k) g_fdb[i].win[k].pos = -1;
    FdbReadF r = (FdbReadF)GetProcAddress(crt, "_read"); FdbSeekF s = (FdbSeekF)GetProcAddress(crt, "_lseek"); FdbWriteF w = (FdbWriteF)GetProcAddress(crt, "_write");
    FdbCloseF c = (FdbCloseF)GetProcAddress(crt, "_close"); FdbWOpenF wo = (FdbWOpenF)GetProcAddress(crt, "_wopen"); FdbOpenF o = (FdbOpenF)GetProcAddress(crt, "_open");
    FdbOsfF h = (FdbOsfF)GetProcAddress(crt, "_get_osfhandle");
    if (!r || !s || !w || !c || !wo || !o || !h) return false;
    fdb_o_lseek = s; fdb_o_write = w; fdb_o_close = c; fdb_o_wopen = wo; fdb_o_open = o; fdb_o_osf = h;
    fdb_NtQif = (FdbNtQif)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationFile");
    fdb_o_read = r;
    // msvcr71's handle table, as far as it can be trusted: this DLL's own file opened in binary and in text mode
    void** pio = (void**)GetProcAddress(crt, "__pioinfo");
    wchar_t self[MAX_PATH];
    if (pio && GetModuleFileNameW(crt, self, MAX_PATH)) {
        int fb = wo(self, 0x8000, 0), ft = wo(self, 0x4000, 0);
        if (fb >= 0 && ft >= 0 && fb < FDB_MAXFD && ft < FDB_MAXFD) {
            g_fdbPio = pio;
            const BYTE* eb = fdbPio(fb); const BYTE* et = fdbPio(ft);
            bool ok = eb && et && *(const INT_PTR*)eb == h(fb) && *(const INT_PTR*)et == h(ft) && h(fb) != -1 && h(ft) != -1
                      && (eb[sizeof(INT_PTR)] & 0x81) == 0x01 && (et[sizeof(INT_PTR)] & 0x81) == 0x81 && fdbLooksBinaryFile(fb) && !fdbLooksBinaryFile(ft);
            if (!ok) g_fdbPio = NULL;
        }
        if (fb >= 0) c(fb);
        if (ft >= 0) c(ft);
    }
    return true;
}

// A short walk over one file through the layer and through msvcr71 side by side, where the layer runs (the game
// process): reads of 1..600 bytes and seeks from all three origins, every result and byte compared.
static bool fdbSelfTest(const wchar_t* path) {
    int a = fdb_wopen(path, 0x8000, 0), b = fdb_o_wopen(path, 0x8000, 0);
    bool ok = a >= 0 && b >= 0;
    static BYTE x[640], y[640]; unsigned seed = 12345;
    for (int i = 0; i < 600 && ok; ++i) {
        seed = seed * 1103515245u + 12345u; unsigned k = (seed >> 16) & 0x7FFF;
        if (k % 5 == 0) {
            int wh = (int)((k >> 3) % 3); long off = wh == 2 ? -(long)(k % 9000) : wh == 1 ? (long)(k % 4000) - 1500 : (long)((k * 37u) % 300000u) - 40;
            ok = fdb_lseek(a, off, wh) == fdb_o_lseek(b, off, wh);
        } else {
            unsigned n = 1 + k % 600; int ra = fdb_read(a, x, n), rb = fdb_o_read(b, y, n);
            ok = ra == rb && (ra <= 0 || memcmp(x, y, (size_t)ra) == 0);
        }
    }
    ok = ok && fdb_lseek(a, 0, 1) == fdb_o_lseek(b, 0, 1) && g_fdb[a].state == 1;
    if (a >= 0) fdb_close(a);
    if (b >= 0) fdb_o_close(b);
    return ok;
}
static void fdbAllRaw() { for (int fd = 0; fd < FDB_MAXFD; ++fd) fdbGoRaw(fd); }
