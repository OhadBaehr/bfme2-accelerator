// BFME2 workshop vanilla 1.06, .text FNV-1a 32667B9B, image base 400000.
// Independent capability set: never enable the RotWK address table for BFME2.
// Sources and retail validation are documented in docs/bfme2-engine-hooks.md.
static const DWORD kB2Equiv = 0x0073BB04;
static const DWORD kB2Final = 0x005E35DF;
static const DWORD kB2Logic = 0x00DFE78C;
static const DWORD kB2Brute = 0x0056CE00;
static const DWORD kB2Cast = 0x0056D7B0;
static const BYTE kB2EquivPre[] = {0x55,0x8B,0xEC,0x83,0xEC,0x0C,0x53,0x56,0x8B,0xF1,0x85,0xF6,0x57};
static const BYTE kB2FinalPre[] = {0x8B,0xC1,0xEB,0x02,0x8B,0xC2,0x8B,0x50,0x04,0x85,0xD2,0x75,0xF7,0xC3};
static const BYTE kB2BrutePre[] = {0x83,0xEC,0x18,0x53,0x55,0x8B,0xE9,0x8B,0x45,0x30,0x8B,0x4D,0x2C,0x56,0x8B,0x71,0x0C};
static const BYTE kB2CastPre[] = {0x8B,0x81,0x88,0,0,0,0x85,0xC0,0x74,0x13,0x8B,0x4C,0x24,0x04,0x8B,0x50,0x0C,0x51,0x52,0x8B,0xC8,0xE8,0x66,0xAE,0x02,0,0xC2,0x04,0,0xE9,0x2E,0xF6,0xFF,0xFF};

struct B2EqEntry { DWORD a, b, fa, fb, logic, frame; BYTE result, used; };
static B2EqEntry* g_b2Eq = NULL;
static tIsEquiv o_b2Eq = NULL;
static LONG g_b2EqProof = 512;
static DWORD g_b2EqChecks = 0;
static bool g_b2EqKill = false;
static DWORD g_b2EqLogic = 0, g_b2EqFrame = 0;
static DWORD b2LogicPointer() { return *(DWORD*)(ULONG_PTR)kB2Logic; }
static DWORD b2LogicFrame(DWORD logic) { return *(DWORD*)(ULONG_PTR)(logic + 0x40); }
static BYTE __fastcall hkB2Equiv(void* self, void* edx, void* other) {
    DWORD a = (DWORD)(ULONG_PTR)self, b = (DWORD)(ULONG_PTR)other;
    if (!a || !b || a == b || g_b2EqKill || !g_mkTid || __readfsdword(0x24) != g_mkTid)
        return o_b2Eq(self, edx, other);
    DWORD logic = b2LogicPointer();
    if (!logic) return o_b2Eq(self, edx, other);
    DWORD frame = b2LogicFrame(logic);
    if (logic != g_b2EqLogic || frame < g_b2EqFrame)
        memset(g_b2Eq, 0, EQ_N * sizeof(B2EqEntry));
    g_b2EqLogic = logic; g_b2EqFrame = frame;
    // Loading/reset work must never seed entries for a later game.
    if (!frame) return o_b2Eq(self, edx, other);
    DWORD fa = eqFinal(a), fb = eqFinal(b);
    B2EqEntry& e = g_b2Eq[((a * 2654435761u) ^ (b * 2246822519u)) >> 19];
    bool hit = e.used && e.a == a && e.b == b && e.fa == fa && e.fb == fb && e.logic == logic && e.frame == frame;
    // Frame-local answers avoid carrying template data changes between logic frames.
    // Keep both override pointers, rather than a lossy combined fingerprint.
    if (hit && !g_b2EqProof && (++g_b2EqChecks & 31) != 0) return e.result;
    BYTE result = o_b2Eq(self, edx, other);
    if (hit) {
        if (result != e.result) {
            g_b2EqKill = true;
            logf("bfme2: equivalence memo mismatch - OFF for this session.");
        } else if (g_b2EqProof && !--g_b2EqProof) {
            logf("bfme2: 512 repeated equivalence answers matched; frame-local memo live (1 in 32 still checked).");
        }
    }
    e.a = a; e.b = b; e.fa = fa; e.fb = fb; e.logic = logic; e.frame = frame; e.result = result; e.used = 1;
    return result;
}

static void installBfme2Hooks() {
    char env[8] = {0};
    bool eqAllowed = !(GetEnvironmentVariableA("AOTR_BFME2_EQ", env, sizeof(env)) && env[0] == '0');
    bool eqOk = false, pickOk = false;
    __try {
        eqOk = !memcmp((void*)kB2Equiv, kB2EquivPre, sizeof(kB2EquivPre)) &&
               !memcmp((void*)kB2Final, kB2FinalPre, sizeof(kB2FinalPre));
        pickOk = !memcmp((void*)kB2Brute, kB2BrutePre, sizeof(kB2BrutePre)) &&
                 !memcmp((void*)kB2Cast, kB2CastPre, sizeof(kB2CastPre));
    } __except (EXCEPTION_EXECUTE_HANDLER) { eqOk = pickOk = false; }
    if (eqAllowed && eqOk) {
        g_b2Eq = (B2EqEntry*)VirtualAlloc(NULL, EQ_N * sizeof(B2EqEntry), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_b2Eq) o_b2Eq = (tIsEquiv)makeTrampoline((BYTE*)kB2Equiv, 6);
        eqOk = g_b2Eq && o_b2Eq;
    } else eqOk = false;
    env[0] = 0;
    // Unlike RotWK's combined installer, this does not require a view-pick hook.
    if (GetEnvironmentVariableA("AOTR_PICKBOX", env, sizeof(env)) && env[0] == '0') pickOk = false;
    if (pickOk) { o_bfCast = (tBfCast)makeTrampoline((BYTE*)kB2Brute, 5); pickOk = o_bfCast != NULL; }
    HANDLE frozen[256]; int count = 0;
    suspendOthers(frozen, &count, 256);
    if (eqOk) eqOk = patchJmp((BYTE*)kB2Equiv, (void*)&hkB2Equiv, 6) != FALSE;
    if (pickOk) pickOk = patchJmp((BYTE*)kB2Brute, (void*)&hkBfCast, 5) != FALSE;
    resumeAll(frozen, count);
    logf("bfme2: equivalence %s; mesh-pick box %s. Shortcuts require stock-result proofs; RotWK hooks remain disabled.",
         eqOk ? "hooked (512 proofs)" : "OFF", pickOk ? "hooked (3000 proofs)" : "OFF");
}
