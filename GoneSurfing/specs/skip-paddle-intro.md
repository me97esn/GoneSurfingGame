# Spec: Skip the paddle phase — start the ride at the cobra pose

## Status
- [x] Spec drafted (2026-08-18)
- [x] C++ implemented: `StartVelocity` / `StartAngularVelocityDeg` / `StartWaveFrame` on
  `AStateTriggerAutoPilot`, START-POSE SNAPSHOT log line, "Wait..." cue removed, tutorial
  trimmed to 3 steps. Revised same day: injection moved from BeginPlay into `Start()`
  (atomic with step 0) after the first in-level attempt missed the catch — see "Where the
  injection is applied".
- [ ] Editor: snapshot values measured from a run and entered on the intro autopilot; paddle
  steps deleted; first step re-tagged Cobra (user, in-editor)
- [ ] Player-validated: ride starts at cobra with no dead wait, pop-up → handoff feels right

## Motivation

Use testing showed that players — even ones who read the instructions — try to control the
board during the paddle/pop-up intro. The paddling phase is a leftover from the initial
design: it exists because the autopilot must wait for the wave to arrive, not because the
wait is fun. Cutting it means:

- The ride starts seconds after pressing Start, at the **cobra pose**, with the wave already
  under the board.
- Tutorial step 4 ("You take control" / the surfer paddles in...) becomes redundant — the
  no-control window shrinks to the ~2s cobra→pop-up beat, short enough to need no
  explanation. The tutorial becomes 3 steps, each an action the player performs.
- The steady **"Wait..."** ride cue becomes pointless and is removed. The one-shot gold
  **"…and surf!"** flash is kept — it is not an instruction but a *go signal* marking the
  exact moment control begins.

Player input stays disabled until the pop-up completes (unchanged handoff): the catch is the
most delicate moment of the ride, the autopilot is still driving `AWeightDistribution`, and
letting a new player shove weight around mid-catch would blow the takeoff.

## Design: inject the wave-catch state instead of simulating up to it

The intro wait is wave-arrival-bound — the whole autopilot design waits for the wave. So the
fix is **state injection**: measure the full world state at the moment the cobra step
activates in a normal run (wave frame + board pose + board velocity, all from the same
instant so they are self-consistent), then initialize the level directly to that state. The
physics simply continues from there rather than re-converging.

### New fields on `AStateTriggerAutoPilot`

All default to "no effect" so every existing autopilot (including all snapshot tests and
their baselines) is untouched:

| field | default | effect when set |
|---|---|---|
| `StartVelocity` (cm/s, world) | zero | Board's linear velocity is set to this in `ApplyStartPose()` instead of being zeroed. Also suppresses the deferred `Start()`'s re-zero — the board is *supposed* to be moving, and yanking velocity back 1s in would visibly hitch. |
| `StartAngularVelocityDeg` (deg/s, world) | zero | Same, for angular velocity. Injecting zero spin when the board is pitching on the face causes a visible first-frame hitch, hence measured. |
| `StartWaveFrame` | −1 | Written once to `WaterController.CurrentFrame` (reflection, same property the replay writer uses) when the driving autopilot applies its start pose. −1 = leave the wave clock alone. |

(`StartLocation` / `StartRotation` already exist and are applied in `ApplyStartPose()`.)

### Why a one-time `CurrentFrame` write sticks

The WaterController BP advances `CurrentFrame` by **relative increment** each tick. Evidence:
`ASurfboardPawn::SetWaveFrame`'s replay notes — without `bManualFrameControl` the wave
"starts on the recorded frame and merely free-runs from there", which is only possible if the
BP increments from the current property value rather than recomputing an absolute
frame-from-elapsed-time. So a single write shifts the whole timeline and the wave free-runs
from the injected frame. We deliberately do **not** set `bManualFrameControl` (that pins the
frame; we want free-run).

