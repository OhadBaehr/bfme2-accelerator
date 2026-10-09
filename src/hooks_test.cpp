// hooks_test.cpp - the two engine-side changes of v54, run against stand-ins for the engine's functions (no game):
//   * exit fix: the two code stubs that replace "mov ecx,[esi+0x20] / test ecx,ecx" in the team code (result, zero
//     flag, every other register and xmm0 untouched), attack areas held back inside a reset and destroyed after it
//     in order, a destroyed area answered as "no area", a live one as the engine has it;
//   * steady scroll: the factor for a few frame rates, the same distance a second at each, the hook passing the
//     scaled offset on and leaving the caller's values where the engine keeps them.
#include <windows.h>
#include <stdio.h>
int main(int argc, char** argv) {
    HMODULE h = LoadLibraryA(argc > 1 ? argv[1] : "bfme2_accel.new.dll");
    if (!h) { printf("cannot load the DLL (%lu)\n", GetLastError()); return 2; }
    typedef int (__cdecl* TestF)(char*, int);
    TestF ef = (TestF)GetProcAddress(h, "AotrExitFixTest"), ss = (TestF)GetProcAddress(h, "AotrScrollTest");
    TestF ls = (TestF)GetProcAddress(h, "AotrLuaSortTest"), st = (TestF)GetProcAddress(h, "AotrStreamSetTest");      // v55: the script table sort, the streamer's sets
    TestF lf = (TestF)GetProcAddress(h, "AotrLogicFirstTest");                                                    // v55: no frame without logic that makes the next logic frame late
    if (!ef || !ss || !ls || !st || !lf) { printf("test exports missing\n"); return 3; }
    static char a[1024], b[1024], c[1024], d[1024], e[2400]; a[0] = b[0] = c[0] = d[0] = e[0] = 0;
    int f1 = ef(a, sizeof(a)), f2 = ss(b, sizeof(b)), f3 = ls(c, sizeof(c)), f4 = st(d, sizeof(d)), f5 = lf(e, sizeof(e));
    TestF fw = (TestF)GetProcAddress(h, "AotrFileWarmTest");                                                      // v58: the game's files opened ahead of it
    static char g[1024]; g[0] = 0; int f6 = fw ? fw(g, sizeof(g)) : 1;
    TestF pp = (TestF)GetProcAddress(h, "AotrPanWarpPathTest");                                                   // v59: where the camera is at a moment (pan pictures)
    static char k[1024]; k[0] = 0; int f7 = pp ? pp(k, sizeof(k)) : 1;
    printf("%s\n%s\n%s\n%s\nlogic first: %s\n%s\n%s\n%s\n", a, b, c, d, e, g, k, (f1 | f2 | f3 | f4 | f5 | f6 | f7) ? "FAILED" : "PASSED");
    return (f1 | f2 | f3 | f4 | f5 | f6 | f7) ? 1 : 0;
}
