#!/usr/bin/env python3
"""
Lobby server for Dab's Mod netplay - rooms, the briefing room list, and the
UDP rendezvous that lets a host and a joiner find each other through NAT.

Runs behind nginx on 127.0.0.1, which terminates TLS and forwards
https://texturepacks.art/pdlobby/ here, exactly like pdghostd. Nothing in
here should be exposed directly over HTTP: it trusts the proxy for the client
address. The UDP rendezvous port is the one thing that faces the internet
itself. Nothing that crosses it is a credential worth anything over HTTP: a
REGISTER is signed with a per-member UDP key and carries no token, so a
sniffed datagram gives an onlooker neither the room nor a seat in it.

Stdlib only - no aiohttp, no ORM. The box already runs several services and
this one should not bring a dependency tree with it.

What it is not: the game server. A room here is a list of who intends to play
together and a place to chat while they get ready; the match itself runs on
the host's machine (listen server or --dedicated), which is the authority.
The lobby's only say in the match is the join ticket, an HMAC the host checks
in its UDP CONNECT with the secret this server handed it when the room was
made - so the host never has to call back here to decide who may connect.

Why asyncio rather than ThreadingHTTPServer like pdghostd: almost every
request here is a long-poll. Each member of each room sits in one for up to
POLL_MAX seconds at a time, all the time, so with a thread per connection the
thread count is the player count, and a cap on threads is a cap on players
that a handful of idle sockets can fill. On one vCPU and ~550 MB shared with
other services, a waiting poll here is a coroutine and an Event - a few KB -
and every room mutation runs on the one loop thread, so the room state needs
no locks at all. The rendezvous socket lives on the same loop. The cost is a
small HTTP/1.1 reader of our own (request line, headers, Content-Length body),
which is all nginx ever sends upstream.

Accounts are pdghostd's: POST /login here forwards the user and PIN to
pdghostd's own /login over localhost, with the player's address in X-Real-IP
so that pdghostd's limiter counts the player and not this process. That is the
least coupled option - pdghostd stays the only thing that reads ghosts.db, its
schema and its PBKDF2 parameters can change without this file knowing, and
this unit needs no access to ~/pdghosts. A successful login becomes a lobby
session token, so the PIN is checked once and not on every room action.
"""

import asyncio
import collections
import concurrent.futures
import hashlib
import hmac
import ipaddress
import json
import os
import re
import socket
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

API_VERSION = 1


def env(name, default):
    return os.environ.get(name, default)


class Config:
    """Every tunable in one place, from the environment, overridable by keyword
    (which is how the test suite runs a copy with short timeouts)."""

    def __init__(self, **over):
        self.host = env("PDLOBBYD_HOST", "127.0.0.1")
        self.port = int(env("PDLOBBYD_PORT", "8091"))
        self.udp_host = env("PDLOBBYD_UDP_HOST", "0.0.0.0")
        self.udp_port = int(env("PDLOBBYD_UDP_PORT", "27101"))
        # "ghost": user + PIN checked by pdghostd's /login. "open": any valid
        # name, no PIN - for a LAN or a test copy only, since anyone can be
        # anyone.
        self.auth = env("PDLOBBYD_AUTH", "ghost")
        self.ghost_url = env("PDLOBBYD_GHOST_URL", "http://127.0.0.1:8090")
        self.ghost_timeout = 10.0

        # HTTP. nginx buffers the body and sends Content-Length, so the body
        # cap is the only one there is to enforce; the header cap is the
        # StreamReader's limit.
        self.max_body = 8 * 1024
        self.max_header = 16 * 1024
        # Kept under the unit's LimitNOFILE=4096 with room for the listen and
        # UDP sockets and the login pool's.
        self.max_conns = 1500
        self.idle_timeout = 30.0
        self.body_timeout = 10.0

        # Long-poll: up to POLL_MAX seconds, one parked poll per member (a new
        # one answers the old), POLLS_PER_IP per address, and no more than
        # MAX_WAITERS at once - past that a poll is answered at once rather
        # than parked. MAX_WAITERS sits well under MAX_CONNS so heartbeats,
        # joins and creates always find a connection when polls are full.
        self.poll_max = 25.0
        self.max_waiters = 1200
        self.polls_per_ip = 24

        # Rooms and members.
        self.max_rooms = 200
        self.max_rooms_per_ip = 4
        self.max_spectators = 4
        self.min_humans = 2
        self.max_humans = 12
        self.chat_keep = 50
        self.countdown = 5.0
        self.host_timeout = 15.0
        self.member_timeout = 30.0
        self.gone_keep = 120.0
        self.reap_interval = 1.0

        # Sessions and tickets.
        self.session_ttl = 12 * 3600.0
        self.max_sessions = 5000
        self.sessions_per_user = 4
        # A ticket covers the connect window, not the lobby wait: it is only
        # issued once the countdown starts, and a fresh one comes with every
        # state reply after that.
        self.ticket_ttl = 30
        self.max_logins_inflight = 8

        # Per-address limits. The bucket is every request but a member's own
        # state polls, which have a bucket per member instead: every room
        # change wakes every member at once, and a LAN party behind one
        # address would otherwise spend the address's bucket on re-polls. The
        # windows are the expensive or abusable requests on top of it.
        self.req_rate = 12.0
        self.req_burst = 60.0
        self.poll_rate = 10.0
        self.poll_burst = 30.0
        self.login_window = (300.0, 10)
        self.create_window = (600.0, 6)
        self.join_window = (60.0, 30)
        self.badpass_window = (300.0, 5)
        self.chat_window = (10.0, 6)

        # UDP rendezvous.
        self.udp_rate = 10.0
        self.udp_burst = 20.0
        # Source addresses on UDP are free to forge, so the per-source table
        # has a ceiling; sources past it share one overflow bucket.
        self.udp_sources_max = 20000
        self.udp_overflow_rate = 200.0
        self.udp_overflow_burst = 400.0
        self.udp_ttl = 60.0
        self.peer_min_interval = 1.0
        self.probe_interval = 10.0

        # The relay: one UDP socket per (room, joiner) pair whose NATs would
        # not punch, forwarding the game's own datagrams between the host and
        # that joiner. Sized for a 1-vCPU VPS shared with other services:
        # every cap drops (never queues) what is over it. A 4-player match is
        # some tens of KB/s per relayed joiner; the per-room budget is that
        # several times over, the global one what the box can spare.
        # relay_ports: "lo-hi" (firewall that range for UDP), or "" for ports
        # the kernel picks (tests).
        self.relay_ports = env("PDLOBBYD_RELAY_PORTS", "27110-27141")
        self.relay_max = 32               # pairs at once, all rooms
        self.relay_per_room = 16          # pairs in one room
        self.relay_room_bps = 256 * 1024  # bytes a second through one room's relays
        self.relay_room_pps = 2000.0      # datagrams a second through one room's relays
        self.relay_global_bps = 2 * 1024 * 1024
        self.relay_global_pps = 8000.0
        self.relay_burst = 2.0            # buckets hold this many seconds of their rate
        self.relay_idle = 60.0            # s with nothing forwarded or bound: closed
        self.relay_max_datagram = 1400    # bytes; the game sends at most 1200
        self.offer_min_interval = 0.5
        # Members report their path to the host and its ping (netinfo); a
        # report moves the room's version only when it changes this much.
        self.netinfo_window = (10.0, 8)
        self.netinfo_ping_step = 10

        for k, v in over.items():
            if not hasattr(self, k):
                raise AttributeError("unknown config %s" % k)
            setattr(self, k, v)


# ------------------------------------------------------------------ text

USERNAME_RE = re.compile(r"^[A-Za-z0-9_.-]{3,15}\Z")
PIN_RE = re.compile(r"^[0-9]{4,8}\Z")
TOKEN_RE = re.compile(r"^[0-9a-f]{32}\Z")
ROOM_RE = re.compile(r"^[0-9a-f]{8}\Z")
CONTENT_RE = re.compile(r"^[0-9a-f]{8,64}\Z")
BUILD_RE = re.compile(r"^[A-Za-z0-9._+-]{1,40}\Z")
RULE_KEY_RE = re.compile(r"^[a-z0-9_]{1,24}\Z")

ROOM_NAME_MAX = 32
PASSWORD_MAX = 16
CHAT_MAX = 120
FIELD_MAX = 32
REGION_MAX = 16
RULES_MAX = 24
ENDPOINTS_MAX = 4
# The game's simulant slots (MAX_BOTS in src/include/constants.h).
SIMS_MAX = 80


def clean_text(value, limit):
    """Printable ASCII only, whitespace folded to single spaces, then cut.

    The game draws these in its own font, which has nothing past ASCII, and
    every string here ends up on somebody else's screen: a control character
    or an escape is not something a stranger's room name gets to carry. What
    is not printable becomes a space, so "a\\tb" reads "a b" rather than "ab".
    """
    if not isinstance(value, str):
        return ""
    value = "".join(c if " " <= c <= "~" else " " for c in value)
    return " ".join(value.split())[:limit]


def parse_endpoint(text):
    """ "1.2.3.4:27100" or "[::1]:27100" -> (ip string, port), or None."""
    if not isinstance(text, str) or len(text) > 64:
        return None
    if text.startswith("["):
        host, sep, port = text[1:].partition("]:")
    else:
        host, sep, port = text.rpartition(":")
    if not sep or not port.isdigit():
        return None
    try:
        ip = ipaddress.ip_address(host)
    except ValueError:
        return None
    port = int(port)
    if not 1 <= port <= 65535:
        return None
    if isinstance(ip, ipaddress.IPv6Address) and ip.ipv4_mapped:
        ip = ip.ipv4_mapped
    return str(ip), port


def format_endpoint(ip, port):
    return ("[%s]:%d" if ":" in ip else "%s:%d") % (ip, port)


