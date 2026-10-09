// rt_harness.cpp - offline correctness test for the render thread (RT) in aotr_accel.dll.
//   rt_harness off <frames> <out.txt>   render with direct D3D9/D3DX calls
//   rt_harness on  <frames> <out.txt>   same frames with the render thread installed
// Every frame is read back (StretchRect -> GetRenderTargetData -> LockRect) and hashed; the two runs must
// produce identical hashes. The scene mirrors the game's patterns: a render-to-texture shadow pass with a
// "_CreateShadowMap" technique, Begin(flags=6) state blocks, parameter blocks, SetRawValue world matrices
// and bone palettes, a preshader, dynamic vertex buffer DISCARD locks, per-frame texture LockRect updates,
// DrawPrimitiveUP / DrawIndexedPrimitiveUP, application state blocks and D3DX texture creation mid-run.
// Uses DXVK's d3d9.dll placed next to the exe and the system d3dx9_27.dll. 32-bit, raw vtables.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

static void** vt(void* o) { return *(void***)o; }
#define CALL0(o, s)                 ((HRESULT(WINAPI*)(void*))vt(o)[s])(o)
#define CALL1(o, s, a)              ((HRESULT(WINAPI*)(void*, DWORD))vt(o)[s])(o, (DWORD)(a))
#define CALL2(o, s, a, b)           ((HRESULT(WINAPI*)(void*, DWORD, DWORD))vt(o)[s])(o, (DWORD)(a), (DWORD)(b))
#define CALL3(o, s, a, b, c)        ((HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD))vt(o)[s])(o, (DWORD)(a), (DWORD)(b), (DWORD)(c))
#define CALL4(o, s, a, b, c, d)     ((HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD, DWORD))vt(o)[s])(o, (DWORD)(a), (DWORD)(b), (DWORD)(c), (DWORD)(d))
#define CALL5(o, s, a, b, c, d, e)  ((HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD))vt(o)[s])(o, (DWORD)(a), (DWORD)(b), (DWORD)(c), (DWORD)(d), (DWORD)(e))
#define CALL6(o, s, a, b, c, d, e, f) ((HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))vt(o)[s])(o, (DWORD)(a), (DWORD)(b), (DWORD)(c), (DWORD)(d), (DWORD)(e), (DWORD)(f))
#define CALL8(o, s, a, b, c, d, e, f, g, h) ((HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))vt(o)[s])(o, (DWORD)(a), (DWORD)(b), (DWORD)(c), (DWORD)(d), (DWORD)(e), (DWORD)(f), (DWORD)(g), (DWORD)(h))
static DWORD fbits(float f) { DWORD d; memcpy(&d, &f, 4); return d; }

typedef struct { UINT BackBufferWidth, BackBufferHeight; DWORD BackBufferFormat; UINT BackBufferCount;
    DWORD MultiSampleType, MultiSampleQuality, SwapEffect; HWND hDeviceWindow; BOOL Windowed;
    BOOL EnableAutoDepthStencil; DWORD AutoDepthStencilFormat, Flags; UINT FullScreen_RefreshRateInHz, PresentationInterval; } PP;
typedef struct { INT Pitch; void* pBits; } LOCKED_RECT;
typedef struct { WORD Stream, Offset; BYTE Type, Method, Usage, UsageIndex; } VELEM;

static const char kFx[] =
"float4x4 World; float4x4 ViewProj; float4x4 LightViewProj; float3x4 Bones[4];\n"
"float4 MatColor = {1,1,1,1}; float Blend = 0.5; float4 Tint = {1,1,1,1};\n"
"float ThisParameterNameIsLongerThanFortyCharactersForTheCache = 1.0; struct MatParams { float4 Col; float Val; }; MatParams Mat = { {1,1,1,1}, 1.0 };\n"
"texture DiffuseTex; sampler DiffuseS = sampler_state { Texture = <DiffuseTex>; MinFilter = LINEAR; MagFilter = LINEAR; MipFilter = LINEAR; };\n"
"texture ShadowTex; sampler ShadowS = sampler_state { Texture = <ShadowTex>; MinFilter = POINT; MagFilter = POINT; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };\n"
"struct VSIn { float4 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };\n"
"struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 lpos : TEXCOORD1; float3 nrm : TEXCOORD2; };\n"
"VSOut VS(VSIn i) { VSOut o; float3 p = mul(float4(i.pos.xyz,1), Bones[0]); float4 w = mul(float4(p,1), World);\n"
"  o.pos = mul(w, ViewProj); o.uv = i.uv; o.lpos = mul(w, LightViewProj); o.nrm = mul(i.nrm, (float3x3)World); return o; }\n"
"float4 PS(VSOut i) : COLOR { float2 suv = i.lpos.xy / i.lpos.w * float2(0.5,-0.5) + 0.5; float d = tex2D(ShadowS, suv).r;\n"
"  float lit = (i.lpos.z / i.lpos.w - 0.01 < d) ? 1.0 : 0.4; float4 c = tex2D(DiffuseS, i.uv) * MatColor * (Tint * Blend * 2.0) * ThisParameterNameIsLongerThanFortyCharactersForTheCache * Mat.Val * Mat.Col;\n"
"  return float4(c.rgb * lit * (0.5 + 0.5 * saturate(normalize(i.nrm).y)), 1); }\n"
"struct SVSOut { float4 pos : POSITION; float z : TEXCOORD0; };\n"
"SVSOut SVS(VSIn i) { SVSOut o; float3 p = mul(float4(i.pos.xyz,1), Bones[0]); float4 w = mul(float4(p,1), World);\n"
"  o.pos = mul(w, LightViewProj); o.z = o.pos.z / o.pos.w; return o; }\n"
"float4 SPS(SVSOut i) : COLOR { return float4(i.z, i.z, i.z, 1); }\n"
"technique Main < int MaxSkinningBones = 24; float Lod = 0.75; > { pass p0 { VertexShader = compile vs_2_0 VS(); PixelShader = compile ps_2_0 PS(); ZEnable = TRUE; CullMode = CCW; AlphaBlendEnable = FALSE; } }\n"
"technique _CreateShadowMap { pass p0 { VertexShader = compile vs_2_0 SVS(); PixelShader = compile ps_2_0 SPS(); ZEnable = TRUE; CullMode = NONE; } }\n";

static void matIdent(float* m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void matMul(const float* a, const float* b, float* o) { float t[16]; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { float s = 0; for (int k = 0; k < 4; ++k) s += a[r*4+k] * b[k*4+c]; t[r*4+c] = s; } memcpy(o, t, 64); }
static void matWorld(float* m, float x, float y, float z, float ang, float sc) {
    matIdent(m); float c = cos(ang) * sc, s = sin(ang) * sc; m[0] = c; m[2] = -s; m[5] = sc; m[8] = s; m[10] = c; m[12] = x; m[13] = y; m[14] = z; }
static void matLookProj(float* out, float ex, float ey, float ez, float fovScale) {
    // simple look-at toward origin (LH) * perspective
    float zx = -ex, zy = -ey, zz = -ez; float zl = sqrt(zx*zx + zy*zy + zz*zz); zx /= zl; zy /= zl; zz /= zl;
    float xx = zz, xy = 0, xz = -zx; float xl = sqrt(xx*xx + xz*xz); xx /= xl; xz /= xl;
    float yx = zy*xz - zz*xy, yy = zz*xx - zx*xz, yz = zx*xy - zy*xx;
    float v[16] = { xx, yx, zx, 0,  xy, yy, zy, 0,  xz, yz, zz, 0,
                    -(xx*ex + xy*ey + xz*ez), -(yx*ex + yy*ey + yz*ez), -(zx*ex + zy*ey + zz*ez), 1 };
    float zn = 1, zf = 100, h = fovScale, w = fovScale * 0.75f;
    float p[16] = { w, 0, 0, 0,  0, h, 0, 0,  0, 0, zf/(zf-zn), 1,  0, 0, -zn*zf/(zf-zn), 0 };
    matMul(v, p, out);
}
static void pixelOpDirect(void* surf, DWORD x, DWORD y, DWORD v, DWORD mode) {
    struct { LONG l, t, rr, b; } rc = { (LONG)x, (LONG)y, (LONG)x + 1, (LONG)y + 1 };
    struct { INT pitch; void* bits; } lr = { 0, NULL };
    ((HRESULT(WINAPI*)(void*, void*, void*, DWORD))vt(surf)[13])(surf, &lr, &rc, 0);
    if (lr.bits) switch (mode) { case 1: *(BYTE*)lr.bits = (BYTE)v; break; case 2: *(WORD*)lr.bits = (WORD)v; break; case 4: *(DWORD*)lr.bits = v; break; case 0x100: ((BYTE*)lr.bits)[3] = (BYTE)v; break; }
    ((HRESULT(WINAPI*)(void*))vt(surf)[14])(surf);
}
static void __cdecl pixelOpDirect_cdecl(void* surf, DWORD x, DWORD y, DWORD v, DWORD mode) { pixelOpDirect(surf, x, y, v, mode); }
static void __cdecl shroudRectDirect(void* surf, DWORD l, DWORD t, DWORD r, DWORD b, const BYTE* rows, DWORD bpp) {   // what the engine does directly
    struct { LONG l, t, rr, b; } rc = { (LONG)l, (LONG)t, (LONG)r, (LONG)b }; struct { INT pitch; void* bits; } lr = { 0, NULL };
    ((HRESULT(WINAPI*)(void*, void*, void*, DWORD))vt(surf)[13])(surf, &lr, &rc, 0x800);
    DWORD rowBytes = (r - l) * bpp;
    if (lr.bits) for (DWORD y = 0; y < b - t; ++y) memcpy((BYTE*)lr.bits + (INT)y * lr.pitch, rows + y * rowBytes, rowBytes);
    ((HRESULT(WINAPI*)(void*))vt(surf)[14])(surf);
}
static void radarModify(BYTE* bits, INT pitch, DWORD w, DWORD h, DWORD op, DWORD e) {
    for (DWORD y = 0; y < h; ++y) for (DWORD x = 0; x < w; ++x) {
        DWORD* px = (DWORD*)(bits + (INT)y * pitch + x * 4);
        *px = op == 1 ? (*px * 3 + e + (x ^ (y << 3))) : ((*px >> 1) ^ (e * (x + 1)));
    }
}
static void __cdecl radarDirect(void* surf, DWORD op, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e) {      // what the engine does directly
    struct { LONG l, t, rr, b; } rc = { (LONG)a, (LONG)b, (LONG)c, (LONG)d };
    struct { INT pitch; void* bits; } lr = { 0, NULL };
    HRESULT hr = ((HRESULT(WINAPI*)(void*, void*, void*, DWORD))vt(surf)[13])(surf, &lr, op == 1 ? &rc : NULL, op == 2 ? 0 : 0x800);
    if (FAILED(hr) || !lr.bits) return;
    if (op == 2) { for (int y = 0; y < 128; ++y) memset((BYTE*)lr.bits + y * lr.pitch, (BYTE)a ? 0xFF : 0x00, 128 * 4); }
    else radarModify((BYTE*)lr.bits, lr.pitch, op == 1 ? c - a : 128, op == 1 ? d - b : 128, op, e);
    ((HRESULT(WINAPI*)(void*))vt(surf)[14])(surf);
}
static unsigned long long fnv(const BYTE* p, size_t n, unsigned long long h) { for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; } return h; }

static LONG g_vehN = 0, g_vehOn = 0;
static LONG CALLBACK harnessVeh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ((code & 0x80000000u) && g_vehOn && g_vehN < 12) {
        g_vehN++;
        void* at = ep->ExceptionRecord->ExceptionAddress;
        char mod[MAX_PATH]; mod[0] = 0; HMODULE hm = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &hm);
        if (hm) GetModuleFileNameA(hm, mod, MAX_PATH);
        printf("VEH %08X at %p rva %08X in %s thread %lu access %p addr %p\n",
               (unsigned)code, at, (unsigned)((BYTE*)at - (BYTE*)hm), mod[0] ? mod : "?", GetCurrentThreadId(),
               (void*)ep->ExceptionRecord->ExceptionInformation[0], (void*)ep->ExceptionRecord->ExceptionInformation[1]);
        CONTEXT* c = ep->ContextRecord;
        printf("   eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X esp %08X ebp %08X\n",
               c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp, c->Ebp);
        DWORD* sp = (DWORD*)c->Esp;
        for (int i = 0, shown = 0; i < 160 && shown < 10; ++i) {
            DWORD v = 0;
            __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            HMODULE h2 = NULL;
            if (v > 0x10000 && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)v, &h2) && h2) {
                char m2[MAX_PATH]; m2[0] = 0; GetModuleFileNameA(h2, m2, MAX_PATH);
                const char* b = strrchr(m2, '\\'); b = b ? b + 1 : m2;
                printf("   stack[%d] %08X = %s+%08X\n", i, v, b, (unsigned)(v - (DWORD)(ULONG_PTR)h2));
                shown++;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
// the waits: what the window got, and when
static volatile LONG g_hMsg77 = 0, g_hMsg78 = 0, g_hMsg78InWait = -1; static LONG (__cdecl* g_hWaiting)() = NULL;
static LRESULT CALLBACK hWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_USER + 77) { InterlockedIncrement(&g_hMsg77); return 0; }
    if (m == WM_USER + 78) { g_hMsg78InWait = g_hWaiting ? g_hWaiting() : -2; InterlockedIncrement(&g_hMsg78); return 0; }
    return DefWindowProcA(h, m, w, l);
}
static DWORD WINAPI hSender(LPVOID hwnd) { Sleep(60); SendMessageA((HWND)hwnd, WM_USER + 78, 0, 0); return 0; }
static volatile LONG g_hWd = 0;
static DWORD WINAPI hWdProc(LPVOID) { Sleep(20000); if (g_hWd) { printf("HUNG: the pump test did not come back in 20 s\n"); fflush(stdout); ExitProcess(9); } return 0; }
// (pcf) the game's shadow lookup as its source must have been: the game's compiler (d3dx9_27) makes the very instructions of the game's shaders from it
static const char kFxPcf[] =
    "struct ShadowSetup { float4x4 WorldToShadow; float4 Zero_Zero_OneOverMapSize_OneOverMapSize; };\n"
    "ShadowSetup ShadowInfo;\n"
    "texture ShadowMap;\n"
    "sampler2D ShadowMapSampler = sampler_state { Texture = <ShadowMap>; MinFilter = POINT; MagFilter = POINT; MipFilter = NONE; AddressU = CLAMP; AddressV = CLAMP; };\n"
    "float4 PcfPS(float3 sc : TEXCOORD0, float4 col : COLOR0) : COLOR {\n"
    "  float4 d;\n"
    "  d.x = tex2D(ShadowMapSampler, sc.xy).x;\n"
    "  d.y = tex2D(ShadowMapSampler, sc.xy + ShadowInfo.Zero_Zero_OneOverMapSize_OneOverMapSize.zx).x;\n"
    "  d.z = tex2D(ShadowMapSampler, sc.xy + ShadowInfo.Zero_Zero_OneOverMapSize_OneOverMapSize.yz).x;\n"
    "  d.w = tex2D(ShadowMapSampler, sc.xy + ShadowInfo.Zero_Zero_OneOverMapSize_OneOverMapSize.wz).x;\n"
    "  float4 lit = (d - (sc.z - 0.002) >= 0) ? 1.0 : 0.0;\n"
    "  float s = dot(lit, 1.0) * 0.25;\n"
    "  return float4(col.rgb * (0.3 + 0.7 * s), 1.0); }\n"
    "technique Pcf { pass p0 { PixelShader = compile ps_2_0 PcfPS(); ZEnable = FALSE; CullMode = NONE; AlphaBlendEnable = FALSE; AlphaTestEnable = FALSE; } }\n"
    ;
