// ---------------------------------------------------------------- a Direct3D 9 shader function's tables
// (shared by the DLL - aotr_devpass.inc - and src/fxprobe.cpp)
// The constants a shader reads straight from registers are listed in its constant table: the 'CTAB' comment the
// compiler puts at the start of the function. A shader the effect compiler gave a PRESHADER - a small program D3DX
// runs on the CPU for every pass and commit, whose results it writes into further registers of the shader - carries
// that program in a 'PRES' comment of its own, and inside it a second constant table naming the effect parameters
// the program reads. Together the two tables say, for one shader, everything a parameter's value can reach:
// registers directly (main table), registers through the preshader (the PRES table), or nothing.
// Offsets inside a table are relative to its first byte (the D3DXSHADER_CONSTANTTABLE header, 28 bytes).
// The preshader's program ('FXLC', with its literal constants in 'CLIT') is a list of instructions over four tables
// of numbers, addressed by component: literals (1), the input parameters laid out in registers as its constant table
// says (2), temporaries (7) and the outputs - the shader's own float registers (4; 5 and 6 are its bool and int
// registers). An instruction is: a word (operation in bits 20-30, bit 31: the first input is one number used for
// every component, low 16 bits: components), the number of inputs, then for every input and for the output: a flag
// (1: indexed by another register, whose table and offset follow), a table and an offset.
#define SHT_MAXC 64
#define SHT_MAXINS 160
#define SHT_MAXLIT 256
#define SHT_COMPS 1024                        // components a table may have
struct ShtConst { const char* name; WORD set, reg, cnt, cls, type, rows, cols, elems, members; };   // set: 0 bool, 1 int, 2 float, 3 sampler
struct ShtArg { BYTE tab, idx; WORD off; };
struct ShtIns { WORD op; BYTE scalar, nin; WORD ncomp; ShtArg in[3], out; };
struct ShtProg { int ok, nIns, nLit; ShtIns ins[SHT_MAXINS]; double lit[SHT_MAXLIT]; };
struct ShtTab { int ok, hasMain, hasPre, nMain, nPre; ShtConst main[SHT_MAXC], pre[SHT_MAXC]; ShtProg prog; };

static const DWORD* shtArg(const DWORD* p, const DWORD* e, ShtArg* a) {
    if (e - p < 3) return NULL;
    a->idx = 0;
    if (*p) { if (*p != 1 || e - p < 5) return NULL; a->idx = 1; p += 3; }
    else ++p;
    if (p[0] > 7 || p[1] >= SHT_COMPS) return NULL;
    a->tab = (BYTE)p[0]; a->off = (WORD)p[1];
    return p + 2;
}
static bool shtProgram(const DWORD* fx, DWORD nfx, const DWORD* lit, DWORD nlit, ShtProg* g) {
    g->ok = 0; g->nIns = 0; g->nLit = 0;
    if (lit) {
        if (nlit < 1 || lit[0] > SHT_MAXLIT || lit[0] * 2 > nlit - 1) return false;
        g->nLit = (int)lit[0]; memcpy(g->lit, lit + 1, lit[0] * 8);
    }
    if (!fx || nfx < 1 || fx[0] > SHT_MAXINS) return false;
    const DWORD* p = fx + 1; const DWORD* e = fx + nfx;
    for (DWORD i = 0; i < fx[0]; ++i) {
        if (e - p < 2) return false;
        ShtIns& n = g->ins[i];
        DWORD code = p[0], nin = p[1]; p += 2;
        if (nin > 3 || (code & 0xFFFF) > 4 || !(code & 0xFFFF)) return false;
        n.op = (WORD)((code >> 20) & 0x7FF); n.scalar = (code >> 31) ? 1 : 0; n.ncomp = (WORD)(code & 0xFFFF); n.nin = (BYTE)nin;
        for (DWORD k = 0; k < nin; ++k) { p = shtArg(p, e, &n.in[k]); if (!p) return false; }
        p = shtArg(p, e, &n.out); if (!p) return false;
        if (n.out.off + n.ncomp > SHT_COMPS) return false;
        for (DWORD k = 0; k < nin; ++k) if (n.in[k].off + n.ncomp > SHT_COMPS) return false;
    }
    g->nIns = (int)fx[0]; g->ok = 1;
    return true;
}

