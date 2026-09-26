// See RideListPanel.h and specs/best-ride-replay.md D10/FR6.

#include "RideListPanel.h"

#include "BackPill.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Styling/AppStyle.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/SlateTypes.h"
#include "Styling/CoreStyle.h"

namespace
{
	// RideList_ prefix: duplicate anonymous-namespace names across .cpp files break the Android
	// unity build, which desktop builds never catch.
	//
	// Same palette as BoardPanel and AssistPanel on purpose. Three screens that pause the same wave
	// should not look like they came from three different games.
	const FLinearColor RideList_Scrim(0.02f, 0.05f, 0.09f, 0.90f);
	const FLinearColor RideList_Ink  (1.00f, 1.00f, 1.00f, 0.95f);
	const FLinearColor RideList_Dim  (1.00f, 1.00f, 1.00f, 0.62f);
	const FLinearColor RideList_Faint(1.00f, 1.00f, 1.00f, 0.30f);
	const FLinearColor RideList_Lit  (0.42f, 0.78f, 1.00f, 0.98f);

	const FLinearColor RideList_RowFill (0.055f, 0.085f, 0.130f, 0.94f);
	const FLinearColor RideList_RowEdge (1.000f, 1.000f, 1.000f, 0.20f);
	const FLinearColor RideList_RowHover(0.090f, 0.130f, 0.190f, 0.96f);
	const FLinearColor RideList_RowPress(0.130f, 0.180f, 0.250f, 0.98f);
	const FLinearColor RideList_LastFill(0.070f, 0.135f, 0.205f, 0.96f);

	FSlateFontInfo RideList_Bold(int32 Size)    { return FCoreStyle::GetDefaultFontStyle("Bold", Size); }
	FSlateFontInfo RideList_Regular(int32 Size) { return FCoreStyle::GetDefaultFontStyle("Regular", Size); }

	/** Rows are pills, like every other control in this game, so the shape says "pressable" before
	 *  a word of the row is read. The last ride gets the accented edge rather than a bigger font:
	 *  emphasis that does not disturb the alignment of the numbers down the right-hand side. */
	const FButtonStyle& RideList_RowStyle(bool bEmphasis)
	{
		static const FButtonStyle Plain = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(RideList_RowFill,  8.0f, RideList_RowEdge, 1.0f))
			.SetHovered(FSlateRoundedBoxBrush(RideList_RowHover, 8.0f, RideList_RowEdge, 1.0f))
			.SetPressed(FSlateRoundedBoxBrush(RideList_RowPress, 8.0f, RideList_Lit,     1.0f))
			.SetNormalPadding(FMargin(0))
			.SetPressedPadding(FMargin(0));

		static const FButtonStyle Emphasis = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(RideList_LastFill, 8.0f, RideList_Lit,     2.0f))
			.SetHovered(FSlateRoundedBoxBrush(RideList_RowHover, 8.0f, RideList_Lit,     2.0f))
			.SetPressed(FSlateRoundedBoxBrush(RideList_RowPress, 8.0f, RideList_Lit,     2.0f))
			.SetNormalPadding(FMargin(0))
			.SetPressedPadding(FMargin(0));

		return bEmphasis ? Emphasis : Plain;
	}

	/** m:ss. A four-second wipeout should look like one at a glance, which "4" alone does not. */
	FString RideList_Duration(float Seconds)
	{
		const int32 Total = FMath::Max(0, FMath::RoundToInt(Seconds));
		return FString::Printf(TEXT("%d:%02d"), Total / 60, Total % 60);
	}
}

/** The close notification, carrying whether a row press is about to follow. */
DECLARE_DELEGATE_OneParam(FRideListClosed, bool /*bToPlay*/);

class SRideListPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRideListPanel) {}
		SLATE_ARGUMENT(const TArray<FRideListEntry>*, Entries)
		SLATE_ARGUMENT(RideListPanel::FHooks, Hooks)
		SLATE_ARGUMENT(int32, HighlightIndex)
		SLATE_EVENT(FRideListClosed, OnClosed)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Hooks = InArgs._Hooks;
		OnClosed = InArgs._OnClosed;
		HighlightIndex = InArgs._HighlightIndex;

		TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
		if (InArgs._Entries)
		{
			for (int32 i = 0; i < InArgs._Entries->Num(); ++i)
			{
				Rows->AddSlot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
				[
					MakeRow((*InArgs._Entries)[i], i)
				];
			}
		}

		ChildSlot
		[
			SNew(SOverlay)

			+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)
			[
				SNew(SImage).Image(FAppStyle::GetBrush("WhiteBrush")).ColorAndOpacity(RideList_Scrim)
			]

			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(SScaleBox)
				.Stretch(EStretch::ScaleToFit)
				.StretchDirection(EStretchDirection::Both)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[
						SNew(STextBlock)
						.Font(RideList_Bold(15))
						.ColorAndOpacity(RideList_Dim)
						.Text(NSLOCTEXT("GoneSurfing", "WatchARide", "WATCH A RIDE"))
					]

					// Says what a row IS before the player has pressed one. Without it the first
					// read of this screen is "why is my last ride in a menu?".
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 18.0f)
					[
						SNew(STextBlock)
						.Font(RideList_Regular(11))
						.ColorAndOpacity(RideList_Faint)
						.Text(NSLOCTEXT("GoneSurfing", "WatchARideSub",
							"Your last ride, and your best on every board you have ridden"))
					]

					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
					[
						Rows
					]

				]
			]

			// The exit, in the corner every other screen puts it in - NOT under the rows, where it
			// used to sit. TickReplay opens this list over a finished replay on a timer, and the
			// ride HUD's pill is what the player was reaching for when it did; a BACK that jumped
			// to screen centre at that moment was the whole complaint. Last slot, so it draws over
			// the scrim.
			+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)
			[
				BackPill::MakeAnchored([this]() { OnBackPressed(); })
			]
		];
	}

private:
	TSharedRef<SWidget> MakeRow(const FRideListEntry& Entry, int32 Index)
	{
		// 560 wide so the longest row - "BEST  SHORTBOARD  RIDING NOW" plus a four-digit score -
		// never has to wrap, and every score lands on the same right edge whatever the board name.
		return SNew(SBox).WidthOverride(560.0f).HeightOverride(72.0f)
		[
			SNew(SButton)
			.ButtonStyle(&RideList_RowStyle(Entry.bIsLastRide || Index == HighlightIndex))
			.ContentPadding(FMargin(18.0f, 0.0f))
			.HAlign(HAlign_Fill).VAlign(VAlign_Center)
			.OnClicked(this, &SRideListPanel::OnRowPressed, Index)
			[
				SNew(SHorizontalBox)

				// Left: which ride, then which board. Kind above name because the player is
				// choosing a RIDE here, not a board - the board is how they tell two bests apart.
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Font(RideList_Bold(10))
						.ColorAndOpacity(Entry.bIsLastRide ? FSlateColor(RideList_Lit) : FSlateColor(RideList_Faint))
						// "YOUR BOARD" borrows the rack's own words for the same fact, so the two
						// screens do not name the player's board two different ways.
						.Text(FText::FromString(Entry.bIsCurrentBoard
							? Entry.KindLabel + TEXT("   ·   YOUR BOARD")
							: Entry.KindLabel))
					]

					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 3.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Font(RideList_Bold(17))
						.ColorAndOpacity(FSlateColor(RideList_Ink))
						.Text(FText::FromString(Entry.BoardName.ToUpper()))
					]
				]

				// Right: the number, and how long it took to earn. Right-aligned so the column of
				// scores can be compared without reading the names beside them.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(14.0f, 0.0f, 0.0f, 0.0f)
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
					[
						SNew(STextBlock)
						.Font(RideList_Bold(20))
						.ColorAndOpacity(FSlateColor(RideList_Ink))
						.Text(FText::AsNumber(Entry.Score))
					]

					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0.0f, 2.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Font(RideList_Regular(11))
						.ColorAndOpacity(RideList_Faint)
						.Text(FText::FromString(RideList_Duration(Entry.DurationSeconds)))
					]
				]
			]
		];
	}

	FReply OnRowPressed(int32 Index)
	{
		// Close FIRST, then play. The panel pauses the world; a replay started underneath a paused
		// modal would sit frozen behind it, which reads exactly like the button doing nothing.
		RideListPanel::FHooks Copy = Hooks;
		OnClosed.ExecuteIfBound(/*bToPlay*/true);
		if (Copy.PlayEntry)
		{
			Copy.PlayEntry(Index);
		}
		return FReply::Handled();
	}

	void OnBackPressed()
	{
		OnClosed.ExecuteIfBound(/*bToPlay*/false);
	}

	RideListPanel::FHooks Hooks;
	FRideListClosed OnClosed;
	int32 HighlightIndex = INDEX_NONE;
};

