#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

static constexpr int TRACKS      = 4;
static constexpr int STEPS       = 16;
static constexpr int SAMPLE_RATE = 44100;

#define COL_PURPLE  "\033[38;2;125;86;244m"
#define COL_DIM     "\033[38;2;60;40;120m"
#define COL_BRIGHT  "\033[38;2;200;180;255m"
#define COL_HEAD    "\033[38;2;255;220;100m"
#define COL_GREEN   "\033[38;2;80;220;120m"
#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"

//ui mode

enum class UIMode { GRID, SYNTH };
static UIMode ui_mode      = UIMode::GRID;
static UIMode ui_mode_prev = UIMode::GRID;

//glob sequencer state

static bool              grid[TRACKS][STEPS] = {};
static std::atomic<int>  play_step{0};
static std::atomic<bool> running{true};
static std::atomic<bool> playing{true};

static int cursor_track = 0;
static int cursor_step  = 0;
static int bpm          = 120;
static int loop_len     = 16;

static std::atomic<bool> trig[TRACKS] = {};

static constexpr float TRACK_PAN[TRACKS] = { 0.f, 0.30f, -0.30f, 0.10f };

//synth state

enum class OscType { SQUARE, SAW, TRIANGLE, SINE };
static constexpr int      OSC_COUNT  = 4;
static const char*        OSC_NAMES[OSC_COUNT] = { "SQUARE", "SAW", "TRIANGLE", "SINE" };

static OscType synth_osc    = OscType::SQUARE;
static int     synth_octave = 4;
static bool    scale_snap   = false;

struct SynthVoice { bool active; float phase, env, t, freq; };
static SynthVoice         synth_voice     = {};
static std::atomic<float> synth_trig_freq{-1.f};
static std::atomic<int>   synth_osc_atom{0};  // mirrors synth_osc for audio thread

//stereometer ring buffer

static constexpr int VIS_BUF = 8192;
struct StereoSample { float l, r; };
static StereoSample     vis_buf[VIS_BUF];
static std::atomic<int> vis_wp{0};

static constexpr int VIS_W = 3 + STEPS * 3;
static constexpr int VIS_H = 7;
static float vis_grid[VIS_H][VIS_W] = {};

//terminal helpers

static termios orig_term;

static void restore_terminal() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
    printf("\033[?25h" COL_RESET "\n");
    fflush(stdout);
}

static void init_terminal() {
    tcgetattr(STDIN_FILENO, &orig_term);
    atexit(restore_terminal);
    termios raw = orig_term;
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    printf("\033[?25l");
    fflush(stdout);
}

static bool stdin_ready(int timeout_us) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv{0, timeout_us};
    return select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
}

//music helpers

// semitone offset from C4 → Hz  (0=C4≈261.6, 9=A4=440)
static float note_to_freq(int st) {
    return 440.f * std::pow(2.f, (st - 9.f) / 12.f);
}

//pentatonic snap
static const int PENTA_MAP[12] = {0,0,3,3,3,5,5,7,7,10,10,10};
static int snap_pentatonic(int semitone) {
    int oct = semitone / 12;
    int s   = semitone % 12;
    if (s < 0) { s += 12; --oct; }  // floor division for negatives
    return oct * 12 + PENTA_MAP[s];
}

// returns semitone offset from C in current octave (0-14), or -1
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
    return s==0 || s==3 || s==5 || s==7 || s==10;
}

//drums engine

struct Voice { bool active; float phase, env, t, freq, env_dur; };
static Voice voices[TRACKS];

static uint32_t lcg_state = 0x12345678u;
static inline float noise() {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (float)(int32_t)lcg_state / 2147483648.f;
}

static void trigger_voice(int track) {
    Voice& v = voices[track];
    v.active = true; v.t = 0.f; v.env = 1.f; v.phase = 0.f;
    switch (track) {
        case 0: v.freq=180.f;  v.env_dur=0.300f; break;
        case 1: v.freq=200.f;  v.env_dur=0.180f; break;
        case 2: v.freq=8000.f; v.env_dur=0.050f; break;
        case 3: v.freq=0.f;    v.env_dur=0.120f; break;
    }
}

