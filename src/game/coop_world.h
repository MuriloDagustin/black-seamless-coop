#pragma once
// Co-op world sync, increment 1: an enemy that dies on one copy dies on the other. Each copy keeps its own AI; an
// enemy is named by its spawner's 64 bit record name (unique in a level) and the count of that spawner's spawns.
// Events go over an acknowledged channel: each frame until acknowledged, in order.
// All of it runs on the game thread.
#include <cstdint>

#include "kernel/kernel.h"

namespace coop::world {

void SetMode(bool net, int stage);  // stage 0: off; 1: deaths; 2: and the partner fires triggers
uint8_t Load();                     // this copy's level loads so far, for the partner
int8_t LoadLevel();                 // and the level of the last
// the partner's, from its packets: a newer load of a level voids the partner's deaths of its earlier loads of it
void PartnerLoad(uint8_t load, int8_t level);

// sender
int Outgoing(uint8_t* out, int cap, int& bytes, uint64_t now);
void Hold(uint64_t now);  // the partner cannot be heard: the events wait for it
void Acked(uint32_t seq);

// receiver
void Incoming(const uint8_t* p, int bytes, int count, uint64_t now);
uint32_t Taken();
void Reset();  // a new partner session: both directions start again from 1

void Tick(X86Context& ctx);  // the partner's events applied, once a frame of play
void LevelReset();           // the level (re)loads: no enemy is known any more
// stage 2: the one shot triggers fire for the partner's body too, after the logic tick 0x1E7FF0 of the manager
void PartnerTriggers(X86Context& ctx, uint32_t mgr);

struct Counters {
    uint32_t sent = 0, taken = 0, applied = 0, waiting = 0, lost = 0, stale = 0, triggers = 0;
};
const Counters& Count();

}  // namespace coop::world
