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
  tests and `pd-nettest`; which ones are slow, the seven that were flaky
  and what made each deterministic (a full sound event queue among them),
  and how the Windows build is run as a lobby client under wine
  (`netlobbywinetest.sh`).
- **Traps** — the section of that name: the ones phases 1-8 met, from
  `bool` being two sizes to a client's START and a name's newline.
- **Online co-op** — the section of that name: the solo missions for up to
  twelve players (`netcoop.c`, `src/game/coop.c`, protocol 12,
  PLANS/netplay/spec-coop.md): the host plays PD's own co-op widened to N,
  clients pose its world; the mission's state rides in the scenario block.

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
| co-op | `netcoop.c` | the solo missions online (protocol 12): the mission in RULES, its state (tick mode and cutscene, objectives, timer, alarm, deaths) in the scenario block as a mission block, the host's end in MATCH_END |
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
RULES), the refusal codes, and the protocol history. Protocol 12 is current (online co-op: the mission block in RULES, the SETUPCHR descriptor, the scenario block as a mission block; 11 let a command's START reach the host, played only for a dead player: the respawn).
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
| `neteventtest.sh` | 4.5 min | kill tables, tick order, hudmsgs, an audio check (two staging rounds in a two-minute match), the client's sound event queue never full |
| `netlobbytest.sh` | 0.5 min | two pairs meet through a local pdlobbyd room, leave and rematch |
| `netlobbyuitest.sh` | 0.7 min | the Briefing Room and Game Lobby screenshotted, PING measured (and by HTTP for a lister with the echo mute), the advertised endpoints tried in turn, the host named by its account on the wire and in its own match, no Save Player prompt |
| `netlobbywinetest.sh` | 0.5 min | the Windows build (WinHTTP) as a lobby member under wine against a Linux host |
| `netnattest.sh` | 0.5 min | punch, relay, direct, LAN, mute rendezvous in user-namespace NATs |
| `netpredicttest.sh` | 6.5 min | prediction at 0/150 ms, loss, wine client, sims, a time limit |
| `netlagcomptest.sh` | 9 min | hits at 150 ms with and without lag compensation (on and loss: 8400 frames, at least 30 shots), loss, soak |
| `netscenariotest.sh` | 21 min | every scenario, two lossy |
| `netcontenttest.sh` | 7.5 min | GoldenEye arenas (YOLT: a client respawned by its START alone), mod maps, overlay, bodies, props |
| `netjointest.sh` | 5.5 min | join in progress, a spectator, reconnect and its hold running out |
| `netwidetest.sh` | 2 min | phase 8: a host and eleven clients (twelve games at once, alone), every slot 1-11 walks from its own commands; a room of two refuses a third |
| `netcooptest.sh` | 8 min (pair 4.5, twelve 2, lobby 1.2) | online co-op (spec-coop.md): a host and one, four and eleven clients on Defection: the host's mission in RULES, every client loads and passes GO, the opening cutscene starts and ends on a client at the host's clock, the guards are posed from SETUPCHR records, prediction matches after the opening, a client's death and START respawn, the host's abort reaching every end screen and every client back in the menus; a lobby room created as a co-op mission |
| `nettwelvetest.sh` | 6.5 min | phase 8: a `--dedicated` host and 2, 4, 8 and 12 clients (`COUNTS`) with six sims in a one-minute match: every slot plays, the last opens and shuts its pause menu with its pad's START (commands neutral meanwhile, the host playing on and playing it neutral), every name with its newline, reaches the end screen and leaves it; kill tables equal the host's at every sample and at MATCH_END; snapshot bytes and ENet's per-client rates measured against a budget, and printed as a table per player count |

Then `tools/ci/replaytest.sh compare pd-base.x86_64 pd.x86_64` (the replay
gate: `build/pd-base.x86_64` is the pre-netplay baseline; offline play must
be identical), `python3 tools/pdlobbyd/test_pdlobbyd.py` and
`./build/pd-nettest`. Build both trees first: `cmake --build build` and the
mingw one in `build-win` (re-run `cmake -Bbuild . && cmake -Bbuild-win .`
after adding a file, or the Windows link fails).

**Flaky gates, and what made them deterministic (2026-10-06).** Each was
fixed at its cause, not by lowering its bar (netlagcomptest's loss case is
the one still statistical):

- netscenariotest htm/htmloss ("client's downloads 0"): Hack That Mac's
  download breaks past 45 degrees or 250 units. The staging now keeps the
  terminal in front of the staged player for the download's 20 seconds (a
  per-tick gdb breakpoint on the host, `HtmKeeper`, put back only when it
  drifted), in the background while the HUD is screenshotted.
