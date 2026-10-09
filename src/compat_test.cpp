#include "aotr_accel.cpp"
#include <assert.h>

static void testAllocator() {
    assert(rpmalloc_initialize() == 0);
    unsigned char* p = (unsigned char*)my_calloc(8, 8);
    assert(p && aOwns(p));
    for (int i = 0; i < 64; ++i) assert(p[i] == 0);
    memset(p, 0x5A, 64);
    unsigned char* larger = (unsigned char*)my_realloc(p, 128);
    assert(larger && aOwns(larger));
    for (int i = 0; i < 64; ++i) assert(larger[i] == 0x5A);
    assert(my_realloc(larger, (size_t)-1) == NULL);
    for (int i = 0; i < 64; ++i) assert(larger[i] == 0x5A);
    LONG frees = g_aFrees;
    assert(my_realloc(larger, 0) == NULL);
    assert(g_aFrees == frees + 1);
    assert(my_calloc((size_t)-1, 2) == NULL);
    assert(my_malloc((size_t)-1) == NULL);
    my_free(NULL);
    puts("PASS: allocator zero-fill, growth preservation, overflow and realloc-to-zero");
}

int main() {
    testAllocator();
    g_rtFxV = (RtFxVal*)calloc(RT_FXV, sizeof(RtFxVal));
    assert(g_rtFxV);
    g_rtFxV[0].epoch = g_rtFxV[RT_FXV / 2].epoch = g_rtFxV[RT_FXV - 1].epoch = 1;
    g_rtFxEpoch = 0xFFFFFFFF;
    rtFxInvalAll();
    assert(g_rtFxEpoch == 1);
    assert(g_rtFxV[0].epoch == 0 && g_rtFxV[RT_FXV / 2].epoch == 0 && g_rtFxV[RT_FXV - 1].epoch == 0);
    free(g_rtFxV);
    g_rtFxV = NULL;
    puts("PASS: complete effect-cache invalidation on epoch wrap");
    return 0;
}
