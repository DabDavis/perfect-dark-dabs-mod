#!/usr/bin/env python3
"""
Ghost server for Dab's Mod - accounts, ghost storage and leaderboards.

Runs behind nginx on 127.0.0.1, which terminates TLS and forwards
https://texturepacks.art/pdghosts/ here. Nothing in here should be exposed
directly: it trusts the proxy for the client address and speaks plain HTTP.

Stdlib only - no Flask, no ORM. The box already runs several services and this
one should not bring a dependency tree with it.

Times are accepted as submitted. That is a deliberate choice and not an
oversight: validating a run properly means replaying it against the stage
geometry, which is a different project. What is enforced here is the shape of
the data, so that a malformed or enormous upload cannot cost disk or CPU.
"""

import contextlib
import gzip
import hashlib
import hmac
import json
import os
import re
import sqlite3
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"
PORT = 8090
ROOT = os.path.expanduser("~/pdghosts")
DB_PATH = os.path.join(ROOT, "ghosts.db")
BLOB_DIR = os.path.join(ROOT, "blobs")

# A fifty minute run is about 1.5MB. Eight is generous and still bounded.
MAX_BODY = 8 * 1024 * 1024
MAX_SAMPLES = 65536
HEADER_SIZE = 128
MAGIC = b"PDGHOST\0"

# Fifteen characters, because a name has to fit the owner field a ghost carries
# (MODGHOST_OWNERLEN in the client's modghost.h, sixteen bytes with the
# terminator). A name that did not fit would be truncated into the file and
# then match the wrong account. Only registration is checked, so an account
# made under the older limit can still sign in.
USERNAME_RE = re.compile(r"^[A-Za-z0-9_.-]{3,15}$")
PIN_RE = re.compile(r"^[0-9]{4,8}$")

# A PIN is four digits. Ten thousand guesses is nothing without a limiter, so
# the limiter is the actual security control here rather than the PIN length.
AUTH_WINDOW = 300
AUTH_MAX_FAILURES = 8

# Every check of a PIN costs a PBKDF2, which is the point for a stored secret
# and a cost for a server. The failure budget above does not bound that work,
# because a success clears it - so anyone holding one valid account could ask
# for it as often as they liked. This budget counts attempts rather than
# failures and is never cleared, so it is the ceiling on how much hashing an
# address can buy. Thirty in five minutes is far above signing in and far
# below being useful as a way to spend somebody else's processor.
AUTH_ATTEMPT_WINDOW = 300
AUTH_ATTEMPT_MAX = 30
UPLOAD_WINDOW = 3600

# One press of Upload sends what the client thinks is worth sending, which is
# its best run per level rather than everything it kept. Sixty an hour was
# written when a player had one ghost per stage and no reason to re-send it.
UPLOAD_MAX = 120

# Downloading a board's worth of ghosts is a legitimate afternoon; downloading
# the same one two hundred times an hour is not something the client does.
DOWNLOAD_WINDOW = 3600
DOWNLOAD_MAX = 200

# The header flag that says a run was set with the fork's added moves off -
# MODGHOSTHF_TRIALRULES in the client's modghost.h. A run without it was made
# under unknown rules, which is not the same as fair rules: everything recorded
# before the flag existed had jump and the combat roll available, and there is
# no asking now which were on. A board that mixes them ranks settings.
GHOST_TRIALRULES = 0x01

# The second flag, MODGHOSTHF_NOCHEATS, says the game's own cheats were off as
# well. It arrived later than the first, so a run can carry TRIALRULES without
# it: that one was set by a build which held the fork's moves off and let
# Invincible through. Nothing here refuses those - the rows on the boards today
# are all of that generation and there is no evidence any was cheated - but the
# flags column carries the whole byte, so the distinction is on the server for
# the day it is wanted.
GHOST_NOCHEATS = 0x02

# How many rows a board keeps, and so how many blobs a stage and difficulty
# can cost. One row per player is the other half of that bound: the client
# stores every attempt because a run cannot be judged while it is being set,
# but the board only wants the answer, and a hundred players' answers is a
# leaderboard where a hundred of one player's afternoons is a log.
BOARD_KEEP = 100

