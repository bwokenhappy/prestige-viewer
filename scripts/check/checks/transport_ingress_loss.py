"""Regression guard for SILENT UDP LOSS in the experimental network transport (2026-11-11).

The owner (2026-11-11, build fabdd0df3b): on login, pieces of the house and its lights never appeared; walking or
moving the camera did not bring them back and they could not be selected; a teleport away and back restored them. The
transport's receive pump read each datagram off the socket FIRST and only then tried to publish it to main's ingress
queue (4096 cells) and arena; when main had fallen behind (the ~1 s first-frame hitch, the login object burst) the
publish failed and the datagram was discarded -- uncounted (the counters were test-only) and unlogged. An unreliable
ObjectUpdateCached / ObjectUpdate lost there is an object that never loads, and the sim never resends it.

The fix, guarded here:
  (a) DON'T READ WHAT CAN'T BE KEPT: LLTransportReceivePump::pollOne checks for room (one queue cell + a
      maximum-size arena record) BEFORE receivePacketOnce. While main's ingress is full, application messages go to
      the owner's OVERFLOW (same arena / queue types, owner-private, published and ACKed, handed to main in order
      every owner turn), so the owner keeps reading: pings answered, ACKs consumed (a first cut that simply stopped
      reading made reliable sends time out and pings go unanswered through a main stall -- the worker UDP test).
      Only when the overflow is full too does it return Backlogged: the datagram waits in the kernel buffer (grown
      at transport start, the granted size logged); the owner loop treats that as blocked (a short retry, or main's
      wake when it releases an ingress slot).
  (b) NO SILENT DROP: every remaining path in processParsed that consumes a datagram and refuses it for capacity
      returns through dropFull() (counted reliable / unreliable and per message, published in LLTransportRingStats,
      a rate-limited Messaging warning, the exit summary's "Dropped packets"). The only bare DataFull returns allowed
      are the resent-duplicate re-ACK sites (data delivered earlier), marked "not a drop".
  (c) OUTBOUND: a send the owner could not admit (all 256 credits of its kind in flight) used to return 0 with no
      trace (a reliable RequestMultipleObjects that never left = an object that never loads); it now waits in order in
      mTransportDeferredSends (bounded) and leaves when credits come back.
"""
import re

NAME = "transport-ingress-loss"
STAGE = "source"
DESCRIPTION = "the network transport never discards a datagram silently: it checks ingress room before reading, " \
              "counts every capacity drop, and defers outbound sends it cannot admit"
GUARDS = ("Objects missing after login until a re-teleport: the transport's receive pump discarded UDP datagrams it "
          "had read when main's ingress queue was full, with no counter or log.")

PUMP = "indra/llmessage/lltransportreceivepump.h"
LOOP = "indra/llmessage/lltransportownerloop.cpp"
GRAPH = "indra/llmessage/lltransportownergraph.cpp"
MSG = "indra/llmessage/message.cpp"


