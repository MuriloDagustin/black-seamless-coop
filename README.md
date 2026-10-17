# Black Seamless Co-op

Networked two-player co-op for **Black** (Criterion, 2006), built as a mod for the
unofficial native PC port [TGP482/BLACK-XBERecomp](https://github.com/TGP482/BLACK-XBERecomp),
a static recompilation of the original Xbox game made with
[XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp).

This repository holds **only the co-op code**: new source files and a small patch. It has no game data
and no code from the port or the recompiler. `setup.bat` fetches the port and the SDK at the exact
versions this mod was made with, and puts the mod on top. You need your own copy of the game.

> Work in progress. Black never had multiplayer; everything here is reverse engineered from the game's
> recompiled code, so expect rough edges.

## What works

| | |
|---|---|
| Connection | UDP, host and join, reconnects when either side restarts |
| Partner marker | the partner's position drawn over the frame (`P2 12`, arrows at the edges) |
| Partner's body | each copy shows the partner as a soldier walking where the partner walks and looking where it looks |
| Partner bullets | the partner's shots are fired again on your copy through the game's own bullet code: impacts, decals, glass, destruction and damage to enemies, with a tracer, the partner's muzzle flash and a 3D gunshot. They never hit you or touch your ammo or HUD |
| Enemies | the host runs them: on the joining copy each enemy is a puppet of the host's (position, animation, aim, fire, reloads, health and death). They see, hear and shoot the joining player's body, so the joining player is hit by them |
| Enemy deaths | an enemy killed on one copy dies on the other, ragdoll and scripts included |
| Props, grenades, rockets | crates, barrels and breakable scenery break on both copies; the partner's grenades and rockets fly and explode on yours |
| Triggers | one-shot area triggers (enemy waves, scripted scenes) fire for either player's position |
| Level flow | the game moves on to the next area only when both players have gone through; the one ahead waits at the next door (a LOADING screen) until the other comes. Mission failed and level complete count for both |
| Get me out of here | **F8**, or **L3 + R3** held for a second on a controller: you are put next to your partner, on the way it just walked |
| Test channel | `--coop-control`: a local text channel that drives a copy without its window's focus (pad, aim, teleport, state), for automated two-copy tests |

Not yet: retrying a failed mission together (each player picks Continue on its own copy),
cinematics in sync, vehicles.

## Setup

Requirements: Windows 10/11 64-bit, [git](https://git-scm.com/download/win),
Visual Studio 2022 or later with *Desktop development with C++*, *C++ Clang tools* and *C++ CMake tools*,
and Python 3 if you set up from a disc image.

```
setup.bat [folder] [Black.iso]
```

1. `setup.bat` clones the port into `black-recomp` (or the folder you name), the SDK into its `sdk`
   folder, applies `patches/upstream.patch` and copies `src/game/coop*`.
2. Give it your disc image (USA release) and it extracts it into the port's `assets` folder with
   `tools/xiso_extract.py`; or put the extracted files there yourself (`default.xbe`, `GlobData.bin`
   and the `chars`, `data`, `export`, `language`, `levels`, `sound`, `videos` folders).
3. Run the port's `build.bat`. The first build recompiles the game and takes a while.
   The game is `out\build\release\BLACK.exe`.

Both players need this same build.

## Playing

```
BLACK.exe --coop host                      the host, UDP 45410
BLACK.exe --coop join --join 192.168.0.10  the partner, the host's address
```

Both load the same level, ideally from the same checkpoint. Over the internet the host forwards
UDP 45410 and allows BLACK.exe through the Windows firewall.

On one PC, for testing, set `display_mode = 'windowed'` in `black.toml` beside the exe and run:

```
BLACK.exe --instance 1 --coop host
BLACK.exe --instance 2 --coop join --join 127.0.0.1 --mute
```

| Option | |
|---|---|
| `--coop host` / `--coop join` | role, for this copy only |
| `--join <address>` | the host to join (or `join_ip` in `black.toml`) |
| `--coop-port <n>` | the host's UDP port (or `coop_port`), 45410 by default |
| `--coop-world <n>` | world sync: 6 everything (default), 5 no level flow, 4 no partner's body, 3 no host-run enemies, 2 deaths and triggers, 1 deaths only, 0 off |
| `--instance <n>` | a second copy on one PC: its own saves (`hddN`) and log |
| `--mute` | no sound |
| `--coop-probe` | on-screen readout: position, shot and world event counters |
| `--coop-echo` | alone: your own position and shots come back 3 s later, to see the partner side work |
| `--coop-control [port]` | test channel on 127.0.0.1 (45500 + instance): this copy plays the pad it is sent and reads no mouse, keyboard or controller |

### Test channel

One command per UDP datagram, one reply each: `ping`, `state`, `npcs`, `page`, `triggers [all]`,
`pad <a,b,x,y,lt,rt,up,down,left,right,start,back,...> [ms]`, `hold <names>`, `stick <lx> <ly> <rx> <ry> [ms]`,
`release`, `yaw`/`pitch`/`turn <deg>`, `aim <x> <y> <z>` / `aim nearest` / `aim off`,
`teleport <x> <y> <z>` / `teleport partner`, `rescue` (as F8), `stand <trigger> [frames]`,
`triggers units`, `props [n]`, `rocket`, `level <n>` (the next load), `flow fail|complete`,
`peek`/`peekf <addr> [n]`, `pokef <addr> <floats>`, `log <text>`. Addresses take `@player+hex` and `@world+hex`.

`black.toml` also takes `coop = 1` (host) or `2` (join) to make a role the default.

## How it works

The port lets host code replace any recompiled function at link time. The mod wraps:

- the frame function `0x20F6A0`: packets in before the frame, the player's state out after it;
- the player's bullet spawn `0x1DB7B0` and trigger pull `0x1DBD50`: each pellet goes to the partner,
  whose copy spawns it again with a weapon of the same type owned by its own player, in a bullet pool
  of its own;
- the character spawn `0x1DF440` and the death paths `0x205C60`, `0x1DA240`, `0x1DA1A0`: an enemy is
  named by its spawner's 64-bit record name, which is the same on both copies; the partner's deaths are
  played with the game's own death function;
- the logic tick `0x1E7FF0`: one-shot triggers are tested again at the partner's position;
- the AI think `0x1E5F70` and the characters' update `0x1CCBB0`: on the joining copy the host's enemies are
  driven from the host's snapshots (30 a second) instead of their own AI;
- the ally spawn `0x1AF880`: the partner's body is an ally made the way the game makes a squad mate, and the
  enemies' perception `0x1AD440` sees it in an empty squad slot;
- the unit advance `0x1B73B0`, the next unit's activation `0x1C7620` and the in-level state's requests
  `0x1F45C0`/`0x1F45B0`/`0x1F4590`: the level flow for both players.

## Português

Co-op em rede para dois jogadores no **Black**, feito como mod da port para PC
[BLACK-XBERecomp](https://github.com/TGP482/BLACK-XBERecomp). Este repositório tem só o código do co-op;
o `setup.bat` baixa a port e o SDK nas versões testadas e aplica o mod. Você precisa da sua própria cópia
do jogo. Funciona hoje: conexão, corpo e marcador do parceiro, balas do parceiro com impacto, dano, clarão
e estampido, inimigos controlados pelo host (que enxergam e atiram no parceiro), mortes, objetos
destrutíveis, granadas e foguetes sincronizados, gatilhos de área disparados por qualquer um dos dois,
troca de área esperando os dois jogadores, missão falhada e fim de fase valendo para ambos, e
"me tira daqui": **F8** ou **L3 + R3** segurados por um segundo te levam para perto do parceiro.

## License

The code in this repository is MIT licensed (see `LICENSE`). It applies to this repository's files only:
the port, the recompiler and the game belong to their authors.
