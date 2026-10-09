#pragma once
#include <windows.h>

// ABI v1: fixed-width fields; input is copied before installation. No pointers inside.
#define BFME2_ACCEL_ABI 1u
#define BFME2_ACCEL_HEAP 1u
#define BFME2_ACCEL_CRT 2u
#define BFME2_ACCEL_PRESHADER 4u
#define BFME2_ACCEL_SUPPORTED 7u
#define BFME2_ACCEL_INVALID 0u
#define BFME2_ACCEL_READY 1u
#define BFME2_ACCEL_BUSY 2u
#define BFME2_ACCEL_CONFIG_CONFLICT 3u
#define BFME2_ACCEL_UNSUPPORTED_HOST 4u
#define BFME2_ACCEL_FAILED 5u
#define BFME2_ACCEL_INCOMPATIBLE 6u

// READY means installation completed; enabled/failed describe individual features.
// reserved must be zero. Unsupported feature bits are rejected before any installation.
struct Bfme2AccelRequest {
    DWORD size, version, requested, reserved;
    DWORD enabled, skipped, failed, status;
};
typedef DWORD (WINAPI* Bfme2AccelInitializeFn)(Bfme2AccelRequest*);
static_assert(sizeof(Bfme2AccelRequest) == 32, "ABI size");
