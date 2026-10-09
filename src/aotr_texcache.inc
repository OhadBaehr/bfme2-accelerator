// aotr_texcache.inc - what D3DX made of a texture, kept on disk (v55, 2026-10-07).
//
// The texture workers (aotr_texjobs.inc) took D3DX's decoding off the game thread, but the work itself is still
// done every time: ten milliseconds for an ordinary texture file, fifty to seventy for a large one, because most of
// the mod's .dds files carry one mip level and D3DX box-filters and DXT-compresses the rest. The render thread
// meets each texture's upload at its place in the queue and waits there until the worker is done. In the seconds
// after a save load (log of 2026-10-07: 538 texture files in 5 s, 5.5 s of D3DX work on 8 workers) it stood waiting
// 1.39 s of those 5 s - no picture can be drawn meanwhile - and every game-thread call that needs the queue empty
// waited behind it (50 ms a time: the hitches in a pan).
//
// D3DX's output for a step is a pure function of what it is given: the same bytes in, the same call, the same
// floating-point control word - the same bytes out (that is what texjob_test.exe measures on every texture file of
// the game and the mod). So the output is kept: after a step has run, the levels it wrote go into a file named by
// a 128-bit hash of everything the step depends on -
//     the kind of step and all of its arguments, the x87 control word and MXCSR it runs under, the texture's
//     size, level count and format, this d3dx9_27.dll (link time and image size), and every input byte (the file
//     for a whole-file step, the pixels for a surface load, the source level for a mip chain)
// - and the next time a step with that hash comes up, a worker reads the file into the memory texture instead of
// running D3DX: a millisecond instead of ten or sixty. What reaches the real texture is byte for byte what D3DX
// wrote the first time; [Engine] TextureCheck compares cached results with a fresh D3DX run on a real texture exactly
// as it does computed ones.
//
// The file: a header (the key again, the shape, the levels kept, a checksum of the content) and the levels as they
// lie in the memory texture. A file whose header, size or checksum is not right is ignored and the step runs. Files
// are written under another name and renamed, so a half-written file never has a result's name. Only steps that
// took D3DX a millisecond or more are kept. [Engine] TextureCache = 0 switches it off, TextureCacheMB (default
// 6000) is the most the folder may hold: when it is larger at start, the files not used for longest are deleted.
// The folder is "texcache" next to the DLL; deleting it costs nothing but the time to fill it again.
#define TXC_MAGIC  0x43585441u                             // "ATXC"
#define TXC_VER    1u
#define TXC_IXN    (1u << 17)
#define TXC_MIN_US 1000                                    // steps faster than this are not worth a file

struct TxcH { DWORD h1, h2, h3, h4, total, nbuf; BYTE buf[16]; };
struct TxcKey { DWORD h[4]; };
struct TxcHdr { DWORD magic, ver, key[4], kind, fmt, w, h, levels, first, last, payBytes, hr, sum[2], pad[3]; };       // 80 bytes
struct TxcIx { DWORD k0, k1, kb; };

static volatile LONG g_txcOn = 0, g_txcReady = 0;
static char g_txcDir[MAX_PATH];
static DWORD g_txcSalt = 0;
static TxcIx* g_txcIx = NULL;
static CRITICAL_SECTION g_txcCs;
static volatile LONG64 g_txcBytes = 0, g_txcCapBytes = 0;
static volatile LONG g_txcFiles = 0, g_txcNHit = 0, g_txcNMiss = 0, g_txcNStore = 0, g_txcNBad = 0, g_txcNFull = 0, g_txcNSmall = 0, g_txcNStoreFail = 0, g_txcNPruned = 0, g_txcNShape = 0;
static volatile LONG64 g_txcHitBytes = 0, g_txcStoreBytes = 0, g_txcLoadTicks = 0, g_txcStoreTicks = 0, g_txcHashTicks = 0, g_txcSavedTicks = 0;

