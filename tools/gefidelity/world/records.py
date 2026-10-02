"""Setup records' raw words on either side: the cartridge's as it holds them,
ours as converted (each with its own type numbers).

    twin.py both world/records.py --game gf --mission miami --out OUT --env GF_RECORDS=5-19,548

GF_RECORDS: numbers and ranges; GF_TICKS: when (default 1). Lines "GF rec I TYPE w0 w1 ...".
"""
import os, sys, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbge' if os.environ['GF_SIDE'] == 'ge' else 'gdbpd')
try:
    want = []
    for part in os.environ.get('GF_RECORDS', '').split(','):
        if '-' in part:
            a, b = part.split('-')
            want += range(int(a), int(b) + 1)
        elif part.strip():
            want.append(int(part))
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    for t in [int(x) for x in os.environ.get('GF_TICKS', '1').split(',')]:
        lib.until_tick(t)
        for i in want:
            r = lib.record_words(i)
            if r is None:
                lib.say('rec', i, 'past the end')
                break
            lib.say('rec', i, '%#x' % r[0], ' '.join('%08x' % w for w in r[1]))
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()
