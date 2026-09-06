// A spawn data generator that needs no EQS query or Zone Graph: entities land
// on explicit points, or at random inside a disc around the spawner.

#pragma once

#include "CoreMinimal.h"
#include "MassEntitySpawnDataGeneratorBase.h"
#include "McpMassRadiusSpawnGenerator.generated.h"

UCLASS(BlueprintType, EditInlineNew, meta = (DisplayName = "McpLink Radius / Points"))
class UMcpMassRadiusSpawnGenerator : public UMassEntitySpawnDataGeneratorBase
{
	GENERATED_BODY()

public:
	/** Explicit spawn points (world space). When set, entities cycle through them and Radius is ignored. */
	UPROPERTY(EditAnywhere, Category = "Points")
	TArray<FVector> Points;

	/** Radius of the disc around the spawner that random points are drawn from. */
	UPROPERTY(EditAnywhere, Category = "Points", meta = (ClampMin = "0"))
	float Radius = 500.f;

	/** Height offset from the spawner for random points. */
	UPROPERTY(EditAnywhere, Category = "Points")
	float HeightOffset = 0.f;

	/** Random yaw per entity. */
	UPROPERTY(EditAnywhere, Category = "Points")
	bool bRandomYaw = true;

	virtual void Generate(UObject& QueryOwner, TConstArrayView<FMassSpawnedEntityType> EntityTypes, int32 Count,
		FFinishedGeneratingSpawnDataSignature& FinishedGeneratingSpawnPointsDelegate) const override;
};
