// See SensorProbeOverlay.h and specs/sensor-probe.md.

#include "SensorProbeOverlay.h"

#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "SurfLog.h"

#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	// ---- The scripted sequence -------------------------------------------------------
	//
	// Static poses pin down the device frame vs gravity. Motion steps are what actually
	// matter for the yaw bug: they are the only way to see whether RotationRate carries
	// anything, and which way its sign runs relative to the lean the player intends.
	struct FProbeStep
	{
		const TCHAR* Label;
		const TCHAR* Instruction;
		bool bMotion;
	};

	// Wording note: steps 4-5 are stated as INTENT ("the motion you'd use to lean left"),
	// because the sign we ultimately need is intent-to-gyro. Steps 6-7 are stated
	// MECHANICALLY, because they exist to identify which axis is which. The gyro vector
	// records the mechanics either way, so intent-phrasing loses nothing.
	//
	// Swing vs twist is the easy pair to confuse, so each carries the same discriminator:
	// during a twist the on-screen image rotates; during a swing it does not.
	const FProbeStep GSteps[] = {
		{ TEXT("flat"),
		  TEXT("Put the phone FLAT on a table,\nscreen facing UP.\n\nLet go. Hold still, press OK."),
		  false },

		{ TEXT("upright"),
		  TEXT("Stand the phone UPRIGHT in landscape:\nscreen vertical, facing you, like a\nsmall TV on a shelf.\n\nHold still, press OK."),
		  false },

		{ TEXT("reclined"),
		  TEXT("Lie back and hold the phone exactly as\nyou do when playing: in front of your\nface, tipped PAST vertical so the screen\nlooks slightly DOWN at you.\n\nHold still, press OK. Keep this pose for\nall remaining steps."),
		  false },

		{ TEXT("swing_left"),
		  TEXT("Make the sideways motion you would\nnaturally use to lean the board LEFT.\n\nMost likely: pivot around an imaginary\nVERTICAL rod through the phone's middle\n- left end toward you, right end away.\nThe image must NOT rotate.\n\nDo the motion, then press OK."),
		  true },

		{ TEXT("swing_right"),
		  TEXT("Same thing, the other way: the motion\nyou'd use to lean the board RIGHT.\n\nRight end toward you, left end away.\nAgain, the image must NOT rotate.\n\nDo the motion, then press OK."),
		  true },

		{ TEXT("twist_left"),
		  TEXT("Different motion. TWIST the phone in\nits own flat plane, like a steering\nwheel facing you - anticlockwise.\n\nBoth ends stay the SAME distance from\nyou. This time the image DOES rotate.\n\nDo the motion, then press OK."),
		  true },

		{ TEXT("pitch_down"),
		  TEXT("Tip the TOP edge of the screen AWAY\nfrom your face, bottom edge toward you\n- like tipping a book away from you.\n\nDo the motion, then press OK."),
		  true },
	};
	const int32 GNumSteps = UE_ARRAY_COUNT(GSteps);

	// ~2 s at 60 Hz. Sized so a motion step captures the whole gesture leading up to the
	// OK press, not just the stillness after it.
	const int32 GRingCapacity = 140;

	struct FSensorSample
	{
		double  T = 0.0;
		FVector Tilt = FVector::ZeroVector;
		FVector Gyro = FVector::ZeroVector;
		FVector Grav = FVector::ZeroVector;
		FVector Accel = FVector::ZeroVector;
		FVector NeutralG = FVector::ZeroVector;
		FVector FwdAxis = FVector::ZeroVector;
		FVector RightAxis = FVector::ZeroVector;
	};

	struct FProbeState;
}

// Declared early: the widget's active timer drives sampling, and the timer needs to reach
// the per-world state that owns the ring buffer.
namespace { void DoSample(UWorld* World); }

