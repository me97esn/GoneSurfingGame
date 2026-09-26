#!/usr/bin/env python3
"""Fit the peel model for Content/WaveGeometry/<map>.json from a `surf.debug.wavegeo 1` run.

    py -3 Tools/WaveGeoFit.py Saved/Logs/GoneSurfing.log [--write Content/WaveGeometry/<map>.json]

Reads the WAVEGEO (board position + crest) and WAVEGEO-FRONTS (every foam cluster's down-line front)
lines, keeps per tick the *newest* front - in the fresh band 1-4.5 m shoreward of the board's crest,
not more than half a period ahead of the board, furthest down the line - and fits its position along
the line as a straight line in unwrapped frame (least squares). Prints the fit, the residuals per
loop, and the two model points at the run's first loop frame 1030 and one loop later. With --write
it replaces the "points" of the JSON and records the fit in "note", keeping the zone constants.

The wave frame (line / back axes) is taken from the WAVEGEO lines themselves, so the printed points
are in world cm exactly as the subsystem consumes them. specs/wave-geometry.md FR6; the measurement
this reproduces is M14 in specs/broken-wave-no-consequences.md.
"""
import json, re, statistics, sys

LOOP_FRAMES = 193
FRESH_BAND = (-450.0, 400.0)   # front c relative to the board's crest c
MIN_COUNT = 2000
HALF_PERIOD_AHEAD = 2600.0


def main(argv):
    if len(argv) < 2:
        print(__doc__); return 2
    path = argv[1]
    write = argv[argv.index("--write") + 1] if "--write" in argv else None
    lines = open(path, encoding="utf-8", errors="ignore").read().splitlines()

    axes = None; board = None; crest = None; last = None; loop = 0
    rows = []
    for l in lines:
        m = re.search(r"WAVEGEO frame=(\d+) loop=(\d+) back=\(([-\d.]+), ([-\d.]+)\) line=\(([-\d.]+), ([-\d.]+)\) pawn s=(-?\d+) c=(-?\d+) \| crest0 (\w+) c=(-?\d+)", l)
        if m:
            axes = ((float(m[5]), float(m[6])), (float(m[3]), float(m[4])))
            board = int(m[7]); crest = int(m[10]) if m[9] == "ok" else None
            continue
        m = re.search(r"WAVEGEO-FRONTS frame=(\d+)(.*)$", l)
        if m and crest is not None:
            f = int(m[1])
            if last is not None and f < last - 50: loop += 1
            last = f
            fronts = [tuple(int(x) for x in g) for g in re.findall(r"t(\d+):s=(-?\d+),c=(-?\d+),n=(\d+),cs=(-?\d+),cc=(-?\d+)", m[2])]
            cand = [x for x in fronts if crest + FRESH_BAND[0] <= x[2] <= crest + FRESH_BAND[1] and x[3] >= MIN_COUNT and x[1] <= board + HALF_PERIOD_AHEAD]
            if cand:
                t, s, c, n, cs, cc = max(cand, key=lambda x: x[1])
                rows.append((loop * LOOP_FRAMES + f, f, loop, s, c))
    if len(rows) < 100 or axes is None:
        print(f"not enough data: {len(rows)} usable ticks (need the wavegeo debug on for >= 2 loops)"); return 1

    def fit(rs):
        uf = [r[0] for r in rs]; s = [r[3] for r in rs]
        n = len(rs); mx = sum(uf) / n; my = sum(s) / n
        b = sum((x - mx) * (y - my) for x, y in zip(uf, s)) / sum((x - mx) ** 2 for x in uf)
        return my - b * mx, b

    # Iterative trimming: a tick whose front belongs to another wave (a period off) or to the aging
    # bore (the board stalled into the whitewater) is far off the line; drop it and refit.
    total = len(rows)
    for _ in range(4):
        a, b = fit(rows)
        res = [r[3] - (a + b * r[0]) for r in rows]
        cut = max(300.0, 2.5 * statistics.pstdev(res))
        kept = [r for r, e in zip(rows, res) if abs(e) <= cut]
        if len(kept) == len(rows): break
        rows = kept
    a, b = fit(rows)
    uf = [r[0] for r in rows]; s = [r[3] for r in rows]; c = [r[4] for r in rows]
    n = len(rows)
    res = [y - (a + b * x) for x, y in zip(uf, s)]
    cm = statistics.mean(c)
    print(f"usable ticks: {n} of {total} over {rows[-1][2] - rows[0][2] + 1} loops ({total - n} trimmed as another wave / the bore)")
    print(f"peel speed along the line: {b:.2f} cm/frame ({b * 24:.0f} cm/s at 24 fps)")
    print(f"residual along the line: sd {statistics.pstdev(res):.0f} cm, p5 {sorted(res)[n // 20]:.0f}, p95 {sorted(res)[-max(1, n // 20)]:.0f}")
    print(f"cross-shore: mean {cm:.0f}, sd {statistics.pstdev(c):.0f} cm")
    for L in sorted(set(r[2] for r in rows)):
        rr = [y - (a + b * x) for x, y, l in zip(uf, s, [r[2] for r in rows]) if l == L]
        print(f"  loop {L}: n={len(rr)} residual mean {statistics.mean(rr):+.0f} sd {statistics.pstdev(rr):.0f}")

    line, back = axes
    f0 = 1 * LOOP_FRAMES + 1030; f1 = f0 + LOOP_FRAMES
    pts = []
    for ff in (f0, f1):
        ss = a + b * ff
        pts.append({"x": round(line[0] * ss + back[0] * cm), "y": round(line[1] * ss + back[1] * cm), "frame": 1030 if ff == f0 else 1030 + LOOP_FRAMES})
    print("points:", json.dumps(pts))

    if write:
        doc = json.load(open(write, encoding="utf-8"))
        doc["points"] = pts
        doc["note"] = (f"Fitted by Tools/WaveGeoFit.py from {path}: {b:.2f} cm/frame along the line, residual sd "
                       f"{statistics.pstdev(res):.0f} cm over {n} ticks, cross-shore sd {statistics.pstdev(c):.0f} cm. "
                       "Re-fit after any change to the wave data, the tiling or the level (specs/wave-geometry.md).")
        json.dump(doc, open(write, "w", encoding="utf-8"), indent=2)
        print("wrote", write)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
