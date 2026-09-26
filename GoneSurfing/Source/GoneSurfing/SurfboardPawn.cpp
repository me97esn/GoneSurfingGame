// Copyright Epic Games, Inc. All Rights Reserved.

#include "SurfboardPawn.h"
#include "SurfLog.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "PhysicsEngine/BodyInstance.h"
#include "WeightDistribution.h"
#include "StateTriggerAutoPilot.h"
#include "SharedCalculations.h"
#include "FluidDynamics.h"
#include "Buoyancy.h"
#include "GridLODActor.h"
#include "EngineUtils.h"
#include "Engine/StaticMeshActor.h"
#include "WaveHeight.h"
#include "ParticleSystemsController.h"
#include "SprayController.h"
#include "SurferAnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "SurfDebug.h"
#include "SurfRails.h"
#include "SurfTuningHUD.h"
#include "SurfTuningSubsystem.h"
#include "TouchControlsOverlay.h"
#include "SurfBoards.h"
#include "BoardPanel.h"
#include "AboutPanel.h"
#include "WaveRadarHUD.h"
#include "ReplayOverlayHUD.h"
#include "StartTutorialOverlay.h"
#include "RideCueOverlay.h"
#include "RideScoreOverlay.h"
#include "RideHudOverlay.h"
#include "WipeoutPanel.h"
#include "StaminaBarOverlay.h"
#include "Blueprint/UserWidget.h"
#include "UObject/UObjectIterator.h"
#include "Sound/SoundBase.h"
#include "SensorProbeOverlay.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if !UE_BUILD_SHIPPING
// PC-only weight-input sensitivity, overridable from the console so the fore/aft feel can be dialled
// mid-session instead of through a rebuild. <= 0 means "use the pawn's own value", which the level
// instance may itself have overridden - which is exactly the kind of thing that is invisible until
// you print it, so GetEffectiveMouseWeightSensitivity() feeds the WEIGHT readout too.
// Defaults of 150 px per full deflection, matched across the two axes, deliberately override the
// level instance's 40 sideways / 400 fore/aft. That pairing made sideways a hair-trigger (a normal
// flick was three times full deflection, so it only ever read 0 or 1) and fore/aft unreachable
// (a full desk sweep bought a quarter of the range). Nothing about ride feel is judged on PC, so
// these are set for whoever is debugging rather than tuned.
static TAutoConsoleVariable<float> CVarMouseWeightSensX(
	TEXT("surf.input.mouseweight.sensx"), 150.0f,
	TEXT("Pixels of mouse travel per full sideways weight deflection. <=0 = use the pawn's value."));
static TAutoConsoleVariable<float> CVarMouseWeightSensY(
	TEXT("surf.input.mouseweight.sensy"), 150.0f,
	TEXT("Pixels of mouse travel per full fore/aft weight deflection. <=0 = use the pawn's value."));
#endif

// Off by default since 2026-09-09: the camera moved far enough back that the wave face and the
// break are readable in the scene itself, which is what the radar existed to convey. A HUD element
// you have to look away from the wave to read is a poor trade once the wave itself shows you the
// same thing. The code stays - this is a CVar, not a deletion - because the camera decision is not
// necessarily final. surf.hud.radar 1 brings it back live, mid-ride.
static TAutoConsoleVariable<int32> CVarWaveRadar(
	TEXT("surf.hud.radar"), 0,
	TEXT("Wave-radar HUD: 0 = off (default), 1 = on. Gates the pawn's bShowWaveRadar."));

// The touch controls on desktop, where there is no touchscreen: the mouse drives them instead.
// ON by default (1) so PIE plays the same controls the phone does - that is the whole point of
// moving steering onto a joystick, and a desktop session that exercises a different input path
// teaches the wrong lessons about feel. 0 restores the old RMB + mouse-delta weight path.
// On Android this is ignored; there the controls follow the WeightInputSource scheme.
static TAutoConsoleVariable<int32> CVarForceTouchUI(
	TEXT("surf.input.touchui"), 1,
	TEXT("Desktop: 1 = touch controls, driven by the mouse (default). 0 = the old RMB weight path. ")
	TEXT("2 = also bypass the ride-state gates, so the layout can be screenshotted before handoff."));

bool ASurfboardPawn::WantsTouchControls() const
{
	// Scheme first: 1 = joystick (default), 0 = tilt. Live-switchable from the tuning HUD.
	const bool bJoystickScheme = !Tuning || Tuning->WeightInputSource >= 0.5f;
	if (!bJoystickScheme)
	{
		return false;
	}

#if !PLATFORM_ANDROID
	if (CVarForceTouchUI.GetValueOnGameThread() == 0)
	{
		return false;   // desktop keeps its mouse path unless explicitly asked otherwise
	}
#endif

	// A headless test run enables player controls after the autopilot hands off. Touch controls
	// must not install there, or a stray input would rewrite the trajectory a snapshot CSV records.
	if (!SurfDebug::CVarAutopilots.GetValueOnGameThread().IsEmpty() || bExternalWeightOverride)
	{
		return false;
	}

	// Layout check: force mode shows the controls before the ride starts, which is the only way to
	// screenshot their placement without a human pressing Start. Deliberately after the test-run
	// gate above, so it can never contaminate a snapshot run.
	if (CVarForceTouchUI.GetValueOnGameThread() >= 2)
	{
		return true;
	}

	if (bReplayActive)
	{
		return false;
	}

	// The hub shows the controls too - at rest, deaf, dimmed - so the player has read what the two
	// thumbs do before START, not at the handoff (specs/two-screen-navigation.md FR1a). Not while
	// the instruction cards are up over it: they draw their own illustrations where the rings go.
	// The modals that open over the hub (rack, ride list, about) need no case of their own: their
	// 0.90 scrims sit above the controls and take them out with everything else.
	if (bStartScreenActive)
	{
		return !bStartInstructionsView;
	}

	// Nothing else may own the screen: no replay, no board picker - EXCEPT the ride-end card. The
	// controls stay up under it, inert and dimmed by its scrim, because their captions are the
	// ride screen's only instructions and the end card is the first moment the player has time to
	// read them (they used to vanish the instant the ride ended). The card is the only modal that
	// can be up while bFallen, so "blocked but fallen" means exactly that.
	// NOTE this deliberately does NOT require bPlayerControlsEnabled - see UpdateTouchControls.
	return !bRideUIBlocked || bFallen;
}

// ======================================================================================
//  Ride HUD (‹ BACK) and the wipeout card — see specs/two-screen-navigation.md
// ======================================================================================

bool ASurfboardPawn::WantsRideHud() const
{
	// Same test-run gate as the start screen and the touch controls: a scripted run never draws it.
	if (FApp::IsUnattended() || bExternalWeightOverride
		|| !SurfDebug::CVarAutopilots.GetValueOnGameThread().IsEmpty())
	{
		return false;
	}
	if (!Cast<APlayerController>(Controller))
	{
		return false;
	}
	// Unlike the touch controls this stays up through a replay (it is the exit) and after a fall
	// (until the card takes over, which sets bRideUIBlocked like every other modal).
	return !bStartScreenActive && !bRideUIBlocked;
}

void ASurfboardPawn::UpdateRideHud()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const bool bWant = WantsRideHud() && !bReloadPending;
	if (bWant && !RideHud::IsInstalled(World))
	{
		TWeakObjectPtr<ASurfboardPawn> WeakThis(this);
		RideHud::Install(World, [WeakThis]()
		{
			if (ASurfboardPawn* Self = WeakThis.Get())
			{
				Self->ReturnToHub();
			}
		});
	}
	else if (!bWant && RideHud::IsInstalled(World))
	{
		RideHud::Uninstall(World);
	}
}

void ASurfboardPawn::PurgeLegacyRideBar()
{
	if (bLegacyRideBarChecked)
	{
		return;
	}
	// First tick, not BeginPlay: the level Blueprint that used to spawn the bar does so in its own
	// BeginPlay, and the order between the two is not something to lean on.
	bLegacyRideBarChecked = true;

	UWorld* World = GetWorld();
	for (TObjectIterator<UUserWidget> It; It; ++It)
	{
		UUserWidget* Widget = *It;
		if (!Widget || Widget->GetWorld() != World || !Widget->IsInViewport())
		{
			continue;
		}
		if (Widget->GetClass()->GetName().StartsWith(TEXT("WBP_SurfboardControls")))
		{
			// NFR2: loud and one-layer. A UMG bar live under the Slate HUD means one of the two
			// silently stops taking taps, which has bitten this project more than once.
			UE_LOG(LogSurf, Warning,
				TEXT("SurfboardPawn: %s is still spawned by the level - removing it. Delete its Create Widget / Add to Viewport nodes from the level Blueprint (specs/two-screen-navigation.md, Editor step)."),
				*Widget->GetClass()->GetName());
			Widget->RemoveFromParent();
		}
	}
}

void ASurfboardPawn::UpdateWipeoutCard(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World || !bFallen)
	{
		FallElapsedSeconds = 0.0f;
		return;
	}
	// Both ways off the card close it and then reload; during the fade that follows, bFallen with
	// no card open is exactly the state that would open one.
	if (WipeoutPanel::IsOpen(World) || bReloadPending)
	{
		return;
	}
	// FR7: never in a scripted run. Fall detection itself is gated only on the autopilot FILTER, so
	// an unfiltered headless run (RunGameAndCollectLogs with no args) can still wipe out in the tail
	// after its recording has flushed - seen 2026-09-14 - and must not open a card there. Same
	// -unattended gate the start screen and the Back pill use.
	if (FApp::IsUnattended() || bExternalWeightOverride
		|| !SurfDebug::CVarAutopilots.GetValueOnGameThread().IsEmpty())
	{
		return;
	}

	// A beat first. The card is what makes the stop read as intended; a card that slams up over
	// the fall would take that away, and the tumble is worth a second of the player's attention.
	// On the authored fall the beat starts when the clip ENDS - the rider must be in the water
	// before the card says so - which is the longer of the two for any clip over ~0.6 s.
	FallElapsedSeconds += DeltaTime;
	float CardDelay = kWipeoutCardDelaySeconds;
	if (bRiderFallAnimated)
	{
		if (const USurferAnimInstance* RiderAnim = ResolveRiderAnim())
		{
			CardDelay = FMath::Max(CardDelay, RiderAnim->GetFallClipLength() + kWipeoutCardBeatAfterClipSeconds);
		}
	}
	if (FallElapsedSeconds < CardDelay)
	{
		return;
	}

	// Same numbers the counter shows, in the same units (UpdateRideScore): the ride's credit plus
	// its tricks through the board multiplier, and the banked best through the same call.
	const float BoardMult = GetBoardScoreMultiplier();
	const int32 Score = TrickScore::ScoreShown(AssistRideCreditSeconds, TrickState.TrickCreditSeconds, BoardMult);
	const int32 Best  = TrickScore::ScoreShown(AssistBestRideSeconds, 0.0f, BoardMult);

	TWeakObjectPtr<ASurfboardPawn> WeakThis(this);
	WipeoutPanel::FHooks Hooks;
	Hooks.SurfAgain = [WeakThis]()
	{
		if (ASurfboardPawn* Self = WeakThis.Get())
		{
			Self->RestartLevel();
		}
	};
	Hooks.Back = [WeakThis]()
	{
		if (ASurfboardPawn* Self = WeakThis.Get())
		{
			Self->ReturnToHub();
		}
	};

	// A modal like the rack and the list: the Back pill stands down while it is up (NFR1). No
	// OnClosed hook to put it back - both ways off the card reload the level.
	bRideUIBlocked = true;
	UpdateRideHud();
	RideScore::Hide(World);
	// One title for every ending (owner, 2026-09-22): WIPEOUT / LOST THE WAVE each described a
	// specific ending that was not always what had happened, and anything warmer would need a
	// judgement of whether the ride was good. RIDE OVER is true every time. RideEndKind still says
	// which in the log and the trace.
	const FText Title = NSLOCTEXT("GoneSurfing", "RideOverTitle", "RIDE OVER");
	WipeoutPanel::Open(World, Title, Score, Best, Hooks);
	// The other half of FR4: the card actually came up. A ride_end line with no card line after it
	// is a card that never showed.
	AppendTraceNote(FString::Printf(TEXT("card=%s after=%.2fs score=%d"), *Title.ToString(), FallElapsedSeconds, Score));
}

void ASurfboardPawn::UpdateTouchControls(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const bool bWant = WantsTouchControls();
	if (bWant != TouchControls::IsInstalled(World))
	{
		if (bWant)
		{
			TouchControls::Install(World);
		}
		else
		{
			TouchControls::Uninstall(World);
		}

		// Say WHY on every transition. The controls are gated on five separate pieces of ride state,
		// and "they are not on screen" is otherwise indistinguishable from "the widget is broken" -
		// which cost a device session already.
		UE_LOG(LogSurf, Display,
			TEXT("TouchControls: %s (scheme=%s controls=%d startScreen=%d replay=%d rideUI=%d fallen=%d test=%d)"),
			bWant ? TEXT("shown") : TEXT("hidden"),
			(!Tuning || Tuning->WeightInputSource >= 0.5f) ? TEXT("joystick") : TEXT("tilt"),
			bPlayerControlsEnabled ? 1 : 0, bStartScreenActive ? 1 : 0, bReplayActive ? 1 : 0,
			bRideUIBlocked ? 1 : 0, bFallen ? 1 : 0,
			SurfDebug::CVarAutopilots.GetValueOnGameThread().IsEmpty() ? 0 : 1);
	}

	if (!bWant)
	{
		bTouchControlsDriving = false;
		bTouchStickHeld       = false;
		bTouchStickLatched    = false;
		bPumpButtonHeld       = false;
		return;
	}

	TouchControls::FTuning T;
	T.FullDeflectionPx  = Tuning ? Tuning->StickFullDeflectionPx     : 90.0f;
	T.DeadzoneFrac      = Tuning ? Tuning->StickDeadzoneFrac         : 0.08f;
	T.ForeAftReturnSecs = Tuning ? Tuning->StickForeAftReturnSeconds : 0.0f;
	T.bMirror           = Tuning && Tuning->MirrorTouchControls >= 0.5f;
	T.StickRadiusFrac   = Tuning ? Tuning->TouchStickRadiusFrac : 0.25f;
	T.PumpRadiusFrac    = Tuning ? Tuning->TouchPumpRadiusFrac  : 0.25f;
	T.HomeYFrac         = Tuning ? Tuning->TouchControlsYFrac   : 0.5f;
	T.Hint              = Tuning ? FMath::RoundToInt(Tuning->TouchControlsHint) : 2;
	T.RestArt           = Tuning ? Tuning->TouchControlsRestArt  : 0.22f;
	T.RestText          = Tuning ? Tuning->TouchControlsRestText : 0.85f;
	T.bLatch            = !Tuning || Tuning->StickLatch >= 0.5f;
	T.bPumpGhost        = !Tuning || Tuning->TouchPumpStyle >= 0.5f;
	TouchControls::SetTuning(World, T);

	// Installed through the intro, live only once the player actually has the board. Keeping the
	// widget alive while inert is the point: a thumb that lands during the pop-up and is still down
	// at the handoff starts steering immediately, because its pad has been tracking it the whole
	// time. Rebuilding the widget at the handoff instead loses that finger - a touch already down
	// generates no new touch-started event - which is why the stick used to do nothing until it was
	// released and pressed again.
	const bool bLive = (bPlayerControlsEnabled && !bStartScreenActive)
		|| CVarForceTouchUI.GetValueOnGameThread() >= 2;
	TouchControls::SetActive(World, bLive);
	// Ride over, or on the hub: drawn at rest, deaf to the glass (see WantsTouchControls).
	TouchControls::SetInert(World, bFallen || bStartScreenActive);
	// "Not yet": half strength on the hub and through the intro, full strength from the handoff,
	// so coming up to full is the signal that control has arrived (with the "...and surf!" cue).
	// Not under the end card - its scrim does the dimming there, and a second dose on top of it
	// would take the captions below readable.
	TouchControls::SetDimmed(World, !bLive && !bFallen);

	// Fore/aft self-centring runs on the game's clock, not Slate's, so it stops with the world.
	TouchControls::Advance(World, DeltaTime);

	const TouchControls::FState S = TouchControls::GetState(World);
	bTouchControlsDriving = bLive;
	bTouchStickHeld       = S.bStickHeld;
	bTouchStickLatched    = S.bStickLatched;
	// ORed with SetPumpHeld below, so a UMG pump button (if one still exists) keeps working
	// alongside the overlay's own.
	bPumpButtonHeld       = S.bPumpHeld;

	if (S.bStickHeld)
	{
		CurrentWeightOffset.X = FMath::Clamp(S.WeightOffset.X, -1.0f, 1.0f);
		CurrentWeightOffset.Y = FMath::Clamp(S.WeightOffset.Y, -1.0f, 1.0f);
	}
	else if (S.bStickLatched)
	{
		// Thumb up, command kept (StickLatch): lean and trim both stay where the drag left them.
		CurrentWeightOffset.X = FMath::Clamp(S.WeightOffset.X, -1.0f, 1.0f);
		CurrentWeightOffset.Y = FMath::Clamp(S.WeightOffset.Y, -1.0f, 1.0f);
	}
}

// A metronome on the pump button, for headless capture. The pump is a Slate pad and the ride
// holds mouse capture, so there is otherwise no way to press it without a human thumb - see
// ASurfboardPawn::SurfPump. Period 0 = off, which is every run that does not ask for it.
static TAutoConsoleVariable<float> CVarPumpAutoPeriod(
	TEXT("surf.pump.auto"),
	0.0f,
	TEXT("Seconds between scripted pump strokes once the player has the board. 0 = off."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarPumpAutoHold(
	TEXT("surf.pump.autohold"),
	0.7f,
	TEXT("How long surf.pump.auto holds the button each stroke, seconds."),
	ECVF_Default);

void ASurfboardPawn::SurfPump(float HoldSeconds, float DelaySeconds)
{
	PumpScriptHoldRemaining  = FMath::Max(0.0f, HoldSeconds);
	PumpScriptDelayRemaining = FMath::Max(0.0f, DelaySeconds);
	UE_LOG(LogSurf, Display, TEXT("SurfPump: holding the pump for %.2f s, starting in %.2f s"),
		PumpScriptHoldRemaining, PumpScriptDelayRemaining);
}

bool ASurfboardPawn::TickScriptedPump(float DeltaTime)
{
	// Runs on the same clock as the real button, right where the button is read, so a scripted
	// stroke and a thumbed one are the same stroke as far as everything downstream is concerned.
	if (PumpScriptDelayRemaining > 0.0f)
	{
		PumpScriptDelayRemaining -= DeltaTime;
		return false;
	}
	if (PumpScriptHoldRemaining > 0.0f)
	{
		PumpScriptHoldRemaining -= DeltaTime;
		// The tick it runs out returns false, which IS the release - no separate edge to get wrong.
		return PumpScriptHoldRemaining > 0.0f;
	}

	// surf.pump.auto: a stroke every Period seconds of live control, starting at the handoff.
	// Read every tick rather than latched, so it can be turned on mid-session over RemoteControl.
	const float Period = CVarPumpAutoPeriod.GetValueOnGameThread();
	if (Period > 0.0f)
	{
		PumpAutoElapsed += DeltaTime;
		const float Hold = FMath::Clamp(CVarPumpAutoHold.GetValueOnGameThread(), 0.05f, Period * 0.9f);
		return FMath::Fmod(PumpAutoElapsed, Period) < Hold;
	}
	PumpAutoElapsed = 0.0f;
	return false;
}

void ASurfboardPawn::UpdateHeldPump(float DeltaTime)
{
	const float ChargeSecs  = Tuning ? FMath::Max(Tuning->PumpChargeSeconds, 0.01f)  : 0.5f;
	const float ReleaseSecs = Tuning ? FMath::Max(Tuning->PumpReleaseSeconds, 0.01f) : 0.35f;
	const float MinCharge   = Tuning ? FMath::Clamp(Tuning->PumpMinCharge, 0.0f, 1.0f) : 0.15f;
	const float RampSecs    = Tuning ? FMath::Max(Tuning->PumpLateralRampSeconds, 0.01f) : 0.2f;

	// A pump is a crouch and a rise. Holding the button crouches; RELEASING extends the legs and
	// drives the board down. Force belongs entirely to the extension - a crouch on its own does
	// nothing, which is why the moment of release is the whole skill: let it go entering a turn and
	// the drive lands where the rail can use it.
	const bool bWasReleasing = bPumpReleasing;

	if (bPumpButtonHeld)
	{
		PumpCharge = FMath::Min(1.0f, PumpCharge + DeltaTime / ChargeSecs);
	}
	else if (bPumpHeldLastTick && PumpCharge > 0.0f)
	{
		// Let go: everything stored goes into one extension. A twitch below MinCharge is discarded
		// rather than fired weakly, so a thumb brushing the button cannot pump.
		if (PumpCharge >= MinCharge)
		{
			PumpReleaseStrength = PumpCharge;
			PumpReleasePhase    = 0.0f;
			bPumpReleasing      = true;
		}
		PumpCharge = 0.0f;
	}

	if (bPumpReleasing)
	{
		PumpReleasePhase += DeltaTime / ReleaseSecs;
		if (PumpReleasePhase >= 1.0f)
		{
			PumpReleasePhase = 1.0f;
			bPumpReleasing   = false;
		}
	}

	// The impulse is shaped across the extension and scaled by what was stored: a half-held pump is
	// half a pump. Zero while crouching - the same loading-phase-only asymmetry the accelerometer
	// path produced, except the player now chooses when the loading phase happens.
	PumpInput = (bPumpReleasing || (bWasReleasing && PumpReleasePhase >= 1.0f))
		? PumpReleaseStrength * FMath::Sin(PI * FMath::Clamp(PumpReleasePhase, 0.0f, 1.0f))
		: 0.0f;

	bPumpHeldLastTick = bPumpButtonHeld;

	// Ease the sideways attenuation with the gesture as a whole (inert at the shipped default).
	const float RampTarget = (bPumpButtonHeld || bPumpReleasing) ? 1.0f : 0.0f;
	PumpLateralRamp = FMath::FInterpConstantTo(PumpLateralRamp, RampTarget, DeltaTime, 1.0f / RampSecs);

	if (SurfDebug::IsFlagSet(TEXT("pump")))
	{
		UE_LOG(LogSurf, Warning,
			TEXT("Pump: held=%d charge=%.3f releasing=%d relPhase=%.3f strength=%.3f PumpInput=%.3f"),
			bPumpButtonHeld ? 1 : 0, PumpCharge, bPumpReleasing ? 1 : 0,
			PumpReleasePhase, PumpReleaseStrength, PumpInput);
	}
}

bool ASurfboardPawn::WantsWaveRadar() const
{
	// AND, not OR, and deliberately so: the level instance has bShowWaveRadar checked, so a default
	// flip in C++ alone would not actually turn it off in the shipping level.
	return bShowWaveRadar && CVarWaveRadar.GetValueOnGameThread() != 0;
}

float ASurfboardPawn::GetEffectiveMouseWeightSensitivity(bool bForeAft) const
{
	float Sens = bForeAft ? MouseWeightSensitivityY : MouseWeightSensitivityX;
#if !UE_BUILD_SHIPPING
	const float Override = bForeAft
		? CVarMouseWeightSensY.GetValueOnGameThread()
		: CVarMouseWeightSensX.GetValueOnGameThread();
	if (Override > 0.0f)
	{
		Sens = Override;
	}
#endif
	// A zero or negative sensitivity would divide the offset to infinity on the first mouse move.
	return FMath::Max(Sens, 1.0f);
}

ASurfboardPawn::ASurfboardPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	// Create camera root (for stabilization)
	CameraRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CameraRoot"));
	RootComponent = CameraRoot;

	// Create camera component
	CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	CameraComponent->SetupAttachment(CameraRoot);
	CameraComponent->bUsePawnControlRotation = false;  // CRITICAL: Camera should NOT use controller rotation
	CameraComponent->SetActive(true);  // Ensure camera is active

	// Use controller rotation for camera (like TemporaryCameraPawn)
	bUseControllerRotationPitch = false;  // Keep pitch locked (no up/down)
	bUseControllerRotationYaw = true;     // Allow yaw rotation
	bUseControllerRotationRoll = false;   // Keep roll locked
}

void ASurfboardPawn::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
#if WITH_EDITOR
	// Editor-only convenience: place the camera at the selected preview config so the viewport
	// camera-preview shows the framing without entering PIE. Skipped during play (UpdateCameraTransform
	// owns the camera there). See specs/camera-bird-view-on-descent.md.
	if (GIsEditor && GetWorld() && !GetWorld()->IsGameWorld())
	{
		ApplyEditorCameraPreview();
	}
#endif
}

#if WITH_EDITOR
void ASurfboardPawn::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	// Re-snap the preview when any camera tunable (or the preview selector) changes.
	if (GIsEditor && GetWorld() && !GetWorld()->IsGameWorld())
	{
		ApplyEditorCameraPreview();
	}
}

void ASurfboardPawn::ApplyEditorCameraPreview()
{
	if (EditorCameraPreview == EEditorCameraPreview::Off || !SurfboardActor || !CameraComponent || !CameraRoot)
	{
		return;
	}

	UStaticMeshComponent* Mesh = SurfboardActor->GetStaticMeshComponent();
	if (!Mesh)
	{
		return;
	}
	const FVector BoardLocation = Mesh->GetComponentLocation();
	const float BoardYaw = Mesh->GetComponentRotation().Yaw;

	FVector CamLocation;
	FRotator CamRotation;

	if (EditorCameraPreview == EEditorCameraPreview::Beside)
	{
		// World-fixed: offset added in world space, rotation is absolute.
		CamLocation = BoardLocation + CameraOffsetSide;
		CamRotation = CameraRotationSide;
	}
	else // BehindChase or BehindBird — board-relative offset + board-following yaw.
	{
		const bool bBird = (EditorCameraPreview == EEditorCameraPreview::BehindBird);
		const FVector Offset = bBird ? CameraOffsetBird : CameraOffset;
		const FRotator RotOff = bBird ? CameraRotationBird : CameraRotationOffset;
		CamLocation = BoardLocation + FRotator(0.0f, BoardYaw, 0.0f).RotateVector(Offset);
		CamRotation = FRotator(RotOff.Pitch, BoardYaw + 90.0f + RotOff.Yaw, RotOff.Roll);
	}

	// Mirror runtime: CameraRoot owns the orientation, CameraComponent stays identity (no baked roll).
	CameraComponent->SetRelativeRotation(FRotator::ZeroRotator);
	CameraRoot->SetWorldLocation(CamLocation);
	CameraRoot->SetWorldRotation(CamRotation);
}
#endif

void ASurfboardPawn::BeginPlay()
{
	Super::BeginPlay();

	UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: BeginPlay - Platform: %s"),
		PLATFORM_ANDROID ? TEXT("Android") : TEXT("PC"));

	// CameraRoot fully owns the camera orientation (driven every tick in UpdateCameraTransform).
	// The child CameraComponent must therefore have an identity relative rotation — any baked
	// rotation on it composes with the root's pitch+yaw and leaks in as roll (tilted horizon).
	// Enforce identity here so an accidental editor rotation/pilot on the component can't drift it.
	if (CameraComponent)
	{
		CameraComponent->SetRelativeRotation(FRotator::ZeroRotator);
	}

	// Install the runtime-tuning HUD (TUNE gear button + panel, and the SEND button beside it)
	// into the viewport. Pure C++ Slate — no UMG asset required. See specs/runtime-tuning.md.
	// IsTuningUIAvailable(), not the raw flag: Shipping has no dev UI whatever the flag says.
	if (bRunSensorProbe)
	{
		SensorProbe::Install(GetWorld());
	}

	if (IsTuningUIAvailable())
	{
		SurfTuningHUD::Install(GetWorld());
	}

	// Install the wave-radar HUD (bottom-left). Pure C++ Slate, no scene capture. See specs/wave-radar.md.
	if (WantsWaveRadar())
	{
		WaveRadar::Install(GetWorld(), RadarSizePx);
	}

	Tuning = SurfTuning::Get(this);

	// The board is the difficulty setting (specs/board-selection.md). Applied here, before the ride
	// exists, so the physics never sees a mid-wave change of feel.
	//
	// Never in a scripted run: the snapshot baselines were recorded on compiled defaults, and a board
	// would silently retune the physics under them — the failure would present as a regression in a
	// test that had not changed.
	ApplyActiveBoard();

	// Validate surfboard reference
	if (!SurfboardActor)
	{
		UE_LOG(LogSurf, Error, TEXT("SurfboardPawn: No SurfboardActor assigned! Please assign in editor."));
	}
	else
	{
		UStaticMeshComponent* SurfboardMesh = SurfboardActor->GetStaticMeshComponent();
		if (!SurfboardMesh)
		{
			UE_LOG(LogSurf, Error, TEXT("SurfboardPawn: SurfboardActor has no StaticMeshComponent!"));
		}
		else
		{
			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: Surfboard reference valid"));
		}
	}

	// Validate WeightDistribution reference
	if (!WeightDistribution)
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: No WeightDistribution assigned! Weight control will not work."));
	}
	else
	{
		UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: WeightDistribution reference valid"));
	}

	// Continue with camera setup if surfboard is valid
	if (SurfboardActor)
	{
		UStaticMeshComponent* SurfboardMesh = SurfboardActor->GetStaticMeshComponent();
		if (SurfboardMesh)
		{

			// Initialize controller rotation based on starting camera position
			float InitialYaw;
			if (bUseBehindCamera)
			{
				// Camera Position 1: Face forward on the surfboard
				// Surfboard mesh is rotated 90 degrees, so add 90 to face the actual forward direction
				FRotator SurfboardRotation = SurfboardMesh->GetComponentRotation();
				InitialYaw = SurfboardRotation.Yaw + 90.0f;
			}
			else
			{
				// Camera Position 2: Use fixed world yaw direction
				InitialYaw = CameraRotationSide.Yaw;
			}

			if (APlayerController* PC = Cast<APlayerController>(Controller))
			{
				PC->SetControlRotation(FRotator(0.0f, InitialYaw, 0.0f));
			}

			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: Initialized camera, Mode=%s, Yaw=%.2f"),
				bUseBehindCamera ? TEXT("BEHIND") : TEXT("BESIDE"), InitialYaw);
		}
	}

	// Add Input Mapping Context
	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: PlayerController found"));

		// CRITICAL: Set this pawn as the view target so we use our camera
		PlayerController->SetViewTargetWithBlend(this, 0.0f);
		UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: Set view target to this pawn"));

		// Configure input mode
		ApplyGameplayInputMode(PlayerController);

		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: Enhanced Input Subsystem found"));

			if (InputMappingContext)
			{
				Subsystem->AddMappingContext(InputMappingContext, 0);
				UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: Added Input Mapping Context"));
			}
			else
			{
				UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: No InputMappingContext assigned! Assign it in Blueprint or Editor."));
			}

			// Log input action assignments
			if (IA_PaddleForward)
			{
				UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: IA_PaddleForward is assigned"));
			}
			else
			{
				UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: IA_PaddleForward is NOT assigned!"));
			}

			if (IA_Turn)
			{
				UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: IA_Turn is assigned"));
			}
			else
			{
				UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: IA_Turn is NOT assigned!"));
			}

			if (IA_Weight)
			{
				UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: IA_Weight is assigned"));
			}
			else
			{
				UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: IA_Weight is NOT assigned!"));
			}

			if (IA_WeightGate)
			{
				UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: IA_WeightGate is assigned"));
			}
			else
			{
				UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: IA_WeightGate is NOT assigned!"));
			}
		}
		else
		{
			UE_LOG(LogSurf, Error, TEXT("SurfboardPawn: Enhanced Input Subsystem NOT found!"));
		}
	}
	else
	{
		UE_LOG(LogSurf, Error, TEXT("SurfboardPawn: No PlayerController!"));
	}

	// Auto-resolve StateTriggerAutoPilot: when the manually-wired ref is missing
	// or points at a disabled autopilot (the duplicate-and-swap workflow), scan
	// for the first enabled AStateTriggerAutoPilot in the world. Without this
	// the pawn keeps the stale ref, sees bAutopilotActive=false from t=0, and
	// starts writing 0.5/0.5 into WeightDistribution every tick — clobbering
	// the new (enabled) autopilot's BP-side currentStep mirror. See
	// specs/surfboardpawn-autopilot-autoresolve.md.
	if (!StateTriggerAutoPilot || !StateTriggerAutoPilot->enabled)
	{
		AStateTriggerAutoPilot* PriorRef = StateTriggerAutoPilot;
		TArray<AActor*> Found;
		UGameplayStatics::GetAllActorsOfClass(GetWorld(),
			AStateTriggerAutoPilot::StaticClass(), Found);

		AStateTriggerAutoPilot* FirstEnabled = nullptr;
		int32 EnabledCount = 0;
		for (AActor* A : Found)
		{
			AStateTriggerAutoPilot* AP = Cast<AStateTriggerAutoPilot>(A);
			if (AP && AP->enabled)
			{
				++EnabledCount;
				if (!FirstEnabled) FirstEnabled = AP;
			}
		}

		if (FirstEnabled)
		{
			StateTriggerAutoPilot = FirstEnabled;
			UE_LOG(LogSurf, Display,
				TEXT("SurfboardPawn: StateTriggerAutoPilot auto-resolved %s -> %s (TestName='%s', %d enabled in world)"),
				PriorRef ? *PriorRef->GetName() : TEXT("(null)"),
				*FirstEnabled->GetName(), *FirstEnabled->TestName, EnabledCount);
			if (EnabledCount > 1)
			{
				UE_LOG(LogSurf, Warning,
					TEXT("SurfboardPawn: %d enabled StateTriggerAutoPilots found; picked %s - wire pawn explicitly if a different one should drive handoff."),
					EnabledCount, *FirstEnabled->GetName());
			}
		}
		else
		{
			UE_LOG(LogSurf, Warning,
				TEXT("SurfboardPawn: StateTriggerAutoPilot auto-resolve found no enabled autopilot in world; ref left as %s"),
				PriorRef ? *PriorRef->GetName() : TEXT("(null)"));
		}
	}

	// Make sure pawn ticks AFTER the autopilot (and any BP tick on its subclass
	// that mirrors currentStep into WeightDistribution). Without this, the
	// pawn's WeightDistribution write below could be clobbered the same frame
	// by the autopilot BP, depending on undefined intra-group tick order.
	if (StateTriggerAutoPilot)
	{
		AddTickPrerequisiteActor(StateTriggerAutoPilot);
	}

	// Symmetric guard: force WeightDistribution to tick AFTER the pawn so that
	// the BP-side calculateWeightTorque call (made from WeightDistributionBP's
	// own tick) reads the pawn's latest WD write rather than the autopilot's
	// (now-stale) mirror. Without this, sibling tick order in TG_PrePhysics is
	// implementation-defined — PC happens to pick pawn-then-WD, Android picks
	// WD-then-pawn, and the calc on Android reads pre-pawn-write values
	// (always 0.5/0.5 once the autopilot's last step is centered) so tilt has
	// no visible effect.
	if (WeightDistribution)
	{
		WeightDistribution->AddTickPrerequisiteActor(this);
	}

	// Initialize autopilot timing state
	InitAutoPilotState();

	// Offline capture of the menu's background loop. Takes over from the ride entirely and never
	// opens the menu — see TickStartScreenCapture.
	bCaptureZSweep = FParse::Param(FCommandLine::Get(), TEXT("CaptureZSweep"));
	bCaptureYawSweep = bCaptureZSweep || FParse::Param(FCommandLine::Get(), TEXT("CaptureYawSweep"));
	if (bCaptureYawSweep || FParse::Param(FCommandLine::Get(), TEXT("StartScreenCapture")))
	{
		bStartScreenCaptureActive = true;
		UE_LOG(LogSurf, Display, TEXT("SurfboardPawn: -StartScreenCapture — recording the menu background loop."));
		return;
	}

	// Kinematic-target velocity probe. Takes over from the ride entirely — see TickKinematicProbe.
	if (FParse::Param(FCommandLine::Get(), TEXT("KinematicProbe")))
	{
		bKinematicProbeActive = true;
		UE_LOG(LogSurf, Display, TEXT("SurfboardPawn: -KinematicProbe — testing kinematic-target velocity derivation."));
		return;
	}

	// -RecordIntro: capture the intro window (here through to handoff) as a rails source trace.
	// Mutually exclusive with playing rails back — recording a rails run would just re-record the
	// track it is already following.
	//
	// Tuning->RecordIntro is the same switch without a command line. It exists because this decision
	// is made here, in BeginPlay, so a console command can never reach it: by the time there is a
	// console to type into, rails have already started. Saved/TuningOverrides.json is read before
	// this point and survives an editor restart, which is what makes it usable from PIE.
	// See specs/deterministic-ride-handoff.md.
	const bool bRecordIntroTuning = (Tuning && Tuning->RecordIntro >= 0.5f);
	if (FParse::Param(FCommandLine::Get(), TEXT("RecordIntro")) || bRecordIntroTuning)
	{
		if (bRecordIntroTuning)
		{
			UE_LOG(LogSurf, Display,
				TEXT("SurfboardPawn: RecordIntro set in TuningOverrides — recording the intro and running it on physics."));
		}
		bRecordingIntroTrace = true;
		FParse::Value(FCommandLine::Get(), TEXT("RecordIntroSeconds="), IntroRecordSeconds);
		StartInputTrace();
		if (IntroRecordSeconds > 0.0f)
		{
			UE_LOG(LogSurf, Display,
				TEXT("SurfboardPawn: -RecordIntro — recording a fixed %.2fs window from BeginPlay."),
				IntroRecordSeconds);
		}
		else
		{
			UE_LOG(LogSurf, Display,
				TEXT("SurfboardPawn: -RecordIntro — recording from BeginPlay until player handoff."));
		}
	}

	// Scripted intro on rails, when a reference trace is configured for this level. Falls through
	// to the live trigger-driven intro on any problem, so an unconfigured or stale trace degrades
	// to today's behaviour rather than breaking the ride.
	//
	// -RailsTrace=<path> overrides the level property AND the test-run gate, so a headless A/B can
	// drive the rails against a known trace without editing the umap. Explicit opt-in: a filtered
	// test run that passes it is asking for rails on purpose.
	FString RailsOverride;
	if (FParse::Value(FCommandLine::Get(), TEXT("RailsTrace="), RailsOverride) && !RailsOverride.IsEmpty())
	{
		bRailsCommandLineOverride = true;
		UE_LOG(LogSurf, Display, TEXT("SurfboardPawn: -RailsTrace override -> %s"), *RailsOverride);
	}

	// -RailsUntil=<trace t>: hand over partway through the trace instead of at its end, which is what
	// turns a recording into an A/B fixture - everything up to here IS the recording, everything after
	// is physics under the tuning being tested. Only meaningful with -RailsTrace.
	// See specs/replay-rails-until.md.
	if (FParse::Value(FCommandLine::Get(), TEXT("RailsUntil="), RailsUntilSeconds))
	{
		if (!bRailsCommandLineOverride)
		{
			UE_LOG(LogSurf, Warning,
				TEXT("SurfboardPawn: -RailsUntil=%.2f ignored - it needs -RailsTrace=<path> to say which trace."),
				RailsUntilSeconds);
			RailsUntilSeconds = -1.0f;
		}
	}

	// Tuning->RailsDisable: run the intro on physics (the live autopilot) instead of the recorded
	// pose track, without clearing RailsIntroTrace off the pawn in the umap. -RailsTrace still wins,
	// so a headless A/B that explicitly asks for a trace still gets one.
	const bool bRailsDisabledByTuning =
		(Tuning && Tuning->RailsDisable >= 0.5f && !bRailsCommandLineOverride);
	if (bRailsDisabledByTuning)
	{
		UE_LOG(LogSurf, Display,
			TEXT("SurfboardPawn: RailsDisable set in TuningOverrides — intro runs on physics (live autopilot). ")
			TEXT("This needs an enabled AStateTriggerAutoPilot wired to the pawn, or there is no takeoff at all."));
	}

	const FString RailsSource = bRailsCommandLineOverride ? RailsOverride : RailsIntroTrace;
	if (!bRecordingIntroTrace && !bRailsDisabledByTuning && !RailsSource.IsEmpty())
	{
		StartRails(ResolveRailsTracePath(RailsSource));
	}

	// Pre-wave start screen + control tutorial (pauses until Start). No-op in test runs.
	MaybeShowStartScreen();

	// No start screen this load, so nothing will call OnStartScreenStart and the ride begins now —
	// fire the assist card here instead.
	//
	// This is the path that MATTERS, and hanging the card solely off the start screen missed it
	// completely: RestartLevel sets GSkipStartScreenOnNextLoad so a restart drops straight into the
	// ride, and a restart is how you reach the ride after earning a level drop. The celebration
	// therefore never appeared, while the first-play card worked because a genuine first load does
	// show the menu. Also covers a level with bShowStartScreen false.
	if (!bStartScreenActive)
	{
	}
}

