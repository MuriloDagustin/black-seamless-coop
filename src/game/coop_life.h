#pragma once
// Co-op lives: a player who dies while a partner lives is down, not a failed mission. The mission fails only when
// the last one alive dies. A down player is revived when the living team reaches a checkpoint: an advance to the
// next unit (where the game saves one) or an objective completed. It is then brought to a living partner.
#include <cstdint>
#include <string>

#include "kernel/kernel.h"

namespace coop::life {

bool Down();       // this copy's player is dead in this play
bool HoldDeath();  // down while a partner lives: no mission failed for it (game thread)
bool Held();       // the same, as last seen by the game thread: for other threads (the HUD)
std::string HeldStatus();  // the HUD's line while held: the partner watched (any thread)
int Watching();            // the partner whose view this copy shows while down, -1 none
void WatchNext();          // the next living partner's (any thread)
void WatchAhead(float metres);  // the camera ahead of the watched eye (the test channel)
void PadWatch(int port, uint16_t buttons);  // the pad thread: A pressed while down watches the next
void Tick(X86Context& ctx);  // each frame: revive at the team's checkpoint
bool Revive(X86Context& ctx, const char* why);
void LevelReset();

}  // namespace coop::life
