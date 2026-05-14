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
    // Reserve the per-track note capacity up front so the timing thread can
    // iterate melodic_notes concurrently with a UI/record push_back without
    // hitting a reallocation. Combined with the 64-cap on add_note, push_back
    // never grows past this capacity, so the original begin()/end() pointers
    // captured by the timing thread's loop stay valid.
    for (int m = 0; m < MELODIC_VOICES; ++m) {
        melodic_notes[m].reserve(MAX_NOTES_PER_MELODIC);
    }
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

void record_input(SessionState& s, int track, uint8_t pitch_midi) {
    if (s.rec_state.load() != RecordState::RECORDING) return;
    if (track < 0 || track >= TRACKS) return;
    int target_step = s.play_step.load() % s.loop_len;
    const TrackDef& td = TRACK_DEFS[track];
    if (td.type == TrackType::DRUM) {
        s.drum_grid[td.drum_kind][target_step] = true;
        s.dirty[track].fetch_or(static_cast<uint16_t>(1) << target_step);
        return;
    }
    int midx = td.melodic_idx;
    auto& notes = s.melodic_notes[midx];
    for (const Note& existing : notes) {
        if (existing.start_step == target_step
            && existing.pitch_midi == pitch_midi) {
            return;  // same-step same-pitch dedup
        }
    }
    Note n{ static_cast<uint8_t>(target_step), 1, pitch_midi, 127 };
    if (!add_note(notes, n)) return;  // 64-cap silent drop
    s.melodic_dirty[midx].store(true);
}

void toggle_record(SessionState& s) {
    switch (s.rec_state.load()) {
        case RecordState::OFF:
            if (s.playing.load()) {
                s.rec_state.store(RecordState::RECORDING);
            } else {
                s.playing.store(true);
                s.play_step.store(0);
                s.countdown_subtick.store(0);
                s.countdown_beat.store(1);
                s.rec_state.store(RecordState::COUNTDOWN);
            }
            return;
        case RecordState::COUNTDOWN:
            // Cancel: revert transport, do not move the playhead away from 0.
            s.playing.store(false);
            s.play_step.store(0);
            s.countdown_subtick.store(0);
            s.countdown_beat.store(0);
            s.rec_state.store(RecordState::OFF);
            return;
        case RecordState::RECORDING:
            s.rec_state.store(RecordState::OFF);
            return;
    }
}

void hard_halt_transport(SessionState& s) {
    if (s.rec_state.load() == RecordState::OFF) return;
    s.rec_state.store(RecordState::OFF);
    s.playing.store(false);
    s.play_step.store(0);
    s.countdown_subtick.store(0);
    s.countdown_beat.store(0);
}

void countdown_tick(SessionState& s) {
    if (s.rec_state.load() != RecordState::COUNTDOWN) return;
    int subtick = s.countdown_subtick.load();
    int beat    = s.countdown_beat.load();

    if (subtick == 0) {
        // Beat boundary: fire metronome (always audible during countdown).
        s.metronome_pitch_hz.store(beat == 1 ? 1000 : 800);
        s.metronome_trig.store(true);
    }

    int new_subtick = subtick + 1;
    if (new_subtick < 4) {
        s.countdown_subtick.store(new_subtick);
        return;
    }
    // 4-tick beat boundary reached.
    int new_beat = beat + 1;
    if (new_beat > 4) {
        // 5th boundary: release the playhead.
        s.rec_state.store(RecordState::RECORDING);
        s.countdown_beat.store(0);
        s.countdown_subtick.store(0);
        return;
    }
    s.countdown_beat.store(new_beat);
    s.countdown_subtick.store(0);
}

void metronome_step_tick(SessionState& s, int step) {
    if (!s.metronome_enabled.load()) return;
    if ((step & 3) != 0) return;  // beat boundary = every 4 steps
    s.metronome_pitch_hz.store(step == 0 ? 1000 : 800);
    s.metronome_trig.store(true);
}

void timing_thread(SessionState& s) {
    using clk = std::chrono::steady_clock;
    auto next = clk::now();
    // play_step's semantic is "currently-sounding step" (the one whose trigs
    // were most recently fired). After each fire we leave play_step pointing
    // at that step; the NEXT tick increments before firing. This keeps
    // record_input's round-down behavior matching what the user just heard:
    // a key press during step X reads play_step=X and lands the note on X,
    // not the upcoming X+1.
    //
    // `first_iter_after_pause` is true on the first tick after a state where
    // we should NOT pre-increment (initial startup, resume from pause, exit
    // from COUNTDOWN). It causes that tick to fire the current play_step
    // as-is so step 0 (or wherever transport was parked) is the first to
    // sound.
    bool first_iter_after_pause = true;
    bool prev_playing            = false;
    while (s.running.load()) {
        next += std::chrono::microseconds((int)(60'000'000.0 / s.bpm / 4));
        std::this_thread::sleep_until(next);
        bool now_playing = s.playing.load();
        if (!now_playing) {
            prev_playing            = false;
            first_iter_after_pause  = true;
            continue;
        }
        if (!prev_playing) first_iter_after_pause = true;
        prev_playing = true;

        // COUNTDOWN: hold the playhead at 0, drive the count-in beats, and
        // skip the trig pass entirely so the pattern stays silent until
        // recording engages. Force first_iter so the first RECORDING tick
        // fires play_step=0 without pre-incrementing it to 1.
        if (s.rec_state.load() == RecordState::COUNTDOWN) {
            first_iter_after_pause = true;
            countdown_tick(s);
            continue;
        }

        int step = s.play_step.load();
        if (!first_iter_after_pause) {
            step = (step + 1) % s.loop_len;
            s.play_step.store(step);
        }
        first_iter_after_pause = false;

        metronome_step_tick(s, step);

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
                //
                // Snapshot size: concurrent record_input appends from the UI
                // thread won't be picked up this loop pass. Combined with
                // the up-front reserve(MAX_NOTES_PER_MELODIC) in the
                // SessionState constructor (so push_back never reallocates),
                // this eliminates the iterator-race double-trigger.
                const auto& notes = s.melodic_notes[td.melodic_idx];
                size_t n_count = notes.size();
                for (size_t i = 0; i < n_count; ++i) {
                    const Note& n = notes[i];
                    if (n.start_step == step) {
                        enqueue_synth_trig(td.melodic_idx, midi_to_hz(n.pitch_midi));
                    }
                }
            }
        }
    }
}
