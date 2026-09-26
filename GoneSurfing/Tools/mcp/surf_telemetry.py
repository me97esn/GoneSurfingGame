#!/usr/bin/env python3
"""
surf-telemetry -- an MCP server over GoneSurfing ride trajectories.

WHY THIS EXISTS
    Ride analysis means answering questions like "how much speed did that turn
    cost?" or "what fraction of the ride was above the slope-thrust gate?".
    Today that is an ad-hoc PowerShell one-liner per question, and the raw
    material is a 125 KB CSV that has to be read in full to answer anything.
    This server turns the trajectory CSVs into a handful of named tools that
    return small JSON answers instead.

HOW MCP WORKS
    See mcp_lite.py -- the protocol lives there, shared with surf_tuning.py.
    This file is the surfing half: what the tools know and how they answer.

PROJECT ROOT
    Resolved as: --project arg, else $GONESURFING_PROJECT, else walked up from
    this file (Tools/mcp/ -> Tools/ -> GoneSurfing/). The override matters
    because Saved/ is gitignored: a fresh worktree has the code but none of the
    run data, so point it at the tree that actually has the CSVs.

        python3 surf_telemetry.py --project E:/windowsgrejor/git/GoneSurfingUE5/GoneSurfing
"""

from __future__ import annotations

import argparse
import csv
import math
import os
from datetime import datetime
from pathlib import Path

import mcp_lite
from mcp_lite import dumps, tool  # dumps re-exported: handy from a REPL

SERVER_INFO = {"name": "surf-telemetry", "version": "0.2.0"}

# Wave-face slope gates that decide whether propulsion terms fire at all. The
# fraction of a ride spent above each is usually the answer to "why is it slow".
SLOPE_GATES = (0.12, 0.30, 0.35)

# Thresholds mirrored from Tests/Compare.ps1. That script stays the authority
# for CI exit codes; these exist so compare() can report the same verdict with
# structure attached. Keep them in sync if Compare.ps1 is recalibrated.
CMP_DEFAULTS = {
    "warn_pos_cm": 1000, "regress_pos_cm": 3000,
    "warn_vel_cms": 1500, "regress_vel_cms": 4000,
    "warn_pitch_deg": 15, "regress_pitch_deg": 30,
}


# ---------------------------------------------------------------------------
# Data layer -- trajectory CSVs
# ---------------------------------------------------------------------------

# Every trajectory CSV (autopilot snapshot tests, device traces, baselines)
# shares one schema, written by AStateTriggerAutoPilot:
#   t gameSeconds frame x y z vx vy vz roll pitch yaw step slopeSin planing underwater
# Units: position cm, velocity cm/s, angles degrees, slopeSin/planing/underwater 0..1.
COLUMNS = ("t", "gameSeconds", "frame", "x", "y", "z", "vx", "vy", "vz",
           "roll", "pitch", "yaw", "step", "slopeSin", "planing", "underwater")


def resolve_root(cli_project: str | None) -> Path:
    if cli_project:
        return Path(cli_project).resolve()
    env = os.environ.get("GONESURFING_PROJECT")
    if env:
        return Path(env).resolve()
    return Path(__file__).resolve().parents[2]


def source_dirs() -> dict[str, Path]:
    return {
        "latest": ROOT / "Saved" / "Tests" / "latest",
        "baselines": ROOT / "Tests" / "baselines",
    }


def find_csv(name: str, source: str = "latest") -> Path:
    """Accept a run name with or without the .csv suffix."""
    dirs = source_dirs()
    if source not in dirs:
        raise ValueError(f"source must be one of {sorted(dirs)}, got {source!r}")
    stem = name[:-4] if name.lower().endswith(".csv") else name
    path = dirs[source] / f"{stem}.csv"
    if not path.exists():
        available = sorted(p.stem for p in dirs[source].glob("*.csv")) if dirs[source].exists() else []
        raise FileNotFoundError(
            f"no run {stem!r} in {source} ({dirs[source]}). "
            f"Available: {', '.join(available[:40]) or '(none -- is the project root right? Saved/ is gitignored)'}"
        )
    return path


_CACHE: dict[tuple, dict] = {}