// ---------------------------------------------------------------- the hash (four coupled 32-bit lanes, 16 bytes a round)
static __forceinline DWORD txcMix(DWORD h) { h ^= h >> 16; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16; return h; }
static void txcHInit(TxcH* s, DWORD seed) { s->h1 = seed; s->h2 = seed ^ 0x9E3779B9u; s->h3 = seed + 0x7F4A7C15u; s->h4 = ~seed; s->total = 0; s->nbuf = 0; }
static void txcHBlocks(TxcH* s, const BYTE* p, DWORD nblocks) {
    DWORD h1 = s->h1, h2 = s->h2, h3 = s->h3, h4 = s->h4;
    for (; nblocks; --nblocks, p += 16) {
        DWORD k1 = ((const DWORD*)p)[0], k2 = ((const DWORD*)p)[1], k3 = ((const DWORD*)p)[2], k4 = ((const DWORD*)p)[3];
        k1 *= 0x239B961Bu; k1 = _rotl(k1, 15); k1 *= 0xAB0E9789u; h1 ^= k1; h1 = _rotl(h1, 19); h1 += h2; h1 = h1 * 5 + 0x561CCD1Bu;
        k2 *= 0xAB0E9789u; k2 = _rotl(k2, 16); k2 *= 0x38B34AE5u; h2 ^= k2; h2 = _rotl(h2, 17); h2 += h3; h2 = h2 * 5 + 0x0BCAA747u;
        k3 *= 0x38B34AE5u; k3 = _rotl(k3, 17); k3 *= 0xA1E38B93u; h3 ^= k3; h3 = _rotl(h3, 15); h3 += h4; h3 = h3 * 5 + 0x96CD1C35u;
        k4 *= 0xA1E38B93u; k4 = _rotl(k4, 18); k4 *= 0x239B961Bu; h4 ^= k4; h4 = _rotl(h4, 13); h4 += h1; h4 = h4 * 5 + 0x32AC3B17u;
    }
    s->h1 = h1; s->h2 = h2; s->h3 = h3; s->h4 = h4;
}
static void txcHUpdate(TxcH* s, const void* data, DWORD n) {
    const BYTE* p = (const BYTE*)data; s->total += n;
    if (s->nbuf) {
        DWORD take = 16 - s->nbuf; if (take > n) take = n;
        memcpy(s->buf + s->nbuf, p, take); s->nbuf += take; p += take; n -= take;
        if (s->nbuf < 16) return;
        txcHBlocks(s, s->buf, 1); s->nbuf = 0;
    }
    DWORD nb = n >> 4;
    if (nb) { txcHBlocks(s, p, nb); p += nb << 4; n -= nb << 4; }
    if (n) { memcpy(s->buf, p, n); s->nbuf = n; }
}
static void txcHFinal(TxcH* s, DWORD out[4]) {
    if (s->nbuf) { memset(s->buf + s->nbuf, 0, 16 - s->nbuf); txcHBlocks(s, s->buf, 1); }      // the length below tells a short tail from zeros
    DWORD h1 = s->h1 ^ s->total, h2 = s->h2 ^ _rotl(s->total, 8), h3 = s->h3 ^ _rotl(s->total, 16), h4 = s->h4 ^ _rotl(s->total, 24);
    h1 += h2; h1 += h3; h1 += h4; h2 += h1; h3 += h1; h4 += h1;
    h1 = txcMix(h1); h2 = txcMix(h2); h3 = txcMix(h3); h4 = txcMix(h4);
    h1 += h2; h1 += h3; h1 += h4; h2 += h1; h3 += h1; h4 += h1;
    out[0] = h1; out[1] = h2; out[2] = h3; out[3] = h4;
}
static void txcHashOf(const void* p, DWORD n, DWORD seed, DWORD out[4]) { TxcH s; txcHInit(&s, seed); txcHUpdate(&s, p, n); txcHFinal(&s, out); }

