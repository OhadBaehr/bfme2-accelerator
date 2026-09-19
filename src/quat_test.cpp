// Offline check: can the engine's quaternion (written per draw as fx.SetRawValue) be reproduced bit-exactly from the
// mesh transform? Input: quatpairs.txt, one line per captured draw - 12 hex words of the 3x4 transform (rows of 4)
// followed by the 4 hex words of the quaternion the engine wrote (x, y, z, w).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef unsigned int u32;
static u32 bswap(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }   // the file holds raw bytes, most significant first
static float fw(u32 w) { w = bswap(w); float f; memcpy(&f, &w, 4); return f; }
static u32 wf(float f) { u32 w; memcpy(&w, &f, 4); return w; }

// Several published forms of the same conversion; the engine's is whichever reproduces every captured value exactly.
static void quatA(const float* m, float* q) {              // sqrt first, then one divide
    const float m00=m[0],m01=m[1],m02=m[2],m10=m[4],m11=m[5],m12=m[6],m20=m[8],m21=m[9],m22=m[10];
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f); q[3] = s * 0.5f; s = 0.5f / s;
        q[0] = (m21 - m12) * s; q[1] = (m02 - m20) * s; q[2] = (m10 - m01) * s;
    } else {
        int i = 0; if (m11 > m00) i = 1; if (m22 > (i == 0 ? m00 : m11)) i = 2;
        const float M[3][3] = {{m00,m01,m02},{m10,m11,m12},{m20,m21,m22}};
        int j = (i + 1) % 3, k = (i + 2) % 3;
        float s = sqrtf(M[i][i] - M[j][j] - M[k][k] + 1.0f);
        q[i] = s * 0.5f; s = 0.5f / s;
        q[3] = (M[k][j] - M[j][k]) * s; q[j] = (M[j][i] + M[i][j]) * s; q[k] = (M[k][i] + M[i][k]) * s;
    }
}
static void quatB(const float* m, float* q) {              // trace + 1 compared against 1, scale = 0.5/sqrt, w = 0.25/scale
    const float m00=m[0],m01=m[1],m02=m[2],m10=m[4],m11=m[5],m12=m[6],m20=m[8],m21=m[9],m22=m[10];
    float tr = m00 + m11 + m22 + 1.0f;
    if (tr > 1.0f) {
        float s = 0.5f / sqrtf(tr);
        q[3] = 0.25f / s; q[0] = (m21 - m12) * s; q[1] = (m02 - m20) * s; q[2] = (m10 - m01) * s;
    } else {
        int i = 0; if (m11 > m00) i = 1; if (m22 > (i == 0 ? m00 : m11)) i = 2;
        const float M[3][3] = {{m00,m01,m02},{m10,m11,m12},{m20,m21,m22}};
        int j = (i + 1) % 3, k = (i + 2) % 3;
        float s = sqrtf(M[i][i] - M[j][j] - M[k][k] + 1.0f);
        q[i] = s * 0.5f; s = 0.5f / s;
        q[3] = (M[k][j] - M[j][k]) * s; q[j] = (M[j][i] + M[i][j]) * s; q[k] = (M[k][i] + M[i][k]) * s;
    }
}
static void quatC(const float* m, float* q) {              // like A, but the reciprocal is formed once and multiplied
    const float m00=m[0],m01=m[1],m02=m[2],m10=m[4],m11=m[5],m12=m[6],m20=m[8],m21=m[9],m22=m[10];
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float r = sqrtf(tr + 1.0f); float inv = 1.0f / r; q[3] = r * 0.5f;
        q[0] = (m21 - m12) * 0.5f * inv; q[1] = (m02 - m20) * 0.5f * inv; q[2] = (m10 - m01) * 0.5f * inv;
    } else {
        int i = 0; if (m11 >= m00) i = 1; if (m22 >= (i == 0 ? m00 : m11)) i = 2;
        const float M[3][3] = {{m00,m01,m02},{m10,m11,m12},{m20,m21,m22}};
        int j = (i + 1) % 3, k = (i + 2) % 3;
        float s = sqrtf(M[i][i] - M[j][j] - M[k][k] + 1.0f);
        q[i] = s * 0.5f; s = 0.5f / s;
        q[3] = (M[k][j] - M[j][k]) * s; q[j] = (M[j][i] + M[i][j]) * s; q[k] = (M[k][i] + M[i][k]) * s;
    }
}
static void quatD(const float* m, float* q) {              // the same, with the intermediates kept wide (x87 style)
    const double m00=m[0],m01=m[1],m02=m[2],m10=m[4],m11=m[5],m12=m[6],m20=m[8],m21=m[9],m22=m[10];
    double tr = m00 + m11 + m22;
    if (tr > 0.0) {
        double s = sqrt(tr + 1.0); q[3] = (float)(s * 0.5); s = 0.5 / s;
        q[0] = (float)((m21 - m12) * s); q[1] = (float)((m02 - m20) * s); q[2] = (float)((m10 - m01) * s);
    } else {
        int i = 0; if (m11 > m00) i = 1; if (m22 > (i == 0 ? m00 : m11)) i = 2;
        const double M[3][3] = {{m00,m01,m02},{m10,m11,m12},{m20,m21,m22}};
        int j = (i + 1) % 3, k = (i + 2) % 3;
        double s = sqrt(M[i][i] - M[j][j] - M[k][k] + 1.0);
        q[i] = (float)(s * 0.5); s = 0.5 / s;
        q[3] = (float)((M[k][j] - M[j][k]) * s); q[j] = (float)((M[j][i] + M[i][j]) * s); q[k] = (float)((M[k][i] + M[i][k]) * s);
    }
}
static void quatE(const float* m, float* q) {              // wide intermediates, and the scale applied as a division
    const double m00=m[0],m01=m[1],m02=m[2],m10=m[4],m11=m[5],m12=m[6],m20=m[8],m21=m[9],m22=m[10];
    double tr = m00 + m11 + m22;
    if (tr > 0.0) {
        double r = sqrt(tr + 1.0); q[3] = (float)(r * 0.5);
        q[0] = (float)((m21 - m12) / (2.0 * r)); q[1] = (float)((m02 - m20) / (2.0 * r)); q[2] = (float)((m10 - m01) / (2.0 * r));
    } else {
        int i = 0; if (m11 > m00) i = 1; if (m22 > (i == 0 ? m00 : m11)) i = 2;
        const double M[3][3] = {{m00,m01,m02},{m10,m11,m12},{m20,m21,m22}};
        int j = (i + 1) % 3, k = (i + 2) % 3;
        double r = sqrt(M[i][i] - M[j][j] - M[k][k] + 1.0);
        q[i] = (float)(r * 0.5);
        q[3] = (float)((M[k][j] - M[j][k]) / (2.0 * r)); q[j] = (float)((M[j][i] + M[i][j]) / (2.0 * r)); q[k] = (float)((M[k][i] + M[i][k]) / (2.0 * r));
    }
}
static void quatF(const float* m, float* q) {              // A, but the else branch sums starting from 1
    const float m00=m[0],m01=m[1],m02=m[2],m10=m[4],m11=m[5],m12=m[6],m20=m[8],m21=m[9],m22=m[10];
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f); q[3] = s * 0.5f; s = 0.5f / s;
        q[0] = (m21 - m12) * s; q[1] = (m02 - m20) * s; q[2] = (m10 - m01) * s;
    } else {
        int i = 0; if (m11 > m00) i = 1; if (m22 > (i == 0 ? m00 : m11)) i = 2;
        const float M[3][3] = {{m00,m01,m02},{m10,m11,m12},{m20,m21,m22}};
        int j = (i + 1) % 3, k = (i + 2) % 3;
        float s = sqrtf(1.0f + M[i][i] - M[j][j] - M[k][k]);
        q[i] = s * 0.5f; s = 0.5f / s;
        q[3] = (M[k][j] - M[j][k]) * s; q[j] = (M[j][i] + M[i][j]) * s; q[k] = (M[k][i] + M[i][k]) * s;
    }
}
static void quatG(const float* m, float* q) {              // A, but the else branch groups the two subtracted terms
    const float m00=m[0],m01=m[1],m02=m[2],m10=m[4],m11=m[5],m12=m[6],m20=m[8],m21=m[9],m22=m[10];
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f); q[3] = s * 0.5f; s = 0.5f / s;
        q[0] = (m21 - m12) * s; q[1] = (m02 - m20) * s; q[2] = (m10 - m01) * s;
    } else {
        int i = 0; if (m11 > m00) i = 1; if (m22 > (i == 0 ? m00 : m11)) i = 2;
        const float M[3][3] = {{m00,m01,m02},{m10,m11,m12},{m20,m21,m22}};
        int j = (i + 1) % 3, k = (i + 2) % 3;
        float s = sqrtf(M[i][i] - (M[j][j] + M[k][k]) + 1.0f);
        q[i] = s * 0.5f; s = 0.5f / s;
        q[3] = (M[k][j] - M[j][k]) * s; q[j] = (M[j][i] + M[i][j]) * s; q[k] = (M[k][i] + M[i][k]) * s;
    }
}
static void quatH(const float* m, float* q) {              // A, but the else branch divides instead of scaling
    const float m00=m[0],m01=m[1],m02=m[2],m10=m[4],m11=m[5],m12=m[6],m20=m[8],m21=m[9],m22=m[10];
    float tr = m00 + m11 + m22;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f); q[3] = s * 0.5f; s = 0.5f / s;
        q[0] = (m21 - m12) * s; q[1] = (m02 - m20) * s; q[2] = (m10 - m01) * s;
    } else {
        int i = 0; if (m11 > m00) i = 1; if (m22 > (i == 0 ? m00 : m11)) i = 2;
        const float M[3][3] = {{m00,m01,m02},{m10,m11,m12},{m20,m21,m22}};
        int j = (i + 1) % 3, k = (i + 2) % 3;
        float r = sqrtf(M[i][i] - M[j][j] - M[k][k] + 1.0f);
        q[i] = r * 0.5f;
        float d = r + r;
        q[3] = (M[k][j] - M[j][k]) / d; q[j] = (M[j][i] + M[i][j]) / d; q[k] = (M[k][i] + M[i][k]) / d;
    }
}
typedef void (*QFn)(const float*, float*);
static QFn kFns[8] = { quatA, quatB, quatC, quatD, quatE, quatF, quatG, quatH };
static const char* kNames[8] = { "A sqrt-then-divide", "B 0.5/sqrt, w=0.25/s", "C reciprocal multiply", "D wide intermediates", "E wide, divide by 2r", "F else: 1 + Mii - ...", "G else: Mii - (Mjj+Mkk)", "H else: divide by 2r" };
int main(int argc, char** argv) {
    FILE* f = fopen(argc > 1 ? argv[1] : "../quatpairs.txt", "r");
    if (!f) { printf("no input\n"); return 1; }
    char line[600];
    long n = 0, exact = 0, near1 = 0, sign = 0, bad = 0, hit[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    double worst = 0;
    while (fgets(line, sizeof(line), f)) {
        u32 w[16]; int got = 0;
        char* p = strtok(line, " \t\n");
        while (p && got < 16) { w[got++] = (u32)strtoul(p, 0, 16); p = strtok(0, " \t\n"); }
        if (got < 16) continue;
        float m[12], q[4], e[4];
        for (int i = 0; i < 12; ++i) m[i] = fw(w[i]);
        for (int i = 0; i < 4; ++i) e[i] = fw(w[12 + i]);
        ++n;
        for (int v = 0; v < 8; ++v) {
            kFns[v](m, q);
            bool be = true; for (int i = 0; i < 4; ++i) if (wf(q[i]) != bswap(w[12 + i])) be = false;
            if (be) ++hit[v];
        }
        kFns[0](m, q);
        bool bitEqual = true;
        for (int i = 0; i < 4; ++i) if (wf(q[i]) != bswap(w[12 + i])) bitEqual = false;
        if (bitEqual) { ++exact; continue; }
        if (0) { float tr = m[0] + m[5] + m[10];
            printf("  A-miss: trace %.7f diag %.6f %.6f %.6f | engine %.9f %.9f %.9f %.9f | mine %.9f %.9f %.9f %.9f\n", tr, m[0], m[5], m[10], e[0], e[1], e[2], e[3], q[0], q[1], q[2], q[3]); }
        double d = 0;
        for (int i = 0; i < 4; ++i) { double a = fabs((double)q[i] - e[i]); if (a > d) d = a; }
        if (d < 1e-6) ++near1; else { ++bad; if (bad <= 3) printf("  mismatch: engine %.8f %.8f %.8f %.8f | mine %.8f %.8f %.8f %.8f\n", e[0], e[1], e[2], e[3], q[0], q[1], q[2], q[3]); }
        if (d > worst) worst = d;
    }
    for (int v = 0; v < 8; ++v) printf("  variant %-24s bit-exact on %ld of %ld\n", kNames[v], hit[v], n);
    printf("pairs %ld: bit-exact %ld, bit-exact up to sign %ld, within 1e-6 %ld, different %ld (worst %.3g)\n", n, exact, sign, near1, bad, worst);
    return 0;
}