def load(path: Path) -> dict:
    """Column-oriented load: {column: [float, ...]} plus derived speed.

    Cached on (path, mtime) so a summary followed by a window on the same run
    parses once. UE writes these with a BOM, hence utf-8-sig.
    """
    key = (str(path), path.stat().st_mtime_ns)
    hit = _CACHE.get(key)
    if hit is not None:
        return hit

    cols: dict[str, list[float]] = {c: [] for c in COLUMNS}
    with path.open(encoding="utf-8-sig", newline="") as fh:
        for row in csv.DictReader(fh):
            for c in COLUMNS:
                raw = row.get(c)
                try:
                    cols[c].append(float(raw))
                except (TypeError, ValueError):
                    cols[c].append(float("nan"))

    cols["speed"] = [math.sqrt(vx * vx + vy * vy + vz * vz)
                     for vx, vy, vz in zip(cols["vx"], cols["vy"], cols["vz"])]
    data = {"path": path, "n": len(cols["t"]), "cols": cols}
    _CACHE[key] = data
    while len(_CACHE) > 4:  # tiny LRU: summary -> find_events -> window on the
        _CACHE.pop(next(iter(_CACHE)))  # same run should parse it once
    return data


# ---------------------------------------------------------------------------
# Small stats helpers (no numpy -- keeping this dependency-free)
# ---------------------------------------------------------------------------

def pct(xs: list[float], p: float):
    if not xs:
        return None
    s = sorted(xs)
    k = (len(s) - 1) * p
    lo, hi = math.floor(k), math.ceil(k)
    return s[int(k)] if lo == hi else s[lo] + (s[hi] - s[lo]) * (k - lo)


def r(x, nd: int = 2):
    """Round for the wire. Whole numbers stay ints so the JSON reads clean."""
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return None
    v = round(float(x), nd)
    return int(v) if nd == 0 else v


def spread(xs: list[float], nd: int = 1) -> dict:
    if not xs:
        return {}
    return {
        "mean": r(sum(xs) / len(xs), nd),
        "median": r(pct(xs, 0.5), nd),
        "p95": r(pct(xs, 0.95), nd),
        "min": r(min(xs), nd),
        "max": r(max(xs), nd),
    }


def wrap180(d: float) -> float:
    """Signed angle delta wrapped into [-180, 180]. Yaw and roll both wrap."""
    while d > 180:
        d -= 360
    while d < -180:
        d += 360
    return d


def yaw_rates(t: list[float], yaw: list[float]) -> list[float]:
    """Signed deg/s, one shorter than the sample list."""
    out = []
    for i in range(len(t) - 1):
        dt = t[i + 1] - t[i]
        out.append(wrap180(yaw[i + 1] - yaw[i]) / dt if dt > 1e-6 else 0.0)
    return out


def file_age(path: Path) -> dict:
    mtime = datetime.fromtimestamp(path.stat().st_mtime)
    return {
        "modified": mtime.strftime("%Y-%m-%d %H:%M"),
        "age_days": r((datetime.now() - mtime).total_seconds() / 86400.0, 1),
    }


# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

@tool(
    "list_runs",
    "List recorded ride trajectories with their date, length and staleness. "
    "Sources: 'latest' = Saved/Tests/latest (last headless run + pulled device "
    "traces, gitignored), 'baselines' = Tests/baselines (committed reference "
    "trajectories). Saved/Tests/latest is never cleaned, so it holds runs from "
    "many months ago alongside today's -- always check age_days before treating "
    "a run as current. Start here to find out what data exists.",
    {
        "type": "object",
        "properties": {
            "source": {"type": "string", "enum": ["latest", "baselines", "all"],
                       "default": "latest"},
            "stale_days": {"type": "number", "default": 14,
                           "description": "Flag runs older than this many days."},
        },
    },
)
def list_runs(source: str = "latest", stale_days: float = 14) -> dict:
    dirs = source_dirs()
    wanted = list(dirs) if source == "all" else [source]
    runs, missing = [], []
    for src in wanted:
        d = dirs.get(src)
        if d is None:
            raise ValueError(f"unknown source {src!r}")
        if not d.exists():
            missing.append(f"{src}: {d} does not exist")
            continue
        for path in sorted(d.glob("*.csv")):
            age = file_age(path)
            data = load(path)
            t = data["cols"]["t"]
            runs.append({
                "name": path.stem,
                "source": src,
                "kind": "device" if path.stem.startswith("phone-") else "test",
                "rows": data["n"],
                "duration_s": r(t[-1] - t[0], 1) if data["n"] > 1 else 0,
                **age,
                "stale": age["age_days"] > stale_days,
            })
    runs.sort(key=lambda x: x["age_days"])
    out = {"project_root": str(ROOT), "count": len(runs), "runs": runs}
    if missing:
        out["missing_sources"] = missing
    # Qualified by source: the same name exists in both latest and baselines.
    stale = [f'{x["name"]} ({x["source"]})' for x in runs if x["stale"]]
    if stale:
        out["note"] = (f"{len(stale)} run(s) older than {stale_days} days are still on disk "
                       "and will be graded by Compare.ps1 as if they were current: "
                       + ", ".join(stale[:10]) + ("..." if len(stale) > 10 else ""))
    return out


