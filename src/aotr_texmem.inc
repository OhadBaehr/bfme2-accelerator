// aotr_texmem.inc - textures that are plain memory, for running D3DX's texture functions off the game thread.
//
// D3DX's texture loaders and filters (D3DXCreateTextureFromFileInMemoryEx, D3DXLoadSurfaceFromMemory,
// D3DXFilterTexture) are pure CPU work - decode, convert, box-filter, DXT-compress - that reaches the texture only
// through its public COM methods: GetLevelDesc, LockRect, UnlockRect. Nothing in them needs Direct3D. So they can be
// given an object that implements those methods over a block of memory, on any thread, with no device involved, and
// the bytes they leave in it are the bytes they would have left in the real texture: same DLL, same code path, same
// input, same answers from every query they make.
//
//   TxMem   IDirect3DTexture9 over one memory block (all mip levels), with an IDirect3DSurface9 per level
//   TxDev   IDirect3DDevice9 as far as D3DX asks: caps, display mode, creation parameters (copies of the real
//           device's), GetDirect3D, and CreateTexture, which either makes a TxMem or - as a PROBE - notes the
//           arguments D3DX settled on and fails, so the caller learns the size/format/level count D3DX would create
//           without paying for the decode
//   TxD3D   IDirect3D9 as far as D3DX asks: CheckDeviceFormat and friends, answered by the real object (read-only
//           queries) and remembered
//
// Every other method of the four interfaces is a generated placeholder that notes it was called and fails
// (aotr_texmem_gen.inc); the offline test (texjob_test.cpp) runs the mod's real texture files through all of this
// and reports any placeholder D3DX reached.
//
// The includer may define TX_LOG(fmt, ...) before including.
#ifndef TX_LOG
#define TX_LOG(...) ((void)0)
#endif

static volatile LONG g_txUnimplN = 0, g_txUnimplLast = -1;
static volatile LONG g_txUnimplSeen[4][4];
static void txUnimpl(int cls, int slot) {
    InterlockedIncrement(&g_txUnimplN); g_txUnimplLast = (cls << 16) | slot;
    _InterlockedOr(&g_txUnimplSeen[cls & 3][(slot >> 5) & 3], 1L << (slot & 31));
}
#include "aotr_texmem_gen.inc"

#define TX_OK            0u
#define TX_INVALIDCALL   0x8876086Cu
#define TX_NOTAVAILABLE  0x8876086Au
#define TX_OUTOFMEMORY   0x8007000Eu
#define TX_NOINTERFACE   0x80004002u
#define TX_MAXLEVELS     15
struct TxRECT { LONG l, t, r, b; };
struct TxLOCKED { INT pitch; void* bits; };
struct TxDESC { DWORD Format, Type, Usage, Pool, MSType, MSQuality, Width, Height; };

// bytes per pixel (plain formats) or bytes per 4x4 block (DXT1..5); false: a format these textures do not take
static bool txFmtInfo(DWORD fmt, DWORD* bytes, DWORD* block) {
    *bytes = 0; *block = 0;
    switch (fmt) {
    case 0x31545844u: *block = 8; return true;                                                       // DXT1
    case 0x32545844u: case 0x33545844u: case 0x34545844u: case 0x35545844u: *block = 16; return true; // DXT2..DXT5
    case 20: *bytes = 3; return true;                                                               // R8G8B8
    case 21: case 22: case 31: case 32: case 33: case 34: case 35: case 62: case 63: case 64: case 67: case 112: case 114: *bytes = 4; return true;
    case 23: case 24: case 25: case 26: case 29: case 30: case 40: case 51: case 60: case 61: case 81: case 111: *bytes = 2; return true;
    case 27: case 28: case 41: case 50: case 52: *bytes = 1; return true;
    case 36: case 110: case 113: case 115: *bytes = 8; return true;
    case 116: *bytes = 16; return true;
    }
    return false;
}
static void txLevelGeom(DWORD bytes, DWORD block, DWORD w, DWORD h, DWORD* pitch, DWORD* rows, DWORD* rowBytes) {
    if (block) { DWORD bw = (w + 3) / 4, bh = (h + 3) / 4; if (!bw) bw = 1; if (!bh) bh = 1; *pitch = bw * block; *rows = bh; *rowBytes = bw * block; }
    else { *pitch = (w * bytes + 3) & ~3u; *rows = h; *rowBytes = w * bytes; }
}
static DWORD txFullLevels(DWORD w, DWORD h) { DWORD n = 1, m = w > h ? w : h; while (m > 1) { m >>= 1; n++; } return n; }