enum { PCF_N = 64, PCF_X0 = 376, PCF_Y0 = 8, PCF_W = 256 };
static float g_pcfMap[PCF_N * PCF_N];
static const float kPcfU0 = 0.25f + 1.0f / (PCF_N * 32.0f);
static float pcfAt(int i, int j) { if (i < 0) i = 0; if (j < 0) j = 0; if (i >= PCF_N) i = PCF_N - 1; if (j >= PCF_N) j = PCF_N - 1; return g_pcfMap[j * PCF_N + i]; }
static double pcfRef(int x, int y, bool nine) {                         // the lit fraction at pixel (x, y) of the rectangle
    double px = ((double)kPcfU0 + 0.5 * (x + 0.5) / PCF_W) * PCF_N, py = ((double)kPcfU0 + 0.5 * (y + 0.5) / PCF_W) * PCF_N;
    int i = (int)floor(px), j = (int)floor(py); double fx = px - i, fy = py - j, s = 0;
    if (!nine) { for (int b = 0; b < 2; ++b) for (int a = 0; a < 2; ++a) s += pcfAt(i + a, j + b) >= 0.498f ? 0.25 : 0.0; return s; }
    double wx[3] = { 0.5 - 0.5 * fx, 0.5, 0.5 * fx }, wy[3] = { 0.5 - 0.5 * fy, 0.5, 0.5 * fy };
    for (int b = 0; b < 3; ++b) for (int a = 0; a < 3; ++a) s += wx[a] * wy[b] * (pcfAt(i - 1 + a, j - 1 + b) >= 0.498f ? 1.0 : 0.0);
    return s;
}
int main(int argc, char** argv) {
    AddVectoredExceptionHandler(1, harnessVeh);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 4) { printf("usage: rt_harness off|on <frames> <out.txt>\n"); return 2; }
    bool rt = strcmp(argv[1], "on") == 0; int frames = atoi(argv[2]);
    bool optNoFpu = false, optBench = false, optWrap = false;
    bool optFxWrap = false;                               // the effect value table's counter starts 64 drops short of its 32-bit wrap: the wrap falls inside the run, and the pictures must not change
    int optTex = 0;                                       // texture decoding on worker threads: 1 on, 2 on with every result checked against D3DX on a real texture
    bool optStream = false;                               // textures loaded by D3DX in the middle of every frame, drawn with at once, released (its own reference: run "off ... texstream" first)
    bool optPanId = false;                                // pan pictures, test: every picture shown goes through the GPU rectangle with the camera it was drawn from and must come out the same (with a tween mode)
    bool optPanSim = false;                               // pan pictures, live: a camera scrolling on the clock, frames of uneven length, a stall now and then (with tweenslow)
    bool optShadowMatch = false;                          // ... the size matched to the map's reach: maps come and go with different numbers
    bool optPanStop = false;                              // pan pictures, live: short scrolls that begin and end while the render thread is on (the plain pan test's scrolls end while it is switched off)
    bool optFakeClock = false;                            // the screen's refreshes made up by the accelerator itself (60 a second): pictures are timed by "the screen" although no monitor is awake
    bool optPcfOn = false;                                // ... with the accelerator's SmoothShadows switched on
    bool optPcf = false;                                  // smooth shadows: a rectangle through a shader with the game's shadow lookup, against the four-point and the nine-point arithmetic
    bool optShadowScale = false;                          // the finer shadow map: the tick against a wanted size the harness keeps (it plays the engine: gives less once, changes map once)
    bool optPumpTest = false;                             // the waits: a message the render thread sends must get through; a message from elsewhere must not be handled inside a busy wait
    bool optDump = false;                                 // picture dumps: two pairs (in-between + game) and two pictures shown from another camera, as files next to the DLL
    bool optPanSlow = false;                              // ... with frames as long as a battle's (30 to 49 ms): the game's pictures then come slower than a 60 Hz screen refreshes, and pictures go up on the screen's beat (v66)
    bool optTerrain = false;                              // ground-patch-style textures: created, level 0 written, the levels below made, drawn with, kept four frames (its own reference: run "off ... terrain" first)
    int optTween = 0; bool optPress = false;              // in-between frames: 4 frames drawn off screen, 1 every frame's stretch run again and compared, 2 draws left out, 8 shown late, 32 as in a slow game
    for (int i = 4; i < argc; ++i) { if (!strcmp(argv[i], "nofpu")) optNoFpu = true; else if (!strcmp(argv[i], "bench")) optBench = true; else if (!strcmp(argv[i], "wrap")) optWrap = true; else if (!strcmp(argv[i], "fxwrap")) optFxWrap = true;
        else if (!strcmp(argv[i], "tweenoff")) optTween = 4; else if (!strcmp(argv[i], "tween")) optTween = 1 | 4; else if (!strcmp(argv[i], "tweenbad")) optTween = 1 | 2 | 4;
        else if (!strcmp(argv[i], "tweenlate")) optTween = 1 | 4 | 8; else if (!strcmp(argv[i], "tweenslow")) optTween = 32;
        else if (!strcmp(argv[i], "tweenpress")) { optTween = 1 | 4; optPress = true; }
        else if (!strcmp(argv[i], "tweenmid")) optTween = 1 | 4 | 8 | 64;      // a waiting frame is presented in the middle of the next frame's second pass
        else if (!strcmp(argv[i], "texjobs")) optTex = 1; else if (!strcmp(argv[i], "texcheck")) optTex = 2; else if (!strcmp(argv[i], "texstream")) optStream = true; else if (!strcmp(argv[i], "terrain")) optTerrain = true; else if (!strcmp(argv[i], "panid")) optPanId = true; else if (!strcmp(argv[i], "pansim")) optPanSim = true; else if (!strcmp(argv[i], "panslow")) { optPanSim = true; optPanSlow = true; } else if (!strcmp(argv[i], "dump")) optDump = true; else if (!strcmp(argv[i], "pumptest")) optPumpTest = true; else if (!strcmp(argv[i], "shadowscale")) optShadowScale = true; else if (!strcmp(argv[i], "shadowmatch")) optShadowMatch = true; else if (!strcmp(argv[i], "pcf")) optPcf = true; else if (!strcmp(argv[i], "fakeclock")) optFakeClock = true; else if (!strcmp(argv[i], "panstop")) { optPanSim = true; optPanStop = true; } else if (!strcmp(argv[i], "pcfon")) { optPcf = true; optPcfOn = true; } }
    typedef void (__cdecl* TwMarkF)(void*); TwMarkF twMark = NULL;
    // MULTITHREADED: the game runs on DXVK, which is safe to call from more than one thread; the Microsoft
    // runtime is only safe with this flag, and without it the render thread faults inside it. nofpu: device
    // may change the thread's x87 precision.
    DWORD behavior = (optNoFpu ? 0x40u : (0x40u | 0x2u)) | 0x4u;
    SetEnvironmentVariableA("DXVK_CONFIG", "dxvk.hud=");
    SetEnvironmentVariableA("DXVK_LOG_LEVEL", "none");
    SetEnvironmentVariableA("DXVK_STATE_CACHE", "0");
    HMODULE hx = LoadLibraryA("d3dx9_27.dll"); if (!hx) { printf("no d3dx9_27\n"); return 3; }
    HMODULE hd3d = LoadLibraryA("d3d9.dll"); if (!hd3d) { printf("no d3d9\n"); return 3; }
    typedef int (__cdecl* InstallF)(void*, void*); typedef void (__cdecl* StatsF)(char*, int); typedef void* (__cdecl* ScopedF)(const char*); typedef void (__cdecl* ToggleF)(int); ToggleF rtToggle = NULL;
    InstallF rtInstall = NULL; StatsF rtStats = NULL; ScopedF rtScoped = NULL;
    typedef void (__cdecl* PixelF)(void*, DWORD, DWORD, DWORD, DWORD); PixelF pixelOp = pixelOpDirect_cdecl;
    typedef void (__cdecl* ShroudF)(void*, DWORD, DWORD, DWORD, DWORD, const BYTE*, DWORD); ShroudF shroudRect = shroudRectDirect;
    typedef void (__cdecl* RadarF)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD); RadarF radarOp = radarDirect;
    if (rt) {
        HMODULE ha = LoadLibraryA("bfme2_accel.new.dll"); if (!ha) { printf("no bfme2_accel.new.dll (%lu)\n", GetLastError()); return 3; }
        rtInstall = (InstallF)GetProcAddress(ha, "AotrRtTestInstall"); rtStats = (StatsF)GetProcAddress(ha, "AotrRtTestStats");
        rtScoped = (ScopedF)GetProcAddress(ha, "AotrRtTestScopedD3DX");
        rtToggle = (ToggleF)GetProcAddress(ha, "AotrRtTestToggle");
        pixelOp = (PixelF)GetProcAddress(ha, "AotrRtTestPixelOp"); if (!pixelOp) { printf("missing AotrRtTestPixelOp\n"); return 3; }
        shroudRect = (ShroudF)GetProcAddress(ha, "AotrRtTestShroudRect"); if (!shroudRect) { printf("missing AotrRtTestShroudRect\n"); return 3; }
        radarOp = (RadarF)GetProcAddress(ha, "AotrRtTestRadarOp"); if (!radarOp) { printf("missing AotrRtTestRadarOp\n"); return 3; }
        if (optWrap) { typedef void (__cdecl* SeqF)(DWORD); SeqF sb = (SeqF)GetProcAddress(ha, "AotrRtTestSeqBase"); if (!sb) { printf("missing AotrRtTestSeqBase\n"); return 3; } sb(0xFFFFC000u); printf("queue sequence starts 16384 records before the 32-bit wrap\n"); }
        if (optFxWrap) { typedef DWORD (__cdecl* FeF)(int, DWORD); FeF fe = (FeF)GetProcAddress(ha, "AotrRtTestFxEpoch"); if (!fe) { printf("missing AotrRtTestFxEpoch\n"); return 3; } fe(1, 0xFFFFFFC0u); printf("effect value counter starts 64 drops before the 32-bit wrap\n"); }
        if (!rtInstall || !rtStats || !rtScoped) { printf("missing test exports\n"); return 3; }
        Sleep(1500);                                      // let the DLL's own init thread finish its patching
        { typedef int (__cdecl* StF)(char*, int); StF st = (StF)GetProcAddress(ha, "AotrScreenClockSelfTest"); static char sb[512]; sb[0] = 0; if (st && optPanSim) { st(sb, sizeof(sb)); printf("%s\n", sb); } }
        if (optFakeClock) { typedef void (__cdecl* FkF)(int); FkF fk = (FkF)GetProcAddress(ha, "AotrScreenClockFake"); if (!fk) { printf("missing AotrScreenClockFake\n"); return 3; } fk(16667); }
    }
    typedef void* (WINAPI* Create9F)(UINT);
    typedef HRESULT (WINAPI* CreateEffectF)(void*, const char*, UINT, void*, void*, DWORD, void*, void**, void**);
    typedef HRESULT (WINAPI* CreateTextureF)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**);
    typedef HRESULT (WINAPI* FilterTextureF)(void*, void*, UINT, DWORD);
    Create9F Create9 = (Create9F)GetProcAddress(hd3d, "Direct3DCreate9");
    CreateEffectF CreateEffect = (CreateEffectF)GetProcAddress(hx, "D3DXCreateEffect");
    CreateTextureF D3DXCreateTexture = (CreateTextureF)GetProcAddress(hx, "D3DXCreateTexture");
    FilterTextureF D3DXFilterTexture = (FilterTextureF)GetProcAddress(hx, "D3DXFilterTexture");
    typedef HRESULT (WINAPI* CreateFromFileF)(void*, const void*, UINT, UINT, UINT, UINT, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, void*, void*, void**);
    typedef HRESULT (WINAPI* LoadSurfMemF)(void*, const void*, const RECT*, const void*, DWORD, UINT, const void*, const RECT*, DWORD, DWORD);
    CreateFromFileF D3DXCreateFromFile = (CreateFromFileF)GetProcAddress(hx, "D3DXCreateTextureFromFileInMemoryEx");
    LoadSurfMemF D3DXLoadSurfMem = (LoadSurfMemF)GetProcAddress(hx, "D3DXLoadSurfaceFromMemory");

    WNDCLASSA wc = {0}; wc.lpfnWndProc = hWndProc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "rtharness";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "rtharness", "rt", WS_POPUP, -3000, -3000, 640, 480, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    void* d3d = Create9(32); if (!d3d) { printf("Direct3DCreate9 failed\n"); return 4; }
    PP pp = {0}; pp.BackBufferWidth = 640; pp.BackBufferHeight = 480; pp.BackBufferFormat = 21; pp.BackBufferCount = 1;
    pp.SwapEffect = 1; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE; pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = 75; pp.PresentationInterval = 0x80000000;
    void* dev = NULL;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, DWORD, HWND, DWORD, PP*, void**))vt(d3d)[16])(d3d, 0, 1, hwnd, behavior, &pp, &dev);
    if (FAILED(hr) || !dev) { printf("CreateDevice hr=0x%08lX\n", (unsigned long)hr); return 4; }

    void* fx = NULL, *err = NULL;
    hr = CreateEffect(dev, kFx, (UINT)strlen(kFx), NULL, NULL, 0, NULL, &fx, &err);
    if (FAILED(hr) || !fx) { printf("D3DXCreateEffect hr=0x%08lX %s\n", (unsigned long)hr, err ? (char*)((void*(WINAPI*)(void*))vt(err)[3])(err) : ""); return 5; }
    #define H(name) ((DWORD(WINAPI*)(void*, DWORD, const char*))vt(fx)[9])(fx, 0, name)
    DWORD hWorld = H("World"), hVP = H("ViewProj"), hLVP = H("LightViewProj"), hBones = H("Bones"), hColor = H("MatColor"),
          hBlend = H("Blend"), hTint = H("Tint"), hDiff = H("DiffuseTex"), hShadow = H("ShadowTex");
    DWORD tMain = ((DWORD(WINAPI*)(void*, const char*))vt(fx)[13])(fx, "Main"), tShadow = ((DWORD(WINAPI*)(void*, const char*))vt(fx)[13])(fx, "_CreateShadowMap");
    if (!hWorld || !hBones || !tMain || !tShadow) { printf("handles missing\n"); return 5; }
    static const char kLong[] = "ThisParameterNameIsLongerThanFortyCharactersForTheCache";
    DWORD hLong = H(kLong), hMat = H("Mat");
    DWORD hMatVal = ((DWORD(WINAPI*)(void*, DWORD, const char*))vt(fx)[9])(fx, hMat, "Val");
    DWORD hMatValByName = ((DWORD(WINAPI*)(void*, const char*, const char*))vt(fx)[9])(fx, "Mat", "Val");
    if (!hLong || !hMat || !hMatVal || hMatVal != hMatValByName) { printf("v5 handles missing or inconsistent (%08lX %08lX %08lX %08lX)\n", hLong, hMat, hMatVal, hMatValByName); return 5; }

    // resources
    struct V { float x, y, z, nx, ny, nz, u, v; };
    V cube[24]; WORD idx[36]; int vi = 0, ii = 0;
    const float n[6][3] = {{0,0,-1},{0,0,1},{-1,0,0},{1,0,0},{0,1,0},{0,-1,0}};
    for (int f = 0; f < 6; ++f) {
        float nx = n[f][0], ny = n[f][1], nz = n[f][2];
        float ux = ny ? 1 : (nz ? 1 : 0), uy = 0, uz = ny ? 0 : (nx ? 1 : 0);
        float wx = ny*uz - nz*uy, wy = nz*ux - nx*uz, wz = nx*uy - ny*ux;
        for (int k = 0; k < 4; ++k) { float a = (k == 0 || k == 3) ? -1.0f : 1.0f, b = (k < 2) ? -1.0f : 1.0f;
            V& q = cube[vi + k]; q.x = nx + a*ux + b*wx; q.y = ny + a*uy + b*wy; q.z = nz + a*uz + b*wz; q.nx = nx; q.ny = ny; q.nz = nz; q.u = (a+1)/2; q.v = (b+1)/2; }
        WORD b0 = (WORD)vi; WORD t[6] = {0,1,2,0,2,3}; for (int k = 0; k < 6; ++k) idx[ii++] = b0 + t[k]; vi += 4;
    }
    void* vb = NULL, *ib = NULL, *dvb = NULL, *tex = NULL, *dyntex = NULL, *srt = NULL, *srtSurf = NULL, *sz = NULL, *bb = NULL, *bbz = NULL, *cap = NULL, *sys = NULL, *decl = NULL, *sblock = NULL;
    CALL6(dev, 26, sizeof(cube), 0, 0, 1, &vb, 0); void* p = NULL;
    CALL4(vb, 11, 0, 0, &p, 0); memcpy(p, cube, sizeof(cube)); CALL0(vb, 12);
    CALL6(dev, 27, sizeof(idx), 0, 101, 1, &ib, 0); CALL4(ib, 11, 0, 0, &p, 0); memcpy(p, idx, sizeof(idx)); CALL0(ib, 12);
    CALL6(dev, 26, 600 * 20, 0x200 | 8, 0x4 | 0x40, 0, &dvb, 0);
    CALL8(dev, 23, 64, 64, 0, 0, 21, 1, &tex, 0);
    { LOCKED_RECT lr; CALL4(tex, 19, 0, &lr, 0, 0); for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) ((DWORD*)lr.pBits)[y*64+x] = 0xFF000000 | ((x*4) << 16) | ((y*4) << 8) | (((x^y) & 8) ? 0xFF : 0x40); CALL1(tex, 20, 0); }
    CALL8(dev, 23, 32, 32, 1, 0, 21, 1, &dyntex, 0);
    CALL8(dev, 23, 256, 256, 1, 1, 21, 0, &srt, 0); CALL2(srt, 18, 0, &srtSurf);
    CALL8(dev, 29, 256, 256, 75, 0, 0, 1, &sz, 0);
    CALL4(dev, 18, 0, 0, 0, &bb); CALL1(dev, 40, &bbz);
    CALL8(dev, 28, 640, 480, 21, 0, 0, 0, &cap, 0);
    CALL6(dev, 36, 640, 480, 21, 2, &sys, 0);
    VELEM el[4] = { {0,0,2,0,0,0}, {0,12,2,0,3,0}, {0,24,1,0,5,0}, {0xFF,0,17,0,0,0} };
    CALL2(dev, 86, el, &decl);
    CALL2(dev, 59, 1, &sblock);
    if (!vb || !ib || !dvb || !tex || !dyntex || !srtSurf || !sz || !bb || !bbz || !cap || !sys || !decl || !sblock) { printf("resource creation failed\n"); return 6; }
    void* fxP = NULL, *pcfTex = NULL; DWORD tPcf = 0; int pcfFour = 0, pcfNine = 0, pcfNeither = 0, pcfWorst4 = 0, pcfWorst9 = 0;
    if (optPcf) {
        void* e2 = NULL;
        hr = CreateEffect(dev, kFxPcf, (UINT)strlen(kFxPcf), NULL, NULL, 0, NULL, &fxP, &e2);
        if (FAILED(hr) || !fxP) { printf("pcf: D3DXCreateEffect hr=0x%08lX %s\n", (unsigned long)hr, e2 ? (char*)((void*(WINAPI*)(void*))vt(e2)[3])(e2) : ""); return 5; }
        for (int j = 0; j < PCF_N; ++j) for (int i = 0; i < PCF_N; ++i) {
            float v = (i * 3 + j * 2 < 5 * PCF_N / 2 + 9) ? 1.0f : 0.0f;
            if ((i == 20 && j == 22) || (i == 23 && j == 27) || (i == 24 && j == 27)) v = 0.0f;
            if ((i == 41 && j == 40) || (i == 44 && j == 36)) v = 1.0f;
            g_pcfMap[j * PCF_N + i] = v; }
        CALL8(dev, 23, PCF_N, PCF_N, 1, 0, 114, 1, &pcfTex, 0);
        if (!pcfTex) { printf("pcf: no R32F texture\n"); return 6; }
        { LOCKED_RECT lr; CALL4(pcfTex, 19, 0, &lr, 0, 0); for (int j = 0; j < PCF_N; ++j) memcpy((BYTE*)lr.pBits + j * lr.Pitch, &g_pcfMap[j * PCF_N], PCF_N * 4); CALL1(pcfTex, 20, 0); }
        DWORD hSI = ((DWORD(WINAPI*)(void*, DWORD, const char*))vt(fxP)[9])(fxP, 0, "ShadowInfo");
        DWORD hInv = hSI ? ((DWORD(WINAPI*)(void*, DWORD, const char*))vt(fxP)[9])(fxP, hSI, "Zero_Zero_OneOverMapSize_OneOverMapSize") : 0;
        DWORD hMap = ((DWORD(WINAPI*)(void*, DWORD, const char*))vt(fxP)[9])(fxP, 0, "ShadowMap");
        tPcf = ((DWORD(WINAPI*)(void*, const char*))vt(fxP)[13])(fxP, "Pcf");
        if (!hInv || !hMap || !tPcf) { printf("pcf: handles missing\n"); return 5; }
        float inv[4] = { 0, 0, 1.0f / PCF_N, 1.0f / PCF_N };
        CALL2(fxP, 34, hInv, inv); CALL2(fxP, 52, hMap, pcfTex);
        if (rt && optPcfOn) { typedef void (__cdecl* PcF)(int, char*, int); PcF pc = (PcF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrShadowPcfTest"); if (!pc) { printf("missing AotrShadowPcfTest"); return 3; } pc(1, NULL, 0); }
    }
    void* sysSurf2 = NULL, *defTex = NULL, *defSurf = NULL, *sysTex = NULL, *defTex2 = NULL, *texLvl = NULL;
    CALL6(dev, 36, 64, 64, 21, 2, &sysSurf2, 0);                           // SYSTEMMEM offscreen plain surface
    CALL8(dev, 23, 64, 64, 1, 0, 21, 0, &defTex, 0); if (defTex) CALL2(defTex, 18, 0, &defSurf);   // DEFAULT texture
    CALL8(dev, 23, 64, 64, 1, 0, 21, 2, &sysTex, 0);                       // SYSTEMMEM texture
    CALL8(dev, 23, 64, 64, 1, 0, 21, 0, &defTex2, 0);                      // DEFAULT texture
    CALL2(tex, 18, 0, &texLvl);                                              // level of a MANAGED texture: not a render target
    if (!sysSurf2 || !defSurf || !sysTex || !defTex2 || !texLvl) { printf("resource creation (v4 cases) failed\n"); return 6; }
    int sysMismatch = 0, mirrorMismatch = 0, lockedMismatch = 0;
    void* pixTex = NULL, *pixSurf = NULL; CALL8(dev, 23, 64, 64, 1, 0, 21, 1, &pixTex, 0); if (pixTex) CALL2(pixTex, 18, 0, &pixSurf);   // MANAGED, like the minimap
    if (!pixSurf) { printf("pixel texture failed\n"); return 6; }
    void* shrTex32 = NULL, *shrSurf32 = NULL, *shrTex16 = NULL, *shrSurf16 = NULL;
    CALL8(dev, 23, 64, 64, 1, 0, 21, 1, &shrTex32, 0); if (shrTex32) CALL2(shrTex32, 18, 0, &shrSurf32);   // MANAGED A8R8G8B8, like the 32-bit shroud
    CALL8(dev, 23, 64, 64, 1, 0, 26, 1, &shrTex16, 0); if (shrTex16) CALL2(shrTex16, 18, 0, &shrSurf16);   // MANAGED A4R4G4B4, like the 16-bit shroud
    if (!shrSurf32 || !shrSurf16) { printf("shroud textures failed\n"); return 6; }
    struct TVB { float x, y, z, w; DWORD c; float u, v; };
    void* rmVB = NULL; CALL6(dev, 26, 8 * sizeof(TVB), 0, 0x4 | 0x40 | 0x100, 1, &rmVB, 0);         // MANAGED, plain locks read and modify it
    if (!rmVB) { printf("vertex buffer failed\n"); return 6; }
    { TVB* v = NULL; if (SUCCEEDED(CALL4(rmVB, 11, 0, 0, &v, 0)) && v) { for (int i = 0; i < 8; ++i) { TVB t = { 700.0f + (i % 2) * 64.0f, 20.0f + (i / 2) * 32.0f, 0, 1, 0xFF102030u + (DWORD)i * 0x00030507u, (float)(i % 2), (float)((i / 2) % 2) }; v[i] = t; } CALL0(rmVB, 12); } }
    void* radTex = NULL, *radSurf = NULL; CALL8(dev, 23, 128, 128, 1, 0, 21, 1, &radTex, 0); if (radTex) CALL2(radTex, 18, 0, &radSurf);   // MANAGED A8R8G8B8 128x128, like the radar overlay
    if (!radSurf) { printf("radar texture failed\n"); return 6; }
    void* atlasTex = NULL, *atlasSurf = NULL; CALL8(dev, 23, 256, 256, 1, 0, 25, 1, &atlasTex, 0); if (atlasTex) CALL2(atlasTex, 18, 0, &atlasSurf);   // MANAGED A1R5G5B5, like the terrain tile atlas
    if (!atlasSurf) { printf("atlas texture failed\n"); return 6; }
    void* atlas2Tex = NULL, *atlas2Surf = NULL; CALL8(dev, 23, 256, 256, 0, 0, 25, 1, &atlas2Tex, 0); if (atlas2Tex) CALL2(atlas2Tex, 18, 0, &atlas2Surf);   // the same with a full mip chain, refiltered after every update
    if (!atlas2Surf) { printf("mip-mapped atlas texture failed\n"); return 6; }
    { LOCKED_RECT lr; if (SUCCEEDED(CALL3(atlas2Surf, 13, &lr, 0, 0))) { for (int y = 0; y < 256; ++y) for (int x = 0; x < 256; ++x) ((WORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = (WORD)(0x8000u | (((y >> 3) & 31) << 10) | (((x >> 3) & 31) << 5) | ((x + y) & 31)); CALL0(atlas2Surf, 14); } }
    { LOCKED_RECT lr; if (SUCCEEDED(CALL3(atlasSurf, 13, &lr, 0, 0))) { for (int y = 0; y < 256; ++y) for (int x = 0; x < 256; ++x) ((WORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = (WORD)(0x8000u | (((x >> 3) & 31) << 10) | (((y >> 3) & 31) << 5) | ((x ^ y) & 31)); CALL0(atlasSurf, 14); } }
    { LOCKED_RECT lr; if (SUCCEEDED(CALL3(radSurf, 13, &lr, 0, 0))) { for (int y = 0; y < 128; ++y) for (int x = 0; x < 128; ++x) ((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = 0x80000000u | (DWORD)(x * 2) << 16 | (DWORD)(y * 2) << 8 | (DWORD)((x * y) & 0xFF); CALL0(radSurf, 14); } }
    static BYTE shrRows[64 * 64 * 4];
    DWORD blockC = 0;
    static const GUID kIIDTex9 = {0x85c31227, 0x3de5, 0x4f00, {0x9b, 0x3a, 0xf1, 0x1a, 0xc3, 0x8c, 0x18, 0xb5}};
    BYTE refDesc[44], refTech[12]; DWORD refUsed = 0;
    memset(refDesc, 0, 44); memset(refTech, 0, 12);
    ((HRESULT(WINAPI*)(void*, DWORD, BYTE*))vt(fx)[4])(fx, hWorld, refDesc);
    ((HRESULT(WINAPI*)(void*, DWORD, BYTE*))vt(fx)[5])(fx, tMain, refTech);
    refUsed = ((DWORD(WINAPI*)(void*, DWORD, DWORD))vt(fx)[62])(fx, hWorld, tMain);
    int queryMismatch = 0, passMismatch = 0;
    UINT passesMain = 0, passesShadow = 0;
    { struct { const char* name; UINT passes, ann; } td;
      memset(&td, 0, sizeof(td)); ((HRESULT(WINAPI*)(void*, DWORD, void*))vt(fx)[5])(fx, tMain, &td); passesMain = td.passes;
      memset(&td, 0, sizeof(td)); ((HRESULT(WINAPI*)(void*, DWORD, void*))vt(fx)[5])(fx, tShadow, &td); passesShadow = td.passes; }
    // parameter block recorded before the render thread exists
    CALL0(fx, 73); { float col[4] = {1, 0.9f, 0.8f, 1}; CALL2(fx, 34, hColor, col); CALL2(fx, 30, hBlend, fbits(0.5f)); }
    DWORD blockA = ((DWORD(WINAPI*)(void*))vt(fx)[74])(fx);

    printf("resources ready (x87 control word now %04x), parameter block handle form %08lX\n", _control87(0, 0) & 0xFFFF, blockA);
    if (rt && optTex) { typedef void (__cdecl* SetupF)(int, int, int); SetupF ts = (SetupF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrTexTestSetup");
        if (!ts) { printf("missing AotrTexTestSetup\n"); return 3; } ts(-1, optTex == 2 ? 100000 : 0, 160); }
    if (rt) { int ok = rtInstall(dev, fx); printf("render thread install: %d\n", ok); if (!ok) return 7;
        { typedef void (__cdecl* AnyF)(int); AnyF any = (AnyF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrRtTestAtlasAny"); if (any) any(1); }
        D3DXCreateTexture = (CreateTextureF)rtScoped("D3DXCreateTexture"); D3DXFilterTexture = (FilterTextureF)rtScoped("D3DXFilterTexture");
        if (optPanId) { typedef void (__cdecl* PanF)(int); PanF pf = (PanF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrPanWarpTest"); if (!pf) { printf("missing AotrPanWarpTest\n"); return 3; } pf(1); }
        if (optPanSim) { typedef void (__cdecl* PanF)(int); PanF pf = (PanF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrPanWarpTest"); if (!pf) { printf("missing AotrPanWarpTest\n"); return 3; } pf(4); }
        if (optDump) { typedef void (__cdecl* DuF)(int, int); DuF du = (DuF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrTweenTestDump"); if (!du) { printf("missing AotrTweenTestDump\n"); return 3; } du(2, 2); }
        D3DXCreateFromFile = (CreateFromFileF)rtScoped("D3DXCreateTextureFromFileInMemoryEx"); D3DXLoadSurfMem = (LoadSurfMemF)rtScoped("D3DXLoadSurfaceFromMemory");
        if (!D3DXCreateFromFile || !D3DXLoadSurfMem) { printf("missing D3DX wrappers\n"); return 3; }
        if (optTween) { typedef void (__cdecl* ModeF)(int); HMODULE ha = GetModuleHandleA("bfme2_accel.new.dll");
            ModeF mode = (ModeF)GetProcAddress(ha, "AotrTweenTestMode"); twMark = (TwMarkF)GetProcAddress(ha, "AotrTweenTestMark");
            if (!mode || !twMark) { printf("missing in-between frame test exports\n"); return 3; }
            mode(optTween); printf("in-between frames: test mode %d\n", optTween); } }
    // a second parameter block recorded through the render thread
    CALL0(fx, 73); { float tint[4] = {0.9f, 1.0f, 1.1f, 1}; CALL2(fx, 34, hTint, tint); }
    DWORD blockB = ((DWORD(WINAPI*)(void*))vt(fx)[74])(fx);

    if (optBench) {                                                   // enqueue cost microbenchmark
        LARGE_INTEGER b0, b1, bf; QueryPerformanceFrequency(&bf);
        QueryPerformanceCounter(&b0);
        for (int i = 0; i < 2000000; ++i) CALL2(dev, 57, 27, i & 1);          // SetRenderState ALPHABLENDENABLE
        QueryPerformanceCounter(&b1);
        printf("bench SetRenderState: %.1f ns/call\n", (double)(b1.QuadPart - b0.QuadPart) * 1e9 / bf.QuadPart / 2000000);
        QueryPerformanceCounter(&b0);
        for (int i = 0; i < 1000000; ++i) CALL4(dev, 100, 0, vb, 0, 32);        // SetStreamSource (object AddRef/Release)
        QueryPerformanceCounter(&b1);
        printf("bench SetStreamSource: %.1f ns/call\n", (double)(b1.QuadPart - b0.QuadPart) * 1e9 / bf.QuadPart / 1000000);
        float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        QueryPerformanceCounter(&b0);
        for (int i = 0; i < 1000000; ++i) ((HRESULT(WINAPI*)(void*, DWORD, const void*, UINT, UINT))vt(fx)[78])(fx, hWorld, m, 0, 64);
        QueryPerformanceCounter(&b1);
        printf("bench SetRawValue(64B): %.1f ns/call\n", (double)(b1.QuadPart - b0.QuadPart) * 1e9 / bf.QuadPart / 1000000);
        { typedef void (__cdecl* Stats4F)(char*, int); Stats4F s4 = (Stats4F)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrRtTestStatsV4");
          char st[512]; if (s4) { s4(st, sizeof(st)); printf("  after SetRawValue: %s\n", st); } }
        { // parameter writes that keep changing: the value cache misses every time
          float mm[16]; for (int k = 0; k < 16; ++k) mm[k] = (float)k;
          QueryPerformanceCounter(&b0);
          for (int i = 0; i < 1000000; ++i) { mm[0] = (float)i; ((HRESULT(WINAPI*)(void*, DWORD, const void*, UINT, UINT))vt(fx)[78])(fx, hWorld, mm, 0, 64); }
          QueryPerformanceCounter(&b1);
          printf("bench SetRawValue(64B, changing): %.1f ns/call\n", (double)(b1.QuadPart - b0.QuadPart) * 1e9 / bf.QuadPart / 1000000); }
        { float v4[4] = {1,2,3,4};
          QueryPerformanceCounter(&b0);
          for (int i = 0; i < 1000000; ++i) { v4[0] = (float)i; ((HRESULT(WINAPI*)(void*, DWORD, const void*))vt(fx)[34])(fx, hWorld, v4); }
          QueryPerformanceCounter(&b1);
          printf("bench SetVector(changing): %.1f ns/call\n", (double)(b1.QuadPart - b0.QuadPart) * 1e9 / bf.QuadPart / 1000000); }
        { // what the game thread pays: a battle-like run of effect parameter writes closed by device calls, enqueued in
          // bursts small enough never to wait for the worker; the backlog is drained between bursts, outside the timing
          float mm[16]; for (int k = 0; k < 16; ++k) mm[k] = (float)k;
          float v4[4] = {1, 2, 3, 4};
          LONGLONG ticks = 0; LONG calls = 0;
          typedef void (__cdecl* HoldF)(int); HoldF hold = (HoldF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrRtTestHold");
          for (int burst = 0; burst < 24; ++burst) {
              if (hold) hold(1);                                                                      // worker parked: the enqueue path alone
              QueryPerformanceCounter(&b0);
              for (int i = 0; i < 3000; ++i) {
                  mm[0] = (float)i; v4[0] = (float)(i + burst);
                  ((HRESULT(WINAPI*)(void*, DWORD, const void*))vt(fx)[44])(fx, hWorld, mm);          // SetMatrixTranspose
                  ((HRESULT(WINAPI*)(void*, DWORD, const void*))vt(fx)[44])(fx, hLVP, mm);
                  for (int k = 0; k < 4; ++k) ((HRESULT(WINAPI*)(void*, DWORD, const void*))vt(fx)[34])(fx, hTint, v4);      // SetVector
                  for (int k = 0; k < 3; ++k) ((HRESULT(WINAPI*)(void*, DWORD, DWORD))vt(fx)[30])(fx, hBlend, fbits(0.5f + k)); // SetFloat
                  for (int k = 0; k < 2; ++k) ((HRESULT(WINAPI*)(void*, DWORD, DWORD))vt(fx)[26])(fx, hBlend, (DWORD)k);        // SetInt
                  for (int k = 0; k < 2; ++k) ((HRESULT(WINAPI*)(void*, DWORD, void*))vt(fx)[52])(fx, hDiff, tex);              // SetTexture
                  for (int k = 0; k < 3; ++k) ((HRESULT(WINAPI*)(void*, DWORD, const void*, UINT, UINT))vt(fx)[78])(fx, hWorld, mm, 0, 64);   // SetRawValue
                  CALL2(dev, 57, 27, i & 1);                                                          // SetRenderState: whatever follows the run
                  CALL4(dev, 100, 0, vb, 0, 32);                                                      // SetStreamSource
              }
              QueryPerformanceCounter(&b1);
              ticks += b1.QuadPart - b0.QuadPart; calls += 3000 * 18;
              if (hold) hold(0);
              CALL2(dev, 32, cap, sys);                                                               // drain, untimed
          }
          printf("bench game-like effect run (16 parameter writes + 2 device calls), enqueue only: %.1f ns/call\n", (double)ticks * 1e9 / bf.QuadPart / calls); }
        { typedef void (__cdecl* CpuF)(char*, int); CpuF cf = (CpuF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrRtTestCpu");
          char st[1024]; if (cf) { cf(st, sizeof(st)); printf("  cpus during the first three benches: %s\n", st); } }
        { typedef void (__cdecl* ThrF)(char*, int); ThrF tf = (ThrF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrRtTestThreads");
          static char st[8192]; if (tf) { tf(st, sizeof(st)); printf("  threads:\n%s", st); } }
        BYTE d[44];
        QueryPerformanceCounter(&b0);
        for (int i = 0; i < 1000000; ++i) ((HRESULT(WINAPI*)(void*, DWORD, BYTE*))vt(fx)[4])(fx, hWorld, d);
        QueryPerformanceCounter(&b1);
        printf("bench GetParameterDesc: %.1f ns/call\n", (double)(b1.QuadPart - b0.QuadPart) * 1e9 / bf.QuadPart / 1000000);
        { LOCKED_RECT lr0; QueryPerformanceCounter(&b0);
          for (int i = 0; i < 200000; ++i) { CALL2(dev, 57, 27, i & 1); if (SUCCEEDED(CALL3(sysSurf2, 13, &lr0, 0, 0))) CALL0(sysSurf2, 14); }
          QueryPerformanceCounter(&b1);
          printf("bench SetRenderState + sysmem LockRect/UnlockRect: %.1f ns/iteration\n", (double)(b1.QuadPart - b0.QuadPart) * 1e9 / bf.QuadPart / 200000); }
        QueryPerformanceCounter(&b0);
        CALL2(dev, 32, cap, sys);                                               // sync point: wait for the worker to finish the backlog
        QueryPerformanceCounter(&b1);
        printf("bench drain of the backlog: %.1f ms\n", (double)(b1.QuadPart - b0.QuadPart) * 1e3 / bf.QuadPart);
        { typedef void (__cdecl* Stats4F)(char*, int); Stats4F s4 = (Stats4F)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrRtTestStatsV4");
          char st[512]; if (s4) { s4(st, sizeof(st)); printf("bench wait stats: %s\n", st); } }
        return 0;
    }
    printf("entering frame loop\n");
    g_vehOn = 1;
    FILE* out = fopen(argv[3], "w");
    LARGE_INTEGER qf, t0, t1; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&t0);
    void* extraTex = NULL;
    if (rt && optPumpTest) {
        typedef void (__cdecl* SendF)(void*, DWORD); typedef void (__cdecl* BusyF)(DWORD, DWORD); typedef LONG (__cdecl* LF)();
        HMODULE hm = GetModuleHandleA("bfme2_accel.new.dll");
        SendF snd = (SendF)GetProcAddress(hm, "AotrRtTestSend"); BusyF busy = (BusyF)GetProcAddress(hm, "AotrRtTestBusy"); LF pumps = (LF)GetProcAddress(hm, "AotrRtTestPumps"); g_hWaiting = (LF)GetProcAddress(hm, "AotrRtTestWaiting");
        if (!snd || !busy || !pumps || !g_hWaiting) { printf("missing pump test exports\n"); return 3; }
        g_hWd = 1; CloseHandle(CreateThread(NULL, 0, hWdProc, NULL, 0, NULL));
        LARGE_INTEGER qf, t0, t1; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&t0);
        snd(hwnd, WM_USER + 77);                                      // the render thread sends this window a message while this thread waits for the queue
        QueryPerformanceCounter(&t1);
        int ms1 = (int)((t1.QuadPart - t0.QuadPart) * 1000 / qf.QuadPart);
        LONG p0 = pumps();
        HANDLE th = CreateThread(NULL, 0, hSender, hwnd, 0, NULL);   // another thread sends one 60 ms into ...
        busy(20, 15);                                                 // ... 300 ms of the render thread working, a record every 20 ms, waited for here
        LONG p1 = pumps(), inside = g_hMsg78;
        MSG msg; DWORD until = GetTickCount() + 2000; while (!g_hMsg78 && GetTickCount() < until) { PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE); Sleep(1); }
        WaitForSingleObject(th, 2000); CloseHandle(th);
        g_hWd = 0;
        bool ok = g_hMsg77 == 1 && inside == 0 && g_hMsg78 == 1 && g_hMsg78InWait == 0 && p1 == p0;
        printf("pump test %s: a message sent by the render thread during a wait for the queue %s (the wait took %d ms); a message from another thread during a busy wait was handled %s (messages let through during it: %ld)\n",
               ok ? "PASSED" : "FAILED", g_hMsg77 == 1 ? "got through" : "DID NOT get through", ms1, (inside == 0 && g_hMsg78 == 1 && g_hMsg78InWait == 0) ? "after the wait, as it must be" : "INSIDE the wait", (long)(p1 - p0));
    }
    // the engine's ShadowMap numbers as the harness plays them: [0] MapSize, [1] MaxViewDistance, [2] MinShadowedTerrainHeight - the current ones and the defaults
    static DWORD hWant[3] = { 2048, 0, 0 }, hDef[3] = { 2048, 0, 0 }, hDevSlot = 0; static char hSmLog[320]; int hSmN = 0;
    struct HSm { static void reach(DWORD* s, float r) { memcpy(&s[1], &r, 4); } };
    HSm::reach(hDef, 1500.0f); HSm::reach(hWant, optShadowMatch ? 6000.0f : 1500.0f);        // (match: the first map is one with a long reach, as Helm's Deep)
    if (rt && (optShadowScale || optShadowMatch)) { typedef void (__cdecl* SsF)(DWORD*, DWORD*, int, DWORD*, int, int); SsF ss = (SsF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrShadowScaleTest");
        if (!ss) { printf("missing AotrShadowScaleTest\n"); return 3; } hDevSlot = (DWORD)(ULONG_PTR)dev; ss(hWant, &hDevSlot, optShadowMatch ? 1 : 4, hDef, optShadowMatch ? 1 : 0, 4); }
    for (int fr = 0; fr < frames; ++fr) {
        if (optShadowScale || optShadowMatch) {
            static DWORD last = 0; if (hWant[0] != last && hSmN < 280) { hSmN += sprintf(hSmLog + hSmN, "%s%d:%lu", hSmN ? " " : "", fr, (unsigned long)hWant[0]); last = hWant[0]; }
            if (optShadowScale) {                                      // the engine's part: at frame 100 it "made" a smaller map than asked, at 200 a new map resets the size
                if (fr == 100 && hWant[0] == 8192) hWant[0] = 4096;
                if (fr == 200) hWant[0] = 1024;
            } else {                                                   // maps come and go: the default numbers, a reach of 2200, a map that already has 4096 over 6000, the default again
                if (fr == 100) { hWant[0] = 2048; HSm::reach(hWant, 1500.0f); }
                if (fr == 200) { hWant[0] = 2048; HSm::reach(hWant, 2200.0f); }
                if (fr == 300) { hWant[0] = 4096; HSm::reach(hWant, 6000.0f); }
                if (fr == 400) { hWant[0] = 2048; HSm::reach(hWant, 1500.0f); }
                if (fr == 500) { hWant[0] = 2048; HSm::reach(hWant, 6000.0f); }      // the same size as the map before it, a longer reach
            }
        }
        float time = fr * 0.05f;
        { BYTE d[44]; BYTE t[12]; memset(d, 0xCD, 44); memset(t, 0xCD, 12);
          ((HRESULT(WINAPI*)(void*, DWORD, BYTE*))vt(fx)[4])(fx, hWorld, d);
          ((HRESULT(WINAPI*)(void*, DWORD, BYTE*))vt(fx)[5])(fx, tMain, t);
          DWORD hw = ((DWORD(WINAPI*)(void*, DWORD, const char*))vt(fx)[9])(fx, 0, "World");
          DWORD tm = ((DWORD(WINAPI*)(void*, const char*))vt(fx)[13])(fx, "Main");
          DWORD used = ((DWORD(WINAPI*)(void*, DWORD, DWORD))vt(fx)[62])(fx, hWorld, tMain);
          if (memcmp(d, refDesc, 44) || memcmp(t, refTech, 12) || hw != hWorld || tm != tMain || used != refUsed) queryMismatch++;
          DWORD hl = ((DWORD(WINAPI*)(void*, DWORD, const char*))vt(fx)[9])(fx, 0, kLong);
          DWORD hv = ((DWORD(WINAPI*)(void*, const char*, const char*))vt(fx)[9])(fx, "Mat", "Val");
          if (hl != hLong || hv != hMatVal) queryMismatch++; }
        if (rtToggle && fr > 0 && fr % 50 == 0) rtToggle((fr / 50) % 2 == 0);   // switch the render thread off/on every 50 frames
        float vp[16], lvp[16], world[16], bones[48];
        matLookProj(vp, 6 * cos(time * 0.3f), 5, 6 * sin(time * 0.3f), 1.2f);
        matLookProj(lvp, 4, 9, 3, 0.8f);
        CALL0(dev, 41);                                                   // BeginScene
        // shadow pass into the render-target texture
        CALL2(dev, 37, 0, srtSurf); CALL1(dev, 39, sz);
        { void* x = NULL; HRESULT h1 = CALL2(dev, 38, 0, &x); if (FAILED(h1) || x != srtSurf) mirrorMismatch++; if (x) CALL0(x, 2);
          void* y = NULL; HRESULT h2 = CALL1(dev, 40, &y); if (FAILED(h2) || y != sz) mirrorMismatch++; if (y) CALL0(y, 2);
          void* z = (void*)1; HRESULT h3 = CALL2(dev, 38, 1, &z); if (h3 != (HRESULT)0x88760866 || z != NULL) mirrorMismatch++;
          HRESULT h4 = CALL2(dev, 37, 0, texLvl); if (SUCCEEDED(h4) && rt == false) mirrorMismatch++;          // not a render target: must fail
          void* w = NULL; CALL2(dev, 38, 0, &w); if (w != srtSurf) mirrorMismatch++; if (w) CALL0(w, 2); }
        CALL6(dev, 43, 0, 0, 1 | 2, 0xFFFFFFFF, fbits(1.0f), 0);
        CALL1(fx, 58, tShadow); UINT np = 0; CALL2(fx, 63, &np, 6); if (np != passesShadow || !np) passMismatch++; CALL1(fx, 64, 0);
        CALL2(fx, 38, hLVP, lvp);
        for (int o = 0; o < 20; ++o) {
            matWorld(world, (o % 5) * 2.2f - 4.4f, 0.5f * sin(time + o), (o / 5) * 2.2f - 3.3f, time + o * 0.3f, 0.6f);
            for (int b = 0; b < 4; ++b) { memset(bones + b*12, 0, 12*sizeof(float)); bones[b*12] = bones[b*12+5] = bones[b*12+10] = 1; bones[b*12+3] = 0.1f * sin(time * (b + 1)); }
            ((HRESULT(WINAPI*)(void*, DWORD, const void*, UINT, UINT))vt(fx)[78])(fx, hWorld, world, 0, 64);
            ((HRESULT(WINAPI*)(void*, DWORD, const void*, UINT, UINT))vt(fx)[78])(fx, hBones, bones, 0, 48);
            CALL0(fx, 65);
            CALL1(dev, 87, decl); CALL4(dev, 100, 0, vb, 0, 32); CALL1(dev, 104, ib);
            CALL6(dev, 82, 4, 0, 0, 24, 0, 12);
        }
        CALL0(fx, 66); CALL0(fx, 67);
        // main pass
        CALL2(dev, 37, 0, bb); CALL1(dev, 39, bbz);
        { void* x = NULL; CALL2(dev, 38, 0, &x); if (x != bb) mirrorMismatch++; if (x) CALL0(x, 2);
          void* y = NULL; CALL1(dev, 40, &y); if (y != bbz) mirrorMismatch++; if (y) CALL0(y, 2);
          CALL1(dev, 39, 0); void* n0 = (void*)1; HRESULT hn = CALL1(dev, 40, &n0); if (hn != (HRESULT)0x88760866 || n0 != NULL) mirrorMismatch++;
          CALL1(dev, 39, bbz);
          if (CALL0(dev, 3) != 0) lockedMismatch++;                                                          // TestCooperativeLevel
          void* b2 = NULL; CALL4(dev, 18, 0, 0, 0, &b2); if (b2 != bb) lockedMismatch++; if (b2) CALL0(b2, 2);   // GetBackBuffer
          void* q9 = NULL; if (FAILED(CALL2(tex, 0, &kIIDTex9, &q9)) || q9 != tex) lockedMismatch++; if (q9) CALL0(q9, 2); }
        static bool panNow = false; static LARGE_INTEGER panT0, panT, panF;
        if (optPanSim) {                                               // the "scroll hook": most frames the camera is stepped, by the clock
            if (fr == 0) { QueryPerformanceFrequency(&panF); QueryPerformanceCounter(&panT0); }
            Sleep(optPanSlow ? 30 + (fr * 7) % 20 : 5 + (fr * 7) % 12);    // frames of uneven length
            if (fr % 60 == 59) Sleep(150);                             // and a stall now and then
            panNow = (fr % 200) >= 20 && (fr % 200) < 180;
            if (optPanStop) panNow = ((fr / 50) % 2 == 0) && (fr % 50) >= 5 && (fr % 50) < 30;
            QueryPerformanceCounter(&panT);
            if (panNow && rt) { typedef void (__cdecl* ScF)(); static ScF sc = (ScF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrPanWarpTestScroll"); if (sc) sc(); }
        }
        if (twMark) twMark(dev);                                       // from here the frame is drawn into the back buffer
        if (optPanSim) {                                               // the scene's camera, set before the scene is drawn (every draw of the frame is made "under" it)
            static float camX = 0;
            if (panNow) camX = 300.0f * (float)((double)(panT.QuadPart - panT0.QuadPart) / (double)panF.QuadPart);
            float V[16] = { 1,0,0,0, 0,0.7071f,0.7071f,0, 0,0.7071f,-0.7071f,0, -camX,0,700.0f,1 };
            float P[16] = { 1.35f,0,0,0, 0,1.8f,0,0, 0,0,1.0f,1, 0,0,-5.0f,0 };
            CALL2(dev, 44, 3, P); CALL2(dev, 44, 2, V);
        }
        CALL6(dev, 43, 0, 0, 1 | 2, 0xFF203040 + fr, fbits(1.0f), 0);
        CALL0(sblock, 4);                                              // application state block: capture
        np = 0; CALL1(fx, 58, tMain); CALL2(fx, 63, &np, 6); if (np != passesMain || !np) passMismatch++; CALL1(fx, 64, 0);
        if (blockC) CALL1(fx, 76, blockC);
        CALL0(fx, 73);
        { float t4[4] = {0.8f + 0.2f * (float)sin(time), 1.0f, 0.9f + 0.1f * (float)cos(time * 0.7f), 1.0f}; CALL2(fx, 34, hTint, t4);
          CALL2(fx, 30, hLong, fbits(0.9f + 0.1f * (float)cos(time))); CALL2(fx, 30, hMatVal, fbits(0.95f + 0.05f * (float)sin(time * 1.3f))); }
        blockC = ((DWORD(WINAPI*)(void*))vt(fx)[74])(fx);
        CALL1(fx, 75, (fr % 3 == 0) ? blockC : ((fr & 1) ? blockA : blockB));
        if (rt && argc > 4 && !strcmp(argv[4], "capture")) { typedef void (__cdecl* CapF)(int); static CapF cap = (CapF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrRtTestCaptureTick"); if (cap) cap(fr); }
        if (fr % 3 == 0) { LOCKED_RECT lr; if (SUCCEEDED(CALL3(atlasSurf, 13, &lr, 0, 0))) {
            for (int k = 0; k < 4; ++k) { int tx = ((fr / 3 + k * 3) & 7) * 32, ty = ((fr / 3 * 5 + k) & 7) * 32;
                for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) ((WORD*)((BYTE*)lr.pBits + (ty + y) * lr.Pitch))[tx + x] = (WORD)(0x8000u | ((fr * 7 + x * 3 + k) & 0x7FFF)); }
            { int tx = ((fr / 3) & 7) * 32, ty = 224; for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) { WORD* q = &((WORD*)((BYTE*)lr.pBits + (ty + y) * lr.Pitch))[tx + x]; *q = (WORD)(*q ^ 0x7C1F); } }
            CALL0(atlasSurf, 14); } }
        if (fr % 3 == 1) { LOCKED_RECT lr; if (SUCCEEDED(CALL3(atlas2Surf, 13, &lr, 0, 0))) {                       // the mip-mapped atlas: tiles rewritten, then the mip chain rebuilt (D3DXFilterTexture, box)
            for (int k = 0; k < 3; ++k) { int tx = ((fr / 3 + k * 5) & 7) * 32, ty = ((fr / 3 * 3 + k) & 7) * 32;
                for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) ((WORD*)((BYTE*)lr.pBits + (ty + y) * lr.Pitch))[tx + x] = (WORD)(0x8000u | ((fr * 11 + x * 5 + y + k) & 0x7FFF)); }
            { int tx = ((fr / 3 + 3) & 7) * 32, ty = 96; for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) { WORD* q = &((WORD*)((BYTE*)lr.pBits + (ty + y) * lr.Pitch))[tx + x]; *q = (WORD)(*q ^ 0x03E0); } }
            CALL0(atlas2Surf, 14); }
            D3DXFilterTexture(atlas2Tex, NULL, 0, 5); }
        CALL2(fx, 52, hDiff, (fr & 1) ? ((fr & 2) ? atlas2Tex : atlasTex) : tex); CALL2(fx, 52, hShadow, srt);
        CALL2(fx, 38, hVP, vp); CALL2(fx, 38, hLVP, lvp);
        CALL2(fx, 30, hBlend, fbits(0.5f + 0.25f * sin(time)));            // preshader input changes every frame
        for (int o = 0; o < 20; ++o) {
            matWorld(world, (o % 5) * 2.2f - 4.4f, 0.5f * sin(time + o), (o / 5) * 2.2f - 3.3f, time + o * 0.3f, 0.6f);
            for (int b = 0; b < 4; ++b) { memset(bones + b*12, 0, 12*sizeof(float)); bones[b*12] = bones[b*12+5] = bones[b*12+10] = 1; bones[b*12+3] = 0.1f * sin(time * (b + 1)); }
            float col[4] = {0.5f + 0.5f * sin(o + time), 0.7f, 0.5f + 0.5f * cos(o * 0.7f), 1};
            ((HRESULT(WINAPI*)(void*, DWORD, const void*, UINT, UINT))vt(fx)[78])(fx, hWorld, world, 0, 64);
            ((HRESULT(WINAPI*)(void*, DWORD, const void*, UINT, UINT))vt(fx)[78])(fx, hBones, bones, 0, 48);
            CALL2(fx, 34, hColor, col);
            CALL0(fx, 65);
            CALL1(dev, 87, decl); CALL4(dev, 100, 0, vb, 0, 32); CALL1(dev, 104, ib);
            CALL6(dev, 82, 4, 0, 0, 24, 0, 12);
        }
        CALL0(fx, 66); CALL0(fx, 67);
        CALL0(sblock, 5);                                              // application state block: apply
        // dynamic texture update + textured quad through DrawPrimitiveUP
        { LOCKED_RECT lr; CALL4(dyntex, 19, 0, &lr, 0, 0); for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) ((DWORD*)lr.pBits)[y*32+x] = 0xFF000000 | (((x + fr) * 8) & 0xFF) << 16 | ((y * 8) << 8); CALL1(dyntex, 20, 0); }
        CALL2(dev, 57, 7, 0); CALL1(dev, 107, 0); CALL1(dev, 92, 0); CALL1(dev, 89, 0x4 | 0x40 | 0x100); CALL2(dev, 65, 0, dyntex);
        CALL3(dev, 69, 0, 5, 1); CALL3(dev, 69, 0, 6, 1);
        struct TV { float x, y, z, w; DWORD c; float u, v; } q[4] = { {10,10,0,1,0xFFFFFFFF,0,0}, {138,10,0,1,0xFFFFFFFF,1,0}, {10,138,0,1,0xFFFFFFFF,0,1}, {138,138,0,1,0xFFFFFFFF,1,1} };
        CALL4(dev, 83, 5, 2, q, sizeof(TV));
        // particles through a dynamic vertex buffer (DISCARD)
        { void* pv = NULL; CALL4(dvb, 11, 0, 0, &pv, 0x2000);
          struct PV { float x, y, z, w; DWORD c; }* v = (PV*)pv;
          for (int k = 0; k < 100; ++k) { float cx = 320 + 200 * sin(time * 0.7f + k), cy = 240 + 150 * cos(time * 0.9f + k * 1.3f); DWORD c = 0x80000000 | (k * 2 << 16) | (255 - k);
            PV quad[6] = { {cx-6,cy-6,0,1,c}, {cx+6,cy-6,0,1,c}, {cx-6,cy+6,0,1,c}, {cx+6,cy-6,0,1,c}, {cx+6,cy+6,0,1,c}, {cx-6,cy+6,0,1,c} };
            memcpy(v + k * 6, quad, sizeof(quad)); }
          CALL0(dvb, 12); }
        CALL2(dev, 65, 0, 0); CALL2(dev, 57, 27, 1); CALL2(dev, 57, 19, 5); CALL2(dev, 57, 20, 6);
        CALL1(dev, 89, 0x4 | 0x40); CALL4(dev, 100, 0, dvb, 0, 20); CALL3(dev, 81, 4, 0, 200);
        { void* pv = NULL; CALL4(dvb, 11, 600 * 20 / 2, 300 * 20, &pv, 0x1000);         // NOOVERWRITE: second half
          struct PV { float x, y, z, w; DWORD c; }* v = (PV*)pv;
          for (int k = 0; k < 50; ++k) { float cx = 100 + 8.0f * k, cy = 60 + 20 * sin(time * 2 + k); DWORD c = 0xC000FF00 | (k * 4);
            PV quad[6] = { {cx-3,cy-3,0,1,c}, {cx+3,cy-3,0,1,c}, {cx-3,cy+3,0,1,c}, {cx+3,cy-3,0,1,c}, {cx+3,cy+3,0,1,c}, {cx-3,cy+3,0,1,c} };
            memcpy(v + k * 6, quad, sizeof(quad)); }
          CALL0(dvb, 12); }
        CALL3(dev, 81, 4, 300, 100);
        CALL2(dev, 57, 27, 0);
        // indexed user-pointer lines
        { struct LV { float x, y, z, w; DWORD c; } lv[5]; WORD li[8] = {0,1, 1,2, 2,3, 3,4};
          for (int k = 0; k < 5; ++k) { lv[k].x = 20.0f + k * 40; lv[k].y = 400 + 30 * sin(time + k); lv[k].z = 0; lv[k].w = 1; lv[k].c = 0xFFFFFF00; }
          CALL8(dev, 84, 2, 0, 5, 4, li, 101, lv, sizeof(LV)); }
        // D3DX creation and mip filtering in the middle of a frame
        if (fr == 10) { D3DXCreateTexture(dev, 64, 64, 0, 0, 21, 1, &extraTex); if (extraTex) { LOCKED_RECT lr; CALL4(extraTex, 19, 0, &lr, 0, 0); memset(lr.pBits, 0x7F, 64 * 64 * 4); CALL1(extraTex, 20, 0); D3DXFilterTexture(extraTex, NULL, 0, 0xFFFFFFFF); } }
        if (optPress && rt && fr % 7 == 4) { typedef void (__cdecl* PressF)(); static PressF press = (PressF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrTweenTestPressure"); if (press) press(); }   // "no ring space" in the middle of the frame
        if (extraTex) { CALL2(dev, 65, 0, extraTex); CALL1(dev, 89, 0x4 | 0x40 | 0x100);
            struct TV2 { float x, y, z, w; DWORD c; float u, v; } q2[4] = { {500,10,0,1,0xFFFFFFFF,0,0}, {630,10,0,1,0xFFFFFFFF,1,0}, {500,140,0,1,0xFFFFFFFF,0,1}, {630,140,0,1,0xFFFFFFFF,1,1} };
            CALL4(dev, 83, 5, 2, q2, sizeof(TV2)); CALL2(dev, 65, 0, 0); }
        // a managed texture created mid-frame, filled through level and surface locks, drawn with, then released:
        // nothing queued can name it yet, so the render thread may fill it without draining the queue
        { void* freshTex = NULL;
          if (SUCCEEDED(CALL8(dev, 23, 32, 32, 1, 0, 21, 1, &freshTex, 0)) && freshTex) {
            LOCKED_RECT lr;
            if (SUCCEEDED(CALL4(freshTex, 19, 0, &lr, 0, 0))) {
                for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x)
                    ((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = 0xFF000000u | (DWORD)((x * 8 + fr) & 0xFF) << 16 | (DWORD)((y * 8 + fr * 3) & 0xFF) << 8 | (DWORD)((x ^ y) & 0xFF);
                CALL1(freshTex, 20, 0);
            }
            void* lvl = NULL;                                                    // second fill through the level surface
            if (SUCCEEDED(CALL2(freshTex, 18, 0, &lvl)) && lvl) {
                if (SUCCEEDED(CALL3(lvl, 13, &lr, 0, 0))) {
                    for (int x = 0; x < 32; ++x) ((DWORD*)((BYTE*)lr.pBits + ((fr * 5) % 32) * lr.Pitch))[x] = 0xFFFF00FFu ^ (DWORD)(fr * 7 + x);
                    CALL0(lvl, 14);
                }
                CALL0(lvl, 2);
            }
            CALL2(dev, 65, 0, freshTex); CALL1(dev, 89, 0x4 | 0x40 | 0x100);
            struct TVF { float x, y, z, w; DWORD c; float u, v; } qf[4] = { {200,420,0,1,0xFFFFFFFF,0,0}, {264,420,0,1,0xFFFFFFFF,1,0}, {200,470,0,1,0xFFFFFFFF,0,1}, {264,470,0,1,0xFFFFFFFF,1,1} };
            CALL4(dev, 83, 5, 2, qf, sizeof(TVF)); CALL2(dev, 65, 0, 0);
            CALL0(freshTex, 2);
          }
        }
        // textures arriving in the middle of the frame, as the engine's asset streamer delivers them: a .dds with one mip
        // level (D3DX builds and compresses the rest), a terrain-style surface load + mip filter into DXT1 and into
        // R5G6B5, each drawn small enough to sample the built levels, then released
        if (optStream) {
            static BYTE dds[128 + 128 * 128 / 2]; static DWORD px[64 * 64];
            int side = (fr % 3 == 0) ? 128 : 64, blocks = (side / 4) * (side / 4);
            memset(dds, 0, 128); memcpy(dds, "DDS ", 4);
            DWORD* hd = (DWORD*)(dds + 4); hd[0] = 124; hd[1] = 0x81007; hd[2] = side; hd[3] = side; hd[4] = blocks * 8; hd[18] = 32; hd[19] = 4; memcpy(&hd[20], "DXT1", 4); hd[26] = 0x1000;
            for (int k = 0; k < blocks; ++k) { BYTE* b = dds + 128 + k * 8; WORD c0 = (WORD)(0x8410 + k * 37 + fr * 211), c1 = (WORD)(0x0400 + k * 11 + fr * 5); if (c0 <= c1) { WORD t = c0; c0 = c1; c1 = t; if (c0 == c1) c0++; }
                b[0] = (BYTE)c0; b[1] = (BYTE)(c0 >> 8); b[2] = (BYTE)c1; b[3] = (BYTE)(c1 >> 8); b[4] = (BYTE)(k * 29 + fr); b[5] = (BYTE)(k * 7 + fr * 3); b[6] = (BYTE)(k ^ fr); b[7] = (BYTE)(k * 13 + 5); }
            for (int k = 0; k < 64 * 64; ++k) px[k] = 0xFF000000u | (DWORD)((k * 3 + fr * 5) & 0xFF) << 16 | (DWORD)(((k >> 6) * 4 + fr) & 0xFF) << 8 | (DWORD)((k * 7) & 0xFF);
            void* st[3] = { NULL, NULL, NULL };
            D3DXCreateFromFile(dev, dds, 128 + blocks * 8, side, side, 0, 0, 0, 1, 0xFFFFFFFFu, 5, 0, NULL, NULL, &st[0]);
            for (int k = 1; k < 3; ++k) {
                if (FAILED(D3DXCreateTexture(dev, 64, 64, 0, 0, k == 1 ? 0x31545844u : 23u, 1, &st[k])) || !st[k]) continue;
                void* lvl = NULL; RECT rc = { 0, 0, 64, 64 };
                if (SUCCEEDED(CALL2(st[k], 18, 0, &lvl)) && lvl) { D3DXLoadSurfMem(lvl, NULL, NULL, px, 22, 64 * 4, NULL, &rc, 1, 0); CALL0(lvl, 2); }
                D3DXFilterTexture(st[k], NULL, 0, 5);
            }
            CALL1(dev, 89, 0x4 | 0x40 | 0x100); CALL3(dev, 69, 0, 7, 1);                                       // mip filter POINT: the built levels are what gets sampled
            for (int k = 0; k < 3; ++k) {
                if (!st[k]) continue;
                CALL2(dev, 65, 0, st[k]);
                float x0 = 300.0f + k * 70, sz = (k == 0) ? 30.0f : 14.0f;
                struct TVS { float x, y, z, w; DWORD c; float u, v; } qs[4] = { {x0,420,0,1,0xFFFFFFFF,0,0}, {x0+sz,420,0,1,0xFFFFFFFF,1,0}, {x0,420+sz,0,1,0xFFFFFFFF,0,1}, {x0+sz,420+sz,0,1,0xFFFFFFFF,1,1} };
                CALL4(dev, 83, 5, 2, qs, sizeof(TVS));
                struct TVS qb[4] = { {x0,440,0,1,0xFFFFFFFF,0,0}, {x0+60,440,0,1,0xFFFFFFFF,1,0}, {x0,476,0,1,0xFFFFFFFF,0,1}, {x0+60,476,0,1,0xFFFFFFFF,1,1} };
                CALL4(dev, 83, 5, 2, qb, sizeof(TVS));
            }
            // ...and geometry arriving the same way: a static vertex buffer and index buffer created now, filled through a
            // lock with no flags (which may read), bound, drawn; every third frame the vertex buffer is locked with no
            // flags AGAIN after it was drawn with, two corners moved, and drawn once more; then both are released
            { void* svb = NULL, *sib = NULL;
              struct SV { float x, y, z, w; DWORD c; float u, v; };
              if (SUCCEEDED(CALL6(dev, 26, 4 * sizeof(SV), 0, 0x4 | 0x40 | 0x100, 1, &svb, 0)) && svb && SUCCEEDED(CALL6(dev, 27, 6 * 2, 0, 101, 1, &sib, 0)) && sib) {
                  void* pv = NULL; float bx = 520.0f + (float)(fr % 7) * 3.0f, by = 400.0f;
                  // (v55) the first fill is sometimes a DISCARD or NOOVERWRITE lock of the whole buffer: what is in the buffer afterwards is the same,
                  // but the DLL has to know it without reading the device when the buffer is locked to read two draws later
                  DWORD firstFlags = (fr % 5 == 2) ? 0x2000u : (fr % 5 == 4) ? 0x1000u : 0u;
                  if (SUCCEEDED(CALL4(svb, 11, 0, 0, &pv, firstFlags)) && pv) {
                      SV q[4] = { {bx,by,0,1,0xFFFFFFFF,0,0}, {bx+70,by,0,1,0xFFFFFFFF,1,0}, {bx,by+50,0,1,0xFFFFFFFF,0,1}, {bx+70,by+50,0,1,0xFF80FFFF,1,1} };
                      memcpy(pv, q, sizeof(q)); CALL0(svb, 12); }
                  if (SUCCEEDED(CALL4(sib, 11, 0, 0, &pv, 0)) && pv) { WORD ix[6] = { 0, 1, 2, 1, 3, 2 }; memcpy(pv, ix, sizeof(ix)); CALL0(sib, 12); }
                  CALL1(dev, 89, 0x4 | 0x40 | 0x100); CALL2(dev, 65, 0, st[0]);
                  CALL4(dev, 100, 0, svb, 0, sizeof(SV)); CALL1(dev, 104, sib);
                  CALL6(dev, 82, 4, 0, 0, 4, 0, 2);
                  if (fr % 3 == 1 && SUCCEEDED(CALL4(svb, 11, 0, 0, &pv, 0)) && pv) {                  // read what is there, change part of it
                      SV* v = (SV*)pv; v[0].y += 60; v[1].y += 60; v[2].y = v[0].y + 30 + (float)(fr % 5); v[3].y = v[2].y; v[3].c = v[0].c ^ 0x00FF00FFu;
                      CALL0(svb, 12);
                      CALL6(dev, 82, 4, 0, 0, 4, 0, 2);
                  }
                  CALL2(dev, 65, 0, 0);
              }
              if (svb) CALL0(svb, 2);
              if (sib) CALL0(sib, 2); }
            CALL2(dev, 65, 0, 0); CALL3(dev, 69, 0, 7, 0);
            for (int k = 0; k < 3; ++k) if (st[k]) CALL0(st[k], 2);
        }
        // ground patch textures as the game makes them while the camera scrolls: a new managed A1R5G5B5 texture with
        // three levels, level 0 written through a full lock, the two levels below made from it (off: D3DXFilterTexture,
        // box; on: the accelerator writes them itself through locks of their own), drawn at once at all three levels,
        // kept alive and drawn for four frames, then released
        if (optTerrain) {
            static void* live[4] = { NULL, NULL, NULL, NULL }; static WORD l0[256 * 256];
            typedef int (__cdecl* TerrF)(void*, const WORD*, int, int); typedef void (__cdecl* AnyF)(int);
            static TerrF terr = rt ? (TerrF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrTerrainHarnessBuild") : NULL;
            static AnyF any = rt ? (AnyF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrRtTestAtlasAny") : NULL;
            int nNew = (fr % 4 == 3) ? 0 : (fr % 4 == 1) ? 2 : 1;                                             // none, one or two new ones in a frame
            for (int q = 0; q < nNew; ++q) {
                int slot = (fr * 2 + q) & 3, side = ((fr + q) % 5 == 0) ? 128 : 256;
                if (live[slot]) { CALL0(live[slot], 2); live[slot] = NULL; }
                DWORD seed = 0x9E3779B9u * (DWORD)(fr * 2 + q + 1);
                for (int k = 0; k < side * side; ++k) { seed = seed * 1664525u + 1013904223u; l0[k] = (WORD)((seed >> 13) ^ (k * 131)); }
                void* tt = NULL;
                if (FAILED(D3DXCreateTexture(dev, side, side, 3, 0, 25, 1, &tt)) || !tt) continue;
                int own = 0;
                if (terr) { if (any) any(0); own = terr(tt, l0, side, (fr % 50) == 2); if (any) any(1); }      // (the atlas mirror takes any caller in this harness; in the game it does not take this one)
                if (own <= 0) {
                    if (own < 0 || !terr) { void* lvl = NULL; LOCKED_RECT lr;
                        if (SUCCEEDED(CALL2(tt, 18, 0, &lvl)) && lvl) { if (SUCCEEDED(CALL3(lvl, 13, &lr, 0, 0))) { for (int y = 0; y < side; ++y) memcpy((BYTE*)lr.pBits + y * lr.Pitch, l0 + y * side, side * 2); CALL0(lvl, 14); } CALL0(lvl, 2); } }
                    D3DXFilterTexture(tt, NULL, 0, 5);
                }
                live[slot] = tt;
            }
            CALL1(dev, 89, 0x4 | 0x40 | 0x100); CALL3(dev, 69, 0, 7, 1);                                       // mip filter POINT: each level is sampled as it is
            for (int k = 0; k < 4; ++k) {
                if (!live[k]) continue;
                CALL2(dev, 65, 0, live[k]);
                float x0 = 20.0f + k * 150.0f, y0 = 150.0f;
                struct TVT { float x, y, z, w; DWORD c; float u, v; };
                TVT a0[4] = { {x0,y0,0,1,0xFFFFFFFF,0,0}, {x0+64,y0,0,1,0xFFFFFFFF,0.25f,0}, {x0,y0+64,0,1,0xFFFFFFFF,0,0.25f}, {x0+64,y0+64,0,1,0xFFFFFFFF,0.25f,0.25f} };   // one texel to a pixel (256): level 0
                TVT a1[4] = { {x0+70,y0,0,1,0xFFFFFFFF,0,0}, {x0+134,y0,0,1,0xFFFFFFFF,0.5f,0}, {x0+70,y0+64,0,1,0xFFFFFFFF,0,0.5f}, {x0+134,y0+64,0,1,0xFFFFFFFF,0.5f,0.5f} }; // two to a pixel: level 1
                TVT a2[4] = { {x0,y0+70,0,1,0xFFFFFFFF,0,0}, {x0+64,y0+70,0,1,0xFFFFFFFF,1,0}, {x0,y0+134,0,1,0xFFFFFFFF,0,1}, {x0+64,y0+134,0,1,0xFFFFFFFF,1,1} };             // four to a pixel: level 2
                CALL4(dev, 83, 5, 2, a0, sizeof(TVT)); CALL4(dev, 83, 5, 2, a1, sizeof(TVT)); CALL4(dev, 83, 5, 2, a2, sizeof(TVT));
            }
            CALL2(dev, 65, 0, 0); CALL3(dev, 69, 0, 7, 0);
            if (fr == frames - 1) for (int k = 0; k < 4; ++k) if (live[k]) { CALL0(live[k], 2); live[k] = NULL; }
        }
        // shroud-style updates: lock the rect (1,1)-(63,63), copy every row, unlock - 32-bit and 16-bit textures
        for (int k = 0; k < 62 * 62 * 4; ++k) shrRows[k] = (BYTE)((k * 7 + fr * 13) ^ (k >> 5));
        shroudRect(shrSurf32, 1, 1, 63, 63, shrRows, 4);
        shroudRect(shrSurf16, 1, 1, 63, 63, shrRows + 1000, 2);
        { CALL2(dev, 65, 0, shrTex32); CALL1(dev, 89, 0x4 | 0x40 | 0x100);
          struct TV7 { float x, y, z, w; DWORD c; float u, v; } q7[4] = { {390,300,0,1,0xFFFFFFFF,0,0}, {454,300,0,1,0xFFFFFFFF,1,0}, {390,364,0,1,0xFFFFFFFF,0,1}, {454,364,0,1,0xFFFFFFFF,1,1} };
          CALL4(dev, 83, 5, 2, q7, sizeof(TV7));
          CALL2(dev, 65, 0, shrTex16);
          struct TV7 q8[4] = { {470,300,0,1,0xFFFFFFFF,0,0}, {534,300,0,1,0xFFFFFFFF,1,0}, {470,364,0,1,0xFFFFFFFF,0,1}, {534,364,0,1,0xFFFFFFFF,1,1} };
          CALL4(dev, 83, 5, 2, q8, sizeof(TV7)); CALL2(dev, 65, 0, 0); }
        // minimap-style pixel writes: 1x1 LockRect / write / UnlockRect per pixel on a MANAGED texture (colour and alpha variants)
        for (int k = 0; k < 40; ++k) {
            DWORD px = (DWORD)((k * 7 + fr) % 64), py = (DWORD)((k * 13 + fr / 3) % 64);
            pixelOp(pixSurf, px, py, 0xFF000000u | ((DWORD)(k * 6 + fr) & 0xFF) << 16 | ((DWORD)(fr * 5) & 0xFF) << 8 | (DWORD)(k * 3), 4);
            if (k & 1) pixelOp(pixSurf, px, py, (DWORD)(128 + k), 0x100);
        }
        // plain vertex-buffer locks: read the colours and positions, change them from what is there, sometimes only a sub-range
        { TVB* v = NULL;
          DWORD off = (fr % 3 == 1) ? 2 * sizeof(TVB) : 0, sz = (fr % 3 == 1) ? 4 * sizeof(TVB) : 0;
          if (SUCCEEDED(CALL4(rmVB, 11, off, sz, &v, 0)) && v) { int cnt = sz ? 4 : 8; for (int i = 0; i < cnt; ++i) { v[i].c = v[i].c * 5 + 0x01020304u + (DWORD)fr; v[i].y = 20.0f + (float)(((DWORD)(v[i].y * 3.0f) + (DWORD)fr) % 200); } CALL0(rmVB, 12); }
          if (fr % 13 == 7) { if (SUCCEEDED(CALL4(rmVB, 11, 0, 0, &v, 0x2000)) && v) { for (int i = 0; i < 8; ++i) { TVB t = { 700.0f + (i % 2) * 64.0f, 20.0f + (i / 2) * 32.0f, 0, 1, 0xFF405060u ^ (DWORD)(fr * i), (float)(i % 2), (float)((i / 2) % 2) }; v[i] = t; } CALL0(rmVB, 12); } }
          CALL1(dev, 89, 0x4 | 0x40 | 0x100); CALL2(dev, 65, 0, 0); CALL4(dev, 100, 0, rmVB, 0, sizeof(TVB));
          CALL3(dev, 81, 5, 0, 6); CALL4(dev, 100, 0, 0, 0, 0); }
        // radar-overlay style updates: rect read-modify-write, whole-surface read-modify-write, clears, pixel writes, and a foreign texture lock
        {
            DWORD l = (DWORD)((fr * 5) % 100), t = (DWORD)((fr * 3) % 90);
            radarOp(radSurf, 1, l, t, l + 20, t + 30, (DWORD)fr);
            if (fr % 11 == 3) radarOp(radSurf, 1, 120, 0, 140, 10, 7);                                   // out of bounds: fails either way
            radarOp(radSurf, 1, (DWORD)((fr * 17) % 64), 64, (DWORD)((fr * 17) % 64) + 64, 128, (DWORD)(fr * 11));
            for (int k = 0; k < 30; ++k) {
                DWORD px = (DWORD)((k * 29 + fr * 3) % 128), py = (DWORD)((k * 31 + fr) % 128);
                pixelOp(radSurf, px, py, 0xFF000000u | (DWORD)((k * 9 + fr) & 0xFF) << 8 | (DWORD)k, 4);
                if (k % 3 == 0) pixelOp(radSurf, px, py, (DWORD)(40 + k), 0x100);
            }
            if (fr % 7 == 2) radarOp(radSurf, 3, 0, 0, 0, 0, (DWORD)(fr + 1));
            if (fr % 17 == 5) radarOp(radSurf, 2, (DWORD)(fr & 1), 0, 0, 0, 0);
            if (fr % 97 == 50) { LOCKED_RECT lr; if (SUCCEEDED(CALL4(radTex, 19, 0, &lr, 0, 0))) { for (int y = 40; y < 50; ++y) for (int x = 0; x < 128; ++x) ((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] ^= 0x00FF00FFu; CALL1(radTex, 20, 0); } }
        }
        { CALL2(dev, 65, 0, radTex); CALL1(dev, 89, 0x4 | 0x40 | 0x100);
          struct TV9 { float x, y, z, w; DWORD c; float u, v; } q9[4] = { {550,300,0,1,0xFFFFFFFF,0,0}, {678,300,0,1,0xFFFFFFFF,1,0}, {550,428,0,1,0xFFFFFFFF,0,1}, {678,428,0,1,0xFFFFFFFF,1,1} };
          CALL4(dev, 83, 5, 2, q9, sizeof(TV9)); CALL2(dev, 65, 0, 0); }
        { CALL2(dev, 65, 0, pixTex); CALL1(dev, 89, 0x4 | 0x40 | 0x100);
          struct TV6 { float x, y, z, w; DWORD c; float u, v; } q6[4] = { {310,300,0,1,0xFFFFFFFF,0,0}, {374,300,0,1,0xFFFFFFFF,1,0}, {310,364,0,1,0xFFFFFFFF,0,1}, {374,364,0,1,0xFFFFFFFF,1,1} };
          CALL4(dev, 83, 5, 2, q6, sizeof(TV6)); CALL2(dev, 65, 0, 0); }
        // system-memory surface: write, UpdateSurface into a DEFAULT texture, READONLY re-lock while the update is still queued
        { LOCKED_RECT lr;
          if (SUCCEEDED(CALL3(sysSurf2, 13, &lr, 0, 0))) {
              for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) ((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = 0xFF000000 | (((x * 4 + fr * 3) & 0xFF) << 16) | (((y * 4) & 0xFF) << 8) | (fr & 0xFF);
              CALL0(sysSurf2, 14); } else sysMismatch++;
          CALL4(dev, 30, sysSurf2, 0, defSurf, 0);
          if (SUCCEEDED(CALL3(sysSurf2, 13, &lr, 0, 0x10))) { DWORD px = ((DWORD*)lr.pBits)[0]; if (px != (0xFF000000u | (((fr * 3) & 0xFF) << 16) | (fr & 0xFF))) sysMismatch++; CALL0(sysSurf2, 14); } else sysMismatch++;
          CALL2(dev, 65, 0, defTex); CALL1(dev, 89, 0x4 | 0x40 | 0x100);
          struct TV4 { float x, y, z, w; DWORD c; float u, v; } q4[4] = { {150,300,0,1,0xFFFFFFFF,0,0}, {214,300,0,1,0xFFFFFFFF,1,0}, {150,364,0,1,0xFFFFFFFF,0,1}, {214,364,0,1,0xFFFFFFFF,1,1} };
          CALL4(dev, 83, 5, 2, q4, sizeof(TV4)); CALL2(dev, 65, 0, 0); }
        // system-memory texture: LockRect on the texture, UpdateTexture; every 5th frame again through its level surface while the first update is queued
        { LOCKED_RECT lr;
          if (SUCCEEDED(CALL4(sysTex, 19, 0, &lr, 0, 0))) {
              for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) ((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = 0xFF000000 | (((y * 3 + fr) & 0xFF) << 16) | ((x * 4) & 0xFF);
              CALL1(sysTex, 20, 0); } else sysMismatch++;
          CALL2(dev, 31, sysTex, defTex2);
          if (fr % 5 == 2) { void* ls = NULL; CALL2(sysTex, 18, 0, &ls);
              if (ls) { if (SUCCEEDED(CALL3(ls, 13, &lr, 0, 0))) { for (int y = 20; y < 30; ++y) for (int x = 0; x < 64; ++x) ((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = 0xFFFFFF00 | (DWORD)fr; CALL0(ls, 14); } else sysMismatch++;
                        CALL0(ls, 2); }
              CALL2(dev, 31, sysTex, defTex2); }
          CALL2(dev, 65, 0, defTex2); CALL1(dev, 89, 0x4 | 0x40 | 0x100);
          struct TV5 { float x, y, z, w; DWORD c; float u, v; } q5[4] = { {230,300,0,1,0xFFFFFFFF,0,0}, {294,300,0,1,0xFFFFFFFF,1,0}, {230,364,0,1,0xFFFFFFFF,0,1}, {294,364,0,1,0xFFFFFFFF,1,1} };
          CALL4(dev, 83, 5, 2, q5, sizeof(TV5)); CALL2(dev, 65, 0, 0); }
        // object churn: child surface get/release every frame; a short-lived texture created, filled, bound,
        // drawn and released while its queued SetTexture still references it
        { void* lvl = NULL; CALL2(dyntex, 18, 0, &lvl); if (lvl) CALL0(lvl, 2); }
        if (fr % 7 == 3) {
            void* t = NULL; CALL8(dev, 23, 16, 16, 1, 0, 21, 1, &t, 0);
            if (t) { LOCKED_RECT lr; CALL4(t, 19, 0, &lr, 0, 0); for (int k = 0; k < 256; ++k) ((DWORD*)lr.pBits)[k] = 0xFF00FF00 | (DWORD)(fr * 37 + k);
                CALL1(t, 20, 0); CALL2(dev, 65, 0, t); CALL1(dev, 89, 0x4 | 0x40 | 0x100);
                struct TV3 { float x, y, z, w; DWORD c; float u, v; } q3[4] = { {300,300,0,1,0xFFFFFFFF,0,0}, {364,300,0,1,0xFFFFFFFF,1,0}, {300,364,0,1,0xFFFFFFFF,0,1}, {364,364,0,1,0xFFFFFFFF,1,1} };
                CALL4(dev, 83, 5, 2, q3, sizeof(TV3)); CALL2(dev, 65, 0, 0); CALL0(t, 2); }
        }
        // a surface fetched from the device and released in the same frame
        { void* rtS = NULL; CALL2(dev, 38, 0, &rtS); if (rtS) CALL0(rtS, 2); }
        if (optPcf) {                                                    // the rectangle with the game's shadow lookup, last, so that nothing is drawn over it
            UINT npP = 0; CALL1(fxP, 58, tPcf); CALL2(fxP, 63, &npP, 6); CALL1(fxP, 64, 0);
            CALL1(dev, 92, 0); CALL1(dev, 89, 0x10144);                  // no vertex shader; XYZRHW | DIFFUSE | TEX1, three wide
            struct PVX { float x, y, z, w; DWORD c; float u, v, d; } pq[4];
            for (int k = 0; k < 4; ++k) { bool r = (k & 1) != 0, b = (k & 2) != 0;
                pq[k].x = (float)PCF_X0 + (r ? (float)PCF_W : 0.0f) - 0.5f; pq[k].y = (float)PCF_Y0 + (b ? (float)PCF_W : 0.0f) - 0.5f; pq[k].z = 0; pq[k].w = 1; pq[k].c = 0xFFFFFFFFu;
                pq[k].u = kPcfU0 + (r ? 0.5f : 0.0f); pq[k].v = kPcfU0 + (b ? 0.5f : 0.0f); pq[k].d = 0.5f; }
            CALL4(dev, 83, 5, 2, pq, sizeof(PVX));
            CALL0(fxP, 66); CALL0(fxP, 67);
            CALL1(dev, 107, 0);
        }
        CALL0(dev, 42);                                                   // EndScene
        // read back and hash
        CALL5(dev, 34, bb, 0, cap, 0, 1);
        CALL2(dev, 32, cap, sys);
        LOCKED_RECT lr; unsigned long long h = 1469598103934665603ull;
        if (SUCCEEDED(CALL3(sys, 13, &lr, 0, 0x10))) { for (int y = 0; y < 480; ++y) h = fnv((BYTE*)lr.pBits + y * lr.Pitch, 640 * 4, h);
            if (optPcf) {                                                // the rectangle against the game's four-point sum and against the nine-point lookup
                int bad4 = 0, bad9 = 0;
                for (int y = 0; y < PCF_W; ++y) for (int x = 0; x < PCF_W; ++x) {
                    int got = (int)((((const DWORD*)((const BYTE*)lr.pBits + (PCF_Y0 + y) * lr.Pitch))[PCF_X0 + x] >> 16) & 0xFF);
                    int w4 = (int)floor(255.0 * (0.3 + 0.7 * pcfRef(x, y, false)) + 0.5), w9 = (int)floor(255.0 * (0.3 + 0.7 * pcfRef(x, y, true)) + 0.5);
                    int d4 = abs(got - w4), d9 = abs(got - w9);
                    if (d4 > 1) ++bad4; if (d9 > 1) ++bad9; if (d4 > pcfWorst4) pcfWorst4 = d4; if (d9 > pcfWorst9) pcfWorst9 = d9;
                }
                if (!bad4) ++pcfFour; else if (!bad9) ++pcfNine; else ++pcfNeither;
            }
            CALL0(sys, 14); }
        { DWORD tMain = ((DWORD(WINAPI*)(void*, const char*))vt(fx)[13])(fx, "Main");                  // annotation reads, as the engine does per skinned mesh
          DWORD ha = ((DWORD(WINAPI*)(void*, DWORD, const char*))vt(fx)[19])(fx, tMain, "MaxSkinningBones");
          DWORD hb = ((DWORD(WINAPI*)(void*, DWORD, DWORD))vt(fx)[18])(fx, tMain, 0);
          INT va = -1, vb = -1;
          for (int k = 0; k < 3; ++k) { ((HRESULT(WINAPI*)(void*, DWORD, INT*))vt(fx)[27])(fx, ha, &va); ((HRESULT(WINAPI*)(void*, DWORD, INT*))vt(fx)[27])(fx, hb, &vb); }
          h ^= (unsigned long long)(DWORD)(va * 131 + vb * 7 + (ha ? 1 : 0) + (hb ? 2 : 0)) << 20; }
        if (fr == 100 || fr == 400 || fr == frames - 1) for (int lv = 1; lv <= 3; lv += 2) { LOCKED_RECT ml;
            if (SUCCEEDED(CALL4(atlas2Tex, 19, lv, &ml, 0, 0x10))) { int dim = 256 >> lv; for (int y = 0; y < dim; ++y) h = fnv((BYTE*)ml.pBits + y * ml.Pitch, dim * 2, h); CALL1(atlas2Tex, 20, lv); } }
        fprintf(out, "%d %016llx\n", fr, h); fflush(out);
        if (optPanSim) {                                               // as the engine does right before the interface: its 2D camera (a VIEW that never moves), then the 3D pass ends
            float V2d[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,-1.0f,1 };
            float P2d[16] = { 2.0f,0,0,0, 0,2.0f,0,0, 0,0,-1.99f,-1, 0,0,-1.98f,0 };
            CALL2(dev, 44, 3, P2d); CALL2(dev, 44, 2, V2d);            // (nothing in this harness draws through the device's transforms)
            if (rt) { typedef void (__cdecl* VeF)(); static VeF ve = (VeF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrTweenTestViewsEnd"); if (ve) ve(); }
        }
        ((HRESULT(WINAPI*)(void*, void*, void*, HWND, void*))vt(dev)[17])(dev, NULL, NULL, NULL, NULL);
    }
    QueryPerformanceCounter(&t1);
    fclose(out);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / qf.QuadPart;
    printf("%s: %d frames in %.1f ms\n", rt ? "RT on" : "RT off", frames, ms);
    printf("cached query mismatches: %d, Begin pass count mismatches: %d\n", queryMismatch, passMismatch);
    if (rt && optShadowScale) printf("shadow scale: wanted size by frame - %s (expected 0:2048 1:8192 101:4096; the change of map at frame 200, 1024 made 4096, is in the log)\n", hSmLog);
    if (rt && optShadowMatch) { bool ok = !strcmp(hSmLog, "0:2048 1:8192 101:2048 201:4096 301:8192 401:2048 501:8192");   /* long reach: 8192; default numbers: left alone; reach 2200: 4096; 4096 over 6000: 8192; default; the same size with a long reach again: 8192 */ printf("shadow match %s: wanted size by frame - %s\n", ok ? "as expected" : "NOT AS EXPECTED", hSmLog); }
    if (rt && optPanSim) { typedef void (__cdecl* TsF)(char*, int); TsF ts = (TsF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrPanWarpStats"); static char tb[2600]; tb[0] = 0; if (ts) ts(tb, sizeof(tb)); printf("%s\n", tb); }
    if (rt && optPanId) { typedef void (__cdecl* TsF)(char*, int); TsF ts = (TsF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrPanWarpStats"); static char tb[768]; tb[0] = 0; if (ts) ts(tb, sizeof(tb)); printf("%s\n", tb); }
    if (rt && optTerrain) { typedef void (__cdecl* TsF)(char*, int); TsF ts = (TsF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrTerrainHarnessStats"); static char tb[512]; tb[0] = 0; if (ts) ts(tb, sizeof(tb)); printf("%s\n", tb); }
    printf("sysmem lock mismatches: %d, render-target mirror mismatches: %d, lock-only call mismatches: %d\n", sysMismatch, mirrorMismatch, lockedMismatch);
    if (optPcf) { static char pb[512]; pb[0] = 0;
        if (rt) { typedef void (__cdecl* PcF)(int, char*, int); PcF pc = (PcF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrShadowPcfTest"); if (pc) pc(-1, pb, sizeof(pb)); }
        printf("pcf: of %d frames the rectangle was the game's four-point sum in %d, the nine-point lookup in %d, neither in %d%s%s\n", frames, pcfFour, pcfNine, pcfNeither, pb[0] ? " | " : "", pb); }
    if (rt && optFxWrap) { typedef DWORD (__cdecl* FeF)(int, DWORD); FeF fe = (FeF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrRtTestFxEpoch"); DWORD e = fe ? fe(0, 0) : 0;
        printf("effect value counter: now %lu - %s\n", e, (e && e < 0xFFFFFFC0u) ? "it wrapped during the run" : "IT DID NOT WRAP"); }
    if (rt) { char st[512]; rtStats(st, sizeof(st)); printf("rt stats: %s\n", st);
        typedef void (__cdecl* Stats4F)(char*, int); Stats4F s4 = (Stats4F)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrRtTestStatsV4");
        if (s4) { s4(st, sizeof(st)); printf("rt v4 stats: %s\n", st); }
        if (optTex) { typedef void (__cdecl* TxStatsF)(char*, int); TxStatsF xs = (TxStatsF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrTexTestStats");
                      if (xs) { static char xb[2000]; xs(xb, sizeof(xb)); printf("%s\n", xb); } }
        if (optTween) { typedef void (__cdecl* TwStatsF)(char*, int); TwStatsF ts = (TwStatsF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrTweenTestStats");
            static char tst[2048]; if (ts) { ts(tst, sizeof(tst)); printf("in-between frames: %s\n", tst); }
            TwStatsF ds = (TwStatsF)GetProcAddress(GetModuleHandleA("bfme2_accel.new.dll"), "AotrDevPassStats");
            if (ds) { ds(tst, sizeof(tst)); printf("in-between frames, %s\n", tst); } } }
    return 0;
}