@tool(
    "run_summary",
    "Whole-ride aggregates for one trajectory: speed distribution, distance, "
    "time planing and submerged, wave-slope distribution against the propulsion "
    "gates, attitude extremes, and a per-autopilot-step breakdown. This is the "
    "right tool for judging a tuning change -- ride quality lives in the "
    "aggregates, not in any single-sample maximum. Speeds cm/s (and km/h), "
    "positions cm, angles degrees.",
    {
        "type": "object",
        "properties": {
            "name": {"type": "string", "description": "Run name, with or without .csv"},
            "source": {"type": "string", "enum": ["latest", "baselines"], "default": "latest"},
        },
        "required": ["name"],
    },
)
def run_summary(name: str, source: str = "latest") -> dict:
    path = find_csv(name, source)
    data = load(path)
    c = data["cols"]
    n = data["n"]
    if n < 2:
        raise ValueError(f"{path.name} has {n} row(s) -- nothing to summarise")

    t, speed = c["t"], c["speed"]
    duration = t[-1] - t[0]

    # Path length vs net displacement: a ride that carves covers far more
    # ground than it advances, and the ratio is a cheap "how much turning".
    dist = sum(math.dist((c["x"][i], c["y"][i], c["z"][i]),
                         (c["x"][i + 1], c["y"][i + 1], c["z"][i + 1]))
               for i in range(n - 1))
    net = math.dist((c["x"][0], c["y"][0], c["z"][0]),
                    (c["x"][-1], c["y"][-1], c["z"][-1]))

    rates = yaw_rates(t, c["yaw"])
    abs_rates = [abs(x) for x in rates]
    yaw_total = sum(abs(wrap180(c["yaw"][i + 1] - c["yaw"][i])) for i in range(n - 1))

    def frac(xs, thr):
        return r(sum(1 for x in xs if x > thr) / len(xs), 3)

    summary = {
        "name": path.stem,
        "source": source,
        "path": str(path),
        **file_age(path),
        "rows": n,
        "duration_s": r(duration, 1),
        "sample_hz": r(n / duration, 1) if duration > 0 else None,
        "speed_cms": spread(speed, 0),
        "speed_kmh": spread([s * 0.036 for s in speed], 1),
        "distance_cm": r(dist, 0),
        "net_displacement_cm": r(net, 0),
        "path_to_net_ratio": r(dist / net, 2) if net > 1 else None,
        # +X is the back of the wave and it breaks toward -X (shore); +Y is
        # down the line. Raw extents, since crest position is not in the CSV.
        "extent_cm": {
            "x": [r(min(c["x"]), 0), r(max(c["x"]), 0)],
            "y": [r(min(c["y"]), 0), r(max(c["y"]), 0)],
            "z": [r(min(c["z"]), 0), r(max(c["z"]), 0)],
        },
        "ride_height_z": spread(c["z"], 0),
        "planing": {"mean": r(sum(c["planing"]) / n, 3),
                    "fraction_above_0.5": frac(c["planing"], 0.5)},
        "underwater": {"mean": r(sum(c["underwater"]) / n, 3),
                       "fraction_above_0.5": frac(c["underwater"], 0.5)},
        "slope_sin": {
            **spread(c["slopeSin"], 3),
            "fraction_above_gate": {str(g): frac(c["slopeSin"], g) for g in SLOPE_GATES},
        },
        "attitude_deg": {
            "roll_abs_mean": r(sum(abs(x) for x in c["roll"]) / n, 1),
            "roll_abs_max": r(max(abs(x) for x in c["roll"]), 1),
            "pitch": spread(c["pitch"], 1),
            "yaw_total_change": r(yaw_total, 0),
            "yaw_rate_abs": {"median": r(pct(abs_rates, 0.5), 1),
                             "p95": r(pct(abs_rates, 0.95), 1),
                             "max": r(max(abs_rates), 1)},
        },
        "steps": [],
    }

    # Per autopilot step: which scripted phase each statistic belongs to.
    for step in sorted({int(s) for s in c["step"] if not math.isnan(s)}):
        idx = [i for i in range(n) if int(c["step"][i]) == step]
        summary["steps"].append({
            "step": step,
            "t0": r(t[idx[0]], 1),
            "t1": r(t[idx[-1]], 1),
            "duration_s": r(t[idx[-1]] - t[idx[0]], 1),
            "speed_cms_mean": r(sum(speed[i] for i in idx) / len(idx), 0),
            "speed_cms_max": r(max(speed[i] for i in idx), 0),
            "slope_sin_mean": r(sum(c["slopeSin"][i] for i in idx) / len(idx), 3),
            "planing_mean": r(sum(c["planing"][i] for i in idx) / len(idx), 2),
        })

    notes = []
    if summary["age_days"] > 14:
        notes.append(f"This run is {summary['age_days']} days old -- confirm it is the one you mean.")
    if summary["slope_sin"]["fraction_above_gate"]["0.3"] < 0.1:
        notes.append("Under 10% of the ride sits above slopeSin 0.30, so slope-gated "
                     "propulsion terms were inactive for nearly all of it.")
    if notes:
        summary["notes"] = notes
    return summary