def new_token():
    """128 random bits as 32 lowercase hex: sessions, members."""
    return os.urandom(16).hex()


# ------------------------------------------------------------------ tickets
#
# A join ticket says "the lobby let <user> into <room> until <expiry>". The
# host checks it in its UDP CONNECT with the room secret it was given at
# create time, so it never asks this server anything during a match.
#
#     <user>|<room>|<expiry>|<nonce>|<mac>
#
#     user    the account name, [A-Za-z0-9_.-]{3,15} (never contains '|')
#     room    8 lowercase hex, the room id
#     expiry  unix seconds on the LOBBY's clock, exactly 10 decimal digits
#     nonce   32 lowercase hex (128 random bits), unique per ticket
#     mac     64 lowercase hex: HMAC-SHA256(key = the room secret's 32 raw
#             bytes, message = the ASCII bytes "<user>|<room>|<expiry>|<nonce>")
#
# A ticket is a signature, not a seat: the lobby hands a member a fresh one on
# every state reply from the countdown on, and cannot take one back. So the
# mac alone does not admit anyone. The host also requires the ticket's user to
# be on the room's current roster (from its own state poll, which wakes on a
# kick or a leave) and holds at most one seat per user - a second good CONNECT
# for a seated user replaces that seat (a restarted game) rather than adding
# one. It drops a connected peer whose user leaves the roster. Expiry is
# compared against lobby time (the host's clock plus the offset from the
# "time" field the lobby puts in every state reply), since a host's own clock
# can be minutes out.

TICKET_MAX = 15 + 1 + 8 + 1 + 10 + 1 + 32 + 1 + 64
TICKET_RE = re.compile(r"^([A-Za-z0-9_.-]{3,15})\|([0-9a-f]{8})\|([1-9][0-9]{9})\|([0-9a-f]{32})\|([0-9a-f]{64})\Z")


def ticket_mac(secret, msg):
    return hmac.new(secret, msg.encode("ascii"), hashlib.sha256).hexdigest()


def make_ticket(secret, user, room_id, expiry, nonce=None):
    if nonce is None:
        nonce = new_token()
    msg = "%s|%s|%d|%s" % (user, room_id, int(expiry), nonce)
    return msg + "|" + ticket_mac(secret, msg)


def verify_ticket(secret, ticket, room_id, now=None, seen_nonces=None, roster=None):
    """The reference the host's C implementation follows. Returns
    (ok, user, reason).

    now is lobby time. seen_nonces, if given, is a dict nonce -> expiry the
    caller keeps (and prunes past expiry); an accepted ticket's nonce is added
    to it, which stops a byte-identical replay and nothing more. roster, if
    given, is the set of lowercase user names currently in the room bar the
    host; a ticket for anyone else is refused. One seat per user is the
    caller's to keep: this says who, not whether they already sit down."""
    if now is None:
        now = time.time()
    if not isinstance(ticket, str) or len(ticket) > TICKET_MAX:
        return False, None, "malformed ticket"
    mt = TICKET_RE.match(ticket)
    if mt is None:
        return False, None, "malformed ticket"
    user, room, expiry, nonce, mac = mt.groups()
    expect = ticket_mac(secret, ticket[:ticket.rindex("|")])
    if not hmac.compare_digest(expect, mac):
        return False, None, "ticket signature does not match"
    if room != room_id:
        return False, None, "ticket is for another room"
    if int(expiry) <= now:
        return False, None, "ticket expired"
    if roster is not None and user.lower() not in roster:
        return False, None, "not in the room"
    if seen_nonces is not None:
        if nonce in seen_nonces:
            return False, None, "ticket already used"
        seen_nonces[nonce] = int(expiry)
    return True, user, None


# ------------------------------------------------------------------ rendezvous wire format
#
# Every datagram, both ways, starts with the same six bytes:
#
#     0..3   magic  'P' 'D' 'L' 'B'  (0x50 0x44 0x4c 0x42)
#     4      version, 1
#     5      type
#
# Integers are big-endian. An endpoint is:
#
#     family u8 (4 or 6), address (4 or 16 bytes), port u16
#
# Types (C = client or host, L = lobby):
#
#     0x01 REGISTER     C->L  room u32, member id 8, seq u32, count u8 (0-4),
#                             count endpoints (the sender's own LAN addresses),
#                             mac 16
#     0x02 REGISTERED   L->C  room u32, endpoint (the sender as the lobby saw it)
#     0x03 PEER         L->C  room u32, cookie 8, role u8 (0 host, 1 player,
#                             2 spectator), namelen u8, name, count u8 (1-5),
#                             count endpoints - the first is the peer's public
#                             address as the lobby saw it, the rest its LAN ones
#     0x04 ERROR        L->C  code u8 (1 unknown room or member, 2 bad mac or
#                             stale seq), room u32
#     0x05 ECHO         C->L  nonce 8, padded with anything to >= 40 bytes total
#     0x06 ECHO_REPLY   L->C  nonce 8, endpoint (the sender as the lobby saw it)
#     0x07 PROBE        L->C  room u32, nonce 8 (to a registered host)
#     0x08 PROBE_REPLY  C->L  room u32, nonce 8 (echoed back by the host)
#     0x10-0x1f               reserved for the relay (not in this version)
#
# REGISTER is signed, not authenticated by a secret it carries: the member id
# (udp_id from create/join, 8 random bytes, not secret) names the member, and
# mac is the first 16 bytes of HMAC-SHA256(key = the 32 raw bytes of udp_key
# from create/join, message = every byte of the datagram before the mac,
# header included). seq starts at 1 for each udp_key and must rise with every
# REGISTER sent; the lobby takes only a seq above the last it accepted, so a
# captured datagram cannot be replayed from somewhere else. A create or a
# join (also a re-join of the same seat) hands out a new udp_key and starts
# seq again. The member token never travels over UDP.
#
# LAN endpoints a REGISTER names are kept only if they are private-range
# (10/8, 172.16/12, 192.168/16, 169.254/16, fc00::/7): a peer is told to
# punch at those and the observed public address, and nothing else, so a
# member cannot aim somebody else's game at a third party.
#
# Both ends of a pair get the same cookie, random per (room, member), so their
# punch packets can carry it and each can tell the other's from noise. Anything
# that does not parse is dropped without an answer, and no answer is ever
# larger than the datagram that caused it unless it goes to an endpoint a
# signed REGISTER registered.

UDP_MAGIC = b"PDLB"
UDP_VERSION = 1
UDP_REGISTER = 0x01
UDP_REGISTERED = 0x02
UDP_PEER = 0x03
UDP_ERROR = 0x04
UDP_ECHO = 0x05
UDP_ECHO_REPLY = 0x06
UDP_PROBE = 0x07
UDP_PROBE_REPLY = 0x08
UDP_RELAY_FIRST = 0x10
UDP_RELAY_LAST = 0x1f
UDP_RELAY_REQUEST = 0x10
UDP_RELAY_OFFER = 0x11
UDP_RELAY_BIND = 0x12
UDP_RELAY_BOUND = 0x13
UDP_RELAY_REQUEST_LEN = 6 + 4 + 8 + 4 + 16
UDP_RELAY_BIND_LEN = 6 + 4 + 8 + 8 + 8 + 4 + 16
UDP_ERR_RELAY = 3
UDP_ECHO_MIN = 40
UDP_ERR_UNKNOWN = 1
UDP_ERR_REFUSED = 2
NETINFO_PATHS = ("lan", "direct", "punch", "relay", "none")
UDP_REGISTER_MIN = 6 + 4 + 8 + 4 + 1 + 16
UDP_MAC_LEN = 16

