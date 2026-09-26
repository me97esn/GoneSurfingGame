# Spec: Input Trace Recording & Replay Autopilot

## Overview
Record the player's raw weight-shift input — phone tilt angles, or stick/mouse deflection on PC fallback — tick-by-tick on the phone, transfer the resulting trace to the PC, and play it back via a new replay-style autopilot. The point is to be able to **tune surfboard forces against real phone input** on a PC editor where iteration is fast, instead of needing to redeploy to the device for every coefficient change.

Paddle and turn inputs are *not* recorded: in the current control surface ([surfing-controls.md](surfing-controls.md)), the cinematic intro autopilot handles the catch-the-wave phase and the player's only live input during the ride is weight shift. `IA_PaddleForward` and `IA_Turn` are wired but unused; if they become player-driven later, the trace schema gets new columns then.

## Objective
Close the loop between "the autopilots feel great on PC, but actual phone control feels bad" by giving the PC tuning workflow a phone-flavored input signal to iterate against. Tuning *is* possible on the phone, but iteration there (rebuild → deploy → re-test on the wave → adjust → repeat) is much slower than in the editor.

## Background
The existing autopilots (`AStateTriggerAutoPilot`) are condition-driven state machines — they advance step on pitch/roll/velocity/planing thresholds and write `weightRight` / `weightNose` on `AWeightDistribution`. They produce *clean* control signals. Real phone input via [surfing-controls.md](surfing-controls.md) (tilt → `CurrentWeightOffset` → `amountInFront` / `amountToTheRight`) is *noisy and high-frequency* by comparison, and the surfboard physics that feels right under the autopilot's clean signal feels poor under the player's actual tilt trace.

We want to make the *player's actual tilt trace* available in the PC editor, where iteration is fastest. The mechanism:
1. On-device, record every tick's player input to a CSV in `Saved/`.
2. ADB-pull the CSV to the PC repo.
3. A new `AInputReplayAutoPilot` actor reads the CSV and drives the same downstream state the player would (`CurrentWeightOffset` → `amountInFront` / `amountToTheRight`) each tick of a PC run.
4. Tune forces in [SurfboardPawn.h](../Source/GoneSurfing/SurfboardPawn.h), [FluidDynamics.cpp](../Source/GoneSurfing/FluidDynamics.cpp), etc. against the trace — same coefficients are reachable on the phone, just at a far slower iteration cadence.

This is intentionally *not* a deterministic regression-test mechanism — physics has large run-to-run variance (see [reference_snapshot_run_variance.md] in auto-memory). The value is qualitative: same recorded player intent, different physics tunings, side-by-side feel comparison.

## Scope

### In scope
- Per-tick recording of player input on-device, written to a CSV under `Saved/InputTraces/`.
- File transfer is **manual** (ADB pull); no in-game upload mechanism.
- A new `AInputReplayAutoPilot` actor that reads a trace and drives the pawn/weight state per tick.
- Integration with `RunGameAndCollectLogs.bat` so a trace can be replayed headlessly the same way other autopilots run.

### Out of scope
- Deterministic physics. Even with identical inputs, the existing snapshot variance applies. Replays are tuning aids, not regression tests.
- Network streaming of input from device to PC (live remote play). Maybe later; ADB pull is enough for v1.
- Multi-player or multi-board recording. Single-pawn, single-board.
- Recording or replaying camera mode / restart / cosmetic actions.
- Re-running the Enhanced Input axis-mapping itself (e.g., gyro → IA_Weight on a stick fallback) — we record at the layer *just above* axis mapping; see "What gets recorded" below.

## What gets recorded

We want the recording at the layer that preserves the most downstream tunability without dragging platform-specific axis-mapping code into the replay. That layer is:

