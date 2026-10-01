#!/usr/bin/env python3
"""Compare the AI commands GoldenEye runs with the ones the conversion runs,
chr by chr, for one mission's first minute or two.

    aidiff.py --mission dam --run                 # trace both sides, then compare
    aidiff.py --mission dam                       # compare traces already in OUT
    aidiff.py --mission dam --null-twice          # the expensive null: ours traced twice

Traces: ai/trace.py through twin.py (GoldenEye's PORT_AI_TRACE plus a marker
per list run; our --ai-trace). The map from GoldenEye's (list, offset) to ours
is ai/aimap.py's, taken from the converter's own walk.

How the two are made comparable:
- every command becomes (our list id, GoldenEye offset); a command the
  conversion leaves out is dropped from GoldenEye's stream (it cannot run in
  ours) and a command the conversion adds (StartPatrol's second word) from
  ours;
- one list run (a chr's AI from its resume point to the Yield) is one step;
  GoldenEye runs its AI 20-30 times a second and ours 60, so a run repeated
  unchanged is folded into one, and only a change of what the list does
  counts;
- chrs are matched by number; background lists (GoldenEye's chr 254, ours
  4000 and up) and vehicles by the first list they run.

For each chr the report gives the first step where the two differ, the steps
before it, both sides' next commands by name (GoldenEye's and the converted
one), and a class from the command the two last agreed on - the branch that
went another way:
  rng     a random draw (IFRandom*, *LessThanRandom): expected, not hidden
  timing  timers, countdowns, fades, animation progress, the intro camera,
          and a split after a command that decides nothing (Yield, Label,
          a goto): one side ran the list again before moving on
  world   sight, hearing, distance, rooms, health, doors, objects, TRY* moves
  logic   anything else: flags, objectives, presets, difficulty - a script
          that should run the same way every time; a logic divergence is the
          one to read first
  window-end  one side's trace stopped still in a loop and the other did
          something new in the window's last tenth
plus the commands one side ran and the other never did in the whole trace.

Nulls, every run: GoldenEye's traced commands must each be the opcode its
bytecode holds at that list and offset (the list tracking is right), ours must
each be the converted command at that offset (the map is right), every stream
must align with itself, and a copy with one command planted mid-stream must be
reported at exactly that step. Any failure exits 2.
"""
import argparse, copy, difflib, json, os, re, subprocess, sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'common'))
import levels  # noqa: E402
import aimap  # noqa: E402

OUTBASE = os.path.expanduser(os.environ.get('GF_AI_OUT', '~/wt/gefidelity-run/ai-out'))
BASE_RUNDIR = os.path.expanduser(os.environ.get('GF_RUNDIR', '~/wt/gefidelity-run'))


def rundir_for(key, binary='pd.base'):
    """A run directory of this mission's own: pd.log and screenshots go beside
    the binary, so two runs in one directory write over each other's trace.
    The binary (a path, or a name in the base run directory) is hard-linked in,
    the rest symlinks; another binary gets directories of its own."""
    src = binary if os.path.isabs(binary) else os.path.join(BASE_RUNDIR, binary)
    src = os.path.realpath(src)
    tag = '' if src == os.path.realpath(os.path.join(BASE_RUNDIR, 'pd.base')) else \
        '-' + __import__('hashlib').sha1(src.encode()).hexdigest()[:8]
    d = os.path.join(BASE_RUNDIR, 'ai-rundir%s-%s' % (tag, key))
    os.makedirs(d, exist_ok=True)
    dst = os.path.join(d, 'pd.base')
    if not os.path.exists(dst) or os.stat(dst).st_ino != os.stat(src).st_ino:
        if os.path.exists(dst):
            os.unlink(dst)
        try:
            os.link(src, dst)
        except OSError:
            __import__('shutil').copy2(src, dst)    # another filesystem
    for sub in ('data', 'added-content', 'mods'):
        p = os.path.join(d, sub)
        if not os.path.lexists(p):
            os.symlink(os.path.join(BASE_RUNDIR, sub), p)
    return d
DEFAULT_TICKS = 3600

GE_SETLIST, GE_SETRETURN, GE_RETURN = 0x05, 0x06, 0x07
CHR_SELF = 0xfd

