#!/usr/bin/env python3
"""The conversion diff: which converted files a change altered, and where.

    convdiff/diff.py BASE/conv TEST/conv [--md gate.md] [--json diff.json]

Not a fidelity judgment - the gate's other legs judge what the files do in the
game against the originals. This shows a change's footprint before any sweep
runs: a door fix that also rewrote twelve unrelated setups stands out here.
Exit 0 always, except 2 when the null fails (filecmp.null: a one-byte flip in a
raw and a compressed copy must both be seen).
"""
import argparse, json, os, sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import filecmp as fc  # noqa: E402  (this directory's, not the standard library's)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('base')
    ap.add_argument('test')
    ap.add_argument('--md')
    ap.add_argument('--json')
    a = ap.parse_args()
    scratch = os.path.join(os.path.dirname(os.path.abspath(a.test.rstrip('/'))), 'convdiff-null')
    ok, msg = fc.null(a.base, a.base, scratch)
    print(msg)
    if not ok:
        print('CONVDIFF NULL FAILED: the comparison cannot see a one-byte change')
        return 2
    res = fc.compare_trees(a.base, a.test)
    by = defaultdict(list)
    for f in sorted(res['different']):
        by[f.split('/')[0] if '/' in f else '.'].append(f)
    lines = ['# Conversion diff', '', '%s -> %s' % (a.base, a.test), '',
             '%d files identical, %d the same once inflated, %d changed, %d only before, %d only after.' % (
                 res['same'], res['recompressed'], len(res['different']), len(res['only_a']), len(res['only_b'])), '']
    for d in sorted(by):
        lines.append('## %s (%d changed)' % (d, len(by[d])))
        for f in by[d]:
            lines.append('- `%s`: %s' % (f, res['different'][f]))
        lines.append('')
    if res['only_a']:
        lines += ['## Only before the change', ''] + ['- `%s`' % f for f in res['only_a']] + ['']
    if res['only_b']:
        lines += ['## Only after the change', ''] + ['- `%s`' % f for f in res['only_b']] + ['']
    text = '\n'.join(lines)
    if a.md:
        open(a.md, 'w').write(text)
    if a.json:
        json.dump(res, open(a.json, 'w'), indent=1)
    print(lines[4])
    return 0


if __name__ == '__main__':
    sys.exit(main())
