# Submerged down-the-line glide — the glide-through is VERTICAL, not horizontal

> **STATUS (2026-07-17): investigation/findings doc.** Companion to
> [barrel-glide-through-bug.md](barrel-glide-through-bug.md), [wave-crossing-deceleration.md](wave-crossing-deceleration.md)
> and [planing-redirect.md](planing-redirect.md). It answers a specific precondition question the author
> raised before proposing solutions: **when the board "glides through" the wave on the `phone-2026-07-17-14-51-09`
> trace, is it driving horizontally into/through the wave, or is it something else?** Answer: it is riding
> **submerged (30–50 cm under the surface) while travelling down the line** — a *depth* condition, not a
> horizontal charge into the face. The force budget (below) then locates it: the board floats at a **submerged
> hydrostatic equilibrium (~40 cm)** because the bottom hull's dynamic lift self-cancels (Bernoulli suction ≈
> upthrust) so it can't plane the board onto the surface. No solution is proposed here — this reframes and
> localises the problem for the next step (the bottom vertical balance).

## The question

Earlier crest-crossing analysis (see [analyze-crest-crossing] in auto-memory / `Tests/AnalyzeCrossing.ps1`)
showed the `14-51-09` replay stays **submerged the whole ride** (submersion +12…+80 cm, `amountUnderWater`
0.1–1.0, 0 of 486 ticks with the board above the surface) — it glides *through* the wave body rather than
*over* the crest. Before designing a fix, two things had to be pinned down:

1. **Is the board mostly facing into the wave, or mostly down the line?** (Accounting for the 90° mesh
   rotation — `board.forwards` is local +Y; the physics forward axis, not the raw yaw, is authoritative.)
2. **Is it the board penetrating a ~stationary wave, or a moving wave gliding over a ~stationary board —
   measured ONLY in the into-the-wave (cross-shore) direction** (down-the-line speed is irrelevant to this).

## Reproduction

- **Level:** `Surfing_infinite_wave` (`TEST_MAP=Surfing_infinite_wave`).
- **Replay:** `-ReplayTrace=phone-2026-07-17-14-51-09.csv -usefixedtimestep -fps=60`, flag `surf.debug.flags 'crossing'`.
- **One-command:** `Tests/AnalyzeCrossing.ps1 -Trace phone-2026-07-17-14-51-09`.
- The `crossing` flag makes `ASharedCalculations` log a per-tick `CROSSING [...]` line carrying: WaterController
  `SecondsElapsed`/`CurrentFrame`, board pos, `signedDistanceToCrest`, `submersion`, `underW`, `waveN`, `fwdH`
  (board.forwards horizontal, normalized), `bVel` (board world velocity), `crestCross` (independent crest
  cross-shore world position), `vCrossFace`/`vDownLine`, planing, slopeSin.

**Axes** (from the `InfiniteWaveManager` tile geometry — tiles step by `(YOffsetPerActor, ActorSpacing) =
(-1155, 2265)`, the down-line axis; perpendicular = cross-shore):
- **into-the-wave (cross-shore) `backDir` = (0.891, 0.454)** — heading 27°, toward the back/crest.
- **down-the-line tangent = (-0.454, 0.891)** — heading 117°.

## Findings

### Q1 — the nose is DOWN THE LINE, not into the wave

Measured with `fwdH` (the true `board.forwards`, normalized — not the `yaw+90°` estimate, though that agreed):

| metric | value |
|---|---|
| mean \|nose · into-wave\| | **0.40** |
| mean \|nose · down-line\| | **0.83** |
| down-line-dominant ticks | **408 / 476 (86 %)** |

The nose points down the line for ~86 % of the ride. The only into-wave swing is the brief catch/pop-up turn.
`fwdH ≈ velocity heading` throughout (little skid) — the board goes roughly where it points.

### Q2 — into-wave motion is small for BOTH board and crest; they move TOGETHER

Independent crest tracker (fine, wide global-peak scan of the wave surface along `backDir`, not the
board-relative hill-climb), least-squares velocities over the ride (6.0–13.6 s):

