# Spec: Per-Axis Velocity Ceiling (Engine-Fork Damping Safety Net)

## Overview

Extend the engine fork's custom Chaos damping system with **per-axis velocity ceilings** on all three local axes (X = forward, Y = sideways, Z = vertical), exposed as UPROPERTYs on `ASurfboardUtils`. Currently only the Y axis has a hard clamp (`p.Chaos.Solver.MaxVelocityY = 2000`); the X and Z axes are uncapped, which lets a runaway hydrofoil force launch the board to multi-tens-of-m/s before any drag can respond.

Scope: engine-fork changes to `PBDRigidsEvolutionGBF.cpp`, plus matching wiring in `ASurfboardUtils`. No gameplay-side force changes. Safety net, not primary brake.

## Motivation

The 2026-05-21 instrumented run captured a force-magnitude breakdown during a launch event in `surfing-down-the-line`:

| time | actor | v²InPlane | actorFwdForceMag | force vector (X,Y,Z) |
|---|---|---|---|---|
| 14:58:28.173 | front | 944,226 | 17833 N | (-830, 15239, **8383**) |
| 14:58:28.223 | front | 1,015,780 | 17861 N | (-773, 14318, **8701**) |
| 14:58:28.624 | back | 3,210,630 | 17722 N | (5655, **48233**, **30874**) |

One bottom actor producing **48 kN forward and 31 kN upward per tick**. Total per-tick force across ~10 bottom actors is hundreds of kN. The diagnosis is in two parts:

