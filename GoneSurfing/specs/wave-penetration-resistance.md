# Spec: Wave Penetration Resistance (per-actor cross-flow drag + yaw damping)

> **SUPERSEDED (2026-06-28) for the sharp-turn glide-through** by [planing-redirect.md](planing-redirect.md).
> Rather than adding a force that *resists* horizontal penetration (the cross-flow "wall" drag below), the
> redirect *rotates* the board's velocity toward the wave up-slope so it rides up the face instead of into
> it — which fixed the observed sharp-turn case (player-verified, default 2.0 rad/s). This per-actor
> cross-flow / yaw-damping approach is not currently needed; keep it as a reference if a residual sideways
> *skid* (distinct from the nose-first penetration the redirect addresses) shows up later.

> **Priority: THIRD of three specs split out from [barrel-glide-through-bug.md](barrel-glide-through-bug.md)** (after [passive-slope-thrust.md](passive-slope-thrust.md) and [fin-force-normalization.md](fin-force-normalization.md)). **Do not start this until the first two land** — the glide-through may substantially change (or resolve) once the board has propulsion and working fins, so the exact shape of any remaining penetration problem must be re-observed first. This spec is scoped from the analysis but intentionally left lighter until then.

## Bug Description

The board slides sideways through the wave to the open-water side, when the wall of water should resist it. After restoring propulsion and fins, if penetration still occurs, the model is missing a force that resists the board moving *across* the water flow / into the wave face.

## Current Behavior / Root Cause (from the investigation)

- The wave is a **height field**: every force derives from `waterColumnAbove = waveSurfaceZ(actorXY) − actorZ` (vertical). There is no force along the wave's *horizontal* normal — no "wall" to resist horizontal penetration. The `waveNormal` horizontal direction is never turned into a force (only `slopeSin` magnitude + roll + downhill are used).
- The board's motion relative to the water at the escape is almost entirely **perpendicular to `board.forwards`** (`relVelAlong ≈ 9` vs `relWaterVel ≈ 264 cm/s`) — i.e. it slides sideways through the water. Bottom drag uses only the *along-forwards* projection and is planing-gated, so it discards the perpendicular slip.
- `waveMassFlowDrag` (the closest existing "wall" term) uses **board-wide** velocity/direction, so it is yaw-neutral by design and blind to differential (rotational) velocity → provides **zero yaw damping**. Applied via `AddImpulseAtLocation` at each actor's location, but with equal board-wide force → torques cancel.
- Net: nothing resists yaw/skid via *local* velocity. Once de-planed the board swings/slides freely.

## Expected Behavior

