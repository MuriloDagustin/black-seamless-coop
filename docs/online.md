# Online co-op with EOS (optional)

This integration supports **up to eight players**, including the host. A player
still hosts the simulation on their PC. EOS supplies lobby discovery and P2P
connectivity, with its relay available when a direct connection cannot be made.
Players do not need a virtual LAN application or a separately hosted game server.

**Status:** compiled with the official EOS C SDK 1.19.2.1 on Windows. Device ID
login, real room creation and discovery were tested against EOS. LAN mission
joining, shared objectives, locomotion and completion passed regression checks
in the EOS-enabled build. Eight simultaneous LAN copies passed automatic mission
joining, seven distinct remote bodies each, shooting from every player, shared
and simultaneous objectives, full-room rejection, rejoining, completion and a
map/difficulty change. Walking/running/idle clips also passed a UDP proxy test
delivering movement states in 180 ms bursts. Eight-player EOS matches and
direct/relayed connectivity between distinct PCs
is still untested; a successful lobby lookup does not validate P2P transport.
A normal build without the SDK keeps LAN/direct IP and shows online as unavailable.
Do not treat a successful LAN test as validation of EOS or relay connectivity.

## Product setup (your Epic developer account)

1. Confirm with Epic that your intended use of an independently developed co-op
   extension/recompilation is eligible. Do not describe yourself as the owner or
   publisher of the original BLACK. Personal/noncommercial use alone does not
   settle the developer agreement's rights requirements.
2. In the [Developer Portal](https://dev.epicgames.com/portal/), create your
   organization and product, and select the sandbox and deployment for your tests.
3. Create a **game client** and a custom client policy with **User required**
   enabled. Under **Lobbies**, enable `connect`, `readLobby` and `findLobbies`.
   This policy does not need Sessions or administrative Connect lookup actions.
   The portal does not expose a separate P2P feature switch. Do not use a
   trusted-server or administrative client in the executable.
4. Download the **C SDK for Windows** from that portal and extract it locally, for
   example to `C:/dev/eos-sdk`. This is an optional dependency, separate from the
   Xbox recompilation SDK. Do not commit the EOS SDK or its redistributable files.
5. Copy `config/coop-online.example.toml` beside `BLACK.exe` as
   `coop-online.toml`, then fill the five product/client fields from your portal.
   Keep this local file out of Git. Use the same product, sandbox, deployment and
   game-client configuration on all PCs. Choose each player's nickname in
   **F10 → CO-OP → Nickname**; this is saved separately in `black.toml`.

This prototype uses **EOS Connect Device ID** for the player identity, with no
Epic friends list, account overlay, store integration, voice chat or anti-cheat.
Epic Account Services login/friends and its brand review are separate features.
**User required** means an authenticated product user, including an automatic
Device ID user; it does not require signing into an Epic Games account. Internal
Connect authentication is still required by EOS Lobbies and P2P.
Two processes under the same Windows account were observed to share their Device
ID. Joining your own device's room is rejected with a message; use LAN for two
copies on one PC. The intended online test is on two distinct PCs.

## Build

Add `-DBLACK_EOS_SDK=C:/dev/eos-sdk` to the existing CMake configuration, then build.
The directory must contain `SDK/Include/eos_sdk.h`,
`SDK/Lib/EOSSDK-Win64-Shipping.lib` and `SDK/Bin/EOSSDK-Win64-Shipping.dll` (or the
equivalent `Include`, `Lib`, `Bin` paths directly under the selected directory).
CMake copies the EOS DLL beside the executable. Distribute the components under
the SDK's terms, including any notices they require.

Omit `BLACK_EOS_SDK` for the standard build without EOS. Merely copying the DLL
beside a standard executable does not enable the integration.

## Play

1. Host: open **F10 → CO-OP → Create online room**. Wait for the eight-character
   room code to appear, then send that code privately to your friend.
2. Partner: **F10 → CO-OP → Join by code**. Both players need the same game image,
   co-op protocol/build and EOS product configuration.
3. Once the existing game handshake finishes, the partner follows the host's map,
   difficulty and objective progress automatically.
4. **Leave / stop server** cancels pending setup as well as an established room.
   Starting the executable never reconnects automatically.

The room has eight slots. The host keeps the physical P2P connections and routes
client-to-client updates; clients only need a connection to the host. Codes are
random lobby search attributes, not passwords
or an authorization system for a public matchmaking service. A closed host room
does not migrate the running BLACK mission to the other player. Join again after
the host creates another room.

For a friend's existing game installation, copy the same `BLACK.exe`,
`EOSSDK-Win64-Shipping.dll`, restricted `coop-online.toml`, and the SDK's third-party
notices (locally copied to `EOS-ThirdPartyNotices`) next to their normal game
files. Each player supplies their own game assets. Keep product credentials
out of public repositories; never include developer/admin credentials.

Optional command line: `--coop-online host` or `--coop-online ABCD2345`.
`--coop-eos-config C:/path/coop-online.toml` selects a local configuration file.
The ordinary `--coop host` / `--coop join --join <IP>` still use UDP/direct IP.

## Validation before calling it ready

- Compile with the actual SDK, then check SDK login and all callback cleanup paths.
- Use two PCs on different internet connections to create/join a room.
- Set `force_relay = true` on both test PCs to validate relayed connectivity;
  then use `false` for normal direct-or-relayed connections.
- Check a wrong/full/closed room, a different protocol/image, cancellation during
  login/search/join, host departure and a new room after leaving.
- Run the existing objective, locomotion, auto-mission and completion checks.
- Keep automated game copies hidden, with `--mute`, and use UDP test controls.

References: [EOS getting started](https://dev.epicgames.com/docs/epic-online-services/eos-get-started/eos-get-started-reference),
[client policies](https://dev.epicgames.com/docs/epic-online-services/eos-fundamentals/client-and-client-policy/client-policy-guide),
[Connect](https://dev.epicgames.com/docs/epic-online-services/eos-fundamentals/connect-interface),
[P2P](https://dev.epicgames.com/docs/epic-online-services/multiplayer/nat-p2p-interface),
[developer agreement](https://onlineservices.epicgames.com/services/terms/agreements).
