#include "SurfRails.h"

namespace SurfRails
{
	namespace
	{
		// One board per playable level, and the rails are a single global phase of the ride, so a
		// module-level flag is sufficient — no per-actor plumbing through ~20 FluidDynamics actors,
		// 4 Buoyancy actors, SharedCalculations and WeightDistribution.
		// Named (not anonymous-namespace free functions) to stay clear of the Android unity-build
		// duplicate-symbol trap that bites same-named helpers across .cpp files.
		bool GForcesSuppressed = false;
	}

	bool AreForcesSuppressed()
	{
		return GForcesSuppressed;
	}

	void SetForcesSuppressed(bool bSuppressed)
	{
		GForcesSuppressed = bSuppressed;
	}

	bool IsKinematicallyDriven()
	{
		return GForcesSuppressed;
	}
}
