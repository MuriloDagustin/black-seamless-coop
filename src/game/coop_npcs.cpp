// Co-op world sync, stage 4: the host's enemies as the joining copy's puppets.
// A character's think is its AI controller's vf0 0x1E5F70 (from the AI manager 0x1B3420); the body then follows
// the controller in the pool update 0x1CCBB0 -> 0x1F08D0: its yaw +0x10, torso +0x14, aim +0x18 and pitch +0x1C in
// degrees, the move vector +0x70 and rate +0x84 for the locomotion blend, crouch +0x40 and the pad bytes +0x41..
// for the weapon. A puppet skips its think, has those written from the host's, is kinematic (+0x3A7: no gravity,
// no physics write back, no ground snap) and is put where the host's is after the pool update.
// The partner's body is made the way the ally spawn 0x1B29D0 makes an ally (0x1AF880 with a squad mate's type) and
// played the same way from the partner's states. The enemies' perception 0x1AD440 takes the nearest seen of the
// four squad slots [AI]+0x22974 (the player one of them) for its target: on the host the body is in an empty one
// for that call, so they turn on it and shoot; the joining copy's puppets of them then shoot from the host's aim,
// at the player the body stands for.
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "game/coop.h"
#include "game/coop_npcs.h"
#include "game/coop_world.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using layout::kWorld;