static bool shtCtab(const BYTE* base, DWORD size, ShtConst* out, int* n, int cap) {
    if (size < 28) return false;
    const DWORD* h = (const DWORD*)base;
    if (h[0] != 28) return false;
    DWORD cnt = h[3], info = h[4];
    if (cnt > (DWORD)cap || info > size || cnt * 20 > size - info) return false;
    for (DWORD i = 0; i < cnt; ++i) {
        const BYTE* ci = base + info + i * 20;
        DWORD nameOff = *(const DWORD*)ci, typeOff = *(const DWORD*)(ci + 12);
        if (nameOff >= size || typeOff > size - 16) return false;
        DWORD k = nameOff; while (k < size && base[k]) ++k;
        if (k >= size) return false;                                   // a name that does not end inside the table
        const WORD* ty = (const WORD*)(base + typeOff);
        ShtConst& c = out[i];
        c.name = (const char*)base + nameOff; c.set = *(const WORD*)(ci + 4); c.reg = *(const WORD*)(ci + 6); c.cnt = *(const WORD*)(ci + 8);
        c.cls = ty[0]; c.type = ty[1]; c.rows = ty[2]; c.cols = ty[3]; c.elems = ty[4]; c.members = ty[5];
    }
    *n = (int)cnt;
    return true;
}
// fn: the function as IDirect3DVertexShader9 / PixelShader9 ::GetFunction returns it; bytes: its size. The names
// in the result point into fn. ok: the shader has a constant table, and a preshader (if any) names its inputs.
static bool shtParse(const DWORD* fn, DWORD bytes, ShtTab* t) {
    memset(t, 0, sizeof(*t));
    DWORD nt = bytes / 4;
    if (nt < 2) return false;
    for (DWORD i = 1; i < nt; ) {                                      // the comments that lead the function
        DWORD tok = fn[i];
        if ((tok & 0xFFFF) != 0xFFFE) break;
        DWORD n = (tok >> 16) & 0x7FFF;
        if (n > nt - i - 1) return false;
        const DWORD* c = fn + i + 1;
        if (n >= 1 && c[0] == 0x42415443u) {                           // 'CTAB'
            if (!shtCtab((const BYTE*)(c + 1), (n - 1) * 4, t->main, &t->nMain, SHT_MAXC)) return false;
            t->hasMain = 1;
        } else if (n >= 2 && c[0] == 0x53455250u) {                    // 'PRES': a version word, then comments of its own (the tag alone - every pixel shader of the game's effects has one - is no program)
            t->hasPre = 1;
            bool tab = false; const DWORD* fxlc = NULL; const DWORD* clit = NULL; DWORD nfxlc = 0, nclit = 0;
            for (DWORD j = 2; j < n; ) {
                DWORD t2 = c[j];
                if ((t2 & 0xFFFF) != 0xFFFE) break;
                DWORD n2 = (t2 >> 16) & 0x7FFF;
                if (n2 > n - j - 1) return false;
                if (n2 >= 1 && c[j + 1] == 0x42415443u) {
                    if (!shtCtab((const BYTE*)(c + j + 2), (n2 - 1) * 4, t->pre, &t->nPre, SHT_MAXC)) return false;
                    tab = true;
                } else if (n2 >= 1 && c[j + 1] == 0x434C5846u) { fxlc = c + j + 2; nfxlc = n2 - 1; }      // 'FXLC'
                else if (n2 >= 1 && c[j + 1] == 0x54494C43u) { clit = c + j + 2; nclit = n2 - 1; }        // 'CLIT'
                j += 1 + n2;
            }
            if (!tab) return false;                                    // a preshader whose inputs cannot be named
            shtProgram(fxlc, nfxlc, clit, nclit, &t->prog);            // (a program that cannot be read: prog.ok stays 0, and nothing is computed from it)
        }
        i += 1 + n;
    }
    t->ok = t->hasMain;
    return t->ok != 0;
}
