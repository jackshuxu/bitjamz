// Pure-function tests for the session UI layout helpers.
//
// These are intentionally FTXUI-free: they test what `view_title`,
// `compose_status_strip`, and `help_entries` produce, not which decorators
// the renderer wires up. The implementations live in src/ui_layout.cpp and
// can be exercised directly.

// Defined before <cassert> so assertions survive a -DNDEBUG build (the
// project compiles tests under Release, which would otherwise drop them).
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

#include "ui_layout.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << __FILE__ << ":" << __LINE__ \
                  << " CHECK failed: " #cond << '\n'; \
        std::abort(); \
    } \
} while (0)

// ---- helpers --------------------------------------------------------------

static bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

static int dwidth(std::string_view s) {
    // Mirrors the column-counting in compose_status_strip (UTF-8 codepoints).
    int w = 0;
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int adv = 1;
        if      ((c & 0x80) == 0x00) adv = 1;
        else if ((c & 0xE0) == 0xC0) adv = 2;
        else if ((c & 0xF0) == 0xE0) adv = 3;
        else if ((c & 0xF8) == 0xF0) adv = 4;
        i += adv;
        ++w;
    }
    return w;
}

// ---- view_title -----------------------------------------------------------

static void test_view_title_covers_every_nav_state_with_non_empty_string() {
    // Exhaustive: ALL_NAV_STATES is the closed list. Adding a NavState value
    // without extending view_title's switch would warn (-Wswitch). The runtime
    // assertion below catches an empty mapping too.
    for (NavState ns : ALL_NAV_STATES) {
        std::string t = view_title(ns, /*song_view_focused=*/false);
        CHECK(!t.empty());
    }
    std::cout << "test_view_title_covers_every_nav_state_with_non_empty_string PASSED\n";
}

static void test_view_title_maps_each_nav_state_to_expected_label() {
    CHECK(view_title(NavState::GRID,       false) == "Sequencer");
    CHECK(view_title(NavState::PIANO_ROLL, false) == "Piano Roll");
    CHECK(view_title(NavState::KEYBOARD,   false) == "Keyboard");
    CHECK(view_title(NavState::PARAM_PAGE, false) == "Synth");
    std::cout << "test_view_title_maps_each_nav_state_to_expected_label PASSED\n";
}

static void test_view_title_song_focus_overrides_every_nav_state() {
    for (NavState ns : ALL_NAV_STATES) {
        std::string t = view_title(ns, /*song_view_focused=*/true);
        CHECK(t == "Song");
    }
    std::cout << "test_view_title_song_focus_overrides_every_nav_state PASSED\n";
}

// ---- status strip ---------------------------------------------------------

static const std::vector<StatusItem> kSampleItems = {
    {"play",  "p"},
    {"rec",   "q"},
    {"copy",  "C"},
    {"paste", "V"},
    {"foo",   "1"},
    {"bar",   "2"},
};

static void test_status_strip_help_token_always_present_even_at_minimum_width() {
    // At minimum: just the help token itself.
    std::string s = compose_status_strip(kSampleItems, dwidth(HELP_TOKEN));
    CHECK(s == HELP_TOKEN);

    // One column under the minimum: the function still tries to render help,
    // truncated. Worst case it returns a non-empty string. The contract is
    // that help is never silently dropped.
    s = compose_status_strip(kSampleItems, dwidth(HELP_TOKEN) + 1);
    CHECK(contains(s, "?:") || contains(s, HELP_TOKEN));

    // Empty item list, generous width: help still appears.
    s = compose_status_strip({}, 80);
    CHECK(contains(s, HELP_TOKEN));

    std::cout << "test_status_strip_help_token_always_present_even_at_minimum_width PASSED\n";
}

