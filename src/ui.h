#pragma once
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <string>
#include <cstdint>

class SessionState;

ftxui::Component build_session_ui(ftxui::ScreenInteractive& screen,
                                  SessionState& state);

struct StartupChoice {
    enum Mode { SOLO, HOST, JOIN, QUIT } mode = QUIT;
    std::string ip;             // join only
    uint16_t    room = 0;       // host: pre-generated; join: user-typed
};

// Blocks on its own ftxui Loop until the user picks. last_error, if
// non-empty, is rendered on the landing screen (e.g. after a failed join).
StartupChoice run_startup_page(const std::string& last_error);
