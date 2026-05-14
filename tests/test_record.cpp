#include <cassert>
#include <iostream>

#include "session.h"

// Track 0 in TRACK_DEFS is the kick drum (drum_kind = DK_KICK).

static void test_drum_record_writes_cell_and_dirty() {
    SessionState s;
    s.rec_state.store(RecordState::RECORDING);
    s.play_step.store(5);

    record_input(s, /*track=*/0, /*pitch=*/0);

    assert(s.drum_grid[DK_KICK][5] == true);
    assert((s.dirty[0].load() & (1u << 5)) != 0);
    std::cout << "test_drum_record_writes_cell_and_dirty PASSED\n";
}

static void test_record_input_noop_when_off() {
    SessionState s;
    s.rec_state.store(RecordState::OFF);
    s.play_step.store(5);

    record_input(s, /*track=*/0, /*pitch=*/0);
    record_input(s, /*track=*/8, /*pitch=*/60);  // melodic track (lead)

    assert(s.drum_grid[DK_KICK][5] == false);
    assert(s.dirty[0].load() == 0);
    assert(s.melodic_notes[0].empty());
    assert(s.melodic_dirty[0].load() == false);
    std::cout << "test_record_input_noop_when_off PASSED\n";
}

// Track 8 in TRACK_DEFS is the lead melodic track (melodic_idx = 0).
static constexpr int LEAD_TRACK   = 8;
static constexpr int LEAD_MELODIC = 0;

static void test_melodic_record_appends_note_and_dirty() {
    SessionState s;
    s.rec_state.store(RecordState::RECORDING);
    s.play_step.store(5);

    record_input(s, LEAD_TRACK, /*pitch=*/64);

    auto& notes = s.melodic_notes[LEAD_MELODIC];
    assert(notes.size() == 1);
    assert(notes[0].start_step == 5);
    assert(notes[0].duration_steps == 1);
    assert(notes[0].pitch_midi == 64);
    assert(notes[0].velocity == 127);
    assert(s.melodic_dirty[LEAD_MELODIC].load() == true);
    std::cout << "test_melodic_record_appends_note_and_dirty PASSED\n";
}

static void test_melodic_record_dedup_same_step_and_pitch() {
    SessionState s;
    s.rec_state.store(RecordState::RECORDING);
    s.play_step.store(5);

    record_input(s, LEAD_TRACK, 64);
    record_input(s, LEAD_TRACK, 64);  // identical

    assert(s.melodic_notes[LEAD_MELODIC].size() == 1);
    std::cout << "test_melodic_record_dedup_same_step_and_pitch PASSED\n";
}

static void test_melodic_record_polyphonic_same_step() {
    SessionState s;
    s.rec_state.store(RecordState::RECORDING);
    s.play_step.store(5);

    record_input(s, LEAD_TRACK, 60);
    record_input(s, LEAD_TRACK, 64);
    record_input(s, LEAD_TRACK, 67);

    assert(s.melodic_notes[LEAD_MELODIC].size() == 3);
    std::cout << "test_melodic_record_polyphonic_same_step PASSED\n";
}

static void test_melodic_record_64_cap() {
    SessionState s;
    s.rec_state.store(RecordState::RECORDING);
    s.play_step.store(0);

    // Pre-fill to the cap with distinct pitches (cap = 64).
    for (int i = 0; i < MAX_NOTES_PER_MELODIC; ++i) {
        s.melodic_notes[LEAD_MELODIC].push_back(
            Note{0, 1, static_cast<uint8_t>(i), 127});
    }
    assert((int)s.melodic_notes[LEAD_MELODIC].size() == MAX_NOTES_PER_MELODIC);

    // 65th: pitch not already in the list, so dedup wouldn't reject it —
    // only the cap should.
    record_input(s, LEAD_TRACK, 100);

    assert((int)s.melodic_notes[LEAD_MELODIC].size() == MAX_NOTES_PER_MELODIC);
    std::cout << "test_melodic_record_64_cap PASSED\n";
}

