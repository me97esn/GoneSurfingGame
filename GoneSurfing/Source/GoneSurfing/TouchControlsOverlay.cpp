// See TouchControlsOverlay.h and specs/pump-button-and-virtual-stick.md.
//
// STRUCTURE, and why it is not one widget: the drawing widget covers the whole screen and is
// HitTestInvisible, so it can never block anything; input comes from two small pads placed over the
// zones. A single full-screen widget that is hit-testable blankets every widget beneath it at this
// ZOrder even when it returns Unhandled - Slate routes to the topmost hit-testable widget and
// bubbles up its PARENTS, never down to other layers. That is what killed the HUD's top bar on
// 2026-09-10, and SurfTuningHUD.cpp records the same mistake from an earlier occurrence.

#include "TouchControlsOverlay.h"
#include "SurfLog.h"
#include "SurfDebug.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"

#include "Widgets/SLeafWidget.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/SlateUser.h"
#include "Fonts/FontMeasure.h"
#include "Engine/Texture2D.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	// Everything here is a FRACTION of the viewport, never a pixel count. This overlay applies its
	// own 2x DPI scaler on Android while the level's UMG HUD goes through the project's DPI curve,
	// so the two live in different logical spaces and a pixel constant tuned against one renders at
	// a different physical size than the same constant in the other.
	//
	// Sizes are fractions of screen HEIGHT (the short edge in landscape, so a control keeps its
	// physical size whatever the aspect ratio); positions are fractions of their own axis.
	// Fallbacks only - the live values come from the tuning subsystem.
	constexpr float kStickOuterRadiusFrac = 0.25f;
	constexpr float kPumpRadiusFrac       = 0.25f;
	/** Knob radius as a fraction of the outer ring, so the two scale together. */
	constexpr float kStickInnerRadiusOfOuter = 0.38f;
	/** Pump glyph half-size as a fraction of the pump ring's radius: fills the ring with a margin,
	 *  so the art reads as *inside* a button rather than as the button. */
	constexpr float kPumpIconRadiusOfOuter = 0.62f;

	constexpr float kStickHomeXFrac = 0.12f;   // centre, in from its own edge
	constexpr float kPumpHomeXFrac  = 0.12f;
	/** Fallback height for both centres. On the centreline, because that is where thumbs sit on a
	 *  phone held in landscape - they wrap around the edges near the middle, not down in the bottom
	 *  corners a desktop layout suggests. Live value comes from the tuning subsystem. */
	constexpr float kHomeYFrac      = 0.50f;

	// Input zones, as anchor rects (left, top, right, bottom in screen fractions). Far larger than
	// the drawn controls - a thumb landing anywhere in the region works, which is the whole point on
	// a phone - but no longer the entire screen, so the top bar and the middle stay clickable.
	const FMargin kStickZone(0.00f, 0.18f, 0.45f, 1.00f);
	const FMargin kPumpZone (0.62f, 0.18f, 1.00f, 1.00f);

	// The stick wears the ride HUD's ghost tier (RideHudOverlay's Back pill, the tutorial's ‹ BACK):
	// translucent white fill with a keyline, so the instrument the player holds all ride reads as an
	// OBJECT, not two hairlines - and as the same family as everything else on the ride screen.
	// Idle/held differ only in weight. The knob is solid: it is the thing under the thumb.
	const FLinearColor kGhostFill    (1.0f, 1.0f, 1.0f, 0.18f);
	// Held/pressed fill is a clear step up: in the ghost tier the rim and fill are the ONLY press
	// feedback (the glyph is under the thumb), and 0.18 -> 0.26 did not register over foam.
	const FLinearColor kGhostFillLive(1.0f, 1.0f, 1.0f, 0.40f);
	const FLinearColor kGhostLine    (1.0f, 1.0f, 1.0f, 0.60f);
	const FLinearColor kGhostLineLive(1.0f, 1.0f, 1.0f, 0.85f);
	const FLinearColor kKnob         (1.0f, 1.0f, 1.0f, 0.70f);
	const FLinearColor kKnobLive     (1.0f, 1.0f, 1.0f, 0.95f);

	// The pump button wears SURF AGAIN's colours (WipeoutPanel.cpp): coral fill, dark ink, darker
	// coral when pressed. It is the ride's one "go" control, the same role START and SURF AGAIN
	// play on their screens, so it shares their identity. The idle fill is slightly translucent
	// where the pill is not - a disc this size sits over the wave the player is reading.
	const FLinearColor kPumpFill     (FLinearColor(FColor(0xFF, 0x6B, 0x4A)).CopyWithNewOpacity(0.88f));
	const FLinearColor kPumpFillPress(FColor(0xD9, 0x55, 0x38));
	const FLinearColor kPumpInk      (FColor(0x2A, 0x0D, 0x05));
	// Text-fallback ring colours, used only if T_PumpIcon is missing.
	const FLinearColor kIdle (1.0f, 1.0f, 1.0f, 0.45f);
	const FLinearColor kLive (1.0f, 1.0f, 1.0f, 0.80f);
	const FLinearColor kPumpIdle(1.0f, 0.45f, 0.32f, 0.70f);
	const FLinearColor kPumpLive(1.0f, 0.55f, 0.40f, 1.00f);

	/** Screenshot aid: draw the pump button in its pressed look without a finger on it. Display
	 *  only - it never feeds the pawn. Desktop has no touch to hold through a capture. */
	TAutoConsoleVariable<int32> CVarPumpDrawHeld(
		TEXT("surf.touch.drawpumpheld"), 0,
		TEXT("1 = paint the pump button as pressed (visual only, for screenshots)."));

	/** A white disc: a rounded box in HalfHeightRadius mode (the colour-only constructor's default)
	 *  is a circle at whatever square it is drawn into. A fixed radius larger than the box does NOT
	 *  clamp - it draws nothing. White so the draw call tints it. */
	const FSlateBrush* PumpDiscBrush()
	{
		static const FSlateRoundedBoxBrush B(FLinearColor::White);
		return &B;
	}

	/** Pump button art: a white-on-transparent glyph tinted with the ink colour at draw time, so
	 *  idle/held styling stays in one place. Loaded once; nullptr (-> the text fallback) if the
	 *  texture has not been imported. Deliberately not shared with StartTutorialOverlay's loader -
	 *  same-named free functions in two anonymous namespaces break the Android unity build. */
	const FSlateBrush* PumpIconBrush()
	{
		static bool bTried = false;
		static TSharedPtr<FSlateBrush> Brush;
		static TStrongObjectPtr<UTexture2D> Keep;
		if (!bTried)
		{
			bTried = true;
			if (UTexture2D* Tex = LoadObject<UTexture2D>(nullptr,
				TEXT("/Game/Images/T_PumpIcon.T_PumpIcon"), nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				Keep.Reset(Tex);
				Brush = MakeShared<FSlateBrush>();
				Brush->SetResourceObject(Tex);
				Brush->ImageSize = FVector2D(Tex->GetSizeX(), Tex->GetSizeY());
				Brush->DrawAs = ESlateBrushDrawType::Image;
			}
		}
		return Brush.Get();
	}

	/** Unit circle, cached once, scaled per draw. Slate has no circle primitive; a line loop is the
	 *  cheapest thing that reads as one at these sizes. */
	const TArray<FVector2D>& UnitCircle()
	{
		static TArray<FVector2D> Points;
		if (Points.Num() == 0)
		{
			const int32 Segments = 28;
			Points.Reserve(Segments + 1);
			for (int32 i = 0; i <= Segments; ++i)
			{
				const float T = (2.0f * PI * i) / Segments;
				Points.Add(FVector2D(FMath::Cos(T), FMath::Sin(T)));
			}
		}
		return Points;
	}

	/** Mirror an anchor rect across the screen's vertical centre, for left-handed players. */
	FMargin MirroredX(const FMargin& In)
	{
		return FMargin(1.0f - In.Right, In.Top, 1.0f - In.Left, In.Bottom);
	}

	/** Is this touch pointer on the glass right now, and where? Reads Slate's per-user pointer table,
	 *  which gains a touch on touch-start and loses it on touch-end whatever any widget heard.
	 *  FSlateUser::IsTouchPointerActive is the same test but protected; GetPointerPosition reads the
	 *  same table and answers exactly (0,0) for a pointer it does not list. The one ambiguity - a
	 *  finger on the top-left pixel - is outside both pads, so it can neither be adopted nor matter. */
	bool TouchPointerDown(const FSlateUser& User, int32 PointerIndex, FVector2D& OutAbsPos)
	{
		if (PointerIndex < 0 || PointerIndex >= (int32)ETouchIndex::CursorPointerIndex)
		{
			return false;
		}
		OutAbsPos = User.GetPointerPosition(PointerIndex);
		return !OutAbsPos.IsZero();
	}
}

