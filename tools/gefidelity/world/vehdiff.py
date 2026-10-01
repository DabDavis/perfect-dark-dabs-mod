#!/usr/bin/env python3
"""Compare world/vehpath.py's vehicle paths: ours against GoldenEye's, on one clock.

    world/vehdiff.py OUT [--every 300] [--offset dx,dy,dz]

The level offset (ours - GoldenEye) comes from OUT's world dumps when there
are any, else --offset, else from the vehicles' first samples (they are placed
by the same record). GoldenEye's samples are a video frame apart; each of our
samples is compared with GoldenEye's interpolated to its tick. Prints a row
every --every ticks and the worst lateral and heading gaps while both move.
Null: GoldenEye's path against itself shifted by the offset gives zero.
"""
import argparse, json, math, os, sys


def interp(samples, t):
    lo, hi = None, None
    for s in samples:
        if s[0] <= t:
            lo = s
        if s[0] >= t:
            hi = s
            break
    if lo is None:
        return hi
    if hi is None or hi[0] == lo[0]:
        return lo
    f = (t - lo[0]) / (hi[0] - lo[0])
    out = [t] + [lo[i] + (hi[i] - lo[i]) * f for i in (1, 2, 3)]
    d = (hi[4] - lo[4] + math.pi) % (2 * math.pi) - math.pi
    out.append(lo[4] + d * f)
    out += [lo[5] + (hi[5] - lo[5]) * f, lo[6], lo[7]]
    return out


def compare(ge, pd, off, every):
    rows, worst = [], {'pos': (0, None), 'lat': (0, None), 'yaw': (0, None)}
    for s in pd:
        g = interp(ge, s[0])
        if g is None:
            continue
        dx, dz = s[1] - (g[1] + off[0]), s[3] - (g[3] + off[2])
        fx, fz = math.sin(g[4]), math.cos(g[4])
        lat = abs(dx * fz - dz * fx)
        along = dx * fx + dz * fz
        dyaw = math.degrees((s[4] - g[4] + math.pi) % (2 * math.pi) - math.pi)
        row = (s[0], math.hypot(dx, dz), along, lat, dyaw, g[5], s[5], int(g[7]), s[7])
        rows.append(row)
        if g[5] > 0 and s[5] > 0:
            for k, v in (('pos', row[1]), ('lat', lat), ('yaw', abs(dyaw))):
                if v > worst[k][0]:
                    worst[k] = (v, s[0])
    return rows, worst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--every', type=int, default=300)
    ap.add_argument('--offset', default=None)
    a = ap.parse_args()
    ge = json.load(open(os.path.join(a.out, 'ge', 'vehpath.json')))['vehicles']
    # the cartridge's last sample of each tick: its tick moves when a game
    # frame starts and the vehicle later in it (propsTick)
    ge = [list({s[0]: s for s in g}.values()) for g in ge]
    pd = json.load(open(os.path.join(a.out, 'pd', 'vehpath.json')))['vehicles']
    if len(ge) != len(pd):
        print('vehicles: GoldenEye %d, ours %d' % (len(ge), len(pd)))
    for k, (g, p) in enumerate(zip(ge, pd)):
        if a.offset:
            off = [float(v) for v in a.offset.split(',')]
        else:
            off = [p[0][i] - g[0][i] for i in (1, 2, 3)]
        shifted = [[s[0], s[1] + off[0], s[2] + off[1], s[3] + off[2]] + s[4:] for s in g]
        nullrows, nullworst = compare(g, shifted, off, a.every)
        if max(v for v, _ in nullworst.values()) > 1e-3:
            print('NULL FAILED: GoldenEye against itself gives %s' % (nullworst,))
            return 2
        rows, worst = compare(g, p, off, a.every)
        print('vehicle %d: offset %s; worst while both move: position %.1f at tick %s, lateral %.1f at %s, heading %.2f deg at %s' % (
            k, [round(v, 1) for v in off], worst['pos'][0], worst['pos'][1], worst['lat'][0], worst['lat'][1], worst['yaw'][0], worst['yaw'][1]))
        print('  tick   apart  along  lateral  dyaw(deg)  speed ge/ours  step ge/ours')
        for r in rows:
            if r[0] % a.every < 2 or r is rows[-1]:
                print('  %5d %7.1f %6.1f %7.1f %8.2f    %4.2f/%4.2f     %d/%d' % r)
    return 0


if __name__ == '__main__':
    sys.exit(main())
