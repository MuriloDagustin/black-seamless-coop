// Co-op lives. The player dies in its controller's event 5 (0x1D54C0): its controller (world+0x520) is swapped for
// the death one embedded in the character (+0x630), its state +0x384 set to 2 (3 for a scripted death, 0x2002C0)
// and its name cleared. The player controller's vf1 (0x1BD360) then sees state 2 each frame and asks the in-level
// state for the mission failed (vf5, 0x1F45C0). While a partner lives, both are held here. A revive undoes the swap,
// restores state, name and health, and lets the rescue bring the player to a living partner. Down, the player
// watches a living partner: its view rows (player+0xD0, which the scene's camera takes) are the partner's.
#include "game/coop_life.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "game/coop.h"
#include "game/coop_world.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using layout::kWorld;
using layout::kWorldPlayer;

namespace coop::life {

namespace {

constexpr uint32_t kGame = 0x2D45F0;
constexpr uint32_t kGameState = 0x21070, kInLevel = 0x20F78;
constexpr uint32_t kWorldController = 0x520;  // the player's own controller
constexpr uint32_t kWorldUnit = 0x5AF0;
constexpr uint32_t kCharName = 0x08, kCharHealth = 0x2F0, kCharControl = 0x324, kCharState = 0x384;
constexpr uint32_t kObjectives = 0x930;  // the objective list: 12-byte rows, +8 the state (3 complete)
constexpr uint32_t kLevel = 0x2D1990, kFilter = 0x830A0;  // the screen's colour filter: colour, +0x34 mode, +0x38 last
constexpr uint32_t kFilterColours = 0x2AD690;             // its modes' colours, 16 bytes each; 0 none

uint32_t g_name[2] = {};      // the player's name, saved as it died
float g_max_health = 0;       // the most it had alive, this play
bool g_down = false;
std::atomic<bool> g_held{false};
uint32_t g_down_unit = 0;     // the unit it died in
uint32_t g_down_objectives = 0;
uint64_t g_down_at = 0;

constexpr uint64_t kWatchAfterMs = 2500;  // the fall first, in the death camera
float g_watch_ahead = 0.3f;               // the camera this far ahead of the partner's eye: clear of its body's head
constexpr float kWatchFollow = 0.35f;     // of the way to the partner's view each frame: its 30 a second smoothed
constexpr uint32_t kPlayerView = 0xD0;    // right, up, forward, eye; 16 bytes each
int g_watch = -1;
std::atomic<int> g_watch_shown{-1};
std::atomic<int> g_watch_step{0};  // asked: +1 the next living partner, -1 the one before
std::atomic<int> g_living{0};      // living partners, for the HUD
bool g_watch_set = false;
float g_watch_view[4][3] = {};
std::mutex g_watch_mutex;
std::string g_watch_name;

uint64_t NowMs() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32_t Player() {
    uint32_t world = R32(kWorld);
    return world ? world + kWorldPlayer : 0;
}

bool InMission() {
    uint32_t game = R32(kGame);
    return game && R32(game + kGameState) == game + kInLevel;
}

uint32_t ObjectivesDone() {
    uint32_t world = R32(kWorld);
    if (!world) return 0;
    uint32_t rows = R32(world + kObjectives), count = std::min(R32(world + kObjectives + 4), 256u), done = 0;
    for (uint32_t i = 0; rows && i < count; i++)
        if (R32(rows + i * 12 + 8) == 3) done++;
    return done;
}

// a partner in this level, alive: the one to be brought to; -1 none
int LivingPartner() {
    for (int player = 0; player < coop::kMaxPlayers; player++) {
        if (!coop::Connected(player)) continue;
        coop::PartnerScope scope(player);
        if (coop::PartnerInLevel() && !coop::PartnerDown()) return player;
    }
    return -1;
}

// the screen's colour filter back to none, as 0x132794 ends one: the death's (mode 9, red) never fades
void ClearFilter() {
    uint32_t level = R32(kLevel);
    if (!level) return;
    uint32_t filter = level + kFilter;
    for (uint32_t i = 0; i < 16; i += 4) W32(filter + i, R32(kFilterColours + i));
    W32(filter + 0x10, 0);
    W32(filter + 0x34, 0);
    W32(filter + 0x38, 0);
}

bool Living(int player) {
    if (player < 0 || player >= coop::kMaxPlayers || !coop::Connected(player)) return false;
    coop::PartnerScope scope(player);
    return coop::PartnerInLevel() && !coop::PartnerDown();
}

// the living partner after 'after' (step 1) or before it (-1), round the players; -1 none
int NextLiving(int after, int step = 1) {
    int from = after < 0 ? (step > 0 ? -1 : 0) : after;
    for (int i = 1; i <= coop::kMaxPlayers; i++) {
        int player = ((from + step * i) % coop::kMaxPlayers + coop::kMaxPlayers) % coop::kMaxPlayers;
        if (Living(player)) return player;
    }
    return -1;
}

void Normalize(float v[3]) {
    float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n > 1e-6f) v[0] /= n, v[1] /= n, v[2] /= n;
}

void StopWatching() {
    if (g_watch >= 0) LOGR("coop: no longer watching player %d", g_watch);
    g_watch = -1;
    g_watch_shown = -1;
    g_watch_set = false;
}

// down while a partner lives: this copy's camera on a living partner's view. Each frame, and after the death
// controller's think (which moves the camera in the fall)
void Watch() {
    uint32_t p = Player();
    if (!p || !g_down || !HoldDeath() || NowMs() - g_down_at < kWatchAfterMs) {
        if (g_watch >= 0) StopWatching();
        return;
    }
    int living = 0;
    for (int i = 0; i < coop::kMaxPlayers; i++) living += Living(i) ? 1 : 0;
    g_living = living;
    int step = g_watch_step.exchange(0);
    if (step || !Living(g_watch)) {
        int to = NextLiving(g_watch, step < 0 ? -1 : 1);
        if (to != g_watch) {
            if (g_watch < 0) ClearFilter();  // the partner's view as it is, not the death's red
            g_watch = to;
            g_watch_set = false;
            g_watch_shown = to;
            std::lock_guard lock(g_watch_mutex);
            g_watch_name = to >= 0 ? coop::Nickname(to) : std::string();
            LOGR("coop: watching player %d (%s)", to, g_watch_name.c_str());
        }
    }
    if (g_watch < 0) return;
    // its health regrows while it lies dead, and low health blurs the screen and flashes it (0x149204): full
    G<float>(p + kCharHealth) = g_max_health > 0 ? g_max_health : 100.0f;
    float view[4][3];
    {
        coop::PartnerScope scope(g_watch);
        if (!coop::PartnerView(view)) return;
    }
    for (int k = 0; k < 3; k++) view[3][k] += view[2][k] * g_watch_ahead;
    float dx = view[3][0] - g_watch_view[3][0], dy = view[3][1] - g_watch_view[3][1], dz = view[3][2] - g_watch_view[3][2];
    if (!g_watch_set || dx * dx + dy * dy + dz * dz > 9.0f) {  // a new partner, or a jump of 3 m: there at once
        std::memcpy(g_watch_view, view, sizeof view);
        g_watch_set = true;
    } else {
        for (int r = 1; r < 4; r++)
            for (int k = 0; k < 3; k++) g_watch_view[r][k] += (view[r][k] - g_watch_view[r][k]) * kWatchFollow;
    }
    // forward and up kept square to each other, right = up x forward as the game's rows are
    float* f = g_watch_view[2];
    float* u = g_watch_view[1];
    float* r = g_watch_view[0];
    Normalize(f);
    float d = u[0] * f[0] + u[1] * f[1] + u[2] * f[2];
    for (int k = 0; k < 3; k++) u[k] -= f[k] * d;
    Normalize(u);
    r[0] = u[1] * f[2] - u[2] * f[1];
    r[1] = u[2] * f[0] - u[0] * f[2];
    r[2] = u[0] * f[1] - u[1] * f[0];
    for (int row = 0; row < 4; row++) std::memcpy(HostPtr(p + kPlayerView + row * 16), g_watch_view[row], 12);
}

}  // namespace

