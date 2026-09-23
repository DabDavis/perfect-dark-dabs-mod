#!/usr/bin/env python3
# Summarise probe.sh output: for each tag (a path prefix such as
# /tmp/simprobe/new), per mode (-n- NormalSims, -f- fast DarkSims):
#   stalled  simulant-samples standing still in ACT_GOPOS for 3 s or more
#   circling samples in runs of 3 s or more near the same waypoint, moving,
#            never reaching it (the "tornado")
#   falls    fall deaths
#   hunt     mean distance of the nearest simulant to the player, and how many
#            simulants have the player as target and in sight, per sample
# and the five spots with the most stalled samples.
import sys, re, glob, collections
def load(fn):
    rows = collections.defaultdict(list); st = []; falls = 0; hunt = []; ticks = 0
    for l in open(fn, errors="replace"):
        if l.startswith("W f"):
            f, b, wp, d, x, y, z = map(int, re.match(r"W f(\d+) b(\d+) wp=(-?\d+) d=(\d+) p=(-?\d+),(-?\d+),(-?\d+)", l).groups())
            rows[b].append((f, wp, d, x, z))
        elif l.startswith("ST f"): st.append(l.split())
        elif l.startswith("FALL"): falls += 1
        elif l.startswith("T f"): ticks += 1
        elif l.startswith("H f"):
            m = re.match(r"H f(\d+) dmin=(\d+) tgt=(\d+) sight=(\d+)", l)
            if m and int(m.group(1)) > 600: hunt.append(tuple(map(int, m.groups()[1:])))
    circ = 0
    for r in rows.values():
        run = []
        for e in r + [None]:
            if e and run and e[0] - run[-1][0] == 30 and e[1] == run[-1][1] >= 0 and e[2] < 300 \
                    and ((e[3] - run[-1][3]) ** 2 + (e[4] - run[-1][4]) ** 2) ** .5 > 60:
                run.append(e); continue
            if len(run) >= 6: circ += len(run)
            run = [e] if e else []
    return ticks, st, falls, hunt, circ
for tag in sys.argv[1:]:
    for mode in "nf":
        files = sorted(glob.glob("%s-%s-*.probe" % (tag, mode)))
        if not files: continue
        T = S = F = C = 0; H = []; spots = collections.Counter(); perstage = collections.Counter()
        for fn in files:
            t, st, f, h, c = load(fn)
            T += t; S += len(st); F += f; C += c; H += h
            perstage[fn.split("-")[-2]] += len(st)
            for s in st: spots[(round(float(s[3]) / 200) * 200, round(float(s[4]) / 100) * 100, round(float(s[5]) / 200) * 200, fn.split("-")[-2])] += 1
        n = max(len(H), 1)
        print("%s %s: %d files %d ticks  stalled %d  circling %d  falls %d  hunt dist %.0f target %.2f sight %.2f  [%s]" % (
            tag.split("/")[-1], mode, len(files), T, S, C, F, sum(x[0] for x in H) / n, sum(x[1] for x in H) / n, sum(x[2] for x in H) / n,
            " ".join("%s=%d" % kv for kv in sorted(perstage.items()))))
        for k, v in spots.most_common(5): print("    %4d at %s" % (v, k))
