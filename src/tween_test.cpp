// tween_test.cpp - offline proof for the blending in aotr_tween.inc, on the Direct3D the game runs on (DXVK d3d9.dll
// beside it).
//
// A scene is drawn the way the game draws: three effects whose parameters are named, shaped and written like the
// game's (units: World / View / Projection / ViewI + a WorldBones palette of quaternion + position joints written with
// SetRawValue; terrain and trees: WorldViewProjection / World / ViewI / WorldViewI; water: WorldView / Projection /
// View - every matrix through SetMatrixTranspose in the engine's layout), plus fixed-function quads through the
// device's own WORLD / VIEW / PROJECTION, quads whose vertices are already in camera space (identity VIEW under the
// camera's projection, as the engine draws them), flat "health bar" quads the test has already placed on screen for the
// frame's camera (identity VIEW and PROJECTION, inside the 3D pass) and an identity overlay after the 3D pass has been
// marked as ended. Units, rigid meshes and the camera move from
// frame to frame; along the way a unit jumps, a rigid mesh jumps, the camera cuts away and back, a unit appears,
// another disappears, one mesh is drawn twice a frame and two meshes share one draw.
//
// Run A, in-between frames on: each frame the DLL shows its in-between picture first; the test keeps it.
// Run B, everything off: the test draws every frame again, and between every two frames the picture it gets from
// the halfway values - computed with the DLL's own blend functions (so the arithmetic is the same) but matched,
// keyed and limited by the test's own knowledge of the scene.
//   1. every in-between picture of run A must be the test's halfway picture, byte for byte;
//   2. every game frame of run A must be the frame of run B, byte for byte (blending leaves nothing behind);
//   3. control: an in-between picture must NOT be the frame it was made from.
// And, to check the arithmetic itself rather than the bookkeeping: on frames where nothing jumps, the in-between
// picture is compared with the scene drawn at the true half-way time (units and camera where they really are at
// f - 0.5). The blend of two frames is not that state exactly - a turning joint is blended along the chord - so
// this one is a tolerance: almost no pixel may be visibly off.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void** vt(void* o) { return *(void***)o; }
#define CALL0(o, s)                 ((HRESULT(WINAPI*)(void*))vt(o)[s])(o)
#define CALL1(o, s, a)              ((HRESULT(WINAPI*)(void*, DWORD))vt(o)[s])(o, (DWORD)(a))
#define CALL2(o, s, a, b)           ((HRESULT(WINAPI*)(void*, DWORD, DWORD))vt(o)[s])(o, (DWORD)(a), (DWORD)(b))
#define CALL3(o, s, a, b, c)        ((HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD))vt(o)[s])(o, (DWORD)(a), (DWORD)(b), (DWORD)(c))
#define CALL4(o, s, a, b, c, d)     ((HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD, DWORD))vt(o)[s])(o, (DWORD)(a), (DWORD)(b), (DWORD)(c), (DWORD)(d))
#define CALL6(o, s, a, b, c, d, e, f) ((HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))vt(o)[s])(o, (DWORD)(a), (DWORD)(b), (DWORD)(c), (DWORD)(d), (DWORD)(e), (DWORD)(f))

struct PP { UINT BackBufferWidth, BackBufferHeight; DWORD BackBufferFormat; UINT BackBufferCount; DWORD MultiSampleType, MultiSampleQuality, SwapEffect;
            HWND hDeviceWindow; BOOL Windowed, EnableAutoDepthStencil; DWORD AutoDepthStencilFormat, Flags; UINT FullScreen_RefreshRateInHz, PresentationInterval; };
struct LR { INT Pitch; void* pBits; };

static const char kFxU[] =
"float4x3 World : World; float4x4 View : View; float4x4 Projection : Projection; float4x3 ViewI : ViewInverse;\n"
"int NumJointsPerVertex = 0; struct Bone { float4 q; float4 p; }; Bone WorldBones[30]; float4 Tint = {1,1,1,1};\n"
"struct VSIn { float4 pos : POSITION; float3 nrm : NORMAL; float4 idx : BLENDINDICES; };\n"
"struct VSOut { float4 pos : POSITION; float3 col : TEXCOORD0; };\n"
"float3 Rot(float4 q, float3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }\n"
"VSOut VSU(VSIn i) { VSOut o; int j = (int)i.idx.x; float4 q = WorldBones[j].q; float3 t = WorldBones[j].p.xyz;\n"
"  float3 w = Rot(q, i.pos.xyz) + t; float3 n = Rot(q, i.nrm);\n"
"  float3 ww = mul(float4(w, 1), World); float3 nn = normalize(mul(n, (float3x3)World));\n"
"  float3 toEye = normalize(ViewI[3] - ww);\n"
"  o.pos = mul(mul(float4(ww, 1), View), Projection);\n"
"  o.col = (nn * 0.5 + 0.5) * Tint.rgb * (0.55 + 0.45 * abs(dot(nn, toEye))); return o; }\n"
"float4 PS(VSOut i) : COLOR { return float4(i.col, 1); }\n"
"technique Default { pass p0 { VertexShader = compile vs_2_0 VSU(); PixelShader = compile ps_2_0 PS(); ZEnable = TRUE; ZWriteEnable = TRUE; CullMode = NONE; AlphaBlendEnable = FALSE; } }\n";
static const char kFxT[] =
"float4x4 WorldViewProjection : WorldViewProjection; float4x3 World : World; float4x3 ViewI : ViewInverse; float4x3 WorldViewI : WorldViewInverse; float4 Tint = {1,1,1,1};\n"
"struct VSIn { float4 pos : POSITION; float3 nrm : NORMAL; };\n"
"struct VSOut { float4 pos : POSITION; float3 col : TEXCOORD0; };\n"
"VSOut VST(VSIn i) { VSOut o; o.pos = mul(float4(i.pos.xyz, 1), WorldViewProjection);\n"
"  float3 ww = mul(float4(i.pos.xyz, 1), World); float3 eyeObj = WorldViewI[3]; float3 eyeW = ViewI[3];\n"
"  float fade = saturate(1.0 - length(eyeObj - i.pos.xyz) * 0.0015);\n"
"  o.col = Tint.rgb * (0.45 + 0.35 * frac(ww.x * 0.02) + 0.2 * frac(ww.z * 0.02)) * (0.5 + 0.5 * fade) * (0.8 + 0.2 * saturate(eyeW.y * 0.002)); return o; }\n"
"float4 PS(VSOut i) : COLOR { return float4(i.col, 1); }\n"
"technique Default { pass p0 { VertexShader = compile vs_2_0 VST(); PixelShader = compile ps_2_0 PS(); ZEnable = TRUE; ZWriteEnable = TRUE; CullMode = NONE; AlphaBlendEnable = FALSE; } }\n";
static const char kFxW[] =
"float4x4 WorldView : WORLDVIEW; float4x4 Projection : PROJECTION; float4x4 View : VIEW; float4 Tint = {1,1,1,1};\n"
"struct VSIn { float4 pos : POSITION; float3 nrm : NORMAL; };\n"
"struct VSOut { float4 pos : POSITION; float3 col : TEXCOORD0; };\n"
"VSOut VSW(VSIn i) { VSOut o; float4 v = mul(float4(i.pos.xyz, 1), WorldView); o.pos = mul(v, Projection);\n"
"  float3 vz = float3(View[0][2], View[1][2], View[2][2]);\n"
"  o.col = Tint.rgb * (0.5 + 0.5 * saturate(v.z * 0.002)) * (0.8 + 0.2 * abs(vz.y)); return o; }\n"
"float4 PS(VSOut i) : COLOR { return float4(i.col, 1); }\n"
"technique Default { pass p0 { VertexShader = compile vs_2_0 VSW(); PixelShader = compile ps_2_0 PS(); ZEnable = TRUE; ZWriteEnable = TRUE; CullMode = NONE; AlphaBlendEnable = FALSE; } }\n";

