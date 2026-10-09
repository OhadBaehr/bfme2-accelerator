// ================================================================ the crash on leaving a match (v54, 2026-10-07)
// "Exception" every time a skirmish against the AI is left through Exit. It is the game's own: the dumps the game
// writes into its folder show it on days before this DLL existed, at the same two addresses - game.dat+0x3330A and
// +0x3396F, both inside the engine's allocator - about sixty times since.
//
// What happens (read from the dump of 2026-10-07 20:39 and the code around it):
//   * The engine resets its subsystems (0x005B4783). The skirmish AI's reset (0x006A9AA9, vtable 0xC13DFC + 0x24)
//     destroys the AI players one after the other; each owns its attack areas (0x3C bytes each, constructor
//     0x006C67C4, destructor 0x006C63D8) and its teams (base destructor 0x008F26C3).
//   * An attack area owns a "who is near here" scanner (0x5BC bytes: twenty 0x48-byte per-player entries, then
//     name, position, radius; destructor 0x007EDE52). A team holds a COUNTED reference to the area it is assigned
//     to (team + 0x20, count at area + 0x1C) and gives it back when it is released (0x008F2006, tail at
//     0x008F21AF).
//   * An AI player's areas are destroyed with it - whether teams still reference them or not. A team that is
//     destroyed LATER (it belongs to an AI player further down the list) still holds its pointer. Its destructor
//     releases the team, the release evaluates the fight around it once more (0x008F1D88 -> 0x008F1499 ->
//     area 0x006C6623 -> scanner 0x007EE166 -> 0x007EE057), and that clears the scanner's twenty entries
//     (0x007EDB33) - inside memory that was freed moments ago. The first two words of a freed block are the
//     allocator's own list links; they are now zero. The very next thing the function does is ask the partition
//     manager for the objects in range, which allocates, and the allocator walks into the zeroed links:
//     malloc at +0x3396F (unsorted list) or its consolidation at +0x3330A (merging with the neighbouring block).
//     In every dump the block is 0x5C0 bytes - the scanner - and the pointers on the stack are the area (freed,
//     0x40 bytes, right in front of it) and the scanner.
//
// What is done about it, only while the skirmish AI's reset is running (nothing changes during play):
//   1. An attack area destroyed inside that reset is not freed yet: it and its scanner stay as they are until the
//      reset is over, then they are destroyed in the order the engine destroyed them. Whatever still points at
//      them points at intact memory.
//   2. A team whose area has been destroyed that way is treated as the engine treats a team without an area: the
//      evaluation takes the engine's own no-area path (0x008F14D3), and the release only drops the reference.
// Kill switch: [Engine] ExitFix = 0 / AOTR_EXITFIX=0. The log line "exitfix:" says what was met in each reset.

#define EF_MAX 4096
struct EfDead { DWORD z, o; LONG skipped; };
static EfDead* g_efDead = NULL;
static LONG g_efN = 0;                                   // areas held back in the reset that is running
static volatile LONG g_efInReset = 0, g_efOn = 0;
static DWORD g_efTid = 0;
static volatile LONG g_efNReset = 0, g_efNHeld = 0, g_efNStaleEval = 0, g_efNStaleRelease = 0, g_efNLiveEval = 0, g_efNOver = 0, g_efNTwice = 0;
typedef void (__fastcall* tEfThis0)(void* self, void* edx);
typedef void (__cdecl* tEfDelete)(void* p);
static tEfThis0 o_efAreaDtor = NULL, o_efReset = NULL;
static tEfThis0 g_efScannerDtor = (tEfThis0)(ULONG_PTR)0x007EDE52;       // (variables: the offline test puts stand-ins here)
static tEfDelete g_efDelete = (tEfDelete)(ULONG_PTR)0x0042F6A0;

static LONG efFind(DWORD z) { for (LONG i = g_efN - 1; i >= 0; --i) if (g_efDead[i].z == z) return i; return -1; }
static BOOL __fastcall efDeadEval(DWORD z) { if (efFind(z) < 0) { g_efNLiveEval++; return FALSE; } g_efNStaleEval++; return TRUE; }
static BOOL __fastcall efDeadRelease(DWORD z) { if (efFind(z) < 0) return FALSE; g_efNStaleRelease++; return TRUE; }