- netscenariotest koh ("points 0"): nobody held the hill ten seconds alone
  in two minutes. The staging (`koh()`) moves the hill to the invincible
  client's own room (lights and all, which the client follows), so its
  team scores; the hill then moves on as usual.
- neteventtest audio: three causes. (1) A real bug: the sound player's
  event queue (`sndpconfig.maxEvents` 64) filled within seconds on a net
  client with eight sims, and `n_alEvtqPostEvent` drops what does not fit:
  a lost PLAY left a state allocated and never heard, so shots went silent
  (34-53% heard on bad runs). PC builds now have 512 (heap room checked: ~120 KB
  free, 512 items 28 KB). What still filled even 512: a client holding its
  clock (ahead of a host stalled by the staging's gdb; a real host's hitch
  does the same) ran no ticks, so `schedAudioFrame` mixed nothing and the
  synth stood still while each frame posted its sounds' pan and effect
  changes (249 PAN and 248 FX events in the full queue). It now mixes a
  frame whenever a tickless frame finds the output under 1100 samples.
  A full queue's dropped event is logged once ("audio: an event queue is
  full"), and the gate fails a client that logs it. (2) "Near" was a
  straight-line distance; a sim behind walls is quiet. The client's event
  log now carries the volume the game itself gives that gun's sound from
  there (`vol`, `psGetTheoreticalVolPan`), and the check takes shots at a
  sixteenth of full or more. (3) Sample size: a two-minute match, with
  its score limits off (`--net-test-scorelimit 100
  --net-test-teamscorelimit 400`: the team limit counts even without teams
  and ended the match at 30-45 seconds, sometimes before the staged
  explosion), and a second round of sims staged in front of the
  client. The bar (60% heard, four times the random rate) is unchanged;
  the sample it needs went from 5 shots begun in a quiet moment to 30
  (38-92 in eight runs): a client that worked volumes out too low would shrink
  the sample, not pass on it.
