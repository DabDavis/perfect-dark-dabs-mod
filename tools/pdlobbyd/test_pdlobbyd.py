#!/usr/bin/env python3
"""
Exercise pdlobbyd on ephemeral ports, in-process, with short timeouts.

    python3 tools/pdlobbyd/test_pdlobbyd.py

Nothing but the standard library and nothing beyond 127.0.0.1. Each test case
gets its own server (its own event loop on a background thread, HTTP and UDP
on ports the kernel picks), so one case's rooms and rate limits never leak
into the next. The account server is a stub on another ephemeral port that
answers /login the way pdghostd does.
"""

import asyncio
import http.client
import json
import os
import socket
import struct
import sys
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pdlobbyd as L  # noqa: E402

L.log = lambda msg: None

CONTENT = "0123456789abcdef0123456789abcdef"
PROTO = 1
BUILD = "v3.9.0-abc123"


class LobbyThread:
    """A Lobby on its own loop on a background thread."""

    def __init__(self, **over):
        cfg = dict(port=0, udp_port=0, udp_host="127.0.0.1", auth="open",
                   reap_interval=0.05, countdown=0.3, relay_ports="")
        cfg.update(over)
        self.cfg = L.Config(**cfg)
        self.loop = asyncio.new_event_loop()
        self.lobby = None
        ready = threading.Event()

        def run():
            asyncio.set_event_loop(self.loop)
            self.lobby = L.Lobby(self.cfg)
            self.loop.run_until_complete(self.lobby.start())
            ready.set()
            self.loop.run_forever()

        self.thread = threading.Thread(target=run, daemon=True)
        self.thread.start()
        if not ready.wait(10):
            raise RuntimeError("lobby did not start")
        self.port = self.lobby.bound_port
        self.udp_port = self.lobby.bound_udp_port

    def stop(self):
        fut = asyncio.run_coroutine_threadsafe(self.lobby.stop(), self.loop)
        fut.result(10)
        self.loop.call_soon_threadsafe(self.loop.stop)
        self.thread.join(10)
        self.loop.close()

    def call(self, fn):
        """Run fn() on the lobby's loop and return its result (for looking at
        state without racing the loop)."""
        done = threading.Event()
        box = {}

        def go():
            box["v"] = fn()
            done.set()
        self.loop.call_soon_threadsafe(go)
        done.wait(10)
        return box.get("v")


class Client:
    def __init__(self, port, ip="10.0.0.1"):
        self.port = port
        self.ip = ip
        self.session = None
        self.token = None

    def req(self, method, path, body=None, raw=None, headers=None, timeout=40):
        h = {"X-Real-IP": self.ip}
        if self.session:
            h["Authorization"] = "Bearer " + self.session
        if self.token:
            h["X-PD-Member"] = self.token
        h.update(headers or {})
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        conn.request(method, path, body=data, headers=h)
        resp = conn.getresponse()
        out = resp.read()
        conn.close()
        try:
            out = json.loads(out)
        except ValueError:
            pass
        return resp.status, out

    def login(self, name, pin=None):
        body = {"username": name}
        if pin is not None:
            body["pin"] = pin
        st, r = self.req("POST", "/login", body)
        if st == 200:
            self.session = r["session"]
        return st, r

    def create(self, **kw):
        body = dict(name="Test Room", max_humans=4, stage="Skedar Ruins",
                    scenario="Combat", sims=2, proto=PROTO, build=BUILD, content=CONTENT,
                    endpoints=["192.168.1.5:27100", "8.8.8.8:27100"],
                    rules={"time_limit": 10, "teams": False, "weapons": "Slayer"})
        body.update(kw)
        st, r = self.req("POST", "/rooms", body)
        if st == 200:
            self.token = r["token"]
            self.room = r["room"]
            self.secret = bytes.fromhex(r["secret"])
            self.udp_id = bytes.fromhex(r["udp_id"])
            self.udp_key = bytes.fromhex(r["udp_key"])
            self.seq = 0
        return st, r

    def join(self, room, **kw):
        body = dict(proto=PROTO, build=BUILD, content=CONTENT)
        body.update(kw)
        st, r = self.req("POST", "/rooms/%s/join" % room, body)
        if st == 200:
            self.token = r["token"]
            self.room = room
            self.udp_id = bytes.fromhex(r["udp_id"])
            self.udp_key = bytes.fromhex(r["udp_key"])
            self.seq = 0
        return st, r

    def register(self, private=()):
        self.seq += 1
        return register(self.room, self.udp_id, self.udp_key, self.seq, private)

    def act(self, action, body=None):
        return self.req("POST", "/rooms/%s/%s" % (self.room, action), body or {})

    def state(self, since=0, wait=0):
        return self.req("GET", "/rooms/%s/state?since=%d&wait=%s" % (self.room, since, wait))


class Base(unittest.TestCase):
    over = {}

    def setUp(self):
        self.srv = LobbyThread(**self.over)

    def tearDown(self):
        self.srv.stop()

    def client(self, name, ip=None):
        c = Client(self.srv.port, ip or "10.0.%d.%d" % (len(name), sum(map(ord, name)) % 250 + 1))
        st, r = c.login(name)
        self.assertEqual(st, 200, r)
        return c

    def room_with(self, *names, **create):
        host = self.client("hostess")
        st, r = host.create(**create)
        self.assertEqual(st, 200, r)
        others = []
        for n in names:
            c = self.client(n)
            st, r = c.join(host.room)
            self.assertEqual(st, 200, r)
            others.append(c)
        return host, others


# ------------------------------------------------------------------ rooms