// ---------------------------------------------------------------- the key of a step
// `a`: the step's ten argument words; `in`/`inBytes`: its input bytes, or NULL for a mip chain (the input is the
// memory texture's level `srcLevel`, read row by row so that padding between rows never counts).
static void txcKeyOf(TxcKey* k, DWORD kind, const DWORD* a, WORD cw, DWORD mxcsr, const TxMem* m, const void* in, DWORD inBytes, DWORD srcLevel) {
    LONG64 t0 = qpcNow();
    TxcH s; txcHInit(&s, 0x55AA1234u ^ g_txcSalt);
    DWORD head[24]; memset(head, 0, sizeof(head));
    head[0] = TXC_VER; head[1] = g_txcSalt; head[2] = kind;
    for (int i = 0; i < 10; ++i) head[3 + i] = a[i];
    head[13] = (DWORD)(cw & 0x0F00);                        // precision and rounding control (the exception masks do not change a result)
    head[14] = mxcsr & 0xE040u;                             // rounding, flush-to-zero, denormals-are-zero
    head[15] = m->w; head[16] = m->h; head[17] = m->levels; head[18] = m->fmt; head[19] = inBytes; head[20] = in ? 0xFFFFFFFFu : srcLevel;
    txcHUpdate(&s, head, sizeof(head));
    if (in) txcHUpdate(&s, in, inBytes);
    else if (srcLevel < m->levels) {
        const TxLevel& L = m->lv[srcLevel];
        if (L.pitch == L.rowBytes) txcHUpdate(&s, L.bits, L.size);
        else for (DWORD y = 0; y < L.rows; ++y) txcHUpdate(&s, L.bits + y * L.pitch, L.rowBytes);
    }
    txcHFinal(&s, k->h);
    InterlockedExchangeAdd64(&g_txcHashTicks, qpcNow() - t0);
}
static void txcPath(char* out, const TxcKey* k, const char* ext) {
    wsprintfA(out, "%s\\%02x\\%08x%08x%08x%08x%s", g_txcDir, (unsigned)(k->h[0] >> 24), (unsigned)k->h[0], (unsigned)k->h[1], (unsigned)k->h[2], (unsigned)k->h[3], ext);
}

// ---------------------------------------------------------------- which results are on disk
static int txcIxFind(DWORD k0, DWORD k1) {                 // g_txcCs held; the slot, or -1
    DWORD h = (k0 ^ (k1 * 2654435761u)) & (TXC_IXN - 1);
    for (int i = 0; i < 64; ++i) { TxcIx& e = g_txcIx[(h + i) & (TXC_IXN - 1)]; if (!e.kb) return -1; if (e.k0 == k0 && e.k1 == k1) return (int)((h + i) & (TXC_IXN - 1)); }
    return -1;
}
static bool txcIxAdd(DWORD k0, DWORD k1, DWORD bytes) {    // g_txcCs held
    DWORD h = (k0 ^ (k1 * 2654435761u)) & (TXC_IXN - 1), kb = (bytes + 1023) >> 10; if (!kb) kb = 1;
    for (int i = 0; i < 64; ++i) {
        TxcIx& e = g_txcIx[(h + i) & (TXC_IXN - 1)];
        if (e.kb && e.k0 == k0 && e.k1 == k1) return true;
        if (!e.kb) { e.k0 = k0; e.k1 = k1; e.kb = kb; return true; }
    }
    return false;                                           // this corner of the table is full: the file is found by trying it
}
// 1 on disk, 0 not, -1 not known (the folder is still being read, or the table had no room)
static int txcKnown(const TxcKey* k) {
    if (!g_txcReady) return -1;
    EnterCriticalSection(&g_txcCs);
    int at = txcIxFind(k->h[0], k->h[1]);
    LeaveCriticalSection(&g_txcCs);
    return at >= 0 ? 1 : (g_txcFiles >= (LONG)(TXC_IXN / 4) ? -1 : 0);
}

