// Fixtures for avatar position sync (#1417; indra/newview/slpossyncmath.h), run by
// scripts/check/checks/pos_sync.py (compiled with the host C++ compiler, no viewer build).
//   PARSE    exactly five fields after the sim-given owner; a body that carries a key (or any
//            sixth field) is rejected whole, so identity can only come from the sim.
//   BOUND    a shared position more than 2 m from the sim's placement is rejected and the
//            avatar eases back to sim placement; within 2 m it is followed, never snapped.
//   RELEASE  (#1526) only still lines are drawn, and only while the sim's placement stands;
//            the sim moving it or a release (S) line lets go, and the offset is handed back
//            with the sim's motion (never against it, never a pop).
//   SENDER   (#1526) nothing is sent while moving; a still line once settled; exactly one
//            release, flagged S, on the move; seated as before.
//   TABLE    the per-avatar table is capped (a newcomer waits) and entries expire.
//   SEQ      repeated / older lines are dropped, the counter wraps.
//   SHARE    the option (and RLV) gate what is sent.
#include "slpossyncmath.h"

#include <cstdio>
#include <string>

using namespace SLPosSyncMath;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static const char* GOOD = "17|<128.50000, 64.25000, 22.00000>|<1.00000, 0.00000, 0.00000>|<0.00000, 0.00000, 0.00000, 1.00000>|F";

static void testParse()
{
    Line l;
    CHECK(parseFields(GOOD, l), "a well-formed line parses");
    CHECK(l.seq == 17 && l.pos.x == 128.5 && l.vel.x == 1.0 && (l.flags & FLAG_FLY), "fields read back");
    CHECK(parseFields("1|<1,2,3>|<0,0,0>|<0,0,0,1>|", l), "no spaces, empty flags");
    // An owner key in the body, before or after the fields: rejected, never read as identity.
    CHECK(!parseFields("a2e76fcd-9360-4f6d-a924-000000000003|17|<1,2,3>|<0,0,0>|<0,0,0,1>|F", l),
          "a key in front of the fields is not an identity");
    CHECK(!parseFields(std::string(GOOD) + "|a2e76fcd-9360-4f6d-a924-000000000003", l),
          "a sixth field (a key) is rejected");
    CHECK(!parseFields("17|<1,2,3>|<0,0,0>|<0,0,0,1>", l), "four fields rejected");
    CHECK(!parseFields("70000|<1,2,3>|<0,0,0>|<0,0,0,1>|", l), "seq out of range");
    CHECK(!parseFields("-1|<1,2,3>|<0,0,0>|<0,0,0,1>|", l), "negative seq");
    CHECK(!parseFields("1|<1,2>|<0,0,0>|<0,0,0,1>|", l), "short vector");
    CHECK(!parseFields("1|<1,2,3,4>|<0,0,0>|<0,0,0,1>|", l), "long vector");
    CHECK(!parseFields("1|<nan,2,3>|<0,0,0>|<0,0,0,1>|", l), "NaN position");
    CHECK(!parseFields("1|<1,2,3>|<0,0,0>|<0,0,0,0>|", l), "zero rotation");
    CHECK(!parseFields("1|<1,2,3>|<0,0,0>|<0,0,0,1>|Q", l), "unknown flag");
    CHECK(parseFields("1|<1,2,3>|<900,0,0>|<0,0,0,1>|", l) && l.vel.length() <= MAX_SPEED + 1e-9,
          "velocity clamped to MAX_SPEED");
    // The sender's numbers never follow the user's locale (a decimal comma breaks the tuple).
    CHECK(formatFixed(128.5, 3) == "128.500" && formatFixed(-1.5, 2) == "-1.50" && formatFixed(-0.0004, 3) == "0.000"
          && formatFixed(0.05, 3) == "0.050", "fixed-point formatting");
    const double p[3] = { 12.345, -0.5, 4000.25 };
    const double r[4] = { 0.0, 0.0, 0.70710678, 0.70710678 };
    const std::string sent = "9|" + formatTuple(p, 3, 3) + "|" + formatTuple(p, 3, 2) + "|" + formatTuple(r, 4, 4) + "|T";
    CHECK(parseFields(sent, l) && std::fabs(l.pos.x - 12.345) < 1e-9 && std::fabs(l.pos.z - 4000.25) < 1e-9,
          "what the sender formats parses back (%s)", sent.c_str());
    CHECK(!parseFields("1|<1,5,2,0,3,0>|<0,0,0>|<0,0,0,1>|", l), "decimal-comma numbers are rejected");
}