namespace
{
	FString CsvRow(int32 StepIndex, const TCHAR* StepLabel, const FSensorSample& S, bool bMark)
	{
		const FVector G = S.Grav.GetSafeNormal();
		const float PitchDeg = S.FwdAxis.IsNearlyZero()
			? 0.0f
			: FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(FVector::DotProduct(G, S.FwdAxis), -1.0f, 1.0f)));
		const float RollDeg = S.RightAxis.IsNearlyZero()
			? 0.0f
			: FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(FVector::DotProduct(G, S.RightAxis), -1.0f, 1.0f)));
		// The one number the whole yaw bug turns on: angular rate about the gravity axis.
		const float YawRate = FVector::DotProduct(S.Gyro, G);

		return FString::Printf(
			TEXT("%d,%s,%.4f,")
			TEXT("%.5f,%.5f,%.5f,")   // tilt
			TEXT("%.5f,%.5f,%.5f,")   // gyro
			TEXT("%.5f,%.5f,%.5f,")   // gravity
			TEXT("%.5f,%.5f,%.5f,")   // accel
			TEXT("%.5f,%.5f,%.5f,%.5f,%.5f,%d\n"),
			StepIndex, StepLabel, S.T,
			S.Tilt.X, S.Tilt.Y, S.Tilt.Z,
			S.Gyro.X, S.Gyro.Y, S.Gyro.Z,
			S.Grav.X, S.Grav.Y, S.Grav.Z,
			S.Accel.X, S.Accel.Y, S.Accel.Z,
			S.Grav.Size(), G.Z, PitchDeg, RollDeg, YawRate,
			bMark ? 1 : 0);
	}
}

// Full-screen wizard: instruction card, live sensor readouts, OK button.
//
// The live readouts are not decoration — they are the safety net. If the motion values
// are frozen (paused world, sensor not delivering, wrong call path), that is visible on
// screen before any file is written, instead of yielding a file full of identical rows
// that looks like a successful measurement.
class SSensorProbe : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSensorProbe) {}
	SLATE_END_ARGS()

	void SetWorld(UWorld* InWorld) { World = InWorld; }

	void Construct(const FArguments&)
	{
		SetVisibility(EVisibility::Visible);

		// Sampling runs on a Slate active timer, NOT the pawn's Tick. The start screen
		// pauses the world (ASurfboardPawn's start-screen install calls SetGamePaused), and
		// this overlay sits above it at ZOrder 400 — so a pawn-Tick-driven sampler collects
		// exactly nothing for the entire probe session and silently writes header-only
		// files. Slate active timers keep firing while the world is paused, which is also
		// what we want: the board stays frozen while the player concentrates on poses.
		RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SSensorProbe::OnProbeTick));

		// Sized for a landscape phone, which is short: the instructions are long and the
		// OK button must never be pushed off the bottom edge.
		const FSlateFontInfo BodyFont  = FCoreStyle::GetDefaultFontStyle("Regular", 15);
		const FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle("Bold", 17);
		const FSlateFontInfo DataFont  = FCoreStyle::GetDefaultFontStyle("Mono", 11);

		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.02f, 0.05f, 0.07f, 0.92f))
			.Padding(FMargin(18.0f))
			[
				// Instructions left, live numbers right: on a landscape phone the width is
				// the abundant axis and the height is not, and both need to be readable at
				// arm's length while holding an awkward pose.
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot().FillWidth(1.35f).Padding(0, 0, 16, 0)
				[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SAssignNew(StepText, STextBlock)
					.Font(TitleFont)
					.ColorAndOpacity(FLinearColor(1.0f, 0.78f, 0.34f))
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 14)
				[
					SAssignNew(InstructionText, STextBlock)
					.Font(BodyFont)
					.ColorAndOpacity(FLinearColor::White)
					.AutoWrapText(true)
				]

				+ SVerticalBox::Slot().FillHeight(1.0f) [ SNew(SSpacer) ]

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
				[
					SNew(SBox).HeightOverride(52.0f)
					[
						SNew(SButton)
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.OnClicked(this, &SSensorProbe::OnOkClicked)
						[
							SNew(STextBlock).Font(TitleFont).Text(FText::FromString(TEXT("OK — capture")))
						]
					]
				]
				]

				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SAssignNew(ReadoutText, STextBlock)
					.Font(DataFont)
					.ColorAndOpacity(FLinearColor(0.65f, 0.88f, 0.85f))
				]
			]
		];

		RefreshStepText();
	}

	void SetOnCapture(TFunction<void(int32)> InFn) { OnCapture = MoveTemp(InFn); }

	int32 GetStepIndex() const { return StepIndex; }

	void SetReadout(const FString& In) { if (ReadoutText.IsValid()) ReadoutText->SetText(FText::FromString(In)); }

	void SetDone(const FString& FilePath)
	{
		bDone = true;
		if (StepText.IsValid())
		{
			StepText->SetText(FText::FromString(TEXT("Done — press OK again to close")));
		}
		if (InstructionText.IsValid())
		{
			// Show the resolved absolute path rather than a guessed adb incantation — the
			// on-device Saved/ location varies by packaging, and a wrong path printed
			// confidently wastes more time than no path at all.
			InstructionText->SetText(FText::FromString(
				FString::Printf(TEXT("Written to:\n%s\n\nadb pull that path."), *FilePath)));
		}
	}

