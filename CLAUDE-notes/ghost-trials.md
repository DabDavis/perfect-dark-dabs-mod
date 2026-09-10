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

## An account is a name, a PIN and a security question

Four endpoints take credentials: `register`, `login`, `setrecovery` and
`resetpin`. All but `login` carry the question with them, and the `pin` field
means the account's PIN in every one except `resetpin`, where it is the PIN the
account is to have - which is why the Reset PIN page types into the same box
the account page does.

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

The daemon (`pdghostd.py`, on the leaderboard host and not in this tree)
accepts a registration with no question, because builds that predate the page
cannot send one. Those accounts get a reset refused with the same sentence a
wrong answer gets, and their owners set a question from the page once they
update - which needs the PIN, and so needs them.
