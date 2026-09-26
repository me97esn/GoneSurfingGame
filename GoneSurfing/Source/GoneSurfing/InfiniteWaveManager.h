// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GridLODActor.h"
#include "InfiniteWaveManager.generated.h"

/**
 * Struct to track managed GridLODActor instances
 */
USTRUCT(BlueprintType)
struct FWaveActorInfo
{
	GENERATED_BODY()

	/** Reference to the managed GridLODActor */
	UPROPERTY()
	TObjectPtr<AGridLODActor> Actor;

	/** Current position index in the infinite grid */
	int32 PositionIndex;

	/** Current world position */
	FVector WorldPosition;

	/** Current frame offset for this actor */
	int32 FrameOffset;

	FWaveActorInfo()
		: Actor(nullptr)
		, PositionIndex(0)
		, WorldPosition(FVector::ZeroVector)
		, FrameOffset(0)
	{}
};

/**
 * Manages infinite side-scrolling wave system by repositioning GridLODActor instances
 * Never spawns or destroys actors - only repositions manually-placed instances
 */
UCLASS(Blueprintable, BlueprintType)
class GONESURFING_API AInfiniteWaveManager : public AActor
{
	GENERATED_BODY()

public:
	AInfiniteWaveManager();

protected:
	virtual void BeginPlay() override;

public:
	virtual void Tick(float DeltaTime) override;

	// ========== Configuration ==========

	/** Manually-placed GridLODActor instances to manage (assign in Blueprint) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Wave|Setup")
	TArray<TObjectPtr<AGridLODActor>> ManagedGridActors;

	/** Camera actor to track (usually player camera) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Wave|Setup")
	TObjectPtr<AActor> CameraActor;

	/** Horizontal spacing between adjacent actors (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Wave|Layout")
	float ActorSpacing;

	/** Vertical offset added per position index (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Wave|Layout")
	float YOffsetPerActor;

	/** Frame offset increment per position index */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Wave|Animation")
	int32 FrameOffsetPerActor;

	/** Distance behind camera before actor gets repositioned (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Wave|Behavior")
	float RepositionDistance;

	/** Enable debug visualization */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Infinite Wave|Debug")
	bool bShowDebugVisualization;

	// ========== Blueprint Events ==========

	/** Called when an actor is repositioned */
	UFUNCTION(BlueprintImplementableEvent, Category = "Infinite Wave|Events")
	void OnActorRepositioned(AGridLODActor* Actor, int32 NewPositionIndex);

	// ========== Public Methods ==========

	/** Initialize tracking info from manually-placed actors */
	UFUNCTION(BlueprintCallable, Category = "Infinite Wave")
	void InitializeFromManagedActors();

	/**
	 * The mesh tiling as the LEVEL defines it: average world step and frame step between consecutive
	 * placed GridLODActors. Reads only actor transforms and FrameOffset, so it is valid at any point
	 * in BeginPlay order — which is why it is static: AWaveHeight calls it to register the physics
	 * height data onto the same tiling (specs/wave-mesh-data-registration.md), and this manager calls
	 * it to configure itself. Returns false with fewer than two valid actors.
	 */
	static bool InferTileLayout(const TArray<TObjectPtr<AGridLODActor>>& Actors,
		float& OutActorSpacing, float& OutYOffsetPerActor, int32& OutFrameOffsetPerActor);

	/** Force update all actor positions based on camera */
	UFUNCTION(BlueprintCallable, Category = "Infinite Wave")
	void UpdateActorPositions();

	/** Get current position index range (min, max) */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Infinite Wave")
	void GetPositionIndexRange(int32& OutMinIndex, int32& OutMaxIndex) const;

protected:
	// ========== Internal State ==========

	/** Tracking info for each managed actor */
	UPROPERTY()
	TArray<FWaveActorInfo> ActorInfos;

	/** Last camera X position (to detect backward movement) */
	float LastCameraX;

	/** Reference position for position index 0 (middle actor's position) */
	FVector ReferencePosition;

	/** Reference frame offset for position index 0 (middle actor's frame offset) */
	int32 ReferenceFrameOffset;

	// ========== Internal Methods ==========

	/** Calculate world position from position index */
	FVector CalculateWorldPosition(int32 PositionIndex) const;

	/** Calculate frame offset from position index */
	int32 CalculateFrameOffset(int32 PositionIndex, AGridLODActor* Actor) const;

	/** Reposition actor to new position index */
	void RepositionActor(FWaveActorInfo& ActorInfo, int32 NewPositionIndex);

	/** Draw debug visualization */
	void DrawDebugInfo(float CameraY);
};
