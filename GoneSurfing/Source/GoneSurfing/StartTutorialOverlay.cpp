// See StartTutorialOverlay.h and specs/start-screen-tutorial.md.

#include "StartTutorialOverlay.h"
#include "SurfLog.h"
#include "SurfTilt.h"
#include "SurfTuningSubsystem.h"
#include "TutorialBoardGlyph.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

#include "Widgets/SCompoundWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"
#include "Engine/Texture2D.h"
#include "UObject/StrongObjectPtr.h"

// ---- palette (sRGB hex -> linear via FLinearColor(FColor)) -----------------------------------
namespace
{
	const FLinearColor C_Night(FColor(0x0A, 0x16, 0x26)); // page ground   #0a1626
	// Night at partial alpha: the scrim the blurred wave shows through. Dark enough to hold C_Foam
	// text over sunlit whitewater, light enough that the wave is still legible as moving water.
	const FLinearColor C_Scrim(0.039f, 0.086f, 0.149f, 0.52f);

	// Every light label on the menu sits over moving water that is brighter than any scrim setting
	// which still lets the wave read, so they all carry a shadow. Cheaper and less heavy-handed than
	// darkening the scrim, which would flatten the background the whole feature exists to show.
	const FVector2D    TextShadow(1.0f, 2.0f);
	const FLinearColor C_TextShadow(0.0f, 0.0f, 0.0f, 0.6f);

	// An extra veil over the tutorial cards only. The start screen is a hero shot and wants the wave
	// bright; the cards are something the player has to read, and a title can land on a sunlit patch
	// where a shadow alone isn't enough. Applied to the whole page so titles, body, counter and dots
	// all gain the same margin, rather than patching each one.
	const FLinearColor C_ScrimSteps(0.039f, 0.086f, 0.149f, 0.30f);

	// The menu background is a pre-recorded loop of the wave, captured by -StartScreenCapture and
	// imported as small textures. It replaced a live blurred scene: Niagara cannot simulate while
	// the world is paused, so the live version could never show white water (see the spec). The
	// frames are stored tiny and stretched to full screen — the upscale *is* the blur, so nothing
	// is blurred at runtime and there is no SBackgroundBlur to be unsupported on a device.
	// Frame assets are /Game/Images/T_StartBg_000 upward — same folder as the instruction images
	// (the path literal lives at the one call site: FString::Printf needs a literal format).
	const int32  BgMaxFrames = 200;
	const float  BgFps = 15.0f;   // half of natural speed (stride 2 at a 60 fps wave = 30) — the
	                              // menu wave reads calmer slowed down
	const FLinearColor C_Panel(FColor(0x12, 0x3B, 0x4D)); // card / slot   #123b4d
	const FLinearColor C_Foam (FColor(0xD6, 0xEE, 0xF2)); // primary text  #d6eef2
	const FLinearColor C_Slate(FColor(0x8F, 0xA9, 0xBD)); // muted text    #8fa9bd
	const FLinearColor C_Coral(FColor(0xFF, 0x6B, 0x4A)); // accent        #ff6b4a
	const FLinearColor C_Gold (FColor(0xFF, 0xC8, 0x57)); // wordmark      #ffc857
	const FLinearColor C_Ink  (FColor(0x2A, 0x0D, 0x05)); // text on coral #2a0d05

	// Matches the in-game Restart / Replay / Back pills in WBP_SurfboardControls: stadium-rounded,
	// no keyline on the filled tiers, hierarchy carried by fill weight alone.
	const FLinearColor C_BtnBlue(FColor(0x12, 0x26, 0xBB));  // #1226bb — keep in sync with WBP_SurfboardControls
	const FLinearColor C_White(FLinearColor::White);
	const FLinearColor C_GhostFill   (1.0f, 1.0f, 1.0f, 0.18f); // translucent scrim for the tertiary tier
	const FLinearColor C_GhostOutline(1.0f, 1.0f, 1.0f, 0.60f); // its keyline — the only outlined tier
	const float        GhostOutline = 2.0f;
	const float        FlipInterval = 0.45f; // seconds per frame of the two-frame illustration flip

	// ---- how the phone is being held (drives the sideways card's two variants) --------------------
	// See "Sideways: two gestures, one card" in the spec. The signal is |g.z|/|g|: device Z is the
	// phone's in-plane vertical axis, so 0 = flat, 1 = upright. Same quantity AdvanceTiltYawFusion
	// uses as YawWeight, deliberately — picture and control law read one number and cannot diverge.
	const float PoseVerticalEnter = 0.60f;  // above this, show the upright-phone card
	const float PoseVerticalExit  = 0.40f;  // below this, back to the flat-phone card
	const float PoseSmoothSeconds = 0.35f;  // low-pass before the hysteresis sees it
	const float PoseMinGravity    = 0.5f;   // |g| below this: no sensor, or a reading still settling

	// Swapping the art and copy on the same frame reads as a glitch, not as a response (device
	// verdict, 2026-08-21). The card dips out and back instead, swapping while invisible, so the
	// change is something the player watches happen. Each way, so a swap costs twice this.
	// A CVar because it is a feel knob, and feel can only be judged on the phone — tune it there
	// rather than round-tripping a rebuild and a deploy per guess.
	TAutoConsoleVariable<float> CVarStartPoseFade(
		TEXT("surf.start.posefade"), 0.18f,
		TEXT("Start-tutorial grip-swap fade, seconds each way (0 = instant swap)."),
		ECVF_Default);

	// Desktop has no gravity, so auto-detect can only ever produce the flat card there. This is the
	// only way to see the other one without a deploy — pair it with -ShowInstructions.
	TAutoConsoleVariable<int32> CVarStartHoldPose(
		TEXT("surf.start.holdpose"), -1,
		TEXT("Start-tutorial sideways card: -1 auto (device gravity), 0 flat phone, 1 upright phone."),
		ECVF_Default);

	// ---- the live board glyph (specs/tutorial-live-feedback.md) -----------------------------------

	/** Below this calibration conditioning the preview axes are meaningless and the glyph hides
	 *  rather than showing a wrong axis. ~|g.z|, so this is roughly "within 15 degrees of flat".
	 *  Hysteresis, like the grip swap, so a phone parked on the line doesn't blink. */
	const float GlyphMinConditioning     = 0.25f;
	const float GlyphMinConditioningExit = 0.18f;
	const float GlyphFadeSeconds         = 0.25f;

	/** Real deflections are small — full tilt is 25 degrees and the deadzone eats the first 2 — so
	 *  a 1:1 glyph would barely twitch. These map offset (-1..1) to drawn degrees. The deadzone is
	 *  NOT compensated for: dead travel before anything happens is part of the lesson (FR5). */
	const float GlyphRollDegAtFull  = 38.0f;
	const float GlyphPitchDegAtFull = 22.0f;

	/** How far the drawn board swings for a given tilt — the *drawing*, not the mapping.
	 *
	 *  This is the tutorial's own knob and changing it teaches nothing false: the underlying signal
	 *  is still the ride's, and this only amplifies it for a board a few centimetres across. The
	 *  mapping itself (how much phone tilt is full deflection) lives on USurfTuningSubsystem as
	 *  TiltRoll/PitchDegreesForFullDeflection, and the preview reads it deliberately — change that
	 *  and both the ride and the tutorial move together, which is the point of FR2. */
	TAutoConsoleVariable<float> CVarStartTiltGain(
		TEXT("surf.start.tiltgain"), 1.0f,
		TEXT("Start-tutorial board: scales how far it swings for a given tilt (drawing only). 1 = default."),
		ECVF_Default);

	/** How fast the pump signal itself is smoothed. Only the surge uses this — attitude is a spring
	 *  (below), because a board is not a slider. */
	const float GlyphFollowHz = 12.0f;

	// ---- the drawn board has inertia ----
	// It used to chase the commanded lean at 12 Hz, i.e. instantly. The real board does not: it has
	// mass and the fork's angular damping, so a weight shift takes real time to become an attitude,
	// and the tutorial was quietly teaching a snappier board than the one being ridden.
	//
	// Modelled as a second-order spring-damper rather than a slower lag, because the character is
	// the point: a board settles into a lean and rights itself, it does not ease exponentially.
	// Applied in the OFFSET domain, so what the player sees is also what success measures.
	//
	// The numbers live on USurfTuningSubsystem (TutorialBoardHz / TutorialBoardDamping) rather than
	// on a CVar, so they can be dialled in with the tuning HUD's sliders while watching the glyph —
	// they can only be set by eye against the real board, so they need to be adjustable by eye.
	// These are the fallbacks for when there is no subsystem.
	const float TutorialBoardHzFallback      = 0.9f;
	const float TutorialBoardDampingFallback = 0.8f;

	// ---- the pump card's board actually flies ----
	// A board that nudges forward is invisible to someone busy pumping a phone, so it launches out
	// of frame instead and comes back round: each pump adds speed, so pumping in rhythm keeps it
	// going, which is exactly what the card claims. Travel is in outline units — past ~1.4 the board
	// has left the widget, which clips.
	// Launch strength and drag live on USurfTuningSubsystem (TutorialPumpLaunch / TutorialPumpDrag)
	// alongside the board's attitude dynamics, so the whole feel of the pump card is dialled in with
	// the same sliders, live, against the thing they change. These are the no-subsystem fallbacks.
	//
	// Launch is large because a pump is a *pulse*, not a hold: only the loading phase registers, so
	// a single deliberate pump contributes maybe a third of a second of input. At the first values a
	// launch took many pumps to build up (device, 2026-08-21); one committed pump should send it.
	const float TutorialPumpLaunchFallback = 14.0f;  // travel-units/s added per second at full pump
	const float TutorialPumpDragFallback   = 0.70f;  // how fast it coasts down with no pump

	/** Runaway stop, not a tuning value — deliberately far above any sensible launch so the slider
	 *  never runs into it and reads as dead. */
	const float GlyphTravelMaxSpeed = 12.0f;

	/** Where it wraps. The board spans x -1..1 and the widget clips at GlyphHalfSpanX (1.15), so it
	 *  is not fully gone until travel exceeds 2.15 — at the old 1.6 its tail was still on screen and
	 *  the wrap read as a teleport rather than as leaving. */
	const float GlyphTravelWrap    = 2.3f;

	/** Below this it has effectively stopped, and drifts back to centred so the resting state is a
	 *  board you can see rather than one parked off the edge. */
	const float GlyphTravelIdleSpeed = 0.12f;

