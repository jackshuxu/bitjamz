#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "audio.h"
#include "sequencer.h"
#include "ui.h"

int main() {
    // default pattern on the drum tracks (matches the MPC layout)
    // track 0 = kick  (v): quarter notes
    // track 1 = snare (b): backbeat
    // track 3 = chat  (m): eighth notes
    static const bool kick_row[STEPS]  = {1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0};
    static const bool snare_row[STEPS] = {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0};
    static const bool chat_row[STEPS]  = {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0};
    std::memcpy(grid[0], kick_row,  sizeof(kick_row));
    std::memcpy(grid[1], snare_row, sizeof(snare_row));
    std::memcpy(grid[3], chat_row,  sizeof(chat_row));

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = SAMPLE_RATE;
    cfg.dataCallback      = audio_callback;

    ma_device dev;
    if (ma_device_init(nullptr, &cfg, &dev) != MA_SUCCESS) {
        std::fprintf(stderr, "Failed to init audio device\n");
        return 1;
    }
    ma_device_start(&dev);

    std::thread timer(timing_thread);

    auto screen = ftxui::ScreenInteractive::Fullscreen();
    screen.TrackMouse(false);

    std::thread refresher([&screen] {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            screen.PostEvent(ftxui::Event::Custom);
        }
    });

    screen.Loop(build_ui(screen));

    running.store(false);

    refresher.join();
    timer.join();
    ma_device_uninit(&dev);
    return 0;
}
