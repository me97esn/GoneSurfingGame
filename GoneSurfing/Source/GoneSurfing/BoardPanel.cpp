// See BoardPanel.h and specs/board-selection.md D6.

#include "BoardPanel.h"

#include "BackPill.h"
#include "BoardOutlineGlyph.h"
#include "SurfBoards.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/SlateTypes.h"
#include "Styling/CoreStyle.h"
#include "Styling/AppStyle.h"

namespace
{
	// BoardPanel_ prefix: duplicate anonymous-namespace names across .cpp files break the Android
	// unity build, which desktop builds never catch.
	//
	// Same palette as AssistPanel deliberately: two screens that pause the same wave should not look
	// like they came from different games.
	const FLinearColor BoardPanel_Scrim(0.02f, 0.05f, 0.09f, 0.90f);
	const FLinearColor BoardPanel_Ink  (1.00f, 1.00f, 1.00f, 0.95f);
	const FLinearColor BoardPanel_Dim  (1.00f, 1.00f, 1.00f, 0.62f);
	const FLinearColor BoardPanel_Faint(1.00f, 1.00f, 1.00f, 0.26f);
	const FLinearColor BoardPanel_Lit  (0.42f, 0.78f, 1.00f, 0.98f);
	/** Difficulty gets the warm accent because it is not a capability like the other two - it is
	 *  what the board will cost you. Separating them by colour says that without a word. */
	const FLinearColor BoardPanel_Gold (1.00f, 0.78f, 0.34f, 1.00f);
	const FLinearColor BoardPanel_Off  (1.00f, 1.00f, 1.00f, 0.14f);

	/** Card surfaces. A bare outline on a scrim reads as a diagram, not a control - the eye needs a
	 *  surface with an edge before it treats something as pressable. Rounded because every other
	 *  button in this game is (Restart, Replay, Back), so the shape itself says "button" before any
	 *  of the content is read. */
	// Near-opaque, and a touch LIGHTER than the scrim behind them, so a card reads as a surface
	// sitting on the screen rather than a tinted window onto it. At 4% white the paused surfer was
	// visible straight through the selected card, which is the opposite of a raised control.
	const FLinearColor BoardPanel_CardFill    (0.055f, 0.085f, 0.130f, 0.94f);
	const FLinearColor BoardPanel_CardEdge    (1.000f, 1.000f, 1.000f, 0.20f);
	const FLinearColor BoardPanel_CardHover   (0.090f, 0.130f, 0.190f, 0.96f);
	const FLinearColor BoardPanel_CardPress   (0.130f, 0.180f, 0.250f, 0.98f);
	const FLinearColor BoardPanel_SelFill     (0.070f, 0.135f, 0.205f, 0.96f);
	const FLinearColor BoardPanel_SelEdge     (0.420f, 0.780f, 1.000f, 0.90f);

	/** Two styles, so the board being ridden is not a shade of the same thing. Selected gets a
	 *  brighter, thicker edge AND a tinted surface; the card also carries a RIDING chip and draws its
	 *  board in the accent. Four cues, because one subtle one was missed. */
	/** The accented button is the one solid, warm fill on the screen - the same role SURF AGAIN's
	 *  coral plays on the wipeout card. Everything else here is cool and outlined. */
	/** Both action buttons share one pair of looks - accented and quiet - so emphasis can move
	 *  between them without either ever looking like a different KIND of control. */
	const FButtonStyle& BoardPanel_ActionStyle(bool bAccent);

	FSlateColor BoardPanel_ActionInk(bool bAccent)
	{
		return FSlateColor(bAccent ? FLinearColor(0.04f, 0.09f, 0.14f, 1.0f)
								   : FLinearColor(1.0f, 1.0f, 1.0f, 0.86f));
	}