struct TxMem;
struct TxDev;
struct TxSurf { void** vt; TxMem* tex; DWORD level; };
struct TxLevel { DWORD w, h, pitch, rows, rowBytes, size; BYTE* bits; };
struct TxMem {
    void** vt;                    // IDirect3DTexture9
    volatile LONG ref;
    DWORD fmt, usage, pool, levels, w, h, bytes, block;
    TxLevel lv[TX_MAXLEVELS];
    TxSurf  sf[TX_MAXLEVELS];
    BYTE* mem; DWORD memSize; LONG memVirtual;
    TxDev* dev;                   // what GetDevice answers
    volatile LONG locked;         // bit i: level i is locked
    volatile LONG written;        // bit i: level i was locked for writing at least once
    volatile LONG nLock;
    void* user;                   // the includer's own
};
struct TxDev {
    void** vt;                    // IDirect3DDevice9
    volatile LONG ref;
    LONG mode;                    // 0 makes memory textures, 1 probe: notes CreateTexture's arguments and fails
    DWORD cw, ch, clevels, cusage, cfmt, cpool; LONG nCreate, nMismatch;
    TxMem* give;                  // mode 0: hand this one out if the arguments are its own
    TxMem* made;                  // mode 0: the last texture handed out (not referenced)
};
struct TxD3D { void** vt; volatile LONG ref; };

// what the real device and Direct3D object answer; filled once on the thread that owns the device
struct TxInfo { BYTE caps[304]; DWORD mode[4]; DWORD params[4]; void* realD3D; LONG ok; };
static TxInfo g_txInfo;
static TxD3D g_txD3D;
static TxDev g_txDevStatic;
static HANDLE g_txHeap = NULL;
static volatile LONG64 g_txMemLive = 0, g_txMemPeak = 0;
static volatile LONG g_txMemN = 0;

static const GUID kTxIID_Unknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kTxIID_Resource = {0x05EEC05D, 0x8F7D, 0x4362, {0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04}};
static const GUID kTxIID_BaseTexture = {0x580CA87E, 0x1D3C, 0x4D54, {0x99, 0x1D, 0xB7, 0xD3, 0xE3, 0xC2, 0x98, 0xCE}};
static const GUID kTxIID_Texture = {0x85C31227, 0x3DE5, 0x4F00, {0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5}};
static const GUID kTxIID_Surface = {0x0CFBAF3A, 0x9FF6, 0x429A, {0x99, 0xB3, 0xA2, 0x79, 0x6A, 0xF8, 0xB8, 0x9B}};
static const GUID kTxIID_Device = {0xD0223B96, 0xBF7A, 0x43FD, {0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB}};
static const GUID kTxIID_D3D = {0x81BDCBCA, 0x64D4, 0x426D, {0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C}};
static __forceinline bool txIs(const GUID* a, const GUID& b) { return a && memcmp(a, &b, sizeof(GUID)) == 0; }

