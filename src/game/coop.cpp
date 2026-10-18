// Co-op, phase 1: the partner's player as a marker over the frame.
// The player is the world's +0x30. Its +0xD0 holds four rows of 16 bytes: right, up, forward and the eye, the last
// two the world space view hooks.cpp also reads. The camera's view and projection are the device copies that
// D3DDevice_SetTransform keeps, set by the camera begin update 0x2B830 before the scene draws.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/app.h"
#include "common/aspect.h"
#include "common/options.h"
#include "core/log.h"
#include "game/coop.h"
#include "game/coop_control.h"
#include "game/coop_life.h"
#include "game/coop_net.h"
#include "game/coop_npcs.h"
#include "game/coop_online.h"
#include "game/coop_shots.h"
#include "game/coop_world.h"
#include "game/layout.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

extern const char kImageSha1[];  // the xbe recompiled into this executable, written by CMake

using namespace kernel;
namespace sprint { void Tick(); }

using layout::kWorld;
using layout::kWorldPlayer;

namespace coop {

namespace {

constexpr uint32_t kFrameCount = 0x2F4368;  // counted up as 0x20F6A0 starts
constexpr uint32_t kWorldLevel = 0x5AEC;    // int8: the level, Levels\Level_%02u
constexpr uint32_t kWorldController = 0x520;
constexpr uint32_t kControllerYaw = 0x18;    // degrees, mouse.cpp
constexpr uint32_t kControllerPitch = 0x1C;
constexpr uint32_t kPlayerView = 0xD0;  // right, up, forward, eye
constexpr uint32_t kPlayerFeet = 0xA0;  // the body's position, the last row of its matrix
constexpr uint32_t kRowEye = 3;
constexpr uint32_t kRowForward = 2;
constexpr uint32_t kPlayerCrouch = 0x3A1;
constexpr uint32_t kGame = 0x2D45F0;
constexpr uint32_t kGameState = 0x21070;     // the game's current state
constexpr uint32_t kInLevelState = 0x20F78;  // the in-level state: current in play and in its pause menu
constexpr uint32_t kWorldUnit = 0x5AF0;
constexpr uint32_t kUnitSlots = 0x49D0, kUnitSlotSize = 0x880;  // +8 the unit held, +0x38 activated
constexpr uint32_t kWorldLastPos = 0x1C0;  // the player's position a frame ago (the triggers' sweep)
constexpr uint32_t kTransformView = 0;  // XDK transform types
constexpr uint32_t kTransformProjection = 1;

constexpr uint32_t kLiveFrames = 30;  // in play: the player update ran within as many frames
constexpr uint64_t kSendMs = 33;      // 30 a second whatever the frame rate
constexpr uint64_t kHelloMs = 500;
constexpr uint64_t kTimeoutMs = 5000;
constexpr uint64_t kEchoMs = 3000;  // the echo shows this copy's own player as it was this long ago
constexpr float kSmoothMs = 80;
constexpr float kSnap = 5;  // world units: farther in a step is a jump, not a move

constexpr uint32_t kMagic = 0x4F434B42;  // "BKCO"
constexpr uint16_t kProtocol = 18;
enum Type : uint8_t { kHello = 1, kWelcome, kState, kBye, kShots, kWorldEvents, kNpcSnap, kDiscover, kOffer, kMissionSync, kRoster, kFull };

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t protocol;
    uint8_t type, flags;
    uint32_t session;  // drawn at each start: a partner that started again counts its seq from 0
    uint32_t seq, ms;
};
struct Hello {
    Header h;
    char xbe_sha1[40];
};
struct Roster {
    Header h;
    uint32_t sessions[kMaxPlayers];
    uint8_t mask;
};
struct Offer {
    Hello hello;
    char name[64];
    uint8_t busy;
};
struct State {
    Header h;
    char nickname[kNicknameBytes];
    uint8_t in_level;
    int8_t level;
    uint8_t load;        // the sender's level loads (world::Load)
    int8_t load_level;   // and the level of its last
    uint8_t crouch;      // its player crouches (player+0x3A1, from its controller +0x40 at 0x1F09C1)
    uint8_t unit;         // its current unit (world+0x5AF0), in or out of play
    uint8_t next_active;  // and the next one activated there
    uint8_t waiting;      // the unit its advance waits for this copy to reach (world::WaitingUnit), 0 none
    uint8_t in_mission;   // its in-level state is the game's (game+0x21070): in play, paused, a hint, a cinematic
    uint8_t mission_ready, difficulty, stage, joining;
    uint8_t down;  // its player is dead, waiting for the team's checkpoint (coop_life)
    uint8_t weapon_variant, unlimited_ammo;
    uint32_t mission_token;
    uint16_t mission_ack;
    float view[4][3];  // right, up, forward, eye
    float feet[3];     // the body, player+0xA0: where the triggers test the player
    float move[3], speed;  // horizontal locomotion measured before network transport
    float yaw, pitch;
    uint32_t shot_ack;   // the last of the partner's shots taken
    uint32_t world_ack;  // and of its world events
};
// the shots (or world events) not yet acknowledged, each frame until they are: one event after another
struct Shots {
    Header h;
    uint32_t ack;
    uint8_t count;
    uint8_t pad[3];
};
struct MissionChunk {
    Header h;
    uint32_t token, total_bytes;
    uint16_t index, bytes;
    uint8_t host_load, receiver_load;
};
#pragma pack(pop)
constexpr int kMaxDatagram = 1170; // also fits the EOS P2P transport
union Packet {
    Header h;
    Hello hello;
    State state;
    Shots shots;
    uint8_t raw[1500];
};

struct Config {
    Role role = Role::Off;
    std::string host;
    uint16_t port = 45410;
    bool echo = false;
    bool probe = false;
    bool online = false;
} g_cfg;

std::atomic<bool> g_net{false};
std::atomic<bool> g_command{false}, g_searching{false};
std::mutex g_ui_mutex;
Session g_ui;
std::string g_nickname = "Player", g_pending_nickname;
enum class Command { None, Start, Stop, Search };
struct Request {
    Command command = Command::None;
    Role role = Role::Off;
    std::string host;
    uint16_t port = 45410;
    bool online = false;
} g_request;
int g_world_stage = 6;
uint64_t g_search_until = 0, g_search_tx = 0;
uint16_t g_search_port = 45410;
bool g_sim_seen = false;
uint32_t g_session = 0, g_seq = 0, g_sim_frame = 0;
int g_local_player = 15;
thread_local int g_selected_partner = -1;
uint64_t g_roster_at = 0;
std::string g_reject;
std::atomic<bool> g_rescue{false};
uint32_t g_rescues = 0;
State g_local{};
struct Motion {
    uint64_t at = 0;
    uint8_t load = 0;
    float feet[3]{}, direction[3] = {0, 0, 1}, speed = 0;
} g_motion;
std::atomic<bool> g_joining{false};
struct Follow {
    int host_load = -1, attached_load = -1;
    uint8_t local_load = 0;
    bool loading = false, placing = false, synchronizing = false;
    uint32_t rescues = 0;
    uint64_t requested = 0;
} g_follow;
constexpr size_t kMissionChunkBytes = 1100, kMissionMaxBytes = 256 * 1024;
struct MissionTransfer {
    std::vector<uint8_t> data;
    uint32_t token = 0;
    uint16_t ack = 0;
    uint8_t host_load = 0, receiver_load = 0;
};

bool MissionReady();

// the partner's walk as it came, for the triggers: each state of its in a level, oldest dropped
struct PathPoint {
    float feet[3];
    int8_t level;
    uint8_t reach;  // the farthest unit the partner could stand in then: its current one or the one it waits for
};
constexpr uint64_t kPathSize = 256;
struct Participant {
    std::string nickname = "Player";
    bool have = false, welcomed = false;
    net::Peer endpoint{};
    uint32_t session = 0, rx_seq = 0;
    uint64_t last_rx = 0, last_tx = 0, last_hello = 0, state_at = 0;
    State remote{}, playing{};
    std::array<PathPoint, kPathSize> path{};
    uint64_t path_count = 0;
    MissionTransfer mission_out{}, mission_in{};
};
std::array<Participant, kMaxPlayers> g_players;
Participant& CurrentParticipant() { return g_players[CurrentPartner()]; }
#define g_have_peer (coop::CurrentParticipant().have)
#define g_welcomed (coop::CurrentParticipant().welcomed)
#define g_peer (coop::CurrentParticipant().endpoint)
#define g_peer_session (coop::CurrentParticipant().session)
#define g_rx_seq (coop::CurrentParticipant().rx_seq)
#define g_last_rx (coop::CurrentParticipant().last_rx)
#define g_last_tx (coop::CurrentParticipant().last_tx)
#define g_last_hello (coop::CurrentParticipant().last_hello)
#define g_remote (coop::CurrentParticipant().remote)
#define g_remote_play (coop::CurrentParticipant().playing)
#define g_path (coop::CurrentParticipant().path)
#define g_path_count (coop::CurrentParticipant().path_count)
#define g_mission_in (coop::CurrentParticipant().mission_in)
#define g_mission_out (coop::CurrentParticipant().mission_out)

void ReceiveMission(const uint8_t* data, int bytes) {
    if (g_cfg.role != Role::Join || bytes < int(sizeof(MissionChunk))) return;
    MissionChunk chunk;
    std::memcpy(&chunk, data, sizeof chunk);
    if (!chunk.token || chunk.total_bytes > kMissionMaxBytes || !chunk.total_bytes || !chunk.bytes ||
        chunk.bytes > kMissionChunkBytes || bytes != int(sizeof chunk + chunk.bytes) ||
        chunk.host_load != g_remote.load || chunk.host_load != g_follow.host_load ||
        chunk.receiver_load != world::Load() || !MissionReady()) return;
    size_t offset = size_t(chunk.index) * kMissionChunkBytes;
    if (offset >= chunk.total_bytes || chunk.bytes != std::min(kMissionChunkBytes, chunk.total_bytes - offset)) return;
    if (chunk.token != g_mission_in.token) {
        if (!g_follow.synchronizing || !g_joining || chunk.index) return;
        g_mission_in = {{}, chunk.token, 0, chunk.host_load, chunk.receiver_load};
        g_mission_in.data.reserve(chunk.total_bytes);
    }
    if (chunk.index != g_mission_in.ack || !g_follow.synchronizing) return;
    g_mission_in.data.insert(g_mission_in.data.end(), data + sizeof chunk, data + bytes);
    if (g_mission_in.data.size() == chunk.total_bytes) {
        if (!world::RestoreMissionSnapshot(g_mission_in.data.data(), g_mission_in.data.size())) {
            LOGR("coop: rejected host mission progress");
            g_mission_in = {};
            return;
        }
        g_follow.attached_load = g_remote.load;
        g_follow.local_load = world::Load();
        g_follow.synchronizing = false;
        g_joining = false;
        LOGR("coop: joined host mission %d difficulty %u unit %u", int(g_remote.level), g_remote.difficulty, g_remote.unit);
    }
    g_mission_in.ack++;
}

struct Echo {
    uint64_t at;
    State state;
};
std::deque<Echo> g_echo;
struct Camera {
    float vp[16];
    uint32_t frame = ~0u;
} g_cam;

uint64_t NowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

Header Make(Type t, uint64_t now) {
    return {kMagic, kProtocol, t, uint8_t((CurrentPartner() << 4) | g_local_player),
            g_session, ++g_seq, uint32_t(now)};
}

bool SendTo(int player, const void* data, int bytes) {
    if (player < 0 || player >= kMaxPlayers || player == g_local_player || !g_players[player].have) return false;
    // Every logical pair has its own channel; only the host needs physical
    // connections to everyone. Clients reach other clients through this router.
    const auto& endpoint = g_cfg.role == Role::Host ? g_players[player].endpoint : g_players[0].endpoint;
    return net::Send(endpoint, data, bytes);
}
void SendCurrent(const void* data, int bytes) { SendTo(CurrentPartner(), data, bytes); }

void SendRoster(uint64_t now) {
    if (g_cfg.role != Role::Host) return;
    Roster r{};
    r.sessions[0] = g_session;
    r.mask = 1;
    for (int i = 1; i < kMaxPlayers; i++) if (g_players[i].have) {
        r.mask |= uint8_t(1u << i);
        r.sessions[i] = g_players[i].session;
    }
    for (int i = 1; i < kMaxPlayers; i++) if (g_players[i].have) {
        PartnerScope scope(i);
        r.h = Make(kRoster, now);
        SendCurrent(&r, sizeof r);
    }
}

uint32_t Frame() { return R32(kFrameCount); }

// in play: the world steps and its player with it
bool Live() { return g_sim_seen && Frame() - g_sim_frame < kLiveFrames; }

bool CampaignLevel(int level) {
    return level >= 0 && level <= 8 && level != 2;
}

bool MissionReady() {
    uint32_t game = R32(kGame), world = R32(kWorld);
    if (!game || !world || R32(game + kGameState) != game + kInLevelState || R32(game + 0x21084) ||
        R32(game + kInLevelState + 4) != 0x1C || R32(world + 0x5AE0) != 0x1C || !R32(world + kWorldUnit)) return false;
    uint32_t sub = R32(game + kInLevelState + 8);
    return sub != game + 0x2103C && sub != game + 0x2104C;
}

bool Finite(const State& s) {
    for (auto& row : s.view)
        for (float f : row)
            if (!std::isfinite(f)) return false;
    for (float f : s.feet)
        if (!std::isfinite(f)) return false;
    for (float f : s.move)
        if (!std::isfinite(f)) return false;
    return std::isfinite(s.yaw) && std::isfinite(s.pitch) && std::isfinite(s.speed) && s.speed >= 0 && s.speed <= 15.0f;
}

void SendHello(uint64_t now) {
    Hello h{Make(kHello, now), {}};
    std::memcpy(h.xbe_sha1, kImageSha1, sizeof h.xbe_sha1);
    SendCurrent(&h, sizeof h);
}

void Sample(State& s, uint64_t now) {
    s = {};
    s.h = Make(kState, now);
    std::snprintf(s.nickname, sizeof s.nickname, "%s", g_nickname.c_str());
    s.shot_ack = shots::Taken();
    s.world_ack = world::Taken();
    s.load = world::Load();
    s.load_level = world::LoadLevel();
    uint32_t world = R32(kWorld);
    s.mission_ready = MissionReady();
    s.joining = g_joining ? (g_follow.synchronizing ? 2 : 1) : 0;
    s.mission_token = g_mission_in.token;
    s.mission_ack = g_mission_in.ack;
    s.in_level = s.mission_ready && Live();
    if (world) {
        s.unit = uint8_t(R32(world + kWorldUnit));
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t slot = world + kUnitSlots + k * kUnitSlotSize;
            if (R32(slot + 8) == s.unit + 1u && G<uint8_t>(slot + 0x38)) s.next_active = 1;
        }
        s.waiting = world::WaitingUnit();
        uint32_t game = R32(kGame);
        s.in_mission = game && R32(game + kGameState) == game + kInLevelState;
        s.difficulty = game ? uint8_t(R32(game + 0x2014C)) : 0;
        s.stage = G<uint8_t>(world + 0x5AED);
        s.weapon_variant = G<uint8_t>(world + 0x5CB0);
        s.unlimited_ammo = G<uint8_t>(world + 0x5CB1);
        s.down = life::Down();
        uint32_t stream = R32(world + 0x5AE8);  // an advance under way (1, then 3 at 0x1F1143): its unit is the next one
        if (stream == 1 || stream == 3) s.waiting = std::max<uint8_t>(s.waiting, uint8_t(s.unit + 1));
    }
    if (!s.mission_ready) { g_motion = {}; return; }
    uint32_t player = world + kWorldPlayer;
    s.level = G<int8_t>(world + kWorldLevel);
    for (uint32_t r = 0; r < 4; r++) std::memcpy(s.view[r], HostPtr(player + kPlayerView + r * 16), 12);
    std::memcpy(s.feet, HostPtr(player + kPlayerFeet), 12);
    s.yaw = G<float>(world + kWorldController + kControllerYaw);
    s.pitch = G<float>(world + kWorldController + kControllerPitch);
    s.crouch = G<uint8_t>(player + kPlayerCrouch);
    // Sample() runs for each logical recipient at the same timestamp. Measure
    // once, then send everyone the same speed rather than differentiating the
    // receiver's bursty packet arrivals (which makes online bodies slide).
    if (!s.in_level || g_motion.load != s.load) g_motion = {};
    if (now != g_motion.at) {
        float dx = s.feet[0] - g_motion.feet[0], dz = s.feet[2] - g_motion.feet[2];
        float distance = std::sqrt(dx * dx + dz * dz);
        uint64_t elapsed = now - g_motion.at;
        g_motion.speed = g_motion.at && elapsed <= 250 && distance < kSnap && s.in_level
                             ? std::min(distance * 1000.0f / float(elapsed), 6.5f) : 0;
        if (distance > 0.001f && g_motion.speed > 0) {
            g_motion.direction[0] = dx / distance;
            g_motion.direction[1] = 0;
            g_motion.direction[2] = dz / distance;
        }
        std::memcpy(g_motion.feet, s.feet, sizeof s.feet);
        g_motion.at = now;
        g_motion.load = s.load;
    }
    std::memcpy(s.move, g_motion.direction, sizeof s.move);
    s.speed = g_motion.speed;
    if (!Finite(s)) s.in_level = 0;
}

