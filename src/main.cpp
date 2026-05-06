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
    bool preset[TRACKS][STEPS] = {
        {1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0},
        {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0},
        {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0},
        {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    };
    std::memcpy(grid, preset, sizeof(grid));

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback); // miniaudio's "connection" object to the OS audio system
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = SAMPLE_RATE;
    cfg.dataCallback      = audio_callback; // callback function the device will call when it needs more samples

    ma_device dev;
    if (ma_device_init(nullptr, &cfg, &dev) != MA_SUCCESS) {
        std::fprintf(stderr, "Failed to init audio device\n");
        return 1;
    }
    ma_device_start(&dev); //tells the OS to begin streaming. The OS spawns a real-time audio thread

    std::thread timer(timing_thread); //sleeps until the next tick -> read play_step -> set trig[t] = true -> advances play_step

    auto screen = ftxui::ScreenInteractive::Fullscreen();
    screen.TrackMouse(false);

    std::thread refresher([&screen] {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            screen.PostEvent(ftxui::Event::Custom);
        }
    });

    screen.Loop(build_ui(screen));

    // ensure refresher exits even if Loop returned via screen.Exit() without our q-handler
    running.store(false);

    refresher.join();
    timer.join();
    ma_device_uninit(&dev);
    return 0;
}
