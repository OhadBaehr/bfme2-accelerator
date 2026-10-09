// pcf_test: the smooth shadow lookup (aotr_shadowpcf.inc) against the game's own pixel shaders, on the game's Direct3D.
//   pcf_test.exe <folder with .fxo> [more folders] [dis <file.fxo> <nth shadow shader>]
// For every pixel shader inside the compiled effects (they are found by their version token and read to their end
// token) the patch is applied. Every shader that reads the shadow map must be recognised and changed; the device
// must accept the original and the changed shader; then both draw the same rectangle three times - over a shadow map
// that is all lit, one that is all shadow, one with a slanted edge and a few single points - with every other texture
// a flat grey and the constants a fixed pattern:
//   * all lit / all shadow: the changed shader must draw what the original draws (one step of an 8-bit colour);
//   * the edge: where the nine map points around a pixel agree, the same again; everywhere else the changed shader
//     must lie between the lit and the shadow picture, and - where the picture is a straight blend of the two, which
//     is counted - at the fraction the nine-point arithmetic gives on the CPU.
// And the lookup alone: a shader that is nothing but the game's lookup (its lit fraction as the colour) is changed
// and compared with the same arithmetic done here, pixel by pixel; unchanged, it must be the game's four-point sum.
// 32-bit, raw vtables, no SDK headers (as fxprobe.cpp).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <string>

#include "aotr_shadowpcf.inc"

static void** vt(void* o) { return *(void***)o; }
typedef struct { UINT BackBufferWidth, BackBufferHeight; DWORD BackBufferFormat; UINT BackBufferCount;
    DWORD MultiSampleType, MultiSampleQuality, SwapEffect; HWND hDeviceWindow; BOOL Windowed;
    BOOL EnableAutoDepthStencil; DWORD AutoDepthStencilFormat, Flags; UINT FullScreen_RefreshRateInHz, PresentationInterval; } PP;
#define DEV(slot, sig) ((sig)vt(g_dev)[slot])
static void* g_dev = NULL;
static HMODULE g_hx = NULL;
enum { RTW = 256, RTH = 256, MAPN = 64 };
static void* g_rtTex = NULL, *g_rtSurf = NULL, *g_sys = NULL, *g_grey = NULL, *g_grey2 = NULL, *g_map[3];
static float g_mapData[3][MAPN * MAPN];

