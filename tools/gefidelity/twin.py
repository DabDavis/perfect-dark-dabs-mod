#!/usr/bin/env python3
"""Run one gdb scenario against GoldenEye (the decomp's native port on the
oracle host) and/or against our port, with the same mission and difficulty.

    twin.py both world/dump.py --mission dam --out out/dam
    twin.py ge   SCRIPT --mission facility --diff 00 --env GF_TICKS=1,600
    twin.py pd   SCRIPT --mission 13 --bin ./pd.fix

A scenario is a gdb Python file that imports gdbge or gdbpd by GF_SIDE (see
world/dump.py). It sees GF_SIDE, GF_COMMON, GF_LEVELID, GF_MISSION, GF_DIFF and
GF_OUT, plus anything passed with --env. Results land in OUT/ge and OUT/pd; the
gdb transcript is OUT/<side>/gdb.log, and for our side pd.log and any
screenshots are copied in too.

The oracle is never modified: the toolkit is rsynced to the host, one copy per
source tree (~/gefidelity/trees/<tree>-<hash>, GE_TOOLS), and run from
~/claude-007/007. Our side runs from a run directory
(--rundir, default ~/wt/gefidelity-run) holding the binary, data/,
added-content/ and mods/ - never a player's install.
"""
import argparse, hashlib, os, re, shlex, subprocess, sys, time, shutil

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'common'))
import levels  # noqa: E402

GE_HOST = os.environ.get('GF_GE_HOST', 'sdg@10.8.0.3')
GE_TREE = os.environ.get('GF_GE_TREE', '~/claude-007/007')
# One copy of the toolkit on the host per source tree. With a single shared
# ~/gefidelity, every worktree's sync (rsync --delete) put its own scenarios and
# oracle code over the others' mid-run: on 2026-10-01 three agents' branches
# reverted each other's aresge.py/gdbge.py and gunscen_ares.py for an hour.
_TREE = os.path.dirname(os.path.dirname(HERE))
GE_TOOLS = os.environ.get('GF_GE_TOOLS', 'gefidelity/trees/%s-%s' % (
    re.sub(r'[^A-Za-z0-9_.-]', '_', os.path.basename(_TREE)), hashlib.sha1(_TREE.encode()).hexdigest()[:8]))
DEFAULT_RUNDIR = os.path.expanduser(os.environ.get('GF_RUNDIR', '~/wt/gefidelity-run'))
_SEQ = __import__('itertools').count()  # several run_ge() calls in one second, one process


def ssh(cmd, check=True, **kw):
    full = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=15', GE_HOST, cmd]
    return subprocess.run(full, check=check, **kw)


def sync_tools():
    subprocess.run(['rsync', '-a', '--delete', '--mkpath', '--exclude', 'out/', '--exclude', '__pycache__/',
                    HERE + '/', '%s:%s/' % (GE_HOST, GE_TOOLS)], check=True)


# Which GoldenEye: 'ares' is the cartridge in ares through n64twin (common/aresge.py);
# 'port' is the decomp's native port under gdb (common/gdbge.py).
DEFAULT_ORACLE = os.environ.get('GF_ORACLE', 'port')


def run_ge(script, m, diff, out, env, timeout, render, oracle=None):
    env = dict(env)
    """Returns a Popen; the oracle's results are fetched by fetch_ge()."""
    oracle = oracle or DEFAULT_ORACLE
    stamp = '%s-%s-%d-%d' % (time.strftime('%Y%m%d-%H%M%S'), m[1], os.getpid(), next(_SEQ))
    rout = '~/gefidelity-out/%s' % stamp
    rel = os.path.relpath(os.path.abspath(script), HERE)
    e = {'PORT_PAD_SCRIPT': '$HOME/%s/common/%s' % (GE_TOOLS, env.pop('GF_PADSCRIPT', 'solo-quiet.padscript')),
         'PORT_BOOT_FRAMES': '1000000', 'PORT_LOCKSTEP': '1', 'PORT_VI_LOCKSTEP': '1',
         'GF_SIDE': 'ge', 'GF_COMMON': '$HOME/%s/common' % GE_TOOLS, 'GF_LEVELID': m[3],
         'GF_MISSION': str(m[0]), 'GF_DIFF': str(diff), 'GF_OUT': rout.replace('~', '$HOME')}
    if not render:
        e['PORT_RENDER_FROM'] = '999999'
    e.update(env)
    envs = ' '.join('%s=%s' % (k, v if v.startswith('$HOME') else shlex.quote(v)) for k, v in e.items())
    cmd = ('mkdir -p {r} && cd {tree} && env {envs} timeout -k 5 {t} gdb -batch -x $HOME/{tools}/{rel} '
           '--args ./build/port/ge007 --boot > {r}/gdb.log 2>&1; echo "GF ge rc=$?" >> {r}/gdb.log').format(
        r=rout, tree=GE_TREE, envs=envs, t=timeout, rel=rel, tools=GE_TOOLS)
    if oracle == 'ares':
        e = {'GF_SIDE': 'ge', 'GF_ORACLE': 'ares', 'GF_COMMON': '$HOME/%s/common/ares' % GE_TOOLS,
             'GF_LEVELID': m[3], 'GF_MISSION': str(m[0]), 'GF_DIFF': str(diff),
             'GF_OUT': rout.replace('~', '$HOME')}
        env.pop('GF_PADSCRIPT', None)
        e.update(env)
        envs = ' '.join('%s=%s' % (k, v if v.startswith('$HOME') else shlex.quote(v)) for k, v in e.items())
        cmd = ('mkdir -p {r} && cd $HOME/{tools} && env {envs} timeout -k 5 {t} python3 $HOME/{tools}/{rel} '
               '> {r}/gdb.log 2>&1; echo "GF ge rc=$?" >> {r}/gdb.log').format(r=rout, envs=envs, t=timeout, rel=rel,
                                                                           tools=GE_TOOLS)
    p = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', GE_HOST, cmd])
    p.gf_remote = rout
    p.gf_local = os.path.join(out, 'ge')
    return p


