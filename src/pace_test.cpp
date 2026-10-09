// pace_test.cpp - the picture schedule (aotr_pace.inc) over frame patterns, against showing every picture the moment
// its place under the old rule came (v50-v52: the game's frame half a period after it was finished, its in-between
// frame half of the frame's own duration before that, the frame before forced out when the next one arrives).
// For every pattern: the gaps between consecutive pictures on screen (the camera moves the same distance between
// any two of them, so an uneven gap is a jerk), and how long the game's frame waits after it is finished.
// Fails if a picture is ever placed before it exists, or pictures leave their order.
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include "aotr_pace.inc"

static const LONG64 F = 10000000;                          // ticks per second
static LONG64 ms2t(double ms) { return (LONG64)(ms * (double)F / 1000.0); }
static double t2ms(LONG64 t) { return (double)t * 1000.0 / (double)F; }
static unsigned g_seed = 12345;
static double rnd() { g_seed = g_seed * 1103515245u + 12345u; return (double)((g_seed >> 8) & 0xFFFF) / 65536.0; }

struct Frame { double d, r; int ib; };                    // duration, second pass, has an in-between picture
struct Stat { double mean, sd, worst, least, over15, wait, waitMax; int late, forced, pulled; };
static int g_bad = 0;

static Stat gaps(const std::vector<LONG64>& shown, size_t skip) {
    Stat s; memset(&s, 0, sizeof(s)); s.least = 1e9;
    double sum = 0, sum2 = 0; int n = 0;
    for (size_t i = skip + 1; i < shown.size(); ++i) { double g = t2ms(shown[i] - shown[i - 1]); sum += g; sum2 += g * g; n++; if (g > s.worst) s.worst = g; if (g < s.least) s.least = g; }
    if (!n) return s;
    s.mean = sum / n; s.sd = sqrt(sum2 / n - s.mean * s.mean > 0 ? sum2 / n - s.mean * s.mean : 0);
    int o = 0; for (size_t i = skip + 1; i < shown.size(); ++i) if (t2ms(shown[i] - shown[i - 1]) > 1.5 * s.mean) o++;
    s.over15 = 100.0 * o / n;
    return s;
}
// the rule of v50-v52
static Stat oldRule(const std::vector<Frame>& fr, size_t skipFrames) {
    std::vector<LONG64> shown; LONG64 T = 0, period = 0, lastIn = 0, pendDue = 0, ibDue = 0; bool pend = false, pendIb = false; double wait = 0, waitMax = 0; int nw = 0, late = 0; size_t skip = 0;
    for (size_t i = 0; i < fr.size(); ++i) {
        T += ms2t(fr[i].d);
        LONG64 dt = lastIn ? T - lastIn : 0;
        if (pendIb) { shown.push_back(ibDue < T ? ibDue : T); pendIb = false; }                                            // what still waits goes first, at the latest now
        if (pend) { LONG64 at = pendDue < T ? pendDue : T; if (pendDue > T) late++; shown.push_back(at); pend = false; }
        LONG64 a = T + (fr[i].ib ? ms2t(fr[i].r) : 0);
        LONG64 dtk = (dt > 0 && dt < F / 4 && period) ? dt : period;
        if (i == skipFrames) skip = shown.size();
        if (fr[i].ib) {
            LONG64 g = a + period / 2;
            ibDue = g - dtk / 2; if (ibDue - a < F / 2000) ibDue = a;
            pendIb = true; pendDue = g;
        } else pendDue = T + ms2t(fr[i].r) + period / 2;
        pend = true;
        if (i >= skipFrames) { double w = t2ms(pendDue - a); wait += w; if (w > waitMax) waitMax = w; nw++; }
        if (dt > F / 500 && dt < F / 4) period = period ? (period * 3 + dt) / 4 : dt;
        lastIn = T;
    }
    Stat s = gaps(shown, skip); s.wait = nw ? wait / nw : 0; s.waitMax = waitMax; s.late = late; s.forced = 0;
    return s;
}
// the schedule, with the queue as the render thread keeps it: in order, never two in the same instant, the oldest shown early when no target is free
static Stat newRule(const std::vector<Frame>& fr, size_t skipFrames, double* inHandMean) {
    TwPace P; twpReset(P, F);
    struct Pic { LONG64 due, beat, made; };
    std::vector<Pic> q; std::vector<LONG64> shown; LONG64 T = 0, lastShow = 0; double wait = 0, waitMax = 0, hand = 0; int nw = 0, late = 0, forced = 0, pulled = 0; size_t skip = 0;
    auto showUntil = [&](LONG64 now) {
        while (!q.empty()) { LONG64 at = q[0].due; LONG64 soonest = lastShow + q[0].beat / 2; if (soonest > at) at = soonest; if (at > now) break;
            if (at < q[0].made) { printf("   FAIL: a picture shown %.3f ms before it existed\n", t2ms(q[0].made - at)); g_bad++; }
            shown.push_back(at); lastShow = at; q.erase(q.begin()); } };
    auto force = [&](LONG64 now, size_t keep) { while (q.size() > keep) { shown.push_back(now > lastShow ? now : lastShow); lastShow = shown.back(); q.erase(q.begin()); forced++; } };
    for (size_t i = 0; i < fr.size(); ++i) {
        T += ms2t(fr[i].d);
        showUntil(T);
        twpArrive(P, T);
        force(T, 3);                                       // five targets: this frame's, the in-between frame's, three waiting
        LONG64 a = T + (fr[i].ib ? ms2t(fr[i].r) : 0);
        showUntil(a);
        LONG64 x, y, beat; twpPlace(P, a, fr[i].ib != 0, &x, &y, &beat);
        if (x < a || y < a || (fr[i].ib && y != x + beat)) { printf("   FAIL: frame %u placed wrongly (x-a %.3f, y-a %.3f)\n", (unsigned)i, t2ms(x - a), t2ms(y - a)); g_bad++; }
        if (i == skipFrames) skip = shown.size() + q.size();
        if (fr[i].ib) { Pic p = { x, beat, a }; q.push_back(p); }
        { Pic p = { y, beat, a }; q.push_back(p); }
        if (P.late && i >= skipFrames) late++;
        if (P.pulled && i >= skipFrames) pulled++;
        if (i >= skipFrames) { double w = t2ms(y - a); wait += w; if (w > waitMax) waitMax = w; hand += t2ms(P.inHand); nw++; }
        force(a, 4);                                       // and one must be free for the next frame
    }
    showUntil((LONG64)1 << 60);
    for (size_t i = 1; i < shown.size(); ++i) if (shown[i] < shown[i - 1]) { printf("   FAIL: pictures out of order in time\n"); g_bad++; break; }
    Stat s = gaps(shown, skip); s.wait = nw ? wait / nw : 0; s.waitMax = waitMax; s.late = late; s.forced = forced; s.pulled = pulled;
    if (inHandMean) *inHandMean = nw ? hand / nw : 0;
    return s;
}
static void run(const char* name, const std::vector<Frame>& fr, size_t skip) {
    Stat o = oldRule(fr, skip); double hand = 0; Stat n = newRule(fr, skip, &hand);
    printf("%-58s\n", name);
    printf("   shown as finished (v52): gap mean %6.2f ms, deviation %5.2f, longest %6.2f, shortest %6.2f, %4.1f%% over 1.5x | game frame waits %5.1f ms (max %5.1f)\n", o.mean, o.sd, o.worst, o.least, o.over15, o.wait, o.waitMax);
    printf("   on the beat      (v53): gap mean %6.2f ms, deviation %5.2f, longest %6.2f, shortest %6.2f, %4.1f%% over 1.5x | game frame waits %5.1f ms (max %5.1f), %4.1f in hand | after their beat %d, pulled in %d, shown early for a target %d\n",
           n.mean, n.sd, n.worst, n.least, n.over15, n.wait, n.waitMax, hand, n.late, n.pulled, n.forced);
}
int main() {
    const int N = 1500; const size_t skip = 300;           // 300 frames to settle, then measured
    std::vector<Frame> v;
    v.clear(); for (int i = 0; i < N; ++i) { Frame f = { 40.0 + (rnd() - 0.5) * 2.0, 13.0 + (rnd() - 0.5), 1 }; v.push_back(f); }
    run("steady battle: 40 ms frames, +-1 ms", v, skip);
    v.clear(); for (int i = 0; i < N; ++i) { Frame f = { (i & 1) ? 20.0 : 35.0, 7.0, 1 }; v.push_back(f); }
    run("a logic frame (35 ms), a frame without (20 ms), in turn", v, skip);
    v.clear(); for (int i = 0; i < N; ++i) { Frame f = { (i % 6) == 5 ? 55.0 : 35.0, 12.0, 1 }; v.push_back(f); }
    run("every sixth frame 20 ms longer (the logic step's heavy part)", v, skip);
    v.clear(); for (int i = 0; i < N; ++i) { Frame f = { 38.0 + rnd() * 12.0, 12.0 + rnd() * 3.0, 1 }; v.push_back(f); }
    run("38 to 50 ms at random", v, skip);
    v.clear(); for (int i = 0; i < N; ++i) { Frame f = { 40.0 + ((i % 10) == 3 ? 15.0 : 0.0) + (rnd() - 0.5), 13.0, 1 }; v.push_back(f); }
    run("a pan: every tenth frame builds terrain, 15 ms longer", v, skip);
    v.clear(); for (int i = 0; i < N; ++i) { Frame f = { 40.0 + ((i % 37) == 3 ? 30.0 : 0.0) + (rnd() - 0.5), 13.0, 1 }; v.push_back(f); }
    run("a 30 ms stall every second and a half", v, skip);
    v.clear(); for (int i = 0; i < N; ++i) { Frame f = { 40.0 + (rnd() - 0.5), 13.0, (i % 9) == 4 ? 0 : 1 }; v.push_back(f); }
    run("every ninth frame without an in-between picture", v, skip);
    v.clear(); for (int i = 0; i < N; ++i) { Frame f = { i < 700 ? 30.0 : 45.0, 10.0, 1 }; v.push_back(f); }
    run("30 ms frames, then 45 ms frames", v, skip);
    v.clear(); for (int i = 0; i < N; ++i) { Frame f = { 16.7 + (rnd() - 0.5) * 3.0, 3.0, 1 }; v.push_back(f); }
    run("light scene: 16.7 ms frames, +-1.5 ms", v, skip);
    v.clear(); for (int i = 0; i < N; ++i) { double d = 40.0; if ((i % 50) == 20) d = 400.0; if ((i % 50) == 30) d = 120.0; Frame f = { d, 13.0, 1 }; v.push_back(f); }
    run("loading hitches: 400 ms and 120 ms frames now and then", v, skip);
    v.clear(); for (int i = 0; i < N; ++i) { bool logic = (rnd() < 0.6); Frame f = { (logic ? 35.0 : 20.0) + (rnd() - 0.5) * 4.0, 7.0 + rnd() * 2.0, 1 }; v.push_back(f); }
    run("logic frames and frames without, in no order, +-2 ms", v, skip);
    v.clear(); for (int i = 0; i < N; ++i) { double h = rnd() < 0.08 ? 8.0 + rnd() * 17.0 : 0.0; Frame f = { 39.0 + (rnd() - 0.5) * 3.0 + h, 13.0 + rnd() * 2.0, 1 }; v.push_back(f); }
    run("a pan in a battle: 39 ms +-1.5, one frame in twelve 8 to 25 ms longer", v, skip);
    v.clear(); for (int i = 0; i < N; ++i) { double h = rnd() < 0.08 ? 8.0 + rnd() * 17.0 : 0.0; Frame f = { 39.0 + (rnd() - 0.5) * 3.0 + h, 13.0 + rnd() * 2.0, rnd() < 0.03 ? 0 : 1 }; v.push_back(f); }
    run("the same, and one frame in thirty without an in-between picture", v, skip);
    v.clear(); { double d = 40.0; for (int i = 0; i < N; ++i) { d += (rnd() - 0.5) * 1.5; if (d < 25.0) d = 25.0; if (d > 60.0) d = 60.0; Frame f = { d, 13.0, 1 }; v.push_back(f); } }
    run("frame time drifting slowly between 25 and 60 ms", v, skip);
    printf("%s\n", g_bad ? "FAILED" : "all placements valid (never before a picture exists, never out of order)");
    return g_bad ? 1 : 0;
}
