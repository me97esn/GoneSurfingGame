// Board selection: the board IS the difficulty setting. See specs/board-selection.md.
//
// User testing found that riding with full assist "feels like surfing a foamie" and riding with none
// "feels like a shortboard". The assist was already producing a BOARD, not a helper — players just
// had no name for it. Naming it removes every UX problem the assist had: nothing to explain (a
// forgiving beginner board explains itself), nothing to threshold (the player switches when they
// want to), nothing to miss (the board is visibly different all ride).
//
// Three rules this file keeps:
//
//   1. A board is a JSON file, not an asset. Content/Boards/<id>.json — text, diffable, editable on
//      the phone without a rebuild. Every physics decision in this project has been made by editing
//      numbers on a device and riding; a UDataAsset would have put these numbers somewhere that loop
//      cannot reach.
//
//   2. The physics GEOMETRY never varies. Same collision hull, same mass, same inertia, same ~20
//      sampler positions, on every board. Only coefficients, assist level and appearance change.
//      This is what makes the whole feature cheap — and it is enough, because the two feels this is
//      built on were produced by coefficient-and-assist differences alone, on one physical board.
//
//   3. Applying a board is idempotent. This subsystem lives on the GAME INSTANCE and survives the
//      level reload a restart performs, so "apply" always means "reset to baseline, then set",
//      never "accumulate".
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SurfBoards.generated.h"

class AStaticMeshActor;

/** Which drawn silhouette a board uses: an id from BoardOutlineShapes.inc ("foamie", "fish", ...).
 *
 *  The shapes are the artist's real outlines, generated from images/"surfboards outline.svg" by
 *  Tools/svg2boards.py, and they all share one scale - so a foamie really is 1.56x a shortboard and
 *  the rack shows that without a word of copy. An unknown id draws nothing rather than guessing. */

/** One board. Everything that distinguishes it from another board lives here. */
USTRUCT(BlueprintType)
struct FSurfBoardProfile
{
	GENERATED_BODY()

	/** Filename stem, and the key the selection is persisted under. */
	UPROPERTY(BlueprintReadOnly, Category="Board")
	FString Id;

	UPROPERTY(BlueprintReadOnly, Category="Board")
	FString DisplayName;

	/** One line, shown under the name in the dialog. */
	UPROPERTY(BlueprintReadOnly, Category="Board")
	FString Tagline;

	/** The dialog body. Plain language — what the board DOES, never pitch/roll/yaw or coefficient
	 *  names. A player who has to be taught the physics vocabulary to read a board description has
	 *  been handed the same problem the assist card had. */
	UPROPERTY(BlueprintReadOnly, Category="Board")
	FString Description;

	/** Fixed assist level, 0..SurfAssist::kAssistLevelCount-1. -1 leaves the assist alone.
	 *  Fixed, never fading: a board whose feel changes over time is not a board. */
	UPROPERTY(BlueprintReadOnly, Category="Board")
	int32 AssistLevel = -1;

	/** Object paths, soft-loaded. A rename fails at runtime rather than compile time, hence the
	 *  "keep the physics mesh visible" fallback in ApplyVisuals. */
	UPROPERTY(BlueprintReadOnly, Category="Board")
	FString VisualMeshPath;

	UPROPERTY(BlueprintReadOnly, Category="Board")
	FString VisualMaterialPath;

	/** Uniform scale on the visual mesh. Exists to bring an imported mesh into the +/-10% length
	 *  band the fixed sampler positions allow — NOT to make a genuinely longer board. Past that the
	 *  nose and tail stick out beyond where any force is sampled and spray spawns inboard of the
	 *  tips. */
	UPROPERTY(BlueprintReadOnly, Category="Board")
	float VisualScale = 1.0f;

	/** Small correction so the surfer's feet stay planted if the visual deck sits slightly high or
	 *  low. Deck thickness is held equal across boards, so this is a trim, not a lift. */
	UPROPERTY(BlueprintReadOnly, Category="Board")
	float SurferDeckOffsetZ = 0.0f;

	/** Id into GBoardShapes. Defaults empty, which draws nothing - a board with no outline is a
	 *  visible authoring mistake rather than a silently wrong silhouette. */
	UPROPERTY(BlueprintReadOnly, Category="Board")
	FString OutlineShapeId;

	// ---- The rack's three ratings, 0..5 pips each.
	//
	// These are a PROMISE the physics has to keep: once a card says Speed 5, a board that is not
	// faster is a lie the player feels within one wave. They are authored alongside the tuning block
	// in the same file precisely so the two cannot drift apart unnoticed.
	//
	// Difficulty is not independent - it falls out of turning minus assist - but it is the axis the
	// player chooses along, so it is stated rather than inferred.
	UPROPERTY(BlueprintReadOnly, Category="Board")
	int32 RatingSpeed = 0;

