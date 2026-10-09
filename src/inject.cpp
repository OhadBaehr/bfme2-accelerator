// 32-bit injector: loads aotr_accel.dll into the running game.dat process.
// Nothing is written into the game folder, so the AotR launcher has nothing to delete
// and game.dat on disk is untouched (its checksum still passes).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include "accel_inject.h"

static DWORD findPid(const char* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

int main(int argc, char** argv) {
    const char* dll = (argc > 1) ? argv[1] : "bfme2_accel.dll";
    char full[MAX_PATH];
    if (!GetFullPathNameA(dll, MAX_PATH, full, NULL)) { printf("bad dll path\n"); return 1; }
    if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) { printf("DLL not found: %s\n", full); return 1; }

    DWORD pid = findPid("game.dat");
    if (!pid) { printf("game.dat is not running\n"); return 2; }
    printf("game.dat pid %lu\n", pid);

    Bfme2AccelRequest report = {};
    bool ok = accelInject(pid, full, BFME2_ACCEL_SUPPORTED, &report);
    printf("API status=%lu enabled=%lu skipped=%lu failed=%lu\n",
           report.status, report.enabled, report.skipped, report.failed);
    return ok ? 0 : 7;
}
