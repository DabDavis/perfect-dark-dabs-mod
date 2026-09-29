#ifndef _IN_LANGPACK_H
#define _IN_LANGPACK_H

#include <PR/ultratypes.h>

/**
 * Languages: every language but the ROM's English is a pack of our own
 * translations (CLAUDE-notes/languages.md). Packs are built from lang/<code>/
 * by tools/langpack/build.py and embedded in the binary; a <code>.lang dropped
 * in the executable's lang/ folder adds one (or replaces a built-in one of the
 * same code). The choice is Game.Language in pd.ini, a code, empty for the
 * ROM's English; --lang <code> overrides it for one run.
 *
 * A pack need not be complete: whatever it leaves out is the ROM's English
 * (English (UK) is a handful of British spellings and nothing else).
 */

#define LANGPACK_SCRIPT_LATIN 0
#define LANGPACK_SCRIPT_CJK   1

// Reads the embedded packs and the lang/ folder, and selects the configured
// language. After configInit().
void langpackInit(void);

// The languages for the dropdown. Index 0 is the ROM's English (US).
s32 langpackGetCount(void);
const char *langpackGetName(s32 index);   // in the language itself, UTF-8
const char *langpackGetCode(s32 index);   // "" for index 0
s32 langpackGetSelected(void);

// Switches language now: every string read after this is the new one. Saves
// it as Game.Language when save is set (the menu does; --lang does not).
void langpackSelect(s32 index, s32 save);

// Bumped by every switch, so a cache of laid-out text knows to redo it.
u32 langpackGeneration(void);

// Whether a pack is selected at all; nothing else is worth asking when not.
s32 langpackActive(void);
s32 langpackScript(void);

// Whether the selected pack is Japanese-style (the JPN ROM's menu layout).
s32 langpackIsCjk(void);

// Whether the selected pack draws any letter the ROM's fonts lack, which is
// when PAL's one extra row of line spacing is wanted for the accents.
s32 langpackNeedsAccentSpacing(void);

// A Perfect Dark text id (bank * 512 + row) in the selected language, or
// NULL for the ROM's own. The caller decides whether the ROM's own is stock
// (a mod's own text is never replaced).
const char *langpackPd(s32 textid);

// A GoldenEye string by its language file and slot (ge.<bank>.<slot>: bank is
// the file's name less its L and E, "title" for LtitleE, "dam" for LdamE;
// slot is GoldenEye's text id & 0x3ff), or by its English. NULL when the
// pack has none.
const char *langpackGe(const char *bank, s32 slot);
const char *langpackGeText(const char *en);
// The same by the bank file's own name ("LdamE", "LtitleE").
const char *langpackGeFile(const char *file, s32 slot);

// A string of the port's own, by its English exactly as in the source; the
// English itself when the pack has no translation. A trailing newline is
// kept as the English has it whether or not the pack wrote one. ctx picks
// between two translations of one English ("ctx|English" in port.json) and
// may be NULL.
const char *langTr(const char *en);
const char *langTrCtx(const char *ctx, const char *en);

// Same, but NULL rather than the English when there is no translation.
const char *langTrFind(const char *en);

// A string of the port's kept in a table and translated where it is used
// (langTr() on the table's entry): the marker tools/langpack/extract.py
// finds it by. It does nothing.
#define LANG_N(s) (s)

// --lang-log-missing (CLAUDE-notes/languages.md): s was handed out by a
// lookup (translated or the English kept), so drawing it is not a leftover.
// Answers s. Costs a branch when the option is off.
const char *langpackNoted(const char *s);
// dst was made from src (wrapped, copied): noted if src was.
void langpackNoteDerived(const char *src, const char *dst);
// A text loop is about to draw s: logs it once if no lookup handed it out.
void langpackCheckDrawn(const char *s);
// Logs a keyed string (ge.dam.5, pd.0203) the pack has no translation of.
void langpackLogMissingKey(const char *key, const char *en);

// For --lang-audit (lang.c): the selected pack's keyed strings in turn.
const char *langpackEntry(u32 i, const char **key, s32 *ns);
const char *langpackSelectedCode(void);

#endif
