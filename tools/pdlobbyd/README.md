# pdlobbyd - the netplay lobby

Rooms for online play: the briefing room list, the game lobby (teams, ready,
chat, the host's LAUNCH and its countdown), the join ticket the host checks,
and a UDP rendezvous so a host and a joiner behind NAT can punch through to
each other. The plan of record is `~/perfect-dark/PLANS/NETPLAY.md` (Lobby).

It is not the game server. The match runs on the host's machine (listen
server or `--dedicated`); the lobby only says who intends to play together,
and the host decides who connects by checking the **join ticket** with the
room secret it was handed at create time - it never calls back here during a
match.

Stdlib only, Python 3.7+, like pdghostd. Everything is in memory: a restart
empties the lobby, and hosts re-create their rooms (the client treats `401` as
"sign in again" and `410` as "join or make the room again").

## Why asyncio and not ThreadingHTTPServer

Almost every request is a long-poll: each member of each room sits in one for
up to 25 s, all the time. With a thread per connection the thread count is the
player count, and a cap on threads is a cap on players that a few idle sockets
can fill. Here a parked poll is a coroutine and an `asyncio.Event`. Measured:
84 full 12-player rooms with 1008 polls parked at once cost ~12 MB of RSS and
two threads (the loop, plus the four-worker pool that only runs pdghostd
logins). Every room mutation runs on the one loop thread, so the room state has
no locks. The cost is a small HTTP/1.1 reader of our own - request line,
headers, `Content-Length` body, keep-alive - which is all nginx sends upstream.
Chunked bodies get `411`; nginx buffers and sends a length.

## Accounts

The same accounts as Ghost Trials. `POST /login` forwards `{username, pin}` to
pdghostd's own `/login` over `http://127.0.0.1:8090` with the player's address
in `X-Real-IP`, so pdghostd's limiter (8 wrong PINs per address, the hot
account slowdown, known addresses) counts the player and not this process.
That is the least coupled option: pdghostd stays the only thing that opens
`ghosts.db`, its schema and PBKDF2 parameters can change without this file
knowing, and this unit needs no path into `~/pdghosts`. A good PIN becomes a
lobby **session** (128-bit random, 12 h sliding, four per account), so the PIN
is checked once and not on every room action. If pdghostd is down, sign-in
answers `502 account server unavailable`; sessions already issued keep working.

`PDLOBBYD_AUTH=open` takes any valid name without a PIN - for a LAN copy or a
test copy only, since anyone can be anyone.

## HTTP API

Behind nginx at `https://texturepacks.art/pdlobby/` (the proxy strips the
prefix). Every reply is JSON, `{"ok": true, ...}` or
`{"ok": false, "error": "<sentence to show the player>", ...}` with a status
code. Two credentials:

- **session**: `Authorization: Bearer <32 hex>` from `/login`; needed to create
  or join.
- **member token**: `X-PD-Member: <32 hex>` from create/join; needed for
  everything under `/rooms/<id>/` except `join`. Per room, 128-bit random.
  It is only ever sent over HTTPS - never in a UDP datagram (REGISTER is
  signed with the separate `udp_key` instead).
  Any well-formed token that is not a live seat in that room answers `410`
  with `"reason": "gone"`; for two minutes the `error` says why
  (`"the host removed you from the room"`, `"you timed out of the room"`,
  `"the host stopped responding"`, `"the host closed the room"`,
  `"you joined another room"`), after that (or after a lobby restart)
  `"you are no longer in this room"`. `403` is only ever a refusal of the
  action (`"reason": "host_only"` for host-only actions, `kicked`,
  `password`), never a dead token.

Request bodies are JSON objects, at most 8 KiB (`413` past it, unread). An
empty body is `{}`.

