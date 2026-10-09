// ================================================================ terrain patch textures on every core (v57, 2026-10-08)
// The ground is drawn in patches of 16 x 16 cells, and each patch near the camera has its own texture (two with
// the second texture set on): 512 x 512, A1R5G5B5, three levels. The engine makes them when a patch comes into
// view - at most two patches a frame (0x004E0A73 -> 0x00511F6B -> 0x004AE3CD) - and throws them away when it
// leaves. A pan therefore builds about thirty of them a second (log of 2026-10-07: 150 "mip chains" per 5 s while
// the camera moves, none while it stands), every one on the game thread, cell by cell:
//
//     TerrainTextureClass::update 0x004EEC82 (map, x0, y0, n, t, flag)
//        GetSurfaceLevel(0), GetDesc, LockRect
//        for each of the n x n cells:  p = map->cell 0x004AE772 (x, y, t, flag, dest, -width)
//              unblended cell: the tile's own 1555 rows are copied to dest, p = NULL
//              blended cell:   the tile is widened to 8888 into a static buffer (0x004AC284), each blend's tile is
//                              widened into a second static buffer and laid over it through an alpha tile, one
//                              pixel at a time in MMX (0x004AD85F -> 0x004AC035), p = the buffer
//              if p: 8888 -> 1555 into dest, four pixels at a time
//        UnlockRect, Release, D3DXFilterTexture(tex, NULL, 0, D3DX_FILTER_BOX)
//
// That is 3.3 ms for the cells and about a millisecond for the rest, per texture, up to four textures in a frame:
// the frames of a pan that take 10 to 18 ms longer than the others, and 4 to 5 ms on a pan's average frame.
//
// Everything in it is integer arithmetic on data that does not change while it runs (the map's per-cell indices,
// the tile bitmaps, the twelve alpha tiles), and every cell writes its own t x t square of the texture. So the
// cells are done on several threads at once, by a replica of the three functions that keeps its two scratch tiles
// on each thread's stack instead of in the engine's two static buffers. The smaller levels are made here as well:
// D3DX's box filter on this format is (a + b + c + d + 2) >> 2 for each colour and for the alpha bit, each level
// from the one before - found with mipprobe.exe on all 2^20 combinations of four 5-bit values, for every level of a
// 2048 x 2048 texture, the same under each x87 precision - so the levels are written directly and no D3DX call, no
// worker job and no file in the texture cache is needed for them.
//
// Exactness is checked three ways: terrain_test.exe runs the engine's own update (game.dat mapped at its base in
// a child process) and this one over the same made-up maps and compares every byte of every level's source; in
// the game the first textures are built a second time with the engine's cell function and compared; and the code
// this replica mirrors is hashed at start - a different build of the game leaves the engine's own code in charge.
// [Engine] TerrainThreads (-1 = as many as fit, 0 = off) / AOTR_TERRAIN=0.

#define TP_UPDATE    0x004EEC82
#define TP_CELL      0x004AE772
#define TP_PEEK      0x005321DC
#define TP_HRFAIL    0x0051EDF0
#define TP_ALPHAINIT 0x004AC0EF
#define TP_FILTER    0x00A3ECD2
#define TP_ALPHATAB  0x00DCFC08
#define TP_GLOBAL    0x00DE4364
#define TP_MAXT      32

typedef int (__fastcall* tTpUpdate)(void* self, void* edx, BYTE* map, int x0, int y0, int n, int t, int flag);
typedef const DWORD* (__fastcall* tTpCell)(BYTE* map, void* edx, int x, int y, int t, int flag, WORD* dest, int stride);
static tTpUpdate o_tpUpdate = NULL;
static volatile LONG g_tpOn = 0;
static LONG g_tpHelpers = 0;
static volatile LONG g_tpChecksLeft = 24, g_tpChecked = 0, g_tpCheckBad = 0;
static volatile LONG g_tpNTex = 0, g_tpNCells = 0, g_tpNBlended = 0, g_tpNOther = 0, g_tpNOwnMips = 0;
static volatile LONG64 g_tpTicks = 0, g_tpCheckMine = 0, g_tpCheckEngine = 0;

