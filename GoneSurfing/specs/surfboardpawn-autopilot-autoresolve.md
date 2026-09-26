# SurfboardPawn: auto-resolve StateTriggerAutoPilot reference

## Bug Description

`ASurfboardPawn` has a single hand-wired `UPROPERTY` pointer to one `AStateTriggerAutoPilot` instance in the level:

```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AutoPilot")
TObjectPtr<class AStateTriggerAutoPilot> StateTriggerAutoPilot;
```

When an autopilot is duplicated in the level editor (Ctrl+W on a `StateTriggerAutoPilotBP` instance) and the original is disabled while the new one is enabled, UE does **not** re-target the pawn's pointer. The pawn keeps pointing at the now-disabled original. This is a recurring foot-gun for the snapshot-test authoring workflow (duplicate an autopilot, change its `TestName`, tweak steps).

## Current Behavior

With a stale ref to a disabled autopilot:

1. [`SurfboardPawn.cpp:904`](../Source/GoneSurfing/SurfboardPawn.cpp) computes
   `bAutopilotActive = StateTriggerAutoPilot->enabled && !StateTriggerAutoPilot->bFinished`. Because `enabled=false` on the stale target, this is `false` from t=0.
2. The pawn skips the autopilot-active early-return and walks straight through the `ControlDelayAfterAutoPilot` handoff timer.
3. Once `bPlayerControlsEnabled` flips true, [`SurfboardPawn.cpp:246-253`](../Source/GoneSurfing/SurfboardPawn.cpp) writes `WeightDistribution->amountToTheRight = 0.5` and `amountInFront = 0.5` every tick (no player input → `CurrentWeightOffset = (0,0)`).
4. Those writes overwrite the *new* (enabled) autopilot's BP-side mirror of `currentStep.weightRight/weightNose` into the same `AWeightDistribution` actor. Logs show the new autopilot's steps activating, but the board never leans.

The `AddTickPrerequisiteActor(StateTriggerAutoPilot)` call at [`SurfboardPawn.cpp:182`](../Source/GoneSurfing/SurfboardPawn.cpp) suffers the same staleness — it orders the pawn after the *disabled* original, not the new one.

## Expected Behavior

When the manually-wired ref is missing or points at a disabled autopilot, the pawn finds the first `AStateTriggerAutoPilot` in the world whose `enabled == true` and uses that as its reference for both the tick-prerequisite and the `bAutopilotActive` handoff check. The duplicate-and-swap workflow then just works without re-wiring the pawn.

## Root Cause

Single hard-coded actor pointer that the editor doesn't re-target when the referenced actor is duplicated, plus no runtime fallback.

## Functional Requirements

