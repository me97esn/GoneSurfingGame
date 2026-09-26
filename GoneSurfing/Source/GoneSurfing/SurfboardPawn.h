// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Camera/CameraComponent.h"
#include "InputActionValue.h"
#include "Engine/StaticMeshActor.h"
#include "SurfTilt.h"
#include "SurfAssist.h"          // FState/FTuning for the gradual control handoff
#include "TrickScoring.h"        // FState/FTuning for turn scoring and the earning window
#include "Stamina.h"             // FState/FTuning for the per-ride energy budget
#include "SurferAnimInstance.h"  // ESurferAnimState (recorded per-row for the rails intro)
#include "RideListPanel.h"      // FRideListEntry: the rows the ride list draws
#include "SurfboardPawn.generated.h"

class UInputMappingContext;
class UInputAction;
class USurfTuningSubsystem;

/** FR7 of specs/gradual-control-handoff.md — the player-facing assist setting.
 *  Auto is the shipping default; the other two exist so the feature is switchable rather than
 *  something done to the player. */
UENUM(BlueprintType)
enum class EAssistMode : uint8
{
	/** Follow the ride-time fade schedule. */
	Auto     UMETA(DisplayName = "Auto (fades as you improve)"),
	/** No assist at all — an experienced player, or a second install. */
	Off      UMETA(DisplayName = "Off"),
	/** Pinned at full strength: a player who wants to keep surfing rather than graduate. */
	AlwaysOn UMETA(DisplayName = "Always on"),
};

/** Selects which input drives weight on Android. Tilt (default) uses device pitch/roll;
 *  Stick falls back to the right thumbstick (retained for A/B comparison). */
UENUM(BlueprintType)
enum class EAndroidWeightInputSource : uint8
{
	Tilt    UMETA(DisplayName = "Device Tilt"),
	Stick   UMETA(DisplayName = "Right Stick"),
};

/** Behind-camera blend mode. AutoBlend follows board heading (chase <-> bird); the Force modes
 *  lock the camera to a single endpoint so each can be positioned/tuned live in the editor. */
UENUM(BlueprintType)
enum class EBehindCameraConfig : uint8
{
	AutoBlend   UMETA(DisplayName = "Auto (blend by heading)"),
	ForceChase  UMETA(DisplayName = "Force Chase (down the line)"),
	ForceBird   UMETA(DisplayName = "Force Bird (down the wave)"),
};

/** What drives the Behind camera's chase->bird switch. Slope/Water are wave-position properties (read
 *  from SharedCalculations) so they don't twitch with board heading or pumping; Heading is the original
 *  fall-line dot product. All run through the same hysteresis + smoothing. */
UENUM(BlueprintType)
enum class EBirdTriggerSource : uint8
{
	WaveOcclusion       UMETA(DisplayName = "Wave occlusion (line of sight)"),
	WaveSlopeSteepness  UMETA(DisplayName = "Wave slope steepness (slopeSin)"),
	WaterColumnAbove    UMETA(DisplayName = "Water above board (cm)"),
	HeadingAlignment    UMETA(DisplayName = "Heading vs fall line"),
};

/** Editor-only: positions the camera at a config in the viewport (without PIE) so the camera-preview
 *  shows the framing while tuning. Has no effect at runtime, where UpdateCameraTransform drives the
 *  camera each tick. Each option maps directly to one camera endpoint. */
UENUM(BlueprintType)
enum class EEditorCameraPreview : uint8
{
	Off          UMETA(DisplayName = "Off (placed transform)"),
	BehindChase  UMETA(DisplayName = "Behind - Chase (down the line)"),
	BehindBird   UMETA(DisplayName = "Behind - Bird (down the wave)"),
	Beside       UMETA(DisplayName = "Beside"),
};

/** Fires when the pre-wave Start screen / tutorial overlay opens and closes, so UMG HUDs can
 *  show/hide their own buttons (Restart / Replay / Back) in step with it. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FStartScreenStateChanged);

/**
 * FPV Camera Pawn attached to surfboard with stabilization and paddling controls
 * - Camera follows surfboard position with configurable offset
 * - Yaw follows surfboard with smoothing, pitch/roll stabilized
 * - Left joystick controls yaw rotation relative to surfboard
 * - Right joystick up applies paddling force to surfboard
 */
UCLASS()
class GONESURFING_API ASurfboardPawn : public APawn
{
	GENERATED_BODY()

public:
	ASurfboardPawn();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnConstruction(const FTransform& Transform) override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	/** Editor-only: snap the camera to EditorCameraPreview's config so the viewport preview reflects it. */
	void ApplyEditorCameraPreview();
#endif

public:
	virtual void Tick(float DeltaTime) override;
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	// ========== Components ==========

	/** Scene root for camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<USceneComponent> CameraRoot;

	/** Camera component */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
	TObjectPtr<UCameraComponent> CameraComponent;

	// ========== Surfboard Reference ==========

	/** Reference to the surfboard StaticMeshActor in the level */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfboard")
	TObjectPtr<AStaticMeshActor> SurfboardActor;

	/** Recorded intro trace the scripted "rails" intro plays back, relative to Saved/ (e.g.
	 *  "InputTraces/intro-reference.csv"). Empty = rails off and the live trigger-driven intro runs
	 *  exactly as before — which is also the fallback if the trace is missing, too short, or
	 *  predates the angular-velocity columns. See specs/deterministic-ride-handoff.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfboard")
	FString RailsIntroTrace;

	/** True while a scripted rails intro is driving the board kinematically. Blueprint force nodes
	 *  (AddImpulseAtLocation and friends) must branch on this and skip: the board is not simulating,
	 *  so the call is a no-op that logs "surfboard has to have 'Simulate Physics' enabled" every
	 *  tick, and applying it would be wrong anyway — the pose track is authoritative.
	 *  Static, so it needs no target pin. See specs/deterministic-ride-handoff.md. */
	UFUNCTION(BlueprintPure, Category = "Surfing|Rails", meta = (DisplayName = "Are Rails Driving The Board"))
	static bool AreRailsDrivingTheBoard();

	/** Reference to the WeightDistribution actor in the level */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surfboard")
	TObjectPtr<class AWeightDistribution> WeightDistribution;

	// ========== AutoPilot Configuration ==========

	/** Reference to the state-based AutoPilot driving the cinematic intro phase */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AutoPilot")
	TObjectPtr<class AStateTriggerAutoPilot> StateTriggerAutoPilot;

	/** Author-set autopilot duration (seconds). Drives camera switch and player-control enable timing. 0 = controls enabled immediately */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AutoPilot")
	float AutoPilotDuration = 0.0f;

	/** Delay in seconds after autopilot completes before player controls are enabled */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AutoPilot")
	float ControlDelayAfterAutoPilot = 1.0f;

	// ========== Gradual control handoff (specs/gradual-control-handoff.md) ==========
	// The handoff above used to be a hard cut: controls flip on in one frame and a novice gets full
	// authority over a board that turns hard on a small weight shift. These replace that with a fade
	// — a closed-loop assist whose authority falls as the player banks unassisted ride time.

	/** Played when the player earns an assist step down, at the moment the score counter celebrates
	 *  it. Unassigned = silent, which is how it ships until a file is dropped in: the project has no
	 *  celebratory audio, and none of the Starter Content sounds (explosions, fire, wind) fit.
	 *
	 *  Named for the level-down CARD it originally accompanied. The card is gone; the sound moved to
	 *  the in-ride celebration rather than going with it. Kept under its old name because renaming a
	 *  UPROPERTY silently drops whatever a designer has already assigned to it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist")
	TObjectPtr<class USoundBase> AssistLevelDownSound;

	/** Player-facing assist setting (FR7). Auto = follow the fade schedule. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assist")
	EAssistMode AssistMode = EAssistMode::Auto;

	/** This ride's authority ceiling, 0..1. Latched at handoff and held for the ride — a board that
	 *  changes feel at second 30 of a good wave produces a fall the player cannot attribute to
	 *  anything they did. Read-only for HUDs. */
	UPROPERTY(BlueprintReadOnly, Category = "Assist")
	float AssistAlpha = 0.0f;

	/** Accumulated unassisted ride credit, seconds. Drives the fade schedule. */
	UPROPERTY(BlueprintReadOnly, Category = "Assist")
	float AssistCreditSeconds = 0.0f;

	/** True while the guard band is actually correcting — i.e. the board is drifting out the back or
	 *  down into the flats. Zero inside the band, which is why the player's carve is never fought. */
	UPROPERTY(BlueprintReadOnly, Category = "Assist")
	bool bAssistGuardActive = false;

	/** Credit earned in THIS ride, the number the on-screen counter shows (as points).
	 *  specs/ride-score-counter.md FR3 - it is the number with a stake, so a fall resets it. */
	UPROPERTY(BlueprintReadOnly, Category = "Assist")
	float AssistRideCreditSeconds = 0.0f;

	/** Best single ride so far, credit. Loaded through the same FR9 seam as everything else. */
	UPROPERTY(BlueprintReadOnly, Category = "Assist")
	float AssistBestRideSeconds = 0.0f;

	/** FR7 settings control. Wire a UMG option to this. Takes effect on the next ride, so the board
	 *  never changes feel mid-wave. */
	UFUNCTION(BlueprintCallable, Category = "Assist")
	void SetAssistMode(EAssistMode NewMode);

	/** FR7 "Reset assist": back to a first-run state. This is the control to bind while passing the
	 *  phone between testers — it re-arms a fresh first run without force-quitting the app, and once
	 *  credit persists it is the only route back. Applies from the next ride. */
	UFUNCTION(BlueprintCallable, Category = "Assist")
	void ResetAssistCredit();

	/** Console alias for the above, for on-device testing without a UI: type "SurfAssistReset". */
	UFUNCTION(Exec)
	void SurfAssistReset() { ResetAssistCredit(); }



	/** Open the assist panel. Pauses the world; the panel resumes it on close. Bound to the badge
	 *  tap, and to the first-play card. */

	/** Show the first-play explanation, or celebrate a level drop, if either is due. Fired from BOTH
	 *  ways a ride can begin: OnStartScreenStart when the menu was shown, and BeginPlay when it was
	 *  not (a Restart skips it deliberately, which is exactly how the ride after a level drop is
	 *  reached). NOT from Tick: the start screen pauses the world, so a Tick-side watch for the
	 *  screen closing never fires at all. */


	/** How many seconds before controls are enabled to start camera transition (allows smooth transition to complete) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "AutoPilot")
	float CameraTransitionLeadTime = 2.0f;

	// ========== Start Screen ==========

	/** Show the pre-wave Start screen + control tutorial. The world is paused behind the menu
	 *  until the player taps Start, so the autopilot pop-up and control handoff run exactly as
	 *  normal, just deferred. Interactive play only — auto-suppressed during snapshot/replay
	 *  test runs (same surf.autopilots gate as fall detection). See specs/start-screen-tutorial.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen")
	bool bShowStartScreen = true;

	/** Re-open the Start screen + control tutorial OVER the current ride (pauses the world until
	 *  Start, which then resumes that same ride). Not what the ride's ‹ BACK does any more - that is
	 *  ReturnToHub, a reload, so START always means a fresh wave. Kept for Blueprint callers. */
	UFUNCTION(BlueprintCallable, Category = "Start Screen")
	void ShowStartScreen();