// ---------------------------------------------------------------- read / write
// the levels a step keeps: those of `mask` it wrote, if they lie in one run
static bool txcRun(const TxMem* m, DWORD mask, DWORD* first, DWORD* last, DWORD* bytes) {
    DWORD w = mask & (DWORD)m->written & ((1u << m->levels) - 1);
    if (!w) return false;
    DWORD f = 0; while (!((w >> f) & 1)) ++f;
    DWORD l = f; while (l + 1 < m->levels && ((w >> (l + 1)) & 1)) ++l;
    if (w != (((2u << l) - 1) & ~((1u << f) - 1))) return false;
    *first = f; *last = l; *bytes = (DWORD)(m->lv[l].bits + m->lv[l].size - m->lv[f].bits);
    return true;
}
static bool txcLoad(const TxcKey* k, TxMem* m, DWORD mask, DWORD kind, DWORD* hr) {
    int known = txcKnown(k);
    if (known == 0) { InterlockedIncrement(&g_txcNMiss); return false; }
    LONG64 t0 = qpcNow();
    char path[MAX_PATH + 64]; txcPath(path, k, ".txc");
    HANDLE f = CreateFileA(path, GENERIC_READ | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) { InterlockedIncrement(&g_txcNMiss); return false; }
    bool ok = false; TxcHdr h; DWORD got = 0;
    if (ReadFile(f, &h, sizeof(h), &got, NULL) && got == sizeof(h) && h.magic == TXC_MAGIC && h.ver == TXC_VER && !memcmp(h.key, k->h, 16) && h.kind == kind &&
        h.fmt == m->fmt && h.w == m->w && h.h == m->h && h.levels == m->levels && h.first <= h.last && h.last < m->levels) {
        DWORD kept = ((2u << h.last) - 1) & ~((1u << h.first) - 1);
        DWORD bytes = (DWORD)(m->lv[h.last].bits + m->lv[h.last].size - m->lv[h.first].bits);
        if ((kept & ~mask) == 0 && bytes == h.payBytes) {
            BYTE* dst = m->lv[h.first].bits; DWORD done = 0; bool rd = true;
            while (done < bytes && rd) { DWORD n = 0; rd = ReadFile(f, dst + done, bytes - done, &n, NULL) && n > 0; done += n; }
            if (rd && done == bytes) {
                DWORD sum[4]; txcHashOf(dst, bytes, 0x1F3D5B79u, sum);
                if (sum[0] == h.sum[0] && sum[1] == h.sum[1]) {
                    _InterlockedOr(&m->written, (LONG)kept);
                    *hr = h.hr; ok = true;
                    InterlockedExchangeAdd64(&g_txcSavedTicks, (LONG64)h.pad[0] * g_pqpf.QuadPart / 1000000);   // what D3DX took for it
                    FILETIME wt, now;                           // used: a file not touched for a day gets today's date (what the pruning goes by)
                    if (GetFileTime(f, NULL, NULL, &wt)) { GetSystemTimeAsFileTime(&now);
                        ULONGLONG a = ((ULONGLONG)wt.dwHighDateTime << 32) | wt.dwLowDateTime, b = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
                        if (b > a && b - a > 864000000000ull) SetFileTime(f, NULL, NULL, &now); }
                    InterlockedExchangeAdd64(&g_txcHitBytes, (LONG64)bytes);
                }
            }
        } else InterlockedIncrement(&g_txcNShape);
    }
    CloseHandle(f);
    if (ok) InterlockedIncrement(&g_txcNHit); else { InterlockedIncrement(&g_txcNBad); InterlockedIncrement(&g_txcNMiss); }
    InterlockedExchangeAdd64(&g_txcLoadTicks, qpcNow() - t0);
    return ok;
}
static void txcStore(const TxcKey* k, TxMem* m, DWORD mask, DWORD kind, DWORD hr, DWORD us) {
    DWORD first, last, bytes;
    if (!txcRun(m, mask, &first, &last, &bytes)) { InterlockedIncrement(&g_txcNShape); return; }
    if (txcKnown(k) == 1) return;                             // another step with the same input got there first
    if (rd64(&g_txcBytes) + (LONG64)bytes + (LONG64)sizeof(TxcHdr) > rd64(&g_txcCapBytes)) { InterlockedIncrement(&g_txcNFull); return; }
    LONG64 t0 = qpcNow();
    TxcHdr h; memset(&h, 0, sizeof(h));
    h.magic = TXC_MAGIC; h.ver = TXC_VER; memcpy(h.key, k->h, 16); h.kind = kind; h.fmt = m->fmt; h.w = m->w; h.h = m->h; h.levels = m->levels;
    h.first = first; h.last = last; h.payBytes = bytes; h.hr = hr; h.pad[0] = us;
    const BYTE* src = m->lv[first].bits;
    { DWORD sum[4]; txcHashOf(src, bytes, 0x1F3D5B79u, sum); h.sum[0] = sum[0]; h.sum[1] = sum[1]; }
    char dir[MAX_PATH + 16], tmp[MAX_PATH + 64], fin[MAX_PATH + 64];
    wsprintfA(dir, "%s\\%02x", g_txcDir, (unsigned)(k->h[0] >> 24)); CreateDirectoryA(dir, NULL);
    txcPath(fin, k, ".txc"); wsprintfA(tmp, "%s.%u.tmp", fin, (unsigned)GetCurrentThreadId());
    bool ok = false;
    HANDLE f = CreateFileA(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        ok = WriteFile(f, &h, sizeof(h), &wr, NULL) && wr == sizeof(h);
        for (DWORD done = 0; ok && done < bytes; ) { DWORD n = 0; ok = WriteFile(f, src + done, bytes - done, &n, NULL) && n > 0; done += n; }
        CloseHandle(f);
        if (ok) ok = MoveFileExA(tmp, fin, MOVEFILE_REPLACE_EXISTING) != 0;
        if (!ok) DeleteFileA(tmp);
    }
    if (ok) {
        EnterCriticalSection(&g_txcCs); txcIxAdd(k->h[0], k->h[1], bytes + sizeof(TxcHdr)); LeaveCriticalSection(&g_txcCs);
        InterlockedExchangeAdd64(&g_txcBytes, (LONG64)bytes + (LONG64)sizeof(TxcHdr)); InterlockedIncrement(&g_txcFiles);
        InterlockedIncrement(&g_txcNStore); InterlockedExchangeAdd64(&g_txcStoreBytes, (LONG64)bytes);
    } else InterlockedIncrement(&g_txcNStoreFail);
    InterlockedExchangeAdd64(&g_txcStoreTicks, qpcNow() - t0);
}