/** Draws both controls and owns their state. Never takes input - the pads below do that. */
class STouchControlsCanvas : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(STouchControlsCanvas) {}
	SLATE_END_ARGS()

	void Construct(const FArguments&)
	{
		// The one line that keeps this overlay from eating the game's UI.
		SetVisibility(EVisibility::HitTestInvisible);
		bCanSupportFocus = false;
	}

	void SetTuning(const TouchControls::FTuning& In) { Tuning = In; }

	void SetActive(bool bIn) { bActive = bIn; }
	bool IsActive() const { return bActive; }

	/** See TouchControls::SetDimmed. The instrument drops to kDimArt, the captions hold at
	 *  kDimText. */
	void SetDimmed(bool bIn) { bDimmed = bIn; }

	/** See TouchControls::SetInert. Drawn at rest, deaf to the pads. */
	void SetInert(bool bIn)
	{
		if (bIn && !bInert)
		{
			bStickHeld    = false;
			bStickDragged = false;
			bPumpHeld     = false;
			Offset        = FVector2D::ZeroVector;
		}
		bInert = bIn;
	}

	TouchControls::FState GetState() const
	{
		TouchControls::FState S;
		S.WeightOffset = Offset;
		S.bStickHeld   = bStickHeld;
		S.bPumpHeld    = bPumpHeld;
		S.bStickLatched = !bStickHeld && !Offset.IsZero();
		return S;
	}

	// ---- Called by the pads, in ABSOLUTE (screen) space ---------------------------------------

	void BeginStick(const FVector2D& AbsPos)
	{
		if (bInert) { return; }
		bStickHeld      = true;
		bStickDragged   = false;
		StickDownAbs    = AbsPos;
		StickOriginAbs  = AbsPos;   // command zero; drifts with the fore/aft self-centring
		StickAnchorAbs  = AbsPos;   // where the ring is drawn; holds still while held
		StickCurrentAbs = AbsPos;

		// Picking up a latched command: the origin is placed so the thumb lands AT the current
		// offset rather than at zero, so touching the stick does not drop the lean or the trim
		// and a drag adjusts them from where they are. This inverts RecomputeOffset's deadzone
		// rescale (magnitude only - direction is unchanged) and its screen-Y flip.
		const FVector2D Latched = Tuning.bLatch ? Offset : FVector2D::ZeroVector;
		const float Mag = Latched.Size();
		if (Mag > 0.0f)
		{
			const float Full   = FMath::Max(1.0f, Tuning.FullDeflectionPx);
			const float RawMag = Tuning.DeadzoneFrac + Mag * (1.0f - Tuning.DeadzoneFrac);
			const FVector2D Dir(Latched.X / Mag, -Latched.Y / Mag);   // back to screen space
			StickOriginAbs = AbsPos - Dir * RawMag * Full;
		}
		Offset = Latched;
	}

	void MoveStick(const FVector2D& AbsPos)
	{
		if (!bStickHeld || bInert) { return; }
		StickCurrentAbs = AbsPos;
		// A tap is a touch that never travelled further than the deadzone from where it landed.
		// Measured against the touch-down point, not the (possibly shifted) origin, so picking up
		// a latched lean and letting go again still counts as a tap.
		if (!bStickDragged &&
			FVector2D::Distance(AbsPos, StickDownAbs) > Tuning.DeadzoneFrac * FMath::Max(1.0f, Tuning.FullDeflectionPx))
		{
			bStickDragged = true;
		}
		RecomputeOffset();
	}

	void EndStick()
	{
		if (bInert) { return; }   // already cleared on entry; a late finger-up must not re-latch
		bStickHeld = false;
		// Latch: a drag leaves the command where the thumb left it - lean AND trim - so a turn
		// carries on and a lifted nose stays lifted after the thumb lifts; a tap (no drag) clears
		// both. Without the latch everything releases and the pawn's auto-centre takes it.
		// (Sideways-only latching was tried first, 2026-09-14: a stick whose two axes come back
		// on different rules felt like two instruments; the player asked for one.)
		if (!(Tuning.bLatch && bStickDragged))
		{
			Offset = FVector2D::ZeroVector;
		}
	}

	void SetPumpHeld(bool bHeld) { if (!bInert) { bPumpHeld = bHeld; } }

	/** Fore/aft self-centring (StickForeAftReturnSeconds > 0 only; the default is now 0 = hold):
	 *  the command origin creeps toward the thumb's current Y, so a push forward or back is a
	 *  nudge that decays rather than a trim that is held. Sideways is never drifted. */
	void Advance(float DeltaTime)
	{
		if (!bStickHeld || DeltaTime <= 0.0f || Tuning.ForeAftReturnSecs <= 0.0f)
		{
			return;
		}
		const float Alpha = 1.0f - FMath::Exp(-DeltaTime / Tuning.ForeAftReturnSecs);
		StickOriginAbs.Y = FMath::Lerp(StickOriginAbs.Y, StickCurrentAbs.Y, Alpha);
		RecomputeOffset();
	}

	// ---- Slate ------------------------------------------------------------------------------

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(100.0f, 100.0f); }

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Clip,
		FSlateWindowElementList& Out, int32 LayerId, const FWidgetStyle& Style,
		bool bParentEnabled) const override
	{
		// Not live - during the intro, say - is still drawn: the pads below keep tracking fingers so
		// a touch held across the handoff works the instant control arrives, and the owner dims it
		// (SetDimmed) so what is on screen reads as "not yet" rather than as a control that ignores
		// you. It used to draw nothing here, which meant the controls first appeared at the very
		// moment the player was expected to use them (specs/two-screen-navigation.md FR1a). Inert
		// is the same picture at rest, deaf to the pads, under the end card.
		//
		// The dim is TWO numbers, not one. Fading the whole thing evenly to 0.5 was not a signal at
		// all - "at least to me it isn't obvious" (owner, 2026-09-22) - because a ghost control at
		// half strength still looks like a ghost control. So the INSTRUMENT drops far enough to read
		// as a diagram of itself, while the CAPTIONS stay bright: on the hub the words are the whole
		// point, and dimming them would trade the one thing the screen is there to say for a signal
		// about a control that is not being used yet. What changes at the handoff is therefore the
		// rings and the glyph coming up solid, with the words steady across the cut.
		//
		// Both levels are tunables (TouchControlsRestArt / ...RestText), because how obvious "not
		// yet" reads is a judgement made on the glass over moving water. A custom OnPaint applies
		// opacity by hand: every colour below goes through Tint (art) or TintText (words), and the
		// widget style's own tint multiplies both so a parent can still fade the lot.
		const float StyleA = Style.GetColorAndOpacityTint().A;
		const float ArtA   = StyleA * (bDimmed ? FMath::Clamp(Tuning.RestArt,  0.0f, 1.0f) : 1.0f);
		const float TextA  = StyleA * (bDimmed ? FMath::Clamp(Tuning.RestText, 0.0f, 1.0f) : 1.0f);
		const auto Tint     = [ArtA ](FLinearColor C) { C.A *= ArtA;  return C; };
		const auto TintText = [TextA](FLinearColor C) { C.A *= TextA; return C; };

		const FVector2D Size = Geometry.GetLocalSize();
		const float OuterRadius = Size.Y * StickRadiusFracEff();

		// Always drawn, even untouched: an invisible control is discoverable only by accident, and
		// the game should say how it is played without being told. When a thumb lands the ring jumps
		// to it (floating origin), so the resting position is an advertisement, not the pivot.
		const FVector2D Centre = bStickHeld
			? Geometry.AbsoluteToLocal(StickAnchorAbs)
			: StickHomeFor(Size);

		// Offset.Y is positive FORWARD while screen Y grows downward, so it flips back here; and the
		// knob is scaled by the drawn radius rather than by FullDeflectionPx, which sets how much
		// travel earns full deflection and has no business deciding how far a ring is drawn.
		// Drawn from Offset whether or not a thumb is down: a latched lean shows as the knob
		// sitting off-centre in the home ring, so a held turn is never invisible state.
		const FVector2D Knob = Centre + FVector2D(Offset.X, -Offset.Y) * OuterRadius;
		const bool bStickLive = bStickHeld || !Offset.IsZero();

		DrawDisc(Out, LayerId, Geometry, Centre, OuterRadius, Tint(bStickHeld ? kGhostFillLive : kGhostFill));
		DrawCircle(Out, LayerId + 1, Geometry, Centre, OuterRadius, Tint(bStickHeld ? kGhostLineLive : kGhostLine), 2.0f);
		DrawDisc(Out, LayerId + 1, Geometry, Knob, OuterRadius * kStickInnerRadiusOfOuter,
			Tint(bStickLive ? kKnobLive : kKnob));

		// The stick's caption, the pump button's twin: same type, same colour, same distance under
		// the ring. Anchored to the HOME position, not the floating ring - it labels the zone, and
		// a label that chased the thumb would sit under the hand that is using it. Words come from
		// the tutorial ("lean it onto its edge", "lift the nose") so the two never disagree. It
		// says BACK, not forward: WeightMaxInFront clamps forward weight (0.6 since 2026-09-14, 0.45
		// before), so forward mostly levels the board off - the axis the player can feel is tail
		// weight lifting the nose.
		if (Tuning.Hint != 0)
		{
			const FSlateFontInfo HintFont = FCoreStyle::GetDefaultFontStyle("Bold",
				FMath::Max(8, FMath::RoundToInt(Size.Y * 0.030f)));
			const float LineH = FSlateApplication::Get().GetRenderer()->GetFontMeasureService()
				->Measure(TEXT("X"), HintFont).Y;
			const TCHAR* const Lines[2] = { TEXT("SIDEWAYS TO TURN"), TEXT("BACK TO LIFT THE NOSE") };
			const FVector2D Home = StickHomeFor(Size);
			DrawCaption(Out, LayerId + 1, Geometry, Home.X, Home.Y + OuterRadius + LineH * 0.4f,
				Lines, HintFont, TintText(FLinearColor(1.0f, 1.0f, 1.0f, 0.92f)), /*bShadow*/ true, TextA);
		}

		// Pump button: opposite side, same size, same height. A solid disc with the glyph
		// (T_PumpIcon) on it. Geometry is in screen fractions, so both are boxes centred on the
		// same point, the glyph a fraction of the disc's diameter. Falls back to the old
		// ring-and-word style if the texture is missing.
		const FVector2D PumpCentre = PumpHomeFor(Size);
		const float PumpRadius = Size.Y * PumpRadiusFracEff();
		const bool bPumpDown = bPumpHeld || CVarPumpDrawHeld.GetValueOnGameThread() != 0;

		if (const FSlateBrush* Icon = PumpIconBrush())
		{
			// Two looks, switchable on the phone (TouchPumpStyle): coral - SURF AGAIN's colours, the
			// button as the ride's one accent; or ghost - the stick's own tier, so the two controls
			// carry equal visual weight and only the glyph says which is which. Which of the two
			// reads right depends on whether the pump feels like "the go button" or "the other
			// control", and that is a judgement made with a thumb, not on a monitor.
			const bool bGhost = Tuning.bPumpGhost;
			const FLinearColor Fill = Tint(bGhost ? (bPumpDown ? kGhostFillLive : kGhostFill)
			                                      : (bPumpDown ? kPumpFillPress : kPumpFill));
			// Ghost: the glyph is full white - the two-pose art carries a 50% ghost figure, and at
			// the knob's 0.70 that figure fell to ~0.2 and vanished over foam. Contrast against
			// bright water comes from a shadow pass under it (below), not from dimming the ink.
			const FLinearColor Ink  = Tint(bGhost ? FLinearColor(1.0f, 1.0f, 1.0f, bPumpDown ? 1.0f : 0.95f) : kPumpInk);
			DrawDisc(Out, LayerId, Geometry, PumpCentre, PumpRadius, Fill);
			if (bGhost)
			{
				DrawCircle(Out, LayerId + 1, Geometry, PumpCentre, PumpRadius, Tint(bPumpDown ? kGhostLineLive : kGhostLine), 2.0f);
			}

			// Pressed: only the fill darkens, like the pill's pressed state. The glyph does NOT
			// change - the thumb is covering it, so nothing drawn there can be seen; and the one
			// time it did (an 8% swell) the raised hand of the standing pose, which sits in the
			// art's top-left corner, poked out of the disc. The rim is the feedback surface.
			// The hint's placement is a tunable (TouchControlsHint) so it can be flipped on the phone:
			// 1 = inside, where the glyph shrinks and rides higher to leave the lower cap free;
			// 2 = below the disc; 3 = the glyph as the button's background, large and faded, with
			// the text centred in front of it.
			const int32 Hint = Tuning.Hint;
			// Below-the-disc leaves the whole disc to the figures, so they get more of it. The art
			// is a wide box in a round button: its corners leave the circle long before its edges
			// do, and the current drawing has ink 1.19x the half-width from centre (that raised
			// hand). 0.72 puts it at 0.86 R - inside with a visible margin.
			const float IconFrac = (Hint == 1) ? kPumpIconRadiusOfOuter * 0.80f
			                     : (Hint == 2) ? 0.72f
			                     : (Hint == 3) ? 0.86f
			                     :               kPumpIconRadiusOfOuter;
			const float IconLift = (Hint == 1) ? PumpRadius * 0.20f : 0.0f;
			// The art is fitted inside a square of this half-size, keeping its own aspect - the
			// two-pose drawing is wider than tall, and a square box would stretch it.
			const float Half = PumpRadius * IconFrac;
			const float Aspect = (Icon->ImageSize.Y > 0.0f) ? (float)(Icon->ImageSize.X / Icon->ImageSize.Y) : 1.0f;
			const FVector2D IconSize = (Aspect >= 1.0f)
				? FVector2D(Half * 2.0f, Half * 2.0f / Aspect)
				: FVector2D(Half * 2.0f * Aspect, Half * 2.0f);
			const FVector2D IconPos = PumpCentre - IconSize * 0.5f - FVector2D(0.0f, IconLift);
			if (bGhost)
			{
				// Shadow pass: the same glyph, offset like the captions' shadow, so white art keeps
				// an edge over white water. Dark ink would not need it - and does not get it.
				FSlateDrawElement::MakeBox(Out, LayerId + 1,
					Geometry.ToPaintGeometry(FVector2f((float)IconSize.X, (float)IconSize.Y),
						FSlateLayoutTransform(FVector2f((float)IconPos.X + 1.5f, (float)IconPos.Y + 2.5f))),
					Icon, ESlateDrawEffect::None, Tint(FLinearColor(0.0f, 0.0f, 0.0f, (Hint == 3) ? 0.12f : 0.40f)));
			}
			FSlateDrawElement::MakeBox(Out, LayerId + 1,
				Geometry.ToPaintGeometry(FVector2f((float)IconSize.X, (float)IconSize.Y),
					FSlateLayoutTransform(FVector2f((float)IconPos.X, (float)IconPos.Y))),
				Icon, ESlateDrawEffect::None, (Hint == 3) ? Ink.CopyWithNewOpacity(0.30f) : Ink);

			if (Hint != 0)
			{
				// Inside: ink on the fill, in the lower cap. Below: white with a shadow, like the
				// rest of the ride HUD, hanging under the disc.
				const FSlateFontInfo HintFont = FCoreStyle::GetDefaultFontStyle("Bold",
					FMath::Max(8, FMath::RoundToInt(Size.Y * ((Hint == 3) ? 0.036f : 0.030f))));
				const float LineH = FSlateApplication::Get().GetRenderer()->GetFontMeasureService()
					->Measure(TEXT("X"), HintFont).Y;
				const TCHAR* const Lines[2] = { TEXT("PRESS & RELEASE"), TEXT("TO SPEED UP") };
				const float Y = (Hint == 1) ? PumpCentre.Y + PumpRadius * 0.28f
				              : (Hint == 3) ? PumpCentre.Y - LineH
				              :               PumpCentre.Y + PumpRadius + LineH * 0.4f;
				// Hint 2 (the default) hangs the words under the disc in the HUD's white-on-water
				// style, so they are the caption the dim leaves alone. Hints 1 and 3 print them ON
				// the button in its ink, where they are part of the instrument and dim with it.
				DrawCaption(Out, LayerId + 1, Geometry, PumpCentre.X, Y, Lines, HintFont,
					(Hint == 2) ? TintText(FLinearColor(1.0f, 1.0f, 1.0f, 0.92f)) : Ink,
					/*bShadow*/ Hint == 2 || bGhost, (Hint == 2) ? TextA : ArtA);
			}
			return LayerId + 3;
		}

		const FLinearColor PumpColour = Tint(bPumpDown ? kPumpLive : kPumpIdle);
		DrawCircle(Out, LayerId, Geometry, PumpCentre, PumpRadius, PumpColour, bPumpDown ? 6.0f : 4.0f);
		DrawCircle(Out, LayerId, Geometry, PumpCentre, PumpRadius * 0.78f, PumpColour, 2.0f);

		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold",
			FMath::Max(8, FMath::RoundToInt(Size.Y * 0.045f)));
		const FString Label(TEXT("PUMP"));
		const TSharedRef<FSlateFontMeasure> Measure =
			FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		const FVector2D TextSize = Measure->Measure(Label, Font);
		const FVector2D TextPos  = PumpCentre - TextSize * 0.5f;
		FSlateDrawElement::MakeText(Out, LayerId + 1,
			Geometry.ToPaintGeometry(FVector2f((float)TextSize.X, (float)TextSize.Y),
				FSlateLayoutTransform(FVector2f((float)TextPos.X, (float)TextPos.Y))),
			Label, Font, ESlateDrawEffect::None, PumpColour);

		return LayerId + 2;
	}

