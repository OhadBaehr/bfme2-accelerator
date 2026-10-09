#include <windows.h>
#include <stdio.h>
#include <assert.h>
#include "accel_inject.h"

int main(int argc, char** argv) {
    if (argc > 1) { Sleep(60000); return 0; } // child owned by the remote-call test
    char dll[MAX_PATH]; assert(GetFullPathNameA("bfme2_accel.dll", MAX_PATH, dll, NULL));
    HMODULE module = LoadLibraryA(dll); assert(module);
    Bfme2AccelInitializeFn init = (Bfme2AccelInitializeFn)GetProcAddress(module, "Bfme2AccelInitialize");
    assert(init);
    assert(init(NULL) == BFME2_ACCEL_INVALID);
    Bfme2AccelRequest r = {32, 99, 0, 0, 0, 0, 0, 0};
    assert(init(&r) == BFME2_ACCEL_INCOMPATIBLE);
    r.version = 1; r.requested = 8;
    assert(init(&r) == BFME2_ACCEL_INVALID);
    r.requested = 0; r.reserved = 1;
    assert(init(&r) == BFME2_ACCEL_INVALID);
    r.reserved = 0;
    assert(init(&r) == BFME2_ACCEL_READY && r.enabled == 0 && r.failed == 0);
    assert(init(&r) == BFME2_ACCEL_READY); // idempotent
    r.requested = 1;
    assert(init(&r) == BFME2_ACCEL_CONFIG_CONFLICT);
    FreeLibrary(module);
    char self[MAX_PATH]; assert(GetModuleFileNameA(NULL, self, MAX_PATH));
    char command[2 * MAX_PATH]; sprintf_s(command, "\"%s\" --host", self);
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    assert(CreateProcessA(self, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi));
    Bfme2AccelRequest report = {};
    bool ok = accelInject(pi.dwProcessId, dll, 0, &report);
    assert(report.status == BFME2_ACCEL_READY && report.enabled == 0);
    assert(ok);
    assert(accelInject(pi.dwProcessId, dll, 0, &report));
    assert(!accelInject(pi.dwProcessId, dll, 7, &report));
    assert(report.status == BFME2_ACCEL_CONFIG_CONFLICT);
    TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    sprintf_s(command, "\"%s\" --host", self);
    assert(CreateProcessA(self, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi));
    ok = accelInject(pi.dwProcessId, dll, 7, &report);
    TerminateProcess(pi.hProcess, 0); WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    assert(!ok && report.status == BFME2_ACCEL_UNSUPPORTED_HOST);
    assert(report.enabled == 0 && report.skipped == 7 && report.failed == 0);

    puts("PASS: API validation, idempotence, request conflict, standalone remote initialization");
    return 0;
}