	const FButtonStyle& BoardPanel_QuietStyle()
	{
		static const FButtonStyle Style = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.07f), 34.0f,
											  FLinearColor(1.0f, 1.0f, 1.0f, 0.34f), 1.0f))
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.14f), 34.0f,
											  FLinearColor(1.0f, 1.0f, 1.0f, 0.50f), 1.0f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.20f), 34.0f,
											  FLinearColor(1.0f, 1.0f, 1.0f, 0.70f), 1.0f))
			.SetNormalPadding(FMargin(0)).SetPressedPadding(FMargin(0));
		return Style;
	}

	const FButtonStyle& BoardPanel_ResumeStyle()
	{
		static const FButtonStyle Style = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(FLinearColor(0.42f, 0.78f, 1.00f, 0.92f), 34.0f))
			.SetHovered(FSlateRoundedBoxBrush(FLinearColor(0.56f, 0.85f, 1.00f, 0.97f), 34.0f))
			.SetPressed(FSlateRoundedBoxBrush(FLinearColor(0.32f, 0.62f, 0.85f, 1.00f), 34.0f))
			.SetNormalPadding(FMargin(0))
			.SetPressedPadding(FMargin(0));
		return Style;
	}

	const FButtonStyle& BoardPanel_ActionStyle(bool bAccent)
	{
		return bAccent ? BoardPanel_ResumeStyle() : BoardPanel_QuietStyle();
	}

	const FButtonStyle& BoardPanel_CardStyle(bool bSelected)
	{
		static const FButtonStyle Plain = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(BoardPanel_CardFill,  8.0f, BoardPanel_CardEdge, 1.0f))
			.SetHovered(FSlateRoundedBoxBrush(BoardPanel_CardHover, 8.0f, BoardPanel_CardEdge, 1.0f))
			.SetPressed(FSlateRoundedBoxBrush(BoardPanel_CardPress, 8.0f, BoardPanel_SelEdge,  1.0f))
			.SetNormalPadding(FMargin(0))
			.SetPressedPadding(FMargin(0));

		static const FButtonStyle Selected = FButtonStyle()
			.SetNormal (FSlateRoundedBoxBrush(BoardPanel_SelFill,   8.0f, BoardPanel_SelEdge, 2.0f))
			.SetHovered(FSlateRoundedBoxBrush(BoardPanel_CardHover, 8.0f, BoardPanel_SelEdge, 2.0f))
			.SetPressed(FSlateRoundedBoxBrush(BoardPanel_CardPress, 8.0f, BoardPanel_SelEdge, 2.0f))
			.SetNormalPadding(FMargin(0))
			.SetPressedPadding(FMargin(0));

		return bSelected ? Selected : Plain;
	}

	FSlateFontInfo BoardPanel_Bold(int32 Size)    { return FCoreStyle::GetDefaultFontStyle("Bold", Size); }
	FSlateFontInfo BoardPanel_Regular(int32 Size) { return FCoreStyle::GetDefaultFontStyle("Regular", Size); }

	/** One rating row: a label and five pips.
	 *
	 *  Pips are drawn, not typed. The obvious implementation is a row of star characters, and on
	 *  Android that is a coin flip on font coverage - tofu boxes in the one screen that sells the
	 *  boards would be fatal. The assist badge already draws its strength this way.
	 */
	TSharedRef<SWidget> BoardPanel_Rating(const FText& Label, int32 Filled, const FLinearColor& OnColor)
	{
		TSharedRef<SHorizontalBox> Pips = SNew(SHorizontalBox);
		for (int32 i = 0; i < 5; ++i)
		{
			Pips->AddSlot().AutoWidth().Padding(1.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(9.0f).HeightOverride(9.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("WhiteBrush"))
					.ColorAndOpacity(i < Filled ? OnColor : BoardPanel_Off)
				]
			];
		}

		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				// 104, not 80: the labels grew with the type, and "DIFFICULTY" clipped at the old
				// width - the same way it did the first time these were sized.
				SNew(SBox).WidthOverride(104.0f)
				[
					SNew(STextBlock)
					.Font(BoardPanel_Regular(13))
					.ColorAndOpacity(BoardPanel_Faint)
					.Text(Label)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[ Pips ];
	}
}

class SBoardPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SBoardPanel) {}
		SLATE_ARGUMENT(const TArray<FSurfBoardProfile>*, Profiles)
		SLATE_ARGUMENT(BoardPanel::FHooks, Hooks)
		SLATE_EVENT(FSimpleDelegate, OnClosed)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Hooks    = InArgs._Hooks;
		OnClosed = InArgs._OnClosed;
		PendingIndex = Hooks.GetActiveIndex ? Hooks.GetActiveIndex() : 0;

		const TArray<FSurfBoardProfile>& Profiles = *InArgs._Profiles;

		// ONE scale for every board in the row. The longest shape is 2.0 units by construction
		// (Tools/svg2boards.py), so this fixes the tallest board's height and every other board
		// lands proportionally shorter - which is the entire point of the screen.
		const float kTallestPx = 190.0f;
		const float PixelsPerUnit = kTallestPx / 2.0f;

		TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
		for (int32 i = 0; i < Profiles.Num(); ++i)
		{
			Row->AddSlot().AutoWidth().Padding(6.0f, 0.0f)
			[
				MakeCard(Profiles[i], i, PixelsPerUnit)
			];
		}

		ChildSlot
		[
			SNew(SOverlay)

			+ SOverlay::Slot()
			[
				SNew(SImage).Image(FAppStyle::GetBrush("WhiteBrush")).ColorAndOpacity(BoardPanel_Scrim)
			]

			+ SOverlay::Slot()
			.Padding(FMargin(20.0f, 12.0f))
			[
				// The rack is designed at one size and SHRUNK to whatever screen it lands on, rather
				// than laid out against numbers that assume a screen.
				//
				// It has to be, because the two screens differ by about 2x in the units Slate lays
				// out in, and nothing on screen says which one you are on. A desktop window has
				// roughly 2300x1040 of layout space to work with; the phone has about 1200x540 - the
				// DPI curve divides the phone's pixels DOWN and the desktop's UP. So a rack tuned
				// until it fitted a PC window sailed off both edges of the phone, with the cards
				// overflowing to the right and the descriptions cut off at the bottom - while every
				// windowed screenshot of it, including the one taken at the phone's PIXEL size, kept
				// showing it fitting comfortably.
				//
				// ScaleToFit measures instead of assuming, so the whole rack lands whole on any
				// screen and occupies the same share of it everywhere. Both directions, not
				// DownOnly: the rack is a compact block by design - one description, not five - and
				// held at 1:1 it sat small and lost in a desktop window. Slate re-rasterises text
				// and rounded boxes at the scaled size, so growing costs no sharpness.
				SNew(SScaleBox)
				.Stretch(EStretch::ScaleToFit)
				.StretchDirection(EStretchDirection::Both)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 16.0f)
				[
					SNew(STextBlock)
					.Font(BoardPanel_Bold(15))
					.ColorAndOpacity(BoardPanel_Dim)
					.Text(NSLOCTEXT("GoneSurfing", "ChooseBoard", "CHOOSE YOUR BOARD"))
				]

				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					// A FLOOR, not a fixed height. The selected card carries a paragraph the others
					// do not and its board lies down to pay for it, so the two heights nearly but
					// not exactly cancel - and left alone the row would breathe a few pixels each
					// time a different card was picked, which with the scale box around it would
					// resize the whole screen rather than just move the buttons.
					//
					// A floor holds it still for every description that fits under it, and a
					// minimum rather than an override means a longer one later grows the row
					// instead of being clipped by it - the failure mode of every fixed height this
					// screen has had.
					SNew(SBox).MinDesiredHeight(462.0f)
					[
						Row
					]
				]

				// The confirming action, in the flow directly below the cards - NOT anchored to the
				// screen bottom. Anchoring did hold it still, but only at the window size its
				// padding was tuned at: the cards are centred and the buttons were bottom-pinned,
				// so at any other resolution the two drifted and the buttons landed on top of the
				// cards. The fixed row height above is what actually stops them moving; being in
				// the flow is what keeps them below the cards at every window size.
				//
				// BACK used to share this row, dismissive-left. It is in the corner now, with every
				// other screen's BACK - see BackPill.h. What is left here is the choice the rack
				// exists to take.
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 20.0f, 0.0f, 0.0f)
				[
					SNew(SBox).HeightOverride(68.0f)
					[
						SAssignNew(RestartButton, SButton)
						.ButtonStyle(&BoardPanel_ActionStyle(false))
						.HAlign(HAlign_Center).VAlign(VAlign_Center)
						// No width override. "RIDE THE SHORTBOARD" is long, and every fixed width
						// guessed on this screen has clipped something; let the content size the
						// button.
						.ContentPadding(FMargin(34.0f, 0.0f))
						.OnClicked(this, &SBoardPanel::OnRestartPressed)
						[
							SNew(STextBlock)
							.Font(BoardPanel_Bold(20))
							.ColorAndOpacity(this, &SBoardPanel::RestartInk)
							.Text(this, &SBoardPanel::RestartText)
						]
					]
				]
				]
			]

			// The exit, in the one corner it lives in on every screen. Last slot, over the scrim -
			// and outside the SScaleBox above, so it stays the size the ride's pill is rather than
			// being scaled with the rack.
			+ SOverlay::Slot().HAlign(HAlign_Fill).VAlign(VAlign_Fill)
			[
				BackPill::MakeAnchored([this]() { OnResumePressed(); })
			]
		];
	}

