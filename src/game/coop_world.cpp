// Co-op world sync, increment 1: enemy deaths.
// Every AI character is made by 0x1DF440, out of the pool [0x2D1980] (32 of 0x3C0 at +0xA0). Its +0x08 holds the
// 64 bit name of the spawner that made it, a record name unique in the level, and a spawner keeps at most one live
// character (its tick waits while one lives, 0x1B309F): a death names the spawner and how many it had made so far.
// A death leaves through 0x1DA240 (to a ragdoll) or 0x1DA1A0 (the character let go, its +0x384 2); the partner's is
// played with the game's own death 0x205C60, so ragdoll, listeners and scripts all run.
// The counts drift where one copy alone removes a character (the cull of 0x1E6860, a unit unloaded), so a death is
// not matched by count: with one live character a spawner, the partner's death is this copy's live one of that
// spawner, or its next. Unless this copy killed its own first and the partner had not heard of it when it killed:
// two kills of the same one crossing, the partner's is stale. Each event carries what its sender had taken.
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "game/coop.h"
#include "game/coop_control.h"
#include "game/coop_npcs.h"
#include "game/coop_shots.h"
#include "game/coop_world.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using layout::kWorld;
using layout::kWorldPlayer;

namespace coop::world {

namespace {

constexpr uint32_t kPool = 0x2D1980;
constexpr uint32_t kPoolChars = 0xA0;
constexpr uint32_t kCharSize = 0x3C0;
constexpr int kChars = 32;
// character
constexpr uint32_t kCharName = 0x08;  // u64, the spawner's; zeroed at death
constexpr uint32_t kCharPos = 0xA0;
constexpr uint32_t kCharHealth = 0x2F0;
constexpr uint32_t kCharControl = 0x324;
constexpr uint32_t kCharState = 0x384;  // 0 alive, 1 ragdoll, 2 dead, 4 free
constexpr uint32_t kCharType = 0x394;
constexpr uint32_t kCharTeam = 0x39C;  // 0 the player's
constexpr uint32_t kCharUnit = 0x388;
constexpr uint32_t kControlActive = 0x90;  // 1: TakeDamage takes damage (0x1E76EC)
constexpr uint32_t kPlayerForward = 0xF0;
constexpr uint32_t kPlayerFeet = 0xA0;
constexpr uint32_t kWorldLevel = 0x5AEC;
constexpr uint32_t kWorldLastPos = 0x1C0;  // the player's position a frame ago, written in its vf17 (0x1F09C7)
// triggers: the logic manager's two lists, counts u16 at +0 and +2
constexpr uint32_t kTriggerLists = 0x48;
constexpr uint32_t kTriggerVtable = 0x28EF88;  // type 11
constexpr uint32_t kTriggerTest = 0x48;        // vf18: ecx the trigger; &pos, actor; ret 8; al fired
constexpr uint32_t kTriggerArmed = 0x11C;      // byte
constexpr uint32_t kTriggerMode = 0x120;       // 0 one shot, 1 2 enter exit, 3 continuous, 4 visibility
constexpr uint32_t kTriggerAction = 0x124;     // 0 Fire, 1 activate the next unit, 2 advance the unit
constexpr float kWalkMax = 3.0f;               // farther between two of the partner's tests is a jump
// props: the loaded units' slots (world+0x49D0, two of 0x880), their arrays of kind 3 and kind 4 props
constexpr uint32_t kUnitSlots = 0x49D0, kUnitSlotSize = 0x880;
constexpr uint32_t kSlotLoaded = 0x1C;  // the slot's state +0 of a loaded unit
constexpr uint32_t kSlotActive = 0x38;  // a byte: the unit activated
constexpr uint32_t kFrameCount = 0x2F4368;
constexpr uint32_t kSlotUnit = 0x08, kSlotTriggers = 0x48;  // its unit, its triggers (a list of the manager's)
constexpr uint32_t kWorldUnit = 0x5AF0;                     // the current unit
constexpr uint32_t kTrigger67Vtable = 0x28EB40;  // type 67: its test 0x1C8B40 also asks for units (0x1C8BE1)
// the level's flow
constexpr uint32_t kAdvance = 0x1B73B0;       // thiscall ecx the world; the unit; ret 4: world+0x5AE8 = 1
constexpr uint32_t kActivateNext = 0x1C7620;  // esi the world, ecx the unit; al: world+0x5AE8 = 2
constexpr uint32_t kTriggerUnit = 0x1C1260;   // eax a trigger: the unit it names
constexpr uint32_t kInside = 0x1C9270;        // eax a type 11 trigger, ebx &pos16; al the player's way in it
constexpr uint32_t kStallResume = 0x1BE8F0;   // plain: the awaited unit came
constexpr uint32_t kGame = 0x2D45F0;
constexpr uint32_t kGameStall = 0x2108C;      // 1: the next unit is awaited, the world stops (0x20D66C); 2 resumed
constexpr uint32_t kGameStalled = 0x210C9;    // byte: the world update is shut (0x1F10E5)
constexpr uint32_t kInLevel = 0x20F78;        // the in-level state, +0x0C its requests: 1 failed, 2 complete, 4 end
constexpr uint32_t kGameState = 0x21070;      // the current state
constexpr uint32_t kFailed = 0x1F45C0;        // the in-level state's vf5: the mission failed; ret 4
constexpr uint32_t kComplete = 0x1F45B0;      // vf6: the level complete; ret 4
constexpr uint32_t kEnd = 0x1F4590;           // vf7: the level's end, two words; ret 0xC
constexpr uint32_t kSlotActive2 = 0x38;
constexpr uint64_t kAloneMs = 3000;  // the partner out of this level that long: an advance waits no more
constexpr uint32_t kWorldLevelInfo = 0x5AF4;  // +0x11 the level's units
constexpr uint32_t kWorldStream = 0x5AE8;     // the streaming step asked: 1 3 advance, 2 4 activate (0x1F112C)
constexpr uint32_t kLogicMgr = 0x2D45D0;      // the logic manager: its trigger lists +0x48, +0x4C
constexpr uint32_t kSlotProps3 = 0x18, kSlotCount3 = 0x30, kProp3Size = 0x1D0;
constexpr uint32_t kSlotProps4 = 0x1C, kSlotCount4 = 0x34, kProp4Size = 0x200;
constexpr uint32_t kPropDef = 0x118, kDefStates = 0x48;  // the definition, its states of 0xD0
constexpr uint32_t kStateSize = 0xD0, kStateNext = 0xCC, kStateFlags = 0x5A;  // next int16; flags 1 2: damage breaks it
constexpr uint32_t kProp3State = 0x1A4, kProp3Health = 0x1A0;  // the state index, -1 destroyed
constexpr uint32_t kProp4Part = 0x1A0, kProp4Parts = 0x1F5;    // parts of 12: health, state; their count, a byte
constexpr uint32_t kPropDamage3 = 0x1E8C50;  // esi the prop; damage; ret 4
constexpr uint32_t kPropChange3 = 0x1E3DB0;  // eax the prop; to destroyed, 0; ret 8
constexpr uint32_t kPropDamage4 = 0x1E5070;  // eax the prop; part, damage, from a blast; ret 0xC
constexpr uint32_t kPropChange4 = 0x1E4B80;  // eax the prop; part, to destroyed; ret 8
constexpr size_t kPropsMax = 1024;
// projectiles: [0x2D1998] makes them; side 3 the player's, 1 its team's AI, 0 the enemy's
constexpr uint32_t kProjectiles = 0x2D1998;
constexpr uint32_t kThrowGrenade = 0x1DCE60;  // edi the maker; &matrix, &velocity, side; ret 0xC
constexpr uint32_t kFireRocket = 0x1E35F0;    // ecx the maker, edi the side; &matrix, &velocity, fx lo, fx hi; ret 0x10
constexpr uint32_t kThrowHand = 0x1DD000;     // a thrown hand grenade (0x1E32A0): edi the maker; &matrix, &velocity, side, flag; ret 0x10
constexpr uint32_t kPlayerSide = 3, kAllySide = 1;
constexpr uint64_t kProjectileLateMs = 400;

constexpr uint32_t kDie = 0x205C60;  // eax noise class; mgr, victim, part, &dir, dmg, instigator; ret 0x18
constexpr uint32_t kMeleeHit = 0x1DFA90;  // thiscall ecx the victim; 0, &dir, 0, the attacker; ret 0x10
constexpr uint32_t kScoreKill = 0x1FF640;  // esi the player's stats [0x2D12CC] (+4 the kill's kind); plain ret
constexpr uint32_t kStats = 0x2D12CC;
constexpr uint32_t kDeathMgr = 0x2D19B0;
// noise classes: [kClassOfType][weapon type] is a type's, kTypeOfClass a class's type (0xFF none, as class 0)
constexpr uint32_t kClassOfType = 0x2D680C;
constexpr uint32_t kTypeOfClass = 0x2D67F8;
constexpr uint32_t kMaxClass = 0x12;
constexpr uint32_t kArmoury = 0x2D12D0;  // [[it]]+1: the weapon types, a byte

constexpr uint64_t kOldMs = 30000;  // an event the partner has not taken by then, link up all the while, goes
constexpr size_t kPendingMax = 512;
constexpr uint32_t kRetryFrames = 120;
constexpr size_t kWaitingMax = 256;
constexpr int kProbeLines = 1500;

enum Type : uint8_t { kDeath = 1, kProp, kProjectile, kMelee, kFlow, kObjective };

#pragma pack(push, 1)
// A death names the spawner and carries the killing hit. A prop's state (kProp) puts the prop's id in name, its
// kind (3, 4) in hit, the part (0xFF for kind 3) in part, the state index (int16, -1 destroyed) in ordinal and the
// health in dmg. A projectile (kProjectile) has its own fields at the end.
struct Event {
    uint32_t seq;
    uint32_t ack;  // the last of the receiver's events its sender had taken when it made this one
    uint8_t type;
    int8_t level;
    uint8_t load;  // the sender's level loads: events of a load the partner has left behind are not played
    uint8_t part;
    uint32_t name_lo, name_hi;
    uint16_t ordinal;  // the sender's count of the spawner's characters, this one included: for the log
    uint8_t hit;       // 1: part, dir, dmg and noise are the killing hit's; 2: the receiver's player killed it
    uint8_t noise;     // its noise class
    float dir[4];
    float dmg;
    // a projectile's own, sent with a projectile only
    uint8_t proj_kind;  // 0 grenade (0x1DCE60), 1 rocket (0x1E35F0), 2 hand grenade (0x1DD000)
    uint8_t proj_side;
    uint16_t age;  // ms it waited on its sender before this datagram
    uint32_t fx_lo, fx_hi;  // the rocket's trail
    float matrix[16];
    float vel[4];
};
#pragma pack(pop)

// a death's or a prop's, on the wire: the event up to the projectile's fields
constexpr int kHeadSize = int(offsetof(Event, proj_kind));
int WireSize(uint8_t type) { return type == kProjectile ? int(sizeof(Event)) : kHeadSize; }

struct Npc {
    uint32_t ch = 0;
    uint64_t name = 0;
    uint16_t ordinal = 0;
    bool alive = false;
    bool remote = false;  // dying by the partner's word: not sent back
    bool hit = false;
    bool partner = false;  // killed by the partner's bullets stepped here
    uint8_t part = 0;
    uint8_t noise = 0;
    float dir[4] = {};
    float dmg = 0;
};

struct Waiting {
    Event e;
    uint32_t frames;  // not ready to die for this many
};

bool g_net = false;
int g_stage = 0;
bool g_applying = false;
int g_culling = 0;
uint8_t g_load = 0;
Counters g_count;

// sender
uint32_t g_seq = 0;
struct Stamped {
    uint64_t at;
    Event e;
};
std::deque<Stamped> g_pending;

// receiver
uint32_t g_taken = 0;
int g_partner_load = -1;          // the partner's level loads, and the level of its last
int8_t g_partner_level = -1;
std::vector<Waiting> g_waiting;  // deaths of enemies not spawned here yet, or not ready to die
std::vector<Event> g_props;      // the partner's prop states for props not loaded here yet
bool g_props_retry = false;      // a unit was activated: its props may be there now
struct Arrived {
    uint64_t at;
    Event e;
};
std::deque<Arrived> g_projectiles;  // to fire on the next step, if not late
bool g_rocket_replay = false;       // a partner's rocket made here: launched as the player's (0x1DDA70 bl 0)
// the last state sent of each prop (by id, kind and part): a change to the same is not sent again
struct PropKey {
    uint32_t lo, hi;
    uint8_t kind, part;
    bool operator==(const PropKey&) const = default;
};
struct PropKeyHash {
    size_t operator()(const PropKey& k) const {
        return std::hash<uint64_t>()(uint64_t(k.lo) | uint64_t(k.hi) << 32) ^ (size_t(k.kind) << 8 | k.part);
    }
};
struct PropSent {
    int16_t state;
    float health;
    uint32_t frame;
};
std::unordered_map<PropKey, PropSent, PropKeyHash> g_props_sent;

// Secondary progress is a set of script action IDs, not a maximum of the two
// players' category totals: each player may collect a different blackmail item.
// These survive a peer reconnect and are cleared only when this level reloads.
std::unordered_set<uint64_t> g_objectives_seen;
std::unordered_set<uint64_t> g_collected_pickups;
uint64_t g_objective_pickup = 0;  // the pickup whose removal is firing its scripts

uint32_t Objective(uint64_t name) {
    uint32_t world = R32(kWorld);
    if (!world) return 0;
    uint32_t array = R32(world + 0x930), count = std::min(R32(world + 0x934), 4096u);
    for (uint32_t i = 0; array && i < count; i++) {
        uint32_t row = array + i * 12, def = R32(row);
        if (def && (uint64_t(R32(def)) | uint64_t(R32(def + 4)) << 32) == name) return row;
    }
    return 0;
}

void HideCollectedPickups(X86Context& ctx) {
    uint32_t mgr = R32(0x2D0D64);
    if (!mgr || g_collected_pickups.empty()) return;
    static constexpr hle::Map kRemove = hle::Make({hle::ecx, hle::esi, hle::stack});
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t p = mgr + i * 0x160;
        uint64_t id = uint64_t(R32(p + 8)) | uint64_t(R32(p + 12)) << 32;
        if (id && g_collected_pickups.count(id) && G<uint8_t>(p + 0x13B) && !G<uint8_t>(p + 0x154))
            hle::Call(ctx, 0x1BED90, kRemove, {mgr, p, 0});
    }
}

// a unit advance of this copy's player's waiting for the partner to go through the same trigger
struct Advance {
    uint32_t trigger = 0;
    uint64_t id = 0;
    uint32_t unit = 0;
    uint64_t partner_seen = 0;
    float last[3] = {};
    bool have_last = false;
    float dir = 0;  // which way across the trigger's thin side this copy's player went (+1, -1; 0 not seen)
};
Advance g_advance;
uint32_t g_partner_crossed_unit = 0;  // the farthest advance trigger the partner was seen through, this play
bool g_rescue_on = false;  // "get me out of here" under way
uint64_t g_rescue_at = 0;
uint64_t g_rescue_behind = 0;  // since when the partner's unit is one this copy has left
constexpr uint64_t kBehindMs = 2000;
constexpr uint64_t kRescueMs = 15000;
bool g_stalled = false;    // this copy's world stopped by us at the next unit while the advance waits
bool g_advancing = false;  // our own call of the advance
bool g_partner_test = false;  // the partner's way through the triggers is being tested
uint32_t g_partner_unit = 0;  // the unit the partner's copy last advanced to, this play
constexpr uint8_t kFlowAdvanced = 8;  // a flow event: the sender's copy advanced to the unit in name_lo
constexpr uint8_t kFlowContinue = 16, kFlowRestart = 17;  // the failed mission's retry, from its checkpoint or start
constexpr uint32_t kContinueMission = 0x1240D0;  // the failed page's choices (front end, plain)
constexpr uint32_t kRestartMission = 0x124190;
constexpr uint64_t kRetryWaitMs = 10000;  // a retry waits that long for this copy's own failed page
bool g_retrying = false;

// this copy's characters, its spawns of each spawner, and its last death sent of each
Npc g_npcs[kChars];
std::unordered_map<uint64_t, uint16_t> g_spawns;
std::unordered_map<uint64_t, uint32_t> g_own;
int8_t g_level = -1;  // the level of this copy's last load
uint32_t g_dir = 0;   // a 16 aligned vector
int g_probe_lines = 0;

uint32_t Player() { return R32(kWorld) + kWorldPlayer; }

uint64_t Name(uint32_t lo, uint32_t hi) { return uint64_t(lo) | uint64_t(hi) << 32; }

uint64_t NowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

int SlotOf(uint32_t ch) {
    uint32_t pool = R32(kPool);
    if (!pool || ch < pool + kPoolChars) return -1;
    uint32_t off = ch - (pool + kPoolChars);
    if (off % kCharSize) return -1;
    uint32_t i = off / kCharSize;
    return i < uint32_t(kChars) ? int(i) : -1;
}

#define PROBE(...)                             \
    do {                                       \
        if (g_probe_lines < kProbeLines) {     \
            g_probe_lines++;                   \
            LOGR(__VA_ARGS__);                 \
        }                                      \
    } while (0)

// a noise class 0x205C60 can look its weapon type up by: the hit's, else weapon type 0's, as the melee passes
uint32_t NoiseClass(uint32_t wanted) {
    uint32_t armoury = R32(kArmoury);
    uint32_t types = armoury && R32(armoury) ? G<uint8_t>(R32(armoury) + 1) : 0;
    auto usable = [&](uint32_t c) {
        if (c < 1 || c > kMaxClass) return false;
        int8_t t = G<int8_t>(kTypeOfClass + c);
        return t >= 0 && uint32_t(t) < types;
    };
    if (usable(wanted)) return wanted;
    uint32_t table = R32(kClassOfType);
    uint32_t fallback = table ? R32(table) : 0;
    return usable(fallback) ? fallback : 0;
}

// an event of this copy's to the partner: numbered, stamped with what this copy has taken, its level and load
void Post(Event& e) {
    e.seq = ++g_seq;
    e.ack = g_taken;
    uint32_t world = R32(kWorld);
    e.level = world ? G<int8_t>(world + kWorldLevel) : -1;
    e.load = g_load;
    g_pending.push_back({NowMs(), e});
    if (g_pending.size() > kPendingMax) {
        g_pending.pop_front();
        g_count.lost++;
    }
    g_count.sent++;
}

void Send(const Npc& n) {
    Event e{};
    e.type = kDeath;
    e.name_lo = uint32_t(n.name);
    e.name_hi = uint32_t(n.name >> 32);
    e.ordinal = n.ordinal;
    if (n.hit) {
        e.hit = n.partner ? 3 : 1;
        e.part = n.part;
        e.noise = n.noise;
        std::memcpy(e.dir, n.dir, sizeof e.dir);
        e.dmg = n.dmg;
    } else {
        // no hit seen (a blast, a fall): along this player's view
        std::memcpy(e.dir, HostPtr(Player() + kPlayerForward), 12);
        e.dir[3] = 0;
        e.dmg = 100;
    }
    Post(e);
    g_own[n.name] = e.seq;
}

// a death on this copy: the partner's enemy too, unless the partner said so first; a cull is this copy's own
void Died(uint32_t ch, const char* how) {
    int s = SlotOf(ch);
    if (s < 0) return;
    Npc& n = g_npcs[s];
    if (n.ch != ch || !n.alive) return;
    n.alive = false;
    coop::npcs::Died(n.name);
    bool send = g_net && g_stage >= 1 && !g_applying && !g_culling && !n.remote && n.name && !coop::npcs::IsBody(ch);
    PROBE("npc- s%d %016llX #%u %s%s", s, (unsigned long long)n.name, n.ordinal, how,
          g_applying ? " (partner's)" : g_culling ? " (culled)" : send ? " sent" : "");
    if (send) Send(n);
}

Npc* LiveOf(uint64_t name) {
    for (Npc& n : g_npcs)
        if (n.alive && n.name == name) return &n;
    return nullptr;
}

enum class Result { Done, Later, Wait, Stale };

bool Finite(const float* v, int n);

Result Kill(X86Context& ctx, Npc& n, const Event& e) {
    uint32_t ch = n.ch;
    if (R32(ch + kCharState) != 0) return Result::Done;  // dying here already
    uint32_t control = R32(ch + kCharControl);
    if (!control || R32(control + kControlActive) != 1) return Result::Later;
    uint32_t noise = NoiseClass(e.hit & 1 ? e.noise : 0);
    if (!noise) return Result::Later;
    if (!g_dir) g_dir = mem::AllocSystem(16, 16);
    float dir[4] = {e.dir[0], e.dir[1], e.dir[2], 0};
    float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!std::isfinite(len) || !(len > 0.01f)) dir[0] = 0, dir[1] = 0, dir[2] = 1, len = 1;
    for (int i = 0; i < 3; i++) dir[i] /= len;
    std::memcpy(HostPtr(g_dir), dir, 16);
    float dmg = std::isfinite(e.dmg) && e.dmg > 0 ? std::min(e.dmg, 10000.0f) : 100.0f;
    n.remote = true;
    G<float>(ch + kCharHealth) = 0.0f;  // as TakeDamage leaves it before its call of the death (0x1E795B)
    static constexpr hle::Map kDieMap =
        hle::Make({hle::eax, hle::stack, hle::stack, hle::stack, hle::stack, hle::stack, hle::stack});
    g_applying = true;
    // this copy's player killed it with its bullets there: its kill, as TakeDamage scores one (0x1E796D)
    uint32_t stats = R32(kStats);
    if ((e.hit & 2) && stats) {
        static constexpr hle::Map kScoreMap = hle::Make({hle::esi});
        hle::Call(ctx, kScoreKill, kScoreMap, {stats});
        W32(stats + 4, 0);
    }
    hle::Call(ctx, kDie, kDieMap, {noise, R32(kDeathMgr), ch, e.part, g_dir, std::bit_cast<uint32_t>(dmg), Player()});
    g_applying = false;
    g_count.applied++;
    PROBE("npc! %016llX #%u (here #%u) killed by the partner", (unsigned long long)n.name, e.ordinal, n.ordinal);
    return Result::Done;
}

