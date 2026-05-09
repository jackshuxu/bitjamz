#include "session.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

// note_to_freq(0) = 440 * 2^(-9/12) = 261.6256 Hz = C4
static const float C4_HZ = 440.f * std::pow(2.f, -9.f / 12.f);

SessionState::SessionState() {
    for (int t = 0; t < TRACKS; ++t) {
        if (TRACK_DEFS[t].type == TrackType::MELODIC) {
            track_root_hz[t] = C4_HZ;
        }
    }
}

SessionState::SessionState(const MsgState& s) {
    session_id = s.session_id;
    bpm        = s.bpm;
    for (int t = 0; t < TRACKS; ++t) {
        track_active[t] = (s.track_active[t] != 0);
        for (int st = 0; st < STEPS; ++st) {
            grid[t][st] = (s.grid[t][st] != 0);
        }
    }
    // Melodic roots — same C4 init as default ctor.
    for (int t = 0; t < TRACKS; ++t) {
        if (TRACK_DEFS[t].type == TrackType::MELODIC) {
            track_root_hz[t] = C4_HZ;
        }
    }
}

std::shared_ptr<SessionState> make_solo_session_state() {
    auto s = std::make_shared<SessionState>();
    static const bool kick_row[STEPS]  = {1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0};
    static const bool snare_row[STEPS] = {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0};
    static const bool chat_row[STEPS]  = {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0};
    std::memcpy(s->grid[0], kick_row,  sizeof(kick_row));
    std::memcpy(s->grid[1], snare_row, sizeof(snare_row));
    std::memcpy(s->grid[3], chat_row,  sizeof(chat_row));
    return s;
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