private:
	BoardPanel::FHooks Hooks;
	FSimpleDelegate OnClosed;
	/** One per card, so the surface can be re-styled when the selection moves. */
	TArray<TSharedPtr<SButton>> CardButtons;
	TSharedPtr<SButton> RestartButton;

	/** The card the player has picked but not yet committed. Starts on the active board, and only
	 *  reaches the game when RIDE THE <board> is pressed. */
	int32 PendingIndex = 0;
	/** Likewise, so a board can be laid down and stood back up as the selection moves. */
	TArray<TSharedPtr<SBoardOutlineGlyph>> CardGlyphs;

	TSharedRef<SWidget> MakeCard(const FSurfBoardProfile& P, int32 Index, float PixelsPerUnit)
	{
		const FString ShapeId = P.OutlineShapeId;

		// Everything that depends on selection is BOUND, not baked. The screen no longer closes when
		// you pick, so a card has to be able to become the selected one while it is on screen.
		TSharedPtr<SButton> Card;
		TSharedPtr<SBoardOutlineGlyph> Glyph;
		TSharedRef<SWidget> Content =
			SNew(SVerticalBox)

			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				// Hidden, not collapsed, on the other cards - it holds its row so the boards stay on
				// one baseline whichever card is selected.
				SNew(SBox).HeightOverride(14.0f)
				.Visibility(this, &SBoardPanel::ChipVisibility, Index)
				[
					SNew(STextBlock)
					.Font(BoardPanel_Bold(9))
					.ColorAndOpacity(BoardPanel_Lit)
					.Text(this, &SBoardPanel::ChipText)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 6.0f, 0.0f, 0.0f)
			[
				// The selected card LAYS ITS BOARD DOWN. Height is the scarce axis here - the
				// description, the pip rows and the best score all have to fit under a 190px board
				// on a 540px screen, and they did not: the selected card ran off the bottom of the
				// phone and took YOUR BEST SCORE with it. Landscape gives ~140px back for nothing
				// but a rotation, which is why the height override is unset when selected: the box
				// then takes the glyph's own (much shorter) desired size, per board, with no magic
				// number to keep in step with the artwork.
				//
				// What this trades is the shared BASELINE, not the shared SCALE. PixelsPerUnit is
				// unchanged, so the laid-down board's width is still its true length - it is read
				// across instead of up. The four unselected boards keep their common baseline and
				// go on comparing with each other, which is what the rack is for; the selected card
				// is the one being read, not the one being compared.
				SNew(SBox)
				.HeightOverride(TAttribute<FOptionalSize>::CreateSP(this, &SBoardPanel::ArtHeight, Index))
				.VAlign(VAlign_Bottom)
				[
					SAssignNew(Glyph, SBoardOutlineGlyph)
					.ShapeId(ShapeId)
					.Facing(TAttribute<EBoardGlyphFacing>::CreateSP(this, &SBoardPanel::CardFacing, Index))
					.PixelsPerUnit(PixelsPerUnit)
					.LineColor(this, &SBoardPanel::CardInk, Index)
					.Thickness(this, &SBoardPanel::CardStroke, Index)
				]
			]

			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 14.0f, 0.0f, 0.0f)
			[
				// Fixed row: the selected card takes a larger face, and left to size itself that
				// alone would make the row a few pixels taller the moment you picked a board.
				SNew(SBox).HeightOverride(28.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(this, &SBoardPanel::CardTitleFont, Index)
					.ColorAndOpacity(this, &SBoardPanel::CardInkSlate, Index)
					.Text(FText::FromString(P.DisplayName.ToUpper()))
				]
			]

			// Fixed height, not auto: a two-line tagline next to a one-line one pushed the pip rows
			// out of step across cards, and rows that do not line up cannot be compared - which is
			// the entire job of this screen.
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(150.0f).HeightOverride(30.0f)
				// The one-liner steps aside for the full description rather than sitting above it
				// saying a shorter version of the same thing.
				.Visibility(EVisibility::Collapsed)   // superseded by the always-reserved description
				[
					SNew(STextBlock)
					.Font(BoardPanel_Regular(10))
					.ColorAndOpacity(BoardPanel_Faint)
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
					.Text(FText::FromString(P.Tagline))
				]
			]

			// The long description, on the selected card alone - see DescriptionVisibility.
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 12.0f, 0.0f, 0.0f)
			[
				// Sizes itself. It used to be pinned to a fixed height to stop the card - and with it
				// the whole row, which stretches every card to the tallest - changing height and
				// moving the buttons below. No single number worked: tall enough for the foamie's
				// 313 characters left a hole under the shorter boards, and every value tried clipped
				// something. The buttons are anchored to the screen instead, so this is free again.
				SNew(SBox).WidthOverride(294.0f)
				.Visibility(this, &SBoardPanel::DescriptionVisibility, Index)
				[
					SNew(STextBlock)
					.Font(BoardPanel_Regular(14))
					.ColorAndOpacity(BoardPanel_Dim)
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
					.Text(FText::FromString(P.Description))
				]
			]

			// Everything above floats; the ratings hang off the bottom. Cards are stretched to the
			// tallest in the row (an SHorizontalBox slot fills vertically), so this one fill slot is
			// what puts SPEED/TURNING/DIFFICULTY on the same line across all five - and the reason
			// no card needs a height of its own to make that happen. Without it each card stacked
			// from the top and the pips landed wherever that card's reserved description left them:
			// three rows at one height, two at two others, on a screen whose whole job is comparing
			// those rows. It also absorbs the leftover space that used to sit under the pips as a void.
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNullWidget::NullWidget
			]

			// Centred, not stretched: left-aligned rows sat against the card edge.
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 14.0f, 0.0f, 0.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)
				[ BoardPanel_Rating(NSLOCTEXT("GoneSurfing", "RSpeed", "SPEED"), P.RatingSpeed, BoardPanel_Lit) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)
				[ BoardPanel_Rating(NSLOCTEXT("GoneSurfing", "RTurn", "TURNING"), P.RatingTurning, BoardPanel_Lit) ]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)
				[ BoardPanel_Rating(NSLOCTEXT("GoneSurfing", "RDiff", "DIFFICULTY"), P.RatingDifficulty, BoardPanel_Gold) ]

				// Your record on THIS board. It belongs on the card because a best is a fact about
				// you and that board together - and because a board you have never ridden then shows
				// a dash, which invites rather than compares.
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 9.0f, 0.0f, 0.0f)
				[
					// Label at its natural width, value pushed to the far edge - rather than a fixed
					// label box like the rating rows use. The label is already the longest thing in
					// the row, so letting the row span the card is what stops it clipping; a value
					// at the edge also reads as a summary line rather than a fourth rating.
					//
					// "YOUR BEST", not "YOUR BEST SCORE": the longer wording left a four-digit score
					// nowhere to go on the four NARROW cards, which are the ones a player reads while
					// comparing. The selected card had room; the other four are the constraint.
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(BoardPanel_Regular(13))
						.ColorAndOpacity(BoardPanel_Faint)
						.Text(NSLOCTEXT("GoneSurfing", "RBest", "YOUR BEST"))
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).HAlign(HAlign_Right)
					.Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Font(BoardPanel_Bold(15))
						.ColorAndOpacity(FSlateColor(BoardPanel_Ink))
						.Text(this, &SBoardPanel::BestScoreText, Index)
					]
				]
			];

		// The selected card WIDENS to hold its description. Width, not height, because two or three
		// sentences in a 172px column is a ~15-character measure - unreadable regardless of how much
		// vertical room it gets. At the expanded width it is about 45 characters, which reads.
		//
		// The boards keep their scale and their shared baseline through this, so widening a card
		// never disturbs the size comparison the rack exists for. Only the row's horizontal packing
		// moves, and that motion is a direct response to the click that caused it.
		// The selected card widens; the row below it holds a minimum height so that widening cannot
		// move anything. See the MinDesiredHeight on the row in Construct.
		TSharedRef<SWidget> Widget = SNew(SBox)
		.WidthOverride(TAttribute<FOptionalSize>::CreateSP(this, &SBoardPanel::CardWidth, Index))
		[
			// The whole card is the button, not just the drawing.
			SAssignNew(Card, SButton)
			.ButtonStyle(&BoardPanel_CardStyle(IsSelected(Index)))
			.ContentPadding(FMargin(10.0f, 12.0f))
			.OnClicked(this, &SBoardPanel::OnCardPressed, Index)
			[
				Content
			]
		];

		CardButtons.Add(Card);
		CardGlyphs.Add(Glyph);
		return Widget;
	}

	/** SButton takes a style pointer rather than an attribute, so the surface is re-pointed when the
	 *  selection moves rather than re-evaluated per frame. */
	void RefreshCardStyles()
	{
		// The action button re-points too: ButtonStyle is a style argument rather than an attribute,
		// so it cannot follow the pick on its own.
		if (RestartButton.IsValid()) { RestartButton->SetButtonStyle(&BoardPanel_ActionStyle(PickChanged())); }

		for (int32 i = 0; i < CardButtons.Num(); ++i)
		{
			if (CardButtons[i].IsValid())
			{
				CardButtons[i]->SetButtonStyle(&BoardPanel_CardStyle(IsSelected(i)));
			}
		}

		// The glyph's facing decides its DESIRED SIZE, and Slate caches that. Colour and thickness
		// are attributes read at paint time and need nothing; an orientation flip is a layout change
		// and has to be asked for, or the board rotates inside a box still sized for the old one.
		for (int32 i = 0; i < CardGlyphs.Num(); ++i)
		{
			if (CardGlyphs[i].IsValid())
			{
				CardGlyphs[i]->Invalidate(EInvalidateWidgetReason::Layout);
			}
		}
	}

	FText BestScoreText(int32 Index) const
	{
		const int32 Best = Hooks.GetBestScore ? Hooks.GetBestScore(Index) : -1;
		return Best < 0 ? NSLOCTEXT("GoneSurfing", "BestNone", "—")
						: FText::AsNumber(Best);
	}

	/** The card you are reading is wider. Not decoration: at the narrow width a description is a
	 *  ~20-character measure, and the extra width wraps it into fewer, longer lines - which is where
	 *  most of the rack's height comes back on a phone.
	 *
	 *  200, not 172, on the others: the rating rows carry a 104px label column now, and "DIFFICULTY"
	 *  plus five pips does not fit a 172px card. */
	FOptionalSize CardWidth(int32 Index) const
	{
		return IsSelected(Index) ? 320.0f : 200.0f;
	}

	/** The card you are reading gets the bigger name. Sized against the height the laid-down board
	 *  gave back, not guessed - and only on the selected card, so the other four keep the compact
	 *  title the rack was measured for. */
	FSlateFontInfo CardTitleFont(int32 Index) const
	{
		return BoardPanel_Bold(IsSelected(Index) ? 19 : 14);
	}

	/** Nose up in the rack; the selected board lies down. The ~150px that buys is what the
	 *  description is written into, and it is why the selected card is no taller than its
	 *  neighbours despite carrying a paragraph they do not. PixelsPerUnit is unchanged, so the
	 *  laid-down board is still its true length - read across instead of up. */
	EBoardGlyphFacing CardFacing(int32 Index) const
	{
		return IsSelected(Index) ? EBoardGlyphFacing::NoseRight : EBoardGlyphFacing::NoseUp;
	}

	/** 196 holds the shared baseline across the standing boards - 6px of air over the tallest. The
	 *  selected card returns an UNSET size instead of a smaller number: the box then fits its
	 *  laid-down glyph exactly, per board, so no constant here can drift out of step with the art. */
	FOptionalSize ArtHeight(int32 Index) const
	{
		return IsSelected(Index) ? FOptionalSize() : FOptionalSize(196.0f);
	}

	EVisibility TaglineVisibility(int32 Index) const
	{
		return IsSelected(Index) ? EVisibility::Collapsed : EVisibility::Visible;
	}

	/** The selected card only, and COLLAPSED on the rest so they reserve nothing for it.
	 *
	 *  Showing all five read well on a monitor and failed on the phone. Five paragraphs plus five
	 *  full-height boards is a tall, wide block, and the rack scales to fit whatever screen it is
	 *  on - so on the phone everything shrank to ~0.6 and the text became unreadable. One
	 *  description, in a wider card, over a board laid on its side is a much smaller block, so it
	 *  scales down far less and the type stays large. The screen the type has to be legible on is
	 *  the phone. */
	EVisibility DescriptionVisibility(int32 Index) const
	{
		return IsSelected(Index) ? EVisibility::Visible : EVisibility::Collapsed;
	}

	EVisibility ChipVisibility(int32 Index) const
	{
		return IsRidingNow(Index) ? EVisibility::Visible : EVisibility::Hidden;
	}

	/** The chip sits on the board the player will ride, which a pick changes on the spot (the rack
	 *  opens from the hub, between waves). There is no pending state for it to have to explain. */
	FText ChipText() const
	{
		return NSLOCTEXT("GoneSurfing", "BoardYours", "YOUR BOARD");
	}

	FLinearColor CardInk(int32 Index) const
	{
		return IsSelected(Index) ? BoardPanel_Lit : BoardPanel_Ink;
	}

	FSlateColor CardInkSlate(int32 Index) const { return FSlateColor(CardInk(Index)); }

	float CardStroke(int32 Index) const { return IsSelected(Index) ? 2.4f : 1.5f; }

	FReply OnCardPressed(int32 Index)
	{
		// Provisional only. A click is how you READ a board - it opens the description - so it must
		// not also change the board under the player. Committing on click made browsing destructive
		// and made the HUD button claim a board the rack said was not active yet.
		PendingIndex = Index;
		RefreshCardStyles();
		return FReply::Handled();
	}

	/** Emphasis, not behaviour: the accent sits on whichever button matches what the player has just
	 *  done - picked a different board, or not. Both buttons do the same thing either way. */
	bool PickChanged() const
	{
		const int32 Active = Hooks.GetActiveIndex ? Hooks.GetActiveIndex() : 0;
		return PendingIndex != Active;
	}

	FSlateColor RestartInk() const { return BoardPanel_ActionInk(PickChanged()); }

	FText RestartText() const
	{
		const FString Name = Hooks.GetBoardName ? Hooks.GetBoardName(PendingIndex) : FString();
		return Name.IsEmpty()
			? NSLOCTEXT("GoneSurfing", "BoardChoosePlain", "CHOOSE")
			: FText::FromString(FString::Printf(TEXT("RIDE THE %s"), *Name.ToUpper()));
	}

	/** Takes the pick. Applied on the spot - the hub is between waves - so the next START rides it;
	 *  nothing is reloaded (specs/two-screen-navigation.md FR5). */
	FReply OnRestartPressed()
	{
		if (Hooks.ChooseBoard)
		{
			Hooks.ChooseBoard(PendingIndex);
		}
		if (Hooks.OnClosed) { Hooks.OnClosed(); }
		OnClosed.ExecuteIfBound();
		return FReply::Handled();
	}

	/** Discards the pick and carries on. Nothing was applied on click, so there is nothing to undo. */
	void OnResumePressed()
	{
		if (Hooks.OnClosed) { Hooks.OnClosed(); }
		OnClosed.ExecuteIfBound();
	}

	/** The card the player has picked - what the highlight, the description and the card width all
	 *  follow. Not necessarily the board being ridden. */
	bool IsSelected(int32 Index) const
	{
		return PendingIndex == Index;
	}

	/** The board actually applied. Only this card wears YOUR BOARD, so the rack never claims a
	 *  switch has happened before it has. */
	bool IsRidingNow(int32 Index) const
	{
		return Hooks.GetActiveIndex && Hooks.GetActiveIndex() == Index;
	}

};

