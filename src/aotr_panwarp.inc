// ================================================================ pan pictures made on the GPU (v59, 2026-10-08)
// Included from aotr_tween.inc (it uses that file's queue of finished pictures, its camera tracking and its ground
// mapping).
//
// WHAT IS LEFT OF A PAN'S UNEVENNESS. The pictures go on screen on a steady beat, but what they SHOW is fixed when
// the game thread draws them: the camera stands where the frame's scroll step put it. A frame that comes late
// (something new streams in, a logic spike) is a picture missing for that long, and the camera stands still on
// screen until it arrives; frames of 33 to 50 ms in a battle are steps of the same size shown at times that are
// not the same. More pictures of the same kind cost the render thread a whole second pass each.
//
// A scroll only moves the camera over the ground, and for that the GPU needs no draw calls at all: the ground is
// a plane, so the picture from camera A as it would be from camera B is one textured rectangle - a plane-to-plane
// mapping of the screen (the same 3x3 the in-between frames use for health bars, twGroundFix). So:
//   * THE CAMERA RUNS ON THE CLOCK. While this is on, the scroll hook (aotr_scroll.inc) steps the camera by the
//     time that really passed since its last step, not by the usual frame time: where the camera is, is a straight
//     line over time, whatever the frames do. Each frame's mark carries the moment its step was taken.
//   * EVERY PICTURE IS SHOWN FROM WHERE THE CAMERA IS AT THAT MOMENT. When a picture's turn comes, the camera's
//     place on that line (the moment of showing, less the time pictures usually take to get there) is compared with
//     the camera the picture was drawn from. Within a pixel and a quarter the picture goes up as it is - byte for
//     byte what it always was. Further off, it is drawn through the mapping instead: one rectangle, one small pixel
//     shader.
//   * NO PICTURE, NO STOP. If the next picture is not there when the beat comes and the camera is scrolling, the
//     picture on screen is shown again from further along the line - up to a quarter of a second ahead of the last
//     frame the game has drawn. The world in it stands still for that long (it is one picture), the pan goes on.
//   * THE INTERFACE STAYS WHERE IT IS. Where the 3D views end (the mark the game thread sets for the in-between
//     frames) the frame is copied aside; a pixel that differs between that copy and the finished frame was drawn by
//     the interface and is taken from the finished frame as it is. Health bars and icons are drawn before that
//     mark, so they move with the ground they stand on.
//   * Where the mapping reaches outside the picture (a strip at the edge the camera moves towards) the scene is
//     left where it was.
// It is used only while the camera scrolls: no copy, no shader and no state change happens for a standing camera
// or for a move the scroll hook did not make (a script, a jump, zoom, rotation) - those are shown exactly as before.
// What it costs: two copies of the screen per frame while scrolling (the scene before the interface, for each of
// the frame's two pictures), and a third copy plus one full-screen rectangle for a picture that is shifted - all
// on the GPU; the render thread's part is a few dozen calls.
// What it is not: the shifted pictures are not drawn by the game. Things that stand up from the ground lean by
// (their height / the camera's height) of the shift, and whatever moves in the world keeps its pose while one
// picture is shown more than once.
// [Engine] PanPictures (default 0) / AOTR_PANWARP; Ctrl+F8 switches it live. Needs InBetweenFrames.
// Proof: rt_harness "panid" (every picture goes through the rectangle with the camera it was drawn from and must
// come out byte for byte the same - the device is left exactly as it was found), tween_test (a picture shifted to
// another camera against the same scene drawn from that camera).

// 89 dwords: psasm.exe (D3DXAssembleShader of d3dx9_27) - the source is in psasm.cpp
static const DWORD kPwPs[89] = {
    0xFFFF0200, 0x05000051, 0xA00F0000, 0x3F800000, 0x3F800000, 0x3F800000, 0x00000000, 0x05000051,
    0xA00F0001, 0x36784D84, 0x00000000, 0x3F800000, 0x00000000, 0x0200001F, 0x80000000, 0xB0030000,
    0x0200001F, 0x80000000, 0xB00F0001, 0x0200001F, 0x90000000, 0xA00F0800, 0x0200001F, 0x90000000,
    0xA00F0801, 0x0200001F, 0x90000000, 0xA00F0802, 0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800,
    0x03000042, 0x800F0001, 0xB0E40000, 0xA0E40801, 0x02000006, 0x80080002, 0xB0FF0001, 0x03000005,
    0x80030002, 0xB0E40001, 0x80FF0002, 0x03000002, 0x80030003, 0xA0AA0001, 0x81E40002, 0x0300000A,
    0x80030003, 0x80E40003, 0x80E40002, 0x0300000A, 0x80010003, 0x80000003, 0x80550003, 0x03000042,
    0x800F0004, 0x80E40002, 0xA0E40802, 0x04000058, 0x800F0004, 0x80000003, 0x80E40004, 0x80E40001,
    0x03000002, 0x800F0005, 0x80E40000, 0x81E40001, 0x03000005, 0x800F0005, 0x80E40005, 0x80E40005,
    0x03000008, 0x80010005, 0x80E40005, 0xA0E40000, 0x03000002, 0x80010005, 0x80000005, 0xA1000001,
    0x04000058, 0x800F0000, 0x80000005, 0x80E40000, 0x80E40004, 0x02000001, 0x800F0800, 0x80E40000,
    0x0000FFFF,
};

// (PwPath, PwW, g_pw, g_pwPend and the switches are declared in aotr_tween.inc, ahead of the code that uses them)
// A picture shown from another camera is the game's own picture with every pixel taken from somewhere else in it.
// Blending the four pixels around that place (v59 to v68) puts the scene exactly where it belongs, to a fraction of a
// pixel - and softens it: the pictures written in the v67 session show stonework that is crisp in the picture as
// drawn and smeared in the same picture as shown, and in a scroll the two kinds alternate (a third of the pictures
// went up as drawn), so the whole screen flickers between sharp and soft. Taking the nearest pixel instead keeps
// every pixel the game drew, unblended; the scene then sits within half a pixel of where it belongs, which at 4K
// nobody sees, and a picture moved by less than half a pixel is the picture itself. [Engine] PanSharp (default 1),
// AOTR_PANSHARP; 0 = blended as before.
static volatile LONG g_pwSharp = 1;
static float g_pwDeadPx = 1.25f;                                    // closer than this: the picture as it is (0.5 when the nearest pixel is taken: nothing is lost by moving it)
static volatile LONG g_pwNShift = 0, g_pwNFill = 0, g_pwNAsIs = 0, g_pwNNoScene = 0, g_pwNTooFar = 0, g_pwNDrawFail = 0, g_pwNCopies = 0, g_pwMaxPx16 = 0, g_pwFillMaxMs = 0, g_pwNTestDraw = 0;
static volatile LONG64 g_pwPxSum16 = 0;
static volatile LONG g_pwBroken = 0;                                // the scene turned out not to be in the frame where the 3D pass ends (pwDiag): stood down for the session
static volatile LONG g_pwNScrolled = 0, g_pwNStood = 0;              // scrolled frames; of them, frames whose camera (as read from the frame) was where the frame before had it
static volatile LONG g_pwBadRun = 0;                                // scrolled frames in a row whose picture could not have been shown from another camera