private:
	EActiveTimerReturnType OnProbeTick(double, float)
	{
		// Close on the timer rather than inside the click handler: Uninstall releases the
		// last reference to this widget, and doing that from within its own OnClicked is
		// asking for trouble. The delegate pins us for the duration of this call.
		if (bPendingClose)
		{
			SensorProbe::Uninstall(World.Get());
			return EActiveTimerReturnType::Stop;
		}
		DoSample(World.Get());
		return EActiveTimerReturnType::Continue;
	}

	FReply OnOkClicked()
	{
		if (bDone)
		{
			// Second press on the done screen dismisses. Without this the wizard sits over
			// the game at ZOrder 400 for the rest of the session, blocking play.
			bPendingClose = true;
			return FReply::Handled();
		}
		if (OnCapture)
		{
			OnCapture(StepIndex);
		}
		++StepIndex;
		RefreshStepText();
		return FReply::Handled();
	}

	void RefreshStepText()
	{
		if (bDone || StepIndex >= GNumSteps)
		{
			return;
		}
		if (StepText.IsValid())
		{
			StepText->SetText(FText::FromString(
				FString::Printf(TEXT("Step %d of %d  ·  %s"), StepIndex + 1, GNumSteps, GSteps[StepIndex].Label)));
		}
		if (InstructionText.IsValid())
		{
			InstructionText->SetText(FText::FromString(GSteps[StepIndex].Instruction));
		}
	}

	TSharedPtr<STextBlock> StepText;
	TSharedPtr<STextBlock> InstructionText;
	TSharedPtr<STextBlock> ReadoutText;
	TFunction<void(int32)>  OnCapture;
	TWeakObjectPtr<UWorld>  World;
	int32 StepIndex = 0;
	bool  bDone = false;
	bool  bPendingClose = false;
};

namespace
{
	struct FProbeState
	{
		TSharedPtr<SWidget>      Root;
		TSharedPtr<SSensorProbe> Widget;
		TArray<FSensorSample>    Ring;
		int32                    RingHead = 0;
		FString                  Buffer;
		FString                  FilePath;
		double                   StartTime = 0.0;
		int64                    SampleCount = 0;
		bool                     bHeaderWritten = false;
		FVector                  NeutralG = FVector::ZeroVector;
		FVector                  FwdAxis = FVector::ZeroVector;
		FVector                  RightAxis = FVector::ZeroVector;
	};
	TMap<UWorld*, TSharedPtr<FProbeState>> GProbeStates;