private:
	TouchControls::FTuning Tuning;

	bool bActive    = false;
	bool bInert     = false;      // ride over: drawn at rest, deaf to the pads (SetInert)
	bool bDimmed    = false;      // not yet live: instrument faded, captions kept (SetDimmed)
	bool bStickHeld = false;
	bool bStickDragged = false;   // moved past the deadzone since touch-down; distinguishes a tap
	bool bPumpHeld  = false;
	FVector2D StickDownAbs    = FVector2D::ZeroVector;
	FVector2D StickOriginAbs  = FVector2D::ZeroVector;
	FVector2D StickAnchorAbs  = FVector2D::ZeroVector;
	FVector2D StickCurrentAbs = FVector2D::ZeroVector;
	FVector2D Offset = FVector2D::ZeroVector;

	float StickRadiusFracEff() const { return Tuning.StickRadiusFrac > 0.0f ? Tuning.StickRadiusFrac : kStickOuterRadiusFrac; }
	float PumpRadiusFracEff()  const { return Tuning.PumpRadiusFrac  > 0.0f ? Tuning.PumpRadiusFrac  : kPumpRadiusFrac; }
	float HomeYFracEff()       const { return Tuning.HomeYFrac       > 0.0f ? Tuning.HomeYFrac       : kHomeYFrac; }

	FVector2D StickHomeFor(const FVector2D& Size) const
	{
		const float Inset = Size.X * kStickHomeXFrac;
		const float X = Tuning.bMirror ? (Size.X - Inset) : Inset;
		return FVector2D(X, Size.Y * HomeYFracEff());
	}

	FVector2D PumpHomeFor(const FVector2D& Size) const
	{
		const float Inset = Size.X * kPumpHomeXFrac;
		const float X = Tuning.bMirror ? Inset : (Size.X - Inset);
		return FVector2D(X, Size.Y * HomeYFracEff());
	}

	/** Positions arrive in absolute space from the pads; deflection has to be measured in THIS
	 *  widget's local space, because FullDeflectionPx is expressed there. */
	void RecomputeOffset()
	{
		const FGeometry& G = GetTickSpaceGeometry();
		const FVector2D Origin  = G.AbsoluteToLocal(StickOriginAbs);
		const FVector2D Current = G.AbsoluteToLocal(StickCurrentAbs);

		const float Full = FMath::Max(1.0f, Tuning.FullDeflectionPx);
		FVector2D Raw = (Current - Origin) / Full;
		Raw.Y = -Raw.Y;   // screen Y grows downward; fore/aft is positive forward

		const float Mag = Raw.Size();
		if (Mag <= Tuning.DeadzoneFrac)
		{
			Offset = FVector2D::ZeroVector;
			return;
		}
		// Rescale past the deadzone so the first live movement is not a step.
		const float Scaled = FMath::Min((Mag - Tuning.DeadzoneFrac) / (1.0f - Tuning.DeadzoneFrac), 1.0f);
		Offset = Raw / Mag * Scaled;
	}

	/** Two-line caption, centred on X, top edge at Y. bShadow = the HUD's white-on-water style
	 *  (offset shadow under white text); otherwise flat ink for text on a solid fill. Uses Layer
	 *  and Layer + 1. */
	static void DrawCaption(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry,
		float CentreX, float Y, const TCHAR* const Lines[2], const FSlateFontInfo& Font,
		const FLinearColor& Colour, bool bShadow, float ShadowOpacity = 1.0f)
	{
		const TSharedRef<FSlateFontMeasure> Measure =
			FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
		for (int32 i = 0; i < 2; ++i)
		{
			const FVector2D TS = Measure->Measure(Lines[i], Font);
			const FVector2D TP(CentreX - TS.X * 0.5f, Y);
			if (bShadow)
			{
				FSlateDrawElement::MakeText(Out, Layer,
					Geometry.ToPaintGeometry(FVector2f((float)TS.X, (float)TS.Y),
						FSlateLayoutTransform(FVector2f((float)TP.X + 1.0f, (float)TP.Y + 1.0f))),
					Lines[i], Font, ESlateDrawEffect::None, FLinearColor(0.0f, 0.0f, 0.0f, 0.45f * ShadowOpacity));
			}
			FSlateDrawElement::MakeText(Out, Layer + 1,
				Geometry.ToPaintGeometry(FVector2f((float)TS.X, (float)TS.Y),
					FSlateLayoutTransform(FVector2f((float)TP.X, (float)TP.Y))),
				Lines[i], Font, ESlateDrawEffect::None, Colour);
			Y += TS.Y;
		}
	}

	/** Filled disc: the white rounded-box brush, tinted. */
	static void DrawDisc(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry,
		const FVector2D& Centre, float Radius, const FLinearColor& Colour)
	{
		const FVector2D Size(Radius * 2.0f, Radius * 2.0f);
		const FVector2D Pos = Centre - Size * 0.5f;
		FSlateDrawElement::MakeBox(Out, Layer,
			Geometry.ToPaintGeometry(FVector2f((float)Size.X, (float)Size.Y),
				FSlateLayoutTransform(FVector2f((float)Pos.X, (float)Pos.Y))),
			PumpDiscBrush(), ESlateDrawEffect::None, Colour);
	}

	static void DrawCircle(FSlateWindowElementList& Out, int32 Layer, const FGeometry& Geometry,
		const FVector2D& Centre, float Radius, const FLinearColor& Colour, float Thickness)
	{
		const TArray<FVector2D>& Unit = UnitCircle();
		TArray<FVector2D> Points;
		Points.Reserve(Unit.Num());
		for (const FVector2D& U : Unit)
		{
			Points.Add(Centre + U * Radius);
		}
		FSlateDrawElement::MakeLines(Out, Layer, Geometry.ToPaintGeometry(), Points,
			ESlateDrawEffect::None, Colour, true, Thickness);
	}
};