@tool(
    "window",
    "Raw samples from a slice of one run -- only the seconds and columns asked "
    "for. Use after run_summary or find_events has pointed at an interesting "
    "moment; this is the zoom, not the overview. Rows beyond max_rows are "
    "evenly downsampled rather than truncated, so the shape of the window "
    "survives. Columns: t gameSeconds frame x y z vx vy vz roll pitch yaw step "
    "slopeSin planing underwater speed.",
    {
        "type": "object",
        "properties": {
            "name": {"type": "string"},
            "source": {"type": "string", "enum": ["latest", "baselines"], "default": "latest"},
            "t0": {"type": "number", "description": "Start time in seconds. Omit for the start of the run."},
            "t1": {"type": "number", "description": "End time in seconds. Omit for the end of the run."},
            "columns": {"type": "array", "items": {"type": "string"},
                        "description": "Defaults to t, speed, z, pitch, roll, yaw, slopeSin, planing."},
            "max_rows": {"type": "integer", "default": 120},
        },
        "required": ["name"],
    },
)
def window(name: str, source: str = "latest", t0: float | None = None,
           t1: float | None = None, columns: list | None = None,
           max_rows: int = 120) -> dict:
    path = find_csv(name, source)
    data = load(path)
    c = data["cols"]
    cols = columns or ["t", "speed", "z", "pitch", "roll", "yaw", "slopeSin", "planing"]
    unknown = [x for x in cols if x not in c]
    if unknown:
        raise ValueError(f"unknown column(s): {unknown}. Known: {sorted(c)}")

    lo = -math.inf if t0 is None else t0
    hi = math.inf if t1 is None else t1
    idx = [i for i in range(data["n"]) if lo <= c["t"][i] <= hi]
    if not idx:
        raise ValueError(f"no samples in t=[{t0}, {t1}]; run spans "
                         f"{r(c['t'][0], 2)}..{r(c['t'][-1], 2)}s")

    stride = max(1, math.ceil(len(idx) / max_rows))
    kept = idx[::stride]
    out = {
        "name": path.stem,
        "t_range": [r(c["t"][idx[0]], 2), r(c["t"][idx[-1]], 2)],
        "matched_rows": len(idx),
        "returned_rows": len(kept),
        "columns": cols,
        "rows": [[r(c[col][i], 3) for col in cols] for i in kept],
    }
    if stride > 1:
        out["downsampled"] = f"every {stride}th sample; narrow t0/t1 or raise max_rows for all of them"
    return out