	// ---- the success beat (FR6) ----
	// Earned by demonstrating control of the axis, not by the glyph having moved: the player has to
	// take it past the threshold in BOTH directions. One-directional would be satisfiable by drift,
	// and going both ways is what proves they found the axis rather than stumbled onto it.
	/** How far, as a fraction of full deflection, counts as having gone that way. Too low and a card
	 *  is earned by accident; too high and it feels withheld. Measured on the *offset*, so it is
	 *  independent of surf.start.tiltgain — making the drawing swing further does not make the card
	 *  easier to earn. */
	TAutoConsoleVariable<float> CVarStartSuccessTilt(
		TEXT("surf.start.successtilt"), 0.45f,
		TEXT("Start-tutorial: fraction of full tilt that counts as reaching a direction (0..1)."),
		ECVF_Default);

	/** Pump is earned by the board **leaving the frame** — the thing the player can actually see —
	 *  rather than by an internal speed reading. The first cut asked for speed >= 1.10, which real
	 *  pumping never reached (pumps are short pulses, and only the loading phase counts), so the
	 *  board would fly and the card would still never be earned. The floor here only exists so a
	 *  slow creep to the wrap point doesn't count as a launch. */
	const float SuccessPumpWrapSpeed = 0.30f;

	/** The glyph caption doubles as the coach: it says what is still wanted, then that it landed.
	 *  Plain language, no axis names. */
	FText GlyphCaption(int32 Step, bool bEarned, bool bHalfWay)
	{
		if (bEarned)  return FText::FromString(TEXT("GOT IT"));
		if (bHalfWay) return FText::FromString(TEXT("NOW THE OTHER WAY"));
		switch (Step)
		{
		case 0:  return FText::FromString(TEXT("LEAN IT BOTH WAYS"));
		case 1:  return FText::FromString(TEXT("NOSE UP AND DOWN"));
		default: return FText::FromString(TEXT("PUMP TO LAUNCH IT"));
		}
	}

	// How long the flourish lasts. A CVar because it is a feel knob — and because stretching it is
	// the only way to catch it in a screenshot, the same trick surf.start.posefade allows.
	TAutoConsoleVariable<float> CVarStartSuccessFlash(
		TEXT("surf.start.successflash"), 1.1f,
		TEXT("Start-tutorial success flourish, seconds."),
		ECVF_Default);

	// Desktop has no accelerometer, so the glyph can never move there under its own steam. This
	// drives it directly (-1..1 on the card's own axis) so the layout can be seen without a deploy.
	TAutoConsoleVariable<float> CVarStartFakeResponse(
		TEXT("surf.start.fakeresponse"), 0.0f,
		TEXT("Start-tutorial feedback board: drive it to this offset (-1..1) instead of the sensor. 0 = off."),
		ECVF_Default);

	// Sweeps the fake response back and forth instead of holding it, so the both-directions success
	// condition can be reached without a sensor. A held value only ever satisfies one direction, so
	// without this the tilt cards can never be earned on desktop.
	TAutoConsoleVariable<float> CVarStartFakeSweep(
		TEXT("surf.start.fakesweep"), 0.0f,
		TEXT("Start-tutorial: sweep surf.start.fakeresponse back and forth at this many Hz. 0 = hold."),
		ECVF_Default);

	// The SHOW INSTRUCTIONS door on the start screen. Off by default: steering is a joystick and the
	// pump is a button that says PUMP, so the controls now explain themselves and a tutorial standing
	// in front of them is ceremony. Everything behind the door is untouched and still reachable with
	// -ShowInstructions, because the controls may grow something that needs teaching again.
	TAutoConsoleVariable<int32> CVarShowInstructionsButton(
		TEXT("surf.start.instructionsbutton"), 0,
		TEXT("Start screen: 1 = show the SHOW INSTRUCTIONS button, 0 = hide it (default)."),
		ECVF_Default);

	// Which card -ShowInstructions opens on. The later ones are only reachable by tapping, which a
	// headless capture cannot do — and they are the ones using the side-on board.
	TAutoConsoleVariable<int32> CVarStartStep(
		TEXT("surf.start.step"), 0,
		TEXT("Start-tutorial: which step -ShowInstructions opens on (0-based)."),
		ECVF_Default);

	// Reusable flat-color brushes (function-local statics -> stable addresses for Slate).
	const FSlateBrush* NightBrush()   { static const FSlateColorBrush B(C_Night); return &B; }
	const FSlateBrush* ScrimBrush()   { static const FSlateColorBrush B(C_Scrim); return &B; }
	const FSlateBrush* StepScrimBrush() { static const FSlateColorBrush B(C_ScrimSteps); return &B; }
	const FSlateBrush* PanelBrush()   { static const FSlateColorBrush B(C_Panel); return &B; }
	const FSlateBrush* WhiteBrush()   { static const FSlateColorBrush B(FLinearColor::White); return &B; } // tint via BorderBackgroundColor

	// How much weight a button carries. This is the one place that decides it — callers name a
	// tier, never a colour, so the Start screen and the tutorial cards can't drift apart.
	enum class ETutorialBtnTier : uint8
	{
		Primary,    // coral fill, dark ink label — the action that moves the player forward
		Secondary,  // blue fill, white label — the alternative
		Ghost,      // scrim + keyline, white label — the way out
	};

	// Stadium pills: the default rounding is HalfHeightRadius, so the radius tracks the button
	// height and every tier stays a true pill whatever font size it is built at.
	const FSlateBrush* TutorialPrimaryBrush()   { static const FSlateRoundedBoxBrush B(C_Coral);   return &B; }
	const FSlateBrush* TutorialSecondaryBrush() { static const FSlateRoundedBoxBrush B(C_BtnBlue); return &B; }

	// Ghost tier. bUseBrushTransparency stays false so the keyline keeps its own 60% alpha over
	// the 18% fill — turning it on would force the outline down to the fill's alpha.
	const FSlateBrush* TutorialGhostBrush()
	{
		static const FSlateRoundedBoxBrush B = []
		{
			FSlateRoundedBoxBrush Brush(C_GhostFill);
			Brush.OutlineSettings = FSlateBrushOutlineSettings(C_GhostOutline, GhostOutline);
			return Brush;
		}();
		return &B;
	}

	const FSlateBrush* TutorialTierBrush(ETutorialBtnTier Tier)
	{
		switch (Tier)
		{
		case ETutorialBtnTier::Primary:   return TutorialPrimaryBrush();
		case ETutorialBtnTier::Secondary: return TutorialSecondaryBrush();
		default:                          return TutorialGhostBrush();
		}
	}

	// Dark ink on coral (6.4:1); white everywhere else. White on coral is only 2.8:1 — legible on a
	// monitor, gone in daylight on a phone.
	FLinearColor TutorialTierLabelColor(ETutorialBtnTier Tier)
	{
		return Tier == ETutorialBtnTier::Primary ? C_Ink : C_White;
	}

	// White rounded card behind the (transparent) illustration art.
	const FSlateBrush* IlloCardBrush()
	{
		static const FSlateRoundedBoxBrush B(FLinearColor::White, 16.0f);
		return &B;
	}

	FSlateFontInfo Bold(int32 Size)    { return FCoreStyle::GetDefaultFontStyle("Bold", Size); }
	FSlateFontInfo Regular(int32 Size) { return FCoreStyle::GetDefaultFontStyle("Regular", Size); }

	// The finalized tutorial copy. 3 steps, each an action the player performs — the old
	// 4th "You take control" card described the paddle-in wait, which the skip-paddle state
	// injection removed (specs/skip-paddle-intro.md).
	const int32 NumSteps = 3;

	// Step 0 reads differently depending on how the phone is held: rocking it works when it lies flat,
	// turning it like a steering wheel works when it stands up. Only step 0 — the pitch and pump
	// gestures survive both grips unchanged (the tilt basis was reseeded onto +X so pitch stays
	// well-conditioned upright: |Fwd| 0.999 upright, 0.921 reclined). Steps 1-2 ignore bVertical.
	FText StepTitle(int32 i, bool bVertical)
	{
		switch (i)
		{
		case 0: return FText::FromString(bVertical ? TEXT("STEER SIDEWAYS") : TEXT("TILT SIDEWAYS"));
		case 1: return FText::FromString(TEXT("TILT FORWARD & BACK"));
		default:return FText::FromString(TEXT("PUMP IN & OUT"));
		}
	}
	FText StepDesc(int32 i, bool bVertical)
	{
		switch (i)
		{
		// The upright line is kept shorter than the flat one on purpose: it has to fit the same slot
		// on a phone, whose usable height after the 2x DPI scale is well under this 16:9 editor view.
		// "Steering wheel" is doing the disambiguating, so the mechanism ("onto its edge") gives way.
		case 0: return FText::FromString(bVertical
			? TEXT("Turn the phone left and right like a steering wheel to carve your turns.")
			: TEXT("Rock the phone left or right to lean the board onto its edge and carve your turns."));
		case 1: return FText::FromString(TEXT("Tip the phone forward to drop the nose for more speed; tilt it back to lift the nose and turn faster."));
		default:return FText::FromString(TEXT("Pump the phone toward you and back to power the board — each pump builds speed."));
		}
	}