static void pwFree() {                                              // exec lock held
    PwW& q = g_pw;
    for (int i = 0; i < TW_NSURF; ++i) {
        if (q.scn[i]) { twSurfCall1(q.scn[i], 2); q.scn[i] = 0; }
        if (q.scnTex[i]) { ((TwF1)g_rtO_tex[2])(q.scnTex[i]); q.scnTex[i] = 0; }
        q.scnValid[i] = 0;
    }
    if (q.surfA) { twSurfCall1(q.surfA, 2); q.surfA = 0; }
    if (q.texA) { ((TwF1)g_rtO_tex[2])(q.texA); q.texA = 0; }
    if (q.ps) { ((TwF1)g_rtO_ps[2])(q.ps); q.ps = 0; }
    if (q.sb) { ((TwF1)g_rtO_sb[2])(q.sb); q.sb = 0; }
    q.ready = 0; q.dev = 0; q.lastOk = 0; q.pathN = 0; q.endSet = 0; g_pwLastSurf = 0;
    g_pwPend[0].ok = g_pwPend[1].ok = 0;
}
static bool pwMakeTex(DWORD dev, DWORD* tex, DWORD* surf) {
    TwW& w = g_tw; void* t = NULL, *s = NULL;
    if ((LONG)((TwF9)g_rtO_dev[23])(dev, w.w, w.h, 1, 1 /* D3DUSAGE_RENDERTARGET */, w.fmt, 0, (DWORD)(ULONG_PTR)&t, 0) < 0 || !t) return false;
    if ((LONG)((TwF3)g_rtO_tex[18])((DWORD)(ULONG_PTR)t, 0, (DWORD)(ULONG_PTR)&s) < 0 || !s) { ((TwF1)g_rtO_tex[2])((DWORD)(ULONG_PTR)t); return false; }
    *tex = (DWORD)(ULONG_PTR)t; *surf = (DWORD)(ULONG_PTR)s;
    return true;
}
static bool pwEnsure() {                                            // exec lock held
    TwW& w = g_tw; PwW& q = g_pw;
    if (!w.resDev || w.nOff < 3 || !g_rtO_ps[2] || !g_rtO_tex[18]) return false;
    if (q.ready && q.dev == w.resDev) return true;
    if (q.fail) return false;
    if (q.dev) pwFree();
    DWORD dev = w.resDev; bool ok = !w.ms;
    if (ok) ok = pwMakeTex(dev, &q.texA, &q.surfA);
    for (LONG i = 0; ok && i < w.nOff; ++i) ok = pwMakeTex(dev, &q.scnTex[i], &q.scn[i]);
    if (ok) { void* p = NULL; ok = (LONG)((TwF3)g_rtO_dev[106])(dev, (DWORD)(ULONG_PTR)kPwPs, (DWORD)(ULONG_PTR)&p) >= 0 && p; q.ps = (DWORD)(ULONG_PTR)p; }
    if (ok) { void* p = NULL; ok = (LONG)((TwF3)g_rtO_dev[59])(dev, 1, (DWORD)(ULONG_PTR)&p) >= 0 && p; q.sb = (DWORD)(ULONG_PTR)p; }
    if (!ok) {
        pwFree(); q.fail = 1;
        logf("panpictures: %s - pictures are shown as they are drawn.", w.ms ? "the back buffer is multisampled, which a texture cannot be" : "the device would not make the textures, the shader or the state block");
        return false;
    }
    q.dev = dev; q.ready = 1;
    { struct { UINT w, h, hz; DWORD fmt; } dm = { 0, 0, 0, 0 };          // the screen's refresh rate (the desktop's when windowed)
      LONG64 f = g_pqpf.QuadPart ? g_pqpf.QuadPart : 1;
      bool got = (LONG)((TwF3)g_rtO_dev[8])(dev, 0, (DWORD)(ULONG_PTR)&dm) >= 0 && dm.hz >= 48 && dm.hz <= 500;
      q.refresh = f / (got ? dm.hz : 60);
      q.lead = (LONG)(f * (LONG64)(g_pwLeadUs ? g_pwLeadUs : 2000) / 1000000); q.slotVb = 0; q.lagGrid = 0;
      struct { UINT adapter; DWORD type; HWND wnd; DWORD flags; } cp = { 0, 0, NULL, 0 };                    // the window tells which monitor: its refreshes are watched from now on (aotr_screen.inc)
      if (g_rtO_dev[9] && (LONG)((TwF2)g_rtO_dev[9])(dev, (DWORD)(ULONG_PTR)&cp) >= 0 && !(g_pwTest & 8)) scrStart(cp.wnd); }
    logf("panpictures: ready - %d scene copies and a picture texture of %ux%u, a pixel shader and a state block.", (int)w.nOff, w.w, w.h);
    return true;
}
static __forceinline int pwIndex(DWORD surf) { const TwW& w = g_tw; for (LONG i = 0; i < w.nOff; ++i) if (w.off[i] == surf) return (int)i; return -1; }
// Render thread, where the 3D views of a frame end: the scene as it is now, before the interface is drawn over it.
static void pwSceneCopy(DWORD surf) {
    PwW& q = g_pw;
    if (!g_pwOn || !surf || g_pwBroken || (!q.frameCt && !(g_pwTest & 3))) return;      // only for a frame the scroll hook moved the camera in
    if (!pwEnsure()) return;
    int i = pwIndex(surf);
    if (i < 0) return;
    if ((LONG)((TwF6)g_twO_Stretch)(q.dev, surf, 0, q.scn[i], 0, 0) >= 0) { q.scnValid[i] = 1; g_pwNCopies++; }
}