namespace coop::npcs {

namespace {

constexpr uint32_t kPool = 0x2D1980;
constexpr uint32_t kAliveList = 0x79B0;  // flags, cap +4, count +8, array +0xC (holes allowed)
constexpr uint32_t kCharName = 0x08;
constexpr uint32_t kCharPos = 0xA0;
constexpr uint32_t kCharWeapon = 0x29C;
constexpr uint32_t kCharHealth = 0x2F0;
constexpr uint32_t kCharControl = 0x324;
constexpr uint32_t kCharState = 0x384;
constexpr uint32_t kCharTeam = 0x39C;
constexpr uint32_t kCharKinematic = 0x3A7;
constexpr uint32_t kCharHit = 0x3AB;
constexpr uint32_t kControlChar = 0x8C;  // the AI controller's character
constexpr uint32_t kControlAngles = 0x10;  // yaw, torso yaw, aim yaw, pitch
constexpr uint32_t kControlSpeed = 0x20;
constexpr uint32_t kControlCrouch = 0x40;
constexpr uint32_t kControlFire = 0x41;
constexpr uint32_t kControlMove = 0x60;     // the displacement's direction, times +0x20 a second
constexpr uint32_t kControlMoveDir = 0x70;  // for the locomotion's direction
constexpr uint32_t kControlRate = 0x84;
constexpr uint32_t kControlLocomotion = 0x4A; // selects gait instead of S_MVI in 0x17FDF0
constexpr uint32_t kWeaponAim = 0x10;
constexpr uint32_t kWeaponState = 0xD8;   // 4 reloading, 0x1B..0x1D a melee swing
constexpr uint32_t kWeaponBarrel = 0xF4;  // its +0x18 the rounds left in the clip (int16)
constexpr uint32_t kCharAnim = 0x328;     // the animation controller: requests by 0x17F270 (ecx it, eax &request)
constexpr uint32_t kControlReload = 0x44;
constexpr uint32_t kControlMelee = 0x4B;
constexpr uint32_t kControlThrow = 0x4C;
constexpr uint32_t kAnimRequest = 0x17F270;
constexpr uint32_t kWorldLevel = 0x5AEC;
// spawners: the logic manager's two arrays, counts u16 +0x18 and +0x1A
constexpr uint32_t kLogic = 0x2D45D0;
constexpr uint32_t kSpawnerName = 0x08;

constexpr uint32_t kSpawn = 0x1B3030;     // esi the spawner; plain ret: its character at +0x24
constexpr uint32_t kSetPos = 0x1CE660;    // ecx the entity, eax &pos16; plain ret
constexpr uint32_t kTeleport = 0x1CCCA0;  // esi the character, ebx &{x, y, z, yaw degrees}; plain ret
constexpr uint32_t kRelease = 0x1DA1A0;   // stdcall (pool, character); ret 8

constexpr uint64_t kFreshMs = 1500;     // a snapshot older: the host paused or gone, the puppets stand
constexpr uint64_t kSpawnRetryMs = 500;
constexpr uint64_t kGoneMs = 1200;      // absent from the host's that long: let go here
constexpr float kTeleportFar = 2.0f;
constexpr float kFollow = 15.0f;  // a second: how fast a puppet closes on the host's place

#pragma pack(push, 1)
struct Entry {
    uint32_t name_lo, name_hi;
    uint8_t life, crouch, fire, bits;  // bits: 1 a melee swing
    float pos[3];
    float angles[4];
    float move[3];
    float rate;
    float health;
    float aim[3];
    // its last hit reaction (an animation request 5): counted, its clip, the part hit, from behind
    uint8_t hit_seq, hit_code, hit_part, hit_back;
};
#pragma pack(pop)

struct Puppet {
    Entry e;
    uint64_t at = 0;          // the snapshot it was in
    uint64_t tried = 0;       // the last spawn asked of its spawner
    bool known = false;       // a snapshot had it before: its hit count is the one played
    uint8_t hit_played = 0;
    bool melee = false;       // the host's swing, to start here
    bool melee_was = false;
};

// the host's: each character's last hit reaction
struct Hit {
    uint8_t seq = 0, code = 0, part = 0, back = 0;
};
std::unordered_map<uint64_t, Hit> g_hits;

bool g_host = false, g_puppets = false, g_bodies = false;

// the partner's body: an ally made here (as the ally spawn 0x1B29D0 makes one, 0x1AF880) and driven from the
// partner's states, the host's enemies seeing it as one of the squad (an empty squad slot during their perception)
constexpr uint32_t kMakeCharacter = 0x1AF880;  // ecx the source, edi the AI's counter; 12 stack args; ret 0x30
constexpr uint32_t kMakeRoom = 0x1E6860;       // edx the pool; al room in the living list (culls one if full)
constexpr uint32_t kAi = 0x2D1988;
constexpr uint32_t kAiCounter = 0xFA4;
constexpr uint32_t kAiFreeControl = 0x22CDC;
constexpr uint32_t kAiSquad = 0x22974;  // four characters, the player among them
constexpr uint32_t kAiBehaviourPool = 0x22C48 + 0x7C; // pointers, used bytes, count; distinct from controllers/living list
constexpr uint32_t kPoolTypes = 0x7A20;  // the character types of 0x40, +0 their model when loaded
constexpr uint32_t kBodyName = 0xC0FFEE00, kBodyNameHi = 0x00C00B0D;
constexpr uint32_t kBodyTypes[] = {0x1E, 0x1D, 0x1F, 0x25};  // the squad mates, a soldier: the first loaded
constexpr uint64_t kBodyRetryMs = 2000;
constexpr uint64_t kBodyGoneMs = 3000;  // the partner gone from this level that long (not paused in it): let go
constexpr float kBodyRun = 6.5f;
struct BodyState {
    uint32_t character = 0, source = 0, sub = 0;
    uint64_t tried = 0, seen = 0;
    float direction[3] = {0, 0, 1}, rate = 0;
};
std::array<BodyState, coop::kMaxPlayers> g_body_states;
BodyState& CurrentBody() { return g_body_states[coop::CurrentPartner()]; }
#define g_body (CurrentBody().character)
#define g_body_src (CurrentBody().source)
#define g_body_sub (CurrentBody().sub)
#define g_body_tried (CurrentBody().tried)
#define g_body_seen (CurrentBody().seen)
#define g_body_dir (CurrentBody().direction)
#define g_body_rate (CurrentBody().rate)
int BodyIndex(uint32_t character) {
    if (!character) return -1;
    for (int i = 0; i < coop::kMaxPlayers; i++)
        if (g_body_states[i].character == character && R32(character + kCharName) == kBodyName + uint32_t(i) &&
            R32(character + kCharName + 4) == kBodyNameHi) return i;
    return -1;
}
std::unordered_map<uint64_t, Puppet> g_state;  // the host's, by spawner name
std::unordered_map<uint64_t, uint64_t> g_absent;  // this copy's enemies the host has not, since
std::unordered_map<uint64_t, uint64_t> g_died;    // puppets died here, when: the host's older states still have them
constexpr uint64_t kDeadHoldMs = 300;
uint64_t g_snap_at = 0;
int8_t g_snap_level = -1;
Counters g_count;
uint32_t g_pos = 0;  // 16 aligned scratch

uint64_t NowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint64_t Name(uint32_t lo, uint32_t hi) { return uint64_t(lo) | uint64_t(hi) << 32; }

uint64_t NameOf(uint32_t ch) { return Name(R32(ch + kCharName), R32(ch + kCharName + 4)); }

// the pool's living characters
template <class F>
void Living(F f) {
    uint32_t pool = R32(kPool);
    if (!pool) return;
    uint32_t list = pool + kAliveList;
    uint32_t cap = std::min(R32(list + 4), 64u);
    uint32_t array = R32(list + 0xC);
    for (uint32_t i = 0; array && i < cap; i++) {
        uint32_t ch = R32(array + i * 4);
        if (ch && R32(ch + kCharState) == 0 && R32(ch + kCharControl) && NameOf(ch)) f(ch);
    }
}

// the host's latest is of now: recent, of this level, the host in play in it
bool Fresh(uint64_t now) {
    coop::PartnerScope host(0);
    uint32_t world = R32(kWorld);
    float feet[3];
    return g_snap_at && now - g_snap_at < kFreshMs && world && g_snap_level == G<int8_t>(world + kWorldLevel) &&
           coop::PartnerFeet(feet);
}

// On the joining copy every living AI character of a spawner is the host's, in the host's latest or not (its
// latest may already miss one the host has just killed while the host's bullets still fly here)
// the host's enemies are its while it is heard; allies (team 0) are each copy's own
bool Owning() { return g_puppets && coop::HostHeard(); }

bool HostOwned(uint32_t ch) {
    return Owning() && ch && !IsBody(ch) && R32(ch + kCharState) == 0 && R32(ch + kCharTeam) != 0 &&
           R32(ch + kCharControl) && NameOf(ch) && R32(R32(ch + kCharControl) + kControlChar) == ch;
}

bool BodyAlive() { return g_body && R32(g_body + kCharState) == 0 && NameOf(g_body) == Name(kBodyName + uint32_t(coop::CurrentPartner()), kBodyNameHi); }

// The AI characters' behaviour slots ([AI]+0x22C48+0x7C: pointers, used bytes, count; sixteen). Every AI character
// takes one in 0x1899F0, which writes slot+0x20 even when its search found none (0x189A23-0x189A33: a write at 0x20,
// the crash of a full pool). The game never has sixteen AI characters alive; the partners' bodies (one each, up to
// seven) and the puppets make it reachable
int BehaviourFree(uint32_t ai) {
    uint32_t pool = ai + kAiBehaviourPool;
    uint32_t pointers = R32(pool), used = R32(pool + 4), count = R32(pool + 8);
    int n = 0;
    for (uint32_t i = 0; pointers && used && i < std::min(count, 64u); i++)
        if (!G<uint8_t>(used + i) && R32(pointers + i * 4)) n++;
    return n;
}

bool BehaviourRoom(uint32_t ai) { return BehaviourFree(ai) > 0; }

constexpr int kBodyHeadroom = 2;  // a body is made only with this many free: the level's own enemies keep theirs

void MakeBody(X86Context& ctx, const float feet[3]) {
    uint32_t pool = R32(kPool), ai = R32(kAi);
    if (!pool || !ai || !R32(ai + kAiFreeControl) || BehaviourFree(ai) < kBodyHeadroom) return;
    if (R32(pool + kAliveList + 4) == R32(pool + kAliveList + 8)) return;  // full: 0x1E6860 would cull one for it
    uint32_t resources = R32(pool + 0x8528);
    if (!resources || !R32(resources + 0x24)) return;
    static constexpr hle::Map kFindType = hle::Make({hle::ecx, hle::stack, hle::stack});
    uint32_t type = 0;
    for (uint32_t t : kBodyTypes) {
        // The pool keeps nonzero pointers for types from previous missions,
        // even after those models have been freed. Require the type in this
        // mission's resource table, as the native type loader (0x1C4C30) does.
        uint32_t definition = hle::Call(ctx, 0x17FC0, kFindType,
                                       {R32(resources + 0x24), R32(0x288A10 + t * 8), R32(0x288A14 + t * 8)});
        if (definition && R32(pool + kPoolTypes + t * 0x40)) {
            type = t;
            break;
        }
    }
    if (!type) return;
    static constexpr hle::Map kRoomMap = hle::Make({hle::edx});
    if (!(hle::Call(ctx, kMakeRoom, kRoomMap, {pool}) & 0xFF)) return;  // a free entry of [0x2D45F4]+0x2960
    if (!g_body_src) g_body_src = mem::AllocSystem(0x40, 16), g_body_sub = mem::AllocSystem(0x40, 16);
    std::memset(HostPtr(g_body_src), 0, 0x40);
    std::memset(HostPtr(g_body_sub), 0, 0x40);
    W32(g_body_src + 0x08, kBodyName + uint32_t(coop::CurrentPartner()));
    W32(g_body_src + 0x0C, kBodyNameHi);
    float pos[4] = {feet[0], feet[1], feet[2], 1};
    std::memcpy(HostPtr(g_body_sub + 0x10), pos, 16);
    static constexpr hle::Map kMakeMap = hle::Make({hle::ecx, hle::edi, hle::stack, hle::stack, hle::stack, hle::stack,
                                                    hle::stack, hle::stack, hle::stack, hle::stack, hle::stack,
                                                    hle::stack, hle::stack, hle::stack});
    // the arguments 0x1B29D0 gives it, its weapon's name included
    uint32_t ctl = hle::Call(ctx, kMakeCharacter, kMakeMap,
                             {g_body_src, ai + kAiCounter, type, 1, 0, 0, g_body_sub, 0xD7970000, 0x5446127A, 0, 0, 0,
                              0xFFFFFFFF, 0});
    uint32_t ch = ctl ? R32(ctl + kControlChar) : 0;
    if (!ch) return;
    G<float>(ch + kCharHealth) = 3.4028235e38f;  // as 0x1B29D0 leaves an ally (0x2B2A39)
    W32(ch + kCharTeam, 0);
    g_body = ch;
    g_count.bodies++;
    LOGR("coop: the partner's body %08X made, type %02X, at %.1f %.1f %.1f", ch, type, double(feet[0]),
         double(feet[1]), double(feet[2]));
}

void ReleaseBody(X86Context& ctx) {
    if (BodyAlive()) {
        static constexpr hle::Map kReleaseMap = hle::Make({hle::stack, hle::stack});
        hle::Call(ctx, kRelease, kReleaseMap, {R32(kPool), g_body});
        LOGR("coop: the partner's body let go");
    }
    g_body = 0;
}

// the partner's state to its body's controller: its view's heading for the body, torso and aim, the elevation for
// the pitch, its walk for the locomotion
void DriveBody(uint32_t ctl) {
    const float* f = coop::PartnerForward();
    float h = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    float heading = std::atan2(f[0], f[2]) * 57.2957795f;
    float pitch = h > 0.01f ? -std::atan2(f[1], h) * 57.2957795f : 0.0f;
    float angles[4] = {heading, heading, heading, pitch};
    std::memcpy(HostPtr(ctl + kControlAngles), angles, 16);
    G<float>(ctl + kControlSpeed) = 0;
    std::memset(HostPtr(ctl + kControlMove), 0, 16);
    float move[4] = {g_body_dir[0], g_body_dir[1], g_body_dir[2], 0};
    std::memcpy(HostPtr(ctl + kControlMoveDir), move, 16);
    G<float>(ctl + kControlRate) = g_body_rate;
    G<uint8_t>(ctl + kControlCrouch) = coop::PartnerCrouch() ? 1 : 0;
    for (uint32_t b = 0x41; b <= 0x45; b++) G<uint8_t>(ctl + b) = 0;
    G<uint8_t>(ctl + 0x4B) = 0, G<uint8_t>(ctl + 0x4C) = 0;
    G<uint8_t>(g_body + kCharKinematic) = 1;
}

// Once a logic tick: use the sender's locomotion, holding it between packets.
void BodyTick(X86Context& ctx) {
    uint64_t now = NowMs();
    float feet[3];
    bool here = coop::PartnerFeet(feet);
    if (g_body && !BodyAlive()) g_body = 0;
    if (coop::PartnerInLevel()) g_body_seen = now;  // paused in it too: its body stands where it was
    if (coop::PartnerDown()) {  // dead: no body for the enemies to shoot until it is revived
        if (g_body) ReleaseBody(ctx);
        return;
    }
    if (!here) {
        if (g_body && now - g_body_seen > kBodyGoneMs) ReleaseBody(ctx);
        g_body_rate = 0;  // it stands
        return;
    }
    g_body_seen = now;
    if (!g_body && now - g_body_tried > kBodyRetryMs) {
        g_body_tried = now;
        MakeBody(ctx, feet);
    }
    coop::PartnerMotion(g_body_dir, g_body_rate);
}

// its state from the host, if the host's latest has it
Puppet* PuppetOf(uint32_t ch) {
    if (!HostOwned(ch)) return nullptr;
    auto it = g_state.find(NameOf(ch));
    return it == g_state.end() ? nullptr : &it->second;
}

uint32_t Spawner(uint64_t name) {
    uint32_t mgr = R32(kLogic);
    if (!mgr) return 0;
    for (uint32_t list = 0; list < 2; list++) {
        uint32_t n = G<uint16_t>(mgr + 0x18 + list * 2);
        uint32_t array = R32(mgr + 0x78 + list * 4);
        for (uint32_t i = 0; array && i < n && i < 4096; i++) {
            uint32_t s = R32(array + i * 4);
            if (s && Name(R32(s + kSpawnerName), R32(s + kSpawnerName + 4)) == name) return s;
        }
    }
    return 0;
}

// a puppet without the host's think: it stands
void Stand(uint32_t ctl) {
    G<uint8_t>(ctl + kControlFire) = 0;
    G<float>(ctl + kControlRate) = 0;
    G<float>(ctl + kControlSpeed) = 0;
    std::memset(HostPtr(ctl + kControlMove), 0, 16);
    for (uint32_t b = 0x42; b <= 0x45; b++) G<uint8_t>(ctl + b) = 0;
    G<uint8_t>(ctl + kControlMelee) = 0, G<uint8_t>(ctl + kControlThrow) = 0;
}

// the host's think written for the puppet's body
void Drive(X86Context& ctx, uint32_t ch, uint32_t ctl, Puppet& pp) {
    const Entry& e = pp.e;
    std::memcpy(HostPtr(ctl + kControlAngles), e.angles, 16);
    G<float>(ctl + kControlSpeed) = 0;  // no displacement of its own: its place is the host's
    std::memset(HostPtr(ctl + kControlMove), 0, 16);
    float move[4] = {e.move[0], e.move[1], e.move[2], 0};
    std::memcpy(HostPtr(ctl + kControlMoveDir), move, 16);
    G<float>(ctl + kControlRate) = e.rate;
    G<uint8_t>(ctl + kControlCrouch) = e.crouch;
    G<uint8_t>(ctl + kControlFire) = e.fire;
    for (uint32_t b = 0x42; b <= 0x45; b++) G<uint8_t>(ctl + b) = 0;
    G<uint8_t>(ctl + kControlMelee) = 0, G<uint8_t>(ctl + kControlThrow) = 0;  // its grenades come as the host's
    uint32_t weapon = R32(ch + kCharWeapon);
    if (weapon) {
        std::memcpy(HostPtr(weapon + kWeaponAim), e.aim, 12);
        // its own clip: reloaded when empty, as the AI asks (0x1A1114)
        uint32_t barrel = R32(weapon + kWeaponBarrel);
        uint32_t state = R32(weapon + kWeaponState);
        if (barrel && state != 4 && G<int16_t>(barrel + 0x18) <= 0) G<uint8_t>(ctl + kControlReload) = 1;
        // the host's swing, asked until this one swings
        if (pp.melee) {
            if (state >= 0x1B && state <= 0x1D) pp.melee = false;
            else G<uint8_t>(ctl + kControlMelee) = 1;
        }
    }
    // the host's hit reaction, once each
    if (pp.hit_played != e.hit_seq) {
        pp.hit_played = e.hit_seq;
        static uint32_t request = 0;
        if (!request) request = mem::AllocSystem(0x20, 16);
        std::memset(HostPtr(request), 0, 0x20);
        W32(request + 0x00, 5);
        W32(request + 0x04, e.hit_code);
        W32(request + 0x0C, e.hit_part);
        G<uint8_t>(request + 0x10) = e.hit_back;
        static constexpr hle::Map kAnimMap = hle::Make({hle::ecx, hle::eax});
        if (R32(ch + kCharAnim)) hle::Call(ctx, kAnimRequest, kAnimMap, {R32(ch + kCharAnim), request});
    }
    G<float>(ch + kCharHealth) = e.health;
    G<uint8_t>(ch + kCharKinematic) = 1;
    g_count.driven++;
}

}  // namespace

bool IsBody(uint32_t ch) { return BodyIndex(ch) >= 0; }

uint32_t Body() { return BodyAlive() ? g_body : 0; }

bool Puppets() { return Owning(); }

bool HostsEnemies() { return g_host; }

bool IsHostOwned(uint32_t ch) { return HostOwned(ch); }

void SetMode(bool host, bool puppets, bool body) {
    g_host = host;
    g_puppets = puppets && !host;
    g_bodies = body;
}

void Stop(X86Context& ctx) {
    // Give enemies their physics back before releasing ownership.
    Living([](uint32_t ch) {
        if (g_puppets && !IsBody(ch) && R32(ch + kCharTeam) != 0)
            G<uint8_t>(ch + kCharKinematic) = 0;
    });
    for (int i = 0; i < coop::kMaxPlayers; i++) { coop::PartnerScope scope(i); ReleaseBody(ctx); }
    SetMode(false, false, false);
    LevelReset();
}

int Snapshot(uint8_t* out, int cap, int& bytes) {
    bytes = 0;
    if (!g_host || !R32(kWorld)) return 0;
    int n = 0;
    Living([&](uint32_t ch) {
        if (bytes + int(sizeof(Entry)) > cap || IsBody(ch) || R32(ch + kCharTeam) == 0) return;
        uint32_t ctl = R32(ch + kCharControl);
        Entry e{};
        e.name_lo = R32(ch + kCharName), e.name_hi = R32(ch + kCharName + 4);
        e.life = 0;
        e.crouch = G<uint8_t>(ctl + kControlCrouch);
        e.fire = G<uint8_t>(ctl + kControlFire);
        uint32_t weapon = R32(ch + kCharWeapon);
        uint32_t wstate = weapon ? R32(weapon + kWeaponState) : 0;
        e.bits = (G<uint8_t>(ctl + kControlMelee) || (wstate >= 0x1B && wstate <= 0x1D)) ? 1 : 0;
        if (auto h = g_hits.find(NameOf(ch)); h != g_hits.end())
            e.hit_seq = h->second.seq, e.hit_code = h->second.code, e.hit_part = h->second.part,
            e.hit_back = h->second.back;
        std::memcpy(e.pos, HostPtr(ch + kCharPos), 12);
        std::memcpy(e.angles, HostPtr(ctl + kControlAngles), 16);
        std::memcpy(e.move, HostPtr(ctl + kControlMoveDir), 12);
        e.rate = G<float>(ctl + kControlRate);
        e.health = G<float>(ch + kCharHealth);
        if (weapon) std::memcpy(e.aim, HostPtr(weapon + kWeaponAim), 12);
        std::memcpy(out + bytes, &e, sizeof e);
        bytes += int(sizeof e);
        n++;
    });
    return n;
}

void OnSnapshot(const uint8_t* p, int bytes, int count, int8_t level, uint64_t now) {
    if (!g_puppets) return;
    g_snap_at = now;
    g_snap_level = level;
    g_count.snapshots++;
    std::unordered_map<uint64_t, Puppet> next;
    for (int i = 0; i < count && (i + 1) * int(sizeof(Entry)) <= bytes; i++) {
        Entry e;
        std::memcpy(&e, p + i * sizeof(Entry), sizeof e);
        bool finite = true;
        for (float f : e.pos) finite &= std::isfinite(f);
        for (float f : e.angles) finite &= std::isfinite(f);
        if (!finite) continue;
        uint64_t name = Name(e.name_lo, e.name_hi);
        if (auto d = g_died.find(name); d != g_died.end() && now - d->second < kDeadHoldMs) continue;
        Puppet pp{};
        auto it = g_state.find(name);
        if (it != g_state.end()) pp = it->second;
        pp.e = e;
        pp.at = now;
        if (!pp.known) pp.hit_played = e.hit_seq, pp.known = true;  // a hit of before it was here is not played
        bool swing = e.bits & 1;
        if (swing && !pp.melee_was) pp.melee = true;
        pp.melee_was = swing;
        next[name] = pp;
    }
    g_state.swap(next);
}

void LogicTick(X86Context& ctx) {
    if (g_bodies) for (int i = 0; i < coop::kMaxPlayers; i++) {
        if (i == coop::LocalPlayer()) continue;
        coop::PartnerScope scope(i);
        BodyTick(ctx);
    }
    if (!g_puppets) return;
    uint64_t now = NowMs();
    // the host not heard: its enemies here are this copy's own again, its spawners and AI theirs
    static bool owning = false;
    bool own = Owning();
    if (owning && !own) {
        Living([&](uint32_t ch) {
            if (!IsBody(ch) && R32(ch + kCharTeam) != 0) G<uint8_t>(ch + kCharKinematic) = 0;
        });
        g_state.clear();
        g_absent.clear();
        g_died.clear();
        LOGR("npc: the host is not heard, its enemies are this copy's own again");
    } else if (!owning && own) {
        LOGR("npc: the host is heard, its enemies are its own");
    }
    owning = own;
    if (!own || !Fresh(now)) return;
    uint32_t pool = R32(kPool);
    static constexpr hle::Map kReleaseMap = hle::Make({hle::stack, hle::stack});
    // the host's that are not here: their spawners make them
    static constexpr hle::Map kSpawnMap = hle::Make({hle::esi});
    for (auto& [name, pp] : g_state) {
        if (world::LiveCharacter(name) || now - pp.tried < kSpawnRetryMs) continue;
        if (auto d = g_died.find(name); d != g_died.end()) {
            if (now - d->second < kDeadHoldMs) continue;  // a state the host sent before it died
            g_died.erase(d);
        }
        uint32_t s = Spawner(name);
        if (!s) {  // its unit is not here yet
            pp.tried = now;
            continue;
        }
        uint32_t ai = R32(kAi);
        if (!ai || !BehaviourRoom(ai)) {
            pp.tried = now;
            continue; // replicated enemies need a behaviour slot as well
        }
        // the living list full: the spawn would make 0x1E6860 cull one, maybe one the host has. One the host has
        // not goes first, else it waits
        if (pool && R32(pool + kAliveList + 8) >= R32(pool + kAliveList + 4)) {
            uint32_t extra = 0;
            uint64_t since = now;
            Living([&](uint32_t ch) {
                uint64_t n = NameOf(ch);
                if (IsBody(ch) || R32(ch + kCharTeam) == 0 || g_state.count(n)) return;
                auto a = g_absent.find(n);
                uint64_t at = a == g_absent.end() ? now : a->second;
                if (!extra || at < since) extra = ch, since = at;
            });
            if (!extra) continue;
            LOGR("npc: %016llX let go for room, the host has none", (unsigned long long)NameOf(extra));
            g_absent.erase(NameOf(extra));
            hle::Call(ctx, kRelease, kReleaseMap, {pool, extra});
            g_count.let_go++;
            if (R32(pool + kAliveList + 8) >= R32(pool + kAliveList + 4)) continue;
        }
        pp.tried = now;
        hle::Call(ctx, kSpawn, kSpawnMap, {s});
        if (world::LiveCharacter(name)) {
            g_count.made++;
            LOGR("npc: puppet %016llX made by its spawner", (unsigned long long)name);
        } else {
            g_count.refused++;
        }
    }
    // this copy's that the host has not: let go once a while absent
    std::vector<uint32_t> gone;
    Living([&](uint32_t ch) {
        uint64_t name = NameOf(ch);
        if (IsBody(ch) || R32(ch + kCharTeam) == 0 || g_state.count(name)) {
            g_absent.erase(name);
            return;
        }
        auto [it, fresh] = g_absent.try_emplace(name, now);
        if (!fresh && now - it->second > kGoneMs) gone.push_back(ch);
    });
    for (uint32_t ch : gone) {
        uint64_t name = NameOf(ch);
        LOGR("npc: %016llX let go, the host has none", (unsigned long long)name);
        g_absent.erase(name);
        hle::Call(ctx, kRelease, kReleaseMap, {pool, ch});
        g_count.let_go++;
    }
}

void Died(uint64_t name) {
    if (!g_puppets) return;
    g_died[name] = NowMs();
    g_state.erase(name);
}

void LevelReset() {
    g_hits.clear();
    for (auto& body : g_body_states) {
        // Scratch records are ours and can be reused; guest characters cannot.
        auto source = body.source, sub = body.sub;
        body = {};
        body.source = source; body.sub = sub;
    }
    g_state.clear();
    g_absent.clear();
    g_died.clear();
    g_snap_at = 0;
}

const Counters& Count() { return g_count; }

}  // namespace coop::npcs

