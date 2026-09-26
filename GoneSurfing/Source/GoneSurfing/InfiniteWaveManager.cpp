// Copyright Epic Games, Inc. All Rights Reserved.

#include "InfiniteWaveManager.h"
#include "SurfLog.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Pawn.h"
#include "DrawDebugHelpers.h"
#include "NiagaraComponent.h"

AInfiniteWaveManager::AInfiniteWaveManager()
	: ActorSpacing(8000.0f)
	, YOffsetPerActor(0.0f)
	, FrameOffsetPerActor(80)
	, RepositionDistance(10000.0f)
	, bShowDebugVisualization(false)
	, LastCameraX(0.0f)
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics; // Update before physics/rendering
}

void AInfiniteWaveManager::BeginPlay()
{
	Super::BeginPlay();

	// Auto-find camera if not set
	if (!CameraActor)
	{
		APlayerController* PC = GetWorld()->GetFirstPlayerController();
		if (PC && PC->GetPawn())
		{
			CameraActor = PC->GetPawn();
			UE_LOG(LogSurf, Log, TEXT("InfiniteWaveManager: Auto-assigned CameraActor to player pawn"));
		}
	}

	// Initialize tracking from manually-placed actors (also infers spacing)
	InitializeFromManagedActors();

	// Validate configuration (after initialization has inferred values)
	if (ActorInfos.Num() > 0 && ActorSpacing > 0)
	{
		float RequiredCoverage = 2.0f * RepositionDistance;
		float ProvidedCoverage = (ActorInfos.Num() - 1) * ActorSpacing;

		if (ProvidedCoverage < RequiredCoverage)
		{
			UE_LOG(LogSurf, Warning, TEXT("InfiniteWaveManager: INSUFFICIENT COVERAGE! Need %.0f units but only have %.0f units."),
				RequiredCoverage, ProvidedCoverage);
			UE_LOG(LogSurf, Warning, TEXT("  Solutions: 1) Add more actors (need %d total, currently have %d)"),
				FMath::CeilToInt(RequiredCoverage / ActorSpacing) + 1, ActorInfos.Num());
			UE_LOG(LogSurf, Warning, TEXT("  OR 2) Manually set RepositionDistance to %.0f or less"),
				ProvidedCoverage / 2.0f);
		}
		else
		{
			UE_LOG(LogSurf, Log, TEXT("InfiniteWaveManager: Configuration OK - Coverage %.0f units >= Required %.0f units"),
				ProvidedCoverage, RequiredCoverage);
		}
	}

	// Store initial camera position
	if (CameraActor)
	{
		LastCameraX = CameraActor->GetActorLocation().Y;
	}
}

