#!/usr/bin/env python3
"""
The Latin characters the game can draw beyond ASCII, and how each is made.

Perfect Dark's NTSC fonts have the 94 printable ASCII characters and nothing
else. port/src/langfont.c builds every other Latin letter at font load out of
those: the ROM's own glyph for the base letter plus a diacritic drawn per font
size, two glyphs joined (ae, oe, guillemets), a glyph turned over (inverted
marks) or a stand-in. This file is the one list of what it can make, so that
the pack checker (check.py) and the game agree:

    python3 tools/langpack/latin.py --header port/src/langfonttable.h

writes the table langfont.c reads. Run it again after changing the list and
commit both.
"""

import sys
import unicodedata

# The marks langfont.c draws, in the order of its LANGFONT_MARK_* numbers.
MARKS = [
    ('ACUTE', 0x0301),
    ('GRAVE', 0x0300),
    ('CIRCUMFLEX', 0x0302),
    ('DIAERESIS', 0x0308),
    ('TILDE', 0x0303),
    ('RING', 0x030a),
    ('CEDILLA', 0x0327),
    ('CARON', 0x030c),
    ('BREVE', 0x0306),
    ('MACRON', 0x0304),
    ('DOTABOVE', 0x0307),
    ('DOUBLEACUTE', 0x030b),
    ('OGONEK', 0x0328),
    ('COMMABELOW', 0x0326),
]
MARK_BY_CP = {cp: i + 1 for i, (name, cp) in enumerate(MARKS)}

KIND_MARK = 0     # base ASCII glyph + up to two marks
KIND_JOIN = 1     # two ASCII glyphs side by side, one column shared
KIND_TURN = 2     # an ASCII glyph turned half round, hung below the x-height
KIND_FOLD = 3     # drawn as another ASCII character
KIND_DRAWN = 4    # a glyph of its own, drawn per font size (sharp s)
KIND_MARKONLY = 5 # a lone mark on no letter (degree sign, spacing accents)

# Characters with no canonical decomposition, or that are better made another way.
SPECIAL = {
    0x00a0: (KIND_FOLD, ' '),        # no-break space
    0x202f: (KIND_FOLD, ' '),        # narrow no-break space (French before ; : ! ?)
    0x2009: (KIND_FOLD, ' '),        # thin space
    0x00a1: (KIND_TURN, '!'),
    0x00bf: (KIND_TURN, '?'),
    0x00ab: (KIND_JOIN, '<<'),
    0x00bb: (KIND_JOIN, '>>'),
    0x2039: (KIND_FOLD, '<'),
    0x203a: (KIND_FOLD, '>'),
    0x00c6: (KIND_JOIN, 'AE'),
    0x00e6: (KIND_JOIN, 'ae'),
    0x0152: (KIND_JOIN, 'OE'),
    0x0153: (KIND_JOIN, 'oe'),
    0x2026: (KIND_JOIN, '..'),       # ellipsis, as close as one glyph gets
    0x201e: (KIND_JOIN, ',,'),
    0x00df: (KIND_DRAWN, 's'),
    0x1e9e: (KIND_DRAWN, 'S'),
    0x00b0: (KIND_MARKONLY, 'RING'),
    0x00b4: (KIND_MARKONLY, 'ACUTE'),
    0x00a8: (KIND_MARKONLY, 'DIAERESIS'),
    0x02c6: (KIND_MARKONLY, 'CIRCUMFLEX'),
    0x02dc: (KIND_MARKONLY, 'TILDE'),
    0x00b8: (KIND_MARKONLY, 'CEDILLA'),
    0x2018: (KIND_FOLD, "'"),
    0x2019: (KIND_FOLD, "'"),
    0x201a: (KIND_FOLD, ','),
    0x201c: (KIND_FOLD, '"'),
    0x201d: (KIND_FOLD, '"'),
    0x2013: (KIND_FOLD, '-'),
    0x2014: (KIND_FOLD, '-'),
    0x2010: (KIND_FOLD, '-'),
    0x2011: (KIND_FOLD, '-'),
    0x2212: (KIND_FOLD, '-'),
    0x00b7: (KIND_FOLD, '.'),
    0x2022: (KIND_FOLD, '*'),
    0x00d7: (KIND_FOLD, 'x'),
    0x00aa: (KIND_FOLD, 'a'),
    0x00ba: (KIND_FOLD, 'o'),
    0x00b2: (KIND_FOLD, '2'),
    0x00b3: (KIND_FOLD, '3'),
    0x00b9: (KIND_FOLD, '1'),
    0x00a9: (KIND_FOLD, 'C'),
    0x00ae: (KIND_FOLD, 'R'),
    0x2122: (KIND_FOLD, 'T'),
    0x00d8: (KIND_FOLD, 'O'),        # no stroke is drawn: the bare letter
    0x00f8: (KIND_FOLD, 'o'),
    0x0141: (KIND_FOLD, 'L'),
    0x0142: (KIND_FOLD, 'l'),
    0x0110: (KIND_FOLD, 'D'),
    0x0111: (KIND_FOLD, 'd'),
    0x0126: (KIND_FOLD, 'H'),
    0x0127: (KIND_FOLD, 'h'),
    0x0131: (KIND_FOLD, 'i'),        # dotless i is drawn as i; see langfont.c
    0x00d0: (KIND_FOLD, 'D'),
    0x00f0: (KIND_FOLD, 'd'),
    0x00de: (KIND_FOLD, 'P'),
    0x00fe: (KIND_FOLD, 'p'),
    0x20ac: (KIND_FOLD, 'E'),
    0x00a3: (KIND_FOLD, 'L'),
    0x00a5: (KIND_FOLD, 'Y'),
}

