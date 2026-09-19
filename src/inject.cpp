// 32-bit injector: loads aotr_accel.dll into the running game.dat process.
// Nothing is written into the game folder, so the AotR launcher has nothing to delete
// and game.dat on disk is untouched (its checksum still passes).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>

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
    const char* dll = (argc > 1) ? argv[1] : "aotr_accel.dll";
    char full[MAX_PATH];
    if (!GetFullPathNameA(dll, MAX_PATH, full, NULL)) { printf("bad dll path\n"); return 1; }
    if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) { printf("DLL not found: %s\n", full); return 1; }

    DWORD pid = findPid("game.dat");
    if (!pid) { printf("game.dat is not running\n"); return 2; }
    printf("game.dat pid %lu\n", pid);

    HANDLE hp = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                            PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!hp) { printf("OpenProcess failed %lu (run elevated)\n", GetLastError()); return 3; }

    SIZE_T len = strlen(full) + 1;
    void* remote = VirtualAllocEx(hp, NULL, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { printf("VirtualAllocEx failed %lu\n", GetLastError()); CloseHandle(hp); return 4; }
    SIZE_T wrote = 0;
    if (!WriteProcessMemory(hp, remote, full, len, &wrote)) {
        printf("WriteProcessMemory failed %lu\n", GetLastError()); CloseHandle(hp); return 5;
    }

    // This injector is 32-bit, so kernel32's LoadLibraryA address matches the target's.
    FARPROC loadLib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
    HANDLE th = CreateRemoteThread(hp, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib, remote, 0, NULL);
    if (!th) { printf("CreateRemoteThread failed %lu\n", GetLastError()); CloseHandle(hp); return 6; }

    WaitForSingleObject(th, 10000);
    DWORD mod = 0;
    GetExitCodeThread(th, &mod);
    printf(mod ? "injected OK, module handle %08lX\n" : "LoadLibrary returned 0 (load failed)\n", mod);

    VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
    CloseHandle(th);
    CloseHandle(hp);
    return mod ? 0 : 7;
}
