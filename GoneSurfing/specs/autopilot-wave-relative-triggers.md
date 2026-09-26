# Spec: Wave-Relative Triggers for `AStateTriggerAutoPilot`

## Overview

Extend `FStateTrigger` with a `slopeSin` (wave-slope-under-board) check so autopilot steps can advance when the board reaches a wave-geometry condition — e.g. "advance when the board is on the wave face" — instead of relying purely on inertial signals (pitch, yaw, roll, velocity, planing) or timeouts.

Scope: one new `bCheckSlopeSin` field plus `MinSlopeSin` / `MaxSlopeSin`, read from the cached `ASharedCalculations` actors the autopilot already resolves for the recorder. ANDs with the existing checks. No new BP-side wiring, no new actor relationships.

## Motivation

The current `FStateTrigger` vocabulary is **inertial**: pitch/yaw/roll/velocity/planing read the surfboard mesh's own state. None of it reads the wave the board is on. That works for pop-up and carve steps (which have predictable inertial signatures) but is fragile for "surf between trough and face" because **exactly when the board reaches the face varies run to run**:

- On one run the board reaches the face in 3.5 s after 70° of yaw.
- On the next, 4.2 s after 95° of yaw.

A timeout-or-yaw step that fits one run misfires on the other — the board advances too early (still in the trough, not actually on the face) or too late (already past the face, sliding back into the trough).

The wave itself is the ground truth for "am I on the face." `ASharedCalculations` already exposes `boardWideSlopeSin` (and `waveSlopeDownVec`, whose magnitude is the same number) — `~0` on flat water / in a trough, `~0.3+` on a moderate wave face, approaching `1.0` on a vertical wall. The recorder already averages this across the board's SC actors and writes it to the trajectory CSV as the `slopeSin` column. Letting trigger conditions read the same scalar closes the loop.

### What this is not

This is **not** a continuous controller. The autopilot stays a discrete step machine. We're just giving the existing trigger machinery a wave-relative sensor so step transitions can fire on "where the board is on the wave" rather than only on inertial signatures and time.

A continuous "stay between slope 0.15 and 0.35" controller for the surfing phase is a separate, larger change — deferred.

### Why only `slopeSin` for the first version

`ASharedCalculations` exposes several wave-relative scalars that could become trigger fields: `boardWideWaterColumnAbove`, `waveRelativeRollSin`, and (with a small addition) the angle between `forwards` and `waveSlopeDownVec`. `slopeSin` is the cheapest and most discriminating single signal for "trough vs face" — the question this spec is motivated by. Each additional field is the same shape of change and can be added in a follow-up spec once the pattern is proven and we know which signals matter.

## Background

### Current `FStateTrigger` shape