// Survive the OpenLevel reload (statics persist in the module across level travel). RestartLevel
// sets the first so the reload drops straight into the ride instead of showing the Start screen
// again; ReturnToHub sets the second so an explicit Back reaches the hub even on a dev machine
// where StartScreenSkip would otherwise swallow it.
namespace
{
	bool GSkipStartScreenOnNextLoad = false;
	bool GWantStartScreenOnNextLoad = false;
}

void ASurfboardPawn::MaybeShowStartScreen()
{
	if (!bShowStartScreen)
	{
		return;
	}

	// Development convenience (specs/board-selection.md): drop straight into the ride. About the
	// developer, not the player - so it yields to the one thing the player can ask for explicitly:
	// Back. With the skip set, Back used to reload and land straight in a fresh wave, which read as
	// "Back restarts the wave" and left no way to the board rack (2026-09-15).
	const bool bWantHub = GWantStartScreenOnNextLoad;
	GWantStartScreenOnNextLoad = false;
	if (Tuning && Tuning->StartScreenSkip >= 0.5f && !bWantHub)
	{
		UE_LOG(LogSurf, Display, TEXT("SurfboardPawn: Start screen skipped (StartScreenSkip)."));
		return;
	}

	// A Restart asked to skip the menu — consume the request and drop straight into the ride.
	if (GSkipStartScreenOnNextLoad)
	{
		GSkipStartScreenOnNextLoad = false;
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Only for real interactive play. Never during snapshot/replay test runs — the overlay would
	// pause the world and starve the autopilot, corrupting CSVs. Same "is this a test run" gate as
	// UpdateFallDetection: bExternalWeightOverride covers -ReplayTrace; a non-empty surf.autopilots
	// filter covers snapshot/state-trigger test runs.
	// NOTE: the cvar check alone is a race the headless runner loses — surf.autopilots arrives
	// via -ExecCmds one frame AFTER this BeginPlay gate runs, so the menu installed, paused the
	// world, and starved the autopilot (600s timeout, no CSV). -unattended is on every
	// RunGameAndCollectLogs launch and FApp::IsUnattended() is set from process start, so it
	// closes the race for headless runs; the cvar check stays for belt-and-braces.
	if (FApp::IsUnattended() || bExternalWeightOverride || SurfDebug::CVarAutopilots.GetValueOnGameThread().Len() > 0)
	{
		return;
	}
	if (World->WorldType != EWorldType::Game && World->WorldType != EWorldType::PIE)
	{
		return;
	}
	if (!Cast<APlayerController>(Controller))
	{
		return;
	}

	OpenStartOverlay();
}

void ASurfboardPawn::ShowStartScreen()
{
	// Logged BEFORE the guard, on purpose. "Back does nothing" has two causes that look identical
	// from the deck - the tap never arrives, or it arrives and the guard below swallows it - and
	// the guard returns silently, so neither leaves a trace. This line splits them: no line at all
	// means the UMG binding never fired; a line reading controller=0 means it fired and was
	// dropped here.
	UE_LOG(LogSurf, Display,
		TEXT("SurfboardPawn: ShowStartScreen requested (world=%d controller=%d startScreen=%d rideUIBlocked=%d)"),
		GetWorld() ? 1 : 0, Cast<APlayerController>(Controller) ? 1 : 0,
		bStartScreenActive ? 1 : 0, bRideUIBlocked ? 1 : 0);

	// Explicit mid-game request (e.g. a "Back to instructions" UI button). Bypasses the
	// test/first-load gates in MaybeShowStartScreen — a UI tap is always a real player action.
	if (!GetWorld() || !Cast<APlayerController>(Controller))
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: ShowStartScreen dropped - no world or no PlayerController."));
		return;
	}
	OpenStartOverlay();
}

void ASurfboardPawn::OpenStartOverlay()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// Freeze the board / autopilot / physics behind the menu until the player taps Start.
	UGameplayStatics::SetGamePaused(World, true);
	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		PC->SetShowMouseCursor(true);
		PC->SetInputMode(FInputModeGameAndUI());
	}

	// The menu's background is a recorded loop (see StartTutorialOverlay.cpp), so nothing in the
	// world needs to keep running behind it — the overlay covers the viewport completely.
	//
	// The radar is a live readout of a ride that hasn't started. Hide it for the duration, restored
	// on Start by the same call BeginPlay makes.
	WaveRadar::Uninstall(World);

	// The hub (specs/two-screen-navigation.md FR1): Start, plus the two things a player does between
	// waves - watch a ride, pick a board. Both open their panel on the modal tier over this menu;
	// the menu is Slate, so there is no UMG-under-Slate layer for them to fight (NFR1).
	TWeakObjectPtr<ASurfboardPawn> WeakThis(this);
	StartTutorial::FHooks Hooks;
	Hooks.OnStart = [WeakThis]()
	{
		if (ASurfboardPawn* Self = WeakThis.Get())
		{
			Self->OnStartScreenStart();
		}
	};
	Hooks.OnReplay = [WeakThis]()
	{
		if (ASurfboardPawn* Self = WeakThis.Get())
		{
			Self->OpenRideList();
		}
	};
	// Evaluated once, not per frame: the menu polls this for the button's visibility every tick, and
	// HasWatchableRides reads the ride records off disk. Nothing can add a ride while the hub is
	// up - leaving it for a replay reloads the level, which rebuilds the hub.
	const bool bHasRides = HasWatchableRides();
	Hooks.HasRides = [bHasRides]() { return bHasRides; };
	Hooks.OnChangeBoard = [WeakThis]()
	{
		if (ASurfboardPawn* Self = WeakThis.Get())
		{
			Self->OpenBoardPicker();
		}
	};
	Hooks.BoardLabel = [WeakThis]() -> FString
	{
		const ASurfboardPawn* Self = WeakThis.Get();
		return Self ? Self->GetActiveBoardName() : FString();   // empty = no boards = no button
	};
	Hooks.OnAbout = [WeakThis]()
	{
		if (ASurfboardPawn* Self = WeakThis.Get())
		{
			Self->OpenAbout();
		}
	};
	// The instruction cards draw their own illustrations where the resting controls sit, so the
	// controls step aside while a card is up. Fires from the widget's own view switch, because the
	// world is paused under the hub and nothing here ticks to notice.
	Hooks.OnInstructionsView = [WeakThis](bool bShowing)
	{
		if (ASurfboardPawn* Self = WeakThis.Get())
		{
			Self->bStartInstructionsView = bShowing;
			Self->UpdateTouchControls(0.0f);
		}
	};
	// Flags first: the -ShowInstructions launch form opens straight on the cards and fires the hook
	// above from inside Install, and the hook's UpdateTouchControls reads them.
	bStartScreenActive = true;
	bRideUIBlocked = true;
	bStartInstructionsView = false;
	StartTutorial::Install(World, MoveTemp(Hooks));

	// Hide the ride HUD while the menu is up. (The flag also used to hide the UMG bar's buttons.)
	UpdateRideHud();

	// Raise the touch controls at rest over the hub (FR1a). Explicitly, because the pause above
	// stops the pawn ticking and UpdateTouchControls would otherwise first run at Start - which is
	// exactly the moment the player used to first see the controls and had no time to read them.
	UpdateTouchControls(0.0f);
	OnStartScreenOpened.Broadcast();
}

void ASurfboardPawn::TickStartScreenCapture(float DeltaTime)
{
	// Records one full wave period from a parked camera, for the menu's pre-blurred frame loop.
	// Deliberately runs with the world UNPAUSED: Niagara does not simulate in a paused world, so a
	// paused capture would record moving water with no white water on it — the exact limit that
	// made the live-background approach a dead end.
	if (!bCaptureInitialised)
	{
		bCaptureInitialised = true;

		// Find the wave clock and the foam controller (the menu path caches these when it opens;
		// capture mode never opens the menu, so do it here).
		for (TActorIterator<AGridLODActor> It(GetWorld()); It; ++It)
		{
			if (AActor* WC = It->WaterController.Get())
			{
				CachedWaveController = WC;
				break;
			}
		}
		for (TActorIterator<AParticleSystemsController> It(GetWorld()); It; ++It)
		{
			CachedFoamController = *It;
			break;
		}

		AActor* WC = nullptr;
		if (!GetWaveFrameProperty(WC))
		{
			UE_LOG(LogSurf, Error, TEXT("StartScreenCapture: no WaterController CurrentFrame — aborting."));
			bStartScreenCaptureActive = false;
			return;
		}

		auto ReadInt = [WC](const TCHAR* Name, int32& Out) -> bool
		{
			if (FIntProperty* P = CastField<FIntProperty>(WC->GetClass()->FindPropertyByName(Name)))
			{
				Out = P->GetPropertyValue_InContainer(WC);
				return true;
			}
			return false;
		};
		if (!ReadInt(TEXT("StartFrame"), CaptureFirstFrame) || !ReadInt(TEXT("EndFrame"), CaptureLastFrame))
		{
			UE_LOG(LogSurf, Error, TEXT("StartScreenCapture: no Start/EndFrame — aborting."));
			bStartScreenCaptureActive = false;
			return;
		}

		// Hide the rider and board: they'd drift through the loop and break its seam, and the
		// background wants water, foam and island only.
		if (SurfboardActor)
		{
			SurfboardActor->SetActorHiddenInGame(true);
			if (UStaticMeshComponent* M = SurfboardActor->GetStaticMeshComponent())
			{
				M->SetSimulatePhysics(false);
			}
			TArray<AActor*> Attached;
			SurfboardActor->GetAttachedActors(Attached);
			for (AActor* A : Attached)
			{
				A->SetActorHiddenInGame(true);
			}
		}

		CaptureSettleTicks = 90;   // let the foam aim point and Niagara settle before framing freezes
		UE_LOG(LogSurf, Display, TEXT("StartScreenCapture: frames %d..%d stride %d -> %d images."),
			CaptureFirstFrame, CaptureLastFrame, CaptureFrameStride,
			(CaptureLastFrame - CaptureFirstFrame + 1) / FMath::Max(1, CaptureFrameStride));
	}

	if (!bStartScreenCaptureActive)
	{
		return;
	}

	// Phase 1: let the camera settle onto the foam, then freeze that transform for the whole loop.
	if (CaptureSettleTicks > 0)
	{
		PoseStartScreenCamera();
		if (--CaptureSettleTicks == 0 && CameraRoot)
		{
			CaptureCameraTransform = CameraRoot->GetComponentTransform();
			UE_LOG(LogSurf, Display, TEXT("StartScreenCapture: camera parked at %s."),
				*CaptureCameraTransform.GetLocation().ToString());
		}
		return;
	}

	// Framing sweep (-CaptureYawSweep): one image per yaw offset, wave held still. Finding a framing
	// that hides the seams between GridLodActors is otherwise one build-and-run per guess; this
	// covers the whole arc in a single run. Pick a value, set StartScreenCameraYawOffsetDeg, then
	// capture the loop for real.
	if (bCaptureYawSweep)
	{
		const int32 SweepCount = 96;
		if (CaptureIndex >= SweepCount)
		{
			UE_LOG(LogSurf, Display, TEXT("StartScreenCapture: yaw sweep DONE (%d images)."), SweepCount);
			bStartScreenCaptureActive = false;
			return;
		}

		if (bCaptureZSweep)
		{
			// Camera height. Lower grazes the surface, so nearer water occludes the far tile edges
			// — the fix for seams at both top-left and bottom-right at once.
			StartScreenCameraOffsetCm.Z = 700.0f - 7.0f * (float)CaptureIndex;
		}
		else
		{
			StartScreenCameraYawOffsetDeg = -48.0f + (float)CaptureIndex;
		}
		PoseStartScreenCamera();

		if (++CaptureHoldTicks < 3)
		{
			return;
		}
		CaptureHoldTicks = 0;

		const FString SweepName = bCaptureZSweep
			? FString::Printf(TEXT("z_%03d.png"), CaptureIndex)
			: FString::Printf(TEXT("yaw_%03d.png"), CaptureIndex);
		const FString SweepFile = FPaths::ProjectSavedDir() / TEXT("StartBgSweep") / SweepName;
		FScreenshotRequest::RequestScreenshot(SweepFile, false, false);
		++CaptureIndex;
		return;
	}

	// Phase 2: hold the camera still and step the wave one stride at a time, one image per step.
	if (CameraRoot)
	{
		CameraRoot->SetWorldTransform(CaptureCameraTransform);
	}

	AActor* WC = nullptr;
	FIntProperty* FrameProp = GetWaveFrameProperty(WC);
	if (!FrameProp)
	{
		return;
	}

	const int32 NumFrames = (CaptureLastFrame - CaptureFirstFrame + 1) / FMath::Max(1, CaptureFrameStride);
	if (CaptureIndex >= NumFrames)
	{
		UE_LOG(LogSurf, Display, TEXT("StartScreenCapture: DONE — %d images in %sStartBg/."),
			NumFrames, *FPaths::ProjectSavedDir());
		bStartScreenCaptureActive = false;
		return;
	}

	// Park the wave on this step's frame, then give Niagara a few ticks to consume the new
	// white-water points before the shutter — the foam trails the data by a frame or two.
	FrameProp->SetPropertyValue_InContainer(WC, CaptureFirstFrame + CaptureIndex * CaptureFrameStride);

	if (++CaptureHoldTicks < 5)
	{
		return;
	}
	CaptureHoldTicks = 0;

	const FString File = FPaths::ProjectSavedDir() / TEXT("StartBg") /
		FString::Printf(TEXT("startbg_%03d.png"), CaptureIndex);
	FScreenshotRequest::RequestScreenshot(File, /*bShowUI*/ false, /*bAddFilenameSuffix*/ false);
	++CaptureIndex;
}

void ASurfboardPawn::PoseStartScreenCamera()
{
	// The menu gets its own framing rather than borrowing the ride's Beside camera. That camera is
	// aimed by a fixed world yaw chosen for riding, which at the start pose points along flat water
	// with the wave out of frame — and it sits ~5cm *under* the surface, which renders as the flat
	// underside of the water mesh. Nudging its offsets was guesswork; standing at a known place
	// relative to the surfer and looking at them keeps the wave face in shot by construction.
	if (!CameraRoot || !SurfboardActor)
	{
		return;
	}

	UStaticMeshComponent* Mesh = SurfboardActor->GetStaticMeshComponent();
	if (!Mesh)
	{
		return;
	}

	FVector Target = Mesh->GetComponentLocation() + FVector(0.0f, 0.0f, StartScreenLookAtRiseCm);

	// Prefer the white-water cluster as the subject. Aiming at the board put the foam 27-39 degrees
	// off-axis (measured) because the break sits well down the line from where the board waits, so
	// the one thing worth looking at was out of shot. Smoothed: the centroid jumps a couple of
	// metres per wave frame and an unsmoothed target reads as camera shake.
	if (bStartScreenLookAtFoam)
	{
		if (const AParticleSystemsController* Foam = CachedFoamController.Get())
		{
			const TArray<FVector>& Pts = Foam->GetWhiteWaterWorldPoints();
			if (Pts.Num() > 0)
			{
				FVector Sum = FVector::ZeroVector;
				for (const FVector& P : Pts)
				{
					Sum += P;
				}
				const FVector Centroid = Sum / (double)Pts.Num();
				StartScreenFoamTarget = StartScreenFoamTarget.IsNearlyZero()
					? Centroid
					: FMath::VInterpTo(StartScreenFoamTarget, Centroid, FApp::GetDeltaTime(), 1.5f);
				Target = StartScreenFoamTarget + FVector(0.0f, 0.0f, StartScreenLookAtRiseCm);
			}
		}
	}

	const FVector CamPos = Target + StartScreenCameraOffsetCm;

	FRotator Look = (Target - CamPos).Rotation();
	Look.Yaw += StartScreenCameraYawOffsetDeg;
	Look.Pitch += StartScreenCameraPitchOffsetDeg;

	CameraRoot->SetWorldLocation(CamPos);
	CameraRoot->SetWorldRotation(Look);
}

FIntProperty* ASurfboardPawn::GetWaveFrameProperty(AActor*& OutController) const
{
	OutController = CachedWaveController.Get();
	if (!OutController)
	{
		return nullptr;
	}
	return CastField<FIntProperty>(OutController->GetClass()->FindPropertyByName(TEXT("CurrentFrame")));
}

void ASurfboardPawn::ApplyGameplayInputMode(APlayerController* PC) const
{
	if (!PC)
	{
		return;
	}

	// Cursor stays hidden in both branches so the PC RMB-gated mouse-weight path
	// (IA_WeightGate + CursorPositionBeforeLock) behaves identically either way.
	PC->SetShowMouseCursor(false);

	// The assist badge is a tappable viewport Slate widget, so it needs the same treatment the
	// tuning HUD's gear button needed — and unlike that one it is PLAYER-facing, so it cannot depend
	// on a dev flag being on. Without it in this condition the branch would never be taken in a
	// Shipping build (where IsTuningUIAvailable() is a constant false) and the badge would be
	// visible but dead in the shipped game.
	if (IsTuningUIAvailable() || bRunSensorProbe || IsAssistEnabledThisSession())
	{
		// GameOnly gives the viewport mouse/touch capture, and Slate widgets added via
		// AddViewportWidgetContent then stop receiving taps — which left the tuning HUD's
		// gear button visible but dead during the ride. GameAndUI keeps gameplay input
		// intact while letting the overlay receive input. See specs/runtime-tuning.md.
		FInputModeGameAndUI Mode;
		Mode.SetHideCursorDuringCapture(true);
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::LockOnCapture);
		PC->SetInputMode(Mode);
		UE_LOG(LogSurf, Display,
			TEXT("SurfAssist: input mode = GameAndUI (tuningHUD=%d sensorProbe=%d assistEnabled=%d). ")
			TEXT("Cursor is hidden: on DESKTOP the viewport keeps mouse capture, so viewport Slate ")
			TEXT("gets no hover/click. Touch on device is not affected."),
			IsTuningUIAvailable() ? 1 : 0, bRunSensorProbe ? 1 : 0, IsAssistEnabledThisSession() ? 1 : 0);
	}
	else
	{
		PC->SetInputMode(FInputModeGameOnly());
		UE_LOG(LogSurf, Display,
			TEXT("SurfAssist: input mode = GameOnly (tuningHUD=%d sensorProbe=%d assistEnabled=%d) ")
			TEXT("- viewport Slate will not receive taps."),
			IsTuningUIAvailable() ? 1 : 0, bRunSensorProbe ? 1 : 0, IsAssistEnabledThisSession() ? 1 : 0);
	}
}

void ASurfboardPawn::OnStartScreenStart()
{
	UWorld* World = GetWorld();
	StartTutorial::Uninstall(World);

	// Restore the in-game input mode the pawn normally uses (mirrors BeginPlay).
	ApplyGameplayInputMode(Cast<APlayerController>(Controller));

	if (World)
	{
		UGameplayStatics::SetGamePaused(World, false);
	}

	// Bring the radar back for the ride. Not during a replay — that mode hides it deliberately
	// and is terminal (see EnterReplayMode). This site used to skip the bShowWaveRadar check
	// entirely, so the radar reappeared at ride start even when it was switched off.
	if (World && !bReplayActive && WantsWaveRadar())
	{
		WaveRadar::Install(World, RadarSizePx);
	}

	// The ride has begun: bring the ride HUD (‹ BACK) up. The flags also used to drive the UMG
	// bar's visibility; the broadcast stays for any Blueprint still listening.
	bStartScreenActive = false;
	bRideUIBlocked = BoardPanel::IsOpen(GetWorld()) || AboutPanel::IsOpen(GetWorld());
	UpdateRideHud();
	OnStartScreenClosed.Broadcast();

	UE_LOG(LogSurf, Display, TEXT("SurfboardPawn: Start pressed — beginning the ride."));

	// The first-play / level-down card, fired HERE rather than from Tick.
	//
	// It used to watch for bStartScreenActive going true->false from Tick, which can never be seen:
	// the start screen pauses the world, so the pawn does not tick for the whole time the flag is
	// true, and by the time ticking resumes the transition has already happened. The card simply
	// never appeared on device. This is the actual moment — the player has tapped Start or Restart,
	// the wave has not begun, and nothing competes for their attention.
	//
	// Deliberately after SetGamePaused(false) above: the panel records whether IT was the thing that
	// paused, and opening while the start screen's pause was still in force would leave the game
	// paused after the card closed.
}

/** -BoardPicker has fired. Static, so it survives a restart: the flag is a DEV hook for taking a
 *  screenshot of the rack, and per-pawn it re-armed on every new wave - which on a handed-over
 *  device reads as the picker opening itself mid-ride, twice, for no reason the player can see.
 *  Once per launch is what "fires once" was always meant to say. */
static bool GSurfBoardPickerAutoOpened = false;

void ASurfboardPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// -BoardPicker: open the rack once, as soon as there is a viewport to open it into. A dev hook,
	// because the rack is otherwise only reachable by tapping, and a screenshot cannot tap.
	//
	// -BoardPickerDelay=<seconds> holds it off. Without that it fires before the autopilot has handed
	// over, so it always tests the BETWEEN-WAVES path where a board switch applies immediately - and
	// never the mid-ride path where it has to defer. Those are the two different behaviours worth
	// checking, and one of them was untestable.
	// Waits on the viewport directly. It used to wait on the C++ button having installed itself,
	// which was the same signal by accident - that button is gone, the UMG bar owns the control now.
	if (!GSurfBoardPickerAutoOpened && GetWorld() && GetWorld()->GetGameViewport()
		&& FParse::Param(FCommandLine::Get(), TEXT("BoardPicker")))
	{
		float Delay = 0.0f;
		FParse::Value(FCommandLine::Get(), TEXT("BoardPickerDelay="), Delay);
		BoardPickerDelayElapsed += DeltaTime;
		if (BoardPickerDelayElapsed >= Delay)
		{
			GSurfBoardPickerAutoOpened = true;
			UE_LOG(LogSurf, Display,
				TEXT("SurfboardPawn: -BoardPicker opening at %.1fs (riding=%d controls=%d fallen=%d startScreen=%d)"),
				BoardPickerDelayElapsed, (bPlayerControlsEnabled && !bFallen && !bStartScreenActive) ? 1 : 0,
				bPlayerControlsEnabled ? 1 : 0, bFallen ? 1 : 0, bStartScreenActive ? 1 : 0);
			OpenBoardPicker();
		}
	}

	// Offline background capture (-StartScreenCapture). Runs instead of the ride, world unpaused.
	if (bStartScreenCaptureActive)
	{
		TickStartScreenCapture(DeltaTime);
		return;
	}

	// Kinematic-target velocity probe (-KinematicProbe). Runs instead of the ride.
	if (bKinematicProbeActive)
	{
		TickKinematicProbe(DeltaTime);
		return;
	}

	// Scripted intro on rails. Drives the board from the recorded pose track and hands over to live
	// physics on the final row. Unlike the ride replay this does NOT return early: the force
	// pipeline, camera and overlays all keep running, because everything except the board's motion
	// should behave exactly as it does in a live intro. See specs/deterministic-ride-handoff.md.
	if (bRailsActive)
	{
		TickRails(DeltaTime);
	}

	// Kinematic ride replay short-circuits the whole live-input/physics path: drive the
	// board transform from the recorded trace, keep the camera following, and skip
	// autopilot/input/weight handling entirely. See specs/on-device-ride-replay.md.
	// The ride HUD's Back pill, and the safety net for the UMG bar it replaced. Before the replay
	// early-out on purpose: the pill is the replay's exit.
	PurgeLegacyRideBar();
	UpdateRideHud();

	if (bReplayActive)
	{
		TickReplay(DeltaTime);
		UpdateCameraTransform(DeltaTime); // Beside camera follows the board
		UpdateReplayOverlay();            // cinematic replay HUD (radar is hidden during replay)
		return;
	}

	// Update autopilot state and player control availability
	UpdatePlayerControlState(DeltaTime);

	// Wipeout detection (roll / lost planing). Arms only once the ride is underway;
	// after a fall this early-outs until Replay/Restart. See specs/surfer-fall-ragdoll.md.
	UpdateFallDetection(DeltaTime);

	// The energy budget: drains once the fall triggers have armed, ends the ride when empty.
	// See specs/stamina.md.
	UpdateStamina(DeltaTime);

	// The fallen rider, on the authored fall: carried by its momentum into the water's drift, on
	// the surface, while the card is up. See specs/surfer-fall-ragdoll.md, "Authored fall".
	UpdateFallenRider(DeltaTime);

	// The ride's ending: WIPEOUT, the score, SURF AGAIN / BACK. See specs/two-screen-navigation.md.
	UpdateWipeoutCard(DeltaTime);

	// Log controller rotation to see if it's interfering
	if (bEnableCameraLogging && Controller)
	{
		FRotator ControlRotation = Controller->GetControlRotation();
		static float ControlLogTimer = 0.0f;
		ControlLogTimer += DeltaTime;
		if (ControlLogTimer >= 1.0f)
		{
			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn::Tick - Controller Rotation: Yaw=%.2f"),
				ControlRotation.Yaw);
			ControlLogTimer = 0.0f;
		}
	}

	// Update camera to follow surfboard with stabilization
	// Must happen AFTER Super::Tick to ensure we override any base class transform changes
	UpdateCameraTransform(DeltaTime);

	// Refresh the wave-radar HUD snapshot (throttled internally).
	UpdateWaveRadar(DeltaTime);

	// Apply simulated paddle input for testing in simulation mode
	if (bSimulateForwardInput)
	{
		FInputActionValue FakeValue(1.0f);
		PaddleForward(FakeValue);
	}
	if (bSimulateLeftInput)
	{
		FInputActionValue FakeValue(-1.0f);
		Turn(FakeValue);
	}

	// Hand the probe our calibrated tilt basis when we happen to be ticking. Best-effort
	// only — the probe does its own sampling on a Slate timer precisely because this Tick
	// does not run while the start screen has the world paused.
	if (SensorProbe::IsActive(GetWorld()))
	{
		SensorProbe::SetTiltBasis(GetWorld(), TiltBasis.NeutralGravity, TiltBasis.ForwardAxis, TiltBasis.RightAxis);
	}

	// On-screen touch controls (joystick + pump button). Runs before the tilt poll: when the
	// joystick scheme is selected these own CurrentWeightOffset and tilt stands down.
	UpdateTouchControls(DeltaTime);

	// Android tilt mode: poll the sensor and write CurrentWeightOffset directly.
	// In this mode the "release" concept doesn't exist — the phone's pose IS
	// the input, so we skip the auto-center block below.
	bool bTiltActive = false;
	if (bExternalWeightOverride)
	{
		// Replay autopilot is driving CurrentWeightOffset; skip our own tilt poll
		// and the auto-center block (bTiltActive=true suppresses it).
		bTiltActive = true;
	}
#if PLATFORM_ANDROID
	else if (bPlayerControlsEnabled &&
		!bTouchControlsDriving &&
		AndroidWeightInputSource == EAndroidWeightInputSource::Tilt &&
		TiltBasis.bCalibrated)
	{
		UpdateTiltWeight();
		bTiltActive = true;
	}
#endif

	// Pump signal — Android reads accelerometer, PC reads space-bar (dev
	// fallback). Both gated on player controls enabled and skipped during
	// replay autopilots. Phase 0 TestPumpInput is read separately by
	// AWeightDistribution::Tick.
	if (bPlayerControlsEnabled && !bExternalWeightOverride)
	{
		// One pump implementation, several ways to hold it down. The touch button and the PC
		// space-bar both feed bPumpButtonHeld and go through UpdateHeldPump, so desktop testing
		// exercises the code the device actually runs. Only the Android tilt scheme still reads
		// the accelerometer, because there the shake gesture IS the pump.
#if PLATFORM_ANDROID
		const bool bHeldPumpScheme = bTouchControlsDriving;
#else
		const bool bHeldPumpScheme = true;
#endif
		if (bHeldPumpScheme)
		{
			// Every way of holding the pump down is ORed together: the overlay's own button (already
			// in bPumpButtonHeld), a UMG button via SetPumpHeld, and the PC space bar. The space bar
			// used to be read only when the overlay was NOT driving, which quietly killed desktop
			// pump testing the moment the touch controls became the desktop default.
#if !PLATFORM_ANDROID
			const APlayerController* PC = Cast<APlayerController>(Controller);
			const bool bSpaceHeld = PC && PC->IsInputKeyDown(EKeys::SpaceBar);
#else
			const bool bSpaceHeld = false;
#endif
			bPumpButtonHeld = bPumpButtonHeld || bPumpHeldFromUI || bSpaceHeld || TickScriptedPump(DeltaTime);
			UpdateHeldPump(DeltaTime);
		}
		else
		{
			UpdatePumpInput();
		}
	}
	else
	{
		PumpInput = 0.0f;
		PumpLowPassA = 0.0f;
		PumpLateralRamp = 0.0f;
		PumpCharge = 0.0f;
		PumpReleasePhase = 0.0f;
		PumpReleaseStrength = 0.0f;
		bPumpReleasing = false;
		bPumpHeldLastTick = false;
	}

	// Handle weight auto-center interpolation (stick + PC mouse paths only).
	if (!bTiltActive && !bWeightInputActive && !bTouchStickHeld && !CurrentWeightOffset.IsZero())
	{
		// Interpolate back to center - unless the touch stick is latched, in which case the
		// command is held as it is (the canvas re-asserts it each tick anyway).
		const FVector2D TargetOffset = bTouchStickLatched ? CurrentWeightOffset : FVector2D::ZeroVector;
		CurrentWeightOffset = FMath::Vector2DInterpTo(CurrentWeightOffset, TargetOffset, DeltaTime, 1.0f / WeightAutoCenterDuration);

		// Snap to zero when very close
		if ((CurrentWeightOffset - TargetOffset).Size() < 0.01f)
		{
			CurrentWeightOffset = TargetOffset;
		}
	}

	// Apply CurrentWeightOffset to WeightDistribution.
	// - Pre-handoff: leave autopilot's BP in charge (only write if the player
	//   somehow has a non-zero offset, which input handlers normally prevent).
	// - Post-handoff: write every tick so an autopilot BP still mirroring its
	//   (now-neutralized) currentStep into WeightDistribution can't overwrite
	//   the player's input.
	if (WeightDistribution && (bPlayerControlsEnabled || !CurrentWeightOffset.IsZero()))
	{
		// Map offset [-1, +1] to weight [0, 1]
		// X axis: left (-1) to right (+1) maps to amountToTheRight
		// Y axis: back (-1) to forward (+1) maps to amountInFront
		float WeightRight   = 0.5f + (CurrentWeightOffset.X * 0.5f);
		float WeightInFront = 0.5f + (CurrentWeightOffset.Y * 0.5f);

		// Cap how far forward the player can put their weight. A nose-heavy board is roll-dead - no
		// amount of leaning turns it - and players read that as the game ignoring them rather than
		// as physics. Below the ceiling the mapping is untouched, so the response stays proportional
		// to tilt. Applied to the mapped value rather than to CurrentWeightOffset so the input trace
		// still records what the player actually did - and applied before the assist, which is
		// exempt (see below).
		const float MaxInFront = GetEffectiveWeightMaxInFront();
		if (bTouchControlsDriving)
		{
			// Joystick: map the forward half of the travel INTO the ceiling rather than mapping past
			// it and clamping, so no part of the stick's range is inert. Below the ceiling the
			// response stays proportional; with the ceiling under 0.5 the forward half compresses to
			// nothing, which is the intent - the board cannot be put nose-heavy. Tilt cannot do this:
			// its neutral is wherever the phone is held, so the ceiling has to stay a clamp there.
			const float Y = CurrentWeightOffset.Y;
			WeightInFront = (Y >= 0.0f)
				? 0.5f + Y * (MaxInFront - 0.5f) * 2.0f
				: 0.5f + Y * 0.5f;
		}
		WeightInFront = FMath::Min(WeightInFront, MaxInFront);

		// Pumping trades turn for speed rather than taking the turn away: the player's commanded
		// lean is attenuated toward centre while pumping, ramped so entering and leaving a pump is
		// not a step. Applied to the player's command, before the assist, so the assist can still
		// correct a bad line mid-pump.
		if (PumpLateralRamp > 0.0f)
		{
			const float Retained = Tuning ? Tuning->PumpLateralAttenuation : 0.5f;
			const float Factor = FMath::Lerp(1.0f, FMath::Clamp(Retained, 0.0f, 1.0f), PumpLateralRamp);
			WeightRight = 0.5f + (WeightRight - 0.5f) * Factor;
		}

		// Gradual control handoff: the assist biases these two scalars and touches nothing else —
		// no forces, no coefficients (NFR2). Deliberately applied HERE rather than to
		// CurrentWeightOffset, for two reasons: the input trace keeps recording what the player
		// actually did, and a replayed trace can be run with the assist layered on top of it, which
		// is exactly what the assist_guard_* tests need. See specs/gradual-control-handoff.md.
		UpdateAssist(DeltaTime, WeightRight, WeightInFront);

		// The assist is deliberately NOT capped. Its trim targets (AssistTrimNeutral 0.5,
		// AssistTrimTargetMax 0.65) sit above the ceiling, so clamping its output here discarded
		// every forward correction it made and silently disabled one of its two channels - it could
		// no longer pull a tail-stalled board back onto its line. The ceiling is about what the
		// PLAYER can hold, and the assist's correction is already bounded by its own MaxAuthority
		// and clamped to [0,1] inside SurfAssist::Evaluate.
		WeightDistribution->amountToTheRight = WeightRight;
		WeightDistribution->amountInFront    = WeightInFront;
	}

	DrawWeightReadout();

	// Outside that block on purpose: pre-handoff the player's offset is zero, so the block does not
	// run, and the badge has to be up through the intro.
	UpdateRideScore();

	// Hand the live pump signal to the WeightDistribution, which applies the
	// downward impulse in its own Tick. See specs/pumping.md.
	if (WeightDistribution)
	{
		WeightDistribution->PumpInput = PumpInput;

		// The feedback channel, deliberately separate: PumpInput is about to be attenuated in place
		// for the force, often to zero, and the player must still see their pump happen.
		WeightDistribution->bPumpActive      = bPumpButtonHeld || bPumpReleasing;
		WeightDistribution->PumpCharge       = PumpCharge;
		WeightDistribution->bPumpReleasing   = bPumpReleasing;
		WeightDistribution->PumpReleasePhase = PumpReleasePhase;
		WeightDistribution->PumpReleaseFromCharge = PumpReleaseStrength;
		// Legacy single-value phase, kept for the input trace: the crouch occupies 0..1 and the
		// extension 1..2, so one float still describes the whole gesture.
		WeightDistribution->PumpStrokePhase  = bPumpReleasing ? (1.0f + PumpReleasePhase) : PumpCharge;
	}

	// Log current camera rotation periodically
	if (bEnableCameraLogging)
	{
		static float LogTimer = 0.0f;
		LogTimer += DeltaTime;
		if (LogTimer >= 1.0f)
		{
			FRotator CameraRotation = CameraRoot->GetComponentRotation();
			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn::Tick - Camera Yaw: %.2f, Mode: %s"),
				CameraRotation.Yaw, bUseBehindCamera ? TEXT("BEHIND") : TEXT("BESIDE"));
			LogTimer = 0.0f;
		}
	}

	// Append a row to the input trace if recording is active. Done last so we
	// capture the same CurrentWeightOffset that was just pushed to WeightDistribution.
	SampleInputTrace(DeltaTime);
}

void ASurfboardPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// Bind Enhanced Input actions
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		// Paddle forward
		if (IA_PaddleForward)
		{
			EnhancedInputComponent->BindAction(IA_PaddleForward, ETriggerEvent::Triggered, this, &ASurfboardPawn::PaddleForward);
			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: IA_PaddleForward bound"));
		}
		else
		{
			UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: No IA_PaddleForward assigned!"));
		}

		// Turn
		if (IA_Turn)
		{
			EnhancedInputComponent->BindAction(IA_Turn, ETriggerEvent::Triggered, this, &ASurfboardPawn::Turn);
			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: IA_Turn bound"));
		}
		else
		{
			UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: No IA_Turn assigned!"));
		}

		// Weight input
		if (IA_Weight)
		{
			EnhancedInputComponent->BindAction(IA_Weight, ETriggerEvent::Triggered, this, &ASurfboardPawn::WeightInput);
			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: IA_Weight bound"));
		}
		else
		{
			UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: No IA_Weight assigned!"));
		}

		// Weight gate (PC only)
		if (IA_WeightGate)
		{
			EnhancedInputComponent->BindAction(IA_WeightGate, ETriggerEvent::Started, this, &ASurfboardPawn::WeightGateStarted);
			EnhancedInputComponent->BindAction(IA_WeightGate, ETriggerEvent::Completed, this, &ASurfboardPawn::WeightGateCompleted);
			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: IA_WeightGate bound"));
		}
		else
		{
			UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: No IA_WeightGate assigned!"));
		}

		// Restart
		if (RestartAction)
		{
			EnhancedInputComponent->BindAction(RestartAction, ETriggerEvent::Triggered, this, &ASurfboardPawn::RestartLevel);
			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: RestartAction bound"));
		}

		// Toggle camera
		if (ToggleCameraAction)
		{
			EnhancedInputComponent->BindAction(ToggleCameraAction, ETriggerEvent::Started, this, &ASurfboardPawn::ToggleCameraMode);
			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: ToggleCameraAction bound"));
		}

		// Replay last ride
		if (ReplayAction)
		{
			EnhancedInputComponent->BindAction(ReplayAction, ETriggerEvent::Started, this, &ASurfboardPawn::ReplayLastRide);
			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: ReplayAction bound"));
		}
	}
}

void ASurfboardPawn::UpdateCameraTransform(float DeltaTime)
{
	// Validate surfboard reference
	if (!SurfboardActor)
	{
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			UE_LOG(LogSurf, Warning, TEXT("UpdateCameraTransform: No SurfboardActor!"));
			bLoggedOnce = true;
		}
		return;
	}

	// Get surfboard's StaticMeshComponent
	UStaticMeshComponent* SurfboardMesh = SurfboardActor->GetStaticMeshComponent();
	if (!SurfboardMesh)
	{
		static bool bLoggedOnce = false;
		if (!bLoggedOnce)
		{
			UE_LOG(LogSurf, Warning, TEXT("UpdateCameraTransform: No SurfboardMesh!"));
			bLoggedOnce = true;
		}
		return;
	}

	// Get surfboard transform
	FTransform SurfboardTransform = SurfboardMesh->GetComponentTransform();
	FRotator SurfboardRotation = SurfboardTransform.GetRotation().Rotator();
	FVector SurfboardLocation = SurfboardTransform.GetLocation();

	// Heading yaw, robust to steep pitch. SurfboardRotation.Yaw (Euler) becomes unstable as the board
	// pitches down the wave (gimbal lock near pitch ±90), swinging the yaw-rotated camera offset to a
	// far-off location even though the board's heading barely changed. Derive it from the forward vector
	// projected onto the horizontal plane, which stays stable through the pitch. The surfboard mesh is
	// rotated 90deg (forward = local +Y), so the forward heading is SurfboardYaw + 90 — hence the -90 here
	// keeps SurfboardYaw equal to the old Euler value when the board is level.
	const FVector BoardForward = SurfboardTransform.GetUnitAxis(EAxis::Y);
	const FVector ForwardHorizontal(BoardForward.X, BoardForward.Y, 0.0f);
	float SurfboardYaw = (ForwardHorizontal.SizeSquared() > KINDA_SMALL_NUMBER)
		? ForwardHorizontal.Rotation().Yaw - 90.0f   // stable: heading from horizontal forward
		: SurfboardRotation.Yaw;                       // nose near-vertical: fall back to Euler yaw

	// Track surfboard motion to detect sudden changes
	if (bEnableCameraLogging)
	{
		static FVector PrevSurfboardLocation = SurfboardLocation;
		static float PrevSurfboardYaw = SurfboardYaw;

		FVector SurfboardLocationChange = SurfboardLocation - PrevSurfboardLocation;
		float SurfboardLocationChangeMagnitude = SurfboardLocationChange.Size();
		float SurfboardYawChange = FMath::Abs(SurfboardYaw - PrevSurfboardYaw);

		if (SurfboardLocationChangeMagnitude > 100.0f || SurfboardYawChange > 10.0f)
		{
			UE_LOG(LogSurf, Warning, TEXT("SURFBOARD SUDDEN MOTION: PosChange=%.2f, YawChange=%.2f, DeltaTime=%.4f, NewYaw=%.2f, NewPos=(%.2f,%.2f,%.2f)"),
				SurfboardLocationChangeMagnitude, SurfboardYawChange, DeltaTime, SurfboardYaw,
				SurfboardLocation.X, SurfboardLocation.Y, SurfboardLocation.Z);
		}

		PrevSurfboardLocation = SurfboardLocation;
		PrevSurfboardYaw = SurfboardYaw;
	}

	// Detect mode changes
	bool bModeChanged = (bUseBehindCamera != bWasBehindCamera);
	if (bModeChanged)
	{
		if (bEnableCameraLogging)
		{
			UE_LOG(LogSurf, Warning, TEXT("Camera position changed: %s -> %s"),
				bWasBehindCamera ? TEXT("BEHIND") : TEXT("BESIDE"),
				bUseBehindCamera ? TEXT("BEHIND") : TEXT("BESIDE"));
		}
		bWasBehindCamera = bUseBehindCamera;

		// Start a unified position+rotation transition blend. The per-component smoothing speeds
		// differ enormously (rotation snaps in ~1 frame, position eases over ~1s), which makes a
		// mode switch look like a teleport followed by a slide. Capture the current camera transform
		// and ease BOTH to the live target together over CameraTransitionDuration. Skipped on the
		// first-ever frame (bCameraInitialized false), where we snap to target anyway.
		if (bCameraInitialized && CameraTransitionDuration > 0.0f)
		{
			// Capture the start as an offset from the CURRENT board position, not a world point.
			// Blending a static world point would anchor the camera to where the board WAS, so it
			// would hang there while the board surfs away. Offset-space keeps it pinned to the live board.
			ModeTransitionStartOffset = CameraRoot->GetComponentLocation() - SurfboardLocation;
			ModeTransitionStartRotation = CameraRoot->GetComponentRotation();
			ModeTransitionTimeRemaining = CameraTransitionDuration;
		}
	}

	// Determine target position and rotation based on camera mode
	FVector TargetOffset;
	float TargetPitch;
	float TargetYaw;
	float TargetRoll = 0.0f;
	// Yaw the rotation offset adds on top of the board-following look yaw (Behind mode only).
	float TargetYawOffset = 0.0f;

	if (bUseBehindCamera)
	{
		// Camera Position 1: Behind surfboard, follows surfboard direction. Blend between the chase
		// config (down-the-line trim) and the bird config (aiming down the wave). The trigger signal
		// is selectable: wave slope steepness or water-column-above (both wave-position properties read
		// from SharedCalculations, so they're steady and ignore board heading/pumping), or the original
		// heading-vs-fall-line dot product. See specs/camera-bird-view-on-descent.md.
		float BirdTriggerValue = 0.0f;
		switch (BirdTriggerSource)
		{
		case EBirdTriggerSource::WaveOcclusion:
			{
				// Line-of-sight test: does the wave BETWEEN the chase camera and the board rise above the
				// sightline and hide the board? Slope/water-above only sample AT the board and miss a crest
				// one metre back. Sample the water height at points along the camera->board segment; the
				// trigger is the max cm any crest pokes above the straight line of sight.
				// Use the CHASE camera position (not the current/bird one) so engaging bird can't lift the
				// camera over the crest and immediately un-trigger itself (feedback oscillation).
				AWaveHeight* Wave = SharedCalculationsForCamera ? SharedCalculationsForCamera->waveVelocity : nullptr;
				if (Wave && OcclusionSampleCount > 1)
				{
					// Aim the sightline at a point OcclusionTargetHeight above the board (rider/sail height),
					// not the board origin — we care whether the rider is visible, so the wave has to rise
					// higher to count as hiding it. Without this the line skims the surface at board height
					// and any wave behind reads as occlusion.
					const FVector BoardTarget = SurfboardLocation + FVector(0.0f, 0.0f, OcclusionTargetHeight);
					const FVector ChaseCamPos = SurfboardLocation + FRotator(0.0f, SurfboardYaw, 0.0f).RotateVector(CameraOffset);
					float MaxOcclusion = 0.0f;
					float WorstWaterZ = 0.0f, WorstSightZ = 0.0f, WorstT = 0.0f;
					for (int32 i = 1; i < OcclusionSampleCount; ++i)
					{
						const float T = (float)i / (float)OcclusionSampleCount;
						const FVector SamplePoint = FMath::Lerp(BoardTarget, ChaseCamPos, T);
						const TArray<FVector> WaterLN = Wave->calculateWaveLocationAndNormalAuto(SamplePoint);
						if (WaterLN.Num() > 0)
						{
							// The sampled water surface is only the FLOOR of the occluder — the breaking lip
							// (animated mesh) and white water (particles) sit on top of it and aren't in the
							// height data. They exist only where the wave towers over the rider, so add a
							// virtual lip height that fades in with how high this crest is above the board.
							float EffectiveWaterZ = (float)WaterLN[0].Z;
							if (OcclusionLipHeight > 0.0f)
							{
								const float CrestAboveBoard = (float)(WaterLN[0].Z - SurfboardLocation.Z);
								const float LipFactor = FMath::SmoothStep(0.0f, FMath::Max(1.0f, OcclusionLipWaterHeight), CrestAboveBoard);
								EffectiveWaterZ += OcclusionLipHeight * LipFactor;
							}
							// SamplePoint.Z is the sightline height here (the segment is a straight line);
							// occluder surface above it means the crest hides the board from the chase view.
							const float Occ = EffectiveWaterZ - SamplePoint.Z;
							if (Occ > MaxOcclusion)
							{
								MaxOcclusion = Occ;
								WorstWaterZ = EffectiveWaterZ;
								WorstSightZ = SamplePoint.Z;
								WorstT = T;
							}
						}
					}
					BirdTriggerValue = MaxOcclusion; // cm the worst crest rises above the line of sight

					if (bDebugBirdTrigger)
					{
						UE_LOG(LogSurf, Log, TEXT("BirdOccl: boardZ=%.1f targetZ=%.1f chaseCamZ=%.1f | worst@t=%.2f waterZ=%.1f sightZ=%.1f occl=%.1f"),
							SurfboardLocation.Z, BoardTarget.Z, ChaseCamPos.Z, WorstT, WorstWaterZ, WorstSightZ, MaxOcclusion);
					}
				}
			}
			break;
		case EBirdTriggerSource::WaveSlopeSteepness:
			BirdTriggerValue = SharedCalculationsForCamera ? SharedCalculationsForCamera->boardWideSlopeSin : 0.0f;
			break;
		case EBirdTriggerSource::WaterColumnAbove:
			BirdTriggerValue = SharedCalculationsForCamera ? SharedCalculationsForCamera->boardWideWaterColumnAbove : 0.0f;
			break;
		case EBirdTriggerSource::HeadingAlignment:
		default:
			{
				// Board forward (horizontal) dotted with the fall line. SurfboardYaw + 90 is the look yaw.
				const FVector ForwardHoriz = FRotator(0.0f, SurfboardYaw + 90.0f, 0.0f).Vector();
				FVector DownhillHoriz = WaveDownhillWorldDir;
				DownhillHoriz.Z = 0.0f;
				DownhillHoriz = DownhillHoriz.GetSafeNormal(); // zero on flat water -> alignment 0 -> chase
				BirdTriggerValue = (float)(ForwardHoriz | DownhillHoriz);
			}
			break;
		}

		// Low-pass the raw trigger before testing it. Hysteresis alone only rejects noise smaller than
		// the deadband; in white water the signal spikes across BOTH thresholds, so the latch flips
		// anyway. Smoothing rejects those transient spikes so only a sustained change moves the camera.
		SmoothedBirdTrigger = FMath::FInterpTo(SmoothedBirdTrigger, BirdTriggerValue, DeltaTime, BirdTriggerSmoothingSpeed);

		// Hysteresis (Schmitt trigger) on the smoothed value: the two thresholds are the enter/exit
		// points, not a ramp. Commit to bird only once it rises above High; return to chase only once it
		// drops below Low. Between them the state is latched. (Requires High >= Low to leave a deadband.)
		if (SmoothedBirdTrigger >= BirdTriggerHigh)     bBirdViewLatched = true;
		else if (SmoothedBirdTrigger <= BirdTriggerLow) bBirdViewLatched = false;

		float TargetBirdBlend = bBirdViewLatched ? 1.0f : 0.0f;

		// Force modes lock the blend to one endpoint so each can be positioned/tuned live in editor.
		if (BehindCameraConfig == EBehindCameraConfig::ForceChase)     TargetBirdBlend = 0.0f;
		else if (BehindCameraConfig == EBehindCameraConfig::ForceBird) TargetBirdBlend = 1.0f;

		// Ease the latched target over time so the chase<->bird move is smooth, not a snap.
		BirdViewBlend = FMath::FInterpTo(BirdViewBlend, TargetBirdBlend, DeltaTime, BirdBlendSpeed);
		const float BirdBlend = BirdViewBlend;

		// Live tuning readout to the log: trigger value vs the Low/High thresholds, plus latch/blend state.
		if (bDebugBirdTrigger)
		{
			UE_LOG(LogSurf, Log, TEXT("BirdCam: trigger=%.3f (raw %.3f) [low %.3f / high %.3f] %s blend=%.2f"),
				SmoothedBirdTrigger, BirdTriggerValue, BirdTriggerLow, BirdTriggerHigh,
				bBirdViewLatched ? TEXT("BIRD") : TEXT("chase"), BirdBlend);
		}

		TargetOffset = FMath::Lerp(CameraOffset, CameraOffsetBird, BirdBlend);
		// Component-wise lerp of the rotation offset (small static angles, no wrap concern).
		TargetPitch     = FMath::Lerp(CameraRotationOffset.Pitch, CameraRotationBird.Pitch, BirdBlend);
		TargetRoll      = FMath::Lerp(CameraRotationOffset.Roll,  CameraRotationBird.Roll,  BirdBlend);
		TargetYawOffset = FMath::Lerp(CameraRotationOffset.Yaw,   CameraRotationBird.Yaw,   BirdBlend);

		// Calculate target yaw following surfboard with smoothing
		static float SmoothBehindYaw = SurfboardYaw;

		// On mode change, initialize SmoothBehindYaw to current camera rotation to avoid jump
		if (bModeChanged)
		{
			FRotator CurrentRotation = CameraRoot->GetComponentRotation();
			// Remove the 90 degree offset to get the surfboard-relative yaw
			SmoothBehindYaw = CurrentRotation.Yaw - 90.0f;
			if (bEnableCameraLogging)
			{
				UE_LOG(LogSurf, Warning, TEXT("Mode changed to BEHIND: Initialized SmoothBehindYaw=%.2f from CurrentYaw=%.2f"),
					SmoothBehindYaw, CurrentRotation.Yaw);
			}
		}

		// Handle angle wrapping: normalize the difference to take the shortest path
		float YawDifference = SurfboardYaw - SmoothBehindYaw;
		// Wrap difference to [-180, 180] range
		while (YawDifference > 180.0f) YawDifference -= 360.0f;
		while (YawDifference < -180.0f) YawDifference += 360.0f;

		// Interpolate using the normalized difference
		float NormalizedTarget = SmoothBehindYaw + YawDifference;
		SmoothBehindYaw = FMath::FInterpTo(SmoothBehindYaw, NormalizedTarget, DeltaTime, YawSmoothingSpeed);

		// Log if there's a large change in smoothed yaw
		if (bEnableCameraLogging)
		{
			static float PrevSmoothBehindYaw = SmoothBehindYaw;
			float YawChange = FMath::Abs(SmoothBehindYaw - PrevSmoothBehindYaw);
			if (YawChange > 5.0f)
			{
				UE_LOG(LogSurf, Warning, TEXT("LARGE YAW CHANGE (Behind): PrevSmooth=%.2f, NewSmooth=%.2f, Target=%.2f, Change=%.2f, DeltaTime=%.4f, SmoothSpeed=%.2f"),
					PrevSmoothBehindYaw, SmoothBehindYaw, SurfboardYaw, YawChange, DeltaTime, YawSmoothingSpeed);
			}
			PrevSmoothBehindYaw = SmoothBehindYaw;
		}

		// Add 90 degree offset to match surfboard's forward direction, plus the config's yaw offset
		TargetYaw = SmoothBehindYaw + 90.0f + TargetYawOffset;
	}
	else
	{
		// Camera Position 2: Beside surfboard, fixed world direction
		TargetOffset = CameraOffsetSide;
		TargetPitch = CameraRotationSide.Pitch;
		TargetYaw = CameraRotationSide.Yaw;
		TargetRoll = CameraRotationSide.Roll;
	}

	// Calculate camera position with offset
	FVector TargetCameraLocation;

	if (bUseBehindCamera)
	{
		// Camera Position 1: Transform offset using ONLY surfboard's yaw (ignore pitch and roll)
		// Create a rotation with only yaw component
		FRotator YawOnlyRotation(0.0f, SurfboardYaw, 0.0f);
		FVector OffsetWorldSpace = YawOnlyRotation.RotateVector(TargetOffset);
		TargetCameraLocation = SurfboardLocation + OffsetWorldSpace;
	}
	else
	{
		// Camera Position 2: Add offset in world space (ignores surfboard rotation)
		TargetCameraLocation = SurfboardLocation + TargetOffset;
	}

	// Live target rotation for this frame (used by both the transition blend and steady-state smoothing).
	const FRotator TargetRotation(TargetPitch, TargetYaw, TargetRoll);

	// Mode-transition blend: while active, ease BOTH position and rotation from the captured
	// start transform to the live target with one shared eased alpha, so they arrive in sync
	// instead of rotation snapping while position slides. Tick the timer down once per frame.
	bool bInModeTransition = false;
	float TransitionAlpha = 0.0f;
	if (ModeTransitionTimeRemaining > 0.0f && CameraTransitionDuration > 0.0f)
	{
		ModeTransitionTimeRemaining = FMath::Max(0.0f, ModeTransitionTimeRemaining - DeltaTime);
		const float Linear = 1.0f - (ModeTransitionTimeRemaining / CameraTransitionDuration);
		TransitionAlpha = FMath::SmoothStep(0.0f, 1.0f, Linear); // ease in/out
		bInModeTransition = true;
	}

	// Smooth camera position to reduce jitter from surfboard movement.
	// On the very first tick, snap to target so the camera doesn't visibly fly in from the actor spawn point.
	FVector CurrentCameraLocation = CameraRoot->GetComponentLocation();
	FVector SmoothedCameraLocation;

	if (!bCameraInitialized || PositionSmoothingSpeed <= 0.0f)
	{
		SmoothedCameraLocation = TargetCameraLocation; // No smoothing (first tick or smoothing disabled)
	}
	else if (bInModeTransition)
	{
		// Morph the board-relative offset from the captured start to the live target offset, added to
		// the CURRENT board position — so the camera tracks the moving board throughout the blend
		// instead of hanging at the world point where the board was when the switch began.
		const FVector LiveOffset = TargetCameraLocation - SurfboardLocation;
		SmoothedCameraLocation = SurfboardLocation + FMath::Lerp(ModeTransitionStartOffset, LiveOffset, TransitionAlpha);
	}
	else
	{
		SmoothedCameraLocation = FMath::VInterpTo(CurrentCameraLocation, TargetCameraLocation, DeltaTime, PositionSmoothingSpeed);
	}

	// Log if there's a large position change
	if (bEnableCameraLogging)
	{
		FVector PositionChange = SmoothedCameraLocation - CurrentCameraLocation;
		float PositionChangeMagnitude = PositionChange.Size();
		if (PositionChangeMagnitude > 50.0f)
		{
			UE_LOG(LogSurf, Warning, TEXT("LARGE POSITION CHANGE: Current=(%.2f,%.2f,%.2f), Target=(%.2f,%.2f,%.2f), Smoothed=(%.2f,%.2f,%.2f), Change=%.2f, DeltaTime=%.4f, SmoothSpeed=%.2f"),
				CurrentCameraLocation.X, CurrentCameraLocation.Y, CurrentCameraLocation.Z,
				TargetCameraLocation.X, TargetCameraLocation.Y, TargetCameraLocation.Z,
				SmoothedCameraLocation.X, SmoothedCameraLocation.Y, SmoothedCameraLocation.Z,
				PositionChangeMagnitude, DeltaTime, PositionSmoothingSpeed);
		}
	}

	// Apply smoothed position
	CameraRoot->SetWorldLocation(SmoothedCameraLocation);

	// Smooth camera rotation. During a mode transition, slerp from the captured start rotation to
	// the live target with the SAME eased alpha as position (quaternion slerp avoids gimbal/wrap
	// issues on the large beside<->behind yaw change). Otherwise use the steady-state follow smoothing.
	FRotator CurrentRotation = CameraRoot->GetComponentRotation();

	FRotator SmoothedRotation;
	if (!bCameraInitialized)
	{
		SmoothedRotation = TargetRotation;
	}
	else if (bInModeTransition)
	{
		SmoothedRotation = FQuat::Slerp(ModeTransitionStartRotation.Quaternion(), TargetRotation.Quaternion(), TransitionAlpha).Rotator();
	}
	else
	{
		SmoothedRotation = FMath::RInterpTo(CurrentRotation, TargetRotation, DeltaTime, YawSmoothingSpeed);
	}
	CameraRoot->SetWorldRotation(SmoothedRotation);

	bCameraInitialized = true;

	// Update controller rotation to match camera
	if (Controller)
	{
		Controller->SetControlRotation(SmoothedRotation);
	}

	// Debug log to see what's happening with the offset
	if (bEnableCameraLogging)
	{
		UE_LOG(LogSurf, Warning, TEXT("UpdateCameraTransform: Mode=%s, Offset=(%.2f,%.2f,%.2f), SurfLoc=(%.2f,%.2f,%.2f), CamLoc=(%.2f,%.2f,%.2f), TargetYaw=%.2f, SmoothedYaw=%.2f"),
			bUseBehindCamera ? TEXT("BEHIND") : TEXT("BESIDE"),
			TargetOffset.X, TargetOffset.Y, TargetOffset.Z,
			SurfboardLocation.X, SurfboardLocation.Y, SurfboardLocation.Z,
			SmoothedCameraLocation.X, SmoothedCameraLocation.Y, SmoothedCameraLocation.Z,
			TargetYaw, SmoothedRotation.Yaw);

		// Periodic detailed logging
		static float DetailedLogTimer = 0.0f;
		DetailedLogTimer += DeltaTime;
		if (DetailedLogTimer >= 2.0f)
		{
			UE_LOG(LogSurf, Log, TEXT("UpdateCameraTransform: Mode=%s, SurfboardLoc=(%.1f,%.1f,%.1f) SurfboardYaw=%.2f TargetYaw=%.2f SmoothedYaw=%.2f"),
				bUseBehindCamera ? TEXT("BEHIND") : TEXT("BESIDE"),
				SurfboardLocation.X, SurfboardLocation.Y, SurfboardLocation.Z,
				SurfboardYaw, TargetYaw, SmoothedRotation.Yaw);
			DetailedLogTimer = 0.0f;
		}
	}
}

void ASurfboardPawn::UpdateWaveRadar(float DeltaTime)
{
	// Track the CVar live so it can be flipped mid-ride without a restart; Install is idempotent.
	const bool bWant = WantsWaveRadar();
	if (bWant != bWaveRadarInstalled)
	{
		bWaveRadarInstalled = bWant;
		if (bWant)
		{
			WaveRadar::Install(GetWorld(), RadarSizePx);
		}
		else
		{
			WaveRadar::Uninstall(GetWorld());
		}
	}

	if (!bWant)
	{
		return;
	}

	// Throttle the sampling to RadarUpdateHz (the widget still repaints every frame).
	RadarUpdateAccumulator += DeltaTime;
	const float Interval = (RadarUpdateHz > 0.0f) ? (1.0f / RadarUpdateHz) : 0.0f;
	if (RadarUpdateAccumulator < Interval)
	{
		return;
	}
	const float Elapsed = RadarUpdateAccumulator; // time since the previous radar update (for foam smoothing)
	RadarUpdateAccumulator = 0.0f;

	AWaveHeight* Wave = SharedCalculationsForCamera ? SharedCalculationsForCamera->waveVelocity : nullptr;
	if (!Wave || !SurfboardActor)
	{
		return;
	}
	UStaticMeshComponent* Mesh = SurfboardActor->GetStaticMeshComponent();
	if (!Mesh)
	{
		return;
	}

	// Center on the mesh's geometric center (bounds origin), NOT GetComponentLocation() — the surfboard
	// mesh pivot is offset toward the tail, so the pivot sits behind the visible board. Bounds.Origin is
	// the true board center regardless of pivot, so the icon lands where the board actually is.
	const FVector Center = Mesh->Bounds.Origin;
	// Board heading (pitch-robust horizontal forward direction).
	const FVector BoardForward = Mesh->GetComponentTransform().GetUnitAxis(EAxis::Y);
	const FVector ForwardHorizontal(BoardForward.X, BoardForward.Y, 0.0f);
	const float HeadingYaw = (ForwardHorizontal.SizeSquared() > KINDA_SMALL_NUMBER)
		? ForwardHorizontal.Rotation().Yaw
		: Mesh->GetComponentRotation().Yaw;

	// The frame the grid is sampled in: board heading (heading-up) or a fixed world yaw (world-fixed,
	// where the surfer triangle rotates instead). Binning the white water uses the same frame.
	const float SampleYaw = bRadarHeadingUp ? HeadingYaw : RadarWorldYawOffset;
	const FRotator HeadingRot(0.0f, SampleYaw, 0.0f);

	const int32 N = FMath::Clamp(RadarGridN, 3, 41);
	const float Spacing = (N > 1) ? (2.0f * RadarExtentCm / (float)(N - 1)) : RadarExtentCm;
	const float Mid = (N - 1) * 0.5f;

	TArray<float> Heights;
	TArray<float> Slopes;
	Heights.SetNumZeroed(N * N);
	Slopes.SetNumZeroed(N * N);
	float MinH = TNumericLimits<float>::Max();
	float MaxH = TNumericLimits<float>::Lowest();

	for (int32 gy = 0; gy < N; ++gy)
	{
		for (int32 gx = 0; gx < N; ++gx)
		{
			const float LocalForward = (gy - Mid) * Spacing;   // +X = board forward
			const float LocalRight = (gx - Mid) * Spacing;     // +Y = board right
			const FVector WorldOffset = HeadingRot.RotateVector(FVector(LocalForward, LocalRight, 0.0f));
			const FVector P(Center.X + WorldOffset.X, Center.Y + WorldOffset.Y, Center.Z);

			const TArray<FVector> WaterLN = Wave->calculateWaveLocationAndNormalAuto(P);
			const int32 Idx = gy * N + gx;
			if (WaterLN.Num() >= 2)
			{
				const float H = (float)WaterLN[0].Z;
				const float NormalZ = (float)WaterLN[1].Z;
				Heights[Idx] = H;
				Slopes[Idx] = FMath::Sqrt(FMath::Max(0.0f, 1.0f - NormalZ * NormalZ));
				MinH = FMath::Min(MinH, H);
				MaxH = FMath::Max(MaxH, H);
			}
			else
			{
				Heights[Idx] = Center.Z;
				Slopes[Idx] = 0.0f;
			}
		}
	}

	FWaveRadarSnapshot Snapshot;
	Snapshot.GridN = N;
	Snapshot.HeightT.SetNumZeroed(N * N);
	Snapshot.Foam.SetNumZeroed(N * N);
	const float Range = MaxH - MinH;
	for (int32 i = 0; i < N * N; ++i)
	{
		Snapshot.HeightT[i] = (Range > KINDA_SMALL_NUMBER) ? ((Heights[i] - MinH) / Range) : 0.5f;
	}

	// White-water density per cell: count the ACTUAL particles per cell (preferred), or fall back to the
	// slope estimate when no particle controller is wired up.
	TArray<float> Counts;
	Counts.SetNumZeroed(N * N);
	if (ParticleControllerForRadar)
	{
		const FVector ForwardDir = HeadingRot.RotateVector(FVector(1.0f, 0.0f, 0.0f)); // board forward, horizontal
		const FVector RightDir = HeadingRot.RotateVector(FVector(0.0f, 1.0f, 0.0f));    // board right, horizontal
		const float InvSpacing = (Spacing > KINDA_SMALL_NUMBER) ? (1.0f / Spacing) : 0.0f;
		for (const FVector& WP : ParticleControllerForRadar->GetWhiteWaterWorldPoints())
		{
			const FVector Off = WP - Center;
			const float LocalForward = Off.X * ForwardDir.X + Off.Y * ForwardDir.Y;
			const float LocalRight = Off.X * RightDir.X + Off.Y * RightDir.Y;
			const int32 gx = FMath::RoundToInt(LocalRight * InvSpacing + Mid);
			const int32 gy = FMath::RoundToInt(LocalForward * InvSpacing + Mid);
			if (gx >= 0 && gx < N && gy >= 0 && gy < N)
			{
				Counts[gy * N + gx] += 1.0f;
			}
		}
	}
	else
	{
		for (int32 i = 0; i < N * N; ++i)
		{
			Counts[i] = (Slopes[i] > BreakSlopeThreshold) ? RadarFoamSaturationCount : 0.0f;
		}
	}

	// Temporally smooth the foam: rises fast where white water appears, fades slowly where it leaves, so
	// it flows between updates and merges instead of popping. Persisted across updates.
	if (RadarFoamIntensity.Num() != N * N)
	{
		RadarFoamIntensity.Init(0.0f, N * N);
	}
	const float SatCount = FMath::Max(1.0f, RadarFoamSaturationCount);
	for (int32 i = 0; i < N * N; ++i)
	{
		const float Target = FMath::Min(1.0f, Counts[i] / SatCount);
		const float Rate = (Target > RadarFoamIntensity[i]) ? RadarFoamRiseSpeed : RadarFoamFallSpeed;
		RadarFoamIntensity[i] = FMath::FInterpTo(RadarFoamIntensity[i], Target, Elapsed, Rate);
		Snapshot.Foam[i] = RadarFoamIntensity[i];
	}
	// Surfer heading on the radar = board heading relative to the sample frame (0 in heading-up mode).
	Snapshot.SurferAngleDeg = HeadingYaw - SampleYaw;
	// Board icon sized to the radar's real world scale: the radar spans 2*RadarExtentCm across its width,
	// so a length in cm is (cm / fullExtent) of the radar. Half-sizes for the centered outline.
	const float FullExtentCm = 2.0f * FMath::Max(1.0f, RadarExtentCm);
	Snapshot.BoardHalfLenFrac = (RadarBoardLengthCm * 0.5f) / FullExtentCm;
	Snapshot.BoardHalfWidFrac = (RadarBoardWidthCm * 0.5f) / FullExtentCm;
	Snapshot.bValid = true;

	WaveRadar::UpdateData(GetWorld(), Snapshot);
}

void ASurfboardPawn::PaddleForward(const FInputActionValue& Value)
{
	// Block input during autopilot phase
	if (!bPlayerControlsEnabled)
	{
		return;
	}

	float PaddleIntensity = Value.Get<float>();

	// Apply simulated input for testing in simulation mode
	if (bSimulateForwardInput)
	{
		PaddleIntensity = 1.0f;
	}

	// Apply deadzone
	const float InputDeadzone = 0.1f;
	if (FMath::Abs(PaddleIntensity) < InputDeadzone)
	{
		return;
	}

	// Validate surfboard reference
	if (!SurfboardActor)
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn::PaddleForward - No SurfboardActor reference!"));
		return;
	}

	UStaticMeshComponent* SurfboardMesh = SurfboardActor->GetStaticMeshComponent();
	if (!SurfboardMesh)
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn::PaddleForward - SurfboardActor has no StaticMeshComponent!"));
		return;
	}

	// Get surfboard's forward direction (note: mesh is rotated 90 degrees)
	FVector SurfboardForward = SurfboardMesh->GetRightVector();

	// Calculate force magnitude (0..1 intensity scales the force strength)
	float ForceMagnitude = PaddleForceStrength * FMath::Clamp(PaddleIntensity, 0.0f, 1.0f);
	FVector Force = SurfboardForward * ForceMagnitude;

	// Apply force at surfboard center
	FVector ForceLocation = SurfboardMesh->GetComponentLocation();
	SurfboardMesh->AddForceAtLocation(Force, ForceLocation);

	UE_LOG(LogSurf, Log, TEXT("SurfboardPawn::PaddleForward - Intensity=%.2f, Magnitude=%.0f N"),
		PaddleIntensity, ForceMagnitude);

	// Debug visualization
	if (bDebugPaddleForce || SurfDebug::ShouldDebug(this, TEXT("paddle")))
	{
		UWorld* World = GetWorld();
		if (World)
		{
			DrawDebugSphere(World, ForceLocation, 25.0f, 12, FColor::Green, false, 0.1f, 0, 3.0f);
			DrawDebugLine(World, ForceLocation, ForceLocation + Force * 0.01f,
				FColor::Yellow, false, 0.1f, 0, 5.0f);
			DrawDebugString(World, ForceLocation + FVector(0, 0, 100),
				FString::Printf(TEXT("Paddle: %.0f N"), ForceMagnitude),
				nullptr, FColor::Yellow, 0.1f);
		}
	}
}

void ASurfboardPawn::Turn(const FInputActionValue& Value)
{
	// Block input during autopilot phase
	if (!bPlayerControlsEnabled)
	{
		return;
	}

	float TurnInput = Value.Get<float>();

	// Apply simulated input for testing
	if (bSimulateLeftInput)
	{
		TurnInput = -1.0f;
	}

	// Validate surfboard reference
	if (!SurfboardActor)
	{
		return;
	}

	UStaticMeshComponent* SurfboardMesh = SurfboardActor->GetStaticMeshComponent();
	if (!SurfboardMesh)
	{
		return;
	}

#if PLATFORM_ANDROID
	// Android: threshold the left stick X axis
	if (FMath::Abs(TurnInput) < AndroidTurnThreshold)
	{
		return; // Below threshold, no turn
	}
	// Convert to binary -1 or +1
	TurnInput = (TurnInput > 0.0f) ? 1.0f : -1.0f;
#else
	// PC: A/D keys already provide -1, 0, or +1
	if (FMath::IsNearlyZero(TurnInput))
	{
		return;
	}
#endif

	// Get surfboard's left direction (note: mesh is rotated 90 degrees)
	// Forward = Right, so Left = Forward
	FVector SurfboardLeft = SurfboardMesh->GetForwardVector();

	// Calculate sideways force (positive input = turn right, so force to the right)
	// Negative input = turn left, so force to the left
	float ForceMagnitude = TurnForceStrength;
	FVector Force = -SurfboardLeft * (TurnInput * ForceMagnitude);

	// Apply force at forward offset to create torque
	FVector ForceLocation = SurfboardMesh->GetComponentLocation();
	FVector LocalOffset = FVector(0.0f, TurnForceForwardOffset, 0.0f); // Y is forward in surfboard local space
	FVector WorldOffset = SurfboardMesh->GetComponentTransform().TransformVector(LocalOffset);
	ForceLocation += WorldOffset;

	SurfboardMesh->AddForceAtLocation(Force, ForceLocation);

	UE_LOG(LogSurf, Log, TEXT("SurfboardPawn::Turn - Input=%.2f, Magnitude=%.0f N"),
		TurnInput, ForceMagnitude);

	// Debug visualization
	if (bDebugPaddleForce || SurfDebug::ShouldDebug(this, TEXT("paddle")))
	{
		UWorld* World = GetWorld();
		if (World)
		{
			DrawDebugSphere(World, ForceLocation, 25.0f, 12, FColor::Cyan, false, 0.1f, 0, 3.0f);
			DrawDebugLine(World, ForceLocation, ForceLocation + Force * 0.01f,
				FColor::Cyan, false, 0.1f, 0, 5.0f);
		}
	}
}