void AInfiniteWaveManager::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!CameraActor || ActorInfos.Num() == 0)
	{
		return;
	}

	float CameraY = CameraActor->GetActorLocation().Y;

	// Find the farthest actor behind camera (if any)
	FWaveActorInfo* FarthestBehind = nullptr;
	float MaxDistanceBehind = 0.0f;

	// Find the farthest actor ahead of camera (if any)
	FWaveActorInfo* FarthestAhead = nullptr;
	float MaxDistanceAhead = 0.0f;

	for (FWaveActorInfo& ActorInfo : ActorInfos)
	{
		if (!ActorInfo.Actor)
		{
			continue;
		}

		float ActorY = ActorInfo.Actor->GetActorLocation().Y;
		float DistanceBehind = CameraY - ActorY;

		// Check if behind camera
		if (DistanceBehind > RepositionDistance)
		{
			if (DistanceBehind > MaxDistanceBehind)
			{
				MaxDistanceBehind = DistanceBehind;
				FarthestBehind = &ActorInfo;
			}
		}
		// Check if too far ahead (for backward movement)
		else if (DistanceBehind < -RepositionDistance)
		{
			float DistanceAhead = -DistanceBehind;
			if (DistanceAhead > MaxDistanceAhead)
			{
				MaxDistanceAhead = DistanceAhead;
				FarthestAhead = &ActorInfo;
			}
		}
	}

	// Only reposition ONE actor per tick - the farthest one that exceeds RepositionDistance
	// Prioritize based on which actor is farther out of range (regardless of direction)
	// This ensures gaps are filled even during slow camera movement

	// Determine which actor to reposition based on distance magnitude
	bool bShouldRepositionBehind = FarthestBehind && (MaxDistanceBehind > MaxDistanceAhead);
	bool bShouldRepositionAhead = FarthestAhead && (MaxDistanceAhead >= MaxDistanceBehind);

	if (bShouldRepositionBehind)
	{
		// Find max position index
		int32 MaxPositionIndex = FarthestBehind->PositionIndex;
		for (const FWaveActorInfo& Info : ActorInfos)
		{
			if (Info.PositionIndex > MaxPositionIndex)
			{
				MaxPositionIndex = Info.PositionIndex;
			}
		}

		int32 NewPositionIndex = MaxPositionIndex + 1;
		UE_LOG(LogSurf, Log, TEXT("InfiniteWaveManager: Repositioning farthest behind actor (was at PosIdx %d, %.1f units behind camera)"),
			FarthestBehind->PositionIndex, MaxDistanceBehind);
		RepositionActor(*FarthestBehind, NewPositionIndex);
	}
	else if (bShouldRepositionAhead)
	{
		// Find min position index
		int32 MinPositionIndex = FarthestAhead->PositionIndex;
		for (const FWaveActorInfo& Info : ActorInfos)
		{
			if (Info.PositionIndex < MinPositionIndex)
			{
				MinPositionIndex = Info.PositionIndex;
			}
		}

		int32 NewPositionIndex = MinPositionIndex - 1;
		UE_LOG(LogSurf, Log, TEXT("InfiniteWaveManager: Repositioning farthest ahead actor (was at PosIdx %d, %.1f units ahead of camera)"),
			FarthestAhead->PositionIndex, MaxDistanceAhead);
		RepositionActor(*FarthestAhead, NewPositionIndex);
	}
	else if (FarthestBehind || FarthestAhead)
	{
		// We have actors out of range but didn't reposition - this means insufficient coverage
		UE_LOG(LogSurf, Warning, TEXT("InfiniteWaveManager: Not enough actors to cover camera range! Consider adding more actors or increasing ActorSpacing. Camera at Y=%.1f"), CameraY);
	}

	// Update last camera position
	LastCameraX = CameraY;

	// Debug visualization
	if (bShowDebugVisualization)
	{
		DrawDebugInfo(CameraY);
	}
}

bool AInfiniteWaveManager::InferTileLayout(const TArray<TObjectPtr<AGridLODActor>>& Actors,
	float& OutActorSpacing, float& OutYOffsetPerActor, int32& OutFrameOffsetPerActor)
{
	// First pass: read positions and frame offsets
	TArray<FVector> Positions;
	TArray<int32> FrameOffsets;
	TArray<int32> AnimLengths;

	for (int32 i = 0; i < Actors.Num(); ++i)
	{
		AGridLODActor* Actor = Actors[i];
		if (!Actor)
		{
			UE_LOG(LogSurf, Warning, TEXT("InfiniteWaveManager: ManagedGridActors[%d] is null, skipping"), i);
			continue;
		}

		Positions.Add(Actor->GetActorLocation());
		FrameOffsets.Add(Actor->FrameOffset);
		AnimLengths.Add(Actor->EndFrame - Actor->StartFrame + 1);
	}

	if (Positions.Num() < 2)
	{
		return false;
	}

	// Calculate spacing by looking at Y-axis difference between actors
	float TotalYSpacing = 0.0f;
	float TotalXOffset = 0.0f;
	int32 TotalFrameOffsetDiff = 0;
	int32 ValidPairs = 0;

	for (int32 i = 1; i < Positions.Num(); ++i)
	{
		float YDiff = Positions[i].Y - Positions[i - 1].Y;
		float XDiff = Positions[i].X - Positions[i - 1].X;
		int32 FrameDiff = FrameOffsets[i] - FrameOffsets[i - 1];

		// Handle frame wrapping
		if (FrameDiff < 0)
		{
			FrameDiff += AnimLengths[i];
		}

		TotalYSpacing += YDiff;
		TotalXOffset += XDiff;
		TotalFrameOffsetDiff += FrameDiff;
		ValidPairs++;
	}

	// Calculate average spacing
	OutActorSpacing = TotalYSpacing / ValidPairs;
	OutYOffsetPerActor = TotalXOffset / ValidPairs;
	OutFrameOffsetPerActor = TotalFrameOffsetDiff / ValidPairs;
	return true;
}

