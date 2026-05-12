#include "session.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

// synth_trig_pool is the UI/timing → audio path for melodic onsets. Defined
// here (not audio.cpp) so the test binary links cleanly without pulling in
// miniaudio. All slots default to -1.f (= empty); audio drains by exchange.
std::atomic<float> synth_trig_pool[MELODIC_VOICES][TRIG_POOL_SIZE];

namespace {
struct TrigPoolInit {
    TrigPoolInit() {
        for (int m = 0; m < MELODIC_VOICES; ++m)
            for (int i = 0; i < TRIG_POOL_SIZE; ++i)
                synth_trig_pool[m][i].store(-1.f);
    }
};
TrigPoolInit g_trig_pool_init;
}

void enqueue_synth_trig(int melodic_idx, float freq_hz) {
    if (melodic_idx < 0 || melodic_idx >= MELODIC_VOICES) return;
    if (!(freq_hz > 0.f)) return;
    for (int i = 0; i < TRIG_POOL_SIZE; ++i) {
        float expected = -1.f;
        if (synth_trig_pool[melodic_idx][i]
                .compare_exchange_strong(expected, freq_hz)) {
            return;
        }
    }
    // Pool full: drop silently. Caller is firing faster than the audio
    // callback drains, which only happens at pathological note densities.
}

SessionState::SessionState() {
    // No special init: members are default-constructed (drum_grid zeroed,
    // melodic_notes empty, track_root_midi seeded inline in the class).
}

std::shared_ptr<SessionState> make_solo_session_state() {
    auto s = std::make_shared<SessionState>();
    static const bool kick_row[STEPS]  = {1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0};
    static const bool snare_row[STEPS] = {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0};
    static const bool chat_row[STEPS]  = {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0};
    std::memcpy(s->drum_grid[DK_KICK],       kick_row,  sizeof(kick_row));
    std::memcpy(s->drum_grid[DK_SNARE],      snare_row, sizeof(snare_row));
    std::memcpy(s->drum_grid[DK_CLOSED_HAT], chat_row,  sizeof(chat_row));
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

        bool any_solo = false;
        for (int t = 0; t < TRACKS; ++t) if (s.track_solo[t]) { any_solo = true; break; }

        for (int t = 0; t < TRACKS; ++t) {
            if (s.track_muted[t]) continue;
            if (any_solo && !s.track_solo[t]) continue;
            const TrackDef& td = TRACK_DEFS[t];
            if (td.type == TrackType::DRUM) {
                if (s.drum_grid[td.drum_kind][step]) s.trig[t].store(true);
            } else {
                // Fire onsets only — sustain is handled by the synth voice
                // envelope. Multiple onsets on the same step (polyphony) all
                // fire; the audio side has one synth voice per melodic_idx
                // so a later note in the loop just retriggers it.
                for (const Note& n : s.melodic_notes[td.melodic_idx]) {
                    if (n.start_step == step) {
                        enqueue_synth_trig(td.melodic_idx, midi_to_hz(n.pitch_midi));
                    }
                }
            }
        }
        s.play_step.store((step + 1) % s.loop_len);
    }
}
