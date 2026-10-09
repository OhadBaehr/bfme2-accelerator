// mipprobe: what does D3DXFilterTexture(tex, NULL, 0, D3DX_FILTER_BOX) make of an A1R5G5B5 texture?
// Level 0 of a 2048 x 2048 texture holds every combination of four 5-bit values as a 2 x 2 block (in the red
// channel; green has them in another order, blue and alpha are hashed), so level 1 answers the question for one
// level exhaustively; the further levels say whether each is made from the one before it.
// Run under the three x87 precisions (the game's device may or may not have been created with FPU_PRESERVE).
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <vector>

struct PP { UINT BackBufferWidth, BackBufferHeight; DWORD BackBufferFormat; UINT BackBufferCount; DWORD MultiSampleType, MultiSampleQuality, SwapEffect; HWND hDeviceWindow; BOOL Windowed, EnableAutoDepthStencil; DWORD AutoDepthStencilFormat, Flags; UINT FullScreen_RefreshRateInHz, PresentationInterval; };
struct LR { INT Pitch; void* pBits; };
static void** vt(void* o) { return *(void***)o; }
typedef HRESULT (WINAPI* CreateTexF)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**);
typedef HRESULT (WINAPI* FilterF)(void*, const void*, UINT, DWORD);
static HRESULT lockLv(void* tex, UINT lv, LR* lr, DWORD flags) { return ((HRESULT(WINAPI*)(void*, UINT, LR*, const RECT*, DWORD))vt(tex)[19])(tex, lv, lr, NULL, flags); }
static HRESULT unlockLv(void* tex, UINT lv) { return ((HRESULT(WINAPI*)(void*, UINT))vt(tex)[20])(tex, lv); }
static DWORD levels(void* tex) { return ((DWORD(WINAPI*)(void*))vt(tex)[13])(tex); }
static unsigned hash32(unsigned x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }

// candidates for one 5-bit channel
static int cRound(int a, int b, int c, int d) { return (a + b + c + d + 2) >> 2; }
static int cTrunc(int a, int b, int c, int d) { return (a + b + c + d) >> 2; }
static int cEven(int a, int b, int c, int d) { int s = a + b + c + d, q = s >> 2, r = s & 3; return r < 2 ? q : r > 2 ? q + 1 : (q & 1) ? q + 1 : q; }
// through 8 bits: expand 5 -> 8 the way D3DX might (v * 255 / 31 rounded), average, back
static int cVia8(int a, int b, int c, int d) { int e[4] = { a, b, c, d }, s = 0; for (int i = 0; i < 4; ++i) s += (e[i] * 255 + 15) / 31; int m = (s + 2) >> 2; return (m * 31 + 127) / 255; }
static int cFloat(int a, int b, int c, int d) { float s = ((float)a / 31.0f + (float)b / 31.0f + (float)c / 31.0f + (float)d / 31.0f) * 0.25f; return (int)(s * 31.0f + 0.5f); }

