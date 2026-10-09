// ================================================================ smooth shadow edges (v72, 2026-10-08)
// WHY SHADOWS ARE BUILT OF SQUARES. The game draws the scene as the sun sees it into one square picture of depths
// (the shadow map) and every pixel of the real picture asks that map whether something stands between it and the sun.
// The 166 pixel shaders that ask (terrain, objects, vegetation, water - Shaders.big) all do it the same way: the map
// is read at the pixel's place and at the three map points next to it, with no filtering (the effects set POINT on
// that sampler, fxprobe "samp"), each of the four depths is compared with the pixel's own (less 0.002), and the four
// yes/no answers are added up:
//      add r2.xy, t7, c4.zxyw / add r1.xy, t7, c4.yzxw / add r0.xy, t7, c4.wzyx       c4 = (0, 0, 1/size, 1/size)
//      texld r10, r2, s8 / texld r9, r1, s8 / texld r8, r0, s8 / texld r7, t7, s8
//      mov r7.y, r10.x / mov r7.z, r9.x / mov r7.w, r8.x
//      add r0.w, t7.z, c11.w            (depth - 0.002)
//      add r7, r7, -r0.w / cmp r7, r7, c11.y, c11.x / dp4 r0.w, -c12.w, r7     (how many of the four are lit: 0..4)
//      ... mul r1.w, r0.w, c13.x        (a quarter of that)
// Everything inside one map point gets the same four answers, so a shadow's edge is made of squares the size of a map
// point, in five shades. A map point is a third of a metre of ground on a default map, three or four screen pixels
// at 4K; on a map that stretches the map further, or in a scripted close-up, it is twenty.
//
// WHAT THIS DOES. When such a shader is first bound, a second one is made from it in which the instruction that adds
// the four answers up is replaced by a lookup of nine: the 3x3 map points around the pixel's own, each read at its
// exact centre and compared exactly as the game compares (same depth, same 0.002), and the nine answers weighted by
// where inside its map point the pixel lies - (1-f)/2, 1/2, f/2 along each axis. That is the average of four
// position-weighted lookups half a map point apart: the lit fraction changes evenly across a map point instead of
// jumping at its border, and a shadow's edge is a soft line whatever the magnification. The result is put, times
// four, where the game's sum was; everything after it in the shader runs unchanged. In the middle of a shadow or of a
// lit area all nine answers agree and the pixel comes out as before (to the last bit of an 8-bit colour or one off).
// The game's own four reads stay in the shader, unused (the driver's compiler drops them).
//
// The new shader needs more instructions and registers than ps_2_0 has (the terrain shader uses all 64 arithmetic
// slots), so it is marked ps_2_x - the same instruction set with the limits the card reports; every card that can
// run DXVK reports 512 slots and 32 registers. If the device refuses it, the game's own shader is used and one line
// says so.
//
// Only a shader that carries the names ShadowMapSampler and ShadowInfo and matches the lookup above piece by piece is
// touched; one that reads the map some other way is left as it is (and reported once). Offline proof: pcf_test.exe
// (every pixel shader of both Shaders.big: found, patched, accepted by the device; on a map that is all lit or all
// shadow the patched shader draws what the original draws; across a shadow edge it differs only within two map
// points of the edge, and there it lies between lit and shadow; the lookup alone against the same arithmetic done on
// the CPU).
// [Engine] SmoothShadows (default 0) / AOTR_SMOOTHSHADOWS.

struct SpInfo {
    DWORD sampler, cOff;                       // the shadow map's sampler; the constant with 1/size in z
    DWORD base, depthTok, biasTok;             // the register the map coordinates come in, the two operands of "depth + bias"
    DWORD dp4Dst; int dp4At;                   // where the sum of the four went
    DWORD temp0, kc[4];                        // the first of seven new registers, four new constants
    int taps, grew;
    char why[120];                             // not empty: the shader reads the shadow map but was not changed, and this is why
};

