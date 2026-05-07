#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include "sequencer.h"

ftxui::Component build_ui(ftxui::ScreenInteractive& screen, SessionState& state);

// session entry point: spins up audio, timing, and the ftxui event loop.
void session();