LAN_NETS = tuple(ipaddress.ip_network(n) for n in (
    "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "169.254.0.0/16", "fc00::/7"))

ROLE_HOST = 0
ROLE_PLAYER = 1
ROLE_SPECTATOR = 2


def udp_header(kind):
    return UDP_MAGIC + bytes((UDP_VERSION, kind))


def encode_endpoint(ip, port):
    addr = ipaddress.ip_address(ip)
    fam = 4 if addr.version == 4 else 6
    return bytes((fam,)) + addr.packed + struct.pack(">H", port)


def decode_endpoint(data, off):
    """-> ((ip, port), new offset) or (None, off) if it does not parse."""
    if off >= len(data):
        return None, off
    fam = data[off]
    size = 4 if fam == 4 else 16 if fam == 6 else 0
    if not size or off + 1 + size + 2 > len(data):
        return None, off
    addr = ipaddress.ip_address(bytes(data[off + 1:off + 1 + size]))
    port = struct.unpack_from(">H", data, off + 1 + size)[0]
    if port == 0:
        return None, off
    return (str(addr), port), off + 1 + size + 2


def lan_address(ip):
    """True for an address a LAN endpoint may name: RFC 1918, v4 link-local
    and v6 unique-local. Public, loopback, multicast, CGNAT and the rest are
    not anybody's LAN address to hand a peer."""
    try:
        addr = ipaddress.ip_address(ip)
    except ValueError:
        return False
    return any(addr.version == n.version and addr in n for n in LAN_NETS)


def register_mac(key, signed):
    return hmac.new(key, signed, hashlib.sha256).digest()[:UDP_MAC_LEN]


def norm_addr(addr):
    """A socket address as (ip, port), with v4-mapped v6 folded to v4."""
    ip = addr[0]
    if ip.startswith("::ffff:"):
        try:
            ip = str(ipaddress.ip_address(ip).ipv4_mapped or ip)
        except ValueError:
            pass
    return ip, addr[1]


# ------------------------------------------------------------------ limiters

class Buckets:
    """A token bucket per key: rate a second, up to burst saved."""

    def __init__(self):
        self.b = {}

    def allow(self, key, rate, burst, now):
        tokens, last = self.b.get(key, (burst, now))
        tokens = min(burst, tokens + (now - last) * rate)
        if tokens < 1.0:
            self.b[key] = (tokens, now)
            return False
        self.b[key] = (tokens - 1.0, now)
        return True

    def prune(self, now, idle):
        for k in [k for k, (_t, last) in self.b.items() if now - last > idle]:
            del self.b[k]


class Windows:
    """At most N events per key in a sliding window."""

    def __init__(self):
        self.w = {}

    def allow(self, key, window, limit, now, record=True):
        q = self.w.get(key)
        if q is None:
            q = self.w[key] = collections.deque()
        while q and now - q[0] > window:
            q.popleft()
        if len(q) >= limit:
            return False
        if record:
            q.append(now)
        return True

    def hit(self, key, now):
        self.w.setdefault(key, collections.deque()).append(now)

    def prune(self, now, window):
        for k in [k for k, q in self.w.items() if not q or now - q[-1] > window]:
            del self.w[k]


# ------------------------------------------------------------------ state

class HttpError(Exception):
    def __init__(self, code, msg, **extra):
        Exception.__init__(self, msg)
        self.code = code
        self.msg = msg
        self.extra = extra


class Session:
    __slots__ = ("token", "user", "expires")

    def __init__(self, token, user, expires):
        self.token, self.user, self.expires = token, user, expires


class Member:
    __slots__ = ("user", "token", "host", "ip", "team", "ready", "spectator",
                 "joined", "seen", "poll", "udp_id", "udp_key", "udp_seq",
                 "udp_public", "udp_private", "udp_seen", "peer_sent",
                 "path", "ping")

    def __init__(self, user, token, host, ip, now):
        self.user = user
        self.token = token
        self.host = host
        self.ip = ip
        self.team = 0
        self.ready = False
        self.spectator = False
        self.joined = time.time()
        self.seen = now
        self.poll = None           # (future, ip) of the one parked poll, or None
        self.udp_id = os.urandom(8)
        self.udp_key = os.urandom(32)
        self.udp_seq = 0
        self.udp_public = None
        self.udp_private = []
        self.udp_seen = 0.0
        self.peer_sent = 0.0
        self.path = None           # what the member reported (netinfo)
        self.ping = None

    def role(self):
        if self.host:
            return ROLE_HOST
        return ROLE_SPECTATOR if self.spectator else ROLE_PLAYER


class Room:
    def __init__(self, room_id, secret, ip, now):
        self.id = room_id
        self.secret = secret
        self.ip = ip
        self.created = int(time.time())
        self.name = ""
        self.pw_salt = b""
        self.pw_hash = b""
        self.max_humans = 4
        self.stage = ""
        self.scenario = ""
        self.sims = 0
        self.rules = {}
        self.region = ""
        self.content = ""
        self.proto = 0
        self.build = ""
        self.endpoints = []
        self.dedicated = False
        self.members = {}
        self.kicked = set()
        self.chat = collections.deque()
        self.version = 1
        self.waiters = set()
        self.status = "open"
        self.countdown_handle = None
        self.countdown_end = 0.0
        self.forced = False
        self.launched_at = 0
        self.host_seen = now
        self.host_rtt = None
        self.probe_nonce = None
        self.probe_sent = 0.0
        self.cookies = {}
        self.relays = {}           # joiner's member token -> Relay
        self.relay_bytes = (0.0, 0.0)   # token bucket (tokens, last)
        self.relay_pkts = (0.0, 0.0)

    def changed(self):
        """Every mutation ends here: the version moves and every parked poll
        on this room wakes and answers."""
        self.version += 1
        for fut in self.waiters:
            if not fut.done():
                fut.set_result(None)
        self.waiters.clear()

    def host_member(self):
        for m in self.members.values():
            if m.host:
                return m
        return None

    def udp_ok_host(self, now, ttl):
        host = self.host_member()
        return host is not None and host.udp_public is not None and now - host.udp_seen <= ttl

    def humans(self):
        return sum(1 for m in self.members.values() if not m.spectator)

    def spectators(self):
        return sum(1 for m in self.members.values() if m.spectator and not m.host)

    def locked(self):
        return bool(self.pw_hash)

    def set_password(self, pw):
        if pw:
            self.pw_salt = os.urandom(16)
            self.pw_hash = hashlib.sha256(self.pw_salt + pw.encode()).digest()
        else:
            self.pw_salt = self.pw_hash = b""

    def password_ok(self, pw):
        if not self.pw_hash:
            return True
        got = hashlib.sha256(self.pw_salt + str(pw).encode()).digest()
        return hmac.compare_digest(got, self.pw_hash)


# ------------------------------------------------------------------ the lobby

class Lobby:
    def __init__(self, cfg=None):
        self.cfg = cfg or Config()
        self.rooms = {}
        self.tokens = {}         # member token -> Room
        self.user_room = {}      # lower(user) -> member token
        self.sessions = {}       # session token -> Session
        self.gone = {}           # member token -> (reason, until): why a token stopped working
        self.udp_ids = {}        # member udp_id (8 bytes) -> member token
        self.waiters = 0
        self.ip_polls = {}       # address -> parked polls from it
        self.conns = 0
        self.logins_inflight = 0
        self.req_buckets = Buckets()
        self.udp_buckets = Buckets()
        self.windows = Windows()
        self.executor = concurrent.futures.ThreadPoolExecutor(max_workers=4)
        self.http_server = None
        self.udp = None
        self.reaper = None
        self.bound_port = None
        self.bound_udp_port = None
        self.relays = {}         # relay id (8 bytes) -> Relay
        self.relay_secret = os.urandom(32)  # the return-routability proofs
        self.relay_next_port = 0
        self.relay_bytes = (0.0, 0.0)
        self.relay_pkts = (0.0, 0.0)
        self.relay_dropped = 0

    # ---------------------------------------------------------- lifecycle

    async def start(self):
        loop = asyncio.get_running_loop()
        self.http_server = await asyncio.start_server(
            self.http_client, self.cfg.host, self.cfg.port, limit=self.cfg.max_header)
        self.bound_port = self.http_server.sockets[0].getsockname()[1]
        transport, _proto = await loop.create_datagram_endpoint(
            lambda: Rendezvous(self), local_addr=(self.cfg.udp_host, self.cfg.udp_port))
        self.udp = transport
        self.bound_udp_port = transport.get_extra_info("sockname")[1]
        self.reaper = loop.create_task(self.reap_loop())

    async def stop(self):
        if self.reaper:
            self.reaper.cancel()
        for r in list(self.relays.values()):
            self.close_relay(r, "lobby stopping")
        if self.udp:
            self.udp.close()
        if self.http_server:
            self.http_server.close()
            await self.http_server.wait_closed()
        self.executor.shutdown(wait=False)

    # ---------------------------------------------------------- HTTP transport

    async def http_client(self, reader, writer):
        if self.conns >= self.cfg.max_conns:
            writer.close()
            return
        self.conns += 1
        peer = writer.get_extra_info("peername") or ("?", 0)
        try:
            while True:
                try:
                    head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"),
                                                  self.cfg.idle_timeout)
                except asyncio.LimitOverrunError:
                    await self.respond(writer, 431, {"ok": False, "error": "headers too large"}, True)
                    break
                except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError):
                    break

                req = parse_request_head(head, peer[0])
                if req is None:
                    await self.respond(writer, 400, {"ok": False, "error": "bad request"}, True)
                    break

                if "transfer-encoding" in req.headers:
                    await self.respond(writer, 411, {"ok": False, "error": "length required"}, True)
                    break
                try:
                    length = int(req.headers.get("content-length", "0"))
                except ValueError:
                    length = -1
                if length < 0:
                    await self.respond(writer, 400, {"ok": False, "error": "bad length"}, True)
                    break
                # A body over the cap is refused without reading it, and the
                # connection closed so it is not parsed as the next request.
                if length > self.cfg.max_body:
                    await self.respond(writer, 413, {"ok": False, "error": "body too large"}, True)
                    break
                if length:
                    try:
                        req.body = await asyncio.wait_for(reader.readexactly(length),
                                                          self.cfg.body_timeout)
                    except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError):
                        break

                code, payload = await self.dispatch(req)
                await self.respond(writer, code, payload, req.close)
                if req.close:
                    break
        except Exception as e:  # never let one connection take the loop down
            log("connection error from %s: %r" % (peer[0], e))
        finally:
            self.conns -= 1
            try:
                writer.close()
            except Exception:
                pass

    async def respond(self, writer, code, payload, close):
        body = json.dumps(payload, separators=(",", ":")).encode()
        head = ("HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                "Content-Length: %d\r\nCache-Control: no-store\r\nConnection: %s\r\n\r\n"
                % (code, REASONS.get(code, "Error"), len(body), "close" if close else "keep-alive"))
        try:
            writer.write(head.encode() + body)
            await writer.drain()
        except ConnectionError:
            pass

    async def dispatch(self, req):
        now = time.monotonic()
        # A live member's own state poll is charged to that member, not to
        # its address (see Config.poll_rate); anything else to the address.
        key, rate, burst = req.ip, self.cfg.req_rate, self.cfg.req_burst
        if req.method == "GET" and req.path.endswith("/state"):
            tok = req.headers.get("x-pd-member", "").strip()
            if tok in self.tokens:
                key, rate, burst = ("poll", tok), self.cfg.poll_rate, self.cfg.poll_burst
        if not self.req_buckets.allow(key, rate, burst, now):
            return 429, {"ok": False, "error": "too many requests, slow down"}
        try:
            payload = await self.route(req)
            return 200, payload
        except HttpError as e:
            out = {"ok": False, "error": e.msg}
            out.update(e.extra)
            return e.code, out

    async def route(self, req):
        parts = [p for p in req.path.split("/") if p]
        m = req.method

        if m == "GET" and parts == ["ping"]:
            return {"ok": True, "service": "pdlobbyd", "api": API_VERSION,
                    "udp_port": self.bound_udp_port, "auth": self.cfg.auth,
                    "time": int(time.time())}
        if m == "GET" and parts == ["rooms"]:
            return self.list_rooms(req)
        if m == "POST" and parts == ["login"]:
            return await self.login(req)
        if m == "POST" and parts == ["logout"]:
            s = self.session_auth(req)
            self.sessions.pop(s.token, None)
            return {"ok": True}
        if m == "POST" and parts == ["rooms"]:
            return self.create_room(req)

        if len(parts) == 3 and parts[0] == "rooms" and ROOM_RE.match(parts[1]):
            room_id, action = parts[1], parts[2]
            if m == "GET" and action == "state":
                return await self.room_state(req, room_id)
            if m == "POST" and action == "join":
                return self.join_room(req, room_id)
            if m == "POST" and action in ROOM_ACTIONS:
                room, member = self.member_auth(req, room_id)
                body = req.json()
                return getattr(self, "act_" + action)(req, room, member, body)

        raise HttpError(404, "no such endpoint")

    # ---------------------------------------------------------- auth

    async def login(self, req):
        now = time.monotonic()
        body = req.json()
        username = str(body.get("username", "")).strip()
        if not USERNAME_RE.match(username):
            raise HttpError(400, "3-15 chars, letters, digits, _ . - only")
        if not self.windows.allow(("login", req.ip), self.cfg.login_window[0],
                                  self.cfg.login_window[1], now):
            raise HttpError(429, "too many sign-ins, wait a few minutes")

        if self.cfg.auth == "ghost":
            pin = str(body.get("pin", "")).strip()
            if not PIN_RE.match(pin):
                raise HttpError(403, "wrong username or pin")
            if self.logins_inflight >= self.cfg.max_logins_inflight:
                raise HttpError(503, "busy, try again in a moment")
            self.logins_inflight += 1
            try:
                loop = asyncio.get_running_loop()
                ok, err = await loop.run_in_executor(
                    self.executor, ghost_login, self.cfg.ghost_url, username, pin,
                    req.ip, self.cfg.ghost_timeout)
            finally:
                self.logins_inflight -= 1
            if ok is None:
                raise HttpError(502, "account server unavailable")
            if not ok:
                raise HttpError(403, err or "wrong username or pin")
        elif self.cfg.auth != "open":
            raise HttpError(503, "sign-in is not configured")

        self.prune_sessions(time.time())
        if len(self.sessions) >= self.cfg.max_sessions:
            raise HttpError(503, "lobby is full, try again later")
        # A few sessions per account (a player's game, a second machine); the
        # oldest goes when one more signs in.
        mine = sorted((s for s in self.sessions.values() if s.user.lower() == username.lower()),
                      key=lambda s: s.expires)
        while len(mine) >= self.cfg.sessions_per_user:
            self.sessions.pop(mine.pop(0).token, None)
        s = Session(new_token(), username, time.time() + self.cfg.session_ttl)
        self.sessions[s.token] = s
        return {"ok": True, "session": s.token, "user": username, "expires": int(s.expires)}

    def session_auth(self, req):
        auth = req.headers.get("authorization", "")
        tok = auth[7:].strip() if auth[:7].lower() == "bearer " else ""
        s = self.sessions.get(tok) if TOKEN_RE.match(tok) else None
        if s is None or s.expires < time.time():
            raise HttpError(401, "sign in first")
        s.expires = time.time() + self.cfg.session_ttl
        return s

    def member_auth(self, req, room_id):
        tok = req.headers.get("x-pd-member", "").strip()
        if not TOKEN_RE.match(tok):
            raise HttpError(401, "member token required")
        room = self.tokens.get(tok)
        if room is None or room.id != room_id:
            # Every well-formed token that is not a live seat in this room is
            # 410, whether the gone table still remembers why (two minutes) or
            # not (later, or after a restart): the client's answer is the same,
            # join or make the room again. 403 stays for "not allowed".
            gone = self.gone.get(tok)
            raise HttpError(410, gone[0] if gone else "you are no longer in this room",
                            reason="gone")
        member = room.members[tok]
        member.seen = time.monotonic()
        return room, member

    def prune_sessions(self, wall):
        for k in [k for k, s in self.sessions.items() if s.expires < wall]:
            del self.sessions[k]

    # ---------------------------------------------------------- rooms

    def room_summary(self, room, build=None):
        host = room.host_member()
        out = {
            "id": room.id,
            "name": room.name,
            "host": host.user if host else "",
            "stage": room.stage,
            "scenario": room.scenario,
            "humans": room.humans(),
            "max_humans": room.max_humans,
            "spectators": room.spectators(),
            "max_spectators": self.cfg.max_spectators,
            "sims": room.sims,
            "locked": room.locked(),
            "proto": room.proto,
            "build": room.build,
            "content": room.content,
            "created": room.created,
            "state": room.status,
            "dedicated": room.dedicated,
            "region": room.region,
            # Ping hint: the lobby's own round trip to the host's UDP socket,
            # in ms, or null before the first probe answers. A client adds its
            # own round trip to the lobby (ECHO) for an estimate that does not
            # hand every lister the host's address.
            "host_rtt_ms": None if room.host_rtt is None else int(room.host_rtt * 1000 + 0.5),
        }
        if build is not None:
            out["same_build"] = room.build == build
        return out

    def list_rooms(self, req):
        proto = req.query.get("proto")
        build = req.query.get("build")
        rooms = sorted(self.rooms.values(), key=lambda r: -r.created)
        if proto:
            try:
                p = int(proto)
            except ValueError:
                raise HttpError(400, "bad proto")
            rooms = [r for r in rooms if r.proto == p]
        return {"ok": True, "rooms": [self.room_summary(r, build) for r in rooms]}

    def read_settings(self, room, body, creating, ip):
        """Fields create and settings share; anything absent is left as it is
        (or its default, on create). Every field is checked before any is
        applied, so a refusal leaves the room exactly as it was. ip is the
        sender's address, the one public address its endpoints may name."""
        upd = {}
        if "name" in body or creating:
            name = clean_text(body.get("name", ""), ROOM_NAME_MAX)
            if not name:
                raise HttpError(400, "room needs a name")
            upd["name"] = name
        if "password" in body:
            pw = body.get("password") or ""
            if not isinstance(pw, str) or len(pw) > PASSWORD_MAX or clean_text(pw, PASSWORD_MAX) != pw:
                raise HttpError(400, "password is up to %d printable characters" % PASSWORD_MAX)
            upd["password"] = pw
        if "max_humans" in body or creating:
            n = body.get("max_humans", 4)
            if not isinstance(n, int) or isinstance(n, bool) \
                    or not self.cfg.min_humans <= n <= self.cfg.max_humans:
                raise HttpError(400, "max humans is %d-%d" % (self.cfg.min_humans, self.cfg.max_humans))
            if n < room.humans():
                raise HttpError(409, "more players than that are already in the room")
            upd["max_humans"] = n
        for key, limit in (("stage", FIELD_MAX), ("scenario", FIELD_MAX), ("region", REGION_MAX)):
            if key in body:
                upd[key] = clean_text(body.get(key), limit)
        if "sims" in body:
            n = body.get("sims")
            if not isinstance(n, int) or isinstance(n, bool) or not 0 <= n <= SIMS_MAX:
                raise HttpError(400, "sims is 0-%d" % SIMS_MAX)
            upd["sims"] = n
        if "rules" in body:
            upd["rules"] = read_rules(body.get("rules"))
        if "endpoints" in body:
            upd["endpoints"] = read_endpoints(body.get("endpoints"), ip)

        if "password" in upd:
            room.set_password(upd.pop("password"))
        for key, value in upd.items():
            setattr(room, key, value)

    def create_room(self, req):
        s = self.session_auth(req)
        now = time.monotonic()
        body = req.json()

        if len(self.rooms) >= self.cfg.max_rooms:
            raise HttpError(503, "the lobby is full, try again later")
        if sum(1 for r in self.rooms.values() if r.ip == req.ip) >= self.cfg.max_rooms_per_ip:
            raise HttpError(429, "too many rooms from this address")
        proto, build, content = read_compat(body)

        room_id = os.urandom(4).hex()
        while room_id in self.rooms:
            room_id = os.urandom(4).hex()
        room = Room(room_id, os.urandom(32), req.ip, now)
        room.proto, room.build, room.content = proto, build, content
        self.read_settings(room, body, True, req.ip)
        room.dedicated = bool(body.get("dedicated", False))

        # Counted only once the request is a room, so a client fixing its
        # fields is not charged for each try.
        if not self.windows.allow(("create", req.ip), self.cfg.create_window[0],
                                  self.cfg.create_window[1], now):
            raise HttpError(429, "too many rooms made, wait a few minutes")

        # One room per player: making one leaves whatever room they were in.
        self.leave_any(s.user)

        host = Member(s.user, new_token(), True, req.ip, now)
        host.ready = True
        host.spectator = room.dedicated
        self.add_member(room, host)
        self.rooms[room.id] = room
        log("room %s '%s' made by %s" % (room.id, room.name, s.user))
        return {"ok": True, "room": room.id, "token": host.token, "secret": room.secret.hex(),
                "udp_id": host.udp_id.hex(), "udp_key": host.udp_key.hex(),
                "version": room.version, "time": int(time.time())}

    def join_room(self, req, room_id):
        s = self.session_auth(req)
        now = time.monotonic()
        body = req.json()
        if not self.windows.allow(("join", req.ip), self.cfg.join_window[0],
                                  self.cfg.join_window[1], now):
            raise HttpError(429, "too many joins, wait a minute")
        room = self.rooms.get(room_id)
        if room is None:
            raise HttpError(404, "that room has closed")

        proto, build, content = read_compat(body)
        # Readable reasons: these are shown to the player as they are.
        if proto != room.proto:
            raise HttpError(409, "this room runs netplay protocol %d and your game has %d - "
                            "one of you needs to update" % (room.proto, proto), reason="proto")
        if content != room.content:
            raise HttpError(409, "this room's game content differs from yours (ROM, mods or "
                            "added content) - load the same set as the host", reason="content")

        if s.user.lower() in room.kicked:
            raise HttpError(403, "the host removed you from this room", reason="kicked")

        badkey = ("badpass", req.ip, room.id)
        if room.locked():
            if not self.windows.allow(badkey, self.cfg.badpass_window[0],
                                      self.cfg.badpass_window[1], now, record=False):
                raise HttpError(429, "too many wrong passwords, wait a few minutes")
            if not room.password_ok(body.get("password", "")):
                self.windows.hit(badkey, now)
                raise HttpError(403, "wrong room password", reason="password")

        # Joining again (a restarted game, a dropped poll) keeps the same seat.
        existing = self.user_room.get(s.user.lower())
        if existing in room.members:
            m = room.members[existing]
            m.seen = now
            # A new UDP key for whatever is asking, which may be a restarted
            # game that never saw the old one or its seq.
            m.udp_key = os.urandom(32)
            m.udp_seq = 0
            return self.join_reply(room, m)

        if room.status != "open":
            raise HttpError(409, "the match is %s" % ("starting" if room.status == "countdown"
                                                     else "in progress"), reason="started")
        spectator = bool(body.get("spectator", False))
        if spectator:
            if room.spectators() >= self.cfg.max_spectators:
                raise HttpError(409, "no spectator seats left", reason="full")
        elif room.humans() >= room.max_humans:
            raise HttpError(409, "the room is full", reason="full")

        self.leave_any(s.user)
        m = Member(s.user, new_token(), False, req.ip, now)
        m.spectator = spectator
        m.team = self.quietest_team(room)
        self.add_member(room, m)
        room.changed()
        return self.join_reply(room, m)

    def add_member(self, room, m):
        room.members[m.token] = m
        self.tokens[m.token] = room
        self.user_room[m.user.lower()] = m.token
        while m.udp_id in self.udp_ids:
            m.udp_id = os.urandom(8)
        self.udp_ids[m.udp_id] = m.token

    def join_reply(self, room, m):
        out = {"ok": True, "room": room.id, "token": m.token,
               "udp_id": m.udp_id.hex(), "udp_key": m.udp_key.hex(),
               "version": room.version, "time": int(time.time())}
        if room.status in ("countdown", "launched"):
            out["ticket"], out["ticket_expires"] = self.issue_ticket(room, m)
        return out

    def issue_ticket(self, room, m):
        expiry = int(time.time()) + self.cfg.ticket_ttl
        return make_ticket(room.secret, m.user, room.id, expiry), expiry

    @staticmethod
    def quietest_team(room):
        counts = collections.Counter(m.team for m in room.members.values() if not m.spectator)
        return min(range(2), key=lambda t: counts.get(t, 0))

    def leave_any(self, user):
        tok = self.user_room.get(user.lower())
        room = self.tokens.get(tok) if tok else None
        if room is not None:
            self.remove_member(room, room.members[tok], "you joined another room")

    def forget_member(self, room, m):
        """The token and the UDP id stop resolving; the seat is gone, and so
        is any relay it had (a host's going takes every relay in the room)."""
        for r in list(room.relays.values()):
            if m.host or r.joiner_token == m.token:
                self.close_relay(r, "member gone")
        self.tokens.pop(m.token, None)
        if self.udp_ids.get(m.udp_id) == m.token:
            del self.udp_ids[m.udp_id]
        if self.user_room.get(m.user.lower()) == m.token:
            del self.user_room[m.user.lower()]

    def remove_member(self, room, m, reason):
        """Out of the room; a host leaving closes it, since the host is the
        game server and there is no room without one."""
        if m.host:
            # The others are told the host closed it; only the host's own
            # token keeps why (it may have joined another room).
            return self.close_room(room, "the host closed the room", host_reason=reason)
        room.members.pop(m.token, None)
        room.cookies.pop(m.token, None)
        self.forget_member(room, m)
        self.gone[m.token] = (reason, time.monotonic() + self.cfg.gone_keep)
        self.cancel_countdown(room, unless_forced=True)
        room.changed()

    def close_room(self, room, reason, host_reason=None):
        if self.rooms.get(room.id) is not room:
            return
        del self.rooms[room.id]
        if room.countdown_handle:
            room.countdown_handle.cancel()
        until = time.monotonic() + self.cfg.gone_keep
        for m in room.members.values():
            self.forget_member(room, m)
            why = host_reason if m.host and host_reason and host_reason != "left" else reason
            self.gone[m.token] = (why, until)
        room.status = "closed"
        room.changed()
        log("room %s closed: %s" % (room.id, reason))

    def cancel_countdown(self, room, unless_forced):
        if room.status != "countdown" or (unless_forced and room.forced):
            return
        if room.countdown_handle:
            room.countdown_handle.cancel()
            room.countdown_handle = None
        room.status = "open"

    def countdown_done(self, room):
        if self.rooms.get(room.id) is not room or room.status != "countdown":
            return
        room.countdown_handle = None
        room.status = "launched"
        room.launched_at = int(time.time())
        room.changed()
        log("room %s launched" % room.id)

    # ---------------------------------------------------------- state / long-poll

    async def room_state(self, req, room_id):
        room, m = self.member_auth(req, room_id)
        try:
            since = int(req.query.get("since", "0"))
            wait = float(req.query.get("wait", str(self.cfg.poll_max)))
        except ValueError:
            raise HttpError(400, "bad since or wait")
        wait = max(0.0, min(wait, self.cfg.poll_max))

        if room.version <= since and wait > 0:
            # One parked poll per member: a new one answers the old at once
            # (a client that gave up on it, or a second copy of the game).
            if m.poll is not None:
                self.release_poll(room, m)
            if self.waiters >= self.cfg.max_waiters:
                return self.state_of(room, m, since)
            if self.ip_polls.get(req.ip, 0) >= self.cfg.polls_per_ip:
                raise HttpError(429, "too many waiting connections from this address")
            fut = asyncio.get_running_loop().create_future()
            m.poll = (fut, req.ip)
            room.waiters.add(fut)
            self.waiters += 1
            self.ip_polls[req.ip] = self.ip_polls.get(req.ip, 0) + 1
            try:
                await asyncio.wait_for(fut, wait)
            except asyncio.TimeoutError:
                pass
            finally:
                if m.poll is not None and m.poll[0] is fut:
                    self.release_poll(room, m)
                m.seen = time.monotonic()
            # Whatever happened while it waited - kicked, room closed, reaped -
            # is answered as if the poll had just arrived.
            if self.tokens.get(m.token) is not room:
                gone = self.gone.get(m.token, ("you are no longer in this room", 0))
                raise HttpError(410, gone[0])

        return self.state_of(room, m, since)

    def release_poll(self, room, m):
        """Unpark m's poll: counts down, and its future answered if it has
        not been already."""
        fut, ip = m.poll
        m.poll = None
        self.waiters -= 1
        left = self.ip_polls.get(ip, 1) - 1
        if left > 0:
            self.ip_polls[ip] = left
        else:
            self.ip_polls.pop(ip, None)
        room.waiters.discard(fut)
        if not fut.done():
            fut.set_result(None)

    def state_of(self, room, me, since):
        members = [{"user": m.user, "team": m.team, "ready": m.ready,
                    "spectator": m.spectator, "host": m.host,
                    "udp": m.udp_public is not None,
                    "path": m.path, "ping": m.ping}
                   for m in room.members.values()]
        chat = [{"v": v, "user": u, "text": t, "t": at}
                for (v, u, t, at) in room.chat if v > since]
        out = {"ok": True, "version": room.version, "time": int(time.time()),
               "room": self.room_summary(room),
               "rules": room.rules, "members": members, "chat": chat,
               "countdown": None, "launch": None,
               "you": {"user": me.user, "host": me.host, "spectator": me.spectator}}
        if room.status == "countdown":
            out["countdown"] = {"remaining": round(max(0.0, room.countdown_end - time.monotonic()), 2),
                                "forced": room.forced}
        if room.status == "launched":
            # What the host advertised, and the address its UDP socket was
            # seen from at the rendezvous (null when it never registered).
            host = room.host_member()
            pub = host.udp_public if host else None
            out["launch"] = {"at": room.launched_at, "endpoints": room.endpoints,
                             "public": format_endpoint(*pub) if pub else None}
        if room.status in ("countdown", "launched") and not me.host:
            ticket, expiry = self.issue_ticket(room, me)
            out["you"]["ticket"] = ticket
            out["you"]["ticket_expires"] = expiry
        return out

    # ---------------------------------------------------------- room actions
    #
    # Each takes (req, room, member, body) after member_auth and returns the
    # reply. Every one that changes the room calls room.changed().

    def require_host(self, m):
        if not m.host:
            raise HttpError(403, "only the host can do that", reason="host_only")

    def act_ready(self, req, room, m, body):
        ready = bool(body.get("ready", True))
        if m.host:
            raise HttpError(400, "the host launches rather than readies")
        if room.status == "launched":
            raise HttpError(409, "the match has started")
        if ready != m.ready:
            m.ready = ready
            if not ready:
                self.cancel_countdown(room, unless_forced=True)
            room.changed()
        return {"ok": True, "version": room.version}

    def act_team(self, req, room, m, body):
        if room.status != "open":
            raise HttpError(409, "teams are fixed once the countdown starts")
        if "team" in body:
            t = body.get("team")
            if not isinstance(t, int) or isinstance(t, bool) or not 0 <= t <= 7:
                raise HttpError(400, "team is 0-7")
            m.team = t
        if "spectator" in body and not m.host:
            spec = bool(body.get("spectator"))
            if spec and not m.spectator and room.spectators() >= self.cfg.max_spectators:
                raise HttpError(409, "no spectator seats left")
            if not spec and m.spectator and room.humans() >= room.max_humans:
                raise HttpError(409, "the room is full")
            m.spectator = spec
            m.ready = False if spec else m.ready
        room.changed()
        return {"ok": True, "version": room.version}

    def act_chat(self, req, room, m, body):
        text = clean_text(body.get("text", ""), CHAT_MAX)
        if not text:
            raise HttpError(400, "nothing to say")
        if not self.windows.allow(("chat", m.token), self.cfg.chat_window[0],
                                  self.cfg.chat_window[1], time.monotonic()):
            raise HttpError(429, "slow down")
        room.changed()
        room.chat.append((room.version, m.user, text, int(time.time())))
        while len(room.chat) > self.cfg.chat_keep:
            room.chat.popleft()
        return {"ok": True, "version": room.version}

    def act_settings(self, req, room, m, body):
        self.require_host(m)
        if room.status == "launched":
            raise HttpError(409, "the match has started")
        self.read_settings(room, body, False, req.ip)
        # New settings are new terms: everybody readies again for them.
        for o in room.members.values():
            if not o.host:
                o.ready = False
        self.cancel_countdown(room, unless_forced=False)
        room.changed()
        return {"ok": True, "version": room.version}

    def act_kick(self, req, room, m, body):
        self.require_host(m)
        who = str(body.get("user", "")).lower()
        for o in list(room.members.values()):
            if o.user.lower() == who and not o.host:
                room.kicked.add(who)
                self.remove_member(room, o, "the host removed you from the room")
                return {"ok": True, "version": room.version}
        raise HttpError(404, "no such player in the room")

    def act_launch(self, req, room, m, body):
        self.require_host(m)
        if body.get("cancel"):
            self.cancel_countdown(room, unless_forced=False)
            room.changed()
            return {"ok": True, "version": room.version}
        if room.status != "open":
            raise HttpError(409, "already %s" % ("counting down" if room.status == "countdown"
                                                 else "launched"))
        force = bool(body.get("force", False))
        if not force:
            waiting = [o.user for o in room.members.values()
                       if not o.host and not o.spectator and not o.ready]
            if waiting:
                raise HttpError(409, "not everyone is ready", waiting=waiting)
        room.status = "countdown"
        room.forced = force
        room.countdown_end = time.monotonic() + self.cfg.countdown
        room.countdown_handle = asyncio.get_running_loop().call_later(
            self.cfg.countdown, self.countdown_done, room)
        room.changed()
        return {"ok": True, "version": room.version, "countdown": self.cfg.countdown}

    def act_reopen(self, req, room, m, body):
        """Back to the lobby after a match: the room is open again and nobody
        is ready."""
        self.require_host(m)
        self.cancel_countdown(room, unless_forced=False)
        room.status = "open"
        room.launched_at = 0
        # Member reaping was off during the match; everyone gets a full
        # member_timeout from here to show they are still there.
        now = time.monotonic()
        for o in room.members.values():
            o.seen = now
            if not o.host:
                o.ready = False
        room.changed()
        return {"ok": True, "version": room.version}

    def act_leave(self, req, room, m, body):
        self.remove_member(room, m, "left")
        return {"ok": True}

    def act_heartbeat(self, req, room, m, body):
        self.require_host(m)
        room.host_seen = time.monotonic()
        if "endpoints" in body:
            eps = read_endpoints(body.get("endpoints"), req.ip)
            if eps != room.endpoints:
                room.endpoints = eps
                room.changed()
        return {"ok": True, "version": room.version, "time": int(time.time())}

    def act_netinfo(self, req, room, m, body):
        """A member's path to the host (lan, direct, punch, relay, none) and
        its measured round trip, for everyone's roster. Display only: the
        lobby never acts on it. The version moves only on a change of path or
        a ping change of netinfo_ping_step ms or more."""
        if not self.windows.allow(("netinfo", m.token), self.cfg.netinfo_window[0],
                                  self.cfg.netinfo_window[1], time.monotonic()):
            raise HttpError(429, "slow down")
        path = body.get("path")
        if path is not None and path not in NETINFO_PATHS:
            raise HttpError(400, "path is one of %s" % ", ".join(NETINFO_PATHS))
        ping = body.get("ping")
        if ping is not None and (not isinstance(ping, int) or isinstance(ping, bool) or not 0 <= ping <= 9999):
            raise HttpError(400, "ping is 0-9999 ms or null")
        if m.host:
            path, ping = None, None
        moved = (path != m.path or (ping is None) != (m.ping is None)
                 or (ping is not None and abs(ping - m.ping) >= self.cfg.netinfo_ping_step))
        if moved:
            m.path, m.ping = path, ping
            room.changed()
        return {"ok": True, "version": room.version}

    def act_ticket(self, req, room, m, body):
        # Only for the connect window: a ticket held through a long lobby
        # wait is one more thing a kick cannot take back.
        if room.status not in ("countdown", "launched"):
            raise HttpError(409, "tickets are issued once the countdown starts", reason="not_started")
        ticket, expiry = self.issue_ticket(room, m)
        return {"ok": True, "ticket": ticket, "ticket_expires": expiry, "time": int(time.time())}

    # ---------------------------------------------------------- reaping

    async def reap_loop(self):
        while True:
            await asyncio.sleep(self.cfg.reap_interval)
            try:
                self.reap()
            except Exception as e:
                log("reaper: %r" % e)

    def reap(self):
        now = time.monotonic()
        wall = time.time()
        for room in list(self.rooms.values()):
            if now - room.host_seen > self.cfg.host_timeout:
                self.close_room(room, "the host stopped responding")
                continue
            # A member with a poll parked is there, whatever its last request
            # time says; the poll itself refreshes it on the way out. During a
            # match nobody is reaped: the game may well stop polling, the host's
            # heartbeat says the room is alive, and the roster is what the host
            # checks tickets against - a crashed player must still be on it to
            # come back.
            if room.status != "launched":
                for m in list(room.members.values()):
                    if not m.host and m.poll is None and now - m.seen > self.cfg.member_timeout:
                        self.remove_member(room, m, "you timed out of the room")
            if room.udp_ok_host(now, self.cfg.udp_ttl) and now - room.probe_sent > self.cfg.probe_interval:
                self.send_probe(room, now)
            lapsed = False
            for m in room.members.values():
                if m.udp_public and now - m.udp_seen > self.cfg.udp_ttl:
                    m.udp_public = None
                    m.udp_private = []
                    lapsed = True
            if lapsed:
                room.changed()
        for r in list(self.relays.values()):
            if now - r.active > self.cfg.relay_idle:
                self.close_relay(r, "idle")
        for k in [k for k, (_r, until) in self.gone.items() if until < now]:
            del self.gone[k]
        self.prune_sessions(wall)
        self.req_buckets.prune(now, 60.0)
        self.udp_buckets.prune(now, 60.0)
        self.windows.prune(now, 600.0)

    # ---------------------------------------------------------- rendezvous

    def udp_send(self, data, addr):
        try:
            self.udp.sendto(data, addr)
        except (OSError, AttributeError):
            pass

    def udp_received(self, data, addr):
        # Junk is dropped before it costs a limiter entry: sources are free
        # to forge, and an entry per forged source is memory without end.
        if len(data) < 6 or data[:4] != UDP_MAGIC or data[4] != UDP_VERSION:
            return
        ip, port = norm_addr(addr)
        now = time.monotonic()
        if not self.udp_allow(ip, now):
            return
        kind = data[5]
        try:
            if kind == UDP_REGISTER:
                self.udp_register(data, ip, port, now)
            elif kind == UDP_ECHO:
                if len(data) >= UDP_ECHO_MIN:
                    self.udp_send(udp_header(UDP_ECHO_REPLY) + bytes(data[6:14])
                                  + encode_endpoint(ip, port), addr)
            elif kind == UDP_PROBE_REPLY:
                self.udp_probe_reply(data, ip, port, now)
            elif kind == UDP_RELAY_REQUEST:
                self.udp_relay_request(data, ip, port, now)
        except (ValueError, struct.error, IndexError):
            return

    def udp_allow(self, ip, now):
        b = self.udp_buckets
        if ip in b.b or len(b.b) < self.cfg.udp_sources_max:
            return b.allow(ip, self.cfg.udp_rate, self.cfg.udp_burst, now)
        # Table full: every newcomer shares one bucket until entries age out.
        return b.allow(None, self.cfg.udp_overflow_rate, self.cfg.udp_overflow_burst, now)

    def udp_register(self, data, ip, port, now):
        if len(data) < UDP_REGISTER_MIN:
            return
        room_num = struct.unpack_from(">I", data, 6)[0]
        udp_id = bytes(data[10:18])
        seq = struct.unpack_from(">I", data, 18)[0]
        count = data[22]
        if count > ENDPOINTS_MAX:
            return
        off = 23
        private = []
        for _ in range(count):
            ep, off = decode_endpoint(data, off)
            if ep is None:
                return
            private.append(ep)
        if off + UDP_MAC_LEN != len(data):
            return

        tok = self.udp_ids.get(udp_id)
        room = self.tokens.get(tok) if tok else None
        if room is None or int(room.id, 16) != room_num:
            self.udp_send(udp_header(UDP_ERROR) + bytes((UDP_ERR_UNKNOWN,))
                          + struct.pack(">I", room_num), (ip, port))
            return
        m = room.members[tok]
        mac = register_mac(m.udp_key, bytes(data[:off]))
        if not hmac.compare_digest(mac, bytes(data[off:])) or seq <= m.udp_seq:
            self.udp_send(udp_header(UDP_ERROR) + bytes((UDP_ERR_REFUSED,))
                          + struct.pack(">I", room_num), (ip, port))
            return
        m.udp_seq = seq
        public = (ip, port)
        # Only private-range LAN candidates: anything else a member names
        # would be somebody else's address for the peer to punch at.
        lan = [ep for ep in private if lan_address(ep[0]) and ep != public]
        if m.udp_public != public or m.udp_private != lan:
            m.udp_public = public
            m.udp_private = lan
            room.changed()
        m.udp_seen = now
        self.udp_send(udp_header(UDP_REGISTERED) + struct.pack(">I", room_num)
                      + encode_endpoint(ip, port), (ip, port))

        # Pair the host with every registered member, both ways. A
        # re-register (clients repeat until they see PEER; hosts every few
        # seconds to hold their NAT mapping) re-sends, at most once a
        # PEER_MIN_INTERVAL per pair.
        host = room.host_member()
        if host is None or host.udp_public is None:
            return
        if m.host:
            others = [o for o in room.members.values() if not o.host and o.udp_public]
        else:
            others = [m]
        for o in others:
            if now - o.peer_sent < self.cfg.peer_min_interval:
                continue
            o.peer_sent = now
            cookie = room.cookies.get(o.token)
            if cookie is None:
                cookie = room.cookies[o.token] = os.urandom(8)
            self.udp_send(peer_packet(room_num, cookie, o), host.udp_public)
            self.udp_send(peer_packet(room_num, cookie, host), o.udp_public)

    def send_probe(self, room, now):
        host = room.host_member()
        room.probe_nonce = os.urandom(8)
        room.probe_sent = now
        self.udp_send(udp_header(UDP_PROBE) + struct.pack(">I", int(room.id, 16))
                      + room.probe_nonce, host.udp_public)

    def udp_probe_reply(self, data, ip, port, now):
        if len(data) < 18:
            return
        room = self.rooms.get("%08x" % struct.unpack_from(">I", data, 6)[0])
        if room is None or room.probe_nonce is None:
            return
        host = room.host_member()
        if host is None or host.udp_public != (ip, port):
            return
        if not hmac.compare_digest(bytes(data[10:18]), room.probe_nonce):
            return
        rtt = now - room.probe_sent
        room.host_rtt = rtt if room.host_rtt is None else room.host_rtt * 0.7 + rtt * 0.3
        room.probe_nonce = None

    # ---------------------------------------------------------- relay
    #
    # A pair whose NATs will not punch (a symmetric NAT on either side) plays
    # through a relay: one UDP socket here per (room, joiner), so the host's
    # game sees each relayed joiner at its own address and port. The joiner
    # asks with a signed RELAY_REQUEST to the rendezvous port; both ends get a
    # RELAY_OFFER (the port and a relay id) and BIND to that port from their
    # game sockets, signed as REGISTER is. The first BIND from an address is
    # answered with a proof (an HMAC of the relay id and that address under a
    # secret of this process); only a BIND that brings the proof back makes
    # the address the pair's end. So a datagram is only ever forwarded to an
    # address that (a) a member's udp_key signed for and (b) answered from,
    # and only as one datagram for one datagram, never larger.

    def relay_signed(self, data, off, room_num, udp_id, seq):
        """The member a signed relay datagram is from, or None (answered with
        ERROR where REGISTER would be)."""
        tok = self.udp_ids.get(udp_id)
        room = self.tokens.get(tok) if tok else None
        if room is None or int(room.id, 16) != room_num:
            return None, None, UDP_ERR_UNKNOWN
        m = room.members[tok]
        mac = register_mac(m.udp_key, bytes(data[:off]))
        if not hmac.compare_digest(mac, bytes(data[off:off + UDP_MAC_LEN])) or seq <= m.udp_seq:
            return None, None, UDP_ERR_REFUSED
        m.udp_seq = seq
        return room, m, 0

    def udp_relay_request(self, data, ip, port, now):
        if len(data) != UDP_RELAY_REQUEST_LEN:
            return
        room_num = struct.unpack_from(">I", data, 6)[0]
        udp_id = bytes(data[10:18])
        seq = struct.unpack_from(">I", data, 18)[0]
        room, m, err = self.relay_signed(data, 22, room_num, udp_id, seq)
        if room is None or m.host:
            self.udp_send(udp_header(UDP_ERROR) + bytes((err or UDP_ERR_REFUSED,))
                          + struct.pack(">I", room_num), (ip, port))
            return
        host = room.host_member()
        r = room.relays.get(m.token)
        if r is None:
            if (host is None or host.udp_public is None or len(self.relays) >= self.cfg.relay_max
                    or len(room.relays) >= self.cfg.relay_per_room):
                self.udp_send(udp_header(UDP_ERROR) + bytes((UDP_ERR_RELAY,))
                              + struct.pack(">I", room_num), (ip, port))
                return
            r = self.open_relay(room, m, host, now)
            if r is None:
                self.udp_send(udp_header(UDP_ERROR) + bytes((UDP_ERR_RELAY,))
                              + struct.pack(">I", room_num), (ip, port))
                return
        r.active = now
        # The joiner asked from here, signed: its offer comes back here. The
        # host's goes to its registered socket (as PEER does), at most twice a
        # second however often the joiner asks.
        self.udp_send(offer_packet(room_num, r, host), (ip, port))
        if host is not None and host.udp_public and now - r.offer_sent >= self.cfg.offer_min_interval:
            r.offer_sent = now
            self.udp_send(offer_packet(room_num, r, m), host.udp_public)

    def relay_port_candidates(self):
        spec = (self.cfg.relay_ports or "").strip()
        if not spec or spec == "0":
            return [0]
        lo, _, hi = spec.partition("-")
        lo, hi = int(lo), int(hi or lo)
        n = hi - lo + 1
        start = self.relay_next_port % n
        return [lo + (start + i) % n for i in range(n)]

    def open_relay(self, room, joiner, host, now):
        loop = asyncio.get_running_loop()
        fam = socket.AF_INET6 if ":" in self.cfg.udp_host else socket.AF_INET
        sock = None
        for p in self.relay_port_candidates():
            s = socket.socket(fam, socket.SOCK_DGRAM)
            try:
                s.bind((self.cfg.udp_host, p))
            except OSError:
                s.close()
                continue
            sock = s
            self.relay_next_port += 1
            break
        if sock is None:
            log("relay: no free port for room %s" % room.id)
            return None
        sock.setblocking(False)
        r = Relay(room, joiner.token, host.token, sock, now)
        self.relays[r.rid] = r
        room.relays[joiner.token] = r
        loop.add_reader(sock.fileno(), self.relay_readable, r)
        log("relay %s: port %d for %s in room %s" % (r.rid.hex(), r.port, joiner.user, room.id))
        return r

    def close_relay(self, r, why):
        if self.relays.get(r.rid) is not r:
            return
        del self.relays[r.rid]
        if r.room.relays.get(r.joiner_token) is r:
            del r.room.relays[r.joiner_token]
        try:
            asyncio.get_running_loop().remove_reader(r.sock.fileno())
        except (RuntimeError, ValueError, OSError):
            pass
        r.sock.close()
        log("relay %s closed (%s): %d datagrams, %d bytes forwarded, %d dropped"
            % (r.rid.hex(), why, r.fwd_pkts, r.fwd_bytes, r.dropped))

    def relay_readable(self, r):
        # Bounded per wake so one busy relay cannot starve the loop.
        for _ in range(64):
            try:
                data, addr = r.sock.recvfrom(2048)
            except (BlockingIOError, InterruptedError):
                return
            except OSError:
                return
            if self.relays.get(r.rid) is not r:
                return
            self.relay_datagram(r, data, norm_addr(addr), time.monotonic())

    def relay_proof(self, r, ep):
        return hmac.new(self.relay_secret, r.rid + encode_endpoint(*ep), hashlib.sha256).digest()[:8]

    def relay_datagram(self, r, data, src, now):
        if (len(data) >= 6 and data[:4] == UDP_MAGIC and data[4] == UDP_VERSION
                and UDP_RELAY_FIRST <= data[5] <= UDP_RELAY_LAST):
            if data[5] == UDP_RELAY_BIND and self.udp_allow(src[0], now):
                try:
                    self.relay_bind(r, data, src, now)
                except (ValueError, struct.error, IndexError):
                    pass
            return
        if src == r.host_ep and r.joiner_ok:
            to = r.joiner_ep
        elif src == r.joiner_ep and r.host_ok:
            to = r.host_ep
        else:
            return   # not one of the pair: never forwarded, never answered
        if len(data) > self.cfg.relay_max_datagram:
            r.dropped += 1
            return
        if not self.relay_budget(r, len(data), now):
            r.dropped += 1
            self.relay_dropped += 1
            return
        try:
            r.sock.sendto(data, to)
        except OSError:
            return
        r.active = now
        r.fwd_pkts += 1
        r.fwd_bytes += len(data)

    def relay_budget(self, r, size, now):
        """The room's and the whole relay's bytes and datagrams a second; a
        datagram over any of them is dropped (the game's own protocol copes
        with loss, and a queue here would only add latency)."""
        c, burst = self.cfg, self.cfg.relay_burst
        room = r.room
        rb, ok1 = take(room.relay_bytes, size, c.relay_room_bps, c.relay_room_bps * burst, now)
        rp, ok2 = take(room.relay_pkts, 1, c.relay_room_pps, c.relay_room_pps * burst, now)
        gb, ok3 = take(self.relay_bytes, size, c.relay_global_bps, c.relay_global_bps * burst, now)
        gp, ok4 = take(self.relay_pkts, 1, c.relay_global_pps, c.relay_global_pps * burst, now)
        if not (ok1 and ok2 and ok3 and ok4):
            # Nothing is spent on a datagram that does not go; the buckets
            # refill from their last spend.
            return False
        room.relay_bytes, room.relay_pkts, self.relay_bytes, self.relay_pkts = rb, rp, gb, gp
        return True

    def relay_bind(self, r, data, src, now):
        if len(data) != UDP_RELAY_BIND_LEN:
            return
        room_num = struct.unpack_from(">I", data, 6)[0]
        udp_id = bytes(data[10:18])
        rid = bytes(data[18:26])
        proof = bytes(data[26:34])
        seq = struct.unpack_from(">I", data, 34)[0]
        if rid != r.rid or int(r.room.id, 16) != room_num:
            return
        room, m, err = self.relay_signed(data, 38, room_num, udp_id, seq)
        if room is not r.room or m is None:
            return
        if m.token == r.host_token:
            who = "host"
        elif m.token == r.joiner_token:
            who = "joiner"
        else:
            return
        r.active = now
        want = self.relay_proof(r, src)
        if hmac.compare_digest(proof, want):
            # The address answered with the proof it was sent: it is this
            # member's, and the relay may send to it.
            setattr(r, who + "_ep", src)
            setattr(r, who + "_ok", True)
        elif getattr(r, who + "_ep") != src:
            # A new address for this end (first bind, or its NAT moved):
            # nothing goes to it until it brings the proof back.
            setattr(r, who + "_ep", src)
            setattr(r, who + "_ok", False)
        flags = (1 if r.host_ok else 0) | (2 if r.joiner_ok else 0)
        try:
            r.sock.sendto(udp_header(UDP_RELAY_BOUND) + struct.pack(">I", room_num) + r.rid
                          + bytes((flags,)) + want, src)
        except OSError:
            pass