class RoomTests(Base):
    def test_ping(self):
        st, r = Client(self.srv.port).req("GET", "/ping")
        self.assertEqual(st, 200)
        self.assertEqual(r["udp_port"], self.srv.udp_port)

    def test_create_and_list(self):
        host = self.client("hostess")
        st, r = host.create(name="  Dab's\x1b[31m\tRoom  ", password="pw")
        self.assertEqual(st, 200, r)
        self.assertRegex(r["room"], r"^[0-9a-f]{8}$")
        self.assertRegex(r["token"], r"^[0-9a-f]{32}$")
        self.assertEqual(len(r["secret"]), 64)

        st, r = Client(self.srv.port).req("GET", "/rooms?proto=1&build=" + BUILD)
        self.assertEqual(st, 200)
        self.assertEqual(len(r["rooms"]), 1)
        room = r["rooms"][0]
        self.assertEqual(room["name"], "Dab's [31m Room")
        self.assertEqual(room["host"], "hostess")
        self.assertEqual(room["stage"], "Skedar Ruins")
        self.assertEqual((room["humans"], room["max_humans"], room["sims"]), (1, 4, 2))
        self.assertTrue(room["locked"])
        self.assertTrue(room["same_build"])
        self.assertEqual(room["content"], CONTENT)
        self.assertIsNone(room["host_rtt_ms"])
        self.assertNotIn("secret", room)
        self.assertNotIn("endpoints", room)

        st, r = Client(self.srv.port).req("GET", "/rooms?proto=2")
        self.assertEqual(r["rooms"], [])
        st, r = Client(self.srv.port).req("GET", "/rooms?build=other")
        self.assertFalse(r["rooms"][0]["same_build"])

    def test_create_validation(self):
        host = self.client("hostess")
        for bad in (dict(max_humans=1), dict(max_humans=13), dict(name="\x01\x02"),
                    dict(content="xyz"), dict(proto=0), dict(build="a b"),
                    dict(rules={"Bad Key": 1}), dict(rules={"k": [1]}),
                    dict(endpoints=["nope"]), dict(password="x" * 17), dict(sims=81),
                    dict(sims=-1)):
            st, r = host.create(**bad)
            self.assertEqual(st, 400, (bad, r))
        st, r = Client(self.srv.port).req("POST", "/rooms", {"name": "x"})
        self.assertEqual(st, 401)
        # every simulant slot the game has (MAX_BOTS) is a room's to fill
        st, r = host.create(sims=80)
        self.assertEqual(st, 200, r)

    def test_join_password_and_mismatch(self):
        host = self.client("hostess")
        host.create(password="secret")
        j = self.client("joiner")
        st, r = j.join(host.room)
        self.assertEqual((st, r["reason"]), (403, "password"))
        st, r = j.join(host.room, password="wrong")
        self.assertEqual(st, 403)
        st, r = j.join(host.room, content="ffffffffffffffff")
        self.assertEqual((st, r["reason"]), (409, "content"))
        self.assertIn("content", r["error"])
        st, r = j.join(host.room, proto=2, password="secret")
        self.assertEqual((st, r["reason"]), (409, "proto"))
        self.assertIn("protocol 1", r["error"])
        st, r = j.join(host.room, password="secret")
        self.assertEqual(st, 200, r)
        # No ticket while the room is open: tickets are for the connect window.
        self.assertNotIn("ticket", r)
        self.assertEqual(j.act("ticket")[1]["reason"], "not_started")
        # Joining again keeps the seat.
        tok = j.token
        st, r = j.join(host.room, password="secret")
        self.assertEqual((st, r["token"]), (200, tok))

    def test_bad_password_limit(self):
        host = self.client("hostess")
        host.create(password="secret")
        j = self.client("joiner")
        codes = [j.join(host.room, password="no")[0] for _ in range(7)]
        self.assertEqual(codes[:5], [403] * 5)
        self.assertEqual(codes[5:], [429, 429])
        self.assertEqual(j.join(host.room, password="secret")[0], 429)

    def test_full_room_and_spectators(self):
        host, (a,) = self.room_with("alpha", max_humans=2)
        b = self.client("bravo")
        st, r = b.join(host.room)
        self.assertEqual((st, r["reason"]), (409, "full"))
        st, r = b.join(host.room, spectator=True)
        self.assertEqual(st, 200, r)
        st, s = b.state()
        me = [m for m in s["members"] if m["user"] == "bravo"][0]
        self.assertTrue(me["spectator"])

    def test_ready_team_chat_state(self):
        host, (a, b) = self.room_with("alpha", "bravo")
        st, s = a.state()
        self.assertEqual(st, 200)
        v = s["version"]
        self.assertEqual([m["user"] for m in s["members"]], ["hostess", "alpha", "bravo"])
        self.assertEqual([m["team"] for m in s["members"]], [0, 1, 0])
        self.assertTrue(s["members"][0]["host"])
        self.assertEqual(s["rules"]["weapons"], "Slayer")

        self.assertEqual(a.act("ready", {"ready": True})[0], 200)
        self.assertEqual(a.act("team", {"team": 3})[0], 200)
        self.assertEqual(a.act("team", {"team": 9})[0], 400)
        self.assertEqual(host.act("ready")[0], 400)
        st, r = b.act("chat", {"text": "hello\x07 \x1b[2Jthere " + "x" * 300})
        self.assertEqual(st, 200)
        st, s = a.state(since=v)
        self.assertGreater(s["version"], v)
        alpha = [m for m in s["members"] if m["user"] == "alpha"][0]
        self.assertEqual((alpha["ready"], alpha["team"]), (True, 3))
        self.assertEqual(len(s["chat"]), 1)
        line = s["chat"][0]
        self.assertEqual(line["user"], "bravo")
        self.assertTrue(line["text"].startswith("hello [2Jthere x"))
        self.assertEqual(len(line["text"]), L.CHAT_MAX)
        # Chat seen once: since the version it was posted at, it is not sent again.
        st, s2 = a.state(since=s["version"])
        self.assertEqual(s2["chat"], [])
        self.assertEqual(b.act("chat", {"text": "   "})[0], 400)

    def test_chat_rate(self):
        host, (a,) = self.room_with("alpha")
        codes = [a.act("chat", {"text": "spam %d" % i})[0] for i in range(8)]
        self.assertEqual(codes[:6], [200] * 6)
        self.assertEqual(codes[6:], [429, 429])

    def test_settings_host_only_and_unready(self):
        host, (a,) = self.room_with("alpha")
        a.act("ready")
        self.assertEqual(a.act("settings", {"name": "mine"})[0], 403)
        st, r = host.act("settings", {"name": "New Name", "stage": "Felicity",
                                      "password": "pw", "max_humans": 6})
        self.assertEqual(st, 200, r)
        st, s = a.state()
        self.assertEqual(s["room"]["name"], "New Name")
        self.assertEqual(s["room"]["stage"], "Felicity")
        self.assertTrue(s["room"]["locked"])
        self.assertFalse([m for m in s["members"] if m["user"] == "alpha"][0]["ready"])
        self.assertEqual(host.act("settings", {"max_humans": 1})[0], 400)

    def test_kick(self):
        host, (a,) = self.room_with("alpha")
        self.assertEqual(a.act("kick", {"user": "hostess"})[0], 403)
        self.assertEqual(host.act("kick", {"user": "nobody"})[0], 404)
        self.assertEqual(host.act("kick", {"user": "ALPHA"})[0], 200)
        st, r = a.state()
        self.assertEqual(st, 410)
        self.assertIn("removed", r["error"])
        st, r = a.join(host.room)
        self.assertEqual((st, r["reason"]), (403, "kicked"))

    def test_launch_countdown(self):
        host, (a, b) = self.room_with("alpha", "bravo")
        st, r = host.act("launch")
        self.assertEqual(st, 409)
        self.assertEqual(sorted(r["waiting"]), ["alpha", "bravo"])
        self.assertEqual(a.act("launch")[0], 403)
        a.act("ready")
        b.act("ready")
        st, r = host.act("launch")
        self.assertEqual(st, 200, r)
        st, s = a.state()
        self.assertEqual(s["room"]["state"], "countdown")
        self.assertIsNotNone(s["countdown"])
        self.assertIn("ticket", s["you"])
        # Joining a match that is starting is refused.
        c = self.client("charlie")
        self.assertEqual(c.join(host.room)[1]["reason"], "started")
        # The countdown runs out into launched, with the host's endpoints.
        st, s = a.state(since=s["version"], wait=5)
        self.assertEqual(s["room"]["state"], "launched")
        # 8.8.8.8 is neither private nor the host's own address: left out.
        self.assertEqual(s["launch"]["endpoints"], ["192.168.1.5:27100"])
        self.assertIsNone(s["launch"]["public"])
        ok, user, _ = L.verify_ticket(host.secret, s["you"]["ticket"], host.room,
                                      now=s["time"], roster={"alpha", "bravo"})
        self.assertTrue(ok)
        self.assertEqual(user, "alpha")
        self.assertLessEqual(s["you"]["ticket_expires"] - s["time"], 30)
        st, r = b.act("ticket")
        self.assertEqual(st, 200, r)
        self.assertTrue(L.verify_ticket(host.secret, r["ticket"], host.room)[0])
        # And back to the lobby.
        self.assertEqual(host.act("reopen")[0], 200)
        st, s = a.state()
        self.assertEqual(s["room"]["state"], "open")
        self.assertIsNone(s["launch"])

    def test_unready_cancels_countdown_unless_forced(self):
        host, (a, b) = self.room_with("alpha", "bravo")
        a.act("ready")
        b.act("ready")
        host.act("launch")
        a.act("ready", {"ready": False})
        st, s = b.state()
        self.assertEqual(s["room"]["state"], "open")
        st, r = host.act("launch", {"force": True})
        self.assertEqual(st, 200, r)
        b.act("ready", {"ready": False})
        st, s = b.state()
        self.assertEqual(s["room"]["state"], "countdown")
        self.assertTrue(s["countdown"]["forced"])
        host.act("launch", {"cancel": True})
        st, s = b.state()
        self.assertEqual(s["room"]["state"], "open")

    def test_leave_and_host_leave(self):
        host, (a, b) = self.room_with("alpha", "bravo")
        self.assertEqual(a.act("leave")[0], 200)
        self.assertEqual(a.state()[0], 410)
        st, s = b.state()
        self.assertEqual(len(s["members"]), 2)
        host.act("leave")
        st, r = b.state()
        self.assertEqual(st, 410)
        self.assertIn("host closed", r["error"])
        self.assertEqual(Client(self.srv.port).req("GET", "/rooms")[1]["rooms"], [])

    def test_one_room_per_user(self):
        h1 = self.client("hostone")
        h1.create()
        h2 = self.client("hosttwo", ip="10.9.9.9")
        h2.create()
        j = self.client("joiner")
        j.join(h1.room)
        first = j.token
        j.join(h2.room)
        st, r = j.req("GET", "/rooms/%s/state" % h1.room, headers={"X-PD-Member": first})
        self.assertEqual(st, 410)
        self.assertIn("another room", r["error"])

    def test_dedicated_host_is_not_a_player(self):
        host = self.client("server1")
        host.create(dedicated=True, max_humans=2)
        a, b = self.client("alpha"), self.client("bravo")
        self.assertEqual(a.join(host.room)[0], 200)
        self.assertEqual(b.join(host.room)[0], 200)
        st, r = Client(self.srv.port).req("GET", "/rooms")
        self.assertEqual(r["rooms"][0]["humans"], 2)

    def test_member_token_wrong_room(self):
        h1 = self.client("hostone")
        h1.create()
        h2 = self.client("hosttwo", ip="10.9.9.9")
        h2.create()
        st, r = h1.req("GET", "/rooms/%s/state" % h2.room)
        self.assertEqual((st, r["reason"]), (410, "gone"))
        st, r = Client(self.srv.port).req("GET", "/rooms/%s/state" % h1.room)
        self.assertEqual(st, 401)


