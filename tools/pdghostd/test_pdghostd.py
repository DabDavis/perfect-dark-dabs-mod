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
import hashlib
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
# Low enough that the full-directory answer can be reached with a handful of
# reports rather than five thousand.
CRASH_MAX_FILES = 6
REPORT_MAX_FILES = 4
# The server's own cap on the optional credit name, mirrored here.
REPORT_MAX_NAME = 64
# A page of the report board, and a thumbnail's width, small enough that a
# handful of reports and a picture of a few dozen pixels reach both.
BOARD_PAGE = 3
THUMB_WIDTH = 16

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
# The reset endpoint waits before it checks an answer, which is most of what
# makes guessing at one expensive. Twenty attempts at two seconds is not a test
# suite, so the copy under test waits a tenth of that.
RESET_DELAY = 0.2


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
    sub("RESET_DELAY = 2.0", "RESET_DELAY = %r" % RESET_DELAY)
    sub("CRASH_MAX_FILES = 5000", "CRASH_MAX_FILES = %d" % CRASH_MAX_FILES)
    sub("REPORT_MAX_FILES = 10000", "REPORT_MAX_FILES = %d" % REPORT_MAX_FILES)
    sub("BOARD_PAGE = 25", "BOARD_PAGE = %d" % BOARD_PAGE)
    sub("THUMB_WIDTH = 320", "THUMB_WIDTH = %d" % THUMB_WIDTH)
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


def register(user, pin, ip=None, question=None, answer=None, more=None):
    body = {"username": user, "pin": pin}
    if question is not None:
        body["question"] = question
    if answer is not None:
        body["answer"] = answer
    body.update(more or {})
    return post_json("/register", body, ip=ip)


def setrecovery(user, pin, question, answer, ip=None, more=None):
    body = {"username": user, "pin": pin, "question": question, "answer": answer}
    body.update(more or {})
    return post_json("/setrecovery", body, ip=ip)


def resetpin(user, question, answer, newpin, ip=None, more=None):
    body = {"username": user, "question": question, "answer": answer, "pin": newpin}
    body.update(more or {})
    return post_json("/resetpin", body, ip=ip)


def pairs23(q2, a2, q3, a3):
    """The second and third security questions, as the client sends them."""
    return {"question2": q2, "answer2": a2, "question3": q3, "answer3": a3}


def seed_old_schema():
    """A database from before an account could hold three questions.

    Written before the server starts, so that its migration runs over a row
    whose one-pair hash predates rec_count, and the tests below can ask
    whether that account still signs in and still resets by its one pair.
    """
    import sqlite3
    os.makedirs(ROOT, exist_ok=True)
    conn = sqlite3.connect(os.path.join(ROOT, "ghosts.db"))
    conn.executescript("""
        CREATE TABLE users (
            username   TEXT PRIMARY KEY COLLATE NOCASE,
            pin_salt   BLOB NOT NULL,
            pin_hash   BLOB NOT NULL,
            created    INTEGER NOT NULL,
            known_ips  TEXT NOT NULL DEFAULT '',
            rec_salt   BLOB NOT NULL DEFAULT x'',
            rec_hash   BLOB NOT NULL DEFAULT x''
        );
    """)
    pin_salt, rec_salt = b"p" * 16, b"r" * 16
    conn.execute("INSERT INTO users VALUES (?,?,?,?,?,?,?)", (
        "migrated", pin_salt, hashlib.pbkdf2_hmac("sha256", b"1234", pin_salt, 120000),
        1, "", rec_salt, hashlib.pbkdf2_hmac("sha256", b"game|tetris", rec_salt, 120000)))
    conn.commit()
    conn.close()


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