def take(bucket, cost, rate, burst, now):
    """A token bucket as a (tokens, last) tuple: (new tuple, allowed)."""
    tokens, last = bucket
    if last == 0.0:
        tokens = burst
    tokens = min(burst, tokens + (now - last) * rate)
    if tokens < cost:
        return (tokens, now), False
    return (tokens - cost, now), True


class Relay:
    __slots__ = ("room", "joiner_token", "host_token", "sock", "port", "rid",
                 "host_ep", "joiner_ep", "host_ok", "joiner_ok", "active",
                 "offer_sent", "fwd_pkts", "fwd_bytes", "dropped")

    def __init__(self, room, joiner_token, host_token, sock, now):
        self.room = room
        self.joiner_token = joiner_token
        self.host_token = host_token
        self.sock = sock
        self.port = sock.getsockname()[1]
        self.rid = os.urandom(8)
        self.host_ep = self.joiner_ep = None
        self.host_ok = self.joiner_ok = False
        self.active = now
        self.offer_sent = 0.0
        self.fwd_pkts = self.fwd_bytes = self.dropped = 0


def offer_packet(room_num, r, other):
    """RELAY_OFFER: the relay's port and id, and who is at the other end."""
    name = other.user.encode("ascii", "replace")[:15] if other is not None else b""
    return (udp_header(UDP_RELAY_OFFER) + struct.pack(">I", room_num) + r.rid
            + struct.pack(">H", r.port) + bytes((len(name),)) + name)


