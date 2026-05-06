#pragma once

#include <atomic>

inline constexpr int TRACKS = 4;
inline constexpr int STEPS  = 16;

// shared sequencer state
extern bool              grid[TRACKS][STEPS];
extern std::atomic<int>  play_step;
extern std::atomic<bool> running;
extern std::atomic<bool> playing;

// drum trigger atomics (sequencer -> audio)
extern std::atomic<bool> trig[TRACKS];

// UI-mutable sequencer parameters
extern int cursor_track;
extern int cursor_step;
extern int bpm;
extern int loop_len;

// music helpers
float note_to_freq(int st);
int   snap_pentatonic(int semitone);
int   key_to_semitone(char c);
bool  in_pentatonic(int st);

// pattern fill
void fill_pattern(int interval);

// timing thread entry point
void timing_thread();
