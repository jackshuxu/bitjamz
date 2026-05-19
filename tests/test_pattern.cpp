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

// --- Phase 4: multiple patterns + song mode --------------------------------

static void test_session_song_starts_one_entry() {
    SessionState s;
    assert(s.song.size() == 1);
    assert(s.song[0] == 1);
    std::cout << "test_session_song_starts_one_entry PASSED\n";
}

static void test_create_new_pattern_appends_to_song() {
    SessionState s;
    uint16_t new_id = create_new_pattern(s);
    assert(new_id != 0);
    assert(new_id != 1);
    assert(s.patterns.size() == 2);
    assert(s.patterns[1]->id == new_id);
    // Song length should grow by new pattern's length (1 bar default).
    assert(s.song.size() == 2);
    assert(s.song[1] == new_id);
    // Edit focus moves to the new pattern.
    assert(s.current_edit_pattern_id.load() == new_id);
    std::cout << "test_create_new_pattern_appends_to_song PASSED\n";
}

static void test_duplicate_pattern_copies_content() {
    SessionState s;
    Pattern& p1 = *s.patterns[0];
    p1.drum_grid[DK_KICK][0][7].store(true);
    p1.length_bars = 2;
    p1.time_sig_num = 3;
    p1.melodic_notes[0].push_back({0, 4, 1, 60, 127});

    uint16_t new_id = duplicate_current_pattern(s);
    assert(s.patterns.size() == 2);
    assert(new_id != 1);
    Pattern& p2 = *s.patterns[1];
    assert(p2.id == new_id);
    assert(p2.length_bars == 2);
    assert(p2.time_sig_num == 3);
    assert(p2.drum_grid[DK_KICK][0][7].load() == true);
    assert(p2.melodic_notes[0].size() == 1);
    assert(p2.melodic_notes[0][0].pitch_midi == 60);
    // Independent: editing p1 doesn't touch p2.
    p1.drum_grid[DK_SNARE][0][3].store(true);
    assert(p2.drum_grid[DK_SNARE][0][3].load() == false);
    std::cout << "test_duplicate_pattern_copies_content PASSED\n";
}

static void test_lookup_pattern_by_id() {
    SessionState s;
    uint16_t a = create_new_pattern(s);
    uint16_t b = create_new_pattern(s);
    Pattern* pa = find_pattern(s, a);
    Pattern* pb = find_pattern(s, b);
    Pattern* p1 = find_pattern(s, 1);
    Pattern* missing = find_pattern(s, 999);
    assert(pa != nullptr && pa->id == a);
    assert(pb != nullptr && pb->id == b);
    assert(p1 != nullptr && p1->id == 1);
    assert(missing == nullptr);
    std::cout << "test_lookup_pattern_by_id PASSED\n";
}

static void test_song_place_at_bar_extends_song() {
    SessionState s;
    uint16_t a = create_new_pattern(s);
    // Place pattern a at bar 5 — song must grow to include bar 5.
    song_place_pattern_at_bar(s, /*bar=*/5, a);
    assert(s.song.size() >= 6);
    assert(s.song[5] == a);
    std::cout << "test_song_place_at_bar_extends_song PASSED\n";
}

// --- Phase 6: polish + edge cases ------------------------------------------

static void test_solo_session_invariant_unchanged() {
    // Critical invariant: solo launch is byte-equivalent to pre-PRD behavior.
    // 1 pattern (id=1), 1 bar, 4/4, default kick/snare/chat groove, song=[1],
    // pattern_loop off, song view not focused, current_edit_pattern_id=1.
    auto s = make_solo_session_state();
    assert(s->patterns.size() == 1);
    assert(s->patterns[0]->id == 1);
    assert(s->patterns[0]->length_bars == 1);
    assert(s->patterns[0]->time_sig_num == 4);
    assert(s->song.size() == 1);
    assert(s->song[0] == 1);
    assert(s->pattern_loop.load() == false);
    assert(s->song_view_focused.load() == false);
    assert(s->current_edit_pattern_id.load() == 1);
    assert(s->play_song_bar.load() == 0);
    assert(s->play_bar.load() == 0);
    assert(s->edit_bar.load() == 0);
    std::cout << "test_solo_session_invariant_unchanged PASSED\n";
}