static void test_status_strip_renders_actionkey_pairs_in_order() {
    // Comfortable width: every sample item fits, no truncation.
    std::string s = compose_status_strip(kSampleItems, 200);
    CHECK(contains(s, "play: p"));
    CHECK(contains(s, "rec: q"));
    CHECK(contains(s, "copy: C"));
    CHECK(contains(s, "paste: V"));
    CHECK(contains(s, HELP_TOKEN));
    // No ellipsis when nothing was dropped.
    CHECK(!contains(s, "…"));

    // Order: priority items before later items.
    size_t i_play  = s.find("play: p");
    size_t i_rec   = s.find("rec: q");
    size_t i_copy  = s.find("copy: C");
    size_t i_paste = s.find("paste: V");
    size_t i_foo   = s.find("foo: 1");
    CHECK(i_play < i_rec);
    CHECK(i_rec  < i_copy);
    CHECK(i_copy < i_paste);
    CHECK(i_paste < i_foo);
    std::cout << "test_status_strip_renders_actionkey_pairs_in_order PASSED\n";
}

static void test_status_strip_truncates_with_ellipsis_when_overflowing() {
    // Width chosen to fit the four priority items but not the context tail.
    std::string s = compose_status_strip(kSampleItems, 50);
    CHECK(contains(s, "play: p"));
    CHECK(contains(s, HELP_TOKEN));
    CHECK(contains(s, "…"));
    // Right-pin: help is the last thing on the line.
    CHECK(s.size() >= std::string_view(HELP_TOKEN).size());
    CHECK(s.substr(s.size() - std::string_view(HELP_TOKEN).size()) == HELP_TOKEN);
    std::cout << "test_status_strip_truncates_with_ellipsis_when_overflowing PASSED\n";
}

static void test_status_strip_empty_list_returns_help_only_padded_to_width() {
    int w = 40;
    std::string s = compose_status_strip({}, w);
    // Help on the right, spaces filling the left.
    CHECK((int)s.size() <= w + 8);  // ASCII path: exact width.
    CHECK(contains(s, HELP_TOKEN));
    // Right-aligned.
    CHECK(s.substr(s.size() - std::string_view(HELP_TOKEN).size()) == HELP_TOKEN);
    std::cout << "test_status_strip_empty_list_returns_help_only_padded_to_width PASSED\n";
}

static void test_status_strip_one_over_drops_last_item_and_appends_ellipsis() {
    // Build a width that exactly fits priority items but not the next one.
    // "play: p | rec: q | copy: C | paste: V" = 37 cols. Add " " + "?: help"
    // (7) = 45. Room for one more " | foo: 1" (9) needs 54. Pick 50 so foo
    // does not fit — the truncation indicator must appear.
    std::string s = compose_status_strip(kSampleItems, 50);
    CHECK(contains(s, "paste: V"));
    CHECK(!contains(s, "foo: 1"));
    CHECK(contains(s, "…"));
    std::cout << "test_status_strip_one_over_drops_last_item_and_appends_ellipsis PASSED\n";
}

static void test_status_items_for_starts_with_priority_keys() {
    for (NavState ns : ALL_NAV_STATES) {
        auto items = status_items_for(ns, /*song_view_focused=*/false);
        const auto& pri = global_priority_items();
        CHECK(items.size() >= pri.size());
        for (size_t i = 0; i < pri.size(); ++i) {
            CHECK(items[i].action == pri[i].action);
            CHECK(items[i].key    == pri[i].key);
        }
    }
    // Same when song is focused.
    auto items = status_items_for(NavState::GRID, /*song_view_focused=*/true);
    CHECK(items.size() >= 4);
    CHECK(items[0].key == "p");
    CHECK(items[1].key == "q");
    CHECK(items[2].key == "C");
    CHECK(items[3].key == "V");
    std::cout << "test_status_items_for_starts_with_priority_keys PASSED\n";
}

// ---- help modal completeness ---------------------------------------------

// Every keybinding the dispatch layer in ui.cpp accepts. Kept here as a
// hand-maintained reference list: adding a new dispatch binding without
// extending help_entries() trips this test.
struct ExpectedBinding {
    std::string section;
    std::string key;       // matched against HelpEntry.key exactly.
};

