#!/usr/bin/env python3
"""Generate the GoldenEye -> Perfect Dark AI command map.

Perfect Dark's AI is GoldenEye's AI with commands inserted, so the two command
sets are the same list in the same order with gaps: 253 commands against 482,
one opcode byte against two. The map is found from two signals and then read by
hand:

  * **the two command tables aligned in order** (Needleman-Wunsch over the
    names and the argument widths, geaicmds.py and pdaicmds.py), which is the
    systematic signal and covers commands no level happens to use;
  * **GE-X as an oracle** - its own converted ai lists beside GoldenEye's, list
    by list for the twenty missions, which says what a real conversion did.

Where they agree the row is taken as it stands. Where they do not, or where
Perfect Dark's command takes arguments GoldenEye's does not (Perfect Dark made
"Bond" an explicit chr on a dozen commands), the row is in OVERRIDE below with
the reason. Writes tools/geconvert/geaitable.py and port/include/geaitable.h,
both of them literal tables meant to be read.

    python3 tools/geconvert/fit/genaitable.py -o OUTDIR

Historical: tools/geconvert/geaitable.py and port/include/geaitable.h have been
kept by hand since, so this writes OUTDIR/geaitable.py and OUTDIR/geaitable.h
for comparison and never over the repo's own.
"""
import collections
import difflib
import os
import re
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import paths

import geaicmds
import pdaicmds
import gefiles
import gexdata
import gexprops

GE = geaicmds.table()
PD = pdaicmds.table()
PDNAME = {op: n for op, n, l in PD}
PDLEN = {op: l for op, n, l in PD}

# The chr a GoldenEye command means when it does not name one. Perfect Dark's
# CHR_BOND (0xf8) is the legacy one; the player in a Perfect Dark mission is
# CHR_P1P2, which is what GE-X writes for GoldenEye's implicit Bond.
CHR_BOND = 0xf2   # CHR_P1P2
CHR_SELF = 0xfd

# The bank the port keeps GoldenEye's own chr flags byte in (constants.h,
# BANK_GE): chrdata.geflags2, which nothing but a converted list ever reads.
BANK_GE = 2

