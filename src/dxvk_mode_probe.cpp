// dxvk_mode_probe: which present mode does the game's DXVK choose for a configuration string?
//   dxvk_mode_probe.exe "<DXVK_CONFIG text>" [fullscreen]
// Sets DXVK_CONFIG for this process only, makes a device on the d3d9.dll next to the exe, presents a few pictures and
// leaves; DXVK writes "Presenter: Actual swapchain properties: ... Present mode: ..." to dxvk_mode_probe_d3d9.log in
// the current directory.
#include <windows.h>
#include <stdio.h>
static void** vt(void* o) { return *(void***)o; }
typedef struct { UINT BackBufferWidth, BackBufferHeight; DWORD BackBufferFormat; UINT BackBufferCount;
    DWORD MultiSampleType, MultiSampleQuality, SwapEffect; HWND hDeviceWindow; BOOL Windowed;
    BOOL EnableAutoDepthStencil; DWORD AutoDepthStencilFormat, Flags; UINT FullScreen_RefreshRateInHz, PresentationInterval; } PP;
int main(int argc, char** argv) {
    SetEnvironmentVariableA("DXVK_CONFIG", argc > 1 ? argv[1] : "");
    SetEnvironmentVariableA("DXVK_LOG_LEVEL", "info"); SetEnvironmentVariableA("DXVK_STATE_CACHE", "0");
    char dll[MAX_PATH]; GetModuleFileNameA(NULL, dll, MAX_PATH); char* s = strrchr(dll, '\\'); if (s) lstrcpyA(s + 1, "d3d9.dll");
    HMODULE hd3d = LoadLibraryA(dll); if (!hd3d) { printf("no d3d9.dll next to the exe\n"); return 3; }
    typedef void* (WINAPI* Create9F)(UINT); Create9F Create9 = (Create9F)GetProcAddress(hd3d, "Direct3DCreate9");
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "dxvkmode"; RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "dxvkmode", "m", WS_POPUP, -3000, -3000, 320, 240, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    void* d3d = Create9(32); if (!d3d) { printf("Direct3DCreate9 failed\n"); return 4; }
    PP pp = {0}; pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.BackBufferFormat = 21; pp.BackBufferCount = 1; pp.SwapEffect = 1; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = 75; pp.PresentationInterval = 1;      // the game asks for vsync; the configuration overrides it
    void* dev = NULL;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, DWORD, HWND, DWORD, PP*, void**))vt(d3d)[16])(d3d, 0, 1, hwnd, 0x40u | 0x2u | 0x4u, &pp, &dev);
    if (FAILED(hr) || !dev) { printf("CreateDevice hr=0x%08lX\n", (unsigned long)hr); return 4; }
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (int i = 0; i < 40; ++i) {
        ((HRESULT(WINAPI*)(void*, DWORD, void*, DWORD, DWORD, float, DWORD))vt(dev)[43])(dev, 0, NULL, 1, 0xFF000000u | (DWORD)(i * 6), 1.0f, 0);
        ((HRESULT(WINAPI*)(void*, void*, void*, HWND, void*))vt(dev)[17])(dev, NULL, NULL, NULL, NULL);
    }
    QueryPerformanceCounter(&t1);
    printf("40 presents in %.1f ms (%.2f ms each)\n", (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart, (double)(t1.QuadPart - t0.QuadPart) * 25.0 / (double)f.QuadPart);
    Sleep(300);
    return 0;
}
