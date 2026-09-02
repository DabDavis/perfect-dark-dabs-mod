# pdghostd — the Ghost Trials server

The other half of Ghost Trials. The game records and races entirely on its own;
this is only what makes a run shareable — accounts, uploads, leaderboards and
downloads. `port/src/ghostnet.c` is the client that talks to it.

It lives here because the two ends share a format. `struct modghostheader` in
`src/include/game/modghost.h` is parsed by `parse_ghost_header()` below, and the
filenames the server hands out are the ones the chooser expects to find in the
ghosts directory. Changing one without the other is how a ghost stops loading,
so they are versioned together.

Stdlib only: no Flask, no ORM. The box it runs on already hosts several
services and this one should not bring a dependency tree with it.

## What it stores

One row per player per stage per difficulty, replaced only by a quicker run.
The client keeps every attempt, because which run mattered is not knowable while
it is being set; a board only ever wanted the one that did.

Blobs are gzipped — a real recording stores at about 41% of its size, and what
comes back out is byte-identical, so nothing about the ghost's sample rate or
quality is traded for disk. Files written before compression are still served:
two bytes at the front of the file decide, not a flag in the database.

Blob filenames are the account name with anything awkward folded to an
underscore plus a hash of it. The hash is not decoration: `_`, `.` and `-` are
all legal in a username and all fold to `_`, so without it `dab.2` and `dab-2`
share a file and one player's board row serves the other player's ghost. Rows
written before this point at their old filenames and keep working.

Each board shows its hundred quickest times. Rows past that are marked
`evicted` and hidden rather than deleted, and their files stay on disk: a run
somebody set is not the server's to destroy because a hundred people were
quicker. The only time the server removes a ghost is when the same player
overwrites their own file with their own quicker run (and, on startup, a file
no row at all references). What bounds the blob directory is one row per
player per level and a per-account cap on uploaded bytes (`USER_QUOTA`,
64 MiB), which counts evicted rows too.

Evicted files are retained indefinitely and can be swept by hand:
`SELECT username, blob FROM ghosts WHERE evicted = 1` lists them, and deleting
those rows and restarting the service lets the startup orphan sweep remove the
files nothing references any more.

An upload reads the board to decide whether a run makes it and then writes on
that answer, so it runs in a `BEGIN IMMEDIATE` transaction: the two halves are
one decision, and two uploads arriving together cannot both take the last place
on a full board. The blob goes down before the row that names it, and comes
back off if that row does not commit.

Times are accepted as submitted. Validating a run properly means replaying it
against the stage geometry, which is a different project. What is enforced is
the shape of the data, so a malformed or enormous upload cannot cost disk or CPU.
The shape is checked the way the client writes it rather than the looser way it
reads it: a 128 byte header, 24 byte samples, version 1 or 2, `rate60` equal to
`MODGHOST_RATE60` (3), `time60` within one interval of `numsamples * rate60`
(the recorder's invariant), a stage from the solo mission list, a difficulty
of 0-2, no trailing bytes, and the FNV-1a hash over the sample block exactly as
`modGhostHash()` computes it.

## Testing it

```sh
python3 tools/pdghostd/test_pdghostd.py
```

Runs a patched copy on port 8392 (`PDGHOSTD_TEST_PORT` to change it) against
a temporary directory and takes it through registration, uploads built the
way the game builds them, two dozen forged headers, the hot-account slowdown,
the quota, eviction, and the malformed requests that used to drop the
connection. Standard library only; nothing outside the temporary directory is
touched, and the live database is never involved.

## Running it

```sh
scp tools/pdghostd/pdghostd.py you@host:~/pdghostd.py
sudo cp tools/pdghostd/pdghostd.service /etc/systemd/system/
sudo systemctl enable --now pdghostd
```

The unit runs with `ProtectHome=tmpfs`, so inside the service `/home/sdg` is
an empty tmpfs apart from what `BindPaths=` and `BindReadOnlyPaths=` bring in:
the data directory and the script itself. If the script lives anywhere other
than `/home/sdg/pdghostd.py`, either put it under `/home/sdg/pdghosts` or add
its path to `BindReadOnlyPaths=`, or the unit fails to start because python
cannot find it.

It listens on `127.0.0.1:8090` and takes the client address from `X-Real-IP`,
falling back to the **last** element of `X-Forwarded-For` — the one
`$proxy_add_x_forwarded_for` appends. Both are only trustworthy because nothing
reaches it except through the proxy, so it must not be exposed directly: a
client that can set those headers can set the address the rate limiter counts
against, and the rate limiter rather than the four digit PIN is what actually
guards an account.

Wrong PINs are counted per address (8 in 5 minutes refuses, and 30 in 5
minutes as a ceiling on hashing) and per account (8 in 15 minutes, whoever is
asking). The account count does not lock — a lock would let anyone keep a
player off the board by naming them. Past it the account is *hot*: attempts
from an address it has not signed in from before are taken one at a time,
each after a 3 second wait, and at most two may queue before the next is told
to try again in a moment. The addresses an account has signed in from (the
last five, including the one that registered it) skip the wait entirely. A
refused sign-in says `wrong username or pin` whether or not the account
exists. Uploads are counted separately, 120 an hour per account, and a valid
PIN counts against nothing. Behind nginx:

```nginx
location /pdghosts/ {
    proxy_pass http://127.0.0.1:8090/;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    client_max_body_size 2m;
}
```

`client_max_body_size` has to clear `MAX_BODY` (2 MiB; the longest run the
recorder can hold is under 1.5 MiB), or a long run is rejected by the proxy
before the server sees it. Data lands in `~/pdghosts`: `ghosts.db` and a
`blobs/` directory.

The client's default server is in `port/src/ghostnet.c` and is overridable per
machine with `Mod.GhostServer` in `pd.ini`, which is also how you point a build
at a local copy for testing.

## Changing it

Schema changes run from `init_db()` on startup, against a live database that is
the only one there is — back it up first. Migrations that create a unique index
must delete the rows that would violate it beforehand, or the service fails to
start and takes the board down with it.

There is no test suite. Test by patching `ROOT` and `PORT` into a copy and
running that against a throwaway directory:

```sh
sed -e 's|^PORT = 8090|PORT = 8391|' \
    -e 's|^ROOT = .*|ROOT = os.path.abspath("testroot")|' \
    pdghostd.py > testd.py
```