static void testBound()
{
    CHECK(withinBound(Vec3(10, 10, 10), Vec3(11.9, 10, 10)), "1.9 m is within the bound");
    CHECK(!withinBound(Vec3(10, 10, 10), Vec3(12.1, 10, 10)), "2.1 m is outside the bound");

    // Followed within 2 m: eases towards the shared position, no snap on the first frame.
    View v;
    Line l;
    parseFields("1|<0,0,0>|<0,0,0>|<0,0,0,1>|T", l);
    v.takeLine(l, Vec3(101.5, 100, 20), 0.0);
    const Quat q;
    Vec3 off = v.step(Vec3(100, 100, 20), q, Vec3(), 0.0167, 0.0167);
    CHECK(off.x > 0.0 && off.x < 0.3, "first frame moves only part of the way (%.3f)", off.x);
    for (int i = 0; i < 120; ++i)
        off = v.step(Vec3(100, 100, 20), q, Vec3(), 0.0167 * (i + 2), 0.0167);
    CHECK(std::fabs(off.x - 1.5) < 0.01 && v.weight > 0.99, "settles on the shared position (%.3f)", off.x);

    // The sim's placement is moved (still standing) so the shared position is 2.5 m away:
    // rejected, eases back.
    off = v.step(Vec3(99, 100, 20), q, Vec3(), 2.1, 0.0167);
    CHECK(!v.hasTarget && v.rejected == 1, "over 2 m: the line is rejected");
    CHECK(off.x > 0.5, "no snap back either (%.3f)", off.x);
    for (int i = 0; i < 200; ++i)
        off = v.step(Vec3(99, 100, 20), q, Vec3(), 2.2 + 0.0167 * i, 0.0167);
    CHECK(off.length() == 0.0 && !v.busy(), "back on sim placement, idle");

    // A line that is far away from the start is never shown, not even partly.
    View w;
    w.takeLine(l, Vec3(110, 100, 20), 0.0);
    off = w.step(Vec3(100, 100, 20), q, Vec3(), 0.01, 0.0167);
    CHECK(off.length() == 0.0 && w.rejected == 1, "a 10 m line moves nothing");

    // Even mid-blend the offset never exceeds the bound.
    View x;
    x.offset = Vec3(5, 0, 0);
    x.weight = 1.0;
    off = x.step(Vec3(), q, Vec3(), 0.0, 0.0167);
    CHECK(off.length() <= BOUND + 1e-9, "offset clamped to the bound");

    // Seated: sim placement.
    View s;
    parseFields("1|<0,0,0>|<0,0,0>|<0,0,0,1>|S", l);
    s.takeLine(l, Vec3(101, 100, 20), 0.0);
    off = s.step(Vec3(100, 100, 20), q, Vec3(), 0.01, 0.0167);
    CHECK(off.length() == 0.0 && !s.hasTarget, "a seated (hold) line keeps the sim's placement");
    parseFields("1|<0,0,0>|<0,0,0>|<0,0,0,1>|ST", l);
    s.takeLine(l, Vec3(101, 100, 20), 0.0);
    CHECK(!s.hasTarget, "seated and still is still a hold");
}

// #1526: one avatar standing at sim x = 100 with a precise position 0.3 m ahead (x = 100.3),
// eased in; returns the view.
static View settledView()
{
    View v;
    Line l;
    parseFields("1|<0,0,0>|<0,0,0>|<0,0,0,1>|T", l);
    v.takeLine(l, Vec3(100.3, 100, 20), 0.0);
    const Quat q;
    for (int i = 0; i < 120; ++i)
        v.step(Vec3(100, 100, 20), q, Vec3(), 0.0167 * i, 0.0167);
    return v;
}

