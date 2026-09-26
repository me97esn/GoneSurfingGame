// See SurfBoards.h and specs/board-selection.md.

#include "SurfBoards.h"

#include "SurfLog.h"
#include "SurfDebug.h"
#include "SurfTuningSubsystem.h"

#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/App.h"
#include "Misc/Parse.h"
#include "Misc/CommandLine.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// Boards_ prefix: duplicate anonymous-namespace names across .cpp files break the Android unity
	// build, which desktop builds never catch.
	const TCHAR* Boards_ConfigSection = TEXT("SurfBoards");
	const TCHAR* Boards_ConfigKey     = TEXT("ActiveBoardId");

	// Board shown when nothing else picks one — no -Board= argument and no saved selection.
	// The foamie is the beginner board and the first thing a new player should be handed; the rack
	// is ordered easiest to hardest and this is its first entry. Reverted from the `tune-the-boards`
	// development default of "shortboard" for release (2026-09-24), see specs/release-checklist.md.
	// A saved ActiveBoardId still wins over this, so a machine that has already picked a board keeps
	// its choice — only a fresh install sees the change.
	const TCHAR* Boards_DefaultId     = TEXT("foamie");

	/** Name of the render-only component that carries a board's look. Found by name on re-apply so
	 *  a board switch reuses the one component instead of stacking a new mesh every time. */
	const TCHAR* Boards_VisualComponentName = TEXT("BoardVisualMesh");

	FString Boards_Dir()
	{
		return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Boards"));
	}

	float Boards_GetNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, float Fallback)
	{
		double Value = 0.0;
		return (Obj.IsValid() && Obj->TryGetNumberField(Field, Value)) ? (float)Value : Fallback;
	}
}

void USurfBoardSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LoadProfiles();
	LoadSelection();

	UE_LOG(LogSurf, Display, TEXT("SurfBoardSubsystem: %d board profile(s) from %s; active='%s'"),
		Profiles.Num(), *Boards_Dir(),
		GetActiveProfile() ? *GetActiveProfile()->Id : TEXT("<none>"));
}

void USurfBoardSubsystem::LoadProfiles()
{
	Profiles.Reset();

	const FString Dir = Boards_Dir();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Dir / TEXT("*.json")), /*Files*/ true, /*Directories*/ false);

	// No boards directory is not an error. It is the pre-feature game: compiled defaults, no board
	// applied, everything exactly as it was.
	if (Files.Num() == 0)
	{
		UE_LOG(LogSurf, Display, TEXT("SurfBoardSubsystem: no board profiles in %s; running on compiled defaults"), *Dir);
		return;
	}

	// Filename order IS rack order, and the numeric prefix orders the rack easiest to hardest
	// (difficulty rating 1..5), not longest to shortest. Board length is already visible in the
	// drawn outline; what the row has to tell the player is where to start and where to go next.
	Files.Sort();
	for (const FString& File : Files)
	{
		FSurfBoardProfile Profile;
		if (LoadProfileFile(Dir / File, Profile))
		{
			Profiles.Add(MoveTemp(Profile));
		}
	}
}

