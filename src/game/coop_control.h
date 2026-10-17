#pragma once
// The test control channel: with --coop-control [port] this copy takes plain text commands on 127.0.0.1 (port
// 45500 + its --instance by default) and plays with the pad they give it, the window in front or not. Its own
// controllers and keyboard are not read then. For tests that drive two copies at once.
#include <cstdint>

#include "kernel/kernel.h"

namespace input {
struct Pad;
}

namespace coop::control {

void Init(uint16_t port);  // before boot
bool Enabled();
void Poll(X86Context& ctx);            // game thread, each frame: the commands, the aim
bool Override(int port, input::Pad& pad);  // the pad thread: true when the channel gives the pad
void BeforeTriggers();  // game thread, before the logic tick 0x1E7FF0 tests the triggers

}  // namespace coop::control
