#pragma once
// Co-op, phase 1 (the ghost): each copy sends its player's view over UDP and the other marks where the partner is.
// Everything here but Init runs on the game thread, inside one call of the frame function 0x20F6A0.
#include <cstdint>

#include "kernel/kernel.h"

namespace coop {

enum class Role : uint32_t { Off = 0, Host = 1, Join = 2 };

void Init();           // the settings and the command line, the socket; before boot
bool Active();         // anything to run or draw: a partner, the echo or the probe
bool Networked();      // a role and an open socket: the partner waits on this copy's packets
void BeginFrame();     // the partner's packets
void Control(X86Context& ctx);  // the test control channel's commands (coop_control.cpp)
void EndFrame();       // this copy's player, to the partner
void MarkSimulated();  // the player update runs this frame (PhysicsTime 0x1F11CD)
void LatchCamera();    // the scene camera's view and projection are the device's (AmbientOcclusion 0x1EE9A5)
bool PartnerFeet(float out[3]);  // the partner's body in this level, in play and heard from of late
bool InPlay();                   // the world steps and its player with it
const float* PartnerForward();   // the partner's view, its last state's forward row

// x and y in lines of a 480 frame from the output's top left, as the overlay's DrawText takes them
struct Marker {
    float x = 0, y = 0;
    bool on_screen = false;
    char text[32] = {};
};
bool RemoteMarker(Marker& m);  // false: no partner to show in this level and frame
bool ProbeMarker(Marker& m);   // the probe's self test: a point ahead of the eye, on the crosshair when all is right
const char* Status();          // 31 characters at most
int ProbeLines(char (*lines)[32], int max);

}  // namespace coop
