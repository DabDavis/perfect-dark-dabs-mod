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
