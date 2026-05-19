// PRD-004 tests: the two optimistic-local-edit funnels exposed by
// sync_protocol.h. Each funnel encodes the four-cell truth table for a
// synced scalar (joiner+dirty=drop, joiner+in_flight=accept+clear,
// joiner+clean=accept, host=accept+dirty). The helpers take stack-local
// atomics — no SessionState, no network. Eight tests total: four per
// overload, one per truth-table cell.

#include <atomic>
#include <cassert>
#include <cstdint>
#include <iostream>

#include "sync_protocol.h"

// --- scalar overload (BPM-shape: bool dirty + bool in_flight) ---

static void test_scalar_joiner_dirty_drops() {
    int field = 100;
    std::atomic<bool> dirty{true};
    std::atomic<bool> in_flight{false};
    apply_optimistic_local_edit_scalar(field, dirty, in_flight, 200, /*is_joiner=*/true);
    assert(field == 100);
    assert(dirty.load() == true);
    assert(in_flight.load() == false);
    std::cout << "test_scalar_joiner_dirty_drops PASSED\n";
}

static void test_scalar_joiner_in_flight_accepts_and_clears() {
    int field = 100;
    std::atomic<bool> dirty{false};
    std::atomic<bool> in_flight{true};
    apply_optimistic_local_edit_scalar(field, dirty, in_flight, 200, /*is_joiner=*/true);
    assert(field == 200);
    assert(dirty.load() == false);
    assert(in_flight.load() == false);
    std::cout << "test_scalar_joiner_in_flight_accepts_and_clears PASSED\n";
}

static void test_scalar_joiner_clean_accepts() {
    int field = 100;
    std::atomic<bool> dirty{false};
    std::atomic<bool> in_flight{false};
    apply_optimistic_local_edit_scalar(field, dirty, in_flight, 200, /*is_joiner=*/true);
    assert(field == 200);
    assert(dirty.load() == false);
    assert(in_flight.load() == false);
    std::cout << "test_scalar_joiner_clean_accepts PASSED\n";
}

static void test_scalar_host_accepts_and_sets_dirty() {
    int field = 100;
    std::atomic<bool> dirty{false};
    std::atomic<bool> in_flight{false};
    apply_optimistic_local_edit_scalar(field, dirty, in_flight, 200, /*is_joiner=*/false);
    assert(field == 200);
    assert(dirty.load() == true);
    assert(in_flight.load() == false);
    std::cout << "test_scalar_host_accepts_and_sets_dirty PASSED\n";
}

// --- bitmask overload (track_root_midi-shape: uint16_t masks, one bit) ---

static void test_bitmask_joiner_dirty_bit_drops() {
    uint8_t field = 60;
    // bit 2 is the target; bits 0 and 5 are unrelated noise that must survive.
    uint16_t bit = 1u << 2;
    std::atomic<uint16_t> dirty_mask{static_cast<uint16_t>(bit | (1u << 0) | (1u << 5))};
    std::atomic<uint16_t> in_flight_mask{static_cast<uint16_t>(1u << 7)};
    apply_optimistic_local_edit_bitmask(field, dirty_mask, in_flight_mask, bit,
                                        static_cast<uint8_t>(72), /*is_joiner=*/true);
    assert(field == 60);
    // Dirty mask untouched (all three bits still set).
    assert(dirty_mask.load() == static_cast<uint16_t>(bit | (1u << 0) | (1u << 5)));
    // in_flight untouched.
    assert(in_flight_mask.load() == static_cast<uint16_t>(1u << 7));
    std::cout << "test_bitmask_joiner_dirty_bit_drops PASSED\n";
}

static void test_bitmask_joiner_in_flight_bit_accepts_and_clears_only_that_bit() {
    uint8_t field = 60;
    uint16_t bit = 1u << 2;
    uint16_t other_bit = 1u << 9;
    std::atomic<uint16_t> dirty_mask{0};
    std::atomic<uint16_t> in_flight_mask{static_cast<uint16_t>(bit | other_bit)};
    apply_optimistic_local_edit_bitmask(field, dirty_mask, in_flight_mask, bit,
                                        static_cast<uint8_t>(72), /*is_joiner=*/true);
    assert(field == 72);
    assert(dirty_mask.load() == 0);
    // Target bit cleared, the other bit survives.
    assert(in_flight_mask.load() == other_bit);
    std::cout << "test_bitmask_joiner_in_flight_bit_accepts_and_clears_only_that_bit PASSED\n";
}

static void test_bitmask_joiner_clean_accepts() {
    uint8_t field = 60;
    uint16_t bit = 1u << 2;
    std::atomic<uint16_t> dirty_mask{0};
    std::atomic<uint16_t> in_flight_mask{0};
    apply_optimistic_local_edit_bitmask(field, dirty_mask, in_flight_mask, bit,
                                        static_cast<uint8_t>(72), /*is_joiner=*/true);
    assert(field == 72);
    assert(dirty_mask.load() == 0);
    assert(in_flight_mask.load() == 0);
    std::cout << "test_bitmask_joiner_clean_accepts PASSED\n";
}

static void test_bitmask_host_accepts_and_sets_only_that_dirty_bit() {
    uint8_t field = 60;
    uint16_t bit = 1u << 2;
    std::atomic<uint16_t> dirty_mask{0};
    std::atomic<uint16_t> in_flight_mask{0};
    apply_optimistic_local_edit_bitmask(field, dirty_mask, in_flight_mask, bit,
                                        static_cast<uint8_t>(72), /*is_joiner=*/false);
    assert(field == 72);
    // Only the target bit set in dirty; in_flight untouched.
    assert(dirty_mask.load() == bit);
    assert(in_flight_mask.load() == 0);
    std::cout << "test_bitmask_host_accepts_and_sets_only_that_dirty_bit PASSED\n";
}

int main() {
    test_scalar_joiner_dirty_drops();
    test_scalar_joiner_in_flight_accepts_and_clears();
    test_scalar_joiner_clean_accepts();
    test_scalar_host_accepts_and_sets_dirty();
    test_bitmask_joiner_dirty_bit_drops();
    test_bitmask_joiner_in_flight_bit_accepts_and_clears_only_that_bit();
    test_bitmask_joiner_clean_accepts();
    test_bitmask_host_accepts_and_sets_only_that_dirty_bit();
    std::cout << "all optimistic-local-edit tests PASSED\n";
    return 0;
}