	// Load an imported texture into a cached FSlateBrush, keeping the texture rooted for the process
	// lifetime (Slate brushes hold a raw resource pointer). Returns nullptr if the asset isn't there
	// yet (e.g. the PNG hasn't been imported), so callers can fall back to a placeholder. Quiet on miss.
	const FSlateBrush* LoadFrameBrush(const TCHAR* AssetPath)
	{
		static TMap<FString, TSharedPtr<FSlateBrush>> Cache;
		static TArray<TStrongObjectPtr<UTexture2D>> Keep;

		const FString Key(AssetPath);
		if (TSharedPtr<FSlateBrush>* Found = Cache.Find(Key)) return Found->Get();

		TSharedPtr<FSlateBrush> Brush;
		if (UTexture2D* Tex = LoadObject<UTexture2D>(nullptr, AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			Keep.Add(TStrongObjectPtr<UTexture2D>(Tex));
			Brush = MakeShared<FSlateBrush>();
			Brush->SetResourceObject(Tex);
			Brush->ImageSize = FVector2D(Tex->GetSizeX(), Tex->GetSizeY());
			Brush->DrawAs = ESlateBrushDrawType::Image;
		}
		Cache.Add(Key, Brush);
		return Brush.Get();
	}
}

// ---- the widget ------------------------------------------------------------------------------
class SStartTutorial : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SStartTutorial) {}
		/** The hub's actions. An argument rather than a member set afterwards, because the menu is
		 *  built inside Construct and its buttons bind their visibility to these. */
		SLATE_ARGUMENT(StartTutorial::FHooks, Hooks)
	SLATE_END_ARGS()

	/** Invoked when the player presses Start (menu button, or Start on the final tutorial card). */
	TFunction<void()> OnStartRequested;

	void Construct(const FArguments& InArgs)
	{
		Hooks = InArgs._Hooks;
		OnStartRequested = Hooks.OnStart;

		// Load the background loop up front. Stops at the first missing index, so the frame count is
		// however many were imported — no constant to keep in sync with the capture.
		for (int32 i = 0; i < BgMaxFrames; ++i)
		{
			const FSlateBrush* Brush = LoadFrameBrush(
				*FString::Printf(TEXT("/Game/Images/T_StartBg_%03d.T_StartBg_%03d"), i, i));
			if (!Brush)
			{
				break;
			}
			BgFrames.Add(Brush);
		}
		UE_LOG(LogSurf, Display, TEXT("StartTutorial: background loop %d frame(s)%s."),
			BgFrames.Num(), BgFrames.Num() == 0 ? TEXT(" — falling back to the flat panel") : TEXT(""));

		ChildSlot
		[
			// Full-screen ground: the recorded wave loop stretched to fill, under a night scrim that
			// keeps the type readable. Falls back to the flat night panel when the frames haven't
			// been imported yet, so the menu is never unreadable.
			SNew(SOverlay)
			+ SOverlay::Slot()
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Fill)
			[
				SNew(SScaleBox)
				.Stretch(EStretch::ScaleToFill)
				[
					SAssignNew(BackgroundImage, SImage)
					.Image(BgFrames.Num() > 0 ? BgFrames[0] : NightBrush())
				]
			]
			// Extra veil for the tutorial cards, full-screen and shown only on that page. Sized by
			// the root overlay rather than the 900px content column, or it draws as a band.
			+ SOverlay::Slot()
			.HAlign(HAlign_Fill)
			.VAlign(VAlign_Fill)
			[
				SAssignNew(StepScrim, SBorder)
				.BorderImage(StepScrimBrush())
				.Padding(0)
				.Visibility(EVisibility::Collapsed)
			]
			+ SOverlay::Slot()
			[
				SNew(SBorder)
				.BorderImage(ScrimBrush())
				.Padding(0)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Fill)
					[
						SNew(SBox)
						.MaxDesiredWidth(900.0f)
						.Padding(FMargin(24.0f, 20.0f))
						[
							SAssignNew(RootSwitcher, SWidgetSwitcher)
							+ SWidgetSwitcher::Slot() [ BuildStartScreen() ]   // index 0
							+ SWidgetSwitcher::Slot() [ BuildTutorial() ]      // index 1
						]
					]
					// ABOUT, in the corner and not in the hub row (specs/about-screen.md). Testers
					// tap START without reading, and the row under it must stay two things you do
					// between waves. Ghost tier at the smallest size so it reads as metadata, not
					// as a choice; hidden on the tutorial cards, whose corners are the step nav's.
					+ SOverlay::Slot()
					.HAlign(HAlign_Right)
					.VAlign(VAlign_Bottom)
					.Padding(FMargin(0.0f, 0.0f, 14.0f, 12.0f))
					[
						SNew(SBox)
						.Visibility_Lambda([this]()
						{
							return (Hooks.OnAbout && RootSwitcher.IsValid() && RootSwitcher->GetActiveWidgetIndex() == 0)
								? EVisibility::Visible : EVisibility::Collapsed;
						})
						[
							SNew(SButton)
							.ButtonStyle(FCoreStyle::Get(), "NoBorder")
							.OnClicked(this, &SStartTutorial::OnAboutClicked)
							.ContentPadding(FMargin(0))
							[
								SNew(SBorder)
								.BorderImage(TutorialGhostBrush())
								// ~80x36 logical: a thumb target (about 8x3.5 mm on the phone), still
								// a third of the hub row's buttons.
								.Padding(FMargin(18.0f, 9.0f))
								[
									SNew(STextBlock)
									.Text(NSLOCTEXT("GoneSurfing", "HubAbout", "ABOUT"))
									.Font(Bold(13))
									.ColorAndOpacity(FSlateColor(FLinearColor(C_Foam.R, C_Foam.G, C_Foam.B, 0.8f)))
									.ShadowOffset(FVector2D(1.0f, 1.0f))
									.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.45f))
								]
							]
						]
					]
				]
			]
		];

		// -ShowInstructions opens straight on the tutorial cards. They are otherwise only reachable
		// by tapping, which a headless screenshot run cannot do — and the cards are the hardest
		// thing to keep legible over the wave, so they are the part most worth capturing.
		// Live, not baked at build time: the rack changes the board while this menu is up.
		if (BoardLabelText.IsValid())
		{
			BoardLabelText->SetText(TAttribute<FText>::CreateSP(this, &SStartTutorial::BoardButtonText));
		}

		const bool bOpenOnSteps = FParse::Param(FCommandLine::Get(), TEXT("ShowInstructions"));
		bOpenedOnSteps = bOpenOnSteps;
		RootSwitcher->SetActiveWidgetIndex(bOpenOnSteps ? 1 : 0);
		SetStepScrim(bOpenOnSteps);
		UpdateNav();
		if (bOpenOnSteps && Hooks.OnInstructionsView) { Hooks.OnInstructionsView(true); }

		// Drive the two-frame illustration flip (runs even while the game is paused behind the menu).
		// Ticks every frame; each step swaps on its own StepFlipInterval accumulated from DeltaTime.
		FlipAccum.SetNumZeroed(FrameSwitchers.Num());
		RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SStartTutorial::TickFlip));
	}

	/** The world whose PlayerController supplies motion state. Set right after construction. */
	void SetWorld(UWorld* InWorld) { World = InWorld; }

	// --------- swipe / tap on the tutorial (touch + mouse-drag) ---------
	virtual FReply OnMouseButtonDown(const FGeometry&, const FPointerEvent& E) override { return BeginPointer(E); }
	virtual FReply OnMouseButtonUp  (const FGeometry&, const FPointerEvent& E) override { return EndPointer(E); }
	virtual FReply OnTouchStarted   (const FGeometry&, const FPointerEvent& E) override { return BeginPointer(E); }
	virtual FReply OnTouchEnded     (const FGeometry&, const FPointerEvent& E) override { return EndPointer(E); }

