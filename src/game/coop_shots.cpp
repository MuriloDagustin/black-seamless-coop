// Co-op shots. The sender takes each pellet where the game makes the player's bullets, the receiver fires it again
// with a weapon of the same type whose owner is its own player: the bullet then runs the player's bullet code, which
// never hits the player itself (only rays with flag 8 test it, 0x1CDECB) and leaves its ammo, recoil and HUD alone.
// The partner's bullets live in a pool of our own, stepped beside the game's, so they never take the player's slots.
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>

#include "core/log.h"
#include "game/coop_shots.h"
#include "game/coop_world.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using layout::kWorld;
using layout::kWorldPlayer;

namespace coop::shots {

namespace {

// the weapons' table and the bullets: [kArmoury], its +0 the type table, +0x6E0 the 16 bullets
constexpr uint32_t kArmoury = 0x2D12D0;
constexpr uint32_t kTypeCount = 1;  // byte, in the type table
constexpr uint32_t kTypes = 4;      // the entries, 32 bytes each
constexpr uint32_t kTypeSize = 32;
constexpr uint32_t kTypeData = 8;
constexpr uint32_t kBullets = 0x6E0;
constexpr uint32_t kBulletSize = 0x90;
constexpr int kBulletCount = 16;
constexpr uint32_t kBulletLive = 0x88;      // byte
constexpr uint32_t kBulletDistance = 0x74;  // float: to the hit, or the range
constexpr uint32_t kPoolSize = kBullets + kBulletCount * kBulletSize;

// weapon, its gun ('shot') and their data
constexpr uint32_t kWeaponType = 0;  // int8
constexpr uint32_t kWeaponSpread = 0xE4;
constexpr uint32_t kWeaponEntry = 0xE8;
constexpr uint32_t kWeaponData = 0xEC;
constexpr uint32_t kWeaponOwner = 0xF0;
constexpr uint32_t kWeaponGun = 0xF4;
constexpr uint32_t kWeaponAmmo = 0xFC;
constexpr uint32_t kWeaponScoped = 0x107;
constexpr uint32_t kWeaponSilenced = 0x108;
constexpr uint32_t kWeaponSize = 0x110;
constexpr uint32_t kGunFireType = 0;
constexpr uint32_t kGunWeapon = 4;
constexpr uint32_t kGunData = 8;
constexpr uint32_t kGunParams = 0xC;  // the data itself for the player, its +0x30 for the AI
constexpr uint32_t kGunBurst = 0x14;
constexpr uint32_t kGunClip = 0x18;  // int16
constexpr uint32_t kGunSize = 0x30;
constexpr uint32_t kDataClip = 0x60;
constexpr uint32_t kDataFireType = 0x90;
constexpr uint32_t kDataBurst = 0x98;
constexpr uint32_t kPlayerWeapon = 0x29C;
constexpr uint32_t kPlayerAmmo = 0x278;
constexpr uint32_t kWorldDt = 0x1C;       // float: the step 0x1E28C0 moves the bullets by
constexpr uint32_t kWorldLevel = 0x5AEC;  // int8

constexpr uint32_t kSpawn = 0x1DB7B0;   // ecx origin, edx the pool's base; shot, dir, range, speed, flags; al spawned
constexpr uint32_t kStep = 0x1E26E0;    // eax a bullet; dt
constexpr uint32_t kTracer = 0x168E20;  // edx the tracers, bl 1 always drawn; start, end
constexpr uint32_t kTracers = 0x2D1990;
constexpr uint32_t kTracerList = 0x4E2C0;

constexpr uint64_t kKeepMs = 500;   // an event not acknowledged by then is given up
constexpr uint64_t kEchoMs = 3000;  // as coop.cpp's echo of the player
constexpr uint64_t kLateMs = 250;   // queued longer, play stopped meanwhile: not fired
constexpr size_t kQueue = 32;

bool g_net = false, g_echo = false;
bool g_replaying = false;  // our own spawn calls: not taken as the player's

// sender
bool g_open = false;
Event g_event{};
uint32_t g_seq = 0;
struct Stamped {
    uint64_t at;
    Event event;
};
std::deque<Stamped> g_pending;  // to the partner until acknowledged, or back to this copy for the echo

// receiver
uint32_t g_taken = 0;
std::deque<Stamped> g_queue;  // to fire on the next bullet step
uint32_t g_pool = 0;          // the partner's bullets: 0x1DB7B0 and 0x1E26E0 read only its +0x6E0
Counters g_count;

// fire with: a weapon and its gun per type, this process's own
struct Ghost {
    uint32_t weapon = 0, gun = 0;
};
Ghost g_ghosts[256];
uint32_t g_buffers = 0;  // origin, dir, end: 16 aligned vectors

uint64_t NowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32_t Player() { return R32(kWorld) + kWorldPlayer; }

uint32_t TypeTable() {
    uint32_t armoury = R32(kArmoury);
    return armoury ? R32(armoury) : 0;
}

uint32_t Bullet(int i) { return g_pool + kBullets + uint32_t(i) * kBulletSize; }

// the type of a name hash, -1 when this game has none
int TypeOf(uint32_t lo, uint32_t hi) {
    uint32_t table = TypeTable();
    if (!table) return -1;
    uint32_t n = G<uint8_t>(table + kTypeCount);
    uint32_t types = R32(table + kTypes);
    for (uint32_t i = 0; i < n; i++)
        if (R32(types + i * kTypeSize) == lo && R32(types + i * kTypeSize + 4) == hi) return int(i);
    return -1;
}

// a weapon and gun as Weapon::Init 0x1C9FA0 and the gun init 0x1C2760 make them for the player, in memory of our own
const Ghost* GhostOf(int type) {
    Ghost& g = g_ghosts[type];
    if (g.weapon) return &g;
    uint32_t table = TypeTable();
    uint32_t entry = R32(table + kTypes) + uint32_t(type) * kTypeSize;
    uint32_t data = R32(entry + kTypeData);
    if (!data) return nullptr;
    uint32_t w = mem::AllocSystem(kWeaponSize);
    uint32_t s = mem::AllocSystem(kGunSize);
    std::memset(HostPtr(w), 0, kWeaponSize);
    std::memset(HostPtr(s), 0, kGunSize);
    G<int8_t>(w + kWeaponType) = int8_t(type);
    W32(w + kWeaponEntry, entry);
    W32(w + kWeaponData, data);
    G<float>(w + kWeaponSpread) = 1.0f;
    W32(w + kWeaponGun, s);
    W32(s + kGunFireType, R32(data + kDataFireType));
    W32(s + kGunWeapon, w);
    W32(s + kGunData, data);
    W32(s + kGunParams, data);
    W32(s + kGunBurst, R32(data + kDataBurst));
    W16(s + kGunClip, R16(data + kDataClip));
    g = {w, s};
    return &g;
}

// the owner is the player, whose world may have been made anew since
void Own(const Ghost& g) {
    uint32_t player = Player();
    W32(g.weapon + kWeaponOwner, player);
    W32(g.weapon + kWeaponAmmo, player + kPlayerAmmo);
}

bool Finite(const float* v, int n) {
    for (int i = 0; i < n; i++)
        if (!std::isfinite(v[i])) return false;
    return true;
}

bool Valid(const Event& e) {
    if (e.count < 1 || e.count > kMaxPellets) return false;
    if (!std::isfinite(e.range) || !(e.range > 0 && e.range <= 4000)) return false;
    if (!std::isfinite(e.speed) || !(e.speed > 0 && e.speed <= 2000)) return false;
    for (int i = 0; i < e.count; i++) {
        const Pellet& p = e.pellets[i];
        if (!Finite(p.origin, 4) || !Finite(p.dir, 4)) return false;
        float len = std::sqrt(p.dir[0] * p.dir[0] + p.dir[1] * p.dir[1] + p.dir[2] * p.dir[2]);
        if (std::fabs(len - 1) > 0.05f) return false;
    }
    return true;
}

void Queue(const Event& e, uint64_t now) {
    if (g_queue.size() >= kQueue) {
        g_count.dropped += g_queue.front().event.count;
        g_queue.pop_front();
    }
    g_queue.push_back({now, e});
}

void DropQueue() {
    for (const Stamped& s : g_queue) g_count.dropped += s.event.count;
    g_queue.clear();
}

// the partner's pellets of one event, from its origin, into our pool. A straight player bullet draws no tracer of its
// own; ours goes from the muzzle to the first wall the spawn found ahead (a character in between is hit later)
void Fire(X86Context& ctx, const Event& e) {
    int type = TypeOf(e.id_lo, e.id_hi);
    const Ghost* ghost = type >= 0 ? GhostOf(type) : nullptr;
    if (!ghost) {
        static int told;
        if (told++ < 4) LOGR("coop: no weapon type %08X:%08X here", e.id_hi, e.id_lo);
        g_count.dropped += e.count;
        return;
    }
    Own(*ghost);
    G<uint8_t>(ghost->weapon + kWeaponSilenced) = e.bits & 1;
    G<uint8_t>(ghost->weapon + kWeaponScoped) = (e.bits >> 1) & 1;
    uint32_t origin = g_buffers, dir = g_buffers + 16, end = g_buffers + 32;
    static constexpr hle::Map kSpawnMap =
        hle::Make({hle::ecx, hle::edx, hle::stack, hle::stack, hle::stack, hle::stack, hle::stack});
    static constexpr hle::Map kTracerMap = hle::Make({hle::edx, hle::ebx, hle::stack, hle::stack});
    for (int i = 0; i < e.count; i++) {
        uint32_t slot = 0;  // the first free bullet is the one the spawn takes
        for (int b = 0; b < kBulletCount && !slot; b++)
            if (!G<uint8_t>(Bullet(b) + kBulletLive)) slot = Bullet(b);
        if (!slot) {
            g_count.dropped += e.count - i;
            return;
        }
        const Pellet& p = e.pellets[i];
        std::memcpy(HostPtr(origin), p.origin, 16);
        std::memcpy(HostPtr(dir), p.dir, 16);
        g_replaying = true;
        uint32_t spawned = hle::Call(ctx, kSpawn, kSpawnMap,
                                     {origin, g_pool, ghost->gun, dir, std::bit_cast<uint32_t>(e.range),
                                      std::bit_cast<uint32_t>(e.speed), e.flags}) & 0xFF;
        g_replaying = false;
        if (!spawned) {
            g_count.dropped++;
            continue;
        }
        if (!g_count.fired++) LOGR("coop: first partner bullet fired, type %d", type);
        float reach = G<float>(slot + kBulletDistance);
        if (!std::isfinite(reach) || reach <= 0) continue;  // the muzzle on a wall: nothing to draw
        float to[4] = {p.origin[0] + p.dir[0] * reach, p.origin[1] + p.dir[1] * reach,
                       p.origin[2] + p.dir[2] * reach, p.origin[3]};
        std::memcpy(HostPtr(end), to, 16);
        hle::Call(ctx, kTracer, kTracerMap, {R32(kTracers) + kTracerList, 1, origin, end});
    }
}

// our pool, as 0x1E28C0 steps the game's
void Step(X86Context& ctx, float dt) {
    static constexpr hle::Map kStepMap = hle::Make({hle::eax, hle::stack});
    for (const Ghost& g : g_ghosts)
        if (g.weapon) Own(g);
    for (int b = 0; b < kBulletCount; b++)
        if (G<uint8_t>(Bullet(b) + kBulletLive)) hle::Call(ctx, kStep, kStepMap, {Bullet(b), std::bit_cast<uint32_t>(dt)});
}

}  // namespace

void SetMode(bool net, bool echo) {
    g_net = net;
    g_echo = echo && !net;
}

int Outgoing(uint8_t* out, int cap, int& bytes, uint64_t now) {
    while (!g_pending.empty() && now - g_pending.front().at > kKeepMs) g_pending.pop_front();
    int n = 0;
    bytes = 0;
    for (const Stamped& s : g_pending) {
        int size = Bytes(s.event);
        if (bytes + size > cap || n == 255) break;
        std::memcpy(out + bytes, &s.event, size);
        bytes += size;
        n++;
    }
    return n;
}

void Acked(uint32_t seq) {
    while (!g_pending.empty() && int32_t(g_pending.front().event.seq - seq) <= 0) g_pending.pop_front();
}

void Incoming(const uint8_t* p, int bytes, int count, int8_t level, uint64_t now) {
    for (int i = 0, at = 0; i < count; i++) {
        if (at + kEventHead > bytes) return;
        Event e{};
        std::memcpy(&e, p + at, kEventHead);
        if (e.count < 1 || e.count > kMaxPellets) return;
        int size = Bytes(e);
        if (at + size > bytes) return;
        std::memcpy(&e, p + at, size);
        at += size;
        if (int32_t(e.seq - g_taken) <= 0) continue;  // taken before, sent again
        g_taken = e.seq;
        g_count.taken++;
        if (!Valid(e) || e.level != level) {
            g_count.dropped += e.count;
            continue;
        }
        Queue(e, now);
    }
}

uint32_t Taken() { return g_taken; }

void Reset() {
    g_taken = 0;
    DropQueue();
}

void Echo(uint64_t now) {
    if (!g_echo) return;
    while (!g_pending.empty() && now - g_pending.front().at >= kEchoMs) {
        const Event& e = g_pending.front().event;
        g_count.taken++;
        if (Valid(e)) Queue(e, now);
        g_pending.pop_front();
    }
}

bool GhostWeapon(uint32_t weapon) {
    if (!weapon) return false;
    for (const Ghost& g : g_ghosts)
        if (g.weapon == weapon) return true;
    return false;
}

const Counters& Count() { return g_count; }

}  // namespace coop::shots

