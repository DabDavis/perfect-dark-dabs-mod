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
- **Content follows the host** — the section of that name (`netcontent.c`,
  protocols 13 and 14): a client plays the host's overlay mod, Stage Loader
  maps, conversions and ROM hack mode from its own copies (the content
  block in ACCEPT and RULES, the live swap and its restart rule, on-demand
  map mounts, LOADED's "mod" component, the lobby's `mod` and `ge` fields),
  and a conversion or map mod it has not got is **served by the host** into
  one of fs.c's memory directories (`$N/<name>`): only the host needs the
  ROM. Also there: **co-op on the conversions' missions** (the mission
  set in RULES, stage key kind 3) and **campaign rooms** (the host plays
  its set's menus; each mission it starts is the room's next match; the
  black screen on GoldenEye's openings, late joins, the host's menus and
  what a client drew wrong, its doors among them: 2026-10-07).

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
- `port/src/netlobbymenu.c` — Online Game (the main menu's row, its own
  menu root; the GoldenEye/ROM hack Combat Simulators' row, hidden in
  Perfect Dark's as a duplicate), Briefing Room, Create Room, Game Lobby,
  Room Settings, Kick. The rows ask `netLobbyBuilt()` whether the build has
  the lobby: `PD_GHOST_NET` is ghostnet.h's, and a file that does not
  include it sees it undefined - both rows were greyed in every build until
  2026-10-07 by an `#ifdef` of it in netlobbymenu.c.
- `tools/pdlobbyd/` — stdlib Python rooms service (`pdlobbyd.py`, its
  `README.md` = the API, `test_pdlobbyd.py`, the systemd unit). Deployed
  2026-10-07 on the ghost server's box (README "Running it": unit, nginx
  location, nftables rules; UDP 27101 and 27110-27141 open in the host
  firewall; the ECHO answered over the public internet, so the provider's
  firewall passes it).
- `tools/nettest/` — `pd-nettest` (built beside the game): codec, buffer,
  snapshot and hostile-packet tests, clean under ASan/UBSan and wine.

## The wire

