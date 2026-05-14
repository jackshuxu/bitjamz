#include "ui.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "audio.h"
#include "session.h"

using namespace ftxui;

// ---- nav state ------------------------------------------------------------

enum class NavState { GRID, KEYBOARD, PARAM_PAGE, PIANO_ROLL };
static NavState nav_state = NavState::GRID;
static bool     seq_mode  = false;  // GRID sub-mode: 8-step window for drum cell toggles

static int cursor_track = 0;
static int cursor_step  = 0;
static int window_start = 0;

// keyboard mode
static int  kbd_octave           = 4;
static int  kbd_active_track     = -1;   // melodic track whose active note is held
static int  kbd_active_note_idx  = -1;   // index into melodic_notes[]
// Global edit-vs-jam toggle for the synth views (keyboard mode + piano roll).
// true: pitch / spacebar input records into the sequencer. false: synth-only.
static bool note_mode            = true;

// piano roll mode. View walks MIDI space by 1 semitone at a time; the
// visible window is 12 rows starting at piano_view_base_midi (bottom row).
static int  piano_track            = -1;
static int  piano_view_base_midi   = 60;  // MIDI of bottom-rendered row
static int  piano_cursor_step      = 0;
static int  piano_cursor_midi      = 60;  // absolute MIDI of cursor pitch
static int  piano_active_note_idx  = -1;
static Note piano_clipboard        = {};
static bool piano_clipboard_set    = false;

// param page (oscillator + scale_snap are global; root pitch is per-track)
enum class OscType { SQUARE, SAW, TRIANGLE, SINE };
static constexpr int OSC_COUNT = 4;
static const char* OSC_NAMES[OSC_COUNT] = { "SQUARE", "SAW", "TRIANGLE", "SINE" };
static OscType synth_osc  = OscType::SQUARE;
static bool    scale_snap = false;

// ---- music helpers --------------------------------------------------------

// piano-keyboard key -> semitone offset from C in current octave (0..14)
static int key_to_semitone(char c) {
    switch (c) {
        case 'a': return 0;   case 'w': return 1;
        case 's': return 2;   case 'e': return 3;
        case 'd': return 4;   case 'f': return 5;
        case 't': return 6;   case 'g': return 7;
        case 'y': return 8;   case 'h': return 9;
        case 'u': return 10;  case 'j': return 11;
        case 'k': return 12;  case 'o': return 13;
        case 'l': return 14;
        default:  return -1;
    }
}

static bool in_pentatonic(int st) {
    int s = ((st % 12) + 12) % 12;
    return s == 0 || s == 3 || s == 5 || s == 7 || s == 10;
}

static const int PENTA_MAP[12] = {0,0,3,3,3,5,5,7,7,10,10,10};
static int snap_pentatonic(int abs_semi) {
    int oct = abs_semi / 12;
    int s   = abs_semi % 12;
    if (s < 0) { s += 12; --oct; }
    return oct * 12 + PENTA_MAP[s];
}

struct NoteName {
    char letter;
    char accidental;   // '_' natural, '#' sharp, 'b' flat (we only emit '_' or '#')
    int  octave;
};

static NoteName note_name(uint8_t midi_pitch) {
    static const char LETTERS[12]   = {'C','C','D','D','E','F','F','G','G','A','A','B'};
    static const char ACCIDENTAL[12] = {'_','#','_','#','_','_','#','_','#','_','#','_'};
    int m = static_cast<int>(midi_pitch);
    int oct = m / 12 - 1;
    int s   = m % 12;
    if (oct < 0) oct = 0;
    if (oct > 9) oct = 9;
    return { LETTERS[s], ACCIDENTAL[s], oct };
}

// MPC key -> track index, or -1
static int track_for_mpc_key(char c) {
    for (int t = 0; t < TRACKS; ++t)
        if (TRACK_DEFS[t].key == c) return t;
    return -1;
}

// ---- color palette --------------------------------------------------------

static const Color COL_PURPLE = Color::RGB(125,  86, 244);
static const Color COL_DIM    = Color::RGB( 60,  40, 120);
static const Color COL_BRIGHT = Color::RGB(200, 180, 255);
static const Color COL_HEAD   = Color::RGB(255, 220, 100);
static const Color COL_GREEN  = Color::RGB( 80, 220, 120);

// ---- cursor / track navigation -------------------------------------------

static void cycle_cursor_to_active(SessionState& /*s*/, int dir) {
    cursor_track = (cursor_track + dir + TRACKS) % TRACKS;
}

static void ensure_cursor_visible(SessionState& /*s*/) {
    // Every track is always visible now; cursor is always valid.
}

// Returns true if any track in the session is soloed.
static bool any_solo(const SessionState& s) {
    for (int t = 0; t < TRACKS; ++t) if (s.track_solo[t]) return true;
    return false;
}

// Whether track t is effectively silenced for this peer right now.
static bool track_is_silenced(const SessionState& s, int t) {
    if (s.track_muted[t]) return true;
    if (any_solo(s) && !s.track_solo[t]) return true;
    return false;
}

// ---- visualizer (unchanged) ----------------------------------------------

static constexpr int VIS_W = 3 + STEPS * 3;
static constexpr int VIS_H = 7;

static Element render_visualizer() {
    static float vis_grid[VIS_H][VIS_W] = {};

    for (int row = 0; row < VIS_H; ++row)
        for (int col = 0; col < VIS_W; ++col)
            vis_grid[row][col] *= 0.68f;

    int wp    = vis_wp.load(std::memory_order_acquire);
    int count = (SAMPLE_RATE / 30) * 2;
    if (count > VIS_BUF) count = VIS_BUF;

    static constexpr float MID_SCALE  = 1.f / 0.50f;
    static constexpr float SIDE_SCALE = 1.f / 0.15f;

    for (int i = 0; i < count; i += 2) {
        const auto& s = vis_buf[(wp - i + VIS_BUF) & (VIS_BUF - 1)];
        float mid  = (s.l + s.r) * MID_SCALE;
        float side = (s.l - s.r) * SIDE_SCALE;
        int gx = (int)((mid  + 1.f) * 0.5f * (VIS_W - 1));
        int gy = (int)((side + 1.f) * 0.5f * (VIS_H - 1));
        if ((unsigned)gx < (unsigned)VIS_W && (unsigned)gy < (unsigned)VIS_H)
            vis_grid[gy][gx] = std::min(1.f, vis_grid[gy][gx] + 0.12f);
    }

    Elements rows;
    for (int row = 0; row < VIS_H; ++row) {
        Elements cells;
        for (int col = 0; col < VIS_W; ++col) {
            float v = vis_grid[row][col];
            if      (v > 0.65f) cells.push_back(text("█") | color(COL_BRIGHT));
            else if (v > 0.30f) cells.push_back(text("•") | color(COL_PURPLE));
            else if (v > 0.08f) cells.push_back(text("·") | color(COL_DIM));
            else                cells.push_back(text(" "));
        }
        rows.push_back(hbox(std::move(cells)));
    }
    rows.push_back(text(""));
    return vbox(std::move(rows));
}

// ---- main-grid rendering --------------------------------------------------

// Title column: 10 chars exactly. `[name](k) ` with name right-padded to 5.
static constexpr int TITLE_COL_WIDTH = 10;

static Element render_track_title(int t, bool focused, bool silenced) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "[%-5s](%c)",
                  TRACK_DEFS[t].name, TRACK_DEFS[t].key);
    Color col = silenced
                ? COL_DIM
                : (TRACK_DEFS[t].type == TrackType::MELODIC ? COL_GREEN : COL_PURPLE);
    Element e = text(buf) | color(col);
    if (focused) e = e | inverted | bold;
    return e;
}

// Draw a 5-char drum cell. `narrow` collapses to 3 chars for sub-90col terms.
// Active hits render the bare ■ glyph in bright+bold so the rhythmic content
// stands out without changing layout width.
static Element draw_drum_cell(bool hit, bool highlighted, bool in_loop,
                              bool on_playhead, bool playing, bool narrow) {
    (void)on_playhead; (void)playing;
    const char* glyph_full = hit ? "  ■  " : "  ·  ";
    const char* glyph_thin = hit ? " ■ "   : " · ";
    const char* g = narrow ? glyph_thin : glyph_full;
    if (!in_loop)    return text(narrow ? " · " : "  ·  ") | color(COL_DIM);
    if (highlighted) return text(g) | color(COL_PURPLE) | inverted;
    if (hit)         return text(g) | color(COL_BRIGHT)  | bold;
    return text(g) | color(COL_DIM);
}