namespace
{
	struct FRideListState
	{
		TSharedPtr<SWidget> Root;
		TSharedPtr<SRideListPanel> Widget;
		bool bWePaused = false;
		RideListPanel::FHooks Hooks;
	};

	TMap<UWorld*, TSharedPtr<FRideListState>> GRideListStates;

	void RideList_Destroy(UWorld* World, bool bToPlay = false)
	{
		TSharedPtr<FRideListState> State;
		if (!GRideListStates.RemoveAndCopyValue(World, State) || !State.IsValid()) { return; }

		if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
		{
			if (State->Root.IsValid())
			{
				Viewport->RemoveViewportWidgetContent(State->Root.ToSharedRef());
			}
		}

		// Only undo a pause we caused - the same rule the rack follows, for the same reason: this
		// can be opened from a screen that was already paused, and resuming there would drop the
		// player into a running wave behind a menu.
		if (State->bWePaused && World)
		{
			UGameplayStatics::SetGamePaused(World, false);
		}

		// NFR3's other half: the caller puts the UMG bar back. Fired after the widget is gone, so
		// the bar can never be live under a panel that is still up.
		if (State->Hooks.OnClosed)
		{
			State->Hooks.OnClosed(bToPlay);
		}
	}
}

namespace RideListPanel
{
	void Open(UWorld* World, const TArray<FRideListEntry>& Entries, const FHooks& Hooks, int32 HighlightIndex)
	{
		if (!World || Entries.Num() == 0) { return; }

		// A stale entry can survive a PIE session: the map is keyed on a raw UWorld* and PIE reuses
		// those pointers. Left alone it makes IsOpen() report true for ever and the list never opens
		// again - which looks exactly like "the button does nothing".
		if (TSharedPtr<FRideListState>* Existing = GRideListStates.Find(World))
		{
			if (Existing->IsValid() && (*Existing)->Widget.IsValid())
			{
				return;
			}
			GRideListStates.Remove(World);
		}

		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!Viewport)
		{
			UE_LOG(LogTemp, Warning, TEXT("RideListPanel: no game viewport; cannot open"));
			return;
		}

#if PLATFORM_ANDROID
		const float Scale = 2.0f;
#else
		const float Scale = 1.0f;
#endif

		TSharedPtr<FRideListState> State = MakeShared<FRideListState>();
		State->Hooks = Hooks;

		// The rows are copied into the widget's own construction above, so this reference does not
		// have to outlive the call - unlike BoardPanel, whose profiles live in a subsystem.
		TSharedPtr<SRideListPanel> Widget;
		TSharedRef<SWidget> Root = SNew(SDPIScaler).DPIScale(Scale)
			[
				SAssignNew(Widget, SRideListPanel)
				.Entries(&Entries)
				.Hooks(Hooks)
				.HighlightIndex(HighlightIndex)
				.OnClosed(FRideListClosed::CreateLambda([World](bool bToPlay) { RideList_Destroy(World, bToPlay); }))
			];

		State->Root = Root;
		State->Widget = Widget;
		State->bWePaused = !UGameplayStatics::IsGamePaused(World);
		if (State->bWePaused)
		{
			UGameplayStatics::SetGamePaused(World, true);
		}

		// ZOrder 260: the modal tier the rack and the assist panel share, on the understanding that
		// two of them are never up at once. This one opens from the ride bar, never from the rack.
		Viewport->AddViewportWidgetContent(Root, /*ZOrder*/ 260);
		GRideListStates.Add(World, State);
	}

	bool IsOpen(UWorld* World)
	{
		if (!World) { return false; }
		const TSharedPtr<FRideListState>* Found = GRideListStates.Find(World);
		return Found && Found->IsValid() && (*Found)->Widget.IsValid();
	}

	void Close(UWorld* World)     { RideList_Destroy(World); }
	void Uninstall(UWorld* World) { RideList_Destroy(World); }
}