def peer_packet(room_num, cookie, who):
    eps = [who.udp_public] + [ep for ep in who.udp_private if ep != who.udp_public]
    eps = eps[:1 + ENDPOINTS_MAX]
    name = who.user.encode("ascii", "replace")[:32]
    out = bytearray(udp_header(UDP_PEER))
    out += struct.pack(">I", room_num) + cookie
    out += bytes((who.role(), len(name))) + name
    out += bytes((len(eps),))
    for ip, port in eps:
        out += encode_endpoint(ip, port)
    return bytes(out)


class Rendezvous(asyncio.DatagramProtocol):
    def __init__(self, lobby):
        self.lobby = lobby

    def datagram_received(self, data, addr):
        self.lobby.udp_received(data, addr)

    def error_received(self, exc):
        # An ICMP unreachable from a peer that went away is not ours to act on.
        pass


# ------------------------------------------------------------------ request parsing

REASONS = {200: "OK", 400: "Bad Request", 401: "Unauthorized", 403: "Forbidden",
           404: "Not Found", 409: "Conflict", 410: "Gone", 411: "Length Required",
           413: "Payload Too Large", 429: "Too Many Requests", 431: "Headers Too Large",
           502: "Bad Gateway", 503: "Service Unavailable"}

ROOM_ACTIONS = frozenset(("ready", "team", "chat", "settings", "kick", "launch",
                          "reopen", "leave", "heartbeat", "ticket", "netinfo"))


