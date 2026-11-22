// #Slipstream Helper Extras -- the second script of the Slipstream viewer's helper (#500).
//
// Slipstream Viewer Source Code, Copyright (C) 2026, Slipstream Viewer contributors.
// LGPL 2.1 (see the viewer's license). Generated into your inventory and uploaded by
// the viewer next to "#Slipstream Helper Script"; edits are overwritten when the viewer
// updates it (the helper's version is a hash of both script files).
//
// Why a second script: one Mono script has 64 KB for its code and its data together, and with
// every feature in one script the code alone took ~58 KB, so a busy region's traffic ran it out
// of memory (Stack-Heap Collision, which stops a script for good). The main script keeps the
// viewer's hello, the pose and consent channels and script info; this one has the features with
// the most traffic:
//   combat log (CombatFX) : region Combat 2.0 events, only while the viewer asks for them: every
//                           death (the kill feed) and the owner's own hits and kills (the hit
//                           marker), version tagged "combat/<name>/<version>" (slcombatfx.h).
//                           Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4.
//   pings (CombatFX)      : "combat/mapto/1|<seq>|<key>,..." then "combat/mapsay/1|<seq>|<line>":
//                           one ping line to each listed avatar in this region (llRegionSayTo,
//                           channel 0), under "Ping from <owner's username>"; the recipients wait
//                           in the object's linkset data (protected), never in script memory.
//   position (#1417)      : "pos/send/1|..." shouted on POS_CHANNEL as "pos/1|..." only while the
//                           viewer shares it, clamped to POS_BOUND of llGetPos(); others' "pos/1"
//                           lines forwarded as "#slb:pos/1|<owner>|..." only for avatars the viewer
//                           listed ("pos/see/1|..."). Visual only.
// It hears the viewer on the main script's private channel (handed over by link message: the
// main script says it on start, and this one asks for it when it starts), handles only its own
// commands and ignores the rest. Like the main script it never speaks first, and it forwards
// nothing unless the viewer drove it within the last minute. Identity always comes from the sim.

integer POS_CHANNEL = [POS_CHANNEL];
integer QUIET_AFTER = 60;       // seconds without a viewer command before we stop forwarding
integer MAX_DEATHS = 8;         // combat log deaths forwarded per second at most
integer MAX_HITS = 16;          // the owner's hits forwarded per second at most (kills get MAX_DEATHS more)
string HELPER_NAME = "[HELPER_NAME]";   // the object's own name (slbridge.cpp)
integer MAX_TO = 50;            // ping recipients per line
integer MEM_FLOOR = 6000;       // bytes left (of the 64 KB) under which pings stop: room for a line
integer LSD_FLOOR = 4096;       // linkset data bytes left under which no more recipients are stored
// The ping recipients live in the object's linkset data, one entry each ("slms.to.<key>"),
// protected with this pass: only the helper's scripts use its linkset data (the viewer strips any
// other), and the script's memory never holds the list.
string TO_PASS = "slms.mapshare.to.v1";
string TO_FIND = "^slms\\.to\\.";
float SAY_BURST = 6.0;          // ping lines (the viewer's limits allow 5 in 30 s) ...
float SAY_RATE = 1.0;           // ... a second

// Position sync (#1417). Sending: at most one line per POS_GAP; a line goes out when the position
// is POS_MOVE off where the last one predicts, the velocity changed by POS_SPEED_STEP, the turn
// is POS_TURN, the flags changed, or a keep-alive is due (POS_KEEP_MOVING / POS_KEEP_STILL).
float POS_BOUND = 2.0;          // m from llGetPos(): never further (clamped here, rejected by receivers)
float POS_SLACK = 0.5;          // receiving: a line further than POS_BOUND + this from the speaker is dropped
float POS_GAP = 0.2;            // s between our own lines (the viewer offers at most 4 a second)
float POS_MOVE = 0.04;          // m
float POS_SPEED_STEP = 0.15;    // m/s
float POS_TURN = 0.07;          // radians (4 degrees)
float POS_KEEP_MOVING = 0.9;    // s
float POS_KEEP_STILL = 9.5;     // s (still or seated)
float POS_MAX_SPEED = 50.0;     // m/s
// Receiving: only avatars in gSee (the viewer's list, at most POS_MAX_SEE), each at most once per
// POS_GAP_IN, all together within POS_RATE a second (burst POS_BURST).
integer POS_MAX_SEE = 24;
float POS_GAP_IN = 0.15;
float POS_RATE = 30.0;
float POS_BURST = 45.0;

