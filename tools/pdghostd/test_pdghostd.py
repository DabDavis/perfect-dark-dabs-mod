#!/usr/bin/env python3
"""
Exercise a throwaway copy of pdghostd on a test port, the way the README says.

    python3 tools/pdghostd/test_pdghostd.py

Nothing but the standard library, no network beyond 127.0.0.1, and nothing
touched outside a temporary directory that is removed at the end.

The copy is patched the same way the README's sed does (PORT, ROOT), plus
BOARD_KEEP and USER_QUOTA lowered so eviction and the quota can be reached in
seconds rather than with a hundred accounts and forty megabytes.
"""

import gzip
import http.client
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "pdghostd.py")
PORT = int(os.environ.get("PDGHOSTD_TEST_PORT", "8392"))
# Everything the test makes goes under one directory that is removed at the
# end, so a run leaves nothing beside the server it tested.
WORK = tempfile.mkdtemp(prefix="pdghostd-test-")
ROOT = os.path.join(WORK, "root")
DAEMON = os.path.join(WORK, "testd.py")
LOG = os.path.join(WORK, "testd.log")
BOARD_KEEP = 3
USER_QUOTA = 4 * 1024 * 1024

passed = 0
failed = 0


def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
        print("  ok   " + what)
    else:
        failed += 1
        print("  FAIL " + what)


USER_SLOW_DELAY = 0.6


def build_daemon():
    src = open(SRC).read()

    def sub(old, new):
        nonlocal src
        assert src.count(old) == 1, old
        src = src.replace(old, new)

    sub("PORT = 8090", "PORT = %d" % PORT)
    sub('ROOT = os.path.expanduser("~/pdghosts")', "ROOT = %r" % ROOT)
    sub("BOARD_KEEP = 100", "BOARD_KEEP = %d" % BOARD_KEEP)
    sub("USER_QUOTA = 64 * 1024 * 1024", "USER_QUOTA = %d" % USER_QUOTA)
    sub("USER_SLOW_DELAY = 3.0", "USER_SLOW_DELAY = %r" % USER_SLOW_DELAY)
    open(DAEMON, "w").write(src)


_server = None
_logf = None


def start_server():
    global _server, _logf
    _logf = open(LOG, "ab")
    _server = subprocess.Popen([sys.executable, DAEMON], stdout=_logf, stderr=subprocess.STDOUT)
    for _ in range(100):
        time.sleep(0.05)
        if _server.poll() is not None:
            raise SystemExit("server exited at startup (port taken?); see " + LOG)
        try:
            st, body, _h = req("GET", "/ping")
            if st == 200:
                return
        except OSError:
            pass
    raise SystemExit("server did not come up; see " + LOG)


def stop_server():
    _server.terminate()
    _server.wait(10)
    _logf.close()


# ------------------------------------------------------------------ client

def req(method, path, body=None, headers=None, ip=None):
    """One request on a fresh connection. Returns (status, parsed body, headers)."""
    h = dict(headers or {})
    if ip:
        h["X-Forwarded-For"] = "1.2.3.4, " + ip
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=60)
    conn.request(method, path, body=body, headers=h)
    resp = conn.getresponse()
    data = resp.read()
    conn.close()
    try:
        parsed = json.loads(data)
    except ValueError:
        parsed = data
    return resp.status, parsed, dict(resp.getheaders())


def post_json(path, obj, ip=None):
    return req("POST", path, body=json.dumps(obj).encode(),
               headers={"Content-Type": "application/json"}, ip=ip)


def register(user, pin, ip=None):
    return post_json("/register", {"username": user, "pin": pin}, ip=ip)


def login(user, pin, ip=None):
    return post_json("/login", {"username": user, "pin": pin}, ip=ip)


def upload(user, pin, data, ip=None):
    return req("POST", "/upload", body=data, headers={
        "Content-Type": "application/octet-stream",
        "X-Ghost-User": user, "X-Ghost-Pin": pin}, ip=ip)


def board(stage, diff):
    st, body, _h = req("GET", "/leaderboard?stage=%d&diff=%d" % (stage, diff))
    assert st == 200, (st, body)
    return [(e["user"], e["time60"]) for e in body["entries"]]


def blobs():
    return sorted(n for n in os.listdir(os.path.join(ROOT, "blobs")) if not n.endswith(".tmp"))