1. **Coefficient root cause:** `actorForwardsThrustCoefficient` was set to 0.100 in the umap, 100× the empirically-derived stability limit of 0.001 ([FluidDynamics.h:115-117](../Source/GoneSurfing/FluidDynamics.h#L115-L117)). The rocker-tilted front actors translate this into a strong vertical force component, launching the board.
2. **Structural amplifier:** Hydrofoil forces scale with v². Once the board launches and gains speed, v² grows quadratically, and every force scales with it. The existing `effectiveWaterHeight` cap (200 cm) bounds one input, but nothing bounds v². No quadratic drag balances it (forward drag is planing-attenuated to ~0; sideways drag has zero forward component).

The coefficient retune ([bottom-hydrofoil-thrust.md](bottom-hydrofoil-thrust.md)) addresses #1. This spec addresses #2: even with safe coefficients, a future tuning error or a new scenario can re-trigger v² runaway. A per-axis velocity ceiling is a structural safety net that catches runaway regardless of which force is misbehaving.

## Background

### Existing damping infrastructure

The engine fork at `E:\windowsgrejor\git\UnrealEngine` already implements custom per-axis damping in `Chaos::PBDRigidsEvolutionGBF`:

- **Per-axis linear damping** ([PBDRigidsEvolutionGBF.cpp:164-170](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp#L164)) — `DampingLocalX`, `DampingLocalY`, `DampingLocalZ` cvars, applied as `(1 - damping)` multipliers on local-frame velocity each integration step.
- **Y-axis hard clamp** ([PBDRigidsEvolutionGBF.cpp:1002](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp#L1002)) — `V.Y = FMath::Clamp(V.Y, -MaxVelocityY, MaxVelocityY)` after integration. Currently the only per-axis ceiling in the fork.
- **Total-magnitude clamp** ([PBDRigidsEvolutionGBF.cpp:1061-1066](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp#L1061)) — `HackMaxVelocity2` cvar, disabled by default (-1).
- **Velocity-dependent damping factor** ([SurfboardUtils.cpp:103-117](../Source/GoneSurfing/SurfboardUtils.cpp#L103)) — on the X axis only, `finalDamping = min(baseDamping + scale² × excessVel², 0.95)`. Damping fraction capped at 0.95, so 5% of any applied force still translates to acceleration regardless of speed.

### Why velocity-dependent damping alone isn't enough

`MaxVelocityDamping = 0.95` means at any speed, 5% of the applied force becomes acceleration. With the observed 48 kN per-actor forward force across ~10 actors, 5% × 480 kN = 24 kN of net force on a ~50 kg board = 480 m/s² ≈ 49g sustained acceleration. The damping fraction approach can never beat a force runaway because it scales with the same force it's trying to suppress.

A hard ceiling on the resulting velocity (regardless of force) is structurally different — it guarantees the board cannot exceed the configured max velocity per axis, no matter what force is applied.

## Design

### Per-axis hard clamp

Mirror the existing `MaxVelocityY` pattern on the X and Z axes. After the integration step, clamp local-frame velocity components:

```cpp
// In PBDRigidsEvolutionGBF.cpp, after the integration step but before world-frame transform:
const Chaos::FVec3 VLocal = LocalRotation.UnrotateVector(V);
const Chaos::FVec3 VLocalClamped(
    FMath::Clamp(VLocal.X, -CVars::MaxVelocityX, CVars::MaxVelocityX),
    FMath::Clamp(VLocal.Y, -CVars::MaxVelocityY, CVars::MaxVelocityY),
    FMath::Clamp(VLocal.Z, -CVars::MaxVelocityZ, CVars::MaxVelocityZ));
V = LocalRotation.RotateVector(VLocalClamped);
```

(Exact code shape depends on the existing local-frame conversion already used by `DampingLocalX/Y/Z`.)

### Asymmetric vertical (optional)

Vertical motion has different real-world bounds depending on direction:
- Upward velocity: should be capped low (a surfboard rarely accelerates upward at more than 5-10 m/s).
- Downward velocity: should be capped higher to allow realistic falling speed when the board leaves the wave.

Recommend exposing `MaxVelocityZUp` and `MaxVelocityZDown` separately. The X axis (forward) and Y axis (sideways) are symmetric.

### Smooth ramp vs hard cliff

A hard clamp produces a velocity discontinuity each tick when the cap is hit — `V` snaps from "would-have-been" to the cap value. This is fine for catastrophic events but can cause numerical noise (constraint solver jitter, integration instability) during sustained high-speed motion.

**Recommendation: ship the hard clamp first.** The existing `MaxVelocityY` already uses this pattern in shipping code without observable issues, and the cap should fire rarely once gameplay coefficients are tuned. If clamp-induced jitter shows up later, layer a soft-ramp approach (similar to the existing `VelocityDampingScale` shape) above the hard clamp — the soft ramp keeps velocity below the cap in normal play, the hard clamp catches anything that slips past.

### Per-tick overshoot characterization

At the observed peak acceleration (1500g), one 16 ms tick can add ~235 m/s of velocity. The hard clamp catches this *after* the integration step, so the **velocity gets clamped, but the position has already been integrated using the un-clamped velocity** for that tick. The board may translate ~3.7 m in the overshoot tick before the next tick's velocity is bounded.

Acceptable tradeoff: the launch event is captured (board cannot keep accelerating), even if the immediate position jump is large. With substepping, the per-substep overshoot is proportionally smaller.

If the position jump itself becomes a gameplay problem (e.g., teleporting through wave geometry), the structural fix is to clamp **acceleration**, not velocity — apply a max-force-magnitude clamp pre-integration. That's option 2 in the original diagnosis (force-magnitude cap), and is out of scope for this spec.

### Configuration surface

Add UPROPERTYs on `ASurfboardUtils` mirroring the existing `ClampYVelocityAt` pattern at [SurfboardUtils.h:102-103](../Source/GoneSurfing/SurfboardUtils.h#L102):

```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
float MaxVelocityX = 3000.0f;  // cm/s, forward axis

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
float MaxVelocityZUp = 1000.0f;  // cm/s, upward velocity cap

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Damping")
float MaxVelocityZDown = 2000.0f;  // cm/s, downward velocity cap (fall speed)
```

(Y already has `ClampYVelocityAt = 2000.0f`. Rename to `MaxVelocityY` for naming consistency or leave for backwards compat.)

In `ASurfboardUtils::BeginPlay`, push these to the corresponding cvars alongside the existing `MaxVelocityYCVar->Set(...)` call at [SurfboardUtils.cpp:29](../Source/GoneSurfing/SurfboardUtils.cpp#L29).

In the engine fork, add the cvar declarations alongside the existing `MaxVelocityY` at [PBDRigidsEvolutionGBF.cpp:179-180](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp#L179) and the clamp invocations alongside the existing one at [PBDRigidsEvolutionGBF.cpp:1002](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp#L1002).

### Starting values

Anchor to observed normal-surfing velocities from the 2026-05-21 snapshot (post-cap, pre-runaway):

- **X (forward):** observed peak ~14 m/s (1400 cm/s) during aggressive down-the-line. Cap at **3000 cm/s** gives 2× headroom for player skill / coefficient experimentation.
- **Y (sideways):** existing 2000 cm/s seems empirically fine. Keep.
- **Z up:** observed peak vz ~3 m/s during board hop, normal surfing keeps vz under 1 m/s. Cap at **1000 cm/s** (10 m/s upward) is generous.
- **Z down:** terminal-velocity falling under reduced gravity (~3.3 m/s²) caps naturally at modest speeds; cap at **2000 cm/s** (20 m/s) is a structural safety net, should rarely fire.

These values are configurable per umap. The defaults exist so the cap isn't accidentally infinite.

## Implementation Sketch

### Engine fork (`PBDRigidsEvolutionGBF.cpp`)

1. Declare X and Z cvars next to the existing `MaxVelocityY`:
   ```cpp
   FRealSingle MaxVelocityX = -1.f;   // disabled by default
   FRealSingle MaxVelocityZUp = -1.f;
   FRealSingle MaxVelocityZDown = -1.f;
   FAutoConsoleVariableRef CVarMaxVelocityX(TEXT("p.Chaos.Solver.MaxVelocityX"), MaxVelocityX, TEXT("Clamp for local-frame velocity on X axis. -1 = disabled."));
   // ... etc
   ```
2. Locate the existing local-frame velocity computation around the Y-clamp ([line 1002](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp#L1002)). Add clamps for X and Z components, with `-1` meaning disabled (no clamp). Apply to the same `VLocal` decomposition the Y clamp uses, then rotate back.
3. Match the existing log shape for `DebugDamping` so all three axes' clamp events are visible when the flag is on.

### Gameplay side (`SurfboardUtils.h/cpp`)

1. Add `MaxVelocityX`, `MaxVelocityZUp`, `MaxVelocityZDown` UPROPERTYs (defaults per Starting values).
2. Cache cvar pointers in the existing `protected:` block (lines 125-143).
3. Push values in `BeginPlay` (next to the existing `MaxVelocityYCVar->Set(...)`).
4. Default new UPROPERTYs to the suggested starting values so umaps without explicit values inherit a reasonable safety net.

### Per-actor vs global

The Chaos solver applies these cvars **globally** — they affect every dynamic body in the simulation, not just the surfboard. If non-surfboard physics in this project needs different limits, the right answer is per-body damping data (Chaos has hooks for this) rather than per-board cvars. **Out of scope for this spec.** Currently the surfboard is the only thing with substantial physics activity, so global cvars are fine.

## Acceptance Criteria

### AC1 — Compiles, all four cvars exist and are settable

Engine fork rebuilds. `p.Chaos.Solver.MaxVelocityX`, `MaxVelocityY` (existing), `MaxVelocityZUp`, `MaxVelocityZDown` all readable via the console and writable via cvar set.

### AC2 — UPROPERTYs push to cvars on BeginPlay

`ASurfboardUtils::BeginPlay` sets all four cvars from its UPROPERTYs. Verified by logging cvar values after BeginPlay completes; they match the UPROPERTY values from the umap.

### AC3 — Board cannot exceed configured limits during normal play

With defaults (MaxVelocityX=3000, MaxVelocityZUp=1000, MaxVelocityZDown=2000, MaxVelocityY=2000), running `surfing-down-the-line` from the snapshot baseline produces no velocity samples exceeding any cap. The snapshot's max-velocity column (`vx`, `vy`, `vz`) stays under the corresponding cap × 1.05 (5% margin for the immediate-pre-clamp tick).

### AC4 — Board cannot escape the simulation under coefficient runaway

With `actorForwardsThrustCoefficient = 0.1` (the unsafe value that triggered the original spike), and all velocity ceilings active: board may behave badly (jittery, oscillating near the cap) but cannot reach the 9000+ cm/s velocities seen in the pre-cap CSV. Specifically `max(|vx|, |vy|, |vz|)` stays under each cap × 1.10.

### AC5 — Disabled state is the pre-spec behavior

Setting all four cvars to -1 (or any negative value) disables the clamps and the simulation behaves exactly as before this spec landed. Verifiable by setting them to -1 and confirming the original runaway reproduces.

### AC6 — Performance budget

Frame time within ±2% of pre-spec. Three additional FMath::Clamp calls per dynamic body per integration step. Negligible.

### AC7 — Snapshot baselines re-approve

`surfing-down-the-line` and `surf-straight` baselines re-recorded after the ceilings are active with the suggested default values. The trajectories may shift slightly (especially `surfing-down-the-line` at the spike window) but should remain qualitatively similar.

## Open Questions / Future Work

These don't block the spec.

1. **Soft ramp under the hard clamp.** If clamp-induced jitter shows up in normal play, layer a quadratic damping ramp (similar to the existing `VelocityDampingScale` shape) above the hard clamp on each axis. The ramp keeps velocity smoothly under the cap; the hard clamp is the last-resort safety net.
2. **Acceleration clamp (force-magnitude cap).** The hard velocity clamp catches velocity after integration; it doesn't prevent the underlying force from being computed. A pre-integration acceleration cap would prevent the per-tick velocity overshoot and the resulting position jump. Should be considered if the overshoot-position-jump becomes a gameplay problem (e.g., board teleporting through wave geometry).
3. **Per-body limits.** The Chaos solver applies these cvars globally. If the project ever simulates multiple distinct bodies with different velocity budgets (e.g., surfer pawn vs board vs waves), migrate to a per-body damping component instead of global cvars.
4. **Coordinated with the `effectiveWaterHeight` cap.** Both this spec and the `maxEffectiveWaterHeight` cap (added in commit `eb4bfc82a`) are bounding-input safety nets. The effectiveH cap is at the force-input layer; this spec is at the velocity-output layer. They're complementary, not redundant: effectiveH bounds the per-tick force magnitude, velocity ceiling bounds the integrated result. Keep both.

## Status

- [ ] Spec reviewed
- [ ] Engine fork: `MaxVelocityX`, `MaxVelocityZUp`, `MaxVelocityZDown` cvars declared
- [ ] Engine fork: clamps applied in the integration step alongside the existing `MaxVelocityY` clamp
- [ ] Engine fork: clamp events logged under `DebugDamping`
- [ ] `ASurfboardUtils`: matching UPROPERTYs added with suggested defaults
- [ ] `ASurfboardUtils::BeginPlay`: pushes UPROPERTYs to cvars
- [ ] AC3 verified (no cap exceeded in normal play with safe coefficients)
- [ ] AC4 verified (runaway with unsafe `actorForwardsThrustCoefficient = 0.1` cannot escape)
- [ ] AC5 verified (disabling cvars reproduces pre-spec behavior)
- [ ] AC6 frame time within ±2%
- [ ] AC7 `surfing-down-the-line` and `surf-straight` baselines re-approved
- [ ] Memory: per-axis velocity ceiling as structural safety net — record once stable