void ASurfboardPawn::WeightInput(const FInputActionValue& Value)
{
	// Block input during autopilot phase
	if (!bPlayerControlsEnabled)
	{
		return;
	}

	FVector2D WeightStick = Value.Get<FVector2D>();
	LatestStickValue = WeightStick;

#if PLATFORM_ANDROID
	// Tilt is the default Android weight source — ignore stick entirely so a
	// resting thumb on the (unused) right stick can't fight the tilt write in Tick.
	if (AndroidWeightInputSource != EAndroidWeightInputSource::Stick)
	{
		return;
	}

	// Android right stick input
	float StickMagnitude = WeightStick.Size();
	if (StickMagnitude < AndroidWeightDeadzone)
	{
		// Below deadzone, mark input as inactive
		bWeightInputActive = false;
		return;
	}

	// Active input, update offset directly from stick
	bWeightInputActive = true;
	CurrentWeightOffset = WeightStick;
	CurrentWeightOffset.X = FMath::Clamp(CurrentWeightOffset.X, -1.0f, 1.0f);
	CurrentWeightOffset.Y = FMath::Clamp(CurrentWeightOffset.Y, -1.0f, 1.0f);
#else
	// PC mouse delta integration, active while the RMB gate is held. It coexists with the on-screen
	// stick rather than standing down for it: the two use different buttons (RMB gate here, left
	// button on the stick), so the only way they can fight is a thumb on the stick while RMB is
	// also down - which is what the bTouchStickHeld check excludes. Suppressing this whenever the
	// overlay merely EXISTED left desktop with no way to steer except dragging the ring.
	if (bWeightInputActive && !bTouchStickHeld)
	{
		// Mouse delta is in pixels, scale by sensitivity (separate for X and Y)
		FVector2D MouseDelta = WeightStick;
		FVector2D DeltaOffset;
		DeltaOffset.X = MouseDelta.X / GetEffectiveMouseWeightSensitivity(false);
		DeltaOffset.Y = MouseDelta.Y / GetEffectiveMouseWeightSensitivity(true);

		// Raw travel actually arriving from the mouse, per axis, for the WEIGHT readout: it is the
		// only way to tell "the axis is under-scaled" apart from "the hand moved less on that axis".
		WeightMouseTravelSinceLog += FVector2D(FMath::Abs(MouseDelta.X), FMath::Abs(MouseDelta.Y));

		// Integrate into current offset
		CurrentWeightOffset += DeltaOffset;
		CurrentWeightOffset.X = FMath::Clamp(CurrentWeightOffset.X, -1.0f, 1.0f);
		CurrentWeightOffset.Y = FMath::Clamp(CurrentWeightOffset.Y, -1.0f, 1.0f);
	}
#endif
}

void ASurfboardPawn::WeightGateStarted(const FInputActionValue& Value)
{
#if !PLATFORM_ANDROID
	// PC only: RMB pressed
	bWeightInputActive = true;

	// Hide cursor but keep mouse input active
	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		// Store cursor position before hiding
		PC->GetMousePosition(CursorPositionBeforeLock.X, CursorPositionBeforeLock.Y);

		// Just hide the cursor, don't lock it (so mouse delta still works)
		PC->bShowMouseCursor = false;

		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn::WeightGateStarted - Cursor hidden, stored position (%.1f, %.1f)"),
			CursorPositionBeforeLock.X, CursorPositionBeforeLock.Y);
	}
#endif
}

void ASurfboardPawn::WeightGateCompleted(const FInputActionValue& Value)
{
#if !PLATFORM_ANDROID
	// PC only: RMB released
	bWeightInputActive = false;

	// Restore cursor
	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		// Restore cursor position
		PC->SetMouseLocation(CursorPositionBeforeLock.X, CursorPositionBeforeLock.Y);

		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn::WeightGateCompleted - Cursor restored to (%.1f, %.1f)"),
			CursorPositionBeforeLock.X, CursorPositionBeforeLock.Y);
	}

	// Weight will auto-center in Tick()
#endif
}

float ASurfboardPawn::GetEffectiveTiltPitchForFull() const
{
	return Tuning ? Tuning->TiltPitchDegreesForFullDeflection : TiltPitchDegreesForFullDeflection;
}

// On-screen fore/aft weight readout. Toggle live from the console with
//   surf.debug.flags weight
// (no actor filter needed - this reads IsFlagSet, so it does not switch on the per-tick
// WeightDistribution log, which additionally wants surf.debug.actors). Exists because the fore/aft
// axis is the one you cannot see: a weight shift back shows up as a couple of degrees of pitch that
// is easy to miss while riding, so "is my input arriving?" and "is the board ignoring it?" are
// indistinguishable by eye. This shows the number that is actually reaching AWeightDistribution.
void ASurfboardPawn::DrawWeightReadout() const
{
#if !UE_BUILD_SHIPPING
	if (!GEngine || !WeightDistribution || !SurfDebug::IsFlagSet(TEXT("weight")))
	{
		return;
	}

	const float MaxInFront = GetEffectiveWeightMaxInFront();
	const float InFront    = WeightDistribution->amountInFront;
	// What the player's input asked for, before the forward ceiling. Divergence between the two is
	// the ceiling doing its job (or the assist biasing the trim).
	const float Commanded  = 0.5f + CurrentWeightOffset.Y * 0.5f;
	// How far onto the tail the weight actually is: 0 = centred stance, 1 = all of it on the tail.
	const float BackAmount = FMath::Clamp((0.5f - InFront) * 2.0f, 0.0f, 1.0f);
	const bool  bCapped    = Commanded > MaxInFront + KINDA_SMALL_NUMBER;

	// Tail at the left, nose at the right. '|' is the centred stance, '#' the forward ceiling,
	// 'X' where the weight actually sits.
	const int32 Cells = 21;
	TCHAR Bar[22];
	for (int32 i = 0; i < Cells; ++i) { Bar[i] = TEXT('.'); }
	Bar[Cells] = TEXT('\0');
	Bar[Cells / 2] = TEXT('|');
	Bar[FMath::Clamp(FMath::RoundToInt(MaxInFront * (Cells - 1)), 0, Cells - 1)] = TEXT('#');
	Bar[FMath::Clamp(FMath::RoundToInt(InFront    * (Cells - 1)), 0, Cells - 1)] = TEXT('X');

#if PLATFORM_ANDROID
	const TCHAR* Source = (AndroidWeightInputSource == EAndroidWeightInputSource::Tilt)
		? TEXT("tilt")
		: (bWeightInputActive ? TEXT("stick") : TEXT("stick-idle"));
#else
	// On PC the fore/aft axis only integrates while the RMB gate is held, so say which it is
	// outright - "no input arriving" and "input arriving but small" look identical otherwise.
	const TCHAR* Source = bWeightInputActive ? TEXT("mouse-RMB-held") : TEXT("idle-no-RMB");
#endif

	const FString Msg = FString::Printf(
		TEXT("WEIGHT fore/aft   back=%.2f   inFront=%.3f   commanded=%.3f   cap=%.2f%s\n")
		TEXT("  tail [%s] nose      right=%.3f   src=%s   controls=%s"),
		BackAmount, InFront, Commanded, MaxInFront, bCapped ? TEXT("  <- CAPPED") : TEXT(""),
		Bar, WeightDistribution->amountToTheRight, Source,
		bPlayerControlsEnabled ? TEXT("on") : TEXT("OFF"));

	// Fixed key so it updates in place instead of scrolling; lifetime just over a frame so it
	// disappears as soon as the flag is cleared.
	GEngine->AddOnScreenDebugMessage(
		/*Key=*/0x5715A17F, /*TimeToDisplay=*/0.2f,
		bCapped ? FColor::Yellow : FColor::Green, Msg);

	// Same numbers to the log, throttled to 5 Hz - fast enough to see a weight shift happen, slow
	// enough to read back afterwards. Gated on IsFlagSet alone, deliberately: the per-tick
	// AWeightDistribution log next door goes through ShouldDebug, which ALSO needs a non-empty
	// surf.debug.actors, and a missing actor filter silently produces no output at all.
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	if (Now - LastWeightReadoutLogTime >= 0.2 || Now < LastWeightReadoutLogTime)
	{
		LastWeightReadoutLogTime = Now;
		UE_LOG(LogSurf, Warning,
			TEXT("WEIGHT back=%.2f inFront=%.3f commanded=%.3f cap=%.2f%s right=%.3f offset=(%.3f,%.3f) travelPx=(%.0f,%.0f) sens=(%.0f,%.0f) src=%s controls=%s"),
			BackAmount, InFront, Commanded, MaxInFront, bCapped ? TEXT(" CAPPED") : TEXT(""),
			WeightDistribution->amountToTheRight, CurrentWeightOffset.X, CurrentWeightOffset.Y,
			WeightMouseTravelSinceLog.X, WeightMouseTravelSinceLog.Y,
			GetEffectiveMouseWeightSensitivity(false), GetEffectiveMouseWeightSensitivity(true),
			Source, bPlayerControlsEnabled ? TEXT("on") : TEXT("OFF"));
		WeightMouseTravelSinceLog = FVector2D::ZeroVector;
	}
#endif
}

float ASurfboardPawn::GetEffectiveWeightMaxInFront() const
{
	return FMath::Clamp(Tuning ? Tuning->WeightMaxInFront : WeightMaxInFront, 0.0f, 1.0f);
}

float ASurfboardPawn::GetEffectiveTiltRollForFull() const
{
	return Tuning ? Tuning->TiltRollDegreesForFullDeflection : TiltRollDegreesForFullDeflection;
}

float ASurfboardPawn::GetEffectiveTiltYawGain() const
{
	return Tuning ? Tuning->TiltYawGain : TiltYawGain;
}

float ASurfboardPawn::GetEffectiveTiltYawSign() const
{
	const float Invert = Tuning ? Tuning->TiltYawInvert : TiltYawInvert;
	return Invert >= 0.5f ? -1.0f : 1.0f;
}

float ASurfboardPawn::GetEffectiveTiltYawLeakSeconds() const
{
	return Tuning ? Tuning->TiltYawLeakSeconds : TiltYawLeakSeconds;
}

float ASurfboardPawn::GetEffectiveTiltYawBiasSampleSeconds() const
{
	return Tuning ? Tuning->TiltYawBiasSampleSeconds : TiltYawBiasSampleSeconds;
}

SurfTilt::FTuning ASurfboardPawn::MakeTiltTuning() const
{
	SurfTilt::FTuning T;
	T.PitchDegreesForFull  = GetEffectiveTiltPitchForFull();
	T.RollDegreesForFull   = GetEffectiveTiltRollForFull();
	T.DeadzoneDegrees      = TiltAngleDeadzoneDegrees;
	T.bInvertPitch         = bInvertTiltPitch;
	T.bInvertRoll          = bInvertTiltRoll;
	T.YawGain              = GetEffectiveTiltYawGain();
	T.YawSign              = GetEffectiveTiltYawSign();
	T.YawLeakSeconds       = GetEffectiveTiltYawLeakSeconds();
	T.YawBiasSampleSeconds = GetEffectiveTiltYawBiasSampleSeconds();
	return T;
}

void ASurfboardPawn::UpdateTiltWeight()
{
#if PLATFORM_ANDROID
	APlayerController* PC = Cast<APlayerController>(Controller);
	if (!PC)
	{
		return;
	}

	FVector Tilt, RotationRate, Gravity, Acceleration;
	PC->GetInputMotionState(Tilt, RotationRate, Gravity, Acceleration);

	if (Gravity.IsNearlyZero() || !TiltBasis.IsUsable())
	{
		return;
	}

	// The mapping itself is in SurfTilt.cpp, shared with the start-screen tutorial's feedback
	// board. Everything below is plumbing: cache what the recorder and the log want, then write
	// the offset.
	const float DeltaTime = FMath::Max(GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.0f, 0.0f);
	const SurfTilt::FResult R = SurfTilt::ComputeWeight(
		TiltBasis, MakeTiltTuning(), TiltYaw, Gravity, RotationRate, DeltaTime);

	// Cache the raw signed deltas for the input-trace recorder (pre-deadzone, pre-scale). Replay
	// reads these and re-runs the deadzone/sensitivity pipeline on the PC side so those
	// parameters remain tunable post-record. RollDeg is the *fused* angle, so replay picks up the
	// fusion for free.
	LatestTiltPitchDeg   = R.PitchDeg;
	LatestTiltRollDeg    = R.RollDeg;
	LatestRawYawRate     = R.RawYawRate;
	LatestGravityRollDeg = R.GravityRollDeg;
	LatestYawWeight      = R.YawWeight;

	CurrentWeightOffset.X = R.Offset.X;
	CurrentWeightOffset.Y = R.Offset.Y;

	if (bDebugTilt || SurfDebug::IsFlagSet(TEXT("tilt")))
	{
		// yawW is now the weight the fusion actually applied. It used to be a separate
		// `1 - |g.z|` computed here "for the debug log only" — the old inverted formula, left
		// beside the corrected one, reporting the opposite of what the code did.
		const FVector G = Gravity.GetSafeNormal();
		UE_LOG(LogSurf, Warning,
			TEXT("Tilt: G=(%.3f,%.3f,%.3f) PitchDeg=%.2f RollDeg=%.2f (gravRoll=%.2f yaw=%.2f yawW=%.2f rawRate=%.4f gyro=(%.3f,%.3f,%.3f) bias=%.4f gain=%.2f sign=%.0f cond=%.3f tuning=%s%s) Offset=(%.3f,%.3f) -> amountInFront=%.3f amountToTheRight=%.3f -> writing to WD=%s"),
			G.X, G.Y, G.Z, R.PitchDeg, R.RollDeg,
			R.GravityRollDeg, TiltYaw.AngleDeg, R.YawWeight, R.RawYawRate,
			RotationRate.X, RotationRate.Y, RotationRate.Z, TiltYaw.RateBias,
			GetEffectiveTiltYawGain(), GetEffectiveTiltYawSign(), TiltBasis.Conditioning,
			Tuning ? TEXT("subsystem") : TEXT("PAWN-FALLBACK"),
			TiltYaw.bBiasReady ? TEXT("") : TEXT(" CALIBRATING"),
			CurrentWeightOffset.X, CurrentWeightOffset.Y,
			FMath::Min(0.5f + CurrentWeightOffset.Y * 0.5f, GetEffectiveWeightMaxInFront()),
			0.5f + CurrentWeightOffset.X * 0.5f,
			WeightDistribution ? *WeightDistribution->GetName() : TEXT("(null)"));
	}
#endif
}

void ASurfboardPawn::UpdatePumpInput()
{
#if PLATFORM_ANDROID
	APlayerController* PC = Cast<APlayerController>(Controller);
	if (!PC)
	{
		PumpInput = 0.0f;
		return;
	}

	FVector Tilt, RotationRate, Gravity, Acceleration;
	PC->GetInputMotionState(Tilt, RotationRate, Gravity, Acceleration);

	// The filter and the loading-only scaling live in SurfTilt.cpp, shared with the start-screen
	// tutorial's pump feedback — which needs no calibration, so it works in any grip.
	SurfTilt::FPumpTuning PumpTuning;
	PumpTuning.LowPassHz    = Tuning ? Tuning->PumpLowPassHz     : PumpTuning.LowPassHz;
	PumpTuning.Deadzone     = Tuning ? Tuning->PumpDeadzone      : PumpTuning.Deadzone;
	PumpTuning.AccelForFull = Tuning ? Tuning->PumpAccelForFull  : PumpTuning.AccelForFull;

	const float dt = FMath::Max(GetWorld()->GetDeltaSeconds(), KINDA_SMALL_NUMBER);
	PumpInput = SurfTilt::ComputePump(Gravity, Acceleration, PumpTuning, PumpLowPassA, dt);

	if (SurfDebug::IsFlagSet(TEXT("pump")))
	{
		const FVector gUnit = Gravity.GetSafeNormal();
		UE_LOG(LogSurf, Warning,
			TEXT("UpdatePumpInput: A_alongG=%.3f filtered=%.3f PumpInput=%.3f"),
			FVector::DotProduct(Acceleration, gUnit), PumpLowPassA, PumpInput);
	}
#else
	// Desktop never reaches here any more: the space-bar is now read in Tick as the pump button
	// and goes through UpdateHeldPump, so PC and device run the same cycle. The old fallback here
	// ramped PumpInput to a sustained 1.0 with no cycle and no weight swing, which meant PC pump
	// testing was exercising a signal the phone never produces.
	// See specs/pumping.md Phase 2 and specs/pump-button-and-virtual-stick.md FR4.
	PumpInput = 0.0f;
#endif
}

void ASurfboardPawn::RestartLevel()
{
	UE_LOG(LogSurf, Display, TEXT("SurfboardPawn::RestartLevel - straight into a fresh wave"));
	ReloadLevel(/*bSkipStartScreen*/true);
}

void ASurfboardPawn::ReturnToHub()
{
	UE_LOG(LogSurf, Display, TEXT("SurfboardPawn::ReturnToHub - back to the start screen"));
	ReloadLevel(/*bSkipStartScreen*/false);
}

void ASurfboardPawn::ReloadLevel(bool bSkipStartScreen)
{
	// A fresh wave skips the menu; the hub shows it - even past the dev StartScreenSkip. The flags
	// survive the reload (module statics).
	GSkipStartScreenOnNextLoad = bSkipStartScreen;
	GWantStartScreenOnNextLoad = !bSkipStartScreen;
	bReloadPending = true;

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogSurf, Error, TEXT("SurfboardPawn::ReloadLevel - No world!"));
		return;
	}

	// Never reload a paused world into a fade: the timer below runs on world time, so it would
	// never fire and the game would sit black behind a modal for ever. The rack and the list pause
	// when they open from the hub; the hub itself pauses.
	if (UGameplayStatics::IsGamePaused(World))
	{
		UGameplayStatics::SetGamePaused(World, false);
	}

	// Get player controller for fade
	APlayerController* PC = Cast<APlayerController>(Controller);
	if (!PC)
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn::ReloadLevel - No PlayerController, reloading without fade"));

		// Get current level name
		FString CurrentLevelName = World->GetMapName();
		CurrentLevelName.RemoveFromStart(World->StreamingLevelsPrefix);

		UGameplayStatics::OpenLevel(World, FName(*CurrentLevelName), false);
		return;
	}

	// Start fade to black
	PC->PlayerCameraManager->StartCameraFade(0.0f, 1.0f, 0.5f, FLinearColor::Black, false, true);

	// Get current level name
	FString CurrentLevelName = World->GetMapName();
	CurrentLevelName.RemoveFromStart(World->StreamingLevelsPrefix);

	UE_LOG(LogSurf, Log, TEXT("SurfboardPawn::ReloadLevel - Fading out, will reload level: %s"), *CurrentLevelName);

	// Set a timer to restart the level after fade completes
	// Use weak pointer to safely check if world is still valid
	TWeakObjectPtr<UWorld> WeakWorld(World);
	FTimerHandle RestartTimerHandle;
	World->GetTimerManager().SetTimer(RestartTimerHandle, [WeakWorld, CurrentLevelName]()
	{
		if (UWorld* CurrentWorld = WeakWorld.Get())
		{
			UGameplayStatics::OpenLevel(CurrentWorld, FName(*CurrentLevelName), false);
		}
	}, 0.5f, false);
}

void ASurfboardPawn::ToggleCameraMode()
{
	// The game runs on the Beside camera all the time now, so the toggle no longer switches.
	// Kept (and still bound to ToggleCameraAction) so the input binding and any Blueprint call
	// stay valid; SetCameraBehind is the deliberate escape hatch if a Behind view is wanted again.
	UE_LOG(LogSurf, Log, TEXT("SurfboardPawn::ToggleCameraMode - ignored, camera stays BESIDE surfboard"));
}

void ASurfboardPawn::SetCameraBehind(bool bBehind)
{
	bUseBehindCamera = bBehind;
	UE_LOG(LogSurf, Log, TEXT("SurfboardPawn::SetCameraBehind - Camera position: %s"),
		bUseBehindCamera ? TEXT("BEHIND surfboard") : TEXT("BESIDE surfboard"));
}

void ASurfboardPawn::InitAutoPilotState()
{
	// State-based path: when a StateTriggerAutoPilot is wired up, control
	// handoff is driven by its bFinished flag (see UpdatePlayerControlState),
	// not by the AutoPilotDuration timer. This avoids the timer-vs-actual-steps
	// mismatch that lets the autopilot keep writing weight after controls
	// "enable" but before the autopilot has actually completed its sequence.
	if (StateTriggerAutoPilot)
	{
		bPlayerControlsEnabled = false;
		AutoPilotElapsedTime = 0.0f;
		TimeAfterAutoPilotComplete = 0.0f;
		bUseBehindCamera = false;
		UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: Waiting on StateTriggerAutoPilot.bFinished; controls enable %.2fs after."),
			ControlDelayAfterAutoPilot);
		return;
	}

	if (AutoPilotDuration <= 0.0f)
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: AutoPilotDuration <= 0 - player controls will be enabled immediately"));
		bPlayerControlsEnabled = true;
		OnPlayerControlsEnabled();
		return;
	}

	// Start with controls disabled (autopilot phase)
	bPlayerControlsEnabled = false;
	AutoPilotElapsedTime = 0.0f;
	TimeAfterAutoPilotComplete = 0.0f;

	// Ensure we start in beside camera mode
	bUseBehindCamera = false;
	UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: Starting autopilot phase (%.2fs) with beside camera"),
		AutoPilotDuration);
}

void ASurfboardPawn::OnPlayerControlsEnabled()
{
#if PLATFORM_ANDROID
	// Snapshot phone's current "down" direction as the tilt neutral so players
	// can hold the device however they like and have that pose count as centered
	// weight. Re-snapshot every handoff (e.g. level restart) — pose may differ.
	if (AndroidWeightInputSource == EAndroidWeightInputSource::Tilt)
	{
		APlayerController* PC = Cast<APlayerController>(Controller);
		if (PC)
		{
			FVector Tilt, RotationRate, Gravity, Acceleration;
			PC->GetInputMotionState(Tilt, RotationRate, Gravity, Acceleration);
			TiltBasis = SurfTilt::Calibrate(Gravity);

			// Restart the gyro-yaw integrator and its bias estimate \u2014 the player's grip
			// (and the gyro's zero-rate offset with it) may differ every handoff.
			TiltYaw.Reset();

			if (TiltBasis.bCalibrated && TiltBasis.Conditioning < 0.2f)
			{
				UE_LOG(LogSurf, Warning,
					TEXT("SurfboardPawn: tilt calibration is near-degenerate (|Fwd|=%.3f) \u2014 phone is close to flat. ")
					TEXT("Pitch/roll axes will be unreliable; recalibrate holding the phone nearer upright."),
					TiltBasis.Conditioning);
			}

			UE_LOG(LogSurf, Display,
				TEXT("SurfboardPawn: Tilt calibrated. NeutralGravity=(%.3f, %.3f, %.3f) |g|=%.3f Forward=(%.3f, %.3f, %.3f) Right=(%.3f, %.3f, %.3f) cond=%.3f calibrated=%s"),
				TiltBasis.NeutralGravity.X, TiltBasis.NeutralGravity.Y, TiltBasis.NeutralGravity.Z, TiltBasis.NeutralGravity.Size(),
				TiltBasis.ForwardAxis.X, TiltBasis.ForwardAxis.Y, TiltBasis.ForwardAxis.Z,
				TiltBasis.RightAxis.X, TiltBasis.RightAxis.Y, TiltBasis.RightAxis.Z,
				TiltBasis.Conditioning,
				TiltBasis.bCalibrated ? TEXT("yes") : TEXT("no (zero gravity vector)"));
		}
		else
		{
			UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: Tilt calibration skipped — no PlayerController"));
		}
	}
#endif

	// Trace recording starts at the same handoff moment so the trace's t=0 lines
	// up with player control — and on tilt sessions, with the freshly snapshotted
	// NeutralGravity above. See specs/input-trace-replay.md.
	if (bRecordInputTrace && !bInputTraceActive)
	{
		StartInputTrace();
	}

	// -RecordIntro is the mirror image: recording started at BeginPlay to capture the INTRO, so it
	// stops here, at handoff. The trace's final row is therefore exactly the state the player takes
	// control from — which is what a rails intro stamps. See specs/deterministic-ride-handoff.md.
	if (bRecordingIntroTrace && bInputTraceActive && IntroRecordSeconds <= 0.0f)
	{
		StopInputTrace(TEXT("intro_recorded"));
		bRecordingIntroTrace = false;
		UE_LOG(LogSurf, Warning,
			TEXT("SurfboardPawn: intro trace captured (BeginPlay -> handoff). Point RailsIntroTrace at it to play this intro back on every device."));
	}
}

void ASurfboardPawn::UpdatePlayerControlState(float DeltaTime)
{
	if (bPlayerControlsEnabled)
	{
		return;
	}

	// The rails own the handoff while they are driving. Without this the autopilot path below hands
	// control over on its own schedule — and in a playable level, where every autopilot is
	// enabled=false, that means IMMEDIATELY: bAutopilotActive is false from frame 0, so the player
	// got control mid-intro while the board was still on rails and ignoring them.
	// FinishRails re-runs this the tick it hands over. See specs/deterministic-ride-handoff.md.
	if (bRailsActive)
	{
		return;
	}

	// A fall is terminal until Replay/Restart: without this guard the finished-autopilot
	// paths below (delay already elapsed) would re-enable controls the very next tick.
	if (bFallen)
	{
		return;
	}

	// State-based handoff: when a StateTriggerAutoPilot is wired up, wait for
	// its bFinished flag (set the tick its last step activates) rather than a
	// duration timer that has no awareness of how long the autopilot's actual
	// step sequence runs. ControlDelayAfterAutoPilot is reused as the camera-
	// transition / "GET READY" grace window between handoff and player input.
	if (StateTriggerAutoPilot)
	{
		// No "Wait..." cue during the autopilot phase anymore: with the skip-paddle state
		// injection (specs/skip-paddle-intro.md) the no-control window is a ~2s cobra→pop-up
		// beat, short enough to need no explanation. Only the "…and surf!" go flash remains
		// (PlayGo self-installs the overlay).
		const bool bAutopilotActive = StateTriggerAutoPilot->enabled && !StateTriggerAutoPilot->bFinished;
		if (bAutopilotActive)
		{
			return;
		}

		if (!bCameraTransitionStarted)
		{
			// The ride stays on the Beside camera end to end — no switch at handoff. This latch
			// still marks the start of the grace window below.
			bCameraTransitionStarted = true;
			UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: Autopilot handoff started (camera stays BESIDE, %.2fs grace)"),
				ControlDelayAfterAutoPilot);
		}

		TimeAfterAutoPilotComplete += DeltaTime;

		if (TimeAfterAutoPilotComplete >= ControlDelayAfterAutoPilot)
		{
			bPlayerControlsEnabled = true;
			OnPlayerControlsEnabled();
			UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: Player controls ENABLED (StateTriggerAutoPilot finished)"));
			AnnounceHandoff();
		}
		return;
	}

	// Legacy timer path (no StateTriggerAutoPilot wired up)
	if (AutoPilotDuration <= 0.0f)
	{
		return;
	}

	// Track autopilot elapsed time
	AutoPilotElapsedTime += DeltaTime;

	// Calculate time remaining until controls are enabled
	float TotalTimeUntilControlsEnabled = AutoPilotDuration + ControlDelayAfterAutoPilot;
	float TimeRemainingUntilControls = TotalTimeUntilControlsEnabled - AutoPilotElapsedTime - TimeAfterAutoPilotComplete;

	// No camera switch before handoff: the Beside camera stays active for the whole ride.
	if (!bCameraTransitionStarted && TimeRemainingUntilControls <= CameraTransitionLeadTime)
	{
		bCameraTransitionStarted = true;
		UE_LOG(LogSurf, Log, TEXT("SurfboardPawn: Handoff window opened, camera stays BESIDE (%.2f seconds before controls enabled)"),
			TimeRemainingUntilControls);
	}

	// Check if autopilot has completed
	if (AutoPilotElapsedTime >= AutoPilotDuration)
	{
		// Autopilot is done, now count down the delay
		TimeAfterAutoPilotComplete += DeltaTime;

		static bool bLoggedAutoPilotComplete = false;
		if (!bLoggedAutoPilotComplete)
		{
			UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: AutoPilot completed! Waiting %.2f seconds before enabling controls..."),
				ControlDelayAfterAutoPilot);
			bLoggedAutoPilotComplete = true;
		}

		// Check if delay has passed
		if (TimeAfterAutoPilotComplete >= ControlDelayAfterAutoPilot)
		{
			// Enable player controls!
			bPlayerControlsEnabled = true;
			OnPlayerControlsEnabled();

			UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: Player controls ENABLED!"));

			AnnounceHandoff();
		}
	}
}

void ASurfboardPawn::AnnounceHandoff()
{
	// The touch controls go live on this same tick (UpdateTouchControls binds them to
	// bPlayerControlsEnabled), and them appearing IS the handoff cue: it says where to put your
	// thumbs, not merely that it is time. Words on top of that were redundant - and words over the
	// wave are exactly what the score counter just lost, for the same reason. So the flash only
	// plays where nothing else marks the moment: the tilt scheme, or a desktop on the old mouse
	// path, where no controls are drawn at all.
	if (WantsTouchControls())
	{
		return;
	}
	RideCue::PlayGo(GetWorld(), FText::FromString(TEXT("…and surf!")));
}

void ASurfboardPawn::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bInputTraceActive)
	{
		StopInputTrace(TEXT("endplay"));
	}
	EndAssistRide();   // bank the ride's credit before the world goes away

	// The trace was closed two lines up, so the record can name it (D8). This is the path a ride
	// that never ended in a fall takes - a restart, or quitting the app.
	CommitRideRecords();

	SensorProbe::Uninstall(GetWorld());
	SurfTuningHUD::Uninstall(GetWorld());
	WaveRadar::Uninstall(GetWorld());
	ReplayOverlay::Uninstall(GetWorld());
	RideListPanel::Uninstall(GetWorld());
	AboutPanel::Uninstall(GetWorld());
	StartTutorial::Uninstall(GetWorld());
	RideCue::Uninstall(GetWorld());
	RideScore::Uninstall(GetWorld());
	RideHud::Uninstall(GetWorld());
	WipeoutPanel::Uninstall(GetWorld());
	StaminaBar::Uninstall(GetWorld());
	// The tuning subsystem outlives this pawn and its level; a tired layer left on would tire the
	// next ride from its first tick (Back mid-spell is the case that reaches here still tired).
	if (Tuning && Tuning->IsTired())
	{
		Tuning->SetTired(false);
	}
	TouchControls::Uninstall(GetWorld());   // every other overlay was here; this one was not
	Super::EndPlay(EndPlayReason);
}

void ASurfboardPawn::StartInputTrace()
{
	// A trace is being recorded, so there is going to be a ride worth listing - whatever the assist
	// is doing. The score is filled in later by EndAssistRide if an assisted ride ran; if none did,
	// this ride is committed at zero and still shows up as LAST RIDE, because "watch what I just
	// did" is the promise the Replay button has always made and must keep.
	bRideAwaitingCommit = true;
	PendingRideScoreCredit = 0.0f;

	const FDateTime Now = FDateTime::Now();
	const FString Timestamp = Now.ToString(TEXT("%Y-%m-%d-%H-%M-%S"));
	const FString SessionName = InputTraceSessionPrefix.IsEmpty()
		? Timestamp
		: (InputTraceSessionPrefix + TEXT("-") + Timestamp);

	InputTraceFilePath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("InputTraces"),
		SessionName + TEXT(".csv"));

#if PLATFORM_ANDROID
	InputTraceSourceTag = (AndroidWeightInputSource == EAndroidWeightInputSource::Tilt)
		? TEXT("tilt") : TEXT("stick");
	const FString PlatformName = TEXT("Android");
#else
	InputTraceSourceTag = TEXT("mouse");
	const FString PlatformName = TEXT("PC");
#endif

	const FString MapName = GetWorld() ? GetWorld()->GetMapName() : FString(TEXT("(no world)"));

	// Metadata block + CSV header, written synchronously at start (~600 bytes).
	FString Header;
	Header.Reserve(1024);
	Header += FString::Printf(TEXT("# session=%s\n"), *SessionName);
	Header += FString::Printf(TEXT("# platform=%s\n"), *PlatformName);
	Header += FString::Printf(TEXT("# map=%s\n"), *MapName);
	// FR4: which board rode this. Not derivable from anywhere else - the filename carries only a
	// timestamp - and both the per-board records and D1's board swap need it. Traces recorded
	// before this line exist and are unattributable for ever; that is acceptable only because
	// there are no users yet (D4).
	Header += FString::Printf(TEXT("# board=%s\n"), *GetActiveBoardId());
	Header += FString::Printf(TEXT("# neutral_gravity=(%.4f, %.4f, %.4f)\n"),
		TiltBasis.NeutralGravity.X, TiltBasis.NeutralGravity.Y, TiltBasis.NeutralGravity.Z);
	Header += FString::Printf(TEXT("# tilt_pitch_for_full=%.2f\n"), GetEffectiveTiltPitchForFull());
	Header += FString::Printf(TEXT("# tilt_roll_for_full=%.2f\n"), GetEffectiveTiltRollForFull());
	Header += FString::Printf(TEXT("# tilt_deadzone_deg=%.2f\n"), TiltAngleDeadzoneDegrees);
	Header += FString::Printf(TEXT("# invert_pitch=%s\n"), bInvertTiltPitch ? TEXT("true") : TEXT("false"));
	Header += FString::Printf(TEXT("# invert_roll=%s\n"), bInvertTiltRoll ? TEXT("true") : TEXT("false"));
	// Gyro-yaw fusion provenance. Not consumed by replay — the fused angle is already
	// baked into the tilt_roll_deg column — but records which sign/leak produced the
	// recording. TryParseMetadataLine ignores unknown keys, so old and new traces are
	// mutually compatible. See specs/tilt-yaw-fusion.md.
	Header += FString::Printf(TEXT("# tilt_yaw_gain=%.3f\n"),
		GetEffectiveTiltYawGain() * GetEffectiveTiltYawSign());
	Header += FString::Printf(TEXT("# tilt_yaw_leak=%.3f\n"), GetEffectiveTiltYawLeakSeconds());
	Header += FString::Printf(TEXT("# tilt_yaw_bias_rads=%.5f\n"), TiltYaw.RateBias);
	Header += FString::Printf(TEXT("# weight_auto_center=%.3f\n"), WeightAutoCenterDuration);
	Header += FString::Printf(TEXT("# source=%s\n"), *InputTraceSourceTag);
	// Wave-clock frame at record start — quick-inspection anchor + fallback for on-device
	// replay wave re-sync. Per-tick values are in the wave_frame column. See
	// specs/on-device-ride-replay.md.
	Header += FString::Printf(TEXT("# wave_start_frame=%d\n"), ReadWaveFrame());
	// Columns 0-5 are the input (parsed by AInputReplayAutoPilot::LoadTrace, which reads
	// exactly Cols[0..5]). Columns 6-14 are the board's own trajectory at record time — the
	// phone's actual pos/vel/rotation — so a PC replay can be diffed against the recording
	// instead of judging divergence by feel (see specs/trace-trajectory-comparison.md).
	// Column 15 (wave_frame) is the wave clock, appended last so the index-based Cols[0..14]
	// parsers above are unaffected; consumed by on-device kinematic replay for wave re-sync.
	// Columns 16-27 are the SprayController's per-site outputs (ejection velocity + spawn rate,
	// post gate/budget), appended last for the same index-compat reason; played back verbatim by
	// the kinematic replay so replay spray IS the live spray. Zeros in levels without spray.
	// Columns 28-29 are the weight amounts (0..1, 0.5 = centered) driving the surfer rider's
	// lean/twist — replayed through USurferAnimInstance::SetReplayWeights (the live
	// WeightDistribution is tick-frozen during replay). See specs/surfer-rider-lean.md.
	Header += TEXT("t,tilt_pitch_deg,tilt_roll_deg,stick_x,stick_y,source,")
		TEXT("board_x,board_y,board_z,board_vx,board_vy,board_vz,board_roll,board_pitch,board_yaw,")
		TEXT("wave_frame,")
		TEXT("spray_vl_x,spray_vl_y,spray_vl_z,spray_rl,")
		TEXT("spray_vr_x,spray_vr_y,spray_vr_z,spray_rr,")
		TEXT("spray_vt_x,spray_vt_y,spray_vt_z,spray_rt,")
		TEXT("weight_front,weight_right,")
		// Angular velocity (deg/s, columns 30-32). Linear velocity was always recorded (cols 9-11)
		// but never read back; both are now parsed into FReplayRow so a rails intro can stamp the
		// board's exact resume state. Never derive these from the solver on the handoff frame —
		// see specs/deterministic-ride-handoff.md.
		TEXT("board_avx,board_avy,board_avz,rider_anim,pump_phase\n");

	// FFileHelper writes the file but won't create missing parent directories.
	IFileManager& Fm = IFileManager::Get();
	const FString Dir = FPaths::GetPath(InputTraceFilePath);
	if (!Fm.DirectoryExists(*Dir))
	{
		Fm.MakeDirectory(*Dir, /*Tree*/true);
	}

	const bool bOk = FFileHelper::SaveStringToFile(
		Header, *InputTraceFilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	if (!bOk)
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: InputTrace FAILED to create %s"), *InputTraceFilePath);
		return;
	}

	InputTraceBuffer.Reset();
	InputTraceBuffer.Reserve(128 * 1024); // ~10s @ 60Hz × ~110B/row (input+pose) ≈ 66KB; pad to avoid reallocs.
	InputTraceElapsedTime = 0.0f;
	InputTraceTimeSinceFlush = 0.0f;
	bInputTraceActive = true;

	UE_LOG(LogSurf, Display,
		TEXT("SurfboardPawn: InputTrace STARTED -> %s (source=%s)"),
		*InputTraceFilePath, *InputTraceSourceTag);
}