// ---------------------------------------------------------------- matrices, Direct3D's row-vector convention
static void mIdent(float* m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void mMul(const float* a, const float* b, float* o) { float t[16]; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { float s = 0; for (int k = 0; k < 4; ++k) s += a[r*4+k] * b[k*4+c]; t[r*4+c] = s; } memcpy(o, t, 64); }
static void mT(const float* a, float* o) { float t[16]; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) t[r*4+c] = a[c*4+r]; memcpy(o, t, 64); }
static void mInv(const float* a, float* o) {              // general 4x4 through Gauss-Jordan in double
    double m[4][8];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { m[r][c] = a[r*4+c]; m[r][4+c] = r == c ? 1.0 : 0.0; }
    for (int i = 0; i < 4; ++i) {
        int p = i; for (int r = i + 1; r < 4; ++r) if (fabs(m[r][i]) > fabs(m[p][i])) p = r;
        if (p != i) for (int c = 0; c < 8; ++c) { double t = m[i][c]; m[i][c] = m[p][c]; m[p][c] = t; }
        double d = m[i][i]; for (int c = 0; c < 8; ++c) m[i][c] /= d;
        for (int r = 0; r < 4; ++r) if (r != i) { double f = m[r][i]; for (int c = 0; c < 8; ++c) m[r][c] -= f * m[i][c]; }
    }
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) o[r*4+c] = (float)m[r][4+c];
}
static void mLookAt(float* m, const float* eye, const float* dir) {                         // left-handed, up = +y
    float z[3] = { dir[0], dir[1], dir[2] }; float zl = (float)sqrt(z[0]*z[0] + z[1]*z[1] + z[2]*z[2]); z[0] /= zl; z[1] /= zl; z[2] /= zl;
    float x[3] = { z[2], 0, -z[0] }; float xl = (float)sqrt(x[0]*x[0] + x[2]*x[2]); x[0] /= xl; x[2] /= xl;      // up cross z
    float y[3] = { z[1]*x[2] - z[2]*x[1], z[2]*x[0] - z[0]*x[2], z[0]*x[1] - z[1]*x[0] };
    float t[16] = { x[0], y[0], z[0], 0,  x[1], y[1], z[1], 0,  x[2], y[2], z[2], 0,
                    -(x[0]*eye[0] + x[1]*eye[1] + x[2]*eye[2]), -(y[0]*eye[0] + y[1]*eye[1] + y[2]*eye[2]), -(z[0]*eye[0] + z[1]*eye[1] + z[2]*eye[2]), 1 };
    memcpy(m, t, 64);
}
static void mPersp(float* m, float w, float h, float zn, float zf) { float t[16] = { w,0,0,0, 0,h,0,0, 0,0,zf/(zf-zn),1, 0,0,-zn*zf/(zf-zn),0 }; memcpy(m, t, 64); }
static void mWorld(float* m, float sc, float ang, float x, float y, float z) {               // scale, turn about y, move
    mIdent(m); float c = (float)cos(ang) * sc, s = (float)sin(ang) * sc; m[0] = c; m[2] = -s; m[5] = sc; m[8] = s; m[10] = c; m[12] = x; m[13] = y; m[14] = z; }
static void quatAxis(float* q, float ax, float ay, float az, float ang) { float s = (float)sin(ang * 0.5f); q[0] = ax * s; q[1] = ay * s; q[2] = az * s; q[3] = (float)cos(ang * 0.5f); }
static void quatMul(const float* a, const float* b, float* o) {
    float t[4] = { a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1], a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0], a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3], a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2] };
    memcpy(o, t, 16);
}

// ---------------------------------------------------------------- the scene
#define NF 30                    // frames
#define NU 10                    // unit meshes: 0..7 single draws, 8 and 9 share one draw
#define NR 4                     // rigid meshes
#define NT 6                     // trees
#define NQ 3                     // fixed-function quads
#define NB 5                     // flat bars pinned to world points
struct UDraw { DWORD id[2]; int nIds, joints; float pal[4 * 8]; };
struct Params {
    float devV[16], devP[16];                              // the device's VIEW / PROJECTION
    float uView[16], uProj[16], uViewI[16];                // unit effect, the engine's layout (transposed)
    float tWVP[16], tViewI[16], tWVI[16];                  // terrain: World is the identity
    float treeWVP[NT][16], treeWVI[NT][16];
    float wWV[16], wProj[16], wView[16];                   // water
    UDraw ud[NU + 2]; int nUd;
    float rigidW[NR][16]; DWORD rigidId[NR];
    float barProj[16]; float barH; float bar[NB][2];       // flat bars: the PROJECTION they are drawn with (the identity in a game frame), the height they are pinned at, where they are on screen
    float csView[16];                                      // what the device's VIEW is while camera-space quads are drawn (the identity in a game frame)
    float csQuad[NQ][4][3];                                // their corners, in this frame's camera space
    DWORD clear;
};
static float g_treeW[NT][16], g_treeWe[NT][16], g_waterW[16], g_ffW[NQ][16];
static float g_barLift = 15.0f;
static Params g_cur[NF], g_mid[NF];

static void unitPalette(int k, float f, float zOff, float* pal) {          // two joints: quaternion, position, 0
    float x = 560.0f + 45.0f * (float)(k % 4) + (2.0f + 0.3f * k) * f, y = 8.0f, z = 430.0f + 40.0f * (float)(k / 4) + (1.0f - 0.2f * k) * f + zOff;
    if (k == 3 && f >= 8) x += 200.0f;                                     // a jump no unit makes in one frame
    float q0[4], q1[4], tilt[4];
    quatAxis(q0, 0, 1, 0, 0.1f * k + 0.07f * f);
    quatAxis(tilt, 1, 0, 0, 0.05f * f + 0.2f * k);
    quatMul(q0, tilt, q1);
    memset(pal, 0, 64);
    memcpy(pal, q0, 16); pal[4] = x; pal[5] = y; pal[6] = z;
    memcpy(pal + 8, q1, 16); pal[12] = x + 2.0f * (float)sin(0.3f * f + k); pal[13] = y + 17.0f; pal[14] = z;
}
static void buildParams(float f, Params& p) {
    memset(&p, 0, sizeof(p));
    float cf = f <= 3.0f ? f : (f <= 13.0f ? 3.0f : f - 10.0f);              // the camera's own time: it stands still from frame 3 to frame 13
    float eye[3] = { 600.0f + 6.0f * cf, 330.0f, 170.0f + 3.0f * cf }, dir[3] = { 0.0f, -0.72f, 0.69f };
    if (f >= 22 && f < 25) eye[0] += 450.0f;                               // a cut away and, three frames later, back
    float V[16], P[16], VI[16], VP[16], t[16];
    mLookAt(V, eye, dir); mPersp(P, 1.35f, 1.8f, 5.0f, 4000.0f);
    memcpy(p.devV, V, 64); memcpy(p.devP, P, 64);
    mInv(V, VI); mMul(V, P, VP);
    mT(V, p.uView); mT(P, p.uProj); mT(VI, p.uViewI);
    mT(VP, p.tWVP); mT(VI, p.tViewI); mT(VI, p.tWVI);
    for (int i = 0; i < NT; ++i) { mMul(g_treeW[i], VP, t); mT(t, p.treeWVP[i]); mMul(g_treeW[i], V, t); mInv(t, t); mT(t, p.treeWVI[i]); }
    mMul(g_waterW, V, t); mT(t, p.wWV); mT(P, p.wProj); mT(V, p.wView);
    p.nUd = 0;
    for (int k = 0; k < 8; ++k) {
        if (k == 5 && f >= 18) continue;                                   // gone
        if (k == 7 && f < 15) continue;                                    // not there yet
        UDraw& d = p.ud[p.nUd++]; d.id[0] = 0x1000u + (DWORD)k * 0x40; d.nIds = 1; d.joints = 2; unitPalette(k, f, 0, d.pal);
    }
    if (f >= 10) { UDraw& d = p.ud[p.nUd++]; d.id[0] = 0x1000u; d.nIds = 1; d.joints = 2; unitPalette(0, f, 34.0f, d.pal); }    // mesh 0 a second time
    { UDraw& d = p.ud[p.nUd++]; d.id[0] = 0x1000u + 8 * 0x40; d.id[1] = 0x1000u + 9 * 0x40; d.nIds = 2; d.joints = 4;
      unitPalette(8, f, 0, d.pal); unitPalette(9, f, 0, d.pal + 16); }
    for (int r = 0; r < NR; ++r) {
        float w[16]; float z = 610.0f - 2.0f * f; if (r == 2 && f >= 14) z += 300.0f;                  // a rigid mesh that jumps
        mWorld(w, 7.0f, 0.2f * r + 0.04f * f, 720.0f + 36.0f * r + 1.5f * f, 10.0f, z);
        mT(w, p.rigidW[r]); p.rigidId[r] = 0x9000u + (DWORD)r * 0x100;
    }
    // flat bars: pinned to points on the plane the DLL will assume - where the frame's units stand (the mean of every
    // tagged palette's first joint, in draw order) plus its lift - and placed on screen with this frame's camera
    { float sum = 0; int n = 0;
      for (int i = 0; i < p.nUd; ++i) for (int sIdx = 0; sIdx < p.ud[i].nIds; ++sIdx) { const float* j0 = p.ud[i].pal + sIdx * (p.ud[i].joints / p.ud[i].nIds) * 8; sum += j0[6]; ++n; }
      p.barH = sum / (float)n + g_barLift;
      mIdent(p.barProj);
      for (int b = 0; b < NB; ++b) {
          float wx = 575.0f + 38.0f * b, wy = 6.0f + 7.0f * b, wz = p.barH;
          float cx = wx * VP[0] + wy * VP[4] + wz * VP[8] + VP[12], cy = wx * VP[1] + wy * VP[5] + wz * VP[9] + VP[13], cw = wx * VP[3] + wy * VP[7] + wz * VP[11] + VP[15];
          p.bar[b][0] = cx / cw; p.bar[b][1] = cy / cw;
      } }
    mIdent(p.csView);
    for (int q = 0; q < NQ; ++q) for (int c = 0; c < 4; ++c) {                                         // upright quads standing in the world, moved into camera space here
        float wx = 590.0f + 55.0f * q + ((c & 1) ? 9.0f : -9.0f), wy = (c & 2) ? 30.0f : 4.0f, wz = 470.0f + 20.0f * q;
        p.csQuad[q][c][0] = wx * V[0] + wy * V[4] + wz * V[8] + V[12];
        p.csQuad[q][c][1] = wx * V[1] + wy * V[5] + wz * V[9] + V[13];
        p.csQuad[q][c][2] = wx * V[2] + wy * V[6] + wz * V[10] + V[14];
    }
    p.clear = 0xFF182028;
}
typedef int (__cdecl* BlendF)(int, const float*, const float*, int, float, float*);
typedef int (__cdecl* ReaimF)(int, int, const float*, const float*, const float*, const float*, const float*, float*);
static BlendF g_blend; static ReaimF g_reaim;
static const float* findSlice(const Params& p, DWORD id, int occurrence, int* joints) {              // the n-th palette of this mesh in a frame
    for (int i = 0; i < p.nUd; ++i) for (int m = 0; m < p.ud[i].nIds; ++m)
        if (p.ud[i].id[m] == id && occurrence-- == 0) { *joints = p.ud[i].joints / p.ud[i].nIds; return p.ud[i].pal + m * (p.ud[i].joints / p.ud[i].nIds) * 8; }
    return NULL;
}
static void buildMid(const Params& a, const Params& b, Params& m, float t) {
    m = b;
    int subP = g_blend(3, a.devP, b.devP, 0, t, m.devP), subV = g_blend(4, a.devV, b.devV, 0, t, m.devV);
    g_blend(2, a.uView, b.uView, 0, t, m.uView); g_blend(3, a.uProj, b.uProj, 0, t, m.uProj); g_blend(2, a.uViewI, b.uViewI, 0, t, m.uViewI);
    g_blend(2, a.tViewI, b.tViewI, 0, t, m.tViewI);
    g_blend(3, a.wProj, b.wProj, 0, t, m.wProj); g_blend(2, a.wView, b.wView, 0, t, m.wView);
    if (subV || subP) {                                    // products: this frame's, re-aimed from this frame's camera to the blended one
        g_reaim(6, 1, b.tWVP, b.devV, b.devP, m.devV, m.devP, m.tWVP);
        g_reaim(8, 1, b.tWVI, b.devV, b.devP, m.devV, m.devP, m.tWVI);
        for (int i = 0; i < NT; ++i) { g_reaim(6, 1, b.treeWVP[i], b.devV, b.devP, m.devV, m.devP, m.treeWVP[i]); g_reaim(8, 1, b.treeWVI[i], b.devV, b.devP, m.devV, m.devP, m.treeWVI[i]); }
        g_reaim(7, 1, b.wWV, b.devV, b.devP, m.devV, m.devP, m.wWV);
    }
    if (subV) g_reaim(1, 0, NULL, b.devV, NULL, m.devV, NULL, m.csView);       // camera-space quads: from this frame's camera space to the blended camera's
    if (subV) g_reaim(2, 0, &b.barH, b.devV, b.devP, m.devV, m.devP, m.barProj);  // flat bars: the ground plane's mapping between the two cameras, as their projection
    for (int i = 0; i < b.nUd; ++i) for (int s = 0; s < b.ud[i].nIds; ++s) {
        DWORD id = b.ud[i].id[s]; int per = b.ud[i].joints / b.ud[i].nIds, occ = 0;
        for (int j = 0; j < i; ++j) for (int q = 0; q < b.ud[j].nIds; ++q) if (b.ud[j].id[q] == id) ++occ;
        for (int q = 0; q < s; ++q) if (b.ud[i].id[q] == id) ++occ;
        int pj = 0; const float* prev = findSlice(a, id, occ, &pj);
        if (prev && pj == per) g_blend(0, prev, b.ud[i].pal + s * per * 8, per, t, m.ud[i].pal + s * per * 8);
    }
    for (int r = 0; r < NR; ++r) g_blend(1, a.rigidW[r], b.rigidW[r], 0, t, m.rigidW[r]);
}