// Draw a 5-char melodic cell given the projection info.
// kind 0=silence, 1=start (single-step or starts >=2), 2=middle, 3=end
static Element draw_melodic_cell(int kind, const Note* n, bool single_step,
                                 bool highlighted, bool in_loop,
                                 bool on_playhead, bool playing, bool narrow) {
    if (!in_loop) return text(narrow ? " · " : "  ·  ") | color(COL_DIM);

    char buf[8];
    if (kind == 0 || kind == 2) {
        std::snprintf(buf, sizeof(buf), "%s", narrow ? " · " : "  ·  ");
    } else if (kind == 1 && n) {
        NoteName nm = note_name(n->pitch_midi);
        char close = single_step ? ']' : ' ';
        if (narrow) {
            std::snprintf(buf, sizeof(buf), "[%c%c", nm.letter, close);
        } else {
            std::snprintf(buf, sizeof(buf), "[%c%c%d%c",
                          nm.letter, nm.accidental, nm.octave, close);
        }
    } else if (kind == 3) {
        std::snprintf(buf, sizeof(buf), "%s", narrow ? " ·]" : "  · ]");
    } else {
        std::snprintf(buf, sizeof(buf), "%s", narrow ? "   " : "     ");
    }

    Element e = text(buf);
    if (highlighted)               e = e | color(COL_PURPLE) | inverted;
    else if (kind == 0)            e = e | color(COL_DIM);
    else                            e = e | color(COL_GREEN);
    if (on_playhead && playing && kind != 0) e = e | bold;
    return e;
}

static Element render_grid_view(SessionState& s) {
    int  ps         = s.play_step.load();
    bool is_playing = s.playing.load();

    // Width-aware fallback: target 10 + 16*5 = 90 cols for the full row.
    // ftxui doesn't tell us screen width inside a Renderer; the fallback is
    // mostly cosmetic, so we always render full-width here. Narrow terms
    // wrap. (Per PRD §17 we'd flip this on a screen-width probe; deferred.)
    const bool narrow = false;
    const int cell_w  = narrow ? 3 : 5;

    Elements lines;

    // header
    {
        char head_buf[80];
        std::snprintf(head_buf, sizeof(head_buf), "  bpm: %d  steps: %d  %s",
                      s.bpm, s.loop_len, is_playing ? "▶" : "■");

        Elements header = {
            text("bitjams") | bold | color(COL_PURPLE),
            text(head_buf)         | color(COL_PURPLE),
        };

        // Metronome glyph next to BPM (bright when armed, dim when off).
        bool metro_on = s.metronome_enabled.load();
        Element metro_glyph = metro_on
            ? (text(" ♩") | color(COL_BRIGHT) | bold)
            : (text(" ♩") | color(COL_DIM));
        header.push_back(metro_glyph);

        // Mode label area: REC > COUNTDOWN > existing nav label.
        auto rec = s.rec_state.load();
        if (rec == RecordState::RECORDING) {
            header.push_back(text("  ● REC") | color(Color::Red) | bold);
        } else if (rec == RecordState::COUNTDOWN) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "  %d", s.countdown_beat.load());
            header.push_back(text(buf) | color(Color::Red) | bold);
        } else {
            const char* mode_label =
                (nav_state == NavState::PARAM_PAGE) ? "  [param]" :
                (nav_state == NavState::KEYBOARD)   ? (note_mode ? "  [kbd]" : "  [kbd jam]") :
                (nav_state == NavState::PIANO_ROLL) ? "  [piano]" :
                (seq_mode)                          ? "  [seq]"   :
                                                      "  [step]";
            header.push_back(text(mode_label) | color(COL_DIM));
        }
        if (s.session_id != 0) {
            std::string room_label = "  ROOM " + std::to_string(s.session_id);
            if (s.is_joiner && !s.network_alive.load()) {
                room_label += " — solo (host left)";
            }
            header.push_back(text(room_label) | color(COL_DIM));
        }
        lines.push_back(hbox(std::move(header)));
    }

    // step numbers — prefix is TITLE_COL_WIDTH + 1 to match the title+spacer
    // layout of each track row, so step labels align with the playhead glyph.
    {
        Elements row;
        row.push_back(text(std::string(TITLE_COL_WIDTH + 1, ' ')));
        for (int step = 0; step < STEPS; ++step) {
            char buf[8];
            if (step < s.loop_len) {
                std::snprintf(buf, sizeof(buf),
                              narrow ? "%2d " : " %2d  ", step + 1);
            } else {
                std::snprintf(buf, sizeof(buf), "%s", narrow ? "   " : "     ");
            }
            row.push_back(text(buf) | color(COL_BRIGHT));
        }
        lines.push_back(hbox(std::move(row)));
    }

    // playhead arrow
    {
        Elements row;
        row.push_back(text(std::string(TITLE_COL_WIDTH + 1, ' ')));
        for (int step = 0; step < STEPS; ++step) {
            const char* glyph;
            if (step == ps && is_playing) glyph = narrow ? " ▼ " : "  ▼  ";
            else                          glyph = narrow ? "   " : "     ";
            Element e = text(glyph);
            if (step == ps && is_playing) e = e | color(COL_HEAD);
            row.push_back(e);
        }
        lines.push_back(hbox(std::move(row)));
    }

    auto render_track_row = [&](int t) -> Element {
        Elements row;
        row.push_back(render_track_title(t, false, track_is_silenced(s, t)));
        row.push_back(text(" "));   // spacer between title and first cell

        const TrackDef& td = TRACK_DEFS[t];
        bool is_cursor_track = (t == cursor_track);

        // Show cursor highlight in both GRID and KEYBOARD modes (kbd-mode
        // edits land at the cursor). PARAM_PAGE/PIANO_ROLL get their own
        // dedicated render, so we never reach this path in those states.
        bool show_cursor = (nav_state == NavState::GRID
                         || nav_state == NavState::KEYBOARD);

        if (td.type == TrackType::DRUM) {
            int dk = td.drum_kind;
            for (int step = 0; step < STEPS; ++step) {
                bool active = s.drum_grid[dk][step];
                bool in_loop = (step < s.loop_len);
                // seq_mode is a GRID sub-mode only.
                bool highlighted = show_cursor
                                && is_cursor_track
                                && (seq_mode && nav_state == NavState::GRID
                                    ? (step >= window_start && step < window_start + 8)
                                    : (step == cursor_step));
                row.push_back(draw_drum_cell(active, highlighted, in_loop,
                                             step == ps, is_playing, narrow));
            }
        } else {
            const auto& notes = s.melodic_notes[td.melodic_idx];
            auto proj = project_row(notes, s.loop_len);
            for (int step = 0; step < STEPS; ++step) {
                bool in_loop = (step < s.loop_len);
                bool highlighted = show_cursor
                                && is_cursor_track
                                && (step == cursor_step);
                if (!in_loop || proj[step].note_idx < 0) {
                    row.push_back(draw_melodic_cell(0, nullptr, false,
                                                    highlighted, in_loop,
                                                    step == ps, is_playing, narrow));
                    continue;
                }
                const Note& n = notes[proj[step].note_idx];
                bool single = (n.duration_steps == 1);
                int kind;
                if (proj[step].is_start && (single || proj[step].is_end)) kind = 1;
                else if (proj[step].is_start)                              kind = 1;
                else if (proj[step].is_end)                                kind = 3;
                else                                                       kind = 2;
                row.push_back(draw_melodic_cell(kind, &n, single,
                                                highlighted, in_loop,
                                                step == ps, is_playing, narrow));
            }
        }

        // Unused so the compiler doesn't complain on a narrow=false build.
        (void)cell_w;
        return hbox(std::move(row));
    };

    for (int t = 0; t < TRACKS; ++t) lines.push_back(render_track_row(t));

    lines.push_back(text(""));

    // footer hints
    const char* hint;
    if (nav_state == NavState::KEYBOARD) {
        hint = note_mode
            ? "  pitch:awsedftgyhujkol  ←→:resize  spc:release  z/x:oct  n:note(rec)  "
              "bksp:del  S:solo  M:mute  C:copy  V:paste  D:clr  AD:clr all  1-9:fill  "
              "q:rec  \\:metro  tab:param  enter:piano  esc:back  Q:quit"
            : "  pitch:awsedftgyhujkol (jam — synth only, not recorded)  z/x:oct  "
              "n:note(jam)  q:rec  \\:metro  tab:param  enter:piano  esc:back  Q:quit";
    } else if (seq_mode) {
        hint = "  1-8:toggle step  ←→:window  ↑↓:track  rtyuvbnmfghj:trigger+focus  "
               "s:exit step  bksp:del  S:solo  M:mute  C:copy  V:paste  D:clr track  AD:clr all  "
               "q:rec  \\:metro  p:play  Q:quit";
    } else {
        hint = "  spc:toggle/note  1-9:fill  ←→↑↓:move  bksp:del  rtyuvbnmfghj:trigger+focus  "
               "tab:param  enter:piano  k:kbd  s:seq  S:solo  M:mute  C:copy  V:paste  D:clr track  AD:clr all  "
               "q:rec  \\:metro  p:play  Q:quit";
    }
    lines.push_back(text(hint) | color(COL_DIM));

    return vbox(std::move(lines));
}