def test_recovery():
    print("resetting a PIN by answering the security question")

    st, body, _ = register("recov", "1234", ip="10.9.0.1", question="game", answer="goldeneye007")
    check(st == 200, "register with a question")
    check(body.get("recovery") is True, "and the reply says it took one")

    st, _, _ = login("recov", "1234", ip="10.9.0.1")
    check(st == 200, "and it signs in")

    st, body, _ = resetpin("recov", "game", "tetris", "5678", ip="10.9.0.2")
    check(st == 403 and body["error"] == "wrong question or answer", "wrong answer refused")

    st, body, _ = resetpin("recov", "food", "goldeneye007", "5678", ip="10.9.0.2")
    check(st == 403, "right answer under the wrong category refused")

    st, _, _ = login("recov", "1234", ip="10.9.0.1")
    check(st == 200, "and the PIN still works after both")

    st, body, _ = resetpin("recov", "game", "goldeneye007", "5678", ip="10.9.0.2")
    check(st == 200, "the right pair resets the PIN")
    check(body.get("recovery") is True, "and the question is still on the account")

    st, _, _ = login("recov", "1234", ip="10.9.0.1")
    check(st == 403, "the old PIN is gone")

    st, _, _ = login("recov", "5678", ip="10.9.0.1")
    check(st == 200, "the new PIN signs in")

    # The question is not the PIN: knowing it a second time is another reset,
    # not a way in.
    st, _, _ = login("recov", "0000", ip="10.9.0.1")
    check(st == 403, "and the answer is not itself a PIN")

    print("a sign-in says whether the account can be reset")

    st, body, _ = login("recov", "5678", ip="10.9.0.1")
    check(body.get("recovery") is True, "an account with a question says so")

    print("accounts made without a question")

    st, body, _ = register("oldreg", "1111", ip="10.9.0.3")
    check(st == 200, "a two field registration still works")
    check(body.get("recovery") is False, "and admits it has no question")

    st, body, _ = login("oldreg", "1111", ip="10.9.0.3")
    check(body.get("recovery") is False, "and one without says that instead")

    st, body, _ = resetpin("oldreg", "game", "goldeneye007", "2222", ip="10.9.0.3")
    check(st == 403 and body["error"] == "wrong question or answer",
          "no question set reads as a wrong answer")

    st, _, _ = setrecovery("oldreg", "9999", "drink", "coffee", ip="10.9.0.3")
    check(st == 403, "setting one needs the account's PIN")

    st, body, _ = setrecovery("oldreg", "1111", "drink", "coffee", ip="10.9.0.3")
    check(st == 200, "with the PIN it is set")
    check(body.get("recovery") is True, "and the reply says the nagging can stop")

    st, body, _ = login("oldreg", "1111", ip="10.9.0.3")
    check(body.get("recovery") is True, "as does the next sign-in")

    st, _, _ = resetpin("oldreg", "drink", "coffee", "2222", ip="10.9.0.4")
    check(st == 200, "and the reset works")

    st, _, _ = login("oldreg", "2222", ip="10.9.0.3")
    check(st == 200, "under the new PIN")

    # Changing it replaces it rather than adding to it.
    st, _, _ = setrecovery("oldreg", "2222", "drink", "beer", ip="10.9.0.3")
    check(st == 200, "the question can be changed")

    st, _, _ = resetpin("oldreg", "drink", "coffee", "3333", ip="10.9.0.4")
    check(st == 403, "the old answer stops working")

    print("a name that is not an account answers the same way")

    st, body, _ = resetpin("nosuchname", "game", "goldeneye007", "2222", ip="10.9.0.5")
    check(st == 403 and body["error"] == "wrong question or answer",
          "and says nothing about whether it exists")

    print("shapes that are not answers")

    st, _, _ = register("badq", "1234", ip="10.9.0.6", question="game", answer="")
    check(st == 400, "half a question is refused at registration")

    st, _, _ = register("badq", "1234", ip="10.9.0.6", question="G A M E", answer="x")
    check(st == 400, "and so is one that is not an id")

    st, _, _ = resetpin("recov", "game", "goldeneye007", "12", ip="10.9.0.7")
    check(st == 400, "a new PIN of two digits is refused")

    print("guessing at the question is what the limiter is for")

    # Five wrong answers at one account, each from an address of its own so
    # that it is the account's budget being spent and not one machine's.
    for i in range(5):
        st, _, _ = resetpin("recov", "game", "tetris", "9999", ip="10.9.1.%d" % i)
        check(st == 403, "wrong answer %d/5 refused" % (i + 1))

    st, body, _ = resetpin("recov", "game", "goldeneye007", "9999", ip="10.9.1.9")
    check(st == 429, "the sixth is refused before the answer is looked at")
    check(st == 429 and "tomorrow" in body["error"], "and says to come back tomorrow")

    st, _, _ = login("recov", "5678", ip="10.9.0.1")
    check(st == 200, "the owner can still sign in")

    st, _, _ = resetpin("recov", "game", "tetris", "9999", ip="10.9.1.9")
    check(st == 403, "and signing in cleared the day's budget")

    # One address, many accounts: the other half of the limiter.
    for i in range(10):
        resetpin("floodee%d" % i, "game", "tetris", "9999", ip="10.9.2.2")

    st, body, _ = resetpin("recov", "game", "goldeneye007", "9999", ip="10.9.2.2")
    check(st == 429 and "later" in body["error"], "ten attempts an hour from one address")

    st, _, _ = resetpin("recov", "game", "tetris", "9999", ip="10.9.2.3")
    check(st == 403, "which does not stop a different address")


