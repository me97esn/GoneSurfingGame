#include "WaveGeometrySubsystem.h"
#include "WaveHeight.h"
#include "InfiniteWaveManager.h"
#include "SurfLog.h"
#include "EngineUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/IConsoleManager.h"

namespace
{
	// A/B override of the two model points without touching the JSON: "x0/y0/f0/x1/y1/f1" (world cm,
	// frame). Read directly here so it sticks from a launch -ExecCmds.
	static FString WaveGeometry_ModelOverride;
	static FAutoConsoleVariableRef CVarWaveGeometryModel(TEXT("surf.wavegeo.model"), WaveGeometry_ModelOverride,
		TEXT("Override the peel model's two points: \"x0/y0/f0/x1/y1/f1\" (world cm, frame). Empty = Content/WaveGeometry/<map>.json."));

	bool WaveGeometry_ParseFloats(const FString& In, TArray<float>& Out)
	{
		TArray<FString> Parts;
		In.Replace(TEXT("/"), TEXT(" ")).Replace(TEXT(","), TEXT(" ")).ParseIntoArrayWS(Parts);
		Out.Reset();
		for (const FString& P : Parts) { if (P.IsNumeric()) { Out.Add(FCString::Atof(*P)); } }
		return Out.Num() > 0;
	}
}

bool UWaveGeometrySubsystem::IsReady()
{
	if (!bInitialised) { Initialise(); }
	return bInitialised && !Line.IsZero();
}

void UWaveGeometrySubsystem::Initialise()
{
	bInitialised = true;
	UWorld* World = GetWorld();
	if (!World) { return; }

	// The wave frame from the tile layout, the same way ASharedCalculations resolves its back axis:
	// tiles step by (YOffsetPerActor, ActorSpacing) in world XY - that is the down-line axis - and
	// the cross-shore/back axis is the perpendicular, oriented +X-ish (this project's convention).
	for (TActorIterator<AInfiniteWaveManager> It(World); It; ++It)
	{
		if (FMath::Abs(It->ActorSpacing) > KINDA_SMALL_NUMBER)
		{
			FVector back = FVector(It->ActorSpacing, -It->YOffsetPerActor, 0.0f).GetSafeNormal();
			if (!back.IsNearlyZero())
			{
				if (back.X < 0.0f) { back = -back; }
				Back = back;
				Line = FVector(-back.Y, back.X, 0.0f);
				if (Line.Y < 0.0f) { Line = -Line; }
			}
		}
		break;
	}
	for (TActorIterator<AWaveHeight> It(World); It; ++It)
	{
		WaveHeight = *It;
		NumLoopFrames = FMath::Max(1, It->EndFrame - It->StartFrame + 1);
		break;
	}
	if (Line.IsZero())
	{
		UE_LOG(LogSurf, Display, TEXT("WaveGeometry: no InfiniteWaveManager in this level; no wave frame, no model."));
		return;
	}

	FString MapName = World->GetMapName();
	MapName.RemoveFromStart(World->StreamingLevelsPrefix);
	if (!ApplyModelOverride()) { LoadModelFromJson(MapName); }
	FinishModel();

	UE_LOG(LogSurf, Display, TEXT("WaveGeometry: map=%s line=(%.3f, %.3f) back=(%.3f, %.3f) loop=%d frames | model %s: %s | peel %.2f cm/frame, period %.0f cm | crestC +%.0f boreFront %.0f pocket -%.0f..+%.0f ramp %.0f"),
		*MapName, Line.X, Line.Y, Back.X, Back.Y, NumLoopFrames,
		Model.bValid ? TEXT("ok") : TEXT("NONE"), *Model.Source, PeelSpeed, PeriodAlongLine,
		Model.ImpactToCrestC, Model.BoreFrontStart, Model.PocketAhead, Model.PocketBehind, Model.BrokenRamp);
}

