// See RideScoreOverlay.h and specs/ride-score-counter.md.

#include "RideScoreOverlay.h"

#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

#include "Widgets/SCompoundWidget.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/CoreStyle.h"
#include "Styling/AppStyle.h"
#include "Rendering/DrawElements.h"
#include "HAL/IConsoleManager.h"

// Capture aid: freeze the celebration at its worst case - full snap, burst mid-flight - so a
// screenshot can check that nothing crops off the screen edge. The real thing is 2.4 s with a
// 0.19 s peak, which no capture pipeline hits by chance (screenshot-game skill: stretch the
// transition, don't race it). Read at the use site, so it takes from a launch -ExecCmds.
static TAutoConsoleVariable<int32> CVarScoreHoldPeak(
	TEXT("surf.score.holdpeak"), 0,
	TEXT("Ride-score counter: 1 = pin the turn celebration at its peak (snap + burst) for layout captures."));

namespace
{
	// Score_ prefix: duplicate free-function and constant names in anonymous namespaces across .cpp
	// files break the Android unity build, which desktop builds never catch.

	// Two inks, and the gap between them is the whole of FR2. A rate change alone is not readable in
	// peripheral vision on a moving wave, so the earning/not-earning states differ in COLOUR as well
	// as in speed. Warm and bright while the player is holding the line unaided; cool and dimmed
	// while the guard is doing the work.
	const FLinearColor Score_InkEarning(1.0f, 0.87f, 0.42f, 0.97f);
	// Cool and subordinate, NOT faded out. Measured on a -Phone capture at alpha 0.55 this washed
	// almost entirely into bright water, which is the assist badge's original mistake repeated:
	// quiet is not the same as invisible. The contrast that carries FR2 is the HUE (warm gold vs
	// cool grey-blue), so the guarded state can stay legible and still read as the lesser one.
	const FLinearColor Score_InkGuarded(0.70f, 0.80f, 0.92f, 0.75f);
	const FLinearColor Score_InkBest   (1.0f, 1.0f, 1.0f, 0.88f);

	/** The chain badge, while an earning window is open and linked (specs/trick-scoring.md FR10).
	 *  Hotter than the earning ink so a chain reads as a different state rather than as more of the
	 *  same one - the counter is already gold while the player is earning at full rate. */

	// ---- The celebration -------------------------------------------------------------------------
	//
	// Crossing a threshold is the player's milestone - the one moment the whole fade exists to
	// produce. The first attempt was a symmetric 1.1s swell to 1.4x and it read as nothing. Device
	// verdict 2026-08-28: "the numbers were just a bit bigger, then a moment later they were smaller
	// again."
	//
	// The lesson is that a symmetric swell is not a celebration, it is a wobble. A milestone needs a
	// SNAP - an attack fast enough to register as an event - and then a long settle, so the eye is
	// caught by the hit and then has time to read what happened. It also needs more than one thing to
	// move: a number changing size on its own is ambiguous, whereas a burst plus a caption plus the
	// assist badge stepping down on the same frame is unmistakably ONE event with a cause.
	constexpr float kScorePulseSeconds = 2.4f;
	/** Fraction of the pulse spent snapping up. Short: this is the hit. */
	constexpr float kScorePulseAttack = 0.055f;
	/** Fraction spent HELD at full size before the settle begins.
	 *
	 *  Without this the curve sheds most of its scale inside half a second and a capture 1s in shows
	 *  a number barely larger than normal - measured on a -Phone grab, and the same failure the first
	 *  build was reported for. A snap with no hold is a flicker: the eye needs the big state to
	 *  persist long enough to register as a state, not just as a transient. */
	constexpr float kScorePulseHold = 0.19f;
	/** Extra scale at the peak of the snap, on top of 1.
	 *
	 *  Bounded by the top of the SCREEN, not by taste. The number's box is ~70 logical px tall and
	 *  sits near y=0, so scaling it about its centre pushed the digits clean off the top edge - which
	 *  is what the first mid-pulse captures showed and what "the numbers were just a bit bigger"
	 *  partly was: the big part was cropped away. This peak, kScoreTopPadding and kScorePulsePivotY
	 *  are one arithmetic package. Change one and re-check the other two. */
	constexpr float kScorePulseScale = 0.8f;
	/** Resting distance from the top of the screen, logical px. Sized so the peak fits below it. */
	constexpr float kScoreTopPadding = 26.0f;