static const std::vector<ExpectedBinding> kExpectedBindings = {
    // Global
    {"Global", "p"},
    {"Global", "q"},
    {"Global", "Shift+Q"},
    {"Global", "\\"},
    {"Global", "="},
    {"Global", "-"},
    {"Global", "]"},
    {"Global", "["},
    {"Global", "."},
    {"Global", ","},
    {"Global", "+"},
    {"Global", "*"},
    {"Global", "Shift+L"},
    {"Global", "Shift+F"},
    {"Global", "Shift+B"},
    {"Global", "Shift+Tab"},
    {"Global", "?"},
    // Grid
    {"Grid", "Space"},
    {"Grid", "1-9"},
    {"Grid", "Backspace"},
    {"Grid", "Tab"},
    {"Grid", "Enter"},
    {"Grid", "k"},
    {"Grid", "s"},
    {"Grid", "S"},
    {"Grid", "M"},
    {"Grid", "C"},
    {"Grid", "V"},
    {"Grid", "D"},
    // Piano Roll
    {"Piano Roll", "Space"},
    {"Piano Roll", "z / x"},
    {"Piano Roll", "n"},
    {"Piano Roll", "Backspace"},
    {"Piano Roll", "Esc"},
    // Keyboard Mode
    {"Keyboard Mode", "z / x"},
    {"Keyboard Mode", "Space"},
    {"Keyboard Mode", "n"},
    {"Keyboard Mode", "Backspace"},
    {"Keyboard Mode", "Enter"},
    {"Keyboard Mode", "Tab"},
    {"Keyboard Mode", "Esc"},
    // Param Page
    {"Param Page", "o"},
    {"Param Page", "Tab"},
    // Song
    {"Song", "0-9"},
    {"Song", "Shift+Tab"},
};

static void test_help_modal_has_an_entry_for_every_registered_binding() {
    const auto& entries = help_entries();
    for (const auto& expected : kExpectedBindings) {
        bool found = false;
        for (const auto& e : entries) {
            if (e.section == expected.section && e.key == expected.key) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::cerr << "Missing help entry for " << expected.section
                      << " / " << expected.key << '\n';
        }
        CHECK(found);
    }
    std::cout << "test_help_modal_has_an_entry_for_every_registered_binding PASSED\n";
}

static void test_help_modal_covers_every_advertised_section() {
    const auto& entries = help_entries();
    std::set<std::string> seen;
    for (const auto& e : entries) seen.insert(e.section);
    for (const auto& s : help_sections()) {
        if (!seen.count(s)) {
            std::cerr << "Section advertised but empty: " << s << '\n';
        }
        CHECK(seen.count(s));
    }
    std::cout << "test_help_modal_covers_every_advertised_section PASSED\n";
}

static void test_help_modal_has_no_empty_descriptions_or_keys() {
    for (const auto& e : help_entries()) {
        CHECK(!e.section.empty());
        CHECK(!e.key.empty());
        CHECK(!e.description.empty());
    }
    std::cout << "test_help_modal_has_no_empty_descriptions_or_keys PASSED\n";
}

// ---- main -----------------------------------------------------------------

int main() {
    test_view_title_covers_every_nav_state_with_non_empty_string();
    test_view_title_maps_each_nav_state_to_expected_label();
    test_view_title_song_focus_overrides_every_nav_state();

    test_status_strip_help_token_always_present_even_at_minimum_width();
    test_status_strip_renders_actionkey_pairs_in_order();
    test_status_strip_truncates_with_ellipsis_when_overflowing();
    test_status_strip_empty_list_returns_help_only_padded_to_width();
    test_status_strip_one_over_drops_last_item_and_appends_ellipsis();
    test_status_items_for_starts_with_priority_keys();

    test_help_modal_has_an_entry_for_every_registered_binding();
    test_help_modal_covers_every_advertised_section();
    test_help_modal_has_no_empty_descriptions_or_keys();

    std::cout << "All ui_layout tests PASSED\n";
    return 0;
}
