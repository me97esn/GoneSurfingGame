# Nose-dive bug — investigation notes

## Symptom

The surfboard pitches forward 30-45° even on flat water with no visible wave slope, and the pitch tends to "stick" near 30°. The forward pitch feels like it brakes the board (visual and visceral; not yet quantified). The same forward pitch is present at the moment of the [barrel-glide-through bug](barrel-glide-through-bug.md) — the user observes 30-45° forward pitch when the board crosses the wave wall (frame ~959, second wave-loop iteration).

## When it happens

- **Manual play, `Boards_on_flat_water`**: pitch builds up early; the board sits at a sustained forward angle even with no wave slope.
- **Autopilot `barrel-glide-through`**: pitch divergence from baseline grows from <2.4° in steps 0-4 to 18.3° in step 5 ("Surf along the wave"). Snapshot test catches this **only** since pitch was added to `Compare.ps1` on 2026-05-08; position drift in step 5 (211cm) is within the unmodified-run noise floor (251-739cm), so a position-only check would have called it OK.

## What the data shows

From `Compare.ps1` after the partial sideways-drag fix (commit `b047aaf1`):

```
per-step max-pitch: step0=0.4deg  step1=0.6deg  step2=0.6deg  step3=0.2deg  step4=2.4deg  step5=18.3deg
per-step max-pos:   step0=7cm     step1=5cm     step2=16cm    step3=12cm    step4=16cm    step5=211cm
```

Pitch divergence is the diagnostic signal; position drift hides in the noise.

User's visual: sustained 30-45° forward pitch during the glide-through. The "stuck near 30°" character is the fingerprint of the [`bottomLift` 30° gate](#why-stuck-near-30-pitch-mechanism-suspected) (see below).

## Why pitch dips (mechanism, suspected)

Two interacting issues:

### 1. Missing front-loaded lift (structural)

Real planing surfaces have a pressure spike near the leading edge — the lift center sits *forward* of the wetted-area centroid, more so as velocity increases. This is what makes "hang ten" possible: at speed, a real surfboard generates enough nose-region lift to support the rider's weight far forward of CoM.

The implementation has no force with this shape. All the upward terms are position-blind:

- **`upwardsThrust`** ([`FluidDynamics.cpp::calcThrustForce`](../Source/GoneSurfing/FluidDynamics.cpp)): magnitude `absSinPitch * v² * coef * amountUnderWater`, direction `boardUp`, applied per-actor with no forward bias. Uses `absSinPitch` so it's symmetric in pitch sign — fires as much for nose-down as nose-up. It adds upward force on any AOA but does nothing to *front-load* the lift distribution.
- **`horizontalVelocityBuoyancy`** ([`Buoyancy.cpp::calculateBuoyancyForce`](../Source/GoneSurfing/Buoyancy.cpp)): magnitude `(|vx| + |vy|) * coef * amountUnderWater`, direction `Lerp(worldUp, surfaceNormal, waveFaceNormalInfluence)`. Position-blind w.r.t. the board's forwards axis. At constant `amountUnderWater` across actors, this is pure heave (zero pitch torque).
- **Basic float buoyancy**: same direction, scales only with `amountUnderWater`. Position-blind.

There is no term whose application point or magnitude has a forward bias scaled by horizontal velocity. So the system has no "the faster I go, the more my nose stays up" feedback that real planing provides.

### 2. Why "stuck near 30°" — `bottomLift` mechanism (suspected)

`bottomLift` ([`FluidDynamics.cpp::calcLiftForce`](../Source/GoneSurfing/FluidDynamics.cpp)) is described in the surrounding comments as "Bernoulli suction" — direction `(0, 0, -1)` (world DOWN), magnitude `v² * apxLiftCoefficient(absTrueCosPitch) * trueCosYaw * liftMagnitude * amountUnderWater`. Each bottom actor multiplies by *its own* `amountUnderWater`.

When the board pitches slightly forward:
- Front bottom actors sit deeper → higher `amountUnderWater`.
- Back bottom actors are shallower → lower `amountUnderWater`.
- Suction (downward) is therefore stronger at the front than the back.
- Net torque: nose-down. Pitch increases. Repeat.

This is a positive-feedback loop on pitch under any forward perturbation, until the gate `absTrueCosPitch >= 0.87` (~30° pitch) cuts in and disables the term entirely. That is exactly where the user observes the board getting "stuck": pitch ramps up to ~30°, the gate fires, the destabilizer turns off, and the board holds near 30°.

(`bottomLift` was probably written intending a uniform suction that helps the board "stick" to a planing wave; the per-actor `amountUnderWater` scaling was the natural way to gate it on water contact, but it has the side effect of front-biasing under any non-zero pitch.)

