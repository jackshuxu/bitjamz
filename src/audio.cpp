#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "session.h"

StereoSample     vis_buf[VIS_BUF];
std::atomic<int> vis_wp{0};

// synth_trig_freq is defined in session.cpp (the timing thread is also a
// producer; defining it there keeps the symbol available to the test binary
// which doesn't link audio.cpp).
std::atomic<int>   synth_osc_atom{0};

// Per-track stereo placement (lightly varied to keep things from collapsing to mono).
static constexpr float TRACK_PAN[TRACKS] = {
     0.00f,  0.05f, -0.05f,  0.00f,   // core drums: kick, snare, clap, chat
    -0.30f,  0.30f, -0.15f,  0.15f,   // extended drums: ohat, cowb, tom, cymb
    -0.20f,  0.20f, -0.10f,  0.10f,   // melodic: lead, bass, chord, drone
};

const DrumParams DRUM_PARAMS[DRUM_KINDS] = {
    /* DK_KICK       */ { "kick",         180.f, 0.300f, 0.00f },
    /* DK_SNARE      */ { "snare",        200.f, 0.180f, 0.70f },
    /* DK_CLAP       */ { "clap",           0.f, 0.120f, 1.00f },
    /* DK_CLOSED_HAT */ { "closed_hat",  8000.f, 0.050f, 1.00f },
    /* DK_OPEN_HAT   */ { "open_hat",    8000.f, 0.300f, 1.00f },
    /* DK_COWBELL    */ { "cowbell",      540.f, 0.250f, 0.00f },
    /* DK_TOM        */ { "tom",          140.f, 0.350f, 0.00f },
    /* DK_CYMBAL     */ { "cymbal",      9000.f, 0.800f, 1.00f },
};

struct DrumVoice {
    bool  active;
    float cycle_phase;
    float aux_phase;        // cowbell second osc
    float level_env;
    float elapsed_s;
    float base_pitch_hz;
    float decay_seconds;
    float filter_prev_in;
    float filter_prev_out;
};
static DrumVoice drum_voices[DRUM_KINDS];

static uint32_t lcg_state = 0x12345678u;
static inline float noise() {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (float)(int32_t)lcg_state / 2147483648.f;
}

static void trigger_drum(int kind) {
    DrumVoice& v        = drum_voices[kind];
    v.active            = true;
    v.cycle_phase       = 0.f;
    v.aux_phase         = 0.f;
    v.elapsed_s         = 0.f;
    v.level_env         = 1.f;
    v.base_pitch_hz     = DRUM_PARAMS[kind].base_pitch_hz;
    v.decay_seconds     = DRUM_PARAMS[kind].decay_seconds;
    v.filter_prev_in    = 0.f;
    v.filter_prev_out   = 0.f;
}

static void trigger_kick()       { trigger_drum(DK_KICK); }
static void trigger_snare()      { trigger_drum(DK_SNARE); }
static void trigger_clap()       { trigger_drum(DK_CLAP); }
static void trigger_closed_hat() { trigger_drum(DK_CLOSED_HAT); }
static void trigger_open_hat()   { trigger_drum(DK_OPEN_HAT); }
static void trigger_cowbell()    { trigger_drum(DK_COWBELL); }
static void trigger_tom()        { trigger_drum(DK_TOM); }
static void trigger_cymbal()     { trigger_drum(DK_CYMBAL); }

