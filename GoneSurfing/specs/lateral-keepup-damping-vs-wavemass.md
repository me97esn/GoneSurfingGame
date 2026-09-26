# Lateral keep-up — sideways damping over-throttles the wave-mass push

> **STATUS (2026-07-17): findings + proposal, no fix committed.** Companion to
> [submerged-downline-glide.md](submerged-downline-glide.md) (the *vertical* half),
> [wave-crossing-deceleration.md](wave-crossing-deceleration.md) and
> [barrel-glide-through-bug.md](barrel-glide-through-bug.md). This is the **horizontal** half: on the
> `phone-2026-07-17-14-51-09` glide, the board drifts shoreward **slower than the crest**, so the wave slowly
> rolls over it. Root cause: the `waveMassFlowDrag` push (which should keep the board with the wave) is
> over-throttled by the sideways linear damping. Verified by A/B; the fix is a co-tune, not committed yet.

## Problem (the horizontal half — separate from the submersion)

Cross-shore ("into/out-of-wave"), the board holds its down-the-line ride but drifts shoreward a bit **slower
than the crest**, so it gradually falls onto the back of the wave ("the wave rolls over the board"). This is
distinct from the vertical submersion problem in [submerged-downline-glide.md](submerged-downline-glide.md):
here it is a **lateral velocity** deficit, and changing it does **not** change the submersion (confirmed below).

## Diagnosis

- `waveMassFlowDrag` is the dominant cross-shore force (~−60 k/tick), pointing **shoreward** — the same way
  the crest moves — i.e. it is the *keep-up* force. It uses **absolute** water velocity, so it does not vanish
  when the board and wave move together.
- The **sideways linear damping** (`SurfboardSidewaysDamping = 0.0975`, ~10× the forward axis, **not**
  planing-gated) eats most of that push, so the board's cross-shore velocity is small and it can't match the
  crest.
- Net: huge push, huge damping, sluggish lateral motion, board lags the crest.

## Verification (A/B, headless replay of `14-51-09`, `surf.debug.flags 'crossing'`)

Metric = **rel(board − crest) cross-shore velocity** (>0 = board falls behind / wave rolls over; ~0 = keeps up).
Board velocity from board position (clean); crest from the independent world-anchored crest tracker.

| sideways damping | Vboard_cross | Vcrest_cross | **rel(b−c)** | down-line | submersion |
|---|---:|---:|---:|---:|---:|
| **0.0975 (baseline)** | −34 | −65 | **+31 (lags → wave rolls over)** | +473 | 43 |
| **0.0488 (halved)** | −169 | −108 | **−61 (leads → overshoots)** | +333 | 43 |

Conclusions:
1. **Reducing the sideways damping fixes keep-up — confirmed.** The board went from lagging the crest (+31) to
   leading it (−61); cross-shore velocity −34 → −169.
2. **Halving overshoots** — the zero-lag point (rel ≈ 0) interpolates to **~0.08**. To run damping lower than
   that, the push must be reduced too (see fix).
3. **Submersion is unchanged (43 → 43)** — horizontal and vertical are independent; this is not the fix for
   the submerged glide.
4. **Side effect:** down-line speed dropped (473 → 333) — less damping routes more energy cross-shore.
5. **The response is non-linear** (~5× cross-shore velocity for a 2× damping cut, and the down-line drop is
   part of it) — the axes are coupled, so the idealized `v = F_flow / dampingCoef` "proportional-scaling is a
   no-op" is only approximate; in the real coupled system reducing both push and damping is a legitimate lever.

## Proposed fix (not committed — decide the tuning)

- **Reduce the sideways damping** so the board keeps up. Damping-only lands the zero-lag point at ~0.08 but
  keeps the violent `waveMassFlowDrag` push and costs some down-the-line speed.
- **Preferred (author):** reduce the sideways damping *more* than the zero-lag point **and** reduce
  `waveMassFlowDragCoefficient`, so the board keeps up without overshoot and with a gentler push. Rationale:
  the wave-mass force is very large and is suspected of adding **unwanted behaviour when the board is not
  perpendicular to the flow** (author) — shrinking it should help beyond just keep-up. Co-tune the two so
  `rel ≈ 0` while preserving down-line speed.
- Consider **planing-/geometry-gating** the sideways damping (it is currently un-gated), so it damps genuine
  slip without throttling the intended cross-shore keep-up drift.

## Open questions

- The exact `SurfboardSidewaysDamping` value and `waveMassFlowDragCoefficient` reduction (co-tune for rel ≈ 0).
- The **down-line-speed tradeoff** — verify a keep-up tune doesn't bleed too much down-the-line drive.
- Whether the sideways damping should be gated rather than globally lowered.
- Re-check on other traces / the `turn-hard-into-the-wave` autopilot (the effect is modest on `14-51-09` —
  rel only +31; it may bite harder with stronger input).

## Tooling

- `Tests/AnalyzeCrossing.ps1` + `surf.debug.flags 'crossing'` → the crossing log carries `bVel` (board world
  velocity) and `crestCross` (independent crest cross-shore position). Project both on the resolved cross-shore
  axis `backDir` and take the slope for the keep-up metric `rel(board − crest)`.
- `SurfboardSidewaysDamping` lives in [SurfTuningSubsystem.h](../Source/GoneSurfing/SurfTuningSubsystem.h); it
  is re-pushed to the engine `DampingLocalY` CVar every tick from the tuning value, so a runtime CVar override
  does not stick — change the tuning value and rebuild (or set it via the on-screen tuning HUD).

## Caveats

- PC replay diverges from the on-device ride; the A/B trajectories differ in absolute terms (different x-range)
  but the **rel(board − crest)** metric is the robust comparison.
- Related: [submerged-downline-glide.md](submerged-downline-glide.md), [wave-crossing-deceleration.md](wave-crossing-deceleration.md),
  [wave-mass-flow-drag.md](wave-mass-flow-drag.md), [barrel-glide-through-bug.md](barrel-glide-through-bug.md).
