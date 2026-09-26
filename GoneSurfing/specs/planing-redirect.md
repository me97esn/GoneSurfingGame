# Spec: Planing redirect (rocker lift via velocity rotation toward wave up-slope)

## Status
- [x] Prototype implemented (engine + project)
- [x] Sign verified (climbs, doesn't dive — world-frame v2)
- [x] Wired through the tuning subsystem (`PlaningRedirectMaxAngle`, rad/s)
- [x] Tuned against `surfing_down_the_line_then_sharp_turn_left` — **θ=2.0 is the sweet spot**:
      reliably stops the glide-through (3/3 runs ride, first-turn peak underwater ~0.76 vs 1.00 off)
      while the board still carves the sharp turn. θ≥3 over-grips and stops the board turning.
- [x] Default set to **2.0** (on). The autopilot is bistable bury-vs-ride, so results came from 3× repeats
      per angle, not single runs.
- [ ] Open: handling tradeoff — the redirect also stiffens yaw (it's grip on the velocity); at high θ the
      board won't turn. Best paired with the over-carve fix ([[small-weight-shift-sharp-turn]]) rather than
      cranking θ. Re-tune θ if the carve gain changes.

## Problem
On a hard turn into the wave the board drives nose-first into the face and **glides through it
fully submerged** instead of riding up the surface. Root analysis in
[barrel-glide-through-bug.md](barrel-glide-through-bug.md): once buried, the per-axis local linear
damping ([PBDRigidsEvolutionGBF.cpp](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp) `DampAsymmetricalLinear`) steers velocity toward the board's
**chord-forward** axis — and the velocity is already there — so nothing curves the path up the face.
Drag only slows; it can't redirect. The redirect has to come from a **lift**-style (perpendicular) term.

## Idea
A planing hull climbs because its rocker / angle of attack bends its momentum up the face. Model that by
**rotating the board's velocity toward the up-the-face direction**, at a rate gated by how buried the nose
is. Speed-preserving rotation; gravity handles the climb's energy cost.

### v1 (board-local tilt) — REJECTED, see sweep below
First attempt applied the forward(X)/vertical(Z) local-linear damping in a frame rotated up by θ toward
**board-local +Z**. Swept θ ∈ {0, 0.3, 0.5, 0.8} on the sharp-turn autopilot (2026-06-28):
- θ=0.3 ≈ θ=0 (within ~10 cm / 0.1 uw run-to-run variance) — no real effect.
- **θ=0.5 actively worse**: board stayed `uw=1.0`, `vz` went to **−191 cm/s** (sank), pitch steepened to
  **−57°**, z dropped 294→253.
- Non-monotonic. Root cause: it tilts toward **board-local up**, but during a hard turn the board pitches
  to −35°…−57° nose-down, so local-up points forward-and-down in the world → the redirect shoves velocity
  **down**, burying the board (feedback: sink → more nose-down → push down harder).

### v2 (world up-slope) — current
"Up the face" is **wave-relative**, not board-relative. Rotate the **world** velocity toward
`up-slope = -waveSlopeDownVec` (which `SharedCalculations` already computes), so it stays correct at any
board attitude. On flat water `waveSlopeDownVec ≈ 0`, so the redirect self-disables (no face to climb).

## Gate — NOSE submersion, not board-wide
θ = `PlaningRedirectMaxAngle` × `gate`, where **gate = the front (nose) SC `amountUnderWater`**. The
redirect is about the *nose* lifting up the face, so it must read nose submersion, not a board-wide
average. `amountUnderWater` is the only **live** depth signal (see [[submersion-gates-comparison]]):
`amountWetted` is pegged at 1.0 in practice, and planing is velocity-only. Its ~1 m gradient is a feature
here — strong lift when the nose is buried (`→1`), fading as the nose rides up (`→0.25`), so it self-limits
and won't launch the board on flat water.

## Implementation (v2)
- **Engine** ([PBDRigidsEvolutionGBF.cpp](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp)), in `Integrate` after `DampAsymmetricalLinear`:
  - CVars `p.Chaos.Solver.PlaningRedirectMaxAngle` (rad/s, default 0 = off), `PlaningRedirectGate` (0..1),
    and `PlaningRedirectUpX/Y/Z` (world up-slope dir), all fed by the project.
  - Rotate world `V` toward `Up` by `phi = MaxAngle * Gate * Dt`, in the plane of (V, Up), clamped to the
    angle-to-Up (no overshoot). Speed-preserving. Skips when `Up ≈ 0` (flat water) or speed ≈ 0.
- **Project** ([SurfboardUtils.cpp](../Source/GoneSurfing/SurfboardUtils.cpp)): each tick push
  `Gate = sharedCalculationsFront->amountUnderWater` and `Up = -sharedCalculationsFront->waveSlopeDownVec`.
  (`sharedCalculationsFront` already exists on the actor.) Gate logged under `damping` as `planingRedirectGate(noseUW)`.

## Tuning / test
`MaxAngle` is now a **rotation rate (rad/s)**, not a static tilt — much smaller numbers. Sweep e.g.:
```
EXTRA_EXECCMDS="p.Chaos.Solver.PlaningRedirectMaxAngle 3"   # ~0.05 rad/tick at 60 Hz, full gate
```
Compare `surfing_down_the_line_then_sharp_turn_left` vs θ=0: does z track the face surface (uw stays <1.0,
no `vz` sink) instead of burying? Then regression on a non-turning autopilot.

## Open questions
- Right θ magnitude, and whether the gate needs a deadzone/smoothstep (only engage above some nose depth)
  so it doesn't add lift during normal planing.
- Interaction with the yaw/over-carve problem ([[small-weight-shift-sharp-turn]]): this fixes the pitch
  plane (climb), not the heading. Best paired with a heading fix so the board climbs *along* the face, not
  *into* it.