static float render_voice(int track) {
    Voice& v = voices[track];
    if (!v.active) return 0.f;
    const float dt = 1.f / SAMPLE_RATE;
    v.env = std::max(0.f, 1.f - v.t / v.env_dur);
    if (v.env <= 0.f) { v.active = false; return 0.f; }
    float s = 0.f;
    if (track == 0) {
        float freq = 40.f + (180.f - 40.f) * std::max(0.f, 1.f - v.t / 0.030f);
        v.phase += freq * dt;
        if (v.phase > 1.f) v.phase -= 1.f;
        s = std::sin(v.phase * 2.f * (float)M_PI);
    } else if (track == 1) {
        v.phase += v.freq * dt;
        if (v.phase > 1.f) v.phase -= 1.f;
        s = noise() * 0.7f + ((v.phase < 0.5f) ? 1.f : -1.f) * 0.3f;
    } else if (track == 2) {
        static float hp_in = 0.f, hp_out = 0.f;
        float n = noise();
        float hp = 0.97f * (hp_out + n - hp_in);
        hp_in = n; hp_out = hp;
        s = hp;
    } else {
        for (int b = 0; b < 3; ++b) {
            float bt = v.t - b * 0.008f;
            if (bt >= 0.f && bt < 0.015f)
                s += noise() * (1.f - bt / 0.015f);
        }
    }
    s = (float)(int8_t)(s * 127.f) / 127.f;
    v.t += dt;
    return s * v.env;
}

//melodic synth engine

static float render_synth_voice() {
    if (!synth_voice.active) return 0.f;
    const float dt = 1.f / SAMPLE_RATE;
    synth_voice.env = std::max(0.f, 1.f - synth_voice.t / 0.5f);
    if (synth_voice.env <= 0.f) { synth_voice.active = false; return 0.f; }
    synth_voice.phase += synth_voice.freq * dt;
    if (synth_voice.phase > 1.f) synth_voice.phase -= 1.f;
    float s = 0.f;
    switch (synth_osc_atom.load(std::memory_order_relaxed)) {
        case 0: s = (synth_voice.phase < 0.5f) ? 1.f : -1.f; break;
        case 1: s = 2.f * synth_voice.phase - 1.f; break;
        case 2: s = 2.f * std::abs(2.f * synth_voice.phase - 1.f) - 1.f; break;
        case 3: s = std::sin(synth_voice.phase * 2.f * (float)M_PI); break;
    }
    s = (float)(int8_t)(s * 127.f) / 127.f;
    synth_voice.t += dt;
    return s * synth_voice.env * 0.4f;
}

//audio callback

static void audio_callback(ma_device* /*dev*/, void* out, const void* /*in*/, ma_uint32 frames) {
    float* buf = (float*)out;

    for (int t = 0; t < TRACKS; ++t)
        if (trig[t].exchange(false))
            trigger_voice(t);

    float f = synth_trig_freq.exchange(-1.f);
    if (f > 0.f) {
        synth_voice = { true, 0.f, 1.f, 0.f, f };
    }

    int wp = vis_wp.load(std::memory_order_relaxed);

    for (ma_uint32 i = 0; i < frames; ++i) {
        float mix_l = 0.f, mix_r = 0.f;
        for (int t = 0; t < TRACKS; ++t) {
            float v     = render_voice(t);
            float angle = (TRACK_PAN[t] + 1.f) * (float)M_PI * 0.25f;
            mix_l += v * std::cos(angle);
            mix_r += v * std::sin(angle);
        }
        float sv = render_synth_voice();
        mix_l += sv; mix_r += sv;

        float l = (float)(int8_t)(mix_l * 127.f) / 127.f * 0.25f;
        float r = (float)(int8_t)(mix_r * 127.f) / 127.f * 0.25f;
        buf[i * 2 + 0] = l;
        buf[i * 2 + 1] = r;
        vis_buf[wp] = { l, r };
        wp = (wp + 1) & (VIS_BUF - 1);
    }
    vis_wp.store(wp, std::memory_order_release);
}

//timing thread

static void timing_thread() {
    using clk = std::chrono::steady_clock;
    auto next = clk::now();
    while (running.load()) {
        next += std::chrono::microseconds((int)(60'000'000.0 / bpm / 4));
        std::this_thread::sleep_until(next);
        if (!playing.load()) continue;
        int s = play_step.load();
        for (int t = 0; t < TRACKS; ++t)
            if (grid[t][s]) trig[t].store(true);
        play_step.store((s + 1) % loop_len);
    }
}

//fill helpers

static void fill_pattern(int interval) {
    for (int s = cursor_step; s < loop_len; s += interval)
        grid[cursor_track][s] = true;
}

//TUI