	UPROPERTY(BlueprintReadOnly, Category="Board")
	int32 RatingTurning = 0;

	UPROPERTY(BlueprintReadOnly, Category="Board")
	int32 RatingDifficulty = 0;

	/** USurfTuningSubsystem property names -> values. Same namespace as
	 *  Saved/TuningOverrides.json, validated the same way, unknown keys logged and skipped. */
	TMap<FName, float> Tuning;

	bool IsValid() const { return !Id.IsEmpty(); }
};

UCLASS()
class GONESURFING_API USurfBoardSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/** Every profile that loaded, in Id order so the buttons keep a stable position between runs. */
	const TArray<FSurfBoardProfile>& GetProfiles() const { return Profiles; }

	UFUNCTION(BlueprintCallable, Category="Board")
	int32 GetBoardCount() const { return Profiles.Num(); }

	UFUNCTION(BlueprintCallable, Category="Board")
	int32 GetActiveIndex() const { return ActiveIndex; }

	UFUNCTION(BlueprintCallable, Category="Board")
	FString GetBoardName(int32 Index) const;

	/** Null when the index is out of range, or when no profile loaded at all — in which case the
	 *  game runs exactly as it did before this feature existed. */
	const FSurfBoardProfile* GetProfile(int32 Index) const;
	const FSurfBoardProfile* GetActiveProfile() const { return GetProfile(ActiveIndex); }

	/** Select a board and persist the choice. Does NOT apply it — the caller decides when, because
	 *  applying mid-ride would change the board's feel under a planing player. */
	void SetActiveIndex(int32 Index);

	/** Push the active profile's tuning block into USurfTuningSubsystem as the new baseline.
	 *  Safe to call repeatedly. */
	void ApplyTuning(const UObject* WorldContext) const;

	/** Outline shape id for a board, or empty when the index is invalid, so UMG never null-checks. */
	UFUNCTION(BlueprintCallable, Category="Board")
	FString GetOutlineShapeId(int32 Index) const;

private:
	void LoadProfiles();
	bool LoadProfileFile(const FString& Path, FSurfBoardProfile& Out) const;

	/** Persisted to GGameIni under [SurfBoards], by Id rather than index so reordering or adding a
	 *  board does not silently move the player onto a different one. */
	void SaveSelection() const;
	void LoadSelection();

	TArray<FSurfBoardProfile> Profiles;
	int32 ActiveIndex = 0;
};

namespace SurfBoards
{
	/** The subsystem for any world context, or null outside a running game instance. */
	GONESURFING_API USurfBoardSubsystem* Get(const UObject* WorldContext);

	/** Swap the board's APPEARANCE only.
	 *
	 *  The actor's own StaticMeshComponent stays the physics body for ever — same convex hull, same
	 *  mass override, same component every AFluidDynamics actor pushes impulses into — and merely
	 *  stops rendering. A second, collision-free component carries the look.
	 *
	 *  SetVisibility(false) deliberately does NOT propagate to children, which is what keeps the
	 *  surfer skeletal mesh riding on the board visible while the board mesh under them is hidden.
	 *
	 *  No-ops safely if the actor is null or the mesh path does not resolve; in the latter case the
	 *  physics mesh is left visible so the player still has a board to look at. */
	GONESURFING_API void ApplyVisuals(AStaticMeshActor* BoardActor, const FSurfBoardProfile& Profile);

	/** True during snapshot / trace-replay runs, where no board may be applied: the baselines were
	 *  recorded on compiled defaults and a board would silently retune the physics under them.
	 *  Same convention as fall detection and the assist. */
	GONESURFING_API bool IsScriptedRun(bool bExternalWeightOverride);

	/** `-BoardInTests`: apply the board even inside a filtered/unattended run. TESTS ONLY, and never
	 *  while recording a baseline — the whole point of the FR8 gate is that baselines ride compiled
	 *  defaults. It exists because the only way to MEASURE that a board's tuning actually reaches the
	 *  physics is to run an autopilot that turns, and autopilots only run in scripted runs.
	 *
	 *  A command-line switch rather than a CVar, deliberately: `-ExecCmds` CVars arrive a frame after
	 *  BeginPlay, which is where a board is applied — the same race that made the first cut of the
	 *  FR8 gate fail open. Command-line args are readable in the first frame. */
	GONESURFING_API bool ForcedInTests();

	/** `-Board=<id>`: force a specific board for this run, ahead of the saved selection. Also read
	 *  from the command line, for the same reason. Empty = use the saved selection. */
	GONESURFING_API FString CommandLineBoardId();
}