# A row is `pd opcode, (argument specs)`; each spec makes one Perfect Dark
# argument out of GoldenEye's:
#
#   n            GoldenEye argument n, at its own width
#   (n, w)       GoldenEye argument n, written as w bytes
#   ('=', v, w)  the constant v, w bytes wide
#
# None drops the command: GoldenEye has it and Perfect Dark does not, so the
# converted list leaves it out and geconvert counts it.
#
# The default row - not written here - is Perfect Dark's aligned command with
# every GoldenEye argument copied in order, which is what 221 of the 253 are.
OVERRIDE = {
    # Perfect Dark's is aiSetReturnList(chr, list); GoldenEye's is the chr's own
    0x06: (0x0006, (('=', CHR_SELF, 1), 0)),
    # the animation is GoldenEye's own and the remake's chrs do not have it, so
    # the set pieces are left out rather than played as some other animation
    0x0a: None,
    0x0b: None,
    # GoldenEye hits a chr with an item it names; Perfect Dark's 0x19 is not the
    # same command
    0x18: None,
    # aiConsiderGrenadeThrow(range(2), range2(2), label); GE-X writes 0x0200 and
    # 0 for the two ranges whatever the list
    0x1a: (0x001b, (('=', 0x0200, 2), ('=', 0, 2), 0)),
    # aiGoToPadPreset(mode) against GoldenEye's, which has no argument
    0x1d: (0x001e, (('=', 0, 1),)),
    # a clear line to the player, which Perfect Dark asks of its target
    0x3c: (0x003f, (0,)),
    # GoldenEye asks about the tile the player stands on; the line to the player
    # is the nearest thing Perfect Dark has
    0x3d: (0x003f, (0,)),
    # aiIfChrInOnScreenRoom(chr, label)
    0x43: (0x0048, (('=', CHR_SELF, 1), 0)),
    # aiIfChrInRoom(chr, mode, pad(2), label)
    0x54: (0x005b, (0, ('=', 0, 1), 1, 2)),
    # Perfect Dark names the chr; GoldenEye means the player
    0x56: (0x005d, (('=', CHR_BOND, 1), 0, 1)),
    0x59: (0x0060, (('=', CHR_BOND, 1), 0, 1)),
    0x5c: (0x0063, (('=', CHR_BOND, 1), 0, 1)),
    # a gadget used on an object is Perfect Dark's "activated" too
    0x5d: (0x0063, (('=', CHR_BOND, 1), 0, 1)),
    # aiIfObjectHealthy(tag, label) is "not destroyed"
    0x5b: (0x0062, (0, 1)),
    # aiObjInteract(tag)
    0x5e: (0x0065, (0,)),
    # aiIfStageTimerLessThan(seconds(2), label)
    0x72: (0x0079, (0, 1)),
    # aiIfStageTimerGreaterThan(seconds(2), label)
    0x73: (0x007a, (0, 1)),
    # aiIfInjured(chr, label): Perfect Dark folds "damaged since last check" in
    0x7e: (0x0083, (0, 1)),
    # aiIfChrHealthLessThan/GreaterThan(chr, health, label)
    0x7f: (0x0082, (('=', CHR_BOND, 1), 0, 1)),
    0x80: (0x0081, (('=', CHR_BOND, 1), 0, 1)),
    # aiIfChrAlertnessLessThan(value, chr, label) - the value comes first
    0x89: (0x008f, (0, ('=', CHR_SELF, 1), 1)),
    # GoldenEye's chr flags are one byte of its own, and neither of Perfect
    # Dark's two banks has eight bits to spare - every one of theirs means
    # something to the game. The byte gets a bank of its own instead
    # (chrdata.geflags2, BANK_GE), so these are Perfect Dark's own six flag
    # commands with the bank named: aiSetFlag(flags(4), bank),
    # aiIfHasFlag(flags(4), equal, bank, label) and the three-by-chr ones,
    # whose test is "any of these bits" exactly as GoldenEye's is.
    0x94: (0x009b, ((0, 4), ('=', BANK_GE, 1))),
    0x95: (0x009c, ((0, 4), ('=', BANK_GE, 1))),
    0x96: (0x009d, ((0, 4), ('=', 1, 1), ('=', BANK_GE, 1), 1)),
    0x97: (0x009e, (0, (1, 4), ('=', BANK_GE, 1))),
    0x98: (0x009f, (0, (1, 4), ('=', BANK_GE, 1))),
    0x99: (0x00a0, (0, (1, 4), ('=', BANK_GE, 1), 2)),
    # aiIfStageFlagEq(flags(4), equal, label); the objective bitfield is Perfect
    # Dark's stage flags, which the oracle writes the same way (34 of 48 votes)
    0x9c: (0x00a3, ((0, 4), ('=', 1, 1), 1)),
    # a hat: the number is GoldenEye's own hat, and Perfect Dark reads it as one
    # of its model numbers. Cradle's Trevelyan was handed Perfect Dark's model
    # 220 and died in modelasm00018680() the frame his hand was posed - the same
    # fault the hat *records* have, which are left out for the same reason.
    0xc0: None,
    # cloning a chr: Perfect Dark's aiDuplicateChr takes eight argument bytes
    # against GoldenEye's four and is not the same command
    0xc1: None,
    # --- the tail --------------------------------------------------------
    #
    # Perfect Dark added around 230 commands of its own after GoldenEye's
    # 0xd8, so from there the aligner has more Perfect Dark commands than
    # GoldenEye ones to spend and pairs them with whatever is left: it gave
    # IFObjectiveAllCompleted Perfect Dark's aiHovercopterFireRocket, which
    # crashed Egyptian and Cradle in chopperFireRocket the first time the
    # level logic asked whether the mission was done. The offset it walks at
    # is +10 up to 0xd8 and +22, +33, +63, +118 after it, so every row past
    # there is hand-written or dropped.
    0xd3: None,   # the camera goes back to Bond: a cutscene camera, not Perfect Dark's
    0xd4: None,   # and one that looks at him from a pad
    0xd7: (0x00e0, (0, ('=', 0, 1))),          # aiRevokeControl(flags, ?)
    0xd8: (0x00e1, (('=', 0, 1),)),            # aiGrantControl(flags)
    0xd9: None,   # teleport a chr to a pad
    0xda: None,   # the screen fades are GoldenEye's own, over its own cinema
    0xdb: None,
    0xdc: (0x01cc, (0,)),                      # aiIfFadeComplete(label)
    0xdd: (0x01d5, (('=', 0, 1),)),            # aiShowCutsceneChrs(show)
    0xde: (0x01d5, (('=', 1, 1),)),
    0xdf: (0x00e8, (0,)),                      # aiSetDoorOpen(tag)
    0xe0: None,   # take what the chr is holding out of its hand
    0xe1: (0x00ea, (0, 1)),                    # aiIfNumPlayersLessThan(n, label)
    0xe2: None,   # Perfect Dark's ammo test names a chr and a weapon slot
    0xe3: None,   # equipping a weapon for Bond, and the cinema's copy
    0xe4: None,
    0xe5: None,   # Bond's locked velocity, which Perfect Dark has no command for
    0xe7: None,
    0xe8: None,
    0xe9: (0x00f2, ()),                        # aiSwitchToAltSky()
    0xea: None,
    0xeb: None,
    0xec: None,
    0xed: None,
    0xef: None,   # the credits are GoldenEye's own
    0xf0: None,
    0xf1: (0x00f7, (0,)),                      # aiIfAllObjectivesComplete(label)
    0xf2: None,
    0xf3: None,
    0xf4: None,   # the music slots are GoldenEye's sequences
    0xf5: None,
    0xf6: (0x00fb, (('=', CHR_BOND, 1),)),     # aiChrExplosions(chr)
    0xf7: None,
    0xf8: (0x0083, (0, 1)),                    # aiIfInjured(chr, label)
    0xf9: (0x00fe, ()),                        # aiKillBond()
    # a debug print, variable length: the walk measures it and it is not converted
    0xad: None,
    # the text is GoldenEye's own bank and slot, which the port has no lang bank
    # for in a mission yet, so the messages are left out for now
    0xc2: None,
    0xc3: None,
    0xd8: None,
    0xe6: None,
    0xee: None,
    0xfa: None,
    0xfb: None,
    0xfc: None,
}


