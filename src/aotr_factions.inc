// ================================================================ any number of factions with a working AI (v61, 2026-10-08)
// What the limit is. A skirmish map lists its "sides" (players): the neutral one, PlyrCivilian, PlyrCreeps, the slot
// players Player_1..n - and one Skirmish<Faction> player per faction a computer may play (SkirmishMen, SkirmishElves,
// ...). The map's table holds 20 sides: the reader stops at the 20th (0x730B4F: cmp i, 14h) and the list itself is two
// arrays of 20 (SidesList +0x40 and +0x7C4, 0x60 bytes a side). When a game starts, SidesList::prepareForMP_or_Skirmish
// (0x73193D) moves every side but the first three into the "skirmish sides", gives each the AI type of its faction's
// PlayerTemplate (DefaultPlayerAIType -> playeraitypes.ini -> the AI's script library), adds SkirmishHuman and imports
// the libraries into them. Then Player::initFromDict (0x6B07EF) looks, for every computer player, for a skirmish side
// whose faction has the player's Side (0x6B090B..0x6B0997, again in 0x6ACFA3). Found: the player becomes a computer
// player and takes that side's scripts and teams. Not found: it is made a HUMAN-type player (0x6B09F5 -> 0x6B0A20), no
// AI object, and stands still for the whole game.
// So a faction needs its own Skirmish player in every map, and a map has room for 20 minus the others: Age of the Ring
// 8.x ships 11 factions and its 551 skirmish / War of the Ring maps carry 19 or 20 sides - after SkirmishHuman two or
// three places are left in 533 of them. In every one of those maps the factions' players are empty: no scripts, one
// default team (two in four maps); they only have to exist. (The name does not matter - High Moor's is called
// "SkimishArnor" and Withered Heath's Gondor is "PlyrMen" - only the faction does.) Where there was no room the
// player is missing: the six-player map Jatikko has ten of them in its 20 sides, and a Haradwaith computer is dead there.
//
// What this does. At the one place where the skirmish sides are complete and their libraries not yet imported (the call
// at 0x731D3F), every computer player of this game whose faction has no skirmish side gets one made exactly as a map
// would have carried it: Skirmish<Faction>, not human, that faction, no allies or enemies, the template's AI type, and
// its default team. From there on it is the engine's own code: it imports the library, the player finds its side, takes
// the scripts and the team. A map with the side is untouched (nothing is made, nothing differs). If the table of 20 is
// full, the Skirmish player of a faction nobody plays in this game makes room (nothing can ever look it up).
// Only what a game needs is made, so the number of factions a mod may have is no longer tied to the table.
//
// Network games are no exception (v62). Every machine goes through this place with the same map and the same slots,
// and nothing here looks at a clock, at the machine, or makes a name key the engine would make later - so every
// machine that runs this makes the same sides in the same order. A machine that does not run it makes that player a
// dead human: in a network game every player needs this, as with any change to what the game does. (v61 left network
// games alone to keep a game shared with a player WITHOUT the accelerator in sync. Such a mixed game has never been
// played and is not something this project is built around.)
// A game's sides are rebuilt the same way when it is loaded from a save, so a save loads as it was made as long as
// this is in the same state (a save from a game in which such a computer player was dead needs FactionAI = 0 to load).
// Only game.dat's code and the engine's own containers are used; nothing is written to the map or any file.
struct FaStr { void* p; };                                        // the engine's AsciiString: one pointer, text at +8
static __forceinline const char* faC(const FaStr* s) { return (s && s->p) ? (const char*)s->p + 8 : ""; }
typedef ULONG_PTR (__fastcall* tFaT0)(void*, void*);
typedef ULONG_PTR (__fastcall* tFaT1)(void*, void*, ULONG_PTR);
typedef ULONG_PTR (__fastcall* tFaT2)(void*, void*, ULONG_PTR, ULONG_PTR);
typedef ULONG_PTR (__fastcall* tFaT3)(void*, void*, ULONG_PTR, ULONG_PTR, ULONG_PTR);
#define FA0(fn, self)          (((tFaT0)(ULONG_PTR)(fn))((void*)(ULONG_PTR)(self), NULL))
#define FA1(fn, self, a)       (((tFaT1)(ULONG_PTR)(fn))((void*)(ULONG_PTR)(self), NULL, (ULONG_PTR)(a)))
#define FA2(fn, self, a, b)    (((tFaT2)(ULONG_PTR)(fn))((void*)(ULONG_PTR)(self), NULL, (ULONG_PTR)(a), (ULONG_PTR)(b)))
#define FA3(fn, self, a, b, c) (((tFaT3)(ULONG_PTR)(fn))((void*)(ULONG_PTR)(self), NULL, (ULONG_PTR)(a), (ULONG_PTR)(b), (ULONG_PTR)(c)))
enum {
    FA_SITE = 0x00731D3F, FA_IMPORT = 0x007318A1, FA_PREPARE = 0x0073193D,
    FA_KEY = 0x00548930,                                          // key object -> its number (made on first use)
    FA_STR_NEW = 0x004374E0, FA_STR_FREE = 0x00435D50, FA_STR_CMP = 0x004065AA, FA_STR_EMPTY = 0x00401E64,
    FA_DICT_NEW = 0x00714E4F, FA_DICT_FREE = 0x00714C6A, FA_DICT_SETSTR = 0x00715028, FA_DICT_SETBOOL = 0x00714FC4, FA_DICT_SETUSTR = 0x0071504F,
    FA_DICT_GETSTR = 0x00714E6D, FA_DICT_TYPE = 0x00714A4A,
    FA_ADDSIDE = 0x0072EA27, FA_REMOVESIDE = 0x0072F201, FA_ADDTEAM = 0x0072F3FF,
    FA_KEYNAME = 0x00548700, FA_SLOT = 0x00800B55, FA_SLOT_HUMAN = 0x008009A7, FA_SLOT_TAKEN = 0x008009B1, FA_TEMPLATE_N = 0x005FCAD9,
    FA_K_NAME = 0x00DA2F2C, FA_K_HUMAN = 0x00DA2F34, FA_K_DISPLAY = 0x00DA2F44, FA_K_FACTION = 0x00DA2F4C, FA_K_ENEMIES = 0x00DA2F54, FA_K_ALLIES = 0x00DA2F5C,
    FA_K_AITYPE = 0x00DA2FA4, FA_K_TEAMNAME = 0x00DA2AFC, FA_K_TEAMOWNER = 0x00DA2B04, FA_K_TEAMSINGLE = 0x00DA2B0C,
    FA_G_KEYS = 0x00DD90E4, FA_G_TEMPLATES = 0x00DE3B10, FA_G_GAMEINFO = 0x00DE892C, FA_G_LOGIC = 0x00DE412C,
    FA_EMPTY_STR = 0x00DC62B8, FA_EMPTY_USTR = 0x00DF25F0,
    FA_MAX = 20, FA_SIDE = 0x60, FA_SIDES = 0x40, FA_COUNT = 0x3C, FA_TEMPLATE = 0x1DC,
};
static volatile LONG g_faOn = 0;
static volatile LONG g_faGames = 0, g_faMade = 0, g_faRoom = 0, g_faNoRoom = 0, g_faNet = 0, g_faFaults = 0, g_faBadAdd = 0;
static void* g_faImport = (void*)(ULONG_PTR)FA_IMPORT;
static char g_faLast[320];                                        // what was made last (the tests read it)