	/** Resting distance from the RIGHT edge, logical px. The counter sits in a top corner, and the
	 *  reason is the surfer: the camera keeps the board dead centre, so anything on the vertical
	 *  centre line lands on top of it. That is exactly what happened when the old UMG bar moved to
	 *  the top and the counter was pushed down by a fifth of the screen to clear it - the number
	 *  sat on the surfer for most of every ride. A corner is the one place the board never is, and
	 *  below it is sky and flat shoulder, so the snap (which grows downward from its pivot) and the
	 *  burst fan into nothing that matters. Top-RIGHT since 2026-09-14: the top-left corner went
	 *  to the ride's Back button, because reading order runs left to right and the way out belongs
	 *  where the eye starts; the score is a glance, not a target, so the far corner suits it
	 *  (specs/two-screen-navigation.md FR2). Sized so the number's right edge stays on screen at
	 *  the peak of the snap: the snap grows both ways from centre, so it needs kScorePulseScale/2
	 *  of the number's width (~50 px for three digits) of room. */
	constexpr float kScoreRightPadding = 56.0f;
	/** Scale pivot, as a fraction down the number's box. Below centre, so the snap grows mostly
	 *  DOWNWARD into empty screen instead of upward into the edge. */
	constexpr float kScorePulsePivotY = 0.4f;
	/** Gap from the number to the best-to-beat line below it. Sized so the number at full snap
	 *  stops just short of it. */
	constexpr float kScoreBestGap = 34.0f;
	// The burst is a radial spray of small shards, painted in OnPaint the same way the assist panel
	// draws its confetti.
	//
	// It started as a single WhiteBrush SImage scaled up behind the number, and that was wrong for a
	// reason worth writing down: WhiteBrush is a solid RECTANGLE, so the effect was a white box
	// expanding to ~480px over the wave. That reads as a rendering glitch, not a celebration. Shards
	// carry the same "something burst outward" without ever looking like a failure to draw.
	constexpr int32 kScoreBurstShards = 18;
	/** Radius the shards travel from, and to, in logical px. */
	constexpr float kScoreBurstRadiusFrom = 42.0f;
	constexpr float kScoreBurstRadiusTo = 190.0f;
	constexpr float kScoreBurstShardSize = 9.0f;
	/** The burst is over well before the number has finished settling, so it punctuates the moment
	 *  rather than sitting on top of the wave the player is still riding. */
	constexpr float kScoreBurstFraction = 0.55f;


	// Plain language, and about the PLAYER rather than the system: the milestone is "you need less
	// help", not "a parameter decreased". No pitch/roll/yaw, no "authority", no "alpha".
	// The assist's own captions are gone for good - a board's assist level is fixed, so the help
	// never decreases and "less help needed" is a claim the game no longer makes. The caption is a
	// parameter now, set by whatever fired the celebration (specs/trick-scoring.md FR6).

	/** No thousands separator on purpose. A space or comma at four digits makes the number jump
	 *  width as it crosses 1000, which reads as the counter glitching rather than climbing. */
	FString Score_Format(int32 Value)
	{
		return FString::FromInt(FMath::Max(0, Value));
	}