struct TpMap {
    const BYTE* m; int w, h, size;
    const short* tile; const int* blend; const int* flags; const int* extra;
    const BYTE* blBegin; int nBlend; int extraOn; int originX, originY;
    const BYTE* alpha[12];
};
static void tpSnap(TpMap& s, const BYTE* map) {
    s.m = map; s.w = *(const int*)(map + 8); s.h = *(const int*)(map + 0xC); s.size = *(const int*)(map + 0x20);
    s.tile = *(const short* const*)(map + 0x98); s.blend = *(const int* const*)(map + 0x9C);
    s.flags = *(const int* const*)(map + 0xA0); s.extra = *(const int* const*)(map + 0xA4);
    s.blBegin = *(const BYTE* const*)(map + 0x80B0);
    s.nBlend = (int)(*(const BYTE* const*)(map + 0x80B4) - s.blBegin) >> 4;            // sar, as the engine has it
    s.extraOn = *(const int*)(*(const BYTE* const*)(ULONG_PTR)TP_GLOBAL + 0x44) != 0;
    s.originX = *(const int*)(map + 0x120E0); s.originY = *(const int*)(map + 0x120E4);
    for (int i = 0; i < 12; ++i) s.alpha[i] = *(const BYTE* const*)(ULONG_PTR)(TP_ALPHATAB + i * 4);
}
// 0x004AB66F: a tile's bitmap for a width
static __forceinline const BYTE* tpData(const BYTE* tile, int width) {
    switch (width) { case 0x20: return tile + 0x2008; case 0x10: return tile + 0x2808; case 8: return tile + 0x2A08;
                     case 4: return tile + 0x2A88; case 2: return tile + 0x2AA8; case 1: return tile + 0x2AB0; default: return tile + 8; }
}
// 0x00511161
static __forceinline bool tpWidthOk(int w) { return w == 0x40 || w == 0x20 || w == 0x10 || w == 8 || w == 4 || w == 2 || w == 1; }
// 0x004AB717
static __forceinline bool tpFlag(const TpMap& s, int x, int y) {
    int idx = (s.originY + y) * s.w + s.originX + x;
    if (idx < 0 || idx >= s.size) return false;
    return s.flags[idx] != 0;
}
// 0x004AC284: a tile widened from 1555 to 8888 (the engine's own widening, quirk and all: the alpha bit lands on
// the red byte's bit 3 as well)
static __forceinline DWORD tpWiden(DWORD w) {
    DWORD v = ((w >> 10) << 19) | (((w >> 5) & 31) << 11) | ((w & 31) << 3);
    return v + ((v >> 5) & 0x003F3F3F);
}
static bool tpTileRGB(const TpMap& s, short tn, int t, int flag, DWORD* out) {
    int idx = (int)tn / 4;
    const BYTE* tile = NULL;
    if (idx < 0x1000) {
        tile = *(const BYTE* const*)(s.m + 0xB0 + idx * 4);
        if (flag == 1) { if (!tile) return false; tile = *(const BYTE* const*)(tile + 0x2AB4); if (!tile) return false; }
    }
    if (0x2000 < t * t * 4) return false;
    if (!tile) return false;
    int w2 = t + t;
    if (!tpWidthOk(w2)) return false;
    const BYTE* pb = tpData(tile, w2);
    if (tn & 1) pb += w2;
    if (tn & 2) pb += t * t * 4;
    const WORD* p = (const WORD*)pb;
    const __m128i zero = _mm_setzero_si128(), m31 = _mm_set1_epi32(31), m3f = _mm_set1_epi32(0x003F3F3F);
    for (int r = 0; r < t; ++r) {
        int k = 0;
        for (; k + 4 <= t; k += 4) {
            __m128i w = _mm_unpacklo_epi16(_mm_loadl_epi64((const __m128i*)(p + k)), zero);         // four pixels, one to a dword
            __m128i v = _mm_or_si128(_mm_or_si128(_mm_slli_epi32(_mm_srli_epi32(w, 10), 19), _mm_slli_epi32(_mm_and_si128(_mm_srli_epi32(w, 5), m31), 11)),
                                     _mm_slli_epi32(_mm_and_si128(w, m31), 3));
            _mm_storeu_si128((__m128i*)(out + k), _mm_add_epi32(v, _mm_and_si128(_mm_srli_epi32(v, 5), m3f)));
        }
        for (; k < t; ++k) out[k] = tpWiden(p[k]);
        out += t; p += w2;                                                        // the row's other half belongs to the tile beside it
    }
    return true;
}
// 0x004AD85F: one blend laid over the cell. The alpha tile is picked as 0x004AC09C does; the pixels as 0x004AC035
// (the same eight operations, on two pixels a step instead of one).
static void tpBlend(const TpMap& s, const BYTE* bi, int t, int flag, DWORD* dst, DWORD* tmp) {
    if (!tpTileRGB(s, *(const short*)bi, t, flag, tmp)) return;
    int e = 0;
    if (bi[4] == 0) {
        if (bi[5]) e = 1;
        else if (bi[6]) e = bi[9] ? 5 : 3;
        else if (bi[7]) e = bi[9] ? 4 : 2;
    }
    if (bi[8]) e += 6;
    const BYTE* a = tpData(s.alpha[e], t);
    if (!a) return;
    const __m128i zero = _mm_setzero_si128(), k = _mm_set1_epi16((short)0xFF01);
    int n = t * t;
    for (int i = 0; i < n; i += 2, a += 4) {
        __m128i sp = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(tmp + i)), zero);
        __m128i dp = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)(dst + i)), zero);
        __m128i av = _mm_set_epi16(a[2], a[2], a[2], a[2], a[0], a[0], a[0], a[0]);
        __m128i m0 = _mm_mullo_epi16(sp, av);
        __m128i m2 = _mm_mullo_epi16(_mm_add_epi16(av, k), dp);
        __m128i r = _mm_srli_epi16(_mm_sub_epi16(m0, m2), 8);
        _mm_storel_epi64((__m128i*)(dst + i), _mm_packus_epi16(r, r));
    }
}
// 0x004EED60 .. 0x004EEDF9: the composed cell, 8888 -> 1555 (alpha always set), rows going up the texture
static __forceinline void tpNarrow(const DWORD* src, WORD* dest, int t, int strideWords) {
    const __m128i m31 = _mm_set1_epi32(31), top = _mm_set1_epi32(0x8000);
    for (int r = 0; r < t; ++r) {
        int k = 0;
        for (; k + 4 <= t; k += 4) {
            __m128i v = _mm_loadu_si128((const __m128i*)(src + k));
            __m128i o = _mm_or_si128(_mm_or_si128(_mm_slli_epi32(_mm_and_si128(_mm_srli_epi32(v, 19), m31), 10), _mm_slli_epi32(_mm_and_si128(_mm_srli_epi32(v, 11), m31), 5)),
                                     _mm_or_si128(_mm_and_si128(_mm_srli_epi32(v, 3), m31), top));
            o = _mm_srai_epi32(_mm_slli_epi32(o, 16), 16);                        // the low word, sign-extended, so that the pack below hands it back unchanged
            _mm_storel_epi64((__m128i*)(dest + k), _mm_packs_epi32(o, o));
        }
        for (; k < t; ++k) { DWORD v = src[k]; dest[k] = (WORD)(0x8000 | (((v >> 19) & 31) << 10) | (((v >> 11) & 31) << 5) | ((v >> 3) & 31)); }
        src += t; dest += strideWords;
    }
}
// 0x004AE772 with the conversion its caller does: one cell into the texture. Returns 0 nothing written, 1 copied,
// 2 composed from blends.
static int tpCell(const TpMap& s, int x, int y, int t, int flag, WORD* dest, int strideWords, DWORD* b1, DWORD* b2) {
    int ndx = s.w * y + x;
    if (y < 0 || x < 0 || x >= s.w || y >= s.h || ndx < 0 || ndx >= s.size) return 0;
    short tn = s.tile[ndx];
    int bl = s.blend[ndx], ex = s.extra[ndx];
    if (tpFlag(s, x, y)) bl = 0;
    if (bl <= 0) {                                                                // (dest is never NULL here)
        int idx = (int)tn / 4;
        if (idx >= 0x1000) return 0;
        const BYTE* tile = *(const BYTE* const*)(s.m + 0xB0 + idx * 4);
        if (flag == 1) { if (!tile) return 0; tile = *(const BYTE* const*)(tile + 0x2AB4); }
        if (!tile) return 0;
        int w2 = t + t;
        if (!tpWidthOk(w2)) return 0;
        const BYTE* p = tpData(tile, w2);
        if (tn & 1) p += w2;
        if (tn & 2) p += t * t * 4;
        for (int r = 0; r < t; ++r) { memcpy(dest, p, (size_t)w2); dest += strideWords; p += t * 4; }
        return 1;
    }
    if (!tpTileRGB(s, tn, t, flag, b1)) return 0;
    if ((unsigned)bl < (unsigned)s.nBlend) tpBlend(s, s.blBegin + bl * 16, t, flag, b1, b2);
    if (s.extraOn && ex > 0 && (unsigned)ex < (unsigned)s.nBlend) tpBlend(s, s.blBegin + ex * 16, t, flag, b1, b2);
    tpNarrow(b1, dest, t, strideWords);
    return 2;
}

// The twelve alpha tiles are made by the engine at the first blend it lays (0x004AC0EF, from 0x004AD85F once the
// blend's own tile is there). The threads only read them, so they must exist before the threads start - and they are
// made at the same point of the game as the engine would: only if this texture has a cell that gets as far.
static bool tpTileThere(const TpMap& s, short tn, int t, int flag) {         // would 0x004AC284 hand back a tile?
    int idx = (int)tn / 4;
    const BYTE* tile = NULL;
    if (idx < 0x1000) {
        tile = *(const BYTE* const*)(s.m + 0xB0 + idx * 4);
        if (flag == 1) { if (!tile) return false; tile = *(const BYTE* const*)(tile + 0x2AB4); if (!tile) return false; }
    }
    return 0x2000 >= t * t * 4 && tile && tpWidthOk(t + t);
}
static bool tpLaysABlend(const TpMap& s, int x0, int y0, int n, int t, int flag) {
    for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
        int x = x0 + i, y = y0 + j, ndx = s.w * y + x;
        if (y < 0 || x < 0 || x >= s.w || y >= s.h || ndx < 0 || ndx >= s.size) continue;
        int bl = s.blend[ndx], ex = s.extra[ndx];
        if (tpFlag(s, x, y)) bl = 0;
        if (bl <= 0 || !tpTileThere(s, s.tile[ndx], t, flag)) continue;
        if ((unsigned)bl < (unsigned)s.nBlend && tpTileThere(s, *(const short*)(s.blBegin + bl * 16), t, flag)) return true;
        if (s.extraOn && ex > 0 && (unsigned)ex < (unsigned)s.nBlend && tpTileThere(s, *(const short*)(s.blBegin + ex * 16), t, flag)) return true;
    }
    return false;
}

