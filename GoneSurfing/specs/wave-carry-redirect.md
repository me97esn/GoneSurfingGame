# Spec: Wave-carry redirect — keep-up via the engine velocity redirect

> **STATUS (2026-07-19): LANDED, default-on.** `WaveCarryRedirectRate = 1.0`, `WaveCarryTargetCrossSpeed = 60`
> baked as the tuning-subsystem defaults after an in-editor feel pass (player-verified "feels good"). The
> self-limiting velocity-deficit gate is what made it stable; it fixes keep-up while preserving the down-line
> ride. Follow-up (optional): retry shrinking `waveMassFlowDrag` / sideways damping *by feel* (headless showed
> they can break the down-line ride — see the co-tune section).
>
> Third instance of the engine velocity-redirect primitive,
> after [planing-redirect.md](planing-redirect.md) (pitch plane, up-slope) and
> [carve-grip-via-redirect.md](carve-grip-via-redirect.md) (yaw plane, toward the nose). This one owns
> **horizontal keep-up**: rotate the board's velocity toward the wave's shoreward travel direction so it holds
> its station on the crest instead of the wave rolling over it — a speed-preserving direction change, replacing
> the force-push-then-damp mechanism. Companion to [lateral-keepup-damping-vs-wavemass.md](lateral-keepup-damping-vs-wavemass.md)
> and [submerged-downline-glide.md](submerged-downline-glide.md).

## Problem

On the `phone-2026-07-17-14-51-09` glide the board drifts shoreward **slower than the crest** and the wave
rolls over it (measured `rel(board−crest) cross-shore ≈ +26..+31 cm/s`). The current mechanism for keeping
the board with the wave is a **force dance** that is self-defeating:

- `waveMassFlowDrag` **pushes** the board shoreward (~−60 k/tick cross-shore),
- the **carve-grip redirect** (rate 4, target = nose = down-the-line) rotates the velocity **back toward the
  down-line heading**, deleting the shoreward component,
- the **sideways linear damping** (`SurfboardSidewaysDamping = 0.0975`) dissipates the shoreward component too.

So the push and the grip/damping fight, and the shoreward keep-up drift the push is trying to create is
removed as fast as it appears (verified: halving the damping restores keep-up — see the companion spec).

## Insight — keep-up is a velocity *direction* problem

The board already has ample speed (~470 cm/s, mostly down the line). Keeping up with the crest just needs its
velocity pointed **~7° more shoreward** (to hold the ~−60 cm/s cross-shore the crest moves at). That is a
*direction* change at constant speed — exactly what a velocity redirect does, and exactly what forces can't do
without dissipation. This is the same argument that motivated the planing and carve-grip redirects; keep-up is
the third job that belongs on the redirect side of the split, not the force side.

## Design — a horizontal redirect toward the wave's travel direction

Reuse `RotateVelocityTowardDir(V, target, phi)` in [PBDRigidsEvolutionGBF.cpp](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp)
(`Integrate`). Add a third instance:

| redirect | plane | target | gate |
|---|---|---|---|
| planing | world (climb) | up-slope `-waveSlopeDownVec` | nose `amountUnderWater` |
| carve-grip | horizontal | nose FD `forwards` | nose `amountUnderWater` |
| **wave-carry (new)** | **horizontal** | **wave travel / crest motion = `-resolvedWaveBackDirection` (shoreward)** | (v1) nose `amountUnderWater` |

- **Target = `-resolvedWaveBackDirection`** — the cross-shore axis derived at runtime from the InfiniteWaveManager
  tile geometry (`SharedCalculations::resolvedWaveBackDirection` points toward the back of the wave; negate for
  shoreward, the direction the crest moves). Horizontal, fed from the project like the other targets.
- **Speed-preserving**, clamped to no-overshoot (the primitive already does this) — it can only rotate the
  velocity *up to* shoreward, never past it, and never changes speed.
- **Order:** damping → carve-grip (horizontal) → **wave-carry (horizontal)** → planing (world/vertical) →
  ceilings. Carve-grip pulls the velocity toward the down-line nose; wave-carry pulls it toward shoreward; the
  two reach an equilibrium a few degrees shoreward of the nose — the keep-up drift. `WaveCarryRedirectRate` vs
  `CarveGripRate` sets the balance.

### What it does / doesn't do
- **Does:** rotate a few degrees of the board's down-line speed into the shoreward keep-up drift, no big force,
  no dissipation. Replaces the `waveMassFlowDrag` cross-shore push.
- **Doesn't add energy** (redirect can't) — fine: the board already has the speed.
- **Doesn't turn the nose** — the heading stays down-line while the *velocity* leads shoreward, i.e. a small
  deliberate slip. That's correct for keep-up (you track down the line but drift with the wave).

## Gating — the key open question