	/** True while the Start screen / tutorial overlay is up (world paused). UMG HUDs bind their
	 *  Restart / Replay / Back button visibility to this — show the buttons only when it is false. */
	UPROPERTY(BlueprintReadOnly, Category = "Start Screen")
	bool bStartScreenActive = false;

	/** True while the hub is showing its instruction cards rather than the menu. The resting touch
	 *  controls the hub draws (specs/two-screen-navigation.md FR1a) step aside for the cards, whose
	 *  illustrations sit where the rings go. Written by the overlay's OnInstructionsView hook. */
	bool bStartInstructionsView = false;

	/** True while ANY full-screen overlay owns the display - the Start screen or the board rack.
	 *
	 *  This is what the Restart / Replay / Back buttons should bind their visibility to: show when
	 *  it is false. They used to bind bStartScreenActive directly, which meant they carried on
	 *  drawing straight through the board rack, over the top of the screen the player was choosing
	 *  on. Binding one flag rather than AND-ing two also means the next modal hides them for free
	 *  instead of needing every widget rewired.
	 *
	 *  Kept as a plain property, not a function, so an existing UMG visibility binding can be
	 *  repointed at it without rebuilding the graph. Updated every tick. */
	UPROPERTY(BlueprintReadOnly, Category = "Start Screen")
	bool bRideUIBlocked = false;

	/** Where the menu camera stands, in world cm *relative to the surfboard*, always looking back at
	 *  it. −X is shoreward (the wave breaks toward the beach), +Y is down the line, +Z is up. So the
	 *  default reads as: 20 m in toward the beach, 8 m down the line, 6 m up — a view back at the
	 *  surfer with the wave face behind them. Ride framing is untouched; this runs only on the menu. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen")
	// Z chosen from the -CaptureZSweep contact sheet: low enough that nearer water occludes the far
	// GridLodActor edges, which is what stops the tile seams reading as hard lines.
	FVector StartScreenCameraOffsetCm = FVector(2500.0f, -1000.0f, 140.0f);

	/** How far above the aim point the camera looks, cm. Raising it tips the framing up into the wave
	 *  face; lowering it centres on the water. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen")
	float StartScreenLookAtRiseCm = 150.0f;

	/** Yaw and pitch added to the menu camera's look-at, degrees (+pitch = look up, +yaw = swing
	 *  right). Used to steer the grid-tile seams out of frame: the wave is drawn by a small number
	 *  of GridLodActors and the joins between them are visible as hard edges at glancing angles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen")
	float StartScreenCameraYawOffsetDeg = -32.0f;   // chosen from the -CaptureYawSweep contact sheet

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen")
	float StartScreenCameraPitchOffsetDeg = 0.0f;

	/** Aim at the white-water cluster rather than the surfboard. The break sits well down the line
	 *  from where the board waits, so aiming at the board leaves the foam far off-axis and out of
	 *  shot. Falls back to the board whenever there are no foam points this frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen")
	bool bStartScreenLookAtFoam = true;

	/** Smoothed foam aim point, so the per-frame jump of the cluster centroid isn't camera shake. */
	FVector StartScreenFoamTarget = FVector::ZeroVector;

	/** The white-water controller, found alongside the wave actors. */
	TWeakObjectPtr<class AParticleSystemsController> CachedFoamController;

	// ---- Start-screen background capture (-StartScreenCapture) --------------------------------
	// Throwaway offline path that records one full wave period from a parked camera, so the menu
	// can use a pre-blurred frame loop instead of a live scene. See specs/start-screen-tutorial.md.
	void TickStartScreenCapture(float DeltaTime);
	bool bStartScreenCaptureActive = false;
	bool bCaptureYawSweep = false;
	bool bCaptureZSweep = false;
	bool bCaptureInitialised = false;
	int32 CaptureSettleTicks = 0;
	int32 CaptureIndex = 0;
	int32 CaptureHoldTicks = 0;
	int32 CaptureFirstFrame = 0;
	int32 CaptureLastFrame = 0;
	FTransform CaptureCameraTransform;

	// ---- Kinematic-target velocity probe (-KinematicProbe) -------------------------------------
	// Throwaway diagnostic for specs/deterministic-ride-handoff.md: makes the board kinematic and
	// drives it along a known constant-velocity, constant-yaw-rate path with ETeleportType::None,
	// then reads the velocity back. Confirms Chaos derives V/W from the kinematic target (and that
	// the board's body actually takes the SetKinematicTarget branch, which needs CanSimulate true).
	// Verdict is logged and the run quits; delete this path once the rails driver is validated.
	void TickKinematicProbe(float DeltaTime);
	bool bKinematicProbeActive = false;
	bool bKinematicProbeInitialised = false;
	float KinematicProbeTime = 0.0f;
	int32 KinematicProbeSamples = 0;
	int32 KinematicProbeGoodSamples = 0;      // direction within ~2.5 deg of commanded
	int32 KinematicProbeNonUniformSamples = 0; // linear and angular disagreed on the scale
	float KinematicProbeRatioSum = 0.0f;
	float KinematicProbeRatioMin = TNumericLimits<float>::Max();
	float KinematicProbeRatioMax = 0.0f;
	FVector KinematicProbeOrigin = FVector::ZeroVector;
	FRotator KinematicProbeOriginRot = FRotator::ZeroRotator;

	/** Commanded probe motion: constant world velocity and constant yaw rate. Chosen to be well
	 *  clear of zero on every axis being read back, so a partial derivation is distinguishable
	 *  from a total failure. */
	static constexpr float KinematicProbeSpeed = 500.0f;    // cm/s along +Y (down the line)
	static constexpr float KinematicProbeRise = 120.0f;     // cm/s along +Z, so Z is non-zero too
	static constexpr float KinematicProbeYawRate = 30.0f;   // deg/s
	static constexpr float KinematicProbeDuration = 3.0f;   // seconds before the verdict

	/** Wave frames to skip between captures. The wave runs at 60 frames/s, so a stride of 2 played
	 *  back at 30 fps reproduces its natural speed at half the file count. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start Screen")
	int32 CaptureFrameStride = 2;


	/** Broadcast when the Start screen opens (at BeginPlay or via ShowStartScreen). Hide gameplay UI here. */
	UPROPERTY(BlueprintAssignable, Category = "Start Screen")
	FStartScreenStateChanged OnStartScreenOpened;

	/** Broadcast when Start is pressed and the ride begins. Show gameplay UI here. */
	UPROPERTY(BlueprintAssignable, Category = "Start Screen")
	FStartScreenStateChanged OnStartScreenClosed;

	// ========== Camera Configuration ==========

	/** Editor-only camera preview. Pick a config (Behind Chase / Behind Bird / Beside) to position the
	 *  viewport camera at it without playing (select the pawn to see the camera-preview thumbnail). No runtime effect. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Editor Preview")
	EEditorCameraPreview EditorCameraPreview = EEditorCameraPreview::Off;

	// Camera Position 1: Behind Surfboard (Following Camera)

	/** Which Behind-camera config is active. AutoBlend blends chase<->bird by heading; the Force
	 *  modes lock to one endpoint so you can position/tune it live in the editor while playing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	EBehindCameraConfig BehindCameraConfig = EBehindCameraConfig::AutoBlend;

	/** Chase offset from surfboard origin (local space) for down-the-line trim. -Y = behind, +Z = up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	FVector CameraOffset = FVector(0.0f, 0.0f, 180.0f); // ~Head height in cm

	/** Chase rotation offset (deg) relative to the board-following look direction.
	 *  Pitch < 0 = look down; Yaw offsets the look left/right; Roll tilts the horizon. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	FRotator CameraRotationOffset = FRotator(-30.0f, 0.0f, 0.0f);

	// Camera Position 1b: Bird view, blended in when the board aims down the wave.
	// The Behind camera lerps from the chase config above (CameraOffset/CameraRotationOffset,
	// used when trimming down the line) toward this bird config as the board's horizontal
	// heading aligns with WaveDownhillWorldDir. Driven by a yaw-plane dot product, so
	// pumping (a pitch oscillation) does not move the camera. See specs/camera-bird-view-on-descent.md.

	/** Bird offset (local space) when the board points straight down the wave. -Y = behind, +Z = up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	FVector CameraOffsetBird = FVector(0.0f, -400.0f, 450.0f);

	/** Bird rotation offset (deg) relative to the board-following look direction (look further down). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	FRotator CameraRotationBird = FRotator(-55.0f, 0.0f, 0.0f);

	/** What signal drives the chase->bird switch. Default is wave occlusion: does the wave between the
	 *  chase camera and the board rise above the line of sight and hide the board. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	EBirdTriggerSource BirdTriggerSource = EBirdTriggerSource::WaveOcclusion;

	/** SharedCalculations actor to read the wave from (front or back). Required for all the wave-based
	 *  trigger sources (occlusion / slope / water-above) — its waveVelocity is the AWaveHeight sampled.
	 *  Unused for Heading. Assign in the level. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	TObjectPtr<class ASharedCalculations> SharedCalculationsForCamera;

	/** Number of points sampled along the camera->board line for the Wave-occlusion source (more = finer,
	 *  costlier). Endpoints are skipped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	int32 OcclusionSampleCount = 8;

	/** Height (cm) above the board the occlusion sightline aims at — the rider/sail point we want to keep
	 *  visible. Higher = the wave must rise more to count as hiding the board (less eager triggering). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	float OcclusionTargetHeight = 150.0f;

	/** Virtual occluder height (cm) added on top of the sampled water surface to account for the breaking
	 *  lip / white water that isn't in the height data. 0 = off (water surface only). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	float OcclusionLipHeight = 0.0f;

	/** Crest height above the board (cm) at which the virtual lip reaches full height; it fades in
	 *  (smoothstep) from 0 up to this, so the lip only counts where the wave towers over the rider. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	float OcclusionLipWaterHeight = 200.0f;

	/** Fixed world-space "down the wave" (fall-line) direction for the Heading source. Need not be
	 *  normalized; horizontal component is used. Ignored by the slope/water sources. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	FVector WaveDownhillWorldDir = FVector(1.0f, 0.0f, 0.0f);

	/** Hysteresis LOW threshold (in the selected source's units): once in bird view, drop back to chase
	 *  only when the trigger value falls below this. Keep it under BirdTriggerHigh to leave a deadband.
	 *  Default is in cm for the Wave-occlusion source; re-tune from the BirdCam log for other sources. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	float BirdTriggerLow = 20.0f;

	/** Hysteresis HIGH threshold (in the selected source's units): commit to bird view once the trigger
	 *  rises above this. The gap to BirdTriggerLow is the deadband that stops jitter flipping the camera.
	 *  Default is cm of crest above the line of sight for the Wave-occlusion source. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	float BirdTriggerHigh = 50.0f;

	/** How fast the camera eases between chase and bird once the hysteresis latch flips (higher = snappier). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	float BirdBlendSpeed = 3.0f;

	/** Low-pass speed for the trigger signal before the hysteresis test (lower = smoother, rejects more
	 *  white-water spikes but reacts slower). Damps transient jumps that the deadband alone can't. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	float BirdTriggerSmoothingSpeed = 2.0f;

	/** Log a per-tick "BirdCam:" line (trigger value, Low/High thresholds, latch, blend) to GoneSurfing.log
	 *  for tuning the thresholds — captured by headless/trace-replay runs unlike on-screen text. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 1 - Behind")
	bool bDebugBirdTrigger = false;

	// Camera Position 2: Beside Surfboard (Fixed Direction Camera).
	// Unlike Behind, this is world-fixed: the offset is added in world space and the rotation is
	// an absolute world orientation, so the view does not rotate with the board. Tuned the same
	// way as Behind — an offset vector plus a full rotation (pitch/yaw/roll).

	/** Camera offset from surfboard origin for Position 2 - Beside (WORLD space). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 2 - Beside")
	FVector CameraOffsetSide = FVector(0.0f, -700.0f, 180.0f);

	/** Absolute world rotation for Position 2 - Beside (Pitch/Yaw/Roll, degrees). Pitch < 0 = look down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Position 2 - Beside")
	FRotator CameraRotationSide = FRotator(-30.0f, 0.0f, 0.0f);

	// Smoothing Configuration

	/** Interpolation speed for yaw smoothing (higher = faster) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Smoothing")
	float YawSmoothingSpeed = 100.0f;

	/** Interpolation speed for position smoothing (higher = faster, 0 = no smoothing) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Smoothing")
	float PositionSmoothingSpeed = 5.0f;

	/** Seconds to blend BOTH position and rotation together when switching camera modes
	 *  (Beside <-> Behind). Avoids the rotation-snaps-while-position-slides teleport. 0 = instant. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera|Smoothing")
	float CameraTransitionDuration = 0.5f;

	// ========== Wave Radar ==========
	// Top-down schematic HUD for rear/peripheral break awareness. Samples wave heights/normals around
	// the board (no scene render) and draws them. See specs/wave-radar.md.

	/** Run the on-device sensor capture wizard instead of relying on log archaeology.
	 *  Walks through a scripted list of poses/motions, shows live sensor readouts, and
	 *  writes every motion value to Saved/SensorProbe/probe-<stamp>.csv.
	 *
	 *  Off: the 2026-08-20 capture answered the question it was built for (the blend weight
	 *  was inverted — see specs/tilt-yaw-fusion.md). Flip on again if the tilt basis ever
	 *  needs re-measuring; the wizard dismisses itself once its steps are done. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tuning")
	bool bRunSensorProbe = false;

	/** Master on/off for the runtime-tuning HUD (the TUNE gear button + panel, and the SEND
	 *  trace-share button that shares its strip). Off = not installed, so neither button exists.
	 *
	 *  Left ON: both are wanted on every development build, on device, right up to the store
	 *  build — TUNE is the only way to change a coefficient mid-ride on a phone and SEND is the
	 *  only way to get a trace off it. What keeps them out of the players' hands is the BUILD, not
	 *  this flag: see IsTuningUIAvailable(), which is a compile-time false in Shipping. So this
	 *  stays true and no release-day flip is owed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tuning")
	bool bShowTuningHUD = true;

	/** Whether the TUNE/SEND dev UI may exist at all: the flag AND the build.
	 *
	 *  Shipping folds this to a constant `false`, so the widgets are never installed however
	 *  bShowTuningHUD is set — by a level, a Blueprint default or a hand that forgot. That is what
	 *  makes "the store build ships no tuning UI and no trace upload" a property of the build
	 *  instead of a checklist item somebody has to remember, and it is what the Play Data-safety
	 *  answer of "collects nothing" rests on: SEND is the game's only outbound request.
	 *
	 *  Every decision that asks "is the dev UI here?" must go through this, not the raw flag —
	 *  including the input mode, because a Shipping build that took the GameAndUI branch for a
	 *  panel it never installed would be a behaviour difference with no UI to justify it. */
	bool IsTuningUIAvailable() const
	{
#if UE_BUILD_SHIPPING
		return false;
#else
		return bShowTuningHUD;
#endif
	}

