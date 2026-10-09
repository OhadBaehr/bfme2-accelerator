// merge_test.cpp - offline proof for aotr_merge.inc, on the Direct3D the game runs on (DXVK d3d9.dll beside it).
//
// k meshes of one model, each with its own joints, are drawn twice:
//   A. one draw per mesh, each with its own palette                          - what the engine does today
//   B. ONE draw of k copies from the multi-copy twin, palette = all meshes   - what aotr_merge.inc makes of it
// through an effect that skins the way the game's shaders do (a palette of quaternion + position entries indexed by
// the vertex's BLENDINDICES bytes). The two pictures must be the same, pixel for pixel. The twin is built by the
// DLL's own builder through its own queued record, and bound through its own buffer swap in the device hooks.
//
// Models: one joint a mesh; three joints a mesh, one per vertex; three joints a mesh, TWO per vertex with a weight
// (the joint DWORD is then slot2 << 8 | slot1, as the engine's builder writes it). k = 1..8, 16-bit indices.
// Also checked: single draws through the swap are the single draws without it (copy 0 is the original), and a
// control with one mesh given the wrong joints, which must NOT match.
#include <windows.h>
#include <stdio.h>
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

static const char kFx[] =
"float4x4 ViewProj; float4 Pal[200];\n"                                           // 100 entries: quaternion, then position
"struct VSIn { float4 pos : POSITION; float3 nrm : NORMAL; float4 idx : BLENDINDICES; float w : TEXCOORD0; };\n"
"struct VSOut { float4 pos : POSITION; float3 col : TEXCOORD0; };\n"
"float3 Rot(float4 q, float3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }\n"
"VSOut VS1(VSIn i) { VSOut o; int j = (int)i.idx.x * 2; float4 q = Pal[j]; float3 t = Pal[j + 1].xyz;\n"
"  float3 w = Rot(q, i.pos.xyz) + t; float3 n = Rot(q, i.nrm);\n"
"  o.pos = mul(float4(w, 1), ViewProj); o.col = n * 0.5 + 0.5; return o; }\n"
"VSOut VS2(VSIn i) { VSOut o; int j = (int)i.idx.x * 2; int k = (int)i.idx.y * 2;\n"
"  float4 q0 = Pal[j]; float3 t0 = Pal[j + 1].xyz; float4 q1 = Pal[k]; float3 t1 = Pal[k + 1].xyz;\n"
"  float3 w = lerp(Rot(q1, i.pos.xyz) + t1, Rot(q0, i.pos.xyz) + t0, i.w); float3 n = lerp(Rot(q1, i.nrm), Rot(q0, i.nrm), i.w);\n"
"  o.pos = mul(float4(w, 1), ViewProj); o.col = n * 0.5 + 0.5; return o; }\n"
"float4 PS(VSOut i) : COLOR { return float4(i.col, 1); }\n"
"technique T1 { pass p0 { VertexShader = compile vs_2_0 VS1(); PixelShader = compile ps_2_0 PS(); ZEnable = TRUE; CullMode = NONE; AlphaBlendEnable = FALSE; } }\n"
"technique T2 { pass p0 { VertexShader = compile vs_2_0 VS2(); PixelShader = compile ps_2_0 PS(); ZEnable = TRUE; CullMode = NONE; AlphaBlendEnable = FALSE; } }\n";

struct Vtx { float x, y, z, nx, ny, nz; DWORD joint; float weight; float pad[4]; };  // 48 bytes, like the game's skins
static unsigned g_seed = 12345;
static float frand() { g_seed = g_seed * 1664525u + 1013904223u; return (float)((g_seed >> 8) & 0xFFFF) / 65535.0f; }

