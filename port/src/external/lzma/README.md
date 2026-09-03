# LZMA SDK (7z reader)

Igor Pavlov's LZMA SDK, public domain, cut down to what reading a `.7z`
needs — no encoder, no xz, no threading. Used by `port/src/archive.c` to
unpack texture packs.

Taken from the copy Project64 carries in `Source/3rdParty/7zip`. Left
as it came apart from being reduced to these files, so it can be diffed
against upstream; nothing here should be edited to fit this codebase.