[StateTriggerAutoPilot.h:14-82](../Source/GoneSurfing/StateTriggerAutoPilot.h#L14-L82) defines the struct as a flat set of `bCheckX` booleans plus `MinX` / `MaxX` ranges, gated by `EditCondition` so the editor hides irrelevant fields. `AreConditionsMet` ([StateTriggerAutoPilot.cpp:327-452](../Source/GoneSurfing/StateTriggerAutoPilot.cpp#L327-L452)) early-returns on the first failing check; if every enabled check passes, the trigger fires. A `bHasAnyCheck` guard returns false when no boxes are ticked, so an unconfigured trigger can only fire via `TimeoutDuration`.

### What `slopeSin` measures

`boardWideSlopeSin` is set per tick in `SharedCalculations.cpp` and equals `sqrt(1 - waveNormal.Z²)` sampled at the SC actor's position. The recorder column `slopeSin` is `waveSlopeDownVec.Size()` averaged across the surfboard's SC actors — the same number, computed by averaging the downhill-vector magnitude rather than reading `boardWideSlopeSin` directly. Both forms are equivalent in magnitude; the recorder uses the vector form because it's already computed for `waveSlopeGravity`.

The trigger should use the **same average** the recorder uses, so values in the trajectory CSV and values in the trigger configuration mean the same thing. A `slopeSin >= 0.18` threshold tuned by reading a CSV will then mean the same thing inside `AreConditionsMet`.

### SC actor resolution

The autopilot already caches its surfboard's SC actors lazily on the first `RecordSample` call ([StateTriggerAutoPilot.cpp:488-504](../Source/GoneSurfing/StateTriggerAutoPilot.cpp#L488-L504)). This spec needs the same cache available inside `AreConditionsMet`, which can fire before the first sample if `bRecorderActive` is false (autopilot has no `TestName`) or before the 50 ms sample cadence has elapsed. The resolution code can be factored out into a small helper called by both sites — no behavior change for the recorder path.

## Design

### New fields on `FStateTrigger`

```cpp
/** If true, check wave-slope-under-board condition. Reads boardWideSlopeSin
 *  averaged across the surfboard's ASharedCalculations actors (same value
 *  the recorder writes as the 'slopeSin' CSV column). 0 = flat water,
 *  ~0.3 = moderate wave face, ~1 = vertical wall. */
UPROPERTY(EditAnywhere, BlueprintReadWrite)
bool bCheckSlopeSin = false;

/** Minimum wave slope (0..1). 0 = no minimum. */
UPROPERTY(EditAnywhere, BlueprintReadWrite,
          meta=(EditCondition="bCheckSlopeSin", ClampMin="0.0", ClampMax="1.0"))
float MinSlopeSin = 0.0f;

/** Maximum wave slope (0..1). 1 = no maximum. */
UPROPERTY(EditAnywhere, BlueprintReadWrite,
          meta=(EditCondition="bCheckSlopeSin", ClampMin="0.0", ClampMax="1.0"))
float MaxSlopeSin = 1.0f;
```

### `AreConditionsMet` extension

After the existing planing check, before the "all conditions met" return:

```cpp
if (Trigger.bCheckSlopeSin)
{
    // Resolve SC cache the same way RecordSample does. Extracted helper —
    // see ResolveSharedCalcsIfNeeded() below.
    ResolveSharedCalcsIfNeeded();

    if (CachedSharedCalcs.Num() == 0)
    {
        // No SC actors found for this surfboard — can't evaluate the check,
        // so treat as not-yet-satisfied (same shape as the planingCalculator
        // missing case). The emergency 30 s force-advance will still rescue
        // a misconfigured trigger.
        return false;
    }

    float slopeSinSum = 0.0f;
    int32 nValid = 0;
    for (ASharedCalculations* SC : CachedSharedCalcs)
    {
        if (!SC) continue;
        slopeSinSum += SC->waveSlopeDownVec.Size();
        ++nValid;
    }
    const float slopeSin = (nValid > 0) ? slopeSinSum / nValid : 0.0f;

    if (bDebugLogging)
    {
        UE_LOG(LogTemp, Display,
            TEXT("StateTriggerAutoPilot: Checking slopeSin - Current: %.4f, Required: %.4f to %.4f"),
            slopeSin, Trigger.MinSlopeSin, Trigger.MaxSlopeSin);
    }

    if (slopeSin < Trigger.MinSlopeSin || slopeSin > Trigger.MaxSlopeSin)
    {
        return false;
    }
}
```

The `bHasAnyCheck` guard at the top of `AreConditionsMet` is updated to OR in `bCheckSlopeSin`, so a trigger that only checks slope still counts as "configured."

### Shared SC-cache helper

Factor the lazy-resolve block in `RecordSample` into a private method:

```cpp
void AStateTriggerAutoPilot::ResolveSharedCalcsIfNeeded()
{
    if (bSharedCalcsResolved || !surfboard) return;
    bSharedCalcsResolved = true;
    TArray<AActor*> Found;
    UGameplayStatics::GetAllActorsOfClass(GetWorld(), ASharedCalculations::StaticClass(), Found);
    for (AActor* A : Found)
    {
        if (ASharedCalculations* SC = Cast<ASharedCalculations>(A))
        {
            if (SC->Surfboard == surfboard)
            {
                CachedSharedCalcs.Add(SC);
            }
        }
    }
    UE_LOG(LogTemp, Display,
        TEXT("StateTriggerAutoPilot: SharedCalculations resolved -> %d actors"),
        CachedSharedCalcs.Num());
}
```

Both `RecordSample` and `AreConditionsMet` call it. The log line ends up in the same place it does today — first time a slope check runs *or* first time a sample is recorded, whichever happens first.

### Tuning notes (for the BP-side autopilot authoring that will follow)

These are starting points, not part of this spec's acceptance:

| Wave region | Approximate `slopeSin` |
|------|------|
| Flat water / trough | < 0.05 |
| Wave shoulder (start of face) | 0.10 – 0.20 |
| Wave face | 0.20 – 0.40 |
| Steep face / near-breaking | > 0.40 |

Concrete numbers should come from reading `slopeSin` in an existing snapshot CSV at the moment the board visibly reaches the face / trough.

## Acceptance Criteria

### AC1 — Compiles, no BP migration needed

`FStateTrigger` gets three new fields with sensible defaults (`bCheckSlopeSin = false`, `MinSlopeSin = 0`, `MaxSlopeSin = 1`). Existing umaps / Blueprints with `FStateTrigger`-typed properties load unchanged because the new check is opt-in.

### AC2 — Slope-only trigger fires correctly

A step configured with `bCheckSlopeSin = true, MinSlopeSin = 0.18`, all other `bCheckX = false`, and `TimeoutDuration = 0` advances exactly when the board's averaged `slopeSin` first crosses 0.18. Verified by:

- Configuring such a step in a throwaway autopilot.
- Running with `surf.debug.flags=` set to enable the autopilot's `bDebugLogging`.
- Confirming the log line `Checking slopeSin - Current: X.XXXX, Required: 0.1800 to 1.0000` appears each tick until X first reaches 0.18, at which point `Activating step …` fires.

### AC3 — `bHasAnyCheck` recognizes the new field

A step that only sets `bCheckSlopeSin = true` (no pitch/yaw/roll/velocity/planing) does **not** fall into the "vacuously true, fire immediately" path. The existing `bHasAnyCheck` guard is updated to include `bCheckSlopeSin`.

### AC4 — Slope check ANDs with existing checks

A step with both `bCheckYaw = true` (e.g., yaw in 60..120°) and `bCheckSlopeSin = true` (e.g., 0.15..1.0) advances only when **both** are true at the same tick. If yaw enters its range while slope is still 0.05, the step does not fire.

### AC5 — Missing SC gracefully degrades

If `CachedSharedCalcs` resolves to zero actors (e.g., misconfigured `surfboard` reference), the slope check returns false rather than crashing or vacuously passing. The trigger then advances only via `TimeoutDuration` or the existing 30 s emergency force-advance.

### AC6 — Existing inertial-only steps unchanged

A step with no `bCheckSlopeSin` (or all the other existing trigger types) behaves byte-identically to the pre-change behavior. No new log lines on the existing path. Snapshot tests (`barrel-glide-through`, etc.) produce CSVs within their existing noise tolerance.

### AC7 — Shared SC-cache log fires once

The "SharedCalculations resolved -> N actors" log line fires at most once per autopilot run, regardless of whether the slope check or the recorder triggered the resolve.

## Open Questions / Future Work

1. **Other wave-relative signals.** `boardWideWaterColumnAbove`, `waveRelativeRollSin`, and a new "angle between `forwards` and `waveSlopeDownVec`" scalar are obvious follow-ups using the same pattern. Add them in separate specs once we know which ones actually drive better autopilot fidelity. Heading-vs-downhill is the most likely next addition because it's what distinguishes "surfing the line" from "aimed at the trough."

2. **Continuous controller for the surfing sub-phase.** A controller that holds `slopeSin` in a band by modulating `amountToTheRight` would be more robust than discrete steps for "surf between trough and face indefinitely." Not in scope here; this spec keeps the discrete-step model. Likely worth introducing once the trigger vocabulary is rich enough to express the boundary conditions of the controller (enter/exit).

3. **`boardWideSlopeSin` vs averaged `waveSlopeDownVec.Size()`.** They should be equivalent in magnitude, but the recorder uses the average-of-vectors-magnitude form. This spec follows the recorder for consistency. If a future change makes them diverge, both sites should update together.

4. **Single-SC optimization.** Most surfboards have two SC actors (one per half-board). Averaging two scalars per tick is cheap, so no special-casing. If a board ever had only one SC, the average degenerates correctly.

## Status

- [x] `bCheckSlopeSin` / `MinSlopeSin` / `MaxSlopeSin` added to `FStateTrigger`
- [x] `ResolveSharedCalcsIfNeeded` helper extracted; called by both `RecordSample` and `AreConditionsMet`
- [x] `AreConditionsMet` reads averaged `slopeSin`, ANDs against `Min/MaxSlopeSin`
- [x] `bHasAnyCheck` guard includes `bCheckSlopeSin`
- [x] `bDebugLogging` prints the slope check line
- [ ] AC2 verified (slope-only trigger fires at threshold)
- [ ] AC4 verified (slope ANDs with yaw)
- [ ] AC6 verified (existing snapshot tests within tolerance)
