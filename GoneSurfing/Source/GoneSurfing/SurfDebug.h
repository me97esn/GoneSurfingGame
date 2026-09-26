// Runtime debug-log control. See specs/debug-logging.md.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

namespace SurfDebug
{
	extern TAutoConsoleVariable<FString> CVarDebugActors;  // e.g. "left_middle,right_middle,tail"
	extern TAutoConsoleVariable<FString> CVarDebugFlags;   // e.g. "thrust,buoyancy"
	extern TAutoConsoleVariable<FString> CVarAutopilots;   // e.g. "surfing-down-the-line,surf-straight"

	/** True iff Flag is in CVarDebugFlags AND Actor's label contains any token from CVarDebugActors. */
	bool ShouldDebug(const AActor* Actor, const FString& Flag);

	/** True iff Flag is in CVarDebugFlags. Use for global (non-actor-scoped) debug output. */
	bool IsFlagSet(const FString& Flag);

	/** True if TestName matches any token in CVarAutopilots, OR if the cvar is empty (no filter
	 *  = all autopilots run). Empty TestName never matches a non-empty filter. */
	bool ShouldRunAutopilot(const FString& TestName);
}