/** An input-only rectangle: hit-testable, draws nothing, and covers exactly one control's zone - so
 *  everything outside the two zones reaches whatever is underneath. */
class STouchPad : public SLeafWidget
{
public:
	enum class EKind : uint8 { Stick, Pump };

	SLATE_BEGIN_ARGS(STouchPad) {}
		SLATE_ARGUMENT(TWeakPtr<STouchControlsCanvas>, Owner)
		SLATE_ARGUMENT(EKind, Kind)
	SLATE_END_ARGS()

	STouchPad() : Kind(EKind::Stick), Finger(INDEX_NONE), FingerUser(INDEX_NONE) {}

	void Construct(const FArguments& Args)
	{
		Owner = Args._Owner;
		Kind  = Args._Kind;
		SetVisibility(EVisibility::Visible);   // this one exists to take touches
		bCanSupportFocus = false;
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1.0f, 1.0f); }

	bool IsHolding() const { return Finger != INDEX_NONE; }
	bool Holds(int32 UserIndex, int32 PointerIndex) const { return Finger == PointerIndex && FingerUser == UserIndex; }

	/** Is this screen-space point inside the pad? False until the pad has been laid out once. */
	bool Covers(const FVector2D& AbsPos) const
	{
		const FGeometry& G = GetTickSpaceGeometry();
		return G.GetLocalSize().X > 0.0f && G.GetLocalSize().Y > 0.0f && G.IsUnderLocation(AbsPos);
	}

	/** Take a finger that is ALREADY down, without a touch-started event. See PollFingers() for why
	 *  that happens and why it cannot be left to the events. */
	void Adopt(int32 UserIndex, int32 PointerIndex, const FVector2D& AbsPos)
	{
		TSharedPtr<STouchControlsCanvas> Canvas = Owner.Pin();
		if (!Canvas.IsValid() || Finger != INDEX_NONE)
		{
			return;
		}
		Finger     = PointerIndex;
		FingerUser = UserIndex;

		if (SurfDebug::IsFlagSet(TEXT("touch")))
		{
			UE_LOG(LogSurf, Warning, TEXT("TouchControls: ADOPT kind=%s finger=%d abs=(%.0f,%.0f) - was down before the pad could see it"),
				Kind == EKind::Stick ? TEXT("stick") : TEXT("pump"), Finger, AbsPos.X, AbsPos.Y);
		}

		// The thumb's position NOW is zero, exactly as if it had just landed: it may have been
		// resting there since before the start screen closed, and that distance is not a command.
		if (Kind == EKind::Stick) { Canvas->BeginStick(AbsPos); }
		else                      { Canvas->SetPumpHeld(true); }
	}

	/** Once per game tick: keep a held TOUCH finger honest against Slate's own pointer table.
	 *
	 *  Position comes from the table rather than from OnTouchMoved because an adopted finger has no
	 *  capture - nothing replied Handled to its touch-down - so its moves route by hit test and
	 *  stop arriving the moment the thumb wanders off the pad. And a finger the table no longer
	 *  lists has lifted, whatever this pad heard about it: Slate drops a touch pointer on every
	 *  touch-end, so a lost OnTouchEnded can never leave the board steering on its own. Neither
	 *  applies to the PIE mouse (the cursor pointer index), which keeps the event path. */
	void PollHeld()
	{
		if (Finger == INDEX_NONE || Finger >= (int32)ETouchIndex::CursorPointerIndex)
		{
			return;
		}
		TSharedPtr<FSlateUser> User = FSlateApplication::Get().GetUser(FingerUser);
		FVector2D AbsPos;
		if (User.IsValid() && TouchPointerDown(*User, Finger, AbsPos))
		{
			if (Kind == EKind::Stick)
			{
				if (TSharedPtr<STouchControlsCanvas> Canvas = Owner.Pin())
				{
					Canvas->MoveStick(AbsPos);
				}
			}
			return;
		}

		if (SurfDebug::IsFlagSet(TEXT("touch")))
		{
			UE_LOG(LogSurf, Warning, TEXT("TouchControls: LOST kind=%s finger=%d - no longer down, released by poll"),
				Kind == EKind::Stick ? TEXT("stick") : TEXT("pump"), Finger);
		}
		Release();
	}

	virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&,
		FSlateWindowElementList&, int32 LayerId, const FWidgetStyle&, bool) const override
	{
		return LayerId;   // input only
	}

	virtual FReply OnTouchStarted(const FGeometry&, const FPointerEvent& Event) override
	{
		TSharedPtr<STouchControlsCanvas> Canvas = Owner.Pin();
		if (!Canvas.IsValid() || Finger != INDEX_NONE)
		{
			return FReply::Unhandled();
		}
		Finger     = Event.GetPointerIndex();
		FingerUser = Event.GetUserIndex();

		if (SurfDebug::IsFlagSet(TEXT("touch")))
		{
			const FVector2D P = Event.GetScreenSpacePosition();
			UE_LOG(LogSurf, Warning, TEXT("TouchControls: DOWN kind=%s finger=%d abs=(%.0f,%.0f)"),
				Kind == EKind::Stick ? TEXT("stick") : TEXT("pump"), Finger, P.X, P.Y);
		}

		if (Kind == EKind::Stick) { Canvas->BeginStick(Event.GetScreenSpacePosition()); }
		else                      { Canvas->SetPumpHeld(true); }

		return FReply::Handled().CaptureMouse(SharedThis(this));
	}

	virtual FReply OnTouchMoved(const FGeometry&, const FPointerEvent& Event) override
	{
		if (Event.GetPointerIndex() != Finger)
		{
			return FReply::Unhandled();
		}
		// A finger keeps the control it started on even if it wanders outside the pad: a thumb
		// sliding off mid-turn must not silently stop steering, nor stop pumping.
		if (Kind == EKind::Stick)
		{
			if (TSharedPtr<STouchControlsCanvas> Canvas = Owner.Pin())
			{
				Canvas->MoveStick(Event.GetScreenSpacePosition());
			}
		}
		return FReply::Handled();
	}

	virtual FReply OnTouchEnded(const FGeometry&, const FPointerEvent& Event) override
	{
		if (Event.GetPointerIndex() != Finger)
		{
			return FReply::Unhandled();
		}
		Release();
		return FReply::Handled().ReleaseMouseCapture();
	}

	// Mouse mirrors touch so the controls can be driven in PIE. A mouse reports the cursor pointer
	// index, and the touch handlers key on the index, so they are reused verbatim.
	virtual FReply OnMouseButtonDown(const FGeometry& G, const FPointerEvent& E) override
	{
		return (E.GetEffectingButton() == EKeys::LeftMouseButton) ? OnTouchStarted(G, E) : FReply::Unhandled();
	}
	virtual FReply OnMouseMove(const FGeometry& G, const FPointerEvent& E) override
	{
		return (Finger != INDEX_NONE) ? OnTouchMoved(G, E) : FReply::Unhandled();
	}
	virtual FReply OnMouseButtonUp(const FGeometry& G, const FPointerEvent& E) override
	{
		return (E.GetEffectingButton() == EKeys::LeftMouseButton) ? OnTouchEnded(G, E) : FReply::Unhandled();
	}