def fetch_ge(p):
    os.makedirs(p.gf_local, exist_ok=True)
    subprocess.run(['rsync', '-a', '%s:%s/' % (GE_HOST, p.gf_remote.replace('~/', '')), p.gf_local + '/'],
                   check=False)
    ssh('rm -rf %s' % p.gf_remote, check=False)


def run_pd(script, m, diff, out, env, timeout, rundir, binary, extra):
    local = os.path.abspath(os.path.join(out, 'pd'))
    os.makedirs(local, exist_ok=True)
    save = os.path.join(local, 'save')
    os.makedirs(save, exist_ok=True)
    if env.get('GF_GAME') == 'gf':
        # A ROM hack's maps are put in Mod.MapMods once, when it is first
        # converted (gexplusrom.c); a fresh save directory has GoldenEye's
        # arenas alone and the hack's missions are not mounted. Appended to
        # whatever pd.ini a tool wrote here (view's window and field of view).
        with open(os.path.join(save, 'pd.ini'), 'a') as fh:
            fh.write('\n[Mod]\nMapMods=GoldenEye Arenas;%s\nGexPlusMapsOffered=1\n' % levels.GF_VARIANT)
    e = dict(os.environ)
    # no real controller reaches a headless run (the Xenia rig's virtual Xbox pad
    # was assigned to player 0 mid-run and pressed buttons in it)
    e.update({'SDL_GAMECONTROLLER_IGNORE_DEVICES': '0x045e/0x028e,0x054c/0x0ce6',
              'SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT': '0x0000/0x0000', 'SDL_JOYSTICK_HIDAPI': '0'})
    e.update({'SDL_VIDEODRIVER': os.environ.get('SDL_VIDEODRIVER', 'offscreen'),
              'SDL_AUDIODRIVER': 'dummy', 'GF_SIDE': 'pd', 'GF_COMMON': os.path.join(HERE, 'common'),
              'GF_LEVELID': m[3], 'GF_MISSION': str(m[0]), 'GF_DIFF': str(diff), 'GF_OUT': local})
    e.update(env)
    args = ['timeout', '-k', '5', str(timeout), 'gdb', '-batch', '-x', os.path.abspath(script), '--args',
            binary, '--savedir', save, '--skip-intro', '--no-sound', '--boot-ge-mission', str(m[0]),
            '--skip-mission-intro', '--fixed-step', '--rng-seed', '1', '--log'] + extra
    # each run gets its own directory: the port writes pd.log and screenshots/
    # beside its executable, and concurrent runs in one directory overwrite
    # each other's. The binary is hard-linked (the port finds its folder from
    # /proc/self/exe, which would see through a symlink); data/, added-content/
    # and mods/ are shared.
    own = os.path.join(local, 'run')
    if os.path.isdir(own):
        shutil.rmtree(own)
    os.makedirs(own)
    src = os.path.normpath(os.path.join(rundir, binary))
    exe = os.path.join(own, os.path.basename(src))
    try:
        os.link(src, exe)
    except OSError:
        shutil.copy2(src, exe)
    for d in ('data', 'added-content', 'mods', 'texture-packs', 'model-packs'):
        if os.path.exists(os.path.join(rundir, d)):
            os.symlink(os.path.abspath(os.path.join(rundir, d)), os.path.join(own, d))
    args[args.index(binary)] = exe
    log = open(os.path.join(local, 'gdb.log'), 'w')
    p = subprocess.Popen(args, cwd=own, env=e, stdout=log, stderr=subprocess.STDOUT)
    p.gf_local = local
    p.gf_rundir = own
    p.gf_log = log
    return p


