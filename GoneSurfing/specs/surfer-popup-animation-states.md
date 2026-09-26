# Spec: Surfer rider — paddle / cobra / pop-up animation states

## Status
- [ ] Spec drafted (2026-08-08)
- [ ] C++ implemented: `ESurferAnimState` enum + `AnimState`/`SetAnimState` on
  `USurferAnimInstance`; `riderAnimState` field on `FStateTriggerStep`; push in
  `AStateTriggerAutoPilot::ApplyStep`
- [ ] Editor: three clips imported (paddle loop, paddle→cobra, cobra→stand), AnimBP state
  machine built, procedural lean/hip-IK moved inside the Surf state
- [ ] Player-validated: clips play on the right steps, pop-up lands on control handoff

> **2026-08-31 — BUG FOUND AND FIXED: the rider sometimes stayed prone for the whole ride.**
>
> `AStateTriggerAutoPilot` pushed `riderAnimState` per step, straight into the variable. Its
> "Cobra position" and "Pop up" steps can activate on the **same frame**, so `AnimState` went
> `0 -> 2` with the machine still in `SK_Paddle`. Every transition rule here is keyed on `AnimState`
> being *exactly* the next state in the chain, so a skipped link matches no rule and the machine
> never moves again — stranded in the paddle clip while the variable carried on to `Surf`.
>
> Measured on `Boards_on_flat_water`: **2 of 4 runs stuck**, and the logged sequence never contained
> a 1. On `Surfing_infinite_wave` it mostly hid, because the rails driver walks the chain one link
> per tick and usually got the machine past Paddle first — but it failed there too. Whether the two
> steps share a frame is frame-timing dependent, which is why it presented as "sometimes".
>
> Fix: the never-skip-a-link rule moved out of the rails driver (where it was a local guard, with a
> comment describing this exact hazard) and into `USurferAnimInstance`, where no caller can bypass
> it. `SetAnimState` now sets a **target**; `AnimState` walks toward it one link per update, and only
> follows the machine into `Surf` once the machine is actually there. 6/6 runs correct afterwards on
> the map that previously failed 2-in-4; 3/3 on the wave map.
>
> Note this retires the old claim below that untagged steps and snapshot tests "sit in the stance
> immediately". They never could: with no `Paddle -> Surf` rule in the machine, pushing `Surf` from
> `Paddle` was precisely the stuck case. Tests now watch the rider pop up, which costs about a second
> of animation and changes no physics — the rider's pose drives nothing the CSVs record.
>
> A `SurferAnim:` log line reports requested state, walked state and actual machine state on every
> change, so a future recurrence names its own cause.

> **2026-08-18 update (specs/skip-paddle-intro.md):** the cinematic intro no longer paddles —
> state injection starts the ride at the cobra step, so the intro pushes `Cobra → PopUp` only.
> The `Paddle` state stays in the enum (it is the AnimInstance default and the AnimBP entry
> state) but no level step needs to tag it, and the paddle loop clip never needs authoring
> unless paddling returns.

## Motivation

The rider currently plays exactly one pose — the surf stance — plus procedural lean/hip-IK on
top (specs/surfer-rider-lean.md, specs/surfer-rider-hip-ik.md). Before the board is planing, the
autopilot drives it through a scripted pop-up sequence (`AStateTriggerAutoPilot` steps), but the
mannequin just stands there in the finished stance the whole time. This spec makes the rider
**paddle**, then rise to a **cobra pose**, then **pop up** to the surf stance — with the animation
state driven by the autopilot step that is already sequencing the physics, so the rider's body and
the board's behaviour stay in lock-step exactly as the lean already does.

The surf stance is not replaced — it becomes one **state** in an AnimBP state machine, and the
existing procedural lean/hip-stab nodes move *inside* that state so they only evaluate once the
rider is standing.

## Design

### State model

Four rider states, in order:

| state | clip | plays | ends by |
|---|---|---|---|
| `Paddle` | paddling loop | prone/kneeling, arms stroking | loops until state changes |
| `Cobra` | paddle→cobra (pop-up anim #1) | chest up, arms straight, legs still down | holds last frame |
| `PopUp` | cobra→stand (pop-up anim #2) | feet swing under, rises to stance | plays once, auto-advances to Surf |
| `Surf` | surf stance + procedural lean/hip-IK | the existing ridden pose | terminal (until ragdoll fall) |

`Surf` is **not** pushed by the autopilot — it is reached automatically when the `PopUp` clip
finishes (`Time Remaining (ratio)` transition). The pop-up clip *ending* is the visual handoff to
the player.

### Coupling: autopilot step → anim state (push, per-step, data-driven)

Each `FStateTriggerStep` gains a `riderAnimState` field (default `Surf`, so existing steps and
tests are unaffected). Tag the relevant steps in the Details panel:
- the "paddle slowly" step → `Paddle`
- the "cobra pose" step → `Cobra`
- the pop-up / control-handoff step → `PopUp`

`AStateTriggerAutoPilot::ApplyStep` resolves the mannequin's `USurferAnimInstance` (the skeletal
mesh component on the autopilot's `surfboard`, same lookup the pawn uses at
`SurfboardPawn.cpp` RiderMesh) and calls `SetAnimState(Step.riderAnimState)`.

**Push, not pull, and not string-matched on `description`:** only the *enabled* autopilot calls
`ApplyStep` (siblings share one board), so pushing sets state exactly when the driving autopilot
advances — no polling, no ambiguity about which autopilot owns the rider. A per-step enum matches
the existing per-step-data pattern (`weightRight`, `jetEngineOn`); matching on the free-text
`description` would be fragile.

### Procedural lean/hip-IK scoped to the Surf state

The C++ keeps computing `LeanRightDeg` / `LeanForwardDeg` / `TwistYawDeg` and the hip-stab
outputs every tick (cheap, read-only). The change is purely graphical: the Transform (Modify)
Bone lean nodes and the hip-stab pelvis/TwoBoneIK chain move from the top-level AnimGraph **into
the Surf state's sub-graph**. A state machine only evaluates the active state's pose, so those
nodes stop firing during Paddle/Cobra/PopUp automatically — the clips play untouched, with no
gating flag to maintain. (This is why hip-stab already switched to *stored* calibration: once a
pre-stance clip plays, the old runtime "capture the stance on flat water" assumption no longer
holds — see `bHipStabUseStoredCalibration`, specs/surfer-rider-hip-ik.md.)

### C++ surface

```cpp
UENUM(BlueprintType)
enum class ESurferAnimState : uint8 { Paddle, Cobra, PopUp, Surf };
```

On `USurferAnimInstance`:
- `UPROPERTY(BlueprintReadOnly) ESurferAnimState AnimState = Paddle;` — the state machine reads
  this in its transition rules.
- `UFUNCTION(BlueprintCallable) void SetAnimState(ESurferAnimState)` — setter the autopilot calls.

On `FStateTriggerStep`:
- `UPROPERTY(EditAnywhere) ESurferAnimState riderAnimState = ESurferAnimState::Surf;`

In `AStateTriggerAutoPilot::ApplyStep`: after `currentStep = Step`, resolve the rider anim
instance off `surfboard` and push `Step.riderAnimState`.

The default `Surf` matters: every existing step and every snapshot-test autopilot leaves
`riderAnimState` at `Surf`, so the state machine sits in the stance immediately and nothing about
the physics tests or the current riderless-through-stance behaviour changes until steps are
re-tagged.

## Editor — manual setup (cannot be authored from C++)

Detailed beginner walkthrough lives in the chat that created this spec; summary of the graph:

1. Import three animation clips onto the mannequin skeleton (Mixamo paddle / cobra / pop-up,
   retargeted, or Control-Rig authored). Set the paddle clip **Looping**; the cobra and pop-up
   clips **not** looping.
2. In `ABP_Surfer`'s AnimGraph, add a **State Machine**; feed its result to the Output Pose.
3. Four states: `Paddle`, `Cobra`, `PopUp`, `Surf`. First three just play their clip (`Surf`
   holds the stance pose). Move the existing lean + hip-IK nodes into `Surf`.
4. Transitions read `Get AnimState`:
   - Entry → `Paddle`
   - `Paddle → Cobra`: `AnimState == Cobra`
   - `Cobra → PopUp`: `AnimState == PopUp`
   - `PopUp → Surf`: automatic, `Time Remaining (ratio) < 0.1`
5. Compile + Save. Set the "paddle slowly" / "cobra pose" / pop-up steps' `riderAnimState` on the
   autopilot actor(s) in the level.

## Replay

Replay drives the board kinematically and freezes the weight actor; the rider's *state* is not
recorded in the trace. A replay that starts after pop-up shows the Surf stance (the default), which
is correct for the ridden portion. If a replay ever needs to show the pop-up itself, record
`AnimState` per frame and push it via a `SetReplayAnimState` sibling to `SetReplayWeights` — out of
scope until a replay actually begins pre-stance. See specs/on-device-ride-replay.md.

## Acceptance criteria

- **Given** the autopilot reaches the step tagged `Paddle`, **then** the rider plays the looping
  paddle clip.
- **Given** the step tagged `Cobra` activates, **then** the rider stops paddling and rises to the
  cobra pose, holding it.
- **Given** the step tagged `PopUp` activates, **then** the rider plays the cobra→stand clip once
  and, on its completion, is in the surf stance with procedural lean/hip-IK live.
- **Given** any step left at the default `riderAnimState = Surf` (all existing steps, all snapshot
  tests), **then** behaviour is unchanged from today.
- **Given** the player shifts weight while in `Surf`, **then** lean/twist/hip-IK respond exactly as
  before (nodes now live inside the Surf state but read the same C++ outputs).
- Zero physics impact: the AnimInstance and the new step field are read-only w.r.t. forces;
  snapshot trajectories unchanged.

## Test cases

1. PIE cinematic intro: watch paddle → cobra → pop-up → stance track the autopilot steps; confirm
   the pop-up finishing lines up with control handoff (`ControlDelayAfterAutoPilot`).
2. PIE, after handoff: shift weight — lean/hip-IK still work in the Surf state.
3. Headless snapshot suite: `Compare.ps1` unchanged (steps stay at default `Surf`; rider is
   cosmetic and read-only).
</content>
</invoke>