key gOwner;
integer gCmdChannel;    // the main script's private channel (link message "slms.ch")
integer gCmdListen;
integer gLastCmd;       // unix time of the last command from the viewer
integer gPosListen;
integer gPublicOn;      // the position listen is enabled (only while the viewer drives us)
integer gCombatWanted;  // the viewer's kill feed asked for the combat log (combat/feed/1|1)
integer gHitsWanted;    // the viewer's hit marker asked for the owner's hits (combat/hits/1|1)
integer gCombatOn;      // the combat log listen below exists (one, shared by both)
integer gCombatListen;  // its handle
integer gDeathSecond;   // unix second of the death budget
integer gDeathsSent;    // deaths forwarded in that second
integer gHitSecond;     // unix second of the hit budget
integer gHitsSent;      // hit lines forwarded in that second
string gToSeq;          // the ping the stored recipients are for
integer gToCount;       // how many are stored (linkset data, see TO_PASS), at most MAX_TO
integer gMemLow;        // low script memory was reported (again once there was enough meanwhile)
integer gLsdLow;        // ... and low linkset data
float gSayTokens;
float gSayTime;
integer gPosShare;      // the viewer shares our position (pos/share/1|1); FALSE until it says so
integer gPosSent;       // a line went out in this region (the thresholds compare against it)
float gPosTime;         // llGetTime() of that line
vector gPosLast;
vector gPosVel;
rotation gPosRot;
string gPosFlags;
list gSee;              // avatars the viewer can plausibly see (keys), at most POS_MAX_SEE
list gSeeT;             // llGetTime() each was last forwarded
float gPosTokens;
float gPosTokTime;

// The name a ping line goes out under, from the line's tag; "" for anything else. (GunMetal-DX also
// says shared marks and zones -- "[Mark] " / "[Zone] " -- which Slipstream does not send.)
string sharePrefix(string line)
{
    if (llGetSubString(line, 0, 6) == "[Ping] ")
        return "Ping from ";
    return "";
}

integer shareName(string name)
{
    return llGetSubString(name, 0, 9) == "Ping from ";
}

// Room for pings: below MEM_FLOOR free script memory (now, not the low-water mark
// llGetFreeMemory gives under Mono) they stop, and the viewer is told once:
// "#slb:combat/mem/1|script|<free bytes>" (or "...|lsd|<free bytes>" for linkset data, see mapTo).
integer memOk()
{
    integer free = llGetMemoryLimit() - llGetUsedMemory();
    if (free >= MEM_FLOOR)
    {
        gMemLow = FALSE;
        return TRUE;
    }
    if (!gMemLow)
        llOwnerSay("#slb:combat/mem/1|script|" + (string)free);
    gMemLow = TRUE;
    return FALSE;
}

// The stored recipients gone (a new ping, one said, a detach).
clearTo()
{
    llLinksetDataDeleteFound(TO_FIND, TO_PASS);
    gToCount = 0;
    gToSeq = "";
}

// "combat/mapto/1|<seq>|<key>,<key>,...": recipients for ping <seq> (a new seq starts a new list, so
// a ping cut short never reaches the next one's people). Only valid keys, never the owner, no
// repeats, at most MAX_TO; none once the linkset data is nearly full (told once, as memOk).
mapTo(string arg)
{
    integer bar = llSubStringIndex(arg, "|");
    if (bar < 1 || bar > 9)
        return;
    string seq = llGetSubString(arg, 0, bar - 1);
    if (seq != gToSeq)
    {
        clearTo();
        gToSeq = seq;
    }
    list ids = llParseString2List(llGetSubString(arg, bar + 1, -1), [","], []);
    integer n = llGetListLength(ids);
    integer i;
    if (llLinksetDataAvailable() >= LSD_FLOOR)
        gLsdLow = FALSE;
    for (i = 0; i < n && gToCount < MAX_TO && memOk(); ++i)
    {
        string id = llList2String(ids, i);
        if (llLinksetDataAvailable() < LSD_FLOOR)
        {
            if (!gLsdLow)
                llOwnerSay("#slb:combat/mem/1|lsd|" + (string)llLinksetDataAvailable());
            gLsdLow = TRUE;
            i = n;
        }
        else if (llStringLength(id) == 36)
            if ((key)id)
                if (id != (string)gOwner && llLinksetDataReadProtected("slms.to." + id, TO_PASS) == "")
                    if (llLinksetDataWriteProtected("slms.to." + id, "1", TO_PASS) == LINKSETDATA_OK)
                        ++gToCount;
    }
}