private:
	void Release()
	{
		Finger     = INDEX_NONE;
		FingerUser = INDEX_NONE;
		if (TSharedPtr<STouchControlsCanvas> Canvas = Owner.Pin())
		{
			if (Kind == EKind::Stick) { Canvas->EndStick(); }
			else                      { Canvas->SetPumpHeld(false); }
		}
	}

	TWeakPtr<STouchControlsCanvas> Owner;
	EKind Kind;
	int32 Finger;
	int32 FingerUser;   // Slate user the finger belongs to; pointer indices are per user
};

namespace
{
	struct FTouchState
	{
		TSharedPtr<STouchControlsCanvas> Canvas;
		TSharedPtr<STouchPad> StickPad;
		TSharedPtr<STouchPad> PumpPad;
		TSharedPtr<SWidget> Root;
		bool bMirror = false;   // layout is baked into the anchors, so a change rebuilds
	};

	TMap<TWeakObjectPtr<UWorld>, FTouchState> GStates;

	FTouchState* Find(UWorld* World)
	{
		return World ? GStates.Find(World) : nullptr;
	}

	TSharedRef<SWidget> BuildControls(TSharedPtr<STouchControlsCanvas>& OutPainter,
		TSharedPtr<STouchPad>& OutStickPad, TSharedPtr<STouchPad>& OutPumpPad, bool bMirror)
	{
		const FMargin StickZone = bMirror ? MirroredX(kStickZone) : kStickZone;
		const FMargin PumpZone  = bMirror ? MirroredX(kPumpZone)  : kPumpZone;

		// SelfHitTestInvisible, NOT the default Visible: a full-screen panel that is hit-testable
		// blankets every widget beneath this ZOrder (see the note at the top of this file). It
		// went unnoticed while nothing hit-testable ever sat under the controls; the hub does
		// (ZOrder 200), and its START took no taps until this was set (2026-09-22).
		TSharedRef<SConstraintCanvas> Canvas = SNew(SConstraintCanvas)
			.Visibility(EVisibility::SelfHitTestInvisible);

		// The painter fills the screen but is HitTestInvisible, so it blocks nothing.
		Canvas->AddSlot()
			.Anchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f))
			.Offset(FMargin(0.0f))
			[
				SAssignNew(OutPainter, STouchControlsCanvas)
			];

		Canvas->AddSlot()
			.Anchors(FAnchors(StickZone.Left, StickZone.Top, StickZone.Right, StickZone.Bottom))
			.Offset(FMargin(0.0f))
			[
				SAssignNew(OutStickPad, STouchPad).Owner(OutPainter).Kind(STouchPad::EKind::Stick)
			];

		Canvas->AddSlot()
			.Anchors(FAnchors(PumpZone.Left, PumpZone.Top, PumpZone.Right, PumpZone.Bottom))
			.Offset(FMargin(0.0f))
			[
				SAssignNew(OutPumpPad, STouchPad).Owner(OutPainter).Kind(STouchPad::EKind::Pump)
			];

		return Canvas;
	}

	void InstallInternal(UWorld* World, bool bMirror)
	{
		// Every dimension in the painter is a logical pixel in a 540-high space, the same convention
		// the other overlays use: phone screens are ~2x the density of a desktop monitor, so the
		// device gets a 2x scaler and desktop gets 1x.
#if PLATFORM_ANDROID
		const float Scale = 2.0f;
#else
		const float Scale = 1.0f;
#endif

		TSharedPtr<STouchControlsCanvas> Painter;
		TSharedPtr<STouchPad> StickPad;
		TSharedPtr<STouchPad> PumpPad;
		TSharedRef<SWidget> Inner = BuildControls(Painter, StickPad, PumpPad, bMirror);

		TSharedRef<SWidget> Root = SNew(SDPIScaler).DPIScale(Scale)
			.Visibility(EVisibility::SelfHitTestInvisible)   // the scaler must not eat touches
			[
				Inner
			];

		// ZOrder 250: above the level's UMG HUD, so a UMG root left at the default Visible cannot
		// swallow these. Safe this high only because nothing here is hit-testable outside the two
		// pads - see the note at the top of this file.
		World->GetGameViewport()->AddViewportWidgetContent(Root, /*ZOrder*/ 250);

		FTouchState State;
		State.Canvas   = Painter;
		State.StickPad = StickPad;
		State.PumpPad  = PumpPad;
		State.Root     = Root;
		State.bMirror  = bMirror;
		GStates.Add(World, State);
	}

	/** Once per game tick: reconcile the pads with the fingers that are actually on the glass.
	 *
	 *  The pads normally learn about a finger from its touch-started event. A finger that is ALREADY
	 *  down when a pad appears never sends one - and at ride start that is common: the left thumb is
	 *  resting on the start screen while the right taps Start, or it lands during the long first frame
	 *  after the world unpauses, before the pads have been added to the viewport. Slate then routes
	 *  that finger's moves by hit test and its touch-end by hit test, so the pad hears about the
	 *  finger only when it lifts - which is why the stick used to do nothing until it was released
	 *  and pressed again (about one ride in five, on device).
	 *
	 *  So the pads also POLL: any touch pointer Slate lists as down, that no live widget holds
	 *  captured, and that sits inside a free pad, is adopted as if it had just landed there. Capture
	 *  is the arbiter because it is how Slate says "this finger belongs to that widget": a held HUD
	 *  button keeps its finger. A capture whose widget is gone (the start screen's, after it was
	 *  removed with a thumb still on it) is cleared by GetCaptorPath itself, and the finger is free. */
	void PollFingers(FTouchState& State)
	{
		if (!State.StickPad.IsValid() || !State.PumpPad.IsValid())
		{
			return;
		}
		STouchPad& Stick = *State.StickPad;
		STouchPad& Pump  = *State.PumpPad;

		Stick.PollHeld();
		Pump.PollHeld();

		if (Stick.IsHolding() && Pump.IsHolding())
		{
			return;   // nothing to adopt into
		}

		FSlateApplication::Get().ForEachUser([&](FSlateUser& User)
		{
			for (int32 Idx = 0; Idx < (int32)ETouchIndex::CursorPointerIndex; ++Idx)
			{
				FVector2D P;
				if (!TouchPointerDown(User, Idx, P)
					|| Stick.Holds(User.GetUserIndex(), Idx) || Pump.Holds(User.GetUserIndex(), Idx))
				{
					continue;
				}
				if (User.HasCapture(Idx) && User.GetCaptorPath(Idx).Widgets.Num() > 0)
				{
					continue;   // a live widget owns this finger
				}
				if      (!Stick.IsHolding() && Stick.Covers(P)) { Stick.Adopt(User.GetUserIndex(), Idx, P); }
				else if (!Pump.IsHolding()  && Pump.Covers(P))  { Pump.Adopt(User.GetUserIndex(), Idx, P); }
			}
		});
	}
}

