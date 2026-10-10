# Online play through the GP4 Ultimate server

[User guide: online races](GUIDE.md#online-races) · [Guía: carreras online](GUIA.md#carreras-online)

Original GP4 plays online with DirectPlay. That needs the legacy DirectPlay feature of Windows. Over the internet it also needs a virtual LAN such as Radmin VPN, because the games list is found by broadcast and every PC connects directly to every other PC (TCP 47624, TCP/UDP 2300-2400 inbound). When any of that is blocked, the player cannot join: the game hangs for about 35 s and fails with `DPERR_TIMEOUT`.

With `[net] server` set, GP4 Ultimate replaces DirectPlay with a small server that every player connects to:

- no DirectPlay feature, no VPN;
- no open port on the players' PCs: they only make outgoing connections. Only the server's machine opens one UDP port;
- the games list works over the internet.

## How to play

1. Someone runs a server (next section) and tells the others its address.
2. Everyone writes it in the configurator, **Online** tab → **Game server**, for example `gp4.example.com` or `203.0.113.7:24004`. Or in `gp4ultimate.ini`:
   ```ini
   [net]
   server = 203.0.113.7
   ```
3. In GP4, play a network game as usual and choose **TCP/IP**. One player creates the game, and the others see it in the list, named after its host (`Alex 1/22`), and join.

Everyone must use the same GP4 Ultimate version: the list only shows games of players with the same version. **Room** (optional, under **Show advanced**) narrows the list further, so friends can share a public server with other groups.

## Running the server

The server is small and needs no setup. Use one of these:

- **Inside your own GP4**: set **Game server** to `local`. Your GP4 runs the server on UDP port 24004 (**Port of the server in this GP4**) from the moment you enter its network game screen. Open (forward) that UDP port in your router to your PC, and allow `GP4.exe` when the Windows firewall asks (it may ask behind the game window). The others write your public IP address (search "what is my IP"). Good for a group of friends.
- **`gp4ultimate-server.exe`** on any Windows PC that stays on: run it, allow it in the Windows firewall, and forward UDP 24004 to it.
- **On a Linux machine or a VPS** (a small cloud server works), so nobody at home opens anything:
  ```bash
  cmake -S . -B build -DGP4U_TESTS_ONLY=ON && cmake --build build --target gp4ultimate-server
  ./build/gp4ultimate-server --port 24004
  ```
  Allow UDP 24004 in the machine's firewall.

`gp4ultimate-server --help` lists the options: port, maximum games, players and players per game. The server writes one line per event: connections, games created, joins, host changes and disconnections.

The server does not run the race. One player's GP4 is still the race master (clock and AI cars), as with DirectPlay: GP4 picks the fastest PC. Through a server, GP4 Ultimate makes it the PC with the shortest and steadiest round trip to the server instead (the server's own PC with `local`), since every packet of the director reaches everyone through it. **Race director** (Online tab, Show advanced) can force it: `prefer` or `avoid`. The server only relays messages. Every message goes through it, so pick a server close to all the players: the extra hop costs a few milliseconds with a server in the same country. With `local`, the host's own messages skip it entirely.

## Troubleshooting

`gp4ultimate.log` says what happened in its `net:` lines:

| Line | Meaning |
|---|---|
| `net: connected to the server (...); it sees us as 1.2.3.4:5678` | OK |
| `net: server ...: no answer from the server` | Wrong address, the server is not running, or its UDP port is not open |
| `net: no server: ...` when creating or joining | Same; GP4 shows its network error |
| `net: server running in this GP4 on UDP port 24004` | `server = local` started |
| `net: cannot run the server here: port 24004: ...` | Another program uses the port: choose another one |
| `net: server ...: this server speaks protocol N...` | Different GP4 Ultimate versions |

Leave **Game server** empty to go back to DirectPlay.

## How it works

The GP4 code that uses DirectPlay is described in [GP4-FINDINGS.md](GP4-FINDINGS.md) ("Lo que GP4 usa de DirectPlay"). `dinput8.dll` already patches GP4's `CoCreateInstance` import. With a server configured, it returns its own `IDirectPlay4A` and `IDirectPlayLobby3A` objects (`src/win/dplay_online.cpp`), which implement what GP4 uses:

- the TCP/IP connection and its address;
- the asynchronous games list;
- create and join;
- players;
- `Send` / `Receive`, guaranteed or not;
- `SetSessionDesc`;
- the system messages: player joined, player left, host migration, session description, session lost.

The free-running race and the playout buffer work on top of it unchanged.

The protocol (`src/core/net_proto.h`) runs over a single UDP conversation per player (`src/core/net_channel.h`):

- **Guaranteed messages** (DirectPlay's `DPSEND_GUARANTEED`) are numbered, acknowledged and resent, and they arrive in order. The resend time is derived from the measured round trip. Long messages are split.
- **Race state packets** are sent once, as with DirectPlay.
- **Keep-alives**: one per second keeps NAT mappings open. A player silent for 15 s is dropped.

The server (`src/core/net_server.h`) keeps the games:

- it gives each player an id;
- it tells the others when a player joins or leaves;
- when the host leaves, it makes the longest-standing player the host and sends it `DPSYS_HOST`, as DirectPlay's host migration does (GP4 opens its games with `DPSESSION_MIGRATEHOST`).

Tests:

- `tests/test_online.cpp` runs the protocol and the server through a simulated network with delay, jitter, loss (up to 35 %) and duplicates.
- `tests/win/test_dpserver.cpp` plays three GP4s in one process through the real DLL, against the server inside the DLL, the Linux server and the Windows server.