	// Called every Slate frame from the widget's active timer — so it keeps running while
	// the world is paused behind the start screen, which is exactly when the probe is used.
	void DoSample(UWorld* World)
	{
		TSharedPtr<FProbeState>* Found = GProbeStates.Find(World);
		if (!Found || !Found->IsValid())
		{
			return;
		}
		TSharedPtr<FProbeState> State = *Found;

		APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
		if (!PC)
		{
			if (State->Widget.IsValid())
			{
				State->Widget->SetReadout(TEXT("no PlayerController\n\nnothing can be sampled"));
			}
			return;
		}

		FVector Tilt, RotationRate, Gravity, Acceleration;
		PC->GetInputMotionState(Tilt, RotationRate, Gravity, Acceleration);

		FSensorSample S;
		S.T         = FPlatformTime::Seconds() - State->StartTime;
		S.Tilt      = Tilt;
		S.Gyro      = RotationRate;
		S.Grav      = Gravity;
		S.Accel     = Acceleration;
		S.NeutralG  = State->NeutralG;
		S.FwdAxis   = State->FwdAxis;
		S.RightAxis = State->RightAxis;

		const int32 N = State->Ring.Num();
		State->Ring[State->RingHead] = S;
		State->RingHead = (State->RingHead + 1) % N;
		++State->SampleCount;

		// Live readout. Deliberately shows the raw vectors: a frozen gyro or an all-zero
		// RotationRate is then obvious on the phone, before any file exists. The paused
		// flag is shown because a paused world is what silently broke the first attempt.
		if (State->Widget.IsValid())
		{
			const FVector G = Gravity.GetSafeNormal();
			State->Widget->SetReadout(FString::Printf(
				TEXT("samples  %lld\n")
				TEXT("(must climb)\n")
				TEXT("paused   %s\n")
				TEXT("\n")
				TEXT("gravity\n %7.3f\n %7.3f\n %7.3f\n |g|=%.3f\n")
				TEXT("\n")
				TEXT("gyro rad/s\n %7.3f\n %7.3f\n %7.3f\n")
				TEXT("\n")
				TEXT("g.z      %6.3f\n")
				TEXT("yawrate  %6.3f\n")
				TEXT("\n")
				TEXT("gyro stuck at\n0.000 while you\nrotate = sensor\nnot reaching us"),
				State->SampleCount,
				UGameplayStatics::IsGamePaused(World) ? TEXT("yes") : TEXT("no"),
				Gravity.X, Gravity.Y, Gravity.Z, Gravity.Size(),
				RotationRate.X, RotationRate.Y, RotationRate.Z,
				G.Z,
				FVector::DotProduct(RotationRate, G)));
		}
	}

	void FlushToDisk(const TSharedPtr<FProbeState>& State)
	{
		if (!State.IsValid() || State->Buffer.IsEmpty())
		{
			return;
		}
		IFileManager& Fm = IFileManager::Get();
		const FString Dir = FPaths::GetPath(State->FilePath);
		if (!Fm.DirectoryExists(*Dir))
		{
			Fm.MakeDirectory(*Dir, /*Tree*/true);
		}
		const bool bOk = FFileHelper::SaveStringToFile(
			State->Buffer, *State->FilePath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
			&Fm,
			State->bHeaderWritten ? EFileWrite::FILEWRITE_Append : EFileWrite::FILEWRITE_None);

		if (bOk)
		{
			State->bHeaderWritten = true;
			State->Buffer.Reset();
		}
		else
		{
			UE_LOG(LogSurf, Warning, TEXT("SensorProbe: FAILED to write %s"), *State->FilePath);
		}
	}
}