namespace TouchControls
{
	void Install(UWorld* World)
	{
		// Drop entries whose world has gone. A PIE session that ended without EndPlay running leaves
		// one behind, and the next session then installs a SECOND overlay: the orphan keeps painting
		// its idle ring at the home position while the live one follows the thumb, which is what
		// "two joysticks" looked like on 2026-09-10.
		for (auto It = GStates.CreateIterator(); It; ++It)
		{
			if (!It.Key().IsValid())
			{
				It.RemoveCurrent();
			}
		}

		if (!World || !World->GetGameViewport() || Find(World))
		{
			return;
		}
		InstallInternal(World, /*bMirror*/ false);
		UE_LOG(LogSurf, Display, TEXT("TouchControls: installed"));
	}

	void Uninstall(UWorld* World)
	{
		FTouchState* State = Find(World);
		if (!State)
		{
			return;
		}
		if (World && World->GetGameViewport() && State->Root.IsValid())
		{
			World->GetGameViewport()->RemoveViewportWidgetContent(State->Root.ToSharedRef());
		}
		GStates.Remove(World);
		UE_LOG(LogSurf, Display, TEXT("TouchControls: uninstalled"));
	}

	bool IsInstalled(UWorld* World)
	{
		return Find(World) != nullptr;
	}

	void SetTuning(UWorld* World, const FTuning& Tuning)
	{
		FTouchState* State = Find(World);
		if (!State)
		{
			return;
		}

		// Handedness is baked into the pads' anchors, so flipping it rebuilds. Rare enough (a
		// settings toggle) that rebuilding beats keeping mutable slots around.
		if (Tuning.bMirror != State->bMirror)
		{
			if (World && World->GetGameViewport() && State->Root.IsValid())
			{
				World->GetGameViewport()->RemoveViewportWidgetContent(State->Root.ToSharedRef());
			}
			GStates.Remove(World);
			InstallInternal(World, Tuning.bMirror);
			State = Find(World);
			if (!State) { return; }
		}

		if (State->Canvas.IsValid())
		{
			State->Canvas->SetTuning(Tuning);
		}
	}

