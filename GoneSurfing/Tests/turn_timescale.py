# -*- coding: utf-8 -*-
"""Measure turn timescales from ride CSVs: how long a turn lasts, how long until the
board settles afterwards, and how often turns get linked. Proxy for D6's window floor."""
import csv, glob, math, os, sys

def load(path):
    rows = []
    with open(path, newline='', encoding='utf-8-sig') as f:
        for r in csv.DictReader(f):
            try:
                rows.append({
                    't': float(r['t']),
                    'yaw': float(r['yaw']),
                    'vx': float(r['vx']), 'vy': float(r['vy']),
                    'planing': float(r.get('planing', 0) or 0),
                })
            except (ValueError, KeyError, TypeError):
                pass
    return rows

def unwrap(vals):
    out, off = [], 0.0
    for i, v in enumerate(vals):
        if i:
            d = v + off - out[-1]
            while d > 180.0:
                off -= 360.0; d -= 360.0
            while d < -180.0:
                off += 360.0; d += 360.0
        out.append(v + off)
    return out

def analyse(path, entry=20.0, exit_=8.0, min_dur=0.35, min_sweep=25.0):
    rows = load(path)
    if len(rows) < 30:
        return None
    # RIDING GATE: a stalled / wiped-out board spins freely and reads as a huge "turn".
    # Keep only the stretch where the board is actually surfing.
    keep = [r for r in rows if math.hypot(r['vx'], r['vy']) > 200.0 and r['planing'] > 0.30]
    if len(keep) < 20:
        return None
    rows = keep
    t = [r['t'] for r in rows]
    yaw = unwrap([r['yaw'] for r in rows])
    spd = [math.hypot(r['vx'], r['vy']) for r in rows]

    # raw yaw rate, then a 0.15 s low-pass to match HeadingRateSmoothingSeconds
    rate = [0.0] * len(rows)
    for i in range(1, len(rows)):
        dt = t[i] - t[i - 1]
        rate[i] = (yaw[i] - yaw[i - 1]) / dt if dt > 1e-5 else rate[i - 1]
    sm, tau = [0.0] * len(rows), 0.15
    for i in range(1, len(rows)):
        dt = t[i] - t[i - 1]
        a = dt / (tau + dt) if dt > 0 else 1.0
        sm[i] = sm[i - 1] + a * (rate[i] - sm[i - 1])

    # event detection with hysteresis, sign must hold
    turns, i, n = [], 1, len(rows)
    while i < n:
        if abs(sm[i]) >= entry:
            sgn = 1 if sm[i] > 0 else -1
            s = i
            while s > 1 and sm[s - 1] * sgn > exit_:
                s -= 1
            j = i
            while j < n - 1 and sm[j + 1] * sgn > exit_:
                j += 1
            dur = t[j] - t[s]
            sweep = abs(yaw[j] - yaw[s])
            if dur >= min_dur and sweep >= min_sweep:
                turns.append({'s': s, 'j': j, 'dur': dur, 'sweep': sweep,
                              'peak': max(abs(sm[k]) for k in range(s, j + 1)),
                              'vin': spd[s], 'vout': spd[j]})
            i = j + 1
        else:
            i += 1

    # settle: from each exit, how long until |rate| stays under exit_ for 0.5 s
    for k, tn in enumerate(turns):
        j, settle = tn['j'], None
        q = j
        while q < n - 1:
            if abs(sm[q]) < exit_:
                r = q
                while r < n - 1 and t[r] - t[q] < 0.5:
                    r += 1
                    if abs(sm[r]) >= exit_:
                        break
                else:
                    settle = t[q] - t[j]; break
                if abs(sm[r]) < exit_ and t[r] - t[q] >= 0.5:
                    settle = t[q] - t[j]; break
                q = r
            q += 1
        tn['settle'] = settle
        tn['gap'] = (turns[k + 1]['s'] and t[turns[k + 1]['s']] - t[j]) if k + 1 < len(turns) else None

    return {'file': os.path.basename(path), 'dur': t[-1] - t[0], 'n': len(rows),
            'hz': (len(rows) - 1) / (t[-1] - t[0]) if t[-1] > t[0] else 0, 'turns': turns}

def pct(vals, q):
    if not vals: return float('nan')
    v = sorted(vals); k = (len(v) - 1) * q
    lo, hi = int(math.floor(k)), int(math.ceil(k))
    return v[lo] if lo == hi else v[lo] + (v[hi] - v[lo]) * (k - lo)

paths = sys.argv[1:]
allturns, gaps, settles, durs = [], [], [], []
print("%-42s %6s %5s %6s  turns" % ("file", "len_s", "Hz", "n"))
print("-" * 78)
for p in paths:
    a = analyse(p)
    if not a: continue
    print("%-42s %6.1f %5.1f %6d  %d" % (a['file'], a['dur'], a['hz'], a['n'], len(a['turns'])))
    for tn in a['turns']:
        allturns.append(tn); durs.append(tn['dur'])
        if tn['settle'] is not None: settles.append(tn['settle'])
        if tn['gap'] is not None and tn['gap'] > 0: gaps.append(tn['gap'])

print("\n=== %d turn events across %d files ===" % (len(allturns), len(paths)))
def line(name, v, unit):
    if not v:
        print("  %-26s (none)" % name); return
    print("  %-26s median %6.2f %s   p25 %5.2f   p75 %5.2f   p90 %5.2f   max %5.2f"
          % (name, pct(v, .5), unit, pct(v, .25), pct(v, .75), pct(v, .90), max(v)))
line("turn duration", durs, "s")
line("settle after exit", settles, "s")
line("gap exit -> next entry", gaps, "s")
line("heading sweep", [t['sweep'] for t in allturns], "deg")
line("peak yaw rate", [t['peak'] for t in allturns], "d/s")
line("speed kept (vout/vin)", [t['vout'] / t['vin'] for t in allturns if t['vin'] > 50], "x")