// "combat/mapsay/1|<seq>|<line>": says the line to ping <seq>'s recipients still in this region, each
// alone (llRegionSayTo, channel 0), under "Ping from <owner>" (the owner's own username, from the
// sim: never a name the viewer sends), then the helper's own name again. Only a line that looks
// like a ping ("[Ping] ..." ending in its "(gmdx:...)" tail -- the tag GunMetal-DX reads too),
// within the budget; the recipients are used once.
mapSay(string arg)
{
    integer bar = llSubStringIndex(arg, "|");
    if (bar < 1 || bar > 9)
        return;
    // (the recipients are read from the linkset data one at a time, each deleted once said)
    if (llGetSubString(arg, 0, bar - 1) != gToSeq)
        clearTo();
    gToSeq = "";
    string line = llGetSubString(arg, bar + 1, -1);
    string prefix = sharePrefix(line);
    if (gToCount == 0 || prefix == "" || llGetSubString(line, -1, -1) != ")" || llSubStringIndex(line, " (gmdx:") < 0
        || !memOk())
    {
        clearTo();
        return;
    }
    float now = llGetTime();
    gSayTokens += (now - gSayTime) * SAY_RATE;
    if (gSayTokens > SAY_BURST)
        gSayTokens = SAY_BURST;
    gSayTime = now;
    if (gSayTokens < 1.0)
    {
        clearTo();
        return;
    }
    gSayTokens -= 1.0;
    string name = llGetUsername(gOwner);
    if (name == "")
        name = llKey2Name(gOwner);
    if (name == "")
    {
        clearTo();
        return;
    }
    string own = llGetObjectName();
    if (shareName(own))
        own = HELPER_NAME;
    llSetObjectName(llGetSubString(prefix + name, 0, 62));
    // at most MAX_TO rounds, so an entry that would not delete cannot keep this going
    integer i;
    list one = llLinksetDataFindKeys(TO_FIND, 0, 1);
    for (i = 0; i < MAX_TO && one != []; ++i)
    {
        string entry = llList2String(one, 0);
        key k = (key)llGetSubString(entry, 8, -1);
        if (llGetAgentSize(k) != ZERO_VECTOR)
            llRegionSayTo(k, 0, line);
        llLinksetDataDeleteProtected(entry, TO_PASS);
        one = llLinksetDataFindKeys(TO_FIND, 0, 1);
    }
    clearTo();
    llSetObjectName(own);
}

// ---- Position sync (#1417) ----
// Field checks shared by what we send and what we forward: "<seq>|<pos>|<vel>|<rot>|<flags>",
// seq a plain 0..65535, the vectors in LSL syntax, flags from "STF" (sitting, still, flying).
integer posSeqOk(string seq)
{
    integer n = (integer)seq;
    return (string)n == seq && n >= 0 && n <= 65535;
}

integer posTupleOk(string s)
{
    integer n = llStringLength(s);
    return n >= 5 && n <= 80 && llGetSubString(s, 0, 0) == "<" && llGetSubString(s, -1, -1) == ">";
}

integer posFlagsOk(string flags)
{
    integer n = llStringLength(flags);
    if (n > 3)
        return FALSE;
    integer i;
    for (i = 0; i < n; ++i)
        if (llSubStringIndex("STF", llGetSubString(flags, i, i)) < 0)
            return FALSE;
    return TRUE;
}

// The five fields checked; FALSE for anything malformed (never a sixth field: a key in a body
// is never read as anyone's identity).
integer posFieldsOk(list f)
{
    return llGetListLength(f) == 5 && posSeqOk(llList2String(f, 0)) && posTupleOk(llList2String(f, 1))
        && posTupleOk(llList2String(f, 2)) && posTupleOk(llList2String(f, 3)) && posFlagsOk(llList2String(f, 4));
}

