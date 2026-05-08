#include "session.h"

#include <chrono>
#include <cmath>
#include <thread>
#include <set>
#include <iostream>
#include <memory>
#include <map>

// note_to_freq(0) = 440 * 2^(-9/12) = 261.6256 Hz = C4
static const float C4_HZ = 440.f * std::pow(2.f, -9.f / 12.f);

std::map<uint16_t, SessionState*> SessionState::active_rooms;
std::mutex SessionState::registry_mutex;

uint16_t SessionState::generate_unique_id() {
    std::lock_guard<std::mutex> lock(registry_mutex);
    
    uint16_t candidate = 1; // Starting room number
    
    // Find the first ID that isn't currently in the set
    while (active_rooms.contains(candidate)) {
        candidate++;
    }

    std::cout << "room created: " << candidate << std::endl;
    
    active_rooms[candidate] = this;

    return candidate;
}

SessionState::SessionState(MsgState& state, bool is_shared) {
    std::memcpy(&state_struct, &state, sizeof(MsgState));
    session_id = state.session_id;
    bpm = state.bpm;
    
    for (int t = 0; t < TRACKS; ++t) {
        track_active[t] = (state.track_active >> t) & 1;

        // Sync the Atomic Bitmask used for networking
        dirty[t].store(state.dirty[t]);

        // Unpack the bitmask into the 2D boolean grid
        for (int s = 0; s < STEPS; ++s) {
            grid[t][s] = (state.dirty[t] & (1 << s)) != 0;
        }
    }

    for (int t = 0; t < TRACKS; ++t)
        if (TRACK_DEFS[t].type == TrackType::MELODIC) track_root_hz[t] = C4_HZ;
}

SessionState::~SessionState() {
    // When a session is destroyed, free up the ID
    std::lock_guard<std::mutex> lock(registry_mutex);
    active_rooms.erase(session_id);
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