// ---- piano roll -----------------------------------------------------------

static Element render_piano_roll(SessionState& s) {
    Elements lines;

    char head_buf[120];
    const TrackDef& td = TRACK_DEFS[piano_track];
    NoteName cur_nm = note_name(static_cast<uint8_t>(piano_cursor_midi));
    std::snprintf(head_buf, sizeof(head_buf),
                  "piano roll: %-5s  bpm %d  steps %d  %s  cursor %c%c%d  [esc: back]",
                  td.name, s.bpm, s.loop_len,
                  s.playing.load() ? "▶" : "■",
                  cur_nm.letter, cur_nm.accidental, cur_nm.octave);
    lines.push_back(text(head_buf) | color(COL_PURPLE) | bold);
    lines.push_back(text(""));

    // step number row
    {
        Elements row;
        row.push_back(text(std::string(8, ' ')) | color(COL_BRIGHT));
        for (int step = 0; step < STEPS; ++step) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), " %2d  ", step + 1);
            row.push_back(text(buf) | color(COL_BRIGHT));
        }
        lines.push_back(hbox(std::move(row)));
    }

    // playhead arrow
    {
        Elements row;
        row.push_back(text(std::string(8, ' ')));
        int ps = s.play_step.load();
        for (int step = 0; step < STEPS; ++step) {
            const char* glyph = (step == ps && s.playing.load()) ? "  ▼  " : "     ";
            Element e = text(glyph);
            if (step == ps && s.playing.load()) e = e | color(COL_HEAD);
            row.push_back(e);
        }
        lines.push_back(hbox(std::move(row)));
    }

    const auto& notes = s.melodic_notes[td.melodic_idx];

    // 12 chromatic pitch rows starting at piano_view_base_midi at the bottom.
    // Render top-down: row 11 (highest) first, row 0 last.
    for (int row_idx = 11; row_idx >= 0; --row_idx) {
        Elements row;
        uint8_t midi = static_cast<uint8_t>(piano_view_base_midi + row_idx);
        NoteName nm = note_name(midi);
        char lbl[12];
        std::snprintf(lbl, sizeof(lbl), "  %c%c%d   ",
                      nm.letter, nm.accidental, nm.octave);
        row.push_back(text(lbl) | color(COL_DIM));

        for (int step = 0; step < STEPS; ++step) {
            bool cursor_here = (midi == piano_cursor_midi && step == piano_cursor_step);
            bool in_loop = (step < s.loop_len);

            int hit = -1;
            for (size_t i = 0; i < notes.size(); ++i) {
                const Note& n = notes[i];
                if (n.pitch_midi != midi) continue;
                int end = n.start_step + n.duration_steps;
                if (step >= n.start_step && step < end) { hit = static_cast<int>(i); break; }
            }

            // All glyphs anchor at col 1 of the 5-char cell so dots and note
            // boundaries align vertically across pitch rows. Start: `[■   `
            // (■ at col 1), end: ` ─]  ` (─ at col 1, ] at col 2), middle:
            // ` ─   ` (─ at col 1), empty: ` ·   ` (· at col 1), single
            // step: `[■]  ` ([ at col 0, ■ at col 1, ] at col 2).
            const char* glyph;
            if (hit < 0) {
                glyph = in_loop ? " ·   " : "     ";
            } else {
                const Note& n = notes[hit];
                bool is_start = (step == n.start_step);
                bool is_end   = (step == n.start_step + n.duration_steps - 1);
                if (is_start && is_end) glyph = "[■]  ";
                else if (is_start)       glyph = "[■   ";
                else if (is_end)         glyph = " ─]  ";
                else                     glyph = " ─   ";
            }
            Element e;
            if (cursor_here) {
                e = text(glyph) | color(COL_PURPLE) | inverted;
            } else if (hit >= 0) {
                e = text(glyph) | color(COL_GREEN);
            } else if (in_loop) {
                e = text(glyph) | color(COL_DIM);
            } else {
                e = text(glyph);
            }
            row.push_back(e);
        }
        lines.push_back(hbox(std::move(row)));
    }

    lines.push_back(text(""));
    {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "  ←→ step  ↑↓ pitch  space:%s  c/v:copy/paste  "
                      "z/x:oct  n:note(%s)  q:rec  \\:metro  esc:back  Q:quit",
                      note_mode ? "add/release" : "audition",
                      note_mode ? "rec" : "jam");
        lines.push_back(text(buf) | color(COL_DIM));
    }
    return vbox(std::move(lines));
}

// ---- param page (renamed melodic synth page; no piano keys) --------------

static Element render_drum_param_page(SessionState& s, int t) {
    const TrackDef& td = TRACK_DEFS[t];
    const DrumParams& p = DRUM_PARAMS[td.drum_kind];

    Elements lines;

    char head_buf[80];
    std::snprintf(head_buf, sizeof(head_buf), "  bpm: %d  steps: %d  %s  ",
                  s.bpm, s.loop_len, s.playing.load() ? "▶" : "■");
    lines.push_back(hbox({
        text("bitjams") | bold | color(COL_PURPLE),
        text(head_buf)         | color(COL_PURPLE),
        text("[drum param]")   | color(COL_DIM),
    }));

    char title[64];
    std::snprintf(title, sizeof(title), "  %s  (key: %c)  procedural drum",
                  p.label, td.key);
    lines.push_back(text(title) | bold | color(COL_BRIGHT));
    lines.push_back(text(""));

    auto param_row = [&](const char* label, const std::string& val) {
        return hbox({
            text("    "),
            text(std::string(label) + ": ") | color(COL_DIM),
            text(val) | color(COL_BRIGHT),
        });
    };

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f Hz", p.base_pitch_hz);
    lines.push_back(param_row("base pitch", buf));
    std::snprintf(buf, sizeof(buf), "%.3f s", p.decay_seconds);
    lines.push_back(param_row("decay", buf));
    std::snprintf(buf, sizeof(buf), "%.2f", p.noise_mix);
    lines.push_back(param_row("noise mix", buf));
    NoteName nm = note_name(s.track_root_midi[t]);
    char root_buf[8];
    std::snprintf(root_buf, sizeof(root_buf), "%c%c%d", nm.letter, nm.accidental, nm.octave);
    lines.push_back(param_row("root midi", root_buf));

    lines.push_back(text(""));
    lines.push_back(text("  z/x:root ▼▲   tab/esc:back  q:rec  \\:metro  p:play  Q:quit")
                    | color(COL_DIM));
    return vbox(std::move(lines));
}