# ------------------------------------------------------------------ long-poll

class PollTests(Base):
    def test_long_poll_wakes_on_change(self):
        host, (a,) = self.room_with("alpha")
        st, s = a.state()
        v = s["version"]
        out = {}

        def poll():
            t = time.monotonic()
            out["r"] = a.state(since=v, wait=10)
            out["t"] = time.monotonic() - t
        th = threading.Thread(target=poll)
        th.start()
        time.sleep(0.3)
        self.assertTrue(th.is_alive())
        self.assertEqual(self.srv.call(lambda: self.srv.lobby.waiters), 1)
        host.act("chat", {"text": "go"})
        th.join(5)
        st, s = out["r"]
        self.assertEqual(st, 200)
        self.assertLess(out["t"], 2.0)
        self.assertGreater(s["version"], v)
        self.assertEqual(s["chat"][0]["text"], "go")
        self.assertEqual(self.srv.call(lambda: self.srv.lobby.waiters), 0)

    def test_long_poll_times_out_unchanged(self):
        host, (a,) = self.room_with("alpha")
        st, s = a.state()
        t = time.monotonic()
        st, s2 = a.state(since=s["version"], wait=0.4)
        self.assertGreaterEqual(time.monotonic() - t, 0.35)
        self.assertEqual(s2["version"], s["version"])

    def test_long_poll_sees_room_close(self):
        host, (a,) = self.room_with("alpha")
        v = a.state()[1]["version"]
        out = {}
        th = threading.Thread(target=lambda: out.setdefault("r", a.state(since=v, wait=10)))
        th.start()
        time.sleep(0.2)
        host.act("leave")
        th.join(5)
        self.assertEqual(out["r"][0], 410)

    def test_poll_cap(self):
        self.srv.call(lambda: setattr(self.srv.cfg, "max_waiters", 0))
        host, (a,) = self.room_with("alpha")
        v = a.state()[1]["version"]
        t = time.monotonic()
        self.assertEqual(a.state(since=v, wait=5)[0], 200)
        self.assertLess(time.monotonic() - t, 1.0)


# ------------------------------------------------------------------ reaping

class ReapTests(Base):
    over = dict(host_timeout=0.6, member_timeout=0.8)

    def test_host_reaped_without_heartbeat(self):
        host, (a,) = self.room_with("alpha")
        for _ in range(4):
            time.sleep(0.3)
            self.assertEqual(host.act("heartbeat")[0], 200)
            a.state()
        self.assertEqual(len(Client(self.srv.port).req("GET", "/rooms")[1]["rooms"]), 1)
        time.sleep(1.0)
        self.assertEqual(Client(self.srv.port).req("GET", "/rooms")[1]["rooms"], [])
        st, r = a.state()
        self.assertEqual(st, 410)
        self.assertIn("stopped responding", r["error"])
        self.assertEqual(a.act("heartbeat")[0], 410)

    def test_member_reaped_on_inactivity(self):
        host, (a, b) = self.room_with("alpha", "bravo")
        for _ in range(5):
            time.sleep(0.3)
            host.act("heartbeat")
            b.state()
        st, s = b.state()
        self.assertEqual([m["user"] for m in s["members"]], ["hostess", "bravo"])
        st, r = a.state()
        self.assertEqual(st, 410)
        self.assertIn("timed out", r["error"])

    def test_parked_poll_keeps_member(self):
        host, (a,) = self.room_with("alpha")
        v = a.state()[1]["version"]
        out = {}
        th = threading.Thread(target=lambda: out.setdefault("r", a.state(since=v, wait=1.5)))
        th.start()
        for _ in range(5):
            time.sleep(0.3)
            host.act("heartbeat")
        th.join(5)
        self.assertEqual(out["r"][0], 200)
        self.assertEqual(len(out["r"][1]["members"]), 2)


# ------------------------------------------------------------------ tickets