// ---------------------------------------------------------------- where the camera is at a moment
static void pwPathPush(bool camOk, const float* V, const float* P, DWORD ct, LONG64 now, LONG64 period) {
    PwW& q = g_pw;
    if (!camOk) { q.pathN = 0; q.endSet = 0; return; }
    if (q.pathN >= PW_PATH) { for (int i = 1; i < PW_PATH; ++i) q.path[i - 1] = q.path[i]; q.pathN = PW_PATH - 1; }
    PwPath& e = q.path[q.pathN];
    PwPath* prev = q.pathN > 0 ? &q.path[q.pathN - 1] : NULL;
    memcpy(e.V, V, 64); memcpy(e.P, P, 64);
    e.moving = 0;
    DWORD per = (DWORD)(period > 0 ? period : g_pqpf.QuadPart / 30);
    if (ct) {
        e.t = ct;
        g_pwNScrolled++;
        { LONG wl = (LONG)((DWORD)now - ct);                            // how long this frame took from its scroll step to here (a stall is not the usual)
          if (wl > 0 && wl < (LONG)(g_pqpf.QuadPart / 4)) {
              if (!q.walkLag) q.walkLag = wl;
              else if (wl * 2 > q.walkLag * 5 && ++q.walkOdd < 3) { }
              else { q.walkOdd = 0; q.walkLag += (wl - q.walkLag) / 8; } } }
        if (prev && !memcmp(prev->V, V, 64)) g_pwNStood++;
        if (prev) {
            bool differs = memcmp(prev->V, V, 64) != 0;
            if (differs && twMatNear(prev->V, V, true, false, g_twMaxCamMove) && twMatNear(prev->P, P, true, true, g_twMaxCamMove)) { e.moving = 1; q.endT = e.t; q.endSet = 1; }
            if (!prev->moving || (LONG)(e.t - prev->t) <= 0 || (LONG)(e.t - prev->t) > (LONG)(g_pqpf.QuadPart / 2))
                prev->t = e.t - per;                                // the first step of a scroll is the usual frame time long (the hook's own rule)
        }
    } else {
        e.t = prev ? prev->t + per : (DWORD)now;
        if (q.endSet && (LONG)((DWORD)now - q.endT) > (LONG)(g_pqpf.QuadPart / 4)) q.endSet = 0;      // a scroll that ended a while ago: nothing of it is left to play out
    }
    q.pathN++;
}
// The camera at moment tau. false: nothing is known. *ahead: how far past the newest frame's moment (ticks, 0 if not).
static bool pwWanted(DWORD tau, float* V, float* P, LONG* ahead) {
    PwW& q = g_pw;
    *ahead = 0;
    if (q.pathN < 1) return false;
    const PwPath& n = q.path[q.pathN - 1];
    LONG d = (LONG)(tau - n.t);
    if (d >= 0) {
        if (q.pathN < 2 || !n.moving) { memcpy(V, n.V, 64); memcpy(P, n.P, 64); return true; }
        const PwPath& p = q.path[q.pathN - 2];
        LONG seg = (LONG)(n.t - p.t);
        if (seg <= 0) { memcpy(V, n.V, 64); memcpy(P, n.P, 64); return true; }
        LONG cap = (LONG)(g_pqpf.QuadPart / 4);                     // a quarter of a second ahead of the last frame drawn, no further
        if (d > cap) d = cap;
        *ahead = d;
        float f = (float)d / (float)seg;
        for (int i = 0; i < 16; ++i) { V[i] = n.V[i] + (n.V[i] - p.V[i]) * f; P[i] = n.P[i] + (n.P[i] - p.P[i]) * f; }
        return true;
    }
    for (LONG i = q.pathN - 1; i >= 1; --i) {
        const PwPath& b = q.path[i]; const PwPath& a = q.path[i - 1];
        if ((LONG)(tau - a.t) < 0) continue;
        LONG seg = (LONG)(b.t - a.t);
        if (seg <= 0 || !b.moving) { const PwPath& c = ((LONG)(tau - a.t) * 2 < seg) ? a : b; memcpy(V, c.V, 64); memcpy(P, c.P, 64); return true; }   // a cut, or no move: one or the other
        float f = (float)(LONG)(tau - a.t) / (float)seg;
        for (int k = 0; k < 16; ++k) { V[k] = a.V[k] + (b.V[k] - a.V[k]) * f; P[k] = a.P[k] + (b.P[k] - a.P[k]) * f; }
        return true;
    }
    memcpy(V, q.path[0].V, 64); memcpy(P, q.path[0].P, 64);
    return true;
}
// The mapping from the screen as the wanted camera sees the ground to the screen of the picture's own camera, and how
// far (in pixels) the corners and the middle of the screen move under it. false: no sane mapping.
static bool pwMapping(const float* Vw, const float* Pw, const TwPic& p, float* H, float* px) {
    if (!twGroundFix(Vw, Pw, p.V, p.P, p.gh, H)) return false;
    static const float kc[5][2] = { { -1, 1 }, { 1, 1 }, { -1, -1 }, { 1, -1 }, { 0, 0 } };
    float m = 0;
    for (int i = 0; i < 5; ++i) {
        float x = kc[i][0], y = kc[i][1];
        float sx = x * H[0] + y * H[4] + H[12], sy = x * H[1] + y * H[5] + H[13], sw = x * H[3] + y * H[7] + H[15];
        if (!(sw > 0.2f && sw < 5.0f)) return false;
        float dx = (sx / sw - x) * 0.5f * (float)g_tw.w, dy = (sy / sw - y) * 0.5f * (float)g_tw.h;
        if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
        if (dx > m) m = dx; if (dy > m) m = dy;
    }
    *px = m;
    return true;
}