def test_three_questions():
    print("three security questions")

    more = pairs23("food", "pizza", "drink", "coffee")

    st, body, _ = register("trio", "1234", ip="10.9.3.1", question="game", answer="goldeneye007",
                           more=more)
    check(st == 200 and body.get("questions") == 3, "register with three pairs, counted as three")

    st, body, _ = login("trio", "1234", ip="10.9.3.1")
    check(body.get("recovery") is True and body.get("questions") == 3, "a sign-in counts them too")

    st, body, _ = resetpin("trio", "game", "goldeneye007", "5678", ip="10.9.3.2")
    check(st == 403 and body["error"] == "wrong question or answer",
          "the first pair alone does not reset a three-question account")

    st, _, _ = resetpin("trio", "game", "goldeneye007", "5678", ip="10.9.3.2",
                        more=pairs23("food", "pizza", "drink", "tea"))
    check(st == 403, "nor do three with the third wrong")

    st, _, _ = resetpin("trio", "game", "goldeneye007", "5678", ip="10.9.3.2",
                        more=pairs23("drink", "coffee", "food", "pizza"))
    check(st == 403, "nor the right three in the wrong order")

    st, body, _ = resetpin("trio", "game", "goldeneye007", "5678", ip="10.9.3.2", more=more)
    check(st == 200 and body.get("questions") == 3, "all three right reset it")

    st, _, _ = login("trio", "5678", ip="10.9.3.1")
    check(st == 200, "under the new PIN")

    print("a one-question account answered by a three-question build")

    st, body, _ = register("uno", "1234", ip="10.9.3.3", question="game", answer="tetris")
    check(body.get("questions") == 1, "registered with one, counted as one")

    st, _, _ = resetpin("uno", "game", "tetris", "5678", ip="10.9.3.4",
                        more=pairs23("food", "pizza", "drink", "tea"))
    check(st == 200, "reset by its one, whatever the other two hold")

    st, body, _ = setrecovery("uno", "5678", "game", "tetris", ip="10.9.3.3", more=more)
    check(st == 200 and body.get("questions") == 3, "and Save To Account makes it three")

    st, body, _ = login("uno", "5678", ip="10.9.3.3")
    check(body.get("questions") == 3, "which the next sign-in reports")

    st, _, _ = resetpin("uno", "game", "tetris", "9999", ip="10.9.3.5")
    check(st == 403, "so its one no longer resets it")

    print("an account from before there were three")

    st, body, _ = login("migrated", "1234", ip="10.9.3.7")
    check(st == 200, "still signs in after the migration")
    check(body.get("recovery") is True and body.get("questions") == 1,
          "and is counted as holding one question")

    st, _, _ = resetpin("migrated", "game", "tetris", "4321", ip="10.9.3.8", more=more)
    check(st == 200, "and is reset by that one")

    st, _, _ = login("migrated", "4321", ip="10.9.3.7")
    check(st == 200, "under the new PIN")

    print("shapes that are not three questions")

    st, _, _ = register("gap", "1234", ip="10.9.3.6", question="game", answer="tetris",
                        more={"question3": "drink", "answer3": "tea"})
    check(st == 400, "a third pair with no second is refused")

    st, _, _ = register("gap", "1234", ip="10.9.3.6", question="game", answer="tetris",
                        more={"question2": "food"})
    check(st == 400, "and so is half a second pair")

    st, _, _ = setrecovery("trio", "5678", "", "", ip="10.9.3.1", more=more)
    check(st == 400, "and pairs two and three without one")



CRASH_DIR = os.path.join(ROOT, "crashes")


def crash_files():
    try:
        return sorted(os.listdir(CRASH_DIR))
    except OSError:
        return []


def send_crash(report, note="", version="abc1234", platform="x86_64-windows",
               channel="dev", ip=None):
    return post_json("/crash", {"report": report, "note": note, "version": version,
                                "platform": platform, "channel": channel}, ip=ip)


def test_crash_reports():
    print("crash reports")

    report = ("Dab's Mod crash report\nversion: dabs-mod abc1234\n\n"
              "--- crash ---\nEXCEPTION: 0xc0000005\nFAULT: read of 0x00000000000000a8\n")

    st, body, _ = send_crash(report, note="runway, sniping", ip="10.7.0.1")
    check(st == 200 and body.get("ok") and body.get("id", "").endswith(".txt"),
          "a report is taken -> 200")

    files = crash_files()
    check(len(files) == 1 and files[0] == body.get("id"), "and written under the id it answered")

    stored = open(os.path.join(CRASH_DIR, files[0])).read()
    check("from: 10.7.0.1" in stored and "version: abc1234" in stored
          and "note: runway, sniping" in stored,
          "the server's own header names the address, the build and the note")
    check("EXCEPTION: 0xc0000005" in stored, "and the report itself is underneath it")

    # A report is text somebody will cat. An escape sequence in one is a report
    # that can repaint their terminal.
    st, body, _ = send_crash(report + "\x1b[2J\x07 and a \x00 byte", ip="10.7.0.2")
    stored = open(os.path.join(CRASH_DIR, body["id"])).read()
    check(st == 200 and "\x1b" not in stored and "\x00" not in stored and "\x07" not in stored,
          "control characters are taken out")
    check("and a  byte" in stored, "and the text around them is kept")

    st, body, _ = send_crash("too short", ip="10.7.0.3")
    check(st == 400 and body.get("error") == "empty report", "something too short to be a crash -> 400")

    st, _, _ = post_json("/crash", {"report": 12345}, ip="10.7.0.3")
    check(st == 400, "a report that is not a string -> 400")

    st, _, _ = req("POST", "/crash", body=b"not json",
                   headers={"Content-Type": "application/json"}, ip="10.7.0.3")
    check(st == 400, "a body that is not JSON -> 400")

    st, body, _ = send_crash("x" * 200000, note="n" * 500, ip="10.7.0.4")
    stored = open(os.path.join(CRASH_DIR, body["id"])).read()
    check(st == 200 and "x" * (32 * 1024) in stored and "x" * (32 * 1024 + 1) not in stored,
          "an enormous report is cut to the cap")
    check("note: " + "n" * 200 + "\n" in stored, "and an enormous note to its own")

    st, _, _ = req("GET", "/crash", ip="10.7.0.5")
    check(st == 404, "there is no way to read one back out")

    # Twelve an hour from one address, and the directory cap behind that. The
    # copy under test holds six files, so the cap is what answers first here.
    seen = set()
    for i in range(8):
        st, _, _ = send_crash(report, ip="10.7.0.9")
        seen.add(st)
    check(507 in seen, "a full crash directory refuses rather than filling the disk")
    check(len(crash_files()) == CRASH_MAX_FILES, "and stops at the cap")

    for f in crash_files():
        os.remove(os.path.join(CRASH_DIR, f))

    # Twelve an hour from one address. The directory is emptied as they go,
    # because the cap the test copy runs with is six files and it is the
    # address limiter being measured here rather than the disk.
    taken = 0
    for i in range(12):
        st, _, _ = send_crash(report, ip="10.7.1.1")
        taken += st == 200
        for f in crash_files():
            os.remove(os.path.join(CRASH_DIR, f))
    check(taken == 12, "twelve reports in an hour from one address are taken")

    st, body, _ = send_crash(report, ip="10.7.1.1")
    check(st == 429 and "too many" in body.get("error", ""),
          "the thirteenth from one address in an hour -> 429")

    st, _, _ = send_crash(report, ip="10.7.1.2")
    check(st == 200, "and another address is unaffected")

    for f in crash_files():
        os.remove(os.path.join(CRASH_DIR, f))