- **For tilt weight (Android default)**: the **decomposed pitch/roll angles in degrees vs `NeutralGravity`**, *before* deadzone / sensitivity / clamping. This is the output of the gravity-vector decomposition in [SurfboardPawn.cpp](../Source/GoneSurfing/SurfboardPawn.cpp) `UpdateTiltWeight()` (the `PitchDeg` / `RollDeg` locals around line 913). Recording at this layer lets us re-tune `TiltAngleDeadzoneDegrees`, `TiltPitchDegreesForFullDeflection`, `TiltRollDegreesForFullDeflection`, `bInvertTiltPitch`, `bInvertTiltRoll` from the same trace.
- **For stick / mouse weight (fallback)**: the `IA_Weight` action value (Vector2D, ±1 per axis), as passed to `WeightInput(const FInputActionValue&)`.

Both weight layers are recorded into the same row. On Android+Tilt sessions, the stick columns will be 0/empty and the tilt columns populated; on PC/stick sessions, the reverse. The replay reads whichever pair has data.

### CSV schema

`Saved/InputTraces/<sessionName>.csv`:

```
t,tilt_pitch_deg,tilt_roll_deg,stick_x,stick_y,source
0.000,  0.12, -0.05, 0.00, 0.00, tilt
0.016,  0.18, -0.03, 0.00, 0.00, tilt
0.033,  0.31,  0.10, 0.00, 0.00, tilt
...
```

- `t` — seconds since record start, monotonic.
- `tilt_pitch_deg`, `tilt_roll_deg` — pitch and roll delta from `NeutralGravity`, signed (positive sign convention matches `UpdateTiltWeight()`). Always written when tilt is the source; zero when stick is the source.
- `stick_x`, `stick_y` — `IA_Weight` Vector2D as written by `WeightInput()`. Zero when tilt is the source.
- `source` — `"tilt"` or `"stick"` or `"mouse"`. Replay branches on this.

Header row is written once at the start of the file. Sampling cadence matches game-thread tick (typically 60 Hz on phone, variable on PC) — record one row per `Tick()`, not on a fixed timer, so the trace is faithful to the actual frame timing of the recording session. Replay interpolates linearly between samples when the replay-tick time falls between two recorded `t`s.

### Recording session metadata

Top of the CSV file, before the header, includes commented metadata so a trace is self-describing:

```
# session=phone-2026-06-03-14-22-08
# platform=Android
# device=Pixel-7
# neutral_gravity=(0.012, -0.918, -0.395)
# tilt_pitch_for_full=18.0
# tilt_roll_for_full=18.0
# tilt_deadzone_deg=2.0
# invert_pitch=true
# invert_roll=false
# weight_auto_center=0.3
# map=Boards_on_flat_water
# build=GoneSurfing-2026-06-03-...
t,tilt_pitch_deg,tilt_roll_deg,stick_x,stick_y,source
```

The pawn's tilt parameters at *record* time are captured so a replay can either reuse them or deliberately override them.

## Architecture

### Recording layer (in `ASurfboardPawn`)

New UPROPERTYs:

```cpp
// Input trace recording
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InputTrace")
bool bRecordInputTrace = false;     // Default off; enabled per level/session

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InputTrace")
FString InputTraceSessionPrefix;    // Optional. Final filename is "<prefix>-<timestamp>.csv"
                                    // (or just "<timestamp>.csv" if prefix is empty).
                                    // The timestamp suffix is always added so every level
                                    // start — including every Restart Level — produces a new file.

UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InputTrace")
float MaxRecordingSeconds = 120.0f; // Safety cap; recorder stops + flushes after this
```

Internal state:
- `FString InputTraceBuffer` — accumulated CSV rows, flushed periodically (every ~10 s of game time) and on stop. The 10 s window means a crash can lose up to 10 s of input; that's acceptable for a tuning tool, and it keeps disk-write frequency low so on-device recording doesn't perturb frame timing.
- `float InputTraceElapsedTime` — `t` axis.
- `bool bInputTraceActive` — true between record-start and record-stop.
- Latest values of the four input quantities, cached by the input handlers and tilt poll, sampled in `Tick()`.