// ---------------------------------------------------------------- the rectangle
struct PwVtx { float x, y, z, rhw, u, v, tx, ty, tz, tw; };
// The picture p, with the scene under the interface taken from where H says, into the real back buffer. Everything
// the device held is put back. exec lock held, outside a scene.
static bool pwDraw(const TwPic& p, const float* H, bool point) {
    TwW& w = g_tw; PwW& q = g_pw; DWORD dev = w.resDev;
    if (!q.ready || !p.scn) return false;
    if ((LONG)((TwF6)g_twO_Stretch)(dev, p.surf, 0, q.surfA, 0, 0) < 0) return false;       // the finished picture, as a texture
    void* rt0 = NULL, *ds = NULL;
    if ((LONG)((TwF3)g_twO_GetRT)(dev, 0, (DWORD)(ULONG_PTR)&rt0) < 0 || !rt0) return false;
    bool haveDs = (LONG)((TwF2)g_rtO_dev[40])(dev, (DWORD)(ULONG_PTR)&ds) >= 0 && ds;
    ((TwF1)g_rtO_sb[4])(q.sb);                                      // Capture: everything but the targets
    bool ok = (LONG)((TwF3)g_twO_SetRT)(dev, 0, w.bb) >= 0;
    if (ok) {
        ((TwF2)g_rtO_dev[39])(dev, 0);                              // no depth buffer
        struct { DWORD X, Y, W, H; float MinZ, MaxZ; } vp = { 0, 0, w.w, w.h, 0.0f, 1.0f };
        ((TwF2)g_rtO_dev[47])(dev, (DWORD)(ULONG_PTR)&vp);
        static const DWORD rs[][2] = { { 7, 0 }, { 8, 3 }, { 14, 0 }, { 15, 0 }, { 22, 1 }, { 27, 0 }, { 28, 0 }, { 52, 0 }, { 136, 1 }, { 152, 0 }, { 168, 0xF }, { 174, 0 }, { 185, 0 }, { 194, 0 }, { 206, 0 } };
        for (int i = 0; i < (int)(sizeof(rs) / sizeof(rs[0])); ++i) ((TwF3)g_rtO_dev[57])(dev, rs[i][0], rs[i][1]);
        ((TwF2)g_rtO_dev[92])(dev, 0);                              // no vertex shader: the corners are in screen pixels already
        ((TwF2)g_rtO_dev[89])(dev, 0x00080204);                     // XYZRHW | TEX2, the second set four wide
        ((TwF2)g_rtO_dev[107])(dev, q.ps);
        const DWORD tex[3] = { q.texA, p.scn, p.scn };
        for (DWORD s = 0; s < 3; ++s) {
            ((TwF3)g_rtO_dev[65])(dev, s, tex[s]);
            DWORD filt = (s == 2 && !point) ? 2u : 1u;              // the scene from elsewhere is filtered; everything read in place is exact
            const DWORD ss[][2] = { { 1, 3 }, { 2, 3 }, { 3, 3 }, { 5, filt }, { 6, filt }, { 7, 0 }, { 8, 0 }, { 9, 0 }, { 11, 0 } };
            for (int i = 0; i < 9; ++i) ((TwF4)g_rtO_dev[69])(dev, s, ss[i][0], ss[i][1]);
        }
        static const float kc[4][2] = { { -1, 1 }, { 1, 1 }, { -1, -1 }, { 1, -1 } };
        PwVtx v[4];
        for (int i = 0; i < 4; ++i) {
            float x = kc[i][0], y = kc[i][1];
            float sx = x * H[0] + y * H[4] + H[12], sy = x * H[1] + y * H[5] + H[13], sw = x * H[3] + y * H[7] + H[15];
            v[i].x = (x > 0 ? (float)w.w : 0.0f) - 0.5f; v[i].y = (y < 0 ? (float)w.h : 0.0f) - 0.5f; v[i].z = 0.0f; v[i].rhw = 1.0f;
            v[i].u = x > 0 ? 1.0f : 0.0f; v[i].v = y < 0 ? 1.0f : 0.0f;
            v[i].tx = 0.5f * sx + 0.5f * sw; v[i].ty = -0.5f * sy + 0.5f * sw; v[i].tz = 0.0f; v[i].tw = sw;
        }
        if ((LONG)((TwF1)g_twO_BeginScene)(dev) >= 0) {
            ok = (LONG)((TwF5)g_rtO_dev[83])(dev, 5, 2, (DWORD)(ULONG_PTR)v, sizeof(PwVtx)) >= 0;
            ((TwF1)g_twO_EndScene)(dev);
        } else ok = false;
    }
    ((TwF3)g_twO_SetRT)(dev, 0, (DWORD)(ULONG_PTR)rt0);              // the targets first: setting one resets the viewport, which the block then puts back
    ((TwF2)g_rtO_dev[39])(dev, haveDs ? (DWORD)(ULONG_PTR)ds : 0);
    ((TwF1)g_rtO_sb[5])(q.sb);                                      // Apply
    twSurfCall1((DWORD)(ULONG_PTR)rt0, 2);
    if (haveDs) twSurfCall1((DWORD)(ULONG_PTR)ds, 2);
    if (!ok) g_pwNDrawFail++;
    return ok;
}
static const float kPwIdent[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
// At the start of a session a picture that is moved by more than two pixels is read back together with its scene
// copy. A pixel that differs between the two is what the shader takes for the interface. If that is most of the
// screen, either the scene is not in the frame where the 3D pass is marked as ended (then moving the copy moves
// nothing, ever) - or something is being drawn over the whole screen just now: the game fades the picture in when a
// match opens, and the first scroll of a match usually falls right into that. One look cannot tell the two apart
// (v64's first fight: 87 % at 20 s into the match, and the feature stood down for the session on that one picture).
// So up to four pictures are looked at, at least two seconds apart: the first that is fine settles it, and only four
// in a row that are not stand the feature down. While a look is bad nothing is wrong on screen - the shader leaves
// what differs where it is. (A look is two read-backs of the screen: a hitch of a few hundredths of a second.)
static LONG g_pwDiagLeft = 4, g_pwDiagBad = 0; static LONG64 g_pwDiagNext = 0;
static void pwDiag(const TwPic& p, float px) {
    TwW& w = g_tw; PwW& q = g_pw;
    int idx = -1; for (LONG i = 0; i < w.nOff; ++i) if (q.scnTex[i] == p.scn) idx = (int)i;
    DWORD bpp = rtFmtBytes(w.fmt);
    if (idx < 0 || bpp < 3) return;
    INT pa = 0, pb = 0; const BYTE* a = NULL; const BYTE* b = NULL;
    bool ra = twReadBack(p.surf, 0, &pa, &a), rb = ra && twReadBack(q.scn[idx], 1, &pb, &b);
    if (ra && rb) {
        DWORD n = 0, d = 0;
        for (DWORD y = 0; y < w.h; y += 2) { const BYTE* x0 = a + (INT)y * pa; const BYTE* x1 = b + (INT)y * pb;
            for (DWORD x = 0; x < w.w; x += 2) { ++n; if (x0[x * bpp] != x1[x * bpp] || x0[x * bpp + 1] != x1[x * bpp + 1] || x0[x * bpp + 2] != x1[x * bpp + 2]) ++d; } }
        DWORD pct = n ? d * 100 / n : 0;
        if (pct <= 60) {
            logf("panpictures: looked at a moved picture (by %d.%d px): %u %% of the screen was drawn after the 3D pass (the interface, which stays where it is), the rest is the scene, which moved.%s", (int)px, (int)(px * 10.0f) % 10, pct,
                 g_pwDiagBad ? " The earlier looks fell into something drawn over the whole screen (a fade); nothing was wrong." : "");
            g_pwDiagLeft = 0;
        } else {
            g_pwDiagBad++;
            if (g_pwDiagLeft > 0)
                logf("panpictures: looked at a moved picture (by %d.%d px): %u %% of the screen differs from the scene as it was where the 3D pass ended - something is drawn over the whole screen right now (a fade, when a match opens). "
                     "Looking again in two seconds before deciding anything (%d more looks).", (int)px, (int)(px * 10.0f) % 10, pct, (int)g_pwDiagLeft);
            else { InterlockedExchange(&g_pwBroken, 1);
                logf("panpictures: looked at a moved picture (by %d.%d px): %u %% of the screen differs from the scene as it was where the 3D pass ended, as in all %d looks - the scene is not in the frame yet at that point, so there is nothing to move. "
                     "Pan pictures stand down for this session; pictures are shown as they are drawn and the camera steps as before.", (int)px, (int)(px * 10.0f) % 10, pct, (int)g_pwDiagBad); }
        }
    } else logf("panpictures: a moved picture could not be read back to look at.");
    if (ra) twSurfCall1(w.chkSys[0], 14);
    if (rb) twSurfCall1(w.chkSys[1], 14);
    twRel(&w.chkSys[0]); twRel(&w.chkSys[1]);                        // two screen-sized surfaces that are not needed again
}
// test (bit 2): the camera the next pictures are shown from, and the back buffer as it was after the rectangle
static float g_pwForceV[16], g_pwForceP[16], g_pwForceGh = 0; static volatile LONG g_pwForceSet = 0, g_pwGrabSeq = 0;
static BYTE* g_pwGrabBuf = NULL; static DWORD g_pwGrabBytes = 0;
static void pwGrabBackBuffer() {
    TwW& w = g_tw;
    INT pitch = 0; const BYTE* bits = NULL;
    DWORD bpp = rtFmtBytes(w.fmt), bytes = w.w * w.h * bpp;
    if (!bpp || !twReadBack(w.bb, 1, &pitch, &bits)) return;
    if (g_pwGrabBytes < bytes) { if (g_pwGrabBuf) VirtualFree(g_pwGrabBuf, 0, MEM_RELEASE); g_pwGrabBuf = (BYTE*)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); g_pwGrabBytes = g_pwGrabBuf ? bytes : 0; }
    if (g_pwGrabBuf) { for (DWORD y = 0; y < w.h; ++y) memcpy(g_pwGrabBuf + y * w.w * bpp, bits + (INT)y * pitch, w.w * bpp); InterlockedIncrement(&g_pwGrabSeq); }
    twSurfCall1(w.chkSys[1], 14);
}
// Diagnostic (with InBetweenDump): a picture as it was drawn, and what went into the back buffer when it was shown
// from where the camera had got to.
static volatile LONG g_pwDumpN = 0;
static void pwDump(const TwPic& p, float px) {
    TwW& w = g_tw;
    DWORD bpp = rtFmtBytes(w.fmt), step = w.w > 2000 ? 2 : 1;
    if (bpp < 3) { InterlockedExchange(&g_pwDumpLeft, 0); return; }
    INT pa = 0, pb = 0; const BYTE* a = NULL; const BYTE* b = NULL; char name[64];
    if (twReadBack(p.surf, 0, &pa, &a)) { wsprintfA(name, "pan_dump_%02d_a_drawn.bmp", (int)g_pwDumpN); twWriteBmp(name, a, pa, w.w, w.h, bpp, step);
        if (step > 1 || g_twDumpAny) { wsprintfA(name, "pan_dump_%02d_a_drawn_middle.bmp", (int)g_pwDumpN); twWriteBmpMiddle(name, a, pa, w.w, w.h, bpp); }
        twSurfCall1(w.chkSys[0], 14); }
    if (twReadBack(w.bb, 1, &pb, &b)) { wsprintfA(name, "pan_dump_%02d_b_shown.bmp", (int)g_pwDumpN); twWriteBmp(name, b, pb, w.w, w.h, bpp, step);
        if (step > 1 || g_twDumpAny) { wsprintfA(name, "pan_dump_%02d_b_shown_middle.bmp", (int)g_pwDumpN); twWriteBmpMiddle(name, b, pb, w.w, w.h, bpp); }
        twSurfCall1(w.chkSys[1], 14); }
    logf("panpictures: pictures %02d written (a picture as the game drew it, and as it went up - shown from %d.%d px further along the scroll).", (int)g_pwDumpN, (int)px, (int)(px * 10.0f) % 10);
    g_pwDumpN++; InterlockedDecrement(&g_pwDumpLeft);
}
// The moment of the scroll the screen should show at time `now`: now, less the usual time from a frame's mark to its
// picture, less the usual time from a frame's scroll step to its mark. (Until v71 the second was each frame's own, so
// that a picture that came exactly on time was shown from its own camera, untouched; but it differs by a millisecond
// or two from frame to frame, and so did the moment shown: a pixel or two of unevenness per picture at scrolling
// speed. One delay for all pictures is an even scroll; a picture is moved by a fraction of a pixel for it.)
static __forceinline DWORD pwTau(const TwPic& p, LONG64 now) { return (DWORD)now - (DWORD)g_pw.lagMark - (DWORD)(g_pw.ctOff ? g_pw.ctOff : (LONG)(p.mt - p.ct)); }

