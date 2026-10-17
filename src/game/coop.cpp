// Co-op, phase 1: the partner's player as a marker over the frame.
// The player is the world's +0x30. Its +0xD0 holds four rows of 16 bytes: right, up, forward and the eye, the last
// two the world space view hooks.cpp also reads. The camera's view and projection are the device copies that
// D3DDevice_SetTransform keeps, set by the camera begin update 0x2B830 before the scene draws.
#include <algorithm>
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

#include "app/app.h"
#include "common/aspect.h"
#include "common/options.h"
#include "core/log.h"
#include "game/coop.h"
#include "game/coop_control.h"
#include "game/coop_net.h"
#include "game/coop_npcs.h"
#include "game/coop_shots.h"
#include "game/coop_world.h"
#include "game/layout.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

extern const char kImageSha1[];  // the xbe recompiled into this executable, written by CMake

using namespace kernel;
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
constexpr uint32_t kFailedState = 0x2103C, kCompleteState = 0x2104C;  // its sub-states once the play is over
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
constexpr uint16_t kProtocol = 15;
enum Type : uint8_t { kHello = 1, kWelcome, kState, kBye, kShots, kWorldEvents, kNpcSnap, kDiscover, kOffer };

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
struct Offer {
    Hello hello;
    char name[64];
    uint8_t busy;
};
struct State {
    Header h;
    uint8_t in_level;
    int8_t level;
    uint8_t load;        // the sender's level loads (world::Load)
    int8_t load_level;   // and the level of its last
    uint8_t crouch;      // its player crouches (player+0x3A1, from its controller +0x40 at 0x1F09C1)
    uint8_t unit;         // its current unit (world+0x5AF0), in or out of play
    uint8_t next_active;  // and the next one activated there
    uint8_t waiting;      // the unit its advance waits for this copy to reach (world::WaitingUnit), 0 none
    uint8_t in_mission;   // it plays this mission: its in-level state is the game's (in play, paused, a hint, a
                          // cinematic), not on its failed or complete page (a play that is over)
    float view[4][3];  // right, up, forward, eye
    float feet[3];     // the body, player+0xA0: where the triggers test the player
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
#pragma pack(pop)
constexpr int kMaxDatagram = 1200;
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
} g_cfg;

std::atomic<bool> g_net{false};
std::atomic<bool> g_command{false}, g_searching{false};
std::mutex g_ui_mutex;
Session g_ui;
enum class Command { None, Start, Stop, Search };
struct Request {
    Command command = Command::None;
    Role role = Role::Off;
    std::string host;
    uint16_t port = 45410;
} g_request;
int g_world_stage = 6;
uint64_t g_search_until = 0, g_search_tx = 0;
uint16_t g_search_port = 45410;
bool g_have_peer = false;
bool g_welcomed = false;
bool g_sim_seen = false;
net::Peer g_peer;
uint32_t g_session = 0, g_peer_session = 0;
uint32_t g_seq = 0, g_rx_seq = 0, g_sim_frame = 0;
uint64_t g_last_rx = 0, g_last_tx = 0, g_last_hello = 0;
State g_remote{};
std::atomic<bool> g_rescue{false};
uint32_t g_rescues = 0;
State g_remote_play{};  // its last state in play: its view and place while it is paused
State g_local{};
// the partner's walk as it came, for the triggers: each state of its in a level, oldest dropped
struct PathPoint {
    float feet[3];
    int8_t level;
    uint8_t reach;  // the farthest unit the partner could stand in then: its current one or the one it waits for
    uint8_t load;   // the partner's level load it was in: points of a play before are not walked any more
};
constexpr uint64_t kPathSize = 256;
PathPoint g_path[kPathSize];
uint64_t g_path_count = 0;
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

Header Make(Type t, uint64_t now) { return {kMagic, kProtocol, t, 0, g_session, ++g_seq, uint32_t(now)}; }

uint32_t Frame() { return R32(kFrameCount); }

// in play: the world steps and its player with it
bool Live() { return g_sim_seen && Frame() - g_sim_frame < kLiveFrames; }

bool Finite(const State& s) {
    for (auto& row : s.view)
        for (float f : row)
            if (!std::isfinite(f)) return false;
    for (float f : s.feet)
        if (!std::isfinite(f)) return false;
    return std::isfinite(s.yaw) && std::isfinite(s.pitch);
}

void SendHello(uint64_t now) {
    Hello h{Make(kHello, now), {}};
    std::memcpy(h.xbe_sha1, kImageSha1, sizeof h.xbe_sha1);
    net::Send(g_peer, &h, sizeof h);
}