	/** Master on/off for the wave-radar HUD. Off = no sampling, no widget, no cost.
	 *  Also gated by the surf.hud.radar CVar, which is 0 (off) by default — see WantsWaveRadar(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	bool bShowWaveRadar = true;

	/** On-screen size of the radar (logical px; doubled on Android via the DPI scaler). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	float RadarSizePx = 200.0f;

	/** Surfboard icon length in world cm — drawn to the radar's real scale (so it's not oversized). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	float RadarBoardLengthCm = 250.0f;

	/** Surfboard icon width in world cm. Bump up for visibility if the icon reads too thin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	float RadarBoardWidthCm = 70.0f;

	/** Half-extent (cm) of the wave region sampled around the surfer in each direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	float RadarExtentCm = 1500.0f;

	/** Samples per side of the radar grid (N x N total). Clamped to [3,41]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	int32 RadarGridN = 11;

	/** Data refresh rate (Hz); the widget repaints every frame regardless. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	float RadarUpdateHz = 15.0f;

	/** White-water source: when set, radar break cells are the ACTUAL white-water particle positions
	 *  (binned into cells), not the slope estimate. Leave unset to fall back to BreakSlopeThreshold. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	TObjectPtr<class AParticleSystemsController> ParticleControllerForRadar;

	/** Fallback only (used when ParticleControllerForRadar is unset): slopeSin above which a cell is
	 *  drawn as breaking. 0 flat .. ~1 vertical. The particle source above is preferred when available. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	float BreakSlopeThreshold = 0.6f;

	/** White-water particles per cell that map to full foam opacity (higher = denser foam needed). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	float RadarFoamSaturationCount = 3.0f;

	/** How fast foam builds up where white water appears (higher = snappier). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	float RadarFoamRiseSpeed = 12.0f;

	/** How fast foam fades where white water leaves (lower = lingers/merges more smoothly). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	float RadarFoamFallSpeed = 3.0f;

	/** If true, the radar rotates with the board (board fixed pointing up). If false (default), the radar
	 *  is world-fixed and the surfer triangle rotates instead — steadier to read. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	bool bRadarHeadingUp = false;

	/** World-fixed mode only: world yaw (deg) mapped to "up" on the radar. Tune to orient the wave —
	 *  e.g. crest vertical / board pointing right. Each +90 rotates the radar a quarter turn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Wave Radar")
	float RadarWorldYawOffset = 90.0f;

	// ========== Paddling Configuration ==========

	/** Force strength applied when paddling (Newtons) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paddling")
	float PaddleForceStrength = 10000.0f;

	/** Show debug visualization for paddle force */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paddling")
	bool bDebugPaddleForce = false;

	// ========== Turn Configuration ==========

	/** Turn force strength (Newtons) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Turn")
	float TurnForceStrength = 10000.0f;

	/** Forward offset for turn force application point (in surfboard local space, positive = toward nose) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Turn")
	float TurnForceForwardOffset = 100.0f;

	/** Android left stick X threshold for turn activation (|X| > threshold triggers turn) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Turn")
	float AndroidTurnThreshold = 0.5f;

	// ========== Weight Shift Configuration ==========

	/** Time in seconds for weight to interpolate back to center after release */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight")
	float WeightAutoCenterDuration = 0.3f;

	/** Ceiling on the player's commanded forward weight (amountInFront). Keeps a nose-heavy trim,
	 *  where the board no longer answers the roll axis, out of reach. Fallback only - effective
	 *  value comes from USurfTuningSubsystem via GetEffectiveWeightMaxInFront(); kept in sync with
	 *  that default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight")
	float WeightMaxInFront = 0.6f;

	/** Mouse sensitivity for PC weight control - sideways (left/right) (pixels of mouse travel per full deflection) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight")
	float MouseWeightSensitivityX = 400.0f;

	/** Mouse sensitivity for PC weight control - front/back (pixels of mouse travel per full deflection) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight")
	float MouseWeightSensitivityY = 200.0f;

	/** Android right stick deadzone for weight input */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight")
	float AndroidWeightDeadzone = 0.1f;

	// ---- Android tilt weight (Phase 6) ----

	/** Which input drives weight on Android: device tilt (default) or right stick (fallback for A/B) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight|Tilt")
	EAndroidWeightInputSource AndroidWeightInputSource = EAndroidWeightInputSource::Tilt;

	/** Phone tilt angle in degrees that produces full forward/back weight deflection.
	 *  Fallback only — effective value comes from USurfTuningSubsystem via
	 *  GetEffectiveTiltPitchForFull(); kept in sync with that default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight|Tilt")
	float TiltPitchDegreesForFullDeflection = 25.0f;

	/** Phone tilt angle in degrees that produces full left/right weight deflection.
	 *  Fallback only — see GetEffectiveTiltRollForFull(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight|Tilt")
	float TiltRollDegreesForFullDeflection = 25.0f;

	/** Angular deadzone (degrees) around neutral — suppresses sensor jitter */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight|Tilt")
	float TiltAngleDeadzoneDegrees = 2.0f;

