// crt_probe: what msvcr71's remaining hot imports actually cost, and what a straightforward SSE replacement
// would cost. Nothing here is installed - it only decides which functions are worth replacing.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <emmintrin.h>
#include <smmintrin.h>
#include <intrin.h>

typedef double (__cdecl* tD1)(double);
typedef int    (__cdecl* tSCmp)(const char*, const char*);
typedef int    (__cdecl* tSNCmp)(const char*, const char*, size_t);
typedef char*  (__cdecl* tSCpy)(char*, const char*);
typedef char*  (__cdecl* tSNCpy)(char*, const char*, size_t);
typedef char*  (__cdecl* tSChr)(const char*, int);
typedef void*  (__cdecl* tMChr)(const void*, int, size_t);
typedef int    (__cdecl* tAtoi)(const char*);

static LARGE_INTEGER g_f;
static double ns(LONG64 d, int iters) { return (double)d * 1e9 / g_f.QuadPart / iters; }

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    QueryPerformanceFrequency(&g_f);
    HMODULE crt = LoadLibraryA("C:\\AgeoftheRing\\rotwk\\msvcr71.dll");
    if (!crt) { printf("cannot load msvcr71 (%lu)\n", GetLastError()); return 2; }
    static const char* kNames[] = { "floor", "ceil", "strcmp", "strncmp", "_mbscpy", "strncpy", "strchr", "memchr", "atoi", "sqrt", "abs" };
    printf("-- msvcr71 prologues --\n");
    for (int i = 0; i < (int)(sizeof(kNames) / sizeof(kNames[0])); ++i) {
        BYTE* p = (BYTE*)GetProcAddress(crt, kNames[i]);
        if (!p) { printf("%-9s missing\n", kNames[i]); continue; }
        printf("%-9s %08X ", kNames[i], (DWORD)(ULONG_PTR)p);
        for (int k = 0; k < 24; ++k) printf("%02X ", p[k]);
        printf("\n");
    }

    tD1 o_floor = (tD1)GetProcAddress(crt, "floor");
    tD1 o_ceil = (tD1)GetProcAddress(crt, "ceil");
    tSCmp o_strcmp = (tSCmp)GetProcAddress(crt, "strcmp");
    tSNCmp o_strncmp = (tSNCmp)GetProcAddress(crt, "strncmp");
    tSCpy o_mbscpy = (tSCpy)GetProcAddress(crt, "_mbscpy");
    tSNCpy o_strncpy = (tSNCpy)GetProcAddress(crt, "strncpy");
    tSChr o_strchr = (tSChr)GetProcAddress(crt, "strchr");
    tMChr o_memchr = (tMChr)GetProcAddress(crt, "memchr");
    tAtoi o_atoi = (tAtoi)GetProcAddress(crt, "atoi");

    // does msvcr71's floor depend on the x87 precision control? (the sqrt hazard)
    printf("\n-- floor / ceil under each x87 control word (value that needs 25 mantissa bits) --\n");
    static const WORD kCw[] = { 0x027F, 0x007F, 0x037F };
    double probe[] = { 16777217.5, 33554433.5, 4503599627370497.0, -16777217.5, 0.5, -0.5 };
    WORD cur = 0; __asm fnstcw cur
    for (int c = 0; c < 3; ++c) {
        WORD cw = kCw[c];
        printf("cw %04X:", cw);
        for (int i = 0; i < 6; ++i) {
            __asm fldcw cw
            double r = o_floor(probe[i]);
            __asm fldcw cur
            unsigned __int64 b; memcpy(&b, &r, 8);
            printf("  %.1f->%016I64X", probe[i], b);
        }
        printf("\n");
    }

    // timings against the obvious replacement
    const int N = 20000000;
    LARGE_INTEGER t0, t1;
    volatile double acc = 0;
    double xs[64]; for (int i = 0; i < 64; ++i) xs[i] = (double)(i * 7919 % 4001) / 7.0 - 285.0;
    QueryPerformanceCounter(&t0); for (int i = 0; i < N; ++i) acc += o_floor(xs[i & 63]); QueryPerformanceCounter(&t1);
    double a = ns(t1.QuadPart - t0.QuadPart, N);
    QueryPerformanceCounter(&t0); for (int i = 0; i < N; ++i) acc += _mm_cvtsd_f64(_mm_floor_sd(_mm_setzero_pd(), _mm_set_sd(xs[i & 63]))); QueryPerformanceCounter(&t1);
    double b = ns(t1.QuadPart - t0.QuadPart, N);
    printf("\nfloor    msvcr71 %5.2f ns   roundsd %5.2f ns\n", a, b);
    QueryPerformanceCounter(&t0); for (int i = 0; i < N; ++i) acc += o_ceil(xs[i & 63]); QueryPerformanceCounter(&t1);
    a = ns(t1.QuadPart - t0.QuadPart, N);
    QueryPerformanceCounter(&t0); for (int i = 0; i < N; ++i) acc += _mm_cvtsd_f64(_mm_ceil_sd(_mm_setzero_pd(), _mm_set_sd(xs[i & 63]))); QueryPerformanceCounter(&t1);
    b = ns(t1.QuadPart - t0.QuadPart, N);
    printf("ceil     msvcr71 %5.2f ns   roundsd %5.2f ns\n", a, b);

    const char* s1 = "GondorSoldierHorde";
    const char* s2 = "GondorSoldierHorse";
    char dst[64];
    volatile int sink = 0;
    QueryPerformanceCounter(&t0); for (int i = 0; i < N; ++i) sink += o_strcmp(s1, s2); QueryPerformanceCounter(&t1);
    printf("strcmp(18, differs at 17)  msvcr71 %5.2f ns\n", ns(t1.QuadPart - t0.QuadPart, N));
    QueryPerformanceCounter(&t0); for (int i = 0; i < N; ++i) sink += o_strcmp(s1, s1); QueryPerformanceCounter(&t1);
    printf("strcmp(18, equal)          msvcr71 %5.2f ns\n", ns(t1.QuadPart - t0.QuadPart, N));
    QueryPerformanceCounter(&t0); for (int i = 0; i < N; ++i) sink += o_strncmp(s1, s2, 18); QueryPerformanceCounter(&t1);
    printf("strncmp(18)                msvcr71 %5.2f ns\n", ns(t1.QuadPart - t0.QuadPart, N));
    QueryPerformanceCounter(&t0); for (int i = 0; i < N; ++i) o_mbscpy(dst, s1); QueryPerformanceCounter(&t1);
    printf("_mbscpy(18)                msvcr71 %5.2f ns\n", ns(t1.QuadPart - t0.QuadPart, N));
    QueryPerformanceCounter(&t0); for (int i = 0; i < N; ++i) o_strncpy(dst, s1, 32); QueryPerformanceCounter(&t1);
    printf("strncpy(18 into 32)        msvcr71 %5.2f ns\n", ns(t1.QuadPart - t0.QuadPart, N));
    QueryPerformanceCounter(&t0); for (int i = 0; i < N; ++i) sink += (int)(ULONG_PTR)o_strchr(s1, 'H'); QueryPerformanceCounter(&t1);
    printf("strchr(18)                 msvcr71 %5.2f ns\n", ns(t1.QuadPart - t0.QuadPart, N));
    QueryPerformanceCounter(&t0); for (int i = 0; i < N; ++i) sink += (int)(ULONG_PTR)o_memchr(s1, 'H', 18); QueryPerformanceCounter(&t1);
    printf("memchr(18)                 msvcr71 %5.2f ns\n", ns(t1.QuadPart - t0.QuadPart, N));
    QueryPerformanceCounter(&t0); for (int i = 0; i < N; ++i) sink += o_atoi("12345"); QueryPerformanceCounter(&t1);
    printf("atoi(5)                    msvcr71 %5.2f ns\n", ns(t1.QuadPart - t0.QuadPart, N));
    return (int)acc & (sink & 0);
}