void Sample(State& s, uint64_t now) {
    s = {};
    s.h = Make(kState, now);
    s.shot_ack = shots::Taken();
    s.world_ack = world::Taken();
    s.load = world::Load();
    s.load_level = world::LoadLevel();
    uint32_t world = R32(kWorld);
    s.in_level = world && Live();
    if (world) {
        s.unit = uint8_t(R32(world + kWorldUnit));
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t slot = world + kUnitSlots + k * kUnitSlotSize;
            if (R32(slot + 8) == s.unit + 1u && G<uint8_t>(slot + 0x38)) s.next_active = 1;
        }
        s.waiting = world::WaitingUnit();
        uint32_t game = R32(kGame);
        uint32_t sub = game ? R32(game + kInLevelState + 0x08) : 0;  // its sub-state (0x20EB60)
        s.in_mission = game && R32(game + kGameState) == game + kInLevelState && sub != game + kFailedState &&
                       sub != game + kCompleteState;
        uint32_t stream = R32(world + 0x5AE8);  // an advance under way (1, then 3 at 0x1F1143): its unit is the next one
        if (stream == 1 || stream == 3) s.waiting = std::max<uint8_t>(s.waiting, uint8_t(s.unit + 1));
    }
    if (!s.in_level) return;
    uint32_t player = world + kWorldPlayer;
    s.level = G<int8_t>(world + kWorldLevel);
    for (uint32_t r = 0; r < 4; r++) std::memcpy(s.view[r], HostPtr(player + kPlayerView + r * 16), 12);
    std::memcpy(s.feet, HostPtr(player + kPlayerFeet), 12);
    s.yaw = G<float>(world + kWorldController + kControllerYaw);
    s.pitch = G<float>(world + kWorldController + kControllerPitch);
    s.crouch = G<uint8_t>(player + kPlayerCrouch);
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
            offer.busy = g_have_peer && now - g_last_rx < kTimeoutMs;
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
        bool known = g_have_peer && from == g_peer;
        // the host takes one partner, a new one once the last has gone quiet
        if (g_cfg.role == Role::Host && !known) {
            if (in.h.type != kHello || n != int(sizeof(Hello))) continue;
            if (g_have_peer && now - g_last_rx < kTimeoutMs) continue;
            if (std::memcmp(in.hello.xbe_sha1, kImageSha1, sizeof in.hello.xbe_sha1) != 0) {
                LOGR("coop: %s runs another xbe, refused", net::Text(from).c_str());
                continue;
            }
            g_peer = from;
            g_have_peer = known = true;
            LOGR("coop: partner %s", net::Text(from).c_str());
        }
        if (!known) continue;
        if (in.h.session != g_peer_session) {  // the partner started again: a new sequence, nothing of the old one
            g_peer_session = in.h.session;
            g_rx_seq = 0;
            g_remote = {}, g_remote_play = {};
            shots::Reset();
            world::Reset();
        }
        g_last_rx = now;
        switch (in.h.type) {
            case kHello: {
                Header w = Make(kWelcome, now);
                net::Send(g_peer, &w, sizeof w);
                break;
            }
            case kWelcome:
                if (!g_welcomed) LOGR("coop: joined %s", net::Text(g_peer).c_str());
                g_welcomed = true;
                break;
            case kState:
                if (n == int(sizeof(State)) && Finite(in.state) && int32_t(in.h.seq - g_rx_seq) > 0) {
                    g_remote = in.state;
                    if (g_remote.in_level) g_remote_play = g_remote;
                    g_rx_seq = in.h.seq;
                    if (in.state.in_level) {
                        PathPoint& pp = g_path[g_path_count % kPathSize];
                        std::memcpy(pp.feet, in.state.feet, sizeof pp.feet);
                        pp.level = in.state.level;
                        pp.reach = std::max(in.state.unit, in.state.waiting);
                        pp.load = in.state.load;
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

void Send(uint64_t now) {
    if (!g_have_peer) return;
    if (now - g_last_rx > kTimeoutMs) world::Hold(now);  // its events wait for the partner to be heard again
    if (g_cfg.role == Role::Join && (!g_welcomed || now - g_last_rx > kTimeoutMs)) {
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
        net::Send(g_peer, out.raw, int(sizeof(Shots)) + bytes);
    }
    n = world::Outgoing(out.raw + sizeof(Shots), kMaxDatagram - int(sizeof(Shots)), bytes, now);
    if (n) {
        out.shots.h = Make(kWorldEvents, now);
        out.shots.ack = world::Taken();
        out.shots.count = uint8_t(n);
        std::memset(out.shots.pad, 0, sizeof out.shots.pad);
        out.shots.pad[0] = world::Load();
        out.shots.pad[1] = uint8_t(world::LoadLevel());
        net::Send(g_peer, out.raw, int(sizeof(Shots)) + bytes);
    }
    if (now - g_last_tx < kSendMs) return;
    g_last_tx = now;
    State s;
    Sample(s, now);
    net::Send(g_peer, &s, sizeof s);
    // the host's living enemies, latest wins
    n = npcs::Snapshot(out.raw + sizeof(Shots), kMaxDatagram - int(sizeof(Shots)), bytes);
    uint32_t world = R32(kWorld);
    if (world && s.in_level) {  // a paused host sends none: its puppets stand
        out.shots.h = Make(kNpcSnap, now);
        out.shots.ack = 0;
        out.shots.count = uint8_t(n);
        std::memset(out.shots.pad, 0, sizeof out.shots.pad);
        out.shots.pad[0] = uint8_t(world ? G<int8_t>(world + kWorldLevel) : int8_t(-1));
        if (g_cfg.role == Role::Host) net::Send(g_peer, out.raw, int(sizeof(Shots)) + bytes);
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

bool Active() { return g_net || g_command || g_searching || g_cfg.echo || g_cfg.probe || control::Enabled(); }

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
bool PartnerCrouch() { return g_remote.in_level && g_remote.crouch; }

bool PartnerFeet(float out[3]) {
    uint32_t world = R32(kWorld);
    if (!g_net || !world || !Live() || !g_remote.in_level || NowMs() - g_last_rx > kTimeoutMs) return false;
    if (g_remote.level != G<int8_t>(world + kWorldLevel)) return false;
    std::memcpy(out, g_remote.feet, 12);
    return true;
}
bool PartnerInLevel() {
    uint32_t world = R32(kWorld);
    uint64_t now = NowMs();
    return g_net && world && now - g_last_rx <= kTimeoutMs && g_remote.load_level == G<int8_t>(world + kWorldLevel) &&
           (g_remote.in_level || g_remote.in_mission);
}
bool PartnerPaused() { return PartnerInLevel() && !g_remote.in_level; }
bool PartnerAt(float out[3]) {
    uint32_t world = R32(kWorld);
    if (!g_net || !world || !g_remote.in_level || NowMs() - g_last_rx > kTimeoutMs) return false;
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
bool HostHeard() { return g_net && g_cfg.role == Role::Join && g_welcomed && NowMs() - g_last_rx <= kTimeoutMs; }
bool Networked() { return g_net; }

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
// (the partner's reach then at least the unit), in the partner's present play. When it stood still, its farthest
// step of that walk, nearer than a step
bool TrailPoint(const float partner[3], int8_t level, uint8_t unit, float out[3]) {
    constexpr float kNear = 1.2f, kFar = 6.0f;
    uint64_t first = g_path_count > kPathSize ? g_path_count - kPathSize : 0;
    float best = -1;
    for (uint64_t k = g_path_count; k-- > first;) {
        const PathPoint& p = g_path[k % kPathSize];
        if (p.level != level || p.load != g_remote.load || p.reach < unit) break;
        float d[3] = {p.feet[0] - partner[0], p.feet[1] - partner[1], p.feet[2] - partner[2]};
        float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist > kFar) break;
        if (dist >= kNear) {
            std::memcpy(out, p.feet, 12);
            return true;
        }
        if (dist > best) best = dist, std::memcpy(out, p.feet, 12);
    }
    return best >= 0;
}

// the partner's newest place on its walk in this play past that unit, for a partner whose world is stopped
bool LastPlace(int8_t level, uint8_t unit, float out[3]) {
    if (!g_path_count) return false;
    const PathPoint& p = g_path[(g_path_count - 1) % kPathSize];
    if (p.level != level || p.load != g_remote.load || p.reach < unit) return false;
    std::memcpy(out, p.feet, 12);
    return true;
}

}  // namespace

// this copy's player put next to the partner: the world's units are the partner's already (world::FlowTick)
bool PlaceByPartner(uint8_t unit) {
    uint32_t world = R32(kWorld);
    if (!world) return false;
    int8_t level = G<int8_t>(world + kWorldLevel);
    float partner[3];
    bool at = PartnerAt(partner);
    if (!at && PartnerInLevel()) at = LastPlace(level, unit, partner);  // stopped (its world waits): where it was last
    if (!at) {
        LOGR("coop: get me out of here: the partner is not in this level");
        return false;
    }
    float to[3];
    // no step of its walk: where it stands (no guess beside it, which may be a wall or back over a trigger)
    if (!TrailPoint(partner, level, unit, to)) std::memcpy(to, partner, 12);
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
    g_ui.port = g_cfg.port;
    g_ui.status = Status();
    if (!error.empty()) g_ui.error = error;
}

void StartSession() {
    if (g_cfg.role == Role::Off) return;
    if (!net::Open(g_cfg.role == Role::Host ? g_cfg.port : 0)) {
        PublishSession("Could not open UDP port " + std::to_string(g_cfg.port) + ". Check whether it is already in use.");
        return;
    }
    if (g_cfg.role == Role::Join) {
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
    shots::SetMode(true, false);
    world::SetMode(true, g_world_stage);
    npcs::SetMode(g_cfg.role == Role::Host && g_world_stage >= 4, g_cfg.role == Role::Join && g_world_stage >= 4,
                  g_world_stage >= 5);
    LOGR("coop: world sync stage %d", g_world_stage);
    if (g_cfg.role == Role::Host) LOGR("coop: host on UDP %u", g_cfg.port);
    else LOGR("coop: joining %s", net::Text(g_peer).c_str());
    PublishSession();
}

void StopSession(X86Context& ctx) {
    if (g_net && g_have_peer) {
        Header bye = Make(kBye, NowMs());
        net::Send(g_peer, &bye, sizeof bye);
    }
    npcs::Stop(ctx);
    world::Stop(ctx);
    shots::Reset();
    shots::SetMode(false, false);
    net::Close();
    g_net = false;
    g_searching = false;
    g_cfg.role = Role::Off;
    g_cfg.echo = false;
    g_have_peer = g_welcomed = false;
    g_peer = {};
    g_remote = {}, g_remote_play = {};
    g_peer_session = g_seq = g_rx_seq = 0;
    g_last_rx = g_last_tx = g_last_hello = 0;
    g_path_count = 0;
    g_echo.clear();
    g_rescue = false;
    LOGR("coop: session stopped");
}

}  // namespace

void Init() {
    g_session = std::random_device{}() | 1;  // never 0, the value before any partner
    const game::Options& o = game::Opt();
    g_cfg.role = Role(std::min(o.coop, 2u));
    g_cfg.port = uint16_t(o.coop_port && o.coop_port <= 65535 ? o.coop_port : 45410);
    {
        std::lock_guard<std::recursive_mutex> ui(app::UiLock());
        g_cfg.host = app::Get().join_ip;
    }
    // per process: copies on one PC share the settings file
    std::string v;
    if (net::Arg("--coop", v)) g_cfg.role = v == "host" ? Role::Host : v == "join" ? Role::Join : Role::Off;
    if (net::Arg("--join", v) && !v.empty()) g_cfg.host = v;
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
    if (g_net || g_searching) Receive(now);
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
    static float shown[3];
    static uint64_t at;
    uint64_t now = NowMs();
    const float* target = from.view[kRowEye];
    float d2 = 0;
    for (int i = 0; i < 3; i++) d2 += (target[i] - shown[i]) * (target[i] - shown[i]);
    float k = at && d2 < kSnap * kSnap ? 1 - std::exp(-float(now - at) / kSmoothMs) : 1;
    for (int i = 0; i < 3; i++) shown[i] += (target[i] - shown[i]) * k;
    at = now;
    if (!Project(shown, m)) return false;
    const float* eye = LocalEye();
    float d[3] = {shown[0] - eye[0], shown[1] - eye[1], shown[2] - eye[2]};
    float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const char* fmt = m.on_screen                    ? (paused ? "P2 %.0f PAUSED" : "P2 %.0f")
                      : m.x < game::Aspect() * 240 ? (paused ? "< P2 %.0f PAUSED" : "< P2 %.0f")
                                                   : (paused ? "P2 %.0f PAUSED >" : "P2 %.0f >");
    std::snprintf(m.text, sizeof m.text, fmt, double(distance));
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
    else if (!g_have_peer) std::snprintf(s, sizeof s, "COOP HOST %u WAITING", g_cfg.port);
    else if (now - g_last_rx > kTimeoutMs) std::snprintf(s, sizeof s, "COOP NO PARTNER");
    else if (world::WaitingForPartner()) std::snprintf(s, sizeof s, "COOP WAITING FOR P2");
    else if (!g_remote.in_level) std::snprintf(s, sizeof s, g_remote.in_mission ? "COOP P2 PAUSED" : "COOP P2 IN MENUS");
    else std::snprintf(s, sizeof s, "COOP P2 LEVEL %d", int(g_remote.level));
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
    if (!coop::Active()) return __imp__sub_0020F6A0(ctx, base);
    coop::BeginFrame(ctx);
    coop::world::FlowTick(ctx);  // the world stopped or not
    if (coop::g_rescue.exchange(false)) coop::world::AskRescue();  // its steps in FlowTick
    coop::Control(ctx);  // test commands; the game object stays in eax, hle calls put registers back
    __imp__sub_0020F6A0(ctx, base);
    coop::EndFrame();
}
