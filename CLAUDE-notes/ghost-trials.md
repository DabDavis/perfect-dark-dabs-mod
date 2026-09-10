# Ghost Trials talks over two different transports

`port/src/ghostnet.c` has one seam, `ghostnetSend()`, and two implementations
behind it. Windows uses **WinHTTP**, which is part of the OS: nothing to ship and
certificates are the system's business. Everywhere else uses **libcurl**, there
being no system HTTP API to use instead — macOS has it in the SDK, Linux wants
`libcurl4-openssl-dev`.

Do not "simplify" this back to one backend. libcurl on Windows means shipping a
dozen DLLs *and* answering for a CA bundle OpenSSL looks for at a compile-time
path no player's machine has — which fails on Windows only, while Linux and macOS
work perfectly. The CI packaging step checks the transport survived on each
platform, because the build degrades to "network support is not built into this
copy" rather than failing, and that silently shipped for a while.

The WinHTTP half can be built and run from Linux: extract it with the mingw
compiler into a harness and run it under wine against a local `pdghostd`.

**The worker thread touches nothing the menu owns.** A job is decided on the main
thread and carried out on the worker, which reads only the `g_Job*` snapshot.
Uploading used to rebuild the ghost catalogue from the worker while the page that
started it was drawing rows out of that array. `fsFullPath()` is `_Thread_local`
for the same reason — it is a single scratch buffer every file call expands into.

## An account is a name, a PIN and three security questions

Four endpoints take credentials: `register`, `login`, `setrecovery` and
`resetpin`. All but `login` carry the questions with them, and the `pin` field
means the account's PIN in every one except `resetpin`, where it is the PIN the
account is to have - which is why the Reset PIN page types into the same box
the account page does.

**Three pairs since 2026-09-10, on the wire as `question`/`answer`,
`question2`/`answer2`, `question3`/`answer3`.** The first pair keeps the names
it had when it was the only one, so a build from before sends one and is
stored as one, and a server from before stores the first and ignores the rest
(proved against the live server by accident - see the testing section). What
the server hashes is every pair joined in order, `q|a|q|a|q|a`, which for one
pair is byte-for-byte the old hash, and `rec_count` beside it says how many
went in (the migration sets it to 1 wherever a hash already was). A reset
hashes the first `rec_count` of the pairs it is sent, so a three-question
account needs all three and a one-question account is reset by its one
whatever a newer client filled the other two with; fewer than the account
holds is refused as a wrong answer. The client requires all three, from
different categories, before Create Account or Save To Account will press
(`ghostnetRecoveryIsSet()`), and one pair before Reset PIN will
(`ghostnetRecoveryCount()`), because the reset page cannot know how many the
account has. A sign-in reply now carries `"questions": n` beside `recovery`;
fewer than three is `GHOSTNET_RECOVERY_PARTIAL`, which nags once a run the
way a missing question does. Absent means "as many as it could have", for the
same reason absent `recovery` means unknown.

**The account page names nothing.** Its row reads `(set)`, `(n of 3)` or
`(not set)`; the categories and answers are shown only on the questions and
reset pages, and those, and the PIN keyboard (which shows the digits as they
are typed), open through the red **Streamer Beware!** dialog
(`g_GhostSensitiveMenuDialog`, `MENUDIALOGTYPE_DANGER`) every time - once a
run would miss the streamer who started streaming after dismissing it. A row
that opens a dialog has no handler, so the three doors are rows with a
handler that pushes the warning and remembers the page for its Show It row,
which closes the warning first (`menuitemSelectableTick` pops before it calls)
and then pushes the page. The nag from the Ghost Trials tick goes through the
same door.

**Offline or Online is asked in front of Ghost Trials every time**
(`g_GhostModeMenuDialog`, what the main menu row opens now). Offline: no
sign-in on open, no account page pushed, no nag, and the Ghost Account, Ghost
Share and Leaderboards rows are greyed (rows with a handler, for the same
reason as above); a trial records and races exactly as before. Online is the
old behaviour. Nothing remembers the choice.

**Being signed in is a fact about the server, not about the boxes.** The page
said "Signed in as X" for anything that merely matched the server's rules for a
name and a PIN, so a player who never pressed Create Account was told he had an
account and got "wrong username or pin" from Upload - which is what the server
answers for an account that does not exist, deliberately, since the difference
is the list of usernames. `ghostnetIsSignedIn()` compares the boxes against the
pair a reply actually accepted. Nothing clears that pair on a failure: 403
covers the throttle as well as a bad PIN.

**The security question's ids are a wire format and are frozen.**
`port/include/ghostrecovery.h` holds ten categories of about fifty answers, and
what the server stores is a hash of `"<category id>|<answer id>"` with a salt -
not the category, so a guesser has to find that too. Renaming or removing an
id, or reordering a list so a name maps to a different id, locks out everybody
who chose it. Append to the end; correct display names freely.

Ten times fifty is not a password, and the reset endpoint's limiter is what
actually guards it: five wrong answers a day at one account, ten attempts an
hour from one address, every attempt delayed before it is checked, and a
successful sign-in clears the account's budget so that a stranger's guessing
cannot stand between its owner and their own recovery.

The daemon (`tools/pdghostd/pdghostd.py`, deployed to the leaderboard host and
kept byte-identical to it) accepts a registration with no question, because
builds that predate the page cannot send one. Those accounts get a reset
refused with the same sentence a wrong answer gets.

## Testing the client against a daemon of your own

The pd.ini key is **`Mod.GhostServer`**, not GhostUrl, and a scratch pd.ini
copied from the real one carries the live server's URL on that line - so an
online drive with the wrong key, or with the key added above the existing
line, talks to production. That is how a `tester`/`1234` account with one
question came to exist on the live board on 2026-09-10. Before any headless
drive that presses Create Account or Sign In: `sed -i
's#^GhostServer=.*#GhostServer=http://127.0.0.1:8393#'` on the scratch ini
(every occurrence), and run a copy of the daemon with `PORT` and `ROOT`
patched the way `test_pdghostd.py` patches its copy. The daemon's log lists
every request it saw; if it shows only `/ping`, the game went somewhere else.

**Their owners are told once a run.** Nothing about such an account looks
different from the outside, so a sign-in reply carries `"recovery": true|false`
— answered only to somebody who has just proved they hold the PIN, which is
what makes it safe to say — and the client keeps it beside the pair the server
accepted. A missing field means *unknown*, not *missing*, so a build talking to
an older server nags nobody. When it is missing, the Ghost Trials dialog pushes
the Security Question page on the tick after the answer lands (not in
`MENUOP_OPEN` — the sign-in is a frame or more away), once per run whether or
not they set one, and only while Ghost Trials is the dialog on top: a tick
reaches every dialog on the stack, and pushing from underneath would open the
page over whatever the player had gone on to open.
