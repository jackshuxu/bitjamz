#pragma once

#include <atomic>

inline constexpr int TRACKS          = 12;
inline constexpr int STEPS           = 16;
inline constexpr int MELODIC_VOICES  = 4;
inline constexpr int DRUM_KINDS      = 8;

// UI-mutable sequencer parameters
extern int cursor_track;
extern int cursor_step;
extern int window_start;
extern int loop_len;

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

class SessionState {
public:
    SessionState();

    uint16_t sessionId; // acts as the ID representing a connection to a session on a given IP address
    // MPC pad layout: core kit on top, extended drums, melodic on bottom.
    const TrackDef TRACK_DEFS[TRACKS] = {
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

    // shared sequencer state
    bool              grid[TRACKS][STEPS] = {}; // MOST OBVIOUS PART OF SESSION STATE
    bool              track_active[TRACKS] = {
        true, true, true, true,
        true, true, true, true,
        true, true, true, true,
    };
    float             track_root_hz[TRACKS] = {}; // melodic tracks: default C4
    std::atomic<int>  play_step{0};
    std::atomic<bool> running{true};
    std::atomic<bool> playing{true};

    // sequencer -> audio
    std::atomic<bool> trig[TRACKS] = {};

    int bpm = 120;
};


// music helpers
float note_to_freq(int st);
int   snap_pentatonic(int semitone);
int   key_to_semitone(char c);
bool  in_pentatonic(int st);

// MPC key -> track index (0..TRACKS-1) or -1
int   track_for_mpc_key(char c);

// pattern fill
void fill_pattern(int interval, int start);

// timing thread entry point
void timing_thread();
