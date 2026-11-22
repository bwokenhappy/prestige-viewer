// #Slipstream Helper -- the Slipstream viewer's on-demand bridge (#500).
//
// Slipstream Viewer Source Code, Copyright (C) 2026, Slipstream Viewer contributors.
// LGPL 2.1 (see the viewer's license). Generated into your inventory and uploaded by
// the viewer; edits are overwritten when the viewer updates it (the viewer versions the
// helper by a hash of both its script files, so any change here updates every worn helper).
//
// A small message router between the viewer and the region:
//   viewer -> bridge : chat from the owner on a random private channel (said in hello)
//   bridge -> viewer : llOwnerSay lines prefixed "#slb:", swallowed by the viewer
//   bridge <-> others: the public Slipstream pose and consent channels, and the
//                      consent-confirm channel (avatar-spoken grant statements)
//   script info      : "scripts|<batch>|<key>,<key>,..." from the viewer is answered with
//                      the script counts, memory and time of those objects (#425)
//   combat log, pings and position sync (#1211, #1417) are in the second script,
//                      "#Slipstream Helper Extras" (slipstream_helper_ext.lsl): one Mono script
//                      has 64 KB for code and data together, and with everything in one the
//                      code alone took ~58 KB, so busy regions stopped it with a Stack-Heap
//                      Collision. It hears the viewer on this script's channel (link message).
// Identity always comes from the sim: llGetOwnerKey() of the speaking attachment, or the
// key of the avatar answering a dialog. The viewer never trusts a key in a message body.
// Only well-formed lines of the known kinds are forwarded, within per-speaker and overall
// rate budgets; everything else is dropped here.
//
// Silent unless driven: the helper never speaks first. The viewer asks for hello on a
// discovery channel derived from the owner's key (only the owner's own chat is heard
// there), and public-channel traffic is only forwarded while the viewer pinged within
// the last minute -- so a helper left worn under another viewer says nothing at all.
//
// Dialog answers are ordinary chat from the answering avatar at its position, so they
// are heard only by bridges within chat range (20 m) of that avatar. Slipstream targets
// re-state their consent as chat from the avatar itself (shout range, 100 m) on the
// confirm channel, which no attachment can imitate; bystanders further away than that
// cannot verify the grant and do not show the pose.

integer VERSION = [VERSION];
integer POSE_CHANNEL = [POSE_CHANNEL];
integer CONSENT_CHANNEL = [CONSENT_CHANNEL];
integer CONFIRM_CHANNEL = [CONFIRM_CHANNEL];
integer QUIET_AFTER = 60;       // seconds without a viewer command before we stop forwarding

// Forwarding budgets (token buckets): per speaking owner, and for everything together.
float OWNER_BURST = 20.0;
float OWNER_RATE = 6.0;         // lines a second
float TOTAL_BURST = 40.0;
float TOTAL_RATE = 12.0;
integer MAX_TRACKED = 16;       // owners with a bucket; the least recently heard is dropped

key gOwner;
string gTag;            // first 8 hex digits of the owner's key: the consent button tag
integer gCmdChannel;
integer gHelloChannel;  // discovery: the viewer says "hello" here (same formula in slbridge.cpp)
integer gLastCmd;       // unix time of the last command from the viewer
list gAsked;            // avatars we sent a pose request to, awaiting their answer
list gPosing;           // targets we sent pose data for (stopped on detach)
integer gInfoWindow;    // unix time the current script-info rate window began
integer gInfoBatches;   // script-info batches answered in that window
integer gPoseListen;
integer gConsentListen;
integer gConfirmListen;
integer gPublicOn;      // public listens enabled (only while the viewer drives us)
list gBuckets;          // [owner, tokens, time] per tracked owner, least recently heard first
float gTotalTokens;
float gTotalTime;

integer INFO_MAX_KEYS = 25;     // keys per batch (the viewer sends at most this many)
integer INFO_WINDOW = 10;       // seconds
integer INFO_MAX_BATCHES = 20;  // batches per window; more are answered "busy"

