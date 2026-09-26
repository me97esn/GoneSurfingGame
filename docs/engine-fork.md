# The engine fork

Gone Surfing does not run on stock Unreal. It runs on a fork of UE 5.4 whose changes live entirely
inside the Chaos physics solver. This page describes what that fork does and why, because the fork
itself cannot be published: Unreal Engine source obtained through Epic's GitHub programme may only
be shared with other Engine licensees, not posted publicly.

That turns out not to cost much. The fork is **four files and about twenty commits**. Almost all of
the substance is one file — the Chaos rigid-body integrator — and it is easier to read about than to
read.

```
Engine/Source/Runtime/Experimental/Chaos/Private/Chaos/PBDRigidsEvolutionGBF.cpp
Engine/Source/Runtime/Experimental/Chaos/Public/Chaos/PerParticleGravity.h
Engine/Plugins/FX/Niagara/Source/Niagara/Private/NiagaraDataInterfaceActorMeshArray.cpp
Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraDataInterfaceActorMeshArray.h
```

The two Chaos files are the physics work described below: together they add roughly fifty console
variables under `p.Chaos.Solver.*`, which the game drives from its own tick. The two Niagara files
are unrelated to the physics — a data interface that hands a Niagara system an array of static
meshes pulled from an actor, used to render the wave and foam.

---

## Why fork at all

Physics like this is usually written entirely in forces, and it goes wrong in a specific way.

A force that is meant to *resist* something is a function of the state it is resisting, so it
overshoots and then reverses. Measured in this project: a drag term whose only job was to resist the
board crossing the wave face went from −2 177 (resisting) to **+12 507** (actively driving) once the
board was fast enough. The obvious fix — a governor on the summed forward drive — was built and
measured too: the three governed terms were cut 45–70 %, and the net forward force moved **12 %**,
because the ungoverned forces grew to replace them. The compensation runs through the board's
*position* on the wave, and the flow-driven forces are functions of exactly that, so a force-level
governor cannot close that loop.

The conclusion was to stop using one tool for three jobs:

| job | tool | lives in |
|---|---|---|
| add energy | force (impulse) | the game — ~20 surface samplers per board |
| remove energy | damping toward a target velocity | **the fork** |
| change direction without changing speed | velocity redirect | **the fork** |

Damping asymptotes instead of overshooting. A redirect conserves speed instead of bleeding it.
Neither is expressible as a force without reintroducing the overshoot, and neither is available in
stock Chaos. Hence the fork.

## The order of operations in one integration step

This ordering is load-bearing — the same set of operations in a different order produces a different
board. Inside the Chaos integrator, after forces have been applied and before the transform is
written back:

1. **Per-axis linear damping**, in the board's local frame
2. **Carve-grip redirect** — horizontal velocity toward the commanded heading
3. **Wave-carry redirect** — horizontal velocity toward the wave's travel direction
4. **Planing redirect** — full 3-D velocity toward the wave's up-slope
5. **Wave-normal damping** — toward the *water's* velocity along the wave normal
6. **Whitewater damping** — the whole horizontal velocity toward the water, split fore/aft
7. **Linear velocity ceilings** — per axis, with separate up and down limits
8. **Pitch-righting servo** — angular
9. **Yaw-rate ceiling** — per sign of rotation

The redirects (2–4) run before the wave-interaction damping (5–6) deliberately: the redirect gets
first claim on the velocity direction, and the damping only acts on what is left. That is what makes
the rocker character emergent rather than tuned — the board climbs because its momentum is bent up
the face, and then loses only the residue.

---

## The five families

### 1. Per-axis damping

Stock Chaos has one scalar linear damping and one angular. The fork replaces both with per-axis
damping evaluated in the **board's** frame, not the world's:

```
DampingLocalX / DampingLocalY / DampingLocalZ     forward / sideways / vertical
DampingZUp / DampingZDown                         asymmetric vertical
AngularDampingX / AngularDampingY / AngularDampingZ
AngularDampingYAwayExtra                          extra pitch damping, nose moving away from the face
AngularDampingYTowardReduction                    less pitch damping, nose moving toward it
PitchDampingBlendScale                            smooths the away/toward switch
FramerateIndependentDamping / DampingRefDt        see below
```

Sideways damping is what makes a board track its nose instead of skidding; vertical damping decides
how a board settles into the water. Those want completely different numbers, and a single scalar
cannot give them.

The asymmetric pitch damping exists because a surfboard is not symmetric about its pitch axis in
practice. Letting the nose drop toward the face and resisting it swinging away are different
behaviours, and an early symmetric version chattered.

`FramerateIndependentDamping` converts every damping fraction from "per tick" to "per `DampingRefDt`
seconds" before applying it. Without it a 30 Hz phone and a 60 Hz desktop are two different physics
models, which makes every headless measurement a lie.

### 2. Velocity redirects — one primitive, three instances

A single helper rotates a velocity vector toward a target direction by at most a given angle,
preserving magnitude. Three call sites use it, each with its own target and its own gate:

| instance | plane | target direction | gate |
|---|---|---|---|
| **planing** | full 3-D | the wave's up-slope | how buried the nose is |
| **carve grip** | horizontal | the board's heading | commanded carve |
| **wave carry** | horizontal | the wave's travel direction | cross-shore velocity deficit |