void Receive(uint64_t now) {
    Packet in;
    net::Peer from;
    for (int i = 0, n; i < 256 && (n = net::Receive(from, &in, sizeof in)) >= 0; i++) {
        if (n < int(sizeof(Header)) || in.h.magic != kMagic || in.h.protocol != kProtocol) continue;
        if (in.h.type == kDiscover) {
            if (!g_net || g_cfg.role != Role::Host || n != sizeof(Hello) ||
                std::memcmp(in.hello.xbe_sha1, kImageSha1, 40)) continue;
            Offer offer{};
            offer.hello.h = Make(kOffer, now);
            std::memcpy(offer.hello.xbe_sha1, kImageSha1, 40);
            std::snprintf(offer.name, sizeof offer.name, "%s", net::HostName().c_str());
            offer.busy = PlayerCount() >= kMaxPlayers;
            net::Send(from, &offer, sizeof offer);
            continue;
        }
        if (in.h.type == kOffer) {
            if (!g_searching || n != sizeof(Offer) || std::memcmp(in.hello.xbe_sha1, kImageSha1, 40)) continue;
            Offer offer;
            std::memcpy(&offer, in.raw, sizeof offer);
            offer.name[sizeof offer.name - 1] = 0;
            for (char& c : offer.name) if (c && (uint8_t(c) < 32 || uint8_t(c) > 126)) c = '?';
            std::string address = net::Text(from);
            address.resize(address.find(':'));
            std::lock_guard lock(g_ui_mutex);
            auto it = std::find_if(g_ui.servers.begin(), g_ui.servers.end(), [&](const Server& s) {
                return s.session == offer.hello.h.session && s.port == from.port;
            });
            Server server{address, offer.name, from.port, offer.busy != 0, offer.hello.h.session};
            // One host can answer through several adapters. Prefer loopback for
            // copies on this PC, and show a single row for the session.
            if (it != g_ui.servers.end()) {
                if (it->address == "127.0.0.1") server.address = it->address;
                *it = server;
            }
            else if (g_ui.servers.size() < 32) g_ui.servers.push_back(server);
            continue;
        }
        if (!g_net) continue;
        int source = in.h.flags & 15, destination = in.h.flags >> 4;
        if (g_cfg.role == Role::Host) {
            int slot = -1;
            for (int i = 1; i < kMaxPlayers; i++)
                if (g_players[i].have && g_players[i].endpoint == from) { slot = i; break; }
            if (slot < 0) {
                if (in.h.type != kHello || n != int(sizeof(Hello)) || destination != 0 ||
                    std::memcmp(in.hello.xbe_sha1, kImageSha1, 40)) continue;
                for (int i = 1; i < kMaxPlayers; i++) if (!g_players[i].have) { slot = i; break; }
                if (slot < 0) {
                    Header full = Make(kFull, now);
                    full.flags = 0xF0;
                    net::Send(from, &full, sizeof full);
                    continue;
                }
                g_players[slot] = {};
                g_players[slot].endpoint = from;
                g_players[slot].have = true;
            }
            if (source != slot && !(in.h.type == kHello && source == 15)) continue;
            if (in.h.type == kHello && (n != sizeof(Hello) || std::memcmp(in.hello.xbe_sha1, kImageSha1, 40))) continue;
            source = slot;
            if (destination != 0) {
                if (destination >= kMaxPlayers || destination == source || !g_players[destination].have ||
                    in.h.session != g_players[source].session || in.h.type == kHello || in.h.type == kWelcome ||
                    in.h.type == kRoster || in.h.type == kDiscover || in.h.type == kOffer || in.h.type == kNpcSnap ||
                    in.h.type == kMissionSync) continue;
                g_players[source].last_rx = now;
                net::Send(g_players[destination].endpoint, &in, n);
                continue;
            }
        } else {
            if (!g_players[0].have || from != g_players[0].endpoint || source >= kMaxPlayers) continue;
            if (in.h.type == kWelcome && source == 0 && n == sizeof(Header) && destination > 0 && destination < kMaxPlayers)
                g_local_player = destination;
            if (destination != g_local_player || source == g_local_player || !g_players[source].have) continue;
            if (source && in.h.session != g_players[source].session) continue;
            if ((in.h.type == kRoster || in.h.type == kNpcSnap || in.h.type == kMissionSync) && source != 0) continue;
        }
        PartnerScope participant(source);
        if (in.h.session != g_peer_session) {  // the partner started again: a new sequence, nothing of the old one
            g_peer_session = in.h.session;
            g_rx_seq = 0;
            g_remote = {}, g_remote_play = {};
            g_path_count = 0;
            if (g_cfg.role == Role::Join && source == 0) {
                g_follow = {};
                g_joining = true;
            }
            g_mission_in = {}, g_mission_out = {};
            shots::Reset();
            world::Reset();
        }
        g_last_rx = now;
        switch (in.h.type) {
            case kHello: {
                Header w = Make(kWelcome, now);
                SendCurrent(&w, sizeof w);
                g_welcomed = true;
                g_roster_at = 0;
                break;
            }
            case kWelcome:
                if (!g_welcomed) LOGR("coop: joined %s", net::Text(g_peer).c_str());
                g_welcomed = true;
                break;
            case kFull:
                if (g_cfg.role == Role::Join && source == 0) g_reject = "This room already has eight players.";
                break;
            case kRoster: {
                if (n != sizeof(Roster) || g_cfg.role != Role::Join || source != 0 || !g_welcomed) break;
                Roster roster;
                std::memcpy(&roster, &in, sizeof roster);
                if (!(roster.mask & 1) || !(roster.mask & (1u << g_local_player)) ||
                    roster.sessions[g_local_player] != g_session) break;
                for (int i = 1; i < kMaxPlayers; i++) {
                    if (i == g_local_player) continue;
                    bool present = (roster.mask & (1u << i)) && roster.sessions[i];
                    if (g_players[i].have == present && (!present || g_players[i].session == roster.sessions[i])) continue;
                    PartnerScope other(i);
                    shots::Reset();
                    world::Reset();
                    g_players[i] = {};
                    g_players[i].have = present;
                    g_players[i].session = roster.sessions[i];
                    g_players[i].last_rx = now;
                    g_players[i].welcomed = present;
                }
                break;
            }
            case kState:
                if (n == int(sizeof(State)) && Finite(in.state) && int32_t(in.h.seq - g_rx_seq) > 0) {
                    g_remote = in.state;
                    CurrentParticipant().nickname = CleanNickname(std::string(in.state.nickname,
                        std::find(std::begin(in.state.nickname), std::end(in.state.nickname), '\0')));
                    CurrentParticipant().state_at = now;
                    if (g_remote.mission_ready) {
                        g_remote_play = g_remote;
                        g_remote_play.in_level = 1; // a usable pose even if first heard while paused
                    }
                    g_rx_seq = in.h.seq;
                    if (in.state.mission_ready) {
                        PathPoint& pp = g_path[g_path_count % kPathSize];
                        std::memcpy(pp.feet, in.state.feet, sizeof pp.feet);
                        pp.level = in.state.level;
                        pp.reach = std::max(in.state.unit, in.state.waiting);
                        g_path_count++;
                    }
                    shots::Acked(in.state.shot_ack);
                    world::Acked(in.state.world_ack);
                    world::PartnerLoad(in.state.load, in.state.load_level);
                }
                break;
            case kNpcSnap:
                if (n >= int(sizeof(Shots)))
                    npcs::OnSnapshot(in.raw + sizeof(Shots), n - int(sizeof(Shots)), in.shots.count,
                                     int8_t(in.shots.pad[0]), now);
                break;
            case kMissionSync:
                if (source == 0) ReceiveMission(in.raw, n);
                break;
            case kWorldEvents:
                if (n >= int(sizeof(Shots))) {
                    world::Acked(in.shots.ack);
                    world::PartnerLoad(in.shots.pad[0], int8_t(in.shots.pad[1]));
                    world::Incoming(in.raw + sizeof(Shots), n - int(sizeof(Shots)), in.shots.count, now);
                }
                break;
            case kShots:
                if (n >= int(sizeof(Shots))) {
                    // taken only while the world steps: paused or loading they would fire late
                    uint32_t world = R32(kWorld);
                    bool stepping = world && g_sim_seen && Frame() - g_sim_frame <= 2;
                    int8_t level = stepping ? G<int8_t>(world + kWorldLevel) : int8_t(-1);
                    shots::Acked(in.shots.ack);
                    shots::Incoming(in.raw + sizeof(Shots), n - int(sizeof(Shots)), in.shots.count, level, now);
                }
                break;
            case kBye:
                LOGR("coop: partner %s left", net::Text(g_peer).c_str());
                g_remote = {}, g_remote_play = {};
                g_welcomed = false;
                if (g_cfg.role == Role::Host) g_have_peer = false;
                break;
        }
    }
}

