# Ghost Trials talks over two different transports

## Digest (moved from CLAUDE.md, 2026-09-30)

The entries CLAUDE.md carried for this note, verbatim. The sections below are
the long form.

- **Ghost Trials networking** — [ghost-trials.md](CLAUDE-notes/ghost-trials.md): WinHTTP and libcurl, why not one of them, and what the worker thread may touch; the three security questions' wire format and `rec_count`; the red Streamer Beware door and the Offline/Online page; **the pd.ini key is `Mod.GhostServer`** - point a scratch ini at a local daemon before any headless drive that presses Create Account
- **One Sign In button** — a new name makes the account (`/login` "create", `ghostnetSignIn`, the questions only for a new name); the server no longer counts a new account as a failed sign-in; the on-screen keyboard on a PC (types at once, Enter OK, Escape CANCEL, held keys latched, name/PIN charsets and the masked PIN)


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
reset pages, and those open through the red **Streamer Beware!** dialog
(`g_GhostSensitiveMenuDialog`, `MENUDIALOGTYPE_DANGER`) every time - once a
run would miss the streamer who started streaming after dismissing it. A row
that opens a dialog has no handler, so the doors are rows with a
handler that pushes the warning and remembers the page for its Show It row,
which closes the warning first (`menuitemSelectableTick` pops before it calls)
and then pushes the page. The nag from the Ghost Trials tick goes through the
same door. The PIN keyboard used to be a third door, for showing the digits
as they were typed; it draws them as `*`s now (`KEYBOARDFLAG_MASKED`) and
opens directly, empty, since a masked PIN is retyped rather than edited.

## One Sign In button: a new name makes the account (2026-10-08)

Players could not tell they had to make an account first. The account page
had Create Account and Sign In side by side, Create greyed until all three
questions were picked, so the one lit button was Sign In - and the live
server's log shows new players pressing it again and again ("wrong name or
pin"), then pressing Save To Account on the questions page (refused: no
account yet), then finding Create Account. The user asked for a sign-in with
a name that does not exist to make it.

- The page is Name, PIN, Sign In, Forgot My PIN. Sign In is
  `ghostnetSignIn()` (`JOB_SIGNIN`): `/login` with `"create": true`, plus the
  three pairs once `ghostnetRecoveryIsSet()`. An existing account signs in
  and never sees the questions; the server ignores pairs sent to one.
- A free name sent without pairs answers `404 "new": true`; the client keeps
  it (`ghostnetIsNewName()`, valid while the name box still holds it), the
  account page's tick pushes the questions through the Streamer door (whose
  first two lines then say a new account needs them), and the questions
  page's button reads Create Account and sends the same Sign In with them.
  The Sign In button itself reads Create Account in that state.
- A sign-in pressed on the account or questions page closes them when it has
  worked (`g_GhostSignInPending`, one pop a tick, the questions page first),
  so the player is back on Online Game or the accounts list.
- Neither button greys while busy: a greyed row loses the cursor, which then
  sat on Forgot My PIN when the answer came back.
- The Security Questions row is hidden unless signed in or the name is known
  to be new.
- `ghostnetLogin()` stays the plain sign-in (no create): choosing a
  remembered account, and Ghost Trials' sign-in on open, never make one.

**A list next to other rows was a cursor trap.** `menuitemListTick` wraps
at both ends and keeps Up and Down to itself while it has entries - right
for a list that is a dialog's only row, a trap anywhere else: on the
accounts list Up from Back went into the list and nothing but the mouse or
Back got out (Add Account and Name And PIN unreachable). A port list with
rows around it carries `MENUITEMFLAG_LIST_LEAVEATENDS` (0x40000000, port
only): Up on its first entry and Down on its last are left to the dialog,
which moves the focus off it. Set on the accounts list, Choose Ghosts, My
Ghosts, Leaderboards, the Briefing Room's rooms and the kick list; the game's
own lists still wrap. The accounts list's status no longer says "No account
on this machine yet" over a list of names whose active one has no PIN.

**The server counted a new account as a failed sign-in.** `/register`'s
failure-budget check recorded, so a player who had pressed Sign In seven
times made the account with the eighth and was refused "too many attempts"
at every sign-in for five minutes ("Roku", 2026-10-05: four refused
sign-ins, three refused Save To Accounts, register 200, five 403s, then 429
on Create). See tools/pdghostd/README.md: registering now asks the budget,
clears it on success, and has its own cap (`REGISTER_MAX`, 20 an hour).

**The on-screen keyboard on a PC (all of them, not only these pages).** Typing
needed TYPE WITH KEYBOARD (or I) first, and letters before it were menu
buttons: `E` is a second Accept, so "Eddie" typed into the name page came
out as "e". In typing mode Enter did not confirm (upstream ccb3c8668 took it
out because the Enter also pressed the menu under the keyboard), Escape left
typing mode under a hint reading "ESC: OK", and a second Escape closed the
keyboard and threw away the text. Now:

- a keyboard opened by a key or mouse press starts typing
  (`inputLastPressWasKeyboard()`, `menuitemKeyboardInit`); a pad's player
  gets the grid;
- Enter is OK, Escape (or back) is CANCEL; Enter's key repeats are not
  presses, so the Enter that opened a keyboard cannot confirm it;
- keys held when typing stops are latched up until released
  (`inputStopTextInput`, `inputKeyPressed`), so the confirming Enter does not
  press the menu underneath - that was upstream's reason;
- while typing, only the keyboard's binds are off; a pad and the mouse still
  work the grid (`inputBindPressedNotKeys`);
- every way the keyboard closes itself stops the typing (a mouse click on OK
  left it on, and the menu under it then took no keys);
- `KEYBOARDFLAG_NAME` / `_DIGITS` / `_MASKED` in a keyboard item's param2
  (port only) filter typed and grid characters (`inputTextCharAllowed`) and
  mask the field. The name keyboard is 15 characters and full width (it
  stopped at 10, the item's param 0, under a label saying 15) and takes `_`
  and `-`, which the old filter dropped.

**Testing it headless:** Xvfb + xdotool against a local pdghostd and pdlobbyd
(`--auth ghost --ghost-url` the local pdghostd), scratch savedir with
`GhostServer`/`LobbyServer` pointed at them. `xdotool type` reaches the game
once typing has started. Restart the local pdghostd after editing it: a copy
made before the change answers the old way (a 403 where a 404 was expected).

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
