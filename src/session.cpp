#include "session.h"

#include <chrono>
#include <cmath>
#include <thread>
#include <set>
#include <iostream>

// note_to_freq(0) = 440 * 2^(-9/12) = 261.6256 Hz = C4
static const float C4_HZ = 440.f * std::pow(2.f, -9.f / 12.f);

std::set<uint16_t> SessionState::active_rooms;
std::mutex SessionState::registry_mutex;

uint16_t SessionState::generate_unique_id() {
    std::lock_guard<std::mutex> lock(registry_mutex);
    
    uint16_t candidate = 1; // Starting room number
    
    // Find the first ID that isn't currently in the set
    while (active_rooms.find(candidate) != active_rooms.end()) {
        candidate++;
    }

    std::cout << "room created: " << candidate << std::endl;
    
    active_rooms.insert(candidate);
    return candidate;
}

SessionState::SessionState(bool is_shared) {
    session_id = generate_unique_id();

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