bool UWaveGeometrySubsystem::ApplyModelOverride()
{
	TArray<float> V;
	if (WaveGeometry_ModelOverride.IsEmpty() || !WaveGeometry_ParseFloats(WaveGeometry_ModelOverride, V) || V.Num() < 6 || V[5] == V[2])
	{
		return false;
	}
	// Keep the JSON's zone constants if it loads; only the points are overridden.
	UWorld* World = GetWorld();
	FString MapName = World ? World->GetMapName() : FString();
	if (World) { MapName.RemoveFromStart(World->StreamingLevelsPrefix); }
	LoadModelFromJson(MapName);
	Model.P0 = FVector2D(V[0], V[1]); Model.F0 = V[2];
	Model.P1 = FVector2D(V[3], V[4]); Model.F1 = V[5];
	Model.bValid = true;
	Model.Source = FString::Printf(TEXT("surf.wavegeo.model '%s'"), *WaveGeometry_ModelOverride);
	return true;
}

bool UWaveGeometrySubsystem::LoadModelFromJson(const FString& MapName)
{
	const FString Path = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("WaveGeometry"), MapName + TEXT(".json"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		Model.Source = FString::Printf(TEXT("%s not found"), *Path);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogSurf, Warning, TEXT("WaveGeometry: %s is not valid JSON"), *Path);
		Model.Source = FString::Printf(TEXT("%s invalid"), *Path);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
	if (!Root->TryGetArrayField(TEXT("points"), Points) || !Points || Points->Num() < 2)
	{
		UE_LOG(LogSurf, Warning, TEXT("WaveGeometry: %s has no \"points\" [2]"), *Path);
		Model.Source = FString::Printf(TEXT("%s has no points"), *Path);
		return false;
	}
	auto ReadPoint = [](const TSharedPtr<FJsonValue>& V, FVector2D& OutXY, float& OutF) -> bool
	{
		const TSharedPtr<FJsonObject>* O = nullptr;
		if (!V.IsValid() || !V->TryGetObject(O) || !O || !O->IsValid()) { return false; }
		double x = 0, y = 0, f = 0;
		if (!(*O)->TryGetNumberField(TEXT("x"), x) || !(*O)->TryGetNumberField(TEXT("y"), y) || !(*O)->TryGetNumberField(TEXT("frame"), f)) { return false; }
		OutXY = FVector2D((float)x, (float)y); OutF = (float)f;
		return true;
	};
	if (!ReadPoint((*Points)[0], Model.P0, Model.F0) || !ReadPoint((*Points)[1], Model.P1, Model.F1) || Model.F1 == Model.F0)
	{
		UE_LOG(LogSurf, Warning, TEXT("WaveGeometry: %s points need x, y, frame and two different frames"), *Path);
		Model.Source = FString::Printf(TEXT("%s bad points"), *Path);
		return false;
	}
	double d = 0;
	if (Root->TryGetNumberField(TEXT("impactToCrestC"), d)) { Model.ImpactToCrestC = (float)d; }
	if (Root->TryGetNumberField(TEXT("pocketAhead"), d))    { Model.PocketAhead = (float)d; }
	if (Root->TryGetNumberField(TEXT("pocketBehind"), d))   { Model.PocketBehind = (float)d; }
	if (Root->TryGetNumberField(TEXT("brokenRamp"), d))     { Model.BrokenRamp = (float)d; }
	if (Root->TryGetNumberField(TEXT("behindMargin"), d))   { Model.BehindMargin = (float)d; }
	if (Root->TryGetNumberField(TEXT("faceExtent"), d))     { Model.FaceExtent = (float)d; }
	if (Root->TryGetNumberField(TEXT("boreFrontStart"), d)) { Model.BoreFrontStart = (float)d; }
	if (Root->TryGetNumberField(TEXT("boreDriftRate"), d))  { Model.BoreDriftRate = (float)d; }
	if (Root->TryGetNumberField(TEXT("boreFrontFade"), d))  { Model.BoreFrontFade = (float)d; }
	if (Root->TryGetNumberField(TEXT("boreFrontSpeed"), d)) { Model.BoreFrontSpeed = (float)d; }
	if (Root->TryGetNumberField(TEXT("shoulderAfterSeconds"), d)) { Model.ShoulderAfterSeconds = (float)d; }
	Model.bValid = true;
	Model.Source = Path;
	return true;
}

