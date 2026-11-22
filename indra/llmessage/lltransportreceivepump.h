/** Disabled owner RX integration prototype. SPDX-License-Identifier: LGPL-2.1-only */
#ifndef LL_LLTRANSPORTRECEIVEPUMP_H
#define LL_LLTRANSPORTRECEIVEPUMP_H

#include "llpacketring.h"
#include "lltransportcircuitstate.h"
#include "lltransportcontrol.h"
#include "lltransportdecoder.h"
#include "lltransportenvelope.h"
#include "lltransportingress.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

// Main freezes this sorted table and its schemas before the worker starts and
// retains them until every event is drained. Handler registrations remain main.
struct LLTransportSchemaEntry
{
    uint32_t messageNumber = 0;
    const LLTransportSchema* schema = nullptr;
    bool requiresTrusted = false;
    bool internalControl = false;
};

enum class LLTransportPendingKind : uint8_t { InternalControl, BusyAppendedAck };
enum class LLTransportControlApply : uint8_t { Ready, Applied, NeedReplyCapacity, NeedDiagnostics, Deferred, Backpressure, Rejected, Poisoned };

enum class LLTransportRxStatus : uint8_t
{
    Idle, ConsumedDiscarded, Interrupted, SocketError, Published, Duplicate, DataFull, Rejected, ControlPending, Poisoned,
    // Neither main's ingress nor the owner's overflow had room for one more message, so
    // nothing was read: the datagram stays in the kernel socket buffer until main drains.
    // Unlike DataFull, nothing is lost.
    Backlogged
};

// Socket owner only. This is a source-integration prototype, NOT an enabled
// viewer path: control servicing, off-circuit UseCircuitCode, the validated
// arrival observation API, timer/fairness scheduling and main reader delivery
// are not implemented here. Do not install it until those paths are complete.
// The containing owner allocates this object and all storage under its ledger.
class LLTransportReceivePump
{
public:
    struct AffectedCircuit
    {
        LLTransportEndpoint endpoint{};
        uint64_t incarnation = 0;
        bool valid = false;
    };

    LLTransportReceivePump(LLPacketRing& ring, LLTransportCircuitState& circuits,
        LLTransportDecodedArena& arena, LLTransportIngressQueue& events,
        std::atomic<bool>& poisoned, const LLTransportSchemaEntry* schemas, std::size_t schemaCount) noexcept
        : mRing(ring), mCircuits(circuits), mArena(arena), mEvents(events), mPoisoned(poisoned),
          mSchemas(schemas), mSchemaCount(schemaCount) {}

    LLTransportRxStatus pollOne(int socket, const LLPacketRing::UDPProxySnapshot& proxy,
                                uint64_t loginGeneration, LLTransportSteadyUsec steadyNow) noexcept
    {
        mAffectedCircuit = {};
        mLastReceiveNativeError = 0;
        // In particular, never call reserveReceive for duplicates after poison:
        // it can add an ACK before any data-slot reservation.
        if (mPoisoned.load()) return LLTransportRxStatus::Poisoned;
        if (mControlPending) return LLTransportRxStatus::ControlPending;
        // Don't read what can't be kept: a datagram read off the socket and then refused
        // by a full ingress queue / arena is gone (an unreliable ObjectUpdateCached or
        // ObjectUpdate is never resent -- objects missing until a re-teleport). While
        // main is behind, application messages go to the owner's OVERFLOW (published,
        // ACKed, moved to main in order as it drains) so the owner keeps reading --
        // pings answered, ACKs consumed. Only when the overflow is full too is nothing
        // read: the datagram waits in the kernel buffer (Backlogged), still not lost.
        if (!ingressHasRoomOnOwner())
        {
            if (!mBacklogged)
            {
                mBacklogged = true;
                incrementSaturating(mIngressBacklogEpisodes);
            }
            mBacklogWakeWanted.store(true, std::memory_order_release);
            noteMainFullForTest();
            return LLTransportRxStatus::Backlogged;
        }
        mBacklogged = false;
        mControlTrailerEffects = false;
        const auto received = mRing.receivePacketOnce(socket, reinterpret_cast<char*>(mWire.data()), proxy);
        mLastReceiveNativeError = received.nativeError;
        using RingStatus = LLPacketRing::OwnerReceiveStatus;
        if (received.status == RingStatus::WouldBlock) return LLTransportRxStatus::Idle;
        if (received.status == RingStatus::ConsumedDiscarded) return LLTransportRxStatus::ConsumedDiscarded;
        if (received.status == RingStatus::Interrupted) return LLTransportRxStatus::Interrupted;
        if (received.status != RingStatus::Delivered) return LLTransportRxStatus::SocketError;
        const S32 size = received.bytes;
        if (size <= 0 || size > NET_BUFFER_SIZE) return LLTransportRxStatus::SocketError;
        const LLHost senderHost = mRing.getLastSender();
        const LLHost interfaceHost = mRing.getLastReceivingInterface();
        if (senderHost.getPort() > 65535u || interfaceHost.getPort() > 65535u)
            return LLTransportRxStatus::Rejected;
        const LLTransportEndpoint sender{senderHost.getAddress(), static_cast<uint16_t>(senderHost.getPort())};
        const LLTransportEndpoint receiving{interfaceHost.getAddress(), static_cast<uint16_t>(interfaceHost.getPort())};
        const auto envelope = llTransportParseEnvelope(mWire.data(), static_cast<std::size_t>(size),
                                                        mExpanded.data(), mExpanded.size());
        if (envelope.status != LLTransportEnvelope::Status::Complete) return LLTransportRxStatus::Rejected;
        return processParsed(envelope, sender, receiving, static_cast<uint32_t>(size),
                             loginGeneration, steadyNow, 0, 0);
    }

