# Principle: Per-Actor vs Board-Wide Sampling

## Why this doc exists

The intuition "more sample points = more accuracy" is correct in many simulation contexts, but is **wrong** for some force calculations in this codebase. This document explains when and why.

## The intuition (and why it breaks)

The board has ~20 FluidDynamics actors arrayed across its bottom and rails. Each actor independently samples the wave geometry at its own (X,Y) position to compute `slopeSin`, `waterColumnAbove`, and `effectiveWaterHeight`. Naïvely, this seems like a Riemann-sum approach to integrating force across the surface — more sample points discretize the underlying continuous physics more faithfully.

That works for forces where each sample point genuinely represents an independent piece of local physics. **It breaks for forces where the physical phenomenon is board-wide in nature**, because the per-actor sampling injects *discretization noise* that the integration treats as real asymmetry.

The 2026-05-22 wave-mass-thrust diagnosis is the canonical example. Each FluidDynamics actor sampled `slopeSin` at its own position, producing values like:

| actor | slopeSin |
|---|---|
| bottom_left_front | 0.358 |
| bottom_right_front | 0.622 |
| bottom_left_middle-front | 0.312 |
| bottom_right_middle-front | 0.506 |

A 75% left-to-right difference at the front. The wave doesn't actually get meaningfully steeper across the ~1.5 m board width — that's noise from the underlying wave-height texture's spatial frequency happening to sample differently at neighboring positions. But the force formula multiplied `slopeSin × effectiveH × v² × coef` and applied the result at the per-actor position, producing an asymmetric thrust × position-arm = strong yaw torque. The board yawed back toward the wave for no physical reason — a pure artifact of independent sampling of a board-wide quantity.

## The principle

Ask: **does this force represent a local property of the surface, or a board-wide phenomenon?**

