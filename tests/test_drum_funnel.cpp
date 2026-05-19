// PRD-002 tests: the three drum-cell write funnels exposed by session.h.
//
// Each test exercises one cell of the policy truth table:
//   - set_local_drum_cell  : UI single-toggle / record_input path
//   - fill_local_drum_cells: UI batch (fill/paste/clear), mask-scoped fetch_or
//   - apply_remote_drum_cell: wire path, owns optimistic-local-edit truth table
//
// Tests use only the public session API (no SessionState mutation through
// drum_grid directly except for read-side asserts).

#include <cassert>
#include <cstdint>
#include <iostream>

#include "session.h"

// Drum track 0 maps to DK_KICK. We use track 0 throughout so the
// mask/dirty bookkeeping is on `s.dirty[0]` / `s.in_flight[0]`.
static constexpr int T = 0;  // kick track

static void test_set_local_drum_cell_sets_dirty_bit() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    s.dirty[T].store(0);
    set_local_drum_cell(s, p.id, T, /*bar=*/0, /*step=*/3, true);
    assert(p.drum_grid[0][0][3].load() == true);
    assert((s.dirty[T].load() & (1u << 3)) != 0);
    assert(s.dirty[T].load() == (1u << 3));  // no stray bits
    std::cout << "test_set_local_drum_cell_sets_dirty_bit PASSED\n";
}

static void test_set_local_drum_cell_no_op_clean_still_flips_dirty() {
    // Today's behavior: writing the same value twice still flips the dirty
    // bit. Documented here as the contract — the funnel must not silently
    // skip the dirty mark when the cell value happens to already match.
    SessionState s;
    Pattern& p = *s.patterns[0];
    p.drum_grid[0][0][5].store(true);
    s.dirty[T].store(0);
    set_local_drum_cell(s, p.id, T, /*bar=*/0, /*step=*/5, true);
    assert(p.drum_grid[0][0][5].load() == true);
    assert((s.dirty[T].load() & (1u << 5)) != 0);
    std::cout << "test_set_local_drum_cell_no_op_clean_still_flips_dirty PASSED\n";
}

static void test_fill_local_drum_cells_scopes_mask_to_written_cells() {
    // PRD Risk #1: the batch funnel's fetch_or must reflect only the cells
    // actually written, not 0xFFFF. Filling every 4th step in a 16-step bar
    // means bits 0,4,8,12 must flip — no more, no less.
    SessionState s;
    Pattern& p = *s.patterns[0];
    s.dirty[T].store(0);
    fill_local_drum_cells(s, p.id, T, /*bar=*/0,
                          /*start=*/0, /*interval=*/4, /*loop_len=*/16,
                          /*v=*/true);
    uint16_t expected = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 12);
    assert(s.dirty[T].load() == expected);
    for (int st = 0; st < 16; ++st) {
        bool want = ((expected >> st) & 1u) != 0;
        assert(p.drum_grid[0][0][st].load() == want);
    }
    std::cout << "test_fill_local_drum_cells_scopes_mask_to_written_cells PASSED\n";
}

static void test_apply_remote_drum_cell_joiner_focused_dirty_drops() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    s.current_edit_pattern_id.store(p.id);
    s.edit_bar.store(0);
    p.drum_grid[0][0][2].store(false);
    s.dirty[T].store(1u << 2);          // local edit pending on this bit
    s.in_flight[T].store(0);
    apply_remote_drum_cell(s, p.id, T, /*bar=*/0, /*step=*/2,
                           /*v=*/true, /*is_joiner=*/true);
    // Dropped: grid unchanged, dirty bit intact.
    assert(p.drum_grid[0][0][2].load() == false);
    assert(s.dirty[T].load() == (1u << 2));
    std::cout << "test_apply_remote_drum_cell_joiner_focused_dirty_drops PASSED\n";
}

static void test_apply_remote_drum_cell_joiner_focused_in_flight_clears() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    s.current_edit_pattern_id.store(p.id);
    s.edit_bar.store(0);
    p.drum_grid[0][0][2].store(false);
    s.dirty[T].store(0);
    uint16_t other = 1u << 7;
    s.in_flight[T].store((1u << 2) | other);
    apply_remote_drum_cell(s, p.id, T, /*bar=*/0, /*step=*/2,
                           /*v=*/true, /*is_joiner=*/true);
    assert(p.drum_grid[0][0][2].load() == true);
    // Target bit cleared; other in_flight bits survive.
    assert(s.in_flight[T].load() == other);
    // Joiner accept path must not set dirty.
    assert(s.dirty[T].load() == 0);
    std::cout << "test_apply_remote_drum_cell_joiner_focused_in_flight_clears PASSED\n";
}