    // Owner only: one more message of any admissible size can be kept now -- in main's
    // ingress (when nothing waits in the overflow, which must go first) or the overflow.
    bool ingressHasRoomOnOwner() noexcept
    { return (overflowEmptyOnOwner() && mainHasRoomOnOwner()) || overflowHasRoomOnOwner(); }

    // Owner only, every turn: move overflowed events to main's ingress, oldest first,
    // while main has room. False: an ownership invariant broke (the pump is poisoned).
    bool drainOverflowOnOwner(uint32_t& moved) noexcept
    {
        moved = 0;
        while (!overflowEmptyOnOwner() && mainHasRoomOnOwner())
        {
            const LLTransportIngressEvent* source = mOverflowEvents.peekConsumer();
            if (!source) break;
            LLTransportDecodedView view;
            if (!mOverflowArena.view(source->arenaRecord, view)) { mPoisoned.store(true); return false; }
            LLTransportIngressEvent* slot = mEvents.reserveProducer();
            if (!slot) break;
            LLTransportDecodedArena::Record record;
            const auto copied = mArena.tryCopy(view, record);
            if (copied != LLTransportDecodedArena::Result::Ready)
            {
                mEvents.cancelProducer();
                if (copied == LLTransportDecodedArena::Result::Full || copied == LLTransportDecodedArena::Result::Busy) break;
                mPoisoned.store(true); return false;
            }
            // The circuit side (ACK, dedup, APP sequence) was committed when it entered the
            // overflow; this is only the hand-over of an already accepted message.
            *slot = *source;
            slot->arenaRecord = record;
            if (!mArena.commit(record) || !mEvents.publishProducer()) { mPoisoned.store(true); return false; }
            if (!mOverflowArena.release(source->arenaRecord) || !mOverflowEvents.releaseConsumer())
            { mPoisoned.store(true); return false; }
            mOverflowPending.store(mOverflowPending.load(std::memory_order_relaxed) - 1, std::memory_order_relaxed);
            ++moved;
        }
        if (!overflowEmptyOnOwner()) mBacklogWakeWanted.store(true, std::memory_order_release);
        return true;
    }