@tool(
    "compare",
    "Diff a run against its committed baseline and return the same verdict "
    "Tests/Compare.ps1 produces (OK / WARN / REGRESSION), with the per-step "
    "drift attached. Compare.ps1 remains the authority for CI exit codes; this "
    "is the structured read of the same comparison. Note that a REGRESSION "
    "verdict only means 'differs from baseline' -- when the baseline predates "
    "intentional ride changes, that is expected, so check both dates. Dates come "
    "from file mtime, so in a freshly checked-out worktree every committed "
    "baseline reads as new; use git log there.",
    {
        "type": "object",
        "properties": {
            "name": {"type": "string", "description": "Run name; the baseline of the same name is the reference."},
            "baseline": {"type": "string", "description": "Compare against a differently-named baseline."},
        },
        "required": ["name"],
    },
)
def compare(name: str, baseline: str | None = None) -> dict:
    lat_path = find_csv(name, "latest")
    base_path = find_csv(baseline or name, "baselines")
    lat, base = load(lat_path)["cols"], load(base_path)["cols"]
    n = min(len(lat["t"]), len(base["t"]))
    if n == 0:
        raise ValueError("one of the CSVs is empty")

    max_pos = max_vel = max_pitch = max_roll = max_yaw = 0.0
    sum_pos = sum_vel = sum_pitch = 0.0
    at_pos = at_pitch = (0.0, 0)
    steps: dict[int, dict] = {}

    for i in range(n):
        d_pos = math.dist((lat["x"][i], lat["y"][i], lat["z"][i]),
                          (base["x"][i], base["y"][i], base["z"][i]))
        d_vel = abs(lat["speed"][i] - base["speed"][i])
        d_pitch = abs(wrap180(lat["pitch"][i] - base["pitch"][i]))
        d_roll = abs(wrap180(lat["roll"][i] - base["roll"][i]))
        d_yaw = abs(wrap180(lat["yaw"][i] - base["yaw"][i]))

        if d_pos > max_pos:
            max_pos, at_pos = d_pos, (base["t"][i], int(base["step"][i]))
        if d_pitch > max_pitch:
            max_pitch, at_pitch = d_pitch, (base["t"][i], int(base["step"][i]))
        max_vel = max(max_vel, d_vel)
        max_roll = max(max_roll, d_roll)
        max_yaw = max(max_yaw, d_yaw)
        sum_pos += d_pos
        sum_vel += d_vel
        sum_pitch += d_pitch

        st = steps.setdefault(int(base["step"][i]), {"max_pos_cm": 0.0, "max_pitch_deg": 0.0, "samples": 0})
        st["max_pos_cm"] = max(st["max_pos_cm"], d_pos)
        st["max_pitch_deg"] = max(st["max_pitch_deg"], d_pitch)
        st["samples"] += 1

    th = CMP_DEFAULTS
    if max_pos > th["regress_pos_cm"] or max_vel > th["regress_vel_cms"] or max_pitch > th["regress_pitch_deg"]:
        verdict, exit_code = "REGRESSION", 2
    elif max_pos > th["warn_pos_cm"] or max_vel > th["warn_vel_cms"] or max_pitch > th["warn_pitch_deg"]:
        verdict, exit_code = "WARN", 1
    else:
        verdict, exit_code = "OK", 0

    final = math.dist((lat["x"][n - 1], lat["y"][n - 1], lat["z"][n - 1]),
                      (base["x"][n - 1], base["y"][n - 1], base["z"][n - 1]))

    result = {
        "name": lat_path.stem,
        "baseline": base_path.stem,
        "verdict": verdict,
        "exit_code_equivalent": exit_code,
        "compared_rows": n,
        "run": file_age(lat_path),
        "baseline_recorded": file_age(base_path),
        "position_cm": {"max": r(max_pos, 1), "mean": r(sum_pos / n, 1), "final": r(final, 1),
                        "max_at_t": r(at_pos[0], 2), "max_at_step": at_pos[1]},
        "speed_cms": {"max": r(max_vel, 1), "mean": r(sum_vel / n, 1)},
        "pitch_deg": {"max": r(max_pitch, 1), "mean": r(sum_pitch / n, 1),
                      "max_at_t": r(at_pitch[0], 2), "max_at_step": at_pitch[1]},
        "roll_deg_max": r(max_roll, 1),
        "yaw_deg_max": r(max_yaw, 1),
        "per_step": {str(k): {"max_pos_cm": r(v["max_pos_cm"], 0),
                              "max_pitch_deg": r(v["max_pitch_deg"], 1),
                              "samples": v["samples"]}
                     for k, v in sorted(steps.items())},
        "thresholds": th,
    }
    notes = []
    if len(lat["t"]) != len(base["t"]):
        notes.append(f"row count differs (run={len(lat['t'])}, baseline={len(base['t'])}); "
                     f"compared the first {n}")
    gap = result["baseline_recorded"]["age_days"] - result["run"]["age_days"]
    if verdict != "OK" and gap > 30:
        notes.append(f"The baseline is {r(gap, 0)} days older than the run. If ride tuning "
                     "changed in between, this verdict measures intent, not a defect.")
    if notes:
        result["notes"] = notes
    return result