def _body(src, signature):
    i = src.find(signature)
    if i < 0:
        return None
    j = src.find("{", i)
    depth = 0
    for k in range(j, len(src)):
        depth += {"{": 1, "}": -1}.get(src[k], 0)
        if depth == 0:
            return src[i:k + 1]
    return None


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    missing = []
    pump = read(PUMP)
    loop = read(LOOP)

    # (a) room checked before the read
    poll = _body(pump, "LLTransportRxStatus pollOne(") or ""
    i_room = poll.find("if (!ingressHasRoomOnOwner())")
    i_read = poll.find("receivePacketOnce(")
    if i_room < 0 or i_read < 0 or i_room > i_read or "return LLTransportRxStatus::Backlogged;" not in poll:
        missing.append(PUMP + " pollOne: ingress room is no longer checked BEFORE receivePacketOnce (a datagram read "
                       "with nowhere to put it is discarded) -- (a)")
    room = _body(pump, "bool ingressHasRoomOnOwner()") or ""
    main_room = _body(pump, "bool mainHasRoomOnOwner()") or ""
    over_room = _body(pump, "bool overflowHasRoomOnOwner()") or ""
    if "mainHasRoomOnOwner()" not in room or "overflowHasRoomOnOwner()" not in room \
            or "overflowEmptyOnOwner()" not in room:
        missing.append(PUMP + " ingressHasRoomOnOwner: must accept a read only when main's ingress (with the overflow "
                       "empty) or the overflow has room -- (a)")
    for name, body in (("mainHasRoomOnOwner", main_room), ("overflowHasRoomOnOwner", over_room)):
        if "producerHasRoom()" not in body or "producerHasRoomForMaximumRecord()" not in body:
            missing.append(PUMP + " " + name + ": must test both the event queue and the arena -- (a)")
    # the overflow keeps the owner reading (pings, ACKs) while main is behind, and drains to main in order
    if "mOverflowArena, mOverflowEvents, event)" not in (_body(pump, "LLTransportRxStatus processParsed(") or ""):
        missing.append(PUMP + " processParsed: application messages no longer go to the owner's overflow while main "
                       "is full (the owner stops reading: unanswered pings, unheard ACKs, failed reliable sends) -- (a)")
    if "mReceive.drainOverflowOnOwner(moved)" not in loop:
        missing.append(LOOP + " driveOnce: the overflow is not handed to main every turn -- (a)")
    if "const bool backlogged=result==LLTransportRxStatus::Backlogged;" not in loop \
            or "!retryDelayed && !backlogged" not in loop or "kIngressBacklogRetryUsec" not in loop:
        missing.append(LOOP + " driveOnce: Backlogged is not treated as blocked with a retry delay (the owner would "
                       "spin on the readable socket) -- (a)")
    graph = read(GRAPH)
    if "takeBacklogWakeOnMain()" not in (_body(graph, "bool LLTransportOwnerFacade::releaseInbound()") or ""):
        missing.append(GRAPH + " releaseInbound: main no longer wakes a backlogged owner when it frees a slot -- (a)")
    msg = read(MSG)
    if "net_grow_receive_buffer(mSocket" not in msg or "kernel granted" not in msg:
        missing.append(MSG + ": the transport no longer grows the UDP receive buffer / logs the granted size -- (a)")

    # (b) every capacity refusal of a consumed datagram is counted
    parsed = _body(pump, "LLTransportRxStatus processParsed(") or ""
    if not parsed:
        missing.append(PUMP + ": processParsed not found (renamed?): the drop audit covers nothing")
    for m in re.finditer(r"LLTransportRxStatus::DataFull", parsed):
        line_start = parsed.rfind("\n", 0, m.start()) + 1
        context = parsed[max(0, line_start - 300):m.end()]
        if "not a drop" not in context:
            missing.append(PUMP + " processParsed: a bare `LLTransportRxStatus::DataFull` that is not a resent "
                           "duplicate's re-ACK -- a consumed datagram refused without dropFull() is a SILENT drop: "
                           + parsed[line_start:m.end()].strip() + " -- (b)")
    drop = _body(pump, "LLTransportRxStatus dropFull(") or ""
    if "mIngressDroppedReliable" not in drop or "mIngressDroppedUnreliable" not in drop \
            or "mIngressDropsReliableBy" not in drop:
        missing.append(PUMP + " dropFull: no longer counts reliable / unreliable / per message -- (b)")
    if "value.ingressDroppedUnreliable=" not in graph:
        missing.append(GRAPH + " ringStats: the ingress drop counters are not published -- (b)")
    rep = _body(msg, "void LLMessageSystem::reportTransportIngressLossOnMain(") or ""
    if "LL_WARNS(\"Messaging\")" not in rep or "ingressDropsAt(" not in rep:
        missing.append(MSG + " reportTransportIngressLossOnMain: no per-message Messaging warning -- (b)")
    if "reportTransportIngressLossOnMain(false)" not in msg or "reportTransportIngressLossOnMain(true)" not in msg:
        missing.append(MSG + ": the ingress loss report is not called (periodically and at detach) -- (b)")
    summ = _body(msg, "void LLMessageSystem::summarizeLogs(") or ""
    if "mTransportIngressSeen.droppedUnreliable" not in summ:
        missing.append(MSG + " summarizeLogs: the exit summary's Dropped packets no longer include transport drops -- (b)")

    # (c) outbound sends the owner cannot admit wait instead of vanishing
    send = _body(msg, "S32 LLMessageSystem::sendTransportTemplateOnMain(") or ""
    if "mTransportDeferredSends.push_back(" not in send or "TransportOfferResult::Full" not in \
            (_body(msg, "LLMessageSystem::TransportOfferResult LLMessageSystem::offerTransportSendOnMain(") or ""):
        missing.append(MSG + " sendTransportTemplateOnMain: a send refused for owner credit is no longer deferred -- (c)")
    if "pumpTransportDeferredSendsOnMain(" not in (_body(msg, "uint32_t LLMessageSystem::pumpTransportCompletions(") or ""):
        missing.append(MSG + " pumpTransportCompletions: deferred sends are not resubmitted as credits return -- (c)")
    if "cancelTransportDeferredSendsOnMain();" not in (_body(msg, "bool LLMessageSystem::requestTransportOwnerStopOnMain(") or ""):
        missing.append(MSG + " requestTransportOwnerStopOnMain: deferred sends keep their callback reservations past "
                       "the stop (the shutdown drain would never finish) -- (c)")

    if missing:
        result.fail("\n".join(missing))
        return
    result.summary = "room checked before the read; every capacity drop counted; outbound sends deferred"