`SecondsElapsed` is left untouched: it is accumulated separately by the BP and is only used
as a logging/onscreen time anchor (`SharedCalculations` crossing log, trace timing). After a
frame jump, `SecondsElapsed` no longer maps to `CurrentFrame` the way it does in an
un-jumped run — when timing wave events in logs from a jumped run, use `CurrentFrame`.

### Where the injection is applied: atomically in `Start()`, NOT BeginPlay

**(Revised 2026-08-18 after the first in-level attempt.)** The first implementation injected
everything at BeginPlay; the board consistently missed the catch and surfaced behind the
breaking wave. The logs showed why: the frame write worked (886→1019, then free-running at
the normal ~25 fps — the relative-increment assumption is confirmed), but `Start()` is
deferred ~1s (the `surf.autopilots` CVar wait), so the board sat in the *live catch moment*
for a full second with **no autopilot driving it** — no step weight, no jet thrust. The
measured run goes cobra→pop-up in <1s, so the whole catch window fell inside that dead zone;
the undriven board was shoved up over the back of the wave (snapshot: +57 cm X, +54 cm Z,
Vz −72 falling vs the injected +62 rising).

A BeginPlay injection is also wrong for tests: a filtered test run on the same level
force-disables the intro autopilot ~1s in, but a BeginPlay wave-clock jump would persist and
shift the test's wave timing.

So the rule is: **measured state and driving must begin on the same tick.** In `Start()`,
when any injection field is set, the full start state (pose + velocities + wave frame) is
re-applied immediately before `ApplyStep(steps[0])`. BeginPlay keeps only the legacy
location/rotation pre-positioning (no frame jump, no velocity persistence — whatever the
board does during the ~1s defer is snapped away by the re-injection; ~tens of cm, barely
visible). The step-0 START-POSE SNAPSHOT line therefore echoes the pasted values on an
injected run — a built-in correctness check.