Lifecycle:
- **Start**: when `bRecordInputTrace == true` *and* `bPlayerControlsEnabled` transitions false → true (same trigger as tilt calibration; see `OnPlayerControlsEnabled()`). Writes the metadata block + header to the file. The neutral-gravity vector snapshotted there is the same one used as the recording's tilt reference.
- **Sample**: every `Tick()`, append one row to `InputTraceBuffer`. `WeightInput()` and `UpdateTiltWeight()` cache their latest values into member fields the recorder reads. A row's stick / tilt values are whatever the last event delivered before this tick — sticky between input events, which is what we want.
- **Flush**: every ~10 s of game time, append `InputTraceBuffer` to disk and clear the buffer. Less frequent than the original 1 s plan — phone-side disk writes are coarse and we'd rather buffer up to 10 s in memory than perturb frame timing. Periodic flush still bounds crash loss to ≤10 s.
- **Stop**: when any of:
  - `bPlayerControlsEnabled` flips back to false (e.g., camera-cuts or end-of-ride logic; out of scope but should be respected).
  - `bRecordInputTrace` flips false at runtime (Blueprint or debug cmd).
  - `InputTraceElapsedTime >= MaxRecordingSeconds`.
  - `EndPlay()`.
  - On stop: final flush + close file. Log path at `Display` level for easy `adb pull`.

File path: `FPaths::ProjectSavedDir() / TEXT("InputTraces") / (SessionName + TEXT(".csv"))`, where `SessionName` is built once in `BeginPlay()` as `<InputTraceSessionPrefix>-<timestamp>` (e.g. `phone-2026-06-03-14-22-08`) or just `<timestamp>` when the prefix is empty. Timestamp resolution to the second is sufficient — `Restart Level` reloads the world and re-runs `BeginPlay()`, so each restart picks up a new timestamp and writes a new file with no risk of overwriting the previous one. On Android, the directory resolves under the per-app sandbox (`/storage/emulated/0/Android/data/<pkg>/files/UnrealGame/.../Saved/InputTraces/`).

### Replay layer: new `AInputReplayAutoPilot` actor

New class in [Source/GoneSurfing/InputReplayAutoPilot.h](../Source/GoneSurfing/InputReplayAutoPilot.h) + `.cpp`. Modeled on `AStateTriggerAutoPilot` for naming consistency and so [run-game-and-collect-logs.md](run-game-and-collect-logs.md) tooling treats it the same.

```cpp
UCLASS()
class GONESURFING_API AInputReplayAutoPilot : public AActor
{
    GENERATED_BODY()
public:
    AInputReplayAutoPilot();

    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool enabled = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool includeInSuite = true;

    /** Path under <Project>/Tests/InputTraces/ of the CSV to replay. Required. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString TraceFileName;

    /** Reference to the SurfboardPawn whose inputs we drive. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) class ASurfboardPawn* TargetPawn = nullptr;

    /** Reference to the WeightDistribution actor (mirrors StateTriggerAutoPilot.surfboard).
     *  Either resolved from TargetPawn->WeightDistribution at BeginPlay, or set explicitly. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) class AWeightDistribution* WeightDistribution = nullptr;

    /** Reference to the surfboard actor for force application. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) AActor* surfboard = nullptr;

    /** If non-empty, the replay also records a trajectory CSV under
     *  Saved/Tests/latest/<TestName>.csv — same as StateTriggerAutoPilot. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString TestName;

    /** Quit the editor in -game mode when the trace ends. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bAutoQuitOnComplete = true;

    /** If true, re-run the tilt-decomposition pipeline using TargetPawn's *current* tilt UPROPERTYs.
     *  If false, use the recorded NeutralGravity + tilt params from the CSV header verbatim. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite) bool bUseCurrentTiltParams = true;

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaTime) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
    struct FTraceRow { float t, tiltPitchDeg, tiltRollDeg, stickX, stickY; FString source; };
    TArray<FTraceRow> Trace;
    int32 NextRowIdx = 0;
    float PlaybackTime = 0.0f;
    bool bLoaded = false;
    bool bFinished = false;

    void LoadTrace();
    void ApplyInterpolatedRow(float t);
};
```