_lock = threading.Lock()
_auth_failures = {}
_auth_attempts = {}
_upload_counts = {}
_download_counts = {}


def db():
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row
    return conn


@contextlib.contextmanager
def db_write(on_rollback=None):
    """A connection whose reads and writes are one transaction.

    BEGIN IMMEDIATE takes the write lock before the first SELECT rather than at
    the first INSERT, which is what makes a decision taken from a read still
    true when it is acted on. The upload path reads the board to decide whether
    a run makes it and then writes on the answer, and without this two uploads
    arriving together could both read a board with one place left and both take
    it - a board of a hundred and one, with the hundred and first invisible to
    every reader that asks for a hundred.

    isolation_level=None turns off the driver's own transaction handling so the
    BEGIN below is the only one, and commit and rollback are explicit.

    on_rollback is for the things a rollback cannot undo - a blob already
    written for a row that then failed to commit.
    """
    conn = sqlite3.connect(DB_PATH, timeout=10, isolation_level=None)
    conn.row_factory = sqlite3.Row

    try:
        conn.execute("BEGIN IMMEDIATE")
        yield conn
    except BaseException:
        conn.execute("ROLLBACK")

        # Whatever was put on disk in the expectation that this would commit.
        # The database undoes itself; files do not.
        if on_rollback is not None:
            on_rollback()

        raise
    else:
        conn.execute("COMMIT")
    finally:
        conn.close()


def blob_name(username, stagenum, difficulty):
    """What one player's run on one level is called on disk.

    The readable part is the username with anything awkward folded to an
    underscore, and that fold is not injective: usernames may contain _ . and
    -, and all three land on _, so dab.2, dab-2 and dab_2 are three accounts
    that were being handed one filename. The second to upload overwrote the
    first, and the first's row went on pointing at it - so a board served one
    player's ghost under another player's name.

    The tag is what actually tells them apart. It is over the lowercased name
    because usernames are unique case insensitively, so Dab and dab are one
    account and must not be two files.

    Rows written under the old scheme point at the old name and keep working:
    /download reads the name out of the row, and the upload path already drops
    a file the row has moved away from.
    """
    canon = username.lower()
    safe = re.sub(r"[^a-z0-9]", "_", canon)
    tag = hashlib.sha256(canon.encode()).hexdigest()[:8]
    return "%s-%s-s%02d-d%d.pdg.gz" % (safe, tag, stagenum, difficulty)


def blob_write(path, data):
    """Store a ghost compressed, atomically.

    Runs are twenty samples a second of position and angle, which is the kind
    of repetitive binary that deflate is good at - a real recording lands
    around forty percent of its original size. Nothing about the ghost itself
    changes: this is the same bytes, stored smaller, and what the client is
    handed back on download is the file it uploaded. Sample rate is what a
    ghost's quality is made of and is not something to trade for disk.
    """
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(gzip.compress(data, 6))
    os.replace(tmp, path)


def blob_read(path):
    """Read a ghost, compressed or not.

    Blobs written before compression are still on disk and still served, so
    what decides is the file rather than a flag in the database: two bytes at
    the front say whether this one needs inflating.
    """
    with open(path, "rb") as f:
        data = f.read()

    if data[:2] == b"\x1f\x8b":
        return gzip.decompress(data)

    return data


def sweep_orphan_blobs():
    """Delete blob files no row points at.

    Rows are what the board is made of and files are what the disk holds, and
    the two part company when rows are deleted by a migration rather than by
    the upload path that would have taken their files with them. Run at
    startup, which is the only moment nothing is being written.
    """
    try:
        names = os.listdir(BLOB_DIR)
    except OSError:
        return

    with db() as conn:
        keep = {r["blob"] for r in conn.execute("SELECT blob FROM ghosts")}

    for name in names:
        if name in keep or name.endswith(".tmp"):
            continue

        try:
            os.remove(os.path.join(BLOB_DIR, name))
            print("swept orphan blob %s" % name, flush=True)
        except OSError:
            pass


