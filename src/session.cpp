#include "session.h"

#include <algorithm>
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
    // Phase 1 invariant: every session begins with one Pattern, id=1. The
    // Pattern constructor reserves the per-track note capacity so the timing
    // thread can iterate melodic_notes concurrently with a UI/record
    // push_back without hitting a reallocation.
    auto p = std::make_unique<Pattern>();
    p->id = 1;
    patterns.push_back(std::move(p));
    // Phase 4: solo session starts with the song looping pattern 1 forever
    // (single bar in the song timeline).
    song.push_back(1);
}

std::shared_ptr<SessionState> make_solo_session_state() {
    auto s = std::make_shared<SessionState>();
    static const bool kick_row[STEPS]  = {1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0};
    static const bool snare_row[STEPS] = {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0};
    static const bool chat_row[STEPS]  = {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0};
    Pattern& p = *s->patterns[0];
    // Phase 2: default groove lives in bar 0 of the 1-bar pattern.
    for (int i = 0; i < STEPS; ++i) p.drum_grid[DK_KICK][0][i].store(kick_row[i]);
    for (int i = 0; i < STEPS; ++i) p.drum_grid[DK_SNARE][0][i].store(snare_row[i]);
    for (int i = 0; i < STEPS; ++i) p.drum_grid[DK_CLOSED_HAT][0][i].store(chat_row[i]);
    return s;
}

void extend_pattern_length(SessionState& s, int n) {
    if (n <= 0) return;
    if (s.patterns.empty()) return;
    Pattern& p = current_edit_pattern(s);
    int new_len = std::min<int>(MAX_BARS_PER_PATTERN, p.length_bars + n);
    p.length_bars = static_cast<uint8_t>(new_len);
}

void shrink_pattern_length(SessionState& s, int n) {
    if (n <= 0) return;
    if (s.patterns.empty()) return;
    Pattern& p = current_edit_pattern(s);
    int new_len = std::max<int>(1, p.length_bars - n);
    p.length_bars = static_cast<uint8_t>(new_len);
}

void inc_pattern_time_sig(SessionState& s) {
    if (s.patterns.empty()) return;
    Pattern& p = current_edit_pattern(s);
    p.time_sig_num = static_cast<uint8_t>(std::min<int>(8, p.time_sig_num + 1));
}

void dec_pattern_time_sig(SessionState& s) {
    if (s.patterns.empty()) return;
    Pattern& p = current_edit_pattern(s);
    p.time_sig_num = static_cast<uint8_t>(std::max<int>(1, p.time_sig_num - 1));
}

namespace {
// Monotonic id allocator: take max existing id + 1. Ids are never reused.
uint16_t next_pattern_id(const SessionState& s) {
    uint16_t mx = 0;
    for (const auto& p : s.patterns) if (p->id > mx) mx = p->id;
    return static_cast<uint16_t>(mx + 1);
}
}  // namespace

uint16_t create_new_pattern(SessionState& s) {
    std::lock_guard<std::mutex> lk(s.patterns_mutex);
    if (s.patterns.size() >= MAX_PATTERNS) return 0;
    auto np = std::make_unique<Pattern>();
    np->id = next_pattern_id(s);
    uint16_t new_id = np->id;
    int append_bars = np->length_bars;
    s.patterns.push_back(std::move(np));
    // Append to song. Drop bars that would overflow the song cap.
    for (int b = 0; b < append_bars; ++b) {
        if (static_cast<int>(s.song.size()) >= MAX_SONG_BARS) break;
        s.song.push_back(new_id);
    }
    s.current_edit_pattern_id.store(new_id);
    s.edit_bar.store(0);
    return new_id;
}