	/** Invert pitch axis (tilt-forward → weight back instead of forward) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight|Tilt")
	bool bInvertTiltPitch = true;

	/** Invert roll axis (tilt-right → weight left instead of right) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight|Tilt")
	bool bInvertTiltRoll = false;

	/** Degrees of sideways lean per degree of phone yaw, for the gyro fusion that covers
	 *  gravity's blind axis near-vertical. Magnitude only; 0 disables. Fallback only —
	 *  effective value comes from USurfTuningSubsystem via GetEffectiveTiltYawGain().
	 *  See specs/tilt-yaw-fusion.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight|Tilt")
	float TiltYawGain = 1.0f;

	/** Reverses the yaw direction when >= 0.5. Fallback only — see GetEffectiveTiltYawSign(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight|Tilt")
	float TiltYawInvert = 0.0f;

	/** Leak time constant for the yaw integrator. Fallback only — see GetEffectiveTiltYawLeakSeconds(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight|Tilt")
	float TiltYawLeakSeconds = 2.0f;

	/** Gyro bias-averaging window after calibration. Fallback only — see GetEffectiveTiltYawBiasSampleSeconds(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight|Tilt")
	float TiltYawBiasSampleSeconds = 1.0f;

	/** Verbose per-tick tilt log (also enabled via surf.debug.flags tilt). Off: it logs
	 *  every tick, and its diagnostic job is done — the axis questions it was turned on for
	 *  are answered in specs/tilt-yaw-fusion.md. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Weight|Tilt")
	bool bDebugTilt = false;

	// ========== Input Trace Recording ==========

	/** Enable per-tick CSV recording of player weight input under Saved/InputTraces/.
	 *  See specs/input-trace-replay.md. Default off; flip on in playable test levels
	 *  where you want to capture phone input for PC-side replay tuning. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InputTrace")
	bool bRecordInputTrace = false;

	/** Optional prefix prepended to the trace filename. Final name is
	 *  "<prefix>-<yyyy-mm-dd-hh-mm-ss>.csv" (or "<timestamp>.csv" when empty).
	 *  The timestamp suffix is always added, so every level start — including
	 *  every Restart Level — writes a fresh file. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InputTrace")
	FString InputTraceSessionPrefix;

	/** Safety cap: recording stops + flushes after this many seconds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "InputTrace")
	float MaxRecordingSeconds = 120.0f;

	// ========== Fall (the rider goes off the board) and the lost-wave stall ==========
	// Two self-inflicted ride endings, both taking the rider off the deck: roll past
	// fallRollThresholdDeg is a WIPEOUT, planing wallowing below fallPlaningStopThreshold is the
	// board LOSING THE WAVE. The card names which. Either way control input is cut until Replay or
	// Restart. Both triggers stay disarmed until the ride is
	// actually underway (controls live + AmountPlaning past fallPlaningArmThreshold), so
	// paddling and the pop-up autopilot's attitude excursions can't trip them.
	//
	// The fall itself is an AUTHORED clip (SK_Falling_left / _right, 2026-09-22) played by
	// USurferAnimInstance, on a rider detached from the board and moved kinematically: it keeps the
	// board's momentum, which relaxes into the water's own velocity, levels to upright, and rides
	// the water surface. The physics ragdoll that preceded it stays as the fallback when the clips
	// are not assigned, and behind bFallUseRagdoll for comparison. See specs/surfer-fall-ragdoll.md.

	/** Master switch for fall detection. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	bool bFallEnabled = true;

	/** True = the physics ragdoll (the pre-2026-09-22 fall) even when the fall clips are assigned.
	 *  For A/B; the ragdoll is also what plays when either clip is missing from the AnimBP. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	bool bFallUseRagdoll = false;

	/** Authored fall only: time constant (s) over which the fallen rider's horizontal velocity relaxes
	 *  from the board's (x fallVelocityInherit) to the water's. A body in the water is carried by
	 *  it; this is how quickly. 0 = follows the water from the first tick. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallMomentumDecaySeconds = 0.6f;

	/** Authored fall only: fraction of the water's horizontal velocity the fallen rider drifts
	 *  with once the momentum has decayed (1 = carried like foam, 0 = stays put). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallWaterDrift = 1.0f;

	/** Authored fall only: seconds over which the detached rider levels from the deck's tilt to
	 *  upright, so the clip plays in an upright frame however the board was pitched or rolled at
	 *  the moment of the fall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallLevelSeconds = 0.3f;

	/** Authored fall only: once the clip has ended, the rider sinks out of sight at this rate
	 *  (cm/s) until fallSinkDepth below where the clip left him. Owner (2026-09-22): a rider held
	 *  on the surface reads as SITTING on the water (the board rides ~50 cm above the rendered
	 *  wave, so "on the surface" is above the mesh); dropping him under it is the honest ending.
	 *  0 = never sinks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallSinkRate = 80.0f;

	/** Authored fall only: how far (cm) the rider sinks after the clip, then holds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallSinkDepth = 200.0f;

	/** Authored fall only: seconds after the clip ends before the sink starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallSinkDelaySeconds = 0.0f;

	/** |board world roll| (deg) beyond this = fall. Computed geometrically from the board
	 *  transform (board.left = local -X), same convention as the rider's roll compensation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallRollThresholdDeg = 90.0f;

	/** AmountPlaning below this (while armed) = the board has wallowed to a stop = fall.
	 *  Planing's own hysteresis + decay (~2s) keeps transient dips from firing this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallPlaningStopThreshold = 0.05f;

	/** AmountPlaning must first exceed this (with player controls enabled) to arm both triggers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallPlaningArmThreshold = 0.5f;

	/** Fraction of the board's velocity the rider keeps at detach (1 = flung, 0 = dropped). Both
	 *  falls: the ragdoll's bodies and the authored fall's kinematic start velocity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallVelocityInherit = 1.0f;

	/** RAGDOLL ONLY. UNIFORM horizontal shove (cm/s) toward the fall side, added identically to every body on
	 *  top of the topple. Whole-body translation — keep small (or 0) or the fall reads as a
	 *  sideways hop. The topple rotation (fallToppleRate) is what makes it read as falling. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallSidewaysKick = 50.0f;

	/** RAGDOLL ONLY. UNIFORM upward kick (cm/s), added identically to every body. Same hop caveat — the
	 *  topple keeps the feet on the deck naturally, so this defaults to off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallUpwardKick = 0.0f;

	/** RAGDOLL ONLY. Topple rate (deg/s) about the feet toward the fall side (the low rail when rolled, else
	 *  the weighted rail). Applied as a rigid rotation: per-body v = ω × (p − feet), so the head
	 *  gets the highest velocity and the feet ~none. 0 = off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fall")
	float fallToppleRate = 150.0f;

	/** The surfer's authored deck height, captured the first time a board is applied. A board's
	 *  SurferDeckOffsetZ is always measured from THIS, never from wherever the last board left the
	 *  rider — otherwise switching boards repeatedly would walk the surfer off the deck. */
	float SurferDeckBaseZ = 0.0f;
	bool bSurferDeckBaseCaptured = false;

	/** -BoardPicker fires once per SESSION, not per pawn - see GSurfBoardPickerAutoOpened. */

	/** Seconds waited so far for -BoardPickerDelay. */
	float BoardPickerDelayElapsed = 0.0f;

	/** Push the active board's tuning, assist level and appearance. No-ops in scripted runs and when
	 *  no profile is installed. Idempotent — the tuning subsystem survives the level reload a restart
	 *  performs, so this always resets to baseline before setting. */
	void ApplyActiveBoard();

	/** True from the ride's end (either kind) until ReplayLastRide/RestartLevel clears it. Terminal
	 *  for the ride: no mid-ride recovery in v1. Read by UI to show a ride-over state. The NAME is
	 *  historical - it was set only by the wipeout - but every reader means "the ride is
	 *  over", which is why a stall sets it too; RideEndKind says which ending it was. */
	UPROPERTY(BlueprintReadOnly, Category = "Fall")
	bool bFallen = false;

	/** The two ways a ride ends on its own. A wipeout is something specific - the rider goes off
	 *  the board - and is a different thing from the board losing the wave and wallowing to a
	 *  stop; the card names which. Both take the rider off the board: a stall that left the rider
	 *  standing was tried (2026-09-14) and did not read as an ending. */
	enum class ERideEndKind : uint8
	{
		Wipeout,   // roll past fallRollThresholdDeg (or SurfFall)
		LostWave,  // AmountPlaning wallowed below fallPlaningStopThreshold
	};

	/** Which ending bFallen records. Meaningless while bFallen is false. */
	ERideEndKind RideEndKind = ERideEndKind::Wipeout;

	/** Take the rider off the board, cut player input (weight re-centered), and stop the
	 *  ride recording. The board's physics stays live so it washes out realistically. */
	UFUNCTION(BlueprintCallable, Category = "Fall")
	void TriggerFall(const FString& Reason);

	/** The ending: mark the ride over, bank the score, take the rider off the board (authored
	 *  clip, or the ragdoll), cut input, close the trace, commit the record. Kind only decides
	 *  the card's title. */
	void EndRide(ERideEndKind Kind, const FString& Reason);

	/** Console alias to force a wipeout from the ` console in PIE / -game. Type "SurfFall". */
	UFUNCTION(Exec)
	void SurfFall() { TriggerFall(TEXT("console")); }

	/** Hold the pump button for `HoldSeconds`, starting `DelaySeconds` from now - a scripted thumb.
	 *
	 *  There was no way to press the pump without a human hand, which is most of why the release
	 *  glitch fixed on 2026-09-22 survived so long: the button is a Slate pad, and in-ride the
	 *  viewport holds mouse capture, so synthetic clicks never reach it; the space bar needs
	 *  keyboard focus that an injected key does not get; and `-ReplayTrace` feeds the autopilot's
	 *  weights, not the pump. This is the way in. Delay exists because `-ExecCmds` fires at level
	 *  load, long before the handoff: `-ExecCmds="SurfPump 0.7 40"` pumps 40 s in, headless.
	 *
	 *  For a headless CAPTURE prefer `surf.pump.auto`: a pawn set up by -ExecCmds at level load is
	 *  destroyed by the reload into the ride, taking this state with it (measured 2026-09-22 - the
	 *  command logs, then nothing pumps). A CVar survives that; this exec is for a live console.
	 *
	 *  Inert unless called, so it cannot touch a baseline. */
	UFUNCTION(Exec)
	void SurfPump(float HoldSeconds = 0.7f, float DelaySeconds = 0.0f);

	// ========== Stamina (specs/stamina.md) ==========
	// One pool per ride, drained by riding, pumping and hard turns, refilled by resting. Empty =
	// TIRED: the assist runs REVERSED (steers away from the pocket, bounded, additive), the tired
	// tuning layer goes on, the rider's anim flag goes up - a state the physics resolves (lose the
	// wave, fall, or rest and recover), not an ending in itself. Armed with the
	// fall triggers (bFallArmed) so the paddle and the pop-up cost nothing, gated out of every
	// automated run the same way, and writing nothing into the physics itself.

	Stamina::FState StaminaState;

	/** Per tick after UpdateFallDetection: drain/refill, flip the tired state, drive the bar. */
	void UpdateStamina(float DeltaTime);

	/** Enter or leave tired: the tuning layer, the anim flag, the log. Idempotent. Also the one
	 *  place that guarantees the layer is LIFTED when a ride ends or the pawn goes away - the
	 *  tuning subsystem outlives the level reload, so a layer left on would tire the next ride. */
	void SetTired(bool bOn);

	/** The pool says tired, or the StaminaForceTired dev switch does. Everything that reacts to
	 *  tired (assist alpha, the layer, the anim flag) reads this, never StaminaState.bTired alone. */
	bool IsRiderTired() const;

	/** Tunables from USurfTuningSubsystem, with the compiled defaults as fallback. */
	Stamina::FTuning GetStaminaTuning() const;

	/** Capture aids (spec FR8). "SurfStamina 0.15" writes the pool - the low state for a
	 *  screenshot; "SurfSpend" empties it, so the ending and its card can be looked at on demand
	 *  instead of waiting a ride out. */
	UFUNCTION(Exec)
	void SurfStamina(float Fraction);
	UFUNCTION(Exec)
	void SurfSpend() { SurfStamina(0.0f); }

	// ========== Replay Hook ==========

	/** Set true by AInputReplayAutoPilot while it is driving CurrentWeightOffset.
	 *  While true, the pawn's tilt poll and auto-center are skipped — the replay
	 *  autopilot writes CurrentWeightOffset directly each tick, and the existing
	 *  Tick() propagation to WeightDistribution carries it the rest of the way.
	 *  Dev-only; production levels never have a replay autopilot active. */
	UPROPERTY(BlueprintReadWrite, Category = "Replay")
	bool bExternalWeightOverride = false;

	/** Replay autopilot uses this to set CurrentWeightOffset without exposing
	 *  the private field. No clamp here — caller is the replay autopilot. */
	void SetCurrentWeightOffsetFromReplay(FVector2D Offset) { CurrentWeightOffset = Offset; }

	/** True once the intro autopilot has handed off and player input is live.
	 *  Replay autopilot reads this to align trace t=0 with the same moment. */
	bool IsPlayerControlsEnabled() const { return bPlayerControlsEnabled; }

	/** Effective phone-tilt-for-full-deflection (degrees), pitch/roll. Reads the live-tunable
	 *  USurfTuningSubsystem value when available (so it tracks the HUD and isn't stuck at a
	 *  stale per-instance override), else falls back to the pawn UPROPERTY. Used by the tilt
	 *  input path, the trace recorder header, and the replay autopilot. */
	float GetEffectiveTiltPitchForFull() const;
	/** Ceiling applied to the player's commanded amountInFront. Live-tunable; see
	 *  USurfTuningSubsystem::WeightMaxInFront. */
	float GetEffectiveWeightMaxInFront() const;