static void test_toggle_record_off_playing_goes_to_recording() {
    SessionState s;
    s.playing.store(true);
    s.play_step.store(7);

    toggle_record(s);

    assert(s.rec_state.load() == RecordState::RECORDING);
    assert(s.playing.load() == true);
    assert(s.play_step.load() == 7);  // unchanged
    std::cout << "test_toggle_record_off_playing_goes_to_recording PASSED\n";
}

static void test_toggle_record_off_stopped_enters_countdown() {
    SessionState s;
    s.playing.store(false);
    s.play_step.store(11);  // some stale step

    toggle_record(s);

    assert(s.rec_state.load() == RecordState::COUNTDOWN);
    assert(s.playing.load() == true);
    assert(s.play_step.load() == 0);
    assert(s.countdown_beat.load() == 1);
    std::cout << "test_toggle_record_off_stopped_enters_countdown PASSED\n";
}

static void test_toggle_record_cancels_countdown() {
    SessionState s;
    s.playing.store(false);
    toggle_record(s);  // → COUNTDOWN
    assert(s.rec_state.load() == RecordState::COUNTDOWN);

    toggle_record(s);  // cancel

    assert(s.rec_state.load() == RecordState::OFF);
    assert(s.playing.load() == false);
    assert(s.play_step.load() == 0);
    std::cout << "test_toggle_record_cancels_countdown PASSED\n";
}

static void test_toggle_record_recording_stops_capture_keeps_playing() {
    SessionState s;
    s.playing.store(true);
    s.play_step.store(9);
    s.rec_state.store(RecordState::RECORDING);

    toggle_record(s);

    assert(s.rec_state.load() == RecordState::OFF);
    assert(s.playing.load() == true);    // loop keeps playing
    assert(s.play_step.load() == 9);     // playhead unchanged
    std::cout << "test_toggle_record_recording_stops_capture_keeps_playing PASSED\n";
}

static void test_hard_halt_during_recording() {
    SessionState s;
    s.playing.store(true);
    s.play_step.store(7);
    s.rec_state.store(RecordState::RECORDING);

    hard_halt_transport(s);

    assert(s.rec_state.load() == RecordState::OFF);
    assert(s.playing.load() == false);
    assert(s.play_step.load() == 0);
    std::cout << "test_hard_halt_during_recording PASSED\n";
}

static void test_hard_halt_during_countdown() {
    SessionState s;
    s.playing.store(false);
    toggle_record(s);  // → COUNTDOWN

    hard_halt_transport(s);

    assert(s.rec_state.load() == RecordState::OFF);
    assert(s.playing.load() == false);
    assert(s.play_step.load() == 0);
    std::cout << "test_hard_halt_during_countdown PASSED\n";
}

static void test_countdown_transitions_after_16_ticks() {
    SessionState s;
    s.playing.store(false);
    toggle_record(s);  // → COUNTDOWN
    assert(s.countdown_beat.load() == 1);

    for (int i = 0; i < 16; ++i) countdown_tick(s);

    assert(s.rec_state.load() == RecordState::RECORDING);
    assert(s.play_step.load() == 0);
    std::cout << "test_countdown_transitions_after_16_ticks PASSED\n";
}

static void test_countdown_beat_progression() {
    SessionState s;
    s.playing.store(false);
    toggle_record(s);  // → COUNTDOWN, beat=1
    assert(s.countdown_beat.load() == 1);

    for (int i = 0; i < 4; ++i) countdown_tick(s);
    assert(s.countdown_beat.load() == 2);
    for (int i = 0; i < 4; ++i) countdown_tick(s);
    assert(s.countdown_beat.load() == 3);
    for (int i = 0; i < 4; ++i) countdown_tick(s);
    assert(s.countdown_beat.load() == 4);
    std::cout << "test_countdown_beat_progression PASSED\n";
}

static void test_countdown_tick_noop_when_not_in_countdown() {
    SessionState s;
    s.rec_state.store(RecordState::OFF);
    countdown_tick(s);  // should not transition or mutate
    assert(s.rec_state.load() == RecordState::OFF);
    assert(s.countdown_beat.load() == 0);
    std::cout << "test_countdown_tick_noop_when_not_in_countdown PASSED\n";
}