// ---------------------------------------------------------------- the smaller levels
// D3DX_FILTER_BOX on A1R5G5B5 (d3dx9_27, mipprobe.exe): each channel (sum of four + 2) >> 2, the alpha bit too.
static void tpHalveRows(const WORD* src, int sw, WORD* dst, int dw, int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
        const WORD* a = src + (size_t)(y * 2) * sw; const WORD* b = a + sw; WORD* o = dst + (size_t)y * dw;
        int x = 0;
        {   // four results a step: the two rows are added channel by channel (16-bit lanes), neighbours summed by pmaddwd with ones, + 2, >> 2
            const __m128i m31 = _mm_set1_epi16(31), ones = _mm_set1_epi16(1), two = _mm_set1_epi32(2), q31 = _mm_set1_epi32(31);
            for (; x + 4 <= dw; x += 4) {
                __m128i ra = _mm_loadu_si128((const __m128i*)(a + 2 * x)), rb = _mm_loadu_si128((const __m128i*)(b + 2 * x));
                __m128i bb = _mm_madd_epi16(_mm_add_epi16(_mm_and_si128(ra, m31), _mm_and_si128(rb, m31)), ones);
                __m128i gg = _mm_madd_epi16(_mm_add_epi16(_mm_and_si128(_mm_srli_epi16(ra, 5), m31), _mm_and_si128(_mm_srli_epi16(rb, 5), m31)), ones);
                __m128i rr = _mm_madd_epi16(_mm_add_epi16(_mm_and_si128(_mm_srli_epi16(ra, 10), m31), _mm_and_si128(_mm_srli_epi16(rb, 10), m31)), ones);
                __m128i aa = _mm_madd_epi16(_mm_add_epi16(_mm_srli_epi16(ra, 15), _mm_srli_epi16(rb, 15)), ones);
                bb = _mm_srli_epi32(_mm_add_epi32(bb, two), 2); gg = _mm_srli_epi32(_mm_add_epi32(gg, two), 2);
                rr = _mm_srli_epi32(_mm_add_epi32(rr, two), 2); aa = _mm_srli_epi32(_mm_add_epi32(aa, two), 2);
                __m128i v = _mm_or_si128(_mm_or_si128(_mm_slli_epi32(aa, 15), _mm_slli_epi32(_mm_and_si128(rr, q31), 10)), _mm_or_si128(_mm_slli_epi32(_mm_and_si128(gg, q31), 5), _mm_and_si128(bb, q31)));
                v = _mm_srai_epi32(_mm_slli_epi32(v, 16), 16);
                _mm_storel_epi64((__m128i*)(o + x), _mm_packs_epi32(v, v));
            }
        }
        for (; x < dw; ++x) {
            DWORD p0 = a[2 * x], p1 = a[2 * x + 1], p2 = b[2 * x], p3 = b[2 * x + 1];
            DWORD rr = ((p0 >> 10) & 31) + ((p1 >> 10) & 31) + ((p2 >> 10) & 31) + ((p3 >> 10) & 31) + 2;
            DWORD gg = ((p0 >> 5) & 31) + ((p1 >> 5) & 31) + ((p2 >> 5) & 31) + ((p3 >> 5) & 31) + 2;
            DWORD bb = (p0 & 31) + (p1 & 31) + (p2 & 31) + (p3 & 31) + 2;
            DWORD aa = (p0 >> 15) + (p1 >> 15) + (p2 >> 15) + (p3 >> 15) + 2;
            o[x] = (WORD)(((aa >> 2) << 15) | ((rr >> 2) << 10) | ((gg >> 2) << 5) | (bb >> 2));
        }
    }
}

// ---------------------------------------------------------------- the threads
// One job at a time (the game thread makes one texture after another). A job is rows: first the n rows of cells of
// level 0, then - for the level that follows - bands of its rows. Helpers are woken through a semaphore; a helper
// says it is in before it looks at the job and the game thread leaves a job only when nobody is in, so a helper
// that wakes late finds either nothing or the next job, complete.
struct TpJob {
    TpMap s; int x0, y0, n, t, flag, W; WORD* bits;
    int kind;                                             // 0 cells, 1 halving a level
    const WORD* hsrc; int hsw; WORD* hdst; int hdw, hband, rows;
    volatile LONG next, open, inside, nOne, nTwo;
};
static TpJob g_tpJob; static HANDLE g_tpSem = NULL;
static void tpRow(TpJob& J, int j, DWORD* b1, DWORD* b2) {
    if (J.kind) { int y0 = j * J.hband, y1 = y0 + J.hband; if (y1 > J.hdw) y1 = J.hdw; tpHalveRows(J.hsrc, J.hsw, J.hdst, J.hdw, y0, y1); return; }
    int one = 0, two = 0;
    WORD* row = J.bits + (size_t)(J.n * J.t - 1 - j * J.t) * J.W;
    for (int i = 0; i < J.n; ++i) { int r = tpCell(J.s, J.x0 + i, J.y0 + j, J.t, J.flag, row + i * J.t, -J.W, b1, b2); one += r == 1; two += r == 2; }
    if (one) InterlockedExchangeAdd(&J.nOne, one);
    if (two) InterlockedExchangeAdd(&J.nTwo, two);
}
static DWORD WINAPI tpHelper(LPVOID) {
    __declspec(align(16)) DWORD b1[TP_MAXT * TP_MAXT], b2[TP_MAXT * TP_MAXT];
    for (;;) {
        WaitForSingleObject(g_tpSem, INFINITE);
        InterlockedIncrement(&g_tpJob.inside);
        if (g_tpJob.open) { LONG j; while ((j = InterlockedIncrement(&g_tpJob.next) - 1) < g_tpJob.rows) tpRow(g_tpJob, (int)j, b1, b2); }
        InterlockedDecrement(&g_tpJob.inside);
    }
}
static void tpRun(DWORD* b1, DWORD* b2) {                 // game thread: the job's fields are set; do it and come back when it is done
    TpJob& J = g_tpJob;
    J.next = 0;
    LONG wake = g_tpHelpers < J.rows - 1 ? g_tpHelpers : J.rows - 1;
    if (wake > 0) { InterlockedExchange(&J.open, 1); ReleaseSemaphore(g_tpSem, wake, NULL); }
    LONG j; while ((j = InterlockedIncrement(&J.next) - 1) < J.rows) tpRow(J, (int)j, b1, b2);
    if (wake > 0) { InterlockedExchange(&J.open, 0); while (J.inside) _mm_pause(); }
}
static void tpFillLevel0(const BYTE* map, int x0, int y0, int n, int t, int flag, WORD* bits, int W, LONG* nOne, LONG* nTwo) {
    __declspec(align(16)) DWORD b1[TP_MAXT * TP_MAXT], b2[TP_MAXT * TP_MAXT];
    TpJob& J = g_tpJob;
    tpSnap(J.s, map);
    if (!J.s.alpha[0]) {                                                          // not made yet: now, if a cell of this texture lays a blend
        if (tpLaysABlend(J.s, x0, y0, n, t, flag)) { ((void (__cdecl*)(void))(ULONG_PTR)TP_ALPHAINIT)(); tpSnap(J.s, map); }
    }
    J.x0 = x0; J.y0 = y0; J.n = n; J.t = t; J.flag = flag; J.W = W; J.bits = bits; J.kind = 0; J.rows = n; J.nOne = 0; J.nTwo = 0;
    tpRun(b1, b2);
    *nOne = J.nOne; *nTwo = J.nTwo;
}
static void tpHalve(const WORD* src, int sw, WORD* dst, int dw) {
    TpJob& J = g_tpJob;
    if (dw < 64) { tpHalveRows(src, sw, dst, dw, 0, dw); return; }
    J.kind = 1; J.hsrc = src; J.hsw = sw; J.hdst = dst; J.hdw = dw; J.hband = 16; J.rows = (dw + 15) / 16;
    tpRun(NULL, NULL);
}

