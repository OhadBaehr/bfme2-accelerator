// fxprobe: what does the game's D3DX (d3dx9_27.dll) do to the device for the game's own effects?
//   fxprobe.exe <effect.fxo> [list | calls | equiv N | time]
// Loads a compiled effect with the game's d3dx9_27.dll on the game's Direct3D (DXVK's d3d9.dll next to the exe),
// swaps the device's method table for one that records every call D3DX makes (method, arguments, a hash of constant
// data) and applies it to a model of the device's state, and then:
//   list   - techniques, passes, parameters;
//   calls  - the device calls of Begin / BeginPass / CommitChanges / EndPass / End for every technique, with the
//            flags the game passes to Begin (6), after typical parameter writes;
//   equiv  - the question behind leaving a pass open: for N random rounds of parameter writes, is the device's state
//            after "CommitChanges inside the open pass" the same as after "EndPass, End, Begin, BeginPass"? Two
//            copies of the effect are driven side by side, each with its own model of the device;
//   time   - D3DX's own cost of each method with the device's methods answered by stubs.
// 32-bit, raw vtables, no SDK headers (as rt_harness.cpp).
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <vector>
#include <string>

static void** vt(void* o) { return *(void***)o; }
#include "aotr_shtab.inc"
typedef struct { UINT BackBufferWidth, BackBufferHeight; DWORD BackBufferFormat; UINT BackBufferCount;
    DWORD MultiSampleType, MultiSampleQuality, SwapEffect; HWND hDeviceWindow; BOOL Windowed;
    BOOL EnableAutoDepthStencil; DWORD AutoDepthStencilFormat, Flags; UINT FullScreen_RefreshRateInHz, PresentationInterval; } PP;
struct FxDesc { const char* Creator; UINT Parameters, Techniques, Functions; };
struct ParamDesc { const char* Name; const char* Semantic; DWORD Class, Type; UINT Rows, Columns, Elements, Annotations, StructMembers; DWORD Flags; UINT Bytes; };
struct TechDesc { const char* Name; UINT Passes, Annotations; };
struct PassDesc { const char* Name; UINT Annotations; const DWORD* pVS; const DWORD* pPS; };

// ---- the recording device table
enum { NSLOT = 119 };
static void* g_orig[NSLOT];
static void* g_tab[NSLOT];
static BYTE* g_thunks = NULL;
static int g_stub = 0;                    // 1: the state-setting methods are answered here (timing D3DX alone)
struct Call { int slot; DWORD a[5]; DWORD hash; };
static std::vector<Call> g_calls;
static int g_rec = 0;
struct Model {                              // what the device would hold
    std::map<DWORD, DWORD> rs, vsI, psI, vsB, psB;
    std::map<DWORD, DWORD> ss, tss;         // (stage << 16 | type) -> value
    std::map<DWORD, DWORD> tex;
    std::map<DWORD, std::vector<DWORD> > vsF, psF;   // register -> 4 dwords
    DWORD vs, ps, fvf, decl; bool hasVs, hasPs;
    Model() : vs(0), ps(0), fvf(0), decl(0), hasVs(false), hasPs(false) {}
};
static Model* g_model = NULL;
static int g_recording = 0;
static DWORD fnv(const void* p, size_t n) { DWORD h = 0x811C9DC5u; const BYTE* b = (const BYTE*)p; for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x01000193u; return h; }
static const char* slotName(int s) {
    switch (s) {
    case 57: return "SetRenderState"; case 58: return "GetRenderState"; case 59: return "CreateStateBlock"; case 60: return "BeginStateBlock"; case 61: return "EndStateBlock";
    case 64: return "GetTexture"; case 65: return "SetTexture"; case 66: return "GetTextureStageState"; case 67: return "SetTextureStageState";
    case 68: return "GetSamplerState"; case 69: return "SetSamplerState"; case 86: return "GetVertexDeclaration"; case 87: return "SetVertexDeclaration";
    case 88: return "GetFVF"; case 89: return "SetFVF"; case 91: return "CreateVertexShader"; case 92: return "SetVertexShader"; case 93: return "GetVertexShader";
    case 94: return "SetVertexShaderConstantF"; case 95: return "GetVertexShaderConstantF"; case 96: return "SetVertexShaderConstantI"; case 98: return "SetVertexShaderConstantB";
    case 106: return "CreatePixelShader"; case 107: return "SetPixelShader"; case 108: return "GetPixelShader"; case 109: return "SetPixelShaderConstantF";
    case 111: return "SetPixelShaderConstantI"; case 113: return "SetPixelShaderConstantB"; case 44: return "SetTransform"; case 49: return "SetMaterial"; case 51: return "SetLight";
    case 53: return "LightEnable"; case 0: return "QueryInterface"; case 1: return "AddRef"; case 2: return "Release"; case 7: return "GetDeviceCaps";
    case 81: return "DrawPrimitive"; case 82: return "DrawIndexedPrimitive"; case 100: return "SetStreamSource"; case 104: return "SetIndices";
    case 23: return "CreateTexture"; case 37: return "SetRenderTarget"; case 39: return "SetDepthStencilSurface"; case 47: return "SetViewport";
    default: { static char b[8][16]; static int k = 0; k = (k + 1) & 7; sprintf_s(b[k], "#%d", s); return b[k]; }
    }
}
// called by every thunk: esp-based args as the method got them ([0] = this)
static int __stdcall onCall(int slot, DWORD* args) {
    if (g_rec) {
        Call c; c.slot = slot; for (int i = 0; i < 5; ++i) c.a[i] = args[1 + i]; c.hash = 0;
        if (slot == 94 || slot == 109) c.hash = fnv((void*)(ULONG_PTR)args[2], args[3] * 16);
        if (slot == 96 || slot == 111) c.hash = fnv((void*)(ULONG_PTR)args[2], args[3] * 16);
        if (slot == 98 || slot == 113) c.hash = fnv((void*)(ULONG_PTR)args[2], args[3] * 4);
        g_calls.push_back(c);
    }
    // What D3DX sets between BeginStateBlock and EndStateBlock goes into the block it is recording, not to the device.
    if (slot == 60) g_recording = 1; else if (slot == 61) g_recording = 0;
    if (g_model && !g_recording) { Model& m = *g_model;
        switch (slot) {
        case 57: m.rs[args[1]] = args[2]; break;
        case 67: m.tss[(args[1] << 16) | args[2]] = args[3]; break;
        case 69: m.ss[(args[1] << 16) | args[2]] = args[3]; break;
        case 65: m.tex[args[1]] = args[2]; break;
        case 92: m.vs = args[1]; m.hasVs = true; break;
        case 107: m.ps = args[1]; m.hasPs = true; break;
        case 89: m.fvf = args[1]; break;
        case 87: m.decl = args[1]; break;
        case 94: case 109: { std::map<DWORD, std::vector<DWORD> >& t = slot == 94 ? m.vsF : m.psF; const DWORD* d = (const DWORD*)(ULONG_PTR)args[2];
            for (DWORD i = 0; i < args[3]; ++i) { std::vector<DWORD>& v = t[args[1] + i]; v.assign(d + i * 4, d + i * 4 + 4); } break; }
        case 96: case 111: { std::map<DWORD, DWORD>& t = slot == 96 ? m.vsI : m.psI; const DWORD* d = (const DWORD*)(ULONG_PTR)args[2];
            for (DWORD i = 0; i < args[3]; ++i) t[args[1] + i] = fnv(d + i * 4, 16); break; }
        case 98: case 113: { std::map<DWORD, DWORD>& t = slot == 98 ? m.vsB : m.psB; const DWORD* d = (const DWORD*)(ULONG_PTR)args[2];
            for (DWORD i = 0; i < args[3]; ++i) t[args[1] + i] = d[i]; break; }
        }
    }
    if (g_stub) switch (slot) { case 57: case 65: case 67: case 69: case 92: case 94: case 96: case 98: case 107: case 109: case 111: case 113: return 1; }
    return 0;
}
static const int kArgs[NSLOT] = {      // stack arguments after "this" (for the stubbed methods only)
    0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,
    2,0,0,0,0,0,0,0, 2,0,3,0,3, 0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0, 0,1,0,3,0,3,0,3,0, 0,0,0,0,0,0, 0,1,0,3,0,3,0,3,0, 0,0,0,0 };
