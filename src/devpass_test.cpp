// devpass_test.cpp - the second pass at the device (aotr_devpass.inc) on the GAME'S OWN compiled effect.
//   devpass_test.exe <defaultw3d.fxo> <out.txt> [frames]
// tween_test.cpp proves the blending on effects written for the test. This one loads the effect the game draws its
// units and buildings with (shaders picked from arrays by expressions, a preshader fed by View and Projection, a
// 90-joint palette, materials applied from parameter blocks) and drives it the way the engine does:
//   per batch:  ApplyParameterBlock(material), SetTechnique, Begin(flags 6), BeginPass(0),
//   per mesh:   SetInt NumJointsPerVertex, SetRawValue WorldBones, SetMatrixTranspose World, CommitChanges, draw,
//   then        EndPass, End.
// Skinned meshes with one and two joints a vertex, rigid meshes, three materials, the low-detail technique with its own
// palette; everything moves from frame to frame; the camera stands still except for a few frames in the middle (there
// a pass at the device has to stop and be made by D3DX, because the camera feeds the preshader).
// The test writes, per frame, a hash of the game's frame and of the in-between picture the DLL made ahead of it.
// Run with AOTR_DEVPASS=0 (in-between pictures made by D3DX) and with AOTR_DEVPASS=2 (made at the device): the two
// files must be the same, line for line. With AOTR_DEVPASS=3 every pass is made by D3DX and compared register by
// register with what the device pass would have issued (the "devpass:" line: differences must be 0).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <tlhelp32.h>

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

