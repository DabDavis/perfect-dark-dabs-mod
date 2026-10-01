#!/usr/bin/env python3
"""The AI trace diff as a leg of the fidelity gate (world/gate.py).

    ai/gatesweep.py --bin BINARY --out DIR [--oracle ares|port] [--missions a,b]
                    [--reuse-oracle BASE_DIR] [--ticks 3600] [-j 3]

Traces our BINARY on each mission (and GoldenEye's side, unless
--reuse-oracle takes it from BASE_DIR/<mission>/ge - GoldenEye's trace does not
depend on our binary, so the gate runs the cartridge once), compares them with
aidiff.analyse(), and writes DIR/report.json: one finding per stream whose
script parts from GoldenEye's, in the gate's shape -

    mission  levels.py key
    kind     ai.diverge  a deterministic split (logic, world, unexplained, or
                         a chr only one side has)
             ai.timing   one put down to random draws, timers or which tick a
                         list ran (compare.py lists these, never fails on them)
    key      the stream and the list it parted in: chr12:list0x40c, bg:list0x1001,
             spawn:list0x41d, obj:list0x40a, chr51:missing
    mag      how early it parts: the window's ticks minus the tick of the split,
             so a fix that moves a split later reads as "better"
    detail   one line: where it parted, GoldenEye's command and the converted one

Streams that agree make no finding. window-end splits (one side's trace
stopped still in a loop) are not findings.

Nulls, every run: aidiff's own per mission (exit 2 if any fails), and
repeatability - the first mission's ours is traced a second time and must give
the same ai.diverge keys (exit 2 otherwise).
"""
import argparse, json, os, shutil, subprocess, sys, concurrent.futures as cf

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'common'))
sys.path.insert(0, ROOT)
import levels  # noqa: E402
import aidiff  # noqa: E402

DIVERGE = ('logic', 'world', 'unexplained')
TIMING = ('rng', 'timing')


def list_of(f):
    """The list a split happened in: the decisive command's, else where it came from."""
    for k in ('branch', 'from'):
        v = f.get(k) or ''
        if '@' in v:
            v = v.split('@', 1)[1]
        if '+' in v:
            try:
                return int(v.split('+', 1)[0])
            except ValueError:
                pass
    return None


def stream_id(stream):
    for p in ('bg@', 'spawn@', 'obj@'):
        if stream.startswith(p):
            return p[:-1]
    return stream


def to_gate(mission, findings, ticks):
    out = []
    for f in findings:
        if f['kind'] == 'missing':
            out.append({'mission': mission, 'kind': 'ai.diverge', 'key': '%s:missing' % f['stream'], 'mag': float(ticks),
                        'detail': '%s (first command %s)' % (f['detail'], f.get('first', ''))})
            continue
        if f['kind'] != 'split':
            continue
        for g, lead in ((f, True), (f.get('first_deterministic'), False)):
            if not g or g.get('class') in ('window-end', None):
                continue
            kind = 'ai.diverge' if g['class'] in DIVERGE else 'ai.timing' if g['class'] in TIMING else None
            if kind is None:
                continue
            if not lead and kind != 'ai.diverge':
                continue
            lst = list_of(g)
            key = '%s:list0x%x' % (stream_id(f['stream']), lst) if lst is not None else '%s:start' % stream_id(f['stream'])
            if stream_id(f['stream']) != f['stream']:
                key = '%s:%s:list0x%x' % (stream_id(f['stream']), f['stream'].split('@', 1)[1], lst or 0)
            ts = [t for t in (g.get('ge_tick'), g.get('pd_tick')) if t is not None]
            mag = float(max(0, ticks - min(ts))) if ts else 0.0
            ours = ' '.join(x for x in (g.get('pd_next_ours') or [])[:2] if x)
            detail = '%s parts (%s) after %s: GoldenEye %s, ours %s%s' % (
                f['stream'], g['class'], g.get('branch') or g.get('from') or 'the run start',
                (g.get('ge_next') or ['?'])[0], (g.get('pd_next') or ['?'])[0], ' (ours: %s)' % ours if ours else '')
            out.append({'mission': mission, 'kind': kind, 'key': key, 'mag': mag, 'detail': detail})
    # one finding per (kind, key): keep the earliest split
    best = {}
    for f in out:
        k = (f['kind'], f['key'])
        if k not in best or f['mag'] > best[k]['mag']:
            best[k] = f
    return list(best.values())


