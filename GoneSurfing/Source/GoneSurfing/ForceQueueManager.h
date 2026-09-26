// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include <deque>

/**
 * Static utility class for managing force queues that spread forces over multiple frames.
 *
 * This class provides shared logic for enqueueing forces and retrieving them in chunks
 * across multiple frames, allowing smooth force application without code duplication.
 */
class GONESURFING_API ForceQueueManager
{
public:
	/**
	 * Enqueues a force to be spread over multiple frames.
	 *
	 * @param queue The force queue to add to (owned by the calling actor)
	 * @param force The total force vector to spread across frames
	 * @param numberOfChunks How many frames to spread the force over
	 */
	static void EnqueueForce(
		std::deque<std::deque<FVector>>& queue,
		FVector force,
		int numberOfChunks
	);

	/**
	 * Gets the force chunks to apply this frame (one chunk from each active force).
	 *
	 * Returns one chunk from each queued force and removes empty queues.
	 * The calling code should apply these forces at the actor's current location.
	 *
	 * @param queue The force queue to retrieve from (owned by the calling actor)
	 * @return Array of force vectors to apply this frame
	 */
	static TArray<FVector> GetEnqueuedForces(
		std::deque<std::deque<FVector>>& queue
	);
};