static Element render_melodic_param_page(SessionState& s, int t) {
    const TrackDef& td = TRACK_DEFS[t];

    Elements lines;

    char head_buf[80];
    std::snprintf(head_buf, sizeof(head_buf), "  bpm: %d  steps: %d  %s  ",
                  s.bpm, s.loop_len, s.playing.load() ? "▶" : "■");
    lines.push_back(hbox({
        text("bitjams")     | bold | color(COL_PURPLE),
        text(head_buf)             | color(COL_PURPLE),
        text("[param]")            | color(COL_DIM),
    }));

    char title[64];
    std::snprintf(title, sizeof(title), "  %s  (key: %c)  oscillator voice",
                  td.name, td.key);
    lines.push_back(text(title) | bold | color(COL_GREEN));

    {
        NoteName nm = note_name(s.track_root_midi[t]);
        char osc_buf[16];
        std::snprintf(osc_buf, sizeof(osc_buf), "%-8s", OSC_NAMES[(int)synth_osc]);
        char root_buf[8];
        std::snprintf(root_buf, sizeof(root_buf), "%c%c%d",
                      nm.letter, nm.accidental, nm.octave);
        lines.push_back(hbox({
            text(" osc ")     | color(COL_DIM),
            text("◄ ")        | color(COL_DIM),
            text(osc_buf)     | bold | color(COL_BRIGHT),
            text("►")         | color(COL_DIM),
            text("  root ")   | color(COL_DIM),
            text(root_buf)    | color(COL_BRIGHT),
        }));
    }

    lines.push_back(text(""));
    lines.push_back(text("  ←→:osc  z/x:root ▼▲  tab/esc:back  q:rec  \\:metro  p:play  Q:quit")
                    | color(COL_DIM));
    return vbox(std::move(lines));
}

// ---- live trigger (MPC pad) ----------------------------------------------

static void trigger_track_live(SessionState& s, int t) {
    if (track_is_silenced(s, t)) return;
    if (TRACK_DEFS[t].type == TrackType::MELODIC) {
        enqueue_synth_trig(TRACK_DEFS[t].melodic_idx, midi_to_hz(s.track_root_midi[t]));
    } else {
        s.trig[t].store(true);
    }
}

// ---- keyboard mode helpers -----------------------------------------------

static void kbd_release_active() {
    kbd_active_track    = -1;
    kbd_active_note_idx = -1;
}

static void kbd_mark_dirty(SessionState& s, int melodic_idx) {
    s.melodic_dirty[melodic_idx].store(true);
}

// Resolve the active note's stable identity (start_step + pitch) into a
// current index — the index may have shifted if other notes were deleted.
static int kbd_resolve_active_index(SessionState& s) {
    if (kbd_active_track < 0 || kbd_active_note_idx < 0) return -1;
    int midx = TRACK_DEFS[kbd_active_track].melodic_idx;
    if (kbd_active_note_idx < (int)s.melodic_notes[midx].size())
        return kbd_active_note_idx;
    return -1;
}

// Place a 1-step note at cursor_step on the keyboard-mode cursor track.
// Returns the new active index, or -1 on failure (drum track / cap).
static int kbd_place_note(SessionState& s, uint8_t pitch_midi) {
    if (TRACK_DEFS[cursor_track].type != TrackType::MELODIC) return -1;
    int midx = TRACK_DEFS[cursor_track].melodic_idx;
    Note n{ static_cast<uint8_t>(cursor_step), 1, pitch_midi, 127 };
    if (!add_note(s.melodic_notes[midx], n)) return -1;
    kbd_mark_dirty(s, midx);
    return static_cast<int>(s.melodic_notes[midx].size() - 1);
}

// Truncate the held note to end just before cursor_step. Caller advances
// cursor before invoking this for the "mid-note overwrite" flow. Held notes
// never shrink below 1 step: if the cursor is still on the start cell, the
// note keeps its single starting cell so a rapid two-key sequence places the
// second note in the next cell instead of overwriting the first.
static void kbd_truncate_active_before(SessionState& s, int new_end_step) {
    int idx = kbd_resolve_active_index(s);
    if (idx < 0) return;
    int midx = TRACK_DEFS[kbd_active_track].melodic_idx;
    Note& n = s.melodic_notes[midx][idx];
    int new_dur = new_end_step - n.start_step;
    if (new_dur < 1) new_dur = 1;
    n.duration_steps = static_cast<uint8_t>(new_dur);
    kbd_mark_dirty(s, midx);
}

// ---- piano-roll helpers --------------------------------------------------

static uint8_t pr_cursor_pitch() {
    return static_cast<uint8_t>(piano_cursor_midi);
}

// Keep piano_cursor_midi inside the visible 12-row window by scrolling the
// viewport one semitone at a time.
static void pr_clamp_view() {
    if (piano_cursor_midi < piano_view_base_midi)
        piano_view_base_midi = piano_cursor_midi;
    else if (piano_cursor_midi > piano_view_base_midi + 11)
        piano_view_base_midi = piano_cursor_midi - 11;
    if (piano_view_base_midi < 0)   piano_view_base_midi = 0;
    if (piano_view_base_midi > 116) piano_view_base_midi = 116;  // 116+11 = 127
}

static void pr_release_active() { piano_active_note_idx = -1; }

static int pr_place_note_at_cursor(SessionState& s, uint8_t duration_steps) {
    if (piano_track < 0) return -1;
    int midx = TRACK_DEFS[piano_track].melodic_idx;
    uint8_t pitch = pr_cursor_pitch();
    // Apply same-row collision rule before adding.
    clear_or_truncate_at(s.melodic_notes[midx], pitch,
                         static_cast<uint8_t>(piano_cursor_step));
    Note n{ static_cast<uint8_t>(piano_cursor_step), duration_steps, pitch, 127 };
    if (!add_note(s.melodic_notes[midx], n)) return -1;
    s.melodic_dirty[midx].store(true);
    return static_cast<int>(s.melodic_notes[midx].size() - 1);
}

// ---- event dispatch ------------------------------------------------------

static bool dispatch_param_page(SessionState& s, Event e) {
    if (e == Event::Tab || e == Event::Escape) {
        nav_state = NavState::GRID;
        return true;
    }
    if (TRACK_DEFS[cursor_track].type == TrackType::MELODIC) {
        if (e == Event::ArrowLeft) {
            int o = ((int)synth_osc - 1 + OSC_COUNT) % OSC_COUNT;
            synth_osc = (OscType)o;
            synth_osc_atom.store(o);
            return true;
        }
        if (e == Event::ArrowRight) {
            int o = ((int)synth_osc + 1) % OSC_COUNT;
            synth_osc = (OscType)o;
            synth_osc_atom.store(o);
            return true;
        }
    }
    if (e.is_character() && e.character().size() == 1) {
        char c = e.character()[0];
        if (c == 'z') {
            if (s.track_root_midi[cursor_track] > 0) {
                --s.track_root_midi[cursor_track];
                s.track_root_dirty.fetch_or(static_cast<uint16_t>(1) << cursor_track);
            }
            return true;
        }
        if (c == 'x') {
            if (s.track_root_midi[cursor_track] < 127) {
                ++s.track_root_midi[cursor_track];
                s.track_root_dirty.fetch_or(static_cast<uint16_t>(1) << cursor_track);
            }
            return true;
        }
    }
    return false;
}

static void delete_at_cursor_cell(SessionState& s);
static bool handle_shared_track_command(SessionState& s, Event e);