| Method + path | Who | Body / query | Reply |
|---|---|---|---|
| `GET /ping` | anyone | | `api`, `udp_port`, `auth`, `time` |
| `POST /login` | anyone | `username`, `pin` | `session`, `user`, `expires` (unix) |
| `POST /logout` | session | | |
| `GET /rooms?proto=&build=` | anyone | `proto` filters to that protocol; `build` adds `same_build` to each | `rooms: [summary]`, newest first |
| `POST /rooms` | session | see **Create** | `room`, `token` (host's member token), `secret` (64 hex), `udp_id` (16 hex), `udp_key` (64 hex), `version`, `time` |
| `POST /rooms/<id>/join` | session | `proto`, `build`, `content`, `password`?, `spectator`? | `room`, `token`, `udp_id`, `udp_key`, `version`, `time`; plus `ticket`, `ticket_expires` when the room is counting down or launched (a re-join of your own seat) |
| `GET /rooms/<id>/state?since=N&wait=S` | member | | see **State** |
| `POST /rooms/<id>/ready` | member (not host) | `ready` (default true) | `version` |
| `POST /rooms/<id>/team` | member | `team` 0-7, `spectator` | `version` |
| `POST /rooms/<id>/chat` | member | `text` | `version` |
| `POST /rooms/<id>/settings` | host | any create field but compat/dedicated | `version`; unreadies everyone, cancels a countdown |
| `POST /rooms/<id>/kick` | host | `user` | `version`; the user cannot rejoin this room |
| `POST /rooms/<id>/launch` | host | `force`?, `cancel`? | `version`, `countdown` (s) |
| `POST /rooms/<id>/reopen` | host | | back to `open` after a match, nobody ready |
| `POST /rooms/<id>/leave` | member | | host leaving closes the room |
| `POST /rooms/<id>/heartbeat` | host | `endpoints`? | `version`, `time`; every 5 s; 15 s without one closes the room |
| `POST /rooms/<id>/ticket` | member | | a fresh `ticket`, `ticket_expires`, `time`; `409` (`reason: not_started`) while the room is open |

**Create** (`POST /rooms`): `name` (required, 1-32), `password` (0-16
printable, "" = none), `max_humans` 2-12 (default 4), `stage`, `scenario`
(<= 32), `sims` 0-32, `region` (<= 16, self-reported, shown in the list),
`rules` (object, <= 24 keys `[a-z0-9_]{1,24}`, values bool / 32-bit int /
string <= 32: the summary the lobby shows, never applied by the lobby),
`endpoints` (<= 4 `"a.b.c.d:port"` / `"[v6]:port"` the host listens on: LAN,
public, UPnP; a malformed one is a `400`, and a well-formed one whose address
is neither private-range nor the address the request came from is quietly
dropped, so a host cannot point its joiners at a third party), `dedicated` (the host is a server, not a player: it takes no
human seat), and the compatibility triple `proto` (1-65535), `build`
(`[A-Za-z0-9._+-]{1,40}`, the VERSION_HASH), `content` (8-64 hex, the session
content hash). Making a room leaves any room the account was in.

**Join** refuses, with the reason in `reason` and a readable `error`:
`proto` (`"this room runs netplay protocol 3 and your game has 2 - one of you
needs to update"`), `content` (`"this room's game content differs from yours
(ROM, mods or added content) - load the same set as the host"`), `password`,
`kicked`, `full`, `started` (countdown or launched; join-in-progress is phase
7). `build` is not compared - it is listed (`same_build`) so the client can
warn. Joining a room you are already in returns the same token. Joining
another room leaves the first.

**Room summary** (list and `state.room`): `id` (8 hex), `name`, `host`,
`stage`, `scenario`, `humans` (non-spectators, host included unless
dedicated), `max_humans`, `spectators`, `max_spectators` (4), `sims`,
`locked`, `proto`, `build`, `content`, `created` (unix), `state`
(`open`/`countdown`/`launched`), `dedicated`, `region`, `host_rtt_ms`.

**Ping hint**: `host_rtt_ms` is the lobby's own UDP round trip to the host's
registered socket (PROBE every 10 s, smoothed), null until the host registers
and answers. A client estimates its ping to a room as its own ECHO round trip
to the lobby plus `host_rtt_ms` - an upper-ish bound that never puts a host's
address in a public list. Addresses are only ever given to members: the
roster says `udp: true/false`, the endpoints come in `launch` and in PEER.

**State** (`GET /rooms/<id>/state?since=N&wait=S`): if the room's `version`
is greater than `N` it answers at once; otherwise it parks up to `S` seconds
(default and cap 25). One parked poll per member: a new one answers the old
one at once. At most 24 parked per address (`429` past that) and 1200 in all
(past that it answers at once), which leaves connections free for heartbeats,
joins and creates when polls are at the cap. A live member's polls are
charged to a bucket of its own (10/s, burst 30), not to its address, so a
room's burst of changes waking everyone - or a LAN party behind one address -
does not spend the address's request bucket. A member with a poll parked is
never reaped. Reply:

```json
{"ok": true, "version": 17, "time": 1791148005,
 "room": {"id": "0a1b2c3d", "...": "summary"},
 "rules": {"time_limit": 10},
 "members": [{"user": "dab", "team": 0, "ready": true, "spectator": false,
              "host": true, "udp": true}],
 "chat": [{"v": 15, "user": "dab", "text": "gl hf", "t": 1791148000}],
 "countdown": {"remaining": 3.2, "forced": false},
 "launch": {"at": 1791148010, "endpoints": ["192.168.1.5:27100"],
            "public": "203.0.113.9:51234"},
 "you": {"user": "joiner", "host": false, "spectator": false,
         "ticket": "...", "ticket_expires": 1791148310}}
```

`chat` holds only lines posted after version `N` (the last 50 are kept), so a
client passes back the `version` it last saw. `countdown` is null unless
counting down; `launch` null unless launched (`public` is where the host's UDP
socket was seen by the rendezvous, null if it never registered; a change of
it, or of any member's `udp`, moves the version and wakes the polls).
`you.ticket` is a fresh ticket on every non-host reply during countdown and
after launch. `time` is the lobby's unix clock: the host keeps the offset to
its own and checks ticket expiry in lobby time.

**Launch**: host only. Refused (`409`, with `waiting: [names]`) unless every
non-spectator, non-host member is ready, or `force: true`. Starts a 5 s
countdown; an unready, a leave or a kick cancels it unless forced; `cancel:
true` cancels it; settings cancel it. When it runs out the room is `launched`.

**Reaping**: a host silent for 15 s (no heartbeat) closes the room; a member
with no request for 30 s and no poll parked is removed - except while the
room is `launched`: then only the host's heartbeat counts, nobody is reaped,
and a member whose game crashed can re-join its seat for a new ticket (the
roster is what the host admits against). `reopen` gives every member a fresh
30 s. A client should still keep its state poll running through the match:
that is how it hears the room close.

## Join ticket

ASCII, five fields joined by `|`:

```
<user>|<room>|<expiry>|<nonce>|<mac>
joiner|0a1b2c3d|2000000000|abababababababababababababababab|4985ec2a...8a01
```

| Field | Format |
|---|---|
| user | the account name, `[A-Za-z0-9_.-]{3,15}` (cannot contain `\|`) |
| room | 8 lowercase hex, the room id |
| expiry | unix seconds on the lobby's clock, decimal, exactly 10 digits `[1-9][0-9]{9}` (issued + 30 s) |
| nonce | 32 lowercase hex, 128 random bits, unique per ticket |
| mac | 64 lowercase hex = HMAC-SHA256(key, msg) |

- **key**: the room secret's 32 raw bytes (`secret` from create is them as 64
  hex; hex-decode it).
- **msg**: the ASCII bytes of the first four fields exactly as sent, with
  their three `|`: `joiner|0a1b2c3d|2000000000|abab...ab`.
- Exactly 10 expiry digits, so a ticket is at most 133 bytes
  (`TICKET_MAX`) and the expiry always fits a u64.

Tickets are only issued from the countdown on: in the `join` reply when
re-joining a counting-down or launched room, on every state reply then, and
from `/ticket` (refused while the room is open). Each lives 30 s - the connect
window - and the lobby cannot take one back, so **a valid mac is not a seat**.
The host accepts a CONNECT's ticket only if all of these hold:

1. It matches the format above exactly (five fields, each its pattern, no
   trailing bytes).
2. The recomputed mac equals the given one, compared in constant time.
3. `room` is the host's own room id.
4. `expiry > lobby_now`, where `lobby_now` is the host's clock plus the offset
   it keeps from the `time` field of its own state replies (a host's clock can
   be minutes out).
5. `user` (compared case-insensitively) is in the room's **current roster**
   from the host's own state poll, as a non-host member. The host keeps that
   poll running through the match; a kick or a leave moves the version, so the
   host hears of it within a round trip.
6. The nonce is not in its accepted set (kept until each nonce's expiry). This
   stops only a byte-identical replay: a member gets a fresh nonce on every
   state reply.

Then **one seat per user**: a good CONNECT for a user who already holds a seat
replaces that seat (a restarted game) instead of taking a second one. And
when a user leaves the roster (kicked, left, room closed), the host drops that
user's connected peer. The player's name is the ticket's `user`, not anything
else CONNECT says. `verify_ticket()` in `pdlobbyd.py` is the reference for 1-6
(`roster=` for 5); `TicketTests.test_known_vector` pins one vector (key =
bytes `00 01 .. 1f`, the message above) at
`4985ec2af1530d6054140a70be302817cb3d097494cdad12dbec75ed3e9f8a01`.

## UDP rendezvous

UDP port 27101 (`PDLOBBYD_UDP_PORT`) on every address. Both the host and
the joiners send from **the same socket ENet will use**, so the NAT mapping
the lobby sees is the one a peer must punch to.

Every datagram starts with six bytes:

| Offset | Size | Value |
|---|---|---|
| 0 | 4 | magic `50 44 4c 42` ("PDLB") |
| 4 | 1 | version, `01` |
| 5 | 1 | type |

Integers are big-endian. An **endpoint** is `family u8` (`04` or `06`), the
address (4 or 16 bytes), `port u16`.

| Type | Dir | Body after the header |
|---|---|---|
| `01` REGISTER | C->L | `room u32`, `member id` 8 bytes (`udp_id` hex-decoded), `seq u32`, `count u8` 0-4, `count` endpoints: the sender's own LAN addresses, `mac` 16 bytes |
| `02` REGISTERED | L->C | `room u32`, endpoint: the sender as the lobby saw it |
| `03` PEER | L->C | `room u32`, `cookie` 8 bytes, `role u8` (0 host, 1 player, 2 spectator), `namelen u8`, name (ASCII, <= 32), `count u8` 1-5, `count` endpoints: the peer's public address as the lobby saw it first, then its LAN ones |
| `04` ERROR | L->C | `code u8` (1 = unknown room or member id, 2 = bad mac or stale seq), `room u32` |
| `05` ECHO | C->L | `nonce` 8 bytes, then padding to at least 40 bytes in all |
| `06` ECHO_REPLY | L->C | `nonce` 8, endpoint: the sender as the lobby saw it |
| `07` PROBE | L->C | `room u32`, `nonce` 8 (to a registered host) |
| `08` PROBE_REPLY | C->L | `room u32`, `nonce` 8, echoed back by the host |
| `10`-`1f` | | reserved for the relay |

**REGISTER signing**: `mac` is the first 16 bytes of HMAC-SHA256(key = the
32 raw bytes of `udp_key` from create/join, message = every byte of the
datagram before the mac, header included). `seq` is 1 for the first REGISTER
under a key and rises by at least one with every REGISTER sent; the lobby
accepts only a `seq` above the last it accepted, so a captured datagram cannot
be replayed from another address. The datagram ends exactly after the mac
(23 + 7..19 per endpoint + 16 bytes; 39 with no endpoints). A create, a join,
and a re-join of the same seat each hand out a new `udp_key` and start `seq`
again (code 2 means: fetch a new key by re-joining). Nothing in a REGISTER is
usable over HTTP: the member token never travels over UDP.

**LAN endpoints**: the lobby keeps only private-range addresses -
`10/8`, `172.16/12`, `192.168/16`, `169.254/16`, `fc00::/7` - and drops the rest
(public, loopback, multicast, CGNAT, v6 link-local which needs a scope id).
PEER therefore lists the peer's observed public address and private-range
candidates, nothing else. The game applies the same filter to what it punches,
so no member can aim another's game at a third party.

Flow:

1. A member (host or joiner) sends REGISTER once a second until it has both
   REGISTERED and, for a joiner, PEER. The host re-registers every 10-15 s
   for as long as the room lives, which keeps its NAT mapping and its
   registration (60 s) fresh.
2. When a joiner and the host are both registered, the lobby sends each a
   PEER describing the other. Both carry the same `cookie` (random per room
   and joiner) for their punch packets to carry, so each side can tell the
   other's probes from noise. A re-register re-sends the pair, at most once a
   second per pair. Spectators pair with the host too; joiners never pair
   with each other (the host is the only server).
3. Each side sends punch packets from its ENet socket to every endpoint in the
   PEER it got (LAN first is fine: same-LAN pairs connect there), then ENet
   connects on whichever answered, and CONNECT carries the ticket.
4. The host answers PROBE with PROBE_REPLY from the same socket; that is the
   list's `host_rtt_ms`. A client sends ECHO to time its own path to the
   lobby (and to learn its public address, STUN-like).

Anything that does not parse is dropped silently. Nothing the lobby sends in
answer to an unauthenticated datagram is larger than that datagram (ERROR is
11 bytes against a REGISTER of at least 39, ECHO_REPLY at most 33 against a
40-byte ECHO), so the port cannot amplify; PEER and PROBE only go to addresses
a signed REGISTER registered. A datagram without the magic and version is
dropped before it touches the limiter; the rest get 10 a second per source
address, burst 20. The per-source table holds at most 20000 addresses (about
4 MB): sources past that share one overflow bucket (200/s) until entries age
out after a minute idle, so forged source addresses cannot grow memory.

**The relay** (types `10`-`1f`) is a later phase. Its place is
`Lobby.relay_datagram()`: a RELAY_ALLOC authenticated like REGISTER gets a
session id, and datagrams under that id are forwarded only between the two
registered public endpoints of that pair, under a per-room byte budget - never
to an address the datagram itself names. Until then those types are dropped.

## Limits

| What | Limit |
|---|---|
| Body | 8 KiB (`413`); headers 16 KiB (`431`) |
| Requests per address | 12/s, burst 60 (`429`); a member's state polls 10/s, burst 30 per member instead |
| Sign-ins per address | 10 / 5 min, plus pdghostd's own |
| Rooms | 200 in all (`503`), 4 live per address, 6 created / 10 min per address |
| Joins per address | 30 / min; wrong passwords 5 / 5 min per address per room |
| Members | 2-12 humans + 4 spectators per room; one room per account |
| Chat | 120 chars, 6 lines / 10 s per member, last 50 kept |
| Parked polls | one per member, 24 per address, 1200 in all; connections 1500 (unit `LimitNOFILE=4096`) |
| Tickets | 30 s, only from the countdown on |
| UDP sources tracked | 20000, then one shared overflow bucket |
| Sessions | 5000, four per account, 12 h sliding |

Names, room names, stage/scenario/region strings, rules strings and chat are
cut to printable ASCII (anything else becomes a space), whitespace folded,
then cut to length: they are drawn in the game's font on other players'
screens. Passwords and the HMAC use `hmac.compare_digest`. Tokens, secrets
and nonces come from `os.urandom`.

## Testing it

```sh
python3 tools/pdlobbyd/test_pdlobbyd.py
```

60 cases in about thirteen seconds, each against its own server (an event
loop on a background thread, HTTP and UDP on ports the kernel picks, timeouts
cut to fractions of a second): create/list/filters, validation, join with
password, the protocol and content refusals, the wrong-password limiter, full
rooms and spectators, ready/team/chat with sanitising and the chat limiter,
host-only settings (refused settings leave the room untouched) and kick, the
launch countdown (refused while unready, cancelled by an unready, forced, run
out into launched with filtered endpoints and a ticket, reopened), leaving,
one room per account, a host leaving for another room, dedicated hosts,
long-poll wakeup on a change (and on a UDP registration), timeout, room close,
the parked-poll cap, one poll per member, polls per address, polls charged per
member, host and member reaping (none during a match; a parked poll keeping a
member), dead tokens as 410, the ticket (known vector, 10-digit expiry and
133-byte cap, expiry, tampering every field, wrong key and room, roster,
malformed, replay, a kicked player off the roster, none before the
countdown), the rendezvous over real UDP sockets (signed REGISTER ->
REGISTERED -> PEER both ways, matching cookies, a sniffed REGISTER replayed or
re-sequenced refused, the UDP key no HTTP credential, a re-join's new key,
public LAN claims dropped, ERROR, junk dropped, ECHO, PROBE -> `host_rtt_ms`),
the UDP rate limit and its bounded table, the HTTP rate limits, the body and
header caps, chunked bodies, keep-alive, and sign-in through a stub pdghostd
(forwarded `X-Real-IP`, refusals passed through, the account server down,
logout). Nothing outside 127.0.0.1.

## Running it (not deployed yet)

```sh
scp tools/pdlobbyd/pdlobbyd.py sdg@10.8.0.1:~/pdlobbyd.py
scp tools/pdlobbyd/pdlobbyd.service sdg@10.8.0.1:/tmp/
ssh sdg@10.8.0.1 'sudo cp /tmp/pdlobbyd.service /etc/systemd/system/ &&
    sudo systemctl daemon-reload && sudo systemctl enable --now pdlobbyd'
```

Then the nginx location beside pdghostd's (same server block), and
`sudo nginx -t && sudo systemctl reload nginx`:

```nginx
# http {} level, beside the server block: lets idle upstream connections be
# reused instead of opened per request. `Connection ""` below only matters
# with this block.
upstream pdlobbyd {
    server 127.0.0.1:8091;
    keepalive 32;
}

location /pdlobby/ {
    proxy_pass http://pdlobbyd/;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header Connection "";
    # Long-polls park for up to 25 s; leave nginx room above that.
    proxy_read_timeout 40s;
    proxy_buffering off;
    client_max_body_size 16k;
}
```

**Check nginx's connection budget before deploying.** Each parked poll holds
two nginx connections (client side and upstream), and that budget is shared
with pdghostd and everything else on the box. Debian's default is
`worker_connections 768` with one worker on one vCPU - about 380 waiting
players before nginx refuses everyone, pdghostd included. Look with
`grep -n 'worker_connections\|worker_processes\|worker_rlimit_nofile' /etc/nginx/nginx.conf`
and raise `worker_connections` (e.g. 4096, with `worker_rlimit_nofile 8192`)
if the lobby is to hold more.

And UDP 27101 open inbound - in the host firewall (`sudo ufw allow
27101/udp` if ufw is in use) **and in the VPS provider's firewall, which the
plan records as unverified**. Check from outside with an ECHO:

```sh
python3 -c 'import socket;s=socket.socket(2,2);s.settimeout(3);s.sendto(b"PDLB\x01\x05"+b"n"*8+b"\0"*26,("texturepacks.art",27101));print(s.recvfrom(64))'
```

Like pdghostd it takes the client address from `X-Real-IP`, then the **last**
`X-Forwarded-For` element, so it must never be reachable over HTTP except
through the proxy - which `PDLOBBYD_HOST=127.0.0.1` ensures. The unit runs with
`ProtectHome=tmpfs` and no data directory: if the script lives anywhere but
`/home/sdg/pdlobbyd.py`, change `BindReadOnlyPaths=` or the unit fails to
start. `MemoryMax=128M` against a measured ~15 MB full lobby (plus at most
~4 MB of UDP limiter table). `LimitNOFILE=4096` keeps the 1500-connection
cap clear of the systemd default 1024 descriptors.

Configuration is environment (in the unit) or flags: `PDLOBBYD_HOST`/`--host`,
`PDLOBBYD_PORT`/`--port` (8091), `PDLOBBYD_UDP_HOST`/`--udp-host`,
`PDLOBBYD_UDP_PORT`/`--udp-port` (27101), `PDLOBBYD_AUTH`/`--auth`
(`ghost`|`open`), `PDLOBBYD_GHOST_URL`/`--ghost-url`. Every other limit is an
attribute of `Config` at the top of the file.

Logs go to the journal: one line per room made, closed (with why) and
launched. `journalctl -u pdlobbyd -f`.
