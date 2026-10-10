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
  Also there: **a death online (protocol 22, 2026-10-08)** - the open
  seats that kept a room played alone on black for ever, the dead watching
  the others with Mission Respawn off (`netspec.c`'s cameras), the last
  death's GoldenEye replay before the end (`gedeathcam.c`).
- **Content follows the host** — the section of that name (`netcontent.c`,
  protocols 13 and 14): a client plays the host's overlay mod, Stage Loader
  maps, conversions and ROM hack mode from its own copies (the content
  block in ACCEPT and RULES, the PD mod entered at the STAGE_LOAD's stage
  change and served when missing, protocol 25, on-demand
  map mounts, LOADED's "mod" component, the lobby's `mod` and `ge` fields),
  and a conversion or map mod it has not got is **served by the host** into
  one of fs.c's memory directories (`$N/<name>`): only the host needs the
  ROM. Also there: **co-op on the conversions' missions** (the mission
  set in RULES, stage key kind 3) and **campaign rooms** (the host plays
  its set's menus; each mission it starts is the room's next match; the
  black screen on GoldenEye's openings, late joins, the host's menus and
  what a client drew wrong, its doors among them: 2026-10-07), and
  **GoldenEye's characters** a guest lacks, mounted or fetched before the
  stage (2026-10-08: a Goldfinger 64 guest's puppets were Dr. Caroll,
  unarmed).
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
- **Host migration (protocol 21, 2026-10-08)** — the section of that
  name (`netmigrate.c`, pdlobbyd README "Host migration",
  `netmigratetest.sh`): a lobby room outlives its host; how the lobby
  picks the next one (`can_host`, `nat`, `hostlost`, `relaunch`,
  `decline`), what a client keeps of the match, how the new host adopts
  the room's rules and carries a Combat Simulator match on (clock, kill
  table, each player back where it stood: RESUME) or starts a mission
  again, content a member was served hosted and served on; the boot's
  `mpInit` that had wiped every scripted room's arena and sims.