class TicketTests(unittest.TestCase):
    secret = bytes(range(32))

    def test_valid(self):
        t = L.make_ticket(self.secret, "joiner", "0a1b2c3d", 2000000000, "ab" * 16)
        self.assertEqual(t.split("|")[:4], ["joiner", "0a1b2c3d", "2000000000", "ab" * 16])
        ok, user, why = L.verify_ticket(self.secret, t, "0a1b2c3d", now=1999999999)
        self.assertEqual((ok, user, why), (True, "joiner", None))

    def test_known_vector(self):
        # Pinned so the C implementation can be checked against it byte for byte.
        t = L.make_ticket(self.secret, "joiner", "0a1b2c3d", 2000000000, "ab" * 16)
        # Key: bytes 0x00..0x1f. Message: "joiner|0a1b2c3d|2000000000|abab...ab".
        self.assertEqual(t, "joiner|0a1b2c3d|2000000000|" + "ab" * 16 + "|"
                         "4985ec2af1530d6054140a70be302817cb3d097494cdad12dbec75ed3e9f8a01")

    def test_expired(self):
        t = L.make_ticket(self.secret, "joiner", "0a1b2c3d", 1000000000)
        self.assertEqual(L.verify_ticket(self.secret, t, "0a1b2c3d", now=1000000000)[2],
                         "ticket expired")

    def test_expiry_is_ten_digits(self):
        # The C side reads it into a u64 and sizes its buffer to TICKET_MAX.
        self.assertEqual(L.TICKET_MAX, 133)
        for exp in (999999999, 10000000000, 99999999999999999999):
            t = L.make_ticket(self.secret, "joiner", "0a1b2c3d", exp)
            self.assertEqual(L.verify_ticket(self.secret, t, "0a1b2c3d", now=1)[2], "malformed ticket")
        t = L.make_ticket(self.secret, "longestname_15c", "0a1b2c3d", 9999999999)
        self.assertEqual(len(t), L.TICKET_MAX)
        self.assertTrue(L.verify_ticket(self.secret, t, "0a1b2c3d", now=1)[0])
        self.assertEqual(L.verify_ticket(self.secret, t + "\n", "0a1b2c3d", now=1)[2],
                         "malformed ticket")

    def test_roster(self):
        # A valid mac is not a seat: the user must be on the host's roster.
        t = L.make_ticket(self.secret, "Joiner", "0a1b2c3d", 2000000000)
        self.assertTrue(L.verify_ticket(self.secret, t, "0a1b2c3d", now=1, roster={"joiner"})[0])
        self.assertEqual(L.verify_ticket(self.secret, t, "0a1b2c3d", now=1, roster={"other"})[2],
                         "not in the room")

    def test_tamper(self):
        t = L.make_ticket(self.secret, "joiner", "0a1b2c3d", 2000000000)
        for bad in (t.replace("joiner", "jo1ner"), t.replace("2000000000", "2000000001"),
                    t[:-1] + ("0" if t[-1] != "0" else "1")):
            ok, _u, why = L.verify_ticket(self.secret, bad, "0a1b2c3d", now=1)
            self.assertFalse(ok)
            self.assertEqual(why, "ticket signature does not match")
        self.assertFalse(L.verify_ticket(b"\0" * 32, t, "0a1b2c3d", now=1)[0])
        self.assertEqual(L.verify_ticket(self.secret, t, "ffffffff", now=1)[2],
                         "ticket is for another room")
        for bad in ("", "a|b", t + "|x", t.replace("joiner", "jo|ner"), "x" * 500):
            self.assertEqual(L.verify_ticket(self.secret, bad, "0a1b2c3d", now=1)[2], "malformed ticket")

    def test_replay(self):
        seen = {}
        t = L.make_ticket(self.secret, "joiner", "0a1b2c3d", 2000000000)
        self.assertTrue(L.verify_ticket(self.secret, t, "0a1b2c3d", now=1, seen_nonces=seen)[0])
        self.assertEqual(L.verify_ticket(self.secret, t, "0a1b2c3d", now=1, seen_nonces=seen)[2],
                         "ticket already used")


# ------------------------------------------------------------------ rendezvous

def udp_sock():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(2.0)
    return s


def register(room, udp_id, key, seq, private=()):
    out = L.udp_header(L.UDP_REGISTER) + struct.pack(">I", int(room, 16)) + udp_id
    out += struct.pack(">I", seq) + bytes((len(private),))
    for ip, port in private:
        out += L.encode_endpoint(ip, port)
    return out + L.register_mac(key, out)


def parse_peer(d):
    assert d[:6] == L.udp_header(L.UDP_PEER), d[:6]
    room = struct.unpack_from(">I", d, 6)[0]
    cookie = d[10:18]
    role, nlen = d[18], d[19]
    name = d[20:20 + nlen].decode()
    off = 20 + nlen
    n = d[off]
    off += 1
    eps = []
    for _ in range(n):
        ep, off = L.decode_endpoint(d, off)
        eps.append(ep)
    assert off == len(d)
    return room, cookie, role, name, eps


def recv_kind(sock, kind, tries=5):
    for _ in range(tries):
        d, _a = sock.recvfrom(2048)
        if d[5] == kind:
            return d
    raise AssertionError("no packet of type %d" % kind)


class RendezvousTests(Base):
    over = dict(probe_interval=0.2)

    def test_exchange(self):
        host, (a,) = self.room_with("alpha")
        hs, js = udp_sock(), udp_sock()
        to = ("127.0.0.1", self.srv.udp_port)

        hs.sendto(host.register([("192.168.1.5", 27100)]), to)
        d = recv_kind(hs, L.UDP_REGISTERED)
        self.assertEqual(struct.unpack_from(">I", d, 6)[0], int(host.room, 16))
        self.assertEqual(L.decode_endpoint(d, 10)[0], hs.getsockname())

        js.sendto(a.register([("10.1.2.3", 27100)]), to)
        recv_kind(js, L.UDP_REGISTERED)
        hp = parse_peer(recv_kind(hs, L.UDP_PEER))
        jp = parse_peer(recv_kind(js, L.UDP_PEER))
        # The host hears about the joiner, the joiner about the host, public first.
        self.assertEqual(hp[3], "alpha")
        self.assertEqual(hp[2], L.ROLE_PLAYER)
        self.assertEqual(hp[4], [js.getsockname(), ("10.1.2.3", 27100)])
        self.assertEqual(jp[3], "hostess")
        self.assertEqual(jp[2], L.ROLE_HOST)
        self.assertEqual(jp[4], [hs.getsockname(), ("192.168.1.5", 27100)])
        self.assertEqual(hp[1], jp[1])
        self.assertEqual(hp[0], int(host.room, 16))

        # The roster says who has a UDP path.
        st, s = a.state()
        self.assertTrue(all(m["udp"] for m in s["members"]))

        # The host is probed; its answer becomes the list's ping hint.
        d = recv_kind(hs, L.UDP_PROBE, tries=10)
        hs.sendto(L.udp_header(L.UDP_PROBE_REPLY) + d[6:18], to)
        for _ in range(20):
            rooms = Client(self.srv.port).req("GET", "/rooms")[1]["rooms"]
            if rooms[0]["host_rtt_ms"] is not None:
                break
            time.sleep(0.05)
        self.assertIsNotNone(rooms[0]["host_rtt_ms"])
        hs.close()
        js.close()

    def test_unknown_token_and_garbage(self):
        s = udp_sock()
        to = ("127.0.0.1", self.srv.udp_port)
        s.sendto(register("01020304", b"\0" * 8, b"\0" * 32, 1), to)
        d = recv_kind(s, L.UDP_ERROR)
        self.assertEqual(d[6], L.UDP_ERR_UNKNOWN)
        self.assertEqual(struct.unpack_from(">I", d, 7)[0], 0x01020304)
        for junk in (b"", b"PDLB", b"XXXX\x01\x01" + b"\0" * 30, b"PDLB\x02\x05" + b"\0" * 40,
                     L.udp_header(L.UDP_REGISTER) + b"\0" * 5,
                     L.udp_header(L.UDP_ECHO) + b"\0" * 10,
                     L.udp_header(0x10) + b"\0" * 40):
            s.sendto(junk, to)
        s.settimeout(0.3)
        with self.assertRaises(socket.timeout):
            s.recvfrom(2048)
        s.close()

    def test_echo(self):
        s = udp_sock()
        s.sendto(L.udp_header(L.UDP_ECHO) + b"nonce123" + b"\0" * 26, ("127.0.0.1", self.srv.udp_port))
        d = recv_kind(s, L.UDP_ECHO_REPLY)
        self.assertEqual(d[6:14], b"nonce123")
        self.assertEqual(L.decode_endpoint(d, 14)[0], s.getsockname())
        s.close()