void SendOne(uint64_t now) {
    if (!g_have_peer) return;
    if (now - g_last_rx > kTimeoutMs) world::Hold(now);  // its events wait for the partner to be heard again
    if (g_cfg.role == Role::Join && CurrentPartner() == 0 && (!g_welcomed || now - g_last_rx > kTimeoutMs)) {
        if (now - g_last_hello >= kHelloMs) {
            g_welcomed = false;
            SendHello(now);
            g_last_hello = now;
        }
        return;
    }
    // shots every frame there are some the partner has not taken: a lost datagram is the next frame's
    Packet out;
    int bytes = 0;
    int n = shots::Outgoing(out.raw + sizeof(Shots), kMaxDatagram - int(sizeof(Shots)), bytes, now);
    if (n) {
        out.shots.h = Make(kShots, now);
        out.shots.ack = shots::Taken();
        out.shots.count = uint8_t(n);
        std::memset(out.shots.pad, 0, sizeof out.shots.pad);
        SendCurrent(out.raw, int(sizeof(Shots)) + bytes);
    }
    n = world::Outgoing(out.raw + sizeof(Shots), kMaxDatagram - int(sizeof(Shots)), bytes, now);
    if (n) {
        out.shots.h = Make(kWorldEvents, now);
        out.shots.ack = world::Taken();
        out.shots.count = uint8_t(n);
        std::memset(out.shots.pad, 0, sizeof out.shots.pad);
        out.shots.pad[0] = world::Load();
        out.shots.pad[1] = uint8_t(world::LoadLevel());
        SendCurrent(out.raw, int(sizeof(Shots)) + bytes);
    }
    if (now - g_last_tx < kSendMs) return;
    g_last_tx = now;
    State s;
    Sample(s, now);
    SendCurrent(&s, sizeof s);
    if (g_cfg.role == Role::Host && s.mission_ready && g_remote.mission_ready && g_remote.joining == 2 &&
        s.level == g_remote.level && s.difficulty == g_remote.difficulty && s.stage == g_remote.stage) {
        if (g_mission_out.data.empty() || g_mission_out.receiver_load != g_remote.load || g_mission_out.host_load != s.load) {
            g_mission_out = {world::MissionSnapshot(), std::random_device{}() | 1u, 0, s.load, g_remote.load};
            LOGR("coop: sending mission progress (%zu bytes) to load %u", g_mission_out.data.size(), g_remote.load);
        }
        uint16_t index = g_remote.mission_token == g_mission_out.token ? g_remote.mission_ack : 0;
        size_t offset = size_t(index) * kMissionChunkBytes;
        if (offset < g_mission_out.data.size()) {
            MissionChunk chunk{Make(kMissionSync, now), g_mission_out.token, uint32_t(g_mission_out.data.size()), index,
                               uint16_t(std::min(kMissionChunkBytes, g_mission_out.data.size() - offset)),
                               s.load, g_remote.load};
            std::memcpy(out.raw, &chunk, sizeof chunk);
            std::memcpy(out.raw + sizeof chunk, g_mission_out.data.data() + offset, chunk.bytes);
            SendCurrent(out.raw, int(sizeof chunk + chunk.bytes));
        }
    }
    // the host's living enemies, latest wins
    n = npcs::Snapshot(out.raw + sizeof(Shots), kMaxDatagram - int(sizeof(Shots)), bytes);
    uint32_t world = R32(kWorld);
    if (world && s.in_level) {  // a paused host sends none: its puppets stand
        out.shots.h = Make(kNpcSnap, now);
        out.shots.ack = 0;
        out.shots.count = uint8_t(n);
        std::memset(out.shots.pad, 0, sizeof out.shots.pad);
        out.shots.pad[0] = uint8_t(world ? G<int8_t>(world + kWorldLevel) : int8_t(-1));
        if (g_cfg.role == Role::Host) SendCurrent(out.raw, int(sizeof(Shots)) + bytes);
    }
}