int Watching() { return g_watch_shown; }


void WatchNext() { g_watch_step = 1; }

void WatchAhead(float metres) { g_watch_ahead = metres; }

void PadWatch(int port, uint16_t buttons) {
    static uint16_t was[4];
    if (port < 0 || port > 3) return;
    constexpr uint16_t kLeft = 0x0004, kRight = 0x0008, kA = 0x1000;
    uint16_t pressed = buttons & ~was[port];
    was[port] = buttons;
    if (!g_held || g_watch_shown < 0) return;
    if (pressed & (kA | kRight)) g_watch_step = 1;
    else if (pressed & kLeft) g_watch_step = -1;
}

std::string HeldStatus() {
    if (g_watch_shown < 0) return "DOWN: TEAM TO CHECKPOINT";
    std::lock_guard lock(g_watch_mutex);
    if (g_living > 1) return "DOWN < " + g_watch_name + " >";  // A, a click or the arrows: the others
    return "DOWN: WATCHING " + g_watch_name;
}

bool Down() {
    uint32_t p = Player();
    if (!p || !InMission()) return false;
    uint32_t state = R32(p + kCharState);
    return state == 2 || state == 3;
}

bool HoldDeath() { return coop::Networked() && Down() && LivingPartner() >= 0; }

bool Held() { return g_held; }