using namespace coop::npcs;

// The joining copy's spawners make nothing of their own (eax the spawner, plain ret): the host says what lives
X86_EXTERN_FUNC(__imp__sub_001B3070);
X86_FUNC(sub_001B3070) {
    if (Owning()) return Return(ctx, ctx.eax, 0);
    __imp__sub_001B3070(ctx, base);
}

// An AI controller's think (ecx the controller, plain ret): a puppet's is the host's
X86_EXTERN_FUNC(__imp__sub_001E5F70);
X86_FUNC(sub_001E5F70) {
    uint32_t ctl = ctx.ecx;
    uint32_t ch = R32(ctl + kControlChar);
    int player = BodyIndex(ch);
    if (player >= 0) {
        coop::PartnerScope scope(player);
        if (!BodyAlive()) return Return(ctx, ctx.eax, 0);
        DriveBody(ctl);
        return Return(ctx, ctx.eax, 0);
    }
    if (HostOwned(ch)) {
        // the host's think, or none while the host has not sent it (it stands until it comes or is let go)
        Puppet* pp = PuppetOf(ch);
        if (pp && Fresh(NowMs())) Drive(ctx, ch, ctl, *pp);
        else Stand(ctl);
        return Return(ctx, ctx.eax, 0);
    }
    __imp__sub_001E5F70(ctx, base);
}

