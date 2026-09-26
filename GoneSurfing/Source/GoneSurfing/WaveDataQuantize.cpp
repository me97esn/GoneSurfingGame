// 16-bit fixed-point on-disk layout for the two big wave data tables.
//
// wave_unified_data (heights / normals / velocities per frame) and white-water-points-data (foam
// positions per frame) were 188 MB of the 406 MB cooked content: TArray<float> written verbatim and
// TArray<FVector>, which in UE5 is DOUBLE precision, 24 bytes per foam point. Nothing that reads them
// needs that: the board never sees anything finer than a fraction of a centimetre.
//
// Each array is stored as (Min, Scale, uint16[]) with Value = Min + Q * Scale, Scale = (Max-Min)/65535.
// That is uniform resolution across the array's range - for heights spanning a few thousand cm it
// is ~0.05 cm everywhere, where an IEEE half would step by a whole centimetre above 1024. Normals
// (range 2) resolve to ~3e-5.
//
// Only the bytes on disk change. The structs keep their float / FVector members, so WaveHeight.cpp,
// ParticleSystemsController.cpp and the DataTable editor are untouched, and the JSON re-import path
// still produces floats that get quantized on the next save. Assets saved before this existed carry
// no custom-version entry and fall back to the ordinary tagged-property load, so the source tables
// in Content/data keep working unchanged; the cooker reads them that way and writes the packed form.

#include "WaveHeight.h"
#include "Serialization/CustomVersion.h"

namespace WaveDataQuantize
{
	enum Type : int32
	{
		BeforeCustomVersionWasAdded = 0,
		// Arrays stored as 16-bit fixed point.
		Quantized16 = 1,

		VersionPlusOne,
		LatestVersion = VersionPlusOne - 1
	};

	const FGuid GUID(0x5A1F0C3D, 0x7B2E4F91, 0x8C6D2A55, 0x3E9B7D14);
	FCustomVersionRegistration GRegisterVersion(GUID, LatestVersion, TEXT("GoneSurfingWaveData"));

	// Whether this archive carries the packed layout. Saving to disk always writes it; loading
	// reads it only from assets saved after the version was introduced (older assets have no entry,
	// CustomVer() == -1). Everything that is not a persistent save/load - undo transactions,
	// reference collectors, memory counting, duplication - goes through the reflected properties.
	static bool UsesPackedLayout(FArchive& Ar)
	{
		Ar.UsingCustomVersion(GUID);
		if (!Ar.IsPersistent() || Ar.IsTransacting())
		{
			return false;
		}
		return Ar.IsSaving() || Ar.CustomVer(GUID) >= Quantized16;
	}

	static void SerializePacked(FArchive& Ar, TArray<float>& Values)
	{
		if (Ar.IsSaving())
		{
			float Min = 0.0f, Max = 0.0f;
			bool bAny = false;
			for (float V : Values)
			{
				if (!FMath::IsFinite(V)) continue;
				if (!bAny) { Min = Max = V; bAny = true; }
				else { Min = FMath::Min(Min, V); Max = FMath::Max(Max, V); }
			}
			float Scale = (Max - Min) / 65535.0f;
			const float InvScale = Scale > 0.0f ? 1.0f / Scale : 0.0f;

			TArray<uint16> Packed;
			Packed.Reserve(Values.Num());
			for (float V : Values)
			{
				// A non-finite sample (none expected) lands on Min rather than poisoning the range.
				const float T = FMath::IsFinite(V) ? (V - Min) * InvScale : 0.0f;
				Packed.Add(static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(T), 0, 65535)));
			}
			Ar << Min << Scale << Packed;
		}
		else
		{
			float Min = 0.0f, Scale = 0.0f;
			TArray<uint16> Packed;
			Ar << Min << Scale << Packed;
			Values.SetNumUninitialized(Packed.Num());
			for (int32 i = 0; i < Packed.Num(); ++i)
			{
				Values[i] = Min + Packed[i] * Scale;
			}
		}
	}

	static void SerializePacked(FArchive& Ar, TArray<FVector>& Points)
	{
		TArray<float> X, Y, Z;
		if (Ar.IsSaving())
		{
			X.Reserve(Points.Num()); Y.Reserve(Points.Num()); Z.Reserve(Points.Num());
			for (const FVector& P : Points)
			{
				X.Add(static_cast<float>(P.X));
				Y.Add(static_cast<float>(P.Y));
				Z.Add(static_cast<float>(P.Z));
			}
		}
		SerializePacked(Ar, X);
		SerializePacked(Ar, Y);
		SerializePacked(Ar, Z);
		if (Ar.IsLoading())
		{
			Points.SetNumUninitialized(X.Num());
			for (int32 i = 0; i < X.Num(); ++i)
			{
				Points[i] = FVector(X[i], Y[i], Z[i]);
			}
		}
	}
}

bool FWaveUnifiedFrameData::Serialize(FArchive& Ar)
{
	if (!WaveDataQuantize::UsesPackedLayout(Ar))
	{
		StaticStruct()->SerializeTaggedProperties(Ar, reinterpret_cast<uint8*>(this), StaticStruct(), nullptr);
		return true;
	}
	for (TArray<float>* Array : { &h, &nx, &ny, &nz, &vx, &vy, &vz })
	{
		WaveDataQuantize::SerializePacked(Ar, *Array);
	}
	return true;
}

bool FWavePointsDataStruct2::Serialize(FArchive& Ar)
{
	if (!WaveDataQuantize::UsesPackedLayout(Ar))
	{
		StaticStruct()->SerializeTaggedProperties(Ar, reinterpret_cast<uint8*>(this), StaticStruct(), nullptr);
		return true;
	}
	WaveDataQuantize::SerializePacked(Ar, Positions);
	return true;
}
