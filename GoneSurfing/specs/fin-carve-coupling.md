# Spec: Fin Carve Coupling (Lift Perpendicular to Flow)

> **✅ Resolved (2026-06-16) by [fin-force-normalization.md](fin-force-normalization.md).** This
> spec's premise — that `sinSlip = cosYawAngleOfAttack` is a true `sin(α)` — was broken by a scale
> bug: [SharedCalculations.cpp:79](../Source/GoneSurfing/SharedCalculations.cpp#L79) dotted the
> **un-normalized** `this->left` (mag ≈ 0.2) with a unit vector, so `cosYawAngleOfAttack` was
> `0.2 × true sin(α)`, and the `liftDir` was built from the un-normalized `left`/`forwards` (a second
> ~0.2 scale) — net ~0.04× (≈25× too weak), with `cosSlip ≈ 1` always so the forward carve-coupling
> component was dead. The fix normalized the basis at point of use: `sinSlip` now reads the
> true-cosine `cosYawAngleOfAttackLeftN` and `liftDir` is built from unit `left`/`forwards`
> ([FluidDynamics.cpp:837](../Source/GoneSurfing/FluidDynamics.cpp#L837)). Measured 2026-06-16:
> `|sinSlip|` now spans up to 0.998 (was capped ~0.2) and `cosSlip` reaches 0.06, so the lift rotates
> toward `+forwards` as intended. **This spec's AC2 (≤5° skid) is now achievable and met** — peak
> skid in the t=6.5–7.5 s window measured 4.0°/4.1° after the accompanying `finDragCoefficient`
> retune (0.1 → 0.01). See the "Full fin-subsystem audit" section in
> [barrel-glide-through-bug.md](barrel-glide-through-bug.md) for the original analysis.

## Overview

Make the fin's lift force point **perpendicular to flow** instead of perpendicular to the board. The existing implementation (`finLiftDirection = ±board.left`) only captures the anti-slip projection of a true hydrofoil lift; the forward projection (`L · sin α`, "induced thrust") is missing. Restoring the geometric lift vector adds the forward component while preserving the anti-slip behavior exactly, and it makes the force a single vector perpendicular to velocity — which is the physics of a carving fin.

Targets the ~7–8° skid observed in `surfing-down-the-line` between t≈6.5–7.5 s, where the board's heading rotates faster than the velocity vector.

Scope: rewrite the fin-lift direction and magnitude in `AFluidDynamics::calcLiftForce`. No new coefficients, no engine-fork changes. Existing `finLiftMagnitude` UPROPERTY stays.

## Background

A fin at slip angle α produces lift L = C · v² · |sin α|, perpendicular to flow. Decomposed in board frame:

- **Anti-slip component** = L · cos α, along ∓`board.left`.
- **Forward component** = L · |sin α|, along +`board.forwards`.

The existing implementation at [FluidDynamics.cpp:572-599](../Source/GoneSurfing/FluidDynamics.cpp#L572) computes a force with magnitude `v² · |sin α| · cos α · finLiftMagnitude` along `−sign(sinSlip) · board.left`. That's *exactly* the anti-slip projection of the hydrofoil lift, missing the forward part.

### Observed skid (pre-spec)

From the 2026-05-17 `surfing-down-the-line` snapshot:

| t (s) | yaw | v   | v_fwd | v_lat | skid° |
|------:|----:|----:|------:|------:|------:|
|  6.60 | 60° | 628 |   616 |  −76 |  −7.0 |
|  6.86 | 51° | 566 |   561 |  −75 |  −7.7 |
|  7.25 | 38° | 493 |   491 |  −36 |  −4.2 |

Peak skid in the 6.5–7.5 s window is 7.7°. AC2 targets ≤5° peak.

### Why a single perpendicular-to-flow force is the right shape

The lift force perpendicular to flow ≈ perpendicular to velocity (slip angle is small). A force perpendicular to velocity does no work on speed magnitude — it only rotates velocity direction. That's what "carve" means: heading-to-velocity angle closes, speed stays roughly constant.

A pure-forward force (the rejected Option B) doesn't rotate velocity — it adds forward translation that the v² forward damping immediately eats. Empirically (commit `cdcc350bc`, since reverted), Option B at coef=1 and coef=10 gave peak skid 8.4–8.6° — slightly worse than pre-spec, never better. The perpendicular-to-flow direction is structurally different: its anti-slip projection moves the velocity vector laterally, which is the rotation we need.

### Yaw torque side effect

Applying the new force at the fin location creates yaw torque (anti-slip component is perpendicular to the fin's position offset). The torque sign reinforces the board's existing yaw rotation — same as the current fin lift already does. So board heading rotates faster too. The skid reduction comes from velocity rotation rate being closer to heading rotation rate, not from the fin slowing down yaw. If post-spec the board over-rotates visibly (heading swings far past the intended carve angle), revisit by reducing `lateralTurnCoefficient` (the bottom-actor force that's the primary yaw driver) rather than weakening the fin lift.

## Required Reading

A fresh implementer should skim these.

### Coordinate convention

`board.forwards = local +Y` (mesh rotated 90°). `board.left` — important caveat per the existing fin-lift comment at [FluidDynamics.cpp:585-587](../Source/GoneSurfing/FluidDynamics.cpp#L585): the in-engine `board.left` vector points to the **surfer's right**. Don't try to reconcile the naming; just use the field as the existing fin code does.

### Existing code touchpoints

- [FluidDynamics.cpp:559-599](../Source/GoneSurfing/FluidDynamics.cpp#L559) — existing fin lift block. The full rewrite target.
- [FluidDynamics.cpp:716](../Source/GoneSurfing/FluidDynamics.cpp#L716) — sum line in `calcLiftForce` return. Stays unchanged.
- `cosYawAngleOfAttack` on fin actors — by the comment at FluidDynamics.cpp:563-566, on fin actors this holds `board.left · relWaterVelDir`, i.e., signed `sin(slip)`. Field is set per-actor in BeginPlay (see [SharedCalculations.cpp:53-54](../Source/GoneSurfing/SharedCalculations.cpp#L53) for the general case). Reuse it; same semantics.

### Build, run, snapshot

Same workflow as [water-mass-aware-forces.md](water-mass-aware-forces.md). Run, observe `surfing-down-the-line`, compare CSV t=6.5–7.5 s skid to pre-spec values in the table above.

## Design

### Force shape

```
sinSlip   = cosYawAngleOfAttack                       // signed sin(α); reused field, not a typo
absSinSlip = |sinSlip|
cosSlip   = sqrt(max(0, 1 - sinSlip²))
v²        = relativeWaterVelocity.SizeSquared()

// Hydrofoil lift magnitude (textbook): L = C · v² · |sin α|
L = v² · absSinSlip · finLiftMagnitude

// Direction = perpendicular to flow, in the horizontal plane around board.up.
// Construct from the existing anti-slip unit vector rotated toward +board.forwards by α:
antiSlipDir = +sign(sinSlip) · board.left              // opposes slip — direction of the flow's sideways component
liftDir     = antiSlipDir · cosSlip + board.forwards · absSinSlip

finLiftForce = liftDir · L
```

Decomposed in board frame:

- Anti-slip component magnitude: `L · cosSlip = v² · |sin α| · cos α · finLiftMagnitude` — **identical to the current implementation**.
- Forward component magnitude: `L · absSinSlip = v² · sin²(α) · finLiftMagnitude` — new (induced thrust).

### Why no new coefficient

The textbook hydrofoil decomposition uses a single coefficient C. Existing `finLiftMagnitude` already plays that role for the anti-slip projection. Adding a separate forward-component knob would let the two projections diverge from physical reality. If we want to scale carve feel without changing anti-slip, the right knob is the lateral turn force on the bottom (the primary yaw driver), not a fin-side cheat.

### Why direction construction by rotation rather than cross-product

`cross(board.up, relWaterVelDir)` would give a perpendicular-to-flow vector, but resolving its sign (which side of flow is the suction side) requires comparing against `board.left`, and the codebase's mesh-rotation-induced sign conventions make that error-prone. Constructing from the existing `antiSlipDir` (which the codebase has already gotten right) and rotating toward `+board.forwards` reuses the well-tested direction logic.

### Magnitude change vs current code

Pre-spec magnitude is `v² · |sin α| · cos α · finLiftMagnitude` along anti-slip. Post-spec magnitude is `v² · |sin α| · finLiftMagnitude` along perpendicular-to-flow. The post-spec **anti-slip projection** has magnitude `v² · |sin α| · cos α · finLiftMagnitude` — identical to pre-spec. The post-spec **forward projection** is `v² · sin²(α) · finLiftMagnitude` — new, ≈12% of anti-slip at α=7°, scales with sin²(α).

So `surfing-down-the-line` should diverge from the pre-spec baseline only by the forward component's effect, which is small at small slips and zero at zero slip.

### Energy / speed effects

The lift force is (approximately) perpendicular to velocity, so it changes velocity direction, not magnitude. No speed-wall fight (the forward damping doesn't preferentially eat perpendicular-to-velocity force). No "free energy" — the carve redirects kinetic energy from lateral to forward, but the total stays the same.

Caveat: at non-trivial slip, the lift vector deviates slightly from "exactly perpendicular to velocity" (because the magnitude formula sin α makes the lift smaller than what would be needed for true perpendicularity at large α). Small effect at the slips we're observing (<10°).

## Implementation Sketch

Replace [FluidDynamics.cpp:571-599](../Source/GoneSurfing/FluidDynamics.cpp#L571) with:

```cpp
FVector finLiftForce = FVector::ZeroVector;
if (this->side == ESide::VE_Fin && finLiftMagnitude > 0.0f)
{
    const float sinSlip = this->cosYawAngleOfAttack;
    const float absSinSlip = FMath::Abs(sinSlip);
    const float sinSlipSq = sinSlip * sinSlip;
    const float cosSlip = FMath::Sqrt(FMath::Max(0.0f, 1.0f - sinSlipSq));
    if (absSinSlip > 0.001f)
    {
        // Fully submerged by construction — no amountUnderWater / effectiveWaterHeight gating.
        const float v2 = this->sharedCalculations->relativeWaterVelocity.SizeSquared();

        // Hydrofoil lift magnitude (textbook): L = C · v² · |sin α|.
        const float liftMagnitude = v2 * absSinSlip * finLiftMagnitude;

        // Direction perpendicular to flow, in the horizontal plane around board.up.
        // Construct as the anti-slip unit vector rotated toward +board.forwards by α.
        // Decomposed in board frame, the resulting force has:
        //   anti-slip component (along ∓board.left) = L · cos α    — matches the previous behavior
        //   forward component   (along +board.forwards) = L · |sin α| — new (induced thrust / carve coupling)
        const FVector antiSlipDir = FMath::Sign(sinSlip) * this->sharedCalculations->left;
        const FVector liftDir     = antiSlipDir * cosSlip
                                   + this->sharedCalculations->forwards * absSinSlip;
        finLiftForce = liftDir * liftMagnitude;

        if (debugLiftLog)
        {
            const float antiSlipMag = liftMagnitude * cosSlip;
            const float forwardMag  = liftMagnitude * absSinSlip;
            UE_LOG(LogTemp, Warning, TEXT("Fin Lift FIRED [%s] - sinSlip: %.3f, cosSlip: %.3f, v²: %.1f, coef: %.3f -> L: %.2f (antiSlip=%.2f fwd=%.2f), force: (%.1f, %.1f, %.1f)"),
                *GetName(), sinSlip, cosSlip, v2, finLiftMagnitude,
                liftMagnitude, antiSlipMag, forwardMag,
                finLiftForce.X, finLiftForce.Y, finLiftForce.Z);
        }
    }
}
```

The return-sum line at [FluidDynamics.cpp:716](../Source/GoneSurfing/FluidDynamics.cpp#L716) is unchanged.

### Cleanup from Option B

The previous implementation added `finCarveCouplingCoefficient` and a separate force branch. Remove both:
- Delete the UPROPERTY at [FluidDynamics.h:124-131](../Source/GoneSurfing/FluidDynamics.h#L124).
- Delete the new branch and the orange debug-draw cone added after Option B's force computation.
- Drop `finCarveCouplingForce` from the return-sum.

## Coefficient Tuning

`finLiftMagnitude` is the only knob and is already set in the umap (current value tuned to pre-spec anti-slip behavior).

Expected behavior at the current umap value: anti-slip projection magnitude is unchanged, plus a forward component that scales as sin²(slip). At α=7.7° this is ~1.8% of v² × finLiftMagnitude — a meaningful nudge over a 1–2 s carve. If the run shows skid still > 5°, try increasing `finLiftMagnitude` modestly (1.5× → 2×). Note that raising it also raises anti-slip force, which raises yaw torque on the board — so over-rotation may grow alongside velocity rotation. Watch for the heading swinging past intended carve angle.

If raising `finLiftMagnitude` over-rotates the heading too much without satisfying AC2, try reducing `lateralTurnCoefficient` on the bottom actors (the dominant yaw driver) in parallel.

## Acceptance Criteria

### AC1 — Anti-slip behavior preserved at current `finLiftMagnitude`

With `finLiftMagnitude` unchanged from pre-spec value, `surfing-down-the-line` snapshot test stays in the WARN bucket (not REGRESSION), and per-step max pitch is within ~2° of pre-spec. The anti-slip projection is mathematically identical to pre-spec, so the only trajectory divergence should come from the new forward component.

### AC2 — Skid reduction

Peak skid in `surfing-down-the-line` t=6.5–7.5 s drops from ~7.7° to ≤5°. Reduction visible at multiple consecutive samples (not just one) and board still completes the turn (yaw still passes through 0° on schedule).

### AC3 — Force is zero at zero slip

At paddling slip (≈0°, step 1), force is zero. Verifiable via `debugLiftLog` — "Fin Lift FIRED" only fires when `absSinSlip > 0.001`.

### AC4 — Top speed unchanged

Max forward speed during step 3 stays within ~10% of pre-spec (≈627 cm/s pre-spec). The new force is perpendicular to velocity, so it shouldn't materially change speed.

### AC5 — `surf-straight` unaffected

`surf-straight` snapshot test stays within its current noise band. This spec isn't trying to fix it; shouldn't make it worse either.

## Open Questions / Future Work

1. **Apply to the rails too?** A submerged rail at slip is also a hydrofoil (vertical-ish chord, horizontal lift). The rail-lift code at [FluidDynamics.cpp:382-455](../Source/GoneSurfing/FluidDynamics.cpp#L382) currently does Bernoulli-shaped suction along `−board.up`. Generalizing it the same way would close the same physics gap on the rails. Defer until fin spec is baselined.

2. **Bottom hydrofoil.** Sibling spec [bottom-hydrofoil-thrust.md](bottom-hydrofoil-thrust.md) does the same transformation in the pitch plane: replace `calcThrustForce`'s `board.up` direction with perpendicular-to-flow in the forward-up plane. Bigger change, depends on this one landing first.

3. **Over-rotation as a deeper issue.** If AC2 misses after tuning `finLiftMagnitude`, the cause is likely the `lateralTurnCoefficient` over-rotating the board faster than the fin can rotate the velocity. Worth investigating as a separate concern.

## Status

- [ ] Spec reviewed
- [ ] Option B implementation reverted (`finCarveCouplingCoefficient` UPROPERTY and force branch removed)
- [ ] Fin lift in `calcLiftForce` rewritten to perpendicular-to-flow direction
- [ ] Build clean
- [ ] AC1 — `surfing-down-the-line` snapshot stays WARN (not REGRESSION); pitch within ~2° of pre-spec
- [ ] AC2 — Peak skid in t=6.5–7.5 s ≤ 5°
- [ ] AC3 — Force zero at zero slip
- [ ] AC4 — Max forward speed within 10% of pre-spec
- [ ] AC5 — `surf-straight` within noise
- [ ] Snapshot baseline re-approved if trajectory looks right
