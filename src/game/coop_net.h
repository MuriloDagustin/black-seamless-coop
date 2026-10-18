#pragma once
// The co-op's Win32 side: one non-blocking UDP socket, the command line and the window's focus
#include <cstdint>
#include <string>

namespace coop::net {

struct Peer {
    uint32_t addr = 0;  // IPv4, network order
    uint16_t port = 0;  // host order
    uint64_t online = 0; // opaque EOS peer; zero for UDP, never sent on the wire
};
inline bool operator==(const Peer& a, const Peer& b) {
    return a.addr == b.addr && a.port == b.port && a.online == b.online;
}

bool Open(uint16_t port);                  // 0: any free port
void Close();
std::string HostName();
uint32_t Resolve(const std::string& host);  // an IPv4 or a name; 0 when not found
bool Send(const Peer& to, const void* p, int n);
int Receive(Peer& from, void* p, int n);  // -1 nothing waiting, 0 a datagram lost, else its bytes
std::string Text(const Peer& p);          // a.b.c.d:port
// --name value or --name=value on this process's command line; true when the name is there, value may stay empty
bool Arg(const char* name, std::string& value);
bool Foreground();  // the game window is the one in front

// the test control channel's own socket, on 127.0.0.1 only
bool OpenControl(uint16_t port);
int ReceiveControl(Peer& from, void* p, int n);  // as Receive
bool SendControl(const Peer& to, const void* p, int n);

}  // namespace coop::net