RNG_WORDS = ('Random',)
TIMING_WORDS = ('Timer', 'MissionTime', 'SystemPowerTime', 'HudCountdown', 'IFPlayingAnimation',
                'IFScreenFadeCompleted', 'IFSfxChannelVolume', 'IFCreditsHasCompleted', 'IFCameraIsIn')
WORLD_WORDS = ('See', 'Saw', 'Heard', 'Hear', 'Shot', 'Die', 'Dying', 'OnScreen', 'Targeted', 'Missed', 'Angle',
               'Distance', 'InRoom', 'Damaged', 'Arghs', 'Health', 'Firing', 'Alarm', 'Gas', 'DoesNotExist',
               'Collected', 'Dropped', 'Attached', 'Equipped', 'Activated', 'Exists', 'Destroyed', 'Door',
               'OnPatrolOrStopped', 'TRY', 'InTank', 'Ammo', 'BondIsDead', 'Killed', 'UsedGadget', 'NumberOfActive')


NOT_A_BRANCH = ('Yield', 'Label', 'GotoFirst', 'GotoNext', 'EndList', 'SetMyChrNum')


def classify(name):
    if not name or name in NOT_A_BRANCH:
        # the two last agreed on a command that decides nothing: one side ran
        # the list once more (or once fewer) before going on - when, not what
        return 'timing'
    if any(w in name for w in RNG_WORDS):
        return 'rng'
    if any(w in name for w in TIMING_WORDS):
        return 'timing'
    if any(w in name for w in WORLD_WORDS):
        return 'world'
    return 'logic'


# ------------------------------------------------------------------ parsing

GE_AI = re.compile(r'^\[ai\] block \d+ chr (-?\d+): offset +(\d+) cmd +(\d+) random (\d+)')
GE_MARK = re.compile(r'^GFAI (\d+) ([co]) (-?\d+) (\d+) (-?\d+)(g?) (\d+) (-?\d+)')
# %#06x prints a zero as 000000, with no 0x: GotoNext is command 0
PD_AI = re.compile(r'ai: f(\d+) (?:chr (-?\d+) |prop )list (-?\d+)(g?) \+(\d+) cmd (0x[0-9a-fA-F]+|0+)\b')


class Trace:
    """Per-stream list of runs; a run is (tick, [event, ...]); an event is
    (our list id, GoldenEye offset) for a mapped command or ('?', side, list, offset, op)."""

    def __init__(self, side):
        self.side = side
        self.streams = defaultdict(list)
        self.names = {}
        self.bad = []          # null failures: a traced command that is not what the bytecode holds
        self.dropped = Counter()
        self.lines = 0