Behavior:
- `BeginPlay()`: load and parse the CSV into `Trace`. Resolve `TargetPawn` / `WeightDistribution` if not explicitly set.
- `Tick()`: advance `PlaybackTime`; find the row pair straddling current `t`; linearly interpolate `tilt_*` and `stick_*`; then write the result into `CurrentWeightOffset` on the target pawn and let the pawn's own `Tick()` propagate to `WeightDistribution` (same code path the player's input would feed).
- When `PlaybackTime` passes the last row's `t`: set `bFinished = true`; if `bAutoQuitOnComplete`, call `QuitGame`; if `TestName` was set, flush the trajectory CSV (reuse the snapshot-test recorder pattern from `StateTriggerAutoPilot::FlushRecorder`).

### Conflict with player input

`AInputReplayAutoPilot` is a **dev-only** actor — it lives only in PC tuning levels and is `enabled=false` or absent in any level shipped to players. Same rule as `AStateTriggerAutoPilot`. **No runtime gating in the pawn** — we do not detect "is a replay active?" to disable input handlers. If both run in the same level simultaneously they will fight; the convention is that this never happens in a shipped build.

The recording layer in the pawn is independently gated on `bRecordInputTrace`, which defaults off — recording does not run in test levels.

### Headless runs

`RunGameAndCollectLogs.bat` already launches any autopilot in the loaded map; the replay autopilot just plugs into that infrastructure. Two practical adjustments to that flow:

- The trace file needs to be in a location the editor can read on PC — propose copying pulled traces to `GoneSurfing/Tests/InputTraces/` (checked in as needed) and have `AInputReplayAutoPilot::TraceFileName` resolve relative to that.
- The default-suite-from-`reference_run_all_autopilots.md` flow should *include* replay autopilots that have `includeInSuite=true`, so adding a new trace to a level adds it to the suite by default.

## Acceptance Criteria

### AC1: Recording starts at handoff
With `bRecordInputTrace=true` on the pawn in a playable level, a CSV at `Saved/InputTraces/<sessionName>.csv` is created the first tick `bPlayerControlsEnabled` transitions true. The file contains the metadata block + header before any data row.

### AC1b: Restart produces a new file
Pressing the Restart button (`ASurfboardPawn::RestartLevel`) during or after a recording reloads the level. The new session's `BeginPlay()` picks up a fresh timestamp, so a *new* CSV is created at the next handoff; the previous run's file is left intact on disk. Repeated restarts in a single phone session produce a sequence of distinct files (e.g. `phone-2026-06-03-14-22-08.csv`, `phone-2026-06-03-14-23-41.csv`, ...).

### AC2: Faithful sampling
A row is appended every game-thread tick while recording is active. `t` advances monotonically; no duplicate `t` values. Last `t` minus first `t` matches the wall-clock recording duration to within one tick.

### AC3: Tilt session captures usable angles
In an Android tilt session, `tilt_pitch_deg` and `tilt_roll_deg` columns are populated; `stick_x` / `stick_y` are zero; `source` is `"tilt"`. The metadata block lists `neutral_gravity`, and tilt parameter values match the pawn's UPROPERTYs at session start.

### AC4: Stick session captures usable axes
In an Android stick-fallback or PC RMB-mouse session, `stick_x` / `stick_y` are populated; tilt columns are zero; `source` is `"stick"` or `"mouse"` accordingly.

### AC5: Periodic flush survives crash
After ≥10 s of recording, killing the game without a clean shutdown leaves a partial-but-valid CSV on disk (header + N rows). Reopening the file in a viewer parses successfully. Crashes within the first 10 s may leave only the metadata + header on disk (no data rows) — acceptable.

