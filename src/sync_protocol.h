#pragma once

#include <atomic>
#include <cstdint>

// PRD-004: the optimistic-local-edit protocol for synced scalars.
//
// A joiner that mutates a synced field pre-applies locally and marks it
// pending; until the host echoes that field back, incoming network values
// for the same field are dropped (otherwise a stale host echo would clobber
// the joiner's edit during the round trip). On the host side, an inbound
// mutation from any peer is applied and re-dirtied for relay to the other
// peers.
//
// Two overloads differ only in the dirty/in_flight storage shape:
//   - scalar : a single bool dirty + bool in_flight (BPM-shape).
//   - bitmask: a uint16_t bitmask, one bit per slot (track_root_midi-shape).
//
// Both use default seq_cst memory order, matching the inline code these
// helpers replaced.

// Joiner truth table:
//   dirty               -> drop incoming (local edit pending)
//   in_flight && !dirty -> accept incoming, clear in_flight (echo closed gap)
//   neither             -> accept incoming
// Host:
//   always              -> accept incoming, set dirty (relay to other peers)
template <typename T>
inline void apply_optimistic_local_edit_scalar(
    T& field,
    std::atomic<bool>& dirty,
    std::atomic<bool>& in_flight,
    T incoming,
    bool is_joiner) {
    if (is_joiner) {
        if (dirty.load()) return;
        field = incoming;
        if (in_flight.load()) in_flight.store(false);
    } else {
        field = incoming;
        dirty.store(true);
    }
}

// One of N scalars guarded by a uint16_t bitmask dirty + uint16_t bitmask
// in_flight. Same truth table as the scalar overload, applied to a single
// bit of the masks.
template <typename T>
inline void apply_optimistic_local_edit_bitmask(
    T& field,
    std::atomic<uint16_t>& dirty_mask,
    std::atomic<uint16_t>& in_flight_mask,
    uint16_t bit,
    T incoming,
    bool is_joiner) {
    if (is_joiner) {
        if (dirty_mask.load() & bit) return;
        field = incoming;
        if (in_flight_mask.load() & bit) {
            in_flight_mask.fetch_and(static_cast<uint16_t>(~bit));
        }
    } else {
        field = incoming;
        dirty_mask.fetch_or(bit);
    }
}
