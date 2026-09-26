# Spec: Deterministic ride handoff — the board is always surging down the line

## Status
- [x] Spec drafted (2026-08-24). **Revised twice the same day** — see "Revision notes".
- [x] **Phase 1** — deterministic intro on rails, ending in a stamped physics resume — **done and
  running in the playable level** (2026-08-25)
  - [x] Force pipeline split: `SurfRails::AreForcesSuppressed()` gates the four impulse/torque sites
    in `AFluidDynamics`, `ABuoyancy` and `ASharedCalculations` while every computation still runs.
    (`AWeightDistribution`'s two sites already sat behind `IsSimulatingPhysics()` and are inert on a
    kinematic board.)
  - [x] Trace format: angular velocity recorded as columns 30-32; linear velocity (cols 9-11, always
    recorded, never read) now parsed. Verified end-to-end — 33-column header, populated values.
  - [x] `-RecordIntro`: records BeginPlay → handoff, so the trace's final row IS the handoff state.
    The normal recorder deliberately starts *at* handoff, which is the wrong window for a rails source.
  - [x] Rails driver: `StartRails` / `TickRails` / `ApplyRailsAtTime` / `FinishRails`, wired to the
    `RailsIntroTrace` property with fallback to the live intro on any problem.
  - [x] Reference intro recorded from a good ride and `RailsIntroTrace` pointed at it:
    `Content/InputTraces/intro-reference.csv` — 254 rows, ~2.4 s, recorded on the phone
    (`session=phone-2026-08-25-15-07-58`). Commit `fa6075016` moved the trace's start to the
    wave-clock injection and made tile preloading per-tile, which is what made playback clean.
  - [x] Round-trip validated headless on `surfing-down-the-line`: handoff state identical across
    frame rates; intro divergence 350 cm/4.6 deg live vs 30 cm/0.1 deg on rails (2026-08-24)
  - [x] Rails playback eye-tested on device — the intro plays back and looks right on the phone
  - [x] Spike: confirmed Chaos derives `V`/`W` from kinematic targets and the game can read them
    back, with no engine change required. Direction correct 262/262; magnitude carries a per-frame
    scale (0.53×–1.83×), so recorded velocity columns are still required (2026-08-24). Probe lives
    behind `-KinematicProbe`; delete once the rails driver is validated. **Now deletable** — the
    rails driver is validated and shipping (`TickKinematicProbe` in `SurfboardPawn.cpp`).
  - [x] Kinematic-target drive (`ETeleportType::None`) so the board carries a solver-derived velocity
  - [x] Stamp at the transition: pose, linear + angular velocity, wave clock released
  - [x] `HANDOFF STATE` log line
  - [x] Editor: intro autopilot switched over — `Content/levels/Surfing_infinite_wave.umap`
    points `RailsIntroTrace` at `intro-reference.csv`
- [x] Cross-frame-rate bench (AC3/AC6) — see "Round-trip validation"
- [ ] Snapshot suite non-regression (AC1) — `surf-straight` OK with rails off; full suite not yet run
- [x] Player-validated on phone: the start looks good on device. (What was confirmed is the
  *intro*; the seam and first-try-ride claims have not been separately measured.)
- [ ] **Phase 2** — gradual control handoff (assist fading out into player authority)

## Revision notes

**Revision 1.** The first draft rejected a world-absolute state stamp and proposed a closed-loop
converging assist instead. The rejection was wrong. It argued the stamp could not work because the
intro's length varies by device, so the wave phase at handoff varies — an objection that dissolves
once the **wave frame is stamped too**. Pose + linear velocity + angular velocity + wave frame is the
complete state vector for this system: the board's rigid-body state, plus the field it moves through.
Pin all of it and the physics has nothing left to disagree about.

The draft also leaned on [skip-paddle-intro.md](skip-paddle-intro.md)'s measured ~70 % missed-catch
result for open-loop injection. That result is real, but it is about injecting **at the catch** — the
most bistable instant of the ride — then requiring the physics to navigate the catch unaided. Handoff
is *after* the catch, with the board already established. Applying the result from one regime to the
other was a mistake.