	/** 0 at rest, 1 at the top of the snap, easing back to 0 over the settle. */
	float Score_PulseCurve(float Elapsed01)
	{
		if (Elapsed01 <= 0.0f || Elapsed01 >= 1.0f)
		{
			return 0.0f;
		}
		if (Elapsed01 < kScorePulseAttack)
		{
			// Fast but eased. Linear here reads as mechanical rather than as an impact.
			const float A = Elapsed01 / kScorePulseAttack;
			return A * A * (3.0f - 2.0f * A);
		}
		if (Elapsed01 < kScorePulseAttack + kScorePulseHold)
		{
			return 1.0f;   // held at full - this is the part that actually registers
		}
		// Cubic fall: most of the remaining size is shed early, then it eases the last of the way
		// home, so the number stays readable at a glance through the settle.
		const float Start = kScorePulseAttack + kScorePulseHold;
		const float D = (Elapsed01 - Start) / (1.0f - Start);
		const float Inv = 1.0f - D;
		return Inv * Inv * Inv;
	}
}

// Top-centre counter: the score, a burst and caption during a level-up, and a BEST line once the
// ride is over.
class SRideScoreCounter : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRideScoreCounter) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		// SelfHitTestInvisible, and it is not optional: this widget is allotted the WHOLE viewport,
		// and a root left at the default Visible is hit-testable across all of it - which would eat
		// every tap meant for the wave, the assist badge and the Restart button below. The same flaw
		// made the assist badge dead once already; see gradual-control-handoff.md's gotchas.
		SetVisibility(EVisibility::SelfHitTestInvisible);
		SetCanTick(true); // the snap is driven from Tick; ReplayOverlayHUD needed this explicitly too

		ChildSlot
		[
			SNew(SVerticalBox)
			// Top-RIGHT. The column is right-anchored as a whole; inside it the number and the
			// best-to-beat line share a centre, so the small line hangs under the middle of the
			// big one however many digits it has.
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
				.Padding(0.0f, kScoreTopPadding, kScoreRightPadding, 0.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					// 58, and it is not a big number here. Sizes go through the 2x SDPIScaler into a
					// 540-high space, next to UMG buttons whose text is ~34 - measured on a -Phone
					// capture, 46 read SMALLER than the Restart button, which is the wrong pecking
					// order for the one thing the player is supposed to be earning. The assist badge
					// made this exact mistake first (font 11, unreadable without zooming in).
					SAssignNew(ScoreText, STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 58))
					.ShadowOffset(FVector2D(2.0f, 2.0f))
					.ShadowColorAndOpacity(FLinearColor(0, 0, 0, 0.8f))
					.RenderTransformPivot(FVector2D(0.5f, kScorePulsePivotY))
				]
				// No words under the number. There was a caption ("BIG TURN", "AGAIN!", "LINKED!")
				// and a x2 / x3 chain badge here, and both were cut on the first device session:
				// reading text takes the eyes off the wave, and it was not obvious what the words
				// meant. A player who just made a good turn already knows it. What they need is the
				// number visibly reacting - that the game NOTICED - not an explanation of by how
				// much. The snap and the burst do that on their own. (The lesson kept from the
				// caption: a fading text's shadow must follow its alpha, or it smears grey.)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, kScoreBestGap, 0.0f, 0.0f)
				[
					SAssignNew(BestText, STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 22))
					.ShadowOffset(FVector2D(1.0f, 1.0f))
					.ShadowColorAndOpacity(FLinearColor(0, 0, 0, 0.7f))
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f) [ SNew(SSpacer) ]
		];

		ScoreText->SetColorAndOpacity(
			TAttribute<FSlateColor>::CreateSP(this, &SRideScoreCounter::ScoreColor));
		BestText->SetColorAndOpacity(
			TAttribute<FSlateColor>::CreateSP(this, &SRideScoreCounter::BestColor));
	}

	void SetScore(int32 InScore, bool bInEarningFast, int32 InBest, bool bInRideOver)
	{
		Score = InScore;
		bEarningFast = bInEarningFast;
		Best = InBest;
		bRideOver = bInRideOver;
		bCounterVisible = true;

		ScoreText->SetText(FText::FromString(Score_Format(Score)));

		// The best is a thing to BEAT, so it only earns screen space once the ride is over and the
		// player is deciding whether to go again. Mid-ride it is noise next to the number that is
		// actually moving. Suppressed until there is one, so a first-ever ride is not told its best
		// is zero.
		const bool bShowBest = bRideOver && Best > 0;
		BestText->SetText(bShowBest
			? FText::FromString(FString::Printf(TEXT("BEST  %s"), *Score_Format(Best)))
			: FText::GetEmpty());
	}

	void HideCounter()
	{
		bCounterVisible = false;
		ScoreText->SetText(FText::GetEmpty());
		BestText->SetText(FText::GetEmpty());
		PulseRemaining = 0.0f;
		bPulseBurst = false;
	}

	/** The number snaps and settles; with bWithBurst the shards fly too. Nothing is written.
	 *
	 *  Two tiers because the burst covers the wave for a moment and an effect that fires on every
	 *  turn is wallpaper within one ride - but every scored turn still moves the number visibly,
	 *  which is the whole signal: the game noticed. */
	void Pulse(bool bWithBurst)
	{
		// Restart cleanly from the snap rather than resuming a settle in progress. A good rider
		// links big turns faster than the 2.4 s settle, and blending into a running animation reads
		// as the digits wobbling - which is the exact failure this effect was rebuilt to stop.
		PulseRemaining = kScorePulseSeconds;
		bPulseBurst = bWithBurst;
	}

	// Slate ticks on the app clock, not the world clock, so the celebration still animates while the
	// assist panel has the world paused.
	virtual void Tick(const FGeometry& Geo, const double CurrentTime, const float DeltaTime) override
	{
		SCompoundWidget::Tick(Geo, CurrentTime, DeltaTime);

		if (CVarScoreHoldPeak.GetValueOnGameThread() != 0)
		{
			// Middle of the hold: scale at its peak, shards about a third of the way out. Set every
			// tick so it needs no real turn to start it and never settles.
			PulseRemaining = kScorePulseSeconds * (1.0f - (kScorePulseAttack + kScorePulseHold * 0.5f));
			bPulseBurst = true;
		}

		if (PulseRemaining <= 0.0f)
		{
			if (bTransformSet)
			{
				ScoreText->SetRenderTransform(TOptional<FSlateRenderTransform>());
				bTransformSet = false;
			}
			return;
		}

		PulseRemaining = FMath::Max(0.0f, PulseRemaining - DeltaTime);

		const float K = PulseK();
		const float Scale = 1.0f + kScorePulseScale * K;
		ScoreText->SetRenderTransform(FSlateRenderTransform(FScale2D(Scale, Scale)));
		bTransformSet = true;

		// The burst is drawn in OnPaint from time-dependent state, and nothing else tells Slate this
		// widget's paint has changed: the number's own SetText only invalidates while the score is
		// actually moving. On a frozen score (ride over, or the holdpeak capture) the cached paint
		// was reused and the shards never appeared - measured: zero burst pixels in a capture with
		// the pulse pinned at its peak. Ask for a repaint every tick the pulse is alive.
		Invalidate(EInvalidateWidgetReason::Paint);
	}

	// The burst. Painted rather than composed from widgets, same as the assist panel's confetti: a
	// scaled SImage can only ever be a growing rectangle, and shards need their own positions.
	//
	// Drawn AFTER the children so it reads as bursting out over the number rather than from behind
	// it, and it starts outside the glyphs (kScoreBurstRadiusFrom) so it never obscures the figure
	// the player is trying to read.
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geo, const FSlateRect& Clip,
		FSlateWindowElementList& Out, int32 LayerId, const FWidgetStyle& Style,
		bool bParentEnabled) const override
	{
		const int32 ChildLayer = SCompoundWidget::OnPaint(Args, Geo, Clip, Out, LayerId, Style, bParentEnabled);

		if (!bCounterVisible || PulseRemaining <= 0.0f || !bPulseBurst)
		{
			return ChildLayer;
		}
		const float U = 1.0f - (PulseRemaining / kScorePulseSeconds);
		const float BurstU = U / kScoreBurstFraction;
		if (BurstU >= 1.0f)
		{
			return ChildLayer;   // burst is over; the number is still settling
		}

		// Ease out: the shards leave fast and coast, which is what makes it read as an impact rather
		// than as something inflating.
		const float Spread = 1.0f - (1.0f - BurstU) * (1.0f - BurstU);
		const float Radius = FMath::Lerp(kScoreBurstRadiusFrom, kScoreBurstRadiusTo, Spread);
		// Linear fade, not squared. Squared put the shards below a fifth of their opacity within a
		// third of the burst, so they were effectively invisible for most of their own animation.
		const float Fade = 1.0f - BurstU;

		// Centre on the number's box, from the LAYOUT rather than from cached geometry. It was a
		// constant once and went stale when the top padding grew; then it read the number's cached
		// geometry back through AbsoluteToLocal, which on a desktop -game window (DPI-curve scale
		// 0.5) resolved to the far bottom-right corner - measured at local (2361, 932) of 2402x1081
		// with the number sitting top-left, so the shards were flying out of the joystick ring.
		// The column is anchored at a fixed padding from the right edge now, so its centre is plain
		// arithmetic from the allotted width: the number is centred inside a column as wide as its
		// widest line, and the render-transform snap scales about that same point.
		float ColumnWidth = 0.0f;
		float NumberHeight = 72.0f;
		if (ScoreText.IsValid())
		{
			const FVector2D NumSize = ScoreText->GetDesiredSize();
			ColumnWidth = NumSize.X;
			NumberHeight = NumSize.Y;
		}
		if (BestText.IsValid())
		{
			ColumnWidth = FMath::Max(ColumnWidth, (float)BestText->GetDesiredSize().X);
		}
		const FVector2D Centre(Geo.GetLocalSize().X - kScoreRightPadding - ColumnWidth * 0.5f,
		                       kScoreTopPadding + NumberHeight * 0.5f);
		const FSlateBrush* Brush = FAppStyle::GetBrush("WhiteBrush");
		const int32 BurstLayer = ChildLayer + 1;

		for (int32 i = 0; i < kScoreBurstShards; ++i)
		{
			// Angles derived from the index, never from Math::Rand: a burst that lands differently
			// every time it is captured is one you cannot compare between builds.
			const float Angle = (2.0f * PI * i) / kScoreBurstShards;
			// Slight per-shard variation in reach so the ring does not look like a drawn circle.
			const float Wobble = 0.72f + 0.28f * FMath::Abs(FMath::Sin(i * 2.399f));
			const float Size = kScoreBurstShardSize * (0.55f + 0.45f * Wobble) * (0.4f + 0.6f * Fade);

			const FVector2D P = Centre + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * (Radius * Wobble);

			FSlateDrawElement::MakeBox(
				Out, BurstLayer,
				Geo.ToPaintGeometry(FVector2D(Size, Size), FSlateLayoutTransform(P - FVector2D(Size * 0.5f, Size * 0.5f))),
				Brush, ESlateDrawEffect::None,
				FLinearColor(1.0f, 0.94f, 0.70f, Fade));
		}
		return BurstLayer;
	}

