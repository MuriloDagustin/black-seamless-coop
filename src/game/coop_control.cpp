// The test control channel. One command a datagram, one reply each:
//   ping                          pong and the frame
//   state                         key=value: frame, level, unit, play, page, feet, eye, fwd, yaw, pitch, hp, coop
//   npcs                          the live characters, one a line
//   triggers [all]                the armed (or all) type 11 triggers: id, mode, action, where
//   props [n]                     the n (12) nearest props of the loaded units: id, kind, state, health, where
//   stand <list.index|id> [n]     the body in that trigger's centre as the triggers are tested, n frames (12)
//   page                          the menu page's name
//   pad <a,b,x,y,black,white,lt,rt,up,down,left,right,start,back,ls,rs> [ms]   press for ms (150)
//   hold <names>                  pressed until release
//   stick <lx> <ly> <rx> <ry> [ms]  -1..1, for ms (500)
//   release                       the pad at rest
//   yaw <deg> / pitch <deg>       the view's angles, absolute; turn <deg>: relative yaw
//   aim <x> <y> <z> / aim nearest the view on a point (a living enemy's chest, followed), until aim off
//   teleport <x> <y> <z> / teleport partner   the body there, or a step behind the partner
//   rocket [speed]                an RPG round of the player's from the eye along the view (0x1E35F0, side 3)
//   log <text>                    a line in this copy's log
//   peek / peekf <addr> [n]       n dwords, hex or floats; addr hex, or @player+hex, @world+hex
//   pokef <addr> <f>...           floats written there
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/input.h"
#include "core/log.h"
#include "game/coop.h"
#include "game/coop_control.h"
#include "game/coop_net.h"
#include "game/coop_npcs.h"
#include "game/coop_shots.h"
#include "game/coop_world.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using layout::kWorld;
using layout::kWorldPlayer;