// "pos/send/1|<seq>|<pos>|<vel>|<rot>|<flags>": our own avatar, from the viewer. Said only while
// the viewer shares it (gPosShare), clamped to POS_BOUND of where the sim has us, only on a real
// change or a keep-alive, never more than one line per POS_GAP.
posSend(string arg)
{
    if (!gPosShare)
        return;
    list f = llParseStringKeepNulls(arg, ["|"], []);
    if (!posFieldsOk(f))
        return;
    vector p = (vector)llList2String(f, 1);
    vector v = (vector)llList2String(f, 2);
    rotation r = (rotation)llList2String(f, 3);
    string flags = llList2String(f, 4);
    vector me = llGetPos();
    if (llVecDist(p, me) > POS_BOUND)
        p = me + llVecNorm(p - me) * POS_BOUND;
    if (llVecMag(v) > POS_MAX_SPEED)
        v = llVecNorm(v) * POS_MAX_SPEED;
    float now = llGetTime();
    float since = now - gPosTime;
    if (gPosSent && since < POS_GAP)
        return;
    float keep = POS_KEEP_MOVING;
    if (llSubStringIndex(flags, "T") >= 0 || llSubStringIndex(flags, "S") >= 0)
        keep = POS_KEEP_STILL;
    if (gPosSent && flags == gPosFlags && since < keep && llVecDist(p, gPosLast + gPosVel * since) < POS_MOVE
        && llVecDist(v, gPosVel) < POS_SPEED_STEP && llAngleBetween(r, gPosRot) < POS_TURN)
        return;
    gPosSent = TRUE;
    gPosTime = now;
    gPosLast = p;
    gPosVel = v;
    gPosRot = r;
    gPosFlags = flags;
    llShout(POS_CHANNEL, "pos/1|" + llList2String(f, 0) + "|" + (string)p + "|" + (string)v + "|" + (string)r + "|" + flags);
}

// "pos/see/1|<key>,<key>,...": the avatars our viewer can plausibly see (nearest first, at most
// POS_MAX_SEE); only their lines are forwarded. Replaces the list; never the owner.
posSee(string arg)
{
    list ids = llParseString2List(arg, [","], []);
    integer n = llGetListLength(ids);
    if (n > POS_MAX_SEE)
        n = POS_MAX_SEE;
    list see = [];
    list times = [];
    integer i;
    for (i = 0; i < n; ++i)
    {
        string id = llList2String(ids, i);
        if (llStringLength(id) == 36)
            if ((key)id)
                if (id != (string)gOwner)
                {
                    integer old = llListFindList(gSee, [(key)id]);
                    float t = -100.0;
                    if (old >= 0)
                        t = llList2Float(gSeeT, old);
                    see += [(key)id];
                    times += [t];
                }
    }
    gSee = see;
    gSeeT = times;
}

// All forwarded position lines together: POS_RATE a second, burst POS_BURST.
integer posAllow(float now)
{
    gPosTokens += (now - gPosTokTime) * POS_RATE;
    if (gPosTokens > POS_BURST)
        gPosTokens = POS_BURST;
    gPosTokTime = now;
    if (gPosTokens < 1.0)
        return FALSE;
    gPosTokens -= 1.0;
    return TRUE;
}

// Speaker identity on the public channels: an attachment speaks for its owner; anything
// else (rezzed objects, avatars typing on the channel) is not an attachment.
key attachedOwner(key id)
{
    if (llList2Integer(llGetObjectDetails(id, [OBJECT_ATTACHED_POINT]), 0) == 0)
        return NULL_KEY;
    return llGetOwnerKey(id);
}

// A "pos/1|..." line heard on POS_CHANNEL from the attachment id: forwarded for its owner (the
// sim's llGetOwnerKey, never anything in the line) when the viewer listed that avatar, at most
// once per POS_GAP_IN each, within POS_BOUND (+ POS_SLACK) of where the sim has the avatar.
posHeard(key id, string msg)
{
    if (llGetSubString(msg, 0, 5) != "pos/1|" || llStringLength(msg) > 240)
        return;
    key owner = llGetOwnerKey(id);
    if (owner == gOwner)
        return;
    integer at = llListFindList(gSee, [owner]);
    if (at < 0)
        return;
    float now = llGetTime();
    if (now - llList2Float(gSeeT, at) < POS_GAP_IN)
        return;
    owner = attachedOwner(id);
    if (owner == NULL_KEY || owner == gOwner)
        return;
    list f = llParseStringKeepNulls(llGetSubString(msg, 6, -1), ["|"], []);
    if (!posFieldsOk(f))
        return;
    vector p = (vector)llList2String(f, 1);
    list d = llGetObjectDetails(owner, [OBJECT_POS]);
    if (d != [])
        if (llVecDist(p, llList2Vector(d, 0)) > POS_BOUND + POS_SLACK)
            return;
    if (!posAllow(now))
        return;
    gSeeT = llListReplaceList(gSeeT, [now], at, at);
    llOwnerSay("#slb:pos/1|" + (string)owner + "|" + llList2String(f, 0) + "|" + (string)p + "|"
        + (string)((vector)llList2String(f, 2)) + "|" + (string)((rotation)llList2String(f, 3)) + "|" + llList2String(f, 4));
}