// Where the worn helper sits, relative to its HUD point (#537). HUD space is y = left/right,
// z = up/down, 1 unit = the screen height: the view spans z -0.5..0.5 and y +-aspect/2
// (+-1.78 at 32:9). This is out of view at every aspect ratio, and inside the 3.5 m an
// attachment may sit from its point (viewer and sim clamp further offsets).
vector HIDDEN_POS = <0.0, 2.4, -2.4>;

hide()
{
    llSetLinkPrimitiveParamsFast(LINK_THIS, [PRIM_SIZE, <0.01, 0.01, 0.01>,
        PRIM_COLOR, ALL_SIDES, <1.0, 1.0, 1.0>, 0.0, PRIM_TEXT, "", ZERO_VECTOR, 0.0]);
    // Off screen, so it never sits over the middle of the view (or catches clicks there).
    // Only while worn: on a rezzed object this would move it in the region.
    if (llGetAttached())
        llSetLinkPrimitiveParamsFast(LINK_THIS, [PRIM_POS_LOCAL, HIDDEN_POS]);
}

// hello|<version>|<channel>|<object key>|<used bytes>|<least free bytes since start>
// (the last two only for the viewer's log: the script's memory now and its low-water mark)
hello()
{
    llOwnerSay("#slb:hello|" + (string)VERSION + "|" + (string)gCmdChannel + "|" + (string)llGetKey() + "|"
        + (string)llGetUsedMemory() + "|" + (string)llGetFreeMemory());
}

start()
{
    gOwner = llGetOwner();
    gTag = llGetSubString((string)gOwner, 0, 7);
    gLastCmd = 0;
    // Positive so the viewer can use plain ChatFromViewer (dialog replies cap at 255 bytes).
    gCmdChannel = 100000000 + (integer)llFrand(1000000000.0);
    // 0x40000000 + the first 7 hex digits of the owner's key: positive, and the viewer
    // computes the same number from its own agent id.
    gHelloChannel = 0x40000000 + (integer)("0x" + llGetSubString((string)gOwner, 0, 6));
    llListen(gCmdChannel, "", gOwner, "");
    llListen(gHelloChannel, "", gOwner, "hello");
    gPoseListen = llListen(POSE_CHANNEL, "", NULL_KEY, "");
    gConsentListen = llListen(CONSENT_CHANNEL, "", NULL_KEY, "");
    gConfirmListen = llListen(CONFIRM_CHANNEL, "", NULL_KEY, "");
    gPublicOn = TRUE;
    publicListens(FALSE);   // until the viewer drives us
    gBuckets = [];
    gTotalTokens = TOTAL_BURST;
    gTotalTime = llGetTime();
    // The extras script (combat, pings, position) hears the viewer on the same channel.
    llMessageLinked(LINK_THIS, gCmdChannel, "slms.ch", NULL_KEY);
}

// The public listens only run while the viewer drives us; an idle helper hears nothing.
publicListens(integer on)
{
    if (on == gPublicOn)
        return;
    gPublicOn = on;
    llListenControl(gPoseListen, on);
    llListenControl(gConsentListen, on);
    llListenControl(gConfirmListen, on);
    if (on)
        llSetTimerEvent(30.0);
    else
        llSetTimerEvent(0.0);
}