def trace(side, m, a, out, rundir=None):
    cmd = [sys.executable, os.path.join(ROOT, 'twin.py'), side, os.path.join(HERE, 'trace.py'),
           '--mission', m[1], '--out', out, '--timeout', str(a.timeout), '--no-sync',
           '--env', 'PORT_AI_TRACE=0:2000000000', '--env', 'GF_AI_TICKS=%d' % a.ticks, '--env', 'GF_AI_INVISIBLE=1']
    if side == 'pd':
        cmd += ['--rundir', rundir, '--pd-arg=--ai-trace', '--pd-arg=%d' % (a.ticks + 10)]
    else:
        cmd += ['--oracle', a.oracle]
    return subprocess.run(cmd, capture_output=True, text=True).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bin', required=True, help='our binary (a path, or a name in the base run directory)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--oracle', choices=['ares', 'port'], default='ares')
    ap.add_argument('--missions', default='all')
    ap.add_argument('--reuse-oracle', help='a base sweep whose <mission>/ge traces are reused')
    ap.add_argument('--ticks', type=int, default=aidiff.DEFAULT_TICKS)
    ap.add_argument('--timeout', type=int, default=3000)
    ap.add_argument('-j', type=int, default=3)
    a = ap.parse_args()
    ms = levels.MISSIONS if a.missions == 'all' else [levels.mission(k) for k in a.missions.split(',')]
    os.makedirs(a.out, exist_ok=True)
    binary = os.path.abspath(a.bin) if os.path.exists(a.bin) else a.bin
    if not a.reuse_oracle:
        import twin
        twin.sync_tools()
    failed = []
    with cf.ThreadPoolExecutor(a.j) as gex, cf.ThreadPoolExecutor(min(2, a.j)) as pex:
        futs = {}
        for m in ms:
            out = os.path.join(a.out, m[1])
            os.makedirs(out, exist_ok=True)
            if a.reuse_oracle:
                src = os.path.join(a.reuse_oracle, m[1], 'ge')
                if not os.path.exists(os.path.join(src, 'gdb.log')):
                    failed.append('%s: no GoldenEye trace in %s' % (m[1], src))
                    continue
                shutil.rmtree(os.path.join(out, 'ge'), ignore_errors=True)
                subprocess.run(['cp', '-al', src, os.path.join(out, 'ge')], check=True)
            else:
                futs[(m[1], 'ge')] = gex.submit(trace, 'ge', m, a, out)
            futs[(m[1], 'pd')] = pex.submit(trace, 'pd', m, a, out, aidiff.rundir_for(m[1], binary))
        for (key, side), fu in futs.items():
            if fu.result():
                failed.append('%s: the %s trace failed' % (key, side))
    report, problems = [], list(failed)
    bad = {f.split(':')[0] for f in failed}
    for m in ms:
        if m[1] in bad:
            continue
        findings, prob, info = aidiff.analyse(m[0], os.path.join(a.out, m[1]), a.ticks)
        if prob:
            problems += ['%s: %s' % (m[1], p) for p in prob]
            continue
        report += to_gate(m[1], findings, a.ticks)
        print('%-10s %d diverge, %d timing' % (m[1], sum(1 for f in report if f['mission'] == m[1] and f['kind'] == 'ai.diverge'),
                                               sum(1 for f in report if f['mission'] == m[1] and f['kind'] == 'ai.timing')), flush=True)
    # repeatability: ours again on the first good mission, the same ai.diverge keys
    good = [m for m in ms if m[1] not in bad]
    if good:
        m = good[0]
        again = os.path.join(a.out, '_null', m[1])
        os.makedirs(again, exist_ok=True)
        shutil.rmtree(os.path.join(again, 'ge'), ignore_errors=True)
        subprocess.run(['cp', '-al', os.path.join(a.out, m[1], 'ge'), os.path.join(again, 'ge')], check=True)
        if trace('pd', m, a, again, aidiff.rundir_for(m[1], binary)):
            problems.append('repeatability: the second trace of ours failed on %s' % m[1])
        else:
            f2, p2, _ = aidiff.analyse(m[0], again, a.ticks)
            k1 = sorted(f['key'] for f in report if f['mission'] == m[1] and f['kind'] == 'ai.diverge')
            k2 = sorted(f['key'] for f in to_gate(m[1], f2, a.ticks) if f['kind'] == 'ai.diverge')
            if p2 or k1 != k2:
                problems.append('repeatability: ours traced twice on %s gave different ai.diverge keys (%s / %s)%s' % (
                    m[1], k1, k2, ' and %s' % p2 if p2 else ''))
            else:
                print('null: ours traced twice on %s gives the same %d ai.diverge keys' % (m[1], len(k1)))
    json.dump(report, open(os.path.join(a.out, 'report.json'), 'w'), indent=1)
    lines = ['# AI trace diff (gate leg): %s against %s, %d missions, %d ticks' % (a.bin, a.oracle, len(ms), a.ticks), '']
    if problems:
        lines += ['**NULL FAILED / not compared:**'] + ['- ' + p for p in problems] + ['']
    for f in sorted(report, key=lambda f: (f['kind'], f['mission'], -f['mag'])):
        lines.append('- %s %s %s (%.0f): %s' % (f['mission'], f['kind'], f['key'], f['mag'], f['detail']))
    open(os.path.join(a.out, 'report.md'), 'w').write('\n'.join(lines) + '\n')
    if problems:
        print('NULL FAILED:', *problems, sep='\n  ')
        return 2
    print('report:', os.path.join(a.out, 'report.json'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