v1 gates on nose `amountUnderWater` (mirrors the other two, simplest). Risk: a face-only gate isn't
self-limiting, so a too-high rate rotates the velocity all the way to pure shoreward and **kills the down-line
ride** (the failure we saw when over-weakening the down-line flow). Two self-limiting options to evaluate after
v1:
1. **Velocity-deficit gate:** gate on how far the board's cross-shore velocity is *behind* the crest's
   (feed `Vcrest_cross − Vboard_cross`); →0 once matched. True keep-up controller, needs the crest velocity fed.
2. **Lag gate:** gate on `signedDistanceToCrest` (ramp up as the board falls toward/behind the crest).

### v1 result (2026-07-18) — face-gate is bang-bang, REGRESSES; moving to the velocity-deficit gate

Implemented v1 (face-gate = nose `amountUnderWater`, target = `-resolvedWaveBackDirection`) and A/B'd on
`14-51-09` at `WaveCarryRedirectRate = 1`:

| | rel(b−c) | Vboard_cross | Vdownline | down-line travel |
|---|---:|---:|---:|---:|
| baseline (off) | +26 | −34 | +488 | 3425 |
| wave-carry rate=1 (face-gate) | **+128 (worse)** | +14 | +377 | 2705 |

It **regressed** — keep-up worse, down-line dropped. Mechanism: the wave-carry redirect (toward shoreward) and
the carve-grip redirect (toward the down-line nose) each apply a *fixed* rate·gate·Dt rotation toward opposite
targets — a **bang-bang** fight with **no stable intermediate** (whichever rate is larger wins outright), so
it destabilizes instead of settling at the few-degrees-shoreward keep-up drift. Confirms the "face-gate isn't
self-limiting" risk. **The scaffolding (engine CVars + apply + project feed + tuning knob) is committed
default-off.** Fix = the **velocity-deficit gate (option 1)**: `gate = smoothstep(0, ramp, Vboard_cross +
WaveCarryTargetCrossSpeed)` — drive while the board's shoreward speed is below the crest's, fade to 0 as it
matches, so it settles instead of banging. Implemented next.

### Self-limiting gate result (2026-07-18) — WORKS, keep-up without killing the ride; overshoots (force side still on)

Implemented the velocity-deficit gate (`gate = clamp((boardCross + WaveCarryTargetCrossSpeed)/30, 0, 1) ×
slopeGate`, `WaveCarryTargetCrossSpeed = 60`, target dir `-resolvedWaveBackDirection`). A/B on `14-51-09`:

| | rel(b−c) | Vboard_cross | Vdownline | travel | maxspd |
|---|---:|---:|---:|---:|---:|
| baseline (off) | +26 | −34 | +488 | 3425 | 966 |
| self-limiting rate=4 | −79 | −194 | +347 | 1917 | 667 |
| **self-limiting rate=1** | **−75** | −129 | **+470** | 2782 | **963** |

- **Stable** (no v1 bang-bang divergence), and at rate=1 the board flips from lagging (+26) to leading (−75)
  while **down-line speed is preserved** (470 vs 488, max 963 vs 966) — the first keep-up mechanism that
  doesn't collapse the ride. Gate debug confirmed the sign (`carryDir=(−0.891,−0.454)` shoreward) and the
  self-limiting fade (gate 0.96 lagging → 0.41 near target).
- **Overshoots** (rel −75 / Vboard_cross −129 vs the −60 target) because `waveMassFlowDrag` + sideways damping
  are still fully on, also pushing shoreward — redundant with the redirect, so they over-drive. The overshoot
  is ~constant across rate (an equilibrium, not momentum), confirming it's the redundant force side.
- **Next:** the spec's "shrink the force side" — co-reduce `waveMassFlowDragCoefficient` +
  `SurfboardSidewaysDamping` with the redirect on, so the redirect alone controls keep-up and settles at
  rel≈0. Committed default-off (`WaveCarryRedirectRate = 0`).

### Co-tune result (2026-07-18) — force-reduction does NOT clean up the overshoot; needs feel tuning

Swept, with the redirect on (rate 1), on `14-51-09`:

| config | rel | Vbcross | Vdownline | travel | maxspd |
|---|---:|---:|---:|---:|---:|
| baseline (off) | +26 | −34 | 488 | 3425 | 966 |
| redirect r1, full forces | −75 | −129 | **470** | 2782 | **963** |
| r1 + drag 0.0005 + damp 0.04 | −77 | −230 | 257 | 1684 | 912 |
| r1 + drag 0.0005 + damp 0.0975 | −29 | −215 | 228 | 1215 | 879 |
| r1 + target 30 (full forces) | −88 | −141 | 460 | 2675 | 1062 |

Findings:
- **Reducing the sideways damping made the overshoot WORSE** (Vbcross −129→−230): the damping was *braking*
  the shoreward over-drive, not just blocking keep-up. So with the redirect providing the drive, the damping
  is now the thing preventing runaway — keep it.
