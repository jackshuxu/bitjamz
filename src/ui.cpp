#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "audio.h"
#include "sequencer.h"

using namespace ftxui;

//color palette

static const Color COL_PURPLE = Color::RGB(125,  86, 244);
static const Color COL_DIM    = Color::RGB( 60,  40, 120);
static const Color COL_BRIGHT = Color::RGB(200, 180, 255);
static const Color COL_HEAD   = Color::RGB(255, 220, 100);
static const Color COL_GREEN  = Color::RGB( 80, 220, 120);

//ui state

enum class OscType { SQUARE, SAW, TRIANGLE, SINE };
static constexpr int OSC_COUNT = 4;
static const char* OSC_NAMES[OSC_COUNT] = { "SQUARE", "SAW", "TRIANGLE", "SINE" };

static OscType synth_osc    = OscType::SQUARE;
static int     synth_octave = 4;
static bool    scale_snap   = false;

enum class NavState { GRID, TITLE_FOCUS, SYNTH_PAGE };
static NavState nav_state = NavState::GRID;
static bool     seq_mode  = false; // GRID sub-mode: 8-step window

//helpers

static int count_active_tracks() {
    int c = 0;
    for (int t = 0; t < TRACKS; ++t) if (track_active[t]) ++c;
    return c;
}

static void cycle_cursor_to_active(int dir) {
    if (count_active_tracks() == 0) return;
    int t = cursor_track;
    for (int i = 0; i < TRACKS; ++i) {
        t = (t + dir + TRACKS) % TRACKS;
        if (track_active[t]) { cursor_track = t; return; }
    }
}

static void ensure_cursor_visible() {
    if (track_active[cursor_track]) return;
    cycle_cursor_to_active(+1);
}

// activate the next inactive track (wrapping); set cursor on it.
static void show_next_hidden() {
    int start = (cursor_track + 1) % TRACKS;
    int t     = start;
    do {
        if (!track_active[t]) {
            track_active[t] = true;
            cursor_track    = t;
            return;
        }
        t = (t + 1) % TRACKS;
    } while (t != start);
}

// hide cursor track, then move cursor to next visible (skip if only one visible)
static void hide_cursor_track() {
    if (count_active_tracks() <= 1) return;
    track_active[cursor_track] = false;
    cycle_cursor_to_active(+1);
}

//stereometer

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

//grid view

// Title column = "[NNNNN](k) " (11 chars). Step rows align so column header,
// playhead, and track rows all share the same step-cell origin.
static constexpr int  TITLE_COL_WIDTH = 11;
static constexpr char TITLE_PAD[]     = "          "; // 10 spaces (TITLE_COL_WIDTH - 1)

static Element render_track_title(int t, bool focused) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "[%-5s](%c) ",
                  TRACK_DEFS[t].name, TRACK_DEFS[t].key);
    Color col = (TRACK_DEFS[t].type == TrackType::MELODIC) ? COL_GREEN : COL_PURPLE;
    Element e = text(buf) | color(col);
    if (focused) e = e | inverted | bold;
    return e;
}