// ---------------------------------------------------------------- the hook
struct TpDesc { DWORD Format, Type, Usage, Pool, MsType, MsQuality, Width, Height; };
struct TpLock { INT Pitch; void* pBits; };
static __forceinline void** tpVt(void* o) { return *(void***)o; }
static void* tpPeek(void* self) { return ((void* (__fastcall*)(void*, void*))(ULONG_PTR)TP_PEEK)(self, NULL); }
static LONG g_tpHarness = 0;                                               // the render-thread harness is calling: the game's code is not there
static void tpFail(HRESULT hr) { if (!g_tpHarness) ((void (__cdecl*)(HRESULT))(ULONG_PTR)TP_HRFAIL)(hr); }
// Is this a texture that nothing in the render queue names yet? Then each of its levels can be locked at once
// (aotr_rt.inc: rt_surf_13 locks a new texture's level directly). Without the render thread every lock is direct.
static __forceinline bool tpTexIsNew(void* tex) { return !g_rtActive || rtFreshHas((DWORD)(ULONG_PTR)tex); }
static WORD* g_tpMip = NULL; static size_t g_tpMipCap = 0;                 // the levels below level 0, one after the other (game thread)
static volatile LONG g_tpMipChecksLeft = 8, g_tpMipChecked = 0, g_tpMipBad = 0;
// the first few: D3DX itself makes the levels of a memory texture holding the same level 0, and they are compared
static int tpMipCheck(const WORD* l0, int W, DWORD levels, const WORD* mine) {
    if (!g_rtXO[12] || !txMemInitOnce()) return -1;
    TxMem* m = txMemCreate((DWORD)W, (DWORD)W, levels, 0, 0x19, 1);
    if (!m) return -1;
    for (int y = 0; y < W; ++y) memcpy(m->lv[0].bits + (size_t)y * m->lv[0].pitch, l0 + (size_t)y * W, (size_t)W * 2);
    int res = -1;
    if (((TxjFilterF)g_rtXO[12])(m, NULL, 0, 5) == 0) {
        res = 1; const WORD* q = mine;
        for (DWORD l = 1; l < m->levels && res == 1; ++l) {
            int dw = W >> l;
            for (int y = 0; y < dw; ++y) if (memcmp(m->lv[l].bits + (size_t)y * m->lv[l].pitch, q + (size_t)y * dw, (size_t)dw * 2)) { res = 0; break; }
            q += (size_t)dw * dw;
        }
    }
    txMemFree(m);
    return res;
}
// the first textures, made a second time by the engine's own cell function into `ref` (which holds what the level
// held before) and compared
static bool tpCheckAgainstEngine(BYTE* map, int x0, int y0, int n, int t, int flag, const WORD* mine, WORD* ref, int W) {
    tTpCell cell = (tTpCell)(ULONG_PTR)TP_CELL;
    for (int j = 0; j < n; ++j) for (int i = 0; i < n; ++i) {
        WORD* dest = ref + (size_t)(n * t - 1 - j * t) * W + i * t;
        const DWORD* p = cell(map, NULL, x0 + i, y0 + j, t, flag, dest, -W);
        if (p) tpNarrow(p, dest, t, -W);
    }
    return memcmp(mine, ref, (size_t)W * W * 2) == 0;
}
static int tpUpdate(void* self, BYTE* map, int x0, int y0, int n, int t, int flag, void* tex, const WORD* given = NULL, int* ownOut = NULL);
static volatile LONG g_tpBusy = 0;                                         // one at a time (the job, the levels' buffer); a second caller gets the engine's code
static int __fastcall hkTpUpdate(void* self, void* edx, BYTE* map, int x0, int y0, int n, int t, int flag) {
    if (!g_tpOn || !(t == 8 || t == 16 || t == 32) || n <= 0 || n > 128 || !map) { g_tpNOther++; return o_tpUpdate(self, edx, map, x0, y0, n, t, flag); }
    void* tex = tpPeek(self);
    if (!tex || InterlockedCompareExchange(&g_tpBusy, 1, 0) != 0) { g_tpNOther++; return o_tpUpdate(self, edx, map, x0, y0, n, t, flag); }
    int r = tpUpdate(self, map, x0, y0, n, t, flag, tex);
    InterlockedExchange(&g_tpBusy, 0);
    return r;
}
// given / ownOut: the harness's way in - level 0 is copied from `given` instead of composed, and whether the levels
// below were written is handed back instead of D3DX being called for them here.
static int tpUpdate(void* self, BYTE* map, int x0, int y0, int n, int t, int flag, void* tex, const WORD* given, int* ownOut) {
    LONG64 q0 = qpcNow();
    void* surf = NULL;
    HRESULT hr = ((HRESULT (__stdcall*)(void*, UINT, void**))tpVt(tex)[18])(tex, 0, &surf);
    if (hr) tpFail(hr);
    TpDesc d; memset(&d, 0, sizeof(d));
    hr = ((HRESULT (__stdcall*)(void*, TpDesc*))tpVt(surf)[12])(surf, &d);
    if (hr) tpFail(hr);
    if ((int)d.Width != n * t) return 0;                                          // (the engine leaves the level's reference behind here as well)
    TpLock lr = { 0, NULL };
    hr = ((HRESULT (__stdcall*)(void*, TpLock*, const RECT*, DWORD))tpVt(surf)[13])(surf, &lr, NULL, 0);
    if (hr) tpFail(hr);
    const int W = (int)d.Width;
    bool own = false;                                                             // the levels below are written here too (else D3DX makes them, as before)
    DWORD levels = 1;
    if (d.Format == 0x19 && lr.pBits) {
        LONG chk = (!given && g_tpChecksLeft > 0) ? InterlockedDecrement(&g_tpChecksLeft) : -1;
        WORD* ref = NULL;
        if (chk >= 0) { ref = (WORD*)VirtualAlloc(NULL, (size_t)W * W * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); if (ref) memcpy(ref, lr.pBits, (size_t)W * W * 2); }
        LONG one = 0, two = 0;
        LONG64 c0 = qpcNow();
        if (given) memcpy(lr.pBits, given, (size_t)W * W * 2);
        else tpFillLevel0(map, x0, y0, n, t, flag, (WORD*)lr.pBits, W, &one, &two);
        LONG64 c1 = qpcNow();
        g_tpNCells += n * n; g_tpNBlended += two;
        if (ref) {
            bool same = tpCheckAgainstEngine(map, x0, y0, n, t, flag, (const WORD*)lr.pBits, ref, W);
            LONG64 c2 = qpcNow();
            g_tpCheckMine += c1 - c0; g_tpCheckEngine += c2 - c1; g_tpChecked++;
            if (!same) {                                                          // the engine's result stands, and its code from here on
                memcpy(lr.pBits, ref, (size_t)W * W * 2);
                g_tpCheckBad++; InterlockedExchange(&g_tpOn, 0);
                logf("terrain: a patch texture (%d x %d cells of %d px at %d,%d, set %d) came out DIFFERENT from the engine's own - the engine's stands and its own code makes them from now on.", n, n, t, x0, y0, flag);
            }
            VirtualFree(ref, 0, MEM_RELEASE);
        }
        // the levels below: only for a square texture nothing in the queue names yet, whose levels can be written at
        // once. They are made now, from level 0 while it is still here, and written when it has been handed back.
        if (g_tpOn && d.Height == d.Width && W >= 2 && W <= 4096 && !(W & (W - 1)) && tpTexIsNew(tex)) {
            levels = ((DWORD (__stdcall*)(void*))tpVt(tex)[13])(tex);             // GetLevelCount
            size_t need = (size_t)W * W / 3 + 16;                                  // W*W/4 + W*W/16 + ... pixels
            if (g_tpMipCap < need) { if (g_tpMip) VirtualFree(g_tpMip, 0, MEM_RELEASE); g_tpMip = (WORD*)VirtualAlloc(NULL, need * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); g_tpMipCap = g_tpMip ? need : 0; }
            if (levels > 1 && levels <= 13 && g_tpMip) {
                const WORD* src = (const WORD*)lr.pBits; int sw = W; WORD* o = g_tpMip;
                for (DWORD l = 1; l < levels && sw > 1; ++l) { int dw = sw / 2; tpHalve(src, sw, o, dw); src = o; o += (size_t)dw * dw; sw = dw; }
                own = true;
                if (g_tpMipChecksLeft > 0 && InterlockedDecrement(&g_tpMipChecksLeft) >= 0) {
                    int r = tpMipCheck((const WORD*)lr.pBits, W, levels, g_tpMip);
                    if (r >= 0) { g_tpMipChecked++; if (!r) { g_tpMipBad++; g_tpMipChecksLeft = 0;
                        logf("terrain: the smaller levels of a patch texture came out DIFFERENT from D3DX's - D3DX makes them from now on."); } }
                }
                if (g_tpMipBad) own = false;
            }
        }
    }
    ((HRESULT (__stdcall*)(void*))tpVt(surf)[14])(surf);                          // UnlockRect
    ((ULONG (__stdcall*)(void*))tpVt(surf)[2])(surf);                             // Release
    if (own) {
        const WORD* q = g_tpMip; int sw = W;
        for (DWORD l = 1; l < levels && sw > 1; ++l) {
            int dw = sw / 2;
            void* ls = NULL; TpLock ll = { 0, NULL };
            hr = ((HRESULT (__stdcall*)(void*, UINT, void**))tpVt(tex)[18])(tex, l, &ls);
            if (hr || !ls) { own = false; break; }
            hr = ((HRESULT (__stdcall*)(void*, TpLock*, const RECT*, DWORD))tpVt(ls)[13])(ls, &ll, NULL, 0);
            if (hr || !ll.pBits || ll.Pitch < dw * 2) { if (!hr) ((HRESULT (__stdcall*)(void*))tpVt(ls)[14])(ls); ((ULONG (__stdcall*)(void*))tpVt(ls)[2])(ls); own = false; break; }
            for (int y = 0; y < dw; ++y) memcpy((BYTE*)ll.pBits + (size_t)y * ll.Pitch, q + (size_t)y * dw, (size_t)dw * 2);
            ((HRESULT (__stdcall*)(void*))tpVt(ls)[14])(ls);
            ((ULONG (__stdcall*)(void*))tpVt(ls)[2])(ls);
            q += (size_t)dw * dw; sw = dw;
        }
        if (own) g_tpNOwnMips++;
    }
    if (ownOut) *ownOut = own ? 1 : 0;
    else if (!own) {                                                              // (also when a level could not be had: D3DX then makes all of them from level 0, which is what stands)
        hr = ((HRESULT (__stdcall*)(void*, const void*, UINT, DWORD))(ULONG_PTR)TP_FILTER)(tpPeek(self), NULL, 0, 5);
        if (hr) tpFail(hr);
    }
    g_tpNTex++; g_tpTicks += qpcNow() - q0;
    return (int)d.Height;
}