REPORT_DIR = os.path.join(ROOT, "reports")


def report_files():
    try:
        return sorted(os.listdir(REPORT_DIR))
    except OSError:
        return []


def clear_reports():
    for f in report_files():
        os.remove(os.path.join(REPORT_DIR, f))


def tiny_png():
    import zlib

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    raw = b"\x00\xff\x00\x00" * 2
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 2, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def send_report(report, note="", screenshot=None, ip=None, raw=None, name=None):
    import base64
    body = {"report": report, "note": note, "version": "abc1234",
            "platform": "x86_64-linux", "channel": "dev"}
    if name is not None:
        body["name"] = name
    if screenshot is not None:
        body["screenshot"] = base64.b64encode(screenshot).decode()
    if raw is not None:
        body.update(raw)
    return post_json("/report", body, ip=ip)


def test_problem_reports():
    print("problem reports")

    report = ("Dab's Mod problem report\nversion: dabs-mod abc1234\n\n"
              "--- trace ---\npd trace 2026-09-17 12:00:00\nstage 0x09 lvframenum 3000\n")
    png = tiny_png()

    st, body, _ = send_report(report, note="guard's gun is floating\nnext to him", screenshot=png,
                              ip="10.6.0.1")
    check(st == 200 and body.get("ok") and body.get("id"), "a report with a picture is taken -> 200")
    base = body.get("id", "")
    check(report_files() == [base + ".png", base + ".txt"], "and written as a .txt and a .png")
    check(open(os.path.join(REPORT_DIR, base + ".png"), "rb").read() == png,
          "the picture is the bytes that were sent")
    stored = open(os.path.join(REPORT_DIR, base + ".txt")).read()
    check("from: 10.6.0.1" in stored and "screenshot: %s.png" % base in stored
          and "note: guard's gun is floating next to him\n" in stored,
          "the header names the address, the picture and the note on one line")
    check("stage 0x09 lvframenum 3000" in stored, "and the dump is underneath")
    clear_reports()

    st, body, _ = send_report(report, ip="10.6.0.2")
    check(st == 200 and report_files() == [body["id"] + ".txt"], "a report without a picture is a .txt alone")
    stored = open(os.path.join(REPORT_DIR, body["id"] + ".txt")).read()
    check("screenshot: -" in stored, "and says so")
    # Every client built before the name field sends no name at all.
    check("name: -" in stored, "a report with no name field is taken and says so")
    clear_reports()

    st, body, _ = send_report(report, name="Velvet Dark", ip="10.6.0.2")
    check(st == 200 and "name: Velvet Dark\n"
          in open(os.path.join(REPORT_DIR, body["id"] + ".txt")).read(),
          "a name is written into the header, for the credits")
    clear_reports()

    st, body, _ = send_report(report, name=" \n  two\nlines " + "z" * 200, ip="10.6.0.2")
    stored = open(os.path.join(REPORT_DIR, body["id"] + ".txt")).read()
    check(st == 200 and "name: two lines " + "z" * (REPORT_MAX_NAME - len(" \n  two\nlines ")) + "\n" in stored,
          "a name is one line, trimmed, and cut to its cap")
    clear_reports()

    st, body, _ = send_report(report, raw={"name": 12345}, ip="10.6.0.2")
    check(st == 400 and body.get("error") == "bad body", "a name that is not a string -> 400")
    clear_reports()

    st, body, _ = send_report(report, screenshot=b"GIF89a not a png", ip="10.6.0.3")
    check(st == 400 and body.get("error") == "bad screenshot" and not report_files(),
          "a picture that is not a PNG -> 400, nothing written")

    st, body, _ = send_report(report, raw={"screenshot": "!!!not base64!!!"}, ip="10.6.0.3")
    check(st == 400 and body.get("error") == "bad screenshot", "a picture that is not base64 -> 400")

    st, body, _ = send_report("short", ip="10.6.0.3")
    check(st == 400 and body.get("error") == "empty report", "an empty report -> 400")

    st, body, _ = send_report(report + "\x1b[2J", note="n" * 5000, ip="10.6.0.4")
    stored = open(os.path.join(REPORT_DIR, body["id"] + ".txt")).read()
    check(st == 200 and "\x1b" not in stored and "note: " + "n" * 1000 + "\n" in stored,
          "control characters out, the note cut to its cap")
    clear_reports()

    # Bigger than the ghost limit, which is the point of the route's own limit.
    big = "x" * (3 * 1024 * 1024)
    st, body, _ = send_report(report + big, screenshot=png, ip="10.6.0.5")
    stored = open(os.path.join(REPORT_DIR, body["id"] + ".txt")).read() if st == 200 else ""
    check(st == 200 and len(stored) < 520 * 1024, "a body past 2MB is taken, and its text cut to the cap")
    clear_reports()

    st, _, _ = req("GET", "/report", ip="10.6.0.5")
    check(st == 404, "there is no way to read one back out")

    seen = set()
    for i in range(6):
        st, _, _ = send_report(report, screenshot=png, ip="10.6.0.9")
        seen.add(st)
    check(507 in seen and len([f for f in report_files() if f.endswith(".txt")]) == REPORT_MAX_FILES,
          "a full report directory refuses at its cap, counting reports rather than files")
    clear_reports()

    taken = 0
    for i in range(30):
        st, _, _ = send_report(report, ip="10.6.1.1")
        taken += st == 200
        clear_reports()
    check(taken == 30, "thirty reports in an hour from one address are taken")
    st, body, _ = send_report(report, ip="10.6.1.1")
    check(st == 429, "the thirty-first -> 429")
    clear_reports()


