// psasm: assembles the pan-warp pixel shader with the game's own D3DX (d3dx9_27.dll) and prints the bytecode as a
// C array for aotr_panwarp.inc. Run from src\ (the DLL is next to it).
#include <windows.h>
#include <stdio.h>
#include <string.h>
static const char kSrc[] =
    "ps_2_0\n"
    "def c0, 1.0, 1.0, 1.0, 0.0\n"
    "def c1, 0.0000037, 0.0, 1.0, 0.0\n"
    "dcl t0.xy\n"
    "dcl t1.xyzw\n"
    "dcl_2d s0\n"
    "dcl_2d s1\n"
    "dcl_2d s2\n"
    "texld r0, t0, s0\n"            // the picture as it was finished: scene + interface
    "texld r1, t0, s1\n"            // the scene before the interface was drawn, at the same place
    "rcp r2.w, t1.w\n"
    "mul r2.xy, t1, r2.w\n"         // where the scene under this pixel comes from
    "sub r3.xy, c1.z, r2\n"
    "min r3.xy, r3, r2\n"
    "min r3.x, r3.x, r3.y\n"        // >= 0: inside the picture
    "texld r4, r2, s2\n"            // the scene from there
    "cmp r4, r3.x, r4, r1\n"        // outside the picture: the scene where it was
    "sub r5, r0, r1\n"
    "mul r5, r5, r5\n"
    "dp3 r5.x, r5, c0\n"
    "sub r5.x, r5.x, c1.x\n"        // >= 0: the interface drew on this pixel
    "cmp r0, r5.x, r0, r4\n"
    "mov oC0, r0\n";
int main() {
    HMODULE hx = LoadLibraryA("d3dx9_27.dll");
    if (!hx) { printf("no d3dx9_27.dll\n"); return 2; }
    typedef HRESULT (WINAPI* AsmF)(const char*, UINT, const void*, void*, DWORD, void**, void**);
    AsmF as = (AsmF)GetProcAddress(hx, "D3DXAssembleShader");
    if (!as) { printf("no D3DXAssembleShader\n"); return 3; }
    void* code = NULL, *err = NULL;
    HRESULT hr = as(kSrc, (UINT)strlen(kSrc), NULL, NULL, 0, &code, &err);
    if (FAILED(hr) || !code) {
        printf("assemble failed %08lX\n", (unsigned long)hr);
        if (err) { void** vt = *(void***)err; const char* m = ((const char*(WINAPI*)(void*))vt[3])(err); printf("%s\n", m ? m : ""); }
        return 4;
    }
    void** vt = *(void***)code;
    const DWORD* p = ((const DWORD*(WINAPI*)(void*))vt[3])(code);
    DWORD n = ((DWORD(WINAPI*)(void*))vt[4])(code) / 4;
    printf("// %u dwords\nstatic const DWORD kPwPs[%u] = {", (unsigned)n, (unsigned)n);
    for (DWORD i = 0; i < n; ++i) printf("%s0x%08lX,", (i % 8) ? " " : "\n    ", (unsigned long)p[i]);
    printf("\n};\n");
    return 0;
}