// ---------------------------------------------------------------- one picture for each refresh of the screen (v72)
// v66 put pictures on the screen's beat only when the game's own came slower than the screen refreshes. But at 48 to
// 60 game frames with a picture in between each, 96 to 115 pictures a second were handed to a 60 Hz screen (the v69
// session): every refresh showed parts of two, joined at a line that wandered over the whole screen, and when the
// game thread stalled for 50 ms nothing filled in. What the eye follows in a scroll is the whole screen moving, and
// for that only one thing counts: every refresh shows the scene from where the camera is at that refresh.
// So, whenever the camera scrolls, at any frame rate:
//   * ONE PICTURE FOR EACH REFRESH. A fixed time (the lead) ahead of each refresh, the newest of the game's pictures
//     that is due by then goes up, shown from where the camera is at that refresh; older ones still waiting are
//     passed over (they would have been on screen for less than a refresh). If none is due, the one on screen goes
//     up again from further along. The moment of each refresh comes from the screen itself (aotr_screen.inc); if
//     that cannot be read, from a beat of our own one refresh interval long, as in v66.
//   * ONE DELAY. The camera's moment on screen runs a steady time behind the refresh - about a game frame and the
//     time a frame takes to get here - so that the camera's place at that moment lies between two frames the game
//     has already drawn: it is read off the camera's line, not guessed ahead (guessed ahead, the screen overshoots
//     when the scroll stops and jumps back). Only when the game stalls is it guessed ahead, a quarter of a second
//     at most - a stall of the game does not stall the scroll.
//   * THE END IS PLAYED OUT. When the scroll stops, the screen is that delay behind: it goes on for those two or
//     three refreshes until it shows the camera where it stopped - with the last scrolled picture, since pictures
//     from after the stop cannot be shown from an earlier place; they are kept back until then.
// Tearing: with vsync off (DXVK immediate mode) a picture goes up the moment the GPU has it, wherever the screen's
// beam is. Handed over ~2 ms ahead of the refresh, that is at the very bottom of the screen (where part of the
// width is the game's own interface, which does not move) or at its very top, instead of anywhere. (No tear at all needs the mailbox mode - [Engine] TearFree.) [Engine] PanRefreshBeat = 1 (default; 2 =
// only below the screen's rate, as v66; 0 = off), PanLeadMs, AOTR_PANGRID / AOTR_PANLEAD.
static void pwSlot(LONG64 now, LONG64* at, LONG64* vb) {
    PwW& q = g_pw; TwW& w = g_tw;
    LONG64 v = 0, per = 0;
    if (scrNext(now - q.refresh / 2, &v, &per)) {                   // the screen's own beat. A refresh that has begun less than half an interval ago and has no picture yet still
        if (v - q.slotVb < per / 2) v += per;                       // gets one (late - most of the screen shows it in time - is better than not at all); one that has: the one after
        *vb = v; *at = v - pwLead();
    } else {                                                        // not known: one refresh interval after the picture before
        v = w.lastShow + q.refresh - q.refresh / 16;
        if (q.slotVb && v - q.slotVb < q.refresh / 2) v = q.slotVb + q.refresh - q.refresh / 16;
        *vb = v; *at = v;
    }
    g_pwSlotDue = *at;
}
static __forceinline bool pwSlotByClock() { LONG64 v, p; return scrNext(qpcNow(), &v, &p); }
// The moment of the scroll the screen shows at refresh vb (once per refresh that is served): the refresh, less the delay.
// The delay is what it takes for the camera's place at that moment to be known when the picture is handed over: the
// usual time from a frame's scroll step to the render thread having read its camera, plus one frame as long as the
// longest of the last twelve (until the next frame arrives, this one is the newest), plus the lead, plus an eighth of
// a refresh. It is followed slowly (a
// sixteenth of the difference per refresh): a stall of the game does not change it - the usual frame time leaves
// stalls out (aotr_pace.inc) - and the screen is then shown from further along the camera's line instead.
static DWORD pwGridTau(LONG64 vb) {
    PwW& q = g_pw;
    LONG64 per = 0;                                                 // the longest of the last twelve frames (stalls are not among them): half the frames are longer than the usual one,
    { const TwPace& pc = g_tw.pace; for (LONG i = 0; i < pc.ndt && i < 12; ++i) if (pc.dts[i] > per) per = pc.dts[i];      // and after one of those the screen would run past the newest frame
      if (!per) per = pc.period > 0 ? pc.period : g_pqpf.QuadPart / 30; }
    LONG want = (LONG)(q.walkLag + per + pwLead() + q.refresh / 8);
    if (want > (LONG)(g_pqpf.QuadPart / 5)) want = (LONG)(g_pqpf.QuadPart / 5);
    if (!q.lagGrid) q.lagGrid = want; else q.lagGrid += (want - q.lagGrid) / 16;
    if (!(q.pathN > 0 && q.path[q.pathN - 1].moving)) g_pwNTail++;
    if (pwSlotByClock()) g_pwNSlotClock++; else g_pwNSlotOwn++;
    DWORD tau = (DWORD)vb - (DWORD)q.lagGrid;
    q.tauLast = tau;
    return tau;
}
// How even is the scroll on screen? At every refresh served while the camera scrolls, the camera's place the screen
// shows (the view matrix's translation - for a scroll it moves in a straight line with the camera) is compared with
// the refresh before: the speed between the two, against the average speed of the eight before. Counted once a
// scroll is twelve refreshes old (it gathers speed over the first few) and not while its end is played out.
// (g_pwEvenN / g_pwEvenOff5 / g_pwEvenOff20 / g_pwEvenWorstPm - refreshes measured; more than 5 % / 20 % off; the worst, per mille - are declared in aotr_tween.inc)
static void pwEvenNote(const float* V, LONG64 vb, bool moving) {
    static float lx, ly, lz, sp[8]; static LONG64 lvb = 0; static LONG run = 0, nsp = 0;
    PwW& q = g_pw;
    if (!moving || !lvb || vb - lvb > q.refresh * 3 / 2 || vb - lvb < q.refresh / 2) { if (!moving || !lvb || vb - lvb > q.refresh * 3) { run = 0; nsp = 0; } }
    else {
        float dx = V[12] - lx, dy = V[13] - ly, dz = V[14] - lz;
        float v = (float)sqrt(dx * dx + dy * dy + dz * dz) / (float)((double)(vb - lvb) / (double)g_pqpf.QuadPart);
        ++run;
        if (run > 12 && nsp >= 8) {
            float m = 0; for (int i = 0; i < 8; ++i) m += sp[i]; m *= 0.125f;
            if (m > 1.0f) { float off = (v - m) / m; if (off < 0) off = -off; LONG pm = (LONG)(off * 1000.0f);
                g_pwEvenN++; if (pm > 50) g_pwEvenOff5++; if (pm > 200) g_pwEvenOff20++; if (pm > g_pwEvenWorstPm) g_pwEvenWorstPm = pm; }
        }
        sp[nsp & 7] = v; ++nsp;
    }
    lx = V[12]; ly = V[13]; lz = V[14]; lvb = vb;
}
// Render thread, a picture's turn has come: true when it has been drawn into the back buffer from where the camera is
// now; false: the caller copies it as it is.
// grid: on the screen's beat - `now` is the refresh the picture is handed over for.
static bool pwTryShow(const TwPic& p, LONG64 now, bool grid) {
    PwW& q = g_pw;
    if (!g_pwOn || !q.ready || g_pwBroken) return false;
    if (g_pwTest & 1) {                                             // test: through the rectangle with the camera it has - the same picture must come out
        if (!p.scn) return false;
        g_pwNTestDraw++;
        return pwDraw(p, kPwIdent, true);
    }
    if (g_pwTest & 2) {                                             // test: from the camera the test names
        if (!g_pwForceSet || !p.scn || !p.camOk) return false;
        TwPic t = p; t.gh = g_pwForceGh;
        float H[16], px = 0;
        if (!pwMapping(g_pwForceV, g_pwForceP, t, H, &px) || !pwDraw(t, H, g_pwSharp != 0)) return false;       // (the test shows it the way the game would)
        pwGrabBackBuffer();
        return true;
    }
    DWORD tau = grid ? pwGridTau(now) : 0;                          // (on the screen's beat the delay is kept for every refresh served, whatever becomes of this picture)
    if (!p.camOk || !p.ct) return false;
    if (!p.scn) { g_pwNNoScene++; return false; }
    if (!grid) { if (!q.lagMark) return false;                      // not known yet how long a picture takes to get here
                 tau = pwTau(p, now); }
    float V[16], P[16], H[16], px = 0; LONG ahead = 0;
    if (!pwWanted(tau, V, P, &ahead)) return false;
    if (grid) pwEvenNote(V, now, q.path[q.pathN - 1].moving != 0);
    if (!pwMapping(V, P, p, H, &px)) return false;
    if (px < (g_pwSharp ? 0.5f : g_pwDeadPx)) { g_pwNAsIs++; return false; }
    if (px > (float)g_tw.w * 0.2f) { g_pwNTooFar++; return false; }
    if (!pwDraw(p, H, g_pwSharp != 0)) return false;
    g_pwNShift++; { LONG p16 = (LONG)(px * 16.0f); g_pwPxSum16 += p16; if (p16 > g_pwMaxPx16) g_pwMaxPx16 = p16; }
    if (g_pwDiagLeft > 0 && px > 2.0f && !g_pwTest) { LONG64 now_ = qpcNow(); if (now_ >= g_pwDiagNext) { g_pwDiagNext = now_ + g_pqpf.QuadPart * 2; g_pwDiagLeft--; pwDiag(p, px); } }
    if (g_pwDumpLeft > 0 && px > 4.0f && !(g_pwTest & 3) && (g_tw.lastTags >= 300 || g_twDumpAny)) { static LONG64 nextAt = 0; LONG64 now_ = qpcNow(); if (now_ >= nextAt) { nextAt = now_ + g_pqpf.QuadPart * 3; pwDump(p, px); } }
    return true;
}
// Render thread: a picture has gone up at t.
static void pwShown(const TwPic& p, LONG64 t) {
    PwW& q = g_pw;
    if (!g_pwOn) { q.lastOk = 0; g_pwLastSurf = 0; return; }
    if (!p.ib && p.mt) {                                            // a game frame's own picture: how long from its mark to here (a stall is not the usual)
        LONG lag = (LONG)((DWORD)t - p.mt);
        if (lag > 0 && lag < (LONG)(g_pqpf.QuadPart / 2)) {
            if (!q.lagMark) q.lagMark = lag;
            else if (lag * 2 > q.lagMark * 3 && ++q.lagOdd < 4) { }
            else { q.lagOdd = 0; q.lagMark += (lag - q.lagMark) / 8; }
        }
    }
    if (!p.ib && p.ct && p.mt) { LONG o = (LONG)(p.mt - p.ct); if (o > 0 && o < (LONG)(g_pqpf.QuadPart / 4)) q.ctOff = q.ctOff ? q.ctOff + (o - q.ctOff) / 8 : o; }
    q.last = p;
    q.lastOk = (p.camOk && p.ct && p.scn) ? 1 : 0;
    g_pwLastSurf = q.lastOk ? p.surf : 0;                           // its target is not handed out while it may be shown again
}
// The screen's refresh interval while pictures go up on the screen's beat (see above); 0 otherwise.
static LONG64 pwGrid() {
    PwW& q = g_pw;
    if (!g_pwGridOn || !pwScrolling() || !q.ready || !q.refresh || g_pwBroken || (g_pwTest & 3)) return 0;
    if (g_pwGridOn == 2) {                                          // as v66: only while the game's pictures come slower than the screen refreshes
        LONG64 beat = q.last.beat > 0 ? q.last.beat : g_tw.pace.period / 2;
        if (!q.lagMark || !q.lastOk || beat <= q.refresh + q.refresh / 16) return 0;
    }
    return q.refresh;
}
// Render thread, between records and while idle: the beat has come, no picture is there, the camera is scrolling.
static void pwFillerTick() {
    TwW& w = g_tw; PwW& q = g_pw;
    if (!g_pwOn || !q.ready || (g_pwTest & 3) || g_pwBroken) return;
    LONG64 now = qpcNow(), grid = pwGrid();
    if (!grid && (!q.lastOk || !q.lagMark)) return;
    LONG64 beat = q.last.beat > 0 ? q.last.beat : w.pace.period / 2;
    if (beat < g_pqpf.QuadPart / 250) beat = g_pqpf.QuadPart / 250;
    float V[16], P[16], H[16], px = 0; LONG ahead = 0;
    if (grid) {                                                     // on the screen's beat: a refresh is near and none of the game's pictures is due for it
        LONG64 at, vb; pwSlot(now, &at, &vb);
        if (now < at) return;
        if (w.qn > 0 && w.q[0].due <= vb) return;                   // (one is: twShowDue puts it up, or keeps it back)
        q.slotVb = vb; g_pwHandAt = at;                             // this refresh is settled here, whether or not anything has to go up for it
        DWORD tau = pwGridTau(vb);
        if (!q.lastOk || !pwWanted(tau, V, P, &ahead)) return;      // (nothing on screen that could be shown from elsewhere)
        pwEvenNote(V, vb, q.path[q.pathN - 1].moving != 0);
    } else {
        if (now - w.lastShow < beat + beat / 4) return;             // a picture that is a millisecond late is not a missing picture
        if (w.qn > 0 && w.q[0].due - now < beat / 2) return;        // the next picture is about to go up
        if (!pwWanted(pwTau(q.last, now), V, P, &ahead) || ahead <= 0) return;      // only past what the game has drawn: everything else has its own picture coming
    }
    if (ahead >= (LONG)(g_pqpf.QuadPart / 4)) {                     // as far ahead as it goes: nothing more to look out for until a picture comes
        if (now - w.lastShow > g_pqpf.QuadPart) { q.lastOk = 0; g_pwLastSurf = 0; }
        return;
    }
    if (!pwMapping(V, P, q.last, H, &px) || px > (float)w.w * 0.2f) return;
    bool asIs = px < (g_pwSharp ? 0.5f : g_pwDeadPx);               // the camera is where the picture was drawn from:
    if (asIs && !(grid && q.shifted)) return;                       // nothing to do - unless what is on screen is that picture from elsewhere: then it goes up as it is
    bool scene = w.inScene != 0;
    if (scene) ((TwF1)g_twO_EndScene)(w.resDev);
    bool ok = true;
    if (asIs) twCopyToBackBuffer(q.last.surf); else ok = pwDraw(q.last, H, g_pwSharp != 0);
    if (ok) {
        q.shifted = asIs ? 0 : 1;
        if (grid) pwHandNote();
        DWORD hr = twRealPresent(q.last.args);
        LONG64 t = qpcNow();
        if (grid) { g_pwNGrid++; pwGridGapNote(t - w.lastShow); }
        w.lastShow = t; w.lastHr = hr;
        g_pwNFill++; { LONG ms = (LONG)((LONG64)ahead * 1000 / g_pqpf.QuadPart); if (ms > g_pwFillMaxMs) g_pwFillMaxMs = ms; }
        InterlockedExchange(&g_rtPresentHR, (LONG)hr);
    }
    if (scene) ((TwF1)g_twO_BeginScene)(w.resDev);
}
// Render thread, at a frame's Present after its stretch was walked (or could not be): what its two pictures show the
// world from. walked: this frame's camera was read; did: the in-between picture I exists.
static void pwAfterWalk(bool walked, bool did, DWORD I, DWORD S, LONG64 now) {
    TwW& w = g_tw; PwW& q = g_pw; TwWalk& W = g_twWalk;
    g_pwPend[0].ok = g_pwPend[1].ok = 0;
    if (!g_pwOn) { q.pathN = 0; q.frameCt = 0; return; }
    bool camOk = walked && W.endOk;
    DWORD ct = camOk ? q.frameCt : 0;
    pwPathPush(camOk, W.endV, W.endP, ct, now, w.pace.period);
    int si = pwIndex(S), ii = (did && I) ? pwIndex(I) : -1;
    if ((g_pwTest & 1) && pwEnsure()) {                             // test: a frame without the end-of-3D mark has no interface - the scene is the picture
        if (si >= 0 && !q.scnValid[si] && (LONG)((TwF6)g_twO_Stretch)(q.dev, S, 0, q.scn[si], 0, 0) >= 0) q.scnValid[si] = 1;
        if (ii >= 0 && !q.scnValid[ii] && (LONG)((TwF6)g_twO_Stretch)(q.dev, I, 0, q.scn[ii], 0, 0) >= 0) q.scnValid[ii] = 1;
    }
    float gh = W.nZ > 0 ? W.sumZ / (float)W.nZ : g_twGroundZ;
    if (q.frameCt) {                                                // a scrolled frame: could its picture be shown from another camera? (five in a row that could not: the scroll hook goes back to its usual step)
        bool good = camOk && si >= 0 && q.scnValid[si] && q.ready;
        if (good) g_pwBadRun = 0;
        else if (g_pwBadRun < 1000 && ++g_pwBadRun == 5 && !g_pwTest)
            logf("panpictures: five scrolled frames in a row could not be shown from another camera (%s) - the camera steps by the usual frame time until one can.", !camOk ? "the frame's camera was not found where the 3D pass ends" : "no copy of the scene was made");
    }
    for (int k = 0; k < 2; ++k) {
        int idx = k ? ii : si;
        PwCam& c = g_pwPend[k];
        c.ok = (camOk && (ct || (g_pwTest & 2))) ? 1 : 0; c.ct = ct; c.mt = q.frameMt; c.gh = gh;
        c.scn = (idx >= 0 && q.scnValid[idx] && q.ready) ? q.scnTex[idx] : 0;
        if (camOk) { memcpy(c.V, k ? W.endTwV : W.endV, 64); memcpy(c.P, k ? W.endTwP : W.endP, 64); }
        if (idx >= 0) q.scnValid[idx] = 0;
    }
    q.frameCt = 0;
}
// Test: the back buffer after the rectangle against the picture itself (its off-screen target) - every pixel.
static volatile LONG g_pwNTestSame = 0, g_pwNTestDiff = 0, g_pwNTestFail = 0;
static DWORD g_pwTestFirst[3];
static void pwTestCompare(const TwPic& p) {
    TwW& w = g_tw;
    INT pa = 0, pb = 0; const BYTE* a = NULL; const BYTE* b = NULL;
    DWORD bpp = rtFmtBytes(w.fmt);
    bool ra = bpp && twReadBack(p.surf, 0, &pa, &a), rb = ra && twReadBack(w.bb, 1, &pb, &b);
    if (ra && rb) {
        DWORD diff = 0, fx = 0, fy = 0;
        for (DWORD y = 0; y < w.h; ++y) {
            const BYTE* x0 = a + (INT)y * pa; const BYTE* x1 = b + (INT)y * pb;
            if (!memcmp(x0, x1, w.w * bpp)) continue;
            for (DWORD x = 0; x < w.w; ++x) if (memcmp(x0 + x * bpp, x1 + x * bpp, bpp == 4 ? 3 : bpp)) { if (!diff) { fx = x; fy = y; } ++diff; }
        }
        if (diff) { if (InterlockedIncrement(&g_pwNTestDiff) == 1) { g_pwTestFirst[0] = fx; g_pwTestFirst[1] = fy; g_pwTestFirst[2] = diff; } }
        else InterlockedIncrement(&g_pwNTestSame);
    } else InterlockedIncrement(&g_pwNTestFail);
    if (ra) twSurfCall1(w.chkSys[0], 14);
    if (rb) twSurfCall1(w.chkSys[1], 14);
}
static void pwToggle() {                                            // key thread
    if (!g_twO_SetRT) return;
    LONG was = InterlockedExchange(&g_pwOn, g_pwOn ? 0 : 1);
    logf("panpictures: Ctrl+F8 - %s.", was ? "OFF: every picture is shown as it was drawn, the camera steps by the usual frame time" : "ON: while the camera scrolls, pictures are shown from where the camera is at that moment");
}