void LevelReset() {
    StopWatching();
    g_down = false;
    g_max_health = 0;
    g_down_unit = g_down_objectives = 0;
}

bool Revive(X86Context& ctx, const char* why) {
    uint32_t world = R32(kWorld), p = Player();
    if (!world || !p) return false;
    uint32_t state = R32(p + kCharState);
    if (state != 2 && state != 3) return false;
    // the player's controller back in place of the death one, as 0x1D54FD swaps them: the old one's vf2, the new
    // one's vf1
    uint32_t own = world + kWorldController, now = R32(p + kCharControl);
    static constexpr hle::Map kThis = hle::Make({hle::ecx});
    if (now != own) {
        if (now) hle::Call(ctx, R32(R32(now) + 0x08), kThis, {now});
        W32(p + kCharControl, own);
    }
    W32(p + kCharState, 0);
    W32(p + kCharName, g_name[0]);
    W32(p + kCharName + 4, g_name[1]);
    G<float>(p + kCharHealth) = g_max_health > 0 ? g_max_health : 100.0f;
    uint32_t game = R32(kGame);
    if (game) {
        // its death paused the game (0x1BD379: 1 asks the pause at 0x20D44B, 2 its end at 0x20D569)
        W32(game + 0x21088, G<uint8_t>(game + 0x210C8) ? 2 : 0);
        G<uint8_t>(game + 0x210CB) = 0;
        W32(game + 0x21098, 0);
    }
    // the screen's colour filter ([0x2D1990]+0x830A0, applied to fx+0xD380 each frame by 0x15E260)
    ClearFilter();
    StopWatching();
    g_down = false;
    LOGR("coop: revived (%s), health %.0f", why, double(G<float>(p + kCharHealth)));
    int partner = LivingPartner();
    if (partner >= 0) coop::world::AskRescueTo(partner);  // by a living partner
    return true;
}

void Tick(X86Context& ctx) {
    uint32_t world = R32(kWorld), p = Player();
    if (!world || !p || !coop::Networked()) {
        g_held = false;
        if (g_watch >= 0) StopWatching();
        return;
    }
    g_held = HoldDeath();
    uint32_t state = R32(p + kCharState);
    if (state == 0 && InMission()) g_max_health = std::max(g_max_health, G<float>(p + kCharHealth));
    bool down = Down();
    if (down && !g_down) {
        g_down = true;
        g_down_unit = R32(world + kWorldUnit);
        g_down_objectives = ObjectivesDone();
        g_down_at = NowMs();
        LOGR("coop: down in unit %u%s", g_down_unit, HoldDeath() ? ", waiting for the team's checkpoint" : "");
    }
    if (!down) {
        g_down = false;
        if (g_watch >= 0) StopWatching();
        return;
    }
    // the team's checkpoint: a living partner past the unit it died in, or an objective done since
    for (int player = 0; player < coop::kMaxPlayers; player++) {
        if (!coop::Connected(player)) continue;
        coop::PartnerScope scope(player);
        if (!coop::PartnerInLevel() || coop::PartnerDown()) continue;
        int waiting = 0;
        int unit = coop::PartnerUnit(nullptr, &waiting);
        if (unit > int(g_down_unit)) {
            Revive(ctx, "the team reached the next area");
            return;
        }
    }
    if (ObjectivesDone() > g_down_objectives && Revive(ctx, "the team completed an objective")) return;
    // a world stopped for a death's sequence (game+0x21098 1, 0x2002C0) while held: going again
    if (uint32_t game = R32(kGame); game && g_held && R32(game + 0x21098) == 1) {
        W32(game + 0x21098, 0);
        LOGR("coop: down: the world goes on (its death sequence stopped it)");
    }
    Watch();
}

}  // namespace coop::life

using namespace coop::life;