static bool dispatch_keyboard(SessionState& s, Event e) {
    if (e == Event::Escape) {
        kbd_release_active();
        nav_state = NavState::GRID;
        return true;
    }
    if (e == Event::ArrowUp || e == Event::ArrowDown) {
        kbd_release_active();
        cycle_cursor_to_active(s, e == Event::ArrowUp ? -1 : +1);
        if (TRACK_DEFS[cursor_track].type != TrackType::MELODIC) {
            nav_state = NavState::GRID;
        }
        return true;
    }

    bool has_active = (kbd_resolve_active_index(s) >= 0);

    if (e == Event::ArrowRight) {
        if (has_active) {
            int idx = kbd_resolve_active_index(s);
            int midx = TRACK_DEFS[kbd_active_track].melodic_idx;
            Note& n = s.melodic_notes[midx][idx];
            if (n.duration_steps < s.loop_len) {
                ++n.duration_steps;
                s.melodic_dirty[midx].store(true);
            }
            if (cursor_step < s.loop_len - 1) ++cursor_step;
        } else {
            cursor_step = (cursor_step + 1) % s.loop_len;
        }
        return true;
    }
    if (e == Event::ArrowLeft) {
        if (has_active) {
            int idx = kbd_resolve_active_index(s);
            int midx = TRACK_DEFS[kbd_active_track].melodic_idx;
            Note& n = s.melodic_notes[midx][idx];
            if (n.duration_steps > 1) {
                --n.duration_steps;
                s.melodic_dirty[midx].store(true);
                if (cursor_step > 0) --cursor_step;
            } else {
                // At dur 1, escape from the held note (like ↑/↓) instead of
                // deleting it. The note stays put; cursor moves left normally.
                kbd_release_active();
                cursor_step = (cursor_step - 1 + s.loop_len) % s.loop_len;
            }
        } else {
            cursor_step = (cursor_step - 1 + s.loop_len) % s.loop_len;
        }
        return true;
    }
    if (e == Event::Character(' ')) {
        if (has_active) kbd_release_active();
        return true;
    }

    // Track-level commands (mix, clipboard, clear, fill, mode switches) work
    // identically here and in grid mode. Any of them releases the held note
    // first so we don't keep editing it through a mode change.
    if (handle_shared_track_command(s, e)) {
        kbd_release_active();
        return true;
    }

    if (e.is_character() && e.character().size() == 1) {
        char c = e.character()[0];
        if (c == 'z') { kbd_octave = std::max(0, kbd_octave - 1); return true; }
        if (c == 'x') { kbd_octave = std::min(9, kbd_octave + 1); return true; }
        if (c == 'n') {
            note_mode = !note_mode;
            // Leaving record mode while a note is held would orphan the hold
            // (Arrow keys would still grow it). Drop it so the user starts
            // jam mode clean.
            if (!note_mode) kbd_release_active();
            return true;
        }

        int semi = key_to_semitone(c);
        if (semi < 0) return true;  // suppress MPC pads and other letters

        int abs_semi = (kbd_octave - 4) * 12 + semi;
        if (scale_snap) abs_semi = snap_pentatonic(abs_semi);
        // MIDI 60 = C4 = abs_semi 0 baseline.
        int midi = 60 + abs_semi;
        if (midi < 0 || midi > 127) return true;

        if (TRACK_DEFS[cursor_track].type != TrackType::MELODIC) return true;

        if (s.rec_state.load() == RecordState::RECORDING) {
            // REC overrides note_mode. Capture at play_step; never start a
            // hold (cursor-edit + arrow-key resize stay out of the way).
            enqueue_synth_trig(TRACK_DEFS[cursor_track].melodic_idx,
                               midi_to_hz(static_cast<uint8_t>(midi)));
            record_input(s, cursor_track, static_cast<uint8_t>(midi));
            return true;
        }

        if (!note_mode) {
            // Jam mode: play the synth, never touch the sequencer.
            enqueue_synth_trig(TRACK_DEFS[cursor_track].melodic_idx,
                               midi_to_hz(static_cast<uint8_t>(midi)));
            return true;
        }

        if (has_active) {
            int idx  = kbd_resolve_active_index(s);
            int midx = TRACK_DEFS[kbd_active_track].melodic_idx;
            Note& n  = s.melodic_notes[midx][idx];
            if (cursor_step > n.start_step) {
                // Held note was grown via ArrowRight. Truncate it so it ends
                // just before the cursor, and place the new pitch at the
                // current cursor cell (don't advance).
                kbd_truncate_active_before(s, cursor_step);
            } else if (cursor_step < s.loop_len - 1) {
                // 1-cell held at cursor: keep it at dur 1, advance the cursor
                // so the new pitch lands in the next cell.
                ++cursor_step;
            } else {
                // 1-cell held at the last cell: cursor can't advance. Delete
                // the held note so the new pitch cleanly replaces it.
                s.melodic_notes[midx].erase(s.melodic_notes[midx].begin() + idx);
                kbd_mark_dirty(s, midx);
            }
            kbd_release_active();
        }

        int new_idx = kbd_place_note(s, static_cast<uint8_t>(midi));
        if (new_idx >= 0) {
            kbd_active_track    = cursor_track;
            kbd_active_note_idx = new_idx;
            // Fire audio immediately.
            enqueue_synth_trig(TRACK_DEFS[cursor_track].melodic_idx,
                               midi_to_hz(static_cast<uint8_t>(midi)));
        }
        return true;
    }
    return false;
}

static bool dispatch_piano_roll(SessionState& s, Event e) {
    if (e == Event::Escape) {
        pr_release_active();
        nav_state = NavState::GRID;
        return true;
    }
    if (piano_track < 0) {
        nav_state = NavState::GRID;
        return true;
    }

    if (e == Event::ArrowRight) {
        if (piano_active_note_idx >= 0) {
            int midx = TRACK_DEFS[piano_track].melodic_idx;
            if (piano_active_note_idx < (int)s.melodic_notes[midx].size()) {
                Note& n = s.melodic_notes[midx][piano_active_note_idx];
                if (n.duration_steps < s.loop_len) {
                    ++n.duration_steps;
                    s.melodic_dirty[midx].store(true);
                }
            }
            if (piano_cursor_step < s.loop_len - 1) ++piano_cursor_step;
        } else {
            piano_cursor_step = (piano_cursor_step + 1) % s.loop_len;
        }
        return true;
    }
    if (e == Event::ArrowLeft) {
        if (piano_active_note_idx >= 0) {
            int midx = TRACK_DEFS[piano_track].melodic_idx;
            if (piano_active_note_idx < (int)s.melodic_notes[midx].size()) {
                Note& n = s.melodic_notes[midx][piano_active_note_idx];
                if (n.duration_steps > 1) {
                    --n.duration_steps;
                    s.melodic_dirty[midx].store(true);
                } else {
                    s.melodic_notes[midx].erase(s.melodic_notes[midx].begin()
                                                + piano_active_note_idx);
                    s.melodic_dirty[midx].store(true);
                    pr_release_active();
                }
            }
            if (piano_cursor_step > 0) --piano_cursor_step;
        } else {
            piano_cursor_step = (piano_cursor_step - 1 + s.loop_len) % s.loop_len;
        }
        return true;
    }
    if (e == Event::ArrowUp) {
        pr_release_active();
        if (piano_cursor_midi < 127) ++piano_cursor_midi;
        pr_clamp_view();
        return true;
    }
    if (e == Event::ArrowDown) {
        pr_release_active();
        if (piano_cursor_midi > 0) --piano_cursor_midi;
        pr_clamp_view();
        return true;
    }
    if (e == Event::Backspace) {
        // Delete the note under the cursor.
        int midx = TRACK_DEFS[piano_track].melodic_idx;
        auto& notes = s.melodic_notes[midx];
        uint8_t pitch = pr_cursor_pitch();
        for (auto it = notes.begin(); it != notes.end(); ++it) {
            if (it->pitch_midi != pitch) continue;
            int end = it->start_step + it->duration_steps;
            if (piano_cursor_step >= it->start_step && piano_cursor_step < end) {
                notes.erase(it);
                pr_release_active();
                s.melodic_dirty[midx].store(true);
                break;
            }
        }
        return true;
    }
    if (e == Event::Character(' ')) {
        if (piano_active_note_idx >= 0) { pr_release_active(); return true; }
        if (!note_mode) {
            // Jam mode: audition the cursor pitch without recording.
            enqueue_synth_trig(TRACK_DEFS[piano_track].melodic_idx,
                               midi_to_hz(pr_cursor_pitch()));
            return true;
        }
        int new_idx = pr_place_note_at_cursor(s, 1);
        if (new_idx >= 0) {
            piano_active_note_idx = new_idx;
            enqueue_synth_trig(TRACK_DEFS[piano_track].melodic_idx,
                               midi_to_hz(pr_cursor_pitch()));
        }
        return true;
    }

    if (e.is_character() && e.character().size() == 1) {
        char c = e.character()[0];
        if (c == 'z') {
            piano_cursor_midi    = std::max(0,   piano_cursor_midi - 12);
            piano_view_base_midi = std::max(0,   piano_view_base_midi - 12);
            return true;
        }
        if (c == 'x') {
            piano_cursor_midi    = std::min(127, piano_cursor_midi + 12);
            piano_view_base_midi = std::min(116, piano_view_base_midi + 12);
            return true;
        }
        if (c == 'C') {
            // Shift+C clears the piano roll's entire pattern.
            int midx = TRACK_DEFS[piano_track].melodic_idx;
            s.melodic_notes[midx].clear();
            pr_release_active();
            s.melodic_dirty[midx].store(true);
            return true;
        }
        if (c == 'n') {
            note_mode = !note_mode;
            if (!note_mode) pr_release_active();
            return true;
        }
        if (c == 'c') {
            // Copy duration of the note under cursor (if any).
            int midx = TRACK_DEFS[piano_track].melodic_idx;
            uint8_t pitch = pr_cursor_pitch();
            for (const Note& n : s.melodic_notes[midx]) {
                if (n.pitch_midi != pitch) continue;
                int end = n.start_step + n.duration_steps;
                if (piano_cursor_step >= n.start_step && piano_cursor_step < end) {
                    piano_clipboard = n;
                    piano_clipboard_set = true;
                    break;
                }
            }
            return true;
        }
        if (c == 'v') {
            uint8_t dur = piano_clipboard_set ? piano_clipboard.duration_steps : 1;
            int new_idx = pr_place_note_at_cursor(s, dur);
            if (new_idx >= 0) {
                piano_active_note_idx = -1;  // paste does not start a hold
                enqueue_synth_trig(TRACK_DEFS[piano_track].melodic_idx,
                                   midi_to_hz(pr_cursor_pitch()));
            }
            return true;
        }
        // pitch keys: absolute C4-based mapping. `a` = C4 (MIDI 60) always,
        // independent of scroll position. Spans up to D5 (`l` = semi 14).
        // Cursor jumps to that pitch; viewport scrolls to keep it visible.
        int semi = key_to_semitone(c);
        if (semi >= 0) {
            pr_release_active();
            int midi = 60 + semi;
            if (midi < 0 || midi > 127) return true;
            if (s.rec_state.load() == RecordState::RECORDING) {
                // REC overrides note_mode. Capture at play_step; the cursor
                // stays put so the user keeps a stable reference point.
                enqueue_synth_trig(TRACK_DEFS[piano_track].melodic_idx,
                                   midi_to_hz(static_cast<uint8_t>(midi)));
                record_input(s, piano_track, static_cast<uint8_t>(midi));
                return true;
            }
            piano_cursor_midi = midi;
            pr_clamp_view();
            int new_idx = pr_place_note_at_cursor(s, 1);
            if (new_idx >= 0) {
                piano_active_note_idx = new_idx;
                enqueue_synth_trig(TRACK_DEFS[piano_track].melodic_idx,
                                   midi_to_hz(pr_cursor_pitch()));
            }
            return true;
        }
    }
    return false;
}

