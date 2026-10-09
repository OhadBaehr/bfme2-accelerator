// texjob_test.cpp - offline proof and measurement for the memory textures (aotr_texmem.inc).
//
// Runs the mod's REAL texture files (every .dds / .tga in the game's and the mod's .big archives and loose folders)
// through the three sequences the engine loads textures with, twice each:
//   reference  D3DX working on a real texture of the game's own Direct3D (DXVK's d3d9.dll), as the engine does it
//   memory     the same d3dx9_27.dll functions, same arguments, working on a memory texture - no device involved
// and compares every mip level of the two byte for byte. It also checks the PROBE (D3DX's CreateTexture arguments
// learned without decoding), the upload of a memory texture into a real one, eight threads decoding at once against
// the single-threaded result, and the two x87 precision settings a game thread can be in.
//
//   W2  D3DXCreateTextureFromFileInMemoryEx(file, w, h, levels 0, format 0, MANAGED, DEFAULT, BOX)   (.dds, odd .tga)
//   W1  D3DXCreateTexture + the engine's own row copy + D3DXFilterTexture(BOX)                        (plain .tga)
//   T1  D3DXLoadSurfaceFromMemory(X8R8G8B8 -> the texture's format, no filter) + D3DXFilterTexture(BOX) (terrain atlas)
//
//   texjob_test [stride] [maxMB] [threads]      stride N: every Nth file of each kind (default 12), 1 = all
//   texjob_test dll [stride] [maxMB]            the same sequences THROUGH bfme2_accel.new.dll with its render thread
//                                               live: reference hashes first (plain D3DX), then the DLL's wrappers, the
//                                               textures read back through the hooked methods and compared
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include <intrin.h>
#include <vector>
#include <string>
#include <map>
#include <algorithm>

#define TX_LOG(...) (printf(__VA_ARGS__), printf("\n"))
#include "aotr_texmem.inc"

static void** vt(void* o) { return *(void***)o; }
typedef struct { UINT BackBufferWidth, BackBufferHeight; DWORD BackBufferFormat; UINT BackBufferCount;
    DWORD MultiSampleType, MultiSampleQuality, SwapEffect; HWND hDeviceWindow; BOOL Windowed;
    BOOL EnableAutoDepthStencil; DWORD AutoDepthStencilFormat, Flags; UINT FullScreen_RefreshRateInHz, PresentationInterval; } PP;
struct IMGINFO { UINT Width, Height, Depth, MipLevels; DWORD Format, ResourceType, ImageFileFormat; };

typedef HRESULT (WINAPI* CreateFromFileF)(void*, const void*, UINT, UINT, UINT, UINT, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, void*, void*, void**);
typedef HRESULT (WINAPI* CreateTexF)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**);
typedef HRESULT (WINAPI* CheckReqF)(void*, UINT*, UINT*, UINT*, DWORD, DWORD*, DWORD);
typedef HRESULT (WINAPI* LoadSurfMemF)(void*, const void*, const TxRECT*, const void*, DWORD, UINT, const void*, const TxRECT*, DWORD, DWORD);
typedef HRESULT (WINAPI* FilterF)(void*, const void*, UINT, DWORD);
typedef HRESULT (WINAPI* ImgInfoF)(const void*, UINT, IMGINFO*);
static CreateFromFileF XCreateFromFile; static CreateTexF XCreateTexture; static CheckReqF XCheckReq; static LoadSurfMemF XLoadSurfMem; static FilterF XFilter; static ImgInfoF XImgInfo;