def norm(s):
    return re.sub(r'[^a-z0-9]', '', re.sub(r'^ai', '', s).lower())


def table_align():
    """Perfect Dark's command list is GoldenEye's with commands inserted."""
    gn = [(norm(n), (l - 1) if l else -1) for n, l, a in GE]
    pn = [(norm(n), (l - 2) if l else -1) for op, n, l in PD]
    n, m, gap = len(gn), len(pn), -0.6
    d = np.zeros((n + 1, m + 1))
    b = np.zeros((n + 1, m + 1), np.int8)
    d[0, :] = np.arange(m + 1) * gap
    d[:, 0] = np.arange(n + 1) * gap
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            s = 3.0 * difflib.SequenceMatcher(None, gn[i - 1][0], pn[j - 1][0]).ratio() - 1.0
            s += 1.2 if gn[i - 1][1] == pn[j - 1][1] else -1.5
            o = (d[i - 1, j - 1] + s, d[i - 1, j] + gap, d[i, j - 1] + gap)
            k = int(np.argmax(o))
            d[i, j] = o[k]
            b[i, j] = k
    i, j, out = n, m, {}
    while i > 0 or j > 0:
        if i and j and b[i, j] == 0:
            out[i - 1] = j - 1
            i -= 1
            j -= 1
        elif i and (not j or b[i, j] == 1):
            i -= 1
        else:
            j -= 1
    return out


# --- the oracle: GE-X's converted lists beside GoldenEye's -------------------

MISSIONS = [('UsetupdamZ', 0x30), ('UsetuparkZ', 0x33), ('UsetuprunZ', 0x22), ('UsetupsevxZ', 0x2c),
            ('UsetupsevbunkerZ', 0x1d), ('UsetupsiloZ', 0x1e), ('UsetupdestZ', 0x2f), ('UsetupsevxbZ', 0x24),
            ('UsetupsevbZ', 0x25), ('UsetupstatueZ', 0x27), ('UsetuparchZ', 0x31), ('UsetuppeteZ', 0x1c),
            ('UsetupdepoZ', 0x21), ('UsetuptraZ', 0x23), ('UsetupjunZ', 0x2d), ('UsetupcontrolZ', 0x34),
            ('UsetupcaveZ', 0x2a), ('UsetupcradZ', 0x2b), ('UsetupaztZ', 0x2e), ('UsetupcrypZ', 0x1a)]


def ge_lists(d):
    h = struct.unpack_from('>10I', d, 0)
    o, out = h[5], []
    while True:
        p, i = struct.unpack_from('>Ii', d, o)
        if not p and not i:
            return out
        out.append((i, p))
        o += 8