// 0x004AE3CD makes the texture object, has D3DX create the texture and calls the update above. Timed as a whole, so
// that the log says what is left around the update (the creation: DXVK clears a new texture's memory).
typedef void* (__fastcall* tTpMake)(BYTE* map, void* edx, void* out, int x, int y, int n, int t, DWORD fmt, int flag);
static tTpMake o_tpMake = NULL;
static volatile LONG g_tpNMake = 0; static volatile LONG64 g_tpMakeTicks = 0;
static void* __fastcall hkTpMake(BYTE* map, void* edx, void* out, int x, int y, int n, int t, DWORD fmt, int flag) {
    LONG64 q0 = qpcNow();
    void* r = o_tpMake(map, edx, out, x, y, n, t, fmt, flag);
    g_tpMakeTicks += qpcNow() - q0; g_tpNMake++;
    return r;
}
static DWORD WINAPI tpReport(LPVOID) {
    LONG lastTex = 0, lastCells = 0, lastBl = 0, lastOther = 0, lastOwn = 0, saidCheck = 0, lastMake = 0; LONG64 lastT = 0, lastMakeT = 0;
    for (;;) {
        Sleep(5000);
        LONG tex = g_tpNTex, cells = g_tpNCells, bl = g_tpNBlended, oth = g_tpNOther, own = g_tpNOwnMips; LONG64 tk = g_tpTicks;
        LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
        if (!saidCheck && g_tpChecked > 0 && (g_tpChecksLeft <= 0 || g_tpCheckBad)) {
            saidCheck = 1;
            logf("terrain: the first %d patch textures were built a second time by the engine's own cell function: %s. The engine's way took %d.%03d ms for them, this one %d.%03d ms (%d helper threads).",
                 (int)g_tpChecked, g_tpCheckBad ? "NOT the same - switched off" : "every byte the same",
                 (int)(g_tpCheckEngine * 1000 / f), (int)((g_tpCheckEngine * 1000000 / f) % 1000), (int)(g_tpCheckMine * 1000 / f), (int)((g_tpCheckMine * 1000000 / f) % 1000), (int)g_tpHelpers);
        }
        { static LONG saidMip = 0;
          if (!saidMip && g_tpMipChecked > 0 && (g_tpMipChecksLeft <= 0 || g_tpMipBad)) { saidMip = 1;
              logf("terrain: the smaller levels of the first %d patch textures were also made by D3DX from the same level 0: %s.", (int)g_tpMipChecked, g_tpMipBad ? "NOT the same - D3DX makes them" : "every byte the same"); } }
        LONG mk = g_tpNMake; LONG64 mkT = g_tpMakeTicks;
        if (tex != lastTex || oth != lastOther) {
            LONG dTex = tex - lastTex, dMk = mk - lastMake; LONG64 dT = tk - lastT, dMkT = mkT - lastMakeT;
            logf("terrain: last 5 s - %d patch textures built (%d cells, %d of them composed from blends) in %d.%03d ms of the game thread, %d.%03d ms each; the smaller levels written directly for %d of them; left to the engine's own code: %d | with creating the texture: %d made in %d.%03d ms, %d.%03d ms each",
                 (int)dTex, (int)(cells - lastCells), (int)(bl - lastBl), (int)(dT * 1000 / f), (int)((dT * 1000000 / f) % 1000),
                 dTex ? (int)(dT * 1000 / f / dTex) : 0, dTex ? (int)((dT * 1000000 / f / dTex) % 1000) : 0, (int)(own - lastOwn), (int)(oth - lastOther),
                 (int)dMk, (int)(dMkT * 1000 / f), (int)((dMkT * 1000000 / f) % 1000), dMk ? (int)(dMkT * 1000 / f / dMk) : 0, dMk ? (int)((dMkT * 1000000 / f / dMk) % 1000) : 0);
        }
        lastTex = tex; lastCells = cells; lastBl = bl; lastOther = oth; lastOwn = own; lastT = tk; lastMake = mk; lastMakeT = mkT;
    }
}

