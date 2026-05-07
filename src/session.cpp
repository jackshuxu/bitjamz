#include "session.h"

#include <chrono>
#include <cmath>
#include <thread>

// note_to_freq(0) = 440 * 2^(-9/12) = 261.6256 Hz = C4
static const float C4_HZ = 440.f * std::pow(2.f, -9.f / 12.f);

SessionState::SessionState() {
    for (int t = 0; t < TRACKS; ++t)
        if (TRACK_DEFS[t].type == TrackType::MELODIC) track_root_hz[t] = C4_HZ;
}

void timing_thread(SessionState& s) {
    using clk = std::chrono::steady_clock;
    auto next = clk::now();
    while (s.running.load()) {
        next += std::chrono::microseconds((int)(60'000'000.0 / s.bpm / 4));
        std::this_thread::sleep_until(next);
        if (!s.playing.load()) continue;
        int step = s.play_step.load();
        for (int t = 0; t < TRACKS; ++t)
            if (s.track_active[t] && s.grid[t][step]) s.trig[t].store(true);
        s.play_step.store((step + 1) % s.loop_len);
    }
}
