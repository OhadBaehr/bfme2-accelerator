// ================================================================ when do a frame's pictures go on screen? (v53, 2026-10-07)
// The game's frames do not come evenly. A frame with a logic step in it is longer than one without, a frame that
// builds new terrain while the camera moves costs a few milliseconds more, the render thread waits now and then
// for a texture. But what the eye follows moves by the SAME step every frame: the engine scrolls the camera by a
// fixed distance per rendered frame (0x006A228C: speed factors times 250, no time in it; measured 25.0 units a
// frame at 23 and at 29 frames a second alike), and a marching unit advances one logic frame per frame. So if the
// pictures are shown the moment they are finished, every uneven frame is a jerk in a pan - which is what v50-v52
// did: the game's frame half a period after it was finished, its in-between frame as soon as it existed.
//
// Here the pictures are put on a beat instead: half the usual frame period apart, in-between frame, game frame,
// in-between frame, game frame. A frame whose pictures are finished before their beat waits for it; that waiting
// time is what a later frame may come late by without anything showing. Three things keep it right:
//   * The beat is the mean of the last twelve frame durations (not the median, not a trimmed mean: a frame that is
//     longer every sixth time, or every second, must count in full, or the beat runs ahead of the frames). After a
//     frame that came late the beat is therefore a little slower than the frames for half a second, and exactly
//     the time that frame was late by is in hand again.
//   * An in-between frame finished after its beat goes on screen at once, and the beat goes on from there.
//   * The smallest time in hand of the last four seconds is steered towards a millisecond or two, by moving the
//     beat a twentieth of itself per frame at most - nothing the eye can tell. So no more is kept waiting than the
//     worst frame of the last four seconds needed, and when the frames come evenly the pictures wait for nothing.
// No more than half a frame period, and no more than 20 ms, is meant to be in hand; when more has built up it is
// given back a twentieth of a beat per frame, and at a quarter above that limit all at once.
//
// Pure arithmetic on tick counts: no device, no globals. src/pace_test.cpp runs it over frame patterns.

struct TwPace {
    LONG64 f;                                // ticks per second
    LONG64 lastIn, period, dtk;              // when the last frame arrived; the usual frame duration; this frame's own
    LONG64 dts[12]; LONG ndt, idt, nOdd;
    LONG64 lastDue;                          // when the newest picture placed is due
    LONG64 smin[8]; LONG64 sbT; LONG sb;     // the least time in hand within each of the last half seconds, as it would be on the beat as it is now
    // about the last frame placed
    LONG late, held, pulled; LONG64 miss, inHand;
};
#define TWP_NONE ((LONG64)1 << 62)
static void twpSlackReset(TwPace& P) { for (int i = 0; i < 8; ++i) P.smin[i] = TWP_NONE; P.sb = 0; P.sbT = 0; }
static void twpReset(TwPace& P, LONG64 f) { memset(&P, 0, sizeof(P)); P.f = f > 0 ? f : 1; twpSlackReset(P); }
// A frame is complete (its Present has been reached).
static void twpArrive(TwPace& P, LONG64 now) {
    P.dtk = 0;
    if (P.lastIn) {
        LONG64 dt = now - P.lastIn;
        bool sane = dt > P.f / 500 && dt < P.f / 4;                                   // not a pause, not a glitch of the clock
        bool odd = sane && P.period && P.ndt >= 4 && dt * 2 >= P.period * 5;          // two and a half times the usual: a stall, not the frame rate ...
        if (odd && ++P.nOdd >= 3) { P.ndt = 0; P.idt = 0; odd = false; }              // ... unless three in a row are like that
        if (sane && !odd) {
            P.nOdd = 0;
            P.dts[P.idt] = dt; P.idt = (P.idt + 1) % 12; if (P.ndt < 12) P.ndt++;
            LONG64 sum = 0; for (LONG i = 0; i < P.ndt; ++i) sum += P.dts[i];
            P.period = sum / P.ndt;
        }
        if (sane) P.dtk = dt;
    }
    P.lastIn = now;
}
// Both pictures of the frame exist at a (only the game's frame when !ib). x: when the in-between frame goes on
// screen, y: when the game's frame does, beat: the distance between two pictures as it is now.
static void twpPlace(TwPace& P, LONG64 a, bool ib, LONG64* px, LONG64* py, LONG64* pbeat) {
    LONG64 f = P.f, beat = P.period / 2, x = a, y = a;
    P.late = 0; P.held = 0; P.pulled = 0; P.miss = 0; P.inHand = 0;
    if (!beat || !P.lastDue || a - P.lastDue > f / 2 || P.lastDue - a > f / 2) {      // the first frame, or the first after a pause: the beat starts here
        twpSlackReset(P);
        y = ib ? a + beat : a;
    } else {
        LONG64 cap = beat < f / 50 ? beat : f / 50;        // never more than half a frame, or 20 ms, in hand
        if (ib) {
            LONG64 ideal = P.lastDue + beat, slack = ideal - a;
            if (!P.sbT || a - P.sbT >= f / 2) {            // a new half second
                if (P.sbT && a - P.sbT >= f * 4) twpSlackReset(P);
                P.sb = (P.sb + 1) & 7; P.smin[P.sb] = TWP_NONE; P.sbT = a;
            }
            if (slack < P.smin[P.sb]) P.smin[P.sb] = slack;
            LONG64 m = TWP_NONE; for (int i = 0; i < 8; ++i) if (P.smin[i] < m) m = P.smin[i];
            if (slack < 0) { P.late = 1; P.miss = -slack; x = a; }                    // finished after its beat: on screen at once, and the beat goes on from here
            else {
                LONG64 want = f / 1000 + beat / 16;
                LONG64 nudge = (want - m) / 4, up = beat / 10, dn = beat / 20;
                if (nudge > up) nudge = up; else if (nudge < -dn) nudge = -dn;
                LONG64 top = cap - cap / 8;                // the steering itself never asks for more than this: the last eighth is room for the frames to vary in,
                if (slack > cap) nudge = -dn;              // so that it is the beat, not the limit, that places the pictures. Above the limit it is given back,
                else if (nudge > 0 && slack + nudge > top) nudge = slack < top ? top - slack : 0;      // a twentieth of a beat per frame
                x = ideal + nudge;
                if (x < a) x = a;
                if (x > a + cap + cap / 4) { x = a + cap; P.pulled = 1; }     // far more in hand than is ever kept (the frames have become much faster): pulled in at once
                if (x > a) P.held = 1;
            }
            LONG64 shift = x - ideal;                      // the beat has moved by this much: so has what every earlier frame would have in hand on it
            if (shift) for (int i = 0; i < 8; ++i) if (P.smin[i] != TWP_NONE) P.smin[i] += shift;
            P.inHand = x - a;
            y = x + beat;
        } else {                                           // no in-between frame this time: the game's frame keeps its own place, two beats after the one before
            y = P.lastDue + 2 * beat;
            if (y < a) y = a;
            if (y > a + beat + cap) y = a + beat + cap;
        }
    }
    P.lastDue = y;
    *px = x; *py = y; *pbeat = beat;
}
