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
constexpr uint16_t kProtocol = 10;
enum Type : uint8_t { kHello = 1, kWelcome, kState, kBye, kShots, kWorldEvents, kNpcSnap };

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
struct State {
    Header h;
    uint8_t in_level;
    int8_t level;
    uint8_t load;        // the sender's level loads (world::Load)
    int8_t load_level;   // and the level of its last
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

bool g_net = false;
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
uint64_t g_remote_play_at = 0;  // when the partner's state last said it was in play
State g_remote_play{};          // its last state in play: its view and place while it is paused
constexpr uint64_t kAwayMs = 60000;  // out of play that long (its menus): not in the level any more
State g_local{};
// the partner's walk as it came, for the triggers: each state of its in a level, oldest dropped
struct PathPoint {
    float feet[3];
    int8_t level;
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
    if (!s.in_level) return;
    uint32_t player = world + kWorldPlayer;
    s.level = G<int8_t>(world + kWorldLevel);
    for (uint32_t r = 0; r < 4; r++) std::memcpy(s.view[r], HostPtr(player + kPlayerView + r * 16), 12);
    std::memcpy(s.feet, HostPtr(player + kPlayerFeet), 12);
    s.yaw = G<float>(world + kWorldController + kControllerYaw);
    s.pitch = G<float>(world + kWorldController + kControllerPitch);
    if (!Finite(s)) s.in_level = 0;
}

void Receive(uint64_t now) {
    Packet in;
    net::Peer from;
    for (int i = 0, n; i < 256 && (n = net::Receive(from, &in, sizeof in)) >= 0; i++) {
        if (n < int(sizeof(Header)) || in.h.magic != kMagic || in.h.protocol != kProtocol) continue;
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
                    if (g_remote.in_level) g_remote_play_at = now, g_remote_play = g_remote;
                    g_rx_seq = in.h.seq;
                    if (in.state.in_level) {
                        PathPoint& pp = g_path[g_path_count % kPathSize];
                        std::memcpy(pp.feet, in.state.feet, sizeof pp.feet);
                        pp.level = in.state.level;
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

bool Active() { return g_net || g_cfg.echo || g_cfg.probe || control::Enabled(); }

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
           (g_remote.in_level || now - g_remote_play_at < kAwayMs);
}
bool PartnerAt(float out[3]) {
    uint32_t world = R32(kWorld);
    if (!g_net || !world || !g_remote.in_level || NowMs() - g_last_rx > kTimeoutMs) return false;
    if (g_remote.level != G<int8_t>(world + kWorldLevel)) return false;
    std::memcpy(out, g_remote.feet, 12);
    return true;
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

// a place the partner stood a moment ago, a step or more from it: walked on, not in a wall
bool TrailPoint(const float partner[3], int8_t level, float out[3]) {
    constexpr float kNear = 1.2f, kFar = 6.0f;
    uint64_t first = g_path_count > kPathSize ? g_path_count - kPathSize : 0;
    for (uint64_t k = g_path_count; k-- > first;) {
        const PathPoint& p = g_path[k % kPathSize];
        if (p.level != level) break;
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

void Rescue(X86Context& ctx) {
    uint32_t world = R32(kWorld);
    if (!world || !Live()) return;
    int8_t level = G<int8_t>(world + kWorldLevel);
    float partner[3];
    bool at = PartnerAt(partner);
    if (!at && PartnerInLevel() && g_path_count) {  // stopped (its world waits): where it was last
        const PathPoint& p = g_path[(g_path_count - 1) % kPathSize];
        if (p.level == level) std::memcpy(partner, p.feet, 12), at = true;
    }
    if (!at) {
        LOGR("coop: get me out of here: the partner is not in this level");
        return;
    }
    float to[3];
    if (!TrailPoint(partner, level, to)) {
        // standing still a while: a step behind it along its view, at its height
        const float* f = g_remote_play.view[kRowForward];
        float h = std::sqrt(f[0] * f[0] + f[2] * f[2]);
        std::memcpy(to, partner, 12);
        if (h > 0.01f) to[0] -= f[0] / h * 1.5f, to[2] -= f[2] / h * 1.5f;
    }
    world::RescueAdvance(ctx);  // past the advance the partner went through: this copy's too
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
}

}  // namespace

void Init() {
    g_session = std::random_device{}() | 1;  // never 0, the value before any partner
    const game::Options& o = game::Opt();
    g_cfg.role = Role(std::min(o.coop, 2u));
    g_cfg.port = uint16_t(o.coop_port ? o.coop_port : 45410);
    {
        std::lock_guard<std::recursive_mutex> ui(app::UiLock());
        g_cfg.host = app::Get().join_ip;
    }
    // per process: copies on one PC share the settings file
    std::string v;
    if (net::Arg("--coop", v)) g_cfg.role = v == "host" ? Role::Host : v == "join" ? Role::Join : Role::Off;
    if (net::Arg("--join", v) && !v.empty()) g_cfg.host = v;
    if (net::Arg("--coop-port", v) && std::atoi(v.c_str()) > 0) g_cfg.port = uint16_t(std::atoi(v.c_str()));
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
    int world_stage = 6;
    if (net::Arg("--coop-world", v) && !v.empty()) world_stage = std::atoi(v.c_str());
    if (g_cfg.role == Role::Off) return;
    if (!net::Open(g_cfg.role == Role::Host ? g_cfg.port : 0)) return;  // WSAStartup, before any name lookup
    if (g_cfg.role == Role::Join) {
        g_peer = {net::Resolve(g_cfg.host), g_cfg.port};
        if (!g_peer.addr) {
            LOGE("coop: join address '%s' not found", g_cfg.host.c_str());
            return;
        }
        g_have_peer = true;
    }
    g_cfg.echo = false;  // a partner instead
    g_net = true;
    shots::SetMode(true, false);
    world::SetMode(true, world_stage);
    npcs::SetMode(g_cfg.role == Role::Host && world_stage >= 4, g_cfg.role == Role::Join && world_stage >= 4,
                  world_stage >= 5);
    LOGR("coop: world sync stage %d", world_stage);
    if (g_cfg.role == Role::Host) LOGR("coop: host on UDP %u", g_cfg.port);
    else LOGR("coop: joining %s", net::Text(g_peer).c_str());
}

void BeginFrame() {
    if (g_net) Receive(NowMs());
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
    if (!world || !Live() || !g_remote.in_level || NowMs() - g_last_rx > kTimeoutMs) return false;
    if (g_remote.level != G<int8_t>(world + kWorldLevel)) return false;
    static float shown[3];
    static uint64_t at;
    uint64_t now = NowMs();
    const float* target = g_remote.view[kRowEye];
    float d2 = 0;
    for (int i = 0; i < 3; i++) d2 += (target[i] - shown[i]) * (target[i] - shown[i]);
    float k = at && d2 < kSnap * kSnap ? 1 - std::exp(-float(now - at) / kSmoothMs) : 1;
    for (int i = 0; i < 3; i++) shown[i] += (target[i] - shown[i]) * k;
    at = now;
    if (!Project(shown, m)) return false;
    const float* eye = LocalEye();
    float d[3] = {shown[0] - eye[0], shown[1] - eye[1], shown[2] - eye[2]};
    float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const char* fmt = m.on_screen ? "P2 %.0f" : m.x < game::Aspect() * 240 ? "< P2 %.0f" : "P2 %.0f >";
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
    if (!g_net) std::snprintf(s, sizeof s, g_cfg.echo ? "COOP ECHO" : g_cfg.probe ? "COOP PROBE" : "COOP CONTROL");
    else if (!g_have_peer) std::snprintf(s, sizeof s, "COOP HOST %u WAITING", g_cfg.port);
    else if (now - g_last_rx > kTimeoutMs) std::snprintf(s, sizeof s, "COOP NO PARTNER");
    else if (world::WaitingForPartner()) std::snprintf(s, sizeof s, "COOP WAITING FOR P2");
    else if (!g_remote.in_level) std::snprintf(s, sizeof s, "COOP P2 IN MENUS");
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
    coop::BeginFrame();
    coop::world::FlowTick(ctx);  // the world stopped or not
    if (coop::g_rescue.exchange(false)) coop::Rescue(ctx);
    coop::Control(ctx);  // test commands; the game object stays in eax, hle calls put registers back
    __imp__sub_0020F6A0(ctx, base);
    coop::EndFrame();
}
