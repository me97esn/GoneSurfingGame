# Spec: Runtime Coefficient Tuning (On-Device + PIE)

## Overview

In-game UMG overlay that lets every shared surfing coefficient — lift, drag, thrust, buoyancy, damping, planing thresholds, weight distribution — be adjusted between rides. Opening the panel pauses the game; closing it resumes. New values are in effect on the next physics tick after resume. Works in PIE and on Android. Single source of truth is a `USurfTuningSubsystem` (UGameInstanceSubsystem). Persisted to JSON in the project's `Saved/` directory.

Replaces the current `FluidDynamicsConstants` BP (the values-passed-to-functions pattern) and the coefficient UPROPERTYs scattered across `ASurfboardUtils`, `AWeightDistribution`, `ASharedCalculations`, `ABuoyancy`, `AFluidDynamics`.

## Motivation

Android deploy round-trip is 30+ minutes. Damping currently feels "way too weak" and weight-distribution torque "perhaps too strong"; reaching a good tuning point at that iteration speed isn't feasible. PC PIE tuning works but the controls (phone tilt + touch) only exist on the device, so PIE-tuned values may not translate.

`FluidDynamicsConstants` was introduced exactly to share coefficients across all FluidDynamics actor instances — but it lives in BP and is consumed by passing values into function pins, which makes runtime mutation awkward and requires a rebuild for every value change.

Replacing it with a runtime-mutable subsystem solves both problems at once: live tuning *and* a cleaner architecture (one shared store instead of a "constants" actor wired into every call site).

## Requirements

### Functional

- **FR1** Every tunable coefficient is editable from an on-screen UMG panel, on Android device and in PIE.
- **FR2** Opening the panel pauses the game (`UGameplayStatics::SetGamePaused(true)`); closing resumes. Physics state is preserved across pause.
- **FR3** One value per coefficient name, broadcast to every actor that reads it. Confirmed: there is only one surfboard.
- **FR4** Persisted across app launches via a JSON file in `FPaths::ProjectSavedDir() / "TuningOverrides.json"`. On Android this resolves to the per-app save directory; in editor it resolves under `GoneSurfing/Saved/`.
- **FR5** Current values (from `FluidDynamicsConstants` defaults and existing UPROPERTYs) become the subsystem's compile-time defaults. The tuning JSON is a sparse overlay — only changed keys are written. Deleting the JSON returns to defaults.
- **FR6** Panel toggle: persistent small button in a screen corner. Visible in all play modes.
- **FR7** Per-row slider scaled `0..1 → 0..4×` of that coefficient's default. Reset button per row returns the value to its default. Numeric text shows current value. Fallback range (`0..1`) used when the default is zero.
- **FR8** Damping changes must propagate to the engine fork's CVars (`p.Chaos.Solver.*`) the same way they do today from `ASurfboardUtils::BeginPlay` / `Tick`. On resume, the next `Tick` re-pushes the current values.
- **FR9** Same values are editable from the editor without starting PIE, via an Editor Utility Widget. Both surfaces read/write the same `TuningOverrides.json` (see Editor-time access).

### Non-Functional

- **NFR1** Per-tick subsystem reads are direct UPROPERTY accesses (single pointer dereference); no map lookups or string ops in hot paths. UMG-side reflection cost is irrelevant — only happens while paused.
- **NFR2** No platform-specific code branches for Android vs Windows in the UMG or persistence layer.
- **NFR3** Per-tick reads from the subsystem must not measurably regress frame time vs the current `FluidDynamicsConstants`-passed-as-pins pattern.

## Design

### Single source of truth: `USurfTuningSubsystem`