**Adaptive defer:** the ~1s `Start()` defer exists only for the headless-runner CVar race
(`surf.autopilots` arrives via `-ExecCmds` after BeginPlay), and those launches are exactly
the `-unattended` ones. So the defer is `FApp::IsUnattended() ? 1.0s : 0.05s` — interactive
runs start on the next tick (still after every BeginPlay, including the WaterController BP's),
which killed the visible "plays for a second at the wrong frame, then resets" artifact of the
1s gap. Interactive sessions never set the filter via `-ExecCmds`; a console-set filter
persists across PIE sessions and is still honored.

**Injection reproduces board state, NOT step config.** The step that runs at the injected
moment must itself carry everything the *original* step applied at that moment — weights
**and `jetEngineOn`/`jetMultiplier`**. A trimmed cobra step without the jet the original
cobra step had leaves the board under-powered and the wave rolls under it. `ApplyStep` now
logs the full step config (`weightRight/weightNose/jet=ON x1.00/rider`) so this is auditable
from any run's log — the previous jet log compared after the `currentStep` copy and could
never fire.

Consequence of Start()-time injection: the Start-menu backdrop shows *pre-jump* water
(frame 886), and the wave snaps to catch position one tick after Start is pressed. If the
backdrop wave ever matters, that's a separate cosmetic problem to solve without
re-introducing the dead zone.

`Start()`'s velocity re-zero (settle-drift cleanup) still runs when no injection field is
set — legacy behavior is preserved exactly for every existing autopilot.

### Measurement workflow: the START-POSE SNAPSHOT log line

Every step activation (and step 0 in `Start()`) logs one copy-pasteable line, always on
(once per step, cheap, permanently useful — retuning the wave or catch means one normal run
to re-measure):

```
StateTriggerAutoPilot: START-POSE SNAPSHOT @ step 3 (cobra pose): StartWaveFrame=1004
  StartLocation=(X=...,Y=...,Z=...) StartRotation=(Pitch=...,Yaw=...,Roll=...)
  StartVelocity=(X=...,Y=...,Z=...) StartAngularVelocityDeg=(X=...,Y=...,Z=...)
```

Values are the surfboard mesh's world transform + physics velocities and the
WaterController frame at that instant, named identically to the UPROPERTYs they belong in.
Workflow: play one normal (un-trimmed) run, find the line for the step *before* which you
want the game to start, paste the values into the intro autopilot, delete the earlier steps.

Measuring the *activation* instant of the cobra step means the injected state starts exactly
where the cobra pose begins. If a settle transient is ever visible on the first frames,
re-measure from a fraction of a second earlier (the snapshot of the preceding step, or a
temporary extra step) — free insurance, probably unnecessary.

## UI / handoff changes (C++)

- **`SurfboardPawn::UpdatePlayerControlState`**: all `RideCue::ShowWaiting(...)` calls
  removed (both the StateTrigger and legacy-timer paths). `RideCue::PlayGo` self-installs
  via `EnsureCue`, so the "…and surf!" flash still appears at handoff. The
  `RideCue::ShowWaiting` API is left in place (harmless, may be useful for debugging).
- **`StartTutorialOverlay`**: `NumSteps` 4 → 3; the "YOU TAKE CONTROL" title/copy/texture
  case removed. Dots, counter ("n / 3"), and NEXT→START promotion adapt automatically.
  `Content/Images/instructions-controls-hand-over` becomes unreferenced (leave the asset;
  deleting content is not this spec's business).
- Handoff timing machinery (`bFinished` → `ControlDelayAfterAutoPilot` grace → enable) is
  **unchanged** — it just runs seconds earlier.

## Editor — manual setup (cannot be authored from C++)

1. Run the game once as-is; note the START-POSE SNAPSHOT line for the cobra step from
   `Saved/Logs/GoneSurfing.log`.
2. On the intro autopilot in the playable level(s): paste `StartWaveFrame`,
   `StartLocation`, `StartRotation`, `StartVelocity`, `StartAngularVelocityDeg`.
3. Delete the paddle steps. The first remaining step is the cobra step
   (`riderAnimState = Cobra`); give it a short `TimeoutDuration` hold (~1–2s) rather than a
   physics trigger — its physics conditions are already true at injection, and step-0
   activation is immediate by design anyway. **Carry over the original cobra step's full
   config** — `weightRight`/`weightNose` and especially `jetEngineOn`/`jetMultiplier`; the
   injection supplies the board's state, the step supplies its propulsion.
4. `InitialSettleDelay` is skipped automatically when injection fields are set (an injected
   start is mid-action; holding step 0 through the settle window delays every later step past
   the catch window — observed: "Pop up" ~1s late → board pitched up 24° and took the
   straight-down-the-face branch instead of the carve).
5. Give the step *after* the injected one (the pop-up step) a `TimeoutDuration` matching the
   measured gap (~0.8s here) as a backstop: its trigger thresholds were tuned against the
   original approach, and the injected ride's attitude evolution differs slightly (snapped
   vs. mid-slew weight), so a marginal trigger can fire late — and the catch is bistable
   enough that late = a different ride.
6. Leave every test autopilot untouched.

## Picking the injection point: inject into the approach, not the catch

Play-testing the cobra-point injection showed ~70% missed catches vs ~100% for the full
intro. The lesson: the old intro's reliability never came from determinism (frame-time
jitter, streaming hitches after the wave-clock jump, and sub-frame wave phase all vary
run-to-run) — it came from being **closed-loop**. The board approached through a forgiving
regime and the step *triggers* fired whenever the observed state was right, re-synchronizing
every run onto the catch. Injecting exactly at the catch moment is open-loop at the most
bistable instant, so tick noise picks the branch.

Rule: **inject ~1–1.5s before the wave engages, and keep the original trigger-driven steps
from there** (the trimmed sequence starts at the last paddle step with its original config,
and the cobra step keeps its original trigger conditions). The closed-loop segment re-syncs
each run; reliability returns by the same mechanism the full intro had, at the cost of ~1s
of intro.

