#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <fstream>
#include <poll.h>
#include <iostream>
#include <optional>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stop_token>
#include <format>

#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "audio.h"
#include "session.h"
#include "ui.h"
#include "network.h"

// Runs one full bitjams session end-to-end: owns the SessionState,
// starts the audio device, the timing thread, and the ftxui event loop,
// and tears them all down on exit.

// is_shared determines whether a session is public or private (yet to be used)
static void main_session(bool is_shared) {
    std::cout << "Before\n";

    //if (is_shared) {
    //    uint16_t session_id = SessionState::generate_unique_id();
    //}

    // init state struct
    uint16_t init_src_dirty[TRACKS]= {0};
    init_src_dirty[0] = 0x1111; // Kick
    init_src_dirty[1] = 0x4444; // Snare
    init_src_dirty[3] = 0x5555; // Chat
    int32_t init_bpm = 120;
    uint16_t init_track_active = 0xFFFF;
    MsgState init_state(init_bpm, 0, init_track_active, init_src_dirty);
    
    auto state = std::make_shared<SessionState>(init_state, is_shared);
    std::cout << "After\n";

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

std::optional<uint32_t> string_to_IPv4(const std::string& ip_str) {
    struct in_addr addr;
    
    if (inet_pton(AF_INET, ip_str.c_str(), &addr) != 1) {
        // Invalid IP address
        return std::nullopt;
    }
    
    return ntohl(addr.s_addr);  // Convert from network to host byte order
}

void execute_repl_command(std::string repl_command) {
    if (repl_command.empty()) {
        return;
    }

    std::stringstream tok_stream(repl_command);
    std::vector<std::string> toks;
    std::string tok;

    while (tok_stream >> tok) {
        toks.push_back(tok);
    }

    char command = toks[0][0];

    switch (command) {
        case 's': {
            if (toks.size() > 1) {
                std::cout << "usage: s\n";
            }

            main_session(true);

            break;
        }
        case 'p': {
            if (toks.size() > 1) {
                std::cout << "usage: p\n";
            }

            main_session(false);

            break;
        }
        case 'c': {
            if (toks.size() < 3) {
                std::cout << "usage: c <ip> <room>\n";
                break;
            }
            
            std::string address_string = toks[1];
            std::optional<uint32_t> address_num = string_to_IPv4(address_string);

            if (!address_num.has_value()) {
                std::cout << "invalid address provided\n";
            } else {
                uint16_t room = std::stoi(toks[2]);
                Network::connect_to_server(address_num.value(), room);
            }

            break;
        }
        case 'q': {
            if (toks.size() > 1) {
                std::cout << "usage: q\n";

                break;
            }    

            // TO-DO: Some state teardown alongside global bool
            App::running.store(false);

            break;
        }
        case 'h': {
            std::cout << "s            : Starts a new public session and assigns room number.\n";
            std::cout << "p            : Starts a new private session.\n";
            std::cout << "c <ip> <room>: Attempts to connect to the desired ip address' room number.\n";
            std::cout << "q            : Quit application.\n";
            std::cout << "h            : Help menu (Yer lookin' at it).\n";
            break;
        }
        default: {
            std::cout << "Invalid command. Type h for a list of valid commands.\n";
            break;
        }
    }
}

void repl_handler() {
    std::string repl_command;

    while (App::running.load()) {
        std::getline(std::cin, repl_command);
        execute_repl_command(repl_command);
    }
}

int main() {
    // TODO: startup page (solo / host / join) lands here, then dispatches
    // into main_session with the chosen networking mode.
    std::cout << "Welcome to TUI-DAW-Network Application!\n";
    std::cout << "Enter your first command (press h for help menu)" << std::endl;

    std::thread receive_connections_thread(Network::receive_connections);
    repl_handler();
    receive_connections_thread.join();

    return 0;
}