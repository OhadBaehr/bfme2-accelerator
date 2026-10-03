// Device Release: avoid a queue barrier for a non-final decrement. Retain the
// reservation until a subsequent drain, then retire it on the GAME thread.
// Never put final device destruction on the worker (D3D9 window/thread rules).
// Generated resource-release records carry flag 2 so pending destruction makes
// this path fall back to the original barrier. Per-record reference mode also
// falls back: those implicit releases could affect device ownership.
static void* g_rtDeviceRefObject = NULL;
static DWORD g_rtDeviceRefCount = 0;
static volatile LONG g_rtNDeviceReleaseFast = 0, g_rtNDeviceRefRetired = 0;
extern "C" __declspec(dllexport) void __cdecl AotrRtTestDeviceReleaseStats(char* out, int n) {
    if (!out || n <= 0) return;
    char text[160];
    wsprintfA(text, "fast=%d retired=%d", (int)g_rtNDeviceReleaseFast, (int)g_rtNDeviceRefRetired);
    lstrcpynA(out, text, n);
}

// Called only after rtDrain has completed all previously queued work.
static void rtRetireDeviceRefs() {
    if (!g_rtDeviceRefCount || GetCurrentThreadId() != g_rtMainTid) return;
    EnterCriticalSection(&g_rtExecCs);
    g_rtDirectScope++;
    void* object = g_rtDeviceRefObject;
    DWORD count = g_rtDeviceRefCount;
    g_rtDeviceRefObject = NULL; g_rtDeviceRefCount = 0;
    for (DWORD i = 0; i < count; ++i) {
        DWORD remaining = ((RtS1)g_rtO_dev[2])(object);
        g_rtNDeviceRefRetired++;
        if (!remaining) {
            // The last retained reference can become final after resource
            // destruction. It is dropped here, on the main thread, after drain.
            InterlockedExchange(&g_rtActive, 0);
            InterlockedExchange(&g_rtInstalled, 0);
            break;
        }
    }
    g_rtDirectScope--;
    LeaveCriticalSection(&g_rtExecCs);
}

static DWORD __stdcall rt_dev_2(void* self) {
    RT_PASSTHRU(dev, 2, RtS1, self);
    bool pendingRelease = g_rtHaveResourceRelease && !rtSeqReached(g_rtExecSeq, g_rtLastResourceRelease);
    if (!g_rtDeviceReleaseFast || g_rtObjRefs || pendingRelease || g_rtDeviceRefCount >= 64 ||
        (g_rtDeviceRefObject && g_rtDeviceRefObject != self)) {
        RT_SYNC(dev, 2, 2, RtS1, self);
    }
    rtExecLock(); g_rtDirectScope++;
    DWORD count = ((RtS1)g_rtO_dev[1])(self); // reservation: keeps self alive across the decision and drain
    // Exclude every reservation we own. A final/uncertain release still waits.
    if (count <= g_rtDeviceRefCount + 2) {
        g_rtDirectScope--; LeaveCriticalSection(&g_rtExecCs);
        rtSyncNote(2, (DWORD)(ULONG_PTR)_ReturnAddress()); g_rtCurOp = 2;
        rtDrain(); // prior reservations retire; this new one protects self
        EnterCriticalSection(&g_rtExecCs); g_rtDirectScope++;
        ((RtS1)g_rtO_dev[2])(self); // caller's reference
        DWORD result = ((RtS1)g_rtO_dev[2])(self); // decision reservation; final destruction stays here
        if (!result) { InterlockedExchange(&g_rtActive, 0); InterlockedExchange(&g_rtInstalled, 0); }
        g_rtDirectScope--; LeaveCriticalSection(&g_rtExecCs);
        g_rtNSync++; g_rtSyncHist[2]++;
        return result;
    }
    DWORD result = ((RtS1)g_rtO_dev[2])(self); // consume caller's reference; reservation remains
    g_rtDeviceRefObject = self; g_rtDeviceRefCount++;
    // COM refcounts are diagnostic. Hide our retained references from this
    // nominal result; queued internal ownership changes can still affect it.
    result = result >= g_rtDeviceRefCount ? result - g_rtDeviceRefCount : 0;
    g_rtNDeviceReleaseFast++;
    g_rtDirectScope--; LeaveCriticalSection(&g_rtExecCs);
    return result;
}