def parse_ge(path, m):
    tr = Trace('ge')
    t0 = 0
    cur = None              # [key, pid, ret, tick, events, addr]
    first_list = {}         # entity -> first list
    pending = []
    # a chr slot is reused: when a slot's marker names another chr, it is a new
    # occupant (a new stream) unless its last run renamed it (SetMyChrNum)
    slot_who, slot_gen, renamed = {}, {}, set()
    GE_SETMYCHRNUM = next(i for i, r in enumerate(aimap.geaitable.TABLE) if r[0] == 'SetMyChrNum')

    def close():
        if cur and cur[4]:
            pending.append((cur[0], cur[3], cur[4], cur[5]))

    for line in open(path, errors='replace'):
        if line.startswith('GFAI t0 '):
            t0 = int(line.split()[2])
            continue
        mk = GE_MARK.match(line)
        if mk:
            close()
            timer, kind, who, addr, lid, glob, off, ret = mk.groups()
            lid = int(lid)
            pid = m.pid_of_ge(lid) if not glob else aimap.gesolo.global_ai_id(lid)
            sk = (kind, addr)
            if sk in slot_who and slot_who[sk] != int(who) and sk not in renamed:
                slot_gen[sk] = slot_gen.get(sk, 0) + 1
            renamed.discard(sk)
            slot_who[sk] = int(who)
            ent = (kind, int(who), '%s.%d' % (addr, slot_gen.get(sk, 0)))
            if lid >= 0:
                first_list.setdefault(ent, pid)
            retpid = m.pid_of_ge(int(ret)) if int(ret) >= 0 else None
            cur = [ent, pid, retpid, int(timer) - t0, [], ent]
            continue
        ma = GE_AI.match(line)
        if not ma or cur is None:
            continue
        tr.lines += 1
        off, op = int(ma.group(2)), int(ma.group(3))
        pid = cur[1]
        c = m.ge_to_cmd.get((pid, off))
        if c is None or c['op'] != op:
            # a list switch the walk did not see: find a list with this opcode at this offset
            tr.bad.append('GoldenEye chr %s list %s +%d: traced op 0x%02x, bytecode has %s' % (
                cur[0][1], pid, off, op, '0x%02x' % c['op'] if c else 'nothing'))
            cur[4].append(('?', 'ge', pid, off, op))
            continue
        if c['pdlen']:
            cur[4].append((pid, off))
        else:
            tr.dropped[(pid, off, c['name'])] += 1
        if op == GE_SETMYCHRNUM:
            renamed.add((cur[0][0], cur[0][2].split('.')[0]))
        # follow the list switches the walk makes inside one run
        L = m.lists.get(pid)
        gb = L['gebytes'] if L else b''
        if isinstance(gb, str):
            gb = bytes.fromhex(gb)
        if op == GE_SETLIST and off + 3 < len(gb) and gb[off + 1] == CHR_SELF:
            cur[1] = m.pid_of_ge(int.from_bytes(gb[off + 2:off + 4], 'big'))
        elif op == GE_SETRETURN and off + 2 < len(gb):
            cur[2] = m.pid_of_ge(int.from_bytes(gb[off + 1:off + 3], 'big'))
        elif op == GE_RETURN and cur[2] is not None:
            cur[1] = cur[2]
    close()
    # name the streams by the entity (its record address), with the name its
    # first run had: SetMyChrNum renames a chr while it runs (a background or
    # spawned chr taking a setup chr's number) and it is still one runner.
    # A chr by its number; GoldenEye's background lists all run as chr 254.
    first_who, names = {}, {}
    for ent, tick, events, _ in pending:
        first_who.setdefault((ent[0], ent[2]), ent)
    for ent, tick, events, _ in pending:
        e0 = first_who[(ent[0], ent[2])]
        if e0 not in names:
            if e0[0] == 'o':
                names[e0] = 'obj@%s' % (events[0][0] if events and events[0][0] != '?' else '?')
            elif e0[1] == 254:
                names[e0] = 'bg@%s' % first_list.get(e0, first_list.get(ent, '?'))
            elif e0[1] >= 5000:
                names[e0] = 'spawn@%s' % first_list.get(e0, first_list.get(ent, '?'))
            else:
                names[e0] = 'chr%d' % e0[1]
        tr.streams[names[e0]].append((tick, events))
    return tr


def parse_pd(path, m):
    tr = Trace('pd')
    runs = []               # (key-ish, frame, events)
    last = None
    first_list = {}
    alias = {}              # a chrnum SetMyChrNum gave -> the chrnum the runner had
    setnum = {c['pdop'] for L in m.lists.values() for c in L['cmds'] if c['name'] == 'SetMyChrNum' and c['pdop'] is not None}
    for line in open(path, errors='replace'):
        mm = PD_AI.search(line)
        if not mm:
            continue
        tr.lines += 1
        f, chrn, lid, glob, off, cmd = mm.groups()
        f, off, cmd, lid = int(f), int(off), int(cmd, 16), int(lid)
        if chrn is not None:
            n = int(chrn)
            if last is not None and last[0][0] == 'c' and last[1] == f and last[3] in setnum and n != last[0][1]:
                alias[n] = alias.get(last[0][1], last[0][1])
            who = ('c', alias.get(n, n))
        else:
            who = ('o', None)
        if last is not None:
            last[3] = cmd
        if last is None or last[0] != who or last[1] != f:
            last = [who, f, [], cmd]
            runs.append(last)
            if who[0] == 'c':
                first_list.setdefault(who[1], ('g%d' % lid) if glob else lid)
        if glob:
            last[2].append(('?', 'pd', 'g%d' % lid, off, cmd))
            continue
        c = m.pd_to_cmd.get((lid, off))
        if c is None:
            L = m.lists.get(lid)
            inside = L and any(x['pd'] < off < x['pd'] + x['pdlen'] for x in L['cmds'])
            if inside:
                tr.dropped[('added', lid, off)] += 1
                continue
            tr.bad.append('ours list %d +%d cmd 0x%04x: no converted command starts there' % (lid, off, cmd))
            last[2].append(('?', 'pd', lid, off, cmd))
            continue
        if c['pdop'] != cmd:
            tr.bad.append('ours list %d +%d: traced cmd 0x%04x, converted 0x%04x (%s)' % (lid, off, cmd, c['pdop'], c['name']))
        last[2].append((lid, c['ge']))
    for who, f, events, _ in runs:
        if not events:
            continue
        if who[0] == 'o':
            key = 'obj@%s' % (events[0][0] if events[0][0] != '?' else '?')
        elif 4000 <= who[1] < 5000:
            key = 'bg@%s' % first_list[who[1]]
        elif who[1] >= 5000:
            key = 'spawn@%s' % first_list[who[1]]
        else:
            key = 'chr%d' % who[1]
        tr.streams[key].append((f, events))
    return tr