A fast lateral water flow against the hull (and the board's own sideways velocity through the water) is resisted, so the board cannot slide broadside through the wave face. The resistance should also naturally damp yaw/skid (front and back resisting their own local lateral velocity → a restoring couple).

## Proposed Approach (to refine after specs 1-2 land)

Add a **per-actor cross-flow drag**: each bottom/rail actor resists the component of *its own local* relative velocity that is **perpendicular to `board.forwards`** (`relWaterVelPerp`), directed to oppose it, **ungated by planing** (the failure happens precisely when planing → 0). Because it's per-actor and local-velocity-based:
- it resists the broadside *translation* slip (the missing wall force), AND
- it damps *yaw*: during a yaw/skid the front and back actors have opposite lateral velocities, so per-actor drag produces a restoring couple — unlike the board-wide `waveMassFlowDrag`.

`calcBottomDrag` already computes `relWaterVel` and the forward projection; this is the perpendicular component it currently throws away.

Alternative / complementary: a force along the wave's **horizontal normal** proportional to the board's inward velocity component and column depth (a true hydrostatic "wall"). Decide between (or combine) these after re-observing the post-propulsion behaviour.

## Constraints

- **NFR1**: Must not brake a cleanly-aligned planing run, where `relWaterVelPerp` is small by construction. Tune the coefficient so straight trim is unaffected.
- **NFR2**: Per-actor symmetry — L/R pairs must produce equal magnitudes at symmetric geometry so the term doesn't inject a spurious steady yaw (the reason `waveMassFlowDrag` went board-wide; here we *want* yaw response, but only to genuine differential velocity, not to sampling noise).

## Acceptance Criteria (provisional)

- **AC1**: With propulsion + fins fixed, the board surfing `surfing-down-the-line` is deflected back onto the face instead of gliding through at the escape window.
- **AC2**: Steps 0-4 of the snapshot do not regress (straight/clean phases unaffected).
- **AC3**: A deliberate hard slip damps out (yaw/skid returns toward aligned) rather than running away.

## Re-observation (2026-06-17) — precondition satisfied; penetration re-confirmed as a high-speed forward charge

Re-observed via **phone input-trace replay** (`InputReplayAutoPilot` driving a recorded tilt trace into
the wave face — a propelled, off-centre approach, unlike the no-input autopilot coast). Prerequisites
are in: passive slope thrust + fin-redirect provide propulsion, fin drag is tamed (`finDragCoefficient
0.0002`), fins normalized. Three clean phases at the escape (gameSeconds ≈ 8.7):

1. **Gains speed on the face** — accelerates **409 → 1020 cm/s** while deep on the steep face
   (`amountUnderWater` 0.4–0.9, `slopeSin` 0.46–0.52). The slope-thrust + fin-redirect work.
2. **Buries inward then launches over the crest** — `amountUnderWater` spikes to ~0.98 (board drives
   *into* the face) then collapses **0.9 → 0.000 while speed is still ~835 cm/s** → the board leaves
   the water surface and flies over the crest to the flat back.
3. **Lands on the back and coasts to a stop** — `slopeSin ≈ 0.03`, no slope to drive it, stalls.

**Decisive geometry: `sinSlip(boardWide) ≈ 0.04–0.18` at the launch** — the velocity is nearly *along*
board.forwards (a forward charge with the nose pointed into the wave), **not** the sideways slide of the
old no-propulsion coast (`sinSlip ≈ −1.0`). So this is a forward punch-through, and the redirect fix
made it *fast* enough to launch clean over the crest — we traded "stall" for "launch through." Either
way nothing resists the board crossing the face.

### Decision: horizontal-normal "wall" drag (NOT per-actor cross-flow drag)

Because the slip is ~0, a cross-flow drag (perpendicular to board.forwards) would barely engage — the
perpendicular component is tiny. The resisted motion is **across the wave face** (along its horizontal
normal), regardless of slip angle. So the chosen term is a per-bottom-actor drag along the wave's
**horizontal normal**, opposing the board's velocity component across the face, **ungated by planing**:

```
waveNormalHoriz = horizontal(SC.waveNormal), normalized
vAcrossFace     = (-relativeWaterVelocity) · waveNormalHoriz      // board's cross-face speed thru water
wallAmount      = wavePenetrationCoefficient × effectiveWaterHeight × vAcrossFace²
wavePenetrationDrag = -sign(vAcrossFace) × waveNormalHoriz × clamp(wallAmount, 0, maxDragAmount)
```

Symmetric (resists crossing in OR out) so it needs no front/back knowledge; tangential down-the-line
motion has `vAcrossFace ≈ 0` → unaffected (NFR1). Per-actor (front/back SC) → also damps yaw/skid.
Implemented in `calcDragForce` VE_Down, summed with the other bottom drags. New tunable
`wavePenetrationCoefficient` (Tuning|WaveMass, starting 0.0005 — tune live).

## Status

- [x] **Prerequisites landed** (passive slope thrust, fin normalization, fin-redirect, fin-drag retune)
- [x] Re-confirmed & characterised post-propulsion: high-speed forward charge over the crest (`sinSlip≈0`)
- [x] Chose horizontal-normal wall force over cross-flow drag (slip is ~0 → cross-flow useless)
- [x] Implement (first cut — `wavePenetrationCoefficient`, default 0.0005, tunable)
- [ ] Tune coefficient & verify AC1 (board deflected back onto face, no launch-through)
- [ ] AC2 (steps 0–4 / clean trim unaffected — NFR1), AC3 (hard slip damps out)
- [ ] Snapshot re-approved

## Related

- [barrel-glide-through-bug.md](barrel-glide-through-bug.md) — parent investigation; full force map and data.
- [passive-slope-thrust.md](passive-slope-thrust.md), [fin-force-normalization.md](fin-force-normalization.md) — prerequisites.
- [wave-mass-flow-drag.md](wave-mass-flow-drag.md) — the board-wide term this complements (and explains why it can't damp yaw).