static void test_apply_remote_drum_cell_joiner_focused_clean_applies() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    s.current_edit_pattern_id.store(p.id);
    s.edit_bar.store(0);
    p.drum_grid[0][0][2].store(false);
    s.dirty[T].store(0);
    s.in_flight[T].store(0);
    apply_remote_drum_cell(s, p.id, T, /*bar=*/0, /*step=*/2,
                           /*v=*/true, /*is_joiner=*/true);
    assert(p.drum_grid[0][0][2].load() == true);
    assert(s.dirty[T].load() == 0);
    assert(s.in_flight[T].load() == 0);
    std::cout << "test_apply_remote_drum_cell_joiner_focused_clean_applies PASSED\n";
}

static void test_apply_remote_drum_cell_host_focused_sets_dirty() {
    SessionState s;
    Pattern& p = *s.patterns[0];
    s.current_edit_pattern_id.store(p.id);
    s.edit_bar.store(0);
    p.drum_grid[0][0][2].store(false);
    s.dirty[T].store(0);
    s.in_flight[T].store(0);
    apply_remote_drum_cell(s, p.id, T, /*bar=*/0, /*step=*/2,
                           /*v=*/true, /*is_joiner=*/false);
    assert(p.drum_grid[0][0][2].load() == true);
    assert(s.dirty[T].load() == (1u << 2));
    std::cout << "test_apply_remote_drum_cell_host_focused_sets_dirty PASSED\n";
}

static void test_apply_remote_drum_cell_non_focus_skips_dirty() {
    // Non-current-focus edits apply but do NOT touch dirty/in_flight on either
    // role. Covers the asymmetry called out in PRD Risk #3. We exercise two
    // shapes: a non-focus pid, and a non-focus bar on the focus pid.
    SessionState s;
    Pattern& p = *s.patterns[0];

    // Force pattern p to have a 2nd bar so we can write to bar=1.
    set_pattern_length(s, p, 2);

    // (a) Different pid: focus = id=1, bar=0; we set focus to a synthetic id
    // and edit pid=1, which is non-focus.
    s.current_edit_pattern_id.store(999);
    s.edit_bar.store(0);
    s.dirty[T].store(0);
    s.in_flight[T].store(0);
    p.drum_grid[0][0][2].store(false);
    apply_remote_drum_cell(s, p.id, T, /*bar=*/0, /*step=*/2,
                           /*v=*/true, /*is_joiner=*/false);
    assert(p.drum_grid[0][0][2].load() == true);
    assert(s.dirty[T].load() == 0);
    assert(s.in_flight[T].load() == 0);

    // (b) Focus pid, non-focus bar: edit bar=1 while focused on bar=0.
    s.current_edit_pattern_id.store(p.id);
    s.edit_bar.store(0);
    s.dirty[T].store(0);
    s.in_flight[T].store(0);
    p.drum_grid[0][1][4].store(false);
    apply_remote_drum_cell(s, p.id, T, /*bar=*/1, /*step=*/4,
                           /*v=*/true, /*is_joiner=*/false);
    assert(p.drum_grid[0][1][4].load() == true);
    assert(s.dirty[T].load() == 0);
    assert(s.in_flight[T].load() == 0);

    // (c) Joiner non-focus: dirty bit set on the SAME track but for a non-focus
    // bar/pid must not block the apply (the dirty mask is per-track, but the
    // joiner drop only applies when current_focus). Set dirty bit to confirm
    // the edit still lands.
    s.current_edit_pattern_id.store(999);
    s.edit_bar.store(0);
    s.dirty[T].store(1u << 6);
    s.in_flight[T].store(0);
    p.drum_grid[0][0][6].store(false);
    apply_remote_drum_cell(s, p.id, T, /*bar=*/0, /*step=*/6,
                           /*v=*/true, /*is_joiner=*/true);
    assert(p.drum_grid[0][0][6].load() == true);
    // dirty untouched (joiner non-focus must not clear).
    assert(s.dirty[T].load() == (1u << 6));
    std::cout << "test_apply_remote_drum_cell_non_focus_skips_dirty PASSED\n";
}

int main() {
    test_set_local_drum_cell_sets_dirty_bit();
    test_set_local_drum_cell_no_op_clean_still_flips_dirty();
    test_fill_local_drum_cells_scopes_mask_to_written_cells();
    test_apply_remote_drum_cell_joiner_focused_dirty_drops();
    test_apply_remote_drum_cell_joiner_focused_in_flight_clears();
    test_apply_remote_drum_cell_joiner_focused_clean_applies();
    test_apply_remote_drum_cell_host_focused_sets_dirty();
    test_apply_remote_drum_cell_non_focus_skips_dirty();
    std::cout << "all drum-funnel tests PASSED\n";
    return 0;
}
