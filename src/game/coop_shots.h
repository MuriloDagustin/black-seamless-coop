#pragma once
// Co-op shots: each bullet this copy's player fires goes to the partner, whose copy fires it again from the same
// origin in the same direction through the game's own bullet spawn 0x1DB7B0, so its hits, decals, impact sounds,
// destruction and damage are the game's. A tracer from the partner's muzzle shows where it went.
// All of it runs on the game thread.
#include <cstddef>
#include <cstdint>

namespace coop::shots {

constexpr int kMaxPellets = 16;

#pragma pack(push, 1)
struct Pellet {
    float origin[4];
    float dir[4];  // unit, the spread already in it
};
// one pull of the trigger: its pellets, as 0x1DB7B0 got them
struct Event {
    uint32_t seq;  // the sender's count, from 1
    uint32_t ms;   // the sender's clock at the shot
    int8_t level;
    uint8_t type;  // the weapon's type index, a hint: id is what counts
    uint8_t bits;  // 1 silenced, 2 scoped
    uint8_t count;
    uint32_t id_lo, id_hi;  // the weapon type's 64 bit name hash
    float range, speed;
    uint8_t flags;  // 0x1DB7B0's last argument
    uint8_t pad[3];
    Pellet pellets[kMaxPellets];
};
#pragma pack(pop)
constexpr int kEventHead = int(offsetof(Event, pellets));
inline int Bytes(const Event& e) { return kEventHead + int(e.count) * int(sizeof(Pellet)); }

void SetMode(bool net, bool echo);  // what Init settled: shots go to a partner, or come back to this copy

// sender: the events the partner has not acknowledged, oldest first, as many as fit; returns how many
int Outgoing(uint8_t* out, int cap, int& bytes, uint64_t now);
void Acked(uint32_t seq);  // the partner has taken every event up to seq

// receiver
void Incoming(const uint8_t* p, int bytes, int count, int8_t level, uint64_t now);
uint32_t Taken();  // the last event taken, the ack
void Reset();      // a new partner session: its count starts again
void Echo(uint64_t now);  // the echo: this copy's own shots come back after a while

bool GhostWeapon(uint32_t weapon);  // a weapon the partner's bullets fire with, not one of the game's
bool SteppingPartner();             // the partner's bullets are stepped (their hits are the partner's)

struct Counters {
    uint32_t sent = 0;      // events made here
    uint32_t taken = 0;     // events from the partner
    uint32_t fired = 0;     // their pellets fired here
    uint32_t dropped = 0;   // pellets not fired: no room, another level, not in play
    uint32_t flashes = 0;   // muzzle flashes started at the partner's gun
    uint32_t gunshots = 0;  // gunshots played there
    uint32_t heard = 0;     // shots the enemies were told of
};
// the gunshot slot: its address, its bound sound set and state (0 when there is none)
void Gunshot(uint32_t& slot, uint32_t& sound, uint32_t& state);
const Counters& Count();

}  // namespace coop::shots