```
PlaningRedirectMaxAngle / PlaningRedirectGate / PlaningRedirectUpX,Y,Z
PlaningRedirectOutwardScale        one-sided: weaker when redirecting away from the wave
CarveGripRate / CarveGripGate / CarveHeadingX,Y
WaveCarryRedirectRate / WaveCarryRedirectGate / WaveCarryDirX,Y
```

The planing redirect is the interesting one. A planing hull climbs the face because its rocker bends
its momentum upward; a board that drives nose-first into the face and stays buried is a board whose
momentum nothing is bending. The first attempt rotated toward *board-local* up, which fails exactly
when it matters: in a hard turn the board is pitched 35–57° nose-down, so local-up points forward
and **down**, and the redirect buries the board harder — a feedback loop. The working version
rotates toward the world-frame wave up-slope, which is correct at any attitude and self-disables on
flat water, where there is no slope to climb.

`PlaningRedirectOutwardScale` makes the redirect one-sided. Turning *into* the wave and turning away
from it are not the same manoeuvre and should not get the same grip.

### 3. Damping toward the water, not toward zero

Ordinary damping pulls velocity toward zero — toward the world frame. Water is not stationary, and a
board in moving water that is damped toward the world frame is being dragged by a current that does
not exist.

So the wave-interaction damping is measured **relative to the water's own velocity**, which the game
feeds in every tick:

```
WaveNormalDampingRate / WaveNormalDampingGate / WaveNormalX,Y
WaterVelX / WaterVelY / WaterVelZ
WhitewaterDampingRate                whole horizontal velocity toward the water
WhitewaterNoseX / WhitewaterNoseY    the board's heading, so the damping can be split
WhitewaterAlongScale                 along-nose vs across-nose ratio
```

The wave-normal term acts only along the cross-shore axis: the board is free to run down the line at
whatever speed it can find, but it is coupled to the water in the direction the wave is moving.

Whitewater is the same mechanism at a different strength. Broken water takes your speed because the
foam is moving at its own velocity and everything in it is dragged toward that — so the whitewater
damping targets the water velocity, and splits along and across the board's nose, because a board
pointed along the foam and a board broadside to it do not lose speed the same way.

### 4. Ceilings

```
MaxVelocityX / MaxVelocityY / MaxVelocityZUp / MaxVelocityZDown
MaxAngularVelocityZPos / MaxAngularVelocityZNeg
```

A structural safety net, not a brake. Hydrofoil forces scale with v², so a coefficient error
compounds: a single instrumented run caught one bottom sampler producing **48 kN forward and 31 kN
upward in one tick**, and there was nothing in the loop that grew faster than the runaway. Damping
cannot fix that — a damping fraction capped at 0.95 still passes 5 % of any force through, and 5 %
of a runaway is still a runaway. A hard ceiling on the resulting velocity is structurally different:
it holds regardless of which force is misbehaving.

The yaw ceiling is **per sign**, which is a gameplay decision rather than a safety one: spinning out
away from the wave and pivoting into it are different events, and only one of them should be capped.

### 5. Pitch righting

```
PitchRightingRate / PitchMisalignment
```

A servo toward surface-relative level, fed a misalignment measured against the wave rather than
against the world. It exists because damping alone cannot recover a nose that is already buried —
damping removes rotation, and what is needed is rotation in a particular direction.

---

## How the game drives it

None of this is configured once and left. `ASurfboardUtils` recomputes the live values each tick
from the board's state and the wave geometry under it, and pushes them through the console-variable
interface before the next integration step. The rest — the ceilings, the damping constants — are
re-pushed whenever they change.

Most of the tunables come in pairs: a **rate** (how fast, a constant) and a **gate** (a 0–1
multiplier computed live). The gate is where the physics goes. `PlaningRedirectGate` is the nose
sampler's submersion; `WaveCarryRedirectGate` is the board's cross-shore velocity deficit against
the crest, which makes it self-limiting — it fades to nothing as the board catches up, instead of
overshooting past the crest. `WaveNormalDampingGate` fades out near the crest, so the same
coefficient behaves differently on the face and at the lip without anything switching modes.

Splitting rate from gate is also what makes the whole fork testable: set any rate to 0 and that
mechanism is inert, with everything else untouched.

## Reading further

The specs carry the measurements, the rejected alternatives and the A/B results:

- [`wave-interaction-damping-and-redirect.md`](../GoneSurfing/specs/wave-interaction-damping-and-redirect.md) — the principle and the current configuration
- [`planing-redirect.md`](../GoneSurfing/specs/planing-redirect.md) — including the board-local version that failed, and why
- [`carve-grip-via-redirect.md`](../GoneSurfing/specs/carve-grip-via-redirect.md) — retiring the force-based anti-slip
- [`wave-carry-redirect.md`](../GoneSurfing/specs/wave-carry-redirect.md) — the self-limiting gate
- [`velocity-ceiling-damping.md`](../GoneSurfing/specs/velocity-ceiling-damping.md) — the 48 kN run
- [`pitch-righting-and-redirect-escape.md`](../GoneSurfing/specs/pitch-righting-and-redirect-escape.md) — the crest punch-through