	/** Fore/aft weight readout - on screen, and to the log at 5 Hz - while
	 *  `surf.debug.flags weight` is set. */
	void DrawWeightReadout() const;

	/** World time of the last weight readout log line, for the 5 Hz throttle. */
	mutable double LastWeightReadoutLogTime = -1.0;

	/** Raw mouse travel (pixels, per axis, absolute) since the last readout log line. */
	mutable FVector2D WeightMouseTravelSinceLog = FVector2D::ZeroVector;

	/** True when the radar should be up: the pawn flag AND the surf.hud.radar CVar (default off). */
	bool WantsWaveRadar() const;

	/** Whether the radar widget is currently installed, for live CVar toggling. */
	bool bWaveRadarInstalled = false;

	/** Pixels of mouse travel per full deflection, after the surf.input.mouseweight.sens* console
	 *  overrides. bForeAft picks the Y (front/back) axis. PC weight input only. */
	float GetEffectiveMouseWeightSensitivity(bool bForeAft) const;
	float GetEffectiveTiltRollForFull() const;
	float GetEffectiveTiltYawGain() const;
	/** +1 or -1, from the TiltYawInvert knob. */
	float GetEffectiveTiltYawSign() const;
	float GetEffectiveTiltYawLeakSeconds() const;
	float GetEffectiveTiltYawBiasSampleSeconds() const;

	// ========== Camera Mode ==========

	/** If true, use Position 1 (Behind). If false, use Position 2 (Beside).
	 *  The game stays on Beside for the whole ride: nothing switches this at runtime any more
	 *  (the autopilot handoff used to flip it to Behind, and ToggleCameraMode is now a no-op).
	 *  Flip it here or via SetCameraBehind only to bring the Behind camera back deliberately. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	bool bUseBehindCamera = false;

	/** Simulate forward input (W key) for testing in simulation mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paddling|Debug")
	bool bSimulateForwardInput = false;

	/** Simulate left input (A key) for testing in simulation mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Paddling|Debug")
	bool bSimulateLeftInput = false;

	// ========== Debug Logging ==========

	/** Enable camera transform debug logging */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|Logging")
	bool bEnableCameraLogging = false;

	// ========== Enhanced Input ==========

	/** Input Mapping Context */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputMappingContext> InputMappingContext;

	/** Paddle Forward Input Action (analog 0..1) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> IA_PaddleForward;

	/** Turn Input Action (float -1..+1) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> IA_Turn;

	/** Weight Shift Input Action (Vector2D) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> IA_Weight;

	/** Weight Gate Input Action (bool, PC only - RMB hold) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> IA_WeightGate;

	/** Restart Input Action (R key for PC testing) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> RestartAction;

	/** Toggle Camera Input Action (T key for PC testing) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> ToggleCameraAction;

	/** Replay Input Action — triggers ReplayLastRide(). Assign IA_Replay and map a key
	 *  in IMC_SurfboardControls. See specs/on-device-ride-replay.md. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	TObjectPtr<UInputAction> ReplayAction;

protected:
	// ========== Input Handlers ==========

	/** Handle paddle forward input (apply forward force) */
	void PaddleForward(const FInputActionValue& Value);

	/** Handle turn input (apply sideways force at forward offset) */
	void Turn(const FInputActionValue& Value);

	/** Handle weight shift input */
	void WeightInput(const FInputActionValue& Value);

	/** Handle weight gate started (PC RMB pressed) */
	void WeightGateStarted(const FInputActionValue& Value);

	/** Handle weight gate completed (PC RMB released) */
	void WeightGateCompleted(const FInputActionValue& Value);

public:
	// ========== Level Control ==========

	// ========== Board selection (specs/board-selection.md) ==========

	/** How many boards loaded. 0 means no profiles are installed and the game runs exactly as it did
	 *  before the feature existed — bind the bar's visibility to this. */
	UFUNCTION(BlueprintCallable, Category = "Board")
	int32 GetBoardCount() const;

	UFUNCTION(BlueprintCallable, Category = "Board")
	int32 GetActiveBoardIndex() const;

	UFUNCTION(BlueprintCallable, Category = "Board")
	FString GetBoardName(int32 Index) const;

	/** Id of the board being ridden, which is what a per-board best score is filed under. Empty when
	 *  no profiles are installed, which keeps the pre-boards global record. */
	UFUNCTION(BlueprintPure, Category = "Board")
	FString GetActiveBoardId() const;

	/** Display name of the board being ridden, for a HUD button's label. Empty when no profiles are
	 *  installed, which is the signal to hide the control entirely. */
	UFUNCTION(BlueprintPure, Category = "Board")
	FString GetActiveBoardName() const;

	/** Pick a board. Applies immediately between waves - so the board under the surfer has already
	 *  changed by the time the rack closes, which is most of what makes the choice feel real - and
	 *  defers to the next wave mid-ride, because swapping tuning under a planing board is a
	 *  discontinuity with no honest physical reading. Selection persists either way. */
	UFUNCTION(BlueprintCallable, Category = "Board")
	void SelectBoard(int32 Index);

	/** Switch to a board AND start a fresh wave on it. Always restarts, whether or not a ride is in
	 *  progress - so a control labelled "restart with X" is telling the truth in both cases. The
	 *  reload re-applies the profile at BeginPlay, so tuning, mesh, scale and assist arrive together
	 *  on a board that is genuinely new, rather than morphing under the surfer. */
	UFUNCTION(BlueprintCallable, Category = "Board")
	void RestartWithBoard(int32 Index);

	/** Open the board rack. What the board button does, and bindable from UMG if wanted. */
	UFUNCTION(BlueprintCallable, Category = "Board")
	void OpenBoardPicker();

	/** Open the About screen (credits, licences, privacy) over the hub. See specs/about-screen.md. */
	UFUNCTION(BlueprintCallable, Category = "UI")
	void OpenAbout();

	/** Straight into a fresh wave: reload the level with the hub skipped. What SURF AGAIN on the
	 *  wipeout card does. */
	UFUNCTION(BlueprintCallable, Category = "Level")
	void RestartLevel();

	/** End whatever is on screen - a ride, a replay, a wipeout - and return to the hub: reload the
	 *  level WITH the start screen. What the ride's ‹ BACK does (specs/two-screen-navigation.md
	 *  FR3). Not ShowStartScreen, which pauses the live ride under the menu and lets START resume
	 *  the abandoned wave; in the two-screen structure START must always mean a fresh one. The
	 *  ride is filed on the way out, in EndPlay, so it still lists as LAST RIDE. */
	UFUNCTION(BlueprintCallable, Category = "Level")
	void ReturnToHub();

	/** Kinematically replay the player's most recent recorded ride (for the Replay UI
	 *  button, beside Restart). Finalizes the in-progress trace, then plays back the
	 *  newest CSV in Saved/InputTraces/ by driving the board transform directly (physics
	 *  off) with the wave clock re-synced. Re-entrant: tapping again restarts playback of
	 *  the same latest ride from the top. See specs/on-device-ride-replay.md. */
	UFUNCTION(BlueprintCallable, Category = "Level")
	void ReplayLastRide();

	/** Console alias for ReplayLastRide() so the replay can be triggered from the ` console
	 *  in PIE / -game before the UI button is wired up. Type "ReplaySurf". */
	UFUNCTION(Exec)
	void ReplaySurf() { ReplayLastRide(); }

	/** THE Replay control (best-ride-replay.md D10/FR6). Point the UMG Replay button here: it opens
	 *  the ride list - last ride first, then each board's best - instead of playing the last ride
	 *  immediately. `ReplayLastRide` stays behind it for the `IA_Replay` key and the console alias,
	 *  and is what the list itself calls for the "last ride" row.
	 *
	 *  No-op when there is nothing to watch, so a Blueprint may call it unconditionally - but the
	 *  button should bind its own Visibility to HasWatchableRides() so it is never a dead control. */
	UFUNCTION(BlueprintCallable, Category = "Replay")
	void OpenRideList();

	/** False when no stored ride can be played. The Replay button binds Visibility to this (FR6:
	 *  never offer a dead control). */
	UFUNCTION(BlueprintPure, Category = "Replay")
	bool HasWatchableRides() const;

	/** D7: leave a replay in place. Restores the world exactly as the replay found it - board
	 *  transform and velocity, wave clock, camera, controls - gives the player their own board back
	 *  after D1's swap (FR5), and puts the ride list back up on the row they just watched (D5).
	 *
	 *  Point the UMG Back button here. It has to be the UMG bar rather than a Slate control drawn
	 *  over the replay: while a replay is on screen the bar is live, and two live UI layers means
	 *  one of them silently stops responding (NFR3).
	 *
	 *  Replay used to be terminal - only a level reload left it - which was tolerable for one
	 *  replay and is not for a list you come back to. */
	UFUNCTION(BlueprintCallable, Category = "Replay")
	void LeaveReplay();

	/** True while a kinematic replay is active (playing back, or holding on the last
	 *  frame). Cleared only by a level reload (Restart). */
	UPROPERTY(BlueprintReadOnly, Category = "Replay")
	bool bReplayActive = false;

	/** No-op: the ride stays on Camera Position 2 (Beside) all the time. Kept so the
	 *  ToggleCameraAction binding and any Blueprint callers stay valid. */
	UFUNCTION(BlueprintCallable, Category = "Camera")
	void ToggleCameraMode();

	/** Set camera position directly */
	UFUNCTION(BlueprintCallable, Category = "Camera")
	void SetCameraBehind(bool bBehind);

private:
	/** Previous frame's camera position state (to detect mode changes) */
	bool bWasBehindCamera = false;

	/** False until the first camera Tick has snapped CameraRoot to its target — prevents the spawn-location-to-target lerp on play start */
	bool bCameraInitialized = false;

	/** Latched chase/bird state for the Behind camera (Schmitt trigger over the smoothed trigger). */
	bool bBirdViewLatched = false;
	/** Low-passed trigger value the hysteresis actually tests (rejects white-water spikes). */
	float SmoothedBirdTrigger = 0.0f;
	/** Eased 0..1 chase->bird blend (FInterpTo'd toward the latched target so the move is smooth). */
	float BirdViewBlend = 0.0f;

	/** Seconds left in the current mode-transition blend (Beside <-> Behind). 0 = not transitioning. */
	float ModeTransitionTimeRemaining = 0.0f;
	/** Camera state captured at the mode switch; the blend eases from here to the live target.
	 *  Position is stored board-RELATIVE (offset) so the blend tracks the moving board, not a stale world point. */
	FVector ModeTransitionStartOffset = FVector::ZeroVector;
	FRotator ModeTransitionStartRotation = FRotator::ZeroRotator;

	/** Update camera position and rotation to follow surfboard with stabilization */
	void UpdateCameraTransform(float DeltaTime);

	/** Time accumulator throttling the wave-radar sampling to RadarUpdateHz. */
	float RadarUpdateAccumulator = 0.0f;

	/** Persistent per-cell foam intensity (0..1), temporally smoothed across radar updates. */
	TArray<float> RadarFoamIntensity;

	/** Sample the wave grid around the board and push a snapshot to the radar HUD (throttled). */
	void UpdateWaveRadar(float DeltaTime);