static void matMul(const float* a, const float* b, float* o) { float t[16]; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { float s = 0; for (int k = 0; k < 4; ++k) s += a[r*4+k] * b[k*4+c]; t[r*4+c] = s; } memcpy(o, t, 64); }
static void viewProj(float* out) {
    float v[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,26,1 };
    float zn = 1, zf = 100, h = 1.6f, w = 1.2f;
    float p[16] = { w,0,0,0, 0,h,0,0, 0,0,zf/(zf-zn),1, 0,0,-zn*zf/(zf-zn),0 };
    matMul(v, p, out);
}
// a cube whose faces are spread over nJoints joints; with two joints a vertex each corner blends two of them
static void makeCube(Vtx* v, WORD* idx, int nJoints, int jpv) {
    static const float n[6][3] = {{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
    int vi = 0, ii = 0;
    for (int f = 0; f < 6; ++f) {
        float nx = n[f][0], ny = n[f][1], nz = n[f][2];
        float ux = ny ? 1.0f : (nz ? 1.0f : 0.0f), uy = 0, uz = ny ? 0.0f : (nx ? 1.0f : 0.0f);
        float wx = ny*uz - nz*uy, wy = nz*ux - nx*uz, wz = nx*uy - ny*ux;
        for (int k = 0; k < 4; ++k) {
            float a = (k == 0 || k == 3) ? -1.0f : 1.0f, b = (k < 2) ? -1.0f : 1.0f;
            Vtx& q = v[vi + k]; memset(&q, 0, sizeof(q));
            q.x = nx + a*ux + b*wx; q.y = ny + a*uy + b*wy; q.z = nz + a*uz + b*wz; q.nx = nx; q.ny = ny; q.nz = nz;
            DWORD s1 = (DWORD)(f % nJoints), s2 = (DWORD)((f + 1 + k) % nJoints);
            q.joint = jpv == 2 ? ((s2 << 8) | s1) : s1;                             // as 0x0058C05A writes it
            q.weight = jpv == 2 ? 0.25f + 0.2f * (float)k : 1.0f;
        }
        WORD b0 = (WORD)vi; static const WORD t[6] = {0,1,2,0,2,3};
        for (int k = 0; k < 6; ++k) idx[ii++] = (WORD)(b0 + t[k]);
        vi += 4;
    }
}
// joint transforms of mesh m: quaternion (normalised) + position + 0, 32 bytes an entry
static void makePalette(float* pal, int mesh, int nJoints) {
    for (int j = 0; j < nJoints; ++j) {
        float q[4] = { frand() - 0.5f, frand() - 0.5f, frand() - 0.5f, frand() + 0.2f };
        float l = (float)sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        float* e = pal + j * 8;
        e[0] = q[0]/l; e[1] = q[1]/l; e[2] = q[2]/l; e[3] = q[3]/l;
        e[4] = (float)((mesh % 4) - 1.5f) * 5.0f + frand(); e[5] = (float)((mesh / 4) - 0.5f) * 6.0f + frand(); e[6] = frand() * 4.0f + j * 0.7f; e[7] = 0;
    }
}

typedef int (__cdecl* InstallF)(void*, void*);
typedef int (__cdecl* BuildF)(void*, void*, void*, void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);
typedef void (__cdecl* SwapF)(int);

static void* g_dev, *g_fx, *g_decl, *g_bb, *g_sys;
static DWORD g_hPal, g_hVP, g_tech[3];
static BYTE* g_shot[2];

// draw: either k single draws (each its own palette) or one draw of k copies; then read the back buffer into shot[which]
static bool render(int which, bool merged, void* vb, void* ib, const float* pal, int k, int nJoints, int jpv, DWORD V, DWORD P) {
    void* dev = g_dev; void* fx = g_fx;
    CALL6(dev, 43, 0, 0, 3, 0xFF203040, 0x3F800000, 0);                             // Clear target + z
    CALL0(dev, 41);                                                                 // BeginScene
    ((HRESULT(WINAPI*)(void*, DWORD))vt(fx)[58])(fx, g_tech[jpv]);                  // SetTechnique
    UINT passes = 0;
    ((HRESULT(WINAPI*)(void*, UINT*, DWORD))vt(fx)[63])(fx, &passes, 0);            // Begin
    CALL1(fx, 64, 0);                                                               // BeginPass
    CALL1(dev, 87, g_decl);                                                         // SetVertexDeclaration
    typedef HRESULT (WINAPI* RawF)(void*, DWORD, const void*, UINT, UINT);
    if (!merged) {
        for (int m = 0; m < k; ++m) {
            ((RawF)vt(fx)[78])(fx, g_hPal, pal + m * nJoints * 8, 0, (UINT)(nJoints * 32));
            CALL0(fx, 65);                                                          // CommitChanges
            CALL4(dev, 100, 0, vb, 0, sizeof(Vtx));                                 // the engine sets these for every draw
            CALL1(dev, 104, ib);
            CALL6(dev, 82, 4, 0, 0, V, 0, P);                                       // DrawIndexedPrimitive
        }
    } else {
        ((RawF)vt(fx)[78])(fx, g_hPal, pal, 0, (UINT)(k * nJoints * 32));
        CALL0(fx, 65);
        CALL4(dev, 100, 0, vb, 0, sizeof(Vtx));
        CALL1(dev, 104, ib);
        CALL6(dev, 82, 4, 0, 0, V * k, 0, P * k);                                   // k times the counts, as 0x0054A1F0 does
    }
    CALL0(fx, 66); CALL0(fx, 67);                                                   // EndPass, End
    CALL0(dev, 42);                                                                 // EndScene
    if (FAILED(CALL2(dev, 32, g_bb, g_sys))) { printf("GetRenderTargetData failed\n"); return false; }
    LR lr = { 0, NULL };
    if (FAILED(CALL3(g_sys, 13, &lr, 0, 0x10)) || !lr.pBits) { printf("LockRect failed\n"); return false; }
    for (int y = 0; y < 480; ++y) memcpy(g_shot[which] + y * 640 * 4, (BYTE*)lr.pBits + y * lr.Pitch, 640 * 4);
    CALL0(g_sys, 14);
    return true;
}
static int covered(const BYTE* shot) { int n = 0; for (int i = 0; i < 640 * 480; ++i) if ((*(const DWORD*)(shot + i * 4) & 0xFFFFFF) != 0x203040) ++n; return n; }

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    SetEnvironmentVariableA("DXVK_CONFIG", "dxvk.hud=");
    SetEnvironmentVariableA("DXVK_LOG_LEVEL", "none");
    SetEnvironmentVariableA("DXVK_STATE_CACHE", "0");
    HMODULE hx = LoadLibraryA("d3dx9_27.dll"); if (!hx) { printf("no d3dx9_27\n"); return 3; }
    HMODULE hd3d = LoadLibraryA("d3d9.dll"); if (!hd3d) { printf("no d3d9\n"); return 3; }
    HMODULE ha = LoadLibraryA("bfme2_accel.new.dll"); if (!ha) { printf("no bfme2_accel.new.dll (%lu)\n", GetLastError()); return 3; }
    InstallF rtInstall = (InstallF)GetProcAddress(ha, "AotrRtTestInstall");
    BuildF mgBuild = (BuildF)GetProcAddress(ha, "AotrMgTestBuild");
    SwapF mgSwap = (SwapF)GetProcAddress(ha, "AotrMgTestSwap");
    if (!rtInstall || !mgBuild || !mgSwap) { printf("missing test exports\n"); return 3; }
    Sleep(1500);                                                                    // the DLL's own init thread

    typedef void* (WINAPI* Create9F)(UINT);
    typedef HRESULT (WINAPI* CreateEffectF)(void*, const char*, UINT, void*, void*, DWORD, void*, void**, void**);
    Create9F Create9 = (Create9F)GetProcAddress(hd3d, "Direct3DCreate9");
    CreateEffectF CreateEffect = (CreateEffectF)GetProcAddress(hx, "D3DXCreateEffect");
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "mergetest";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "mergetest", "mt", WS_POPUP, -3000, -3000, 640, 480, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    void* d3d = Create9(32); if (!d3d) { printf("Direct3DCreate9 failed\n"); return 4; }
    PP pp = {0}; pp.BackBufferWidth = 640; pp.BackBufferHeight = 480; pp.BackBufferFormat = 21; pp.BackBufferCount = 1;
    pp.SwapEffect = 1; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE; pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = 75; pp.PresentationInterval = 0x80000000;
    void* dev = NULL;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, DWORD, HWND, DWORD, PP*, void**))vt(d3d)[16])(d3d, 0, 1, hwnd, 0x40u | 0x2u | 0x4u, &pp, &dev);
    if (FAILED(hr) || !dev) { printf("CreateDevice hr=0x%08lX\n", (unsigned long)hr); return 4; }
    void* fx = NULL, *err = NULL;
    hr = CreateEffect(dev, kFx, (UINT)strlen(kFx), NULL, NULL, 0, NULL, &fx, &err);
    if (FAILED(hr) || !fx) { printf("D3DXCreateEffect hr=0x%08lX %s\n", (unsigned long)hr, err ? (char*)((void*(WINAPI*)(void*))vt(err)[3])(err) : ""); return 5; }
    g_dev = dev; g_fx = fx;
    g_hPal = ((DWORD(WINAPI*)(void*, DWORD, const char*))vt(fx)[9])(fx, 0, "Pal");
    g_hVP = ((DWORD(WINAPI*)(void*, DWORD, const char*))vt(fx)[9])(fx, 0, "ViewProj");
    g_tech[1] = ((DWORD(WINAPI*)(void*, const char*))vt(fx)[13])(fx, "T1");
    g_tech[2] = ((DWORD(WINAPI*)(void*, const char*))vt(fx)[13])(fx, "T2");
    if (!g_hPal || !g_hVP || !g_tech[1] || !g_tech[2]) { printf("handles missing\n"); return 5; }

    // position, normal, BLENDINDICES as UBYTE4 at offset 24, a weight as TEXCOORD0 at 28
    static const BYTE kDecl[] = { 0,0, 0,0, 2, 0, 0, 0,    0,0, 12,0, 2, 0, 3, 0,    0,0, 24,0, 5, 0, 2, 0,    0,0, 28,0, 0, 0, 5, 0,    0xFF,0, 0,0, 17, 0, 0, 0 };
    if (FAILED(CALL2(dev, 86, kDecl, &g_decl)) || !g_decl) { printf("CreateVertexDeclaration failed\n"); return 5; }
    if (FAILED(((HRESULT(WINAPI*)(void*, UINT, UINT, DWORD, DWORD, void**, void*))vt(dev)[36])(dev, 640, 480, 21, 2, &g_sys, NULL)) || !g_sys) { printf("no system surface\n"); return 5; }
    if (FAILED(CALL4(dev, 18, 0, 0, 0, &g_bb)) || !g_bb) { printf("no back buffer\n"); return 5; }
    g_shot[0] = (BYTE*)malloc(640 * 480 * 4); g_shot[1] = (BYTE*)malloc(640 * 480 * 4);

    int ok = rtInstall(dev, fx);
    printf("render thread install: %d\n", ok);
    if (!ok) return 7;

    float vp[16]; viewProj(vp);
    ((HRESULT(WINAPI*)(void*, DWORD, const float*))vt(fx)[38])(fx, g_hVP, vp);      // SetMatrix

    static const struct { int nJoints, jpv; const char* what; } kModels[3] = {
        { 1, 1, "one joint a mesh" }, { 3, 1, "three joints a mesh, one per vertex" }, { 3, 2, "three joints a mesh, two per vertex" } };
    int failures = 0, checks = 0;
    for (int mi = 0; mi < 3; ++mi) {
        const int nJoints = kModels[mi].nJoints, jpv = kModels[mi].jpv;
        Vtx cube[24]; WORD idx[36];
        makeCube(cube, idx, nJoints, jpv);
        void* vb = NULL, *ib = NULL, *p = NULL;
        CALL6(dev, 26, sizeof(cube), 8, 0, 1, &vb, 0);                              // WRITEONLY, MANAGED - static, like a model's
        CALL4(vb, 11, 0, 0, &p, 0); memcpy(p, cube, sizeof(cube)); CALL0(vb, 12);
        CALL6(dev, 27, sizeof(idx), 8, 101, 1, &ib, 0);
        CALL4(ib, 11, 0, 0, &p, 0); memcpy(p, idx, sizeof(idx)); CALL0(ib, 12);
        float pal[9 * 3 * 8];
        for (int m = 0; m < 9; ++m) makePalette(pal + m * nJoints * 8, m, nJoints);

        // without the swap: the engine's picture for k = 1..8
        static BYTE* ref[9];
        mgSwap(0);
        for (int k = 1; k <= 8; ++k) {
            if (!render(0, false, vb, ib, pal, k, nJoints, jpv, 24, 12)) return 8;
            if (!ref[k]) ref[k] = (BYTE*)malloc(640 * 480 * 4);
            memcpy(ref[k], g_shot[0], 640 * 480 * 4);
        }
        int K = mgBuild(dev, vb, ib, g_decl, 24, 12, (DWORD)nJoints, sizeof(Vtx), 8, 0x7E570000u + (DWORD)mi, (DWORD)jpv);
        printf("%s: twin built with %d copies%s\n", kModels[mi].what, K, K >= 2 ? "" : "  <-- REFUSED");
        if (K < 2) { ++failures; continue; }
        mgSwap(1);
        for (int k = 1; k <= K; ++k) {
            // one draw per mesh, through the swap: copy 0 of the twin must be the original
            if (!render(1, false, vb, ib, pal, k, nJoints, jpv, 24, 12)) return 8;
            bool same1 = memcmp(ref[k], g_shot[1], 640 * 480 * 4) == 0; ++checks; if (!same1) ++failures;
            // ONE draw of k copies
            if (!render(1, true, vb, ib, pal, k, nJoints, jpv, 24, 12)) return 8;
            bool same2 = memcmp(ref[k], g_shot[1], 640 * 480 * 4) == 0; ++checks; if (!same2) ++failures;
            int diff = 0; for (int i = 0; i < 640 * 480; ++i) if (*(DWORD*)(ref[k] + i * 4) != *(DWORD*)(g_shot[1] + i * 4)) ++diff;
            printf("   %d mesh(es): %6d pixels covered | single draws through the swap %s | one shared draw %s\n", k, covered(ref[k]),
                   same1 ? "IDENTICAL" : "DIFFER", same2 ? "IDENTICAL" : "DIFFERS");
            if (!same2) printf("      %d pixels differ\n", diff);
        }
        // negative control: the same shared draw with mesh 1 given mesh 8's joints (a place it does not belong) must NOT match
        { float bad[9 * 3 * 8]; memcpy(bad, pal, sizeof(bad));
          memcpy(bad + nJoints * 8, pal + 8 * nJoints * 8, nJoints * 32);
          if (!render(1, true, vb, ib, bad, 2, nJoints, jpv, 24, 12)) return 8;
          bool differs = memcmp(ref[2], g_shot[1], 640 * 480 * 4) != 0; ++checks; if (!differs) ++failures;
          printf("   control (second mesh given the wrong joints): %s\n", differs ? "differs, as it must" : "IDENTICAL - the comparison proves nothing"); }
        mgSwap(0);
    }
    printf("%s: %d of %d comparisons as they must be\n", failures ? "FAILED" : "PASSED", checks - failures, checks);
    return failures ? 1 : 0;
}