using namespace coop::shots;

// One pull of the trigger, stdcall, [esp+4] the gun; called from the weapon's state machine 0x1EEFD0 only.
// The player's pellets inside it go through 0x1DB7B0, which makes them one event.
X86_EXTERN_FUNC(__imp__sub_001DBD50);
X86_FUNC(sub_001DBD50) {
    uint32_t gun = R32(ctx.esp + 4);
    uint32_t weapon = gun ? R32(gun + kGunWeapon) : 0;
    uint32_t world = R32(kWorld);
    bool mine = (g_net || g_echo) && !g_replaying && world && weapon && R32(weapon + kWeaponOwner) == Player();
    if (!mine) return __imp__sub_001DBD50(ctx, base);
    uint32_t entry = R32(weapon + kWeaponEntry);
    g_event = {};
    g_event.level = G<int8_t>(world + kWorldLevel);
    g_event.type = G<uint8_t>(weapon + kWeaponType);
    g_event.bits = uint8_t((G<uint8_t>(weapon + kWeaponSilenced) ? 1 : 0) | (G<uint8_t>(weapon + kWeaponScoped) ? 2 : 0));
    g_event.id_lo = entry ? R32(entry) : 0;
    g_event.id_hi = entry ? R32(entry + 4) : 0;
    g_open = true;
    __imp__sub_001DBD50(ctx, base);
    g_open = false;
    if (!g_event.count) return;  // a rocket, a grenade: not bullets
    g_event.seq = ++g_seq;
    g_count.sent++;
    uint64_t now = NowMs();
    g_event.ms = uint32_t(now);
    g_pending.push_back({now, g_event});
    if (g_pending.size() > 64) g_pending.pop_front();
}