# ------------------------------------------------------------ report board

STATUS_FILE = os.path.join(REPORT_DIR, "status.txt")
THUMB_DIR = os.path.join(ROOT, "reportthumbs")
ARCHIVE_DIR = os.path.join(ROOT, "reports-archive")
# What a report carries that the board must never show: the address it came
# from, and the dump, settings, log and paths underneath the header.
SECRETS = ("10.99.88.77", "10.66.55.44", "SECRET-DUMP-LINE", "Mod.SecretSetting",
           "/home/tester", "from:")


def make_png(w, h, filters=(0,)):
    """An 8-bit RGB PNG of a gradient, its rows filtered by turns with `filters`.

    The same picture comes out whatever the filters, which is what lets the
    thumbnailer's unfiltering be checked against the plain one.
    """
    import zlib

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    rows = [bytes(((x * 7 + y * 3 + c * 50) & 0xff) for x in range(w) for c in range(3))
            for y in range(h)]
    raw = bytearray()
    prev = bytes(w * 3)
    for y, row in enumerate(rows):
        f = filters[y % len(filters)]
        out = bytearray()
        for i, v in enumerate(row):
            left = row[i - 3] if i >= 3 else 0
            up = prev[i]
            ul = prev[i - 3] if i >= 3 else 0
            if f == 0:
                pred = 0
            elif f == 1:
                pred = left
            elif f == 2:
                pred = up
            elif f == 3:
                pred = (left + up) >> 1
            else:
                p = left + up - ul
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - ul)
                pred = left if pa <= pb and pa <= pc else (up if pb <= pc else ul)
            out.append((v - pred) & 0xff)
        raw += bytes([f]) + out
        prev = row
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def put_report(base, name="-", note="-", png=None, version="abc1234",
               platform="x86_64-windows", ip="10.99.88.77", header_name=True):
    """A report written straight to disk the way the /report route writes one,
    which is the only way to have one with a stamp from months ago."""
    os.makedirs(REPORT_DIR, exist_ok=True)
    if png is not None:
        with open(os.path.join(REPORT_DIR, base + ".png"), "wb") as f:
            f.write(png)
    with open(os.path.join(REPORT_DIR, base + ".txt"), "w") as f:
        f.write("received: %s-%s-%s %s:%s:%s\n" % (
            base[0:4], base[4:6], base[6:8], base[9:11], base[11:13], base[13:15]))
        f.write("from: %s\nversion: %s\nplatform: %s\nchannel: dev\n" % (ip, version, platform))
        f.write("screenshot: %s\n" % (base + ".png" if png is not None else "-"))
        if header_name:
            f.write("name: %s\n" % name)
        f.write("note: %s\n\n" % note)
        f.write("Dab's Mod problem report\nSECRET-DUMP-LINE\n[Mod]\nMod.SecretSetting=1\n"
                "path: /home/tester/pd\n")


def put_status(text):
    with open(STATUS_FILE, "w") as f:
        f.write(text)
    # The board notices a change by mtime and size; a rewrite within the same
    # tick of the clock at the same size would be missed, which a person
    # editing by hand never does and a test can.
    time.sleep(0.02)


def board_json(query=""):
    st, body, _ = req("GET", "/board.json" + query)
    return st, body


def board_page(query=""):
    st, body, headers = req("GET", "/board" + query)
    return st, body.decode("utf8") if isinstance(body, bytes) else str(body), headers


def clear_board():
    clear_reports()
    for d in (THUMB_DIR, ARCHIVE_DIR):
        shutil.rmtree(d, ignore_errors=True)


