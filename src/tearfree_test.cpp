// tearfree_test: what [Engine] TearFree does to this process's DXVK_CONFIG (the accelerator's own code, through its test export)
#include <windows.h>
#include <stdio.h>
#include <string.h>
int main() {
    HMODULE h = LoadLibraryA("bfme2_accel.new.dll"); if (!h) { printf("no bfme2_accel.new.dll\n"); return 3; }
    typedef int (__cdecl* TfF)(char*, int); TfF tf = (TfF)GetProcAddress(h, "AotrTearFreeTest"); if (!tf) { printf("missing AotrTearFreeTest\n"); return 3; }
    Sleep(500);
    struct { const char* cfg; const char* onoff; const char* want; int asked; } k[] = {
        { "d3d9.maxFrameLatency = 1; d3d9.samplerAnisotropy = 16; d3d9.presentInterval = 0; dxvk.hud = fps", "1", "d3d9.maxFrameLatency = 1; d3d9.samplerAnisotropy = 16; d3d9.presentInterval = 0; dxvk.hud = fps; dxvk.tearFree = True", 1 },
        { "", "1", "dxvk.tearFree = True", 1 },
        { "d3d9.presentInterval = 0; dxvk.tearFree = False", "1", "d3d9.presentInterval = 0; dxvk.tearFree = False", 2 },      // the user's own word on it stands
        { "d3d9.presentInterval = 0; DXVK.TEARFREE = Auto", "1", "d3d9.presentInterval = 0; DXVK.TEARFREE = Auto", 2 },
        { "d3d9.presentInterval = 0", "0", "d3d9.presentInterval = 0", 0 },                                                    // off: nothing is touched
    };
    int fail = 0;
    for (int i = 0; i < 5; ++i) {
        SetEnvironmentVariableA("DXVK_CONFIG", k[i].cfg[0] ? k[i].cfg : NULL); SetEnvironmentVariableA("AOTR_TEARFREE", k[i].onoff);
        char out[4096]; out[0] = 0; int asked = tf(out, sizeof(out));
        bool ok = !strcmp(out, k[i].want) && asked == k[i].asked;
        printf("%s  \"%s\" with TearFree %s -> \"%s\"\n", ok ? "ok  " : "FAIL", k[i].cfg, k[i].onoff, out);
        if (!ok) fail = 1;
    }
    printf("%s\n", fail ? "FAILED" : "tear-free option: as it must be");
    return fail;
}
