// Phase 1 tests: Pattern data model, exercised through public session API.
//
// The Pattern struct owns drum_grid and melodic_notes (previously inlined on
// SessionState). SessionState holds one Pattern (id=1) by std::unique_ptr.
// These tests verify the invariants the rest of the codebase depends on.

#include <cassert>
#include <iostream>

#include "session.h"

static void test_session_has_one_pattern_id_1() {
    SessionState s;
    assert(s.patterns.size() == 1);
    assert(s.patterns[0] != nullptr);
    assert(s.patterns[0]->id == 1);
    std::cout << "test_session_has_one_pattern_id_1 PASSED\n";
}

static void test_pattern_default_drum_grid_all_false() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    for (int k = 0; k < DRUM_KINDS; ++k) {
        for (int bar = 0; bar < MAX_BARS_PER_PATTERN; ++bar) {
            for (int step = 0; step < STEPS; ++step) {
                assert(p.drum_grid[k][bar][step].load() == false);
            }
        }
    }
    std::cout << "test_pattern_default_drum_grid_all_false PASSED\n";
}

static void test_pattern_default_melodic_notes_empty() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    for (int m = 0; m < MELODIC_VOICES; ++m) {
        assert(p.melodic_notes[m].empty());
    }
    std::cout << "test_pattern_default_melodic_notes_empty PASSED\n";
}

static void test_solo_session_state_bootstrap() {
    auto s = make_solo_session_state();
    assert(s->patterns.size() == 1);
    Pattern& p = *s->patterns[0];
    assert(p.id == 1);

    // Default kick/snare/chat groove must be byte-equivalent to before.
    static const bool kick_row[STEPS]  = {1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0};
    static const bool snare_row[STEPS] = {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0};
    static const bool chat_row[STEPS]  = {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0};

    for (int i = 0; i < STEPS; ++i) {
        assert(p.drum_grid[DK_KICK][0][i].load() == kick_row[i]);
        assert(p.drum_grid[DK_SNARE][0][i].load() == snare_row[i]);
        assert(p.drum_grid[DK_CLOSED_HAT][0][i].load() == chat_row[i]);
    }
    std::cout << "test_solo_session_state_bootstrap PASSED\n";
}

static void test_record_input_writes_to_pattern() {
    SessionState s;
    s.rec_state.store(RecordState::RECORDING);
    s.play_step.store(5);

    record_input(s, /*track=*/0, /*pitch=*/0);

    assert(s.patterns[0]->drum_grid[DK_KICK][0][5].load() == true);
    std::cout << "test_record_input_writes_to_pattern PASSED\n";
}

static void test_melodic_record_writes_to_pattern() {
    SessionState s;
    s.rec_state.store(RecordState::RECORDING);
    s.play_step.store(3);

    // Track 8 = lead, melodic_idx 0
    record_input(s, /*track=*/8, /*pitch=*/60);

    auto& notes = s.patterns[0]->melodic_notes[0];
    assert(notes.size() == 1);
    assert(notes[0].start_step == 3);
    assert(notes[0].pitch_midi == 60);
    std::cout << "test_melodic_record_writes_to_pattern PASSED\n";
}

// --- Phase 2: multi-bar patterns -------------------------------------------

static void test_pattern_default_length_one_bar() {
    SessionState s;
    assert(s.patterns[0]->length_bars == 1);
    std::cout << "test_pattern_default_length_one_bar PASSED\n";
}

static void test_pattern_drum_grid_indexed_by_bar() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    p.length_bars = 4;
    // Place a cell in every bar at step 0.
    for (int b = 0; b < 4; ++b) {
        p.drum_grid[DK_KICK][b][0].store(true);
    }
    for (int b = 0; b < 4; ++b) {
        assert(p.drum_grid[DK_KICK][b][0].load() == true);
    }
    // Other steps untouched.
    assert(p.drum_grid[DK_KICK][0][1].load() == false);
    std::cout << "test_pattern_drum_grid_indexed_by_bar PASSED\n";
}

static void test_note_has_bar_field() {
    Note n{};
    n.bar = 2;
    n.start_step = 5;
    n.pitch_midi = 60;
    assert(n.bar == 2);
    assert(n.start_step == 5);
    std::cout << "test_note_has_bar_field PASSED\n";
}

static void test_extend_pattern_bar() {
    SessionState s;
    assert(s.patterns[0]->length_bars == 1);
    extend_pattern_length(s, 1);
    assert(s.patterns[0]->length_bars == 2);
    extend_pattern_length(s, 1);
    extend_pattern_length(s, 1);
    assert(s.patterns[0]->length_bars == 4);
    // Capped at MAX_BARS_PER_PATTERN.
    extend_pattern_length(s, 1);
    assert(s.patterns[0]->length_bars == 4);
    std::cout << "test_extend_pattern_bar PASSED\n";
}