bool USurfBoardSubsystem::LoadProfileFile(const FString& Path, FSurfBoardProfile& Out) const
{
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *Path))
	{
		UE_LOG(LogSurf, Warning, TEXT("SurfBoardSubsystem: cannot read %s"), *Path);
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		// Malformed profile is skipped, never fatal — a typo on the phone must not brick the game.
		UE_LOG(LogSurf, Warning, TEXT("SurfBoardSubsystem: cannot parse %s; skipped"), *Path);
		return false;
	}

	// TryGet, not Get: a missing field must fall back to the filename, not log an error.
	if (!Root->TryGetStringField(TEXT("id"), Out.Id) || Out.Id.IsEmpty())
	{
		Out.Id = FPaths::GetBaseFilename(Path);
	}
	Root->TryGetStringField(TEXT("displayName"), Out.DisplayName);
	Root->TryGetStringField(TEXT("tagline"), Out.Tagline);
	Root->TryGetStringField(TEXT("description"), Out.Description);
	Root->TryGetStringField(TEXT("visualMesh"), Out.VisualMeshPath);
	Root->TryGetStringField(TEXT("visualMaterial"), Out.VisualMaterialPath);

	if (Out.DisplayName.IsEmpty())
	{
		Out.DisplayName = Out.Id;
	}

	int32 Level = -1;
	Out.AssistLevel   = Root->TryGetNumberField(TEXT("assistLevel"), Level) ? Level : -1;
	Out.VisualScale       = Boards_GetNumber(Root, TEXT("visualScale"), 1.0f);
	Out.SurferDeckOffsetZ = Boards_GetNumber(Root, TEXT("surferDeckOffsetZ"), 0.0f);

	Root->TryGetStringField(TEXT("outline"), Out.OutlineShapeId);

	const TSharedPtr<FJsonObject>* RatingsObj = nullptr;
	if (Root->TryGetObjectField(TEXT("ratings"), RatingsObj) && RatingsObj)
	{
		int32 V = 0;
		Out.RatingSpeed      = (*RatingsObj)->TryGetNumberField(TEXT("speed"), V) ? V : 0;
		Out.RatingTurning    = (*RatingsObj)->TryGetNumberField(TEXT("turning"), V) ? V : 0;
		Out.RatingDifficulty = (*RatingsObj)->TryGetNumberField(TEXT("difficulty"), V) ? V : 0;
	}

	const TSharedPtr<FJsonObject>* TuningObj = nullptr;
	if (Root->TryGetObjectField(TEXT("tuning"), TuningObj) && TuningObj)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*TuningObj)->Values)
		{
			double Number = 0.0;
			if (Pair.Value.IsValid() && Pair.Value->TryGetNumber(Number))
			{
				Out.Tuning.Add(FName(*Pair.Key), (float)Number);
			}
		}
	}

	UE_LOG(LogSurf, Display,
		TEXT("SurfBoardSubsystem: loaded board '%s' (%s) - outline='%s', assistLevel=%d, %d tuning override(s), ratings %d/%d/%d"),
		*Out.Id, *Out.DisplayName, *Out.OutlineShapeId, Out.AssistLevel, Out.Tuning.Num(),
		Out.RatingSpeed, Out.RatingTurning, Out.RatingDifficulty);
	return true;
}

const FSurfBoardProfile* USurfBoardSubsystem::GetProfile(int32 Index) const
{
	return Profiles.IsValidIndex(Index) ? &Profiles[Index] : nullptr;
}

FString USurfBoardSubsystem::GetBoardName(int32 Index) const
{
	const FSurfBoardProfile* P = GetProfile(Index);
	return P ? P->DisplayName : FString();
}

FString USurfBoardSubsystem::GetOutlineShapeId(int32 Index) const
{
	const FSurfBoardProfile* P = GetProfile(Index);
	return P ? P->OutlineShapeId : FString();
}

void USurfBoardSubsystem::SetActiveIndex(int32 Index)
{
	if (!Profiles.IsValidIndex(Index) || Index == ActiveIndex)
	{
		return;
	}
	ActiveIndex = Index;
	SaveSelection();
	UE_LOG(LogSurf, Display, TEXT("SurfBoardSubsystem: board set to '%s'"), *Profiles[ActiveIndex].Id);
}

void USurfBoardSubsystem::ApplyTuning(const UObject* WorldContext) const
{
	USurfTuningSubsystem* Tuning = SurfTuning::Get(WorldContext);
	if (!Tuning)
	{
		return;
	}
	const FSurfBoardProfile* Profile = GetActiveProfile();

	// An empty map with an empty id is meaningful: it returns the subsystem to compiled defaults plus
	// the global file, which is what a run with no boards installed wants - and it makes the HUD fall
	// back to writing that global file, as it did before boards existed.
	Tuning->ApplyBoardBaseline(Profile ? Profile->Tuning : TMap<FName, float>(),
		Profile ? Profile->Id : FString());
}