static void makeThunks(void* dev) {
    memcpy(g_orig, vt(dev), sizeof(g_orig));
    g_thunks = (BYTE*)VirtualAlloc(NULL, NSLOT * 48, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    for (int s = 0; s < NSLOT; ++s) {
        BYTE* p = g_thunks + s * 48; g_tab[s] = p;
        // pushad; lea eax,[esp+36]; push eax; push slot; call onCall; test eax,eax; popad(keeps flags? no) ...
        *p++ = 0x60;                                              // pushad
        *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x24;       // lea eax, [esp+0x24]   (-> this, args)
        *p++ = 0x50;                                              // push eax
        *p++ = 0x68; *(DWORD*)p = (DWORD)s; p += 4;               // push slot
        *p++ = 0xE8; *(DWORD*)p = (DWORD)(ULONG_PTR)&onCall - (DWORD)(ULONG_PTR)(p + 4); p += 4;
        *p++ = 0x85; *p++ = 0xC0;                                 // test eax, eax
        *p++ = 0x61;                                              // popad (flags stay)
        *p++ = 0x75; *p++ = 0x06;                                 // jnz stub
        *p++ = 0xFF; *p++ = 0x25; *(DWORD*)p = (DWORD)(ULONG_PTR)&g_orig[s]; p += 4;   // jmp [orig]
        *p++ = 0x31; *p++ = 0xC0;                                 // stub: xor eax, eax
        *p++ = 0xC2; *(WORD*)p = (WORD)(4 + kArgs[s] * 4); p += 2;                    // ret n
    }
    *(void***)dev = (void**)g_tab;
}
static void printCalls(const char* what) {
    std::map<int, int> n; for (size_t i = 0; i < g_calls.size(); ++i) n[g_calls[i].slot]++;
    printf("  %-34s %3d device calls:", what, (int)g_calls.size());
    for (std::map<int, int>::iterator it = n.begin(); it != n.end(); ++it) printf(" %s x%d", slotName(it->first), it->second);
    printf("\n");
}
static void printDetail(int max) {
    for (size_t i = 0; i < g_calls.size() && (int)i < max; ++i) { const Call& c = g_calls[i];
        printf("        %-26s %08lX %08lX %08lX %08lX%s\n", slotName(c.slot), c.a[0], c.a[1], c.a[2], c.a[3], c.hash ? "  (data)" : ""); }
}
// a shader object's identity: a hash of its code (IDirect3DVertexShader9 / PixelShader9 ::GetFunction, slot 4)
static std::map<DWORD, DWORD> g_shaderId;
static DWORD shaderId(DWORD sh) {
    if (!sh) return 0;
    std::map<DWORD, DWORD>::iterator it = g_shaderId.find(sh); if (it != g_shaderId.end()) return it->second;
    static BYTE buf[1 << 16]; UINT n = sizeof(buf); void* o = (void*)(ULONG_PTR)sh;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, void*, UINT*))vt(o)[4])(o, buf, &n);
    DWORD id = FAILED(hr) ? sh : (fnv(buf, n) | 1);
    g_shaderId[sh] = id; return id;
}
struct ShaderUse { bool c[256]; bool rel; int relBase; bool samp[16]; bool ok; };
static std::map<DWORD, ShaderUse> g_use;
static const ShaderUse& shaderUse(DWORD sh) {
    std::map<DWORD, ShaderUse>::iterator it = g_use.find(sh); if (it != g_use.end()) return it->second;
    ShaderUse u; memset(&u, 0, sizeof(u)); u.relBase = 256;
    static DWORD buf[1 << 14]; UINT n = sizeof(buf); void* o = (void*)(ULONG_PTR)sh;
    if (sh && SUCCEEDED(((HRESULT(WINAPI*)(void*, void*, UINT*))vt(o)[4])(o, buf, &n))) {
        u.ok = true; UINT nt = n / 4; bool defd[256]; memset(defd, 0, sizeof(defd));
        for (UINT i = 1; i < nt; ) {
            DWORD t = buf[i]; DWORD op = t & 0xFFFF;
            if (op == 0xFFFF) break;
            if (op == 0xFFFE) { i += 1 + ((t >> 16) & 0x7FFF); continue; }           // comment (tables, preshader)
            if (op == 0x51) { DWORD d = buf[i + 1]; if ((((d >> 28) & 7) | ((d >> 8) & 0x18)) == 2 && (d & 0x7FF) < 256) defd[d & 0x7FF] = true; i += 6; continue; }   // def c#
            if (op == 0x30) { i += 6; continue; }                                        // defi
            if (op == 0x2F) { i += 3; continue; }                                        // defb
            if (op == 0x1F) { DWORD d = buf[i + 2]; if ((((d >> 28) & 7) | ((d >> 8) & 0x18)) == 10 && (d & 0x7FF) < 16) u.samp[d & 0x7FF] = true; i += 3; continue; }   // dcl (sampler)
            UINT j = i + 1; int k = 0;
            while (j < nt && (buf[j] & 0x80000000u)) {
                DWORD q = buf[j]; DWORD type = ((q >> 28) & 7) | ((q >> 8) & 0x18); DWORD num = q & 0x7FF;
                bool src = k > 0 || op == 0x41 /* texkill reads its one operand */;
                if (src && type == 2 && num < 256) { if (q & 0x2000) { u.rel = true; if ((int)num < u.relBase) u.relBase = (int)num; if ((buf[0] >> 16) == 0xFFFE && (buf[0] & 0xFFFF) >= 0x0200) ++j; } else u.c[num] = true; }
                if (src && type == 10 && num < 16) u.samp[num] = true;
                ++j; ++k;
            }
            i = j;
        }
        for (int r = 0; r < 256; ++r) if (defd[r]) u.c[r] = false;                      // defined inside the shader: the device's value is not read
        if (u.rel) for (int r = u.relBase; r < 256; ++r) if (!defd[r]) u.c[r] = true;    // indexed (the joints): everything from the base up
    }
    g_use[sh] = u; return g_use[sh];
}
template <class M> static bool firstDiff(const M& a, const M& b, char* out, const char* what) {
    typename M::const_iterator i = a.begin(), j = b.begin();
    for (; i != a.end() && j != b.end(); ++i, ++j) if (i->first != j->first || i->second != j->second) { sprintf_s(out, 200, "%s: entry %08lX / %08lX", what, (unsigned long)i->first, (unsigned long)j->first); return true; }
    if (i != a.end()) { sprintf_s(out, 200, "%s: entry %08lX only in the first", what, (unsigned long)i->first); return true; }
    if (j != b.end()) { sprintf_s(out, 200, "%s: entry %08lX only in the second", what, (unsigned long)j->first); return true; }
    return false;
}
static bool sameModel(const Model& a, const Model& b, char* why) {
    #define CMP(f, n) if (a.f != b.f) { if (!firstDiff(a.f, b.f, why, n)) sprintf_s(why, 200, "%s differ", n); return false; }
    CMP(rs, "render states") CMP(ss, "sampler states") CMP(tss, "stage states") CMP(tex, "textures") CMP(vsF, "vertex shader float constants") CMP(psF, "pixel shader float constants")
    CMP(vsI, "vertex shader int constants") CMP(psI, "pixel shader int constants") CMP(vsB, "vertex shader bool constants") CMP(psB, "pixel shader bool constants")
    #undef CMP
    if (shaderId(a.vs) != shaderId(b.vs) || a.hasVs != b.hasVs) { strcpy_s(why, 200, "vertex shader differs"); return false; }
    if (shaderId(a.ps) != shaderId(b.ps) || a.hasPs != b.hasPs) { strcpy_s(why, 200, "pixel shader differs"); return false; }
    return true;
}