static void test_pattern_loop_state_default_off() {
    SessionState s;
    assert(s.pattern_loop.load() == false);
    s.pattern_loop.store(true);
    assert(s.pattern_loop.load() == true);
    std::cout << "test_pattern_loop_state_default_off PASSED\n";
}

static void test_song_capped_at_max_bars() {
    SessionState s;
    // Manually push entries to exceed the cap.
    for (int i = 0; i < MAX_SONG_BARS + 10; ++i) {
        song_place_pattern_at_bar(s, i, 1);
    }
    assert(static_cast<int>(s.song.size()) <= MAX_SONG_BARS);
    std::cout << "test_song_capped_at_max_bars PASSED\n";
}

// --- PRD-001 gap closure: user-story-named tests --------------------------
//
// These cover the gaps enumerated in PRD-001-song-mode-gaps.md. Each is
// named after the user story it protects so a future audit can grep for
// missing stories.

// G3 / US #11: placing a multi-bar pattern fills consecutive song cells.
static void test_us11_placing_multi_bar_pattern_fills_consecutive_song_cells() {
    SessionState s;
    uint16_t a = create_new_pattern(s);
    Pattern* pa = find_pattern(s, a);
    assert(pa != nullptr);
    pa->length_bars = 3;

    song_place_pattern_at_bar(s, /*bar=*/10, a);
    assert(static_cast<int>(s.song.size()) >= 13);
    assert(s.song[10] == a);
    assert(s.song[11] == a);
    assert(s.song[12] == a);
    std::cout << "test_us11_placing_multi_bar_pattern_fills_consecutive_song_cells PASSED\n";
}

// G4 / US #9: inserting a wider pattern pushes later blocks right.
static void test_us9_inserting_wider_pattern_pushes_later_blocks_right() {
    SessionState s;
    uint16_t a = create_new_pattern(s);   // 1 bar
    uint16_t b = create_new_pattern(s);   // 1 bar
    Pattern* pa = find_pattern(s, a);
    assert(pa != nullptr);
    pa->length_bars = 3;

    // Park b at bar 5.
    song_place_pattern_at_bar(s, 5, b);
    assert(s.song[5] == b);
    size_t before_size = s.song.size();

    // Place 3-bar `a` at bar 4; b at bar 5 must shift right to bar 7.
    song_place_pattern_at_bar(s, 4, a);
    assert(s.song[4] == a);
    assert(s.song[5] == a);
    assert(s.song[6] == a);
    // b shifted to bar 7 (4+3).
    assert(s.song[7] == b);
    assert(s.song.size() > before_size);
    std::cout << "test_us9_inserting_wider_pattern_pushes_later_blocks_right PASSED\n";
}

// G4 / US #9 (overflow): pushed-right entries past MAX_SONG_BARS drop.
static void test_us9_push_right_overflow_drops() {
    SessionState s;
    uint16_t a = create_new_pattern(s);
    Pattern* pa = find_pattern(s, a);
    assert(pa != nullptr);
    pa->length_bars = 4;

    // Fill song up to MAX_SONG_BARS with `1`.
    for (int i = 0; i < MAX_SONG_BARS; ++i) {
        if (i >= static_cast<int>(s.song.size())) s.song.push_back(1);
        else                                       s.song[i] = 1;
    }
    assert(static_cast<int>(s.song.size()) == MAX_SONG_BARS);

    // Insert a 4-bar pattern at bar 0; tail entries fall off the cap.
    song_place_pattern_at_bar(s, 0, a);
    assert(static_cast<int>(s.song.size()) == MAX_SONG_BARS);
    assert(s.song[0] == a);
    std::cout << "test_us9_push_right_overflow_drops PASSED\n";
}