void Send(uint64_t now) {
    if (g_cfg.role == Role::Host) {
        for (int i = 1; i < kMaxPlayers; i++) if (g_players[i].have && now - g_players[i].last_rx > kTimeoutMs) {
            PartnerScope scope(i);
            shots::Reset(); world::Reset();
            g_players[i] = {};
            g_roster_at = 0;
        }
        if (!g_roster_at || now - g_roster_at >= 500) {
            SendRoster(now);
            g_roster_at = now;
        }
    }
    for (int i = 0; i < kMaxPlayers; i++) if (i != g_local_player && g_players[i].have) {
        PartnerScope scope(i);
        SendOne(now);
    }
}

// the echo: this copy's own player of kEchoMs ago stands in for a partner
void Echo(uint64_t now) {
    if (g_echo.empty() || now - g_echo.back().at >= kSendMs) {
        State s;
        Sample(s, now);
        g_echo.push_back({now, s});
    }
    while (g_echo.size() > 1 && now - g_echo[1].at >= kEchoMs) g_echo.pop_front();
    if (now - g_echo.front().at >= kEchoMs) {
        g_remote = g_echo.front().state;
        if (g_remote.in_level) g_remote_play = g_remote;
        g_last_rx = now;
        CurrentParticipant().state_at = now;
    }
}

// world to the overlay's lines; false without this frame's scene camera
bool Project(const float w[3], Marker& m) {
    if (g_cam.frame != Frame()) return false;
    float c[4];
    for (int j = 0; j < 4; j++)
        c[j] = w[0] * g_cam.vp[j] + w[1] * g_cam.vp[4 + j] + w[2] * g_cam.vp[8 + j] + g_cam.vp[12 + j];
    float wide = game::Aspect() * 480;
    bool behind = c[3] < 0.05f;  // w is the depth in front of the camera
    float nx = behind ? (c[0] >= 0 ? 2.0f : -2.0f) : c[0] / c[3];
    float ny = behind ? 0.0f : c[1] / c[3];
    m.on_screen = !behind && std::fabs(nx) <= 1 && std::fabs(ny) <= 1;
    m.x = std::clamp((nx * 0.5f + 0.5f) * wide, 24.0f, wide - 24.0f);
    m.y = std::clamp((0.5f - ny * 0.5f) * 480.0f, 24.0f, 456.0f);
    return true;
}

const float* LocalEye() { return &G<float>(R32(kWorld) + kWorldPlayer + kPlayerView + kRowEye * 16); }

}  // namespace

int LocalPlayer() { return g_local_player; }
std::string CleanNickname(const std::string& name) {
    // The native HUD consumes UTF-16. Bound names by display characters and
    // retain complete UTF-8 characters on the wire, including accented names.
    wchar_t wide[kNicknameBytes]{};
    if (name.empty() || name.size() >= kNicknameBytes) return "Player";
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), int(name.size()), wide, kNicknameBytes);
    std::wstring clean;
    for (int i = 0; i < count && clean.size() < kNicknameCharacters; i++) {
        wchar_t c = wide[i];
        if (c >= 32 && !(c >= 127 && c <= 159) && !(c >= 0xD800 && c <= 0xDFFF) && c != 0x2028 && c != 0x2029)
            clean += c;
    }
    auto first = clean.find_first_not_of(L' '), last = clean.find_last_not_of(L' ');
    if (first == std::wstring::npos) return "Player";
    clean = clean.substr(first, last - first + 1);
    char utf8[kNicknameBytes]{};
    int bytes = WideCharToMultiByte(CP_UTF8, 0, clean.data(), int(clean.size()), utf8, kNicknameBytes - 1, nullptr, nullptr);
    return bytes ? std::string(utf8, bytes) : "Player";
}