// ---------------------------------------------------------------- the memory texture
static void txMemFree(TxMem* t) {
    if (t->mem) { if (t->memVirtual) VirtualFree(t->mem, 0, MEM_RELEASE); else HeapFree(g_txHeap, 0, t->mem); }
    InterlockedExchangeAdd64(&g_txMemLive, -(LONG64)t->memSize); InterlockedDecrement(&g_txMemN);
    HeapFree(g_txHeap, 0, t);
}
static TxMem* txMemCreate(DWORD w, DWORD h, DWORD levels, DWORD usage, DWORD fmt, DWORD pool) {
    DWORD bytes, block;
    if (!w || !h || w > 16384 || h > 16384 || !txFmtInfo(fmt, &bytes, &block)) return NULL;
    DWORD full = txFullLevels(w, h);
    if (!levels || levels > full) levels = full;
    if (levels > TX_MAXLEVELS) return NULL;
    if (!g_txHeap) return NULL;
    TxMem* t = (TxMem*)HeapAlloc(g_txHeap, HEAP_ZERO_MEMORY, sizeof(TxMem));
    if (!t) return NULL;
    t->vt = g_txVt_tex; t->ref = 1; t->fmt = fmt; t->usage = usage; t->pool = pool; t->levels = levels; t->w = w; t->h = h; t->bytes = bytes; t->block = block;
    t->dev = &g_txDevStatic;
    DWORD total = 0;
    for (DWORD i = 0; i < levels; ++i) {
        TxLevel& L = t->lv[i];
        L.w = (w >> i) ? (w >> i) : 1; L.h = (h >> i) ? (h >> i) : 1;
        txLevelGeom(bytes, block, L.w, L.h, &L.pitch, &L.rows, &L.rowBytes);
        L.size = L.pitch * L.rows;
        total += (L.size + 63) & ~63u;
        t->sf[i].vt = g_txVt_surf; t->sf[i].tex = t; t->sf[i].level = i;
    }
    t->memSize = total; t->memVirtual = total >= (256u << 10) ? 1 : 0;
    t->mem = t->memVirtual ? (BYTE*)VirtualAlloc(NULL, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : (BYTE*)HeapAlloc(g_txHeap, HEAP_ZERO_MEMORY, total);
    if (!t->mem) { HeapFree(g_txHeap, 0, t); return NULL; }
    DWORD at = 0;
    for (DWORD i = 0; i < levels; ++i) { t->lv[i].bits = t->mem + at; at += (t->lv[i].size + 63) & ~63u; }
    LONG64 live = InterlockedExchangeAdd64(&g_txMemLive, (LONG64)total) + total;
    if (live > g_txMemPeak) g_txMemPeak = live;
    InterlockedIncrement(&g_txMemN);
    return t;
}
static DWORD __stdcall tx_tex_QueryInterface(TxMem* t, const GUID* iid, void** out) {
    if (!out) return TX_INVALIDCALL;
    if (txIs(iid, kTxIID_Unknown) || txIs(iid, kTxIID_Resource) || txIs(iid, kTxIID_BaseTexture) || txIs(iid, kTxIID_Texture)) { InterlockedIncrement(&t->ref); *out = t; return TX_OK; }
    *out = NULL; return TX_NOINTERFACE;
}
static DWORD __stdcall tx_tex_AddRef(TxMem* t) { return (DWORD)InterlockedIncrement(&t->ref); }
static DWORD __stdcall tx_tex_Release(TxMem* t) { LONG r = InterlockedDecrement(&t->ref); if (!r) txMemFree(t); return (DWORD)r; }
static DWORD __stdcall tx_dev_AddRef(TxDev* d);
static DWORD __stdcall tx_tex_GetDevice(TxMem* t, void** out) { if (!out) return TX_INVALIDCALL; tx_dev_AddRef(t->dev); *out = t->dev; return TX_OK; }
static DWORD __stdcall tx_tex_SetPriority(TxMem*, DWORD) { return 0; }
static DWORD __stdcall tx_tex_GetPriority(TxMem*) { return 0; }
static DWORD __stdcall tx_tex_PreLoad(TxMem*) { return 0; }
static DWORD __stdcall tx_tex_GetType(TxMem*) { return 3; }                         // D3DRTYPE_TEXTURE
static DWORD __stdcall tx_tex_SetLOD(TxMem*, DWORD) { return 0; }
static DWORD __stdcall tx_tex_GetLOD(TxMem*) { return 0; }
static DWORD __stdcall tx_tex_GetLevelCount(TxMem* t) { return t->levels; }
static DWORD __stdcall tx_tex_SetAutoGenFilterType(TxMem*, DWORD) { return TX_OK; }
static DWORD __stdcall tx_tex_GetAutoGenFilterType(TxMem*) { return 2; }            // D3DTEXF_LINEAR, a texture's default
static DWORD __stdcall tx_tex_GenerateMipSubLevels(TxMem*) { return 0; }
static void txDesc(TxMem* t, DWORD level, TxDESC* d) {
    d->Format = t->fmt; d->Type = 1; d->Usage = t->usage; d->Pool = t->pool; d->MSType = 0; d->MSQuality = 0; d->Width = t->lv[level].w; d->Height = t->lv[level].h;
}
static DWORD __stdcall tx_tex_GetLevelDesc(TxMem* t, DWORD level, TxDESC* d) { if (level >= t->levels || !d) return TX_INVALIDCALL; txDesc(t, level, d); return TX_OK; }
static DWORD __stdcall tx_tex_GetSurfaceLevel(TxMem* t, DWORD level, void** out) {
    if (!out) return TX_INVALIDCALL;
    if (level >= t->levels) { *out = NULL; return TX_INVALIDCALL; }
    InterlockedIncrement(&t->ref); *out = &t->sf[level]; return TX_OK;
}
static DWORD __stdcall tx_tex_LockRect(TxMem* t, DWORD level, TxLOCKED* lr, const TxRECT* rc, DWORD flags) {
    if (level >= t->levels || !lr) return TX_INVALIDCALL;
    TxLevel& L = t->lv[level];
    LONG bit = 1L << level;
    if (_InterlockedOr(&t->locked, bit) & bit) { lr->pitch = 0; lr->bits = NULL; return TX_INVALIDCALL; }   // already locked
    DWORD off = 0;
    if (rc) {
        if (rc->l < 0 || rc->t < 0 || rc->r > (LONG)L.w || rc->b > (LONG)L.h || rc->l >= rc->r || rc->t >= rc->b) { _InterlockedAnd(&t->locked, ~bit); lr->pitch = 0; lr->bits = NULL; return TX_INVALIDCALL; }
        off = t->block ? ((DWORD)rc->t / 4) * L.pitch + ((DWORD)rc->l / 4) * t->block : (DWORD)rc->t * L.pitch + (DWORD)rc->l * t->bytes;
    }
    if (!(flags & 0x10)) _InterlockedOr(&t->written, bit);                           // not D3DLOCK_READONLY
    InterlockedIncrement(&t->nLock);
    lr->pitch = (INT)L.pitch; lr->bits = L.bits + off;
    return TX_OK;
}
static DWORD __stdcall tx_tex_UnlockRect(TxMem* t, DWORD level) {
    if (level >= t->levels) return TX_INVALIDCALL;
    _InterlockedAnd(&t->locked, ~(1L << level));
    return TX_OK;
}
static DWORD __stdcall tx_tex_AddDirtyRect(TxMem*, const TxRECT*) { return TX_OK; }

static DWORD __stdcall tx_surf_QueryInterface(TxSurf* s, const GUID* iid, void** out) {
    if (!out) return TX_INVALIDCALL;
    if (txIs(iid, kTxIID_Unknown) || txIs(iid, kTxIID_Resource) || txIs(iid, kTxIID_Surface)) { InterlockedIncrement(&s->tex->ref); *out = s; return TX_OK; }
    *out = NULL; return TX_NOINTERFACE;
}
static DWORD __stdcall tx_surf_AddRef(TxSurf* s) { return (DWORD)InterlockedIncrement(&s->tex->ref); }
static DWORD __stdcall tx_surf_Release(TxSurf* s) { return tx_tex_Release(s->tex); }
static DWORD __stdcall tx_surf_GetDevice(TxSurf* s, void** out) { return tx_tex_GetDevice(s->tex, out); }
static DWORD __stdcall tx_surf_SetPriority(TxSurf*, DWORD) { return 0; }
static DWORD __stdcall tx_surf_GetPriority(TxSurf*) { return 0; }
static DWORD __stdcall tx_surf_PreLoad(TxSurf*) { return 0; }
static DWORD __stdcall tx_surf_GetType(TxSurf*) { return 1; }                       // D3DRTYPE_SURFACE
static DWORD __stdcall tx_surf_GetContainer(TxSurf* s, const GUID* iid, void** out) { return tx_tex_QueryInterface(s->tex, iid, out); }
static DWORD __stdcall tx_surf_GetDesc(TxSurf* s, TxDESC* d) { if (!d) return TX_INVALIDCALL; txDesc(s->tex, s->level, d); return TX_OK; }
static DWORD __stdcall tx_surf_LockRect(TxSurf* s, TxLOCKED* lr, const TxRECT* rc, DWORD flags) { return tx_tex_LockRect(s->tex, s->level, lr, rc, flags); }
static DWORD __stdcall tx_surf_UnlockRect(TxSurf* s) { return tx_tex_UnlockRect(s->tex, s->level); }

// ---------------------------------------------------------------- the device and Direct3D object D3DX asks
static DWORD __stdcall tx_dev_QueryInterface(TxDev* d, const GUID* iid, void** out) {
    if (!out) return TX_INVALIDCALL;
    if (txIs(iid, kTxIID_Unknown) || txIs(iid, kTxIID_Device)) { InterlockedIncrement(&d->ref); *out = d; return TX_OK; }
    *out = NULL; return TX_NOINTERFACE;
}
static DWORD __stdcall tx_dev_AddRef(TxDev* d) { return (DWORD)InterlockedIncrement(&d->ref); }
static DWORD __stdcall tx_dev_Release(TxDev* d) { return (DWORD)InterlockedDecrement(&d->ref); }       // never freed: static or on the caller's stack
static DWORD __stdcall tx_dev_TestCooperativeLevel(TxDev*) { return TX_OK; }
static DWORD __stdcall tx_dev_GetDirect3D(TxDev*, void** out) { if (!out) return TX_INVALIDCALL; InterlockedIncrement(&g_txD3D.ref); *out = &g_txD3D; return TX_OK; }
static DWORD __stdcall tx_dev_GetDeviceCaps(TxDev*, void* caps) { if (!caps) return TX_INVALIDCALL; memcpy(caps, g_txInfo.caps, sizeof(g_txInfo.caps)); return TX_OK; }
static DWORD __stdcall tx_dev_GetDisplayMode(TxDev*, DWORD, void* mode) { if (!mode) return TX_INVALIDCALL; memcpy(mode, g_txInfo.mode, 16); return TX_OK; }
static DWORD __stdcall tx_dev_GetCreationParameters(TxDev*, void* p) { if (!p) return TX_INVALIDCALL; memcpy(p, g_txInfo.params, 16); return TX_OK; }
static DWORD __stdcall tx_dev_CreateTexture(TxDev* d, DWORD w, DWORD h, DWORD levels, DWORD usage, DWORD fmt, DWORD pool, void** out, void*) {
    if (!out) return TX_INVALIDCALL;
    *out = NULL;
    if (InterlockedIncrement(&d->nCreate) == 1) { d->cw = w; d->ch = h; d->clevels = levels; d->cusage = usage; d->cfmt = fmt; d->cpool = pool; }
    if (d->mode == 1) return TX_OUTOFMEMORY;                                         // probe: the arguments are what was wanted
    TxMem* t = d->give;
    DWORD want = levels ? levels : txFullLevels(w, h);
    if (t && t->w == w && t->h == h && t->levels == want && t->fmt == fmt && t->usage == usage && t->pool == pool) { d->give = NULL; InterlockedIncrement(&t->ref); }
    else { if (t) d->nMismatch++; t = txMemCreate(w, h, levels, usage, fmt, pool); if (!t) return TX_OUTOFMEMORY; }
    t->dev = d; d->made = t;
    *out = t; return TX_OK;
}

// CheckDeviceFormat answers are remembered: D3DX asks the same handful of questions for every texture
struct TxCdf { DWORD k[6]; DWORD hr; LONG used; };
#define TX_CDFN 512
static TxCdf g_txCdf[TX_CDFN];
static CRITICAL_SECTION g_txCdfCs;
static volatile LONG g_txCdfMiss = 0, g_txCdfHit = 0;
static DWORD __stdcall tx_d3d_QueryInterface(TxD3D* d, const GUID* iid, void** out) {
    if (!out) return TX_INVALIDCALL;
    if (txIs(iid, kTxIID_Unknown) || txIs(iid, kTxIID_D3D)) { InterlockedIncrement(&d->ref); *out = d; return TX_OK; }
    *out = NULL; return TX_NOINTERFACE;
}
static DWORD __stdcall tx_d3d_AddRef(TxD3D* d) { return (DWORD)InterlockedIncrement(&d->ref); }
static DWORD __stdcall tx_d3d_Release(TxD3D* d) { return (DWORD)InterlockedDecrement(&d->ref); }
#define TX_REAL(SLOT) ((*(void***)g_txInfo.realD3D)[SLOT])
static DWORD __stdcall tx_d3d_GetAdapterCount(TxD3D*) { return ((DWORD(__stdcall*)(void*))TX_REAL(4))(g_txInfo.realD3D); }
static DWORD __stdcall tx_d3d_GetAdapterIdentifier(TxD3D*, DWORD a, DWORD b, DWORD c) { return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD))TX_REAL(5))(g_txInfo.realD3D, a, b, c); }
static DWORD __stdcall tx_d3d_GetAdapterDisplayMode(TxD3D*, DWORD a, DWORD b) { return ((DWORD(__stdcall*)(void*, DWORD, DWORD))TX_REAL(8))(g_txInfo.realD3D, a, b); }
static DWORD __stdcall tx_d3d_CheckDeviceType(TxD3D*, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e) { return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD))TX_REAL(9))(g_txInfo.realD3D, a, b, c, d, e); }
static DWORD __stdcall tx_d3d_CheckDeviceFormat(TxD3D*, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f) {
    DWORD k[6] = { a, b, c, d, e, f };
    DWORD h = (a * 31 + b * 131 + c * 2654435761u + d * 40503u + e * 97 + f * 2246822519u) >> 23;
    EnterCriticalSection(&g_txCdfCs);
    for (int i = 0; i < 16; ++i) {
        TxCdf& e2 = g_txCdf[(h + i) & (TX_CDFN - 1)];
        if (!e2.used) break;
        if (memcmp(e2.k, k, sizeof(k)) == 0) { DWORD r = e2.hr; LeaveCriticalSection(&g_txCdfCs); g_txCdfHit++; return r; }
    }
    LeaveCriticalSection(&g_txCdfCs);
    DWORD hr = ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))TX_REAL(10))(g_txInfo.realD3D, a, b, c, d, e, f);
    EnterCriticalSection(&g_txCdfCs);
    for (int i = 0; i < 16; ++i) {
        TxCdf& e2 = g_txCdf[(h + i) & (TX_CDFN - 1)];
        if (e2.used) continue;
        memcpy(e2.k, k, sizeof(k)); e2.hr = hr; e2.used = 1; break;
    }
    LeaveCriticalSection(&g_txCdfCs);
    InterlockedIncrement(&g_txCdfMiss);
    return hr;
}
static DWORD __stdcall tx_d3d_CheckDeviceMultiSampleType(TxD3D*, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e, DWORD f) { return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD))TX_REAL(11))(g_txInfo.realD3D, a, b, c, d, e, f); }
static DWORD __stdcall tx_d3d_CheckDepthStencilMatch(TxD3D*, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e) { return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD))TX_REAL(12))(g_txInfo.realD3D, a, b, c, d, e); }
static DWORD __stdcall tx_d3d_CheckDeviceFormatConversion(TxD3D*, DWORD a, DWORD b, DWORD c, DWORD d) { return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD, DWORD))TX_REAL(13))(g_txInfo.realD3D, a, b, c, d); }
static DWORD __stdcall tx_d3d_GetDeviceCaps(TxD3D*, DWORD a, DWORD b, DWORD c) { return ((DWORD(__stdcall*)(void*, DWORD, DWORD, DWORD))TX_REAL(14))(g_txInfo.realD3D, a, b, c); }
static DWORD __stdcall tx_d3d_GetAdapterMonitor(TxD3D*, DWORD a) { return ((DWORD(__stdcall*)(void*, DWORD))TX_REAL(15))(g_txInfo.realD3D, a); }