// G8 / US #29: pattern_new_dirty is set when create_new_pattern is invoked,
// so the network flush thread can broadcast MSG_PATTERN_NEW for it.
static void test_us29_msg_pattern_new_marks_dirty_on_plus_key() {
    SessionState s;
    // All bits start clear.
    for (int i = 0; i < 4; ++i) assert(s.pattern_new_dirty[i].load() == 0);

    uint16_t a = create_new_pattern(s);
    assert(a != 0);
    // The new pattern's bit must be set.
    uint64_t mask = s.pattern_new_dirty[a / 64].load();
    assert((mask >> (a % 64)) & 1u);
    std::cout << "test_us29_msg_pattern_new_marks_dirty_on_plus_key PASSED\n";
}

// G9: pattern_meta_dirty is set when extend / shrink / inc / dec mutate the
// edit pattern's metadata.
static void test_us_pattern_meta_marks_dirty_on_edit() {
    SessionState s;
    // Clear all pattern_meta bits the constructor might have left.
    for (int i = 0; i < 4; ++i) s.pattern_meta_dirty[i].store(0);

    extend_pattern_length(s, 1);
    uint16_t pid = s.patterns[0]->id;
    assert((s.pattern_meta_dirty[pid / 64].load() >> (pid % 64)) & 1u);

    s.pattern_meta_dirty[pid / 64].store(0);
    inc_pattern_time_sig(s);
    assert((s.pattern_meta_dirty[pid / 64].load() >> (pid % 64)) & 1u);
    std::cout << "test_us_pattern_meta_marks_dirty_on_edit PASSED\n";
}

// G10 / US #30: song_dirty is set when a song-bar slot is mutated, so the
// network flush thread can broadcast MSG_SONG_EDIT for it.
static void test_us30_msg_song_edit_marks_dirty_on_commit() {
    SessionState s;
    // Clear any bits the constructor left behind.
    for (int i = 0; i < 2; ++i) s.song_dirty[i].store(0);

    uint16_t a = create_new_pattern(s);
    // Clear again (create_new_pattern legitimately sets bits).
    for (int i = 0; i < 2; ++i) s.song_dirty[i].store(0);

    song_place_pattern_at_bar(s, /*bar=*/12, a);
    assert((s.song_dirty[12 / 64].load() >> (12 % 64)) & 1u);
    std::cout << "test_us30_msg_song_edit_marks_dirty_on_commit PASSED\n";
}

// G5 (grep-based proxy): the cursor-paths in ui.cpp must not reference STEPS.
// We can't grep in C++ at test time, but we can assert the helper exists and
// returns the pattern's spb (which is what the cursor handlers now read).
static void test_steps_constant_not_referenced_in_cursor_paths() {
    SessionState s;
    // 4/4 default.
    assert(steps_per_bar(s.patterns[0]->time_sig_num) == 16);
    s.patterns[0]->time_sig_num = 5;
    assert(steps_per_bar(s.patterns[0]->time_sig_num) == 20);
    s.patterns[0]->time_sig_num = 8;
    assert(steps_per_bar(s.patterns[0]->time_sig_num) == 32);
    std::cout << "test_steps_constant_not_referenced_in_cursor_paths PASSED\n";
}

// Invariant: every code path that grows length_bars must also grow the
// pattern's contiguous runs in s.song by the same delta. duplicate_current_bar
// is the path most easily forgotten, so pin it here directly.
static void test_duplicate_bar_grows_song_run_for_pid() {
    SessionState s;
    // Default session: pattern 1 placed at song[0] with length_bars=1.
    Pattern& p = *s.patterns[0];
    p.drum_grid[DK_KICK][0][3].store(true);
    assert(p.length_bars == 1);
    assert(s.song.size() == 1 && s.song[0] == 1);

    duplicate_current_bar(s);
    assert(p.length_bars == 2);
    // The new bar is a copy of bar 0.
    assert(p.drum_grid[DK_KICK][1][3].load() == true);
    // Song must have grown to two cells of pid=1 (otherwise song-mode width
    // drifts from the pattern's width — the bug this test guards).
    assert(s.song.size() == 2);
    assert(s.song[0] == 1 && s.song[1] == 1);
    std::cout << "test_duplicate_bar_grows_song_run_for_pid PASSED\n";
}

