// Build/run in both reporting and AOTR_PROD modes with build_production_hotpath_test.bat.
// Include the full implementation so these checks exercise the shipping functions.
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cassert>
#include <cstdio>

static unsigned fixtureClockCalls;
static BOOL WINAPI fixtureClock(LARGE_INTEGER* value) {
    ++fixtureClockCalls;
    return QueryPerformanceCounter(value);
}
#define QueryPerformanceCounter fixtureClock
#include "aotr_accel.cpp"
#undef QueryPerformanceCounter

static unsigned interpreterCalls;
static double interpreterBias;
static int interpreterResult;
static int __stdcall fixtureInterpreter(DWORD, DWORD, DWORD, DWORD input, DWORD, DWORD output, DWORD,
                                        DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD) {
    ++interpreterCalls;
    if (input && input != 1 && output)
        *(double*)output = *(const double*)input + interpreterBias;
    return interpreterResult;
}

static int preshader(double* input, double* output, DWORD mask = 0) {
    return my_preshader(0x1234, 0, 0, (DWORD)input, 0, (DWORD)output, 0,
                       0, 0, mask, 0, 0, 0, 0);
}

static void testPreshader() {
    g_pc = (PreEnt*)VirtualAlloc(NULL, PRE_CACHE * sizeof(PreEnt), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(g_pc);
    o_preshader = fixtureInterpreter;
    interpreterBias = 2;
    double input = 4, output = -1;
    assert(preshader(&input, &output) == 0 && output == 6 && interpreterCalls == 1);
    for (unsigned hit = 1; hit <= 64; ++hit) {
        output = -1;
        assert(preshader(&input, &output) == 0 && output == 6);
        assert(interpreterCalls == 1 + hit / 32);
    }
    assert(g_cSkip == 64 && !g_cOff);

    // The next verification observes a changed interpreter and must disable reuse.
    interpreterBias = 3;
    for (unsigned hit = 1; hit <= 32; ++hit) {
        output = -1;
        assert(preshader(&input, &output) == 0);
        assert(output == (hit == 32 ? 7 : 6));
    }
    assert(g_cSkip == 96 && g_cOff && interpreterCalls == 4);
    assert(preshader(&input, &output) == 0 && output == 7 && interpreterCalls == 5);

    // Uncacheable and faulting cache inputs still return the original result.
    g_cOff = 0;
    interpreterResult = 17;
    assert(preshader(&input, &output, PRE_CAP) == 17 && output == 7);
    assert(preshader((double*)1, &output) == 17);
    PreEnt* cache = g_pc;
    g_pc = NULL;
    assert(preshader(&input, &output) == 17 && output == 7);
    g_preNoop = true;
    assert(preshader(&input, &output) == 0 && interpreterCalls == 8);
    g_preNoop = false;
    VirtualFree(cache, 0, MEM_RELEASE);

#ifdef AOTR_PROD
    assert(!g_preCount && !g_preTicks && !g_cHit && !g_cMiss && !g_cMismatch && !g_cNocache);
    assert(fixtureClockCalls == 0);
#else
    assert(g_preCount == 102 && g_cHit == 96 && g_cMiss == 1 && g_cMismatch == 1 && g_cNocache == 3);
    assert(fixtureClockCalls == 8);
#endif
}

static unsigned foreignReallocs, foreignFrees;
static void* foreignPointer;
static void* __cdecl fixtureRealloc(void* pointer, size_t size) {
    assert(pointer == foreignPointer && size == 48);
    ++foreignReallocs;
    return pointer;
}
static void __cdecl fixtureFree(void* pointer) {
    assert(pointer == foreignPointer);
    ++foreignFrees;
}

static void testAllocator() {
    assert(rpmalloc_initialize() == 0);
    unsigned char* bytes = (unsigned char*)my_malloc(32);
    assert(bytes && aOwns(bytes));
    memset(bytes, 0x5a, 32);
    bytes = (unsigned char*)my_realloc(bytes, 64);
    assert(bytes && aOwns(bytes));
    for (unsigned i = 0; i < 32; ++i) assert(bytes[i] == 0x5a);
    unsigned char* zeroes = (unsigned char*)my_calloc(4, 8);
    assert(zeroes && aOwns(zeroes));
    for (unsigned i = 0; i < 32; ++i) assert(zeroes[i] == 0);
    void* fromNull = my_realloc(NULL, 16);
    assert(fromNull && aOwns(fromNull));
    assert(!my_malloc((size_t)-1));
    assert(!my_calloc((size_t)-1, 2));
    assert(!my_realloc(bytes, (size_t)-1));
    assert(bytes[0] == 0x5a); // overflow rejection leaves the original allocation alive
    my_free(bytes); my_free(zeroes); my_free(fromNull); my_free(NULL);

    // Known foreign storage: the fixture's stock handlers own its lifetime.
    unsigned char foreign[AHDR + 32] = {};
    foreignPointer = foreign + AHDR;
    o_realloc = fixtureRealloc; o_free = fixtureFree;
    assert(my_realloc(foreignPointer, 48) == foreignPointer);
    my_free(foreignPointer);
    assert(foreignReallocs == 1 && foreignFrees == 1);
#ifdef AOTR_PROD
    assert(!g_aAllocs && !g_aFrees && !g_aForwarded);
#else
    assert(g_aAllocs == 3 && g_aFrees == 3 && g_aForwarded == 2);
#endif
    rpmalloc_thread_finalize(1);
    rpmalloc_finalize();
}

int main() {
    static_assert(sizeof(void*) == 4, "Use the x86 MSVC toolset");
    testPreshader();
    testAllocator();
#ifdef AOTR_PROD
    puts("PASS production: no report counters/timers; cache verification, fallback and allocation preserved");
#else
    puts("PASS reporting: counters/timers active; cache verification, fallback and allocation preserved");
#endif
}