// The device as the next draw reads it: the pass's render states, the shaders, the float constants the shaders read,
// the textures and sampler states of the stages the pixel shader samples.
static bool sameForDraw(const Model& a, const Model& b, char* why) {
    if (a.rs != b.rs) { if (!firstDiff(a.rs, b.rs, why, "render states")) strcpy_s(why, 200, "render states"); return false; }
    if (shaderId(a.vs) != shaderId(b.vs)) { strcpy_s(why, 200, "vertex shader"); return false; }
    if (shaderId(a.ps) != shaderId(b.ps)) { strcpy_s(why, 200, "pixel shader"); return false; }
    const ShaderUse& uv = shaderUse(a.vs); const ShaderUse& up = shaderUse(a.ps);
    for (int r = 0; r < 256; ++r) {
        if (uv.c[r]) { std::map<DWORD, std::vector<DWORD> >::const_iterator i = a.vsF.find(r), j = b.vsF.find(r);
            if ((i == a.vsF.end()) != (j == b.vsF.end()) || (i != a.vsF.end() && i->second != j->second)) { sprintf_s(why, 200, "vertex shader constant c%d (read by the shader)", r); return false; } }
        if (up.c[r]) { std::map<DWORD, std::vector<DWORD> >::const_iterator i = a.psF.find(r), j = b.psF.find(r);
            if ((i == a.psF.end()) != (j == b.psF.end()) || (i != a.psF.end() && i->second != j->second)) { sprintf_s(why, 200, "pixel shader constant c%d (read by the shader)", r); return false; } }
    }
    for (DWORD st = 0; st < 16; ++st) if (up.samp[st]) {
        std::map<DWORD, DWORD>::const_iterator i = a.tex.find(st), j = b.tex.find(st);
        if ((i == a.tex.end()) != (j == b.tex.end()) || (i != a.tex.end() && i->second != j->second)) { sprintf_s(why, 200, "texture of stage %lu (sampled)", st); return false; }
        for (DWORD ty = 1; ty <= 13; ++ty) { DWORD k = (st << 16) | ty; i = a.ss.find(k); j = b.ss.find(k);
            if ((i == a.ss.end()) != (j == b.ss.end()) || (i != a.ss.end() && i->second != j->second)) {
                sprintf_s(why, 200, "sampler state %lu of stage %lu (sampled): %08lX against %08lX", ty, st, i == a.ss.end() ? 0xFFFFFFFFul : i->second, j == b.ss.end() ? 0xFFFFFFFFul : j->second); return false; } }
    }
    return true;
}