// The partner's character of a spawner died: this copy's live one of it, or its next, unless this copy's own death
// of that spawner had not reached the partner yet when it killed (the same one, killed on both)
Result Apply(X86Context& ctx, const Event& e) {
    uint64_t name = Name(e.name_lo, e.name_hi);
    auto own = g_own.find(name);
    if (own != g_own.end() && int32_t(own->second - e.ack) > 0) return Result::Stale;
    if (Npc* live = LiveOf(name)) return Kill(ctx, *live, e);
    // the host's enemies are puppets here: one not made here is gone, the next of its spawner another
    if (coop::npcs::Puppets()) {
        coop::npcs::Died(name);
        return Result::Stale;
    }
    return Result::Wait;  // not made here yet, or removed and to come again
}

// the partner's mission failed, level complete or level end: this copy's too, if in that play and in the level
Result ApplyFlow(X86Context& ctx, const Event& e) {
    if (e.ordinal != 0xFFFF && uint8_t(e.ordinal) != g_load) return Result::Stale;  // of a play of before
    if (e.part == kFlowAdvanced || e.part == kFlowContinue || e.part == kFlowRestart) return Result::Done;  // FlowTick's
    uint32_t game = R32(kGame);
    if (!game) return Result::Stale;
    uint32_t state = game + kInLevel;
    if (R32(game + kGameState) != state) return Result::Wait;  // a menu or a page: when back in the level
    uint32_t flags = R32(state + 0x0C);
    // where its vf1 0x20EB60 takes each request: the failed and complete states, the end's (+0x10 | +0x14 nonzero)
    uint32_t sub = R32(state + 0x08);
    if (e.part != 1 && e.part != 2) return Result::Done;
    if (sub == game + 0x2103C || sub == game + 0x2104C) return Result::Done;  // failed or complete here already
    static constexpr hle::Map kOne = hle::Make({hle::ecx, hle::stack});
    g_applying = true;
    if (e.part == 1 && !(flags & 1)) hle::Call(ctx, kFailed, kOne, {state, 0});
    if (e.part == 2 && !(flags & 2)) hle::Call(ctx, kComplete, kOne, {state, 0});
    g_applying = false;
    g_count.applied++;
    LOGR("coop: the partner's %s here too", e.part == 1 ? "mission failed" : "level complete");
    return Result::Done;
}