private:
	StartTutorial::FHooks Hooks;              // the hub's actions (specs/two-screen-navigation.md)
	TSharedPtr<SBorder> StepScrim;            // extra veil, tutorial page only
	TSharedPtr<SImage> BackgroundImage;       // the recorded wave loop behind everything
	TArray<const FSlateBrush*> BgFrames;      // cached by LoadFrameBrush; stable for the process
	float BgAccum = 0.0f;
	int32 BgIndex = 0;

	TSharedPtr<SWidgetSwitcher> RootSwitcher; // 0 = start menu, 1 = tutorial
	TSharedPtr<SWidgetSwitcher> StepSwitcher; // 0..NumSteps-1
	TSharedPtr<STextBlock> CounterText;
	TSharedPtr<STextBlock> NextLabel;
	TSharedPtr<STextBlock> BoardLabelText;    // the hub's board button, "BOARD · SHORTBOARD"
	TArray<TSharedPtr<SBorder>> Dots;
	TArray<TSharedPtr<SWidgetSwitcher>> FrameSwitchers; // per-step two-frame illustration flip
	TArray<float> FlipAccum;                             // per-step time since last frame swap

	// How the phone is being held, and the widgets that follow it. Only steps with a pose variant
	// appear in the two maps.
	TWeakObjectPtr<UWorld> World;
	TMap<int32, TSharedPtr<STextBlock>> PoseTitles;
	TMap<int32, TSharedPtr<STextBlock>> PoseDescs;
	TMap<int32, TSharedPtr<SWidget>>    PosePages;   // the whole card, for the dip
	bool  bVerticalPose = false;  // false = flat-phone card (also the desktop / no-sensor default)
	float PoseVertical  = 0.0f;   // low-passed |g.z|/|g|
	bool  bPoseSeeded   = false;  // first good sample jumps straight to it instead of fading in
	bool  bPoseDecided  = false;  // has any pose classification happened yet?

	// The pose the sensor wants, which trails bVerticalPose by one dip: the visible state only
	// catches up at the bottom of the fade. Rotating back mid-dip therefore cancels the swap
	// instead of queueing a second one.
	bool  bPendingVertical = false;
	float PoseFade = 1.0f;        // 0 = card invisible (the moment the swap happens), 1 = settled

	// ---- the live board glyph ----
	TArray<TSharedPtr<STutorialBoardGlyph>> GlyphWidgets;  // per step
	TArray<TSharedPtr<SWidget>> GlyphGroups;               // per step: glyph + its caption, for fading

	/** The tutorial's OWN neutral pose. Never written to the pawn: the ride calibrates at autopilot
	 *  handoff and corrupting that would move where "centred" is for the whole session. */
	SurfTilt::FBasis PreviewBasis;
	SurfTilt::FYawState PreviewYaw;
	float PreviewPumpLowPass = 0.0f;

	/** Smoothed display pose, and how long since the player last did anything. */
	float GlyphOffsetX = 0.0f;   // the board's own attitude -- lagged, and what success measures
	float GlyphOffsetY = 0.0f;
	float GlyphVelX = 0.0f;      // its angular velocity, so it carries momentum into a lean
	float GlyphVelY = 0.0f;
	float GlyphRollDeg = 0.0f;   // and the drawn angles, which carry the exaggeration
	float GlyphPitchDeg = 0.0f;
	float GlyphSurge = 0.0f;   // raw pump input, 0..1
	float GlyphTravel = 0.0f;  // where the pump board is along its flight
	float GlyphSpeed = 0.0f;   // and how fast — drives the speed lines
	float FakeSweepPhase = 0.0f;  // surf.start.fakesweep only

	/** Visibility of the glyph half of the card: 0 while the basis is untrustworthy. */
	float GlyphFade = 0.0f;
	bool  bGlyphAllowed = false;

	// ---- success (FR6) ----
	/** Per step: earned yet, whether each direction has been reached, and the decaying flourish.
	 *  Latched — going back to a card never takes the mark away. */
	/** Ever earned — the permanent record, and the only thing the dots read. Never cleared. */
	TArray<bool>  StepEarned;
	/** Earned on THIS visit. Cleared every time the card is opened, so the gesture can always be
	 *  practised again — a player who does not know what they did needs to be able to redo it, and
	 *  a card frozen on "GOT IT" gives them nothing to try. */
	TArray<bool>  StepVisitEarned;
	TArray<bool>  StepReachedPos;
	TArray<bool>  StepReachedNeg;
	TArray<float> StepFlash;
	TArray<TSharedPtr<STextBlock>> GlyphCaptions;   // doubles as the coach — see GlyphCaption()

	/** Set when the pump board wraps off the edge with real speed behind it. */
	bool bPumpLaunched = false;

	/** Emphasis on Next after a card is earned: a beat that says "you are done here, go on".
	 *  Without it, success landed and then nothing suggested what to do with it. */
	TSharedPtr<SBox> NextButtonBox;
	float NextPulse = 0.0f;

	int32 StepIndex = 0;
	FVector2D PointerDown = FVector2D::ZeroVector;
	bool bPointerActive = false;

	/** -ShowInstructions capture support: jump to whatever surf.start.step names. Applied on change
	 *  rather than at Construct, because -ExecCmds lands a frame or two after the overlay is built. */
	bool  bOpenedOnSteps = false;
	int32 LastStartStepCVar = 0;

	// ---------- navigation ----------
	void ShowMenu()         { RootSwitcher->SetActiveWidgetIndex(0); SetStepScrim(false); if (Hooks.OnInstructionsView) { Hooks.OnInstructionsView(false); } }
	void ShowInstructions() { StepIndex = 0; StepSwitcher->SetActiveWidgetIndex(0); RootSwitcher->SetActiveWidgetIndex(1); SetStepScrim(true); UpdateNav(); SeedPreviewBasis(); if (Hooks.OnInstructionsView) { Hooks.OnInstructionsView(true); } }

	/** The tutorial-only veil. Hit-test invisible so it never swallows a tap meant for a button. */
	void SetStepScrim(bool bOn)
	{
		if (StepScrim.IsValid())
		{
			StepScrim->SetVisibility(bOn ? EVisibility::HitTestInvisible : EVisibility::Collapsed);
		}
	}
	void RequestStart()     { if (OnStartRequested) OnStartRequested(); }

	void GoToStep(int32 NewIndex)
	{
		StepIndex = FMath::Clamp(NewIndex, 0, NumSteps - 1);
		StepSwitcher->SetActiveWidgetIndex(StepIndex);
		ResetStepAttempt(StepIndex);
		UpdateNav();
	}

	/** Clear this card's attempt so the gesture can be performed again. StepEarned — the permanent
	 *  record the dots read — is deliberately untouched: coming back to practise should not cost
	 *  you the mark, and a card stuck on "GOT IT" with a board that still moves reads as unfinished
	 *  business rather than as an achievement. */
	void ResetStepAttempt(int32 Step)
	{
		if (!StepVisitEarned.IsValidIndex(Step)) return;
		StepVisitEarned[Step] = false;
		StepReachedPos[Step] = false;
		StepReachedNeg[Step] = false;
		StepFlash[Step] = 0.0f;
		bPumpLaunched = false;
		RefreshCaption(Step, /*bEarned*/ false, /*bHalfWay*/ false);
		if (GlyphWidgets.IsValidIndex(Step) && GlyphWidgets[Step].IsValid())
		{
			GlyphWidgets[Step]->SetDirectionProgress(Step != 2, false, false);
			GlyphWidgets[Step]->SetSuccessFlash(0.0f);
		}
	}
	void Next() { if (StepIndex < NumSteps - 1) GoToStep(StepIndex + 1); else RequestStart(); }
	void Prev() { if (StepIndex > 0) GoToStep(StepIndex - 1); else ShowMenu(); }

	void UpdateNav()
	{
		if (CounterText.IsValid())
		{
			CounterText->SetText(FText::FromString(FString::Printf(TEXT("%d / %d"), StepIndex + 1, NumSteps)));
		}
		if (NextLabel.IsValid())
		{
			NextLabel->SetText(FText::FromString(StepIndex == NumSteps - 1 ? TEXT("START") : TEXT("NEXT ›")));
			NextLabel->SetColorAndOpacity(FSlateColor(TutorialTierLabelColor(ETutorialBtnTier::Primary)));
		}
		for (int32 i = 0; i < Dots.Num(); ++i)
		{
			if (!Dots[i].IsValid()) continue;
			// Three states now: where you are (white), what you have actually performed (coral),
			// and the rest (faint). Earned beats current, so the mark is never hidden by standing
			// on the card that earned it.
			const bool bEarned = StepEarned.IsValidIndex(i) && StepEarned[i];
			Dots[i]->SetBorderBackgroundColor(
				bEarned ? FSlateColor(C_Coral)
				        : (i == StepIndex ? FSlateColor(C_White) : FSlateColor(FLinearColor(1, 1, 1, 0.35f))));
		}
	}

	// ---------- pointer helpers ----------
	FReply BeginPointer(const FPointerEvent& E)
	{
		if (!RootSwitcher.IsValid() || RootSwitcher->GetActiveWidgetIndex() != 1) return FReply::Unhandled();
		PointerDown = E.GetScreenSpacePosition();
		bPointerActive = true;
		return FReply::Handled().CaptureMouse(SharedThis(this));
	}
	FReply EndPointer(const FPointerEvent& E)
	{
		if (!bPointerActive) return FReply::Unhandled();
		bPointerActive = false;
		const FVector2D D = E.GetScreenSpacePosition() - PointerDown;
		FReply R = FReply::Handled().ReleaseMouseCapture();
		if (FMath::Abs(D.X) > 40.0f && FMath::Abs(D.X) > FMath::Abs(D.Y))
		{
			if (D.X < 0.0f) Next(); else Prev();   // swipe left = forward, right = back
		}
		else if (D.Size() < 12.0f)
		{
			Next();                                 // a tap advances
		}
		return R;
	}

	// ---------- builders ----------
	TSharedRef<SWidget> MakeButton(const FText& Label, ETutorialBtnTier Tier, int32 FontSize, FOnClicked OnClicked, TSharedPtr<STextBlock>* OutLabel = nullptr)
	{
		// Light labels get a drop shadow so they survive whitewater showing through the ghost
		// tier; the dark label on coral would only muddy it.
		const bool bLightLabel = (Tier != ETutorialBtnTier::Primary);

		TSharedPtr<STextBlock> Text;
		TSharedRef<SButton> Btn = SNew(SButton)
			.ButtonStyle(FCoreStyle::Get(), "NoBorder")
			.OnClicked(OnClicked)
			.HAlign(HAlign_Center)
			.VAlign(VAlign_Center)
			.ContentPadding(FMargin(0))
			[
				SNew(SBorder)
				.BorderImage(TutorialTierBrush(Tier))
				.Padding(FMargin(40.0f, 18.0f))
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SAssignNew(Text, STextBlock)
					.Text(Label)
					.Font(Bold(FontSize))
					.ColorAndOpacity(FSlateColor(TutorialTierLabelColor(Tier)))
					.ShadowOffset(bLightLabel ? FVector2D(1.0f, 2.0f) : FVector2D::ZeroVector)
					.ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, bLightLabel ? 0.45f : 0.0f))
				]
			];
		if (OutLabel) *OutLabel = Text;
		return Btn;
	}

	TSharedRef<SWidget> BuildStartScreen()
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().FillHeight(1.0f).HAlign(HAlign_Center).VAlign(VAlign_Bottom)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
				[
					SNew(SHorizontalBox)
					// Shadowed: the wordmark now sits over live water, which can be brighter than any
					// scrim setting that still lets the wave read.
					+ SHorizontalBox::Slot().AutoWidth() [ SNew(STextBlock).Text(FText::FromString(TEXT("GONE "))).Font(Bold(46)).ColorAndOpacity(FSlateColor(C_Foam))
						.ShadowOffset(FVector2D(2.0f, 3.0f)).ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.55f)) ]
					+ SHorizontalBox::Slot().AutoWidth() [ SNew(STextBlock).Text(FText::FromString(TEXT("SURFING"))).Font(Bold(46)).ColorAndOpacity(FSlateColor(C_Gold))
						.ShadowOffset(FVector2D(2.0f, 3.0f)).ShadowColorAndOpacity(FLinearColor(0.0f, 0.0f, 0.0f, 0.55f)) ]
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 6)
				[
					MakeButton(FText::FromString(TEXT("START")), ETutorialBtnTier::Primary, 28,
						FOnClicked::CreateSP(this, &SStartTutorial::OnStartClicked))
				]
				// The hub row (specs/two-screen-navigation.md FR1). Replay and the board rack used to
				// be buttons on the ride screen, where testers never found them and where they
				// competed with the wave; reviewing rides and picking gear are things you do between
				// waves, so they live here now. Both are secondaries under START, which stays the
				// single big control - testers tap it without reading, and that must survive.
				// Each one is drawn only when it has something to do: no rides, no Replay; no
				// boards installed, no board button.
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 6)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0)
					[
						SNew(SBox)
						.Visibility_Lambda([this]()
						{
							return (Hooks.OnReplay && Hooks.HasRides && Hooks.HasRides())
								? EVisibility::Visible : EVisibility::Collapsed;
						})
						[
							MakeButton(FText::FromString(TEXT("REPLAY")), ETutorialBtnTier::Secondary, 18,
								FOnClicked::CreateSP(this, &SStartTutorial::OnReplayClicked))
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0)
					[
						SNew(SBox)
						.Visibility_Lambda([this]()
						{
							return (Hooks.OnChangeBoard && Hooks.BoardLabel && !Hooks.BoardLabel().IsEmpty())
								? EVisibility::Visible : EVisibility::Collapsed;
						})
						[
							// The label carries the current board, so a pick made in the rack is
							// readable from the hub without opening it again.
							MakeButton(FText::GetEmpty(), ETutorialBtnTier::Secondary, 18,
								FOnClicked::CreateSP(this, &SStartTutorial::OnChangeBoardClicked), &BoardLabelText)
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 6)
				[
					// Hidden by default since 2026-09-09: with steering on a joystick and a button
					// labelled PUMP, the controls explain themselves and a tutorial in front of them
					// reads as ceremony. That only holds while the controls can be READ before the
					// ride, which is why the pawn now draws them at rest over this menu (FR1a,
					// 2026-09-22: they used to appear at the handoff, with no time to read). The
					// cards, their live board preview and -ShowInstructions all still work - this
					// only removes the door from the start screen, because the controls may well
					// grow something that needs teaching again.
					SNew(SBox)
					.Visibility_Lambda([]()
					{
						return CVarShowInstructionsButton.GetValueOnGameThread() != 0
							? EVisibility::Visible : EVisibility::Collapsed;
					})
					[
						MakeButton(FText::FromString(TEXT("SHOW INSTRUCTIONS")), ETutorialBtnTier::Secondary, 18,
							FOnClicked::CreateSP(this, &SStartTutorial::OnShowInstructionsClicked))
					]
				]
			];
	}


	TSharedRef<SWidget> BuildTutorial()
	{
		TSharedRef<SWidgetSwitcher> Steps = SNew(SWidgetSwitcher);
		for (int32 i = 0; i < NumSteps; ++i)
		{
			Steps->AddSlot() [ BuildStep(i) ];
		}
		StepSwitcher = Steps;

		// progress dots
		TSharedRef<SHorizontalBox> DotRow = SNew(SHorizontalBox);
		Dots.Reset();
		for (int32 i = 0; i < NumSteps; ++i)
		{
			TSharedPtr<SBorder> Dot;
			DotRow->AddSlot().AutoWidth().Padding(4, 0).VAlign(VAlign_Center)
			[
				SNew(SBox).WidthOverride(11.0f).HeightOverride(11.0f)
				[
					SAssignNew(Dot, SBorder).BorderImage(WhiteBrush())
				]
			];
			Dots.Add(Dot);
		}

		return SNew(SVerticalBox)
			// top bar: Back / counter / Skip
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					MakeButton(FText::FromString(TEXT("‹ BACK")), ETutorialBtnTier::Ghost, 16,
						FOnClicked::CreateSP(this, &SStartTutorial::OnBackClicked))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					SAssignNew(CounterText, STextBlock).Text(FText::FromString(TEXT("1 / 4"))).Font(Bold(16))
					.ColorAndOpacity(FSlateColor(FLinearColor(C_Foam.R, C_Foam.G, C_Foam.B, 0.85f)))
					.ShadowOffset(TextShadow).ShadowColorAndOpacity(C_TextShadow)
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					MakeButton(FText::FromString(TEXT("SKIP ✕")), ETutorialBtnTier::Ghost, 16,
						FOnClicked::CreateSP(this, &SStartTutorial::OnSkipClicked))
				]
			]
			// the step content
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0, 10)
			[
				Steps
			]
			// bottom bar: dots / Next|Start
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center) [ DotRow ]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					// Boxed so AdvanceNextPulse has something to scale after a card is earned.
					SAssignNew(NextButtonBox, SBox)
					[
						MakeButton(FText::FromString(TEXT("NEXT ›")), ETutorialBtnTier::Primary, 20,
							FOnClicked::CreateSP(this, &SStartTutorial::OnNextClicked), &NextLabel)
					]
				]
			];
	}

	// Seconds between frame swaps for a step. Pump (step 2) flips faster to read as a quick pumping motion.
	float StepFlipInterval(int32 Step) const { return Step == 2 ? 0.20f : FlipInterval; }

	// Advance each step's two-frame flip on its own cadence.
	EActiveTimerReturnType TickFlip(double, float DeltaTime)
	{
		// Background loop. Slate active timers run on real time, independent of the world pause —
		// which is exactly why the recorded loop plays behind a paused menu when a live scene can't.
		if (BgFrames.Num() > 1 && BackgroundImage.IsValid())
		{
			BgAccum += DeltaTime;
			const float FrameTime = 1.0f / FMath::Max(1.0f, BgFps);
			if (BgAccum >= FrameTime)
			{
				// Advance by however many frames elapsed, so a hitch doesn't slow the wave down.
				const int32 Steps = FMath::FloorToInt(BgAccum / FrameTime);
				BgAccum -= Steps * FrameTime;
				BgIndex = (BgIndex + Steps) % BgFrames.Num();
				BackgroundImage->SetImage(BgFrames[BgIndex]);
			}
		}

		if (bOpenedOnSteps)
		{
			const int32 WantStep = CVarStartStep.GetValueOnGameThread();
			if (WantStep != LastStartStepCVar)
			{
				LastStartStepCVar = WantStep;
				GoToStep(WantStep);
			}
		}

		UpdateHoldPose(DeltaTime);
		AdvancePoseFade(DeltaTime);
		UpdateBoardGlyph(DeltaTime);

		// The illustration flip runs unconditionally. It used to pause while the player was moving
		// the phone, on the theory that the loop and the hand were two motions arguing — but a hand
		// holding a phone is never still, so in practice the demo simply never played (device,
		// 2026-08-21). A frozen illustration is a worse failure than a busy one.
		for (int32 i = 0; i < FrameSwitchers.Num(); ++i)
		{
			FlipAccum[i] += DeltaTime;
			if (FlipAccum[i] >= StepFlipInterval(i))
			{
				FlipAccum[i] = 0.0f;
				if (FrameSwitchers[i].IsValid())
				{
					// Low bit is the A/B frame; the rest is the hold-pose variant's slot offset.
					const int32 Frame = FrameSwitchers[i]->GetActiveWidgetIndex() & 1;
					FrameSwitchers[i]->SetActiveWidgetIndex(StepPoseBase(i) + (Frame ^ 1));
				}
			}
		}
		return EActiveTimerReturnType::Continue;
	}

	// Track how the phone is being held and swap the sideways card to match. Runs on the active
	// timer, NOT the pawn's Tick: the start screen pauses the world, so the pawn never ticks — but
	// the sensors keep delivering and GetInputMotionState reads straight through a pause. Sampling
	// this from a Tick is exactly how the sensor probe silently captured nothing for six device
	// sessions (specs/sensor-probe.md FR4).
	void UpdateHoldPose(float DeltaTime)
	{
		bool bWantVertical = bVerticalPose;

		const int32 Override = CVarStartHoldPose.GetValueOnGameThread();
		if (Override >= 0)
		{
			bWantVertical = Override != 0;
		}
		else
		{
			UWorld* W = World.Get();
			APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
			if (!PC) return;

			FVector Tilt, RotationRate, Gravity, Acceleration;
			PC->GetInputMotionState(Tilt, RotationRate, Gravity, Acceleration);

			// Drop untrustworthy samples rather than classifying on them: this catches both "no
			// sensor at all" (desktop, where the card must stay flat) and the settling transient
			// after the sensor starts — the probe capture shows |g| climbing 0.61 → 0.85 → 0.95
			// across three consecutive samples.
			const float GMag = Gravity.Size();
			if (GMag < PoseMinGravity) return;

			// Device Z is the phone's in-plane vertical axis: 0 flat, 1 upright. Absolute value, so
			// both landscape orientations (and lying back past vertical) select the same axis.
			const float Sample = FMath::Clamp(FMath::Abs(Gravity.Z) / GMag, 0.0f, 1.0f);
			PoseVertical = bPoseSeeded
				? FMath::Lerp(PoseVertical, Sample, FMath::Clamp(DeltaTime / PoseSmoothSeconds, 0.0f, 1.0f))
				: Sample;
			bPoseSeeded = true;

			// Hysteresis band: a phone parked near the ~30° crossover would otherwise flicker
			// between the two cards. Inside the band, whatever is showing stays.
			if      (PoseVertical > PoseVerticalEnter) bWantVertical = true;
			else if (PoseVertical < PoseVerticalExit)  bWantVertical = false;
		}

		// The very first classification lands before the player has looked at the card, so it is
		// applied outright — dipping here would just be a flinch on open, and would show the flat
		// card for a moment to someone already holding the phone upright.
		if (!bPoseDecided)
		{
			bPoseDecided = true;
			bPendingVertical = bVerticalPose = bWantVertical;
			ApplyHoldPose();
			return;
		}

		bPendingVertical = bWantVertical;
	}

	/** Drive the dip. The swap itself happens at the trough, where nothing is on screen to jump. */
	void AdvancePoseFade(float DeltaTime)
	{
		if (PosePages.Num() == 0) return;

		// Only the sideways card has two variants, so only it has anything to transition *to*. On the
		// pitch and pump cards the dip was pure damage: the pitch gesture (tip the phone forward and
		// back) is the very motion the pose classifier reads, so crossing the flat/upright threshold
		// ran the whole swap sequence — and its trough re-seeds the preview neutral, snapping the
		// board back to level and cancelling the gesture the card is asking for, all to land on the
		// same picture again. Off a pose-variant card the swap is applied silently instead: the
		// off-screen sideways card is already correct when the player returns to it.
		if (!StepHasPoseVariant(StepIndex))
		{
			if (bPendingVertical != bVerticalPose)
			{
				bVerticalPose = bPendingVertical;
				ApplyHoldPose();   // art + copy only — no SeedPreviewBasis, nothing visible to dip
			}
			if (PoseFade < 1.0f)
			{
				// A dip caught in flight by a page turn: restore the card rather than leaving it
				// parked at partial opacity for the next visit.
				PoseFade = 1.0f;
				for (const TPair<int32, TSharedPtr<SWidget>>& It : PosePages)
				{
					if (It.Value.IsValid()) It.Value->SetRenderOpacity(1.0f);
				}
			}
			return;
		}

		// A zero (or negative) knob means the caller wants the old instant swap: one frame at
		// opacity 0, no visible dip.
		const float FadeSeconds = FMath::Max(CVarStartPoseFade.GetValueOnGameThread(), 0.0f);
		const float Step = (FadeSeconds > 0.0f) ? (DeltaTime / FadeSeconds) : 1.0f;
		if (bPendingVertical != bVerticalPose)
		{
			PoseFade -= Step;
			if (PoseFade <= 0.0f)
			{
				PoseFade = 0.0f;
				bVerticalPose = bPendingVertical;
				ApplyHoldPose();
				// The grip changed, so the neutral captured in the old one is stale by roughly the
				// angle between them — re-seed here, at the trough, where nothing is on screen.
				SeedPreviewBasis();
			}
		}
		else if (PoseFade < 1.0f)
		{
			PoseFade = FMath::Min(PoseFade + Step, 1.0f);
		}
		else
		{
			return; // settled — don't touch render opacity every frame for nothing
		}

		// Eased rather than linear: a linear dip still reads mechanical at this duration.
		const float Opacity = FMath::SmoothStep(0.0f, 1.0f, PoseFade);
		for (const TPair<int32, TSharedPtr<SWidget>>& It : PosePages)
		{
			if (It.Value.IsValid()) It.Value->SetRenderOpacity(Opacity);
		}
	}

	/** Seed the tutorial's preview neutral from the current pose. Called when the cards open, and
	 *  again at the grip-swap trough — a neutral captured flat is ~90 degrees off once the player
	 *  sits up, and the preview would peg at full deflection and stay there. */
	void SeedPreviewBasis()
	{
		UWorld* W = World.Get();
		APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
		if (!PC) return;

		FVector Tilt, RotationRate, Gravity, Acceleration;
		PC->GetInputMotionState(Tilt, RotationRate, Gravity, Acceleration);
		if (Gravity.Size() < PoseMinGravity) return;

		PreviewBasis = SurfTilt::Calibrate(Gravity);
		PreviewYaw.Reset();
		// Level the board and kill its momentum. The drawn angles are derived from these, so zeroing
		// those alone would be undone on the next tick.
		GlyphOffsetX = GlyphOffsetY = GlyphVelX = GlyphVelY = 0.0f;
		GlyphRollDeg = GlyphPitchDeg = GlyphSurge = 0.0f;
	}

	/** Read the sensor, run it through the ride's own mapping, and pose the drawn board. */
	void UpdateBoardGlyph(float DeltaTime)
	{
		if (GlyphWidgets.Num() == 0) return;

		// Kept as the raw offset, and turned into drawn degrees only at the end. The success test
		// reads the offset, so scaling the drawing (surf.start.tiltgain) cannot quietly make a card
		// easier or harder to earn.
		float TargetOffsetX = 0.0f, TargetOffsetY = 0.0f, TargetSurge = 0.0f;
		bool bWantAllowed = false;

		float Fake = CVarStartFakeResponse.GetValueOnGameThread();
		const float SweepHz = CVarStartFakeSweep.GetValueOnGameThread();
		if (SweepHz > 0.0f)
		{
			FakeSweepPhase = FMath::Fmod(FakeSweepPhase + DeltaTime * SweepHz, 1.0f);
			const float Amplitude = FMath::IsNearlyZero(Fake) ? 1.0f : FMath::Abs(Fake);
			Fake = Amplitude * FMath::Sin(FakeSweepPhase * 2.0f * PI);
		}

		if (!FMath::IsNearlyZero(Fake))
		{
			// Desktop has no sensor at all, so this is the only way to see the glyph move without a
			// deploy. Drives whichever axis the visible card teaches.
			const float V = FMath::Clamp(Fake, -1.0f, 1.0f);
			TargetOffsetX = V;
			TargetOffsetY = V;
			TargetSurge = FMath::Abs(V);
			bWantAllowed = true;
		}
		else
		{
			// Seed lazily as well as on opening the cards: the player controller may not exist yet
			// when the overlay is built, the sensor may still be settling (SeedPreviewBasis rejects
			// |g| < 0.5), and -ShowInstructions jumps straight to the cards without passing through
			// the button that would otherwise seed it.
			if (!PreviewBasis.bCalibrated)
			{
				SeedPreviewBasis();
			}

			UWorld* W = World.Get();
			APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
			if (PC)
			{
				FVector Tilt, RotationRate, Gravity, Acceleration;
				PC->GetInputMotionState(Tilt, RotationRate, Gravity, Acceleration);

				// Pump needs no basis — gravity's direction and the acceleration along it — so it
				// stays live in every grip, flat included. Tilt does not, hence the gate below.
				SurfTilt::FPumpTuning PumpTuning;
				if (const USurfTuningSubsystem* T = TuningSubsystem())
				{
					PumpTuning.LowPassHz    = T->PumpLowPassHz;
					PumpTuning.Deadzone     = T->PumpDeadzone;
					PumpTuning.AccelForFull = T->PumpAccelForFull;
				}
				TargetSurge = SurfTilt::ComputePump(Gravity, Acceleration, PumpTuning, PreviewPumpLowPass, DeltaTime);

				bWantAllowed = PreviewBasis.IsUsable()
					&& PreviewBasis.Conditioning > (bGlyphAllowed ? GlyphMinConditioningExit : GlyphMinConditioning);

				if (bWantAllowed)
				{
					const SurfTilt::FResult R = SurfTilt::ComputeWeight(
						PreviewBasis, MakePreviewTuning(), PreviewYaw, Gravity, RotationRate, DeltaTime);

					// Exaggerated, because full deflection is 25 degrees of phone and a 1:1 glyph
					// would barely twitch. The deadzone is left uncompensated on purpose: dead
					// travel before anything happens is part of what the card is teaching.
					TargetOffsetX = R.Offset.X;
					TargetOffsetY = R.Offset.Y;
				}
			}
		}

		// Chase rather than snap: raw sensor reads as jitter, but too much smoothing and the board
		// stops feeling moved by the hand holding it.
		const float A = FMath::Clamp(DeltaTime * GlyphFollowHz, 0.0f, 1.0f);
		// Attitude follows a spring-damper toward the commanded lean, so the board has weight to it.
		// Semi-implicit (velocity first, then position) and a clamped step, because an explicit
		// integrator at a hitched frame is how a spring like this explodes.
		{
			float HzSetting   = TutorialBoardHzFallback;
			float ZetaSetting = TutorialBoardDampingFallback;
			if (const USurfTuningSubsystem* T = TuningSubsystem())
			{
				HzSetting   = T->TutorialBoardHz;
				ZetaSetting = T->TutorialBoardDamping;
			}
			const float Hz   = FMath::Max(HzSetting, 0.05f);
			const float Zeta = FMath::Max(ZetaSetting, 0.0f);
			const float W    = 2.0f * PI * Hz;
			const float Step = FMath::Min(DeltaTime, 1.0f / 30.0f);

			GlyphVelX += (W * W * (TargetOffsetX - GlyphOffsetX) - 2.0f * Zeta * W * GlyphVelX) * Step;
			GlyphVelY += (W * W * (TargetOffsetY - GlyphOffsetY) - 2.0f * Zeta * W * GlyphVelY) * Step;
			GlyphOffsetX += GlyphVelX * Step;
			GlyphOffsetY += GlyphVelY * Step;
		}

		// The pump signal is not an attitude, so it keeps the plain lag.
		GlyphSurge = FMath::Lerp(GlyphSurge, TargetSurge, A);

		const float TiltGain = FMath::Max(CVarStartTiltGain.GetValueOnGameThread(), 0.0f);
		GlyphRollDeg  = GlyphOffsetX * GlyphRollDegAtFull  * TiltGain;
		GlyphPitchDeg = GlyphOffsetY * GlyphPitchDegAtFull * TiltGain;

		// Fly the pump board. Pumping adds speed, nothing takes it away but drag, and at the far
		// edge it wraps round — so a rhythm of pumps keeps it running off the frame, which is the
		// card's whole claim. Stopped, it eases back to centred rather than parking off-screen.
		float PumpLaunch = TutorialPumpLaunchFallback;
		float PumpDrag   = TutorialPumpDragFallback;
		if (const USurfTuningSubsystem* T = TuningSubsystem())
		{
			PumpLaunch = T->TutorialPumpLaunch;
			PumpDrag   = T->TutorialPumpDrag;
		}
		GlyphSpeed += GlyphSurge * FMath::Max(PumpLaunch, 0.0f) * DeltaTime;
		GlyphSpeed -= GlyphSpeed * FMath::Min(DeltaTime * FMath::Max(PumpDrag, 0.0f), 1.0f);
		GlyphSpeed = FMath::Clamp(GlyphSpeed, 0.0f, GlyphTravelMaxSpeed);

		if (GlyphSpeed > GlyphTravelIdleSpeed)
		{
			GlyphTravel += GlyphSpeed * DeltaTime;
			if (GlyphTravel > GlyphTravelWrap)
			{
				GlyphTravel -= 2.0f * GlyphTravelWrap;
				// The board just left the frame. That — not an internal speed reading — is what the
				// pump card is earned by, because it is the thing the player can see happen.
				if (GlyphSpeed >= SuccessPumpWrapSpeed)
				{
					bPumpLaunched = true;
				}
			}
		}
		else
		{
			GlyphTravel = FMath::Lerp(GlyphTravel, 0.0f, FMath::Clamp(DeltaTime * 3.0f, 0.0f, 1.0f));
		}

		// Each card drives only the axis it teaches. The view alone isn't enough to decide this:
		// fore/aft and pump share the side-on board, so without the filter a pump card would also
		// pitch to a gesture it says nothing about.
		for (int32 i = 0; i < GlyphWidgets.Num(); ++i)
		{
			if (!GlyphWidgets[i].IsValid()) continue;
			switch (i)
			{
			case 0:  GlyphWidgets[i]->SetPose(GlyphRollDeg, 0.0f, 0.0f, 0.0f);  break;  // sideways
			case 1:  GlyphWidgets[i]->SetPose(0.0f, GlyphPitchDeg, 0.0f, 0.0f); break;  // fore/aft
			default: GlyphWidgets[i]->SetPose(0.0f, 0.0f, GlyphTravel, FMath::Min(GlyphSpeed, 1.0f)); break;
			}
		}

		UpdateSuccess(DeltaTime);

		// Fade the glyph out where the basis can't be trusted, rather than leaving it sitting at
		// rest — an inert glyph is the same "looks like a bug" failure the grip-swap dip fixed.
		// The pump card is exempt: its signal needs no basis.
		bGlyphAllowed = bWantAllowed;
		const bool bPumpCard = (StepIndex == 2);
		const float FadeTarget = (bWantAllowed || bPumpCard) ? 1.0f : 0.0f;
		const float FadeStep = DeltaTime / FMath::Max(GlyphFadeSeconds, KINDA_SMALL_NUMBER);
		GlyphFade = FMath::Clamp(GlyphFade + FMath::Clamp(FadeTarget - GlyphFade, -FadeStep, FadeStep), 0.0f, 1.0f);
		for (const TSharedPtr<SWidget>& Group : GlyphGroups)
		{
			if (Group.IsValid()) Group->SetRenderOpacity(GlyphFade);
		}
	}

	/** Watch the visible card for the gesture actually landing, and run the flourish (FR6).
	 *
	 *  Only the visible step is watched — earning a card the player is not looking at would spend
	 *  the beat on nobody. Nothing here gates anything: Next is always live, and a card that can't
	 *  be earned (tilt, held flat, glyph suppressed) simply doesn't collect the mark. */
	void UpdateSuccess(float DeltaTime)
	{
		const int32 i = StepIndex;
		if (!StepEarned.IsValidIndex(i)) return;

		if (!StepVisitEarned[i])
		{
			const bool bWasHalfWay = StepReachedPos[i] || StepReachedNeg[i];
			bool bJustEarned = false;

			if (i == 2)
			{
				// Pump: earned by the board leaving the frame, which is the thing the player sees.
				bJustEarned = bPumpLaunched;
			}
			else if (bGlyphAllowed)
			{
				// Both ways past the threshold. Measured on the drawn angle, so what the player has
				// to do is exactly what they can see themselves doing.
				const float Norm = (i == 0) ? GlyphOffsetX : GlyphOffsetY;
				const float Want = FMath::Clamp(CVarStartSuccessTilt.GetValueOnGameThread(), 0.05f, 1.0f);
				if (Norm >=  Want) StepReachedPos[i] = true;
				if (Norm <= -Want) StepReachedNeg[i] = true;
				bJustEarned = StepReachedPos[i] && StepReachedNeg[i];
			}

			// Reaching the FIRST direction is itself worth saying. It used to pass in silence, so a
			// player who tilted one way correctly had no idea it had counted.
			const bool bHalfWay = StepReachedPos[i] || StepReachedNeg[i];
			if (bHalfWay != bWasHalfWay || bJustEarned)
			{
				RefreshCaption(i, bJustEarned, bHalfWay);
			}

			if (bJustEarned)
			{
				StepVisitEarned[i] = true;
				StepEarned[i] = true;
				StepFlash[i] = 1.0f;
				NextPulse = 1.0f;   // and point at the way out
				UpdateNav();        // repaint the dots, so the row becomes a running score
			}
		}

		// The markers under the board, so progress through a two-way gesture is legible.
		if (GlyphWidgets.IsValidIndex(i) && GlyphWidgets[i].IsValid())
		{
			GlyphWidgets[i]->SetDirectionProgress(i != 2, StepReachedPos[i], StepReachedNeg[i]);
		}

		AdvanceNextPulse(DeltaTime);

		for (int32 s = 0; s < StepFlash.Num(); ++s)
		{
			if (StepFlash[s] > 0.0f)
			{
				const float FlashSeconds = FMath::Max(CVarStartSuccessFlash.GetValueOnGameThread(), KINDA_SMALL_NUMBER);
				StepFlash[s] = FMath::Max(0.0f, StepFlash[s] - DeltaTime / FlashSeconds);
			}
			if (GlyphWidgets.IsValidIndex(s) && GlyphWidgets[s].IsValid())
			{
				GlyphWidgets[s]->SetSuccessFlash(StepFlash[s]);
			}
		}
	}

	/** The caption is the coach: what is still wanted, then that it landed. */
	void RefreshCaption(int32 Step, bool bEarned, bool bHalfWay)
	{
		if (!GlyphCaptions.IsValidIndex(Step) || !GlyphCaptions[Step].IsValid()) return;
		GlyphCaptions[Step]->SetText(GlyphCaption(Step, bEarned, bHalfWay));
		GlyphCaptions[Step]->SetColorAndOpacity(FSlateColor((bEarned || bHalfWay) ? C_Coral : C_Slate));
	}

	/** A short scale pulse on Next once a card is earned. Success used to land and then sit there
	 *  with nothing suggesting what to do with it, which read as "there must be more to do". */
	void AdvanceNextPulse(float DeltaTime)
	{
		if (NextPulse <= 0.0f || !NextButtonBox.IsValid()) return;
		NextPulse = FMath::Max(0.0f, NextPulse - DeltaTime / 0.9f);
		const float Pop = 1.0f + 0.13f * FMath::Sin(NextPulse * PI);
		NextButtonBox->SetRenderTransformPivot(FVector2D(0.5f, 0.5f));
		NextButtonBox->SetRenderTransform(TransformCast<FSlateRenderTransform>(FScale2D(Pop, Pop)));
	}

	/** The tuning subsystem, or null. Guards GetGameInstance(), which is not always there while the
	 *  menu is up. */
	const USurfTuningSubsystem* TuningSubsystem() const
	{
		UWorld* W = World.Get();
		UGameInstance* GI = W ? W->GetGameInstance() : nullptr;
		return GI ? GI->GetSubsystem<USurfTuningSubsystem>() : nullptr;
	}

	/** The ride's own deflection knobs, so the preview deflects exactly as far as the board will. */
	SurfTilt::FTuning MakePreviewTuning() const
	{
		SurfTilt::FTuning T;
		if (const USurfTuningSubsystem* S = TuningSubsystem())
		{
			T.PitchDegreesForFull  = S->TiltPitchDegreesForFullDeflection;
			T.RollDegreesForFull   = S->TiltRollDegreesForFullDeflection;
			T.YawGain              = S->TiltYawGain;
			T.YawSign              = (S->TiltYawInvert >= 0.5f) ? -1.0f : 1.0f;
			T.YawLeakSeconds       = S->TiltYawLeakSeconds;
			T.YawBiasSampleSeconds = S->TiltYawBiasSampleSeconds;
		}
		return T;
	}

	/** Point every pose-variant step at the art and copy for the current hold pose. */
	void ApplyHoldPose()
	{
		for (int32 i = 0; i < FrameSwitchers.Num(); ++i)
		{
			if (!StepHasPoseVariant(i) || !FrameSwitchers[i].IsValid()) continue;
			// Keep the A/B phase, move the variant: swapping mid-flip shouldn't restart the motion.
			const int32 Frame = FrameSwitchers[i]->GetActiveWidgetIndex() & 1;
			FrameSwitchers[i]->SetActiveWidgetIndex(StepPoseBase(i) + Frame);
		}
		for (const TPair<int32, TSharedPtr<STextBlock>>& It : PoseTitles)
		{
			if (It.Value.IsValid()) It.Value->SetText(StepTitle(It.Key, bVerticalPose));
		}
		for (const TPair<int32, TSharedPtr<STextBlock>>& It : PoseDescs)
		{
			if (It.Value.IsValid()) It.Value->SetText(StepDesc(It.Key, bVerticalPose));
		}
	}

	// One frame of a step's placeholder animation: a blue marker parked left (A) or right (B), so
	// the flip visibly reads as a two-frame loop. Replaced by real art per BuildStep().
	TSharedRef<SWidget> MakePlaceholderFrame(bool bFrameB)
	{
		return SNew(SBorder)
			.BorderImage(PanelBrush())
			.Padding(0)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
					.HAlign(bFrameB ? HAlign_Right : HAlign_Left).VAlign(VAlign_Center).Padding(30.0f)
				[
					SNew(SBox).WidthOverride(56.0f).HeightOverride(56.0f)
					[
						SNew(SBorder).BorderImage(WhiteBrush()).BorderBackgroundColor(FSlateColor(C_BtnBlue))
					]
				]
				+ SOverlay::Slot()
					.HAlign(HAlign_Center).VAlign(VAlign_Bottom).Padding(FMargin(8, 8, 8, 12))
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(TEXT("frame %s — your art here"), bFrameB ? TEXT("B") : TEXT("A"))))
					.Font(Regular(11))
					.ColorAndOpacity(FSlateColor(C_Slate))
				]
			];
	}

	// Imported-texture path for a step's frame, or nullptr if that step has no art yet (→ placeholder).
	// Names match the PNGs the artist drops in Content/Images (imported to /Game/Images/<name>).
	//
	// The instruction textures were REMOVED from Content on 2026-09-13 (4 x 22 MB uncompressed, and
	// the cards are unreachable since the SHOW INSTRUCTIONS button went) to shrink the Android
	// package. LoadFrameBrush is LOAD_Quiet, so the cards fall back to the placeholder; re-import
	// the PNGs from git history if the tutorial ever comes back.
	const TCHAR* FrameTexturePath(int32 Step, bool bFrameB, bool bVertical = false) const
	{
		switch (Step)
		{
		// The "-2-" pair is the upright-phone variant of the sideways gesture (steering-wheel turn
		// rather than a rock). Nothing else has a second variant — see StepTitle.
		case 0:
			if (bVertical)
			{
				return bFrameB ? TEXT("/Game/Images/instructions-tilting-side-2-B.instructions-tilting-side-2-B")
				               : TEXT("/Game/Images/instructions-tilting-side-2-A.instructions-tilting-side-2-A");
			}
			return bFrameB ? TEXT("/Game/Images/instructions-tilting-side-B.instructions-tilting-side-B")
			               : TEXT("/Game/Images/instructions-tilting-side-A.instructions-tilting-side-A");
		case 1: return bFrameB ? TEXT("/Game/Images/instructions-tilting-pitch-B.instructions-tilting-pitch-B")
		                       : TEXT("/Game/Images/instructions-tilting-pitch-A.instructions-tilting-pitch-A");
		case 2: return bFrameB ? TEXT("/Game/Images/instructions-pump-B.instructions-pump-B")
		                       : TEXT("/Game/Images/instructions-pump-A.instructions-pump-A");
		default: return nullptr;
		}
	}

	// Aspect (width/height) of a step's art, read from its frame-A texture; 1.0 (square) if the step
	// has no art yet (placeholder). Used to size the card so the art isn't letterboxed in a square.
	float StepAspect(int32 Step) const
	{
		if (const TCHAR* Path = FrameTexturePath(Step, /*bFrameB*/ false))
		{
			if (const FSlateBrush* Brush = LoadFrameBrush(Path))
			{
				if (Brush->ImageSize.Y > 0.0f) return Brush->ImageSize.X / Brush->ImageSize.Y;
			}
		}
		return 1.0f;
	}

	// A step animates (two-frame flip) only if it has a distinct frame B; a step with only frame A
	// is shown as a single static image, no switcher.
	bool StepAnimates(int32 Step) const { return FrameTexturePath(Step, /*bFrameB*/ true) != nullptr; }

	// A step carries a second, hold-pose-specific variant of its art. Its switcher then holds four
	// slots — [flat A, flat B, upright A, upright B] — indexed as PoseBase + FrameBit, so there is
	// still exactly one switcher per step for TickFlip to walk.
	//
	// The upright path has to be *different*, not merely present: FrameTexturePath ignores bVertical
	// for the pitch and pump steps, so a null check alone called every step a variant step. Those two
	// then got two duplicate switcher slots and joined the swap machinery — which is how tipping the
	// phone made the pitch card dip out, dip back in on the identical picture, and reset the glyph.
	bool StepHasPoseVariant(int32 Step) const
	{
		if (!StepAnimates(Step)) return false;
		const TCHAR* Flat     = FrameTexturePath(Step, /*bFrameB*/ false, /*bVertical*/ false);
		const TCHAR* Upright  = FrameTexturePath(Step, /*bFrameB*/ false, /*bVertical*/ true);
		return Flat && Upright && FCString::Strcmp(Flat, Upright) != 0;
	}

	/** Slot offset of the variant currently on show for this step: 0 = flat art, 2 = upright art. */
	int32 StepPoseBase(int32 Step) const { return (bVerticalPose && StepHasPoseVariant(Step)) ? 2 : 0; }

	// One frame: the imported texture (fit to the box, aspect preserved) if it loads, else the placeholder.
	TSharedRef<SWidget> MakeFrame(int32 Step, bool bFrameB, bool bVertical = false)
	{
		if (const TCHAR* Path = FrameTexturePath(Step, bFrameB, bVertical))
		{
			if (const FSlateBrush* Brush = LoadFrameBrush(Path))
			{
				// Transparent art. The white rounded card is no longer per-frame: BuildStep owns it,
				// because the live board shares it (layout B).
				return SNew(SScaleBox).Stretch(EStretch::ScaleToFit) [ SNew(SImage).Image(Brush) ];
			}
		}
		return MakePlaceholderFrame(bFrameB);
	}

	TSharedRef<SWidget> BuildStep(int32 i)
	{
		// ---- ILLUSTRATION SLOT i — two-frame flip ----
		// The SWidgetSwitcher holds frame A (index 0) and frame B (index 1); TickFlip toggles every
		// step between them each FlipInterval. MakeFrame() uses the imported texture if present
		// (see FrameTexturePath), otherwise the placeholder marker.
		// Size the card to the art's aspect within a max envelope, so landscape art fills it instead
		// of being letterboxed in a square. Steps without art (placeholder) fall back to square.
		const float MaxW = 320.0f, MaxH = 240.0f;
		const float Aspect = StepAspect(i);
		float BoxW = MaxW, BoxH = MaxW / Aspect;
		if (BoxH > MaxH) { BoxH = MaxH; BoxW = MaxH * Aspect; }

		// Animated steps get a two-frame switcher; a single-image step (no frame B) shows one static
		// image and no switcher — a nullptr keeps the FrameSwitchers index aligned with the step index.
		TSharedRef<SWidget> Content = [&]() -> TSharedRef<SWidget>
		{
			if (StepAnimates(i))
			{
				TSharedRef<SWidgetSwitcher> Frames = SNew(SWidgetSwitcher)
					+ SWidgetSwitcher::Slot() [ MakeFrame(i, /*bFrameB*/ false) ]   // 0: flat, A
					+ SWidgetSwitcher::Slot() [ MakeFrame(i, /*bFrameB*/ true) ];   // 1: flat, B
				if (StepHasPoseVariant(i))
				{
					Frames->AddSlot() [ MakeFrame(i, /*bFrameB*/ false, /*bVertical*/ true) ]; // 2
					Frames->AddSlot() [ MakeFrame(i, /*bFrameB*/ true,  /*bVertical*/ true) ]; // 3
				}
				FrameSwitchers.Add(Frames);
				return Frames;
			}
			FrameSwitchers.Add(nullptr);
			return MakeFrame(i, /*bFrameB*/ false);
		}();

		// Layout B (specs/tutorial-live-feedback.md): the live board shares the illustration card,
		// beside the drawing. Chosen over its own panel because the card's white is a controlled
		// ground — a dark board reads cleanly on it at any size, where a panel over moving water
		// needs a scrim to place, tune and fade — and because demo and response in one frame read
		// as cause and effect without the eye leaving the reading column. The card grows sideways
		// into empty water, so it costs no height, which the phone layout has none of.
		TSharedPtr<STutorialBoardGlyph> Glyph;
		TSharedPtr<SWidget> GlyphGroup;
		{
			const ETutorialBoardView GlyphView = (i == 0)
				? ETutorialBoardView::TailOn    // sideways: tail-on, so a lean is a roll
				: ETutorialBoardView::SideOn;   // fore/aft and pump: side-on, nose and surge visible

			TSharedPtr<STextBlock> Caption;
			GlyphGroup = SNew(SVerticalBox)
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SAssignNew(Glyph, STutorialBoardGlyph)
					.View(GlyphView)
					.LineColor(C_Panel)
					.SuccessColor(C_Coral)
				]
				+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 4, 0, 0)
				[
					SAssignNew(Caption, STextBlock)
					.Text(GlyphCaption(i, /*bEarned*/ false, /*bHalfWay*/ false))
					.Font(Regular(10))
					.ColorAndOpacity(FSlateColor(C_Slate))
				];
			GlyphCaptions.Add(Caption);
		}
		GlyphWidgets.Add(Glyph);
		GlyphGroups.Add(GlyphGroup);
		StepEarned.Add(false);
		StepVisitEarned.Add(false);
		StepReachedPos.Add(false);
		StepReachedNeg.Add(false);
		StepFlash.Add(0.0f);

		// Explicit sizes, no scale box. Deriving the halves from the *available* height (or wrapping
		// the lot in an SScaleBox) both produced a card far smaller than the layout study, because
		// the body area's reported height is not the generous space it looks like on screen. Fixed
		// sizes are legible on a phone and predictable to change.
		const float ArtW = BoxW;
		const float ArtH = BoxW / Aspect;
		const float GlyphW = ArtH * 0.82f;

		TSharedRef<SBox> Illo = SNew(SBox).HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				SNew(SBorder)
				.BorderImage(IlloCardBrush())
				.Padding(FMargin(12.0f))
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SBox).WidthOverride(ArtW).HeightOverride(ArtH) [ Content ]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(14, 6).VAlign(VAlign_Fill)
					[
						SNew(SBox).WidthOverride(1.0f)
						[
							SNew(SBorder).BorderImage(WhiteBrush())
							.BorderBackgroundColor(FSlateColor(FLinearColor(0, 0, 0, 0.12f)))
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SBox).WidthOverride(GlyphW).HeightOverride(ArtH)
						[
							GlyphGroup.ToSharedRef()
						]
					]
				]
			];

		// A pose-variant step's copy changes with its art, so its two text blocks are kept to be
		// rewritten by ApplyHoldPose. Every other step's text is set once and never touched.
		TSharedPtr<STextBlock> Title, Desc;

		TSharedRef<SWidget> Page = SNew(SVerticalBox)
			+ SVerticalBox::Slot().FillHeight(1.0f).HAlign(HAlign_Center).VAlign(VAlign_Center)
			[
				Illo
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0, 14, 0, 6)
			[
				SAssignNew(Title, STextBlock).Text(StepTitle(i, bVerticalPose)).Font(Bold(26)).ColorAndOpacity(FSlateColor(C_Foam))
				.ShadowOffset(TextShadow).ShadowColorAndOpacity(C_TextShadow)
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(SBox).MaxDesiredWidth(420.0f)
				[
					// Foam rather than the muted C_Slate: over sunlit water the muted tone dropped
					// below readable. The title still leads on size and weight, so the hierarchy
					// survives losing the colour difference.
					SAssignNew(Desc, STextBlock)
					.Text(StepDesc(i, bVerticalPose))
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
					.Font(Regular(19))
					.ColorAndOpacity(FSlateColor(C_Foam))
					.ShadowOffset(TextShadow)
					.ShadowColorAndOpacity(C_TextShadow)
				]
			];

		if (StepHasPoseVariant(i))
		{
			PoseTitles.Add(i, Title);
			PoseDescs.Add(i, Desc);
			PosePages.Add(i, Page);
		}
		return Page;
	}

	// ---------- button handlers ----------
	FReply OnStartClicked()            { RequestStart();        return FReply::Handled(); }
	FReply OnShowInstructionsClicked() { ShowInstructions();    return FReply::Handled(); }
	FReply OnReplayClicked()           { if (Hooks.OnReplay)      Hooks.OnReplay();      return FReply::Handled(); }
	FReply OnChangeBoardClicked()      { if (Hooks.OnChangeBoard) Hooks.OnChangeBoard(); return FReply::Handled(); }
	FReply OnAboutClicked()            { if (Hooks.OnAbout)       Hooks.OnAbout();       return FReply::Handled(); }

	FText BoardButtonText() const
	{
		const FString Name = Hooks.BoardLabel ? Hooks.BoardLabel() : FString();
		return FText::FromString(FString::Printf(TEXT("BOARD · %s"), *Name.ToUpper()));
	}
	FReply OnNextClicked()             { Next();                return FReply::Handled(); }
	FReply OnBackClicked()             { Prev();                return FReply::Handled(); }
	FReply OnSkipClicked()             { ShowMenu();            return FReply::Handled(); }
};

