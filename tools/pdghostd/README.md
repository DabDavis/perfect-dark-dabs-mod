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
the quota, eviction, the three security questions (and accounts with one,
and one from a database that predates `rec_count`), the two limiters behind
resetting a PIN, the crash and problem reports (what is stored, what is cut,
what is stripped, the address limiter and the full directory), the report
board (no address or dump on the page, escaping, the status and patch notes
files, paging, filters, pictures and every path that is not one), the monthly
archive (run as a subprocess against the temporary directory) and the
malformed requests that used to drop the connection. Standard library only; nothing outside the temporary directory is
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
PIN counts against nothing.

A PIN that is lost is reset by answering the account's security questions:
up to three, each one category out of ten and one answer out of that
category's fifty, all chosen from dropdowns in the client
(`port/include/ghostrecovery.h`). What is stored is a PBKDF2 of every pair
joined in order, `"<category id>|<answer id>|..."`, and a salt of its own —
not the categories, so a guesser has to find those as well — with `rec_count`
beside it saying how many pairs went in. **Those ids are a wire format**:
renaming or reordering one in the client locks out everybody who chose it.

`POST /resetpin` takes the name, the pairs and the PIN the account is to have;
the first `rec_count` pairs sent are hashed, so a three-question account needs
all three and a one-question account is reset by its one. `POST /setrecovery`
replaces the questions and needs the account's PIN, which is why a new account
picks them at registration: the page that changes them is the one somebody
who has lost the PIN cannot use. `/register` takes `question`/`answer`,
`question2`/`answer2` and `question3`/`answer3` alongside the PIN and still
accepts a body with one pair or none, because builds that predate the page
cannot send more; accounts with none are refused a reset until their owner
sets some, and a sign-in reply says `"recovery"` and `"questions"` so the
client can tell an account with one that it could have three.

Ten times fifty is not a password, so the limiter carries it: five wrong
answers a day at one account, ten attempts an hour from one address, and every
attempt waits `RESET_DELAY` before the answer is looked at, so an account that
does not exist takes as long to refuse as one that does. A successful sign-in
clears the account's budget, so a stranger guessing at the question cannot keep
its owner from their own recovery. A wrong answer, a wrong category, an account
with no question and a name that is not an account are one sentence:
`wrong question or answer`.

## Crash reports

The one thing here that is not about ghosts, and the only reason it lives in
this daemon is that the client already has an HTTP client pointed at it.

The game writes a report when it dies - the error and its stack, which build it
is, the mod that was mounted, the player's `[Mod]` settings and the last few
hundred lines of its log - and offers to send it: from the crash dialog's *Send
report to Dab* button, and from *Crash Reports* on the Perfect Menu next time
the game starts, which is also where a note can be typed. Nothing is sent
unless somebody presses one of those. `port/src/crashreport.c` is that end.

`POST /crash` takes `{report, note, version, platform, channel}` and writes one
file per report into `~/pdghosts/crashes`, named for the time it arrived. The
file gets a header of what the server knows - when, from which address, and the
three fields - and the client's report underneath it. No account is involved:
a crash belongs to whoever had it rather than to an account, and the page that
sends one is reachable by a player who has never signed in.

There is **no endpoint that lists or serves a report back**, deliberately.
They are files for reading over ssh:

```sh
ls -t ~/pdghosts/crashes | head
head -40 ~/pdghosts/crashes/20260912-213535-8ac7d9af.txt
```

What bounds it: 32 KiB of report and 200 characters of note (anything longer is
cut), twelve reports an hour from one address, and `CRASH_MAX_FILES` (5000) in
the directory - past which reports are refused with a 507 rather than filling
the disk. Reports are never deleted by the server; sweeping them is by hand,
which is one `rm` and no database to keep in step. Control characters are
stripped as a report is stored, because a stack trace that carries an escape
sequence would otherwise repaint the terminal of whoever cats it.

Behind nginx:

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