// One line from owner may be forwarded: both budgets have a token left.
integer allow(key owner)
{
    float now = llGetTime();
    gTotalTokens += (now - gTotalTime) * TOTAL_RATE;
    if (gTotalTokens > TOTAL_BURST)
        gTotalTokens = TOTAL_BURST;
    gTotalTime = now;
    if (gTotalTokens < 1.0)
        return FALSE;

    float tokens = OWNER_BURST;
    integer at = llListFindList(gBuckets, [owner]);
    if (at >= 0)
    {
        tokens = llList2Float(gBuckets, at + 1) + (now - llList2Float(gBuckets, at + 2)) * OWNER_RATE;
        if (tokens > OWNER_BURST)
            tokens = OWNER_BURST;
        gBuckets = llDeleteSubList(gBuckets, at, at + 2);
    }
    integer ok = (tokens >= 1.0);
    if (ok)
    {
        tokens -= 1.0;
        gTotalTokens -= 1.0;
    }
    gBuckets += [owner, tokens, now];
    if (llGetListLength(gBuckets) > MAX_TRACKED * 3)
        gBuckets = llDeleteSubList(gBuckets, 0, 2);
    return ok;
}

// "<prefix><key>" exactly, e.g. "G1|" + a 36-character key.
integer keyLine(string msg, string prefix)
{
    return llStringLength(msg) == 39 && llGetSubString(msg, 0, 2) == prefix;
}

// "<verb> <8-character tag>" exactly: a consent dialog answer.
integer answerLine(string msg, string verb)
{
    integer n = llStringLength(verb);
    return llStringLength(msg) == n + 9 && llGetSubString(msg, 0, n) == verb + " ";
}

// Speaker identity on the public channels: an attachment speaks for its owner; anything
// else (rezzed objects, avatars typing on the channel) is not an attachment.
key attachedOwner(key id)
{
    if (llList2Integer(llGetObjectDetails(id, [OBJECT_ATTACHED_POINT]), 0) == 0)
        return NULL_KEY;
    return llGetOwnerKey(id);
}

// Script counts, memory and time of objects in this region (#425). Commands come only
// from our owner on the private channel, and every field is still checked: the batch
// must be a plain number, keys must be valid 36-character keys, at most INFO_MAX_KEYS
// of them; anything else is skipped. Reply lines stay under ~950 bytes:
//   #slb:scripts|<batch>|<key>,<running>,<total>,<memory bytes>,<time us>|<key>,-|...
// where "<key>,-" means the object is not in this region.
scriptInfo(string arg)
{
    integer bar = llSubStringIndex(arg, "|");
    if (bar < 1 || bar > 9)
        return;
    string batch = llGetSubString(arg, 0, bar - 1);
    if ((string)((integer)batch) != batch)
        return;
    integer now = llGetUnixTime();
    if (now - gInfoWindow >= INFO_WINDOW)
    {
        gInfoWindow = now;
        gInfoBatches = 0;
    }
    if (++gInfoBatches > INFO_MAX_BATCHES)
    {
        llOwnerSay("#slb:scripts|" + batch + "|busy");
        return;
    }
    list ids = llParseString2List(llGetSubString(arg, bar + 1, -1), [","], []);
    integer n = llGetListLength(ids);
    if (n > INFO_MAX_KEYS)
        n = INFO_MAX_KEYS;
    string out = "";
    integer i;
    for (i = 0; i < n; ++i)
    {
        string id = llList2String(ids, i);
        if (llStringLength(id) == 36)
        {
            if ((key)id)
            {
                list d = llGetObjectDetails((key)id, [OBJECT_RUNNING_SCRIPT_COUNT, OBJECT_TOTAL_SCRIPT_COUNT,
                    OBJECT_SCRIPT_MEMORY, OBJECT_SCRIPT_TIME]);
                string e = id + ",-";
                if (llGetListLength(d) == 4)
                    e = id + "," + (string)llList2Integer(d, 0) + "," + (string)llList2Integer(d, 1) + ","
                        + (string)llList2Integer(d, 2) + "," + (string)((integer)(llList2Float(d, 3) * 1000000.0));
                if (llStringLength(out) + llStringLength(e) > 900)
                {
                    llOwnerSay("#slb:scripts|" + batch + out);
                    out = "";
                }
                out += "|" + e;
            }
        }
    }
    if (out != "")
        llOwnerSay("#slb:scripts|" + batch + out);
}

integer driven()
{
    return gLastCmd > 0 && llGetUnixTime() - gLastCmd < QUIET_AFTER;
}