// ---------------------------------------------------------------- writing happens elsewhere
// A new file costs about two milliseconds here (create, write, rename - most of it the virus scanner looking at
// each new file; measured 2.2 ms a result over 1,400 results). On the decoding workers that would slow the very
// first load down by a third, so results are handed to one writer thread of low priority. What waits to be written
// keeps its memory texture alive; when more than TXC_Q_MAXMB of those are waiting, a result is simply not kept
// this time - it is decoded again at the next load and kept then.
#define TXC_QN 1024
#define TXC_Q_MAXMB 48
struct TxcJob { TxcKey key; TxMem* mem; DWORD mask, kind, hr, us, bytes; };
static TxcJob g_txcQ[TXC_QN];
static DWORD g_txcQHead = 0, g_txcQTail = 0;               // under g_txcCs
static volatile LONG64 g_txcQBytes = 0;
static volatile LONG g_txcNBusy = 0, g_txcQLen = 0;
static HANDLE g_txcQSem = NULL;
// true: the caller's reference on `m` now belongs to the writer
static bool txcStoreLater(const TxcKey* k, TxMem* m, DWORD mask, DWORD kind, DWORD hr, DWORD us) {
    if (!g_txcQSem) return false;
    bool in = false;
    EnterCriticalSection(&g_txcCs);
    if (g_txcQHead - g_txcQTail < TXC_QN && rd64(&g_txcQBytes) + (LONG64)m->memSize <= ((LONG64)TXC_Q_MAXMB << 20)) {
        TxcJob& j = g_txcQ[g_txcQHead++ & (TXC_QN - 1)];
        j.key = *k; j.mem = m; j.mask = mask; j.kind = kind; j.hr = hr; j.us = us; j.bytes = m->memSize;
        InterlockedExchangeAdd64(&g_txcQBytes, (LONG64)j.bytes); InterlockedIncrement(&g_txcQLen);
        in = true;
    }
    LeaveCriticalSection(&g_txcCs);
    if (in) ReleaseSemaphore(g_txcQSem, 1, NULL); else InterlockedIncrement(&g_txcNBusy);
    return in;
}
static DWORD WINAPI txcWriter(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_txcQSem, INFINITE);
        TxcJob j; bool have = false;
        EnterCriticalSection(&g_txcCs);
        if (g_txcQTail != g_txcQHead) { j = g_txcQ[g_txcQTail++ & (TXC_QN - 1)]; have = true; }
        LeaveCriticalSection(&g_txcCs);
        if (!have) continue;
        txcStore(&j.key, j.mem, j.mask, j.kind, j.hr, j.us);
        tx_tex_Release(j.mem);
        InterlockedExchangeAdd64(&g_txcQBytes, -(LONG64)j.bytes); InterlockedDecrement(&g_txcQLen);
    }
}

