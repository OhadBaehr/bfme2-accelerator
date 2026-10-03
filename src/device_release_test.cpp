// x86 cl /O2 /MT /EHsc device_release_test.cpp; tests the production helper.
#include <windows.h>
#include <cstdio>
#include <cassert>
#include <intrin.h>
#pragma comment(lib, "user32.lib")
typedef DWORD (__stdcall* RtS1)(void*);
static DWORD g_rtMainTid, g_rtWorkerTid = 0, g_rtExecSeq = 0, g_rtSeq = 0;
static volatile LONG g_rtActive = 1, g_rtInstalled = 1, g_rtNForeign = 0;
static CRITICAL_SECTION g_rtExecCs;
static int g_rtDirectScope = 0;
static bool g_rtObjRefs = false, g_rtDeviceReleaseFast = true, g_rtHaveResourceRelease = false;
static DWORD g_rtLastResourceRelease = 0;
static LONG g_rtNSync = 0, g_rtSyncHist[512] = {};
static int g_rtCurOp = 511;
static void* g_rtO_dev[3];
static unsigned drains, pendingResourceDrops;
struct Device { DWORD refs; unsigned destroyed; };
static DWORD __stdcall addRef(void* object) {
    Device& d = *(Device*)object; assert(!d.destroyed && d.refs); return ++d.refs;
}
static DWORD __stdcall release(void* object) {
    Device& d = *(Device*)object; assert(!d.destroyed && d.refs);
    DWORD result = --d.refs;
    if (!result) {
        assert(GetCurrentThreadId() == g_rtMainTid);
        assert(g_rtExecSeq == g_rtSeq); // destruction must follow all pending work
        d.destroyed++;
    }
    return result;
}
static bool rtSeqReached(DWORD cur, DWORD target) { DWORD delta = target - cur; return !delta || delta >= 0x80000000u; }
static void rtRetireDeviceRefs();
static Device* fixture;
static void rtDrain() {
    drains++;
    // Simulate queued resource destruction before publishing queue completion.
    while (pendingResourceDrops) { release(fixture); --pendingResourceDrops; }
    g_rtExecSeq = g_rtSeq;
    rtRetireDeviceRefs();
}
static void rtExecLock() { EnterCriticalSection(&g_rtExecCs); }
static void rtSyncNote(int, DWORD) {}
#define RT_PASSTHRU(CLS, SLOT, SIG, ...) do { \
    DWORD tid_ = GetCurrentThreadId(); \
    if (tid_ != g_rtMainTid || !g_rtActive || g_rtDirectScope) { \
        if (g_rtActive && tid_ != g_rtWorkerTid && tid_ != g_rtMainTid) { EnterCriticalSection(&g_rtExecCs); DWORD r_ = ((SIG)g_rtO_dev[SLOT])(__VA_ARGS__); LeaveCriticalSection(&g_rtExecCs); InterlockedIncrement(&g_rtNForeign); return r_; } \
        return ((SIG)g_rtO_dev[SLOT])(__VA_ARGS__); } } while (0)
#define RT_SYNC(CLS, SLOT, OP, SIG, ...) do { rtDrain(); EnterCriticalSection(&g_rtExecCs); g_rtDirectScope++; DWORD result_ = ((SIG)g_rtO_dev[SLOT])(__VA_ARGS__); \
    g_rtDirectScope--; LeaveCriticalSection(&g_rtExecCs); g_rtNSync++; g_rtSyncHist[OP]++; return result_; } while (0)
#include "aotr_rt_release.inc"