void SendFlow(uint8_t kind, uint32_t a1, uint32_t a2) {
    if (!g_net || g_stage < 6 || (g_applying && kind != kFlowAdvanced) || g_retrying) return;
    Event e{};
    e.type = kFlow;
    e.part = kind;
    e.name_lo = a1, e.name_hi = a2;
    e.ordinal = g_partner_load < 0 ? 0xFFFF : uint16_t(uint8_t(g_partner_load));  // the partner's play it is for
    Post(e);
    if (kind != kFlowAdvanced)
        LOGR("coop: %s, to the partner too", kind == 1   ? "mission failed"
                                             : kind == 2 ? "level complete"
                                             : kind == 4 ? "level end"
                                             : kind == kFlowContinue ? "continue the mission"
                                                                     : "restart the mission");
}

// kObjective: name is the unique action; dir[0..2] carry the objective's two
// words and message hash as raw bits, dir[3]/dmg the collectible's two words.
Result ApplyObjective(X86Context& ctx, const Event& e) {
    if (e.ordinal != 0xFFFF && uint8_t(e.ordinal) != g_load) return Result::Stale;
    uint32_t row = Objective(Name(std::bit_cast<uint32_t>(e.dir[0]), std::bit_cast<uint32_t>(e.dir[1])));
    if (!row) return Result::Wait;
    uint32_t def = R32(row);
    if (!(G<uint8_t>(def + 0x15) & 0x3C)) return Result::Stale;
    // Apply before the game's complete request is processed, including when a
    // packet arrived on the frame that the local end trigger fired.
    uint64_t id = Name(e.name_lo, e.name_hi);
    if (g_objectives_seen.insert(id).second && R32(row + 8) != 3) {
        if (!R32(row + 8)) W32(row + 8, 1);  // its local activation may be a frame behind
        static constexpr hle::Map kProgress = hle::Make({hle::esi, hle::eax});
        g_applying = true;
        hle::Call(ctx, 0x1BA430, kProgress, {row, std::bit_cast<uint32_t>(e.dir[2])});
        g_applying = false;
        g_count.applied++;
        LOGR("coop: secondary objective action %016llX collected here too", (unsigned long long)id);
    }
    uint64_t pickup = Name(std::bit_cast<uint32_t>(e.dir[3]), std::bit_cast<uint32_t>(e.dmg));
    if (pickup) g_collected_pickups.insert(pickup);
    return Result::Done;
}

// Called before the original script action, so its event precedes any mission
// complete event it may cause. Returning true suppresses an action already seen.
bool ObjectiveAction(uint32_t action) {
    if (!g_net || g_stage < 6 || !action) return false;
    uint32_t data = R32(action + 0x20);
    if (!data || G<uint8_t>(data + 0x0C) != 1) return false;
    uint32_t row = Objective(Name(R32(data), R32(data + 4)));
    if (!row || !(G<uint8_t>(R32(row) + 0x15) & 0x3C)) return false;
    uint64_t id = Name(R32(action + 8), R32(action + 12));
    if (!id) return false;
    if (g_objectives_seen.count(id)) return true;
    uint32_t game = R32(kGame), state = R32(row + 8);
    if (!game || R32(game + kGameState) != game + kInLevel || (state != 1 && state != 2)) return false;
    g_objectives_seen.insert(id);
    Event e{};
    e.type = kObjective;
    e.name_lo = uint32_t(id), e.name_hi = uint32_t(id >> 32);
    e.ordinal = g_partner_load < 0 ? 0xFFFF : uint16_t(uint8_t(g_partner_load));
    e.dir[0] = std::bit_cast<float>(R32(data));
    e.dir[1] = std::bit_cast<float>(R32(data + 4));
    e.dir[2] = std::bit_cast<float>(R32(data + 8));
    e.dir[3] = std::bit_cast<float>(uint32_t(g_objective_pickup));
    e.dmg = std::bit_cast<float>(uint32_t(g_objective_pickup >> 32));
    Post(e);
    LOGR("coop: secondary objective action %016llX sent, pickup %016llX", (unsigned long long)id,
         (unsigned long long)g_objective_pickup);
    return false;
}

// the joining player's melee on the host's enemy: its hit there, by the joining player's body
Result ApplyMelee(X86Context& ctx, const Event& e) {
    uint32_t ch = LiveCharacter(Name(e.name_lo, e.name_hi));
    if (!ch) return Result::Stale;
    if (!g_dir) g_dir = mem::AllocSystem(16, 16);
    float dir[4] = {e.dir[0], e.dir[1], e.dir[2], 0};
    if (!Finite(dir, 3)) return Result::Stale;
    std::memcpy(HostPtr(g_dir), dir, 16);
    uint32_t by = coop::npcs::Body();
    static constexpr hle::Map kMeleeMap = hle::Make({hle::ecx, hle::stack, hle::stack, hle::stack, hle::stack});
    // not as the partner's event: the death it makes here goes to the partner as the host's own (its ragdoll there)
    hle::Call(ctx, kMeleeHit, kMeleeMap, {ch, 0, g_dir, 0, by ? by : Player()});
    g_count.applied++;
    PROBE("melee! %08X%08X by the partner", e.name_hi, e.name_lo);
    return Result::Done;
}

