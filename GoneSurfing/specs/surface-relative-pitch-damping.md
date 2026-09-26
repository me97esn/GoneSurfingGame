# Spec: Surface-relative pitch damping

> **STATUS (2026-06-25): proposed → implementing.** Supersedes
> [weight-gated-pitch-damping.md](weight-gated-pitch-damping.md). Generalizes that mechanism (extra
> pitch damping on nose-down rotation, gated by weight-forward / nose-deeper-than-tail) to work on **any
> surface shape** — flat water *and* a steep breaking-wave face — by keying the damping on the board's
> pitch **relative to the local water surface** instead of flat-water proxies.

## Overview

Make the board's pitch **track the local water surface** by damping the pitch-axis angular velocity
**asymmetrically**:

- rotating **away** from surface alignment → **more** damping (board can't dive into / overshoot the surface),
- rotating **toward** surface alignment → **less** damping (board pitches freely to follow the surface).

The "surface" is whatever the wave data says locally — flat, gentle, or near-vertical. The single signal
driving it is `ASharedCalculations::waveRelativePitchSin` (board bottom vs local wave-surface normal, in the
pitch plane; 0 = flush/tracking).

## Motivation

Two symptoms, one root cause — **the nose doesn't track the water surface**:

1. **Hang-ten nose-dive** (flat water): rider walks weight forward, nose digs in.
   ([weight-gated-pitch-damping.md](weight-gated-pitch-damping.md))
2. **Wave-face plow** (steep water): climbing a steepening face, the board can't pitch up fast enough — the
   front buoyancies bury to ~100 % while the back sit at ~35 %, then it over-rotates and buries the tail.
   Measured on `hard_turn_towards_the_wave`: `waveRelativePitchSin` swings −0.33 (nose-down/front-plow) →
   +0.48 (nose-up/back-plow) and never settles near 0. The pitch loop is **too slow to track** (laggy in)
   **and underdamped** (overshoots out).

The original weight-gated design *named this exact gap* — its Open Question 5:

> *"The conceptually right signal is 'pitch relative to the wave surface'… the wetted-asymmetry gate
> `(front.amountWetted − back.amountWetted)` is in effect a **proxy** for board-relative-to-water-surface
> pitch… A direct geometric measurement (angle between `board.up` and the local wave-surface normal) could
> replace the proxy if precision becomes important."*

That direct geometric measurement now exists: `waveRelativePitchSin`. The wetted-diff proxy breaks on a
steep face (the front reads more-wetted when plowing a near-vertical wall regardless of alignment); the
geometric signal does not. This spec replaces the proxy + weight gate with the real thing.

## Design

### Signal

`waveRelativePitchSin` ∈ [−1, 1] — board pitch vs the local water surface (already computed in
`SharedCalculations::calculateAll` from `board.up` / `board.forwards` vs the wave normal):

- `< 0` → nose below the surface plane (nose-down / plowing in)
- `≈ 0` → bottom flush with the surface (tracking)
- `> 0` → nose above the surface plane (nose-up / overshooting)

Each board has two SCs (front/back). The project feeds the **average** of their `waveRelativePitchSin` as
the board-wide misalignment.

### Toward vs away

The board's local pitch angular velocity is `W.Y_local` (engine convention: `< 0` = nose-down rotation,
verified 2026-06-02). Misalignment and rotation have the **same sign** exactly when the board is rotating
*away* from the surface:

```
movingAway  ⟺  W.Y_local · waveRelativePitchSin > 0
```

This auto-flips on each side of the surface — no separate cases:
- nose below (`sin<0`), rotating further down (`W.Y<0`) → away → damp.
- nose below (`sin<0`), rotating up toward flush (`W.Y>0`) → toward → free.
- nose above (`sin>0`), rotating further up (`W.Y>0`) → away → damp.
- nose above (`sin>0`), rotating back down toward flush (`W.Y<0`) → toward → free.

### Damping law (engine)

Per pitch tick, the Y-axis damping becomes directional:

```cpp
const bool  movingAway = (angularVelocityLocalSpace.Y * CVars::PitchMisalignment > 0.0f);
const float yExtra     = movingAway ?  CVars::AngularDampingYAwayExtra
                                    : -CVars::AngularDampingYTowardReduction;
const float yDamp      = FMath::Clamp(CVars::AngularDampingY + yExtra, 0.0f, 1.0f);
angularVelocityLocalSpace.Y *= 1 - yDamp;
```

- `AngularDampingYAwayExtra` (≥0) — added when rotating away → strong brake on diving/overshooting.
- `AngularDampingYTowardReduction` (≥0) — subtracted when rotating toward → frees up tracking (less plow).
- Base `AngularDampingY` unchanged. When the project feeds both extras as 0 (off-water, see contact gate),
  behaviour is byte-identical to current.

Sign-only (binary away/toward); magnitude-scaling by `|waveRelativePitchSin|` is deferred (Open Q1).

### Project feed (SurfboardUtils::Tick)

Mirrors the existing `RotationPitch/Yaw/Roll` and nose-down-extra pushes:

```cpp
const float pitchMisalign = 0.5f * (frontSC->waveRelativePitchSin + backSC->waveRelativePitchSin);
// Contact gate: only damp pitch when the board is actually on the water (nothing to track when airborne).
const float contactGate   = FMath::SmoothStep(0.1f, 0.5f, 0.5f*(frontSC->amountWetted + backSC->amountWetted));
PitchMisalignmentCVar->Set(pitchMisalign);
AngularDampingYAwayExtraCVar->Set(AngularDampingYAwayExtra * contactGate);
AngularDampingYTowardReductionCVar->Set(AngularDampingYTowardReduction * contactGate);
```

