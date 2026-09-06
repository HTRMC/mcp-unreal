#include "McpMassRadiusSpawnGenerator.h"

#include "GameFramework/Actor.h"
#include "MassSpawnLocationProcessor.h"
#include "MassSpawnerTypes.h"
#include "Math/RandomStream.h"

void UMcpMassRadiusSpawnGenerator::Generate(UObject& QueryOwner, TConstArrayView<FMassSpawnedEntityType> EntityTypes, int32 Count,
	FFinishedGeneratingSpawnDataSignature& FinishedGeneratingSpawnPointsDelegate) const
{
	TArray<FMassEntitySpawnDataGeneratorResult> Results;
	if (Count <= 0)
	{
		FinishedGeneratingSpawnPointsDelegate.Execute(Results);
		return;
	}
	BuildResultsFromEntityTypes(Count, EntityTypes, Results);

	FVector Origin = FVector::ZeroVector;
	if (const AActor* Owner = Cast<AActor>(&QueryOwner))
	{
		Origin = Owner->GetActorLocation();
	}
	FRandomStream Random(GetRandomSelectionSeed());
	int32 PointIndex = 0;
	for (FMassEntitySpawnDataGeneratorResult& Result : Results)
	{
		Result.SpawnDataProcessor = UMassSpawnLocationProcessor::StaticClass();
		Result.SpawnData.InitializeAs<FMassTransformsSpawnData>();
		FMassTransformsSpawnData& Transforms = Result.SpawnData.GetMutable<FMassTransformsSpawnData>();
		Transforms.Transforms.Reserve(Result.NumEntities);
		for (int32 Index = 0; Index < Result.NumEntities; ++Index)
		{
			FVector Location;
			if (Points.Num() > 0)
			{
				Location = Points[PointIndex++ % Points.Num()];
			}
			else
			{
				const float Angle = Random.FRandRange(0.f, 2.f * PI);
				const float Distance = Radius * FMath::Sqrt(Random.FRand());
				Location = Origin + FVector(FMath::Cos(Angle) * Distance, FMath::Sin(Angle) * Distance, HeightOffset);
			}
			const FRotator Rotation(0.f, bRandomYaw ? Random.FRandRange(0.f, 360.f) : 0.f, 0.f);
			Transforms.Transforms.Emplace(Rotation, Location);
		}
	}
	FinishedGeneratingSpawnPointsDelegate.Execute(Results);
}
