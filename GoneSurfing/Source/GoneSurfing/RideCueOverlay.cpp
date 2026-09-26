// See RideCueOverlay.h and specs/start-screen-tutorial.md.

#include "RideCueOverlay.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"
#include "Animation/CurveSequence.h"
#include "Animation/CurveHandle.h"

namespace
{
	const FLinearColor Cue_White(FLinearColor::White);
	const FLinearColor Cue_Gold (FColor(0xFF, 0xC8, 0x57)); // celebratory "…and surf!" tint
}

// Centered upper-third cue text with a soft shadow; fades out on the handoff "go".
class SRideCue : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRideCue) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		SetVisibility(EVisibility::HitTestInvisible); // purely cosmetic — never eats input

		ChildSlot
		[
			// Lower third, not upper. The cue used to sit at ~29% of screen height, which was clear
			// when the top of the screen was empty. It no longer is: the control bar moved to the top
			// and the score dropped below it to clear the bar, so 25-45% now belongs to the score and
			// the cue was landing on top of it. The bottom half is water and, since the bar left it,
			// free.
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().FillHeight(2.3f) [ SNew(SSpacer) ]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SAssignNew(Label, STextBlock)
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 40))
				.Justification(ETextJustify::Center)
				.ShadowOffset(FVector2D(2.0f, 2.0f))
				.ShadowColorAndOpacity(FLinearColor(0, 0, 0, 0.6f))
			]
			+ SVerticalBox::Slot().FillHeight(1.0f) [ SNew(SSpacer) ]
		];

		Label->SetColorAndOpacity(TAttribute<FSlateColor>::CreateSP(this, &SRideCue::LabelColor));
	}

	void ShowWaiting(const FText& Text)
	{
		if (bGo || !Label.IsValid()) return;          // never revert once the handoff has fired
		if (!Label->GetText().EqualTo(Text)) Label->SetText(Text);
	}

	void PlayGo(const FText& Text)
	{
		if (bGo || !Label.IsValid()) return;          // one-shot
		bGo = true;
		Label->SetText(Text);
		Label->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 60));

		Fade = FCurveSequence();
		FadeHandle = Fade.AddCurve(1.0f /*hold*/, 1.4f /*fade*/, ECurveEaseFunction::CubicIn);
		Fade.Play(AsShared());
		RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SRideCue::TickFade));
	}

private:
	TSharedPtr<STextBlock> Label;
	FCurveSequence Fade;
	FCurveHandle FadeHandle;
	bool bGo = false;

	FSlateColor LabelColor() const
	{
		const float Alpha = bGo ? (1.0f - FadeHandle.GetLerp()) : 1.0f;
		const FLinearColor Base = bGo ? Cue_Gold : Cue_White;
		return FSlateColor(FLinearColor(Base.R, Base.G, Base.B, Alpha));
	}

	EActiveTimerReturnType TickFade(double, float)
	{
		if (bGo && !Fade.IsPlaying())
		{
			SetVisibility(EVisibility::Collapsed);
			return EActiveTimerReturnType::Stop;
		}
		return EActiveTimerReturnType::Continue;
	}
};

// ---- install / update / uninstall ------------------------------------------------------------
namespace
{
	struct FCueState
	{
		TSharedPtr<SWidget> Root;
		TSharedPtr<SRideCue> Widget;
	};
	TMap<UWorld*, TSharedPtr<FCueState>> GCueStates;

	SRideCue* EnsureCue(UWorld* World)
	{
		if (!World) return nullptr;
		if (TSharedPtr<FCueState>* Found = GCueStates.Find(World))
		{
			return Found->IsValid() ? (*Found)->Widget.Get() : nullptr;
		}
		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!Viewport) return nullptr;

#if PLATFORM_ANDROID
		const float Scale = 2.0f;
#else
		const float Scale = 1.0f;
#endif

		TSharedPtr<SRideCue> Widget;
		TSharedRef<SWidget> Root = SNew(SDPIScaler).DPIScale(Scale)
			[
				SAssignNew(Widget, SRideCue)
			];

		TSharedPtr<FCueState> State = MakeShared<FCueState>();
		State->Root = Root;
		State->Widget = Widget;

		Viewport->AddViewportWidgetContent(Root, /*ZOrder*/ 150); // above the game, below the start overlay
		GCueStates.Add(World, State);
		return Widget.Get();
	}
}

namespace RideCue
{
	void ShowWaiting(UWorld* World, const FText& Text)
	{
		if (SRideCue* Cue = EnsureCue(World)) Cue->ShowWaiting(Text);
	}

	void PlayGo(UWorld* World, const FText& Text)
	{
		if (SRideCue* Cue = EnsureCue(World)) Cue->PlayGo(Text);
	}

	void Uninstall(UWorld* World)
	{
		TSharedPtr<FCueState> State;
		if (!GCueStates.RemoveAndCopyValue(World, State) || !State.IsValid()) return;
		if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
		{
			if (State->Root.IsValid())
			{
				Viewport->RemoveViewportWidgetContent(State->Root.ToSharedRef());
			}
		}
	}
}