void UWaveGeometrySubsystem::FinishModel()
{
	if (!Model.bValid) { PeelSpeed = 0.0f; PeriodAlongLine = 0.0f; return; }
	VelXY = (Model.P1 - Model.P0) / (Model.F1 - Model.F0);
	PeelSpeed = (float)(VelXY.X * Line.X + VelXY.Y * Line.Y);
	PeriodAlongLine = PeelSpeed * (float)NumLoopFrames;
	if (FMath::Abs(PeriodAlongLine) < 1.0f)
	{
		UE_LOG(LogSurf, Warning, TEXT("WaveGeometry: the model's two points give no motion along the line; model disabled (%s)"), *Model.Source);
		Model.bValid = false;
	}
}

FVector2D UWaveGeometrySubsystem::ImpactXY(int32 Frame, int32 WaveIndex)
{
	if (!HasModel()) { return FVector2D::ZeroVector; }
	return Model.P0 + VelXY * ((float)Frame - Model.F0 + (float)WaveIndex * (float)NumLoopFrames);
}

int32 UWaveGeometrySubsystem::NearestWaveIndex(int32 Frame, float AtS)
{
	if (!HasModel()) { return 0; }
	const FVector2D Base = ImpactXY(Frame, 0);
	const float BaseS = (float)(Base.X * Line.X + Base.Y * Line.Y);
	return FMath::RoundToInt((AtS - BaseS) / PeriodAlongLine);
}

int32 UWaveGeometrySubsystem::CurrentFrame()
{
	IsReady();
	if (AWaveHeight* WH = WaveHeight.Get()) { return WH->GetCurrentFrameFromWaterController(); }
	return 0;
}

