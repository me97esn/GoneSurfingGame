# Spec: Wave hull-normal impact (wave hits the presented bottom) — REMOVED

> **STATUS (2026-06-25): REMOVED from the code.** Labeled per-actor force logging showed this term fired
> only on the TAIL at the breaking-wave slam, delivering a ~22750 straight-UP force (board.up) — a large
> nose-down/tail-up pitch torque that *lofted* the board rather than restoring it to the face. Setting
> `waveHullImpactCoefficient=0` removed the loft (tail vertical force +24069 → +2508) but the board **still
> crossed to the far side** (crest-relative trace unchanged). So the term neither caused nor cured the
> crossing — it only added a spurious vertical loft. All code (`FluidDynamics`, `SurfTuningSubsystem`) was
> deleted. This doc is kept only as a record of the dead end.
>
> Note: the "de-planing" framing in [wave-crossing-deceleration.md](wave-crossing-deceleration.md) is also
> refuted — the board is fully planing throughout the crossing (`AmountPlaning` lags, so its later decay was
> misread as a cause). The investigation is being restarted.

> **STATUS (2026-06-25): proposed.** Third piece of the glide-through fix, after the slope-thrust
> front-face gate ([wave-crossing-deceleration.md](wave-crossing-deceleration.md)) and the pitch
> alignment ([wave-face-pitch-alignment.md](wave-face-pitch-alignment.md)). The pitch fix now turns the
> board's bottom to face the oncoming breaking wave — but no force converts "wave momentum hitting the
> presented bottom" into a push, so the board still glides over to the far side.

## Problem

The bottom-surface wave forces are **orientation-blind**:

