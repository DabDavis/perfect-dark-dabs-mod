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
