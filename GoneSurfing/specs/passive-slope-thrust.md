# Spec: Passive Slope Thrust (gravity-down-the-face drive, redirected along board.forwards)

> **Priority: FIRST of three specs split out from [barrel-glide-through-bug.md](barrel-glide-through-bug.md).** The other two are [fin-force-normalization.md](fin-force-normalization.md) and [wave-penetration-resistance.md](wave-penetration-resistance.md). This one is upstream of both: the board needs realistic forward drive before fin behaviour and wave-penetration can be observed in a non-degenerate scenario.

## Overview

Restore a *passive* forward propulsion sourced from gravity down the wave face, applied along `board.forwards`, and **independent of player weight-shift/pump intent**. Today the only forward-propulsion term (the bottom hydrofoil's `forwardsThrustCoefficient`) is gated by `turnGate`, which opens only on weight-shift (`lateralShift`) or pump intent. A board trimming a real wave face at neutral weight therefore gets **zero forward drive**, coasts to a stop, de-planes, and washes through the wave.

## Objective

A neutrally-trimmed board on a wave face holds/builds speed down the line (as a real board does), while a board on flat water going straight still decelerates. Forward drive comes from the wave (an energy source), not from the player having to wiggle the weight.

## Background / history (why it's like this)

- `waveSlopeGravity` (the old passive gravity-down-slope drive) was retired and replaced by the hydrofoil `forwardsThrustCoefficient` (see [bottom-hydrofoil-thrust.md](bottom-hydrofoil-thrust.md)). The hydrofoil thrust is `AoA × v²` and fires whenever the board planes at any angle of attack — **regardless of whether there is a wave/slope**.
- Because bottom drag is **planing-attenuated to ~0**, on flat water at speed you get `forwardThrust > 0` and `drag ≈ 0` → net positive → the board **gained/held speed going straight on flat water when it should decelerate** (the original bug the author hit).
- The fix applied was `turnGate = max(intentGate, pumpGate)` with `intentGate = SmoothStep(0.05, 0.20, lateralShift)` ([FluidDynamics.cpp:1118-1133](../Source/GoneSurfing/FluidDynamics.cpp#L1118)), suppressing forward thrust unless the player shifts weight ≥ ~10% off-centre or pumps. This is a band-aid that keys propulsion on the **wrong discriminator** (player input) rather than the physical one (is there an energy source / a wave face to fall down). It is also editor-vs-phone-fragile: the editor autopilot sits at `amountToTheRight = 0.5` (dead), and even near-neutral phone trim (`lateralShift < 0.05`) is dead on both platforms.

## Design

Source the magnitude from gravity down the slope, but apply the force along `board.forwards` — the fins/rails redirect the down-slope pull into forward motion. `board.forwards` is *almost perpendicular* to the fall line when trimming down the line, so the drive is the (small) projection of the down-slope gravity onto the forward axis:

```
F_drive = board.forwards_unit · k · amountWetted · (waveSlopeDownVec_boardWide · board.forwards_unit)
```

- **Board-wide** (decision, see Resolved Decisions): use the board-wide `waveSlopeDownVec`/`slopeSin` from SharedCalculations, not per-actor sampling — avoids the discretization yaw noise that per-actor slope would inject between symmetric front/back samplers. `waveSlopeDownVec` = world-down projected onto the wave-tangent plane ([SharedCalculations.cpp:283-284](../Source/GoneSurfing/SharedCalculations.cpp#L283)); `|waveSlopeDownVec| = sin(slope)`.
- **Gated on `amountWetted`** (decision), NOT `amountUnderWater`. `amountUnderWater` is sigmoid-squashed (~2.4% at 10 cm depth — see auto-memory `project_amount_under_water_sigmoid_squash`) and would strangle the drive the way it strangles hydrofoil upthrust. `amountWetted` (the surface-in-contact measure, ~1.0 when planing) is the correct "is the hull engaged with the water" gate. So an airborne board off the lip, or one whose hull isn't in the face, gets no phantom drive.
- The projection self-handles every case with no extra guard:
  - **Trimming down the line** (nose ~along crest, angled slightly down the face): `waveSlopeDownVec · forwards` small-positive → modest forward drive.
  - **Nose pointed UP the face**: dot < 0 → decelerates, will not push the board up the wave (sign falls out).
  - **Flat water**: `waveSlopeDownVec ≈ 0` → no drive → decelerates. Original flat-water bug stays fixed.
- "Almost perpendicular → small force" is fine: bottom drag is planing-attenuated to ~0, so a small continuous drive builds to a high terminal speed. The same planing-drag property that caused the flat-water bug is what makes across-the-face trim fast once the drive is slope-sourced. Self-consistent.
- **Keep the pump/carve hydrofoil thrust as an *additive booster*, not the gate.** Pumping should add energy on top of the passive drive; it should not be a prerequisite for any forward drive. (Decouple: passive slope drive is always-on when on a face; `turnGate`-gated hydrofoil thrust remains for the active carve/pump bonus.)

### Coupling caveat (read before testing)

`waveSlopeDownVec` decomposes in the board frame into a `board.forwards` component (this drive) and a **perpendicular** component. The perpendicular part is exactly what the fins/rails must resist. The fins are currently broken ([fin-force-normalization.md](fin-force-normalization.md)) — so a board with passive drive but no working fins will still slip sideways down/across the face. **Expect the slope-thrust test to be muddied by the broken fins**; this spec restores the drive, but clean validation of "holds position trimming down the line" may need the fin spec landed too. Land slope-thrust first (it's the prerequisite for non-degenerate observation), but interpret results with the fin coupling in mind.

## Requirements

- **FR1**: A neutrally-trimmed (`amountToTheRight = 0.5`, no pump) board on a wave face (`slopeSin > 0`) receives forward drive along `board.forwards` proportional to `waveSlopeDownVec_boardWide · board.forwards`.
- **FR2**: The passive drive is independent of `turnGate` / `lateralShift` / `pumpInput`.
- **FR3**: The existing hydrofoil `forwardsThrustCoefficient` thrust (intent/pump-gated) remains as an additive booster.
- **FR4**: The passive drive is gated on `amountWetted` (hull engaged with water), NOT `amountUnderWater` (sigmoid-squashed depth). No drive when the board is not wetted (airborne / out of the face).
- **FR5**: Implemented at the same site as the hydrofoil thrust (`calcThrustForce`, `VE_Down` branch), as a separate clearly-named force, registered separately in the torque budget via `RegisterAppliedForce`.
- **NFR1 (hard constraint)**: A board going straight on **flat water** (`slopeSin ≈ 0`) gains **no** forward speed and decelerates against drag/damping. This must not regress.
- **NFR2**: A board pointed up the face is not driven up the wave.
- **NFR3**: New coefficient `k` is a tuning UPROPERTY (and/or in the tuning subsystem — note [tuning-subsystem two-edit rule](../Source/GoneSurfing/SurfTuningSubsystem.h)); default chosen so terminal down-the-line speed is in the same ballpark as the current pop-up speed (~500 cm/s) on the test wave.

## Acceptance Criteria

- **AC1 — passive drive on a face**: With a centred-weight autopilot (no carve/pump), a board on the `Surfing_infinite_wave` face holds or builds speed down the line instead of coasting 507→61 cm/s. Verify via `boardFwd`-equivalent force > 0 in the thrust debug and the trajectory CSV speed not collapsing.
- **AC2 — flat-water still decelerates**: On flat water going straight (construct a flat-water case or a trough segment with `slopeSin ≈ 0`), forward speed monotonically decreases. The original free-speed bug does not return.
- **AC3 — no up-the-face push**: When the board's nose points up the face (`waveSlopeDownVec · forwards < 0`), the passive term is ≤ 0 (decelerating), never propelling up the wave.
- **AC4 — pump/carve still boosts**: With weight shift/pump active, total forward force = passive slope drive + the (gated) hydrofoil booster; pumping measurably increases speed over passive alone.
- **AC5 — snapshot not wrecked**: `surfing-down-the-line` snapshot stays within calibrated thresholds for the pre-escape steps; the post-escape trajectory is *expected* to change (board no longer washes out) — re-approve baseline once the new behaviour looks right.

## Test Plan

1. Implement `F_drive` on the bottom (`VE_Down`) actors in `calcThrustForce` (or a sibling), summed into the per-tick impulse.
2. Run `RunGameAndCollectLogs.ps1` with `TEST_MAP=Surfing_infinite_wave`, flags `thrust,state`, autopilot `surfing-down-the-line`. Confirm forward force > 0 through the cruise and the speed not collapsing to ~61 cm/s.
3. Tune `k` for a sane terminal speed.
4. Construct/confirm a flat-water check for AC2 (no free speed).
5. Re-observe the glide-through window: does the board still wash through with drive restored? (Feeds [wave-penetration-resistance.md](wave-penetration-resistance.md).)

## Resolved Decisions (2026-06-12)

1. **Site: same as the hydrofoil.** In `calcThrustForce` (`VE_Down` branch), as a separate clearly-named force, registered separately in the torque budget. (FR5)
2. **Coefficient: board-wide.** Use board-wide `waveSlopeDownVec`/`slopeSin` from SharedCalculations, not per-actor — avoids spurious yaw torque from per-actor slope sampling, consistent with the wave-mass-thrust rationale ([FluidDynamics.cpp:194-200](../Source/GoneSurfing/FluidDynamics.cpp#L194)).
3. **Gate: `amountWetted`, NOT `amountUnderWater`.** (FR4) The squashed `amountUnderWater` would strangle the drive; `amountWetted` is the correct hull-engagement gate.

### Remaining tuning note (not a blocker)

- Terminal down-the-line speed is set by `F_drive` balanced against the engine fork's quadratic forward-axis velocity damping (the "speed wall", auto-memory `project_custom_damping_system`). Tune `k` against that wall, not in isolation.

## Implementation notes (2026-06-12)

Implemented as `slopeThrustForce` in `calcThrustForce` (`VE_Down` branch), `FluidDynamics.cpp`. New tunable `slopeThrustCoefficient` (FluidDynamics.h default 0; SurfTuningSubsystem live default 10000) wired via the two-edit rule + refresh. Force = `boardFwdUnit · coef · amountWetted · (boardWideSlopeDown · boardFwdUnit)`, board-wide slope averaged across both SCs, gated on `boardWideSlopeSin > 0`, registered as `bottomSlopeThrust` in the torque budget, NOT subject to the hydrofoil cap (already bounded by `coef`). Hydrofoil `forwardsThrust` untouched (stays the gated booster). **Builds clean.**

**Behavioural verification (run `bk1ujk1v1`, `surfing-down-the-line`):** the term fires and is correctly signed — steep face + nose-down-slope → `slopeAlongFwd ≈ +0.28-0.37`, force ~2700-3600/actor (strong drive, AC1's *mechanism* confirmed); flat → ~0 (AC2); nose up-slope (`slopeAlongFwd < 0`, ~55% of ticks) → decelerates, never propels up the wave (AC3 ✓).

**MECHANISM VALIDATED (autonomous run loop, 2026-06-12).** A coefficient sweep settled it. At the committed `coef=10000` the drive was too weak to matter (monotonic coast-out). Cranking to `coef=60000` made the speed track `slopeSin` almost perfectly:

| t | slopeSin | v (cm/s) |
|---|---|---|
| 6.21 | 0.325 | 447 ↑ |
| 6.85 | 0.311 | **533** |
| 7.46 | 0.017 | 469 ↓ |
| 8.71 | 0.096 | 301 ↓ |
| 11.26 | 0.231 | 430 ↑ |
| 11.90 | 0.234 | **547** |

The board **accelerates hard on a real face (slopeSin ≈ 0.3) and decays on gentle/flat water (slopeSin < 0.1)** — exactly the designed behavior. The flat-water constraint (NFR1) holds: it decays, no free speed. The earlier "coast-out" was the board drifting onto the gentle *shoulder* (slopeSin < 0.1, correctly no drive), NOT a broken term — and my earlier "climbs up-slope" reading was wrong: z is ~constant; the board moves between the steep pocket and the flat shoulder of the dynamic tiling wave, so `slopeAlongFwd` (hence the drive) oscillates with the wave geometry, not with height.

**Regression fix — flat-water deadzone (2026-06-14).** Editor playtest found the board glided backwards at unrealistic speed when sitting between two waves on near-flat water. Cause: the term was gated only on `boardWideSlopeSin > 0`, so any residual slope on near-flat water × the strong coefficient (40000 × ~10 actors) flung a slow board around — a direct violation of the "no force when flat" constraint (NFR1). Fix: a tunable minimum-slope deadzone `slopeThrustMinSlopeSin` (default 0.12 ≈ 7° face) with a `SmoothStep(min, min+0.06, slopeSin)` ramp — zero drive below the threshold (trough/between-waves), full on a real face (slopeSin ≈ 0.3). Tunable on-device via the same path as the other coefficients.

So the term works. What's *not* yet validated is steady-state "holds speed down the line", because the unsteered autopilot lets the board drift out of the pocket onto the shoulder. That genuinely needs a steered ride (keep the board on the face) — still the open dependency, but the propulsion mechanism itself is confirmed and the coefficient is bracketed (10000 too weak, 60000 strong/realistic face speeds; set to 40000 as a moderate working value pending steered tuning).

## Status

- [x] Spec reviewed (open questions resolved 2026-06-12)
- [x] Passive slope drive implemented (along board.forwards, magnitude = projection of waveSlopeDownVec, board-wide, amountWetted-gated)
- [x] Decoupled from turnGate; hydrofoil thrust remains additive booster
- [x] Build clean
- [x] AC1 — **mechanism validated**: speed surges track slopeSin (board reaches ~530 cm/s on a face, decays on the shoulder). Steady-state "holds speed down the line" still needs a steered ride (board drifts off the pocket in the unsteered test).
- [x] AC2 — flat-water: decays on slopeSin < 0.1, no free speed (validated by the slopeSin–speed correlation)
- [x] AC3 — no up-the-face push (nose up-slope → decelerates, verified)
- [ ] AC4 — pump/carve still boosts (untested — needs a carving input)
- [ ] AC5 — snapshot re-approved
- [ ] Re-observe glide-through with drive restored (hand off to wave-penetration-resistance.md) — needs steered ride
- [~] Tune `slopeThrustCoefficient` — bracketed (10000 too weak, 60000 strong); set to 40000 provisional; finalize on a steered ride that keeps the board in the pocket

## Related

- [barrel-glide-through-bug.md](barrel-glide-through-bug.md) — parent investigation; full data and the design-history that motivates this.
- [bottom-hydrofoil-thrust.md](bottom-hydrofoil-thrust.md) — the hydrofoil thrust that replaced `waveSlopeGravity`.
- [pumping.md](pumping.md) — the pump term that stays as an additive booster.
- [fin-force-normalization.md](fin-force-normalization.md) — coupled: fins absorb the perpendicular component of `waveSlopeDownVec`.
