// ================================================================ a shadow map as fine as it is meant to be (v68 / v70, 2026-10-08)
// The game draws its shadows from one depth picture of the scene as the sun sees it (UpdateShadowMap, 0x47D5C9): a
// square R32F render target of MapSize texels stretched over everything the camera could see up to MaxViewDistance,
// down to MinShadowedTerrainHeight. The three numbers are the ShadowMap block of environment.ini (AotR: 1024 texels
// - at least 2048 at the highest detail level - over a reach of 1500, down to height 0), and a map may set its own
// in its map.ini. The pixel shader reads four neighbouring texels and averages their four yes/no verdicts, so the
// edge of a shadow is made of squares one texel big in five shades.
//
// With the mod's default numbers a texel is a pixel or two from the usual camera and that reads as a soft edge. But
// 21 of the mod's maps set their own, and most of those stretch the map over a reach of 6000 - Helm's Deep, the
// Hornburg and Erebor fortress maps - four times the default. A few of them raise MapSize to make up for it (the
// main menu's scene: 4096; Balin's expedition: 6000), most keep 2048: every texel covers four times the ground in
// each direction, about five pixels at 4K from the usual camera and twenty in a scripted close-up, and the shadows
// are visibly built of blocks (reported 2026-10-08 on Helm's Deep). That is the
// game's own picture, with or without this DLL - the game's frame shows it before anything of ours touches it.
//
// What this does: the size the engine WANTS (0x00D9952C, the first field of its current ShadowMap settings; the
// defaults they are copied from are at 0x00D99520) is raised, and the engine does the rest itself. At the top of
// every frame UpdateShadowMap compares the wanted size with the texture it has and, when they differ, releases it
// and makes a new one (0x47B356, 0x47D3EB) - the path a change of map takes - then reads the size it really got
// back into the same field. Nothing else of the shadow code is touched: same light, same reach, same shader; the
// texel size the shader steps by comes from the real texture.
//   * [Engine] ShadowMapMatch = 1: a map whose reach is longer than the default's gets as many texels per unit of
//     reach as the default map has (MapSize x reach / default reach, the next power of two, 8192 at most). Helm's
//     Deep: 2048 over 6000 becomes 8192. A map on the default numbers is not touched at all - its picture is the
//     same to the pixel.
//   * [Engine] ShadowMapScale = 2 or 4: every map's size times that (on top of the above). Finer and with it
//     harder edges everywhere; nobody has asked for it yet, default 1.
// Before a size is asked for, the three surfaces the engine will make (the map, its blur copy, the depth buffer)
// are made here once and given back: an engine that fails to get its map draws no shadows at all until the next
// map, so a size the device cannot give is never asked for - the next smaller one is tried instead.
// Cost: the GPU's only - at 8192 the three surfaces are 16 times the pixels of 2048 and about 770 MB of video
// memory; neither thread does a thing more.
static volatile LONG g_smScale = 1, g_smMatch = 0, g_smOk = 0;
static DWORD g_smOurs = 0, g_smBase = 0, g_smCeil = 8192, g_smTold = 0, g_smReachSeen = 0;
static DWORD* g_smWant = (DWORD*)(ULONG_PTR)0x00D9952C;    // the engine's current settings: [0] MapSize, [1] MaxViewDistance (float), [2] MinShadowedTerrainHeight (float). (A test points this at numbers of its own.)
static DWORD* g_smDef = (DWORD*)(ULONG_PTR)0x00D99520;     // the defaults they are copied from at every map
static DWORD* g_smDevSlot = (DWORD*)(ULONG_PTR)MG_DEVICE;  // where the engine keeps its device
static LONG   g_smLod = -1;                                // test: the detail level (otherwise the game's own, [[0x00DE3B84] + 0x177C])
// Game thread: can the device give all three at this size? Made and given back at once, under the render lock and
// as a direct scope (the calls go through the hooked method tables and pass straight through there).
static bool smTrial(DWORD dev, DWORD v) {
    typedef DWORD (__stdcall* tCT)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, void**, void*);
    typedef DWORD (__stdcall* tCDS)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, void**, void*);
    void* t[2] = { NULL, NULL }; void* ds = NULL; bool ok = true;
    rtExecLock(); g_rtDirectScope++;
    __try {
        DWORD* vt = *(DWORD**)(ULONG_PTR)dev;
        for (int i = 0; i < 2 && ok; ++i) ok = (LONG)((tCT)(ULONG_PTR)vt[23])(dev, v, v, 1, 1 /* D3DUSAGE_RENDERTARGET */, 114 /* D3DFMT_R32F */, 0, &t[i], NULL) >= 0 && t[i];
        if (ok) ok = (LONG)((tCDS)(ULONG_PTR)vt[29])(dev, v, v, 75 /* D3DFMT_D24S8 */, 0, 0, 1, &ds, NULL) >= 0 && ds;
        if (ds) ((DWORD(__stdcall*)(void*))(*(DWORD**)ds)[2])(ds);
        for (int i = 0; i < 2; ++i) if (t[i]) ((DWORD(__stdcall*)(void*))(*(DWORD**)t[i])[2])(t[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    g_rtDirectScope--; LeaveCriticalSection(&g_rtExecCs);
    return ok;
}
// The size this map's shadow map should have: what it has (cur), raised to the default map's texels per unit of
// reach (ShadowMapMatch), times ShadowMapScale. Never above the ceiling.
static DWORD smWanted(DWORD cur, float* reach, float* reach0, DWORD* size0) {
    DWORD want = cur; *reach = 0; *reach0 = 0; *size0 = 0;
    if (g_smMatch) {
        __try {
            DWORD s0 = g_smDef[0]; float r0 = *(const float*)&g_smDef[1], r = *(const float*)&g_smWant[1];
            LONG lod = g_smLod;
            if (lod < 0) { DWORD o = *(DWORD*)(ULONG_PTR)0x00DE3B84; lod = o ? *(LONG*)(ULONG_PTR)(o + 0x177C) : 0; }
            if (lod >= 4 && s0 < 2048) s0 = 2048;           // (the engine's own rule at the highest detail level, 0x47B2E2 / 0x47B6C2)
            *reach = r; *reach0 = r0; *size0 = s0;
            if (s0 >= 256 && s0 <= 8192 && r0 >= 100.0f && r0 <= 100000.0f && r > r0 * 1.05f && r <= 100000.0f) {
                float need = (float)s0 * (r / r0);
                while ((float)want < need * 0.999f && want < g_smCeil) want <<= 1;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { want = cur; }
    }
    for (LONG k = g_smScale; k > 1 && want < g_smCeil; k >>= 1) want <<= 1;
    return want;
}
static void smFrameTick() {                                 // game thread, once a frame (the Present hook)
    if (!g_smOk || (g_smScale <= 1 && !g_smMatch) || __readfsdword(0x24) != g_rtMainTid || g_rtDirectScope) return;
    DWORD cur = g_smWant[0], reachBits = g_smWant[1];
    if (cur == g_smOurs && reachBits == g_smReachSeen) return;      // ours, or numbers already looked at and left alone
    if (g_smBase && reachBits == g_smReachSeen && cur > g_smBase && cur < g_smOurs) {      // asked for g_smOurs, and the engine came back with less: its own ceiling. That is the size then, and nothing above it is asked for again
        logf("shadows: asked for %u x %u, the engine made %u x %u (its own limit) - that is the size it stays at.", g_smOurs, g_smOurs, cur, cur);
        g_smCeil = cur; g_smOurs = cur; g_smBase = 0;
        return;
    }
    g_smReachSeen = reachBits;
    if (cur < 256 || cur >= g_smCeil || (cur & (cur - 1))) { g_smOurs = cur; g_smBase = 0; return; }      // not a size to raise (an odd number is rounded by the engine when it makes the texture, and comes round again)
    DWORD dev = *g_smDevSlot;
    if (!dev) { g_smOurs = 0; return; }
    float reach = 0, reach0 = 0; DWORD size0 = 0;
    DWORD want = smWanted(cur, &reach, &reach0, &size0);
    if (want <= cur) { g_smOurs = cur; g_smBase = 0; return; }       // fine as it is (a map on the default numbers: nothing is touched)
    DWORD v = want;
    while (v > cur && !smTrial(dev, v)) v >>= 1;
    if (v <= cur) {
        g_smOurs = cur; g_smBase = 0;
        if (g_smTold != cur) { g_smTold = cur; logf("shadows: this map's shadow map should be %u x %u, but the device would not make one larger than the map's own %u x %u - it stays as it is.", want, want, cur, cur); }
        return;
    }
    g_smBase = cur; g_smOurs = v;
    g_smWant[0] = v;
    if (g_smMatch && reach0 > 0.0f && reach > reach0 * 1.05f)
        logf("shadows: this map stretches its shadow map of %u x %u over a reach of %d (the mod's default map is %u x %u over %d) - it is made %u x %u%s, so a shadow's edge is as fine here as on the default maps%s. The engine makes the new map itself at the start of the next frame; same light, same reach, same shader.",
             cur, cur, (int)reach, size0, size0, (int)reach0, v, v, v != want ? " (the device would not give more)" : "", g_smScale > 1 ? " and ShadowMapScale finer still" : "");
    else
        logf("shadows: this map's shadow map is %u x %u texels; ShadowMapScale = %d makes it %u x %u%s. The engine makes the new one itself at the start of the next frame; same light, same reach, same shader.",
             cur, cur, (int)g_smScale, v, v, v != want ? " (the device would not give more)" : "");
}
static void installShadowScale() {
    char ini[MAX_PATH]; aotrPath(ini, "bfme2_accel.ini");
    bool have = GetFileAttributesA(ini) != INVALID_FILE_ATTRIBUTES;
    int scale = have ? (int)GetPrivateProfileIntA("Engine", "ShadowMapScale", 1, ini) : 1;
    int match = have ? (int)GetPrivateProfileIntA("Engine", "ShadowMapMatch", 0, ini) : 0;
    char env[16]; env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_SHADOWSCALE", env, sizeof(env)) && env[0]) scale = atoi(env);
    env[0] = 0;
    if (GetEnvironmentVariableA("AOTR_SHADOWMATCH", env, sizeof(env)) && env[0]) match = atoi(env);
    scale = scale >= 4 ? 4 : (scale >= 2 ? 2 : 1); match = match > 0 ? 1 : 0;
    if (scale < 2 && !match) return;                         // off: nothing is read, nothing is hooked
    // this build's code must read the wanted size where it was found (UpdateShadowMap's compare, the two creations), and the numbers must be numbers
    static const BYTE k1[6] = { 0x8B, 0x0D, 0x2C, 0x95, 0xD9, 0x00 }, k2[5] = { 0xA1, 0x2C, 0x95, 0xD9, 0x00 }, k3[6] = { 0x8B, 0x0D, 0x2C, 0x95, 0xD9, 0x00 };
    bool ok = false;
    __try { float r0 = *(const float*)&g_smDef[1];
            ok = !memcmp((const void*)(ULONG_PTR)0x0047D619, k1, 6) && !memcmp((const void*)(ULONG_PTR)0x0047D414, k2, 5) && !memcmp((const void*)(ULONG_PTR)0x0047D4BD, k3, 6) &&
                 g_smWant[0] >= 256 && g_smWant[0] <= 8192 && g_smDef[0] >= 256 && g_smDef[0] <= 8192 && r0 >= 100.0f && r0 <= 100000.0f; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok) { logf("shadows: a finer shadow map was asked for, but this game build does not keep its shadow map numbers where they were found - left alone."); return; }
    InterlockedExchange(&g_smScale, scale); InterlockedExchange(&g_smMatch, match);
    g_rtFrameTick = smFrameTick;
    InterlockedExchange(&g_smOk, 1);
    logf("shadows: %s%s%s - the engine makes the larger map itself; the GPU's work only.",
         match ? "ShadowMapMatch - a map that stretches its shadow map over a longer reach than the mod's default gets a proportionally larger one (8192 at most), so its shadows are as fine as everywhere else; maps on the default numbers are not touched" : "",
         (match && scale > 1) ? "; " : "", scale > 1 ? (scale == 4 ? "ShadowMapScale = 4 - every map's shadow map four times as wide and as high" : "ShadowMapScale = 2 - every map's shadow map twice as wide and as high") : "");
}
// test: the tick against numbers and a device of the test's own (the engine is played by the test)
extern "C" __declspec(dllexport) void __cdecl AotrShadowScaleTest(DWORD* want3, DWORD* devSlot, int scale, DWORD* def3, int match, int lod) {
    g_smWant = want3; g_smDevSlot = devSlot; g_smOurs = 0; g_smBase = 0; g_smCeil = 8192; g_smTold = 0; g_smReachSeen = 0;
    if (def3) g_smDef = def3;
    g_smLod = lod;
    InterlockedExchange(&g_smScale, scale); InterlockedExchange(&g_smMatch, (match && def3) ? 1 : 0); g_rtFrameTick = smFrameTick; InterlockedExchange(&g_smOk, 1);
}