static void mIdent(float* m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void mT(const float* a, float* o) { float t[16]; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) t[r*4+c] = a[c*4+r]; memcpy(o, t, 64); }
static void mInv(const float* a, float* o) {
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
static void mLookAt(float* m, const float* eye, const float* dir) {
    float z[3] = { dir[0], dir[1], dir[2] }; float zl = (float)sqrt(z[0]*z[0] + z[1]*z[1] + z[2]*z[2]); z[0] /= zl; z[1] /= zl; z[2] /= zl;
    float x[3] = { z[2], 0, -z[0] }; float xl = (float)sqrt(x[0]*x[0] + x[2]*x[2]); x[0] /= xl; x[2] /= xl;
    float y[3] = { z[1]*x[2] - z[2]*x[1], z[2]*x[0] - z[0]*x[2], z[0]*x[1] - z[1]*x[0] };
    float t[16] = { x[0], y[0], z[0], 0,  x[1], y[1], z[1], 0,  x[2], y[2], z[2], 0,
                    -(x[0]*eye[0] + x[1]*eye[1] + x[2]*eye[2]), -(y[0]*eye[0] + y[1]*eye[1] + y[2]*eye[2]), -(z[0]*eye[0] + z[1]*eye[1] + z[2]*eye[2]), 1 };
    memcpy(m, t, 64);
}
static void mPersp(float* m, float w, float h, float zn, float zf) { float t[16] = { w,0,0,0, 0,h,0,0, 0,0,zf/(zf-zn),1, 0,0,-zn*zf/(zf-zn),0 }; memcpy(m, t, 64); }
static void mWorld(float* m, float sc, float ang, float x, float y, float z) { mIdent(m); float c = (float)cos(ang) * sc, s = (float)sin(ang) * sc; m[0] = c; m[2] = -s; m[5] = sc; m[8] = s; m[10] = c; m[12] = x; m[13] = y; m[14] = z; }
static void quatAxis(float* q, float ax, float ay, float az, float ang) { float s = (float)sin(ang * 0.5f); q[0] = ax * s; q[1] = ay * s; q[2] = az * s; q[3] = (float)cos(ang * 0.5f); }

// 76 bytes: two positions, two normals, joint indices (as a colour: the shaders scale them by 510), a weight, two
// texture coordinates, a colour - a superset of what any of the effect's vertex shaders declares
struct Vtx { float p0[3], p1[3], n0[3], n1[3]; DWORD joints; float weight; float uv0[2], uv1[2]; DWORD color; };
static void makeCube(Vtx* v, WORD* idx, int nJoints) {
    static const float n[6][3] = {{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
    int vi = 0, ii = 0;
    for (int f = 0; f < 6; ++f) {
        float nx = n[f][0], ny = n[f][1], nz = n[f][2];
        float ux = ny ? 1.0f : (nz ? 1.0f : 0.0f), uy = 0, uz = ny ? 0.0f : (nx ? 1.0f : 0.0f);
        float wx = ny*uz - nz*uy, wy = nz*ux - nx*uz, wz = nx*uy - ny*ux;
        for (int k = 0; k < 4; ++k) {
            float a = (k == 0 || k == 3) ? -1.0f : 1.0f, b = (k < 2) ? -1.0f : 1.0f;
            Vtx& q = v[vi + k]; memset(&q, 0, sizeof(q));
            q.p0[0] = q.p1[0] = (nx + a*ux + b*wx) * 7.0f; q.p0[1] = q.p1[1] = (ny + a*uy + b*wy) * 7.0f; q.p0[2] = q.p1[2] = (nz + a*uz + b*wz) * 7.0f;
            q.n0[0] = q.n1[0] = nx; q.n0[1] = q.n1[1] = ny; q.n0[2] = q.n1[2] = nz;
            DWORD j0 = (DWORD)(f % nJoints), j1 = (DWORD)((f + 1) % nJoints);
            q.joints = j0 | (j1 << 8);                                       // first byte in memory: the first joint (the shaders read .z and .y of the colour)
            q.weight = 0.75f; q.uv0[0] = q.uv1[0] = (a + 1) * 0.5f; q.uv0[1] = q.uv1[1] = (b + 1) * 0.5f; q.color = 0xFFFFFFFF;
        }
        static const WORD t[6] = {0,1,2,0,2,3};
        for (int k = 0; k < 6; ++k) idx[ii++] = (WORD)(vi + t[k]);
        vi += 4;
    }
}
static void* g_dev, *g_fx, *g_decl, *g_bb, *g_sys, *g_vb[3], *g_ib[3];
static void* makeVB(const void* data, UINT bytes) { void* vb = NULL, *p = NULL; CALL6(g_dev, 26, bytes, 8, 0, 1, &vb, 0); CALL4(vb, 11, 0, 0, &p, 0); memcpy(p, data, bytes); CALL0(vb, 12); return vb; }
static void* makeIB(const void* data, UINT bytes) { void* ib = NULL, *p = NULL; CALL6(g_dev, 27, bytes, 8, 101, 1, &ib, 0); CALL4(ib, 11, 0, 0, &p, 0); memcpy(p, data, bytes); CALL0(ib, 12); return ib; }
static bool shot(BYTE* dst) {
    if (FAILED(CALL2(g_dev, 32, g_bb, g_sys))) return false;
    LR lr = { 0, NULL };
    if (FAILED(CALL3(g_sys, 13, &lr, 0, 0x10)) || !lr.pBits) return false;
    for (int y = 0; y < 480; ++y) memcpy(dst + y * 640 * 4, (BYTE*)lr.pBits + y * lr.Pitch, 640 * 4);
    CALL0(g_sys, 14);
    return true;
}
static DWORD fnv(const BYTE* p, size_t n) { DWORD h = 0x811C9DC5u; for (size_t i = 0; i < n; ++i) if ((i & 3) != 3) h = (h ^ p[i]) * 0x01000193u; return h; }
typedef void (__cdecl* MarkF)(void*); typedef void (__cdecl* TagF)(void*, int);
static MarkF g_mark; static TagF g_tag; static void (__cdecl* g_viewsEnd)();
typedef HRESULT (WINAPI* MatF)(void*, DWORD, const float*);
typedef HRESULT (WINAPI* RawF)(void*, DWORD, const void*, UINT, UINT);
typedef HRESULT (WINAPI* ValF)(void*, DWORD, const void*, UINT);
static DWORD H(const char* name) { return ((DWORD(WINAPI*)(void*, DWORD, const char*))vt(g_fx)[9])(g_fx, 0, name); }
static void setMT(DWORD h, const float* m) { if (h) ((MatF)vt(g_fx)[44])(g_fx, h, m); }
static void setVal(DWORD h, const void* p, UINT n) { if (h) ((ValF)vt(g_fx)[20])(g_fx, h, p, n); }
static void setInt(DWORD h, int v) { if (h) CALL2(g_fx, 26, h, v); }
static void setBool(DWORD h, int v) { if (h) CALL2(g_fx, 22, h, v); }
static void setFloat(DWORD h, float v) { if (h) ((HRESULT(WINAPI*)(void*, DWORD, float))vt(g_fx)[30])(g_fx, h, v); }
static void setTex(DWORD h, void* t) { if (h) CALL2(g_fx, 52, h, t); }

static struct { DWORD joints, world, bones, bonesL, view, proj, viewI, time, ambient, dirLight, numShadows, colAmb, colDif, colSpec, shin, colEmi, opacity, numTex, tex0, tex1,
                depthW, alphaT, cull, blend, shroudTex, cloudTex, shadowTex, houseColor, opacityOv; DWORD tech[3]; } h;
static DWORD g_block[3];
static int g_reps = 1;
static bool g_dense = false;
static void palette(int unit, float f, int joints, float* pal) {             // quaternion, position, 0 for every joint
    for (int j = 0; j < joints; ++j) {
        float x = 540.0f + 30.0f * (float)(unit % 6) + (1.5f + 0.2f * (float)(unit % 20)) * f + 3.0f * (float)(j % 3), y = 8.0f + 9.0f * (float)(j % 3), z = 420.0f + 34.0f * (float)((unit / 6) % 4) + (0.8f - 0.1f * (float)(unit % 20)) * f;
        float* q = pal + j * 8; memset(q, 0, 32);
        quatAxis(q, 0, 1, 0, 0.1f * unit + 0.06f * f + 0.3f * j);
        q[4] = x; q[5] = y; q[6] = z;
    }
}
static void drawFrame(float f, float camF, bool mark) {
    void* dev = g_dev, *fx = g_fx; UINT np = 0;
    static const float kI[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    float eye[3] = { 600.0f + 5.0f * camF, 330.0f, 170.0f + 2.0f * camF }, dir[3] = { 0.0f, -0.72f, 0.69f }, V[16], P[16], VI[16], t[16];
    mLookAt(V, eye, dir); mPersp(P, 1.35f, 1.8f, 5.0f, 4000.0f); mInv(V, VI);
    if (g_dense) { static const float kS[16] = { 0.013f, 0.0071f, -0.0033f, 0.0019f,  -0.0057f, 0.021f, 0.0043f, -0.0027f,  0.0031f, -0.0049f, 0.0067f, 0.0011f,  0.37f, -0.53f, 0.71f, 0.29f };
                   for (int i = 0; i < 16; ++i) P[i] += kS[i]; }
    if (mark) g_mark(dev);
    CALL6(dev, 43, 0, 0, 3, 0xFF182028, 0x3F800000, 0);
    CALL0(dev, 41);
    CALL2(dev, 44, 3, P); CALL2(dev, 44, 256, kI); CALL2(dev, 44, 2, V);
    // the frame's own values, once
    mT(V, t); setMT(h.view, t); mT(P, t); setMT(h.proj, t); mT(VI, t); setMT(h.viewI, t);
    setFloat(h.time, 0.25f * f);
    CALL1(dev, 87, g_decl);
    static const float kIdentPal[8] = { 0, 0, 0, 1, 0, 0, 0, 0 };
    int unit = 0;
    for (int rb = 0; rb < 5 * g_reps; ++rb) {
        int batch = rb % 5;
        int tech = batch == 3 ? 1 : (batch == 4 ? 2 : 0);                    // Default, Default, Default, Default_L, Default_M
        if (!h.tech[tech]) { unit += 4; continue; }
        int jpv = batch == 0 ? 1 : (batch == 1 ? 2 : (batch == 2 ? 0 : 1));  // joints a vertex
        CALL1(fx, 75, g_block[batch % 3]);                                  // the batch's material
        CALL1(fx, 58, h.tech[tech]);
        if (FAILED(((HRESULT(WINAPI*)(void*, UINT*, DWORD))vt(fx)[63])(fx, &np, 6))) { unit += 4; continue; }
        if (FAILED(CALL1(fx, 64, 0))) { CALL0(fx, 67); unit += 4; continue; }
        for (int m = 0; m < 4; ++m, ++unit) {
            g_tag((void*)(ULONG_PTR)(0x4000u + (DWORD)unit * 0x40), 1);
            setInt(h.joints, jpv);
            DWORD hb = tech == 1 && h.bonesL ? h.bonesL : h.bones;
            if (jpv) { float pal[56 * 8]; int nj = 24 + (unit * 7) % 30; if (tech == 1 && nj > 32) nj = 32; palette(unit, f, nj, pal); ((RawF)vt(fx)[78])(fx, hb, pal, 0, (UINT)(nj * 32)); setMT(h.world, kI); }
            else { float w[16], wt[16]; ((RawF)vt(fx)[78])(fx, hb, kIdentPal, 0, 32);
                   mWorld(w, 1.0f, 0.2f * unit + 0.05f * f, 560.0f + 28.0f * (float)(unit % 6) + 1.2f * f, 10.0f, 500.0f + 3.0f * (float)unit - 1.1f * f); mT(w, wt); setMT(h.world, wt); }
            CALL0(fx, 65);
            int g = jpv == 2 ? 1 : (jpv == 0 ? 2 : 0);
            CALL4(dev, 100, 0, g_vb[g], 0, sizeof(Vtx)); CALL1(dev, 104, g_ib[g]);
            CALL6(dev, 82, 4, 0, 0, 24, 0, 12);
        }
        CALL0(fx, 66); CALL0(fx, 67);
    }
    if (mark) g_viewsEnd();
    CALL0(dev, 42);
}

// ---- a sampler for the render thread (DEVPASS_TEST_PROFILE): where is its instruction pointer, 15,000 times a second
static volatile LONG g_profRun = 0; static DWORD g_profTid = 0; static DWORD* g_profIp = NULL; static volatile LONG g_profN = 0;
#define PROF_MAX (1 << 20)
static DWORD WINAPI profThread(LPVOID) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_profTid);
    if (!th) return 0;
    LARGE_INTEGER qf, t; QueryPerformanceFrequency(&qf);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (g_profRun) {
        QueryPerformanceCounter(&t); LONGLONG until = t.QuadPart + qf.QuadPart / 15000;
        do { YieldProcessor(); QueryPerformanceCounter(&t); } while (t.QuadPart < until);
        if (g_profRun != 2 || g_profN >= PROF_MAX) continue;
        CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(th) == (DWORD)-1) continue;
        if (GetThreadContext(th, &c)) g_profIp[g_profN++] = c.Eip;
        ResumeThread(th);
    }
    CloseHandle(th);
    return 0;
}
static void profWrite(const char* path) {
    FILE* o = fopen(path, "w"); if (!o) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0); MODULEENTRY32 me; me.dwSize = sizeof(me);
    struct Mod { DWORD lo, hi; char path[MAX_PATH]; }; static Mod mods[256]; int nm = 0;
    if (snap != INVALID_HANDLE_VALUE && Module32First(snap, &me)) do { if (nm < 256) { mods[nm].lo = (DWORD)(ULONG_PTR)me.modBaseAddr; mods[nm].hi = mods[nm].lo + me.modBaseSize; lstrcpynA(mods[nm].path, me.szExePath, MAX_PATH); ++nm; } } while (Module32Next(snap, &me));
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    for (int i = 0; i < nm; ++i) fprintf(o, "M %08lX %08lX %s\n", (unsigned long)mods[i].lo, (unsigned long)mods[i].hi, mods[i].path);
    LONG n = g_profN;
    for (LONG i = 0; i < n; ++i) fprintf(o, "S %08lX\n", (unsigned long)g_profIp[i]);
    fclose(o);
    printf("profile: %ld samples of the render thread written to %s\n", n, path);
}
int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) { printf("devpass_test <effect.fxo> <out.txt> [frames]\n"); return 2; }
    int frames = argc > 3 ? atoi(argv[3]) : 40;
    if (argc > 4) g_reps = atoi(argv[4]) > 0 ? atoi(argv[4]) : 1;
    FILE* f = fopen(argv[1], "rb"); if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET); char* data = (char*)malloc(size); fread(data, 1, size, f); fclose(f);
    SetEnvironmentVariableA("DXVK_CONFIG", "dxvk.hud="); SetEnvironmentVariableA("DXVK_LOG_LEVEL", "none"); SetEnvironmentVariableA("DXVK_STATE_CACHE", "0");
    HMODULE hx = LoadLibraryA("d3dx9_27.dll"), hd3d = LoadLibraryA("d3d9.dll"), ha = LoadLibraryA("bfme2_accel.new.dll");
    if (!hx || !hd3d || !ha) { printf("d3dx9_27.dll / d3d9.dll / bfme2_accel.new.dll missing\n"); return 3; }
    typedef int (__cdecl* InstallF)(void*, void*); typedef void (__cdecl* ModeF)(int); typedef int (__cdecl* GrabF)(BYTE*, int); typedef void (__cdecl* StatsF)(char*, int);
    InstallF rtInstall = (InstallF)GetProcAddress(ha, "AotrRtTestInstall");
    ModeF mode = (ModeF)GetProcAddress(ha, "AotrTweenTestMode"); GrabF grab = (GrabF)GetProcAddress(ha, "AotrTweenTestGrab");
    StatsF stats = (StatsF)GetProcAddress(ha, "AotrTweenTestStats"), dstats = (StatsF)GetProcAddress(ha, "AotrDevPassStats");
    g_mark = (MarkF)GetProcAddress(ha, "AotrTweenTestMark"); g_tag = (TagF)GetProcAddress(ha, "AotrTweenTestTag"); g_viewsEnd = (void (__cdecl*)())GetProcAddress(ha, "AotrTweenTestViewsEnd");
    if (!rtInstall || !mode || !grab || !stats || !dstats || !g_mark || !g_tag || !g_viewsEnd) { printf("missing test exports\n"); return 3; }
    Sleep(1500);
    typedef void* (WINAPI* Create9F)(UINT);
    typedef HRESULT (WINAPI* CreateEffectF)(void*, const void*, UINT, void*, void*, DWORD, void*, void**, void**);
    Create9F Create9 = (Create9F)GetProcAddress(hd3d, "Direct3DCreate9");
    CreateEffectF CreateEffect = (CreateEffectF)GetProcAddress(hx, "D3DXCreateEffect");
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "devpasstest"; RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "devpasstest", "dp", WS_POPUP, -3000, -3000, 640, 480, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    void* d3d = Create9(32);
    PP pp = {0}; pp.BackBufferWidth = 640; pp.BackBufferHeight = 480; pp.BackBufferFormat = 21; pp.BackBufferCount = 1; pp.SwapEffect = 1; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = 75; pp.PresentationInterval = 0x80000000;
    void* dev = NULL;
    bool fpu24 = getenv("DEVPASS_TEST_FPU24") != NULL;
    g_dense = getenv("DEVPASS_TEST_DENSE") != NULL;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, DWORD, HWND, DWORD, PP*, void**))vt(d3d)[16])(d3d, 0, 1, hwnd, 0x40u | (fpu24 ? 0u : 0x2u) | 0x4u, &pp, &dev);
    if (FAILED(hr) || !dev) { printf("CreateDevice hr=0x%08lX\n", (unsigned long)hr); return 4; }
    g_dev = dev;
    if (fpu24) _control87(_PC_24, _MCW_PC);                                  // (what Direct3D does to the creating thread; made sure of here)
    printf("x87 precision control: %s\n", (_control87(0, 0) & _MCW_PC) == _PC_24 ? "24 bits (as in the game)" : (_control87(0, 0) & _MCW_PC) == _PC_53 ? "53 bits" : "64 bits");
    void* err = NULL;
    hr = CreateEffect(dev, data, (UINT)size, NULL, NULL, 0, NULL, &g_fx, &err);
    if (FAILED(hr) || !g_fx) { printf("D3DXCreateEffect hr=0x%08lX\n", (unsigned long)hr); return 5; }
    h.joints = H("NumJointsPerVertex"); h.world = H("World"); h.bones = H("WorldBones"); h.bonesL = H("WorldBones_L"); h.view = H("View"); h.proj = H("Projection"); h.viewI = H("ViewI"); h.time = H("Time");
    h.ambient = H("AmbientLightColor"); h.dirLight = H("DirectionalLight"); h.numShadows = H("NumShadows"); h.colAmb = H("ColorAmbient"); h.colDif = H("ColorDiffuse"); h.colSpec = H("ColorSpecular");
    h.shin = H("Shininess"); h.colEmi = H("ColorEmissive"); h.opacity = H("Opacity"); h.numTex = H("NumTextures"); h.tex0 = H("Texture_0"); h.tex1 = H("Texture_1"); h.depthW = H("DepthWriteEnable");
    h.alphaT = H("AlphaTestEnable"); h.cull = H("CullingEnable"); h.blend = H("BlendMode"); h.shroudTex = H("ShroudTexture"); h.cloudTex = H("CloudTexture"); h.shadowTex = H("ShadowMap");
    h.houseColor = H("HouseColorEnable"); h.opacityOv = H("OpacityOverride");
    static const char* kTech[3] = { "Default", "Default_L", "Default_M" };
    for (int i = 0; i < 3; ++i) h.tech[i] = ((DWORD(WINAPI*)(void*, const char*))vt(g_fx)[13])(g_fx, kTech[i]);
    if (!h.joints || !h.world || !h.bones || !h.view || !h.proj || !h.tech[0]) { printf("this is not the effect the test is written for (handles missing)\n"); return 5; }
    // textures: three small ones with a pattern, one white
    void* tex[4];
    for (int i = 0; i < 4; ++i) { tex[i] = NULL; ((HRESULT(WINAPI*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, void*))vt(dev)[23])(dev, 16, 16, 1, 0, 21, 1, &tex[i], NULL);
        LR lr = { 0, NULL }; if (tex[i] && SUCCEEDED(CALL4(tex[i], 19, 0, &lr, 0, 0)) && lr.pBits) {
            for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) { DWORD c = i == 3 ? 0xFFFFFFFF : (0xFF000000 | (((x ^ y) & 4) ? (0x00C06030u << (i * 4)) & 0xFFFFFF : 0x00F0E0D0)); *(DWORD*)((BYTE*)lr.pBits + y * lr.Pitch + x * 4) = c; }
            CALL1(tex[i], 20, 0); } }
    static const BYTE kDecl[] = { 0,0, 0,0, 2, 0, 0, 0,    0,0, 12,0, 2, 0, 0, 1,    0,0, 24,0, 2, 0, 3, 0,    0,0, 36,0, 2, 0, 3, 1,    0,0, 48,0, 4, 0, 2, 0,    0,0, 52,0, 0, 0, 1, 0,
                                  0,0, 56,0, 1, 0, 5, 0,    0,0, 64,0, 1, 0, 5, 1,    0,0, 72,0, 4, 0, 10, 0,    0xFF,0, 0,0, 17, 0, 0, 0 };
    if (FAILED(CALL2(dev, 86, kDecl, &g_decl)) || !g_decl) { printf("CreateVertexDeclaration failed\n"); return 5; }
    if (FAILED(((HRESULT(WINAPI*)(void*, UINT, UINT, DWORD, DWORD, void**, void*))vt(dev)[36])(dev, 640, 480, 21, 2, &g_sys, NULL)) || !g_sys) { printf("no system surface\n"); return 5; }
    if (FAILED(CALL4(dev, 18, 0, 0, 0, &g_bb)) || !g_bb) { printf("no back buffer\n"); return 5; }
    { Vtx c[24]; WORD ix[36];
      makeCube(c, ix, 3); g_vb[0] = makeVB(c, sizeof(c)); g_ib[0] = makeIB(ix, sizeof(ix));
      makeCube(c, ix, 3); g_vb[1] = makeVB(c, sizeof(c)); g_ib[1] = makeIB(ix, sizeof(ix));
      makeCube(c, ix, 1); g_vb[2] = makeVB(c, sizeof(c)); g_ib[2] = makeIB(ix, sizeof(ix)); }
    // what the engine sets once: lights, the look-up textures
    { float amb[3] = { 0.35f, 0.35f, 0.4f }; setVal(h.ambient, amb, 12);
      float dl[18] = { 0.9f, 0.85f, 0.8f, 0.3f, -0.8f, 0.5f,   0.2f, 0.2f, 0.3f, -0.5f, -0.3f, -0.8f,   0, 0, 0, 0, -1, 0 }; setVal(h.dirLight, dl, 72);
      setInt(h.numShadows, 0); setTex(h.shroudTex, tex[3]); setTex(h.cloudTex, tex[3]); setTex(h.shadowTex, tex[3]); setFloat(h.opacityOv, 1.0f); }
    int ok = rtInstall(dev, g_fx);
    if (!ok) { printf("render thread install failed\n"); return 7; }
    // three materials, each a parameter block (recorded through the queue, as the game's)
    for (int m = 0; m < 3; ++m) {
        CALL0(g_fx, 73);
        float dif[3] = { 0.9f - 0.2f * m, 0.6f + 0.15f * m, 0.5f + 0.2f * m }, amb[3] = { 0.5f, 0.5f + 0.1f * m, 0.5f }, spec[3] = { 0.2f * m, 0.2f, 0.1f }, emi[3] = { 0.02f * m, 0, 0.03f };
        setVal(h.colDif, dif, 12); setVal(h.colAmb, amb, 12); setVal(h.colSpec, spec, 12); setVal(h.colEmi, emi, 12); setFloat(h.shin, 8.0f + 6.0f * m); setFloat(h.opacity, 1.0f);
        setInt(h.numTex, m == 2 ? 2 : 1); setTex(h.tex0, tex[m]); setTex(h.tex1, tex[(m + 1) % 3]);
        setBool(h.depthW, 1); setBool(h.alphaT, m == 1); setBool(h.cull, 0); setInt(h.blend, 0); setBool(h.houseColor, m == 2);
        g_block[m] = ((DWORD(WINAPI*)(void*))vt(g_fx)[74])(g_fx);
    }
    const char* profPath = getenv("DEVPASS_TEST_PROFILE");
    if (profPath) {
        typedef void (__cdecl* ThrF)(char*, int); ThrF thr = (ThrF)GetProcAddress(ha, "AotrRtTestThreads");
        static char tb[8192]; tb[0] = 0; if (thr) thr(tb, sizeof(tb));
        for (const char* p = tb; p && *p; ) { const char* e = strchr(p, '\n'); const char* w = strstr(p, " RTWORKER"); if (w && (!e || w < e)) { g_profTid = (DWORD)strtoul(p, NULL, 10); break; } p = e ? e + 1 : NULL; }
        g_profIp = (DWORD*)VirtualAlloc(NULL, PROF_MAX * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_profTid && g_profIp) { g_profRun = 1; CreateThread(NULL, 0, profThread, NULL, 0, NULL); } else printf("profile: the render thread was not found\n");
    }
    FILE* out = fopen(argv[2], "w"); if (!out) { printf("cannot write %s\n", argv[2]); return 2; }
    BYTE* G = (BYTE*)malloc(640 * 480 * 4); BYTE* T = (BYTE*)malloc(640 * 480 * 4);
    mode(16);
    int lastSeq = grab(NULL, 0), made = 0, shown = 0, differ = 0;
    LARGE_INTEGER qf, q0, q1; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    for (int fr = 0; fr < frames; ++fr) {
        if (fr == 3 && g_profRun) g_profRun = 2;                           // (the first frames set things up)
        float camF = fr < frames / 2 ? 0.0f : (fr < frames / 2 + 5 ? (float)(fr - frames / 2 + 1) : 5.0f);    // the camera stands still, moves for five frames, stands again
        drawFrame((float)fr, camF, true);
        if (!shot(G)) { printf("could not read the frame back\n"); return 8; }
        ((HRESULT(WINAPI*)(void*, void*, void*, HWND, void*))vt(dev)[17])(dev, NULL, NULL, NULL, NULL);
        { float m[16]; CALL2(dev, 45, 2, m); }                              // wait for the render thread: the in-between picture exists now
        int seq = grab(T, 640 * 480 * 4); bool has = seq != lastSeq; lastSeq = seq;
        int lit = 0; for (int i = 0; i < 640 * 480; ++i) if ((*(DWORD*)(G + i * 4) & 0xFFFFFF) != 0x182028) ++lit;
        int d = 0; if (has) { for (int i = 0; i < 640 * 480; ++i) if ((*(DWORD*)(G + i * 4) ^ *(DWORD*)(T + i * 4)) & 0xFFFFFF) ++d; ++made; if (d) ++differ; }
        if (lit) ++shown;
        fprintf(out, "F %3d game %08lX drawn %6d in-between %08lX differs_from_frame %6d\n", fr, (unsigned long)fnv(G, 640 * 480 * 4), lit, has ? (unsigned long)fnv(T, 640 * 480 * 4) : 0ul, has ? d : -1);
    }
    fclose(out);
    QueryPerformanceCounter(&q1);
    if (g_profRun) { g_profRun = 0; Sleep(20); profWrite(profPath); }
    static char st[4096]; stats(st, sizeof(st)); static char ds[2048]; dstats(ds, sizeof(ds));
    { const char* p = strstr(st, "second_pass_ms="); char ms[32]; ms[0] = 0; if (p) { int k = 0; p += 15; while (*p && *p != ' ' && k < 30) ms[k++] = *p++; ms[k] = 0; }
      printf("%d passes and %d meshes a frame: a second pass takes the render thread %s ms; the whole run %.1f ms a frame\n", 5 * g_reps, 20 * g_reps, ms, (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart / frames); }
    mode(0);
    printf("%d frames: %d show geometry, %d in-between pictures, %d of them differ from their frame (something moved)\n", frames, shown, made, differ);
    printf("%s\n", ds);
    { const char* p = strstr(st, "tags="); printf("in-between frames: %s\n", p ? p : st); }
    return (shown == frames && made >= frames - 3 && differ >= made - 2) ? 0 : 1;
}