namespace coop::control {

namespace {

constexpr uint32_t kFrameCount = 0x2F4368;
constexpr uint32_t kWorldLevel = 0x5AEC;
constexpr uint32_t kWorldUnit = 0x5AF0;
constexpr uint32_t kWorldController = 0x520;
constexpr uint32_t kControllerYaw = 0x18;    // degrees, positive left (mouse.cpp)
constexpr uint32_t kControllerPitch = 0x1C;  // positive down
constexpr uint32_t kPlayerFeet = 0xA0;
constexpr uint32_t kWorldLastPos = 0x1C0;  // the player's position a frame ago
constexpr uint32_t kWorldHeight = 0x310;   // the player's height, float
constexpr uint32_t kPlayerForward = 0xF0;
constexpr uint32_t kPlayerEye = 0x100;
constexpr uint32_t kCharHealth = 0x2F0;
constexpr uint32_t kPool = 0x2D1980;
constexpr uint32_t kPoolChars = 0xA0;
constexpr uint32_t kCharSize = 0x3C0;
constexpr int kChars = 32;
constexpr uint32_t kCharName = 0x08;
constexpr uint32_t kCharPos = 0xA0;
constexpr uint32_t kCharState = 0x384;
constexpr uint32_t kCharType = 0x394;
constexpr uint32_t kCharTeam = 0x39C;
constexpr float kChest = 1.3f;  // above the feet, world units
constexpr uint32_t kLogic = 0x2D45D0;          // the logic manager: trigger lists at +0x48/+0x4C, counts u16 +0/+2
constexpr uint32_t kTriggerVtable = 0x28EF88;  // type 11
constexpr float kPi = 3.14159265f;

bool g_enabled = false;
X86Context* g_ctx = nullptr;  // the game thread's, while Poll runs the commands

// stand: the player put in a trigger's centre for some frames, after its own update and before the triggers are
// tested (and before this copy's state goes to the partner), the trigger's box holding the body as it is tested
struct Stand {
    uint32_t id_lo = 0, id_hi = 0;  // the trigger, found again each frame: a unit unloaded takes it away
    int frames = 0;
} g_stand;

// the pad, from the game thread to the pad thread
struct Injected {
    uint64_t buttons_until = 0, sticks_until = 0;
    bool held = false;
    uint16_t buttons = 0;
    uint8_t analog[8] = {};
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
std::mutex g_pad_lock;
Injected g_pad;

// aiming: each frame the view's angles are set on the target, a living enemy followed while it lives.
// The yaw is the forward row's heading atan2(x, z) in degrees, the pitch its elevation negated (positive down).
struct Aim {
    bool on = false;
    bool nearest = false;
    float target[3] = {};
    int frames = 0;
    float error = 0;  // degrees between the forward row and the target, last measured
} g_aim;

uint64_t NowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32_t Player() { return R32(kWorld) + kWorldPlayer; }

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t' && s[j] != '\r' && s[j] != '\n') j++;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

// names to the pad: buttons bits and analog indices
bool AddButtons(const std::string& list, uint16_t& buttons, uint8_t analog[8]) {
    static const char* kAnalog[] = {"a", "b", "x", "y", "black", "white", "lt", "rt"};
    static const char* kBits[] = {"up", "down", "left", "right", "start", "back", "ls", "rs"};
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string name = list.substr(i, j - i);
        bool found = false;
        for (int k = 0; k < 8; k++)
            if (name == kAnalog[k]) analog[k] = 0xFF, found = true;
        for (int k = 0; k < 8; k++)
            if (name == kBits[k]) buttons |= uint16_t(1 << k), found = true;
        if (!found) return false;
        i = j + 1;
    }
    return true;
}

int16_t Axis(const std::string& s) {
    float f = std::clamp(float(std::atof(s.c_str())), -1.0f, 1.0f);
    return int16_t(f * 32767);
}

void Vec(char* out, size_t n, const char* key, const float* v) {
    std::snprintf(out, n, " %s=%.2f,%.2f,%.2f", key, double(v[0]), double(v[1]), double(v[2]));
}

std::string PageName() {
    uint32_t ui = R32(layout::kUi);
    if (!ui) return "-";
    const char* name = static_cast<const char*>(HostPtr(ui + layout::kPageName));
    std::string s;
    for (int i = 0; i < 48 && name[i]; i++) s += name[i];
    return s.empty() ? "-" : s;
}

bool InPlay() {
    uint32_t world = R32(kWorld);
    return world && coop::InPlay();
}

std::string State() {
    char buf[1024];
    std::string s;
    uint32_t world = R32(kWorld);
    std::snprintf(buf, sizeof buf, "frame=%u level=%d unit=%d play=%d page=%s", R32(kFrameCount),
                  world ? int(G<int8_t>(world + kWorldLevel)) : -1, world ? int(R32(world + kWorldUnit)) : -1,
                  InPlay() ? 1 : 0, PageName().c_str());
    s += buf;
    if (world) {
        uint32_t p = Player();
        Vec(buf, sizeof buf, "feet", &G<float>(p + kPlayerFeet));
        s += buf;
        Vec(buf, sizeof buf, "eye", &G<float>(p + kPlayerEye));
        s += buf;
        Vec(buf, sizeof buf, "fwd", &G<float>(p + kPlayerForward));
        s += buf;
        uint32_t c = world + kWorldController;
        std::snprintf(buf, sizeof buf, " yaw=%.1f pitch=%.1f hp=%.1f", double(G<float>(c + kControllerYaw)),
                      double(G<float>(c + kControllerPitch)), double(G<float>(p + kCharHealth)));
        s += buf;
    }
    if (g_aim.on || g_aim.frames) {
        std::snprintf(buf, sizeof buf, " aim=%s err=%.1f", g_aim.on ? "on" : "done", double(g_aim.error));
        s += buf;
    }
    const shots::Counters& sc = shots::Count();
    const world::Counters& wc = world::Count();
    uint32_t slot, sound, sstate;
    shots::Gunshot(slot, sound, sstate);
    const npcs::Counters& nc = npcs::Count();
    std::snprintf(buf, sizeof buf,
                  " shots=%u/%u/%u/%u flash=%u gunshot=%u heard=%u slot=%08X sound=%08X sstate=%u world=%u/%u/%u/%u/%u/%u/%u"
                  " props=%u proj=%u npcs=%u/%u/%u/%u/%u body=%08X/%u seen=%u targeted=%u coop=\"%s\"",
                  sc.sent, sc.taken, sc.fired, sc.dropped, sc.flashes, sc.gunshots, sc.heard, slot, sound, sstate, wc.sent, wc.taken,
                  wc.applied, wc.waiting, wc.lost, wc.stale, wc.triggers, wc.props, wc.projectiles, nc.snapshots, nc.made,
                  nc.let_go, nc.driven, nc.refused, npcs::Body(), nc.bodies, nc.seen, nc.targeted, coop::Status());
    s += buf;
    return s;
}

// a living enemy's chest, the nearest to the eye; false when none
bool Nearest(float out[3], float* distance = nullptr) {
    uint32_t pool = R32(kPool);
    if (!pool || !R32(kWorld)) return false;
    const float* eye = &G<float>(Player() + kPlayerEye);
    float best = 1e30f;
    bool found = false;
    for (int i = 0; i < kChars; i++) {
        uint32_t ch = pool + kPoolChars + uint32_t(i) * kCharSize;
        if (R32(ch + kCharState) != 0 || !R32(ch + kCharTeam) || !(R32(ch + kCharName) | R32(ch + kCharName + 4)))
            continue;
        const float* pos = &G<float>(ch + kCharPos);
        float d[3] = {pos[0] - eye[0], pos[1] + kChest - eye[1], pos[2] - eye[2]};
        float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (dd < best) {
            best = dd;
            out[0] = pos[0], out[1] = pos[1] + kChest, out[2] = pos[2];
            found = true;
        }
    }
    if (found && distance) *distance = std::sqrt(best);
    return found;
}

std::string Npcs() {
    uint32_t pool = R32(kPool);
    if (!pool || !R32(kWorld)) return "none";
    const float* eye = &G<float>(Player() + kPlayerEye);
    std::string s;
    char buf[192];
    for (int i = 0; i < kChars; i++) {
        uint32_t ch = pool + kPoolChars + uint32_t(i) * kCharSize;
        uint32_t state = R32(ch + kCharState);
        if (state != 0 || !R32(ch + kCharType)) continue;  // a slot never used is all 0
        const float* pos = &G<float>(ch + kCharPos);
        float d[3] = {pos[0] - eye[0], pos[1] - eye[1], pos[2] - eye[2]};
        std::snprintf(buf, sizeof buf, "s%d %08X%08X team=%u type=%u hp=%.0f at=%.1f,%.1f,%.1f d=%.1f\n", i,
                      R32(ch + kCharName + 4), R32(ch + kCharName), R32(ch + kCharTeam), R32(ch + kCharType),
                      double(G<float>(ch + kCharHealth)), double(pos[0]), double(pos[1]), double(pos[2]),
                      double(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2])));
        s += buf;
    }
    return s.empty() ? "none" : s;
}

