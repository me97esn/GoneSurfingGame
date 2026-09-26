# Spec: Decouple wave-mass drag from roll torque

## Status
- [x] Root cause investigated and confirmed (2026-06-29, via the `surf.debug.flags 'torque'` budget)
- [x] One fix attempted and rejected (reproject the drag direction — see "Rejected" below)
- [x] **Decision (2026-06-30): option B** — relocate the roll-producing wave-mass *flow* drag to the CoM
      longitudinal axis. A ruled out (kills the pitch); see "Force structure" below. C kept as the exact
      fallback if a feel test shows B lost a pitch nuance.
- [x] Implement B (`0b0c096c7`, `waveMassRollDecouple` default 1.0) — roll torque of `dragBottom_waveMassFlow`
      / `dragRailFlow` now measures **0** in the budget; confirmed.
- [x] **Option C for YAW added (2026-07-01, `waveMassFlowYawDecouple`, default 1.0).** B kept the flow drag's
      yaw (`r_fwd·F_left`), and that yaw was measured as the **dominant into-the-wave steering torque** down
      the line (`dragBottom_waveMassFlow` −34k + `dragRailFlow` −31k ≈ −64k net) — it turns the nose up the
      face at centred weight, which reads as the "board yaws into the wave even when rolled away" bug. C splits
      `applyWaveMassForceAsImpulse` into `AddImpulse` (linear at CoM) + `AddAngularImpulseInRadians` with the
      board.up (yaw) axis stripped via `getWaveMassKeptTorque`; the nose-up-into-the-face **pitch is kept**.
      Budget stays honest via a new `RegisterAppliedForceWithTorque`. Verified: flow yaw → 0, pitch preserved,
      roll still 0. **3× A/B on `surfing-down-the-line`:** ON beats OFF on all three metrics (median minYaw
      −21° vs −48°, down-line travel 3671 vs 2819, planing sustained 12.3s vs 11.3s) — a real net improvement,
      though distributions overlap (bistable at n=3).
- [x] **Extended option C to `wavePenetration`** (2026-07-02, `wavePenetrationYawDecouple`, default 1.0) — it
      was the #1 into-wave yaw left after the flow decouple. Routed through the same path (new
      `PendingWavePenetrationForce`, own knob so it's independently A/B-able); `getWaveMassKeptTorque` /
      `applyWaveMassForceAsImpulse` now take the yaw-decouple factor as a param. The linear "wall" (glide-through
      stop) + pitch survive; roll+yaw dropped. Also removed `wavePenetration` from the `dragForce` aggregate, so
      `dragBottom` no longer double-counts it.
- [x] **DIRECTION FIXED (the real win, 3× on `surfing-down-the-line`).** The into-wave turn is driven by the
      wave shoving the board *away* from itself (measured `F_left ≈ +15,800` = toward shore) applied effectively
      **aft of the CoM**, which with a ~central (rail) pivot yaws the nose *into* the wave. Two levers together
      flip it: (a) **tail fin authority** — raise `finLiftMagnitude` 0.001→**0.01** (moves the centre of lateral
      resistance aft = weathercock stability; at 0.01 the bistable ±50° `minYaw` spread **collapses to ±1.5°**,
      0.03 over-shoots), and (b) **remove the wave-mass drag family's into-wave yaw** (flow + penetration
      decouple). Combined result: `minYaw` **+3.5 / +8.9 / +10.3** (was −2/−21/−52) — the board **no longer turns
      past straight into the wave**, holds its line, travels furthest down the line (maxY ~4160), sustains
      planing (no stall through gs12), clean carve (AC2 slip ~3.4°, maxSlip 9–14°, no over-grip). See
      [[downline-yaw-into-wave-is-overcarve]].
- [ ] **Regression pass before adopting as defaults:** the combo (finLift 0.01, flow + penetration yaw-decouple)
      is validated on `surfing-down-the-line` only. Re-check `pop-up-2` (the stall), the sharp-turn autopilots,
      and the flat-water snapshot baselines (all shift). `wavePenetration` losing its yaw may affect hard
      turns / glide-through — verify.
- [ ] Close the buoyancy caveat
- NOTE the torque budget's `TOTAL` **double-counts** per-side aggregates (`dragBottom`/`dragRail`/…) with their
  sub-components (except the flow/penetration terms, now registered once via `RegisterAppliedForceWithTorque`).
  Trust per-category deltas, not TOTAL magnitude. Slip must use heading = **yaw+90** (mesh rotated 90°).