// ---- install / open / close --------------------------------------------------------------------
namespace
{
	struct FBoardPanelState
	{
		TSharedPtr<SWidget> Root;
		TSharedPtr<SBoardPanel> Widget;
		bool bWePaused = false;
	};
	TMap<UWorld*, TSharedPtr<FBoardPanelState>> GBoardPanelStates;

	void BoardPanel_Destroy(UWorld* World)
	{
		TSharedPtr<FBoardPanelState> State;
		if (!GBoardPanelStates.RemoveAndCopyValue(World, State) || !State.IsValid()) { return; }
		if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
		{
			if (State->Root.IsValid())
			{
				Viewport->RemoveViewportWidgetContent(State->Root.ToSharedRef());
			}
		}
		// Only undo a pause we caused. The start screen pauses too, and this is most often opened
		// from it - resuming there would drop the player into a running wave behind a menu.
		if (State->bWePaused && World)
		{
			UGameplayStatics::SetGamePaused(World, false);
		}
	}
}

namespace BoardPanel
{
	void Open(UWorld* World, const TArray<FSurfBoardProfile>& Profiles, const FHooks& Hooks)
	{
		if (!World || Profiles.Num() == 0) { return; }

		// A stale entry can survive a PIE session: the map is keyed on a raw UWorld* and PIE reuses
		// those pointers. Left alone it makes IsOpen() report true for ever and the rack never opens
		// again - which looks exactly like "the button does nothing".
		if (TSharedPtr<FBoardPanelState>* Existing = GBoardPanelStates.Find(World))
		{
			if (Existing->IsValid() && (*Existing)->Widget.IsValid())
			{
				return;
			}
			GBoardPanelStates.Remove(World);
		}

		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!Viewport)
		{
			UE_LOG(LogTemp, Warning, TEXT("BoardPanel: no game viewport; cannot open"));
			return;
		}

#if PLATFORM_ANDROID
		const float Scale = 2.0f;
#else
		const float Scale = 1.0f;
#endif