- netlagcomptest on/loss: 33 shots put one run at 93.9% on two misses.
  `--net-test-input` kept only 256 script lines, so the shooter (a line
  every 30 ticks) silently stopped at tick 7800; the cap is 4096 and a
  longer script now says it was cut. The rate runs are 8400 frames with the
  shooter firing to tick 7800 (36-65 shots the shooter saw hit, against
  33) and need at least 30 behind the rate; the 95% bar is unchanged.
  **Still statistical.** Firing past 7800 (now possible) showed the target
  pinned on Temple's wall near x 6270, where grazing shots (the ray 46-50
  units from the target's place) that the shooter saw hit miss on the host
  with the view tick equal to 0.01, no loss and no extrapolation: 4 of 65
  in a run without loss. Under 2% loss misses ran 0 to 9 of 52-76 in
  three-minute runs. A lag compensation question for its own phase (the
  rewound pose against the drawn one: an anim frame, a blocked strafe, or
  a lost snapshot interpolated across); until then a rate failure gets one
  solo rerun.
  **Narrowed 2026-10-07** (on-case runs at 77.6% and 91.8%, 11 and 4
  misses, all with the target pinned at x 6270, which in those runs was
  from about tick 1300 on, not 7200): with `--net-lagcomp-debug` on the
  host, every missed shot had the same origin on both machines (to 0.1),
  the same view tick, and the host's rewound target place equal to the
  shooter's puppet (to 0.1), but a different direction: dir.y off by
  0.011-0.020, about 30-50 units at the 2600-unit range, so the grazing
  shots a pinned target gets flip. The cause is the crosshair: the
  shooter's sat centred (`crosspos` 160.0 110.0) while the host's run of
  the same commands had it swivelled to the autoaim target (160.5 107.4);
  `autoaimx/y` were near equal (-0.034 / -0.029) or one side had none.
  So the remote player's autoaim lock (`autoxaimprop`/`autoyaimprop`,
  `canautoaim` in `bmoveUpdate`'s swivel branch) differs between the two
  machines.
  **Found 2026-10-07 (the user: auto-aim is to work online, not be turned
  off):** a per-tick trace of the aim on both machines (`--net-lagcomp-debug`
  on the shooter and the host: an `A` line per tick with the lock, autoaimx/y,
  the crosshair and the view angles, and an `M` line per other player with
  the lock test's inputs, `chrCalculateAutoAim` + `func0f06438c` run again
  on the matrices as autoaimTick saw them; scratchpad `aimdiff.py` and
  `lockdiff.py` lined them up by command) showed the two machines with the
  same view angles and the target's matrices within half a unit, yet the
  host locking on 17-18 ticks before the client every time. The one input
  left was the lock test's line of sight, `cdTestLos03` from the shooter to
  `prop->pos`: the pass rewinds a chr's matrices, not its prop's place, so
  the host tested the line to where the strafing target was *now*, 17 ticks
  on, and it cleared a wall's edge ticks before the line to the puppet did.
  `netLagCompAimPos()` (netlagcomp.c) now gives func0f06438c the rewound
  place of a chr rewound for this pass (`s_Rew[].rewpos`), prop->pos
  otherwise, and the GE tile walk gets the same. After it the lock starts on
  the same command on both (8388 of 8389 ticks the same target, the
  crosshair within half a pixel on 95% of ticks, from 74% and 51%); what is
  left is the puppet's interpolation between snapshots against the host's
  per-tick ring (about 6 units for 5 ticks at a strafe reversal) and the
  isolated 180-unit "jumps" in the trace on a shot's tick, which are the
  trace reading live matrices after the shot ended the rewind, not a fault.
  The aim is still not in the predicted state (netmove): a lock that still
  parts is never corrected by the host; carrying it would mean a protocol
  bump and a visible nudge of the crosshair, left for when a run shows the
  need. MPOPTION_NOAUTOAIM is the match's own option again; nothing forces
  it.