```cpp
UCLASS()
class GONESURFING_API USurfTuningSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    // --- Lift (replaces FluidDynamicsConstants vars passed to applyLiftAsImpulse) ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Tuning|Lift")
    float bottomLiftMagnitude = <current default>;
    UPROPERTY(...) float railLiftMagnitude = ...;
    UPROPERTY(...) float finLiftMagnitude = ...;

    // --- Drag ---
    UPROPERTY(...) float bottomDragCoefficient = ...;
    UPROPERTY(...) float maxDragAmount = ...;
    UPROPERTY(...) float finDragCoefficient = ...;
    UPROPERTY(...) float railDragCoefficient = ...;
    UPROPERTY(...) float tailDragCoefficient = ...;

    // --- Thrust ---
    UPROPERTY(...) float alongThrustCoefficient = ...;
    UPROPERTY(...) float upwardsThrustCoefficient = ...;

    // --- Buoyancy ---
    UPROPERTY(...) float basicFloatBuoyancyCoefficient = ...;
    UPROPERTY(...) float horizontalVelocityBuoyancyCoefficient = ...;
    UPROPERTY(...) float verticalVelocityBuoyancyCoefficient = ...;
    UPROPERTY(...) float amountUnderWaterPower = ...;
    UPROPERTY(...) float weightForceMaxMultiplier = ...;

    // --- Damping (migrated from ASurfboardUtils) ---
    UPROPERTY(...) float SurfboardSidewaysDamping = ...;
    UPROPERTY(...) float SurfboardForwardsDamping = ...;
    UPROPERTY(...) float SurfboardVerticalDamping = ...;
    UPROPERTY(...) float WorldUpwardsDamping = ...;
    UPROPERTY(...) float WorldDownwardsDamping = ...;
    UPROPERTY(...) float AngularDampingX = ...;
    UPROPERTY(...) float AngularDampingY = ...;
    UPROPERTY(...) float AngularDampingZ = ...;
    UPROPERTY(...) float YawDampingTiltBackInfluence = 0.7f;
    UPROPERTY(...) float ClampYVelocityAt = 2000.0f;
    UPROPERTY(...) float MaxVelocityX = 3000.0f;
    UPROPERTY(...) float MaxVelocityZUp = 1000.0f;
    UPROPERTY(...) float MaxVelocityZDown = 2000.0f;
    UPROPERTY(...) float VelocityDampingThreshold = 500.0f;
    UPROPERTY(...) float VelocityDampingScale = 0.0005f;
    UPROPERTY(...) float MaxVelocityDamping = 0.95f;

    // --- Planing (migrated from ASharedCalculations) ---
    UPROPERTY(...) float PlaningStartsVelocity = 200.0f;
    UPROPERTY(...) float PlaningStopsVelocity = 100.0f;
    UPROPERTY(...) float PlaningFullVelocity = 300.0f;
    UPROPERTY(...) float MaxPlaning = 0.9f;
    UPROPERTY(...) float PlaningDecayTime = 2.0f;
    UPROPERTY(...) float velocitySmoothingFactor = 0.3f;

    // --- Weight distribution (migrated from AWeightDistribution) ---
    // Note: weightForceMagnitude and downwardForceMagnitude were removed —
    // the four-point impulse model they fed was retired in favour of the
    // current calculateWeightTorque + COM-shift model. See
    // WEIGHT_DISTRIBUTION_SPEC.md for the deprecation note.
    UPROPERTY(...) float torqueMagnitude = 5000.0f;
    UPROPERTY(...) float maxTiltAngle = 45.0f;
    UPROPERTY(...) float offsetDistance = 30.0f;

    // Defaults snapshot — captured at Initialize() before JSON overlay is applied.
    // Used for slider scaling (0..1 → 0..4×default) and reset-to-default.
    TMap<FName, float> Defaults;

    // Per-property change notification — emitters: setter UFUNCTION, JSON loader,
    // reset button. Listeners: damping push to CVars; future per-system hooks.
    DECLARE_MULTICAST_DELEGATE_TwoParams(FOnTuningChanged, FName, float);
    FOnTuningChanged OnTuningChanged;

    UFUNCTION(BlueprintCallable, Category="Tuning")
    void SetByName(FName PropertyName, float NewValue);  // dirties save; fires delegate

    UFUNCTION(BlueprintCallable, Category="Tuning")
    float GetByName(FName PropertyName) const;

    UFUNCTION(BlueprintCallable, Category="Tuning")
    float GetDefault(FName PropertyName) const;

    UFUNCTION(BlueprintCallable, Category="Tuning")
    void ResetToDefault(FName PropertyName);

    UFUNCTION(BlueprintCallable, Category="Tuning")
    TArray<FName> GetAllPropertyNames() const;  // for UMG enumeration

private:
    void LoadFromDisk();
    void SaveToDisk();     // debounced ~300 ms via FTSTicker

    bool bDirty = false;
    float SaveDebounceSeconds = 0.0f;
};
```