| | into-wave velocity (cross-shore) | down-the-line velocity |
|---|---|---|
| **Board** | **−43 cm/s** (net; oscillates around small) | **+480 cm/s** |
| **Crest** (independent phase velocity) | **−60 cm/s** | — |
| **Board relative to crest** | **+17 cm/s** | — |

- The board's velocity **into the wave is tiny** (~−43 cm/s) and *shoreward* (−`backDir`) — it isn't even
  driving toward the back. It is ~**10× smaller** than the down-the-line speed (+480 cm/s).
- The crest's independently-measured cross-shore phase velocity (−60 cm/s) is also small and **confirms** the
  board-relative estimate (−43) from the earlier pass.
- They move **nearly in lockstep**: the crest drifts shoreward only ~17 cm/s faster than the board, so the
  board falls behind it very slowly (~1.3 m over the whole 7.6 s window).

## Conclusion — it is neither of the two hypotheses; the glide-through is a DEPTH condition

- **NOT "a fast board penetrating a stationary wave"** — the board's into-wave velocity is negligible.
- **NOT "a fast wave gliding over a stationary board"** — the board isn't stationary cross-shore; it moves
  *with* the crest, and the crest itself is slow.

Cross-shore, **board and crest drift shoreward slowly and nearly together** while the board's real motion is
**down the line** (~480 cm/s), nose pointed down the line. Combined with the submersion result, the board is
**riding 30–50 cm below the surface, along the face, down the line** — it holds its cross-shore station on the
crest but sits *under* the water rather than *on* it. The "glide-through" is **vertical (it rides submerged),
not horizontal (it is not charging into the wall).**

## Consequence for the existing fixes

- **The planing redirect ([planing-redirect.md](planing-redirect.md)) premise does not match this case.** It is
  designed for **nose-into-the-wave** penetration and rotates the world velocity up the face, gated on nose
  submersion. Here the nose is **down the line** and there is **~no velocity into the wave** to redirect. The
  gate (nose `amountUnderWater`) may fire, but the geometry it assumes (velocity aimed into the face) isn't
  present — so it is not the right lever for this failure.
- The horizontal "wall" family ([wave-penetration-resistance.md](wave-penetration-resistance.md),
  hull-normal impact) targets cross-face charging — also not this case (cross-face velocity is tiny).
- This is consistent with [wave-crossing-deceleration.md](wave-crossing-deceleration.md)'s corrected view that
  the board is not "punched/pushed through" horizontally.

## Force budget (2026-07-17) — why it rides submerged

Measured with the `torque` force-budget dump (per-category force decomposed into `Fin` into-wave+/out−,
`Fdown` down-line, `Fz` up+/down−; now includes `buoyancy` + `gravity`). Window 6.5–13.0 s, per-tick
averages, both SC actors summed. Run: `surf.debug.flags 'crossing,torque'`, `surf.debug.actors 'SharedCalculations'`.

### Horizontal (into/out-of-wave) — net is OUT (restoring), NOT into the wave

| term | Fin/tick (into+ / out−) |
|---|---:|
| dragBottom_waveMassFlow | **−59836 (OUT)** |
| dragRailFlow | **−25408 (OUT)** |
| dragBottom_waveMassThrust | +6147 |
| dragBottom / wavePenetration / slopeThrust | +4624 / +4130 / +2421 |
| railLift | −1852 (net, but OSCILLATES — see below) |
| **NET** | **−76450 (OUT)** |

- Dominated by the wave-mass flow drags (`waveMassFlowDrag` uses **absolute** water velocity, so it does NOT
  vanish when board ≈ wave — it stays the biggest term and points **shoreward/out**, the keep-up/restoring
  direction, consistent with [wave-crossing-deceleration.md](wave-crossing-deceleration.md)).
- Net is out ~78 % of ticks; it flips **into** the wave (~22 %) only when the flow drag momentarily lulls.
- **`railLift` oscillates into/out (into ~40 % of ticks, ±20k swings) instead of steady-into — suspected
  bug, tracked in auto-memory [[rail-lift-oscillates-not-steady-into-wave]]. Not the cause here (small vs the
  flows).** So the horizontal budget is net-restoring; the glide-through is not a horizontal in-pull.

