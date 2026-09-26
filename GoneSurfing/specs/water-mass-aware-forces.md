# Spec: Water-Mass-Aware Surface Forces

## Overview

Rewrite every per-surface fluid force (drag, lift, thrust) on the board to scale with the **water column height above the surface** at the surface's location. Replaces the current `coef × v²` family of formulas with `coef × waterColumn × v²`, making the model dimensionally honest: force ∝ ρ_local × v² where the column height is the proxy for `ρ_local`.

Affected: bottom, rails (left/right), tail, nose, fins × drag, lift, thrust.

Out of scope: buoyancy (unchanged).

## Required Reading and Operational Context

A fresh implementer should skim these before editing code.

### Coordinate convention

**`board.forwards = local +Y`** (the surfboard mesh is rotated 90° from the UE default). `board.left = local -X`, `board.up = local +Z`. Many gating signs in the existing formulas depend on this — assuming `forwards = +X` will silently break left/right/forward/backward logic. The mesh-rotation reason is structural; don't try to "fix" it by hardcoding board axes.

### Required reading (existing code)

Read these before editing — the spec assumes their current shape:

- [FluidDynamics.cpp:61-307](../Source/GoneSurfing/FluidDynamics.cpp#L61) — `calcDragForce` with all six `ESide` cases. The rail case (`VE_Left`/`VE_Right`) at lines 132-194 is the recent rewrite that introduced the forwards/sideways split and the directional engagement gates; this spec keeps that structure and only adds the column factor (and reverts sideways to relative velocity).
- [FluidDynamics.cpp:309-455](../Source/GoneSurfing/FluidDynamics.cpp#L309) — `calcRailLiftForceAmount` and `calcLiftForce`. The `amountUnderwater` factor at lines ~336 and ~358 is what the spec replaces with `waterColumn`.
- [FluidDynamics.cpp around line 504](../Source/GoneSurfing/FluidDynamics.cpp#L504) — `calcThrustForce` (`alongThrustCoefficient` × `(1 - cosPitchAngleOfAttack)` × relWaterVelMag + upwards term).
- [SharedCalculations.h](../Source/GoneSurfing/SharedCalculations.h) — board-level fields shared across this board's FluidDynamics actors. Specifically: `AmountPlaning`, `amountUnderwater`, `relativeWaterVelocity` (and `…Magnitude` / `…Normalized`), `absoluteWaterVelocity`, `cosYawAngleOfAttackLeft`, `pitchSinAngleOfAttack`, `forwards`, `left`, `up`. Two SC actors per board (front + back) exist purely for perf — they're not per-surface.
- [WaveHeight.h:299, 304-308, 325-328](../Source/GoneSurfing/WaveHeight.h#L299) — `waveHeightAndNormal(location, frame)` (internal) and `calculateWaveLocationAndNormalAuto(location)` (UFUNCTION). **Always call the `…Auto` variant from FluidDynamics.** The sample frame is *not* a simple read from `WaterController` — `AWaveHeight` internally computes a tile-adjusted frame (current frame + per-tile offset) as part of the infinite-tiling system. Passing the `WaterController` frame directly would silently break tiling alignment. Returns `TArray<FVector>` where element 0 is the surface location and element 1 is the normal.
- [specs/debug-logging.md](debug-logging.md) — runtime debug flag system. Spec relies on adding `waterColumn:` to existing debug log lines; that system is how you'd verify it.

### Build, run, snapshot, approve

```powershell
# Kill stale UE processes first — auto-respawn can shadow the launch silently
Stop-Process -Name UnrealEditor,LiveCodingConsole,zenserver,TraceServer,UnrealTraceServer -Force -ErrorAction SilentlyContinue

# Build + run in -game mode (autopilot runs once, then quits)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "E:\windowsgrejor\git\GoneSurfingUE5\RunGameAndCollectLogs.ps1" -TimeoutSeconds 240

# Same, with debug logs on a subset of actors (flags:actorLabelSubstring):
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "E:\windowsgrejor\git\GoneSurfingUE5\RunGameAndCollectLogs.ps1" -Argument "drag,lift,thrust:middle" -TimeoutSeconds 240
```

Outputs:

- Log: `Saved/Logs/GoneSurfing.log` (rotated to `.prev` each run).
- Snapshot CSVs: `Saved/Tests/latest/<TestName>.csv`.
- Snapshot baselines (committed): `Tests/baselines/<TestName>.csv`.

After each run, the wrapper auto-runs `Tests/Compare.ps1` for every `latest/*.csv`. To approve a latest run as the new baseline:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "Tests\Approve.ps1" -TestName surf-straight
```

Tests of interest:

- **`surf-straight`** — short paddle-and-surf-straight scenario. **Currently not behaving as expected pre-spec; expect it to continue not passing after this rewrite too.** Do not treat its diff as a tuning signal; do not gate the spec on it.
- **`surfing-down-the-line`** — longer down-the-line run. **First ~8.5 s is a valid pre-spec baseline** (matches expected behavior up through pop-up). **After 8.5 s, divergence is expected** — the deferred "wave glides through board" bug fires at gs≈8.7 and the post-bug trajectory is not a meaningful reference. Tune coefficients against the first 8.5 s only.

Snapshot CSV columns (post-spec, after gameSeconds was added): `t,gameSeconds,frame,x,y,z,vx,vy,vz,roll,pitch,yaw,step,slopeSin,planing,underwater`. The `gameSeconds` column anchors to `WaterController.SecondsElapsed` so log lines and CSV rows share the same time coordinate.

### Pre-spec state of the absolute-velocity sideways drags

This spec reverts two prior changes:

- **Rail sideways drag** uses absolute velocity as of commit `466fefcaa`. Revert to relative velocity per this spec, but **keep the forwards/sideways split and directional engagement gating that `466fefcaa` introduced** — only the velocity reference flips.
- **Bottom sideways drag** uses absolute velocity (see existing comment block at [FluidDynamics.cpp:107-113](../Source/GoneSurfing/FluidDynamics.cpp#L107) explaining why). Revert to relative.

The historical rationale that justified switching to absolute (trough drift / over-engagement when board overtakes water) becomes obsolete once the water-column factor lands: trough drift is *exactly* what the column factor prevents (column ≈ 0 in trough → no force regardless of velocity reference).

### Coefficient context

`bottomDragSidewaysCoefficient` defaults to `0.001` in [FluidDynamics.h](../Source/GoneSurfing/FluidDynamics.h) but is set to `0.01` in the level. `railDragSidewaysCoefficient` defaults to `0.01` (commit `466fefcaa` bumped from 0.001 to match the level's bottom setting). After this spec lands, every coefficient that gates a per-surface force will need retune by roughly the typical column-height factor (tens of cm) — defaults and level values both wrong.

### Wave sampling — known unknowns

`calculateWaveLocationAndNormalAuto` resolves the sample frame internally via the infinite-tiling system (current `WaterController` frame + per-tile offset). Two consequences worth knowing about when debugging:

- **Frame-wrap discontinuity.** The wave frame loops over a ~190-frame range and wraps abruptly (e.g., `1078 → 886`). If sampler outputs are discontinuous across the wrap, derived quantities (`waterColumn`, velocity) will be too. The deferred gs=8.70 "wave glides through board" bug lives in this window.
- **Tile-seam discontinuity.** Adjacent tiles can be on different frames (per-tile offsets), so a board straddling a tile seam may see a `waterColumn` jump along the seam. Not blocking for this spec; flag if it manifests visibly.

## Background

The current per-surface force formulas are `coef × v²` — implicitly assuming water density is uniform across the wave system. It isn't. A breaking wave wall has a thick column of moving water; a trough has a thin sheet of moving water. The current model treats them as equivalent when their surface velocities match.

A recent attempt to address this (commit `466fefcaa` for rails, the bottom sideways term before it) switched the sideways drag from relative to absolute water velocity. That partly worked but introduced a new failure: absolute velocity is non-zero **everywhere** in the wave field — including the trough — so it produces lateral drift the board shouldn't experience. There is no coefficient value that makes the absolute-velocity model both engage on the wave wall AND stay quiet in the trough.

The correct discriminator is the **amount of water actually present** at the surface, not the velocity. A surface buried under a wall of water has lots of mass behind the flow; a surface skimming the top of a calm trough has ~none. Water-column-above-the-surface naturally captures this.

With water mass as a multiplier, **the velocity reference reverts to relative water velocity** everywhere. The cases the absolute-velocity switch was trying to fix (wave overtaking a wave-aligned board) are now handled by the column factor: in the wave wall the column is large, so even small relative velocity produces meaningful force.

## Design Model

### Per-surface scalar

For each `AFluidDynamics` actor each tick:

```
waterSurfaceZ  = waveHeightSampler->surfaceZAt(actorLocation)
waterColumn    = max(0, waterSurfaceZ - actorLocation.Z)
```

`waterColumn` is in cm (UE world units). Zero when the surface point is at or above the local water surface (rail in the air, bottom above a trough). Positive and large when the surface is buried under wave mass (rail under the wall, bottom under the lip).

### Force shape

Every per-surface drag/lift/thrust formula becomes:

```
force = coef × waterColumn × existingDirectionalAndVelocityFactors
```

The `existingDirectionalAndVelocityFactors` retain their current structure (pitch gates, perpendicularity, cos⁴ falloffs, planing attenuation, etc.). The water-column factor multiplies on top.

### Velocity reference

Relative water velocity (`sharedCalculations->relativeWaterVelocity`) throughout. The absolute-velocity sideways terms introduced for bottom and rails are reverted.

### Planing attenuation

Kept wherever it currently appears. `AmountPlaning` is derived from a tunable physics model with low noise; `waterColumn` is sampled from wave data and noisier. The two encode different things — column captures "is there water mass here," planing captures "is the board lifted by hydroplaning" — so they should both remain.

### Buoyancy

Unchanged.

## Per-Surface Specification

The table below lists every (surface, force) combination and what its new formula is. "× column" means multiply by `waterColumn` as defined above. "Existing factors" means: keep all the gates, perpendicularity, planing attenuation, etc. that the current formula has.

| Surface (ESide) | Force | New formula (sketch) | Notes |
|---|---|---|---|
| `VE_Down` (bottom) | Drag — forwards (skin friction) | `coef × column × pitchSinAOA × (1-planing)² × relVelAlongForwards²` | Existing formula + column. |
| `VE_Down` (bottom) | Drag — sideways (wall pushback) | `coef × column × relVelSideways²` | **Revert to relative velocity** from absolute. No planing attenuation, no pitch gate (current behavior). |
| `VE_Down` (bottom) | Thrust | `coef × column × existingFactors` | Includes both `alongThrustCoefficient` and `upwardsThrustCoefficient` terms in [FluidDynamics.cpp:504](../Source/GoneSurfing/FluidDynamics.cpp#L504). |
| `VE_Left` / `VE_Right` (rails) | Drag — forwards | `coef × column × (1-planing)² × relVelAlongForwards²` | Existing formula from commit `466fefcaa` + column. |
| `VE_Left` / `VE_Right` (rails) | Drag — sideways (face-on) | `coef × column × relVelSideways²` | **Revert to relative velocity** from absolute. Keep engagement gating from `466fefcaa` (LEFT engages when `relVelSideways · boardLeft > 0`, RIGHT when `< 0`). No planing attenuation. |
| `VE_Left` / `VE_Right` (rails) | Lift (Bernoulli) | `coef × column × cos² × relVel²` | **Replace `amountUnderwater`** factor in [FluidDynamics.cpp:336,358](../Source/GoneSurfing/FluidDynamics.cpp#L336) with `column`. |
| `VE_Tail` | Drag | `coef × column × cosWaterForwards × (1-planing*…) × relVel²` | Existing formula + column. |
| `VE_Nose` (new) | Drag | `coef × column × cosWaterBackwards × relVel²` | New surface — mirrors tail but engages when relative water hits the nose from in front. Currently `VE_Nose` returns zero force in [FluidDynamics.cpp:254](../Source/GoneSurfing/FluidDynamics.cpp#L254); replace with mirror-of-tail formula. |
| `VE_Fin` | Drag | `coef × column × cos⁴ × relVel²` | Existing formula + column. |

The `existingFactors` columns are intentionally not pinned to current source-line formulas because the goal is to add the column factor, not to refactor anything else.

## Sampling

`AWaveHeight` already exposes:

- `waveHeightAndNormal(location, frame)` — **internal; do not call from FluidDynamics.** The `frame` parameter must already be tile-adjusted (current frame + per-tile offset). Passing `WaterController->CurrentFrame` here would break the infinite-tiling system.
- `calculateWaveLocationAndNormalAuto(location)` — UFUNCTION, the **required** entry point from FluidDynamics. Internally resolves the correct tile-adjusted frame for the sample location.

Both return surface position (use Z) and normal at a (world X, Y).

### Where to sample

**Preferred: per-FluidDynamics.** ~20 FluidDynamics actors per board × one wave sample per tick = ~20 samples/tick/board. The wave sampler is already called for velocity, so the marginal cost of also returning height is small. Per-actor sampling preserves spatial accuracy: a front rail under the wave wall correctly reads a tall column while a back rail in flat water reads ~0.

**Fallback: per-SharedCalculations.** If profiling on mobile shows per-FD sampling is too expensive, the two SC actors (front, back) can sample once each and FluidDynamics actors borrow their nearest SC's column. This loses front-to-back resolution within a half-board but cuts samples from 20 → 2.

Implementation suggestion: add the sample in `AFluidDynamics::setup()` (already called per tick, already populates `up`/`forwards`/etc.). Cache as a `float waterColumnAbove` member on the actor and read it from each `calc*Force` function.

### Sampler perf considerations

- The current `calculateWaveVelocity` path does a per-tile-grid lookup with bilinear interpolation. Adding height to the existing call (rather than a second call) would amortize the grid lookup. If `AWaveHeight` doesn't already expose a combined "height + velocity at point" call, consider adding one before this spec lands.
- `FMath::Sqrt` and similar are mobile-sensitive; the new formula adds only one comparison (`max(0, …)`) and one multiplication per force, so the per-force cost is small.

## Implementation Sketch

### New UPROPERTYs / state

Per-surface UPROPERTYs already exist for all the relevant coefficients (`bottomDragCoefficient`, `bottomDragSidewaysCoefficient`, `railDragCoefficient`, `railDragSidewaysCoefficient`, `finDragCoefficient`, `tailDragCoefficient`, lift magnitudes, thrust coefficients). **No new coefficient UPROPERTYs are needed** — the column factor multiplies on top of them. Coefficients will need re-tuning (see "Coefficient retune" below).

Add to `AFluidDynamics`:

```cpp
// Refreshed each tick in setup(). Vertical distance from this actor's position
// up to the wave surface at the same (X,Y). Clamped to 0 when surface is below
// actor (surface in the air above a trough → surface lower than rail → 0).
UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
float waterColumnAbove = 0.0f;
```

### Sampling call

In `AFluidDynamics::setup()`:

```cpp
if (this->waveHeight)  // already wired or wire via reference / SC
{
    const FVector samplePos = GetActorLocation();
    const FVector waveLocation = this->waveHeight->calculateWaveLocationAndNormalAuto(samplePos)[0];
    this->waterColumnAbove = FMath::Max(0.0f, (float)(waveLocation.Z - samplePos.Z));
}
else
{
    this->waterColumnAbove = 0.0f;
}
```

(`calculateWaveLocationAndNormalAuto` returns `TArray<FVector>` per its signature; element 0 is the location, element 1 is the normal.)

### Per-formula edits

Each `calc*Force` function in `FluidDynamics.cpp` multiplies its computed magnitude by `this->waterColumnAbove`. Conceptually:

```cpp
const float oldMag = coef * existingFactors * v2;
const float newMag = coef * waterColumnAbove * existingFactors * v2;
```

Keep the per-surface debug logging; add `waterColumn:` to the log lines so we can verify in the snapshot tests.

### Reverting the absolute-velocity sideways drags

Two sites:

1. Bottom sideways drag in [FluidDynamics.cpp:114-127](../Source/GoneSurfing/FluidDynamics.cpp#L114) — switch `absWaterVel` → `relativeWaterVelocity`.
2. Rail sideways drag from commit `466fefcaa` — switch `absWaterVel` → `relativeWaterVelocity`. Keep the engagement-gating direction check (it's now applied to relative-velocity sideways component).

### Nose drag (new)

`VE_Nose` currently returns zero in [FluidDynamics.cpp:254](../Source/GoneSurfing/FluidDynamics.cpp#L254). Add a tail-mirror formula:

```cpp
// Mirror of tail: fires when water hits the nose from in front (board nose-dive,
// wave-front impact). cosWaterBackwards = relVel direction · -forwards.
float cosWaterBackwards = -(this->sharedCalculations->relativeWaterVelocityDirectionProjectedUp | this->sharedCalculations->forwards);
if (cosWaterBackwards > 0)
{
    const float planingAttenuation = 1.0f - sharedCalculations->AmountPlaning * (1.0f - cosWaterBackwards);
    float dragAmount = waterColumnAbove * cosWaterBackwards * relVel² × noseDragCoefficient × 0.01 × planingAttenuation;
    dragForce = -sharedCalculations->forwards * Clamp(dragAmount, 0, maxDragAmount);
}
```

A new `noseDragCoefficient` UPROPERTY is needed (mirror of `tailDragCoefficient`).

## Coefficient Retune

Adding a `× waterColumn` factor changes the units of every force from `cm²/s²` to `cm³/s²`. Every existing coefficient becomes wrong by a factor of roughly the typical column height (tens of cm). All affected coefs must be retuned in the editor after the code lands.

No normalization (e.g., `column / referenceWaveHeight`) — that would introduce a magic reference parameter and obscure the dimensional honesty the spec is trying to restore. Full retune is the right cost.

Procedure:

1. Land code change with all coefficients unchanged. Game will be visibly broken (forces ~10-100× too strong).
2. Run autopilot, observe peak force magnitudes per surface per force type in debug logs.
3. Scale each coefficient down by the observed peak-column factor (rough ratio of old peak force to desired peak force).
4. Re-run `surfing-down-the-line` autopilot; iterate against the **first ~8.5 s** of the trajectory (the window where pre-spec behavior was correct). Do not try to match anything after 8.5 s — divergence there is expected.
5. Once the first ~8.5 s lines up, eyeball the full run: verify trough drift is gone and the rails actually engage on the wave wall. Re-baseline if the full trajectory looks right (post-8.5 s will not match the prior baseline; that's fine).
6. `surf-straight` is not a reliable comparator right now (broken pre-spec) and should not gate this change.

## Acceptance Criteria

### AC1 — `waterColumnAbove` populated
Each FluidDynamics actor has a non-negative `waterColumnAbove` value, refreshed each tick. Verifiable via debug log or visualizer.

### AC2 — Trough drift eliminated
With the board sitting in flat water or in a wave trough, the lateral position drift over 5 seconds is < ~10 cm (i.e., no continuous sideways acceleration from the sideways drag terms). Compare against pre-spec behavior at the same coefficient values where the absolute-velocity sideways term produced visible drift.

### AC3 — Wave-wall engagement
In the autopilot scenario around gs=8.7 (the `surfing-down-the-line` test's wave-wall window), the per-rail sideways drag magnitude is non-trivial (~10s-100s of force units rather than ~1).

### AC4 — `surfing-down-the-line` first 8.5 s tracks pre-spec
After coefficient retune, the first ~8.5 s of the `surfing-down-the-line` trajectory matches the pre-spec run within snapshot-test thresholds (position drift < ~1000 cm, velocity drift < ~1500 cm/s, pitch < ~15°). Behavior after 8.5 s is allowed to diverge — that's the deferred-bug window. `surf-straight` is not a gate (broken pre-spec).

### AC5 — Nose drag fires only when expected
With autopilot pop-up, nose drag is zero (board pitched up); during a hypothetical nose-dive (pitch-down + forward motion), nose drag fires.

### AC6 — Performance budget
Frame time on the autopilot scenario is within ±5% of pre-spec performance on the dev machine. (Mobile perf measurement is a separate exercise — flag if it regresses materially.)

## Open Questions / Future Work

These don't block the spec but should be tracked:

1. **Vertical-column proxy may miss overhanging waves.** A wave wall that arcs over the rail has lots of mass *to the side* of the rail, not directly above it. The vertical column at the rail's (X,Y) underestimates this. If we see "wave overhangs the board but rails don't engage," consider sampling the column offset in the surface-normal direction (e.g., for a rail, sample 5-10 cm out in the -board.left direction).

2. **Relative velocity assumption may need revisiting.** With column-as-mass, relative velocity should be the right reference (skin friction is `ρ × ν × ∇v`, and `ν` is per-relative-flow). But if we discover wall-pushback scenarios where the board is moving with the wave AND the column is large AND we still want lateral force, we may need to mix in absolute velocity again. Don't pre-add the complexity; observe first.

3. **Fin engagement in flat water.** With the column factor, a fin in still water sees `column ≈ board draft (small)`, so fin drag is ~constant for steady-state planing. That's probably fine, but worth checking against the existing fin-stability tuning.

4. **Combined wave-sampler call.** If perf becomes a concern, add a `AWaveHeight::getSurfaceAndVelocityAt(location)` that returns both in one grid lookup, and route both SC and FD through it.

5. **Bug at gs=8.70 ("wave glides through board") deferred.** This spec is not a fix for that bug. Once the new force model is in place, re-evaluate whether the bug persists. The hypothesis that it's a wave-loop-wrap geometry artifact (rather than a force-balance issue) still stands; revisit independently.

## Status

- [ ] Spec reviewed
- [ ] `waterColumnAbove` sampled in `AFluidDynamics::setup()`
- [ ] Bottom forwards drag × column
- [ ] Bottom sideways drag: revert to relative velocity, × column
- [ ] Bottom thrust × column
- [ ] Rail forwards drag × column
- [ ] Rail sideways drag: revert to relative velocity, × column
- [ ] Rail lift: replace `amountUnderwater` with column
- [ ] Tail drag × column
- [ ] Fin drag × column
- [ ] Nose drag (new): tail-mirror formula × column, new `noseDragCoefficient` UPROPERTY
- [ ] Per-formula debug logs include `waterColumn:`
- [ ] Coefficients retuned to land first ~8.5 s of `surfing-down-the-line` near pre-spec trajectory
- [ ] `surfing-down-the-line` first 8.5 s within snapshot thresholds vs pre-spec
- [ ] `surfing-down-the-line` full run eyeballed (trough drift gone, rails engage on wall); re-baselined if looking right
- [ ] Memory: vertical-column-as-mass-proxy assumption (revisit if overhanging waves are wrong)
- [ ] Memory: relative-velocity-throughout assumption (revisit if wall pushback feels under-coupled)