// the partner loaded that level again: its enemies there live again, its deaths of before are void
void DropOld(uint8_t load, int8_t level) {
    auto old = [&](const Event& e) { return e.level == level && e.load != load; };
    std::erase_if(g_waiting, [&](const Waiting& w) {
        if (old(w.e)) g_count.lost++;
        return old(w.e);
    });
    std::erase_if(g_props, [&](const Event& e) { return old(e); });
}

// --- props: a state change on one copy is played on the other where it is behind; a state only goes on along
// its chain (state +0xCC the next, -1 destroyed), so the most broken wins and the two meet

int16_t Next(uint32_t prop, int16_t state) {
    if (state < 0) return -1;
    uint32_t states = R32(R32(prop + kPropDef) + kDefStates);
    return int16_t(G<int16_t>(states + uint32_t(state) * kStateSize + kStateNext));
}

// target on from here along the chain: this copy is behind
bool Behind(uint32_t prop, int16_t here, int16_t target) {
    if (here == target || here < 0) return false;
    if (target < 0) return true;
    for (int i = 0, s = here; i < 16 && s >= 0; i++) {
        s = Next(prop, int16_t(s));
        if (s == target) return true;
    }
    return false;
}

// a prop by its id in this copy's loaded and active units, 0 when not in one here. A unit loaded ahead and not
// active yet has its props set up on its activation (0x1EB440: their vf18 resets them), one unloading still lists
// its old ones
uint32_t FindProp(uint32_t lo, uint32_t hi, uint8_t kind) {
    uint32_t world = R32(kWorld);
    if (!world) return 0;
    for (uint32_t slot = 0; slot < 2; slot++) {
        uint32_t s = world + kUnitSlots + slot * kUnitSlotSize;
        if (R32(s) != kSlotLoaded || !G<uint8_t>(s + kSlotActive)) continue;  // 0x1EB445, 0x1EB683, 0x1EB42E
        uint32_t array = R32(s + (kind == 3 ? kSlotProps3 : kSlotProps4));
        uint32_t n = R32(s + (kind == 3 ? kSlotCount3 : kSlotCount4));
        uint32_t stride = kind == 3 ? kProp3Size : kProp4Size;
        if (!array || n > 4096) continue;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t p = array + i * stride;
            if (R32(p + 0x08) == lo && R32(p + 0x0C) == hi) return p;
        }
    }
    return 0;
}

// the state and health of a kind 3 prop, or of a part of a kind 4 one
bool PropState(uint32_t p, uint8_t kind, uint8_t part, int16_t& state, float& health) {
    if (kind == 3) {
        state = int16_t(int32_t(R32(p + kProp3State)));
        health = G<float>(p + kProp3Health);
        return true;
    }
    if (part >= G<uint8_t>(p + kProp4Parts)) return false;
    uint32_t entry = R32(p + kProp4Part) + uint32_t(part) * 12;
    health = G<float>(entry);
    state = int16_t(int32_t(R32(entry + 4)));
    return true;
}

enum class PropResult { Done, Absent };

// the partner's state played here: damage enough to break this state, as a hit would, so its effects, model, debris,
// listeners and a barrel's blast all come; the state change itself when the state takes no damage
PropResult ApplyProp(X86Context& ctx, const Event& e) {
    uint8_t kind = e.hit, part = e.part;
    int16_t target = int16_t(e.ordinal);
    uint32_t p = FindProp(e.name_lo, e.name_hi, kind);
    if (!p) return PropResult::Absent;
    static constexpr hle::Map kDamage3 = hle::Make({hle::esi, hle::stack});
    static constexpr hle::Map kChange3 = hle::Make({hle::eax, hle::stack, hle::stack});
    static constexpr hle::Map kDamage4 = hle::Make({hle::eax, hle::stack, hle::stack, hle::stack});
    static constexpr hle::Map kChange4 = hle::Make({hle::eax, hle::stack, hle::stack});
    for (int step = 0; step < 8; step++) {
        int16_t here;
        float health;
        if (!PropState(p, kind, part, here, health) || !Behind(p, here, target)) break;
        uint32_t states = R32(R32(p + kPropDef) + kDefStates);
        uint8_t flags = G<uint8_t>(states + uint32_t(here) * kStateSize + kStateFlags);
        uint32_t dmg = std::bit_cast<uint32_t>(std::max(health, 0.0f) + 1.0f);
        g_applying = true;
        if (flags & 3) {
            if (kind == 3) hle::Call(ctx, kPropDamage3, kDamage3, {p, dmg});
            else hle::Call(ctx, kPropDamage4, kDamage4, {p, part, dmg, 0});
        }
        int16_t now;
        PropState(p, kind, part, now, health);
        if (now == here) {  // no damage taken in this state: the change itself
            uint32_t to_destroyed = target < 0 ? 1 : 0;
            if (kind == 3) hle::Call(ctx, kPropChange3, kChange3, {p, to_destroyed, 0});
            else hle::Call(ctx, kPropChange4, kChange4, {p, part, to_destroyed});
            PropState(p, kind, part, now, health);
        }
        g_applying = false;
        if (now == here) break;
        g_count.props++;
        PROBE("prop! %08X%08X k%u p%d %d -> %d (partner's %d)", e.name_hi, e.name_lo, kind, part == 0xFF ? -1 : part,
              here, now, target);
    }
    return PropResult::Done;
}

// --- projectiles: the player's grenades and rockets fired again on the partner's copy, the partner an ally there

void FireProjectile(X86Context& ctx, const Event& e) {
    // the partner's player's as an ally's; the host's enemy's as an enemy's (0)
    uint32_t side = e.proj_side == kPlayerSide ? kAllySide : 0;
    static uint32_t matrix = 0, vel = 0;
    if (!matrix) matrix = mem::AllocSystem(64, 16), vel = mem::AllocSystem(16, 16);
    std::memcpy(HostPtr(matrix), e.matrix, 64);
    std::memcpy(HostPtr(vel), e.vel, 16);
    uint32_t mgr = R32(kProjectiles);
    if (!mgr) return;
    g_applying = true;
    if (e.proj_kind == 0) {
        static constexpr hle::Map kGrenade = hle::Make({hle::edi, hle::stack, hle::stack, hle::stack});
        hle::Call(ctx, kThrowGrenade, kGrenade, {mgr, matrix, vel, side});
    } else if (e.proj_kind == 2) {
        static constexpr hle::Map kHand = hle::Make({hle::edi, hle::stack, hle::stack, hle::stack, hle::stack});
        hle::Call(ctx, kThrowHand, kHand, {mgr, matrix, vel, side, e.fx_lo & 0xFF});
    } else {
        static constexpr hle::Map kRocket =
            hle::Make({hle::ecx, hle::edi, hle::stack, hle::stack, hle::stack, hle::stack});
        g_rocket_replay = true;
        hle::Call(ctx, kFireRocket, kRocket, {mgr, side, matrix, vel, e.fx_lo, e.fx_hi});
        g_rocket_replay = false;
    }
    g_applying = false;
    g_count.projectiles++;
    static const char* kKinds[] = {"grenade", "rocket", "hand grenade"};
    PROBE("proj! %s of the partner at %.1f %.1f %.1f", kKinds[e.proj_kind % 3], double(e.matrix[12]),
          double(e.matrix[13]), double(e.matrix[14]));
}

bool Finite(const float* v, int n) {
    for (int i = 0; i < n; i++)
        if (!std::isfinite(v[i])) return false;
    return true;
}

// a state change of this copy's own to the partner: a unit's prop, by its id (debris of the pools has none)
void SendProp(uint32_t p, uint8_t kind, uint8_t part) {
    if (!g_net || g_stage < 3 || g_applying) return;
    uint32_t lo = R32(p + 0x08), hi = R32(p + 0x0C);
    if ((!lo && !hi) || FindProp(lo, hi, kind) != p) return;
    int16_t state;
    float health;
    if (!PropState(p, kind, part, state, health)) return;
    uint32_t frame = R32(kFrameCount);
    auto [sent, fresh] = g_props_sent.try_emplace(PropKey{lo, hi, kind, part}, PropSent{state, health, frame});
    if (!fresh) {
        // 0x1E3DB0 twice for one change (a state of both flags); a prop reset by its unit's activation and broken
        // again is a change again
        bool same = sent->second.state == state && sent->second.health == health && frame - sent->second.frame <= 1;
        sent->second = {state, health, frame};
        if (same) return;
    }
    Event e{};
    e.type = kProp;
    e.ordinal = uint16_t(state);
    e.dmg = health;
    e.name_lo = lo, e.name_hi = hi;
    e.hit = kind;
    e.part = part;
    Post(e);
    PROBE("prop- %08X%08X k%u p%d now %d sent", hi, lo, kind, part == 0xFF ? -1 : part, int16_t(e.ordinal));
}

void SendProjectile(uint8_t kind, uint32_t side, uint32_t matrix, uint32_t vel, uint32_t fx_lo, uint32_t fx_hi) {
    Event e{};
    e.type = kProjectile;
    e.proj_kind = kind;
    e.proj_side = uint8_t(side);
    std::memcpy(e.matrix, HostPtr(matrix), 64);
    std::memcpy(e.vel, HostPtr(vel), 16);
    e.fx_lo = fx_lo, e.fx_hi = fx_hi;
    Post(e);
    static const char* kKinds[] = {"grenade", "rocket", "hand grenade"};
    PROBE("proj- %s sent at %.1f %.1f %.1f", kKinds[kind % 3], double(e.matrix[12]), double(e.matrix[13]),
          double(e.matrix[14]));
}

