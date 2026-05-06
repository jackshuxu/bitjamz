#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "sequencer.h"

StereoSample     vis_buf[VIS_BUF];
std::atomic<int> vis_wp{0};

std::atomic<float> synth_trig_freq{-1.f};
std::atomic<int>   synth_osc_atom{0};

static constexpr float TRACK_PAN[TRACKS] = { 0.f, 0.30f, -0.30f, 0.10f };

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

struct SynthVoice { bool active; float phase, env, t, freq; };
static SynthVoice synth_voice = {};

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

void audio_callback(ma_device* /*dev*/, void* out, const void* /*in*/, ma_uint32 frames) {
    float* buf = (float*)out;

    for (int t = 0; t < TRACKS; ++t) //for each track: read & clear, trigger voice
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