private:
	TSharedPtr<STextBlock> ScoreText;
	TSharedPtr<STextBlock> BestText;

	int32 Score = 0;
	int32 Best = 0;
	bool bEarningFast = false;
	bool bRideOver = false;
	bool bCounterVisible = false;
	/** Whether the current pulse carries the burst (a big turn) or is the number alone. */
	bool bPulseBurst = false;

	float PulseRemaining = 0.0f;
	bool bTransformSet = false;

	/** 0 at rest, 1 at the peak of the celebration. */
	float PulseK() const
	{
		if (PulseRemaining <= 0.0f) { return 0.0f; }
		return Score_PulseCurve(1.0f - (PulseRemaining / kScorePulseSeconds));
	}

	FSlateColor ScoreColor() const
	{
		if (!bCounterVisible)
		{
			return FSlateColor(FLinearColor(0, 0, 0, 0));
		}

		// A ride that has ended is neither earning nor being guarded - it is finished. Show it in the
		// earning ink so the final number reads as an achievement rather than as a reprimand.
		FLinearColor Ink = (bEarningFast || bRideOver) ? Score_InkEarning : Score_InkGuarded;

		// Whitens as it snaps, so the level-up reads even where the scale change alone might not -
		// notably against bright water, which is most of this screen.
		const float K = PulseK();
		if (K > 0.0f)
		{
			Ink = FMath::Lerp(Ink, FLinearColor::White, K);
		}
		return FSlateColor(Ink);
	}

	FSlateColor BestColor() const
	{
		return bCounterVisible ? FSlateColor(Score_InkBest) : FSlateColor(FLinearColor(0, 0, 0, 0));
	}

};