bool SendsProjectiles() { return g_net && g_stage >= 3 && !g_applying; }

}  // namespace

void SetMode(bool net, int stage) {
    g_net = net;
    g_stage = stage;
}

uint8_t Load() { return g_load; }

void PartnerLoad(uint8_t load, int8_t level) {
    if (g_partner_load >= 0 && int8_t(load - uint8_t(g_partner_load)) <= 0) return;
    g_partner_load = load;
    g_partner_level = level;
    g_partner_unit = 0;  // its advances and crossings were of its old play
    g_partner_crossed_unit = 0;
    DropOld(load, level);
}

int Outgoing(uint8_t* out, int cap, int& bytes, uint64_t now) {
    bytes = 0;
    while (!g_pending.empty() && now - g_pending.front().at > kOldMs) {
        g_pending.pop_front();
        g_count.lost++;
    }
    // a projectile not taken by then would come too late to be fired: it goes (the receiver takes any newer)
    std::erase_if(g_pending, [&](const Stamped& s) { return s.e.type == kProjectile && now - s.at > kProjectileLateMs; });
    int n = 0;
    for (const Stamped& s : g_pending) {
        int size = WireSize(s.e.type);
        if (bytes + size > cap || n == 255) break;
        Event e = s.e;
        e.age = uint16_t(std::min<uint64_t>(now - s.at, 0xFFFF));
        std::memcpy(out + bytes, &e, size_t(size));
        bytes += size;
        n++;
    }
    return n;
}

void Hold(uint64_t now) {
    // the partner cannot be heard: the events wait for it, their age does not run; its projectiles would be late
    std::erase_if(g_pending, [](const Stamped& s) { return s.e.type == kProjectile; });
    for (Stamped& s : g_pending) s.at = now;
}

void Acked(uint32_t seq) {
    while (!g_pending.empty() && int32_t(g_pending.front().e.seq - seq) <= 0) g_pending.pop_front();
}

void Incoming(const uint8_t* p, int bytes, int count, uint64_t now) {
    for (int i = 0, at = 0; i < count && at + kHeadSize <= bytes; i++) {
        Event e{};
        int size = WireSize(p[at + offsetof(Event, type)]);
        if (at + size > bytes) break;
        std::memcpy(&e, p + at, size_t(size));
        at += size;
        // every datagram starts at the oldest event not acknowledged: newer is the next one
        if (int32_t(e.seq - g_taken) <= 0) continue;
        g_taken = e.seq;
        g_count.taken++;
        PartnerLoad(e.load, e.level);
        bool reloaded = e.level == g_partner_level && e.load != uint8_t(g_partner_load);
        bool wanted = (e.type == kDeath && g_stage >= 1) || ((e.type == kProp || e.type == kProjectile) && g_stage >= 3) ||
                      (e.type == kMelee && g_stage >= 4) || ((e.type == kFlow || e.type == kObjective) && g_stage >= 6);
        if (!wanted || reloaded) {
            g_count.lost++;  // from before the partner loaded that level again
            continue;
        }
        if (e.type == kProp) {
            if (g_props.size() >= kPropsMax) g_props.erase(g_props.begin());
            g_props.push_back(e);
            g_props_retry = true;
            continue;
        }
        if (e.type == kProjectile) {
            // its age on the sender counts: the receiver's clock starts on arrival
            if (Finite(e.matrix, 16) && Finite(e.vel, 4) && e.proj_kind <= 2 && e.age <= kProjectileLateMs)
                g_projectiles.push_back({now - e.age, e});
            if (g_projectiles.size() > 32) g_projectiles.pop_front();
            continue;
        }
        if (g_waiting.size() >= kWaitingMax) {
            g_waiting.erase(g_waiting.begin());
            g_count.lost++;
        }
        g_waiting.push_back({e, 0});
    }
}

uint32_t Taken() { return g_taken; }

void Reset() {
    g_taken = 0;
    g_partner_load = -1;
    g_partner_level = -1;
    g_waiting.clear();
    g_props.clear();
    g_projectiles.clear();
    g_seq = 0;
    g_pending.clear();
    g_own.clear();
    g_props_sent.clear();
    g_partner_unit = 0;
    g_partner_crossed_unit = 0;
}

void Tick(X86Context& ctx) {
    if (g_stage < 1) return;
    uint32_t world = R32(kWorld);
    int8_t level = world ? G<int8_t>(world + kWorldLevel) : int8_t(-1);
    // the partner's projectiles, at once or not at all
    uint64_t now = NowMs();
    while (!g_projectiles.empty()) {
        Arrived a = g_projectiles.front();
        g_projectiles.pop_front();
        if (a.e.level == level && now - a.at <= kProjectileLateMs) FireProjectile(ctx, a.e);
        else g_count.lost++;
    }
    // the partner's prop states: tried as they come and again when a unit comes in
    if (g_props_retry) {
        g_props_retry = false;
        std::erase_if(g_props, [&](const Event& e) { return e.level == level && ApplyProp(ctx, e) == PropResult::Done; });
    }
    if (g_waiting.empty()) {
        HideCollectedPickups(ctx);
        return;
    }
    for (size_t i = 0; i < g_waiting.size();) {
        Waiting& w = g_waiting[i];
        if (w.e.level != level) {  // the partner's level, not ours yet: it waits
            i++;
            continue;
        }
        Result r = w.e.type == kObjective ? ApplyObjective(ctx, w.e)
                   : w.e.type == kMelee  ? ApplyMelee(ctx, w.e)
                   : w.e.type == kFlow ? ApplyFlow(ctx, w.e)
                                       : Apply(ctx, w.e);
        if (r == Result::Done || r == Result::Stale || (r == Result::Later && ++w.frames > kRetryFrames)) {
            if (r == Result::Stale) g_count.stale++;
            if (r == Result::Later) g_count.lost++;
            g_waiting.erase(g_waiting.begin() + ptrdiff_t(i));
            continue;
        }
        i++;
    }
    g_count.waiting = uint32_t(g_waiting.size());
    HideCollectedPickups(ctx);
}

// From the loader's bullet reset (0x1EDE4C), the new level already in world+0x5AEC (0x1EDD64)
void LevelReset() {
    g_objectives_seen.clear();
    g_collected_pickups.clear();
    g_objective_pickup = 0;
    for (Npc& n : g_npcs) n = {};
    g_spawns.clear();
    g_own.clear();
    g_props_sent.clear();
    g_advance = {};
    g_partner_crossed_unit = 0;
    g_partner_unit = 0;
    g_stalled = false;
    g_rescue_on = false;
    uint32_t world = R32(kWorld);
    int8_t level = world ? G<int8_t>(world + kWorldLevel) : int8_t(-1);
    // ours of this level, loaded again, are void on the partner; of another level they still stand
    std::erase_if(g_pending, [&](const Stamped& s) { return s.e.level == level; });
    // the partner's of any other level would only meet a later play of it
    std::erase_if(g_waiting, [&](const Waiting& w) { return w.e.level != level; });
    std::erase_if(g_props, [&](const Event& e) { return e.level != level; });
    g_projectiles.clear();
    g_props_retry = true;
    coop::npcs::LevelReset();
    coop::control::LevelReset();
    g_level = level;
    g_load++;
}

int8_t LoadLevel() { return g_level; }

uint32_t LiveCharacter(uint64_t name) {
    Npc* n = LiveOf(name);
    return n && R32(n->ch + kCharState) == 0 ? n->ch : 0;
}

bool Applying() { return g_applying; }

bool WaitingForPartner() { return g_advance.trigger != 0; }

uint8_t WaitingUnit() { return uint8_t(g_advance.unit); }