void ASurfboardPawn::SampleInputTrace(float DeltaTime)
{
	if (!bInputTraceActive)
	{
		return;
	}

	InputTraceElapsedTime += DeltaTime;
	InputTraceTimeSinceFlush += DeltaTime;

	// Board trajectory at this instant — same mesh accessors the replay's trajectory
	// recorder uses (AInputReplayAutoPilot::RecordTrajectorySample), so the recording and
	// a later PC replay are directly comparable. Zeros if the mesh is missing.
	FVector BoardPos = FVector::ZeroVector;
	FVector BoardVel = FVector::ZeroVector;
	FVector BoardAngVel = FVector::ZeroVector;
	FRotator BoardRot = FRotator::ZeroRotator;
	if (SurfboardActor)
	{
		if (UStaticMeshComponent* Mesh = SurfboardActor->GetStaticMeshComponent())
		{
			BoardPos = Mesh->GetComponentLocation();
			BoardRot = Mesh->GetComponentRotation();
			// No IsSimulatingPhysics guard: the getters read the particle with no such check, so a
			// kinematic board (a rails intro being re-recorded) reports its velocity normally. The
			// old guard would have silently written zeros in exactly that case.
			BoardVel = Mesh->GetPhysicsLinearVelocity();
			BoardAngVel = Mesh->GetPhysicsAngularVelocityInDegrees();
		}
	}

	// Wave clock at this instant, so an on-device replay can re-sync the wave surface
	// (a pure function of this frame) to the recorded ride. -1 if no WaterController.
	const int32 WaveFrame = ReadWaveFrame();

	// Spray outputs at this instant (zeros in levels without a SprayController), so a replay
	// can play the live spray back verbatim. See specs/on-device-ride-replay.md.
	FVector SprayVelL = FVector::ZeroVector, SprayVelR = FVector::ZeroVector, SprayVelT = FVector::ZeroVector;
	float SprayRateL = 0.0f, SprayRateR = 0.0f, SprayRateT = 0.0f;
	if (ASprayController* Spray = ResolveSprayController())
	{
		Spray->GetSprayOutputs(SprayVelL, SprayRateL, SprayVelR, SprayRateR, SprayVelT, SprayRateT);
	}

	// Scripted rider animation state, so a rails intro can replay the animation as well as the
	// motion (see the rider_anim column note in StartInputTrace).
	int32 RiderAnim = (int32)ESurferAnimState::Paddle;
	if (USurferAnimInstance* RA = ResolveRiderAnim())
	{
		RiderAnim = (int32)RA->AnimState;
	}

	// Weight amounts driving the rider's lean/twist (0.5/0.5 = centered when no actor is wired).
	float WeightFront = 0.5f;
	float WeightRight = 0.5f;
	if (WeightDistribution)
	{
		WeightFront = WeightDistribution->amountInFront;
		WeightRight = WeightDistribution->amountToTheRight;
	}

	// Pump stroke, so a replay can show the rider pumping. The FEEDBACK channel, not the force one:
	// the phase is what the animation samples, and it reads the same whether or not the stroke was
	// achieving anything. -1 = not pumping.
	const float PumpPhaseForTrace = (WeightDistribution && WeightDistribution->bPumpActive)
		? WeightDistribution->PumpStrokePhase : -1.0f;

	InputTraceBuffer += FString::Printf(
		TEXT("%.3f,%.3f,%.3f,%.3f,%.3f,%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%d,")
		TEXT("%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%.3f,")
		TEXT("%.2f,%.2f,%.2f,%d,%.3f\n"),
		InputTraceElapsedTime,
		LatestTiltPitchDeg,
		LatestTiltRollDeg,
		LatestStickValue.X,
		LatestStickValue.Y,
		*InputTraceSourceTag,
		BoardPos.X, BoardPos.Y, BoardPos.Z,
		BoardVel.X, BoardVel.Y, BoardVel.Z,
		BoardRot.Roll, BoardRot.Pitch, BoardRot.Yaw,
		WaveFrame,
		SprayVelL.X, SprayVelL.Y, SprayVelL.Z, SprayRateL,
		SprayVelR.X, SprayVelR.Y, SprayVelR.Z, SprayRateR,
		SprayVelT.X, SprayVelT.Y, SprayVelT.Z, SprayRateT,
		WeightFront, WeightRight,
		BoardAngVel.X, BoardAngVel.Y, BoardAngVel.Z, RiderAnim, PumpPhaseForTrace);

	// Periodic background flush. ~30 KB every ~10 s.
	if (InputTraceTimeSinceFlush >= 10.0f)
	{
		FlushInputTrace(/*bAsync*/true);
	}

	// Discard everything before the autopilot's wave-clock injection.
	//
	// -RecordIntro starts at BeginPlay, but AStateTriggerAutoPilot::Start() is DEFERRED, and it is
	// Start() that applies StartWaveFrame. So the opening ~1s of a recording is the dead zone
	// skip-paddle-intro documents: the board sits undriven while the wave free-runs at the PRE-jump
	// frame, and then the clock jumps (measured: 910 -> 1009). A rails intro replays that faithfully
	// — including the jump — which is seen as the wave starting in one place and snapping to another
	// shortly after the level loads.
	//
	// So when the clock jumps forward hard, treat that instant as t=0 and throw away what came
	// before. Guards: only while still recording the intro, only before the first flush (nothing has
	// reached disk yet), only inside the opening window, and only on a large FORWARD jump — the wave
	// loop wraps 1078 -> 886, which is a large NEGATIVE delta and must not trigger this.
	if (bRecordingIntroTrace && !bIntroTraceRebased && InputTraceElapsedTime < IntroRebaseWindowSeconds
		&& InputTraceTimeSinceFlush == InputTraceElapsedTime) // no flush has happened yet
	{
		if (LastRecordedWaveFrame >= 0 && WaveFrame - LastRecordedWaveFrame > IntroRebaseMinJump)
		{
			UE_LOG(LogSurf, Warning,
				TEXT("SurfboardPawn: intro trace rebased at %.2fs — wave clock jumped %d -> %d (autopilot state injection). ")
				TEXT("Dropped %d dead-zone row(s) so the trace starts where the ride starts."),
				InputTraceElapsedTime, LastRecordedWaveFrame, WaveFrame, IntroRowsRecorded);
			InputTraceBuffer.Reset();
			InputTraceBuffer.Reserve(128 * 1024);
			InputTraceElapsedTime = 0.0f;
			InputTraceTimeSinceFlush = 0.0f;
			IntroRowsRecorded = 0;
			bIntroTraceRebased = true;
		}
	}
	LastRecordedWaveFrame = WaveFrame;
	++IntroRowsRecorded;

	// -RecordIntroSeconds=N: capture a fixed window from BeginPlay instead of stopping at handoff.
	// Needed because in a FILTERED test run the level's intro autopilot is disabled, so handoff
	// fires ~1s in and the stop-at-handoff rule would only ever yield a 1s trace — far too short to
	// exercise the rails against a long autopilot ride.
	if (bRecordingIntroTrace && IntroRecordSeconds > 0.0f && InputTraceElapsedTime >= IntroRecordSeconds)
	{
		StopInputTrace(TEXT("intro_recorded"));
		bRecordingIntroTrace = false;
		UE_LOG(LogSurf, Warning,
			TEXT("SurfboardPawn: intro trace captured (BeginPlay -> %.2fs, fixed window)."),
			IntroRecordSeconds);
		return;
	}

	if (InputTraceElapsedTime >= MaxRecordingSeconds)
	{
		UE_LOG(LogSurf, Display,
			TEXT("SurfboardPawn: InputTrace MaxRecordingSeconds (%.1fs) reached — stopping"),
			MaxRecordingSeconds);
		StopInputTrace(TEXT("max_seconds"));
	}
}

void ASurfboardPawn::FlushInputTrace(bool bAsync)
{
	if (InputTraceBuffer.IsEmpty())
	{
		InputTraceTimeSinceFlush = 0.0f;
		return;
	}

	// Snapshot buffer + path so the (potentially async) write can't race with
	// continued recording. The member buffer is reset and ready for new rows.
	FString Payload = MoveTemp(InputTraceBuffer);
	InputTraceBuffer.Reset();
	InputTraceBuffer.Reserve(128 * 1024);
	const FString PathCopy = InputTraceFilePath;
	InputTraceTimeSinceFlush = 0.0f;

	auto DoWrite = [Payload = MoveTemp(Payload), PathCopy]()
	{
		// Append — each flush opens its own handle, so concurrent background
		// flushes don't share file state.
		const bool bOk = FFileHelper::SaveStringToFile(
			Payload, *PathCopy,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&IFileManager::Get(),
			FILEWRITE_Append);

		if (!bOk)
		{
			UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn: InputTrace flush FAILED -> %s"), *PathCopy);
		}
	};

	if (bAsync)
	{
		// Fire and forget; the future is discarded.
		Async(EAsyncExecution::ThreadPool, MoveTemp(DoWrite));
	}
	else
	{
		DoWrite();
	}
}

void ASurfboardPawn::StopInputTrace(const TCHAR* Why)
{
	if (!bInputTraceActive)
	{
		return;
	}

	// Footer: why the rows stop here. A trace that just ends used to look the same whether the
	// ride ended, the level was left, or the 120 s cap hit - and "did the game decide the ride was
	// over?" is exactly the question a trace shared from the phone has to answer (specs/
	// share-trace-from-phone.md FR4). Every parser skips '#' lines wherever they sit.
	AppendTraceNote(FString::Printf(TEXT("trace_stop=%s t=%.3f"), Why, InputTraceElapsedTime));

	// Final flush is synchronous — EndPlay / safety-cap path runs near
	// teardown, and a deferred async write might lose the tail.
	FlushInputTrace(/*bAsync*/false);
	bInputTraceActive = false;

	UE_LOG(LogSurf, Display,
		TEXT("SurfboardPawn: InputTrace STOPPED at %.2fs -> %s"),
		InputTraceElapsedTime, *InputTraceFilePath);
}

void ASurfboardPawn::AppendTraceNote(const FString& Note)
{
	const FString Line = TEXT("# ") + Note + TEXT("\n");
	if (bInputTraceActive)
	{
		InputTraceBuffer += Line;   // lands between the rows it belongs to, in order
		return;
	}
	// Closed file: the note still belongs to this ride (the card opens a second after the trace
	// stops). Appended synchronously; every flush already opens its own handle, so nothing is held.
	if (!InputTraceFilePath.IsEmpty() && FPaths::FileExists(InputTraceFilePath))
	{
		FFileHelper::SaveStringToFile(Line, *InputTraceFilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
	}
}

void ASurfboardPawn::FlushInputTraceForSharing()
{
	if (bInputTraceActive)
	{
		AppendTraceNote(FString::Printf(TEXT("shared_mid_ride t=%.3f"), InputTraceElapsedTime));
		FlushInputTrace(/*bAsync*/false);
	}
}

// ======================================================================================
//  Ride records, retention, and the ride list — see specs/best-ride-replay.md
// ======================================================================================

FString ASurfboardPawn::TraceDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("InputTraces"));
}

void ASurfboardPawn::CommitRideRecords()
{
	// FR7. A scripted run records traces like any other, so without this an automated sweep would
	// overwrite a real best with an autopilot's run and then prune the trace it replaced.
	if (SurfBoards::IsScriptedRun(bExternalWeightOverride))
	{
		return;
	}
	if (!bRideAwaitingCommit || InputTraceFilePath.IsEmpty())
	{
		return;
	}
	bRideAwaitingCommit = false;

	// D8: this runs where the file is CLOSED, so the trace named here is final. Committing at the
	// other three EndAssistRide sites would name a file that goes on growing for the rest of the
	// wave, and the replay would show more ride than the record was scored on.
	if (bInputTraceActive)
	{
		UE_LOG(LogSurf, Warning,
			TEXT("SurfboardPawn::CommitRideRecords - trace still open; not committing a growing file"));
		return;
	}

	const FString BoardId = GetActiveBoardId();

	SurfAssist::FRideRecord Record;
	Record.ScoreCredit     = FMath::Max(0.0f, PendingRideScoreCredit);
	Record.TraceFile       = FPaths::GetCleanFilename(InputTraceFilePath);
	Record.BoardId         = BoardId;
	Record.DurationSeconds = InputTraceElapsedTime;
	Record.RecordedAt      = FDateTime::Now();

	// A ride nobody could watch is not worth a record - and a record pointing at a file that was
	// never written is exactly FR2's dead button.
	if (!FPaths::FileExists(InputTraceFilePath))
	{
		UE_LOG(LogSurf, Warning,
			TEXT("SurfboardPawn::CommitRideRecords - %s is not on disk; no record written"),
			*InputTraceFilePath);
		return;
	}

	// Latest always. This is the record the old Replay button's behaviour now lives in, and D3's
	// reason for storing it explicitly rather than scanning for the newest file: once a best is
	// preserved while newer traces are pruned, "newest file" and "last ride" come apart.
	SurfAssist::SaveRideRecord(BoardId, SurfAssist::ERideSlot::Latest, Record);

	// Best only if it beat the stored one. Compared on RAW credit, the same currency the float best
	// is stored in, so the two can never disagree about which ride was better.
	//
	// A zero-scored ride never becomes a best: with the assist switched off no score is banked at
	// all, and a "best" of 0 would sit in the list claiming to be the player's finest work.
	SurfAssist::FRideRecord PreviousBest;
	const bool bHadBest = SurfAssist::LoadRideRecord(BoardId, SurfAssist::ERideSlot::Best, PreviousBest);
	if (Record.ScoreCredit > 0.0f && (!bHadBest || Record.ScoreCredit > PreviousBest.ScoreCredit))
	{
		SurfAssist::SaveRideRecord(BoardId, SurfAssist::ERideSlot::Best, Record);
		UE_LOG(LogSurf, Display,
			TEXT("SurfboardPawn: new BEST record for '%s' - %d points, %.1fs, %s"),
			*BoardId, SurfAssist::ScoreFromCredit(Record.ScoreCredit),
			Record.DurationSeconds, *Record.TraceFile);
	}

	// FR3's ordering, and the one it warns about: the record is committed BEFORE anything is
	// deleted, so a prune can never eat the ride it was about to preserve.
	PruneTraces();
}

/** The kill switch for the one irreversible thing this feature does.
 *
 *  FR3 calls the prune "the requirement most likely to cause data loss if implemented casually",
 *  and on a machine that has been recording since April it is not a trickle: the first live ride
 *  after this shipped had ~2600 traces (239 MB) in range. The grace period is the only thing
 *  standing between a developer's capture and the bin, because every trace on disk shares one
 *  session prefix - so the exemption D9 suggested as the cheap fix cannot tell them apart.
 *
 *  2 = say what would go without touching anything. Use it before the first ride on a machine with
 *  history worth keeping. */
static TAutoConsoleVariable<int32> CVarTracePrune(
	TEXT("surf.traces.prune"), 1,
	TEXT("Retention for Saved/InputTraces (best-ride-replay.md FR3).\n")
	TEXT("1 = delete unreferenced traces older than the grace period (default).\n")
	TEXT("0 = never delete anything.\n")
	TEXT("2 = dry run: log what would be deleted, delete nothing."),
	ECVF_Default);

void ASurfboardPawn::PruneTraces()
{
	if (SurfBoards::IsScriptedRun(bExternalWeightOverride))
	{
		return;   // FR7: a test run must never delete a real best
	}

	const int32 PruneMode = CVarTracePrune.GetValueOnGameThread();
	if (PruneMode == 0)
	{
		return;
	}
	const bool bDryRun = (PruneMode == 2);

	const FString Dir = TraceDirectory();
	IFileManager& Fm = IFileManager::Get();

	TArray<FString> Files;
	Fm.FindFiles(Files, *(Dir / TEXT("*.csv")), /*Files*/true, /*Directories*/false);
	if (Files.Num() == 0)
	{
		return;
	}

	const TSet<FString> Referenced = SurfAssist::ReferencedTraceFiles();
	const FDateTime Cutoff = FDateTime::Now() - FTimespan::FromDays(kTraceGraceDays);

	int32 Deleted = 0;
	for (const FString& File : Files)
	{
		// Protected for ever: anything a record names (FR3). Two of these per board, and they are
		// the whole reason the list has rows.
		if (Referenced.Contains(File))
		{
			continue;
		}
		// Protected while it is open or on screen: the ride being recorded, and the ride being
		// watched. Neither is referenced by a record yet, and deleting either is silent corruption.
		if (File == FPaths::GetCleanFilename(InputTraceFilePath) || File == ReplayingTraceFileName)
		{
			continue;
		}

		// D9: the grace period is not slack, it is the development machine. -RecordIntro rails
		// captures and PC tuning traces land in this directory too, and "delete anything
		// unreferenced" would take the capture made ten minutes ago to stage as a rails intro.
		const FString Full = Dir / File;
		if (Fm.GetTimeStamp(*Full) > Cutoff)
		{
			continue;
		}

		if (bDryRun)
		{
			++Deleted;
			continue;
		}
		if (Fm.Delete(*Full, /*RequireExists*/false, /*EvenReadOnly*/false, /*Quiet*/true))
		{
			++Deleted;
		}
	}

	if (Deleted > 0)
	{
		// Warning, not Display: deleting is irreversible and the count is worth seeing in a log
		// somebody skims. A big number here on a development machine is the backlog going, once.
		UE_LOG(LogSurf, Warning,
			TEXT("SurfboardPawn: %s %d trace(s) older than %d days that no record references%s"),
			bDryRun ? TEXT("WOULD prune") : TEXT("pruned"), Deleted, kTraceGraceDays,
			bDryRun ? TEXT(" (surf.traces.prune 2 = dry run)") : TEXT(""));
	}
}

void ASurfboardPawn::DeleteTraceFiles(const TSet<FString>& FileNames)
{
	if (FileNames.Num() == 0 || SurfBoards::IsScriptedRun(bExternalWeightOverride))
	{
		return;
	}
	const FString Dir = TraceDirectory();
	IFileManager& Fm = IFileManager::Get();
	int32 Deleted = 0;
	for (const FString& Name : FileNames)
	{
		if (Name.IsEmpty() || Name == ReplayingTraceFileName)
		{
			continue;
		}
		if (Fm.Delete(*(Dir / Name), /*RequireExists*/false, /*EvenReadOnly*/false, /*Quiet*/true))
		{
			++Deleted;
		}
	}
	UE_LOG(LogSurf, Display, TEXT("SurfboardPawn: reset deleted %d trace(s) the cleared records held"), Deleted);
}

TArray<FRideListEntry> ASurfboardPawn::BuildRideList() const
{
	TArray<FRideListEntry> Entries;
	const USurfBoardSubsystem* Boards = SurfBoards::Get(this);
	const FString Dir = TraceDirectory();
	const FString ActiveId = GetActiveBoardId();

	// Resolve a record into a row, or into nothing at all. FR2: a record whose trace has gone is
	// not drawn greyed and is never quietly replaced by a different ride - it simply has no row.
	auto AddEntry = [&](const SurfAssist::FRideRecord& R, bool bLast) -> void
	{
		const FString Path = Dir / R.TraceFile;
		if (!FPaths::FileExists(Path))
		{
			UE_LOG(LogSurf, Warning,
				TEXT("SurfboardPawn: ride record for '%s' names %s, which is gone - offering no row"),
				*R.BoardId, *R.TraceFile);
			return;
		}

		FRideListEntry E;
		E.KindLabel       = bLast ? TEXT("LAST RIDE") : TEXT("BEST");
		E.BoardId         = R.BoardId;
		E.BoardName       = R.BoardId;
		E.DurationSeconds = R.DurationSeconds;
		E.TracePath       = Path;
		E.bIsLastRide     = bLast;
		E.bIsCurrentBoard = (R.BoardId == ActiveId);

		// The multiplier is applied HERE and nowhere else, to both sides of every comparison - the
		// same rule EndAssistRide follows. Baking it into a stored record would put one board's
		// history into two currencies the first time it is re-tuned.
		//
		// And it is the RECORDED board's multiplier, not the one being ridden: a row states what
		// that ride was worth, so reading it through whatever board happens to be underneath the
		// player would make the same ride show two different scores on two different days.
		int32 Difficulty = 1;
		if (Boards)
		{
			for (int32 i = 0; i < Boards->GetBoardCount(); ++i)
			{
				if (const FSurfBoardProfile* P = Boards->GetProfile(i))
				{
					if (P->Id == R.BoardId)
					{
						E.BoardName = P->DisplayName;
						Difficulty = P->RatingDifficulty;
						break;
					}
				}
			}
		}
		const float Multiplier = TrickScore::BoardMultiplier(Difficulty, GetTrickTuning().BoardDifficultyK);
		E.Score = TrickScore::ScoreShown(R.ScoreCredit, 0.0f, Multiplier);

		Entries.Add(E);
	};

	// Row 1 is the last ride, whatever board it was on: "watch what I just did" is the common case
	// and the reason the one-tap Replay button could afford to become a list at all (D10).
	SurfAssist::FRideRecord Latest;
	FString LatestFile;
	if (SurfAssist::LoadRideRecord(ActiveId, SurfAssist::ERideSlot::Latest, Latest))
	{
		LatestFile = Latest.TraceFile;
		AddEntry(Latest, /*bLast*/true);
	}

	// Then one row per board's best, in rack order so the two screens agree about what comes first.
	if (Boards)
	{
		for (int32 i = 0; i < Boards->GetBoardCount(); ++i)
		{
			const FSurfBoardProfile* P = Boards->GetProfile(i);
			if (!P) { continue; }

			SurfAssist::FRideRecord Best;
			if (!SurfAssist::LoadRideRecord(P->Id, SurfAssist::ERideSlot::Best, Best))
			{
				continue;   // never ridden: no row, which is FR6's "only rows that lead somewhere"
			}
			if (Best.TraceFile == LatestFile)
			{
				continue;   // the ride they just did IS their best: one row, not the same ride twice
			}
			AddEntry(Best, /*bLast*/false);
		}
	}

	return Entries;
}

bool ASurfboardPawn::HasWatchableRides() const
{
	return BuildRideList().Num() > 0;
}

void ASurfboardPawn::OpenRideList()
{
	UWorld* World = GetWorld();
	if (!World || RideListPanel::IsOpen(World))
	{
		return;
	}

	const TArray<FRideListEntry> Entries = BuildRideList();
	if (Entries.Num() == 0)
	{
		// FR6: never a dead control. The button should be hidden by HasWatchableRides() long before
		// this, so reaching here means a Blueprint called it unconditionally - say so, once.
		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn::OpenRideList - nothing watchable; not opening"));
		return;
	}

	// Keep the rows alive for as long as the panel: the widget reads them during Construct, but the
	// hooks below index back into this array every time a row is pressed.
	RideListEntries = Entries;

	TWeakObjectPtr<ASurfboardPawn> WeakThis(this);
	RideListPanel::FHooks Hooks;
	Hooks.PlayEntry = [WeakThis](int32 Index)
	{
		if (ASurfboardPawn* Pawn = WeakThis.Get())
		{
			Pawn->PlayRideListEntry(Index);
		}
	};
	Hooks.OnClosed = [WeakThis](bool bToPlay)
	{
		if (ASurfboardPawn* Pawn = WeakThis.Get())
		{
			// NFR3: the ride HUD comes back only once the Slate panel is gone. While both are up,
			// one of the two silently stops responding to taps.
			Pawn->bRideUIBlocked = Pawn->bStartScreenActive;
			Pawn->UpdateRideHud();

			// Closing the list is also how a player leaves a replay (D7): the list is the only
			// screen in front of them while one is on hold, so its Back button is the way out.
			// This runs before the row's PlayEntry, so picking a second ride tears the first one
			// down and starts the next from a restored world rather than from a borrowed board and
			// a hijacked wave clock.
			//
			// A replay that came from the hub goes back to the hub, by reload: there is no
			// mid-ride world behind it for LeaveReplay to restore (two-screen-navigation.md FR4).
			// Picking another row from that list is the one exception - the reload would throw
			// the pick away - so the first replay is left the ordinary way and the next one starts
			// from the restored world, as it always has.
			if (Pawn->bReplayActive)
			{
				if (Pawn->bReplayFromHub && !bToPlay)
				{
					Pawn->ReturnToHub();
				}
				else
				{
					Pawn->LeaveReplay();
				}
			}
		}
	};

	// Set here, not polled in Tick: the list pauses the world, so the pawn stops ticking the moment
	// it opens and a polled flag could never turn true. Same reason the rack does it this way.
	bRideUIBlocked = true;
	UpdateRideHud();

	RideListPanel::Open(World, RideListEntries, Hooks, RideListHighlightIndex);
}

void ASurfboardPawn::PlayRideListEntry(int32 Index)
{
	if (!RideListEntries.IsValidIndex(Index))
	{
		return;
	}
	RideListHighlightIndex = Index;   // D5: this is the row the list comes back on

	const FRideListEntry& Entry = RideListEntries[Index];

	SurfAssist::FRideRecord Record;
	Record.TraceFile = FPaths::GetCleanFilename(Entry.TracePath);
	Record.BoardId   = Entry.BoardId;

	// From the hub (specs/two-screen-navigation.md FR4): the replay is driven from Tick, so the
	// world has to run and the menu has to go. Nothing that the menu was holding back matters -
	// the force pipeline and the intro autopilot are frozen by EnterReplayMode, and leaving the
	// replay reloads the level, which is how the hub is defined. So the start screen is simply
	// torn down here rather than suspended and restored.
	//
	// Sticky across rows: picking a second ride from the list a finished hub replay reopened goes
	// through LeaveReplay (the world is restored, not reloaded, so the pick survives) and lands
	// here with the menu long gone - it is still a hub replay, and must still exit to the hub.
	const bool bFromHub = bStartScreenActive || bReplayFromHub;
	if (bStartScreenActive)
	{
		StartTutorial::Uninstall(GetWorld());
		bStartScreenActive = false;
		bRideUIBlocked = false;
		if (UWorld* World = GetWorld())
		{
			UGameplayStatics::SetGamePaused(World, false);
		}
		ApplyGameplayInputMode(Cast<APlayerController>(Controller));
	}

	if (!StartReplayOfRecord(Record))
	{
		// The trace would not load and the menu is already gone: the honest recovery is the hub
		// itself, freshly built, rather than a pre-intro world with no menu and no ride.
		if (bFromHub)
		{
			ReturnToHub();
		}
		return;
	}
	bReplayFromHub = bFromHub;
	UpdateRideHud();   // the Back pill is the replay's exit
}

bool ASurfboardPawn::StartReplayOfRecord(const SurfAssist::FRideRecord& Record)
{
	const FString Path = TraceDirectory() / Record.TraceFile;

	// Load FIRST, change nothing until it works. A half-entered replay - board swapped, physics
	// frozen, no rows - is the state FR5 warns about and there is no way back out of it.
	if (!LoadTraceFile(Path))
	{
		UE_LOG(LogSurf, Warning,
			TEXT("SurfboardPawn::StartReplayOfRecord - %s would not load; staying live"), *Path);
		return false;
	}
	if (ReplayRows.Num() < 2)
	{
		return false;
	}

	ReplayingTraceFileName = Record.TraceFile;

	CapturePreReplayState();
	ApplyReplayBoardVisuals(Record.BoardId);

	// A fall leaves the rider off the deck; the replay drives it from recorded weights.
	RestoreRiderAfterFall();
	EnterReplayMode();
	return true;
}

void ASurfboardPawn::CapturePreReplayState()
{
	PreReplay = FPreReplayState();
	PreReplay.bValid            = true;
	PreReplay.WaveFrame         = ReadWaveFrame();
	PreReplay.bBehindCamera     = bUseBehindCamera;
	PreReplay.bControlsEnabled  = bPlayerControlsEnabled;
	PreReplay.bRadarWasUp       = WantsWaveRadar();
	PreReplay.BoardId           = GetActiveBoardId();

	if (SurfboardActor)
	{
		PreReplay.BoardTransform = SurfboardActor->GetActorTransform();
		if (UStaticMeshComponent* Mesh = SurfboardActor->GetStaticMeshComponent())
		{
			PreReplay.bWasSimulating     = Mesh->IsSimulatingPhysics();
			PreReplay.LinearVelocity     = Mesh->GetPhysicsLinearVelocity();
			PreReplay.AngularVelocityRad = Mesh->GetPhysicsAngularVelocityInRadians();
		}
	}
}

void ASurfboardPawn::ApplyReplayBoardVisuals(const FString& BoardId)
{
	// D1, and precisely this much of it: the replay is kinematic, so nothing about tuning
	// coefficients or assist level is consulted - none of it is simulated. Routing this through the
	// full profile-application path would do work the replay cannot use, on a path where a
	// half-applied profile is a mess.
	if (BoardId.IsEmpty() || BoardId == GetActiveBoardId())
	{
		return;
	}
	USurfBoardSubsystem* Boards = SurfBoards::Get(this);
	if (!Boards || !SurfboardActor)
	{
		return;
	}
	for (int32 i = 0; i < Boards->GetBoardCount(); ++i)
	{
		const FSurfBoardProfile* P = Boards->GetProfile(i);
		if (P && P->Id == BoardId)
		{
			SurfBoards::ApplyVisuals(SurfboardActor, *P);
			UE_LOG(LogSurf, Display,
				TEXT("SurfboardPawn: replay borrows board '%s' (visuals only)"), *BoardId);
			return;
		}
	}
	UE_LOG(LogSurf, Warning,
		TEXT("SurfboardPawn: trace names board '%s', which is not installed - replaying on the current board"),
		*BoardId);
}

void ASurfboardPawn::LeaveReplay()
{
	if (!bReplayActive)
	{
		return;
	}

	bReplayActive = false;
	bReplayHolding = false;
	ReplayingTraceFileName.Reset();

	// Give the board back before anything redraws (D5's ordering constraint): the rack and the list
	// both read the player's board, and a screen that comes back showing the borrowed one is a wrong
	// selection in the one place the player reads their selection from.
	if (PreReplay.bValid && !PreReplay.BoardId.IsEmpty())
	{
		ApplyReplayBoardVisuals(PreReplay.BoardId);
	}

	// Stop driving the spray from recorded columns, or the controller keeps spraying the last
	// frame's fan for ever.
	if (ASprayController* Spray = ResolveSprayController())
	{
		Spray->SetReplaySpray(FVector::ZeroVector, 0.0f, FVector::ZeroVector, 0.0f,
		                      FVector::ZeroVector, 0.0f);
		Spray->ClearReplaySpray();
	}

	ReplayOverlay::Uninstall(GetWorld());

	// Put the world back where the replay found it. The board was teleported along a recorded
	// trajectory and the wave clock was pinned to recorded frames, so "resume" means restoring
	// both - not simply unfreezing the board wherever the recording happened to end.
	if (PreReplay.bValid)
	{
		if (SurfboardActor)
		{
			SurfboardActor->SetActorTransform(PreReplay.BoardTransform, /*bSweep*/false,
				/*OutHit*/nullptr, ETeleportType::TeleportPhysics);

			if (UStaticMeshComponent* Mesh = SurfboardActor->GetStaticMeshComponent())
			{
				Mesh->SetSimulatePhysics(PreReplay.bWasSimulating);
				if (PreReplay.bWasSimulating)
				{
					Mesh->SetPhysicsLinearVelocity(PreReplay.LinearVelocity);
					Mesh->SetPhysicsAngularVelocityInRadians(PreReplay.AngularVelocityRad);
				}
			}
		}

		if (PreReplay.WaveFrame >= 0)
		{
			SetWaveFrame(PreReplay.WaveFrame);
		}

		SetForcePipelineTicking(true);

		bUseBehindCamera       = PreReplay.bBehindCamera;
		bPlayerControlsEnabled = PreReplay.bControlsEnabled;

		if (PreReplay.bRadarWasUp && WantsWaveRadar())
		{
			WaveRadar::Install(GetWorld(), RadarSizePx);
		}
	}

	PlayReplayTransitionFade();
	UE_LOG(LogSurf, Display, TEXT("SurfboardPawn::LeaveReplay - back to the live wave"));

	// Deliberately does NOT reopen the list. D5's "land back on the list" is handled where the
	// replay ENDS (TickReplay, on the hold), not here: this function is also what runs when the
	// player closes the list to get back to the wave, and reopening it there would trap them in it.
}

// ======================================================================================
//  Ride Replay (kinematic playback) — see specs/on-device-ride-replay.md
// ======================================================================================

AActor* ASurfboardPawn::ResolveWaterController()
{
	if (CachedWaterController)
	{
		return CachedWaterController;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	// Same resolution path as ASharedCalculations::FindWaterController — the first
	// GridLODActor that has a WaterController wired up.
	TArray<AActor*> Grids;
	UGameplayStatics::GetAllActorsOfClass(World, AGridLODActor::StaticClass(), Grids);
	for (AActor* A : Grids)
	{
		if (AGridLODActor* Grid = Cast<AGridLODActor>(A))
		{
			if (Grid->WaterController)
			{
				CachedWaterController = Grid->WaterController;
				return CachedWaterController;
			}
		}
	}
	return nullptr;
}

int32 ASurfboardPawn::ReadWaveFrame()
{
	AActor* WaterCtrl = ResolveWaterController();
	if (!WaterCtrl)
	{
		return -1;
	}
	FProperty* Prop = WaterCtrl->GetClass()->FindPropertyByName(TEXT("CurrentFrame"));
	if (FIntProperty* I = CastField<FIntProperty>(Prop))
	{
		return I->GetPropertyValue_InContainer(WaterCtrl);
	}
	if (FFloatProperty* F = CastField<FFloatProperty>(Prop))
	{
		return FMath::RoundToInt(F->GetPropertyValue_InContainer(WaterCtrl));
	}
	return -1;
}

void ASurfboardPawn::SetWaveFrame(int32 Frame)
{
	AActor* WaterCtrl = ResolveWaterController();
	if (!WaterCtrl || Frame < 0)
	{
		return;
	}
	// The WaterController BP auto-increments CurrentFrame every tick, so a bare write is
	// overwritten. If the BP exposes a bManualFrameControl bool (added for replay — see
	// spec), set it so the BP's own stepping is suppressed and our per-tick write wins.
	// Written every tick regardless, so if the bool is absent the wave still starts on the
	// recorded frame and merely free-runs from there (Phase 2 behaviour).
	if (FProperty* ManualProp = WaterCtrl->GetClass()->FindPropertyByName(TEXT("bManualFrameControl")))
	{
		if (FBoolProperty* B = CastField<FBoolProperty>(ManualProp))
		{
			B->SetPropertyValue_InContainer(WaterCtrl, true);
		}
	}
	if (FProperty* FrameProp = WaterCtrl->GetClass()->FindPropertyByName(TEXT("CurrentFrame")))
	{
		if (FIntProperty* I = CastField<FIntProperty>(FrameProp))
		{
			I->SetPropertyValue_InContainer(WaterCtrl, Frame);
		}
		else if (FFloatProperty* F = CastField<FFloatProperty>(FrameProp))
		{
			F->SetPropertyValue_InContainer(WaterCtrl, static_cast<float>(Frame));
		}
	}
}

bool ASurfboardPawn::LoadLatestTrace()
{
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("InputTraces"));

	// Newest *.csv by file timestamp (robust to mixed session prefixes).
	IFileManager& Fm = IFileManager::Get();
	TArray<FString> Files;
	Fm.FindFiles(Files, *(Dir / TEXT("*.csv")), /*Files*/true, /*Directories*/false);
	if (Files.Num() == 0)
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn::ReplayLastRide - no traces in %s"), *Dir);
		return false;
	}
	FString NewestPath;
	FDateTime Newest = FDateTime::MinValue();
	for (const FString& F : Files)
	{
		const FString Full = Dir / F;
		const FDateTime Ts = Fm.GetTimeStamp(*Full);
		if (Ts >= Newest)
		{
			Newest = Ts;
			NewestPath = Full;
		}
	}

	return LoadTraceFile(NewestPath);
}

bool ASurfboardPawn::LoadTraceFile(const FString& NewestPath)
{
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *NewestPath))
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn::ReplayLastRide - failed to read %s"), *NewestPath);
		return false;
	}

	ReplayRows.Reset();
	bReplayWaveSynced = true; // cleared below if any data row lacks the wave_frame column
	for (const FString& Line : Lines)
	{
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#")) || Line.StartsWith(TEXT("t,")))
		{
			continue; // metadata, header, or blank
		}
		TArray<FString> Cols;
		Line.ParseIntoArray(Cols, TEXT(","), /*CullEmpty*/false);
		if (Cols.Num() < 15)
		{
			continue; // malformed row
		}
		FReplayRow Row;
		Row.t = FCString::Atof(*Cols[0]);
		Row.pos = FVector(FCString::Atof(*Cols[6]), FCString::Atof(*Cols[7]), FCString::Atof(*Cols[8]));
		// Linear velocity has been recorded since the format's first version (cols 9-11) but was
		// never read back — kinematic replay had no use for it. A rails intro does: it is what the
		// board resumes physics with. See specs/deterministic-ride-handoff.md.
		Row.vel = FVector(FCString::Atof(*Cols[9]), FCString::Atof(*Cols[10]), FCString::Atof(*Cols[11]));
		// Recorded as (Roll=Col12, Pitch=Col13, Yaw=Col14); FRotator ctor is (Pitch, Yaw, Roll).
		Row.rot = FRotator(FCString::Atof(*Cols[13]), FCString::Atof(*Cols[14]), FCString::Atof(*Cols[12]));
		if (Cols.Num() >= 16)
		{
			Row.waveFrame = FCString::Atoi(*Cols[15]);
		}
		else
		{
			bReplayWaveSynced = false; // old trace, no wave clock — wave will free-run
		}
		// Spray columns 16-27 (older traces lack them: replay then runs sprayless — hasSpray
		// stays false and zeros are pushed, which also suppresses the frozen-accumulator spray).
		if (Cols.Num() >= 28)
		{
			for (int32 s = 0; s < 3; ++s)
			{
				const int32 base = 16 + s * 4;
				Row.sprayVel[s] = FVector(FCString::Atof(*Cols[base]),
				                          FCString::Atof(*Cols[base + 1]),
				                          FCString::Atof(*Cols[base + 2]));
				Row.sprayRate[s] = FCString::Atof(*Cols[base + 3]);
			}
			Row.hasSpray = true;
		}
		// Weight columns 28-29 (rider lean/twist; pre-rider traces replay a neutral rider).
		if (Cols.Num() >= 30)
		{
			Row.weightFront = FCString::Atof(*Cols[28]);
			Row.weightRight = FCString::Atof(*Cols[29]);
			Row.hasWeight = true;
		}
		// Angular velocity columns 30-32. Older traces lack them; a rails intro recorded from such
		// a trace would resume with no spin, which skip-paddle-intro documents as a visible
		// first-frame hitch — hence hasAngVel, checked before a trace is accepted as a rails source.
		if (Cols.Num() >= 33)
		{
			Row.angVelDeg = FVector(FCString::Atof(*Cols[30]),
			                        FCString::Atof(*Cols[31]),
			                        FCString::Atof(*Cols[32]));
			Row.hasAngVel = true;
		}
		// Rider animation state (column 33).
		if (Cols.Num() >= 34)
		{
			Row.riderAnim = (ESurferAnimState)FMath::Clamp(FCString::Atoi(*Cols[33]),
				(int32)ESurferAnimState::Paddle, (int32)ESurferAnimState::Surf);
			Row.hasRiderAnim = true;
		}
		// Pump stroke phase (column 34), -1 when not pumping.
		if (Cols.Num() >= 35)
		{
			Row.pumpPhase = FCString::Atof(*Cols[34]);
		}
		ReplayRows.Add(Row);
	}

	if (ReplayRows.Num() < 2)
	{
		UE_LOG(LogSurf, Warning,
			TEXT("SurfboardPawn::ReplayLastRide - %s has too few rows (%d) to replay"),
			*NewestPath, ReplayRows.Num());
		return false;
	}

	ReplayTraceFile = NewestPath;
	UE_LOG(LogSurf, Display,
		TEXT("SurfboardPawn::ReplayLastRide - loaded %d rows from %s (waveSync=%s)"),
		ReplayRows.Num(), *NewestPath, bReplayWaveSynced ? TEXT("yes") : TEXT("no"));
	return true;
}