// A trigger's +0x70..+0xAF is world to its box (0x1BF4E0 applies it to the player with 0x12600, p.x r0 + p.y r1 +
// p.z r2 + r3): the box's centre in the world is where that gives 0, -(r_j . r3) on each axis j
void TriggerCentre(uint32_t t, float out[3]) {
    const float* r3 = &G<float>(t + 0xA0);
    for (int j = 0; j < 3; j++) {
        const float* r = &G<float>(t + 0x70 + uint32_t(j) * 16);
        out[j] = -(r[0] * r3[0] + r[1] * r3[1] + r[2] * r3[2]);
    }
}

// the logic manager's type 11 triggers: list, id, armed, mode, action, the box's centre and half sizes
std::string Triggers(bool all) {
    uint32_t mgr = R32(kLogic);
    if (!mgr || !R32(kWorld)) return "none";
    const float* eye = &G<float>(Player() + kPlayerEye);
    std::string s;
    char buf[160];
    for (uint32_t list = 0; list < 2; list++) {
        uint32_t n = G<uint16_t>(mgr + list * 2);
        uint32_t triggers = R32(mgr + 0x48 + list * 4);
        for (uint32_t i = 0; triggers && i < n && s.size() < 7800; i++) {
            uint32_t t = R32(triggers + i * 4);
            if (!t || R32(t) != kTriggerVtable) continue;
            bool armed = G<uint8_t>(t + 0x11C);
            if (!all && !armed) continue;
            float p[3];
            TriggerCentre(t, p);
            const float* half = &G<float>(t + 0x130);
            float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
            std::snprintf(buf, sizeof buf, "%u.%u %08X%08X armed=%d mode=%u action=%u at=%.1f,%.1f,%.1f half=%.1f,%.1f,%.1f d=%.1f\n",
                          list, i, R32(t + 0x0C), R32(t + 0x08), armed ? 1 : 0, R32(t + 0x120), R32(t + 0x124),
                          double(p[0]), double(p[1]), double(p[2]), double(half[0]), double(half[1]), double(half[2]),
                          double(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2])));
            s += buf;
        }
    }
    return s.empty() ? "none" : s;
}

