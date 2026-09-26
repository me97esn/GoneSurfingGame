# Planing depth equilibrium — depth-ramped upthrust vs constant-when-wetted suction

> **STATUS (2026-07-24): ABANDONED — implementation lives on the unmerged branch
> `abandoned/plane-higher-up` (commits `cd5654b47` + `741c2acbb`); do NOT merge it into main.**
> After full implementation, replay-tuning, and two rounds of on-device play, the player verdict
> was that the surface-planing board is net WORSE than main: it doesn't hold its line well, and
> turns are both too slow and too unpredictable. The core problem (turns fight a roll-righting
> spring whose stiffness varies with per-rail depth and v² near the surface — see the
> "surface-carve" sections below) was mitigated (board-wide ramp blend, stronger player weight
> torque, split player/autopilot torque) but never beat the old deep-riding feel. This spec is
> kept on main as the record of the design, measurements, and iteration history in case the
> planing-ride-height idea is picked up again. The sections below are a snapshot of the branch
> state at abandonment and describe code that exists ONLY on that branch.

> **STATUS (2026-07-23, superseded): IMPLEMENTED + tuned on replay; awaiting player validation.** The fix for
> [submerged-downline-glide.md](submerged-downline-glide.md): the board rides 30–50 cm below the
> surface at planing speed because the bottom hull's dynamic vertical forces self-cancel at every
> depth, leaving buoyancy-vs-gravity (equilibrium ~40 cm under) to set ride height. This spec gives
> the bottom a *depth-crossing* dynamic equilibrium so the hull itself pins the ride height near the
> surface at speed.
>
> **Implementation outcome (see "Measured results" at the bottom):** the mechanism works but two
> design assumptions were corrected by measurement — (1) the constant down-force in the crossing is
> **gravity**, not the bottom suction (suction measured ~−600, negligible); the grows-with-depth
> up-side is **buoyancy + the ramped upthrust**. (2) The ramp **replaces** `actorWetted` on the
> up-force term instead of stacking on it — `actorWetted`'s 60 cm smoothstep was itself a huge
> depth gate that killed the upthrust near the surface, exactly where the planing force must peak.
> Tuned on the `14-51-09` replay to `upthrustDepthRampDistance = 10`,
> `upwardsThrustCoefficient = 25` (via `Saved/TuningOverrides.json`, not yet baked as defaults):
> the down-line glide phase rides at **10–20 cm** (was 30–50 the whole ride), sinks only in
> low-speed phases (physically correct), floats at the old ~50 cm when slow.

## Problem recap (measured, 2026-07-17 force budget)

- Buoyancy grows with depth (113k @ 20–32 cm → 286k @ 48–70 cm) against constant ~250k gravity →
  hydrostatic equilibrium ~40–48 cm under. That is where the board rides.
- Bottom suction (down, ~−4k) ≈ bottom upthrust (up, ~+3.4k) **at every depth** — the hull
  contributes ~zero net vertical force, so nothing planes the board up onto the surface.