static void test_shrink_pattern_non_destructive() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    p.length_bars = 4;
    p.drum_grid[DK_SNARE][2][7].store(true);
    shrink_pattern_length(s, 1);
    assert(p.length_bars == 3);
    shrink_pattern_length(s, 1);
    assert(p.length_bars == 2);
    // Cell at bar=2 should still be in storage (non-destructive).
    assert(p.drum_grid[DK_SNARE][2][7].load() == true);
    // Extending back should re-expose it.
    extend_pattern_length(s, 1);
    extend_pattern_length(s, 1);
    assert(p.length_bars == 4);
    assert(p.drum_grid[DK_SNARE][2][7].load() == true);
    std::cout << "test_shrink_pattern_non_destructive PASSED\n";
}

static void test_shrink_pattern_min_one_bar() {
    SessionState s;
    s.patterns[0]->length_bars = 1;
    shrink_pattern_length(s, 1);
    assert(s.patterns[0]->length_bars == 1);
    std::cout << "test_shrink_pattern_min_one_bar PASSED\n";
}

static void test_record_input_drum_writes_to_current_bar() {
    SessionState s;
    s.patterns[0]->length_bars = 2;
    s.rec_state.store(RecordState::RECORDING);
    s.play_step.store(5);
    s.play_bar.store(1);  // recording into bar 1

    record_input(s, /*track=*/0, /*pitch=*/0);
    assert(s.patterns[0]->drum_grid[DK_KICK][1][5].load() == true);
    assert(s.patterns[0]->drum_grid[DK_KICK][0][5].load() == false);
    std::cout << "test_record_input_drum_writes_to_current_bar PASSED\n";
}

static void test_record_input_melodic_writes_bar() {
    SessionState s;
    s.patterns[0]->length_bars = 2;
    s.rec_state.store(RecordState::RECORDING);
    s.play_step.store(3);
    s.play_bar.store(1);

    record_input(s, /*track=*/8, /*pitch=*/60);
    auto& notes = s.patterns[0]->melodic_notes[0];
    assert(notes.size() == 1);
    assert(notes[0].bar == 1);
    assert(notes[0].start_step == 3);
    std::cout << "test_record_input_melodic_writes_bar PASSED\n";
}

// --- Phase 3: time signatures ----------------------------------------------

static void test_pattern_default_time_sig_4_4() {
    SessionState s;
    assert(s.patterns[0]->time_sig_num == 4);
    std::cout << "test_pattern_default_time_sig_4_4 PASSED\n";
}

static void test_steps_per_bar_formula() {
    assert(steps_per_bar(1) == 4);
    assert(steps_per_bar(3) == 12);
    assert(steps_per_bar(4) == 16);
    assert(steps_per_bar(5) == 20);
    assert(steps_per_bar(8) == 32);
    std::cout << "test_steps_per_bar_formula PASSED\n";
}

static void test_inc_dec_time_sig() {
    SessionState s;
    inc_pattern_time_sig(s);
    assert(s.patterns[0]->time_sig_num == 5);
    inc_pattern_time_sig(s);
    inc_pattern_time_sig(s);
    inc_pattern_time_sig(s);
    assert(s.patterns[0]->time_sig_num == 8);
    // Cap at 8.
    inc_pattern_time_sig(s);
    assert(s.patterns[0]->time_sig_num == 8);
    // Decrement.
    dec_pattern_time_sig(s);
    assert(s.patterns[0]->time_sig_num == 7);
    // Floor at 1.
    for (int i = 0; i < 20; ++i) dec_pattern_time_sig(s);
    assert(s.patterns[0]->time_sig_num == 1);
    std::cout << "test_inc_dec_time_sig PASSED\n";
}

static void test_time_sig_change_non_destructive() {
    // Place a cell at step 18 (only visible in 5/4 or wider). Switch to 4/4
    // (16 steps/bar) — the cell stays in storage but isn't fired.
    SessionState s;
    Pattern& p = *s.patterns[0];
    p.time_sig_num = 8;  // 32 steps/bar
    p.drum_grid[DK_SNARE][0][18].store(true);

    p.time_sig_num = 4;
    // Storage retained.
    assert(p.drum_grid[DK_SNARE][0][18].load() == true);
    // Widening restores visibility (data was already there).
    p.time_sig_num = 8;
    assert(p.drum_grid[DK_SNARE][0][18].load() == true);
    std::cout << "test_time_sig_change_non_destructive PASSED\n";
}

int main() {
    test_session_has_one_pattern_id_1();
    test_pattern_default_drum_grid_all_false();
    test_pattern_default_melodic_notes_empty();
    test_solo_session_state_bootstrap();
    test_record_input_writes_to_pattern();
    test_melodic_record_writes_to_pattern();
    // Phase 2
    test_pattern_default_length_one_bar();
    test_pattern_drum_grid_indexed_by_bar();
    test_note_has_bar_field();
    test_extend_pattern_bar();
    test_shrink_pattern_non_destructive();
    test_shrink_pattern_min_one_bar();
    test_record_input_drum_writes_to_current_bar();
    test_record_input_melodic_writes_bar();
    // Phase 3
    test_pattern_default_time_sig_4_4();
    test_steps_per_bar_formula();
    test_inc_dec_time_sig();
    test_time_sig_change_non_destructive();
    std::cout << "All pattern tests passed.\n";
    return 0;
}
