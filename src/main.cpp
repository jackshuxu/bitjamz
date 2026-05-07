#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>

#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "audio.h"
#include "session.h"
#include "ui.h"

// Runs one full bitjams session end-to-end: owns the SessionState,
// starts the audio device, the timing thread, and the ftxui event loop,
// and tears them all down on exit.
static void main_session() {
    auto state = std::make_shared<SessionState>();

    // default pattern on the drum tracks (matches the MPC layout)
    static const bool kick_row[STEPS]  = {1,0,0,0,1,0,0,0,1,0,0,0,1,0,0,0};
    static const bool snare_row[STEPS] = {0,0,1,0,0,0,1,0,0,0,1,0,0,0,1,0};
    static const bool chat_row[STEPS]  = {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,0};
    std::memcpy(state->grid[0], kick_row,  sizeof(kick_row));
    std::memcpy(state->grid[1], snare_row, sizeof(snare_row));
    std::memcpy(state->grid[3], chat_row,  sizeof(chat_row));

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = SAMPLE_RATE;
    cfg.dataCallback      = audio_callback;
    cfg.pUserData         = state.get();

    ma_device dev;
    if (ma_device_init(nullptr, &cfg, &dev) != MA_SUCCESS) {
        std::fprintf(stderr, "Failed to init audio device\n");
        return;
    }
    ma_device_start(&dev);

    std::thread timer(timing_thread, std::ref(*state));

    auto screen = ftxui::ScreenInteractive::Fullscreen();
    screen.TrackMouse(false);

    std::thread refresher([&screen, state] {
        while (state->running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            screen.PostEvent(ftxui::Event::Custom);
        }
    });

    screen.Loop(build_ui(screen, *state));

    state->running.store(false);

    refresher.join();
    timer.join();
    ma_device_uninit(&dev);
}

int main() {
    // TODO: startup page (solo / host / join) lands here, then dispatches
    // into main_session with the chosen networking mode.
    main_session();
    return 0;
}