	void SetActive(UWorld* World, bool bActive)
	{
		if (FTouchState* State = Find(World))
		{
			if (State->Canvas.IsValid())
			{
				State->Canvas->SetActive(bActive);
			}
		}
	}

	void SetInert(UWorld* World, bool bInert)
	{
		if (FTouchState* State = Find(World))
		{
			if (State->Canvas.IsValid())
			{
				State->Canvas->SetInert(bInert);
			}
			// Deaf pads should not be in the way either. On the hub the controls sit ABOVE the
			// start screen (ZOrder 250 over 200), and the pads' zones cover the hub's REPLAY button
			// and the ABOUT pill; a hit-testable pad there would swallow those taps. Under the end
			// card the scrim is above the pads anyway, so this changes nothing there.
			const EVisibility PadVis = bInert ? EVisibility::HitTestInvisible : EVisibility::Visible;
			if (State->StickPad.IsValid()) { State->StickPad->SetVisibility(PadVis); }
			if (State->PumpPad.IsValid())  { State->PumpPad->SetVisibility(PadVis); }
		}
	}

	void SetDimmed(UWorld* World, bool bDimmed)
	{
		if (FTouchState* State = Find(World))
		{
			if (State->Canvas.IsValid())
			{
				State->Canvas->SetDimmed(bDimmed);
			}
		}
	}

	FState GetState(UWorld* World)
	{
		if (FTouchState* State = Find(World))
		{
			// An inactive overlay reports nothing, so a thumb resting on the stick through the intro
			// cannot leak into the weight before the player actually has control.
			if (State->Canvas.IsValid() && State->Canvas->IsActive())
			{
				return State->Canvas->GetState();
			}
		}
		return FState();
	}

	void Advance(UWorld* World, float DeltaTime)
	{
		if (FTouchState* State = Find(World))
		{
			PollFingers(*State);
			if (State->Canvas.IsValid())
			{
				State->Canvas->Advance(DeltaTime);
			}
		}
	}
}