## Force structure — is the wave-mass force board-wide? (decides A vs B)

Checked in code; the nuance flips the A-vs-B choice:
- **Flow direction & speed ARE board-wide:** `boardWideAbsWaterVel = 0.5·(frontSC + backSC)`
  ([FluidDynamics.cpp:241](../Source/GoneSurfing/FluidDynamics.cpp#L241)) — identical for every actor.
- **Force MAGNITUDE is NOT:** it scales with `boardWideEffectiveH`/`boardWideSlopeSin` taken from the
  actor's *own* SC. Bottom actors are split front/back across two SCs, so **front actors get a larger force
  than back when the nose is in the face**. `wavePenetrationDrag` is fully per-actor. **This front/back
  variation IS the pitch** that redirects the board along the face instead of nose-first into it.
- Within each half it is L/R-uniform by design (`per-actor-vs-board-wide-sampling`), so there's no spurious
  L/R magnitude asymmetry — **the roll is the keel effect** (uniform sideways force at below-CoM actors),
  not magnitude asymmetry.

Consequence: **A (collapse to the CoM point) would zero `r_fwd` and kill the pitch → rejected.** The fix
must preserve the fore/aft application (the pitch) while removing the lateral+vertical moment arms (the roll).

## Overview

The **wave-mass drag** (the `waveMassFlowDrag` / `wavePenetrationDrag` / `waveMassThrust` family on the
bottom and rail `AFluidDynamics` actors) exists to:

1. **Stop the board gliding through the breaking wave** (nose-first or rails-first) — a positional /
   penetration resistance that keeps the board *on* the face.
2. **Push the board hard when the breaking wave hits it** — propulsion from the wave's water mass.

Both are intended as **linear** effects. The problem found below is that a strong wave-mass drag, applied
**at the FluidDynamics actor position** (offset from the centre of mass), also produces a **torque** — and
its **roll** component drives a runaway that banks the board into the wave and (downstream) yaws it over the
far side, even at centred weight with no rider input.

This spec records the confirmed root cause and lays out the full menu of ways to keep the wave's
push/stop while removing the unwanted roll, so the approach can be chosen deliberately.

## Root cause (confirmed)

Captured with the engine torque budget (`ASharedCalculations::RegisterAppliedForce` → per-category
board-space roll/pitch/yaw torque, logged under `surf.debug.flags 'torque'`):

- The board banks "into the wave" at centred weight. The dominant **roll torque** comes from the
  **wave-mass *flow* drag** — `dragRail` (rail `flowDrag`) and `dragBottom_waveMassFlow` (bottom). It runs
  away as a feedback loop (bank → rail digs in → more drag → more bank), scaling with
  `slopeSin × |absWaterVel|²`.
- **The roll is NOT from the force pointing "up".** Board-frame force decomposition shows the rail drag
  force is mostly **forward** (skin friction along `board.forwards`), which produces **zero** roll torque.
  The roll comes from the **smaller board-up component** of `flowDrag` — whose direction follows the wave's
  actual `absoluteWaterVelocity` (which has a vertical component) — at the actor's lateral offset:
  `roll torque = r_left·F_up − r_up·F_left` (verified: consistent ~13–15 moment arm).
- **The carve (`lateralTurnForce`) is downstream, not the cause** — it contributes ~0 roll torque; the bank
  feeds the roll-keyed carve which then yaws the board. (Ruled out by torque budget + a deadzone test.)
- **The planing redirect is not involved** — ruled out by A/B (`PlaningRedirectMaxAngle 0` → identical
  trajectory).

Key framing: **torque = r × F.** The force `F` is doing its job; the roll is purely because it is applied at
`r ≠ 0` from the CoM. Also note `waveMassFlowDrag` is computed from **board-wide** quantities
(`boardWideAbsWaterVel`, `boardWideEffectiveH`, `boardWideSlopeSin`) — so its per-actor *application* adds no
real spatial physics, only spurious torque. Moving it to the CoM is arguably *correcting* the model.

## Rejected approach: reproject the drag direction

`waveMassFlowVerticalScale` (FluidDynamics, **default 1.0 = off**) strips the board-up component of the wave
flow feeding `waveMassFlowDrag` on rail + bottom. **It does not work and is left off:**

- Rail-only strip cut `dragRail` roll torque 132k → 27k, but net behaviour was unchanged (still yawed over).
- Adding the bottom strip made it **worse** (TOTAL roll torque 100k → 245k, peak yaw 96° → 127°).
- **Why (geometric, not bistability):** the bottom actors sit *below* the CoM. Zeroing `F_up` redistributes
  the flow's magnitude into board-left, and the `−r_up·F_left` term (sideways drag below CoM, keel-like)
  *grows*. **You cannot remove "the roll part" by reprojecting a force's direction** — the actors' lateral
  AND vertical offset from CoM turns every force direction into roll/pitch/yaw torque.

The knob is kept in code (set to 0 to re-test) but **default off**.

## The option menu

Three levers: **(1) shrink `r`** (apply at/near CoM), **(2) keep `r`, cancel the unwanted part of `r × F`**,
or **(3) drop the force and operate on velocity** (CoM-centric by definition).

| # | Option | Removes | Keeps | Effort | Robustness |
|---|---|---|---|---|---|
| A | **Apply wave-mass force at CoM** (`AddImpulse` instead of `AddImpulseAtLocation`) | all torque | linear push + glide-through stop | small (project) | total |
| B | **Project the application point onto the CoM roll-axis** (`r_left=r_up=0`) | roll only | linear + pitch + yaw | small | total (roll) |
| C | **Selective torque cancellation** (apply linear at CoM + add back only chosen torque axes) | chosen axes | per-axis choice | moderate | total (chosen) |
| D | **Engine-fork velocity coupling** (wave-normal velocity clamp + flow-direction Δv) | all torque | linear, fork-consistent | large (fork + feed + retune) | highest |
| E | **Centreline / symmetrised application** | roll | force model + pitch | small | high |
| F | **Reduce `waveMassFlowDragCoefficient`** | scales push+roll together | — (no decoupling) | trivial | weak |
| G | **Roll-rate damping / righting** (complementary) | the *symptom* (any source) | — | moderate | high, but fights a force you could just stop |

### A — Apply the wave-mass force at the CoM  — **REJECTED (kills the pitch)**
`AddImpulse` (at CoM) instead of `AddImpulseAtLocation` → pure linear, zero torque. But it zeroes `r_fwd`
too, so it **destroys the front/back pitch** (see "Force structure") that redirects the board along the
face. Only viable if that pitch were unwanted — it isn't.

### B — Project onto the CoM longitudinal axis (kill roll, keep pitch) — **CHOSEN**
Apply the roll-producing wave-mass *flow* drag at the point on the **board-forward line through the CoM**
(`r_left = 0` **and** `r_up = 0`, keep `r_fwd`). From the moment-arm decomposition:
- `roll = r_left·F_up − r_up·F_left` → **0** (both arms removed — this also kills the keel-roll, which is
  why zeroing *only* the lateral offset isn't enough).
- `pitch = r_up·F_fwd − r_fwd·F_up = −r_fwd·F_up` → **preserved** via the front/back magnitude difference.
- `yaw = r_fwd·F_left` → preserved.

Scope: relocate only the **flow** terms (`waveMassFlowDrag` on bottom, `flowDrag` on rail) — those are the
confirmed roll sources (their direction is off-axis). `forwardDrag` (skin friction) and `waveMassThrust`
(both along `board.forwards` → 0 roll) and `wavePenetrationDrag` (per-actor, intended yaw/skid damping)
stay at the actor. Gated by a `waveMassRollDecouple` knob (0 = legacy at-actor, 1 = on the CoM axis) for
A/B and tuning; register the relocated force at its new point so the torque budget reflects the fix.

**Caveat:** zeroing `r_up` drops one *minor* pitch contribution (`r_up·F_fwd`). If a feel test shows it
mattered, switch to **C** (below), which keeps it.

### C — Selective torque cancellation
`AddImpulseAtLocation` ≡ `AddImpulse(impulse)` + `AddAngularImpulse(r × impulse)`. Apply the linear at CoM
and add back only the torque components you want (e.g. pitch yes, roll/yaw no) via
`AddAngularImpulseInRadians`. Makes attitude effects **deliberate** instead of accidental byproducts.

### D — Engine-fork velocity coupling (the "like the redirect" option)
Express both purposes as operations on the rigid body's **linear velocity `V`** in the Chaos integrator
(`PBDRigidsEvolutionGBF.cpp`), fed by CVars like `PlaningRedirectUp` — `V` is the CoM velocity, so **zero
torque by construction**:
- **Stop glide-through** = damp/clamp the component of `V` along the horizontal wave normal (velocity-space
  `wavePenetrationDrag`; same shape as the existing per-axis velocity ceilings, along a fed direction).
- **Wave push** = inject **Δv along the wave-flow direction** when the breaking wave hits (gated by impact).
  Note this *adds energy* — an injection, not a speed-preserving redirect like planing.
- Feed `WaveNormalHoriz`, flow direction, and slope/impact gates as CVars from `SurfboardUtils` each tick
  (mirror the redirect feed).

Heaviest change (the fork is what everything else is tuned against) but physically cannot roll.

### E — Centreline / symmetrised application
Apply the wave-mass push at the board **centreline** (no lateral offset) instead of at the engaged rail /
L-R bottom actors. Force-based cousin of A/B; loses the single-sided "only the submerged rail" realism
(acceptable if the purpose is purely linear push/stop). Note: the bottom L/R pairs *look* symmetric but
still rolled when banked (board-frame symmetry ≠ world symmetry under roll).

### F — Reduce magnitude
`waveMassFlowDragCoefficient` scales push and roll together. Stopgap / combine only.

### G — Roll damping / righting (complementary)
A roll-rate damper / righting moment about `board.forwards` in the integrator. Treats the *symptom* (kills
the runaway regardless of source, **including the unregistered buoyancy contribution**) rather than the
cause. Pairs with any of A–F.

## Decision guidance — which torques are wanted?

- **Roll (about forward):** unwanted — this is the bug.
- **Pitch (about left, nose-up):** plausibly **wanted** ("stop nose-first glide-through" may rely on the
  wave pitching the nose up). If so, A is too aggressive → prefer **B** or **C**.
- **Yaw (about up):** probably unwanted (it fed the over-turn via roll→carve), but confirm no intended
  "turn along the wave" depends on it.

So the choice largely reduces to **"do we keep the wave's nose-up pitch effect?"**
- No → **A** (simplest). Keep pitch, kill roll → **B** (small) or **C** (flexible). Most robust, fork-level
  → **D**.

## Open questions / caveats

- **Buoyancy is NOT in the torque budget** (`RegisterAppliedForce` is called only from `AFluidDynamics`, not
  `ABuoyancy`). If buoyancy also contributes roll, force-side fixes A–E won't fully solve it and **D/G**
  become more attractive. **Close this first** to narrow the choice.
- The `pop-up-2` autopilot is **bistable** — verify with 3× repeats per config, not single runs. The
  torque/force decompositions are deterministic and trustworthy; peak-yaw/excursion are not.
- Whether the per-actor application multiplies the intended board-wide magnitude (~N bottom actors apply the
  same board-wide force) — check the coefficient's tuning assumption before consolidating.

## Diagnostic recipe (how this was measured)

- Run `pop-up-2` on `Surfing_infinite_wave` with `surf.debug.flags 'torque'` + `surf.debug.actors 'Shared'`.
- **Capture via the stdout pipe, not `GoneSurfing.log`** — the Stop-hook editor relaunch clobbers the log,
  but `-game`'s `-stdout` survives: `RunGameAndCollectLogs ... | grep -aE "TORQUE \[...\]"`.
- The heavy map crash-loops headless on a nav ensure ~most launches; just retry.
- `T[roll/pitch/yaw]` is decomposed about `board.forwards/left/up`; `dragRail` / `dragBottom_waveMassFlow`
  are the roll-dominant categories; `waveRollSin` is logged on the TOTAL line.

## Related

- [planing-redirect.md](planing-redirect.md) — the pitch-plane velocity redirect; share its engine machinery
  for option D.
- [carve-grip-via-redirect.md](carve-grip-via-redirect.md) — the carve, which is downstream of this roll.
- [wave-mass-flow-drag.md](wave-mass-flow-drag.md) — the force being decoupled.
- [wave-penetration-resistance.md](wave-penetration-resistance.md) — the glide-through "wall" drag (purpose 1).
- [barrel-glide-through-bug.md](barrel-glide-through-bug.md) — the glide-through problem wave-mass drag fixes.
- [lateral-turn-world-relative-roll.md](lateral-turn-world-relative-roll.md) — the downstream carve driver.