def init_db():
    os.makedirs(BLOB_DIR, exist_ok=True)
    with db() as conn:
        conn.executescript("""
            CREATE TABLE IF NOT EXISTS users (
                username   TEXT PRIMARY KEY COLLATE NOCASE,
                pin_salt   BLOB NOT NULL,
                pin_hash   BLOB NOT NULL,
                created    INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS ghosts (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                username   TEXT NOT NULL COLLATE NOCASE,
                stagenum   INTEGER NOT NULL,
                difficulty INTEGER NOT NULL,
                time60     INTEGER NOT NULL,
                numsamples INTEGER NOT NULL,
                bytes      INTEGER NOT NULL,
                uploaded   INTEGER NOT NULL,
                blob       TEXT NOT NULL,
                flags      INTEGER NOT NULL DEFAULT 0,
                mpbody     INTEGER NOT NULL DEFAULT 0,
                mphead     INTEGER NOT NULL DEFAULT 0
            );
            CREATE INDEX IF NOT EXISTS ghosts_board
                ON ghosts(stagenum, difficulty, time60);
        """)

        # One row per player per level. This briefly allowed a row per
        # distinct time instead, so the duplicates that policy may have left
        # are collapsed to each player's best before the index that forbids
        # them goes back on - creating it against duplicate rows would fail
        # and take the service down on a restart, which is a bad way to find
        # out. Their blobs are collected afterwards by the sweep below.
        # The column is added rather than assumed, because a database made
        # before the flag existed has every other column already and CREATE
        # TABLE IF NOT EXISTS will not go back and fill one in.
        have = {r[1] for r in conn.execute("PRAGMA table_info(ghosts)")}

        if "flags" not in have:
            conn.execute("ALTER TABLE ghosts ADD COLUMN flags INTEGER NOT NULL DEFAULT 0")

        # Rows stored before the board carried a character default to zero,
        # which is the same "nobody in particular" the client reads out of a
        # run recorded before the picker existed. They correct themselves the
        # next time that player uploads.
        for col in ("mpbody", "mphead"):
            if col not in have:
                conn.execute("ALTER TABLE ghosts ADD COLUMN %s INTEGER NOT NULL DEFAULT 0" % col)

        # Rows that cannot show they were set under trial rules go, with their
        # files. Uploads are refused on the same test, so this runs once in
        # practice and finds nothing afterwards - but it is what makes the
        # board's promise true of the rows already in it rather than only of
        # the next ones.
        stale = conn.execute(
            "SELECT COUNT(*) FROM ghosts WHERE flags & ? = 0",
            (GHOST_TRIALRULES,)).fetchone()[0]

        if stale:
            print("dropping %d run(s) not recorded under trial rules" % stale, flush=True)
            conn.execute("DELETE FROM ghosts WHERE flags & ? = 0", (GHOST_TRIALRULES,))

        conn.execute("DROP INDEX IF EXISTS ghosts_one_per_time")
        conn.execute("""
            DELETE FROM ghosts WHERE id NOT IN (
                SELECT id FROM (
                    SELECT id, ROW_NUMBER() OVER (
                        PARTITION BY username, stagenum, difficulty
                        ORDER BY time60 ASC, id ASC) AS rn
                    FROM ghosts
                ) WHERE rn = 1
            )
        """)
        conn.execute("""
            CREATE UNIQUE INDEX IF NOT EXISTS ghosts_one_per_user
                ON ghosts(username, stagenum, difficulty)
        """)

    sweep_orphan_blobs()


def hash_pin(pin, salt):
    return hashlib.pbkdf2_hmac("sha256", pin.encode(), salt, 120000)


# Keys live in these tables until something clears them, and one key is one
# address. Past this many the expired ones are swept, which is cheap and
# bounded and stops a stream of addresses from growing the process forever.
RATE_TABLE_MAX = 4096


