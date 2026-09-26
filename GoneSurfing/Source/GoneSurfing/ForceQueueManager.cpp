// Fill out your copyright notice in the Description page of Project Settings.

#include "ForceQueueManager.h"
#include "SurfLog.h"

void ForceQueueManager::EnqueueForce(
	std::deque<std::deque<FVector>>& queue,
	FVector force,
	int numberOfChunks)
{
	// Divide the force into equal chunks
	FVector forceChunk = force / numberOfChunks;

	UE_LOG(LogSurf, Warning, TEXT("ForceQueueManager::EnqueueForce - Original force: (%.2f, %.2f, %.2f), numberOfChunks: %d"),
		force.X, force.Y, force.Z, numberOfChunks);
	UE_LOG(LogSurf, Warning, TEXT("  -> Force chunk (divided): (%.2f, %.2f, %.2f)"),
		forceChunk.X, forceChunk.Y, forceChunk.Z);

	// Create an inner queue to hold all chunks for this force
	std::deque<FVector> innerQueue;
	for (int i = 0; i < numberOfChunks; i++)
	{
		innerQueue.push_back(forceChunk);
	}

	// Add the inner queue to the outer queue
	queue.push_back(innerQueue);

	UE_LOG(LogSurf, Warning, TEXT("  -> Queue now has %d force sequences"), (int)queue.size());
}

TArray<FVector> ForceQueueManager::GetEnqueuedForces(
	std::deque<std::deque<FVector>>& queue)
{
	UE_LOG(LogSurf, Warning, TEXT("ForceQueueManager::GetEnqueuedForces - Queue has %d force sequences"), (int)queue.size());

	TArray<FVector> forcesArray;

	// Get one chunk from each active force
	int sequenceIndex = 0;
	for (auto& innerQueue : queue)
	{
		if (!innerQueue.empty())
		{
			// Pop the front chunk and add to result
			FVector forceChunk = innerQueue.front();
			UE_LOG(LogSurf, Warning, TEXT("  -> Sequence %d: Popping chunk (%.2f, %.2f, %.2f), %d chunks remaining after pop"),
				sequenceIndex, forceChunk.X, forceChunk.Y, forceChunk.Z, (int)innerQueue.size() - 1);
			forcesArray.Add(forceChunk);
			innerQueue.pop_front();
		}
		sequenceIndex++;
	}

	// Remove any inner queues that are now empty
	int removedCount = 0;
	queue.erase(
		std::remove_if(queue.begin(), queue.end(),
			[&removedCount](const std::deque<FVector>& q) {
				if (q.empty()) {
					removedCount++;
					return true;
				}
				return false;
			}),
		queue.end()
	);

	UE_LOG(LogSurf, Warning, TEXT("  -> Removed %d empty sequences, returning %d forces, %d sequences remain"),
		removedCount, forcesArray.Num(), (int)queue.size());

	return forcesArray;
}