// ---- install / update / uninstall ---------------------------------------------------------------
namespace
{
	struct FScoreState
	{
		TSharedPtr<SWidget> Root;
		TSharedPtr<SRideScoreCounter> Widget;
	};
	TMap<UWorld*, TSharedPtr<FScoreState>> GScoreStates;

	SRideScoreCounter* Score_EnsureCounter(UWorld* World)
	{
		if (!World) return nullptr;
		if (TSharedPtr<FScoreState>* Found = GScoreStates.Find(World))
		{
			return Found->IsValid() ? (*Found)->Widget.Get() : nullptr;
		}
		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!Viewport) return nullptr;

		// Phone screens are ~2x the density of a desktop monitor, so every size in this file is that
		// many LOGICAL pixels in a 540-high space. Judge them on a -Phone screenshot; the editor
		// viewport lies about all of it.
#if PLATFORM_ANDROID
		const float Scale = 2.0f;
#else
		const float Scale = 1.0f;
#endif

		TSharedPtr<SRideScoreCounter> Widget;
		TSharedRef<SWidget> Root = SNew(SDPIScaler).DPIScale(Scale)
			.Visibility(EVisibility::SelfHitTestInvisible)
			[
				SAssignNew(Widget, SRideScoreCounter)
			];

		// ZOrder 210, alongside the assist badge: above the start/tutorial overlay (200, whose scrim
		// is a full-screen hit-testable border) and below the assist panel (260) and the dev HUDs.
		Viewport->AddViewportWidgetContent(Root, /*ZOrder*/ 210);