static Element render_grid_view() {
    int  ps         = play_step.load();
    bool is_playing = playing.load();

    Elements lines;

    // header
    {
        char head_buf[80];
        const char* mode_label =
            (nav_state == NavState::TITLE_FOCUS) ? "  [title]" :
            (seq_mode                          ) ? "  [seq]"   :
                                                   "  [step]";
        std::snprintf(head_buf, sizeof(head_buf), "  bpm: %d  steps: %d  %s",
                      bpm, loop_len, is_playing ? "▶" : "■");
        lines.push_back(hbox({
            text("bitjams") | bold | color(COL_PURPLE),
            text(head_buf)         | color(COL_PURPLE),
            text(mode_label)       | color(COL_DIM),
        }));
    }

    // step numbers (offset by TITLE_COL_WIDTH-1 so digits align with cells)
    {
        Elements row;
        row.push_back(text(TITLE_PAD) | color(COL_DIM));
        for (int s = 0; s < STEPS; ++s) {
            if (s == 9) row.push_back(text(" ") | color(COL_DIM));
            char buf[8];
            if (s < loop_len) std::snprintf(buf, sizeof(buf), " %2d", s + 1);
            else              std::snprintf(buf, sizeof(buf), "   ");
            row.push_back(text(buf) | color(COL_DIM));
        }
        lines.push_back(hbox(std::move(row)));
    }

    // playhead row
    {
        Elements row;
        row.push_back(text(std::string(TITLE_COL_WIDTH, ' ')));
        for (int s = 0; s < STEPS; ++s) {
            if (s == ps && is_playing)
                row.push_back(text(" ▼ ") | color(COL_HEAD));
            else
                row.push_back(text("   "));
        }
        lines.push_back(hbox(std::move(row)));
    }

    // visible tracks grouped (melodic / mid drums / bottom drums) with
    // blank separator rows between non-empty groups
    auto render_track_row = [&](int t) {
        bool title_focused = (nav_state == NavState::TITLE_FOCUS) && (t == cursor_track);
        Elements row;
        row.push_back(render_track_title(t, title_focused));
        for (int s = 0; s < STEPS; ++s) {
            bool active      = grid[t][s];
            bool highlighted = (nav_state == NavState::GRID)
                            && (t == cursor_track)
                            && (seq_mode
                                ? (s >= window_start && s < window_start + 8)
                                : (s == cursor_step));
            bool in_loop     = (s < loop_len);
            if (!in_loop) {
                row.push_back(text(" · ") | color(COL_DIM));
            } else if (highlighted) {
                row.push_back(text(active ? " ■ " : " · ") | color(COL_PURPLE) | inverted);
            } else if (active) {
                Element e = text(" ■ ") | color(COL_BRIGHT);
                if (s == ps && is_playing) e = e | bold;
                row.push_back(e);
            } else {
                row.push_back(text(" · ") | color(COL_DIM));
            }
        }
        return hbox(std::move(row));
    };

    for (int t = 0; t < TRACKS; ++t)
        if (track_active[t]) lines.push_back(render_track_row(t));

    // help bar
    lines.push_back(text(""));
    if (nav_state == NavState::TITLE_FOCUS) {
        lines.push_back(text("  ↑↓:track  enter:open synth  tab/esc:back  rtyufghjvbnm:trigger"
                             "  p:play  q:quit")
                        | color(COL_DIM));
    } else if (seq_mode) {
        lines.push_back(text("  1-8:toggle step  ←→:window  ↑↓:track  rtyufghjvbnm:trigger+focus"
                             "  a:show  d:hide  s:step mode  tab:title  p:play  q:quit")
                        | color(COL_DIM));
    } else {
        lines.push_back(text("  spc:toggle step  1-9:fill  ←→↑↓:move  rtyufghjvbnm:trigger+focus"
                             "  a:show  d:hide  s:seq mode  tab:title  p:play  q:quit")
                        | color(COL_DIM));
    }

    return vbox(std::move(lines));
}

//synth page (drum, display only)