command(string msg)
{
    gLastCmd = llGetUnixTime();
    publicListens(TRUE);

    integer bar = llSubStringIndex(msg, "|");
    string cmd = msg;
    string arg = "";
    if (bar >= 0)
    {
        cmd = llGetSubString(msg, 0, bar - 1);
        arg = llGetSubString(msg, bar + 1, -1);
    }

    if (cmd == "say")
    {
        // Pose data, already encoded and size-capped by the viewer. Remember who we pose
        // (P1|seq|kind|part|target|...) so a detach can stop it for everyone.
        if (llGetSubString(arg, 0, 2) == "P1|")
        {
            // (only the head: the fields read come before the payload, up to ~970 bytes)
            list f = llParseStringKeepNulls(llGetSubString(arg, 0, 99), ["|"], []);
            string target = llList2String(f, 4);
            if (target != "-" && target != "" && llListFindList(gPosing, [target]) < 0)
                gPosing = llList2List(gPosing + [target], -8, -1);
            if (llList2String(f, 2) == "X")
            {
                integer at = llListFindList(gPosing, [target]);
                if (at >= 0)
                    gPosing = llDeleteSubList(gPosing, at, at);
            }
        }
        llRegionSay(POSE_CHANNEL, arg);
    }
    else if (cmd == "scripts")
    {
        scriptInfo(arg);
    }
    else if (cmd == "ping")
    {
        // Keep-alive; nothing to say back.
    }
    else if (cmd == "hello")
    {
        hello();
    }
    else if (cmd == "ask")
    {
        key target = (key)arg;
        if (llGetAgentSize(target) == ZERO_VECTOR)
        {
            llOwnerSay("#slb:err|ask|" + arg);
            return;
        }
        // Announce the request (owner-authenticated) so every Slipstream bridge can tie
        // the target's "Accept <tag>" answer to us.
        llRegionSay(POSE_CHANNEL, "R1|" + arg);
        if (llListFindList(gAsked, [target]) < 0)
            gAsked = llList2List(gAsked + [target], -8, -1);
        llDialog(target, llGetDisplayName(gOwner) + " (" + llKey2Name(gOwner)
            + ") wants to pose your avatar.\n\nThe pose is only shown in Slipstream viewers, and you can stop it at any time.",
            ["Accept " + gTag, "Deny " + gTag], CONSENT_CHANNEL);
    }
    else if (cmd == "end")
    {
        // We stop posing someone: everyone drops our grant for them.
        llRegionSay(POSE_CHANNEL, "E1|" + arg);
        integer at = llListFindList(gPosing, [arg]);
        if (at >= 0)
            gPosing = llDeleteSubList(gPosing, at, at);
    }
    // (combat/..., pos/...: the extras script's commands)
    else if (cmd == "csay")
    {
        // Our owner's own consent statements (grant renewals, revocations), which other
        // bridges attribute to our owner by llGetOwnerKey.
        string kind = llGetSubString(arg, 0, 2);
        if (kind == "G1|" || kind == "S1|")
            llRegionSay(CONSENT_CHANNEL, arg);
    }
}