static void reset(Device& d, DWORD refs) {
    assert(!g_rtDeviceRefCount);
    d.refs = refs; d.destroyed = 0; fixture = &d;
    g_rtActive = g_rtInstalled = 1; g_rtDirectScope = 0;
    g_rtObjRefs = false; g_rtDeviceReleaseFast = true;
    g_rtHaveResourceRelease = false; g_rtLastResourceRelease = 0;
    g_rtExecSeq = 10; g_rtSeq = 11; pendingResourceDrops = 0;
    drains = 0; g_rtNDeviceReleaseFast = g_rtNDeviceRefRetired = g_rtNSync = 0;
}
static DWORD WINAPI foreignRelease(void* d) { return rt_dev_2(d); }
int main() {
    static_assert(sizeof(void*) == 4, "Build this test with the x86 toolset");
    InitializeCriticalSection(&g_rtExecCs); g_rtMainTid = GetCurrentThreadId();
    g_rtO_dev[1] = (void*)&addRef; g_rtO_dev[2] = (void*)&release;
    Device d;
    reset(d, 3);
    assert(rt_dev_2(&d) == 2 && drains == 0 && d.refs == 3);
    assert(rt_dev_2(&d) == 1 && drains == 0 && g_rtDeviceRefCount == 2);
    assert(rt_dev_2(&d) == 0 && drains == 1 && d.destroyed == 1);
    assert(g_rtDeviceRefCount == 0 && g_rtNDeviceRefRetired == 2 && !g_rtActive);

    reset(d, 2); assert(rt_dev_2(&d) == 1 && drains == 0);
    // A subsequent synchronous operation retires the retained reference.
    rtDrain(); assert(d.refs == 1 && !d.destroyed && !g_rtDeviceRefCount);
    assert(rt_dev_2(&d) == 0 && d.destroyed == 1);

    reset(d, 2); g_rtHaveResourceRelease = true; g_rtLastResourceRelease = g_rtSeq;
    pendingResourceDrops = 1;
    assert(rt_dev_2(&d) == 0 && drains == 1 && d.destroyed == 1);
    // Pending releases across sequence wrap must also force the barrier.
    reset(d, 2); g_rtExecSeq = 0xFFFFFFFE; g_rtSeq = 1;
    g_rtHaveResourceRelease = true; g_rtLastResourceRelease = 0;
    pendingResourceDrops = 1;
    assert(rt_dev_2(&d) == 0 && drains == 1 && d.destroyed == 1);

    reset(d, 3); g_rtObjRefs = true;
    assert(rt_dev_2(&d) == 2 && drains == 1 && !g_rtDeviceRefCount);
    reset(d, 3); g_rtDeviceReleaseFast = false;
    assert(rt_dev_2(&d) == 2 && drains == 1 && !g_rtDeviceRefCount);
    reset(d, 3); g_rtDirectScope = 1;
    assert(rt_dev_2(&d) == 2 && drains == 0 && !g_rtDeviceRefCount);
    g_rtDirectScope = 0;
    reset(d, 3); g_rtActive = 0;
    assert(rt_dev_2(&d) == 2 && drains == 0 && !g_rtDeviceRefCount);

    reset(d, 100);
    for (int i = 0; i < 64; ++i) assert(rt_dev_2(&d) == (DWORD)(99-i));
    assert(drains == 0 && g_rtDeviceRefCount == 64);
    assert(rt_dev_2(&d) == 35 && drains == 1 && !g_rtDeviceRefCount);

    reset(d, 3); assert(rt_dev_2(&d) == 2);
    HANDLE thread = CreateThread(0, 0, foreignRelease, &d, 0, 0); assert(thread);
    assert(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0); CloseHandle(thread);
    assert(d.refs == 2 && g_rtDeviceRefCount == 1);
    rtDrain(); assert(d.refs == 1 && !g_rtDeviceRefCount);
    assert(rt_dev_2(&d) == 0 && d.destroyed == 1);

    // Resource destruction after a fast release can make retirement final;
    // retirement still occurs after drain and on the creator thread.
    reset(d, 2); assert(rt_dev_2(&d) == 1);
    pendingResourceDrops = 1; rtDrain();
    assert(d.destroyed == 1 && !g_rtActive && !g_rtDeviceRefCount);
    DeleteCriticalSection(&g_rtExecCs);
    puts("PASS: non-final releases avoid barriers; final/resource-pending releases wait; reservations retire on main thread; ref-mode/toggle/scope/foreign/cap/wrap cases");
}