// ---------------------------------------------------------------- offline test of the path arithmetic (hooks_test)
extern "C" __declspec(dllexport) int __cdecl AotrPanWarpPathTest(char* out, int nOut) {
    if (!g_pqpf.QuadPart) QueryPerformanceFrequency(&g_pqpf);
    PwW keep = g_pw; int fail = 0;
    const LONG64 f = g_pqpf.QuadPart; const DWORD ms = (DWORD)(f / 1000);
    float P[16] = { 1.35f,0,0,0, 0,1.8f,0,0, 0,0,1.0f,1, 0,0,-5.0f,0 };
    struct L { static void view(float* V, float x) { float t[16] = { 1,0,0,0, 0,0.7071f,0.7071f,0, 0,0.7071f,-0.7071f,0, -x,0,700.0f,1 }; memcpy(V, t, 64); } };
    g_pw.pathN = 0;
    // a standing camera for two frames, then a scroll at 100 units a second with frames 30, 45, 33 and 60 ms apart
    static const int kGap[4] = { 30, 45, 33, 60 };
    DWORD t0 = 0x10000000u, t = t0; float x = 0, V[16];
    L::view(V, 0); pwPathPush(true, V, P, 0, (LONG64)t, f / 30);                      // still (no scroll step)
    L::view(V, 0); pwPathPush(true, V, P, 0, (LONG64)t, f / 30);
    DWORD tFirst = 0; DWORD tk[5]; float xk[5]; int nk = 0;
    for (int i = 0; i < 4; ++i) {
        t += (DWORD)kGap[i] * ms;
        x += (i == 0 ? (1000.0f / 30.0f) : (float)kGap[i]) * 0.1f;                      // the first step is the usual frame time long; the others as long as the time that passed
        L::view(V, x); pwPathPush(true, V, P, t, (LONG64)t, f / 30);
        if (!tFirst) tFirst = t;
        tk[nk] = t; xk[nk] = x; ++nk;
    }
    float Vw[16], Pw[16]; LONG ahead = 0;
    // at each frame's own moment: that frame's camera
    for (int i = 0; i < nk; ++i) { if (!pwWanted(tk[i], Vw, Pw, &ahead) || fabs(-Vw[12] - xk[i]) > 0.01f) fail |= 1; }
    // between two frames: on the line
    { DWORD mid = tk[1] + (tk[2] - tk[1]) / 3; float want = xk[1] + (xk[2] - xk[1]) / 3.0f; if (!pwWanted(mid, Vw, Pw, &ahead) || fabs(-Vw[12] - want) > 0.02f || ahead) fail |= 2; }
    // past the newest frame: further along at the same speed (100 units a second), a quarter of a second at most
    { if (!pwWanted(tk[3] + 40 * ms, Vw, Pw, &ahead) || fabs(-Vw[12] - (xk[3] + 4.0f)) > 0.05f || ahead <= 0) fail |= 4;
      if (!pwWanted(tk[3] + 900 * ms, Vw, Pw, &ahead) || fabs(-Vw[12] - (xk[3] + 25.0f)) > 0.2f) fail |= 4; }
    // before the scroll began: where the camera stood; the scroll's first step leads up to its moment over one usual frame time
    { if (!pwWanted(tFirst - 200 * ms, Vw, Pw, &ahead) || fabs(Vw[12]) > 0.01f) fail |= 8;
      if (!pwWanted(tFirst - (DWORD)(f / 60), Vw, Pw, &ahead) || fabs(-Vw[12] - xk[0] * 0.5f) > 0.05f) fail |= 8; }
    // the scroll stops: a frame without a step keeps the camera, and nothing runs on past it
    { L::view(V, x); pwPathPush(true, V, P, 0, (LONG64)t, f / 30);
      if (!pwWanted(tk[3] + 500 * ms, Vw, Pw, &ahead) || fabs(-Vw[12] - xk[3]) > 0.01f || ahead) fail |= 16; }
    // a cut (the camera somewhere else altogether): one side or the other, never in between
    { g_pw.pathN = 0; L::view(V, 0); pwPathPush(true, V, P, t0, (LONG64)t0, f / 30); L::view(V, 5000.0f); pwPathPush(true, V, P, t0 + 33 * ms, (LONG64)t0, f / 30);
      if (!pwWanted(t0 + 10 * ms, Vw, Pw, &ahead) || fabs(Vw[12]) > 0.01f) fail |= 32;
      if (!pwWanted(t0 + 30 * ms, Vw, Pw, &ahead) || fabs(-Vw[12] - 5000.0f) > 0.01f) fail |= 32;
      if (!pwWanted(t0 + 100 * ms, Vw, Pw, &ahead) || fabs(-Vw[12] - 5000.0f) > 0.01f || ahead) fail |= 32; }
    // a frame whose camera could not be read forgets everything
    { pwPathPush(false, V, P, 0, 0, f / 30); if (pwWanted(t0, Vw, Pw, &ahead)) fail |= 64; }
    // the mapping between two cameras a step apart moves the middle of the screen by what the step is on screen, and a camera onto itself by nothing
    { TwPic p; memset(&p, 0, sizeof(p)); L::view(p.V, 0); memcpy(p.P, P, 64); p.gh = 0;
      DWORD kw = g_tw.w, kh = g_tw.h; g_tw.w = 640; g_tw.h = 480;
      float H[16], px = -1;
      if (!pwMapping(p.V, P, p, H, &px) || px > 0.001f) fail |= 128;
      L::view(V, 10.0f);
      if (!pwMapping(V, P, p, H, &px) || px < 2.0f || px > 40.0f) fail |= 128;
      g_tw.w = kw; g_tw.h = kh; }
    g_pw = keep;
    if (out && nOut > 300) sprintf(out, "pan pictures, where the camera is at a moment: %s (flags %02X) - at each frame's moment its camera, between two frames on the line, past the newest frame further along at the same speed (a quarter of a second at most), "
                                "before a scroll where the camera stood, after it where it stopped, across a cut one side or the other; the mapping of a camera onto itself moves nothing",
                                fail ? "FAILED" : "as it must be", fail);
    return fail;
}