void RequestNickname(const std::string& name) {
    std::string clean = CleanNickname(name);
    {
        std::lock_guard<std::recursive_mutex> ui(app::UiLock());
        game::Opt().coop_nickname = clean;
        app::Save();
    }
    std::lock_guard lock(g_ui_mutex);
    g_ui.nickname = clean;
    g_pending_nickname = clean;
}
bool Hosting() { return g_net && g_cfg.role == Role::Host; }
int CurrentPartner() {
    if (g_selected_partner >= 0) return g_selected_partner;
    if (g_cfg.role == Role::Join) return 0;
    for (int i = 1; i < kMaxPlayers; i++) if (g_players[i].have) return i;
    return 1;
}
PartnerScope::PartnerScope(int player) : previous(g_selected_partner) { g_selected_partner = std::clamp(player, 0, kMaxPlayers - 1); }
PartnerScope::~PartnerScope() { g_selected_partner = previous; }
bool Connected(int player) { return g_net && player >= 0 && player < kMaxPlayers && player != g_local_player && g_players[player].have; }
int PlayerCount() {
    int count = 1;
    for (int i = 0; i < kMaxPlayers; i++) if (i != g_local_player && g_players[i].have) count++;
    return count;
}
std::string Players() {
    std::string out = "players=" + std::to_string(PlayerCount()) + "/8 local=" + std::to_string(g_local_player) +
                      " nick=\"" + g_nickname + "\"\n";
    char line[200];
    for (int i = 0; i < kMaxPlayers; i++) if (Connected(i)) {
        PartnerScope scope(i);
        std::snprintf(line, sizeof line, "P%d nick=\"%s\" session=%u level=%d joining=%u body=%08X feet=%.2f,%.2f,%.2f\n",
                      i + 1, CurrentParticipant().nickname.c_str(), g_peer_session, int(g_remote.level), g_remote.joining, npcs::Body(),
                      double(g_remote.feet[0]), double(g_remote.feet[1]), double(g_remote.feet[2]));
        out += line;
    }
    return out;
}

bool Active() { return g_net || g_command || g_searching || online::Pumping() || g_cfg.echo || g_cfg.probe || control::Enabled(); }

Session GetSession() {
    std::lock_guard lock(g_ui_mutex);
    Session s = g_ui;
    s.pending = g_command;
    s.searching = g_searching;
    return s;
}

void RequestStart(Role role, const std::string& host, uint16_t port) {
    std::lock_guard lock(g_ui_mutex);
    g_request = {Command::Start, role, host, port};
    g_command = true;
}
void RequestOnline(Role role, const std::string& code) {
    std::lock_guard lock(g_ui_mutex);
    g_request = {Command::Start, role, code, 45410, true};
    g_command = true;
}
void RequestStop() {
    std::lock_guard lock(g_ui_mutex);
    g_request.command = Command::Stop;
    g_command = true;
}
void RequestSearch(uint16_t port) {
    std::lock_guard lock(g_ui_mutex);
    g_request = {Command::Search, Role::Off, {}, port};
    g_command = true;
}

bool InPlay() { return Live(); }

int PartnerPath(uint64_t& after, float (*out)[3], int max, bool& gap) {
    uint64_t first = g_path_count > kPathSize ? g_path_count - kPathSize : 0;
    gap = after < first;
    if (gap) after = first;
    uint32_t world = R32(kWorld);
    int8_t level = world ? G<int8_t>(world + kWorldLevel) : int8_t(-1);
    int n = 0;
    while (after < g_path_count && n < max) {
        const PathPoint& p = g_path[after % kPathSize];
        after++;
        if (p.level == level) std::memcpy(out[n++], p.feet, 12);
    }
    return n;
}

uint64_t PartnerPathEnd() { return g_path_count; }

const float* PartnerForward() { return g_remote_play.view[kRowForward]; }
bool PartnerView(float out[4][3]) {
    if (!PartnerInLevel()) return false;
    const State& s = g_remote.in_level ? g_remote : g_remote_play;  // paused: its last pose in play
    if (!s.in_level) return false;
    std::memcpy(out, s.view, sizeof s.view);
    return true;
}
std::string Nickname(int player) {
    if (player == g_local_player) return g_nickname;
    return player >= 0 && player < kMaxPlayers ? g_players[player].nickname : std::string();
}
bool PartnerCrouch() { return g_remote.in_level && g_remote.crouch; }
void PartnerMotion(float direction[3], float& speed) {
    std::memcpy(direction, g_remote.move, sizeof g_remote.move);
    speed = g_remote.in_level && NowMs() - CurrentParticipant().state_at <= 500 ? g_remote.speed : 0;
}

bool PartnerFeet(float out[3]) {
    uint32_t world = R32(kWorld);
    if (!g_net || !world || !Live() || !g_remote.in_level || g_remote.joining || NowMs() - g_last_rx > kTimeoutMs) return false;
    if (g_remote.level != G<int8_t>(world + kWorldLevel)) return false;
    std::memcpy(out, g_remote.feet, 12);
    return true;
}
bool PartnerInLevel() {
    uint32_t world = R32(kWorld);
    uint64_t now = NowMs();
    return g_net && world && !g_remote.joining && now - g_last_rx <= kTimeoutMs && g_remote.load_level == G<int8_t>(world + kWorldLevel) &&
           (g_remote.in_level || g_remote.in_mission);
}
bool PartnerPaused() { return PartnerInLevel() && !g_remote.in_level; }
bool PartnerDown() { return PartnerInLevel() && g_remote.down; }
bool PartnerAt(float out[3]) {
    uint32_t world = R32(kWorld);
    if (!g_net || !world || !g_remote.mission_ready || g_remote.joining || NowMs() - g_last_rx > kTimeoutMs) return false;
    if (g_remote.level != G<int8_t>(world + kWorldLevel)) return false;
    std::memcpy(out, g_remote.feet, 12);
    return true;
}
int PartnerUnit(int* next_active, int* waiting) {
    if (!PartnerInLevel()) return -1;
    if (next_active) *next_active = g_remote.next_active;
    if (waiting) *waiting = g_remote.waiting;
    return g_remote.unit;
}
bool HostHeard() { return g_net && g_cfg.role == Role::Join && g_players[0].welcomed && NowMs() - g_players[0].last_rx <= kTimeoutMs; }
bool Networked() { return g_net; }
bool JoiningMission() { return g_joining; }

bool LoadMission(X86Context& ctx, int level, int difficulty, int stage) {
    uint32_t game = R32(kGame);
    if (!game || !R32(game + kGameState) || R32(game + 0x21084) || !CampaignLevel(level) ||
        difficulty < 0 || difficulty > 3 || stage != 1) return false;
    // not before the game's strings are in: its pages' texts are set from them on the way (0x11A4A0 with a null
    // text read through it, a crash of a copy that joins a host in a mission as it boots)
    uint32_t strings = R32(0x2D45D8);
    if (!strings) return false;
    uint32_t saved_ecx = ctx.ecx;
    ctx.ecx = strings;
    uint32_t text = CallGuest(ctx, 0x1F3A80, {0x4A8272D7u});  // FE_QUIT's text, 0 before they load
    ctx.ecx = saved_ecx;
    if (!text) return false;
    if (!R32(game + 0x21060)) {
        // F10 is available even on Press Start. That page normally binds an
        // active controller via 0x11A750 before a mission can run. Bind port 0
        // (also the keyboard) to its already initialized input record.
        uint32_t input = R32(0x2D45E8);
        if (!input || !R32(input + 0x518 + 0x0C)) return false;
        W32(game + 0x21060, input + 0x518);
        G<uint8_t>(input + 0x518 + 8) = 1;
        W32(0x2F3BD4, game + 0x20174);
    }
    W32(game + 0x2014C, uint32_t(difficulty));
    G<uint8_t>(game + 0x2020C) = uint8_t(level);
    G<uint8_t>(game + 0x2020D) = 1;
    G<uint8_t>(game + 0x2020E) = uint8_t(stage);
    G<uint8_t>(0x2F3BF8) = 0;
    W32(0x2EC464, 0);
    W32(0x2A7EDC, ~0u);
    // Use the game's asynchronous fade/unload/load transition, including from
    // the front end. Never call the world loader on a still-live world.
    hle::Call(ctx, 0x20DCA0, hle::Make({hle::edi, hle::stack}), {game, game + kInLevelState});
    LOGR("coop: loading host mission %d difficulty %d stage %d", level, difficulty, stage);
    return true;
}