static DWORD tpFnv(const BYTE* p, const BYTE* e) { DWORD h = 0x811C9DC5u; for (; p < e; ++p) h = (h ^ *p) * 0x01000193u; return h; }
static bool tpCodeIsKnown() {
    static const DWORD k[][3] = {
        { 0x004EEC82, 0x004EEE64, 0xD3CCD2B0 }, { 0x004AE772, 0x004AE91E, 0x6D581CFA }, { 0x004AC035, 0x004AC0EF, 0x07A3A18B },
        { 0x004AC284, 0x004AC3D0, 0x3994A763 }, { 0x004AD85F, 0x004AD8BA, 0xFABF0722 }, { 0x004AB66F, 0x004AB6C5, 0x70014604 },
        { 0x004AB717, 0x004AB74D, 0x867CBA88 }, { 0x00511131, 0x00511138, 0x3248F96C }, { 0x00511161, 0x00511190, 0x0E209DDA },
        { 0x005321DC, 0x00532201, 0xC391F4AE },
    };
    __try {
        for (int i = 0; i < (int)(sizeof(k) / sizeof(k[0])); ++i)
            if (tpFnv((const BYTE*)(ULONG_PTR)k[i][0], (const BYTE*)(ULONG_PTR)k[i][1]) != k[i][2]) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
static int tpAutoHelpers() {
    SYSTEM_INFO si; GetSystemInfo(&si);
    int n = (int)si.dwNumberOfProcessors / 2 - 2;                                 // the game thread works too; the render thread and the texture workers keep theirs
    return n < 1 ? 1 : n > 6 ? 6 : n;
}
static bool tpStartThreads(int helpers) {
    if (helpers < 0) helpers = tpAutoHelpers();
    if (helpers > 15) helpers = 15;
    if (!g_tpSem) g_tpSem = CreateSemaphoreA(NULL, 0, 0x7FFFFFFF, NULL);
    if (!g_tpSem) return false;
    int made = 0;
    for (int i = 0; i < helpers; ++i) { HANDLE h = CreateThread(NULL, 256 * 1024, tpHelper, NULL, 0, NULL); if (h) { CloseHandle(h); ++made; } }
    g_tpHelpers = made;
    return true;
}
static void installTerrain() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    int want = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES ? (int)GetPrivateProfileIntA("Engine", "TerrainThreads", -1, ini) : -1;
    char env[8]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_TERRAIN", env, sizeof(env)) && env[0]) want = env[0] == '0' ? 0 : want;
    if (want == 0) { logf("terrain: off - patch textures are built by the engine's own code, cell by cell on the game thread."); return; }
    if (!tpCodeIsKnown()) { logf("terrain: the patch texture code is not what this build has - the engine's own code builds them."); return; }
    if (!tpStartThreads(want)) { logf("terrain: no threads - off."); return; }
    o_tpUpdate = (tTpUpdate)makeTrampoline((BYTE*)TP_UPDATE, 6);
    if (!o_tpUpdate) { logf("terrain: no trampoline - off."); return; }
    static const BYTE kMake[10] = { 0xB8, 0x3C, 0x52, 0xB7, 0x00, 0xE8, 0x19, 0xEB, 0x58, 0x00 };          // mov eax, 0xB7523C; call the frame set-up
    bool makeOk = false;
    __try { makeOk = !memcmp((void*)(ULONG_PTR)0x004AE3CD, kMake, sizeof(kMake)) && *(const BYTE*)(ULONG_PTR)0x004AE4E7 == 0xC2 && *(const WORD*)(ULONG_PTR)0x004AE4E8 == 0x001C; } __except (EXCEPTION_EXECUTE_HANDLER) { makeOk = false; }
    if (makeOk) o_tpMake = (tTpMake)makeTrampoline((BYTE*)(ULONG_PTR)0x004AE3CD, 5);
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)TP_UPDATE, (void*)&hkTpUpdate, 6);
    if (o_tpMake) patchJmp((BYTE*)(ULONG_PTR)0x004AE3CD, (void*)&hkTpMake, 5);
    resumeAll(fz, nf);
    InterlockedExchange(&g_tpOn, 1);
    CreateThread(NULL, 0, tpReport, NULL, 0, NULL);
    logf("terrain: installed - the texture of each ground patch that comes into view is built on the game thread and %d helper threads at once, and its smaller levels are written directly (D3DX's box filter for this format is whole-number arithmetic; no worker job, no cache file). The first %d are built a second time by the engine's own code and compared.",
         (int)g_tpHelpers, (int)g_tpChecksLeft);
}

// The render-thread harness (rt_harness.exe ... terrain): a square A1R5G5B5 texture it has just created gets the
// given level 0 and its levels below exactly as the update above writes them. 1 = the levels were written here,
// 0 = the caller has D3DX make them, -1 = refused.
extern "C" __declspec(dllexport) int __cdecl AotrTerrainHarnessBuild(void* tex, const WORD* level0, int W, int mipCheck) {
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    if (!tex || !level0 || W < 2 || InterlockedCompareExchange(&g_tpBusy, 1, 0) != 0) return -1;
    if (!g_tpSem) tpStartThreads(3);
    LONG keepOn = g_tpOn, keepMip = g_tpMipChecksLeft; g_tpOn = 1; g_tpHarness = 1; g_tpMipChecksLeft = mipCheck ? 1 : 0;
    int own = 0;
    int r = tpUpdate(NULL, NULL, 0, 0, W, 1, 0, tex, level0, &own);
    g_tpOn = keepOn; g_tpHarness = 0; g_tpMipChecksLeft = keepMip;
    InterlockedExchange(&g_tpBusy, 0);
    return r > 0 ? own : -1;
}
extern "C" __declspec(dllexport) void __cdecl AotrTerrainHarnessStats(char* out, int n) {
    if (out && n > 200) sprintf(out, "terrain textures: %d built, the levels below written directly for %d; of those checked against D3DX on a memory texture: %d, different %d",
                                (int)g_tpNTex, (int)g_tpNOwnMips, (int)g_tpMipChecked, (int)g_tpMipBad);
}