// In place of "mov ecx, [esi+0x20] / test ecx, ecx" at 0x008F14BD (the team asks its area for the fight around it):
// an area that has been destroyed is no area. Everything but ecx and the flags is as it was (xmm0 is live there).
static __declspec(naked) void efStubEval() {
    __asm {
        mov ecx, [esi + 0x20]
        cmp dword ptr [g_efInReset], 0
        je done
        test ecx, ecx
        jz done
        push eax
        push edx
        sub esp, 16
        movdqu [esp], xmm0
        call efDeadEval
        movdqu xmm0, [esp]
        lea esp, [esp + 16]
        test eax, eax
        pop edx
        pop eax
        mov ecx, [esi + 0x20]
        jz done
        xor ecx, ecx
    done:
        test ecx, ecx
        ret
    }
}
// The same two instructions at 0x008F21AF (the team gives its area back): a destroyed area is only let go of.
static __declspec(naked) void efStubRelease() {
    __asm {
        mov ecx, [esi + 0x20]
        cmp dword ptr [g_efInReset], 0
        je done
        test ecx, ecx
        jz done
        push eax
        push edx
        sub esp, 16
        movdqu [esp], xmm0
        call efDeadRelease
        movdqu xmm0, [esp]
        lea esp, [esp + 16]
        test eax, eax
        pop edx
        pop eax
        mov ecx, [esi + 0x20]
        jz done
        and dword ptr [esi + 0x20], 0
        xor ecx, ecx
    done:
        test ecx, ecx
        ret
    }
}
// The area's destructor (0x006C63D8: destroy and free its scanner). Inside the reset nothing is destroyed yet.
static void __fastcall hkEfAreaDtor(void* z, void* edx) {
    if (g_efInReset && z && __readfsdword(0x24) == g_efTid) {
        if (efFind((DWORD)(ULONG_PTR)z) >= 0) { g_efNTwice++; return; }          // (destroyed a second time: once is what it gets)
        if (g_efN < EF_MAX) {
            EfDead& d = g_efDead[g_efN]; d.z = (DWORD)(ULONG_PTR)z; d.o = *(DWORD*)((BYTE*)z + 0x24); d.skipped = 0;
            g_efN++; g_efNHeld++;
            return;
        }
        g_efNOver++;
    }
    o_efAreaDtor(z, edx);
}
// The three "operator delete(area)" calls that follow that destructor: the memory of a held area stays.
static void __cdecl hkEfAreaDelete(void* p) {
    if (g_efN) { LONG i = efFind((DWORD)(ULONG_PTR)p); if (i >= 0) { g_efDead[i].skipped = 1; return; } }
    g_efDelete(p);
}
static void efRelease() {                                 // the reset is over: now they go, in the order the engine destroyed them
    LONG n = g_efN;
    for (LONG i = 0; i < n; ++i) {
        EfDead d = g_efDead[i];
        if (d.o) { g_efScannerDtor((void*)(ULONG_PTR)d.o, NULL); g_efDelete((void*)(ULONG_PTR)d.o); }
        if (d.skipped) g_efDelete((void*)(ULONG_PTR)d.z);                        // (not seen at one of the three calls: then the engine has freed it itself)
    }
    g_efN = 0;
}
static void __fastcall hkEfReset(void* self, void* edx) {
    if (g_efInReset) { g_efN = 0; }                       // a reset that never came back (never seen): what it held is forgotten, not freed twice
    g_efTid = __readfsdword(0x24);
    LONG h0 = g_efNHeld, e0 = g_efNStaleEval, r0 = g_efNStaleRelease, l0 = g_efNLiveEval;
    InterlockedExchange(&g_efInReset, 1);
    o_efReset(self, edx);
    InterlockedExchange(&g_efInReset, 0);
    efRelease();
    LONG n = InterlockedIncrement(&g_efNReset);
    if (g_efNHeld != h0 || g_efNStaleEval != e0)
        logf("exitfix: skirmish AI reset %d - %d attack areas were destroyed while teams could still reach them and were kept until the reset was over; %d times a team asked an area that had already been destroyed "
             "(the write into freed memory behind the exit crash - each one answered as a team without an area), %d times one gave such an area back; %d asked areas that were alive%s.",
             (int)n, (int)(g_efNHeld - h0), (int)(g_efNStaleEval - e0), (int)(g_efNStaleRelease - r0), (int)(g_efNLiveEval - l0), g_efNOver ? " | MORE areas than the table holds: the rest went the engine's way" : "");
}