- **Weak spots found and fixed (2026-10-08, session 16)** — the section
  of that name: an audit of the whole netcode for what a crowd, a long
  session, a hostile peer or an odd moment breaks; what a client's death
  and a name from the wire did on every machine, why the host's pause is
  not one, and the rest of that batch.
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
| codec | `netbuf.c`, `netdelta.c` | bounded little-endian reader/writer with a sticky error; EQOA-style XOR against the last acked baseline (protocol 23: each record against the newest the client acked of it) plus zero-run RLE, a 64-entry baseline ring per peer |
| tick | `net.c` | an integer 60 Hz clock: `mainNetFrame` runs the whole `mainTick`s due, one pad sample per tick, the mouse summed per frame |
| session | `netsession.c`, `netrules.c`, `nethash.c`, `netticket.c` | CONNECT/ACCEPT/REFUSE (protocol, build, region, converter, named content-hash components, lobby ticket), RULES and STAGE_LOAD by stage key, LOADED stage hashes, the GO barrier, MATCH_END, seats (a room's size or `Net.MaxPlayers`), join in progress, reconnect holds |
| remote players | `netplayers.c` | a client's commands drive a virtual pad on the host (`osContGetReadData` hook), per-slot settings in a side table |
| snapshots | `netents.c`, `netsnap.c` | prop generations, per-client visibility and priority, fixed quantized records, a full-precision local-player block, acks/NACKs |
| puppets | `netpuppets.c` | the client poses the host's world: chrs interpolated ~2 snapshots behind, objects, doors, lifts, projectiles from descriptors |
| events | `netevents.c` | reliable tick-stamped shots, explosions, hits, deaths, hudmsgs, sounds, applied after their tick's puppets |
| prediction | `netpredict.c` | a 256-tick ring of commands and movement state; a mismatch reloads the host's state and replays the later commands |
| lag compensation | `netlagcomp.c` | a 96-tick pose ring per chr on the host (Net.LagCompMaxMs's 1000 ms with the interpolation on top); a remote shot is tested against the chrs as its shooter saw them, every chr on its screen rebuildable |
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
RULES), the refusal codes, and the protocol history (netproto.h's list is the full one). Protocol 23 is current (the 2026-10-08 weak-spot batch: SNAP's per-entity bases and deferred bitmap, Traps "Snapshots in a crowded match"; the local-player block's tank and a tank's turret and barrel in its OBJ record, Online co-op); 22 was (a co-op death online: the mission block's player bit 8 names the death that lost the mission, bit 16 a death that comes back; co-op's respawn is Mission Respawn's); 21 was (host migration: GO's stagetime for every client, RESUME); 14 was (co-op on the conversions' missions: the mission block's set tag and stage key kind 3; content served by the host: CONTENT_REQ/BEGIN/FILE/END/NO; 13 was content follows the host: the content block in ACCEPT and RULES, CONNECT's "mod" and "added" logged rather than refused, LOADED's "mod" component, LEAVE NOMOD; 12 was online co-op: the mission block in RULES, the SETUPCHR descriptor, the scenario block as a mission block; 11 let a command's START reach the host, played only for a dead player: the respawn).
The lobby's HTTP API and the rendezvous/relay datagrams are in
`tools/pdlobbyd/README.md`. A change to a message's shape or meaning bumps
`NET_PROTOCOL_VERSION`; pdlobbyd lists a room's protocol and the Briefing
Room marks rooms of another one with `!`.

## Gates

Run them **one at a time** (they share the GPU and loopback ports) with
`SDL_VIDEODRIVER=offscreen`. Each writes its logs under
`build/<name>-out/` and screenshots under `build/net-shots/`.

`tools/ci/netallgates.sh [LOG]` runs them all in turn (~2 h) and reruns a
gate that fails once: passing the second time it is FLAKY (first run kept as
`build/gate-NAME.try1.log`), failing twice it is FAIL; the log ends with
`== summary`. `RERUN=0` turns the rerun off. It exits 0 only when nothing
failed (a FLAKY gate passed), the replay, pdlobbyd and pd-nettest steps
counted too; before 2026-10-10 its status was its last line's, 1 on a clean
run. Before a deploy the set that
matters is each changed area's own case, `replaytest.sh compare` against the
deployed build, `pd-nettest` and `netlobbywinetest.sh` (~25 min); the full
run follows the deploy in the background.

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
| `netnattest.sh` | 1 min | punch, relay, direct, LAN, mute rendezvous in user-namespace NATs; a punch through both games frozen after PEER (s) |
| `netpredicttest.sh` | 6.5 min | prediction at 0/150 ms, loss, wine client, sims, a time limit |
| `netlagcomptest.sh` | 9 min | hits at 150 ms with and without lag compensation (on and loss: 8400 frames, at least 30 shots), loss, soak |
| `netscenariotest.sh` | 21 min | every scenario, two lossy |
| `netcontenttest.sh` | 9 min | GoldenEye arenas (YOLT: a client respawned by its START alone), Goldfinger's mode (gfvariant: the conversion mounted is enough, no zip needed), a client with nothing installed served the conversion by the host (fetch) and in a Goldfinger room the hack and GoldenEye's characters (gffetch), a Perfect Dark match on Complex with Goldfinger 64's guns chosen (Mod.CsHackGuns, weapons.md: gfcs, the client mounting the hack for its guns; gfcsfetch, fetching it), mod maps (one mounted on demand: modmount), overlay (and one the client lacks, left over with NOMOD: modmissing), bodies, props |
| `netjointest.sh` | 5.5 min | join in progress, a spectator, reconnect and its hold running out |
| `netchattest.sh` | 4.5 min | protocol 18: chat on every machine in order, the host's burst limit (5 of 7, the sender alone told), the notices of a join in progress (with "3/4"), a spectator, a drop, a return within the hold, a hold running out and a spectator gone, a client's first PLAYERS, the host's pause Control page and Players page, screenshots of the feed, panel and open line |
| `netwidetest.sh` | 2 min | phase 8: a host and eleven clients (twelve games at once, alone), every slot 1-11 walks from its own commands; a room of two refuses a third |
| `netcooptest.sh` | 19.5 min (pair 4.5, twelve 2, lobby 1.2, ge 1.5, campaign 1.5, geend 2, camproom 1.5, death 1.5, endjoin 4, nomodroom 0.7, campleave 4) | online co-op (spec-coop.md; ge: GoldenEye's Dam as a co-op mission, the client mounting the conversion on demand and taking the set from RULES): a host and one, four and eleven clients on Defection (eleven: stacked on one spot): the host's mission in RULES, every client loads and passes GO, the opening cutscene starts and ends on a client at the host's clock, the guards are posed from SETUPCHR records, prediction matches after the opening, a client's death and START respawn, the host's abort reaching every end screen and every client back in the menus; a lobby room created as a co-op mission; campaign: a GoldenEye campaign host starting Dam alone from its folder, the opening ending on its own player, a client joining in progress with no opening, on the host's spot, predicting at 95% or better; geend: Dam's own ending kicked on a campaign host with a client in, the client taking the host's outro shot and fades, its START skipping the outro on the host, both screens fading, each machine on GoldenEye's REPORT page for its own player (the client's kills the host's), no PD end screen, the client's NEXTs closing its folder; camproom: a GoldenEye campaign room on a local pdlobbyd launched by its host alone, Dam started from the folder and aborted, the room still launched, a newcomer connecting between missions and taken into Facility; death: Mission Respawn off on a GoldenEye campaign's Dam with two seats open, the client killed (no respawn offered, out to watch the host's player), then the host killed (the mission lost, GoldenEye's replay three times on the host, the end after it, both on the report, killed); endjoin: a Perfect Dark campaign room, a joiner refused STARTED past 30 tries through a held barrier (gdb) and in once it lets go, its game killed, the host's death losing Defection, its game back while the host's end screen stays up (`--net-lobby-keep-endscreen`): taken at once for the next mission and in it; nomodroom: a Perfect Dark campaign room whose host has a mod the guest lacks, the guest's Game Lobby status line giving the NOMOD reason (not "connecting") and one connect only; campleave: a PD campaign room's host leaves it for another host's launched co-op room whose end screen is up, accepted for the next match (protocol 26), its Game Lobby saying so, the host told, and in the room's next launch; the pair's respawn runs with Mission Respawn on |
| `netmigratetest.sh` | 8 min (quit 1.5, crash 1.7, lobby 1.5, coop 1.6, served 1.7) | host migration (protocol 21): a host and two joiners per case, a lobby of its own each. quit: the host quits mid-match on 0x32 (two sims, three kills given to a seat): a joiner takes the room over, the other reconnects with a new ticket and its RESUME, the match carries on (clock within 4 s of the old host's, the kill table, every living record's life back at its place); crash: the same with the host gone as a crash is (the joiners' `hostlost` move the room on); lobby: the host leaves between matches, the room stays open under a joiner who plays the next match with the other on the room's arena; coop: Defection, the mission starts again under the new host; served: a GoldenEye arena whose joiner has nothing installed, hosted after the host quits from the served copy and served on to a newcomer |
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
- netnattest a ("joiner did not connect over punch", 2026-10-08, once in
  fifteen runs; root-caused): the ten games start at once on one GPU and
  froze together for ~16 s just after the joiner's PEER. The rendezvous'
  deadlines were wall-clock time, so the joiner woke with its ladder 16 s
  old, failed it on that tick with not one punch sent ("no path ...
  answered"; no "asking the lobby for a relay" line between, which is the
  proof no tick ran), READYed, and the launch took the advertised
  endpoints; the host read its PEER on the same wake (it reported the
  joiner reaching it "20001 ms after its PEER": the joiner's 20 s retry).
  netrdv.c now runs on its own clock (`rdvNow()`, `rdvClockTick()`): real
  milliseconds, except that a gap between two ticks counts at most
  `RDV_STALL_MS` (1 s), so a frozen game cannot count its own freeze as the
  network's silence; a gap over 2 s in a room is logged ("rdv: this game
  did not tick for N ms"), and the gate prints those lines. Forced to
  happen by `stall.sh` (gdb holds the joiner 17 s on its first ladder tick,
  SIGSTOP the host 12 s - under pdlobbyd's 15 s host_timeout, past which
  the room migrates instead): the build before failed it as the gate did,
  this one passes on the punch. netnattest's games are line-buffered now
  (`stdbuf -oL -eL`; "file 1742WARNING: rdv: ..." was stderr landing in a
  4 KB stdout block, and the order of the log lines was not their order).
- netcontenttest gfvariant ("not in Goldfinger 64's mode: ... variant \"\"",
  failing since cf5e36c's build dir was left for `build/`): never the
  client. The host's `--net-test-ge-variant gf64` looked the hack up in
  gexplusrom.c's converted list, which holds a hack only while its source
  is in added-content/; `build/` has the conversion (its mods/ is the main
  tree's) but no goldfinger64.zip, so the host warned "not converted here"
  and played GoldenEye's own mode, and RULES carried no tag. The switch
  now finds the hack as a guest and co-op do (`netContentVariantName()`:
  converted here, or its folder mounted), the host's start line names the
  mode ("(GoldenEye mode, variant gf64)"), and the case checks the host's
  mode before the client's. Session 5's pass was build-share/, which had
  the zip.

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
- **A number in the host's list is not one in yours (2026-10-10).** A
  weapon set's number - Random 5, Random and Custom above all - follows
  however many sets the machine's own list holds (`WEAPONSET_CUSTOM` is
  `g_MpNumWeaponSets + 2`: a PD mod's sets, GoldenEye's fourteen appended or
  not), so RULES' set is the host's numbering: `netRulesRead` bounds it by
  `MP_MAX_WEAPONSETS + 2`, and `netRulesApply` makes one past this machine's
  list its Custom (the slots in RULES are the host's guns). Bounded by the
  guest's own Custom, a guest inside the host's PerfectBear mod (Custom 14)
  refused the host's (28) - "the host's RULES did not parse" - and so did
  any guest without GoldenEye converted against a GoldenEye set (F3
  20261009-215118). Any list a mod or a conversion grows is the same trap.
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
- **Snapshots in a crowded match (protocol 23, 2026-10-08).** Three faults
  of the 1100-byte cap, all in `netsnap.c`. (1) A changed record that did
  not fit went out present-not-updated, i.e. the client copied its base's
  (the acked snapshot's, a round trip old), and the puppets posed it at
  the new snapshot's tick: a chr stepped back about one RTT and forward
  again. Now each store keeps the host tick its record's bytes are from
  (`netStoreTick`, store bytes 4-7, never on the wire: updated or unchanged
  = the snapshot's tick, deferred = the base's), SNAP carries a "deferred"
  bitmap so the client can tell unchanged from left behind, and
  `netClientPosePuppetsRun` poses any record whose tick is not its
  snapshot's from the newest record held at or before the render tick and
  the oldest after it, from whichever snapshots carried them
  (`netPupSearch`; the puppets' log line: "chr records older than the last
  posed", "blended towards an older one", "posed from other snapshots").
  It also poses an id present in the snapshot after the render tick but
  missing from the one before (left out while new), so a made prop is not
  freed and rebuilt. (2) The rate fell to 20 Hz after ten snapshots that
  left changes behind - less bandwidth exactly when short of it; only loss
  lowers it now. (3) The ramp after a join or a WANTKEY: every snapshot was
  a delta against the one newest acked snapshot, so entities sent in the
  snapshots in flight were new again in the next ones (resent or left out
  every RTT, and gone from a later acked snapshot that had left them out).
  Each entity is now coded against its own newest acked record
  (`netSnapHostBase` = `netBaselineGetAcked`, less than 64 behind), and
  SNAP lists the present ids whose base is not the baseline (u8 seq minus
  it). Measured with `tools/ci/netcrowd.sh` (not a gate: a listen host on
  Defection co-op, two clients, `--net-sim 2,75` everywhere, so 150 ms and
  2% loss each way): every offered entity present after 30 ticks, was 86;
  entity keyframes in the first 300 ticks 507, was 1344, left out 2623, was
  4692; the client's chr poses blended towards an older record 0, was 1659
  and 2291. On Skedar with 24 sims spraying and a third client joining in
  progress (`MODE=mp`): the joiner's entity keyframes in its first 300
  ticks 117, was 425; blends towards an older record 0, was 881-2541 per
  client; made props freed and rebuilt about 25% fewer. The host's log line now ends "first at tick F, last left out
  at tick T, deferred behind a newer one sent N" (N: the deferrals the old
  puppets showed as a step back). The reliable EVENTS channel was measured
  too (24 sims spraying SMGs, two clients, 150 ms/2%, `traffic slot`
  lines' "reliable in flight peak", "queued peak"; the events client line's
  "lag ... max, over 6 ticks"): about 7 KB/s per client, at most 4 KB in
  flight and three commands queued - no backlog; a late event is the
  ordered channel waiting out one loss (max 25 ticks), not volume, so
  events are still sent to everyone reliably.
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
- **Leave after a match lands on Online Game** (F3 2026-10-09, Shockwave):
  menutick.c puts Perfect Dark's Combat Simulator up as the
  `MENUROOT_MPSETUP` root after every match, and the Game Lobby used to be
  pushed straight over it, so Leave (and Back) went into the local Combat
  Simulator. `netLobbyMenuPushRoomAfterMatch()` (from
  `netLobbyMenuAfterMatch`) builds the stack a room is entered from: in
  Perfect Dark the root is swapped for Online Game (`func0f0f3704`), whose
  Back goes to the Perfect Menu (`mpsetupmenu` GENERAL); a GoldenEye/ROM
  hack room keeps its mode's Combat Simulator under Online Game. A client
  dropped on the Perfect Menu with its seat (`netMainMenuTick`) gets Online
  Game opened as its row opens it (`func0f0f820c`) and the Game Lobby over
  it once up (`netLobbyMenuFrame`, from `netPump`). A campaign host's menus
  (`netCoopCampaignAfterMatch`) and an Advanced Setup's pages are as before.
  A room joined from Browse Rooms leaves to the Briefing Room before its
  first match and to Online Game after it.
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
  to the next address; a STARTED refusal (host loading) retries
  the same address that answered (`s_CandNext` kept), not the dead ones
  ahead of it, for as long as the room stays launched (2026-10-09: it
  had stopped after 30 tries). Only the first endpoint used to be tried.
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
- **A session that ends in the stage loop leaves `g_NetInStageLoop` up**
  (F3 20261009-045729, ElmoBear: "my sounds are randomly fast"). A client
  that leaves, is dropped or loses its host goes to NETMODE_NONE in
  `netStageStopped`, so the next stage's loop neither calls
  `netSessionInStage` nor `netStageStart` (both behind `g_NetMode`), and
  the flag stayed 1 with `g_NetTicksThisFrame` frozen at the last net
  frame's count. `schedAudioFrame` then mixed that many audio frames per
  presented frame for the rest of the run: at 2 (the report's
  "game step in 240ths: 8 x600" in the menus) twice what the output plays,
  the queue at `Audio.QueueLimit` and whole buffers dropped - sound that
  skips ahead. Random because it is whatever the leaving frame ticked
  (0 or 1 sounded normal); the pad read (`joyReadData`) was skipped too,
  and the frame step was wrong. mainLoop now clears both when the mode is
  NONE and the flag is still up.
- **A record for what this machine holds (2026-10-09).** The host's
  snapshot never carries a child (capture skips a prop with a parent), but
  the same setup object can still be a child here: a guard's shield or gun
  from the setup that the host's guard has dropped, while this machine's
  (which never drops: `objDrop` is the host's) still holds it - before that
  guard's record says so, or for good when the dropped thing is not what
  its hands hold (`netPupHeld` frees only `weapons_held`). `netPupObj` posed
  it as it stood, and a child with a sibling looks paused (`prev`/`next` are
  its sibling links): `propUnpause` put it in the active list with its
  holder's child list still running through it, and the holder's next
  `chr0f022214`/`func0f0706f8` walked on into every active prop as its
  held things - garbage models, or recursion to the stack's end. Crash
  reports 20261009-050322 and -050412 (every guest of a co-op Defection out
  of the match, one 25 ticks into a join in progress, when the dropped
  things' records came before the dead guards'). The client now lets go
  of it first as `objDrop` does (`netPupObjLetGo`: `objDetach`, listed,
  placed from the record; "let go" in the puppets line);
  `netcooptest.sh letgo` stages it (Defection's chr 10 drops its shield
  on the host and keeps its gun).

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
- **A stage's background lists past two (2026-10-09, F3 20261009-160132).**
  The 0x1000+ lists run on `g_BgChrs`, chrs with no prop, so nothing is
  nearest them: `coopToggleP1P2` kept their p1p2 where it was, player 0
  for ever, and a death handed it to `coopOtherPlayerNum`'s "any other"
  - in a room played alone a dead open seat, out of play for good. A51
  Escape's hangar check (0x100a: toggle, `if_chr_in_room(CHR_P1P2, 0xe7)`)
  then watched seat 1 after the host's Mission Respawn death, and
  "Locate secret hangar" and its cutscene never came. Now a propless chr's
  toggle steps to the next living, noticeable player in slot order
  (`coopNextLivingAfter`), and past two a death with nobody else alive
  leaves a chr on the dead player (its new life comes back to it) rather
  than an open seat (`chrsPlayerAfterDeath`). Before, a 4-player room's
  background lists also only ever looked at the host.
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
- **A death online (protocol 22, 2026-10-08).** The user: "similar to how
  we fixed the intro black screen, the death screen where bond is replayed
  3x showing his death is causing the same. also if co-op with no mission
  respawn, let the dead players only spectate with no weapons allowed, then
  if last player dies the 3x death scene then mission failure."
  - *The black screen.* A lobby room's mission has a seat per member slot;
    an open seat's player is killed to vacate it (`netSeatVacate`) and the
    host builds no view for it (`netHostPassIdle`), so its fall never
    finishes (`redbloodfinished`/`deathanimfinished` stay 0). The end of a
    co-op mission waits on `coopAllDeadDone()`, every player dead *and done
    falling*: a room played alone, or everyone dead, sat on black for ever
    and never failed (probed: host and client both dead, 24 s on, the host
    at `colourscreenfrac` 1, its two open seats `red 0 anim 0`). Now
    `coopAllDead`/`coopAllDeadDone` leave an open seat's player out
    (`netPlayerOutOfPlay`, the host's seat table; 0 offline). The replay was
    never the cause: it had been for one player alone (`PLAYERCOUNT() == 1`
    and no co-op), so it never ran online at all.
  - *Mission Respawn's rules (the user, the same day: "switch co-op respawn
    to Mission Respawn's rules").* Online, PD's co-op respawn (A, Z or
    START, half a living player's health) is gone: the host decides at each
    death whether it comes back (`netCoopDeathRespawns()`, from
    `playerDieByShooter` before the drop): the host's Mission Respawn on
    (a RULES key) and Mission Lives not spent, counted for the team
    (`s_CoopDeaths`, the mission's deaths by every player: with five, the
    fifth does not come back). One that comes back keeps its kit (nothing
    dropped), and once the fade is black the host's pass for that player
    asks Mission Respawn for the new life (`modRespawnBegin`: where the
    player fell, full health, the guns in its hands, "N lives left" - a
    hudmsg for a guest goes to the guest's machine); the mission block's
    player bit 16 (`MISP_RESPAWNDUE`) tells the guest, which holds it
    (`s_LocalRespawnDue`: the block saying the player lives may come before
    the local block's respawn) and sets its own new life up the same way
    (`modRespawnMark`, netents.c), the fade back in its own; the guest's
    lives line is the host's. The music is ended for this machine's own
    player only (the host's pass for a guest restarted the host's music).
    The mission is not lost, nor ended, while a dead player is due back
    (`netCoopAnyRespawnDue()` in `netCoopPlayerDied` and before bond's
    `mainEndStage`): everyone dying at once with lives left all get up. A
    death that does not come back drops its guns as co-op's always did (the
    team can pick them up) and its player is offered nothing
    (`coopcanrestart` 0, no Press START) and
    watches: `netSpecDeadCameraTick()` from playerTick's dead camera, on
    this machine's own player alone, once the death has gone to black (and
    not when nobody is left alive). The picture fades back in on a living
    player (the spectator seat's follow camera; Z the next, A the free
    camera, the line along the foot of the view from `mpRenderModalText`).
    The player stays dead throughout: no gun, no pickups, nobody's target.
    START and ESC open its pause (`NET_DEAD_WATCHING()` in bondmove.c: a
    dead player had no pause at all, START being the respawn) - PD's
    mission pause, never GoldenEye's watch, whose arm a dead player has not
    got; its title names a converted mission by its own name now (it said
    "dataDyne Defection" on Dam, by the index). Trap met: the host runs
    every player's pass, so the dead camera returns at once for any pass
    but this machine's own - resetting its state there put the host's
    watcher back on the floor's death camera every frame.
  - *The last death.* On the host, the death that leaves nobody in play
    alive loses the mission (`netCoopPlayerDied` from
    `playerDieByShooter`; a seat being emptied is never it); the mission
    block's player bit 8 (`MISP_LASTDEATH`) tells the clients whose it is.
    On a converted mission `deathcamEligible()` online is that death, for
    the host's pass of that player (which poses the body: every machine
    sees it fall again through the host's records, and the watchers follow
    it) and for that player's own machine (which watches it from GoldenEye's
    cameras, with the swoosh; a client makes no tank explosion of its own).
    The host's end waits for it (`geDeathCamLostHolds()`, given up a minute
    after the death), then MATCH_END takes everyone to GoldenEye's report,
    killed. A replay cut short by A, B, Z or START: the subject's press
    reaches the host in its command (a dead player's START is let through).
  - Probed (scratchpad death/run.sh, SCEN clientfirst/hostfirst/solo;
    run2.sh SCEN respawn/lives for Mission Respawn's rules; gdb `playerDie`
    per player on the host, screenshots from both): every order ends on the
    report after three replays; the watcher's line reads "Watching Player 1
    Z: next   A: free camera". Gate: netcooptest `death`, and `pair`'s
    respawn (Mission Respawn on: where it fell, nobody's health taken).
- **GoldenEye's tank driven by a client (protocol 23, 2026-10-08).** The
  tank is the walk's (`geTankDrive` in the step, `geTankTick` after it), so
  a client predicts it, but its state lives outside struct player
  (`g_Tank[p]`, the tankobj's speed and yaws) and was in neither the
  prediction ring nor the wire: every correction replayed the commands
  from the tank as it stood *now*, integrating the speed and the hull's
  turn twice, and nothing ever pulled the hull's yaw back to the host's -
  the client's tank drifted and snapped. Now `geTankNetSave/Load`
  (struct getanknet: state, penalty, entert, hullyaw, speed, turnsum,
  turretyaw) go in each ring tick, the host's for that player in the
  local-player block (bytes 688..719, NETLP_SIZE 720), a difference starts
  a replay like a moved position, and the replay starts from the host's
  tank. In a replay `geTankTick` walks the tank and nothing else (no
  sounds, no crush, no hands or shells). A tank this machine's player
  drives is not posed from the host's OBJ record (it had stood a snapshot
  behind on the frames between ticks); anyone else's takes its hull angle
  from the record's rotation and its turret and barrel from extra bytes 1
  and 2. Entering and leaving stay each machine's own press. The
  prediction log has a `K` line per compared block in a tank (host's and
  this machine's state, speed, yaws). Check (Runway co-op, the client
  put beside the tank on the host, its script climbing in and driving at
  150 ms, the host's copy of that player knocked 30 units aside three times
  mid-drive to force corrections): before, every block after the first
  knock was corrected (73.7% matched, 475 corrections, 49 snaps); after,
  4-5 corrections per knock and the host's tank to the bit after (96.8%,
  58, 2 - the knocks' own gdb pauses). Undisturbed, both agree exactly:
  the bug needs a correction while driving.
- **A setup object the mission's lists take out (protocol 24, 2026-10-09).**
  F3 20261009-070425, a client in a Villa room: "ship is on screen still" -
  the dropship of the opening hung over the villa all mission. Villa's
  intro list ends with `disable_object(OBJ_DROPSHIP)` (`aiDisableObj`:
  `propDeregisterRooms`, `propDelist`, `propDisable`), and a prop in no list
  was never captured (`netCaptureAll` walks the active and paused lists): it
  simply left the snapshots, which a client cannot tell from leaving its
  scope, so its copy - mapped from SETUPOBJ, never told, and running no
  lists of its own - kept its last pose for ever. Any init list's
  `disable_object` was the same (shown on a client from the start). Now the
  host also captures each setup object (by its setup command, its
  generation unchanged) that is in no list and has no parent, as a SETUPOBJ
  with `NETOBJ_DELISTED` (a door's `NETDOOR_DELISTED`), always in scope
  (unchanged, it costs its presence bit); the client takes its own out of
  the world as `aiDisableObj` does, and lists it again (`propActivate`,
  placed afresh) when a record comes without the bit (`enable_object`).
  Counters: the host's snap slot line "setup objects out of the world", the
  puppets line "taken out" and "put back".
- **F3 pass 35 (2026-10-10, fix/f3-1010a-netplay, no wire change).**
  *The Cinema page in a campaign room* (crash 20261009-233623): the
  folder's Cinema page starts its stage through Accept Mission, so a
  campaign host's "Dam's ending" became the room's next co-op match (four
  seats), and the cinema goes back to the folder by `gexFrontGoBack`,
  never `mainEndStage`: the Institute loaded with `mplayerisrunning` up
  skipped `mpReset`, and the `lvmpbotlevel` the host's own Combat Simulator
  sims had raised sent `lvReset` into `mpCalculateTeamIsOnlyAi` over the
  cinema's four stale player chrs (aibot NULL). Repro: a `--host
  --net-test-campaign ge` host, gdb `mpSetSimSlotOn(0,1)` and
  `'gexfront.c'::frontStartCinema(0,1)`, then `gecinemaEndingOver()`. Now
  the Cinema page's cinemas and credits are the host's own
  (`gexFrontStartingCinema`, `netCoopAcceptMission`), `gexFrontGoBack`
  online ends a match its stage still runs and takes the match's flags down
  (`netCoopFolderBack`), an online co-op mission never has `lvmpbotlevel`
  (the host's sims had made every object of the mission regenerate and
  dropped guns fade - offline co-op has the same leak: `g_MpSimSlots` is not
  what co-op's chrslots mask clears; not changed), and
  `mpCalculateTeamIsOnlyAi` passes over a chr with no aibot.
  *Players inside each other*: a camera inside another player's body (the
  stacked spawn, pass-through) showed slivers of it in first person, and a
  guest's opening swirl circled its Bond with the host's player standing
  through him (F3 20261009-230503/-230613, both machines on the card, never
  offline): `playerGetNetBodyAlphaFrac` hides another player's body while
  the camera is within its radius (whole 15 units out) and during this
  machine's GoldenEye opening; the name tag passes over it.
  *Runtime chrs on a guest* (F3 20261009-232342, "only the weapons
  dropping"): GoldenEye's clones and reinforcements take the next of the
  level's heads, so no chr on a guest wore their pair and `netPupMakeBody`
  never made them; outside a match it now builds the pair from the tables
  (one modeldef per head in a mission), a living one as a level's chr.
  *A guard's dropped gun*: `netPupHeld` freed the setup object the host had
  dropped when the guard's record came before the gun's, and the gun's
  record then found nothing; it is let go of and hidden for that record
  (`netEntsIsSetupProp`). A dropped KF7 then has the model's scale on the
  guest (bdee81af4's fix, F3 20261009-230911).
- **The Cinema page in a campaign room (protocol 27, 2026-10-10).** The
  owner: "let's pull guests in also" (pass 35 had made the Cinema page the
  host's own after its crash). A campaign host's Cinema-page opening, ending
  or the credits is the room's next co-op match (`netCoopAcceptMission`
  takes `gexFrontStartingCinema()`'s kind, `netCoopHostStart` keeps it),
  RULES' mission block naming it (cinema, loop, minutes). A guest arms the
  same cinema (`gecinemaArm`; the credits are Cuba's stage itself) and plays
  it locally against the host's mission block: the opening's gallery takes
  the host's shot (cineshot/cineseq, `gecinemaNetFollow`) instead of its own
  clock, the ending's cast and list are the host's (puppets, CameraSwitch,
  fades), the credits' orbit and roll follow (`gecreditsNetFollow`). A guest
  waits for GO in black and comes in from it (left black, the whole ending
  stayed black on both guests), reads no presses, and holds on black at its
  own end; the host's end - `gecinemaFinish` -> `gexFrontGoBack` ->
  `netCoopFolderBack`'s MATCH_END, or Cuba's own EndLevel through
  `mainEndStage` (`netCoopCinemaEnded`, `gexFrontCreditsOver`) - ends on no
  report: the host back on its folder, each guest in the room for the next
  mission. A late joiner is in step from its first block; a guest who leaves
  is an open seat; Loop All goes level to level as matches (the folder
  starts the next level as it reopens: `g_FrontCinemaChain`); a migrated
  host does not start a watched cinema again. Every screen has one Bond
  (`playerGetNetBodyAlphaFrac`: an opening's is each machine's own player, an
  ending's and the credits' the host's), no seat walks or shoots through it,
  no guest's gun comes back from the local block, no name tags. And a
  converted mission's list lines (`aiShowHudmsg`, `aiShowHudmsgTopMiddle`:
  GoldenEye's text_print_top, Cuba's dialogue) go to every player online
  (`netCoopListTextToAll`) - a guest had seen none of them, in any GoldenEye
  mission. Harness: two guests through the ending, the gallery (Loop Level,
  a third joining part way, the second leaving), the credits, Loop All
  chaining Dam to Facility, and the room's next mission after each. Trap met
  testing it: this box has game controllers attached, and a headless run
  without the gates' `SDL_GAMECONTROLLER_IGNORE_DEVICES*` env read a held A
  on pad 0 - the folder picked the Cinema page's Intro by itself.
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
- **The overlay mod on a client (protocol 25, 2026-10-09: "PD mods as a
  live mode").** The restart rule is gone: a PD mod is a runtime mode now
  (`modmode.c`; mods.md, "A mod entered live"), every part of it swapped at
  a stage boundary, ROM segments and sound banks included. The host enters
  its mod from the Perfect Menu's Perfect Dark Mods row *before* making a
  room (the row is disabled online: `modModeCanChange()`), and the overlay
  is that mod. `netContentFollow()` finds the host's mod by name and the
  "mod" hash: this machine's installed copy with the same bytes, else a copy
  a host served this process (`fsMemDirFind`), else it asks the host for
  its own (`netContentFetchOverlay` -> CONTENT_REQ by the mod's name,
  NETCONTENT_FETCH: at ACCEPT the guest keeps waiting in the Game Lobby
  while it comes, at STAGE_LOAD the message is kept in `s_FetchStage`). The
  host serves its overlay now (`netContentServeRequest` looks from dir 0,
  `netContentSessionNeeds` accepts the overlay's name) by the one list
  `netModFileAllowed()` (nethash.c) that the "mod" hash also walks: files/,
  segs/, animations/, sequences/, textures/*.bin, modconfig.txt and
  IMPORT.txt (modloader.c reads it); never the patch, readmes,
  1964_HIRES_Files/, files.incompatible/, segs.unlocated/ or the emulator
  pack's .htc (GE-X's 20.8 MB, read only with Mod.LoadTextures through
  gzopen, which cannot open "$N/"; a served copy simply has no HD pack).
  The hash reads through fs.c (`fsFileLoad`), so a memory copy hashes as
  the host's disk copy; the fetch is hashed before it is sealed
  (`netHashDirContentsUncached`) and refused if it differs. GE-X is 2833
  files, 26.6 MB, 3.4 s on loopback. The mod is entered with
  `modModeRequestAtNextStage(path, name)`: queued, applied at the
  session's next stage change - normally the match's STAGE_LOAD.
  **The order trap:** STAGE_LOAD resolved its stage key and applied the
  RULES before that stage change, i.e. over the *old* mod's tables (a kind
  2 key "stage N of mod X" needs X to be the overlay; a mod's map ids,
  arenas, character rows, weapon sets and solo stages are the mod's only
  after the swap). So `netClientBeginStage` follows first; when a swap is
  pending it keeps the message (`s_SwapStage`), loads a stand-in (the
  Institute, never actually loaded) and `modModeStageBoundary()` calls
  `netSessionModSwapped()` after the swap, which resolves the key again,
  re-runs `netRulesApply()` and sets `g_StageNum` (the outer loop has not
  read it yet). The "mod" component of LOADED is the swapped overlay's.
  The save is NOT switched for a mod followed online (modmode's per-mod
  save, `modModeSwitchSave`, would reread the eeprom and apply its options
  mid-session under the RULES netrules.c saved): the guest plays on the
  save it joined with. The player's own mod (`s_OwnPath`) comes back when
  the session ends: `netContentRestore(1)` from `netStageStopped` (a stage
  change follows: at-next-stage), `netContentRestore(0)` between stages
  (netClientEnd in the menus, leaving a room, a migration declined: a
  waiting request is withdrawn and the Perfect Menu's own
  `modModeRequestEnter/Leave` reloads the Institute). Only --moddir games
  still refuse (RESTART text), and a host that cannot serve (an overlay
  with no files, a refused CONTENT_REQ) still ends with NOMOD, named.
  Lobby: the Briefing Room's note says "Mod X: playing here / entered on
  join / served on join"; `netCoopGameName("")` is the entered mod's name,
  so Create Room's Game list reads "<Mod> Campaign". Gates:
  netcontenttest `pdmodown` and `pdmodfetch` (`--net-test-pdmod NAME`
  enters an installed mod at the first stage), netsessiontest's swap case.
  fs.c: a relative load inside a "$N" overlay (`segs/foo`,
  `modconfig.txt`, textures/) used to find the file through fsFullPath and
  then stat/fopen the "$N/..." name; every primitive now resolves memory
  on the expanded name (`fsMemResolveName`), and `fsReplaceModDir` keeps
  "$N" maps mounts across a swap (a served GoldenEye Arenas stays mounted).
  Caps widened: FS_MAXMEMDIRS 8 -> 32 (served mods last the process),
  NETHASH_DIRCACHE 8 -> 32. Left: a fetched mod's sfx bank longer than
  any installed mod's is not covered by modaudio's boot-time sound-id
  reserve (all known mods are stock-sized); a migrated host whose follow
  is still pending writes its RULES before its swap (`netContentHostNeed`
  names the pending mod, but the stage tables are the old until the
  match's stage change).
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
  (Since protocol 25 the overlay PD mod is served too: "The overlay mod on
  a client" above.)
- **How fast it is served (2026-10-09, F3 "the upload from host takes
  awhile").** ENet held a peer's reliable data in flight to
  `ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE`, 64 KB with bandwidth 0, so a serve
  ran at 64 KB a round trip: GoldenEye Arenas in 26.5 s at 120 ms (27.2 s
  with 1% loss), a Goldfinger 64 room (Goldfinger then GoldenEye Arenas for
  the characters) in 68 s. Now: the window is 1 MB (enet.h, PD patch; the
  same build at both ends, which CONNECT enforces), a client's socket takes
  `NET_MAXWAITING_CLIENT` (3 MB, `netHostSetMaxWaiting`) so a window's worth
  held behind a lost datagram is not refused (the host keeps 256 KB a peer),
  and `netContentServeTick` sends by a byte rate (every serve together,
  shared by a credit each, small files many to a pass) rather than two parts
  a pass: it starts at 2 MB/s and, every 200 ms while the serves use all
  of it, grows by a quarter (no peer's smoothed round trip over its lowest
  by 10 ms) or an eighth (under 20 ms), up to 12 MB/s; any connected
  peer's 40 ms over its lowest (the host's uplink queueing, which the
  players' snapshots share) cuts it by a quarter and holds it for that
  round trip ("serving at N KB/s" in the log); and what ENet may hold for a
  peer is its share of the rate for its lowest round trip plus 40 ms, so
  a cut shortens the link's queue too. Through a 1 MB/s bottleneck
  (`--net-sim 0,60,0,1024`) a late joiner fetched in 20.7 s with its own
  round trip 0-40 ms over its lowest in the 5 s summaries (the first cut,
  with 2 MB held and growth by a quarter, ran it 870 ms over), and the
  playing client's snapshots stayed 150 a 300 ticks, its round trip 126 ms. GoldenEye Arenas now 2.45 s at 120 ms,
  2.6 s with 1% loss, the Goldfinger 64 room 6.1 s (`build/cspeed.sh`,
  `build/cslate.sh` in the contentspeed worktree, not kept: a host and a
  modless guest with `--net-sim` on both). What limits it now: the rate
  cap and its ramp (on bare loopback 2.1 s against 1.4 s unpaced), then
  the window at long round trips (1 MB a round trip: 4 MB/s at 250 ms),
  then the 2 MB `NETCONTENT_MAXQUEUED`. Not compressed: zlib saves 11% of
  GoldenEye Arenas and 9% of Goldfinger 64 (their files are Rare-zipped
  already), not worth a protocol change. `--net-sim` took a fourth field,
  KBPS: a bottleneck with a queue on what that machine receives
  (`netHostSetSimRate`), to watch the pace back off.
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
  before, blew a stretched object up). **A matrix of nought (2026-10-09
  F3s 20261009-212235, -212401):** an object the guest never posed itself
  has a realrot of noughts - Defection's two dataDyne banners (model 0xa7,
  posed only by the host AI's aiSetObjAnim through objTickPlayer) and a
  shield a guard held (a held object's realrot is nought; model 0xf4 once
  let go, `netPupObjLetGo`). `netRotSameOnWire` took the nought matrix for
  the axes (a nought column made unit), and `netRotScales` gave nought
  columns a scale of 1, so from the next record on the object was its
  model's 1/scale times too big: the banner filled the roof's view with flat
  blue (ElmoBear's "blue walls", and the earlier "giant blue thing" on stock
  Defection, which was this, not the banner as it is), the dropped shield
  sat as a huge white thing in the office. Now a matrix with a nought column
  (`netRotDegenerate`) is never "the same on the wire", and a nought column
  takes the model's own scale. Client-only, every stage, mod or not; no
  wire change. The client's own gun stood in its
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
  F3 over the watch online (F3 20261009-185625): `traceReportTick` runs
  in lvTick with whichever player the last loop left current (a host's
  last open seat), so `geWatchIsOpen()` said no and the report fell to the
  in-play branch, which waits for `pausemode` UNPAUSED - the watch's
  PAUSED held it until the watch closed. Online it asks by number
  (`geWatchHoldsPlayer(pnum)`, `geWatchIsSettledFor(pnum)`, pnum the local
  slot): the report opens over the watch, the level runs on (no pause of
  its own), and closing it leaves the watch up (`func0f0fa6ac` skips the
  unpause only for the watch's owner; anyone else's PAUSED is untouched).
- **Dam's truck in a gateway (F3 20261009-203043, not online-only).** A
  truck held with its nose through gate 1 (by Bond or a guard, or reaching
  the gate as its 25 s auto-close fired) had the gate close onto its side
  and stop there, blocked by the truck; the truck's next step, still
  turning onto the gate road, touched the gate - ahead of its middle, so not
  "behind" - and neither moved again. gexplusveh.c `vehTruckBlocked` now
  lets a door whose line is behind the truck's **nose** go too (part 10's
  box zmax at scale); a shut gate ahead stops the nose ~58 short of its
  line, so it still holds the truck. By design otherwise: the truck waits at
  each shut gate until the player opens it from that gate's console, and
  the gates are an airlock (gate 2 opens only once gate 1 is shut).
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
- **GoldenEye's characters follow the host (2026-10-08, no protocol
  change).** In GoldenEye's mode and a ROM hack's alike the sims and
  players wear gebean.c's pool: rows from `GEBEAN_POOL_BASE`, built over
  GoldenEye's own conversion (never a hack's: `gexPlusRomMpBegin()`) at
  startup. A guest with no conversion (a Goldfinger 64 room's guest that
  fetched only Goldfinger 64), or one fetched or mounted since its pool was
  built (the fetch case's GoldenEye Arenas), lists none of those rows, and
  `netMpBodyIndex()` fell back to the host's raw list place: past this
  list's end, which `mpGetBodyId()` makes Dark Combat or, one past,
  `BODY_DRCAROLL`, whose skeleton has no hand - `chrEquipWeapon()` refused
  the puppet its gun ("held guns not made", netcontenttest's new gffetch
  case, three runs in ten). Now RULES keep each character's rows
  (`netReadMpCharRows()`) and put them in this machine's places when the
  rules are applied; at STAGE_LOAD `netContentGeCharsFollow()` builds the
  pool again over this machine's own conversion (mounted now if
  Mod.MapMods left it out) or fetches the host's GoldenEye Arenas first
  (the STAGE_LOAD is kept through a second fetch now, the map's then the
  characters'). A row still not listed stands in with a place inside this
  list, never one past it. What the XBLA release alone has - eleven heads
  the ROM has no head for - a ROM-only guest cannot list; the gate allows
  heads missing and fails on a body. `net: puppets: ... not made` names
  the body and its skeleton when a held gun still fails.
- **A download's edges (2026-10-08, the weak-spot audit).** A session that
  ended mid-fetch (the host quit or migrated, the guest left) left `s_Fetch`
  active and its unsealed memory directory in place, so the next STAGE_LOAD's
  fetch of the same folder returned 0 and the guest left "not installed
  here" for the rest of the process. `netContentSessionEnd()` (from
  `netClientEnd`, `netSessionLobbyStop`, `netSessionClose`) drops the fetch
  (`fsMemDirDestroy` frees an unsealed dir and its slot), stops every
  transfer and forgets the session's refusals; `fsMemDirFind` counts only a
  sealed dir. A fetch fails as soon as more files or bytes come than
  CONTENT_BEGIN announced, and frees its dir. The host serves only what this
  session needs - a folder a STAGE_LOAD named (`netContentHostStageDir`, in
  `netWriteStageKey`) or GoldenEye Arenas while its lists hold GoldenEye's
  characters - refused before the folder is walked, at most twice a folder
  per connection (`netContentPeerReset` at CONNECT), and paces the parts by
  what ENet still holds for the peer (`netHostPeerQueuedBytes`, 2 MB). A
  transfer stops on any disconnect, LEAVE or kick whatever the peer's state;
  a late joiner being served is not kicked at 60 s. A loaded guest waits at
  the barrier as long as the host does: the host sends its LOADED clients
  PLAYERS every 5 s while the barrier is held, which resets their 60 s
  deadline (no protocol change). A MATCH_END for the match a guest is still
  fetching for drops the kept STAGE_LOAD and sends LOBBY (it had loaded the
  finished match after the download, AWAY on the host, and left); the fetch
  runs on, and the next STAGE_LOAD for the same folder waits for it.
  netmigratetest's `midfetch` (the host quits 3 MB into serving,
  `--net-test-serve-quit`) and `fetchend` (`--net-test-serve-pace MS`, a
  client's GO line now says how long it waited after its LOADED: 80+ s
  there against its own 60) cases. fetchend plays PD_Kakariko's Playground,
  not a GoldenEye arena: a lobby host's second GoldenEye match never opened
  its end screen then (fixed since: "A room's second GoldenEye match", in
  the section on GoldenEye's end of a mission online). Not guarded: the served segments
  and modconfig.txt go to the decomp's loaders as they come.
- **The wait at the barrier said nothing (F3 20261009-222518, 2026-10-10).**
  A host whose guest was still being served an 18 MB map folder through the
  lobby's relay (128 KB/s for relayed serves) sat on the stage's first frame
  with no HUD for minutes: "frozen". `netSessionWaitLine` (drawn by
  `netHudRender` while the HUD is not live): the host names who it waits for
  and how far the folder it sends has got, a loaded guest says it waits for
  the others, a fetching guest shows `netContentFetchStatus` (which had no
  caller) over whatever it is on, menus included.
- **Not done.** (The segs/ mods' restart-and-rejoin is moot since protocol
  25.) The MUST_GE keys
  follow now (the looks by the content latch, Mod.GePlusRevisionFixes as
  SYNC since 2026-10-09: two testers set differently could join neither
  one's GoldenEye room); a client's prediction held during
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

**Which game a row is (F3 20261009-095124, online and offline alike).** The
Combat Simulator's dropdowns put GoldenEye's, Goldfinger 64's and Tomorrow
Never Dies 64's rows beside Perfect Dark's under the same kind of names
("Pistols", "Grenade Launchers"), so a row a conversion brings ends in
" [GE]", " [GF]" or " [TND]" (`gexPlusMenuTagged()`, the tag from the
conversion's folder by `gexPlusMenuTagOfDir()`). The rule is "only where the
list mixes": the weapon set dropdown tags GoldenEye's block of the whole list
(`gexPlusWeaponSetTag()`: PD mode, or GE Plus with Mod.GePlusPdGuns) and not
GE Plus's own list, which is the block alone; the per-slot weapon dropdowns
and the Random weapons list always hold Perfect Dark's guns, so their
GoldenEye rows are always tagged (`gegunsMenuTagAtOption()`: the hack's tag
when the menus name the hack's guns, none under a mod with its own weapon
list); scenarios are never mixed (GE Plus lists GoldenEye's five alone under
its "GoldenEye" group), nor are the arenas (grouped by game), and stay as
they were. Text only, in the menu handlers: `mpGetWeaponLabel()` and
`mpGetWeaponSetName()` stay bare for the HUD and end screens. The room
summary's `weapons` field (pdlobbyd's list) carries the set's tag too unless
the room is GE Plus's own sets.

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

**A room's second GoldenEye match (2026-10-09).** In a lobby room in
GoldenEye's mode (any arena, two players or three), the host's second match
never opened its end screen: `g_MainIsEndscreen` 1, `g_MenuData.prevmenuroot`
-6, no dialog, the level running on for good. Perfect Dark's arenas were
fine. After the first match menutick.c's return pushed the Game Lobby
(`netMenuAfterMatch` -> `netLobbyMenuAfterMatch`), then, since
`netMenuAfterMatch` said 0 for a match that was not a mission, took GE Plus's
own way back from a match: `gexFrontOpenAfterMatch()` opened the folder on
Multiplayer Options and pushed the Perfect Menu over the room. The lobby's
launch then started the match with the folder still open, and an open
folder takes every `menuTick()` (its own tick instead), so the -6 that opens
the end screen was never acted on. Found with gdb: `g_Front.active` 1 in the
stuck match, the breakpoint on `gexFrontOpen` reached from menuTick's
match return. Now `netMenuAfterMatch` says 1 in a room (the room's menus
are the way back), and a folder still open when the room's match or
mission starts is put away (`gexFrontCloseForNetMatch`, from
`netSessionLobbyStartMatch` and a client's `netClientBeginStage`; it logs
"the folder put away for the room's match"). A guest in GoldenEye's own mode
had the same folder over its Game Lobby. netmigratetest's `gesecond` case
(`GESECOND_PLAYERS=2`, `GESECOND_PD=1` for the two other shapes); the guard
alone was tried with the first fix taken out, and passed.

**The folder over a stage loaded under it (2026-10-09, F3 20261009-045526).**
A migrated Goldfinger 64 campaign host's folder came out in the level's
walls. The folder's model (`frontLoadModel()`: `modeldefLoad(..., NULL)`)
and its pictures (`frontTexture()` through `texSelect(..., NULL)`) load
their textures into the **stage's shared texture pool**, and the model
instance is in the stage's model pool; offline every way out of the folder
unloads it before a stage changes, but a campaign host's folder stays
loaded (`g_Front.loaded`, and even `active`) while the room's stage reloads
under it. Its lists then named the new stage's textures by the old
addresses; the renderer kept drawing its old uploads until something
emptied its cache (texpack reload, the XBLA switch) and then showed the
level's walls. Drawn straight after the reload on a build of 1e1949559 it
crashed in `frontSetSwitch()` (stale instance). `gexFrontStageReset()`,
called from `lvReset()` online, drops the instances and texture pointers
without freeing them, frees the folder's own buffers, and the model loads
again before the folder is next drawn or opened (log: "a stage loaded under
the folder"). Repro: `--host P --net-test-campaign gf64`, then from gdb
`mainChangeToStage(0x26)`, `netCoopCampaignMenusOpen()` +
`players[0]->menuisactive = 1`, `texpackReload()`, `screenshotRequest()`.

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

- **Shut out of a launched room (2026-10-09).** The user: "players cannot
  join once the level is started, but have to wait until next level"; a
  guest whose game crashed in a Perfect Dark campaign room's Defection
  (Mission Respawn off, the other guests gone) came back with its ticket and
  was refused STARTED thirty times, 2 s apart, then never tried again. The
  host's CONNECT refused STARTED while its match loaded, held its barrier,
  had changed stage, or **had ended** (`s_HostEnded`, MATCH_END gone): the
  last lasts as long as the host leaves its end screen up - here the lost
  mission's, the host alone. Now a CONNECT after the end is taken as
  between matches (`nextmatch` in `netHostOnConnect`: JOINED, not in
  progress, the stage's own content keys not asked, an older connection of
  the same account replaced; logged "the host's match is over: in for the
  next"), and the host's H12 leaves it JOINED for the next STAGE_LOAD (a
  campaign's next mission, a room's next launch). The loading and barrier
  refusals stay (they end by themselves). And a lobby member gives up on no
  room that is still launched: STARTED retries for as long as it stays
  launched and the player in it (2 s for fifteen tries, then 4 s, then 8 s:
  `lobbyRetrySecs`), and a launch whose every address went unanswered is
  tried again from the top of the list (5, 10, then 20 s apart); the status
  line counts the tries and goes when the player is in. A campaign's room
  keeps one launch through every mission, so the 30-try cap had shut such a
  player out until it left the room and joined again. Gate: netcooptest
  `endjoin`.

- **Waiting on the host's end screen, said (protocol 26, 2026-10-09, F3
  20261009-191306).** "Cannot join dick's match, relay should be able
  to": dab, fresh out of a Tomorrow Never Dies 64 campaign room he had
  hosted, joined dick's launched Defection co-op room, punched through,
  was accepted into slot 1 and sat at client state 3 (JOINED) under
  "Launched: connecting to the host..." with no STAGE_LOAD. Accepted
  without "of the match in progress" means the host had no match running:
  dick's mission was over and his end screen up (`nextmatch`, above) - the
  path and the earlier campaign room had nothing to do with it (netcooptest
  `campleave` plays that very sequence; with the host mid-mission the same
  client joins in progress). The player is in for the next match, which
  comes when the host leaves its end screen (the room reopens; READY;
  LAUNCH) - but nothing said so on either machine. Now ACCEPT's flags
  carry `NETACC_NEXTMATCH` (protocol 26) when the host took the player
  after its match ended; the client logs "the host's match is over: in for
  its next" and its Game Lobby says "In: the host's match is over; you
  join its next." (`netSessionClientWaiting` / `netLobbyJoinedWait`; a
  member in with the host and no match yet, e.g. through a launch's wait
  for the others, reads "In: waiting for the host to start the match."),
  and the host's own player gets a notice "NAME is in for the next match"
  on its HUD only (`netHudFeed`, not sent to the clients: the joiner
  knows). The feed had never been drawn over an end screen: a full-screen
  menu (PD's mission end screen, the Combat Simulator's Game Over) takes
  `lvRender`'s `var8009dfc0` path, which draws only the menus, so
  `netHudRender` is called there too (behind `g_NetMode`); a client on its
  own end screen draws its feed there the same way (joins, leaves, chat)
  and gets no "in for the next match" line. Gate: netcooptest `campleave`
  (fails on 021ce263b: no flag, the connecting line, no notice; and on
  the first fix alone: the feed not drawn over the end screen, the notice
  sent to the joiner), its gdb probe breaking in `netHudRenderFeed` with
  `var8009dfc0` set and taking the host's screenshot.

- **A refusal said in the room (2026-10-09, F3 20261009-070735).** A guest
  without the host's mod (PerfectBear) joined a launched Perfect Dark
  campaign room, left at once [nomod], and its Game Lobby said "Launched:
  connecting to the host..." for good: `lobbyClientTick` had put the
  reason in `s_MainMessage`, but the status line shows that only for an
  open room. Now every end other than the two retried ones (STARTED, every
  address unanswered) marks the launch ended here (`s_LaunchEndedAt`:
  nomod, build, protocol, content, must, ticket, full, the player's own
  leaving - "You left the game. Leave the room and join it again to play
  on."), keeps the reason apart (`s_LaunchEndedText`: the room page's
  MENUOP_OPEN clears `s_MainMessage`, and the main menu's notice dialog
  could take `g_NetNoticePending` first, so the text is read from
  `netSessionNoticeText()` whatever the flag), and `netLobbyLaunchEnded()`
  puts it in the status line, wrapped (`textWrapN`, 270 wide: one line cut
  it off mid-sentence), in place of "connecting" until the room launches
  anew, the player is in, or joins it again. Nothing reconnects to that launch (it never did:
  `s_LaunchHandled` stays the launch; only STARTED and unreached retry).
  Gate: netcooptest `nomodroom` (a host with an empty `--moddir`, a guest
  with none; gdb reads `textRoomStatus` and shoots the room).
  Serving the overlay mod itself was looked at and not built: 88 of the 94
  non-conversion dirs in the main tree's mods/ are imported console patches
  with `segs/` (the data segment at least), which load only at a start, so
  a served copy would have to be written to the guest's disk and the game
  restarted - a new design against "the share is without distributing". A
  files-only (native format) overlay could be served like a map dir
  (CONTENT_REQ at ACCEPT/RULES time rather than STAGE_LOAD, a
  `modListSwap` to a `$N/` path), but `fsFileLoadTo` (the overlay's and a
  stage mod's `textures/`) and the `stat` dir tests do not read memory
  dirs yet.

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

- **The room's setup pages and Room Rules (2026-10-09, no wire change).**
  The user (F3 20261009-071718, -071851): "need combat sim settings
  online, missing many options", "need dab's mod menu in online settings
  especially for host". Create Room and Room Settings now carry every Game
  Setup page: the scenario's Options (with More Options), Player
  Handicaps, Teams, Load and Save Settings beside the five there were, and
  a **Room Rules** page (optionsmenu.c `g_NetRoomRulesMenuDialog`): the
  SYNC keys above plus Simulant AI, each row the Dab's Mod menu's own
  handler. It edits the host's own values (pd.ini at the page's close), as
  the Dab's Mod menu does; RULES takes them at the match start, so a client
  plays them and H12 gives it its own back. Mission Respawn/Lives only in a
  co-op room, GoldenEye's three only in a GoldenEye room, Simulant AI only
  in a match. Everything the setup pages write was on the wire already
  (`g_MpSetup.options`, the scenario's save bits, each slot's team and
  handicap); what was missing: a client never got its own King of the
  Hill hill time back at H12 (`mphilltime` in `netrulessaved` now). Teams:
  the stock page (the sims, Teams Enabled, Auto Team); the host's own row
  is its team in the room (`netLobbySetHostTeam`, sent when Room Settings
  closes, or once the room exists from Create Room), the others pick
  theirs with Change Team (the launch puts each member's room team on its
  slot). Handicaps: a page of the room's players by account
  (`netLobbyHandicapOf`), the host's own its profile's, put on each
  client's slot in `netHostMatchStarting` after `netRulesSaveHost`, so
  H12 gives the host's slots back; a host that takes the room over starts
  everyone at 100%. Room Settings un-readies the room for a change on any
  of the pages (`settingsExtra()`: handicaps, hill time,
  `netRulesSyncHash()`); the host's team, like Change Team, does not.
  Left out: the Player pages (each player's own), Soundtrack and Team
  Names (the Stuff menu, not the setup). Shown with a harness in the
  netcooptest style: pdlobbyd, the host with `--net-lobby-wait 2` so
  it never launches by itself, gdb calls the pages' handlers between
  ticks, then `netLobbyLaunch(1)` and `lobbyScriptStep(2, ...)`.

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
- **The same shape in GoldenEye's gadgets and hits (2026-10-08,
  fix/net-1008-ge).** The camera's photograph was one flag
  (`g_Gadgets.photo`) consumed in whichever view pass came first - the
  host's - so a client's photograph (Silo's satellite, Bunker's screen) was
  judged against the host's screen; it is per player now and judged only in
  the presser's own pass (`gegadgetsAfterProps`). The detonator's press and
  the watch laser's muzzle are per player too. GoldenEye's hit grunt
  (`chrGeHitGrunt`, chraction.c) followed whoever's pass dealt the hit: the
  host grunted for a client's hit and a client's shot muted the host's own;
  now only a local victim grunts (through `netWorldSoundBegin`), its spacing
  per player online, and a client grunts for itself from NETEV_CHRDAMAGE
  (bit 4, the shield took it, picks the armour's spacing). A guard's or a
  sim's GoldenEye rocket launch is played by chrTick at the rocket's making,
  which a client never runs, and the fireslot is silent for that gun: a
  client plays it from the FIRESLOT event (a remote player's, from where it
  stands, from PLAYERSHOT). Swept and clean besides: gehitpuff, geimpact,
  gesfx (its hit weapon is a begin/end bracket), gewater (lvTick, once a
  tick); `g_Gadgets.keyprop` stays one (there is one key).

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


## Host migration (protocol 21, 2026-10-08)

The user: "lets add host-migration, so lobby doesnt end if host leaves";
asked, they chose for a Combat Simulator match to **carry on** under the
new host and for a co-op mission (a campaign's too) to **start again**;
then "they should still have the download from original host, even if
they dont own the mods/conversions should work fine". `netmigrate.c` is
the game's side, pdlobbyd's "Host migration" (its README) the lobby's;
`tools/ci/netmigratetest.sh` the gate.

**The lobby picks the next host.** A host's leave, its heartbeat 15 s
stale, or half the playing members reporting it lost (`hostlost`, from
`netClientHostGone`) with its heartbeat 8 s stale (`hostlost_grace`: a
live host beats every 5 s, so reports alone never move a room off one)
hand the room to `pick_host`: a non-spectator whose game said
`can_host` (join and netinfo) on the room's build; no NAT first (netinfo
`nat`, the rendezvous saw its socket at its own address), then a
punched/direct path over a relayed one, the ping, the longest in the
room. Nobody able: the room closes as before (members that never say
`can_host` keep the old behaviour; the 71 older lobby tests pass
unchanged). `promote` renews the room secret (the old host could go on
minting tickets) and the new host's UDP key (a REGISTER still on its way
from its member socket must not be taken for its host socket's; both reach
the game in its state reply's `you`), clears every pair's punch cookie (so
each member's ladder restarts toward the new host: a member keeping the
old cookie kept punching the old address for 20 s), closes the relays,
cancels a countdown, unreadies everyone, and marks a launched room
`migrating`: its state carries no `launch` until the new host says
`relaunch`, so no member connects to an address that is not listening yet.
A host that cannot host after all says `decline` and the room moves on
(it stays a member). `reopen` clears `migrating` (a room whose match had
ended is opened, not relaunched).

**What a client keeps.** `netMigrateKeep`, from two places: H12 of every
lobby-room match (the setup: RULES via `netRulesKeep`, the stage as it
resolved here, the seats' accounts from the last ROSTER), and
`netClientHostGone` when the host goes mid-match (a LEAVE SHUTDOWN/LEFT
from it, its connection lost, or `netSessionClientNewHost` when the lobby
moved the room while this machine still played with the old one): the
match itself too - the level clock as of the host's last snapshot (the
client's ticks past the last SNAP's host tick are taken off: it runs on a
moment after the host is gone, a crash's ~5 s ENet timeout, and its
clock holds once it is a second ahead, so wall time over-corrected), the
kill table, the scenario block it applied last (`netScenKeep`) and its
own last local-player block (`netEntsClientLastLp`, the host's word on
its player: place, rooms, facing, health, shield, guns, ammo, clips). A
co-op mission keeps only that it is to start again. Kept for 3 minutes.

**The new host takes over** (`lobbyTakeOver`, netlobby.c): once its
session with the old host has closed and the stage is down, it opens a
listen session (`netSessionLobbyHost` on `Net.Port`, a new socket: the
member socket has two peers and is dial-only), adopts the kept setup
(`netMigrateAdopt`: the kept content followed as a client follows a host's
- a mod switched live, the GoldenEye mode's variant - then
`netRulesAdopt`, the RULES over its own setup with seat 0 and its own old
seat exchanged; its own setup is saved in `s_Own` and comes back when it
stops hosting, and a pd.ini write meanwhile writes its own values, H13),
enters the rendezvous as host under the new key, heartbeats with its
endpoints, and once registered (or 4 s) relaunches a migrating room - or
reopens one with nothing to carry on. The other members follow the
relaunch as any launch: ladder, ticket, CONNECT. The members' message
line and Game Lobby status say who hosts now
(`netLobbyLaunchState` 4 while migrating).

**The match carried on** (`netMigrateHostStart`, from
`netSessionLobbyStartMatch`): `mpStartMatch`'s tail without its rerolls
(random arena, random weapons, quick-team sims) on the kept stage. Seats:
an account connecting before it gets its old seat (0 and the new host's
swapped: `netMigrateSeatOf`; `netHostFreeSlot` skips seats kept for other
accounts), and H1 holds the seats of accounts not back yet
(`NETSEAT_HELD`, ticketed, `Net.ReconnectHold`, vacated at the start - the
seats tick now vacates a held seat too), so the old host or a slow member
gets seat and score back through the ordinary join in progress. GO's
`stagetime60` (protocol 21: every client takes it) starts the clock where
it was; at the first tick the kill table (rows and columns 0 and the swap
exchanged) and the scenario's per-seat counts (`netScenResume`: Hacker
Central's downloads, Pop a Cap's caps and survivals, a briefcase's held
time; points are the table's) go in, a SCORES event sends the table to
everyone, and a notice says who left and who hosts. Each player with a
record alive in it (RESUME, below; the host's own from its kept block)
is given a new life (`dostartnewlife`, as the Randomizer moves a living
player) that player.c's hooks turn into its old one, the way the
Randomizer's run lands a player: `netMigrateTakeSpawn` (its place, rooms,
facing; the ground found under it), `netMigrateRestoreInventory` (guns,
ammo capped at capacity), `netMigrateSpawnHands` (the gun it held, dual
if it was; the only place a hand may be filled from) and
`netMigrateRestoreHealth` (health, shield, loaded clips, after
`playerSpawn` zeroes the shield). A client's own machine sees its player
move by the local-player block's teleport counter (a jump past
`NETENT_TELEPORT`) and its guns by `netLpInventory`. Pickups, a
briefcase, the hill and the terminal start over; sims are made afresh
with their rows of the table.

**RESUME** (client -> host, after ACCEPT, protocol 21): the kept block
with the old match's id; the host keeps it for the seat until the
player's next life (a held seat's player comes back through
`netHostLateGo`'s respawn and lands on it the same way). A record that
came before the start is kept through it (the last connect starts the
match, the RESUME follows its ACCEPT). A dead player's record is
accepted and gives nothing (a fresh life); one that does not hold
together (non-finite, a position past 10^6, a health past 100, a weapon
past the table) is logged and the seat starts afresh. The record is the
client's word: a modified client could claim a full kit. Accepted, the
lobby's players being friends; clamping it against the new host's own
view of that player (its puppet's place and held gun) would be the
answer if it matters.

**Content a member was served.** A conversion or map mod the old host
served into memory (`$N/<name>`, fs.c, alive for the process) is the
member's to host from: `netContentCanHost` (the room's mod switchable
live, its conversion mounted here own or served, a `map:` stage key's map
found) counts it, and `netContentServeRequest` now serves `$N/` folders
too (it had excluded them), so the new host's own joiners are served what
it was served. The served case of the gate: a guest with nothing
installed hosts Complex from its copy and serves 2905 files (12.7 MB) on
to a newcomer in about a second.

**Traps met.**

- The lobby test scripts made their room 3 s into the boot, before the
  boot's own `mpInit` (filemgr.c/pdmain.c: Skedar, no sims) ran as the
  menus came up: every scripted room (netlobbytest's too) played 0x32
  with no simulants, whatever it was made with. The host script now waits
  for the boot's menus first. (`--net-test-givekills` also takes a player
  as its victim when there are no sims.)
- pdlobbyd limits sign-ins to 10 per address per 5 minutes: a gate of
  several cases from 127.0.0.1 runs a lobby per case.
- With sims really in the match, idle test players die: a record dead at
  the host's going is a fresh life, and the gate counts lives given back
  against records alive.

**Not done.** A client's predicted state between the last snapshot and
the host's going is lost (the block is the host's last word); the swirl
at the resumed match's start plays as at any start; a campaign mission
restarted on GoldenEye's 007 difficulty keeps the room's difficulty, not
the old host's sliders; the gate has no GoldenEye campaign case.

**Hardened (2026-10-08, branch fix/net-1008-lobby; pdlobbyd api 3, same
shape).** An audit found a healthy host could be moved and a member could
steer it. The host's heartbeat had run from the action thread after every
queued job, so one slow request made it late; it now has a thread of its
own (`lobbyBeatThread`, 4 s timeout). `hostlost` carries only with more
than half the players and at least two, the host silent 12 s on both its
heartbeat and its rendezvous socket (REGISTER, PROBE_REPLY); a room of two
waits for the 15 s timeout (netmigratetest `crash` is a room of three, so
it still moves on the reports). `pick_host` ranks on the lobby's own PROBE
round trip to each member (members' games answer PROBE now; older ones
rank after every timed member, on their own ping) and treats a member on
one of the lobby's relays as relayed whatever it reports; `nat` "open"
counts only while registered. Relays: only forwarded traffic keeps one,
both ends must bind within 10 s, 2 per joiner address, open rooms hold at
most 16 of the 32 (the game climbs its ladder on entering the room and now
again at once when the room starts counting down, `netRdvRetrySoon`, so a
member refused in the open room gets one for the match). Per-address limits
key IPv6 on the /64; 64 sessions per address; 6 creates / 10 min per
account as well as per address (one room per account was already so).
netticket: one user holds at most 4 of the host's 64 live nonces, so a
member reconnecting twice a second no longer turns every other join away
as "too many joins at once" (the self test covers it). No protocol change.

## Weak spots found and fixed (2026-10-08, session 16)

The user: "we have polished it up pretty well, look for weak spots", then
"fix all of them, gates at the end". Five read-only reviews (the wire, the
content and the lobby, sync and limits, the session's edges, per-player
state in the game's code); this section has the host-side fixes made in
place, and the others (content, snapshots, GoldenEye co-op, the lobby) are
in their own sections' notes.

- **Checked, not a bug: the host's pause.** The audit read
  `playerTickPauseMenu`'s `lvSetPaused(true)` (player.c) and F3's
  (`tracereport.c`) as freezing a co-op room for everyone. Every way into
  them (`playerPause` from bondmove.c, bondeyespy.c, player.c's eyespy;
  tracereport.c's solo root) is behind `!g_Vars.mplayerisrunning`, and
  `mpReset` sets `mplayerisrunning` for co-op too (only
  `normmplayerisrunning` is off), so online START, a dead watcher's START
  and F3 all open the multiplayer pause (`mpPushPauseDialog`, the 2P
  mission pause in co-op), which never stops the level. Probed on a
  Defection co-op host: its pause up from tick 1000, `lvIsPaused()` 0 and
  the level's frame 1225 then 1357 two seconds later.
- **The host heard every client's death.** The host builds each player's
  view; `playerRenderHud`'s death branch started the death music
  (`musicStartMpDeath`, `musicStartSoloDeath` in co-op, which also stops
  the level's tracks) for whoever's view it was. Only a local slot's now
  (`netIsLocalSlot`: offline every player, splitscreen as before).
- **Text from the wire is drawn by a font that trusts it.** The font takes
  0x21-0x7e from a 94-entry table by `c - 0x21` and a high byte as half of
  a two-byte (Japanese) character it steps over whole. A name with DEL
  (0x7f) read `chars[94]` and its unrelocated pixel pointer on the host
  and, through ROSTER, on every client; a control byte from a modified host
  read the kerning; a lone high byte last stepped over the string's end.
  CONNECT's name had only bytes under 0x20 replaced. `netTextPrintable()`
  (netsession.c) now cleans every wire string the game draws: names (in
  `netNameSet`, so RULES' humans and sims too, and CONNECT's, ROSTER's),
  the match and team names in RULES, a refusal's text and component, the
  not-installed text from a stage key, a HUDMSG. Chat and the host's title
  already went through `netChatClean`.
- **EVENTS read past its packet.** An event's length over 0x7fff skipped
  0x7fff bytes and gave the event's reader the whole length; now such a
  length is malformed. PICKUPSFX's sound number went to `sndStart`, which
  indexes `g_AudioRussMappings` by a config number up to 0x7fff (0x400
  entries): `netEvSoundOk` refuses one past the tables.
- **Connections that never say CONNECT.** ENet takes up to 4095
  connections from one address, and the host waited 10 s for each one's
  CONNECT: one machine could hold every seat's connection open and keep
  everyone out. A client sends CONNECT the moment ENet connects, so the
  wait is 5 s (`NET_PRECONNECT_MS`), and an address may hold 3 such
  connections at once (`NET_PRECONNECT_PERADDR`), not counting loopback,
  LAN addresses and the lobby's (relayed peers all come from the relay:
  `netRdvAddrShared`). The fourth is refused STARTED, which a lobby room's
  joiner tries again on, so a household joining at one launch is only
  delayed.
- **Lag compensation's limits.** `Net.LagCompMaxMs` allows 1000 ms, but the
  pose ring held 64 ticks, and a rewind of 60 + the interpolation found no
  pose and tested the chr where it is now; the ring is 96. A remote pass's
  autoaim rebuilt at most 32 chrs on its screen (a fixed table), the
  farther ones of a crowd tested unrewound; the table is now one per chr
  slot, allocated with the history.
- **A kicked seat was held.** `netHostKick` held the seat 30 s as for a
  drop; only a load too slow (`NETREFUSE_TIMEOUT`) is held now: a player
  taken off the roster or refused for its stage never comes back on it.
- **A client on the end screen sat the next match out.** A client still on
  its end screen when the host's next match starts is AWAY and left out
  (its STAGE_LOAD would be lost); its LOBBY then only made it JOINED, so it
  waited in the menus through the whole match with its seat open.
  `netHostAwayJoins` seats it in the running match as a join in progress.
- **A campaign outlived its room.** `netCoopCampaignEnd` ran only on the
  player's own Leave: a room that closed under its host, or a host that
  stepped down, kept the campaign on, and the next room it hosted took
  every mission as the campaign's and never reopened. `lobbyStopSession`
  and `lobbyStepDown` end it.
- **A guest's mod after Leave.** A guest that left the room between matches
  (`netSessionLobbyStop`, its session ending no other way) kept the host's
  mod until a restart; it switches back (`netContentRestore`).
- **One commit, two spellings of its build.** VERSION_HASH is `git rev-parse
  --short`, nine characters in this working copy and seven in CI's fresh
  clone, and CONNECT, the Briefing Room's build mark and pdlobbyd's
  `pick_host`/`same_build` compared it exactly: a build made here (the
  user's ~/pd-test) and the dev release of the same commit refused each
  other ("both need the same build"). Found merging into dabs-mod, where
  testers' games are CI's. `netBuildSame` (and pdlobbyd's `same_build`)
  compare the shorter of the two, never under seven characters, as
  update.c compares a release's commit.
- **Left as it is: a client's body facing (NETCMD_BODY).** The host poses a
  client's body by the facing it sends whenever it sends one; the audit
  noted a client could turn its body from its aim. The Camera Tether is
  the player's own setting (the user's "all own"), so checking it would
  only stop a client claiming a tether it can turn on, and the facing
  turns the body about its place without moving where it can be hit.
- **A client's end screen froze (2026-10-09, F3 20261009-071538).** The
  client's clock trim (`netPlayersClockPpm`) holds still past 90 ticks
  ahead of the host's last ack. After MATCH_END the client is still
  `netPlaying()` on its end screen and its tick runs on, while the host
  stops acking once it leaves its own end screen: 90 ticks later every tick
  of the client stopped, so its Game Over could not be read or left until
  the host's next match ("froze here again", pacing 9 ticks a second, the
  log's "91 ticks ahead of the host ... holding"). No hold on the end
  screen. Shown with a lobby room, the host closing its end screen and the
  joiner keeping its own (`--net-lobby-keep-endscreen`): 1021c6439 stops at
  frame 300, the fix runs to 4800.
- **GoldenEye Arenas' Train: sims under pads, and the bridge (2026-10-09,
  F3 20261009-191916 / -191845).** Offline too, not netplay. Train keeps
  GoldenEye's own solo waypoints (104, one piece), so the pad-graph builder
  never runs; the ones beside the train (pads 84, 101-104) stand at the
  carriages' pad height, 345-370 over the ballast, and a simulant's arrival
  test (`chrGoPosIsArrivingAtPos()`) refused anything over 300: sims stood
  under pad 104 or 84 on errands for whole matches. On a converted level a
  pad over 210 is now arrived at when the floor under it (`geRoomGround()`)
  is the simulant's, as GoldenEye arrives by x and z alone: idle samples
  158 -> 36 of 944 (8 sims, seed 1, 7200 frames). Left: a few seconds
  among the freight car's crates (room 7/8, x 14700). The "misplaced
  models" report is GoldenEye's own scenery: a concrete bridge with a red
  star spanning the track at x 1000-1500 and its piers (room 53), and
  crates by the line; prop placement on Train matches the cartridge (the
  gefidelity world gate), the solo mission just never goes outside.
  randomizer-run.md, "Wider spacing left pieces", has the pad-graph side.