// Animation needs measured speed AND the native gait flag / movement speed.
// 0x17FDF0 selects S_MVI (moving idle) when +0x4A is clear. Nonzero +0x20
// enables 0x17FC50's direction-aware gait selection (including S_MOV).
// Supply these only while selecting animations; local physics must still see
// zero movement speed because the remote position is applied after its update.
X86_EXTERN_FUNC(__imp__sub_0017E280);
X86_FUNC(sub_0017E280) {
    uint32_t ch = R32(ctx.esi);
    int player = BodyIndex(ch);
    coop::PartnerScope scope(player >= 0 ? player : coop::CurrentPartner());
    uint32_t controller = 0;
    float previous_speed = 0;
    uint8_t previous_gait = 0;
    if ((player >= 0 && BodyAlive()) || HostOwned(ch)) {
        uint32_t ctl = R32(ch + kCharControl);
        float speed = ctl ? G<float>(ctl + kControlRate) : 0.0f;
        speed = std::isfinite(speed) ? std::clamp(speed, 0.0f, kBodyRun) : 0.0f;
        G<float>(ch + 0x2D8) = speed;
        if (ctl) {
            controller = ctl;
            previous_speed = G<float>(ctl + kControlSpeed);
            previous_gait = G<uint8_t>(ctl + kControlLocomotion);
            G<float>(ctl + kControlSpeed) = speed;
            G<uint8_t>(ctl + kControlLocomotion) = speed > 0.1f;
        }
    }
    __imp__sub_0017E280(ctx, base);
    if (controller) {
        G<float>(controller + kControlSpeed) = previous_speed;
        G<uint8_t>(controller + kControlLocomotion) = previous_gait;
    }
}