- netpredicttest sims ("head data refused 1"): a block taken at the tick of
  a death or respawn carries the head's death animation (`headanim -1`,
  `bheadStartDeathAnimation`), real data that was counted as refused. It is
  kept out of the head (as before) and counted apart ("death heads kept"),
  but only within 120 ticks of a block that has this player dead, or is a
  respawn or teleport (`netPredDeathCheck`): a death head away from any is
  counted refused and fails the gate, so a host sending death heads for a
  living player cannot pass. Out-of-range head data still counts as refused.
  A second sims flake ("walks' first ticks moved the player otherwise",
  4.13 on the host against 0.76 here, one run in five): the host's player
  was already moving under a sim's push (2.4 units a tick on neutral
  commands) when the walk began, which the client learns only from later
  blocks. Such a walk is counted apart ("while the host had the player
  pushed"); every walk begun from rest on both machines is still compared
  to 0.05 units.
- netsnaptest hostile (unparsable summary line): its games were the only
  ones without `stdbuf -oL -eL`; block-buffered stdout broke a 700-byte
  summary line mid-way and a stderr warning landed inside it. Line-buffered
  now, and the check reads the last whole summary line.
- netnattest c ("host sprayed"/"path is not direct", not reproduced in ten
  runs, loaded or not): a public host (the lobby sees its socket at its own
  address: `rdv: no NAT here`) now waits 2.5 s, not 1 s, before spraying a
  joiner that has not come by itself, and never marks its reply (or its
  spray) SPRAYED, so a late joiner's path is direct. Not root-caused: a
  guess at a joiner later than the 1 s window on a loaded machine. The
  joiner takes its path's kind from the host's PUNCH_REPLY flag alone
  (`rdvOnPunchReply`; a spray is a PUNCH, whose flag it never reads). The
  kind is a label (the roster's netinfo, logs): a public host behind a
  stateful firewall that only its spray opens is really a punch, and is now
  called direct. On loopback lobbies (`no NAT here` every time) the longer
  window delays nothing: the joiner reaches the host before any spray.

replaytest's `randrun` case flips between two hashes on the base binary too
(pre-existing); rerun `CASES=randrun` alone before looking for a regression.

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
- **A client's START.** It opens the client's own pause menu (never the
  host's, never a pause of the match): the command's START is cleared on the
  host unless that player is dead, where it is the death screen's respawn as
  offline (`playerTick` reads A, Z or START; before protocol 11 a client's
  "Press START" did nothing). While a menu is up the client sends neutral
  commands (spec-players §6) and logs "this machine's menu up/down", which
  nettwelvetest reads; `--net-test-menu-shot` has the game screenshot its
  first menu a third of a second in (a gdb stop came a second late, after a
  death had shut it). Phase 8 thought START never paused a client: its test
  pressed at tick 30, inside the match's opening swirl (`TICKMODE_MPSWIRL`,
  no control until about tick 114, offline too). A test script's line that
  covers a tick wins over a later one: put START lines first.
  Checked by netcontenttest geyolt (the client presses START alone every
  second; its first death's respawn, which the second YOLT kill needs, is
  START's: phase 7's binary fails it, 0 respawns) and nettwelvetest (the
  menu from the pad; on the host, in every window its menu was up, the
  host played only neutral commands for that slot and its walk speed
  stayed 0, though its script walks there. The host's `--net-test-trace`
  line carries `busy` (commands played with any input), `speed`
  (forwards, sideways) and `push` (`bondshotspeed`) for that: a player
  standing still is pushed by shots and explosions, once 1100 units in
  150 ticks, so its place is printed, not gated. That player is
  invincible on the host (`--net-test-invincible-pad`, a host test flag):
  with twelve players and explosives it was dead at all six of its
  presses in one run in five, each press a respawn instead.
- **A name ends in a newline.** PD keeps every `base.name` as "text\n"
  (`textMeasure` sizes a label by its lines); a name from the wire, a lobby
  account or "(open)" had none, so the end screen's name row measured no
  height and "Title:" was drawn over it (every slot, phase 7 too). Net code
  writes names with `netNameSet` and prints them with `netNameLen`; "the
  match's players" log line says "(its name has no newline)", which
  nettwelvetest fails on. `base.name` holds 13 characters and the newline:
  a 14-character lobby or wire name loses its last.
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
- **A client's event queue is made at the first EVENTS message.** A match
  whose host had sent no event yet (a mission's quiet opening: nobody
  fires) left `netEventsClientDrain` returning at once, and the scenario
  blocks are applied from that drain, so a co-op client never took a
  mission block. The drain makes the queue itself now.
- **Prediction in a cutscene.** The reconcile reloaded the ring entry's
  copy of the player (taken before this machine's cutscene began: walk
  mode) over the live one, which then recorded itself every tick: the
  client's player fell under gravity to the ground and was moved back to
  the host's held place, 54 units a tick, through Defection's whole
  opening. In a cutscene nothing is reconciled (`cutscene blocks left`
  in the prediction summary) and the host's place is taken outright when
  it ends (`netPredictForceSnap`).
- **Test driving.** Wait on PIDs or log lines, never `pgrep -f` (it matches
  the waiting shell); `pkill -x` with the exact process name; a
  `--net-lobby-shots` run screenshots from inside the game
  (`screenshotRequest`) at the step it reached, which is steadier than gdb
  from outside; the boot's Choose Your Reality can come up over a menu the
  script pushed, so it checks the page is current before shooting.

## Online co-op

The solo missions for up to twelve players (PLANS/netplay/spec-coop.md; the
user, 2026-10-07: "add online co-op up to 12 players for the game's solo
missions", "no need for it to affect offline progress, like the locked
progression").

- **PD's own co-op, widened.** Offline co-op is bond and one coop player
  (`g_Vars.coop`, `chr->p1p2`, `CHR_P1P2_OPPOSITE`, "the other player").
  `src/game/coop.c` answers those questions for N players and gives the
  old answer at two, so the replay gate stays identical:
  `coopOtherPlayerNum` (the nearest living other past two),
  `coopRespawnBuddy` (the living other with the most health),
  `coopAllDead`/`coopAllDeadDone`/`coopAnyAborted`,
  `coopAlternatePlayerProp` (cameras and autoguns cycle the players),
  `coopIsCoopPlayer`/`coopIsPlayerProp`. `chr->p1p2` is 4 bits (was 2).
  `mpReset`'s co-op branch makes a player per human slot; online the
  solo stash swap ([0]/[1] with [12]/[13]) is skipped on both sides and
  `menutick.c` goes back to the menus as after a match. A setup file's
  mine is owned by `COOP_SETUP_MINE_OWNER` (2 offline, 12 online: slot 2
  is a player online).
- **Starting one.** The host: a lobby room with Game = Co-op Mission
  (mission, difficulty, radar, friendly fire; `g_NetCoopSetup`, the room's
  summary says the mission and "Co-op Agent"), the Carrington Institute's
  Accept Mission in a `--host` session (Co-Operative with a human buddy;
  anything else is refused with a notice), or `--net-test-coop INDEX`
  (`--net-test-difficulty`, `--net-test-coop-ff`). `netCoopHostStart`
  does what Accept Mission does and calls H1 (`netHostMatchStarting`) for
  the seats; RULES carry the mission block (protocol 12); STAGE_LOAD's
  stock key names the mission. A client applies the mission at H3
  (`netCoopClientApplyRules`, `netCoopClientStage`) and restores its own
  `g_MissionConfig`, difficulty and co-op options at H12.
- **The world on a client.** As a match: every chr a puppet. A mission's
  setup chrs are spawned on every machine from the setup, and the host
  maps its to the client's by setup command index (`NETDESC_SETUPCHR`,
  kind 10, alongside `SETUPOBJ`), so the guards pose from their records
  with their guns. Chrs spawned at run time are `BODY` puppets (made when
  a chr of the same body and head is here). The client's `objectiveCheck`
  returns the host's statuses (its objects are puppets) and
  `objectivesCheckAll` runs only on the host (its "Objective N:
  Completed" reaches the client as a hudmsg event).
- **The mission block.** When a mission runs, the SNAP scenario block is
  a mission block (scenario byte 0xfe; the body at netscen.c's OFF_BODY,
  layout in netcoop.c): tick mode, in_cutscene, the cutscene anim, frame
  and tween, the alarm timer, the countdown timer, the objective
  statuses, each player's dead/aborted/coopcanrestart bits. A client
  starts its cutscene when the host's begins (`playerStartCutscene` on
  its own player, the frame reset from every block, the tween only when
  it is bond) and ends it when the host's does; the alarm and countdown
  follow. MATCH_END's block is the final one: a client takes it before
  its end screen (`netCoopMatchEnded` in pdmain.c's co-op branch).
- **Ends.** The host alone ends a mission: all dead (`coopAllDeadDone`
  from bond's pass), the objectives met (`aiEndLevel`), its own Abort.
  A client's Abort leaves the session (as End Game does). Every machine
  shows PD's co-op end screen for its own player; the solo stash swap,
  Deep Sea's auto-advance and `endscreenPushSolo` are skipped online;
  `endscreenSetCoopCompleted` writes nothing online (no completion bit,
  no best time, no game file save).
- **Spawns.** Past two players a co-op spawn is spread in a ring round the
  pad (`netCoopSpreadSpawn`, 60 or 90 units, the rooms found again). A
  co-op player online wears its own Combat Simulator character; bond
  stays Joanna. A co-op mission past two players keeps the setup's
  two-player exclusions (`OBJFLAG2_EXCLUDE_2P`).
- **Left for later.** Counter-op; GoldenEye's missions (gewatch/gecinema
  read pad 0); AI buddies; the host's cheats are not synced to clients;
  spectators were not tried on a mission; a client's START in a cutscene
  does not skip it on the host (A, B or Z do, as the host reads them).
