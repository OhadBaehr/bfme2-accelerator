// filewarm_run: runs the accelerator's file opener (the real walk and the real worker threads) over a folder.
//     filewarm_run.exe <folder> [threads] [dll]
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: filewarm_run <folder> [threads] [dll]\n"); return 2; }
    HMODULE h = LoadLibraryW(argc > 3 ? argv[3] : L"bfme2_accel.new.dll");
    if (!h) { printf("cannot load the DLL (%lu)\n", GetLastError()); return 2; }
    typedef int (__cdecl* RunF)(const wchar_t*, int, char*, int);
    RunF run = (RunF)GetProcAddress(h, "AotrFileWarmRun");
    if (!run) { printf("no AotrFileWarmRun export\n"); return 3; }
    static char out[2048]; out[0] = 0;
    int r = run(argv[1], argc > 2 ? _wtoi(argv[2]) : 8, out, sizeof(out));
    printf("%s\n", out);
    return r;
}
