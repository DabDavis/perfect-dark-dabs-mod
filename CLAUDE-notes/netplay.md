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
- **A player's own look, and choices made in a menu** — the section of
  that name (protocol 15): the N64 or XBLA/HD look is each machine's own,
  F6 included mid-match; what a look decides at a load (GoldenEye's
  Community Edition copies, the PD release's collision, the guards' heads,
  Agent 4's numbers) is the host's on every machine (`NETLOOK_*`,
  `netLookData()`); characters go on the wire by row; Mod.SimBrain is the
  host's alone; a gun or device a client picks in a menu reaches the host
  (`NETCMD_EQUIP`/`NETCMD_DEVICE`); what of the host's look is still the
  host's world, and the start-of-match swirl that runs ahead on a client.
- **GoldenEye's end of a mission online** — the section of that name
  (protocols 16 and 17): a client sees the host's ending (its CameraSwitch
  shots and its lists' fades, from its own setup); the exit's wait for a
  press asks every player online (it
  had read whoever was current at the top of `lvTick`, a seat nobody
  held), a client's START reaches it and its screen fades with the host's;
  the mission ends on the folder's REPORT/STATISTICS for each machine's
  own player (a campaign host's in its menus, anyone else's for the two
  pages over the room), MATCH_END carrying each player's time, kills and
  hits; the same-frame `menuTick` that cleared `var80087260` before the
  Institute had loaded.
- **The online HUD (protocol 18, 2026-10-08)** — the section of that name
  (`nethud.c`): the feed of joins, leaves, drops, returns and spectators
  with the seats played ("3/8"), the chat (T; Space when no bind uses it;
  Space or T in the Game Lobby for the room's chat), the players panel
  (hold P) and the pause menu's Players page for pads; CHAT and PLAYERS
  on the wire, ROSTER at every GO; why ESC typed into the line never
  pauses, why the page is a row and not a tab, `netchattest.sh`; a
  player's name over its head while the crosshair is on it
  (`netHudAimFrame`, `Net.PlayerNames`, no wire change).
- **A client's tethered body (protocol 19, 2026-10-08)** — the section of
  that name: NETCMD_BODY carries the facing a client's Camera Tether gave
  its body and its steadied travel; the host poses that body by it (and
  never by its own tether setting), so the host, its lag compensation and
  every snapshot see it turned as the client does; the client's third
  person camera settings in its per-slot settings, swapped in around its
  `playerTick` on the host (a third person shot is fired from the camera).
- **A player's own settings (protocol 20, 2026-10-08)** — the section of
  that name: three classes of ini setting online (the host's rules, each
  player's own that the host simulates it by, each machine's own picture);
  NETKEY_PLAYER keys by name in SLOTCFG, the host's per-slot copy swapped
  in around a client's `playerTick` and read through `netSlotOwnS32`
  outside it (the HUD pass, `amTick`); a read about player Y during player
  X's tick (a tranquilizer hit); the drugged screen in the local-player
  block; why a setting costs nothing on the wire while it stays the same.
- **A host ticks every player's hands (2026-10-08)** — the section of
  that name: GoldenEye's slap ran twice as fast online (and its knife
  slash and throw were cancelled) because its swing state was one pair of
  slots for the machine; per player now. Any per-hand state in port code
  is per player.
- **F3 online (2026-10-08)** — the [netplay] section of a trace
  (`nettrace.c`): role, links, tick pacing, rules and own settings, every
  player's hands and swings, a client's commands as the host plays them,
  and every module's summary into the log "at F3".
- **Joining from anywhere, tried live (2026-10-08)** — the section of that
  name: six headless joiners in the user's own GoldenEye campaign room on
  the deployed lobby; the hole punch and the VPS relay both working
  between two home routers; a campaign room reopened after its first
  mission (nobody could join after that); the lobby answering a WireGuard
  peer from the wrong address; how to steer one joiner's traffic.

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
| online HUD | `nethud.c` (+ the Players page in `netlobbymenu.c`) | protocol 18: the feed of the host's notices and the chat, the chat line, the players panel, the keys (`Net.ChatKey`, `Net.PlayersKey`, `Net.ChatSpace`) |

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
| `netchattest.sh` | 4.5 min | protocol 18: chat on every machine in order, the host's burst limit (5 of 7, the sender alone told), the notices of a join in progress (with "3/4"), a spectator, a drop, a return within the hold, a hold running out and a spectator gone, a client's first PLAYERS, the host's pause Control page and Players page, screenshots of the feed, panel and open line |
| `netwidetest.sh` | 2 min | phase 8: a host and eleven clients (twelve games at once, alone), every slot 1-11 walks from its own commands; a room of two refuses a third |
| `netcooptest.sh` | 14 min (pair 4.5, twelve 2, lobby 1.2, ge 1.5, campaign 1.5, geend 2, camproom 1.5) | online co-op (spec-coop.md; ge: GoldenEye's Dam as a co-op mission, the client mounting the conversion on demand and taking the set from RULES): a host and one, four and eleven clients on Defection (eleven: stacked on one spot): the host's mission in RULES, every client loads and passes GO, the opening cutscene starts and ends on a client at the host's clock, the guards are posed from SETUPCHR records, prediction matches after the opening, a client's death and START respawn, the host's abort reaching every end screen and every client back in the menus; a lobby room created as a co-op mission; campaign: a GoldenEye campaign host starting Dam alone from its folder, the opening ending on its own player, a client joining in progress with no opening, on the host's spot, predicting at 95% or better; geend: Dam's own ending kicked on a campaign host with a client in, the client taking the host's outro shot and fades, its START skipping the outro on the host, both screens fading, each machine on GoldenEye's REPORT page for its own player (the client's kills the host's), no PD end screen, the client's NEXTs closing its folder; camproom: a GoldenEye campaign room on a local pdlobbyd launched by its host alone, Dam started from the folder and aborted, the room still launched, a newcomer connecting between missions and taken into Facility |
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
- **The Online Game page signs in as it opens** (2026-10-08): its tick
  calls `netLobbySignIn()`, a `JOB_SIGNIN` once per name and PIN
  (`netLobbySignInAgain()` on every open), so the first line reads "Signing
  in as X..." then "Signed in as X" or the refusal before anything is
  pressed; it used to sign in at the first Browse or Create. Its Sign In row
  opens the account page itself (ghostmenu.c), not Ghost Trials' list of
  remembered accounts, whose list the cursor could not leave upwards (fixed
  since, `MENUITEMFLAG_LIST_LEAVEATENDS`, the Briefing Room's room list too:
  Refresh and Create were out of reach from it); one
  Sign In there makes the account for a new name and the page closes when it
  has worked (ghost-trials.md, "One Sign In button"), and this page then
  signs into the lobby with it. Signed in, the row reads Change Account.
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
  `coopOtherPlayerNum` (the nearest living other past two; no longer the
  guards' switch, see below),
  `coopRespawnBuddy` (the living other with the most health),
  `coopAllDead`/`coopAllDeadDone`/`coopAnyAborted`,
  `coopAlternatePlayerProp` (cameras and autoguns cycle the players),
  `coopIsCoopPlayer`/`coopIsPlayerProp`. `chr->p1p2` is 4 bits (was 2).
  `mpReset`'s co-op branch makes a player per human slot; online the
  solo stash swap ([0]/[1] with [12]/[13]) is skipped on both sides and
  `menutick.c` goes back to the menus as after a match. A setup file's
  mine is owned by `COOP_SETUP_MINE_OWNER` (2 offline, 12 online: slot 2
  is a player online).
- **Which player a guard is after, past two.** A guard watches one
  player, `chr->p1p2` (its target when `chr->target` is -1; `chrInit` sets
  player one, the host online), and sight and hearing test only that one.
  The guard lists reach the others through `chr_toggle_p1p2`, often as
  "switch, test, switch back". The first widening went to the player
  nearest the *current target*, which walks a nearest-neighbour chain:
  a guard on the host bounced between the host and whoever stood by it
  and never tested a player off on their own, even one standing beside
  it (2026-10-08 user: "the AI enemies ignore the other players, only
  target the host"). Now (`coop.c`, all at two players the old answer):
  `coopToggleP1P2` has a home, the living player nearest the guard: from
  anyone else it goes home, from home to the next living player after
  `chr->coopturn`, so two switches come home and everyone is tested in
  turn. A player's gunfire (`chrsCheckForNoise`) or explosion
  (`explosionAlertChrs`, credited to `exp->owner` past two; owner 0 is also
  "nobody's", as stock's current player mostly was) turns a guard whose
  target is another player to the noise (`coopHearPlayerNoise`) unless it
  saw its target within a second and that one is nearer. A death sends
  each guard on the dead player to the living player nearest it
  (`chrsClearRefsToPlayer`). The check: the scratchpad harness of
  2026-10-08 put a guard beside a client that had walked 10 m off; before
  the change it kept the host for 600 ticks at alertness 0, after it shot
  that client.
- **A converted mission's guards (GoldenEye, Goldfinger 64, TND 64).**
  Their lists never call `chr_toggle_p1p2` (GoldenEye had no co-op), so the
  switch above never ran and every guard kept the host (user, in
  Goldfinger: "the guards are slow to target the other players though they
  are closer", "we want the guard targetting nearest player to it, for
  GE,GF and TND"). The converter's "Bond" is the chr's *target* for seeing,
  hearing, aiming, running to and distance (`IFISeeBond` is
  `if_can_see_target`), and `CHR_P1P2` (0x00f2 inlined, `geaitable.h`) for
  the Bond-only commands (give, equip, control, "is Bond dead"). So
  `coopRetarget` (called in `chraiExecute`, host only) re-chooses the
  **target** every ten ticks toward the nearest living player and leaves
  `p1p2` the host's: unseen for a second or dead gives way to the nearest; a
  seen one to a nearer player in sight at under 70% of the distance; on the
  nearest and two seconds unseen, one choice in four looks at the next player
  in turn. A noise there sets the target, not p1p2. Census (every armed
  guard within 30 m of a player, every 30 ticks, the host and three
  clients): guards whose nearest is a client targeting it, before/after -
  Dam 0%/89%, Goldfinger's Cartel 0%/69%, TND's Bazaar -/90%. A trap met
  measuring it: `chrMoveToPos` onto a player's own spot leaves the guard
  not running its list for hundreds of ticks; measure guards where they
  stand. Since GoldenEye's lists cannot switch at any count, the converted
  missions' rules apply from two players (`coopTurnsGuards()`); Perfect
  Dark's own two-player co-op stays stock.
- **A spectating player is hidden alone.** `modspectate.c` used to clear the
  global `g_Vars.bondvisible` (and `bondcollisions`): online, the host
  spectating hid every client from every guard, camera, autogun and
  simulant - the guards ran circles round the clients and never fired
  (user, 2026-10-08) - and took every client's walk off props. Now
  `modSpectatePropNoticeable(prop)` answers per player (the global still
  hides everyone during a cinema shot or the cheat) at each of the eight
  reads, the globals are left alone (the spectator's own movement is
  `modSpectateTick()` in place of the walk), and the co-op choices skip a
  spectator (`coopPlayerTargetable()`). The start options (pd.ini Start
  Spectating, `--spectate`, Spectator Start Game) went to whichever player's
  movement tick ran first on the stage - online a client's, dealt by
  `netHostOrderPlayers`; now only the local player's, and a client's none (the
  key still works: it rides the command's buttons to the host). Census with
  the host spectating from tick 900 (gdb `modSpectateSetOn` on player 0):
  guards that saw their target / guard-samples attacking a client, before
  and after - TND's Bazaar 0/0 and 10/23, Defection 0/0 and 8/24.
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
- **Spawns: every player on one spot (2026-10-08).** The user, on
  GoldenEye's Cradle and Archives: "the spawn point online is spawning
  players behind walls, outside levels ... might be easier to stack all
  players on same tile since collision is disabled for players only". Past
  two players the first life had been spread in a ring round the pad
  (`netCoopSpreadSpawn`, 60 or 90 units, nothing tested, and the empty
  seats counted, so a 12-seat room used 90 for four players and moved
  player 0 too): on Cradle's catwalk the host stood outside the railing.
  And PD's own pick moves a second player off the first
  (`chrAdjustPosForSpawn`'s eight directions, a line test a converted
  level's walls do not always stop). Online co-op players pass through each
  other (`g_NetPlayersPassThrough`), so now: `netCoopStackSpawn` - lvReset
  resets the players in order, the first picks the pad and the rest take
  its spot; `netCoopSpawnPick` - a respawn's pad picked with players no
  obstacle (the flag set round `scenarioChooseSpawnLocation`); a join in
  progress on its buddy's own spot (`netCoopJoinSpawn`, a buddy in a
  vehicle or off the ground passed over for one on its feet). Probed on
  the host with four players: Cradle, Archives, Defection all on the one
  spot (an opening that walks player 0 leaves it up to 34 units on), the
  late joiner on player 0's. netcooptest's twelve now checks the stack
  (clients on one spot within 2 units, within 100 of player 0), campaign
  "spawns on player 0"; prediction 99.7%+ stacked.
- **Players are no cover for each other online (2026-10-08).** With the
  stack deployed, the user in the live Archives room: "the guards are not
  able to shoot the bots, but can shoot me well", "im hiding behind them
  and the bots act like a wall to protect me". A solo guard's shot at a
  player lands only if `chrHasLosToAttackTarget` passes, and its line
  (`chrHasLosToEntity`) tests `CDTYPE_ALL`, players included, with only the
  target's own perimeter off: a stacked player stood inside the others'
  cylinders and could never be hit, and anyone behind the group was
  covered. The bullet itself already passes a player it was not aimed at
  (`chrTickShoot`'s `cdExamLos08` types leave players out), so online co-op
  now drops `CDTYPE_PLAYERS` from that test too. Cradle, four players, the
  three clients stacked and standing, no god mode: before, 5-7 guards
  attacking a client for 100 s and every client still on 1.000 health;
  after, all four hurt, three dead by tick 4800. Sight tests
  (`chrHasLosToPos` and friends) never counted players. A
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
  unfounded. A mission ends on the folder's REPORT and STATISTICS pages
  online too, each machine's for its own player (protocol 16: "GoldenEye's
  end of a mission online", below); Perfect Dark's missions keep PD's co-op
  end screen.
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
  and `playerStartNewLife` puts that one life on a living player's spot
  (`netCoopJoinSpawn`: `coopRespawnBuddy`'s pick; until 2026-10-08 PD's
  own `chrAdjustPosForSpawn` 60 units round it) rather than at the mission's
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
  host's own player (nothing on the wire carried a choice; PD's pause
  inventory online was the same: protocol 15's NETCMD_EQUIP since, "A
  player's own look" below); the host's own player is unarmed to the
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
  refused every joiner while its own Mod.SimBrain was "modern", and an HD
  host one whose Mod.XblaMeshes differed (both met setting this up; both
  gone with protocol 15, "A player's own look" below).
- **Seen once: netcontenttest geyolt, the client's START respawn missed
  (2026-10-07).** In the gate run for the commits above, the client took
  its first death from the host's block, pressed START every second, and
  never came back ("deaths 1, respawns 0", 935 ticks out, no revive), while
  the host had respawned it (the second staged kill found it alive). The
  same binary passed the case alone three times after. **Traced the next
  day (three in six runs): the staging, not the game.** gdb's attach stalls
  the host while the client goes on sending; the first tick after it plays
  the queued START (the respawn, `playerStartNewLife` at 1099) and the
  step's breakpoint at that same tick's end killed the player again, before
  `netLpTrack` sampled it: dead at 1098's end, dead at 1099's, so no block
  ever counted the respawn (nothing out of step: the client stayed dead,
  as the host had it). `stage()` now runs a step 30 ticks after the attach
  (`ignore 1 30`): 4/4. A life shorter than a tick is still never sent;
  the client then stays dead with the host, which is right.
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
  still refuse rather than follow; a client's prediction held during
  GoldenEye's opening.
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

## A player's own look, and choices made in a menu (protocol 15, 2026-10-07)

The user: "players should be able to have xbla or n64 mode active without
affecting other players", then "the players and host being decoupled as
much as possible without losing sync is best". Before this a host refused
a joiner on a GoldenEye stage whose Mod.XblaMeshes or
Mod.GeXblaCommunityEdition differed (MUST_GE), F6 and Mod.XblaMeshes were
locked in a match (H14), and on any stage a differing Mod.XblaMeshes could
get a client refused by the stage hash's `rng`. Two read-only audits (the
GoldenEye HD look, the PD release's parts) listed every place a look
reaches the simulation; what came of them:

- **What a look decides at a load is the host's** (`netContentLookLatch()`
  at the host's match start, RULES' content block carries `look`,
  `netLookData()` answers on both sides, offline 0 so the local look
  decides as before):
  - `NETLOOK_GECE`: GoldenEye's Community Edition copies of a mission's
    setup, pads, tiles and stan (`geRoomCeData()`). The converter writes
    the `_ce` files into every conversion (they are the ROM with
    `g_RomPatches`, geconvert.c), and a served conversion carries them, so
    any client follows whatever its look and whether it has the CE zip.
  - `NETLOOK_XBLATILES`: the PD release's collision on Area 51 and MP Ruins
    (`xblaStageLoadTiles()`). It followed whether the package was
    *unpacked*, which any F6 does once: an N64 player with the release
    still in its .7z differed from one who had looked once. A client takes
    the host's choice, unpacking its own copy if it must
    (`xblaMeshPackageReady(1)`); one with no release at all is still
    refused, now with the xblatiles text (`netCompsDiffer` looks at
    `xblatiles` before `tiles`, which differs too and used to win).
  - `NETLOOK_MESHES`: the release's meshes take Penny out of the male
    guards' heads (bodyreset.c), which changes the RNG's draws at the load
    on every stage; and Agent 4's numbers (xblaagent4.c,
    `xblaAgent4TakeNumbers`: height, scale, animscale, the random height -
    the Shock Trooper's row varies, his does not, so the N64 machine drew
    one more random number and was refused every time he played). His
    model stays this machine's look's; the rows are applied again at every
    stage reset.
- **The look itself is live in a match**: H14 is gone from
  `xblaSwitchSetParts()` and `xblaMeshSetEnabled()` (the other parts'
  checkboxes never had it). `--net-test-look-switch TICK` switches as F6
  does at a match tick.
- **Characters by row** (`netWriteMpChar`/`netReadMpChar`, RULES' sims and
  humans, CONNECT/SLOTCFG): Agent 4 is listed only where the release is
  unpacked, ahead of GoldenEye's characters, so their list indexes differ
  by one between machines. An index goes with the row it names and the
  reader takes its own index of it; one past the list as how far past; an
  Agent 4 the reader does not list is the Shock Trooper (his N64 look).
- **Mod.SimBrain is the host's** (out of `s_NetKeys`; `simbrainWanted()`
  is 0 on a client): simulants are ticked on the host alone and modern
  draws no random number. netsessiontest's loopback runs both with modern.
- **A multiplayer room's visibility** (bg.c, `g_MpRoomVisibility`) asks
  `bgRoomIsPortalVisible()` as solo's `bgRoomIsOnscreen()` does: on an HD
  level every room is ROOMFLAG_ONSCREEN, so in a match or a co-op mission
  nobody spawned "off screen" (Ourumov's squad, offline 2P co-op too), no
  simulant went cheap and lag compensation rewound everyone. Identical in
  the N64 look.
- **A menu's choices reach the host** (`NETCMD_EQUIP` u8 right, u8 left;
  `NETCMD_DEVICE` u8 weaponnum, u8 on; after a command's fixed part, only
  when its flag is set, resent with it until acked, folded by the later
  one): PD's pause inventory (mainmenu.c, guns and devices) and GoldenEye's
  watch (`g_Watch.picked`, sent as the hand comes back). The host applies
  them at that player's pass (`netPlayersHostApplyChoices`, from
  `netRemotePassBegin`) when the player holds what was picked; the
  client's own equip stands, and the host's block agrees before
  `NET_LPSTABLE` would take it back. `--net-test-equip TICK,WEAPON`;
  netcooptest `ge` checks it (the host took the pick a tick later, the
  hand still the pick three seconds on).

**Still the host's world** (the host simulates everyone; its look decides,
for all, details the same level has in both looks): shots against the
rooms it loaded (the release's PD rooms carry two to four times the
triangles; HD GoldenEye rooms are Bean's), translucent and cut-out
surfaces, hit textures and their RNG, the release's meshes in projectile
and object hit tests (chr shots online are boxes: `cheap`), landed
projectiles, held guns and hats moved onto the release's meshes, head
boxes seated by headfit, the XBLA tables (smoke types, Crash Site's fog,
head types), props ticked in the foreground. None of it is on a client's
prediction path, and none desyncs. Found on the way and fixed after: on
the release's PD rooms a hit's texture was read out of the header before a
SETTIMG's image, which for a release-only record is the stand-in tile's
malloc header (xbla.md, "A shot at a release room's stand-in").

**The start-of-match swirl runs ahead on a client.** netcontenttest
gelook (an HD+CE host, an N64 client, Bunker) predicted at 97.8%, all of
its 24-25 corrections (about 41 units, the eye against the body's root)
before tick 130; the same with both N64 gave 2-8, both HD 10. Not the
look: the client's `TICKMODE_MPSWIRL` advanced about 14 steps at
`g_NetTick` 0 (gdb on `playerTickMpSwirl`) while it waited for the host's
first ticks, so its swirl ended ~20 ticks before the host's, and until the
host's ended the host's block had the player at its body's root (the swirl
is third person: chrTick puts prop->pos there) where the client's bwalk had
the eye. A slow-starting host (the HD look builds meshes in its first
ticks) makes the gap; nothing is out of step after it. Left as found.

**Gate changes.** netcontenttest `gemust` (a refusal) is `gelook` now (it
plays, every stage hash component equal, the client's own N64 look at tick
900, both switch looks in the match); netsessiontest's `r-notstock` case is
gone, its loopback runs Mod.SimBrain=modern on both (the host's modern, the
client's stock); netcooptest `ge` has the pick.

## GoldenEye's end of a mission online (protocol 16, 2026-10-07)

The user, after playing GoldenEye's Dam online: "the outro cannot be
skipped, then the completion screen goes to a PD screen instead of GE".

- **The exit's press** (`gexPlusMissionExitTick`, GoldenEye's
  TriggerFadeAndExitLevelOnButtonPress: the list says the mission is over,
  the next press fades out and leaves; Dam's comes as Bond's dive starts).
  It read `g_Vars.currentplayerstats`' pad, and the tick runs at the top of
  `lvTick`, outside every player's pass, with whichever player the last
  drawn view left current: a campaign host has four (a seat per member
  slot), and the order puts the open ones last, so the host's own press was
  never read. A client could not skip at all: it runs no AI list, so never
  starts the wait, and the host strips START from a living player's pad
  (protocol 11). Now, online, `gexPlusExitPressed()` asks every player:
  this machine's pad and Esc, and each client's raw command
  (`netPlayersHostPressed()`: the press edge of the command played this
  tick, START included). Offline is the current player's pad as before.
- **A client knows of the wait**: the mission block's flags carry it (8
  waiting, 16 fading; `gexPlusExitFromHost()`), so its START is the press
  rather than its pause (bondmove.c skips the pause while
  `gexPlusExitPending()`; offline too, where the wait has the controls
  locked anyway), its Esc goes as START in the command, and its screen
  fades with the host's (`lvConfigureFade` once). It never ends the level
  itself: MATCH_END does.
- **The report** (`gexFrontNetMissionReport()`, from mainEndStage's co-op
  branch for each local player): a mission of a conversion's set ends as
  offline, on the folder's REPORT then STATISTICS pages, not PD's co-op end
  screen. The end is co-op's (completed unless all dead or one aborted;
  killed in action only on a mission that was not completed); the mission
  number by its stage (a client's folder never started it); the folder's
  difficulty where it started the mission, else `lvGetDifficulty()`;
  nothing filed (no best time, no ghost). The level is left at once
  (`netCoopLeaveMission()`, the end screen's close for online co-op). At
  the Institute `netMenuAfterMatch()` takes it: a campaign host's menus
  open the folder on the report (`gexFrontOpenAfterMission`, as offline;
  NEXT goes on to the next briefing, which it starts for everyone); anyone
  else gets the folder for the two pages alone over the room
  (`gexFrontOpenNetReport()`, the mission's own set put in
  `g_GexPlusVariant` while it is up, since the rules' is off again), and
  NEXT from STATISTICS or BACK closes it to the room. With no room (a
  `--connect` session) the Perfect Menu goes under it: the folder is drawn
  in place of the menus and nothing was up. A STAGE_LOAD closes it first
  (`gexFrontCloseNetReport()` before the rules are applied).
- **MATCH_END carries each player's numbers** (time, kill count, seven
  shot counts, after its awards): a client's guards are puppets whose
  deaths and hits are counted on the host alone, so a client's STATISTICS
  (and PD's co-op end screen) had zero kills. A co-op client takes them as
  its own at H10; a match's are not applied (its tables follow as before).
- **The ending's shots on a client (protocol 17; the user: "let guests
  see the host's outro camera too").** GoldenEye's CameraSwitch is ai00df,
  Perfect Dark's warp (`playerPrepareWarpType2`, TICKMODE_WARP, the camera
  at a setup record's place); a client runs no list, so it played on in its
  own view while the host's showed Bond's dive. The host notes the
  record's setup command (`netCoopHostCameraSwitch`, from ai00df) and every
  screen fade a converted mission's lists ask for (`netCoopHostFade`, from
  aiFadeScreen: Dam fades to black between its three shots); the mission
  block carries the shot while the warp is still that one (s16 command,
  s16 direction word; an opening's swirl and the credits are warps of
  their own and leave it -1) and the fades (a count, colour, length). A
  client takes the same record from its own setup
  (`setupGetCmdByIndex`, `netCoopApplyWarp`), leaves the shot when the
  host's tick mode does (then `netPredictForceSnap`), and holds prediction
  as in a cutscene (`netCoopFollowingWarp`). The shot that looks at the
  player (direction word 1) looks at Bond's prop, `g_Vars.bond` - the
  host's player on every machine and for every view (it was the current
  player's, the guest's own body on a client; offline the same prop).
  Filmed on loopback: the client took Dam's shots 322, 324, 326 and four
  fades, about 50 ticks behind the host under the screenshot attaches.
  Not carried: who stands in the shot - HideAllChrs leaves every player's
  chr on a converted mission, so a guest beside Bond is in it, as the host
  sees it.
- **Trap: a match left from inside its own tick.** The co-op leave sets
  `var80087260` and changes stage from inside `lvTick`, and `menuTick` runs
  later in that same frame: it saw the flag with `lvframenum >= 4` on the
  mission's own stage and cleared it, so the Institute came up with no
  menu. The end screen's close never met it (it sets the flag in
  `menuTick` itself, after that check). The return-from-match branch now
  waits while `g_MainChangeToStageNum` is pending.
- **Gate:** netcooptest `geend` (Dam's list 0x1004 kicked to its exit body,
  the first label 7, found by walking the list in gdb; the client's player
  given 3 kills and 20 shots on the host; the client taking the host's
  first shot and a fade to black). Harness by hand: scratch
  `run.sh` beside it drove the host's own START (`buttonspressed[0]` at a
  `gexPlusMissionExitTick` break), the host starting Facility while the
  client read its STATISTICS (`set var 'gexfront.c'::g_Front.mission = 1`,
  `call (void)frontStartMission()`), and screenshots of each page.

## Joining from anywhere, tried live (2026-10-08)

The user, hosting a GoldenEye campaign room on the deployed lobby from
10.8.0.3: "please join with 5 other players so I can test", then "how can
we make this easier for players abroad, so anyone can play anyone".

- **Live joiners.** `build/netbots/` (gitignored): a modless guest folder
  (the binary hard-linked from `build/`, `data` symlinked, empty `mods/` and
  `added-content/`), `saveN/pd.ini` with `[Mod] GhostUser=netbotN`,
  `GhostPin=`, `run.sh N` = `--net-lobby-script join --net-lobby-room NAME
  --net-lobby-leave-frame 0` offscreen. Accounts netbot1-6 are real ones on
  pdghostd. The join script needs nothing more for a launched room:
  `netLobbyTick` connects by itself while the script waits at step 2.
  Four modless joiners fetched GoldenEye Arenas (12.7 MB, 2905 files) from
  the host at once in 25 s each over the internet; 0 resyncs, ~130 B a
  snapshot. Harmless: `$N/GoldenEye Arenas/menu/gewatch.bin` not found
  (the converter writes it for hack variants only).
- **Trap: this box sends everything down the tunnel.** wg-quick here has
  `AllowedIPs = 0.0.0.0/0`, so this box's internet traffic, the lobby's
  rendezvous included, leaves through 10.8.0.1 - not the user's network
  (an early guess that both share a public address was wrong). Its own
  line is enp5s0 (public 158.62.150.136, a different network from the
  user's 166.113.108.166). The host advertised 192.168.1.234 (its LAN) and
  166.113.108.166:27100 (no port forward), neither reachable from here.
- **The lobby answered the tunnel from 10.8.0.1.** pdlobbyd's UDP bound to
  0.0.0.0 replied to a REGISTER that came in over wg0 from 10.8.0.1, and
  netrdv.c drops anything not from the rendezvous it sent to: "the lobby's
  rendezvous did not answer; no path from it", so no punch and no relay
  (tcpdump on the VPS showed every REGISTERED and PEER going out). The unit
  now binds `PDLOBBYD_UDP_HOST` to the public address (the relays bind
  there too; a RELAY_OFFER carries only a port). Players outside the VPN
  never met it.
- **Steering one joiner.** Tunnel joiners reached the host through an nft
  table `netbots` here (`dnat` 192.168.1.234:27100 to 10.8.0.3, masquerade
  on wg0; a client that tried before the rule needs a restart, conntrack
  keeps the old flow). A real-network joiner runs as its own unit
  (`systemd-run --user --unit=netbot6 ... bash -c "sleep 8; exec ..."`, the
  sleep so the rules can name its cgroup before its first packet) with a
  table `netbots6`: a `type route hook output` chain marking its UDP
  0xca6c (wg-quick's own mark, so the main table and enp5s0 carry it) with
  `ct mark` for the replies, masquerade on enp5s0, and a `return` for its
  cgroup at the top of `netbots`' dnat. nft resolves a `socket cgroupv2`
  path when the rule is loaded: a restarted unit needs its rules again.
- **What the ladder did, live.** From the real line to the user's home
  router: REGISTER seen as 158.62.150.136, PEER, a punch through both NATs
  in 45 ms - no port forwarding anywhere. With the host's three addresses
  dropped for it (166.113.108.166, 192.168.1.234 and 10.8.0.3 - the host
  also lists its WireGuard address, which the main table reaches over wg0,
  and the first try punched there): "no direct or punched path in 4500 ms;
  asking the lobby for a relay", port 27110 offered and bound at both ends,
  relay path 54 ms. The match over the relay was not played: the room was
  open by then (next bullet).
- **A campaign room reopened after its first mission.** `lobbyHostTick`'s
  "back from the match" reopened every room. A campaign's missions after
  the first start from the host's menus over the session and never launch
  the room again, so a member who came after mission 1 waited READY in an
  open room for good (netbot6 did). A campaign's room now stays launched
  while `netCoopCampaignOn()` ("the mission is over; campaign room ... stays
  launched for the next"): a newcomer connects with its ticket between
  missions and the next STAGE_LOAD takes it in, as the earlier late-join
  bullet meant. pdlobbyd reaps nobody in a launched room, so a member whose
  game dies stays on the roster (its ticket brings it back) until it leaves
  or the host kicks it. Gate: netcooptest `camproom` (fails 6 of 11 on
  the build before, all pass after).
- **Not done.** A client whose session ends mid-campaign (its own Abort
  Mission, a refusal) is not connected again by itself: the launch it took
  is handled, and only a fresh Join gets a new one. Router port mapping
  (UPnP/NAT-PMP/PCP), the room's empty `region`, IPv6 and a TCP fallback
  for networks that drop UDP are the next steps the user was offered.

## The online HUD (protocol 18, 2026-10-08)

The user: "lets add online play hud, when players join the game in
progress. also in game player list and count. in game text chat also",
then mid-way: "if space doesnt do anything lets also make chat open for
lobby and ingame automatically with space and the keyboard can
immediately type without choosing type with keyboard".

- **What is on screen** (`port/src/net/nethud.c`, drawn by `netHudRender`
  from lvRender after the modal text, the local view only): the feed at the
  top left, oldest first (ten seconds a line, the last ten while the chat
  line is open, below the panel's foot while it is up, no lower than 60% of
  the view; it was low on the left until the user's "lets move the chat to
  the top left of the screen", where the stock pickups and kill messages
  sit); the chat line under its newest line; the players panel
  at the top while P is held (not with the line open: twelve rows would
  leave the history three lines; seats played or kept,
  score and deaths from `scenarioCalculatePlayerScore` in a Combat
  Simulator match, the host's ping for each, open seats, spectators, the
  count). The key hint ("T: chat    hold P: players") is the feed's first
  line each time the HUD goes live.
- **Who says what.** The host makes every notice (`netHostNotice`, from
  `netHostLateGo`, `netHostPeerGone`, the hold running out in
  `netHostSeatsTick`) and sends it as a CHAT of kind 1 to everyone in the
  session; clients never work joins out for themselves. A client's line
  goes to the host, which cleans it, holds it to a burst of 5 and one per
  1.5 s (`chattokens` on the netclient; past that kind 2 back to the sender
  alone) and sends it to everyone with the sender's seat and name (its
  CONNECT name, which is the seat's name; a ticket's account). The host's
  own line goes out the same way. Names and text are printable ASCII
  only (`netChatClean`), both ends.
- **ROSTER at GO.** A client knew the seats only after a change, so its
  list could not tell an open seat from a taken one at the start of a
  `JoinInProgress` match. The host now sends ROSTER right after the GO of
  a match's start too; `s_ClSeat` keeps it on the client.
- **PLAYERS** every 60 host ticks, unreliable: each seat's ENet round trip
  as the host measures it (the host's own seat 0), the spectators by name.
  ENet's RTT includes each end's service interval, so loopback reads 8-50
  ms and a fresh peer starts high and settles.
- **ESC typed into the line never pauses.** `netHudFrame` runs in
  schedEndFrame right after `inputUpdate`, before any tick asks
  `inputKeyJustPressed(VK_ESCAPE)` (the pause in bondmove.c, a spectator's
  leave, a menu's back), and calls it itself every frame the line is open,
  which spends the press. The keys that open the line are read with
  `inputKeyPressedThisFrame` instead, which spends nothing: a hotkey bound
  to the same key still sees its press. While the line is open input.c's
  `textInput` drops the keyboard's binds (as for a menu's keyboard), so the
  player stops and the mouse still aims; the Enter that sends is latched by
  `inputStopTextInput` and is neither the menu's OK nor the N64 binds' START.
  GoldenEye's watch (its close) and cinema (leave and skip) read the
  frame's ESC with `inputKeyPressedThisFrame`, which nothing spends: they
  ask `netHudAteEscape()` (the line open, or its ESC this frame) first.
- **Space.** Space is the second Fire bind in the PC defaults (`CK_ZTRIG`)
  and Z is Select in the menus. In the Game Lobby Space opens the line
  always (its Select is a duplicate of Enter's, and a stray one could hit
  Leave); in a match only when no bind of player 1's uses it
  (`netHudSpaceFree`, `Net.ChatSpace` to turn it off). The line opens with
  text input already on, so the keyboard types at once - no keyboard to
  pick on screen; a pad player's **Chat...** row is still the on-screen
  keyboard (17 characters, MPSETUP_MAXNAME, `handlerChatKeyboard` now says
  it through `netHudSay`: the room's chat in the lobby, the match's in one).
- **Why the pause menu has a row, not a tab.** `menuPushDialog` builds at
  most five siblings, and a team match's pause chain is already five (Team
  Ranking, Ranking, Stats, Inventory, Control): a sixth would have pushed
  Control, End Game with it, off the end. So the Players page is a row on
  Control (ingame.c) and on the mission pause (mainmenu.c, both lists),
  hidden offline by its handler. Its rows are labels, and a label's size is
  taken when the dialog opens: the page lists the seats as they were then,
  their names and pings live.
- **Harness flags** (nethud.c): `--net-test-chat "TICK:TEXT|..."` (up to 8),
  `--net-test-chat-type TICK:TEXT` (the line opened with TEXT),
  `--net-test-players TICK` (the panel from then on),
  `--net-test-players-page TICK` (the pause's Control page, three seconds
  later the Players page; from `netHudTick` in netTickEnd, a tick's
  context, as menus want), `--net-test-lobby-chat-type TEXT` (the Game
  Lobby's line). A client logs its first PLAYERS of a match ("net: players
  (tick N): seat pings 0:0 1:12; 0 watching").
- **`--net-test-chat-echo`**: a headless joiner answers every line from a
  name not starting "netbot" with "NAME said: TEXT", so a player testing
  with the netbots has someone to talk to.
- **A player's name over its head (2026-10-08, the user: "names over
  players heads, that only show up when the cross hair is hovered over that
  player", then "also for co-op so players can see who each char is being
  controlled by").** `netHudAimFrame` runs in lvRender's pass right after
  the stock `lookingatprop` block, the local player's pass only (a remote
  player's on the host returns at once). The stock block asks
  `propFindAimingAt` only with one player or in co-op (the N64's cost), so
  the hook asks it itself for every net match: the bullet's own query
  (`shotCalculateHits` with `isshooting` false), so a wall stops it where
  it would stop a shot and the hit is on the puppet as this machine draws
  it. The query writes the gun's dot (`hasdotinfo`/`dotpos`/`dotrot`, read
  by the laser sight and a thrown gun's aim): the hook puts them back as
  `bgunAimThrowAtCrosshair` does, so the sim is the same with it. A
  player's prop only (simulants and guards have none), alive, not
  cloaked but to the IR scanner, not spectating (`modSpectatePropNoticeable`).
  The name's place is the top middle of the box the stock target box is
  drawn from (`modelGetScreenCoords`, valid in that pass: the model's
  matrices are this camera's), worked out every frame while the name
  shows; `netHudRender` draws it only on the frame it was worked out
  (`s_TagFrame == s_Frame`). It stays 250 ms after the crosshair leaves
  and fades over 200 ms, so a strafing target does not flicker. The name
  is the seat's (`netSessionSeatInfo`, as the panel and the chat have it:
  a direct host without a room is its profile's name, "Player 1" in a
  fresh save, not `Net.Name`); a team match's names are the team colour
  lightened 40% toward white. Checked with a scratch harness (host and two
  clients on Temple with `--net-test-aimat` at each other; co-op Defection
  with the host turned by gdb, `--net-test-aimat` does not turn a host's
  own player): each client showed the other's name, the host the
  client's, the host's view of nobody none.
- **Seen with five netbots joining the user's TND64 campaign (2026-10-08),
  not fixed:** every bot segfaulted seconds after its GO in progress -
  unbounded recursion in func0f0706f8 (propobj.c) from chrTick: a chr's
  child prop list loops on the client. Two later single joins at other
  points of the mission ran on, so it depends on what the mission has in a
  chr's hands at the join. Next session: catch it with the cycle walker
  (a gdb script walking `prop->child`/`next` from chrTick's prop).
- **Co-op players pass through each other (2026-10-08, the user: "players
  may need to be able to move through each other, but not shoot through
  each other", "only coop").** Five netbots joining in progress were put
  beside the host (netCoopJoinSpawn) and boxed it in. `bmoveTick` sets
  `g_NetPlayersPassThrough` around a player's own movement on an online
  co-op mission (`g_Vars.coopplayernum >= 0`, set the same way by
  netCoopHostStart and netCoopClientStage), and `propIsOfCdType` then takes
  no player prop as an obstacle - the one filter every movement test uses.
  The host and a client's prediction run the same `bmoveTick`, so they agree.
  Shots never ask `propIsOfCdType` (hits are model tests), so players still
  take each other's fire; guards still block and are blocked. A Combat
  Simulator match keeps players solid.
- **Also seen in that test, not fixed (next session):** a leaving player's
  guns float at head height on the host (netSeatVacate's
  currentPlayerDropAllItems: never let go of, or the projectile never
  ticks - its view is skipped once the seat is out of play); "dying black
  screens" in the co-op mission (the user's words; ask what they saw).

## A client's tethered body (protocol 19, 2026-10-08)

The user, after the host's own tethered body was fixed (third-person.md,
"A body is ticked in every view's pass"): "do the protocol change by
sending each clients body facing to the host".

- **Why the host could not work it out.** Camera Tether (`Mod.ThirdPersonTether`,
  Body Turn Speed) is each machine's own `g_ModOptions`; nothing of third
  person was in SLOTCFG. The host ran `playerTetherBody` for a client's
  player with the host's setting, or (host tether off, or the host not
  thinking the client in third person) posed the stock body facing the
  look. A client's own screen was right (its own pass), everyone else's
  wrong: client 2 drew client 1 backing up facing its camera, 0 degrees,
  where client 1 saw its body turned 180.
- **The wire.** NETCMD_BODY (0x08) in a CMD's command: u16 facing, u16
  travel (65536ths of a turn of the game's radians), u8 bodyflags
  (NETBODY_TRAVEL while the travel is held). A client sets it on every
  command while its tether poses the body (`playerTetherBodyState`: the
  setting on, third person, `thirdpersonbodyset`), so a command without it
  means none. `netFold` takes the later command's body (or none). A dry
  tick holds the last command, flag included.
- **The host.** `netPlayersHostBody(playernum)` reads the command the tick
  plays; `playerTickThirdPerson` poses a client's body by it
  (`playerTetherBodyRemote`: the facing, and the speeds read against it
  for the animation chooser, as `playerTetherBody` does) in every pass, its
  own included (`playerTetherBodyRemoteWanted` is in the block's condition:
  the host need not think the client in third person, and without it the
  client's own pass left the body to its movement, facing the look, for
  the snapshot). The host's own tether now poses the host's player alone
  (`netIsLocalSlot` in `playerTetherBodyActive`/`Held`). The body reaches
  everyone as any chr's yaw (`modelGetChrRotY` in the chr record).
- **Checked** (scratchpad harness: host + two clients on Temple, gdb turns
  tethered third person on for client 1 only, which backs up on C-down):
  host's pose of client 1 and client 2's drawing of it 0 degrees from the
  look before, 180 after in every sample. pd-nettest passes.
- **The camera too (same protocol).** The user asked "do we only have that
  issue with tether or all", then "yes add the camera settings too":
  `playerPullBackCameraNow` and its helpers read `g_ModOptions.camdist/
  camclearance/camside/camfwd/camheight/camtether` (the rod) and
  `cameratilt/tiltinvert/tiltforward` for whichever player's `playerTick`
  it is, and a third person shot is fired from the camera, so the host had
  built a client's camera, and fired its shots, by the host's own settings.
  The per-slot settings (CONNECT and SLOTCFG, `netWriteSlotCfg`) now end in
  those nine, and `lvTickPlayer` on the host wraps a client's `playerTick`
  in `netPlayersCamBegin`/`End`: the slot's values (clamped to the
  options' registered ranges) in `g_ModOptions` for that tick, the host's
  own back after. A swap rather than a per-player accessor because every
  read is inside that one call, `modoptions.c`'s tilt helpers included.
  SLOTCFG goes at connect and again only when the encoding changes (the
  client compares every 30 ticks; `s_SlotCfgSent` is 256 bytes now, the
  block 100). Body Fade is drawing only; Body Turn Speed's body comes in
  NETCMD_BODY. Checked: a client at Distance 400, Sideways 80, Height 40,
  third person on both ends by gdb: the host's camera for it 200 straight
  back before, 410 (400, 40, -80) after, the client's own 410; the host's
  own camera still its 200.
- **Bandwidth.** NETCMD_BODY is 5 bytes on each command while a client's
  tether poses its body (the CMD resends every unacked command, so a few
  of them a packet): of the order of 1 KB/s up from such a client, against
  about 20 KB/s of snapshots down.
- **Superseded in protocol 20** (next section): the nine camera fields
  and `netPlayersCamBegin`/`End` became NETKEY_PLAYER keys and
  `netPlayersOwnBegin`/`End`; the swap is the same, the clamp is now the
  ini registration's own range.

## A player's own settings (protocol 20, 2026-10-08)

The user, after protocol 19: "the host mod settings, etc. affect the
clients. we want clients to be able to use their own mod settings/settings
... also is this optimal? we do use deltas to update clients, so most
settings would remain static."

- **Where the host's settings leaked in.** Two ways. RULES' SYNC keys put
  the host's COD Style Aiming (and with it Aim Lock), Quick Weapon Swap,
  Skip Death Screen, Disable Fog, Glass See-Through and Tranquilizer Effect
  over a client's own for the whole match (spec-stage.md had them SYNC for
  prediction and, for the last three, fairness). And the host simulated a
  client's player by the host's own values of whatever it read during that
  player's tick or HUD pass. Probe (scratchpad own/run.sh: host all off,
  client all on): before, the client's own values read 0 during the match
  and the host played it with 0; after, 1 on both, the host's own player
  still 0.
- **Three classes now (netrules.c's header).** The host's rules (SYNC,
  MUST...: jump, roll, melee, flinch since it moves hitboxes, Start Armed,
  bodies, akimbo, the guards, respawns and lives, GoldenEye's guns and
  region). A player's own that the host simulates it by (NETKEY_PLAYER: COD
  Style Aiming, Aim Lock, Akimbo Triggers, Quick Weapon Swap, Skip Death
  Screen, the third person camera and the tilt). Each machine's own,
  never on the wire: anything that is only the picture or the sound, now
  Disable Fog, Glass See-Through and Tranquilizer Effect too. The user chose
  "all own" for the four that give an edge (Quick Weapon Swap, fog, glass,
  tranquilizer): every player has the same options in their menu.
- **The wire.** SLOTCFG (and CONNECT) end in `u8 n` and (str key, VALUE)
  for each NETKEY_PLAYER key, written from the table
  (`netRulesWritePlayerKeys`), so a new player setting is one table line.
  By name, not position: a key the host does not list as a player's own is
  read past and the host plays it by its own value. ENet fragments reliable
  sends, so the ~500 bytes are no concern; `s_SlotCfgSent` is 1024.
- **Is it optimal (the user's question).** Settings never go in the
  snapshot deltas: those are host to client, settings client to host.
  SLOTCFG goes at connect and again only when its bytes change (compared
  every 30 ticks), so a setting that stays put costs nothing after the
  join. A change mid-match reaches the host within half a second plus the
  ping; until then the client predicts by its new value and the host plays
  the old one, and the corrections cover it.
- **The host.** `netRulesReadPlayerKeys` resolves each key to this
  machine's variable once (`configGetEntry`) and clamps the value with
  `configClampValue` (the registration's range; a NaN float, a string or a
  type that differs is dropped). `netPlayersOwnBegin`/`End` swap the whole
  set in around a client's `playerTick` (lv.c `lvTickPlayer`): many reads
  there are straight `g_ModOptions.x`. **Reads outside that window** are
  the trap: Skip Death Screen is read in `playerRenderHud` (the HUD pass)
  and Akimbo Triggers in `amTick`, which loops the players itself. So
  modoptions.c's getters for the integer player keys go through `modOwn()`
  -> `netSlotOwnS32(currentplayernum, &field, mine)` (the slot's value by
  the variable's address), right inside or outside the window. A new
  player key read outside `playerTick` needs its getter on `modOwn`.
- **A read about another player.** A tranquilizer hit is read during the
  shooter's tick (`setCurrentPlayerNum` to the victim, but the swap is the
  shooter's). Tranquilizer Effect is each machine's own, so the host now
  drugs a remote victim whatever its own setting says
  (`!netIsLocalSlot`), and the client's setting decides its screen.
- **The drugged screen online.** Before this a client never saw it: the
  amount lived in the host's chr only. The local-player block carries
  `u16 blurdrug` (byte 200, from the zero tail) and the client sets its
  chr's `blurdrugamount` from each block; lv.c runs it down between
  blocks. Checked: host sets client 1's to 4000, the client reads 3926 a
  second later (0 before).
- **Not swapped: the host's passes for a client's view.** The host's
  drawing of a client's view (fog's room walk, glass's portal, a chr's
  past-the-fog flag) uses the host's picture settings. That only moves
  what the host considers on screen, which is the host's world anyway;
  snapshots are not culled by view.

## A host ticks every player's hands: GoldenEye's slap, knife and throw (2026-10-08)

The user, hosting: "unarmed punches are incredibly rapid online", "also
the sound fires fast", "it only happens online", then "maybe because slap
is not working". Every reproduction on Perfect Dark's stages (Temple,
Defection; offline 39 swings in 10 s, online 40-42, at 60 or 144 fps,
with latency and loss, with the user's own pd.ini, with real input held
under Xvfb) was normal: Perfect Dark's fists keep their state in each
player's hand. On a GoldenEye level unarmed is GoldenEye's slap, timed
by `geslappers.c`'s own clock, and that clock was `track[2]`,
`slaptime[2]`: one pair of slots for the whole machine.

- **What happened.** A host runs `bgunTickHand` for every player.
  Another unarmed player's tick advanced the host's slap clock too (twice
  GoldenEye's speed with one other player, six times with five netbots);
  a player holding anything else cancelled it, and a cancelled slap counts
  as struck (`geslappersStruck`), so the next began at once. Before (GE
  Arenas' Complex, one still unarmed client): the host's slaps 21 ticks
  apart; after, GoldenEye's 42. Both slapping: 21-63 tick gaps, each
  cutting the other's swing, before; 42 and 42, matching the client's
  own prediction to the tick, after.
- **The fix.** `[MAX_PLAYERS][2]` by `g_Vars.currentplayernum`: the slap
  (`geslappers.c`), GoldenEye's own hunting knife slash and its throwing
  knife's throw (`geguns.c` `geKnifeTrack`/`geThrowStep`, which a player
  without that weapon reset outright). Reset at every stage load.
- **The rule.** Any port-side per-hand state is per player too. Grep for
  `static .*\[2\]` beside a `handnum` in a new file; offline it is
  invisible (one player), online every client's tick runs on the host.

## F3 online: the [netplay] section (2026-10-08)

The user: "you can upgrade F3 to help with online bugs". `nettrace.c`,
called from `trace.c` when `g_NetMode` is not NONE:

- the role, protocol, tick, local slot, match and HUD state; a client's
  route to the host (`netRdvPathName`: lan/direct/punch/relay, or a
  direct connect with no lobby);
- **links** (`netSessionTraceLinks`): each peer's transport stats, rtt,
  jitter, resent share, mtu, bytes; the host's lists every client with
  its slot and state;
- **pacing**: the last 600 frames' ticks per frame (0 is a frame drawn
  between ticks) with their span, fps and ticks a second
  (`netTraceNoteFrame` from `mainNetFrame`);
- the **rules in force** (SYNC keys: a client's are the host's), this
  machine's **own settings** (NETKEY_PLAYER), pd.ini [Net] without
  `RoomSecret`;
- **every player**: seat name and ping, who simulates it, life, health,
  position, camera, weapon and switch; each hand's state, minor state,
  frames, animation, trigger, and GoldenEye's slap/slash/throw mid-swing
  (`geslappersTraceHand`, `gegunsOwnSwingTrace`); on the host a client's
  command queue, the buttons it plays now and its own settings as kept
  (`netPlayersTraceSlot`);
- then each module's running summary goes into the log "at F3"
  (`netPlayersLogNow`, `netEntsLogNow`, `netEventsLogNow`,
  `netPredictLog`, `netPuppetsLog`, `netLagCompLog`), so the log tail the
  trace ends with carries them.

A report from both ends of one moment is the best: the host's picture of
a client next to the client's own. In a harness, `call
(void)traceRequest()` from gdb writes one (`Mod.TraceReport=0` keeps the
send dialog from holding the input); traces land in `build/traces/`.