# ------------------------------------------------------------------ relay

def relay_request(c):
    c.seq += 1
    out = L.udp_header(L.UDP_RELAY_REQUEST) + struct.pack(">I", int(c.room, 16)) + c.udp_id
    out += struct.pack(">I", c.seq)
    return out + L.register_mac(c.udp_key, out)


def relay_bind(c, rid, proof=b"\0" * 8):
    c.seq += 1
    out = L.udp_header(L.UDP_RELAY_BIND) + struct.pack(">I", int(c.room, 16)) + c.udp_id
    out += rid + proof + struct.pack(">I", c.seq)
    return out + L.register_mac(c.udp_key, out)


def parse_offer(d):
    assert d[:6] == L.udp_header(L.UDP_RELAY_OFFER), d[:6]
    room = struct.unpack_from(">I", d, 6)[0]
    rid = d[10:18]
    port = struct.unpack_from(">H", d, 18)[0]
    n = d[20]
    name = d[21:21 + n].decode()
    assert len(d) == 21 + n
    return room, rid, port, name


def parse_bound(d):
    assert d[:6] == L.udp_header(L.UDP_RELAY_BOUND), d[:6]
    return d[10:18], d[18], d[19:27]


def quiet(sock, t=0.3):
    """Nothing arrives on sock for t seconds."""
    end = time.monotonic() + t
    try:
        while True:
            left = end - time.monotonic()
            if left <= 0:
                return True
            sock.settimeout(left)
            try:
                d = sock.recvfrom(2048)
            except socket.timeout:
                return True
            # The rendezvous's PROBE to a registered host is not the relay's.
            if d[0][:6] != L.udp_header(L.UDP_PROBE):
                raise AssertionError("unexpected datagram %r" % (d,))
    finally:
        sock.settimeout(2.0)


class RelayTests(Base):
    over = dict(udp_rate=1000.0, udp_burst=1000.0, probe_interval=1000.0)

    def setup_pair(self, joiner="alpha"):
        host, (a,) = self.room_with(joiner)
        hs, js = udp_sock(), udp_sock()
        to = ("127.0.0.1", self.srv.udp_port)
        hs.sendto(host.register(), to)
        recv_kind(hs, L.UDP_REGISTERED)
        js.sendto(a.register(), to)
        recv_kind(js, L.UDP_REGISTERED)
        recv_kind(hs, L.UDP_PEER)
        recv_kind(js, L.UDP_PEER)
        return host, a, hs, js, to

    def bind_both(self, host, a, hs, js, rid, port):
        rel = ("127.0.0.1", port)
        for c, s in ((host, hs), (a, js)):
            s.sendto(relay_bind(c, rid), rel)
            _rid, _flags, proof = parse_bound(recv_kind(s, L.UDP_RELAY_BOUND))
            s.sendto(relay_bind(c, rid, proof), rel)
            _rid, flags, _p = parse_bound(recv_kind(s, L.UDP_RELAY_BOUND))
        self.assertEqual(flags, 3)
        return rel

    def offer(self, host, a, hs, js, to):
        req = relay_request(a)
        js.sendto(req, to)
        d = recv_kind(js, L.UDP_RELAY_OFFER)
        self.assertLessEqual(len(d), len(req))      # no bigger than what asked
        jo = parse_offer(d)
        ho = parse_offer(recv_kind(hs, L.UDP_RELAY_OFFER))
        self.assertEqual(jo[3], "hostess")
        self.assertEqual(ho[3], "alpha")
        self.assertEqual(jo[1], ho[1])
        self.assertEqual(jo[2], ho[2])
        return jo[1], jo[2]

    def test_relay_forwards_between_the_bound_pair(self):
        host, a, hs, js, to = self.setup_pair()
        rid, port = self.offer(host, a, hs, js, to)
        rel = ("127.0.0.1", port)

        # Unbound, nothing is forwarded or answered.
        js.sendto(b"\x01early", rel)
        quiet(hs, 0.2)

        # A first BIND gets the proof, and is not yet an end the relay sends to.
        js.sendto(relay_bind(a, rid), rel)
        d = recv_kind(js, L.UDP_RELAY_BOUND)
        _rid, flags, proof = parse_bound(d)
        self.assertEqual(flags, 0)
        self.assertLess(len(d), L.UDP_RELAY_BIND_LEN)
        js.sendto(relay_bind(a, rid, proof), rel)
        self.assertEqual(parse_bound(recv_kind(js, L.UDP_RELAY_BOUND))[1], 2)
        hs.sendto(relay_bind(host, rid), rel)
        proof = parse_bound(recv_kind(hs, L.UDP_RELAY_BOUND))[2]
        hs.sendto(relay_bind(host, rid, proof), rel)
        self.assertEqual(parse_bound(recv_kind(hs, L.UDP_RELAY_BOUND))[1], 3)

        # Opaque datagrams, both ways, from the relay's port.
        js.sendto(b"\x07enet-from-joiner", rel)
        d, src = hs.recvfrom(2048)
        self.assertEqual((d, src), (b"\x07enet-from-joiner", rel))
        hs.sendto(b"\x08enet-from-host", rel)
        d, src = js.recvfrom(2048)
        self.assertEqual((d, src), (b"\x08enet-from-host", rel))
        # A punch packet (PDLB, outside the relay's types) goes through too.
        js.sendto(L.udp_header(0x20) + b"x" * 17, rel)
        self.assertEqual(hs.recvfrom(2048)[0][5], 0x20)

        # A stranger is neither forwarded nor answered, and a sniffed BIND
        # replayed from elsewhere is refused (stale seq).
        ss = udp_sock()
        ss.sendto(b"\x09stranger", rel)
        quiet(hs, 0.2)
        a.seq -= 1
        ss.sendto(relay_bind(a, rid, proof), rel)
        quiet(ss, 0.2)
        quiet(js, 0.1)
        # Too big for the game: dropped.
        js.sendto(b"\x01" * 1500, rel)
        quiet(hs, 0.2)
        ss.close()

        # A re-request returns the same relay.
        self.assertEqual(self.offer(host, a, hs, js, to), (rid, port))
        hs.close()
        js.close()

    def test_new_address_needs_its_proof(self):
        host, a, hs, js, to = self.setup_pair()
        rid, port = self.offer(host, a, hs, js, to)
        rel = self.bind_both(host, a, hs, js, rid, port)
        # The joiner's NAT moved: a signed BIND from a new socket makes that
        # the joiner's end, but nothing goes there before its proof comes back.
        js2 = udp_sock()
        js2.sendto(relay_bind(a, rid), rel)
        proof = parse_bound(recv_kind(js2, L.UDP_RELAY_BOUND))[2]
        hs.sendto(b"\x01to-joiner", rel)
        quiet(js2, 0.2)
        quiet(js, 0.1)
        # Another address's proof does not do.
        js2.sendto(relay_bind(a, rid, b"\x11" * 8), rel)
        self.assertEqual(parse_bound(recv_kind(js2, L.UDP_RELAY_BOUND))[1], 1)
        js2.sendto(relay_bind(a, rid, proof), rel)
        self.assertEqual(parse_bound(recv_kind(js2, L.UDP_RELAY_BOUND))[1], 3)
        hs.sendto(b"\x01to-joiner", rel)
        self.assertEqual(js2.recvfrom(2048)[0], b"\x01to-joiner")
        for s in (hs, js, js2):
            s.close()

    def test_request_refused(self):
        host, a, hs, js, to = self.setup_pair()
        # The host does not ask for a relay; unknown ids and bad macs get ERROR.
        hs.sendto(relay_request(host), to)
        self.assertEqual(recv_kind(hs, L.UDP_ERROR)[6], L.UDP_ERR_REFUSED)
        bad = bytearray(relay_request(a))
        bad[-1] ^= 1
        js.sendto(bytes(bad), to)
        self.assertEqual(recv_kind(js, L.UDP_ERROR)[6], L.UDP_ERR_REFUSED)
        self.assertEqual(self.srv.call(lambda: len(self.srv.lobby.relays)), 0)
        hs.close()
        js.close()

    def test_relay_closes_with_the_member(self):
        host, a, hs, js, to = self.setup_pair()
        rid, port = self.offer(host, a, hs, js, to)
        self.assertEqual(self.srv.call(lambda: len(self.srv.lobby.relays)), 1)
        st, _ = a.act("leave")
        self.assertEqual(st, 200)
        self.assertEqual(self.srv.call(lambda: len(self.srv.lobby.relays)), 0)
        hs.close()
        js.close()


