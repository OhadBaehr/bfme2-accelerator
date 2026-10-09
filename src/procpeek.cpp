// procpeek.cpp - where are the threads of a (hung) process?   procpeek.exe <pid> [stack bytes]
// For every thread: CPU time used over a short interval (a spinning thread shows), its instruction pointer, and
// every word on its stack that points into a module's code (return addresses, more or less). Reads only; each
// thread is suspended for the moment its registers are taken and resumed at once.
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct Mod { DWORD lo, hi; char name[64]; };
static Mod g_mod[512]; static int g_nmod = 0;
static const Mod* modOf(DWORD a) { for (int i = 0; i < g_nmod; ++i) if (a >= g_mod[i].lo && a < g_mod[i].hi) return &g_mod[i]; return NULL; }
static ULONGLONG cpuOf(HANDLE th) { FILETIME c, e, k, u; if (!GetThreadTimes(th, &c, &e, &k, &u)) return 0; return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime); }
int main(int argc, char** argv) {
    if (argc < 2) { printf("procpeek <pid> [stack bytes]\n"); return 2; }
    DWORD pid = (DWORD)atoi(argv[1]); DWORD depth = argc > 2 ? (DWORD)atoi(argv[2]) : 6144;
    HANDLE proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!proc) { printf("OpenProcess failed (%lu)\n", GetLastError()); return 3; }
    HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32 me; me.dwSize = sizeof(me);
    if (ms != INVALID_HANDLE_VALUE && Module32First(ms, &me)) do { if (g_nmod < 512) { g_mod[g_nmod].lo = (DWORD)(ULONG_PTR)me.modBaseAddr; g_mod[g_nmod].hi = g_mod[g_nmod].lo + me.modBaseSize; lstrcpynA(g_mod[g_nmod].name, me.szModule, 64); ++g_nmod; } } while (Module32Next(ms, &me));
    if (ms != INVALID_HANDLE_VALUE) CloseHandle(ms);
    for (int i = 0; i < g_nmod; ++i) if (strstr(g_mod[i].name, "accel") || strstr(g_mod[i].name, "game.dat") || !lstrcmpiA(g_mod[i].name, "d3d9.dll") || strstr(g_mod[i].name, "d3dx9")) printf("M %08lX %08lX %s\n", (unsigned long)g_mod[i].lo, (unsigned long)g_mod[i].hi, g_mod[i].name);
    struct T { DWORD tid; HANDLE h; ULONGLONG c0, c1; } t[256]; int nt = 0;
    HANDLE ts = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te; te.dwSize = sizeof(te);
    if (Thread32First(ts, &te)) do { if (te.th32OwnerProcessID == pid && nt < 256) { t[nt].tid = te.th32ThreadID; t[nt].h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID); ++nt; } } while (Thread32Next(ts, &te));
    CloseHandle(ts);
    for (int i = 0; i < nt; ++i) t[i].c0 = t[i].h ? cpuOf(t[i].h) : 0;
    Sleep(500);
    for (int i = 0; i < nt; ++i) t[i].c1 = t[i].h ? cpuOf(t[i].h) : 0;
    static BYTE stack[65536]; if (depth > sizeof(stack)) depth = sizeof(stack);
    for (int i = 0; i < nt; ++i) {
        if (!t[i].h) { printf("T %lu (cannot be opened)\n", (unsigned long)t[i].tid); continue; }
        CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread(t[i].h) == (DWORD)-1) { printf("T %lu (cannot be suspended)\n", (unsigned long)t[i].tid); continue; }
        BOOL ok = GetThreadContext(t[i].h, &c);
        SIZE_T got = 0; if (ok) ReadProcessMemory(proc, (LPCVOID)(ULONG_PTR)c.Esp, stack, depth, &got);
        ResumeThread(t[i].h);
        const Mod* m = ok ? modOf(c.Eip) : NULL;
        printf("T %lu cpu %lu%% total %lu ms | eip %s+%lX\n", (unsigned long)t[i].tid, (unsigned long)((t[i].c1 - t[i].c0) / 50000), (unsigned long)(t[i].c1 / 10000), m ? m->name : "?", (unsigned long)(m ? c.Eip - m->lo : c.Eip));
        int n = 0;
        for (SIZE_T o = 0; o + 4 <= got && n < 70; o += 4) { DWORD v = *(DWORD*)(stack + o); const Mod* q = modOf(v); if (q && v - q->lo >= 0x1000) { printf("   +%04lX %s+%lX\n", (unsigned long)o, q->name, (unsigned long)(v - q->lo)); ++n; } }
    }
    return 0;
}
