"""The cartridge's AI lists as loaded, raw: GF_LISTS=1025,1036 (ids) - each list's
bytes up to 4 KB into OUT/ge/list_<id>.bin, read at the level's first tick.
Decode with ai/listdecode.py (GoldenEye's command lengths and names from
port/include/geaitable.h). Cartridge only (aresge)."""
import os, sys, traceback
sys.path.insert(0, os.environ['GF_COMMON'])
lib = __import__('gdbge')
import aresge  # noqa: E402  (the shim's star import leaves out _ailist_ids)
OUT = os.environ['GF_OUT']
want = [int(x, 0) for x in os.environ.get('GF_LISTS', '1025').split(',')]
try:
    lib.boot(os.environ['GF_LEVELID'], int(os.environ.get('GF_DIFF', '0')))
    lib.until_tick(1)
    ids = {v: k for k, v in aresge._ailist_ids().items()}
    for i in want:
        if i not in ids:
            lib.say('no list', i)
            continue
        open(os.path.join(OUT, 'list_%d.bin' % i), 'wb').write(lib.peek(ids[i], 4096))
        lib.say('list', i, 'at 0x%08x' % ids[i])
except Exception:
    traceback.print_exc()
    lib.say('FAILED')
lib.finish()
