// edge_test: two edge cases of the accelerator's own code, through its test exports.
//   The allocator at its edges (size zero, sizes that cannot be) - with msvcr71's own answers printed first when the
//   game's copy can be loaded (argument: the path of msvcr71.dll; default the usual place of the game).
//   The effect value table when its 32-bit counter wraps.
#include <windows.h>
#include <stdio.h>
int main(int argc, char** argv) {
    int fail = 0;
    HMODULE crt = LoadLibraryA(argc > 1 ? argv[1] : "C:\\AgeoftheRing\\rotwk\\msvcr71.dll");
    if (crt) {
        typedef void* (__cdecl* MallocF)(size_t); typedef void* (__cdecl* ReallocF)(void*, size_t); typedef void (__cdecl* FreeF)(void*);
        MallocF m = (MallocF)GetProcAddress(crt, "malloc"); ReallocF r = (ReallocF)GetProcAddress(crt, "realloc"); FreeF f = (FreeF)GetProcAddress(crt, "free");
        if (!m || !r || !f) { printf("this msvcr71.dll has no malloc / realloc / free\n"); return 3; }
        void* block = m(64); void* gone = r(block, 0);
        void* m0 = m(0); void* r0 = r(NULL, 0);
        printf("msvcr71:\n      realloc(block, 0) = %s, malloc(0) = %s, realloc(NULL, 0) = %s\n", gone ? "a block" : "NULL", m0 ? "a block" : "NULL", r0 ? "a block" : "NULL");
        if (gone || !m0 || !r0) { printf("FAIL  msvcr71 does not answer as the checks below assume\n"); fail = 1; }
        if (gone) f(gone); if (m0) f(m0); if (r0) f(r0);
    } else printf("(msvcr71.dll not found - its answers are not shown; give its path as the argument)\n");
    HMODULE h = LoadLibraryA("bfme2_accel.new.dll"); if (!h) { printf("no bfme2_accel.new.dll (%lu)\n", GetLastError()); return 3; }
    typedef int (__cdecl* TestF)(char*, int);
    TestF at = (TestF)GetProcAddress(h, "AotrAllocTest"), fw = (TestF)GetProcAddress(h, "AotrRtTestFxWrap");
    if (!at || !fw) { printf("missing test exports\n"); return 3; }
    Sleep(500);                                              // the DLL's own start-up thread: it finds no game here and stops
    static char out[4096];
    out[0] = 0; fail += at(out, sizeof(out)); printf("accelerator:\n%s", out);
    out[0] = 0; fail += fw(out, sizeof(out)); printf("%s\n", out);
    printf("%s\n", fail ? "FAILED" : "edge cases: as they must be");
    return fail ? 1 : 0;
}