// ---------------------------------------------------------------- drawing
struct Vtx { float x, y, z, nx, ny, nz; DWORD joint; float weight; float pad[4]; };                  // 48 bytes, like the game's skins
static void makeCube(Vtx* v, WORD* idx, int nJoints, DWORD jointBase, WORD indexBase) {
    static const float n[6][3] = {{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
    int vi = 0, ii = 0;
    for (int f = 0; f < 6; ++f) {
        float nx = n[f][0], ny = n[f][1], nz = n[f][2];
        float ux = ny ? 1.0f : (nz ? 1.0f : 0.0f), uy = 0, uz = ny ? 0.0f : (nx ? 1.0f : 0.0f);
        float wx = ny*uz - nz*uy, wy = nz*ux - nx*uz, wz = nx*uy - ny*ux;
        for (int k = 0; k < 4; ++k) {
            float a = (k == 0 || k == 3) ? -1.0f : 1.0f, b = (k < 2) ? -1.0f : 1.0f;
            Vtx& q = v[vi + k]; memset(&q, 0, sizeof(q));
            q.x = (nx + a*ux + b*wx) * 7.0f; q.y = (ny + a*uy + b*wy) * 7.0f; q.z = (nz + a*uz + b*wz) * 7.0f; q.nx = nx; q.ny = ny; q.nz = nz;
            q.joint = jointBase + (DWORD)(f % nJoints); q.weight = 1.0f;
        }
        static const WORD t[6] = {0,1,2,0,2,3};
        for (int k = 0; k < 6; ++k) idx[ii++] = (WORD)(indexBase + vi + t[k]);
        vi += 4;
    }
}
typedef void (__cdecl* MarkF)(void*);
typedef void (__cdecl* TagF)(void*, int);
typedef void (__cdecl* TagNF)(const DWORD*, int);
static MarkF g_mark; static TagF g_tag; static TagNF g_tagN; static void (__cdecl* g_viewsEnd)();
static void (__cdecl* g_press)() = NULL; static bool g_pressInFrame = false;   // "the producer ran out of queue space in the middle of this frame": its stretch is given back
static void* g_dev, *g_fxU, *g_fxT, *g_fxW, *g_decl, *g_bb, *g_sys;
static void* g_vbUnit, *g_ibUnit, *g_vbPair, *g_ibPair, *g_vbRigid, *g_ibRigid, *g_vbGrid, *g_ibGrid, *g_vbWater, *g_ibWater;
static DWORD g_gridV, g_gridP;
static struct { DWORD world, view, proj, viewI, joints, bones, tint, tech; } hU;
static struct { DWORD wvp, world, viewI, wvi, tint, tech; } hT;
static struct { DWORD wv, proj, view, tint, tech; } hW;
typedef HRESULT (WINAPI* MatF)(void*, DWORD, const float*);
typedef HRESULT (WINAPI* RawF)(void*, DWORD, const void*, UINT, UINT);
static void setMT(void* fx, DWORD h, const float* m) { ((MatF)vt(fx)[44])(fx, h, m); }               // SetMatrixTranspose
static void setVec(void* fx, DWORD h, float r, float g, float b) { float v[4] = { r, g, b, 1 }; ((MatF)vt(fx)[34])(fx, h, v); }
static void bindMesh(void* vb, void* ib) { void* dev = g_dev; CALL1(dev, 87, g_decl); CALL4(dev, 100, 0, vb, 0, sizeof(Vtx)); CALL1(dev, 104, ib); }

static void drawFrame(const Params& p, bool mark) {
    void* dev = g_dev; UINT np = 0;
    static const float kI[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    if (mark) g_mark(dev);                                                  // from here the frame is drawn into the back buffer
    CALL6(dev, 43, 0, 0, 3, p.clear, 0x3F800000, 0);                        // Clear target + z
    CALL0(dev, 41);                                                         // BeginScene
    CALL2(dev, 44, 3, p.devP); CALL2(dev, 44, 256, kI); CALL2(dev, 44, 2, p.devV);          // the view's camera, as the engine sets it
    if (mark && g_pressInFrame && g_press) g_press();
    // terrain and trees
    { void* fx = g_fxT;
      CALL1(fx, 58, hT.tech);
      setMT(fx, hT.wvp, p.tWVP); setMT(fx, hT.world, kI); setMT(fx, hT.viewI, p.tViewI); setMT(fx, hT.wvi, p.tWVI); setVec(fx, hT.tint, 0.55f, 0.75f, 0.45f);
      ((HRESULT(WINAPI*)(void*, UINT*, DWORD))vt(fx)[63])(fx, &np, 6); CALL1(fx, 64, 0);
      CALL0(fx, 65); bindMesh(g_vbGrid, g_ibGrid); CALL6(dev, 82, 4, 0, 0, g_gridV, 0, g_gridP);
      for (int i = 0; i < NT; ++i) {
          setMT(fx, hT.wvp, p.treeWVP[i]); setMT(fx, hT.world, g_treeWe[i]); setMT(fx, hT.wvi, p.treeWVI[i]); setVec(fx, hT.tint, 0.3f + 0.1f * i, 0.5f, 0.2f);
          CALL0(fx, 65); bindMesh(g_vbRigid, g_ibRigid); CALL6(dev, 82, 4, 0, 0, 24, 0, 12);
      }
      CALL0(fx, 66); CALL0(fx, 67); }
    // water
    { void* fx = g_fxW;
      CALL1(fx, 58, hW.tech);
      setMT(fx, hW.wv, p.wWV); setMT(fx, hW.proj, p.wProj); setMT(fx, hW.view, p.wView); setVec(fx, hW.tint, 0.3f, 0.5f, 0.9f);
      ((HRESULT(WINAPI*)(void*, UINT*, DWORD))vt(fx)[63])(fx, &np, 6); CALL1(fx, 64, 0);
      CALL0(fx, 65); bindMesh(g_vbWater, g_ibWater); CALL6(dev, 82, 4, 0, 0, 4, 0, 2);
      CALL0(fx, 66); CALL0(fx, 67); }
    // units and rigid meshes
    { void* fx = g_fxU;
      static const float kIdentPal[8] = { 0, 0, 0, 1, 0, 0, 0, 0 };
      CALL1(fx, 58, hU.tech);
      setMT(fx, hU.view, p.uView); setMT(fx, hU.viewI, p.uViewI); setMT(fx, hU.proj, p.uProj); setVec(fx, hU.tint, 1, 1, 1);
      CALL2(fx, 26, hU.joints, 0); ((RawF)vt(fx)[78])(fx, hU.bones, kIdentPal, 0, 32); setMT(fx, hU.world, kI);      // a batch's defaults
      ((HRESULT(WINAPI*)(void*, UINT*, DWORD))vt(fx)[63])(fx, &np, 6); CALL1(fx, 64, 0);
      for (int i = 0; i < p.nUd; ++i) {
          const UDraw& d = p.ud[i];
          if (d.nIds == 1) g_tag((void*)(ULONG_PTR)d.id[0], 1); else g_tagN(d.id, d.nIds);
          CALL2(fx, 26, hU.joints, 1);
          ((RawF)vt(fx)[78])(fx, hU.bones, d.pal, 0, (UINT)(d.joints * 32));
          setMT(fx, hU.world, kI);
          CALL0(fx, 65);
          if (d.nIds == 1) { bindMesh(g_vbUnit, g_ibUnit); CALL6(dev, 82, 4, 0, 0, 24, 0, 12); }
          else { bindMesh(g_vbPair, g_ibPair); CALL6(dev, 82, 4, 0, 0, 48, 0, 24); }
      }
      for (int r = 0; r < NR; ++r) {
          g_tag((void*)(ULONG_PTR)p.rigidId[r], 1);
          CALL2(fx, 26, hU.joints, 0);
          ((RawF)vt(fx)[78])(fx, hU.bones, kIdentPal, 0, 32);
          setMT(fx, hU.world, p.rigidW[r]);
          CALL0(fx, 65); bindMesh(g_vbRigid, g_ibRigid); CALL6(dev, 82, 4, 0, 0, 24, 0, 12);
      }
      CALL0(fx, 66); CALL0(fx, 67); }
    // without shaders: quads through the device's own transforms, then an overlay with an identity view
    CALL1(dev, 92, 0); CALL1(dev, 107, 0); CALL1(dev, 89, 0x002 | 0x040);
    CALL2(dev, 57, 137, 0); CALL2(dev, 57, 22, 1); CALL2(dev, 57, 7, 1); CALL2(dev, 65, 0, 0);
    struct FV { float x, y, z; DWORD c; };
    for (int q = 0; q < NQ; ++q) {
        FV v[4] = { {-14, 0, 0, 0xFFE0C040}, {14, 0, 0, 0xFFE0C040}, {-14, 34, 0, 0xFFFFF0A0}, {14, 34, 0, 0xFFFFF0A0} };
        CALL2(dev, 44, 256, g_ffW[q]);
        CALL4(dev, 83, 5, 2, v, sizeof(FV));
    }
    for (int q = 0; q < NQ; ++q) {                                           // camera-space quads: identity VIEW, the camera's projection, then the camera again
        FV v[4]; for (int c = 0; c < 4; ++c) { v[c].x = p.csQuad[q][c][0]; v[c].y = p.csQuad[q][c][1]; v[c].z = p.csQuad[q][c][2]; v[c].c = 0xFFFF4060 + (DWORD)q * 0x00003000; }
        CALL2(dev, 44, 256, kI); CALL2(dev, 44, 2, p.csView);
        CALL4(dev, 83, 5, 2, v, sizeof(FV));
        CALL2(dev, 44, 2, p.devV);
    }
    // flat bars, still inside the 3D pass: projection, world, view to the identity, one quad each, the camera's projection back
    CALL2(dev, 44, 3, p.barProj); CALL2(dev, 44, 256, kI); CALL2(dev, 44, 2, kI); CALL2(dev, 57, 7, 0);
    for (int b = 0; b < NB; ++b) {
        float x = p.bar[b][0], y = p.bar[b][1]; DWORD c = 0xFF30E040 + (DWORD)b * 0x00200000;
        FV v[4] = { {x - 0.035f, y - 0.012f, 0, c}, {x + 0.035f, y - 0.012f, 0, c}, {x - 0.035f, y + 0.012f, 0, c}, {x + 0.035f, y + 0.012f, 0, c} };
        CALL4(dev, 83, 5, 2, v, sizeof(FV));
    }
    CALL2(dev, 44, 3, p.devP);
    if (mark) g_viewsEnd();                                                 // the 3D pass is over: what follows is the interface
    { float ortho[16]; mIdent(ortho);
      CALL2(dev, 44, 256, kI); CALL2(dev, 44, 2, kI); CALL2(dev, 44, 3, ortho); CALL2(dev, 57, 7, 0);
      FV v[4] = { {-0.95f, 0.75f, 0.5f, 0xFF40A0FF}, {-0.55f, 0.75f, 0.5f, 0xFF40A0FF}, {-0.95f, 0.95f, 0.5f, 0xFF2060C0}, {-0.55f, 0.95f, 0.5f, 0xFF2060C0} };
      CALL4(dev, 83, 5, 2, v, sizeof(FV)); }
    CALL0(dev, 42);                                                         // EndScene
}
static bool shot(BYTE* dst) {
    if (FAILED(CALL2(g_dev, 32, g_bb, g_sys))) { printf("GetRenderTargetData failed\n"); return false; }
    LR lr = { 0, NULL };
    if (FAILED(CALL3(g_sys, 13, &lr, 0, 0x10)) || !lr.pBits) { printf("LockRect failed\n"); return false; }
    for (int y = 0; y < 480; ++y) memcpy(dst + y * 640 * 4, (BYTE*)lr.pBits + y * lr.Pitch, 640 * 4);
    CALL0(g_sys, 14);
    return true;
}
static void present() { ((HRESULT(WINAPI*)(void*, void*, void*, HWND, void*))vt(g_dev)[17])(g_dev, NULL, NULL, NULL, NULL); }
static int differing(const BYTE* a, const BYTE* b) { int n = 0; for (int i = 0; i < 640 * 480; ++i) if (*(const DWORD*)(a + i * 4) != *(const DWORD*)(b + i * 4)) ++n; return n; }
static DWORD g_fakeDraw[3];
static DWORD __cdecl fakeBeginRender(DWORD a, DWORD b, DWORD c, DWORD d) { return a ^ b ^ c ^ d; }
static void __fastcall fakeMeshDraw(void* self, void* edx, DWORD effect, DWORD count) { (void)edx; g_fakeDraw[0] = (DWORD)(ULONG_PTR)self; g_fakeDraw[1] = effect; g_fakeDraw[2] = count; }
static void* makeVB(const void* data, UINT bytes) { void* vb = NULL, *p = NULL; CALL6(g_dev, 26, bytes, 8, 0, 1, &vb, 0); CALL4(vb, 11, 0, 0, &p, 0); memcpy(p, data, bytes); CALL0(vb, 12); return vb; }
static void* makeIB(const void* data, UINT bytes) { void* ib = NULL, *p = NULL; CALL6(g_dev, 27, bytes, 8, 101, 1, &ib, 0); CALL4(ib, 11, 0, 0, &p, 0); memcpy(p, data, bytes); CALL0(ib, 12); return ib; }

// A step that must come back: if it has not after 20 s, the test says so and ends (a hang would otherwise only show as a test that never finishes).
static volatile LONG g_wdArmed = 0; static const char* g_wdWhat = "";
static DWORD WINAPI wdProc(LPVOID) { Sleep(20000); if (g_wdArmed) { printf("HUNG: %s did not come back in 20 s\nFAILED\n", g_wdWhat); fflush(stdout); ExitProcess(9); } return 0; }
static void wdArm(const char* what) { g_wdWhat = what; g_wdArmed = 1; CloseHandle(CreateThread(NULL, 0, wdProc, NULL, 0, NULL)); }
static void wdOff() { g_wdArmed = 0; }
int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    SetEnvironmentVariableA("DXVK_CONFIG", "dxvk.hud=");
    SetEnvironmentVariableA("DXVK_LOG_LEVEL", "none");
    SetEnvironmentVariableA("DXVK_STATE_CACHE", "0");
    HMODULE hx = LoadLibraryA("d3dx9_27.dll"); if (!hx) { printf("no d3dx9_27\n"); return 3; }
    HMODULE hd3d = LoadLibraryA("d3d9.dll"); if (!hd3d) { printf("no d3d9\n"); return 3; }
    HMODULE ha = LoadLibraryA("bfme2_accel.new.dll"); if (!ha) { printf("no bfme2_accel.new.dll (%lu)\n", GetLastError()); return 3; }
    typedef int (__cdecl* InstallF)(void*, void*); typedef void (__cdecl* ModeF)(int); typedef int (__cdecl* GrabF)(BYTE*, int); typedef void (__cdecl* StatsF)(char*, int);
    InstallF rtInstall = (InstallF)GetProcAddress(ha, "AotrRtTestInstall");
    ModeF mode = (ModeF)GetProcAddress(ha, "AotrTweenTestMode"); GrabF grab = (GrabF)GetProcAddress(ha, "AotrTweenTestGrab"); StatsF stats = (StatsF)GetProcAddress(ha, "AotrTweenTestStats");
    g_mark = (MarkF)GetProcAddress(ha, "AotrTweenTestMark"); g_tag = (TagF)GetProcAddress(ha, "AotrTweenTestTag"); g_tagN = (TagNF)GetProcAddress(ha, "AotrTweenTestTagN");
    g_blend = (BlendF)GetProcAddress(ha, "AotrTweenTestBlend"); g_reaim = (ReaimF)GetProcAddress(ha, "AotrTweenTestReaim");
    g_viewsEnd = (void (__cdecl*)())GetProcAddress(ha, "AotrTweenTestViewsEnd");
    g_press = (void (__cdecl*)())GetProcAddress(ha, "AotrTweenTestPressure");
    { typedef float (__cdecl* LiftF)(); LiftF lift = (LiftF)GetProcAddress(ha, "AotrTweenTestBarLift"); if (lift) g_barLift = lift(); }
    if (!rtInstall || !mode || !grab || !stats || !g_mark || !g_tag || !g_tagN || !g_blend || !g_reaim || !g_viewsEnd) { printf("missing test exports\n"); return 3; }
    Sleep(1500);                                                            // the DLL's own init thread

    typedef void* (WINAPI* Create9F)(UINT);
    typedef HRESULT (WINAPI* CreateEffectF)(void*, const char*, UINT, void*, void*, DWORD, void*, void**, void**);
    Create9F Create9 = (Create9F)GetProcAddress(hd3d, "Direct3DCreate9");
    CreateEffectF CreateEffect = (CreateEffectF)GetProcAddress(hx, "D3DXCreateEffect");
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "tweentest";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "tweentest", "tt", WS_POPUP, -3000, -3000, 640, 480, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    void* d3d = Create9(32); if (!d3d) { printf("Direct3DCreate9 failed\n"); return 4; }
    PP pp = {0}; pp.BackBufferWidth = 640; pp.BackBufferHeight = 480; pp.BackBufferFormat = 21; pp.BackBufferCount = 1;
    pp.SwapEffect = 1; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE; pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = 75; pp.PresentationInterval = 0x80000000;
    void* dev = NULL;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, DWORD, HWND, DWORD, PP*, void**))vt(d3d)[16])(d3d, 0, 1, hwnd, 0x40u | 0x2u | 0x4u, &pp, &dev);
    if (FAILED(hr) || !dev) { printf("CreateDevice hr=0x%08lX\n", (unsigned long)hr); return 4; }
    g_dev = dev;
    const char* src[3] = { kFxU, kFxT, kFxW }; void** fxs[3] = { &g_fxU, &g_fxT, &g_fxW };
    if (getenv("TWEEN_TEST_NOPRE")) {                                       // no expression of parameters alone: nothing for the compiler to turn into a preshader
        static char t2[4096], w2[4096];
        static const char kT0[] = "saturate(eyeW.y * 0.002)", kT1[] = "saturate((eyeW.y + i.pos.y) * 0.002)", kW0[] = "abs(vz.y)", kW1[] = "abs(vz.y + i.pos.x * 0.0001)";
        const char* a = strstr(kFxT, kT0); const char* b = strstr(kFxW, kW0);
        if (!a || !b) { printf("TWEEN_TEST_NOPRE: the expressions are not in the effects\n"); return 5; }
        memcpy(t2, kFxT, a - kFxT); strcpy(t2 + (a - kFxT), kT1); strcat(t2, a + sizeof(kT0) - 1);
        memcpy(w2, kFxW, b - kFxW); strcpy(w2 + (b - kFxW), kW1); strcat(w2, b + sizeof(kW0) - 1);
        src[1] = t2; src[2] = w2;
        printf("effects without preshaders (TWEEN_TEST_NOPRE)\n");
    }
    for (int i = 0; i < 3; ++i) {
        void* err = NULL;
        hr = CreateEffect(dev, src[i], (UINT)strlen(src[i]), NULL, NULL, 0, NULL, fxs[i], &err);
        if (FAILED(hr) || !*fxs[i]) { printf("D3DXCreateEffect %d hr=0x%08lX %s\n", i, (unsigned long)hr, err ? (char*)((void*(WINAPI*)(void*))vt(err)[3])(err) : ""); return 5; }
    }
    #define H(fx, name) ((DWORD(WINAPI*)(void*, DWORD, const char*))vt(fx)[9])(fx, 0, name)
    #define TECH(fx) ((DWORD(WINAPI*)(void*, const char*))vt(fx)[13])(fx, "Default")
    hU.world = H(g_fxU, "World"); hU.view = H(g_fxU, "View"); hU.proj = H(g_fxU, "Projection"); hU.viewI = H(g_fxU, "ViewI"); hU.joints = H(g_fxU, "NumJointsPerVertex");
    hU.bones = H(g_fxU, "WorldBones"); hU.tint = H(g_fxU, "Tint"); hU.tech = TECH(g_fxU);
    hT.wvp = H(g_fxT, "WorldViewProjection"); hT.world = H(g_fxT, "World"); hT.viewI = H(g_fxT, "ViewI"); hT.wvi = H(g_fxT, "WorldViewI"); hT.tint = H(g_fxT, "Tint"); hT.tech = TECH(g_fxT);
    hW.wv = H(g_fxW, "WorldView"); hW.proj = H(g_fxW, "Projection"); hW.view = H(g_fxW, "View"); hW.tint = H(g_fxW, "Tint"); hW.tech = TECH(g_fxW);
    if (!hU.world || !hU.view || !hU.proj || !hU.viewI || !hU.joints || !hU.bones || !hU.tech || !hT.wvp || !hT.world || !hT.viewI || !hT.wvi || !hT.tech || !hW.wv || !hW.proj || !hW.view || !hW.tech) { printf("handles missing\n"); return 5; }

    static const BYTE kDecl[] = { 0,0, 0,0, 2, 0, 0, 0,    0,0, 12,0, 2, 0, 3, 0,    0,0, 24,0, 5, 0, 2, 0,    0,0, 28,0, 0, 0, 5, 0,    0xFF,0, 0,0, 17, 0, 0, 0 };
    if (FAILED(CALL2(dev, 86, kDecl, &g_decl)) || !g_decl) { printf("CreateVertexDeclaration failed\n"); return 5; }
    if (FAILED(((HRESULT(WINAPI*)(void*, UINT, UINT, DWORD, DWORD, void**, void*))vt(dev)[36])(dev, 640, 480, 21, 2, &g_sys, NULL)) || !g_sys) { printf("no system surface\n"); return 5; }
    if (FAILED(CALL4(dev, 18, 0, 0, 0, &g_bb)) || !g_bb) { printf("no back buffer\n"); return 5; }

    // geometry
    { Vtx c[48]; WORD ix[72];
      makeCube(c, ix, 2, 0, 0); g_vbUnit = makeVB(c, 24 * sizeof(Vtx)); g_ibUnit = makeIB(ix, 36 * sizeof(WORD));
      makeCube(c, ix, 2, 0, 0); makeCube(c + 24, ix + 36, 2, 2, 24); g_vbPair = makeVB(c, 48 * sizeof(Vtx)); g_ibPair = makeIB(ix, 72 * sizeof(WORD));     // two copies, joints 0-1 and 2-3
      makeCube(c, ix, 1, 0, 0); g_vbRigid = makeVB(c, 24 * sizeof(Vtx)); g_ibRigid = makeIB(ix, 36 * sizeof(WORD)); }
    { const int G = 24; static Vtx gv[25 * 25]; static WORD gi[24 * 24 * 6]; int ii = 0;
      for (int z = 0; z <= G; ++z) for (int x = 0; x <= G; ++x) { Vtx& q = gv[z * (G + 1) + x]; memset(&q, 0, sizeof(q)); q.x = x * 125.0f; q.y = 0.3f * (float)((x * 7 + z * 3) % 5); q.z = z * 125.0f; q.ny = 1; }
      for (int z = 0; z < G; ++z) for (int x = 0; x < G; ++x) { WORD a = (WORD)(z * (G + 1) + x), b = (WORD)(a + 1), c = (WORD)(a + G + 1), d = (WORD)(c + 1);
          gi[ii++] = a; gi[ii++] = c; gi[ii++] = b; gi[ii++] = b; gi[ii++] = c; gi[ii++] = d; }
      g_vbGrid = makeVB(gv, sizeof(gv)); g_ibGrid = makeIB(gi, sizeof(gi)); g_gridV = (G + 1) * (G + 1); g_gridP = G * G * 2; }
    { Vtx wv[4]; memset(wv, 0, sizeof(wv)); WORD wi[6] = { 0, 2, 1, 1, 2, 3 };
      for (int i = 0; i < 4; ++i) { wv[i].x = (i & 1) ? 150.0f : -150.0f; wv[i].z = (i & 2) ? 120.0f : -120.0f; wv[i].ny = 1; }
      g_vbWater = makeVB(wv, sizeof(wv)); g_ibWater = makeIB(wi, sizeof(wi)); }
    for (int i = 0; i < NT; ++i) { mWorld(g_treeW[i], 1.0f + 0.3f * i, 0.5f * i, 520.0f + 70.0f * i, 7.0f + 2.0f * i, 700.0f - 35.0f * i); g_treeW[i][5] *= 3.0f; mT(g_treeW[i], g_treeWe[i]); }
    mWorld(g_waterW, 1.0f, 0.3f, 880.0f, 2.0f, 480.0f);
    for (int q = 0; q < NQ; ++q) mWorld(g_ffW[q], 1.0f, 0.4f * q, 640.0f + 60.0f * q, 0.0f, 540.0f + 25.0f * q);

    int ok = rtInstall(dev, g_fxU);
    printf("render thread install: %d\n", ok);
    if (!ok) return 7;
    int stubFail = 0;
    { typedef DWORD (__cdecl* StubsF)(void*, void*, DWORD, DWORD, DWORD, DWORD); StubsF stubs = (StubsF)GetProcAddress(ha, "AotrTweenTestStubs");
      if (!stubs) { printf("missing AotrTweenTestStubs\n"); return 3; }
      DWORD r = stubs((void*)&fakeBeginRender, (void*)&fakeMeshDraw, 0x11112222, 0x33334444, 0x55556666, 0x77778888);
      bool good = r == (0x11112222u ^ 0x33334444u ^ 0x55556666u ^ 0x77778888u) && g_fakeDraw[0] == (0x11112222u ^ 0x5A5A5A5Au) && g_fakeDraw[1] == 0x33334444u && g_fakeDraw[2] == 0x55556666u;
      printf("engine-side stubs (Begin_Render call, mesh draw): %s\n", good ? "arguments and result pass through" : "WRONG");
      if (!good) stubFail = 1; }

    for (int f = 0; f < NF; ++f) buildParams((float)f, g_cur[f]);
    for (int f = 1; f < NF; ++f) buildMid(g_cur[f - 1], g_cur[f], g_mid[f], 0.5f);

    static BYTE* G[NF]; static BYTE* T[NF]; static bool hasT[NF];
    static Params trueMid;
    BYTE* R = (BYTE*)malloc(640 * 480 * 4); BYTE* X = (BYTE*)malloc(640 * 480 * 4);
    for (int f = 0; f < NF; ++f) { G[f] = (BYTE*)malloc(640 * 480 * 4); T[f] = (BYTE*)malloc(640 * 480 * 4); }

    // run A: in-between frames on
    mode(16);
    int lastSeq = grab(NULL, 0);
    for (int f = 0; f < NF; ++f) {
        drawFrame(g_cur[f], true);
        if (!shot(G[f])) return 8;
        present();
        { float m[16]; CALL2(dev, 45, 2, m); }                              // wait for the render thread: the in-between picture exists now
        int seq = grab(T[f], 640 * 480 * 4);
        hasT[f] = seq != lastSeq; lastSeq = seq;
    }
    // a device reset in the middle of it all (what alt-tab does): nothing of the DLL's may be left on the device
    int resetFail = 0, afterReset = 0;
    { CALL0(g_bb, 2); g_bb = NULL;
      CALL0(g_fxU, 69); CALL0(g_fxT, 69); CALL0(g_fxW, 69);                 // OnLostDevice
      HRESULT rh = CALL1(dev, 16, &pp);
      CALL0(g_fxU, 70); CALL0(g_fxT, 70); CALL0(g_fxW, 70);                 // OnResetDevice
      if (FAILED(rh)) { printf("device Reset FAILED 0x%08lX with in-between frames live\n", (unsigned long)rh); resetFail = 1; }
      if (FAILED(CALL4(dev, 18, 0, 0, 0, &g_bb)) || !g_bb) { printf("no back buffer after the reset\n"); return 5; }
      int seq0 = grab(NULL, 0);
      for (int f = 0; f < 8; ++f) { drawFrame(g_cur[f], true); present(); { float m[16]; CALL2(dev, 45, 2, m); } }
      afterReset = grab(NULL, 0) - seq0;
      printf("device reset with in-between frames live: %s; %d in-between pictures in the 8 frames after it\n", resetFail ? "FAILED" : "ok", afterReset); }
    // A frame whose stretch could not be run again and was not even read (queue space ran out in the middle of it).
    // The frame after it must still get its in-between picture: blended from the frame BEFORE the missed one, three
    // quarters of the way (frames 4, 5, 6: 5 is the one missed).
    static BYTE* T2 = (BYTE*)malloc(640 * 480 * 4); bool hasT2 = false;
    { drawFrame(g_cur[3], true); present(); { float m[16]; CALL2(dev, 45, 2, m); }
      drawFrame(g_cur[4], true); present(); { float m[16]; CALL2(dev, 45, 2, m); }
      g_pressInFrame = true; drawFrame(g_cur[5], true); g_pressInFrame = false; present(); { float m[16]; CALL2(dev, 45, 2, m); }
      int s0 = grab(NULL, 0);
      drawFrame(g_cur[6], true); present(); { float m[16]; CALL2(dev, 45, 2, m); }
      hasT2 = grab(T2, 640 * 480 * 4) != s0; }
    static char st[4096]; stats(st, sizeof(st));
    mode(0);
    for (int k = 0; k < 3; ++k) { drawFrame(g_cur[0], false); present(); }  // back to plain frames
    { float m[16]; CALL2(dev, 45, 2, m); }

    // run B: the same frames, and the halfway pictures, drawn directly
    int failures = 0, checks = 0, made = 0, trueOff = 0, trueWorst = 0, trueN = 0;
    ++checks; if (resetFail) ++failures;
    ++checks; if (stubFail) ++failures;
    ++checks; if (afterReset < 5) ++failures;
    for (int f = 0; f < NF; ++f) {
        drawFrame(g_cur[f], false);
        if (!shot(R)) return 8;
        present();
        int d1 = differing(G[f], R); ++checks; if (d1) ++failures;
        int d2 = -1, d3 = -1;
        if (f >= 1 && hasT[f]) {
            drawFrame(g_mid[f], false);
            if (!shot(X)) return 8;
            present();
            d2 = differing(T[f], X); ++checks; if (d2) ++failures;
            d3 = differing(T[f], G[f]); ++checks; if (!d3) ++failures;          // control
            ++made;
            static const int kEvent[] = { 8, 10, 14, 15, 18, 22, 25 };          // a jump, an appearance, a cut: no half-way state to compare with
            bool event = false; for (int e = 0; e < 7; ++e) if (kEvent[e] == f) event = true;
            if (!event) {
                buildParams((float)f - 0.5f, trueMid);
                drawFrame(trueMid, false);
                if (!shot(X)) return 8;
                present();
                int off = 0, worst = 0;
                for (int i = 0; i < 640 * 480 * 4; ++i) { if ((i & 3) == 3) continue; int d = (int)T[f][i] - (int)X[i]; if (d < 0) d = -d; if (d > worst) worst = d; if (d > 24) ++off; }
                ++checks; if (off > 640 * 480 * 3 / 500) ++failures;            // 0.2 %
                trueOff += off; if (worst > trueWorst) trueWorst = worst; ++trueN;
                if (f == 5 && getenv("TWEEN_TEST_KEEP")) { FILE* o = fopen("tween_f5.raw", "wb"); if (o) { fwrite(G[f - 1], 1, 640 * 480 * 4, o); fwrite(T[f], 1, 640 * 480 * 4, o); fwrite(G[f], 1, 640 * 480 * 4, o); fwrite(X, 1, 640 * 480 * 4, o); fclose(o); } }
            }
        }
        printf("frame %2d: game frame %s", f, d1 ? "DIFFERS" : "untouched");
        if (d1) printf(" (%d pixels)", d1);
        if (d2 >= 0) printf(" | in-between picture %s", d2 ? "DIFFERS from the halfway picture" : "is the halfway picture"), d2 ? printf(" (%d pixels)", d2) : 0,
                     printf(" | control: %d pixels away from the frame itself%s", d3, d3 ? "" : "  <-- proves nothing");
        else printf(" | no in-between picture%s", f < 2 ? " (nothing to blend from yet)" : "");
        printf("\n");
    }
    if (made < NF - 4) { printf("only %d in-between pictures were made\n", made); ++failures; }
    { static Params mid2; int dA = -1, dB = -1; bool counted = strstr(st, "across_a_missed_frame=1 ") != NULL && strstr(st, "gave_up=1 ") != NULL;
      ++checks; if (!hasT2 || !counted) ++failures;
      if (hasT2) {
          buildMid(g_cur[4], g_cur[6], mid2, 0.75f);
          drawFrame(mid2, false); if (!shot(X)) return 8; present();
          dA = differing(T2, X); ++checks; if (dA) ++failures;
          drawFrame(g_mid[6], false); if (!shot(X)) return 8; present();
          dB = differing(T2, X); ++checks; if (!dB) ++failures;                 // control: it is not the picture made from frames 5 and 6
      }
      printf("after a frame that could not be read (5): frame 6 %s", !hasT2 ? "got NO in-between picture" : dA ? "has an in-between picture that DIFFERS from the three-quarter blend of frames 4 and 6" :
             "has its in-between picture, the three-quarter blend of frames 4 and 6");
      if (hasT2) printf(dA ? " (%d pixels)" : "", dA), printf(" | control: %d pixels away from the half-way blend of frames 5 and 6%s", dB, dB ? "" : "  <-- proves nothing");
      printf("%s\n", counted ? "" : " | the counters do not show one frame given back and one blend across it"); }
    // ---- pan pictures (aotr_panwarp.inc): a picture drawn from one camera, shown from another -----------------
    // A ground at height gh with a fine, smooth colour field (so that a mapping that is a few pixels off shows), and an
    // "interface" rectangle drawn after the 3D pass is marked as ended. The frame is drawn from camera 1; the DLL is
    // told to show it from camera 2 (one scroll step away); what it put into the real back buffer must be
    //   * the scene the test draws itself from camera 2, wherever the ground maps into the picture (a few levels of
    //     tolerance: the picture is resampled),
    //   * the frame's own interface, exactly, where the interface drew,
    //   * the frame's own pixel where the mapping reaches outside the picture;
    // and, as a control, it must NOT be the frame as it was drawn.
    { typedef void (__cdecl* PanF)(int); typedef void (__cdecl* ForceF)(const float*, const float*, float); typedef int (__cdecl* PGrabF)(BYTE*, int);
      PanF pan = (PanF)GetProcAddress(ha, "AotrPanWarpTest"); ForceF force = (ForceF)GetProcAddress(ha, "AotrPanWarpForce"); PGrabF pgrab = (PGrabF)GetProcAddress(ha, "AotrPanWarpGrab");
      StatsF pstats = (StatsF)GetProcAddress(ha, "AotrPanWarpStats");
      ++checks;
      if (!pan || !force || !pgrab || !pstats) { printf("pan pictures: test exports missing\n"); ++failures; }
      else {
        struct FV { float x, y, z; DWORD c; };
        const int GN = 200; const float cell = 25.0f, gh = 37.0f;
        static FV gv[201 * 201]; static WORD gi[200 * 200 * 6]; int ii = 0;
        for (int y = 0; y <= GN; ++y) for (int x = 0; x <= GN; ++x) {
            FV& q = gv[y * (GN + 1) + x]; q.x = (x - GN / 2) * cell; q.y = (y - GN / 2) * cell; q.z = gh;
            unsigned h = (unsigned)(x * 73856093) ^ (unsigned)(y * 19349663); h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
            q.c = 0xFF000000u | ((60 + (h & 127)) << 16) | ((60 + ((h >> 8) & 127)) << 8) | (60 + ((h >> 16) & 127));
        }
        for (int y = 0; y < GN; ++y) for (int x = 0; x < GN; ++x) { WORD a = (WORD)(y * (GN + 1) + x), b = (WORD)(a + 1), c = (WORD)(a + GN + 1), d = (WORD)(c + 1);
            gi[ii++] = a; gi[ii++] = c; gi[ii++] = b; gi[ii++] = b; gi[ii++] = c; gi[ii++] = d; }
        void* gvb = makeVB(gv, sizeof(gv)); void* gib = makeIB(gi, sizeof(gi));
        float P[16], V1[16], V2[16];
        mPersp(P, 1.35f, 1.8f, 5.0f, 4000.0f);
        { float eye1[3] = { 10.0f, -300.0f, 500.0f + gh }, eye2[3] = { 10.0f + 31.0f, -300.0f + 23.0f, 500.0f + gh }, dir[3] = { 0.0f, 0.7071f, -0.7071f };
          mLookAt(V1, eye1, dir); mLookAt(V2, eye2, dir); }
        static const float kI[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        struct Draw { static void scene(void* dev, void* vb, void* ib, const float* V, const float* Pm, bool mark, int nv, int np) {
            if (mark) g_mark(dev);
            CALL6(dev, 43, 0, 0, 3, 0xFF203040, 0x3F800000, 0);
            CALL0(dev, 41);
            CALL2(dev, 44, 3, Pm); CALL2(dev, 44, 256, kI); CALL2(dev, 44, 2, V);
            CALL1(dev, 92, 0); CALL1(dev, 107, 0); CALL1(dev, 89, 0x002 | 0x040);
            CALL2(dev, 57, 137, 0); CALL2(dev, 57, 22, 1); CALL2(dev, 57, 7, 0); CALL2(dev, 57, 27, 0); CALL2(dev, 65, 0, 0);
            CALL4(dev, 100, 0, vb, 0, sizeof(FV)); CALL1(dev, 104, ib);
            CALL6(dev, 82, 4, 0, 0, nv, 0, np);
            { static const float V2d[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,-1.0f,1 }, P2d[16] = { 2.0f,0,0,0, 0,2.0f,0,0, 0,0,-1.99f,-1, 0,0,-1.98f,0 };
              CALL2(dev, 44, 3, P2d); CALL2(dev, 44, 2, V2d); }          // as the engine: its 2D camera is set right before the 3D pass ends - it must not be taken for the scene's
            if (mark) g_viewsEnd();
            CALL2(dev, 44, 256, kI); CALL2(dev, 44, 2, kI); CALL2(dev, 44, 3, kI);
            FV v[4] = { {0.5f, -0.6f, 0.5f, 0xFFFF00FF}, {0.9f, -0.6f, 0.5f, 0xFFFF00FF}, {0.5f, -0.9f, 0.5f, 0xFFFF00FF}, {0.9f, -0.9f, 0.5f, 0xFFFF00FF} };
            CALL4(dev, 83, 5, 2, v, sizeof(FV));
            CALL0(dev, 42);
        } };
        const int nv = (GN + 1) * (GN + 1), np = GN * GN * 2;
        BYTE* F1 = (BYTE*)malloc(640 * 480 * 4); BYTE* W = (BYTE*)malloc(640 * 480 * 4); BYTE* X2 = (BYTE*)malloc(640 * 480 * 4);
        mode(16); pan(2); force(V2, P, gh);
        int seq0 = pgrab(NULL, 0), seq = seq0;
        for (int k = 0; k < 3; ++k) {
            Draw::scene(dev, gvb, gib, V1, P, true, nv, np);
            if (!shot(F1)) return 8;
            present();
            { float m[16]; CALL2(dev, 45, 2, m); }
            for (int w8 = 0; w8 < 200 && pgrab(NULL, 0) == seq; ++w8) Sleep(5);
            seq = pgrab(W, 640 * 480 * 4);
        }
        static char pst[1024]; pst[0] = 0; pstats(pst, sizeof(pst));
        pan(0); force(NULL, NULL, 0); mode(0);
        for (int k = 0; k < 3; ++k) { Draw::scene(dev, gvb, gib, V1, P, false, nv, np); present(); }
        { float m[16]; CALL2(dev, 45, 2, m); }
        Draw::scene(dev, gvb, gib, V2, P, false, nv, np);
        if (!shot(X2)) return 8;
        present();
        // the mapping, from the DLL's own ground function: camera 2's screen -> camera 1's screen
        float Hm[16]; int mapOk = g_reaim(2, 0, &gh, V2, P, V1, P, Hm);
        int nScene = 0, nSceneOff = 0, worst = 0, nUi = 0, nUiBad = 0, nOut = 0, nOutBad = 0, nMoved = 0, nOff2 = 0, nBlend = 0; double sumAbs = 0, sumAbs2 = 0;
        for (int y = 0; y < 480; ++y) for (int x = 0; x < 640; ++x) {
            const BYTE* w = W + (y * 640 + x) * 4; const BYTE* f = F1 + (y * 640 + x) * 4; const BYTE* r = X2 + (y * 640 + x) * 4;
            bool ui = x >= 480 && x < 608 && y >= 384 && y < 456;
            if (ui) { ++nUi; if (w[0] != f[0] || w[1] != f[1] || w[2] != f[2]) ++nUiBad; continue; }
            float cx = ((float)x + 0.5f) / 320.0f - 1.0f, cy = 1.0f - ((float)y + 0.5f) / 240.0f;
            float sx = cx * Hm[0] + cy * Hm[4] + Hm[12], sy = cx * Hm[1] + cy * Hm[5] + Hm[13], sw = cx * Hm[3] + cy * Hm[7] + Hm[15];
            float u = (sx / sw + 1.0f) * 0.5f, v = (1.0f - sy / sw) * 0.5f;
            float margin = 1.5f / 480.0f;
            if (u < -margin || u > 1.0f + margin || v < -margin || v > 1.0f + margin) { ++nOut; if (w[0] != f[0] || w[1] != f[1] || w[2] != f[2]) ++nOutBad; continue; }
            if (u < margin || u > 1.0f - margin || v < margin || v > 1.0f - margin) continue;          // on the border itself: either
            // (a pixel whose source is under the frame's interface rectangle shows that rectangle's place as the scene copy has it: the scene)
            ++nScene;
            int d = 0; for (int c = 0; c < 3; ++c) { int e = (int)w[c] - (int)r[c]; if (e < 0) e = -e; if (e > d) d = e; sumAbs += e; }
            if (d > worst) worst = d;
            if (d > 8) ++nSceneOff;
            if (x >= 2) { const BYTE* r2 = r - 8; int d2 = 0; for (int c = 0; c < 3; ++c) { int e = (int)w[c] - (int)r2[c]; if (e < 0) e = -e; if (e > d2) d2 = e; sumAbs2 += e; } if (d2 > 8) ++nOff2; }   // against the reference two pixels to the side
            if (w[0] != f[0] || w[1] != f[1] || w[2] != f[2]) ++nMoved;
            // is it one of the game's own pixels? (the frame as drawn, at the place the mapping names, give or take a pixel)
            { int px1 = (int)floorf(u * 640.0f), py1 = (int)floorf(v * 480.0f); bool own = false;
              for (int dy = -1; dy <= 1 && !own; ++dy) for (int dx = -1; dx <= 1 && !own; ++dx) {
                  int qx = px1 + dx, qy = py1 + dy; if (qx < 0 || qx >= 640 || qy < 0 || qy >= 480) continue;
                  if (qx >= 480 && qx < 608 && qy >= 384 && qy < 456) { own = true; break; }      // under the frame's interface: the scene copy has what the frame as drawn does not
                  const BYTE* s1 = F1 + (qy * 640 + qx) * 4; own = s1[0] == w[0] && s1[1] == w[1] && s1[2] == w[2]; }
              if (!own) ++nBlend; }
        }
        bool got = seq != seq0 && mapOk;
        ++checks; if (!got) ++failures;
        double meanErr = nScene ? sumAbs / ((double)nScene * 3.0) : 99.0, meanErr2 = nScene ? sumAbs2 / ((double)nScene * 3.0) : 0.0;
        // Two ways to take the scene from elsewhere. Blended from four pixels ([Engine] PanSharp = 0): the place is
        // right to a fraction of a pixel (2 % of the pixels may be more than 8 levels off, the mean under 1.5) and
        // most pixels are no pixel the game drew. The nearest pixel (the default): every pixel is one the game drew,
        // and the place is right to half a pixel - which this picture, 640 wide with a hard edge every few pixels,
        // shows far more than a 4K frame does (8 %, mean under 3).
        { char e[8]; e[0] = 0; bool sharp = !(GetEnvironmentVariableA("AOTR_PANSHARP", e, sizeof(e)) && e[0] == '0');
          ++checks; if (sharp ? (nBlend > nScene / 500) : (nBlend < nScene / 4)) ++failures;       // its own pixels (0.2 % allowed at the picture's rim) - and, blended, the control: it is not
          printf("pan pictures, %s: %d of %d scene pixels (%.2f %%) are not a pixel of the frame as drawn\n", sharp ? "the nearest pixel taken" : "four pixels blended", nBlend, nScene, nScene ? 100.0 * nBlend / nScene : 0.0);
          if (sharp) { ++checks; if (nSceneOff > nScene * 8 / 100 || meanErr > 3.0) ++failures; }
          else       { ++checks; if (nSceneOff > nScene / 50 || meanErr > 1.5) ++failures; } }
        ++checks; if (nOff2 < nSceneOff * 8 || meanErr2 < meanErr * 3.0) ++failures;      // control: two pixels to the side is clearly worse - the mapping is right to well under that
        ++checks; if (nUiBad) ++failures;
        ++checks; if (nOutBad) ++failures;
        ++checks; if (nMoved < nScene / 2) ++failures;                         // control: the picture really was moved
        printf("pan pictures: a frame drawn from one camera, shown from a camera one scroll step away - %s; of %d scene pixels %d (%.2f %%) are more than 8 levels from the scene drawn from that camera, mean difference %.2f of 255, largest %d "
               "(against that scene moved two pixels sideways: %d pixels, mean %.2f); interface: %d of %d pixels changed; outside the picture: %d pixels, %d not left as they were; control: %d scene pixels differ from the frame as drawn\n",
               got ? "shown" : "NOT shown that way", nScene, nSceneOff, nScene ? 100.0 * nSceneOff / nScene : 0.0, meanErr, worst, nOff2, meanErr2, nUiBad, nUi, nOut, nOutBad, nMoved);
        printf("%s\n", pst);
      }
    }
    // A device reset once pan pictures have been used. They keep textures of their own, and v65 hung here on every
    // minimise: the reset freed them under the render lock, the device's Release (which DXVK calls itself when an
    // object's last reference goes) then waited for the queue, and the queue needs that lock.
    { ++checks;
      mode(16);
      for (int f = 0; f < 3; ++f) { drawFrame(g_cur[f], true); present(); { float m[16]; CALL2(dev, 45, 2, m); } }
      CALL0(g_bb, 2); g_bb = NULL;
      CALL0(g_fxU, 69); CALL0(g_fxT, 69); CALL0(g_fxW, 69);                 // OnLostDevice
      wdArm("the device reset after pan pictures were used");
      HRESULT rh = CALL1(dev, 16, &pp);
      wdOff();
      CALL0(g_fxU, 70); CALL0(g_fxT, 70); CALL0(g_fxW, 70);                 // OnResetDevice
      if (FAILED(CALL4(dev, 18, 0, 0, 0, &g_bb)) || !g_bb) { printf("no back buffer after the second reset\n"); return 5; }
      int seq0 = grab(NULL, 0);
      for (int f = 0; f < 8; ++f) { drawFrame(g_cur[f], true); present(); { float m[16]; CALL2(dev, 45, 2, m); } }
      int after2 = grab(NULL, 0) - seq0;
      if (FAILED(rh) || after2 < 6) ++failures;
      printf("device reset after pan pictures were used: %s; %d in-between pictures in the 8 frames after it\n", FAILED(rh) ? "FAILED" : "ok", after2);
      mode(0); }
    // A dynamic vertex buffer made in the middle of a frame, filled whole, drawn with and given back - then a device
    // reset. The DLL keeps a copy of what such a buffer holds and, with the copy, a reference to the buffer. A reset
    // marked the copy dead but gave the reference back only at some later lock - and a device on which a
    // default-pool buffer is still alive refuses to be reset. The game then asks again every frame, fifty times, and
    // gives up: the crash after alt-tabbing back in (v52 to v69; in the log, the game's own stack walk under
    // game.dat+1222B8 ten seconds after the reset).
    { ++checks;
      mode(16);
      struct DV { float x, y, z; DWORD c; };
      static const float kId[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
      void* dvb = NULL; bool filled = false;
      for (int f = 0; f < 3; ++f) {
          g_mark(dev);
          CALL6(dev, 43, 0, 0, 3, 0xFF203040, 0x3F800000, 0);
          CALL0(dev, 41);
          if (f == 0) {
              CALL6(dev, 26, (UINT)(6 * sizeof(DV)), 0x208 /* D3DUSAGE_DYNAMIC | WRITEONLY */, 0x042, 0 /* D3DPOOL_DEFAULT */, &dvb, 0);
              void* p = NULL;
              if (dvb && SUCCEEDED(CALL4(dvb, 11, 0, 0, &p, 0x2000 /* D3DLOCK_DISCARD */)) && p) {
                  DV v[6] = { { -0.5f, -0.5f, 0.5f, 0xFF40A0FF }, { -0.5f, 0.5f, 0.5f, 0xFF40A0FF }, { 0.5f, 0.5f, 0.5f, 0xFF40A0FF },
                              { -0.5f, -0.5f, 0.5f, 0xFFFFA040 }, { 0.5f, 0.5f, 0.5f, 0xFFFFA040 }, { 0.5f, -0.5f, 0.5f, 0xFFFFA040 } };
                  memcpy(p, v, sizeof(v)); CALL0(dvb, 12); filled = true;
              }
          }
          if (filled) {
              CALL2(dev, 44, 3, kId); CALL2(dev, 44, 256, kId); CALL2(dev, 44, 2, kId);
              CALL1(dev, 92, 0); CALL1(dev, 107, 0); CALL1(dev, 89, 0x042);
              CALL2(dev, 57, 137, 0); CALL2(dev, 57, 22, 1); CALL2(dev, 57, 7, 0); CALL2(dev, 57, 27, 0); CALL2(dev, 65, 0, 0);
              CALL4(dev, 100, 0, dvb, 0, sizeof(DV)); CALL3(dev, 81, 4, 0, 2);
          }
          CALL0(dev, 42);
          present(); { float m[16]; CALL2(dev, 45, 2, m); }
      }
      CALL4(dev, 100, 0, 0, 0, 0);                                          // unbound and given back, as the engine does with its dynamic buffers before a reset
      if (dvb) CALL0(dvb, 2);
      CALL0(g_bb, 2); g_bb = NULL;
      CALL0(g_fxU, 69); CALL0(g_fxT, 69); CALL0(g_fxW, 69);                 // OnLostDevice
      wdArm("the device reset after a dynamic buffer was filled inside a frame");
      HRESULT rh = CALL1(dev, 16, &pp);
      wdOff();
      printf("device reset after a dynamic buffer was made and filled inside a frame: %s (0x%08lX)%s\n", FAILED(rh) ? "REFUSED by the device" : "ok", (unsigned long)rh, filled ? "" : " - the buffer could not be made, nothing was tested");
      if (FAILED(rh)) { printf("FAILED: the device is left unreset - something of the DLL's still holds one of its default-pool objects\n"); return 1; }
      if (!filled) ++failures;
      CALL0(g_fxU, 70); CALL0(g_fxT, 70); CALL0(g_fxW, 70);                 // OnResetDevice
      if (FAILED(CALL4(dev, 18, 0, 0, 0, &g_bb)) || !g_bb) { printf("no back buffer after the third reset\n"); return 5; }
      for (int f = 0; f < 4; ++f) { drawFrame(g_cur[f], true); present(); { float m[16]; CALL2(dev, 45, 2, m); } }
      mode(0); }
    printf("against the true half-way state, %d frames: %d colour values visibly off out of %d (%.4f %%), largest difference %d of 255\n",
           trueN, trueOff, trueN * 640 * 480 * 3, trueN ? 100.0 * trueOff / ((double)trueN * 640 * 480 * 3) : 0.0, trueWorst);
    printf("in-between frames: %s\n", st);
    { StatsF ds = (StatsF)GetProcAddress(ha, "AotrDevPassStats"); static char dst[2048]; if (ds) { ds(dst, sizeof(dst)); printf("%s\n", dst); } }
    printf("%s: %d of %d comparisons as they must be, %d in-between pictures\n", failures ? "FAILED" : "PASSED", checks - failures, checks, made);
    return failures ? 1 : 0;
}