static const char* track_names[TRACKS] = {"k", "s", "h", "c"};

static void draw_stereometer() {
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

    for (int row = 0; row < VIS_H; ++row) {
        for (int col = 0; col < VIS_W; ++col) {
            float v = vis_grid[row][col];
            if      (v > 0.65f) printf(COL_BRIGHT "█" COL_RESET);
            else if (v > 0.30f) printf(COL_PURPLE  "•" COL_RESET);
            else if (v > 0.08f) printf(COL_DIM     "·" COL_RESET);
            else                printf(" ");
        }
        printf("\n");
    }
    printf("\n");
}

//synth view

static const char* key_col(int semitone) {
    if (!scale_snap) return COL_PURPLE;
    return in_pentatonic(semitone) ? COL_BRIGHT : COL_DIM;
}

static void draw_key(const char* label, int semitone) {
    printf("%s[%s]%s", key_col(semitone), label, COL_RESET);
}

static void draw_synth() {
    // header row
    printf(COL_PURPLE COL_BOLD "bitjams" COL_RESET
           COL_PURPLE "  bpm: %d  steps: %d  %s  " COL_RESET
           COL_DIM "[synth]" COL_RESET "\n",
           bpm, loop_len, playing.load() ? "▶" : "■");

    // oscillator / octave / scale controls
    printf(COL_DIM " osc " COL_RESET COL_DIM "◄ " COL_RESET
           COL_BRIGHT COL_BOLD "%-8s" COL_RESET COL_DIM "►" COL_RESET
           COL_DIM "  oct " COL_RESET COL_BRIGHT "%d" COL_RESET
           COL_DIM "  scale " COL_RESET COL_BRIGHT "%s" COL_RESET "\n",
           OSC_NAMES[(int)synth_osc], synth_octave,
           scale_snap ? "minor penta" : "chromatic");

    printf("\n");

    // piano keyboard — black keys row
    // Layout (each white key = 3 chars, black key sits between):
    //  [w][e]   [t][y][u]   [o]
    // [a][s][d][f][g][h][j][k][l]
    printf(" ");
    draw_key("w",  1); draw_key("e",  3);
    printf("   ");
    draw_key("t",  6); draw_key("y",  8); draw_key("u", 10);
    printf("   ");
    draw_key("o", 13);
    printf("\n");

    // white keys row
    draw_key("a",  0); draw_key("s",  2); draw_key("d",  4);
    draw_key("f",  5); draw_key("g",  7); draw_key("h",  9);
    draw_key("j", 11); draw_key("k", 12); draw_key("l", 14);
    printf("\n\n");

    // help bar — pad to same line count as grid view
    printf(COL_DIM
           "  z:oct▼  x:oct▲  n:scale  ↑↓:osc  p:play  +/-:bpm  [/]:steps  tab:grid  q:quit"
           COL_RESET "\n");
    printf("\n"); // extra line to match grid view height
}

static void draw_grid() {
    int ps = play_step.load();

    printf(COL_PURPLE COL_BOLD "bitjams" COL_RESET
           COL_PURPLE "  bpm: %d  steps: %d  %s\n" COL_RESET,
           bpm, loop_len, playing.load() ? "▶" : "■");

    printf(COL_DIM "   ");
    for (int s = 0; s < STEPS; ++s)
        printf(s < loop_len ? COL_DIM " %2d" : "   ", s + 1);
    printf(COL_RESET "\n");

    printf("   ");
    for (int s = 0; s < STEPS; ++s)
        printf(s == ps && playing.load() ? COL_HEAD "  ▼" COL_RESET : "   ");
    printf("\n");

    for (int t = 0; t < TRACKS; ++t) {
        printf(COL_PURPLE "%s  " COL_RESET, track_names[t]);
        for (int s = 0; s < STEPS; ++s) {
            bool active  = grid[t][s];
            bool is_cur  = (t == cursor_track && s == cursor_step);
            bool in_loop = (s < loop_len);
            if (!in_loop) {
                printf(COL_DIM "  ·" COL_RESET);
            } else if (is_cur) {
                printf("  \033[7m" COL_PURPLE "%s" COL_RESET, active ? "■" : "·");
            } else if (active) {
                printf(s == ps && playing.load()
                    ? COL_BRIGHT COL_BOLD "  ■" COL_RESET
                    : COL_BRIGHT "  ■" COL_RESET);
            } else {
                printf(COL_DIM "  ·" COL_RESET);
            }
        }
        printf("\n");
    }

    printf("\n" COL_DIM
           "  spc:toggle  1-4:fill interval  arrows:move"
           "  p:play  +/-:bpm  [/]:steps  c:clear  tab:synth  q:quit"
           COL_RESET "\n");
}