// the loaded units' kind 3 and kind 4 props nearest the eye: id, kind, state, health, where (their matrix row +0xA0)
std::string Props(int max) {
    uint32_t world = R32(kWorld);
    if (!world) return "none";
    const float* eye = &G<float>(Player() + kPlayerEye);
    struct Row {
        float d;
        std::string text;
    };
    std::vector<Row> rows;
    char buf[160];
    for (uint32_t slot = 0; slot < 2; slot++) {
        uint32_t s = world + 0x49D0 + slot * 0x880;
        for (uint32_t kind = 3; kind <= 4; kind++) {
            uint32_t array = R32(s + (kind == 3 ? 0x18 : 0x1C));
            uint32_t n = R32(s + (kind == 3 ? 0x30 : 0x34));
            uint32_t stride = kind == 3 ? 0x1D0 : 0x200;
            if (!array || n > 4096) continue;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t p = array + i * stride;
                const float* pos = &G<float>(p + 0xA0);
                float d[3] = {pos[0] - eye[0], pos[1] - eye[1], pos[2] - eye[2]};
                float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                int state = kind == 3 ? int(int32_t(R32(p + 0x1A4))) : int(int32_t(R32(R32(p + 0x1A0) + 4)));
                float hp = kind == 3 ? G<float>(p + 0x1A0) : G<float>(R32(p + 0x1A0));
                std::snprintf(buf, sizeof buf, "%08X%08X k%u u%u state=%d hp=%.0f at=%.1f,%.1f,%.1f d=%.1f\n", R32(p + 0x0C),
                              R32(p + 0x08), kind, slot, state, double(hp), double(pos[0]), double(pos[1]),
                              double(pos[2]), double(dist));
                rows.push_back({dist, buf});
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d < b.d; });
    std::string out;
    for (int i = 0; i < int(rows.size()) && i < max; i++) out += rows[size_t(i)].text;
    return out.empty() ? "none" : out;
}

// a type 11 trigger by "list.index" or by its id in hex
uint32_t FindTrigger(const std::string& key) {
    uint32_t mgr = R32(kLogic);
    if (!mgr) return 0;
    size_t dot = key.find('.');
    for (uint32_t list = 0; list < 2; list++) {
        uint32_t n = G<uint16_t>(mgr + list * 2);
        uint32_t triggers = R32(mgr + 0x48 + list * 4);
        for (uint32_t i = 0; triggers && i < n; i++) {
            uint32_t t = R32(triggers + i * 4);
            if (!t || R32(t) != kTriggerVtable) continue;
            bool match;
            if (dot != std::string::npos && key.size() < 8)
                match = std::to_string(list) + "." + std::to_string(i) == key;
            else {
                char id[20];
                std::snprintf(id, sizeof id, "%08X%08X", R32(t + 0x0C), R32(t + 0x08));
                match = _stricmp(id, key.c_str()) == 0;
            }
            if (match) return t;
        }
    }
    return 0;
}

// the view's errors to the target, degrees: heading around the up axis (y), elevation
bool AimErrors(float& yaw_err, float& pitch_err) {
    uint32_t p = Player();
    const float* eye = &G<float>(p + kPlayerEye);
    const float* f = &G<float>(p + kPlayerForward);
    float t[3] = {g_aim.target[0] - eye[0], g_aim.target[1] - eye[1], g_aim.target[2] - eye[2]};
    float th = std::sqrt(t[0] * t[0] + t[2] * t[2]);
    float fh = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    if (th < 0.01f || fh < 0.01f) return false;
    float heading_f = std::atan2(f[0], f[2]);
    float heading_t = std::atan2(t[0], t[2]);
    yaw_err = std::remainder(heading_t - heading_f, 2 * kPi) * 180 / kPi;
    pitch_err = (std::atan2(t[1], th) - std::atan2(f[1], fh)) * 180 / kPi;
    return true;
}

