// fxparams.cpp - lists the top-level parameters of compiled effects (name, semantic, class, type, shape), and for
// one technique the vertex shader disassembly. Used to learn how the game's shaders take the camera and the
// object transforms (in-between frames, aotr_tween.inc).  fxparams <effect.fxo>... [--dis <technique>]
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void** vt(void* o) { return *(void***)o; }
typedef struct { UINT BackBufferWidth, BackBufferHeight; DWORD BackBufferFormat; UINT BackBufferCount;
    DWORD MultiSampleType, MultiSampleQuality, SwapEffect; HWND hDeviceWindow; BOOL Windowed;
    BOOL EnableAutoDepthStencil; DWORD AutoDepthStencilFormat, Flags; UINT FullScreen_RefreshRateInHz, PresentationInterval; } PP;
struct EDesc { const char* Creator; UINT Parameters, Techniques, Functions; };
struct PDesc { const char* Name; const char* Semantic; DWORD Class, Type; UINT Rows, Columns, Elements, Annotations, StructMembers; DWORD Flags; UINT Bytes; };
static const char* kClass[] = { "scalar", "vector", "matrix_rows", "matrix_columns", "object", "struct" };
static const char* kType[] = { "void", "bool", "int", "float", "string", "texture", "texture1D", "texture2D", "texture3D", "textureCUBE", "sampler", "sampler1D", "sampler2D",
                               "sampler3D", "samplerCUBE", "pixelshader", "vertexshader", "pixelfragment", "vertexfragment" };

int main(int argc, char** argv) {
    HMODULE hd3d = LoadLibraryA("d3d9.dll"), hx = LoadLibraryA("d3dx9_27.dll");
    if (!hd3d || !hx) { printf("d3d9 / d3dx9_27 missing\n"); return 3; }
    typedef void* (WINAPI* Create9F)(UINT);
    typedef HRESULT (WINAPI* CreateEffectF)(void*, LPCSTR, const void*, void*, DWORD, void*, void**, void**);
    typedef HRESULT (WINAPI* DisEffF)(void*, BOOL, void**);
    Create9F Create9 = (Create9F)GetProcAddress(hd3d, "Direct3DCreate9");
    CreateEffectF CreateEffect = (CreateEffectF)GetProcAddress(hx, "D3DXCreateEffectFromFileA");
    DisEffF DisEff = (DisEffF)GetProcAddress(hx, "D3DXDisassembleEffect");
    SetEnvironmentVariableA("DXVK_LOG_LEVEL", "none");
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "fxp";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("fxp", "fx", WS_OVERLAPPED, 0, 0, 8, 8, NULL, NULL, wc.hInstance, NULL);
    void* d3d = Create9(32); if (!d3d) return 4;
    PP pp = {0}; pp.Windowed = TRUE; pp.SwapEffect = 1; pp.hDeviceWindow = hwnd; pp.BackBufferWidth = 8; pp.BackBufferHeight = 8;
    void* dev = NULL;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, DWORD, HWND, DWORD, PP*, void**))vt(d3d)[16])(d3d, 0, 1, hwnd, 0x40, &pp, &dev);
    if (FAILED(hr) || !dev) { printf("CreateDevice 0x%08lX\n", (unsigned long)hr); return 5; }
    const char* disTech = NULL;
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--dis") && i + 1 < argc) disTech = argv[i + 1];
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dis")) { ++i; continue; }
        void* fx = NULL, *err = NULL;
        hr = CreateEffect(dev, argv[i], NULL, NULL, 0, NULL, &fx, &err);
        if (FAILED(hr) || !fx) { printf("== %s: load failed 0x%08lX\n", argv[i], (unsigned long)hr); continue; }
        EDesc ed; memset(&ed, 0, sizeof(ed));
        ((HRESULT(WINAPI*)(void*, EDesc*))vt(fx)[3])(fx, &ed);
        printf("== %s: %u parameters, %u techniques\n", argv[i], ed.Parameters, ed.Techniques);
        for (UINT p = 0; p < ed.Parameters; ++p) {
            DWORD h = ((DWORD(WINAPI*)(void*, DWORD, UINT))vt(fx)[8])(fx, 0, p);
            PDesc pd; memset(&pd, 0, sizeof(pd));
            ((HRESULT(WINAPI*)(void*, DWORD, PDesc*))vt(fx)[4])(fx, h, &pd);
            if (pd.Type >= 5 && pd.Type <= 14) continue;                              // textures and samplers
            printf("   %-28s %-22s %-14s %-6s %ux%u", pd.Name ? pd.Name : "?", pd.Semantic ? pd.Semantic : "-", pd.Class < 6 ? kClass[pd.Class] : "?", pd.Type < 19 ? kType[pd.Type] : "?", pd.Rows, pd.Columns);
            if (pd.Elements) printf(" [%u]", pd.Elements);
            printf("  %u bytes\n", pd.Bytes);
        }
        if (disTech && DisEff) {
            void* txt = NULL;
            if (SUCCEEDED(DisEff(fx, FALSE, &txt)) && txt) {
                const char* t = (const char*)((void*(WINAPI*)(void*))vt(txt)[3])(txt); DWORD n = ((DWORD(WINAPI*)(void*))vt(txt)[4])(txt);
                char path[MAX_PATH]; _snprintf(path, sizeof(path), "%s.dis.txt", argv[i]);
                FILE* f = fopen(path, "wb"); if (f) { fwrite(t, 1, n, f); fclose(f); printf("   disassembly -> %s (%lu bytes)\n", path, n); }
            }
        }
        ((ULONG(WINAPI*)(void*))vt(fx)[2])(fx);
    }
    return 0;
}