// The player's death (thiscall ecx its controller; the event, three more; ret 0x10): event 5 kills it. Its name is
// kept for a revive
X86_EXTERN_FUNC(__imp__sub_001D54C0);
X86_FUNC(sub_001D54C0) {
    uint32_t world = R32(kWorld);
    if (world && R32(ctx.esp + 4) == 5 && R32(ctx.ecx + 0x8C) == world + kWorldPlayer) {
        uint32_t p = world + kWorldPlayer;
        if (R32(p + 0x384) == 0) g_name[0] = R32(p + 0x08), g_name[1] = R32(p + 0x0C);
        // [ctl+0x110] set, the death is the scripted one (0x2002C0): game+0x21098 1, and the world update then runs
        // only its sequence (0x1F110C): the enemies, the scripts, all that is the host's, still for everyone. With a
        // partner alive, the plain one
        int32_t scripted = int32_t(R32(ctx.ecx + 0x110));
        if (scripted > 0 && coop::Networked() && LivingPartner() >= 0) {
            uint32_t ctl = ctx.ecx;
            W32(ctl + 0x110, 0);
            LOGR("coop: a scripted death (%d) made a plain one: a partner lives", scripted);
            __imp__sub_001D54C0(ctx, base);
            W32(ctl + 0x110, uint32_t(scripted));
            return;
        }
    }
    __imp__sub_001D54C0(ctx, base);
}

// The player controller's vf1 (ecx it, plain ret): dead, it asks for the mission failed. Not while a partner lives
X86_EXTERN_FUNC(__imp__sub_001BD360);
X86_FUNC(sub_001BD360) {
    uint32_t world = R32(kWorld);
    if (world && R32(ctx.ecx + 0x8C) == world + kWorldPlayer && HoldDeath()) return Return(ctx, ctx.eax, 0);
    __imp__sub_001BD360(ctx, base);
}

// The death controller's think (ecx it, plain ret): once its fall is over it pauses the game (0x1C6CAD: game+0x21088
// 1) and asks for the mission failed. The failed is held in vf5; the pause too while a partner lives: the down
// copy's world goes on, its partners and enemies with it
X86_EXTERN_FUNC(__imp__sub_001C6410);
X86_FUNC(sub_001C6410) {
    uint32_t world = R32(kWorld);
    bool player = world && ctx.ecx == world + kWorldPlayer + 0x630;
    uint32_t game = R32(kGame);
    uint32_t ask = game ? R32(game + 0x21088) : 0;
    uint8_t shown = game ? G<uint8_t>(game + 0x210CB) : 0;
    __imp__sub_001C6410(ctx, base);
    if (game && ask == 0 && R32(game + 0x21088) == 1 && HoldDeath()) {
        W32(game + 0x21088, 0);
        G<uint8_t>(game + 0x210CB) = shown;
    }
    if (player) Watch();  // its camera moved in the fall: the watched partner's view again
}

// A camera's begin update (a render callback, 0x2C124: [esp+8] the camera; its frame +4, the frame's world matrix
// 0x2F760, synced if dirty: right, up, at, position as the player's view rows). The scene's camera is the one at the
// player's eye. Watching, it is put on the watched partner's view here: in a death by a hit the body's fall moves
// the player's view rows after the frame's update, the camera with them (through the floor)
X86_EXTERN_FUNC(__imp__sub_0002B830);
X86_FUNC(sub_0002B830) {
    uint32_t cam = R32(ctx.esp + 8), frame = cam ? R32(cam + 4) : 0, p = Player();
    if (frame && p && g_watch >= 0 && g_watch_set) {
        uint32_t ltm = kernel::CallGuest(ctx, 0x2F760, {frame});
        float d = 0;
        for (uint32_t k = 0; k < 3; k++) {
            float e = G<float>(ltm + 0x30 + k * 4) - G<float>(p + kPlayerView + 0x30 + k * 4);
            d += e * e;
        }
        if (d < 0.01f)
            for (uint32_t row = 0; row < 4; row++) {
                std::memcpy(HostPtr(ltm + row * 16), g_watch_view[row], 12);
                std::memcpy(HostPtr(p + kPlayerView + row * 16), g_watch_view[row], 12);
            }
    }
    __imp__sub_0002B830(ctx, base);
}

// The world's update (from the frame at 0x20F7FF; the player's at 0x1F1201 inside it): a body's fall after a hit
// moves the player's view rows in it, after the frame's start. The scene's look follows those rows, not only its
// camera (the sea's, the sky's, what is drawn where), so watching they are the watched partner's again after it
X86_EXTERN_FUNC(__imp__sub_001F1080);
X86_FUNC(sub_001F1080) {
    __imp__sub_001F1080(ctx, base);
    uint32_t p = Player();
    if (p && g_watch >= 0 && g_watch_set)
        for (uint32_t row = 0; row < 4; row++) std::memcpy(HostPtr(p + kPlayerView + row * 16), g_watch_view[row], 12);
}
