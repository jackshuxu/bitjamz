#pragma once
#include <cstdint>
#include <memory>
#include "session.h"

inline constexpr uint16_t NET_PORT     = 47474;
inline constexpr int      NET_FLUSH_MS = 50;

// MSG_EDIT — drum-cell sparse edit batch (one per NET_FLUSH_MS window):
//   uint8_t  cell_count
//   for each cell:
//     uint16_t pattern_id  (NBO; Phase 5)
//     uint8_t  track       (0..7)
//     uint8_t  bar         (0..MAX_BARS_PER_PATTERN-1)
//     uint8_t  step        (0..MAX_STEPS_PER_BAR-1)
//     uint8_t  value       (0 or 1)
//   uint8_t  bpm_present
//   int32_t  bpm                       (NBO; always present)
//
// MSG_MELODIC_TRACK — whole-list resend for one melodic track of one pattern:
//   uint16_t pattern_id   (NBO)
//   uint8_t  track_id     (8..11; validated on recv)
//   uint16_t note_count   (NBO; was u8 in Phase 4 and earlier)
//   { uint8_t bar, start_step, duration_steps, pitch_midi, velocity }[note_count]
//
// MSG_TRACK_ROOT_MIDI — scalar:
//   uint8_t  track_id                  (0..11)
//   uint8_t  midi
//
// MSG_PATTERN_NEW — Phase 5; tell peer to allocate a new pattern shell:
//   uint16_t pattern_id   (NBO)
//   uint8_t  length_bars
//   uint8_t  time_sig_num
//
// MSG_PATTERN_META — Phase 5; pattern metadata change:
//   uint16_t pattern_id   (NBO)
//   uint8_t  length_bars
//   uint8_t  time_sig_num
//
// MSG_SONG_EDIT — Phase 5; one song-bar slot edit:
//   uint16_t bar_index    (NBO; 0..MAX_SONG_BARS-1)
//   uint16_t pattern_id   (NBO; 0 = clear)
//
// MSG_STATE — variable-length handshake snapshot. Phase 5 widens to carry
// the full patterns vector and song timeline:
//   uint16_t session_id                (NBO)
//   int32_t  bpm                       (NBO)
//   uint8_t  track_root_midi[12]
//   uint16_t pattern_count             (NBO)
//   for each pattern:
//     uint16_t id                      (NBO)
//     uint8_t  length_bars
//     uint8_t  time_sig_num
//     uint8_t  drum_grid_packed[DRUM_KINDS][MAX_BARS][MAX_STEPS_PER_BAR/8]
//                                       (128 bytes per pattern)
//     for melodic_idx 0..3:
//       uint16_t note_count             (NBO)
//       { uint8_t bar, start, dur, pitch, vel }[note_count]
//   uint16_t song_len                  (NBO)
//   { uint16_t pattern_id }[song_len]  (NBO each)

enum MsgType : uint8_t {
    MSG_HANDSHAKE        = 0x00,
    MSG_HANDSHAKE_FAIL   = 0x01,
    MSG_STATE            = 0x02,
    MSG_EDIT             = 0x03,
    MSG_MELODIC_TRACK    = 0x04,
    MSG_TRACK_ROOT_MIDI  = 0x05,
    MSG_PATTERN_NEW      = 0x06,
    MSG_PATTERN_META     = 0x07,
    MSG_SONG_EDIT        = 0x08,
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
