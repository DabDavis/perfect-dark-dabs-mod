"""Which of a converted mission's setup chrs our port refuses to make.

    twin.py pd ai/spawnrefused.py --mission facility --out OUT --rundir RUNDIR

bodyAllocateChr() (body.c) leaves out a setup chr whose pad volume (radius 20)
collides with anything, unless it sits in a chair or carries
SPAWNFLAG_IGNORECOLLISION. GoldenEye has no such refusal: expand_09_characters()
asks getposstan() for a clear spot within 20 units on the tile graph and spawns
there, refusing only when that fails. A chr refused here that runs AI in
GoldenEye (aidiff.py's "missing" streams) is a guard the conversion lost.
Prints "GFSPAWN refused chr N pad P" per refusal and the setup's totals.
"""
import os, sys
sys.path.insert(0, os.environ['GF_COMMON'])
import gdb
import gdbpd as L

gdb.execute('break lvReset')
gdb.execute('run')
gdb.execute('delete')
gdb.execute('set variable g_Difficulty = %d' % int(os.environ.get('GF_DIFF', '0')))
# the line inside the refusal (g_BodySpawnStats.collision++)
line = next(i + 1 for i, l in enumerate(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                          '..', '..', '..', 'src', 'game', 'body.c')))
            if 'g_BodySpawnStats.collision++' in l)
gdb.execute('break body.c:%d' % line)
gdb.execute('commands\nsilent\nprintf "GFSPAWN refused chr %d pad %d\\n", packed->chrnum, packed->padnum\ncontinue\nend')
gdb.execute('break videoEndFrame if g_Vars.lvframenum >= 1')
gdb.execute('continue')
L.say('setup chrs', int(L.ev('g_BodySpawnStats.entries')), 'refused', int(L.ev('g_BodySpawnStats.collision')))
L.finish()