void AInfiniteWaveManager::InitializeFromManagedActors()
{
	ActorInfos.Empty();

	if (ManagedGridActors.Num() == 0)
	{
		UE_LOG(LogSurf, Warning, TEXT("InfiniteWaveManager: No ManagedGridActors assigned!"));
		return;
	}

	if (ManagedGridActors.Num() < 2)
	{
		UE_LOG(LogSurf, Error, TEXT("InfiniteWaveManager: Need at least 2 actors to infer spacing! Only have %d"), ManagedGridActors.Num());
		return;
	}

	UE_LOG(LogSurf, Log, TEXT("InfiniteWaveManager: Initializing %d actors and inferring configuration"), ManagedGridActors.Num());

	// Infer the tiling from the placed actors (shared with AWaveHeight, see InferTileLayout).
	if (!InferTileLayout(ManagedGridActors, ActorSpacing, YOffsetPerActor, FrameOffsetPerActor))
	{
		UE_LOG(LogSurf, Error, TEXT("InfiniteWaveManager: Need at least 2 valid actors!"));
		return;
	}

	UE_LOG(LogSurf, Log, TEXT("InfiniteWaveManager: Inferred configuration:"));
	UE_LOG(LogSurf, Log, TEXT("  ActorSpacing (Y-axis): %.1f"), ActorSpacing);
	UE_LOG(LogSurf, Log, TEXT("  YOffsetPerActor (X-axis): %.1f"), YOffsetPerActor);
	UE_LOG(LogSurf, Log, TEXT("  FrameOffsetPerActor: %d"), FrameOffsetPerActor);

	// Auto-calculate RepositionDistance if not manually set (default is 10000, so check if it's still default)
	if (FMath::IsNearlyEqual(RepositionDistance, 10000.0f))
	{
		// Set to 1.5x spacing to ensure proper coverage
		RepositionDistance = ActorSpacing * 1.5f;
		UE_LOG(LogSurf, Log, TEXT("  Auto-calculated RepositionDistance: %.1f"), RepositionDistance);
	}

	// Calculate middle index offset to center the actors around the first actor's position
	int32 MiddleIndex = ManagedGridActors.Num() / 2;
	UE_LOG(LogSurf, Log, TEXT("  Centering on middle actor (index %d)"), MiddleIndex);

	// Calculate offset needed to move middle actor to first actor's position
	FVector FirstActorPosition = ManagedGridActors[0]->GetActorLocation();
	FVector MiddleActorPosition = ManagedGridActors[MiddleIndex]->GetActorLocation();
	FVector PhysicalOffset = FirstActorPosition - MiddleActorPosition;

	UE_LOG(LogSurf, Log, TEXT("  Shifting all actors by offset: (%.1f, %.1f, %.1f)"),
		PhysicalOffset.X, PhysicalOffset.Y, PhysicalOffset.Z);

	// Store the reference position and frame offset (where middle actor will be after shift)
	ReferencePosition = FirstActorPosition;
	ReferenceFrameOffset = ManagedGridActors[MiddleIndex]->FrameOffset;
	UE_LOG(LogSurf, Log, TEXT("  Reference position (for index 0): (%.1f, %.1f, %.1f)"),
		ReferencePosition.X, ReferencePosition.Y, ReferencePosition.Z);
	UE_LOG(LogSurf, Log, TEXT("  Reference frame offset (for index 0): %d"), ReferenceFrameOffset);

	// Second pass: physically move all actors and create tracking info
	for (int32 i = 0; i < ManagedGridActors.Num(); ++i)
	{
		AGridLODActor* Actor = ManagedGridActors[i];
		if (!Actor)
		{
			continue;
		}

		// Physically shift the actor
		FVector NewPosition = Actor->GetActorLocation() + PhysicalOffset;
		Actor->SetActorLocation(NewPosition);

		FWaveActorInfo Info;
		Info.Actor = Actor;
		Info.WorldPosition = NewPosition;

		// Shift position index so middle actor is at index 0
		// Example with 3 actors: indices 0,1,2 become -1,0,1
		Info.PositionIndex = i - MiddleIndex;

		// Calculate and apply new frame offset based on position index
		Info.FrameOffset = CalculateFrameOffset(Info.PositionIndex, Actor);
		Actor->FrameOffset = Info.FrameOffset;

		// Update WorldPositionOffset for shader
		FVector2D Offset2D(NewPosition.X, NewPosition.Y);
		Actor->WorldPositionOffset = Offset2D;

		ActorInfos.Add(Info);

		UE_LOG(LogSurf, Log, TEXT("InfiniteWaveManager: Actor[%d] tracking at PositionIndex=%d, NewPos=(%.1f, %.1f, %.1f), FrameOffset=%d"),
			i, Info.PositionIndex, Info.WorldPosition.X, Info.WorldPosition.Y, Info.WorldPosition.Z, Info.FrameOffset);
	}
}