integer driven()
{
    return gLastCmd > 0 && llGetUnixTime() - gLastCmd < QUIET_AFTER;
}

// The region combat log (Combat 2.0): only sim events, from COMBAT_LOG_ID. It is busy in
// a fight, so it is heard only while the viewer's kill feed or hit marker asks for it.
combatListen(integer on)
{
    if (on && !gCombatOn)
    {
        gCombatListen = llListen(COMBAT_CHANNEL, "", COMBAT_LOG_ID, "");
        gCombatOn = TRUE;
    }
    else if (!on && gCombatOn)
    {
        llListenRemove(gCombatListen);
        gCombatOn = FALSE;
    }
}

// A key field of a combat event, NULL_KEY when missing or null.
key eventKey(string msg, list at, string field)
{
    key k = (key)llJsonGetValue(msg, at + [field]);
    if (k)
        return k;
    return NULL_KEY;
}

// An object's name; "" for an avatar (OBJECT_NAME would give the avatar's name) or a
// missing object.
string objectName(key id)
{
    if (id)
        if (llGetAgentSize(id) == ZERO_VECTOR)
            return llList2String(llGetObjectDetails(id, [OBJECT_NAME]), 0);
    return "";
}

// One DEATH from the combat log, forwarded as
//   #slb:combat/death/1|<victim>|<killer>|<damage type>|<damage>|<weapon>
// (type and damage empty when unknown; the weapon last, as an object name may hold "|").
// A DEATH carries its type but no damage: the killing blow's DAMAGE normally arrives in the
// same message, and the caller passes its amount (or "").
combatDeath(string msg, list at, string damage)
{
    key victim = eventKey(msg, at, "target");
    if (victim == NULL_KEY)
        return;
    string type = llJsonGetValue(msg, at + ["type"]);
    if (type == JSON_INVALID)
        type = "";

    // A mass kill must not flood the viewer: the feed shows a handful of lines anyway.
    integer now = llGetUnixTime();
    if (now != gDeathSecond)
    {
        gDeathSecond = now;
        gDeathsSent = 0;
    }
    if (++gDeathsSent > MAX_DEATHS)
        return;

    key source = eventKey(msg, at, "source");
    key killer = eventKey(msg, at, "owner");
    if (killer == NULL_KEY)
        killer = source;
    // Name the weapon while it still exists: a projectile usually dies on impact, so
    // fall back to what rezzed it (the gun, normally still worn by the killer).
    string weapon = objectName(source);
    if (weapon == "")
        weapon = objectName(eventKey(msg, at, "rezzer"));
    llOwnerSay("#slb:combat/death/1|" + (string)victim + "|" + (string)killer + "|"
        + llGetSubString(type, 0, 10) + "|" + llGetSubString(damage, 0, 15) + "|"
        + llGetSubString(weapon, 0, 63));
}

// One of the owner's own hits ("D", a DAMAGE; "d" when the same message holds the owner's
// kill of that target, so the viewer plays only the kill sound) or kills ("K", a DEATH),
// forwarded as
//   #slb:combat/hit/1|<D, d or K>|<target>|<1 if the target is an avatar, else 0>|<damage type>|<damage>
// (type and damage empty when unknown). Anything not dealt by the owner, and the owner
// hurting themselves, stays here.
combatHit(string msg, list at, string what)
{
    if (llJsonGetValue(msg, at + ["owner"]) != (string)gOwner)
        return;
    string target = llJsonGetValue(msg, at + ["target"]);
    if (target == JSON_INVALID || target == (string)gOwner)
        return;

    // A full-auto burst or a shotgun must not flood the viewer: it batches hits over 0.3 s
    // anyway. Kills get a little headroom over plain hits.
    integer now = llGetUnixTime();
    if (now != gHitSecond)
    {
        gHitSecond = now;
        gHitsSent = 0;
    }
    // Only lines sent count, so dropped hits never eat the kills' headroom.
    if (gHitsSent >= MAX_HITS + MAX_DEATHS || (gHitsSent >= MAX_HITS && what != "K"))
        return;
    ++gHitsSent;

    string type = llJsonGetValue(msg, at + ["type"]);
    if (type == JSON_INVALID)
        type = "";
    string damage = llJsonGetValue(msg, at + ["damage"]);
    if (damage == JSON_INVALID)
        damage = "";
    llOwnerSay("#slb:combat/hit/1|" + what + "|" + llGetSubString(target, 0, 35) + "|"
        // A DEATH target is always an avatar, even once it has been sent home (the log
        // arrives up to a second late); a hit's target is asked.
        + (string)(what == "K" || llGetAgentSize((key)target) != ZERO_VECTOR) + "|"
        + llGetSubString(type, 0, 10) + "|" + llGetSubString(damage, 0, 15));
}