RANGES = [(0x00a0, 0x0180), (0x0218, 0x021c), (0x1e9e, 0x1e9f), (0x2010, 0x2027),
          (0x2039, 0x203b), (0x20ac, 0x20ad), (0x2122, 0x2123), (0x2212, 0x2213),
          (0x02c6, 0x02c7), (0x02dc, 0x02dd), (0x202f, 0x2030), (0x2009, 0x200a)]


def recipe(cp):
    """(kind, text, marks) for a code point, or None when it cannot be drawn."""
    if cp in SPECIAL:
        kind, arg = SPECIAL[cp]
        if kind == KIND_MARKONLY:
            return (kind, ' ', [MARK_BY_CP[dict(MARKS)[arg]]])
        return (kind, arg, [])

    d = unicodedata.normalize('NFD', chr(cp))

    if len(d) < 2 or not (0x21 <= ord(d[0]) <= 0x7e):
        return None

    marks = []

    for c in d[1:]:
        if ord(c) not in MARK_BY_CP:
            return None
        marks.append(MARK_BY_CP[ord(c)])

    if len(marks) > 2:
        return None

    return (KIND_MARK, d[0], marks)


def table():
    rows = {}
    for lo, hi in RANGES:
        for cp in range(lo, hi):
            r = recipe(cp)
            if r:
                rows[cp] = r
    return rows


def drawable(ch):
    """Whether the game can draw this character in a Latin pack."""
    cp = ord(ch)
    return ch == '\n' or 0x20 <= cp <= 0x7e or cp in TABLE


TABLE = table()


def write_header(path):
    out = []
    out.append('// Generated by tools/langpack/latin.py - do not edit, run it again.')
    out.append('// code point, kind, ASCII text, mark 1, mark 2')
    out.append('')
    for i, (name, cp) in enumerate(MARKS):
        out.append('#define LANGFONT_MARK_%s %d' % (name, i + 1))
    out.append('#define LANGFONT_NUM_MARKS %d' % (len(MARKS) + 1))
    out.append('')
    out.append('static const struct langfontrecipe g_LangFontRecipes[] = {')
    for cp in sorted(TABLE):
        kind, text, marks = TABLE[cp]
        m = marks + [0, 0]
        esc = text.replace('\\', '\\\\').replace("'", "\\'").replace('"', '\\"')
        out.append('\t{ 0x%04x, %d, "%s", %d, %d }, // %s' % (cp, kind, esc, m[0], m[1],
                   unicodedata.name(chr(cp), '?').lower()))
    out.append('};')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--header':
        write_header(sys.argv[2])
    else:
        for cp in sorted(TABLE):
            print('%04x %s %r' % (cp, chr(cp), TABLE[cp]))