static void testRelease()
{
    const Quat q;
    const double dt = 0.0167;

    // Stationary: eased in on the precise position, smoothly.
    View v = settledView();
    CHECK(std::fabs(v.offset.x - 0.3) < 0.005 && v.engaged, "a standing avatar eases in (%.3f)", v.offset.x);

    // Moving lines are never drawn (an old sender walking): nothing eases in.
    {
        View m;
        Line l;
        parseFields("1|<0,0,0>|<3,0,0>|<0,0,0,1>|", l);
        m.takeLine(l, Vec3(100.3, 100, 20), 0.0);
        Vec3 o;
        for (int i = 0; i < 30; ++i)
            o = m.step(Vec3(100 + 3 * dt * i, 100, 20), q, Vec3(3, 0, 0), dt * i, dt);
        CHECK(o.length() == 0.0 && !m.hasTarget, "a moving line is not drawn (%.3f)", o.x);
    }

    // The sim starts moving it (walking +x at 3 m/s, the precise position is ahead): released
    // on the first moving frame, and the drawn position never goes back or stops, never more
    // than half the sim's step off it, and the offset is gone within twice its length.
    {
        View w = settledView();
        double sim_x = 100.0, drawn = sim_x + w.offset.x, t = 2.0;
        bool back = false, too_fast = false;
        int frames = 0;
        for (int i = 0; i < 60 && w.busy(); ++i, ++frames)
        {
            sim_x += 3.0 * dt;
            t += dt;
            const Vec3 o = w.step(Vec3(sim_x, 100, 20), q, Vec3(3, 0, 0), t, dt);
            const double d = sim_x + o.x;
            const double moved = d - drawn;
            if (moved < 0.5 * 3.0 * dt - 1e-9)
                back = true;
            if (moved > 1.5 * 3.0 * dt + 1e-9)
                too_fast = true;
            drawn = d;
        }
        CHECK(!back && !too_fast, "the hand-off moves with the sim: 50-150%% of its step each frame");
        CHECK(!w.busy() && w.offset.length() == 0.0, "handed back to interpolation (%d frames)", frames);
        CHECK(sim_x - 100.0 <= 2.0 * 0.3 + 3.0 * dt + 1e-6, "gone within twice the offset of travel (%.3f m)", sim_x - 100.0);
    }

    // Walking backwards (-x, the offset is behind the motion): the same rule, never a pop.
    {
        View w = settledView();
        double sim_x = 100.0, drawn = sim_x + w.offset.x, t = 2.0;
        bool bad = false;
        for (int i = 0; i < 60 && w.busy(); ++i)
        {
            sim_x -= 3.0 * dt;
            t += dt;
            const Vec3 o = w.step(Vec3(sim_x, 100, 20), q, Vec3(-3, 0, 0), t, dt);
            const double moved = drawn - (sim_x + o.x);
            if (moved < 0.5 * 3.0 * dt - 1e-9 || moved > 1.5 * 3.0 * dt + 1e-9)
                bad = true;
            drawn = sim_x + o.x;
        }
        CHECK(!bad && !w.busy(), "moving away from the offset: also 50-150%% of the sim's step");
    }

    // Velocity alone (the sim says it moves, the placement hasn't yet): not drawn this frame,
    // and the offset is held (no slide while standing), then released by distance.
    {
        View w = settledView();
        const Vec3 o = w.step(Vec3(100, 100, 20), q, Vec3(1.5, 0, 0), 2.0, dt);
        CHECK(std::fabs(o.x - 0.3) < 0.005 && w.hasTarget, "velocity only: held, not eased back (%.3f)", o.x);
        double sim_x = 100.0;
        double t = 2.0;
        for (int i = 0; i < 20; ++i)
        {
            sim_x += 1.5 * dt;
            t += dt;
            w.step(Vec3(sim_x, 100, 20), q, Vec3(1.5, 0, 0), t, dt);
        }
        CHECK(!w.hasTarget, "walked off more than RELEASE_MOVE: the line is done");
        // Stops again at the new spot: the old line is not drawn there.
        for (int i = 0; i < 200; ++i)
            w.step(Vec3(sim_x, 100, 20), q, Vec3(), t + dt * i, dt);
        CHECK(!w.busy(), "never re-engaged at a new spot without a new line");
    }

    // The sender's release line (S) comes before the sim moves it: held until the sim moves,
    // then handed off with the motion; and if the sim never moves, eased out after the hold.
    {
        View w = settledView();
        Line r;
        parseFields("2|<0,0,0>|<3,0,0>|<0,0,0,1>|S", r);
        w.takeLine(r, Vec3(100.3, 100, 20), 2.0);
        CHECK(!w.hasTarget, "a release line lets go");
        Vec3 o = w.step(Vec3(100, 100, 20), q, Vec3(), 2.0 + dt, dt);
        CHECK(std::fabs(o.x - 0.3) < 0.005, "nothing slides while the sim's placement stands (%.3f)", o.x);
        for (int i = 0; i < 150; ++i)
            o = w.step(Vec3(100, 100, 20), q, Vec3(), 2.0 + RELEASE_HOLD + dt * i, dt);
        CHECK(o.length() == 0.0 && !w.busy(), "standing after a release: eased out after the hold");
    }

    // Expiry / a dropped entry: eased out standing still, at once (no hold).
    {
        View w = settledView();
        w.release(2.0, 0.0);
        const Vec3 o = w.step(Vec3(100, 100, 20), q, Vec3(), 2.0 + dt, dt);
        CHECK(o.x < 0.3 && o.x > 0.25, "dropped: eases back, no snap (%.3f)", o.x);
    }

    // A still line that arrives while the sim's placement still moves: waits, then engages
    // where the sim settles; a sim that never settles drops it.
    {
        View w;
        Line l;
        parseFields("1|<0,0,0>|<0,0,0>|<0,0,0,1>|T", l);
        w.takeLine(l, Vec3(100.3, 100, 20), 0.0);
        Vec3 o = w.step(Vec3(99.9, 100, 20), q, Vec3(1, 0, 0), dt, dt);
        CHECK(o.length() == 0.0 && w.hasTarget && !w.engaged, "waits for the sim's placement to stand");
        o = w.step(Vec3(100, 100, 20), q, Vec3(), 2 * dt, dt);
        CHECK(w.engaged && o.x > 0.0, "engages once it stands");
        View n;
        n.takeLine(l, Vec3(100.3, 100, 20), 0.0);
        for (int i = 0; i < 80; ++i)
            n.step(Vec3(100 + 0.001 * i, 100, 20), q, Vec3(1, 0, 0), dt * i, dt);
        CHECK(!n.hasTarget && !n.busy(), "dropped when the sim never settles for it");
    }
}

