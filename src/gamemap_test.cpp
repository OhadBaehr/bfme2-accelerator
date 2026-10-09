// gamemap_test: runs a test export of the accelerator DLL in a process that has game.dat's image mapped at its
// base address (no imports resolved, nothing of the game started), so the export can call the engine's own
// functions and compare them with the replacement.
//     gamemap_test.exe [dll] [export]        default: bfme2_accel.new.dll AotrTerrainTest
// The test process's own heaps already sit at 0x400000 when main runs, so the parent creates a suspended child
// (this exe, image based at 0x70000000), writes game.dat's headers and sections at the image base before the
// child's loader creates anything there, and lets the child run the test.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int runChildWithImage(const char* path, const char* dll, const char* fn) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { printf("cannot open %s\n", path); return 2; }
    DWORD size = GetFileSize(f, NULL), got = 0;
    BYTE* d = (BYTE*)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ReadFile(f, d, size, &got, NULL); CloseHandle(f);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(d + ((IMAGE_DOS_HEADER*)d)->e_lfanew);
    DWORD base = nt->OptionalHeader.ImageBase, img = nt->OptionalHeader.SizeOfImage;
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    char cmd[MAX_PATH * 3]; sprintf_s(cmd, "\"%s\" child \"%s\" %s", exe, dll, fn);
    STARTUPINFOA si = { sizeof(si) }; PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) { printf("CreateProcess failed %u\n", GetLastError()); return 2; }
    void* m = VirtualAllocEx(pi.hProcess, (void*)(ULONG_PTR)base, img, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (m != (void*)(ULONG_PTR)base) { printf("cannot map the image at %08X (size %08X) in the child: error %u\n", base, img, GetLastError()); TerminateProcess(pi.hProcess, 2); return 2; }
    SIZE_T wr = 0;
    WriteProcessMemory(pi.hProcess, m, d, nt->OptionalHeader.SizeOfHeaders, &wr);
    IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        DWORD n = sh[i].SizeOfRawData; if (sh[i].Misc.VirtualSize && sh[i].Misc.VirtualSize < n) n = sh[i].Misc.VirtualSize;
        if (n) WriteProcessMemory(pi.hProcess, (BYTE*)m + sh[i].VirtualAddress, d + sh[i].PointerToRawData, n, &wr);
    }
    printf("mapped game.dat at %08X in the test child (%u sections)\n", base, nt->FileHeader.NumberOfSections);
    fflush(stdout);
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 2; GetExitCodeProcess(pi.hProcess, &code);
    return (int)code;
}

int main(int argc, char** argv) {
    if (argc < 2 || strcmp(argv[1], "child"))
        return runChildWithImage("C:\\AgeoftheRing\\rotwk\\game.dat", argc > 1 ? argv[1] : "bfme2_accel.new.dll", argc > 2 ? argv[2] : "AotrTerrainTest");
    HMODULE h = LoadLibraryA(argc > 2 ? argv[2] : "bfme2_accel.new.dll");
    if (!h) { printf("cannot load the DLL (%lu)\n", GetLastError()); return 2; }
    typedef int (__cdecl* TestF)(char*, int);
    TestF t = (TestF)GetProcAddress(h, argc > 3 ? argv[3] : "AotrTerrainTest");
    if (!t) { printf("the DLL has no export %s\n", argc > 3 ? argv[3] : "AotrTerrainTest"); return 3; }
    static char out[4096]; out[0] = 0;
    int r = 1;
    __try { r = t(out, sizeof(out)); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("the test FAULTED (%08lX)\n", GetExceptionCode()); r = 99; }
    printf("%s\n%s\n", out, r ? "FAILED" : "PASSED");
    return r ? 1 : 0;
}
