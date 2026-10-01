"""The AI trace diff against the cartridge (ares), where only where each list
SITS can be seen, not each command it runs.

ares hooks function entries only, so on the cartridge trace.py reads every
chr's (ailist, aioffset) each video frame instead of tracing commands. A run
of a chr's list ends at a Yield and GoldenEye runs a list at most once a
frame, so that trail is the chain of Yields each list reached. Ours is traced
command by command; each run's first command is the same thing, a resume
point. The two chains are compared as aidiff compares runs, and where they
part, the branch is recovered from GoldenEye's own bytecode: walking our run
from the last common resume point, the latest conditional whose other way
leads (by the list's control flow, no Yield crossed) to where GoldenEye
stopped is the command that went the other way. Its name gives the class, as
in aidiff.

Nulls, every run: each of GoldenEye's samples must be a resume point the
bytecode allows - offset 0, or the command just after a Yield - in a list the
map knows; ours as aidiff's; each chain aligns with itself; a planted step is
found where it was put.
"""
import copy, difflib, re
from collections import Counter, defaultdict

import aidiff
from aidiff import Trace, fold, first_split, classify, name_of, pd_name

GE_AS = re.compile(r'^GFAS (\d+) (\S+) (-?\d+) (\d+)')
CHR_SELF = 0xfd


class Flow:
    """GoldenEye's control flow over one mission's lists (the map's bytes)."""

    def __init__(self, m):
        self.m = m
        self.cmds = {}
        for pid, L in m.lists.items():
            gb = L['gebytes'] if isinstance(L['gebytes'], bytes) else bytes.fromhex(L['gebytes'])
            cs = L['cmds']
            rows = {}
            for i, c in enumerate(cs):
                end = cs[i + 1]['ge'] if i + 1 < len(cs) else len(gb)
                rows[c['ge']] = (c, gb[c['ge']:end])
            self.cmds[pid] = rows

    def label(self, pid, frm, lab):
        for o in sorted(self.cmds[pid]):
            if o < frm:
                continue
            c, b = self.cmds[pid][o]
            if c['name'] == 'Label' and len(b) > 1 and b[1] == lab:
                return o
            if c['name'] == 'EndList':
                return None
        return None

    def succ(self, pid, o):
        """The offsets a command can go on to within its list, and whether it
        ends the run here (Yield) or leaves the list."""
        c, b = self.cmds[pid][o]
        n = c['name']
        nxt = o + len(b)
        if n in ('Yield', 'EndList', 'Return', 'Stop'):
            return []
        if n == 'SetChrAiList' and len(b) > 1 and b[1] == CHR_SELF:
            return []
        if n == 'GotoNext':
            t = self.label(pid, o, b[1])
            return [t] if t is not None else []
        if n == 'GotoFirst':
            t = self.label(pid, 0, b[1])
            return [t] if t is not None else []
        row = aidiff.aimap.geaitable.TABLE[c['op']]
        if any(a == 'GOTOLABEL' for a, _ in row[2]):
            t = self.label(pid, o, b[-1])
            return [nxt] + ([t] if t is not None else [])
        return [nxt]

    def reaches(self, pid, start, goal):
        """Can the list get from `start` to resuming at goal (pid, off) without
        a Yield in between: the Yield just before goal's offset in its own
        list, or a SetChrAiList to goal's list."""
        gpid, goff = goal
        seen, todo = set(), [start]
        while todo:
            o = todo.pop()
            if o is None or o in seen or o not in self.cmds[pid]:
                continue
            seen.add(o)
            c, b = self.cmds[pid][o]
            if c['name'] == 'Yield':
                if pid == gpid and o + 1 == goff:
                    return True
                continue
            if c['name'] == 'SetChrAiList' and len(b) > 3 and b[1] == CHR_SELF:
                lid = int.from_bytes(b[2:4], 'big')
                if self.m.pid_of_ge(lid) == gpid:
                    return True
                continue
            todo.extend(self.succ(pid, o))
        return False


def canon(m, pid, off):
    if pid == '?':
        return (pid, off)
    """A resume point as the first converted command at or after it (a command
    the conversion leaves out can never be where ours resumes)."""
    L = m.lists.get(pid)
    if not L:
        return (pid, off)
    for c in L['cmds']:
        if c['ge'] >= off and c['pdlen']:
            return (pid, c['ge'])
    return (pid, off)