- **`waveMassFlowDrag`** ([FluidDynamics.cpp:234](../Source/GoneSurfing/FluidDynamics.cpp#L234)) pushes
  along the *flow direction* (`boardWideAbsWaterVel.GetSafeNormal()`), magnitude `coef·slopeSin·effH·v²`.
  `board.up` never appears — it ignores how the hull is angled to the flow.
- **`wavePenetrationDrag`** ([:272](../Source/GoneSurfing/FluidDynamics.cpp#L272)) keys on the board's
  *cross-face velocity* and is gated above a 150 cm/s threshold → **zero at the slow cresting moment**.
- The bottom **hydrofoil upthrust** is airfoil-style lift (∝ `sinAOA`, perpendicular to flow, from the
  *relative* flow); it under-models the high-angle **form impact** of a strong flow slamming a presented
  hull, and uses relative not absolute (wave-mass) velocity.

So when the board pitches up and the breaking wave hits its bottom, nothing pushes it back.

### Verification (2026-06-25, `turn-hard-into-the-wave`)

Computed `absWaterVel · board.up` (hull-normal component of the wave flow) through the crossing:
- Bottom-facing phase: flow into the bottom up to **~150 cm/s, ~33% of the ~480 cm/s flow**; `+board.up`
  there points up-and-back (−X) → a push along it is **restoring** (back onto the wave, off the lip).
- Sign flips as the board banks (flow later hits the deck). So the effect is real and large but
  **intermittent** — it fires during the bottom-facing phase, which is the case we care about.

## Objective

Add a bottom force that converts the wave's hull-normal momentum into a push along the hull normal — the
orientation-aware complement to `waveMassFlowDrag`. When the breaking wave drives into the presented
bottom, push the board off that face (up-and-back), helping deflect it back onto the wave instead of
gliding over.

## The force

Per bottom (`VE_Down`) actor, using the board-wide **absolute** wave velocity (the wave-mass momentum,
same source as `waveMassFlowDrag` — and deliberately *not* the relative flow, so it does not overlap the
relative-flow hydrofoil lift):

```cpp
const FVector boardUpUnit = this->sharedCalculations->up.GetSafeNormal();   // NORMALIZE (0.2-scale basis)
const float intoBottom = FVector::DotProduct(boardWideAbsWaterVel, boardUpUnit); // >0 = flow into the bottom
if (intoBottom > 0.0f) {                                                     // bottom-facing only (first cut)
    const float amount = waveHullImpactCoefficient * boardWideEffectiveH * amountWetted * (intoBottom * intoBottom);
    waveHullImpact = boardUpUnit * FMath::Clamp(amount, 0.0f, maxDragAmount);
}
```

- `(intoBottom)²` = flat-plate normal momentum flux (∝ ρ A v²sin²α). Quadratic in the normal component, so
  it's small at low angle of attack (clean planing, flow ~along the hull) and large when the wave slams a
  presented hull — naturally isolating the slam from normal riding.
- Direction `+board.up` (push off the bottom). Gated `intoBottom > 0` → only when the wave drives *into*
  the bottom (the targeted phase); deck-hits (`intoBottom < 0`) are ignored in the first cut.
- Gated on `amountWetted` and `boardWideSlopeSin > 0` (on a wave face), like `waveMassFlowDrag`.
- Added into the bottom `dragForce` sum alongside `waveMassFlowDrag` / `wavePenetrationDrag`.

### Sign / direction

`+board.up` during the bottom-facing phase points up-and-back (−X = restoring), confirmed in the
verification. Re-confirm empirically when wiring (log `intoBottom` and the force), as with the other
direction-sensitive terms.

### Overlap & regression risks

- **Hydrofoil upthrust:** uses *relative* flow + airfoil lift; this term uses *absolute* wave velocity +
  form impact, and the `intoBottom²` shape keeps it ~0 at the small AOA of clean planing. Watch the
  `surfing-down-the-line` regression for added lift / disturbed planing; if it intrudes, add a deadzone on
  `intoBottom` (subtract a threshold, like `wavePenetrationDrag`) to isolate the high-impact regime.
- **Yaw/translation:** applied per bottom actor at its location like the other bottom drags; board-wide
  `absWaterVel` keeps it ~symmetric (no spurious yaw), consistent with `waveMassFlowDrag`.

## Tunables

- `waveHullImpactCoefficient` on [SurfTuningSubsystem.h](../Source/GoneSurfing/SurfTuningSubsystem.h) +
  mirror on [FluidDynamics.h](../Source/GoneSurfing/FluidDynamics.h), copied at init (the established FD
  pattern). Default 0 = disabled; set to a tuned value once verified. Trial start ~0.005 (scaled to land
  in the same force range as `waveMassFlowDrag`).
- Optional `waveHullImpactThreshold` if a deadzone proves necessary.
- Log `intoBottom` + force under the `drag` flag.

## Acceptance criteria

- **AC1:** With the term enabled, `turn-hard-into-the-wave` no longer glides to the far side — the board
  is pushed back / deflected onto the face (net horizontal force at the crossing turns restoring).
- **AC2:** `intoBottom` and the force log show the term firing during the bottom-facing phase and ~0
  during clean planing.
- **AC3 (regression):** `surfing-down-the-line` unharmed — speed/planing comparable; no spurious lift or
  pitch/altitude disturbance from the new force.
- **AC4:** Flat-water autopilots unaffected (`boardWideSlopeSin` ~0 there → term off).
- **AC5:** `waveHullImpactCoefficient = 0` reproduces current behaviour exactly.

## Test plan

1. Implement with default 0; confirm no change. Then enable with the trial coefficient.
2. `turn-hard-into-the-wave` with `state,drag,abs_velocity` — verify `intoBottom > 0` during the
   bottom-facing phase, the force is restoring, and the crossing outcome improves (AC1, AC2).
3. `surfing-down-the-line` regression (AC3); add a deadzone if planing is disturbed.
4. Tune `waveHullImpactCoefficient` for deflection without overshoot.

## Implementation note (2026-06-25): gating

First trial (ungated, `coef=0.005`) **broke normal surfing** — the wave drives into the angled bottom
(`intoBottom > 0`) during *all* face riding, not just the crossing, so the ungated upward force launched
the board and both autopilots terminated right after pop-up.

Gating evolved (author guidance):
- ❌ `amountUnderWater` — its 0→1 range is too gradual to gate on; rejected.
- ❌ `AmountPlaning` (de-planing gate) — **planing lags**: it stays full while the board is fast and only
  drops *after* other forces have already slowed it (the deceleration analysis in
  [wave-crossing-deceleration.md](wave-crossing-deceleration.md)). A de-planing gate would fire too late —
  after the board has already washed up — so it can't deflect the approach. Rejected.
- ✅ **`amountWetted`**: the wetted-area scale ([Buoyancy.cpp:194](../Source/GoneSurfing/Buoyancy.cpp#L194)) —
  a sharp 0→1 smoothstep over ~8 cm of submersion: ~0 when the hull skims the surface (clean planing), 1
  when it's submerged / engulfed by the breaking wave. Pure geometry ⇒ **instant, no lag**, and it's the
  conventional surface-force scale the hydrofoil/lift already use. `F ∝ amountWetted · (absWaterVel·up)²`.

But `amountWetted` **saturates to 1** whenever the hull is submerged (>8 cm), so it scales the force but
can't *gate* it — verified: it read 1.0 throughout a normal ride and the force still fired (~2780/actor),
disrupting it. The actual discriminator is a **deadzone on `intoBottom`** itself (mirrors
`wavePenetrationThreshold`):
- `intoBottomExcess = max(0, intoBottom − waveHullImpactThreshold)`, force ∝ `excess²`. Normal riding/pop-up
  drives ~75 cm/s into the bottom; the breaking-wave slam ~120–150. Threshold `90` cuts the former, passes
  the latter. No lag, no saturation.
- **Per-actor (not board-wide) absolute wave velocity:** the wave hits the FRONT of the board (front SC
  ~150); a board-wide front/back average is diluted by the weak back flow (~75) and never clears the
  deadzone. Per-actor also front-loads the impact where the wave strikes.

### Verification (headless, 2026-06-25)

- **`surfing-down-the-line` (regression):** hull force fires **0 ticks** — `intoBottom` (~75) stays under
  the deadzone, so normal riding is untouched. (The autopilot's early quit seen during tuning was
  step-timing run-variance with the board healthy/planing, not the hull force.)
- **`turn-hard-into-the-wave`:** fires **28 ticks** at the crossing (`intoBottom` up to 117, force up to
  ~2819/actor) — engages exactly at the breaking-wave slam.

**Settings:** `waveHullImpactCoefficient = 0.02`, `waveHullImpactThreshold = 90`. The force is correctly
*gated* (off in normal riding, on at the slam) but its *magnitude* at 0.02 is modest (~14 k total vs the
~24 k net toward crossing measured in wave-crossing-deceleration.md) — likely needs raising to actually
deflect. **Tune `waveHullImpactCoefficient` up interactively** where the crossing outcome is visible; the
headless trajectory recorder stops at the autopilot's terminal step (~gs6.96), before the deepest crossing,
so the deflection magnitude can't be judged headlessly.

## Open questions

- Symmetric (also handle deck-hits, `intoBottom < 0`, pushing −board.up) vs bottom-only (first cut)? Start
  bottom-only; revisit if deck-hit phases matter.
- Absolute (wave-mass) vs relative flow: starting absolute to complement `waveMassFlowDrag` and avoid the
  hydrofoil overlap; revisit if the slow-board regime needs the relative flow.
- Could this *replace* `wavePenetrationDrag` (the velocity-gated wall term that's ~0 when it's most
  needed)? Evaluate after AC1.

## File references

- [FluidDynamics.cpp](../Source/GoneSurfing/FluidDynamics.cpp) `calcDragForce` `VE_Down` block — add the
  term next to `waveMassFlowDrag`.
- [FluidDynamics.h](../Source/GoneSurfing/FluidDynamics.h) / [SurfTuningSubsystem.h](../Source/GoneSurfing/SurfTuningSubsystem.h) — coefficient.
- [SharedCalculations.h](../Source/GoneSurfing/SharedCalculations.h) — `up` (normalize before use),
  `absoluteWaterVelocity`, `boardWideSlopeSin`.
- Related: [wave-crossing-deceleration.md](wave-crossing-deceleration.md),
  [wave-face-pitch-alignment.md](wave-face-pitch-alignment.md).