uint16_t duplicate_current_pattern(SessionState& s) {
    std::lock_guard<std::mutex> lk(s.patterns_mutex);
    if (s.patterns.size() >= MAX_PATTERNS) return 0;
    uint16_t src_id = s.current_edit_pattern_id.load();
    Pattern* src = nullptr;
    for (auto& p : s.patterns) if (p->id == src_id) { src = p.get(); break; }
    if (!src) src = s.patterns[0].get();

    auto np = std::make_unique<Pattern>();
    np->id           = next_pattern_id(s);
    np->length_bars  = src->length_bars;
    np->time_sig_num = src->time_sig_num;
    // Copy drum cells.
    for (int k = 0; k < DRUM_KINDS; ++k) {
        for (int b = 0; b < MAX_BARS_PER_PATTERN; ++b) {
            for (int st = 0; st < MAX_STEPS_PER_BAR; ++st) {
                np->drum_grid[k][b][st].store(src->drum_grid[k][b][st].load());
            }
        }
    }
    // Copy melodic notes.
    {
        std::lock_guard<std::mutex> nlk(src->melodic_mutex);
        for (int m = 0; m < MELODIC_VOICES; ++m) {
            np->melodic_notes[m] = src->melodic_notes[m];
        }
    }

    uint16_t new_id = np->id;
    int append_bars = np->length_bars;
    s.patterns.push_back(std::move(np));
    for (int b = 0; b < append_bars; ++b) {
        if (static_cast<int>(s.song.size()) >= MAX_SONG_BARS) break;
        s.song.push_back(new_id);
    }
    s.current_edit_pattern_id.store(new_id);
    s.edit_bar.store(0);
    return new_id;
}

Pattern* find_pattern(SessionState& s, uint16_t id) {
    for (auto& p : s.patterns) if (p->id == id) return p.get();
    return nullptr;
}

const Pattern* find_pattern(const SessionState& s, uint16_t id) {
    for (const auto& p : s.patterns) if (p->id == id) return p.get();
    return nullptr;
}

void song_place_pattern_at_bar(SessionState& s, int bar, uint16_t id) {
    if (bar < 0 || bar >= MAX_SONG_BARS) return;
    std::lock_guard<std::mutex> lk(s.patterns_mutex);
    while (static_cast<int>(s.song.size()) <= bar) {
        if (static_cast<int>(s.song.size()) >= MAX_SONG_BARS) return;
        s.song.push_back(0);  // 0 = empty slot
    }
    s.song[bar] = id;
}