- **Reducing the flow drag broke the down-line ride** (Vdownline 470→228, travel →1215): the flow drag's
  down-line component is load-bearing for down-line travel (same failure as the earlier flow-drag experiments).
- **The overshoot is robust** (rel ~−75…−88 across rate 1/4 and target 30/60) — an equilibrium of the coupled
  redirect+forces+damping system, NOT tunable via the redirect's own knobs. Lowering the target didn't help.
- **Best config = redirect + FULL forces:** stops the lag (rel +26→−75, board leads instead of the wave
  rolling over) and preserves down-line SPEED (470/963 ≈ baseline 488/966); costs ~20% down-line *travel* and
  a shoreward overshoot (board rides ahead of the crest).

**Conclusion:** the redirect is the right mechanism and *fixes keep-up while preserving down-line speed* — the
first thing to do so. But dialing the overshoot to rel≈0 with full travel is a delicate multi-parameter
balance that the headless single-run metrics can't resolve (chaotic run-to-run divergence; "rel" wanders as
the trajectory diverges). Per the same note in [carve-grip-via-redirect.md](carve-grip-via-redirect.md),
this needs **in-editor feel tuning** of `WaveCarryRedirectRate` / `WaveCarryTargetCrossSpeed` (live via the
tuning HUD), not more headless A/Bs. Scaffolding + self-limiting gate are committed default-off; the final
tune is a live pass.

## Engine fork changes ([PBDRigidsEvolutionGBF.cpp](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp))

- CVars: `p.Chaos.Solver.WaveCarryRedirectRate` (rad/s, 0=off), `WaveCarryRedirectGate` (0..1),
  `WaveCarryDirX/Y` (world horizontal target).
- Apply block in `Integrate`, horizontal plane, after carve-grip, before planing — same shape as carve-grip.

## Project changes

- **[SurfboardUtils.cpp](../Source/GoneSurfing/SurfboardUtils.cpp)** (mirror the planing/carve feed): each tick push
  `WaveCarryDirX/Y = (-sharedCalculationsFront->resolvedWaveBackDirection)` horizontal+normalized,
  `WaveCarryRedirectGate = sharedCalculationsFront->amountUnderWater`, and `WaveCarryRedirectRate` from tuning.
- **[SurfTuningSubsystem.h](../Source/GoneSurfing/SurfTuningSubsystem.h)**: `WaveCarryRedirectRate` (default 0),
  wired like `PlaningRedirectMaxAngle`.

## Risks / interactions
- **Over-carve into pure shoreward** (kills down-line) if the rate is too high / gate not self-limiting — the v1
  face-gate needs a conservative rate; escalate to a velocity-deficit gate if it over-rotates.
- **Fights carve-grip** by construction (opposite horizontal targets). Intended — the equilibrium is the keep-up
  drift — but co-tune the two rates.
- **Then shrink the force side:** once keep-up is on the redirect, reduce `waveMassFlowDragCoefficient` and
  `SurfboardSidewaysDamping` (they were only there to fake this, badly). Do that as a follow-up, not in v1.
- Composition with planing redirect (separate plane) should be safe; verify a hard turn is stable.

## Acceptance criteria
- **AC1:** With `WaveCarryRedirectRate > 0`, `rel(board−crest)` moves toward 0 on `14-51-09` (board keeps up).
- **AC2:** Down-line speed / travel are preserved (NOT the collapse the flow-drag split caused) — a few % of
  down-line speed traded for the keep-up drift is acceptable; a >30% drop is not.
- **AC3:** `WaveCarryRedirectRate = 0` reproduces current behaviour bit-for-bit (opt-in).
- **AC4:** Flat water / no wave unaffected (target ~0 or gate ~0 there).

## Test plan
1. Implement default-off; confirm inert.
2. Sweep `WaveCarryRedirectRate` on `14-51-09`; measure `rel`, `Vdownline`, down-line travel, submersion via
   `Tests/AnalyzeCrossing.ps1` + the `crossing` `bVel`/`crestCross` metric.
3. If it over-rotates, switch the gate to velocity-deficit (option 1) and re-sweep.
4. Once tuned, A/B a co-reduction of `waveMassFlowDragCoefficient` + `SurfboardSidewaysDamping`.

## Related
- [planing-redirect.md](planing-redirect.md), [carve-grip-via-redirect.md](carve-grip-via-redirect.md) — the
  shared redirect primitive.
- [lateral-keepup-damping-vs-wavemass.md](lateral-keepup-damping-vs-wavemass.md) — the keep-up problem + the
  damping A/B this replaces.
- [submerged-downline-glide.md](submerged-downline-glide.md), [wave-mass-flow-drag.md](wave-mass-flow-drag.md).