// ---------------------------------------------------------------- the folder at start: what is there, and not more than allowed
struct TxcFile { ULONGLONG when; DWORD bytes; DWORD h[4]; };
static int __cdecl txcFileCmp(const void* a, const void* b) { ULONGLONG x = ((const TxcFile*)a)->when, y = ((const TxcFile*)b)->when; return x < y ? -1 : (x > y ? 1 : 0); }
static bool txcHex8(const char* s, DWORD* out) {
    DWORD v = 0;
    for (int i = 0; i < 8; ++i) { char c = s[i]; DWORD d = (c >= '0' && c <= '9') ? (DWORD)(c - '0') : (c >= 'a' && c <= 'f') ? (DWORD)(c - 'a' + 10) : (c >= 'A' && c <= 'F') ? (DWORD)(c - 'A' + 10) : 16; if (d > 15) return false; v = (v << 4) | d; }
    *out = v; return true;
}
static DWORD WINAPI txcScan(LPVOID) {
    LONG64 t0 = qpcNow();
    DWORD cap = 1u << 17, n = 0; LONG stray = 0;
    TxcFile* fl = (TxcFile*)VirtualAlloc(NULL, cap * sizeof(TxcFile), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    LONG64 total = 0;
    for (int d = 0; d < 256 && fl; ++d) {
        char pat[MAX_PATH + 32]; wsprintfA(pat, "%s\\%02x\\*", g_txcDir, d);
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            int len = lstrlenA(fd.cFileName);
            TxcFile e; bool good = len == 36 && !lstrcmpiA(fd.cFileName + 32, ".txc") && txcHex8(fd.cFileName, &e.h[0]) && txcHex8(fd.cFileName + 8, &e.h[1]) && txcHex8(fd.cFileName + 16, &e.h[2]) && txcHex8(fd.cFileName + 24, &e.h[3]);
            if (!good || fd.nFileSizeHigh || fd.nFileSizeLow < sizeof(TxcHdr) || n >= cap) {                 // a leftover of a write that was cut short, or not ours
                if (len > 4 && !lstrcmpiA(fd.cFileName + len - 4, ".tmp")) { char p[MAX_PATH + 300]; wsprintfA(p, "%s\\%02x\\%s", g_txcDir, d, fd.cFileName); DeleteFileA(p); stray++; }
                continue;
            }
            e.when = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime; e.bytes = fd.nFileSizeLow;
            fl[n++] = e; total += e.bytes;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    LONG pruned = 0; LONG64 freed = 0; DWORD from = 0;
    LONG64 capB = rd64(&g_txcCapBytes);
    if (fl && total > capB) {                                  // too much: the files not used for longest go, down to four fifths
        qsort(fl, n, sizeof(TxcFile), txcFileCmp);
        while (from < n && total - freed > capB / 5 * 4) {
            TxcKey k; memcpy(k.h, fl[from].h, 16); char p[MAX_PATH + 64]; txcPath(p, &k, ".txc");
            if (DeleteFileA(p)) { freed += fl[from].bytes; pruned++; }
            ++from;
        }
    }
    EnterCriticalSection(&g_txcCs);
    LONG kept = 0;
    for (DWORD i = from; fl && i < n; ++i) { txcIxAdd(fl[i].h[0], fl[i].h[1], fl[i].bytes); kept++; }
    LeaveCriticalSection(&g_txcCs);
    InterlockedExchangeAdd64(&g_txcBytes, total - freed); InterlockedExchangeAdd(&g_txcFiles, kept); g_txcNPruned = pruned;
    if (fl) VirtualFree(fl, 0, MEM_RELEASE);
    InterlockedExchange(&g_txcReady, 1);
    char m[16];
    logf("texcache: %d decoded textures on disk (%d MB of at most %d MB) in %s - read in %s ms%s%s.", (int)kept, (int)((total - freed) >> 20), (int)(capB >> 20), g_txcDir, msStr(qpcNow() - t0, 1, m),
         pruned ? "; the folder was over its limit and the oldest were deleted" : "", stray ? "; leftovers of unfinished writes removed" : "");
    return 0;
}
static DWORD WINAPI txcReport(LPVOID) {
    LONG lHit = 0, lMiss = 0, lStore = 0; LONG64 lLoad = 0, lHitB = 0, lStoreB = 0, lSaved = 0;
    for (;;) {
        Sleep(5000);
        LONG hit = g_txcNHit, miss = g_txcNMiss, store = g_txcNStore;
        LONG64 load = rd64(&g_txcLoadTicks), hitB = rd64(&g_txcHitBytes), storeB = rd64(&g_txcStoreBytes), saved = rd64(&g_txcSavedTicks);
        if (hit - lHit + store - lStore < 8 && miss - lMiss < 32) continue;
        char m1[16], m2[16];
        logf("texcache: last 5 s - %d texture steps read from disk instead of decoded (%d KB in %s ms; decoding them took %s ms when they were made), %d not there and decoded, %d new results kept (%d KB) | on disk now %d files, %d MB%s%s",
             (int)(hit - lHit), (int)((hitB - lHitB) >> 10), msStr(load - lLoad, 1, m1), msStr(saved - lSaved, 1, m2), (int)(miss - lMiss), (int)(store - lStore), (int)((storeB - lStoreB) >> 10),
             (int)g_txcFiles, (int)(rd64(&g_txcBytes) >> 20), g_txcNFull ? " | THE FOLDER IS AT ITS LIMIT: new results are not kept (TextureCacheMB)" : "", g_txcNBad ? " | some files did not pass their check and were ignored" : "");
        if (g_txcNBusy) { static LONG lBusy = 0; LONG b = g_txcNBusy; if (b != lBusy) { logf("texcache: %d results were not kept because the writer had too much waiting (they are decoded again next time and kept then).", (int)(b - lBusy)); lBusy = b; } }
        lHit = hit; lMiss = miss; lStore = store; lLoad = load; lHitB = hitB; lStoreB = storeB; lSaved = saved;
    }
}
static void txcSetup(int on, int capMb, const char* dirOverride) {
    if (g_txcOn || on <= 0) { if (on <= 0) logf("texcache: off ([Engine] TextureCache = 0) - every texture is decoded every time."); return; }
    if (capMb < 64) capMb = 64;
    if (dirOverride && dirOverride[0]) lstrcpynA(g_txcDir, dirOverride, MAX_PATH - 64); else aotrPath(g_txcDir, "texcache");
    CreateDirectoryA(g_txcDir, NULL);
    DWORD attr = GetFileAttributesA(g_txcDir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) { logf("texcache: cannot make the folder %s - off.", g_txcDir); return; }
    g_txcIx = (TxcIx*)VirtualAlloc(NULL, TXC_IXN * sizeof(TxcIx), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_txcIx) { logf("texcache: no memory - off."); return; }
    InitializeCriticalSection(&g_txcCs);
    g_txcCapBytes = (LONG64)capMb << 20;
    HMODULE hx = GetModuleHandleA("d3dx9_27.dll"); if (!hx) hx = LoadLibraryA("d3dx9_27.dll");
    if (hx) { IMAGE_NT_HEADERS* nh = (IMAGE_NT_HEADERS*)((BYTE*)hx + ((IMAGE_DOS_HEADER*)hx)->e_lfanew); g_txcSalt = nh->FileHeader.TimeDateStamp ^ _rotl(nh->OptionalHeader.SizeOfImage, 7); }
    InterlockedExchange(&g_txcOn, 1);
    g_txcQSem = CreateSemaphoreA(NULL, 0, 0x7FFFFFFF, NULL);
    { HANDLE tw = g_txcQSem ? CreateThread(NULL, 0, txcWriter, NULL, 0, NULL) : NULL; if (tw) { SetThreadPriority(tw, THREAD_PRIORITY_LOWEST); CloseHandle(tw); } }
    HANDLE th = CreateThread(NULL, 0, txcScan, NULL, 0, NULL); if (th) { SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(th); } else InterlockedExchange(&g_txcReady, 1);
    CreateThread(NULL, 0, txcReport, NULL, 0, NULL);
}
static void installTexCache() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    bool have = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
    int on = have ? (int)GetPrivateProfileIntA("Engine", "TextureCache", 1, ini) : 1;
    int capMb = have ? (int)GetPrivateProfileIntA("Engine", "TextureCacheMB", 6000, ini) : 6000;
    char env[MAX_PATH]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_TEXCACHE", env, 8) && env[0]) on = env[0] - '0';
    env[0] = 0; if (GetEnvironmentVariableA("AOTR_TEXCACHEMB", env, 16) && env[0]) capMb = atoi(env);
    env[0] = 0; GetEnvironmentVariableA("AOTR_TEXCACHE_DIR", env, sizeof(env));
    txcSetup(on, capMb, env);
}
extern "C" __declspec(dllexport) void __cdecl AotrTexCacheTestSetup(int on, int capMb, const char* dir) { if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf); txcSetup(on, capMb, dir); }
extern "C" __declspec(dllexport) void __cdecl AotrTexCacheTestStats(char* out, int n) {
    char m1[16], m2[16], m3[16], m4[16], buf[900];
    for (int i = 0; i < 300 && !g_txcReady && g_txcOn; ++i) Sleep(10);
    for (int i = 0; i < 6000 && g_txcQLen > 0; ++i) Sleep(10);                 // what is still waiting to be written
    wsprintfA(buf, "texcache %s: %d steps read from disk (%d KB, %s ms), %d not there, %d kept (%d KB, %s ms writing), %d files failed their check, %d shapes not kept, %d too quick to keep, %d not kept for the limit, %d not kept because the writer was behind, %d writes failed; "
                   "hashing %s ms; the read steps took D3DX %s ms when they were made; on disk %d files, %d MB",
              g_txcOn ? "on" : "OFF", (int)g_txcNHit, (int)(rd64(&g_txcHitBytes) >> 10), msStr(rd64(&g_txcLoadTicks), 1, m1), (int)g_txcNMiss, (int)g_txcNStore, (int)(rd64(&g_txcStoreBytes) >> 10), msStr(rd64(&g_txcStoreTicks), 1, m2),
              (int)g_txcNBad, (int)g_txcNShape, (int)g_txcNSmall, (int)g_txcNFull, (int)g_txcNBusy, (int)g_txcNStoreFail, msStr(rd64(&g_txcHashTicks), 1, m3), msStr(rd64(&g_txcSavedTicks), 1, m4), (int)g_txcFiles, (int)(rd64(&g_txcBytes) >> 20));
    lstrcpynA(out, buf, n);
}
