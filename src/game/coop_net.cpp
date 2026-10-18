// The co-op's Win32 side, apart from the guest headers as xnet.cpp keeps it
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>

#include "app/app.h"
#include "core/log.h"
#include "game/coop_net.h"
#include "game/coop_online.h"

namespace coop::net {

namespace {

SOCKET g_socket = INVALID_SOCKET;
bool g_started = false;
constexpr DWORD kUdpConnReset = 0x9800000C;  // SIO_UDP_CONNRESET: a port unreachable must not fail the next recv

}  // namespace

bool Open(uint16_t port) {
    Close();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOGE("coop: WSAStartup failed");
        return false;
    }
    g_started = true;
    g_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET) {
        LOGE("coop: socket failed (%d)", WSAGetLastError());
        Close();
        return false;
    }
    u_long on = 1;
    ::ioctlsocket(g_socket, FIONBIO, &on);
    BOOL off = FALSE;
    BOOL broadcast = TRUE;
    setsockopt(g_socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof broadcast);
    DWORD bytes = 0;
    WSAIoctl(g_socket, kUdpConnReset, &off, sizeof off, nullptr, 0, &bytes, nullptr, nullptr);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(g_socket, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        LOGE("coop: bind of UDP %u failed (%d)", port, WSAGetLastError());
        Close();
        return false;
    }
    return true;
}

void Close() {
    online::Stop();
    if (g_socket != INVALID_SOCKET) ::closesocket(g_socket);
    g_socket = INVALID_SOCKET;
    if (g_started) WSACleanup();
    g_started = false;
}

std::string HostName() {
    char name[64]{};
    return gethostname(name, sizeof name - 1) == 0 ? name : "BLACK host";
}

uint32_t Resolve(const std::string& host) {
    size_t from = host.find_first_not_of(" \t");
    size_t to = host.find_last_not_of(" \t");
    if (from == std::string::npos) return 0;
    std::string name = host.substr(from, to - from + 1);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &found) != 0 || !found) return 0;
    uint32_t addr = reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(found);
    return addr;
}

bool Send(const Peer& to, const void* p, int n) {
    if (to.online) return online::Send(to.online, p, n);
    if (g_socket == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(to.port);
    a.sin_addr.s_addr = to.addr;
    return ::sendto(g_socket, static_cast<const char*>(p), n, 0, reinterpret_cast<sockaddr*>(&a), sizeof a) == n;
}

int Receive(Peer& from, void* p, int n) {
    if (online::Active()) return online::Receive(from, p, n);
    if (g_socket == INVALID_SOCKET) return -1;
    sockaddr_in a{};
    int len = sizeof a;
    int r = ::recvfrom(g_socket, static_cast<char*>(p), n, 0, reinterpret_cast<sockaddr*>(&a), &len);
    if (r == SOCKET_ERROR) return WSAGetLastError() == WSAEWOULDBLOCK ? -1 : 0;  // WSAEMSGSIZE and the like: lost
    from = {a.sin_addr.s_addr, ntohs(a.sin_port)};
    return r;
}

std::string Text(const Peer& p) {
    if (p.online) return online::PeerName(p.online);
    in_addr a{};
    a.s_addr = p.addr;
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(p.port);
}

bool Arg(const char* name, std::string& value) {
    std::wstring cmd = GetCommandLineW();
    std::wstring n(name, name + std::strlen(name));
    for (size_t at = cmd.find(n); at != std::wstring::npos; at = cmd.find(n, at + 1)) {
        size_t end = at + n.size();
        if (end < cmd.size() && cmd[end] != L' ' && cmd[end] != L'=') continue;  // --coop is not --coop-port
        if (at > 0 && cmd[at - 1] != L' ') continue;
        value.clear();
        size_t from = cmd.find_first_not_of(L" =", end);
        if (from != std::wstring::npos && cmd[from] != L'-')
            for (size_t i = from; i < cmd.size() && cmd[i] != L' '; i++) value += char(cmd[i]);
        return true;
    }
    return false;
}

bool Foreground() { return GetForegroundWindow() == static_cast<HWND>(app::Window()); }

namespace {
SOCKET g_control = INVALID_SOCKET;
}

bool OpenControl(uint16_t port) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_control = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_control == INVALID_SOCKET) return false;
    u_long on = 1;
    ::ioctlsocket(g_control, FIONBIO, &on);
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(g_control, kUdpConnReset, &off, sizeof off, nullptr, 0, &bytes, nullptr, nullptr);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(g_control, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        LOGE("coop: control channel bind of UDP %u failed (%d)", port, WSAGetLastError());
        ::closesocket(g_control);
        g_control = INVALID_SOCKET;
        return false;
    }
    return true;
}

int ReceiveControl(Peer& from, void* p, int n) {
    if (g_control == INVALID_SOCKET) return -1;
    sockaddr_in a{};
    int len = sizeof a;
    int r = ::recvfrom(g_control, static_cast<char*>(p), n, 0, reinterpret_cast<sockaddr*>(&a), &len);
    if (r == SOCKET_ERROR) return WSAGetLastError() == WSAEWOULDBLOCK ? -1 : 0;
    from = {a.sin_addr.s_addr, ntohs(a.sin_port)};
    return r;
}

bool SendControl(const Peer& to, const void* p, int n) {
    if (g_control == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(to.port);
    a.sin_addr.s_addr = to.addr;
    return ::sendto(g_control, static_cast<const char*>(p), n, 0, reinterpret_cast<sockaddr*>(&a), sizeof a) == n;
}

}  // namespace coop::net
