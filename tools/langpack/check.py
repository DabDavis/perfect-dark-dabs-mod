#!/usr/bin/env python3
"""
Checks a language's source files (lang/<code>/) against the English they
translate. build.py runs the same checks and leaves out any string that fails
one - it falls back to English in the game - while this reports them all and
exits non-zero, for a translator to run before committing:

    python3 tools/langpack/check.py [--source lang/_source] lang/fr [lang/de ...]

What is checked (CLAUDE-notes/languages.md):
- every key is one the English has: pd ids against the ntsc-final files, and
  port and GE keys against the source catalogs in lang/_source/ that
  tools/langpack/extract.py writes (when they are there - they are generated
  and not committed; --source names another copy, --no-source skips it);
- printf conversions (%s, %d, %02d, ...) the same and in the same order as the
  English, because a mismatch crashes the game;
- a ROM string keeps its trailing newline (one without measures zero high);
- the length: a string must fit the buffers text goes through;
- every character can be drawn: in a Latin pack, ASCII plus what latin.py can
  compose; in a CJK pack, ASCII plus the glyphs tools/langfont baked.
Every pack falls back to the ROM's English ("fallback": "en") for what it
leaves out; a sparse pack (en-GB, or "sparse": true) is not warned about it.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import latin  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
NTSC_LANG = os.path.join(ROOT, 'src', 'assets', 'ntsc-final', 'lang')
# the Japanese glyphs tools/langfont/bake.py baked, read from its code point table
CJK_GLYPH_SOURCE = os.path.join(ROOT, 'port', 'src', 'langfont_cjk.c')

# The largest string the text path can hold: menuitem.c's wrap buffers.
MAX_BYTES = 15000
# A HUD message is cut at 400 bytes (struct hudmessage.text).
MAX_HUD_BYTES = 399

# Strings copied into a fixed field of the save or the setup: the most bytes,
# newline included. The game cuts a longer one at a whole character, but a
# translator should know. Team names are g_BossFile.teamnames[8][12]; the
# default player name goes through "%s %d\n" into mpchrconfig.name[15].
SIZE_LIMITS = dict([('L_OPTIONS_%03d' % i, 11) for i in range(8, 16)] + [('L_MISC_437', 10)])

PRINTF = re.compile(r'%(?:%|[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|L|z|j|t)?[diouxXeEfgGcsp])')

BANKS = [
    '', 'ame', 'arch', 'ark', 'ash', 'azt', 'cat', 'cave', 'arec', 'crad', 'cryp',
    'dam', 'depo', 'dest', 'dish', 'ear', 'eld', 'imp', 'jun', 'lee', 'len', 'lip',
    'lue', 'oat', 'pam', 'pete', 'ref', 'rit', 'run', 'sevb', 'sev', 'sevx', 'sevxb',
    'sho', 'silo', 'stat', 'tra', 'wax', 'gun', 'title', 'mpmenu', 'propobj',
    'mpweapons', 'options', 'misc', 'uff', 'old', 'ate', 'lam', 'mp1', 'mp2', 'mp3',
    'mp4', 'mp5', 'mp6', 'mp7', 'mp8', 'mp9', 'mp10', 'mp11', 'mp12', 'mp13', 'mp14',
    'mp15', 'mp16', 'mp17', 'mp18', 'mp19', 'mp20',
]  # tools/assetmgr/mklang's order: a text id is bank * 512 + row


def conversions(s):
    return [c for c in PRINTF.findall(s) if c != '%%']


_english = None


def english():
    """{ 'L_AME_000': (textid, english) } from the ntsc-final files."""
    global _english
    if _english is None:
        _english = {}
        for bank, name in enumerate(BANKS):
            if not name:
                continue
            path = os.path.join(NTSC_LANG, name + '.json')
            if not os.path.exists(path):
                continue
            with open(path, encoding='utf-8') as f:
                rows = json.load(f)
            for row, r in enumerate(rows):
                _english[r['id']] = (bank * 512 + row, r.get('en'))
    return _english


_cjk = None


def cjk_glyphs():
    global _cjk
    if _cjk is None:
        if not os.path.exists(CJK_GLYPH_SOURCE):
            return None
        with open(CJK_GLYPH_SOURCE, encoding='utf-8') as f:
            src = f.read()
        m = re.search(r's_CodePoints\[\d+\] = \{(.*?)\};', src, re.S)
        _cjk = set(chr(int(x, 16)) for x in re.findall(r'0x([0-9a-fA-F]+)', m.group(1))) if m else set()
    return _cjk


class Problems:
    def __init__(self, code):
        self.code = code
        self.errors = []
        self.warnings = []

    def error(self, key, msg):
        self.errors.append('%s: %s: %s' % (self.code, key, msg))

    def warn(self, key, msg):
        self.warnings.append('%s: %s: %s' % (self.code, key, msg))


def check_string(p, key, en, text, script, hud=False):
    """Whether text may stand in for en. Records why not in p."""
    ok = True

    if en is not None and conversions(en) != conversions(text):
        p.error(key, 'printf conversions %s, English has %s' % (conversions(text), conversions(en)))
        ok = False

    size = len(text.encode('utf-8'))

    if size > MAX_BYTES:
        p.error(key, '%d bytes, the most is %d' % (size, MAX_BYTES))
        ok = False
    elif hud and size > MAX_HUD_BYTES:
        p.warn(key, '%d bytes; a HUD message keeps %d' % (size, MAX_HUD_BYTES))

    # a NUL inside a ROM string (the hangar bios' "name\0|subheading") is
    # data, written \0 in the .lang
    if en is not None and '\x00' in en:
        text = text.replace('\x00', '')

    if script == 'cjk':
        glyphs = cjk_glyphs()
        for ch in text:
            if latin.drawable(ch):
                continue
            if glyphs is not None and ch not in glyphs:
                p.error(key, 'no glyph for %r (U+%04X)' % (ch, ord(ch)))
                ok = False
                break
    else:
        for ch in text:
            if not latin.drawable(ch):
                p.error(key, 'no glyph for %r (U+%04X) in a Latin pack' % (ch, ord(ch)))
                ok = False
                break

    return ok


def load_json(path, p):
    try:
        with open(path, encoding='utf-8') as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        p.error(os.path.basename(path), 'cannot be read: %s' % e)
        return {}
    if not isinstance(data, dict):
        p.error(os.path.basename(path), 'is not an object of "key": "text"')
        return {}
    return data


DEFAULT_SOURCE = os.path.join(ROOT, 'lang', '_source')


class Source:
    """The English source catalogs (lang/_source/, tools/langpack/extract.py)."""

    def __init__(self, path):
        self.path = path
        self.name = os.path.relpath(path)
        with open(os.path.join(path, 'port.json'), encoding='utf-8') as f:
            self.port = json.load(f)
        try:
            with open(os.path.join(path, 'ge.json'), encoding='utf-8') as f:
                self.ge = json.load(f)
        except OSError:
            self.ge = None
        # GoldenEye's by their English (ge:<English>), as the game looks them up
        self.ge_english = set(strip_nl(v) for v in (self.ge or {}).values())
        self.port_english = set(strip_nl(v) for v in self.port.values())

    def port_english_of(self, key):
        """The English a port.json key stands for, or None if it is not one."""
        k = strip_nl(key)
        for c in (k, k + '\n'):
            if c in self.port:
                return self.port[c]
        if '|' in k:
            rest = k.split('|', 1)[1]
            for c in (rest, rest + '\n'):
                if c in self.port:
                    return self.port[c]
        return None


def strip_nl(s):
    return s[:-1] if s.endswith('\n') else s


def load_source(path):
    if path and os.path.exists(os.path.join(path, 'port.json')):
        return Source(path)
    return None


def read_language(srcdir, source=None):
    """
    The checked strings of one language:
    (meta, { key: text }, Problems), keys as the .lang file has them -
    pd.<hex id>, port:<English>, ge.<bank>.<slot>, ge:<English>.

    With a Source, port and GE keys are checked against the English source
    catalogs too: a key they do not have is an error (it is never shown), and
    a GE string's printf conversions and trailing newline are GoldenEye's.
    """
    code = os.path.basename(os.path.normpath(srcdir))
    p = Problems(code)
    meta = load_json(os.path.join(srcdir, 'meta.json'), p)
    script = meta.get('script', 'latin')
    # Every pack falls back to the ROM's English for what it leaves out; a
    # sparse one (English (UK)'s few spellings) is not warned about it.
    sparse = bool(meta.get('sparse', code.lower().startswith('en')))
    out = {}

    if 'name' not in meta:
        p.error('meta.json', 'has no "name" (the language in its own words)')

    if script not in ('latin', 'cjk'):
        p.error('meta.json', 'script is "%s", not "latin" or "cjk"' % script)

    en = english()
    pddir = os.path.join(srcdir, 'pd')
    seen = set()

    if os.path.isdir(pddir):
        for fname in sorted(os.listdir(pddir)):
            if not fname.endswith('.json'):
                continue
            bankname = fname[:-5]
            if bankname not in BANKS:
                p.error('pd/' + fname, 'is not one of the ROM\'s banks')
                continue
            for key, text in load_json(os.path.join(pddir, fname), p).items():
                if key not in en:
                    p.error(key, 'is not a text id of the ROM')
                    continue
                textid, english_text = en[key]
                if english_text is None:
                    p.warn(key, 'the ROM has no text here; left out')
                    continue
                if not isinstance(text, str):
                    p.error(key, 'is not a string')
                    continue
                if text == '':
                    continue  # not translated yet
                if english_text.endswith('\n') and not text.endswith('\n'):
                    p.warn(key, 'had no trailing newline; one was added')
                    text += '\n'
                hud = False
                if key in SIZE_LIMITS and len(text.encode('utf-8')) > SIZE_LIMITS[key]:
                    p.warn(key, '%d bytes; the field holds %d and the game cuts the rest'
                           % (len(text.encode('utf-8')), SIZE_LIMITS[key]))
                if check_string(p, key, english_text, text, script, hud):
                    out['pd.%04x' % textid] = text
                seen.add(key)

    if not sparse:
        missing = [k for k in en if en[k][1] and k not in seen]
        if missing:
            p.warn('pd', '%d ids not translated, English is shown for them (first: %s)' % (len(missing), missing[0]))

    for fname, prefix in (('port.json', 'port:'), ('ge.json', 'ge')):
        path = os.path.join(srcdir, fname)
        if not os.path.exists(path):
            continue
        for key, text in load_json(path, p).items():
            if not isinstance(text, str) or text == '':
                continue
            # a '|' is a ctx separator only when the whole key is not itself
            # a port string ("%s|Patch %d%s%s" is one)
            whole = source is not None and any(c in source.port for c in (strip_nl(key), strip_nl(key) + '\n'))
            en_text = key.split('|', 1)[1] if '|' in key and fname == 'port.json' and not whole else key
            if prefix == 'ge':
                if re.fullmatch(r'ge\.[a-z0-9]+\.\d+', key):
                    k = key
                    en_text = None
                    if source is not None and source.ge is not None:
                        if key not in source.ge:
                            p.error(key, 'is not a GoldenEye string (%s/ge.json)' % source.name)
                            continue
                        en_text = source.ge[key]
                        if en_text.endswith('\n') and not text.endswith('\n'):
                            p.warn(key, 'had no trailing newline; one was added')
                            text += '\n'
                else:
                    k = 'ge:' + key
                    if source is not None and strip_nl(key) not in source.ge_english \
                            and strip_nl(key) not in source.port_english:
                        p.error(key, 'is not the English of a GoldenEye string (%s/ge.json)' % source.name)
                        continue
            else:
                k = prefix + key
                if source is not None:
                    found = source.port_english_of(key)
                    if found is None:
                        p.error(key, 'is not a string of the port (%s/port.json; '
                                'run tools/langpack/extract.py if the game changed)' % source.name)
                        continue
            if check_string(p, key, en_text, text, script):
                out[k] = text

    return meta, out, p


def main(argv):
    status = 0
    source_path = DEFAULT_SOURCE
    dirs = []
    i = 0
    while i < len(argv):
        if argv[i] == '--source' and i + 1 < len(argv):
            source_path = argv[i + 1]
            i += 2
            continue
        if argv[i] == '--no-source':
            source_path = None
        else:
            dirs.append(argv[i])
        i += 1
    source = load_source(source_path)
    if source is None and source_path:
        print('note: no source catalogs at %s - port and GE keys are not checked against the English '
              '(python3 tools/langpack/extract.py writes them)' % source_path)
    for srcdir in dirs:
        meta, strings, p = read_language(srcdir, source)
        for w in p.warnings:
            print('warning: ' + w)
        for e in p.errors:
            print('error: ' + e)
        print('%s: %d strings, %d errors, %d warnings' % (p.code, len(strings), len(p.errors), len(p.warnings)))
        if p.errors:
            status = 1
    return status


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1:]))
