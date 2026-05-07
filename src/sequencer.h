#pragma once

#include <atomic>

inline constexpr int TRACKS          = 12;
inline constexpr int STEPS           = 16;
inline constexpr int MELODIC_VOICES  = 4;
inline constexpr int DRUM_KINDS      = 8;

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

extern const TrackDef TRACK_DEFS[TRACKS];

// shared sequencer state
extern bool              grid[TRACKS][STEPS];
extern bool              track_active[TRACKS];
extern float             track_root_hz[TRACKS]; // melodic tracks: default C4
extern std::atomic<int>  play_step;
extern std::atomic<bool> running;
extern std::atomic<bool> playing;

// sequencer -> audio
extern std::atomic<bool> trig[TRACKS];

// UI-mutable sequencer parameters
extern int cursor_track;
extern int cursor_step;
extern int window_start;
extern int bpm;
extern int loop_len;

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