static __forceinline DWORD spType(DWORD t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }
static __forceinline DWORD spNum(DWORD t) { return t & 0x7FF; }
static __forceinline DWORD spDst(DWORD type, DWORD num, DWORD mask) { return 0x80000000u | ((type & 7) << 28) | ((type & 0x18) << 8) | (mask << 16) | num; }
static __forceinline DWORD spSrc(DWORD type, DWORD num, DWORD swz, DWORD mod) { return 0x80000000u | ((type & 7) << 28) | ((type & 0x18) << 8) | (swz << 16) | (mod << 24) | num; }
enum { SP_T = 0, SP_C = 2, SP_S = 10 };       // temp, constant, sampler
enum { SW_ID = 0xE4, SW_X = 0x00, SW_Y = 0x55, SW_Z = 0xAA, SW_W = 0xFF, SW_YZXW = 0xC9, SW_ZXYW = 0xD2, SW_WZYX = 0x1B };
struct SpIns { DWORD at, op, len; };

// code: a pixel shader's tokens (nTok of them, the end token included or not). out: room for the changed shader.
// Returns its length in tokens, or 0 when nothing was changed (info->why says why if the shader reads the shadow map).
static DWORD spPatch(const DWORD* code, DWORD nTok, DWORD* out, DWORD outCap, SpInfo* info) {
    memset(info, 0, sizeof(*info));
    if (nTok < 8 || (code[0] != 0xFFFF0200u && code[0] != 0xFFFF0201u && code[0] != 0xFFFF0300u)) return 0;
    const bool sm3 = code[0] == 0xFFFF0300u;
    const DWORD nConst = sm3 ? 224 : 32;
    static const DWORD kMaxIns = 1024;
    SpIns ins[kMaxIns]; DWORD nIns = 0, end = 0, firstIns = 0;
    BYTE cUsed[224]; memset(cUsed, 0, sizeof(cUsed));
    float defv[224][4]; BYTE defSet[224]; memset(defSet, 0, sizeof(defSet));
    int sampler = -1, cShadow = -1; DWORD maxTemp = 0; bool anyTemp = false;
    for (DWORD i = 1; i < nTok; ) {
        DWORD t = code[i];
        if (t == 0x0000FFFFu) { end = i; break; }
        DWORD op = t & 0xFFFF;
        if (op == 0xFFFE) {                                            // a comment; the constant table is one
            DWORD n = (t >> 16) & 0x7FFF;
            if (i + 1 + n > nTok) return 0;
            if (n >= 8 && code[i + 1] == 0x42415443u) {
                const BYTE* c = (const BYTE*)(code + i + 2); DWORD cb = (n - 1) * 4;
                DWORD nc = *(const DWORD*)(c + 12), ci = *(const DWORD*)(c + 16);
                for (DWORD k = 0; k < nc && ci + 20 * (k + 1) <= cb; ++k) {
                    const BYTE* e = c + ci + 20 * k;
                    DWORD name = *(const DWORD*)e; WORD set = *(const WORD*)(e + 4), idx = *(const WORD*)(e + 6), cnt = *(const WORD*)(e + 8);
                    if (name >= cb) continue;
                    const char* nm = (const char*)c + name; DWORD room = cb - name;
                    if (set == 2) for (DWORD r = idx; r < (DWORD)idx + cnt && r < 224; ++r) cUsed[r] = 1;
                    if (set == 3 && room > 16 && !strncmp(nm, "ShadowMapSampler", 17)) sampler = idx;
                    if (set == 2 && room > 10 && !strncmp(nm, "ShadowInfo", 11) && cnt >= 5) cShadow = idx;
                }
            }
            i += 1 + n; continue;
        }
        if (t & 0x80000000u) return 0;
        DWORD len = (t >> 24) & 0xF;
        if (i + 1 + len > nTok || nIns >= kMaxIns) return 0;
        if (!firstIns) firstIns = i;
        ins[nIns].at = i; ins[nIns].op = op; ins[nIns].len = len; ++nIns;
        if (op == 0x51 && len == 5) {                                  // def
            DWORD r = spNum(code[i + 1]);
            if (spType(code[i + 1]) == SP_C && r < 224) { cUsed[r] = 1; defSet[r] = 1; memcpy(defv[r], code + i + 2, 16); }
        } else if (op != 0x1F && op != 0x30 && op != 0x2F) {
            for (DWORD k = 1; k <= len; ++k) {
                DWORD p = code[i + k];
                if (!(p & 0x80000000u)) continue;
                DWORD ty = spType(p), nu = spNum(p);
                if (ty == SP_T) { anyTemp = true; if (nu > maxTemp) maxTemp = nu; }
                else if (ty == SP_C && nu < 224) cUsed[nu] = 1;
            }
        }
        i += 1 + len;
    }
    if (!end || sampler < 0) return 0;                                 // not a shader that reads the shadow map
#define SP_NO(text) do { lstrcpynA(info->why, text, sizeof(info->why)); return 0; } while (0)
    if (cShadow < 0 || cShadow + 4 >= (int)nConst) SP_NO("it has the shadow map's sampler but not the ShadowInfo constants");
    const DWORD cOff = (DWORD)cShadow + 4;
    info->sampler = (DWORD)sampler; info->cOff = cOff;

    // the four reads of the map
    int tap[8]; int nTap = 0;
    for (DWORD k = 0; k < nIns; ++k) {
        const DWORD* a = code + ins[k].at;
        if (ins[k].op == 0x42 && ins[k].len == 3 && spType(a[3]) == SP_S && spNum(a[3]) == (DWORD)sampler) { if (nTap < 8) tap[nTap] = (int)k; ++nTap; }
    }
    info->taps = nTap;
    if (nTap != 4) SP_NO("it does not read the shadow map exactly four times");
    for (int k = 0; k < 4; ++k) if ((code[ins[tap[k]].at] >> 16) & 0xFF) SP_NO("a read of the shadow map is not a plain texld");
    // their coordinates: one is the base register itself, three are base + (0 or 1/size, 0 or 1/size)
    DWORD base = 0; int nDirect = 0, nOffset = 0, iFirst = tap[0];
    for (int k = 0; k < 4; ++k) {
        DWORD c = code[ins[tap[k]].at + 2], b = c; bool off = false;
        if (spType(c) == SP_T) {
            for (int j = tap[k] - 1; j >= 0; --j) {
                const DWORD* a = code + ins[j].at;
                if (ins[j].op == 0x51 || ins[j].op == 0x1F || ins[j].len < 1) continue;
                if (spType(a[1]) != SP_T || spNum(a[1]) != spNum(c) || !((a[1] >> 16) & 3)) continue;
                if (ins[j].op == 2 && ins[j].len == 3 && ((a[1] >> 16) & 0xF) == 3) {
                    int ci = (spType(a[2]) == SP_C && spNum(a[2]) == cOff) ? 2 : (spType(a[3]) == SP_C && spNum(a[3]) == cOff) ? 3 : 0;
                    if (ci) { b = a[5 - ci]; off = true; if (j < iFirst) iFirst = j; }
                }
                break;
            }
        }
        if (((b >> 16) & 0xFF) != SW_ID || ((b >> 24) & 0xF)) SP_NO("the map coordinates are not a plain register");
        if (k == 0) base = b; else if (spType(b) != spType(base) || spNum(b) != spNum(base)) SP_NO("the four reads do not start from one register");
        if (off) ++nOffset; else ++nDirect;
    }
    if (nDirect != 1 || nOffset != 3) SP_NO("the four reads are not one place and its three neighbours");
    info->base = base;
    const int lastTap = tap[3];

    // the sum of the four answers: dp4 dst.c, (1), R - and before it cmp R, R, 1, 0; add R, R, -W.c; add W.c, D.z, (bias)
    int iDp = -1; DWORD R = 0;
    for (DWORD k = (DWORD)lastTap + 1; k < nIns && iDp < 0; ++k) {
        if (ins[k].op != 9 || ins[k].len != 3) continue;
        const DWORD* a = code + ins[k].at;
        for (int s = 2; s <= 3; ++s) {
            DWORD cst = a[s], reg = a[5 - s];
            if (spType(cst) != SP_C || spNum(cst) >= 224 || !defSet[spNum(cst)] || spType(reg) != SP_T) continue;
            DWORD sw = (cst >> 16) & 0xFF, comp = sw & 3;
            if (sw != comp * 0x55u) continue;
            float v = defv[spNum(cst)][comp]; DWORD mod = (cst >> 24) & 0xF;
            if (mod == 1) v = -v; else if (mod) continue;
            if (v != 1.0f || ((reg >> 16) & 0xFF) != SW_ID || ((reg >> 24) & 0xF)) continue;
            iDp = (int)k; R = spNum(reg); break;
        }
        break;                                                         // the first dp4 after the reads is the one, or it is not this lookup
    }
    if (iDp < 0) SP_NO("the four answers are not added up by a dp4 with ones");
    const DWORD dpDst = code[ins[iDp].at + 1];
    { DWORD m = (dpDst >> 16) & 0xF;
      if (spType(dpDst) != SP_T || (m != 1 && m != 2 && m != 4 && m != 8) || ((dpDst >> 20) & 0xFF)) SP_NO("the sum does not go into one component of a register"); }
    int iCmp = -1, iSub = -1, iBias = -1; DWORD W = 0, Wc = 0;
    for (int j = iDp - 1; j > 0; --j) {                                // the last full write of R before the sum
        const DWORD* a = code + ins[j].at;
        if (ins[j].op == 0x51 || ins[j].op == 0x1F || ins[j].len < 1 || spType(a[1]) != SP_T || spNum(a[1]) != R) continue;
        if (ins[j].op == 0x58 && ins[j].len == 4 && ((a[1] >> 16) & 0xF) == 0xF && spType(a[2]) == SP_T && spNum(a[2]) == R && ((a[2] >> 16) & 0xFF) == SW_ID && !((a[2] >> 24) & 0xF)) {
            bool ok = true; float v[2];
            for (int s = 0; s < 2 && ok; ++s) {
                DWORD cst = a[3 + s], sw = (cst >> 16) & 0xFF, comp = sw & 3;
                ok = spType(cst) == SP_C && spNum(cst) < 224 && defSet[spNum(cst)] && sw == comp * 0x55u && !((cst >> 24) & 0xF);
                if (ok) v[s] = defv[spNum(cst)][comp];
            }
            if (ok && v[0] == 1.0f && v[1] == 0.0f) iCmp = j;
        }
        break;
    }
    if (iCmp < 0) SP_NO("the answers are not made by cmp R, R, 1, 0");
    for (int j = iCmp - 1; j > 0; --j) {
        const DWORD* a = code + ins[j].at;
        if (ins[j].op == 0x51 || ins[j].op == 0x1F || ins[j].len < 1 || spType(a[1]) != SP_T || spNum(a[1]) != R || ((a[1] >> 16) & 0xF) != 0xF) continue;
        if (ins[j].op == 2 && ins[j].len == 3) {
            for (int s = 2; s <= 3; ++s) {
                DWORD w = a[s], r = a[5 - s], sw = (w >> 16) & 0xFF, comp = sw & 3;
                if (spType(w) == SP_T && ((w >> 24) & 0xF) == 1 && sw == comp * 0x55u && spType(r) == SP_T && spNum(r) == R && ((r >> 16) & 0xFF) == SW_ID && !((r >> 24) & 0xF)) { iSub = j; W = spNum(w); Wc = comp; }
            }
        }
        break;
    }
    if (iSub < 0) SP_NO("the depths are not compared by add R, R, -W");
    for (int j = iSub - 1; j > 0; --j) {
        const DWORD* a = code + ins[j].at;
        if (ins[j].op == 0x51 || ins[j].op == 0x1F || ins[j].len < 1 || spType(a[1]) != SP_T || spNum(a[1]) != W || !((a[1] >> 16) & (1u << Wc))) continue;
        if (ins[j].op == 2 && ins[j].len == 3) {
            for (int s = 2; s <= 3; ++s) {
                DWORD cst = a[s], d = a[5 - s], sw = (cst >> 16) & 0xFF, comp = sw & 3;
                if (spType(cst) != SP_C || spNum(cst) >= 224 || !defSet[spNum(cst)] || sw != comp * 0x55u || ((cst >> 24) & 0xF)) continue;
                float v = defv[spNum(cst)][comp];
                if (!(v < 0.0f && v > -0.05f)) continue;
                if ((spType(d) != 3 && spType(d) != 1) || ((d >> 16) & 0xFF) != SW_Z || ((d >> 24) & 0xF)) continue;   // the pixel's own depth: z of an interpolated register
                iBias = j; info->depthTok = d; info->biasTok = cst;
            }
        }
        break;
    }
    if (iBias < 0) SP_NO("the pixel's depth is not taken as (an interpolated z) + (a small negative constant)");
    // R holds what the reads returned: one of them went into R itself or was moved into it
    { bool fed = false;
      for (int k = 0; k < 4 && !fed; ++k) {
          DWORD d = code[ins[tap[k]].at + 1];
          if (spType(d) == SP_T && spNum(d) == R) fed = true;
          for (int j = tap[k] + 1; j < iSub && !fed; ++j) { const DWORD* a = code + ins[j].at; if (ins[j].op == 1 && ins[j].len == 2 && spType(a[1]) == SP_T && spNum(a[1]) == R && spType(a[2]) == SP_T && spNum(a[2]) == spNum(d)) fed = true; }
      }
      if (!fed) SP_NO("the register that is compared does not hold the four depths"); }
    // room: seven registers, four constants
    DWORD t0 = anyTemp ? maxTemp + 1 : 0;
    if (t0 + 7 > 32) SP_NO("no seven registers are free");
    DWORD kc[4]; int nk = 0;
    for (int r = (int)nConst - 1; r >= 0 && nk < 4; --r) if (!cUsed[r]) kc[nk++] = (DWORD)r;
    if (nk < 4) SP_NO("no four constants are free");
    info->temp0 = t0; memcpy(info->kc, kc, sizeof(kc)); info->dp4Dst = dpDst; info->dp4At = iDp;

    // ---- the new shader
    DWORD n = 0;
#define SP_PUT(v) do { if (n >= outCap) SP_NO("no room for the new shader"); out[n++] = (DWORD)(v); } while (0)
#define SP_I1(op, d, a)          do { SP_PUT((op) | (2u << 24)); SP_PUT(d); SP_PUT(a); } while (0)
#define SP_I2(op, d, a, b)       do { SP_PUT((op) | (3u << 24)); SP_PUT(d); SP_PUT(a); SP_PUT(b); } while (0)
#define SP_I3(op, d, a, b, c)    do { SP_PUT((op) | (4u << 24)); SP_PUT(d); SP_PUT(a); SP_PUT(b); SP_PUT(c); } while (0)
#define T_(i, m)   spDst(SP_T, t0 + (i), (m))
#define S_(i, sw)  spSrc(SP_T, t0 + (i), (sw), 0)
#define N_(i, sw)  spSrc(SP_T, t0 + (i), (sw), 1)
#define K_(i, sw)  spSrc(SP_C, kc[i], (sw), 0)
    SP_PUT(sm3 ? code[0] : 0xFFFF0201u);
    for (DWORD i = 1; i < firstIns; ++i) SP_PUT(code[i]);              // the comments (the constant table)
    static const float kDef[4][4] = { { 0.0f, 1.0f, 2.0f, 0.0f }, { 1.0f, 0.0f, 2.0f, 1.0f }, { -0.5f, 0.0f, 0.5f, 4.0f }, { 0.5f, 0.5f, 0.0f, 0.0f } };
    for (int k = 0; k < 4; ++k) { SP_PUT(0x51u | (5u << 24)); SP_PUT(spDst(SP_C, kc[k], 0xF)); for (int c = 0; c < 4; ++c) { DWORD v; memcpy(&v, &kDef[k][c], 4); SP_PUT(v); } }
    const DWORD inv = spSrc(SP_C, cOff, SW_Z, 0), smp = spSrc(SP_S, (DWORD)sampler, SW_ID, 0);
    for (DWORD i = firstIns; i < ins[iFirst].at; ++i) SP_PUT(code[i]);
    // where the game first reads the coordinates for its own lookup (they are in their register there for certain):
    SP_I1(6,  T_(0, 8), inv);                                          // rcp   n0.w, 1/size            the map's size
    SP_I2(5,  T_(0, 3), base, S_(0, SW_W));                            // mul   n0.xy, B, n0.w          where in the map, in map points
    SP_I1(19, T_(1, 3), S_(0, SW_ID));                                 // frc   n1.xy, n0               where inside its map point
    SP_I2(2,  T_(0, 3), S_(0, SW_ID), N_(1, SW_ID));                   // add   n0.xy, n0, -n1          the map point
    SP_I2(2,  T_(0, 3), S_(0, SW_ID), K_(2, SW_X));                    // add   n0.xy, n0, -0.5         the centre of the one before it, both ways
    SP_I2(5,  T_(0, 3), S_(0, SW_ID), inv);                            // mul   n0.xy, n0, 1/size
    for (DWORD i = ins[iFirst].at; i < ins[iDp].at; ++i) SP_PUT(code[i]);
    // in place of the sum of the four:
    SP_I2(2,  T_(1, 8), info->depthTok, info->biasTok);                // add   n1.w, D.z, bias         the pixel's depth as the game takes it
    struct Tp { int k; DWORD sw; };                                    // the nine places, as (constant, swizzle) giving (column, row)
    static const Tp kTp[9] = { { -1, 0 }, { 1, SW_ID }, { 0, SW_ZXYW },   { 0, SW_ID }, { 0, SW_Y }, { 1, SW_ZXYW },   { 0, SW_WZYX }, { 0, SW_YZXW }, { 0, SW_Z } };
    for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) {
        const Tp& tp = kTp[row * 3 + col];
        DWORD coord = S_(0, SW_ID);
        if (tp.k >= 0) { SP_I3(4, T_(5, 3), K_(tp.k, tp.sw), inv, S_(0, SW_ID)); coord = S_(5, SW_ID); }   // mad n5.xy, (col,row), 1/size, n0
        if (col == 0) SP_I2(0x42, T_(2 + row, 0xF), coord, smp);       // texld n(2+row), .., s
        else { SP_I2(0x42, T_(6, 0xF), coord, smp); SP_I1(1, T_(2 + row, 1u << col), S_(6, SW_X)); }   // texld n6 / mov n(2+row).y|z, n6.x
    }
    for (int row = 0; row < 3; ++row) {
        SP_I2(2, T_(2 + row, 7), S_(2 + row, SW_ID), N_(1, SW_W));     // add   nR.xyz, nR, -n1.w
        SP_I3(0x58, T_(2 + row, 7), S_(2 + row, SW_ID), K_(0, SW_Y), K_(0, SW_X));   // cmp nR.xyz, nR, 1, 0
    }
    SP_I3(4, T_(5, 7), S_(1, SW_X), K_(2, SW_ID), K_(3, SW_ID));       // mad   n5.xyz, n1.x, (-.5,0,.5), (.5,.5,0)   the three columns' weights
    SP_I3(4, T_(6, 7), S_(1, SW_Y), K_(2, SW_ID), K_(3, SW_ID));       // mad   n6.xyz, n1.y, ...                     the three rows'
    for (int row = 0; row < 3; ++row) SP_I2(8, T_(0, 1u << row), S_(2 + row, SW_ID), S_(5, SW_ID));     // dp3 n0.x|y|z, nR, n5
    SP_I2(8, T_(0, 8), S_(0, SW_ID), S_(6, SW_ID));                    // dp3   n0.w, n0, n6            the lit fraction
    SP_I2(5, dpDst, S_(0, SW_W), K_(2, SW_W));                         // mul   dst, n0.w, 4            what the sum of the four was
    for (DWORD i = ins[iDp].at + 1 + ins[iDp].len; i <= end; ++i) SP_PUT(code[i]);