// a frame of the aim: the angles on the target
void AimStep() {
    if (!g_aim.on) return;
    uint32_t world = R32(kWorld);
    if (!world || (g_aim.nearest && !Nearest(g_aim.target))) {
        g_aim.on = false;
        return;
    }
    g_aim.frames++;
    float ye, pe;
    if (AimErrors(ye, pe)) g_aim.error = std::sqrt(ye * ye + pe * pe);
    const float* eye = &G<float>(Player() + kPlayerEye);
    float t[3] = {g_aim.target[0] - eye[0], g_aim.target[1] - eye[1], g_aim.target[2] - eye[2]};
    float th = std::sqrt(t[0] * t[0] + t[2] * t[2]);
    if (th < 0.01f) return;
    uint32_t c = world + kWorldController;
    float& yaw = G<float>(c + kControllerYaw);
    float heading = std::atan2(t[0], t[2]) * 180 / kPi;
    yaw += std::remainder(heading - yaw, 360.0f);
    G<float>(c + kControllerPitch) = -std::atan2(t[1], th) * 180 / kPi;
}

// the player's body there: its matrix's position row is where the character controller takes it from, and the
// position a frame ago goes with it, so no trigger sweeps the way between
void Teleport(const float to[3]) {
    uint32_t world = R32(kWorld);
    float* feet = &G<float>(Player() + kPlayerFeet);
    float* last = &G<float>(world + kWorldLastPos);
    for (int i = 0; i < 3; i++) feet[i] = to[i], last[i] = to[i];
}

// hex, or @player+hex / @world+hex
uint32_t Address(const std::string& s) {
    uint32_t base = 0;
    std::string rest = s;
    if (s.rfind("@player", 0) == 0) base = R32(kWorld) ? Player() : 0, rest = s.substr(7);
    else if (s.rfind("@world", 0) == 0) base = R32(kWorld), rest = s.substr(6);
    if (s[0] == '@' && !base) return 0;
    if (!rest.empty() && rest[0] == '+') rest = rest.substr(1);
    uint32_t off = rest.empty() ? 0 : uint32_t(std::strtoul(rest.c_str(), nullptr, 16));
    return base + off;
}