### AC6: Safety cap
If recording reaches `MaxRecordingSeconds`, it stops, the file is flushed, and the pawn logs the final path at `Display` level. Further game ticks do not append.

### AC7: ADB pull docs
The spec implementation includes a one-line `adb pull` example in `HOOKS.md` or a sibling readme that successfully transfers `Saved/InputTraces/*.csv` from the device to `GoneSurfing/Tests/InputTraces/` on the PC.

### AC8: Replay loads
`AInputReplayAutoPilot` with `TraceFileName` set to an existing trace successfully parses it at `BeginPlay`. A parse failure logs the line number and a snippet; the actor disables itself and the level continues without crashing.

### AC9: Replay drives weight
In a level with `AInputReplayAutoPilot.enabled=true` and `TargetPawn` set, the pawn's `CurrentWeightOffset` (and downstream `amountInFront` / `amountToTheRight`) match the replayed trace's values at the corresponding `t` to within interpolation tolerance.

### AC10: Replay completion
When `PlaybackTime` passes the last row's `t`, `bFinished` becomes true. If `bAutoQuitOnComplete`, the game quits. If `TestName` was set, a trajectory CSV is flushed under `Saved/Tests/latest/<TestName>.csv` (same format as `StateTriggerAutoPilot`).

### AC11: Tilt-parameter override
With `bUseCurrentTiltParams=true`, changing `TargetPawn`'s tilt UPROPERTYs (e.g., `TiltPitchDegreesForFullDeflection` from 18° to 30°) before replay changes the resulting `CurrentWeightOffset` trajectory even though the underlying recorded `tilt_pitch_deg` series is unchanged. With `bUseCurrentTiltParams=false`, the same change has no effect.

### AC12: Replay autopilot is dev-only
`AInputReplayAutoPilot` is treated like `AStateTriggerAutoPilot`: `enabled=false` or absent in any level shipped to players. No pawn-side detection of an active replay autopilot — the convention against fighting *player input within a level* is content-level, not code-level.

Additionally, the actor is **editor-only at runtime**: `BeginPlay()` hard-disables itself (`enabled=false`, early return) when `!WITH_EDITOR`, so a level that still carries the actor when packaged for Android never replays on-device. `WITH_EDITOR` is true in the editor and in `-game` mode launched from the editor binary (so PC headless runs are unaffected), and false in a packaged Android build.

### AC13: Headless run integration
`RunGameAndCollectLogs.bat` loading a map with a replay autopilot exits cleanly when the trace ends (autopilot calls `Quit`), within the existing 600 s safety net.

## Test plan

1. **On-device record** — deploy to phone, enable `bRecordInputTrace` on the level's pawn, surf for ~30 s, return to dock, verify CSV in `Saved/InputTraces/` via `adb shell ls`.
2. **Transfer** — `adb pull` the trace into `GoneSurfing/Tests/InputTraces/`.
3. **PC replay** — open a test level with an `AInputReplayAutoPilot` referencing the trace, hit Play, observe the board moving the way it did on the phone.
4. **Force tuning iteration** — adjust a coefficient (e.g., `forwardsThrustCoefficient` on bottom hydrofoil actors), re-Play, compare trajectory snapshot CSV via `Compare.ps1` (mostly to *see* the trajectory, not because the comparison is meaningful given run variance).
5. **Tilt-param tuning iteration** — with the same trace, change `TiltPitchDegreesForFullDeflection`, re-Play, confirm the board carves more/less aggressively for the same recorded tilt.

## Open Questions