void USurfBoardSubsystem::SaveSelection() const
{
	if (!Profiles.IsValidIndex(ActiveIndex) || !GConfig)
	{
		return;
	}
	GConfig->SetString(Boards_ConfigSection, Boards_ConfigKey, *Profiles[ActiveIndex].Id, GGameIni);
	GConfig->Flush(false, GGameIni);
}

void USurfBoardSubsystem::LoadSelection()
{
	// Start from the configured default rather than index 0, so the fallback board is a named
	// choice instead of whatever sorts first. -Board= and a saved selection both still win.
	ActiveIndex = 0;
	for (int32 i = 0; i < Profiles.Num(); ++i)
	{
		if (Profiles[i].Id == Boards_DefaultId)
		{
			ActiveIndex = i;
			break;
		}
	}

	// -Board=<id> beats the saved selection, so a run can be pinned to one board without touching
	// the player's config (and without leaving it changed afterwards).
	FString SavedId = SurfBoards::CommandLineBoardId();
	if (!SavedId.IsEmpty())
	{
		UE_LOG(LogSurf, Display, TEXT("SurfBoardSubsystem: -Board=%s overrides the saved selection"), *SavedId);
	}
	else if (!GConfig || !GConfig->GetString(Boards_ConfigSection, Boards_ConfigKey, SavedId, GGameIni))
	{
		return;
	}
	// By Id, not index: adding or reordering a board must not silently move the player onto a
	// different one.
	for (int32 i = 0; i < Profiles.Num(); ++i)
	{
		if (Profiles[i].Id == SavedId)
		{
			ActiveIndex = i;
			return;
		}
	}
	UE_LOG(LogSurf, Display, TEXT("SurfBoardSubsystem: saved board '%s' no longer exists; falling back to first"), *SavedId);
}

namespace SurfBoards
{
	USurfBoardSubsystem* Get(const UObject* WorldContext)
	{
		if (!WorldContext) return nullptr;
		const UWorld* World = WorldContext->GetWorld();
		if (!World) return nullptr;
		UGameInstance* GI = World->GetGameInstance();
		if (!GI) return nullptr;
		return GI->GetSubsystem<USurfBoardSubsystem>();
	}