# ---------------------------------------------------------------- comparing

def fold(runs):
    """Consecutive identical runs folded: [(first tick, events, count)]."""
    out = []
    for tick, ev in runs:
        ev = tuple(ev)
        if out and out[-1][1] == ev:
            out[-1][2] += 1
        else:
            out.append([tick, ev, 1])
    return out


def prefix(x, y):
    k = 0
    while k < min(len(x), len(y)) and x[k] == y[k]:
        k += 1
    return k


def first_split(a, b):
    """Index of the first folded step that differs, or None."""
    for i in range(min(len(a), len(b))):
        if a[i][1] != b[i][1]:
            return i
    if len(a) != len(b):
        return min(len(a), len(b))
    return None


def name_of(m, ev):
    if ev and ev[0] == '?':
        return '%s-only:%s+%s(0x%x)' % (ev[1], ev[2], ev[3], ev[4])
    c = m.ge_to_cmd.get(ev)
    return '%s@%s+%d' % (c['name'], ev[0], ev[1]) if c else '%s+%s' % ev


def pd_name(m, pdnames, ev):
    if ev and ev[0] == '?':
        return ''
    c = m.ge_to_cmd.get(ev)
    if not c or c['pdop'] is None:
        return ''
    return pdnames.get(c['pdop'], '0x%04x' % c['pdop'])


def split_at(m, pdnames, a, i, b, j, tag='walk'):
    """What happened where GoldenEye's step i and our step j part. tag is
    difflib's: 'delete' means GoldenEye ran steps ours did not (there is no
    step of ours to compare with), 'insert' the other way round."""
    ea = a[i][1] if i < len(a) and tag != 'insert' else ()
    eb = b[j][1] if j < len(b) and tag != 'delete' else ()
    k = prefix(ea, eb)
    branch = ea[k - 1] if k else None
    if k == 0:
        # the two differ from the run's first command: one side went round
        # its loop again (the previous step, folded) while the other left it,
        # or one side's trace ended still in the loop. The branch is where the
        # side that moved on parted from the loop body.
        best = 0
        for x, y in ((ea, b[j - 1][1] if j else ()), (eb, a[i - 1][1] if i else ())):
            kk = prefix(x, y)
            if x and kk > best:
                best, branch = kk, x[kk - 1]
        if branch is None and i and a[i - 1][1]:
            branch = a[i - 1][1][-1]
    bname = m.ge_to_cmd[branch]['name'] if branch and branch[0] != '?' and branch in m.ge_to_cmd else ''
    return {
        'class': classify(bname), 'ge_step': i, 'pd_step': j,
        'ge_tick': a[i][0] if i < len(a) else None, 'pd_tick': b[j][0] if j < len(b) else None,
        'trace_ended': 'GoldenEye' if i >= len(a) else 'ours' if j >= len(b) else None,
        'tag': tag,
        'branch': name_of(m, branch) if branch else '',
        'context': [[name_of(m, e) for e in a[q][1]] + (['x%d' % a[q][2]] if a[q][2] > 1 else [])
                    for q in range(max(0, i - 3), min(i, len(a)))],
        'ge_next': [name_of(m, e) for e in ea[k:k + 6]] or ['(stream ends)'],
        'pd_next': [name_of(m, e) for e in eb[k:k + 6]] or ['(stream ends)'],
        'pd_next_ours': [pd_name(m, pdnames, e) for e in eb[k:k + 6]]}


