// Included after all feature installers; DllMain only prepares their locks.
#include "accel_api.h"
#pragma comment(linker, "/EXPORT:Bfme2AccelInitialize=_Bfme2AccelInitialize@4")
static volatile LONG g_apiState = 0; // 0 untouched, 1 installing, 2 completed
static Bfme2AccelRequest g_apiResult = {};

static DWORD apiInstall(Bfme2AccelRequest* result) {
    if (!result->requested) return BFME2_ACCEL_READY;
    BYTE* base = (BYTE*)GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || (ULONG_PTR)base != 0x00400000 ||
        !GetModuleHandleA("msvcr71.dll") || !GetModuleHandleA("mss32.dll")) {
        result->skipped = result->requested;
        return BFME2_ACCEL_UNSUPPORTED_HOST;
    }
    // ABI v1 offers portable features only. No address-based installer or diagnostic
    // environment switch is allowed to expand this request.
    rotateLog();
    logf("api: v1 initialization requested=%lu", result->requested);
    g_engineHooks = 0;
    if (result->requested & BFME2_ACCEL_HEAP) {
        installAllocatorSwap(base);
        if (g_apiHeapInstalled) result->enabled |= BFME2_ACCEL_HEAP;
        else result->failed |= BFME2_ACCEL_HEAP;
    }
    if (result->requested & BFME2_ACCEL_CRT) {
        installFastCrt(base);
        if (g_crtInstalled) result->enabled |= BFME2_ACCEL_CRT;
        else result->failed |= BFME2_ACCEL_CRT;
    }
    if (result->requested & BFME2_ACCEL_PRESHADER) {
        if (!GetModuleHandleA("d3dx9_27.dll")) result->skipped |= BFME2_ACCEL_PRESHADER;
        else {
            installPreshaderHook();
            if (g_apiPreshaderInstalled) result->enabled |= BFME2_ACCEL_PRESHADER;
            else result->failed |= BFME2_ACCEL_PRESHADER;
        }
    }
    logf("api: enabled=%lu skipped=%lu failed=%lu; renderer and engine hooks OFF",
         result->enabled, result->skipped, result->failed);
    return BFME2_ACCEL_READY;
}

extern "C" DWORD WINAPI Bfme2AccelInitialize(Bfme2AccelRequest* request) {
    Bfme2AccelRequest input = {};
    __try {
        if (!request || request->size != sizeof(input)) return BFME2_ACCEL_INVALID;
        input = *request;
        request->enabled = request->skipped = request->failed = 0;
        request->status = BFME2_ACCEL_INVALID;
        if (input.version != BFME2_ACCEL_ABI) {
            request->status = BFME2_ACCEL_INCOMPATIBLE;
            return request->status;
        }
        if (input.reserved || (input.requested & ~BFME2_ACCEL_SUPPORTED)) return BFME2_ACCEL_INVALID;
        LONG previous = InterlockedCompareExchange(&g_apiState, 1, 0);
        if (previous == 1) return request->status = BFME2_ACCEL_BUSY;
        if (previous == 2) {
            if (input.requested != g_apiResult.requested)
                return request->status = BFME2_ACCEL_CONFIG_CONFLICT;
            *request = g_apiResult;
            return request->status;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { return BFME2_ACCEL_INVALID; }
    input.enabled = input.skipped = input.failed = 0;
    __try { input.status = apiInstall(&input); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        input.status = BFME2_ACCEL_FAILED;
        input.failed |= input.requested & ~(input.enabled | input.skipped);
    }
    g_apiResult = input;
    InterlockedExchange(&g_apiState, 2);
    __try { *request = input; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return BFME2_ACCEL_INVALID; }
    return input.status;
}