void AInfiniteWaveManager::UpdateActorPositions()
{
	for (FWaveActorInfo& ActorInfo : ActorInfos)
	{
		if (ActorInfo.Actor)
		{
			// Recalculate position and offset
			ActorInfo.WorldPosition = CalculateWorldPosition(ActorInfo.PositionIndex);
			ActorInfo.FrameOffset = CalculateFrameOffset(ActorInfo.PositionIndex, ActorInfo.Actor.Get());

			// Apply to actor
			ActorInfo.Actor->SetActorLocation(ActorInfo.WorldPosition);
			ActorInfo.Actor->FrameOffset = ActorInfo.FrameOffset;

			FVector2D Offset2D(ActorInfo.WorldPosition.X, ActorInfo.WorldPosition.Y);
			ActorInfo.Actor->WorldPositionOffset = Offset2D;
		}
	}
}

void AInfiniteWaveManager::GetPositionIndexRange(int32& OutMinIndex, int32& OutMaxIndex) const
{
	if (ActorInfos.Num() == 0)
	{
		OutMinIndex = 0;
		OutMaxIndex = 0;
		return;
	}

	OutMinIndex = ActorInfos[0].PositionIndex;
	OutMaxIndex = ActorInfos[0].PositionIndex;

	for (const FWaveActorInfo& Info : ActorInfos)
	{
		OutMinIndex = FMath::Min(OutMinIndex, Info.PositionIndex);
		OutMaxIndex = FMath::Max(OutMaxIndex, Info.PositionIndex);
	}
}

FVector AInfiniteWaveManager::CalculateWorldPosition(int32 PositionIndex) const
{
	// Calculate position relative to reference position (middle actor at index 0)
	float X = ReferencePosition.X + (PositionIndex * YOffsetPerActor);  // X offset (minor)
	float Y = ReferencePosition.Y + (PositionIndex * ActorSpacing);     // Y position (major, along movement axis)
	float Z = ReferencePosition.Z;                                      // Keep same Z as reference

	return FVector(X, Y, Z);
}

int32 AInfiniteWaveManager::CalculateFrameOffset(int32 PositionIndex, AGridLODActor* Actor) const
{
	if (!Actor)
	{
		return 0;
	}

	// Calculate frame offset relative to reference (middle actor)
	// PositionIndex 0 = ReferenceFrameOffset
	// PositionIndex 1 = ReferenceFrameOffset + FrameOffsetPerActor
	// PositionIndex -1 = ReferenceFrameOffset - FrameOffsetPerActor
	int32 Offset = ReferenceFrameOffset + (PositionIndex * FrameOffsetPerActor);

	// No wrapping here - let the GridLODActor handle wrapping based on its StartFrame/EndFrame
	return Offset;
}