int main(int argc, char** argv) {
    SetEnvironmentVariableA("DXVK_CONFIG", "dxvk.hud="); SetEnvironmentVariableA("DXVK_LOG_LEVEL", "none"); SetEnvironmentVariableA("DXVK_STATE_CACHE", "0");
    HMODULE hx = LoadLibraryA("d3dx9_27.dll"), hd = LoadLibraryA("d3d9.dll");
    if (!hx || !hd) { printf("d3dx9_27.dll / d3d9.dll missing\n"); return 3; }
    CreateTexF XCreateTexture = (CreateTexF)GetProcAddress(hx, "D3DXCreateTexture"); FilterF XFilter = (FilterF)GetProcAddress(hx, "D3DXFilterTexture");
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "mipprobe"; RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "mipprobe", "t", WS_POPUP, -3000, -3000, 640, 480, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    void* d3d = ((void*(WINAPI*)(UINT))GetProcAddress(hd, "Direct3DCreate9"))(32);
    PP pp = {0}; pp.BackBufferWidth = 640; pp.BackBufferHeight = 480; pp.BackBufferFormat = 21; pp.BackBufferCount = 1; pp.SwapEffect = 1; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = 75; pp.PresentationInterval = 0x80000000;
    void* dev = NULL;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, DWORD, HWND, DWORD, PP*, void**))vt(d3d)[16])(d3d, 0, 1, hwnd, 0x40u | 0x2u, &pp, &dev);
    if (FAILED(hr) || !dev) { printf("CreateDevice hr=0x%08lX\n", (unsigned long)hr); return 4; }
    const int W = 2048;
    std::vector<WORD> src((size_t)W * W);
    for (int by = 0; by < W / 2; ++by) for (int bx = 0; bx < W / 2; ++bx) {
        unsigned k = (unsigned)by * (W / 2) + bx;
        int r[4] = { (int)(k & 31), (int)((k >> 5) & 31), (int)((k >> 10) & 31), (int)((k >> 15) & 31) };
        int g[4] = { r[3], r[1], r[0], r[2] };
        for (int s = 0; s < 4; ++s) {
            unsigned h = hash32(k * 4 + s);
            int b = (int)(h & 31), a = (int)((h >> 9) & 1);
            src[(size_t)(by * 2 + (s >> 1)) * W + bx * 2 + (s & 1)] = (WORD)((a << 15) | (r[s] << 10) | (g[s] << 5) | b);
        }
    }
    static const unsigned kPc[3] = { _PC_24, _PC_53, _PC_64 }; static const char* kPcName[3] = { "24 bit", "53 bit", "64 bit" };
    int pools[2] = { 1, 2 };
    std::vector<std::vector<WORD> > ref;
    for (int pc = 0; pc < 3; ++pc) for (int pl = 0; pl < 2; ++pl) {
        void* tex = NULL;
        hr = XCreateTexture(dev, W, W, 0, 0, 25, pools[pl], &tex);
        if (FAILED(hr) || !tex) { printf("D3DXCreateTexture hr=0x%08lX\n", (unsigned long)hr); return 5; }
        DWORD nl = levels(tex);
        LR lr; lockLv(tex, 0, &lr, 0);
        for (int y = 0; y < W; ++y) memcpy((BYTE*)lr.pBits + (size_t)y * lr.Pitch, &src[(size_t)y * W], W * 2);
        unlockLv(tex, 0);
        unsigned keep = _control87(0, 0);
        _control87(kPc[pc], _MCW_PC);
        LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        hr = XFilter(tex, NULL, 0, 5);
        QueryPerformanceCounter(&t1);
        _control87(keep, _MCW_PC);
        std::vector<std::vector<WORD> > lv(nl);
        for (DWORD l = 0; l < nl; ++l) {
            int w = W >> l; if (w < 1) w = 1;
            lv[l].resize((size_t)w * w);
            lockLv(tex, l, &lr, 0x10);
            for (int y = 0; y < w; ++y) memcpy(&lv[l][(size_t)y * w], (BYTE*)lr.pBits + (size_t)y * lr.Pitch, w * 2);
            unlockLv(tex, l);
        }
        ((ULONG(WINAPI*)(void*))vt(tex)[2])(tex);
        printf("x87 precision %s, pool %d: D3DXFilterTexture hr=0x%08lX, %lu levels, %.1f ms\n", kPcName[pc], pools[pl], (unsigned long)hr, (unsigned long)nl, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
        if (memcmp(&lv[0][0], &src[0], src.size() * 2)) printf("   level 0 was CHANGED by the filter\n");
        // level 1, channel by channel, against the candidates
        typedef int (*Cand)(int, int, int, int);
        static const Cand cands[5] = { cRound, cTrunc, cEven, cVia8, cFloat }; static const char* cname[5] = { "(sum+2)>>2", "sum>>2", "half to even", "through 8 bits", "float /31 *0.25 *31 +0.5" };
        for (int ch = 0; ch < 3; ++ch) {
            int sh = ch == 0 ? 10 : ch == 1 ? 5 : 0;
            for (int cd = 0; cd < 5; ++cd) {
                long bad = 0; int ex[6] = { 0 };
                for (int by = 0; by < W / 2; ++by) for (int bx = 0; bx < W / 2; ++bx) {
                    int v[4]; for (int s = 0; s < 4; ++s) v[s] = (src[(size_t)(by * 2 + (s >> 1)) * W + bx * 2 + (s & 1)] >> sh) & 31;
                    int got = (lv[1][(size_t)by * (W / 2) + bx] >> sh) & 31, want = cands[cd](v[0], v[1], v[2], v[3]);
                    if (got != want) { if (!bad) { ex[0] = v[0]; ex[1] = v[1]; ex[2] = v[2]; ex[3] = v[3]; ex[4] = got; ex[5] = want; } ++bad; }
                }
                if (pl == 0 || bad) printf("   level 1 %s vs %-26s: %ld of %d blocks differ%s", ch == 0 ? "red  " : ch == 1 ? "green" : "blue ", cname[cd], bad, (W / 2) * (W / 2), bad ? "" : "\n");
                if (bad) printf(" (first: %d %d %d %d -> D3DX %d, candidate %d)\n", ex[0], ex[1], ex[2], ex[3], ex[4], ex[5]);
            }
        }
        { long bad[4] = { 0, 0, 0, 0 }; long cnt[5][2]; memset(cnt, 0, sizeof(cnt));
          for (int by = 0; by < W / 2; ++by) for (int bx = 0; bx < W / 2; ++bx) {
              int s = 0; for (int q = 0; q < 4; ++q) s += (src[(size_t)(by * 2 + (q >> 1)) * W + bx * 2 + (q & 1)] >> 15) & 1;
              int got = (lv[1][(size_t)by * (W / 2) + bx] >> 15) & 1; cnt[s][got]++;
          }
          printf("   level 1 alpha by number of set bits among the four: ");
          for (int s = 0; s <= 4; ++s) printf("%d -> %s  ", s, cnt[s][0] && cnt[s][1] ? "MIXED" : cnt[s][1] ? "1" : "0");
          printf("\n"); (void)bad; }
        // further levels: from the level before, with (sum+2)>>2 per colour channel and alpha as found
        for (DWORD l = 2; l < nl; ++l) {
            int w = W >> l, pw = w * 2; long badPrev = 0;
            for (int y = 0; y < w; ++y) for (int x = 0; x < w; ++x) {
                WORD p[4] = { lv[l - 1][(size_t)(y * 2) * pw + x * 2], lv[l - 1][(size_t)(y * 2) * pw + x * 2 + 1], lv[l - 1][(size_t)(y * 2 + 1) * pw + x * 2], lv[l - 1][(size_t)(y * 2 + 1) * pw + x * 2 + 1] };
                int r = cRound((p[0] >> 10) & 31, (p[1] >> 10) & 31, (p[2] >> 10) & 31, (p[3] >> 10) & 31), g = cRound((p[0] >> 5) & 31, (p[1] >> 5) & 31, (p[2] >> 5) & 31, (p[3] >> 5) & 31), b = cRound(p[0] & 31, p[1] & 31, p[2] & 31, p[3] & 31);
                int as = (p[0] >> 15) + (p[1] >> 15) + (p[2] >> 15) + (p[3] >> 15);
                WORD want = (WORD)(((as >= 2) ? 0x8000 : 0) | (r << 10) | (g << 5) | b);
                if (want != lv[l][(size_t)y * w + x]) ++badPrev;
            }
            printf("   level %lu (%d x %d) from the level before with (sum+2)>>2, alpha >= 2 of 4: %ld differ\n", (unsigned long)l, w, w, badPrev);
        }
        if (ref.empty()) ref = lv;
        else { bool same = ref.size() == lv.size(); for (size_t l = 0; same && l < lv.size(); ++l) same = ref[l] == lv[l]; printf("   all levels %s the first run's\n", same ? "IDENTICAL to" : "DIFFERENT from"); }
    }
    return 0;
}