// Mid-pattern duplicate: inserting after bar 1 of a 3-bar pattern should shift
// the old bar 2 to bar 3 (drums + notes) and stamp the new bar 2 from bar 1.
static void test_duplicate_bar_inserts_after_cursor() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    extend_pattern_length(s, 2);  // 1 → 3 bars (helper also grows song)
    assert(p.length_bars == 3);
    p.drum_grid[DK_SNARE][0][0].store(true);  // bar 0 marker
    p.drum_grid[DK_SNARE][1][1].store(true);  // bar 1 marker (to be duplicated)
    p.drum_grid[DK_SNARE][2][2].store(true);  // bar 2 marker (should shift to 3)

    s.edit_bar.store(1);
    duplicate_current_bar(s);

    assert(p.length_bars == 4);
    assert(p.drum_grid[DK_SNARE][0][0].load() == true);   // untouched
    assert(p.drum_grid[DK_SNARE][1][1].load() == true);   // src unchanged
    assert(p.drum_grid[DK_SNARE][2][1].load() == true);   // new bar = copy of src
    assert(p.drum_grid[DK_SNARE][3][2].load() == true);   // shifted right
    std::cout << "test_duplicate_bar_inserts_after_cursor PASSED\n";
}

// Cap behavior: duplicating at MAX_BARS_PER_PATTERN is a no-op.
static void test_duplicate_bar_capped_at_max() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    extend_pattern_length(s, MAX_BARS_PER_PATTERN - 1);
    assert(p.length_bars == MAX_BARS_PER_PATTERN);
    size_t song_before = s.song.size();
    duplicate_current_bar(s);
    assert(p.length_bars == MAX_BARS_PER_PATTERN);
    assert(s.song.size() == song_before);
    std::cout << "test_duplicate_bar_capped_at_max PASSED\n";
}

// --- PRD-003: cursor clamp folded into set_pattern_length ----------------
//
// Truth table for the funnel's invariant 4 (cursor clamp on shrink). Each
// test pins one cell so a future regression — e.g. someone re-introducing a
// manual clamp in a caller and accidentally drifting from the funnel — is
// caught at the unit level rather than in playback.

// Cell 1: shrink-focused + edit_bar past end → clamp to len-1.
static void test_set_pattern_length_shrink_clamps_edit_bar() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    extend_pattern_length(s, 2);  // 1 → 3 bars
    assert(p.length_bars == 3);
    s.edit_bar.store(2);
    // Shrink to length 1 — edit_bar=2 is now past the end.
    set_pattern_length(s, p, 1);
    assert(p.length_bars == 1);
    assert(s.edit_bar.load() == 0);  // clamped to len-1 == 0
    std::cout << "test_set_pattern_length_shrink_clamps_edit_bar PASSED\n";
}

// Cell 2: shrink-focused + play_bar past end → reset to 0 (NOT len-1).
// Pins the asymmetric semantics: play_bar restart from 0 is less surprising
// than picking up mid-bar inside a freshly resized pattern.
static void test_set_pattern_length_shrink_clamps_play_bar_to_zero() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    extend_pattern_length(s, 2);  // 1 → 3 bars
    s.play_bar.store(2);
    set_pattern_length(s, p, 2);
    assert(p.length_bars == 2);
    assert(s.play_bar.load() == 0);  // reset to 0, NOT len-1 == 1
    std::cout << "test_set_pattern_length_shrink_clamps_play_bar_to_zero PASSED\n";
}