static float render_drum_voice(int kind) {
    DrumVoice& voice = drum_voices[kind];
    if (!voice.active) return 0.f;

    const float dt = 1.f / SAMPLE_RATE;

    voice.level_env = std::max(0.f, 1.f - voice.elapsed_s / voice.decay_seconds);
    if (voice.level_env <= 0.f) {
        voice.active = false;
        return 0.f;
    }

    float sample = 0.f;
    switch (kind) {
        case DK_KICK: {
            // sine body with a fast pitch drop from base_pitch_hz down to 40 Hz
            float p = 40.f + (voice.base_pitch_hz - 40.f)
                          * std::max(0.f, 1.f - voice.elapsed_s / 0.030f);
            voice.cycle_phase += p * dt;
            if (voice.cycle_phase > 1.f) voice.cycle_phase -= 1.f;
            sample = std::sin(voice.cycle_phase * 2.f * (float)M_PI);
            break;
        }
        case DK_SNARE: {
            // noise rattle + quiet square shell
            voice.cycle_phase += voice.base_pitch_hz * dt;
            if (voice.cycle_phase > 1.f) voice.cycle_phase -= 1.f;
            sample = noise() * 0.7f
                   + ((voice.cycle_phase < 0.5f) ? 1.f : -1.f) * 0.3f;
            break;
        }
        case DK_CLAP: {
            // three short staggered noise bursts
            for (int b = 0; b < 3; ++b) {
                float te = voice.elapsed_s - b * 0.008f;
                if (te >= 0.f && te < 0.015f)
                    sample += noise() * (1.f - te / 0.015f);
            }
            break;
        }
        case DK_CLOSED_HAT: {
            // high-passed noise, short decay (handled by decay_seconds)
            float w = noise();
            float h = 0.97f * (voice.filter_prev_out + w - voice.filter_prev_in);
            voice.filter_prev_in  = w;
            voice.filter_prev_out = h;
            sample = h;
            break;
        }
        case DK_OPEN_HAT: {
            // high-passed noise, long decay
            float w = noise();
            float h = 0.97f * (voice.filter_prev_out + w - voice.filter_prev_in);
            voice.filter_prev_in  = w;
            voice.filter_prev_out = h;
            sample = h;
            break;
        }
        case DK_COWBELL: {
            // two square oscillators at ~540 and ~810 Hz mixed, gives a metallic clang
            voice.cycle_phase += voice.base_pitch_hz * dt;
            if (voice.cycle_phase > 1.f) voice.cycle_phase -= 1.f;
            voice.aux_phase += voice.base_pitch_hz * 1.5f * dt;
            if (voice.aux_phase > 1.f) voice.aux_phase -= 1.f;
            float a = (voice.cycle_phase < 0.5f) ? 1.f : -1.f;
            float b = (voice.aux_phase   < 0.5f) ? 1.f : -1.f;
            sample = (a + b) * 0.5f;
            break;
        }
        case DK_TOM: {
            // sine body with a slower pitch drop than the kick, mid frequency
            float p = 80.f + (voice.base_pitch_hz - 80.f)
                          * std::max(0.f, 1.f - voice.elapsed_s / 0.060f);
            voice.cycle_phase += p * dt;
            if (voice.cycle_phase > 1.f) voice.cycle_phase -= 1.f;
            sample = std::sin(voice.cycle_phase * 2.f * (float)M_PI);
            break;
        }
        case DK_CYMBAL: {
            // bright high-passed noise, long decay
            float w = noise();
            float h = 0.98f * (voice.filter_prev_out + w - voice.filter_prev_in);
            voice.filter_prev_in  = w;
            voice.filter_prev_out = h;
            sample = h;
            break;
        }
    }

    // bit-crush down to 8-bit for that lo-fi pocket-operator edge
    sample = (float)(int8_t)(sample * 127.f) / 127.f;

    voice.elapsed_s += dt;
    return sample * voice.level_env;
}

// Dedicated metronome voice. Outside the 12-track roster so it can't be
// muted/soloed and never competes with user-programmed drums.
struct MetronomeVoice {
    bool  active;
    float phase;
    float elapsed_s;
    float pitch_hz;
};
static MetronomeVoice metronome_voice = {};
static constexpr float METRONOME_DECAY_S = 0.030f;  // PRD: ~30 ms

static void render_metronome_into(float& mix_l, float& mix_r) {
    if (!metronome_voice.active) return;
    const float dt = 1.f / SAMPLE_RATE;
    float env = std::exp(-metronome_voice.elapsed_s / METRONOME_DECAY_S);
    if (env < 0.001f) { metronome_voice.active = false; return; }
    metronome_voice.phase += metronome_voice.pitch_hz * dt;
    if (metronome_voice.phase > 1.f) metronome_voice.phase -= 1.f;
    float s = std::sin(metronome_voice.phase * 2.f * (float)M_PI) * env * 0.35f;
    metronome_voice.elapsed_s += dt;
    mix_l += s;
    mix_r += s;
}

struct SynthVoice {
    bool  active;
    float cycle_phase;
    float level_env;
    float elapsed_s;
    float pitch_hz;
};

// Per-melodic-track polyphony pool. Each new trigger claims an inactive
// voice; if all are active, the longest-running voice is stolen.
static constexpr int VOICES_PER_TRACK = 8;
static SynthVoice synth_voices[MELODIC_VOICES][VOICES_PER_TRACK] = {};

static void trigger_synth_polyphonic(int idx, float pitch_hz) {
    int chosen = -1;
    float worst_elapsed = -1.f;
    int   worst_idx     = 0;
    for (int v = 0; v < VOICES_PER_TRACK; ++v) {
        if (!synth_voices[idx][v].active) { chosen = v; break; }
        if (synth_voices[idx][v].elapsed_s > worst_elapsed) {
            worst_elapsed = synth_voices[idx][v].elapsed_s;
            worst_idx     = v;
        }
    }
    if (chosen < 0) chosen = worst_idx;  // voice-steal: drop oldest
    synth_voices[idx][chosen] = { true, 0.f, 1.f, 0.f, pitch_hz };
}