std::string Run(const std::string& line) {
    std::vector<std::string> a = Split(line);
    if (a.empty()) return "error empty";
    const std::string& c = a[0];
    uint64_t now = NowMs();
    if (c == "ping") return "pong frame=" + std::to_string(R32(kFrameCount));
    if (c == "state") return State();
    if (c == "npcs") return Npcs();
    if (c == "triggers") return Triggers(a.size() > 1 && a[1] == "all");
    if (c == "props") return Props(std::clamp(a.size() > 1 ? std::atoi(a[1].c_str()) : 12, 1, 60));
    if (c == "stand") {
        // stand <list.index | id hex> [frames]
        if (a.size() < 2) return "error stand <list.index|id> [frames]";
        uint32_t t = FindTrigger(a[1]);
        if (!t) return "error no such trigger";
        g_stand = {R32(t + 0x08), R32(t + 0x0C), std::clamp(a.size() > 2 ? std::atoi(a[2].c_str()) : 12, 1, 600)};
        char buf[64];
        std::snprintf(buf, sizeof buf, "standing in %08X%08X", R32(t + 0x0C), R32(t + 0x08));
        LOGR("ctl: %s", buf);
        return buf;
    }
    if (c == "page") return PageName();
    if (c == "log") {
        LOGR("ctl: %s", line.c_str());
        return "ok";
    }
    if (c == "peek" || c == "peekf") {
        // peek <address> [dwords]: hex, a guest address; @player and @world stand for those objects
        if (a.size() < 2) return "error " + c + " <address|@player+off|@world+off> [count]";
        uint32_t at = Address(a[1]);
        int n = std::clamp(a.size() > 2 ? std::atoi(a[2].c_str()) : 4, 1, 64);
        if (!at || !mem::IsCommitted(at) || !mem::IsCommitted(at + uint32_t(n) * 4 - 1)) return "error not mapped";
        std::string s;
        char buf[64];
        for (int i = 0; i < n; i++) {
            if (c == "peek") std::snprintf(buf, sizeof buf, "%08X ", R32(at + uint32_t(i) * 4));
            else std::snprintf(buf, sizeof buf, "%.9g ", double(G<float>(at + uint32_t(i) * 4)));
            s += buf;
        }
        return s;
    }
    if (c == "pokef") {
        if (a.size() < 3) return "error pokef <address> <float>...";
        uint32_t at = Address(a[1]);
        if (!at || !mem::IsCommitted(at) || !mem::IsCommitted(at + uint32_t(a.size() - 2) * 4 - 1)) return "error not mapped";
        for (size_t i = 2; i < a.size(); i++) G<float>(at + uint32_t(i - 2) * 4) = float(std::atof(a[i].c_str()));
        LOGR("ctl: %s", line.c_str());
        return "ok";
    }
    if (c == "pad" || c == "hold") {
        if (a.size() < 2) return "error pad <names> [ms]";
        Injected next;
        if (!AddButtons(a[1], next.buttons, next.analog)) return "error unknown button in " + a[1];
        int ms = a.size() > 2 ? std::atoi(a[2].c_str()) : 150;
        std::lock_guard<std::mutex> lock(g_pad_lock);
        g_pad.buttons = next.buttons;
        std::memcpy(g_pad.analog, next.analog, sizeof g_pad.analog);
        g_pad.held = c == "hold";
        g_pad.buttons_until = now + uint64_t(std::max(ms, 1));
        return "ok";
    }
    if (c == "stick") {
        if (a.size() < 5) return "error stick <lx> <ly> <rx> <ry> [ms]";
        int ms = a.size() > 5 ? std::atoi(a[5].c_str()) : 500;
        std::lock_guard<std::mutex> lock(g_pad_lock);
        g_pad.lx = Axis(a[1]), g_pad.ly = Axis(a[2]), g_pad.rx = Axis(a[3]), g_pad.ry = Axis(a[4]);
        g_pad.sticks_until = now + uint64_t(std::max(ms, 1));
        return "ok";
    }
    if (c == "release") {
        std::lock_guard<std::mutex> lock(g_pad_lock);
        g_pad = {};
        g_aim.on = false;
        return "ok";
    }
    uint32_t world = R32(kWorld);
    if (c == "yaw" || c == "pitch" || c == "turn") {
        if (!world) return "error no world";
        if (a.size() < 2) return "error " + c + " <degrees>";
        float v = float(std::atof(a[1].c_str()));
        uint32_t ctl = world + kWorldController;
        if (c == "yaw") G<float>(ctl + kControllerYaw) = v;
        else if (c == "pitch") G<float>(ctl + kControllerPitch) = v;
        else G<float>(ctl + kControllerYaw) += v;
        return "ok";
    }
    if (c == "rocket") {
        // the player's RPG round from the eye along the view, as the RPG's fire does (0x1E878F): the game's own rocket
        if (!world) return "error no world";
        uint32_t armoury = R32(0x2D12D0), table = armoury ? R32(armoury) : 0;
        uint32_t entry = table ? R32(table + 4) + 6 * 32 : 0;  // type 6, BG1_RPG
        uint32_t fx = entry ? R32(entry + 0x10) : 0;
        if (!fx || !R32(0x2D1998)) return "error no rocket here";
        float speed = a.size() > 1 ? float(std::atof(a[1].c_str())) : 40.0f;
        static uint32_t matrix = 0, vel = 0;
        if (!matrix) matrix = mem::AllocSystem(64, 16), vel = mem::AllocSystem(16, 16);
        uint32_t p = Player();
        float m[16];
        std::memcpy(m, HostPtr(p + 0xD0), 64);  // right, up, forward, the eye
        for (int i = 0; i < 3; i++) m[12 + i] += m[8 + i] * 1.0f;
        m[15] = 1;
        float v[4] = {m[8] * speed, m[9] * speed, m[10] * speed, 0};
        std::memcpy(HostPtr(matrix), m, 64);
        std::memcpy(HostPtr(vel), v, 16);
        static constexpr hle::Map kRocket =
            hle::Make({hle::ecx, hle::edi, hle::stack, hle::stack, hle::stack, hle::stack});
        hle::Call(*g_ctx, 0x1E35F0, kRocket, {R32(0x2D1998), 3, matrix, vel, R32(fx + 0x08), R32(fx + 0x0C)});
        LOGR("ctl: rocket fired");
        return "rocket fired";
    }
    if (c == "teleport") {
        if (!world) return "error no world";
        float to[3];
        if (a.size() >= 2 && a[1] == "partner") {
            if (!coop::PartnerFeet(to)) return "error no partner in this level";
            // a step behind the partner along its view, not into its body
            float back = 1.5f;
            const float* fwd = coop::PartnerForward();
            float len = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
            if (len > 0.01f) to[0] -= fwd[0] / len * back, to[2] -= fwd[2] / len * back;
        } else if (a.size() >= 4) {
            for (int i = 0; i < 3; i++) to[i] = float(std::atof(a[1 + i].c_str()));
        } else {
            return "error teleport <x> <y> <z> | teleport partner";
        }
        Teleport(to);
        char buf[96];
        std::snprintf(buf, sizeof buf, "teleported to %.2f,%.2f,%.2f", double(to[0]), double(to[1]), double(to[2]));
        LOGR("ctl: %s", buf);
        return buf;
    }
    if (c == "aim") {
        if (!world) return "error no world";
        if (a.size() >= 2 && a[1] == "off") {
            g_aim.on = false;
            return "ok";
        }
        Aim next;
        next.on = true;
        if (a.size() >= 2 && a[1] == "nearest") {
            float d = 0;
            if (!Nearest(next.target, &d)) return "error no living enemy";
            next.nearest = true;
            g_aim = next;
            char buf[96];
            std::snprintf(buf, sizeof buf, "aiming at %.1f,%.1f,%.1f d=%.1f", double(next.target[0]),
                          double(next.target[1]), double(next.target[2]), double(d));
            return buf;
        }
        if (a.size() < 4) return "error aim <x> <y> <z> | aim nearest";
        for (int i = 0; i < 3; i++) next.target[i] = float(std::atof(a[1 + i].c_str()));
        g_aim = next;
        return "aiming";
    }
    return "error unknown command " + c;
}

}  // namespace