		TSharedPtr<FScoreState> State = MakeShared<FScoreState>();
		State->Root = Root;
		State->Widget = Widget;
		GScoreStates.Add(World, State);
		return Widget.Get();
	}

	SRideScoreCounter* Score_FindCounter(UWorld* World)
	{
		if (!World) return nullptr;
		TSharedPtr<FScoreState>* Found = GScoreStates.Find(World);
		return (Found && Found->IsValid()) ? (*Found)->Widget.Get() : nullptr;
	}
}

namespace RideScore
{
	void Show(UWorld* World, int32 Score, bool bEarningFast, int32 BestScore, bool bRideOver)
	{
		if (SRideScoreCounter* C = Score_EnsureCounter(World))
		{
			C->SetScore(Score, bEarningFast, BestScore, bRideOver);
		}
	}

	void PlayCelebration(UWorld* World, bool bBigTurn)
	{
		// Find, not ensure: a celebration on a counter that was never shown would install the widget
		// for one animation and leave it behind.
		if (SRideScoreCounter* C = Score_FindCounter(World)) { C->Pulse(bBigTurn); }
	}

	void Hide(UWorld* World)
	{
		// Deliberately does NOT install. The common case - assist off, and every automated run - must
		// not spawn a widget just to hide it.
		if (SRideScoreCounter* C = Score_FindCounter(World)) { C->HideCounter(); }
	}

	void Uninstall(UWorld* World)
	{
		TSharedPtr<FScoreState> State;
		if (!GScoreStates.RemoveAndCopyValue(World, State) || !State.IsValid()) return;
		if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
		{
			if (State->Root.IsValid())
			{
				Viewport->RemoveViewportWidgetContent(State->Root.ToSharedRef());
			}
		}
	}
}
