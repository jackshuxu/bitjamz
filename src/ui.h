#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

class SessionState;

ftxui::Component build_ui(ftxui::ScreenInteractive& screen, SessionState& state);