- **FR1**: At pawn `BeginPlay`, if `StateTriggerAutoPilot` is null OR its target has `enabled == false`, scan the world for `AStateTriggerAutoPilot` actors and pick the first one with `enabled == true`. Assign it to `StateTriggerAutoPilot`.
- **FR2**: If a manually-wired ref is non-null AND its target is `enabled`, leave it alone. (Manual wire wins when consistent — needed for the suite case where one autopilot in `stateTriggerAutoPilots` should be tracked specifically by the pawn.)
- **FR3**: If the auto-resolve scan finds zero enabled autopilots, leave the ref as-is (null or stale) and log a warning. Downstream behavior is unchanged from today.
- **FR4**: If the auto-resolve scan finds more than one enabled autopilot, pick the first deterministically and log a warning naming all enabled candidates. (Caller can manually wire if they care which one drives the pawn's handoff.)
- **FR5**: The `AddTickPrerequisiteActor(StateTriggerAutoPilot)` and `InitAutoPilotState()` calls must run *after* the resolution so they see the resolved ref.
- **FR6**: Log the resolution outcome (Display level when resolved, Warning when ambiguous or empty) including the prior ref, the new ref, and the new ref's `TestName`.

## Non-Functional Requirements

- **NFR1**: Pure C++ change in `SurfboardPawn.cpp` / `.h` (no BP edits required for the workflow to be fixed).
- **NFR2**: Zero allocations on the pawn's hot path. The scan happens once in `BeginPlay`, never per-tick.
- **NFR3**: The change must not affect headless snapshot-test runs. In `-game -unattended` the pawn's input handling is dormant, so even if auto-resolve picked a "wrong" autopilot, snapshot tests still record their own CSV and quit on their own — but for cleanliness the resolution should still pick correctly when only one autopilot is `enabled`, which is the normal snapshot-test case.

## Acceptance Criteria

- **AC1 — Duplicate-and-swap (primary bug)**: Given two `StateTriggerAutoPilot` actors in `Surfing_infinite_wave` — original `surfing-down-the-line` with `enabled=false` and duplicate `surfing-down-the-line-2` with `enabled=true`, and given the `SurfboardPawn`'s `StateTriggerAutoPilot` ref still points at the original — when the level starts, the pawn's `BeginPlay` log shows `StateTriggerAutoPilot auto-resolved <original> -> <duplicate> (TestName='surfing-down-the-line-2', 1 enabled in world)`, and during play the board leans according to the duplicate's step weights (`WeightDistribution.amountToTheRight` is not stuck at 0.5).

- **AC2 — Manual wire honored**: Given a single enabled autopilot wired into the pawn manually, the pawn's `BeginPlay` log does **not** print an `auto-resolved` line. The wired ref is used as-is.

- **AC3 — Empty world**: Given no enabled `AStateTriggerAutoPilot` in the world (e.g. a non-autopilot test scene), the pawn's `BeginPlay` logs `auto-resolve found no enabled autopilot in world` at Warning level, leaves the ref unchanged, and falls through to the legacy `AutoPilotDuration` timer path without crashing.

- **AC4 — Multiple enabled (suite)**: Given an `AAutoPilotSuite` with N autopilots all `enabled=true`, the pawn picks the first one found and logs `<N> enabled StateTriggerAutoPilots found; picked <name>` at Warning level. (The suite's own start-deferral handles the actual sequencing; the pawn just needs one ref to watch.)

- **AC5 — Tick prerequisite uses resolved ref**: After resolution, `AddTickPrerequisiteActor` is called with the *resolved* `StateTriggerAutoPilot`, so the new autopilot's BP-side `currentStep` mirror is guaranteed to tick before the pawn's `WeightDistribution` write.

## Implementation Sketch

In `ASurfboardPawn::BeginPlay`, immediately before the existing
`if (StateTriggerAutoPilot) AddTickPrerequisiteActor(...)` block at
[`SurfboardPawn.cpp:180`](../Source/GoneSurfing/SurfboardPawn.cpp):

```cpp
const bool bRefNeedsResolution =
    !StateTriggerAutoPilot || !StateTriggerAutoPilot->enabled;
if (bRefNeedsResolution)
{
    AStateTriggerAutoPilot* PriorRef = StateTriggerAutoPilot;
    TArray<AActor*> Found;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(),
        AStateTriggerAutoPilot::StaticClass(), Found);

    AStateTriggerAutoPilot* FirstEnabled = nullptr;
    int32 EnabledCount = 0;
    for (AActor* A : Found)
    {
        AStateTriggerAutoPilot* AP = Cast<AStateTriggerAutoPilot>(A);
        if (AP && AP->enabled)
        {
            ++EnabledCount;
            if (!FirstEnabled) FirstEnabled = AP;
        }
    }

    if (FirstEnabled)
    {
        StateTriggerAutoPilot = FirstEnabled;
        UE_LOG(LogTemp, Display,
            TEXT("SurfboardPawn: StateTriggerAutoPilot auto-resolved %s -> %s (TestName='%s', %d enabled in world)"),
            PriorRef ? *PriorRef->GetName() : TEXT("(null)"),
            *FirstEnabled->GetName(), *FirstEnabled->TestName, EnabledCount);
        if (EnabledCount > 1)
        {
            UE_LOG(LogTemp, Warning,
                TEXT("SurfboardPawn: %d enabled StateTriggerAutoPilots found; picked %s — wire pawn explicitly if a different one should drive handoff."),
                EnabledCount, *FirstEnabled->GetName());
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning,
            TEXT("SurfboardPawn: StateTriggerAutoPilot auto-resolve found no enabled autopilot in world; ref left as %s"),
            PriorRef ? *PriorRef->GetName() : TEXT("(null)"));
    }
}
```

The existing `AddTickPrerequisiteActor` block then naturally uses the resolved ref.

## Out of Scope

- **CVar-filter timing race**. `AStateTriggerAutoPilot::BeginPlay` defers its `surf.autopilots` filter pass by 1.0 s, after which it may flip an autopilot's `enabled` flag. At pawn-BeginPlay (t=0) the pawn sees the umap-default `enabled`, not the filter-resolved value. This only matters in `-game -unattended` snapshot-test runs, where the pawn doesn't drive anything anyway — the `bAutopilotActive` check is re-evaluated every tick and will correctly reflect any later `enabled` flip on the *resolved* autopilot. Picking the "wrong" autopilot during the 1 s window has no observable effect because no player input arrives in unattended mode.
- **Tracking the suite's *last* autopilot for handoff**. Suite users still need to manually wire `StateTriggerAutoPilot` to whichever suite member should gate `bPlayerControlsEnabled`. Documented at AC4.

## Test Plan

Manual editor playthrough (UI features have no automated coverage per [CLAUDE.md](../../CLAUDE.md)):

1. **Repro the bug pre-fix** in `Surfing_infinite_wave`: confirm `surfing-down-the-line-2` is the enabled duplicate, confirm pawn's `StateTriggerAutoPilot` ref still points at the disabled `surfing-down-the-line`. Play in PIE — observe board not leaning, logs show step activation but `WeightDistribution.amountToTheRight` stuck near 0.5.
2. **Apply the fix**, rebuild, relaunch.
3. **AC1**: Same setup, play in PIE. Check log for the `auto-resolved` line naming `surfing-down-the-line-2`. Visually confirm board leans (right or left as the duplicate's steps dictate).
4. **AC2**: Re-wire the pawn's `StateTriggerAutoPilot` to the enabled duplicate manually. Play. Confirm log does *not* print `auto-resolved`.
5. **AC3**: Disable all `AStateTriggerAutoPilot` actors in the level. Play. Confirm warning log line, no crash, pawn proceeds with controls enabled after the legacy duration timer.

## Status

- [x] Spec written
- [x] `SurfboardPawn::BeginPlay` updated
- [x] Build compiles
- [ ] Manual AC1 verified in editor