`client_max_body_size` has to clear `REPORT_MAX_BODY` (8 MiB, the F3 problem reports) and `MAX_BODY` (2 MiB; the longest run the
recorder can hold is under 1.5 MiB, and a crash report is under 200 KiB once
its newlines are escaped), or a long run is rejected by the proxy
before the server sees it. Data lands in `~/pdghosts`: `ghosts.db` and a
`blobs/` directory.

The client's default server is in `port/src/ghostnet.c` and is overridable per
machine with `Mod.GhostServer` in `pd.ini`, which is also how you point a build
at a local copy for testing.

## Problem reports

`POST /report` is the F3 key's *Report a Problem* dialog
(`port/src/tracereport.c`): the same rules as a crash report - no account,
nothing sent unless the player pressed Send, and only the public part read
back (see **The report board**) - with a
bigger payload: `{report, note, version, platform, channel, screenshot}`, where
`report` is the whole F3 state dump plus settings and log, and `screenshot` a
base64 PNG the client has already scaled to at most 1280 wide.

Each report is two files in `~/pdghosts/reports`, `<stamp>-<hex>.txt` (header
of what the server knows, note on one line, then the report) and `.png`
(written first, so a `.txt` always has its picture; only a real PNG is kept).

```sh
ls -t ~/pdghosts/reports | head
scp 'sdg@10.8.0.1:~/pdghosts/reports/20260917-103319-ee727897.*' .
```

Bounds: 8 MiB body on this route (`REPORT_MAX_BODY`; the ghost routes keep
2 MiB), 512 KiB of text, 1000 characters of note, 5 MiB of picture, thirty an
hour from one address, `REPORT_MAX_FILES` (10000) reports - counted by the
shape of a report's name, so `status.txt` and `patchnotes.txt` beside them are
not. 10000 is a backstop (~10 GB at ~1 MB a report); the monthly archive below
is what keeps the directory to a month or two. **nginx's
`client_max_body_size` must be at least 8m** for this route, or a report with a
picture is refused by the proxy with a 413 the server never sees.

## The report board

`GET /board` is a public, read-only page of the problem reports:
<https://texturepacks.art/pdghosts/board>. Two columns - the reports, newest
first, 25 a page and grouped by day, and **Fixed**, every fix that shipped,
newest first, each crediting its reporters and linking to their reports
(a phone gets the reports and a link to the Fixed column). Filters are query
parameters: `?name=<tester>`, `?status=<word>`, `?id=<id or stamp>`,
`?page=<n>`. `GET /board.json` is the same page as data (plus every fix), and
`GET /board/shot/<id>.png` and `/board/thumb/<id>.png` the picture and a
320-wide copy. No script, no external anything, one inline stylesheet that
follows the system's light or dark setting.

What it shows of a report is only what the player typed or chose to send: the
time, the `name:` (`-` or none reads "anonymous"), the note, the picture, the
build and the OS name from `platform:`. The header is read line by line into
named fields, so the `from:` address, the dump, the settings, the log and the
paths are never read into anything the page can reach; every string is
HTML-escaped and the page carries a `Content-Security-Policy` that runs no
script. A picture is served only for an id that matches the exact shape the
`/report` route writes (`YYYYmmdd-HHMMSS-<8 hex>`), of a report on the board,
that has a picture - there is no path in the URL to traverse. Full pictures
are limited to 300 an hour an address.

Headers are cached and re-read when the directory's mtime moves; the status
file, the patch notes and the archive index when theirs do - so editing any of
them shows on the next request, no restart.

Thumbnails are made without an imaging library (the box has no Pillow): the
client writes every screenshot as 8-bit RGB with no row filters, which is
shrunk by a 4x4 box filter in about a fifth of a second, once per report, and
kept in `~/pdghosts/reportthumbs/`. Any other PNG is served full size.

### status.txt