`SetByName` and `GetByName` use UE reflection (`FProperty::SetValue_InContainer` / `GetValue_InContainer`) on the subsystem's own UClass so adding a new `UPROPERTY` automatically appears in the UMG enumeration without touching the UMG code.

### Refactor map

| Today | After |
|---|---|
| `FluidDynamicsConstants` BP holds lift/drag/thrust/buoyancy floats | Deleted. |
| `applyDragAsImpulse(DT, bottomDragCoef, maxDragAmt, finDragCoef, railDragCoef, tailDragCoef)` | `applyDragAsImpulse(DT)` — reads from subsystem |
| `applyLiftAsImpulse(DT, bottomLiftMag, railLiftMag, finLiftMag)` | `applyLiftAsImpulse(DT)` |
| `applyThrustAsImpulse(DT, alongThrustCoef, upwardsThrustCoef, debugColor)` | `applyThrustAsImpulse(DT, debugColor)` |
| `calculateBuoyancyForce(BasePrimComp, basicFloat, horizVel, vertVel, underwaterPow, weightForceMaxMult, rowName)` | `calculateBuoyancyForce(BasePrimComp, rowName)` |
| `ASurfboardUtils` damping UPROPERTYs ([SurfboardUtils.h:50-128](../Source/GoneSurfing/SurfboardUtils.h#L50-L128)) | Removed. Tick reads from subsystem. |
| `ASharedCalculations` planing UPROPERTYs ([SharedCalculations.h:34-54](../Source/GoneSurfing/SharedCalculations.h#L34-L54)) | Removed. Planing math reads from subsystem. |
| `AWeightDistribution` tunable floats ([WeightDistribution.h:51-67](../Source/GoneSurfing/WeightDistribution.h#L51-L67)) | Removed. Calc functions read from subsystem. |

Per-actor non-tunable UPROPERTYs stay (debug flags, `Surfboard` actor pointer, etc.).

### Subsystem access helper

To avoid `UGameplayStatics::GetGameInstance(GetWorld())->GetSubsystem<USurfTuningSubsystem>()` boilerplate at every read site, a static helper:

```cpp
// In SurfTuningSubsystem.h
namespace SurfTuning {
    GONESURFING_API USurfTuningSubsystem* Get(const UObject* WorldContext);
}
```

with a single null check + cached `mutable` pointer per actor (set in `BeginPlay`). Per-tick code becomes:

```cpp
const float bottomDrag = Tuning->bottomDragCoefficient;  // direct UPROPERTY read
```

### Damping CVar push hook

The existing per-tick CVar pushes in [SurfboardUtils.cpp:89-110](../Source/GoneSurfing/SurfboardUtils.cpp#L89-L110) already read from the actor's UPROPERTYs. After migration they read from the subsystem instead — same shape, different source pointer. The `BeginPlay` pushes at [SurfboardUtils.cpp:26-33](../Source/GoneSurfing/SurfboardUtils.cpp#L26-L33) also re-source from the subsystem.

No new "push on change" callback is strictly needed because `Tick` re-reads every frame anyway. The `OnTuningChanged` delegate exists for future systems where a one-shot push is cleaner than per-tick polling.

### Persistence

`USurfTuningSubsystem::Initialize`:

1. Snapshot all UPROPERTY default values into `Defaults`.
2. Read `FPaths::ProjectSavedDir() / "TuningOverrides.json"`. If present, parse `{ "propertyName": value, ... }` and apply via `SetByName`.

`SetByName`:

1. Use reflection to write the property.
2. Fire `OnTuningChanged`.
3. Set `bDirty = true`, reset debounce counter.

A ticker (registered in `Initialize`, unregistered in `Deinitialize`) checks each tick:

```cpp
if (bDirty) {
    SaveDebounceSeconds -= DeltaTime;
    if (SaveDebounceSeconds <= 0.0f) {
        SaveToDisk();
        bDirty = false;
    }
}
```

`SaveToDisk` writes a sparse JSON: only properties whose current value differs from `Defaults`.

### UMG `WBP_Tuner`

Vertical `ScrollBox` containing one `VerticalBox` per Category (Lift / Drag / Thrust / Buoyancy / Damping / Planing / Weight). Categories derived from the `UPROPERTY(Category=...)` metadata via `FProperty::GetMetaData("Category")`.

Each coefficient row:

- `TextBlock` — property name (formatted from camelCase / PascalCase)
- `TextBlock` — current numeric value (e.g. `0.0034`, 4 significant figures)
- `Slider` — `0..1` → `currentValue` maps to `min(1, current / (4 × default))` on bind; `OnValueChanged` calls `Subsystem->SetByName(name, value * 4 × default)`
- `Button "↺"` — calls `Subsystem->ResetToDefault(name)`

Zero-default fallback: if `Defaults[name] == 0`, slider maps `0..1` → `0..1` and a small `×10` button beside the row multiplies the current value by 10 (so the user can climb out of the zero-default range manually).

Rows are generated in `WBP_Tuner` Construct event by iterating `Subsystem->GetAllPropertyNames()`. No per-property UMG code.

### Editor-time access (no PIE required)

`USurfTuningSubsystem` only exists once a Game Instance is running — i.e. in PIE, in `-game`, or on device. To allow editing the same tuning values without starting PIE, expose a parallel **Editor Utility Widget** that reads/writes `Saved/TuningOverrides.json` directly.

**Data model**: the JSON is the single source of truth for "overrides on top of C++ defaults". Both surfaces edit the same file:

```
Saved/TuningOverrides.json
        ▲                  ▲
        │                  │
        │   reads/writes   │ reads/writes
        │                  │
   USurfTuningSubsystem    UEUW_TunerEditor
   (runtime, PIE/device)   (editor, no PIE needed)
```

The runtime subsystem reads the JSON in `Initialize` (PIE start or device launch). When PIE is *not* running, the editor widget bypasses the subsystem entirely and reads/writes the file directly.

**Asset**: `EUW_Tuner` (parent class `UEditorUtilityWidget`). Same row layout as `WBP_Tuner` — in fact `EUW_Tuner` can either:
- inherit from `WBP_Tuner` (cleaner — one row layout, two binding backends), or
- duplicate the layout if multiple inheritance / cross-parent issues arise

Row binding indirection: introduce a tiny `USurfTuningStore` interface with `GetByName / SetByName / GetDefault / GetAllPropertyNames`. Two implementations:
- `USurfTuningSubsystem` (runtime): existing methods.
- `UEditorTuningStore` (editor-only, `WITH_EDITOR`-gated): JSON-backed. Reads defaults by inspecting `USurfTuningSubsystem::StaticClass()->GetDefaultObject()` via reflection — same property set, no duplication.

`WBP_Tuner` row code takes a store pointer and doesn't care which side it's on.

**How to open**:
1. Right-click `EUW_Tuner` in Content Browser → **Run Editor Utility Widget**. Opens as a dockable tab.
2. (Polish) Add a Level Editor toolbar button via a small editor-module extension that spawns the EUW with one click. Optional; the right-click flow works without it.

**Lifecycle**:
- On widget construct: load JSON, populate rows from `EditorTuningStore`.
- On slider change: write through to `EditorTuningStore` → JSON (debounced, same ~300 ms as runtime).
- No "save" button needed — auto-saves match the runtime behavior.

**PIE-while-EUW-is-open edge case**: if the user has the EUW open and then presses Play, the runtime subsystem's `Initialize` re-reads the JSON. They see the values they just edited. If they edit in-game during PIE and then close PIE, the JSON has been updated by the runtime side; next time they open EUW it re-reads and shows the new values. Both sides "win last" — acceptable since only one is interactively in focus at a time.

### Toggle button and pause handling

Persistent `WBP_TunerToggle` widget anchored top-right of a HUD widget that's always on the viewport. Single 64×64 button (icon: gear).

`OnClicked` (or close button on the panel itself):

1. Toggle `WBP_Tuner` visibility.
2. `UGameplayStatics::SetGamePaused(World, panelNowVisible)` — paused while open, running while closed.
3. While paused: input mode switches to `UIOnly` so touch events go to UMG, not the surfboard pawn.
4. While closed: input mode back to `GameOnly`.

Pause also stops `Tick` on all actors, so the damping CVar push naturally re-fires on the first tick after resume — no extra wiring needed.

Added to whichever HUD widget is already always-visible during play. (To-confirm during implementation: there's a `BP_GoneSurfingHUD` or equivalent — if none exists, the toggle widget is added to viewport directly from `ASurfboardPawn::BeginPlay`.)

## Implementation Sketch (phased)

Each phase ships independently — you can tune more coefficients as each phase lands.

### Phase 1: Subsystem + UMG wired to damping only (smallest viable)

**Why first:** damping is the bottleneck you're hitting *right now*. All damping fields are already UPROPERTYs on `ASurfboardUtils` so this phase is pure addition — no signature changes, no BP refactor.

1. New files: `Source/GoneSurfing/SurfTuningSubsystem.h/.cpp`. Floats for damping + `MaxVelocity*` + velocity-dependent damping set.
2. Defaults match current `ASurfboardUtils` UPROPERTY defaults.
3. `Initialize`: snapshot defaults, load `TuningOverrides.json` if present.
4. `ASurfboardUtils::Tick` and `BeginPlay` read damping values from the subsystem instead of from its own UPROPERTYs. Its own UPROPERTYs stay for now (BP backwards-compat) but are unused.
5. New UMG: `WBP_Tuner` and `WBP_TunerToggle`. Rendered fields = all subsystem UPROPERTYs in category `Damping`. Tuner row uses a `ISurfTuningStore`-style pointer so it can bind to either backend.
6. Toggle widget added to viewport from `ASurfboardPawn::BeginPlay`.
7. New `EUW_Tuner` (parent `UEditorUtilityWidget`) + `UEditorTuningStore` (`WITH_EDITOR`-gated). EUW shares row layout with `WBP_Tuner`. Verify it loads/saves the same JSON.
8. Verify on PIE + on device + via right-click Run Editor Utility Widget in the Content Browser.

### Phase 2: Migrate remaining C++ UPROPERTYs

8. `ASharedCalculations` planing fields → subsystem. SC reads via cached pointer in `BeginPlay`.
9. `AWeightDistribution` tunable fields → subsystem. Same pattern.
10. `AFluidDynamics` per-actor C++ UPROPERTYs that are *intended* to be shared (`lateralTurnCoefficient`, `forwardsThrustCoefficient`, `waveMassThrustCoefficient`, etc.) → subsystem. Keep `side`, debug flags, and runtime-state fields (`waterColumnAbove`, `slopeSin`, etc.) on the actor.
11. `ABuoyancy::waveFaceNormalInfluence`, `horizontalVelocityBuoyancyFrontBias`, `boardHalfLength` → subsystem.
12. UMG rows for all migrated fields appear automatically (driven by `GetAllPropertyNames`).

### Phase 3: BP-side coefficient migration (deletes `FluidDynamicsConstants`)

13. Change function signatures: drop the coefficient pins from `applyDragAsImpulse`, `applyLiftAsImpulse`, `applyThrustAsImpulse`, `calculateBuoyancyForce`. Each function reads from the subsystem at the top of its body.
14. Subsystem defaults for these come straight from `FluidDynamicsConstants` BP defaults (read once during this phase, pasted into the subsystem's UPROPERTY defaults).
15. Edit BP graphs: every call site to the four functions loses its coefficient input pins. Then delete `FluidDynamicsConstants` BP asset and its variable references on actor BPs.
16. UMG rows for lift/drag/thrust/buoyancy appear automatically.
17. Approve snapshot baselines (the refactor shouldn't change behavior, but tiny numerical differences are possible from BP-pin vs C++-read; re-approve if the changes are within noise per [reference_snapshot_run_variance](../../../C:/Users/esand/.claude/projects/e--windowsgrejor-git-GoneSurfingUE5/memory/reference_snapshot_run_variance.md)).

## Acceptance Criteria

### AC1 — Subsystem exists, loads/saves, defaults snapshot

`USurfTuningSubsystem` instantiated on game start. `Defaults` populated from compile-time UPROPERTY values. `TuningOverrides.json` absent → defaults preserved; present → values applied. Setting any value via `SetByName` writes a sparse JSON within ~300 ms.

### AC2 — Phase 1: damping is tunable between rides on device and in PIE

UMG panel toggleable via corner button. Opening pauses; closing resumes. Each damping field has a row. After closing, new values are in effect on the next physics tick. App restart preserves changed values via JSON. Reset button restores default.

### AC3 — Phase 2: every C++ shared UPROPERTY is in the subsystem and tunable

Every field listed in the Refactor Map for `ASharedCalculations` / `AWeightDistribution` / `ABuoyancy` / shared-by-design fields on `AFluidDynamics` lives in the subsystem and appears in the UMG. Per-actor non-tunable UPROPERTYs (debug flags, references, runtime state) stay on their actors.

### AC4 — Phase 3: `FluidDynamicsConstants` BP deleted

The asset is removed from `Content/`. No actor BP references it. `applyDragAsImpulse`, `applyLiftAsImpulse`, `applyThrustAsImpulse`, `calculateBuoyancyForce` BP nodes have only the non-coefficient pins. Game runs identically (within snapshot noise).

### AC5 — Single shared value confirmed

Changing `bottomLiftMagnitude` from 1.0 to 2.0 in the tuner affects every bottom FluidDynamics actor on the board. Verifiable via a `surf.debug.flags lift` log dump: every bottom actor's reported lift force scales by 2×.

### AC6 — Performance: no measurable regression

`stat unit` / `stat physics` before vs after migration of any phase: frame time within ±2%. No new per-tick allocations (verified by Insights memory trace on a 60-second capture).

### AC7 — Damping CVar push still works

After Phase 1, `p.Chaos.Solver.DampingLocalY` reflects the subsystem's `SurfboardSidewaysDamping` every tick (verified by editing the value and reading the CVar via the in-game console).

### AC8 — Tuning JSON is robust to schema drift

Adding a new UPROPERTY to the subsystem in a future change: old `TuningOverrides.json` files still load (extra-keys-in-file: ignored or warned; missing-key-in-file: default used). Removing a UPROPERTY: old JSON's stale keys logged and skipped, not crashed on.

## Open Questions / Future Work

1. **Categorization metadata** for fields currently lacking `Category=` (the FluidDynamics coefficients have no Category metadata today). Phase 3 should add `Category="Tuning|Drag"` etc. when defining the subsystem UPROPERTYs so the UMG groups them correctly.
2. **Presets.** Out of scope. If the auto-save single-overlay isn't enough, a named-presets layer can be added later: JSON files in a `Saved/TuningPresets/` folder, dropdown in the UMG header. Architecture supports it (load preset = bulk `SetByName` calls).
3. **PC HUD button mis-clicks.** The toggle button is always-visible during gameplay. Mostly mitigated by the small 64×64 corner footprint; if this still interferes with PIE input, fall back to "hidden until hovered near corner" or bind a keyboard shortcut (e.g. `~` or `F1`) instead.
4. **Per-coefficient curated ranges.** Currently `0..4× default` for all. If certain coefficients need finer control (e.g. log-scale for `MaxVelocityDamping` near 0.95), add a per-property override table in the subsystem header.
5. **Multiple-board future.** Subsystem is a global singleton. If multi-board gameplay is added, the subsystem stays as the "default" coefficients and per-board override actors could be added later. Out of scope.
6. **Networked tuning.** Subsystem is purely client-side; no replication. Fine for single-player.

## Status

- [x] Spec reviewed
- [x] Diagnostic `TuningDefaults:` logs added to capture live BP-overridden values (2026-06-07 run; values used as subsystem header defaults)
- [x] Phase 1: `USurfTuningSubsystem` created with damping fields only ([SurfTuningSubsystem.h](../Source/GoneSurfing/SurfTuningSubsystem.h) / [.cpp](../Source/GoneSurfing/SurfTuningSubsystem.cpp))
- [x] Phase 1: JSON load/save with 0.3 s debounced persistence (sparse: only overrides written)
- [x] Phase 1: ~~`WBP_Tuner` + `WBP_TunerToggle` widgets~~ replaced by pure-C++ Slate HUD ([SurfTuningHUD.h](../Source/GoneSurfing/SurfTuningHUD.h) / [.cpp](../Source/GoneSurfing/SurfTuningHUD.cpp)). Gear button anchored top-right of viewport toggles a panel auto-built from `Subsystem->GetAllPropertyNames()`. Per-row: name, numeric readout (live TAttribute), slider (`0..1 → 0..4×default`, zero-default fallback to `0..1`), reset button. Pause + UIOnly input on open, GameOnly on close. Installed from [SurfboardPawn::BeginPlay](../Source/GoneSurfing/SurfboardPawn.cpp), uninstalled in `EndPlay`.
- [ ] Phase 1: `ISurfTuningStore` interface + `UEditorTuningStore` (JSON-direct) implementation (deferred — Phase 1B alongside `EUW_Tuner`)
- [ ] Phase 1: `EUW_Tuner` editor utility widget (deferred to Phase 1B; in-game panel covers PIE + device)
- [x] Phase 1: `ASurfboardUtils` Tick/BeginPlay re-sourced from subsystem (RefreshFromTuningSubsystem pulls each tick; MaxVelocity CVars re-pushed)
- [ ] Phase 1: verified on PIE and on Android device
- [ ] Phase 2: planing thresholds migrated (`ASharedCalculations`)
- [ ] Phase 2: weight-distribution tunables migrated (`AWeightDistribution`)
- [x] Phase 2: `AFluidDynamics` shared-by-design UPROPERTYs migrated (thrustMagnitude, lateralTurnCoefficient, forwards/actorForwards/upwardsThrustPitchSensitivity/yawHydrofoil, waveSlope/maxSupplement/waveMassThrust/waveMassFlowDrag, baseHeight, slopeHeight, maxEffectiveWaterHeight, maxHydrofoilForceAmount). Refreshed in `setup()` each tick. `lateralTurnMultiplier` intentionally stays per-actor — see [[project_lateral_turn_back_counter_carve]].
- [x] Phase 2: `ABuoyancy` shared-by-design UPROPERTYs migrated (distanceWhereMaxForce, wettedTransitionDistance, waveFaceNormalInfluence, horizontalVelocityBuoyancyFrontBias, boardHalfLength). Refreshed in `Tick`.
- [x] Phase 3 (partial): BP-passed coefficients overridden in `applyDragAsImpulse` / `applyLiftAsImpulse` / `applyThrustAsImpulse` / `calculateBuoyancyForce`. Function signatures unchanged for BP-graph compat — args ignored when `Tuning` is non-null. `FluidDynamicsConstants` BP is now de-facto dead but not yet deleted.
- [ ] Phase 3: BP graphs cleaned, `FluidDynamicsConstants` BP asset deleted (manual cleanup once values are stable)
- [ ] Phase 3: snapshot baselines re-approved
- [ ] Memory: subsystem-as-tuning-store pattern recorded once stable