void ASurfboardPawn::ReplayLastRide()
{
	// D10: this is THE Replay control, and it now asks which ride instead of assuming the last one.
	//
	// The redirect lives here, rather than in the Blueprint button, deliberately: repointing
	// `ReplayButton_1` at OpenRideList() would mean editing WBP_SurfboardControls, and a Blueprint
	// whose call goes stale fails by killing the whole graph silently - the board free-falls and
	// nothing says why. Leaving the existing call target alone and changing what it MEANS needs no
	// editor work at all, and the IA_Replay key and the `ReplaySurf` console alias come along for
	// free.
	if (HasWatchableRides())
	{
		OpenRideList();
		return;
	}

	// No records yet - a device with traces from before this feature existed (D3 keeps the scan as
	// exactly this fallback). Play the newest file the old way, and say so, because after the first
	// ride of this session the records exist and this branch means they were lost.
	UE_LOG(LogSurf, Warning,
		TEXT("SurfboardPawn::ReplayLastRide - no ride records; falling back to newest-file-on-disk"));

	// Re-entrant: if a replay is already running or holding on the last frame, just
	// restart it from the top (same latest ride — no new file has been created and the
	// board stays kinematic). This is the "watch it again" path.
	if (bReplayActive)
	{
		ReplayTime = 0.0f;
		ReplayRowHint = 0;
		bReplayHolding = false;
		ApplyReplayAtTime(0.0f);
		PlayReplayTransitionFade(); // same fade-in seam as first entry ("watch it again")
		UE_LOG(LogSurf, Display, TEXT("SurfboardPawn::ReplayLastRide - restarting current replay"));
		return;
	}

	// First entry: finalize the in-progress recording so "latest ride" == "what I just
	// did", then load the newest trace off disk.
	if (bInputTraceActive)
	{
		StopInputTrace(TEXT("replay"));
	}
	if (!LoadLatestTrace())
	{
		return; // nothing to replay; leave the live level untouched (rider stays fallen)
	}

	// A fall leaves the rider off the deck. Replay drives the rider from recorded
	// weight amounts (standing through to the fall moment — see spec, out-of-scope), so put
	// the mesh back on the board, animation-driven, and clear the fall state.
	RestoreRiderAfterFall();

	EnterReplayMode();
}

void ASurfboardPawn::SetForcePipelineTicking(bool bEnabled)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// One board per playable level, so disabling every actor of these classes is safe.
	auto DisableClass = [World, bEnabled](UClass* Cls)
	{
		TArray<AActor*> Found;
		UGameplayStatics::GetAllActorsOfClass(World, Cls, Found);
		for (AActor* A : Found)
		{
			A->SetActorTickEnabled(bEnabled);
		}
	};
	DisableClass(AFluidDynamics::StaticClass());
	DisableClass(ABuoyancy::StaticClass());
	DisableClass(ASharedCalculations::StaticClass());
	DisableClass(AWeightDistribution::StaticClass());
	// The intro autopilot too. Replays start from the hub now, which is pre-intro: left ticking, the
	// autopilot would watch the replayed board's attitude cross its step triggers, advance, and push
	// Paddle / Cobra / PopUp onto a rider the replay is already posing from the recorded rows
	// (specs/two-screen-navigation.md FR4). Never mattered before because every replay used to
	// start post-handoff, with the autopilot already finished.
	DisableClass(AStateTriggerAutoPilot::StaticClass());
	// ASprayController deliberately NOT disabled: during replay it keeps computing emission
	// positions/waterline/settle grid live (deterministic under the re-synced wave clock) while
	// its velocity/rate come from the recorded spray columns via SetReplaySpray.
}

ASprayController* ASurfboardPawn::ResolveSprayController()
{
	if (!CachedSprayController)
	{
		CachedSprayController = Cast<ASprayController>(
			UGameplayStatics::GetActorOfClass(GetWorld(), ASprayController::StaticClass()));
	}
	return CachedSprayController;
}

void ASurfboardPawn::EnterReplayMode()
{
	bReplayActive = true;
	bReplayHolding = false;
	ReplayTime = 0.0f;
	ReplayRowHint = 0;

	// Take control away from the player/autopilot and make sure we don't record the
	// replay itself.
	bPlayerControlsEnabled = false;
	if (bInputTraceActive)
	{
		StopInputTrace(TEXT("replay"));
	}

	// Freeze the board's physics so the recorded transform is authoritative.
	if (SurfboardActor)
	{
		if (UStaticMeshComponent* Mesh = SurfboardActor->GetStaticMeshComponent())
		{
			Mesh->SetSimulatePhysics(false);
		}
	}

	// Silence the whole force pipeline. With the board kinematic, every FluidDynamics /
	// Buoyancy / SharedCalculations / WeightDistribution tick would still call
	// Add*Impulse/Torque (or read physics velocity) on a non-simulating body — each is a
	// no-op that logs a per-call warning/error. None of them do anything useful during a
	// kinematic replay (the transform is driven directly and the camera reads the mesh, not
	// these actors), so stop them ticking. Terminal for the replay: a Restart reloads the
	// level, which restores everything.
	SetForcePipelineTicking(false);

	// Beside camera for v1 (a dedicated replay camera is a follow-on). Set directly rather
	// than via SetCameraBehind's log, and bypass the bPlayerControlsEnabled gate.
	bUseBehindCamera = false;

	// Seat the board at the recorded start + pin the wave clock before playback advances.
	ApplyReplayAtTime(0.0f);

	// Make the mode switch unmistakable: hide the live wave radar, install the cinematic replay
	// overlay (letterbox + REPLAY badge + progress bar), and fade in from black over the seam.
	// Replay is terminal (only Restart, via level reload, exits it), so the radar is not restored.
	WaveRadar::Uninstall(GetWorld());
	ReplayOverlay::Install(GetWorld());

	// The score counter too. Tick returns before UpdateRideScore() for the whole replay, so nothing
	// else ever tells the counter the ride went away - it froze on screen with the last number, over
	// a recording it has nothing to do with. Hide, not Uninstall: the first live tick after
	// LeaveReplay runs UpdateRideScore again and brings it back through its own gates.
	RideScore::Hide(GetWorld());
	PlayReplayTransitionFade();

	UE_LOG(LogSurf, Display,
		TEXT("SurfboardPawn::EnterReplayMode - replaying %s (%d rows)"),
		*ReplayTraceFile, ReplayRows.Num());
}

void ASurfboardPawn::PlayReplayTransitionFade()
{
	// Reuse the same camera-manager fade the Restart flow uses. Fade FROM black TO clear so the
	// board (already seated at row 0) is revealed — a quick, deliberate LIVE->REPLAY seam.
	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		if (PC->PlayerCameraManager)
		{
			PC->PlayerCameraManager->StartCameraFade(
				1.0f, 0.0f, 0.35f, FLinearColor::Black, /*bShouldFadeAudio*/ false, /*bHoldWhenFinished*/ false);
		}
	}
}

void ASurfboardPawn::UpdateReplayOverlay()
{
	FReplayOverlaySnapshot Snap;
	const float Total = (ReplayRows.Num() > 0) ? ReplayRows.Last().t : 0.0f;
	Snap.TotalSeconds = Total;
	Snap.CurrentSeconds = FMath::Min(ReplayTime, Total);
	Snap.Progress = (Total > 0.0f) ? FMath::Clamp(ReplayTime / Total, 0.0f, 1.0f) : 0.0f;
	Snap.bHolding = bReplayHolding;
	ReplayOverlay::UpdateData(GetWorld(), Snap);
}

bool ASurfboardPawn::AreRailsDrivingTheBoard()
{
	return SurfRails::IsKinematicallyDriven();
}

USurferAnimInstance* ASurfboardPawn::ResolveRiderAnim() const
{
	if (!SurfboardActor)
	{
		return nullptr;
	}
	if (USkeletalMeshComponent* RiderMesh = SurfboardActor->FindComponentByClass<USkeletalMeshComponent>())
	{
		return Cast<USurferAnimInstance>(RiderMesh->GetAnimInstance());
	}
	return nullptr;
}

FString ASurfboardPawn::ResolveRailsTracePath(const FString& InPath) const
{
	// Trim first. RailsIntroTrace is hand-typed into a level property, and a single leading space
	// resolved to "Saved/ InputTraces/intro-reference.csv", missed both roots, and dropped the ride
	// to the LIVE intro. On a packaged build that is invisible: nothing looks broken, the handoff
	// just stops being deterministic. Cost a run to find, so it cannot be allowed to happen twice.
	const FString Trimmed = InPath.TrimStartAndEnd();

	if (!FPaths::IsRelative(Trimmed))
	{
		return Trimmed;
	}

	IFileManager& Fm = IFileManager::Get();

	// Content/ first: that is the only location that survives packaging (staged as UFS via
	// DirectoriesToAlwaysStageAsUFS in DefaultGame.ini — a CSV is not a UAsset, so the cook
	// settings never reach it). On device, Saved/ is the app's own writable directory and will not
	// contain a trace recorded on a PC.
	const FString ContentPath = FPaths::Combine(FPaths::ProjectContentDir(), Trimmed);
	if (Fm.FileExists(*ContentPath))
	{
		return ContentPath;
	}

	// Saved/ second: where -RecordIntro writes, so a trace can be recorded and played back on PC
	// with no copying. Returned even when missing, so the failure log names the path the author
	// most likely meant.
	return FPaths::Combine(FPaths::ProjectSavedDir(), Trimmed);
}

bool ASurfboardPawn::StartRails(const FString& TracePath)
{
	// Never during a snapshot/replay test run: the rails would drive the board over whatever the
	// autopilot is doing and corrupt every recorded trajectory. Same gate as the other
	// controls-gated features (see UpdateFallDetection). -RailsTrace bypasses this deliberately —
	// that is how the headless A/B drives rails against a known trace.
	if (!bRailsCommandLineOverride
		&& (bExternalWeightOverride || SurfDebug::CVarAutopilots.GetValueOnGameThread().Len() > 0))
	{
		UE_LOG(LogSurf, Display, TEXT("Rails: suppressed (test/replay run)."));
		return false;
	}

	if (!LoadTraceFile(TracePath))
	{
		// Error, not Warning: on a packaged device build this is invisible to the player — the ride
		// just quietly reverts to the non-deterministic live intro, which looks like "the rails
		// don't work on phone" rather than "the file was not packaged". Name both candidate roots
		// so the cause is obvious from a single logcat line.
		UE_LOG(LogSurf, Error,
			TEXT("Rails: FAILED to load intro trace '%s' — falling back to the LIVE intro (non-deterministic handoff). ")
			TEXT("Checked Content='%s' and Saved='%s'. For a packaged build the trace must live under Content/ ")
			TEXT("AND its directory must be listed in DirectoriesToAlwaysStageAsUFS (Config/DefaultGame.ini)."),
			*TracePath,
			*FPaths::Combine(FPaths::ProjectContentDir(), TEXT("<RailsIntroTrace>")),
			*FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("<RailsIntroTrace>")));
		return false;
	}

	// A trace without recorded angular velocity can seat the board but cannot hand physics a spin,
	// and resuming with zero spin while the board is pitching through a pop-up is the visible
	// first-frame hitch skip-paddle-intro documents. Refuse rather than ship that.
	if (!ReplayRows.Last().hasAngVel)
	{
		UE_LOG(LogSurf, Error,
			TEXT("Rails: '%s' predates the angular-velocity columns (needs 33; re-record with -RecordIntro) — ")
			TEXT("falling back to the LIVE intro (non-deterministic handoff)."),
			*TracePath);
		return false;
	}

	UStaticMeshComponent* Mesh = SurfboardActor ? SurfboardActor->GetStaticMeshComponent() : nullptr;
	if (!Mesh)
	{
		UE_LOG(LogSurf, Warning, TEXT("Rails: no surfboard mesh — falling back to the live intro."));
		return false;
	}

	bRailsActive = true;
	RailsTime = 0.0f;
	ReplayRowHint = 0;

	// Which row hands over. Default is the last one (the shipped intro). -RailsUntil picks the last
	// row AT OR BEFORE the requested time, deliberately not an interpolated pose: seating and stamping
	// the same recorded row means the state physics resumes from is exactly a state the phone
	// recorded, with no interpolation error in the one place the whole fixture rests on.
	RailsReleaseRow = ReplayRows.Num() - 1;
	if (RailsUntilSeconds >= 0.0f)
	{
		int32 Row = 0;
		while (Row + 1 < ReplayRows.Num() && ReplayRows[Row + 1].t <= RailsUntilSeconds)
		{
			++Row;
		}
		RailsReleaseRow = Row;
		UE_LOG(LogSurf, Warning,
			TEXT("Rails: -RailsUntil=%.3f -> releasing on row %d of %d at trace t=%.3f (A/B fixture; ")
			TEXT("everything before this is the recording, everything after is physics)."),
			RailsUntilSeconds, RailsReleaseRow, ReplayRows.Num(), ReplayRows[RailsReleaseRow].t);
	}

	// Board goes kinematic. Unlike StartReplay this does NOT stop the force pipeline ticking —
	// it keeps computing so planing, relative water velocity and the wave-mass smoothing filters
	// are warm when physics resumes. Only force APPLICATION is suppressed.
	Mesh->SetSimulatePhysics(false);
	SurfRails::SetForcesSuppressed(true);

	// Player cannot steer during the intro; the autopilot must not fight the pose track either.
	bPlayerControlsEnabled = false;

	// Seat the board at row 0 and pin the wave clock before playback advances.
	ApplyRailsAtTime(0.0f);

	// Re-preload the wave meshes around the frame we just jumped to.
	//
	// AGridLODActor::BeginPlay preloads 20 frames starting from whatever
	// GetCurrentFrameFromController() reads at the time, and actor BeginPlay order is not
	// guaranteed — so it may well have preloaded around the PRE-jump frame. The cells then have no
	// mesh for the recorded frame and stream it in over the following frames, which reads exactly
	// as reported: the wave runs at one frame, then visibly snaps to the trace's frame a moment
	// later. Preloading here (synchronously, at level start, where a brief hitch is invisible)
	// closes that window. This is the GridLOD gotcha skip-paddle-intro predicted.
	if (bReplayWaveSynced && ReplayRows[0].waveFrame >= 0)
	{
		const int32 RailsStartFrame = ReplayRows[0].waveFrame;
		int32 NumGrids = 0;
		for (TActorIterator<AGridLODActor> It(GetWorld()); It; ++It)
		{
			// Per-tile, NOT one shared frame: each GridLODActor renders a different phase of the
			// wave via its own FrameOffset, so it has to preload around the frame IT needs.
			It->PreloadAroundCurrentFrame(20);
			++NumGrids;
		}
		UE_LOG(LogSurf, Display,
			TEXT("Rails: preloaded %d GridLODActor(s) at their own tile frames (wave clock now %d) - avoids the start-frame pop"),
			NumGrids, RailsStartFrame);
	}

	UE_LOG(LogSurf, Display,
		TEXT("Rails: STARTED — %d rows, %.2fs, from %s"),
		ReplayRows.Num(), ReplayRows.Last().t, *TracePath);
	return true;
}

void ASurfboardPawn::ApplyRailsAtTime(float t)
{
	if (ReplayRows.Num() == 0 || !SurfboardActor)
	{
		return;
	}

	// Same monotonic row-pair cursor the replay uses.
	int32 i = FMath::Clamp(ReplayRowHint, 0, ReplayRows.Num() - 1);
	while (i < ReplayRows.Num() - 1 && ReplayRows[i + 1].t <= t)
	{
		++i;
	}
	ReplayRowHint = i;

	const FReplayRow& A = ReplayRows[i];
	FVector Pos = A.pos;
	FRotator Rot = A.rot;
	int32 WaveFrame = A.waveFrame;

	if (i < ReplayRows.Num() - 1)
	{
		const FReplayRow& B = ReplayRows[i + 1];
		const float Span = B.t - A.t;
		const float Alpha = (Span > KINDA_SMALL_NUMBER) ? FMath::Clamp((t - A.t) / Span, 0.0f, 1.0f) : 0.0f;
		Pos = FMath::Lerp(A.pos, B.pos, Alpha);
		Rot = FQuat::Slerp(A.rot.Quaternion(), B.rot.Quaternion(), Alpha).Rotator();
		WaveFrame = (Alpha < 0.5f) ? A.waveFrame : B.waveFrame;
	}

	// ETeleportType::None — NOT TeleportPhysics. This is what routes the move through
	// FBodyInstance::SetBodyTransform's SetKinematicTarget branch, which is what makes Chaos derive
	// V and W from the motion. TeleportPhysics warps via SetGlobalPose and derives nothing, leaving
	// the board reading as stationary to every velocity-based consumer (AmountPlaning above all).
	// Measured: direction correct 262/262 samples. See specs/deterministic-ride-handoff.md.
	SurfboardActor->SetActorLocationAndRotation(
		Pos, Rot, /*bSweep*/false, /*OutHit*/nullptr, ETeleportType::None);

	if (bReplayWaveSynced)
	{
		SetWaveFrame(WaveFrame);
	}

	// Drive the scripted rider animation from the recording. In a playable level the autopilots are
	// all enabled=false, so ApplyStep never runs and nothing else would ever advance the rider past
	// step 0 (Paddle).
	//
	// The AnimBP is a CHAIN — Paddle -> Cobra -> PopUp -> (auto) Surf — with no transition between
	// non-adjacent states; SurferAnimInstance.h is explicit that there is "deliberately no path into
	// Surf other than the chain". Two rules follow, and breaking either leaves the rider stuck in
	// Paddle for the entire intro:
	//
	//  (2026-08-31: rule 1 is now ALSO enforced centrally in USurferAnimInstance::SetAnimState, which
	//  is where it belongs — the autopilot pushed straight into the variable and hit exactly this bug.
	//  Kept here too: it is proven, and clamping the target away from Surf is still this driver's job.)
	//
	//  1. Never skip a link. Playback is time-indexed, so a frame can step clean over a short-lived
	//     state — a recorded Cobra can last ~10 ms when the autopilot fires its cobra and pop-up
	//     steps on the same tick. Pushing PopUp while the machine is still in Paddle matches no
	//     transition rule and the machine never moves again. Whether a frame lands inside that
	//     window is frame-timing dependent, which is exactly why this presented as intermittent.
	//     So advance at most ONE link per tick and let the machine walk the chain.
	//  2. Never push Surf. The machine enters it by itself when the PopUp clip ends (a time-based
	//     transition). Pushing it would desynchronise AnimState from the state the machine is
	//     actually in — which the hip-stab calibration gate keys on.
	if (A.hasRiderAnim)
	{
		if (USurferAnimInstance* RiderAnim = ResolveRiderAnim())
		{
			const uint8 Target  = FMath::Min((uint8)A.riderAnim, (uint8)ESurferAnimState::PopUp);
			const uint8 Current = (uint8)RiderAnim->AnimState;
			if (Current < Target)
			{
				const ESurferAnimState Next = (ESurferAnimState)(Current + 1);
				RiderAnim->SetAnimState(Next);
				UE_LOG(LogSurf, Display,
					TEXT("Rails: rider anim %d -> %d at t=%.2f (recorded target %d)"),
					Current, (int32)Next, t, (int32)A.riderAnim);
			}
		}
	}
}

void ASurfboardPawn::TickRails(float DeltaTime)
{
	if (!bRailsActive || ReplayRows.Num() < 2)
	{
		return;
	}

	RailsTime += DeltaTime;

	const int32 ReleaseRow = ReplayRows.IsValidIndex(RailsReleaseRow) ? RailsReleaseRow : ReplayRows.Num() - 1;
	const float ReleaseT = ReplayRows[ReleaseRow].t;
	if (RailsTime >= ReleaseT)
	{
		RailsTime = ReleaseT;
		ApplyRailsAtTime(RailsTime); // seat exactly on the release row before handing over
		FinishRails(ReleaseRow);
		return;
	}

	ApplyRailsAtTime(RailsTime);
}

void ASurfboardPawn::FinishRails(int32 ReleaseRow)
{
	bRailsActive = false;

	UStaticMeshComponent* Mesh = SurfboardActor ? SurfboardActor->GetStaticMeshComponent() : nullptr;
	if (!Mesh)
	{
		SurfRails::SetForcesSuppressed(false);
		return;
	}

	// The row the rails seated the board on this tick. Normally the trace's last row; with
	// -RailsUntil, the row that time picked. Stamping ReplayRows.Last() here regardless would fling
	// the board to the END of the ride the instant a partial rails run handed over.
	const FReplayRow& Final = ReplayRows[ReplayRows.IsValidIndex(ReleaseRow) ? ReleaseRow : ReplayRows.Num() - 1];
	RailsReleaseTraceTime = Final.t;

	// Where the board ACTUALLY is, before anything is stamped on it. Logged as a delta against the
	// release row: on a healthy rails run this is ~0, which is the proof that playback tracked
	// the pose rather than the log merely echoing the file back. It is also the seam measurement —
	// a growing delta means the rails and the physics disagree about where the board should be.
	const FVector ActualPos = SurfboardActor->GetActorLocation();
	const FRotator ActualRot = SurfboardActor->GetActorRotation();
	const FVector PosDelta = ActualPos - Final.pos;
	const FRotator RotDelta = (ActualRot.Quaternion() * Final.rot.Quaternion().Inverse()).Rotator();

	UE_LOG(LogSurf, Warning,
		TEXT("SurfboardPawn: HANDOFF DELTA (actual - recorded): pos=(%.2f,%.2f,%.2f) |pos|=%.2f cm  rot=(P=%.2f,Y=%.2f,R=%.2f) deg"),
		PosDelta.X, PosDelta.Y, PosDelta.Z, PosDelta.Size(),
		RotDelta.Pitch, RotDelta.Yaw, RotDelta.Roll);

	// Resume simulation, then stamp the recorded velocities. Order matters: the setters early-out
	// on a non-simulating body, so setting before re-enabling would silently do nothing.
	Mesh->SetSimulatePhysics(true);
	Mesh->SetPhysicsLinearVelocity(Final.vel);
	Mesh->SetPhysicsAngularVelocityInDegrees(Final.angVelDeg);

	// Velocities come from the RECORDING, never from reading the solver here. Chaos's derived
	// magnitude carries a per-frame scale (measured 0.53x-1.83x), so sampling it on this one frame
	// could hand the player a board at half or double the intended speed.

	// Release the wave-clock pin so the wave free-runs from the recorded frame (SetWaveFrame sets
	// bManualFrameControl every call). Without this the wave stays frozen for the whole ride.
	ReleaseWaveFrameControl();

	SurfRails::SetForcesSuppressed(false);

	// Hand control over on THIS tick, not ControlDelayAfterAutoPilot (1 s) later. Only for the
	// headless A/B fixture: -RailsTrace was passed on the command line AND a replay is driving the
	// weight. In the playable level neither holds, so the shipped intro keeps its grace window.
	// Without this the board runs a full second of physics with no input, through exactly the window
	// the fixture exists to study. See specs/replay-rails-until.md.
	if (bRailsCommandLineOverride && bExternalWeightOverride && !bPlayerControlsEnabled)
	{
		bPlayerControlsEnabled = true;
		OnPlayerControlsEnabled();
		UE_LOG(LogSurf, Warning,
			TEXT("Rails: controls ENABLED on the release tick (A/B fixture) at trace t=%.3f."), Final.t);
	}

	LogHandoffState(Final);
}

void ASurfboardPawn::ReleaseWaveFrameControl()
{
	AActor* WaterCtrl = ResolveWaterController();
	if (!WaterCtrl)
	{
		return;
	}
	if (FProperty* ManualProp = WaterCtrl->GetClass()->FindPropertyByName(TEXT("bManualFrameControl")))
	{
		if (FBoolProperty* B = CastField<FBoolProperty>(ManualProp))
		{
			B->SetPropertyValue_InContainer(WaterCtrl, false);
		}
	}
}

void ASurfboardPawn::LogHandoffState(const FReplayRow& Final)
{
	// Wave-relative diagnostics: not control input, but the audit for "is the recorded intro still
	// a good one?" after a tuning or wave-data change. Averaged across the board's SharedCalculations
	// actors, the same way the trajectory recorder averages slopeSin.
	float HeadingVsDownLineDeg = 0.0f, WaveRelRollSin = 0.0f, SignedDistToCrest = 0.0f;
	float SlopeSin = 0.0f, WaterColumnAbove = 0.0f, SurgeSpeed = 0.0f;
	// AC5: planing must already be at its riding value here, not climbing from zero. It is
	// velocity-based, so it is the sharpest indicator that the board carried a real velocity
	// through the rails rather than reading as stationary.
	float Planing = 0.0f;
	int32 nSC = 0;

	for (TActorIterator<ASharedCalculations> It(GetWorld()); It; ++It)
	{
		ASharedCalculations* SC = *It;
		if (!SC || SC->Surfboard != SurfboardActor)
		{
			continue;
		}
		// Down-the-line axis from the level's wave orientation, never hardcoded +Y:
		// up x back = +Z x +X = +Y for this project's default wave.
		const FVector DownLine = FVector::CrossProduct(SC->up, SC->waveBackDirection).GetSafeNormal();
		const FVector Fwd = SC->forwards.GetSafeNormal();
		HeadingVsDownLineDeg += FMath::RadiansToDegrees(
			FMath::Atan2(FVector::CrossProduct(DownLine, Fwd) | SC->up, DownLine | Fwd));
		Planing           += SC->AmountPlaning;
		WaveRelRollSin    += (float)SC->waveRelativeRollSin;
		SignedDistToCrest += SC->signedDistanceToCrest;
		SlopeSin          += SC->boardWideSlopeSin;
		WaterColumnAbove  += SC->boardWideWaterColumnAbove;
		SurgeSpeed        += Final.vel | Fwd;
		++nSC;
	}

	if (nSC > 0)
	{
		const float Inv = 1.0f / (float)nSC;
		HeadingVsDownLineDeg *= Inv; WaveRelRollSin *= Inv; SignedDistToCrest *= Inv;
		SlopeSin *= Inv; WaterColumnAbove *= Inv; SurgeSpeed *= Inv; Planing *= Inv;
	}

	UE_LOG(LogSurf, Warning,
		TEXT("SurfboardPawn: HANDOFF STATE: waveFrame=%d pos=(X=%.1f,Y=%.1f,Z=%.1f) rot=(P=%.2f,Y=%.2f,R=%.2f) ")
		TEXT("vel=(X=%.1f,Y=%.1f,Z=%.1f) angVelDeg=(X=%.1f,Y=%.1f,Z=%.1f) | headingVsDownLineDeg=%.1f ")
		TEXT("waveRelativeRollSin=%.3f signedDistanceToCrest=%.1f slopeSin=%.3f waterColumnAbove=%.1f surgeSpeed=%.1f planing=%.3f"),
		Final.waveFrame,
		Final.pos.X, Final.pos.Y, Final.pos.Z,
		Final.rot.Pitch, Final.rot.Yaw, Final.rot.Roll,
		Final.vel.X, Final.vel.Y, Final.vel.Z,
		Final.angVelDeg.X, Final.angVelDeg.Y, Final.angVelDeg.Z,
		HeadingVsDownLineDeg, WaveRelRollSin, SignedDistToCrest,
		SlopeSin, WaterColumnAbove, SurgeSpeed, Planing);
}

void ASurfboardPawn::TickKinematicProbe(float DeltaTime)
{
	UStaticMeshComponent* Mesh = SurfboardActor ? SurfboardActor->GetStaticMeshComponent() : nullptr;
	if (!Mesh)
	{
		UE_LOG(LogSurf, Error, TEXT("KinematicProbe: no surfboard mesh — aborting."));
		bKinematicProbeActive = false;
		return;
	}

	if (!bKinematicProbeInitialised)
	{
		bKinematicProbeInitialised = true;

		// Report the body's configuration BEFORE going kinematic. bSimulatePhysics is the setup
		// flag that ultimately backs CanSimulate; if it is false the SetKinematicTarget branch in
		// FBodyInstance::SetBodyTransform can never be taken and the probe will read zeros.
		FBodyInstance* BI = Mesh->GetBodyInstance();
		UE_LOG(LogSurf, Display,
			TEXT("KinematicProbe: pre-state simulating=%d bSimulatePhysics=%d mobility=%d hasBody=%d"),
			Mesh->IsSimulatingPhysics() ? 1 : 0,
			(BI && BI->bSimulatePhysics) ? 1 : 0,
			(int32)Mesh->Mobility.GetValue(),
			BI ? 1 : 0);

		// Kinematic, exactly as the rails would leave it.
		Mesh->SetSimulatePhysics(false);

		// Silence the force pipeline so its impulse calls don't spam warnings at us while the
		// board is non-simulating (same reason StartReplay does it).
		SetForcePipelineTicking(false);

		KinematicProbeOrigin = SurfboardActor->GetActorLocation();
		KinematicProbeOriginRot = SurfboardActor->GetActorRotation();

		UE_LOG(LogSurf, Display,
			TEXT("KinematicProbe: driving %.0f s at v=(0,%.0f,%.0f) cm/s, yawRate=%.0f deg/s via ETeleportType::None"),
			KinematicProbeDuration, KinematicProbeSpeed, KinematicProbeRise, KinematicProbeYawRate);
		return;
	}

	KinematicProbeTime += DeltaTime;

	// Commanded pose: constant velocity along +Y/+Z, constant yaw rate. Absolute-from-origin
	// rather than incremental, so a dropped frame doesn't accumulate error into the commanded path.
	const FVector CommandedVel(0.0f, KinematicProbeSpeed, KinematicProbeRise);
	const FVector NewPos = KinematicProbeOrigin + CommandedVel * KinematicProbeTime;
	FRotator NewRot = KinematicProbeOriginRot;
	NewRot.Yaw += KinematicProbeYawRate * KinematicProbeTime;

	// THE point of the probe: ETeleportType::None is what routes this to SetKinematicTarget, which
	// is what makes Chaos derive V/W. TeleportPhysics would warp via SetGlobalPose and derive
	// nothing — see specs/deterministic-ride-handoff.md.
	SurfboardActor->SetActorLocationAndRotation(
		NewPos, NewRot, /*bSweep*/false, /*OutHit*/nullptr, ETeleportType::None);

	// Read back what the solver thinks the board's velocity is.
	const FVector ReadVel = Mesh->GetPhysicsLinearVelocity();
	const FVector ReadAngVel = Mesh->GetPhysicsAngularVelocityInDegrees();

	// Skip the first couple of ticks: the kinematic target needs one solver step to produce a
	// velocity, and DeltaTime on the opening frames is unrepresentative.
	//
	// What is actually being tested: whether Chaos derives velocity from the kinematic target at
	// all. The first run showed it does — but with a per-frame UNIFORM scale on all components
	// (readV.Y/500 == readV.Z/120 == readAngVel.Z/30 to 3 decimals every sample), because the
	// solver divides the position delta by its own step while the commanded pose advances by the
	// game frame's delta. So direction is the correctness signal, and the magnitude ratio is a
	// frame-timing measurement — track both separately rather than failing the whole thing on a
	// tolerance that only ever measured frame jitter.
	if (KinematicProbeTime > 0.2f)
	{
		++KinematicProbeSamples;

		const FVector CommandedDir = CommandedVel.GetSafeNormal();
		const FVector ReadDir = ReadVel.GetSafeNormal();
		const float DirDot = FVector::DotProduct(CommandedDir, ReadDir);
		if (DirDot > 0.999f) // within ~2.5 degrees
		{
			++KinematicProbeGoodSamples;
		}

		// Uniform-scale check: linear and angular must agree on the same ratio, which is what
		// proves a single dt is behind the discrepancy rather than two unrelated bugs.
		const float LinRatio = ReadVel.Y / KinematicProbeSpeed;
		const float AngRatio = ReadAngVel.Z / KinematicProbeYawRate;
		KinematicProbeRatioSum += LinRatio;
		KinematicProbeRatioMin = FMath::Min(KinematicProbeRatioMin, LinRatio);
		KinematicProbeRatioMax = FMath::Max(KinematicProbeRatioMax, LinRatio);
		if (FMath::Abs(LinRatio - AngRatio) > 0.01f)
		{
			++KinematicProbeNonUniformSamples;
		}
	}

	// One line every ~0.25 s so the log shows the trend rather than a wall of text.
	static float ProbeLogTimer = 0.0f;
	ProbeLogTimer += DeltaTime;
	if (ProbeLogTimer >= 0.25f)
	{
		ProbeLogTimer = 0.0f;
		UE_LOG(LogSurf, Display,
			TEXT("KinematicProbe: t=%.2f commandedV=(0.0,%.1f,%.1f) readV=(%.1f,%.1f,%.1f) | commandedYawRate=%.1f readAngVel=(%.1f,%.1f,%.1f)"),
			KinematicProbeTime, KinematicProbeSpeed, KinematicProbeRise,
			ReadVel.X, ReadVel.Y, ReadVel.Z,
			KinematicProbeYawRate, ReadAngVel.X, ReadAngVel.Y, ReadAngVel.Z);
	}

	if (KinematicProbeTime >= KinematicProbeDuration)
	{
		const bool bDerived = KinematicProbeSamples > 0
		                   && KinematicProbeGoodSamples >= (KinematicProbeSamples * 95) / 100;
		const bool bUniform = KinematicProbeNonUniformSamples * 20 <= KinematicProbeSamples;
		const float MeanRatio = (KinematicProbeSamples > 0)
			? KinematicProbeRatioSum / (float)KinematicProbeSamples : 0.0f;

		UE_LOG(LogSurf, Warning,
			TEXT("KinematicProbe: VERDICT %s — direction correct on %d/%d samples; magnitude ratio mean=%.3f min=%.3f max=%.3f; non-uniform samples=%d"),
			(bDerived && bUniform)
				? TEXT("PASS (Chaos derives V/W from kinematic targets; no engine change needed). Magnitude carries frame-time scale — do NOT take the resume velocity from the solver at handoff")
				: TEXT("FAIL (velocity not derived as expected — see spec open question 4)"),
			KinematicProbeGoodSamples, KinematicProbeSamples,
			MeanRatio, KinematicProbeRatioMin, KinematicProbeRatioMax,
			KinematicProbeNonUniformSamples);

		bKinematicProbeActive = false;

		if (GetWorld() && GetWorld()->WorldType == EWorldType::Game)
		{
			UKismetSystemLibrary::QuitGame(GetWorld(), nullptr, EQuitPreference::Quit, false);
		}
	}
}

void ASurfboardPawn::TickReplay(float DeltaTime)
{
	if (!bReplayActive || ReplayRows.Num() < 2)
	{
		return;
	}

	const float LastT = ReplayRows.Last().t;
	if (!bReplayHolding)
	{
		ReplayTime += DeltaTime;
		if (ReplayTime >= LastT)
		{
			ReplayTime = LastT;
			bReplayHolding = true; // reached the end — hold on the final frame
		}
	}

	// Re-apply every tick (even while holding) so the wave clock stays pinned against the
	// WaterController BP's own stepping.
	ApplyReplayAtTime(ReplayTime);

	// Holding on the last frame = playback paused: stop spray emission too (previously the
	// controller kept spawning from frozen force accumulators forever). In-flight droplets
	// live out their lifetime, which reads like pausing a video.
	if (bReplayHolding)
	{
		if (ASprayController* Spray = ResolveSprayController())
		{
			Spray->SetReplaySpray(FVector::ZeroVector, 0.0f, FVector::ZeroVector, 0.0f,
			                      FVector::ZeroVector, 0.0f);
		}

		// D5: the replay has ended, so put the player back on the screen they launched it from,
		// on the row they just watched. They came from a comparison and the thing they most likely
		// want next is another board's best; dropping them into a live wave ends that comparison at
		// the moment they were most engaged with it.
		//
		// After a beat, not instantly. The end-card is what makes the stop read as intentional
		// rather than as a freeze, and a list that slams up over it would take that away.
		//
		// Not once a reload is on its way. Closing that list is how a hub replay returns to the hub
		// (ReturnToHub, a fade then a reload on a world timer), and this branch keeps running through
		// the fade with the replay still on hold - so without the guard it put the list straight
		// back up, which paused the world, which froze the timer, which left the player on a list
		// whose BACK did nothing. Measured 2026-09-14, first time the path was walked.
		ReplayHoldSeconds += DeltaTime;
		if (ReplayHoldSeconds >= kReplayHoldBeforeListSeconds && RideListEntries.Num() > 0
			&& !RideListPanel::IsOpen(GetWorld()) && !bReloadPending)
		{
			OpenRideList();
		}
	}
	else
	{
		ReplayHoldSeconds = 0.0f;
	}
}