`~/pdghosts/reports/status.txt`, edited by hand at each pass over the
reports, says what became of each one. No file, or no line for a report,
means *Received*. One report a line:

    <report id or stamp> <status> [<when>] <public comment>

    20260925-224209-e9d359f1 working Fixed, in the next update - Chicago: the patrol robot's shots no longer freeze
    20260925-222028-fbe67314 fixed 1.17 Fixed in 5ac4c31
    20260917-202627-b6421b31 fixed 2026-09-18 Fixed in f6b6f4c - GE-X Plus: the menu is no longer greyed out
    20260925-224248-8a9f8748 duplicate 20260925-224209-e9d359f1 - same robot shots

Status words: `received`, `working` (shown as *In progress*), `fixed`,
`needinfo`, `notabug`, `wontfix`, `duplicate` (comment starts with the other
report's id; it shares that report's fix), and `hidden` (off the board
altogether - for a note that should not be public; the report is kept). A
stamp without the hex names every report sent that second; a full id's line
beats a stamp's, a later line beats an earlier one, and a line the board cannot
read (bad id, unknown word) is skipped. `#` starts a comment. The comment is
public: plain words, never an address, path or function name. Report ids in a
comment become links.

`<when>` is for `fixed`: either `<batch>.<line>`, a line of the patch notes
(below), or `YYYY-MM-DD`, the day it shipped, for a fix the notes do not
announce - in which case the comment is the whole fix line, and reports fixed
by the same change get the identical date and comment so the Fixed column
shows them once. `tools/pdghostd/status-seed.txt` has the full format at its
top and was the first copy of the file.

### patchnotes.txt

The repo's `patchnotes.txt` (the game's own patch notes, see its header),
copied to `~/pdghosts/reports/patchnotes.txt` at each release. Each of its
lines is an entry in the Fixed column, with the batch's date and the line's
`(thanks ...)` as the credit (a line that thanks nobody credits whoever sent
the reports that name it). A status line `fixed 1.17 Fixed in 5ac4c31` ties a
report to the seventeenth fix line under `notes 1`: the card then reads
"Fixed in 5ac4c31 - <that line>", and the fix links to the report. A pushed
batch is never edited, so the number stays right - so each fix is written once,
in the patch notes. Without the file the board still works on dates alone.

## Archiving reports

`python3 ~/pdghostd.py --archive` moves every report pair older than 30 days
(by the stamp in its name; `--days N` to change it, `--dry-run` to only list)
into `~/pdghosts/reports-archive/<YYYY-MM>.tar.xz`, one tar per month of
stamps. A month's tar is rebuilt as its old members plus the new files under a
temporary name, read back and compared (every old member present, every new
file byte for byte), renamed over the old one, and only then are the originals
deleted - a failure anywhere leaves the originals and the old tar as they were
and exits 1. Only report-shaped names are touched: `status.txt` and
`patchnotes.txt` stay. Each archived report's id, time and name are appended to
`reports-archive/index.txt`, which is how a fix whose reports were archived
still credits them in the Fixed column (without a link); archived reports leave
the reports column. Their thumbnails are deleted with them.

It runs monthly from `pdghostd-archive.timer` (the 1st, 04:30 UTC,
`Persistent=true`), a oneshot unit with the same walls as the server:

```sh
sudo cp tools/pdghostd/pdghostd-archive.service tools/pdghostd/pdghostd-archive.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now pdghostd-archive.timer
systemctl list-timers pdghostd-archive.timer
```

Read an archived report with `tar -xJf ~/pdghosts/reports-archive/2026-09.tar.xz 20260917-104752-a27712e9.txt -O | less`.

## Changing it

Schema changes run from `init_db()` on startup, against a live database that is
the only one there is — back it up first. Migrations that create a unique index
must delete the rows that would violate it beforehand, or the service fails to
start and takes the board down with it.

Run `test_pdghostd.py` before deploying — see **Testing it** above. It patches
`ROOT` and `PORT` into a copy the same way this does, and a change worth making
is worth a case there:

```sh
sed -e 's|^PORT = 8090|PORT = 8391|' \
    -e 's|^ROOT = .*|ROOT = os.path.abspath("testroot")|' \
    pdghostd.py > testd.py
```
