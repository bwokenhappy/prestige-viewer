/**
 * @file slpossyncmath.h
 * @brief Avatar position sync (#1417): the pure parts -- wire parsing, sequence order, the
 *        2 m bound, the bounded per-avatar table and the per-frame blend. No viewer types,
 *        so scripts/check/test_pos_sync.cpp compiles and runs it on its own.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SL_POS_SYNC_MATH_H
#define SL_POS_SYNC_MATH_H

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

// Wire format (helper -> viewer, after the helper's own checks):
//   #slb:pos/1|<owner>|<seq>|<pos>|<vel>|<rot>|<flags>
// <owner> is the sim's llGetOwnerKey() of the attachment that spoke; it is the only identity
// there is. The fields after it never name anyone: parseFields() takes exactly five and
// rejects a line with more, so nothing in a body can say whose position it is.
//   <seq>   0..65535, the sender's counter (wraps)
//   <pos>   region-local render position, LSL vector syntax "<x, y, z>"
//   <vel>   region velocity, m/s
//   <rot>   the avatar's root rotation, LSL rotation syntax "<x, y, z, s>"
//   <flags> any of S (hold), T (still), F (flying); may be empty
//
// Only a still line (T without S) is ever drawn (#1526). S means "hold: draw the sim's
// placement": it is sent while seated, and once when a still avatar starts to move (the
// release). Any line without T, or with S, makes a receiver let go. The release carries S
// rather than no flag at all because viewers from before #1526 extrapolate a bare moving line
// for 0.5 s and then drag the avatar back towards it, while they ease out at once on S.
namespace SLPosSyncMath
{
constexpr double BOUND           = 2.0;     // m: a shared position never differs more from the sim's
constexpr double EXPIRE_MOVING   = 3.0;     // s: entries for hold / moving lines (nothing is drawn for them)
constexpr double EXPIRE_STILL    = 25.0;    // s: two missed 10 s keep-alives (and a half)
constexpr double BLEND_IN_TAU    = 0.12;    // s: ease towards a shared position
constexpr double BLEND_OUT_TAU   = 0.3;     // s: ease back to the sim's placement while it stands still
constexpr double ROT_MAX_COS_HALF = 0.70710678; // a shared rotation more than 90 degrees off is not shown
constexpr double MAX_SPEED       = 50.0;    // m/s: faster velocities are not trusted
constexpr double RESYNC_AFTER    = 10.0;    // s: a sender quiet this long may restart its counter
constexpr size_t MAX_ENTRIES     = 32;      // avatars followed at once

// Receiver (#1526): a still line is drawn only while the sim's placement stands still.
constexpr double RELEASE_SPEED   = 0.25;    // m/s: the sim moves the avatar this fast: not drawn (this frame)
constexpr double RELEASE_MOVE    = 0.2;     // m: the sim moved it this far from where it settled: the line is done
constexpr double ENGAGE_WAIT     = 1.0;     // s: a still line the sim's placement never settled for is dropped
constexpr double RELEASE_HOLD    = 1.0;     // s: after a release, the offset waits this long for the sim to move
constexpr double RELEASE_SHARE   = 0.5;     // hand-off: the offset shrinks by at most this share of the sim's own motion

// Sender (#1526): the precise position goes out only once the avatar has settled.
constexpr double SEND_STILL_SPEED = 0.05;   // m/s: slower than this counts as standing
constexpr double SETTLE_DWELL    = 0.5;     // s: standing, no movement input, drawn position steady this long
constexpr double SETTLE_JITTER   = 0.01;    // m: the drawn position moved more than this since the last check
constexpr double SEND_MIN_INTERVAL = 0.25;  // s: at most four lines a second (the helper's own gap is 0.2 s)
constexpr double KEEP_STILL      = 10.0;    // s: keep-alive while settled or seated
constexpr double SEND_MOVE       = 0.05;    // m: settled somewhere else
constexpr double SEND_TURN       = 0.0872665; // rad (5 degrees): turned in place

enum : uint8_t
{
    FLAG_SIT   = 1,
    FLAG_STILL = 2,
    FLAG_FLY   = 4,
};

struct Vec3
{
    double x = 0.0, y = 0.0, z = 0.0;
    Vec3() = default;
    Vec3(double ax, double ay, double az) : x(ax), y(ay), z(az) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(double s) const { return Vec3(x * s, y * s, z * s); }
    double length() const { return std::sqrt(x * x + y * y + z * z); }
    bool finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};

struct Quat
{
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};

inline double angleBetween(const Quat& a, const Quat& b)
{
    const double d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0 * std::acos(std::fmin(d, 1.0));
}

// A line a receiver may draw: still, and not hold.
inline bool isStillLine(uint8_t flags)
{
    return (flags & FLAG_STILL) && !(flags & FLAG_SIT);
}

struct Line
{
    uint16_t seq = 0;
    Vec3 pos;       // region-local
    Vec3 vel;
    Quat rot;
    uint8_t flags = 0;
};

inline double expiryFor(uint8_t flags)
{
    return (flags & FLAG_STILL) ? EXPIRE_STILL : EXPIRE_MOVING;
}

// a is newer than b (serial number arithmetic, the counter wraps at 65536)
inline bool seqNewer(uint16_t a, uint16_t b)
{
    return a != b && (uint16_t)(a - b) < 0x8000;
}

// The shared position may be shown: within BOUND of where the sim puts the avatar.
inline bool withinBound(const Vec3& shared, const Vec3& sim)
{
    const Vec3 d = shared - sim;
    return d.finite() && d.length() <= BOUND;
}

// Fixed-point text ("-12.345") that never follows the user's locale: the helper's LSL casts
// and parseTuple both expect a decimal point.
inline std::string formatFixed(double v, int decimals)
{
    if (!std::isfinite(v))
        v = 0.0;
    long long scale = 1;
    for (int i = 0; i < decimals; ++i)
        scale *= 10;
    long long q = std::llround(v * (double)scale);
    std::string out;
    if (q < 0)
    {
        out += '-';
        q = -q;
    }
    out += std::to_string(q / scale);
    if (decimals > 0)
    {
        const std::string frac = std::to_string(q % scale);
        out += '.';
        out.append((size_t)decimals - frac.size(), '0');
        out += frac;
    }
    return out;
}

// "<a,b,c>" / "<a,b,c,d>" of formatFixed numbers.
inline std::string formatTuple(const double* v, int n, int decimals)
{
    std::string out = "<";
    for (int i = 0; i < n; ++i)
    {
        if (i)
            out += ',';
        out += formatFixed(v[i], decimals);
    }
    return out + ">";
}

// "<a, b, c>" or "<a, b, c, d>" (LSL syntax, spaces optional) into n numbers.
inline bool parseTuple(const std::string& s, double* out, int n)
{
    if (s.size() < 5 || s.size() > 80 || s.front() != '<' || s.back() != '>')
        return false;
    const std::string body = s.substr(1, s.size() - 2);
    int got = 0;
    size_t start = 0;
    for (;;)
    {
        const size_t comma = body.find(',', start);
        if (got == n)
            return false;   // more numbers than asked for
        const std::string part = body.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        // std::from_chars: never the user's locale (a decimal comma would split the tuple).
        const char* p = part.data();
        const char* end = p + part.size();
        while (p < end && *p == ' ')
            ++p;
        double v = 0.0;
        const std::from_chars_result r = std::from_chars(p, end, v);
        if (r.ec != std::errc() || r.ptr == p || !std::isfinite(v))
            return false;
        p = r.ptr;
        while (p < end && *p == ' ')
            ++p;
        if (p != end)
            return false;
        out[got++] = v;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return got == n;
}

// The five fields after the sim-given owner: "<seq>|<pos>|<vel>|<rot>|<flags>". Exactly five;
// anything else (a sixth field, an owner key smuggled into the body) is rejected whole.
inline bool parseFields(const std::string& fields, Line& out)
{
    std::array<std::string, 5> f;
    size_t start = 0;
    for (size_t i = 0; i < f.size(); ++i)
    {
        const size_t bar = fields.find('|', start);
        if (i + 1 < f.size())
        {
            if (bar == std::string::npos)
                return false;
            f[i] = fields.substr(start, bar - start);
            start = bar + 1;
        }
        else
        {
            if (bar != std::string::npos)
                return false;
            f[i] = fields.substr(start);
        }
    }
    if (f[0].empty() || f[0].size() > 5 || f[0].find_first_not_of("0123456789") != std::string::npos)
        return false;
    const unsigned long seq = std::strtoul(f[0].c_str(), nullptr, 10);
    if (seq > 65535)
        return false;
    double p[3], v[3], r[4];
    if (!parseTuple(f[1], p, 3) || !parseTuple(f[2], v, 3) || !parseTuple(f[3], r, 4))
        return false;
    if (f[4].size() > 3 || f[4].find_first_not_of("STF") != std::string::npos)
        return false;
    Line line;
    line.seq = (uint16_t)seq;
    line.pos = Vec3(p[0], p[1], p[2]);
    line.vel = Vec3(v[0], v[1], v[2]);
    if (line.vel.length() > MAX_SPEED)
        line.vel = line.vel * (MAX_SPEED / line.vel.length());
    const double n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    if (n < 0.5 || n > 1.5)
        return false;
    line.rot.x = r[0] / n;
    line.rot.y = r[1] / n;
    line.rot.z = r[2] / n;
    line.rot.w = r[3] / n;
    for (char c : f[4])
        line.flags |= (c == 'S') ? FLAG_SIT : (c == 'T') ? FLAG_STILL : FLAG_FLY;
    out = line;
    return true;
}

// One followed avatar, keyed by whatever the viewer uses for avatar ids. Fixed capacity: no
// growth with the crowd beyond MAX_ENTRIES.
template <class Key>
class Table
{
public:
    struct Entry
    {
        Key key{};
        bool used = false;
        uint16_t seq = 0;
        uint8_t flags = 0;
        double heard = 0.0;
    };

    // A line from key (as the sim identified it) with seq: true if it is to be used (a new
    // avatar with room, or newer than the last line). Full table: a newcomer is refused
    // until an entry expires; a stale-but-unexpired entry is never displaced by a stranger.
    bool accept(const Key& key, uint16_t seq, uint8_t flags, double now)
    {
        Entry* free_slot = nullptr;
        for (Entry& e : mEntries)
        {
            if (e.used && e.key == key)
            {
                if (!seqNewer(seq, e.seq) && now - e.heard < RESYNC_AFTER)
                    return false;   // repeated or out of order
                e.seq = seq;
                e.flags = flags;
                e.heard = now;
                return true;
            }
            if (!free_slot && (!e.used || now - e.heard > expiryFor(e.flags)))
                free_slot = &e;
        }
        if (!free_slot)
            return false;
        free_slot->key = key;
        free_slot->used = true;
        free_slot->seq = seq;
        free_slot->flags = flags;
        free_slot->heard = now;
        return true;
    }

    template <class F>
    void expire(double now, F on_drop)
    {
        for (Entry& e : mEntries)
        {
            if (e.used && now - e.heard > expiryFor(e.flags))
            {
                e.used = false;
                on_drop(e.key);
            }
        }
    }

    void erase(const Key& key)
    {
        for (Entry& e : mEntries)
        {
            if (e.used && e.key == key)
                e.used = false;
        }
    }

    bool contains(const Key& key) const
    {
        for (const Entry& e : mEntries)
        {
            if (e.used && e.key == key)
                return true;
        }
        return false;
    }

    size_t size() const
    {
        size_t n = 0;
        for (const Entry& e : mEntries)
            n += e.used ? 1 : 0;
        return n;
    }

    template <class F>
    void forEach(F f) const
    {
        for (const Entry& e : mEntries)
        {
            if (e.used)
                f(e.key);
        }
    }

private:
    std::array<Entry, MAX_ENTRIES> mEntries{};
};

// The per-avatar state a receiver keeps for drawing: the last accepted still line (in a
// global frame the caller chooses) and what is shown now. step() runs every frame for an
// avatar that is busy(); the offset it returns is added to the sim-driven root position.
//
// #1526: a still line is drawn only while the sim's placement stands still too. It is
// engaged (anchored at the sim's placement) on the first frame the sim's velocity is under
// RELEASE_SPEED, and not drawn on any frame it is over; once the sim's placement is
// RELEASE_MOVE from the anchor, or a release (hold / moving) line arrives, the line is done.
// The hand-off then shrinks the offset only as fast as RELEASE_SHARE of the sim's own motion
// each frame: the drawn avatar moves with the sim from the first frame, at 50-150% of its
// speed, never against it, and the offset is gone after twice its length of travel. Only
// while the sim's placement stands still (RELEASE_HOLD after a release; at once on expiry,
// the 2 m bound or a dropped entry) does it ease back with BLEND_OUT_TAU as before.
struct View
{
    // last accepted still line
    bool hasTarget = false;
    Vec3 pos;           // global
    Quat rot;
    uint8_t flags = 0;
    double heard = 0.0;
    bool engaged = false;   // the sim's placement settled for this line ...
    Vec3 anchor;            // ... here
    // shown
    Vec3 offset;
    double weight = 0.0;    // 0 = sim placement, 1 = the shared one
    Quat shownRot;
    bool rotShown = false;  // the shared rotation is part of what is shown
    double fallbackAt = 0.0;    // from then on, the offset also eases out while the sim stands still
    bool validLast = false;     // the line was drawn last frame
    // the sim-driven position seen last frame (the hand-off follows its motion)
    Vec3 lastSim;
    bool haveSim = false;
    unsigned rejected = 0;
    // Measurements (#1417 test plan), per frame while a shared position is followed:
    //   gap      |shared - sim|: how far the sim-driven placement is from where the sender
    //            draws itself (what other viewers show without position sync)
    //   residual |shared - drawn|: what is left after the blend
    double gapSum = 0.0, gapMax = 0.0, residSum = 0.0, residMax = 0.0;
    unsigned frames = 0;
    // #1526: hand-offs started by the sim moving the avatar, and lines done by RELEASE_MOVE
    unsigned letGo = 0, walkedOff = 0;

    void takeStats(double& gap_sum, double& gap_max, double& resid_sum, double& resid_max, unsigned& n,
                   unsigned& let_go, unsigned& walked_off)
    {
        gap_sum += gapSum;
        resid_sum += residSum;
        gap_max = std::fmax(gap_max, gapMax);
        resid_max = std::fmax(resid_max, residMax);
        n += frames;
        let_go += letGo;
        walked_off += walkedOff;
        gapSum = gapMax = residSum = residMax = 0.0;
        frames = 0;
        letGo = walkedOff = 0;
    }

    bool busy() const { return hasTarget || weight > 0.0 || offset.length() > 0.0; }

    // A line accepted from the table (sequence order checked). A still line becomes the
    // target; anything else (hold, moving) is the sender letting go.
    void takeLine(const Line& line, const Vec3& global_pos, double now)
    {
        if (!isStillLine(line.flags))
        {
            release(now, RELEASE_HOLD);
            return;
        }
        hasTarget = true;
        pos = global_pos;
        rot = line.rot;
        flags = line.flags;
        heard = now;
        // engaged / anchor stay: a keep-alive or a turn in place doesn't restart the line
    }

    // Stop drawing the line. hold: how long the offset waits for the sim's placement to move
    // before it also eases out standing still (0: at once).
    void release(double now, double hold)
    {
        if (hasTarget || engaged)
            fallbackAt = now + hold;
        hasTarget = false;
        engaged = false;
    }

    // Seated (the seat places the avatar): back to the sim at once; the sit is itself a jump.
    void reset()
    {
        hasTarget = false;
        engaged = false;
        validLast = false;
        offset = Vec3();
        weight = 0.0;
        rotShown = false;
        haveSim = false;
    }

    // The offset to draw this frame. sim: where the sim-driven placement puts the avatar
    // now (same global frame as pos); sim_rot: its rotation; sim_vel: its velocity.
    Vec3 step(const Vec3& sim, const Quat& sim_rot, const Vec3& sim_vel, double now, double dt)
    {
        const Vec3 sim_step = haveSim ? sim - lastSim : Vec3();
        lastSim = sim;
        haveSim = true;
        Vec3 target;
        bool valid = false;
        if (hasTarget)
        {
            const bool sim_moving = sim_vel.length() > RELEASE_SPEED;
            if (now - heard > EXPIRE_STILL)
            {
                release(now, 0.0);
            }
            else if (!withinBound(pos, sim))
            {
                // Rejected: this line no longer counts; the next one is checked afresh.
                release(now, 0.0);
                ++rejected;
            }
            else if (engaged && (sim - anchor).length() > RELEASE_MOVE)
            {
                // The avatar walked off: this line is done (never re-engaged somewhere else).
                release(now, RELEASE_HOLD);
                ++walkedOff;
            }
            else if (!engaged && now - heard > ENGAGE_WAIT && sim_moving)
            {
                // The sim's placement never settled for it.
                release(now, 0.0);
            }
            else if (!sim_moving)
            {
                if (!engaged)
                {
                    engaged = true;
                    anchor = sim;
                }
                target = pos - sim;
                valid = true;
            }
            else if (validLast)
            {
                // Moving this frame (not yet RELEASE_MOVE): not drawn; the hand-off below.
                fallbackAt = now + RELEASE_HOLD;
                ++letGo;
            }
        }
        validLast = valid;

        if (valid)
        {
            const double a = (dt > 0.0) ? 1.0 - std::exp(-dt / BLEND_IN_TAU) : 0.0;
            offset = offset + (target - offset) * a;
            weight = weight + (1.0 - weight) * a;
        }
        else
        {
            // Hand-off: shrink the offset with the sim's own motion (never against it, never a
            // pop); standing still, ease out only after the hold (or at once, see release()).
            const double len = offset.length();
            double cut = RELEASE_SHARE * sim_step.length();
            if (now >= fallbackAt && dt > 0.0)
                cut = std::fmax(cut, len * (1.0 - std::exp(-dt / BLEND_OUT_TAU)));
            if (len > 0.0 && std::isfinite(len) && cut < len)
            {
                offset = offset * ((len - cut) / len);
                weight = weight * ((len - cut) / len);
            }
            else
            {
                offset = Vec3();
                weight = 0.0;
            }
        }
        // Never more than BOUND from the sim's placement, whatever the blend is doing.
        const double len = offset.length();
        if (!std::isfinite(len))
        {
            offset = Vec3();
            weight = 0.0;
        }
        else if (len > BOUND)
        {
            offset = offset * (BOUND / len);
        }
        if (!valid && offset.length() < 0.0005)
        {
            offset = Vec3();
            weight = 0.0;
        }
        if (!busy())
            haveSim = false;    // the next frame that follows starts its motion afresh
        if (valid)
        {
            const double gap = target.length();
            const double resid = (target - offset).length();
            gapSum += gap;
            residSum += resid;
            gapMax = std::fmax(gapMax, gap);
            residMax = std::fmax(residMax, resid);
            ++frames;
        }

        // Rotation: eased towards the shared one; not shown when it is far from the sim's.
        if (valid)
        {
            const double d = rot.x * sim_rot.x + rot.y * sim_rot.y + rot.z * sim_rot.z + rot.w * sim_rot.w;
            if (std::fabs(d) < ROT_MAX_COS_HALF)
                valid = false;
        }
        if (valid)
        {
            if (!rotShown)
            {
                shownRot = sim_rot;
                rotShown = true;
            }
            Quat q = rot;
            const double d = q.x * shownRot.x + q.y * shownRot.y + q.z * shownRot.z + q.w * shownRot.w;
            if (d < 0.0)
            {
                q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w;
            }
            const double b = (dt > 0.0) ? 1.0 - std::exp(-dt / BLEND_IN_TAU) : 0.0;
            shownRot.x += (q.x - shownRot.x) * b;
            shownRot.y += (q.y - shownRot.y) * b;
            shownRot.z += (q.z - shownRot.z) * b;
            shownRot.w += (q.w - shownRot.w) * b;
            const double n = std::sqrt(shownRot.x * shownRot.x + shownRot.y * shownRot.y
                                       + shownRot.z * shownRot.z + shownRot.w * shownRot.w);
            if (n > 1e-6)
            {
                shownRot.x /= n; shownRot.y /= n; shownRot.z /= n; shownRot.w /= n;
            }
            else
            {
                rotShown = false;
            }
        }
        else
        {
            // Hand the rotation back: the avatar's own orientation code eases from here.
            rotShown = false;
        }
        return offset;
    }
};

// The sender's decision (#1526), once per tick: our precise position goes out only once the
// avatar has settled, never while it moves. Settled = not seated, slower than
// SEND_STILL_SPEED, no movement input (translation controls, autopilot), and the drawn
// position no more than SETTLE_JITTER off the previous tick's, all for SETTLE_DWELL. Then a
// still line (T) at once, again when it settles SEND_MOVE elsewhere or turns SEND_TURN in
// place, and a KEEP_STILL keep-alive. On the move after a still line: one release line (S,
// "hold"), then nothing until it settles again. Seated: hold lines on change and KEEP_STILL,
// as before #1526. At most one line per SEND_MIN_INTERVAL.
struct Sender
{
    bool sent = false;          // a line went out since the helper last knew nothing of us
    uint8_t sentFlags = 0;
    double sentAt = 0.0;
    Vec3 sentPos;
    Quat sentRot;
    bool haveLast = false;
    Vec3 lastPos;               // drawn position at the previous tick
    double stillSince = 0.0;    // standing still since

    // pos: our drawn position (global); speed: |velocity|; move_input: translation controls held
    // or autopilot; new_region: the last line went out in another region. True when a line is
    // due now, with its flags in *out_flags; call sentLine() once it went out.
    bool decide(const Vec3& pos, double speed, const Quat& rot, bool sitting, bool flying, bool move_input,
                bool new_region, double now, uint8_t* out_flags)
    {
        const bool slow = speed < SEND_STILL_SPEED;
        const bool moving = !slow || move_input;
        if (sitting || moving || !haveLast || (pos - lastPos).length() > SETTLE_JITTER)
            stillSince = now;
        haveLast = true;
        lastPos = pos;
        const bool settled = !sitting && !moving && now - stillSince >= SETTLE_DWELL;

        uint8_t flags = flying ? FLAG_FLY : 0;
        if (sitting)
            flags |= FLAG_SIT | (slow ? FLAG_STILL : 0);
        else if (settled)
            flags |= FLAG_STILL;
        else if (moving && sent && isStillLine(sentFlags))
            flags |= FLAG_SIT;      // the release: receivers let go (S, see the wire format)
        else
            return false;           // moving, or not settled yet: nothing

        const double since = now - sentAt;
        if (sent && !new_region && since < SEND_MIN_INTERVAL)
            return false;
        bool due = !sent || new_region || flags != sentFlags;
        if (!due)
        {
            due = since >= KEEP_STILL;
            if (!sitting)
                due = due || (pos - sentPos).length() > SEND_MOVE || angleBetween(rot, sentRot) > SEND_TURN;
        }
        if (due)
            *out_flags = flags;
        return due;
    }

    void sentLine(uint8_t flags, const Vec3& pos, const Quat& rot, double now)
    {
        sent = true;
        sentFlags = flags;
        sentAt = now;
        sentPos = pos;
        sentRot = rot;
    }

    // The helper knows nothing of us (restarted, sharing off): nothing to release either.
    void forget() { sent = false; }
};

// Whether our own position goes out: the user's option, no RLV restriction that hides
// where we are, and a working bridge.
inline bool shouldShare(bool option_on, bool rlv_hides_position, bool bridge_ready)
{
    return option_on && !rlv_hides_position && bridge_ready;
}
} // namespace SLPosSyncMath

#endif // SL_POS_SYNC_MATH_H
