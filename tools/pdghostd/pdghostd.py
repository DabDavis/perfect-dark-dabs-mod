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

# The client writes one shape of file and this end takes only that shape: a
# 128 byte header and 24 byte samples (struct modghostheader and struct
# modghostsample in the client's modghost.h, the second pinned by a static
# assert in modghost.c), at most MODGHOST_MAXSAMPLES of them, one every
# MODGHOST_RATE60 sixtieths. The longest run the recorder can hold is under
# 1.5MB, so two is generous and still bounded.
MAX_BODY = 2 * 1024 * 1024
MAX_SAMPLES = 65536
HEADER_SIZE = 128
SAMPLE_SIZE = 24
RATE60 = 3
VERSION_MAX = 2
MAGIC = b"PDGHOST\0"

# How much one account may hold across all its rows, counted in the bytes it
# uploaded rather than the smaller number that landed on disk. A full set of
# the longest possible runs on every board is more than this, which is the
# point: an account is a player's best times, not a place to keep files.
USER_QUOTA = 64 * 1024 * 1024

# The stages a trial can be set on: the solo missions, as g_SoloStages in the
# client's mainmenu.c lists them. modGhostStageIsEligible() in modghost.c is
# looser - anything below STAGE_TITLE that is not the Institute - but the rest
# of that range is Combat Simulator arenas, which have no clock to race, and
# ids the mod loader hands out at runtime, which name different maps on
# different machines. A board for either would be a board of nothing.
STAGES = frozenset((
    0x30,  # Defection
    0x33,  # Investigation
    0x22,  # Extraction
    0x2c,  # Villa
    0x1d,  # Chicago
    0x1e,  # G5 Building
    0x2f,  # Infiltration
    0x35,  # Rescue
    0x19,  # Escape
    0x27,  # Air Base
    0x31,  # Air Force One
    0x1c,  # Crash Site
    0x21,  # Pelagic II
    0x38,  # Deep Sea
    0x2d,  # Defense
    0x34,  # Attack Ship
    0x2a,  # Skedar Ruins
    0x37,  # Mr Blonde's Revenge
    0x09,  # Maian SOS
    0x16,  # WAR!
    0x4f,  # The Duel
))
DIFFICULTIES = (0, 1, 2)

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

# The same again per account, whoever is asking. The address limiter stops
# one machine guessing; it does nothing about many machines each guessing a
# few times at one account, and a four digit PIN is ten thousand guesses.
# Eight in fifteen minutes makes that a matter of weeks. The cost is that
# anybody can lock an account for a quarter of an hour by naming it, which is
# the trade every lockout makes and the cheaper one here.
USER_WINDOW = 900
USER_MAX_FAILURES = 8

# Every check of a PIN costs a PBKDF2, which is the point for a stored secret
# and a cost for a server. The failure budget above is cleared by a success,
# so an address holding one valid account could keep failing between
# sign-ins. This budget counts failures too but is never cleared, so it is
# the ceiling on how much hashing a wrong guess can buy. Thirty in five
# minutes is far above mistyping a PIN and far below being useful as a way
# to spend somebody else's processor.
#
# Successes are not counted here, and uploads do not touch it at all: one
# press of Upload is one PIN check per ghost sent, and UPLOAD_MAX below is
# what bounds those. Counting them here meant a player with more than thirty
# runs to send was refused on the thirty-first for "too many attempts".
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

# The header flag that says a run was set under trial rules - the fork's added
# moves off, and the game's own cheats with them. MODGHOSTHF_TRIALRULES in the
# client's modghost.h. A run without it was made under unknown rules, which is
# not the same as fair rules: everything recorded before the flag existed had
# jump and the combat roll available, and there is no asking now which were on.
# A board that mixes them ranks settings.
GHOST_TRIALRULES = 0x01

# How many rows a board keeps, and so how many blobs a stage and difficulty
# can cost. One row per player is the other half of that bound: the client
# stores every attempt because a run cannot be judged while it is being set,
# but the board only wants the answer, and a hundred players' answers is a
# leaderboard where a hundred of one player's afternoons is a log.
BOARD_KEEP = 100

_lock = threading.Lock()
_auth_failures = {}
_auth_attempts = {}
_user_failures = {}
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
                mphead     INTEGER NOT NULL DEFAULT 0,
                evicted    INTEGER NOT NULL DEFAULT 0
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

        # A row that fell off the end of its board stays, hidden, so that
        # the file it names stays too - see the eviction note in the upload
        # path. Every row before the column existed is one that is on its
        # board, so the default is right for all of them.
        if "evicted" not in have:
            conn.execute("ALTER TABLE ghosts ADD COLUMN evicted INTEGER NOT NULL DEFAULT 0")

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


