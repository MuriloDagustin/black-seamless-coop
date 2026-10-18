#pragma once
// Optional EOS transport. All functions run on the game thread; the F10 panel
// reads only coop::Session, never SDK handles or callbacks.
#include <cstdint>
#include <string>
#include "game/coop_net.h"

namespace coop::online {
bool Built();
bool Start(bool host, const std::string& code, const std::string& build);
void Stop();
void Tick();
bool Pumping();
bool Active();
net::Peer Partner();
std::string Code();
std::string Status();
std::string Error();
bool Send(uint64_t peer, const void* data, int bytes);
int Receive(net::Peer& peer, void* data, int cap);
std::string PeerName(uint64_t peer);
} // namespace coop::online
