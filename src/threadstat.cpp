// threadstat.cpp - what the threads of a process are doing, WITHOUT opening it:   threadstat.exe <pid> [interval ms] [dll ...]
// The game runs elevated, so a tool started from an ordinary shell may not open it or read its memory. The system's
// own process list (NtQuerySystemInformation) is readable all the same, and it carries for every thread: state, what
// it waits for, processor time used, context switches, and the address it was started at. Two snapshots an interval
// apart show which threads run and which stand still. Each DLL named on the command line is mapped here to learn
// where it sits (one image has one address per boot), so the start addresses can be given as module+offset.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef LONG NTSTATUS;
struct UStr { USHORT Length, MaximumLength; PWSTR Buffer; };
struct CliId { HANDLE UniqueProcess, UniqueThread; };
struct ThrInfo { LARGE_INTEGER KernelTime, UserTime, CreateTime; ULONG WaitTime; PVOID StartAddress; CliId ClientId; LONG Priority, BasePriority; ULONG ContextSwitches, ThreadState, WaitReason; };
struct ThrInfoEx { ThrInfo t; PVOID StackBase, StackLimit, Win32StartAddress, TebBase; ULONG_PTR r2, r3, r4; };
struct ProcInfo {
    ULONG NextEntryOffset, NumberOfThreads; LARGE_INTEGER WorkingSetPrivateSize; ULONG HardFaultCount, NumberOfThreadsHighWatermark; ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime, UserTime, KernelTime; UStr ImageName; LONG BasePriority; HANDLE UniqueProcessId, InheritedFromUniqueProcessId; ULONG HandleCount, SessionId; ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize, VirtualSize; ULONG PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivatePageCount;
    LARGE_INTEGER ReadOperationCount, WriteOperationCount, OtherOperationCount, ReadTransferCount, WriteTransferCount, OtherTransferCount;
};
typedef NTSTATUS (NTAPI* tNQSI)(ULONG, PVOID, ULONG, PULONG);
static const char* kState[] = { "initialized", "ready", "RUNNING", "standby", "terminated", "waiting", "transition", "deferred-ready", "gate-wait", "waiting-for-process-in-swap" };
static const char* kWait[] = { "Executive", "FreePage", "PageIn", "PoolAllocation", "DelayExecution(Sleep)", "Suspended", "UserRequest(WaitForObject)", "WrExecutive", "WrFreePage", "WrPageIn", "WrPoolAllocation", "WrDelayExecution(Sleep)",
    "WrSuspended", "WrUserRequest(window message wait)", "WrEventPair", "WrQueue(thread pool / completion port)", "WrLpcReceive", "WrLpcReply", "WrVirtualMemory", "WrPageOut", "WrRendezvous", "WrKeyedEvent", "WrTerminated", "WrProcessInSwap",
    "WrCpuRateControl", "WrCalloutStack", "WrKernel", "WrResource", "WrPushLock", "WrMutex", "WrQuantumEnd", "WrDispatchInt", "WrPreempted", "WrYieldExecution", "WrFastMutex", "WrGuardedMutex", "WrRundown", "WrAlertByThreadId(lock / condition variable)", "WrDeferredPreempt", "WrPhysicalFault", "WrIoRing", "WrMdlCache" };