class RelayCapTests(RelayTests):
    over = dict(udp_rate=1000.0, udp_burst=1000.0, probe_interval=1000.0, relay_room_pps=20.0, relay_burst=1.0,
                relay_max=1, relay_idle=0.6)

    def test_relay_forwards_between_the_bound_pair(self):
        pass

    def test_new_address_needs_its_proof(self):
        pass

    def test_caps(self):
        host, a, hs, js, to = self.setup_pair()
        rid, port = self.offer(host, a, hs, js, to)
        rel = self.bind_both(host, a, hs, js, rid, port)
        for i in range(200):
            js.sendto(b"\x01" + bytes([i]) * 100, rel)
        got = 0
        hs.settimeout(0.3)
        try:
            while True:
                hs.recvfrom(2048)
                got += 1
        except socket.timeout:
            pass
        # 20 a second with a one-second bucket: about 20 of 200 went.
        self.assertGreater(got, 5)
        self.assertLess(got, 40)
        self.assertGreater(self.srv.call(lambda: self.srv.lobby.relay_dropped), 150)

        # relay_max is 1: a second pair in another room is refused.
        host2 = self.client("hostess2")
        host2.create(name="Two")
        b = self.client("bravo")
        b.join(host2.room)
        h2, bs = udp_sock(), udp_sock()
        h2.sendto(host2.register(), to)
        recv_kind(h2, L.UDP_REGISTERED)
        bs.sendto(b.register(), to)
        recv_kind(bs, L.UDP_REGISTERED)
        bs.sendto(relay_request(b), to)
        self.assertEqual(recv_kind(bs, L.UDP_ERROR)[6], L.UDP_ERR_RELAY)
        for s in (hs, js, h2, bs):
            s.close()

    def test_idle_relay_reaped(self):
        host, a, hs, js, to = self.setup_pair()
        rid, port = self.offer(host, a, hs, js, to)
        rel = self.bind_both(host, a, hs, js, rid, port)
        time.sleep(1.2)
        self.assertEqual(self.srv.call(lambda: len(self.srv.lobby.relays)), 0)
        js.sendto(b"\x01after", rel)
        quiet(hs, 0.2)
        hs.close()
        js.close()


class NetinfoTests(Base):
    def test_netinfo_in_the_roster(self):
        host, (a,) = self.room_with("alpha")
        st, r = a.act("netinfo", {"path": "punch", "ping": 40})
        self.assertEqual(st, 200, r)
        v = r["version"]
        st, s = host.state()
        me = [m for m in s["members"] if m["user"] == "alpha"][0]
        self.assertEqual((me["path"], me["ping"]), ("punch", 40))
        # A small ping change does not wake everyone; a path change does.
        st, r = a.act("netinfo", {"path": "punch", "ping": 44})
        self.assertEqual(r["version"], v)
        st, r = a.act("netinfo", {"path": "relay", "ping": 44})
        self.assertGreater(r["version"], v)
        for bad in ({"path": "carrier pigeon"}, {"path": "lan", "ping": -1},
                    {"path": "lan", "ping": "12"}, {"path": "lan", "ping": True}):
            self.assertEqual(a.act("netinfo", bad)[0], 400)
        # The host has no path to itself.
        host.act("netinfo", {"path": "lan", "ping": 1})
        st, s = host.state()
        me = [m for m in s["members"] if m["host"]][0]
        self.assertIsNone(me["path"])


class UdpRateTests(Base):
    over = dict(udp_rate=1.0, udp_burst=5.0)

    def test_udp_rate_limit(self):
        s = udp_sock()
        for i in range(30):
            s.sendto(L.udp_header(L.UDP_ECHO) + struct.pack(">Q", i) + b"\0" * 26,
                     ("127.0.0.1", self.srv.udp_port))
        got = 0
        s.settimeout(0.5)
        try:
            while True:
                s.recvfrom(2048)
                got += 1
        except socket.timeout:
            pass
        self.assertGreaterEqual(got, 5)
        self.assertLessEqual(got, 7)
        s.close()


# ------------------------------------------------------------------ limits

class LimitTests(Base):
    over = dict(max_rooms=3, req_rate=2.0, req_burst=200.0)

    def test_body_cap(self):
        c = self.client("alpha")
        st, r = c.req("POST", "/rooms", raw=b"{" + b" " * (8 * 1024 + 10) + b"}")
        self.assertEqual(st, 413)

    def test_bad_bodies(self):
        c = self.client("alpha")
        self.assertEqual(c.req("POST", "/rooms", raw=b"not json")[0], 400)
        self.assertEqual(c.req("POST", "/rooms", raw=b"[1,2]")[0], 400)
        self.assertEqual(c.req("POST", "/rooms", raw=b"[" * 100000)[0], 413)
        self.assertEqual(c.req("GET", "/nothing")[0], 404)
        self.assertEqual(c.req("POST", "/rooms/zzz/join", {})[0], 404)

    def test_chunked_refused(self):
        sock = socket.create_connection(("127.0.0.1", self.srv.port))
        sock.sendall(b"POST /rooms HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")
        self.assertIn(b" 411 ", sock.recv(4096))
        sock.close()

    def test_header_cap(self):
        sock = socket.create_connection(("127.0.0.1", self.srv.port))
        sock.sendall(b"GET /ping HTTP/1.1\r\nX-Big: " + b"a" * 20000 + b"\r\n\r\n")
        self.assertIn(b" 431 ", sock.recv(4096))
        sock.close()

    def test_keepalive(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.srv.port, timeout=5)
        for _ in range(3):
            conn.request("GET", "/ping")
            r = conn.getresponse()
            self.assertEqual(r.status, 200)
            r.read()
        conn.close()

    def test_room_caps(self):
        hosts = []
        for i in range(3):
            h = self.client("host%d" % i, ip="10.1.0.%d" % (i + 1))
            self.assertEqual(h.create()[0], 200)
            hosts.append(h)
        h = self.client("host9", ip="10.1.0.9")
        st, r = h.create()
        self.assertEqual(st, 503)

    def test_request_bucket(self):
        self.srv.call(lambda: setattr(self.srv.cfg, "req_burst", 5.0))
        c = Client(self.srv.port, ip="10.7.7.7")
        codes = [c.req("GET", "/ping")[0] for _ in range(10)]
        self.assertEqual(codes[:5], [200] * 5)
        self.assertIn(429, codes[5:])
        # Another address is unaffected.
        self.assertEqual(Client(self.srv.port, ip="10.7.7.8").req("GET", "/ping")[0], 200)