static Element render_drum_synth_page(int t) {
    const TrackDef& td = TRACK_DEFS[t];
    const DrumParams& p = DRUM_PARAMS[td.drum_kind];

    Elements lines;

    char head_buf[80];
    std::snprintf(head_buf, sizeof(head_buf), "  bpm: %d  steps: %d  %s  ",
                  bpm, loop_len, playing.load() ? "▶" : "■");
    lines.push_back(hbox({
        text("bitjams") | bold | color(COL_PURPLE),
        text(head_buf)         | color(COL_PURPLE),
        text("[drum synth]")   | color(COL_DIM),
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
    lines.push_back(param_row("track key", std::string(1, td.key)));

    lines.push_back(text(""));
    lines.push_back(text("  (display only — knob editing is future work)") | color(COL_DIM));
    lines.push_back(text(""));
    lines.push_back(text("  rtyufghjvbnm:trigger any pad  tab/esc:back  p:play  q:quit")
                    | color(COL_DIM));

    return vbox(std::move(lines));
}

//synth page (melodic, piano keyboard)

static Color key_col(int semitone) {
    if (!scale_snap) return COL_PURPLE;
    return in_pentatonic(semitone) ? COL_BRIGHT : COL_DIM;
}

static Element draw_key_el(const char* label, int semitone) {
    return text(std::string("[") + label + "]") | color(key_col(semitone));
}

static Element render_melodic_synth_page(int t) {
    const TrackDef& td = TRACK_DEFS[t];
    bool is_playing = playing.load();

    Elements lines;

    char head_buf[80];
    std::snprintf(head_buf, sizeof(head_buf), "  bpm: %d  steps: %d  %s  ",
                  bpm, loop_len, is_playing ? "▶" : "■");
    lines.push_back(hbox({
        text("bitjams")           | bold | color(COL_PURPLE),
        text(head_buf)                   | color(COL_PURPLE),
        text("[melodic synth]")          | color(COL_DIM),
    }));

    char title[64];
    std::snprintf(title, sizeof(title), "  %s  (key: %c)  oscillator voice",
                  td.name, td.key);
    lines.push_back(text(title) | bold | color(COL_GREEN));

    {
        char osc_buf[16];
        std::snprintf(osc_buf, sizeof(osc_buf), "%-8s", OSC_NAMES[(int)synth_osc]);
        char oct_buf[8];
        std::snprintf(oct_buf, sizeof(oct_buf), "%d", synth_octave);
        const char* scale_str = scale_snap ? "minor penta" : "chromatic";
        lines.push_back(hbox({
            text(" osc ")     | color(COL_DIM),
            text("◄ ")        | color(COL_DIM),
            text(osc_buf)     | bold | color(COL_BRIGHT),
            text("►")         | color(COL_DIM),
            text("  oct ")    | color(COL_DIM),
            text(oct_buf)     | color(COL_BRIGHT),
            text("  scale ")  | color(COL_DIM),
            text(scale_str)   | color(COL_BRIGHT),
        }));
    }

    lines.push_back(text(""));

    // black-key row
    {
        Elements row;
        row.push_back(text(" "));
        row.push_back(draw_key_el("w",  1));
        row.push_back(draw_key_el("e",  3));
        row.push_back(text("   "));
        row.push_back(draw_key_el("t",  6));
        row.push_back(draw_key_el("y",  8));
        row.push_back(draw_key_el("u", 10));
        row.push_back(text("   "));
        row.push_back(draw_key_el("o", 13));
        lines.push_back(hbox(std::move(row)));
    }

    // white-key row
    {
        Elements row;
        row.push_back(draw_key_el("a",  0));
        row.push_back(draw_key_el("s",  2));
        row.push_back(draw_key_el("d",  4));
        row.push_back(draw_key_el("f",  5));
        row.push_back(draw_key_el("g",  7));
        row.push_back(draw_key_el("h",  9));
        row.push_back(draw_key_el("j", 11));
        row.push_back(draw_key_el("k", 12));
        row.push_back(draw_key_el("l", 14));
        lines.push_back(hbox(std::move(row)));
    }

    lines.push_back(text(""));
    lines.push_back(text("  z:oct▼  x:oct▲  n:scale  ↑↓:osc  tab/esc:back  p:play  q:quit")
                    | color(COL_DIM));

    return vbox(std::move(lines));
}

//input handling

static void play_synth_key_for_track(int t, int semitone) {
    if (TRACK_DEFS[t].type != TrackType::MELODIC) return;
    int abs_st = (synth_octave - 4) * 12 + semitone;
    if (scale_snap) abs_st = snap_pentatonic(abs_st);
    synth_trig_freq[TRACK_DEFS[t].melodic_idx].store(note_to_freq(abs_st));
}

// trigger from MPC pad: fire the track's voice at its root pitch (or drum).
static void trigger_track_live(int t) {
    if (!track_active[t]) return;
    if (TRACK_DEFS[t].type == TrackType::MELODIC) {
        synth_trig_freq[TRACK_DEFS[t].melodic_idx].store(track_root_hz[t]);
    } else {
        trig[t].store(true);
    }
}

Component build_ui(ScreenInteractive& screen) {
    auto root = Renderer([] {
        Element page;
        if (nav_state == NavState::SYNTH_PAGE) {
            ensure_cursor_visible();
            page = (TRACK_DEFS[cursor_track].type == TrackType::MELODIC)
                 ? render_melodic_synth_page(cursor_track)
                 : render_drum_synth_page(cursor_track);
        } else {
            page = render_grid_view();
        }
        return vbox({
            render_visualizer(),
            page,
        });
    });

    auto with_events = CatchEvent(root, [&screen](Event e) -> bool {
        // global quit
        if (e == Event::Character('q') || e == Event::Character('Q')) {
            running.store(false);
            screen.Exit();
            return true;
        }

        // global play / bpm / loop_len
        if (e == Event::Character('p') || e == Event::Character('P')) {
            playing.store(!playing.load());
            if (!playing.load()) play_step.store(0);
            return true;
        }
        if (e == Event::Character('+') || e == Event::Character('=')) {
            bpm = std::min(300, bpm + 5); return true;
        }
        if (e == Event::Character('-')) {
            bpm = std::max(40, bpm - 5); return true;
        }
        if (e == Event::Character(']')) {
            loop_len = std::min(16, loop_len + 1); return true;
        }
        if (e == Event::Character('[')) {
            loop_len = std::max(1, loop_len - 1); return true;
        }

        // ---- SYNTH_PAGE ----
        if (nav_state == NavState::SYNTH_PAGE) {
            if (e == Event::Tab || e == Event::Escape) {
                nav_state = NavState::GRID;
                return true;
            }
            if (TRACK_DEFS[cursor_track].type == TrackType::MELODIC) {
                if (e == Event::ArrowUp) {
                    int o = ((int)synth_osc - 1 + OSC_COUNT) % OSC_COUNT;
                    synth_osc = (OscType)o;
                    synth_osc_atom.store(o);
                    return true;
                }
                if (e == Event::ArrowDown) {
                    int o = ((int)synth_osc + 1) % OSC_COUNT;
                    synth_osc = (OscType)o;
                    synth_osc_atom.store(o);
                    return true;
                }
                if (e.is_character() && e.character().size() == 1) {
                    char c = e.character()[0];
                    int st = key_to_semitone(c);
                    if (st >= 0) { play_synth_key_for_track(cursor_track, st); return true; }
                    if (c == 'z') { synth_octave = std::max(1, synth_octave - 1); return true; }
                    if (c == 'x') { synth_octave = std::min(7, synth_octave + 1); return true; }
                    if (c == 'n') { scale_snap = !scale_snap; return true; }
                }
            } else {
                // drum synth page: still allow MPC-pad triggers so the user can
                // audition any track without leaving the page
                if (e.is_character() && e.character().size() == 1) {
                    int t = track_for_mpc_key(e.character()[0]);
                    if (t >= 0) { trigger_track_live(t); return true; }
                }
            }
            return false;
        }

        // ---- TITLE_FOCUS ----
        if (nav_state == NavState::TITLE_FOCUS) {
            if (e == Event::Tab || e == Event::Escape) {
                nav_state = NavState::GRID;
                return true;
            }
            if (e == Event::Return) {
                nav_state = NavState::SYNTH_PAGE;
                return true;
            }
            if (e == Event::ArrowUp)   { cycle_cursor_to_active(-1); return true; }
            if (e == Event::ArrowDown) { cycle_cursor_to_active(+1); return true; }
            // MPC keys still trigger (no focus move; user is navigating)
            if (e.is_character() && e.character().size() == 1) {
                int t = track_for_mpc_key(e.character()[0]);
                if (t >= 0) { trigger_track_live(t); return true; }
            }
            return false;
        }

        // ---- GRID (default) ----
        if (e == Event::Tab) {
            ensure_cursor_visible();
            nav_state = NavState::TITLE_FOCUS;
            return true;
        }

        // MPC pad: trigger sound + move cursor focus to that track
        if (e.is_character() && e.character().size() == 1) {
            char c = e.character()[0];
            int t = track_for_mpc_key(c);
            if (t >= 0 && track_active[t]) {
                trigger_track_live(t);
                cursor_track = t;
                return true;
            }
        }

        // show/hide
        if (e == Event::Character('a')) { show_next_hidden(); return true; }
        if (e == Event::Character('d')) { hide_cursor_track(); return true; }

        // track navigation (cycles only visible tracks)
        if (e == Event::ArrowUp)   { cycle_cursor_to_active(-1); return true; }
        if (e == Event::ArrowDown) { cycle_cursor_to_active(+1); return true; }

        if (e == Event::Character('s')) { seq_mode = !seq_mode; return true; }
        if (e == Event::Character('c')) {
            std::memset(grid[cursor_track], 0, sizeof(grid[cursor_track]));
            return true;
        }
        if (e == Event::Character('C')) {
            std::memset(grid, 0, sizeof(grid));
            return true;
        }

        if (seq_mode) {
            if (e == Event::ArrowLeft)  { window_start = (window_start - 8 + 16) % 16; return true; }
            if (e == Event::ArrowRight) { window_start = (window_start + 8) % 16;      return true; }
            if (e.is_character() && e.character().size() == 1) {
                char c = e.character()[0];
                if (c >= '1' && c <= '8') {
                    int s = window_start + (c - '1');
                    grid[cursor_track][s] = !grid[cursor_track][s];
                    return true;
                }
            }
        } else {
            if (e == Event::ArrowLeft)  { cursor_step = (cursor_step - 1 + STEPS) % STEPS; return true; }
            if (e == Event::ArrowRight) { cursor_step = (cursor_step + 1) % STEPS;          return true; }
            if (e == Event::Character(' ')) {
                grid[cursor_track][cursor_step] = !grid[cursor_track][cursor_step];
                return true;
            }
            if (e.is_character() && e.character().size() == 1) {
                char c = e.character()[0];
                if (c >= '1' && c <= '9') { fill_pattern(c - '0', cursor_step); return true; }
            }
        }

        return false;
    });

    return with_events;
}
