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
        for (int step = 0; step < STEPS; ++step) {
            assert(p.drum_grid[k][step].load() == false);
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
        assert(p.drum_grid[DK_KICK][i].load() == kick_row[i]);
        assert(p.drum_grid[DK_SNARE][i].load() == snare_row[i]);
        assert(p.drum_grid[DK_CLOSED_HAT][i].load() == chat_row[i]);
    }
    std::cout << "test_solo_session_state_bootstrap PASSED\n";
}

static void test_record_input_writes_to_pattern() {
    SessionState s;
    s.rec_state.store(RecordState::RECORDING);
    s.play_step.store(5);

    record_input(s, /*track=*/0, /*pitch=*/0);

    assert(s.patterns[0]->drum_grid[DK_KICK][5].load() == true);
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

int main() {
    test_session_has_one_pattern_id_1();
    test_pattern_default_drum_grid_all_false();
    test_pattern_default_melodic_notes_empty();
    test_solo_session_state_bootstrap();
    test_record_input_writes_to_pattern();
    test_melodic_record_writes_to_pattern();
    std::cout << "All pattern tests passed.\n";
    return 0;
}
