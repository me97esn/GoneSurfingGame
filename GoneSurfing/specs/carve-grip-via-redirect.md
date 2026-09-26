# Spec: Carve grip via the engine velocity redirect (retire the force-based anti-slip)

## Status
- [x] Spec drafted
- [x] **Decisions resolved (2026-06-28):**
  - **A** = velocity-redirect (rotate toward heading), keep a small `DampingLocalY` baseline.
  - **B** = **re-derive** the carve-coupling forward thrust into the propulsion model (do *not* keep the
    yaw-hydrofoil/fin forward term as-is). This is its own sub-task — see "Phase 2" below.
  - **C** = grip aligns velocity to heading over ~0.2–0.3 s (not instant).
  - **D** = break-loose emergent from reduced grip (no explicit slip threshold yet).
  - **E** = redirect target = **nose FD actor `forwards`** (rocker-pitched), horizontal projection.
- [x] **Phase 1 — mechanism, default-off**: engine grip redirect + project feed + tuning knobs +
  `AntiSlipForceScale`. Confirmed inert at defaults (CarveGripRate 0 at runtime, glide-through still fixed).
- [x] **Phase 2 — tuned + default-on (player-verified "works very well")**: `CarveGripRate = 4`,
  `AntiSlipForceScale = 0` (force anti-slip off — the grip redirect carries slip-resistance). Baked as the
  tuning-subsystem defaults.
  - **Decision B turned out unnecessary**: `AntiSlipForceScale` scales only the anti-slip *side* component,
    so the forward/carve-coupling thrust survives at scale 0 — no re-derivation into propulsion needed.
- [ ] **Open**: land the [[small-weight-shift-sharp-turn]] over-carve fix and re-check (grip × carve gain
  multiply). Snapshot regression / broader feel pass.

## Overview

A carve is two separate jobs:

1. **Rotate the heading** — yaw the nose to point where you want to go.
2. **Make velocity follow the heading** — anti-slip *grip* (kill the sideways component so the board
   tracks its nose instead of skidding).

The codebase currently does job #2 **twice, in two different models**, which is redundant and the source
of real bugs:

- **Kinematic**: per-axis sideways linear damping `DampingLocalY` (`SurfboardSidewaysDamping = 0.05`) in
  the Chaos integrator (`DampAsymmetricalLinear`, [PBDRigidsEvolutionGBF.cpp](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp)).
  Robust, angle-independent — in practice it already carries most of the grip.