// The characters' update (esi the pool; dt; ret 4): then each puppet is put where the host's is
X86_EXTERN_FUNC(__imp__sub_001CCBB0);
X86_FUNC(sub_001CCBB0) {
    float dt = G<float>(ctx.esp + 4);
    __imp__sub_001CCBB0(ctx, base);
    if (!g_pos) g_pos = mem::AllocSystem(16, 16);
    static constexpr hle::Map kSetPosMap = hle::Make({hle::ecx, hle::eax});
    static constexpr hle::Map kTeleportMap = hle::Make({hle::esi, hle::ebx});
    float k = std::clamp(std::isfinite(dt) ? dt * kFollow : 1.0f, 0.0f, 1.0f);
    for (int player = 0; player < coop::kMaxPlayers; player++) {
        coop::PartnerScope scope(player);
    // the partner's body where the partner stands
    float feet[3];
    if (BodyAlive() && coop::PartnerFeet(feet)) {
        const float* now = &G<float>(g_body + kCharPos);
        float d[3] = {feet[0] - now[0], feet[1] - now[1], feet[2] - now[2]};
        const float* f = coop::PartnerForward();
        if (std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) > kTeleportFar) {
            float to[4] = {feet[0], feet[1], feet[2], std::atan2(f[0], f[2]) * 57.2957795f};
            std::memcpy(HostPtr(g_pos), to, 16);
            hle::Call(ctx, kTeleport, kTeleportMap, {g_body, g_pos});
        } else {
            float to[4] = {now[0] + d[0] * k, now[1] + d[1] * k, now[2] + d[2] * k, now[3]};
            std::memcpy(HostPtr(g_pos), to, 16);
            hle::Call(ctx, kSetPos, kSetPosMap, {g_body, g_pos});
        }
    }
    }
    if (!g_puppets || !Fresh(NowMs())) return;
    for (auto& [name, pp] : g_state) {
        uint32_t ch = coop::world::LiveCharacter(name);
        if (!ch) continue;
        const float* now = &G<float>(ch + kCharPos);
        float d[3] = {pp.e.pos[0] - now[0], pp.e.pos[1] - now[1], pp.e.pos[2] - now[2]};
        float far = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (far > kTeleportFar) {
            float to[4] = {pp.e.pos[0], pp.e.pos[1], pp.e.pos[2], pp.e.angles[0]};
            std::memcpy(HostPtr(g_pos), to, 16);
            hle::Call(ctx, kTeleport, kTeleportMap, {ch, g_pos});
        } else {
            float to[4] = {now[0] + d[0] * k, now[1] + d[1] * k, now[2] + d[2] * k, now[3]};
            std::memcpy(HostPtr(g_pos), to, 16);
            hle::Call(ctx, kSetPos, kSetPosMap, {ch, g_pos});
        }
    }
}