def rate_ok(table, key, window, limit):
    now = time.time()
    with _lock:
        if len(table) > RATE_TABLE_MAX:
            for dead in [k for k, v in table.items() if not v or now - v[-1] >= window]:
                del table[dead]

        hits = [t for t in table.get(key, []) if now - t < window]
        if len(hits) >= limit:
            table[key] = hits
            return False
        hits.append(now)
        table[key] = hits
        return True


def parse_ghost_header(data):
    """Read a .pdg header, or return None if it is not one.

    Every field used to size or index anything is checked before it is
    believed, because this arrived over the internet.
    """
    if len(data) < HEADER_SIZE:
        return None

    if data[:8] != MAGIC:
        return None

    version, headersize, samplesize, numsamples, time60, rate60 = struct.unpack_from("<6I", data, 8)
    stagenum, difficulty, stageindex, flags = struct.unpack_from("<4B", data, 0x20)
    # The Combat Simulator body and head the run was set as, plus one so that
    # zero can mean "whoever Joanna comes with" - MODGHOST_BODY_DEFAULT in the
    # client's modghost.h. Stored so a board can say who a time belongs to
    # rather than only what they were called.
    mpbody, mphead = struct.unpack_from("<2B", data, 0x2c)
    player = data[0x30:0x50].split(b"\0")[0].decode("ascii", "replace")
    owner = data[0x70:0x80].split(b"\0")[0].decode("ascii", "replace")

    if version < 1 or version > 16:
        return None
    if headersize < HEADER_SIZE or samplesize < 16 or samplesize > 1024:
        return None
    if numsamples < 2 or numsamples > MAX_SAMPLES:
        return None
    if rate60 < 1 or rate60 > 60:
        return None
    if time60 < 1 or time60 > 0xFFFFFF:
        return None
    if difficulty > 3:
        return None
    # The engine's own test for "is this a real level" is stagenum < 0x5a, and
    # nothing above it can be played, so nothing above it can be run. Without
    # this a byte's worth of invented stage numbers is a byte's worth of
    # separate boards, each willing to hold its hundred files.
    if stagenum >= 0x5a:
        return None
    if len(data) < headersize + numsamples * samplesize:
        return None

    return {
        "version": version, "numsamples": numsamples, "time60": time60,
        "rate60": rate60, "stagenum": stagenum, "difficulty": difficulty,
        "player": player, "owner": owner, "flags": flags,
        "mpbody": mpbody, "mphead": mphead,
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "pdghostd/1.0"
    protocol_version = "HTTP/1.1"
    # A thread per connection with no timeout is a thread a client can hold
    # open by sending a Content-Length and then nothing. nginx buffers a
    # request body before it forwards it, so this should never fire in
    # production - it is what keeps that true rather than assumed.
    timeout = 30

    def log_message(self, fmt, *args):
        print("%s - %s" % (self.client_ip(), fmt % args), flush=True)

    def log_error(self, fmt, *args):
        # An idle keep-alive connection hitting the timeout above is how a
        # connection ends, not a fault, and logging one per client would fill
        # the journal with the service working correctly.
        msg = fmt % args
        if msg.startswith("Request timed out"):
            return
        self.log_message("%s", msg)

    def client_ip(self):
        """Who is asking, as far as the rate limiter is concerned.

        X-Real-IP first, because the nginx in front of this sets it from
        $remote_addr and a client cannot influence it.

        X-Forwarded-For is read from the RIGHT. $proxy_add_x_forwarded_for
        appends the peer address to whatever the client sent, so the first
        element is whatever the client wanted it to be and the last is the one
        our own proxy wrote. Taking the first meant every guess could carry a
        fresh invented address, which is the whole rate limiter - and the rate
        limiter, not the four digits, is what actually guards a PIN.

        Both headers are only trustworthy because nothing reaches this except
        through the proxy. Exposing it directly hands the limiter back to the
        client; see the note at the top of this file.
        """
        real = self.headers.get("X-Real-IP", "").strip()
        if real:
            return real

        fwd = self.headers.get("X-Forwarded-For", "")
        if fwd:
            return fwd.rsplit(",", 1)[-1].strip()

        return self.client_address[0]

    def send_json(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_blob(self, data, filename):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Disposition", 'attachment; filename="%s"' % filename)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def read_body(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return None
        if length <= 0 or length > MAX_BODY:
            return None
        return self.rfile.read(length)

    def path_parts(self):
        path = self.path.split("?", 1)
        query = {}
        if len(path) > 1:
            for pair in path[1].split("&"):
                if "=" in pair:
                    k, v = pair.split("=", 1)
                    query[k] = v
        return path[0].rstrip("/"), query

    def drop_blobs(self, names):
        """Remove blob files that no row references, ignoring what is missing."""
        for name in names:
            with db() as conn:
                still = conn.execute(
                    "SELECT 1 FROM ghosts WHERE blob = ? LIMIT 1", (name,)).fetchone()

            if still is not None:
                continue

            try:
                os.remove(os.path.join(BLOB_DIR, name))
            except OSError:
                pass

    def authenticate(self, username, pin):
        """Check a username and PIN. Returns (ok, error)."""
        if not rate_ok(_auth_failures, self.client_ip(), AUTH_WINDOW, AUTH_MAX_FAILURES):
            return False, "too many attempts, wait a few minutes"

        if not username or not pin:
            return False, "username and pin required"

        # Counted before the hash below, which is the expensive part.
        if not rate_ok(_auth_attempts, self.client_ip(), AUTH_ATTEMPT_WINDOW, AUTH_ATTEMPT_MAX):
            return False, "too many attempts, wait a few minutes"

        with db() as conn:
            row = conn.execute("SELECT * FROM users WHERE username = ?", (username,)).fetchone()

        if row is None:
            return False, "no such account"

        expect = bytes(row["pin_hash"])
        actual = hash_pin(pin, bytes(row["pin_salt"]))

        if not hmac.compare_digest(expect, actual):
            return False, "wrong pin"

        # A success clears the failure budget for this address.
        with _lock:
            _auth_failures.pop(self.client_ip(), None)

        return True, None

    # ---------------------------------------------------------------- routes

    def do_GET(self):
        path, query = self.path_parts()

        if path in ("", "/ping"):
            return self.send_json(200, {"ok": True, "service": "pdghostd", "version": 1})

        if path == "/leaderboard":
            try:
                stage = int(query.get("stage", "-1"))
                diff = int(query.get("diff", "-1"))
                # Clamped at both ends: SQLite reads a negative LIMIT as no
                # limit at all, so an unclamped one served the whole board.
                limit = max(1, min(int(query.get("limit", "100")), 100))
            except ValueError:
                return self.send_json(400, {"ok": False, "error": "bad query"})

            with db() as conn:
                # The id breaks ties so that the rows served are the rows the
                # upload path decided to keep, in the same order. Two runs at
                # the same time60 are two places on the board and the earlier
                # upload takes the higher one.
                rows = conn.execute(
                    "SELECT id, username, time60, uploaded, flags, mpbody, mphead FROM ghosts "
                    "WHERE stagenum = ? AND difficulty = ? "
                    "ORDER BY time60 ASC, id ASC LIMIT ?",
                    (stage, diff, limit)).fetchall()

            # trialrules is sent even though nothing without it can be stored,
            # so a client can mark a row rather than having to assume. If this
            # policy is ever loosened to grandfather old runs, the boards will
            # already be able to say which are which.
            return self.send_json(200, {"ok": True, "entries": [
                {"id": r["id"], "user": r["username"], "time60": r["time60"],
                 "uploaded": r["uploaded"],
                 "trialrules": 1 if r["flags"] & GHOST_TRIALRULES else 0,
                 "mpbody": r["mpbody"], "mphead": r["mphead"]} for r in rows]})

        if path == "/download":
            # The only endpoint that hands back megabytes, and the only one
            # with nothing else bounding how often it may be asked.
            if not rate_ok(_download_counts, self.client_ip(), DOWNLOAD_WINDOW, DOWNLOAD_MAX):
                return self.send_json(429, {"ok": False, "error": "too many downloads, wait a while"})

            try:
                ghost_id = int(query.get("id", "-1"))
            except ValueError:
                return self.send_json(400, {"ok": False, "error": "bad id"})

            with db() as conn:
                row = conn.execute("SELECT * FROM ghosts WHERE id = ?", (ghost_id,)).fetchone()

            if row is None:
                return self.send_json(404, {"ok": False, "error": "no such ghost"})

            blob_path = os.path.join(BLOB_DIR, row["blob"])
            if not os.path.exists(blob_path):
                return self.send_json(410, {"ok": False, "error": "ghost file is gone"})

            try:
                data = blob_read(blob_path)
            except (OSError, gzip.BadGzipFile, EOFError):
                return self.send_json(410, {"ok": False, "error": "ghost file is unreadable"})

            # Named the way the client names it, time included, so a download
            # lands beside the runs recorded here rather than on top of one.
            return self.send_blob(data, "pd-s%02d-d%d-%s-%06d.pdg" % (
                row["stagenum"], row["difficulty"],
                re.sub(r"[^A-Za-z0-9]", "_", row["username"]), row["time60"]))

        return self.send_json(404, {"ok": False, "error": "no such endpoint"})

    def do_POST(self):
        path, query = self.path_parts()

        if path == "/register":
            body = self.read_body()
            if body is None:
                return self.send_json(400, {"ok": False, "error": "bad body"})

            try:
                req = json.loads(body)
            except ValueError:
                return self.send_json(400, {"ok": False, "error": "bad json"})

            username = str(req.get("username", "")).strip()
            pin = str(req.get("pin", "")).strip()

            if not USERNAME_RE.match(username):
                return self.send_json(400, {"ok": False,
                    "error": "3-15 chars, letters, digits, _ . - only"})
            if not PIN_RE.match(pin):
                return self.send_json(400, {"ok": False, "error": "pin must be 4-8 digits"})

            if not rate_ok(_auth_failures, self.client_ip(), AUTH_WINDOW, AUTH_MAX_FAILURES):
                return self.send_json(429, {"ok": False, "error": "too many attempts"})

            # Registering hashes a PIN as well, and needs no account to reach.
            if not rate_ok(_auth_attempts, self.client_ip(), AUTH_ATTEMPT_WINDOW, AUTH_ATTEMPT_MAX):
                return self.send_json(429, {"ok": False, "error": "too many attempts"})

            salt = os.urandom(16)
            try:
                with db() as conn:
                    conn.execute(
                        "INSERT INTO users (username, pin_salt, pin_hash, created) VALUES (?,?,?,?)",
                        (username, salt, hash_pin(pin, salt), int(time.time())))
            except sqlite3.IntegrityError:
                # Unique usernames, case-insensitively.
                return self.send_json(409, {"ok": False, "error": "username already taken"})

            return self.send_json(200, {"ok": True, "username": username})

        if path == "/login":
            body = self.read_body()
            if body is None:
                return self.send_json(400, {"ok": False, "error": "bad body"})
            try:
                req = json.loads(body)
            except ValueError:
                return self.send_json(400, {"ok": False, "error": "bad json"})

            ok, err = self.authenticate(str(req.get("username", "")).strip(),
                                        str(req.get("pin", "")).strip())
            if not ok:
                return self.send_json(403, {"ok": False, "error": err})
            return self.send_json(200, {"ok": True})

        if path == "/upload":
            username = self.headers.get("X-Ghost-User", "").strip()
            pin = self.headers.get("X-Ghost-Pin", "").strip()

            ok, err = self.authenticate(username, pin)
            if not ok:
                return self.send_json(403, {"ok": False, "error": err})

            if not rate_ok(_upload_counts, username.lower(), UPLOAD_WINDOW, UPLOAD_MAX):
                return self.send_json(429, {"ok": False, "error": "too many uploads this hour"})

            body = self.read_body()
            if body is None:
                return self.send_json(413, {"ok": False, "error": "body missing or too large"})

            info = parse_ghost_header(body)
            if info is None:
                return self.send_json(400, {"ok": False, "error": "not a valid ghost file"})

            # A run recorded since ghosts carried an owner says which account
            # set it, and only that account may publish it. This is the check
            # that means something: the client does the same one to decide what
            # to offer, but the client is the part a thief controls, and the
            # file is what actually arrives here.
            #
            # It is a claim in a file rather than a signature, so it stops
            # somebody publishing a downloaded ghost, not somebody editing one.
            # Editing is the same effort as inventing a time outright, which
            # nothing here can catch either - see the note at the top about
            # what validating a run would actually take.
            # A ghost has to say which account set it, and it has to be this
            # one. Treating an absent owner as "anybody's" was a hole with a
            # worked example: a second account on the same machine uploaded a
            # first account's run, because the client fell back to comparing
            # the agent - a save file name, which does not change when the
            # ghost account does - and this end agreed by not asking.
            #
            # Files recorded before ghosts carried an owner are therefore
            # nobody's to publish. That is the safe direction: the alternative
            # lets anyone holding the file claim it.
            if not info["owner"]:
                return self.send_json(403, {"ok": False,
                    "error": "that ghost does not say which account recorded it"})

            if info["owner"].lower() != username.lower():
                return self.send_json(403, {"ok": False,
                    "error": "that ghost was recorded by %s" % info["owner"]})

            # Set under the rules the board ranks, or not on the board. A run
            # made with the fork's moves available is a different event on the
            # same map, and the file is the only thing that can say which this
            # was.
            if not info["flags"] & GHOST_TRIALRULES:
                return self.send_json(403, {"ok": False,
                    "error": "that run was not recorded in Ghost Trials"})

            # One file per player per level, overwritten in place, so a
            # replaced run leaves nothing behind to collect. The .gz is in the
            # name because what is on disk really is a gzip file, and a name
            # that lies about that is a trap for whoever looks in the
            # directory later.
            name = blob_name(username, info["stagenum"], info["difficulty"])
            blob_path = os.path.join(BLOB_DIR, name)
            doomed = []
            # Whether this file was already there decides what may be undone if
            # the row that is about to reference it never commits.
            existed = os.path.exists(blob_path)

            def undo_blob():
                # Only a file this request created. One that was already there
                # belongs to the row that is still pointing at it.
                if not existed:
                    try:
                        os.remove(blob_path)
                    except OSError:
                        pass

            with db_write(undo_blob) as conn:
                held = conn.execute(
                    "SELECT time60, blob, mpbody, mphead FROM ghosts WHERE username = ? AND stagenum = ? "
                    "AND difficulty = ?",
                    (username, info["stagenum"], info["difficulty"])).fetchone()

                # One row per player per level, replaced only by a quicker run.
                # The client keeps every attempt because it cannot know which
                # one mattered until afterwards; a board only ever wanted the
                # one that did, and storing the rest is disk spent on rows
                # nobody would read.
                if held is not None and info["time60"] >= held["time60"]:
                    # The run does not displace what is held, but it may still
                    # know something the row does not. A row stored before the
                    # board carried a character has none, and the only file
                    # that can supply one is the run itself - which is this
                    # one, since the client sends its best per level and the
                    # best is what is on the board. Without this the character
                    # could only ever arrive by beating your own time, so every
                    # row that predates the column would have stayed blank for
                    # good.
                    #
                    # Only ever fills a blank. It does not overwrite a
                    # character already recorded, and it does not touch the
                    # time, the blob or the ranking.
                    filled = False

                    if (held["mpbody"] == 0 and held["mphead"] == 0
                            and (info["mpbody"] or info["mphead"])):
                        conn.execute(
                            "UPDATE ghosts SET mpbody = ?, mphead = ? "
                            "WHERE username = ? AND stagenum = ? AND difficulty = ?",
                            (info["mpbody"], info["mphead"], username,
                             info["stagenum"], info["difficulty"]))
                        filled = True

                    return self.send_json(200, {"ok": True, "stored": False,
                        "reason": "you already have a faster time",
                        "best60": held["time60"], "character": filled})

                # Whether a new placement makes the board is asked before the
                # file is written, so a slow run on a full board costs a query
                # rather than a write and a delete. A player already holding a
                # row is on the board by definition and is only getting faster.
                if held is None:
                    faster = conn.execute(
                        "SELECT COUNT(*) FROM ghosts WHERE stagenum = ? AND difficulty = ? "
                        "AND time60 <= ?",
                        (info["stagenum"], info["difficulty"], info["time60"])).fetchone()[0]

                    if faster >= BOARD_KEEP:
                        return self.send_json(200, {"ok": True, "stored": False,
                            "reason": "outside the top %d" % BOARD_KEEP,
                            "time60": info["time60"]})

                # The file goes down before the row that points at it, so a
                # failure between them leaves a file nothing references rather
                # than a row whose file is missing - the first is invisible and
                # swept at startup, the second is a download that 410s.
                #
                # If the row does not commit, a file that was not there before
                # is taken back off. One that was there has been overwritten and
                # cannot be restored; its row is unchanged and still names it, so
                # what is left is a board row whose file holds a newer run by the
                # same player on the same level, which the next upload corrects.
                try:
                    blob_write(blob_path, body)
                except OSError:
                    return self.send_json(500, {"ok": False, "error": "could not store the ghost"})

                # A row written under the older naming points at a different
                # file, which nothing will reference once this row moves.
                if held is not None and held["blob"] != name:
                    doomed.append(held["blob"])

                conn.execute(
                    "INSERT INTO ghosts (username, stagenum, difficulty, time60, numsamples, bytes, uploaded, blob, flags, mpbody, mphead) "
                    "VALUES (?,?,?,?,?,?,?,?,?,?,?) "
                    "ON CONFLICT(username, stagenum, difficulty) DO UPDATE SET "
                    "time60=excluded.time60, numsamples=excluded.numsamples, "
                    "bytes=excluded.bytes, uploaded=excluded.uploaded, blob=excluded.blob, "
                    "flags=excluded.flags, mpbody=excluded.mpbody, mphead=excluded.mphead",
                    (username, info["stagenum"], info["difficulty"], info["time60"],
                     info["numsamples"], len(body), int(time.time()), name, info["flags"],
                     info["mpbody"], info["mphead"]))

                # Whatever fell off the end of the board goes, whoever it
                # belonged to. Ordered the same way the leaderboard is read so
                # that the hundred rows kept here are the hundred rows served,
                # with the id breaking a tie that time60 alone cannot.
                doomed += [r["blob"] for r in conn.execute(
                    "SELECT blob FROM ghosts WHERE stagenum = ? AND difficulty = ? "
                    "ORDER BY time60 ASC, id ASC LIMIT -1 OFFSET ?",
                    (info["stagenum"], info["difficulty"], BOARD_KEEP)).fetchall()]

                conn.execute(
                    "DELETE FROM ghosts WHERE id IN ("
                    "  SELECT id FROM ghosts WHERE stagenum = ? AND difficulty = ? "
                    "  ORDER BY time60 ASC, id ASC LIMIT -1 OFFSET ?)",
                    (info["stagenum"], info["difficulty"], BOARD_KEEP))

                rank = conn.execute(
                    "SELECT COUNT(*) + 1 FROM ghosts WHERE stagenum = ? AND difficulty = ? AND time60 < ?",
                    (info["stagenum"], info["difficulty"], info["time60"])).fetchone()[0]

            # After the commit, and only for files nothing points at any more.
            # A row left with no file downloads as a 410, which is a worse
            # failure than a file left with no row: that one is invisible.
            self.drop_blobs(doomed)

            return self.send_json(200, {"ok": True, "stored": True, "rank": rank,
                                        "time60": info["time60"]})

        return self.send_json(404, {"ok": False, "error": "no such endpoint"})


def main():
    init_db()
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print("pdghostd listening on %s:%d, data in %s" % (HOST, PORT, ROOT), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