// A player's bullet: ecx its origin, edx [kArmoury]; stack: the gun, the direction, range, speed, flags (ret 0x14).
// Its one call is 0x1DC0D1, inside 0x1DBD50's player branch. A bullet the full pool did not take is not sent.
X86_EXTERN_FUNC(__imp__sub_001DB7B0);
X86_FUNC(sub_001DB7B0) {
    bool take = g_open && !g_replaying && g_event.count < kMaxPellets;
    Pellet p;
    float range = 0, speed = 0;
    uint8_t flags = 0;
    if (take) {
        std::memcpy(p.origin, HostPtr(ctx.ecx), 16);
        std::memcpy(p.dir, HostPtr(R32(ctx.esp + 8)), 16);
        range = G<float>(ctx.esp + 0xC);
        speed = G<float>(ctx.esp + 0x10);
        flags = G<uint8_t>(ctx.esp + 0x14);
    }
    __imp__sub_001DB7B0(ctx, base);
    if (!take || !(ctx.eax & 0xFF)) return;
    if (!g_event.count) {
        g_event.range = range;
        g_event.speed = speed;
        g_event.flags = flags;
    }
    g_event.pellets[g_event.count++] = p;
}

// The bullets' step, stdcall, [esp+4] the armoury; once a frame of play from the world update (0x1F1222), after the
// player's update and before the characters, physics and effects: the partner's bullets join here and step at once.
X86_EXTERN_FUNC(__imp__sub_001E28C0);
X86_FUNC(sub_001E28C0) {
    uint32_t world = R32(kWorld);
    if (world) coop::world::Tick(ctx);  // the partner's world events, after the characters' update
    // the bullets read their owner's weapon: none in hand, none fired or stepped
    bool ready = world && R32(Player() + kPlayerWeapon) && TypeTable();
    if (ready && !g_pool) {
        g_pool = mem::AllocSystem(kPoolSize, 16);
        std::memset(HostPtr(g_pool), 0, kPoolSize);
        g_buffers = mem::AllocSystem(48, 16);
    }
    if (ready) {
        int8_t level = G<int8_t>(world + kWorldLevel);
        uint64_t now = NowMs();
        while (!g_queue.empty()) {
            Stamped s = g_queue.front();
            g_queue.pop_front();
            if (s.event.level != level || now - s.at > kLateMs) g_count.dropped += s.event.count;
            else Fire(ctx, s.event);
        }
        Step(ctx, G<float>(world + kWorldDt));
    } else {
        DropQueue();
    }
    __imp__sub_001E28C0(ctx, base);
}

// The level loader's bullet reset, esi the armoury, plain ret; its one call is 0x1EDE4C. Ours go with the game's.
X86_EXTERN_FUNC(__imp__sub_001CEED0);
X86_FUNC(sub_001CEED0) {
    __imp__sub_001CEED0(ctx, base);
    if (g_pool)
        for (int b = 0; b < kBulletCount; b++) G<uint8_t>(Bullet(b) + kBulletLive) = 0;
    DropQueue();
    coop::world::LevelReset();
}