**Revision 2.** The second draft made a stamp-at-handoff the first stage and put the rails behind a
measurement gate. That gate was redundant: the board's heading is *already observed* to differ a lot
between runs before control transfers. A stamp still delivers a deterministic handoff state — that is
what stamping does — but with a large pre-stamp spread the correction is large, and a yaw snap of
tens of degrees in one tick is not shippable. The rails are therefore the primary mechanism, and the
gradual handoff (the first draft's assist) becomes a follow-on phase, valuable *because* it sits on
top of a deterministic state rather than a random one.

## Motivation

A new player must be able to start surfing on the **first try**. That means the instant control
transfers, the board is already surging **down the line** (+Y), not aimed at the beach (−X).

Today the heading at handoff is a *simulation outcome*: the intro autopilot applies weight and jet,
and whatever heading ~2 s of chaotic physics produces is what the player inherits. That outcome is
device- and load-dependent, which is why the same build goes down the line on one machine and at the
beach on another. It is directly observable in play that the pre-handoff direction varies widely.

The frame-rate dependence is measured, not theoretical:

- [framerate-independent-angular-damping.md](framerate-independent-angular-damping.md) recorded
  **three identical headless PC runs of one input trace diverging from each other by 6.8–22.8 m and
  up to 180° of yaw** under a variable timestep, collapsing to cm-scale only under
  `-usefixedtimestep`.
- [FluidDynamics.cpp:1766](../Source/GoneSurfing/FluidDynamics.cpp#L1766) applies
  `impulse = force × min(DeltaTime, 0.033)`. Below 30 fps the excess dt is **discarded**, so a 20 fps
  phone receives ~2/3 of the propulsion per real second that a 60 fps phone does.

The conclusion to draw is not "determinism is unattainable" — it is that determinism cannot be
obtained by *simulating* toward it. It has to be **set**, and set for the whole window the player
watches, not just its final instant.

### Scope of the guarantee

This spec makes the intro, and the **state at handoff**, identical on every device. It does not make
the ride *after* handoff identical — under a variable timestep that is not achievable, and it is not
needed. What the player needs is to inherit a board that is unambiguously surfing, from which the
first few seconds go well on any device.

### The animation states stay

Paddle → Cobra → PopUp → Surf are **kept, visible, and unchanged**. They read well, they sell the
realism, and their duration is the player's window to prepare. Under this design they become *more*
consistent, not less: every player sees the identical intro, at the identical pace.

## Non-goals

- Changing, shortening, or hiding any rider animation state.
- Making the post-handoff ride reproducible across devices.
- A general frame-rate-independence fix for the force loop — see "Follow-up work".

## Phase 1 — the intro runs on rails

Drive the whole visible intro as a **wave-synced kinematic playback** of a recorded good ride, and
resume physics at its end from a stamped state.

```
[ scripted intro — kinematic, wave-pinned ][ STAMP ][ physics ride, player in control ]
   identical on every device                  ^ physics resumes, wave released to free-run
   rider anim, spray, wave all replayed         camera cuts to BEHIND, "…and surf!"
```

Three properties make this stronger than correcting at the end:

1. **No seam.** The board is already at the handoff state, because playback put it there. The
   transition changes almost nothing.
2. **The discontinuities move to the start of the intro, where they are already invisible.** The
   initial seat and wave-frame jump happen at ride start — exactly where skip-paddle-intro already
   does a frame jump today and reports it as unnoticeable.
3. **Playback is time-indexed, so it is frame-rate independent in the way that matters.** A device
   that hitches skips rows and still arrives at the same final row. A slow device sees a choppier
   intro; it does not see a *different* intro.

### What can be reused, and what cannot

[`ApplyReplayAtTime`](../Source/GoneSurfing/SurfboardPawn.cpp#L2832) already does the pose
interpolation, `TeleportPhysics` warp, `SetWaveFrame` sync, and spray/rider-weight playback exactly as
needed. That part is directly reusable.

`StartReplay` around it is **not**. It disables physics *and* calls `SetForcePipelineTicking(false)`,
killing every `AFluidDynamics` / `ABuoyancy` / `ASharedCalculations` / `AWeightDistribution` tick
([SurfboardPawn.cpp:2735-2751](../Source/GoneSurfing/SurfboardPawn.cpp#L2735-L2751)). Its own comment
says it is terminal: "a Restart reloads the level." It is a viewing mode, not a resumable state.

So Phase 1 needs a *second* playback mode: board kinematically driven, but the force pipeline still
**computing**. Split the pipeline's "compute" from its "apply" — sampling water, deriving planing and
slope state, and updating the smoothing filters continue; the impulse/torque applications are
suppressed. The moment physics resumes, every consumer is warm.

### Why the board needs a real velocity on rails

The reason the compute half cannot simply be left on today is that a teleported board's physics
velocity is meaningless, and the derived state that matters is velocity-based:

- **`AmountPlaning` keys purely on board speed vs a threshold** — velocity-based, not depth-based. A
  board reading ~0 velocity arrives at the transition flagged *not planing*, and the first ticks of
  the real ride run with planing-gated forces switched off.
- `AFluidDynamics::SmoothedWaveMassForce` is a first-order lag (`alpha = dt / waveMassSmoothingTau`)
  needing a few hundred ms of coherent input to converge.
- The relative-water-velocity computation in `ASharedCalculations` — which every per-surface force
  reads — is a velocity difference. Without a board velocity it is measuring the wrong thing entirely.

### Kinematic targets supply that velocity — no engine change needed

**Spike result (2026-08-24, source trace through the fork).** The whole chain works in stock code.
The engine fork is not on the critical path, and neither are recorded velocity columns.

1. **Chaos derives both velocities from a kinematic target.** In
   `FPBDRigidsEvolutionGBF::ApplyKinematicTargets`, `EKinematicTargetMode::Position` interpolates the
   particle toward its target across substeps and computes
   `NewV = FVec3::CalculateVelocity(CurrentX, NewX, Dt)` and
   `NewW = FRotation3f::CalculateAngularVelocity(CurrentR, NewR, Dt)`, writing both onto the particle
   (`PBDRigidsEvolutionGBF.cpp` ~1508-1524). Velocity is a *derived* quantity, exactly as wanted.
2. **UE routes a move to a kinematic target when — and only when — the teleport type is `None`.**
   `FBodyInstance::SetBodyTransform` takes the `SetKinematicTarget_AssumesLocked` branch under
   `bIsSimKinematic && Teleport == ETeleportType::None`, where
   `bIsSimKinematic = IsKinematic && CanSimulate` (`BodyInstance.cpp` ~2660-2681). Anything else —
   including `TeleportPhysics` — falls through to `SetGlobalPose`, which warps the body with **no**
   velocity derivation.
3. **The game can read the result back with no guard.**
   `UPrimitiveComponent::GetPhysicsLinearVelocity` goes straight to `BI->GetUnrealWorldVelocity()`
   with no `IsSimulatingPhysics()` check (`PrimitiveComponentPhysics.cpp` ~362), and
   `FBodyInstance::GetUnrealWorldVelocity_AssumesLocked` reads the particle directly. Kinematic
   bodies report their velocity normally.

So the requirement reduces to a **one-enum change** in the rails driver: pass
`ETeleportType::None`, not `TeleportPhysics`. The existing replay uses `TeleportPhysics`
([SurfboardPawn.cpp:2876](../Source/GoneSurfing/SurfboardPawn.cpp#L2876)) and therefore derives no
velocity — which is correct for a terminal viewing mode and wrong for a resumable one.

**Measured, not just read (2026-08-24).** The `-KinematicProbe` path (`TickKinematicProbe`) makes the
board kinematic, drives it with `ETeleportType::None` along a constant-velocity, constant-yaw-rate
path, and reads the velocity back. Headless result over 262 samples:

| Measure | Result |
|---|---|
| Direction of derived velocity vs commanded | correct on **262/262** samples (within ~2.5°) |
| Magnitude ratio (derived / commanded) | mean **1.043**, min **0.534**, max **1.828** |
| Linear and angular disagreeing on that ratio | 6/262 (2.3 %) |

`bSimulatePhysics=1` on the board's body, so `CanSimulate` holds and the `SetKinematicTarget` branch
is genuinely taken.

### The catch the probe found: derived magnitude carries a frame-time scale

Direction is exact every sample, but magnitude is scaled per frame — and **uniformly across all
components**, linear and angular alike (`readV.Y/500 == readV.Z/120 == readAngVel.Z/30` to three
decimals on essentially every sample). One `Dt` is behind it: the solver divides the position delta by
its own step, while the commanded pose advances by the game frame's delta. Over a run the mean ratio
is ~1.04, i.e. correct on average — but any *single* frame can read nearly 2× or half the true speed.

Two consequences, and they are the reason this spike changed the plan rather than just confirming it:

1. **Never take the resume velocity from the solver at the transition.** Reading `V`/`W` on the
   handoff frame would inherit that frame's scale error and could hand the player a board moving at
   half or double the intended speed — the opposite of AC3. The resume velocity must come from
   recorded data.
2. **Velocity-based derived state sees jitter during the rails.** `AmountPlaning` is a threshold on
   speed; a board riding just above the threshold could flicker across it on a scaled frame. Smoothing
   what the pipeline reads, or feeding it the recorded velocity rather than the solver's, is the
   mitigation — decide during implementation, but do not assume the raw derived value is clean enough.

Kinematic targets still earn their place: they are what stops the board reading as *stationary*, which
is a far worse failure than a noisy magnitude. They just do not remove the need for recorded velocity.

**The trap: a kinematic target must be re-set every frame.** Once consumed, the mode switches to
`EKinematicTargetMode::Reset`, which on the following frame **zeroes `V` and `W`** if no new target
has been set. Driving from the replay track every tick satisfies this, but a frame that skips the
drive silently collapses the board's velocity to zero — and every velocity-based consumer with it.
Assert on it rather than trusting it.

Velocity setters are a different story — `UPrimitiveComponent::SetPhysicsLinearVelocity` calls
`WarnInvalidPhysicsOperations` and `FBodyInstance::SetLinearVelocity` guards on `IsRigidBody`. That
is the warning spam `SetForcePipelineTicking(false)` exists to silence. Irrelevant here: on rails
nothing needs to *set* velocity, only read it.

### Velocity columns are needed after all

`FReplayRow` carries `t`, `pos`, `rot`, `waveFrame`, spray and weights — no velocities
([SurfboardPawn.h:880](../Source/GoneSurfing/SurfboardPawn.h#L880)). The first reading of the spike
suggested these could be skipped, since Chaos derives velocity from the pose track. The frame-time
scale above kills that: the derived value is right on average but wrong on any given frame, and the
transition happens on exactly one frame.

So add linear and angular velocity columns, recorded from the source ride, backwards-compatible the
same way `hasSpray` / `hasWeight` already are. These are what FR4 stamps at the resume.

Do **not** finite-difference them from consecutive rows. At 50 ms sampling, with the board pitching
through a pop-up, the differenced angular velocity is noise, and injecting a wrong spin is precisely
the visible first-frame hitch skip-paddle-intro documents.

### The transition

At the stamp instant: re-enable simulation, set pose and both velocities to the recorded values
(`ApplyStartPose()` already does exactly this — [StateTriggerAutoPilot.cpp:764-765](../Source/GoneSurfing/StateTriggerAutoPilot.cpp#L764-L765)),
re-enable force application, and clear `bManualFrameControl` so the wave free-runs from the recorded
frame — the "Phase 2 behaviour" described in `SetWaveFrame`'s own comment, and the same free-run
skip-paddle-intro relies on. The wave is already at the right phase, so nothing jumps.

### Test and replay gating

The scripted intro must be **off** for anything that records or replays a trajectory, or it corrupts
baselines and trace replays. Gate it the way existing controls-gated features are gated
([SurfboardPawn.cpp:2989](../Source/GoneSurfing/SurfboardPawn.cpp#L2989)):

```cpp
if (bExternalWeightOverride || SurfDebug::CVarAutopilots.GetValueOnGameThread().Len() > 0)
{
    // headless snapshot test or -ReplayTrace run: no scripted intro
}
```

Headless test runs **do** enable player controls after autopilot handoff, so "controls are on" is not
a sufficient discriminator — the CVar filter is.

### Measurement: the HANDOFF STATE log line

Emit one line at the transition, carrying the state plus the wave-relative quantities that say
whether it is a *good* state:

```
SurfboardPawn: HANDOFF STATE: waveFrame=1043 pos=(...) rot=(...) vel=(...) angVelDeg=(...)
  | headingVsDownLineDeg=-4.2 waveRelativeRollSin=0.18 signedDistanceToCrest=-315.4
    slopeSin=0.27 waterColumnAbove=41.2 surgeSpeed=1180.6
```

Its job is twofold: verify AC3 (the state really is identical everywhere), and answer "is the
recorded intro still a good one?" after a tuning or wave-data change. A recording is only as good as
the ride it came from, and this line is how that gets audited. `ASharedCalculations` already publishes
`signedDistanceToCrest`, `waveRelativeRollSin`, `boardWideSlopeSin`, `boardWideWaterColumnAbove` and
`waveBackDirection`, so it costs nothing to log. Derive the down-line axis rather than hardcoding
`+Y`:

```cpp
// waveBackDirection defaults to +X (wave travels +X, breaks toward -X, +Y is down the line).
const FVector downLine = FVector::CrossProduct(SC->up, SC->waveBackDirection).GetSafeNormal();
```

### Choosing which ride to record

Record from a ride that is **robustly** down the line — good surge speed, heading well past the point
where the outcome could fork — not one that merely happens to be pointed correctly at that instant.
The post-handoff regime has known bistable branches
(see [pitch-righting-and-redirect-escape.md](pitch-righting-and-redirect-escape.md)), and while an
identical resume state resolves a fork identically in principle, per-device tick noise can still pick
sides if the state sits near a boundary. Starting deep inside the basin removes the question.

## Phase 2 — gradual control handoff

With a deterministic starting state in place, fading control in rather than cutting it over is worth
building. It absorbs the divergence that resumes the moment physics takes over, and it directly
serves the first-try goal: a player who touches nothing still gets a few seconds of surfing.

- **Actuation through `AWeightDistribution`** (`amountToTheRight` / `amountInFront`) — the same
  channel the autopilot steps and the player use. In-fiction: the rider shifts weight, so a
  correction can never look like the board being shoved, and the rider's procedural lean follows for
  free (`USurferAnimInstance` reads the same values the physics consumes).
- **Control law**: proportional on wave-relative heading error, with a smaller term on bank error,
  clamped to a maximum weight offset from centre. Start P-only; add rate damping only if it
  oscillates.
- **Fade-out**: assist authority ramps linearly to zero over `AssistFadeOut`, continuing past the
  tick controls enable. Player input is summed on top at full authority throughout, so the player
  never fights the assist for the same channel.

Keep assist output below `lateralTurnHardCarveBoostStart`. `lateralTurnHardCarveBoost` (0.5) ramps on
*commanded* lean, so a large assist command would trip the boost band and get ~2.3× the carve rate it
expects. Staying under it also keeps the assist in the gentle-trim regime, which is where a handoff
correction belongs — it should look like trim, not like a carve.

## Round-trip validation (2026-08-24, headless)

Record-then-replay A/B on the `surfing-down-the-line` autopilot — a ~14 s ride through the real
Paddle -> Cobra -> Pop up -> Surf intro, with an 8 s rails window. Rails were driven from a trace
recorded by an earlier live run of the same autopilot (`-RecordIntro -RecordIntroSeconds=8`, then
`-RailsTrace=`).

### Same machine, same frame rate

| Comparison | Max divergence in the intro window (0-8 s) |
|---|---|
| Rails run vs the run it was recorded from | **3.8 cm**, yaw 0.05 deg |
| Control: two plain live runs | 15.1 cm, yaw 0.22 deg |

`HANDOFF DELTA` (board's actual pose minus the trace's final row) was **0.00 cm / 0.00 deg** — the
few cm above is CSV-vs-trace sampling offset, not tracking error.

### Across frame rates — the case this spec exists for

Repeating both at `-usefixedtimestep -fps=20` against the default variable rate:

| Comparison | Max divergence, intro window | Max yaw error |
|---|---|---|
| **Live @20 fps vs live @default** | **350 cm** | **4.61 deg** |
| **Rails @20 fps vs rails @default** | 29.8 cm | 0.10 deg |
| After handoff (8-13.5 s), live | 440 cm | 3.08 deg |
| After handoff (8-13.5 s), rails | 113.8 cm | 1.32 deg |

And the criterion the spec is actually judged on, AC3:

> `HANDOFF STATE` at 20 fps was **identical to every printed digit** to the default-rate run —
> `waveFrame=984 pos=(4847.9, 773.3, 303.5) rot=(P=-3.34, Y=80.71, R=-1.31) vel=(-107.3, 18.8, 2.6)
> angVelDeg=(-1.8, -4.2, -0.1) headingVsDownLineDeg=50.6 surgeSpeed=108.1`.
>
> Only the live-sampled wave-relative diagnostics differ in the third decimal
> (`waveRelativeRollSin` 0.011 vs 0.010), as they must — those are read from `ASharedCalculations`,
> which is still ticking.

So frame rate moves a live intro by **3.5 m and 4.6 deg of heading**, and moves a rails intro by an
amount consistent with CSV sampling offset (the board covers ~40 cm per 50 ms sample at this speed).
The handoff state itself does not move at all. Divergence after handoff is real but ~4x smaller than
live, because both runs start from the same place.

### What this does not cover

- Never run on a phone, and never eye-tested — the seam, the wave-frame jump at intro start and the
  rider animation have only been reasoned about, not seen (AC2, AC4).
- The reference trace here is a headless PC recording of an autopilot, not a hand-picked good ride.
  Whether a PC recording is an acceptable source for a phone build is untested.
- AC1 was checked on `surf-straight` with rails off (`OK`), not on the full suite.

## Requirements

### Phase 1

**FR1** — The visible intro plays back from a recorded track — board pose, wave frame, rider
animation state, spray, rider weights — identical on every device.

**FR2** — The force pipeline keeps computing derived state while the board is on rails, with force
*application* suppressed, so `AmountPlaning`, relative water velocity and the wave-mass smoothing are
warm at the transition.

**FR3** — The board carries a real linear and angular velocity while kinematically driven, so
velocity-based derived state is correct rather than merely warm. Satisfied by driving the pose track
with `ETeleportType::None` so Chaos derives `V`/`W` from the kinematic target, with a kinematic target
set every frame the rails are active.

**FR4** — At the transition, simulation and force application re-enable, pose and both velocities are
set to the recorded values, and manual wave-frame control is released so the wave free-runs.

**FR5** — Player controls enable on the existing schedule (`ControlDelayAfterAutoPilot` after the
transition), with the existing camera cut and "…and surf!" cue unchanged.

**FR6** — One `HANDOFF STATE` line is logged at the transition, carrying the state and the
wave-relative diagnostics.

**FR7** — All new behaviour is opt-in and off by default, so every existing autopilot — including
every snapshot test — is behaviourally untouched.

**FR8** — The scripted intro is disabled when `bExternalWeightOverride` is set or the
`surf.autopilots` filter CVar is non-empty.

**FR9** — The rails assert that a kinematic target is set every active frame, so a skipped drive
cannot silently zero the board's velocity.

**FR10** — `FReplayRow` gains linear/angular velocity columns recorded from the source ride,
backwards-compatible via a `hasVelocity`-style flag. FR4's resume velocity comes from these, **never**
from reading the solver on the transition frame.

### Phase 2

**FR11** — A bounded assist drives wave-relative heading and bank toward configured targets via
`AWeightDistribution`, with authority fading linearly to zero past the moment controls enable.

**FR12** — Player input is applied at full authority throughout the fade and is summed with, never
replaced by, the assist output.

**NFR1** — No new per-tick allocation or actor iteration; reuse the existing replay row-hint cursor
and the autopilot's `ResolveSharedCalcsIfNeeded()` cache.

**NFR2** — The transition must not be perceivable in normal play.

## Acceptance criteria

**AC1 — Defaults are a no-op.** *Given* the feature off, *when* the headless suite runs, *then*
`Compare.ps1` exits 0 on every baseline. (Check the `full-iso` dates first — `Compare` grades every
CSV in `Saved/Tests/latest/`, including stale ones.)

**AC2 — Animation unchanged.** *Given* an interactive run, *then* Paddle → Cobra → PopUp → Surf play
as they do today, at the recorded pace, with spray present throughout.

**AC3 — Bit-identical handoff state.** *Given* any two runs on any two devices or frame rates, *then*
the `HANDOFF STATE` pose, velocities and wave frame are identical to float print precision. This is
the criterion the spec exists for and it should hold **exactly**, not statistically.

**AC4 — No seam.** *Given* an interactive run, *then* no pose jump, wave-shape pop, spray dropout or
force step is visible at the transition. Verified by eye plus continuity of the `thrust` / `buoyancy`
debug output either side of the boundary.

**AC5 — Derived state coherent.** *Measured 2026-08-25:* `planing=0.750` at handoff with zero
"not simulating physics" errors, confirming the board carried a real velocity through the rails.
Original wording follows.

**AC5 (original) — Derived state coherent.** *Given* the transition instant, *then* `AmountPlaning` is already at
its riding value rather than climbing from zero, confirming FR2/FR3.

**AC6 — Frame-rate insensitivity of the early ride.** *Given* N ≥ 5 headless runs at
`-usefixedtimestep -fps=60` and N ≥ 5 at `-fps=20`, *then* every run is still surfing down the line
3 s after controls enable, and `headingVsDownLineDeg` at +3 s stays within tolerance across both
rates. Baseline the same measurement with the feature off, to quantify what was bought.

**AC7 — Do-nothing ride.** *Given* a run where no player input is ever applied, *then* the board is
still surfing down the line ≥ 3 s after controls enable.

**AC8 — Player retains authority (Phase 2).** *Given* a hard lean commanded on the first tick controls
enable, *then* the board responds at full commanded authority, not a damped fraction.

## Test cases

1. **Suite non-regression**: `RunGameAndCollectLogs.bat` → all baselines exit 0.
2. **Frame-rate bench**: `EXTRA_ARGS="-usefixedtimestep -fps=20" ./RunGameAndCollectLogs.bat` and the
   same at `-fps=60`; diff the `HANDOFF STATE` lines (AC3) and compare heading at +3 s (AC6).
   `$env:EXTRA_ARGS` is the runner's launch-arg passthrough
   ([RunGameAndCollectLogs.ps1:131](../../RunGameAndCollectLogs.ps1#L131)).
3. **Seam inspection**: interactive run with `surf.debug.flags 'thrust,buoyancy'`, reading force
   continuity across the transition.
4. **Do-nothing ride** on device: press Start, never touch the controls, confirm AC7.
5. **Old-trace compatibility**: replay a pre-velocity-columns trace, confirm it still loads and plays.

## Gotchas / watch items

- **`board.forwards` is local +Y**, and the down-line axis derives from `waveBackDirection`
  (default +X, breaking toward −X). Never hardcode either.
- **`AmountPlaning` is velocity-based, not depth-based.** This is the trap behind FR3 — a teleported
  board reads as not planing however deep and fast it looks.
- **`StartReplay` is terminal.** Do not extend it; add a second, resumable playback mode.
- **`ETeleportType::None` is load-bearing on the rails.** `TeleportPhysics` warps the body via
  `SetGlobalPose` and derives no velocity — silently defeating FR3 while looking correct on screen.
- **`IsSimulatingPhysics()` guards are traps once the board is kinematically driven.** Found in play
  (2026-08-25): `ASharedCalculations::calcComponentVelocity` guarded on it and returned
  **`FVector::ZeroVector`** for a kinematic board, so every force computation during the rails saw a
  stationary board — silently defeating FR2/FR3 while logging only "BasePrimComp is not simulating
  physics". Fixed by OR-ing in `SurfRails::IsKinematicallyDriven()`; the same pattern in
  `LogStartPoseSnapshot` would have reported zero velocities for any snapshot taken during a rails
  run. Audit any new `IsSimulatingPhysics()` guard on a *velocity read* — application guards are fine.
  **The headless A/B could not have caught this**: the board follows the pose track whether or not
  the forces are right, so trajectories matched perfectly while the derived state underneath was
  wrong. It surfaced only from watching the log during a real playthrough.
- **The recording must start AFTER the autopilot's wave-clock injection.** `-RecordIntro` begins at
  BeginPlay, but `AStateTriggerAutoPilot::Start()` is deferred and it is `Start()` that applies
  `StartWaveFrame`. So the opening ~1 s of a raw recording is the deferred-Start dead zone
  skip-paddle-intro documents: the board sits undriven while the wave free-runs at the PRE-jump frame
  (measured 887 -> 910), and then the clock jumps (910 -> 1009). Rails replay that faithfully,
  jump included — which is seen as the wave starting in one place and snapping to another shortly
  after the level loads. The recorder now rebases at that jump and discards what came before, so a
  trace begins where the ride begins. Guarded to forward jumps only: the wave loop wraps
  1078 -> 886, a large NEGATIVE delta that must never trigger it.
  *This was the actual cause of the reported wave jump; the two GridLOD preload fixes below are real
  but were treating a secondary symptom.*
- **The wave visibly snaps to the trace's frame shortly after start** unless the GridLOD meshes are
  re-preloaded. `AGridLODActor::BeginPlay` preloads 20 frames around whatever
  `GetCurrentFrameFromController()` returns, and actor BeginPlay order is not guaranteed — so it
  often preloads the PRE-jump frame, and the cells then stream the recorded frame's meshes in over
  the following frames. This is the pop skip-paddle-intro predicted.
  **The preload must be per-tile.** Every `AGridLODActor` renders a different phase of the wave —
  `GetCurrentFrameFromController()` adds that tile's `FrameOffset` (measured: 0 / 95 / 190, giving
  frames 887 / 982 / 1077 off one wave clock of 887) and wraps into `StartFrame..EndFrame`. A first
  attempt preloaded one shared frame across all tiles, which fixed only the zero-offset tile and left
  the others streaming — the snap persisted and looked identical. Use
  `AGridLODActor::PreloadAroundCurrentFrame`, which asks each tile for the frame it actually needs.
- **Blueprint force nodes bypass the C++ suppression gate.** `BuoyancyBP`,
  `WeightDistributionBP` and `JetEngine_Blueprint` call `AddImpulseAtLocation` directly from their
  tick graphs, so gating the C++ appliers is not enough: they spam
  "surfboard has to have 'Simulate Physics' enabled" every tick. Harmless (a no-op on a kinematic
  board) but wrong in principle. Branch each on the `Are Rails Driving The Board` BlueprintPure node.
- **Discrete state must be REPLAYED, not SAMPLED.** The rider AnimBP is a chain
  (Paddle -> Cobra -> PopUp -> auto Surf) with no transition between non-adjacent states. Playback is
  time-indexed and may skip rows, and a recorded Cobra can last ~10 ms when the autopilot fires its
  cobra and pop-up steps on the same tick — so at 60 fps a frame may step clean over it. Pushing
  PopUp while the machine is still in Paddle matches no rule and the rider stays in Paddle for the
  whole intro. Whether a frame lands in that window is frame-timing luck, so it presents as
  *intermittent* — the one thing this spec exists to eliminate. Fix: advance at most one link per
  tick and never push Surf (the machine enters it via its own time-based transition when the PopUp
  clip ends; pushing it would desynchronise AnimState from the machine's real state, which the
  hip-stab gate keys on). Generalise the lesson: continuous quantities can be interpolated or
  sampled, discrete ones must be walked.
- **The rider animation does not advance on its own.** In a playable level every autopilot is
  `enabled=false`, so `ApplyStep`/`PushRiderAnimState` never run and the rider sits in Paddle for the
  whole intro. The state is therefore recorded as trace column 33 (`rider_anim`) and pushed during
  playback — the animation has to be part of the recording, not a side effect of the autopilot.
- **A kinematic target must be set every active frame** or Chaos's `Reset` mode zeroes `V`/`W` the
  following frame.
- **Solver-derived velocity magnitude is frame-scaled** (measured min 0.53×, max 1.83×, mean 1.04×).
  Correct on average, wrong on any single frame — never sample it for the resume, and treat it as
  noisy input anywhere a threshold is applied to it.
- **The board's body must satisfy `CanSimulate`** for the kinematic-target branch to be taken —
  `bIsSimKinematic` requires it. This is the one link in the spike that source reading cannot confirm;
  verify on the actual board component first (see open question 5).
- **Do not finite-difference velocities** from replay rows.
- **The recorded intro is a maintained asset.** Any wave-data or propulsion-tuning change can
  invalidate it. The `HANDOFF STATE` wave-relative diagnostics are the audit; re-record when they
  drift.
- **`bManualFrameControl` must be released** at the transition, or the wave stays pinned for the
  whole ride.
- **`SecondsElapsed` desync**: on a frame-jumped run, time wave events by `CurrentFrame`.
- **GridLOD mesh preload** happens around the pre-jump frame at BeginPlay; watch for a brief
  placeholder pop at the start of the intro, and widen the preload rather than moving the jump if it
  appears (same finding as skip-paddle-intro).
- **Editor worlds skip kinematic targets** (`bEditorWorld` check in `SetBodyTransform`). PIE and
  `-game` are unaffected, but do not try to validate this from a non-PIE editor viewport.

## Recording a reference intro

1. **Enable the intro autopilot in the umap.** The `surf.autopilots` filter force-enables a disabled
   autopilot only when its `TestName` *matches a token*, and `ShouldRunAutopilot("")` is false by
   design — so an intro autopilot with an empty `TestName` can never be selected by the filter and
   must be enabled by hand. Remember to disable it again before shipping: in a playable level an
   enabled autopilot fights player weight input.
2. `$env:EXTRA_ARGS = "-RecordIntro"` (add `-RecordIntroSeconds=N` for a fixed window instead of
   stopping at handoff — needed when the pawn's wired autopilot is not the one running, because then
   handoff fires almost immediately and the trace captures nothing).
3. Play/run. The recorder **rebases at the wave-clock injection**, discarding the deferred-Start dead
   zone, so the trace starts at the injected frame.
4. Copy the CSV to `Content/InputTraces/` and point `RailsIntroTrace` at it.

**The live recording run still shows the wave jump — that is expected and is not the product.** The
autopilot injects `StartWaveFrame` ~1 s into a live run by design; moving that to BeginPlay was tried
and rejected in [skip-paddle-intro.md](skip-paddle-intro.md) for two measured reasons (the board sat
undriven through the catch and missed it; and a BeginPlay clock jump leaks into filtered test runs on
the same level). The rails path does not inherit the jump because it sets the clock from row 0 at
BeginPlay, before anything renders — the recorder simply discards the part of the source material
that contained it.

## Shipping the trace to a device

`RailsIntroTrace` resolves **Content/ first, then Saved/** (`ResolveRailsTracePath`). That ordering is
the whole point: `Saved/` is where `-RecordIntro` writes and is fine on PC, but on Android it is the
app's own writable directory and will never contain a PC-recorded trace.

For a packaged build the trace must be **both**:

1. under `Content/` — e.g. `Content/InputTraces/intro-reference.csv`, with `RailsIntroTrace` set to
   `InputTraces/intro-reference.csv`; and
2. listed in `Config/DefaultGame.ini` as
   `+DirectoriesToAlwaysStageAsUFS=(Path="InputTraces")` — **already added**. A CSV is not a UAsset,
   so `DirectoriesToAlwaysCook` does not reach it; non-asset files need UFS staging to enter the pak.

Miss either and `StartRails` falls back to the live intro. That fallback is deliberate (a stale path
must not break the ride) but it is invisible to a player, so the failure logs at **Error** naming both
candidate roots and the packaging requirement — one logcat line should be enough to diagnose it on
device. Verified 2026-08-25: Content-root load succeeds, and a missing trace produces the error and
falls through cleanly.

## Open questions

1. **Where does the rails intro begin?** At the very start of the ride, or after the catch with the
   existing trigger-driven steps kept ahead of it? Starting at the beginning maximises determinism and
   is the straightforward reading of the goal; keeping the catch live preserves the current intro's
   character. The observed pre-handoff spread argues for the beginning.
2. **Does anything else need stamping?** Pose, velocities and wave frame are the state vector as far
   as this analysis goes, but the `AFluidDynamics` smoothing accumulators are also state. FR2/FR3
   should converge them naturally; if a force step is still visible (AC4), they may need priming.
3. **Recording provenance** — is the reference intro recorded on PC or on a phone, and is the same
   recording right for both? Start with one and check the other.
4. ~~**Does the surfboard's body satisfy `CanSimulate` while kinematic?**~~ **Resolved (2026-08-24):**
   yes — `bSimulatePhysics=1` on the board's body and the probe measured correctly-derived velocity
   direction on 262/262 samples. No engine change needed. The probe did expose the frame-time
   magnitude scale, which is why FR10 exists.
6. **How should the force pipeline read velocity during the rails?** Raw solver-derived (jittery
   magnitude), smoothed over a few frames, or fed the recorded value directly. `AmountPlaning`'s
   speed threshold is the consumer most likely to misbehave on raw values. Decide with a measurement
   once the rails driver exists.
5. ~~**Does the rider mannequin need the same treatment?**~~ **Resolved (2026-08-24):** no. The rider
   is pure animation until the player takes control; its physics only becomes relevant after the
   transition. It is attached to the board and follows the pose track for free, so the rails need no
   special handling for it.

## Follow-up work (separate specs)

- **Frame-rate-correct force integration.** Replace the discarded-dt clamp with leftover-dt
  accumulation so sub-30 fps devices receive the full impulse. Touches every force application site
  and needs a full snapshot pass. This spec makes the *start* of the ride identical everywhere; that
  one is what keeps the rest of the ride comparable.
