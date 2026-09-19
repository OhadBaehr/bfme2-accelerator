// crt_test: checks the fast CRT replacements in aotr_fastcrt.inc against msvcr71.dll bit for bit
// (return value, argument slot, x87 control word afterwards), under the x87 control words the game runs with.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
static void logf(const char* fmt, ...) { (void)fmt; }
#include "aotr_fastcrt.inc"

static unsigned __int64 g_rng = 0x9E3779B97F4A7C15ull;
static unsigned __int64 rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return g_rng; }

// call fn(x) the way game code does: push the double, call, fstp the result, then read the argument slot
static void __cdecl callD1(tCrtD1 fn, const double* x, double* res, double* slot) {
    __asm {
        mov eax, x
        sub esp, 8
        movsd xmm0, qword ptr [eax]
        movsd qword ptr [esp], xmm0
        call dword ptr [fn]
        mov eax, res
        fstp qword ptr [eax]
        movsd xmm0, qword ptr [esp]
        mov eax, slot
        movsd qword ptr [eax], xmm0
        add esp, 8
    }
}
static int g_fail = 0;
static void checkD1(const char* name, tCrtD1 orig, tCrtD1 fast, double x, WORD cw) {
    WORD cur = 0; __asm fnstcw cur
    WORD want = cw;
    __asm fldcw want
    double r1, s1, r2, s2;
    callD1(orig, &x, &r1, &s1);
    WORD cw1 = 0; __asm fnstcw cw1
    __asm fldcw want
    callD1(fast, &x, &r2, &s2);
    WORD cw2 = 0; __asm fnstcw cw2
    __asm fldcw cur
    bool finite = crtFinite(&x);
    bool slotMatters = finite;                     // a forwarded call leaves the original's own slot behaviour inside our frame
    if (memcmp(&r1, &r2, 8) || cw1 != cw2 || (slotMatters && memcmp(&s1, &s2, 8))) {
        if (g_fail++ < 20) {
            unsigned __int64 xb, a, b, c, d; memcpy(&xb, &x, 8); memcpy(&a, &r1, 8); memcpy(&b, &r2, 8); memcpy(&c, &s1, 8); memcpy(&d, &s2, 8);
            printf("MISMATCH %s cw=%04X x=%016I64X: orig %016I64X slot %016I64X cw %04X | fast %016I64X slot %016I64X cw %04X\n", name, cw, xb, a, c, cw1, b, d, cw2);
        }
    }
}
static double mk(unsigned __int64 bits) { double d; memcpy(&d, &bits, 8); return d; }

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE crt = LoadLibraryA("C:\\AgeoftheRing\\rotwk\\msvcr71.dll");
    if (!crt) { printf("cannot load msvcr71.dll (%lu)\n", GetLastError()); return 2; }
    if (!crtPrepare(crt)) { printf("crtPrepare failed\n"); return 3; }
    printf("msvcr71 at %p\n", crt);
    static const WORD kCw[] = { 0x027F, 0x007F, 0x037F, 0x0E7F, 0x0A7F, 0x067F, 0x027B };
    static const unsigned __int64 kSpecial[] = {
        0, 0x8000000000000000ull, 1, 0x8000000000000001ull, 0x000FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull,
        0x0010000000000000ull, 0x8010000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0xFFEFFFFFFFFFFFFFull,
        0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000000ull,
        0x7FF0000000000001ull, 0xFFF0000000000001ull, 0x7FF4000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull,
        0x3FE0000000000000ull, 0xBFE0000000000000ull, 0x4330000000000000ull, 0xC330000000000000ull, 0x432FFFFFFFFFFFFFull,
        0xC32FFFFFFFFFFFFFull, 0x4340000000000000ull, 0x3FEFFFFFFFFFFFFFull, 0xBFEFFFFFFFFFFFFFull, 0x3CB0000000000000ull };
    long n = 0;
    for (int c = 0; c < (int)(sizeof(kCw) / sizeof(kCw[0])); ++c) {
        WORD cw = kCw[c];
        for (int i = 0; i < (int)(sizeof(kSpecial) / sizeof(kSpecial[0])); ++i) {
            double x = mk(kSpecial[i]);
            checkD1("fabs", o_crtFabs, fastFabs, x, cw); n += 1;
        }
        int iters = c < 2 ? 3000000 : 200000;
        for (int i = 0; i < iters; ++i) {
            unsigned __int64 r = rnd();
            double x;
            switch (i & 7) {
            case 0: x = mk(r); break;                                                         // any bit pattern
            case 1: x = mk((r & 0x800FFFFFFFFFFFFFull) | ((0x3F0ull + (r >> 52) % 0x60) << 52)); break;   // near 1..2^52
            case 2: x = (double)(__int64)(r % 2000001) - 1000000.0; break;                   // integers
            case 3: x = ((double)(__int64)(r % 2000001) - 1000000.0) + 0.5; break;           // halves
            case 4: { double b = (double)(__int64)(r % 200001) - 100000.0; x = _nextafter(b, (r >> 40) & 1 ? 1e300 : -1e300); } break;   // just off integers
            case 5: x = (double)(float)((double)(__int64)(r % 4000001) / 1000.0 - 2000.0); break;   // float-derived, like game math
            case 6: x = mk(r & 0x800FFFFFFFFFFFFFull); break;                                // denormals
            default: x = mk((r & 0x800FFFFFFFFFFFFFull) | (((0x3FFull + 52 - 8) + (r >> 56) % 16) << 52)); break;   // 2^44..2^60
            }
            checkD1("fabs", o_crtFabs, fastFabs, x, cw); n += 1;
        }
    }
    printf("math: %ld comparisons, %d mismatches\n", n, g_fail);
    // memcmp: random buffers, lengths 0..96, one or two differences, all alignments
    static BYTE bufA[512], bufB[512];
    long m = 0; int mf = 0;
    for (int it = 0; it < 4000000; ++it) {
        unsigned __int64 r = rnd();
        int len = (int)(r % 97), oa = (int)((r >> 8) % 4), ob = (int)((r >> 12) % 4);
        for (int k = 0; k < len; ++k) { bufA[oa + k] = (BYTE)rnd(); }
        memcpy(bufB + ob, bufA + oa, len);
        int mode = (int)((r >> 16) % 4);
        if (len && mode) { int p = (int)((r >> 20) % len); bufB[ob + p] = (BYTE)(bufB[ob + p] + 1 + (r >> 28) % 255); if (mode == 3) { int q = (int)((r >> 36) % len); bufB[ob + q] ^= 0x80; } }
        int x1 = o_crtMemcmp(bufA + oa, bufB + ob, len), x2 = fastMemcmp(bufA + oa, bufB + ob, len);
        ++m;
        if (x1 != x2 && mf++ < 10) printf("MISMATCH memcmp len %d: orig %d fast %d\n", len, x1, x2);
    }
    printf("memcmp: %ld comparisons, %d mismatches\n", m, mf);
    // speed
    volatile double acc = 0; double xs[64]; for (int i = 0; i < 64; ++i) xs[i] = ((double)(__int64)(rnd() % 20001) - 10000.0) / 7.0;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f);
    struct { const char* nm; tCrtD1 a, b; } sp[] = { {"fabs", o_crtFabs, fastFabs} };
    for (int s = 0; s < 1; ++s) {
        QueryPerformanceCounter(&t0); for (int i = 0; i < 20000000; ++i) acc += sp[s].a(xs[i & 63]); QueryPerformanceCounter(&t1);
        double ta = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / 20000000;
        QueryPerformanceCounter(&t0); for (int i = 0; i < 20000000; ++i) acc += sp[s].b(xs[i & 63]); QueryPerformanceCounter(&t1);
        double tb = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / 20000000;
        printf("speed %-6s msvcr71 %.2f ns  fast %.2f ns\n", sp[s].nm, ta, tb);
    }
    for (int k = 0; k < 64; ++k) bufA[k] = bufB[k] = (BYTE)k;
    volatile int ai = 0;
    for (int len = 8; len <= 32; len += 12) {
        QueryPerformanceCounter(&t0); for (int i = 0; i < 20000000; ++i) ai += o_crtMemcmp(bufA, bufB, len); QueryPerformanceCounter(&t1);
        double ta = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / 20000000;
        QueryPerformanceCounter(&t0); for (int i = 0; i < 20000000; ++i) ai += fastMemcmp(bufA, bufB, len); QueryPerformanceCounter(&t1);
        double tb = (double)(t1.QuadPart - t0.QuadPart) * 1e9 / f.QuadPart / 20000000;
        printf("speed memcmp(%d) msvcr71 %.2f ns  fast %.2f ns\n", len, ta, tb);
    }
    return (g_fail || mf) ? 1 : 0;
}