class Request:
    __slots__ = ("method", "path", "query", "headers", "body", "ip", "close")

    def json(self):
        """The body as a JSON object; an empty body is an empty object, and
        anything that is not an object is a 400."""
        if not self.body:
            return {}
        try:
            obj = json.loads(self.body)
        except (ValueError, RecursionError):
            raise HttpError(400, "bad body")
        if not isinstance(obj, dict):
            raise HttpError(400, "bad body")
        return obj


def parse_request_head(head, peer_ip):
    try:
        text = head.decode("latin-1")
    except Exception:
        return None
    lines = text.split("\r\n")
    parts = lines[0].split(" ")
    if len(parts) != 3 or not parts[2].startswith("HTTP/1."):
        return None
    req = Request()
    req.method = parts[0]
    target = parts[1]
    path, _, qs = target.partition("?")
    req.path = path
    req.query = {k: v[0] for k, v in urllib.parse.parse_qs(qs).items()}
    req.headers = {}
    for line in lines[1:]:
        if not line:
            continue
        k, sep, v = line.partition(":")
        if not sep:
            return None
        req.headers[k.strip().lower()] = v.strip()
    req.body = b""
    conn = req.headers.get("connection", "").lower()
    req.close = conn == "close" or (parts[2] == "HTTP/1.0" and conn != "keep-alive")
    req.ip = client_ip(req.headers, peer_ip)
    return req


