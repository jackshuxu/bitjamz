#pragma once

#include <atomic>

#include "miniaudio.h"
#include "sequencer.h"

inline constexpr int SAMPLE_RATE = 44100;
inline constexpr int VIS_BUF     = 8192;

struct StereoSample { float l, r; };

// per-drum-kind voice description (display-only for the synth page in this stage)
struct DrumParams {
    const char* label;
    float       base_pitch_hz;
    float       decay_seconds;
    float       noise_mix;     // 0 = pure tonal, 1 = pure noise
};
extern const DrumParams DRUM_PARAMS[DRUM_KINDS];

// stereo visualization ring buffer (audio -> ui)
extern StereoSample     vis_buf[VIS_BUF];
extern std::atomic<int> vis_wp;

// melodic synth trigger atomics (ui -> audio): one per melodic track
extern std::atomic<float> synth_trig_freq[MELODIC_VOICES];
extern std::atomic<int>   synth_osc_atom;

void audio_callback(ma_device* dev, void* out, const void* in, ma_uint32 frames);