static LARGE_INTEGER g_qpf;
static double ms(LONGLONG t) { return (double)t * 1000.0 / (double)g_qpf.QuadPart; }
static LONGLONG now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// ---------------------------------------------------------------- the files
struct File { std::string name, src; const BYTE* p; DWORD n; int kind; };      // kind 0 dds, 1 plain tga (engine row copy), 2 other tga
static std::vector<File> g_files;
static bool tgaPlain(const BYTE* h, DWORD n) {
    if (n < 18) return false;
    WORD w = *(const WORD*)(h + 12);
    return h[2] == 2 && h[0] == 0 && h[1] == 0 && (h[16] == 24 || h[16] == 32) && (w % 4) == 0 && !(h[17] & 0xF0);
}
static void addFile(const std::string& name, const std::string& src, const BYTE* p, DWORD n) {
    size_t dot = name.rfind('.'); if (dot == std::string::npos) return;
    std::string ext = name.substr(dot); for (size_t i = 0; i < ext.size(); ++i) ext[i] = (char)tolower((unsigned char)ext[i]);
    File f; f.name = name; f.src = src; f.p = p; f.n = n;
    if (ext == ".dds") f.kind = 0; else if (ext == ".tga") f.kind = tgaPlain(p, n) ? 1 : 2; else return;
    g_files.push_back(f);
}
static const BYTE* mapFile(const char* path, DWORD* size) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    *size = GetFileSize(h, NULL);
    HANDLE m = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
    const BYTE* p = m ? (const BYTE*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0) : NULL;
    return p;
}
static DWORD be32(const BYTE* p) { return ((DWORD)p[0] << 24) | ((DWORD)p[1] << 16) | ((DWORD)p[2] << 8) | p[3]; }
static bool isTexName(const char* nm) { const char* dot = strrchr(nm, '.'); return dot && (!_stricmp(dot, ".dds") || !_stricmp(dot, ".tga")); }
static void addBig(const char* path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    BYTE hdr[16]; DWORD got = 0;
    if (!ReadFile(h, hdr, 16, &got, NULL) || got != 16 || (memcmp(hdr, "BIGF", 4) && memcmp(hdr, "BIG4", 4))) { CloseHandle(h); return; }
    DWORD n = be32(hdr + 8), first = be32(hdr + 12);
    if (first < 16 || first > (64u << 20)) { CloseHandle(h); return; }
    std::vector<BYTE> dir(first - 16 + 1);
    if (!ReadFile(h, &dir[0], first - 16, &got, NULL)) { CloseHandle(h); return; }
    CloseHandle(h);
    dir[got] = 0;
    const BYTE* e = &dir[0]; const BYTE* end = e + got; bool any = false;
    for (DWORD i = 0; i < n && e + 9 <= end; ++i) { const char* nm = (const char*)e + 8; if (isTexName(nm)) { any = true; break; } e += 8 + strlen(nm) + 1; }
    if (!any) return;
    DWORD size = 0; const BYTE* m = mapFile(path, &size);
    if (!m) { printf("  (cannot map %s)\n", path); return; }
    const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
    e = &dir[0];
    for (DWORD i = 0; i < n && e + 9 <= end; ++i) {
        DWORD off = be32(e), sz = be32(e + 4); e += 8;
        const char* nm = (const char*)e; e += strlen(nm) + 1;
        if (off + sz <= size && isTexName(nm)) addFile(nm, base, m + off, sz);
    }
}
static void walk(const std::string& dir, const std::string& rel, bool bigs) {
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        std::string full = dir + "\\" + fd.cFileName, r = rel.empty() ? fd.cFileName : rel + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { walk(full, r, bigs); continue; }
        const char* dot = strrchr(fd.cFileName, '.'); if (!dot) continue;
        if (!_stricmp(dot, ".big")) { if (bigs) addBig(full.c_str()); }
        else if (!_stricmp(dot, ".dds") || !_stricmp(dot, ".tga")) { DWORD sz = 0; const BYTE* p = mapFile(full.c_str(), &sz); if (p && sz) addFile(r, "loose", p, sz); }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------- reading textures back
static unsigned long long fnv(unsigned long long h, const BYTE* p, DWORD n) { for (DWORD i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; } return h; }
struct Snap { DWORD fmt, w, h, levels; std::vector<std::vector<BYTE> > lv; unsigned long long hash; };
static bool snapReal(void* tex, Snap* s) {
    s->lv.clear(); s->hash = 1469598103934665603ull;
    s->levels = ((DWORD(WINAPI*)(void*))vt(tex)[13])(tex);
    for (DWORD i = 0; i < s->levels; ++i) {
        TxDESC d; if (FAILED(((HRESULT(WINAPI*)(void*, DWORD, TxDESC*))vt(tex)[17])(tex, i, &d))) return false;
        if (!i) { s->fmt = d.Format; s->w = d.Width; s->h = d.Height; }
        DWORD bytes, block, pitch, rows, rowBytes;
        if (!txFmtInfo(d.Format, &bytes, &block)) return false;
        txLevelGeom(bytes, block, d.Width, d.Height, &pitch, &rows, &rowBytes);
        TxLOCKED lr = { 0, NULL };
        if (FAILED(((HRESULT(WINAPI*)(void*, DWORD, TxLOCKED*, const TxRECT*, DWORD))vt(tex)[19])(tex, i, &lr, NULL, 0x10)) || !lr.bits) return false;
        std::vector<BYTE> v(rows * rowBytes);
        for (DWORD y = 0; y < rows; ++y) memcpy(&v[y * rowBytes], (const BYTE*)lr.bits + (INT)y * lr.pitch, rowBytes);
        ((HRESULT(WINAPI*)(void*, DWORD))vt(tex)[20])(tex, i);
        s->hash = fnv(s->hash, v.empty() ? NULL : &v[0], (DWORD)v.size());
        s->lv.push_back(v);
    }
    return true;
}
static void snapMem(TxMem* m, Snap* s) {
    s->lv.clear(); s->hash = 1469598103934665603ull; s->levels = m->levels; s->fmt = m->fmt; s->w = m->w; s->h = m->h;
    for (DWORD i = 0; i < m->levels; ++i) {
        const TxLevel& L = m->lv[i];
        std::vector<BYTE> v(L.rows * L.rowBytes);
        for (DWORD y = 0; y < L.rows; ++y) memcpy(&v[y * L.rowBytes], L.bits + y * L.pitch, L.rowBytes);
        s->hash = fnv(s->hash, v.empty() ? NULL : &v[0], (DWORD)v.size());
        s->lv.push_back(v);
    }
}
static bool same(const Snap& a, const Snap& b, int from, char* why) {
    if (a.fmt != b.fmt || a.w != b.w || a.h != b.h || a.levels != b.levels) { sprintf(why, "shape %ux%u fmt %08X x%u vs %ux%u fmt %08X x%u", a.w, a.h, a.fmt, a.levels, b.w, b.h, b.fmt, b.levels); return false; }
    for (DWORD i = (DWORD)from; i < a.levels; ++i) {
        if (a.lv[i].size() != b.lv[i].size()) { sprintf(why, "level %u size", i); return false; }
        if (!a.lv[i].empty() && memcmp(&a.lv[i][0], &b.lv[i][0], a.lv[i].size())) {
            size_t k = 0; while (a.lv[i][k] == b.lv[i][k]) ++k; size_t nd = 0; for (size_t q = 0; q < a.lv[i].size(); ++q) nd += a.lv[i][q] != b.lv[i][q];
            sprintf(why, "level %u: %u of %u bytes differ, first at %u", i, (unsigned)nd, (unsigned)a.lv[i].size(), (unsigned)k); return false; }
    }
    return true;
}
static void release(void* o) { if (o) ((ULONG(WINAPI*)(void*))vt(o)[2])(o); }

// ---------------------------------------------------------------- the engine's sequences
static void* g_dev;
// the engine's plain-TGA row copy (game.dat 0x005311D9): bottom-up rows, 24 bit pixels spread into 32 with the
// fourth byte LEFT AS IT WAS, 32 bit rows copied whole
static void engineTgaFill(const BYTE* file, BYTE* bits, INT pitch) {
    int w = *(const SHORT*)(file + 12), h = *(const SHORT*)(file + 14); const BYTE* src = file + 18;
    BYTE* row = bits + (h - 1) * pitch;
    if (file[16] == 24) for (int y = 0; y < h; ++y, row -= pitch) { BYTE* d = row; for (int x = 0; x < w; ++x, d += 4, src += 3) { d[0] = src[0]; d[1] = src[1]; d[2] = src[2]; } }
    else for (int y = 0; y < h; ++y, row -= pitch, src += w * 4) memcpy(row, src, w * 4);
}
struct Stat { const char* what; int n, same, diff, skipped; LONGLONG tReal, tMem, tProbe, tUp; double worstReal; std::vector<std::string> bad; std::map<std::string, std::pair<int, LONGLONG> > byShape; };
static void note(Stat& st, const File& f, bool ok, const char* why) { if (ok) st.same++; else { st.diff++; if (st.bad.size() < 12) st.bad.push_back(f.name + " (" + f.src + "): " + why); } }

// W2 on one file. `memOnly`: just the memory run (threads), result hash in *outHash
static bool runW2(const File& f, Stat* st, unsigned long long* outHash, bool memOnly, DWORD levelsArg = 0, DWORD fmtArg = 0, int shift = 0, DWORD mipFilter = 5) {
    IMGINFO info; memset(&info, 0, sizeof(info));
    if (FAILED(XImgInfo(f.p, f.n, &info)) || info.ResourceType != 3) { if (st) st->skipped++; return false; }
    if (shift) { if ((info.Width >> shift) < 32 || (info.Height >> shift) < 32) { if (st) st->skipped++; return false; } info.Width >>= shift; info.Height >>= shift; }
    char why[200] = "";
    void* real = NULL; Snap sr; LONGLONG t0 = now(), t1 = t0;
    if (!memOnly) {
        HRESULT hr = XCreateFromFile(g_dev, f.p, f.n, info.Width, info.Height, levelsArg, 0, fmtArg, 1, 0xFFFFFFFFu, mipFilter, 0, NULL, NULL, &real);
        t1 = now();
        if (FAILED(hr) || !real) {                         // the engine gets a failure: the probe must report one too (or we fall back - not an error here)
            TxDev pd; txDevInit(&pd, 1, NULL); void* dummy = NULL;
            XCreateFromFile(&pd, f.p, f.n, info.Width, info.Height, levelsArg, 0, fmtArg, 1, 0xFFFFFFFFu, mipFilter, 0, NULL, NULL, &dummy);
            if (st) { st->skipped++; if (pd.nCreate) { st->diff++; if (st->bad.size() < 12) st->bad.push_back(f.name + ": the real call FAILED but the probe reached CreateTexture"); } }
            return false;
        }
        if (!snapReal(real, &sr)) { release(real); if (st) st->skipped++; return false; }
    }
    // probe
    TxDev pd; txDevInit(&pd, 1, NULL); void* dummy = NULL;
    LONGLONG t2 = now();
    HRESULT hp = XCreateFromFile(&pd, f.p, f.n, info.Width, info.Height, levelsArg, 0, fmtArg, 1, 0xFFFFFFFFu, mipFilter, 0, NULL, NULL, &dummy);
    LONGLONG t3 = now();
    bool ok = true;
    if (pd.nCreate != 1 || SUCCEEDED(hp) || dummy) { ok = false; sprintf(why, "probe: %d CreateTexture calls, hr %08lX", (int)pd.nCreate, (unsigned long)hp); }
    TxMem* give = ok ? txMemCreate(pd.cw, pd.ch, pd.clevels, pd.cusage, pd.cfmt, pd.cpool) : NULL;
    if (ok && !give) { ok = false; sprintf(why, "no memory texture for %ux%u fmt %08X x%u", pd.cw, pd.ch, pd.cfmt, pd.clevels); }
    TxMem* m = NULL; LONGLONG t4 = now(), t5 = t4;
    if (ok) {
        TxDev jd; txDevInit(&jd, 0, give); void* out = NULL;
        HRESULT hm = XCreateFromFile(&jd, f.p, f.n, info.Width, info.Height, levelsArg, 0, fmtArg, 1, 0xFFFFFFFFu, mipFilter, 0, NULL, NULL, &out);
        t5 = now();
        m = (TxMem*)out;
        if (FAILED(hm) || !m) { ok = false; sprintf(why, "memory run failed hr %08lX", (unsigned long)hm); }
        else if (m != give || jd.nMismatch || jd.nCreate != 1) { ok = false; sprintf(why, "memory run made its own texture (%d creates, %d mismatches)", (int)jd.nCreate, (int)jd.nMismatch); }
        else if ((DWORD)m->written != (1u << m->levels) - 1) { ok = false; sprintf(why, "levels written %X of %u", (unsigned)m->written, m->levels); }
        if (m) m->dev = &g_txDevStatic;
    }
    Snap sm; if (m) snapMem(m, &sm);
    if (outHash) *outHash = m ? sm.hash : 0;
    if (!memOnly && ok && !same(sr, sm, 0, why)) ok = false;
    LONGLONG tu = 0;
    if (!memOnly && ok) {                                  // the upload: a second real texture made from the probe's arguments, filled from memory
        void* t2x = NULL;
        HRESULT hc = ((HRESULT(WINAPI*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, void*))vt(g_dev)[23])(g_dev, pd.cw, pd.ch, pd.clevels, pd.cusage, pd.cfmt, pd.cpool, &t2x, NULL);
        if (FAILED(hc) || !t2x) { ok = false; sprintf(why, "real CreateTexture with the probe's arguments failed %08lX", (unsigned long)hc); }
        else {
            LONGLONG u0 = now();
            bool up = txUpload(m, t2x, vt(t2x)[19], vt(t2x)[20], (1u << m->levels) - 1);
            tu = now() - u0;
            Snap s2; if (!up || !snapReal(t2x, &s2) || !same(sr, s2, 0, why)) { ok = false; if (!why[0]) strcpy(why, "upload"); else { char w2[220]; sprintf(w2, "after upload: %s", why); strcpy(why, w2); } }
            release(t2x);
        }
    }
    if (st && !memOnly) {
        st->n++; st->tReal += t1 - t0; st->tProbe += t3 - t2; st->tMem += t5 - t4; st->tUp += tu; if (ms(t1 - t0) > st->worstReal) st->worstReal = ms(t1 - t0);
        note(*st, f, ok, why);
        char shape[96]; char fcc[5] = { (char)sr.fmt, (char)(sr.fmt >> 8), (char)(sr.fmt >> 16), (char)(sr.fmt >> 24), 0 };
        sprintf(shape, "%s %s file %u mips -> %u", f.kind ? "tga" : "dds", sr.fmt > 0xFFFF ? fcc : (sr.fmt == 21 ? "A8R8G8B8" : sr.fmt == 22 ? "X8R8G8B8" : sr.fmt == 20 ? "R8G8B8" : "other"), info.MipLevels, sr.levels);
        std::pair<int, LONGLONG>& b = st->byShape[shape]; b.first++; b.second += t1 - t0;
    }
    if (m) tx_tex_Release(m);                              // the reference D3DX handed back
    if (give) tx_tex_Release(give);                        // and our own
    release(real);
    return ok;
}
// W1 on one plain TGA
static bool runW1(const File& f, Stat* st, unsigned long long* outHash, bool memOnly, const Snap* level0From = NULL) {
    UINT w = *(const WORD*)(f.p + 12), h = *(const WORD*)(f.p + 14); DWORD fmt = f.p[16] == 24 ? 22 : 21;
    if (f.n < 18 + (DWORD)w * h * (f.p[16] / 8) || !w || !h) { if (st) st->skipped++; return false; }
    char why[200] = ""; bool ok = true;
    UINT rw = w, rh = h; DWORD rfmt = fmt;
    if (FAILED(XCheckReq(g_dev, &rw, &rh, NULL, 0, &rfmt, 1)) || rw != w || rh != h || rfmt != fmt) { if (st) st->skipped++; return false; }   // the engine takes the general path then
    // the same question to the device stand-in
    { TxDev qd; txDevInit(&qd, 0, NULL); UINT qw = w, qh = h; DWORD qf = fmt;
      if (FAILED(XCheckReq(&qd, &qw, &qh, NULL, 0, &qf, 1)) || qw != rw || qh != rh || qf != rfmt) { ok = false; sprintf(why, "CheckTextureRequirements differs on the stand-in"); } }
    void* real = NULL; Snap sr; LONGLONG t0 = now(), t1 = t0, tf = 0;
    HRESULT hr = XCreateTexture(g_dev, w, h, 0, 0, fmt, 1, &real);
    if (FAILED(hr) || !real) { if (st) st->skipped++; return false; }
    TxDev pd; txDevInit(&pd, 1, NULL); void* dummy = NULL; LONGLONG p0 = now();
    XCreateTexture(&pd, w, h, 0, 0, fmt, 1, &dummy);
    LONGLONG p1 = now();
    TxDESC d0; ((HRESULT(WINAPI*)(void*, DWORD, TxDESC*))vt(real)[17])(real, 0, &d0);
    DWORD realLevels = ((DWORD(WINAPI*)(void*))vt(real)[13])(real);
    if (ok && (pd.nCreate != 1 || pd.cw != d0.Width || pd.ch != d0.Height || pd.cfmt != d0.Format || (pd.clevels ? pd.clevels : txFullLevels(pd.cw, pd.ch)) != realLevels)) { ok = false; sprintf(why, "D3DXCreateTexture probe: %ux%u fmt %u x%u, real %ux%u fmt %u x%u", pd.cw, pd.ch, pd.cfmt, pd.clevels, d0.Width, d0.Height, d0.Format, realLevels); }
    TxLOCKED lr = { 0, NULL };
    ((HRESULT(WINAPI*)(void*, DWORD, TxLOCKED*, const TxRECT*, DWORD))vt(real)[19])(real, 0, &lr, NULL, 0x800);
    engineTgaFill(f.p, (BYTE*)lr.bits, lr.pitch);
    ((HRESULT(WINAPI*)(void*, DWORD))vt(real)[20])(real, 0);
    // memory side first (it needs level 0 as the engine left it, before the reference filter touches anything)
    TxMem* m = ok ? txMemCreate(d0.Width, d0.Height, realLevels, 0, d0.Format, 1) : NULL;
    LONGLONG c0 = now();
    if (m && !txDownload(m, real, vt(real)[19], vt(real)[20], 0)) { ok = false; strcpy(why, "level 0 read"); }
    LONGLONG c1 = now(), m0 = c1, m1 = c1;
    if (m && ok) { HRESULT hm = XFilter(m, NULL, 0, 5); m1 = now(); if (FAILED(hm)) { ok = false; sprintf(why, "memory filter hr %08lX", (unsigned long)hm); }
                   else if (m->levels > 1 && ((DWORD)m->written & ~1u) != ((1u << m->levels) - 2)) { ok = false; sprintf(why, "levels written %X of %u", (unsigned)m->written, m->levels); } }
    if (!memOnly) { LONGLONG f0 = now(); HRESULT hf = XFilter(real, NULL, 0, 5); tf = now() - f0; t1 = now(); if (FAILED(hf)) { ok = false; sprintf(why, "real filter hr %08lX", (unsigned long)hf); } }
    Snap sm; if (m) snapMem(m, &sm);
    if (outHash) *outHash = m ? sm.hash : 0;
    if (!memOnly && ok) { if (!snapReal(real, &sr) || !same(sr, sm, 0, why)) ok = false; }
    if (st && !memOnly) { st->n++; st->tReal += tf; st->tMem += m1 - m0; st->tProbe += p1 - p0; st->tUp += c1 - c0; if (ms(tf) > st->worstReal) st->worstReal = ms(tf); note(*st, f, ok, why);
        UINT big = w > h ? w : h; char shape[64]; sprintf(shape, "tga %d bit, longer side %s", f.p[16], big > 1024 ? "over 1024" : big > 512 ? "513..1024" : big > 256 ? "257..512" : "up to 256");
        std::pair<int, LONGLONG>& b = st->byShape[shape]; b.first++; b.second += tf; }
    (void)t0; (void)t1; (void)level0From;
    if (m) tx_tex_Release(m);
    release(real);
    return ok;
}
// W1 with no device at all (threads): the row copy goes straight into the memory texture
static bool runW1Mem(const File& f, unsigned long long* outHash) {
    UINT w = *(const WORD*)(f.p + 12), h = *(const WORD*)(f.p + 14); DWORD fmt = f.p[16] == 24 ? 22 : 21;
    TxMem* m = txMemCreate(w, h, 0, 0, fmt, 1);
    if (!m) { *outHash = 0; return false; }
    engineTgaFill(f.p, m->lv[0].bits, (INT)m->lv[0].pitch);
    HRESULT hm = XFilter(m, NULL, 0, 5);
    Snap sm; snapMem(m, &sm); *outHash = sm.hash;
    tx_tex_Release(m);
    return SUCCEEDED(hm);
}
// T1: pixels of a plain TGA as the terrain code hands them over (X8R8G8B8 rows), into a texture of format `dfmt`
static bool runT1(const File& f, DWORD dfmt, Stat* st, unsigned long long* outHash, bool memOnly) {
    UINT w = *(const WORD*)(f.p + 12), h = *(const WORD*)(f.p + 14);
    if (w != h || (w & (w - 1)) || w < 64 || w > 1024 || f.p[16] != 32 || f.n < 18 + w * h * 4) return false;
    char why[200] = ""; bool ok = true;
    const BYTE* src = f.p + 18; TxRECT rc = { 0, 0, (LONG)w, (LONG)h };
    void* real = NULL; Snap sr; LONGLONG tl = 0, tf = 0;
    if (!memOnly) {
        if (FAILED(XCreateTexture(g_dev, w, h, 0, 0, dfmt, 1, &real)) || !real) { if (st) st->skipped++; return false; }
        TxDESC d0; ((HRESULT(WINAPI*)(void*, DWORD, TxDESC*))vt(real)[17])(real, 0, &d0);
        if (d0.Format != dfmt || d0.Width != w) { release(real); if (st) st->skipped++; return false; }
        void* surf = NULL; ((HRESULT(WINAPI*)(void*, DWORD, void**))vt(real)[18])(real, 0, &surf);
        LONGLONG a = now(); HRESULT h1 = XLoadSurfMem(surf, NULL, NULL, src, 22, w * 4, NULL, &rc, 1, 0); LONGLONG b = now();
        release(surf);
        HRESULT h2 = XFilter(real, NULL, 0, 5); LONGLONG c = now();
        tl = b - a; tf = c - b;
        if (FAILED(h1) || FAILED(h2) || !snapReal(real, &sr)) { release(real); if (st) st->skipped++; return false; }
    }
    TxMem* m = txMemCreate(w, h, 0, 0, dfmt, 1);
    LONGLONG m0 = now(), m1 = m0;
    if (!m) { ok = false; strcpy(why, "no memory texture"); }
    else {
        void* ms0 = NULL; tx_tex_GetSurfaceLevel(m, 0, &ms0);
        HRESULT h1 = XLoadSurfMem(ms0, NULL, NULL, src, 22, w * 4, NULL, &rc, 1, 0);
        tx_surf_Release((TxSurf*)ms0);
        HRESULT h2 = XFilter(m, NULL, 0, 5); m1 = now();
        if (FAILED(h1) || FAILED(h2)) { ok = false; sprintf(why, "memory run hr %08lX %08lX", (unsigned long)h1, (unsigned long)h2); }
        else if ((DWORD)m->written != (1u << m->levels) - 1) { ok = false; sprintf(why, "levels written %X of %u", (unsigned)m->written, m->levels); }
    }
    Snap sm; if (m) snapMem(m, &sm);
    if (outHash) *outHash = m ? sm.hash : 0;
    if (!memOnly && ok && !same(sr, sm, 0, why)) ok = false;
    if (st && !memOnly) { st->n++; st->tReal += tl + tf; st->tMem += m1 - m0; st->tProbe += tl; if (ms(tl + tf) > st->worstReal) st->worstReal = ms(tl + tf); note(*st, f, ok, why);
        char fcc[5] = { (char)dfmt, (char)(dfmt >> 8), (char)(dfmt >> 16), (char)(dfmt >> 24), 0 }; char shape[64];
        sprintf(shape, "-> %s %ux%u", dfmt > 0xFFFF ? fcc : (dfmt == 21 ? "A8R8G8B8" : dfmt == 22 ? "X8R8G8B8" : dfmt == 23 ? "R5G6B5" : dfmt == 25 ? "A1R5G5B5" : "other"), w, h);
        std::pair<int, LONGLONG>& b = st->byShape[shape]; b.first++; b.second += tl + tf; }
    if (m) tx_tex_Release(m);
    release(real);
    return ok;
}
static void report(const Stat& st) {
    printf("\n%s: %d textures, %d identical, %d DIFFERENT, %d not applicable\n", st.what, st.n, st.same, st.diff, st.skipped);
    if (st.n) printf("   on the real texture %.1f ms in all, %.2f ms each (worst %.1f) | on memory %.2f ms each | probe %.3f ms each | copy %.3f ms each\n",
                     ms(st.tReal), ms(st.tReal) / st.n, st.worstReal, ms(st.tMem) / st.n, ms(st.tProbe) / st.n, ms(st.tUp) / st.n);
    for (std::map<std::string, std::pair<int, LONGLONG> >::const_iterator it = st.byShape.begin(); it != st.byShape.end(); ++it)
        printf("   %-44s %5d  %8.2f ms each  %9.1f ms\n", it->first.c_str(), it->second.first, ms(it->second.second) / it->second.first, ms(it->second.second));
    for (size_t i = 0; i < st.bad.size(); ++i) printf("   DIFFERENT: %s\n", st.bad[i].c_str());
}

// ---------------------------------------------------------------- threads
struct Work { int kind; const File* f; DWORD dfmt; unsigned long long serial, threaded; int shift; DWORD mipFilter; };
static std::vector<Work> g_work; static volatile LONG g_next = 0; static unsigned int g_cw = 0;
static DWORD WINAPI worker(LPVOID) {
    _control87(g_cw, _MCW_PC | _MCW_RC);
    for (;;) {
        LONG i = InterlockedIncrement(&g_next) - 1;
        if (i >= (LONG)g_work.size()) return 0;
        Work& w = g_work[i]; unsigned long long hsh = 0;
        if (w.kind == 0) runW2(*w.f, NULL, &hsh, true, 0, 0, w.shift, w.mipFilter); else if (w.kind == 1) runW1Mem(*w.f, &hsh); else runT1(*w.f, w.dfmt, NULL, &hsh, true);
        w.threaded = hsh;
    }
}

static void memReport(const char* tag) {
    MEMORY_BASIC_INFORMATION mbi; BYTE* p = NULL; SIZE_T freeAll = 0, freeMax = 0, priv = 0, mapped = 0, image = 0, reserved = 0;
    while (VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_FREE) { freeAll += mbi.RegionSize; if (mbi.RegionSize > freeMax) freeMax = mbi.RegionSize; }
        else if (mbi.State == MEM_RESERVE) reserved += mbi.RegionSize;
        else if (mbi.Type == MEM_PRIVATE) priv += mbi.RegionSize; else if (mbi.Type == MEM_MAPPED) mapped += mbi.RegionSize; else image += mbi.RegionSize;
        BYTE* next = (BYTE*)mbi.BaseAddress + mbi.RegionSize; if (next <= p) break; p = next;
    }
    printf("   memory %s: free %u MB (largest block %u MB), private %u MB, mapped files %u MB, reserved %u MB, images %u MB\n", tag,
           (unsigned)(freeAll >> 20), (unsigned)(freeMax >> 20), (unsigned)(priv >> 20), (unsigned)(mapped >> 20), (unsigned)(reserved >> 20), (unsigned)(image >> 20));
}
// ---------------------------------------------------------------- the same through the DLL
struct Ref { const File* f; int seq; DWORD dfmt; unsigned long long hash; DWORD fmt, w, h, levels; void* tex; };   // seq 0 W2, 1 W1, 2 T1, 3 T1 twice on level 0
static unsigned long long hashOf(void* tex, Ref* r) { Snap s; if (!snapReal(tex, &s)) return 0; if (r) { r->fmt = s.fmt; r->w = s.w; r->h = s.h; r->levels = s.levels; } return s.hash; }
typedef HRESULT (WINAPI* LoadSurfFileF)(void*, const void*, const TxRECT*, const void*, UINT, const TxRECT*, DWORD, DWORD, void*);
static LoadSurfFileF XLoadSurfFile;
struct Fns { CreateFromFileF file; CreateTexF create; CheckReqF req; LoadSurfMemF load; FilterF filter; LoadSurfFileF loadFile; };
// one sequence with the given D3DX entry points; the texture is returned unread
static void* doSeq(const Fns& X, const Ref& r) {
    const File& f = *r.f; void* tex = NULL;
    if (r.seq == 0) {
        IMGINFO info; memset(&info, 0, sizeof(info));
        if (FAILED(XImgInfo(f.p, f.n, &info)) || info.ResourceType != 3) return NULL;
        if (FAILED(X.file(g_dev, f.p, f.n, info.Width, info.Height, 0, 0, 0, 1, 0xFFFFFFFFu, 5, 0, NULL, NULL, &tex))) return NULL;
        return tex;
    }
    if (r.seq == 4) {                                       // the engine's path for a colour file with its alpha elsewhere: an A8R8G8B8 texture, the file into its level 0, the chain
        IMGINFO info; memset(&info, 0, sizeof(info));
        if (FAILED(XImgInfo(f.p, f.n, &info)) || info.ResourceType != 3) return NULL;
        if (FAILED(X.create(g_dev, info.Width, info.Height, 0, 0, 21, 1, &tex)) || !tex) return NULL;
        void* surf = NULL; ((HRESULT(WINAPI*)(void*, DWORD, void**))vt(tex)[18])(tex, 0, &surf);
        HRESULT hl = surf ? X.loadFile(surf, NULL, NULL, f.p, f.n, NULL, 0xFFFFFFFFu, 0, NULL) : E_FAIL;
        release(surf);
        if (FAILED(hl)) { release(tex); return NULL; }
        X.filter(tex, NULL, 0, 5);
        return tex;
    }
    UINT w = *(const WORD*)(f.p + 12), h = *(const WORD*)(f.p + 14);
    if (r.seq == 1) {
        DWORD fmt = f.p[16] == 24 ? 22 : 21; UINT rw = w, rh = h; DWORD rfmt = fmt;
        if (f.n < 18 + (DWORD)w * h * (f.p[16] / 8) || !w || !h) return NULL;
        if (FAILED(X.req(g_dev, &rw, &rh, NULL, 0, &rfmt, 1)) || rw != w || rh != h || rfmt != fmt) return NULL;
        if (FAILED(X.create(g_dev, w, h, 0, 0, fmt, 1, &tex)) || !tex) return NULL;
        TxLOCKED lr = { 0, NULL };
        ((HRESULT(WINAPI*)(void*, DWORD, TxLOCKED*, const TxRECT*, DWORD))vt(tex)[19])(tex, 0, &lr, NULL, 0x800);
        if (lr.bits) { if (f.p[16] == 24) for (UINT y = 0; y < h; ++y) memset((BYTE*)lr.bits + (INT)y * lr.pitch, 0, w * 4);     // the byte the engine leaves alone: defined here, so two runs can be compared
                       engineTgaFill(f.p, (BYTE*)lr.bits, lr.pitch); }
        ((HRESULT(WINAPI*)(void*, DWORD))vt(tex)[20])(tex, 0);
        X.filter(tex, NULL, 0, 5);
        return tex;
    }
    if (FAILED(X.create(g_dev, w, h, 0, 0, r.dfmt, 1, &tex)) || !tex) return NULL;
    TxRECT rc = { 0, 0, (LONG)w, (LONG)h };
    for (int pass = (r.seq == 3 ? 0 : 1); pass < 2; ++pass) {
        void* surf = NULL; ((HRESULT(WINAPI*)(void*, DWORD, void**))vt(tex)[18])(tex, 0, &surf);
        std::vector<BYTE> other;
        const BYTE* src = f.p + 18;
        if (pass == 0) { other.assign(src, src + w * h * 4); for (size_t i = 0; i < other.size(); ++i) other[i] = (BYTE)(255 - other[i]); src = &other[0]; }   // first another picture, then the real one
        X.load(surf, NULL, NULL, src, 22, w * 4, NULL, &rc, 1, 0);
        release(surf);
    }
    X.filter(tex, NULL, 0, 5);
    return tex;
}
static int runDll(int stride, DWORD maxBytes) {
    Fns raw = { XCreateFromFile, XCreateTexture, XCheckReq, XLoadSurfMem, XFilter, XLoadSurfFile };
    std::vector<Ref> refs; int seen[3] = { 0, 0, 0 }, nT1 = 0, nF4 = 0, nDds = 0;
    for (size_t i = 0; i < g_files.size(); ++i) {
        const File& f = g_files[i];
        if (f.n > maxBytes || (seen[f.kind]++ % stride)) continue;
        Ref r; memset(&r, 0, sizeof(r)); r.f = &f;
        if (f.kind == 1) {
            r.seq = 1; refs.push_back(r);
            UINT w = *(const WORD*)(f.p + 12), h = *(const WORD*)(f.p + 14);
            if (w == h && !(w & (w - 1)) && w >= 64 && w <= 512 && f.p[16] == 32 && f.n >= 18 + w * h * 4 && nT1 < 600) {
                static const DWORD kF[4] = { 0x31545844u, 23, 0x35545844u, 25 };
                r.seq = 2; r.dfmt = kF[nT1 & 3]; refs.push_back(r); ++nT1;
                if ((nT1 & 7) == 0) { r.seq = 3; refs.push_back(r); }
            }
        } else { r.seq = 0; refs.push_back(r);
                 if (f.kind == 0 && (nDds++ % 9) == 4 && nF4 < 500 && f.n <= (6u << 20)) { r.seq = 4; refs.push_back(r); ++nF4; } }
    }
    static const char kFx[] = "float4 c; technique T { pass P { ZEnable = TRUE; } }";
    typedef HRESULT (WINAPI* CreateEffectF)(void*, const char*, UINT, void*, void*, DWORD, void*, void**, void**);
    CreateEffectF XCreateEffect = (CreateEffectF)GetProcAddress(GetModuleHandleA("d3dx9_27.dll"), "D3DXCreateEffect");
    void* fx = NULL, *err = NULL;
    HRESULT hfx = XCreateEffect(g_dev, kFx, sizeof(kFx) - 1, NULL, NULL, 0, NULL, &fx, &err);
    if (!fx) { printf("no effect: hr %08lX %s\n", (unsigned long)hfx, err ? (char*)((void*(WINAPI*)(void*))vt(err)[3])(err) : ""); return 4; }
    // 1. what plain D3DX leaves in real textures
    memReport("before the reference pass");
    LONGLONG t0 = now(); size_t nRef = 0;
    for (size_t i = 0; i < refs.size(); ++i) { void* t = doSeq(raw, refs[i]); if (!t) continue; refs[i].hash = hashOf(t, &refs[i]); release(t); nRef++; }
    LONGLONG tRef = now() - t0;
    // one-level texture: what does the filter say?
    void* one = NULL; XCreateTexture(g_dev, 64, 64, 1, 0, 21, 1, &one); HRESULT hrOneRef = XFilter(one, NULL, 0, 5); release(one);
    printf("reference: %u sequences with plain D3DX in %.1f ms (%.2f ms each)\n", (unsigned)nRef, ms(tRef), ms(tRef) / (nRef ? nRef : 1));

    // 2. the DLL, its render thread, its texture workers
    HMODULE ha = LoadLibraryA("bfme2_accel.new.dll"); if (!ha) { printf("no bfme2_accel.new.dll (%lu)\n", GetLastError()); return 3; }
    typedef int (__cdecl* InstallF)(void*, void*); typedef void (__cdecl* StatsF)(char*, int); typedef void* (__cdecl* ScopedF)(const char*);
    InstallF rtInstall = (InstallF)GetProcAddress(ha, "AotrRtTestInstall"); ScopedF scoped = (ScopedF)GetProcAddress(ha, "AotrRtTestScopedD3DX");
    StatsF texStats = (StatsF)GetProcAddress(ha, "AotrTexTestStats"), rtStats = (StatsF)GetProcAddress(ha, "AotrRtTestStats");
    typedef void (__cdecl* SetupF)(int, int, int); SetupF texSetup = (SetupF)GetProcAddress(ha, "AotrTexTestSetup");
    typedef void (__cdecl* CacheSetupF)(int, int, const char*); CacheSetupF cacheSetup = (CacheSetupF)GetProcAddress(ha, "AotrTexCacheTestSetup");
    StatsF cacheStats = (StatsF)GetProcAddress(ha, "AotrTexCacheTestStats");
    if (!rtInstall || !scoped || !texStats || !texSetup || !cacheSetup || !cacheStats) { printf("missing test exports\n"); return 3; }
    { char dir[MAX_PATH] = ""; GetEnvironmentVariableA("TXC_DIR", dir, sizeof(dir));          // the disk cache: a folder given -> on (run twice: the second run reads what the first kept)
      if (dir[0]) { cacheSetup(1, 20000, dir); printf("disk cache in %s\n", dir); } else printf("disk cache off (TXC_DIR not set)\n"); }
    Sleep(1500);
    { char e1[32] = "", e2[32] = "", e3[32] = ""; GetEnvironmentVariableA("AOTR_TEXTHREADS", e1, 32); GetEnvironmentVariableA("AOTR_TEXCHECK", e2, 32); GetEnvironmentVariableA("AOTR_TEXWORKMB", e3, 32);
      texSetup(e1[0] ? atoi(e1) : -1, e2[0] ? atoi(e2) : 0, e3[0] ? atoi(e3) : 160); }
    memReport("after the reference pass");
    { struct { DWORD cw, sw, tag, ip, cs, dp, ds; } env; __asm { fnstenv env } __asm { fldenv env }
      void* fx2 = NULL, *e2 = NULL; HRESULT h2 = XCreateEffect(g_dev, kFx, sizeof(kFx) - 1, NULL, NULL, 0, NULL, &fx2, &e2);
      if (fx2) release(fx2);
      else {
          printf("   (a second effect after the reference pass: hr %08lX %s; x87 tag word %04X, status %04X, control %04X)\n", (unsigned long)h2, e2 ? (char*)((void*(WINAPI*)(void*))vt(e2)[3])(e2) : "", env.tag & 0xFFFF, env.sw & 0xFFFF, env.cw & 0xFFFF);
          __asm { emms }
          fx2 = NULL; h2 = XCreateEffect(g_dev, kFx, sizeof(kFx) - 1, NULL, NULL, 0, NULL, &fx2, NULL);
          printf("   (after emms: hr %08lX)\n", (unsigned long)h2); if (fx2) release(fx2);
          DWORD avail = ((DWORD(WINAPI*)(void*))vt(g_dev)[4])(g_dev); void* tt = NULL;
          HRESULT h3 = ((HRESULT(WINAPI*)(void*, UINT, UINT, UINT, DWORD, DWORD, DWORD, void**, void*))vt(g_dev)[23])(g_dev, 64, 64, 1, 0, 21, 1, &tt, NULL); if (tt) release(tt);
          void* sb = NULL; HRESULT h4 = ((HRESULT(WINAPI*)(void*, DWORD, void**))vt(g_dev)[59])(g_dev, 1, &sb); if (sb) release(sb);
          printf("   (device: available texture memory %u MB, CreateTexture %08lX, CreateStateBlock %08lX)\n", (unsigned)(avail >> 20), (unsigned long)h3, (unsigned long)h4);
      } }
    if (!rtInstall(g_dev, fx)) { printf("the render thread did not start\n"); return 4; }
    Fns dll = { (CreateFromFileF)scoped("D3DXCreateTextureFromFileInMemoryEx"), (CreateTexF)scoped("D3DXCreateTexture"), (CheckReqF)scoped("D3DXCheckTextureRequirements"),
                (LoadSurfMemF)scoped("D3DXLoadSurfaceFromMemory"), (FilterF)scoped("D3DXFilterTexture"), (LoadSurfFileF)scoped("D3DXLoadSurfaceFromFileInMemory") };
    if (!dll.file || !dll.create || !dll.req || !dll.load || !dll.filter || !dll.loadFile) { printf("missing wrappers\n"); return 4; }

    // 3. the same sequences through the wrappers, in batches; then every texture read back through the hooked methods
    int same_ = 0, diff = 0, bad = 0; LONGLONG tSubmit = 0, tDrain = 0; size_t nDone = 0;
    const size_t B = 192;
    LONGLONG tAll = now();
    for (size_t at = 0; at < refs.size(); at += B) {
        size_t end = at + B < refs.size() ? at + B : refs.size();
        LONGLONG a = now();
        for (size_t i = at; i < end; ++i) refs[i].tex = refs[i].hash ? doSeq(dll, refs[i]) : NULL;
        LONGLONG b = now();
        ((DWORD(WINAPI*)(void*))vt(g_dev)[4])(g_dev);                 // GetAvailableTextureMem: waits for everything queued
        LONGLONG c = now();
        tSubmit += b - a; tDrain += c - b;
        for (size_t i = at; i < end; ++i) {
            Ref& r = refs[i]; if (!r.hash) continue;
            if (!r.tex) { bad++; if (bad < 6) printf("   NO TEXTURE through the DLL: %s\n", r.f->name.c_str()); continue; }
            Ref got; memset(&got, 0, sizeof(got)); unsigned long long hsh = hashOf(r.tex, &got);
            if (hsh == r.hash && got.fmt == r.fmt && got.w == r.w && got.levels == r.levels) same_++;
            else { if (diff++ < 10) printf("   DIFFERENT through the DLL: %s seq %d (%ux%u fmt %08X x%u, reference fmt %08X x%u)\n", r.f->name.c_str(), r.seq, got.w, got.h, got.fmt, got.levels, r.fmt, r.levels); }
            release(r.tex); r.tex = NULL; nDone++;
        }
    }
    LONGLONG tTotal = now() - tAll;
    printf("through the DLL: %u sequences, %d identical, %d DIFFERENT, %d without a texture\n", (unsigned)nDone, same_, diff, bad);
    printf("   the calling thread spent %.1f ms inside the calls (%.3f ms each; plain D3DX %.2f ms each), then waited %.1f ms for the queue; all of it %.1f ms against %.1f ms\n",
           ms(tSubmit), ms(tSubmit) / (nDone ? nDone : 1), ms(tRef) / (nRef ? nRef : 1), ms(tDrain), ms(tSubmit + tDrain), ms(tRef));
    (void)tTotal;

    // 4. corners
    int cornerBad = 0;
    { void* t1 = NULL; dll.create(g_dev, 64, 64, 1, 0, 21, 1, &t1); HRESULT h1 = t1 ? dll.filter(t1, NULL, 0, 5) : E_FAIL; release(t1);
      if (h1 != hrOneRef) { cornerBad++; printf("   CORNER: filter of a one-level texture returns %08lX, plain D3DX %08lX\n", (unsigned long)h1, (unsigned long)hrOneRef); } }
    // read at once, one level only, right behind the call
    { int n = 0, okc = 0;
      for (size_t i = 0; i < refs.size() && n < 400; i += refs.size() / 400 + 1) {
          Ref& r = refs[i]; if (!r.hash) continue;
          void* t = doSeq(dll, r); if (!t) continue; ++n;
          Ref got; memset(&got, 0, sizeof(got)); if (hashOf(t, &got) == r.hash) okc++; release(t);
      }
      if (okc != n) { cornerBad++; printf("   CORNER: read straight after the call: %d of %d identical\n", okc, n); } else printf("   read straight after the call: %d of %d identical\n", okc, n); }
    // released straight after the call, never read
    { int n = 0; for (size_t i = 0; i < refs.size() && n < 60; i += refs.size() / 60 + 1) { if (!refs[i].hash) continue; void* t = doSeq(dll, refs[i]); if (t) { release(t); ++n; } }
      ((DWORD(WINAPI*)(void*))vt(g_dev)[4])(g_dev); printf("   %d textures released straight after the call: no fault\n", n); }
    // a device reset with work in flight, then more
    { std::vector<void*> keep; std::vector<size_t> idx;
      for (size_t i = 0; i < refs.size() && keep.size() < 40; i += refs.size() / 40 + 1) { if (!refs[i].hash) continue; void* t = doSeq(dll, refs[i]); if (t) { keep.push_back(t); idx.push_back(i); } }
      PP pp = {0}; pp.BackBufferWidth = 640; pp.BackBufferHeight = 480; pp.BackBufferFormat = 21; pp.BackBufferCount = 1; pp.SwapEffect = 1; pp.hDeviceWindow = GetActiveWindow(); pp.Windowed = TRUE;
      pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = 75; pp.PresentationInterval = 0x80000000;
      extern HWND g_hwnd; pp.hDeviceWindow = g_hwnd;
      HRESULT hrR = ((HRESULT(WINAPI*)(void*, PP*))vt(g_dev)[16])(g_dev, &pp);
      int okc = 0; for (size_t k = 0; k < keep.size(); ++k) { Ref got; memset(&got, 0, sizeof(got)); if (hashOf(keep[k], &got) == refs[idx[k]].hash) okc++; release(keep[k]); }
      int n2 = 0, ok2 = 0;
      for (size_t i = 1; i < refs.size() && n2 < 40; i += refs.size() / 40 + 1) { if (!refs[i].hash) continue; void* t = doSeq(dll, refs[i]); if (!t) continue; ++n2; Ref got; memset(&got, 0, sizeof(got)); if (hashOf(t, &got) == refs[i].hash) ok2++; release(t); }
      if (FAILED(hrR) || okc != (int)keep.size() || ok2 != n2) { cornerBad++; printf("   CORNER: device reset %08lX with work in flight: %d of %u identical, then %d of %d\n", (unsigned long)hrR, okc, (unsigned)keep.size(), ok2, n2); }
      else printf("   device reset with work in flight: %d of %u identical, %d of %d after it\n", okc, (unsigned)keep.size(), ok2, n2); }
    // the engine's terrain build (game.dat 0x004AE3D2 and the patch geometry after it): per patch one 256x256 texture
    // made from X8R8G8B8 pixels into DXT1 with its mip chain, then a new vertex buffer locked with no flags and filled.
    // Before v52 that lock waited for the whole render queue - so for the texture just handed to a worker - every time.
    { const int N = 600; static DWORD px[256 * 256]; std::vector<void*> tex(N), vbs(N);
      for (int k = 0; k < 256 * 256; ++k) px[k] = 0xFF000000u | (DWORD)((k * 7) & 0xFF) << 16 | (DWORD)(((k >> 8) * 3) & 0xFF) << 8 | (DWORD)((k * 13 + (k >> 5)) & 0xFF);
      ((DWORD(WINAPI*)(void*))vt(g_dev)[4])(g_dev);
      LONGLONG t0 = now();
      for (int i = 0; i < N; ++i) {
          px[i] ^= 0x00FFFFFFu;                                                              // every patch its own picture
          tex[i] = NULL; vbs[i] = NULL;
          if (FAILED(dll.create(g_dev, 256, 256, 0, 0, 0x31545844u, 1, &tex[i])) || !tex[i]) continue;
          void* lvl = NULL; TxRECT rc = { 0, 0, 256, 256 };
          if (SUCCEEDED(((HRESULT(WINAPI*)(void*, DWORD, void**))vt(tex[i])[18])(tex[i], 0, &lvl)) && lvl) { dll.load(lvl, NULL, NULL, px, 22, 256 * 4, NULL, &rc, 1, 0); release(lvl); }
          dll.filter(tex[i], NULL, 0, 5);
          if (SUCCEEDED(((HRESULT(WINAPI*)(void*, UINT, DWORD, DWORD, DWORD, void**, void*))vt(g_dev)[26])(g_dev, 4096, 0, 0x152, 1, &vbs[i], NULL)) && vbs[i]) {
              void* pv = NULL;
              if (SUCCEEDED(((HRESULT(WINAPI*)(void*, UINT, UINT, void**, DWORD))vt(vbs[i])[11])(vbs[i], 0, 0, &pv, 0)) && pv) { memset(pv, i & 0xFF, 4096); ((HRESULT(WINAPI*)(void*))vt(vbs[i])[12])(vbs[i]); }
          }
      }
      LONGLONG t1 = now();
      ((DWORD(WINAPI*)(void*))vt(g_dev)[4])(g_dev);
      LONGLONG t2 = now();
      int vbBad = 0;                                                                        // what was written into the buffers is there
      for (int i = 0; i < N; ++i) { if (!vbs[i]) continue; void* pv = NULL;
          if (SUCCEEDED(((HRESULT(WINAPI*)(void*, UINT, UINT, void**, DWORD))vt(vbs[i])[11])(vbs[i], 0, 0, &pv, 0x10)) && pv) { const BYTE* b = (const BYTE*)pv; if (b[0] != (BYTE)i || b[4095] != (BYTE)i) vbBad++; ((HRESULT(WINAPI*)(void*))vt(vbs[i])[12])(vbs[i]); } else vbBad++; }
      for (int i = 0; i < N; ++i) { release(tex[i]); release(vbs[i]); }
      printf("   terrain pattern, %d patches (texture into DXT1 + mip chain, then a new vertex buffer filled): the calling thread took %.1f ms, the queue was empty %.1f ms later; %d buffers hold the wrong bytes\n",
             N, ms(t1 - t0), ms(t2 - t1), vbBad);
      if (vbBad) cornerBad++; }
    ((DWORD(WINAPI*)(void*))vt(g_dev)[4])(g_dev);
    Sleep(300);
    char st[3000]; texStats(st, sizeof(st)); printf("\n%s\n", st);
    { char cs[1000]; cacheStats(cs, sizeof(cs)); printf("%s\n", cs); if (strstr(cs, " 0 files failed their check") == NULL) { printf("   CACHE FILES FAILED THEIR CHECK\n"); cornerBad++; } }
    bool chkDiff = strstr(st, ", 0 DIFFERENT") == NULL, failed = strstr(st, "steps failed 0,") == NULL, flight = strstr(st, "in flight now 0 KB") == NULL;
    if (rtStats) { rtStats(st, sizeof(st)); printf("%.600s\n", st); }
    int total = diff + bad + cornerBad + (chkDiff ? 1 : 0) + (failed ? 1 : 0) + (flight ? 1 : 0);
    printf("\n%s\n", total ? "FAILED" : "PASSED");
    return total ? 1 : 0;
}
HWND g_hwnd;

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    bool dllMode = argc > 1 && !strcmp(argv[1], "dll");
    if (dllMode) { argc--; argv++; }
    int stride = argc > 1 ? atoi(argv[1]) : 12; if (stride < 1) stride = 1;
    DWORD maxBytes = (argc > 2 ? (DWORD)atoi(argv[2]) : 24) << 20;
    int nThreads = argc > 3 ? atoi(argv[3]) : 8;
    QueryPerformanceFrequency(&g_qpf);
    SetEnvironmentVariableA("DXVK_CONFIG", "dxvk.hud="); SetEnvironmentVariableA("DXVK_LOG_LEVEL", "none"); SetEnvironmentVariableA("DXVK_STATE_CACHE", "0");
    HMODULE hx = LoadLibraryA("d3dx9_27.dll"), hd = LoadLibraryA("d3d9.dll");
    if (!hx || !hd) { printf("d3dx9_27.dll / d3d9.dll missing\n"); return 3; }
    XCreateFromFile = (CreateFromFileF)GetProcAddress(hx, "D3DXCreateTextureFromFileInMemoryEx"); XCreateTexture = (CreateTexF)GetProcAddress(hx, "D3DXCreateTexture");
    XCheckReq = (CheckReqF)GetProcAddress(hx, "D3DXCheckTextureRequirements"); XLoadSurfMem = (LoadSurfMemF)GetProcAddress(hx, "D3DXLoadSurfaceFromMemory");
    XFilter = (FilterF)GetProcAddress(hx, "D3DXFilterTexture"); XImgInfo = (ImgInfoF)GetProcAddress(hx, "D3DXGetImageInfoFromFileInMemory");
    XLoadSurfFile = (LoadSurfFileF)GetProcAddress(hx, "D3DXLoadSurfaceFromFileInMemory");
    WNDCLASSA wc = {0}; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "texjobtest"; RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, "texjobtest", "t", WS_POPUP, -3000, -3000, 640, 480, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    g_hwnd = hwnd;
    void* d3d = ((void*(WINAPI*)(UINT))GetProcAddress(hd, "Direct3DCreate9"))(32);
    PP pp = {0}; pp.BackBufferWidth = 640; pp.BackBufferHeight = 480; pp.BackBufferFormat = 21; pp.BackBufferCount = 1; pp.SwapEffect = 1; pp.hDeviceWindow = hwnd; pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = 75; pp.PresentationInterval = 0x80000000;
    bool noFpu = argc > 4 && !strcmp(argv[4], "nofpu");
    HRESULT hr = ((HRESULT(WINAPI*)(void*, UINT, DWORD, HWND, DWORD, PP*, void**))vt(d3d)[16])(d3d, 0, 1, hwnd, noFpu ? 0x40u : (0x40u | 0x2u), &pp, &g_dev);
    if (FAILED(hr) || !g_dev) { printf("CreateDevice hr=0x%08lX\n", (unsigned long)hr); return 4; }
    g_cw = _control87(0, 0);
    printf("device created %s; this thread's x87 control word: precision %s\n", noFpu ? "WITHOUT FPU_PRESERVE" : "with FPU_PRESERVE", (g_cw & _MCW_PC) == _PC_24 ? "24 bit" : (g_cw & _MCW_PC) == _PC_53 ? "53 bit" : "64 bit");
    if (!txMemInitOnce() || !txInfoFill(g_dev, vt(g_dev))) { printf("memory textures: init failed\n"); return 5; }

    walk("C:\\AgeoftheRing\\rotwk", "", true); walk("C:\\AgeoftheRing\\aotr", "", true);
    int nk[3] = { 0, 0, 0 }; for (size_t i = 0; i < g_files.size(); ++i) nk[g_files[i].kind]++;
    printf("%u texture files: %d .dds, %d plain .tga, %d other .tga; taking every %d%s of each kind, up to %u MB a file\n", (unsigned)g_files.size(), nk[0], nk[1], nk[2], stride, stride == 1 ? "st" : "th", (unsigned)(maxBytes >> 20));
    if (dllMode) return runDll(stride, maxBytes);

    Stat w2c = { "W2  half size asked for (D3DX resizes: triangle filter with dither)" }, w2d = { "W2  half size by skipping the file's first mip level" }; int nRepeat = 0;
    Stat w2 = { "W2  whole file (D3DXCreateTextureFromFileInMemoryEx)" }, w1 = { "W1  plain TGA: engine copy + D3DXFilterTexture" }, t1 = { "T1  D3DXLoadSurfaceFromMemory + D3DXFilterTexture" }, w2b = { "W2  same, one mip level asked for" };
    int seen[3] = { 0, 0, 0 }; LONGLONG tStart = now();
    static const DWORD kT1Fmt[] = { 0x31545844u, 0x33545844u, 0x35545844u, 23, 25, 22, 21 };
    int nT1 = 0;
    for (size_t i = 0; i < g_files.size(); ++i) {
        const File& f = g_files[i];
        if (f.n > maxBytes || (seen[f.kind]++ % stride)) continue;
        unsigned long long hsh = 0; Work wk; wk.f = &f; wk.dfmt = 0; wk.threaded = 0; wk.shift = 0; wk.mipFilter = 5;
        if (f.kind == 1) { if (runW1(f, &w1, &hsh, false) && runW1Mem(f, &hsh)) { wk.kind = 1; wk.serial = hsh; g_work.push_back(wk); }
                           if (nT1 < 400) for (int k = 0; k < 7; ++k) if (runT1(f, kT1Fmt[k], &t1, &hsh, false)) { ++nT1; wk.kind = 2; wk.dfmt = kT1Fmt[k]; wk.serial = hsh; g_work.push_back(wk); } }
        else { if (runW2(f, &w2, &hsh, false)) { wk.kind = 0; wk.serial = hsh; g_work.push_back(wk); }
               if ((seen[f.kind] / stride) % 8 == 0) runW2(f, &w2b, NULL, false, 1, 0);
               if ((seen[f.kind] / stride) % 4 == 1) { Work w3 = wk; w3.kind = 0; w3.shift = 1; w3.mipFilter = 5;                       // half size: D3DX resizes (its default filter dithers)
                   if (runW2(f, &w2c, &hsh, false, 0, 0, 1, 5)) { w3.serial = hsh; g_work.push_back(w3); }
                   unsigned long long again = 0; if (runW2(f, NULL, &again, true, 0, 0, 1, 5) && again != hsh) { nRepeat++; }
                   w3.mipFilter = (1u << 26) | 5;                                                                                    // half size by skipping the file's first level
                   if (runW2(f, &w2d, &hsh, false, 0, 0, 1, (1u << 26) | 5)) { w3.serial = hsh; g_work.push_back(w3); } }
        }
        if ((w1.n + w2.n) % 200 == 199) printf("  ... %d files, %.0f s\n", w1.n + w2.n + 1, ms(now() - tStart) / 1000.0);
    }
    report(w2); report(w2b); report(w2c); report(w2d); report(w1); report(t1);
    printf("\nresized textures decoded a second time: %d gave another result than the first time (D3DX's dither must not depend on anything but its input)\n", nRepeat);

    // the same memory runs on several threads at once
    LONGLONG ts = now();
    std::vector<HANDLE> th; for (int i = 0; i < nThreads; ++i) th.push_back(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    WaitForMultipleObjects((DWORD)th.size(), &th[0], TRUE, INFINITE);
    LONGLONG te = now(); int tdiff = 0;
    for (size_t i = 0; i < g_work.size(); ++i) if (g_work[i].serial != g_work[i].threaded) { if (tdiff++ < 8) printf("   THREADED RESULT DIFFERS: %s\n", g_work[i].f->name.c_str()); }
    double serialMs = ms(w2.tMem + w1.tMem + t1.tMem);
    printf("\n%d threads at once: %u memory runs in %.1f ms (one after another: %.1f ms, x%.1f), %d differ from the single-threaded result\n", nThreads, (unsigned)g_work.size(), ms(te - ts), serialMs, serialMs / (ms(te - ts) > 0 ? ms(te - ts) : 1), tdiff);

    // the other x87 precision: does it change D3DX's output, and do real and memory still agree under it?
    unsigned int other = (g_cw & _MCW_PC) == _PC_24 ? _PC_53 : _PC_24;
    _control87(other, _MCW_PC);
    Stat c2 = { "other x87 precision: W2" }, c1 = { "other x87 precision: W1" }; int changed = 0, cmp = 0; size_t step = g_work.size() / 160 + 1;
    for (size_t i = 0; i < g_work.size(); i += step) {
        unsigned long long hsh = 0; const Work& w = g_work[i];
        bool ok = w.kind == 0 ? runW2(*w.f, &c2, &hsh, false, 0, 0, w.shift, w.mipFilter) : w.kind == 1 ? (runW1(*w.f, &c1, &hsh, false) && runW1Mem(*w.f, &hsh)) : runT1(*w.f, w.dfmt, &c1, &hsh, false);
        if (ok) { cmp++; if (hsh != w.serial) changed++; }
    }
    _control87(g_cw, _MCW_PC);
    printf("\nx87 precision set to %s instead: real and memory agree on %d of %d textures (%d DIFFERENT); the output itself changed with the precision for %d of %d\n",
           other == _PC_24 ? "24 bit" : "53 bit", c2.same + c1.same, c2.n + c1.n, c2.diff + c1.diff, changed, cmp);
    for (size_t i = 0; i < c2.bad.size(); ++i) printf("   DIFFERENT: %s\n", c2.bad[i].c_str());
    for (size_t i = 0; i < c1.bad.size(); ++i) printf("   DIFFERENT: %s\n", c1.bad[i].c_str());

    char un[2048]; txUnimplText(un, sizeof(un));
    printf("\nmethods D3DX reached that the stand-ins do not implement: %s\n", un[0] ? un : "none");
    printf("format questions: %d answered by the real Direct3D object, %d from memory; memory textures peak %.1f MB, %d still alive\n", (int)g_txCdfMiss, (int)g_txCdfHit, (double)g_txMemPeak / 1048576.0, (int)g_txMemN);
    int bad = w2.diff + w2b.diff + w2c.diff + w2d.diff + nRepeat + w1.diff + t1.diff + tdiff + c2.diff + c1.diff + (un[0] ? 1 : 0) + (g_txMemN ? 1 : 0);
    printf("\n%s\n", bad ? "FAILED" : "PASSED");
    return bad ? 1 : 0;
}
