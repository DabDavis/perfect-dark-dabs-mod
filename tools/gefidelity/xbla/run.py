#!/usr/bin/env python3
"""Census runs: the hook build (feat/gefidelity-xbla, GEBEAN_CENSUS) booted into
GE Plus missions in the HD look, one game at a time, each pd.log kept.

    run.py --out OUT [--missions dam,facility|all] [--guns MISSION] [--bin ./pd.census]
    census.py OUT/runs/*.log --plant <the plant run.py printed> --md OUT/report.md --json OUT/report.json

The run directory (--rundir, default ~/wt/gefidelity-run/xbla-run) holds the
binary, data/pd.ntsc-final.z64, data/save/pd.ini with the HD look on and the
Community Edition off, added-content/ with the GoldenEye ROM and the release
(goldeneye/), and mods/ (converted on the first start). Never a player's
install. The HD level disk cache (cache/xbla/goldeneye/levels) is emptied
before every run, or a cached level is never walked.

Every model a mission loads is built at its load in census mode, drawn or not.
--guns MISSION adds a run under gdb that gives and equips each of GoldenEye's
25 guns in turn, so every first-person build is made. The first mission run
plants one fault (GEBEAN_CENSUS_SKIP on a draw of its level's release file)
for census.py's null.
"""
import argparse, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'common'))
sys.path.insert(0, HERE)
import levels  # noqa: E402
import beanref  # noqa: E402

# GoldenEye's level key per mission: the release's new/background/<key> (gebeanstagetable.h)
BACKGROUND = {'dam': 'dam', 'facility': 'facility', 'runway': 'runway', 'surface': 'surface', 'bunker': 'bunker',
              'silo': 'silo', 'frigate': 'frigate', 'surface2': 'surface', 'bunker2': 'bunker2', 'statue': 'statuepark',
              'archives': 'archives', 'streets': 'streets', 'depot': 'depot', 'train': 'train', 'jungle': 'jungle',
              'control': 'control', 'caverns': 'cavern', 'cradle': 'cradle', 'aztec': 'aztec', 'egyptian': 'temple'}

GUNS_GDB = r'''
import gdb, os
gdb.execute('set pagination off'); gdb.execute('set confirm off')
st = {'s': False}
def at(n):
    b = gdb.Breakpoint('videoEndFrame', internal=True)
    b.condition = 'g_Vars.lvframenum >= %d' % n
    gdb.execute('continue' if st['s'] else 'run'); st['s'] = True
    b.delete()
# WEAPON_GE_FIRST and NUM_GE_GUNS are macros, which the DWARF does not keep
first = int(gdb.parse_and_eval('WEAPON_GE_PP7'))
n = int(gdb.parse_and_eval('WEAPON_GE_COVERTMODEM')) - first
fr = 60
at(fr)
for w in range(first, first + n):
    gdb.execute('call (int)invGiveSingleWeapon(%d)' % w, to_string=True)
    gdb.execute('call (void)bgunEquipWeapon(%d)' % w, to_string=True)
    fr += 45
    at(fr)
    print('CENSUSGUN %d held %s' % (w, gdb.parse_and_eval('g_Vars.currentplayer->hands[0].gset.weaponnum')), flush=True)
gdb.execute('kill'); gdb.execute('quit')
'''


def run_one(a, m, plant=None, guns=False, arena=None):
    rd = a.rundir
    shutil.rmtree(os.path.join(rd, 'cache', 'xbla', 'goldeneye', 'levels'), ignore_errors=True)
    env = dict(os.environ, GEBEAN_CENSUS='1', SDL_VIDEODRIVER=os.environ.get('SDL_VIDEODRIVER', 'offscreen'),
               SDL_AUDIODRIVER='dummy')
    if plant:
        env['GEBEAN_CENSUS_SKIP'] = plant
    boot = ['--boot-map', arena] if arena else ['--boot-ge-mission', str(m[0]), '--skip-mission-intro']
    args = [a.bin, '--savedir', 'save', '--skip-intro', '--no-sound'] + boot + ['--fixed-step', '--rng-seed', '1', '--log']
    if guns:
        script = os.path.join(a.out, 'guns.gdb.py')
        open(script, 'w').write(GUNS_GDB)
        cmd = ['timeout', '-k', '5', str(a.timeout), 'gdb', '-batch', '-x', script, '--args'] + args
        tag = m[1] + '-guns'
    else:
        cmd = ['timeout', '-k', '5', str(a.timeout)] + args + ['--exit-frame', str(a.frames)]
        tag = ('arena-' + arena.lower()) if arena else m[1]
    out = open(os.path.join(a.out, 'runs', tag + '.out'), 'w')
    rc = subprocess.run(cmd, cwd=rd, env=env, stdout=out, stderr=subprocess.STDOUT).returncode
    log = os.path.join(rd, 'pd.log')
    dst = os.path.join(a.out, 'runs', tag + '.log')
    if os.path.exists(log):
        shutil.copy(log, dst)
    if plant:   # census.py judges the planted fault in this run alone
        open(os.path.join(a.out, 'runs', tag + '.plant'), 'w').write(plant + '\n')
    n = sum(1 for l in open(dst, errors='replace') if 'census: ' in l) if os.path.exists(dst) else 0
    print('%-14s rc %d, %d census lines%s' % (tag, rc, n, (' (planted %s)' % plant) if plant else ''), flush=True)
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', required=True)
    ap.add_argument('--missions', default='all')
    ap.add_argument('--guns', default='dam', help='mission for the 25-gun run, or "none"')
    ap.add_argument('--arenas', default='Complex,Caves,Temple,Library',
                    help='multiplayer maps booted with --boot-map for their HD levels, or "none"')
    ap.add_argument('--rundir', default=os.path.expanduser('~/wt/gefidelity-run/xbla-run'))
    ap.add_argument('--bin', default='./pd.census')
    ap.add_argument('--frames', type=int, default=120)
    ap.add_argument('--timeout', type=int, default=600)
    a = ap.parse_args()
    os.makedirs(os.path.join(a.out, 'runs'), exist_ok=True)
    ms = levels.MISSIONS if a.missions == 'all' else [levels.mission(k) for k in a.missions.split(',')]
    plant = None
    for i, m in enumerate(ms):
        p = None
        if i == 0:
            bg = 'new/background/' + BACKGROUND[m[1]]
            draws = [d for d in beanref.BeanFile(bg).draws if d['how'] == 'main' and d['tris'] > 4]
            p = plant = '%s@%x' % (bg, draws[len(draws) // 2]['pc'])
        run_one(a, m, plant=p)
    if a.guns != 'none':
        run_one(a, levels.mission(a.guns), guns=True)
    for arena in ([] if a.arenas == 'none' else a.arenas.split(',')):
        run_one(a, None, arena=arena)
    print('plant', plant)
    open(os.path.join(a.out, 'plant.txt'), 'w').write(plant or '')
    return 0


if __name__ == '__main__':
    sys.exit(main())