struct Mod { DWORD lo, hi; char name[64]; };
static Mod g_mod[32]; static int g_nmod = 0;
struct Row { DWORD tid; ULONGLONG cpu, create; ULONG cs, state, wait; DWORD start; bool ok; };
static BYTE* snap(tNQSI q, ULONG cls) {
    ULONG len = 1 << 20, got = 0;
    for (int i = 0; i < 8; ++i) {
        BYTE* b = (BYTE*)malloc(len);
        NTSTATUS s = q(cls, b, len, &got);
        if (s >= 0) return b;
        free(b); len = got + (1 << 18);
    }
    return NULL;
}
static int rows(BYTE* b, DWORD pid, bool ex, Row* out, int max) {
    int n = 0;
    for (ProcInfo* p = (ProcInfo*)b; ; p = (ProcInfo*)((BYTE*)p + p->NextEntryOffset)) {
        if ((DWORD)(ULONG_PTR)p->UniqueProcessId == pid) {
            BYTE* t = (BYTE*)(p + 1);
            for (ULONG i = 0; i < p->NumberOfThreads && n < max; ++i, t += ex ? sizeof(ThrInfoEx) : sizeof(ThrInfo)) {
                ThrInfo* ti = (ThrInfo*)t; Row& r = out[n++];
                r.tid = (DWORD)(ULONG_PTR)ti->ClientId.UniqueThread; r.cpu = (ULONGLONG)ti->KernelTime.QuadPart + (ULONGLONG)ti->UserTime.QuadPart; r.create = (ULONGLONG)ti->CreateTime.QuadPart;
                r.cs = ti->ContextSwitches; r.state = ti->ThreadState; r.wait = ti->WaitReason; r.start = ex ? (DWORD)(ULONG_PTR)((ThrInfoEx*)t)->Win32StartAddress : (DWORD)(ULONG_PTR)ti->StartAddress; r.ok = true;
            }
            return n;
        }
        if (!p->NextEntryOffset) break;
    }
    return -1;
}
int main(int argc, char** argv) {
    if (argc < 2) { printf("threadstat <pid> [interval ms] [dll ...]\n"); return 2; }
    DWORD pid = (DWORD)atoi(argv[1]); DWORD ms = argc > 2 ? (DWORD)atoi(argv[2]) : 2000;
    tNQSI q = (tNQSI)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    for (int i = 3; i < argc && g_nmod < 32; ++i) {
        HMODULE h = LoadLibraryExA(argv[i], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!h) { printf("M (could not map %s: %lu)\n", argv[i], GetLastError()); continue; }
        IMAGE_DOS_HEADER* d = (IMAGE_DOS_HEADER*)h; IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)h + d->e_lfanew);
        Mod& m = g_mod[g_nmod++]; m.lo = (DWORD)(ULONG_PTR)h; m.hi = m.lo + nt->OptionalHeader.SizeOfImage;
        const char* s = strrchr(argv[i], '\\'); const char* s2 = strrchr(argv[i], '/'); if (s2 > s) s = s2;
        lstrcpynA(m.name, s ? s + 1 : argv[i], 64);
        printf("M %08lX-%08lX %s (as mapped here)\n", (unsigned long)m.lo, (unsigned long)m.hi, m.name);
    }
    bool ex = true;
    BYTE* a = snap(q, 57);                                       // SystemExtendedProcessInformation: with the Win32 start address
    if (!a) { ex = false; a = snap(q, 5); }
    if (!a) { printf("the system's process list could not be read\n"); return 3; }
    static Row r0[1024], r1[1024];
    int n0 = rows(a, pid, ex, r0, 1024);
    if (n0 < 0) { printf("process %lu is not in the list\n", (unsigned long)pid); return 4; }
    Sleep(ms);
    BYTE* b = snap(q, ex ? 57 : 5);
    int n1 = b ? rows(b, pid, ex, r1, 1024) : -1;
    if (n1 < 0) { printf("process %lu went away\n", (unsigned long)pid); return 5; }
    printf("process %lu: %d threads, %lu ms apart (%s start addresses)\n", (unsigned long)pid, n1, (unsigned long)ms, ex ? "Win32" : "kernel");
    ULONGLONG first = ~0ull; for (int i = 0; i < n1; ++i) if (r1[i].create < first) first = r1[i].create;
    for (int i = 0; i < n1; ++i) {
        const Row& r = r1[i]; const Row* o = NULL;
        for (int k = 0; k < n0; ++k) if (r0[k].tid == r.tid) o = &r0[k];
        char where[96];
        const Mod* m = NULL; for (int k = 0; k < g_nmod; ++k) if (r.start >= g_mod[k].lo && r.start < g_mod[k].hi) m = &g_mod[k];
        if (m) wsprintfA(where, "%s+%lX", m->name, (unsigned long)(r.start - m->lo)); else wsprintfA(where, "%08lX", (unsigned long)r.start);
        double pct = o ? (double)(r.cpu - o->cpu) / 10000.0 / (double)ms * 100.0 : 0.0;
        printf("T %6lu  born +%7.1f s  cpu %8.1f s  now %5.1f%%  switches +%-6lu  %-9s %-44s start %s\n", (unsigned long)r.tid, (double)(r.create - first) / 1e7, (double)r.cpu / 1e7, pct,
               (unsigned long)(o ? r.cs - o->cs : 0), r.state < 10 ? kState[r.state] : "?", r.state == 5 ? (r.wait < 42 ? kWait[r.wait] : "?") : "", where);
    }
    return 0;
}