	/** Push the current playback state (progress / timer / end-hold) to the replay overlay HUD.
	 *  Called each replay tick in place of the wave radar. See specs/replay-mode-clarity.md. */
	void UpdateReplayOverlay();

	/** Quick fade-in from black to mark the LIVE->REPLAY transition (reuses the camera-manager
	 *  fade the Restart flow uses). Called on replay enter and on re-tap ("watch again"). */
	void PlayReplayTransitionFade();

	// ========== Weight Shift State ==========

	/** Current weight offset in [-1, +1] per axis (0 = centered) */
	FVector2D CurrentWeightOffset = FVector2D::ZeroVector;

	/** True while Android right stick is past deadzone or PC RMB is held */
	bool bWeightInputActive = false;

	/** Cursor position before RMB lock (PC only) */
	FVector2D CursorPositionBeforeLock = FVector2D::ZeroVector;

	/** Device-frame axes captured at handoff — the neutral pose live gravity is measured against.
	 *  Built by SurfTilt::Calibrate; the tutorial keeps a separate preview basis of its own. */
	SurfTilt::FBasis TiltBasis;

	/** Gyro yaw fusion state (specs/tilt-yaw-fusion.md). Reset at calibration. */
	SurfTilt::FYawState TiltYaw;

	/** Raw gyro rate about the gravity axis (rad/s), cached for the tilt debug log. Splits
	 *  "sensor delivers nothing" (stays ~0 while the phone is rotating) from "integrator is
	 *  gated" (non-zero here, but TiltYaw.AngleDeg stays 0). */
	float LatestRawYawRate = 0.0f;

	/** Latest fused/gravity roll split and the gyro's current authority, cached from the last
	 *  SurfTilt::ComputeWeight purely for the debug log. */
	float LatestGravityRollDeg = 0.0f;
	float LatestYawWeight = 0.0f;

	/** Deflection knobs gathered from USurfTuningSubsystem (or the editable fallbacks). */
	SurfTilt::FTuning MakeTiltTuning() const;

	/** Apply the normal gameplay input mode. GameAndUI whenever a tappable viewport Slate widget
	 *  is present — the assist badge, the sensor probe, or the dev UI when the build has it
	 *  (IsTuningUIAvailable) — because GameOnly's viewport capture stops viewport Slate receiving
	 *  taps and leaves such a widget visible but dead. GameOnly otherwise. Cursor stays hidden
	 *  either way. Safe with a null PC. */
	void ApplyGameplayInputMode(APlayerController* PC) const;

	/** Read live gravity, run it through SurfTilt::ComputeWeight against TiltBasis, and write the
	 *  result to CurrentWeightOffset. The mapping itself lives in SurfTilt.h so the start-screen
	 *  tutorial can drive its feedback board from the same maths. */
	void UpdateTiltWeight();

	/** Read live acceleration, project onto gravity, low-pass, deadzone, clamp to
	 *  loading-only [0,1], and write to PumpInput. Mirrors the tilt-weight
	 *  pipeline. Android-only; PC pumping is deferred. See specs/pumping.md. */
	void UpdatePumpInput();

	/** True when the on-screen touch controls should be up: the joystick scheme is selected, the
	 *  ride is live and nothing else owns the screen. */
	bool WantsTouchControls() const;

	/** The control handoff's on-screen cue. The "…and surf!" flash plays only when no touch controls
	 *  are drawn - where they are, their appearance is the cue, and words would be redundant. */
	void AnnounceHandoff();

	/** Install/remove the touch overlay to match WantsTouchControls(), push it live tuning, and
	 *  read this frame's thumb state into CurrentWeightOffset / bPumpButtonHeld. */
	void UpdateTouchControls(float DeltaTime);

	/** Charge-and-release pump: holding the button crouches the surfer, releasing extends the legs
	 *  and drives the board down. Force belongs entirely to the extension, scaled by how long the
	 *  crouch was held. See specs/pump-button-and-virtual-stick.md FR4. */
	void UpdateHeldPump(float DeltaTime);

	/** SurfPump's scripted thumb, ticked where the real button is read. True while "held". */
	bool TickScriptedPump(float DeltaTime);

	/** True while the touch overlay is driving weight, so the tilt and PC mouse paths stand down. */
	bool bTouchControlsDriving = false;

	/** True while a thumb is on the joystick — suppresses auto-centring, as holding a stick does. */
	bool bTouchStickHeld = false;
	/** Thumb up but the canvas is holding the last command (StickLatch); it must not auto-centre. */
	bool bTouchStickLatched = false;

	/** True while the pump button is held. */
	bool bPumpButtonHeld = false;

	/** How far into the crouch the surfer is, 0..1. Rises while the button is held. */
	float PumpCharge = 0.0f;

	/** True while the legs are extending after a release; the phase runs 0..1 across it and the
	 *  downward impulse is shaped over exactly that window. */
	bool  bPumpReleasing = false;
	float PumpReleasePhase = 0.0f;

	/** Charge captured at the moment of release, which scales the extension's impulse. */
	float PumpReleaseStrength = 0.0f;

	/** Previous tick's held state, for detecting a fresh press. */
	bool bPumpHeldLastTick = false;

	/** SurfPump's scripted thumb: seconds until the press, then seconds left holding. Negative /
	 *  zero = idle, which is every run nobody typed the command in. */
	float PumpScriptDelayRemaining = 0.0f;
	float PumpScriptHoldRemaining = 0.0f;

	/** Seconds of live control this pawn has had, for the surf.pump.auto metronome. */
	float PumpAutoElapsed = 0.0f;

	/** Held state coming from a UMG pump button, if the UI has one. ORed with the other sources. */
	bool bPumpHeldFromUI = false;

public:
	/** Wire a UMG pump button to this: true on Pressed, false on Released. It feeds the same
	 *  hold-to-pump cycle as the C++ touch button and the PC space bar, so a UMG button needs no
	 *  logic of its own - it only reports whether it is down. See
	 *  specs/pump-button-and-virtual-stick.md. */
	UFUNCTION(BlueprintCallable, Category = "Surfing|Pump")
	void SetPumpHeld(bool bHeld) { bPumpHeldFromUI = bHeld; }

	/** True while a pump gesture is running - crouching or extending. Unattenuated: it reports the
	 *  gesture, not whether it is achieving anything. */
	UFUNCTION(BlueprintPure, Category = "Surfing|Pump")
	bool IsPumpStrokeActive() const { return bPumpButtonHeld || bPumpReleasing; }

	/** Dev-only, for the SEND panel (specs/share-trace-from-phone.md): flush the ride still being
	 *  recorded so the file on disk is current when it is sent mid-ride. No-op when not recording. */
	void FlushInputTraceForSharing();

	/** Absolute path of the trace being recorded now, or empty. The SEND panel marks that row as
	 *  the live ride rather than a finished one. */
	FString ActiveInputTracePath() const { return bInputTraceActive ? InputTraceFilePath : FString(); }

protected:

	/** Ramped 0..1 pump-ness, for easing the sideways attenuation in and out. */
	float PumpLateralRamp = 0.0f;

	/** 0..1 pump input from the loading phase of phone vertical motion. Public
	 *  so AWeightDistribution can read it during its Tick. */
public:
	UPROPERTY(BlueprintReadOnly, Category = "Pump")
	float PumpInput = 0.0f;

private:
	/** Low-pass-filter state for the gravity-projected acceleration. Updated
	 *  each call to UpdatePumpInput; cutoff is USurfTuningSubsystem::PumpLowPassHz. */
	float PumpLowPassA = 0.0f;

	/** Cached pointer to USurfTuningSubsystem; resolved in BeginPlay. Reads
	 *  pump pipeline parameters during UpdatePumpInput. */
	UPROPERTY(Transient)
	USurfTuningSubsystem* Tuning = nullptr;

	// ========== Input Trace Recorder State ==========

	/** True while we're actively appending rows to the trace file. */
	bool bInputTraceActive = false;

	/** Full path to the per-session CSV being written. */
	FString InputTraceFilePath;

	/** Buffered CSV rows (data only — header/metadata are written at start). */
	FString InputTraceBuffer;

	/** Seconds since recording started (t-axis in the CSV). */
	float InputTraceElapsedTime = 0.0f;

	/** Seconds since the last disk flush; flush cadence is ~10 s. */
	float InputTraceTimeSinceFlush = 0.0f;

	/** Latest raw signed pitch delta vs NeutralGravity, cached by UpdateTiltWeight
	 *  for the recorder to read (pre-deadzone, pre-scale). */
	float LatestTiltPitchDeg = 0.0f;

	/** Latest raw signed roll delta vs NeutralGravity, cached by UpdateTiltWeight. */
	float LatestTiltRollDeg = 0.0f;

	/** Latest stick/mouse Vec2 as written by WeightInput (Vec2 in IA_Weight units). */
	FVector2D LatestStickValue = FVector2D::ZeroVector;

	/** Source tag written into each CSV row: "tilt", "stick", or "mouse". */
	FString InputTraceSourceTag;

	void StartInputTrace();
	void SampleInputTrace(float DeltaTime);
	void FlushInputTrace(bool bAsync);
	/** Why is written as a "# trace_stop=<why>" footer (specs/share-trace-from-phone.md FR4). */
	void StopInputTrace(const TCHAR* Why);
	/** Writes "# <Note>" into the trace: buffered in order while recording, appended to the closed
	 *  file otherwise (the wipeout card opens after the trace has stopped). */
	void AppendTraceNote(const FString& Note);

	// ========== Ride Replay (kinematic playback) ==========

	/** One recorded trajectory sample used for playback. Only the fields needed to
	 *  drive the board kinematically + re-sync the wave are kept. */
	struct FReplayRow
	{
		float t = 0.0f;
		FVector pos = FVector::ZeroVector;
		FRotator rot = FRotator::ZeroRotator;
		int32 waveFrame = -1;
		// Board velocities. Linear (cols 9-11) has always been recorded; angular (cols 30-32) is
		// newer, hence hasAngVel. Kinematic replay ignores both — it drives the transform directly.
		// A rails intro needs them: they are what the board resumes PHYSICS with at handoff, and
		// they must come from the recording, never from reading the solver on the handoff frame
		// (its derived magnitude carries a per-frame scale). See specs/deterministic-ride-handoff.md.
		FVector vel = FVector::ZeroVector;
		FVector angVelDeg = FVector::ZeroVector;
		bool hasAngVel = false;
		// Scripted rider animation state (column 33). Needed because a playable level's autopilots
		// are all enabled=false, so nothing else advances the rider past Paddle during a rails intro.
		ESurferAnimState riderAnim = ESurferAnimState::Paddle;
		bool hasRiderAnim = false;
		/** Pump stroke phase (column 34): 0..1 while a stroke runs, -1 when not pumping. One column
		 *  rather than two because the inactive state needs no phase. Pre-pump traces lack it and
		 *  replay with the rider not pumping, which is what they recorded. */
		float pumpPhase = -1.0f;
		// Recorded spray outputs (columns 16-27; zeros for pre-spray traces — see hasSpray).
		// Played back verbatim through ASprayController::SetReplaySpray so replay spray is the
		// live spray, not a reconstruction.
		FVector sprayVel[3] = { FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector };
		float sprayRate[3] = { 0.0f, 0.0f, 0.0f };
		bool hasSpray = false;
		// Recorded weight amounts (columns 28-29; 0.5 = centered). Played back through
		// USurferAnimInstance::SetReplayWeights so the rider leans/twists through the replay
		// (the live WeightDistribution is tick-frozen there). hasWeight false = pre-rider trace,
		// replay pushes neutral 0.5/0.5.
		float weightFront = 0.5f;
		float weightRight = 0.5f;
		bool hasWeight = false;
	};