static void rel(void* o) { if (o) ((ULONG(WINAPI*)(void*))vt(o)[2])(o); }
static bool makeDevice() {
    SetEnvironmentVariableA("DXVK_CONFIG", "dxvk.hud="); SetEnvironmentVariableA("DXVK_LOG_LEVEL", "none"); SetEnvironmentVariableA("DXVK_STATE_CACHE", "0");
    g_hx = LoadLibraryA("d3dx9_27.dll"); HMODULE hd3d = LoadLibraryA("d3d9.dll");
    if (!g_hx || !hd3d) { printf("d3dx9_27.dll / d3d9.dll missing next to the exe\n"); return false; }
    typedef void* (WINAPI* Create9F)(UINT);
    Create9F Create9 = (Create9F)GetProcAddress(hd3d, "Direct3DCreate9");
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "pcftest"; RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "pcftest", "pcf", WS_POPUP, -3000, -3000, 320, 240, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    void* d3d = Create9(32);
    PP pp = {0}; pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.BackBufferFormat = 21; pp.BackBufferCount = 1; pp.SwapEffect = 1; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = 75; pp.PresentationInterval = 0x80000000;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, DWORD, HWND, DWORD, PP*, void**))vt(d3d)[16])(d3d, 0, 1, hwnd, 0x40u | 0x2u | 0x4u, &pp, &g_dev);
    if (FAILED(hr) || !g_dev) { printf("CreateDevice hr=0x%08lX\n", (unsigned long)hr); return false; }
    typedef HRESULT (WINAPI* CreateTexF)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, void*);
    CreateTexF ct = DEV(23, CreateTexF);
    if (FAILED(ct(g_dev, RTW, RTH, 1, 1, 21, 0, &g_rtTex, NULL))) { printf("no render target\n"); return false; }
    ((HRESULT(WINAPI*)(void*, UINT, void**))vt(g_rtTex)[18])(g_rtTex, 0, &g_rtSurf);
    if (FAILED(DEV(36, HRESULT(WINAPI*)(void*, UINT, UINT, DWORD, DWORD, void**, void*))(g_dev, RTW, RTH, 21, 2, &g_sys, NULL))) { printf("no system-memory surface\n"); return false; }
    struct LR { INT Pitch; void* pBits; } lr;
    ct(g_dev, 4, 4, 1, 0, 21, 1, &g_grey, NULL);
    ((HRESULT(WINAPI*)(void*, UINT, LR*, void*, DWORD))vt(g_grey)[19])(g_grey, 0, &lr, NULL, 0);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) ((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = 0xFF808080u;
    ((HRESULT(WINAPI*)(void*, UINT))vt(g_grey)[20])(g_grey, 0);
    ct(g_dev, 4, 4, 1, 0, 21, 1, &g_grey2, NULL);                      // the same grey, half transparent (some shaders blend the lit part away by the texture's alpha)
    ((HRESULT(WINAPI*)(void*, UINT, LR*, void*, DWORD))vt(g_grey2)[19])(g_grey2, 0, &lr, NULL, 0);
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) ((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = 0x80808080u;
    ((HRESULT(WINAPI*)(void*, UINT))vt(g_grey2)[20])(g_grey2, 0);
    // the three shadow maps: all lit (far), all shadow (near), a slanted edge with a few single points on either side
    for (int m = 0; m < 3; ++m) {
        for (int j = 0; j < MAPN; ++j) for (int i = 0; i < MAPN; ++i) {
            float v = m == 0 ? 1.0f : 0.0f;
            if (m == 2) { v = (i * 3 + j * 2 < 5 * MAPN / 2 + 9) ? 1.0f : 0.0f;
                if ((i == 20 && j == 22) || (i == 23 && j == 27) || (i == 24 && j == 27)) v = 0.0f;
                if ((i == 41 && j == 40) || (i == 44 && j == 36)) v = 1.0f; }
            g_mapData[m][j * MAPN + i] = v;
        }
        g_map[m] = NULL;
        if (FAILED(ct(g_dev, MAPN, MAPN, 1, 0, 114 /* R32F */, 1, &g_map[m], NULL)) || !g_map[m]) { printf("no R32F texture\n"); return false; }
        ((HRESULT(WINAPI*)(void*, UINT, LR*, void*, DWORD))vt(g_map[m])[19])(g_map[m], 0, &lr, NULL, 0);
        for (int j = 0; j < MAPN; ++j) memcpy((BYTE*)lr.pBits + j * lr.Pitch, &g_mapData[m][j * MAPN], MAPN * 4);
        ((HRESULT(WINAPI*)(void*, UINT))vt(g_map[m])[20])(g_map[m], 0);
    }
    return true;
}
struct Vtx { float x, y, z, rhw; DWORD diff, spec; float tc[8][4]; };
static const float kU0 = 0.25f + 1.0f / (MAPN * 32.0f), kUSpan = 0.5f;           // 8 pixels per map point, never on a border between two
// where pixel (x, y) of the target is in the map, in map points
static __forceinline void mapPos(int x, int y, double* px, double* py) { *px = ((double)kU0 + (double)kUSpan * (x + 0.5) / RTW) * MAPN; *py = ((double)kU0 + (double)kUSpan * (y + 0.5) / RTH) * MAPN; }
static int g_pattern = 0;                                           // which set of made-up constants the shaders get
static bool draw(void* ps, int map, DWORD shadowStage, DWORD cOff, bool sm3, std::vector<DWORD>& outPix) {
    typedef HRESULT (WINAPI* F1)(void*); typedef HRESULT (WINAPI* F2)(void*, DWORD); typedef HRESULT (WINAPI* F3)(void*, DWORD, DWORD); typedef HRESULT (WINAPI* F4)(void*, DWORD, DWORD, DWORD);
    DEV(37, HRESULT(WINAPI*)(void*, DWORD, void*))(g_dev, 0, g_rtSurf);
    DEV(39, HRESULT(WINAPI*)(void*, void*))(g_dev, NULL);
    DEV(43, HRESULT(WINAPI*)(void*, DWORD, void*, DWORD, DWORD, float, DWORD))(g_dev, 0, NULL, 1, 0xFF203040u, 1.0f, 0);
    static const DWORD rs[][2] = { { 7, 0 }, { 14, 0 }, { 15, 0 }, { 22, 1 }, { 27, 0 }, { 28, 0 }, { 52, 0 }, { 136, 0 }, { 137, 0 }, { 168, 0xF }, { 174, 0 }, { 194, 0 }, { 206, 0 } };
    for (int i = 0; i < (int)(sizeof(rs) / sizeof(rs[0])); ++i) DEV(57, F3)(g_dev, rs[i][0], rs[i][1]);
    for (DWORD s = 0; s < 16; ++s) {
        bool sh = s == shadowStage;
        DEV(65, HRESULT(WINAPI*)(void*, DWORD, void*))(g_dev, s, sh ? g_map[map] : (g_pattern >= 9 ? g_grey2 : g_grey));
        const DWORD ss[][2] = { { 1, 3 }, { 2, 3 }, { 3, 3 }, { 5, sh ? 1u : 2u }, { 6, sh ? 1u : 2u }, { 7, 0 }, { 8, 0 }, { 9, 0 }, { 11, 0 } };
        for (int i = 0; i < 9; ++i) DEV(69, F4)(g_dev, s, ss[i][0], ss[i][1]);
    }
    DEV(92, HRESULT(WINAPI*)(void*, void*))(g_dev, NULL);
    DEV(89, F2)(g_dev, 0xAAAA08C4u);                                   // XYZRHW | DIFFUSE | SPECULAR | TEX8, every set four wide
    float c[224][4]; const int g_pat = g_pattern % 9;
    for (int r = 0; r < 224; ++r) for (int k = 0; k < 4; ++k) { float v = 0.15f + 0.7f * (float)((r * (7 + 2 * g_pat) + k * (13 + 4 * g_pat) + 5 * g_pat) % 17) / 16.0f;
        if (g_pat == 3) v = 1.0f; else if (g_pat == 4) v = 0.0f; else if (g_pat >= 5 && ((r * 3 + k + g_pat) % (g_pat - 2)) == 0) v = -v;     // 5..8: some negative
        c[r][k] = v; }
    c[cOff][0] = 0; c[cOff][1] = 0; c[cOff][2] = 1.0f / MAPN; c[cOff][3] = 1.0f / MAPN;
    DEV(109, HRESULT(WINAPI*)(void*, UINT, const float*, UINT))(g_dev, 0, &c[0][0], sm3 ? 224 : 32);
    DEV(107, HRESULT(WINAPI*)(void*, void*))(g_dev, ps);
    Vtx v[4]; memset(v, 0, sizeof(v));
    for (int i = 0; i < 4; ++i) {
        bool r = (i & 1) != 0, b = (i & 2) != 0;
        v[i].x = (r ? (float)RTW : 0.0f) - 0.5f; v[i].y = (b ? (float)RTH : 0.0f) - 0.5f; v[i].z = 0.5f; v[i].rhw = 1.0f;
        v[i].diff = 0xFFB0A090u; v[i].spec = 0xFF808080u;
        for (int t = 0; t < 8; ++t) { v[i].tc[t][0] = kU0 + (r ? kUSpan : 0.0f); v[i].tc[t][1] = kU0 + (b ? kUSpan : 0.0f); v[i].tc[t][2] = 0.5f; v[i].tc[t][3] = 1.0f; }
    }
    bool ok = SUCCEEDED(DEV(41, F1)(g_dev));
    if (ok) { ok = SUCCEEDED(DEV(83, HRESULT(WINAPI*)(void*, DWORD, UINT, const void*, UINT))(g_dev, 5, 2, v, sizeof(Vtx))); DEV(42, F1)(g_dev); }
    DEV(107, HRESULT(WINAPI*)(void*, void*))(g_dev, NULL);
    if (!ok) return false;
    if (FAILED(DEV(32, HRESULT(WINAPI*)(void*, void*, void*))(g_dev, g_rtSurf, g_sys))) return false;
    struct LR { INT Pitch; void* pBits; } lr;
    if (FAILED(((HRESULT(WINAPI*)(void*, LR*, void*, DWORD))vt(g_sys)[13])(g_sys, &lr, NULL, 0x10))) return false;
    outPix.resize(RTW * RTH);
    for (int y = 0; y < RTH; ++y) memcpy(&outPix[y * RTW], (BYTE*)lr.pBits + y * lr.Pitch, RTW * 4);
    ((HRESULT(WINAPI*)(void*))vt(g_sys)[14])(g_sys);
    return true;
}
static __forceinline float mapAt(int m, int i, int j) { if (i < 0) i = 0; if (j < 0) j = 0; if (i >= MAPN) i = MAPN - 1; if (j >= MAPN) j = MAPN - 1; return g_mapData[m][j * MAPN + i]; }
// the nine-point lit fraction, as the new lookup makes it
static double refNine(int m, double px, double py) {
    int i = (int)floor(px), j = (int)floor(py); double fx = px - i, fy = py - j;
    double wx[3] = { 0.5 - 0.5 * fx, 0.5, 0.5 * fx }, wy[3] = { 0.5 - 0.5 * fy, 0.5, 0.5 * fy }, s = 0;
    for (int b = 0; b < 3; ++b) for (int a = 0; a < 3; ++a) s += wx[a] * wy[b] * (mapAt(m, i - 1 + a, j - 1 + b) >= 0.498f ? 1.0 : 0.0);
    return s;
}
static double refFour(int m, double px, double py) {
    int i = (int)floor(px), j = (int)floor(py); double s = 0;
    for (int b = 0; b < 2; ++b) for (int a = 0; a < 2; ++a) s += mapAt(m, i + a, j + b) >= 0.498f ? 0.25 : 0.0;
    return s;
}
static bool nineAgree(int m, double px, double py) {
    int i = (int)floor(px), j = (int)floor(py); float v = mapAt(m, i, j);
    for (int b = -1; b <= 1; ++b) for (int a = -1; a <= 1; ++a) if (mapAt(m, i + a, j + b) != v) return false;
    return true;
}
static void* makePs(const DWORD* code) { void* ps = NULL; if (FAILED(DEV(106, HRESULT(WINAPI*)(void*, const DWORD*, void**))(g_dev, code, &ps))) return NULL; return ps; }
static void disasm(const DWORD* code) {
    typedef HRESULT (WINAPI* DisF)(const DWORD*, BOOL, const char*, void**);
    DisF dis = (DisF)GetProcAddress(g_hx, "D3DXDisassembleShader"); void* buf = NULL;
    if (!dis || FAILED(dis(code, FALSE, NULL, &buf)) || !buf) { printf("(cannot be disassembled)\n"); return; }
    const char* txt = ((const char*(WINAPI*)(void*))vt(buf)[3])(buf); DWORD n = ((DWORD(WINAPI*)(void*))vt(buf)[4])(buf);
    fwrite(txt, 1, n ? n - 1 : 0, stdout); rel(buf);
}

struct Sh { std::string file; DWORD off; std::vector<DWORD> code; };
static void findShaders(const char* path, std::vector<Sh>& out) {
    FILE* f = NULL; fopen_s(&f, path, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<BYTE> d(size); fread(d.data(), 1, size, f); fclose(f);
    const char* base = strrchr(path, '\\'); if (!base) base = strrchr(path, '/'); base = base ? base + 1 : path;
    for (long off = 0; off + 8 <= size; ) {
        DWORD v; memcpy(&v, &d[off], 4);
        if (v != 0xFFFF0200u && v != 0xFFFF0201u && v != 0xFFFF0300u) { off += 4; continue; }
        long p = off + 4; bool ok = false; int nIns = 0;
        while (p + 4 <= size) {
            DWORD t; memcpy(&t, &d[p], 4);
            if (t == 0x0000FFFFu) { ok = true; p += 4; break; }
            if ((t & 0xFFFF) == 0xFFFE) { p += 4 + 4 * ((t >> 16) & 0x7FFF); continue; }
            if (t & 0x80000000u) break;
            DWORD op = t & 0xFFFF; if (op > 0x60) break;
            p += 4 + 4 * ((t >> 24) & 0xF); ++nIns;
        }
        if (!ok || nIns < 2) { off += 4; continue; }
        Sh s; s.file = base; s.off = (DWORD)off; s.code.resize((p - off) / 4); memcpy(s.code.data(), &d[off], p - off);
        out.push_back(s); off = p;
    }
}
// a constant table naming ShadowInfo (c0, five registers) and ShadowMapSampler (s0), as a comment block
static void makeCtab(std::vector<DWORD>& out) {
    std::vector<BYTE> b(28 + 40, 0);
    const char* n0 = "ShadowInfo", *n1 = "ShadowMapSampler", *cr = "pcf_test";
    DWORD o0 = (DWORD)b.size(); b.insert(b.end(), n0, n0 + strlen(n0) + 1);
    DWORD o1 = (DWORD)b.size(); b.insert(b.end(), n1, n1 + strlen(n1) + 1);
    DWORD oc = (DWORD)b.size(); b.insert(b.end(), cr, cr + strlen(cr) + 1);
    while (b.size() & 3) b.push_back(0);
    DWORD* h = (DWORD*)b.data(); h[0] = 28; h[1] = oc; h[2] = 0xFFFF0200u; h[3] = 2; h[4] = 28; h[5] = 0; h[6] = oc;
    struct CI { DWORD Name; WORD Set, Index, Count, Res; DWORD Type, Def; } ci[2] = { { o0, 2, 0, 5, 0, 0, 0 }, { o1, 3, 0, 1, 0, 0, 0 } };
    memcpy(b.data() + 28, ci, 40);
    DWORD n = (DWORD)b.size() / 4 + 1;
    out.push_back(0x0000FFFEu | (n << 16)); out.push_back(0x42415443u);
    for (size_t i = 0; i < b.size(); i += 4) { DWORD v; memcpy(&v, &b[i], 4); out.push_back(v); }
}
static int lookupAlone() {
    static const char kSrc[] =
        "ps_2_0\n"
        "def c5, -0.002, 1, -0, 0.25\n"
        "dcl t0.xyz\n"
        "dcl_2d s0\n"
        "add r2.xy, t0, c4.zxyw\n"
        "add r1.xy, t0, c4.yzxw\n"
        "add r0.xy, t0, c4.wzyx\n"
        "texld r6, r2, s0\n"
        "texld r5, r1, s0\n"
        "texld r4, r0, s0\n"
        "texld r0, t0, s0\n"
        "mov r0.y, r6.x\n"
        "mov r0.z, r5.x\n"
        "mov r0.w, r4.x\n"
        "add r2.w, t0.z, c5.x\n"
        "add r0, r0, -r2.w\n"
        "cmp r0, r0, c5.y, c5.z\n"
        "dp4 r0.w, c5.y, r0\n"
        "mul r0, r0.w, c5.w\n"
        "mov oC0, r0\n";
    typedef HRESULT (WINAPI* AsmF)(const char*, UINT, const void*, void*, DWORD, void**, void**);
    AsmF as = (AsmF)GetProcAddress(g_hx, "D3DXAssembleShader"); void* code = NULL, *err = NULL;
    if (!as || FAILED(as(kSrc, (UINT)strlen(kSrc), NULL, NULL, 0, &code, &err)) || !code) { printf("lookup alone: the test shader could not be assembled\n"); return 1; }
    const DWORD* p = ((const DWORD*(WINAPI*)(void*))vt(code)[3])(code); DWORD n = ((DWORD(WINAPI*)(void*))vt(code)[4])(code) / 4;
    std::vector<DWORD> src; src.push_back(p[0]); makeCtab(src); for (DWORD i = 1; i < n; ++i) src.push_back(p[i]);
    std::vector<DWORD> out(src.size() + 512); SpInfo info;
    DWORD m = spPatch(src.data(), (DWORD)src.size(), out.data(), (DWORD)out.size(), &info);
    if (!m) { printf("lookup alone: NOT recognised (%s)\n", info.why); return 1; }
    void* po = makePs(src.data()), *pn = makePs(out.data());
    if (!po || !pn) { printf("lookup alone: the device refused %s\n", !po ? "the test shader" : "the changed shader"); disasm(out.data()); return 1; }
    int fail = 0;
    for (int map = 0; map < 3; ++map) {
        std::vector<DWORD> a, b;
        if (!draw(po, map, 0, 4, false, a) || !draw(pn, map, 0, 4, false, b)) { printf("lookup alone: could not draw\n"); return 1; }
        int bad4 = 0, bad9 = 0, worst4 = 0, worst9 = 0;
        for (int y = 0; y < RTH; ++y) for (int x = 0; x < RTW; ++x) {
            double px, py; mapPos(x, y, &px, &py);
            int want4 = (int)floor(refFour(map, px, py) * 255.0 + 0.5), want9 = (int)floor(refNine(map, px, py) * 255.0 + 0.5);
            int got4 = (int)(a[y * RTW + x] & 0xFF), got9 = (int)(b[y * RTW + x] & 0xFF);
            int d4 = abs(got4 - want4), d9 = abs(got9 - want9);
            if (d4 > worst4) worst4 = d4; if (d9 > worst9) worst9 = d9;
            if (d4 > 1) ++bad4; if (d9 > 1) ++bad9;
        }
        printf("lookup alone, map %d (%s): the game's lookup against its four-point sum: %d pixels off by more than one step (worst %d) | the new lookup against the nine-point arithmetic: %d off (worst %d)\n",
               map, map == 0 ? "all lit" : map == 1 ? "all shadow" : "an edge and single points", bad4, worst4, bad9, worst9);
        if (bad4 || bad9) fail = 1;
    }
    rel(po); rel(pn); rel(code);
    return fail;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("pcf_test <folder with .fxo> [more folders] | dis <file.fxo> <nth>\n"); return 2; }
    if (!makeDevice()) return 3;
    if (!strcmp(argv[1], "dis") && argc >= 3) {
        std::vector<Sh> sh; findShaders(argv[2], sh); int want = argc > 3 ? atoi(argv[3]) : 0, k = 0;
        for (size_t i = 0; i < sh.size(); ++i) {
            std::vector<DWORD> out(sh[i].code.size() + 512); SpInfo info;
            DWORD m = spPatch(sh[i].code.data(), (DWORD)sh[i].code.size(), out.data(), (DWORD)out.size(), &info);
            if (!m) continue;
            if (k++ != want) continue;
            printf("==== %s @%06lX as it is\n", sh[i].file.c_str(), (unsigned long)sh[i].off); disasm(sh[i].code.data());
            printf("==== changed (%d more tokens; registers r%lu-r%lu, constants c%lu c%lu c%lu c%lu)\n", info.grew, (unsigned long)info.temp0, (unsigned long)info.temp0 + 6,
                   (unsigned long)info.kc[0], (unsigned long)info.kc[1], (unsigned long)info.kc[2], (unsigned long)info.kc[3]); disasm(out.data());
            return 0;
        }
        printf("no such shader\n"); return 1;
    }
    if (!strcmp(argv[1], "hlsl") && argc >= 4) {                       // does the game's compiler turn this source into the lookup the patch knows?
        FILE* f = NULL; fopen_s(&f, argv[2], "rb"); if (!f) { printf("cannot open %s\n", argv[2]); return 2; }
        fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET); std::vector<char> src(size); fread(src.data(), 1, size, f); fclose(f);
        typedef HRESULT (WINAPI* CompF)(const char*, UINT, const void*, void*, const char*, const char*, DWORD, void**, void**, void**);
        CompF comp = (CompF)GetProcAddress(g_hx, "D3DXCompileShader"); void* code = NULL, *err = NULL;
        if (!comp || FAILED(comp(src.data(), (UINT)size, NULL, NULL, argv[3], "ps_2_0", 0, &code, &err, NULL)) || !code) {
            printf("not compiled: %s\n", err ? ((const char*(WINAPI*)(void*))vt(err)[3])(err) : "?"); return 1; }
        const DWORD* p = ((const DWORD*(WINAPI*)(void*))vt(code)[3])(code); DWORD n = ((DWORD(WINAPI*)(void*))vt(code)[4])(code) / 4;
        disasm(p);
        std::vector<DWORD> out(n + 512); SpInfo info; DWORD m = spPatch(p, n, out.data(), (DWORD)out.size(), &info);
        printf("\n%s%s\n", m ? "RECOGNISED and changed" : "NOT recognised: ", m ? "" : info.why);
        return m ? 0 : 1;
    }
    if (getenv("PCF_PROBE")) {                                         // what the rectangle hands a pixel shader in each input register
        typedef HRESULT (WINAPI* AsmF)(const char*, UINT, const void*, void*, DWORD, void**, void**);
        AsmF as = (AsmF)GetProcAddress(g_hx, "D3DXAssembleShader");
        static const char* kIn[] = { "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "v0", "v1" };
        for (int k = 0; k < 10; ++k) {
            char src[256]; sprintf_s(src, "ps_2_0\ndcl %s\nmov r0, %s\nmov oC0, r0\n", kIn[k], kIn[k]);
            void* code = NULL, *err = NULL;
            if (FAILED(as(src, (UINT)strlen(src), NULL, NULL, 0, &code, &err)) || !code) { printf("probe %s: not assembled%s", kIn[k], "\n"); continue; }
            void* ps = makePs(((const DWORD*(WINAPI*)(void*))vt(code)[3])(code)); std::vector<DWORD> px;
            if (ps && draw(ps, 0, 15, 4, false, px)) printf("probe %s: corner %08lX middle %08lX far corner %08lX%s", kIn[k], (unsigned long)px[2 * RTW + 2], (unsigned long)px[128 * RTW + 128], (unsigned long)px[253 * RTW + 253], "\n");
            rel(ps); rel(code);
        }
    }
    int fail = lookupAlone();
    std::vector<Sh> sh;
    for (int a = 1; a < argc; ++a) {
        char pat[MAX_PATH]; sprintf_s(pat, "%s\\*.fxo", argv[a]);
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) { printf("no .fxo in %s\n", argv[a]); continue; }
        do { char p[MAX_PATH]; sprintf_s(p, "%s\\%s", argv[a], fd.cFileName); findShaders(p, sh); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    int nShadow = 0, nPatched = 0, nNot = 0, nRefused = 0, nDrawFail = 0, nFlatBad = 0, nAgreeBad = 0, nRangeBad = 0, nBlend = 0, nBlendOk = 0, nTested = 0, nSame = 0;
    std::string lastFile;
    for (size_t i = 0; i < sh.size(); ++i) {
        const Sh& s = sh[i];
        std::vector<DWORD> out(s.code.size() + 512); SpInfo info;
        DWORD m = spPatch(s.code.data(), (DWORD)s.code.size(), out.data(), (DWORD)out.size(), &info);
        if (!m) { if (info.why[0]) { ++nShadow; ++nNot; printf("NOT CHANGED %s @%06lX: %s\n", s.file.c_str(), (unsigned long)s.off, info.why); } continue; }
        ++nShadow; ++nPatched;
        { SpInfo again; std::vector<DWORD> out2(out.size() + 512);                 // a changed shader is not changed a second time
          if (spPatch(out.data(), m, out2.data(), (DWORD)out2.size(), &again)) { printf("CHANGED TWICE %s @%06lX\n", s.file.c_str(), (unsigned long)s.off); fail = 1; } }
        void* po = makePs(s.code.data()), *pn = makePs(out.data());
        if (!po || !pn) { ++nRefused; printf("REFUSED by the device: %s @%06lX (%s)\n", s.file.c_str(), (unsigned long)s.off, !po ? "the original" : "the changed shader"); rel(po); rel(pn); continue; }
        bool sm3 = s.code[0] == 0xFFFF0300u;
        std::vector<DWORD> A0, A1, S0, S1, E0, E1;
        bool drew = true; int spread = 0;
        for (g_pattern = 0; g_pattern < 18; ++g_pattern) {                   // a set of constants under which the shadow shows, if there is one
            drew = draw(po, 0, info.sampler, info.cOff, sm3, A0) && draw(po, 1, info.sampler, info.cOff, sm3, S0);
            if (!drew) break;
            spread = 0;
            for (int k = 0; k < RTW * RTH; ++k) for (int ch = 0; ch < 3; ++ch) { int d = abs((int)((A0[k] >> (8 * ch)) & 0xFF) - (int)((S0[k] >> (8 * ch)) & 0xFF)); if (d > spread) spread = d; }
            if (getenv("PCF_DEBUG") && s.off == strtoul(getenv("PCF_DEBUG"), NULL, 16)) printf("   pattern %d: lit %08lX %08lX %08lX | shadow %08lX %08lX %08lX | spread %d\n", g_pattern, (unsigned long)A0[40 * RTW + 40], (unsigned long)A0[128 * RTW + 128], (unsigned long)A0[200 * RTW + 220], (unsigned long)S0[40 * RTW + 40], (unsigned long)S0[128 * RTW + 128], (unsigned long)S0[200 * RTW + 220], spread);
            if (spread >= 8 || g_pattern == 17) break;
        }
        if (!drew || !draw(pn, 0, info.sampler, info.cOff, sm3, A1) || !draw(pn, 1, info.sampler, info.cOff, sm3, S1) ||
            !draw(po, 2, info.sampler, info.cOff, sm3, E0) || !draw(pn, 2, info.sampler, info.cOff, sm3, E1)) { ++nDrawFail; rel(po); rel(pn); g_pattern = 0; continue; }
        g_pattern = 0;
        ++nTested;
        // From the original's own picture of the edge: a pixel is lit (1) or in shadow (2) when it is what the all-lit or
        // the all-shadow picture has there; "deep inside" when every pixel within two map points and a bit is the same.
        // (Not from where the rectangle puts the pixel in the map: the water shaders move their coordinates.)
        static int cl[RTH][RTW], sum1[RTH + 1][RTW + 1], sum2[RTH + 1][RTW + 1];
        for (int y = 0; y < RTH; ++y) for (int x = 0; x < RTW; ++x) {
            int k = y * RTW + x; bool isA = true, isS = true;
            for (int ch = 0; ch < 3; ++ch) { int a0 = (A0[k] >> (8 * ch)) & 0xFF, s0 = (S0[k] >> (8 * ch)) & 0xFF, e0 = (E0[k] >> (8 * ch)) & 0xFF; if (abs(e0 - a0) > 1) isA = false; if (abs(e0 - s0) > 1) isS = false; }
            cl[y][x] = (isA ? 1 : 0) | (isS ? 2 : 0);
        }
        for (int y = 0; y <= RTH; ++y) for (int x = 0; x <= RTW; ++x) {
            if (!y || !x) { sum1[y][x] = sum2[y][x] = 0; continue; }
            sum1[y][x] = sum1[y - 1][x] + sum1[y][x - 1] - sum1[y - 1][x - 1] + ((cl[y - 1][x - 1] & 1) ? 1 : 0);
            sum2[y][x] = sum2[y - 1][x] + sum2[y][x - 1] - sum2[y - 1][x - 1] + ((cl[y - 1][x - 1] & 2) ? 1 : 0);
        }
        const int kR = 20;                                                 // 8 pixels a map point: two and a half map points
        int flatBad = 0, agreeBad = 0, rangeBad = 0, blendPix = 0, blendBad = 0, deep = 0; bool affine = true;
        for (int y = 0; y < RTH; ++y) for (int x = 0; x < RTW; ++x) {
            int k = y * RTW + x; double px, py; mapPos(x, y, &px, &py);
            int x0 = x - kR < 0 ? 0 : x - kR, x1 = x + kR + 1 > RTW ? RTW : x + kR + 1, y0 = y - kR < 0 ? 0 : y - kR, y1 = y + kR + 1 > RTH ? RTH : y + kR + 1, area = (x1 - x0) * (y1 - y0);
            bool agree = (sum1[y1][x1] - sum1[y0][x1] - sum1[y1][x0] + sum1[y0][x0] == area) || (sum2[y1][x1] - sum2[y0][x1] - sum2[y1][x0] + sum2[y0][x0] == area);
            if (agree) ++deep;
            double L4 = refFour(2, px, py);
            for (int ch = 0; ch < 3; ++ch) {
                int a0 = (A0[k] >> (8 * ch)) & 0xFF, a1 = (A1[k] >> (8 * ch)) & 0xFF, s0 = (S0[k] >> (8 * ch)) & 0xFF, s1 = (S1[k] >> (8 * ch)) & 0xFF, e0 = (E0[k] >> (8 * ch)) & 0xFF, e1 = (E1[k] >> (8 * ch)) & 0xFF;
                if (abs(a0 - a1) > 1 || abs(s0 - s1) > 1) ++flatBad;
                int lo = a0 < s0 ? a0 : s0, hi = a0 < s0 ? s0 : a0;
                if (agree) { if (abs(e1 - e0) > 1) ++agreeBad; }
                if (e1 < lo - 1 || e1 > hi + 1) ++rangeBad;
                if (fabs((double)e0 - (s0 + (a0 - s0) * L4)) > 1.6) affine = false;            // the original is not a straight blend at the fraction worked out here (moved coordinates, a clamp): no fraction to compare with
            }
        }
        if (deep < RTW * RTH / 8) { printf("TEST TOO WEAK %s @%06lX: only %d pixels are deep inside lit or shadow\n", s.file.c_str(), (unsigned long)s.off, deep); fail = 1; }
        if (affine && spread >= 8) {
            for (int y = 0; y < RTH; ++y) for (int x = 0; x < RTW; ++x) {
                int k = y * RTW + x; double px, py; mapPos(x, y, &px, &py);
                if (nineAgree(2, px, py)) continue;
                double L9 = refNine(2, px, py);
                for (int ch = 0; ch < 3; ++ch) {
                    int a0 = (A0[k] >> (8 * ch)) & 0xFF, s0 = (S0[k] >> (8 * ch)) & 0xFF, e1 = (E1[k] >> (8 * ch)) & 0xFF;
                    ++blendPix; if (fabs((double)e1 - (s0 + (a0 - s0) * L9)) > 1.6) ++blendBad;
                }
            }
            ++nBlend; if (!blendBad) ++nBlendOk;
        }
        if (spread < 2) { ++nSame; if (nSame <= 40) printf("   (no difference between lit and shadow: %s @%06lX, sampler s%lu, %d tokens)\n", s.file.c_str(), (unsigned long)s.off, (unsigned long)info.sampler, (int)s.code.size()); }
        if (flatBad) ++nFlatBad; if (agreeBad) ++nAgreeBad; if (rangeBad) ++nRangeBad;
        if (flatBad || agreeBad || rangeBad || (affine && spread >= 8 && blendBad))
            printf("DIFFERS %s @%06lX (ps_%d_%d, sampler s%lu, ShadowInfo+4 = c%lu): lit/shadow maps %d channel values off, where the nine agree %d off, outside lit..shadow %d, off the nine-point fraction %d of %d\n",
                   s.file.c_str(), (unsigned long)s.off, (int)((s.code[0] >> 8) & 0xFF), (int)(s.code[0] & 0xFF), (unsigned long)info.sampler, (unsigned long)info.cOff, flatBad, agreeBad, rangeBad, blendBad, blendPix);
        rel(po); rel(pn);
    }
    printf("%d pixel shaders (ps_2_0 and up) in the effects; %d read the shadow map: %d changed, %d NOT recognised\n", (int)sh.size(), nShadow, nPatched, nNot);
    printf("device: %d refused, %d could not be drawn; %d drawn both ways\n", nRefused, nDrawFail, nTested);
    printf("all lit / all shadow - the changed shader draws what the original draws: %d of %d\n", nTested - nFlatBad, nTested);
    printf("an edge - deep inside lit or shadow the same picture: %d of %d; everywhere between lit and shadow: %d of %d\n", nTested - nAgreeBad, nTested, nTested - nRangeBad, nTested);
    printf("an edge - at the nine-point fraction exactly (shaders whose picture is a straight blend of lit and shadow there): %d of %d   (%d shaders show no difference between lit and shadow with these inputs)\n", nBlendOk, nBlend, nSame);
    if (nNot || nRefused || nDrawFail || nFlatBad || nAgreeBad || nRangeBad || nBlendOk != nBlend || !nPatched) fail = 1;
    printf("%s\n", fail ? "FAILED" : "ALL AS IT MUST BE");
    return fail;
}