def pd_lists(d):
    h = struct.unpack_from('>8I', d, 0)
    o, out = h[6], []
    while True:
        p, i = struct.unpack_from('>Ii', d, o)
        if not p and not i:
            return out
        out.append((i, p))
        o += 8


def ge_walk(d, at):
    out = []
    while at < len(d):
        op = d[at]
        if op >= len(GE) or GE[op][1] is None:
            out.append((at, op, None))
            return out
        ln = GE[op][1]
        out.append((at, op, d[at:at + ln]))
        at += ln
        if GE[op][0] == 'EndList':
            return out
    return out


def pd_walk(d, at):
    out = []
    while at + 1 < len(d):
        op = struct.unpack_from('>H', d, at)[0]
        ln = PDLEN.get(op)
        if not ln:
            out.append((at, op, None))
            return out
        out.append((at, op, d[at:at + ln]))
        at += ln
        if op == 4:
            return out
    return out


def align(a, b):
    n, m, gap = len(a), len(b), -0.7
    d = np.zeros((n + 1, m + 1))
    bk = np.zeros((n + 1, m + 1), np.int8)
    d[0, :] = np.arange(m + 1) * gap
    d[:, 0] = np.arange(n + 1) * gap
    for i in range(1, n + 1):
        ga = a[i - 1][2]
        for j in range(1, m + 1):
            pb = b[j - 1][2]
            s = -0.2
            if ga is not None and pb is not None:
                s = 1.0 if (len(ga) - 1) == (len(pb) - 2) else -0.1
                if len(ga) > 1 and len(pb) > 2 and ga[1:] == pb[2:]:
                    s = 2.0
            o = (d[i - 1, j - 1] + s, d[i - 1, j] + gap, d[i, j - 1] + gap)
            k = int(np.argmax(o))
            d[i, j] = o[k]
            bk[i, j] = k
    i, j, out = n, m, []
    while i > 0 or j > 0:
        if i and j and bk[i, j] == 0:
            out.append((a[i - 1], b[j - 1]))
            i -= 1
            j -= 1
        elif i and (not j or bk[i, j] == 1):
            i -= 1
        else:
            j -= 1
    return out


def oracle_votes():
    st = gexdata.stages()
    ev = collections.defaultdict(collections.Counter)
    for gename, sid in MISSIONS:
        g = gefiles.rom_file(gename)
        x = gexprops.setup(st[sid]['setup'])
        gl = dict(ge_lists(g))
        xl = dict(pd_lists(x))
        for lid in sorted(set(gl) & set(xl)):
            a, b = ge_walk(g, gl[lid]), pd_walk(x, xl[lid])
            if len(a) > 400 or len(b) > 400:
                continue
            for pa, pb in align(a, b):
                if pa and pb and pa[2] is not None and pb[2] is not None:
                    ev[pa[1]][pb[1]] += 1
    return ev


def build():
    ta = table_align()
    ev = oracle_votes()
    rows = []
    for i, (name, ln, args) in enumerate(GE):
        if i in OVERRIDE:
            row = OVERRIDE[i]
            why = 'hand'
        else:
            pd = ta.get(i)
            row = (pd, tuple(range(len(args)))) if pd is not None else None
            why = 'table'
            if pd is not None and ev.get(i):
                best, votes = ev[i].most_common(1)[0]
                if best == pd:
                    why = 'both'
        rows.append((i, name, ln, args, row, why))
    return rows


def emit_python(rows, path):
    out = ['"""The GoldenEye -> Perfect Dark AI command map.',
           '',
           'Generated by tools/geconvert/fit/genaitable.py - read that for how each row',
           'was found. A row is GoldenEye\'s opcode:',
           '',
           '    (name, length, [(argument, width)], perfect dark opcode, (argument specs))',
           '',
           'and an argument spec makes one Perfect Dark argument: `n` copies GoldenEye\'s',
           'argument n at its own width, `(n, w)` copies it as w bytes, `("=", v, w)` is a',
           'constant. A row whose Perfect Dark opcode is None has no equivalent and the',
           'command is left out of the converted list.',
           '"""',
           '',
           'CHR_BOND = 0x%04x' % CHR_BOND,
           'CHR_SELF = 0x%04x' % CHR_SELF,
           '',
           '# opcode: (name, length, args, pd opcode, pd args, where the row came from)',
           'TABLE = [']
    for i, name, ln, args, row, why in rows:
        pd, spec = row if row else (None, ())
        out.append('    (%-38s %-5s %-46s %-7s %-44s %r),  # %02x %s' % (
            '%r,' % name, '%r,' % ln, '%r,' % args,
            ('0x%04x,' % pd) if pd is not None else 'None,',
            '%r,' % (spec,), why, i, PDNAME.get(pd, '-')))
    out.append(']')
    out.append('')
    open(path, 'w').write('\n'.join(out) + '\n')


