// Landscape terrain and the foliage scatter that sits on it: height encoding,
// creating a landscape, sculpting it, and reading it back.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "McpLandscapeUtils.h"
#include "McpTestFlags.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpLandscapeTest, "McpLink.World.Landscape", McpTestFlags)
bool FMcpLandscapeTest::RunTest(const FString& Parameters)
{
	using namespace McpLink::Landscapes;

	// ---- height encoding: what crosses the wire is centimetres, not samples
	TestEqual(TEXT("the midpoint sample is the actor's own Z"), HeightToWorld(32768, 100.0), 0.0);
	TestEqual(TEXT("one sample is 1/128 of a scaled unit"),
		HeightToWorld(32768 + 128, 100.0), 100.0, 0.001);
	TestEqual(TEXT("zero centimetres round-trips"), WorldToHeight(0.0, 100.0), static_cast<uint16>(32768));
	for (const double Centimetres : {-5000.0, -250.0, 0.0, 100.0, 250.0, 5000.0})
	{
		TestEqual(*FString::Printf(TEXT("%.0f cm round-trips"), Centimetres),
			HeightToWorld(WorldToHeight(Centimetres, 100.0), 100.0), Centimetres, 0.5);
	}
	// The encoding saturates rather than wrapping around.
	TestEqual(TEXT("far above the range clamps"), WorldToHeight(1.0e9, 100.0), static_cast<uint16>(65535));
	TestEqual(TEXT("far below the range clamps"), WorldToHeight(-1.0e9, 100.0), static_cast<uint16>(0));
	TestEqual(TEXT("a zero Z scale does not divide by zero"),
		WorldToHeight(500.0, 0.0), static_cast<uint16>(32768));

	TestTrue(TEXT("63 is a valid section size"), IsValidSectionSize(63));
	TestFalse(TEXT("9 is not"), IsValidSectionSize(9));
	TestTrue(TEXT("4 sections per component is valid"), IsValidSectionCount(4));
	TestFalse(TEXT("2 is not"), IsValidSectionCount(2));

	// ---- a real landscape
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("McpLandscapeTestWorld"));
	if (!TestNotNull(TEXT("test world created"), World))
	{
		return false;
	}

	FString Error;
	FCreateParams Bad;
	Bad.QuadsPerSection = 9;
	TestNull(TEXT("an invalid section size is refused"), Create(World, Bad, Error));
	TestTrue(TEXT("section size message"), Error.Contains(TEXT("7, 15, 31, 63")));
	FCreateParams BadCount;
	BadCount.SectionsPerComponent = 3;
	TestNull(TEXT("an invalid section count is refused"), Create(World, BadCount, Error));

	FCreateParams Params;
	Params.QuadsPerSection = 7;
	Params.Height = 100.0;
	ALandscape* Landscape = Create(World, Params, Error);
	if (!TestNotNull(*FString::Printf(TEXT("landscape created: %s"), *Error), Landscape))
	{
		World->DestroyWorld(false);
		return false;
	}

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!TestNotNull(TEXT("landscape info exists"), Info))
	{
		World->DestroyWorld(false);
		return false;
	}
	int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	TestTrue(TEXT("the landscape has an extent"), Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY));
	// 7 quads per section, 1 section, 1 component -> 8 vertices a side.
	TestEqual(TEXT("vertices across"), MaxX - MinX + 1, 8);
	TestEqual(TEXT("vertices down"), MaxY - MinY + 1, 8);

	TArray<double> Read;
	if (!TestTrue(*FString::Printf(TEXT("heights read: %s"), *Error),
		GetHeights(*Landscape, MinX, MinY, MaxX, MaxY, Read, Error)))
	{
		World->DestroyWorld(false);
		return false;
	}
	TestEqual(TEXT("one height per vertex"), Read.Num(), 64);
	TestEqual(TEXT("the landscape starts at the height it was made with"), Read[0], 100.0, 1.0);

	// ---- sculpt a ramp and read it back
	TArray<double> Ramp;
	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			Ramp.Add(static_cast<double>(X - MinX) * 50.0);
		}
	}
	TestTrue(*FString::Printf(TEXT("ramp written: %s"), *Error),
		SetHeights(*Landscape, MinX, MinY, MaxX, MaxY, Ramp, Error));

	TArray<double> AfterRamp;
	TestTrue(TEXT("ramp read back"), GetHeights(*Landscape, MinX, MinY, MaxX, MaxY, AfterRamp, Error));
	if (TestEqual(TEXT("same vertex count"), AfterRamp.Num(), Ramp.Num()))
	{
		bool bMatches = true;
		for (int32 Index = 0; Index < Ramp.Num(); ++Index)
		{
			bMatches &= FMath::IsNearlyEqual(AfterRamp[Index], Ramp[Index], 1.0);
		}
		TestTrue(TEXT("every sculpted height survives the round trip"), bMatches);
	}

	// A sub-region reads the same values as the full read.
	TArray<double> Row;
	TestTrue(TEXT("a sub-region reads"),
		GetHeights(*Landscape, MinX, MinY, MinX + 3, MinY, Row, Error));
	TestEqual(TEXT("four samples"), Row.Num(), 4);
	TestEqual(TEXT("the ramp rises 50 cm a quad"), Row[3] - Row[0], 150.0, 1.0);

	// ---- the wrong number of samples is refused rather than half-applied
	TArray<double> TooFew = {1.0, 2.0};
	TestFalse(TEXT("a short height array is refused"),
		SetHeights(*Landscape, MinX, MinY, MaxX, MaxY, TooFew, Error));
	TestTrue(TEXT("count message names both numbers"),
		Error.Contains(TEXT("64")) && Error.Contains(TEXT("2")));

	// ---- foliage lives in the same world
	AInstancedFoliageActor* Foliage =
		AInstancedFoliageActor::Get(World, /*bCreateIfNone=*/true, World->GetCurrentLevel());
	if (TestNotNull(TEXT("foliage actor created with a level hint"), Foliage))
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
		if (TestNotNull(TEXT("a mesh to scatter"), Mesh))
		{
			UFoliageType* Type = nullptr;
			FFoliageInfo* FoliageInfo = Foliage->AddMesh(Mesh, &Type);
			if (TestNotNull(TEXT("foliage type made from the mesh"), FoliageInfo)
				&& TestNotNull(TEXT("and reported back"), Type))
			{
				TestEqual(TEXT("it starts empty"), FoliageInfo->Instances.Num(), 0);

				FFoliageInstance Instance;
				Instance.Location = FVector(100.0, 200.0, 300.0);
				Instance.DrawScale3D = FVector3f(2.0f);
				TArray<const FFoliageInstance*> ToAdd = {&Instance};
				FoliageInfo->AddInstances(Type, ToAdd);
				TestEqual(TEXT("one instance placed"), FoliageInfo->Instances.Num(), 1);
				TestEqual(TEXT("at the location it was given"),
					FoliageInfo->Instances[0].Location, FVector(100.0, 200.0, 300.0));

				const TArray<int32> Remove = {0};
				FoliageInfo->RemoveInstances(Remove, /*RebuildFoliageTree=*/true);
				TestEqual(TEXT("and removed again"), FoliageInfo->Instances.Num(), 0);
			}
		}
	}

	World->DestroyWorld(false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