		TSharedPtr<FBoardPanelState> State = MakeShared<FBoardPanelState>();

		TSharedPtr<SBoardPanel> Widget;
		TSharedRef<SWidget> Root = SNew(SDPIScaler).DPIScale(Scale)
			[
				SAssignNew(Widget, SBoardPanel)
				.Profiles(&Profiles)
				.Hooks(Hooks)
				.OnClosed(FSimpleDelegate::CreateLambda([World]() { BoardPanel_Destroy(World); }))
			];

		State->Root = Root;
		State->Widget = Widget;
		State->bWePaused = !UGameplayStatics::IsGamePaused(World);
		if (State->bWePaused)
		{
			UGameplayStatics::SetGamePaused(World, true);
		}

		// ZOrder 260: the same modal tier as the assist panel. The two are never up together.
		Viewport->AddViewportWidgetContent(Root, /*ZOrder*/ 260);
		GBoardPanelStates.Add(World, State);
	}

	bool IsOpen(UWorld* World)
	{
		if (!World) { return false; }
		const TSharedPtr<FBoardPanelState>* Found = GBoardPanelStates.Find(World);
		return Found && Found->IsValid() && (*Found)->Widget.IsValid();
	}

	void Close(UWorld* World)     { BoardPanel_Destroy(World); }
	void Uninstall(UWorld* World) { BoardPanel_Destroy(World); }
}
