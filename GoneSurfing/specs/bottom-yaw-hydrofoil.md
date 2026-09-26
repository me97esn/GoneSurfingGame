# Spec: Bottom Yaw Hydrofoil (Forward Thrust from Sideways Flow)

## Overview

Extend the bottom hydrofoil model in `AFluidDynamics::calcThrustForce` from a pitch-only hydrofoil to one that also produces thrust from **yaw angle-of-attack** — i.e., when the board points across the flow rather than into it. This adds the missing forward propulsion mechanism that lets a board surfing down the line on a steep wave convert the wave's lateral water motion into forward speed, the same way a planing surface or an edged ski does in reality.

Scope: extend `calcThrustForce` on `ESide::VE_Down` actors. The existing pitch-AOA thrust ([FluidDynamics.cpp:660-746](../Source/GoneSurfing/FluidDynamics.cpp#L660)) and the existing sideways drag ([FluidDynamics.cpp:109-127](../Source/GoneSurfing/FluidDynamics.cpp#L109)) remain. No engine-fork changes. No BP changes (BP calls the same applier).

## Motivation

A 2026-05-21 instrumented run of `surfing-down-the-line` (logged with `surf.debug.flags 'thrust,drag,lift'`) shows the board reaches the down-the-line phase, glides for ~1.5 s, then stalls. Force-magnitude breakdown during the stall:

| step-4 t | Planing | sinAOA (pitch) | boardFwdForceMag (pitch hydrofoil) | bottomFwdDrag | sidewaysBottomDrag | finLift fwd | relWaterVel |
|---|---|---|---|---|---|---|---|
| 0.5 s | 0.95 | +0.056 | 1082 N | 26 N | 339 N | ~2 N | 568 cm/s |
| 1.5 s | 0.95 | +0.023 (only one actor) | 8902 N | 153 N | 30 N | ~5 N | 413 cm/s |
| 2.5 s | 0.94 → decaying | **all negative** | **0 N** | 6–8 N | 3000–5500 N | ~2 N | 116 cm/s |
| 4.5 s | 0.04 | +0.013 | 19 N | 224 N | 110 N | ~3 N | 42 cm/s |

The pitch hydrofoil gates out at t≈2 s because the board levels off relative to the flow direction (positive sinAOA → zero). From that point there is **no forward force on the board**: sideways drag is by construction perpendicular to `board.forwards`, the lateral-turn carve coefficient is currently 0, fin lift forward peaks at ~55 N (~2% of pitch-hydrofoil output), and the retired `waveSlopeGravityCoefficient` is gone. The board decelerates with no propulsion, planing collapses, and the bottom forward drag's `(1-AmountPlaning)²` attenuation snaps off — but by then drag is the consequence, not the cause. **Forward force goes to zero first**.

Physically, the missing mechanism is the bottom acting as a hydrofoil for **yaw** flow. A planing surface yawed relative to its flow deflects water laterally and produces a reaction force perpendicular to the flow, in the plane of the surface — the same physics as the existing pitch hydrofoil, just on the yaw axis. The forward component of that force is what propels a surfboard down the line.

## Background

### Coordinate convention

`board.forwards = local +Y` (mesh rotated 90°). Reading axes from the actor's transform per existing convention.

The in-engine `board.left` points to the **surfer's right** (see the comment at [FluidDynamics.cpp:528-529](../Source/GoneSurfing/FluidDynamics.cpp#L528)). Don't reconcile the naming; mirror the fin-lift sign convention.

### Existing pitch hydrofoil

[FluidDynamics.cpp:660-746](../Source/GoneSurfing/FluidDynamics.cpp#L660) — `calcThrustForce`. Projects `relativeWaterVelocity` onto the **forwards × up** plane (pitch plane), gates on `sinAOA > 0.001` in that plane, produces thrust perpendicular to in-plane flow. Forward component scales with `sinAOA² × v²`. Independent up/forward coefficients per the 2026-05-19 split.

### Existing sideways bottom drag

[FluidDynamics.cpp:109-127](../Source/GoneSurfing/FluidDynamics.cpp#L109) — projects `relativeWaterVelocity` onto the **forwards × left** plane (bottom plane), removes the forwards component, and produces a force along `sidewaysVel.normalize()`. By construction this force has **zero forward component on the board** — it's pure lateral skin-friction drag. No planing attenuation.

The yaw hydrofoil added by this spec lives in the same projection plane as the sideways drag, but produces force **perpendicular** to the in-plane flow rather than along it. The two coexist: drag is dissipative (skin friction along the surface), the new term is conservative (deflection of momentum perpendicular to flow). Real planing surfaces produce both.

### Existing fin carve coupling

[FluidDynamics.cpp:530-558](../Source/GoneSurfing/FluidDynamics.cpp#L530), [fin-carve-coupling.md](fin-carve-coupling.md). The fin already does exactly this — lift perpendicular to flow in the horizontal plane, with anti-slip and forward components. The yaw hydrofoil added here is the same shape applied to the bottom surface, which has much larger area and produces correspondingly larger forces. Mirror the fin construction.

### Why the pitch hydrofoil alone isn't enough

The pitch hydrofoil's forward component scales with `sinAOA²` ([bottom-hydrofoil-thrust.md Split coefficient](bottom-hydrofoil-thrust.md#split-coefficient--forwardsthrustcoefficient-shipped-future-work-3-graduated) — that's why the split coefficient was needed in the first place). At small pitch AOA (level board on a smooth wave face) the forward share vanishes. The 2026-05-19 split papered over this for the pop-up-and-go-straight case by cranking `forwardsThrustCoefficient` to 0.3, but it can't paper over `sinAOA ≤ 0`.

The yaw hydrofoil's forward component scales with `sinYawAOA² × v²` — a **different** AOA variable. On a wave face with sideways water motion, sinYawAOA is the dominant signal even when sinPitchAOA is zero. The two terms are decoupled and additive.

## Design

### Force shape

```
relVel              = sharedCalculations->relativeWaterVelocity
relVelInBottomPlane = relVel - (relVel · this->up) × this->up        // project out the up component
v²InBottomPlane     = relVelInBottomPlane.SizeSquared()
if v²InBottomPlane < ε: return 0

flowDirYaw          = relVelInBottomPlane / sqrt(v²InBottomPlane)
sinSlip             = flowDirYaw · this->left                         // signed; sign = which rail the flow hits
absSinSlip          = |sinSlip|
cosSlip             = sqrt(max(0, 1 - sinSlip²))
if absSinSlip < ε: return 0                                           // aligned flow — pitch hydrofoil's job

// Direction: perpendicular to in-plane flow, in the bottom plane.
// Mirror the fin construction (anti-slip unit vector rotated toward +board.forwards by slip).
antiSlipDir         = +sign(sinSlip) × this->left   // opposes slip; equals the direction of the flow's sideways component
yawThrustDir        = antiSlipDir × cosSlip + this->forwards × absSinSlip

// Magnitude: hydrofoil lift shape, same as pitch term but on the yaw axis.
yawThrustMag        = yawHydrofoilCoefficient
                    × v²InBottomPlane
                    × absSinSlip
                    × amountUnderWater
                    × effectiveWaterHeight

yawThrust           = yawThrustDir × yawThrustMag
```

Decomposed in board frame:

- **Anti-slip component** = `yawThrustMag × cosSlip` along `∓board.left`. Opposes the sideways flow that the existing sideways drag is dissipating.
- **Forward component** = `yawThrustMag × absSinSlip` along `+board.forwards`. **The missing propulsion** — this is the term that drives the board down the wave face from lateral water motion.

### Combine with existing pitch hydrofoil

The pitch hydrofoil (existing) and yaw hydrofoil (new) project onto orthogonal planes. They are summed:

```
return pitchHydrofoilForce + yawHydrofoilForce;
```

A board with both positive pitch AOA AND yaw misalignment gets both terms. A board level in pitch but yawed gets only the yaw term — which is the down-the-line case. A board pitched but flow-aligned gets only the pitch term — the straight-line case.

### Why a new coefficient (not reuse `forwardsThrustCoefficient`)

`forwardsThrustCoefficient` scales the *forward component of the pitch hydrofoil*, which is a different axis. Reusing it would couple yaw-driven and pitch-driven propulsion under one knob and force every retune to pay tax on the other. Cleaner to add `yawHydrofoilCoefficient` and tune it independently.

Default value: start at the same order as `forwardsThrustCoefficient` (0.3) and bisect. The bottom area is much larger than the fin's, so this coefficient may need to be **smaller** than `forwardsThrustCoefficient` to avoid overpropulsion, despite scaling with `sinSlip²` not `sinAOA²` (yaw slip during down-the-line is often larger than pitch AOA, which amplifies the difference).

### Why the projection onto `this->up`

Symmetric to the pitch hydrofoil's `this->left` projection. The yaw hydrofoil lives in the **bottom plane** (forwards × left); vertical flow shouldn't contribute to yaw-AOA thrust — that's the pitch hydrofoil's job. Projecting out the vertical component cleanly separates the two.

### Relationship to existing sideways drag

The new yaw hydrofoil and the existing sideways drag share the same input (`relVelInBottomPlane`) but produce orthogonal forces:

- **Sideways drag** points along `sidewaysVel` (the projected-out-forward part of `relVelInBottomPlane`) — purely lateral relative to the board, dissipative.
- **Yaw hydrofoil** points perpendicular to `flowDirYaw` (the full in-plane flow direction) — has anti-slip + forward components, conservative.

Both can coexist. They model different aspects of the same planing surface: skin friction (drag) and momentum deflection (lift). Empirically, after this spec lands, expect to **reduce `bottomDragSidewaysCoefficient`** because part of what it was modeling (the deflection part) is now handled correctly. See Coefficient Retune.

### Performance (mobile)

The yaw hydrofoil normalizes `relVelInBottomPlane` to compute `flowDirYaw`, which requires one `FMath::Sqrt` per bottom actor per tick. Worth a sanity check given the ~10 bottom actors and the mobile target.

- **Frequency.** FluidDynamics actors tick at the game frame rate (their `Tick` applies a `force × DeltaTime` impulse — they don't run inside the physics substep callback). At ~10 bottom actors × 30 Hz mobile frame rate that's 300 sqrts/sec. At a worst-case ARM scalar sqrt of ~30 cycles, ~9 k cycles/sec ≈ 0.0009% of a 1 GHz core.
- **Existing baseline.** A bottom actor already does ~3–4 sqrts per tick today: `FMath::Sqrt(v2InPlane)` in the pitch hydrofoil ([FluidDynamics.cpp:680](../Source/GoneSurfing/FluidDynamics.cpp#L680)), two `GetSafeNormal()` calls in the bottom drag path ([FluidDynamics.cpp:100](../Source/GoneSurfing/FluidDynamics.cpp#L100), [FluidDynamics.cpp:119](../Source/GoneSurfing/FluidDynamics.cpp#L119)), plus a `cosSlip` sqrt on each fin ([FluidDynamics.cpp:536](../Source/GoneSurfing/FluidDynamics.cpp#L536)). The yaw hydrofoil adds one more sqrt of the same shape — not a new cost pattern, just a 25–33% relative increase in sqrt count per bottom actor.

**Decision: ship with `FMath::Sqrt`.** The absolute cost is invisible at the current call rate. If mobile profiling later identifies FluidDynamics ticks as hot (unlikely — physics body simulation and wave-surface sampling are higher-likelihood culprits), the cheap fallback is a one-line swap to `FMath::InvSqrt`, which several platforms accelerate with `rsqrtss` / `FRSQRTE` intrinsics. The more aggressive option — dropping the anti-slip component entirely to keep only the sqrt-free forward term `forwards × (relVelInBottomPlane · leftUnit)² × coef × amountUnderWater × effectiveWaterHeight` — is available but trades physics correctness for an optimization we don't need.

AC7 (frame time within ±5% of pre-spec) is the verification gate. If it fails, revisit; don't preempt.

### Negative cases

- Flow aligned with `board.forwards` (`absSinSlip < ε`): returns zero. Pitch hydrofoil handles this case.
- Board moving with water (`v²InBottomPlane < ε`): returns zero. No flow, no force. Correct.
- Board upside down: `cosSlip` always nonnegative. `yawThrustDir` direction follows the actual flow geometry. No sign pathology.

## Implementation Sketch

Add the new term to `calcThrustForce` in [FluidDynamics.cpp:660-746](../Source/GoneSurfing/FluidDynamics.cpp#L660). The existing function already returns `hydrofoilForce` (the pitch term); change it to return `pitchForce + yawForce`.

```cpp
FVector AFluidDynamics::calcThrustForce(float alongThrustCoefficient, float upwardsThrustCoefficient, FColor debugColor /* = FColor::Red */)
{
    if (this->side != ESide::VE_Down) return FVector::ZeroVector;

    const FVector relVel = this->sharedCalculations->relativeWaterVelocity;
    const float amountUnderWater = this->sharedCalculations->amountUnderWater;

    // === PITCH HYDROFOIL (existing) ===
    // Project onto forwards × up plane; thrust perpendicular to in-plane flow.
    // ... existing code from line 671 onward, factored into a local FVector pitchForce ...

    // === YAW HYDROFOIL (new) ===
    FVector yawForce = FVector::ZeroVector;
    if (yawHydrofoilCoefficient > 0.0f)
    {
        const FVector upUnit  = this->up.GetSafeNormal();
        const FVector relVelInBottomPlane = relVel - FVector::DotProduct(relVel, upUnit) * upUnit;
        const float v2InBottomPlane = relVelInBottomPlane.SizeSquared();
        if (v2InBottomPlane >= 1.0f)
        {
            const FVector flowDirYaw = relVelInBottomPlane / FMath::Sqrt(v2InBottomPlane);
            const FVector leftUnit = this->left.GetSafeNormal();
            const float sinSlip = FVector::DotProduct(flowDirYaw, leftUnit);
            const float absSinSlip = FMath::Abs(sinSlip);
            if (absSinSlip > 0.001f)
            {
                const float cosSlip = FMath::Sqrt(FMath::Max(0.0f, 1.0f - sinSlip * sinSlip));
                const FVector antiSlipDir = FMath::Sign(sinSlip) * leftUnit;
                const FVector yawThrustDir = antiSlipDir * cosSlip
                                           + this->forwards.GetSafeNormal() * absSinSlip;
                const float yawThrustMag = yawHydrofoilCoefficient
                                         * v2InBottomPlane
                                         * absSinSlip
                                         * amountUnderWater
                                         * this->effectiveWaterHeight;
                yawForce = yawThrustDir * yawThrustMag;

                if ((this->debugThrust || SurfDebug::ShouldDebug(this, TEXT("thrust"))) && shouldDebugLog())
                {
                    const float antiSlipMag = yawThrustMag * cosSlip;
                    const float fwdMag      = yawThrustMag * absSinSlip;
                    UE_LOG(LogTemp, Warning, TEXT("Yaw Hydrofoil [%s] - sinSlip: %.3f, cosSlip: %.3f, v²Bot: %.1f, coef: %.3f, amountUW: %.3f, effH: %.2f -> mag: %.2f (antiSlip=%.2f fwd=%.2f), force: (%.1f, %.1f, %.1f)"),
                        *GetName(), sinSlip, cosSlip, v2InBottomPlane, yawHydrofoilCoefficient, amountUnderWater, this->effectiveWaterHeight,
                        yawThrustMag, antiSlipMag, fwdMag,
                        yawForce.X, yawForce.Y, yawForce.Z);
                }
            }
        }
    }

    return pitchForce + yawForce;
}
```

### New UPROPERTY on `AFluidDynamics`

Add alongside the existing thrust coefficients in [FluidDynamics.h:97-119](../Source/GoneSurfing/FluidDynamics.h#L97):

```cpp
/** Coefficient for yaw-AOA hydrofoil thrust on the bottom. Force is perpendicular to in-plane
 *  flow in the board's bottom plane (forwards × left), with anti-slip and forward components
 *  proportional to cos(slip) and |sin(slip)| respectively. Magnitude scales with
 *  v²InBottomPlane × |sinSlip| × amountUnderWater × effectiveWaterHeight × this coefficient.
 *  Mirrors the fin's carve-coupling shape but on the much larger bottom surface, so this
 *  coefficient is typically much smaller than finLiftMagnitude despite the same force shape.
 *  Default 0 = disabled. Set on bottom-side actors only. See specs/bottom-yaw-hydrofoil.md. */
UPROPERTY(EditAnywhere, BlueprintReadWrite)
float yawHydrofoilCoefficient = 0.0f;
```

### Debug log gating

Reuse the `"thrust"` flag (same gate as the pitch hydrofoil). The new "Yaw Hydrofoil" log line sits alongside the existing "Bottom Hydrofoil" line so a `surf.debug.flags 'thrust'` run prints both.

### Debug-draw

Same draw call as the existing pitch hydrofoil. Use a distinct color (e.g., `FColor::Magenta`) so the two terms are visually separable when both fire.

## Coefficient Retune

After this spec, the board has two independent sources of forward thrust on the bottom: pitch (existing) and yaw (new). Their working ranges overlap during the down-the-line phase. Expect to:

1. **Set `yawHydrofoilCoefficient` first.** Start at 0.05 (much smaller than `forwardsThrustCoefficient = 0.3` because `sinSlip` during down-the-line is much larger than `sinPitchAOA`, sometimes 0.3-0.7). Run; observe the "Yaw Hydrofoil" log magnitudes during step 4. Target: forward component of yaw thrust in the 500-2000 N range during the down-the-line phase. Higher values cause the board to launch sideways out of the wave; lower values don't propel.
2. **Reduce `bottomDragSidewaysCoefficient`.** Currently 0.001 per the default; produces ~3-5 kN of pure lateral drag during the stall. With yaw hydrofoil added, part of what this drag was modeling (the deflection part) is now correct — leaving the dissipative skin-friction part. Halve and observe.
3. **Leave `forwardsThrustCoefficient` alone unless straight-line trajectory destabilizes.** The yaw hydrofoil may pick up a small share of straight-line propulsion from incidental slip — that's expected and welcome (the current straight-line top speed is undertuned; any boost from the new term is a win, not something to compensate for). Only revisit `forwardsThrustCoefficient` if `surf-straight` starts oscillating, spinning out, or showing other instability — i.e., if AC4 fails on *shape*, not on *speed*.

After retune, re-approve both `surfing-down-the-line.csv` and `surf-straight.csv` baselines.

## Acceptance Criteria

### AC1 — Compiles, calls back into the same applier

`calcThrustForce` still has the same signature and is called from `applyThrustAsImpulse` ([FluidDynamics.cpp:804-808](../Source/GoneSurfing/FluidDynamics.cpp#L804)). No BP changes required.

### AC2 — Forward propulsion exists at zero pitch AOA + non-zero yaw slip

With `yawHydrofoilCoefficient > 0` and the board level (pitch AOA ≈ 0) but yawed across the flow, the forward component of `calcThrustForce` is non-zero. Verifiable via "Yaw Hydrofoil" debug log: during the down-the-line phase of `surfing-down-the-line` (step 4, t=2-5 s, where the pitch hydrofoil currently produces zero), the yaw hydrofoil's forward component should be in the 500-2000 N range per bottom actor.

### AC3 — Down-the-line phase no longer stalls

`surfing-down-the-line` step 4 ends with the board still moving forward (`relativeWaterVelMag` > 200 cm/s at t=5 s, vs the current ~42 cm/s). The physics target is the speed retention.

**Not blocking pure implementation.** Step 4 currently times out at 5 s with the autopilot's existing exit conditions tuned around the pre-spec trajectory. Once the yaw hydrofoil is in and producing forward thrust, the autopilot will likely need a longer step-4 duration and/or a "still moving" exit condition to capture the new, longer down-the-line glide. That tuning is part of *feature complete* but not part of *implementation complete* — land the C++ change first, observe the new trajectory, then adjust the autopilot in `Boards_on_flat_water.umap` as a follow-up commit. Re-baseline the snapshot only after the autopilot tuning settles.

### AC4 — Straight-line case stable (speed increases are a win, not a regression)

The yaw hydrofoil fires only at non-zero slip; a straight-line autopilot has small slip and so small yaw forces, so the straight-line trajectory should be qualitatively similar to pre-spec. What this AC verifies is **stability and shape**, not a specific speed number:

- Pop-up sequence still completes and lands the board in a planing state.
- Board still tracks roughly straight when commanded straight — no spinning out, no sideways launch, no oscillating yaw.
- No new instabilities at speed (porpoising, lift-off, sinking).

If `surf-straight` top speed *increases* as a side effect (the yaw hydrofoil picking up a small share of propulsion from incidental slip), **that is a positive outcome** — the current straight-line speed is below where it should be, and any boost is a feature, not a bug. Re-baseline `surf-straight` to capture the new trajectory; do not retune coefficients downward just to match the old number.

**Not blocking pure implementation.** The pop-up phase is tuned around the pre-spec force balance — adding a new propulsion source may change pop-up timing, pitch dynamics, or landing attitude enough to need retuning the pop-up autopilot steps (their thresholds, durations, or weight-shift inputs) before the board enters the planing phase cleanly. That retune is part of *feature complete* but not *implementation complete* — land the C++ change first, observe the new pop-up trajectory, then adjust autopilot/coefficients in `Boards_on_flat_water.umap` as a follow-up commit.

### AC5 — Zero force at zero slip

On a paddle-straight scenario with the board pointing directly into the flow (sinSlip ≈ 0), `yawForce` is zero. Verifiable: paddle phase (step 1) of `surfing-down-the-line` produces no "Yaw Hydrofoil FIRED" log lines (or `absSinSlip < 0.001` gate fires).

### AC6 — Existing pitch hydrofoil unchanged

At positive pitch AOA with zero slip, `calcThrustForce` produces exactly the same force as pre-spec. The yaw term is additive; the pitch term's code is not modified.

### AC7 — Performance budget

Frame time on the autopilot scenario within ±5% of pre-spec. The yaw term adds one projection, one sqrt, two dot products — all per-bottom-actor per-tick. Should be negligible.

## Open Questions / Future Work

These don't block the spec.

1. **Should sideways drag be removed entirely?** The new yaw hydrofoil's anti-slip component opposes sideways flow, just like sideways drag. If the deflection model is sufficient, the dissipative drag becomes redundant. Defer until the new model is tuned; a conservative + dissipative pair is physically correct, but the codebase might prefer one knob over two.
2. **Apply the same yaw extension to rails.** Rails currently have sideways drag ([FluidDynamics.cpp:130-189](../Source/GoneSurfing/FluidDynamics.cpp#L130)) and Bernoulli rail lift ([FluidDynamics.cpp:312-380](../Source/GoneSurfing/FluidDynamics.cpp#L312)) — both produce purely lateral forces (along `±board.left`), neither produces forward propulsion. A rail engaged on a wave face is the canonical "edged ski": the dug-in rail face is near-vertical, sideways flow hits it perpendicular-ish to the face, and the deflection produces a force with **forward + anti-slip components** — exactly the carve-propulsion mechanism. This is what makes a surfer accelerate *through* a turn rather than bleeding speed through it.

   **Why it's a separate operating regime from the bottom:**
   - The bottom yaw hydrofoil works on a roughly horizontal surface, so it captures yaw-on-flat-board (board level relative to wave, flow askew because of board heading vs wave-direction).
   - The rail yaw hydrofoil works on a near-vertical surface (when rolled), so it captures yaw-on-edged-board (board rolled, the engaged rail deflecting flow that the flat bottom wouldn't see at the same angle). Rolling the board pinches one rail deeper into the water while lifting the other out.

   On the same flow, the two can fire in different proportions depending on board roll. A flat board sees mostly bottom yaw thrust; a hard-carving board sees rail yaw thrust dominate. So the rail term is the more important one for *carving feel* even if its peak magnitude is smaller than the bottom's.

   **Engagement gating** (mirrors the existing rail sideways drag at [FluidDynamics.cpp:171-178](../Source/GoneSurfing/FluidDynamics.cpp#L171)):
   ```
   sidewaysOnLeft     = sidewaysVel · board.left
   sidewaysEngaged    = isLeftRail ? (sidewaysOnLeft > 0) : (sidewaysOnLeft < 0)
   if (!sidewaysEngaged) return 0
   ```
   Only the rail facing into the sideways flow contributes; the lifted-out rail produces zero. Same gate the existing drag uses, so the two terms fire together and stop together.

   **Code organization** is a design choice:
   - **Option A** — factor the bottom yaw-hydrofoil math into a helper (e.g. `calcYawHydrofoilForce(coefficient, engagementSign)`), call it from `calcThrustForce` for both `VE_Down` (no engagement gate, both sides contribute) and `VE_Left`/`VE_Right` (engagement-gated). Cleanest, mirrors the existing pitch hydrofoil's bottom-only scope but extended.
   - **Option B** — add a separate `railYawHydrofoilCoefficient` UPROPERTY on rail actors and inline the math in `calcThrustForce`'s rail case. Less DRY but easier to tune independently (likely the right tradeoff if rails want different coefficient curves than the bottom).

   **Coefficient relationship.** Rail surface area is much smaller than bottom area, so naïvely the coefficient should be smaller. But the rail's *effective* engagement depends on roll — at hard roll the rail face presents nearly all of its area perpendicular to flow, while the bottom's effective area shrinks (lifted off the water on the high side). So the rail coefficient might be comparable to the bottom's when accounting for roll-dependent effective area. Bisect from a starting value similar to `yawHydrofoilCoefficient` and observe.

   **Interaction with existing rail Bernoulli lift.** The Bernoulli rail lift ([FluidDynamics.cpp:312-380](../Source/GoneSurfing/FluidDynamics.cpp#L312)) is a suction force pulling the board *toward* the engaged rail — a purely lateral force from horizontal/vertical curvature flow, not from yaw misalignment. Different physics, different inputs (curvature, not slip angle). The two coexist without overlap, similar to how the pitch hydrofoil and the bottom Bernoulli suction coexist.

   **Defer until the bottom term is baselined.** Once `yawHydrofoilCoefficient` on the bottom is producing visibly correct down-the-line propulsion, the same shape should produce visibly correct carve propulsion when applied to rails — at which point the lateral turn force ([FluidDynamics.cpp:476-508](../Source/GoneSurfing/FluidDynamics.cpp#L476), currently bottom-only and currently disabled) may also become redundant. Re-evaluate the three carve-related terms (lateral turn, rail Bernoulli lift, rail yaw hydrofoil) together as a follow-up spec.
3. **Pumping coupling.** With both pitch and yaw hydrofoils active on the bottom, deliberate yaw oscillation (slalom-style) becomes a propulsion mechanism via the same logic as pitch pumping. No code needed; just an observation. Worth a qualitative gameplay test.
4. **Lateral turn force interaction.** The existing lateral turn force ([FluidDynamics.cpp:476-508](../Source/GoneSurfing/FluidDynamics.cpp#L476)) currently fires only when the board is rolled relative to the wave surface. Its purpose overlaps partially with the yaw hydrofoil's anti-slip component. After this spec, revisit whether the lateral turn term is still pulling its weight or has been subsumed.

## Status

- [ ] Spec reviewed
- [ ] Run-first sanity check: confirm the pre-spec behavior matches the table in Motivation (down-the-line stall at t≈2 s, relWaterVel collapses to ~42 cm/s by t=5 s)
- [ ] `yawHydrofoilCoefficient` UPROPERTY added to `AFluidDynamics`
- [ ] `calcThrustForce` extended with the yaw hydrofoil term per Implementation Sketch
- [ ] "Yaw Hydrofoil" debug log fires under `surf.debug.flags 'thrust'`
- [ ] `yawHydrofoilCoefficient` tuned per Coefficient Retune; forward thrust visible during step 4
- [ ] `bottomDragSidewaysCoefficient` retuned (halved or further)
- [ ] AC2 verified (forward thrust exists at zero pitch AOA + non-zero yaw slip)
- [ ] AC3 physics verified (board still moving at end of down-the-line glide; speed retention target hit)
- [ ] AC3 autopilot tuned (step-4 duration / exit conditions adjusted to capture the new longer glide — *feature-complete*, not implementation-complete)
- [ ] AC4 stability verified (surf-straight stable and qualitatively similar; speed increases are a win, not a regression)
- [ ] AC4 pop-up retuned if needed (autopilot timing / thresholds adjusted so pop-up still lands cleanly in the new physics — *feature-complete*, not implementation-complete)
- [ ] AC5 verified (zero force at zero slip)
- [ ] AC6 verified (pitch hydrofoil unchanged at zero slip)
- [ ] AC7 frame time within ±5% of pre-spec
- [ ] `surfing-down-the-line` re-baselined; baseline reviewed and approved
- [ ] `surf-straight` re-baselined if needed
- [ ] Memory: yaw hydrofoil converts sideways wave flow into forward thrust — record once stable