def _peak_sign(values, a: int, b: int) -> int:
    peak = max(values[a:b + 1], key=abs)
    return 1 if peak >= 0 else -1


def _runs_of(flags: list[bool], t: list[float], min_duration: float,
             merge_gap: float, signed=None) -> list[tuple[int, int]]:
    """Contiguous index ranges where `flags` holds, gaps shorter than
    `merge_gap` seconds bridged, then ranges shorter than `min_duration` cut.

    Bridging matters because a carve momentarily dipping under the rate
    threshold is one turn, not three. But when `signed` is given, two spans are
    only bridged if they turn the same way -- otherwise a swing one way
    followed immediately by a swing back (a wipeout, usually) would be reported
    as a single turn whose direction contradicts its own net yaw."""
    spans, start = [], None
    for i, on in enumerate(flags):
        if on and start is None:
            start = i
        elif not on and start is not None:
            spans.append((start, i - 1))
            start = None
    if start is not None:
        spans.append((start, len(flags) - 1))

    merged = []
    for span in spans:
        close = merged and t[span[0]] - t[merged[-1][1]] <= merge_gap
        same_way = signed is None or (
            _peak_sign(signed, *merged[-1]) == _peak_sign(signed, *span) if merged else True)
        if close and same_way:
            merged[-1] = (merged[-1][0], span[1])
        else:
            merged.append(span)
    return [(a, b) for a, b in merged if t[b] - t[a] >= min_duration]