void AInfiniteWaveManager::RepositionActor(FWaveActorInfo& ActorInfo, int32 NewPositionIndex)
{
	if (!ActorInfo.Actor)
	{
		return;
	}

	int32 OldPositionIndex = ActorInfo.PositionIndex;
	FVector OldWorldPosition = ActorInfo.WorldPosition;

	ActorInfo.PositionIndex = NewPositionIndex;

	// Recalculate position and frame offset
	ActorInfo.WorldPosition = CalculateWorldPosition(NewPositionIndex);
	ActorInfo.FrameOffset = CalculateFrameOffset(NewPositionIndex, ActorInfo.Actor.Get());

	// FIX FOR UE-209013: Deactivate Niagara components before moving to prevent duplicate spawns
	// When a Niagara component with active particles is moved, Unreal may trigger a burst spawn
	// at the old location. Deactivating first ensures clean state.
	TArray<UNiagaraComponent*> NiagaraComponents;
	ActorInfo.Actor->GetComponents<UNiagaraComponent>(NiagaraComponents);

	for (UNiagaraComponent* NiagaraComp : NiagaraComponents)
	{
		if (NiagaraComp && NiagaraComp->IsActive())
		{
			NiagaraComp->Deactivate();
		}
	}

	// Apply to actor
	ActorInfo.Actor->SetActorLocation(ActorInfo.WorldPosition);
	ActorInfo.Actor->FrameOffset = ActorInfo.FrameOffset;

	// Update WorldPositionOffset for shader
	FVector2D Offset2D(ActorInfo.WorldPosition.X, ActorInfo.WorldPosition.Y);
	ActorInfo.Actor->WorldPositionOffset = Offset2D;

	// Reactivate Niagara components after move is complete
	for (UNiagaraComponent* NiagaraComp : NiagaraComponents)
	{
		if (NiagaraComp)
		{
			NiagaraComp->Activate(true); // true = reset system to fresh state
		}
	}

	UE_LOG(LogSurf, Log, TEXT("InfiniteWaveManager: Repositioned actor from PositionIndex %d to %d (X: %.1f -> %.1f, Y: %.1f -> %.1f, FrameOffset %d, Niagara components: %d)"),
		OldPositionIndex, NewPositionIndex, OldWorldPosition.X, ActorInfo.WorldPosition.X,
		OldWorldPosition.Y, ActorInfo.WorldPosition.Y, ActorInfo.FrameOffset, NiagaraComponents.Num());

	// Fire Blueprint event
	OnActorRepositioned(ActorInfo.Actor.Get(), NewPositionIndex);
}

void AInfiniteWaveManager::DrawDebugInfo(float CameraY)
{
	if (!GetWorld())
	{
		return;
	}

	// Draw camera position line (perpendicular to Y-axis)
	FVector CameraPos(0, CameraY, GetActorLocation().Z);
	DrawDebugLine(GetWorld(), CameraPos + FVector(-5000, 0, 0), CameraPos + FVector(5000, 0, 0),
		FColor::Cyan, false, -1.0f, 0, 50.0f);

	// Draw reposition boundaries
	FVector ReposBackPos(0, CameraY - RepositionDistance, GetActorLocation().Z);
	FVector ReposFrontPos(0, CameraY + RepositionDistance, GetActorLocation().Z);

	DrawDebugLine(GetWorld(), ReposBackPos + FVector(-5000, 0, 0), ReposBackPos + FVector(5000, 0, 0),
		FColor::Yellow, false, -1.0f, 0, 25.0f);
	DrawDebugLine(GetWorld(), ReposFrontPos + FVector(-5000, 0, 0), ReposFrontPos + FVector(5000, 0, 0),
		FColor::Yellow, false, -1.0f, 0, 25.0f);

	// Draw actor positions and info
	for (const FWaveActorInfo& ActorInfo : ActorInfos)
	{
		if (!ActorInfo.Actor)
		{
			continue;
		}

		FVector ActorPos = ActorInfo.WorldPosition;
		float DistanceBehind = CameraY - ActorPos.Y;

		// Color code based on distance from camera
		FColor ActorColor = FColor::Green;
		if (DistanceBehind > RepositionDistance * 0.8f)
		{
			ActorColor = FColor::Red; // About to be repositioned
		}
		else if (DistanceBehind < 0)
		{
			ActorColor = FColor::Blue; // Ahead of camera
		}

		// Draw actor marker
		DrawDebugSphere(GetWorld(), ActorPos, 200.0f, 8, ActorColor, false, -1.0f, 0, 10.0f);

		// Draw info text
		FString InfoText = FString::Printf(TEXT("Idx:%d\nFrame:%d\nDist:%.0f"),
			ActorInfo.PositionIndex, ActorInfo.FrameOffset, DistanceBehind);
		DrawDebugString(GetWorld(), ActorPos + FVector(0, 0, 500), InfoText, nullptr, FColor::White, 0.0f, true);
	}
}