void record_input(SessionState& s, int track, uint8_t pitch_midi) {
    if (s.rec_state.load() != RecordState::RECORDING) return;
    if (track < 0 || track >= TRACKS) return;
    if (s.patterns.empty()) return;
    // Phase 4: record into the currently-playing pattern so overdubs land on
    // the section the user is hearing. (Solo mode and single-pattern songs
    // resolve to the same pattern as before.)
    uint16_t cur_id;
    if (s.pattern_loop.load()) {
        cur_id = s.current_edit_pattern_id.load();
    } else {
        int sbar = s.play_song_bar.load();
        cur_id = (sbar >= 0 && sbar < static_cast<int>(s.song.size()))
                 ? s.song[sbar] : s.patterns[0]->id;
        if (cur_id == 0) cur_id = s.patterns[0]->id;
    }
    Pattern* pat_ptr = find_pattern(s, cur_id);
    if (!pat_ptr) pat_ptr = s.patterns[0].get();
    Pattern& pat = *pat_ptr;
    int spb = steps_per_bar(pat.time_sig_num);
    int target_step = s.play_step.load() % spb;
    int target_bar  = std::clamp<int>(s.play_bar.load(), 0,
                                      pat.length_bars - 1);
    const TrackDef& td = TRACK_DEFS[track];
    if (td.type == TrackType::DRUM) {
        pat.drum_grid[td.drum_kind][target_bar][target_step] = true;
        s.dirty[track].fetch_or(static_cast<uint16_t>(1) << target_step);
        return;
    }
    int midx = td.melodic_idx;
    auto& notes = pat.melodic_notes[midx];
    for (const Note& existing : notes) {
        if (existing.bar == target_bar
            && existing.start_step == target_step
            && existing.pitch_midi == pitch_midi) {
            return;  // same-bar same-step same-pitch dedup
        }
    }
    Note n{ static_cast<uint8_t>(target_bar),
            static_cast<uint8_t>(target_step), 1, pitch_midi, 127 };
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

        // Phase 4: resolve the playing pattern. In pattern-loop mode the
        // playhead stays within the current edit pattern; otherwise it
        // follows the song timeline.
        uint16_t cur_id;
        if (s.pattern_loop.load()) {
            cur_id = s.current_edit_pattern_id.load();
        } else {
            int sbar = s.play_song_bar.load();
            int song_len = static_cast<int>(s.song.size());
            if (song_len <= 0) {
                cur_id = s.patterns.empty() ? 1 : s.patterns[0]->id;
            } else {
                if (sbar >= song_len) { sbar = 0; s.play_song_bar.store(0); }
                cur_id = s.song[sbar];
                if (cur_id == 0) {
                    // Empty song slot: fall back to pattern 1.
                    cur_id = s.patterns.empty() ? 1 : s.patterns[0]->id;
                }
            }
        }
        Pattern* pat = find_pattern(s, cur_id);
        if (!pat && !s.patterns.empty()) pat = s.patterns[0].get();

        int len_bars = pat ? std::max<int>(1, pat->length_bars) : 1;
        // Phase 3: bar length is derived from the pattern's time signature.
        int spb = pat ? steps_per_bar(pat->time_sig_num)
                      : steps_per_bar(4);
        int step = s.play_step.load();
        int bar  = s.play_bar.load();
        if (!first_iter_after_pause) {
            step = step + 1;
            if (step >= spb) {
                step = 0;
                bar = bar + 1;
                if (bar >= len_bars) {
                    bar = 0;
                    // Phase 4: end of pattern. In song mode, advance the
                    // song bar pointer; if it overflows, loop the song.
                    if (!s.pattern_loop.load()) {
                        int sbar = s.play_song_bar.load() + 1;
                        if (sbar >= static_cast<int>(s.song.size())) sbar = 0;
                        s.play_song_bar.store(sbar);
                    }
                }
                s.play_bar.store(bar);
            }
            s.play_step.store(step);
        } else {
            // On first iteration after pause, ensure play_bar/step in-range.
            if (bar >= len_bars) { bar = 0; s.play_bar.store(bar); }
            if (step >= spb)     { step = 0; s.play_step.store(step); }
        }
        first_iter_after_pause = false;

        metronome_step_tick(s, step);

        bool any_solo = false;
        for (int t = 0; t < TRACKS; ++t) if (s.track_solo[t]) { any_solo = true; break; }

        for (int t = 0; t < TRACKS; ++t) {
            if (s.track_muted[t]) continue;
            if (any_solo && !s.track_solo[t]) continue;
            if (!pat) continue;
            const TrackDef& td = TRACK_DEFS[t];
            if (td.type == TrackType::DRUM) {
                if (pat->drum_grid[td.drum_kind][bar][step]) s.trig[t].store(true);
            } else {
                // Fire onsets only — sustain is handled by the synth voice
                // envelope. Multiple onsets on the same step (polyphony) all
                // fire; the audio side has one synth voice per melodic_idx
                // so a later note in the loop just retriggers it.
                //
                // Snapshot size: concurrent record_input appends from the UI
                // thread won't be picked up this loop pass. Combined with
                // the up-front reserve(MAX_NOTES_PER_MELODIC) in the Pattern
                // constructor (so push_back never reallocates), this
                // eliminates the iterator-race double-trigger.
                const auto& notes = pat->melodic_notes[td.melodic_idx];
                size_t n_count = notes.size();
                for (size_t i = 0; i < n_count; ++i) {
                    const Note& n = notes[i];
                    if (n.bar == bar && n.start_step == step) {
                        enqueue_synth_trig(td.melodic_idx, midi_to_hz(n.pitch_midi));
                    }
                }
            }
        }
    }
}