// ---------------------------------------------------------------- offline test (terrain_test.exe: game.dat mapped at its base)
// A made-up map, made-up tiles and alpha tiles, a stand-in texture; the engine's own update at 0x004EEC82 and the
// hook above fill the same level 0 and must leave the same bytes; the levels below must be what the stand-in
// D3DXFilterTexture (the box rule, written out separately) makes of the engine's level 0.
struct TpTSurf { void** vt; struct TpTTex* tex; int level; };
struct TpTTex { void** vt; int w, levels; WORD* lv[12]; TpTSurf surf[12]; LONG locks, unlocks, releases, filters; };
static HRESULT __stdcall tptGetSurfaceLevel(TpTTex* t, UINT l, void** out) { if ((int)l >= t->levels) return 0x8876086C; *out = &t->surf[l]; return 0; }
static DWORD __stdcall tptLevelCount(TpTTex* t) { return (DWORD)t->levels; }
static HRESULT __stdcall tptGetDesc(TpTSurf* s, TpDesc* d) { memset(d, 0, sizeof(*d)); d->Format = 0x19; d->Type = 1; d->Pool = 1; d->Width = d->Height = (DWORD)(s->tex->w >> s->level); return 0; }
static HRESULT __stdcall tptLockRect(TpTSurf* s, TpLock* lr, const RECT*, DWORD) { lr->Pitch = (s->tex->w >> s->level) * 2; lr->pBits = s->tex->lv[s->level]; s->tex->locks++; return 0; }
static HRESULT __stdcall tptUnlockRect(TpTSurf* s) { s->tex->unlocks++; return 0; }
static ULONG __stdcall tptRelease(TpTSurf* s) { s->tex->releases++; return 1; }
static HRESULT __stdcall tptFilter(TpTTex* t, const void*, UINT, DWORD) {       // the box rule once more, plainly
    t->filters++;
    for (int l = 1; l < t->levels; ++l) {
        int dw = t->w >> l, sw = dw * 2; const WORD* s = t->lv[l - 1]; WORD* o = t->lv[l];
        for (int y = 0; y < dw; ++y) for (int x = 0; x < dw; ++x) {
            WORD p[4] = { s[(y * 2) * sw + x * 2], s[(y * 2) * sw + x * 2 + 1], s[(y * 2 + 1) * sw + x * 2], s[(y * 2 + 1) * sw + x * 2 + 1] };
            int r = 2, g = 2, b = 2, a = 0;
            for (int q = 0; q < 4; ++q) { r += (p[q] >> 10) & 31; g += (p[q] >> 5) & 31; b += p[q] & 31; a += p[q] >> 15; }
            o[y * dw + x] = (WORD)((a >= 2 ? 0x8000 : 0) | ((r / 4) << 10) | ((g / 4) << 5) | (b / 4));
        }
    }
    return 0;
}
static bool __fastcall tptInited(void*, void*) { return true; }
static DWORD g_tptRng = 0x12345u;
static DWORD tptRnd() { g_tptRng ^= g_tptRng << 13; g_tptRng ^= g_tptRng >> 17; g_tptRng ^= g_tptRng << 5; return g_tptRng; }
extern "C" __declspec(dllexport) int __cdecl AotrTerrainTest(char* out, int nOut) {
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    bool mapped = false;
    __try { mapped = *(const BYTE*)(ULONG_PTR)TP_UPDATE == 0x55 && tpCodeIsKnown(); } __except (EXCEPTION_EXECUTE_HANDLER) { mapped = false; }
    if (!mapped) { if (out && nOut > 80) sprintf(out, "terrain: the game's code is not mapped in this process - not run here (terrain_test.exe runs it)"); return 0; }
    int fail = 0;
    // what the engine's code reaches for outside itself: the global data (+0x44), the twelve alpha tiles, D3DXFilterTexture and memcpy through its import slots
    static BYTE global[0x1000]; *(BYTE**)(ULONG_PTR)TP_GLOBAL = global;
    const int kTileBytes = 0x2AC0;
    BYTE* alphaTiles = (BYTE*)VirtualAlloc(NULL, 12 * kTileBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (int i = 0; i < 12 * kTileBytes; ++i) alphaTiles[i] = (BYTE)tptRnd();
    for (int i = 0; i < 12; ++i) *(BYTE**)(ULONG_PTR)(TP_ALPHATAB + i * 4) = alphaTiles + i * kTileBytes;
    *(void**)(ULONG_PTR)0x00BD0A20 = (void*)&tptFilter;                          // the slot 0x00A3ECD2 jumps through
    *(void**)(ULONG_PTR)0x00BD06C8 = (void*)&memcpy;                             // the slot 0x00A3CF22 jumps through
    // tiles: 40 of them, each with an alternate (the second texture set); some slots empty
    const int kTiles = 40;
    BYTE* tiles = (BYTE*)VirtualAlloc(NULL, 2 * kTiles * kTileBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (int i = 0; i < 2 * kTiles * kTileBytes; ++i) tiles[i] = (BYTE)tptRnd();
    // the map object: 0x120E8 bytes are enough for every field the code reads
    const int MW = 70, MH = 60, MS = MW * MH;
    BYTE* map = (BYTE*)VirtualAlloc(NULL, 0x13000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    short* tileNdx = (short*)VirtualAlloc(NULL, MS * 2 + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int* blendNdx = (int*)VirtualAlloc(NULL, MS * 4 + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int* flagArr = (int*)VirtualAlloc(NULL, MS * 4 + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int* extraNdx = (int*)VirtualAlloc(NULL, MS * 4 + 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    const int kBlends = 90; BYTE* blends = (BYTE*)VirtualAlloc(NULL, kBlends * 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    *(int*)(map + 8) = MW; *(int*)(map + 0xC) = MH; *(int*)(map + 0x20) = MS;
    *(short**)(map + 0x98) = tileNdx; *(int**)(map + 0x9C) = blendNdx; *(int**)(map + 0xA0) = flagArr; *(int**)(map + 0xA4) = extraNdx;
    *(BYTE**)(map + 0x80B0) = blends; *(BYTE**)(map + 0x80B4) = blends + kBlends * 16;
    // the stand-in texture (two of them: one for the engine, one for the hook) behind the wrapper the engine passes
    static void* texVt[32]; static void* surfVt[32]; static void* objVt[16];
    texVt[13] = (void*)&tptLevelCount; texVt[18] = (void*)&tptGetSurfaceLevel;
    surfVt[2] = (void*)&tptRelease; surfVt[12] = (void*)&tptGetDesc; surfVt[13] = (void*)&tptLockRect; surfVt[14] = (void*)&tptUnlockRect;
    objVt[10] = (void*)&tptInited; objVt[11] = (void*)&tptInited;
    static TpTTex T[2]; static BYTE texObj[2][0x40]; static BYTE inner[2][0x10]; static BYTE* wrap[2];
    for (int k = 0; k < 2; ++k) {
        T[k].vt = texVt; for (int l = 0; l < 12; ++l) { T[k].surf[l].vt = surfVt; T[k].surf[l].tex = &T[k]; T[k].surf[l].level = l; T[k].lv[l] = (WORD*)VirtualAlloc(NULL, 1024 * 1024 * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); }
        *(void***)texObj[k] = objVt; *(BYTE**)(texObj[k] + 0x14) = inner[k]; *(void**)(inner[k] + 8) = &T[k]; wrap[k] = texObj[k];
    }
    tTpUpdate engine = (tTpUpdate)(ULONG_PTR)TP_UPDATE;
    tTpUpdate keepOrig = o_tpUpdate; LONG keepOn = g_tpOn, keepChecks = g_tpChecksLeft, keepHelpers = g_tpHelpers;
    LONG keepMipChecks = g_tpMipChecksLeft; g_tpMipChecksLeft = 0;                  // (no D3DX in this process; mipprobe.exe and the game have it)
    o_tpUpdate = engine; g_tpOn = 1;
    if (!g_tpSem) { char e[8]; e[0] = 0; tpStartThreads(GetEnvironmentVariableA("AOTR_TPTEST_HELPERS", e, sizeof(e)) && e[0] ? atoi(e) : 5); }
    int cases = 0, bad = 0, badMips = 0, checkedInside = 0; LONG64 tEng = 0, tMine = 0; long cellsOne = 0, cellsTwo = 0;
    for (int round = 0; round < 260; ++round) {
        int t = round % 3 == 0 ? 32 : round % 3 == 1 ? 16 : 8;
        int n = (round % 5 == 4) ? 32 : (round % 7 == 3) ? 4 : 16;
        if (n * t > 1024) n = 1024 / t;
        int flag = (round % 4 == 1) ? 1 : 0;
        int style = round % 13;
        // the map's contents for this round
        *(int*)(map + 0x120E0) = style == 5 ? 3 : style == 6 ? -2 : 0; *(int*)(map + 0x120E4) = style == 5 ? -1 : style == 6 ? 4 : 0;
        *(int*)(global + 0x44) = (round % 3 == 2) ? 0 : 1;
        for (int i = 0; i < 0x1000; ++i) *(BYTE**)(map + 0xB0 + i * 4) = NULL;
        for (int i = 0; i < kTiles; ++i) {
            BYTE* tl = tiles + (size_t)(2 * i) * kTileBytes; BYTE* alt = tl + kTileBytes;
            *(BYTE**)(tl + 0x2AB4) = (style == 7 && (i % 5) == 0) ? NULL : alt; *(BYTE**)(alt + 0x2AB4) = NULL;
            *(BYTE**)(map + 0xB0 + i * 4) = (style == 8 && (i % 6) == 0) ? NULL : tl;
        }
        for (int i = 0; i < MS; ++i) {
            DWORD r = tptRnd();
            int tn = (int)(r % (kTiles * 4));
            if (style == 9 && (r >> 20) % 31 == 0) tn = 0x4000 + (int)((r >> 8) & 0xFF);          // an index past the tile table
            if (style == 10 && (r >> 20) % 29 == 0) tn = -1 - (int)((r >> 8) % 11);                // a negative one: the engine reads below the table (two slots), so does the replica
            tileNdx[i] = (short)tn;
            int blendShare = style == 0 ? 0 : style == 1 ? 100 : 45;
            blendNdx[i] = ((int)((r >> 12) % 100) < blendShare) ? 1 + (int)((r >> 4) % (kBlends + (style == 11 ? 6 : -1))) : (style == 12 && (r & 64) ? -3 : 0);
            DWORD r2 = tptRnd();
            extraNdx[i] = ((r2 % 100) < 30) ? 1 + (int)((r2 >> 8) % (kBlends + (style == 11 ? 4 : -1))) : (style == 12 && (r2 & 32) ? -7 : 0);
            flagArr[i] = ((r2 >> 16) % 9 == 0) ? 1 : 0;
        }
        *(BYTE**)(map + 0xAC) = (style == 10) ? tiles + (size_t)6 * kTileBytes : NULL; *(BYTE**)(map + 0xA8) = NULL;                 // what "below the table" holds
        for (int i = 0; i < kBlends; ++i) {
            BYTE* b = blends + i * 16; DWORD r = tptRnd(), r2 = tptRnd();
            *(short*)b = (short)(r % (kTiles * 4)); b[2] = b[3] = 0;
            b[4] = (r >> 8) % 4 == 0; b[5] = (r >> 10) % 3 == 0; b[6] = (r >> 12) % 2; b[7] = (r >> 13) % 2; b[8] = (r >> 14) % 2; b[9] = (r >> 15) % 2;
            *(int*)(b + 12) = (int)r2;
            if (style == 9 && i % 11 == 0) *(short*)b = 0x4100;
        }
        int x0 = (style == 2) ? -5 : (style == 3) ? MW - n / 2 : (int)(tptRnd() % (MW - 4));
        int y0 = (style == 2) ? -3 : (style == 4) ? MH - n / 2 : (int)(tptRnd() % (MH - 4));
        for (int k = 0; k < 2; ++k) { T[k].w = n * t; T[k].levels = 1; { int w = n * t; while (w > 1 && T[k].levels < 12) { w >>= 1; T[k].levels++; } }
                                      if (round % 2) T[k].levels = T[k].levels < 3 ? T[k].levels : 3;
                                      DWORD seed = 0xC0FFEE00u + round;
                                      for (int l = 0; l < T[k].levels; ++l) { int w = (n * t) >> l; for (int i = 0; i < w * w; ++i) { seed = seed * 1664525u + 1013904223u; T[k].lv[l][i] = (WORD)(seed >> 16); } } }
        g_tpChecksLeft = (round % 10 == 0) ? 1 : 0;                               // every tenth also runs the in-game check path
        LONG chkBefore = g_tpChecked;
        LONG64 a0 = qpcNow();
        int r0 = engine(&wrap[0], NULL, map, x0, y0, n, t, flag);
        LONG64 a1 = qpcNow();
        int r1 = hkTpUpdate(&wrap[1], NULL, map, x0, y0, n, t, flag);
        LONG64 a2 = qpcNow();
        ++cases;
        if (g_tpChecked != chkBefore) ++checkedInside; else { tEng += a1 - a0; tMine += a2 - a1; }
        if (r0 != r1 || memcmp(T[0].lv[0], T[1].lv[0], (size_t)(n * t) * (n * t) * 2)) { if (bad++ < 3 && out) { } }
        for (int l = 1; l < T[0].levels; ++l) { int w = (n * t) >> l; if (memcmp(T[0].lv[l], T[1].lv[l], (size_t)w * w * 2)) { ++badMips; break; } }
        if (!g_tpOn) { fail |= 4; g_tpOn = 1; }
    }
    cellsOne = g_tpNCells; cellsTwo = g_tpNBlended;
    if (bad) fail |= 1; if (badMips) fail |= 2; if (g_tpCheckBad) fail |= 8; if (T[1].filters != 0) fail |= 16; if (checkedInside != 26) fail |= 32;
    if (T[0].locks != cases || T[0].unlocks != cases || T[0].releases != cases) fail |= 64;
    // a control: one alpha value changed must show (the comparison is not blind)
    { int t = 32, n = 16; *(int*)(map + 0x120E0) = 0; *(int*)(map + 0x120E4) = 0;
      for (int i = 0; i < MS; ++i) { blendNdx[i] = 1 + (i % (kBlends - 1)); flagArr[i] = 0; tileNdx[i] = (short)(i % (kTiles * 4)); }
      for (int i = 0; i < kTiles; ++i) *(BYTE**)(map + 0xB0 + i * 4) = tiles + (size_t)(2 * i) * kTileBytes;
      for (int k = 0; k < 2; ++k) { T[k].w = n * t; T[k].levels = 3; }
      g_tpChecksLeft = 0;
      engine(&wrap[0], NULL, map, 2, 2, n, t, 0);
      for (int i = 0; i < 12; ++i) for (int j = 0x2008; j < 0x2808; j += 2) alphaTiles[i * kTileBytes + j] ^= 0x10;
      hkTpUpdate(&wrap[1], NULL, map, 2, 2, n, t, 0);
      if (!memcmp(T[0].lv[0], T[1].lv[0], (size_t)(n * t) * (n * t) * 2)) fail |= 128; }
    LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
    if (out && nOut > 400) sprintf(out, "terrain: %d patch textures (cells of 32, 16 and 8 px; both texture sets; blends, second blends, empty and out-of-range cells, indices outside the tables) built by the engine's own update and by the replica: level 0 differs in %d, the smaller levels in %d; the in-game check ran on %d and failed on %d; %ld cells, %ld of them composed | the engine's code %.1f ms, the replica with its levels %.1f ms (%d helper threads) | control: %s - %s (flags %02X)",
                                cases, bad, badMips, checkedInside, (int)g_tpCheckBad, cellsOne, cellsTwo, tEng * 1000.0 / f, tMine * 1000.0 / f, (int)g_tpHelpers,
                                (fail & 128) ? "a changed alpha tile was NOT noticed" : "a changed alpha tile shows", fail ? "FAILED" : "identical", fail);
    o_tpUpdate = keepOrig; g_tpOn = keepOn; g_tpChecksLeft = keepChecks; g_tpMipChecksLeft = keepMipChecks; (void)keepHelpers;
    return fail;
}