namespace {

// the world stopped by us goes on
void Resume(X86Context& ctx) {
    if (!g_stalled) return;
    g_stalled = false;
    uint32_t game = R32(kGame);
    if (!game) return;
    if (G<uint8_t>(game + kGameStalled)) {
        static constexpr hle::Map kNone = hle::Make({});
        hle::Call(ctx, kStallResume, kNone, {});
    } else if (R32(game + kGameStall) == 1) {
        W32(game + kGameStall, 0);  // asked, not taken yet (0x20D65D)
    }
}

// a unit slot of this copy's holding that unit, 0 none
uint32_t SlotOfUnit(uint32_t world, uint32_t unit) {
    for (uint32_t k = 0; k < 2; k++) {
        uint32_t s = world + kUnitSlots + k * kUnitSlotSize;
        if (R32(s + kSlotUnit) == unit) return s;
    }
    return 0;
}

// the farthest unit the partner has reached in this play: its copy's current unit, the one it waits to advance to
// (it went through that advance's trigger), the advances it told of and the advance triggers it was seen through
uint32_t PartnerReached() {
    int waiting = 0;
    int unit = coop::PartnerUnit(nullptr, &waiting);
    uint32_t r = std::max(g_partner_unit, g_partner_crossed_unit);
    if (unit > 0) r = std::max(r, uint32_t(std::max(unit, waiting)));
    return r;
}

// where a point is across a trigger's thin side, in its own frame (+0x70 world to local), and that side's half size
float AcrossTrigger(uint32_t t, const float p[3], float& half) {
    const float* h = &G<float>(t + 0x130);
    int k = h[0] <= h[1] && h[0] <= h[2] ? 0 : h[1] <= h[2] ? 1 : 2;
    half = h[k];
    float local = G<float>(t + 0xA0 + uint32_t(k) * 4);
    for (int j = 0; j < 3; j++) local += p[j] * G<float>(t + 0x70 + uint32_t(j) * 16 + uint32_t(k) * 4);
    return local;
}

// the waiting advance given up: its trigger armed again, so this copy's player asks for it once more going through
void CancelAdvance(X86Context& ctx, const char* why) {
    uint32_t t = g_advance.trigger;
    if (t && R32(t) == kTriggerVtable && Name(R32(t + 0x08), R32(t + 0x0C)) == g_advance.id)
        G<uint8_t>(t + kTriggerArmed) = 1;
    if (t) LOGR("coop: the advance to unit %u waits no more: %s", g_advance.unit, why);
    g_advance = {};
    Resume(ctx);
}

void Advance(X86Context& ctx, uint32_t world, uint32_t unit) {
    static constexpr hle::Map kAdvanceMap = hle::Make({hle::ecx, hle::stack});
    g_advancing = true;
    hle::Call(ctx, kAdvance, kAdvanceMap, {world, unit});
    g_advancing = false;
}

// this copy's next unit activated (0x1C7620 with the current unit: world+0x5AE8 = 2, 0x1EB440 two frames on)
void Activate(X86Context& ctx, uint32_t world, uint32_t current) {
    static constexpr hle::Map kActivateMap = hle::Make({hle::esi, hle::ecx});
    hle::Call(ctx, kActivateNext, kActivateMap, {world, current});
}

void ApplyAdvance(X86Context& ctx, const char* why) {
    uint32_t world = R32(kWorld);
    uint32_t unit = g_advance.unit;
    if (!world) return;
    // this copy's player back on the side it came from: the unit it stands in would go
    float half;
    float side = AcrossTrigger(g_advance.trigger, &G<float>(Player() + kPlayerFeet), half);
    if (g_advance.dir != 0 && side * g_advance.dir < -half) {
        CancelAdvance(ctx, "this player went back");
        return;
    }
    g_advance = {};
    Resume(ctx);
    Advance(ctx, world, unit);
    LOGR("coop: advance to unit %u, %s", unit, why);
}

// "get me out of here": this copy brought to the partner's unit one streaming step a frame (the next unit
// activated, then advanced to), and the player put by the partner once that unit is here
void RescueTick(X86Context& ctx) {
    if (!g_rescue_on) return;
    uint32_t world = R32(kWorld);
    uint64_t now = NowMs();
    if (!world || now - g_rescue_at > kRescueMs) {
        LOGR("coop: get me out of here: given up, the partner's unit did not come");
        g_rescue_on = false;
        return;
    }
    int next_active = 0, waiting = 0;
    int unit = coop::PartnerUnit(&next_active, &waiting);
    if (unit <= 0) {
        LOGR("coop: get me out of here: the partner is not in this level");
        g_rescue_on = false;
        return;
    }
    // where the partner stands: its current unit, or the one it waits to advance to (it is past that trigger)
    uint32_t target = uint32_t(std::max(unit, waiting));
    uint32_t current = R32(world + kWorldUnit);
    if (current > target) {  // the partner's copy may be a frame or two into the same advance
        if (!g_rescue_behind) g_rescue_behind = now;
        if (now - g_rescue_behind > kBehindMs) {
            LOGR("coop: get me out of here: the partner's unit %u is left behind here", target);
            g_rescue_on = false;
        }
        return;
    }
    g_rescue_behind = 0;
    if (g_stalled) Resume(ctx);
    if (R32(world + kWorldStream)) return;  // a streaming step of the game's still going
    if (current < target) {
        uint32_t s = SlotOfUnit(world, current + 1);
        if (!s || R32(s) != kSlotLoaded) return;  // still loading
        if (!G<uint8_t>(s + kSlotActive2)) {
            Activate(ctx, world, current);
            return;
        }
        g_advance = {};
        Advance(ctx, world, current + 1);
        LOGR("coop: get me out of here: advance to unit %u, the partner's way", current + 1);
        return;
    }
    // the partner's unit is this copy's: its next one too if the partner has it active
    if (next_active && waiting <= int(current)) {
        uint32_t s = SlotOfUnit(world, current + 1);
        if (!s || R32(s) != kSlotLoaded) return;
        if (!G<uint8_t>(s + kSlotActive2)) {
            Activate(ctx, world, current);
            return;
        }
    }
    // this copy's advance waiting past the partner's place: the player goes back before it
    if (g_advance.trigger && g_advance.unit > target) CancelAdvance(ctx, "this player put back by the partner");
    g_rescue_on = false;
    coop::PlaceByPartner(uint8_t(target));
}

}  // namespace

void AskRescue() {
    if (!g_net) return;
    if (g_stage < 6) {
        coop::PlaceByPartner(0);
        return;
    }
    g_rescue_on = true;
    g_rescue_at = NowMs();
    g_rescue_behind = 0;
}

void Stop(X86Context& ctx) {
    Resume(ctx);
    g_advance = {};
    g_rescue_on = false;
    g_advancing = false;
    g_partner_test = false;
    Reset();
    SetMode(false, 0);
}

// Each frame, the world stopped or not: the advance waiting for the partner goes once the partner has gone through
// its trigger too (or left the level); a rescue's steps; the partner's flow events a stopped world would not take
void FlowTick(X86Context& ctx) {
    uint32_t world = R32(kWorld);
    int8_t level = world ? G<int8_t>(world + kWorldLevel) : int8_t(-1);
    auto this_play = [&](const Event& e) { return e.level == level && (e.ordinal == 0xFFFF || uint8_t(e.ordinal) == g_load); };
    // the partner's retry of the failed mission: this copy's too, once on its own failed page
    for (auto it = g_waiting.begin(); it != g_waiting.end();) {
        const Event& e = it->e;
        if (e.type != kFlow || (e.part != kFlowContinue && e.part != kFlowRestart)) {
            ++it;
            continue;
        }
        uint32_t game = R32(kGame);
        bool failed = game && R32(game + kGameState) == game + kInLevel && R32(game + kInLevel + 0x08) == game + 0x2103C;
        if (this_play(e) && failed) {
            static constexpr hle::Map kNone = hle::Make({});
            LOGR("coop: the partner's %s here too", e.part == kFlowContinue ? "continue" : "restart");
            g_retrying = true;
            hle::Call(ctx, e.part == kFlowContinue ? kContinueMission : kRestartMission, kNone, {});
            g_retrying = false;
            it = g_waiting.erase(it);
            continue;
        }
        if (!this_play(e) || ++it->frames > kRetryWaitMs * 60 / 1000) {
            it = g_waiting.erase(it);
            continue;
        }
        ++it;
    }
    // the partner's copy advanced: this copy's waiting advance to that unit goes, a later one of ours need not wait
    bool outcome = false;  // the partner's mission failed or level complete is waiting
    for (auto it = g_waiting.begin(); it != g_waiting.end();) {
        const Event& e = it->e;
        if (e.type == kFlow && (e.part == 1 || e.part == 2) && this_play(e)) outcome = true;
        if (e.type != kFlow || e.part != kFlowAdvanced) {
            ++it;
            continue;
        }
        if (this_play(e) && e.name_lo > g_partner_unit) {
            g_partner_unit = e.name_lo;
            LOGR("coop: the partner's copy advanced to unit %u", g_partner_unit);
        }
        it = g_waiting.erase(it);
    }
    RescueTick(ctx);
    // this copy's next unit activated as the partner's is: its enemies made there too
    int next_active = 0;
    int unit = coop::PartnerUnit(&next_active, nullptr);
    if (world && g_stage >= 6 && unit > 0 && next_active && !g_stalled && !R32(world + kWorldStream) &&
        R32(world + kWorldUnit) == uint32_t(unit)) {
        uint32_t s = SlotOfUnit(world, uint32_t(unit) + 1);
        if (s && R32(s) == kSlotLoaded && !G<uint8_t>(s + kSlotActive2)) {
            Activate(ctx, world, uint32_t(unit));
            LOGR("coop: unit %d activated, as the partner's", unit + 1);
        }
    }
    if (!g_advance.trigger) return;
    uint32_t t = g_advance.trigger;
    if (!world || R32(t) != kTriggerVtable || Name(R32(t + 0x08), R32(t + 0x0C)) != g_advance.id) {
        g_advance = {};
        Resume(ctx);
        return;
    }
    // the partner failed or completed: its outcome is this copy's too, the world goes on to take it (Tick)
    if (outcome) {
        CancelAdvance(ctx, "the partner's mission ended");
        return;
    }
    uint64_t now = NowMs();
    if (coop::PartnerInLevel()) g_advance.partner_seen = now;
    float feet[3];
    if (PartnerReached() < g_advance.unit && coop::PartnerAt(feet)) {
        // the partner's way since the last frame through the trigger, as 0x1C9270 sweeps the player's
        static uint32_t pos = 0, saved = 0;
        if (!pos) pos = mem::AllocSystem(16, 16), saved = mem::AllocSystem(16, 16);
        const float* from = g_advance.have_last ? g_advance.last : feet;
        float d[3] = {feet[0] - from[0], feet[1] - from[1], feet[2] - from[2]};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 9.0f) from = feet;  // a jump, not a walk: where it lands
        uint32_t last = world + kWorldLastPos;
        std::memcpy(HostPtr(saved), HostPtr(last), 16);
        float at[4] = {feet[0], feet[1], feet[2], 1}, start[4] = {from[0], from[1], from[2], 1};
        std::memcpy(HostPtr(pos), at, 16);
        std::memcpy(HostPtr(last), start, 16);
        static constexpr hle::Map kInsideMap = hle::Make({hle::eax, hle::ebx});
        bool crossed = hle::Call(ctx, kInside, kInsideMap, {t, pos}) & 0xFF;
        std::memcpy(HostPtr(last), HostPtr(saved), 16);
        std::memcpy(g_advance.last, feet, sizeof feet);
        g_advance.have_last = true;
        if (crossed) g_partner_crossed_unit = std::max(g_partner_crossed_unit, g_advance.unit);
    }
    if (R32(world + kWorldStream)) return;  // never over a streaming step in progress
    if (PartnerReached() >= g_advance.unit) ApplyAdvance(ctx, "the partner came");
    else if (now - g_advance.partner_seen > kAloneMs) ApplyAdvance(ctx, "the partner is not in the level");
}

