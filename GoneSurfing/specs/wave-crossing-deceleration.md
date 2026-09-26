# Wave-crossing deceleration — what actually slows the board

> **STATUS (2026-06-24): investigation/findings doc.** Companion to
> [barrel-glide-through-bug.md](barrel-glide-through-bug.md). That doc asks *why the board ends up on
> the wrong side of the wave*; this doc answers the narrower, prerequisite question the author raised:
> **when the board turns hard into the wave and stops, what force actually stops it — and is it
> deterministic or chance?** All numbers below come from headless replays of the
> `turn-hard-into-the-wave` autopilot (2026-06-24). Where this conflicts with earlier intuition in the
> glide-through doc, trust the dated, data-backed numbers here.

> **REFUTED (2026-06-25, later): the de-planing story below is WRONG.** The author confirmed in-editor that
> the board is **fully planing the entire time, until long after it has already passed to the far side** —
> `AmountPlaning` only *lags* (its decay caps how fast it can drop), so the "planing 0.80 → 0.00" trace
> below is the indicator catching up *after* the crossing, not a cause. The board crosses **while fully
> planing** (full speed, full lift). So this is neither a deceleration nor a planing-loss problem. The
> `waveHullImpact` term proposed to fix it was [removed](wave-hull-normal-impact.md). Investigation restarted
> — do not re-derive a de-planing / speed-preservation theory from the numbers below.

> **CORRECTION (2026-06-25): the board falls BEHIND the crest — it is not pushed over.** Measured with a
> new crest-relative signal (`SharedCalculations::signedDistanceToCrest`: signed distance to the wave crest
> along `waveBackDirection`; >0 = behind the crest / back side, <0 = front face). World frame (author):
> **+X = back of wave, wave breaks toward −X, +Y = down the line.** The earlier force-budget "what pushes
> it over / restoring" framing below is **superseded** by this. Key trace through the crossing
> (`hard_turn_towards_the_wave`, the renamed autopilot):
>
> | planing | signedDistanceToCrest | speed | |
> |---|---|---|---|
> | 0.80 | −150 (front face) | ~500 | on the face, ~1.5 m in front of the crest |
> | 0.55 | ≈0 (at crest) | ~130 | de-planing |
> | 0.25 | +100 (behind crest) | ~150 | now on the back side |
> | 0.00 | +250 (far side) | ~34 | de-planed, left behind |
>
> **The board's absolute X *decreases* the whole time — it never moves +X over the top.** It crosses to
> the far side by **de-planing (planing 0.8→0), losing speed (500→34), so the −X-moving crest outruns it**
> and it ends up behind the wave. This is "getting left behind," not "punching/being pushed through."
>
> Consequences (correcting claims below):
> - **NOT a restoring/wall-force problem.** A `wavePenetrationDrag`-style wall would slow the board *more*
>   and make it fall behind *faster* — wrong direction. The "add a cross-flow/wall drag" fix direction in
>   this doc and in [barrel-glide-through-bug.md](barrel-glide-through-bug.md) is **wrong for this case.**
> - **`waveMassFlowDrag` is HELPING, not a culprit.** It pushes −X (with the wave), helping the board keep
>   up. (My intermediate "waveMassFlowDrag is the dominant over-push" claim was a sign error — it points
>   toward the front/breaking side, which is the keep-up direction.)
> - **The lever is speed/planing preservation through the turn.** The hard carve bleeds too much speed
>   (the anti-slip Yaw-Hydrofoil + linear/angular damping quantified below), the board de-planes, and the
>   wave leaves it. Keep it planing / restore drive after the apex so it stays with the crest. Adding drag
>   is the opposite of what's needed.
> - **Status of the three shipped forces:** slope-thrust front-face gate — keep (it prevents the *other*
>   failure: slope-thrust driving the board over the top). Pitch-align — mild help (keeps the hull presented
>   to the face). Hull-normal impact ([wave-hull-normal-impact.md](wave-hull-normal-impact.md)) — **largely
>   ineffective here** (it's near-vertical and clamped by the `MaxVelocityZUp=500` ceiling; tuning the coef
>   0.02→0.3 did nothing); candidate to set `coef=0` until a case needs it.
> - **New permanent instrument:** `signedDistanceToCrest` / `crestWorldPosition` / `waveBackDirection` on
>   `ASharedCalculations`, plus the `crossing` debug flag (per-tick board velocity + crest-relative position).

