#pragma once
// Co-op world sync, stage 4: the host's enemies. The host sends its living enemies 30 times a second; on the joining
// copy each is a puppet of the same spawner, made by that spawner when the host has it, driven from the host's
// state with its own AI off, hurt only by the host (its deaths come as world events), let go when the host's is
// gone. Allies stay each copy's own. Stage 5: each copy has the partner's body, an ally driven from the partner's
// states that takes no harm; the host's enemies see it as one of the squad and shoot at it, their puppets on the
// joining copy then shooting at that player. All of it on the game thread.
#include <cstdint>

#include "kernel/kernel.h"

namespace coop::npcs {
void Stop(X86Context& ctx);

void SetMode(bool host, bool puppets, bool body);  // the host sends, the joining copy plays puppets; the partner's body

// host: the living enemies, one entry each, as many as fit; returns how many
int Snapshot(uint8_t* out, int cap, int& bytes);
// joining copy: the host's latest, its level
void OnSnapshot(const uint8_t* p, int bytes, int count, int8_t level, uint64_t now);
void LogicTick(X86Context& ctx);  // after the logic tick 0x1E7FF0: puppets made and let go
void Died(uint64_t name);         // a puppet of that spawner died here: the host's states of just before do not remake it
void LevelReset();
bool IsBody(uint32_t ch);  // the partner's body: no world event, no puppet
uint32_t Body();           // the partner's body's character, 0 when none
bool Puppets();            // joining copy: the host's enemies are puppets here now (the host heard)
bool HostsEnemies();       // host: its enemies are the partner's puppets
bool IsHostOwned(uint32_t ch);  // joining copy: a puppet of the host's

struct Counters {
    uint32_t snapshots = 0, made = 0, let_go = 0, driven = 0, refused = 0;
    uint32_t bodies = 0, seen = 0, targeted = 0;  // bodies made; perceptions it was in; the ones that took it
};
const Counters& Count();

}  // namespace coop::npcs