### Why both matter

(1) explains why there's no restoring force toward a small positive AOA. (2) explains why even a tiny forward pitch is amplified rather than damped. Either alone might produce a marginally stable board; together they produce the observed "pitches forward to 30° and stays there" behavior.

## Force map (pitch perspective)

### Pitch-stabilizing (resist nose-down at small forward pitch)
- `upwardsThrust` — upward, scales with `|sin(pitch)|` so contributes only when AOA is non-zero. Symmetric in pitch sign; doesn't restore toward zero pitch by itself.
- Basic buoyancy — pure heave; no pitch torque on its own.
- `horizontalVelocityBuoyancy` — pure heave (position-blind); no pitch torque on its own.

### Pitch-destabilizing (drive nose-down) at small forward pitch
- `bottomLift` "Bernoulli suction" — downward; per-actor `amountUnderWater` introduces front-bias under forward pitch (positive feedback). Gated off above ~30° pitch.

### Doesn't apply to flat-water case
- ~~`waveSlopeSupplementForce` — zero on flat water (`slopeSin = 0`).~~ **Retired (2026-05-19):** the supplement, and its successor `waveSlopeGravityForce`, are both removed. The current forward-propulsion path is the bottom hydrofoil's `forwardsThrustCoefficient` — also zero at zero AOA, so the "doesn't apply to flat-water" framing still holds for the live successor.
- `lateralTurnForce`, `railLift`, `finLift`, drags — primarily yaw/roll/along-axis; not direct pitch drivers.

## Diagnosis confidence

**Moderate.** The pitch divergence (18.3° in step 5) is verified by the snapshot test. The bottomLift positive-feedback hypothesis is consistent with:
- The "stuck at 30°" pattern matching the gate threshold.
- The structural absence of any front-loaded lift term.

But it has not been confirmed by isolating the term (e.g. with `bottomLift` coefficient set to 0, the pitch should not run away). That experiment is cheap and worth running before committing to a fix on the destabilizer side.

The structural argument for adding front-loaded lift is independent: even if `bottomLift` isn't the destabilizer, the codebase has no term with the right shape to produce realistic planing pitch behavior.

## Tools available for verifying a fix

1. **Snapshot test with pitch tracking** ([`Compare.ps1`](../Tests/Compare.ps1), 2026-05-08). Per-step max-pitch divergence; pitch counts toward WARN/REGRESSION verdict (defaults 15° / 30°). Latest barrel-glide-through run shows `step5=18.3deg` — fix should bring this back near `step4=2.4deg` (or whatever the new step-4 figure ends up being).
2. **Debug flags** `lift`, `thrust`, `buoyancy` on bottom actors (`surf.debug.flags lift,thrust,buoyancy` `surf.debug.actors left_middle,right_middle,nose,tail,...`) — log every candidate term per-tick to see who's producing nose-down vs nose-up torque.
3. **Visual confirmation** in editor — pitch is easy to eyeball; "30-45° forward pitch on flat water" is unambiguously wrong.
4. **Isolation experiment**: set `bottomLift` coefficient to 0 and re-run autopilot. If the runaway pitch stops, the destabilizer hypothesis is confirmed. (Cheap; do this before deciding on destabilizer-side fixes.)

## Candidate fix directions (not yet decided)

These are sketches for future-me, not commitments. Each has trade-offs.

- **Front-load `horizontalVelocityBuoyancy`** *(recommended starting point)*. Per-actor scalar `1 + frontBias * clamp(forwardOffset / halfLength, -1, 1)` where `forwardOffset = (actor.location - surfboard.location) | boardForwards`. Front actors get a bonus, back actors a deficit; for symmetric placement the *sum* of forces is unchanged so heave is unaffected, only the moment about CoM gets a forward bias. Scales with horizontal velocity (matching real planing physics). Self-stabilizes via per-actor `amountUnderWater` feedback (front lifts → front emerges → contribution drops → equilibrium AOA reached). Requires per-actor `forwardOffset` cached at `setup()` (actors are positioned relative to the surfboard root, not named-by-role) and a new `frontBias` UPROPERTY (default 0). Equilibrium AOA is set by `frontBias` magnitude — needs tuning; expect a few degrees nose-up at planing speeds.

- **Remove the destabilizing `bottomLift` per-actor `amountUnderWater` scaling.** Replace with a single board-level "is the board planing" gate, applied as uniform suction at all bottom actors (or at the board centroid, no torque). Eliminates the positive feedback below 30° pitch. Risk: changes planing/suction feel — this term is also what keeps the board "stuck" to the wave when carving cleanly. Combine with (1) if the destabilizer isn't sufficient on its own.

