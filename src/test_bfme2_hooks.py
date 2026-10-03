"""Generate a 32-bit C++ test from the actual hook code; optionally verify retail.

python test_bfme2_hooks.py --output test.cpp [--retail path/to/game.dat]
Compile test.cpp with MSVC's x86 cl /O2 /MT /EHsc and run it.
Retail verification additionally requires pefile and capstone.
"""
import argparse
from pathlib import Path
import re

p = argparse.ArgumentParser()
p.add_argument('--output', type=Path, required=True)
p.add_argument('--retail', type=Path)
args = p.parse_args()
root = Path(__file__).resolve().parent
hooks = (root / 'aotr_bfme2.inc').read_text()
if args.retail:
    import pefile
    import capstone
    pe = pefile.PE(str(args.retail))
    assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
    section = next(s for s in pe.sections if s.Name.startswith(b'.text'))
    h = 2166136261
    for b in section.get_data()[:min(section.Misc_VirtualSize, section.SizeOfRawData)]:
        h = ((h ^ b) * 16777619) & 0xffffffff
    assert h == 0x32667B9B, f'Unsupported retail text hash {h:08X}'
    constants = dict(re.findall(r'const DWORD (kB2\w+) = (0x[0-9A-Fa-f]+);', hooks))
    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for name in ('Equiv', 'Final', 'Brute', 'Cast'):
        body = re.search(r'kB2' + name + r'Pre\[\] = \{([^}]+)\}', hooks)[1]
        expected = bytes(int(v, 0) for v in body.split(','))
        va = int(constants['kB2' + name], 0)
        assert pe.get_data(va - 0x400000, len(expected)) == expected, name
        if name in ('Equiv', 'Brute'):
            stolen = 6 if name == 'Equiv' else 5
            instructions = list(cs.disasm(expected[:stolen], va))
            assert sum(i.size for i in instructions) == stolen
            assert not any(i.mnemonic.startswith(('j', 'call', 'loop')) for i in instructions)
    # Independent matched TurretAI frame-counter getter: global and +40 layout.
    assert bytes.fromhex('8b158ce7df008b5240') in pe.get_data(0x4D82D6, 32)
    print('PASS: retail hash, four hook signatures, stolen instruction boundaries and logic frame layout')

