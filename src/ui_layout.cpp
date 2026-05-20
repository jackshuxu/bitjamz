#include "ui_layout.h"

#include <string>
#include <string_view>

// ---- dynamic title -------------------------------------------------------

std::string view_title(NavState ns, bool song_view_focused) {
    if (song_view_focused) return "Song";
    switch (ns) {
        case NavState::GRID:        return "Sequencer";
        case NavState::PIANO_ROLL:  return "Piano Roll";
        case NavState::KEYBOARD:    return "Keyboard";
        case NavState::PARAM_PAGE:  return "Synth";
    }
    return "Sequencer"; // unreachable; switch is exhaustive.
}

// ---- status strip --------------------------------------------------------

const std::vector<StatusItem>& global_priority_items() {
    static const std::vector<StatusItem> kItems = {
        {"play",  "p"},
        {"rec",   "q"},
        {"copy",  "C"},
        {"paste", "V"},
    };
    return kItems;
}

std::vector<StatusItem> context_items_for(NavState ns, bool song_view_focused) {
    if (song_view_focused) {
        return {
            {"move",     "←→"},
            {"pid",      "0-9"},
            {"seq view", "shift+tab"},
        };
    }
    switch (ns) {
        case NavState::GRID:
            return {
                {"toggle", "spc"},
                {"move",   "←→↑↓"},
                {"fill",   "1-9"},
                {"del",    "bksp"},
                {"track",  "rtyuvbnmfghj"},
                {"piano",  "enter"},
                {"kbd",    "k"},
                {"step",   "s"},
                {"param",  "tab"},
                {"solo",   "S"},
                {"mute",   "M"},
                {"new",    "+"},
                {"dup",    "*"},
                {"loop",   "L"},
                {"follow", "F"},
                {"bpm",    "=/-"},
                {"bars",   "[/]"},
                {"sig",    ",/."},
                {"metro",  "\\"},
                {"quit",   "Q"},
            };
        case NavState::PIANO_ROLL:
            return {
                {"place/del", "spc"},
                {"move",      "←→↑↓"},
                {"oct",       "z/x"},
                {"jam",       "n"},
                {"back",      "esc"},
                {"bpm",       "=/-"},
                {"metro",     "\\"},
                {"quit",      "Q"},
            };
        case NavState::KEYBOARD:
            return {
                {"pitch",  "awsedftgyhujkol"},
                {"resize", "←→"},
                {"oct",    "z/x"},
                {"jam",    "n"},
                {"del",    "bksp"},
                {"piano",  "enter"},
                {"param",  "tab"},
                {"back",   "esc"},
                {"solo",   "S"},
                {"mute",   "M"},
                {"clr",    "D"},
                {"clr all", "AD"},
                {"fill",   "1-9"},
                {"bpm",    "=/-"},
                {"metro",  "\\"},
                {"quit",   "Q"},
            };
        case NavState::PARAM_PAGE:
            return {
                {"osc",   "o"},
                {"root",  "[/]"},
                {"back",  "tab"},
                {"bpm",   "=/-"},
                {"metro", "\\"},
                {"quit",  "Q"},
            };
    }
    return {};
}

std::vector<StatusItem> status_items_for(NavState ns, bool song_view_focused) {
    std::vector<StatusItem> out = global_priority_items();
    for (auto& it : context_items_for(ns, song_view_focused)) {
        out.push_back(std::move(it));
    }
    return out;
}

namespace {

// Display width: count Unicode codepoints (treat each multibyte sequence as
// one column). Sufficient for our tokens, which only contain ASCII plus the
// ellipsis "…".
int dwidth(std::string_view s) {
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

std::string token_of(const StatusItem& it) {
    return it.action + ": " + it.key;
}

} // namespace

std::string compose_status_strip(const std::vector<StatusItem>& items, int width) {
    constexpr std::string_view SEP = " | ";
    constexpr std::string_view ELL = "…";
    const std::string help = HELP_TOKEN;
    const int help_w = dwidth(help);

    if (width <= 0) return "";
    if (width <= help_w) {
        // Tightest possible strip: just "?: help" (truncated if needed).
        std::string out = help;
        if ((int)out.size() > width) out.resize(width);
        return out;
    }

    // Reserve at least one column of gap between the left chunk and help.
    const int left_budget = width - help_w - 1;

    std::string left;
    int left_w = 0;
    size_t kept = 0;
    for (; kept < items.size(); ++kept) {
        std::string tok = token_of(items[kept]);
        int sep_w = left.empty() ? 0 : (int)SEP.size();
        int add   = sep_w + dwidth(tok);
        if (left_w + add > left_budget) break;
        if (!left.empty()) left += SEP;
        left += tok;
        left_w += add;
    }

    bool truncated = kept < items.size();
    if (truncated) {
        // Try to append " | …" (or "…" if nothing fit yet). If that does not
        // fit either, drop trailing tokens until it does.
        const int ell_w = dwidth(ELL);
        while (true) {
            int sep_w = left.empty() ? 0 : (int)SEP.size();
            if (left_w + sep_w + ell_w <= left_budget) {
                if (!left.empty()) left += SEP;
                left += ELL;
                left_w += sep_w + ell_w;
                break;
            }
            if (left.empty()) {
                // Help token alone fits; ellipsis does not. Drop the
                // ellipsis and just render help, right-aligned.
                break;
            }
            // Drop trailing token.
            size_t pos = left.rfind(" | ");
            if (pos == std::string::npos) {
                left.clear();
                left_w = 0;
            } else {
                left_w -= dwidth(std::string_view(left).substr(pos));
                left.erase(pos);
            }
        }
    }

    // Pad with spaces and append help.
    int pad = width - help_w - left_w;
    if (pad < 1) pad = 1;
    std::string out;
    out.reserve(width);
    out += left;
    out.append(pad, ' ');
    out += help;
    return out;
}