// the pages after a mission's end where its progress is saved to the profile (made there if there is none): the
// results, the unlocks, the memory card's messages and slots, the profile's name
std::atomic<bool> g_save_wait{false};
bool SavingPage() {
    uint32_t ui = R32(layout::kUi);
    if (!ui) return false;
    const char* name = static_cast<const char*>(HostPtr(ui + layout::kPageName));
    char page[49] = {};
    for (int i = 0; i < 48 && name[i]; i++) page[i] = name[i];
    return std::strncmp(page, "Results", 7) == 0 || std::strcmp(page, "Unlocks") == 0 ||
           std::strncmp(page, "memcard", 7) == 0 || std::strcmp(page, "VirtualKeyboard") == 0;
}

void MissionTick(X86Context& ctx) {
    PartnerScope host(0);
    if (!HostHeard() || !g_remote.mission_ready || !CampaignLevel(g_remote.level) ||
        g_remote.difficulty > 3 || g_remote.stage != 1 || g_remote.weapon_variant > 1 || g_remote.unlimited_ammo > 1) return;
    uint32_t game = R32(kGame), world = R32(kWorld);
    if (!game || !world) return;
    bool ready = MissionReady();
    bool matches = ready && G<int8_t>(world + kWorldLevel) == g_remote.level &&
                   R32(game + 0x2014C) == g_remote.difficulty && G<uint8_t>(world + 0x5AED) == g_remote.stage &&
                   G<uint8_t>(world + 0x5CB0) == g_remote.weapon_variant && G<uint8_t>(world + 0x5CB1) == g_remote.unlimited_ammo;
    if (g_follow.attached_load == g_remote.load && matches && !g_joining) return;
    // A result page belongs to this attempt until the host starts another one.
    if (g_follow.attached_load == g_remote.load && !g_joining && !ready &&
        R32(game + kGameState) == game + kInLevelState) return;
    // the host on to its next mission: this copy first through its own end's pages, where the progress is saved
    bool saving = !g_joining && SavingPage();
    if (saving != g_save_wait) LOGR("coop: %s", saving ? "the host goes on: saving here first" : "saved: following the host");
    g_save_wait = saving;
    if (saving) return;
    if (!g_joining) g_follow.host_load = -1;
    g_joining = true;
    uint64_t now = NowMs();
    if (g_follow.host_load != g_remote.load) {
        g_follow.host_load = g_remote.load;
        g_follow.loading = g_follow.placing = g_follow.synchronizing = false;
        g_mission_in = {};
    }
    if (!g_follow.loading) {
        // Existing retry replication may finish loading after the host's first
        // new state arrives. Reuse that load instead of starting a second one.
        if (g_follow.attached_load >= 0 && world::Load() != g_follow.local_load && matches) {
            g_follow.loading = true;
        } else {
            if (!LoadMission(ctx, g_remote.level, g_remote.difficulty, g_remote.stage)) return;
            g_follow.loading = true;
            g_follow.local_load = world::Load();
            g_follow.requested = now;
            return;
        }
    }
    if (!matches || world::Load() == g_follow.local_load) return;
    if (g_follow.synchronizing) return;
    if (!g_follow.placing) {
        g_follow.placing = true;
        g_follow.rescues = Rescues();
        world::AskRescue();
        g_follow.requested = now;
    }
    if (Rescues() != g_follow.rescues) {
        g_follow.synchronizing = true;
    } else if (now - g_follow.requested > 16000) {
        // A streamed unit can be temporarily unavailable; allow another rescue.
        g_follow.placing = false;
    }
}

void AskRescue() {
    if (g_net) g_rescue = true;
}

void PadRescue(int port, uint16_t buttons) {
    static uint64_t since[4];
    static bool done[4];
    if (port < 0 || port > 3) return;
    constexpr uint16_t kSticks = 0xC0;  // left and right thumb clicks
    if ((buttons & kSticks) != kSticks) {
        since[port] = 0, done[port] = false;
        return;
    }
    uint64_t now = NowMs();
    if (!since[port]) since[port] = now;
    else if (!done[port] && now - since[port] >= 1000) {
        done[port] = true;
        AskRescue();
    }
}

uint32_t Rescues() { return g_rescues; }

namespace {

// a place the partner stood a moment ago, a step or more from it: walked on, not in a wall, in a unit this copy has
// (the partner's reach then at least the unit)
bool TrailPoint(const float partner[3], int8_t level, uint8_t unit, float out[3]) {
    constexpr float kNear = 1.2f, kFar = 6.0f;
    uint64_t first = g_path_count > kPathSize ? g_path_count - kPathSize : 0;
    for (uint64_t k = g_path_count; k-- > first;) {
        const PathPoint& p = g_path[k % kPathSize];
        if (p.level != level || p.reach < unit) break;
        float d[3] = {p.feet[0] - partner[0], p.feet[1] - partner[1], p.feet[2] - partner[2]};
        float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist > kFar) break;
        if (dist >= kNear) {
            std::memcpy(out, p.feet, 12);
            return true;
        }
    }
    return false;
}

}  // namespace

// this copy's player put next to the partner: the world's units are the partner's already (world::FlowTick)
bool PlaceByPartner(uint8_t unit) {
    uint32_t world = R32(kWorld);
    if (!world) return false;
    int8_t level = G<int8_t>(world + kWorldLevel);
    float partner[3];
    bool at = PartnerAt(partner);
    if (!at && PartnerInLevel() && g_path_count) {  // stopped (its world waits): where it was last
        const PathPoint& p = g_path[(g_path_count - 1) % kPathSize];
        if (p.level == level) std::memcpy(partner, p.feet, 12), at = true;
    }
    if (!at) {
        LOGR("coop: get me out of here: the partner is not in this level");
        return false;
    }
    float to[3];
    if (PlayerCount() > 2 || !TrailPoint(partner, level, unit, to)) {
        // standing still a while: beside it, across its view (not back over a trigger it came through)
        const float* f = g_remote_play.view[kRowForward];
        float h = std::sqrt(f[0] * f[0] + f[2] * f[2]);
        std::memcpy(to, partner, 12);
        if (PlayerCount() > 2) {
            // Give simultaneous joins distinct places instead of stacking seven
            // solid bodies on the same point and obstructing movement/fire.
            float angle = float(std::max(0, LocalPlayer() - 1)) * 6.2831853f / float(kMaxPlayers - 1);
            to[0] += std::cos(angle) * 1.6f;
            to[2] += std::sin(angle) * 1.6f;
        } else if (h > 0.01f) to[0] += f[2] / h * 0.8f, to[2] -= f[0] / h * 0.8f;
    }
    uint32_t player = world + kWorldPlayer;
    float* feet = &G<float>(player + kPlayerFeet);
    float* last = &G<float>(world + kWorldLastPos);
    for (int i = 0; i < 3; i++) feet[i] = to[i], last[i] = to[i];
    // facing the partner
    float d[2] = {partner[0] - to[0], partner[2] - to[2]};
    if (d[0] * d[0] + d[1] * d[1] > 0.01f)
        G<float>(world + kWorldController + kControllerYaw) = std::atan2(d[0], d[1]) * 57.2957795f;
    g_rescues++;
    LOGR("coop: get me out of here: put at %.1f %.1f %.1f, the partner at %.1f %.1f %.1f", double(to[0]), double(to[1]),
         double(to[2]), double(partner[0]), double(partner[1]), double(partner[2]));
    return true;
}

