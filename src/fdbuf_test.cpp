// fdbuf_test.cpp - offline proof and measurement for the read-ahead layer (aotr_fdbuf.inc).
//
// The same random sequence of reads, seeks, skips and end-of-file reads is run twice on two handles of the same
// file: through the layer, and through msvcr71's own _read / _lseek. Every return value, every byte and the file
// position after every step must be the same. Then the files the layer must leave alone (text mode, opened for
// writing, a handle whose OS handle was asked for), several threads, and how long a W3D-style parse (millions of
// tiny reads) takes each way.
//
//   fdbuf_test [steps] [file]
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define FDB_LOG(...) (printf(__VA_ARGS__), printf("\n"))
#include "aotr_fdbuf.inc"

static LARGE_INTEGER g_qpf;
static double ms(LONGLONG t) { return (double)t * 1000.0 / (double)g_qpf.QuadPart; }
static LONGLONG now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
struct Rng { unsigned long long s; unsigned next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return (unsigned)(s >> 33); } unsigned upto(unsigned n) { return n ? next() % n : 0; } };

static int g_bad = 0;
static void fail(const char* what, long step, long a, long b) { if (g_bad++ < 12) printf("   DIFFERENT: %s at step %ld: layer %ld, msvcr71 %ld\n", what, step, a, b); }