def cadence(ge, pd):
    """How often each side runs a chr's AI: per chr stream present on both,
    the median gap between its runs in ticks, and the median of the ratio
    ours/GoldenEye. GoldenEye wakes a chr when its sleep runs out; a different
    sleep rule makes every guard react sooner or later than GoldenEye's."""
    import statistics
    rows = []
    for key in sorted(set(ge.streams) & set(pd.streams)):
        if not key.startswith('chr'):
            continue
        ga = [t for t, _ in ge.streams[key]]
        pa = [t for t, _ in pd.streams[key]]
        if len(ga) < 20 or len(pa) < 20:
            continue
        gg = statistics.median(y - x for x, y in zip(ga, ga[1:]))
        pg = statistics.median(y - x for x, y in zip(pa, pa[1:]))
        if gg > 0:
            rows.append((key, gg, pg, pg / gg))
    ratio = statistics.median(r[3] for r in rows) if rows else None
    return {'ratio': ratio, 'chrs': [{'stream': k, 'ge_gap': g, 'pd_gap': p} for k, g, p, _ in rows]}


def compare(ge, pd, m, pdnames, mission, ticks=None):
    findings = []
    margin = max(120, ticks // 10) if ticks else 0
    keys = sorted(set(ge.streams) | set(pd.streams), key=lambda k: (k[:3], len(k), k))
    for key in keys:
        a, b = fold(ge.streams.get(key, [])), fold(pd.streams.get(key, []))
        f = {'mission': mission, 'stream': key, 'ge_steps': len(a), 'pd_steps': len(b)}
        if not a or not b:
            f.update({'kind': 'missing', 'side': 'ours' if not b else 'GoldenEye',
                      'detail': '%s runs in %s only' % (key, 'GoldenEye' if a else 'ours'), 'class': 'logic',
                      'first': name_of(m, (a or b)[0][1][0]) if (a or b) and (a or b)[0][1] else ''})
            findings.append(f)
            continue
        i = first_split(a, b)
        cova = Counter(e for _, ev, _ in a for e in ev)
        covb = Counter(e for _, ev, _ in b for e in ev)
        only_ge = sorted(e for e in cova if e not in covb and e[0] != '?')
        only_pd = sorted(e for e in covb if e not in cova and e[0] != '?')
        f['ge_only'] = [name_of(m, e) for e in only_ge][:12]
        f['pd_only'] = [name_of(m, e) for e in only_pd][:12]
        f['unmapped'] = sorted({name_of(m, e) for _, ev, _ in a + b for e in ev if e[0] == '?'})[:8]
        if i is None:
            if only_ge or only_pd or f['unmapped']:
                f.update({'kind': 'coverage', 'class': 'logic', 'detail': 'same steps, different commands'})
                findings.append(f)
            continue
        A, B = [x[1] for x in a], [x[1] for x in b]
        sm = difflib.SequenceMatcher(None, A, B, autojunk=False)
        regions = []
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == 'equal':
                continue
            regions.append(split_at(m, pdnames, a, i1, b, j1, tag))
        # the first region by step; difflib may line the start up differently
        # from a straight walk, so the straight walk's split leads
        lead = split_at(m, pdnames, a, i, b, i)
        later = [r for r in regions if (r['ge_step'], r['pd_step']) != (i, i)]
        for r in [lead] + later:
            # one side's trace stopped and the other did something new in the
            # last stretch of the window, or only went on round the cycle the
            # stopped side was in (each side wakes a chr at its own rate, so
            # the same loop is more steps on one): the windows end, not the
            # scripts
            if not r['trace_ended']:
                continue
            t = r['pd_tick'] if r['trace_ended'] == 'GoldenEye' else r['ge_tick']
            longer, shorter, at = (b, a, r['pd_step']) if r['trace_ended'] == 'GoldenEye' else (a, b, r['ge_step'])
            recent = {x[1] for x in shorter[-30:]}
            if (margin and t is not None and t >= ticks - margin) or all(x[1] in recent for x in longer[at:]):
                r['class'] = 'window-end'
        det = next((r for r in [lead] + later if r['class'] in ('logic', 'world')), None)
        f.update(lead)
        f.update({'kind': 'split', 'step': i, 'similarity': round(sm.ratio(), 3),
                  'regions': dict(Counter(r['class'] for r in [lead] + later)),
                  'first_deterministic': det if det is not lead else None})
        f['detail'] = '%s at step %d (GoldenEye tick %s, ours %s): after %s GoldenEye ran %s, ours %s' % (
            key, i, f['ge_tick'], f['pd_tick'], f['branch'] or 'the run start',
            f['ge_next'][0], f['pd_next'][0])
        findings.append(f)
    return findings


# --------------------------------------------------------------------- nulls

def nulls(ge, pd, m, pdnames):
    problems = []
    if ge.bad:
        problems.append('%d GoldenEye commands are not what the bytecode holds (first: %s)' % (len(ge.bad), ge.bad[0]))
    if pd.bad:
        problems.append('%d of our commands do not map (first: %s)' % (len(pd.bad), pd.bad[0]))
    if not ge.lines or not pd.lines:
        problems.append('a side traced nothing (GoldenEye %d lines, ours %d)' % (ge.lines, pd.lines))
        return problems
    for tr in (ge, pd):
        self_f = [f for f in compare(tr, tr, m, pdnames, 'null') if f['kind'] != 'coverage']
        if self_f:
            problems.append('%s against itself: %d findings' % (tr.side, len(self_f)))
    # plant one command mid-stream in a copy of ours and find it at that step
    key = max(pd.streams, key=lambda k: len(fold(pd.streams[k])))
    folded = fold(pd.streams[key])
    if len(folded) >= 3:
        k = len(folded) // 2
        tick = folded[k][0]
        planted = copy.deepcopy(pd)
        runs = planted.streams[key]
        idx = next(n for n, (t, ev) in enumerate(runs) if t == tick)
        runs[idx] = (runs[idx][0], list(runs[idx][1]) + [('?', 'pd', 'planted', 0, 0)])
        got = [f for f in compare(pd, planted, m, pdnames, 'null') if f['stream'] == key and f['kind'] == 'split']
        if not got or got[0]['step'] != k:
            problems.append('a command planted at step %d of %s was %s' % (
                k, key, 'not found' if not got else 'found at step %s' % got[0]['step']))
    else:
        problems.append('no stream long enough to plant a command in')
    return problems


# ------------------------------------------------------------------- running

def run_traces(mission, out, ticks, invisible, timeout, binary='pd.base', oracle='ares'):
    rundir = rundir_for(levels.mission(mission)[1], binary)
    env = ['--env', 'PORT_AI_TRACE=0:2000000000', '--env', 'GF_AI_TICKS=%d' % ticks]
    if invisible:
        env += ['--env', 'GF_AI_INVISIBLE=1']
    cmd = [sys.executable, os.path.join(ROOT, 'twin.py'), 'both', os.path.join(HERE, 'trace.py'),
           '--mission', str(mission), '--out', out, '--rundir', rundir, '--timeout', str(timeout),
           '--pd-arg=--ai-trace', '--pd-arg=%d' % (ticks + 10), '--oracle', oracle] + env
    if os.environ.get('GF_NO_SYNC'):
        cmd.append('--no-sync')
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(r.stdout.strip())
    return r.returncode


def markdown(mname, findings, ge, pd, problems, ticks, cad=None):
    lines = ['## %s' % mname, '',
             'GoldenEye %d commands in %d streams, ours %d in %d; %d left out by the conversion ran in GoldenEye; '
             'first %d ticks.' % (ge.lines, len(ge.streams), pd.lines, len(pd.streams), sum(ge.dropped.values()), ticks), '']
    if cad and cad['ratio']:
        c = cad['chrs']
        lines += ['AI cadence: a chr\'s list runs every %.1f ticks in GoldenEye and every %.1f in ours (medians over %d '
                  'chrs; ours/GoldenEye %.2f)%s.' % (
                      sorted(x['ge_gap'] for x in c)[len(c) // 2], sorted(x['pd_gap'] for x in c)[len(c) // 2],
                      len(c), cad['ratio'], ' - **ours thinks at a different rate**' if abs(cad['ratio'] - 1) > 0.1 else ''), '']
    if problems:
        lines += ['**NULL FAILED:** ' + '; '.join(problems), '']
    order = {'logic': 0, 'unexplained': 0, 'world': 1, 'timing': 2, 'rng': 3, 'window-end': 4}
    fs = sorted(findings, key=lambda f: (f['kind'] != 'missing', order.get(f['class'], 9), f.get('ge_tick') or 0))
    if not fs:
        lines.append('Every stream runs the same commands.')
    for f in fs:
        if f['kind'] == 'missing':
            lines.append('- **missing** `%s` - %s (first: %s)' % (f['stream'], f['detail'], f.get('first', '')))
            continue
        if f['kind'] == 'coverage':
            lines.append('- **coverage** `%s` - same steps; GoldenEye only %s, ours only %s, unmapped %s' % (
                f['stream'], f['ge_only'], f['pd_only'], f['unmapped']))
            continue
        lines.append('- **%s** `%s` step %d, GoldenEye tick %s / ours %s, similarity %.2f' % (
            f['class'], f['stream'], f['step'], f['ge_tick'], f['pd_tick'], f['similarity']))
        if f.get('mode') == 'resume':
            lines.append('  - from `%s`: GoldenEye next stopped at `%s`, ours at `%s`' % (f['from'], f['ge_next'][0], f['pd_next'][0]))
            lines.append('  - went the other way at: `%s`' % (f['branch'] or '(nothing on our run reaches GoldenEye\'s stop)'))
            if f.get('pd_run'):
                lines.append('  - our run from there: `%s`' % ' '.join(f['pd_run'][:14]))
            if f.get('first_deterministic'):
                d = f['first_deterministic']
                lines.append('  - later, a **%s** split at GoldenEye step %d / ours %d (ticks %s / %s): from `%s` GoldenEye `%s`, '
                             'ours `%s`, at `%s`' % (d['class'], d['ge_step'], d['pd_step'], d['ge_tick'], d['pd_tick'],
                                                     d['from'], d['ge_next'][0], d['pd_next'][0], d['branch']))
            lines.append('  - all splits by class: %s' % f.get('regions'))
            continue
        lines.append('  - after: `%s`' % (f['branch'] or '(start of run)'))
        if f['context']:
            lines.append('  - before: ' + ' / '.join('`%s`' % ' '.join(c) for c in f['context'][-2:]))
        lines.append('  - GoldenEye next: `%s`' % ' '.join(f['ge_next']))
        lines.append('  - ours next: `%s` (ours: %s)' % (' '.join(f['pd_next']), ' '.join(x for x in f['pd_next_ours'] if x)))
        if f.get('first_deterministic'):
            d = f['first_deterministic']
            lines.append('  - later, a **%s** split at GoldenEye step %d / ours %d (ticks %s / %s): after `%s` '
                         'GoldenEye `%s`, ours `%s`' % (d['class'], d['ge_step'], d['pd_step'], d['ge_tick'], d['pd_tick'],
                                                         d['branch'] or '(start of run)', ' '.join(d['ge_next'][:4]),
                                                         ' '.join(d['pd_next'][:4])))
        if f.get('regions'):
            lines.append('  - all splits by class: %s' % f['regions'])
        if f['ge_only'] or f['pd_only']:
            lines.append('  - ran in GoldenEye only: %s; in ours only: %s' % (f['ge_only'][:6], f['pd_only'][:6]))
        if f['unmapped']:
            lines.append('  - unmapped: %s' % f['unmapped'])
    lines.append('')
    return '\n'.join(lines)


def analyse(mission, out, ticks=DEFAULT_TICKS):
    """Compare the traces in OUT (ge/gdb.log, pd/pd.log) for one mission, write
    OUT/report.{md,json}, and return (findings, problems, info)."""
    mm = levels.mission(mission)
    m = aimap.load(mm[0])
    pdnames = aimap.pd_command_names()
    gelog = os.path.join(out, 'ge', 'gdb.log')
    ares = any(l.startswith('GFAS ') for l in open(gelog, errors='replace'))
    pd = parse_pd(os.path.join(out, 'pd', 'pd.log'), m)
    if ares:
        import resume
        ge = resume.parse_ares(gelog, m)
        pdfull = pd
        pd, runs = resume.resume_of_pd(pdfull, m)
        problems = resume.nulls(ge, pd, runs, m, pdnames, ticks)
    else:
        ge = parse_ge(gelog, m)
        problems = nulls(ge, pd, m, pdnames)
    if ares:
        findings = resume.compare(ge, pd, runs, m, pdnames, mm[1], ticks)
        cad = {'ratio': None, 'chrs': []}      # the cartridge is sampled a frame at a time, not run by run
    else:
        findings = compare(ge, pd, m, pdnames, mm[1], ticks)
        cad = cadence(ge, pd)
    md = markdown(mm[2] + (' (GoldenEye on the cartridge, ares)' if ares else ' (GoldenEye on the native port)'),
                  findings, ge, pd, problems, ticks, cad)
    open(os.path.join(out, 'report.md'), 'w').write(md)
    json.dump({'mission': mm[1], 'oracle': 'ares' if ares else 'port', 'problems': problems, 'findings': findings, 'cadence': cad,
               'dropped_ge': {'%s+%s %s' % k: v for k, v in ge.dropped.items() if k[0] != 'added'}},
              open(os.path.join(out, 'report.json'), 'w'), indent=1)
    return findings, problems, {'ares': ares, 'ge': ge, 'pd': pd, 'cadence': cad, 'map': m, 'pdnames': pdnames}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mission', required=True)
    ap.add_argument('--out')
    ap.add_argument('--run', action='store_true', help='trace both sides first')
    ap.add_argument('--ticks', type=int, default=DEFAULT_TICKS)
    ap.add_argument('--invisible', action='store_true', help='guards cannot see Bond on either side')
    ap.add_argument('--null-twice', action='store_true', help='also trace ours a second time and require a full match')
    ap.add_argument('--timeout', type=int, default=2400)
    ap.add_argument('--bin', default='pd.base', help='our binary, in the base run directory')
    ap.add_argument('--oracle', choices=['ares', 'port'], default='ares',
                    help='GoldenEye side: the cartridge in ares (resume points; default) or the native port (every command)')
    a = ap.parse_args()
    mm = levels.mission(a.mission)
    out = a.out or os.path.join(OUTBASE, mm[1])
    if a.run:
        if run_traces(mm[0], out, a.ticks, a.invisible, a.timeout, a.bin, a.oracle):
            print('a trace failed; see %s/*/gdb.log' % out)
            return 2
    findings, problems, info = analyse(mm[0], out, a.ticks)
    ge, pd, cad, ares, m, pdnames = info['ge'], info['pd'], info['cadence'], info['ares'], info['map'], info['pdnames']
    if a.null_twice:
        out2 = out + '-again'
        subprocess.run([sys.executable, os.path.join(ROOT, 'twin.py'), 'pd', os.path.join(HERE, 'trace.py'),
                        '--mission', str(mm[0]), '--out', out2, '--rundir', rundir_for(mm[1], a.bin),
                        '--env', 'GF_AI_TICKS=%d' % a.ticks, '--pd-arg=--ai-trace', '--pd-arg=%d' % (a.ticks + 10)],
                       capture_output=True)
        p1 = parse_pd(os.path.join(out, 'pd', 'pd.log'), m)
        p2 = parse_pd(os.path.join(out2, 'pd', 'pd.log'), m)
        # ours is deterministic under --fixed-step --rng-seed: twice must be identical, every class
        twice = compare(p1, p2, m, pdnames, 'null')
        if twice:
            problems.append('ours traced twice: %d findings (%s)' % (len(twice), twice[0].get('detail', twice[0]['stream'])))
        else:
            print('null: ours traced twice aligns fully')
    if problems:
        print('NULL FAILED:', *problems, sep='\n  ')
        return 2
    print('null: GoldenEye %d %s and ours %d commands all map; self-alignment clean; planted step found' % (
        ge.lines, 'samples' if ares else 'commands', pd.lines))
    c = Counter(f['class'] for f in findings if f['kind'] == 'split')
    print('%s: %d streams differ (%s), %d missing, %d coverage-only, cadence ours/GoldenEye %s -> %s' % (
        mm[1], sum(c.values()), ', '.join('%s %d' % kv for kv in sorted(c.items())),
        sum(1 for f in findings if f['kind'] == 'missing'), sum(1 for f in findings if f['kind'] == 'coverage'),
        '%.2f' % cad['ratio'] if cad['ratio'] else '-', os.path.join(out, 'report.md')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
