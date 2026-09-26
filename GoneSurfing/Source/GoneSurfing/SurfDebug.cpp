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

	TAutoConsoleVariable<FString> CVarAutopilots(
		TEXT("surf.autopilots"), TEXT(""),
		TEXT("Comma-separated TestName substrings; only autopilots matching any of these run. Empty = no filter."));

	namespace
	{
		// Cached parsed tokens — refreshed when any cvar's raw value changes.
		TArray<FString> CachedActorTokens;
		TArray<FString> CachedFlagTokens;
		TArray<FString> CachedAutopilotTokens;
		FString LastActorsRaw;
		FString LastFlagsRaw;
		FString LastAutopilotsRaw;

		void RefreshCacheIfNeeded()
		{
			const FString CurActors     = CVarDebugActors.GetValueOnGameThread();
			const FString CurFlags      = CVarDebugFlags.GetValueOnGameThread();
			const FString CurAutopilots = CVarAutopilots.GetValueOnGameThread();

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
			if (CurAutopilots != LastAutopilotsRaw)
			{
				LastAutopilotsRaw = CurAutopilots;
				CachedAutopilotTokens.Reset();
				CurAutopilots.ParseIntoArray(CachedAutopilotTokens, TEXT(","), /*CullEmpty=*/true);
				for (FString& T : CachedAutopilotTokens) T = T.TrimStartAndEnd();
			}
		}
	}

	bool ShouldDebug(const AActor* Actor, const FString& Flag)
	{
#if !UE_BUILD_SHIPPING
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
#endif
		return false;
	}

	bool IsFlagSet(const FString& Flag)
	{
#if !UE_BUILD_SHIPPING
		RefreshCacheIfNeeded();
		return CachedFlagTokens.Contains(Flag);
#else
		return false;
#endif
	}

	bool ShouldRunAutopilot(const FString& TestName)
	{
#if !UE_BUILD_SHIPPING
		RefreshCacheIfNeeded();
		if (CachedAutopilotTokens.Num() == 0) return true; // empty filter = no restriction
		if (TestName.IsEmpty()) return false;              // empty TestName can't match a token
		for (const FString& Token : CachedAutopilotTokens)
		{
			if (!Token.IsEmpty() && TestName.Contains(Token)) return true;
		}
		return false;
#else
		return true;
#endif
	}
}
