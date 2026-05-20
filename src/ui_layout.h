#pragma once

// Pure layout / labeling logic for the session UI. Kept FTXUI-free so unit
// tests can link against it directly without pulling in any rendering deps.

#include <array>
#include <string>
#include <vector>

// Mirrors the navigation states tracked inside ui.cpp. Exposed here so the
// title-mapping function can be tested exhaustively.
enum class NavState { GRID, KEYBOARD, PARAM_PAGE, PIANO_ROLL };

inline constexpr std::array<NavState, 4> ALL_NAV_STATES = {
    NavState::GRID,
    NavState::KEYBOARD,
    NavState::PARAM_PAGE,
    NavState::PIANO_ROLL,
};

// Title shown on the middle "view" window. When the song row is focused
// (Shift+Tab), the middle title is overridden with "Song" regardless of the
// underlying NavState. Switch is exhaustive — adding a NavState value will
// trip -Wswitch.
std::string view_title(NavState ns, bool song_view_focused);

// ---- status strip --------------------------------------------------------

struct StatusItem {
    std::string action;   // e.g. "play"
    std::string key;      // e.g. "p"
};

// "?: help" is reserved on the far right. Caller never includes it.
inline constexpr const char* HELP_TOKEN = "?: help";

// Highest-priority keys, in render order. Fixed across views.
const std::vector<StatusItem>& global_priority_items();

// Context-specific items for the active view, in render order.
std::vector<StatusItem> context_items_for(NavState ns, bool song_view_focused);

// Priority items followed by context items.
std::vector<StatusItem> status_items_for(NavState ns, bool song_view_focused);

// Compose a single-line status strip exactly `width` columns wide.
//
//   <action>: <key> | <action>: <key> | …            ?: help
//
// Items are kept greedily left-to-right; if any are dropped, an ellipsis
// token "…" is appended to indicate truncation. "?: help" is always pinned
// to the far right — even at minimum viable width it stays visible.
std::string compose_status_strip(const std::vector<StatusItem>& items, int width);