`netproto.h` documents every message byte by byte (u8 type first, then
fields through netbuf, never a struct copied whole), the channel each goes
on (RULES and STAGE_LOAD share BULK so a STAGE_LOAD never overtakes its
RULES), the refusal codes, and the protocol history. Protocol 14 is current (co-op on the conversions' missions: the mission block's set tag and stage key kind 3; content served by the host: CONTENT_REQ/BEGIN/FILE/END/NO; 13 was content follows the host: the content block in ACCEPT and RULES, CONNECT's "mod" and "added" logged rather than refused, LOADED's "mod" component, LEAVE NOMOD; 12 was online co-op: the mission block in RULES, the SETUPCHR descriptor, the scenario block as a mission block; 11 let a command's START reach the host, played only for a dead player: the respawn).
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
| `netsessiontest.sh` | 0.5 min | handshake, refusals, host quit (load-sensitive: alone); a client with no mod switches to the host's `mod_allinone` live and back (swap), one with a `--moddir` of its own leaves with NOMOD (r-nomod) |
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
| `netcontenttest.sh` | 9 min | GoldenEye arenas (YOLT: a client respawned by its START alone), Goldfinger's mode (gfvariant, needs its zip in added-content), a client with nothing installed served the conversion by the host (fetch), mod maps (one mounted on demand: modmount), overlay (and one the client lacks, left over with NOMOD: modmissing), bodies, props |
| `netjointest.sh` | 5.5 min | join in progress, a spectator, reconnect and its hold running out |
| `netwidetest.sh` | 2 min | phase 8: a host and eleven clients (twelve games at once, alone), every slot 1-11 walks from its own commands; a room of two refuses a third |
| `netcooptest.sh` | 10.5 min (pair 4.5, twelve 2, lobby 1.2, ge 1.5, campaign 1.5) | online co-op (spec-coop.md; ge: GoldenEye's Dam as a co-op mission, the client mounting the conversion on demand and taking the set from RULES): a host and one, four and eleven clients on Defection: the host's mission in RULES, every client loads and passes GO, the opening cutscene starts and ends on a client at the host's clock, the guards are posed from SETUPCHR records, prediction matches after the opening, a client's death and START respawn, the host's abort reaching every end screen and every client back in the menus; a lobby room created as a co-op mission; campaign: a GoldenEye campaign host starting Dam alone from its folder, the opening ending on its own player, a client joining in progress with no opening, beside the host, predicting at 95% or better |
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

## Content follows the host

Protocol 13 (`port/src/net/netcontent.c`; the user, 2026-10-07: "share the
host's mod/conversion with the guests ... the share is without
distributing"). A client plays what the host plays out of its own copies;
nothing is ever sent from one machine to another but names and hashes.

- **What the host names.** `netContentHostNeed()`: the overlay mod's dir
  basename and its contents hash (`netHashDirContents()`, the "mod"
  component's walk, cached per path for the process), and the GoldenEye
  ROM hack mode's conversion tag (`g_GexPlusVariant` -> "gf64"). ACCEPT
  carries the mod (a client learns at the join); RULES carry the mod and
  the tag (per match, checked again at H3: a join in progress has both at
  once, and a host may have swapped since).
- **The overlay mod on a client.** `netContentFollow()`: the same name
  among the installed mods (`modListIndexOf`), the same bytes
  (`netHashDirContents` of that dir), then `modListSwap()` live, with the
  player's own `Mod.ModDir` put back into the selection so pd.ini keeps it
  (H13) and `netContentRestore()` swapping the own mod back when the
  session ends (`netClientEnd` outside a match, H12 inside one). The
  restart rule is mods.md's: a mod with `segs/` (every imported patch:
  GoldenEye X, the Mario characters) swaps neither in nor out under a
  running game, so the client leaves with NOMOD and the text says to choose
  it in Load Mods, Restart Now and join again; so does one whose mods came
  from `--moddir` (the gates'). Missing or another version: named, with the
  hashes. The host trusts none of it: LOADED's "mod" component is the
  overlay as the client has it loaded (`netStageHashCloseWindow`), and a
  difference is STAGEHASH "mod".
- **Maps and conversions.** `netResolveStageKey` kind 1: a dir installed
  but not mounted (Mod.MapMods left it out) is mounted on the spot
  (`netContentMountMaps` -> `modMapsMountIndex` -> `fsAddMapsDir` +
  `modloaderAddDir`, the per-dir half of `modloaderInit` split out) and the
  key looked up again; it stays mounted until the next swap or start. A
  conversion missing here (GoldenEye Arenas, Goldfinger 64, Tomorrow Never
  Dies 64: `gexPlusRomIsConversionDir`) is refused with the source to put in
  `added-content/` (`netContentNoStageText`); the game converts it at the
  next start. The converter version is still compared at CONNECT.
- **The mode.** `netRulesApply` sets `g_GexPlusVariant` from the tag
  (`netContentVariantApply`: the hack converted here under it) and calls
  `mpSetGexPlusMode()` rather than writing `g_GexPlusMode`: the setter puts
  the hack's weapon sets in the list's block (gexplus.c), which the set
  number and weapons RULES carry index; its own choice of set and scenario
  is overwritten by the host's right after. `netRulesRestore` does the
  same with the saved pointer. Online Game is no longer greyed in the
  GoldenEye and ROM hack modes; a room made there is a match (the Game
  dropdown offers no co-op mission in those modes).
- **CONNECT and the lobby.** The host logs, and no longer refuses, a
  different "mod" or "added" component (as "mapmods" since protocol 8);
  "rom", "borrow" and "geconv" still refuse. The session hash's "all", the
  lobby's `content`, is rom + borrow + geconv, so rooms list as joinable
  across mods and added content; pdlobbyd's room summary has `mod` and
  `ge` (create/settings fields, caps 127 and 15), the Briefing Room's focus
  note says "Mod X: loaded / installed, loads on join / installed: load it,
  restart / NOT INSTALLED HERE" (`roomContentNote`), or for a GoldenEye
  room whether its arenas are converted here, and the Game Lobby's weapons
  line names the mod.
- **Served by the host (protocol 14).** The user, later the same day:
  "only the host needs the rom conversion". A client whose STAGE_LOAD key
  (kind 1 or 3) names a conversion or map mod it has not got, and could not
  mount, keeps the STAGE_LOAD (`s_FetchStage`) and sends CONTENT_REQ; the
  host (`netContentServeRequest`) serves only a dir it has mounted for its
  maps (never its overlay, never a "$N/" dir), lists it without text,
  caches, archives and ROMs but **with modconfig.txt** (the first cut
  skipped every .txt, and the maps registered under their file names), and
  sends CONTENT_FILE parts of 48 KB, two a tick on BULK
  (`netContentServeTick`), with CONTENT_BEGIN/END round them and the load
  deadline kept open meanwhile. The client puts the files in a memory
  directory (fs.c: `fsMemDirCreate/AddFile/Seal`, path `$N/<name>`, which
  fsFileSize/fsFileLoad/fsScanDir/fsFileOpenRead answer from the table;
  `fsAddModDirWith` mounts such a path verbatim), mounts it for its maps,
  `modloaderAddDir`s it and replays the kept STAGE_LOAD. GoldenEye Arenas
  is 2905 files, 12.7 MB, 1.1 s on loopback; the stage hash (stan too) came
  out the host's on a machine with no mods and no added content at all.
  Nothing touches the client's disk; the memory lasts the process. A hack's
  mode on such a client: `netContentVariantName(tag)` gives the hack's
  converted name, else the served folder's name (a static copy), and both
  `netContentVariantApply` and netcoop's set resolution go through it.
  GE-X and the Mario characters (segs/) are not served: they load only at
  a start.
- **Co-op on the conversions' missions (protocol 14).** `g_NetCoopSetup.game`
  / RULES' mission block `game`: "" Perfect Dark's, else a conversion tag;
  `stageindex` is then the mission's number in that conversion
  (`modloaderMissionStageOf(variant, n)`, the mode's list test factored out
  of `modloaderMissionStage`). The host sets `g_GexPlusVariant` for a
  hack's mission and puts it back at the match's end; a client applies it
  from the tag. STAGE_LOAD's key is kind 3 (dir + mission number: a
  mission's map name is its arena's, so a map key found the arena) and
  `netClientBeginStage` sets `g_MissionConfig.stagenum` from the key's
  stage. The room settings: a Missions dropdown (the sets converted here,
  `netCoopGameTag`), the Mission list per set. `--net-test-coop-game TAG`
  for the gates. GoldenEye's opening is gecinema's, run on each machine:
  the client predicts while the host holds the player, so prediction
  wobbles for the opening and settles (75% -> 83% matched in the smoke
  run); not judged by the gate. GoldenEye's own code addresses players
  through `g_Vars.currentplayernum`, so the pad-0 fear in spec-coop.md was
  unfounded. The folder's REPORT page does not open online
  (`gexFrontMissionReport` bails in co-op): PD's co-op end screen instead.
- **Campaign rooms.** The user: "start the whole mode ... host controls
  everything, and game is seamless like offline" and "same for PD
  missions". Game = "<Set> Campaign" in Create Room (`g_NetCoopSetup.campaign`,
  the lobby's `stage_key` "campaign:ge"); the launch
  (`netSessionLobbyStartMatch`) opens the host's menus for the set instead
  of a match (`netCoopCampaignOpen`: the Perfect Menu root, and the set's
  folder over it through `gexFrontOpen`); `netCoopAcceptMission` then
  takes any mission the host's menus start (the folder's
  `frontStartMission` goes through Accept Mission) as the session's next
  co-op match, the set from the stage's dir, the host's pdmode kept;
  after it `netMenuAfterMatch` (menutick.c's one hook, replacing the
  lobby-only call) sends a campaign host back to its set's menus
  (`gexFrontOpenAfterMission` + the main menu root + `playerPause`), the
  guests to the room, where the next STAGE_LOAD pulls them in (a guest
  still on its end screen joins late, as before). `--net-test-campaign
  TAG` opens one on a `--host` session eight seconds in. Verified on the
  rig with gdb driving the folder (`set var g_Front.mission=N`,
  `call frontStartMission()`): Dam then Facility, the guest fetching the
  conversion first; the host's return to its folder after the end screen
  is by code path only - a headless host's end screen cannot be dismissed
  (`--net-test-input` feeds the match, not the menus, and
  `menuPopDialog()` under gdb did not take it down).
- **The black screen on GoldenEye's, Goldfinger 64's and TND64's campaigns
  (2026-10-07).** The user: "the GE, GF64, TND64 campaigns black screen, i
  started them solo". GoldenEye's opening (gecinema.c) was made for one
  player and ran on whoever was current: `gecinemaTick` at the top of
  `lvTick` (whoever the last player loop left), and `gecinemaSwirlTick` in
  *every* player's `playerTick`, each call advancing the swirl's clock. A
  lobby room always has a seat per member slot (join in progress), so even
  a solo room's mission has two players and the host ticks both: the fade
  to black at the end of the still landed on one, the fade back in and
  `player0f0b9a20()` on the other, and the host's player stayed at
  `colourscreenfrac` 1 for the rest of the mission. Which one depended on
  the frame (every run on the user's box, about half here). Now the
  opening has an owner (`gecinemaOwnPlayerNum`: this machine's
  `g_NetLocalSlot`, bond on a dedicated host, offline the current player as
  before): `gecinemaTick` runs as it, and only its tick moves the swirl on
  and ends it; the other players get the swirl's camera round their own
  bodies (`gecinemaSwirlView`). Found with gdb: the black host had
  `players[0]->colourscreenfrac` 1, `bondfadefracnew` 1. Perfect Dark's
  missions were never affected (their intro is per player).
- **Late join into a campaign (the same day: "should allow a late join in
  progress").** A launched room was already joinable: between missions a
  joiner connects and waits in the Game Lobby (status "Campaign: you join
  the host's next mission", `netLobbyCampaignState`) and the next
  STAGE_LOAD takes it in; during a mission it is a join in progress. Three
  fixes made that work: a client holds GoldenEye's opening until the host's
  GO, in black (`netClientAwaitingGo`), and skips it when the GO says the
  match is under way (`netClientJoinedInProgress`) - its local opening had
  held the player through 270 corrections, prediction 49% where it is now
  99.9%; the host's `netHostLateGo` marks the seat (`netCoopHostLateJoin`)
  and `playerStartNewLife` puts that one life beside a living player
  (`netCoopJoinSpawn`: `coopRespawnBuddy`'s pick, PD's own
  `chrAdjustPosForSpawn` 60 units round it) rather than at the mission's
  start (a death later respawns the mission's way); a client hides a
  mission's setup chrs until their first record, as it does sims (one the
  host killed and freed before the join is never sent, and would stand
  where the load left it).
- **A campaign host's menus.** The Perfect Menu's tick pushed the Game
  Lobby over it whenever a room was open (`netMainMenuTick`), so a Perfect
  Dark campaign host got the Game Lobby on the first tick after the launch,
  with Back ignored and only Leave out of it; a GoldenEye campaign host met
  it on backing out of the folder. A campaign's host is left on its menus
  now; the Perfect Menu's Online Game row takes anyone in a room to its
  Game Lobby (keeping the mode as it is), and the Game Lobby has "Back to
  the Campaign" for a campaign's host (`netCoopCampaignMenusOpen`).
- **What a client drew wrong in GoldenEye's levels (the same session).**
  An object's rotation went on the wire as a quaternion of the matrix read
  row by row, and `netQuatToRot` wrote it back column by column: every
  object the host sent stood turned -theta for theta (PD's props are
  mostly square to the axes, so nobody saw it). Fixed in the decode only
  (the wire's meaning is unchanged; no protocol bump). A setup object keeps
  its own matrix while the host's rotation is still the one it would send
  (`netRotSameOnWire`, within a sixth of a degree after the wire's
  quantization): GoldenEye's converted panes are sheared into place, which
  no rotation and scale can carry; a turned one takes the record's rotation
  with each column's own scale (`netRotScales`; one scale for all three, as
  before, blew a stretched object up). The client's own gun stood in its
  opening's shots: the local block re-equipped it from the host, which
  plays no opening for that player; not while `gecinemaIntroIsOn()`.
- **A client's doors that clip to their box (the same day).** Dam's tower
  doors and its tunnel gate drew as shards that changed every frame on
  every client, with their `frac`, mode, place, rooms and `realrot` the
  host's to the digit. A door with `DOORFLAG_0004` (clips to its box as it
  slides: most of a converted GoldenEye level's, and by xblamesh.c's count
  nearly every sliding door in Perfect Dark's own) draws its display list
  from clipped vertices that `doorsCalcFrac` makes every tick: into the
  door's own buffer once it is still (`func0f08d460`, `door->unka4`), into
  the frame's memory while it moves and at the load (`func0f08d3dc`,
  `gfxAllocateVertices`). A client never runs `doorTick`, so every such
  door drew from the load's frame memory, whatever later frames had put
  there. Now objTickPlayer's full tick calls `netPuppetDoorTick` for a
  door on a client (C6c), which does the still-door half, and `netPupDoor`
  clips again whenever a record changes the door's `frac`. Found by
  elimination with gdb: hiding the doors (`OBJFLAG2_INVISIBLE`) took every
  shard away, their tick state matched the host's, and the one thing a
  door's draw reads that only the tick writes was `rwdata->dl.vertices`.
- **The host's own view, flickering in the HD look (the same day).** The
  user, hosting Dam's campaign alone in a lobby room: "the door by the spawn
  of dam is glitching and also the truck". A lobby room's mission has a
  seat per member slot (four here, three empty: dead, hidden, at the spawn
  ring), and the host builds every player's view each frame and keeps only
  its own (lv.c, `netDiscardPass`), in the order `playermgrShuffle` deals
  every frame. An HD mesh keeps its pose and GPU palette for the frame
  (xblamesh.c, keyed on `frameCount`, which `gfxSwapBuffers` moved), and
  with them `posedmtx`/`gpumtx`: a *copy* of the model's root matrix,
  which every view computes under its own camera. So whenever an empty
  seat's view came before the host's, the host drew the gate and the truck
  under that seat's camera, off its screen: the gate was there one frame
  in three or four, a grey slab and black ground behind it the rest. The
  N64 look never did it, nor one player alone, nor the same run with the
  host's player forced first in the order (gdb on `playerorder`); the
  door's state was the host's own all along. Now `xblaMeshPassBegin()`
  moves the key at the head of each view in lvRender's loop: a view's
  meshes are posed for that view (the arena stays the frame's). Any host
  with more than one player had it (guests in a co-op mission, a Combat
  Simulator match on an HD arena); a client never did (it builds only its
  own player's view, `netClientRenderPass`). Found by filming the gate
  every three frames from gdb (`screenshotRequest` called from the main
  loop of the gdb script, never inside a Breakpoint's `stop()`, which
  freezes the game).
- **GoldenEye's watch online (the same day).** The user: "the ingame pause
  menu it unreadable it shows sims and lots". gewatch.c's `watchIsMp()` read
  `mplayerisrunning`, which `mpReset` sets for co-op too, so START in a
  co-op mission on a converted level opened GoldenEye's multiplayer overlay
  (every seat, the simulants). It reads `normmplayerisrunning` now (a match
  started from the folder keeps the overlay; the Combat Simulator's own on a
  GoldenEye arena keeps PD's dialog, `gexFrontIsInside`). Online the watch:
  has an owner (`g_Watch.owner`: tick, render and `geWatchIsOpen` act for
  that player only - the host ticks every player); never freezes the level
  (`lvSetPaused` offline only; `pausemode` PAUSED still stops the zoom and a
  second START); holds its owner's controls (`geWatchHoldsPlayer`,
  `geWatchMpHoldsInput`); reads its owner's pad (`watchPadNum`: a client's
  pad 0 is neutral); closes when its owner dies; and Abort goes through
  `netCoopClientAbort`. On a client its commands are neutral while it is up
  (netplayers.c, as a menu), the host's gun is not re-equipped over it
  (netents.c) and prediction does not reconcile under it (netpredict.c,
  "watch blocks left N" at the end of the prediction line). Left: a weapon
  chosen on the watch's inventory page reaches the host only from the
  host's own player (nothing on the wire carries a choice; PD's pause
  inventory online is the same); the host's own player is unarmed to the
  others while its watch is up; the static flash stays off online (the
  host's RNG).
- **An out-of-play seat's view is not built (the same day).** The user,
  on hearing a host builds every player's view: "is that the most optimal
  way?" Half of it has to stay - PD's simulation is per view (bgTick's
  rooms, propsTickPlayer, autoaim, handsTickAttack's shot against what that
  player had on screen) - and the costly half (gfx_run) never ran for a
  discarded view. But an open seat's player (netSeatVacate: dead, hidden,
  nothing in it moves or shoots) had its whole view built every frame: three
  of the four in a lobby room played alone. `netHostRenderPass` skips such a
  view (seat OPEN and vacated, player dead, no `dostartnewlife` pending - a
  joiner's GO sets that, and the respawn happens inside the view's pass), as
  `netClientRenderPass` skips a puppet's on a client, and
  `netHostOrderPlayers` puts those seats last after playermgrShuffle, since
  eight places key once-a-frame work on `currentplayerindex == 0` (bg.c,
  prop.c, player.c). The frame's first pass always runs (a dedicated host
  with every seat open). The seats line says how many views were skipped
  ("their views not built N").
  **Measured, not done: a remote player's view without its drawing.**
  Dam co-op in the HD look, a host and three idle clients on one machine,
  the host's main thread over ticks 1200-2400 (`perf stat -t`, the game's
  pid by its --savedir - not timeout's): alone 14.4 M instructions a tick;
  with the three views built as now 31.3 / 31.2 M; with a temporary switch
  skipping the views' drawing calls (skyRender, gebeanStageRenderBackdrop,
  bgRender with the props, beams, puffs, shards, sparks, weather, the HUD)
  22.2 / 22.5 M. So a guest's view costs the host about 5.6 M a tick, 3.0
  M of it drawing that is thrown away and 2.6 M the simulation it needs:
  skipping the drawing would take ~29% off the host's thread with three
  guests, ~43% with eleven. Cycles were too noisy to use with four games
  on one box (25.4 / 30.4 against 24.5 / 21.0 M; the frame limiter also
  spins its last 1.5 ms, sync_framerate_with_timer). Before doing it: the
  drawing calls have game side effects to keep (ROOMFLAG and prop
  on-screen flags, the sky's flare timers), so an audit, then the net
  gates; idle guests at the spawn understate the simulation half. A host
  refuses every joiner while its own Mod.SimBrain is "modern", and an HD
  host one whose Mod.XblaMeshes differs (both met setting this up).
- **Seen once: netcontenttest geyolt, the client's START respawn missed
  (2026-10-07).** In the gate run for the commits above, the client took
  its first death from the host's block, pressed START every second, and
  never came back ("deaths 1, respawns 0", 935 ticks out, no revive), while
  the host had respawned it (the second staged kill found it alive). The
  same binary passed the case alone three times after. Not traced: the
  client's side is netEntsClientApplyLocal's `s_LpRespawnPending` from the
  block's respawn counter (`netLpTrack` on the host). Rerun the case alone
  (`CASES=geyolt`); if it comes back, trace that counter on both sides.
- **Still open on a client:** the gate's switch on Dam (the pillar beside the
  tunnel gate) shows its light grey where the host's is red; Dam's truck,
  inside the near plane of one opening still, may not fade on a client
  (gecinemaPropAlpha) - seen once, not confirmed after the door fix.
- **Test switches for these (lobby script host):** `--net-lobby-campaign
  TAG` (the room is TAG's campaign: "pd", "ge", "gf64", "tnd64"),
  `--net-lobby-solo` (LAUNCH with nobody else in), `--net-lobby-menus` (made
  from the Perfect Menu's Online Game, the menu root a player has). A
  mission from the folder by gdb: `set var g_Front.mission=N`, `call
  (void)frontStartMission()`; a forced opening still on any machine: `set
  var g_GeIntroShot = g_GeCinemaShots[K]`, `set var g_GeIntroTimer = 0`.
- **Not done.** A restart-and-rejoin for the segs/ mods (the game could
  relaunch itself with the mod selected for that run and join the room
  again: `updateRelaunchSelf` is the relaunch, `--net-lobby-*` the
  precedent for driving the lobby from arguments); the three MUST_GE keys
  still refuse rather than follow; GoldenEye's REPORT page after an online
  mission; a client's prediction held during GoldenEye's opening.
- **Traps met.** `modListSwap()` sets the selection to what it loaded, which
  is bound to `Mod.ModDir`: the player's own selection must be read before
  the swap and put back after it (the first cut read it after, remembered
  the host's mod as the player's and wrote `ModDir=mod_allinone` into the
  gate client's pd.ini; netsessiontest's swap case checks the file).
  `mpSetGexPlusMode(true)` calls `mpApplyWeaponSet()`, which rewrites
  `g_MpSetup.weapons` from the set it chose: it must run before RULES'
  weapons are written, never after. The lobby's `rules` values are capped
  at 32 characters, too short for a mod dir name (one archive mod's is 40):
  `mod` is a field of its own. The "rom" session component hashed
  `g_RomFile`, and the game preprocesses a stock segment in place inside
  that image (`ROMSEG_DECL_SEG`'s preprocess functions; a mod's `segs/`
  copy is preprocessed in its own buffer instead), so a host with GoldenEye
  X loaded hashed its ROM as 4e51142a... and a joiner with no mod the
  identical file as 69a7c78a...: found on the first cross-machine join,
  the guest refused for its "ROM". It now hashes the file on disk
  (`romdataGetRomPath()`).
  `netsessiontest.sh` and `netlobbytest.sh` launched the game without
  `stdbuf -oL` (netcontenttest had it): a redirected stdout is
  block-buffered, so whether "net: hosting on UDP port" had reached the
  log inside the 60 s wait depended on the game having logged 4 KB by
  then. One more log line before it ("net: hashed ...") left the host's
  total just short, and the gate failed its host's start every time
  (`stat` showed the log at 259 bytes, the stderr warnings, until the
  kill). Every Linux gate launches through `stdbuf -oL -eL` now (netplayertest and netwidetest too).

