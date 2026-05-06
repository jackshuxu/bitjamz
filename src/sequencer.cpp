#include "sequencer.h"

#include <chrono>
#include <cmath>
#include <thread>

bool              grid[TRACKS][STEPS] = {};
std::atomic<int>  play_step{0};
std::atomic<bool> running{true};
std::atomic<bool> playing{true};

std::atomic<bool> trig[TRACKS] = {};

int cursor_track = 0;
int cursor_step  = 0;
int bpm          = 120;
int loop_len     = 16;

//music helpers

// semitone offset from C4 → Hz  (0=C4≈261.6, 9=A4=440)
float note_to_freq(int st) {
    return 440.f * std::pow(2.f, (st - 9.f) / 12.f);
}

//pentatonic snap
static const int PENTA_MAP[12] = {0,0,3,3,3,5,5,7,7,10,10,10};
int snap_pentatonic(int semitone) {
    int oct = semitone / 12;
    int s   = semitone % 12;
    if (s < 0) { s += 12; --oct; }  // floor division for negatives
    return oct * 12 + PENTA_MAP[s];
}

// returns semitone offset from C in current octave (0-14), or -1
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

//fill helpers

void fill_pattern(int interval) {
    for (int s = cursor_step; s < loop_len; s += interval)
        grid[cursor_track][s] = true;
}

//timing thread: sequencer clock, tells X should be played now

void timing_thread() {
    using clk = std::chrono::steady_clock;
    auto next = clk::now();
    while (running.load()) {
        next += std::chrono::microseconds((int)(60'000'000.0 / bpm / 4));
        std::this_thread::sleep_until(next);
        if (!playing.load()) continue;
        int s = play_step.load();
        for (int t = 0; t < TRACKS; ++t)
            if (grid[t][s]) trig[t].store(true);
        play_step.store((s + 1) % loop_len);
    }
}
