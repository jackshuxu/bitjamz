#include "sequencer.h"

#include <chrono>
#include <cmath>
#include <thread>


int cursor_track = 0;
int cursor_step  = 0;
int window_start = 0;
int loop_len     = 16;

// note_to_freq(0) = 440 * 2^(-9/12) = 261.6256 Hz = C4
static const float C4_HZ = 440.f * std::pow(2.f, -9.f / 12.f);

struct RootInit {
    RootInit() {
        for (int t = 0; t < TRACKS; ++t)
            if (TRACK_DEFS[t].type == TrackType::MELODIC) track_root_hz[t] = C4_HZ;
    }
};
static RootInit _root_init;

//music helpers

// semitone offset from C4 -> Hz  (0=C4~261.6, 9=A4=440)
float note_to_freq(int st) {
    return 440.f * std::pow(2.f, (st - 9.f) / 12.f);
}

static const int PENTA_MAP[12] = {0,0,3,3,3,5,5,7,7,10,10,10};
int snap_pentatonic(int semitone) {
    int oct = semitone / 12;
    int s   = semitone % 12;
    if (s < 0) { s += 12; --oct; }
    return oct * 12 + PENTA_MAP[s];
}

// piano-keyboard key -> semitone offset from C in current octave (0..14), or -1
int key_to_semitone(char c) {
    switch (c) {
        case 'a': return 0;   case 'w': return 1;
        case 's': return 2;   case 'e': return 3;
        case 'd': return 4;   case 'f': return 5;
        case 't': return 6;   case 'g': return 7;
        case 'y': return 8;   case 'h': return 9;
        case 'u': return 10;  case 'j': return 11;
        case 'k': return 12;  case 'o': return 13;
        case 'l': return 14;
        default:  return -1;
    }
}

bool in_pentatonic(int st) {
    int s = ((st % 12) + 12) % 12;
    return s==0 || s==3 || s==5 || s==7 || s==10;
}

int track_for_mpc_key(char c) {
    for (int t = 0; t < TRACKS; ++t)
        if (TRACK_DEFS[t].key == c) return t;
    return -1;
}

void fill_pattern(int interval, int start) {
    for (int s = start; s < loop_len; s += interval)
        grid[cursor_track][s] = true;
}

//timing thread: sequencer clock

void timing_thread() {
    using clk = std::chrono::steady_clock;
    auto next = clk::now();
    while (running.load()) {
        next += std::chrono::microseconds((int)(60'000'000.0 / bpm / 4));
        std::this_thread::sleep_until(next);
        if (!playing.load()) continue;
        int s = play_step.load();
        for (int t = 0; t < TRACKS; ++t)
            if (track_active[t] && grid[t][s]) trig[t].store(true);
        play_step.store((s + 1) % loop_len);
    }
}
