// lerp_test.cpp - the joint palette blend four joints at a time (twLerpPalette, SSE2) against the one-joint code it
// replaces (twLerpPaletteRef), through the DLL's AotrTweenTestLerp.
//   1. bit for bit: random palettes of every size from 1 to 100 joints at every alignment of the three buffers, with
//      the cases that make the code branch put in on purpose - a joint that moved too far, a quaternion that is not a
//      rotation, a rotation turned by more than the blend allows (both signs), a blend that comes out at nearly zero
//      length, NaN and infinity anywhere, denormals, and every kind of junk in the unused eighth word. The verdicts
//      must be the same; when the verdict is "blend", every byte of the output must be the same. Under the x87 / SSE
//      modes the game runs in (24-bit and 53-bit x87 precision; the SSE unit as the game leaves it).
//   2. timing: a battle's worth (3,500 palettes of 24 to 53 joints) both ways.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <math.h>

typedef int (__cdecl* LerpF)(const float*, const float*, int, float, float*, float*);
static unsigned g_seed = 20261008;
static unsigned rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }
static float frand() { return (float)(rnd() % 200001) / 100000.0f - 1.0f; }
static void quat(float* q) { float l; do { q[0] = frand(); q[1] = frand(); q[2] = frand(); q[3] = frand(); l = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]; } while (l < 0.05f); l = 1.0f / (float)sqrt(l); for (int k = 0; k < 4; ++k) q[k] *= l; }
static void joint(float* a, float* b, int kind) {
    quat(a);
    if ((rnd() & 3) == 0) quat(b);                                      // anywhere: often turned too far, either sign
    else { for (int k = 0; k < 4; ++k) b[k] = a[k] + frand() * 0.15f; float l = 0; for (int k = 0; k < 4; ++k) l += b[k]*b[k]; l = 1.0f / (float)sqrt(l); for (int k = 0; k < 4; ++k) b[k] *= l;
           if (rnd() & 1) for (int k = 0; k < 4; ++k) b[k] = -b[k]; }
    for (int k = 4; k < 7; ++k) { a[k] = frand() * 900.0f; b[k] = a[k] + frand() * 20.0f; }
    DWORD junk[8] = { 0, 1, 0x7FC00000u, 0xFFFFFFFFu, 0x00000300u, 0x7F800000u, rnd() * 2654435761u, 0x80000000u };
    ((DWORD*)a)[7] = junk[rnd() & 7]; ((DWORD*)b)[7] = junk[rnd() & 7];
    switch (kind) {
    case 1: b[4 + rnd() % 3] += (rnd() & 1) ? 61.0f : 59.0f; break;                          // around the limit of one frame's move
    case 2: { float sc = (rnd() & 1) ? 0.893f : 1.119f; for (int k = 0; k < 4; ++k) (rnd() & 1 ? a : b)[k] *= sc; } break;   // around the "is it a rotation" limits (0.8, 1.25 squared length)
    case 3: for (int k = 0; k < 4; ++k) b[k] = -a[k] * (1.0f + frand() * 1e-6f); break;      // opposite: the blend passes near zero only without the sign flip - stays a rotation
    case 4: ((DWORD*)(rnd() & 1 ? a : b))[rnd() % 7] = (rnd() & 1) ? 0x7FC00000u : (rnd() & 1 ? 0x7F800000u : 0xFF800000u); break;      // NaN / infinity somewhere
    case 5: ((DWORD*)(rnd() & 1 ? a : b))[4 + rnd() % 3] = rnd() & 0x7FFFFF; break;          // a denormal position
    case 6: { float d = 0.3f + frand() * 0.02f; float ang = (float)acos(d); float c = (float)cos(ang), s_ = (float)sin(ang);       // a turn right at the blend's limit (dot = 0.3)
              a[0] = 1; a[1] = a[2] = a[3] = 0; b[0] = c; b[1] = s_; b[2] = b[3] = 0; if (rnd() & 1) for (int k = 0; k < 4; ++k) b[k] = -b[k]; } break;
    }
}
int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE ha = LoadLibraryA("bfme2_accel.new.dll"); if (!ha) { printf("no bfme2_accel.new.dll (%lu)\n", GetLastError()); return 3; }
    LerpF lerp = (LerpF)GetProcAddress(ha, "AotrTweenTestLerp"); if (!lerp) { printf("missing AotrTweenTestLerp\n"); return 3; }
    Sleep(300);
    static BYTE bufA[4096 + 64], bufB[4096 + 64], bufO[4096 + 64], bufR[4096 + 64];
    long long cases = 0, joints = 0, blended = 0, left = 0, verdictDiff = 0, byteDiff = 0; long long kinds[8] = { 0 };
    static const unsigned kPc[2] = { _PC_24, _PC_53 };
    int rounds = argc > 1 ? atoi(argv[1]) : 400000;
    for (int pc = 0; pc < 2; ++pc) {
        _control87(kPc[pc], _MCW_PC);
        for (int r = 0; r < rounds; ++r) {
            int n = 1 + (int)(rnd() % 100);
            float* a = (float*)(bufA + (rnd() & 15)); float* b = (float*)(bufB + (rnd() & 15)); float* o = (float*)(bufO + (rnd() & 15)); float* ref = (float*)(bufR + (rnd() & 15));
            int special = (rnd() % 3) ? 0 : 1 + (int)(rnd() % 6), at = (int)(rnd() % n);     // two palettes in three are plain; the third has one special joint
            for (int j = 0; j < n; ++j) joint(a + j * 8, b + j * 8, j == at ? special : 0);
            if (special && (rnd() & 7) == 0) joint(a + (rnd() % n) * 8, b + (rnd() % n) * 8, 1 + (int)(rnd() % 6));       // sometimes a second one
            static const float kT[5] = { 0.5f, 0.75f, 0.25f, 0.0f, 1.0f }; float t = (rnd() & 3) ? kT[rnd() % 5] : (float)(rnd() % 1001) / 1000.0f;
            memset(o, 0xA5, n * 32); memset(ref, 0x5A, n * 32);
            int v = lerp(a, b, n, t, o, ref);
            ++cases; joints += n; ++kinds[special];
            if ((v & 1) != ((v >> 1) & 1)) { if (verdictDiff++ < 5) printf("VERDICTS DIFFER: %d joints, special %d at %d, t %g: four-at-a-time %d, one-joint %d\n", n, special, at, t, v & 1, (v >> 1) & 1); continue; }
            if (!(v & 1)) { ++left; continue; }
            ++blended;
            if (memcmp(o, ref, n * 32)) { if (byteDiff++ < 5) { int k = 0; while (((DWORD*)o)[k] == ((DWORD*)ref)[k]) ++k;
                printf("OUTPUT DIFFERS: %d joints, special %d at %d, t %g: word %d of joint %d: %08lX against %08lX\n", n, special, at, t, k & 7, k / 8, (unsigned long)((DWORD*)o)[k], (unsigned long)((DWORD*)ref)[k]); } }
        }
    }
    _control87(_PC_53, _MCW_PC);
    printf("%lld palettes (%lld joints) at 24-bit and 53-bit x87 precision: %lld blended, %lld left as they are; special joints: far %lld, not a rotation %lld, opposite %lld, NaN / infinity %lld, denormal %lld, at the turn limit %lld\n",
           cases, joints, blended, left, kinds[1], kinds[2], kinds[3], kinds[4], kinds[5], kinds[6]);
    printf("verdicts that differ: %lld; blended palettes whose bytes differ: %lld\n", verdictDiff, byteDiff);
    // timing: 3,500 palettes of 24..53 joints, as far apart in memory as a frame's are
    { const int N = 3500; static float* A; static float* B; static float* O; static int nj[3500];
      A = (float*)VirtualAlloc(NULL, N * 56 * 32, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); B = (float*)VirtualAlloc(NULL, N * 56 * 32, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); O = (float*)VirtualAlloc(NULL, 56 * 32 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
      for (int i = 0; i < N; ++i) { nj[i] = 24 + (i * 7) % 30; for (int j = 0; j < nj[i]; ++j) joint(A + (i * 56 + j) * 8, B + (i * 56 + j) * 8, 0); }
      typedef int (__cdecl* BlendF)(int, const float*, const float*, int, float, float*);
      LARGE_INTEGER qf, t0, t1; QueryPerformanceFrequency(&qf);
      double best[2] = { 1e9, 1e9 }; long long nJ = 0; for (int i = 0; i < N; ++i) nJ += nj[i];
      for (int rep = 0; rep < 30; ++rep) for (int which = 0; which < 2; ++which) {
          // which 0: both (four-at-a-time + one-joint); the one-joint code alone is timed by difference with a run of the new code alone through AotrTweenTestBlend
          BlendF blend = (BlendF)GetProcAddress(ha, "AotrTweenTestBlend");
          QueryPerformanceCounter(&t0);
          if (which == 0) for (int i = 0; i < N; ++i) lerp(A + i * 56 * 8, B + i * 56 * 8, nj[i], 0.5f, O, O + 56 * 8);
          else for (int i = 0; i < N; ++i) blend(0, A + i * 56 * 8, B + i * 56 * 8, nj[i], 0.5f, O);
          QueryPerformanceCounter(&t1);
          double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qf.QuadPart; if (ms < best[which]) best[which] = ms;
      }
      printf("timing, %d palettes / %lld joints: four at a time %.3f ms (%.1f ns a joint); one joint at a time %.3f ms (%.1f ns a joint)\n", N, nJ, best[1], best[1] * 1e6 / (double)nJ, best[0] - best[1], (best[0] - best[1]) * 1e6 / (double)nJ); }
    return (verdictDiff || byteDiff) ? 1 : 0;
}