void Init(uint16_t port) {
    if (!net::OpenControl(port)) return;
    g_enabled = true;
    LOGR("coop: control channel on 127.0.0.1:%u", port);
}

bool Enabled() { return g_enabled; }

void Poll(X86Context& ctx) {
    if (!g_enabled) return;
    g_ctx = &ctx;
    char in[512];
    net::Peer from;
    for (int i = 0, n; i < 16 && (n = net::ReceiveControl(from, in, sizeof in - 1)) >= 0; i++) {
        if (n == 0) continue;
        in[n] = 0;
        std::string reply = Run(in);
        constexpr size_t kReplyMax = 8000;  // loopback takes a datagram of 64 KB
        if (reply.size() > kReplyMax) reply = reply.substr(0, kReplyMax) + "\n...cut";
        net::SendControl(from, reply.data(), int(reply.size()));
    }
    AimStep();
}

void BeforeTriggers() {
    if (!g_enabled || g_stand.frames <= 0 || !R32(kWorld)) return;
    g_stand.frames--;
    char id[20];
    std::snprintf(id, sizeof id, "%08X%08X", g_stand.id_hi, g_stand.id_lo);
    uint32_t t = FindTrigger(id);
    if (!t) {
        g_stand.frames = 0;
        return;
    }
    float at[3];
    TriggerCentre(t, at);
    at[1] -= G<float>(R32(kWorld) + kWorldHeight) * 0.5f;  // the test lifts the body by half its height
    Teleport(at);
}

void LevelReset() {
    g_stand = {};
    g_aim.on = false;
}

bool Override(int port, input::Pad& pad) {
    if (!g_enabled) return false;
    pad = input::Pad{};
    if (port != 0) return true;  // the others unplugged
    pad.connected = true;
    uint64_t now = NowMs();
    std::lock_guard<std::mutex> lock(g_pad_lock);
    if (g_pad.held || now < g_pad.buttons_until) {
        pad.buttons = g_pad.buttons;
        std::memcpy(pad.analog, g_pad.analog, sizeof pad.analog);
    }
    if (now < g_pad.sticks_until) {
        pad.lx = g_pad.lx, pad.ly = g_pad.ly, pad.rx = g_pad.rx, pad.ry = g_pad.ry;
    }
    return true;
}

}  // namespace coop::control
