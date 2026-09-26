// See RideHudOverlay.h and specs/two-screen-navigation.md.

#include "RideHudOverlay.h"
#include "SurfLog.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

#include "BackPill.h"

#include "Widgets/Layout/SDPIScaler.h"

namespace
{
	struct FRideHudState
	{
		TSharedPtr<SWidget> Root;
	};
	TMap<UWorld*, FRideHudState> GStates;
}

namespace RideHud
{
	void Install(UWorld* World, TFunction<void()> OnBack)
	{
		if (!World || GStates.Contains(World) || !World->GetGameViewport())
		{
			return;
		}

#if PLATFORM_ANDROID
		const float Scale = 2.0f;   // match the other overlays: phones ~2x pixel density
#else
		const float Scale = 1.0f;
#endif

		TSharedRef<SWidget> Root = SNew(SDPIScaler).DPIScale(Scale)
			.Visibility(EVisibility::SelfHitTestInvisible)   // the scaler must not eat touches
			[
				BackPill::MakeAnchored(MoveTemp(OnBack))
			];

		// ZOrder 250, with the touch controls: above the replay overlay (100) it is the exit from,
		// below the modal tier (260) whose panels must cover it.
		World->GetGameViewport()->AddViewportWidgetContent(Root, /*ZOrder*/ 250);

		FRideHudState State;
		State.Root = Root;
		GStates.Add(World, State);
		UE_LOG(LogSurf, Display, TEXT("RideHud: Installed."));
	}

	void Uninstall(UWorld* World)
	{
		FRideHudState State;
		if (!GStates.RemoveAndCopyValue(World, State))
		{
			return;
		}
		if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
		{
			if (State.Root.IsValid())
			{
				Viewport->RemoveViewportWidgetContent(State.Root.ToSharedRef());
			}
		}
	}

	bool IsInstalled(UWorld* World)
	{
		return World && GStates.Contains(World);
	}
}
