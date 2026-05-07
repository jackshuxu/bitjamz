#pragma once

#include <atomic>
#include <cstdint>

// Anything used by more than one module (audio, ui, net, main) lives here.
// Per-module helpers live in that module's own .cpp as file-locals.

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
    SessionState();

    // --- networking bookkeeping ---
    uint16_t session_id = 0;

    // --- shared creative state (synced) ---
    bool  grid[TRACKS][STEPS] = {};
    bool  track_active[TRACKS] = {
        true, true, true, true,
        true, true, true, true,
        true, true, true, true,
    };
    float track_root_hz[TRACKS] = {};
    int   bpm = 120;

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
};

// The sequencer clock: walks SessionState::grid step by step at the
// session's bpm, writing to SessionState::trig for the audio callback.
void timing_thread(SessionState& s);