static void txDevInit(TxDev* d, int mode, TxMem* give) { memset(d, 0, sizeof(*d)); d->vt = g_txVt_dev; d->ref = 1; d->mode = mode; d->give = give; }

// The real device's answers. `devMethods` is the device's ORIGINAL method table (unhooked), `dev` the device.
// Call on the thread that may call the device, before the first memory texture is made, and again after a Reset.
static bool txInfoFill(void* dev, void** devMethods) {
    BYTE caps[304]; DWORD mode[4], params[4]; void* d3d = NULL;
    if ((LONG)((DWORD(__stdcall*)(void*, void*))devMethods[7])(dev, caps) < 0) return false;                  // GetDeviceCaps
    if ((LONG)((DWORD(__stdcall*)(void*, DWORD, void*))devMethods[8])(dev, 0, mode) < 0) return false;        // GetDisplayMode
    if ((LONG)((DWORD(__stdcall*)(void*, void*))devMethods[9])(dev, params) < 0) return false;                // GetCreationParameters
    if (!g_txInfo.realD3D) {
        if ((LONG)((DWORD(__stdcall*)(void*, void**))devMethods[6])(dev, &d3d) < 0 || !d3d) return false;     // GetDirect3D: the reference is kept
        g_txInfo.realD3D = d3d;
    }
    memcpy(g_txInfo.caps, caps, sizeof(caps)); memcpy(g_txInfo.mode, mode, 16); memcpy(g_txInfo.params, params, 16);
    g_txInfo.ok = 1;
    return true;
}
static void txForgetFormats() {                                    // after a device reset: the display format may have changed
    EnterCriticalSection(&g_txCdfCs); memset(g_txCdf, 0, sizeof(g_txCdf)); LeaveCriticalSection(&g_txCdfCs);
}
static bool txMemInitOnce() {
    static volatile LONG done = 0;
    if (done) return g_txHeap != NULL;
    InitializeCriticalSection(&g_txCdfCs);
    g_txHeap = HeapCreate(0, 1u << 20, 0);
    #define TX_SET(CLS, SLOT, FN) g_txVt_##CLS[SLOT] = (void*)&FN
    TX_SET(tex, 0, tx_tex_QueryInterface); TX_SET(tex, 1, tx_tex_AddRef); TX_SET(tex, 2, tx_tex_Release); TX_SET(tex, 3, tx_tex_GetDevice);
    TX_SET(tex, 7, tx_tex_SetPriority); TX_SET(tex, 8, tx_tex_GetPriority); TX_SET(tex, 9, tx_tex_PreLoad); TX_SET(tex, 10, tx_tex_GetType);
    TX_SET(tex, 11, tx_tex_SetLOD); TX_SET(tex, 12, tx_tex_GetLOD); TX_SET(tex, 13, tx_tex_GetLevelCount); TX_SET(tex, 14, tx_tex_SetAutoGenFilterType);
    TX_SET(tex, 15, tx_tex_GetAutoGenFilterType); TX_SET(tex, 16, tx_tex_GenerateMipSubLevels); TX_SET(tex, 17, tx_tex_GetLevelDesc);
    TX_SET(tex, 18, tx_tex_GetSurfaceLevel); TX_SET(tex, 19, tx_tex_LockRect); TX_SET(tex, 20, tx_tex_UnlockRect); TX_SET(tex, 21, tx_tex_AddDirtyRect);
    TX_SET(surf, 0, tx_surf_QueryInterface); TX_SET(surf, 1, tx_surf_AddRef); TX_SET(surf, 2, tx_surf_Release); TX_SET(surf, 3, tx_surf_GetDevice);
    TX_SET(surf, 7, tx_surf_SetPriority); TX_SET(surf, 8, tx_surf_GetPriority); TX_SET(surf, 9, tx_surf_PreLoad); TX_SET(surf, 10, tx_surf_GetType);
    TX_SET(surf, 11, tx_surf_GetContainer); TX_SET(surf, 12, tx_surf_GetDesc); TX_SET(surf, 13, tx_surf_LockRect); TX_SET(surf, 14, tx_surf_UnlockRect);
    TX_SET(dev, 0, tx_dev_QueryInterface); TX_SET(dev, 1, tx_dev_AddRef); TX_SET(dev, 2, tx_dev_Release); TX_SET(dev, 3, tx_dev_TestCooperativeLevel);
    TX_SET(dev, 6, tx_dev_GetDirect3D); TX_SET(dev, 7, tx_dev_GetDeviceCaps); TX_SET(dev, 8, tx_dev_GetDisplayMode); TX_SET(dev, 9, tx_dev_GetCreationParameters);
    TX_SET(dev, 23, tx_dev_CreateTexture);
    TX_SET(d3d, 0, tx_d3d_QueryInterface); TX_SET(d3d, 1, tx_d3d_AddRef); TX_SET(d3d, 2, tx_d3d_Release); TX_SET(d3d, 4, tx_d3d_GetAdapterCount);
    TX_SET(d3d, 5, tx_d3d_GetAdapterIdentifier); TX_SET(d3d, 8, tx_d3d_GetAdapterDisplayMode); TX_SET(d3d, 9, tx_d3d_CheckDeviceType);
    TX_SET(d3d, 10, tx_d3d_CheckDeviceFormat); TX_SET(d3d, 11, tx_d3d_CheckDeviceMultiSampleType); TX_SET(d3d, 12, tx_d3d_CheckDepthStencilMatch);
    TX_SET(d3d, 13, tx_d3d_CheckDeviceFormatConversion); TX_SET(d3d, 14, tx_d3d_GetDeviceCaps); TX_SET(d3d, 15, tx_d3d_GetAdapterMonitor);
    #undef TX_SET
    g_txD3D.vt = g_txVt_d3d; g_txD3D.ref = 1;
    txDevInit(&g_txDevStatic, 0, NULL);
    InterlockedExchange(&done, 1);
    return g_txHeap != NULL;
}
// which placeholders has D3DX reached? (a line per interface, empty when none)
static int txUnimplText(char* out, int n) {
    static const char* const* const names[4] = { kTxName_dev, kTxName_d3d, kTxName_tex, kTxName_surf };
    static const char* const cls[4] = { "device", "Direct3D", "texture", "surface" };
    static const int count[4] = { 119, 17, 22, 17 };
    int at = 0; out[0] = 0;
    for (int c = 0; c < 4; ++c) for (int s = 0; s < count[c]; ++s)
        if ((g_txUnimplSeen[c][s >> 5] >> (s & 31)) & 1) { if (at + 64 < n) at += wsprintfA(out + at, "%s.%s ", cls[c], names[c][s]); }
    return at;
}