// ---- effect helpers (ID3DXEffect slots)
#define FX(o, slot, sig) ((sig)vt(o)[slot])
static DWORD fxParam(void* fx, DWORD parent, UINT i) { return FX(fx, 8, DWORD(WINAPI*)(void*, DWORD, UINT))(fx, parent, i); }
static void fxParamDesc(void* fx, DWORD h, ParamDesc* d) { FX(fx, 4, HRESULT(WINAPI*)(void*, DWORD, ParamDesc*))(fx, h, d); }
static DWORD fxTech(void* fx, UINT i) { return FX(fx, 12, DWORD(WINAPI*)(void*, UINT))(fx, i); }
static HRESULT fxSetTech(void* fx, DWORD t) { return FX(fx, 58, HRESULT(WINAPI*)(void*, DWORD))(fx, t); }
static HRESULT fxBegin(void* fx, UINT* n, DWORD flags) { return FX(fx, 63, HRESULT(WINAPI*)(void*, UINT*, DWORD))(fx, n, flags); }
static HRESULT fxBeginPass(void* fx, UINT p) { return FX(fx, 64, HRESULT(WINAPI*)(void*, UINT))(fx, p); }
static HRESULT fxCommit(void* fx) { return FX(fx, 65, HRESULT(WINAPI*)(void*))(fx); }
static HRESULT fxEndPass(void* fx) { return FX(fx, 66, HRESULT(WINAPI*)(void*))(fx); }
static HRESULT fxEnd(void* fx) { return FX(fx, 67, HRESULT(WINAPI*)(void*))(fx); }
static HRESULT fxSetValue(void* fx, DWORD h, const void* p, UINT n) { return FX(fx, 20, HRESULT(WINAPI*)(void*, DWORD, const void*, UINT))(fx, h, p, n); }
static HRESULT fxSetTexture(void* fx, DWORD h, void* t) { return FX(fx, 52, HRESULT(WINAPI*)(void*, DWORD, void*))(fx, h, t); }
static HRESULT fxSetRaw(void* fx, DWORD h, const void* p, UINT off, UINT n) { return FX(fx, 78, HRESULT(WINAPI*)(void*, DWORD, const void*, UINT, UINT))(fx, h, p, off, n); }
static DWORD fxBeginBlock(void* fx) { return (DWORD)FX(fx, 73, HRESULT(WINAPI*)(void*))(fx); }
static DWORD fxEndBlock(void* fx) { return FX(fx, 74, DWORD(WINAPI*)(void*))(fx); }
static HRESULT fxApplyBlock(void* fx, DWORD b) { return FX(fx, 75, HRESULT(WINAPI*)(void*, DWORD))(fx, b); }
struct Param { DWORD h; ParamDesc d; std::string name; std::vector<BYTE> def; std::vector<int> okVals; };
static std::vector<std::string> g_written;                         // the parameters the current round wrote
static void listParams(void* fx, std::vector<Param>& out) {
    FxDesc fd; FX(fx, 3, HRESULT(WINAPI*)(void*, FxDesc*))(fx, &fd);
    for (UINT i = 0; i < fd.Parameters; ++i) { Param p; p.h = fxParam(fx, 0, i); fxParamDesc(fx, p.h, &p.d); p.name = p.d.Name ? p.d.Name : "?";
        if (p.d.Class != 4 && p.d.Bytes && p.d.Bytes <= 4096) { p.def.resize(p.d.Bytes); FX(fx, 21, HRESULT(WINAPI*)(void*, DWORD, void*, UINT))(fx, p.h, &p.def[0], p.d.Bytes); }
        out.push_back(p); }
}
static void restoreDefaults(void* fx, const std::vector<Param>& ps) {
    for (size_t i = 0; i < ps.size(); ++i) if (!ps[i].def.empty()) FX(fx, 20, HRESULT(WINAPI*)(void*, DWORD, const void*, UINT))(fx, ps[i].h, &ps[i].def[0], (UINT)ps[i].def.size());
}
static unsigned g_seed = 12345;
static unsigned rnd() { g_seed = g_seed * 1664525u + 1013904223u; return g_seed >> 8; }
static float frand() { return (float)(rnd() % 20001) / 10000.0f - 1.0f; }
// one random write to a parameter, the same bytes to both effects
static void randomWrite(void* fxA, void* fxB, const std::vector<Param>& pa, const std::vector<Param>& pb, void** texs, int ntex) {
    size_t k = rnd() % pa.size(); const Param& a = pa[k]; const Param& b = pb[k];
    if (a.d.Class == 4) {                                              // object
        if (a.d.Type >= 5 && a.d.Type <= 9) { void* t = texs[rnd() % ntex]; fxSetTexture(fxA, a.h, t); fxSetTexture(fxB, b.h, t); g_written.push_back(a.name); }
        return;
    }
    if ((a.d.Type == 1 || a.d.Type == 2) && a.okVals.empty()) return;  // an int / bool no value of which opens the pass on its own: left alone
    g_written.push_back(a.name);
    if (a.d.Class == 5 || a.d.Bytes == 0 || a.d.Bytes > 4096) {         // struct: its bytes as a block
        if (a.d.Bytes == 0 || a.d.Bytes > 4096) return;
    }
    static BYTE buf[4096];
    UINT n = a.d.Bytes;
    for (UINT i = 0; i + 4 <= n; i += 4) {
        if (a.d.Type == 1 || a.d.Type == 2) *(INT*)(buf + i) = a.okVals[rnd() % a.okVals.size()];
        else *(float*)(buf + i) = (rnd() % 7 == 0) ? 0.0f : frand() * ((rnd() & 3) ? 1.0f : 40.0f);
    }
    fxSetValue(fxA, a.h, buf, n); fxSetValue(fxB, b.h, buf, n);
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("fxprobe <effect.fxo> [list|calls|equiv N|time]\n"); return 2; }
    const char* mode = argc > 2 ? argv[2] : "calls";
    FILE* f = NULL; fopen_s(&f, argv[1], "rb"); if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET); char* data = (char*)malloc(size); fread(data, 1, size, f); fclose(f);
    SetEnvironmentVariableA("DXVK_CONFIG", "dxvk.hud="); SetEnvironmentVariableA("DXVK_LOG_LEVEL", "none"); SetEnvironmentVariableA("DXVK_STATE_CACHE", "0");
    HMODULE hx = LoadLibraryA("d3dx9_27.dll"); HMODULE hd3d = LoadLibraryA("d3d9.dll");
    if (!hx || !hd3d) { printf("d3dx9_27.dll / d3d9.dll missing next to the exe\n"); return 3; }
    typedef void* (WINAPI* Create9F)(UINT);
    typedef HRESULT (WINAPI* CreateEffectF)(void*, const void*, UINT, void*, void*, DWORD, void*, void**, void**);
    Create9F Create9 = (Create9F)GetProcAddress(hd3d, "Direct3DCreate9");
    CreateEffectF CreateEffect = (CreateEffectF)GetProcAddress(hx, "D3DXCreateEffect");
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "fxprobe"; RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "fxprobe", "fx", WS_POPUP, -3000, -3000, 320, 240, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    void* d3d = Create9(32);
    PP pp = {0}; pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.BackBufferFormat = 21; pp.BackBufferCount = 1; pp.SwapEffect = 1; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = 75; pp.PresentationInterval = 0x80000000;
    void* dev = NULL;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, DWORD, HWND, DWORD, PP*, void**))vt(d3d)[16])(d3d, 0, 1, hwnd, 0x40u | 0x2u | 0x4u, &pp, &dev);
    if (FAILED(hr) || !dev) { printf("CreateDevice hr=0x%08lX\n", (unsigned long)hr); return 4; }
    void* texs[6];
    for (int i = 0; i < 6; ++i) { texs[i] = NULL; ((HRESULT(WINAPI*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, void*))vt(dev)[23])(dev, 16, 16, 1, 0, 21, 1, &texs[i], NULL); }
    makeThunks(dev);                                                  // from here every device call is seen
    void* fxA = NULL, *fxB = NULL, *err = NULL;
    hr = CreateEffect(dev, data, (UINT)size, NULL, NULL, 0, NULL, &fxA, &err);
    if (FAILED(hr) || !fxA) { printf("D3DXCreateEffect hr=0x%08lX\n", (unsigned long)hr); return 5; }
    hr = CreateEffect(dev, data, (UINT)size, NULL, NULL, 0, NULL, &fxB, &err);
    std::vector<Param> pa, pb; listParams(fxA, pa); listParams(fxB, pb);
    FxDesc fd; FX(fxA, 3, HRESULT(WINAPI*)(void*, FxDesc*))(fxA, &fd);
    printf("%s: %u parameters, %u techniques\n", argv[1], fd.Parameters, fd.Techniques);

    if (!strcmp(mode, "dis")) {                                        // the whole effect as D3DX reads it back: parameters, sampler states, every pass's states and shader code
        typedef HRESULT (WINAPI* DisF)(void*, BOOL, void**);
        DisF dis = (DisF)GetProcAddress(hx, "D3DXDisassembleEffect");
        void* buf = NULL;
        if (!dis || FAILED(dis(fxA, FALSE, &buf)) || !buf) { printf("D3DXDisassembleEffect failed\n"); return 6; }
        const char* txt = ((const char*(WINAPI*)(void*))vt(buf)[3])(buf); DWORD n = ((DWORD(WINAPI*)(void*))vt(buf)[4])(buf);
        fwrite(txt, 1, n ? n - 1 : 0, stdout);
        return 0;
    }
    if (!strcmp(mode, "list")) {
        for (UINT t = 0; t < fd.Techniques; ++t) { DWORD th = fxTech(fxA, t); TechDesc td; FX(fxA, 5, HRESULT(WINAPI*)(void*, DWORD, TechDesc*))(fxA, th, &td); printf("  technique %-22s %u passes\n", td.Name, td.Passes); }
        static const char* kClass[] = { "scalar", "vector", "matrix(rows)", "matrix(cols)", "object", "struct" };
        static const char* kType[] = { "void", "bool", "int", "float", "string", "texture", "texture1D", "texture2D", "texture3D", "textureCUBE", "sampler", "sampler1D", "sampler2D", "sampler3D", "samplerCUBE", "pixelshader", "vertexshader" };
        for (size_t i = 0; i < pa.size(); ++i) { const ParamDesc& d = pa[i].d;
            printf("  %-52s %-12s %-10s %ux%u elements %u bytes %u%s%s\n", pa[i].name.c_str(), d.Class < 6 ? kClass[d.Class] : "?", d.Type < 17 ? kType[d.Type] : "?", d.Rows, d.Columns, d.Elements, d.Bytes,
                   d.Semantic ? " : " : "", d.Semantic ? d.Semantic : ""); }
        return 0;
    }
    if (!strcmp(mode, "samp")) {                                       // which stage each texture goes to and how that stage is filtered, pass by pass (the calls of a pass begun a second time)
        size_t k = 0;
        for (size_t i = 0; i < pa.size(); ++i) if (pa[i].d.Class == 4 && pa[i].d.Type >= 5 && pa[i].d.Type <= 9) {
            bool shadow = pa[i].name.find("Shadow") != std::string::npos || pa[i].name.find("shadow") != std::string::npos;
            void* tx = shadow ? texs[5] : texs[k++ % 5];
            fxSetTexture(fxA, pa[i].h, tx); printf("texture parameter %-28s = %08lX%s\n", pa[i].name.c_str(), (unsigned long)(ULONG_PTR)tx, shadow ? "   <- the shadow map" : "");
        }
        for (size_t i = 0; i < pa.size(); ++i) if (pa[i].name == "NumShadows" && pa[i].d.Bytes == 4) { int one = 1; fxSetValue(fxA, pa[i].h, &one, 4); printf("NumShadows = 1\n"); }   // the variants that read the shadow map
        for (UINT t = 0; t < fd.Techniques; ++t) {
            DWORD th = fxTech(fxA, t); TechDesc td; FX(fxA, 5, HRESULT(WINAPI*)(void*, DWORD, TechDesc*))(fxA, th, &td);
            fxSetTech(fxA, th);
            for (int round = 0; round < 2; ++round) {
                UINT np = 0; fxBegin(fxA, &np, 6);
                for (UINT p = 0; p < np; ++p) {
                    g_calls.clear(); g_rec = 1; fxBeginPass(fxA, p); g_rec = 0;
                    if (round == 1) { char w[96]; sprintf_s(w, "technique %s pass %u", td.Name, p); printCalls(w); printDetail(600); }
                    fxEndPass(fxA);
                }
                fxEnd(fxA);
            }
        }
        return 0;
    }
    if (!strcmp(mode, "calls")) {
        for (UINT t = 0; t < fd.Techniques; ++t) {
            DWORD th = fxTech(fxA, t); TechDesc td; FX(fxA, 5, HRESULT(WINAPI*)(void*, DWORD, TechDesc*))(fxA, th, &td);
            printf("technique %s (%u passes)\n", td.Name, td.Passes);
            fxSetTech(fxA, th);
            for (int round = 0; round < 2; ++round) {
                UINT np = 0;
                g_calls.clear(); g_rec = 1; hr = fxBegin(fxA, &np, 6); g_rec = 0; { char w[64]; sprintf_s(w, "Begin(flags 6) hr=%08lX", (unsigned long)hr); printCalls(w); if (round == 0) printDetail(12); }
                for (UINT p = 0; p < np; ++p) {
                    g_calls.clear(); g_rec = 1; fxBeginPass(fxA, p); g_rec = 0; { char w[64]; sprintf_s(w, "BeginPass(%u)%s", p, round ? " again" : ""); printCalls(w); if (round == 0) printDetail(200); }
                    g_calls.clear(); g_rec = 1; fxCommit(fxA); g_rec = 0; printCalls("CommitChanges, nothing written");
                    // the game's per-draw writes: a world matrix (any float4x3 / 4x4 called World) and a commit
                    for (size_t i = 0; i < pa.size(); ++i) if (pa[i].name == "World") { float m[16]; for (int k = 0; k < 16; ++k) m[k] = frand(); fxSetValue(fxA, pa[i].h, m, pa[i].d.Bytes); }
                    g_calls.clear(); g_rec = 1; fxCommit(fxA); g_rec = 0; printCalls("CommitChanges after World"); if (round == 0) printDetail(12);
                    for (size_t i = 0; i < pa.size(); ++i) if (pa[i].d.Class == 4 && pa[i].d.Type >= 5 && pa[i].d.Type <= 9) { fxSetTexture(fxA, pa[i].h, texs[(i + round) % 6]); break; }
                    g_calls.clear(); g_rec = 1; fxCommit(fxA); g_rec = 0; printCalls("CommitChanges after a texture"); if (round == 0) printDetail(12);
                    g_calls.clear(); g_rec = 1; fxEndPass(fxA); g_rec = 0; printCalls("EndPass"); if (round == 0) printDetail(12);
                }
                g_calls.clear(); g_rec = 1; fxEnd(fxA); g_rec = 0; printCalls("End"); if (round == 0) printDetail(12);
            }
        }
        return 0;
    }
    if (!strcmp(mode, "equiv")) {
        int rounds = argc > 3 ? atoi(argv[3]) : 2000;
        int bad = 0, total = 0; char why[200]; why[0] = 0;
        for (size_t k = 0; k < pa.size(); ++k) if ((pa[k].d.Type == 1 || pa[k].d.Type == 2) && pa[k].d.Class != 4 && pa[k].d.Bytes == 4) {
            for (int v = 0; v < (pa[k].d.Type == 1 ? 2 : 8); ++v) {
                bool ok = true;
                for (UINT t = 0; t < fd.Techniques && ok; ++t) { UINT np = 0; DWORD th = fxTech(fxA, t); fxSetTech(fxA, th);
                    fxSetValue(fxA, pa[k].h, &v, 4);
                    HRESULT h1 = fxBegin(fxA, &np, 6), h2 = fxBeginPass(fxA, 0), h3 = fxCommit(fxA); if (!h2) fxEndPass(fxA); if (!h1) fxEnd(fxA);
                    if (h1 || h2 || h3) ok = false; }
                if (ok) { pa[k].okVals.push_back(v); pb[k].okVals.push_back(v); }
            }
            restoreDefaults(fxA, pa);
            printf("   %s: values that open every pass: ", pa[k].name.c_str()); for (size_t q = 0; q < pa[k].okVals.size(); ++q) printf("%d ", pa[k].okVals[q]); printf("\n");
        }
        // variant 0: plain writes | 1: every third round the writes go through a parameter block applied inside the open
        // pass | 2: SetTechnique (the same one) inside the open pass every round, as the engine calls it before every Begin
        for (int variant = 0; variant < 3; ++variant) {
          printf(variant == 0 ? "-- plain parameter writes\n" : variant == 1 ? "-- with parameter blocks applied while the pass is open\n" : "-- with SetTechnique (the same technique) while the pass is open\n");
          for (UINT t = 0; t < fd.Techniques; ++t) {
            DWORD thA = fxTech(fxA, t), thB = fxTech(fxB, t); TechDesc td; FX(fxA, 5, HRESULT(WINAPI*)(void*, DWORD, TechDesc*))(fxA, thA, &td);
            for (UINT p = 0; p < td.Passes; ++p) {
                Model mA, mB; UINT np = 0; int badHere = 0, refBlock = 0, refTech = 0, refOther = 0, anyDiff = 0, badWithSwitch = 0, thrown = 0; long callsA = 0, callsB = 0;
                restoreDefaults(fxA, pa); restoreDefaults(fxB, pb);
                fxSetTech(fxA, thA); fxSetTech(fxB, thB);
                g_model = &mA; fxBegin(fxA, &np, 6); fxBeginPass(fxA, p);
                g_model = &mB; fxBegin(fxB, &np, 6); fxBeginPass(fxB, p);
                g_model = NULL;
                if (!sameModel(mA, mB, why)) { printf("technique %s pass %u: the two copies differ after the first BeginPass (%s)\n", td.Name, p, why); ++bad; mB = mA; }
                for (int r = 0; r < rounds; ++r) {
                    DWORD psBefore = shaderId(mA.ps), vsBefore = shaderId(mA.vs);
                    int ref0 = refOther; g_written.clear();
                    g_model = &mA; if (fxEndPass(fxA) != 0) ++refOther; if (fxEnd(fxA) != 0) ++refOther;
                    g_model = NULL;
                    int nw = 1 + (int)(rnd() % 12);
                    bool withBlock = variant == 1 && (r % 3) == 1; DWORD blkA = 0, blkB = 0;
                    if (withBlock) { fxBeginBlock(fxA); fxBeginBlock(fxB); }
                    for (int w = 0; w < nw; ++w) randomWrite(fxA, fxB, pa, pb, texs, 6);
                    if (withBlock) { blkA = fxEndBlock(fxA); blkB = fxEndBlock(fxB);
                        for (int w = 0; w < 3; ++w) randomWrite(fxA, fxB, pa, pb, texs, 6);
                        g_model = &mA; if (fxApplyBlock(fxA, blkA) != 0) ++refOther;
                        g_model = &mB; if (fxApplyBlock(fxB, blkB) != 0) ++refBlock;
                        g_model = NULL; }
                    if (variant == 2) { g_model = &mB; if (fxSetTech(fxB, thB) != 0) ++refTech; g_model = NULL; fxSetTech(fxA, thA); }
                    g_model = &mA; g_rec = 1; g_calls.clear(); if (fxBegin(fxA, &np, 6) != 0) ++refOther; if (fxBeginPass(fxA, p) != 0) ++refOther; callsA += (long)g_calls.size();
                    g_model = &mB; g_calls.clear(); if (fxCommit(fxB) != 0) ++refOther; callsB += (long)g_calls.size(); g_rec = 0;
                    g_model = NULL;
                    if (refOther != ref0) {                                   // a value combination the effect refuses: the round does not count, both start afresh
                        refOther = ref0; ++thrown;
                        fxEndPass(fxA); fxEnd(fxA); fxEndPass(fxB); fxEnd(fxB); restoreDefaults(fxA, pa); restoreDefaults(fxB, pb);
                        mA = Model(); mB = Model();
                        g_model = &mA; fxBegin(fxA, &np, 6); fxBeginPass(fxA, p); g_model = &mB; fxBegin(fxB, &np, 6); fxBeginPass(fxB, p); g_model = NULL; mB = mA;
                        continue;
                    }
                    ++total;
                    if (!sameModel(mA, mB, why)) { ++anyDiff; char w2[200];
                        if (!sameForDraw(mA, mB, w2)) { if (badHere < 6) { printf("   technique %s pass %u round %d: %s | written:", td.Name, p, r, w2); for (size_t q = 0; q < g_written.size(); ++q) printf(" %s", g_written[q].c_str()); printf("%s\n", (shaderId(mA.ps) != psBefore || shaderId(mA.vs) != vsBefore) ? " | the shaders in use changed" : ""); }
                            ++badHere; if (shaderId(mA.ps) != psBefore || shaderId(mA.vs) != vsBefore) ++badWithSwitch; }
                        mB = mA; g_model = &mB; fxEndPass(fxB); fxEnd(fxB); fxBegin(fxB, &np, 6); fxBeginPass(fxB, p); g_model = NULL; mB = mA; }
                    for (int d = 0; d < 3; ++d) { randomWrite(fxA, fxB, pa, pb, texs, 6); g_model = &mA; fxCommit(fxA); g_model = &mB; fxCommit(fxB); g_model = NULL;
                        if (!sameModel(mA, mB, why)) { char w2[200]; if (!sameForDraw(mA, mB, w2)) { if (badHere < 4) printf("   technique %s pass %u round %d draw %d: %s\n", td.Name, p, r, d, w2); ++badHere; }
                            mB = mA; g_model = &mB; fxEndPass(fxB); fxEnd(fxB); fxBegin(fxB, &np, 6); fxBeginPass(fxB, p); g_model = NULL; mB = mA; } }
                }
                g_model = &mA; fxEndPass(fxA); fxEnd(fxA); g_model = &mB; fxEndPass(fxB); fxEnd(fxB); g_model = NULL;
                printf("   technique %-18s pass %u: %d rounds: what the next draw reads differed %d times (%d of them when the round changed the shaders in use); somewhere on the device %d times; refused: %d block applies, %d SetTechnique, %d others; %d rounds thrown away | device calls a round: close + open %.1f, commit %.1f\n",
                       td.Name, p, rounds, badHere, badWithSwitch, anyDiff, refBlock, refTech, refOther, thrown, (double)callsA / rounds, (double)callsB / rounds);
                bad += badHere + refOther + refBlock + refTech;
            }
          }
        }
        printf("%s: %d rounds in all, %d differences or refusals\n", bad ? "DIFFERENT" : "THE SAME", total, bad);
        return bad ? 1 : 0;
    }
    if (!strcmp(mode, "each")) {
        // For every technique and every parameter: both copies start from the same open pass; the parameter gets a new
        // value in both; copy A closes and opens the pass again, copy B commits. What differs on the device afterwards,
        // by kind. Each parameter is tried with several values; ints 0..3, bools both ways.
        for (UINT t = 0; t < fd.Techniques; ++t) {
            DWORD thA = fxTech(fxA, t), thB = fxTech(fxB, t); TechDesc td; FX(fxA, 5, HRESULT(WINAPI*)(void*, DWORD, TechDesc*))(fxA, thA, &td);
            printf("technique %s\n", td.Name);
            fxSetTech(fxA, thA); fxSetTech(fxB, thB);
            UINT np = 0; Model mA, mB;
            g_model = &mA; fxBegin(fxA, &np, 6); fxBeginPass(fxA, 0); g_model = &mB; fxBegin(fxB, &np, 6); fxBeginPass(fxB, 0); g_model = NULL;
            for (size_t k = 0; k < pa.size(); ++k) {
                const Param& a = pa[k]; const Param& b = pb[k];
                int nRS = 0, nSS = 0, nTex = 0, nVS = 0, nPS = 0, nCF = 0, nCP = 0, nOther = 0, tries = 0, refused = 0, changedA = 0, nDraw = 0; char drawWhy[200]; drawWhy[0] = 0;
                for (int v = 0; v < 6; ++v) {
                    static BYTE buf[4096]; UINT n = a.d.Bytes; if (n > 4096) n = 4096;
                    if (a.d.Class == 4) { if (!(a.d.Type >= 5 && a.d.Type <= 9)) break; void* tx = texs[(v + 1) % 6]; fxSetTexture(fxA, a.h, tx); fxSetTexture(fxB, b.h, tx); }
                    else { if (!n) break;
                        for (UINT i = 0; i + 4 <= n; i += 4) {
                            if (a.d.Type == 1) *(BOOL*)(buf + i) = (v & 1);
                            else if (a.d.Type == 2) *(INT*)(buf + i) = v % 4;
                            else *(float*)(buf + i) = (v & 1) ? 0.0f : (float)(v + 1) * 0.37f + (float)i * 0.01f;
                        }
                        fxSetValue(fxA, a.h, buf, n); fxSetValue(fxB, b.h, buf, n); }
                    Model before = mA;
                    g_model = &mA; HRESULT h1 = fxEndPass(fxA), h2 = fxEnd(fxA), h3 = fxBegin(fxA, &np, 6), h4 = fxBeginPass(fxA, 0);
                    g_model = &mB; HRESULT h5 = fxCommit(fxB); g_model = NULL;
                    ++tries; if (h1 || h2 || h3 || h4 || h5) ++refused;
                    { char w[200]; if (!sameModel(before, mA, w)) ++changedA; }
                    { char w[200]; if (!h4 && !h5 && !sameForDraw(mA, mB, w)) { if (!nDraw) sprintf_s(drawWhy, "value %d: %s", v, w); ++nDraw; } }
                    if (mA.rs != mB.rs) ++nRS; if (mA.ss != mB.ss) ++nSS; if (mA.tex != mB.tex) ++nTex;
                    if (shaderId(mA.vs) != shaderId(mB.vs)) ++nVS; if (shaderId(mA.ps) != shaderId(mB.ps)) ++nPS;
                    if (mA.vsF != mB.vsF) ++nCF; if (mA.psF != mB.psF) ++nCP;
                    if (mA.vsI != mB.vsI || mA.psI != mB.psI || mA.vsB != mB.vsB || mA.psB != mB.psB || mA.tss != mB.tss) ++nOther;
                    if (h4) { g_model = &mA; fxEndPass(fxA); fxEnd(fxA); fxBegin(fxA, &np, 6); fxBeginPass(fxA, 0); g_model = NULL; }
                    mB = mA;                                                  // the next try starts level again
                    // level the effect too: B is closed and opened, so both hold the same state inside D3DX
                    g_model = &mB; fxEndPass(fxB); fxEnd(fxB); fxBegin(fxB, &np, 6); fxBeginPass(fxB, 0); g_model = NULL; mB = mA;
                }
                if (!tries) continue;
                if (nRS || nSS || nTex || nVS || nPS || nCF || nCP || nOther || refused)
                    printf("   %-52s of %d writes: a commit left different - render states %d, sampler states %d, textures %d, vertex shader %d, pixel shader %d, vs constants %d, ps constants %d, other %d%s (reopening changed the device %d times) | what the next draw reads differs %d times%s%s\n",
                           a.name.c_str(), tries, nRS, nSS, nTex, nVS, nPS, nCF, nCP, nOther, refused ? " | refused" : "", changedA, nDraw, nDraw ? ": " : "", drawWhy);
                else if (changedA) printf("   %-52s of %d writes: a commit carries it over (reopening changed the device %d times)\n", a.name.c_str(), tries, changedA);
            }
            g_model = &mA; fxEndPass(fxA); fxEnd(fxA); g_model = &mB; fxEndPass(fxB); fxEnd(fxB); g_model = NULL;
        }
        return 0;
    }
    if (!strcmp(mode, "tab")) {
        // every shader the techniques bind for the int / bool values that open their pass: its tables
        std::map<DWORD, int> seen; int nSh = 0, nPre = 0, nBad = 0;
        static DWORD fnbuf[1 << 16];
        for (UINT t = 0; t < fd.Techniques; ++t) {
            DWORD th = fxTech(fxA, t); TechDesc td; FX(fxA, 5, HRESULT(WINAPI*)(void*, DWORD, TechDesc*))(fxA, th, &td);
            fxSetTech(fxA, th);
            for (int round = 0; round < 400; ++round) {
                if (round) for (size_t k = 0; k < pa.size(); ++k) if ((pa[k].d.Type == 1 || pa[k].d.Type == 2) && pa[k].d.Class != 4 && pa[k].d.Bytes == 4) { int v = (int)(rnd() % (pa[k].d.Type == 1 ? 2 : 6)); fxSetValue(fxA, pa[k].h, &v, 4); }
                Model m; UINT np = 0; g_model = &m; HRESULT h1 = fxBegin(fxA, &np, 6), h2 = fxBeginPass(fxA, 0); g_model = NULL;
                if (!h2) fxEndPass(fxA); if (!h1) fxEnd(fxA);
                if (h1 || h2) { restoreDefaults(fxA, pa); continue; }
                DWORD sh[2] = { m.vs, m.ps };
                for (int w = 0; w < 2; ++w) {
                    if (!sh[w] || seen.count(shaderId(sh[w]))) continue;
                    seen[shaderId(sh[w])] = 1; ++nSh;
                    UINT n = 0; void* o = (void*)(ULONG_PTR)sh[w];
                    if (FAILED(((HRESULT(WINAPI*)(void*, void*, UINT*))vt(o)[4])(o, NULL, &n)) || !n || n > sizeof(fnbuf) || FAILED(((HRESULT(WINAPI*)(void*, void*, UINT*))vt(o)[4])(o, fnbuf, &n))) { printf("  GetFunction failed\n"); ++nBad; continue; }
                    ShtTab tab; bool ok = shtParse(fnbuf, n, &tab);
                    if (!ok) { printf("  could not be read: version %08lX;", (unsigned long)fnbuf[0]);
                        for (UINT i = 1; i < n / 4; ) { DWORD tok = fnbuf[i]; if ((tok & 0xFFFF) != 0xFFFE) { printf(" then token %08lX", (unsigned long)tok); break; } DWORD k = (tok >> 16) & 0x7FFF;
                            printf(" comment %lu words '%.4s'", (unsigned long)k, (const char*)&fnbuf[i + 1]);
                            if (fnbuf[i + 1] == 0x53455250u) { printf(" {version %08lX:", (unsigned long)fnbuf[i + 2]); for (UINT j = i + 3; j < i + 1 + k; ) { DWORD t2 = fnbuf[j]; if ((t2 & 0xFFFF) != 0xFFFE) { printf(" token %08lX", (unsigned long)t2); break; } printf(" '%.4s' %lu", (const char*)&fnbuf[j + 1], (unsigned long)((t2 >> 16) & 0x7FFF)); j += 1 + ((t2 >> 16) & 0x7FFF); } printf("}"); }
                            i += 1 + k; }
                        printf("\n"); }
                    if (!ok) ++nBad; if (tab.hasPre) ++nPre;
                    printf("SHADER %s %s %08lX bytes=%u ok=%d main=%d pre=%d\n", td.Name, w ? "ps" : "vs", (unsigned long)shaderId(sh[w]), n, ok ? 1 : 0, tab.nMain, tab.hasPre ? tab.nPre : -1);
                    for (int i = 0; i < tab.nMain; ++i) printf("  M %-44s %c%-4u %3u  class %u rows %u cols %u elems %u members %u\n", tab.main[i].name, "bics"[tab.main[i].set & 3], tab.main[i].reg, tab.main[i].cnt, tab.main[i].cls, tab.main[i].rows, tab.main[i].cols, tab.main[i].elems, tab.main[i].members);
                    for (int i = 0; i < tab.nPre; ++i) printf("  P %-44s %c%-4u %3u\n", tab.pre[i].name, "bics"[tab.pre[i].set & 3], tab.pre[i].reg, tab.pre[i].cnt);
                }
            }
            restoreDefaults(fxA, pa);
        }
        printf("TOTAL %d shaders, %d with a preshader, %d could not be read\n", nSh, nPre, nBad);
        return 0;
    }
    if (!strcmp(mode, "pres")) {
        // every preshader program as aotr_shtab.inc reads it, written the way D3DX's disassembler writes it
        std::map<DWORD, int> seen; static DWORD fnbuf[1 << 16]; int nProg = 0, nBad = 0;
        for (UINT t = 0; t < fd.Techniques; ++t) {
            DWORD th = fxTech(fxA, t); TechDesc td; FX(fxA, 5, HRESULT(WINAPI*)(void*, DWORD, TechDesc*))(fxA, th, &td);
            fxSetTech(fxA, th);
            for (int round = 0; round < 400; ++round) {
                if (round) for (size_t k = 0; k < pa.size(); ++k) if ((pa[k].d.Type == 1 || pa[k].d.Type == 2) && pa[k].d.Class != 4 && pa[k].d.Bytes == 4) { int v = (int)(rnd() % (pa[k].d.Type == 1 ? 2 : 6)); fxSetValue(fxA, pa[k].h, &v, 4); }
                Model m; UINT np = 0; g_model = &m; HRESULT h1 = fxBegin(fxA, &np, 6), h2 = fxBeginPass(fxA, 0); g_model = NULL;
                if (!h2) fxEndPass(fxA); if (!h1) fxEnd(fxA);
                if (h1 || h2) { restoreDefaults(fxA, pa); continue; }
                DWORD sh[2] = { m.vs, m.ps };
                for (int w = 0; w < 2; ++w) {
                    if (!sh[w] || seen.count(shaderId(sh[w]))) continue;
                    seen[shaderId(sh[w])] = 1;
                    UINT n = 0; void* o = (void*)(ULONG_PTR)sh[w];
                    if (FAILED(((HRESULT(WINAPI*)(void*, void*, UINT*))vt(o)[4])(o, NULL, &n)) || !n || n > sizeof(fnbuf) || FAILED(((HRESULT(WINAPI*)(void*, void*, UINT*))vt(o)[4])(o, fnbuf, &n))) continue;
                    static ShtTab tab; if (!shtParse(fnbuf, n, &tab) || !tab.hasPre) continue;
                    ++nProg; if (!tab.prog.ok) ++nBad;
                    if (!tab.prog.ok) {                                          // where did the reader stop?
                        for (UINT i = 1; i < n / 4; ) { DWORD tok = fnbuf[i]; if ((tok & 0xFFFF) != 0xFFFE) break; DWORD k = (tok >> 16) & 0x7FFF;
                            if (fnbuf[i + 1] == 0x53455250u) for (UINT j = i + 3; j < i + 1 + k; ) { DWORD t2 = fnbuf[j]; if ((t2 & 0xFFFF) != 0xFFFE) break; DWORD k2 = (t2 >> 16) & 0x7FFF;
                                printf("  block '%.4s' %lu words", (const char*)&fnbuf[j + 1], (unsigned long)k2);
                                if (fnbuf[j + 1] == 0x434C5846u || fnbuf[j + 1] == 0x54494C43u) { printf(":"); for (UINT z = 0; z < 14 && z + 1 < k2; ++z) printf(" %lX", (unsigned long)fnbuf[j + 2 + z]); }
                                printf("\n");
                                if (fnbuf[j + 1] == 0x434C5846u) { const DWORD* p = &fnbuf[j + 3]; const DWORD* e = &fnbuf[j + 1 + k2]; ShtArg a;
                                    for (DWORD q = 0; q < fnbuf[j + 2]; ++q) { const DWORD* p0 = p; DWORD code = p[0], nin = p[1]; p += 2; bool bad = nin > 3 || (code & 0xFFFF) > 4 || !(code & 0xFFFF);
                                        for (DWORD z = 0; !bad && z <= nin; ++z) { p = shtArg(p, e, &a); if (!p) bad = true; }
                                        if (bad) { printf("  stopped at instruction %lu:", (unsigned long)q); for (int z = 0; z < 16; ++z) printf(" %lX", (unsigned long)p0[z]); printf("\n"); break; } } }
                                j += 1 + k2; }
                            i += 1 + k; } }
                    printf("PROGRAM %s %s ok=%d instructions=%d literals=%d main:", td.Name, w ? "ps" : "vs", tab.prog.ok, tab.prog.nIns, tab.prog.nLit);
                    for (int i = 0; i < tab.nMain; ++i) printf(" %s/%c%u/%u", tab.main[i].name, "bics"[tab.main[i].set & 3], tab.main[i].reg, tab.main[i].cnt);
                    printf("\n");
                    for (int i = 0; i < tab.prog.nIns; ++i) { const ShtIns& q = tab.prog.ins[i];
                        static const struct { WORD op; const char* name; } kOp[] = { {0x100,"mov"},{0x101,"neg"},{0x103,"rcp"},{0x104,"frc"},{0x105,"exp"},{0x106,"log"},{0x107,"rsq"},{0x108,"sin"},{0x109,"cos"},{0x10A,"asin"},{0x10B,"acos"},{0x10C,"atan"},
                            {0x200,"min"},{0x201,"max"},{0x202,"lt"},{0x203,"ge"},{0x204,"add"},{0x205,"mul"},{0x206,"atan2"},{0x208,"div"},{0x300,"cmp"},{0x500,"dot"} };
                        const char* nm = "?"; for (int k = 0; k < 22; ++k) if (kOp[k].op == q.op) nm = kOp[k].name;
                        printf("  I %s", nm); if (!strcmp(nm, "?")) printf("%03X", q.op);
                        for (int a = -1; a < (int)q.nin; ++a) { const ShtArg& g = a < 0 ? q.out : q.in[a];
                            // tables: 1 literal, 2 input (c), 4 output (c), 7 temporary (r); a register is four components
                            bool one = a == 0 && q.scalar; int comps = one ? 1 : q.ncomp;
                            if (g.tab == 1) { printf(" (");  for (int z = 0; z < comps; ++z) printf("%s%g", z ? ", " : "", g.off + z < (WORD)tab.prog.nLit ? tab.prog.lit[g.off + z] : -999.0); printf(")"); }
                            else { printf(" %c%u", g.tab == 7 ? 'r' : (g.tab == 2 || g.tab == 4) ? 'c' : (g.tab == 5 ? 'b' : 'i'), g.off / 4);
                                   if (g.idx) printf("[]");
                                   if (!(comps == 4 && (g.off & 3) == 0)) { printf("."); for (int z = 0; z < comps; ++z) printf("%c", "xyzw"[(g.off + z) & 3]); } }
                            printf(a < (int)q.nin - 1 ? "," : ""); }
                        printf("\n"); }
                }
            }
            restoreDefaults(fxA, pa);
        }
        printf("TOTAL %d preshader programs, %d could not be read\n", nProg, nBad);
        return 0;
    }
    if (!strcmp(mode, "image")) {
        // What reaches the device for a joint palette written with SetRawValue (part of it), and for a matrix written
        // with SetMatrix / SetMatrixTranspose: where, how many registers, in which layout.
        static DWORD fnbuf[1 << 16];
        for (UINT t = 0; t < fd.Techniques; ++t) {
            DWORD th = fxTech(fxA, t); TechDesc td; FX(fxA, 5, HRESULT(WINAPI*)(void*, DWORD, TechDesc*))(fxA, th, &td);
            for (int joints = 0; joints <= 2; ++joints) {
                restoreDefaults(fxA, pa);
                for (size_t i = 0; i < pa.size(); ++i) if (pa[i].name == "NumJointsPerVertex") fxSetValue(fxA, pa[i].h, &joints, 4);
                fxSetTech(fxA, th); UINT np = 0; Model m; g_model = &m;
                if (fxBegin(fxA, &np, 6) || fxBeginPass(fxA, 0)) { g_model = NULL; fxEndPass(fxA); fxEnd(fxA); printf("%s joints %d: the pass does not open\n", td.Name, joints); continue; }
                g_model = NULL;
                UINT n = 0; void* o = (void*)(ULONG_PTR)m.vs; ShtTab tab; memset(&tab, 0, sizeof(tab));
                if (m.vs && SUCCEEDED(((HRESULT(WINAPI*)(void*, void*, UINT*))vt(o)[4])(o, NULL, &n)) && n && n <= sizeof(fnbuf) && SUCCEEDED(((HRESULT(WINAPI*)(void*, void*, UINT*))vt(o)[4])(o, fnbuf, &n))) shtParse(fnbuf, n, &tab);
                for (size_t i = 0; i < pa.size(); ++i) {
                    const Param& a = pa[i];
                    bool pal = !strncmp(a.name.c_str(), "WorldBones", 10), mat = (a.d.Class == 2 || a.d.Class == 3) && a.d.Type == 3 && !a.d.Elements;
                    if (!pal && !mat) continue;
                    const ShtConst* c = NULL; for (int k = 0; k < tab.nMain; ++k) if (a.name == tab.main[k].name) c = &tab.main[k];
                    bool pre = false; for (int k = 0; k < tab.nPre; ++k) if (a.name == tab.pre[k].name) pre = true;
                    static float buf[1024]; for (int k = 0; k < 1024; ++k) buf[k] = 1000.0f + (float)k;
                    for (int how = 0; how < (pal ? 1 : 2); ++how) {
                        if (pal) fxSetRaw(fxA, a.h, buf, 0, 20 * 32);                       // 20 joints of the palette
                        else FX(fxA, how ? 44 : 38, HRESULT(WINAPI*)(void*, DWORD, const float*))(fxA, a.h, buf);
                        g_calls.clear(); g_rec = 1; g_model = &m; fxCommit(fxA); g_model = NULL; g_rec = 0;
                        printf("%-18s joints %d  %-22s %-18s table: %s%s | commit: %u calls", td.Name, joints, a.name.c_str(), pal ? "SetRawValue(640)" : how ? "SetMatrixTranspose" : "SetMatrix",
                               c ? "direct" : "-", pre ? " +preshader" : "", (unsigned)g_calls.size());
                        if (c) printf(" [c%u, %u registers, class %u %ux%u]", c->reg, c->cnt, c->cls, c->rows, c->cols);
                        for (size_t q = 0; q < g_calls.size(); ++q) { const Call& cl = g_calls[q];
                            printf(" | %s", slotName(cl.slot));
                            if (cl.slot == 94 || cl.slot == 109) { printf("(c%lu, %lu)", cl.a[0], cl.a[2]);
                                // the registers of this parameter, as the device now holds them
                                if (c && cl.slot == 94) { std::map<DWORD, std::vector<DWORD> >::iterator it = m.vsF.find(c->reg);
                                    if (it != m.vsF.end()) { printf(" first register of the parameter:"); for (int z = 0; z < 4; ++z) printf(" %g", *(float*)&it->second[z]);
                                        it = m.vsF.find(c->reg + 1); if (it != m.vsF.end()) { printf(" / second:"); for (int z = 0; z < 4; ++z) printf(" %g", *(float*)&it->second[z]); } } } }
                        }
                        printf("\n");
                    }
                }
                fxEndPass(fxA); fxEnd(fxA);
            }
        }
        return 0;
    }
    if (!strcmp(mode, "same")) {
        // Does D3DX leave out an upload when a parameter is written with the value it already holds, or when it believes
        // the register already has it? (It matters to anyone who writes registers behind its back.)
        for (UINT t = 0; t < fd.Techniques; ++t) {
            DWORD th = fxTech(fxA, t); TechDesc td; FX(fxA, 5, HRESULT(WINAPI*)(void*, DWORD, TechDesc*))(fxA, th, &td);
            fxSetTech(fxA, th); UINT np = 0; fxBegin(fxA, &np, 6); fxBeginPass(fxA, 0);
            for (size_t i = 0; i < pa.size(); ++i) if (pa[i].name == "World" || pa[i].name == "WorldBones" || pa[i].name == "NumJointsPerVertex" || pa[i].name == "Opacity") {
                static BYTE buf[4096]; UINT n = pa[i].d.Bytes; if (n > 4096) n = 4096;
                for (UINT k = 0; k + 4 <= n; k += 4) { if (pa[i].d.Type == 2) *(INT*)(buf + k) = 1; else *(float*)(buf + k) = 0.25f + (float)k; }
                fxSetValue(fxA, pa[i].h, buf, n); g_calls.clear(); g_rec = 1; fxCommit(fxA); g_rec = 0; size_t first = g_calls.size();
                fxSetValue(fxA, pa[i].h, buf, n); g_calls.clear(); g_rec = 1; fxCommit(fxA); g_rec = 0; size_t again = g_calls.size();
                g_calls.clear(); g_rec = 1; fxCommit(fxA); g_rec = 0; size_t none = g_calls.size();
                printf("  %-18s %-20s first write + commit: %u device calls | the same value written again + commit: %u | commit alone: %u\n", td.Name, pa[i].name.c_str(), (unsigned)first, (unsigned)again, (unsigned)none);
            }
            fxEndPass(fxA); fxEnd(fxA);
        }
        return 0;
    }
    if (!strcmp(mode, "time")) {
        LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
        for (int stub = 1; stub >= 0; --stub) {
            printf(stub ? "D3DX alone (the device's state methods answered by stubs):\n" : "with the real device (DXVK) underneath:\n");
            for (UINT t = 0; t < fd.Techniques; ++t) {
                DWORD th = fxTech(fxA, t); TechDesc td; FX(fxA, 5, HRESULT(WINAPI*)(void*, DWORD, TechDesc*))(fxA, th, &td);
                fxSetTech(fxA, th);
                DWORD hWorld = 0; UINT wBytes = 0; for (size_t i = 0; i < pa.size(); ++i) if (pa[i].name == "World") { hWorld = pa[i].h; wBytes = pa[i].d.Bytes; }
                const int N = 20000; UINT np = 0; float m[16]; for (int k = 0; k < 16; ++k) m[k] = frand();
                g_stub = stub;
                LARGE_INTEGER a, b, c, d, e;
                QueryPerformanceCounter(&a);
                for (int i = 0; i < N; ++i) { fxBegin(fxA, &np, 6); fxBeginPass(fxA, 0); fxEndPass(fxA); fxEnd(fxA); }
                QueryPerformanceCounter(&b);
                fxBegin(fxA, &np, 6); fxBeginPass(fxA, 0);
                for (int i = 0; i < N; ++i) { m[12] = (float)i; if (hWorld) fxSetValue(fxA, hWorld, m, wBytes); fxCommit(fxA); }
                QueryPerformanceCounter(&c);
                for (int i = 0; i < N; ++i) { fxCommit(fxA); }
                QueryPerformanceCounter(&d);
                for (int i = 0; i < N; ++i) { m[12] = (float)i; if (hWorld) fxSetValue(fxA, hWorld, m, wBytes); }
                QueryPerformanceCounter(&e);
                fxEndPass(fxA); fxEnd(fxA);
                g_stub = 0;
                #define US(x, y) ((double)((y).QuadPart - (x).QuadPart) * 1e6 / (double)qf.QuadPart / N)
                printf("  %-20s Begin+BeginPass+EndPass+End %.3f us | write World + CommitChanges %.3f us | CommitChanges with nothing written %.3f us | write World alone %.3f us\n",
                       td.Name, US(a, b), US(b, c), US(c, d), US(d, e));
            }
        }
        return 0;
    }
    printf("unknown mode %s\n", mode);
    return 2;
}