start()
{
    gOwner = llGetOwner();
    gLastCmd = 0;
    gCmdChannel = 0;
    gPosShare = FALSE;
    gPosSent = FALSE;
    gSee = [];
    gSeeT = [];
    gPosTokens = POS_BURST;
    gPosTokTime = llGetTime();
    gPosListen = llListen(POS_CHANNEL, "", NULL_KEY, "");
    gPublicOn = TRUE;
    publicListens(FALSE);   // until the viewer drives us
    clearTo();   // (left over from a detach in the middle of a ping)
    gSayTokens = SAY_BURST;
    gSayTime = llGetTime();
    // A ping cut short keeps its name: our own again.
    if (shareName(llGetObjectName()))
        llSetObjectName(HELPER_NAME);
    // The main script's channel: it says it when it starts; ask in case it started first.
    llMessageLinked(LINK_THIS, 0, "slms.ch?", NULL_KEY);
}

// The position listen and the quiet timer only run while the viewer drives us.
publicListens(integer on)
{
    if (on == gPublicOn)
        return;
    gPublicOn = on;
    llListenControl(gPosListen, on);
    if (on)
        llSetTimerEvent(30.0);
    else
        llSetTimerEvent(0.0);
}

command(string msg)
{
    gLastCmd = llGetUnixTime();
    publicListens(TRUE);
    // The viewer is back after a quiet spell that dropped the combat listen: hear it again
    // (any command will do, so a lost combat/feed/1|1 or combat/hits/1|1 can't leave either
    // feature dead).
    if (gCombatWanted || gHitsWanted)
        combatListen(TRUE);

    integer bar = llSubStringIndex(msg, "|");
    if (bar < 0)
        return;     // (hello, ping: the main script's)
    string cmd = llGetSubString(msg, 0, bar - 1);
    string arg = llGetSubString(msg, bar + 1, -1);
    if (cmd == "combat/feed/1")
    {
        // Kill feed on (1) or off (0). The viewer sends 1 only with CombatFX on.
        gCombatWanted = (arg == "1");
        combatListen(gCombatWanted || gHitsWanted);
    }
    else if (cmd == "combat/hits/1")
    {
        // Hit marker on (1) or off (0). The viewer sends 1 only with CombatFX on.
        gHitsWanted = (arg == "1");
        combatListen(gCombatWanted || gHitsWanted);
    }
    else if (cmd == "combat/mapto/1")
    {
        mapTo(arg);
    }
    else if (cmd == "combat/mapsay/1")
    {
        mapSay(arg);
    }
    else if (cmd == "pos/send/1")
    {
        posSend(arg);
    }
    else if (cmd == "pos/see/1")
    {
        posSee(arg);
    }
    else if (cmd == "pos/share/1")
    {
        // The viewer's "Share my precise position" option (and its RLV check): 0 stops at once.
        gPosShare = (arg == "1");
        if (!gPosShare)
            gPosSent = FALSE;
    }
}