void ASurfboardPawn::ApplyReplayAtTime(float t)
{
	if (ReplayRows.Num() == 0)
	{
		return;
	}

	// Advance the monotonic hint to the row pair straddling t.
	int32 i = FMath::Clamp(ReplayRowHint, 0, ReplayRows.Num() - 1);
	while (i < ReplayRows.Num() - 1 && ReplayRows[i + 1].t <= t)
	{
		++i;
	}
	ReplayRowHint = i;

	const FReplayRow& A = ReplayRows[i];
	FVector Pos = A.pos;
	FRotator Rot = A.rot;
	int32 WaveFrame = A.waveFrame;
	FVector SprayVel[3] = { A.sprayVel[0], A.sprayVel[1], A.sprayVel[2] };
	float SprayRate[3] = { A.sprayRate[0], A.sprayRate[1], A.sprayRate[2] };
	bool bHasSpray = A.hasSpray;

	if (i < ReplayRows.Num() - 1)
	{
		const FReplayRow& B = ReplayRows[i + 1];
		const float Span = B.t - A.t;
		const float Alpha = (Span > KINDA_SMALL_NUMBER) ? FMath::Clamp((t - A.t) / Span, 0.0f, 1.0f) : 0.0f;
		Pos = FMath::Lerp(A.pos, B.pos, Alpha);
		Rot = FQuat::Slerp(A.rot.Quaternion(), B.rot.Quaternion(), Alpha).Rotator();
		WaveFrame = (Alpha < 0.5f) ? A.waveFrame : B.waveFrame; // nearest recorded int frame
		if (bHasSpray && B.hasSpray)
		{
			for (int32 s = 0; s < 3; ++s)
			{
				SprayVel[s] = FMath::Lerp(A.sprayVel[s], B.sprayVel[s], Alpha);
				SprayRate[s] = FMath::Lerp(A.sprayRate[s], B.sprayRate[s], Alpha);
			}
		}
	}

	if (SurfboardActor)
	{
		SurfboardActor->SetActorLocationAndRotation(
			Pos, Rot, /*bSweep*/false, /*OutHit*/nullptr, ETeleportType::TeleportPhysics);
	}

	if (bReplayWaveSynced)
	{
		SetWaveFrame(WaveFrame);
	}

	// Recorded spray, played back verbatim. Always push (zeros for pre-spray traces): enabling
	// the override is ALSO what stops the controller spraying a constant stale fan from the
	// frozen force accumulators — the cause of spray randomly appearing in old replays.
	if (ASprayController* Spray = ResolveSprayController())
	{
		Spray->SetReplaySpray(SprayVel[0], SprayRate[0], SprayVel[1], SprayRate[1],
		                      SprayVel[2], SprayRate[2]);
	}

	// Recorded weight amounts → the surfer rider's lean/twist (the live WeightDistribution is
	// tick-frozen during replay). Always push: neutral 0.5/0.5 for pre-rider traces beats a
	// rider frozen in a stale lean. Board-roll compensation needs nothing here — the
	// AnimInstance reads it geometrically off the replayed board transform.
	if (SurfboardActor)
	{
		if (USkeletalMeshComponent* RiderMesh = SurfboardActor->FindComponentByClass<USkeletalMeshComponent>())
		{
			if (USurferAnimInstance* RiderAnim = Cast<USurferAnimInstance>(RiderMesh->GetAnimInstance()))
			{
				float WFront = 0.5f;
				float WRight = 0.5f;
				if (i < ReplayRows.Num() - 1 && A.hasWeight && ReplayRows[i + 1].hasWeight)
				{
					const FReplayRow& B = ReplayRows[i + 1];
					const float Span = B.t - A.t;
					const float Alpha = (Span > KINDA_SMALL_NUMBER) ? FMath::Clamp((t - A.t) / Span, 0.0f, 1.0f) : 0.0f;
					WFront = FMath::Lerp(A.weightFront, B.weightFront, Alpha);
					WRight = FMath::Lerp(A.weightRight, B.weightRight, Alpha);
				}
				else if (A.hasWeight)
				{
					WFront = A.weightFront;
					WRight = A.weightRight;
				}
				RiderAnim->SetReplayWeights(WFront, WRight);

				// Pump stroke. Sampled, never interpolated: the phase wraps 1 -> 0 at every stroke
				// boundary, and lerping across that wrap would sweep the animation backwards through
				// the whole clip in a single frame.
				RiderAnim->SetReplayPump(A.pumpPhase >= 0.0f, FMath::Max(A.pumpPhase, 0.0f));
			}
		}
	}
}

// ======================================================================================
//  Fall (ragdoll wipeout) and the lost-wave stall — see specs/surfer-fall-ragdoll.md
// ======================================================================================

ASharedCalculations* ASurfboardPawn::ResolveSharedCalcForFall()
{
	if (CachedSharedCalcForFall)
	{
		return CachedSharedCalcForFall;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}
	// Prefer the SC wired to our board (planing is board-speed-based, so either half-board's
	// SC reads the same); fall back to the camera's SC, then any SC in the level.
	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClass(World, ASharedCalculations::StaticClass(), Found);
	for (AActor* A : Found)
	{
		ASharedCalculations* SC = Cast<ASharedCalculations>(A);
		if (SC && SurfboardActor && SC->Surfboard == SurfboardActor)
		{
			CachedSharedCalcForFall = SC;
			return CachedSharedCalcForFall;
		}
	}
	if (SharedCalculationsForCamera)
	{
		CachedSharedCalcForFall = SharedCalculationsForCamera;
	}
	else if (Found.Num() > 0)
	{
		CachedSharedCalcForFall = Cast<ASharedCalculations>(Found[0]);
	}
	return CachedSharedCalcForFall;
}

float ASurfboardPawn::GetBoardWorldRollDeg() const
{
	if (!SurfboardActor)
	{
		return 0.0f;
	}
	// Same geometric convention as USurferAnimInstance's board-attitude compensation:
	// board.left = local -X (mesh is rotated 90°), roll = asin(left.Z).
	const FTransform BoardTF = SurfboardActor->GetActorTransform();
	const FVector BoardLeft = BoardTF.TransformVectorNoScale(FVector(-1.0, 0.0, 0.0)).GetSafeNormal();
	return FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp((float)BoardLeft.Z, -1.0f, 1.0f)));
}

// ===== Gradual control handoff ===================================================================
// See specs/gradual-control-handoff.md. The controller itself lives in SurfAssist.h/.cpp as pure
// functions; everything here is the wiring: what state to feed it, when it is allowed to run, and
// what to do with the credit it earns.

SurfAssist::FTuning ASurfboardPawn::GetAssistTuning() const
{
	SurfAssist::FTuning T;   // struct defaults are the fallback when no subsystem is running
	// Reuses the pawn's BeginPlay-cached subsystem pointer (the same one the pump pipeline reads),
	// so this is a field copy per tick with no lookup.
	if (!Tuning)
	{
		return T;
	}
	T.BandNear               = Tuning->AssistBandNear;
	T.BandFar                = Tuning->AssistBandFar;
	T.BandSoftness           = Tuning->AssistBandSoftness;
	T.MaxHeadingSin          = Tuning->AssistMaxHeadingSin;
	T.HeadingP               = Tuning->AssistHeadingP;
	T.HeadingD               = Tuning->AssistHeadingD;
	T.HeadingRateSmoothingSeconds = Tuning->AssistHeadingRateSmoothingSeconds;
	T.MaxAuthority           = Tuning->AssistMaxAuthority;
	T.SteerGain              = Tuning->AssistSteerGain;
	T.TrimGain               = Tuning->AssistTrimGain;
	T.TrimNeutral            = Tuning->AssistTrimNeutral;
	T.TrimSlopeGain          = Tuning->AssistTrimSlopeGain;
	T.TrimSpeedRef           = Tuning->AssistTrimSpeedRef;
	T.TrimSpeedGain          = Tuning->AssistTrimSpeedGain;
	T.TrimTargetMin          = Tuning->AssistTrimTargetMin;
	T.TrimTargetMax          = Tuning->AssistTrimTargetMax;
	T.TrimP                  = Tuning->AssistTrimP;
	T.PumpInputThreshold     = Tuning->AssistPumpInputThreshold;
	T.PumpReleaseSeconds     = Tuning->AssistPumpReleaseSeconds;
	T.RateLimitAtFullAssist  = Tuning->AssistRateLimitAtFullAssist;
	T.RateLimitAtNoAssist    = Tuning->AssistRateLimitAtNoAssist;
	T.CreditFullSeconds      = Tuning->AssistCreditFullSeconds;
	T.CreditMidSeconds       = Tuning->AssistCreditMidSeconds;
	T.CreditZeroSeconds      = Tuning->AssistCreditZeroSeconds;
	T.AlphaAtMid             = Tuning->AssistAlphaAtMid;
	T.AssistedCreditRate     = Tuning->AssistAssistedCreditRate;
	T.WhitewaterCreditRate   = Tuning->ScoreWhitewaterCreditRate;
	T.SteerSign              = Tuning->AssistSteerSign;

	// Tired (specs/stamina.md FR3): the guard steers AWAY from the pocket. Same controller, same
	// clamp, opposite sign - positive feedback outside the band, nothing inside it. Trim is
	// scaled by its own tired gain (0 = off), never reversed. StaminaTiredGuardEverywhere (the
	// foamie) drops the "nothing inside it": the guard's band collapses onto the crest and the
	// reversed push points down the face from everywhere on it, because a rider who held the
	// pocket and made no big movement was never pushed at all.
	if (IsRiderTired())
	{
		T.SteerSign = -T.SteerSign;
		T.SteerGain *= FMath::Max(0.0f, Tuning->StaminaTiredSteerGain);
		T.TrimGain  *= FMath::Max(0.0f, Tuning->StaminaTiredTrimGain);
		T.bGuardEverywhere = Tuning->StaminaTiredGuardEverywhere >= 0.5f;
		// The assist also rate-limits steering INPUT at high alpha (FR5 there: 1.5 units/s at
		// alpha 1) - a beginner's guard against over-turning. Forcing alpha up for the reversed
		// guard must not bring that with it: a slower stick is an easier board in this game, the
		// very thing the tuning-layer attempt proved. No limit while tired.
		T.RateLimitAtFullAssist = T.RateLimitAtNoAssist;
	}
	return T;
}

// ======================================================================================
//  Stamina - see specs/stamina.md
// ======================================================================================

Stamina::FTuning ASurfboardPawn::GetStaminaTuning() const
{
	Stamina::FTuning T;   // struct defaults are the spec's first-cut values
	if (!Tuning)
	{
		return T;
	}
	T.bEnabled           = Tuning->StaminaEnabled >= 0.5f;
	T.PassiveRideSeconds = Tuning->StaminaPassiveRideSeconds;
	T.PumpCostPerSecond  = Tuning->StaminaPumpCostPerSecond;
	T.TurnFreeRateDeg    = Tuning->StaminaTurnFreeRateDeg;
	T.TurnCostPerDegree  = Tuning->StaminaTurnCostPerDegree;
	T.LowFraction        = Tuning->StaminaLowFraction;
	T.RecoverySeconds    = Tuning->StaminaRecoverySeconds;
	T.TiredExitFraction  = Tuning->StaminaTiredExitFraction;
	return T;
}

void ASurfboardPawn::UpdateStamina(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const Stamina::FTuning T = GetStaminaTuning();

	// FR6: the same gates as fall detection, for the same reason - a headless run enables player
	// controls after the handoff, and an un-gated pool would end the ride in the recording tail
	// and corrupt the CSV. Plus -unattended, like every overlay. Nothing installs, nothing drains.
	const bool bAutomated = FApp::IsUnattended() || bExternalWeightOverride
		|| !SurfDebug::CVarAutopilots.GetValueOnGameThread().IsEmpty();
	if (!T.bEnabled || bAutomated)
	{
		StaminaBar::Hide(World);
		return;
	}

	// FR1: armed with the fall triggers - the paddle and the pop-up cost nothing. Before that
	// there is no bar either: a full bar over the intro would be a promise about a ride that has
	// not started. After the ride ends the bar stays at its final value until the card covers it
	// (bRideUIBlocked hides the whole ride HUD tier), so the empty bar IS the ending for a beat.
	if (!bFallArmed || bStartScreenActive || bReplayActive || bRideUIBlocked)
	{
		StaminaBar::Hide(World);
		return;
	}

	// The StaminaForceTired switch FREEZES the pool while it is on. It is for sitting in the tired
	// state to tune it; if the pool kept draining underneath, ~90 s under the switch would tire
	// the rider for real (recovery only runs on the pool's own tired flag, which the switch never
	// sets), and flipping it back to 0 would leave a rider who stays slumped until they have
	// rested it off - which is exactly what happened the first time it was used on device.
	const bool bForcedTired = Tuning && Tuning->StaminaForceTired >= 0.5f;
	if (!bFallen && bPlayerControlsEnabled && !bForcedTired)
	{
		Stamina::FInputs In;
		In.DeltaTime = DeltaTime;
		// The gesture, not the attenuated force: bPumpActive is the stroke as the player makes it.
		// PumpInputAttenuated is zero most of the time (speed and slope attenuation) and would let
		// a tired rider pump flat out while counting as resting.
		In.PumpEffort = (WeightDistribution && WeightDistribution->bPumpActive) ? 1.0f : 0.0f;
		// The trick detector's smoothed wave-relative rate from its last tick (it runs later in
		// this same frame, inside UpdateAssist). One frame of lag on a 0.15 s smoother is nothing;
		// a second smoother that could disagree with the first would not be.
		In.HeadingRateDeg = TrickState.SmoothedRateDeg;

		Stamina::Step(In, T, StaminaState);
	}

	// FR3: tired is a state, not an ending. The assist drops out, the tired tuning layer goes on,
	// and the physics decides what happens next - the rider rests and recovers, or loses the wave /
	// falls through the ordinary endings, honestly earned. Driven from the resolved state every
	// tick rather than from Step's edges so the StaminaForceTired dev switch takes effect the tick
	// it is flipped, in either direction; SetTired is idempotent and logs only on change. Not
	// after the ride has ended: EndRide lifted the layer for good, and the switch must not put
	// it back for the card's beat.
	if (!bFallen)
	{
		SetTired(IsRiderTired());
	}

	// The dev TUNE toggle owns the same top-centre strip; step down under it while it is up.
	StaminaBar::Show(World, StaminaState.Pool, StaminaState.Pool <= T.LowFraction,
		SurfTuningHUD::IsInstalled(World));
}

void ASurfboardPawn::SurfStamina(float Fraction)
{
	StaminaState.Pool = FMath::Clamp(Fraction, 0.0f, 1.0f);
	UE_LOG(LogSurf, Display, TEXT("Stamina: pool set to %.2f from the console"), StaminaState.Pool);
	// The next UpdateStamina tick takes it from here through the normal edges: 0 tires the rider,
	// a value past the exit fraction recovers them - the log, the layer, the flag all as for real.
}

bool ASurfboardPawn::IsRiderTired() const
{
	return StaminaState.bTired || (Tuning && Tuning->StaminaForceTired >= 0.5f);
}

void ASurfboardPawn::SetTired(bool bOn)
{
	const bool bWas = Tuning && Tuning->IsTired();
	if (Tuning)
	{
		Tuning->SetTired(bOn);
	}
	if (USurferAnimInstance* RiderAnim = ResolveRiderAnim())
	{
		RiderAnim->bTired = bOn;
	}
	if (bOn != bWas)
	{
		UE_LOG(LogSurf, Display, TEXT("Stamina: rider %s (pool %.0f%%, %.1f s into the ride)"),
			bOn ? TEXT("TIRED - assist REVERSED, tired tuning layer on") : TEXT("recovered - assist normal, layer off"),
			StaminaState.Pool * 100.0f, StaminaState.RideSeconds);
	}
}

TrickScore::FTuning ASurfboardPawn::GetTrickTuning() const
{
	TrickScore::FTuning T;   // struct defaults are the measured first-cut values
	if (!Tuning)
	{
		return T;
	}
	T.TurnEntryRateDeg            = Tuning->TrickTurnEntryRateDeg;
	T.TurnExitRateDeg             = Tuning->TrickTurnExitRateDeg;
	T.TurnMinSeconds              = Tuning->TrickTurnMinSeconds;
	T.TurnMinSweepDeg             = Tuning->TrickTurnMinSweepDeg;
	T.HeadingRateSmoothingSeconds = Tuning->TrickHeadingRateSmoothingSeconds;
	T.TurnGuardDiscardFraction    = Tuning->TrickTurnGuardDiscardFraction;
	T.GradeRefSweepDeg            = Tuning->TrickGradeRefSweepDeg;
	T.GradeBiteFloor              = Tuning->TrickGradeBiteFloor;
	T.GradeDriveFloor             = Tuning->TrickGradeDriveFloor;
	T.BigTurnGrade                = Tuning->TrickBigTurnGrade;
	T.WindowSeconds               = Tuning->TrickWindowSeconds;
	T.WindowPeakMultiplier        = Tuning->TrickWindowPeakMultiplier;
	T.TurnExitLumpCredit          = Tuning->TrickTurnExitLumpCredit;
	T.ChainMaxMultiplier          = Tuning->TrickChainMaxMultiplier;
	T.BoardDifficultyK            = Tuning->TrickBoardDifficultyK;
	return T;
}

float ASurfboardPawn::GetBoardScoreMultiplier() const
{
	// The difficulty rating is authored in Content/Boards/*.json and already drawn as pips on the
	// rack card, so the number that pays is the number the player read before they picked. No new
	// field, and no new concept to explain.
	const USurfBoardSubsystem* Boards = SurfBoards::Get(this);
	const FSurfBoardProfile* Active = Boards ? Boards->GetActiveProfile() : nullptr;
	const int32 Difficulty = Active ? Active->RatingDifficulty : 1;
	return TrickScore::BoardMultiplier(Difficulty, GetTrickTuning().BoardDifficultyK);
}

bool ASurfboardPawn::IsAssistEnabledThisSession() const
{
	if (!SurfAssist::IsEnabled() || AssistMode == EAssistMode::Off)
	{
		return false;
	}

	// Live dev switch, alongside the CVar so it also works on a phone (no console there). Credit is
	// session-only, so every playtest opens at alpha 1 — flip AssistDisable in the tuning HUD before
	// tuning or eyeballing anything else, or the assist is silently in the loop.
	if (Tuning && Tuning->AssistDisable >= 0.5f)
	{
		return false;
	}

	return !IsAssistRunAutomated();
}

bool ASurfboardPawn::IsAssistRunAutomated() const
{
	// FR8 test override, deliberately checked BEFORE the automated-run gates and AFTER the master
	// switch: this is what lets the assist_guard_* tests run the controller inside a filtered replay.
	if (SurfAssist::IsForcedInTests())
	{
		return false;
	}

	// Same "is this an automated run" convention as UpdateFallDetection — deliberately reusing its
	// shape rather than re-deriving it, because a drift between the two would show up as an
	// unexplained baseline shift months later. FApp::IsUnattended() closes the same
	// cvar-arrives-a-frame-late race documented there.
	return FApp::IsUnattended()
		|| bExternalWeightOverride
		|| SurfDebug::CVarAutopilots.GetValueOnGameThread().Len() > 0;
}

float ASurfboardPawn::GetPendingAssistAlpha() const
{
	if (AssistState.bRideActive)
	{
		return AssistState.RideAlpha;   // latched; constant for the ride (FR6)
	}
	if (AssistMode == EAssistMode::AlwaysOn)
	{
		return 1.0f;
	}
	// A hand-picked level PINS alpha and stops the fade until the player chooses Auto again. Once
	// they have made a choice the board must stop changing under them — that is the whole point of
	// offering the choice.
	const int32 ManualLevel = SurfAssist::LoadManualLevel();
	if (ManualLevel != SurfAssist::kAssistLevelAuto)
	{
		return SurfAssist::AlphaForLevel(ManualLevel);
	}
	float Credit = SurfAssist::LoadCredit();
	const float ForcedCredit = SurfAssist::CreditOverride();
	if (ForcedCredit >= 0.0f)
	{
		Credit = ForcedCredit;
	}
	return SurfAssist::AlphaFromCredit(Credit, GetAssistTuning());
}

bool ASurfboardPawn::IsAssistSuppressed() const
{
	// Never in an automated run, tired or not: the controller writes the same scalars the player
	// writes, so a live one would shift every snapshot baseline and corrupt every trace comparison.
	if (IsAssistRunAutomated())
	{
		return true;
	}

	// The intro owns the board until handoff: pre-handoff the autopilot writes weight, and during a
	// rails intro the board is kinematic and the pose track is authoritative. Note this is the ONLY
	// part of the gate the badge does not share — the badge is deliberately up during the intro.
	if (!bPlayerControlsEnabled || bFallen || SurfRails::AreForcesSuppressed())
	{
		return true;
	}

	// The player-facing switches (AssistMode::Off, the AssistDisable dev toggle, surf.assist.enabled)
	// turn off the HELP. While TIRED the controller is not help - it is the difficulty, the reversed
	// guard (specs/stamina.md FR3) - so a rider who switched the assist off is still pushed off the
	// wave. Without this, being tired did nothing at all on any board with the assist off, which on
	// this project's PC is EVERY dev session: AssistDisable is 1 in Saved/TuningOverrides.json, and
	// the owner's "I forced tired and the board still surfs forever" (2026-09-22) was exactly that.
	// Switch the tired guard off with StaminaEnabled 0 or StaminaTiredAssistAlpha 0, not with an
	// assist switch. No score comes of it either way: UpdateAssist only opens an assist ride when
	// the assist is genuinely enabled, so credit and trick scoring stay off while it is disabled.
	if (!IsAssistEnabledThisSession() && !IsRiderTired())
	{
		return true;
	}
	return false;
}

float ASurfboardPawn::GetEarnedAssistAlpha() const
{
	if (AssistMode == EAssistMode::AlwaysOn)
	{
		return 1.0f;
	}
	const int32 ManualLevel = SurfAssist::LoadManualLevel();
	if (ManualLevel != SurfAssist::kAssistLevelAuto)
	{
		return SurfAssist::AlphaForLevel(ManualLevel);
	}
	const float Credit = AssistState.bRideActive ? AssistState.CreditSeconds : SurfAssist::LoadCredit();
	return SurfAssist::AlphaFromCredit(Credit, GetAssistTuning());
}

// specs/ride-score-counter.md FR5. Credit in, and one side effect: the moment the accumulated credit
// crosses a schedule threshold, the counter that earned it reacts.
//
// The reaction is on the COUNTER, not the board. The crossing happens mid-ride but alpha is latched
// for the ride (gradual-control-handoff.md FR6), so anything here that claimed the assist had just
// changed would be lying for the rest of the wave. This celebrates EARNING the step down; the card
// on the next ride explains the board changing. Two moments, two messages.
void ASurfboardPawn::AccrueRideCredit(float DeltaTime, bool bGuardActive, float SpeedCmPerSecond,
	const SurfAssist::FTuning& T, const ASharedCalculations* SC)
{
	if (!AssistState.bRideActive)
	{
		// A fall needs no special case anywhere in trick scoring: the ride is over, this returns,
		// the window stops paying, and the turn that preceded it is worth whatever was collected
		// before the board went down. That IS FR9's requirement.
		return;
	}

	// brokenGeo (the wave-geometry service, 2026-09-21) is fed through, but ScoreWhitewaterCreditRate
	// is 1.0 (FR12 retired 2026-09-18 because the foam read false-positived through turns), so it
	// prices nothing by default; pricing the whitewater again is the scoring spec's call. Missing
	// SC (autopilot rides, tests) = face.
	const float Broken = SC ? SC->brokenGeo : 0.0f;
	const float BaseCredit = SurfAssist::CreditEarned(DeltaTime, bGuardActive, SpeedCmPerSecond, T, Broken);
	AssistState.CreditSeconds += BaseCredit;

	// ---- Trick scoring (specs/trick-scoring.md) -----------------------------------------------
	// Sits here because this is where the credit rate already exists, and the window is one more
	// factor on the same product - with ONE factor swapped. The survival base above pays a quarter
	// outside the guard band; the window's base pays TrickWindowGuardRate there instead (1.0 by
	// default, so the band is ignored). D9: the band is 100-200 cm from the crest and any turn
	// that sweeps 60+ degrees leaves it, so a shared 0.25 put every real turn's window on a quarter
	// base and a 3x chain paid 0.75x the straight-line rate. Speed and whitewater factors are the
	// ones the survival term used, not re-estimated.
	//
	// Both of UpdateAssist's paths reach this, which matters more than it looks: every board past
	// the funboard is authored at assistLevel 0, so the graduated branch is the one most rides
	// actually take. Trick scoring that only ran under the controller would be invisible on three
	// of the five boards.
	if (SC && SurfboardActor && DeltaTime > 0.0f)
	{
		TrickScore::FInputs TIn;
		const FVector WaveBack = SC->resolvedWaveBackDirection.IsZero()
			? SC->waveBackDirection : SC->resolvedWaveBackDirection;
		// Board axes read off the actor, never assumed: the mesh is rotated 90 degrees so forwards
		// is local +Y. Same convention as UpdateAssist and GetBoardWorldRollDeg.
		const FVector BoardFwd = SurfboardActor->GetActorTransform()
			.TransformVectorNoScale(FVector(0.0, 1.0, 0.0));

		TIn.WaveRelativeHeadingDeg = TrickScore::WaveRelativeHeadingDeg(BoardFwd, WaveBack);
		TIn.SpeedCmPerSecond       = SpeedCmPerSecond;
		TIn.SideslipAbs            = (float)FMath::Abs(SC->cosYawAngleOfAttackLeftN);

		// Same call as the survival base, same speed and whitewater factors; only the out-of-band
		// rate differs. Routed through CreditEarned rather than rebuilt here so the two bases cannot
		// drift apart on any factor but the one D9 names.
		SurfAssist::FTuning TWindow = T;
		TWindow.AssistedCreditRate = FMath::Clamp(Tuning ? Tuning->TrickWindowGuardRate : 1.0f, 0.0f, 1.0f);
		TIn.BaseCreditEarned       = SurfAssist::CreditEarned(DeltaTime, bGuardActive, SpeedCmPerSecond, TWindow, Broken);

		// FR5 discards a turn the ASSIST steered, and it reads bGuardActive to know. That flag means
		// two different things depending on the path that set it. Under the controller (alpha > 0)
		// it is "the assist is correcting right now", which is what FR5 is about. On the graduated
		// path (alpha 0 - every board past the funboard) it is recomputed as "the board is outside
		// the crest band", purely to pay survival credit at a quarter rate out of the pocket: the
		// assist steers nothing there and cannot have driven anything.
		//
		// Passing it through unqualified was the bug behind the first device session: a sharp turn
		// on a shortboard always leaves the band, so the guard read active for most of every arc and
		// FR5 threw the turn away as the assist's. An offline replay of the best ride's yaw through
		// the same detector scored 12 turns, 5 of them big; in-game it scored 1. The window payment
		// is unaffected either way - BaseCredit already carries the quarter rate, and out of the
		// pocket SHOULD pay less. Only the discard needs the assist to actually have authority.
		TIn.bGuardActive           = bGuardActive && AssistAlpha > 0.0f;

		const TrickScore::FTuning TT = GetTrickTuning();
		const TrickScore::FOutput TOut = TrickScore::Tick(TIn, TT, DeltaTime, TrickState);

		TrickWindowMultiplier = TOut.WindowMultiplier;
		TrickChainStep        = TOut.ChainStep;

		if (TOut.bTurnScored)
		{
			// FR6's two tiers, with no words in either. Every scored turn snaps the number so the
			// player sees the game noticed; a big one adds the burst. The caption and the x2 / x3
			// badge were cut on device: text pulls the eyes off the wave, and someone who just made
			// a good turn does not need telling. See RideScore::PlayCelebration.
			RideScore::PlayCelebration(GetWorld(), TOut.bBigTurn);

			UE_LOG(LogSurf, Display,
				TEXT("Trick: turn scored sweep=%.0fdeg grade=%.2f%s chain=%d window=%.1fx for %.1fs ")
				TEXT("(trick credit %.2fs)"),
				TOut.TurnSweepDeg, TOut.TurnGrade, TOut.bBigTurn ? TEXT(" BIG") : TEXT(""),
				TOut.ChainStep, TrickState.WindowPeakMultiplier, TT.WindowSeconds,
				TrickState.TrickCreditSeconds);
		}
		else if (SurfDebug::IsFlagSet(TEXT("tricks")))
		{
			// Throttled state dump. "No turn scored" and "the heading signal is dead" look identical
			// from outside without this.
			static float TrickLogTimer = 0.0f;
			TrickLogTimer += DeltaTime;
			if (TrickLogTimer >= 0.25f)
			{
				TrickLogTimer = 0.0f;
				// d2crest/band/foam are here because the 2026-09-18 session had to be re-run with
				// a second flag to learn that the whole post-turn ride was out of band: "the window
				// paid nothing" and "the base was a quarter" are indistinguishable without them.
				UE_LOG(LogSurf, Warning,
					TEXT("Trick: head=%+.1fdeg rate=%+.1fdeg/s turnOpen=%s sweep=%.0f | window=%.2fx %.1fs chain=%d ")
					TEXT("| slip=%.2f survBase=%.4f winBase=%.4f d2crest=%.0f band=%s foam=%.2f trickCredit=%.2fs"),
					TOut.HeadingDeg, TOut.HeadingRateDeg,
					TrickState.bTurnOpen ? TEXT("YES") : TEXT("no"),
					TrickState.bTurnOpen ? FMath::Abs(TrickState.UnwrappedHeadingDeg - TrickState.TurnStartHeadingDeg) : 0.0f,
					TOut.WindowMultiplier, TrickState.WindowRemaining, TOut.ChainStep,
					TIn.SideslipAbs, BaseCredit, TIn.BaseCreditEarned,
					SC->signedDistanceToCrest, bGuardActive ? TEXT("OUT") : TEXT("in"), Broken,
					TrickState.TrickCreditSeconds);
			}
		}

		if (TOut.bTurnDiscardedByGuard && SurfDebug::IsFlagSet(TEXT("tricks")))
		{
			// "Why did that turn not score" is otherwise unanswerable from outside.
			UE_LOG(LogSurf, Warning,
				TEXT("Trick: turn sweep=%.0fdeg DISCARDED - the steering guard drove most of it (FR5)."),
				TOut.TurnSweepDeg);
		}
	}
	AssistCreditSeconds = AssistState.CreditSeconds;
	AssistRideCreditSeconds = FMath::Max(0.0f, AssistState.CreditSeconds - AssistRideStartCredit);

	const int32 EarnedLevel = SurfAssist::LevelForAlpha(
		SurfAssist::AlphaFromCredit(AssistState.CreditSeconds, T));
	if (AssistEarnedLevel >= 0 && EarnedLevel < AssistEarnedLevel)
	{
		// The "LESS HELP NEEDED" celebration is gone with the rest of the assist. It fired when
		// accumulated credit crossed a schedule threshold, and said the player now needs less help
		// - which was true when help faded as you earned it. It is not true any more: a board's
		// assist level is a property of the board you chose, fixed for as long as you ride it, so
		// nothing steps down mid-wave and there is nothing to celebrate crossing. The step-down
		// sound went with it for the same reason.
		//
		// Credit still accrues - it is what the score and the per-board best are made of. Only the
		// claim that the help just decreased is retired.
		SurfAssist::SaveLastShownLevel(EarnedLevel);

		UE_LOG(LogSurf, Display,
			TEXT("RideScore: credit threshold crossed, level %d -> %d at credit=%.1fs (score %d). ")
			TEXT("Nothing on screen changes - a board's assist is fixed (alpha %.2f)."),
			AssistEarnedLevel, EarnedLevel, AssistState.CreditSeconds,
			SurfAssist::ScoreFromCredit(AssistState.CreditSeconds), AssistState.RideAlpha);
	}
	AssistEarnedLevel = EarnedLevel;
}

// The counter itself. Reads state the assist already computed and shows it; it decides nothing.
void ASurfboardPawn::UpdateRideScore()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// FR6: same gate as the badge. Inert in every automated run, and out of the way behind the start
	// screen, which is a modal.
	if (bStartScreenActive || !IsAssistEnabledThisSession())
	{
		RideScore::Hide(World);
		return;
	}

	// The wipeout card carries the score and the best itself; a second copy in the corner behind
	// it would be two numbers for one ride.
	if (WipeoutPanel::IsOpen(World))
	{
		RideScore::Hide(World);
		return;
	}

	// Before the first handoff there is no ride and nothing has been earned yet, so a zero sitting on
	// screen through the paddle-out would be a promise the game has not made. The badge covers that
	// stretch; the counter starts when the player does.
	if (!AssistState.bRideActive && AssistRideCreditSeconds <= 0.0f)
	{
		RideScore::Hide(World);
		return;
	}

	// A ride that has ended keeps its final number up rather than clearing it: this IS FR4's
	// "after a fall" surface. The counter is already on screen, so showing the result there costs no
	// new widget and no modal on every wipeout.
	const bool bRideOver = !AssistState.bRideActive;

	// Survival + tricks, and FR11's board multiplier applied HERE rather than in either accumulator.
	// Both the live number and the best go through the same call, so the two are always in the same
	// units - which is what lets the multiplier be re-tuned without invalidating a stored best.
	const float BoardMult = GetBoardScoreMultiplier();
	const float RideCredit = bRideOver
		? AssistRideCreditSeconds
		: FMath::Max(0.0f, AssistState.CreditSeconds - AssistRideStartCredit);

	// The counter reads "earning fast" while the guard is silent OR while a trick window is paying,
	// because both are states where the number is genuinely climbing faster than the floor rate.
	const bool bEarningFast = !bAssistGuardActive || TrickWindowMultiplier > 1.0f;

	RideScore::Show(World,
		TrickScore::ScoreShown(RideCredit, TrickState.TrickCreditSeconds, BoardMult),
		bEarningFast,
		TrickScore::ScoreShown(AssistBestRideSeconds, 0.0f, BoardMult),
		bRideOver);
}

void ASurfboardPawn::BeginAssistRide()
{
	AssistState.Reset();

	float Credit = SurfAssist::LoadCredit();
	const float ForcedCredit = SurfAssist::CreditOverride();
	if (ForcedCredit >= 0.0f)
	{
		Credit = ForcedCredit;   // test hook: land anywhere on the schedule without riding there
	}
	AssistState.CreditSeconds = Credit;

	AssistState.RideAlpha = GetPendingAssistAlpha();   // still !bRideActive, so this reads the schedule
	AssistState.bRideActive = true;

	AssistCreditSeconds = Credit;

	// specs/ride-score-counter.md FR3: the in-ride counter is the delta from here, not the running
	// total, so a wipeout resets it and there is something to lose.
	AssistRideStartCredit = Credit;
	AssistRideCreditSeconds = 0.0f;
	// The window, the chain and the ride's trick total all start empty. Nothing here may survive a
	// wipeout, or the next ride opens holding a multiplier it did not earn.
	TrickState.Reset();
	TrickWindowMultiplier = 1.0f;
	TrickChainStep = 0;
	AssistBestRideSeconds = SurfAssist::LoadBestRide(GetActiveBoardId());
	AssistEarnedLevel = SurfAssist::LevelForAlpha(
		SurfAssist::AlphaFromCredit(Credit, GetAssistTuning()));

	UE_LOG(LogSurf, Display,
		TEXT("SurfAssist: ride begins. credit=%.1fs alpha=%.2f mode=%s storage=%s"),
		Credit, AssistState.RideAlpha,
		AssistMode == EAssistMode::AlwaysOn ? TEXT("AlwaysOn") : TEXT("Auto"),
		SurfAssist::IsPersistenceEnabled() ? TEXT("persistent") : TEXT("session-only"));

	// FR7's WORDS live on the start screen (SStartTutorial::BuildStartScreen), not here. Firing them
	// at handoff put them on top of the "...and surf!" cue at the exact moment the player should be
	// watching the wave — measured on device 2026-08-26. The badge is the only in-ride signal.
	if (AssistState.RideAlpha <= 0.0f && Credit > 0.0f)
	{
		UE_LOG(LogSurf, Display, TEXT("SurfAssist: player graduated (credit %.1fs) — assist is off from here."), Credit);
	}
}

void ASurfboardPawn::EndAssistRide()
{
	if (!AssistState.bRideActive)
	{
		return;
	}
	AssistState.bRideActive = false;

	// FR9: the one place credit leaves the controller. Session mode keeps it in-process (a graduated
	// player stays graduated across rides); only an app restart clears it.
	SurfAssist::SaveCredit(AssistState.CreditSeconds);

	// FR4: the best is what makes the next attempt specific - "beat 340", not "keep surfing".
	const float RideCredit = FMath::Max(0.0f, AssistState.CreditSeconds - AssistRideStartCredit);
	AssistRideCreditSeconds = RideCredit;

	// FR8: a ride's trick points fold into the same per-board best. Banked RAW - survival plus
	// tricks, with no board multiplier applied. Baking FR11's multiplier in here would silently
	// invalidate every best already stored the first time it is re-tuned, leaving one board's
	// history in two different currencies with nothing marking the change. It is applied at
	// presentation instead, to both sides of the comparison.
	const float RideTotal = RideCredit + FMath::Max(0.0f, TrickState.TrickCreditSeconds);

	// best-ride-replay.md D8: the ride ends HERE, but the trace file closes somewhere else - at the
	// fall, ~90 lines later; at EndPlay, one line earlier. The record needs both, so the score waits
	// here for the file to close rather than naming a file that is still growing. The float best
	// below is unaffected: it is what the board card reads and it has no filename to be wrong about.
	//
	// Only the SCORE is set here. Whether there is a ride to list at all is decided by the recorder
	// (StartInputTrace), not by the assist: this function does not run when the assist is switched
	// off, and a player who turns it off must still be able to watch the ride they just did.
	PendingRideScoreCredit = RideTotal;
	if (RideTotal > SurfAssist::LoadBestRide(GetActiveBoardId()))
	{
		SurfAssist::SaveBestRide(GetActiveBoardId(), RideTotal);
	}
	AssistBestRideSeconds = SurfAssist::LoadBestRide(GetActiveBoardId());

	// Reports both terms and the shown total. Quoting only the survival score here under-reported
	// the ride the moment tricks existed, which made a ride that HAD just set the best look as
	// though it had not.
	const float EndBoardMult = GetBoardScoreMultiplier();
	UE_LOG(LogSurf, Display,
		TEXT("SurfAssist: ride ends. credit=%.1fs ride=%.1fs + tricks=%.1fs => score %d (best %d, ")
		TEXT("board x%.2f) alpha was %.2f"),
		AssistState.CreditSeconds, RideCredit, TrickState.TrickCreditSeconds,
		TrickScore::ScoreShown(RideCredit, TrickState.TrickCreditSeconds, EndBoardMult),
		TrickScore::ScoreShown(AssistBestRideSeconds, 0.0f, EndBoardMult),
		EndBoardMult, AssistState.RideAlpha);

	bAssistGuardActive = false;
}

