# Spec: Steep-Face Takeoff (board stays glued until the wave is steep)

> Implements the behavior change motivated by
> [wave-catch-propulsion-budget.md](wave-catch-propulsion-budget.md) (read that first — it has the
> measured force ranking and the verified kill-switch experiment).

## Overview

Today the board accelerates and planes the moment the arriving wave's face reaches
`slopeSin ≈ 0.10–0.12` (~6–7°). The two dominant pushers at wave-catch are the wave-mass flow drag
(~35 kN, **no slope deadzone** — fires linearly from slopeSin 0) and the passive slope thrust
(~25 kN, deadzone at 0.12). The result is an unrealistic instant takeoff on a barely-sloped face.

## Objective

The board stays glued to the water (displacement mode, speed well below planing) while the face is
gentle, and only accelerates/planes once the face is genuinely steep. Takeoff steepness must be a
designer-tunable dial. Riding behavior *after* takeoff (planing on a steep face) is unchanged.

## Design

Two changes, one shared idea: **both wave-sourced propulsion families get a minimum-slope smoothstep
deadzone, and both thresholds move up to "steep face" territory.**

1. **Raise `slopeThrustMinSlopeSin`**: default 0.12 → **0.25** (~14.5° face). Tuning-only; the knob
   and its smoothstep (min → min+0.06) already exist.
2. **New tunable `waveMassMinSlopeSin`** (default **0.25**, category `Tuning|WaveMass`): a smoothstep
   gate `SmoothStep(min, min + 0.06, slopeSin)` multiplied into every wave-mass force magnitude:
   - bottom `waveMassFlowDrag` (uses `boardWideSlopeSin`)
   - rail `flowDrag` (uses `boardWideSlopeSin`)
   - tail wave-mass flow variant (uses per-actor `slopeSin`, keeping that site's existing per-actor
     semantics)
   - bottom `waveMassThrust` (uses `boardWideSlopeSin`; negligible at current tuning but same
     physical family, gated for consistency)

   Each site's existing `slopeSin > 0` early-out becomes `slopeSin > waveMassMinSlopeSin`.

### Why smoothstep-with-deadzone rather than a sinusoid ramp

A sinusoid (or any curve reshape like slopeSin²) still produces non-zero force on gentle faces —
it only redistributes the ramp; to make a gentle face truly force-free it needs a threshold anyway.
Once a threshold exists, smoothstep *is* the sinusoid-shaped S-curve onset (C¹-continuous, no kink),
and it's the established idiom in this codebase (slope-thrust gate, pitch-align gate, intent gates),
so the gate reads and tunes identically everywhere. The takeoff steepness also becomes a *direct*
designer dial (sin of the takeoff angle), which a pure curve reshape doesn't offer. The linear
`slopeSin` magnitude factor above the gate already delivers "stronger when steeper".

### What is deliberately NOT gated

- **`wavePenetrationDrag`** — it's a resistance (wall), not propulsion; gating it would re-open the
  glide-through bug.
- **Tail skin drag (`tailDragCoefficient`)** — a real v²-relative shove from water hitting the tail;
  measured ≤ 8.5 kN *combined* with the tail flow variant. Left alone initially; see AC3/Test Plan —
  if it alone still produces takeoff on a gentle face, lower `tailDragCoefficient` (tuning) rather
  than adding another gate.
- **Bottom/rail skin friction, hydrofoils, fin lift** — measured < 1 kN or braking at wave-catch.

### Known trade-off: takeoff gate = sustain gate

`boardWideSlopeSin` while *riding* often sits at 0.05–0.20 (the board outruns onto flatter shoulder
sections; see the surf-straight-is-slow note). Raising both thresholds to 0.25 also cuts wave-sourced
drive on those flatter stretches, so sustained rides will decelerate off the pocket more than today.
That is directionally realistic (you must stay in the pocket / pump), but if it proves too punishing,
the follow-up is a **planing-based hysteresis** (once planing, the effective threshold drops, e.g. to
half) — out of scope here, noted as future work.

## Requirements

- **FR1**: `waveMassMinSlopeSin` tunable exists in `USurfTuningSubsystem` (category
  `Tuning|WaveMass`, in `kCategoryTable` — the two-edit rule), live-editable via HUD/JSON, copied to
  `AFluidDynamics` scratch each tick via `RefreshFromTuningSubsystem`.
- **FR2**: All four wave-mass force sites multiply their magnitude by
  `SmoothStep(waveMassMinSlopeSin, waveMassMinSlopeSin + 0.06, slopeSinUsedByThatSite)` and early-out
  below the threshold.
- **FR3**: `slopeThrustMinSlopeSin` default raised to 0.25 (subsystem header + FluidDynamics header
  kept consistent).