static __forceinline BYTE* faSideDict(BYTE* sides, LONG i) { return sides + FA_SIDES + i * FA_SIDE + 4; }
// The faction's PlayerTemplate by its name, without making a name key that did not exist (the engine asks by key; a key
// made here earlier than the engine would make it shifts every later key's number).
static BYTE* faTemplateNamed(const FaStr* name) {
    BYTE* store = *(BYTE**)(ULONG_PTR)FA_G_TEMPLATES; BYTE* keys = *(BYTE**)(ULONG_PTR)FA_G_KEYS;
    BYTE* b = *(BYTE**)(store + 0xC); BYTE* e = *(BYTE**)(store + 0x10);
    LONG n = (b && e > b) ? (LONG)((e - b) / FA_TEMPLATE) : 0;
    for (LONG i = 0; i < n; ++i) {
        FaStr* nm = (FaStr*)FA1(FA_KEYNAME, keys, *(LONG*)(b + i * FA_TEMPLATE + 0x10));
        if (nm && FA1(FA_STR_CMP, nm, name) == 0) return (BYTE*)FA1(FA_TEMPLATE_N, store, i);      // (the first of that name, and its last override: as 0x5FCA2E)
    }
    return NULL;
}
static BYTE* faSideTemplate(BYTE* sides, LONG i) {                // the template of side i's faction (NULL: no such faction)
    FaStr f = { NULL };
    FA3(FA_DICT_GETSTR, faSideDict(sides, i), &f, FA0(FA_KEY, FA_K_FACTION), 0);
    BYTE* pt = faTemplateNamed(&f);
    FA0(FA_STR_FREE, &f);
    return pt;
}
// The engine's own question (0x6B091C..0x6B0995): is there a skirmish side whose faction has this Side?
static bool faHasSide(BYTE* sides, const FaStr* side) {
    LONG n = *(LONG*)(sides + FA_COUNT);
    for (LONG i = 0; i < n; ++i) {
        BYTE* pt = faSideTemplate(sides, i);
        if (pt && FA1(FA_STR_CMP, pt + 0x18, side) == 0) return true;
    }
    return false;
}
struct FaPlay { BYTE* pt; LONG slot; bool ai; };
// The table is full: the Skirmish player of a faction nobody plays in this game goes (no player can ask for it by Side,
// and no slot player is called Skirmish...). From the back, so the sides before it keep their places.
static bool faMakeRoom(BYTE* sides, const FaPlay* play, int np, char* gone, int goneCap) {
    for (LONG i = *(LONG*)(sides + FA_COUNT) - 1; i >= 0; --i) {
        FaStr nm = { NULL };
        FA3(FA_DICT_GETSTR, faSideDict(sides, i), &nm, FA0(FA_KEY, FA_K_NAME), 0);
        const char* c = faC(&nm);
        bool shell = !strncmp(c, "Skirmish", 8) && strcmp(c, "SkirmishHuman") != 0;
        if (shell) lstrcpynA(gone, c, goneCap);
        FA0(FA_STR_FREE, &nm);
        if (!shell) continue;
        BYTE* pt = faSideTemplate(sides, i);
        bool played = false;
        for (int k = 0; pt && k < np && !played; ++k) played = FA1(FA_STR_CMP, pt + 0x18, play[k].pt + 0x18) == 0;
        if (played) continue;
        FA1(FA_REMOVESIDE, sides, i);
        return true;
    }
    gone[0] = 0;
    return false;
}
static bool faNameTaken(BYTE* sides, const char* name) {
    LONG n = *(LONG*)(sides + FA_COUNT); bool taken = false;
    for (LONG i = 0; i < n && !taken; ++i) {
        FaStr nm = { NULL };
        FA3(FA_DICT_GETSTR, faSideDict(sides, i), &nm, FA0(FA_KEY, FA_K_NAME), 0);
        taken = !strcmp(faC(&nm), name);
        FA0(FA_STR_FREE, &nm);
    }
    return taken;
}
// One skirmish side as a map carries it, and its default team (as 0x731B9C..0x731D34 makes SkirmishHuman).
static LONG faAddSide(BYTE* sides, BYTE* teams, BYTE* pt, char* nameOut, int nameCap) {
    BYTE* keys = *(BYTE**)(ULONG_PTR)FA_G_KEYS;
    FaStr* fac = (FaStr*)FA1(FA_KEYNAME, keys, *(LONG*)(pt + 0x10));
    const char* fc = faC(fac);
    char name[160], team[176];
    wsprintfA(name, "Skirmish%.80s", !strncmp(fc, "Faction", 7) && fc[7] ? fc + 7 : fc);
    for (int k = 2; faNameTaken(sides, name) && k < 100; ++k) wsprintfA(name, "Skirmish%.80s_%d", !strncmp(fc, "Faction", 7) && fc[7] ? fc + 7 : fc, k);
    wsprintfA(team, "team%s", name);
    FaStr sName = { NULL }, sTeam = { NULL };
    FA1(FA_STR_NEW, &sName, name); FA1(FA_STR_NEW, &sTeam, team);
    void* d = NULL;
    FA1(FA_DICT_NEW, &d, 0);
    FA2(FA_DICT_SETSTR, &d, FA0(FA_KEY, FA_K_NAME), &sName);
    FA2(FA_DICT_SETBOOL, &d, FA0(FA_KEY, FA_K_HUMAN), 0);
    FA2(FA_DICT_SETUSTR, &d, FA0(FA_KEY, FA_K_DISPLAY), FA_EMPTY_USTR);
    FA2(FA_DICT_SETSTR, &d, FA0(FA_KEY, FA_K_FACTION), fac);
    FA2(FA_DICT_SETSTR, &d, FA0(FA_KEY, FA_K_ENEMIES), FA_EMPTY_STR);
    FA2(FA_DICT_SETSTR, &d, FA0(FA_KEY, FA_K_ALLIES), FA_EMPTY_STR);
    if (!FA0(FA_STR_EMPTY, pt + 0x1B0)) FA2(FA_DICT_SETSTR, &d, FA0(FA_KEY, FA_K_AITYPE), pt + 0x1B0);       // (0x731B5C: only a template that names one)
    LONG at = (LONG)FA1(FA_ADDSIDE, sides, &d);
    FA0(FA_DICT_FREE, &d);
    if (at >= 0) {
        void* t = NULL;
        FA1(FA_DICT_NEW, &t, 0);
        FA2(FA_DICT_SETSTR, &t, FA0(FA_KEY, FA_K_TEAMNAME), &sTeam);
        FA2(FA_DICT_SETSTR, &t, FA0(FA_KEY, FA_K_TEAMOWNER), &sName);
        FA2(FA_DICT_SETBOOL, &t, FA0(FA_KEY, FA_K_TEAMSINGLE), 1);
        FA1(FA_ADDTEAM, teams, &t);
        FA0(FA_DICT_FREE, &t);
    }
    FA0(FA_STR_FREE, &sTeam); FA0(FA_STR_FREE, &sName);
    lstrcpynA(nameOut, name, nameCap);
    return at;
}
static int faSynth(BYTE* sides, BYTE* teams) {
    BYTE* gi = *(BYTE**)(ULONG_PTR)FA_G_GAMEINFO;
    if (!gi || !*(BYTE**)(ULONG_PTR)FA_G_TEMPLATES || !*(BYTE**)(ULONG_PTR)FA_G_KEYS) return 0;
    LONG n0 = *(LONG*)(sides + FA_COUNT);
    if (n0 < 0 || n0 > FA_MAX) return 0;
    BYTE* store = *(BYTE**)(ULONG_PTR)FA_G_TEMPLATES;
    FaPlay play[8]; int np = 0;
    for (LONG s = 0; s < 8; ++s) {                                // the slots as 0x627D5F reads them when it makes their sides, right after this
        BYTE* slot = (BYTE*)FA1(FA_SLOT, gi, s);
        if (!slot || !FA0(FA_SLOT_TAKEN, slot)) continue;
        bool human = FA0(FA_SLOT_HUMAN, slot) != 0;
        LONG ti = *(LONG*)(slot + 0x18);
        if (ti < 0) continue;                                     // an observer
        BYTE* pt = (BYTE*)FA1(FA_TEMPLATE_N, store, ti);
        if (!pt || pt[0x150] || FA0(FA_STR_EMPTY, pt + 0x18)) continue;      // (0x6B08D6: such a template's player does not look for a side)
        play[np].pt = pt; play[np].slot = s; play[np].ai = !human; np++;
    }
    BYTE* gl = *(BYTE**)(ULONG_PTR)FA_G_LOGIC;
    LONG mode = gl ? *(LONG*)(gl + 0x110) : -1;
    bool net = mode == 1 || mode == 5;
    int made = 0, missing = 0;
    for (int k = 0; k < np; ++k) {
        if (!play[k].ai || faHasSide(sides, (FaStr*)(play[k].pt + 0x18))) continue;
        ++missing;
        const char* sideName = faC((FaStr*)(play[k].pt + 0x18));
        char gone[160]; gone[0] = 0;
        if (*(LONG*)(sides + FA_COUNT) >= FA_MAX) {
            if (!faMakeRoom(sides, play, np, gone, sizeof(gone))) {
                InterlockedIncrement(&g_faNoRoom);
                logf("factions: this map has no Skirmish player for %s (the computer in slot %d) and its table of %d is full of players this game needs - left as it is.", sideName, (int)play[k].slot + 1, (int)FA_MAX);
                continue;
            }
            InterlockedIncrement(&g_faRoom);
        }
        char name[160]; name[0] = 0;
        LONG before = *(LONG*)(sides + FA_COUNT);
        LONG at = faAddSide(sides, teams, play[k].pt, name, sizeof(name));
        bool ok = at == before && *(LONG*)(sides + FA_COUNT) == before + 1 && faHasSide(sides, (FaStr*)(play[k].pt + 0x18));
        if (!ok) { InterlockedIncrement(&g_faBadAdd);
            logf("factions: a Skirmish player for %s could not be added (place %d of %d, %d sides after) - left as the map has it.", sideName, (int)at, (int)before, (int)*(LONG*)(sides + FA_COUNT));
            continue; }
        ++made; InterlockedIncrement(&g_faMade); if (net) InterlockedIncrement(&g_faNet);
        const char* ai = faC((FaStr*)(play[k].pt + 0x1B0));
        wsprintfA(g_faLast, "%s for side %s, AI type %s%s%s", name, sideName, ai[0] ? ai : "(none in the template)", gone[0] ? ", in the place of " : "", gone);
        logf("factions: this map has no Skirmish player for %s - made %s (AI type %s) for the computer in slot %d, as a map would carry it; without it that player stands still for the whole game.%s%s%s%s",
             sideName, name, ai[0] ? ai : "none in the template", (int)play[k].slot + 1,
             gone[0] ? " The map's table of 20 was full: " : "", gone, gone[0] ? ", whose faction nobody plays in this game, made room." : "",
             net ? " This is a network game: every player's game has to do the same (this build, FactionAI on), or it goes out of sync." : "");
    }
    if (made) InterlockedIncrement(&g_faGames);
    return made;
}
static void __stdcall faBeforeImport(BYTE* sides, BYTE* teams) {
    if (!g_faOn) return;
    __try { faSynth(sides, teams); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_faFaults); InterlockedExchange(&g_faOn, 0);
        logf("factions: FAULT %08lX while looking at the game's sides - switched off for this session; the game goes on as the map has it.", GetExceptionCode());
    }
}
// In the place of "call importLibraries" (0x731D3F) in SidesList::prepareForMP_or_Skirmish: ecx = the skirmish sides
// being put together (a SidesList on that function's stack), [ebp-0x290] = their teams.
static __declspec(naked) void faStubImport() {
    __asm {
        push ecx
        lea eax, [ebp - 0x290]
        push eax
        push ecx
        call faBeforeImport
        pop ecx
        jmp dword ptr [g_faImport]
    }
}
static bool faCodeIsKnown(int* which) {
    static const DWORD k[][3] = {
        { 0x0073193D, 0x00731DEC, 0x1A958DBC }, { 0x006B08D6, 0x006B0A2B, 0x56E5046D }, { 0x006ACFA3, 0x006AD05C, 0x69B68D9E },
        { 0x0072EA27, 0x0072EA66, 0x9CA1D518 }, { 0x0072F201, 0x0072F278, 0x20F9EF79 }, { 0x0072F3FF, 0x0072F470, 0x8E6D99AB },
        { 0x00714E42, 0x00714E6D, 0x54629B25 }, { 0x00714C6A, 0x00714CA0, 0x486E8DB7 }, { 0x00714FC4, 0x00715075, 0x8E00C9C9 },
        { 0x00714E6D, 0x00714EB4, 0xFCB81876 }, { 0x00714A4A, 0x00714A66, 0xC561401E }, { 0x00548930, 0x00548950, 0xF6D70EAE },
        { 0x004374E0, 0x00437521, 0x74FC4F91 }, { 0x00435D50, 0x00435D8D, 0x0BCD11CB }, { 0x004065AA, 0x004065D4, 0x2486D6F6 },
        { 0x00401E64, 0x00401E78, 0xA3BBF208 }, { 0x00548700, 0x00548766, 0xF4971C93 }, { 0x008009A7, 0x008009DD, 0xE5C4FF0D },
        { 0x00800B55, 0x00800B74, 0x964D8292 }, { 0x005FCAD9, 0x005FCB17, 0xA8F3B33D }, { 0x00465AF4, 0x00465B02, 0xC2560986 },
        { 0x00627E20, 0x00627E7F, 0x209E82EC }, { 0x007318A1, 0x007318F4, 0x82A4CAB6 }, { 0x0073161C, 0x0073173A, 0x87564A60 },
    };
    *which = -1;
    __try {
        for (int i = 0; i < (int)(sizeof(k) / sizeof(k[0])); ++i)
            if (tpFnv((const BYTE*)(ULONG_PTR)k[i][0], (const BYTE*)(ULONG_PTR)k[i][1]) != k[i][2]) { *which = i; return false; }
        if (!rtCallTo(FA_SITE, FA_IMPORT)) { *which = 100; return false; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { *which = 101; return false; }
    return true;
}
static void installFactions() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    int want = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES ? (int)GetPrivateProfileIntA("Engine", "FactionAI", 1, ini) : 1;
    char env[8]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_FACTIONS", env, sizeof(env)) && env[0] >= '0' && env[0] <= '9') want = env[0] - '0';
    if (want <= 0) { logf("factions: off - a computer player needs a Skirmish player of its faction in the map (the map's table holds 20 sides), as the game has it."); return; }
    int which = 0;
    if (!faCodeIsKnown(&which)) { logf("factions: the game's code for a map's sides is not what this build has (check %d) - nothing changed.", which); return; }
    HANDLE fz[256]; int nf = 0;
    suspendOthers(fz, &nf, 256);
    rtSetCall(FA_SITE, (void*)&faStubImport);
    resumeAll(fz, nf);
    InterlockedExchange(&g_faOn, 1);
    logf("factions: installed - a computer player whose faction has no Skirmish player in the map gets one made at game start, exactly as a map would carry it (the map's table holds 20 sides, and a mod's factions no longer have to fit into it). In a network game every player needs this.");
}

// ---------------------------------------------------------------- offline test (gamemap_test.exe <dll> AotrFactionTest)
// game.dat's image is mapped at its base in the test process; nothing of the game runs. The test gives the engine's
// code what it needs to work on sides - its imports from kernel32 and msvcr71, a memory manager, a name key
// generator made by the engine's own constructor - and stand-ins for what an INI would have filled: player templates,
// AI types (none), the game's slots. Then the ENGINE's prepareForMP_or_Skirmish runs on a list of sides made the way
// a map reader makes it, and the ENGINE's own search (0x6ACFA3, what decides whether a computer player lives) is asked.
static LONG g_faTAllocs = 0, g_faTFrees = 0;
static void* __cdecl faTAlloc(unsigned size, int kind, unsigned x) {
    unsigned* p = (unsigned*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 8);
    if (!p) return NULL;
    p[0] = size; p[1] = 0xFAC710A5; g_faTAllocs++;
    return p + 2;
}
static void __cdecl faTFree(void* q, int kind) { if (q) { g_faTFrees++; HeapFree(GetProcessHeap(), 0, (unsigned*)q - 2); } }
static void* __cdecl faTRealloc(void* q, unsigned size, int kind, unsigned x) {
    void* n = faTAlloc(size, kind, x);
    if (q && n) { unsigned old = ((unsigned*)q)[-2]; memcpy(n, q, old < size ? old : size); faTFree(q, kind); }
    return n;
}
static int faTBind(char* why, int nWhy) {                         // the image's imports from kernel32 and msvcr71 (nothing else is called)
    BYTE* base = (BYTE*)(ULONG_PTR)0x00400000;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    int bound = 0;
    for (; d->Name; ++d) {
        const char* nm = (const char*)(base + d->Name);
        HMODULE h = NULL;
        if (!lstrcmpiA(nm, "kernel32.dll")) h = GetModuleHandleA("kernel32.dll");
        else if (!lstrcmpiA(nm, "msvcr71.dll")) { h = LoadLibraryA("msvcr71.dll"); if (!h) h = LoadLibraryA("C:\\AgeoftheRing\\rotwk\\msvcr71.dll"); if (!h) { lstrcpynA(why, "msvcr71.dll could not be loaded", nWhy); return -1; } }
        else continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            FARPROC f = (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) ? GetProcAddress(h, (LPCSTR)(names->u1.Ordinal & 0xFFFF))
                                                                 : GetProcAddress(h, (LPCSTR)((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name);
            if (f) { iat->u1.Function = (DWORD)(ULONG_PTR)f; ++bound; }
        }
    }
    return bound;
}
struct FaTFault { DWORD code, eip, addr, esp, stack[10]; };
static FaTFault g_faTFault;
static int faTFilter(EXCEPTION_POINTERS* ep) {
    g_faTFault.code = ep->ExceptionRecord->ExceptionCode; g_faTFault.eip = (DWORD)(ULONG_PTR)ep->ExceptionRecord->ExceptionAddress;
    g_faTFault.addr = ep->ExceptionRecord->NumberParameters >= 2 ? (DWORD)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    g_faTFault.esp = ep->ContextRecord->Esp;
    __try { for (int i = 0; i < 10; ++i) g_faTFault.stack[i] = ((DWORD*)(ULONG_PTR)ep->ContextRecord->Esp)[i]; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // the return addresses up the frame chain say more than the top of the stack
    __try { DWORD* f = (DWORD*)(ULONG_PTR)ep->ContextRecord->Ebp; for (int i = 0; i < 10 && f; ++i) { g_faTFault.stack[i] = f[1]; f = (DWORD*)(ULONG_PTR)f[0]; } } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return EXCEPTION_EXECUTE_HANDLER;
}
static BYTE* g_faTKeys = NULL;
static void faTEnv() {
    *(DWORD*)(ULONG_PTR)0x00E09070 = 0xFFFFFFFF; *(DWORD*)(ULONG_PTR)0x00E0906C = 0xFFFFFFFF;      // the image's atexit list: "use the runtime's"
    *(void**)(ULONG_PTR)0x00DC5E44 = (void*)&faTAlloc; *(void**)(ULONG_PTR)0x00DC5E3C = (void*)&faTFree; *(void**)(ULONG_PTR)0x00DC5E40 = (void*)&faTRealloc;
    g_faTKeys = (BYTE*)faTAlloc(0x2BF84, 0, 0);
    FA0(0x00548B7E, g_faTKeys);                                    // NameKeyGenerator::NameKeyGenerator
    *(BYTE**)(ULONG_PTR)FA_G_KEYS = g_faTKeys;
    ((void (__fastcall*)(void*, void*))(*(void***)g_faTKeys)[1])(g_faTKeys, NULL);                  // init()
    *(BYTE**)(ULONG_PTR)0x00DE3D5C = (BYTE*)faTAlloc(0x40, 0, 0);                                  // the AI types: none (no library is read in this test)
    BYTE* gl = (BYTE*)faTAlloc(0x400, 0, 0); *(LONG*)(gl + 0x110) = 2;                             // a skirmish
    *(BYTE**)(ULONG_PTR)FA_G_LOGIC = gl;
}
struct FaTFaction { const char* name; const char* side; const char* ai; BYTE flag150; };
static BYTE* faTTemplates(const FaTFaction* f, int n) {
    BYTE* st = (BYTE*)faTAlloc(0x40, 0, 0); BYTE* e = (BYTE*)faTAlloc(n * FA_TEMPLATE + 16, 0, 0);
    *(BYTE**)(st + 0xC) = e; *(BYTE**)(st + 0x10) = e + n * FA_TEMPLATE;
    for (int i = 0; i < n; ++i) { BYTE* t = e + i * FA_TEMPLATE;
        *(LONG*)(t + 0x10) = (LONG)FA1(0x005487EC, g_faTKeys, f[i].name);                           // nameToKey(const char*)
        FA1(FA_STR_NEW, t + 0x18, f[i].side); FA1(FA_STR_NEW, t + 0x1B0, f[i].ai);
        t[0x150] = f[i].flag150; }
    *(BYTE**)(ULONG_PTR)FA_G_TEMPLATES = st;
    return st;
}
static void faTSlots(const int* tmpl, const int* state) {         // 8 slots: template number (-1 an observer), state (0 open, 2..5 a computer, 6 a human)
    static BYTE* gi = NULL;
    if (!gi) { gi = (BYTE*)faTAlloc(0x100, 0, 0); for (int i = 0; i < 8; ++i) *(BYTE**)(gi + 0x18 + 4 * i) = (BYTE*)faTAlloc(0x200, 0, 0); }
    for (int i = 0; i < 8; ++i) { BYTE* s = *(BYTE**)(gi + 0x18 + 4 * i);
        *(LONG*)(s + 4) = state[i]; *(LONG*)(s + 0x18) = tmpl[i]; *(LONG*)(s + 0x10) = i; s[0x1AC] = 1; }
    *(BYTE**)(ULONG_PTR)FA_G_GAMEINFO = gi;
}
static void faTSide(BYTE* S, const char* name, const char* faction, bool human) {   // a side and its default team, as a map has them
    FaStr sn = { NULL }, sf = { NULL }, st = { NULL }; char team[200]; wsprintfA(team, "team%s", name);
    FA1(FA_STR_NEW, &sn, name); FA1(FA_STR_NEW, &sf, faction); FA1(FA_STR_NEW, &st, team);
    void* d = NULL; FA1(FA_DICT_NEW, &d, 0);
    FA2(FA_DICT_SETSTR, &d, FA0(FA_KEY, FA_K_NAME), &sn); FA2(FA_DICT_SETBOOL, &d, FA0(FA_KEY, FA_K_HUMAN), human ? 1 : 0);
    FA2(FA_DICT_SETUSTR, &d, FA0(FA_KEY, FA_K_DISPLAY), FA_EMPTY_USTR); FA2(FA_DICT_SETSTR, &d, FA0(FA_KEY, FA_K_FACTION), &sf);
    FA2(FA_DICT_SETSTR, &d, FA0(FA_KEY, FA_K_ALLIES), FA_EMPTY_STR); FA2(FA_DICT_SETSTR, &d, FA0(FA_KEY, FA_K_ENEMIES), FA_EMPTY_STR);
    FA1(FA_ADDSIDE, S, &d); FA0(FA_DICT_FREE, &d);
    void* t = NULL; FA1(FA_DICT_NEW, &t, 0);
    FA2(FA_DICT_SETSTR, &t, FA0(FA_KEY, FA_K_TEAMNAME), &st); FA2(FA_DICT_SETSTR, &t, FA0(FA_KEY, FA_K_TEAMOWNER), &sn); FA2(FA_DICT_SETBOOL, &t, FA0(FA_KEY, FA_K_TEAMSINGLE), 1);
    FA1(FA_ADDTEAM, S + 0xF44, &t); FA0(FA_DICT_FREE, &t);
    FA0(FA_STR_FREE, &st); FA0(FA_STR_FREE, &sf); FA0(FA_STR_FREE, &sn);
}
// Does the engine find a skirmish side for a player of this Side? (0x6ACFA3 with a stand-in player: only its Side is read.)
static int faTEngineFinds(BYTE* S, const char* side, int* at) {
    static BYTE player[0x400];
    memset(player, 0, sizeof(player));
    FA1(FA_STR_NEW, player + 0x58, side);
    *(BYTE**)(ULONG_PTR)0x00DE77A0 = S;                            // TheSidesList
    LONG idx = -1;
    int r = (int)(FA1(0x006ACFA3, player, &idx) & 0xFF);
    FA0(FA_STR_FREE, player + 0x58);
    if (at) *at = (int)idx;
    return r;
}
static void faTSkirmishName(BYTE* S, int i, int keyObj, char* out, int n) {   // a string of skirmish side i, through the engine's accessor
    BYTE* si = (BYTE*)FA1(0x006AA31E, S, i);
    out[0] = 0; if (!si) return;
    FaStr v = { NULL }; BYTE ex = 0;
    FA3(FA_DICT_GETSTR, si + 4, &v, FA0(FA_KEY, keyObj), &ex);
    lstrcpynA(out, ex ? faC(&v) : "<none>", n);
    FA0(FA_STR_FREE, &v);
}
static int faTTeamOwned(BYTE* rec, const char* owner, char* teamName, int n) {  // teams of that owner in a team list (the engine's own walk: 0x731A56)
    int found = 0; teamName[0] = 0;
    BYTE* arr = *(BYTE**)(rec + 0xC);
    for (LONG i = arr ? (LONG)*(short*)arr : 0; i; ) {
        BYTE* r = *(BYTE**)(rec + 0xC) + i * 16; LONG next = (LONG)*(short*)r;
        FaStr o = { NULL }, t = { NULL };
        FA3(FA_DICT_GETSTR, r + 0xC, &o, FA0(FA_KEY, FA_K_TEAMOWNER), 0);
        if (!strcmp(faC(&o), owner)) { ++found; FA3(FA_DICT_GETSTR, r + 0xC, &t, FA0(FA_KEY, FA_K_TEAMNAME), 0); lstrcpynA(teamName, faC(&t), n); FA0(FA_STR_FREE, &t); }
        FA0(FA_STR_FREE, &o);
        i = next;
    }
    return found;
}
static BYTE* faTMap(const char* const* shells, int nShells, int slotPlayers, bool civilians) {   // a map's sides, in a map's order
    BYTE* S = (BYTE*)faTAlloc(0x11B0, 0, 0);
    FA0(0x007307D0, S);                                            // SidesList::SidesList
    faTSide(S, "", "", false);
    if (civilians) { faTSide(S, "PlyrCivilian", "FactionCivilian", false); faTSide(S, "PlyrCreeps", "FactionCivilian", false); }
    for (int i = 0; i < slotPlayers; ++i) { char n[32]; wsprintfA(n, "Player_%d", i + 1); faTSide(S, n, "FactionCivilian", false); }
    faTSide(S, "PlyrNeutral", "FactionNeutral", true);
    for (int i = 0; i < nShells; ++i) { char n[96], f[96]; wsprintfA(n, "Skirmish%s", shells[i]); wsprintfA(f, "Faction%s", shells[i]); faTSide(S, n, f, false); }
    return S;
}
static int faTestBody(char* out, int nOut) {
    DWORD fl = 0; char why[200]; why[0] = 0;
    int bound = faTBind(why, sizeof(why));
    if (bound < 0) { wsprintfA(out, "factions: %s", why); return 1; }
    int which = 0;
    bool known = faCodeIsKnown(&which);
    if (!known) { wsprintfA(out, "factions: the code check fails on this game.dat (check %d)", which); return 1; }
    faTEnv();
    // Eleven factions as Age of the Ring has them, a twelfth and thirteenth a mod adds (their own Sides and AI types), one
    // faction that shares another's Side, one whose template names no AI type, one of the kind that never looks for a side.
    static const FaTFaction kF[] = {
        { "FactionMen", "Men", "MenSkirmishAI", 0 }, { "FactionRohan", "Rohan", "MenSkirmishAI", 0 }, { "FactionDwarves", "Dwarves", "DwarvesSkirmishAI", 0 },
        { "FactionElves", "Elves", "ElvesSkirmishAI", 0 }, { "FactionArnor", "Arnor", "ArnorSkirmishAI", 0 }, { "FactionMirkwood", "Mirkwood", "MirkwoodSkirmishAI", 0 },
        { "FactionMordor", "Mordor", "MordorSkirmishAI", 0 }, { "FactionIsengard", "Isengard", "IsengardSkirmishAI", 0 }, { "FactionWild", "Wild", "WildSkirmishAI", 0 },
        { "FactionGuldur", "Guldur", "MordorSkirmishAI", 0 }, { "FactionAngmar", "Angmar", "AngmarSkirmishAI", 0 },
        { "FactionHarad", "Harad", "HaradSkirmishAI", 0 }, { "FactionRhun", "Rhun", "RhunSkirmishAI", 0 },           // 11, 12: new factions, no map has them
        { "FactionTutorial", "Rohan", "MenSkirmishAI", 0 },                                                           // 13: another faction of the Side Rohan
        { "FactionDale", "Dale", "", 0 },                                                                             // 14: no AI type in the template
        { "FactionObserver", "Observer", "", 1 }, { "FactionCivilian", "Civilian", "", 0 }, { "FactionNeutral", "Neutral", "", 0 },
    };
    faTTemplates(kF, (int)(sizeof(kF) / sizeof(kF[0])));
    static const char* const kAll[11] = { "Men", "Elves", "Dwarves", "Isengard", "Mordor", "Wild", "Angmar", "Rohan", "Arnor", "Mirkwood", "Guldur" };
    static const char* const kNoArnor[10] = { "Men", "Elves", "Dwarves", "Isengard", "Mordor", "Wild", "Angmar", "Rohan", "Mirkwood", "Guldur" };
    // the call site: as the product installs it
    rtSetCall(FA_SITE, (void*)&faStubImport);
    char nm[160], fa[160], ai[160], tn[200]; int at = -1;
    int nSk[12]; memset(nSk, 0, sizeof(nSk));

    // --- 1. A map as Age of the Ring ships it (19 sides), every faction has its player; a human, Gondor and Rivendell computers.
    //        Nothing may be made; with the feature off and on the result is the same.
    { int tm[8] = { 0, 0, 4, -1, -1, -1, -1, -1 }, st[8] = { 6, 3, 4, 0, 0, 0, 0, 0 }; faTSlots(tm, st);
      BYTE* A = faTMap(kAll, 11, 4, true); InterlockedExchange(&g_faOn, 0); FA0(FA_PREPARE, A);
      BYTE* B = faTMap(kAll, 11, 4, true); InterlockedExchange(&g_faOn, 1); LONG m0 = g_faMade; FA0(FA_PREPARE, B);
      nSk[0] = (int)*(LONG*)(A + 0x7C0); nSk[1] = (int)*(LONG*)(B + 0x7C0);
      if (nSk[0] != 17 || nSk[1] != 17 || g_faMade != m0) fl |= 1;
      for (int i = 0; i < 17; ++i) { char a[160], b[160]; faTSkirmishName(A, i, FA_K_NAME, a, 160); faTSkirmishName(B, i, FA_K_NAME, b, 160); if (strcmp(a, b)) fl |= 1;
                                     faTSkirmishName(A, i, FA_K_AITYPE, a, 160); faTSkirmishName(B, i, FA_K_AITYPE, b, 160); if (strcmp(a, b)) fl |= 1; }
      if (!faTEngineFinds(B, "Men", NULL) || !faTEngineFinds(B, "Arnor", NULL)) fl |= 1;
      if (*(LONG*)(B + FA_COUNT) != 3) fl |= 1; }                  // (the neutral side, PlyrCivilian and PlyrCreeps stay as the map's sides)

    // --- 2. The same map without Rivendell's player (as Jatikko is without Haradwaith's): the engine does not find a side
    //        for the Rivendell computer - the dead player - and with the feature it does, with the template's AI type and a team.
    { int tm[8] = { 0, 0, 4, 4, -1, -1, -1, -1 }, st[8] = { 6, 3, 4, 2, 0, 0, 0, 0 }; faTSlots(tm, st);     // two Rivendell computers: one side between them
      BYTE* A = faTMap(kNoArnor, 10, 4, true); InterlockedExchange(&g_faOn, 0); FA0(FA_PREPARE, A);
      if (faTEngineFinds(A, "Arnor", NULL)) fl |= 2;               // stock: not found
      if (!faTEngineFinds(A, "Men", NULL)) fl |= 2;
      BYTE* B = faTMap(kNoArnor, 10, 4, true); InterlockedExchange(&g_faOn, 1); LONG m0 = g_faMade; FA0(FA_PREPARE, B);
      nSk[2] = (int)*(LONG*)(A + 0x7C0); nSk[3] = (int)*(LONG*)(B + 0x7C0);
      if (nSk[2] != 16 || nSk[3] != 17 || g_faMade != m0 + 1) fl |= 4;
      if (!faTEngineFinds(B, "Arnor", &at) || at != 16) fl |= 4;
      faTSkirmishName(B, 16, FA_K_NAME, nm, 160); faTSkirmishName(B, 16, FA_K_FACTION, fa, 160); faTSkirmishName(B, 16, FA_K_AITYPE, ai, 160);
      if (strcmp(nm, "SkirmishArnor") || strcmp(fa, "FactionArnor") || strcmp(ai, "ArnorSkirmishAI")) fl |= 4;
      if (faTTeamOwned(B + 0xF60, "SkirmishArnor", tn, 200) != 1 || strcmp(tn, "teamSkirmishArnor")) fl |= 8;
      if (faTTeamOwned(B + 0xF60, "SkirmishMen", tn, 200) != 1 || faTTeamOwned(B + 0xF60, "SkirmishHuman", tn, 200) != 1) fl |= 8;      // (the map's own, as before)
      for (int i = 0; i < 16; ++i) { char a[160], b[160]; faTSkirmishName(A, i, FA_K_NAME, a, 160); faTSkirmishName(B, i, FA_K_NAME, b, 160); if (strcmp(a, b)) fl |= 8; }
      faTSkirmishName(B, 15, FA_K_NAME, nm, 160); if (strcmp(nm, "SkirmishHuman")) fl |= 8; }

    // --- 3. A human plays the faction without a player, no computer does: nothing is made (a human needs none).
    { int tm[8] = { 4, 0, -1, -1, -1, -1, -1, -1 }, st[8] = { 6, 3, 0, 0, 0, 0, 0, 0 }; faTSlots(tm, st);
      BYTE* B = faTMap(kNoArnor, 10, 4, true); LONG m0 = g_faMade; FA0(FA_PREPARE, B);
      if (*(LONG*)(B + 0x7C0) != 16 || g_faMade != m0) fl |= 0x10; }

    // --- 4. A network game (mode 1 as on a LAN, 5 as on the internet), and a game with two humans in the slots: the same
    //        as in a skirmish - every machine of that game makes the same side.
    { int tm[8] = { 0, 4, -1, -1, -1, -1, -1, -1 }, st[8] = { 6, 3, 0, 0, 0, 0, 0, 0 }; faTSlots(tm, st);
      BYTE* gl = *(BYTE**)(ULONG_PTR)FA_G_LOGIC;
      for (int pass = 0; pass < 2; ++pass) {
          *(LONG*)(gl + 0x110) = pass ? 5 : 1;
          BYTE* B = faTMap(kNoArnor, 10, 4, true); LONG m0 = g_faMade, n0 = g_faNet; FA0(FA_PREPARE, B);
          faTSkirmishName(B, 16, FA_K_NAME, nm, 160);
          if (*(LONG*)(B + 0x7C0) != 17 || g_faMade != m0 + 1 || g_faNet != n0 + 1 || !faTEngineFinds(B, "Arnor", &at) || at != 16 || strcmp(nm, "SkirmishArnor")) fl |= 0x20;
      }
      *(LONG*)(gl + 0x110) = 2;
      int tm2[8] = { 0, 4, 1, -1, -1, -1, -1, -1 }, st2[8] = { 6, 3, 6, 0, 0, 0, 0, 0 }; faTSlots(tm2, st2);
      BYTE* C = faTMap(kNoArnor, 10, 4, true); LONG m0 = g_faMade; FA0(FA_PREPARE, C);
      if (*(LONG*)(C + 0x7C0) != 17 || g_faMade != m0 + 1 || !faTEngineFinds(C, "Arnor", NULL)) fl |= 0x20; }

    // --- 5. The mod's two new factions as computers on an Age of the Ring map of 20 sides (room for two more skirmish
    //        sides), plus the faction without an AI type, an observer, and the kind of template that never looks.
    { static const char* const kTwelve[12] = { "Men", "Elves", "Dwarves", "Isengard", "Mordor", "Wild", "Angmar", "Rohan", "Arnor", "Mirkwood", "Guldur", "Gondor" };
      int tm[8] = { 0, 11, 12, -1, 15, -1, -1, -1 }, st[8] = { 6, 2, 5, 6, 3, 0, 0, 0 };          // (slot 4: an observer)
      faTSlots(tm, st);
      BYTE* A = faTMap(kTwelve, 12, 4, true); InterlockedExchange(&g_faOn, 0); FA0(FA_PREPARE, A);
      if (faTEngineFinds(A, "Harad", NULL) || faTEngineFinds(A, "Rhun", NULL)) fl |= 0x40;
      BYTE* B = faTMap(kTwelve, 12, 4, true); InterlockedExchange(&g_faOn, 1); LONG m0 = g_faMade, r0 = g_faRoom; FA0(FA_PREPARE, B);
      nSk[4] = (int)*(LONG*)(A + 0x7C0); nSk[5] = (int)*(LONG*)(B + 0x7C0);
      if (nSk[4] != 18 || nSk[5] != 20 || g_faMade != m0 + 2 || g_faRoom != r0) fl |= 0x40;
      if (!faTEngineFinds(B, "Harad", &at) || at != 18 || !faTEngineFinds(B, "Rhun", &at) || at != 19 || !faTEngineFinds(B, "Men", NULL)) fl |= 0x40;
      faTSkirmishName(B, 19, FA_K_AITYPE, ai, 160); if (strcmp(ai, "RhunSkirmishAI")) fl |= 0x40; }

    // --- 6. The table full: a map of 20 sides without the civilians (19 of them become skirmish sides, SkirmishHuman is
    //        the 20th). Three computers of factions the map does not have: three players of factions nobody plays go,
    //        from the back; those of the factions in the game stay, and every side that is played is found.
    { static const char* const kFull[14] = { "Men", "Elves", "Dwarves", "Isengard", "Mordor", "Wild", "Angmar", "Rohan", "Arnor", "Mirkwood", "Guldur", "Gondor", "Extra1", "Extra2" };
      int tm[8] = { 0, 11, 12, 14, 10, 13, -1, -1 }, st[8] = { 6, 2, 3, 4, 5, 2, 0, 0 };          // human Gondor; Harad, Rhun, Dale (no AI type), Dol Guldur, and FactionTutorial (Side Rohan: the map has that)
      faTSlots(tm, st);
      BYTE* A = faTMap(kFull, 14, 4, false); InterlockedExchange(&g_faOn, 0); FA0(FA_PREPARE, A);
      BYTE* B = faTMap(kFull, 14, 4, false); InterlockedExchange(&g_faOn, 1); LONG m0 = g_faMade, r0 = g_faRoom; FA0(FA_PREPARE, B);
      nSk[6] = (int)*(LONG*)(A + 0x7C0); nSk[7] = (int)*(LONG*)(B + 0x7C0);
      if (nSk[6] != 20 || nSk[7] != 20 || g_faMade != m0 + 3 || g_faRoom != r0 + 3) fl |= 0x80;
      if (!faTEngineFinds(B, "Harad", NULL) || !faTEngineFinds(B, "Rhun", NULL) || !faTEngineFinds(B, "Dale", &at) || !faTEngineFinds(B, "Guldur", NULL) || !faTEngineFinds(B, "Rohan", NULL) || !faTEngineFinds(B, "Men", NULL)) fl |= 0x80;
      faTSkirmishName(B, at, FA_K_AITYPE, ai, 160); if (strcmp(ai, "<none>")) fl |= 0x80;        // (Dale: no AI type in its template, none on its side)
      // gone: the three last Skirmish players of factions nobody plays - Extra2, Extra1, Gondor (no template of that name) - and nothing else
      int gone = 0, kept = 0;
      for (int i = 0; i < 20; ++i) { faTSkirmishName(A, i, FA_K_NAME, nm, 160); bool still = false;
          for (int j = 0; j < 20; ++j) { char b[160]; faTSkirmishName(B, j, FA_K_NAME, b, 160); if (!strcmp(nm, b)) still = true; }
          if (still) ++kept; else { ++gone; if (strcmp(nm, "SkirmishExtra2") && strcmp(nm, "SkirmishExtra1") && strcmp(nm, "SkirmishGondor")) fl |= 0x100; } }
      if (gone != 3 || kept != 17) fl |= 0x100;
      if (faTEngineFinds(B, "Mirkwood", NULL) == 0 || faTEngineFinds(B, "Arnor", NULL) == 0) fl |= 0x100;      // (not played, but there was no need to take them)
      nSk[8] = gone; }

    // --- 7. No room to make: every Skirmish player of the full table is of a faction in the game.
    { static const char* const kTight[2] = { "Men", "Elves" };
      int tm[8] = { 0, 3, 11, -1, -1, -1, -1, -1 }, st[8] = { 6, 2, 3, 0, 0, 0, 0, 0 };
      faTSlots(tm, st);
      BYTE* B = (BYTE*)faTAlloc(0x11B0, 0, 0); FA0(0x007307D0, B);
      faTSide(B, "", "", false);
      for (int i = 0; i < 17; ++i) { char n[32]; wsprintfA(n, "Scripted_%d", i + 1); faTSide(B, n, "FactionCivilian", false); }
      faTSide(B, "SkirmishMen", "FactionMen", false); faTSide(B, "SkirmishElves", "FactionElves", false);
      LONG m0 = g_faMade, z0 = g_faNoRoom; FA0(FA_PREPARE, B);
      if (*(LONG*)(B + 0x7C0) != 20 || g_faMade != m0 || g_faNoRoom != z0 + 1 || faTEngineFinds(B, "Harad", NULL) || !faTEngineFinds(B, "Men", NULL) || !faTEngineFinds(B, "Elves", NULL)) fl |= 0x200; }

    // --- 8. The name is taken: the map has a player called SkirmishHarad, of another faction (a map maker's slip). The new
    //        one gets another name, and its team with it; the engine finds it by its faction all the same.
    { int tm[8] = { 0, 11, -1, -1, -1, -1, -1, -1 }, st[8] = { 6, 4, 0, 0, 0, 0, 0, 0 }; faTSlots(tm, st);
      BYTE* B = (BYTE*)faTAlloc(0x11B0, 0, 0); FA0(0x007307D0, B);
      faTSide(B, "", "", false); faTSide(B, "PlyrCivilian", "FactionCivilian", false); faTSide(B, "PlyrCreeps", "FactionCivilian", false);
      faTSide(B, "SkirmishMen", "FactionMen", false); faTSide(B, "SkirmishHarad", "FactionCivilian", false); faTSide(B, "SkimishRohan", "FactionRohan", false);
      LONG m0 = g_faMade; FA0(FA_PREPARE, B);
      if (*(LONG*)(B + 0x7C0) != 5 || g_faMade != m0 + 1 || !faTEngineFinds(B, "Harad", &at) || at != 4 || !faTEngineFinds(B, "Rohan", NULL)) fl |= 0x400;      // (a player of the faction under any name will do: "SkimishRohan")
      faTSkirmishName(B, 4, FA_K_NAME, nm, 160); faTSkirmishName(B, 4, FA_K_FACTION, fa, 160);
      if (strcmp(nm, "SkirmishHarad_2") || strcmp(fa, "FactionHarad") || faTTeamOwned(B + 0xF60, "SkirmishHarad_2", tn, 200) != 1 || strcmp(tn, "teamSkirmishHarad_2")) fl |= 0x400;
      if (faTTeamOwned(B + 0xF60, "SkirmishHarad", tn, 200) != 1) fl |= 0x400; }

    wsprintfA(out, "factions: %s (flags %03lX) - the engine's own preparation and its own search, on sides made as a map has them: a map with every faction's player - nothing made, the %d skirmish sides the same with and without; "
                   "a map without Rivendell's player - the engine finds no side for that computer (the dead player), with this it finds %s with its AI type and team (%d sides, were %d); a human of that faction - nothing made; "
                   "a network game (LAN, internet) or two humans - made just the same; two new factions on a map with 12 players - both made (%d sides, were %d); a full table - %d players of factions nobody plays made room, every played side found; "
                   "a table full of needed players - left alone, said so; a name already taken - another name, found all the same | %d imports bound, %d allocations",
             fl ? "FAILED" : "all as it must be", fl, nSk[1], "SkirmishArnor", nSk[3], nSk[2], nSk[5], nSk[4], nSk[8], bound, (int)g_faTAllocs);
    return fl ? 1 : 0;
}
// ---- the mod's own maps. A text file (AOTR_FACTION_DATA) made from the map files: lines "T<tab>faction<tab>side<tab>AI
// type<tab>playable" and "M<tab>map<tab>name|faction,name|faction,..." (a map's sides in its order). For every map and
// every playable faction: a game with one human and one computer of that faction goes through the engine's preparation
// without this and with it. Without: does the engine find a side for the computer? (No = the dead opponent of the
// stock game.) With: it must find one in every case, nothing may be made where the map had one, and then the
// skirmish sides must be the same list.
struct FaTMapFaction { char name[48], side[48], ai[48]; int playable; };
static int faSweepBody(char* out, int nOut) {
    char path[MAX_PATH]; path[0] = 0;
    if (!GetEnvironmentVariableA("AOTR_FACTION_DATA", path, sizeof(path)) || !path[0]) { lstrcpynA(out, "factions sweep: AOTR_FACTION_DATA names no file", nOut); return 1; }
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { wsprintfA(out, "factions sweep: cannot open %s", path); return 1; }
    DWORD size = GetFileSize(f, NULL), got = 0; char* txt = (char*)HeapAlloc(GetProcessHeap(), 0, size + 2);
    ReadFile(f, txt, size, &got, NULL); CloseHandle(f); txt[got] = 0;
    char why[200]; why[0] = 0; int which = 0;
    if (faTBind(why, sizeof(why)) < 0) { wsprintfA(out, "factions sweep: %s", why); return 1; }
    if (!faCodeIsKnown(&which)) { wsprintfA(out, "factions sweep: the code check fails (check %d)", which); return 1; }
    faTEnv();
    static FaTMapFaction tf[40]; static FaTFaction kf[40]; int nt = 0;
    for (char* p = txt; *p; ) {                                    // the templates first
        char* e = strchr(p, '\n'); if (e) *e = 0;
        if (p[0] == 'T' && p[1] == '\t' && nt < 40) {
            char* a = p + 2; char* b = strchr(a, '\t'); char* c = b ? strchr(b + 1, '\t') : NULL; char* d = c ? strchr(c + 1, '\t') : NULL;
            if (b && c && d) { *b = *c = *d = 0; lstrcpynA(tf[nt].name, a, 48); lstrcpynA(tf[nt].side, b + 1, 48); lstrcpynA(tf[nt].ai, c + 1, 48); tf[nt].playable = d[1] == '1';
                kf[nt].name = tf[nt].name; kf[nt].side = tf[nt].side; kf[nt].ai = tf[nt].ai; kf[nt].flag150 = !lstrcmpA(tf[nt].name, "FactionObserver"); nt++; *b = *c = *d = '\t'; }
        }
        if (!e) break; *e = '\n'; p = e + 1;
    }
    int ntReal = nt;
    static const char* const kNew[9][3] = { { "FactionNew1", "New1", "New1SkirmishAI" }, { "FactionNew2", "New2", "New2SkirmishAI" }, { "FactionNew3", "New3", "New3SkirmishAI" },
        { "FactionNew4", "New4", "New4SkirmishAI" }, { "FactionNew5", "New5", "New5SkirmishAI" }, { "FactionNew6", "New6", "New6SkirmishAI" }, { "FactionNew7", "New7", "New7SkirmishAI" },
        { "FactionNew8", "New8", "New8SkirmishAI" }, { "FactionNew9", "New9", "New9SkirmishAI" } };
    for (int i = 0; i < 9 && nt < 40; ++i, ++nt) { kf[nt].name = kNew[i][0]; kf[nt].side = kNew[i][1]; kf[nt].ai = kNew[i][2]; kf[nt].flag150 = 0;
        lstrcpynA(tf[nt].name, kNew[i][0], 48); lstrcpynA(tf[nt].side, kNew[i][1], 48); lstrcpynA(tf[nt].ai, kNew[i][2], 48); tf[nt].playable = 0; }      // (not part of the first sweep)
    faTTemplates(kf, nt);
    rtSetCall(FA_SITE, (void*)&faStubImport);
    int human = 0; for (int i = 0; i < nt; ++i) if (tf[i].playable) { human = i; break; }
    int human2 = human; for (int i = human + 1; i < nt; ++i) if (tf[i].playable) { human2 = i; break; }
    LONG maps = 0, games = 0, dead = 0, bad = 0, fixedNow = 0, madeWrongly = 0, listDiffers = 0, room = 0; char deadList[1200]; deadList[0] = 0; char firstBad[200]; firstBad[0] = 0;
    for (char* p = txt; *p; ) {
        char* e = strchr(p, '\n'); if (e) *e = 0;
        if (p[0] == 'M' && p[1] == '\t') {
            char* mapName = p + 2; char* tab = strchr(mapName, '\t');
            if (tab) { *tab = 0; ++maps;
                for (int fi = 0; fi < nt; ++fi) { if (!tf[fi].playable) continue;
                    int tm[8] = { fi == human ? human2 : human, fi, -1, -1, -1, -1, -1, -1 }, st[8] = { 6, 3, 0, 0, 0, 0, 0, 0 };
                    faTSlots(tm, st);
                    BYTE* S[2]; char names[2][20][80]; LONG nS[2]; int found[2];
                    LONG m0 = g_faMade, r0 = g_faRoom;
                    for (int pass = 0; pass < 2; ++pass) {
                        S[pass] = (BYTE*)faTAlloc(0x11B0, 0, 0); FA0(0x007307D0, S[pass]);
                        char line[4096]; lstrcpynA(line, tab + 1, sizeof(line));
                        for (char* q = line; ; ) { char* c = strchr(q, ','); if (c) *c = 0;
                            char* bar = strchr(q, '|'); if (bar) { *bar = 0; faTSide(S[pass], q, bar + 1, false); }
                            if (!c) break; q = c + 1; }
                        InterlockedExchange(&g_faOn, pass);
                        FA0(FA_PREPARE, S[pass]);
                        nS[pass] = *(LONG*)(S[pass] + 0x7C0);
                        found[pass] = faTEngineFinds(S[pass], tf[fi].side, NULL);
                        for (int i = 0; i < 20; ++i) faTSkirmishName(S[pass], i, FA_K_NAME, names[pass][i], 80);
                    }
                    ++games;
                    bool ok = true;
                    if (!found[0]) { ++dead; if (lstrlenA(deadList) < 1000) wsprintfA(deadList + lstrlenA(deadList), "%s%s on %s", deadList[0] ? "; " : "", tf[fi].name, mapName); }
                    if (!found[1]) ok = false;                                           // with it, the computer always has its side
                    if (found[0]) { if (g_faMade != m0) { ok = false; ++madeWrongly; }   // the map had one: nothing made, the same list
                                    if (nS[0] != nS[1]) { ok = false; ++listDiffers; } else for (int i = 0; i < 20; ++i) if (strcmp(names[0][i], names[1][i])) { ok = false; ++listDiffers; break; } }
                    else { if (g_faMade == m0 + 1 && found[1]) ++fixedNow; else ok = false; }
                    if (g_faRoom != r0) ++room;
                    if (!ok) { ++bad; if (!firstBad[0]) wsprintfA(firstBad, "%s / %s: found %d -> %d, sides %d -> %d", mapName, tf[fi].name, found[0], found[1], (int)nS[0], (int)nS[1]); }
                    for (int pass = 0; pass < 2; ++pass) { FA0(0x0073060F, S[pass]); faTFree(S[pass], 0); }      // SidesList::~SidesList
                }
                *tab = '\t'; }
        }
        if (!e) break; *e = '\n'; p = e + 1;
    }
    // Second part: the mod grows by nine factions (20 playable) and no map is touched. On every map: a human (the first
    // faction) and seven computers of seven of the new factions - more than any map's table has free. Every one of the
    // eight must find its side; what made room must be players of factions that are not in the game.
    LONG maps2 = 0, bad2 = 0, room2 = 0, made2 = 0, stockDead2 = 0; char firstBad2[200]; firstBad2[0] = 0;
    for (char* p = txt; *p; ) {
        char* e = strchr(p, '\n'); if (e) *e = 0;
        if (p[0] == 'M' && p[1] == '\t') {
            char* mapName = p + 2; char* tab = strchr(mapName, '\t');
            if (tab) { *tab = 0; ++maps2;
                int tm[8] = { human, ntReal + (int)(maps2 % 3), ntReal + 1 + (int)(maps2 % 3), ntReal + 2 + (int)(maps2 % 3), ntReal + 3 + (int)(maps2 % 3), ntReal + 4 + (int)(maps2 % 3), ntReal + 5 + (int)(maps2 % 3), ntReal + 6 + (int)(maps2 % 3) };
                int st[8] = { 6, 2, 3, 4, 5, 2, 3, 4 };
                faTSlots(tm, st);
                for (int pass = 0; pass < 2; ++pass) {
                    BYTE* S = (BYTE*)faTAlloc(0x11B0, 0, 0); FA0(0x007307D0, S);
                    char line[4096]; lstrcpynA(line, tab + 1, sizeof(line));
                    for (char* q = line; ; ) { char* c = strchr(q, ','); if (c) *c = 0;
                        char* bar = strchr(q, '|'); if (bar) { *bar = 0; faTSide(S, q, bar + 1, false); }
                        if (!c) break; q = c + 1; }
                    LONG m0 = g_faMade, r0 = g_faRoom;
                    InterlockedExchange(&g_faOn, pass);
                    FA0(FA_PREPARE, S);
                    int foundAll = faTEngineFinds(S, tf[human].side, NULL) ? 1 : 0, foundNew = 0;
                    for (int k = 1; k < 8; ++k) if (faTEngineFinds(S, tf[tm[k]].side, NULL)) ++foundNew;
                    if (pass == 0) { if (foundNew == 0) ++stockDead2; }
                    else { bool ok = foundAll && foundNew == 7 && g_faMade == m0 + 7 && *(LONG*)(S + 0x7C0) <= 20;
                           made2 += g_faMade - m0; room2 += g_faRoom - r0;
                           if (!ok) { ++bad2; if (!firstBad2[0]) wsprintfA(firstBad2, "%s: the human's side %d, %d of 7 new ones found, %d made, %d sides", mapName, foundAll, foundNew, (int)(g_faMade - m0), (int)*(LONG*)(S + 0x7C0)); } }
                    FA0(0x0073060F, S); faTFree(S, 0);
                }
                *tab = '\t'; }
        }
        if (!e) break; *e = '\n'; p = e + 1;
    }
    if (bad2 || stockDead2 != maps2) bad++;
    wsprintfA(out, "factions sweep: %s - %d maps x %d playable factions = %d games through the engine's own preparation, each without and with this. Without it the engine finds no side for the computer in %d of them (%s). "
                   "With it: a side found in every game; made in exactly those %d (%d times a player nobody plays had to make room); in the other %d games nothing made and the skirmish sides the same list (made wrongly %d, list differs %d)%s%s "
                   "| the mod with nine more factions and no map changed, a human and seven computers of seven new factions on each of the %d maps: without this none of those computers has a side on %d maps; with it all eight players find theirs on %d maps "
                   "(%d sides made, %d players of factions not in the game made room)%s%s",
             bad ? "FAILED" : "all as it must be", (int)maps, (int)(maps ? games / maps : 0), (int)games, (int)dead, deadList[0] ? deadList : "none", (int)fixedNow, (int)room, (int)(games - dead), (int)madeWrongly, (int)listDiffers,
             firstBad[0] ? " | first that is not: " : "", firstBad,
             (int)maps2, (int)stockDead2, (int)(maps2 - bad2), (int)made2, (int)room2, firstBad2[0] ? " | first that is not: " : "", firstBad2);
    return bad || !maps ? 1 : 0;
}
extern "C" __declspec(dllexport) int __cdecl AotrFactionSweep(char* out, int nOut) {
    bool mapped = false;
    __try { mapped = *(const WORD*)(ULONG_PTR)0x00400000 == 0x5A4D && *(const BYTE*)(ULONG_PTR)FA_SITE == 0xE8; } __except (EXCEPTION_EXECUTE_HANDLER) { mapped = false; }
    if (!mapped) { lstrcpynA(out, "factions sweep: game.dat is not mapped in this process (run through gamemap_test.exe)", nOut); return 1; }
    if (*(void**)(ULONG_PTR)0x00DC5E44) { lstrcpynA(out, "factions sweep: this process is a running game (its memory manager is set) - the test is for gamemap_test.exe only", nOut); return 1; }
    int r = 1;
    __try { r = faSweepBody(out, nOut); }
    __except (faTFilter(GetExceptionInformation())) {
        wsprintfA(out, "factions sweep: FAULTED %08lX at %08lX (address %08lX); called from %08lX %08lX %08lX %08lX %08lX %08lX",
                  g_faTFault.code, g_faTFault.eip, g_faTFault.addr, g_faTFault.stack[0], g_faTFault.stack[1], g_faTFault.stack[2], g_faTFault.stack[3], g_faTFault.stack[4], g_faTFault.stack[5]);
        r = 1;
    }
    return r;
}
extern "C" __declspec(dllexport) int __cdecl AotrFactionTest(char* out, int nOut) {
    bool mapped = false;
    __try { mapped = *(const WORD*)(ULONG_PTR)0x00400000 == 0x5A4D && *(const BYTE*)(ULONG_PTR)FA_SITE == 0xE8; } __except (EXCEPTION_EXECUTE_HANDLER) { mapped = false; }
    if (!mapped) { lstrcpynA(out, "factions: game.dat is not mapped in this process (run through gamemap_test.exe)", nOut); return 1; }
    if (*(void**)(ULONG_PTR)0x00DC5E44) { lstrcpynA(out, "factions: this process is a running game (its memory manager is set) - the test is for gamemap_test.exe only", nOut); return 1; }
    int r = 1;
    __try { r = faTestBody(out, nOut); }
    __except (faTFilter(GetExceptionInformation())) {
        wsprintfA(out, "factions: the test FAULTED %08lX at %08lX (address %08lX); called from %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                  g_faTFault.code, g_faTFault.eip, g_faTFault.addr, g_faTFault.stack[0], g_faTFault.stack[1], g_faTFault.stack[2], g_faTFault.stack[3], g_faTFault.stack[4], g_faTFault.stack[5], g_faTFault.stack[6], g_faTFault.stack[7]);
        r = 1;
    }
    return r;
}
