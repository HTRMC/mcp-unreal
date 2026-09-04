// Landscape height encoding and the terrain operations the route wraps.
//
// Heights are centimetres of world Z relative to the landscape actor; the
// uint16 storage (32768 as the actor's own Z, 1/128 of a scaled unit per step)
// never leaves this file.
#pragma once

#include "CoreMinimal.h"

class ALandscape;
class UWorld;

namespace McpLink::Landscapes
{
	double HeightToWorld(uint16 Raw, double ScaleZ);
	uint16 WorldToHeight(double World, double ScaleZ);

	/// The resolution a landscape can actually be built at.
	bool IsValidSectionSize(int32 QuadsPerSection);
	bool IsValidSectionCount(int32 SectionsPerComponent);

	struct FCreateParams
	{
		FVector Location = FVector::ZeroVector;
		FVector Scale = FVector(100.0);
		int32 QuadsPerSection = 63;
		int32 SectionsPerComponent = 1;
		int32 ComponentsX = 1;
		int32 ComponentsY = 1;
		double Height = 0.0;
	};

	/// Spawn and import a landscape, the way the New Landscape panel does.
	ALandscape* Create(UWorld* World, const FCreateParams& Params, FString& OutError);

	/// Heights in cm for a vertex rect, row-major from Y1.
	bool GetHeights(
		ALandscape& Landscape, int32 X1, int32 Y1, int32 X2, int32 Y2,
		TArray<double>& OutHeights, FString& OutError);

	/// Write one height in cm per vertex, and rebuild the collision the scene
	/// traces against.
	bool SetHeights(
		ALandscape& Landscape, int32 X1, int32 Y1, int32 X2, int32 Y2,
		const TArray<double>& Heights, FString& OutError);
}