// ---- install / uninstall ---------------------------------------------------------------------
namespace
{
	struct FStartState
	{
		TSharedPtr<SWidget> Root;
		TSharedPtr<SStartTutorial> Widget;
	};
	TMap<UWorld*, TSharedPtr<FStartState>> GStartStates;
}

namespace StartTutorial
{
	void Install(UWorld* World, TFunction<void()> OnStart)
	{
		FHooks Hooks;
		Hooks.OnStart = MoveTemp(OnStart);
		Install(World, MoveTemp(Hooks));
	}

	void Install(UWorld* World, FHooks Hooks)
	{
		if (!World || GStartStates.Contains(World)) return;
		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!Viewport) return;

#if PLATFORM_ANDROID
		const float Scale = 2.0f; // match SurfTuningHUD / WaveRadar: phones ~2x pixel density
#else
		const float Scale = 1.0f;
#endif

		TSharedPtr<SStartTutorial> Widget;
		TSharedRef<SWidget> Root = SNew(SDPIScaler).DPIScale(Scale)
			[
				SAssignNew(Widget, SStartTutorial).Hooks(Hooks)
			];
		Widget->SetWorld(World);

		TSharedPtr<FStartState> State = MakeShared<FStartState>();
		State->Root = Root;
		State->Widget = Widget;

		Viewport->AddViewportWidgetContent(Root, /*ZOrder*/ 200);
		GStartStates.Add(World, State);

		UE_LOG(LogSurf, Display, TEXT("StartTutorial: Installed."));
	}

	void Uninstall(UWorld* World)
	{
		TSharedPtr<FStartState> State;
		if (!GStartStates.RemoveAndCopyValue(World, State) || !State.IsValid()) return;
		if (UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr)
		{
			if (State->Root.IsValid())
			{
				Viewport->RemoveViewportWidgetContent(State->Root.ToSharedRef());
			}
		}
		UE_LOG(LogSurf, Display, TEXT("StartTutorial: Uninstalled."));
	}

	bool IsInstalled(UWorld* World)
	{
		return World && GStartStates.Contains(World);
	}
}