- Root cause in the formulas: both terms scale with `v² × actorWetted` and `actorWetted`
  saturates at 1.0 a few cm below the surface ([FluidDynamics.cpp:1318](../Source/GoneSurfing/FluidDynamics.cpp#L1318),
  [FluidDynamics.cpp:914](../Source/GoneSurfing/FluidDynamics.cpp#L914)). Their ratio is a
  depth-independent constant, so no depth is dynamically preferred.

## Design (author's scheme, 2026-07-23)

**Upthrust grows with depth; suction stays on/off-when-wetted.** Then there is exactly one depth
`d*` where they are equal, and it is a *stable* equilibrium:

- deeper than `d*` → upthrust > suction → net up → board rises;
- shallower than `d*` → suction > upthrust → net down → board settles back.

The suction side already has the required shape (constant once `actorWetted` saturates) — **no code
change on the lift/suction path.** The entire mechanical change is a depth ramp on the upthrust.

### Emergent properties (why this shape is right)

1. **Speed-independent target depth, speed-dependent stiffness.** Both terms ∝ v², so v² cancels in
   the crossing condition — `d*` is set purely by coefficients. But the *restoring force* around
   `d*` scales with v²: fast board = hull dominates and pins ride height near `d*` (planing); slow
   board = dynamic pair is weak and the board sinks to the old buoyant equilibrium (floating).
   Planing-on-top emerges from speed with no explicit speed gate. (The v² cancellation is
   approximate — upthrust uses `v²InPitchPlane × sinAOA`, suction uses `v² × liftCoef(pitch) ×
   cosYaw` — so `d*` drifts somewhat with attitude. Acceptable.)
2. **Free pitch/roll righting.** The ramp input is the *per-actor* depth (`waterColumnAbove`,
   sampled at each bottom actor's own position), so a nose-down board has deeper nose actors →
   more up-force at the nose → restoring pitch torque. Same for roll. This extends the per-actor
   asymmetry mechanism from [per-actor-wetting.md](per-actor-wetting.md) from the ~8 cm wetted band
   to the full ramp depth.

### Formula change

In `calcThrustForce` ([FluidDynamics.cpp:1318](../Source/GoneSurfing/FluidDynamics.cpp#L1318)),
scale **only the upForce term** (`commonMagUp` path — the forward-thrust terms keep their current
scaling) by a clamped linear depth ramp:

```
depthRamp = clamp(waterColumnAbove / upthrustDepthRampDistance, 0, 1)
upForce   = upUnit × upEffectiveCoef × commonMagUp × depthRamp × upDot
```

- **Signal = raw per-actor `waterColumnAbove`** (cm of water above the actor). NOT
  `effectiveWaterHeight` — its `baseHeight` floor and `slopeSin × slopeHeight` term keep it
  non-zero for non-submerged actors (deliberately, for other forces), which would push the board
  *out* of the water; a ride-height servo must go to 0 at the surface. NOT the per-SC
  `amountUnderWater` — board-wide, so it can't carry the per-actor righting asymmetry.
- `upthrustDepthRampDistance` — new tunable in `USurfTuningSubsystem` (JSON-A/B-able per
  [[tuning-overrides-json-ab]]). Ramp saturates at 1 beyond this depth (bounded force when buried).

### Relation to bottom-hydrofoil-upthrust-decoupling.md

That spec removed `effectiveWaterHeight` from the upForce on the argument "real hydrofoil lift
doesn't scale with water-column-above." This spec *reintroduces* a depth factor — deliberately and
with a different signal and rationale: not wave-mass energy scaling, but a ride-height servo that
creates the missing planing equilibrium. The decoupling spec's real point (upForce shouldn't
inherit the wave-energy effH with its baseHeight/slope terms) still stands; `depthRamp` shares none
of those terms.

## Magnitude sizing — the shape alone is not enough

Today's upthrust (~3.4k) is noise against the ~137k hydrostatic deficit at 20–32 cm. The full
equilibrium condition at target ride depth `d_t` is

```
B(d_t) − G + U(d_t) − L = 0
```

Using the measured budget (both SCs summed, v ≈ 480 cm/s): G ≈ 250k, B ≈ 4.3k/cm (linearized),
L ≈ 4k. The required upthrust slope is `k ≈ (G − B(d_t) + L) / d_t`:

| target ride depth | required U at that depth | vs today's 3.4k |
|---|---:|---:|
| 20 cm | ~168k | ~50× |
| 10 cm | ~210k | ~60× |

So `upwardsThrustCoefficient` (currently 0.2) must rise into the ~5–15 range, tuned together with
`upthrustDepthRampDistance`. Rough starting point: ramp distance 40 cm, coefficient sized so
U(saturated) ≈ 400–500k at v = 480 (≈ coef 10–12 at the measured `v²·sinAOA ≈ 14k`) → equilibrium
lands ~10–15 cm and shallower as speed rises. Expect iteration; the measured `v²·sinAOA` operating
point is itself attitude-dependent and will shift once the board rides higher.

**Do NOT try to reach the target by also shrinking suction to ~0** — suction is the shallow-side
restoring force in this scheme; without it the only down-force near the surface is gravity and the
equilibrium logic still works, but `d*` becomes purely hydrostatic-vs-U and loses the tunable
dynamic crossing.

## Risks / open questions

1. **`sinAOA` gate on upthrust.** Upthrust fires only at positive pitch AOA; suction fires whenever
   |pitch AOA| < ~30°. Riding perfectly flat to the flow (sinAOA → 0) kills the upthrust at any
   depth while suction persists → net down. Expected to be self-correcting (board sinks slightly →
   per-actor depth asymmetry + rocker restore a positive trim angle, which is how real planing
   hulls ride), but if flat-trim sinking shows up in practice, add a small AOA-independent
   depth-ramped term rather than flooring sinAOA.
2. **Engine Z ceiling and damping.** `MaxVelocityZUp` (500 in tuning, 1000 in SurfboardUtils) caps
   upward velocity, and the fork's per-axis Z damping resists the rise. The 2026-07-17 budget never
   measured either. A 50× upthrust boost may slam into the ceiling during pop-up — watch for the
   board "elevatoring" at exactly the cap, and for damping eating the restoring stiffness
   (over-damped = slow surfacing, fine; ceiling-limited = tune down).
3. **Wave-catch / takeoff timing will change.** The upthrust participates in the catch (see
   [[wave-catch-propulsion-budget]]). A 50× coefficient is a big perturbation — every snapshot
   baseline will regress (intentionally). Re-tune takeoff feel, then `Approve.ps1` new baselines
   after player validation.
4. **Suction magnitude re-check.** With the board riding near the surface, `actorWetted` leaves its
   pegged-at-1.0 regime and starts doing real gating (its ~8 cm band finally overlaps the operating
   depth). Both force magnitudes change regime simultaneously — measure, don't assume.
5. **Oscillation.** A stiff v²-scaled spring around `d*` with a capped/damped mass can porpoise.
   If it does, the fix is lower stiffness (longer ramp distance) before any new damping term —
   the engine Z damping already provides the dashpot.

## Validation plan

1. **A/B without recompile** where possible: coefficient + ramp distance via
   `Saved/TuningOverrides.json` (the ramp-distance tunable must land in `USurfTuningSubsystem`).
2. **Replay budget**: rerun the `phone-2026-07-17-14-51-09` force budget
   (`surf.debug.flags 'crossing,torque'`) — acceptance: NET Fz crosses zero at the tuned `d*`,
   with U(d) visibly ramping and L ~constant in the depth-binned table.
3. **Ride height**: `Tests/AnalyzeCrossing.ps1 -Trace phone-2026-07-17-14-51-09` — acceptance:
   mean submersion during the down-line glide drops from 30–50 cm to ≤ ~15 cm, with sustained
   ticks at/above the surface.
4. **Low speed still floats**: at rest / paddle speed the board must sit at the old buoyant depth
   (no dynamic lift-out at v ≈ 0 — guaranteed by the v² factor, verify anyway).
5. **Regression**: full snapshot suite; expect intentional trajectory shifts everywhere the board
   planes; player-validate feel (esp. pop-up and down-line) before `Approve.ps1`.

## Measured results (2026-07-23, `phone-2026-07-17-14-51-09` replay, ride window 6–13.5 s)

Per-frame Fz budget binned by SC submersion (parser: scratchpad `ParseBudget.ps1`; run with
`surf.debug.flags 'crossing,torque,thrust'`, `surf.debug.actors 'SharedCalculations,bottom'`).
Iteration history (mean submersion over the window):

| config | mean subm | note |
|---|---:|---|
| baseline (pre-change) | 31 cm | rides at buoyant equilibrium |
| ramp 40 / coef 10 (stacked on wetted) | 31 cm | matched linear model's predicted 29.5 — mechanism confirmed, too weak |
| ramp 20 / coef 25 (stacked) | 28 cm | diminishing returns: `actorWetted`(60 cm) is a second depth gate crushing near-surface force |
| ramp 5 / coef 25 (stacked) | 25.7 cm | confirmed: upthrust 74k @20–30 cm but only 26k @0–10 cm |
| **gate swap: ramp replaces wetted** ramp 20 / coef 25 | 21.5 cm | dominant bin now 10–20 cm, budget balances there |
| ramp 20 / coef 35 | 22.2 cm | self-regulating: higher ride → slower → v² down → upthrust down |
| ramp 10 / coef 25 | 24.3 cm* | *mean inflated by slow phases; **glide phase rides 10–20 cm**, pops to ~7 cm |
| **world-Z redirect** ramp 10 / coef 25 (current) | 17.1 cm (glide) | speed 435 vs 527 default (board-up direction had cost 324); hydrofoilPitch Fdown brake −9.9k → 0 |

**Speed A/B vs defaults (glide window, `CompareRuns.ps1`):** the board-up/⊥-flow up-force direction
leaned ~16% backward along the down-line axis — a **−9.9k brake** (vs −0.8k default) that cost ~40%
of terminal speed (324 vs 527 cm/s). Fix: with the ramp active, the up-force is applied along
**world +Z**, sized by the vertical share of the ⊥-flow reaction (`thrustDir.Z`) — the same
simulation-level shortcut the bottom suction uses in the opposite direction. Result: brake
eliminated (Fdown exactly 0), speed 435 cm/s, ride height 17 cm. The remaining −92 cm/s vs defaults
is legitimate submersion-gated propulsion loss (bottomSlopeThrust 5.1k→1.3k, hydrofoilYaw
3.7k→1.2k, effH −33% — riding ON the water couples less wave energy). Trade ride height back for
speed by LOWERING the coefficient (rides deeper → more wave coupling); with world-Z the full
magnitude is vertical, so coef ~12–15 ≈ the old ride depth of board-up coef 25, with no brake.

Key findings baked into the design:

- **Gravity is the constant down-force** (−100k in this config); bottom suction is negligible
  (~−600 vs the old budget's −4k). Buoyancy (≈15k @0–10 cm → 80k+ @40 cm, ~4.3k/cm) provides a
  depth slope that stabilizes ANY equilibrium — the ramp's job is magnitude near the surface, not
  creating stability from scratch.
- **v² coupling self-regulates the last few cm**: pushing the board higher on the face slows it,
  which cuts upthrust — coefficient increases beyond ~25 buy almost nothing (35 measured no better).
  Slower board rides deeper; that gradient is physically right and worth keeping.
- **Fz/|upForce| ≈ 0.5–0.6 in-ride**: the board-up-directed force leaks a large lateral/forward
  component on the tilted board + inclined flow (hydrofoil force is ⊥ flow, not ⊥ surface). Size
  coefficients against the measured Fz, not the force magnitude.
- Low-speed floating unchanged (v² factor): pre-catch the board sits at ~51 cm as before.
- `MaxVelocityZUp` never approached (surfacing at ~50–100 cm/s vs the 500 cap).

## Compensated coefficient preset (2026-07-23)

The shallower ride shrinks every submersion gate during the glide (measured, defaults vs world-Z
ramp10/coef25: `effectiveWaterHeight` ×1.49, `amountUnderWater` ×2.95, `amountWetted` ×6.9), so
coefficients tuned at the old ~43 cm depth produce weaker forces at ~17 cm. Two A/B presets in
`Saved/TuningPresets/` (copy over `Saved/TuningOverrides.json` to switch):

- **A-current.json** — ramp 10 / coef 25 only (forces at whatever the new gates give).
- **B-compensated.json** — same, plus per-category magnitude compensation from the measured glide
  ratios (÷1.47 for board-v²-scaled categories, since restoring the force balance also restores
  speed): `bottomDragCoefficient` 0.4→0.7, `yawHydrofoilCoefficient` 0.001→0.0017,
  `bottomLiftMagnitude` 0.001→0.007, `railDragCoefficient` 1e-5→1.3e-5, `slopeThrustCoefficient`
  40000→150000, `lateralTurnCoefficient` 4000→1800 (lateralTurn measured ~2× STRONGER at the
  surface — more roll — so it's scaled DOWN).

**Replay verification of B (glide, vs defaults):** speed 519 vs 527 cm/s (−1.5%), submersion
15.5 cm, and the gated categories within ~±30 % of default magnitudes (dragBottom 0.92,
forwardDrag 1.05, dragRail 1.02, bernoulli 0.81). Known deviations: bottomSlopeThrust overshoots
~1.8× (redirect drive grew with restored speed — trim 150000→~85000 if takeoff feels pushy);
yawHydrofoil reads low because slip itself shrank at speed (behavioral, don't chase); catch phase
is ~19 % slower than defaults (constant-coefficient compensation overshoots drag when the board is
DEEP — the inherent limit of compensating a gate change with a constant; the proper long-term fix
is re-tuning or depth-aware gates).

Skipped (measured ≈1 or not depth-gated): finDrag/finLift (ungated by construction), tailDrag,
railLift (intent-gated), waveMass*/wavePenetration (water-velocity/threshold-based),
waveSlopeGravity + lateralTurn (planing-gated — though lateralTurn got the roll-driven correction
above).

## References

- [submerged-downline-glide.md](submerged-downline-glide.md) — the diagnosis this fixes.
- [bottom-hydrofoil-upthrust-decoupling.md](bottom-hydrofoil-upthrust-decoupling.md) — why upForce
  currently has no effH (partially superseded here, see section above).
- [per-actor-wetting.md](per-actor-wetting.md) — the per-actor depth sampling this builds on.
- Auto-memory: [[submersion-gates-comparison]], [[wave-catch-propulsion-budget]],
  [[tuning-overrides-json-ab]].
