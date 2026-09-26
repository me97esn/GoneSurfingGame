# Spec: Runtime Debug-Log Control

## Overview
Replace the editor-only checkbox pattern for enabling debug logs / debug visualization with a runtime-configurable system that can be set from the command line. Preserves the existing per-instance editor checkboxes; adds a CVar-based override so automation (the on-demand `RunGameAndCollectLogs.bat` script — see [run-game-and-collect-logs.md](run-game-and-collect-logs.md)) can enable specific debug flags on specific actor instances without manual editor work.

## Background
The codebase has many debug-emitting actors per surfboard: ~20 FluidDynamics, 4 Buoyancy, 1 WeightDistribution, 1 SurfboardUtils, 2 SharedCalculations. Each exposes a small set of `bDebug*` UPROPERTY booleans toggled in the editor. With everything enabled, the per-frame work degrades performance enough that the bug under investigation may behave differently than it does without logging.

The typical debugging pattern is:
- Enable specific flags (e.g., "thrust" + "buoyancy")
- On a specific subset of actors (e.g., the two bottom-middle, the two fins, the tail)
- Run, observe, iterate

The actors have human-readable labels following a consistent convention, e.g.:
- `FluidDynamicsBP_bottom_left_middle_back`
- `FluidDynamicsBP_left_fin`

Front-to-back tokens used: `back`, `middle_back`, `middle`, `middle_front`, `front`. Side tokens: `left`, `right`. This means substring-matching against labels is precise enough to address subsets of any granularity.

## Requirements

### Functional

- **Two CVars** drive the runtime override:
  - `surf.debug.actors` — comma-separated label substring tokens. An actor matches if any token is a substring of its `GetActorLabel()`.
  - `surf.debug.flags` — comma-separated category names (e.g., `thrust`, `buoyancy`, `weight`, `rail_lift`, `planing`).
- **A single helper** `SurfDebug::ShouldDebug(const AActor*, const FString& Flag)` returns true iff:
  1. `Flag` is in `surf.debug.flags`, AND
  2. Some token in `surf.debug.actors` is a substring of the actor's label.
- **Existing UPROPERTY bools are preserved**: each debug site becomes `if (bDebugX || SurfDebug::ShouldDebug(this, "x")) { ... }`. Editor toggling keeps working unchanged.
- **CVars settable** from `-ExecCmds` at launch, the in-game console, or `.ini` files.

### Non-functional

- Works in `-game` mode launched from the Development Editor binary (`UnrealEditor.exe -game`). `GetActorLabel()` is gated by `#if WITH_EDITOR`; that flag is true for the editor binary even in `-game` mode.
- Does NOT need to work in packaged Shipping builds. In Shipping, `ShouldDebug` returns `false` unconditionally.
- Helper must be cheap enough to call per-tick from many sites. With CVars empty, target is sub-microsecond per call.

## Implementation

### Header
```cpp
// Source/GoneSurfing/SurfDebug.h
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

namespace SurfDebug
{
    extern TAutoConsoleVariable<FString> CVarDebugActors;  // e.g. "left_middle,right_middle,tail"
    extern TAutoConsoleVariable<FString> CVarDebugFlags;   // e.g. "thrust,buoyancy"

    /** True iff Flag is in CVarDebugFlags AND Actor's label contains any token from CVarDebugActors. */
    bool ShouldDebug(const AActor* Actor, const FString& Flag);
}
```

### Implementation
```cpp
// Source/GoneSurfing/SurfDebug.cpp
#include "SurfDebug.h"
#include "HAL/IConsoleManager.h"

namespace SurfDebug
{
    TAutoConsoleVariable<FString> CVarDebugActors(
        TEXT("surf.debug.actors"), TEXT(""),
        TEXT("Comma-separated label substring tokens; actors matching any are debug-enabled."));

    TAutoConsoleVariable<FString> CVarDebugFlags(
        TEXT("surf.debug.flags"), TEXT(""),
        TEXT("Comma-separated debug category names (thrust, buoyancy, weight, ...)."));

    namespace
    {
        // Cached parsed tokens — refreshed when either CVar's raw value changes.
        TArray<FString> CachedActorTokens;
        TArray<FString> CachedFlagTokens;
        FString LastActorsRaw;
        FString LastFlagsRaw;

        void RefreshCacheIfNeeded()
        {
            const FString CurActors = CVarDebugActors.GetValueOnGameThread();
            const FString CurFlags  = CVarDebugFlags.GetValueOnGameThread();

            if (CurActors != LastActorsRaw)
            {
                LastActorsRaw = CurActors;
                CachedActorTokens.Reset();
                CurActors.ParseIntoArray(CachedActorTokens, TEXT(","), /*CullEmpty=*/true);
                for (FString& T : CachedActorTokens) T = T.TrimStartAndEnd();
            }
            if (CurFlags != LastFlagsRaw)
            {
                LastFlagsRaw = CurFlags;
                CachedFlagTokens.Reset();
                CurFlags.ParseIntoArray(CachedFlagTokens, TEXT(","), /*CullEmpty=*/true);
                for (FString& T : CachedFlagTokens) T = T.TrimStartAndEnd();
            }
        }
    }

    bool ShouldDebug(const AActor* Actor, const FString& Flag)
    {
        if (!Actor) return false;
        RefreshCacheIfNeeded();
        if (CachedActorTokens.Num() == 0 || CachedFlagTokens.Num() == 0) return false;
        if (!CachedFlagTokens.Contains(Flag)) return false;

#if WITH_EDITOR
        const FString Label = Actor->GetActorLabel();
        for (const FString& Token : CachedActorTokens)
        {
            if (!Token.IsEmpty() && Label.Contains(Token)) return true;
        }
#endif
        return false;
    }
}
```

