# Stage numbers

Adding stages at runtime is constrained from several directions at once:

- `stagenum < STAGE_TITLE` (0x5a) is the engine's "is this a real level" test, in
  16 places. At or above it, `setupLoadFiles()` silently does nothing.
- `STAGE_TITLE`, `STAGE_BOOTPAKMENU`, `STAGE_CREDITS`, `STAGE_4MBMENU` are used
  outside `g_Stages`, so scanning the table alone will not show them as taken.
- `langGetLangBankIndexFromStagenum()` must know the stage (now falls back safely).
- `g_StageAllocations8Mb` has no entry for runtime stages, so they get a default
  allocation. Too small means `MEMPOOL_STAGE` exhaustion surfacing far from the cause.

Only 27 ids are free below `STAGE_TITLE`.

Since 2026-09-07 the 16 "real level" tests ask `STAGE_IS_LEVEL(stagenum)`
(constants.h), which is true below `STAGE_TITLE` and again from
`STAGE_4MBMENU + 1` to `STAGE_MAX_ID` (0xff): a stage number is a byte
wherever the game keeps one (`g_MpSetup.stagenum`, `struct missionconfig`),
and the MP setup save format's 7-bit field writes Random for an id past 0x7f
(`mpsetupfileSave`, in mplayer.c) rather than a truncated one. So 189 ids
are usable, and `MAX_MODSTAGES` (189) gives the tables room for every one.

The Stage Loader (mods.md, "The Stage Loader") hands ids out above the table
first (0x51-0x59), then the gaps below it, then the high range; a whole mod
archive's maps fit at once.