// 1-9 in grid mode: fill the cursor track at `interval`-step spacing, starting
// from `start`. Drum tracks set hits; melodic tracks place 1-step notes at the
// track's root pitch (existing notes at those steps on the root pitch are
// preserved if already present; we don't dedupe — chord stacking is fine).
static void fill_pattern(SessionState& s, int interval, int start) {
    if (TRACK_DEFS[cursor_track].type == TrackType::DRUM) {
        int dk = TRACK_DEFS[cursor_track].drum_kind;
        uint16_t fill_mask = 0;
        for (int step = start; step < s.loop_len; step += interval) {
            s.drum_grid[dk][step] = true;
            fill_mask |= static_cast<uint16_t>(1) << step;
        }
        s.dirty[cursor_track].fetch_or(fill_mask, std::memory_order_relaxed);
        return;
    }
    int midx = TRACK_DEFS[cursor_track].melodic_idx;
    uint8_t root = s.track_root_midi[cursor_track];
    auto& notes = s.melodic_notes[midx];
    bool changed = false;
    for (int step = start; step < s.loop_len; step += interval) {
        Note n{ static_cast<uint8_t>(step), 1, root, 127 };
        if (add_note(notes, n)) changed = true;
    }
    if (changed) s.melodic_dirty[midx].store(true);
}

// ---- track clipboard (c=copy, v=paste) ----------------------------------
//
// Single-slot clipboard. Cross-type paste translates:
//   drum -> melodic: every hit becomes a 1-step note at the destination
//                    track's root pitch.
//   melodic -> drum: every note's start_step becomes a hit (durations and
//                    pitches discarded).
// Native-type paste is a straight copy.
static int               clip_kind = -1;  // -1 empty, 0 drum, 1 melodic
static bool              clip_drum[STEPS] = {};
static std::vector<Note> clip_notes;

static void copy_cursor_track(SessionState& s) {
    if (TRACK_DEFS[cursor_track].type == TrackType::DRUM) {
        int dk = TRACK_DEFS[cursor_track].drum_kind;
        for (int st = 0; st < STEPS; ++st) clip_drum[st] = s.drum_grid[dk][st];
        clip_notes.clear();
        clip_kind = 0;
    } else {
        int midx = TRACK_DEFS[cursor_track].melodic_idx;
        clip_notes = s.melodic_notes[midx];
        for (int st = 0; st < STEPS; ++st) clip_drum[st] = false;
        clip_kind = 1;
    }
}

static void paste_to_cursor_track(SessionState& s) {
    if (clip_kind < 0) return;
    if (TRACK_DEFS[cursor_track].type == TrackType::DRUM) {
        int dk = TRACK_DEFS[cursor_track].drum_kind;
        if (clip_kind == 0) {
            for (int st = 0; st < STEPS; ++st) s.drum_grid[dk][st] = clip_drum[st];
        } else {
            // melodic -> drum: hit on every note start_step.
            for (int st = 0; st < STEPS; ++st) s.drum_grid[dk][st] = false;
            for (const Note& n : clip_notes) {
                if (n.start_step < STEPS) s.drum_grid[dk][n.start_step] = true;
            }
        }
        s.dirty[cursor_track].fetch_or(0xFFFFu, std::memory_order_relaxed);
    } else {
        int midx = TRACK_DEFS[cursor_track].melodic_idx;
        uint8_t root = s.track_root_midi[cursor_track];
        auto& notes = s.melodic_notes[midx];
        notes.clear();
        if (clip_kind == 0) {
            // drum -> melodic: 1-step note at root pitch for every hit.
            for (int st = 0; st < STEPS; ++st) {
                if (!clip_drum[st]) continue;
                Note n{ static_cast<uint8_t>(st), 1, root, 127 };
                add_note(notes, n);
            }
        } else {
            for (const Note& n : clip_notes) add_note(notes, n);
        }
        s.melodic_dirty[midx].store(true);
    }
}