## Reproduction

- **Level:** `Surfing_infinite_wave` (set `TEST_MAP=Surfing_infinite_wave`).
- **Autopilot:** `AStateTriggerAutoPilot` named **`turn-hard-into-the-wave`** (`StateTriggerAutoPilotBP_C_5`).
  It commits a hard turn into the face and reproduces the crossing **every run** (unlike the gentle
  input-trace replay, which sat on the bifurcation and crossed ~50% of the time — see
  [input-trace-replay.md](input-trace-replay.md)). It records a trajectory CSV to
  `Saved/Tests/latest/turn-hard-into-the-wave.csv`. It has no `bAutoQuitOnComplete`, so a headless run
  hits the wrapper timeout (exit 2) **after** the CSV has already flushed — that's expected.
- **Run commands** (PowerShell, call the `.ps1` directly):
  ```powershell
  $env:TEST_MAP="Surfing_infinite_wave"
  # force breakdown (include rails!) :
  & ...\RunGameAndCollectLogs.ps1 -Argument "state,buoyancy,lift,thrust,drag,abs_velocity:bottom,rail,fin,Buoyancy,SharedCalc,nose,tail:turn-hard-into-the-wave" -TimeoutSeconds 120
  # engine damping V-before/after (decisive for damping vs force split) :
  & ...\RunGameAndCollectLogs.ps1 -Argument "damping,state,abs_velocity:SurfboardUtils,SharedCalc:turn-hard-into-the-wave" -TimeoutSeconds 120
  ```

## What happens (kinematics)

The board pops up, builds to ~520–550 cm/s, carves hard into the face, decelerates to ~40–55 cm/s near
the crest, de-planes, and ends up on the back of the wave.