The string-equality cache freshness check is cheap. A nicer-but-optional refinement: register an `FConsoleVariableSinkHandle` so the cache rebuild fires once per CVar change rather than every call. Skip until profiling shows this matters.

### Migrating existing debug sites

Each existing `if (bDebugX) { ... }` site becomes:
```cpp
if (bDebugX || SurfDebug::ShouldDebug(this, "x")) { ... }
```

Where `"x"` is a short, lowercase, snake-case flag name. Proposed canonical naming (extend during implementation by grepping `Source/GoneSurfing/` for `if (.*[Dd]ebug.*)`):

| UPROPERTY                         | Flag string    |
| --------------------------------- | -------------- |
| `bDebugThrust` / `debugThrust`    | `"thrust"`     |
| `bDebugBuoyancy`                  | `"buoyancy"`   |
| `bDebugWeight` / `debug` (on AWeightDistribution) | `"weight"` |
| `bDebugRailLift`                  | `"rail_lift"`  |
| `bDebugPlaning` / `debugPlaning`  | `"planing"`    |
| `bDebugForces`                    | `"forces"`     |
| `bDebugPaddleForce`               | `"paddle"`     |
| `bDebugPumping`                   | `"pumping"`    |

Final list (≤20 entries) compiled during implementation.

## Acceptance Criteria

### AC1: CVar enables debug at a specific actor + flag
Set `surf.debug.actors "left_fin"` and `surf.debug.flags "thrust"`. `FluidDynamicsBP_left_fin` logs / draws thrust debug. No other actor does. No other flag does.

### AC2: Substring match catches multiple instances
Set `surf.debug.actors "left_middle"` and `surf.debug.flags "buoyancy"`. Any actor whose label contains `left_middle` (e.g., `bottom_left_middle_back`, `bottom_left_middle_front`, `rail_left_middle_*`) emits buoyancy debug.

### AC3: Empty CVars = no override
With `surf.debug.actors ""` (or unset), `ShouldDebug` always returns false. Only the existing UPROPERTY checkboxes drive output.

### AC4: Editor checkbox still works
With CVars empty, ticking `bDebugThrust` on a single actor in the editor produces thrust debug for that actor. Same as today.

### AC5: Both paths combine via OR
With `bDebugThrust=true` on actor A AND `surf.debug.actors "B"; surf.debug.flags "thrust"` on actor B, both produce thrust debug. Neither path disables the other.

### AC6: Performance
With both CVars empty, helper overhead per call is dominated by the cache freshness check + early returns; sub-microsecond. Migration must not measurably regress frame time when CVars are empty.

### AC7: -ExecCmds works at startup
Launching `UnrealEditor.exe project.uproject map -game -ExecCmds="surf.debug.actors 'tail'; surf.debug.flags 'thrust'"` produces tail thrust debug from frame 1.

## Open Questions / Future

1. Do we need a verbosity axis (Verbose vs. VeryVerbose) on top of binary on/off? Probably not first pass.
2. Wildcard sentinels — `surf.debug.actors "*"` and `surf.debug.flags "*"` to mean "all". Useful for early exploration before labels are remembered.
3. Match by `Side` UPROPERTY (FluidDynamics-only) as a secondary OR check. Skip until labels prove insufficient.
4. Sink-driven cache rebuild instead of equality check on every call. Microscopic optimization; defer.

## Related Files
- New: `Source/GoneSurfing/SurfDebug.h`, `SurfDebug.cpp`.
- All files in `Source/GoneSurfing/` containing `if (.*[Dd]ebug.*)` — these are the migration sites.
- Companion: [run-game-and-collect-logs.md](run-game-and-collect-logs.md) — consumes the CVars from the command line.