    // Owner only, after deferring/resolving the outstanding action. The same
    // packet and its unread ACK index remain held while Busy. Main login and
    // circuit incarnation are revalidated; stale packets never affect a new
    // circuit. Do not service another packet on this circuit while it is held.
    LLTransportRxStatus resumeBusyAcknowledgements(uint64_t currentLoginGeneration) noexcept
    {
        mAffectedCircuit = {};
        if (mPoisoned.load()) return LLTransportRxStatus::Poisoned;
        if (!mControlPending || mPendingKind != LLTransportPendingKind::BusyAppendedAck)
            return LLTransportRxStatus::Rejected;
        if (currentLoginGeneration != mControlLoginGeneration)
        {
            mControlPending = false; mControlApplied = false;
            return LLTransportRxStatus::Rejected;
        }
        const auto envelope = mControlEnvelope;
        const auto sender = mControlSender;
        const auto receiving = mControlReceiving;
        const auto size = mControlWireSize;
        const auto incarnation = mControlIncarnation;
        const auto index = mPendingAckIndex;
        const LLTransportSteadyUsec received(mControlReceivedUsec);
        mControlPending = false;
        return processParsed(envelope, sender, receiving, size, currentLoginGeneration,
                             received, incarnation, index);
    }

private:
    LLTransportRxStatus processParsed(const LLTransportEnvelope& envelope, LLTransportEndpoint sender,
        LLTransportEndpoint receiving, uint32_t size, uint64_t loginGeneration,
        LLTransportSteadyUsec steadyNow, uint64_t expectedIncarnation, uint16_t firstAck) noexcept
    {
        const auto circuit = mCircuits.lookup(sender, steadyNow.value);
        // First viewer A/B supports expected peers enabled before their
        // outgoing UseCircuitCode. Generic inbound off-circuit handshakes are
        // not implemented; unknown senders are rejected and not ACKed.
        if (circuit.state != LLTransportSnapshotState::Present ||
            (expectedIncarnation && circuit.incarnation != expectedIncarnation)) return LLTransportRxStatus::Rejected;
        // Reliable control capacity is checked before ANY appended-ACK effect.
        // The sole owner holds this packet until reservation; no other receive
        // can consume the preflighted receive/dedup/ACK slots in between.
        const auto* preflightEntry=findSchema(envelope.messageNumber);
        const bool guardedControl=envelope.reliable && preflightEntry && preflightEntry->internalControl && preflightEntry->schema &&
            preflightEntry->schema->isFrozen() && preflightEntry->schema->messageNumber()==envelope.messageNumber &&
            (!preflightEntry->requiresTrusted || circuit.trusted);
        if (guardedControl)
        {
            const auto available=mCircuits.preflightReliableControl(sender,circuit.incarnation,loginGeneration,
                                                                     envelope.packetId,envelope.resent);
            if (available==LLTransportCircuitState::ReceiveStatus::Full ||
                available==LLTransportCircuitState::ReceiveStatus::DuplicateFull)
            {
                if (mControlTrailerEffects) { mPoisoned.store(true); return LLTransportRxStatus::Poisoned; }
                if (available==LLTransportCircuitState::ReceiveStatus::DuplicateFull)
                { incrementSaturating(mControlDuplicateAckBackpressure); }
                else noteControlBackpressure();
                return dropFull(envelope);
            }
            if (available==LLTransportCircuitState::ReceiveStatus::Invalid) return LLTransportRxStatus::Rejected;
        }
        // A later failure can follow consumed ACKs or an arrival update.
        // Report conservatively before the first possible state mutation.
        mAffectedCircuit = {sender, circuit.incarnation, true};
        // Legacy applies appended ACKs to the matching circuit before schema
        // validation. State completion capacity must already be reserved by
        // its outgoing admission; failure is terminal, never silently ignored.
        for (uint16_t i = firstAck; i < envelope.ackCount; ++i)
        {
            uint32_t id = 0;
            if (!llTransportAppendedAck(mWire.data(), static_cast<std::size_t>(size), envelope, static_cast<uint8_t>(i), id))
            { mPoisoned.store(true); return LLTransportRxStatus::Poisoned; }
            const auto result = mCircuits.consumeAck(sender, circuit.incarnation, id);
            if (result==LLTransportCircuitState::Status::Ok) mControlTrailerEffects=true;
            if (result == LLTransportCircuitState::Status::Busy)
            {
                // Keep the original bytes and next unconsumed ACK index.
                // Never replay an earlier ACK against a later reused ID.
                mControlEnvelope = envelope; mControlSender = sender;
                mControlIncarnation = circuit.incarnation;
                mControlReceiving = receiving; mControlWireSize = static_cast<uint32_t>(size);
                mControlLoginGeneration = loginGeneration; mControlReceivedUsec = steadyNow.value;
                mPendingAckIndex = i;
                mPendingKind = LLTransportPendingKind::BusyAppendedAck;
                mControlPending = true;
                return LLTransportRxStatus::ControlPending;
            }
            // Unknown/out-of-range IDs have no matching legacy reliable
            // packet. A bad remote ACK must not poison the entire owner.
            if (result != LLTransportCircuitState::Status::Ok &&
                result != LLTransportCircuitState::Status::Absent &&
                result != LLTransportCircuitState::Status::Invalid)
                return LLTransportRxStatus::Rejected;
        }
        // Legacy discards committed resent IDs before template/trust lookup.
        const auto classification = mCircuits.classifyResentDuplicate(
            sender, circuit.incarnation, loginGeneration, envelope.packetId, envelope.resent);
        if (classification == LLTransportCircuitState::DuplicateClass::Duplicate)
        {
            if (!envelope.reliable) return LLTransportRxStatus::Duplicate;
            if (mPoisoned.load()) return LLTransportRxStatus::Poisoned;
            const auto ack = mCircuits.acknowledgeCommittedDuplicate(
                sender, circuit.incarnation, loginGeneration, envelope.packetId, steadyNow.value);
            if (ack == LLTransportCircuitState::ReceiveStatus::Duplicate) return LLTransportRxStatus::Duplicate;
            if (guardedControl && mControlTrailerEffects) { mPoisoned.store(true); return LLTransportRxStatus::Poisoned; }
            // (a resent duplicate: its data was delivered; only the re-ACK waits -- not a drop)
            return ack == LLTransportCircuitState::ReceiveStatus::Full
                ? LLTransportRxStatus::DataFull : LLTransportRxStatus::Rejected;
        }
        if (classification == LLTransportCircuitState::DuplicateClass::Busy)
        {
            if (guardedControl && mControlTrailerEffects) { mPoisoned.store(true); return LLTransportRxStatus::Poisoned; }
            return dropFull(envelope);
        }
        if (classification != LLTransportCircuitState::DuplicateClass::NotDuplicate)
        {
            if (guardedControl && mControlTrailerEffects) { mPoisoned.store(true); return LLTransportRxStatus::Poisoned; }
            return LLTransportRxStatus::Rejected;
        }
        const LLTransportSchemaEntry* entry = findSchema(envelope.messageNumber);
        if (!entry || !entry->schema || !entry->schema->isFrozen() ||
            entry->schema->messageNumber() != envelope.messageNumber ||
            (entry->requiresTrusted && !circuit.trusted)) return LLTransportRxStatus::Rejected;
        // Record validated nonduplicate traffic before attempting data
        // retention: even a full APP queue must refresh activity, while never
        // ACKing or retaining a fresh packet until admission succeeds.
        const auto observed = mCircuits.observeValidatedArrival(sender, circuit.incarnation, loginGeneration,
            envelope.packetId, envelope.resent, static_cast<uint32_t>(size), steadyNow.value);
        if (observed.status == LLTransportCircuitState::ArrivalStatus::Duplicate)
        {
            if (!envelope.reliable) return LLTransportRxStatus::Duplicate;
            if (mPoisoned.load()) return LLTransportRxStatus::Poisoned;
            const auto duplicate = mCircuits.acknowledgeCommittedDuplicate(
                sender, circuit.incarnation, loginGeneration, envelope.packetId, steadyNow.value);
            if (duplicate == LLTransportCircuitState::ReceiveStatus::Duplicate) return LLTransportRxStatus::Duplicate;
            // (delivered before; only the re-ACK waits -- not a drop)
            return duplicate == LLTransportCircuitState::ReceiveStatus::Full
                ? LLTransportRxStatus::DataFull : LLTransportRxStatus::Rejected;
        }
        if ((observed.status != LLTransportCircuitState::ArrivalStatus::Observed &&
             observed.status != LLTransportCircuitState::ArrivalStatus::PartialLoss) || !observed.observation)
        {
            if (guardedControl && mControlTrailerEffects) { mPoisoned.store(true); return LLTransportRxStatus::Poisoned; }
            return observed.status == LLTransportCircuitState::ArrivalStatus::Busy
                ? dropFull(envelope) : LLTransportRxStatus::Rejected;
        }
        if (entry->internalControl)
        {
            // One owner-held control datagram, not an application queue slot.
            // The owner must consume it before the next poll; no viewer pointer
            // or handler is called here. No ACK for the new message is implied.
            mPendingKind = LLTransportPendingKind::InternalControl;
            mControlEnvelope = envelope;
            mControlSender = sender;
            mControlIncarnation = circuit.incarnation;
            mControlLoginGeneration = loginGeneration; mControlObservation = observed.observation;
            mControlReceiving = receiving; mControlWireSize = static_cast<uint32_t>(size);
            mControlReceivedUsec = steadyNow.value;
            mControlAckIndex = 0; mControlReservation = 0; mControlRequiresReply = false;
            mControlWarningRetained = false;
            mControlApplied = false;
            mControlPending = true;
            return LLTransportRxStatus::ControlPending;
        }
        if (mNextAppSequence == 0) return dropFull(envelope);
        const auto receive = mCircuits.reserveObservedReceive(sender, circuit.incarnation, loginGeneration,
            envelope.packetId, envelope.resent, envelope.reliable, steadyNow.value, observed.observation);
        if (receive.status == LLTransportCircuitState::ReceiveStatus::Full) return dropFull(envelope);
        if (receive.status != LLTransportCircuitState::ReceiveStatus::New || !receive.reservation)
            return LLTransportRxStatus::Rejected;
        const auto decoded = decodeTransportFields(*entry->schema, mExpanded.data(), envelope.expandedSize,
                                                   envelope.bodyOffset, mScratch);
        if (decoded.status != LLTransportDecodeStatus::Complete)
        {
            mCircuits.cancelReceive(receive.reservation);
            return LLTransportRxStatus::Rejected;
        }
        LLTransportIngressEvent event;
        event.sender = sender; event.receivingInterface = receiving;
        event.circuitIncarnation = circuit.incarnation; event.appSequence = mNextAppSequence;
        event.schemaGeneration = entry->schema->generation(); event.packetId = envelope.packetId;
        event.messageNumber = envelope.messageNumber; event.wireBytes = static_cast<uint32_t>(size);
        event.expandedBytes = static_cast<uint32_t>(envelope.expandedSize);
        event.compressedBytes = envelope.compressed ? static_cast<uint32_t>(envelope.wireMessageSize) : 0;
        event.reliable = envelope.reliable;
        event.firstTruncationOffset = decoded.firstTruncationOffset;
        event.firstTruncationWanted = decoded.firstTruncationWanted; event.truncations = decoded.truncations;
        // Main's ingress if it has room and nothing older waits in the overflow (FIFO),
        // else the overflow (pollOne checked that one of them has room before reading).
        const bool via_overflow = !overflowEmptyOnOwner() || !mainHasRoomOnOwner();
        LLTransportPublishStatus result = LLTransportPublishStatus::Backpressure;
        if (!via_overflow)
            result = llTransportPublishDecoded(mCircuits, receive.reservation, steadyNow,
                entry->schema->generation(), mPoisoned, mScratch, mArena, mEvents, event);
        else if (overflowHasRoomOnOwner())
            result = llTransportPublishDecoded(mCircuits, receive.reservation, steadyNow,
                entry->schema->generation(), mPoisoned, mScratch, mOverflowArena, mOverflowEvents, event);
        else
            mCircuits.cancelReceive(receive.reservation);
        if (result == LLTransportPublishStatus::Published)
        {
            mLastPublishedAppSequence = mNextAppSequence;
            mNextAppSequence = mNextAppSequence == (std::numeric_limits<uint64_t>::max)() ? 0 : mNextAppSequence + 1;
            if (via_overflow)
            {
                mOverflowPending.store(mOverflowPending.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                incrementSaturating(mIngressOverflowEvents);
                mBacklogWakeWanted.store(true, std::memory_order_release);
                noteMainFullForTest();
            }
            return LLTransportRxStatus::Published;
        }
        if (result == LLTransportPublishStatus::InvariantFailure || result == LLTransportPublishStatus::Poisoned)
            return LLTransportRxStatus::Poisoned;
        // (pollOne checked for room before reading, so this is now only a race with an
        // invariant elsewhere -- but if it ever happens it is counted, not silent)
        return result == LLTransportPublishStatus::Backpressure ? dropFull(envelope) : LLTransportRxStatus::Rejected;
    }

public:

    // Owner-only: decode an already held internal-control packet without
    // dispatching a handler. Nonzero truncation diagnostics must be routed
    // according to the main exception ordering contract before treating the
    // message as accepted. The caller retains this packet until response
    // capacity and circuit identity are revalidated.
    bool decodePendingControl(LLTransportControlValues& values, LLTransportDecodeResult& diagnostic) noexcept
    {
        if (mPoisoned.load() || !mControlPending || mPendingKind != LLTransportPendingKind::InternalControl)
            return false;
        const auto* entry = findSchema(mControlEnvelope.messageNumber);
        if (!entry || !entry->schema || !entry->schema->isFrozen() ||
            entry->schema->messageNumber() != mControlEnvelope.messageNumber) return false;
        const auto result = decodeTransportFields(*entry->schema, mExpanded.data(), mControlEnvelope.expandedSize,
                                                   mControlEnvelope.bodyOffset, mScratch);
        if (result.status != LLTransportDecodeStatus::Complete) return false;
        LLTransportControlValues decoded;
        if (!llTransportDecodeControl(mControlEnvelope.messageNumber, mScratch.view(), decoded)) return false;
        diagnostic = result; values = decoded;
        return true;
    }

    // Owner folds this value into independent bounded warning statistics.
    // No viewer callback, logging or template pointer crosses the boundary.
    bool pendingControlWarning(LLTransportControlWarning& out) noexcept
    {
        LLTransportControlValues values; LLTransportDecodeResult diagnostic;
        if (!decodePendingControl(values,diagnostic) || !diagnostic.truncations || !mControlObservation) return false;
        LLTransportControlWarning warning;
        warning.endpoint=mControlSender; warning.loginGeneration=mControlLoginGeneration;
        warning.incarnation=mControlIncarnation; warning.observation=mControlObservation;
        warning.firstOffset=diagnostic.firstTruncationOffset; warning.firstWanted=diagnostic.firstTruncationWanted;
        warning.messageNumber=mControlEnvelope.messageNumber; warning.packetId=mControlEnvelope.packetId;
        warning.truncations=diagnostic.truncations; out=warning; return true;
    }
    bool controlWarningRetainedOnOwner() const noexcept { return mControlWarningRetained; }
    // Owner shutdown only, after RX has stopped. This is deliberately usable
    // after poison and is idempotent; it never commits an ACK or replays any
    // already applied trailer. The pump must not be polled after shutdown.
    void cancelHeldForStop() noexcept
    {
        if (mControlReservation) mCircuits.cancelReceive(mControlReservation);
        mControlReservation=0; mControlPending=false; mControlApplied=false;
        mControlRequiresReply=false; mControlWarningRetained=false;
        mControlAckIndex=0; mPendingAckIndex=0; mControlTrailerEffects=false;
        mControlObservation=0; mAffectedCircuit={};
    }
    // Only after that exact warning has been accounted in the coalesced
    // publication, and only under the frozen no-callback policy. Failure
    // preserves the held input.
    bool markControlWarningRetainedOnOwner(const LLTransportControlWarning& warning) noexcept
    {
        LLTransportControlWarning current;
        if (mControlApplied || mControlWarningRetained || !pendingControlWarning(current) ||
            current.endpoint.addressNetworkOrder!=warning.endpoint.addressNetworkOrder ||
            current.endpoint.portHostOrder!=warning.endpoint.portHostOrder || current.loginGeneration!=warning.loginGeneration ||
            current.incarnation!=warning.incarnation || current.observation!=warning.observation ||
            current.messageNumber!=warning.messageNumber || current.packetId!=warning.packetId ||
            current.firstOffset!=warning.firstOffset || current.firstWanted!=warning.firstWanted ||
            current.truncations!=warning.truncations) return false;
        mControlWarningRetained=true; return true;
    }

    // Reserve before effects. replyReserved means a bounded StartPing reply
    // slot is already retained. The legacy diagnosticsHandled boolean is not
    // accepted as event identity; a truncated control requires its exact
    // warning to have been published under the frozen no-callback policy.
    // A reservation stays attached to the held event across Deferred retries.
    LLTransportControlApply preparePendingControl(LLTransportSteadyUsec steadyNow,
                                                   bool replyReserved, bool diagnosticsHandled) noexcept
    {
        (void)diagnosticsHandled; // a global boolean cannot identify a retained event
        if (mPoisoned.load()) return LLTransportControlApply::Poisoned;
        if (mControlApplied) return LLTransportControlApply::Rejected;
        LLTransportControlValues value; LLTransportDecodeResult diagnostic;
        if (!decodePendingControl(value,diagnostic)) return LLTransportControlApply::Rejected;
        if (diagnostic.truncations && !mControlWarningRetained) return LLTransportControlApply::NeedDiagnostics;
        const auto current=mCircuits.lookup(mControlSender,steadyNow.value);
        if (current.state!=LLTransportSnapshotState::Present || !current.alive ||
            current.incarnation!=mControlIncarnation) return LLTransportControlApply::Rejected;
        mControlRequiresReply=value.kind==LLTransportControlValues::Kind::StartPing;
        if (mControlRequiresReply && !replyReserved) return LLTransportControlApply::NeedReplyCapacity;
        if (!mControlEnvelope.reliable || mControlReservation) return LLTransportControlApply::Ready;
        const auto receive=mCircuits.reserveObservedReceive(mControlSender,mControlIncarnation,
            mControlLoginGeneration,mControlEnvelope.packetId,mControlEnvelope.resent,true,
            steadyNow.value,mControlObservation);
        if (receive.status==LLTransportCircuitState::ReceiveStatus::Full)
        {
            // Unexpected loss of preflight capacity must never replay an
            // already applied trailer/body ACK against a later reused ID.
            if (mControlTrailerEffects || mControlAckIndex)
            { mPoisoned.store(true); return LLTransportControlApply::Poisoned; }
            noteControlBackpressure(); mControlPending=false; mControlRequiresReply=false;
            mControlWarningRetained=false;
            return LLTransportControlApply::Backpressure;
        }
        if (receive.status!=LLTransportCircuitState::ReceiveStatus::New || !receive.reservation)
            return LLTransportControlApply::Rejected;
        mControlReservation=receive.reservation;
        return LLTransportControlApply::Ready;
    }

    // Apply only after reply/diagnostic prerequisites and reliable receive
    // admission. Partial PacketAck progress survives Busy/Full retry.
    LLTransportControlApply applyPendingControl(LLTransportSteadyUsec steadyNow, uint64_t heartbeatUsec,
                                                bool replyReserved, bool diagnosticsHandled) noexcept
    {
        mAffectedCircuit = {};
        const auto prepared=preparePendingControl(steadyNow,replyReserved,diagnosticsHandled);
        if (prepared!=LLTransportControlApply::Ready) return prepared;
        LLTransportControlValues value; LLTransportDecodeResult diagnostic;
        if (!decodePendingControl(value, diagnostic)) return LLTransportControlApply::Rejected;
        if (diagnostic.truncations && !mControlWarningRetained) return LLTransportControlApply::NeedDiagnostics;
        const auto current = mCircuits.lookup(mControlSender, steadyNow.value);
        if (current.state != LLTransportSnapshotState::Present || current.incarnation != mControlIncarnation)
            return LLTransportControlApply::Rejected;
        mAffectedCircuit = {mControlSender, mControlIncarnation, true};
        using Status = LLTransportCircuitState::Status;
        if (value.kind == LLTransportControlValues::Kind::StartPing)
        {
            if (!replyReserved) return LLTransportControlApply::NeedReplyCapacity;
            const auto status = mCircuits.forgetReceivedBefore(mControlSender, mControlIncarnation,
                                                                value.oldestUnacked, steadyNow.value);
            if (status == Status::Busy || status == Status::Full) return LLTransportControlApply::Deferred;
            if (status != Status::Ok) return LLTransportControlApply::Rejected;
        }
        else if (value.kind == LLTransportControlValues::Kind::CompletePing)
        {
            const auto status = mCircuits.notePingReply(mControlSender, mControlIncarnation,
                                                         value.pingId, heartbeatUsec, steadyNow.value);
            // Other circuits can publish after preflight. Sequence exhaustion
            // cannot resolve by retrying this held control: fail closed and
            // let owner shutdown cancel its uncommitted reservation.
            if (status == Status::Full) { mPoisoned.store(true); return LLTransportControlApply::Poisoned; }
            if (status == Status::Busy) return LLTransportControlApply::Deferred;
            if (status != Status::Ok) return LLTransportControlApply::Rejected;
        }
        else if (value.kind == LLTransportControlValues::Kind::PacketAck)
        {
            for (; mControlAckIndex < value.ackCount; ++mControlAckIndex)
            {
                const auto status = mCircuits.consumeAck(mControlSender, mControlIncarnation, value.acknowledgements[mControlAckIndex]);
                if (status == Status::Busy || status == Status::Full) return LLTransportControlApply::Deferred;
                if (status != Status::Ok && status != Status::Absent && status != Status::Invalid)
                    return LLTransportControlApply::Rejected;
            }
        }
        else return LLTransportControlApply::Rejected;
        mControlApplied = true;
        return LLTransportControlApply::Applied;
    }

    // Call immediately after successful apply, in the SAME owner turn and
    // with no interleaved close/timeout. StartPing's bounded reply must still
    // be retained; only after this returns Applied may it become sendable.
    // Any impossible commit failure poisons rather than ACKing/forgetting.
    LLTransportControlApply commitPendingControl(LLTransportSteadyUsec steadyNow, bool replyRetained) noexcept
    {
        if (mPoisoned.load()) return LLTransportControlApply::Poisoned;
        if (!mControlPending || mPendingKind!=LLTransportPendingKind::InternalControl || !mControlApplied)
            return LLTransportControlApply::Rejected;
        if (mControlRequiresReply && !replyRetained) return LLTransportControlApply::NeedReplyCapacity;
        if (mControlEnvelope.reliable)
        {
            if (!mControlReservation || mCircuits.commitReceive(mControlReservation,mControlWireSize,steadyNow.value)!=LLTransportCircuitState::Status::Ok)
            { mPoisoned.store(true); return LLTransportControlApply::Poisoned; }
            mControlReservation=0;
        }
        mControlPending=false; mControlApplied=false; mControlAckIndex=0; mControlRequiresReply=false; mControlWarningRetained=false;
        return LLTransportControlApply::Applied;
    }

    // Owner must interpret control without a viewer handler, preserve identity,
    // and clear only after it has retained every required control response.
    LLTransportPendingKind pendingKind() const noexcept { return mPendingKind; }
    // Value for the most recent poll; meaningful on Interrupted/SocketError.
    S32 lastReceiveNativeError() const noexcept { return mLastReceiveNativeError; }
    // Owner-only value report for the immediately preceding poll, resume or
    // control application. Republish this circuit before ending the directory
    // mutation bracket, even when that operation returned a failure.
    AffectedCircuit affectedCircuit() const noexcept { return mAffectedCircuit; }
    const LLTransportEnvelope& controlEnvelope() const noexcept { return mControlEnvelope; }
    LLTransportEndpoint controlSender() const noexcept { return mControlSender; }
    LLTransportEndpoint controlReceivingInterface() const noexcept { return mControlReceiving; }
    const uint8_t* pendingWireBytes() const noexcept { return mWire.data(); }
    uint32_t pendingWireSize() const noexcept { return mControlWireSize; }
    uint64_t controlIncarnation() const noexcept { return mControlIncarnation; }
    uint64_t controlLoginGeneration() const noexcept { return mControlLoginGeneration; }
    uint64_t controlObservation() const noexcept { return mControlObservation; }
    // Independently sampled monotonic counters, including for main-thread
    // diagnostics after join. One owner writes; neither counter can wrap.
    uint64_t controlBackpressureOnOwner() const noexcept { return mControlBackpressure.load(std::memory_order_relaxed); }
    uint64_t controlDuplicateAckBackpressureOnOwner() const noexcept { return mControlDuplicateAckBackpressure.load(std::memory_order_relaxed); }
    // Ingress loss / backlog, same sampling rules. A drop is a datagram read off the
    // socket and then discarded for lack of capacity (DataFull): an unreliable one is
    // lost for good, a reliable one goes un-ACKed and the sim resends it. A backlog
    // episode is one stretch of polls that left datagrams in the kernel (Backlogged).
    uint64_t ingressDroppedReliable() const noexcept { return mIngressDroppedReliable.load(std::memory_order_relaxed); }
    uint64_t ingressDroppedUnreliable() const noexcept { return mIngressDroppedUnreliable.load(std::memory_order_relaxed); }
    uint64_t ingressBacklogEpisodes() const noexcept { return mIngressBacklogEpisodes.load(std::memory_order_relaxed); }
    // Messages that went through the owner's overflow (main was behind; none lost), and
    // how many wait there now (after join: what the stop discards with the pump).
    uint64_t ingressOverflowEvents() const noexcept { return mIngressOverflowEvents.load(std::memory_order_relaxed); }
    uint64_t ingressOverflowPending() const noexcept { return mOverflowPending.load(std::memory_order_relaxed); }
    // Per message: slots 0 .. ingressDropSlotCount()-1. Message number 0 in the last
    // slot collects drops of a message with no schema slot (unknown / beyond the table).
    static constexpr std::size_t kIngressDropSlots = 1024;
    std::size_t ingressDropSlotCount() const noexcept { return kIngressDropSlots; }
    bool ingressDropsAt(std::size_t slot, uint32_t& messageNumber, uint64_t& reliable, uint64_t& unreliable) const noexcept
    {
        if (slot >= kIngressDropSlots) return false;
        const uint64_t rel = mIngressDropsReliableBy[slot].load(std::memory_order_relaxed);
        const uint64_t unrel = mIngressDropsUnreliableBy[slot].load(std::memory_order_relaxed);
        if (!rel && !unrel) return false;
        messageNumber = slot + 1 < kIngressDropSlots && slot < mSchemaCount && mSchemas ? mSchemas[slot].messageNumber : 0;
        reliable = rel; unreliable = unrel;
        return true;
    }
    // Main, after releasing an ingress slot: true once per backlog stretch, meaning the
    // owner stopped reading for lack of room and should be woken now that there is some.
    bool takeBacklogWakeOnMain() noexcept
    {
        return mBacklogWakeWanted.load(std::memory_order_acquire) &&
               mBacklogWakeWanted.exchange(false, std::memory_order_acq_rel);
    }
#if defined(LL_TRANSPORT_PUMP_TESTING)
    // Test reads only after the owner has joined.
    uint64_t testAppPublishBackpressureAfterJoin() const noexcept { return mTestAppPublishBackpressure; }
    uint64_t testStalledAppBackpressureAfterJoin() const noexcept { return mTestStalledAppBackpressure; }
    void testBeginMainStall() noexcept { mTestStallPhase.store(1,std::memory_order_release); }
    void testEndMainStall() noexcept { mTestStallPhase.store(2,std::memory_order_release); }
    bool testMainStallEnded() const noexcept { return mTestStallPhase.load(std::memory_order_acquire)==3; }
#endif
    const uint8_t* controlBytes() const noexcept { return mExpanded.data(); }
    // Legacy decodeData returns false for a template with blocks but no
    // decoded blocks. Only that proven terminal result is discardable here;
    // capacity, invalid schema and control-layout errors need separate fault
    // handling, and reliable or partially processed input must stay held.
    bool discardEmptyUnreliableControl(bool diagnosticsHandled) noexcept
    {
        mAffectedCircuit = {};
        if (mPoisoned.load() || !mControlPending || mControlApplied ||
            mPendingKind != LLTransportPendingKind::InternalControl || mControlEnvelope.reliable) return false;
        const auto* entry = findSchema(mControlEnvelope.messageNumber);
        if (!entry || !entry->schema || !entry->schema->isFrozen() ||
            entry->schema->messageNumber() != mControlEnvelope.messageNumber) return false;
        const auto result = decodeTransportFields(*entry->schema, mExpanded.data(), mControlEnvelope.expandedSize,
                                                   mControlEnvelope.bodyOffset, mScratch);
        if (result.status != LLTransportDecodeStatus::EmptyMessage ||
            (result.truncations && !diagnosticsHandled)) return false;
        if (mControlReservation) { mCircuits.cancelReceive(mControlReservation); mControlReservation=0; }
        mControlAckIndex=0; mControlRequiresReply=false; mControlWarningRetained=false; mControlPending = false;
        return true;
    }
    // Owner-only. Discard only an unapplied control whose identity is no
    // longer current. Cancel its uncommitted receive/ACK reservation; prior
    // applied PacketAck entries are never replayed in this incarnation.
    bool discardPendingIfStale(uint64_t currentLoginGeneration, LLTransportSteadyUsec now) noexcept
    {
        mAffectedCircuit = {};
        if (mPoisoned.load() || !currentLoginGeneration || !mControlLoginGeneration ||
            !mControlPending || mControlApplied ||
            mPendingKind != LLTransportPendingKind::InternalControl) return false;
        const auto current = mCircuits.lookup(mControlSender, now.value);
        if (currentLoginGeneration == mControlLoginGeneration &&
            current.state == LLTransportSnapshotState::Unavailable) return false;
        if (currentLoginGeneration == mControlLoginGeneration &&
            current.state == LLTransportSnapshotState::Present && current.alive &&
            current.incarnation == mControlIncarnation) return false;
        if (mControlReservation) { mCircuits.cancelReceive(mControlReservation); mControlReservation=0; }
        mControlAckIndex=0; mControlRequiresReply=false; mControlPending = false;
        return true;
    }
    // Compatibility for the existing unreliable-only loop. Reliable controls
    // must use commitPendingControl so the reserved ACK is committed once.
    bool releaseControl() noexcept
    {
        if (!mControlPending || mPendingKind != LLTransportPendingKind::InternalControl ||
            !mControlApplied || mControlEnvelope.reliable) return false;
        mControlPending = false; mControlApplied = false; mControlAckIndex=0; mControlRequiresReply=false; mControlWarningRetained=false; return true;
    }
    uint64_t lastPublishedAppSequence() const noexcept { return mLastPublishedAppSequence; }

private:
    static void incrementSaturating(std::atomic<uint64_t>& counter) noexcept
    {
        const auto value=counter.load(std::memory_order_relaxed);
        if (value!=(std::numeric_limits<uint64_t>::max)()) counter.store(value+1,std::memory_order_relaxed);
    }
    void noteControlBackpressure() noexcept { incrementSaturating(mControlBackpressure); }
    bool mainHasRoomOnOwner() const noexcept
    { return mEvents.producerHasRoom() && mArena.producerHasRoomForMaximumRecord(); }
    bool overflowEmptyOnOwner() const noexcept
    {
        bool pending = true;
        return mOverflowEvents.pendingOnConsumer(pending) && !pending;
    }
    // Allocated the first time main falls behind (owner thread; new(nothrow)). If that
    // fails the owner backlogs instead -- slower, never lossy -- and retries later.
    bool overflowHasRoomOnOwner() noexcept
    {
        if (!mOverflowArena.initialized())
        {
            if (mOverflowInitRetry) { --mOverflowInitRetry; return false; }
            if (!mOverflowArena.initialize(kOverflowArenaBytes, mArena.maximumRecordBytes()))
            {
                mOverflowInitRetry = 4096;   // polls before the next attempt
                return false;
            }
        }
        return mOverflowEvents.producerHasRoom() && mOverflowArena.producerHasRoomForMaximumRecord();
    }
    void noteMainFullForTest() noexcept
    {
#if defined(LL_TRANSPORT_PUMP_TESTING)
        // The stall handshake: main was full (an overflow publish or a backlogged poll).
        ++mTestAppPublishBackpressure;
        const auto phase=mTestStallPhase.load(std::memory_order_acquire);
        if (phase==1) ++mTestStalledAppBackpressure;
        else if (phase==2) mTestStallPhase.store(3,std::memory_order_release);
#endif
    }
    // Every path that consumes a datagram and then discards it for lack of capacity
    // goes through here, so no such loss is silent.
    LLTransportRxStatus dropFull(const LLTransportEnvelope& envelope) noexcept
    {
        incrementSaturating(envelope.reliable ? mIngressDroppedReliable : mIngressDroppedUnreliable);
        std::size_t slot = kIngressDropSlots - 1;
        if (const LLTransportSchemaEntry* entry = findSchema(envelope.messageNumber))
        {
            const auto index = static_cast<std::size_t>(entry - mSchemas);
            if (index + 1 < kIngressDropSlots) slot = index;
        }
        incrementSaturating(envelope.reliable ? mIngressDropsReliableBy[slot] : mIngressDropsUnreliableBy[slot]);
        return LLTransportRxStatus::DataFull;
    }
    const LLTransportSchemaEntry* findSchema(uint32_t number) const noexcept
    {
        if (!mSchemas) return nullptr;
        std::size_t lo = 0, hi = mSchemaCount;
        while (lo < hi) { const auto mid = lo + (hi-lo)/2; if (mSchemas[mid].messageNumber < number) lo=mid+1; else hi=mid; }
        return lo < mSchemaCount && mSchemas[lo].messageNumber == number ? &mSchemas[lo] : nullptr;
    }
    LLPacketRing& mRing;
    LLTransportCircuitState& mCircuits;
    LLTransportDecodedArena& mArena;
    LLTransportIngressQueue& mEvents;
    std::atomic<bool>& mPoisoned;
    const LLTransportSchemaEntry* mSchemas;
    std::size_t mSchemaCount;
    std::array<uint8_t, NET_BUFFER_SIZE> mWire{}, mExpanded{};
    LLTransportIngressScratch mScratch{};
    LLTransportEnvelope mControlEnvelope{};
    S32 mLastReceiveNativeError = 0;
    AffectedCircuit mAffectedCircuit{};
    LLTransportEndpoint mControlSender{}, mControlReceiving{};
    uint32_t mControlWireSize = 0;
    uint64_t mControlIncarnation = 0, mControlLoginGeneration = 0, mControlObservation = 0;
    uint64_t mControlReceivedUsec = 0;
    uint64_t mControlReservation = 0;
    std::atomic<uint64_t> mControlBackpressure{0}, mControlDuplicateAckBackpressure{0};
    std::atomic<uint64_t> mIngressDroppedReliable{0}, mIngressDroppedUnreliable{0}, mIngressBacklogEpisodes{0};
    std::array<std::atomic<uint64_t>, kIngressDropSlots> mIngressDropsReliableBy{}, mIngressDropsUnreliableBy{};
    std::atomic<bool> mBacklogWakeWanted{false};
    bool mBacklogged = false; // owner only
    // The owner's overflow for application messages while main's ingress is full:
    // the same arena / queue types, owner-private (both ends on the owner thread).
    static constexpr std::size_t kOverflowArenaBytes = 16u * 1024u * 1024u;
    LLTransportDecodedArena mOverflowArena;
    LLTransportSpscQueue<LLTransportIngressEvent, 16384> mOverflowEvents;
    uint32_t mOverflowInitRetry = 0; // owner only
    std::atomic<uint64_t> mOverflowPending{0}, mIngressOverflowEvents{0};
#if defined(LL_TRANSPORT_PUMP_TESTING)
    std::atomic<uint8_t> mTestStallPhase{0};
    uint64_t mTestAppPublishBackpressure=0, mTestStalledAppBackpressure=0; // read after join
#endif
    uint32_t mControlAckIndex = 0;
    uint16_t mPendingAckIndex = 0;
    uint64_t mNextAppSequence = 1, mLastPublishedAppSequence = 0;
    LLTransportPendingKind mPendingKind = LLTransportPendingKind::InternalControl;
    bool mControlPending = false, mControlApplied = false, mControlRequiresReply = false, mControlWarningRetained = false;
    bool mControlTrailerEffects = false;
};
#endif
