#include "game/coop_online.h"
#include "game/coop.h"

#if !defined(BLACK_WITH_EOS)
namespace coop::online {
bool Built() { return false; }
bool Start(bool, const std::string&, const std::string&) { return false; }
void Stop() {}
void Tick() {}
bool Pumping() { return false; }
bool Active() { return false; }
net::Peer Partner() { return {}; }
std::string Code() { return {}; }
std::string Status() { return "ONLINE NOT INSTALLED"; }
std::string Error() { return "This build needs the EOS C SDK. See docs/online.md."; }
bool Send(uint64_t, const void*, int) { return false; }
int Receive(net::Peer&, void*, int) { return -1; }
std::string PeerName(uint64_t) { return "EOS"; }
}
#else

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <vector>
#include <toml++/toml.hpp>

#include <eos_sdk.h>
#include <eos_connect.h>
#include <eos_lobby.h>
#include <eos_p2p.h>

#include "app/app.h"
#include "core/log.h"

namespace coop::online {
namespace {
EOS_HPlatform g_platform = nullptr;
EOS_HConnect g_connect = nullptr;
EOS_HLobby g_lobbies = nullptr;
EOS_HP2P g_p2p = nullptr;
EOS_ProductUserId g_user = nullptr;
std::vector<EOS_ProductUserId> g_peers;
EOS_NotificationId g_request_notify = EOS_INVALID_NOTIFICATIONID;
EOS_NotificationId g_auth_notify = EOS_INVALID_NOTIFICATIONID;
EOS_NotificationId g_member_notify = EOS_INVALID_NOTIFICATIONID;
EOS_P2P_SocketId g_socket{};
bool g_active = false, g_host = false, g_ready = false;
uint64_t g_generation = 0, g_members_at = 0, g_started_at = 0;
uint64_t g_retry_at = 0;
std::string g_lobby, g_code, g_build, g_status = "ONLINE OFF", g_error;
constexpr const char* kCodeAttribute = "join_code";
constexpr uint8_t kChannel = 0;
struct Config {
    std::string product, sandbox, deployment, client, secret, name, cache;
    bool force_relay = false;
} g_config;

uint64_t Now() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct Operation {
    uint64_t generation = g_generation;
    EOS_HLobbySearch search = nullptr;
    EOS_HLobbyDetails details = nullptr;
    ~Operation() {
        if (search) EOS_LobbySearch_Release(search);
        if (details) EOS_LobbyDetails_Release(details);
    }
    bool Current() const { return g_active && generation == g_generation; }
};

void Fail(const char* action, EOS_EResult result) {
    if (result == EOS_EResult::EOS_NotFound)
        g_error = "Room not found or already closed. Ask the host for a new code.";
    else if (result == EOS_EResult::EOS_Lobby_TooManyPlayers)
        g_error = "This room already has eight players.";
    else
        g_error = std::string(action) + ": " + EOS_EResult_ToString(result);
    g_status = "ONLINE ERROR";
    g_ready = false;
    LOGR("coop online: %s: %s", action, EOS_EResult_ToString(result));
}

void ForgetLobby(const std::string& id, bool host) {
    if (id.empty()) return;
    if (host) {
        EOS_Lobby_DestroyLobbyOptions options{};
        options.ApiVersion = EOS_LOBBY_DESTROYLOBBY_API_LATEST;
        options.LocalUserId = g_user;
        options.LobbyId = id.c_str();
        EOS_Lobby_DestroyLobby(g_lobbies, &options, nullptr, [](const EOS_Lobby_DestroyLobbyCallbackInfo*) {});
    } else {
        EOS_Lobby_LeaveLobbyOptions options{};
        options.ApiVersion = EOS_LOBBY_LEAVELOBBY_API_LATEST;
        options.LocalUserId = g_user;
        options.LobbyId = id.c_str();
        EOS_Lobby_LeaveLobby(g_lobbies, &options, nullptr, [](const EOS_Lobby_LeaveLobbyCallbackInfo*) {});
    }
}

void ClosePeer(EOS_ProductUserId peer) {
    if (g_p2p && g_user && peer) {
        EOS_P2P_CloseConnectionOptions options{};
        options.ApiVersion = EOS_P2P_CLOSECONNECTION_API_LATEST;
        options.LocalUserId = g_user;
        options.RemoteUserId = peer;
        options.SocketId = &g_socket;
        EOS_P2P_CloseConnection(g_p2p, &options);
    }
}
bool Allowed(EOS_ProductUserId peer) { return std::find(g_peers.begin(), g_peers.end(), peer) != g_peers.end(); }

void RefreshMembers() {
    if (!g_active || !g_ready || g_lobby.empty()) return;
    EOS_Lobby_CopyLobbyDetailsHandleOptions options{};
    options.ApiVersion = EOS_LOBBY_COPYLOBBYDETAILSHANDLE_API_LATEST;
    options.LocalUserId = g_user;
    options.LobbyId = g_lobby.c_str();
    EOS_HLobbyDetails details = nullptr;
    auto result = EOS_Lobby_CopyLobbyDetailsHandle(g_lobbies, &options, &details);
    if (result != EOS_EResult::EOS_Success) { Fail("Lobby closed", result); return; }
    EOS_LobbyDetails_GetMemberCountOptions count_options{};
    count_options.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERCOUNT_API_LATEST;
    uint32_t count = EOS_LobbyDetails_GetMemberCount(details, &count_options);
    std::vector<EOS_ProductUserId> members;
    EOS_LobbyDetails_GetLobbyOwnerOptions owner_options{};
    owner_options.ApiVersion = EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST;
    auto owner = EOS_LobbyDetails_GetLobbyOwner(details, &owner_options);
    for (uint32_t i = 0; i < count; i++) {
        EOS_LobbyDetails_GetMemberByIndexOptions member_options{};
        member_options.ApiVersion = EOS_LOBBYDETAILS_GETMEMBERBYINDEX_API_LATEST;
        member_options.MemberIndex = i;
        auto member = EOS_LobbyDetails_GetMemberByIndex(details, &member_options);
        if (member && member != g_user && (g_host || member == owner)) members.push_back(member);
    }
    EOS_LobbyDetails_Release(details);
    for (auto peer : g_peers) if (std::find(members.begin(), members.end(), peer) == members.end()) ClosePeer(peer);
    g_peers = std::move(members);
    g_status = !g_peers.empty() ? "ONLINE CONNECTING" : "ONLINE WAITING FOR FRIENDS";
}

void AcceptPeer(const EOS_P2P_OnIncomingConnectionRequestInfo* info) {
    if (!g_active || !g_ready || info->LocalUserId != g_user ||
        std::strcmp(info->SocketId->SocketName, g_socket.SocketName)) return;
    RefreshMembers(); // hosts accept lobby members; clients connect only to the lobby owner
    if (!Allowed(info->RemoteUserId)) return;
    EOS_P2P_AcceptConnectionOptions options{};
    options.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
    options.LocalUserId = g_user;
    options.RemoteUserId = info->RemoteUserId;
    options.SocketId = &g_socket;
    auto result = EOS_P2P_AcceptConnection(g_p2p, &options);
    if (result != EOS_EResult::EOS_Success) Fail("Accept connection", result);
}

void BeginLobby();
void Login();

void LoggedIn(EOS_ProductUserId user) {
    g_user = user;
    if (g_ready || !g_lobby.empty()) return; // token refresh preserves the match
    BeginLobby();
}

void Login() {
    if (!g_active) return;
    EOS_Connect_Credentials credentials{};
    credentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
    credentials.Type = EOS_EExternalCredentialType::EOS_ECT_DEVICEID_ACCESS_TOKEN;
    EOS_Connect_UserLoginInfo user_info{};
    user_info.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
    user_info.DisplayName = g_config.name.c_str();
    EOS_Connect_LoginOptions options{};
    options.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
    options.Credentials = &credentials;
    options.UserLoginInfo = &user_info;
    EOS_Connect_Login(g_connect, &options, new Operation, [](const EOS_Connect_LoginCallbackInfo* info) {
        std::unique_ptr<Operation> operation(static_cast<Operation*>(info->ClientData));
        if (!operation->Current()) return;
        if (info->ResultCode == EOS_EResult::EOS_Success) { LoggedIn(info->LocalUserId); return; }
        if (info->ResultCode != EOS_EResult::EOS_InvalidUser) { Fail("Device login", info->ResultCode); return; }
        EOS_Connect_CreateUserOptions create{};
        create.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
        create.ContinuanceToken = info->ContinuanceToken;
        EOS_Connect_CreateUser(g_connect, &create, new Operation, [](const EOS_Connect_CreateUserCallbackInfo* created) {
            std::unique_ptr<Operation> operation(static_cast<Operation*>(created->ClientData));
            if (!operation->Current()) return;
            if (created->ResultCode != EOS_EResult::EOS_Success) { Fail("Create player", created->ResultCode); return; }
            LoggedIn(created->LocalUserId);
        });
    });
}

bool Initialize() {
    if (g_platform) {
        if (g_auth_notify == EOS_INVALID_NOTIFICATIONID || g_member_notify == EOS_INVALID_NOTIFICATIONID) {
            g_error = "Could not monitor the online connection. Restart the game and try again.";
            return false;
        }
        return true;
    }
    auto path = app::ExeDir() / "coop-online.toml";
    std::string override_path;
    if (net::Arg("--coop-eos-config", override_path) && !override_path.empty()) path = override_path;
    try {
        auto table = toml::parse_file(path.string());
        g_config.product = table["product_id"].value_or(std::string{});
        g_config.sandbox = table["sandbox_id"].value_or(std::string{});
        g_config.deployment = table["deployment_id"].value_or(std::string{});
        g_config.client = table["client_id"].value_or(std::string{});
        g_config.secret = table["client_secret"].value_or(std::string{});
        g_config.name = coop::GetSession().nickname;
        g_config.force_relay = table["force_relay"].value_or(false);
    } catch (...) {
        g_error = "Create coop-online.toml beside BLACK.exe using config/coop-online.example.toml.";
        return false;
    }
    for (const auto* field : {&g_config.product, &g_config.sandbox, &g_config.deployment, &g_config.client, &g_config.secret}) {
        if (field->empty() || field->find_first_of("<>\r\n") != std::string::npos) {
            g_error = "Fill all EOS product and game-client credentials in coop-online.toml.";
            return false;
        }
    }
    if (g_config.name.empty()) g_config.name = "Player";
    g_config.name.resize(std::min<size_t>(g_config.name.size(), 32));
    EOS_InitializeOptions initialize{};
    initialize.ApiVersion = EOS_INITIALIZE_API_LATEST;
    initialize.ProductName = "BlackCoop";
    initialize.ProductVersion = "0.1";
    auto result = EOS_Initialize(&initialize);
    if (result != EOS_EResult::EOS_Success) { Fail("Initialize SDK", result); return false; }
    auto cache = app::ExeDir() / ("hdd" + (app::Instance() ? std::to_string(app::Instance()) : std::string{})) / "eos";
    std::error_code error;
    std::filesystem::create_directories(cache, error);
    g_config.cache = cache.string();
    EOS_Platform_Options platform{};
    platform.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
    platform.ProductId = g_config.product.c_str();
    platform.SandboxId = g_config.sandbox.c_str();
    platform.DeploymentId = g_config.deployment.c_str();
    platform.ClientCredentials.ClientId = g_config.client.c_str();
    platform.ClientCredentials.ClientSecret = g_config.secret.c_str();
    platform.Flags = EOS_PF_DISABLE_OVERLAY;
    platform.CacheDirectory = g_config.cache.c_str();
    platform.TickBudgetInMilliseconds = 2;
    g_platform = EOS_Platform_Create(&platform);
    if (!g_platform) {
        g_error = "EOS platform initialization failed. Check product/client configuration.";
        EOS_Shutdown();
        return false;
    }
    g_connect = EOS_Platform_GetConnectInterface(g_platform);
    g_lobbies = EOS_Platform_GetLobbyInterface(g_platform);
    g_p2p = EOS_Platform_GetP2PInterface(g_platform);
    g_socket.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
    std::strcpy(g_socket.SocketName, "blackcoop");
    EOS_P2P_SetRelayControlOptions relay{};
    relay.ApiVersion = EOS_P2P_SETRELAYCONTROL_API_LATEST;
    relay.RelayControl = g_config.force_relay ? EOS_ERelayControl::EOS_RC_ForceRelays : EOS_ERelayControl::EOS_RC_AllowRelays;
    EOS_P2P_SetRelayControl(g_p2p, &relay);
    EOS_Connect_AddNotifyAuthExpirationOptions auth{};
    auth.ApiVersion = EOS_CONNECT_ADDNOTIFYAUTHEXPIRATION_API_LATEST;
    g_auth_notify = EOS_Connect_AddNotifyAuthExpiration(g_connect, &auth, nullptr,
        [](const EOS_Connect_AuthExpirationCallbackInfo*) { Login(); });
    EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions members{};
    members.ApiVersion = EOS_LOBBY_ADDNOTIFYLOBBYMEMBERSTATUSRECEIVED_API_LATEST;
    g_member_notify = EOS_Lobby_AddNotifyLobbyMemberStatusReceived(g_lobbies, &members, nullptr,
        [](const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* info) {
            if (!g_active || g_lobby.empty() || g_lobby != info->LobbyId) return;
            auto status = info->CurrentStatus;
            if (status == EOS_ELobbyMemberStatus::EOS_LMS_CLOSED ||
                (info->TargetUserId == g_user && (status == EOS_ELobbyMemberStatus::EOS_LMS_KICKED ||
                                                status == EOS_ELobbyMemberStatus::EOS_LMS_DISCONNECTED))) {
                g_error = "The online room closed or disconnected. Ask the host for a new room.";
                g_ready = false;
            } else RefreshMembers();
        });
    if (g_auth_notify == EOS_INVALID_NOTIFICATIONID || g_member_notify == EOS_INVALID_NOTIFICATIONID) {
        g_error = "Could not monitor the online connection. Restart the game and try again.";
        return false;
    }
    return true;
}

void BeginLobby() {
    if (g_request_notify == EOS_INVALID_NOTIFICATIONID) {
        EOS_P2P_AddNotifyPeerConnectionRequestOptions notify{};
        notify.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
        notify.LocalUserId = g_user;
        notify.SocketId = &g_socket;
        g_request_notify = EOS_P2P_AddNotifyPeerConnectionRequest(g_p2p, &notify, nullptr, AcceptPeer);
        if (g_request_notify == EOS_INVALID_NOTIFICATIONID) {
            g_error = "Could not listen for your friend's connection. Restart the game and try again.";
            return;
        }
    }
    if (g_host) {
        g_status = "ONLINE CREATING ROOM";
        EOS_Lobby_CreateLobbyOptions options{};
        options.ApiVersion = EOS_LOBBY_CREATELOBBY_API_LATEST;
        options.LocalUserId = g_user;
        options.MaxLobbyMembers = coop::kMaxPlayers;
        options.PermissionLevel = EOS_ELobbyPermissionLevel::EOS_LPL_PUBLICADVERTISED;
        options.BucketId = g_build.c_str();
        options.bDisableHostMigration = EOS_TRUE;
        EOS_Lobby_CreateLobby(g_lobbies, &options, new Operation, [](const EOS_Lobby_CreateLobbyCallbackInfo* info) {
            std::unique_ptr<Operation> operation(static_cast<Operation*>(info->ClientData));
            if (!operation->Current()) {
                if (info->ResultCode == EOS_EResult::EOS_Success) ForgetLobby(info->LobbyId, true);
                return;
            }
            if (info->ResultCode != EOS_EResult::EOS_Success) { Fail("Create room", info->ResultCode); return; }
            g_lobby = info->LobbyId;
            EOS_Lobby_UpdateLobbyModificationOptions modify{};
            modify.ApiVersion = EOS_LOBBY_UPDATELOBBYMODIFICATION_API_LATEST;
            modify.LocalUserId = g_user;
            modify.LobbyId = g_lobby.c_str();
            EOS_HLobbyModification handle = nullptr;
            auto result = EOS_Lobby_UpdateLobbyModification(g_lobbies, &modify, &handle);
            if (result != EOS_EResult::EOS_Success) { Fail("Configure room", result); return; }
            EOS_Lobby_AttributeData attribute{};
            attribute.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
            attribute.Key = kCodeAttribute;
            attribute.ValueType = EOS_EAttributeType::EOS_AT_STRING;
            attribute.Value.AsUtf8 = g_code.c_str();
            EOS_LobbyModification_AddAttributeOptions add{};
            add.ApiVersion = EOS_LOBBYMODIFICATION_ADDATTRIBUTE_API_LATEST;
            add.Attribute = &attribute;
            add.Visibility = EOS_ELobbyAttributeVisibility::EOS_LAT_PUBLIC;
            result = EOS_LobbyModification_AddAttribute(handle, &add);
            if (result != EOS_EResult::EOS_Success) {
                EOS_LobbyModification_Release(handle);
                Fail("Set room code", result); return;
            }
            EOS_Lobby_UpdateLobbyOptions update{};
            update.ApiVersion = EOS_LOBBY_UPDATELOBBY_API_LATEST;
            update.LobbyModificationHandle = handle;
            EOS_Lobby_UpdateLobby(g_lobbies, &update, new Operation, [](const EOS_Lobby_UpdateLobbyCallbackInfo* updated) {
                std::unique_ptr<Operation> operation(static_cast<Operation*>(updated->ClientData));
                if (!operation->Current()) return;
                if (updated->ResultCode != EOS_EResult::EOS_Success) { Fail("Publish room", updated->ResultCode); return; }
                g_ready = true;
                RefreshMembers();
            });
            EOS_LobbyModification_Release(handle);
        });
    } else {
        g_status = "ONLINE FINDING ROOM";
        auto operation = std::make_unique<Operation>();
        EOS_Lobby_CreateLobbySearchOptions options{};
        options.ApiVersion = EOS_LOBBY_CREATELOBBYSEARCH_API_LATEST;
        options.MaxResults = 4;
        auto result = EOS_Lobby_CreateLobbySearch(g_lobbies, &options, &operation->search);
        if (result != EOS_EResult::EOS_Success) { Fail("Search room", result); return; }
        for (const auto& [key, value] : {std::pair{kCodeAttribute, g_code.c_str()},
                                      std::pair{EOS_LOBBY_SEARCH_BUCKET_ID, g_build.c_str()}}) {
            EOS_Lobby_AttributeData attribute{};
            attribute.ApiVersion = EOS_LOBBY_ATTRIBUTEDATA_API_LATEST;
            attribute.Key = key;
            attribute.ValueType = EOS_EAttributeType::EOS_AT_STRING;
            attribute.Value.AsUtf8 = value;
            EOS_LobbySearch_SetParameterOptions parameter{};
            parameter.ApiVersion = EOS_LOBBYSEARCH_SETPARAMETER_API_LATEST;
            parameter.Parameter = &attribute;
            parameter.ComparisonOp = EOS_EComparisonOp::EOS_CO_EQUAL;
            result = EOS_LobbySearch_SetParameter(operation->search, &parameter);
            if (result != EOS_EResult::EOS_Success) { Fail("Search parameters", result); return; }
        }
        EOS_LobbySearch_FindOptions find{};
        find.ApiVersion = EOS_LOBBYSEARCH_FIND_API_LATEST;
        find.LocalUserId = g_user;
        auto search = operation->search;
        EOS_LobbySearch_Find(search, &find, operation.release(), [](const EOS_LobbySearch_FindCallbackInfo* info) {
            std::unique_ptr<Operation> operation(static_cast<Operation*>(info->ClientData));
            if (!operation->Current()) return;
            if (info->ResultCode != EOS_EResult::EOS_Success) { Fail("Find room", info->ResultCode); return; }
            EOS_LobbySearch_GetSearchResultCountOptions count{};
            count.ApiVersion = EOS_LOBBYSEARCH_GETSEARCHRESULTCOUNT_API_LATEST;
            uint32_t found = EOS_LobbySearch_GetSearchResultCount(operation->search, &count);
            if (!found && Now() - g_started_at < 15000) {
                // A newly published room can take a moment to enter the search index.
                g_retry_at = Now() + 2000;
                return;
            }
            if (found != 1) {
                g_error = found ? "Room code is ambiguous. Ask the host to create another room."
                                : "Room not found. Check the code and use the same build and EOS configuration.";
                return;
            }
            EOS_LobbySearch_CopySearchResultByIndexOptions copy{};
            copy.ApiVersion = EOS_LOBBYSEARCH_COPYSEARCHRESULTBYINDEX_API_LATEST;
            auto result = EOS_LobbySearch_CopySearchResultByIndex(operation->search, &copy, &operation->details);
            if (result != EOS_EResult::EOS_Success) { Fail("Read room", result); return; }
            EOS_LobbyDetails_GetLobbyOwnerOptions owner{};
            owner.ApiVersion = EOS_LOBBYDETAILS_GETLOBBYOWNER_API_LATEST;
            if (EOS_LobbyDetails_GetLobbyOwner(operation->details, &owner) == g_user) {
                g_error = "This device already owns that room. Join from your friend's PC; use LAN for copies on one PC.";
                return;
            }
            EOS_Lobby_JoinLobbyOptions join{};
            join.ApiVersion = EOS_LOBBY_JOINLOBBY_API_LATEST;
            join.LocalUserId = g_user;
            join.LobbyDetailsHandle = operation->details;
            g_status = "ONLINE JOINING ROOM";
            EOS_Lobby_JoinLobby(g_lobbies, &join, operation.release(), [](const EOS_Lobby_JoinLobbyCallbackInfo* joined) {
                std::unique_ptr<Operation> operation(static_cast<Operation*>(joined->ClientData));
                if (!operation->Current()) {
                    if (joined->ResultCode == EOS_EResult::EOS_Success) ForgetLobby(joined->LobbyId, false);
                    return;
                }
                if (joined->ResultCode != EOS_EResult::EOS_Success) { Fail("Join room", joined->ResultCode); return; }
                g_lobby = joined->LobbyId;
                g_ready = true;
                RefreshMembers();
            });
        });
    }
}
} // namespace

bool Built() { return true; }
bool Pumping() { return g_platform != nullptr; }
bool Active() { return g_active; }
std::string Error() { return g_error; }
std::string Code() { return g_ready ? g_code : std::string{}; }
std::string Status() { return g_status; }
net::Peer Partner() { return {0, 0, g_peers.empty() ? 0 : uint64_t(reinterpret_cast<uintptr_t>(g_peers.front()))}; }

bool Start(bool host, const std::string& code, const std::string& build) {
    Stop();
    g_error.clear();
    g_host = host;
    g_build = build;
    g_code.clear();
    if (host) {
        constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        std::random_device random;
        for (int i = 0; i < 8; i++) g_code += alphabet[random() % (sizeof alphabet - 1)];
    } else {
        for (unsigned char c : code) {
            if (c == ' ' || c == '-') continue;
            if (!std::isalnum(c) || c > 127) { g_error = "Enter the host's eight-character room code."; return false; }
            g_code += char(std::toupper(c));
        }
        if (g_code.size() != 8) { g_error = "Enter the host's eight-character room code."; return false; }
    }
    if (!Initialize()) return false;
    g_active = true;
    g_started_at = Now();
    g_status = "ONLINE CONNECTING";
    // Refresh even after a long time offline: an expiration notification while
    // stopped must not leave the next room using an expired access token.
    if (g_user) Login();
    else {
        EOS_Connect_CreateDeviceIdOptions options{};
        options.ApiVersion = EOS_CONNECT_CREATEDEVICEID_API_LATEST;
        options.DeviceModel = "Windows PC";
        EOS_Connect_CreateDeviceId(g_connect, &options, new Operation, [](const EOS_Connect_CreateDeviceIdCallbackInfo* info) {
            std::unique_ptr<Operation> operation(static_cast<Operation*>(info->ClientData));
            if (!operation->Current()) return;
            if (info->ResultCode != EOS_EResult::EOS_Success && info->ResultCode != EOS_EResult::EOS_DuplicateNotAllowed) {
                Fail("Create device identity", info->ResultCode); return;
            }
            Login();
        });
    }
    return true;
}

void Stop() {
    g_active = g_ready = false;
    ++g_generation; // outstanding SDK callbacks can finish, but cannot resurrect a cancelled session
    for (auto peer : g_peers) ClosePeer(peer);
    g_peers.clear();
    ForgetLobby(g_lobby, g_host);
    g_lobby.clear();
    g_code.clear();
    g_status = "ONLINE OFF";
    g_members_at = 0;
    g_retry_at = 0;
}

void Tick() {
    if (!g_platform) return;
    EOS_Platform_Tick(g_platform);
    if (!g_active || !g_error.empty()) return;
    uint64_t now = Now();
    if (!g_ready && now - g_started_at > 45000) { Fail("Online setup timed out", EOS_EResult::EOS_TimedOut); return; }
    if (g_retry_at && now >= g_retry_at) { g_retry_at = 0; BeginLobby(); }
    if (g_ready && now - g_members_at >= 500) { g_members_at = now; RefreshMembers(); }
}

bool Send(uint64_t peer, const void* data, int bytes) {
    auto remote = reinterpret_cast<EOS_ProductUserId>(uintptr_t(peer));
    if (!g_active || !g_ready || !Allowed(remote) || bytes <= 0 || bytes > EOS_P2P_MAX_PACKET_SIZE) return false;
    EOS_P2P_SendPacketOptions options{};
    options.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;
    options.LocalUserId = g_user;
    options.RemoteUserId = remote;
    options.SocketId = &g_socket;
    options.Channel = kChannel;
    options.DataLengthBytes = uint32_t(bytes);
    options.Data = data;
    options.bAllowDelayedDelivery = EOS_TRUE;
    options.Reliability = EOS_EPacketReliability::EOS_PR_UnreliableUnordered;
    return EOS_P2P_SendPacket(g_p2p, &options) == EOS_EResult::EOS_Success;
}

int Receive(net::Peer& peer, void* data, int cap) {
    if (!g_active || !g_ready || g_peers.empty() || cap <= 0) return -1;
    for (int i = 0; i < 32; i++) {
        EOS_P2P_ReceivePacketOptions options{};
        options.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
        options.LocalUserId = g_user;
        options.MaxDataSizeBytes = uint32_t(cap);
        options.RequestedChannel = &kChannel;
        EOS_ProductUserId sender = nullptr;
        EOS_P2P_SocketId socket{};
        socket.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
        uint8_t channel = 0;
        uint32_t bytes = 0;
        auto result = EOS_P2P_ReceivePacket(g_p2p, &options, &sender, &socket, &channel, data, &bytes);
        if (result != EOS_EResult::EOS_Success) return -1;
        if (!Allowed(sender) || std::strcmp(socket.SocketName, g_socket.SocketName) || channel != kChannel) continue;
        peer = {0, 0, uint64_t(reinterpret_cast<uintptr_t>(sender))};
        return int(bytes);
    }
    return -1;
}

std::string PeerName(uint64_t peer) {
    char name[EOS_PRODUCTUSERID_MAX_LENGTH + 1]{};
    int32_t length = sizeof name;
    auto id = reinterpret_cast<EOS_ProductUserId>(uintptr_t(peer));
    return EOS_ProductUserId_ToString(id, name, &length) == EOS_EResult::EOS_Success ? std::string{"EOS:"} + name : "EOS";
}
} // namespace coop::online
#endif