static void draw() {
    bool mode_changed = (ui_mode != ui_mode_prev);
    ui_mode_prev = ui_mode;

    if (mode_changed)
        printf("\033[2J\033[H");
    else
        printf("\033[H");

    draw_stereometer();

    if (ui_mode == UIMode::SYNTH)
        draw_synth();
    else
        draw_grid();

    fflush(stdout);
}

//input handling

static void play_synth_key(int semitone) {
    int abs_st = (synth_octave - 4) * 12 + semitone;
    if (scale_snap) abs_st = snap_pentatonic(abs_st);
    synth_trig_freq.store(note_to_freq(abs_st));
}

static void handle_input() {
    if (!stdin_ready(0)) return;
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) <= 0) return;

    // escape sequence (arrow keys)
    if (c == '\033') {
        if (!stdin_ready(1000)) return;
        char seq[2] = {};
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return;
        if (seq[0] != '[') return;
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return;

        if (ui_mode == UIMode::SYNTH) {
            int osc_i = (int)synth_osc;
            if      (seq[1] == 'A') osc_i = (osc_i - 1 + OSC_COUNT) % OSC_COUNT;
            else if (seq[1] == 'B') osc_i = (osc_i + 1) % OSC_COUNT;
            synth_osc = (OscType)osc_i;
            synth_osc_atom.store(osc_i);
        } else {
            switch (seq[1]) {
                case 'A': cursor_track = (cursor_track - 1 + TRACKS) % TRACKS; break;
                case 'B': cursor_track = (cursor_track + 1) % TRACKS; break;
                case 'C': cursor_step  = (cursor_step  + 1) % STEPS;  break;
                case 'D': cursor_step  = (cursor_step  - 1 + STEPS) % STEPS; break;
            }
        }
        return;
    }

    // shared keys (work in both modes)
    switch (c) {
        case 'q': case 'Q': running.store(false); return;
        case 'p': case 'P':
            playing.store(!playing.load());
            if (!playing.load()) play_step.store(0);
            return;
        case '+': case '=': bpm = std::min(300, bpm + 5); return;
        case '-':           bpm = std::max(40,  bpm - 5); return;
        case ']':           loop_len = std::min(16, loop_len + 1); return;
        case '[':           loop_len = std::max(1,  loop_len - 1); return;
        case '\t':
            ui_mode = (ui_mode == UIMode::GRID) ? UIMode::SYNTH : UIMode::GRID;
            return;
    }

    if (ui_mode == UIMode::SYNTH) {
        int st = key_to_semitone(c);
        if (st >= 0) { play_synth_key(st); return; }
        switch (c) {
            case 'z': synth_octave = std::max(1, synth_octave - 1); break;
            case 'x': synth_octave = std::min(7, synth_octave + 1); break;
            case 'n': scale_snap = !scale_snap; break;
        }
    } else {
        switch (c) {
            case ' ':
                grid[cursor_track][cursor_step] = !grid[cursor_track][cursor_step];
                break;
            case '1': fill_pattern(1); break;
            case '2': fill_pattern(2); break;
            case '3': fill_pattern(3); break;
            case '4': fill_pattern(4); break;
            case 'c': case 'C': memset(grid, 0, sizeof(grid)); break;
        }
    }
}

//main

int main() {
    init_terminal();

    bool preset[TRACKS][STEPS] = {
        {1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},
        {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
        {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    };
    memcpy(grid, preset, sizeof(grid));

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = SAMPLE_RATE;
    cfg.dataCallback      = audio_callback;

    ma_device dev;
    if (ma_device_init(nullptr, &cfg, &dev) != MA_SUCCESS) {
        fprintf(stderr, "Failed to init audio device\n");
        return 1;
    }
    ma_device_start(&dev);

    std::thread timer(timing_thread);

    printf("\033[2J");

    using clk = std::chrono::steady_clock;
    auto next_frame = clk::now();
    while (running.load()) {
        handle_input();
        auto now = clk::now();
        if (now >= next_frame) {
            draw();
            next_frame = now + std::chrono::milliseconds(33);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    timer.join();
    ma_device_uninit(&dev);
    printf("\033[2J\033[H");
    return 0;
}