def parse_ares(path, m):
    tr = Trace('ge')
    for line in open(path, errors='replace'):
        mm = GE_AS.match(line)
        if not mm:
            continue
        tick, key, pid, off = int(mm.group(1)), mm.group(2), int(mm.group(3)), int(mm.group(4))
        tr.lines += 1
        L = m.lists.get(pid)
        if L is None:
            tr.bad.append('GoldenEye %s at tick %d sits in list %d, which the map does not know' % (key, tick, pid))
            continue
        if off and canon(m, pid, off) != canon(m, pid, 0):
            prev = [c for c in L['cmds'] if c['ge'] < off]
            if not prev or prev[-1]['name'] != 'Yield' or prev[-1]['ge'] + 1 != off:
                at = next((c for c in L['cmds'] if c['ge'] == off), None)
                tr.bad.append('GoldenEye %s at tick %d sits at %d+%d, not just after a Yield (%s)' % (
                    key, tick, pid, off, at['name'] if at else 'mid-command'))
                continue
            if key.startswith('obj@'):
                key = 'obj@%d' % pid
            tr.streams[key].append((tick, (canon(m, pid, off),), None))
    for k in tr.streams:
        tr.streams[k] = [(t, ev) for t, ev, _ in tr.streams[k]]
    return tr


def resume_of_pd(pd, m):
    """Ours as resume points: each run's first command, with the run kept
    beside it for the branch analysis."""
    tr = Trace('pd')
    tr.lines, tr.bad = pd.lines, pd.bad
    runs = {}
    for key, rs in pd.streams.items():
        out, full = [], []
        for tick, ev in rs:
            if not ev or ev[0][0] == '?':
                continue
            if canon(m, ev[0][0], ev[0][1]) == canon(m, ev[0][0], 0):
                continue        # a list's start: a run never resumes there but when something set the list
            out.append((tick, (canon(m, *ev[0]),)))
            full.append(ev)
        if out:
            tr.streams[key] = out
            runs[key] = full
    return tr, runs


def decisive(flow, run, goal):
    """The command in `run` (our events from the common resume point) that
    went the other way: the latest conditional whose untaken successor
    reaches GoldenEye's next resume point."""
    for k in range(len(run) - 1, -1, -1):
        e = run[k]
        if e[0] == '?' or e[0] not in flow.cmds or e[1] not in flow.cmds[e[0]]:
            continue
        nexts = flow.succ(e[0], e[1])
        if len(nexts) < 2:
            continue
        taken = run[k + 1] if k + 1 < len(run) else None
        for s in nexts:
            if taken is not None and taken[0] == e[0] and s == taken[1]:
                continue
            if flow.reaches(e[0], s, goal):
                return e
    return None


