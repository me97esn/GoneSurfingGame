# Spec: Per-actor water velocity for the contact forces (feel the lip's core, not the fringe)

## Status
- [x] Spec drafted (2026-07-22)
- [x] Phase 1 — `actorWaterVelocity` sampled per FluidDynamics actor + `PerActorWaterVelocityBlend`
  knob (default 0 = current board-wide behavior) blending it into the contact forces
- [x] A/B probe on `top-turn` lip window (0.18 s, comparable rides):
  | | blend 0 | blend 1 |
  |---|---|---|
  | waveMassFlow \|F\| / Fin / Fz | 44.4k / −36.6k / −2.1k | 56.8k / −47.2k / **−11.9k** |
  | railFlow \|F\| | 18.0k | 21.0k |
  | combined shoreward push | ~51k | **~65k (+26%)** |
  | combined downward push | ~3k | **~16k (~5×)** |
  The headline isn't the +26% magnitude — it's the **downward component**: the nose actors' local
  samples carry the lip's falling-water vz that the board-midline average washed out, so the lip now
  pushes DOWN on the nose. The budget also under-states the nose concentration (it averages all 10
  bottom actors; only the nose pair sits in the core), and concentrating the force at the nose is
  itself the felt change (pitch response). No instability observed in the probe ride.
- [ ] Player feel pass → bake blend default
- [ ] Phase 2 (optional) — per-actor relative velocity via `GetPhysicsLinearVelocityAtPoint` (a nose
  SWINGING into the lip has extra closing speed the component velocity can't see)

## Motivation (measured, 2026-07-22)

After the registration fix ([wave-data-velocity-registration.md](wave-data-velocity-registration.md)),
the lip's water genuinely reaches the board — but the measured lip push during top-turn lip contact is
~51k shoreward for ~0.2 s (≈ half board weight, ≈ 90 cm/s Δv against a 1000 cm/s ride): perceptible,
tiny. Root cause of the smallness: **all forces consume water velocity sampled at the two SC positions
(board midline)**. During lip contact the SC reads the fringe (200–260 cm/s) while the lip's core
(500–700 cm/s, wavescan-measured) sits at the nose's own position. Since flow forces go with v², the
core carries ~4–7× the push the fringe delivers.

Wave HEIGHT is already per-actor (each FD actor samples `calculateWaveLocationAndNormalAuto` at its
own position → `waterColumnAbove`, `actorWetted` — see per-actor-wetting.md). Only velocity is still
SC-centralized. Cost of extending: ~2–3 µs per call × 15 actors = ~45 µs/frame (~0.3% of 60 fps);
empirically the `wavescan` diagnostic ran 66+66 samples/tick without measurable impact. The
centralization is retired premature optimization (user-confirmed).

## History that must be respected

Board-wide averaging was introduced DELIBERATELY (wave-mass-flow-drag.md: "eliminate front/back
per-actor sampling asymmetry"; per-actor-vs-board-wide-sampling.md: per-actor magnitude differences
between symmetric L/R pairs inject yaw-torque noise). **Plausible reinterpretation: much of that
front/back asymmetry WAS the height/velocity misregistration** (fixed 2026-07-22) — the front and back
SCs sampled a shifted velocity field, manufacturing artificial differences. With registration fixed,
per-actor sampling deserves a re-trial — but behind a blend knob, and NOT for every force:

| force | sampling | why |
|---|---|---|
| waveMassFlowDrag (bottom) | **blend → per-actor** | contact force; the lip push |
| rail flowDrag | **blend → per-actor** | contact force |
| wavePenetration wall | **blend → per-actor** (relative form) | THE nose-hits-lip force; already "per-actor by design" in spirit |
| waveMassThrust | board-wide (unchanged) | its own comment: symmetric-pair yaw-noise sensitivity |
| lift/hydrofoil/relativeWaterVelocity family | board-wide (unchanged) | board-wide phenomena per per-actor-vs-board-wide-sampling.md |

## Design (phase 1)

- `AFluidDynamics::actorWaterVelocity` (world), sampled in the same per-tick block as the per-actor
  height sample: `waveVelocity->calculateWaveVelocity(actorPos, sharedCalculations->lastTileAdjustedFrame)`
  rotated by the wave actor's quat — the exact SC call pattern/transform, so blend=1 differs from the
  SC value only by WHERE it samples. Per-actor temporal smoothing with the SC's
  `enableVelocitySmoothing`/`velocitySmoothingFactor` (striping artifacts must not return at actor
  granularity).
- `PerActorWaterVelocityBlend` (Tuning|WaveMass, 0..1, **default 0 = bit-identical current behavior**):
  - flow drags: `flowWaterVel = lerp(boardWideAbsWaterVel, actorWaterVelocity, blend)` (magnitude AND
    direction).
  - penetration: `effRel = lerp(scRelativeWaterVelocity, actorWaterVelocity − scComponentVelocity, blend)`.
- Watch item at blend 1: L/R symmetric-pair noise → spurious yaw/roll. The wave-mass decouples
  (`waveMassFlowYawDecouple`, `wavePenetrationYawDecouple`, roll decouple/CoM routing) already strip
  the torque channels where this bites — a large part of why the risk is lower than in the
  pre-decouple era. If residual noise shows, a mid blend (0.5) or per-actor smoothing tau is the lever.

## Acceptance / probe

- AC1: blend 0 reproduces current behavior bit-for-bit (default; snapshot suite unchanged).
- AC2 (the point): top-turn lip-window budget at blend 1 shows the lip push scaling toward the core's
  v² (expect ~2–5× the blend-0 ~51k, depending on how deep the nose actors sit in the core band).
- AC3: no new yaw/roll instability down-the-line (straight-ride autopilots stay clean, 2×).

## Related
- [wave-data-velocity-registration.md](wave-data-velocity-registration.md) — made per-actor viable.
- specs/per-actor-wetting.md — the height-side precedent (same shape: per-actor value + compensation).
- specs/per-actor-vs-board-wide-sampling.md — the principle this spec selectively relaxes.
- specs/wave-mass-flow-drag.md, wave-penetration-resistance.md — the consumers.
