# Netplay: online matches, rooms and the lobby

## Digest

- **The shape** — the section of that name: our own netcode, written from
  scratch (never merged from upstream `port-net` or the murkantor fork),
  server-authoritative in the Quake 3 mould: a listen (or `--dedicated`)
  host runs the only real simulation, clients send commands and pose the
  host's world from delta-coded snapshots, predict their own player and
  get lag-compensated hits. Rooms, the connectivity ladder and join tickets
  come from `tools/pdlobbyd` on the VPS. Plan of record and specs live
  outside the tree (`~/perfect-dark/PLANS/NETPLAY.md`,
  `PLANS/netplay/spec-*.md`).
- **Files** — the section of that name: `port/src/net/*` (one file per
  layer), `port/include/net/*` (what game code may include),
  `port/src/netlobbymenu.c` (the Online Game pages), `tools/pdlobbyd`,
  `tools/nettest`, `tools/ci/net*test.sh`.
- **The wire** — the section of that name: `port/include/net/netproto.h`
  is the byte-level contract (every message field by field, refusal codes,
  `NET_PROTOCOL_VERSION`); the lobby's HTTP and UDP formats are in
  `tools/pdlobbyd/README.md`. Bump the protocol for any change of shape or
  meaning and say what changed beside the number.
- **Gates** — the section of that name: every `tools/ci/net*test.sh`, one at
  a time, offscreen on the RX 580, plus the replay gate, pdlobbyd's unit
  tests and `pd-nettest`; which ones are slow, which ones are statistical
  and get one solo rerun, and how the Windows build is run as a lobby
  client under wine (`netlobbywinetest.sh`).
- **Traps** — the section of that name: the ones phases 1-7 met, from
  `bool` being two sizes to a profile prompt over a net match's end screen.

## The shape

One host, up to eleven clients (`MAX_PLAYERS` is 12 since phase 8; a room
seats its own size, 2-12, a direct host `Net.MaxPlayers`, default 4), plus two
spectators and the host's simulants. Splitscreen, pads, binds, pd.ini's
`Game.Player1-4` and every file format stay at `MAX_LOCAL_PLAYERS` (4). Netplay is off unless
`--host`, `--connect`, `--dedicated` or a lobby room turns it on, and every
hook in `src/` is a one-line call into `port/src/net/` behind
`g_NetMode != NETMODE_NONE`, so offline play is bit-identical (the replay
gate proves it after each change).