namespace {

void PublishSession(const std::string& error = {}) {
    std::lock_guard lock(g_ui_mutex);
    g_ui.role = g_net ? g_cfg.role : Role::Off;
    g_ui.running = g_net;
    g_ui.online = g_net && g_cfg.online;
    g_ui.online_available = online::Built();
    g_ui.room_code = g_ui.online ? online::Code() : std::string{};
    g_ui.players = g_net ? PlayerCount() : 0;
    g_ui.members.clear();
    if (g_net) for (int i = 0; i < kMaxPlayers; i++) {
        if (i == g_local_player) g_ui.members.push_back({i, g_nickname, true, i == 0});
        else if (Connected(i)) g_ui.members.push_back({i, g_players[i].nickname, false, i == 0});
    }
    g_ui.port = g_cfg.port;
    g_ui.status = Status();
    if (!error.empty()) g_ui.error = error;
}

void StartSession() {
    if (g_cfg.role == Role::Off) return;
    g_local_player = g_cfg.role == Role::Host ? 0 : 15;
    if (g_cfg.online) {
        if (!online::Start(g_cfg.role == Role::Host, g_cfg.host, "black-coop-" + std::to_string(kProtocol) + "-" + kImageSha1)) {
            PublishSession(online::Error());
            return;
        }
    } else if (!net::Open(g_cfg.role == Role::Host ? g_cfg.port : 0)) {
        PublishSession("Could not open UDP port " + std::to_string(g_cfg.port) + ". Check whether it is already in use.");
        return;
    }
    if (g_cfg.role == Role::Join && !g_cfg.online) {
        g_peer = {net::Resolve(g_cfg.host), g_cfg.port};
        if (!g_peer.addr) {
            net::Close();
            PublishSession("Host address not found: " + g_cfg.host);
            return;
        }
        g_have_peer = true;
    }
    g_cfg.echo = false;
    g_net = true;
    g_follow = {};
    g_mission_in = {}, g_mission_out = {};
    g_joining = g_cfg.role == Role::Join;
    shots::SetMode(true, false);
    world::SetMode(true, g_world_stage);
    npcs::SetMode(g_cfg.role == Role::Host && g_world_stage >= 4, g_cfg.role == Role::Join && g_world_stage >= 4,
                  g_world_stage >= 5);
    LOGR("coop: world sync stage %d", g_world_stage);
    if (g_cfg.online) LOGR("coop: starting online %s", g_cfg.role == Role::Host ? "host" : "join");
    else if (g_cfg.role == Role::Host) LOGR("coop: host on UDP %u", g_cfg.port);
    else LOGR("coop: joining %s", net::Text(g_peer).c_str());
    PublishSession();
}

void StopSession(X86Context& ctx) {
    if (g_net) for (int i = 0; i < kMaxPlayers; i++) if (i != g_local_player && g_players[i].have) {
        PartnerScope scope(i);
        Header bye = Make(kBye, NowMs());
        SendCurrent(&bye, sizeof bye);
    }
    npcs::Stop(ctx);
    world::Stop(ctx);
    shots::Reset();
    shots::SetMode(false, false);
    net::Close();
    g_net = false;
    g_searching = false;
    g_cfg.role = Role::Off;
    g_cfg.online = false;
    g_cfg.echo = false;
    g_players = {};
    g_local_player = 15;
    g_roster_at = 0;
    for (int i = 0; i < kMaxPlayers; i++) { PartnerScope scope(i); shots::Reset(); world::Reset(); }
    g_have_peer = g_welcomed = false;
    g_peer = {};
    g_remote = {}, g_remote_play = {};
    g_peer_session = g_seq = g_rx_seq = 0;
    g_last_rx = g_last_tx = g_last_hello = 0;
    g_path_count = 0;
    g_echo.clear();
    g_rescue = false;
    g_joining = false;
    g_follow = {};
    g_mission_in = {}, g_mission_out = {};
    LOGR("coop: session stopped");
}

}  // namespace

void Init() {
    g_session = std::random_device{}() | 1;  // never 0, the value before any partner
    const game::Options& o = game::Opt();
    g_nickname = CleanNickname(o.coop_nickname);
    g_ui.nickname = g_nickname;
    // Saved connection preferences are not a request to reconnect on launch.
    // A session starts from F10, or an explicit per-process --coop argument.
    g_cfg.role = Role::Off;
    g_cfg.port = uint16_t(o.coop_port && o.coop_port <= 65535 ? o.coop_port : 45410);
    {
        std::lock_guard<std::recursive_mutex> ui(app::UiLock());
        g_cfg.host = app::Get().join_ip;
    }
    // per process: copies on one PC share the settings file
    std::string v;
    if (net::Arg("--coop", v)) g_cfg.role = v == "host" ? Role::Host : v == "join" ? Role::Join : Role::Off;
    if (net::Arg("--join", v) && !v.empty()) g_cfg.host = v;
    if (net::Arg("--coop-online", v)) {
        g_cfg.online = true;
        g_cfg.role = v == "host" ? Role::Host : Role::Join;
        g_cfg.host = v == "host" ? std::string{} : v;
    }
    if (net::Arg("--coop-port", v) && std::atoi(v.c_str()) > 0 && std::atoi(v.c_str()) <= 65535)
        g_cfg.port = uint16_t(std::atoi(v.c_str()));
    g_cfg.echo = net::Arg("--coop-echo", v);
    g_cfg.probe = net::Arg("--coop-probe", v);
    if (g_cfg.echo || g_cfg.probe) LOGR("coop:%s%s", g_cfg.echo ? " echo" : "", g_cfg.probe ? " probe" : "");
    if (net::Arg("--coop-control", v)) {
        int port = std::atoi(v.c_str());
        control::Init(uint16_t(port > 0 ? port : 45500 + app::Instance()));
    }
    shots::SetMode(false, g_cfg.echo);
    world::SetMode(false, 0);
    // --coop-world 5: no level flow; 4: no partner's body; 3: no host enemies; 2: deaths and triggers; 1: deaths
    // only; 0: no world sync
    if (net::Arg("--coop-world", v) && !v.empty()) g_world_stage = std::atoi(v.c_str());
    StartSession();
    PublishSession();
}

void BeginFrame(X86Context& ctx) {
    uint64_t now = NowMs();
    {
        std::lock_guard lock(g_ui_mutex);
        if (!g_pending_nickname.empty()) {
            g_nickname = std::move(g_pending_nickname);
            g_pending_nickname.clear();
        }
    }
    if (g_command) {
        Request request;
        {
            std::lock_guard lock(g_ui_mutex);
            request = g_request;
            g_request = {};
            g_command = false;
            g_ui.error.clear();
        }
        if (request.command == Command::Start || request.command == Command::Stop) {
            StopSession(ctx);
            if (request.command == Command::Start && request.port &&
                (request.role == Role::Host || request.role == Role::Join)) {
                g_session = std::random_device{}() | 1;
                g_cfg.role = request.role;
                g_cfg.host = request.host;
                g_cfg.port = request.port;
                g_cfg.online = request.online;
                StartSession();
            }
        } else if (request.command == Command::Search) {
            if (!g_net && !net::Open(0)) PublishSession("Could not open a socket for LAN search.");
            else {
                g_search_port = request.port;
                g_search_until = now + 3000;
                g_search_tx = 0;
                g_searching = true;
                std::lock_guard lock(g_ui_mutex);
                g_ui.servers.clear();
            }
        }
    }
    online::Tick();
    if (g_net && g_cfg.online) {
        std::string error = online::Error();
        if (!error.empty()) {
            StopSession(ctx);
            PublishSession(error);
        } else if (g_cfg.role == Role::Join) {
            PartnerScope host(0);
            net::Peer peer = online::Partner();
            if (peer.online && (!g_have_peer || peer != g_peer)) {
                g_peer = peer;
                g_have_peer = true;
                g_welcomed = false;
                g_remote = {}, g_remote_play = {};
                g_path_count = 0;
                g_peer_session = 0;
                g_last_rx = g_last_tx = g_last_hello = 0;
            } else if (!peer.online && g_have_peer) {
                g_have_peer = g_welcomed = false;
                g_remote = {}, g_remote_play = {};
                g_path_count = 0;
                g_last_rx = 0;
            }
        }
    }
    if (g_net || g_searching) Receive(now);
    if (!g_reject.empty()) {
        std::string error = std::move(g_reject);
        g_reject.clear();
        StopSession(ctx);
        PublishSession(error);
    }
    if (g_searching) {
        if (now >= g_search_until) {
            g_searching = false;
            if (!g_net) net::Close();
        } else if (!g_search_tx || now - g_search_tx >= 500) {
            Hello query{Make(kDiscover, now), {}};
            std::memcpy(query.xbe_sha1, kImageSha1, 40);
            net::Send({0xFFFFFFFFu, g_search_port}, &query, sizeof query);
            net::Send({net::Resolve("127.0.0.1"), g_search_port}, &query, sizeof query);
            g_search_tx = now;
        }
    }
    PublishSession();
}

void Control(X86Context& ctx) { control::Poll(ctx); }

void EndFrame() {
    uint64_t now = NowMs();
    if (g_net) Send(now);
    else if (g_cfg.echo) {
        Echo(now);
        shots::Echo(now);
    }
    if (g_cfg.probe) Sample(g_local, now);
}

void MarkSimulated() {
    g_sim_frame = Frame();
    g_sim_seen = true;
}