def emit_c(rows, path):
    out = ['/**',
           ' * The GoldenEye -> Perfect Dark AI command map.',
           ' *',
           ' * Generated by tools/geconvert/fit/genaitable.py; the reference copy is',
           ' * tools/geconvert/geaitable.py and the two must stay the same bytes.',
           ' */',
           '#ifndef GEAITABLE_H',
           '#define GEAITABLE_H',
           '',
           '#define GEAI_NUM_COMMANDS %d' % len(rows),
           '#define GEAI_MAX_ARGS 12',
           '#define GEAI_CHR_BOND 0x%04x' % CHR_BOND,
           '#define GEAI_CHR_SELF 0x%04x' % CHR_SELF,
           '',
           '// an argument spec: `from` is the GoldenEye argument, or -1 for a constant',
           'struct geaiarg {',
           '\tint8_t from;',
           '\tuint8_t width;',
           '\tuint16_t value;',
           '};',
           '',
           'struct geaicmd {',
           '\tuint8_t len;      // GoldenEye\'s length in bytes, 0 when it is measured (PRINT)',
           '\tint16_t pd;       // Perfect Dark\'s opcode, -1 when there is no equivalent',
           '\tuint8_t numge;    // GoldenEye\'s own arguments, in order',
           '\tuint8_t gewidth[GEAI_MAX_ARGS];',
           '\tuint16_t gepad;   // a bit per GoldenEye argument that is a pad id',
           '\tuint8_t numargs;  // and what Perfect Dark\'s command is given',
           '\tstruct geaiarg args[GEAI_MAX_ARGS];',
           '};',
           '',
           'static const struct geaicmd g_GeAiCommands[GEAI_NUM_COMMANDS] = {']
    for i, name, ln, args, row, why in rows:
        pd, spec = row if row else (None, ())
        cells = []
        for s in spec:
            if isinstance(s, tuple) and s and s[0] == '=':
                cells.append('{-1, %d, 0x%04x}' % (s[2], s[1]))
            elif isinstance(s, tuple):
                cells.append('{%d, %d, 0}' % (s[0], s[1]))
            else:
                cells.append('{%d, %d, 0}' % (s, args[s][1]))
        pad = 0
        for k, (aname, w) in enumerate(args):
            if 'PAD' in aname and w >= 2:
                pad |= 1 << k
        out.append('\t/* %02x %-38s */ { %2d, %6s, %2d, { %s }, 0x%04x, %2d, { %s } },' % (
            i, name, ln or 0, ('0x%04x' % pd) if pd is not None else '-1',
            len(args), ', '.join(str(w) for _, w in args) or '0', pad,
            len(cells), ', '.join(cells) or '{0, 0, 0}'))
    out.append('};')
    out.append('')
    out.append('#endif')
    open(path, 'w').write('\n'.join(out) + '\n')


if __name__ == '__main__':
    rows = build()
    if '-o' not in sys.argv[1:]:
        sys.exit('usage: genaitable.py -o OUTDIR (the repo\'s tables are kept by hand)')
    root = sys.argv[sys.argv.index('-o') + 1]
    emit_python(rows, os.path.join(root, 'geaitable.py'))
    emit_c(rows, os.path.join(root, 'geaitable.h'))
    both = sum(1 for r in rows if r[5] == 'both')
    hand = sum(1 for r in rows if r[5] == 'hand')
    none = sum(1 for r in rows if r[4] is None)
    bad = [r for r in rows if r[4] and PDLEN.get(r[4][0]) != 2 + sum(
        (s[2] if isinstance(s, tuple) and s[0] == '=' else
         s[1] if isinstance(s, tuple) else r[3][s][1]) for s in r[4][1])]
    print('%d commands: %d confirmed by GE-X, %d by hand, %d with no equivalent'
          % (len(rows), both, hand, none))
    print('rows whose arguments do not fill Perfect Dark\'s command: %d' % len(bad))
    for r in bad:
        print('   %02x %-34s -> %04x %-30s pd len %s' % (
            r[0], r[1], r[4][0], PDNAME.get(r[4][0], '?'), PDLEN.get(r[4][0])))
