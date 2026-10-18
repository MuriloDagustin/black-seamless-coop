#pragma once
// Co-op participant state and host-routed UDP/EOS sessions, up to eight players.
// Everything here but Init runs on the game thread, inside one call of the frame function 0x20F6A0.
#include <cstdint>
#include <string>
#include <vector>

#include "kernel/kernel.h"

namespace coop {

constexpr int kMaxPlayers = 8;
constexpr int kNicknameBytes = 64, kNicknameCharacters = 20;
std::string CleanNickname(const std::string& name);
void RequestNickname(const std::string& name); // save locally, broadcast on the next frame
int LocalPlayer();
int CurrentPartner();
bool Connected(int player);
int PlayerCount();
bool Hosting();
// Game-thread context for an independent participant's state and event channel.
class PartnerScope {
    int previous;
public:
    explicit PartnerScope(int player);
    ~PartnerScope();
    PartnerScope(const PartnerScope&) = delete;
};
std::string Players();

enum class Role : uint32_t { Off = 0, Host = 1, Join = 2 };

struct Server {
    std::string address, name;
    uint16_t port = 45410;
    bool busy = false;
    uint32_t session = 0;
};
struct Session {
    struct Member { int player; std::string nickname; bool local, host; };
    std::string nickname = "Player";
    std::vector<Member> members;
    Role role = Role::Off;
    bool running = false, pending = false, searching = false;
    bool online = false, online_available = false;
    uint16_t port = 45410;
    std::string status = "COOP OFF", error;
    int players = 0, capacity = kMaxPlayers;
    std::string room_code;
    std::vector<Server> servers;
};
Session GetSession();  // thread-safe copy for F10
void RequestStart(Role role, const std::string& host, uint16_t port);
void RequestOnline(Role role, const std::string& code = {});
void RequestStop();
void RequestSearch(uint16_t port);

void Init();           // the settings and the command line, the socket; before boot
bool Active();         // anything to run or draw: a partner, the echo or the probe
bool Networked();      // a role and an open socket: the partner waits on this copy's packets
bool JoiningMission(); // joining/loading/catching up: wait for the host's world baseline
void MissionTick(X86Context& ctx); // follow the host's mission, after native retry flow
bool LoadMission(X86Context& ctx, int level, int difficulty, int stage = 1);
void BeginFrame(X86Context& ctx);  // queued UI commands and the partner's packets
void Control(X86Context& ctx);  // the test control channel's commands (coop_control.cpp)
void EndFrame();       // this copy's player, to the partner
void MarkSimulated();  // the player update runs this frame (PhysicsTime 0x1F11CD)
void LatchCamera();    // the scene camera's view and projection are the device's (AmbientOcclusion 0x1EE9A5)
bool PartnerFeet(float out[3]);  // the partner's body in this level, in play and heard from of late
void PartnerMotion(float direction[3], float& speed);  // measured by the sender, independent of packet jitter
bool PartnerInLevel();           // heard from of late, its last load this level: in play or paused in it a while
bool PartnerAt(float out[3]);    // the partner in play in this level, this copy's world stopped or not
bool PartnerPaused();            // in this level, out of play (its pause menu, a hint, a page)
// "get me out of here": this copy's player put next to the partner, on the way the partner came. Asked from any
// thread (F8, a controller's two stick clicks held a second, the test channel); done at the next frame
void AskRescue();
void PadRescue(int port, uint16_t buttons);  // the pad thread: a controller's L3 + R3 held
uint32_t Rescues();
bool HostHeard();                // a joining copy: its host welcomed it and was heard from of late
bool InPlay();                   // the world steps and its player with it
const float* PartnerForward();   // the partner's view, its last state's forward row
bool PartnerCrouch();            // the partner crouches
// the partner's current unit in this level (-1 not in it), its next one activated, the unit its advance waits for
int PartnerUnit(int* next_active, int* waiting);
bool PartnerDown();  // the partner is dead in this level, waiting for the team's checkpoint
bool PartnerView(float out[4][3]);  // the partner's view in this level: right, up, forward, eye
std::string Nickname(int player);   // game thread
bool PlaceByPartner(uint8_t unit);  // this copy's player put by the partner, on its way in that unit or past it
// the partner's walk in this level as received, the points after the count 'after' (moved on); gap: some were lost
int PartnerPath(uint64_t& after, float (*out)[3], int max, bool& gap);
uint64_t PartnerPathEnd();

// x and y in lines of a 480 frame from the output's top left, as the overlay's DrawText takes them
struct Marker {
    float x = 0, y = 0;
    bool on_screen = false;
    char text[96] = {};
};
bool RemoteMarker(Marker& m);  // false: no partner to show in this level and frame
bool ProbeMarker(Marker& m);   // the probe's self test: a point ahead of the eye, on the crosshair when all is right
const char* Status();          // 31 characters at most
int ProbeLines(char (*lines)[32], int max);

}  // namespace coop