| Layer | File | What |
|---|---|---|
| transport | `nettransport.c` | the only file that includes ENet (vendored from upstream, patched: fragment-count bomb, oversized Windows datagrams, a 5 s retry limit); three channels, raw datagrams on the same socket for hole punching, a loss/latency simulator (`--net-sim`) |
| codec | `netbuf.c`, `netdelta.c` | bounded little-endian reader/writer with a sticky error; EQOA-style XOR against the last acked baseline plus zero-run RLE, a 64-entry baseline ring per peer |
| tick | `net.c` | an integer 60 Hz clock: `mainNetFrame` runs the whole `mainTick`s due, one pad sample per tick, the mouse summed per frame |
| session | `netsession.c`, `netrules.c`, `nethash.c`, `netticket.c` | CONNECT/ACCEPT/REFUSE (protocol, build, region, converter, named content-hash components, lobby ticket), RULES and STAGE_LOAD by stage key, LOADED stage hashes, the GO barrier, MATCH_END, seats (a room's size or `Net.MaxPlayers`), join in progress, reconnect holds |
| remote players | `netplayers.c` | a client's commands drive a virtual pad on the host (`osContGetReadData` hook), per-slot settings in a side table |
| snapshots | `netents.c`, `netsnap.c` | prop generations, per-client visibility and priority, fixed quantized records, a full-precision local-player block, acks/NACKs |
| puppets | `netpuppets.c` | the client poses the host's world: chrs interpolated ~2 snapshots behind, objects, doors, lifts, projectiles from descriptors |
| events | `netevents.c` | reliable tick-stamped shots, explosions, hits, deaths, hudmsgs, sounds, applied after their tick's puppets |
| prediction | `netpredict.c` | a 256-tick ring of commands and movement state; a mismatch reloads the host's state and replays the later commands |
| lag compensation | `netlagcomp.c` | a 64-tick pose ring per chr on the host; a remote shot is tested against the chrs as its shooter saw them |
| scenarios | `netscen.c` | every Combat Simulator scenario: props as host entities, state in a delta-coded block in every SNAP |
| spectators | `netspec.c` | follow and free cameras for the two spectator seats |
| lobby | `netlobby.c`, `netrdv.c`, `netlobbymenu.c` | the pdlobbyd client on two worker threads, the Online Game pages, the LAN/direct/punch/relay ladder |

Clients keep all N player slots with the host's numbering; every slot that
is not local is a puppet. The host's RULES carry each slot's name, head,
body and options; a client saves its own setup first and gets it back when
the stage stops (H12), and never writes the host's values to its pd.ini
(H13).

## Files

- `port/include/net/net.h` — what the game's hooks call. Game-visible, so
  nothing in it is a `bool` or ENet's.
- `port/include/net/netproto.h` — the wire (below). `netsnap.h`,
  `netdelta.h`, `netbuf.h`, `nettransport.h`, `netsession.h`,
  `netlobby.h` — each layer's face.
- `port/src/net/netint.h` — calls between the net files only.
- `port/src/netlobbymenu.c` — Combat Simulator > Online Game, Briefing
  Room, Create Room, Game Lobby, Room Settings, Kick.
- `tools/pdlobbyd/` — stdlib Python rooms service (`pdlobbyd.py`, its
  `README.md` = the API, `test_pdlobbyd.py`, the systemd unit). Not
  deployed: the VPS needs UDP 27101 (rendezvous) and 27110-27141 (relay)
  open and nginx's `/pdlobby/` location first.
- `tools/nettest/` — `pd-nettest` (built beside the game): codec, buffer,
  snapshot and hostile-packet tests, clean under ASan/UBSan and wine.

## The wire

`netproto.h` documents every message byte by byte (u8 type first, then
fields through netbuf, never a struct copied whole), the channel each goes
on (RULES and STAGE_LOAD share BULK so a STAGE_LOAD never overtakes its
RULES), the refusal codes, and the protocol history. Protocol 10 is current (phase 8: twelve human slots).
The lobby's HTTP API and the rendezvous/relay datagrams are in
`tools/pdlobbyd/README.md`. A change to a message's shape or meaning bumps
`NET_PROTOCOL_VERSION`; pdlobbyd lists a room's protocol and the Briefing
Room marks rooms of another one with `!`.

## Gates

Run them **one at a time** (they share the GPU and loopback ports) with
`SDL_VIDEODRIVER=offscreen`. Each writes its logs under
`build/<name>-out/` and screenshots under `build/net-shots/`.

| Gate | Time (2026-10-06 run) | What it proves |
|---|---|---|
| `netclocktest.sh` | 0.5 min | `--net-clock-test N` state hashes equal the stock run |
| `netsessiontest.sh` | 0.5 min | handshake, refusals, host quit (load-sensitive: alone) |
| `netplayertest.sh` | 1 min | the host moves a remote player from its commands, clean and lossy |
| `netsnaptest.sh` | 1.5 min | snapshots decode, lossy, hostile input |
| `netpuppettest.sh` | 4 min | puppets, doors, two clients |
| `neteventtest.sh` | 2 min | kill tables, tick order, hudmsgs, an audio check |
| `netlobbytest.sh` | 0.5 min | two pairs meet through a local pdlobbyd room, leave and rematch |
| `netlobbyuitest.sh` | 0.7 min | the Briefing Room and Game Lobby screenshotted, PING measured (and by HTTP for a lister with the echo mute), the advertised endpoints tried in turn, the host named by its account on the wire and in its own match, no Save Player prompt |
| `netlobbywinetest.sh` | 0.5 min | the Windows build (WinHTTP) as a lobby member under wine against a Linux host |
| `netnattest.sh` | 0.5 min | punch, relay, direct, LAN, mute rendezvous in user-namespace NATs |
| `netpredicttest.sh` | 6.5 min | prediction at 0/150 ms, loss, wine client, sims, a time limit |
| `netlagcomptest.sh` | 6 min | hits at 150 ms with and without lag compensation, loss, soak |
| `netscenariotest.sh` | 21 min | every scenario, two lossy |
| `netcontenttest.sh` | 7.5 min | GoldenEye arenas, mod maps, overlay, bodies, props |
| `netjointest.sh` | 5.5 min | join in progress, a spectator, reconnect and its hold running out |
| `netwidetest.sh` | 2 min | phase 8: a host and eleven clients (twelve games at once, alone), every slot 1-11 walks from its own commands; a room of two refuses a third |
| `nettwelvetest.sh` | 6.5 min | phase 8: a `--dedicated` host and 2, 4, 8 and 12 clients (`COUNTS`) with six sims in a one-minute match: every slot plays, pauses, reaches the end screen and leaves it; kill tables equal the host's at every sample and at MATCH_END; snapshot bytes and ENet's per-client rates measured against a budget, and printed as a table per player count |

Then `tools/ci/replaytest.sh compare pd-base.x86_64 pd.x86_64` (the replay
gate: `build/pd-base.x86_64` is the pre-netplay baseline; offline play must
be identical), `python3 tools/pdlobbyd/test_pdlobbyd.py` and
`./build/pd-nettest`. Build both trees first: `cmake --build build` and the
mingw one in `build-win` (re-run `cmake -Bbuild . && cmake -Bbuild-win .`
after adding a file, or the Windows link fails).

**Statistical gates get one solo rerun.** neteventtest's audio check,
netscenariotest's King of the Hill, netnattest's case c, netpredicttest's
sims case (head data refused) and netlagcomptest's on (hit rate 93.9%),
loss and soak cases each failed once in a full sequential run and passed
alone. netscenariotest's htmloss ("not played enough": the download
started, then "Connection broken") fails alone too, about half the time
on the phase 7 binary as well (2 of 3 on 2026-10-06; 4 of 5 on phase 8's):
a flake of the case, not a regression; replaytest's `randrun` case flips between two hashes on the base
binary too. Rerun the one gate alone before looking for a regression.

**The Windows lobby client under wine.** `netlobbywinetest.sh` stages
`build-win/pd.x86_64.exe` in `build/netlobbywine-out/win/` with
`SDL2.dll` and the win32-threads `libgcc_s_seh-1.dll`, links `data/`, the
mod and `added-content/` beside it, and runs it on `DISPLAY=:0` (wine has no
offscreen video; `xset dpms force on` first, or a sleeping monitor freezes
it) with a scratch `--savedir` whose `pd.ini` has `[Mod] GhostUser=`,
`GhostPin=` and `[Net] LobbyServer=http://127.0.0.1:<port>` of a local
`pdlobbyd.py --auth open`. `--net-lobby-script join --net-lobby-room NAME`
then lists, joins, READYs and plays; the Linux host is
`--net-lobby-script host`. The exe has no curl: its HTTP is WinHTTP
(`ghostnet.c`), which takes plain `http://` as well as https.

## Traps

- **Two player counts (phase 8).** `MAX_PLAYERS` (12) is a net room's
  humans: player structs, stats, menus, pads (`NUM_PADS`, a virtual pad
  per slot), side tables. `MAX_LOCAL_PLAYERS` (4) is everything physical or
  on disk: splitscreen, `MAXCONTROLLERS`/`g_Paks`/`g_Pfses` (every rumble
  and pak call is bounded there: `mpindex` is a pad number up to 11 on the
  host), binds, `g_PlayerExtCfg` (pd.ini `Game.Player1-4`; slot k's is
  `LOCALPLAYER(k)` offline), challenge completions, mpsetup teams, the save
  queue's sentinel 4, `mpconfigsim.difficulties` (ROM). `g_MpSetup.chrslots`
  keeps bits 0-3 humans and 4-11 sims (ROM, saves, challenges); humans 4-11
  live in `g_MpHumanSlotsHi`, set only by net code, so test a human with
  `mpIsHumanSlotOn()`. The sim mirror shifts by `MPSETUP_HUMANBITS`, never
  `MAX_PLAYERS` (at 12 it runs off the u16 and every save loses its sims;
  the replay gate does not see that, a save round trip does). The first bot
  mpchr slot is `MAX_PLAYERS`; the solo/co-op stash is
  `[MPINDEX_SOLO]`/`[+1]` = 12/13 (it was 4/5: `gamefile.c` and the solo
  options' items, which still say 4 and 5, map through `SOLO_OPTIONS_INDEX`).
- **The owner nibble (phase 8).** `obj->hidden >> 28` is an owner mpchr
  index (a thrown or placed weapon's, an object's last attacker, the HTM
  terminal's activator), and a room of 12 humans and sims passes 16 chrs.
  Every writer notes the full index in a side table (`NET_OBJ_OWNER`,
  netents.c) and every reader goes through `OBJ_OWNER(obj)`, which returns
  the table's value when its low four bits still equal the nibble, else the
  nibble (offline: the nibble, the ROM's). A new writer of those bits needs
  the hook, or a stale table entry can name the wrong chr.
- **A command-line client has no agent file.** `--connect` from a fresh
  `--savedir` skips Choose Your Reality, and the game-file save PD queues
  after every match (save queue value 4) put "Error Saving Game: insert the
  Controller Pak" over the end screen, which START only retried. A net match
  queues it only when a game file is loaded.
- **Host traffic.** Every 300 ticks the host logs ENet's bytes to and from
  each client (`net: traffic slot N so far`) beside the snapshot stats
  (`net: snap slot N so far`); nettwelvetest reads both. A client that has
  left for the menus (its LOBBY for the current match came) gets no more
  snapshots while the host sits on its end screen: they had all become
  keyframes, nothing acked, about 23 KB/s each until the host stopped the
  stage (`netSessionSlotLeftMatch`).
- **A pad's START does not pause a client.** A `--net-test-input` START
  on a client in a match (its player alive, tick 30) left the pause menu
  shut, though START on the end screen works; ESC is the path bondmove.c
  names for a net client. Not chased in phase 8 (commands strip START on
  the way to the host; where the client's own player loses it is unknown).
  nettwelvetest puts the pause menu up from gdb instead, as the player:
  `setCurrentPlayerNum(slot)` first, or it goes into slot 0's menu and
  nothing draws.
- **Sound off fills the music queue.** With `g_SndDisabled` (`--no-sound`,
  every `--dedicated` host) nothing drains `g_MusicEventQueue[40]`, and
  `musicRestoreInterval`, unlike the other queue writers, did not check it:
  two entries per MP death of a player whose HUD is drawn, so a dedicated
  host with a dozen players ran past the queue into the globals after it
  within a match (it crashed in `objectiveCheckRoomEntered`). Found by
  nettwelvetest under ASan; the writer now returns with sound off.
- **The player-order shuffle.** `playermgrShuffle` draws RNG for a fixed
  count every tick: 4, or 12 in a room of more than 4 (keyed on the count,
  never on `g_NetMode`: `--net-clock-test` runs offline as a server and
  compares RNG with the stock run). Award ties draw RNG per player too
  (`mpFind*N`, the same draws for 4 or fewer).

- **`bool` is two sizes.** ENet includes `<stdbool.h>` (1-byte `bool`); the
  game's `types.h` makes `bool` an `s32`. Keep ENet in `nettransport.c`
  alone and keep game-visible net headers free of `bool` (see recording.md,
  and the 48- vs 56-byte `ghostnetreq` that crashed every Windows F3 send).
- **RNG prototypes are undeclared or wrong.** `rngSetSeed` is declared
  `u32` in two places against a `u64` definition; net code declares it
  itself and sends the full u64. Seed at H4 in `lvReset`, after the music
  RNG and before props; any local-only `rngRandom` after H4 desyncs the
  rest (the LOADED "rng" component catches it).
- **A client never calls `mpStartMatch`.** It would reroll the stage,
  weapons and sims. The client applies RULES instead (H3), before the
  stage loop, because `Mod.Bodies` changes the stage heap.
- **Stage identity is a key, never an index.** Overlay mods replace the
  arena table and the Stage Loader assigns mod stage ids per machine;
  `mainChangeToStage` silently turns an unloadable stage into the title, so
  check first and refuse by name.
- **`VERSION_HASH` is configure-time.** A stale build passes the build check;
  CONNECT also compares `NET_PROTOCOL_VERSION` and the GE converter version.
- **pd.ini pollution.** Applied RULES are live ini variables and any
  `configSave` would write the host's values into the client's ini (H13
  swaps the player's own back for the write). The host's remote slots would
  overwrite its own Player 2-4 values (H12 restores them).
- **Profile side effects.** Remote slots never carry a `fileguid`, a
  challenge is never marked complete from a net match, and PD's "Save
  Player" prompt is not put up after a net match (`mpPushEndscreenDialog`,
  phase 7d: a client was offered to save a profile it did not own).
- **A lobby host plays as its account.** Its local profile is often
  unnamed ("Player 1"). Renaming slot 0 only on the wire
  (`netSessionWireName`, RULES and ROSTER) is not enough: the host builds
  every name-bearing HUD message itself ("Killed by %s", Pop a Cap's
  "Get %s!", the case messages) and forwards the finished text, so members
  read "Killed by Player 1" beside a scoreboard saying the account. The
  host's own `base.name` is set to the account at `netHostMatchStarting`
  and the profile's name put back at H9 (`netHostMatchEnded`), before the
  end screen and before the queued MPPLAYER save (`func0f0fd548`) of a
  filed profile, which would otherwise write the account into it; also at
  H12 and `netSessionLobbyStop` if the match never reached H9. So the
  host's end screen ranking shows its profile name; everything during play
  shows the account.
- **Clients judging the end.** Gate only the two `mainEndStage` calls on a
  client (H8), not the whole block, or the one-minute message and the alarm
  go too. Hold the barrier through `lvupdate240` (H7), not the frame clock,
  or the game catches up in a burst at GO.
- **The local-player block is full.** `NETLP_SIZE` 688 and `NETMOVE_SIZE`
  480 have no spare byte; the next field is a size change and a protocol
  bump.
- **A lobby member's connect.** The ladder's path goes first, then every
  endpoint the host advertised, then where the rendezvous saw it. The
  ladder's path is the measured working one (often the only one behind
  NAT), so it keeps the full 30 s `NET_CONNECT_WINDOW_MS` (a host loading
  the stage may not service its socket for a while); the fallbacks after it
  get 8 s each (`LOBBY_CAND_WINDOW_MS`) except the last, which gets 30 s.
  Only a window that found no host (`netSessionClientUnreached`) moves on
  to the next address; a STARTED refusal (host loading or ending) retries
  the same address that answered (`s_CandNext` kept), not the dead ones
  ahead of it. Only the first endpoint used to be tried.
- **The Briefing Room is ~266 units wide.** The list item says 304 but the
  dialog shows about 266 of it at any window size, and `textMeasure`'s
  widths are in the same units as `drawCell`'s x. A room is two rows of
  text (name and host, then IN/ARENA/MODE/PING/PASS) so a 32-character
  name fits. A plain list centres its focused row, leaving half the view
  empty above row 0; `listClamp` holds it like a dropdown.
- **PING is an estimate through the lobby.** Before joining, nothing goes to
  a host (its address stays out of the public list), so the column is this
  machine's ECHO round trip to the rendezvous plus the lobby's own probe of
  the host (`host_rtt_ms`, every 10 s); `--` means the host has not
  registered its UDP socket yet. With this machine's UDP to the lobby
  blocked (no ECHO back), its leg is estimated from the room list's HTTP
  round trip instead (half the lowest seen: a fresh connection plus the
  request, so over TLS it reads high, never low) and the column shows
  `~n`; `--net-test-no-echo` forces that in a test. Once in the room the
  roster shows the measured path and ping (LAN/DIR/NAT/RLY).
- **A label does not widen the dialog.** The Game Lobby's
  "Arena / Mode / Host" line and the Briefing Room's selected-room note
  are cut value by value with `labelFitDots` to `LABEL_WIDTH` (262 units, measured in HandelGothicXs, the font a small-font label draws in, not the Sm the list cells use);
  the host's name is cut last.
- **Test driving.** Wait on PIDs or log lines, never `pgrep -f` (it matches
  the waiting shell); `pkill -x` with the exact process name; a
  `--net-lobby-shots` run screenshots from inside the game
  (`screenshotRequest`) at the step it reached, which is steadier than gdb
  from outside; the boot's Choose Your Reality can come up over a menu the
  script pushed, so it checks the page is current before shooting.