To find the point: the periodic 0.5s step log now emits the full paste-ready
START-POSE SNAPSHOT line, so any moment of any run is measurable — not just step
boundaries. Dial-in procedure: inject at a known-safe early moment, play a few runs, move
the injection point later using a mid-run periodic snapshot, repeat until reliability drops,
back off one notch.

## Verification findings (2026-08-18, headless)

- Injection verified end-to-end: the step-0 SNAPSHOT echoes the pasted values exactly, the
  wave clock jumps once and free-runs, and the board catches and accelerates — tracking the
  measured PIE run within ~cm for the first ~0.8s.
- **PIE and headless runs take different catch branches even un-trimmed** (committed
  baseline: cobra at frame 1004; the PIE measured run: cobra at 1019) — trigger-timing noise
  selects different rides. So measured values transplant fine across contexts, but
  step-*trigger* timing after injection is the sensitive part (hence the timeout backstop).
- Non-regression: fresh `surf-straight` headless run → `Compare.ps1` OK vs baseline with all
  injection code in place (defaults are a no-op).
- The old full-sequence `pop-up` baseline can't compare against trimmed runs on the raw
  t-axis; once the trimmed intro is player-validated, re-record via `Approve.ps1` (align by
  the `frame` column when comparing across the trim — but mind the wave-loop wrap: frames
  recur each ~192-frame loop, so window the baseline rows by t before joining).

## Acceptance criteria

- **Given** an autopilot with all new fields at defaults, **then** behavior is bit-identical
  to today (snapshot suite: `Compare.ps1` exit 0 on all baselines).
- **Given** `StartWaveFrame >= 0` on the enabled autopilot, **then** the WaterController
  frame jumps once in `Start()` — the same tick step 0 begins driving — and free-runs from
  there (periodic step log shows frames advancing from the injected value).
- **Given** measured values pasted into the intro autopilot and paddle steps deleted,
  **then** the ride reaches the pop-up/handoff within a few seconds of pressing Start, and
  the board's trajectory through cobra → pop-up matches the un-trimmed run (no visible
  snap, sink, or stall at t=0).
- **Given** any run, **then** no "Wait..." text ever appears; "…and surf!" still flashes
  once at control handoff.
- **Given** the tutorial, **then** it shows exactly 3 steps and the final card's button
  reads START.

## Test cases

1. Headless snapshot suite unchanged: `RunGameAndCollectLogs.bat` → all baselines exit 0.
2. PIE with injected values: pressing Start → (~1s defer) wave snaps to catch position under
   the board → cobra → pop-up → "…and surf!" → controls live. Compare wave shape under the
   board at cobra against the same moment in an un-trimmed run.
3. Log check: exactly one `START-POSE SNAPSHOT` line per step activation; on injected runs
   the step-0 line's values echo the pasted values (the injection happens immediately before
   the snapshot — mismatch means the injection didn't apply).

## Gotchas / watch items

- **GridLOD mesh preload**: `AGridLODActor::BeginPlay` preloads meshes around the *pre-jump*
  frame (the jump now happens in `Start()`, after every BeginPlay). The first ticks after the
  jump stream meshes for the new frame lazily — watch for a brief placeholder/pop right at
  ride start. If seen, the fix is to widen/nudge the preload (or preload `StartWaveFrame`'s
  neighborhood for the enabled autopilot), not to move the injection back to BeginPlay.
- **Frame wrap**: the wave loops `StartFrame..EndFrame` (886..1078 by default). Measured
  values are inherently in range; hand-typed ones outside it will wrap like any other frame.
- **`SecondsElapsed` desync** (see above): use `CurrentFrame` to time wave events in logs
  from injected runs.
- The `ESurferAnimState::Paddle` state and its clip slot remain (the enum default and the
  AnimBP entry state) — the intro simply never pushes it once the paddle steps are deleted.
  See specs/surfer-popup-animation-states.md.
- specs/start-screen-tutorial.md is updated by this spec's implementation (3-step copy, ride
  cue section).