@tool(
    "find_events",
    "Locate moments worth zooming into: turns (sustained yaw rate, reported "
    "with the speed they cost), stalls, airborne stretches, planing "
    "transitions, and autopilot step boundaries. Turn speed-loss is the number "
    "to watch -- a turn that costs most of the board's speed is the signature "
    "of a turn drawing more energy than the wave supplies. Everything is "
    "derived from the trajectory CSV alone, so wave-relative events such as "
    "crest crossings are NOT available here; use Tests/AnalyzeCrossing.ps1 for "
    "those.",
    {
        "type": "object",
        "properties": {
            "name": {"type": "string"},
            "source": {"type": "string", "enum": ["latest", "baselines"], "default": "latest"},
            "kinds": {"type": "array", "items": {
                "type": "string",
                "enum": ["turn", "stall", "airborne", "planing_change", "step"]},
                "description": "Defaults to turn, stall, airborne."},
            "min_yaw_rate_dps": {"type": "number", "default": 30,
                                 "description": "Turn threshold. Ordinary trim sits near 2-17 deg/s."},
            "min_turn_s": {"type": "number", "default": 0.4},
            "stall_speed_cms": {"type": "number", "default": 150},
        },
        "required": ["name"],
    },
)
def find_events(name: str, source: str = "latest", kinds: list | None = None,
                min_yaw_rate_dps: float = 30, min_turn_s: float = 0.4,
                stall_speed_cms: float = 150) -> dict:
    path = find_csv(name, source)
    data = load(path)
    c, n = data["cols"], data["n"]
    t, speed = c["t"], c["speed"]
    kinds = kinds or ["turn", "stall", "airborne"]
    events = []

    if "turn" in kinds:
        rates = yaw_rates(t, c["yaw"])
        flags = [abs(x) > min_yaw_rate_dps for x in rates]
        for a, b in _runs_of(flags, t, min_turn_s, 0.25, signed=rates):
            seg = list(range(a, b + 2))          # rates[i] spans samples i..i+1
            peak = max(rates[a:b + 1], key=abs)
            v_in, v_out = speed[seg[0]], speed[seg[-1]]
            v_min = min(speed[i] for i in seg)
            net_yaw = wrap180(c["yaw"][seg[-1]] - c["yaw"][seg[0]])
            events.append({
                "kind": "turn",
                "t0": r(t[seg[0]], 2), "t1": r(t[seg[-1]], 2),
                "duration_s": r(t[seg[-1]] - t[seg[0]], 2),
                "direction": "right" if net_yaw >= 0 else "left",
                "yaw_change_deg": r(net_yaw, 1),
                "peak_yaw_rate_dps": r(peak, 1),
                "roll_abs_max_deg": r(max(abs(c["roll"][i]) for i in seg), 1),
                "speed_in_cms": r(v_in, 0), "speed_out_cms": r(v_out, 0),
                "speed_min_cms": r(v_min, 0),
                "speed_loss_pct": r(100 * (v_in - v_min) / v_in, 1) if v_in > 1 else None,
                "step": int(c["step"][seg[0]]),
            })

    if "stall" in kinds:
        for a, b in _runs_of([s < stall_speed_cms for s in speed], t, 0.5, 0.25):
            events.append({"kind": "stall", "t0": r(t[a], 2), "t1": r(t[b], 2),
                           "duration_s": r(t[b] - t[a], 2),
                           "speed_min_cms": r(min(speed[a:b + 1]), 0),
                           "step": int(c["step"][a])})

    if "airborne" in kinds:
        for a, b in _runs_of([u < 0.05 for u in c["underwater"]], t, 0.2, 0.1):
            events.append({"kind": "airborne", "t0": r(t[a], 2), "t1": r(t[b], 2),
                           "duration_s": r(t[b] - t[a], 2),
                           "z_max_cm": r(max(c["z"][a:b + 1]), 0),
                           "step": int(c["step"][a])})

    if "planing_change" in kinds:
        for i in range(1, n):
            if (c["planing"][i - 1] < 0.5) != (c["planing"][i] < 0.5):
                events.append({"kind": "planing_change", "t0": r(t[i], 2),
                               "to": "planing" if c["planing"][i] >= 0.5 else "off-plane",
                               "speed_cms": r(speed[i], 0), "step": int(c["step"][i])})

    if "step" in kinds:
        for i in range(1, n):
            if int(c["step"][i]) != int(c["step"][i - 1]):
                events.append({"kind": "step", "t0": r(t[i], 2),
                               "from_step": int(c["step"][i - 1]), "to_step": int(c["step"][i]),
                               "speed_cms": r(speed[i], 0)})

    events.sort(key=lambda e: e["t0"])
    out = {"name": path.stem, "duration_s": r(t[-1] - t[0], 1),
           "kinds": kinds, "count": len(events), "events": events}
    turns = [e for e in events if e["kind"] == "turn" and e["speed_loss_pct"] is not None]
    if turns:
        out["turn_rollup"] = {
            "count": len(turns),
            "median_speed_loss_pct": r(pct([e["speed_loss_pct"] for e in turns], 0.5), 1),
            "worst_speed_loss_pct": r(max(e["speed_loss_pct"] for e in turns), 1),
            "median_duration_s": r(pct([e["duration_s"] for e in turns], 0.5), 2),
        }
    return out


ROOT = resolve_root(None)


def main() -> None:
    global ROOT
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--project", help="Path to the GoneSurfing project directory "
                                      "(the one holding Saved/ and Tests/).")
    args = ap.parse_args()
    ROOT = resolve_root(args.project)
    mcp_lite.serve(SERVER_INFO, f"project root = {ROOT}")


if __name__ == "__main__":
    main()
