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

Each board keeps its hundred quickest times and deletes the rest with their
files. That and one-row-per-player are the only things bounding the blob
directory, since nothing else here ever removes a ghost.

Times are accepted as submitted. Validating a run properly means replaying it
against the stage geometry, which is a different project. What is enforced is
the shape of the data, so a malformed or enormous upload cannot cost disk or CPU.

## Running it

```sh
scp tools/pdghostd/pdghostd.py you@host:~/pdghostd.py
sudo cp tools/pdghostd/pdghostd.service /etc/systemd/system/
sudo systemctl enable --now pdghostd
```

It listens on `127.0.0.1:8090` and takes the client address from `X-Real-IP`,
falling back to the **last** element of `X-Forwarded-For` — the one
`$proxy_add_x_forwarded_for` appends. Both are only trustworthy because nothing
reaches it except through the proxy, so it must not be exposed directly: a
client that can set those headers can set the address the rate limiter counts
against, and the rate limiter rather than the four digit PIN is what actually
guards an account. Behind nginx:

```nginx
location /pdghosts/ {
    proxy_pass http://127.0.0.1:8090/;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    client_max_body_size 8m;
}
```

`client_max_body_size` has to clear `MAX_BODY`, or a long run is rejected by the
proxy before the server sees it. Data lands in `~/pdghosts`: `ghosts.db` and a
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
