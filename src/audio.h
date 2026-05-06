#pragma once

#include <atomic>

#include "miniaudio.h"

inline constexpr int SAMPLE_RATE = 44100;
inline constexpr int VIS_BUF     = 8192;

struct StereoSample { float l, r; };

// stereo visualization ring buffer (audio -> ui)
extern StereoSample     vis_buf[VIS_BUF];
extern std::atomic<int> vis_wp;

// synth trigger atomics (ui -> audio)
extern std::atomic<float> synth_trig_freq;
extern std::atomic<int>   synth_osc_atom;

void audio_callback(ma_device* dev, void* out, const void* in, ma_uint32 frames);