void SendMelee(uint64_t name, uint32_t dir) {
    if (!g_net || g_stage < 4 || g_applying || !name || !dir) return;
    Event e{};
    e.type = kMelee;
    e.name_lo = uint32_t(name), e.name_hi = uint32_t(name >> 32);
    std::memcpy(e.dir, HostPtr(dir), 16);
    e.dir[3] = 0;
    Post(e);
    PROBE("melee- %016llX sent", (unsigned long long)name);
}

const Counters& Count() { return g_count; }

// The partner's walk through this copy's triggers. A one shot trigger of type 11 (vtable 0x28EF88) that Fires
// (action 0) is tested again at the partner's body, as the dispatcher 0x1E7FF0 tests it at the player's: its test
// 0x1C9270 sweeps from the player's last position, world+0x1C0, which stands in for the partner's meanwhile.
// Not tested: the unit triggers (actions 1, 2: the streaming follows this copy's player), the enter and exit ones
// (modes 1, 2: their inside flag is one player's), the visibility zones (mode 4) and type 12's enter and exit.
namespace {

// a trigger whose outputs set the scene's look for where it stands (type 32 0x1C22E0 writes world+0x5C98, type 31
// 0x1C2370 world+0x5C90): the partner's place is not this copy's player's, it waits for the player
bool SetsTheScene(uint32_t t) {
    uint32_t n = G<uint8_t>(t + 0x14), outputs = R32(t + 0x10);
    for (uint32_t i = 0; outputs && i < n; i++) {
        uint32_t o = R32(outputs + i * 4);
        if (o && (R32(o) == 0x28F0A4 || R32(o) == 0x28F0E0)) return true;
    }
    return false;
}

// the one shot triggers of this copy's current unit tested on the way from one of the partner's points to the next:
// the manager's lists are the two units' (a unit slot's +0x48), the other one's outputs would act on this unit
void TestPartner(X86Context& ctx, uint32_t mgr, const float from[3], const float to[3]) {
    static uint32_t pos = 0, saved = 0;
    if (!pos) pos = mem::AllocSystem(16, 16), saved = mem::AllocSystem(16, 16);
    uint32_t world = R32(kWorld);
    uint32_t last = world + kWorldLastPos;
    std::memcpy(HostPtr(saved), HostPtr(last), 16);
    float at[4] = {to[0], to[1], to[2], G<float>(Player() + kPlayerFeet + 12)};
    float start[4] = {from[0], from[1], from[2], G<float>(saved + 12)};
    std::memcpy(HostPtr(pos), at, 16);
    std::memcpy(HostPtr(last), start, 16);
    static constexpr hle::Map kTestMap = hle::Make({hle::ecx, hle::stack, hle::stack});
    static constexpr hle::Map kInsideMap = hle::Make({hle::eax, hle::ebx});
    static constexpr hle::Map kUnitMap = hle::Make({hle::eax});
    uint32_t unit = R32(world + kWorldUnit);
    for (uint32_t list = 0; list < 2; list++) {
        uint32_t n = G<uint16_t>(mgr + list * 2);
        uint32_t triggers = R32(mgr + kTriggerLists + list * 4);
        bool current = false;
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t s = world + kUnitSlots + k * kUnitSlotSize;
            if (triggers && R32(s + kSlotTriggers) == triggers && R32(s + kSlotUnit) == unit) current = true;
        }
        for (uint32_t i = 0; i < n; i++) {
            uint32_t t = R32(triggers + i * 4);
            if (!t || R32(t) != kTriggerVtable || R32(t + kTriggerMode) != 0) continue;
            uint32_t action = R32(t + kTriggerAction);
            if (action == 2) {  // a unit advance, of either unit: the partner's way through it noted
                if (g_stage >= 6 && (hle::Call(ctx, kInside, kInsideMap, {t, pos}) & 0xFF)) {
                    uint32_t to = hle::Call(ctx, kTriggerUnit, kUnitMap, {t});
                    if (to > g_partner_crossed_unit) {
                        g_partner_crossed_unit = to;
                        LOGR("coop: the partner went through the advance to unit %u", to);
                    }
                }
                continue;
            }
            // the activations (action 1) follow the partner's state (FlowTick)
            if (!current || !G<uint8_t>(t + kTriggerArmed) || action != 0 || SetsTheScene(t)) continue;
            g_partner_test = true;
            uint32_t fired = hle::Call(ctx, R32(R32(t) + kTriggerTest), kTestMap, {t, pos, Player()}) & 0xFF;
            g_partner_test = false;
            if (!fired) continue;
            G<uint8_t>(t + kTriggerArmed) = 0;  // as the dispatcher disarms it (0x1E8055)
            g_count.triggers++;
            PROBE("trigger %08X%08X fired at the partner", R32(t + 0x0C), R32(t + 0x08));
        }
    }
    std::memcpy(HostPtr(last), HostPtr(saved), 16);
}

}  // namespace

void PartnerTriggers(X86Context& ctx, uint32_t mgr) {
    static float prev[3];
    static bool have_prev = false;
    static uint64_t after = 0;
    float feet[3];
    uint32_t world = R32(kWorld);
    if (!g_net || g_stage < 2 || !world || !mgr || !coop::PartnerFeet(feet)) {
        have_prev = false;
        return;
    }
    if (!have_prev) {
        after = coop::PartnerPathEnd();
        std::memcpy(prev, feet, sizeof prev);
        have_prev = true;
        TestPartner(ctx, mgr, feet, feet);
        return;
    }
    // each received step of the partner's since the last tick, a pause or a hitch of this copy's included;
    // a step longer than a walk (a teleport, a respawn, lost states) is only tested where it lands
    float points[64][3];
    bool gap;
    int n = coop::PartnerPath(after, points, 64, gap);
    if (gap && n) std::memcpy(prev, points[0], sizeof prev);
    for (int i = 0; i < n; i++) {
        float moved = 0;
        for (int j = 0; j < 3; j++) moved += (points[i][j] - prev[j]) * (points[i][j] - prev[j]);
        TestPartner(ctx, mgr, moved > kWalkMax * kWalkMax ? points[i] : prev, points[i]);
        std::memcpy(prev, points[i], sizeof prev);
    }
}

}  // namespace coop::world

using namespace coop::world;

// A script's objective action, thiscall; one actor argument, ret 4.
X86_EXTERN_FUNC(__imp__sub_001C2290);
X86_FUNC(sub_001C2290) {
    if (ObjectiveAction(ctx.ecx)) {
        ctx.esp += 8;
        return;
    }
    __imp__sub_001C2290(ctx, base);
}

// A collected pickup fires its linked scripts on its update, after removal.
// Remember its stable map ID while those actions run so the peer can remove it
// as well; ammunition and health without an objective stay local.
X86_EXTERN_FUNC(__imp__sub_001E4580);
X86_FUNC(sub_001E4580) {
    uint64_t previous = g_objective_pickup;
    uint32_t p = ctx.ecx;
    // Its base update performs collision tests and may mark it collected during
    // this very call, so capture the ID even before +0x154 has been set.
    if (G<uint8_t>(p + 0x13B))
        g_objective_pickup = Name(R32(p + 8), R32(p + 12));
    __imp__sub_001E4580(ctx, base);
    g_objective_pickup = previous;
}

// The one place AI characters are made (its one call 0x1AF967): eax the pool; the controller, the descriptor;
// ret 8, eax the character or 0
X86_EXTERN_FUNC(__imp__sub_001DF440);
X86_FUNC(sub_001DF440) {
    __imp__sub_001DF440(ctx, base);
    uint32_t ch = ctx.eax;
    int s = ch ? SlotOf(ch) : -1;
    if (s < 0) return;
    Npc& n = g_npcs[s];
    n = {};
    n.ch = ch;
    n.name = Name(R32(ch + kCharName), R32(ch + kCharName + 4));
    if (!n.name) return;
    n.ordinal = ++g_spawns[n.name];
    n.alive = true;
    const float* pos = &G<float>(ch + kCharPos);
    PROBE("npc+ s%d %016llX #%u type %u team %u unit %d at %.1f %.1f %.1f", s, (unsigned long long)n.name, n.ordinal,
          R32(ch + kCharType), R32(ch + kCharTeam), int32_t(R32(ch + kCharUnit)), double(pos[0]), double(pos[1]),
          double(pos[2]));
}

// The death, eax the noise class; the manager, the victim, the part hit, &dir, the damage, who did it (ret 0x18):
// from TakeDamage 0x1E79AA and the melee 0x1DFBF0. Its hit goes with the event if this copy sends it.
X86_EXTERN_FUNC(__imp__sub_00205C60);
X86_FUNC(sub_00205C60) {
    uint32_t ch = R32(ctx.esp + 8);
    int s = SlotOf(ch);
    if (s >= 0 && g_npcs[s].ch == ch && g_npcs[s].alive) {
        Npc& n = g_npcs[s];
        n.hit = true;
        n.noise = uint8_t(ctx.eax);
        n.part = uint8_t(R32(ctx.esp + 0xC));
        std::memcpy(n.dir, HostPtr(R32(ctx.esp + 0x10)), 16);
        n.dmg = G<float>(ctx.esp + 0x14);
        n.partner = coop::shots::SteppingPartner();
    }
    __imp__sub_00205C60(ctx, base);
}

// To a ragdoll, eax the character, ecx the pool, plain ret; its one call 0x205665. Its name is still there
X86_EXTERN_FUNC(__imp__sub_001DA240);
X86_FUNC(sub_001DA240) {
    Died(ctx.eax, "ragdoll");
    __imp__sub_001DA240(ctx, base);
}

