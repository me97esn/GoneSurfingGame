// Round-trips the real wave tables through the packed 16-bit layout (WaveDataQuantize.cpp) and
// reports the worst error per array plus the byte saving. The desktop game loads the source assets
// through the tagged-property fallback, so this test is the only thing on a dev machine that
// exercises the packed LOAD path the cooked build relies on.
//
//   UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests GoneSurfing.WaveData" -unattended
//       -nopause -nullrhi -testexit="Automation Test Queue Empty"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/DataTable.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "SurfLog.h"
#include "WaveHeight.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWaveDataQuantizeTest, "GoneSurfing.WaveData.Quantize16",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
{
	struct FArrayStats
	{
		float Min = TNumericLimits<float>::Max();
		float Max = TNumericLimits<float>::Lowest();
		float MaxError = 0.0f;
		int64 Count = 0;

		void Add(float Original, float Restored)
		{
			Min = FMath::Min(Min, Original);
			Max = FMath::Max(Max, Original);
			MaxError = FMath::Max(MaxError, FMath::Abs(Original - Restored));
			++Count;
		}
		// The layout guarantees half a quantum; anything above that is a bug, not precision. The
		// slack is float rounding: the reconstruction Min + Q * Scale and, for FVector, the
		// double -> float cast, both a few ULPs of the range.
		float Allowed() const { return (Max - Min) / 65535.0f * 0.5f + FMath::Max(FMath::Abs(Max), FMath::Abs(Min)) * 1e-5f; }
	};

	template <typename TRow>
	bool RoundTrip(const TRow& In, TRow& Out, int64& OutPackedBytes)
	{
		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes, /*bIsPersistent*/ true);
		const_cast<TRow&>(In).Serialize(Writer);
		OutPackedBytes = Bytes.Num();

		FMemoryReader Reader(Bytes, /*bIsPersistent*/ true);
		Reader.SetCustomVersions(Writer.GetCustomVersions());
		Out.Serialize(Reader);
		return !Reader.IsError();
	}
}

bool FWaveDataQuantizeTest::RunTest(const FString& Parameters)
{
	// ---- wave_unified_data: 7 float arrays per frame --------------------------------------------
	{
		UDataTable* Table = LoadObject<UDataTable>(nullptr, TEXT("/Game/data/wave_unified_data.wave_unified_data"));
		if (!TestNotNull(TEXT("wave_unified_data loads"), Table)) return false;

		static const TCHAR* Names[7] = { TEXT("h"), TEXT("nx"), TEXT("ny"), TEXT("nz"), TEXT("vx"), TEXT("vy"), TEXT("vz") };
		FArrayStats Stats[7];
		int64 FloatBytes = 0, PackedBytes = 0;
		int32 Rows = 0;

		for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
		{
			const FWaveUnifiedFrameData& In = *reinterpret_cast<const FWaveUnifiedFrameData*>(Pair.Value);
			FWaveUnifiedFrameData Out;
			int64 RowPacked = 0;
			if (!TestTrue(FString::Printf(TEXT("row %s round-trips"), *Pair.Key.ToString()), RoundTrip(In, Out, RowPacked))) return false;
			PackedBytes += RowPacked;

			const TArray<float>* InArrays[7]  = { &In.h, &In.nx, &In.ny, &In.nz, &In.vx, &In.vy, &In.vz };
			const TArray<float>* OutArrays[7] = { &Out.h, &Out.nx, &Out.ny, &Out.nz, &Out.vx, &Out.vy, &Out.vz };
			for (int32 a = 0; a < 7; ++a)
			{
				if (!TestEqual(FString::Printf(TEXT("row %s.%s count"), *Pair.Key.ToString(), Names[a]), OutArrays[a]->Num(), InArrays[a]->Num())) return false;
				FloatBytes += InArrays[a]->Num() * sizeof(float);
				for (int32 i = 0; i < InArrays[a]->Num(); ++i)
				{
					Stats[a].Add((*InArrays[a])[i], (*OutArrays[a])[i]);
				}
			}
			++Rows;
		}

		UE_LOG(LogSurf, Display, TEXT("Quantize16 wave_unified_data: %d rows, %.1f MB as float -> %.1f MB packed"),
			Rows, FloatBytes / 1e6, PackedBytes / 1e6);
		for (int32 a = 0; a < 7; ++a)
		{
			UE_LOG(LogSurf, Display, TEXT("  %-3s range [%10.3f, %10.3f]  max error %.5f  (allowed %.5f)"),
				Names[a], Stats[a].Min, Stats[a].Max, Stats[a].MaxError, Stats[a].Allowed());
			TestTrue(FString::Printf(TEXT("%s error within half a quantum"), Names[a]), Stats[a].MaxError <= Stats[a].Allowed());
		}
		TestTrue(TEXT("packed is at most 55%% of float"), PackedBytes < FloatBytes * 0.55);
	}

	// ---- white-water-points-data-rotX: FVector (double) positions per frame -----------------------
	{
		UDataTable* Table = LoadObject<UDataTable>(nullptr, TEXT("/Game/data/white-water-points-data-rotX.white-water-points-data-rotX"));
		if (!TestNotNull(TEXT("white-water-points-data-rotX loads"), Table)) return false;

		FArrayStats Stats[3];
		int64 VectorBytes = 0, PackedBytes = 0;
		int32 Rows = 0;

		for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
		{
			const FWavePointsDataStruct2& In = *reinterpret_cast<const FWavePointsDataStruct2*>(Pair.Value);
			FWavePointsDataStruct2 Out;
			int64 RowPacked = 0;
			if (!TestTrue(FString::Printf(TEXT("row %s round-trips"), *Pair.Key.ToString()), RoundTrip(In, Out, RowPacked))) return false;
			PackedBytes += RowPacked;
			if (!TestEqual(FString::Printf(TEXT("row %s count"), *Pair.Key.ToString()), Out.Positions.Num(), In.Positions.Num())) return false;
			VectorBytes += In.Positions.Num() * sizeof(FVector);
			for (int32 i = 0; i < In.Positions.Num(); ++i)
			{
				Stats[0].Add(In.Positions[i].X, Out.Positions[i].X);
				Stats[1].Add(In.Positions[i].Y, Out.Positions[i].Y);
				Stats[2].Add(In.Positions[i].Z, Out.Positions[i].Z);
			}
			++Rows;
		}

		UE_LOG(LogSurf, Display, TEXT("Quantize16 white-water-points: %d rows, %.1f MB as FVector -> %.1f MB packed"),
			Rows, VectorBytes / 1e6, PackedBytes / 1e6);
		static const TCHAR* Axis[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };
		for (int32 a = 0; a < 3; ++a)
		{
			UE_LOG(LogSurf, Display, TEXT("  %s range [%10.3f, %10.3f]  max error %.5f  (allowed %.5f)"),
				Axis[a], Stats[a].Min, Stats[a].Max, Stats[a].MaxError, Stats[a].Allowed());
			TestTrue(FString::Printf(TEXT("%s error within half a quantum"), Axis[a]), Stats[a].MaxError <= Stats[a].Allowed());
		}
		TestTrue(TEXT("packed is at most 30%% of FVector"), PackedBytes < VectorBytes * 0.30);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