	bool ForcedInTests()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("BoardInTests"));
	}

	FString CommandLineBoardId()
	{
		FString Id;
		FParse::Value(FCommandLine::Get(), TEXT("Board="), Id);
		return Id;
	}

	bool IsScriptedRun(bool bExternalWeightOverride)
	{
		// FApp::IsUnattended() FIRST, and it is the one that actually does the work here.
		//
		// The cvar check alone loses a race the headless runner always wins: surf.autopilots arrives
		// via -ExecCmds a frame AFTER the pawn's BeginPlay, so at the moment a board would be applied
		// the filter still reads empty. Measured 2026-08-31 — a `::surf-straight` run applied the
		// soft-top and retuned five coefficients under a baseline recorded on compiled defaults,
		// which is precisely the silent-regression failure FR8 exists to prevent.
		//
		// -unattended is on every RunGameAndCollectLogs launch and is set from process start, so it
		// closes the race. Same gate, same reasoning, as the Start screen and fall detection.
		return FApp::IsUnattended()
			|| bExternalWeightOverride
			|| SurfDebug::CVarAutopilots.GetValueOnGameThread().Len() > 0;
	}

	void ApplyVisuals(AStaticMeshActor* BoardActor, const FSurfBoardProfile& Profile)
	{
		if (!BoardActor)
		{
			return;
		}
		UStaticMeshComponent* PhysicsMesh = BoardActor->GetStaticMeshComponent();
		if (!PhysicsMesh)
		{
			return;
		}

		UStaticMesh* VisualMesh = Profile.VisualMeshPath.IsEmpty()
			? nullptr
			: LoadObject<UStaticMesh>(nullptr, *Profile.VisualMeshPath);

		if (!VisualMesh)
		{
			// Leave the physics mesh visible: a board that cannot be seen is worse than a board that
			// looks like the wrong one.
			if (!Profile.VisualMeshPath.IsEmpty())
			{
				UE_LOG(LogSurf, Warning,
					TEXT("SurfBoards: visualMesh '%s' for board '%s' did not resolve; keeping the physics mesh visible"),
					*Profile.VisualMeshPath, *Profile.Id);
			}
			PhysicsMesh->SetVisibility(true, /*bPropagateToChildren*/ false);
			return;
		}

		// Reuse the component across switches rather than stacking one per board.
		UStaticMeshComponent* Visual = nullptr;
		TArray<USceneComponent*> Children;
		PhysicsMesh->GetChildrenComponents(/*bIncludeAllDescendants*/ false, Children);
		for (USceneComponent* Child : Children)
		{
			if (Child && Child->GetFName() == FName(Boards_VisualComponentName))
			{
				Visual = Cast<UStaticMeshComponent>(Child);
				break;
			}
		}

		if (!Visual)
		{
			Visual = NewObject<UStaticMeshComponent>(BoardActor, FName(Boards_VisualComponentName));
			if (!Visual)
			{
				return;
			}
			// Mobility and attachment before RegisterComponent — SetupAttachment is only valid on an
			// unregistered component, and mobility cannot change once a body exists.
			Visual->SetMobility(EComponentMobility::Movable);
			Visual->SetupAttachment(PhysicsMesh);
			// Render-only. No collision, no physics, no contribution to mass or inertia — the
			// physics body's convex hull and MassInKgOverride stay the single authority.
			Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Visual->SetCollisionProfileName(TEXT("NoCollision"));
			Visual->SetGenerateOverlapEvents(false);
			Visual->RegisterComponent();
		}

		Visual->SetStaticMesh(VisualMesh);
		Visual->SetRelativeScale3D(FVector(Profile.VisualScale));
		Visual->SetRelativeLocation(FVector(0.0f, 0.0f, 0.0f));
		Visual->SetVisibility(true, false);

		if (!Profile.VisualMaterialPath.IsEmpty())
		{
			if (UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *Profile.VisualMaterialPath))
			{
				const int32 NumSlots = Visual->GetNumMaterials();
				for (int32 Slot = 0; Slot < NumSlots; ++Slot)
				{
					Visual->SetMaterial(Slot, Mat);
				}
			}
			else
			{
				UE_LOG(LogSurf, Warning, TEXT("SurfBoards: visualMaterial '%s' for board '%s' did not resolve"),
					*Profile.VisualMaterialPath, *Profile.Id);
			}
		}

		// Hide the physics body's own rendering. Not propagated to children, so the surfer riding on
		// it — and the visual component just attached — stay visible.
		PhysicsMesh->SetVisibility(false, /*bPropagateToChildren*/ false);

		// The swap hides the board everyone can see and shows a replacement. If the replacement is
		// unregistered, invisible or zero-sized, the failure is an INVISIBLE BOARD — and on a mesh
		// identical to the one just hidden there is nothing else to notice. So report the state that
		// distinguishes "swapped" from "vanished", where a headless log can reach it.
		const FBoxSphereBounds B = Visual->Bounds;
		UE_LOG(LogSurf, Display,
			TEXT("SurfBoards: visuals applied for board '%s' (mesh=%s, scale=%.3f) - visual registered=%d visible=%d radius=%.1fcm | physics mesh hidden=%d"),
			*Profile.Id, *Profile.VisualMeshPath, Profile.VisualScale,
			Visual->IsRegistered() ? 1 : 0,
			Visual->IsVisible() ? 1 : 0,
			(float)B.SphereRadius,
			PhysicsMesh->IsVisible() ? 0 : 1);
	}
}
