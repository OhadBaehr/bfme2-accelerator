#pragma once
#include "accel_api.h"
#include <string.h>

// Shared by the standalone GUI launcher and console injector. No pySAGE dependency.
// On timeout the remote buffer remains allocated: a live remote thread may still use it.
static bool accelRemoteCall(HANDLE process, LPTHREAD_START_ROUTINE entry,
                            void* argument, DWORD* result, bool* completed) {
    *completed = true;
    HANDLE thread = CreateRemoteThread(process, NULL, 0, entry, argument, 0, NULL);
    if (!thread) return false;
    DWORD wait = WaitForSingleObject(thread, 15000);
    *completed = wait == WAIT_OBJECT_0;
    bool ok = *completed && GetExitCodeThread(thread, result);
    CloseHandle(thread);
    return ok;
}
static bool accelRead(HANDLE process, DWORD address, void* out, SIZE_T size) {
    SIZE_T count = 0;
    return ReadProcessMemory(process, (void*)(ULONG_PTR)address, out, size, &count) && count == size;
}
// Resolve against the module actually loaded in the target, which may predate the
// file currently on disk. Never call a locally computed RVA in an unverified image.
static DWORD accelRemoteExport(HANDLE process, DWORD module) {
    IMAGE_DOS_HEADER dos = {}; IMAGE_NT_HEADERS32 nt = {}; IMAGE_EXPORT_DIRECTORY exports = {};
    if (!accelRead(process, module, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < 0 || dos.e_lfanew > 0x100000 ||
        !accelRead(process, module + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return 0;
    IMAGE_DATA_DIRECTORY directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    DWORD size = nt.OptionalHeader.SizeOfImage;
    if (size < sizeof(exports) || !directory.VirtualAddress || directory.VirtualAddress > size - sizeof(exports) ||
        !accelRead(process, module + directory.VirtualAddress, &exports, sizeof(exports)) ||
        exports.NumberOfNames > 4096 || exports.NumberOfFunctions > 4096) return 0;
    if (exports.AddressOfNames > size || exports.NumberOfNames * 4 > size - exports.AddressOfNames ||
        exports.AddressOfNameOrdinals > size || exports.NumberOfNames * 2 > size - exports.AddressOfNameOrdinals ||
        exports.AddressOfFunctions > size || exports.NumberOfFunctions * 4 > size - exports.AddressOfFunctions) return 0;
    for (DWORD i = 0; i < exports.NumberOfNames; ++i) {
        DWORD name = 0, target = 0; WORD ordinal = 0; char text[22] = {};
        if (!accelRead(process, module + exports.AddressOfNames + i * 4, &name, 4) ||
            name > size || sizeof(text) > size - name || !accelRead(process, module + name, text, sizeof(text))) return 0;
        if (memcmp(text, "Bfme2AccelInitialize", sizeof("Bfme2AccelInitialize"))) continue;
        if (!accelRead(process, module + exports.AddressOfNameOrdinals + i * 2, &ordinal, 2) ||
            ordinal >= exports.NumberOfFunctions ||
            !accelRead(process, module + exports.AddressOfFunctions + ordinal * 4, &target, 4) || !target || target >= size) return 0;
        if (target >= directory.VirtualAddress && target - directory.VirtualAddress < directory.Size) return 0; // forwarded export
        return module + target;
    }
    return 0;
}
static bool accelInject(DWORD pid, const char* fullPath, DWORD features,
                        Bfme2AccelRequest* report) {
    *report = { sizeof(*report), BFME2_ACCEL_ABI, features, 0, 0, 0, 0, 0 };
    // Map only for the exported RVA; never initialize a second copy in the launcher.
    HMODULE local = LoadLibraryExA(fullPath, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) return false;
    FARPROC proc = GetProcAddress(local, "Bfme2AccelInitialize");
    DWORD rva = proc ? (DWORD)((BYTE*)proc - (BYTE*)local) : 0;
    FreeLibrary(local);
    if (!rva) { report->status = BFME2_ACCEL_INCOMPATIBLE; return false; }
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) return false;
    SIZE_T pathSize = lstrlenA(fullPath) + 1;
    SIZE_T size = pathSize > sizeof(*report) ? pathSize : sizeof(*report);
    void* remote = VirtualAllocEx(process, NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool completed = true, ok = false;
    DWORD module = 0, status = 0, remoteEntry = 0;
    SIZE_T count = 0;
    if (remote && WriteProcessMemory(process, remote, fullPath, pathSize, &count) && count == pathSize &&
        accelRemoteCall(process, (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                        "LoadLibraryA"), remote, &module, &completed) && module &&
        (remoteEntry = accelRemoteExport(process, module)) != 0 &&
        WriteProcessMemory(process, remote, report, sizeof(*report), &count) && count == sizeof(*report) &&
        accelRemoteCall(process, (LPTHREAD_START_ROUTINE)(ULONG_PTR)remoteEntry, remote, &status, &completed) &&
        ReadProcessMemory(process, remote, report, sizeof(*report), &count) && count == sizeof(*report)) {
        ok = status == BFME2_ACCEL_READY && report->status == status && !report->failed &&
             (!features || report->enabled != 0);
    }
    if (remote && completed) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}