void LatchCamera() {
    if (!Active()) return;
    uint32_t frame = Frame();
    if (g_cam.frame == frame) return;  // the frame's first scene pass, the main camera's
    uint32_t transforms = SYM(D3D_Device) + XDK_CDevice_Transforms;
    const float* v = &G<float>(transforms + kTransformView * 64);
    const float* p = &G<float>(transforms + kTransformProjection * 64);
    // row vectors: clip = [x y z 1] view projection
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += v[i * 4 + k] * p[k * 4 + j];
            g_cam.vp[i * 4 + j] = s;
        }
    g_cam.frame = frame;
}

bool RemoteMarker(Marker& m) {
    uint32_t world = R32(kWorld);
    if (!world || !Live()) return false;
    bool paused = PartnerPaused();  // in its pause menu, a hint, its failed page: where it was last
    if (!paused && (!g_remote.in_level || NowMs() - g_last_rx > kTimeoutMs)) return false;
    const State& from = paused ? g_remote_play : g_remote;
    if (!from.in_level || from.level != G<int8_t>(world + kWorldLevel)) return false;
    if (life::Watching() == CurrentPartner()) return false;  // this copy's camera is its eye
    static float positions[kMaxPlayers][3]{};
    static uint64_t timestamps[kMaxPlayers]{};
    auto& shown = positions[CurrentPartner()];
    auto& at = timestamps[CurrentPartner()];
    uint64_t now = NowMs();
    // down: where it lies (its view, while it watches someone, is that one's)
    const float lying[3] = {from.feet[0], from.feet[1] + 0.4f, from.feet[2]};
    const float* target = from.down ? lying : from.view[kRowEye];
    float d2 = 0;
    for (int i = 0; i < 3; i++) d2 += (target[i] - shown[i]) * (target[i] - shown[i]);
    float k = at && d2 < kSnap * kSnap ? 1 - std::exp(-float(now - at) / kSmoothMs) : 1;
    for (int i = 0; i < 3; i++) shown[i] += (target[i] - shown[i]) * k;
    at = now;
    if (!Project(shown, m)) return false;
    const float* eye = LocalEye();
    float d[3] = {shown[0] - eye[0], shown[1] - eye[1], shown[2] - eye[2]};
    float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const char* tag = from.down ? " DOWN" : paused ? " PAUSED" : "";
    const char* fmt = m.on_screen ? "%s %.0f%s" : m.x < game::Aspect() * 240 ? "< %s %.0f%s" : "%s %.0f%s >";
    std::snprintf(m.text, sizeof m.text, fmt, CurrentParticipant().nickname.c_str(), double(distance), tag);
    return true;
}

bool ProbeMarker(Marker& m) {
    if (!g_cfg.probe || !R32(kWorld) || !Live()) return false;
    const float* eye = LocalEye();
    const float* forward = eye - 4 * (kRowEye - kRowForward);
    float ahead[3];
    for (int i = 0; i < 3; i++) ahead[i] = eye[i] + forward[i] * 10;
    if (!Project(ahead, m)) return false;
    std::snprintf(m.text, sizeof m.text, "+");
    return true;
}

const char* Status() {
    static char s[32];
    uint64_t now = NowMs();
    if (!g_net) std::snprintf(s, sizeof s, g_cfg.echo ? "COOP ECHO" : g_cfg.probe ? "COOP PROBE" :
                                           control::Enabled() ? "COOP CONTROL" : "COOP OFF");
    else if (g_cfg.online && !g_have_peer) std::snprintf(s, sizeof s, "%s", online::Status().c_str());
    else if (life::Held()) std::snprintf(s, sizeof s, "%s", life::HeldStatus().c_str());
    else if (!g_have_peer) std::snprintf(s, sizeof s, "COOP HOST %u WAITING", g_cfg.port);
    else if (now - g_last_rx > kTimeoutMs) std::snprintf(s, sizeof s, "COOP NO PARTNER");
    else if (g_save_wait) std::snprintf(s, sizeof s, "COOP SAVE, THEN TO THE HOST");
    else if (g_joining) std::snprintf(s, sizeof s, !g_remote.mission_ready ? "COOP WAITING HOST MISSION" :
                                                g_follow.synchronizing ? "COOP SYNCING HOST PROGRESS" :
                                                g_follow.placing ? "COOP JOINING HOST" : "COOP LOADING HOST MISSION");
    else if (PlayerCount() > 2) std::snprintf(s, sizeof s, "COOP %d/8 LEVEL %d", PlayerCount(), int(g_remote.level));
    else if (g_remote.joining) std::snprintf(s, sizeof s, "COOP PARTNER JOINING");
    else if (world::WaitingForPartner()) std::snprintf(s, sizeof s, "COOP WAITING FOR PARTNERS");
    else if (g_remote.down) std::snprintf(s, sizeof s, "COOP PARTNER DOWN");
    else if (!g_remote.in_level) std::snprintf(s, sizeof s, g_remote.in_mission ? "COOP PARTNER PAUSED" : "COOP PARTNER IN MENUS");
    else std::snprintf(s, sizeof s, "COOP %d/8 LEVEL %d", PlayerCount(), int(g_remote.level));
    return s;
}

int ProbeLines(char (*lines)[32], int max) {
    if (!g_cfg.probe || max < 5) return 0;
    const State& s = g_local;
    if (!s.in_level) {
        std::snprintf(lines[0], 32, "NOT IN PLAY F%u", Frame());
        return 1;
    }
    std::snprintf(lines[0], 32, "EYE %.1f %.1f %.1f", double(s.view[3][0]), double(s.view[3][1]), double(s.view[3][2]));
    std::snprintf(lines[1], 32, "FWD %.2f %.2f %.2f", double(s.view[2][0]), double(s.view[2][1]), double(s.view[2][2]));
    std::snprintf(lines[2], 32, "UP %.2f %.2f %.2f", double(s.view[1][0]), double(s.view[1][1]), double(s.view[1][2]));
    std::snprintf(lines[3], 32, "RGT %.2f %.2f %.2f", double(s.view[0][0]), double(s.view[0][1]), double(s.view[0][2]));
    std::snprintf(lines[4], 32, "LVL %d YAW %.0f PIT %.0f", int(s.level), double(s.yaw), double(s.pitch));
    const shots::Counters& c = shots::Count();
    std::snprintf(lines[5], 32, "SHOT S%u T%u F%u D%u", c.sent, c.taken, c.fired, c.dropped);
    const world::Counters& w = world::Count();
    std::snprintf(lines[6], 32, "WLD S%u T%u A%u W%u L%u X%u", w.sent, w.taken, w.applied, w.waiting, w.lost, w.stale);
    return 7;
}

}  // namespace coop

// One frame of the main loop, eax the game object; its one call is at 0xE1AB7. The world's update, the scene and
// the overlay all run inside it, so the partner's packets go in before and this copy's player goes out after.
X86_EXTERN_FUNC(__imp__sub_0020F6A0);
X86_FUNC(sub_0020F6A0) {
    sprint::Tick();  // the sprint's longer step, before the frame takes the player's place
    if (!coop::Active()) return __imp__sub_0020F6A0(ctx, base);
    coop::BeginFrame(ctx);
    coop::world::FlowTick(ctx);  // the world stopped or not
    coop::MissionTick(ctx);
    if (coop::g_rescue.exchange(false)) coop::world::AskRescue();  // its steps in FlowTick
    coop::Control(ctx);  // test commands; the game object stays in eax, hle calls put registers back
    __imp__sub_0020F6A0(ctx, base);
    coop::EndFrame();
}

// The loader derives these gameplay options from local campaign unlocks, then
// requests resource type 0x0C (0x1EE21A). A joining copy uses the host's variant
// before that lookup and the fallback filename, without altering either save.
X86_EXTERN_FUNC(__imp__sub_001F3CC0);
X86_FUNC(sub_001F3CC0) {
    using namespace coop;
    uint32_t world = R32(kWorld);
    uint32_t saved_type = ctx.esi;
    if (HostHeard() && g_follow.loading && !MissionReady() && ctx.edi == 0x0C && world &&
        G<int8_t>(world + kWorldLevel) == g_remote.level && g_remote.weapon_variant <= 1 && g_remote.unlimited_ammo <= 1) {
        G<uint8_t>(world + 0x5CB0) = g_remote.weapon_variant;
        G<uint8_t>(world + 0x5CB1) = g_remote.unlimited_ammo;
        ctx.esi = (ctx.esi & 0xFFu) | uint32_t(g_remote.weapon_variant) << 8;
    }
    __imp__sub_001F3CC0(ctx, base);
    ctx.esi = saved_type;
}