static float render_one_voice(SynthVoice& v) {
    if (!v.active) return 0.f;

    const float dt = 1.f / SAMPLE_RATE;

    v.level_env = std::max(0.f, 1.f - v.elapsed_s / 0.5f);
    if (v.level_env <= 0.f) {
        v.active = false;
        return 0.f;
    }

    v.cycle_phase += v.pitch_hz * dt;
    if (v.cycle_phase > 1.f) v.cycle_phase -= 1.f;

    float sample = 0.f;
    switch (synth_osc_atom.load(std::memory_order_relaxed)) {
        case 0: sample = (v.cycle_phase < 0.5f) ? 1.f : -1.f; break;             // square
        case 1: sample = 2.f * v.cycle_phase - 1.f; break;                       // saw
        case 2: sample = 2.f * std::abs(2.f * v.cycle_phase - 1.f) - 1.f; break; // triangle
        case 3: sample = std::sin(v.cycle_phase * 2.f * (float)M_PI); break;     // sine
    }

    sample = (float)(int8_t)(sample * 127.f) / 127.f;
    v.elapsed_s += dt;
    return sample * v.level_env * 0.4f;
}

// Sum every active voice in a track's pool. A chord of N notes superposes
// here; trigger time was already separate (one enqueue per onset).
static float render_synth_track(int idx) {
    float sum = 0.f;
    for (int v = 0; v < VOICES_PER_TRACK; ++v) {
        sum += render_one_voice(synth_voices[idx][v]);
    }
    // Soft attenuation to keep N-voice chords from clipping. /sqrt(N) is the
    // textbook equal-power rule; we apply a milder constant so loud single
    // notes still feel loud.
    return sum * 0.6f;
}

void audio_callback(ma_device* dev, void* out, const void* /*in*/, ma_uint32 frames) {
    float* buf = (float*)out;
    SessionState* state = static_cast<SessionState*>(dev->pUserData);

    for (int t = 0; t < TRACKS; ++t) {
        if (!state->trig[t].exchange(false)) continue;
        const TrackDef& td = TRACK_DEFS[t];
        if (td.type == TrackType::DRUM) {
            switch (td.drum_kind) {
                case DK_KICK:       trigger_kick();       break;
                case DK_SNARE:      trigger_snare();      break;
                case DK_CLAP:       trigger_clap();       break;
                case DK_CLOSED_HAT: trigger_closed_hat(); break;
                case DK_OPEN_HAT:   trigger_open_hat();   break;
                case DK_COWBELL:    trigger_cowbell();    break;
                case DK_TOM:        trigger_tom();        break;
                case DK_CYMBAL:     trigger_cymbal();     break;
            }
        } else {
            // Fallback live-trigger path. Sequencer + UI keys go through
            // synth_trig_pool instead so chords don't collapse.
            trigger_synth_polyphonic(td.melodic_idx,
                                     midi_to_hz(state->track_root_midi[t]));
        }
    }

    // Metronome onset: one-shot blip at the latched pitch.
    if (state->metronome_trig.exchange(false)) {
        metronome_voice.active    = true;
        metronome_voice.phase     = 0.f;
        metronome_voice.elapsed_s = 0.f;
        metronome_voice.pitch_hz  = (float)state->metronome_pitch_hz.load();
    }

    // Drain every pool slot. Each surviving freq claims its own voice, so an
    // N-note chord all fired in the same audio block plays as N voices.
    for (int m = 0; m < MELODIC_VOICES; ++m) {
        for (int i = 0; i < TRIG_POOL_SIZE; ++i) {
            float f = synth_trig_pool[m][i].exchange(-1.f);
            if (f > 0.f) trigger_synth_polyphonic(m, f);
        }
    }

    int wp = vis_wp.load(std::memory_order_relaxed);

    for (ma_uint32 i = 0; i < frames; ++i) {
        float mix_l = 0.f, mix_r = 0.f;

        // drums: render once per kind, pan by the (single) track that owns that kind
        for (int t = 0; t < TRACKS; ++t) {
            const TrackDef& td = TRACK_DEFS[t];
            if (td.type != TrackType::DRUM) continue;
            float v = render_drum_voice(td.drum_kind);
            float a = (TRACK_PAN[t] + 1.f) * (float)M_PI * 0.25f;
            mix_l += v * std::cos(a);
            mix_r += v * std::sin(a);
        }
        for (int t = 0; t < TRACKS; ++t) {
            const TrackDef& td = TRACK_DEFS[t];
            if (td.type != TrackType::MELODIC) continue;
            float v = render_synth_track(td.melodic_idx);
            float a = (TRACK_PAN[t] + 1.f) * (float)M_PI * 0.25f;
            mix_l += v * std::cos(a);
            mix_r += v * std::sin(a);
        }

        // Metronome: centred sine blip, summed AFTER the per-track bit-crush
        // so the click stays clean.
        render_metronome_into(mix_l, mix_r);

        float l = (float)(int8_t)(mix_l * 127.f) / 127.f * 0.25f;
        float r = (float)(int8_t)(mix_r * 127.f) / 127.f * 0.25f;
        buf[i * 2 + 0] = l;
        buf[i * 2 + 1] = r;
        vis_buf[wp] = { l, r };
        wp = (wp + 1) & (VIS_BUF - 1);
    }
    vis_wp.store(wp, std::memory_order_release);
}