def client_ip(headers, peer_ip):
    """X-Real-IP, then the LAST X-Forwarded-For element, then the socket -
    exactly pdghostd's rule and for its reason: nginx writes those two, and the
    first XFF element is whatever the client sent."""
    real = headers.get("x-real-ip", "").strip()
    if real:
        return real
    fwd = headers.get("x-forwarded-for", "")
    if fwd:
        return fwd.rsplit(",", 1)[-1].strip()
    return peer_ip


def read_compat(body):
    proto = body.get("proto")
    if not isinstance(proto, int) or isinstance(proto, bool) or not 1 <= proto <= 65535:
        raise HttpError(400, "proto must be 1-65535")
    build = str(body.get("build", ""))
    if not BUILD_RE.match(build):
        raise HttpError(400, "bad build")
    content = str(body.get("content", "")).lower()
    if not CONTENT_RE.match(content):
        raise HttpError(400, "content must be 8-64 hex digits")
    return proto, build, content


def read_rules(rules):
    if not isinstance(rules, dict) or len(rules) > RULES_MAX:
        raise HttpError(400, "rules is an object of up to %d fields" % RULES_MAX)
    out = {}
    for k, v in rules.items():
        if not RULE_KEY_RE.match(k):
            raise HttpError(400, "bad rules key")
        if isinstance(v, bool):
            out[k] = v
        elif isinstance(v, int) and -2 ** 31 <= v < 2 ** 31:
            out[k] = v
        elif isinstance(v, str):
            out[k] = clean_text(v, FIELD_MAX)
        else:
            raise HttpError(400, "rules values are booleans, 32-bit integers or short strings")
    return out