// Copy the levels in `mask` of a memory texture into a real one, through the real texture's own LockRect / UnlockRect
// (`lockFn`, `unlockFn`: its ORIGINAL methods). The caller owns whatever serialises calls into Direct3D.
static bool txUpload(TxMem* m, void* tex, void* lockFn, void* unlockFn, DWORD mask) {
    bool ok = true;
    for (DWORD i = 0; i < m->levels; ++i) {
        if (!((mask >> i) & 1)) continue;
        TxLOCKED lr = { 0, NULL };
        if ((LONG)((DWORD(__stdcall*)(void*, DWORD, TxLOCKED*, const TxRECT*, DWORD))lockFn)(tex, i, &lr, NULL, 0) < 0 || !lr.bits) { ok = false; continue; }
        const TxLevel& L = m->lv[i];
        if (lr.pitch == (INT)L.pitch) memcpy(lr.bits, L.bits, L.size);
        else for (DWORD y = 0; y < L.rows; ++y) memcpy((BYTE*)lr.bits + (INT)y * lr.pitch, L.bits + y * L.pitch, L.rowBytes);
        ((DWORD(__stdcall*)(void*, DWORD))unlockFn)(tex, i);
    }
    return ok;
}
// ...and the other way: a level of a real texture into the memory one (read-only lock)
static bool txDownload(TxMem* m, void* tex, void* lockFn, void* unlockFn, DWORD level) {
    TxLOCKED lr = { 0, NULL };
    if ((LONG)((DWORD(__stdcall*)(void*, DWORD, TxLOCKED*, const TxRECT*, DWORD))lockFn)(tex, level, &lr, NULL, 0x10) < 0 || !lr.bits) return false;
    const TxLevel& L = m->lv[level];
    if (lr.pitch == (INT)L.pitch) memcpy(L.bits, lr.bits, L.size);
    else for (DWORD y = 0; y < L.rows; ++y) memcpy(L.bits + y * L.pitch, (const BYTE*)lr.bits + (INT)y * lr.pitch, L.rowBytes);
    ((DWORD(__stdcall*)(void*, DWORD))unlockFn)(tex, level);
    return true;
}
