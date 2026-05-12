#pragma once
#include <cstdint>
#include <memory>
#include "session.h"

inline constexpr uint16_t NET_PORT     = 47474;
inline constexpr int      NET_FLUSH_MS = 50;

// MSG_EDIT — drum-cell sparse edit batch (one per NET_FLUSH_MS window):
//   uint8_t  cell_count
//   { uint8_t track, uint8_t step, uint8_t value }[cell_count]   (track 0..7)
//   uint8_t  bpm_present
//   int32_t  bpm                       (network byte order; always present)
//
// MSG_MELODIC_TRACK — whole-list resend for one melodic track:
//   uint8_t  track_id                  (8..11; validated on recv)
//   uint8_t  note_count
//   { uint8_t start_step, duration_steps, pitch_midi, velocity }[note_count]
//
// MSG_TRACK_ROOT_MIDI — scalar:
//   uint8_t  track_id                  (0..11)
//   uint8_t  midi
//
// MSG_STATE — variable-length handshake snapshot:
//   uint16_t session_id                (NBO)
//   int32_t  bpm                       (NBO)
//   uint8_t  track_root_midi[12]
//   uint16_t drum_grid_mask[8]         (NBO, per-drum-kind 16-bit step mask)
//   for melodic_idx in 0..3:
//       uint8_t note_count
//       { uint8_t start_step, duration_steps, pitch_midi, velocity }[note_count]

enum MsgType : uint8_t {
    MSG_HANDSHAKE        = 0x00,
    MSG_HANDSHAKE_FAIL   = 0x01,
    MSG_STATE            = 0x02,
    MSG_EDIT             = 0x03,
    MSG_MELODIC_TRACK    = 0x04,
    MSG_TRACK_ROOT_MIDI  = 0x05,
};

class Network {
public:
    // Start listener on NET_PORT and a flush thread that broadcasts MSG_EDIT
    // packets to every connected peer at NET_FLUSH_MS cadence. Per-peer
    // recv threads apply inbound edits to `state` and dirty them so the
    // flush relays back to all peers (the original sender included).
    static void host(std::shared_ptr<SessionState> state);

    // Synchronous: connect, handshake, recv MSG_STATE. On success, leaves
    // the socket open, spawns a recv thread + a flush thread, and returns
    // a fresh SessionState with `network_alive == true`. Returns nullptr
    // on any failure (host unreachable, wrong room, socket error).
    static std::shared_ptr<SessionState> join(const char* ip, uint16_t room);

    // Signal everything to stop, shutdown sockets to unblock blocking
    // recv/accept, join all background threads. Safe to call when no host
    // or join was issued, and safe to call multiple times.
    static void stop();
};