def finish_pd(p):
    p.gf_log.close()
    exe = [f for f in os.listdir(p.gf_rundir) if os.path.isfile(os.path.join(p.gf_rundir, f)) and os.access(os.path.join(p.gf_rundir, f), os.X_OK)]
    for f in exe:
        os.unlink(os.path.join(p.gf_rundir, f))   # 47 MB a run otherwise
    src = os.path.join(p.gf_rundir, 'pd.log')
    if os.path.exists(src):
        shutil.copy(src, os.path.join(p.gf_local, 'pd.log'))
        n = 0
        for line in open(src, errors='replace'):
            if 'screenshot:' in line:
                f = line.split('screenshot:', 1)[1].strip()
                if os.path.exists(f):
                    shutil.move(f, os.path.join(p.gf_local, 'shot_%03d%s' % (n, os.path.splitext(f)[1])))
                    n += 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('side', choices=['ge', 'pd', 'both'])
    ap.add_argument('script')
    ap.add_argument('--mission', default='dam')
    ap.add_argument('--diff', default='agent')
    ap.add_argument('--out', required=True)
    ap.add_argument('--env', action='append', default=[], help='K=V for the scenario (both sides); '
                    'GF_PADSCRIPT=solo.padscript picks the oracle\'s pad script (default solo-quiet: one Z to '
                    'dismiss the opening still, then hands off)')
    ap.add_argument('--timeout', type=int, default=900)
    ap.add_argument('--render', action='store_true', help='oracle renders (needed for shots)')
    ap.add_argument('--rundir', default=DEFAULT_RUNDIR)
    ap.add_argument('--bin', default='./pd.base', help='our binary, relative to --rundir')
    ap.add_argument('--pd-arg', action='append', default=[], help='extra argument for our binary')
    ap.add_argument('--no-sync', action='store_true', help='the toolkit is already on the oracle host')
    ap.add_argument('--oracle', choices=['ares', 'port'], default=None,
                    help='GoldenEye side: the cartridge in ares, or the native port (default %s)' % DEFAULT_ORACLE)
    ap.add_argument('--game', choices=['ge', 'gf'], default=levels.GAME,
                    help='ge: GoldenEye and GE Plus; gf: Goldfinger 64 - its cartridge (ares only, Expansion Pak) '
                         'against ours booted in its mode (--boot-ge-variant). Default GF_GAME, else ge')
    a = ap.parse_args()
    if a.game != levels.GAME:
        # mission names are the game's own: levels.py reads GF_GAME at import
        os.environ['GF_GAME'] = a.game
        import importlib
        importlib.reload(levels)
    if a.game == 'gf':
        a.oracle = a.oracle or 'ares'
        if a.oracle != 'ares' and a.side != 'pd':
            ap.error('Goldfinger 64 runs on the cartridge only (--oracle ares)')
        a.env.append('GF_GAME=gf')
        a.pd_arg = ['--boot-ge-variant', levels.GF_VARIANT] + a.pd_arg
    m = levels.mission(a.mission)
    diff = levels.difficulty(a.diff)
    env = dict(kv.split('=', 1) for kv in a.env)
    os.makedirs(a.out, exist_ok=True)
    procs = []
    if a.side in ('ge', 'both'):
        if not a.no_sync:
            sync_tools()
        procs.append(('ge', run_ge(a.script, m, diff, a.out, env, a.timeout, a.render, a.oracle)))
    if a.side in ('pd', 'both'):
        procs.append(('pd', run_pd(a.script, m, diff, a.out, env, a.timeout, a.rundir, a.bin, a.pd_arg)))
    rc = 0
    for side, p in procs:
        p.wait()
        if side == 'ge':
            fetch_ge(p)
        else:
            finish_pd(p)
        log = os.path.join(a.out, side, 'gdb.log')
        tail = open(log, errors='replace').read().splitlines()[-3:] if os.path.exists(log) else ['(no log)']
        lines = open(log, errors='replace').read().splitlines() if os.path.exists(log) else []
        ok = any(l.startswith('GF ') for l in lines) and not any(l.startswith('GF FAILED') for l in lines)
        print('%s: %s  %s' % (side, 'ran' if ok else 'FAILED', ' | '.join(tail)))
        rc |= 0 if ok else 1
    return rc


if __name__ == '__main__':
    sys.exit(main())