def raw(payload, timeout=10):
    """Send bytes on a raw socket and return everything read until close."""
    s = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
    s.sendall(payload)
    out = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
    except socket.timeout:
        out += b"<<TIMEOUT>>"
    s.close()
    return out


# --------------------------------------------------------- ghost builder
#
# struct modghostheader and struct modghostsample from modghost.h, and the
# hash from modGhostHash() in modghost.c: FNV-1a 32 over the sample block.

def fnv1a(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def sample(i):
    return struct.pack("<fffHhbbbBBBh",
                       i * 1.5, 0.0, -i * 2.0,   # x y z
                       i & 0xFFFF, i % 200,      # theta, shootrotx
                       1, 0, 0,                  # speed forwards/sideways/theta
                       0, 1, 0,                  # crouchpos, weaponnum, flags
                       -i % 300)                 # shootroty


def ghost(owner, stage=0x30, diff=0, n=200, time60=None, version=2,
          headersize=128, samplesize=24, rate60=3, flags=1, hash_=None,
          samples=None, trailing=b""):
    if samples is None:
        samples = b"".join(sample(i) for i in range(n))
    if time60 is None:
        # As the recorder leaves it: (n-1)*rate <= time60 < n*rate.
        time60 = (n - 1) * rate60 + 1
    if hash_ is None:
        hash_ = fnv1a(samples)
    hdr = struct.pack("<8s6I4BII2BH32s32s16s",
                      b"PDGHOST\0", version, headersize, samplesize, n, time60, rate60,
                      stage, diff, 0, flags,
                      hash_, int(time.time()),
                      0, 0, 0,
                      b"player", b"build", owner.encode())
    assert len(hdr) == 128
    return hdr + samples + trailing


# ------------------------------------------------------------------ tests

def test_register_and_upload():
    print("register + valid upload")
    st, body, _ = register("tester", "1234")
    check(st == 200 and body["ok"], "register tester -> 200")
    st, body, _ = register("tester", "1234")
    check(st == 409, "register again -> 409")
    st, body, _ = register("bad name!", "1234")
    check(st == 400, "register bad name -> 400")

    data = ghost("tester")
    t0 = time.time()
    st, body, _ = upload("tester", "1234", data)
    check(st == 200 and body.get("stored") is True and body.get("rank") == 1,
          "upload valid ghost -> stored, rank 1 (%r)" % (body,))
    check(board(0x30, 0) == [("tester", 598)], "leaderboard shows it")
    names = blobs()
    check(len(names) == 1 and names[0].startswith("tester-") and names[0].endswith("-s48-d0.pdg.gz"),
          "one blob on disk: %s" % names)
    stored = gzip.decompress(open(os.path.join(ROOT, "blobs", names[0]), "rb").read())
    check(stored == data, "blob round-trips byte for byte")
    gid = req("GET", "/leaderboard?stage=48&diff=0")[1]["entries"][0]["id"]
    st, got, _ = req("GET", "/download?id=%d" % gid)
    check(st == 200 and got == data, "download returns the upload")

    # Version 1 is still a shape the client wrote.
    st, body, _ = upload("tester", "1234", ghost("tester", stage=0x33, version=1))
    check(st == 200 and body.get("stored") is True, "version 1 header accepted")
    # Both ends of the time60 tolerance.
    st, body, _ = upload("tester", "1234", ghost("tester", stage=0x22, n=50, time60=49 * 3))
    check(st == 200, "time60 == (n-1)*rate accepted")
    st, body, _ = upload("tester", "1234", ghost("tester", stage=0x2c, n=50, time60=51 * 3))
    check(st == 200, "time60 == (n+1)*rate accepted")


def test_forged_headers():
    print("forged headers")
    good = ghost("tester")
    cases = [
        ("bad hash", ghost("tester", hash_=fnv1a(good[128:]) ^ 1)),
        ("hash over header+samples instead of samples", ghost("tester", hash_=fnv1a(good))),
        ("edited sample under a stale hash", good[:200] + bytes([good[200] ^ 0xFF]) + good[201:]),
        ("samplesize 32", ghost("tester", samplesize=32,
                                samples=b"".join(sample(i) + b"\0" * 8 for i in range(200)))),
        ("samplesize 16", ghost("tester", samplesize=16)),
        ("headersize 256", ghost("tester", headersize=256)),
        ("time60 too large", ghost("tester", n=50, time60=51 * 3 + 1)),
        ("time60 too small", ghost("tester", n=50, time60=49 * 3 - 1)),
        ("time60 zero", ghost("tester", n=50, time60=0)),
        ("rate60 2", ghost("tester", rate60=2)),
        ("rate60 4", ghost("tester", rate60=4)),
        ("stage 0x26 Institute", ghost("tester", stage=0x26)),
        ("stage 0x1f MP Complex", ghost("tester", stage=0x1f)),
        ("stage 0x59 below title", ghost("tester", stage=0x59)),
        ("stage 0x5a title", ghost("tester", stage=0x5a)),
        ("stage 0xff", ghost("tester", stage=0xff)),
        ("difficulty 3", ghost("tester", diff=3)),
        ("version 0", ghost("tester", version=0)),
        ("version 3", ghost("tester", version=3)),
        ("trailing byte", ghost("tester", trailing=b"\0")),
        ("truncated", good[:-1]),
        ("one sample", ghost("tester", n=1)),
        ("65537 samples", ghost("tester", n=65537)),
        ("bad magic", b"PDGHOSX\0" + good[8:]),
    ]
    for what, data in cases:
        st, body, _ = upload("tester", "1234", data)
        check(st == 400, "%s -> 400 (got %d %r)" % (what, st, body))

    st, body, _ = upload("tester", "1234", ghost("someoneelse"))
    check(st == 403, "wrong owner -> 403")
    st, body, _ = upload("tester", "1234", ghost("tester", flags=0))
    check(st == 403, "no trial-rules flag -> 403")


_heat = [0]


def heat(user):
    """Eight wrong PINs at an account from two addresses nobody has used yet:
    an address that has hit its own limit is refused before the attempt can
    count against the account, so each round needs fresh ones."""
    for _ in range(2):
        _heat[0] += 1
        for _ in range(4):
            login(user, "0000", ip="10.0.2.%d" % _heat[0])


def test_lockout():
    print("hot accounts: slowed for strangers, not locked, not for known addresses")
    register("victim", "9999", ip="10.0.0.9")
    register("victim2", "9999", ip="10.0.0.9")

    msgs = set()
    for ip in ("10.0.0.1", "10.0.0.2"):
        for _ in range(4):
            st, body, _ = login("victim", "0000", ip=ip)
            msgs.add((st, body.get("error")))
    check(msgs == {(403, "wrong username or pin")},
          "8 wrong PINs over two addresses each refused as wrong (%r)" % msgs)

    t0 = time.time()
    st, body, _ = login("victim", "9999", ip="10.0.0.3")
    dt = time.time() - t0
    check(st == 200 and dt >= USER_SLOW_DELAY,
          "9th attempt, right PIN, new address -> accepted after the wait (%d, %.2fs)" % (st, dt))
    t0 = time.time()
    st, body, _ = login("victim", "9999", ip="10.0.0.3")
    check(st == 200 and time.time() - t0 < USER_SLOW_DELAY,
          "that address is now known: no wait (success also cooled the account)")

    heat("victim")
    t0 = time.time()
    st, body, _ = login("victim", "9999", ip="10.0.0.9")
    check(st == 200 and time.time() - t0 < USER_SLOW_DELAY,
          "hot again; the registering address signs in at once")
    heat("victim")
    t0 = time.time()
    st, body, _ = login("VICTIM", "0000", ip="10.0.0.6")
    dt = time.time() - t0
    check(st == 403 and body.get("error") == "wrong username or pin" and dt >= USER_SLOW_DELAY,
          "wrong PIN from a stranger while hot: still just wrong, after the wait (%.2fs)" % dt)
    t0 = time.time()
    st, body, _ = upload("victim", "9999", ghost("victim"), ip="10.0.0.9")
    check(st == 200 and time.time() - t0 < USER_SLOW_DELAY, "upload from a known address while hot -> at once")

    # Queue cap: many simultaneous strangers, at most USER_SLOW_WAITING wait,
    # the rest are told to come back - and the queue drains afterwards.
    heat("victim")
    results = []
    def guess(n):
        results.append(login("victim", "0000", ip="10.0.1.%d" % n)[1].get("error"))
    threads = [threading.Thread(target=guess, args=(n,)) for n in range(6)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    busy = sum(1 for e in results if e and "busy" in e)
    wrong = sum(1 for e in results if e == "wrong username or pin")
    check(busy >= 1 and wrong >= 2 and busy + wrong == 6,
          "6 at once: %d waited and were refused as wrong, %d told to come back" % (wrong, busy))
    st, body, _ = login("victim", "0000", ip="10.0.1.99")
    check(st == 403 and body.get("error") == "wrong username or pin", "queue drained afterwards")

    st1, b1, _ = login("nobody-here", "0000", ip="10.0.0.4")
    st2, b2, _ = login("victim2", "0000", ip="10.0.0.4")
    check((st1, b1) == (st2, b2) == (403, {"ok": False, "error": "wrong username or pin"}),
          "no-such-account and wrong-pin answers are identical")

    # Per-address limiter still there: 8 failures from one address on
    # different accounts locks the address.
    for i in range(8):
        login("nobody%d" % i, "0000", ip="10.0.0.7")
    st, body, _ = login("victim2", "9999", ip="10.0.0.7")
    check(st == 403 and "too many" in body.get("error", ""),
          "8 failures from one address -> address locked even with right PIN")
    st, body, _ = login("victim2", "9999", ip="10.0.0.8")
    check(st == 200, "victim2 still signs in from elsewhere (only 2 failures on the account)")


def test_bad_json():
    print("bad JSON bodies")
    for what, body in (("list", b"[1,2]"), ("number", b"123"), ("string", b'"x"'),
                       ("null", b"null"), ("deeply nested", b"[" * 200000),
                       ("not json", b"{{{")):
        st, resp, h = req("POST", "/login", body=body, headers={"Content-Type": "application/json"})
        check(st == 400 and resp == {"ok": False, "error": "bad body"},
              "login body %s -> 400 (%r)" % (what, resp))
    st, resp, _ = req("POST", "/register", body=b"[]")
    check(st == 400, "register body list -> 400")
    st, resp, _ = post_json("/register", {"username": 10 ** 40, "pin": [1]})
    check(st == 400, "register with non-string fields -> 400")
    st, resp, _ = post_json("/login", {"username": {"a": 1}, "pin": 12})
    check(st == 403 and resp["error"] == "wrong username or pin", "login with odd types -> refused")

    st, resp, _ = req("GET", "/leaderboard?stage=99999999999999999999999&diff=0")
    check(st == 400, "leaderboard stage past 64 bits -> 400")
    st, resp, _ = req("GET", "/leaderboard?stage=48&diff=0&limit=-99999999999999999999")
    check(st == 400, "leaderboard limit past 64 bits -> 400")
    st, resp, _ = req("GET", "/leaderboard?stage=48&diff=0&limit=9223372036854775807")
    check(st == 200, "leaderboard limit at 2^63-1 -> clamped, 200")
    st, resp, _ = req("GET", "/download?id=99999999999999999999999")
    check(st == 400, "download id past 64 bits -> 400")
    st, resp, _ = req("GET", "/download?id=1e3")
    check(st == 400, "download id not an int -> 400")


def test_quota():
    print("per-account quota (patched to %d MiB)" % (USER_QUOTA >> 20))
    register("hoarder", "1111")
    big = 65536  # MAX_SAMPLES: 128 + 65536*24 = 1572992 bytes
    t0 = time.time()
    st, body, _ = upload("hoarder", "1111", ghost("hoarder", stage=0x30, n=big))
    dt = time.time() - t0
    check(st == 200 and body.get("stored") is True, "1st max-size upload stored (%.2fs round trip)" % dt)
    st, body, _ = upload("hoarder", "1111", ghost("hoarder", stage=0x33, n=big))
    check(st == 200 and body.get("stored") is True, "2nd max-size upload stored")
    st, body, _ = upload("hoarder", "1111", ghost("hoarder", stage=0x22, n=big))
    check(st == 413 and "storage" in body.get("error", ""), "3rd would pass quota -> 413 (%r)" % body)
    check("hoarder" not in [u for u, t in board(0x22, 0)], "and nothing landed on that board")
    st, body, _ = upload("hoarder", "1111", ghost("hoarder", stage=0x30, n=big - 1))
    check(st == 200 and body.get("stored") is True,
          "replacing own row does not count the replaced bytes -> stored")
    st, body, _ = upload("hoarder", "1111", ghost("hoarder", stage=0x22, n=big))
    check(st == 413, "still over for a new board -> 413")
    st, body, _ = upload("hoarder", "1111", ghost("hoarder", stage=0x22, n=1000))
    check(st == 200 and body.get("stored") is True, "a small one on a new board fits")


def test_eviction():
    print("eviction keeps files (BOARD_KEEP patched to %d)" % BOARD_KEEP)
    S, D = 0x4f, 2
    for u in ("ev1", "ev2", "ev3", "ev4"):
        register(u, "2222")
    for u, n in (("ev1", 10), ("ev2", 20), ("ev3", 30)):
        st, body, _ = upload(u, "2222", ghost(u, stage=S, diff=D, n=n))
        check(st == 200 and body.get("stored") is True, "%s stored" % u)
    st, body, _ = upload("ev4", "2222", ghost("ev4", stage=S, diff=D, n=40))
    check(st == 200 and body.get("stored") is False and "outside" in body.get("reason", ""),
          "e4 slower than the board -> not stored")

    def user_blobs(u):
        return [n for n in blobs() if n.startswith(u + "-") and n.endswith("-s79-d2.pdg.gz")]

    st, body, _ = upload("ev4", "2222", ghost("ev4", stage=S, diff=D, n=5))
    check(st == 200 and body.get("stored") is True and body.get("rank") == 1, "e4 faster -> rank 1")
    check([u for u, t in board(S, D)] == ["ev4", "ev1", "ev2"], "board is e4 e1 e2")
    check(len(user_blobs("ev3")) == 1, "evicted e3's file is still on disk")

    st, body, _ = upload("ev3", "2222", ghost("ev3", stage=S, diff=D, n=35))
    check(st == 200 and body.get("stored") is False and "faster" in body.get("reason", ""),
          "e3 slower than its own evicted run -> not stored")
    st, body, _ = upload("ev3", "2222", ghost("ev3", stage=S, diff=D, n=4))
    check(st == 200 and body.get("stored") is True and body.get("rank") == 1, "e3 back on at rank 1")
    check([u for u, t in board(S, D)] == ["ev3", "ev4", "ev1"], "board is e3 e4 e1")
    check(len(user_blobs("ev2")) == 1, "evicted e2's file is still on disk")
    check(len(user_blobs("ev3")) == 1, "e3 overwrote its own file in place (one file)")

    st, body, _ = upload("ev1", "2222", ghost("ev1", stage=S, diff=D, n=8))
    check(st == 200 and body.get("stored") is True, "e1 beats own time")
    check(len(user_blobs("ev1")) == 1 and len(blobs()) == len(set(blobs())), "still one file per user")

    # Restarting runs the orphan sweep; evicted rows still reference their files.
    before = blobs()
    stop_server()
    start_server()
    check(blobs() == before, "startup sweep left the evicted files alone")
    check([u for u, t in board(S, D)] == ["ev3", "ev4", "ev1"], "board survives restart")

    import sqlite3
    conn = sqlite3.connect(os.path.join(ROOT, "ghosts.db"))
    ev = sorted(r[0] for r in conn.execute("SELECT username FROM ghosts WHERE evicted = 1"))
    conn.close()
    check(ev == ["ev2"], "evicted rows in the table: %r" % ev)


def test_legacy_shared_blob():
    print("legacy shared filename: dropped only when no row names it")
    import sqlite3
    S = 0x2d
    legacy = "legacy-s45-d0.pdg.gz"
    open(os.path.join(ROOT, "blobs", legacy), "wb").write(gzip.compress(b"old"))
    conn = sqlite3.connect(os.path.join(ROOT, "ghosts.db"))
    for u, t in (("tester", 1000), ("victim2", 1001)):
        conn.execute("INSERT INTO ghosts (username, stagenum, difficulty, time60, numsamples, bytes, "
                     "uploaded, blob, flags) VALUES (?,?,?,?,?,?,?,?,1)",
                     (u, S, 0, t, 2, 10, 0, legacy))
    conn.commit()
    conn.close()
    check(board(S, 0) == [("tester", 1000), ("victim2", 1001)], "two rows share the old file")

    st, body, _ = upload("tester", "1234", ghost("tester", stage=S, n=50))
    check(st == 200 and body.get("stored") is True, "tester moves to a new-style file")
    check(os.path.exists(os.path.join(ROOT, "blobs", legacy)),
          "old file kept: victim2's row still names it")
    st, body, _ = upload("victim2", "9999", ghost("victim2", stage=S, n=60))
    check(st == 200 and body.get("stored") is True, "victim2 moves too")
    check(not os.path.exists(os.path.join(ROOT, "blobs", legacy)),
          "old file dropped once no row names it")
    check(board(S, 0) == [("tester", 148), ("victim2", 178)], "board reads the new rows")


def test_early_close():
    print("early responses close the connection")
    hdr = ("POST /upload HTTP/1.1\r\nHost: x\r\nX-Forwarded-For: 10.9.9.1\r\n"
           "X-Ghost-User: tester\r\nX-Ghost-Pin: 0000\r\nContent-Length: 100\r\n\r\n")
    out = raw(hdr.encode(), timeout=5)
    check(out.startswith(b"HTTP/1.1 403") and b"connection: close" in out.lower() and b"<<TIMEOUT>>" not in out,
          "refused PIN with unread body -> 403 + Connection: close + socket closed")

    hdr = ("POST /upload HTTP/1.1\r\nHost: x\r\n"
           "X-Ghost-User: tester\r\nX-Ghost-Pin: 1234\r\nContent-Length: 3000000\r\n\r\n")
    out = raw(hdr.encode(), timeout=5)
    check(out.startswith(b"HTTP/1.1 413") and b"connection: close" in out.lower() and b"<<TIMEOUT>>" not in out,
          "oversize body -> 413 + Connection: close + socket closed")

    hdr = "POST /nowhere HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n"
    out = raw(hdr.encode(), timeout=5)
    check(out.startswith(b"HTTP/1.1 404") and b"connection: close" in out.lower(),
          "unknown POST with a body -> 404 + Connection: close")

    # And a request whose body was read keeps the connection open: two
    # pings on one connection.
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=10)
    conn.request("POST", "/login", body=b"[]")
    r1 = conn.getresponse(); r1.read()
    conn.request("GET", "/ping")
    r2 = conn.getresponse(); r2.read()
    conn.close()
    check(r1.status == 400 and r2.status == 200, "a request whose body was read keeps keep-alive")


def test_malformed_requests():
    print("malformed request lines are answered")
    out = raw(b"GET /" + b"a" * 70000 + b" HTTP/1.1\r\nHost: x\r\n\r\n", timeout=5)
    check(out.startswith(b"HTTP/1.1 414"), "70KB request line -> 414 written (%r)" % out[:40])
    # A request line with no version is answered HTTP/0.9 style by the
    # stdlib - body only, no status line - so what to check is that the
    # 400 body arrives and the socket closes rather than hanging or dying.
    out = raw(b"NOTHTTP\r\n\r\n", timeout=5)
    check(b"Error code: 400" in out and b"<<TIMEOUT>>" not in out,
          "junk request line -> 400 body written, socket closed")
    out = raw(b"GET /x HTTP/1.1 junk\r\nHost: x\r\n\r\n", timeout=5)
    check(b"400" in out[:200] and b"<<TIMEOUT>>" not in out,
          "bad request line -> 400 written (%r)" % out[:40])
    out = raw(b"GET / HTTP/1.1\r\n" + b"X-A: b\r\n" * 200 + b"\r\n", timeout=5)
    check(out.startswith(b"HTTP/1.1 431"), "200 headers -> 431 written (%r)" % out[:40])


def test_upload_not_auth_limited():
    print("uploads are not throttled by the sign-in limiter")
    data = ghost("tester")
    codes = set()
    for _ in range(35):
        st, body, _ = upload("tester", "1234", data)
        codes.add(st)
    check(codes == {200}, "35 uploads in a row all 200 (%r)" % codes)
    st, body, _ = login("tester", "1234")
    check(st == 200, "and the account still signs in (successes not counted)")
    st, body, _ = login("tester", "1234", ip="10.7.7.7")
    check(st == 200, "from another address too")


def main():
    build_daemon()
    start_server()
    try:
        test_register_and_upload()
        test_forged_headers()
        test_lockout()
        test_bad_json()
        test_quota()
        test_eviction()
        test_legacy_shared_blob()
        test_early_close()
        test_malformed_requests()
        test_upload_not_auth_limited()
    finally:
        stop_server()
    print("\n%d passed, %d failed" % (passed, failed))
    if failed:
        print("--- server log tail ---")
        print(open(LOG, "rb").read().decode("utf8", "replace")[-4000:])
    shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
