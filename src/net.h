#pragma once

#include <cstdint>
#include <memory>

#include "sequencer.h"

// Single compile-time port for all session connections.
inline constexpr uint16_t NET_PORT = 47474;

// Flush window for outbound diff aggregation.
inline constexpr int NET_FLUSH_MS = 50;

// Wire message type tags. Each message on the socket is a 1-byte type
// prefix followed by a fixed-size packed payload.
enum MsgType : uint8_t {
    MSG_HANDSHAKE = 0x00, // joiner -> host: session_id only
    MSG_STATE     = 0x01, // host -> peer: full SessionState snapshot
    MSG_DIFF      = 0x02, // peer <-> host: incremental edits
};

// Full snapshot of the serializable portion of SessionState. Sent by the
// host to a joiner immediately after a successful handshake. Replaces the
// joiner's local state entirely.
struct __attribute__((packed)) MsgState {
    uint16_t session_id;
    int32_t  bpm;
    uint8_t  track_active[TRACKS];        // 0/1 per track
    uint8_t  grid[TRACKS][STEPS];         // 0/1 per cell
};

// One changed cell carried inside a MsgDiff payload.
struct __attribute__((packed)) DiffCell {
    uint8_t track; // 0..TRACKS-1
    uint8_t step;  // 0..STEPS-1
    uint8_t value; // 0/1
};

// Maximum cells that can ride in a single diff. Equal to one full grid
// because the dirty bitmask claims at most one bit per cell per flush.
inline constexpr int DIFF_MAX_CELLS = TRACKS * STEPS;

// Incremental edit batch. Sent on every flush window where any field
// changed. `cell_count` indicates how many entries of `cells` are valid.
// `bpm_present` and `track_active_present` flag whether their respective
// optional fields carry new values for this batch.
struct __attribute__((packed)) MsgDiff {
    uint16_t cell_count;
    DiffCell cells[DIFF_MAX_CELLS];

    uint8_t  bpm_present;          // 0/1
    int32_t  bpm;                  // valid iff bpm_present

    uint8_t  track_active_present; // 0/1
    uint16_t track_active_mask;    // bit t = track_active[t]; valid iff present
};

// Public net interface. Implementations live in net.cpp (future work).
void net_host(std::shared_ptr<SessionState> state, uint16_t port);
void net_join(std::shared_ptr<SessionState> state, const char* ip, uint16_t port);
void net_stop();
