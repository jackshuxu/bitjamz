#pragma once

#include <atomic>
#include <cstdint>
#include <set>
#include <memory>
#include <map>

// Anything used by more than one module (audio, ui, net, main) lives here.
// Per-module helpers live in that module's own .cpp as file-locals.

namespace App {
    inline std::atomic<bool> running{true};
    inline uint16_t GLOBAL_PORT = 1234;
    inline constexpr uint8_t STATUS_SUCCESS = 0x00;
    inline constexpr uint8_t STATUS_FAILURE = 0xFF;
}

inline constexpr int TRACKS         = 12;
inline constexpr int STEPS          = 16;
inline constexpr int MELODIC_VOICES = 4;
inline constexpr int DRUM_KINDS     = 8;

enum class TrackType { DRUM, MELODIC };

// drum_kind values when type == DRUM (index into audio drum voice + DRUM_PARAMS)
enum DrumKind {
    DK_KICK = 0,
    DK_SNARE,
    DK_CLAP,
    DK_CLOSED_HAT,
    DK_OPEN_HAT,
    DK_COWBELL,
    DK_TOM,
    DK_CYMBAL,
};

struct TrackDef {
    const char* name;        // <= 5 chars
    char        key;         // MPC trigger key
    TrackType   type;
    int         drum_kind;   // 0..7 if DRUM, else -1
    int         melodic_idx; // 0..3 if MELODIC, else -1
};

// Static configuration. Not part of SessionState; never serialized.
// MPC pad layout: core kit on top, extended drums, melodic on bottom.
inline constexpr TrackDef TRACK_DEFS[TRACKS] = {
    // core drums: v b n m
    { "kick",  'v', TrackType::DRUM, DK_KICK,       -1 },
    { "snare", 'b', TrackType::DRUM, DK_SNARE,      -1 },
    { "clap",  'n', TrackType::DRUM, DK_CLAP,       -1 },
    { "chat",  'm', TrackType::DRUM, DK_CLOSED_HAT, -1 },
    // extended drums: f g h j
    { "ohat",  'f', TrackType::DRUM, DK_OPEN_HAT,   -1 },
    { "cowb",  'g', TrackType::DRUM, DK_COWBELL,    -1 },
    { "tom",   'h', TrackType::DRUM, DK_TOM,        -1 },
    { "cymb",  'j', TrackType::DRUM, DK_CYMBAL,     -1 },
    // melodic: r t y u
    { "lead",  'r', TrackType::MELODIC, -1, 0 },
    { "bass",  't', TrackType::MELODIC, -1, 1 },
    { "chord", 'y', TrackType::MELODIC, -1, 2 },
    { "drone", 'u', TrackType::MELODIC, -1, 3 },
};

// Full snapshot of the serializable portion of SessionState. Sent by the
// host to a joiner immediately after a successful handshake. Replaces the
// joiner's local state entirely.
struct __attribute__((packed)) MsgState {
    MsgState(int32_t bpm, uint16_t session_id, uint16_t track_active, uint16_t const src_dirty[TRACKS]):
        bpm(bpm), session_id(session_id), track_active(track_active) {
            std::copy(src_dirty, src_dirty + TRACKS, dirty);
        }
    uint16_t session_id;
    int32_t  bpm = 120;
    uint16_t track_active = 0xFFFF;       // 0/1 per track
    uint16_t dirty[TRACKS];               // 0/1 per cell with bitpacking
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
    uint16_t edited_tracks_mask;
    uint16_t dirty[TRACKS];

    uint8_t  bpm_present;          // 0/1
    int32_t  bpm;                  // valid iff bpm_present

    uint8_t  track_active_present; // 0/1
    uint16_t track_active_mask;    // bit t = track_active[t]; valid iff present
};

// One bitjams session.
//
// A "session" is the collaborative unit: what a host hosts, what a joiner
// joins, what gets wiped on session switch. Shared by every thread —
// UI, timing thread, audio callback, and (future) network flush thread.
//
// Three logical groups of fields:
//   1. Shared creative state — synced across the network:
//        grid, track_active, bpm, track_root_hz
//   2. Per-peer runtime — never synced, each peer runs its own:
//        play_step, playing, running, trig, loop_len
//   3. Networking bookkeeping — only meaningful in multi-peer mode:
//        session_id, dirty

class SessionState {
public:
    SessionState(MsgState& state, bool is_shared);
    ~SessionState();

    uint16_t generate_unique_id();
    static std::map<uint16_t, SessionState*> active_rooms;
    static std::mutex registry_mutex;

    // --- networking bookkeeping ---
    uint16_t session_id = 0;
    std::atomic<bool> has_updates{false}; // Indicates whether there is updated DAW info to share

    // --- shared creative state (synced) ---
    bool  grid[TRACKS][STEPS] = {};
    bool  track_active[TRACKS] = {
        true, true, true, true,
        true, true, true, true,
        true, true, true, true,
    };
    float track_root_hz[TRACKS] = {};
    int   bpm;

    // --- per-peer runtime (not synced) ---
    int               loop_len = STEPS;
    std::atomic<int>  play_step{0};
    std::atomic<bool> running{true};
    std::atomic<bool> playing{true};

    // sequencer -> audio: one-shot triggers
    std::atomic<bool> trig[TRACKS] = {};

    // --- networking bookkeeping ---
    // One bitmask per track, one bit per step. UI sets bits via fetch_or
    // when it edits a cell; the flush thread claims them via exchange(0)
    // every NET_FLUSH_MS to construct outbound diffs. Never serialized.
    std::atomic<uint16_t> dirty[TRACKS] = {};

    std::mutex diff_mutex; // Might not be necessary I think only one thread is managing diff state
    MsgDiff diff {};
};

// The sequencer clock: walks SessionState::grid step by step at the
// session's bpm, writing to SessionState::trig for the audio callback.
void timing_thread(SessionState& s);