- **Force-based**: the yaw hydrofoil (`yawHydrofoilCoefficient = 0.001`,
  [FluidDynamics.cpp:1270-1307](../Source/GoneSurfing/FluidDynamics.cpp#L1270)) and fin lift/drag
  (`finLiftMagnitude = 0.001`, `finDragCoefficient = 0.0002`). These apply a side force opposing slip.
  Fragile geometry: the fins are crippled ~350× by the un-normalized SC basis (`cos⁴ × 0.2` — see
  [fin-force-normalization.md](fin-force-normalization.md)), and we repeatedly measured them contributing
  ~0 at the moments they were supposed to grip.

This mirrors the just-shipped [planing-redirect.md](planing-redirect.md): there we made the engine
*redirect velocity* (toward the wave up-slope) do a job that adding forces couldn't do cleanly. This spec
applies the same principle to the **yaw plane**: let the engine redirect own *grip*, and reserve forces
for the jobs only forces can do.

## The clean split

| Job | Owner (proposed) |
|---|---|
| Grip / anti-slip (velocity follows heading) | **Engine velocity redirect** (yaw plane) |
| Rotate the heading (carve) | **Force/torque** — `lateralTurnForce` (kept) |
| Energy exchange — speed bleed in turns, **pumping for speed**, break-loose | **Forces** (kept) |

Why forces can't be dropped entirely: a velocity redirect *deletes/rotates* momentum — it can never *add*
energy, so it cannot pump. And it cannot rotate the heading (it only acts on linear velocity). So the
redirect handles grip; forces still turn the nose and exchange energy with the wave.

## Current state (grounded)

- **Grip**: `DampingLocalY` (0.05, constant) + yaw-hydrofoil anti-slip side force + fin lift/drag.
  - Yaw hydrofoil ([FluidDynamics.cpp:1270](../Source/GoneSurfing/FluidDynamics.cpp#L1270)):
    `yawForce = (antiSlipDir·cosSlip + forwardsUnit·|sinSlip|) · coef·v²·|sinSlip|·amountWetted·effH`.
    The **`antiSlipDir·cosSlip`** term is the anti-slip (redundant with grip damping). The
    **`forwardsUnit·|sinSlip|`** term is induced/carve-coupling *thrust* (energy). Uses unit vectors —
    geometrically sound, unlike the fins.
  - Fin lift ([FluidDynamics.cpp:879](../Source/GoneSurfing/FluidDynamics.cpp#L879)): same anti-slip +
    forward shape; `sinSlip` still carries the 0.2 basis scale → magnitude unreliable.
  - Fin drag: `cos⁴ × scale` → ~0 in practice.
- **Yaw driver**: `lateralTurnForce` ([FluidDynamics.cpp:815](../Source/GoneSurfing/FluidDynamics.cpp#L815)),
  `lateralTurnCoefficient = 20000`, keys on `worldRelativeRollSin`. High-gain (see the over-carve note in
  [[small-weight-shift-sharp-turn]]).
- **Energy**: slope thrust, bottom-hydrofoil forward thrust, pumping ([pumping.md](pumping.md)), plus the
  carve-coupling forward components above.

## Proposed design

### 1. Grip = a yaw-plane velocity redirect (the "engine redirect")

Add a redirect that rotates the board's **horizontal** velocity toward its **horizontal heading** at a
tunable rate, gated by nose submersion (`amountUnderWater` from the front SC — **not** `amountWetted`,
which is dead/pegged at 1.0, see [[submersion-gates-comparison]]).
This is the yaw-plane analog of the planing redirect (which rotates toward up-slope in the pitch plane);
**reuse/generalize the same machinery** so there is one redirect primitive — "rotate world velocity
toward a fed target direction at rate·gate·Dt, clamped to no-overshoot" — instantiated twice:

- planing redirect → target = up-slope (`-waveSlopeDownVec`), pitch/climb.
- carve grip → target = horizontal projection of the **nose FluidDynamics actor's `forwards`**, yaw/anti-slip.

Keep the two in **separate planes** (grip operates on the horizontal velocity component, planing on the
vertical) so they compose without fighting; apply grip first, then planing.

**Decision A — mechanism:** rotate-toward-heading (recommended, consistent + speed-preserving) vs simply
tuning the existing `DampingLocalY` up and deleting the forces (smaller change, but constant per-tick
damping, not slip-angle/speed aware). Recommend the redirect; keep a small `DampingLocalY` baseline.

**Decision E — target direction: nose FD `forwards`, not board/SC chord (recommended).** A surfboard with
**rocker** has its nose pitched up relative to the chord. When the board is **rolled** into a turn, that
pitched-up nose `forwards` projects horizontally **into the turn** (deflection ≈ `sin(rocker)·sin(roll)`),
so redirecting velocity toward it **aids the carve when banked** — and it's zero at neutral roll, so it
only helps when you've actually leaned. This is the real carving mechanism (follow the rolled, rockered
rail) and lets the grip redirect *contribute* turn, offloading the high-gain `lateralTurnForce`.
**Critical:** the `SharedCalculations` actors are deliberately **un-rotated** (axis-aligned to the board,
[SharedCalculations.cpp:264](../Source/GoneSurfing/SharedCalculations.cpp#L264)), so SC `forwards` is the
flat chord and carries **no** rocker — using it gives no turn-aid. The **FluidDynamics** actors *do* carry
the board's real (pitched) orientation, so the target must be read from the **nose FD actor's** `forwards`.
Verify that actor is genuinely pitched (rocker) at placement. Use its **horizontal projection** so the
vertical/climb stays owned by the planing redirect (avoid double-counting lift).

### 2. Retire the force-based anti-slip (keep the thrust part)

- **Remove the anti-slip side component** (`antiSlipDir·cosSlip`) from the yaw hydrofoil and fin lift —
  the grip redirect now owns it. Implement as tuning coefficients that can be driven to 0, not deletion,
  so it's A/B-able and revertible.
- **Keep the forward / induced-thrust component** (`forwardsUnit·|sinSlip|`) — that's energy
  (carve-coupling), which belongs to the forces side of the split. **Decision B:** keep it as-is, or
  re-derive it into the propulsion model. Note this is exactly what [fin-carve-coupling.md](fin-carve-coupling.md)
  relies on — coordinate with that spec.
- Fin drag: fold into the general bottom cross-flow story or leave at ~0; it's not load-bearing.

### 3. Keep the yaw driver and energy terms

`lateralTurnForce` (carve), slope thrust, hydrofoil forward thrust, and pumping are unchanged by this
spec. (The over-carve high-gain is a *separate* fix, [[small-weight-shift-sharp-turn]]; but it interacts —
stronger grip makes a given carve track harder, so re-tune the two together.)

### 4. Don't go on-rails — leave room to drift

Pure/maxed grip = no slide at all, which feels robotic. Cap the grip below full (rate that aligns velocity
to heading over ~a few tenths of a second, not instantly) so some slip survives, and let the **absence of
a hard kill + the forces** produce break-loose when overpowered. **Decision C:** drift amount (grip rate)
and **Decision D:** whether break-loose is purely emergent (reduced grip) or wants an explicit
slip-angle threshold that drops grip when exceeded (a "lose the rail" feel).

## Engine fork changes ([PBDRigidsEvolutionGBF.cpp](../../../UnrealEngine/Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp))

1. Generalize the planing-redirect block in `Integrate` into a small helper
   `RotateVelocityToward(V, targetWorldDir, phi)` (the rotate-in-plane, clamp-to-no-overshoot logic that
   already exists).
2. Add CVars for the carve grip: `p.Chaos.Solver.CarveGripRate` (rad/s), `p.Chaos.Solver.CarveGripGate`
   (0..1), `p.Chaos.Solver.CarveHeadingX/Y` (horizontal heading, fed by project). Apply in the **horizontal
   plane** (zero the target's Z; operate on `(V.X, V.Y)` only) so it doesn't disturb climb.
3. Order in `Integrate`: damping → **carve grip (horizontal)** → **planing redirect (vertical)** → velocity
   ceilings. Default rate 0 = off (no behavior change until tuned).
4. Optionally reduce the role of `DampingLocalY` once the grip redirect is tuned (keep a baseline).
   **NOT DONE — and reversed. See "Step 4 revisited" below.**

## Step 4 revisited (2026-09-23) — measured, and an open question

Step 4 above was never taken, and the value moved the other way: this spec records
`SurfboardSidewaysDamping = 0.05`, and `333e64ffa` ("Re-calibrate damping defaults after
frame-rate-independence fix", 2026-07-17, *after* the grip redirect baked on 2026-06-28) raised it to
**0.0975** — restoring "the old high-fps feel", for reasons unrelated to the redirect having taken
over the job. No board overrides it at any of the four tuning layers, and neither does `CarveGripRate`;
both sit at the compiled default for all five boards.

**It is not redundant.** 2x2 on `phone-2026-09-18-08-41-02` (hybrid, `-RailsUntil=2.00`, overrides at
the release tick; |slip| and speed as whole-ride aggregates over 20-28 s):

| config | \|slip\| mean | p50 | p90 | speed | down-line |
|---|---:|---:|---:|---:|---:|
| **shipped** side .0975 / grip 4 | **39.3** | 34.7 | 98.2 | **225** | **1397** |
| side .05 / grip 6 | 42.6 | 40.5 | **85.1** | 261 | 1566 |
| side .03 / grip 8 | 55.3 | 48.3 | 127.7 | 245 | 1683 |
| side **0** / grip 4 | 59.1 | 68.9 | 105.2 | 243 | 2807 |
| side .0975 / grip **0** | 46.2 | 45.6 | 83.0 | 262 | 1643 |
| side 0 / grip 0 | 54.0 | 42.2 | 108.9 | 323 | 4442 |

Removing the damping costs more slip (39.3 -> 59.1) than removing the redirect built to replace it
(39.3 -> 46.2). The old term is carrying **more** of the anti-slip job than the new one.

**These aggregates do not separate the two terms.** The cells diverge into qualitatively different
rides (down-line 1397 vs 4442), and "both off" scores *lower* mean slip than "damping off alone" —
incoherent as attribution, and the tell that the ride, not the coefficient, is setting the number. The
[[ab-video-invalid-when-trajectory-diverges]] assumption that whole-ride aggregates survive divergence
holds for phase differences, not for one cell finding a straighter line. **The trade is a feel call
and belongs on the owner's mid-ride sliders**, not in a headless sweep: both coefficients are live in
the tuning HUD and via `surf-live`.

### The case for finally doing step 4 (2026-09-23)

Step 4 has an argument behind it now, not just tidiness. **Once the board slips, the sideways damping
cannot tell "slip I should resist" from "the direction the board is travelling" — and the redirect
can.**

First, the axis convention, because the comment beside the feed
(`// It appears that Y is sideways when I debug this`) contradicts CLAUDE.md's "board.forwards is
local +Y", and the solver's frame comes from `RotationPitch/Yaw/Roll` — CVars that
`ASurfboardUtils::Tick` fills from **its own** `GetActorRotation()`, a different actor from the mesh.
Fitted in 3D against the logged `Forward Speed` over the fixture ride:

| hypothesis | mean abs error |
|---|---:|
| solver local X = mesh rotator X | 229.0 cm/s |
| **solver local X = mesh rotator Y (the nose)** | **10.1 cm/s** |

The SurfboardUtils actor is yawed 90 deg from the mesh, so its local X *is* the nose. `DampingLocalY`
genuinely is board-sideways; the comment is right and the labelling is not the bug. The forward
channel is also exonerated: `DampingLocalX` runs at **0.006** against sideways' 0.0975, and its
velocity-dependent term needs `VelocityDampingThreshold = 2000` cm/s against a board doing ~450, so
it fired **zero** times across the ride.

What actually happens is geometric. Over the stall on `phone-2026-09-22-20-26-02`:

| trace t | board sideways axis ∥ down-the-line | v along nose | v across hull | down-line |
|---|---:|---:|---:|---:|
| 3.34 | 71% | **425** | 15 | 280 |
| 3.64 | 88% | 438 | -128 | 361 |
| 3.94 | 86% | 194 | -331 | 399 |
| 4.09 | 85% | **37** | **-364** | 311 |
| 4.39 | 82% | -149 | -370 | 147 |

At t=3.34 the board goes where it points — 425 along the nose, 15 across the hull — and the damping has
almost nothing to remove. By t=4.09 that has inverted, and through the whole stall the across-hull
axis sits **82-88% parallel to the crest tangent**. So the damping is doing exactly its job, on
exactly the right axis, and deleting the ride speed, because "sideways" and "down the line" have
become nearly the same vector.

**That is the argument for the redirect owning this job.** Both terms reduce slip; they differ in
what they do with the energy. The damping destroys the across-hull component. The grip redirect
*rotates* the velocity toward the nose and conserves speed — it turns a slide back into a ride
instead of into heat. That is the project's own split (CLAUDE.md: "change direction without changing
speed" is a redirect's job, "resist / remove energy" is damping's), and anti-slip is a direction
problem.

Three layers produced the reported ride, and only the third is fixed:

1. The board stops tracking its nose — the grip terms are gated on nose `amountUnderWater`, which
   reads `0.000` through the event, so neither the carve grip nor the planing redirect is running.
2. Once slipping, the sideways damping converts ride speed into nothing. **Still true in the water.**
3. With no contact gate it kept doing it in mid-air. Fixed by `SidewaysDampingAirScale`.

Layer 1 is why layer 2 bites: the redirect that should have been rotating the velocity back onto the
nose was gated off at the same moment the damping was at full strength. So step 4 is not just
"turn the old term down" — it is *give the redirect the authority the damping currently holds*, which
also means asking whether a nose-submersion gate is the right gate for the grip at all.

What *is* settled: the shipped config is the slowest and shortest of every cell tested, and
`DampingLocalY` is **ungated by water contact**, unlike the other two local axes
(`DampingLocalZ` x `avgAmountUnderWater`, `DampingLocalX` x `(1 - avgAmountPlaning)`). On
`phone-2026-09-22-20-26-02` the board loses **266 cm/s of horizontal speed over 0.45 s while both
contact gates read 0.000** — airborne, where horizontal velocity must be constant; zeroing the
damping makes it flat (850 -> 883). That is a bug independent of the trade above, and fixing it is
the prerequisite for judging the trade. See
[replay-rails-until.md](replay-rails-until.md) for the fixture both results come from.

## Amendment: the gate (2026-09-23, owner-raised)

The gate this spec chose was never physically motivated. Its whole argument is in "1. Grip = a
yaw-plane velocity redirect" above — *"gated by nose submersion ... reuse/generalize the same
machinery so there is one redirect primitive ... instantiated twice"*. The only substantive part of
that sentence is the choice of `amountUnderWater` over the dead `amountWetted`; the *nose* half is
symmetry with the planing redirect, not physics.

The owner's objection (2026-09-23):

> the carving redirect by design only works when the nose is wetted. This means that the board can't
> do a top turn, since during a top turn the nose is lifted above the wave at a big part of the
> manuever. I understand the reasoning why carving redirect shouldn't be applied when the board is
> *completely* airborne. But if *only* the nose is not wetted but the rest of the board is, the
> carving redirect should be in effect.

That is right, and it is what the surfaces say: grip comes from the **fins and the loaded rail**,
both aft of the nose. A lifted nose over a wet tail is a board that still grips.

### Measured, on the rails fixture over `phone-2026-09-22-20-26-02`

| trace t | nose | tail | ships (nose) | board contact |
|---|---:|---:|---:|---:|
| 3.83 | 0.869 | 1.000 | 0.869 | 1.000 |
| **3.91** | **0.000** | **0.700** | **0.000** | **0.683** |
| 3.99-4.41 | 0.000 | 0.000 | 0.000 | **0.000** |
| 4.58 | 0.000 | 0.464 | 0.000 | 0.254 |
| 4.66 | 0.000 | 0.566 | 0.000 | 0.436 |

The board gate reads 0.683 exactly where the nose gate reads 0, and still reads 0 through the window
where the whole board is clear of the water — the case that *should* kill the grip. It also fades the
grip back in progressively as the tail re-engages, which the nose gate never does.

**Saturating, not a flat mean.** The 4-point buoyancy average reads **0.177** at t=3.91 because it
dilutes by the dry nose. Fins do not care about the nose. So the signal is the board-wide
`contactGate` already computed a few lines earlier — `SmoothStep(0.1, 0.5, mean(front, back))`, the
same one the pitch damping, the wave-normal damping and (since 2026-09-23) `DampingLocalY` use. One
contact definition across the whole feed.

### The crest fade is the bigger multiplier, and its un-fade excludes a top turn

`CarveGripGate = contact x gripCrestGate`, and fixing only the contact half would have been
measurably inert:

| trace t | towardSign | yawTowardWave | gripCrestGate |
|---|---:|---:|---:|
| 3.58 | -0.679 | 0.561 | 0.57 |
| 3.66 - 3.99 | -0.70 .. -0.79 | **0.000** | **0.00-0.02** |
| 4.24 | -0.884 | 0.000 | 0.07 |
| 4.58 | -0.999 | 0.000 | 0.62 |

`CarveGripTurnUnfadeRate` reads 0.561 during the snap up the face and then **0.000 for the entire
rest of the manoeuvre**, because it keys on yaw *toward* the wave and a top turn yaws back **away**
from the wave by definition. So the un-fade was built for the half of a turn that goes in, and
structurally excludes the half that comes out. Grip at t=3.91 would have been 0.683 x 0.00.

Keying the un-fade on **|yaw rate|** instead asks the question the fade actually wants — *is this
rail loaded in a turn* — and leaves the fade's validated job intact: an unloaded board drifting over
the lip has a low yaw rate and is still not escorted, which is the bottom-turn punch-through fix
(600/600, player-validated 2026-07-22).

### Result: two knobs, both baked ON (see "Conclusion" below for why the first answer was wrong)

`CarveGripContactBlend` (0 = nose, 1 = board contact) and `CarveGripTurnUnfadeSymmetric`
(0 = toward-only, 1 = |yaw rate|). Both default to **1**; **0 on each restores the pre-amendment
behaviour bit for bit**, so the A/B stays available.

The top-turn fixture (`phone-2026-09-22-20-26-02`, fish, window t=3.0-5.5):

| config | min down-line | peak slip |
|---|---:|---:|
| legacy | 150 | 95 deg |
| contact blend only | 166 | **72 deg** |
| symmetric un-fade only | 127 | 94 deg |
| both | **194** | **53 deg** |

The 30 s clean ride (`phone-2026-09-18-08-41-02`, hybrid, 562 rows, identical span):

| config | slip mean | p50 | p90 | speed | down-line |
|---|---:|---:|---:|---:|---:|
| legacy | 47.7 | 45.2 | 80.1 | 139 | 1319 |
| contact blend only | **59.6** | 50.7 | **119.9** | 139 | 1572 |
| symmetric un-fade only | **41.4** | **39.5** | **79.5** | **146** | 1539 |
| both | 59.5 | 43.3 | **152.5** | 139 | 1812 |

**They disagree, and not subtly.** The contact blend is the knob that rescues the top turn and the
knob that costs general tracking. The symmetric un-fade is ~neutral on the event and improves every
metric on the clean ride. That is the opposite of what was predicted here before the runs — the
un-fade was expected to be the risky one because it touches the validated crest fade, and it is the
safer of the two.

~~So neither is baked.~~ **Superseded** — the whole-ride half of this comparison was measuring
whitewater and flat water. See "The clean-ride numbers were measuring the wrong water" below.

A plausible reading of why the contact blend costs tracking, to test rather than to trust: the grip
target is the nose's `forwards`, and with the nose clear of the water the grip rotates the velocity
toward a heading no water is supporting — grip toward a direction the board cannot actually hold. If
that is it, the fix is not the gate but the target, and the honest version of this amendment is a
gate on the fins and the loaded rail with a target to match.

### Player verdict on the manoeuvre (2026-09-23) — BOTH

Watching the four configurations on the top-turn fixture, the owner picked **both enabled**:

> I liked the last video most, with both enabled. It redirected the nose most realistically. Even
> though the board ended up on the wrong side of the wave, that feels like the responsibility of the
> wave push, not the carve redirect.

Two things follow. The first is that on the manoeuvre this amendment exists to fix, the answer is
both knobs on, from the only judge that counts for feel. The second is a **scoping rule**: the board
finishing on the wrong side of the wave is not the grip's account. Do not tune `CarveGripRate`, the
contact blend or the crest fade to keep the board on the right side of the crest — that is the wave
push's job, and spending grip authority on it is how the two get conflated again.

### The clean-ride numbers were measuring the wrong water

The 30 s table above is **withdrawn as stated**. The owner, on the same videos:

> in the "The whole ride" videos, a big part of the ride is in the white water. How much the board
> grips on a green wave doesn't necessarily have to be the same as in white water, so only the clean
> part of the ride should be compared.

That is correct and it undercuts the comparison: those aggregates average green-face grip together
with bore behaviour, where a separate damping family (`WhitewaterDampingRate`, `BrokenYawCapScale`)
owns the board and the carve grip is not the mechanism under test. A slip number dominated by
whitewater ticks says nothing about whether the gate change is right.

**How wrong the water was:** over that 30 s ride the wave-geometry service calls
**49% whitewater, 39% flat, 13% pocket**. So 87% of the comparison was water that is not a green
wave, and the green-face portion is ~3.8 s of it.

Re-measured per zone, slip computed from `fwdH` and `bVel` on the same `CROSSING` line so the zone
and the geometry come from one sample:

**Green face** (pocket/shoulder, `broken < 0.01`) — the only water that judges this:

| config | ticks | slip mean | slip p90 | speed |
|---|---:|---:|---:|---:|
| legacy | 226 | 12.0 | 31.7 | 625 |
| contact blend only | 228 | 11.4 | 31.8 | 626 |
| symmetric un-fade only | 226 | 11.9 | 32.0 | 626 |
| **both** | 230 | **9.7** | **21.1** | 610 |

**Whitewater** — not the mechanism under test (`WhitewaterDampingRate` / `BrokenYawCapScale` own the
board there):

| config | ticks | slip mean | slip p90 | speed |
|---|---:|---:|---:|---:|
| legacy | 741 | 44.6 | 78.6 | 224 |
| contact blend only | 689 | 33.2 | 77.3 | 233 |
| symmetric un-fade only | 800 | 43.6 | 75.7 | 215 |
| both | 805 | 45.3 | 83.5 | 210 |

**Flat**, off the wave (33-106 ticks, too thin to weigh): legacy 54.8, contact 82.5, symmetric 32.3,
both 63.0.

### Conclusion: both baked ON

On the face, **both** is the best configuration — mean slip 12.0 -> 9.7 and p90 31.7 -> 21.1, a third
off the tail, for 2.4% less speed (625 -> 610). Neither knob alone moves the green-face numbers at
all (11.4 / 11.9 against 12.0), the same interaction the event fixture showed: they are two
multiplicands of one gate. That agrees with the player verdict, so both defaults are 1.

The regression that briefly held them at 0 was real arithmetic on the wrong sample. Caveat kept: the
green-face window is ~226 ticks (3.8 s) per run, one run each — the right water, a thin slice of it.

**Method rule this establishes.** Never judge a grip, drag or slip change on a whole-ride aggregate.
Split by wave zone first and report the green face separately: a ride can be half whitewater without
looking like it, and the carve grip is not the mechanism that owns the board in a bore.

**Still open.** The planing redirect keeps the nose signal on purpose. And with the grip gate open
more of the time, Decision D ("break-loose emergent from reduced grip") has less to be emergent
from — worth deciding whether break-loose should come from somewhere explicit instead.

## Project changes

- **[SurfboardUtils.cpp](../Source/GoneSurfing/SurfboardUtils.cpp)** + `.h`: add a reference to the **nose
  FluidDynamics actor** (the SCs won't do — see Decision E). Each tick feed
  `CarveHeadingX/Y = noseFD->forwards`, horizontal (zero Z) and normalized;
  `CarveGripGate = sharedCalculationsFront->amountUnderWater` (the only live submersion signal — `amountWetted`
  is dead, [[submersion-gates-comparison]]); and push `CarveGripRate` from a new tuning knob.
  (Mirror the planing-redirect feed already in this file.)
- **[FluidDynamics.cpp](../Source/GoneSurfing/FluidDynamics.cpp)**: gate the **anti-slip side component**
  of the yaw hydrofoil (line ~1286) and fin lift (line ~895) by a new `antiSlipForceScale` (default 1.0 =
  current; drive to 0 once grip lands). Leave the forward component on.
- **[SurfTuningSubsystem.h](../Source/GoneSurfing/SurfTuningSubsystem.h)** + `RefreshFromTuningSubsystem`:
  add `CarveGripRate` (default 0), `AntiSlipForceScale` (default 1). Both wired like
  `PlaningRedirectMaxAngle`.

## Risks / interactions

- **Over-carve coupling**: grip and the carve gain (`lateralTurnCoefficient`) multiply each other's effect.
  Tune together; ideally land the [[small-weight-shift-sharp-turn]] fix in the same pass.
- **Pumping must survive** ([pumping.md](pumping.md)) — the redirect must not eat the pump's speed gain.
  Verify the pump still accelerates the board.
- **Carve-coupling thrust** ([fin-carve-coupling.md](fin-carve-coupling.md)) depends on the forward
  component we're keeping — don't remove it with the anti-slip part.
- **Composition with planing redirect** — keep planes separate; verify a hard bottom-turn (both active) is
  stable.
- **On-rails feel** — Decisions C/D; needs in-editor feel, not just CSVs.

## Acceptance criteria

- **AC1**: With the grip redirect on and the force anti-slip scaled to 0, the board grips at least as well
  as today on a clean carve (sideways slip ≤ current at matched speed/angle), measured board-frame
  `alongLeft`.
- **AC2**: A hard carve still **turns** (yaw swings comparably) — grip doesn't freeze the heading (contrast
  the θ≥3 planing-redirect over-grip failure).
- **AC3**: Pumping still gains speed (forces intact).
- **AC4**: Some drift is possible — a deliberately overpowered turn breaks loose rather than railing.
- **AC5**: Defaults (rate 0 / anti-slip scale 1) reproduce current behavior bit-for-bit; the change is
  opt-in until tuned. Snapshot tests unchanged at defaults.

## Test plan

1. A/B the `surfing_down_the_line_then_sharp_turn_left` and a clean down-the-line autopilot: current vs
   (grip on, force anti-slip off), comparing slip (`alongLeft`), yaw swing, and speed.
2. Pumping autopilot/trace — confirm speed still builds.
3. In-editor feel pass for drift / break-loose / on-rails (Decisions C/D).
4. 3× repeats per config (the autopilots are bistable — see planing-redirect.md).

## Related

- [planing-redirect.md](planing-redirect.md) — the pitch-plane redirect; share its engine machinery.
- [fin-force-normalization.md](fin-force-normalization.md) — why the force anti-slip is unreliable.
- [fin-carve-coupling.md](fin-carve-coupling.md) — the forward component we keep.
- [lateral-turn-world-relative-roll.md](lateral-turn-world-relative-roll.md) — the yaw driver we keep.
- [[small-weight-shift-sharp-turn]] — the carve-gain fix to land alongside.
- [pumping.md](pumping.md) — energy term that must survive.
