# -*- coding: utf-8 -*-
"""For candidate FR1 thresholds and candidate FR9 window lengths, what fraction of the
riding time would the earning window be OPEN? D6's 'too long' failure is the window being
open most of the ride; this measures it against real device rides."""
import csv, math, os, sys

def load(path):
    rows = []
    with open(path, newline='', encoding='utf-8-sig') as f:
        for r in csv.DictReader(f):
            try:
                v = (float(r['vx']), float(r['vy']))
                rows.append({'t': float(r['t']), 'yaw': float(r['yaw']),
                             'spd': math.hypot(*v), 'plan': float(r.get('planing', 0) or 0)})
            except (ValueError, KeyError, TypeError):
                pass
    return [r for r in rows if r['spd'] > 200.0 and r['plan'] > 0.30]

def unwrap(v):
    out, off = [], 0.0
    for i, x in enumerate(v):
        if i:
            d = x + off - out[-1]
            while d > 180.0: off -= 360.0; d -= 360.0
            while d < -180.0: off += 360.0; d += 360.0
        out.append(x + off)
    return out

def turns_of(rows, entry, exit_, min_dur, min_sweep):
    if len(rows) < 20: return [], 0.0
    t = [r['t'] for r in rows]; yaw = unwrap([r['yaw'] for r in rows])
    n = len(rows)
    rate = [0.0]*n
    for i in range(1, n):
        dt = t[i]-t[i-1]
        rate[i] = (yaw[i]-yaw[i-1])/dt if dt > 1e-5 else rate[i-1]
    sm = [0.0]*n; tau = 0.15
    for i in range(1, n):
        dt = t[i]-t[i-1]; a = dt/(tau+dt) if dt > 0 else 1.0
        sm[i] = sm[i-1] + a*(rate[i]-sm[i-1])
    ev, i = [], 1
    while i < n:
        if abs(sm[i]) >= entry:
            sgn = 1 if sm[i] > 0 else -1
            s = i
            while s > 1 and sm[s-1]*sgn > exit_: s -= 1
            j = i
            while j < n-1 and sm[j+1]*sgn > exit_: j += 1
            if t[j]-t[s] >= min_dur and abs(yaw[j]-yaw[s]) >= min_sweep:
                ev.append(t[j])                      # exit time = when the window opens
            i = j+1
        else:
            i += 1
    # riding span, counting only contiguous stretches (gaps in the gate are not ride time)
    span = sum(min(t[i]-t[i-1], 0.5) for i in range(1, n))
    return ev, span

paths = sys.argv[1:]
print("thresholds: entry deg/s, min sweep deg -> turns, then %% of ride time the window is open\n")
print("%-22s %6s %7s   %s" % ("qualification", "turns", "per min", "window open, % of ride time"))
print("%-22s %6s %7s   %s" % ("", "", "", "  1.5s    2.5s    4.0s    6.0s"))
print("-"*78)

for entry, sweep in ((25, 40), (30, 50), (35, 60), (45, 80), (60, 100)):
    allev, total = [], 0.0
    for p in paths:
        rows = load(p)
        ev, span = turns_of(rows, entry, entry*0.4, 0.35, sweep)
        if span <= 0: continue
        allev.append((ev, rows))
        total += span
    cells = []
    for W in (1.5, 2.5, 4.0, 6.0):
        open_t = 0.0
        for ev, rows in allev:
            if not ev: continue
            t0, t1 = rows[0]['t'], rows[-1]['t']
            iv = sorted((e, min(e+W, t1)) for e in ev)   # union of [exit, exit+W]
            cur_s, cur_e = iv[0]
            for s, e in iv[1:]:
                if s <= cur_e: cur_e = max(cur_e, e)
                else: open_t += cur_e-cur_s; cur_s, cur_e = s, e
            open_t += cur_e-cur_s
        cells.append(100.0*open_t/total if total else 0.0)
    nev = sum(len(e) for e, _ in allev)
    print("%-22s %6d %7.1f   %6.0f%% %6.0f%% %6.0f%% %6.0f%%"
          % ("entry %d, sweep %d" % (entry, sweep), nev, 60.0*nev/total, *cells))

print("\ntotal riding time analysed: %.0f s across %d rides" % (total, len(paths)))