default
{
    state_entry()
    {
        if (llGetAttached())
            start();
    }

    on_rez(integer param)
    {
        llResetScript();
    }

    link_message(integer sender, integer num, string str, key id)
    {
        // The main script's private channel (it is in this prim: LINK_THIS).
        if (str == "slms.ch" && num != gCmdChannel && llGetAttached())
        {
            if (gCmdChannel != 0)
                llListenRemove(gCmdListen);
            gCmdChannel = num;
            gCmdListen = llListen(gCmdChannel, "", gOwner, "");
        }
    }

    timer()
    {
        if (!driven())
        {
            publicListens(FALSE);
            combatListen(FALSE);    // the busy combat log too, even in a quiet region
            // No viewer: share nothing until one says so again (#1417).
            gPosShare = FALSE;
            gSee = [];
            gSeeT = [];
        }
    }

    attach(key id)
    {
        clearTo();   // (a ping cut short by a detach, or left from before)
        if (shareName(llGetObjectName()))
            llSetObjectName(HELPER_NAME);
    }

    changed(integer change)
    {
        if (change & CHANGED_OWNER)
            llResetScript();
        if (change & (CHANGED_REGION | CHANGED_TELEPORT))
            gPosSent = FALSE;   // the first position in the new region goes out at once
    }

    listen(integer channel, string name, key id, string msg)
    {
        if (channel == gCmdChannel)
        {
            command(msg);
            return;
        }
        if (!driven())
        {
            // The viewer is gone (closed, crashed, another viewer): stop hearing the busy
            // combat log rather than wait to be told.
            if (channel == COMBAT_CHANNEL)
                combatListen(FALSE);
            return;
        }

        if (channel == COMBAT_CHANNEL)
        {
            // Only deaths (kill feed) and events naming the owner (hit marker) can be
            // forwarded: skip everything else before any JSON work, so a busy fight can't
            // crowd the event queue.
            integer deaths = gCombatWanted && llSubStringIndex(msg, "\"DEATH\"") >= 0;
            integer mine = gHitsWanted && llSubStringIndex(msg, (string)gOwner) >= 0;
            if (!deaths && !mine)
                return;
            // Room first: a busy fight's batches are the biggest thing this script handles, and
            // running out of memory here stops the whole helper (Stack-Heap Collision). A batch
            // that does not fit is dropped (the viewer is told once, as for pings).
            if (llGetMemoryLimit() - llGetUsedMemory() < MEM_FLOOR + 2 * llStringLength(msg))
            {
                if (!gMemLow)
                    llOwnerSay("#slb:combat/mem/1|script|" + (string)(llGetMemoryLimit() - llGetUsedMemory()));
                gMemLow = TRUE;
                return;
            }
            gMemLow = FALSE;
            // The sim may batch several events into one JSON array; a killing blow's DAMAGE
            // comes before its DEATH. Each event is read in place by its path ([i] in an array,
            // [] for a single event): no copy of the batch or of its events.
            integer arr = llJsonValueType(msg, []) == JSON_ARRAY;
            integer n = 1;
            if (arr)
            {
                n = 0;
                while (llJsonValueType(msg, [n]) != JSON_INVALID)
                    ++n;
            }
            list blows = [];    // [target, damage] of this message's DAMAGE events, newest first
            integer i;
            list at = [];
            // Targets the owner kills in this message: their hits are part of the kill
            // (a one-shot kill plays the kill sound only).
            list killed = [];
            if (mine && llSubStringIndex(msg, "\"DEATH\"") >= 0)
                for (i = 0; i < n; ++i)
                {
                    if (arr)
                        at = [i];
                    if (llJsonGetValue(msg, at + ["event"]) == "DEATH")
                        if (llJsonGetValue(msg, at + ["owner"]) == (string)gOwner)
                            killed += [llJsonGetValue(msg, at + ["target"])];
                }
            for (i = 0; i < n; ++i)
            {
                if (arr)
                    at = [i];
                string kind = llJsonGetValue(msg, at + ["event"]);
                if (kind == "DAMAGE")
                {
                    if (deaths)
                        blows = [llJsonGetValue(msg, at + ["target"]), llJsonGetValue(msg, at + ["damage"])] + blows;
                    if (mine)
                    {
                        if (llListFindList(killed, [llJsonGetValue(msg, at + ["target"])]) >= 0)
                            combatHit(msg, at, "d");
                        else
                            combatHit(msg, at, "D");
                    }
                }
                else if (kind == "DEATH")
                {
                    if (deaths)
                    {
                        string damage = "";
                        integer b = llListFindList(blows, [llJsonGetValue(msg, at + ["target"])]);
                        if (b >= 0)
                            damage = llList2String(blows, b + 1);
                        if (damage == JSON_INVALID)
                            damage = "";
                        combatDeath(msg, at, damage);
                    }
                    if (mine)
                        combatHit(msg, at, "K");
                }
            }
            return;
        }

        if (channel == POS_CHANNEL)
        {
            posHeard(id, msg);
            return;
        }
    }
}
