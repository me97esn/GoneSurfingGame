# Spec: Bottom Hydrofoil Thrust (AOA → Upthrust + Forward, replacing wave-slope supplement)

## Overview

Replace `AFluidDynamics::calcThrustForce` and the `waveSlopeSupplementForce` branch of `calcLiftForce` with a unified hydrofoil model for the board's bottom. Upthrust direction becomes perpendicular to flow (not perpendicular to board), which automatically produces a forward component when the bottom is at positive angle-of-attack. This forward component is the physically principled replacement for the current `waveSlopeSupplementCoefficient` propulsion proxy.

Scope: `ESide::VE_Down` thrust path. Existing bottom Bernoulli "lift" (downward suction at negative AOA, [FluidDynamics.cpp:382-455](../Source/GoneSurfing/FluidDynamics.cpp#L382)) is **unchanged** by this spec — see Future Work for the eventual unification.

**Prerequisite (DONE):** [fin-carve-coupling.md](fin-carve-coupling.md) landed as commit `8e1fdf314`. The Option-A perpendicular-to-flow fin lift is in place. Do not re-implement.

## Post-implementation update (2026-05-19)

The intermediate "wave-slope gravity" force this spec introduced as a parallel mechanism to the bottom hydrofoil has now itself been retired. Empirically, with `forwardsThrustCoefficient` cranked up (decoupled from `upwardsThrustCoefficient`), the bottom hydrofoil's perpendicular-to-flow forward component is sufficient to propel the board down the wave face *and* carve instead of skid — wave-slope gravity was double-counting and contributing a sideways component (relative to board heading) that caused the skid. `waveSlopeGravityCoefficient`, the `waveSlopeGravityForce` calc/draw blocks, and the purple debug cones are all removed. `waveSlopeDownVec` is retained because [StateTriggerAutoPilot.cpp:482](../Source/GoneSurfing/StateTriggerAutoPilot.cpp#L482) still uses its magnitude as a slope-sin metric for state transitions.

The sections below are kept as historical record of how we got here.

## Post-implementation update (2026-05-24): turn-gate added to forward thrust

`waveSlopeGravityCoefficient` was un-retired (see [wave-slope-gravity-supplement.md](wave-slope-gravity-supplement.md)) and then a deeper issue surfaced: the bottom-hydrofoil forward thrust was firing on **any** positive pitch AOA — including straight-line surf — which made it the dominant cruise propulsion. Combined with planing-attenuated drag, this prevented the board from ever decelerating: `forwardsThrustCoefficient` × v² × sinAOA grows quadratically with speed and beats drag at all speeds.

Original design intent (line 163 below): the forward thrust is meant to keep the board from bleeding speed *mid-carve*. It was never meant to fire on straight-line surf. The fix gates the forward components on the **surfer's commanded weight shift** (intent signal):

```cpp
const float lateralShift = this->sharedCalculations->lateralShift; // = |amountToTheRight - 0.5|
const float intentGate   = FMath::SmoothStep(0.05f, 0.20f, lateralShift);
const float turnGate     = intentGate;
boardFwdForce = boardFwdUnit * (turnGate * forwardsThrustCoefficient      * commonMagBoardWide * boardFwdDot);
actorFwdForce = actorFwdUnit * (turnGate * actorForwardsThrustCoefficient * commonMagPerActor  * actorFwdDot);
```

- Centered weight (lateralShift = 0): `turnGate = 0` → no forward thrust. Wave-catch (player keeps weight centered) and straight cruise both stay closed.
- Mild lean (lateralShift = 0.10, ~20% input deflection): `turnGate ≈ 0.26` → partial thrust.
- Hard carve (lateralShift ≥ 0.20, ~40% input deflection): `turnGate = 1.0` → full forward thrust.

`AWeightDistribution::amountToTheRight` is driven by `ASurfboardPawn` from player input and (via Blueprint) by some autopilot steps — see [[project-autopilot-drives-weight-in-bp]]. The pointer to AWeightDistribution is wired into both ASharedCalculations actors by `ASurfboardUtils::BeginPlay`, alongside the existing `otherHalfSharedCalculations` wiring.

The upward (anti-sink) component is **left ungated** — only the forward components are turn-restricted. This preserves the bottom hydrofoil's planing-lift role.

**Why intent alone, not also slip:** an earlier attempt (2026-05-24) used `turnGate = slipGate × intentGate` with `slipGate = smoothstep(0.05, 0.20, |sinSlip|)`. The two signals don't co-occur in time — slip is the lag in response, intent is the command. Empirically only 0.4% of ticks had both signals open simultaneously, killing the gate. Intent alone is the correct signal: it's what the surfer directly commands at the moment they want forward thrust.

### Gate design history

- (0.05, 0.20) on **|sinSlip|** alone — opened during wave-catch (slip ≈ 0.10–0.15 from lateral wave flow even without yaw input); at high `actorForwardsThrustCoefficient` the rocker-tilt leaked ~17% into world Z, breaking wave-catching.
- (0.20, 0.35) on **|sinSlip|** alone — tightened version; empirically too tight, `forwardsThrustCoefficient = 1e6` had no effect during carving since carve slip also stays < 0.20.
- (0.05, 0.20) on **slip × intent (AND)** — the two signals only co-occurred 0.4% of ticks, gate almost always closed.
- (0.05, 0.20) on **intent alone** — current. Closed during centered cruise/wave-catch, opens on commanded lean.

### Known interaction: hydrofoil force cap

`maxHydrofoilForceAmount` (default 5000 N/actor, see [hydrofoil-force-cap.md](hydrofoil-force-cap.md)) clamps the combined `pitchForce` after the gates apply. If you raise `forwardsThrustCoefficient` enough that the raw `pitchForce.Size()` exceeds the cap, further coefficient increases have no observable effect — the cap binds. To probe coefficient sensitivity, either lower the coefficient until the cap doesn't bind, or set `maxHydrofoilForceAmount` to 0 (disables the cap entirely per the `if (maxHydrofoilForceAmount > 0.0f)` guard).

Combined with the linear-drag-attenuation change (see [planing-drag-attenuation.md](planing-drag-attenuation.md)) and a 10× bump on `bottomDragCoefficient`/`railDragCoefficient`, the cruise behavior is now: burst → rapid deceleration → stable cruise equilibrium → slow further decay. No more indefinite glide.

## Current Landscape (read this before anything else)

Since this spec was first drafted, two things shifted the problem materially:

1. **Fin spec (`8e1fdf314`) landed but didn't satisfy its own AC2.** The fin lift is now correctly perpendicular-to-flow, but peak skid in `surfing-down-the-line` at t=6.5–7.5s only dropped from 8.0° to 8.0° (within noise). The principled physics is in place; the *visible* carve improvement came from elsewhere.

2. **`VelocityScale` reduced 1000 → 800 (commit `907427186`).** During the post-fin-spec investigation we discovered that the dominant skid driver was *bottom-actor sideways drag yaw torque*, not the fin direction. Reducing `VelocityScale` shrinks every relative-water-velocity-squared force quadratically, including the yaw-torque-creating bottom sideways drag, and that drop produced the actual skid improvement. Current state, post `907427186`:

   | Metric | Pre-fin-spec | Post-`907427186` |
   |---|---|---|
   | Peak skid t=6.5–7.5s | 8.0° | 5.8° |
   | Peak skid t=7.8–8.5s | ~20° | 10.5° |
   | Max speed step 3 | 630 | 662 |
   | End yaw step 3 | −148° | −138° |

So **the urgency of this spec has decreased**. The skid that motivated it is half what it was. The remaining motivations are still valid:
- Make propulsion couple to actual AOA instead of wave-slope proxy
- Retire `waveSlopeSupplementCoefficient`
- Enable pumping as a natural propulsion mechanism

But a fresh implementer should know: **this spec is now a principled-physics cleanup, not a skid emergency**. Less invasive alternatives (e.g., further tuning `bottomDragSidewaysCoefficient`, which was demonstrated to be a real lever in the same investigation) should be considered before committing to this rewrite.

## Background

### Terminology note

In this codebase, **"lift" means Bernoulli suction** — on the bottom it points *downward* (the existing `bottomLiftForce` path). That naming is preserved throughout this spec; it never refers to an upward force.

The force this spec adds — the perpendicular-to-flow reaction on a hydrofoil — is called **"thrust"** everywhere in this document, matching the existing `calcThrustForce` naming convention. Its components are **"upthrust"** (along `board.up`) and **"forward thrust"** (along `board.forwards`).

### Physics

A surfboard bottom is a hydrofoil with chord = `board.forwards` and spanwise = `board.left`. At positive pitch angle of attack (nose up relative to flow), water gets deflected downward and the reaction force on the bottom acts perpendicular to the flow. Call this reaction force `T` (for thrust). In board frame:

- **upthrust component** = T·cos(α) — the existing `upwardsThrustCoefficient` path models this as a force along `board.up`. But applying it along `board.up` is geometrically incorrect: when the board is nose-up at pitch θ in world frame, `board.up` tilts backward by θ in world frame, so this term currently has a tiny backward bias. The correct thrust direction is perpendicular to flow.
- **forward thrust component** = T·sin(α) — currently absent in `calcThrustForce`. The codebase patches this gap by adding `waveSlopeSupplementForce`, a separate term that pushes along `board.forwards` proportional to `sin(slope)` × planing × underwater. The supplement works as propulsion-on-wave but is decoupled from the board's actual angle: a steep wave with the board momentarily flat still produces forward push.

Unifying both into a single **thrust perpendicular to flow on the bottom** yields the same upward force, plus an automatic forward component when AOA is positive, and zero when the board is aligned to the flow (water just glides past, no force) — which is the physically correct behavior.

This unification has a side effect we want: it makes propulsion couple to the board's *actual* angle-of-attack rather than to wave geometry. Pumping (deliberately changing pitch) becomes a propulsion mechanism without extra code. Floating across flat water with the board angled (e.g., wake from a passing wave) no longer produces phantom forward force from wave slope.

## Required Reading

A fresh implementer should skim these.

### Coordinate convention

`board.forwards = local +Y`. `board.up`, `board.left` follow. Don't hardcode axes.

### Existing code touchpoints

- [FluidDynamics.cpp:716-765](../Source/GoneSurfing/FluidDynamics.cpp#L716) — `calcThrustForce` in full. Current shape: `sinPitch × v² × upCoef × amountUnderWater × effectiveWaterHeight`, direction `board.up`, gated `sinPitch ≥ 0.001`. This is the function being rewritten.
- [FluidDynamics.cpp:495-519](../Source/GoneSurfing/FluidDynamics.cpp#L495) — `waveSlopeSupplementForce` branch in `calcLiftForce`. The block this spec retires (set coefficient to 0 by default after spec lands; remove the code in a follow-up once the new model is baselined).
- [FluidDynamics.cpp:382-455](../Source/GoneSurfing/FluidDynamics.cpp#L382) — `calcLiftForce` head, including `bottomLiftForce` (Bernoulli suction at negative AOA, force along `−board.up`). **Not touched by this spec**, but be aware: at negative AOA the bottom currently produces downward suction via this path; at positive AOA the bottom produces upward thrust via `calcThrustForce`. After this spec, only positive AOA changes.
- [FluidDynamics.h:96-103](../Source/GoneSurfing/FluidDynamics.h#L96) — `waveSlopeSupplementCoefficient` UPROPERTY. **Strip the `UPROPERTY` decoration entirely and set the value in `BeginPlay`** (see the precedent in commit `907427186` for `AWaveHeight::VelocityScale`). Leaving it editor-tunable while code overwrites it gives the misleading impression it's adjustable.
- [SharedCalculations.h](../Source/GoneSurfing/SharedCalculations.h) — `relativeWaterVelocity`, `amountUnderWater`, `AmountPlaning`, `forwards`, `left`, `up`. Also `effectiveWaterHeight` on the FluidDynamics actor itself ([FluidDynamics.h:166-170](../Source/GoneSurfing/FluidDynamics.h#L166)) — the water-mass proxy.
- Commit `8e1fdf314` — the fin spec implementation; shows the perpendicular-to-flow construction pattern (anti-slip unit vector rotated toward forwards). Mirror its shape for bottom thrust, but in the forwards-up plane instead of horizontal.
- Commit `907427186` — `VelocityScale` 1000 → 800. Sets the current baseline trajectory all numbers in this spec are anchored to.

### Why the existing `board.up` direction has a backward bias

For a nose-up board (pitch θ > 0) moving forward, `board.up` in world frame ≈ `(−sin θ, 0, cos θ)`. A force `F × board.up` decomposes in world frame as `−F sin θ` along world +X and `F cos θ` along world +Z. The X component is *backward* if "forward" was world +X. Geometric perpendicular-to-flow would give `(0, 0, F)`, no backward bias; project that onto `board.forwards = (cos θ, 0, sin θ)` and you get `F sin θ` *forward*. So the existing implementation not only misses forward thrust — it has a small backward bias from the rotation alone.

### Build, run, snapshot

Same as [fin-carve-coupling.md](fin-carve-coupling.md). After this spec, expect coefficient retune; the `surfing-down-the-line` baseline will need re-approval.

### Run-first sanity check (do this before editing any code)

Confirm the current baseline matches the numbers in "Current Landscape" above before changing anything. The baseline has shifted twice since this spec was first drafted (fin spec `8e1fdf314`, then VelocityScale `907427186`), and the snapshot baseline file `Tests/baselines/surfing-down-the-line.csv` may or may not reflect the current state — check the file's commit history.

```powershell
Stop-Process -Name UnrealEditor,LiveCodingConsole,zenserver,TraceServer,UnrealTraceServer -Force -ErrorAction SilentlyContinue
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "E:\windowsgrejor\git\GoneSurfingUE5\RunGameAndCollectLogs.ps1" -TimeoutSeconds 300
```

Expected step-3 trajectory characteristics with code as of `907427186`:

- Max speed ≈ 660 cm/s
- End yaw ≈ −138°
- Peak skid t=6.5–7.5s ≈ 6°, t=7.8–8.5s ≈ 10°
- Planing peak ≈ 0.91

If your run diverges materially, *stop and figure out why* before implementing this spec. The numbers in Coefficient Retune below assume the run-first baseline matches.

## Design

### Force shape

Replace `calcThrustForce` body with:

```
1. Compute flow direction in board's forward-up plane:
     relVel               = sharedCalculations->relativeWaterVelocity
     relVelInBoardPlane   = relVel - (relVel · this->left) × this->left
     v²InPlane            = relVelInBoardPlane.SizeSquared()
     if v²InPlane < ε: return 0

2. Decompose into AOA:
     flowDir = relVelInBoardPlane / sqrt(v²InPlane)
     sinAOA  = flowDir · this->up        // signed; > 0 when flow rises into the bottom

3. Gate on positive AOA:
     if sinAOA < 0.001: return 0          // negative AOA → bottom Bernoulli handles it (separate path)

4. Thrust direction (perpendicular to flow, in the board's forward-up plane):
     thrustDir = cross(this->left, flowDir)
     // sign correction: ensure thrustDir · this->up > 0 (thrust points up-ish, not down-ish)
     if (thrustDir · this->up) < 0: thrustDir = -thrustDir

5. Magnitude (matches existing `calcThrustForce` shape, just with v²InPlane and explicit sinAOA):
     amountUnderWater = sharedCalculations->amountUnderWater
     thrustMag = upwardsThrustCoefficient
               × v²InPlane
               × sinAOA
               × amountUnderWater
               × effectiveWaterHeight

6. Return:
     return thrustDir × thrustMag
```

In board frame, the resulting force decomposes as:

- `board.up`        component: `thrustMag × cosAOA`  — upthrust (was the entire force before)
- `board.forwards`  component: `thrustMag × sinAOA`  — forward thrust (new)

### Split coefficient — `forwardsThrustCoefficient` (shipped, Future Work #3 graduated)

The unified single-coefficient design above is *not* what shipped. Instead the up and forward components are scaled by independent coefficients:

```
commonMag = v²InPlane × sinAOA × amountUnderWater × effectiveWaterHeight
upDot     = thrustDir · upUnit      // ≈ cosAOA
fwdDot    = thrustDir · fwdUnit     // ≈ sinAOA
upForce   = upUnit  × (upwardsThrustCoefficient   × commonMag × upDot)
fwdForce  = fwdUnit × (forwardsThrustCoefficient  × commonMag × fwdDot)
force     = upForce + fwdForce
```

At `forwardsThrustCoefficient == upwardsThrustCoefficient` this reduces to the unified force above (since `thrustDir = upDot × upUnit + fwdDot × fwdUnit` for any vector in the (up, forwards) plane). Cranking `forwardsThrustCoefficient` higher gives more forward push per unit AOA without raising upthrust.

**Why the split was needed**, despite the spec's "don't pre-add" guidance:

- The forward component of the unified force scales as `sinAOA²` (since `thrustMag` already contains `sinAOA` and the forward decomposition multiplies by another `sinAOA`). At typical AOA ~0.1 that's ~1% of total — vanishing compared to upthrust.
- On a wave face the supplement's propulsion role was initially handed off to a separate gravity-along-slope mechanism (`waveSlopeGravityForce` in `calcLiftForce`). **That mechanism has since been retired** (see the post-implementation update at the top of this file): with `forwardsThrustCoefficient` decoupled and cranked up, the bottom hydrofoil's forward component handles wave-face propulsion on its own, without the sideways-relative-to-heading skid that gravity-along-slope was producing.
- On **flat water during a sharp turn** the forward thrust matters for the same reason: it keeps the board from bleeding speed mid-carve. The decoupled `forwardsThrustCoefficient` (raised above `upwardsThrustCoefficient`) is what makes that work.

The naming differs from the Future Work item's anticipated `bottomHydrofoilCoefficient`: keeping the `upwardsThrustCoefficient` / `forwardsThrustCoefficient` pair reads more obviously than a single coefficient + a "split-amount" parameter, and matches the existing `upwards/along` naming of the function signature.

### Why this magnitude formula

Identical structure to the existing `calcThrustForce` line 731:

```cpp
upThrustAmount = sinPitch × v² × upCoef × amountUnderWater × effectiveWaterHeight
```

— except `v²` is now `v²InPlane` (only the in-plane component contributes to AOA-induced thrust; the sideways component is the fin's job), and `sinPitch` is replaced by `sinAOA` computed from the actual flow direction, not from the board's static orientation. On flat water with a pitched board, these are numerically similar; on a wave face with rising water, `sinAOA` correctly captures the additional AOA from the water rising into the bottom, which `sinPitch` alone misses.

### Why the projection onto `this->left`

The bottom is a hydrofoil in the *pitch* plane (forward × up). Sideways flow shouldn't contribute to pitch-AOA thrust — that's the fin's job, handled by [fin-carve-coupling.md](fin-carve-coupling.md) in the perpendicular plane. Projecting out the sideways component cleanly separates the two hydrofoils.

### Why drop `waveSlopeSupplementForce`

After this spec, the bottom hydrofoil automatically produces forward force when there's positive AOA. On a wave face, the water has an upward component relative to the (forward-moving) board because of orbital motion — that's exactly what gives the bottom positive AOA, which gives forward thrust. The supplement was a fudge for this physics; the fudge becomes redundant.

Procedure: set `waveSlopeSupplementCoefficient = 0` in BP/umap during this spec. Don't delete the code path in the same change — leave it as a quick rollback escape if coefficient retune turns out to be harder than expected. Remove the code in a follow-up once the new model is baselined.

### Negative AOA

When `sinAOA < 0`, the bottom is angled such that water flows underneath from the top side (board nose-down relative to flow). The existing bottom Bernoulli "lift" path ([FluidDynamics.cpp:382-455](../Source/GoneSurfing/FluidDynamics.cpp#L382)) already models the suction force in this regime, with direction `−board.up`. Don't touch it in this spec. Future Work item describes the eventual unification.

### When water is moving with the board (zero relative velocity)

`v²InPlane → 0`, force → 0. Correct: a perfectly wave-locked board has no slip past the bottom, no hydrofoil action.

### When the board is upside down or otherwise extreme

The sign correction `if (thrustDir · this->up) < 0: thrustDir = -thrustDir` handles weird orientations gracefully. At ±90° pitch the formula degrades to zero magnitude (`sinAOA → ±1` but `flowDir` is no longer in the forward-up plane after projection — `v²InPlane → 0`), which is fine.

## Implementation Sketch

### Replace calcThrustForce body

Drop-in replacement for the function body at [FluidDynamics.cpp:716-765](../Source/GoneSurfing/FluidDynamics.cpp#L716):

```cpp
FVector AFluidDynamics::calcThrustForce(float alongThrustCoefficient, float upwardsThrustCoefficient, FColor debugColor = FColor::Red)
{
    if (this->side != ESide::VE_Down || upwardsThrustCoefficient <= 0.0f)
    {
        return FVector::ZeroVector;
    }

    // Project relative water velocity onto the board's forward-up plane (the pitch plane).
    // Lateral flow is the fin's job (see fin-carve-coupling.md), not the bottom's.
    const FVector relVel = this->sharedCalculations->relativeWaterVelocity;
    const FVector relVelInPlane = relVel - FVector::DotProduct(relVel, this->left) * this->left;
    const float v2InPlane = relVelInPlane.SizeSquared();
    if (v2InPlane < 1.0f)
    {
        return FVector::ZeroVector;
    }

    const FVector flowDir = relVelInPlane / FMath::Sqrt(v2InPlane);
    const float sinAOA = FVector::DotProduct(flowDir, this->up);
    if (sinAOA < 0.001f)
    {
        return FVector::ZeroVector;  // negative AOA → bottom Bernoulli handles it
    }

    // Thrust perpendicular to flow, in the pitch plane.
    FVector thrustDir = FVector::CrossProduct(this->left, flowDir);
    if (FVector::DotProduct(thrustDir, this->up) < 0.0f)
    {
        thrustDir = -thrustDir;  // sign correction for unusual orientations
    }

    const float amountUnderWater = this->sharedCalculations->amountUnderWater;
    const float thrustMag = upwardsThrustCoefficient
                          * v2InPlane
                          * sinAOA
                          * amountUnderWater
                          * this->effectiveWaterHeight;

    const FVector hydrofoilForce = thrustDir * thrustMag;

    if ((this->debugThrust || SurfDebug::ShouldDebug(this, TEXT("thrust"))) && shouldDebugLog())
    {
        // Decompose for debug clarity: how much is upthrust vs forward thrust?
        const float fwdComponent = FVector::DotProduct(hydrofoilForce, this->sharedCalculations->forwards);
        const float upComponent  = FVector::DotProduct(hydrofoilForce, this->up);
#if WITH_EDITOR
        const FString LabelForLog = GetActorLabel();
#else
        const FString LabelForLog = TEXT("<no-label>");
#endif
        UE_LOG(LogTemp, Warning, TEXT("Bottom Hydrofoil [name=%s label=%s] - sinAOA: %.3f, v²InPlane: %.1f, upCoef: %.3f, amountUnderWater: %.3f, effectiveH: %.2f -> thrustMag: %.2f, force: (%.1f, %.1f, %.1f) [fwd=%.1f up=%.1f]"),
            *GetName(), *LabelForLog, sinAOA, v2InPlane, upwardsThrustCoefficient, amountUnderWater, this->effectiveWaterHeight,
            thrustMag,
            hydrofoilForce.X, hydrofoilForce.Y, hydrofoilForce.Z,
            fwdComponent, upComponent);
    }

    if ((this->debugDrawForces || SurfDebug::ShouldDebug(this, TEXT("forces"))) && !hydrofoilForce.IsNearlyZero())
    {
        DrawDebugCone(
            GetWorld(),
            GetActorLocation(),
            hydrofoilForce,
            hydrofoilForce.Size() * debugForceDrawScale,
            FMath::DegreesToRadians(1),
            FMath::DegreesToRadians(1),
            50,
            debugColor,
            false,
            0.1f);
    }

    return hydrofoilForce;
}
```

The `alongThrustCoefficient` parameter is now unused but stays in the signature for BP compatibility (same as the comment at [FluidDynamics.h:221-224](../Source/GoneSurfing/FluidDynamics.h#L221) already notes).

### Disable the wave-slope supplement

**Strip the `UPROPERTY` decoration** rather than leaving it editor-tunable while code overrides it (see precedent in commit `907427186` for `AWaveHeight::VelocityScale`). Keep the field as a plain class member, default-initialized to `0.0f`, with a comment explaining why:

```cpp
/** Coefficient for the wave-slope gravity supplement.
 *
 *  Not a UPROPERTY: deprecated and retained at 0. Forward propulsion now comes from
 *  the bottom hydrofoil in calcThrustForce (see specs/bottom-hydrofoil-thrust.md);
 *  the supplement code path is retained as a rollback escape during coefficient
 *  retune but produces no force at the default value of 0. To be removed once the
 *  new model is baselined.
 */
float waveSlopeSupplementCoefficient = 0.0f;
```

The umap value (currently set in `Boards_on_flat_water.umap`) becomes irrelevant once the UPROPERTY is gone; the default-initialized 0 takes over.

Do NOT remove the `waveSlopeSupplementForce` branch in `calcLiftForce` yet — at coefficient=0 it's a no-op, but keeping the code means a one-line revert is enough if the new model needs to be backed out during tuning.

## Coefficient Retune

The new `calcThrustForce` produces both upward and forward force from a single `upwardsThrustCoefficient`. The two components are coupled (`cosAOA` and `sinAOA` of the same thrust vector). Tuning is therefore tighter than before — you can't independently dial "upthrust" and "forward thrust."

### Order of magnitude (numbers updated for `907427186` baseline)

At VelocityScale=800, typical down-the-line moment post-pop-up:

- `v²InPlane` ≈ 200 000–350 000 (cm²/s²) — board speed in step 3 peaks at 662 cm/s, relative water speed in the bottom-plane projection sits a bit lower
- `sinAOA` ≈ 0.1–0.3 (board pitch settles around −15° during the surf, plus wave-induced vertical flow)
- `amountUnderWater` ≈ 0.3
- `effectiveWaterHeight` ≈ 50

Magnitude per unit `upwardsThrustCoefficient` ≈ 250 000 × 0.2 × 0.3 × 50 = ~750 000. Multiply by your starting coefficient and observe whether the new force matches the order of the wave-slope supplement it replaces (the supplement was ~7000 N-equivalent per actor at peak in the pre-`907427186` runs; at VS=800 it's smaller because both `v²` and `slopeSin` × planing are now smaller — sample the actual peak from a debug-log run before retuning).

Start by halving `upwardsThrustCoefficient` (since it now does double duty: up + forward) and iterate.

### Procedure

1. Land code change. **Strip the `UPROPERTY` from `waveSlopeSupplementCoefficient` in [FluidDynamics.h:96-103](../Source/GoneSurfing/FluidDynamics.h#L96) and set the field to `0.0f` directly** (or set it to 0 in `AFluidDynamics::BeginPlay` for clarity). The existing code path stays as rollback escape but produces no force. Leave `upwardsThrustCoefficient` at its current umap value.
2. Run autopilot. Observe peak forward force from "Bottom Hydrofoil" debug log lines.
3. Adjust `upwardsThrustCoefficient` so down-the-line top speed in `surfing-down-the-line` is within ~10% of the `907427186` baseline (662 cm/s peak).
4. Verify the upthrust component is also reasonable (board doesn't sink or porpoise). If upthrust is too high but forward is right (or vice versa), you've hit the coupled-tuning constraint — accept the tradeoff or raise this as a Future Work item to split the coefficient.
5. Eyeball-tune until `surfing-down-the-line` first 8.5 s matches the spirit of `907427186`: planing engages cleanly at pop-up, board reaches ~660 cm/s during step 3, end yaw lands near −138°, peak skid in t=7.8–8.5s ≤ 10°. The exact trajectory will differ — baseline must be re-approved.

## Acceptance Criteria

### AC1 — Compiles, calls back into the same applier

`calcThrustForce` still has the same signature and is still called from `applyThrustAsImpulse`. No BP changes required.

### AC2 — Forward propulsion exists at positive AOA without the supplement

With `waveSlopeSupplementCoefficient = 0` and the new `calcThrustForce`, the board propels forward on a wave face. `surfing-down-the-line` reaches at least 400 cm/s during step 3 (pop-up + down-the-line). If it doesn't, the coefficient is too low or the bottom AOA isn't being seen.

### AC3 — Zero force at zero AOA

On flat water with the board level (pitch ≈ 0, no wave-induced flow), thrust = 0. Verifiable: paddle slowly phase (step 1) should produce ~zero thrust impulses ("Bottom Hydrofoil FIRED" lines absent or vanishing in the first ~5 s).

### AC4 — Downward suction unchanged

The existing bottom Bernoulli "lift" (downward force at negative AOA) is untouched. At negative pitch (nose-dive scenario from [nose-dive-bug.md](nose-dive-bug.md)), the existing `bottomLiftForce` path fires as before. New `calcThrustForce` returns zero in that regime.

### AC5 — Pumping works (qualitative)

A board on flat water with periodic pitch oscillation (manually scripted in a one-off test or observed in-editor by tilting) gains forward speed. Doesn't need to be a fast pump — just demonstrably non-zero average forward force from non-zero AOA. This is a qualitative gate; no snapshot test required.

### AC6 — surfing-down-the-line behavior matches spirit

After retune, the autopilot completes all steps (step 0→1→2→3→4), with the board planing, reaching a sensible top speed in the 400–900 cm/s range, and not crashing into a wall of upthrust at pop-up. New baseline approved via `Tests\Approve.ps1 -TestName surfing-down-the-line`. Exact trajectory will differ from pre-spec — that's expected.

### AC7 — Performance budget

Frame time on the autopilot scenario within ±5% of pre-spec. The new formula adds a projection, a sqrt, two dot products, and a cross — all per-bottom-actor per-tick. Should be negligible.

## Open Questions / Future Work

These don't block the spec.

1. **Unify with the existing `bottomLiftForce` (Bernoulli suction).** Endpoint: replace both the existing `bottomLiftForce` (downward Bernoulli suction at negative AOA, force `× −board.up`) and the new `calcThrustForce` (upward+forward thrust at positive AOA) with a single force `dir × C × v²InPlane × sinAOA_signed`. At positive AOA the resulting force is up+forward (thrust); at negative AOA it's down+backward (suction — what the codebase calls "lift"). The two regimes are the same physics, just different sign of AOA. Defer until this spec is baselined and the codebase has one less proxy. Naming-wise, the unified function should be named for the physics not the sign, e.g., `calcBottomHydrofoilForce` — neither "lift" nor "thrust" describes both regimes accurately.

2. **Remove `waveSlopeSupplementCoefficient` entirely.** Once the new model is baselined and stable, delete the field and its code path. Tracked here so we don't forget.

3. ~~**Coefficient split if coupled tuning fights us.**~~ **Shipped.** Split into `upwardsThrustCoefficient` and `forwardsThrustCoefficient` after coupled tuning made the forward share unreachable (`sinAOA²` ≈ 1% of total). See "Split coefficient" subsection under Design.

4. **Sideways thrust on rolled board.** When the board is rolled, the bottom's effective thrust direction also has a sideways component (toward the high rail). This spec ignores that — it projects out the sideways flow component. If we observe that rolling-only produces forward thrust but no realistic side-push, revisit the projection.

5. **Mobile perf check.** The cross + dot + projection are cheap but not free. Profile on the target mobile device before shipping to the mobile branch.

## Status

- [ ] Spec reviewed
- [x] Sibling spec [fin-carve-coupling.md](fin-carve-coupling.md) landed (commit `8e1fdf314`)
- [ ] Considered less-invasive alternative (`bottomDragSidewaysCoefficient` sweep) per Open Questions item 0
- [ ] Run-first sanity check matches expected `907427186` baseline (peak speed ≈ 660, end yaw ≈ −138°, peak skid late ≈ 10°)
- [ ] `calcThrustForce` body replaced per Implementation Sketch
- [ ] `waveSlopeSupplementCoefficient` UPROPERTY decoration removed; field defaulted to 0
- [ ] Debug log "Bottom Hydrofoil" line printed and decomposed into fwd/up components
- [ ] `upwardsThrustCoefficient` retuned so AC2 (forward propulsion exists) and AC6 (sensible top speed) both hold
- [ ] AC3 verified (zero force at zero AOA in step 1)
- [ ] AC4 verified (negative-AOA suction path unaffected)
- [ ] AC5 qualitative pump test (informal)
- [ ] AC7 frame time within ±5% of pre-spec
- [ ] `surfing-down-the-line` re-baselined; baseline reviewed and approved
- [ ] Memory: hydrofoil-as-propulsion replaces wave-slope supplement — record once stable