// #1526: the sender. 10 Hz ticks.
struct SenderRun
{
    Sender s;
    double t = 0.0;
    Vec3 pos = Vec3(128, 128, 22);
    int lines = 0;
    uint8_t last = 0;
    // n ticks at speed (x direction), with or without movement input; returns lines sent
    int ticks(int n, double speed, bool input, bool sitting = false)
    {
        int sent = 0;
        for (int i = 0; i < n; ++i)
        {
            t += 0.1;
            pos.x += speed * 0.1;
            uint8_t f = 0;
            if (s.decide(pos, speed, Quat(), sitting, false, input, false, t, &f))
            {
                s.sentLine(f, pos, Quat(), t);
                ++sent;
                ++lines;
                last = f;
            }
        }
        return sent;
    }
};

static void testSender()
{
    SenderRun r;
    CHECK(r.ticks(50, 3.0, true) == 0, "walking from the start: nothing is sent");
    CHECK(r.ticks(4, 0.0, false) == 0, "standing, before the dwell: nothing");
    CHECK(r.ticks(2, 0.0, false) == 1 && r.last == FLAG_STILL, "settled: one still line");
    CHECK(r.ticks(80, 0.0, false) == 0, "standing: nothing more before the keep-alive");
    CHECK(r.ticks(30, 0.0, false) == 1 && r.last == FLAG_STILL, "10 s keep-alive while standing");
    // Starts walking: exactly one release, and it is a hold (S): viewers from before #1526
    // drag the avatar back towards a bare moving line, and ease out at once on S.
    CHECK(r.ticks(1, 3.0, true) == 1 && r.last == FLAG_SIT, "on the move: one release, flagged S");
    CHECK(r.ticks(100, 3.0, true) == 0, "walking: nothing sent");
    // Movement input with no speed yet (a key just pressed): not settled.
    CHECK(r.ticks(20, 0.0, true) == 0, "input held, not moving yet: nothing");
    CHECK(r.ticks(6, 0.0, false) == 1 && r.last == FLAG_STILL, "settles again: a still line");
    // Creeping slower than SEND_STILL_SPEED counts as standing: one update once 5 cm off.
    CHECK(r.ticks(20, 0.03, false) == 1 && r.last == FLAG_STILL, "creeping 3 cm/s: one still update");
    // Turning in place is not moving: the new rotation goes out.
    {
        SenderRun u;
        u.ticks(7, 0.0, false);
        CHECK(u.lines == 1, "settled");
        uint8_t f = 0;
        Quat turned;
        turned.z = std::sin(0.1);
        turned.w = std::cos(0.1);
        u.t += 0.3;
        CHECK(u.s.decide(u.pos, 0.0, turned, false, false, false, false, u.t, &f) && f == FLAG_STILL,
              "turning in place: a still line with the new rotation");
    }
    // Seated: hold lines as before #1526, never a release.
    {
        SenderRun u;
        CHECK(u.ticks(3, 0.0, false, true) == 1 && u.last == (FLAG_SIT | FLAG_STILL), "seated: one hold line");
        CHECK(u.ticks(20, 0.0, false, true) == 0, "seated: nothing more before the keep-alive");
        CHECK(u.ticks(20, 3.0, true) == 0, "stood up and walked off: nothing (no still line was out)");
    }
    // The helper restarted (forget) while walking: nothing, not even a release.
    {
        SenderRun u;
        u.ticks(7, 0.0, false);
        u.s.forget();
        CHECK(u.ticks(20, 3.0, true) == 0, "after a helper restart, walking sends nothing");
    }
    // Never more than one line per SEND_MIN_INTERVAL: a release right after a still line waits.
    {
        SenderRun u;
        u.ticks(7, 0.0, false);
        CHECK(u.lines == 1, "settled");
        CHECK(u.ticks(1, 3.0, true) == 0, "release held back by the 0.25 s interval");
        CHECK(u.ticks(2, 3.0, true) == 1 && u.last == FLAG_SIT, "then sent once");
    }
}