def read_endpoints(eps, ip):
    """The host's advertised endpoints: malformed is a 400; well-formed but
    neither a private-range address nor ip (the address the request came
    from, where a UPnP mapping lives) is quietly left out, since joiners
    connect to these and a host does not get to aim them at third parties."""
    if not isinstance(eps, list) or len(eps) > ENDPOINTS_MAX:
        raise HttpError(400, "endpoints is a list of up to %d" % ENDPOINTS_MAX)
    own = norm_addr((ip, 0))[0]
    out = []
    for e in eps:
        p = parse_endpoint(e)
        if p is None:
            raise HttpError(400, "bad endpoint")
        if lan_address(p[0]) or p[0] == own:
            out.append(format_endpoint(*p))
    return out


def ghost_login(base, username, pin, ip, timeout):
    """pdghostd's /login, from a worker thread. -> (True, None), (False,
    error) for a refusal, (None, None) when the account server could not be
    asked. The player's address goes in X-Real-IP so pdghostd limits the
    player rather than this process."""
    data = json.dumps({"username": username, "pin": pin}).encode()
    r = urllib.request.Request(base.rstrip("/") + "/login", data=data, method="POST",
                               headers={"Content-Type": "application/json", "X-Real-IP": ip})
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            reply = json.loads(resp.read(65536) or b"{}")
            return bool(reply.get("ok")), reply.get("error")
    except urllib.error.HTTPError as e:
        if e.code in (403, 429, 400):
            try:
                reply = json.loads(e.read(65536) or b"{}")
            except ValueError:
                reply = {}
            return False, reply.get("error")
        return None, None
    except (OSError, ValueError):
        return None, None


def log(msg):
    print(msg, flush=True)


# ------------------------------------------------------------------ main

def main():
    cfg = Config()
    args = sys.argv[1:]
    for flag, attr, conv in (("--port", "port", int), ("--udp-port", "udp_port", int),
                             ("--host", "host", str), ("--udp-host", "udp_host", str),
                             ("--auth", "auth", str), ("--ghost-url", "ghost_url", str),
                             ("--relay-ports", "relay_ports", str)):
        if flag in args:
            setattr(cfg, attr, conv(args[args.index(flag) + 1]))

    async def run():
        lobby = Lobby(cfg)
        await lobby.start()
        log("pdlobbyd listening on %s:%d, rendezvous udp %s:%d, auth %s, relay ports %s"
            % (cfg.host, lobby.bound_port, cfg.udp_host, lobby.bound_udp_port, cfg.auth,
               cfg.relay_ports or "any"))
        await asyncio.Event().wait()

    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
