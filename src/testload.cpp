#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "accel_api.h"
int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "bfme2_accel.dll";
    HMODULE module = LoadLibraryA(path);
    if (!module) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    Bfme2AccelInitializeFn init = (Bfme2AccelInitializeFn)GetProcAddress(module, "Bfme2AccelInitialize");
    if (!init) { printf("Missing API export\n"); FreeLibrary(module); return 2; }
    Bfme2AccelRequest request = {32, BFME2_ACCEL_ABI, 0, 0, 0, 0, 0, 0};
    DWORD status = init(&request);
    printf("No-op API handshake: %lu (no optimizations requested)\n", status);
    FreeLibrary(module);
    return status == BFME2_ACCEL_READY ? 0 : 3;
}
