// vblank_probe: does Windows tell when each monitor refreshes? (the calls aotr_screen.inc uses)
#include <windows.h>
#include <stdio.h>
struct ScrOpen { HDC hDc; UINT hAdapter; LUID luid; UINT source; };
struct ScrWait { UINT hAdapter, hDevice, source; };
struct ScrClose { UINT hAdapter; };
struct ScrScan { UINT hAdapter; UINT source; BOOLEAN inVBlank; UINT line; };
typedef LONG (WINAPI* OpenF)(ScrOpen*); typedef LONG (WINAPI* WaitF)(const ScrWait*); typedef LONG (WINAPI* CloseF)(const ScrClose*); typedef LONG (WINAPI* ScanF)(ScrScan*);
static OpenF g_open; static WaitF g_wait; static CloseF g_close; static ScanF g_scan;
static BOOL CALLBACK mon(HMONITOR hm, HDC, LPRECT, LPARAM) {
    MONITORINFOEXA mi; memset(&mi, 0, sizeof(mi)); mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(hm, (MONITORINFO*)&mi)) return TRUE;
    DEVMODEA dm; memset(&dm, 0, sizeof(dm)); dm.dmSize = sizeof(dm); EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
    printf("%s%s %lux%lu @ %lu Hz: ", mi.szDevice, (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
    HDC dc = CreateDCA(NULL, mi.szDevice, NULL, NULL);
    ScrOpen oa; memset(&oa, 0, sizeof(oa)); oa.hDc = dc;
    LONG st = dc ? g_open(&oa) : -1; if (dc) DeleteDC(dc);
    if (st < 0 || !oa.hAdapter) { printf("adapter not opened (0x%08lX)\n", (unsigned long)st); return TRUE; }
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f);
    ScrScan sc; memset(&sc, 0, sizeof(sc)); sc.hAdapter = oa.hAdapter; sc.source = oa.source; LONG ss = g_scan ? g_scan(&sc) : -1;
    printf("source %u, scan line now %u%s (0x%08lX); ", oa.source, sc.line, sc.inVBlank ? " in the blank" : "", (unsigned long)ss);
    ScrWait w = { oa.hAdapter, 0, oa.source }; double last = 0, sum = 0, mn = 1e9, mx = 0; int n = 0, fail = 0; LONG firstFail = 0;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 130; ++i) {
        st = g_wait(&w); QueryPerformanceCounter(&t);
        if (st < 0) { if (!fail++) firstFail = st; Sleep(5); continue; }
        double ms = (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        if (i > 8 && last > 0) { double d = ms - last; sum += d; ++n; if (d < mn) mn = d; if (d > mx) mx = d; }
        last = ms;
    }
    if (n) printf("%d refreshes waited for, %.4f ms apart on average (%.3f to %.3f); %d waits failed", n, sum / n, mn, mx, fail);
    else printf("no wait succeeded (first failure 0x%08lX, %d failures)", (unsigned long)firstFail, fail);
    printf("\n");
    ScrClose cl = { oa.hAdapter }; g_close(&cl);
    return TRUE;
}
int main() {
    HMODULE g = LoadLibraryA("gdi32.dll");
    g_open = (OpenF)GetProcAddress(g, "D3DKMTOpenAdapterFromHdc"); g_wait = (WaitF)GetProcAddress(g, "D3DKMTWaitForVerticalBlankEvent");
    g_close = (CloseF)GetProcAddress(g, "D3DKMTCloseAdapter"); g_scan = (ScanF)GetProcAddress(g, "D3DKMTGetScanLine");
    if (!g_open || !g_wait || !g_close) { printf("calls missing\n"); return 2; }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    EnumDisplayMonitors(NULL, NULL, mon, 0);
    LASTINPUTINFO li; li.cbSize = sizeof(li); if (GetLastInputInfo(&li)) printf("last keyboard or mouse input %lu s ago\n", (GetTickCount() - li.dwTime) / 1000);
    return 0;
}