default
{
    state_entry()
    {
        hide();
        if (llGetAttached())
            start();
    }

    on_rez(integer param)
    {
        llResetScript();
    }

    link_message(integer sender, integer num, string str, key id)
    {
        // The extras script started after us: tell it our channel again.
        if (str == "slms.ch?" && gCmdChannel != 0)
            llMessageLinked(LINK_THIS, gCmdChannel, "slms.ch", NULL_KEY);
    }

    timer()
    {
        if (!driven())
        {
            publicListens(FALSE);
        }
    }

    attach(key id)
    {
        if (id != NULL_KEY)
            hide();
        if (id == NULL_KEY)
        {
            // Taken off: nobody should keep showing poses we were sending.
            llRegionSay(POSE_CHANNEL, "P1|0|X|1/1|-|");
            integer i;
            for (i = 0; i < llGetListLength(gPosing); ++i)
            {
                string target = llList2String(gPosing, i);
                llRegionSay(POSE_CHANNEL, "P1|0|X|1/1|" + target + "|");
                llRegionSay(POSE_CHANNEL, "E1|" + target);
            }
            gPosing = [];
        }
    }

    changed(integer change)
    {
        if (change & CHANGED_OWNER)
            llResetScript();
        if (change & (CHANGED_REGION | CHANGED_TELEPORT))
        {
            gAsked = [];
            gPosing = [];
        }
    }

    listen(integer channel, string name, key id, string msg)
    {
        if (channel == gCmdChannel)
        {
            command(msg);
            return;
        }
        if (channel == gHelloChannel)
        {
            hello();
            return;
        }
        if (!driven())
            return;

        if (channel == POSE_CHANNEL)
        {
            // P1 pose data, Q1 full-pose requests, R1 / E1 consent bookkeeping only.
            integer n = llStringLength(msg);
            string head = llGetSubString(msg, 0, 2);
            if (head == "P1|")
            {
                if (n > 970)
                    return;
            }
            else if (head == "Q1|")
            {
                if (msg != "Q1|*" && n != 39)
                    return;
            }
            else if (head != "R1|" && head != "E1|")
                return;
            else if (n != 39)
                return;
            key owner = llGetOwnerKey(id);
            if (owner == gOwner || !allow(owner))
                return;
            owner = attachedOwner(id);
            if (owner == NULL_KEY || owner == gOwner)
                return;
            llOwnerSay("#slb:pose|" + (string)owner + "|" + msg);
            return;
        }

        if (channel == CONFIRM_CHANNEL)
        {
            // A Slipstream target's own viewer re-stating (G1) or ending (S1) a grant, as
            // chat from the avatar itself: no attachment can say this for it.
            if (id == gOwner || !(keyLine(msg, "G1|") || keyLine(msg, "S1|")))
                return;
            if (llGetAgentSize(id) == ZERO_VECTOR || !allow(id))
                return;
            llOwnerSay("#slb:consent|" + (string)id + "|v|" + (string)id + "|" + msg);
            return;
        }

        if (channel == CONSENT_CHANNEL)
        {
            key who = NULL_KEY;
            string via = "";
            if (llGetAgentSize(id) != ZERO_VECTOR)
            {
                // A dialog answer, said by the avatar itself: only the three answers count.
                if (id != gOwner && (answerLine(msg, "Accept") || answerLine(msg, "Deny") || answerLine(msg, "Stop")))
                {
                    who = id;
                    via = "d";
                }
            }
            else if (keyLine(msg, "G1|") || keyLine(msg, "S1|"))
            {
                // An attachment speaking for its owner: an older Slipstream bridge, or any
                // other attachment. Forwarded with the object, so the viewer can tell (and,
                // for our own owner, report) one that is not a real bridge.
                who = attachedOwner(id);
                via = "a";
                if (who == gOwner && llGetSubString(msg, 0, 2) != "G1|")
                    who = NULL_KEY;
            }
            if (who == NULL_KEY)
                return;
            if (!allow(who))
                return;
            llOwnerSay("#slb:consent|" + (string)who + "|" + via + "|" + (string)id + "|" + msg);

            // Someone we asked said yes: give them a way to stop it from any viewer.
            integer at = llListFindList(gAsked, [who]);
            if (via == "d" && at >= 0 && msg == "Accept " + gTag)
            {
                gAsked = llDeleteSubList(gAsked, at, at);
                llDialog(who, llGetDisplayName(gOwner) + " is posing you. Press Stop at any time to end it.",
                    ["Stop " + gTag], CONSENT_CHANNEL);
            }
            else if (via == "d" && at >= 0 && msg == "Deny " + gTag)
            {
                gAsked = llDeleteSubList(gAsked, at, at);
            }
        }
    }
}
