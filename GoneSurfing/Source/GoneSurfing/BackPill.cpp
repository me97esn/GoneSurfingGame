// See BackPill.h.

#include "BackPill.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"

namespace
{
	// The start screen's ghost tier, by value: a translucent scrim with a keyline, white label.
	// Kept identical so the BACK on the ride and the ‹ BACK on the tutorial cards read as the same
	// control. (StartTutorialOverlay keeps its palette in an anonymous namespace; these are copies,
	// which is the price of not making that file export them.)
	const FLinearColor BackPill_GhostFill   (1.0f, 1.0f, 1.0f, 0.18f);
	const FLinearColor BackPill_GhostOutline(1.0f, 1.0f, 1.0f, 0.60f);
	const float        BackPill_GhostStroke = 2.0f;
	const float        BackPill_LabelSize   = 18.0f;

	const FSlateBrush* BackPill_GhostBrush()
	{
		static const FSlateRoundedBoxBrush B = []
		{
			FSlateRoundedBoxBrush Brush(BackPill_GhostFill);
			Brush.OutlineSettings = FSlateBrushOutlineSettings(BackPill_GhostOutline, BackPill_GhostStroke);
			return Brush;
		}();
		return &B;
	}

	/** SButton wants an FOnClicked; every caller here has a plain void(). One adapter, held alive by
	 *  the lambda capture the button owns. */
	class SBackPill : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SBackPill) {}
		SLATE_END_ARGS()

		TFunction<void()> OnPressed;

		void Construct(const FArguments&)
		{
			ChildSlot
			[
				SNew(SButton)
				.ButtonStyle(FCoreStyle::Get(), "NoBorder")
				.OnClicked(this, &SBackPill::Clicked)
				.ContentPadding(FMargin(BackPill::kHitMargin))   // invisible, hit-testable margin
				[
					SNew(SBorder)
					.BorderImage(BackPill_GhostBrush())
					.Padding(FMargin(24.0f, 12.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("‹ BACK")))
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", BackPill_LabelSize))
						.ColorAndOpacity(FSlateColor(FLinearColor::White))
						.ShadowOffset(FVector2D(1.0f, 2.0f))
						.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.45f))
					]
				]
			];
		}

	private:
		FReply Clicked()
		{
			if (OnPressed)
			{
				OnPressed();
			}
			return FReply::Handled();
		}
	};
}

namespace BackPill
{
	// The touch zones begin 18 % down the screen (about 97 px on the phone's 540-high space), so the
	// pill with its padding must end above that.
	//
	// The pill itself is 42 px tall (18 px label + 12 px vertical padding) and its HIT AREA extends
	// kHitMargin further on every side, invisibly: at 2x on a 1080-high phone the visible pill is
	// ~84 px (5 mm) and the target ~132 px (8 mm), which is where Android's 48 dp floor sits. The
	// 16 px label / 36 px pill it replaced was ~4.5 mm and got missed. The visible offset from the
	// corner stays 18 px: kEdgePadding + kHitMargin. 6 + 12 + 42 + 12 = 72 < 97, still above the
	// touch zones.
	const float kEdgePadding = 6.0f;
	const float kHitMargin   = 12.0f;

	TSharedRef<SWidget> MakeAnchored(TFunction<void()> OnPressed)
	{
		TSharedPtr<SBackPill> Pill;
		TSharedRef<SWidget> Anchored =
			SNew(SHorizontalBox)
			// The box is allotted the whole viewport. Only the pill may be hit-testable, or it eats
			// every tap meant for the stick and the pump beneath it - the mistake the score counter
			// and the assist badge each made once.
			.Visibility(EVisibility::SelfHitTestInvisible)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top)
				.Padding(kEdgePadding, kEdgePadding, 0.0f, 0.0f)
			[
				SAssignNew(Pill, SBackPill)
			];

		Pill->OnPressed = MoveTemp(OnPressed);
		return Anchored;
	}
}