def test_report_board():
    print("report board")
    clear_board()

    # One through the real route, so that what it writes is what is read.
    st, body, _ = send_report("Dab's Mod problem report\nSECRET-DUMP-LINE\n" + "x" * 64,
                              note="posted <b>live</b>", name="Poster", screenshot=tiny_png(),
                              ip="10.66.55.44")
    posted = body.get("id", "")
    check(st == 200, "a report posted to /report ...")

    shot = make_png(64, 32)
    put_report("20260920-100000-aaaaaaa1", name="Alice", note="first", png=shot)
    put_report("20260920-110000-aaaaaaa2", name="-", note="-")
    put_report("20260921-100000-aaaaaaa3", header_name=False,
               note="<script>alert(1)</script> & \"quotes\" 'too'")
    put_report("20260921-110000-aaaaaaa4", name="<i>Bob</i>", note="second", png=shot,
               platform="x86_64-linux")
    put_report("20260922-100000-aaaaaaa5", name="alice", note="third")
    put_report("20260922-110000-aaaaaaa6", name="Carol", note="hide me", png=shot)

    st, page, headers = board_page()
    check(st == 200 and headers.get("Content-Type", "").startswith("text/html"),
          "... and GET /board is a page")
    check("posted &lt;b&gt;live&lt;/b&gt;" in page and "Poster" in page,
          "which shows it, its note escaped")
    check(not any(x in page for x in SECRETS),
          "no address, dump, setting or path anywhere on the page")
    st, js = board_json("?page=1")
    check(st == 200 and not any(x in json.dumps(js) for x in SECRETS),
          "nor in board.json")
    st, one, _ = board_page("?id=20260921-100000-aaaaaaa3")
    check("<script>alert(1)</script>" not in one
          and "&lt;script&gt;alert(1)&lt;/script&gt; &amp; &quot;quotes&quot; &#x27;too&#x27;" in one,
          "a note's markup is escaped, quotes included")
    check("<i>Bob</i>" not in page and "&lt;i&gt;Bob&lt;/i&gt;" in page, "and so is a name")
    check("script-src" not in headers.get("Content-Security-Policy", "x")
          and "default-src 'none'" in headers.get("Content-Security-Policy", ""),
          "and the page is served with a policy that runs no script")

    # No status file: everything is received.
    check(not os.path.exists(STATUS_FILE), "with no status file ...")
    st, js = board_json()
    check(js.get("total") == 7 and all(r["status"] == "received" for r in js["reports"]),
          "... every report is received")
    check(js["reports"][0]["id"] == "20260922-110000-aaaaaaa6" or js["reports"][0]["id"] == posted,
          "newest first")

    names = {r["id"]: r["name"] for r in board_json("?page=3")[1]["reports"]}
    names.update({r["id"]: r["name"] for r in board_json("?page=2")[1]["reports"]})
    check(names.get("20260920-110000-aaaaaaa2") == "anonymous"
          and names.get("20260921-100000-aaaaaaa3") == "anonymous",
          'a name of "-", or no name line at all, is "anonymous"')

    # Pagination: seven reports at three a page.
    st, js = board_json("?page=2")
    check(js.get("page") == 2 and js.get("pages") == 3 and len(js["reports"]) == 3,
          "three to a page, three pages")
    check(board_json("?page=99")[1].get("page") == 3, "a page past the end is the last")
    check(board_json("?page=nope")[1].get("page") == 1, "a page that is not a number is the first")
    st, page, _ = board_page("?page=2")
    check("Page 2 of 3" in page and "Newer" in page and "Older" in page,
          "the page links both ways from the middle")

    # The status file.
    put_status(
        "# a comment line\n"
        "\n"
        "20260920-100000-aaaaaaa1 fixed 2026-09-24 Fixed in abc1234 - the thing works\n"
        "20260921-110000-aaaaaaa4 fixed 2026-09-24 Fixed in abc1234 - the thing works\n"
        "20260922-100000-aaaaaaa5 duplicate 20260920-100000-aaaaaaa1 - same thing\n"
        "20260921-100000 needinfo which level? <b>bold</b>\n"
        "20260920-110000-aaaaaaa2 bogusword not a status\n"
        "not-an-id fixed nothing\n"
        "20260922-110000-aaaaaaa6 hidden\n"
        "%s working first\n"
        "%s working second, later lines win\n" % (posted, posted))
    st, js = board_json()
    by_id = {}
    for n in (1, 2, 3):
        for r in board_json("?page=%d" % n)[1]["reports"]:
            by_id[r["id"]] = r
    check(by_id["20260920-100000-aaaaaaa1"]["status"] == "fixed"
          and by_id["20260920-100000-aaaaaaa1"]["comment"] == "Fixed in abc1234 - the thing works",
          "a status line sets the word and the comment, the date apart")
    check(by_id["20260921-100000-aaaaaaa3"]["status"] == "needinfo",
          "a stamp alone names the report sent that second")
    check(by_id["20260920-110000-aaaaaaa2"]["status"] == "received",
          "a line with an unknown word is skipped")
    check(by_id[posted]["comment"] == "second, later lines win", "a later line replaces an earlier one")
    check("20260922-110000-aaaaaaa6" not in by_id and js.get("total") == 6,
          "a hidden report is off the board ...")
    st, _, _ = req("GET", "/board/shot/20260922-110000-aaaaaaa6.png")
    check(st == 404, "... picture and all")
    st, page, _ = board_page("?status=needinfo")
    check("which level? &lt;b&gt;bold&lt;/b&gt;" in page, "a status comment is escaped too")

    # Filters.
    st, js = board_json("?name=ALICE")
    check(sorted(r["id"] for r in js["reports"]) ==
          ["20260920-100000-aaaaaaa1", "20260922-100000-aaaaaaa5"],
          "the tester filter takes a name whatever its capitals")
    st, js = board_json("?status=fixed")
    check(js.get("total") == 2, "the status filter")
    check(board_json("?status=hidden")[1].get("total") == 6, "and hidden is not one it takes")
    st, js = board_json("?id=20260921-100000-aaaaaaa3")
    check(js.get("total") == 1, "one report by its id")

    # The Fixed column: one change, two reports and a duplicate, one entry.
    fixes = board_json()[1].get("fixes", [])
    check(len(fixes) == 1 and fixes[0]["names"] == ["Alice", "<i>Bob</i>"]
          and len(fixes[0]["reports"]) == 3,
          "reports fixed by one change are one fix, crediting each name once")
    put_status(open(STATUS_FILE).read()
               + "20260921-110000-aaaaaaa4 fixed 2026-09-25 Fixed in def5678 - another thing\n")
    fixes = board_json()[1].get("fixes", [])
    check([f["date"] for f in fixes] == ["2026-09-25", "2026-09-24"], "newest fix first")
    st, page, _ = board_page()
    check('id="fixes"' in page and "Thanks Alice" in page and "build def5678" in page,
          "and the page has the column")

    # A status for a report that is not there changes nothing and breaks nothing.
    put_status(open(STATUS_FILE).read() + "20250101-000000-deadbeef fixed 2025-01-01 gone\n")
    st, js = board_json()
    check(st == 200 and js.get("total") == 6 and len(js["fixes"]) == 2,
          "a status line for a report that does not exist is ignored")

    # The patch notes: a fix that names a line of them is that line.
    with open(os.path.join(REPORT_DIR, "patchnotes.txt"), "w") as f:
        f.write("# header, and anything above the first notes line, is skipped\n"
                "not a fix\n\n"
                "notes 2 2026-09-27\n"
                "Somewhere: a newer fix that nobody reported\n\n"
                "notes 1 2026-09-26\n"
                "Area: the first fix (thanks Zed)\n"
                "Area: the <second> fix\n")
    put_status("20260920-100000-aaaaaaa1 fixed 1.2 Fixed in abc1234\n"
               "20260921-110000-aaaaaaa4 fixed 1.2 Fixed in abc1234\n"
               "20260922-100000-aaaaaaa5 fixed 1.1 A comment of its own\n"
               "20260921-100000-aaaaaaa3 fixed 9.9 Fixed in abc1234 - a line the notes lack\n")
    st, js = board_json("?id=20260920-100000-aaaaaaa1")
    check(js["reports"][0]["comment"] == "Fixed in abc1234 - Area: the <second> fix",
          "a card whose fix is a patch notes line says that line")
    fixes = js.get("fixes", [])
    check([(f["notes"], f["date"]) for f in fixes][:3]
          == [("2.1", "2026-09-27"), ("1.1", "2026-09-26"), ("1.2", "2026-09-26")],
          "every patch notes line is a fix, newest batch first and in the notes' order")
    by_ref = {f["notes"]: f for f in fixes}
    check(by_ref["1.1"]["names"] == ["Zed"] and by_ref["1.2"]["names"] == ["Alice", "<i>Bob</i>"]
          and by_ref["2.1"]["reports"] == [],
          "credited by the notes' thanks, else by the reports' senders")
    check(by_ref["1.2"]["comment"] == "Fixed in abc1234 - Area: the <second> fix"
          and len(by_ref["1.2"]["reports"]) == 2,
          "and one line fixed for two reports is one fix with the build they agree on")
    check(any(f["notes"] is None and f["date"] == "" and "the notes lack" in f["comment"] for f in fixes),
          "a line the notes do not have is a fix in the status file's own words")
    st, page, _ = board_page()
    check("Area: the &lt;second&gt; fix" in page and "Thanks Zed" in page
          and "a newer fix that nobody reported" in page,
          "and the page shows them, escaped")
    os.remove(os.path.join(REPORT_DIR, "patchnotes.txt"))

    # Pictures, and everything that is not one.
    st, body, headers = req("GET", "/board/shot/20260920-100000-aaaaaaa1.png")
    check(st == 200 and body == shot and headers.get("Content-Type") == "image/png",
          "a report's picture is served as it was sent")
    st, body, _ = req("GET", "/board/thumb/20260920-100000-aaaaaaa1.png")
    check(st == 200 and isinstance(body, bytes) and body[16:24] == struct.pack(">II", 16, 8),
          "its thumbnail is the picture shrunk to the thumbnail width")
    check(os.path.exists(os.path.join(THUMB_DIR, "20260920-100000-aaaaaaa1.png")),
          "and is kept for next time")
    st, body, _ = req("GET", "/board/thumb/%s.png" % posted)
    check(st == 200 and body == tiny_png(), "a picture already small is its own thumbnail")
    for path in ("/board/shot/..%2F..%2Fghosts.db", "/board/shot/../../ghosts.db",
                 "/board/shot/../reports/status.txt", "/board/shot/20260920-100000-aaaaaaa1.txt",
                 "/board/shot/20260920-100000-AAAAAAA1.png", "/board/shot/20260920-100000-aaaaaaa1",
                 "/board/shot/20260920-110000-aaaaaaa2.png", "/board/thumb/status.png",
                 "/board/shot/%2e%2e/ghosts.png", "/board/other"):
        st, _, _ = req("GET", path)
        check(st == 404, "%s -> 404" % path)

    st, _, headers = req("GET", "/board/?status=fixed")
    check(st == 301 and headers.get("Location") == "../board?status=fixed",
          "a trailing slash is sent back to where the relative links work")

    # The status file is not a report, for the directory's cap.
    clear_board()
    put_status("# nothing\n")
    taken = sum(send_report("Dab's Mod problem report\n" + "y" * 64, ip="10.6.2.%d" % i)[0] == 200
                for i in range(REPORT_MAX_FILES))
    check(taken == REPORT_MAX_FILES, "the status file does not count against the report cap")
    clear_board()


