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
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "game/coop.h"
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

constexpr uint32_t kDie = 0x205C60;  // eax noise class; mgr, victim, part, &dir, dmg, instigator; ret 0x18
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

enum Type : uint8_t { kDeath = 1 };

#pragma pack(push, 1)
struct Event {
    uint32_t seq;
    uint32_t ack;  // the last of the receiver's events its sender had taken when it made this one
    uint8_t type;
    int8_t level;
    uint8_t load;  // the sender's level loads: events of a load the partner has left behind are not played
    uint8_t part;
    uint32_t name_lo, name_hi;
    uint16_t ordinal;  // the sender's count of the spawner's characters, this one included: for the log
    uint8_t hit;       // 1: part, dir, dmg and noise are the killing hit's
    uint8_t noise;     // its noise class
    float dir[4];
    float dmg;
};
#pragma pack(pop)

struct Npc {
    uint32_t ch = 0;
    uint64_t name = 0;
    uint16_t ordinal = 0;
    bool alive = false;
    bool remote = false;  // dying by the partner's word: not sent back
    bool hit = false;
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

void Send(const Npc& n) {
    Event e{};
    e.seq = ++g_seq;
    e.ack = g_taken;
    g_own[n.name] = e.seq;
    e.type = kDeath;
    uint32_t world = R32(kWorld);
    e.level = world ? G<int8_t>(world + kWorldLevel) : -1;
    e.load = g_load;
    e.name_lo = uint32_t(n.name);
    e.name_hi = uint32_t(n.name >> 32);
    e.ordinal = n.ordinal;
    if (n.hit) {
        e.hit = 1;
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
    g_pending.push_back({NowMs(), e});
    if (g_pending.size() > kPendingMax) {
        g_pending.pop_front();
        g_count.lost++;
    }
    g_count.sent++;
}

// a death on this copy: the partner's enemy too, unless the partner said so first; a cull is this copy's own
void Died(uint32_t ch, const char* how) {
    int s = SlotOf(ch);
    if (s < 0) return;
    Npc& n = g_npcs[s];
    if (n.ch != ch || !n.alive) return;
    n.alive = false;
    bool send = g_net && g_stage >= 1 && !g_applying && !g_culling && !n.remote && n.name;
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

Result Kill(X86Context& ctx, Npc& n, const Event& e) {
    uint32_t ch = n.ch;
    if (R32(ch + kCharState) != 0) return Result::Done;  // dying here already
    uint32_t control = R32(ch + kCharControl);
    if (!control || R32(control + kControlActive) != 1) return Result::Later;
    uint32_t noise = NoiseClass(e.hit ? e.noise : 0);
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
    return Result::Wait;  // not made here yet, or removed and to come again
}

// the partner loaded that level again: its enemies there live again, its deaths of before are void
void DropOld(uint8_t load, int8_t level) {
    std::erase_if(g_waiting, [&](const Waiting& w) {
        bool old = w.e.level == level && w.e.load != load;
        if (old) g_count.lost++;
        return old;
    });
}

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
    DropOld(load, level);
}

int Outgoing(uint8_t* out, int cap, int& bytes, uint64_t now) {
    bytes = 0;
    while (!g_pending.empty() && now - g_pending.front().at > kOldMs) {
        g_pending.pop_front();
        g_count.lost++;
    }
    int n = 0;
    for (const Stamped& s : g_pending) {
        if (bytes + int(sizeof(Event)) > cap || n == 255) break;
        std::memcpy(out + bytes, &s.e, sizeof(Event));
        bytes += int(sizeof(Event));
        n++;
    }
    return n;
}

void Hold(uint64_t now) {
    // the partner cannot be heard: the events wait for it, their age does not run
    for (Stamped& s : g_pending) s.at = now;
}

void Acked(uint32_t seq) {
    while (!g_pending.empty() && int32_t(g_pending.front().e.seq - seq) <= 0) g_pending.pop_front();
}

void Incoming(const uint8_t* p, int bytes, int count, uint64_t) {
    for (int i = 0; i < count && (i + 1) * int(sizeof(Event)) <= bytes; i++) {
        Event e;
        std::memcpy(&e, p + i * sizeof(Event), sizeof(Event));
        // every datagram starts at the oldest event not acknowledged: newer is the next one
        if (int32_t(e.seq - g_taken) <= 0) continue;
        g_taken = e.seq;
        g_count.taken++;
        PartnerLoad(e.load, e.level);
        bool reloaded = e.level == g_partner_level && e.load != uint8_t(g_partner_load);
        if (e.type != kDeath || g_stage < 1 || reloaded) {
            g_count.lost++;  // from before the partner loaded that level again
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
    g_seq = 0;
    g_pending.clear();
    g_own.clear();
}

void Tick(X86Context& ctx) {
    if (g_stage < 1 || g_waiting.empty()) return;
    uint32_t world = R32(kWorld);
    int8_t level = world ? G<int8_t>(world + kWorldLevel) : int8_t(-1);
    for (size_t i = 0; i < g_waiting.size();) {
        Waiting& w = g_waiting[i];
        if (w.e.level != level) {  // the partner's level, not ours yet: it waits
            i++;
            continue;
        }
        Result r = Apply(ctx, w.e);
        if (r == Result::Done || r == Result::Stale || (r == Result::Later && ++w.frames > kRetryFrames)) {
            if (r == Result::Stale) g_count.stale++;
            if (r == Result::Later) g_count.lost++;
            g_waiting.erase(g_waiting.begin() + ptrdiff_t(i));
            continue;
        }
        i++;
    }
    g_count.waiting = uint32_t(g_waiting.size());
}

// From the loader's bullet reset (0x1EDE4C), the new level already in world+0x5AEC (0x1EDD64)
void LevelReset() {
    for (Npc& n : g_npcs) n = {};
    g_spawns.clear();
    g_own.clear();
    uint32_t world = R32(kWorld);
    int8_t level = world ? G<int8_t>(world + kWorldLevel) : int8_t(-1);
    // ours of this level, loaded again, are void on the partner; of another level they still stand
    std::erase_if(g_pending, [&](const Stamped& s) { return s.e.level == level; });
    // the partner's of any other level would only meet a later play of it
    std::erase_if(g_waiting, [&](const Waiting& w) { return w.e.level != level; });
    g_level = level;
    g_load++;
}

int8_t LoadLevel() { return g_level; }

const Counters& Count() { return g_count; }

// The partner's walk through this copy's triggers. A one shot trigger of type 11 (vtable 0x28EF88) that Fires
// (action 0) is tested again at the partner's body, as the dispatcher 0x1E7FF0 tests it at the player's: its test
// 0x1C9270 sweeps from the player's last position, world+0x1C0, which stands in for the partner's meanwhile.
// Not tested: the unit triggers (actions 1, 2: the streaming follows this copy's player), the enter and exit ones
// (modes 1, 2: their inside flag is one player's), the visibility zones (mode 4) and type 12's enter and exit.
void PartnerTriggers(X86Context& ctx, uint32_t mgr) {
    static float prev[3];
    static bool have_prev = false;
    float feet[3];
    uint32_t world = R32(kWorld);
    if (!g_net || g_stage < 2 || !world || !mgr || !coop::PartnerFeet(feet)) {
        have_prev = false;
        return;
    }
    if (!have_prev) std::memcpy(prev, feet, sizeof prev);
    static uint32_t pos = 0, saved = 0;
    if (!pos) {
        pos = mem::AllocSystem(16, 16);
        saved = mem::AllocSystem(16, 16);
    }
    uint32_t last = world + kWorldLastPos;
    std::memcpy(HostPtr(saved), HostPtr(last), 16);
    float w = G<float>(Player() + kPlayerFeet + 12);
    float at[4] = {feet[0], feet[1], feet[2], w};
    float from[4] = {prev[0], prev[1], prev[2], G<float>(saved + 12)};
    std::memcpy(HostPtr(pos), at, 16);
    std::memcpy(HostPtr(last), from, 16);
    static constexpr hle::Map kTestMap = hle::Make({hle::ecx, hle::stack, hle::stack});
    for (uint32_t list = 0; list < 2; list++) {
        uint32_t n = G<uint16_t>(mgr + list * 2);
        uint32_t triggers = R32(mgr + kTriggerLists + list * 4);
        for (uint32_t i = 0; triggers && i < n; i++) {
            uint32_t t = R32(triggers + i * 4);
            if (!t || R32(t) != kTriggerVtable || !G<uint8_t>(t + kTriggerArmed)) continue;
            if (R32(t + kTriggerMode) != 0 || R32(t + kTriggerAction) != 0) continue;
            uint32_t fired = hle::Call(ctx, R32(R32(t) + kTriggerTest), kTestMap, {t, pos, Player()}) & 0xFF;
            if (!fired) continue;
            G<uint8_t>(t + kTriggerArmed) = 0;  // as the dispatcher disarms it (0x1E8055)
            g_count.triggers++;
            PROBE("trigger %08X%08X fired at the partner", R32(t + 0x0C), R32(t + 0x08));
        }
    }
    std::memcpy(HostPtr(last), HostPtr(saved), 16);
    std::memcpy(prev, feet, sizeof prev);
    have_prev = true;
}

}  // namespace coop::world

using namespace coop::world;

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
            PROBE("npc~ s%d %016llX #%u let go", s, (unsigned long long)g_npcs[s].name, g_npcs[s].ordinal);
        }
    }
    __imp__sub_001DA1A0(ctx, base);
}

// The logic tick, esi the manager; dt, &the player's body (ret 8); once a frame of play from 0x1F12A6, after the
// player, the characters and physics. The partner's walk follows this copy's own.
X86_EXTERN_FUNC(__imp__sub_001E7FF0);
X86_FUNC(sub_001E7FF0) {
    uint32_t mgr = ctx.esi;
    __imp__sub_001E7FF0(ctx, base);
    PartnerTriggers(ctx, mgr);
}

// The cull when the 16 live characters are all taken (0x1E6860) and its kin: each copy culls its own, not sent
X86_EXTERN_FUNC(__imp__sub_002056A0);
X86_FUNC(sub_002056A0) {
    g_culling++;
    __imp__sub_002056A0(ctx, base);
    g_culling--;
}