// A puppet takes no harm here: the host's decides (TakeDamage, the blast and the melee hit, thiscall ecx the victim)
X86_EXTERN_FUNC(__imp__sub_001E74B0);
X86_FUNC(sub_001E74B0) {
    if (IsBody(ctx.ecx)) return Return(ctx, 0, 7);  // the partner's, hurt on its own copy
    if (HostOwned(ctx.ecx) && !coop::world::Applying()) {
        G<uint8_t>(ctx.ecx + kCharHit) = 1;
        return Return(ctx, 0, 7);
    }
    __imp__sub_001E74B0(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_001DF830);
X86_FUNC(sub_001DF830) {
    if (IsBody(ctx.ecx) || (HostOwned(ctx.ecx) && !coop::world::Applying())) return Return(ctx, 0, 7);
    __imp__sub_001DF830(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_001DFA90);
X86_FUNC(sub_001DFA90) {
    if (IsBody(ctx.ecx)) return Return(ctx, 0, 4);
    if (HostOwned(ctx.ecx) && !coop::world::Applying()) {
        coop::world::SendMelee(NameOf(ctx.ecx), R32(ctx.esp + 8));  // the host's decides; &dir its second
        return Return(ctx, 0, 4);
    }
    __imp__sub_001DFA90(ctx, base);
}

// An enemy's perception (stdcall: its perception, [+0] the controller; ret 4) looks at the four squad slots and takes
// the nearest it sees for its target (+4): on the host the partner's body is in an empty slot for it
X86_EXTERN_FUNC(__imp__sub_001AD440);
X86_FUNC(sub_001AD440) {
    uint32_t ai = R32(kAi);
    uint32_t perception = R32(ctx.esp + 4);
    std::array<uint32_t, 4> original{};
    bool replaced = false;
    if (g_host && g_bodies && ai) {
        std::vector<uint32_t> candidates;
        for (int i = 0; i < 4; i++) {
            original[i] = R32(ai + kAiSquad + uint32_t(i) * 4);
            if (original[i]) candidates.push_back(original[i]);
        }
        for (int player = 0; player < coop::kMaxPlayers; player++) {
            coop::PartnerScope scope(player);
            float feet[3];
            if (BodyAlive() && coop::PartnerFeet(feet) &&
                std::find(candidates.begin(), candidates.end(), g_body) == candidates.end()) candidates.push_back(g_body);
        }
        uint32_t controller = perception ? R32(perception) : 0;
        uint32_t enemy = controller ? R32(controller + kControlChar) : 0;
        if (enemy && candidates.size()) {
            const float* position = &G<float>(enemy + kCharPos);
            auto distance = [&](uint32_t ch) {
                float sum = 0;
                for (int k = 0; k < 3; k++) { float d = G<float>(ch + kCharPos + uint32_t(k) * 4) - position[k]; sum += d*d; }
                return sum;
            };
            std::stable_sort(candidates.begin(), candidates.end(), [&](uint32_t a, uint32_t b) { return distance(a) < distance(b); });
            for (int i = 0; i < 4; i++) W32(ai + kAiSquad + uint32_t(i) * 4, i < int(candidates.size()) ? candidates[i] : 0);
            replaced = true;
        }
    }
    __imp__sub_001AD440(ctx, base);
    if (replaced) {
        for (int i = 0; i < 4; i++) W32(ai + kAiSquad + uint32_t(i) * 4, original[i]);
        g_count.seen++;
        if (IsBody(R32(perception + 4))) g_count.targeted++;
    }
}

// An animation request (ecx a character's controller +0x328, eax &request; plain ret): on the host a hit reaction
// (request 5) of an enemy is counted for its puppet
X86_EXTERN_FUNC(__imp__sub_0017F270);
X86_FUNC(sub_0017F270) {
    if (g_host && ctx.ecx && ctx.eax && R32(ctx.eax) == 5) {
        uint32_t anim = ctx.ecx, request = ctx.eax;
        Living([&](uint32_t ch) {
            if (R32(ch + kCharAnim) != anim || IsBody(ch) || R32(ch + kCharTeam) == 0) return;
            Hit& h = g_hits[NameOf(ch)];
            h.seq++;
            h.code = uint8_t(R32(request + 4));
            h.part = uint8_t(R32(request + 0xC));
            h.back = G<uint8_t>(request + 0x10);
        });
    }
    __imp__sub_0017F270(ctx, base);
}

// A character hit by a moving body (vf9, thiscall ecx the victim; ret 0x10) and crushed (0x1D6000: edx the victim,
// edi the other; impulse; ret 4) dies at once: not a puppet nor the partner's body, the host's decides
X86_EXTERN_FUNC(__imp__sub_001DFC30);
X86_FUNC(sub_001DFC30) {
    if (IsBody(ctx.ecx) || (HostOwned(ctx.ecx) && !coop::world::Applying())) return Return(ctx, 0, 4);
    __imp__sub_001DFC30(ctx, base);
}

X86_EXTERN_FUNC(__imp__sub_001D6000);
X86_FUNC(sub_001D6000) {
    if (IsBody(ctx.edx) || (HostOwned(ctx.edx) && !coop::world::Applying())) return Return(ctx, ctx.eax, 1);
    __imp__sub_001D6000(ctx, base);
}

// The AI's nearest-character scans (0x1C2F30: ecx a sphere; 0x1C30D0: &pos, edx the out list; ret 4) leave out the
// squad by controller type: the body, of an enemy soldier's type where the level has no squad mate's, is left out
// too (its controller's type +0x90 not an AI's while they run). A grenade's warning then goes to a real enemy
template <class F>
void WithoutBody(F f) {
    std::array<uint32_t, coop::kMaxPlayers> controls{}, types{};
    for (int i = 0; i < coop::kMaxPlayers; i++) {
        coop::PartnerScope scope(i);
        controls[i] = BodyAlive() ? R32(g_body + kCharControl) : 0;
        if (controls[i]) { types[i] = R32(controls[i] + 0x90); W32(controls[i] + 0x90, 0); }
    }
    f();
    for (int i = 0; i < coop::kMaxPlayers; i++) if (controls[i]) W32(controls[i] + 0x90, types[i]);
}

X86_EXTERN_FUNC(__imp__sub_001C2F30);
X86_FUNC(sub_001C2F30) {
    WithoutBody([&] { __imp__sub_001C2F30(ctx, base); });
}

X86_EXTERN_FUNC(__imp__sub_001C30D0);
X86_FUNC(sub_001C30D0) {
    WithoutBody([&] { __imp__sub_001C30D0(ctx, base); });
}

// The room check every character spawn makes first (edx the pool; al: room; 0x1B25D6, 0x1B264B, 0x1B29F5, 0x1B2A85,
// 0x1B2B15 before each 0x1AF880, and MakeBody): no room either while no behaviour slot is free. The spawner tries
// again later instead of 0x1899F0 writing through a null slot
X86_EXTERN_FUNC(__imp__sub_001E6860);
X86_FUNC(sub_001E6860) {
    __imp__sub_001E6860(ctx, base);
    uint32_t ai = R32(kAi);
    if ((ctx.eax & 0xFF) && ai && !BehaviourRoom(ai)) {
        static uint64_t told = 0;
        uint64_t now = NowMs();
        if (now - told > 5000) {
            told = now;
            LOGR("npc: no behaviour slot free: a spawn waits");
        }
        ctx.eax &= ~0xFFu;
    }
}

// An AI character's behaviour slot taken (ecx [AI]+0x22C48, edi the character; plain ret, al 1). A spawn that came
// some other way with the pool full: a partner's body is let go for it (made again once there is room), else the
// character goes without one rather than the write through null
X86_EXTERN_FUNC(__imp__sub_001899F0);
X86_FUNC(sub_001899F0) {
    uint32_t ch = ctx.edi, ai = R32(kAi);
    if (ai && ctx.ecx == ai + 0x22C48 && R32(ch + 0xC4) == 1 && !BehaviourRoom(ai)) {
        int self = BodyIndex(ch);
        for (int i = 0; i < coop::kMaxPlayers && !BehaviourRoom(ai); i++) {
            if (i == self) continue;
            coop::PartnerScope scope(i);
            if (!BodyAlive()) continue;
            LOGR("npc: behaviour slots full: partner %d's body let go for %016llX", i, (unsigned long long)NameOf(ch));
            ReleaseBody(ctx);
        }
        if (!BehaviourRoom(ai)) {
            LOGR("npc: behaviour slots full: %016llX goes without one", (unsigned long long)NameOf(ch));
            W32(ch + 0x344, 0);
            return Return(ctx, (ctx.eax & ~0xFFu) | 1, 0);
        }
    }
    __imp__sub_001899F0(ctx, base);
}