static void test_countdown_beat_1_is_accented() {
    SessionState s;
    s.playing.store(false);
    toggle_record(s);  // → COUNTDOWN, beat=1, subtick=0

    countdown_tick(s);  // tick 1 fires beat 1
    assert(s.metronome_trig.exchange(false) == true);
    assert(s.metronome_pitch_hz.load() == 1000);
    std::cout << "test_countdown_beat_1_is_accented PASSED\n";
}

static void test_countdown_beats_2_through_4_unaccented() {
    SessionState s;
    s.playing.store(false);
    toggle_record(s);  // → COUNTDOWN

    // Beat 1 fires on tick 1
    countdown_tick(s);
    s.metronome_trig.store(false);  // consume

    // Beats 2,3,4 fire on ticks 5,9,13
    for (int beat = 2; beat <= 4; ++beat) {
        for (int i = 0; i < 4; ++i) countdown_tick(s);
        assert(s.metronome_trig.exchange(false) == true);
        assert(s.metronome_pitch_hz.load() == 800);
    }
    std::cout << "test_countdown_beats_2_through_4_unaccented PASSED\n";
}

static void test_countdown_fires_metronome_regardless_of_enabled_flag() {
    SessionState s;
    s.playing.store(false);
    s.metronome_enabled.store(false);  // explicitly off
    toggle_record(s);  // → COUNTDOWN

    countdown_tick(s);  // beat 1
    assert(s.metronome_trig.exchange(false) == true);
    std::cout << "test_countdown_fires_metronome_regardless_of_enabled_flag PASSED\n";
}

static void test_metronome_step_tick_silent_when_disabled() {
    SessionState s;
    s.metronome_enabled.store(false);
    metronome_step_tick(s, 0);
    metronome_step_tick(s, 4);
    assert(s.metronome_trig.load() == false);
    std::cout << "test_metronome_step_tick_silent_when_disabled PASSED\n";
}

static void test_metronome_step_tick_accents_step_0() {
    SessionState s;
    s.metronome_enabled.store(true);
    metronome_step_tick(s, 0);
    assert(s.metronome_trig.exchange(false) == true);
    assert(s.metronome_pitch_hz.load() == 1000);
    std::cout << "test_metronome_step_tick_accents_step_0 PASSED\n";
}

static void test_metronome_step_tick_unaccented_on_other_beats() {
    SessionState s;
    s.metronome_enabled.store(true);
    for (int step : {4, 8, 12}) {
        metronome_step_tick(s, step);
        assert(s.metronome_trig.exchange(false) == true);
        assert(s.metronome_pitch_hz.load() == 800);
    }
    std::cout << "test_metronome_step_tick_unaccented_on_other_beats PASSED\n";
}

static void test_metronome_step_tick_off_beat_silent() {
    SessionState s;
    s.metronome_enabled.store(true);
    for (int step : {1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15}) {
        metronome_step_tick(s, step);
        assert(s.metronome_trig.load() == false);
    }
    std::cout << "test_metronome_step_tick_off_beat_silent PASSED\n";
}

int main() {
    test_drum_record_writes_cell_and_dirty();
    test_record_input_noop_when_off();
    test_melodic_record_appends_note_and_dirty();
    test_melodic_record_dedup_same_step_and_pitch();
    test_melodic_record_polyphonic_same_step();
    test_melodic_record_64_cap();
    test_toggle_record_off_playing_goes_to_recording();
    test_toggle_record_off_stopped_enters_countdown();
    test_toggle_record_cancels_countdown();
    test_toggle_record_recording_stops_capture_keeps_playing();
    test_hard_halt_during_recording();
    test_hard_halt_during_countdown();
    test_countdown_transitions_after_16_ticks();
    test_countdown_beat_progression();
    test_countdown_tick_noop_when_not_in_countdown();
    test_countdown_beat_1_is_accented();
    test_countdown_beats_2_through_4_unaccented();
    test_countdown_fires_metronome_regardless_of_enabled_flag();
    test_metronome_step_tick_silent_when_disabled();
    test_metronome_step_tick_accents_step_0();
    test_metronome_step_tick_unaccented_on_other_beats();
    test_metronome_step_tick_off_beat_silent();
    std::cout << "All record tests passed.\n";
    return 0;
}