def compare(ge, pd, runs, m, pdnames, mission, ticks):
    flow = Flow(m)
    findings = []
    margin = max(120, ticks // 10) if ticks else 0
    for key in sorted(set(ge.streams) | set(pd.streams), key=lambda k: (k[:3], len(k), k)):
        a, b = fold(ge.streams.get(key, [])), fold(pd.streams.get(key, []))
        f = {'mission': mission, 'stream': key, 'ge_steps': len(a), 'pd_steps': len(b), 'mode': 'resume'}
        if not a or not b:
            f.update({'kind': 'missing', 'class': 'logic', 'side': 'ours' if not b else 'GoldenEye',
                      'detail': '%s runs in %s only' % (key, 'GoldenEye' if a else 'ours'),
                      'first': name_of(m, (a or b)[0][1][0])})
            findings.append(f)
            continue
        i = first_split(a, b)
        if i is None:
            continue
        # our folded steps back to their runs: step j is the j-th change of resume point
        prs = runs.get(key, [])
        rstart = [ev[0] for ev in prs]
        groups, gi = [], -1
        for k, st in enumerate(rstart):
            if not groups or canon(m, *st) != groups[-1][0]:
                groups.append([canon(m, *st), k])
            else:
                groups[-1][1] = k
        sm = difflib.SequenceMatcher(None, [x[1] for x in a], [x[1] for x in b], autojunk=False)
        regions = []
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag != 'equal':
                regions.append(_region(flow, m, pdnames, a, i1, b, j1, tag, prs, groups, ticks, margin))
        lead = _region(flow, m, pdnames, a, i, b, i, 'walk', prs, groups, ticks, margin)
        later = [r for r in regions if (r['ge_step'], r['pd_step']) != (i, i)]
        det = next((r for r in [lead] + later if r['class'] in ('logic', 'world', 'unexplained')), None)
        f.update(lead)
        f.update({'kind': 'split', 'step': i, 'similarity': round(sm.ratio(), 3),
                  'regions': dict(Counter(r['class'] for r in [lead] + later)),
                  'first_deterministic': det if det is not lead else None,
                  'ge_only': [], 'pd_only': [], 'unmapped': []})
        f['detail'] = '%s at step %d (GoldenEye tick %s, ours %s): from %s GoldenEye next stopped at %s, ours at %s' % (
            key, i, f['ge_tick'], f['pd_tick'], f['from'], f['ge_next'][0], f['pd_next'][0])
        findings.append(f)
    return findings


def _region(flow, m, pdnames, a, i, b, j, tag, prs, groups, ticks, margin):
    ge_q = a[i][1][0] if i < len(a) and tag != 'insert' else None
    pd_q = b[j][1][0] if j < len(b) and tag != 'delete' else None
    p = (b[j - 1][1][0] if j else None) or (a[i - 1][1][0] if i else None)
    run = []
    if j and j - 1 < len(groups):
        k = groups[j - 1][1]
        run = prs[k] if k < len(prs) else []
    dec = decisive(flow, run, ge_q) if ge_q and run else None
    bname = flow.cmds[dec[0]][dec[1]][0]['name'] if dec else ''
    cls = classify(bname) if dec else ('unexplained' if ge_q and pd_q else 'timing')
    ended = 'GoldenEye' if i >= len(a) else 'ours' if j >= len(b) else None
    r = {'class': cls, 'ge_step': i, 'pd_step': j, 'tag': tag,
         'ge_tick': a[i][0] if i < len(a) else None, 'pd_tick': b[j][0] if j < len(b) else None,
         'trace_ended': ended, 'from': name_of(m, p) if p else '(start)',
         'branch': name_of(m, dec) if dec else '',
         'ge_next': [_at(m, ge_q)] if ge_q else ['(stream ends)'],
         'pd_next': [_at(m, pd_q)] if pd_q else ['(stream ends)'],
         'pd_run': [name_of(m, e) for e in run[:24]],
         'pd_next_ours': [pd_name(m, pdnames, e) for e in run[:6]],
         'context': [[_at(m, a[q][1][0])] for q in range(max(0, i - 3), min(i, len(a)))]}
    if ended:
        t = r['pd_tick'] if ended == 'GoldenEye' else r['ge_tick']
        longer, shorter, at = (b, a, j) if ended == 'GoldenEye' else (a, b, i)
        recent = {x[1] for x in shorter[-30:]}
        if (margin and t is not None and t >= ticks - margin) or all(x[1] in recent for x in longer[at:]):
            r['class'] = 'window-end'
    return r


def _at(m, q):
    """A resume point by what precedes it: 'after Yield@1037+30' reads as 'at 1037+31'."""
    if not q:
        return '?'
    if q[0] == '?':
        return '%s:%s' % tuple(q[1:3])
    return '%s+%d' % (q[0], q[1])


def nulls(ge, pd, runs, m, pdnames, ticks):
    problems = []
    if ge.bad:
        problems.append('%d GoldenEye samples are not resume points the bytecode allows (first: %s)' % (len(ge.bad), ge.bad[0]))
    if pd.bad:
        problems.append('%d of our commands do not map (first: %s)' % (len(pd.bad), pd.bad[0]))
    if not ge.lines or not pd.lines:
        problems.append('a side traced nothing (GoldenEye %d samples, ours %d commands)' % (ge.lines, pd.lines))
        return problems
    for tr, rr in ((ge, {}), (pd, runs)):
        if compare(tr, tr, rr, m, pdnames, 'null', ticks):
            problems.append('%s against itself is not clean' % tr.side)
    key = max(pd.streams, key=lambda k: len(fold(pd.streams[k])))
    folded = fold(pd.streams[key])
    if len(folded) >= 1:
        # the middle step, or (every stream short) the last one
        k = len(folded) // 2 if len(folded) >= 3 else len(folded) - 1
        planted = copy.deepcopy(pd)
        rs = planted.streams[key]
        idx = next(n for n, (t, ev) in enumerate(rs) if t == folded[k][0])
        rs.insert(idx, (rs[idx][0], (('?', 'planted', 0),)))
        got = [f for f in compare(pd, planted, runs, m, pdnames, 'null', ticks) if f['stream'] == key and f['kind'] == 'split']
        if not got or got[0]['step'] != k:
            problems.append('a step planted at %d of %s was %s' % (k, key, 'not found' if not got else 'found at %s' % got[0]['step']))
    else:
        problems.append('no stream long enough to plant a step in')
    return problems