#undef SP_PUT
#undef SP_I1
#undef SP_I2
#undef SP_I3
#undef T_
#undef S_
#undef N_
#undef K_
#undef SP_NO
    info->grew = (int)n - (int)(end + 1);
    return n;
}

#ifdef SP_RUNTIME
// ---------------------------------------------------------------- in the game: where a pixel shader is bound
// The effects' shaders are made when an effect is loaded, long before the device's method table is in our hands, so
// they are met where they are used: the device's own SetPixelShader (the entry every path ends in - the render
// thread's queue, a direct call, and the device pass that issues an in-between picture's calls again) is wrapped.
// A shader seen for the first time is read back, tried, and remembered by its pointer: with its nine-sample twin, or
// as one to bind as it is. The original is held by a reference while it is remembered, so the pointer cannot come to
// mean another shader; the twin is ours. Everything here runs with the render lock held (or before the render thread
// exists), like every device call.
static volatile LONG g_spOn = 0;
static void* g_spO_SetPS = NULL;
struct SpEnt { DWORD orig, repl; };            // repl: 0 bind as it is; == orig: one of ours (bind as it is, not held); else the twin
#define SP_NMAP 2048
static SpEnt g_spMap[SP_NMAP]; static LONG g_spNEnt = 0;
static volatile LONG g_spNSeen = 0, g_spNMade = 0, g_spNOdd = 0, g_spNRefused = 0, g_spNDrops = 0, g_spNBinds = 0;
static __forceinline DWORD spHash(DWORD ps) { return ((ps >> 3) * 0x9E3779B1u) >> 21; }
static void spDropAll() {
    for (int i = 0; i < SP_NMAP; ++i) { SpEnt& e = g_spMap[i]; if (!e.orig) continue;
        if (e.repl != e.orig) { if (e.repl) ((TwF1)g_rtO_ps[2])(e.repl); ((TwF1)g_rtO_ps[2])(e.orig); }
        e.orig = e.repl = 0; }
    g_spNEnt = 0; g_spNDrops++;
}
static void spPut(DWORD ps, DWORD repl) {
    DWORD h = spHash(ps);
    while (g_spMap[h].orig) h = (h + 1) & (SP_NMAP - 1);
    g_spMap[h].orig = ps; g_spMap[h].repl = repl; g_spNEnt++;
}
static DWORD spMake(DWORD dev, DWORD ps) {
    UINT bytes = 0;
    if (!g_rtO_ps[4] || (LONG)((TwF3)g_rtO_ps[4])(ps, 0, (DWORD)(ULONG_PTR)&bytes) < 0 || bytes < 16 || bytes > 512 * 1024 || (bytes & 3)) return 0;
    DWORD n = bytes / 4;
    DWORD* buf = (DWORD*)malloc((size_t)(n * 2 + 1024) * 4);
    if (!buf) return 0;
    DWORD repl = 0;
    if ((LONG)((TwF3)g_rtO_ps[4])(ps, (DWORD)(ULONG_PTR)buf, (DWORD)(ULONG_PTR)&bytes) >= 0) {
        SpInfo info; DWORD m = spPatch(buf, n, buf + n, n + 1024, &info);
        if (m) {
            void* p = NULL; DWORD hr = ((TwF3)g_rtO_dev[106])(dev, (DWORD)(ULONG_PTR)(buf + n), (DWORD)(ULONG_PTR)&p);
            if ((LONG)hr >= 0 && p) {
                repl = (DWORD)(ULONG_PTR)p;
                LONG k = InterlockedIncrement(&g_spNMade);
                if (k == 1) logf("shadows: SmoothShadows - the first pixel shader that reads the shadow map was bound; its twin with the nine-sample lookup was made (%u tokens, %d more; sampler s%u, map size from c%u) and is drawn with in its place. Every such shader gets one the first time it is used.", n, info.grew, info.sampler, info.cOff);
                else if (k == 20 || k == 80 || k == 160) logf("shadows: SmoothShadows - %d shadow-reading pixel shaders have their nine-sample twin so far (%d shaders looked at, %d that read the map some other way, %d the device refused).", (int)k, (int)g_spNSeen, (int)g_spNOdd, (int)g_spNRefused);
            } else if (InterlockedIncrement(&g_spNRefused) <= 3)
                logf("shadows: SmoothShadows - the device refused a nine-sample twin (0x%08X; %u tokens; it needs r%u-r%u and c%u-c%u) - that shader keeps the game's own lookup. Please report this line.", hr, m, info.temp0, info.temp0 + 6, info.kc[3], info.kc[0]);
        } else if (info.why[0] && InterlockedIncrement(&g_spNOdd) <= 3)
            logf("shadows: SmoothShadows - a pixel shader reads the shadow map in a way that is not the game's four-sample lookup (%s) - it is left as it is. Please report this line.", info.why);
    }
    free(buf);
    return repl;
}
static DWORD __stdcall spSetPixelShader(DWORD dev, DWORD ps) {
    if (!ps || !g_spOn) return ((TwF2)g_spO_SetPS)(dev, ps);
    for (DWORD h = spHash(ps); ; h = (h + 1) & (SP_NMAP - 1)) {
        const SpEnt& e = g_spMap[h];
        if (e.orig == ps) { if (e.repl && e.repl != ps) { g_spNBinds++; return ((TwF2)g_spO_SetPS)(dev, e.repl); } return ((TwF2)g_spO_SetPS)(dev, ps); }
        if (!e.orig) break;
    }
    if (g_spNEnt >= SP_NMAP / 2) spDropAll();              // (far more shaders than the game has: start over - what is bound stays alive in the device)
    g_spNSeen++;
    DWORD repl = spMake(dev, ps);
    ((TwF1)g_rtO_ps[1])(ps);                               // held while it is remembered
    spPut(ps, repl);
    if (repl) { spPut(repl, repl); g_spNBinds++; }
    return ((TwF2)g_spO_SetPS)(dev, repl ? repl : ps);
}
static void spWrap() {                                     // after the device's table is hooked (rtInstall)
    if (!g_rtO_ps[1] || !g_rtO_ps[2] || !g_rtO_ps[4]) { if (g_spOn) logf("shadows: SmoothShadows - pixel shader objects are not in our hands on this device (their methods could not be reached) - the game's own lookup stays."); return; }
    if (g_rtO_dev[107] && g_rtO_dev[107] != (void*)&spSetPixelShader) { g_spO_SetPS = g_rtO_dev[107]; g_rtO_dev[107] = (void*)&spSetPixelShader; }
}
static void installSmoothShadows() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    int on = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES ? (int)GetPrivateProfileIntA("Engine", "SmoothShadows", 0, ini) : 0;
    char env[16]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_SMOOTHSHADOWS", env, sizeof(env)) && env[0]) on = atoi(env);
    if (on <= 0) return;
    InterlockedExchange(&g_spOn, 1);
    logf("shadows: SmoothShadows - the game asks its shadow map four yes/no questions per pixel and adds them up, so a shadow's edge is built of squares the size of a map point in five shades. Every pixel shader that does that gets a twin that asks nine, each at a map point's exact centre, and weights them by where the pixel lies - an even edge at any magnification. Same map, same depth test, same light; the GPU's work only (nine reads of the map instead of four).");
}
// test: the switch (live) and the counters
extern "C" __declspec(dllexport) void __cdecl AotrShadowPcfTest(int on, char* out, int nOut) {
    if (on >= 0) InterlockedExchange(&g_spOn, on ? 1 : 0);
    if (out && nOut > 200) sprintf(out, "smooth shadows %s: %d pixel shaders looked at, %d twins made, %d read the map some other way, %d refused by the device, %d binds answered with a twin, table emptied %d times%s",
                                   g_spOn ? "ON" : "off", (int)g_spNSeen, (int)g_spNMade, (int)g_spNOdd, (int)g_spNRefused, (int)g_spNBinds, (int)g_spNDrops, g_spO_SetPS ? "" : " (the device's SetPixelShader is NOT wrapped)");
}
#endif