namespace SensorProbe
{
	void Install(UWorld* World)
	{
		if (!World || GProbeStates.Contains(World))
		{
			return;
		}
		UGameViewportClient* Viewport = World->GetGameViewport();
		if (!Viewport)
		{
			UE_LOG(LogSurf, Warning, TEXT("SensorProbe::Install: no GameViewportClient"));
			return;
		}

#if PLATFORM_ANDROID
		const float Scale = 2.0f;
#else
		const float Scale = 1.0f;
#endif

		TSharedPtr<SSensorProbe> Widget;
		TSharedRef<SWidget> Root = SNew(SDPIScaler).DPIScale(Scale)
			[
				SAssignNew(Widget, SSensorProbe)
			];

		TSharedPtr<FProbeState> State = MakeShared<FProbeState>();
		State->Root = Root;
		State->Widget = Widget;
		Widget->SetWorld(World);
		State->Ring.SetNum(GRingCapacity);
		State->StartTime = FPlatformTime::Seconds();

		const FString Stamp = FDateTime::Now().ToString(TEXT("%Y-%m-%d-%H-%M-%S"));
		State->FilePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("SensorProbe"), TEXT("probe-") + Stamp + TEXT(".csv")));

		// Metadata that the analysis needs and that the rows themselves cannot carry:
		// screen orientation drives the engine's axis remap, and AndroidUnifyMotionSpace
		// decides whether all four motion vectors even share a frame.
		static IConsoleVariable* UnifyCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Android.UnifyMotionSpace"));
		State->Buffer += FString::Printf(TEXT("# session=%s\n"), *Stamp);
		State->Buffer += FString::Printf(TEXT("# platform=%s\n"), ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()));
		State->Buffer += FString::Printf(TEXT("# device_orientation=%d\n"), (int32)FPlatformMisc::GetDeviceOrientation());
		State->Buffer += FString::Printf(TEXT("# unify_motion_space=%s\n"),
			UnifyCVar ? *FString::FromInt(UnifyCVar->GetInt()) : TEXT("(cvar not found)"));
		State->Buffer += FString::Printf(TEXT("# ring_capacity=%d\n"), GRingCapacity);
		State->Buffer += TEXT("# gyro/rotation-rate is rad/s; gravity is in g units when unify_motion_space=1\n");
		State->Buffer += TEXT("# is_mark=1 marks the exact sample at the OK press; rows before it are the preceding ~2s\n");
		State->Buffer += TEXT("step,label,t,tilt_x,tilt_y,tilt_z,gyro_x,gyro_y,gyro_z,")
			TEXT("grav_x,grav_y,grav_z,acc_x,acc_y,acc_z,gmag,gz,pitch_deg,roll_deg,yaw_rate,is_mark\n");

		TWeakPtr<FProbeState> WeakState = State;
		Widget->SetOnCapture([WeakState](int32 StepIndex)
		{
			TSharedPtr<FProbeState> S = WeakState.Pin();
			if (!S.IsValid() || StepIndex >= GNumSteps)
			{
				return;
			}
			const TCHAR* Label = GSteps[StepIndex].Label;

			// Replay the ring oldest-first so a motion step reads as a time series.
			const int32 N = S->Ring.Num();
			const int32 Count = (int32)FMath::Min<int64>(S->SampleCount, N);
			for (int32 i = 0; i < Count; ++i)
			{
				const int32 Idx = (S->RingHead - Count + i + 2 * N) % N;
				const bool bMark = (i == Count - 1);
				S->Buffer += CsvRow(StepIndex, Label, S->Ring[Idx], bMark);
			}
			FlushToDisk(S);

			// Reset the ring so the next step starts empty. Without this, a step pressed
			// less than the ring's ~2 s after the previous one inherits the tail of that
			// previous gesture — the 2026-08-20 capture showed an identical peak in both
			// swing_left and swing_right for exactly this reason. Harmless there because
			// the dominant signs still separated cleanly, but it is a trap.
			S->SampleCount = 0;
			S->RingHead = 0;

			UE_LOG(LogSurf, Display, TEXT("SensorProbe: captured step %d (%s), %d samples -> %s"),
				StepIndex, Label, Count, *S->FilePath);

			if (StepIndex + 1 >= GNumSteps && S->Widget.IsValid())
			{
				S->Widget->SetDone(S->FilePath);
			}
		});

		// ZOrder 400 — above the tuning HUD (300) and every gameplay overlay. This is a
		// measurement tool and must never be the thing that is occluded.
		Viewport->AddViewportWidgetContent(Root, /*ZOrder*/ 400);
		GProbeStates.Add(World, State);

		UE_LOG(LogSurf, Display, TEXT("SensorProbe: installed, writing to %s"), *State->FilePath);
	}

	void Uninstall(UWorld* World)
	{
		if (!World)
		{
			return;
		}
		if (TSharedPtr<FProbeState>* Found = GProbeStates.Find(World))
		{
			TSharedPtr<FProbeState> State = *Found;
			FlushToDisk(State);
			if (UGameViewportClient* Viewport = World->GetGameViewport())
			{
				if (State.IsValid() && State->Root.IsValid())
				{
					Viewport->RemoveViewportWidgetContent(State->Root.ToSharedRef());
				}
			}
			GProbeStates.Remove(World);
			UE_LOG(LogSurf, Display, TEXT("SensorProbe: uninstalled."));
		}
	}

	bool IsActive(UWorld* World)
	{
		return World && GProbeStates.Contains(World);
	}

	void SetTiltBasis(
		UWorld* World,
		const FVector& NeutralGravity,
		const FVector& TiltForwardAxis,
		const FVector& TiltRightAxis)
	{
		TSharedPtr<FProbeState>* Found = GProbeStates.Find(World);
		if (!Found || !Found->IsValid())
		{
			return;
		}
		(*Found)->NeutralG  = NeutralGravity;
		(*Found)->FwdAxis   = TiltForwardAxis;
		(*Found)->RightAxis = TiltRightAxis;
	}
}