### Vertical — the board floats at a SUBMERGED hydrostatic equilibrium (~40–48 cm under)

Binned by submersion depth (per-tick `Fz`, up+/down−):

| submersion (cm) | 20–32 | 32–40 | 40–48 | 48–70 |
|---|---:|---:|---:|---:|
| buoyancy | 113010 | 164845 | 198775 | 285674 |
| gravity | ~−250000 (≈ constant) | | | |
| bottom suction (down) | −2532 | −4015 | −3997 | −7180 |
| bottom upthrust (up) | +2660 | +3377 | +3438 | +5745 |
| **NET Fz** | **−85241** | −37895 | **−8156** | **+59636** |

- **NET vertical is strongly DOWN at shallow depth, ≈0 at ~40–48 cm, and UP when deeper** → a stable
  equilibrium at ~40 cm submerged, exactly where the board rides.
- **Driver = buoyancy** (∝ `amountUnderWater`): it grows steeply with depth (113k → 286k) while gravity is
  constant, so it only balances the weight once the board is ~40 cm under.
- **The bottom hull provides ~zero net vertical dynamic lift** — the downward Bernoulli suction (−3997 @ 40 cm)
  ≈ cancels the upthrust (+3438). It tracks that way at every depth (suction slightly wins). A planing hull
  should net *up* to ride on the surface; here it nets ~0, so nothing lifts the board out and it settles to
  buoyant equilibrium **under** the water.

**Conclusion:** submerged because (1) buoyancy alone balances gravity only ~40 cm deep, and (2) the bottom's
dynamic lift self-cancels (suction ≈ upthrust), so the hull can't plane the board up onto the surface. The
eventual fix targets the **bottom vertical balance** (suction too strong and/or upthrust too weak). *(Gravity
is logged once/tick via a pointer guard; ~18 % of ticks miss it, so the standalone `gravity` row reads low —
the `NET` row is logged directly and is authoritative. Engine Z-damping / `MaxVelocityZUp` ceiling are not yet
in the budget.)*

## Instrumentation / gotchas (carried from prior investigations)

- **Timing:** read the WaterController `SecondsElapsed`/`CurrentFrame` (logged in the crossing line), NOT
  engine-frame/60. `CurrentFrame` wraps (~24/s), so pair it with `SecondsElapsed`. See [[timing-wave-events-in-replays]].
- **`signedDistanceToCrest`** scans along the **runtime-resolved** cross-shore axis (from the tile geometry)
  with an unsaturated hill-climb — the old fixed +X axis was ~34° off on this diagonally-tiled wave and
  saturated at ±400. See [[analyze-crest-crossing]].
- **Do NOT compare heights across wave-sampling functions:** `waveHeightAndNormal` (crest scan) and
  `calculateWaveLocationAndNormal` (submersion) use **different vertical frames**; only same-function
  comparisons are valid. `submersion` uses `calculateWaveLocationAndNormal` throughout and is trustworthy.
- **Un-normalized SC basis:** `this->left/forwards/up` carry the ~0.2 actor scale; normalize at point of use
  (`fwdH` does). The recurring bug behind many "forces don't fire" findings — see
  [barrel-glide-through-bug.md](barrel-glide-through-bug.md).
- **Replay divergence:** PC replay is chaotic vs the on-device/editor ride, so exact timing won't match to
  <0.1 s. The qualitative geometry (nose down-line, tiny into-wave velocity, submerged) is the robust result.

## File / tooling references

- [Source/GoneSurfing/SharedCalculations.cpp](../Source/GoneSurfing/SharedCalculations.cpp) — the `crossing`
  debug block (crest scan, `submersion`, `fwdH`, `bVel`, `crestCross`); resolved cross-shore axis.
- `Tests/AnalyzeCrossing.ps1` — one-command replay + crossing/through-vs-over analysis.
- `Tests/InputTraces/phone-2026-07-17-14-51-09.csv` — the trace under study.
- Related: [barrel-glide-through-bug.md](barrel-glide-through-bug.md), [wave-crossing-deceleration.md](wave-crossing-deceleration.md),
  [planing-redirect.md](planing-redirect.md), [wave-penetration-resistance.md](wave-penetration-resistance.md).
