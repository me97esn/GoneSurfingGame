# Findings: Wave-Catch Propulsion Budget (why the board planes as soon as the wave arrives)

> Measurement/analysis doc, 2026-07-21. Feeds [steep-face-takeoff.md](steep-face-takeoff.md) (the behavior change).
> Method: `RunGameAndCollectLogs.bat "torque:SharedCalculations:bottom-turn"` — the per-tick force
> budget (`TORQUE` lines, both SCs) joined with the trajectory CSV (`Saved/Tests/latest/bottom-turn.csv`).

## The observed problem

In the `bottom-turn` autopilot, the board takes off the moment the wave reaches it: at t≈4.2 s
(`boardWideSlopeSin` crossing ~0.10–0.12, a ~6–7° face) speed surges 22 → 430 cm/s in ~0.6 s and the
board planes. Realistically it should stay glued (displacement mode) until the face is steep, then
accelerate down it.

## Key facts (measured)

1. **Planing is a consequence, never a cause.** `AmountPlaning` keys purely on board speed
   (starts 300 cm/s, full 400 — `SharedCalculations::calculateAmountPlaning`). It stayed 0.000 until
   the board was already at ~350 cm/s. None of the propulsion terms below is planing-gated, so
   raising planing thresholds cannot prevent the takeoff.
2. **Force ranking during the surge window** (t 4.0–4.8 s, mean shoreward force per tick, both SCs
   summed; shoreward = −`Fin` in the torque budget):

   | Rank | Torque-budget category | Shoreward force | Knob |
   |---|---|---|---|
   | 1 | `dragBottom_waveMassFlow` + `dragRailFlow` | ~35 kN | `waveMassFlowDragCoefficient` (0.002) |
   | 2 | `bottomSlopeThrust` | ~25 kN | `slopeThrustCoefficient` (40000), gated by `slopeThrustMinSlopeSin` (0.12) |
   | 3 | `dragTail` | ~8.5 kN | `tailDragCoefficient` (15) skin part + `waveMassFlowDragCoefficient` tail variant (unsplit in the budget) |
   | — | everything else | < 1 kN each | `waveMassThrustCoefficient` measured ~15 N — negligible |

   Bottom skin drag (`dragBottom_forwardDrag`) was *braking* (−1.5 kN); buoyancy and gravity are
   purely vertical in the world frame (`Fin = Fdown = 0`).
3. **The takeoff timing is set by `slopeThrustMinSlopeSin`** — acceleration begins on the tick the
   slope crosses its 0.12→0.18 smoothstep — while **the waveMassFlow push has no slope deadzone at
   all** (magnitude scales linearly with slopeSin from 0), so it shoves the board on arbitrarily
   gentle faces.
4. **"Drag" terms are propulsion at wave-catch.** Every term whose direction follows the water's
   (relative or absolute) velocity pushes the board *forward* when the wave's water overtakes a slow
   board. This is physically right (a wave does shove a board shoreward); the issue is magnitude
   vs. face steepness, not sign.

## Verified kill-switch (what makes the wave unrideable)

Via `Saved/TuningOverrides.json` (see the JSON A/B workflow):

```json
{ "slopeThrustCoefficient": 0, "waveMassFlowDragCoefficient": 0, "tailDragCoefficient": 0, "waveMassThrustCoefficient": 0 }
```

Result: max speed 194 cm/s for the whole run, planing 0.000 throughout, and the wave passes under
the board — slopeSin peaks at **0.47** directly beneath it (vs. 0.27 max in the baseline, where the
board accelerates away before the face fully steepens). `waveMassThrustCoefficient` is negligible;
the practical minimal set is the other three. (Overrides restored to `{}` after the experiment.)

## Implications

- To delay takeoff until the face is steep, gate the *wave-mass flow* terms on a minimum slope the
  same way slope thrust already is, and raise both thresholds — see
  [steep-face-takeoff.md](steep-face-takeoff.md).
- The un-gated tail skin drag (`tailDragCoefficient`, v²-relative shove from behind) is the largest
  remaining pusher after those two are gated; whether it alone can reach planing speed on a gentle
  face is an open question the takeoff-gate A/B answers directly.
- Logging gotchas hit during measurement: the `torque`/`state` dumps require a **non-empty**
  `surf.debug.actors` match (`SurfDebug::ShouldDebug` returns false on an empty actor list), and
  PowerShell passes space-free args to `.bat` files unquoted, so cmd splits them at commas —
  `"torque,state::bottom-turn"` silently became flags=`torque` with no autopilot filter.