class CreateRateTests(Base):
    over = dict(max_rooms_per_ip=2)

    def test_rooms_per_ip_and_create_window(self):
        a = self.client("alpha", ip="10.3.3.3")
        b = self.client("bravo", ip="10.3.3.3")
        c = self.client("charlie", ip="10.3.3.3")
        self.assertEqual(a.create()[0], 200)
        self.assertEqual(b.create()[0], 200)
        self.assertEqual(c.create()[0], 429)
        # Re-creating (which closes your own room first) is bounded by the window.
        codes = []
        for _ in range(6):
            codes.append(a.create()[0])
        self.assertIn(429, codes)

    def test_login_window(self):
        c = Client(self.srv.port, ip="10.4.4.4")
        codes = [c.login("alpha")[0] for _ in range(12)]
        self.assertEqual(codes[:10], [200] * 10)
        self.assertEqual(codes[10:], [429, 429])


# ------------------------------------------------------------------ review fixes

class SecurityTests(Base):
    """The holes a review found, each pinned so it stays shut."""

    def test_register_is_signed_not_a_token(self):
        host, (a,) = self.room_with("alpha")
        to = ("127.0.0.1", self.srv.udp_port)
        hs, spy = udp_sock(), udp_sock()
        d = host.register()
        # Nothing in the datagram is the member token...
        self.assertNotIn(bytes.fromhex(host.token), d)
        hs.sendto(d, to)
        recv_kind(hs, L.UDP_REGISTERED)
        # ...and a sniffed copy sent from elsewhere is refused (seq already
        # used), the registration staying where it was.
        spy.sendto(d, to)
        e = recv_kind(spy, L.UDP_ERROR)
        self.assertEqual(e[6], L.UDP_ERR_REFUSED)
        pub = self.srv.call(lambda: self.srv.lobby.rooms[host.room].host_member().udp_public)
        self.assertEqual(pub, hs.getsockname())
        # A forged seq with the old mac is refused too.
        forged = bytearray(d)
        struct.pack_into(">I", forged, 18, 99)
        spy.sendto(bytes(forged), to)
        self.assertEqual(recv_kind(spy, L.UDP_ERROR)[6], L.UDP_ERR_REFUSED)
        # The UDP key is no HTTP credential.
        st, r = host.req("POST", "/rooms/%s/leave" % host.room,
                         headers={"X-PD-Member": host.udp_key.hex()})
        self.assertEqual(st, 401)
        self.assertEqual(len(Client(self.srv.port).req("GET", "/rooms")[1]["rooms"]), 1)
        hs.close()
        spy.close()

    def test_rejoin_gets_a_new_udp_key(self):
        host, (a,) = self.room_with("alpha")
        old = a.udp_key
        st, r = a.join(host.room)
        self.assertEqual(st, 200, r)
        self.assertNotEqual(a.udp_key, old)
        s = udp_sock()
        s.sendto(register(a.room, a.udp_id, old, 5), ("127.0.0.1", self.srv.udp_port))
        self.assertEqual(recv_kind(s, L.UDP_ERROR)[6], L.UDP_ERR_REFUSED)
        s.sendto(a.register(), ("127.0.0.1", self.srv.udp_port))
        recv_kind(s, L.UDP_REGISTERED)
        s.close()

    def test_lan_endpoints_filtered(self):
        host, (a,) = self.room_with("alpha")
        to = ("127.0.0.1", self.srv.udp_port)
        hs, js = udp_sock(), udp_sock()
        hs.sendto(host.register(), to)
        recv_kind(hs, L.UDP_REGISTERED)
        js.sendto(a.register([("8.8.8.8", 53), ("127.0.0.1", 9), ("192.168.0.7", 27100),
                              ("fd00::1", 27100)]), to)
        hp = parse_peer(recv_kind(hs, L.UDP_PEER))
        self.assertEqual(hp[4], [js.getsockname(), ("192.168.0.7", 27100), ("fd00::1", 27100)])
        hs.close()
        js.close()

    def test_host_endpoints_own_address_only(self):
        host = self.client("hostess", ip="203.0.113.9")
        st, r = host.create(endpoints=["203.0.113.9:27100", "198.51.100.1:53", "10.0.0.2:27100"])
        self.assertEqual(st, 200, r)
        eps = self.srv.call(lambda: self.srv.lobby.rooms[host.room].endpoints)
        self.assertEqual(eps, ["203.0.113.9:27100", "10.0.0.2:27100"])

    def test_kicked_player_leaves_the_roster(self):
        host, (a, b) = self.room_with("alpha", "bravo")
        host.act("launch", {"force": True})
        st, s = a.state()
        ticket = s["you"]["ticket"]
        self.assertEqual(host.act("kick", {"user": "alpha"})[0], 200)
        st, hs = host.state()
        roster = {m["user"].lower() for m in hs["members"] if not m["host"]}
        self.assertEqual(roster, {"bravo"})
        ok, _u, why = L.verify_ticket(host.secret, ticket, host.room, now=hs["time"], roster=roster)
        self.assertEqual((ok, why), (False, "not in the room"))

    def test_settings_refused_leave_room_untouched(self):
        host, _ = self.room_with("alpha", "bravo")
        v = host.state()[1]["version"]
        st, r = host.act("settings", {"name": "RENAMED", "password": "pw", "max_humans": 2})
        self.assertEqual(st, 409)
        st, s = host.state()
        self.assertEqual((s["room"]["name"], s["room"]["locked"], s["version"]),
                         ("Test Room", False, v))

    def test_host_joining_elsewhere_tells_members_host_closed(self):
        h1, (a,) = self.room_with("alpha")
        h2 = self.client("hosttwo", ip="10.9.9.9")
        h2.create()
        hold = h1.token
        self.assertEqual(h1.join(h2.room)[0], 200)
        st, r = a.state()
        self.assertEqual(st, 410)
        self.assertIn("host closed", r["error"])
        st, r = h1.req("GET", "/rooms/%s/state" % a.room, headers={"X-PD-Member": hold})
        self.assertIn("another room", r["error"])

    def test_dead_token_is_410_after_gone_expires(self):
        host, (a,) = self.room_with("alpha")
        a.act("leave")
        self.srv.call(lambda: self.srv.lobby.gone.clear())
        st, r = a.state()
        self.assertEqual((st, r["reason"]), (410, "gone"))
        st, r = Client(self.srv.port).req("GET", "/rooms/%s/state" % host.room,
                                          headers={"X-PD-Member": "ab" * 16})
        self.assertEqual(st, 410)

    def test_registration_wakes_polls(self):
        host, (a,) = self.room_with("alpha")
        v = a.state()[1]["version"]
        out = {}
        th = threading.Thread(target=lambda: out.setdefault("r", a.state(since=v, wait=10)))
        th.start()
        time.sleep(0.2)
        hs = udp_sock()
        hs.sendto(host.register(), ("127.0.0.1", self.srv.udp_port))
        recv_kind(hs, L.UDP_REGISTERED)
        th.join(5)
        st, s = out["r"]
        self.assertEqual(st, 200)
        self.assertTrue([m for m in s["members"] if m["host"]][0]["udp"])
        # The same registration again changes nothing and wakes nobody.
        v = s["version"]
        hs.sendto(host.register(), ("127.0.0.1", self.srv.udp_port))
        recv_kind(hs, L.UDP_REGISTERED)
        self.assertEqual(a.state()[1]["version"], v)
        hs.close()