eq = hooks[:hooks.index('static void installBfme2Hooks()')]
eq = re.sub(r'static DWORD b2LogicPointer\(\).*?\n', 'static DWORD b2LogicPointer() { return mockLogic; }\n', eq)
eq = re.sub(r'static DWORD b2LogicFrame\(DWORD logic\).*?\n', 'static DWORD b2LogicFrame(DWORD) { return mockFrame; }\n', eq)
pick = (root / 'aotr_pick.inc').read_text()
bounds = pick[pick.index('static __forceinline double pbAbs'):pick.index('static BYTE __fastcall hkBfCast')]
prefix = r'''
#include <windows.h>
#include <intrin.h>
#include <emmintrin.h>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <limits>
#define EQ_N 8192
typedef BYTE (__fastcall* tIsEquiv)(void*, void*, void*);
static DWORD g_mkTid, mockLogic = 1, mockFrame = 1;
static unsigned stockCalls;
static BYTE expectedResult;
static void logf(const char*, ...) {}
static DWORD eqFinal(DWORD t) { DWORD n; while ((n = *(DWORD*)(t + 4))) t = n; return t; }
static BYTE __fastcall stock(void*, void*, void*) { ++stockCalls; return expectedResult; }
'''
tests = r'''
static unsigned bucket(DWORD a, DWORD b) { return ((a * 2654435761u) ^ (b * 2246822519u)) >> 19; }
int main() {
    static_assert(sizeof(void*) == 4, "Test must be built for x86");
    g_mkTid = GetCurrentThreadId(); o_b2Eq = stock;
    g_b2Eq = new B2EqEntry[EQ_N]();
    DWORD templates[10000][2] = {};
    void* a = templates[0]; void* b = templates[1];
    expectedResult = 1;
    assert(hkB2Equiv(a, 0, b) == 1);
    for (int i = 0; i < 512; ++i) assert(hkB2Equiv(a, 0, b) == 1);
    assert(g_b2EqProof == 0 && stockCalls == 513);
    unsigned before = stockCalls;
    for (int i = 0; i < 64; ++i) assert(hkB2Equiv(a, 0, b) == 1);
    assert(stockCalls == before + 2);
    expectedResult = 0; ++mockFrame;
    assert(hkB2Equiv(a, 0, b) == 0); // new frame recomputes
    templates[0][1] = (DWORD)templates[2]; before = stockCalls;
    assert(hkB2Equiv(a, 0, b) == 0 && stockCalls == before + 1);
    ++mockLogic; expectedResult = 1;
    assert(hkB2Equiv(a, 0, b) == 1); // new logic instance
    mockFrame = 0; expectedResult = 0; before = stockCalls;
    for (int i = 0; i < 10; ++i) assert(hkB2Equiv(a, 0, b) == 0);
    assert(stockCalls == before + 10);
    mockFrame = 1; assert(hkB2Equiv(a, 0, b) == 0);
    // Two distinct pairs deliberately occupy the same direct-mapped bucket.
    DWORD ad = (DWORD)a, bd = (DWORD)b;
    void* collision = 0;
    for (int i = 2; i < 10000; ++i)
        if (bucket(ad, (DWORD)templates[i]) == bucket(ad, bd)) { collision = templates[i]; break; }
    assert(collision); expectedResult = 1;
    assert(hkB2Equiv(a, 0, collision) == 1);
    expectedResult = 0; before = stockCalls;
    assert(hkB2Equiv(a, 0, b) == 0 && stockCalls == before + 1);
    g_mkTid = 0; expectedResult = 1; before = stockCalls;
    assert(hkB2Equiv(a, 0, b) == 1 && stockCalls == before + 1);
    g_mkTid = GetCurrentThreadId();
    // A changed same-frame stock answer must permanently stop shortcuts.
    for (int i = 0; i < 32 && !g_b2EqKill; ++i) hkB2Equiv(a, 0, b);
    assert(g_b2EqKill); before = stockCalls;
    for (int i = 0; i < 32; ++i) assert(hkB2Equiv(a, 0, b) == 1);
    assert(stockCalls == before + 32);

    SYSTEM_INFO info; GetSystemInfo(&info); DWORD size = info.dwPageSize;
    BYTE* pages = (BYTE*)VirtualAlloc(0, size * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(pages); DWORD old; assert(VirtualProtect(pages + size, size, PAGE_NOACCESS, &old));
    float* vertices = (float*)(pages + size - 9 * sizeof(float));
    const float input[] = {-1,-2,-3, 4,5,6, 1,2,3}; memcpy(vertices, input, sizeof(input));
    WORD indices[36]; for (int i = 0; i < 36; ++i) indices[i] = (WORD)(i % 3);
    DWORD ih[4] = {}, vh[4] = {}; ih[3] = (DWORD)indices; vh[3] = (DWORD)vertices;
    DWORD model[13] = {}; model[0x2c/4] = (DWORD)ih; model[0x30/4] = (DWORD)vh;
    float mn[3], mx[3]; assert(pbVertexBounds((BYTE*)model, 12, mn, mx));
    for (int i = 0; i < 3; ++i) { assert(mn[i] == input[i]); assert(mx[i] == input[i+3]); }
    vertices[0] = std::numeric_limits<float>::quiet_NaN();
    assert(!pbVertexBounds((BYTE*)model, 12, mn, mx));
    vertices[0] = input[0]; vertices[8] = std::numeric_limits<float>::quiet_NaN();
    assert(!pbVertexBounds((BYTE*)model, 12, mn, mx));
    const float low[] = {-1,-1,-1}, high[] = {1,1,1};
    const float cross0[] = {-10,0,0}, cross1[] = {10,0,0};
    const float miss0[] = {-10,10,0}, miss1[] = {10,10,0};
    const float edge0[] = {-10,1,1}, edge1[] = {10,1,1};
    assert(!pbSegmentMissesBox(low, high, cross0, cross1));
    assert(!pbSegmentMissesBox(low, high, edge0, edge1));
    assert(pbSegmentMissesBox(low, high, miss0, miss1));
    float bad[] = {std::numeric_limits<float>::infinity(),0,0};
    assert(!pbSegmentMissesBox(low, high, bad, cross1));
    VirtualFree(pages, 0, MEM_RELEASE); delete[] g_b2Eq;
    puts("PASS: memo proofs, sampled checks, overrides, frame/reset invalidation, hash collisions, thread bypass and mismatch shutdown; guarded vertex bounds, NaNs and segment edge cases");
}
'''
args.output.write_text(prefix + eq + bounds + tests)
