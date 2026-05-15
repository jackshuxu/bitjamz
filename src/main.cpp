#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <string>

#include "net_compat.h"
#ifndef _WIN32
#  include <ifaddrs.h>
#  include <net/if.h>
#endif

#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "audio.h"
#include "session.h"
#include "ui.h"
#include "network.h"

namespace App {
    inline std::atomic<bool> running{true};
}

// Picks the first non-loopback IPv4 interface address. Falls back to
// "127.0.0.1" if no LAN interface is up. Used only to print the host's
// reachable address on the HOST_INFO screen.
std::string get_local_ipv4() {
#ifdef _WIN32
    return "127.0.0.1";
#else
    struct ifaddrs* head = nullptr;
    if (::getifaddrs(&head) != 0 || head == nullptr) {
        return "127.0.0.1";
    }
    std::string result = "127.0.0.1";
    for (struct ifaddrs* ifa = head; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        auto* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        if (sin->sin_addr.s_addr == 0) continue;

        char buf[INET_ADDRSTRLEN] = {0};
        if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr) {
            result = buf;
            break;
        }
    }
    ::freeifaddrs(head);
    return result;
#endif
}

// Runs one full bitjams session end-to-end: owns the SessionState's audio
// device, timing thread, and ftxui event loop. Returns when the user quits.
static void main_session(std::shared_ptr<SessionState> state) {
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

    state->running.store(true);
    std::thread timer(timing_thread, std::ref(*state));

    auto screen = ftxui::ScreenInteractive::Fullscreen();
    screen.TrackMouse(false);

    std::thread refresher([&screen, state] {
        while (state->running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
            screen.PostEvent(ftxui::Event::Custom);
        }
    });

    screen.Loop(build_session_ui(screen, *state));

    state->running.store(false);
    refresher.join();
    timer.join();
    ma_device_uninit(&dev);
}

int main() {
    net_init();
    std::string last_error;

    while (App::running.load()) {
        StartupChoice choice = run_startup_page(last_error);
        last_error.clear();

        switch (choice.mode) {
            case StartupChoice::QUIT:
                App::running.store(false);
                break;

            case StartupChoice::SOLO: {
                auto state = make_solo_session_state();
                main_session(state);
                break;
            }

            case StartupChoice::HOST: {
                auto state = make_solo_session_state();
                state->session_id = choice.room;
                Network::host(state);
                main_session(state);
                Network::stop();
                break;
            }

            case StartupChoice::JOIN: {
                auto state = Network::join(choice.ip.c_str(), choice.room);
                if (!state) {
                    last_error = "Could not join — host unreachable or wrong room.";
                    break;
                }
                main_session(state);
                Network::stop();
                break;
            }
        }
    }
    net_cleanup();
    return 0;
}