- **Add a dedicated "leading-edge lift" term.** New force applied at the front of the board, scaled by velocity. Cleaner physics-wise but introduces another tunable; (1) achieves the same physical effect through the existing buoyancy system without adding terms.

- **Tune `upwardsThrust` to use `max(0, sinPitch)` instead of `absSinPitch`.** Physical correction (only positive AOA generates planing lift); turns the term into an asymmetric restoring force that helps at nose-down. Risk: at negative pitch (nose-up beyond wave-relative AOA, e.g. pumping), there's no lift to push the board back down — could destabilize the opposite way.

(1) and (2) target different mechanisms (add stabilizer vs. remove destabilizer); they're complementary if both are needed.

## File references

- `Source/GoneSurfing/Buoyancy.cpp` — `calculateBuoyancyForce` (basic + horizontal-velocity + vertical-velocity buoyancy)
- `Source/GoneSurfing/FluidDynamics.cpp::calcLiftForce` — `bottomLift` (suction, downward), `railLift`, `lateralTurnForce`, `finLift` (the `waveSlopeSupplementForce`/`waveSlopeGravityForce` paths referenced earlier were retired 2026-05-19)
- `Source/GoneSurfing/FluidDynamics.cpp::calcThrustForce` — `upwardsThrust` (`absSinPitch` lift, direction boardUp)
- `Source/GoneSurfing/SharedCalculations.cpp` — `pitchSinAngleOfAttack`, per-actor `amountUnderWater`
- `Tests/baselines/barrel-glide-through.csv` — baseline trajectory; pitch column (col 10) drives the diagnostic
- `Tests/Compare.ps1` — pitch tracking + WARN/REGRESSION thresholds (commit `99100cfe`)

## Related

- [`barrel-glide-through-bug.md`](barrel-glide-through-bug.md) — the wave-wall-pushback bug that motivated this investigation. Partial fix (sideways bottom drag, commit `b047aaf1`) deflects a level board off the wall but is overwhelmed when the board enters at 30-45° forward pitch — fixing the pitch is upstream of fully closing that bug.
- ~~`project_supplement_direction.md` (auto-memory) — `waveSlopeSupplementForce` direction-awareness; deferred.~~ Both the supplement and its `waveSlopeGravityForce` successor were retired 2026-05-19; forward propulsion is now the bottom hydrofoil's `forwardsThrustCoefficient`. The auto-memory entry is stale and should be removed.

## Investigation history

- **2026-05-08**: discovered as a separate failure mode after the partial sideways-drag fix (`b047aaf1`) didn't fully close `barrel-glide-through`. User observed sustained 30-45° forward pitch during glide-through and on flat water generally.
- **2026-05-08**: pitch tracking added to `Compare.ps1` (commit `99100cfe`). Verified that the latest `barrel-glide-through` run shows 18.3° pitch divergence in step 5 vs <2.4° in steps 0-4 — pitch is the diagnostic axis; position drift hides in the noise floor.
- **2026-05-08**: spec written; `bottomLift` per-actor `amountUnderWater` front-bias identified as the suspected destabilizer; front-loaded `horizontalVelocityBuoyancy` identified as the recommended stabilizer fix.
- **2026-05-08**: implemented front-loaded `horizontalVelocityBuoyancy` fix in `Buoyancy.{h,cpp}`. New UPROPERTYs `horizontalVelocityBuoyancyFrontBias` (default 0.5) and `boardHalfLength` (default 100cm). Per-actor `forwardOffset` cached on first tick from `(actor.location - surfboard.location) | sharedCalculations->forwards`. Force scaled by `1 + frontBias * clamp(forwardOffset / boardHalfLength, -1, 1)` then clamped to ≥ 0.
- **2026-05-08**: snapshot test result with fix — verdict OK. **step 5 pitch divergence: 18.3° → 8.2°** (55% reduction). Other steps within noise floor; step 0 picked up a small pitch divergence (0.4° → 5.9°, planing-AOA settling). Bug is significantly mitigated; isolation experiment for `bottomLift` was not run because the stabilizer-side fix alone hit OK threshold.

Open follow-ups (deferred):
- The residual 8.2° step-5 pitch divergence is still notably above the step-4 floor of ~3°, suggesting `bottomLift` still contributes some destabilization. If user observes lingering nose-down feel, run the `bottomLift = 0` isolation experiment and/or apply candidate fix #2 (remove per-actor `amountUnderWater` scaling on `bottomLift`).
- `frontBias=0.5` is a starting value, not tuned by feel. Adjust per-actor in BP if equilibrium AOA is too low/high under the rider's weight.