def rate_ok(table, key, window, limit, record=True):
    """Whether a key is under its limit, counting this call against it.

    record=False only asks. That is for the limiters that count failures: an
    attempt is checked before the work it guards and counted, with rate_hit()
    below, only once it has failed.
    """
    now = time.time()
    with _lock:
        if len(table) > RATE_TABLE_MAX:
            for dead in [k for k, v in table.items() if not v or now - v[-1] >= window]:
                del table[dead]

        hits = [t for t in table.get(key, []) if now - t < window]
        if len(hits) >= limit:
            table[key] = hits
            return False
        if record:
            hits.append(now)
        if hits or key in table:
            table[key] = hits
        return True


def rate_hit(table, key):
    """Count one against a key without asking whether it is over."""
    with _lock:
        table.setdefault(key, []).append(time.time())


def fnv1a(data):
    """FNV-1a over a byte string, as modGhostHash() in the client's modghost.c.

    32 bit, offset basis 2166136261, prime 16777619, one byte at a time in
    file order. The client hashes the sample block alone - see the header
    comment in modghost.h - and so does the caller here. A pure Python loop
    over the biggest block the recorder can produce is a few tenths of a
    second, which an upload can afford and nothing else here calls.
    """
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def query_int(query, key, default):
    """An integer query parameter, or ValueError if it is not one SQLite holds.

    int() reads a number of any size and sqlite3 raises OverflowError binding
    one past 64 bits - from inside the execute, where nothing was catching it.
    """
    value = int(query.get(key, default))
    if value < -(1 << 63) or value >= (1 << 63):
        raise ValueError(key)
    return value


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
    (hash_,) = struct.unpack_from("<I", data, 0x24)
    # The Combat Simulator body and head the run was set as, plus one so that
    # zero can mean "whoever Joanna comes with" - MODGHOST_BODY_DEFAULT in the
    # client's modghost.h. Stored so a board can say who a time belongs to
    # rather than only what they were called.
    mpbody, mphead = struct.unpack_from("<2B", data, 0x2c)
    player = data[0x30:0x50].split(b"\0")[0].decode("ascii", "replace")
    owner = data[0x70:0x80].split(b"\0")[0].decode("ascii", "replace")

    # What the client writes, not what it would read. modGhostReadFile()
    # accepts a header or a sample larger than its own so that a later build
    # can add fields; none has, so every file the game has ever saved has
    # exactly this shape, and one that does not came from something else.
    if version < 1 or version > VERSION_MAX:
        return None
    if headersize != HEADER_SIZE or samplesize != SAMPLE_SIZE:
        return None
    if numsamples < 2 or numsamples > MAX_SAMPLES:
        return None
    if rate60 != RATE60:
        return None
    # The recorder - modGhostRecordSample() in modghost.c - keeps
    # (numsamples - 1) * rate60 <= clock < numsamples * rate60 at every sample
    # it takes, and the time in the header is read when the run is saved from
    # the end screen, where the mission clock has stopped. One interval of
    # slack on either side covers a frame between the last sample and the
    # stop; a time outside that describes a different run than the samples do.
    if time60 < (numsamples - 1) * rate60 or time60 > (numsamples + 1) * rate60:
        return None
    if stagenum not in STAGES or difficulty not in DIFFICULTIES:
        return None
    # The whole file and nothing else. Trailing bytes are not a ghost, and
    # they would count against the quota as if they were.
    if len(data) != headersize + numsamples * samplesize:
        return None
    # The hash covers the sample block, as modGhostHash() takes it: the bytes
    # after the header, numsamples * samplesize of them, in file order. It is
    # what tells a complete file from a truncated or edited one; it says
    # nothing about whether the run happened, which nothing here can tell.
    if hash_ != fnv1a(data[headersize:headersize + numsamples * samplesize]):
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
        # A request line too long or too malformed to parse is answered from
        # inside parse_request(), before there are any headers, and the answer
        # is logged through here. Reaching for self.headers then raised in
        # the log call and the 400 was never written.
        headers = getattr(self, "headers", None)
        if headers is None:
            return self.client_address[0]

        real = headers.get("X-Real-IP", "").strip()
        if real:
            return real

        fwd = headers.get("X-Forwarded-For", "")
        if fwd:
            return fwd.rsplit(",", 1)[-1].strip()

        return self.client_address[0]

    def body_pending(self):
        """Whether the request carried a body that has not been read.

        Anything answered before the body is read - a refused PIN, an
        oversized upload - leaves that body in the socket, where the next
        request on the connection would be parsed out of it. Those answers
        close the connection instead.
        """
        if getattr(self, "body_done", False):
            return False
        if self.headers.get("Transfer-Encoding"):
            return True
        try:
            return int(self.headers.get("Content-Length", "0")) > 0
        except ValueError:
            return True

    def send_json(self, code, payload):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        if self.body_pending():
            self.send_header("Connection", "close")
            self.close_connection = True
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
        body = self.rfile.read(length)
        self.body_done = True
        return body

    def read_json(self):
        """The body as a JSON object, or None if it is not one.

        Not an object covers a lot: a body that is not JSON, one that is JSON
        but a list or a number, and one nested deeply enough that the parser
        gives up on it. Each of those was a different exception, and only the
        first was being caught.
        """
        body = self.read_body()
        if body is None:
            return None
        try:
            req = json.loads(body)
        except (ValueError, RecursionError):
            return None
        if not isinstance(req, dict):
            return None
        return req

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
        """Remove blob files, ignoring what is missing.

        Whether a file is still referenced is decided by the caller inside
        the transaction that changed the rows, not here after it: asked
        afterwards, the answer is about whatever the rows have become since.
        """
        for name in names:
            try:
                os.remove(os.path.join(BLOB_DIR, name))
            except OSError:
                pass

    def authenticate(self, username, pin):
        """Check a username and PIN. Returns (ok, error).

        Nothing is counted until it fails, and a failure counts against the
        address and against the account named, so that neither one machine
        nor many can sit and guess. The answer is the same whether the account
        exists or the PIN was wrong: the difference between those is the list
        of usernames, and a guesser should have to guess those as well.
        """
        ip = self.client_ip()
        user = username.lower()

        if (not rate_ok(_auth_failures, ip, AUTH_WINDOW, AUTH_MAX_FAILURES, record=False)
                or not rate_ok(_auth_attempts, ip, AUTH_ATTEMPT_WINDOW, AUTH_ATTEMPT_MAX, record=False)
                or not rate_ok(_user_failures, user, USER_WINDOW, USER_MAX_FAILURES, record=False)):
            return False, "too many attempts, wait a few minutes"

        if not username or not pin:
            return False, "username and pin required"

        with db() as conn:
            row = conn.execute("SELECT * FROM users WHERE username = ?", (username,)).fetchone()

        if row is None:
            # Hashed anyway, against nothing, so that a name that is not an
            # account takes as long to refuse as a PIN that is wrong.
            salt, expect = b"\0" * 16, b"\0" * 32
        else:
            salt, expect = bytes(row["pin_salt"]), bytes(row["pin_hash"])

        if row is None or not hmac.compare_digest(expect, hash_pin(pin, salt)):
            rate_hit(_auth_failures, ip)
            rate_hit(_auth_attempts, ip)
            rate_hit(_user_failures, user)
            return False, "wrong username or pin"

        # A success clears the failure budgets for this address and account.
        with _lock:
            _auth_failures.pop(ip, None)
            _user_failures.pop(user, None)

        return True, None

    # ---------------------------------------------------------------- routes

    def do_GET(self):
        path, query = self.path_parts()

        if path in ("", "/ping"):
            return self.send_json(200, {"ok": True, "service": "pdghostd", "version": 1})

        if path == "/leaderboard":
            try:
                stage = query_int(query, "stage", "-1")
                diff = query_int(query, "diff", "-1")
                # Clamped at both ends: SQLite reads a negative LIMIT as no
                # limit at all, so an unclamped one served the whole board.
                limit = max(1, min(query_int(query, "limit", "100"), 100))
            except (ValueError, OverflowError):
                return self.send_json(400, {"ok": False, "error": "bad query"})

            with db() as conn:
                # The id breaks ties so that the rows served are the rows the
                # upload path decided to keep, in the same order. Two runs at
                # the same time60 are two places on the board and the earlier
                # upload takes the higher one.
                rows = conn.execute(
                    "SELECT id, username, time60, uploaded, flags, mpbody, mphead FROM ghosts "
                    "WHERE stagenum = ? AND difficulty = ? AND evicted = 0 "
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
                ghost_id = query_int(query, "id", "-1")
            except (ValueError, OverflowError):
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
            req = self.read_json()
            if req is None:
                return self.send_json(400, {"ok": False, "error": "bad body"})

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
            req = self.read_json()
            if req is None:
                return self.send_json(400, {"ok": False, "error": "bad body"})

            ok, err = self.authenticate(str(req.get("username", "")).strip(),
                                        str(req.get("pin", "")).strip())
            if not ok:
                return self.send_json(403, {"ok": False, "error": err})
            return self.send_json(200, {"ok": True})

        if path == "/upload":
            username = self.headers.get("X-Ghost-User", "").strip()
            pin = self.headers.get("X-Ghost-Pin", "").strip()

            # Counted before the PIN is checked, so that this and not the
            # sign-in limiter is what bounds the hashing an upload can ask
            # for. A refused PIN spends one of the account's hundred and
            # twenty, which is nothing next to the lockout it also spends.
            if not rate_ok(_upload_counts, username.lower(), UPLOAD_WINDOW, UPLOAD_MAX):
                return self.send_json(429, {"ok": False, "error": "too many uploads this hour"})

            ok, err = self.authenticate(username, pin)
            if not ok:
                return self.send_json(403, {"ok": False, "error": err})

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
                # An evicted row counts as held: its file is still the best
                # run this player has sent, and a slower one is not news.
                held = conn.execute(
                    "SELECT time60, blob, bytes, mpbody, mphead, evicted FROM ghosts "
                    "WHERE username = ? AND stagenum = ? AND difficulty = ?",
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
                # rather than a write and a delete. A player holding a row on
                # the board is there by definition and is only getting faster;
                # one holding an evicted row is asking to get back on.
                if held is None or held["evicted"]:
                    faster = conn.execute(
                        "SELECT COUNT(*) FROM ghosts WHERE stagenum = ? AND difficulty = ? "
                        "AND evicted = 0 AND time60 <= ?",
                        (info["stagenum"], info["difficulty"], info["time60"])).fetchone()[0]

                    if faster >= BOARD_KEEP:
                        return self.send_json(200, {"ok": True, "stored": False,
                            "reason": "outside the top %d" % BOARD_KEEP,
                            "time60": info["time60"]})

                # What the account holds across every board, less the row
                # this run replaces, plus this run, against the cap. Asked
                # here rather than before the board check so that a run that
                # was never going to be stored is not refused for a reason
                # that would not have applied.
                used = conn.execute(
                    "SELECT COALESCE(SUM(bytes), 0) FROM ghosts WHERE username = ?",
                    (username,)).fetchone()[0]

                if held is not None:
                    used -= held["bytes"]

                if used + len(body) > USER_QUOTA:
                    return self.send_json(413, {"ok": False,
                        "error": "your account is out of storage"})

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
                    "flags=excluded.flags, mpbody=excluded.mpbody, mphead=excluded.mphead, "
                    "evicted=0",
                    (username, info["stagenum"], info["difficulty"], info["time60"],
                     info["numsamples"], len(body), int(time.time()), name, info["flags"],
                     info["mpbody"], info["mphead"]))

                # Whatever fell off the end of the board is hidden, whoever it
                # belonged to, and its file stays. A run somebody set is not
                # this end's to destroy because a hundred people were quicker,
                # and the client keeps no copy of what it uploaded from a
                # machine it may since have lost. Ordered the same way the
                # leaderboard is read so that the hundred rows shown are the
                # hundred rows kept, with the id breaking a tie that time60
                # alone cannot. Evicted rows keep their bytes on the account's
                # quota, which is the only thing bounding them now.
                conn.execute(
                    "UPDATE ghosts SET evicted = 1 WHERE id IN ("
                    "  SELECT id FROM ghosts WHERE stagenum = ? AND difficulty = ? AND evicted = 0 "
                    "  ORDER BY time60 ASC, id ASC LIMIT -1 OFFSET ?)",
                    (info["stagenum"], info["difficulty"], BOARD_KEEP))

                rank = conn.execute(
                    "SELECT COUNT(*) + 1 FROM ghosts WHERE stagenum = ? AND difficulty = ? "
                    "AND evicted = 0 AND time60 < ?",
                    (info["stagenum"], info["difficulty"], info["time60"])).fetchone()[0]

                # Only a file no row names any more, decided here where the
                # rows cannot change under the answer. The old naming folded
                # dab.2 and dab-2 onto one file, so the row that moved away
                # from it may not have been the only one pointing at it.
                doomed = [n for n in doomed if conn.execute(
                    "SELECT 1 FROM ghosts WHERE blob = ? LIMIT 1", (n,)).fetchone() is None]

            # After the commit, so that a row is never left naming a file that
            # is gone: that downloads as a 410, which is a worse failure than
            # a file left with no row, which is invisible.
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