void ASurfboardPawn::UpdateAssist(float DeltaTime, float& WeightRight, float& WeightInFront)
{
	// Nominate the SC that draws the band overlay — the same one the controller reads below, so the
	// drawn band cannot be a picture of a different sample than the enforced one. Done before the
	// suppression check on purpose: the overlay's whole job is diagnosing the band, which you want
	// to do with the assist switched OFF as often as on.
	if (Tuning && Tuning->AssistDrawBand >= 0.5f)
	{
		if (ASharedCalculations* BandSC = ResolveSharedCalcForFall())
		{
			BandSC->bAssistBandReference = true;
		}
	}

	if (IsAssistSuppressed() || DeltaTime <= 0.0f)
	{
		EndAssistRide();          // no-op unless a ride was actually running
		AssistAlpha = 0.0f;
		return;
	}

	// Only a genuinely-enabled assist opens a scoring ride. A tired rider whose assist is switched
	// off reaches here for the reversed guard alone (see IsAssistSuppressed); with no ride open,
	// AccrueRideCredit returns immediately and nothing is scored - which is what "the assist is off"
	// has always meant for the score.
	if (!AssistState.bRideActive && IsAssistEnabledThisSession())
	{
		BeginAssistRide();
	}

	const SurfAssist::FTuning T = GetAssistTuning();

	// Alpha precedence: console CVar, then the tuning-HUD pin, then this ride's latched value.
	// Both overrides are live-editable so a specific assist strength can be felt without riding to
	// it; unlike the latch they are allowed to move mid-ride, because they only exist for dev work.
	float Alpha = AssistState.RideAlpha;
	if (Tuning && Tuning->AssistAlphaForce > 0.0f)
	{
		Alpha = FMath::Clamp(Tuning->AssistAlphaForce, 0.0f, 1.0f);
	}
	const float Forced = SurfAssist::AlphaOverride();
	if (Forced >= 0.0f)
	{
		Alpha = FMath::Clamp(Forced, 0.0f, 1.0f);
	}
	// Tired (specs/stamina.md FR3): the assist runs REVERSED (GetAssistTuning flips the steer sign)
	// at its own strength, forced up from whatever the board rides at so it bites on every board.
	// Above every override on purpose - a pinned alpha is a dev tool for feeling a strength, and
	// tired is not a strength to feel. StaminaTiredAssistAlpha 0 = no reversed assist: tired then
	// just has no assist at all.
	if (IsRiderTired())
	{
		Alpha = FMath::Clamp(Tuning ? Tuning->StaminaTiredAssistAlpha : 1.0f, 0.0f, 1.0f);
	}
	AssistAlpha = Alpha;

	// Graduated (or forced off): return the player's input completely untouched, so a post-assist
	// ride is bit-identical to a build with no assist compiled in. Running the controller at alpha 0
	// would still put the rate limiter in the path, and "identical" has to mean identical.
	//
	// Credit still accrues, though (specs/ride-score-counter.md). Freezing the counter the moment the
	// player graduates would kill the score at exactly the point the assist stops supplying its own
	// progression - the boredom problem the counter exists to solve, arriving on schedule. The guard
	// state is recomputed from SurfAssist::BandError, which is a pure read of a value
	// ASharedCalculations already has: no correction, no rate limiter, no weight touched.
	if (Alpha <= 0.0f)
	{
		const ASharedCalculations* GraduatedSC = ResolveSharedCalcForFall();
		bAssistGuardActive = GraduatedSC && SurfAssist::BandError(
			GraduatedSC->signedDistanceToCrest, T.BandNear, T.BandFar) != 0.0f;

		float GraduatedSpeed = 0.0f;
		if (SurfboardActor)
		{
			if (UStaticMeshComponent* Mesh = SurfboardActor->GetStaticMeshComponent())
			{
				GraduatedSpeed = (float)Mesh->GetPhysicsLinearVelocity().Size();
			}
		}
		AccrueRideCredit(DeltaTime, bAssistGuardActive, GraduatedSpeed, T, GraduatedSC);
		return;
	}

	ASharedCalculations* SC = ResolveSharedCalcForFall();
	if (!SC || !SurfboardActor)
	{
		return;   // nothing to steer against; leave the player's input alone
	}

	SurfAssist::FInputs In;
	In.SignedDistanceToCrest = SC->signedDistanceToCrest;
	In.BoardWideSlopeSin     = SC->boardWideSlopeSin;
	In.WaveBackDirection     = SC->resolvedWaveBackDirection.IsZero()
		? SC->waveBackDirection : SC->resolvedWaveBackDirection;

	// Board axes read from the actor, never assumed: the mesh is rotated 90 degrees, so forwards is
	// local +Y and left is local -X (same convention as GetBoardWorldRollDeg).
	const FTransform BoardTF = SurfboardActor->GetActorTransform();
	In.BoardForward = BoardTF.TransformVectorNoScale(FVector(0.0, 1.0, 0.0));
	In.BoardLeft    = BoardTF.TransformVectorNoScale(FVector(-1.0, 0.0, 0.0));
	if (UStaticMeshComponent* Mesh = SurfboardActor->GetStaticMeshComponent())
	{
		In.BoardVelocity = Mesh->GetPhysicsLinearVelocity();
	}

	In.PlayerWeightRight   = WeightRight;
	In.PlayerWeightInFront = WeightInFront;
	// UpdatePumpInput() has already run this tick, so this is the live stroke, not last frame's.
	In.PumpInput           = PumpInput;

	const SurfAssist::FOutput Out = SurfAssist::Evaluate(In, T, Alpha, DeltaTime, AssistState);

	WeightRight   = Out.WeightRight;
	WeightInFront = Out.WeightInFront;
	// The score reads the AUTHORED band (bOutsideBand), not the one the controller ran on: while
	// tired the guard's band may sit on the crest (StaminaTiredGuardEverywhere) so the reversed
	// push covers the whole face, and a push that works against the player is not help the score
	// should dock, nor a reason to move the pocket it pays for. Same flag at any other time.
	bAssistGuardActive = Out.bOutsideBand;

	// FR6: credit accrues at full rate only while the guard is silent. Seconds spent being shepherded
	// were earned by the assist, not the player, so they count for a quarter.
	AccrueRideCredit(DeltaTime, Out.bOutsideBand, (float)In.BoardVelocity.Size(), T, SC);

	if (SurfDebug::IsFlagSet(TEXT("assist")))
	{
		static float AssistLogTimer = 0.0f;
		AssistLogTimer += DeltaTime;
		if (AssistLogTimer >= 0.25f)
		{
			AssistLogTimer = 0.0f;
			UE_LOG(LogSurf, Warning,
				TEXT("Assist alpha=%.2f gain=%.1f/%.1f credit=%.1fs rate=%.2fx | d2crest=%.0f bandErr=%.0f guard=%s | ")
				TEXT("headSin=%.3f target=%.3f err=%.3f steer=%+.3f | pump=%.2f%s trimTarget=%.3f trim=%+.3f | ")
				TEXT("right %.3f->%.3f inFront %.3f->%.3f"),
				Alpha, T.SteerGain, T.TrimGain,
				AssistState.CreditSeconds, Out.bOutsideBand ? T.AssistedCreditRate : 1.0f,
				In.SignedDistanceToCrest, Out.BandError, Out.bGuardActive ? TEXT("ON") : TEXT("silent"),
				Out.HeadingSin, Out.TargetHeadingSin, Out.HeadingError, Out.SteerCorrection,
				In.PumpInput, Out.bPumpRelease ? TEXT("(RELEASED)") : TEXT(""),
				Out.TrimTarget, Out.TrimCorrection,
				In.PlayerWeightRight, Out.WeightRight, In.PlayerWeightInFront, Out.WeightInFront);
		}
	}
}

void ASurfboardPawn::ApplyActiveBoard()
{
	// Scripted runs ride the compiled defaults, always. The snapshot baselines were recorded that
	// way, so a board applied here would retune the physics under every one of them and present as a
	// regression in a test nobody had touched (FR8).
	if (SurfBoards::IsScriptedRun(bExternalWeightOverride) && !SurfBoards::ForcedInTests())
	{
		return;
	}
	if (SurfBoards::ForcedInTests())
	{
		// Loud, because a baseline recorded in this state would be wrong in a way nothing downstream
		// could detect.
		UE_LOG(LogSurf, Warning,
			TEXT("SurfboardPawn: -BoardInTests is set - a board WILL be applied in this scripted run. Never approve a baseline from it."));
	}

	USurfBoardSubsystem* Boards = SurfBoards::Get(this);
	const FSurfBoardProfile* Profile = Boards ? Boards->GetActiveProfile() : nullptr;
	if (!Profile)
	{
		// No profiles installed: the pre-feature game, untouched.
		return;
	}

	// Coefficients first. Idempotent by construction — ApplyBoardBaseline resets every tunable to its
	// compiled default before setting this board's, so the game-instance-scoped subsystem surviving a
	// level reload cannot accumulate one board's numbers on top of another's.
	Boards->ApplyTuning(this);

	// A board's assist is fixed, never fading: a board whose feel changes over time is not a board.
	if (Profile->AssistLevel >= 0)
	{
		SurfAssist::SaveManualLevel(Profile->AssistLevel);
		AssistAlpha = GetPendingAssistAlpha();
	}

	SurfBoards::ApplyVisuals(SurfboardActor, *Profile);

	if (SurfboardActor)
	{
		if (USkeletalMeshComponent* RiderMesh = SurfboardActor->FindComponentByClass<USkeletalMeshComponent>())
		{
			// A trim so the feet stay planted, not a lift — deck thickness is held equal across
			// boards. Always measured from the authored height, never from where the last board left
			// the rider, or repeated switches would walk the surfer off the deck.
			if (!bSurferDeckBaseCaptured)
			{
				SurferDeckBaseZ = RiderMesh->GetRelativeLocation().Z;
				bSurferDeckBaseCaptured = true;
			}
			FVector Rel = RiderMesh->GetRelativeLocation();
			Rel.Z = SurferDeckBaseZ + Profile->SurferDeckOffsetZ;
			RiderMesh->SetRelativeLocation(Rel);
		}
	}

	UE_LOG(LogSurf, Display, TEXT("SurfboardPawn: board '%s' applied (assistLevel=%d)"),
		*Profile->Id, Profile->AssistLevel);
}

int32 ASurfboardPawn::GetBoardCount() const
{
	const USurfBoardSubsystem* Boards = SurfBoards::Get(this);
	return Boards ? Boards->GetBoardCount() : 0;
}

int32 ASurfboardPawn::GetActiveBoardIndex() const
{
	const USurfBoardSubsystem* Boards = SurfBoards::Get(this);
	return Boards ? Boards->GetActiveIndex() : 0;
}

FString ASurfboardPawn::GetBoardName(int32 Index) const
{
	const USurfBoardSubsystem* Boards = SurfBoards::Get(this);
	return Boards ? Boards->GetBoardName(Index) : FString();
}

FString ASurfboardPawn::GetActiveBoardId() const
{
	const USurfBoardSubsystem* Boards = SurfBoards::Get(this);
	const FSurfBoardProfile* Active = Boards ? Boards->GetActiveProfile() : nullptr;
	return Active ? Active->Id : FString();
}

FString ASurfboardPawn::GetActiveBoardName() const
{
	const USurfBoardSubsystem* Boards = SurfBoards::Get(this);
	const FSurfBoardProfile* Active = Boards ? Boards->GetActiveProfile() : nullptr;
	return Active ? Active->DisplayName : FString();
}

void ASurfboardPawn::SelectBoard(int32 Index)
{
	USurfBoardSubsystem* Boards = SurfBoards::Get(this);
	if (!Boards || !Boards->GetProfile(Index))
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn::SelectBoard - no board at index %d"), Index);
		return;
	}
	Boards->SetActiveIndex(Index);
	ApplyActiveBoard();
}

void ASurfboardPawn::RestartWithBoard(int32 Index)
{
	USurfBoardSubsystem* Boards = SurfBoards::Get(this);
	if (!Boards || !Boards->GetProfile(Index))
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn::RestartWithBoard - no board at index %d"), Index);
		return;
	}

	// Always restarts, mid-ride or not. The earlier version only restarted while riding, which made
	// the same control mean two different things and left a "restart with X" label lying between
	// waves. The selection survives the reload - the board subsystem lives on the game instance -
	// and ApplyActiveBoard runs again at BeginPlay.
	Boards->SetActiveIndex(Index);
	UE_LOG(LogSurf, Display, TEXT("SurfboardPawn: restarting onto board '%s'"),
		*Boards->GetProfile(Index)->Id);
	RestartLevel();
}

void ASurfboardPawn::OpenBoardPicker()
{
	USurfBoardSubsystem* Boards = SurfBoards::Get(this);
	if (!Boards || Boards->GetBoardCount() == 0)
	{
		return;
	}

	TWeakObjectPtr<ASurfboardPawn> WeakThis(this);
	BoardPanel::FHooks Hooks;
	Hooks.GetActiveIndex = [WeakThis]()
	{
		const ASurfboardPawn* Pawn = WeakThis.Get();
		return Pawn ? Pawn->GetActiveBoardIndex() : 0;
	};
	Hooks.GetBestScore = [WeakThis](int32 Index) -> int32
	{
		const ASurfboardPawn* Pawn = WeakThis.Get();
		const USurfBoardSubsystem* Boards = Pawn ? SurfBoards::Get(Pawn) : nullptr;
		const FSurfBoardProfile* P = Boards ? Boards->GetProfile(Index) : nullptr;
		if (!P)
		{
			return -1;
		}
		const float Credit = SurfAssist::LoadBestRide(P->Id);
		// Never ridden reads as a dash, not a zero: a board you have not tried is an invitation.
		return Credit > 0.0f ? SurfAssist::ScoreFromCredit(Credit) : -1;
	};
	Hooks.GetBoardName = [WeakThis](int32 Index) -> FString
	{
		const ASurfboardPawn* Pawn = WeakThis.Get();
		return Pawn ? Pawn->GetBoardName(Index) : FString();
	};
	// A pick, not a restart (specs/two-screen-navigation.md FR5): the rack opens from the hub,
	// between waves, where SelectBoard applies on the spot and the next START rides the choice.
	Hooks.ChooseBoard = [WeakThis](int32 Index)
	{
		if (ASurfboardPawn* Pawn = WeakThis.Get())
		{
			Pawn->SelectBoard(Index);
		}
	};
	Hooks.OnClosed = [WeakThis]()
	{
		if (ASurfboardPawn* Pawn = WeakThis.Get())
		{
			Pawn->bRideUIBlocked = Pawn->bStartScreenActive;
			Pawn->UpdateRideHud();
		}
	};

	// Set here, not polled in Tick: the rack pauses the world, so the pawn stops ticking the moment
	// it opens and a polled flag could never turn true. bStartScreenActive is written at its
	// transition for the same reason.
	bRideUIBlocked = true;
	UpdateRideHud();

	BoardPanel::Open(GetWorld(), Boards->GetProfiles(), Hooks);
}

void ASurfboardPawn::OpenAbout()
{
	TWeakObjectPtr<ASurfboardPawn> WeakThis(this);
	AboutPanel::FHooks Hooks;
	Hooks.OnClosed = [WeakThis]()
	{
		if (ASurfboardPawn* Pawn = WeakThis.Get())
		{
			Pawn->bRideUIBlocked = Pawn->bStartScreenActive;
			Pawn->UpdateRideHud();
		}
	};

	// Same as the rack: set at the transition, because the panel keeps the world paused and a
	// polled flag would never turn true.
	bRideUIBlocked = true;
	UpdateRideHud();

	AboutPanel::Open(GetWorld(), Hooks);
}

void ASurfboardPawn::SetAssistMode(EAssistMode NewMode)
{
	if (AssistMode == NewMode)
	{
		return;
	}
	AssistMode = NewMode;
	UE_LOG(LogSurf, Display, TEXT("SurfAssist: mode set to %d (takes effect next ride)"), (int32)NewMode);

	// Deliberately does NOT re-latch the current ride's alpha: the board must not change feel
	// mid-wave. Ending the ride banks whatever credit it earned; the next handoff picks the new mode
	// up. Switching to Off also drops the badge immediately, which is the one bit of feedback the
	// setting owes the player right away.
	EndAssistRide();
	if (NewMode == EAssistMode::Off)
	{
		AssistAlpha = 0.0f;
	}
}

void ASurfboardPawn::ResetAssistCredit()
{
	// FR8: whatever the records were protecting becomes an orphan the moment they are cleared, and
	// FR3's grace period would then hold those files for a week for no reason. Collected BEFORE the
	// reset, because after it there is nothing left to say which files they were.
	const TSet<FString> Orphans = SurfAssist::ReferencedTraceFiles();

	SurfAssist::ResetCredit();
	DeleteTraceFiles(Orphans);
	SurfAssist::SaveManualLevel(SurfAssist::kAssistLevelAuto);
	AssistState.CreditSeconds = 0.0f;
	AssistCreditSeconds = 0.0f;

	// End the ride rather than re-latching alpha mid-wave — same reasoning as SetAssistMode. The
	// next handoff comes up at full assist, which is what "hand the phone to the next tester" means.
	EndAssistRide();

	UE_LOG(LogSurf, Display, TEXT("SurfAssist: credit reset — next ride starts at full assist."));
}

void ASurfboardPawn::UpdateFallDetection(float DeltaTime)
{
	if (!bFallEnabled || bFallen || !bPlayerControlsEnabled || !SurfboardActor)
	{
		return;
	}

	// Never fall in test runs. The pawn DOES enable player controls once the test autopilot
	// finishes (handoff delay), so without this gate the planing trigger fires in the
	// post-recording tail of snapshot runs — and would corrupt trace-replay tests, which
	// record AFTER handoff. Same "is this a test run" convention as bAutoQuitOnComplete
	// (surf.autopilots filter non-empty); bExternalWeightOverride covers -ReplayTrace runs
	// that bypass the filter.
	if (bExternalWeightOverride || SurfDebug::CVarAutopilots.GetValueOnGameThread().Len() > 0)
	{
		return;
	}

	ASharedCalculations* SC = ResolveSharedCalcForFall();
	const float Planing = SC ? SC->AmountPlaning : -1.0f;

	// Arming: the ride has to actually get underway before either trigger is live, or
	// "not planing" would fire instantly while paddling. No SC in the level = never arms.
	if (!bFallArmed)
	{
		if (Planing > fallPlaningArmThreshold)
		{
			bFallArmed = true;
			UE_LOG(LogSurf, Display, TEXT("SurfboardPawn: Fall triggers ARMED (AmountPlaning=%.2f > %.2f)"),
				Planing, fallPlaningArmThreshold);
		}
		return;
	}

	const float RollDeg = GetBoardWorldRollDeg();
	if (FMath::Abs(RollDeg) > fallRollThresholdDeg)
	{
		TriggerFall(FString::Printf(TEXT("roll %.1f deg (threshold %.1f)"), RollDeg, fallRollThresholdDeg));
		return;
	}

	if (Planing >= 0.0f && Planing < fallPlaningStopThreshold)
	{
		// Not a wipeout: the board lost the wave and stopped. Same ragdoll, but the card says
		// what happened - see ERideEndKind.
		EndRide(ERideEndKind::LostWave,
			FString::Printf(TEXT("lost planing (%.3f < %.3f)"), Planing, fallPlaningStopThreshold));
		return;
	}

	// The off-wave ending: off the clean wave (the wave-geometry service's zone at the SC not Pocket
	// or Shoulder - whitewater, behind, the flat) for FallOffWaveSeconds and slow at that moment.
	// The timer only knows the zone when the level has a model (Unknown = no model = never).
	// See specs/surfer-fall-ragdoll.md, "Off the wave".
	{
		const USurfTuningSubsystem* FallTuning = SurfTuning::Get(this);
		const float OffSeconds = FallTuning ? FallTuning->FallOffWaveSeconds : 0.0f;
		const float OffSpeed   = FallTuning ? FallTuning->FallOffWaveSpeed : 0.0f;
		const EWaveZone Zone = SC ? SC->waveZone : EWaveZone::Unknown;
		if (OffSeconds <= 0.0f || Zone == EWaveZone::Unknown)
		{
			OffWaveSeconds = 0.0f;
		}
		else if (Zone == EWaveZone::Pocket || Zone == EWaveZone::Shoulder)
		{
			OffWaveSeconds = 0.0f;
		}
		else
		{
			OffWaveSeconds += DeltaTime;
			const float Speed = SC ? (float)SC->componentVelocity.Size2D() : 0.0f;
			if (OffWaveSeconds >= OffSeconds && Speed < OffSpeed)
			{
				EndRide(ERideEndKind::LostWave,
					FString::Printf(TEXT("off the wave for %.1f s (%s) at %.0f cm/s (< %.0f)"),
						OffWaveSeconds, UWaveGeometrySubsystem::ZoneName(Zone), Speed, OffSpeed));
			}
		}
	}
}

void ASurfboardPawn::TriggerFall(const FString& Reason)
{
	EndRide(ERideEndKind::Wipeout, Reason);
}

void ASurfboardPawn::EndRide(ERideEndKind Kind, const FString& Reason)
{
	if (bFallen)
	{
		return;
	}
	bFallen = true;
	RideEndKind = Kind;

	const TCHAR* KindLabel = TEXT("WIPEOUT");
	switch (Kind)
	{
	case ERideEndKind::Wipeout:  KindLabel = TEXT("WIPEOUT");       break;
	case ERideEndKind::LostWave: KindLabel = TEXT("LOST THE WAVE"); break;
	}
	UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn::EndRide - %s: %s"), KindLabel, *Reason);

	// The stamina story of the ride, once: which drain actually shaped it, how much rest refilled,
	// how long was spent tired. That is what tuning the rates needs (specs/stamina.md FR7) and it
	// is cheap enough to log always. Then the tired layer comes off - the tuning subsystem outlives
	// the level reload, and a layer left on would tire the next ride from its first tick.
	if (GetStaminaTuning().bEnabled)
	{
		UE_LOG(LogSurf, Display,
			TEXT("Stamina: ride over after %.1f s with %.0f%% left - spent passive %.0f%%, pump %.0f%%, turns %.0f%%; recovered %.0f%%; tired %d time(s) for %.1f s"),
			StaminaState.RideSeconds, StaminaState.Pool * 100.0f,
			StaminaState.SpentPassive * 100.0f, StaminaState.SpentPump * 100.0f, StaminaState.SpentTurn * 100.0f,
			StaminaState.Recovered * 100.0f, StaminaState.TiredSpells, StaminaState.TiredSeconds);
	}
	SetTired(false);

	// Either ending is the end of a ride, so bank whatever assist credit it earned. Done explicitly
	// here rather than relying on the next UpdateAssist tick to notice bFallen: this path also stops
	// the weight write that calls it, so the tick may never come and the credit would be lost.
	EndAssistRide();

	// 1. Rider goes off the board, on EITHER ending. The stall was briefly rider-on-deck
	//    (2026-09-14: "a board that stopped is not a fall") but a rider left standing on a stopped
	//    board did not read as an ending at all - the owner took it for a ragdoll that had been
	//    reset. The two endings now differ only in what the card calls them (ERideEndKind -> title).
	//
	//    The fall is the authored clip when the AnimBP has both (SK_Falling_left / _right): detach
	//    keep-world, start the clip for the fall side, and from here UpdateFallenRider moves the mesh
	//    kinematically - the board's momentum relaxing into the water's drift, levelled, on the
	//    surface. Otherwise (clips missing, or bFallUseRagdoll for comparison) the physics ragdoll:
	//    full sim, inherit the board's velocity so the surfer gets flung, not dropped, plus a rigid
	//    topple. Needs a physics asset on the mesh (Manny ships PA_Mannequin).
	if (SurfboardActor)
	{
		if (USkeletalMeshComponent* RiderMesh = SurfboardActor->FindComponentByClass<USkeletalMeshComponent>())
		{
			// Snapshot the attachment so RestoreRiderAfterFall (Replay path) can undo this.
			RiderPreFallParent = RiderMesh->GetAttachParent();
			RiderPreFallSocket = RiderMesh->GetAttachSocketName();
			RiderPreFallRelativeTransform = RiderMesh->GetRelativeTransform();
			RiderPreFallCollisionProfile = RiderMesh->GetCollisionProfileName();

			FVector BoardVelocity = FVector::ZeroVector;
			if (UStaticMeshComponent* BoardMesh = SurfboardActor->GetStaticMeshComponent())
			{
				BoardVelocity = BoardMesh->GetPhysicsLinearVelocity();
			}

			// Side the surfer goes off: the low rail when the board is rolled (roll falls), else
			// the weighted rail (read BEFORE the recenter in step 2 below).
			// RollDeg > 0 means board.left points up = banked right = fall right (-board.left).
			const FTransform BoardTF = SurfboardActor->GetActorTransform();
			const FVector BoardLeft = BoardTF.TransformVectorNoScale(FVector(-1.0, 0.0, 0.0)).GetSafeNormal();
			const float RollDeg = GetBoardWorldRollDeg();
			float SideSign; // +1 = toward board right (-board.left)
			if (FMath::Abs(RollDeg) > 2.0f)
			{
				SideSign = (RollDeg > 0.0f) ? 1.0f : -1.0f;
			}
			else
			{
				SideSign = (WeightDistribution && WeightDistribution->amountToTheRight < 0.5f) ? -1.0f : 1.0f;
			}
			FVector SideDir = -BoardLeft * SideSign;
			SideDir.Z = 0.0f; // horizontal shove; the vertical part is the explicit upward kick
			SideDir = SideDir.GetSafeNormal();

			RiderMesh->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);

			USurferAnimInstance* RiderAnim = Cast<USurferAnimInstance>(RiderMesh->GetAnimInstance());
			const bool bAnimatedFall = !bFallUseRagdoll && RiderAnim && RiderAnim->HasFallClips();
			if (bAnimatedFall)
			{
				bRiderFallAnimated = true;
				RiderAnim->StartFall(SideSign > 0.0f);

				// Kinematic start: the board's momentum, horizontal only - the height is the water's
				// from here (UpdateFallenRider), relative to where the rider stood over it now.
				RiderFallVelocity = BoardVelocity * fallVelocityInherit;
				RiderFallVelocity.Z = 0.0f;
				RiderFallHeightAboveWater = 0.0f;
				if (AWaveHeight* Wave = ResolveWaveHeightForFall())
				{
					const FVector RiderLoc = RiderMesh->GetComponentLocation();
					const TArray<FVector> Surface = Wave->calculateWaveLocationAndNormalAuto(RiderLoc);
					if (Surface.Num() > 0)
					{
						RiderFallHeightAboveWater = RiderLoc.Z - Surface[0].Z;
					}
				}

				// Level: keep the rider's heading, drop the deck's pitch and roll, over fallLevelSeconds.
				// Built from the mesh's own axes (its up is the character's up), so it holds whatever
				// the rider's rotation on the deck is.
				RiderFallRotFrom = RiderMesh->GetComponentQuat();
				FVector Heading = RiderFallRotFrom.GetForwardVector();
				Heading.Z = 0.0f;
				RiderFallRotLevel = Heading.Normalize()
					? FRotationMatrix::MakeFromXZ(Heading, FVector::UpVector).ToQuat()
					: FQuat(FRotator(0.0f, RiderFallRotFrom.Rotator().Yaw, 0.0f));
				RiderFallLevelElapsed = 0.0f;
				RiderFallAfterClipSeconds = 0.0f;
				RiderFallSunkCm = 0.0f;

				UE_LOG(LogSurf, Display,
					TEXT("SurfboardPawn::EndRide - authored fall to the board's %s at %.0f cm/s, %.0f cm over the water"),
					SideSign > 0.0f ? TEXT("right") : TEXT("left"), RiderFallVelocity.Size2D(), RiderFallHeightAboveWater);
			}
			else
			{
				if (!bFallUseRagdoll)
				{
					UE_LOG(LogSurf, Warning,
						TEXT("SurfboardPawn::EndRide - fall clips not assigned on the rider's AnimBP (Surfer|Fall); ragdolling instead"));
				}
				RiderMesh->SetCollisionProfileName(TEXT("Ragdoll"));
				RiderMesh->SetSimulatePhysics(true);

				// Rigid topple about the feet: each body gets v = ω × (p − feet), so the head moves
				// fastest and the feet stay ~put — a fall, not a hop. (A uniform SetAllPhysics-
				// LinearVelocity kick translates the whole body sideways, and SetAllPhysicsAngular-
				// Velocity only spins each bone about its own center — neither reads as toppling.)
				// The small uniform kicks + inherited board velocity are added on top.
				const FVector UniformVel = BoardVelocity * fallVelocityInherit
					+ SideDir * fallSidewaysKick
					+ FVector::UpVector * fallUpwardKick;
				const FVector ToppleAxis = FVector::CrossProduct(FVector::UpVector, SideDir).GetSafeNormal();
				const FVector OmegaRad = ToppleAxis * FMath::DegreesToRadians(fallToppleRate);
				const FBoxSphereBounds MeshBounds = RiderMesh->Bounds;
				const FVector FeetPivot(MeshBounds.Origin.X, MeshBounds.Origin.Y,
					MeshBounds.Origin.Z - MeshBounds.BoxExtent.Z);
				for (FBodyInstance* Body : RiderMesh->Bodies)
				{
					if (!Body || !Body->IsValidBodyInstance())
					{
						continue;
					}
					const FVector BodyPos = Body->GetUnrealWorldTransform().GetLocation();
					Body->SetLinearVelocity(
						UniformVel + FVector::CrossProduct(OmegaRad, BodyPos - FeetPivot),
						/*bAddToCurrent*/false);
					// Same ω on every body = coherent rigid rotation, not per-bone spinning.
					Body->SetAngularVelocityInRadians(OmegaRad, /*bAddToCurrent*/false);
				}
			}
		}
		else
		{
			UE_LOG(LogSurf, Warning, TEXT("SurfboardPawn::EndRide - no rider SkeletalMeshComponent on the board; fall is input-lockout only"));
		}
	}

	// 2. Board stops reacting to input: lock out player controls and re-center the weight so
	//    no stale weight torque keeps steering. The board's physics stays live — it washes
	//    out / tumbles on its own.
	bPlayerControlsEnabled = false;
	bWeightInputActive = false;
	CurrentWeightOffset = FVector2D::ZeroVector;
	if (WeightDistribution)
	{
		WeightDistribution->amountInFront = 0.5f;
		WeightDistribution->amountToTheRight = 0.5f;
	}

	// 3. The ride ended here, so "latest ride" ends here too — a subsequent Replay plays up
	//    to the fall and holds. The verdict goes into the trace first, so a shared trace says
	//    what the game decided and why, not just where the rows stop.
	AppendTraceNote(FString::Printf(TEXT("ride_end=%s t=%.3f reason=%s"),
		KindLabel, InputTraceElapsedTime, *Reason));
	if (bInputTraceActive)
	{
		StopInputTrace(TEXT("ride_end"));
	}

	// 4. ...and now that the file is closed, the ride can be banked as a record (D8). Order is
	//    fixed and FR3 is explicit about why: end ride -> close file -> commit -> prune. A prune
	//    before the commit deletes the ride it was about to preserve.
	CommitRideRecords();
}

void ASurfboardPawn::UpdateFallenRider(float DeltaTime)
{
	if (!bFallen || !bRiderFallAnimated || !SurfboardActor || DeltaTime <= 0.0f)
	{
		return;
	}
	USkeletalMeshComponent* RiderMesh = SurfboardActor->FindComponentByClass<USkeletalMeshComponent>();
	if (!RiderMesh)
	{
		return;
	}
	AWaveHeight* Wave = ResolveWaveHeightForFall();
	FVector Loc = RiderMesh->GetComponentLocation();

	// Momentum into drift: the velocity relaxes from the board's toward the water's. A body in the
	// water is neither a projectile nor a cork - it carries what it had for a moment, then goes
	// where the water goes (the whitewater carries the fallen rider shoreward with it).
	FVector WaterVel = FVector::ZeroVector;
	if (Wave)
	{
		WaterVel = Wave->calculateWaveVelocityAuto(Loc) * fallWaterDrift;
		WaterVel.Z = 0.0f;
	}
	if (fallMomentumDecaySeconds > KINDA_SMALL_NUMBER)
	{
		const float Alpha = 1.0f - FMath::Exp(-DeltaTime / fallMomentumDecaySeconds);
		RiderFallVelocity += (WaterVel - RiderFallVelocity) * Alpha;
	}
	else
	{
		RiderFallVelocity = WaterVel;
	}
	Loc += RiderFallVelocity * DeltaTime;

	// Height: the water's, plus what the rider had over it at the fall. Relative on purpose - the
	// board rides submerged and the data's absolute height is not world Z, and neither matters
	// when the same offset is kept. Then, once the clip has ended, the sink: held on the surface he
	// reads as sitting on the water, so he goes under it and stays there.
	const USurferAnimInstance* RiderAnim = Cast<USurferAnimInstance>(RiderMesh->GetAnimInstance());
	float SinkThisTick = 0.0f;
	if (RiderAnim && RiderAnim->IsFallClipFinished() && fallSinkRate > 0.0f)
	{
		RiderFallAfterClipSeconds += DeltaTime;
		if (RiderFallAfterClipSeconds >= fallSinkDelaySeconds)
		{
			SinkThisTick = FMath::Min(fallSinkRate * DeltaTime, FMath::Max(fallSinkDepth - RiderFallSunkCm, 0.0f));
			RiderFallSunkCm += SinkThisTick;
		}
	}
	bool bOnSurface = false;
	if (Wave)
	{
		const TArray<FVector> Surface = Wave->calculateWaveLocationAndNormalAuto(Loc);
		if (Surface.Num() > 0)
		{
			Loc.Z = Surface[0].Z + RiderFallHeightAboveWater - RiderFallSunkCm;
			bOnSurface = true;
		}
	}
	if (!bOnSurface)
	{
		Loc.Z -= SinkThisTick;   // no surface to follow: sink from where he is
	}

	// Level, over fallLevelSeconds, then hold.
	RiderFallLevelElapsed += DeltaTime;
	const float LevelT = (fallLevelSeconds > KINDA_SMALL_NUMBER)
		? FMath::Clamp(RiderFallLevelElapsed / fallLevelSeconds, 0.0f, 1.0f) : 1.0f;
	const FQuat Rot = FQuat::Slerp(RiderFallRotFrom, RiderFallRotLevel, FMath::SmoothStep(0.0f, 1.0f, LevelT));

	RiderMesh->SetWorldLocationAndRotation(Loc, Rot);
}

AWaveHeight* ASurfboardPawn::ResolveWaveHeightForFall()
{
	if (const ASharedCalculations* SC = ResolveSharedCalcForFall())
	{
		return SC->waveVelocity;   // an AWaveHeight despite the name
	}
	return nullptr;
}

void ASurfboardPawn::RestoreRiderAfterFall()
{
	if (!bFallen)
	{
		return;
	}
	bFallen = false;
	bFallArmed = false;
	OffWaveSeconds = 0.0f;
	SetTired(false);
	Stamina::Reset(StaminaState);   // the next ride starts on a full pool, whichever path reaches it

	const bool bWasAnimated = bRiderFallAnimated;
	bRiderFallAnimated = false;
	RiderFallVelocity = FVector::ZeroVector;
	RiderFallAfterClipSeconds = 0.0f;
	RiderFallSunkCm = 0.0f;

	if (!SurfboardActor)
	{
		return;
	}
	USkeletalMeshComponent* RiderMesh = SurfboardActor->FindComponentByClass<USkeletalMeshComponent>();
	if (!RiderMesh)
	{
		return;
	}

	// Either way off: the clip stops (the ordinary pose resumes under the replay's recorded
	// weights) and the physics comes off - harmless when it was never on.
	if (USurferAnimInstance* RiderAnim = Cast<USurferAnimInstance>(RiderMesh->GetAnimInstance()))
	{
		RiderAnim->StopFall();
	}
	RiderMesh->SetSimulatePhysics(false);
	RiderMesh->SetAllBodiesSimulatePhysics(false);
	RiderMesh->SetAllBodiesPhysicsBlendWeight(0.0f);
	if (RiderPreFallCollisionProfile != NAME_None)
	{
		RiderMesh->SetCollisionProfileName(RiderPreFallCollisionProfile);
	}
	if (USceneComponent* Parent = RiderPreFallParent)
	{
		RiderMesh->AttachToComponent(Parent, FAttachmentTransformRules::KeepWorldTransform, RiderPreFallSocket);
		RiderMesh->SetRelativeTransform(RiderPreFallRelativeTransform);
	}
	RiderPreFallParent = nullptr;

	UE_LOG(LogSurf, Display, TEXT("SurfboardPawn: Rider restored from the %s fall (replay path)"),
		bWasAnimated ? TEXT("authored") : TEXT("ragdoll"));
}