FWaveGeoSample UWaveGeometrySubsystem::Sample(const FVector& WorldPos, int32 Frame)
{
	FWaveGeoSample R;
	if (!HasModel()) { return R; }
	R.bValid = true;
	R.S = S(WorldPos);
	R.C = C(WorldPos);
	// PeelSpeed is cm/frame; the wave runs at 24 frames/s (193 frames = 8.0 s, M1).
	const float FramesPerSecond = 24.0f;
	const float PeelCmPerSec = PeelSpeed * FramesPerSecond;
	const FWaveGeoModel& M = Model;

	// The wave whose impact point this point is BEHIND (kb: its peel has passed here, its whitewater
	// is what is here) and the one it is AHEAD of (ka = the next wave up the line, still to break
	// here). Both are real at the same time: the bore of kb is a lump drifting shoreward as it ages,
	// and the swell of ka comes up behind it - so the bore band is tested first, then ka's face.
	const int32 kNear = NearestWaveIndex(Frame, R.S);
	const FVector2D INear = ImpactXY(Frame, kNear);
	const float sNear = (float)(INear.X * Line.X + INear.Y * Line.Y);
	// Wave index k is one loop further along the line per step, i.e. k+1 broke here one loop EARLIER
	// and its impact is PeriodS further down the line (for PeelSpeed > 0).
	const int32 dir = (PeelSpeed > 0.0f) ? 1 : -1;
	const int32 kb = (sNear >= R.S) ? kNear : kNear + dir;   // the wave whose impact is down-line of us
	const int32 ka = kb - dir;                                // the next wave up the line
	auto Fill = [&](int32 k)
	{
		const FVector2D I = ImpactXY(Frame, k);
		R.ImpactWorld = FVector(I.X, I.Y, WorldPos.Z);
		R.ImpactS = (float)(I.X * Line.X + I.Y * Line.Y);
		R.ImpactC = (float)(I.X * Back.X + I.Y * Back.Y);
		R.DistBehindImpact = R.ImpactS - R.S;
		R.CrossFromImpact  = R.C - R.ImpactC;
		R.SecondsSinceBroken = (FMath::Abs(PeelCmPerSec) > KINDA_SMALL_NUMBER) ? R.DistBehindImpact / PeelCmPerSec : 0.0f;
	};

	// Whitewater test for wave k: fills R with k and returns true when the point is behind k's pocket
	// and inside k's bore band for its age (the band starts BoreFrontStart shoreward of the impact
	// and drifts shoreward at BoreDriftRate as the bore ages).
	// Broken fades over the last BoreFrontFade of the band's shoreward edge (0 at the edge), so a
	// board riding out of the bore gets its drives back gradually, not in one tick.
	float frontFade = 1.0f;
	auto InBoreBand = [&](int32 k) -> bool
	{
		Fill(k);
		if (R.DistBehindImpact <= M.PocketBehind) { return false; }
		const float a = FMath::Max(0.0f, R.SecondsSinceBroken);
		const float front = -FMath::Min(M.BoreFrontSpeed * a, M.BoreFrontStart + M.BoreDriftRate * a);   // shoreward edge
		const float back  =  M.ImpactToCrestC  - M.BoreDriftRate * a;   // back edge
		const float fade  = FMath::Max(1.0f, M.BoreFrontFade);
		frontFade = FMath::SmoothStep(front - fade, front, R.CrossFromImpact);
		return R.CrossFromImpact >= front - fade && R.CrossFromImpact <= back;
	};
	auto Whitewater = [&]()
	{
		R.Zone = EWaveZone::Whitewater;
		R.Broken = FMath::SmoothStep(M.PocketBehind, M.PocketBehind + FMath::Max(1.0f, M.BrokenRamp), R.DistBehindImpact) * frontFade;
	};
	// The wave before kb: broke here one loop earlier, its bore is a period further along its drift.
	// A point shoreward of kb's face can still be in THAT bore - and the instant kb's impact passes
	// a point, kb changes identity, so without this test the previous bore vanished on that frame
	// (a whitewater/flat flicker at the impact, broken-wave M15/T2) and stayed forgotten after.
	const int32 kPrev = kb + dir;

	// 1. Behind kb, inside its bore band -> whitewater (or the pocket right at the impact).
	Fill(kb);
	const float age = FMath::Max(0.0f, R.SecondsSinceBroken);
	const float boreFront = -FMath::Min(M.BoreFrontSpeed * age, M.BoreFrontStart + M.BoreDriftRate * age);   // shoreward edge
	const float boreBack  =  M.ImpactToCrestC  - M.BoreDriftRate * age;          // back edge
	if (R.DistBehindImpact <= M.PocketBehind)
	{
		if (R.CrossFromImpact > M.ImpactToCrestC + M.BehindMargin) { R.Zone = EWaveZone::Behind; return R; }
		if (R.CrossFromImpact < -M.FaceExtent)
		{
			if (InBoreBand(kPrev)) { Whitewater(); return R; }
			Fill(kb);
			R.Zone = EWaveZone::Flat;
			return R;
		}
		R.Zone = EWaveZone::Pocket;
		return R;
	}
	if (R.CrossFromImpact >= boreFront - FMath::Max(1.0f, M.BoreFrontFade) && R.CrossFromImpact <= boreBack)
	{
		frontFade = FMath::SmoothStep(boreFront - FMath::Max(1.0f, M.BoreFrontFade), boreFront, R.CrossFromImpact);
		Whitewater();
		return R;
	}

	// 2. Not in kb's bore band, and the bore is still young: the bore's back / the trough behind it, or
	//    the flat in front of it - unless the previous wave's bore is still passing through here.
	//    The next wave's face has not re-formed here yet.
	if (age < M.ShoulderAfterSeconds)
	{
		if (R.CrossFromImpact <= boreBack && InBoreBand(kPrev)) { Whitewater(); return R; }
		Fill(kb);
		R.Zone = (R.CrossFromImpact > boreBack) ? EWaveZone::Behind : EWaveZone::Flat;
		return R;
	}

	// 3. The bore is old, the face has re-formed: this point is ahead of ka - its face, its pocket,
	//    over its back, or the flat.
	Fill(ka);
	if (R.CrossFromImpact > M.ImpactToCrestC + M.BehindMargin) { R.Zone = EWaveZone::Behind; return R; }
	if (R.CrossFromImpact < -M.FaceExtent)                     { R.Zone = EWaveZone::Flat;   return R; }
	R.Zone = (R.DistBehindImpact >= -M.PocketAhead) ? EWaveZone::Pocket : EWaveZone::Shoulder;
	return R;
}

const TCHAR* UWaveGeometrySubsystem::ZoneName(EWaveZone Z)
{
	switch (Z)
	{
	case EWaveZone::Behind:     return TEXT("behind");
	case EWaveZone::Flat:       return TEXT("flat");
	case EWaveZone::Shoulder:   return TEXT("shoulder");
	case EWaveZone::Pocket:     return TEXT("pocket");
	case EWaveZone::Whitewater: return TEXT("whitewater");
	default:                    return TEXT("unknown");
	}
}
