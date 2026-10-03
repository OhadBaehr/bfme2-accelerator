"""Generate a differential C++ test against an unmodified aotr_rt.inc.

Usage: python gen_xindex_test.py ORIGINAL_INC OUTPUT_CPP
Compile OUTPUT_CPP with a 32-bit MSVC developer prompt, then run it.
Both tracking implementations are extracted from their actual source files.
"""
import pathlib
import sys


def tracking(path):
    source = pathlib.Path(path).read_text(encoding="utf-8")
    start = source.index("// Per object (hashed)")
    end = source.index("static volatile LONG g_rtCpuHistW", start)
    return source[start:end]


original = tracking(sys.argv[1])
indexed = tracking(pathlib.Path(__file__).with_name("aotr_rt.inc"))
prelude = "DWORD g_rtSeq = 0, g_rtExecSeq = 0; LONG g_mkFrames = 0;\n"
test = r'''
static void check(bool ok) { if (!ok) { fprintf(stderr, "Tracking mismatch\n"); exit(1); } }
static void compare(bool full) {
    check(stock::g_rtXN == indexed::g_rtXN);
    check(!memcmp(stock::g_rtX, indexed::g_rtX, sizeof(stock::g_rtX)));
    check(stock::g_rtNXHit == indexed::g_rtNXHit);
    check(stock::g_rtNXSaved == indexed::g_rtNXSaved);
    check(stock::g_rtNXAdd == indexed::g_rtNXAdd);
    check(stock::g_rtNXEvict == indexed::g_rtNXEvict);
    check(stock::g_rtNH2Saved == indexed::g_rtNH2Saved);
    check(stock::g_rtPendOp == indexed::g_rtPendOp);
    check(stock::g_rtPendExact == indexed::g_rtPendExact);
    if (!full) return;
    check(!memcmp(stock::g_rtXMark, indexed::g_rtXMark, sizeof(stock::g_rtXMark)));
    check(!memcmp(stock::g_rtRefSeq, indexed::g_rtRefSeq, sizeof(stock::g_rtRefSeq)));
    check(!memcmp(stock::g_rtRefSeq2, indexed::g_rtRefSeq2, sizeof(stock::g_rtRefSeq2)));
    bool seen[RT_XN] = {};
    for (DWORD b = 0; b < RT_XHASHN; ++b) {
        int steps = 0;
        for (WORD link = indexed::g_rtXHead[b]; link; link = indexed::g_rtXNext[link - 1]) {
            check(++steps <= RT_XN && link <= indexed::g_rtXN);
            LONG i = link - 1;
            check(!seen[i] && indexed::rtXHash(indexed::g_rtX[i].obj) == b);
            seen[i] = true;
        }
    }
    for (LONG i = 0; i < indexed::g_rtXN; ++i) {
        check(seen[i]);
        check(indexed::rtXFind(indexed::g_rtX[i].obj) == i);
    }
}
static void touch(DWORD obj, WORD op) {
    stock::rtTouch(obj, op); indexed::rtTouch(obj, op);
    ++stock::g_rtSeq; ++indexed::g_rtSeq;
}
static void pending(DWORD obj) {
    check(stock::rtRefPending(obj) == indexed::rtRefPending(obj));
    compare(false);
}
static DWORD rng = 0xDEADBEEF;
static DWORD randomWord() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static LONG scanFind(DWORD obj) {
    for (LONG i = 0; i < stock::g_rtXN; ++i) if (stock::g_rtX[i].obj == obj) return i;
    return -1;
}
static void benchmark(const DWORD* pool, int count) {
    LARGE_INTEGER freq, begin, middle, end;
    QueryPerformanceFrequency(&freq);
    DWORD expected = 0, actual = 0;
    const DWORD iterations = 2000000;
    QueryPerformanceCounter(&begin);
    for (DWORD n = 0; n < iterations; ++n) expected += (DWORD)scanFind(pool[n % count]);
    QueryPerformanceCounter(&middle);
    for (DWORD n = 0; n < iterations; ++n) actual += (DWORD)indexed::rtXFind(pool[n % count]);
    QueryPerformanceCounter(&end);
    check(expected == actual);
    printf("Lookup microbenchmark (includes deliberate hash collisions): scan %.2f ns, index %.2f ns\n",
           (middle.QuadPart - begin.QuadPart) * 1e9 / freq.QuadPart / iterations,
           (end.QuadPart - middle.QuadPart) * 1e9 / freq.QuadPart / iterations);
}
int main() {
    DWORD pool[768];
    int count = 0;
    for (DWORD n = 0; n < 512; ++n) pool[count++] = 0x10000 + n * 8;
    // Deliberate collisions in the exact index and in the conservative mark table.
    for (DWORD obj = 0x20000; count < 640; obj += 8)
        if (indexed::rtXHash(obj) == 0) pool[count++] = obj;
    for (DWORD obj = 0x20000; count < 768; obj += 8)
        if (stock::rtRefIdx(obj) == 0) pool[count++] = obj;
    // Saturation: no exact entry may be evicted while all 192 are pending.
    for (int n = 0; n < RT_XN; ++n) { touch(pool[n], (WORD)n); pending(pool[n]); }
    check(indexed::g_rtXN == RT_XN);
    LONG before = indexed::g_rtNXEvict;
    touch(pool[RT_XN], 1); pending(pool[RT_XN]);
    check(indexed::g_rtNXEvict == before && indexed::rtXFind(pool[RT_XN]) < 0);
    compare(true);
    // Random touches, hits, misses, collision updates, eviction and pointer reuse.
    for (DWORD n = 0; n < 400000; ++n) {
        if (n == 150000) stock::g_rtSeq = indexed::g_rtSeq = 0xFFFFF000u;
        if (n == 250000) stock::g_mkFrames = indexed::g_mkFrames = 0x7FFFFFF0;
        if ((n & 7) == 0) {
            stock::g_mkFrames = indexed::g_mkFrames = (LONG)((DWORD)stock::g_mkFrames + 1);
            stock::g_rtExecSeq = indexed::g_rtExecSeq = stock::g_rtSeq - (randomWord() & 255);
        }
        DWORD obj = pool[randomWord() % count];
        if (randomWord() & 1) touch(obj, (WORD)randomWord());
        else pending(obj);
        if ((n & 63) == 0) compare(true);
    }
    compare(true);
    check(indexed::g_rtNXEvict > before);
    puts("PASS: 400000 differential operations; collisions, saturation, eviction, pointer reuse and wraparound");
    benchmark(pool, count);
}
'''
pathlib.Path(sys.argv[2]).write_text(
    '#include <windows.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n'
    + 'namespace stock {\n' + prelude + original + '}\n'
    + 'namespace indexed {\n' + prelude + indexed + '}\n' + test,
    encoding="utf-8",
)