1. **Where do PC-side trace files live in the repo?** Proposed: `GoneSurfing/Tests/InputTraces/`. Whether to check them in: case-by-case. A small "golden" trace per failure mode (e.g., `nose-dive-trace.csv`) makes sense; raw playtest dumps don't.
2. **CSV vs binary**: CSV is human-readable and matches the snapshot-test format, at the cost of file size. A 2-minute trace at 60 Hz is ~7200 rows — well under any size concern. Stay with CSV.
3. **Time-axis alignment**: should replay match the *frame timing* of the original recording or just the wall-clock seconds? Going with wall-clock + interpolation; revisit if the resulting motion looks "off-cadence" vs the device.
4. **Replay-vs-record tilt-axis sign convention**: must be identical. Risk if `UpdateTiltWeight` decomposition changes after a trace was recorded — the trace becomes mis-interpretable. Plan: bump a `format_version` field in the CSV metadata if/when this changes.
5. **Multi-board levels**: out of scope. If we ever record/replay multiple pawns, the trace file becomes a directory.
6. **Live network replay (PC drives phone, or phone streams to PC)**: out of scope for v1. Useful follow-on for "iterate physics without redeploy" if record/replay loop ends up being too slow.
7. **Trace editing**: trim, splice, loop. Out of scope; a separate offline tool if/when needed.
8. **Replay reset / restart**: the AC list assumes one-shot replays. If we want loop / scrub, that's a follow-on.
9. **What about the autopilot's intro-pop-up phase?** A playable session starts with `AStateTriggerAutoPilot` driving the pop-up before player control hands off. The trace's `t=0` aligns with the player-control handoff (consistent with the tilt-calibration moment), so the intro autopilot still runs on the PC side and the replay picks up where the player would. The PC test level needs the *same* intro autopilot as the record-time level, or initial state will be off.

## Implementation Phases

- **Phase 1** — Recording-only. Add UPROPERTYs + recorder state to `ASurfboardPawn`. Wire start/sample/flush/stop. Confirm a clean CSV from a manual playtest on PC (RMB-mouse session) before going to device.
- **Phase 2** — Android validation. Deploy, record a real tilt session, `adb pull`, sanity-check the CSV contents (signs, magnitudes, cadence).
- **Phase 3** — Replay autopilot. New `AInputReplayAutoPilot` class. Parse + linear interpolation + apply-to-pawn. Test against the Phase 2 trace.
- **Phase 4** — Tilt-param override mode. Verify `bUseCurrentTiltParams` toggle changes the resulting trajectory for the same trace.
- **Phase 5** — Suite integration. Hook into the default-autopilot-suite path so a trace+autopilot pair runs under `RunGameAndCollectLogs.bat` without bespoke wiring per trace.
- **Phase 6** — Force-tuning loop. Capture 2–3 "golden" trace cases (clean ride, nose-dive recovery, hard carve), commit under `GoneSurfing/Tests/InputTraces/`, document the iteration loop in this spec's Test Plan.

## Related Files

- New: [Source/GoneSurfing/InputReplayAutoPilot.h](../Source/GoneSurfing/InputReplayAutoPilot.h) / `.cpp`
- New: `GoneSurfing/Tests/InputTraces/` (directory; possibly with one or more committed traces).
- Modified: [Source/GoneSurfing/SurfboardPawn.h](../Source/GoneSurfing/SurfboardPawn.h) / [.cpp](../Source/GoneSurfing/SurfboardPawn.cpp) — recorder + replay-gate logic.
- Sibling: [Source/GoneSurfing/StateTriggerAutoPilot.h](../Source/GoneSurfing/StateTriggerAutoPilot.h) / [.cpp](../Source/GoneSurfing/StateTriggerAutoPilot.cpp) — pattern reference (trajectory recorder, `bAutoQuitOnComplete`, `TestName` semantics).
- Related spec: [surfing-controls.md](surfing-controls.md) — defines the inputs being recorded.
- Related spec: [run-game-and-collect-logs.md](run-game-and-collect-logs.md) — the headless run flow the replay plugs into.
- Related spec: [autopilot-wave-relative-triggers.md](autopilot-wave-relative-triggers.md) — autopilot conventions reference.