static void installExitFix() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    int want = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES ? (int)GetPrivateProfileIntA("Engine", "ExitFix", 1, ini) : 1;
    char env[8]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_EXITFIX", env, sizeof(env)) && env[0]) want = env[0] - '0';
    if (want <= 0) { logf("exitfix: off."); return; }
    static const BYTE kAreaDtor[12] = { 0x56, 0x8B, 0x71, 0x24, 0x85, 0xF6, 0x74, 0x0E, 0x8B, 0xCE, 0xE8, 0x6B };      // push esi; mov esi,[ecx+24]; test esi,esi; je; mov ecx,esi; call scanner dtor
    static const BYTE kReset[14] = { 0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56, 0x8B, 0xF1, 0x57, 0x8D, 0xBE, 0x48, 0x0A, 0x00 }; // push ebp; mov ebp,esp; push ecx; push ebx; push esi; mov esi,ecx; push edi; lea edi,[esi+A48]
    static const BYTE kAsk[7] = { 0x8B, 0x4E, 0x20, 0x85, 0xC9, 0x74, 0x0F };                                           // mov ecx,[esi+20]; test ecx,ecx; je +0F
    static const BYTE kGive[7] = { 0x8B, 0x4E, 0x20, 0x85, 0xC9, 0x74, 0x36 };                                          // mov ecx,[esi+20]; test ecx,ecx; je +36
    static const DWORD kDel[3] = { 0x0090B489, 0x0090B771, 0x0090B1C3 };                                                // operator delete(area) after its destructor
    static const DWORD kDtorCall[3] = { 0x0090B481, 0x0090B769, 0x0090B1B6 };
    bool ok = false; int bad = 0;
    __try {
        ok = true;
        if (memcmp((void*)0x006C63D8, kAreaDtor, sizeof(kAreaDtor))) { ok = false; bad = 1; }
        else if (memcmp((void*)0x006A9AA9, kReset, sizeof(kReset)) || *(DWORD*)0x00C13E20 != 0x006A9AA9) { ok = false; bad = 2; }
        else if (memcmp((void*)0x008F14BD, kAsk, sizeof(kAsk)) || !rtCallTo(0x008F14C9, 0x006C6623)) { ok = false; bad = 3; }
        else if (memcmp((void*)0x008F21AF, kGive, sizeof(kGive)) || *(DWORD*)0x008F21E2 != 0xFF20468B) { ok = false; bad = 4; }      // ... mov eax,[esi+20]; dec [eax+1C]
        else if (!rtCallTo(0x006C63E2, 0x007EDE52) || !rtCallTo(0x006C63E8, 0x0042F6A0) || !rtCallTo(0x008F26E5, 0x008F2006)) { ok = false; bad = 5; }
        for (int i = 0; ok && i < 3; ++i) if (!rtCallTo(kDtorCall[i], 0x006C63D8) || !rtCallTo(kDel[i], 0x0042F6A0)) { ok = false; bad = 6 + i; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; bad = 9; }
    if (!ok) { logf("exitfix: the skirmish AI's code is not what this build has (check %d) - nothing changed.", bad); return; }
    g_efDead = (EfDead*)VirtualAlloc(NULL, EF_MAX * sizeof(EfDead), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    o_efAreaDtor = (tEfThis0)makeTrampoline((BYTE*)0x006C63D8, 6);
    o_efReset = (tEfThis0)makeTrampoline((BYTE*)0x006A9AA9, 6);
    if (!g_efDead || !o_efAreaDtor || !o_efReset) { logf("exitfix: no memory - nothing changed."); return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    patchJmp((BYTE*)0x006C63D8, (void*)&hkEfAreaDtor, 6);
    patchJmp((BYTE*)0x006A9AA9, (void*)&hkEfReset, 6);
    for (int i = 0; i < 3; ++i) rtSetCall(kDel[i], (void*)&hkEfAreaDelete);
    { DWORD op = 0; struct { DWORD at; void* stub; } s[2] = { { 0x008F14BD, (void*)&efStubEval }, { 0x008F21AF, (void*)&efStubRelease } };
      for (int i = 0; i < 2; ++i) { BYTE* q = (BYTE*)(ULONG_PTR)s[i].at;
          if (VirtualProtect(q, 5, PAGE_EXECUTE_READWRITE, &op)) { q[0] = 0xE8; *(DWORD*)(q + 1) = (DWORD)(ULONG_PTR)s[i].stub - (s[i].at + 5); VirtualProtect(q, 5, op, &op); FlushInstructionCache(GetCurrentProcess(), q, 5); } } }
    resumeAll(fz, nf);
    InterlockedExchange(&g_efOn, 1);
    logf("exitfix: installed - while the skirmish AI is reset (leaving a match), attack areas stay in memory until every team that points at them is gone. The game's own crash on Exit (game.dat+0x3330A / +0x3396F) came from a team "
         "writing into an area's scanner after it had been freed.");
}

// ---------------------------------------------------------------- offline test (no game: stand-ins for the engine's functions)
static LONG g_efTestDtor = 0, g_efTestDel = 0, g_efTestOrigDtor = 0; static DWORD g_efTestLastDel[8];
static void __fastcall efTestScannerDtor(void* self, void* edx) { g_efTestDtor++; *(DWORD*)self = 0xDEAD0001; }
static void __cdecl efTestDelete(void* p) { if (g_efTestDel < 8) g_efTestLastDel[g_efTestDel] = (DWORD)(ULONG_PTR)p; g_efTestDel++; }
static void __fastcall efTestOrigAreaDtor(void* z, void* edx) { g_efTestOrigDtor++; }
static void __fastcall efTestOrigReset(void* self, void* edx);
static DWORD g_efTestZF = 0;
// runs one of the two stubs with esi = team; hands back ecx, and whether every other register came back as it went in
static __declspec(naked) DWORD __cdecl efTestCall(void* stub, void* team, DWORD* regsOk) {
    __asm {
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        sub esp, 16
        mov eax, 0x3F800000
        movd xmm0, eax
        movdqu [esp], xmm0
        mov esi, [ebp + 12]
        mov eax, 0x11111111
        mov edx, 0x22222222
        mov ebx, 0x33333333
        mov edi, 0x44444444
        call dword ptr [ebp + 8]
        setz byte ptr [g_efTestZF]
        push ecx
        xor ecx, ecx
        cmp eax, 0x11111111
        setne cl
        cmp edx, 0x22222222
        jz a1
        or ecx, 2
    a1: cmp ebx, 0x33333333
        jz a2
        or ecx, 4
    a2: cmp edi, 0x44444444
        jz a3
        or ecx, 8
    a3: cmp esi, [ebp + 12]
        jz a4
        or ecx, 16
    a4: movd eax, xmm0
        cmp eax, 0x3F800000
        jz a5
        or ecx, 32
    a5: mov eax, [ebp + 16]
        mov [eax], ecx
        pop eax
        add esp, 16
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
    }
}
struct EfTestCtx { BYTE* team[3]; BYTE* area[3]; BYTE* scanner[3]; int fail; DWORD seen[8]; };
static EfTestCtx* g_efTestCtx = NULL;
static void __fastcall efTestOrigReset(void* self, void* edx) {
    // what the engine does, in its order: the first AI player's areas go (areas 0 and 1); then a team of the second
    // player is destroyed - it evaluates and gives back area 0 (destroyed) - and another team asks area 2 (alive)
    EfTestCtx& c = *g_efTestCtx; DWORD ok = 0;
    hkEfAreaDtor(c.area[0], NULL); hkEfAreaDelete(c.area[0]);
    hkEfAreaDtor(c.area[1], NULL); hkEfAreaDelete(c.area[1]);
    if (g_efTestDtor || g_efTestDel || g_efTestOrigDtor) c.fail |= 1;                                      // nothing may have been destroyed or freed yet
    DWORD r = efTestCall((void*)&efStubEval, c.team[0], &ok);   if (r != 0 || !g_efTestZF || ok) c.fail |= 2;                 // destroyed area: "no area", zero flag set
    if (*(DWORD*)(c.team[0] + 0x20) != (DWORD)(ULONG_PTR)c.area[0]) c.fail |= 4;                           // asking does not drop the reference
    r = efTestCall((void*)&efStubRelease, c.team[0], &ok);      if (r != 0 || !g_efTestZF || ok) c.fail |= 8;
    if (*(DWORD*)(c.team[0] + 0x20) != 0) c.fail |= 16;                                                    // giving back does
    if (*(DWORD*)(c.area[0] + 0x1C) != 5) c.fail |= 32;                                                    // and the destroyed area's count is not touched
    r = efTestCall((void*)&efStubEval, c.team[2], &ok);         if (r != (DWORD)(ULONG_PTR)c.area[2] || g_efTestZF || ok) c.fail |= 64;   // a live area: as the engine has it
    r = efTestCall((void*)&efStubRelease, c.team[2], &ok);      if (r != (DWORD)(ULONG_PTR)c.area[2] || g_efTestZF || ok) c.fail |= 128;
    if (*(DWORD*)(c.team[2] + 0x20) != (DWORD)(ULONG_PTR)c.area[2]) c.fail |= 256;                         // (the engine's own code clears it after the stub)
    r = efTestCall((void*)&efStubEval, c.team[1], &ok);         if (r != 0 || !g_efTestZF || ok) c.fail |= 512;               // a team without an area
    hkEfAreaDtor(c.area[1], NULL);                                                                         // destroyed twice: counted, nothing more
    if (*(DWORD*)c.scanner[0] != 0x5CA77E50 || *(DWORD*)c.scanner[1] != 0x5CA77E51) c.fail |= 1024;        // the scanners are untouched so far
}
extern "C" __declspec(dllexport) int __cdecl AotrExitFixTest(char* out, int n) {
    static BYTE mem[9][0x80]; EfTestCtx c; memset(&c, 0, sizeof(c)); memset(mem, 0, sizeof(mem));
    for (int i = 0; i < 3; ++i) { c.team[i] = mem[i]; c.area[i] = mem[3 + i]; c.scanner[i] = mem[6 + i];
        *(DWORD*)(c.area[i] + 0x24) = (DWORD)(ULONG_PTR)c.scanner[i]; *(DWORD*)(c.area[i] + 0x1C) = 5; *(DWORD*)c.scanner[i] = 0x5CA77E50 + i; }
    *(DWORD*)(c.team[0] + 0x20) = (DWORD)(ULONG_PTR)c.area[0]; *(DWORD*)(c.team[1] + 0x20) = 0; *(DWORD*)(c.team[2] + 0x20) = (DWORD)(ULONG_PTR)c.area[2];
    static EfDead tbl[EF_MAX]; EfDead* keepTbl = g_efDead; tEfThis0 keepA = o_efAreaDtor, keepR = o_efReset, keepS = g_efScannerDtor; tEfDelete keepD = g_efDelete;
    g_efDead = tbl; g_efN = 0; o_efAreaDtor = &efTestOrigAreaDtor; o_efReset = &efTestOrigReset; g_efScannerDtor = &efTestScannerDtor; g_efDelete = &efTestDelete;
    g_efTestDtor = g_efTestDel = g_efTestOrigDtor = 0; g_efTestCtx = &c;
    DWORD ok = 0;
    // outside a reset: everything is the engine's - the stubs hand the pointer through, the destructor and the delete run at once
    DWORD r = efTestCall((void*)&efStubEval, c.team[0], &ok);    if (r != (DWORD)(ULONG_PTR)c.area[0] || g_efTestZF || ok) c.fail |= 0x10000;
    r = efTestCall((void*)&efStubRelease, c.team[0], &ok);       if (r != (DWORD)(ULONG_PTR)c.area[0] || g_efTestZF || ok) c.fail |= 0x20000;
    hkEfAreaDtor(c.area[2], NULL); hkEfAreaDelete(c.area[2]);
    if (g_efTestOrigDtor != 1 || g_efTestDel != 1 || g_efTestLastDel[0] != (DWORD)(ULONG_PTR)c.area[2] || g_efN) c.fail |= 0x40000;
    g_efTestOrigDtor = 0; g_efTestDel = 0;
    LONG held0 = g_efNHeld, stale0 = g_efNStaleEval, rel0 = g_efNStaleRelease, twice0 = g_efNTwice;
    hkEfReset(NULL, NULL);                                 // the reset: efTestOrigReset above runs inside it
    // after it: both held areas' scanners destroyed and freed, then the areas, in order; nothing left held
    if (g_efTestDtor != 2 || g_efTestDel != 4 || g_efN || g_efInReset) c.fail |= 0x80000;
    if (g_efTestLastDel[0] != (DWORD)(ULONG_PTR)c.scanner[0] || g_efTestLastDel[1] != (DWORD)(ULONG_PTR)c.area[0] ||
        g_efTestLastDel[2] != (DWORD)(ULONG_PTR)c.scanner[1] || g_efTestLastDel[3] != (DWORD)(ULONG_PTR)c.area[1]) c.fail |= 0x100000;
    if (g_efNHeld - held0 != 2 || g_efNStaleEval - stale0 != 1 || g_efNStaleRelease - rel0 != 1 || g_efNTwice - twice0 != 1 || g_efTestOrigDtor) c.fail |= 0x200000;
    g_efDead = keepTbl; o_efAreaDtor = keepA; o_efReset = keepR; g_efScannerDtor = keepS; g_efDelete = keepD; g_efN = 0; g_efTestCtx = NULL;
    if (out && n > 200) wsprintfA(out, "exit fix: %s (flags %08X) - held %d, asked-after-destroyed %d, given-back-after-destroyed %d, destroyed twice %d; scanner destructors %d, deletes %d",
                                  c.fail ? "FAILED" : "all cases as they must be", (unsigned)c.fail, (int)(g_efNHeld - held0), (int)(g_efNStaleEval - stale0), (int)(g_efNStaleRelease - rel0), (int)(g_efNTwice - twice0),
                                  (int)g_efTestDtor, (int)g_efTestDel);
    return c.fail;
}