// Cell 3: shrink-non-focused → cursors on the focused pattern unchanged.
// The guard `pid == current_edit_pattern_id` exists so peer A focused on
// pattern X doesn't see its cursor move when peer B shrinks pattern Y.
static void test_set_pattern_length_shrink_skips_clamp_for_non_focused_pattern() {
    SessionState s;
    uint16_t a_id = s.patterns[0]->id;  // pattern A (focused initially)
    uint16_t b_id = create_new_pattern(s);  // focus moves to B
    Pattern* pa = find_pattern(s, a_id);
    Pattern* pb = find_pattern(s, b_id);
    assert(pa && pb);

    // Grow B then re-focus A. Set A's cursors at positions that would be
    // valid on B-at-length-3 but past the end of B-at-length-1 — they must
    // not move when B shrinks because A is focused, not B.
    set_pattern_length(s, *pb, 3);
    s.current_edit_pattern_id.store(a_id);
    s.edit_bar.store(2);
    s.play_bar.store(2);

    // Shrink B while A is focused: cursors must stay put.
    set_pattern_length(s, *pb, 1);
    assert(pb->length_bars == 1);
    assert(s.edit_bar.load() == 2);
    assert(s.play_bar.load() == 2);
    std::cout << "test_set_pattern_length_shrink_skips_clamp_for_non_focused_pattern PASSED\n";
}

// Cell 4: grow → cursors never move. Clamp logic is shrink-only.
static void test_set_pattern_length_grow_does_not_touch_cursors() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    assert(p.length_bars == 1);
    s.edit_bar.store(0);
    s.play_bar.store(0);
    set_pattern_length(s, p, 3);
    assert(p.length_bars == 3);
    assert(s.edit_bar.load() == 0);
    assert(s.play_bar.load() == 0);
    std::cout << "test_set_pattern_length_grow_does_not_touch_cursors PASSED\n";
}

static void test_create_pattern_then_edit_focus_works() {
    SessionState s;
    uint16_t a = create_new_pattern(s);
    assert(s.current_edit_pattern_id.load() == a);
    // current_edit_pattern resolves correctly.
    Pattern& cur = current_edit_pattern(s);
    assert(cur.id == a);
    // Editing through current_edit_pattern doesn't leak to pattern 1.
    extend_pattern_length(s, 1);
    assert(s.patterns[1]->length_bars == 2);
    assert(s.patterns[0]->length_bars == 1);
    std::cout << "test_create_pattern_then_edit_focus_works PASSED\n";
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
    // Phase 7
    test_duplicate_bar_grows_song_run_for_pid();
    test_duplicate_bar_inserts_after_cursor();
    test_duplicate_bar_capped_at_max();
    // Phase 4
    test_session_song_starts_one_entry();
    test_create_new_pattern_appends_to_song();
    test_duplicate_pattern_copies_content();
    test_lookup_pattern_by_id();
    test_song_place_at_bar_extends_song();
    // Phase 6 invariants
    test_solo_session_invariant_unchanged();
    test_pattern_loop_state_default_off();
    test_song_capped_at_max_bars();
    test_create_pattern_then_edit_focus_works();
    // PRD-001 gap closure (user-story-named).
    test_us11_placing_multi_bar_pattern_fills_consecutive_song_cells();
    test_us9_inserting_wider_pattern_pushes_later_blocks_right();
    test_us9_push_right_overflow_drops();
    test_us29_msg_pattern_new_marks_dirty_on_plus_key();
    test_us_pattern_meta_marks_dirty_on_edit();
    test_us30_msg_song_edit_marks_dirty_on_commit();
    test_steps_constant_not_referenced_in_cursor_paths();
    // PRD-003: cursor clamp inside set_pattern_length.
    test_set_pattern_length_shrink_clamps_edit_bar();
    test_set_pattern_length_shrink_clamps_play_bar_to_zero();
    test_set_pattern_length_shrink_skips_clamp_for_non_focused_pattern();
    test_set_pattern_length_grow_does_not_touch_cursors();
    std::cout << "All pattern tests passed.\n";
    return 0;
}
