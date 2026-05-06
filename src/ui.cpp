#include "ui.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <ftxui/dom/elements.hpp>

#include "audio.h"
#include "sequencer.h"

using namespace ftxui;

//color palette (RGB triplets matching the original COL_* macros)

static const Color COL_PURPLE = Color::RGB(125,  86, 244);
static const Color COL_DIM    = Color::RGB( 60,  40, 120);
static const Color COL_BRIGHT = Color::RGB(200, 180, 255);
static const Color COL_HEAD   = Color::RGB(255, 220, 100);
static const Color COL_GREEN  = Color::RGB( 80, 220, 120);

//ui state (module-private)

enum class OscType { SQUARE, SAW, TRIANGLE, SINE };
static constexpr int OSC_COUNT = 4;
static const char* OSC_NAMES[OSC_COUNT] = { "SQUARE", "SAW", "TRIANGLE", "SINE" };

static OscType synth_osc    = OscType::SQUARE;
static int     synth_octave = 4;
static bool    scale_snap   = false;

static int tab_index = 0;  // 0 = grid, 1 = synth

static const char* track_names[TRACKS] = {"k", "s", "h", "c"};

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
    rows.push_back(text(""));  // matches trailing newline after stereometer
    return vbox(std::move(rows));
}

//grid view

static Element render_grid_view() {
    int  ps         = play_step.load();
    bool is_playing = playing.load();

    Elements lines;

    // header
    char head_buf[64];
    std::snprintf(head_buf, sizeof(head_buf), "  bpm: %d  steps: %d  %s",
                  bpm, loop_len, is_playing ? "▶" : "■");
    lines.push_back(hbox({
        text("bitjams") | bold | color(COL_PURPLE),
        text(head_buf)         | color(COL_PURPLE),
    }));

    // step numbers
    {
        Elements row;
        row.push_back(text("   ") | color(COL_DIM));
        for (int s = 0; s < STEPS; ++s) {
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
        row.push_back(text("   "));
        for (int s = 0; s < STEPS; ++s) {
            if (s == ps && is_playing)
                row.push_back(text("  ▼") | color(COL_HEAD));
            else
                row.push_back(text("   "));
        }
        lines.push_back(hbox(std::move(row)));
    }

    // tracks
    for (int t = 0; t < TRACKS; ++t) {
        Elements row;
        row.push_back(text(std::string(track_names[t]) + "  ") | color(COL_PURPLE));
        for (int s = 0; s < STEPS; ++s) {
            bool active  = grid[t][s];
            bool is_cur  = (t == cursor_track && s == cursor_step);
            bool in_loop = (s < loop_len);
            if (!in_loop) {
                row.push_back(text("  ·") | color(COL_DIM));
            } else if (is_cur) {
                row.push_back(text("  "));
                row.push_back(text(active ? "■" : "·") | color(COL_PURPLE) | inverted);
            } else if (active) {
                Element e = text("  ■") | color(COL_BRIGHT);
                if (s == ps && is_playing) e = e | bold;
                row.push_back(e);
            } else {
                row.push_back(text("  ·") | color(COL_DIM));
            }
        }
        lines.push_back(hbox(std::move(row)));
    }

    // blank + help bar
    lines.push_back(text(""));
    lines.push_back(text("  spc:toggle  1-4:fill interval  arrows:move"
                         "  p:play  +/-:bpm  [/]:steps  c:clear  tab:synth  q:quit")
                    | color(COL_DIM));

    return vbox(std::move(lines));
}

//synth view

static Color key_col(int semitone) {
    if (!scale_snap) return COL_PURPLE;
    return in_pentatonic(semitone) ? COL_BRIGHT : COL_DIM;
}

static Element draw_key_el(const char* label, int semitone) {
    return text(std::string("[") + label + "]") | color(key_col(semitone));
}

static Element render_synth_view() {
    bool is_playing = playing.load();

    Elements lines;

    // header
    {
        char head_buf[64];
        std::snprintf(head_buf, sizeof(head_buf), "  bpm: %d  steps: %d  %s  ",
                      bpm, loop_len, is_playing ? "▶" : "■");
        lines.push_back(hbox({
            text("bitjams")  | bold | color(COL_PURPLE),
            text(head_buf)          | color(COL_PURPLE),
            text("[synth]")         | color(COL_DIM),
        }));
    }

    // controls row
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

    // black keys row:  [w][e]   [t][y][u]   [o]
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

    // white keys row: [a][s][d][f][g][h][j][k][l]
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

    // help bar
    lines.push_back(text("  z:oct▼  x:oct▲  n:scale  ↑↓:osc  p:play  +/-:bpm  [/]:steps  tab:grid  q:quit")
                    | color(COL_DIM));

    // extra blank to match grid view height
    lines.push_back(text(""));

    return vbox(std::move(lines));
}

//input handling

static void play_synth_key(int semitone) {
    int abs_st = (synth_octave - 4) * 12 + semitone;
    if (scale_snap) abs_st = snap_pentatonic(abs_st);
    synth_trig_freq.store(note_to_freq(abs_st));
}

Component build_ui(ScreenInteractive& screen) {
    auto grid_view  = Renderer([] { return render_grid_view(); });
    auto synth_view = Renderer([] { return render_synth_view(); });

    auto tabs = Container::Tab({grid_view, synth_view}, &tab_index);

    auto root = Renderer(tabs, [tabs] {
        return vbox({
            render_visualizer(),
            tabs->Render(),
        });
    });

    auto with_events = CatchEvent(root, [&screen](Event e) -> bool {
        // quit
        if (e == Event::Character('q') || e == Event::Character('Q')) {
            running.store(false);
            screen.Exit();
            return true;
        }

        // view switch
        if (e == Event::Tab) {
            tab_index = (tab_index == 0) ? 1 : 0;
            return true;
        }

        // shared keys
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
            loop_len = std::max(1,  loop_len - 1); return true;
        }

        if (tab_index == 1) {
            // synth mode
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
            if (e.is_character()) {
                const std::string& cs = e.character();
                if (cs.size() == 1) {
                    char c = cs[0];
                    int st = key_to_semitone(c);
                    if (st >= 0) { play_synth_key(st); return true; }
                    if (c == 'z') { synth_octave = std::max(1, synth_octave - 1); return true; }
                    if (c == 'x') { synth_octave = std::min(7, synth_octave + 1); return true; }
                    if (c == 'n') { scale_snap = !scale_snap; return true; }
                }
            }
        } else {
            // grid mode
            if (e == Event::ArrowUp)    { cursor_track = (cursor_track - 1 + TRACKS) % TRACKS; return true; }
            if (e == Event::ArrowDown)  { cursor_track = (cursor_track + 1) % TRACKS; return true; }
            if (e == Event::ArrowLeft)  { cursor_step  = (cursor_step  - 1 + STEPS)  % STEPS;  return true; }
            if (e == Event::ArrowRight) { cursor_step  = (cursor_step  + 1) % STEPS;           return true; }
            if (e == Event::Character(' ')) {
                grid[cursor_track][cursor_step] = !grid[cursor_track][cursor_step];
                return true;
            }
            if (e == Event::Character('1')) { fill_pattern(1); return true; }
            if (e == Event::Character('2')) { fill_pattern(2); return true; }
            if (e == Event::Character('3')) { fill_pattern(3); return true; }
            if (e == Event::Character('4')) { fill_pattern(4); return true; }
            if (e == Event::Character('c') || e == Event::Character('C')) {
                std::memset(grid, 0, sizeof(grid));
                return true;
            }
        }

        return false;
    });

    return with_events;
}