def run_archive(*args):
    r = subprocess.run([sys.executable, DAEMON, "--archive"] + list(args),
                       capture_output=True, text=True, timeout=120)
    return r.returncode, r.stdout + r.stderr


def tar_members(path):
    import tarfile
    with tarfile.open(path, "r:xz") as t:
        return {m.name: t.extractfile(m).read() for m in t}


def test_report_archive():
    print("report archive")
    clear_board()

    new = time.strftime("%Y%m%d-%H%M%S") + "-bbbbbbb1"
    shot = make_png(32, 16)
    put_report("20200115-120000-aaaaaaa1", name="Oldie", note="old one", png=shot)
    put_report("20200120-120000-aaaaaaa2", name="-", note="old two")
    put_report("20200210-120000-aaaaaaa3", name="Febby", note="february", png=shot)
    put_report(new, name="Newbie", note="new one", png=shot)
    put_status("20200115-120000-aaaaaaa1 fixed 2020-03-01 Fixed in 1111111 - old fix\n"
               "20200210-120000-aaaaaaa3 working later\n")
    with open(os.path.join(REPORT_DIR, "patchnotes.txt"), "w") as f:
        f.write("notes 1 2020-03-01\nSomething (thanks Someone)\n")
    req("GET", "/board/thumb/20200115-120000-aaaaaaa1.png")
    before = {n: open(os.path.join(REPORT_DIR, n), "rb").read() for n in report_files()}

    code, out = run_archive("--dry-run")
    check(code == 0 and report_files() == sorted(before) and not os.path.exists(ARCHIVE_DIR),
          "a dry run moves nothing")

    code, out = run_archive()
    check(code == 0, "the archive runs")
    check(report_files() == sorted([new + ".png", new + ".txt", "status.txt", "patchnotes.txt"]),
          "old pairs leave the reports directory; new ones, status and patch notes stay")
    jan = tar_members(os.path.join(ARCHIVE_DIR, "2020-01.tar.xz"))
    feb = tar_members(os.path.join(ARCHIVE_DIR, "2020-02.tar.xz"))
    check(sorted(jan) == ["20200115-120000-aaaaaaa1.png", "20200115-120000-aaaaaaa1.txt",
                          "20200120-120000-aaaaaaa2.txt"]
          and sorted(feb) == ["20200210-120000-aaaaaaa3.png", "20200210-120000-aaaaaaa3.txt"],
          "one tar per month of stamps")
    check(all(before[n] == data for n, data in list(jan.items()) + list(feb.items())),
          "every file in them byte for byte")
    check(not os.path.exists(os.path.join(THUMB_DIR, "20200115-120000-aaaaaaa1.png")),
          "an archived report's thumbnail goes with it")
    index = open(os.path.join(ARCHIVE_DIR, "index.txt")).read()
    check("20200115-120000-aaaaaaa1\t2020-01-15 12:00:00\tOldie\n" in index
          and "10.99.88.77" not in index and "SECRET" not in index,
          "the index keeps the id, time and name, and nothing private")

    # The board after: the archived report is gone from the list, its fix is not.
    st, js = board_json()
    check(js.get("total") == 1 and js["reports"][0]["id"] == new,
          "archived reports drop off the board")
    fixes = [f for f in js.get("fixes", []) if f["notes"] is None]
    check(len(fixes) == 1 and fixes[0]["names"] == ["Oldie"]
          and fixes[0]["archived"] == ["20200115-120000-aaaaaaa1"],
          "a fix whose report was archived still credits its sender")
    st, page, _ = board_page()
    check(st == 200 and "old fix" in page and "Thanks Oldie" in page
          and "#r-20200115-120000-aaaaaaa1" not in page,
          "and shows on the page without a link to a report that is not there")
    st, _, _ = req("GET", "/board/shot/20200115-120000-aaaaaaa1.png")
    check(st == 404, "nor its picture")

    # Again, with one more January report: the month's tar gains it.
    put_report("20200125-120000-aaaaaaa4", name="Late", note="late january")
    code, out = run_archive()
    jan2 = tar_members(os.path.join(ARCHIVE_DIR, "2020-01.tar.xz"))
    check(code == 0 and sorted(jan2) == sorted(list(jan) + ["20200125-120000-aaaaaaa4.txt"])
          and all(jan2[n] == jan[n] for n in jan),
          "a second run adds to a month's tar and loses nothing in it")

    # A month whose tar cannot be read back: nothing deleted, nothing replaced.
    put_report("20200305-120000-aaaaaaa5", name="March", note="march", png=shot)
    with open(os.path.join(ARCHIVE_DIR, "2020-03.tar.xz"), "wb") as f:
        f.write(b"this is not a tar")
    code, out = run_archive()
    check(code == 1 and os.path.exists(os.path.join(REPORT_DIR, "20200305-120000-aaaaaaa5.txt"))
          and os.path.exists(os.path.join(REPORT_DIR, "20200305-120000-aaaaaaa5.png")),
          "a month that fails keeps its originals")
    check(open(os.path.join(ARCHIVE_DIR, "2020-03.tar.xz"), "rb").read() == b"this is not a tar"
          and not os.path.exists(os.path.join(ARCHIVE_DIR, "2020-03.tar.xz.tmp")),
          "and its old tar, with no temporary left behind")

    code, out = run_archive("--days", "100000")
    check(code == 0 and "nothing older" in out, "--days moves the cutoff")
    clear_board()
    try:
        os.remove(STATUS_FILE)
    except OSError:
        pass


def main():
    build_daemon()
    seed_old_schema()
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
        test_recovery()
        test_three_questions()
        test_crash_reports()
        test_problem_reports()
        test_report_board()
        test_report_archive()
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
