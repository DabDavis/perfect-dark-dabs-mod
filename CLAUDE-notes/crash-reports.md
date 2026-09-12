# Crash reports

What a player's crash looks like from this end, and how it gets here.

Before this, a crash reached us as a screenshot of the dialog: an exception
code and a stack of `[base]+offset` lines. That is enough to name the function
(CLAUDE.md, **Debugging**, has the `addr2line` recipe) and nothing else. The
v3.4.0 report that started this - an access violation in `weatherResetRooms` -
could not be reproduced here on any stage, with or without the mod, under ASan
or under wine, because what was missing was never in the dialog: which mission,
which mod, which switches, and what the log said on the way in.

## The two halves

**Writing** is `crashReportSave()`, called from `sysFatalError()` - so every
fatal error gets one, not only an access violation. It composes and writes;
it does not transmit. The file lands in `$S/crashreports/crash-<stamp>.txt`
and holds:

- the build (`sysGetVersionString()`), the channel, the time, the mod directory
- the dialog's own text: the exception, the stack, and on Windows the `FAULT:`
  line naming the address and whether it was read or written
- the `[Mod]` section of pd.ini, from `configDumpSection()` - the **live**
  values rather than the file's, because a setting changed from the menu since
  the last save is exactly the sort of thing worth knowing
- the last `CRASHREPORT_LOGLINES` (200) lines of the log

**Sending** is `crashReportSend()`, which POSTs to `<Mod.GhostServer>/crash`
through `ghostnetSend()` - the same transport the ghost client and the updater
use, so there is one WinHTTP half and one libcurl half in the port and not
three. A report that is sent is deleted locally; one that is not stays.

## The log is a ring, not the file

`pd.log` **only exists when the game was started with `--log`**, which no
player does. So the report does not read a file: `sysLogPrintf()` copies every
line it formats into a fixed ring in crashreport.c, and the report writes that
out. This is the only copy of those lines an ordinary player has, and it is why
a report from somebody who has never touched the command line still says which
stage was loading.

## Where the player says yes

Two places, and neither one sends anything on its own:

- **The crash dialog** grew a *Send report to Dab* button (`SDL_ShowMessageBox`
  with two buttons in place of `SDL_ShowSimpleMessageBox`). It sends with no
  note, because a message box cannot take text - and the player about to close
  a crashed game is the one who will say yes now and never again. The report is
  on disk **before** the box goes up, so Close loses nothing.
- **Crash Reports**, on the Perfect Menu next to Check for Updates
  (`port/src/crashreportmenu.c`). The row reads *Send Crash Report* when one is
  waiting. This is where a note can be typed, and where a failed send is
  retried. The send is on a worker thread and the page polls it, the way Check
  for Updates and the ghost menus do theirs.

The dialog and the page both say what is in a report before offering to send
it. A player deciding whether to hand over their log should be told that is
what it is.

## Sending from inside a crash

The dialog's button does network I/O in a process that has already crashed -
on Linux, inside a signal handler. That is a real risk and it is taken
knowingly: the file is written first, so the worst case is a second crash with
the report still on disk for the menu to offer next time. Do not add anything
to that path that is not needed to send bytes.

## The other end

`tools/pdghostd/pdghostd.py`, `POST /crash`, documented in its README under
**Crash reports**: one file per report under `~/pdghosts/crashes`, no account,
no endpoint that reads one back, twelve an hour from an address, 32 KiB of
report and 200 characters of note, and a cap on the directory. Run
`test_pdghostd.py` before deploying it - the crash cases are in there.