- **It is a nose-first drive, not a broadside slide.** The relative water flow is ~99% **along**
  `board.forwards` (anti-parallel — water meets the nose). Both `board.forwards` and the board's own
  velocity are ~99% aligned at speed. (See the `relVelAlongMag` caveat below — the log value *looks*
  like it says "perpendicular"; it doesn't.)
- **Penetration is the board's own momentum.** It travels ~5 m into the wave under its own ~500 cm/s
  velocity; the wave's water is slower in that direction (abs water vel ≈ 150 cm/s behind the board)
  and the wave's mass-flow force actually points the *same* way (it assists entry).
- **The final crossing to the back is the wave moving past the board.** Near the crest the board is
  nearly stationary (~40 cm/s, X fixed to ±15 cm for ~2.5 s) while `amountUnderWater` drops 1.0 → 0.3 —
  the wave propagates out from under it and deposits it on the far side. This low-speed tip-over is the
  metastable point where the original 50/50 lived.

## What slows the board (the core finding)

Measured two ways on the `turn-hard-into-the-wave` decel (523 → 273 cm/s):

### 1. Damping vs. forces (from the engine `V before/after damping` log)

| Source | Speed removed | Share |
|---|---|---|
| Engine custom linear damping | ~111 cm/s | **~44%** |
| Fluid forces + gravity | ~139 cm/s | **~56%** |

### 2. Which forces (rails-included sum, projected onto the velocity; negative = decelerating)

| Force term | along-velocity | Notes |
|---|---|---|
| **Yaw Hydrofoil (anti-slip)** | **−13245** | dominant; v²-scaled lift resisting sideways slip |
| **Lateral Turn** | **−9592** | second |
| Bottom Hydrofoil (up) | −1028 | minor |
| Fin drag | ≈0 | broken (un-normalized basis, see glide-through doc) |
| Rail drag | ≈0 | negligible |
| `waveMassFlowDrag` | **+210** | slightly *propulsive* (follows the wave flow) |
| `Slope Thrust` | **+2668** | actively *propelling* down-the-line throughout |
| Buoyancy / Bottom Lift | 0 | purely vertical |

**It is not the drag.** Every drag term (rail/fin/wave-mass-flow/wave-penetration) is ≈0 or even pushes
the board *along* its path. The deceleration is the **anti-slip hydrofoil lift** (Yaw Hydrofoil +
Lateral Turn) reacting against the board's sideways slip as it carves, plus the engine's linear damping.

### 3. Timing / causality (the subtle part)

**Planing stays saturated at its max (0.800) through the entire main deceleration** (523 → ~247 cm/s,
i.e. >50% of the speed). Planing only begins to fall once the board is *already* slow (~224 cm/s):

```
speed=522 planing=0.800   speed=466 planing=0.800   speed=304 planing=0.800
speed=247 planing=0.800   speed=224 planing=0.790 ← planing first drops here
speed=131 planing=0.720
```

Consequences:
- **Planing reduction lags the deceleration — it's a consequence of slowing, not a cause.** Planing is
  speed/dynamic-pressure dependent; it pins at 0.8 while fast and falls when the board can't sustain it.
- **The forward `(1−planing)` damping does NOT initiate the slowdown.** At planing 0.8 its coefficient
  is `SurfboardForwardsDamping × (1−0.8) = 0.005 × 0.2 = 0.001` (negligible). It only un-gates *after*
  planing drops — a late-stage finisher (≈247 → 55 cm/s), not the cause.
- **The damping doing the work at full planing is the sideways axis.** Logged per-axis coefficients at
  the decel: `DampingLocalX/Y/Z = 0.001 / 0.050 / 0.014`. The **sideways** term (0.050) is 50× the
  forward one and is **not planing-gated**; it grabs the board's growing slip velocity as it carves
  (per-tick damping rises from ~1 cm/s when aligned to ~5–9 cm/s once slipping).

**Chain of events:** cruise (aligned, planing 0.8, damping minimal, slope-thrust ≈ resistance) → hard
turn → sideways slip grows → anti-slip *forces* + *sideways* damping shed >50% of speed **at full
planing** → board slows below the planing threshold → planing drops → forward `(1−planing)` damping and
un-attenuating bottom drag finish the slowdown.

## Is it deterministic? (yes)

Not chance: the speed decay is smooth and monotonic, it reproduces every run, and it is tied to measured
physical state (slip growth, then submersion `amountUnderWater → 1.0` and slope steepening
`slopeSin → 0.72`). The deceleration is correct surfing physics. The **bug** is not the deceleration —
it is the post-stall tip-over onto the *back* of the wave instead of being deflected back onto the face
(that is the glide-through doc's problem).

## Instrumentation gotchas (cost real time on this investigation)

- **`relVelAlongMag` in the drag log is deflated ~25×.** `board.forwards` (=`SharedCalculations->forwards`)
  is **un-normalized** (carries the SC actor's ~0.2 scale), and `calcBottomDrag` uses it *twice*
  (`relVelAlongBoard = boardForwards * (relWaterVel · boardForwards)`), so the logged magnitude is
  `0.04 × |relWaterVel| × cosθ`. Reading it as true cm/s makes a nose-first drive look like a
  perpendicular slide. To recover the real angle: `cosθ = relVelAlongMag / (0.04 × relativeWaterVelMag)`.
  Same un-normalized-basis family of bugs documented for the fins in
  [barrel-glide-through-bug.md](barrel-glide-through-bug.md).
- **The log frame counter `[N]` wraps (~every 10 s / ~600 frames).** It is NOT unique — summing forces
  by `][N]` across the whole file silently adds a dozen unrelated ticks. Isolate one tick by the
  millisecond timestamp window instead.
- **Damping debug:** `surf.debug.flags damping` + an actor token matching `SurfboardUtils` enables
  `p.Chaos.Solver.DebugDamping`, which makes the engine log `V before damping` / `V after damping` and
  `DampingLocalX/Y/Z` each tick. This is the only way to separate engine damping from fluid forces.
  Requires `UseCustomLinearDamping` true. `ShouldDebug` needs **both** the flag and an actor-token match.
- **Include `rail` in `surf.debug.actors`** for any force-budget sum — rails are major surfaces and were
  accidentally excluded in the first passes, which left the budget un-closable.
- Buoyancy logs as `TOTAL buoyancyForce: (0,0,Z)` — purely vertical; don't expect it to brake horizontal
  motion. It lifts the submerged board up/over.

## File references

- `Source/GoneSurfing/SurfboardUtils.cpp` — per-axis damping CVar push; `finalDamping` (forward
  `(1−planing)` + velocity-dependent), `DampingLocalX/Y/Z`, `ClampYVelocityAt`/`MaxVelocity*`.
- `Source/GoneSurfing/SurfboardUtils.h` — damping/ceiling defaults (`SurfboardForwardsDamping=0.005`,
  `SurfboardSidewaysDamping=0.05`, `VelocityDampingThreshold=500`, `VelocityDampingScale=0.0005`,
  `ClampYVelocityAt=2000`, `debugDamping`, `debugLogEveryNthTick`).
- `UnrealEngine/.../Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp` — the engine integrator that applies
  Z damping + `DampAsymmetricalLinear` + per-axis velocity clamps and emits the `V before/after damping`
  block (custom fork).
- `Source/GoneSurfing/FluidDynamics.cpp` — `calcBottomDrag` (`relVelAlongMag` deflation), Yaw Hydrofoil
  (anti-slip), Lateral Turn, Slope Thrust, `waveMassFlowDrag`, `wavePenetrationDrag`, fin terms.
- `Source/GoneSurfing/SharedCalculations.cpp` — `forwards/left/up` basis (un-normalized, line ~264).
- `Saved/Tests/latest/turn-hard-into-the-wave.csv` — recorded trajectory.

## Why the deceleration doesn't push the board back (corrected root cause, 2026-06-24)

The deceleration is real but stops the board in the **wrong place** — *past the crest* — and even there the
restoring forces lose to the board's own propulsion. Decomposed at the stall.

**Decelerating forces can't reverse the board.** The terms that do the slowing (anti-slip Yaw Hydrofoil,
Lateral Turn, the velocity-dependent drags, the linear damping) are all **dissipative** — they oppose
whatever velocity exists and go to **zero at rest**. They bring the board to a stop; they cannot push it
back. Reversing requires a *restoring* force that acts even at rest.

**Restoring forces DO exist — `waveMassFlowDrag` is the main one.** Net force at the stall, projected onto
the wave normal (`+` = toward the other side / crossing, `−` = restoring back onto the wave):

| Force | on wave-normal | |
|---|---|---|
| **Slope Thrust** | **+39903** | drives it through (dominant) |
| Wave-Slope Gravity | +973 | toward crossing (board has crested) |
| Yaw Hydrofoil | +46 | ~0 (board ~stopped) |
| wavePenetrationDrag | ~0 | not firing (board too slow) |
| Lateral Turn | −2387 | restoring |
| Rail drag | −3865 | restoring |
| **waveMassFlowDrag** | **−10372** | restoring (biggest restoring term) |
| Buoyancy | 0 | vertical |
| **NET** | **≈ +24297** | **toward the other side** |

`waveMassFlowDrag` keys on the **water's** velocity, not the board's, so unlike the other drags it does
**not** vanish at the stall — it's a genuine restoring force (~−10.4k, back onto the wave). The total
restoring set is ~−16.6k. It is simply **overwhelmed**.

**What overwhelms it: the slope-thrust propulsion, driving the board down the *reversed* slope.**
`Slope Thrust = forwards · k · (slopeDown · forwards)` always drives the board **down the local slope**.
Once the board has **crested**, the local downhill flips to point over the back: at the stall
`boardWideSlopeDown = (0.17, 0, −0.03)` (+X) and +X is the crossing direction (`waveNormalHoriz`,
`Wave-Slope Gravity force = (+98,…)`). So slope thrust (+39903) and gravity (+973) both drive the board
**down the back-slope = through**, ~2.4× the restoring drag. The propulsion is **not gated on whether the
board has crested** — it drives down whatever slope is beneath it.

**On the nose direction:** the "turn hard into the wave points the nose through the wave" framing is
*imprecise*. `board.forwards` swings ~300° during the turn (`(-0.96,0.25) → (0,1) → (0.88,0.40) →
(-0.83,-0.56)`); the board is essentially spinning and nearly stopped at the stall, so the nose direction
there is incidental. The causal driver is the **slope** direction (downhill reversed after cresting) plus
the **unconditional slope-thrust propulsion**, not where the nose happens to point.

### Source of the dominant force: the PASSIVE SLOPE THRUST term

The +39903 is not a stray force — it is the deliberate **passive slope-thrust** drive
([FluidDynamics.cpp:1318-1361](../Source/GoneSurfing/FluidDynamics.cpp#L1318), restored by
`specs/passive-slope-thrust.md`):

```
slopeThrustForce = boardFwdUnit * (slopeThrustCoefficient * amountWetted * slopeGate
                                   * (slopeAlongFwdDrive + redirectDrive))
```

- **Setting: `slopeThrustCoefficient = 40000`** — central config in
  [SurfTuningSubsystem.h:156](../Source/GoneSurfing/SurfTuningSubsystem.h#L156), pushed into each
  FluidDynamics actor at init ([FluidDynamics.cpp:47](../Source/GoneSurfing/FluidDynamics.cpp#L47)).
  Fires on all ~10 bottom hydrofoil actors at coef 40000 → that's the +39903 sum.
- Related knobs: `slopeThrustMinSlopeSin = 0.12` (deadzone: off on near-flat water between waves),
  `slopeThrustFinRedirect = 0.5` (couples the *lateral* board-left slope component into forward drive).
- **It only gates on `slopeSin > 0.12` and `amountWetted`** — there is **no gate for "front face vs.
  back of the crest."** Once the board is over the lip the slope is still present and the hull still
  wetted, so it drives down the back at full strength.
- Two design choices make it unable to stop the crossing:
  - **drive-only clamp** `slopeAlongFwdDrive = max(0, slopeAlongFwd)` ([line 1352](../Source/GoneSurfing/FluidDynamics.cpp#L1352))
    prevents *reverse* thrust (a prior fix for "~60kN reverse thrust braking 1000→100 cm/s during a
    carve up the face") — but the over-the-crest motion is *forward/down-slope*, which the clamp allows.
  - **`redirectDrive`** ([line 1353-1360](../Source/GoneSurfing/FluidDynamics.cpp#L1353)) keeps it
    driving even when the nose points up-face, by design — removing the natural stall.

### Fix leverage (upstream)

1. **DECIDED — gate slope-thrust to the front face via a hard-coded wave-travel direction.** The waves
   always travel the same world direction, so the front (rideable) face is always on a known side. Gate
   the passive slope-thrust so it fires only when the board is on the front face and suppress it on the
   back / over the crest — directly removing the +39903 that drives the board through. Implementation
   notes:
   - **Use a hard-coded world direction, not the per-tick local slope.** The data shows
     `boardWideSlopeDown` flips sign repeatedly through the breaking wave (−0.35 on the steep face,
     +0.17 at the stall, oscillating between) — a local-slope sign gate would chatter. A fixed
     wave-travel/normal vector (from the `InfiniteWaveManager` / crest-line config) is stable.
   - Gate condition: front face ⇔ the board is on the travel-facing side of the crest (or, equivalently,
     the drive direction has a positive component along the fixed wave-travel direction). Suppress
     (zero or ramp down) otherwise.
   - This is a *propulsion* gate; it's independent of, and complementary to, the cross-flow/wall drag in
     #2. It's the higher-leverage of the two because it removes the dominant term in the net.
2. **Stop the board before it crests** — resist penetration *on the front face* (the cross-flow/wall drag
   from [barrel-glide-through-bug.md](barrel-glide-through-bug.md)) so it stalls low on the face where
   down-slope still points back onto the wave. Adding restoring drag at the crest alone won't do it —
   there's already ~16.6k of it and it loses to the propulsion. Secondary to #1.

## Implemented (2026-06-24): front-face gate on passive slope thrust

Fix #1 shipped. A `frontGate` multiplies the slope-thrust force, computed from the board-wide down-slope
projected onto a hard-coded world direction:

```cpp
// FluidDynamics.cpp, passive slope-thrust block
float frontGate = 1.0f;
if (!slopeThrustFrontFaceDir.IsNearlyZero()) {
    const FVector slopeDownHoriz = FVector(boardWideSlopeDown.X, boardWideSlopeDown.Y, 0).GetSafeNormal();
    const float frontFaceAlign = slopeDownHoriz | slopeThrustFrontFaceDir.GetSafeNormal();
    frontGate = FMath::SmoothStep(-0.2f, 0.2f, frontFaceAlign);
}
slopeThrustForce = boardFwdUnit * (coef * amountWetted * slopeGate * frontGate * (slopeAlongFwdDrive + redirectDrive));
```

- New tunable `slopeThrustFrontFaceDir` (default `(-1,0,0)`, the measured front-face fall-line) on
  [SurfTuningSubsystem.h](../Source/GoneSurfing/SurfTuningSubsystem.h) + mirror on
  [FluidDynamics.h](../Source/GoneSurfing/FluidDynamics.h); `(0,0,0)` disables the gate (legacy behaviour).
- `frontGate` added to the `Slope Thrust` debug log.

**Verification (headless `turn-hard-into-the-wave` + `surfing-down-the-line`):**
- Compiles; gate fires as designed — `frontGate = 0.000` on ~43% of `turn-hard` ticks (the back of the
  crest), `1.000` on the front.
- **`turn-hard-into-the-wave`:** the board is **no longer powered over the crest**. Instead of the old
  +X excursion to the far side, it continues −X/+Y (down the line) and stalls at ~10 cm/s (vs ~40 before).
  The over-the-crest slope-thrust drive is removed.
- **`surfing-down-the-line` (regression):** healthy — `frontGate` 1.0 for ~83% of the ride, board surges
  to 660 cm/s and sustains 300–600 cm/s at planing 0.8. Normal propulsion preserved.
- Flat-water autopilots (`surf-straight`, `hang-ten`, `top-turn`) are unaffected: `slopeSin < 0.12` there,
  so slope thrust is already deadzone-gated off.

## Open questions / next steps

- Re-approve snapshot baselines for `turn-hard-into-the-wave` and `surfing-down-the-line` once the new
  trajectories are accepted (`Tests/Approve.ps1`).
- `slopeThrustFrontFaceDir` is hard-coded in the tuning subsystem; if the wave orientation ever changes,
  source it from the `InfiniteWaveManager` crest-line instead. Also consider tuning the `(-0.2, 0.2)`
  smoothstep band — the gate is off ~17% of a normal ride (transitions); tightening it could recover a
  little propulsion, but speed is already healthy.
- After the gate, the `turn-hard` board *stalls* rather than cleanly resuming surfing — separate feel
  question (the hard turn legitimately bleeds the speed); the crossing bug itself is resolved.
- Worth revisiting whether the anti-slip Yaw Hydrofoil / sideways damping carry *too much* of the carve
  energy (the board nearly stops dead in a hard turn) once the fin/basis-normalization fixes land — they
  change the slip dynamics this analysis depends on.