static void testTable()
{
    Table<int> table;
    for (int i = 0; i < (int)MAX_ENTRIES; ++i)
        CHECK(table.accept(i, 1, 0, 0.0), "room for avatar %d", i);
    CHECK(table.size() == MAX_ENTRIES, "table full");
    CHECK(!table.accept(1000, 1, 0, 1.0), "a newcomer waits while the table is full");
    CHECK(table.accept(5, 2, 0, 1.0), "a known avatar still updates");
    // Moving entries expire after EXPIRE_MOVING; the refreshed one (5) stays.
    int dropped = 0;
    table.expire(EXPIRE_MOVING + 0.5, [&](int) { ++dropped; });
    CHECK(dropped == (int)MAX_ENTRIES - 1 && table.size() == 1 && table.contains(5), "expired (%d)", dropped);
    CHECK(table.accept(1000, 1, 0, EXPIRE_MOVING + 0.6), "room again after expiry");
    // A still entry lives longer.
    Table<int> still;
    still.accept(1, 1, FLAG_STILL, 0.0);
    still.expire(EXPIRE_MOVING + 1.0, [](int) {});
    CHECK(still.contains(1), "a still entry outlives the moving expiry");
    still.expire(EXPIRE_STILL + 1.0, [](int) {});
    CHECK(!still.contains(1), "and expires after EXPIRE_STILL");
    // An expired (not yet swept) entry makes room for a newcomer.
    Table<int> full;
    for (int i = 0; i < (int)MAX_ENTRIES; ++i)
        full.accept(i, 1, 0, 0.0);
    CHECK(full.accept(999, 1, 0, EXPIRE_MOVING + 0.1), "an expired slot is reused");
}

static void testSeq()
{
    CHECK(seqNewer(2, 1) && !seqNewer(1, 2) && !seqNewer(5, 5), "plain order");
    CHECK(seqNewer(0, 65535) && seqNewer(3, 65530), "wraps");
    Table<int> t;
    CHECK(t.accept(1, 10, 0, 0.0), "first");
    CHECK(!t.accept(1, 10, 0, 0.1), "repeat dropped");
    CHECK(!t.accept(1, 9, 0, 0.2), "older dropped");
    CHECK(t.accept(1, 11, 0, 0.3), "newer used");
    CHECK(t.accept(1, 3, 0, 0.3 + RESYNC_AFTER + 0.1), "a restarted counter is taken after a quiet spell");
}

static void testShare()
{
    CHECK(shouldShare(true, false, true), "on, no RLV, bridge ready: send");
    CHECK(!shouldShare(false, false, true), "option off: nothing is sent");
    CHECK(!shouldShare(true, true, true), "an RLV restriction that hides position: nothing is sent");
    CHECK(!shouldShare(true, false, false), "no bridge: nothing is sent");
}

int main()
{
    testParse();
    testBound();
    testRelease();
    testSender();
    testTable();
    testSeq();
    testShare();
    if (g_fail)
    {
        std::printf("%d position sync fixture(s) failed\n", g_fail);
        return 1;
    }
    return 0;
}