	// ========== Scripted "rails" intro (deterministic handoff) ==================================
	// The visible intro is played back from a recorded pose track so every player, on every device,
	// takes control from the same board state. Distinct from the ride replay above: that one is a
	// terminal VIEWING mode (physics off, force pipeline not ticking, level reload to exit); this
	// one is resumable — the pipeline keeps computing, only force application is suppressed, and
	// the board hands back to live physics with the recorded velocities stamped on.
	// See specs/deterministic-ride-handoff.md.

	/** Load the intro trace, go kinematic, suppress force application, seat at row 0 and pin the
	 *  wave clock. Returns false (and leaves the live intro to run) on any problem: test run,
	 *  missing/too-short trace, or a trace predating the angular-velocity columns. */
	bool StartRails(const FString& TracePath);

	/** Advance the pose track; hands over via FinishRails on the final row — or on the last row at
	 *  or before RailsUntilSeconds, when -RailsUntil was passed. */
	void TickRails(float DeltaTime);

	/** Drive the board to the interpolated pose for time t with ETeleportType::None (kinematic
	 *  target, so Chaos derives V/W) and re-sync the wave frame. */
	void ApplyRailsAtTime(float t);

	/** Resume simulation, stamp the recorded velocities, release the wave-clock pin, restore force
	 *  application, log HANDOFF STATE. Releases on ReplayRows[ReleaseRow] — the last row, unless
	 *  -RailsUntil picked an earlier one. */
	void FinishRails(int32 ReleaseRow);

	/** Clear bManualFrameControl so the wave free-runs from the recorded frame. */
	void ReleaseWaveFrameControl();

	void LogHandoffState(const FReplayRow& Final);

	/** Parse one trace CSV into ReplayRows. Shared by the replay's LoadLatestTrace and the rails. */
	bool LoadTraceFile(const FString& Path);

	/** The rider mannequin's anim instance, or null. Shared by trace recording and rails playback. */
	class USurferAnimInstance* ResolveRiderAnim() const;

	/** Resolve a RailsIntroTrace value to an absolute path: Content/ first (the only root that
	 *  survives packaging), then Saved/ (where -RecordIntro writes). Absolute paths pass through. */
	FString ResolveRailsTracePath(const FString& InPath) const;

	bool bRailsActive = false;
	float RailsTime = 0.0f;

	/** -RecordIntro: the input trace was started at BeginPlay to capture the intro, and stops at
	 *  handoff so its final row is the state the player takes control from. */
	bool bRecordingIntroTrace = false;

	/** -RecordIntroSeconds=N: record a fixed N-second window from BeginPlay instead of stopping at
	 *  handoff. 0 = stop at handoff. Filtered test runs disable the intro autopilot, so handoff
	 *  fires ~1s in and only a fixed window can cover a long autopilot ride. */
	float IntroRecordSeconds = 0.0f;

	/** Wave-clock rebase state: -RecordIntro discards rows recorded before the autopilot's
	 *  StartWaveFrame injection, so the trace begins where the ride begins rather than in the
	 *  deferred-Start dead zone. See the rebase block in SampleInputTrace. */
	bool bIntroTraceRebased = false;
	int32 LastRecordedWaveFrame = -1;
	int32 IntroRowsRecorded = 0;

	/** Only rebase inside this opening window, and only on a forward jump at least this large. The
	 *  wave loop wraps 1078 -> 886 (a large negative delta), which must never count. */
	static constexpr float IntroRebaseWindowSeconds = 4.0f;
	static constexpr int32 IntroRebaseMinJump = 10;

	/** -RailsTrace=<path> was passed: use it instead of RailsIntroTrace and bypass the test-run
	 *  gate, so a headless A/B can exercise the rails without editing the level. */
	bool bRailsCommandLineOverride = false;

	/** -RailsUntil=<trace t>: release the rails at the last recorded row at or before this trace
	 *  time instead of running to the end of the trace. This is what turns a recording into an A/B
	 *  fixture — the ride up to here is the recording by construction, and only what follows is
	 *  physics under test. < 0 = run to the last row (the shipped intro behaviour).
	 *  See specs/replay-rails-until.md. */
	float RailsUntilSeconds = -1.0f;

	/** Index into ReplayRows the rails hand over on. Resolved once in StartRails from
	 *  RailsUntilSeconds; defaults to the last row. */
	int32 RailsReleaseRow = INDEX_NONE;

public:
	/** Trace time the rails actually released at, or < 0 while they are still driving / were never
	 *  used. AInputReplayAutoPilot reads this so input playback resumes at the same instant the
	 *  board resumed physics, rather than restarting the trace from t=0. */
	float GetRailsReleaseTraceTime() const { return RailsReleaseTraceTime; }

private:
	float RailsReleaseTraceTime = -1.0f;

	TArray<FReplayRow> ReplayRows;
	FString ReplayTraceFile;        // path of the trace currently loaded for playback
	float ReplayTime = 0.0f;        // seconds into the trace
	int32 ReplayRowHint = 0;        // straddling-row search hint (monotonic)
	bool bReplayHolding = false;    // reached the end; holding on the last frame
	bool bReplayWaveSynced = false; // loaded trace carried wave_frame data

	/** Cached WaterController BP actor (the wave clock). Resolved lazily; used for both
	 *  recording wave_frame and re-syncing it during replay. */
	UPROPERTY(Transient)
	AActor* CachedWaterController = nullptr;

	/** Cached SprayController (spray recording + replay playback). Lazily resolved; null in
	 *  levels without spray — recording then writes zero spray columns and replay skips spray. */
	UPROPERTY(Transient)
	class ASprayController* CachedSprayController = nullptr;

	class ASprayController* ResolveSprayController();

	AActor* ResolveWaterController();
	int32 ReadWaveFrame();          // reflection read of WaterController.CurrentFrame (-1 if none)
	void SetWaveFrame(int32 Frame); // reflection write of CurrentFrame (+ bManualFrameControl if present)

	bool LoadLatestTrace();         // find newest CSV + parse into ReplayRows; false if none
	void EnterReplayMode();         // freeze physics, teleport to row 0, Beside camera
	void TickReplay(float DeltaTime);
	void ApplyReplayAtTime(float t);

	// ---------- The ride list, and replaying something other than the last ride ----------
	// specs/best-ride-replay.md

	/** Where the traces live. One place, because two spellings of this path is how a prune and a
	 *  picker end up disagreeing about which directory they are protecting. */
	static FString TraceDirectory();

	/** FR2/FR9/D8: bank the just-ended ride as the LATEST record, and as the BEST if it beat the
	 *  stored one. Called only where the trace file is CLOSED - the fall and EndPlay - never from
	 *  the other three EndAssistRide sites, which can fire mid-wave while the file is still
	 *  growing. Score and filename land in one write, or neither does. */
	void CommitRideRecords();

	/** FR3: delete traces that no record references AND that are older than the grace period.
	 *  Never runs in a scripted run (FR7), never against the file being written or replayed. */
	void PruneTraces();

	/** Delete these trace file NAMES outright, whatever their age. Only the reset path uses it:
	 *  the records that protected them have just been cleared, so they are orphans by construction
	 *  (FR8). */
	void DeleteTraceFiles(const TSet<FString>& FileNames);

	/** Every ride the list can offer, newest-interesting first: the last ride, then each board's
	 *  best. Records whose trace is gone are dropped here rather than drawn greyed (FR2). */
	TArray<struct FRideListEntry> BuildRideList() const;

	/** Start a replay of one listed ride: swap in the board it was ridden on (D1, presentation
	 *  only), load the trace, enter replay mode. False if the trace would not load, in which case
	 *  nothing has been changed. */
	bool StartReplayOfRecord(const SurfAssist::FRideRecord& Record);

	/** What a replay borrowed and has to give back. Captured on the way in, spent on the way out;
	 *  the restore is the half FR5 says will be got wrong, so it is one struct rather than a
	 *  handful of members nobody can audit at a glance. */
	struct FPreReplayState
	{
		bool      bValid = false;
		FTransform BoardTransform;
		FVector   LinearVelocity = FVector::ZeroVector;
		FVector   AngularVelocityRad = FVector::ZeroVector;
		bool      bWasSimulating = false;
		int32     WaveFrame = -1;
		bool      bBehindCamera = false;
		bool      bControlsEnabled = false;
		bool      bRadarWasUp = false;
		FString   BoardId;          /**< The player's own board, to put back after D1's swap. */
	};
	FPreReplayState PreReplay;

	/** The just-ended ride's raw score, held between the ride ending and the trace file closing.
	 *  Those are not the same moment (D8), and the record needs both. */
	float PendingRideScoreCredit = 0.0f;
	bool  bRideAwaitingCommit = false;

	/** File name of the trace currently being replayed, so a prune can never delete it. */
	FString ReplayingTraceFileName;

	/** FR3's grace period. Days, not hours: `Saved/InputTraces/` is shared with development, and a
	 *  rule that deletes anything unreferenced takes the `-RecordIntro` capture a developer made
	 *  minutes ago to stage as a rails intro (D9). */
	static constexpr int32 kTraceGraceDays = 7;

	/** The rows currently on screen. Held because the panel's hooks index back into them, and
	 *  because D5 comes back to the row the player watched. */
	TArray<FRideListEntry> RideListEntries;
	int32 RideListHighlightIndex = INDEX_NONE;

	/** How long the replay has been holding on its final frame, so D5's return to the list waits
	 *  for the end-card to be read rather than slamming up over it. */
	float ReplayHoldSeconds = 0.0f;
	static constexpr float kReplayHoldBeforeListSeconds = 1.6f;

	void PlayRideListEntry(int32 Index);
	void CapturePreReplayState();

	/** D1's swap, and only as much of it as a kinematic replay can use: mesh and profile visuals,
	 *  no tuning, no assist. Also the restore, called with the player's own board id. */
	void ApplyReplayBoardVisuals(const FString& BoardId);

	/** Enable/disable ticking of the board's force-pipeline actors (FluidDynamics, Buoyancy,
	 *  SharedCalculations, WeightDistribution). Turned off on replay start so they don't apply
	 *  impulses/torques to the kinematic board (which only logs no-op warnings). */
	void SetForcePipelineTicking(bool bEnabled);

	// ========== Fall State ==========

	/** Seconds the board has been off the clean wave (zone not Pocket/Shoulder) since it was last on
	 *  it; the off-wave ending (specs/surfer-fall-ragdoll.md) fires when this passes
	 *  FallOffWaveSeconds while the board is slower than FallOffWaveSpeed. */
	float OffWaveSeconds = 0.0f;

	/** True once AmountPlaning has exceeded fallPlaningArmThreshold with controls live —
	 *  from then on the roll / lost-planing triggers are checked each tick. */
	bool bFallArmed = false;