class PollLimitTests(Base):
    over = dict(polls_per_ip=2)

    def test_one_poll_per_member(self):
        host, (a,) = self.room_with("alpha")
        v = a.state()[1]["version"]
        out = {}

        def first():
            t = time.monotonic()
            out["r"] = a.state(since=v, wait=10)
            out["t"] = time.monotonic() - t
        th = threading.Thread(target=first)
        th.start()
        time.sleep(0.3)
        st, _s = a.state(since=v, wait=0.3)
        self.assertEqual(st, 200)
        th.join(5)
        self.assertEqual(out["r"][0], 200)
        self.assertLess(out["t"], 2.0)
        self.assertEqual(self.srv.call(lambda: self.srv.lobby.waiters), 0)
        self.assertEqual(self.srv.call(lambda: self.srv.lobby.ip_polls), {})

    def test_polls_per_address(self):
        host = self.client("hostess")
        host.create()
        js = []
        for n in ("alpha", "bravo", "charlie"):
            c = self.client(n, ip="10.5.5.5")
            self.assertEqual(c.join(host.room)[0], 200)
            js.append(c)
        v = js[0].state()[1]["version"]
        ths = [threading.Thread(target=c.state, args=(v, 2)) for c in js[:2]]
        for th in ths:
            th.start()
        time.sleep(0.3)
        st, r = js[2].state(since=v, wait=2)
        self.assertEqual(st, 429)
        # Polls at the cap never keep the host from being served.
        self.assertEqual(host.act("heartbeat")[0], 200)
        for th in ths:
            th.join(5)

    def test_polls_not_charged_to_the_address(self):
        host, (a,) = self.room_with("alpha")

        def tight():
            self.srv.cfg.req_rate = 0.01
            self.srv.cfg.req_burst = 3.0
        self.srv.call(tight)
        for _ in range(8):
            self.assertEqual(a.state()[0], 200)
        codes = [a.req("GET", "/ping")[0] for _ in range(4)]
        self.assertEqual(codes[:3], [200] * 3)
        self.assertEqual(codes[3], 429)


class LaunchedReapTests(Base):
    over = dict(host_timeout=0.6, member_timeout=0.5)

    def test_members_kept_through_a_match(self):
        host, (a, b) = self.room_with("alpha", "bravo")
        host.act("launch", {"force": True})
        for _ in range(4):
            time.sleep(0.3)
            host.act("heartbeat")
        st, s = host.state()
        self.assertEqual(s["room"]["state"], "launched")
        self.assertEqual(len(s["members"]), 3)
        # A crashed game comes back to its seat and a ticket.
        st, r = a.join(host.room)
        self.assertEqual(st, 200, r)
        self.assertIn("ticket", r)
        # Back in the lobby everyone has a fresh member_timeout.
        self.assertEqual(host.act("reopen")[0], 200)
        self.assertEqual(len(host.state()[1]["members"]), 3)


class UdpJunkTests(Base):
    over = dict(udp_sources_max=50)

    def test_limiter_table_is_bounded(self):
        lobby = self.srv.lobby

        def flood():
            for i in range(2000):
                lobby.udp_received(b"junk", ("198.18.%d.%d" % (i // 250, i % 250), 9))
            junk_entries = len(lobby.udp_buckets.b)
            for i in range(2000):
                # Well-formed header, too short to answer: costs an entry, no reply.
                lobby.udp_received(L.udp_header(L.UDP_PROBE_REPLY),
                                   ("198.19.%d.%d" % (i // 250, i % 250), 9))
            return junk_entries, len(lobby.udp_buckets.b)
        junk, valid = self.srv.call(flood)
        self.assertEqual(junk, 0)
        self.assertLessEqual(valid, 51)


# ------------------------------------------------------------------ ghost accounts

class StubGhost(BaseHTTPRequestHandler):
    seen = []

    def log_message(self, *a):
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        StubGhost.seen.append((body, self.headers.get("X-Real-IP")))
        if body.get("pin") == "1234":
            code, out = 200, {"ok": True, "recovery": True, "questions": 3}
        else:
            code, out = 403, {"ok": False, "error": "wrong username or pin"}
        data = json.dumps(out).encode()
        self.send_response(code)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class GhostAuthTests(unittest.TestCase):
    def setUp(self):
        self.stub = ThreadingHTTPServer(("127.0.0.1", 0), StubGhost)
        threading.Thread(target=self.stub.serve_forever, daemon=True).start()
        self.srv = LobbyThread(auth="ghost",
                               ghost_url="http://127.0.0.1:%d" % self.stub.server_address[1])
        StubGhost.seen = []

    def tearDown(self):
        self.srv.stop()
        if self.stub is not None:
            self.stub.shutdown()
            self.stub.server_close()

    def test_login_through_pdghostd(self):
        c = Client(self.srv.port, ip="198.51.100.7")
        st, r = c.login("alpha", "1234")
        self.assertEqual(st, 200, r)
        self.assertEqual(r["user"], "alpha")
        self.assertEqual(StubGhost.seen[-1], ({"username": "alpha", "pin": "1234"}, "198.51.100.7"))
        self.assertEqual(c.create()[0], 200)

        d = Client(self.srv.port, ip="198.51.100.8")
        st, r = d.login("alpha", "9999")
        self.assertEqual((st, r["error"]), (403, "wrong username or pin"))
        st, r = d.login("alpha")
        self.assertEqual(st, 403)
        self.assertEqual(len(StubGhost.seen), 2)  # a missing PIN never reaches pdghostd

    def test_account_server_down(self):
        self.stub.shutdown()
        self.stub.server_close()
        self.stub = None
        st, r = Client(self.srv.port).login("alpha", "1234")
        self.assertEqual(st, 502)
        self.assertEqual(r["error"], "account server unavailable")

    def test_logout(self):
        c = Client(self.srv.port)
        c.login("alpha", "1234")
        self.assertEqual(c.req("POST", "/logout")[0], 200)
        self.assertEqual(c.create()[0], 401)


if __name__ == "__main__":
    unittest.main(verbosity=2)