// A character let go, stdcall (pool, character), ret 8: dead (+0x384 2), a corpse (1) or gone, unloaded (0)
X86_EXTERN_FUNC(__imp__sub_001DA1A0);
X86_FUNC(sub_001DA1A0) {
    uint32_t ch = R32(ctx.esp + 8);
    uint32_t state = ch ? R32(ch + kCharState) : 1;
    if (state == 2) Died(ch, "dead");
    else if (state == 0) {
        int s = SlotOf(ch);
        if (s >= 0 && g_npcs[s].ch == ch && g_npcs[s].alive) {
            g_npcs[s].alive = false;
            PROBE("npc~ s%d %016llX #%u let go (from %08X)", s, (unsigned long long)g_npcs[s].name, g_npcs[s].ordinal,
                  R32(ctx.esp));
        }
    }
    __imp__sub_001DA1A0(ctx, base);
}

// The logic tick, esi the manager; dt, &the player's body (ret 8); once a frame of play from 0x1F12A6, after the
// player, the characters and physics. The partner's walk follows this copy's own.
X86_EXTERN_FUNC(__imp__sub_001E7FF0);
X86_FUNC(sub_001E7FF0) {
    uint32_t mgr = ctx.esi;
    coop::control::BeforeTriggers();  // a test's "stand": the player in a trigger as the triggers are tested
    __imp__sub_001E7FF0(ctx, base);
    PartnerTriggers(ctx, mgr);
    coop::npcs::LogicTick(ctx);  // where the spawners' tick would make them: the host's enemies made here
}

// A kind 3 prop's state change, eax the prop; to destroyed, 0 (ret 8): from its damage 0x1E8C50 and its other hurts
X86_EXTERN_FUNC(__imp__sub_001E3DB0);
X86_FUNC(sub_001E3DB0) {
    uint32_t p = ctx.eax;
    __imp__sub_001E3DB0(ctx, base);
    SendProp(p, 3, 0xFF);
}

// A kind 4 prop's part state change, eax the prop; the part, to destroyed (ret 8)
X86_EXTERN_FUNC(__imp__sub_001E4B80);
X86_FUNC(sub_001E4B80) {
    uint32_t p = ctx.eax;
    uint8_t part = uint8_t(R32(ctx.esp + 4));
    __imp__sub_001E4B80(ctx, base);
    SendProp(p, 4, part);
}

// A rocket's launch (esi the rocket, eax &velocity, bl 1 for an AI's: its ray starts 5 m out, 0x1DDA79): the
// partner's rocket replayed here is a player's, its ray from the muzzle, its fx and flags +0x117/+0x118 the player's
X86_EXTERN_FUNC(__imp__sub_001DDA70);
X86_FUNC(sub_001DDA70) {
    if (g_rocket_replay) ctx.ebx &= ~0xFFu;
    __imp__sub_001DDA70(ctx, base);
}

// A grenade made, edi the maker; &matrix, &velocity, side (ret 0xC): the player's (side 3) go to the partner
X86_EXTERN_FUNC(__imp__sub_001DCE60);
X86_FUNC(sub_001DCE60) {
    PROBE("grenade made: side %u from %08X", R32(ctx.esp + 0xC), R32(ctx.esp));
    bool mine = R32(ctx.esp + 0xC) == kPlayerSide && SendsProjectiles();
    uint32_t matrix = R32(ctx.esp + 4), vel = R32(ctx.esp + 8);
    if (mine) SendProjectile(0, kPlayerSide, matrix, vel, 0, 0);
    __imp__sub_001DCE60(ctx, base);
}

// A hand grenade thrown, edi the maker; &matrix, &velocity, side, a flag byte (ret 0x10): from 0x1E32A0, the throw
X86_EXTERN_FUNC(__imp__sub_001DD000);
X86_FUNC(sub_001DD000) {
    uint32_t side = R32(ctx.esp + 0xC);
    bool mine = side == kPlayerSide && SendsProjectiles();
    // an enemy's (side 0) on the host: its puppet on the joining copy throws none of its own
    bool enemy = side == 0 && g_stage >= 4 && coop::npcs::HostsEnemies() && SendsProjectiles();
    if (mine || enemy) SendProjectile(2, side, R32(ctx.esp + 4), R32(ctx.esp + 8), R32(ctx.esp + 0x10) & 0xFF, 0);
    __imp__sub_001DD000(ctx, base);
}

// A rocket made, ecx the maker, edi the side; &matrix, &velocity, its trail's fx (ret 0x10)
X86_EXTERN_FUNC(__imp__sub_001E35F0);
X86_FUNC(sub_001E35F0) {
    PROBE("rocket made: side %u from %08X", ctx.edi, R32(ctx.esp));
    bool mine = ctx.edi == kPlayerSide && SendsProjectiles();
    if (mine) SendProjectile(1, kPlayerSide, R32(ctx.esp + 4), R32(ctx.esp + 8), R32(ctx.esp + 0xC), R32(ctx.esp + 0x10));
    __imp__sub_001E35F0(ctx, base);
}

// A unit activated (stdcall, its slot; ret 4): its props are in the slot's arrays now
X86_EXTERN_FUNC(__imp__sub_001EB440);
X86_FUNC(sub_001EB440) {
    __imp__sub_001EB440(ctx, base);
    g_props_retry = true;
}

// The cull when the 16 live characters are all taken (0x1E6860) and its kin: each copy culls its own, not sent
X86_EXTERN_FUNC(__imp__sub_002056A0);
X86_FUNC(sub_002056A0) {
    g_culling++;
    __imp__sub_002056A0(ctx, base);
    g_culling--;
}

// A unit advance asked by a trigger this copy's player went through (thiscall ecx the world; the unit; ret 4; esi the
// trigger, from its test 0x1C9476; type 67's 0x1C8BE1 goes as it is): with the partner in the level and not through
// it yet, it waits for the partner. The unit behind would go, the partner still in it
X86_EXTERN_FUNC(__imp__sub_001B73B0);
X86_FUNC(sub_001B73B0) {
    uint32_t t = ctx.esi;
    uint32_t world = ctx.ecx, unit = R32(ctx.esp + 4);
    // an advance it makes: not past the last unit, the next one (0x1B73C0)
    uint32_t info = world ? R32(world + kWorldLevelInfo) : 0;
    uint32_t current = world ? R32(world + kWorldUnit) : 0;
    bool asks = info && current != G<uint8_t>(info + 0x11) && current + 1 == unit;
    if (asks && !g_advancing && g_net && g_stage >= 6 && t && R32(t) == kTriggerVtable && coop::PartnerInLevel() &&
        PartnerReached() < unit) {
        if (!g_advance.trigger) LOGR("coop: the advance to unit %u waits for the partner", unit);
        g_advance = {};
        g_advance.trigger = t, g_advance.id = Name(R32(t + 0x08), R32(t + 0x0C)), g_advance.unit = unit;
        g_advance.partner_seen = NowMs();
        // the way this copy's player goes across it, from a frame ago to now
        float half;
        float now = AcrossTrigger(t, &G<float>(Player() + kPlayerFeet), half);
        float before = AcrossTrigger(t, &G<float>(world + kWorldLastPos), half);
        g_advance.dir = now - before > 0.001f ? 1.0f : now - before < -0.001f ? -1.0f : 0.0f;
        return Return(ctx, ctx.eax, 1);
    }
    if (asks) SendFlow(kFlowAdvanced, unit, 0);  // the partner's copy waiting for it goes too
    __imp__sub_001B73B0(ctx, base);
}

// The next unit's activation asked (esi the world, ecx the unit; al): this copy's player at it while the advance
// before it waits for the partner stops the world, as for a unit still loading (0x1C764C), till the partner comes
X86_EXTERN_FUNC(__imp__sub_001C7620);
X86_FUNC(sub_001C7620) {
    uint32_t world = ctx.esi, unit = ctx.ecx;
    if (g_advance.trigger && !g_partner_test && world && unit == g_advance.unit && R32(world + kWorldUnit) + 1 == unit) {
        uint32_t game = R32(kGame);
        if (game && !g_stalled && R32(game + kGameStall) != 1) {
            W32(game + kGameStall, 1);
            g_stalled = true;
            LOGR("coop: at unit %u's door: waiting for the partner", unit);
        }
        return Return(ctx, ctx.eax & ~0xFFu, 0);
    }
    __imp__sub_001C7620(ctx, base);
}

// The in-level state's requests (thiscall ecx it): the mission failed (vf5), the level complete (vf6): the partner's
// too. Its vf7 (0x1F4590) is a scripted lock of the player (a cinematic, on and off): each copy's script makes its own
X86_EXTERN_FUNC(__imp__sub_001F45C0);
X86_FUNC(sub_001F45C0) {
    uint32_t game = R32(kGame);
    if (game && ctx.ecx == game + kInLevel && !(R32(ctx.ecx + 0x0C) & 1) && R32(ctx.ecx + 0x08) != game + 0x2103C)
        SendFlow(1, 0, 0);  // once: its death and 0x1C6410 may both ask
    __imp__sub_001F45C0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_001F45B0);
X86_FUNC(sub_001F45B0) {
    uint32_t game = R32(kGame);
    if (game && ctx.ecx == game + kInLevel && !(R32(ctx.ecx + 0x0C) & 2) && R32(ctx.ecx + 0x08) != game + 0x2104C)
        SendFlow(2, 0, 0);
    __imp__sub_001F45B0(ctx, base);
}


// The failed mission's choices (front end callbacks, plain): retried together, the partner's copy on its own failed
// page does the same
X86_EXTERN_FUNC(__imp__sub_001240D0);
X86_FUNC(sub_001240D0) {
    SendFlow(kFlowContinue, 0, 0);
    __imp__sub_001240D0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_00124190);
X86_FUNC(sub_00124190) {
    SendFlow(kFlowRestart, 0, 0);
    __imp__sub_00124190(ctx, base);
}