	/** Seconds since TriggerFall. The wipeout card opens once this passes
	 *  kWipeoutCardDelaySeconds - a beat, not instantly, for the same reason the replay's end-card
	 *  waits before the list: the beat is what makes the stop read as intended rather than as a
	 *  card slamming up over the fall. See specs/two-screen-navigation.md FR6. With the authored
	 *  fall the beat is measured from the END of the clip instead (kWipeoutCardBeatAfterClipSeconds),
	 *  so the card never comes up over a rider still going in. */
	float FallElapsedSeconds = 0.0f;
	static constexpr float kWipeoutCardDelaySeconds = 1.2f;
	static constexpr float kWipeoutCardBeatAfterClipSeconds = 0.6f;

	/** Open the wipeout card once the beat has passed; hide the score counter under it. */
	void UpdateWipeoutCard(float DeltaTime);

	/** True while the rider is off the board on the AUTHORED fall (clip + kinematic motion), as
	 *  opposed to the ragdoll. Cleared with the rest of the fall state by RestoreRiderAfterFall. */
	bool bRiderFallAnimated = false;

	/** Kinematic state of the animated fall: the rider's horizontal velocity (starts at the board's
	 *  x fallVelocityInherit, relaxes into the water's), its height ABOVE the water surface at the
	 *  moment of the fall (held, so the rider rides the wave through the ending; relative because
	 *  the data's absolute height is not world Z - see waveheight-return-not-world-z), and the
	 *  levelling from the deck's tilt at detach to upright. */
	FVector RiderFallVelocity = FVector::ZeroVector;
	float RiderFallHeightAboveWater = 0.0f;
	FQuat RiderFallRotFrom = FQuat::Identity;
	FQuat RiderFallRotLevel = FQuat::Identity;
	float RiderFallLevelElapsed = 0.0f;
	/** The sink after the clip: seconds since it ended, and cm sunk so far (to fallSinkDepth). */
	float RiderFallAfterClipSeconds = 0.0f;
	float RiderFallSunkCm = 0.0f;

	/** Move the fallen rider each tick (animated fall only): momentum into water drift, level,
	 *  follow the surface. */
	void UpdateFallenRider(float DeltaTime);

	// ========== Ride HUD (‹ BACK) ==========

	/** True when the ride screen owns the display: not the start screen, no modal up. The Back pill
	 *  shows during a replay too - it is the replay's exit. */
	bool WantsRideHud() const;

	/** Install/remove the Back pill to match WantsRideHud(). */
	void UpdateRideHud();

	/** NFR2's safety net: the UMG ride bar (WBP_SurfboardControls) is supposed to be gone from the
	 *  level Blueprint. If a build still spawns it, remove it on the first tick and say so, so the
	 *  failure is loud and one-layer rather than a silent second UI layer under the Slate one. */
	void PurgeLegacyRideBar();
	bool bLegacyRideBarChecked = false;

	/** Set when a replay was launched from the hub (specs/two-screen-navigation.md FR4). Leaving it
	 *  then reloads into the hub rather than restoring a mid-ride world that never existed. */
	bool bReplayFromHub = false;

	/** RestartLevel and ReturnToHub, with one switch between them. */
	void ReloadLevel(bool bSkipStartScreen);

	/** True from ReloadLevel until the world goes away (the fade lasts half a second). Nothing
	 *  UI-side should react to state during that window - the wipeout card, for one, would see
	 *  bFallen with no card open and put itself straight back up over the fade. */
	bool bReloadPending = false;

	/** Rider attachment snapshot taken at TriggerFall so RestoreRiderAfterFall (Replay
	 *  path) can put the mesh back on the deck exactly where it stood. */
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> RiderPreFallParent = nullptr;
	FTransform RiderPreFallRelativeTransform;
	FName RiderPreFallSocket;
	FName RiderPreFallCollisionProfile;

	/** Cached SharedCalculations used for the AmountPlaning reads. Resolved lazily:
	 *  the SC wired to our board, else SharedCalculationsForCamera, else any in the level. */
	UPROPERTY(Transient)
	TObjectPtr<class ASharedCalculations> CachedSharedCalcForFall = nullptr;

	class ASharedCalculations* ResolveSharedCalcForFall();

	/** The wave-height actor the fallen rider follows (the SC's), or null. */
	class AWaveHeight* ResolveWaveHeightForFall();

	// ========== Gradual control handoff ==========

	/** Controller state for the assist. Reset at each handoff; owned here so nothing in SurfAssist
	 *  needs a global. */
	SurfAssist::FState AssistState;

	/** Controller state for trick scoring (specs/trick-scoring.md). Same ownership rule as
	 *  AssistState and for a sharper reason: it holds the earning window's timer, which is exactly
	 *  the kind of state that becomes a static by accident and then leaks between rides and between
	 *  PIE runs. Reset at each handoff. */
	TrickScore::FState TrickState;

	/** Live window state, cached for the counter. The overlay changes colour while the window is
	 *  open and shows the chain step above 1: a change of earning RATE alone is too subtle to read
	 *  in peripheral vision while the player is watching a wave. */
	float TrickWindowMultiplier = 1.0f;
	int32 TrickChainStep = 0;




	/** Biases the two weight scalars in place. No-op (leaves them exactly as passed) whenever the
	 *  assist is suppressed or alpha is 0, so a graduated player's ride is bit-identical to an
	 *  assist-free build. */
	void UpdateAssist(float DeltaTime, float& WeightRight, float& WeightInFront);

	/** True when the assist is switched on for this session at all: mode, the tuning-HUD kill switch,
	 *  and FR8's automated-run gates. Deliberately does NOT include the pre-handoff/rails/fallen
	 *  checks — the BADGE is up through the intro, only the controller is held back. */
	bool IsAssistEnabledThisSession() const;

	/** FR8's automated-run gates alone, without the player-facing switches: unattended, an external
	 *  weight override, or a non-empty autopilot filter (unless -AssistInTests forces it on). Split
	 *  out because the two halves answer different questions - see IsAssistSuppressed, where a
	 *  TIRED rider runs the controller even with the assist switched off, but never in a test. */
	bool IsAssistRunAutomated() const;

	/** The authority ceiling this ride is running at, or (before handoff) the one it is about to
	 *  start with. Single source for the latch, the badge and any HUD. */
	float GetPendingAssistAlpha() const;

	/** FR8: true in every automated run, AND before the player has the board. The assist writes the
	 *  same scalars the player writes, so leaving it live would shift every snapshot baseline and
	 *  corrupt every trace comparison. */
	bool IsAssistSuppressed() const;

	/** Badge visibility, every tick and independent of the controller: it is up through paddle,
	 *  cobra and pop-up, which is the dead time where the player actually reads it. */

	/** The alpha accumulated credit has EARNED right now, ignoring the ride latch. What the badge
	 *  shows mid-ride, so a step down is visible the moment it happens rather than announced on a
	 *  card at the start of the next ride. A hand-picked manual level still wins. */
	float GetEarnedAssistAlpha() const;

	/** specs/ride-score-counter.md - drives the top-centre counter from the credit the assist is
	 *  already accruing. Read-only: it must never become a second definition of how well the ride is
	 *  going (NFR2 there). */
	void UpdateRideScore();

	/** Accrue this tick's credit and watch for a schedule threshold crossing (FR5).
	 *
	 *  Split out of UpdateAssist because credit must keep accruing after the player graduates. At
	 *  alpha 0 UpdateAssist returns before evaluating the controller - deliberately, so a graduated
	 *  ride is bit-identical to a build with no assist - which would have frozen the counter at
	 *  exactly the moment the player has most reason to want one. The guard state is recomputed here
	 *  from SurfAssist::BandError, which is a pure read: no correction, no rate limiter, no weight
	 *  touched, so the trajectory stays identical. */
	void AccrueRideCredit(float DeltaTime, bool bGuardActive, float SpeedCmPerSecond,
		const SurfAssist::FTuning& T, const class ASharedCalculations* SC);

	/** Trick tuning from the subsystem, so the whole feel is dialled in through
	 *  Saved/TuningOverrides.json with no rebuild. Struct defaults are the fallback. */
	TrickScore::FTuning GetTrickTuning() const;

	/** FR11: this board's score multiplier, from the difficulty rating the rack card already shows.
	 *  Applied at presentation only - never baked into a stored best, or re-tuning it would put one
	 *  board's history into two different currencies. */
	float GetBoardScoreMultiplier() const;

	/** Credit accumulated when this ride began, so the in-ride counter can show the delta. */
	float AssistRideStartCredit = 0.0f;

	/** Assist level the accumulated credit had earned at the last check, so a threshold crossing is
	 *  detected once. Uses SurfAssist::LevelForAlpha - the single shared definition the badge and
	 *  the card already use, because deriving it twice is what let them disagree before. */
	int32 AssistEarnedLevel = -1;

	/** Latch this ride's alpha from stored credit. Called on the first assisted tick after handoff. */
	void BeginAssistRide();

	/** Bank the ride's credit. Called on fall, on EndPlay, and whenever the ride stops being live. */
	void EndAssistRide();

	/** Copy the live tunables out of USurfTuningSubsystem, falling back to the struct defaults. */
	SurfAssist::FTuning GetAssistTuning() const;

	/** Geometric |board world roll| source: asin(board.left.Z), board.left = local -X. */
	float GetBoardWorldRollDeg() const;

	/** Per-tick arming + trigger checks; calls TriggerFall when one fires. */
	void UpdateFallDetection(float DeltaTime);

	/** Undo the ragdoll for kinematic replay: physics off, original collision profile,
	 *  re-attached at the pre-fall relative transform, animation-driven again. */
	void RestoreRiderAfterFall();

	// ========== AutoPilot State ==========

	/** Time elapsed since autopilot started */
	float AutoPilotElapsedTime = 0.0f;

	/** Time elapsed since autopilot completed */
	float TimeAfterAutoPilotComplete = 0.0f;

	/** True if player controls are currently enabled */
	bool bPlayerControlsEnabled = false;

	/** True if camera transition to behind has started */
	bool bCameraTransitionStarted = false;

	/** Initialize autopilot timing state at BeginPlay */
	void InitAutoPilotState();

	/** Install the pre-wave start/tutorial overlay and pause the world until the player taps
	 *  Start. Interactive play only (no-op in snapshot/replay test runs, or right after a
	 *  Restart, which skips straight into the ride). */
	void MaybeShowStartScreen();

	/** Shared installer: pause the world, switch to menu input, add the overlay. Used by both
	 *  MaybeShowStartScreen (gated, at BeginPlay) and ShowStartScreen (explicit, mid-game). */
	void OpenStartOverlay();

	/** Start pressed: remove the overlay, restore game input, unpause and let the ride begin. */
	void OnStartScreenStart();

	/** Place the menu camera at StartScreenCameraOffsetCm from the board, looking back at it. Used
	 *  instead of the ride's UpdateCameraTransform while the Start screen is up. */
	void PoseStartScreenCamera();

	/** The WaterController's CurrentFrame int property, plus the controller it lives on. Used by the
	 *  capture pass to step the wave frame by frame. */
	FIntProperty* GetWaveFrameProperty(AActor*& OutController) const;

	/** The WaterController blueprint, found by the capture pass. */
	TWeakObjectPtr<AActor> CachedWaveController;

	/** Check if player controls should be enabled and handle transitions */
	void UpdatePlayerControlState(float DeltaTime);

	/** Called the tick player controls flip to enabled — snapshot tilt neutral here */
	void OnPlayerControlsEnabled();
};