The away/toward sign test (in the engine) is itself the discriminator that the old weight/wetted gates
approximated — so those gates are **removed**. The only remaining project gate is the contact gate (no
water surface ⇒ no damping), which also keeps the off-water path byte-identical to current.

### What is removed / replaced

- `AngularDampingYNoseDownExtra` (engine CVar, `SurfTuningSubsystem`, `SurfboardUtils` UPROPERTY + CVar
  binding, the `weightGate`/`wettedGate`/`combinedGate` computation) → replaced by the away/toward
  mechanism. The nose-down case is the `sin<0, W.Y<0` quadrant of the general law, handled more robustly.
- The project-side `AStateTriggerAutoPilot`-independent **pitch-align restoring torque**
  ([wave-face-pitch-alignment.md](wave-face-pitch-alignment.md)) is **kept for now** behind its
  `nopitchalign` debug flag, so we can A/B whether damping alone tracks the face well enough (Open Q2). If
  yes, it is removed in a follow-up.

## Acceptance Criteria

- **AC1 — off-water no-op.** With `contactGate = 0` (airborne) or both tuning extras at 0, the engine sees
  `yExtra = 0` and behaviour is byte-identical to current.
- **AC2 — wave-face tracking.** On `hard_turn_towards_the_wave`, peak `|waveRelativePitchSin|` through the
  climb drops materially from the baseline (≈0.48 swing): target the front/back submersion gap to stay well
  under the current 100 %-vs-35 % — i.e. the board follows the face instead of plowing then over-rotating.
- **AC3 — hang-ten preserved.** Nose-dive on flat water is still braked (the `sin<0,W.Y<0` quadrant), visual
  pitch no worse than the weight-gated design achieved.
- **AC4 — normal surfing not degraded.** `surfing-down-the-line` (and other baselined autopilots) stay
  within snapshot tolerance, or change only in the intended tracking-improvement direction (re-approve).

## Open Questions

1. **Magnitude scaling.** ~~Binary away/toward for v1.~~ **RESOLVED (2026-06-27): the binary switch caused
   visible pitch chatter and is replaced by a smooth blend.** Measured per-tick: in steady surf the board
   holds a small *steady* misalignment (~−0.12) while `W.Y` makes tiny sign-flipping wiggles, so
   `P = W.Y·misalignment` is minuscule (~1e-3) yet flips sign every few ticks — slamming the damping
   0.05↔0.95 each flip. The engine now uses `blend = P/(|P| + PitchDampingBlendScale)` (scale 0.1): the
   extra fades to ~0 when `|P|` is tiny (chatter → base damping) and ramps to full only when `P` is large
   (real divergence/overshoot). After: `yDamp` is a smooth 0.11–0.48 (was only ever 0.05 or 0.95).
   See the engine commit `Pitch damping: smooth away/toward blend`.
2. **Drop the pitch-align torque?** **RESOLVED (2026-06-25): keep it.** A/B on `hard_turn_towards_the_wave`:
   with the pitch-align torque, the climb tracks at `pitchSin ≈ −0.05..−0.12` (brief −0.46 at the near-
   vertical wall); with it off (`nopitchalign`), the nose-down plow deepens to **−0.5..−0.6** sustained.
   Damping is dissipative — it kills the overshoot but can't *create* the nose-up tracking rotation. So the
   final design is PD: pitch-align torque + buoyancy = P, surface-relative asymmetric damping = D.
3. **Engine sign convention.** Verify empirically that `W.Y_local` and `waveRelativePitchSin` agree on
   "nose-down" (the product test). The damping debug log + the `pitch` flag log confirm the quadrant.
4. **Contact-gate input.** `amountWetted` avg is the first choice (sharp contact signal). If it reads low
   during clean planing (board skimming) and weakens useful damping, revisit (planing implies flush, so
   little away-rotation to damp — likely fine).
5. **Base `AngularDampingY`.** Once toward-reduction is tuned, re-check whether the base (0.15) is still the
   right symmetric floor or should drop.

## Status

- [x] Engine: `PitchMisalignment`, `AngularDampingYAwayExtra`, `AngularDampingYTowardReduction` CVars +
      directional application; remove `AngularDampingYNoseDownExtra`.
- [x] `SurfTuningSubsystem`: replace tunable; update category map.
- [x] `SurfboardUtils`: bind new CVars, feed misalignment + contact-gated extras each tick, update debug log.
- [ ] AC1 verified (off-water byte-identical).
- [x] AC2 verified (wave-face tracking on `hard_turn_towards_the_wave`): plow/overshoot −0.33/+0.48 →
      −0.12/+0.08; front/back submersion gap 100%/35% → ~100%/90%. Brief −0.46 spike only at slopeSin 0.81.
- [x] AC3 verified (hang-ten) — visually unchanged in editor (2026-06-26).
- [x] AC4 — pop-up, surf-straight, surfing-down-the-line, hang-ten visually validated good at Kp=10
      (2026-06-26); snapshot baselines regenerated headless and approved (commit d1a1c2431).
- [x] Open Q2 resolved (keep pitch-align torque — damping alone plows to −0.6).

## Related

- [weight-gated-pitch-damping.md](weight-gated-pitch-damping.md) — superseded; its Open Question 5 is this spec.
- [wave-face-pitch-alignment.md](wave-face-pitch-alignment.md) — the restoring-torque companion, kept behind
  `nopitchalign` pending Open Q2.
- `amount-wetted-split.md` — provides `amountWetted` (contact gate input).