// Backspace handler for the sequencer view. On a drum track, clears the hit
// at cursor_step. On a melodic track, erases every note whose span covers
// cursor_step (so stacked / sustaining notes all go in one keystroke).
static void delete_at_cursor_cell(SessionState& s) {
    if (TRACK_DEFS[cursor_track].type == TrackType::DRUM) {
        int dk = TRACK_DEFS[cursor_track].drum_kind;
        if (!s.drum_grid[dk][cursor_step]) return;
        s.drum_grid[dk][cursor_step] = false;
        s.dirty[cursor_track].fetch_or(static_cast<uint16_t>(1) << cursor_step,
                                       std::memory_order_relaxed);
        return;
    }
    int midx = TRACK_DEFS[cursor_track].melodic_idx;
    auto& notes = s.melodic_notes[midx];
    bool changed = false;
    for (auto it = notes.begin(); it != notes.end();) {
        int end = it->start_step + it->duration_steps;
        if (cursor_step >= it->start_step && cursor_step < end) {
            it = notes.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) s.melodic_dirty[midx].store(true);
}

// Track-level commands that work the same in GRID and KEYBOARD modes:
// nav-mode switches (Tab/Return), the delete key, mix toggles (S/M), the
// clipboard (C/V), clear-track + AD-clear-all, and the 1-9 fill pattern.
// Returns true if the event was a shared command.
static bool handle_shared_track_command(SessionState& s, Event e) {
    if (e == Event::Tab) {
        ensure_cursor_visible(s);
        nav_state = NavState::PARAM_PAGE;
        return true;
    }
    if (e == Event::Return) {
        ensure_cursor_visible(s);
        if (TRACK_DEFS[cursor_track].type == TrackType::MELODIC) {
            piano_track          = cursor_track;
            piano_cursor_step    = std::min(cursor_step, s.loop_len - 1);
            piano_cursor_midi    = s.track_root_midi[cursor_track];
            piano_view_base_midi = std::max(0, piano_cursor_midi - 6);
            if (piano_view_base_midi > 116) piano_view_base_midi = 116;
            pr_release_active();
            nav_state = NavState::PIANO_ROLL;
        }
        return true;
    }
    if (e == Event::Backspace) { delete_at_cursor_cell(s); return true; }
    if (e == Event::Character('S')) {
        s.track_solo[cursor_track] = !s.track_solo[cursor_track];
        return true;
    }
    if (e == Event::Character('M')) {
        s.track_muted[cursor_track] = !s.track_muted[cursor_track];
        return true;
    }
    if (e == Event::Character('C')) { copy_cursor_track(s);     return true; }
    if (e == Event::Character('V')) { paste_to_cursor_track(s); return true; }

    using clk = std::chrono::steady_clock;
    static auto last_A = clk::time_point{};
    constexpr auto CLEAR_ALL_WINDOW = std::chrono::milliseconds(500);

    if (e == Event::Character('A')) {
        // Arm clear-all. A standalone 'A' does nothing else; the next 'D'
        // within the window commits the wipe.
        last_A = clk::now();
        return true;
    }
    if (e == Event::Character('D')) {
        auto now = clk::now();
        if (now - last_A <= CLEAR_ALL_WINDOW) {
            for (int k = 0; k < DRUM_KINDS; ++k)
                std::memset(s.drum_grid[k], 0, sizeof(s.drum_grid[k]));
            for (int m = 0; m < MELODIC_VOICES; ++m) s.melodic_notes[m].clear();
            for (int t = 0; t < TRACKS; ++t) s.dirty[t].fetch_or(0xFFFFu);
            for (int m = 0; m < MELODIC_VOICES; ++m) s.melodic_dirty[m].store(true);
            last_A = clk::time_point{};
            return true;
        }
        if (TRACK_DEFS[cursor_track].type == TrackType::DRUM) {
            int dk = TRACK_DEFS[cursor_track].drum_kind;
            std::memset(s.drum_grid[dk], 0, sizeof(s.drum_grid[dk]));
            s.dirty[cursor_track].fetch_or(0xFFFFu, std::memory_order_relaxed);
        } else {
            int midx = TRACK_DEFS[cursor_track].melodic_idx;
            s.melodic_notes[midx].clear();
            s.melodic_dirty[midx].store(true);
        }
        return true;
    }
    if (e.is_character() && e.character().size() == 1) {
        char c = e.character()[0];
        if (c >= '1' && c <= '9') {
            fill_pattern(s, c - '0', cursor_step);
            return true;
        }
    }
    return false;
}

static bool dispatch_grid(SessionState& s, Event e) {
    if (handle_shared_track_command(s, e)) return true;

    if (e.is_character() && e.character().size() == 1 && e.character()[0] == 'k') {
        if (TRACK_DEFS[cursor_track].type == TrackType::MELODIC) {
            kbd_release_active();
            kbd_octave = std::max(0, (s.track_root_midi[cursor_track] / 12) - 1);
            nav_state = NavState::KEYBOARD;
        }
        return true;
    }

    // MPC pad triggers: focus + fire audio. Lowercase letters that match
    // TRACK_DEFS keys (v, b, n, m, f, g, h, j, r, t, y, u). These are
    // grid-only — keyboard mode treats lowercase letters as pitches.
    if (e.is_character() && e.character().size() == 1) {
        char c = e.character()[0];
        int t = track_for_mpc_key(c);
        if (t >= 0) {
            trigger_track_live(s, t);
            cursor_track = t;
            if (s.rec_state.load() == RecordState::RECORDING) {
                // Drum pads capture as bool-grid steps; melodic pads capture
                // 1-step root-pitch notes (root for skeleton, melodies get
                // refined later in piano roll / keyboard mode).
                const TrackDef& td = TRACK_DEFS[t];
                uint8_t pitch = (td.type == TrackType::MELODIC)
                              ? s.track_root_midi[t] : 0;
                record_input(s, t, pitch);
            }
            return true;
        }
    }

    if (e == Event::Character('s')) { seq_mode = !seq_mode; return true; }

    if (e == Event::ArrowUp)   { cycle_cursor_to_active(s, -1); return true; }
    if (e == Event::ArrowDown) { cycle_cursor_to_active(s, +1); return true; }

    if (seq_mode) {
        if (e == Event::ArrowLeft)  { window_start = (window_start - 8 + 16) % 16; return true; }
        if (e == Event::ArrowRight) { window_start = (window_start + 8) % 16;      return true; }
        if (e.is_character() && e.character().size() == 1) {
            char c = e.character()[0];
            if (c >= '1' && c <= '8') {
                int step = window_start + (c - '1');
                if (TRACK_DEFS[cursor_track].type == TrackType::DRUM) {
                    int dk = TRACK_DEFS[cursor_track].drum_kind;
                    s.drum_grid[dk][step] = !s.drum_grid[dk][step];
                    s.dirty[cursor_track].fetch_or(1u << step, std::memory_order_relaxed);
                }
                return true;
            }
        }
    } else {
        if (e == Event::ArrowLeft) {
            cursor_step = (cursor_step - 1 + STEPS) % STEPS; return true;
        }
        if (e == Event::ArrowRight) {
            cursor_step = (cursor_step + 1) % STEPS; return true;
        }
        if (e == Event::Character(' ')) {
            if (TRACK_DEFS[cursor_track].type == TrackType::DRUM) {
                int dk = TRACK_DEFS[cursor_track].drum_kind;
                s.drum_grid[dk][cursor_step] = !s.drum_grid[dk][cursor_step];
                s.dirty[cursor_track].fetch_or(1u << cursor_step, std::memory_order_relaxed);
            } else {
                // Place a 1-step note at root pitch. If a note already starts
                // at this cell on the root pitch, remove it (toggle feel).
                int midx = TRACK_DEFS[cursor_track].melodic_idx;
                uint8_t root = s.track_root_midi[cursor_track];
                auto& notes = s.melodic_notes[midx];
                bool removed = false;
                for (auto it = notes.begin(); it != notes.end(); ++it) {
                    if (it->start_step == cursor_step && it->pitch_midi == root) {
                        notes.erase(it); removed = true; break;
                    }
                }
                if (!removed) {
                    Note n{ static_cast<uint8_t>(cursor_step), 1, root, 127 };
                    add_note(notes, n);
                }
                s.melodic_dirty[midx].store(true);
            }
            return true;
        }
    }
    return false;
}

Component build_session_ui(ScreenInteractive& screen, SessionState& state) {
    auto root = Renderer([&state] {
        Element page;
        if (nav_state == NavState::PARAM_PAGE) {
            ensure_cursor_visible(state);
            page = (TRACK_DEFS[cursor_track].type == TrackType::MELODIC)
                 ? render_melodic_param_page(state, cursor_track)
                 : render_drum_param_page(state, cursor_track);
        } else if (nav_state == NavState::PIANO_ROLL) {
            page = render_piano_roll(state);
        } else {
            page = render_grid_view(state);
        }
        return vbox({
            render_visualizer(),
            page,
        });
    });

    auto with_events = CatchEvent(root, [&screen, &state](Event e) -> bool {
        // Global keys first.
        // Quit moved to shift+Q. Lowercase q drives record (PRD).
        if (e == Event::Character('Q')) {
            state.running.store(false);
            screen.Exit();
            return true;
        }
        if (e == Event::Character('q')) {
            // While recording, releasing any keyboard-mode active note so we
            // don't keep growing it via the still-tracked hold.
            kbd_release_active();
            pr_release_active();
            toggle_record(state);
            return true;
        }
        if (e == Event::Character('\\')) {
            state.metronome_enabled.store(!state.metronome_enabled.load());
            return true;
        }
        if (e == Event::Character('p') || e == Event::Character('P')) {
            // Hard halt if COUNTDOWN or RECORDING is active (PRD: p is never
            // ambiguous during those states).
            if (state.rec_state.load() != RecordState::OFF) {
                hard_halt_transport(state);
                return true;
            }
            state.playing.store(!state.playing.load());
            if (!state.playing.load()) state.play_step.store(0);
            return true;
        }
        if (e == Event::Character('+') || e == Event::Character('=')) {
            state.bpm = std::min(300, state.bpm + 5);
            state.bpm_dirty.store(true, std::memory_order_relaxed);
            return true;
        }
        if (e == Event::Character('-')) {
            state.bpm = std::max(40, state.bpm - 5);
            state.bpm_dirty.store(true, std::memory_order_relaxed);
            return true;
        }
        if (e == Event::Character(']')) {
            state.loop_len = std::min(16, state.loop_len + 1); return true;
        }
        if (e == Event::Character('[')) {
            state.loop_len = std::max(1, state.loop_len - 1); return true;
        }

        switch (nav_state) {
            case NavState::PARAM_PAGE: return dispatch_param_page(state, e);
            case NavState::KEYBOARD:   return dispatch_keyboard(state, e);
            case NavState::PIANO_ROLL: return dispatch_piano_roll(state, e);
            case NavState::GRID:
            default:                   return dispatch_grid(state, e);
        }
    });

    return with_events;
}

// ---- startup page (unchanged) --------------------------------------------

static const char* const BITJAMS_LOGO[] = {
    "    ██████╗ ██╗████████╗     ██╗ █████╗ ███╗   ███╗███████╗",
    "   ██╔══██╗██║╚══██╔══╝     ██║██╔══██╗████╗ ████║██╔════╝",
    "  ██████╔╝██║   ██║        ██║███████║██╔████╔██║███████╗ ",
    " ██╔══██╗██║   ██║   ██   ██║██╔══██║██║╚██╔╝██║╚════██║  ",
    "██████╔╝██║   ██║   ╚█████╔╝██║  ██║██║ ╚═╝ ██║███████║   ",
};
static constexpr int BITJAMS_LOGO_LINES = sizeof(BITJAMS_LOGO) / sizeof(BITJAMS_LOGO[0]);

static const Color COL_PURPLE_DIM = Color::RGB(80, 55, 160);

static const char* const BITJAMS_LOGO_UNDERLINE =
    "       ──── ──── ──── ──── ──── ──── ──── ────";

static uint16_t generate_room_id() {
    static std::mt19937 engine(std::random_device{}());
    std::uniform_int_distribution<uint16_t> dist(1, 65535);
    return dist(engine);
}

static Element render_logo() {
    Elements rows;
    for (int i = 0; i < BITJAMS_LOGO_LINES; ++i) {
        rows.push_back(text(BITJAMS_LOGO[i]) | color(COL_PURPLE));
    }
    rows.push_back(text(BITJAMS_LOGO_UNDERLINE) | color(COL_PURPLE_DIM));
    return vbox(std::move(rows));
}

namespace {
constexpr int LANDING    = 0;
constexpr int HOST_INFO  = 1;
constexpr int JOIN_ENTRY = 2;

struct StartupCtx {
    int           current      = LANDING;
    uint16_t      room         = 0;
    std::string   ip_buf;
    std::string   room_buf;
    std::string   last_error;
    StartupChoice* result      = nullptr;
    ftxui::ScreenInteractive*  screen = nullptr;
    std::string   local_ip     = "(unknown)";
};
} // namespace

extern std::string get_local_ipv4();

static Element render_landing(const StartupCtx& ctx) {
    Elements lines;
    lines.push_back(render_logo());
    lines.push_back(text(""));
    lines.push_back(text(""));
    lines.push_back(text("    [h]  host a session") | color(COL_PURPLE));
    lines.push_back(text("    [j]  join a session") | color(COL_PURPLE));
    lines.push_back(text("    [s]  solo")           | color(COL_PURPLE));
    lines.push_back(text("    [q]  quit")           | color(COL_PURPLE));
    if (!ctx.last_error.empty()) {
        lines.push_back(text(""));
        lines.push_back(text("  " + ctx.last_error) | color(Color::Red));
    }
    return vbox(std::move(lines)) | center;
}

static Element render_host_info(const StartupCtx& ctx) {
    char room_buf[16];
    std::snprintf(room_buf, sizeof(room_buf), "%u", (unsigned)ctx.room);

    Elements lines;
    lines.push_back(render_logo());
    lines.push_back(text(""));
    lines.push_back(text("  hosting on this machine") | color(COL_PURPLE));
    lines.push_back(text(""));
    lines.push_back(hbox({
        text("    ip:    ")   | color(COL_PURPLE_DIM),
        text(ctx.local_ip)    | color(COL_PURPLE),
    }));
    lines.push_back(hbox({
        text("    room:  ")   | color(COL_PURPLE_DIM),
        text(room_buf)        | color(COL_PURPLE),
    }));
    lines.push_back(text(""));
    lines.push_back(text("    [enter]  start session    [esc]  back")
                    | color(COL_PURPLE_DIM));
    return vbox(std::move(lines)) | center;
}

ftxui::Component build_startup_screens(ftxui::ScreenInteractive& screen,
                                       const std::string& last_error,
                                       StartupChoice& result_out) {
    auto ctx = std::make_shared<StartupCtx>();
    ctx->result     = &result_out;
    ctx->screen     = &screen;
    ctx->last_error = last_error;
    ctx->local_ip   = get_local_ipv4();

    auto ip_field   = Input(&ctx->ip_buf,   "192.168.1.42");
    auto room_field = Input(&ctx->room_buf, "room number");
    auto join_form  = Container::Vertical({ip_field, room_field});

    auto root = Renderer(join_form, [ctx, ip_field, room_field] {
        switch (ctx->current) {
            case HOST_INFO: return render_host_info(*ctx);
            case JOIN_ENTRY: {
                Elements lines;
                lines.push_back(render_logo());
                lines.push_back(text(""));
                lines.push_back(text("  join an existing session") | color(COL_PURPLE));
                lines.push_back(text(""));
                lines.push_back(hbox({
                    text("    ip:    ")  | color(COL_PURPLE_DIM),
                    ip_field->Render()   | color(COL_PURPLE) | size(WIDTH, EQUAL, 24),
                }));
                lines.push_back(hbox({
                    text("    room:  ") | color(COL_PURPLE_DIM),
                    room_field->Render() | color(COL_PURPLE) | size(WIDTH, EQUAL, 24),
                }));
                if (!ctx->last_error.empty()) {
                    lines.push_back(text(""));
                    lines.push_back(text("  " + ctx->last_error) | color(Color::Red));
                }
                lines.push_back(text(""));
                lines.push_back(text("    [enter]  join    [esc]  back")
                                | color(COL_PURPLE_DIM));
                return vbox(std::move(lines)) | center;
            }
            case LANDING:
            default:
                return render_landing(*ctx);
        }
    });

    auto with_events = CatchEvent(root, [ctx](Event e) -> bool {
        switch (ctx->current) {
            case LANDING: {
                if (e == Event::Character('h') || e == Event::Character('H')) {
                    ctx->room    = generate_room_id();
                    ctx->current = HOST_INFO;
                    return true;
                }
                if (e == Event::Character('j') || e == Event::Character('J')) {
                    ctx->last_error.clear();
                    ctx->current = JOIN_ENTRY;
                    return true;
                }
                if (e == Event::Character('s') || e == Event::Character('S')) {
                    *ctx->result = StartupChoice{StartupChoice::SOLO, "", 0};
                    ctx->screen->Exit();
                    return true;
                }
                if (e == Event::Character('q') || e == Event::Character('Q')) {
                    *ctx->result = StartupChoice{StartupChoice::QUIT, "", 0};
                    ctx->screen->Exit();
                    return true;
                }
                return false;
            }
            case HOST_INFO: {
                if (e == Event::Return) {
                    *ctx->result = StartupChoice{StartupChoice::HOST, "", ctx->room};
                    ctx->screen->Exit();
                    return true;
                }
                if (e == Event::Escape) {
                    ctx->current = LANDING;
                    return true;
                }
                return false;
            }
            case JOIN_ENTRY: {
                if (e == Event::Return) {
                    if (ctx->ip_buf.empty() || ctx->room_buf.empty()) return true;
                    int parsed = 0;
                    try { parsed = std::stoi(ctx->room_buf); }
                    catch (...) { return true; }
                    if (parsed < 1 || parsed > 65535) return true;
                    *ctx->result = StartupChoice{
                        StartupChoice::JOIN,
                        ctx->ip_buf,
                        static_cast<uint16_t>(parsed)
                    };
                    ctx->screen->Exit();
                    return true;
                }
                if (e == Event::Escape) {
                    ctx->current = LANDING;
                    return true;
                }
                return false;
            }
        }
        return false;
    });

    return with_events;
}

StartupChoice run_startup_page(const std::string& last_error) {
    auto screen = ftxui::ScreenInteractive::Fullscreen();
    screen.TrackMouse(false);
    StartupChoice result;
    auto root = build_startup_screens(screen, last_error, result);
    screen.Loop(root);
    return result;
}