// one random walk over `name` opened with `oflag`; returns steps done
static bool g_openBehind = false;                              // open the layer's handle behind its back, as a file opened before the hooks were in
static long walk(const wchar_t* name, int oflag, long steps, unsigned long long seed, bool osfMid) {
    int a = g_openBehind ? fdb_o_wopen(name, oflag, 0) : fdb_wopen(name, oflag, 0), b = fdb_o_wopen(name, oflag, 0);
    if (g_openBehind && a >= 0 && a < FDB_MAXFD) g_fdb[a].state = 0;
    if (a < 0 || b < 0) { printf("   cannot open\n"); g_bad++; return 0; }
    long size = fdb_o_lseek(b, 0, 2); fdb_o_lseek(b, 0, 0);
    Rng r = { seed };
    std::vector<BYTE> ba(400000), bb(400000);
    for (long i = 0; i < steps; ++i) {
        unsigned k = r.upto(100);
        if (osfMid && i == steps / 2) { INT_PTR h1 = fdb_get_osfhandle(a); (void)h1; }
        if (k < 55) {                                              // small read
            unsigned n = 1 + r.upto(k < 40 ? 40 : 600);
            memset(&ba[0], 0xA5, n); memset(&bb[0], 0xA5, n);
            int ra = fdb_read(a, &ba[0], n), rb = fdb_o_read(b, &bb[0], n);
            if (ra != rb) fail("read result", i, ra, rb); else if (ra > 0 && memcmp(&ba[0], &bb[0], ra)) fail("read bytes", i, ra, rb);
        } else if (k < 62) {                                       // medium / large read
            unsigned n = k < 59 ? 601 + r.upto(40000) : 30000 + r.upto(300000);
            int ra = fdb_read(a, &ba[0], n), rb = fdb_o_read(b, &bb[0], n);
            if (ra != rb) fail("big read result", i, ra, rb); else if (ra > 0 && memcmp(&ba[0], &bb[0], ra)) fail("big read bytes", i, ra, rb);
        } else if (k < 64) {                                       // nothing to read / nowhere to put it
            int ra = fdb_read(a, &ba[0], 0), rb = fdb_o_read(b, &bb[0], 0);
            if (ra != rb) fail("zero read", i, ra, rb);
        } else if (k < 80) {                                       // seek from the start: inside, and sometimes outside
            long off = (long)r.upto((unsigned)size + 1); unsigned e = r.upto(40);
            if (e == 0) off = -(long)r.upto(50) - 1; else if (e == 1) off = size + (long)r.upto(200); else if (e < 6) off = size - (long)r.upto(size < 300 ? (unsigned)size + 1 : 300);
            long ra = fdb_lseek(a, off, 0), rb = fdb_o_lseek(b, off, 0);
            if (ra != rb) fail("seek set", i, ra, rb);
        } else if (k < 92) {                                       // relative: mostly short hops forwards (a skipped chunk)
            long off = (long)r.upto(3000) - (r.upto(4) == 0 ? 2500 : 200); if (r.upto(60) == 0) off = (long)r.upto(0x7FFFFFFF) - 0x40000000;
            long ra = fdb_lseek(a, off, 1), rb = fdb_o_lseek(b, off, 1);
            if (ra != rb) fail("seek cur", i, ra, rb);
        } else if (k < 96) {
            long off = -(long)r.upto(size < 5000 ? (unsigned)size + 30 : 5000) + (r.upto(10) == 0 ? 40 : 0);
            long ra = fdb_lseek(a, off, 2), rb = fdb_o_lseek(b, off, 2);
            if (ra != rb) fail("seek end", i, ra, rb);
        } else {                                                   // where are we?
            long ra = fdb_lseek(a, 0, 1), rb = fdb_o_lseek(b, 0, 1);
            if (ra != rb) fail("position", i, ra, rb);
        }
        if ((i & 63) == 0) { long ra = fdb_lseek(a, 0, 1), rb = fdb_o_lseek(b, 0, 1); if (ra != rb) fail("position check", i, ra, rb); }
    }
    int ca = fdb_close(a), cb = fdb_o_close(b);
    if (ca != cb) fail("close", steps, ca, cb);
    return steps;
}
struct ThreadArg { const wchar_t* name; long steps; unsigned long long seed; };
static DWORD WINAPI walker(LPVOID p) { ThreadArg* t = (ThreadArg*)p; walk(t->name, 0x8000, t->steps, t->seed, false); return 0; }
// two threads on ONE handle, each doing seek + read pairs under the program's own lock, as an archive's streams would
static int g_shared = -1; static CRITICAL_SECTION g_sharedCs; static const wchar_t* g_sharedName;
static DWORD WINAPI sharer(LPVOID p) {
    Rng r = { (unsigned long long)(ULONG_PTR)p * 977 + 5 };
    int b = fdb_o_wopen(g_sharedName, 0x8000, 0); long size = fdb_o_lseek(b, 0, 2);
    long at = (long)r.upto((unsigned)size);
    BYTE ba[512], bb[512];
    for (int i = 0; i < 200000; ++i) {
        unsigned n = 1 + r.upto(60);
        if (r.upto(50) == 0) at = (long)r.upto((unsigned)size);
        EnterCriticalSection(&g_sharedCs);
        long sa = fdb_lseek(g_shared, at, 0); int ra = fdb_read(g_shared, ba, n);
        LeaveCriticalSection(&g_sharedCs);
        long sb = fdb_o_lseek(b, at, 0); int rb = fdb_o_read(b, bb, n);
        if (sa != sb || ra != rb || (ra > 0 && memcmp(ba, bb, ra))) fail("shared handle", i, ra, rb);
        if (ra > 0) at += ra;
    }
    fdb_o_close(b);
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    QueryPerformanceFrequency(&g_qpf);
    long steps = argc > 1 ? atol(argv[1]) : 400000;
    HMODULE crt = LoadLibraryA("C:\\AgeoftheRing\\rotwk\\msvcr71.dll");
    if (!crt || !fdbInit(crt)) { printf("msvcr71.dll or its file functions missing\n"); return 3; }
    InterlockedExchange(&g_fdbOn, 1);
    wchar_t big[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, argc > 2 ? argv[2] : "C:\\AgeoftheRing\\rotwk\\W3D.big", -1, big, MAX_PATH);
    const int BIN = 0x8000;

    { wchar_t self[MAX_PATH]; GetModuleFileNameW(crt, self, MAX_PATH); bool ok = fdbSelfTest(self); printf("the self-test the DLL runs at install: %s\n", ok ? "passes" : "FAILS"); if (!ok) g_bad++; }
    printf("random walk, read-only binary, %ld steps on the archive:\n", steps);
    for (int k = 0; k < 4; ++k) walk(big, BIN, steps / 4, 1000 + k * 77, false);
    printf("   %d reads from windows, %d real reads, %d seeks answered here, %d real seeks, %d windows of 64 KB\n", (int)g_fdbNRead, (int)g_fdbNReal, (int)g_fdbNSeek, (int)g_fdbNSeekReal, (int)g_fdbNWin);

    // small files: shorter than a window, shorter than a read, empty
    wchar_t tmp[MAX_PATH], small_[MAX_PATH], empty[MAX_PATH], text[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wsprintfW(small_, L"%sfdbuf_small.bin", tmp); wsprintfW(empty, L"%sfdbuf_empty.bin", tmp); wsprintfW(text, L"%sfdbuf_text.txt", tmp);
    { HANDLE h = CreateFileW(small_, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); BYTE b[9000]; for (int i = 0; i < 9000; ++i) b[i] = (BYTE)(i * 31 + (i >> 7)); DWORD w; WriteFile(h, b, 9000, &w, NULL); CloseHandle(h);
      h = CreateFileW(empty, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); CloseHandle(h);
      h = CreateFileW(text, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); const char* t = "line one\r\nline two\r\n\r\nthree\x1A" "after eof mark\r\n"; for (int i = 0; i < 200; ++i) WriteFile(h, t, (DWORD)strlen(t), &w, NULL); CloseHandle(h); }
    printf("9000-byte file, empty file:\n"); walk(small_, BIN, 60000, 31, false); walk(empty, BIN, 5000, 32, false);
    LONG adoptBefore = g_fdbNAdopt;
    printf("text mode (left to msvcr71), read-write (left to msvcr71), OS handle asked half way (left to msvcr71 from then on):\n");
    walk(text, 0x4000, 30000, 33, false); walk(text, 0, 30000, 34, false);
    if (g_fdbNAdopt != adoptBefore) { printf("   WRONG: a text-mode file was given read-ahead\n"); g_bad++; }
    walk(small_, BIN | 2, 30000, 35, false);
    if (g_fdbNAdopt != adoptBefore) { printf("   WRONG: a file open for writing was given read-ahead\n"); g_bad++; }
    walk(small_, BIN, 30000, 36, true);
    // written through the layer: the write goes to msvcr71 and what is read back is what was written
    { int a = fdb_wopen(small_, BIN | 2, 0); BYTE x[16] = { 9, 8, 7, 6, 5, 4, 3, 2, 1 }, y[16] = { 0 };
      fdb_lseek(a, 100, 0); fdb_write(a, x, 9); fdb_lseek(a, 100, 0); int r = fdb_read(a, y, 9); fdb_close(a);
      if (r != 9 || memcmp(x, y, 9)) { printf("   WRONG: write then read back\n"); g_bad++; } }

    printf("handles opened before the layer saw them (msvcr71's handle table %s):\n", g_fdbPio ? "understood" : "NOT understood - such files stay as they are");
    { g_openBehind = true; LONG late0 = g_fdbNLate;
      walk(big, BIN, steps / 4, 71, false); walk(small_, BIN, 30000, 72, false);
      LONG lateBin = g_fdbNLate - late0;
      walk(text, 0x4000, 30000, 73, false); walk(small_, BIN | 2, 30000, 74, false);
      if (g_fdbPio && lateBin != 2) { printf("   WRONG: %d of 2 read-only binary files were taken up\n", (int)lateBin); g_bad++; }
      if (g_fdbNLate - late0 != lateBin) { printf("   WRONG: a text-mode or writable file was taken up\n"); g_bad++; }
      g_openBehind = false; }
    printf("four threads, a handle each:\n");
    { HANDLE th[4]; ThreadArg ta[4]; for (int i = 0; i < 4; ++i) { ta[i].name = big; ta[i].steps = steps / 4; ta[i].seed = 500 + i; th[i] = CreateThread(NULL, 0, walker, &ta[i], 0, NULL); }
      WaitForMultipleObjects(4, th, TRUE, INFINITE); }
    printf("two threads on one handle (seek + read under the program's lock):\n");
    { InitializeCriticalSection(&g_sharedCs); g_sharedName = big; g_shared = fdb_wopen(big, BIN, 0);
      HANDLE th[2]; for (int i = 0; i < 2; ++i) th[i] = CreateThread(NULL, 0, sharer, (LPVOID)(ULONG_PTR)(i + 1), 0, NULL);
      WaitForMultipleObjects(2, th, TRUE, INFINITE); fdb_close(g_shared); }

    // a W3D-style parse: chunk header (8 bytes), then the chunk in small pieces, forwards, with a skip now and then
    printf("W3D-style parse of 96 MB (8-byte headers, small fields, skips):\n");
    for (int pass = 0; pass < 2; ++pass) {
        int fd = pass ? fdb_wopen(big, BIN, 0) : fdb_o_wopen(big, BIN, 0);
        Rng r = { 4242 }; BYTE buf[4096]; long reads = 0; unsigned long long sum = 0; long pos = 0;
        LONGLONG t0 = now();
        while (pos < (96 << 20)) {
            int g = pass ? fdb_read(fd, buf, 8) : fdb_o_read(fd, buf, 8); if (g != 8) break; pos += 8; ++reads; sum += buf[3];
            unsigned len = 16 + r.upto(700);
            if (r.upto(12) == 0) { if (pass) fdb_lseek(fd, (long)len, 1); else fdb_o_lseek(fd, (long)len, 1); pos += len; continue; }
            while (len) { unsigned n = 4 + r.upto(60); if (n > len) n = len; int q = pass ? fdb_read(fd, buf, n) : fdb_o_read(fd, buf, n); if (q != (int)n) { len = 0; pos = 0x7FFFFFFF; break; } sum += buf[n - 1]; len -= n; pos += n; ++reads; }
        }
        LONGLONG t1 = now();
        printf("   %s: %ld reads in %.1f ms (%.0f ns each), checksum %llu\n", pass ? "through the layer" : "msvcr71 alone    ", reads, ms(t1 - t0), ms(t1 - t0) * 1e6 / reads, sum);
        if (pass) fdb_close(fd); else fdb_o_close(fd);
        static unsigned long long first; if (!pass) first = sum; else if (sum != first) { printf("   WRONG: the two parses read different bytes\n"); g_bad++; }
    }
    DeleteFileW(small_); DeleteFileW(empty); DeleteFileW(text);
    printf("\n%s (%d differences)\n", g_bad ? "FAILED" : "PASSED", g_bad);
    return g_bad ? 1 : 0;
}