- **FR4**: Setting `waveMassMinSlopeSin = 0` reproduces today's behavior (gate fully open at any
  slope > 0) for A/B.
- **NFR1**: No change to wavePenetration, buoyancy, damping, planing thresholds, or any
  non-wave-mass force.
- **NFR2**: Debug logs at the gated sites report the gate value so the takeoff moment is visible in
  `drag`/`thrust` dumps.

## Acceptance Criteria

- **AC1 — delayed takeoff**: In the `bottom-turn` test, planing onset (AmountPlaning > 0) occurs
  only once `boardWideSlopeSin` has reached ≈ the new threshold (≥ ~0.22), not at ~0.10–0.12.
  Baseline for comparison: surge began t≈4.2 at slope 0.10; the unridden wave steepens to 0.47
  under a glued board, so takeoff still happens — just later, on a visibly steep face.
- **AC2 — still surfable**: After the delayed takeoff the board accelerates and planes
  (AmountPlaning reaches 0.8) — the wave is *harder to catch*, not uncatchable.
- **AC3 — glued phase is genuinely glued**: Before the threshold crossing, board speed stays below
  ~150 cm/s (no planing, no creeping takeoff from the un-gated tail skin drag). If violated, lower
  `tailDragCoefficient` and re-run.
- **AC4 — A/B reversibility**: `{"waveMassMinSlopeSin": 0, "slopeThrustMinSlopeSin": 0.12}` in
  `Saved/TuningOverrides.json` restores the old takeoff timing without a rebuild.

## Test Plan

1. Implement; build via `RunGameAndCollectLogs.bat "torque:SharedCalculations:bottom-turn"`.
2. From `Saved/Tests/latest/bottom-turn.csv`: check slope at first planing>0 row (AC1), max planing
   (AC2), max speed before threshold crossing (AC3).
3. A/B with overrides JSON (AC4).
4. Snapshot tests (`pop-up`, `surf-straight`, `surfing-down-the-line`, …) are **expected to change**
   — takeoff dynamics are different by design. Re-approve baselines (`Approve.ps1`) once behavior is
  accepted; do not chase the old trajectories.

## Status

- [x] Spec written (2026-07-21)
- [x] Implemented (2026-07-21)
- [x] AC1–AC3 verified (AC4 by construction — all three knobs are live tunables)
- [ ] Baselines re-approved

## Implementation notes (2026-07-21)

- `waveMassMinSlopeSin` added (subsystem default 0.25 + `kCategoryTable` entry + FluidDynamics
  scratch UPROPERTY + per-tick refresh). Gate `SmoothStep(min, min+0.06, slopeSin)` multiplied into
  bottom `waveMassThrust` / `waveMassFlowDrag`, rail `flowDrag` (board-wide slopeSin) and the tail
  flow variant (per-actor slopeSin); each site's `> 0` early-out now checks the gate.
- `slopeThrustMinSlopeSin` default 0.12 → 0.25 (subsystem + FluidDynamics headers).
- **AC3 contingency was needed**: with only the gates, the un-gated tail *skin* drag measured
  ~40 kN at wave-catch (bigger than baseline — the glued board lets fast wave water stream past,
  and the term is v²-relative) and crept the board to 192 cm/s at slopeSin 0.13.
  `tailDragCoefficient` 15 → 3 fixed it (validated via TuningOverrides.json A/B, then baked in).
- **Measured result** (bottom-turn): glued phase ≤ 93 cm/s at slope 0.16; planing onset at
  slopeSin 0.269 (baseline: surge from 0.10); peak speed 629 cm/s — takeoff is *later and
  stronger*, since the face is steeper and the water faster when the gates open.
- Caveat: on this test wave the face steepens 0.10 → 0.27 in ~0.6 s under a slow board, so the
  takeoff *time* only shifts ~0.3–0.5 s; the visible difference is the takeoff *steepness* and the
  clean glued phase. On slower-building waves the delay will be much more pronounced.
- **Final defaults (live-tuned by the author 2026-07-21, baked into the headers)**:
  `waveMassMinSlopeSin = 0.35`, `slopeThrustMinSlopeSin = 0.35` (~20° takeoff face),
  `tailDragCoefficient = 1`, `waveMassFlowDragCoefficient = 0.001` (halved — softens the shove
  overall, not just its onset). Spec-text values (0.25 / 3 / 0.002) were the first-pass defaults.
- Measurement gotcha (cost two dead runs): the Stop hook (`BuildAndLaunch.bat` on dirty .cpp/.h)
  kills `UnrealEditor.exe` — including a freshly-launched `-game` test — if the agent's turn ends
  while `RunGameAndCollectLogs` is in its early game phase. Runs with a no-op build are the ones at
  risk (game launches within seconds of the turn ending).