- **Local property**: the value at this point on the board is genuinely independent of the value at another point. Per-actor sampling is correct. Example: `amountUnderWater` for buoyancy — the nose can be above water while the tail is submerged, and each buoyancy actor needs its own value to model the moment couple. Example: `sinPitchAngleOfAttack` on the bottom — front actors at positive rocker tilt and back actors at negative rocker tilt genuinely have different angles of attack, and that asymmetry drives the realistic pitch dynamics we want.
- **Board-wide phenomenon**: the value is a property of the wave or the environment around the whole board, and any per-actor variation is sampling noise rather than physical reality. Use a single board-wide sample (typically at the SharedCalculations actor's position, which represents the board center). Example: wave-mass thrust — the wave is one continuous body of water that pushes the board forward as a whole; per-actor `slopeSin` variation is texture noise, not differential physics.

The decision turns on **physical interpretation**, not on whether the sample values happen to differ. The sample values *will* differ across actors regardless — the question is whether that difference is signal or noise *for this particular force*.

## Forces in this codebase, categorized

### Genuinely per-actor (keep independent samples)

- **Buoyancy** (`amountUnderWater` at each buoyancy actor's position) — board can be partially submerged, and the moment couple matters.
- **Sideways drag** (`sidewaysVel` per actor via SC) — the SC split into front/back means front-half and back-half can have different water flow, which is real.
- **Bottom forward drag** (planing-attenuated) — even-handed per-actor sampling. The asymmetry's effect is small because the planing attenuation `(1-planing)²` shrinks it.
- **Pitch hydrofoil thrust `sinAOA`** — rocker geometry means front and back actors have genuinely different angle of attack, and that's exactly what drives pitch dynamics. Keep per-actor for the AOA input.

### Should be board-wide (single sample at SC center)

- **Wave-mass thrust** — implemented per [wave-mass-thrust.md](wave-mass-thrust.md). Reads `boardWideSlopeSin` and `boardWideWaterColumnAbove` from SC. The motivating case for this doc.
- **Pitch hydrofoil thrust `effectiveWaterHeight` *for the boardFwd term only*** — see "Per-component nuance" below.
- **Yaw hydrofoil `sinSlip`** — implemented per [yaw-hydrofoil-board-wide-slip.md](yaw-hydrofoil-board-wide-slip.md). Computed from a board-wide `relativeWaterVelocity` (averaged across front and back SC) rather than the actor's per-SC value. See "Direction-flip nuance" below.
- **(Future candidates)** Any new force term that represents "the wave does X to the whole board" rather than "this surface point feels X". Wave-mass drag is a borderline case — it has `slopeSin` factor too but its direction is along flow, which by symmetry means the L/R per-actor differences cancel into pitch torque rather than yaw torque, so it doesn't bite in practice. If a future tuning pass shows it does bite, board-wide it.

### Direction-flip nuance: when per-SC sampling determines force *direction*

Some forces compute their direction from a per-SC quantity (e.g., the yaw hydrofoil's anti-slip direction is `+sign(sinSlip) × leftUnit`, where `sign(sinSlip)` flips when the dot product of flow and `left` changes sign). If the front and back SCs disagree on the sign of this quantity, the front and back forces point in *opposite* directions — same root cause as the magnitude-asymmetry case above, but with a discontinuous failure mode rather than a continuous one.

The fix shape is the same: use a board-wide value (averaged) for the input that determines direction. The yaw hydrofoil reads `(front.relativeWaterVelocity + back.relativeWaterVelocity) / 2` for its slip calculation. All bottom actors then share one `sign(sinSlip)`, so the anti-slip direction is consistent across the whole bottom.

Signature for this failure mode: a force with a `sign(...)` term in its direction formula, where the sign-determining quantity is sampled per-SC. When the board enters a flow transition zone, the back SC's value crosses zero before (or after) the front SC's — and during the brief window where they disagree on sign, the force opposition is at maximum.

Implementation pattern: cross-link the two SC actors (each has a pointer to the other) and average their inputs when computing the direction-determining quantity. See [yaw-hydrofoil-board-wide-slip.md](yaw-hydrofoil-board-wide-slip.md) for the working example.

### Per-component nuance: when one force has both flavors

A force that decomposes into multiple components with **different directions** can need different sampling strategies *per component*. The bottom (pitch) hydrofoil in [bottom-hydrofoil-thrust.md](bottom-hydrofoil-thrust.md) is the example: it produces three force components from a shared `commonMag = v² × sinAOA × amountUnderWater × effectiveWaterHeight`:

| Component | Direction | Same direction across actors? | Use which `effectiveH`? |
|---|---|---|---|
| `upForce` | per-actor `up` (rocker-tilted) | no — front/back tilt differently | per-actor |
| `boardFwdForce` | board-shared `forwards` (uniform) | **yes** | **board-wide** |
| `actorFwdForce` | per-actor `forwards` (rocker-tilted) | no — front/back tilt differently | per-actor |

The reasoning: the **same-direction** component (`boardFwdForce`) sums constructively into a single board-level vector, so per-actor magnitude variation directly becomes a yaw torque when applied at offset positions. Using board-wide `effectiveH` here eliminates the noise-driven L/R asymmetry on that component. The **different-direction** components (`upForce`, `actorFwdForce`) are physically distributed — their per-actor direction differences are the whole point of the rocker model, and the L/R per-actor magnitude differences on these components don't create a clean spurious yaw the way the boardFwd asymmetry did.

So in code, compute `commonMag` *twice* — once with per-actor `effectiveH` and once with board-wide `effectiveH` — and use each in the component where it belongs. `sinAOA`, `amountUnderWater`, and `v²InPitchPlane` stay per-actor in both versions; only `effectiveH` differs.

## How to spot the failure mode

Symptoms that a per-actor force is producing discretization noise:

1. **Uncommanded yaw or roll torque** with weight centered. The board pivots around an axis the player didn't intend.
2. **Pattern correlates with wave geometry** — happens specifically on wave faces, not on flat water, even when the force is meant to fire on both.
3. **Per-actor debug log shows large L/R asymmetry** between symmetric pairs (e.g., `bottom_left_front` vs `bottom_right_front` at the same Y).
4. **The torque scales with the formula's overall magnitude** — e.g., raising the coefficient makes the spurious torque worse proportionally. (This rules out one-off bugs and points at structural noise.)

When you see this, the fix is to move the sampled inputs from per-actor to a single board-wide value, *for this force only*. Don't migrate forces that are genuinely per-actor.

## Implementation pattern

Store board-wide values on `ASharedCalculations`, computed once per tick in `SC::setup()` from a single sample at the SC actor's position. FluidDynamics actors read SC's board-wide values when computing a force-of-the-board-wide-kind. They keep their own per-actor values for forces-of-the-local-kind.

Example (from wave-mass thrust):

```cpp
// In ASharedCalculations::setup, where SC already samples the wave:
const FVector actorLocation = this->GetActorLocation();
auto locationAndNormal = this->waveVelocity->calculateWaveLocationAndNormal(actorLocation, tileAdjustedFrame);
this->waveNormal = locationAndNormal[1];
this->boardWideSlopeSin = FMath::Sqrt(FMath::Max(0.0, 1.0 - this->waveNormal.Z * this->waveNormal.Z));
this->boardWideWaterColumnAbove = FMath::Max(0.0f, (float)(locationAndNormal[0].Z - actorLocation.Z));

// In AFluidDynamics wave-mass thrust calculation (per-actor function, board-wide inputs):
const float boardWideEffH = FMath::Min(
    this->baseHeight
        + this->sharedCalculations->boardWideWaterColumnAbove
        + this->sharedCalculations->boardWideSlopeSin * this->slopeHeight,
    this->maxEffectiveWaterHeight);
const float thrustAmount = this->waveMassThrustCoefficient
                         * boardWideEffH
                         * this->sharedCalculations->boardWideSlopeSin
                         * sidewaysVel.SizeSquared();
```

Every bottom actor produces the same `thrustAmount` for the same flow conditions. Net torque from L/R-symmetric placement is zero by construction.

## When in doubt

Default to per-actor (it's the existing convention and is usually right). If a tuning pass produces unexplained yaw or roll, suspect this failure mode and check the per-actor debug log for L/R asymmetry on the suspect force. Then either move the inputs to SC (as above) or, if the force is genuinely local but its formula is too sensitive, apply a per-actor magnitude cap (the `maxHydrofoilForceAmount` pattern) that normalizes magnitudes across the spike.
