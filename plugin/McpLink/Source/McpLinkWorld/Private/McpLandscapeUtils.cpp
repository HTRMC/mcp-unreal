#include "McpLandscapeUtils.h"

#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeFileFormatInterface.h"
#include "LandscapeImportHelper.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "Misc/Paths.h"

namespace McpLink::Landscapes
{
	namespace
	{
		constexpr double HeightMidpoint = 32768.0;
		constexpr double HeightStep = 1.0 / 128.0;
	}

	double HeightToWorld(uint16 Raw, double ScaleZ)
	{
		return (static_cast<double>(Raw) - HeightMidpoint) * HeightStep * ScaleZ;
	}

	uint16 WorldToHeight(double World, double ScaleZ)
	{
		const double Raw = ScaleZ != 0.0
			? (World / (HeightStep * ScaleZ)) + HeightMidpoint
			: HeightMidpoint;
		return static_cast<uint16>(FMath::Clamp(FMath::RoundToDouble(Raw), 0.0, 65535.0));
	}

	bool IsValidSectionSize(int32 QuadsPerSection)
	{
		return QuadsPerSection == 7 || QuadsPerSection == 15 || QuadsPerSection == 31
			|| QuadsPerSection == 63 || QuadsPerSection == 127 || QuadsPerSection == 255;
	}

	bool IsValidSectionCount(int32 SectionsPerComponent)
	{
		return SectionsPerComponent == 1 || SectionsPerComponent == 4;
	}

	ALandscape* Create(UWorld* World, const FCreateParams& Params, FString& OutError)
	{
		if (World == nullptr)
		{
			OutError = TEXT("no world");
			return nullptr;
		}
		if (!IsValidSectionSize(Params.QuadsPerSection))
		{
			OutError = TEXT("quads_per_section must be 7, 15, 31, 63, 127 or 255");
			return nullptr;
		}
		if (!IsValidSectionCount(Params.SectionsPerComponent))
		{
			OutError = TEXT("sections_per_component must be 1 or 4");
			return nullptr;
		}
		if (Params.ComponentsX < 1 || Params.ComponentsY < 1)
		{
			OutError = TEXT("components_x and components_y must be at least 1");
			return nullptr;
		}

		const int32 QuadsPerComponent = Params.QuadsPerSection * Params.SectionsPerComponent;
		const int32 SizeX = Params.ComponentsX * QuadsPerComponent + 1;
		const int32 SizeY = Params.ComponentsY * QuadsPerComponent + 1;

		TArray<uint16> Heights;
		if (Params.RawHeights.IsEmpty())
		{
			Heights.Init(WorldToHeight(Params.Height, Params.Scale.Z), SizeX * SizeY);
		}
		else if (Params.RawHeights.Num() == SizeX * SizeY)
		{
			Heights = Params.RawHeights;
		}
		else
		{
			OutError = FString::Printf(
				TEXT("the landscape is %d x %d vertices, so the height data needs %d samples, not %d"),
				SizeX, SizeY, SizeX * SizeY, Params.RawHeights.Num());
			return nullptr;
		}
		TMap<FGuid, TArray<uint16>> HeightDataPerLayer;
		HeightDataPerLayer.Add(FGuid(), MoveTemp(Heights));
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayer;
		MaterialLayerDataPerLayer.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

		ALandscape* Landscape = World->SpawnActor<ALandscape>(Params.Location, FRotator::ZeroRotator);
		if (Landscape == nullptr)
		{
			OutError = TEXT("could not spawn a Landscape actor");
			return nullptr;
		}
		Landscape->SetActorRelativeScale3D(Params.Scale);
		Landscape->Import(
			FGuid::NewGuid(), 0, 0, SizeX - 1, SizeY - 1,
			Params.SectionsPerComponent, Params.QuadsPerSection,
			HeightDataPerLayer, nullptr, MaterialLayerDataPerLayer,
			ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());

		ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
		if (Info == nullptr)
		{
			OutError = TEXT("the landscape imported without producing landscape info");
			return nullptr;
		}
		Info->UpdateLayerInfoMap(Landscape);
		return Landscape;
	}

	bool GetHeights(
		ALandscape& Landscape, int32 X1, int32 Y1, int32 X2, int32 Y2,
		TArray<double>& OutHeights, FString& OutError)
	{
		ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
		if (Info == nullptr)
		{
			OutError = TEXT("this landscape has no landscape info");
			return false;
		}
		const int32 Width = X2 - X1 + 1;
		const int32 Rows = Y2 - Y1 + 1;
		if (Width <= 0 || Rows <= 0)
		{
			OutError = TEXT("the region is empty");
			return false;
		}

		TArray<uint16> Raw;
		Raw.SetNumZeroed(Width * Rows);
		{
			FLandscapeEditDataInterface Edit(Info);
			Edit.GetHeightDataFast(X1, Y1, X2, Y2, Raw.GetData(), Width);
		}

		const double ScaleZ = Landscape.GetActorScale3D().Z;
		OutHeights.Reset(Raw.Num());
		for (const uint16 Sample : Raw)
		{
			OutHeights.Add(HeightToWorld(Sample, ScaleZ));
		}
		return true;
	}

	bool SetHeights(
		ALandscape& Landscape, int32 X1, int32 Y1, int32 X2, int32 Y2,
		const TArray<double>& Heights, FString& OutError)
	{
		ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
		if (Info == nullptr)
		{
			OutError = TEXT("this landscape has no landscape info");
			return false;
		}
		const int32 Width = X2 - X1 + 1;
		const int32 Rows = Y2 - Y1 + 1;
		if (Width <= 0 || Rows <= 0)
		{
			OutError = TEXT("the region is empty");
			return false;
		}
		if (Heights.Num() != Width * Rows)
		{
			OutError = FString::Printf(
				TEXT("the region is %d x %d, so 'heights' needs %d values, not %d"),
				Width, Rows, Width * Rows, Heights.Num());
			return false;
		}

		const double ScaleZ = Landscape.GetActorScale3D().Z;
		TArray<uint16> Raw;
		Raw.Reserve(Heights.Num());
		for (const double World : Heights)
		{
			Raw.Add(WorldToHeight(World, ScaleZ));
		}
		return SetRawHeights(Landscape, X1, Y1, X2, Y2, Raw, OutError);
	}

	bool SetRawHeights(
		ALandscape& Landscape, int32 X1, int32 Y1, int32 X2, int32 Y2,
		const TArray<uint16>& Raw, FString& OutError)
	{
		ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
		if (Info == nullptr)
		{
			OutError = TEXT("this landscape has no landscape info");
			return false;
		}
		const int32 Width = X2 - X1 + 1;
		const int32 Rows = Y2 - Y1 + 1;
		if (Width <= 0 || Rows <= 0 || Raw.Num() != Width * Rows)
		{
			OutError = FString::Printf(
				TEXT("the region is %d x %d, which needs %d samples, not %d"),
				Width, Rows, Width * Rows, Raw.Num());
			return false;
		}

		Landscape.Modify();
		{
			FLandscapeEditDataInterface Edit(Info);
			Edit.SetHeightData(X1, Y1, X2, Y2, Raw.GetData(), Width, /*InCalcNormals=*/true);
		}
		Info->ForceLayersFullUpdate();
		// The heightfield the collision scene traces against is rebuilt separately
		// from the height data, so without this a scatter (or anything else that
		// line-traces) keeps hitting the old terrain.
		Landscape.RecreateCollisionComponents();
		return true;
	}

	bool ReadHeightmapFile(
		const FString& FilePath, TArray<uint16>& OutRaw, int32& OutWidth, int32& OutHeight,
		FString& OutError)
	{
		if (!FPaths::FileExists(FilePath))
		{
			OutError = FString::Printf(TEXT("no file at '%s'"), *FilePath);
			return false;
		}
		// The import helper is what the New Landscape panel's file picker
		// runs: it knows the registered heightmap formats (16-bit PNG, raw
		// and r16) and reports resolution before reading.
		FLandscapeImportDescriptor Descriptor;
		FText Message;
		const ELandscapeImportResult DescribeResult = FLandscapeImportHelper::GetHeightmapImportDescriptor(
			FilePath, /*bSingleFile*/ true, /*bFlipYAxis*/ false, Descriptor, Message);
		if (DescribeResult == ELandscapeImportResult::Error || Descriptor.ImportResolutions.IsEmpty())
		{
			OutError = FString::Printf(TEXT("cannot read '%s' as a heightmap: %s"), *FilePath,
				Message.IsEmpty() ? TEXT("unsupported format — use a 16-bit greyscale PNG, .raw or .r16")
								  : *Message.ToString());
			return false;
		}
		const FLandscapeImportResolution& Resolution = Descriptor.ImportResolutions[0];
		const ELandscapeImportResult ReadResult =
			FLandscapeImportHelper::GetHeightmapImportData(Descriptor, 0, OutRaw, Message);
		if (ReadResult == ELandscapeImportResult::Error
			|| OutRaw.Num() != static_cast<int32>(Resolution.Width * Resolution.Height))
		{
			OutError = FString::Printf(TEXT("cannot read '%s' as a heightmap: %s"), *FilePath,
				Message.IsEmpty() ? TEXT("the file did not yield one sample per pixel") : *Message.ToString());
			return false;
		}
		OutWidth = static_cast<int32>(Resolution.Width);
		OutHeight = static_cast<int32>(Resolution.Height);
		return true;
	}

	bool FitHeightmap(
		const TArray<uint16>& InRaw, int32 InWidth, int32 InHeight,
		int32 OutWidth, int32 OutHeight, const FString& Transform,
		TArray<uint16>& OutRaw, FString& OutError)
	{
		if (InWidth == OutWidth && InHeight == OutHeight)
		{
			OutRaw = InRaw;
			return true;
		}
		ELandscapeImportTransformType Type = ELandscapeImportTransformType::Resample;
		const FString Lower = Transform.ToLower();
		if (Lower.IsEmpty() || Lower == TEXT("resample"))
		{
			Type = ELandscapeImportTransformType::Resample;
		}
		else if (Lower == TEXT("original"))
		{
			Type = ELandscapeImportTransformType::None;
		}
		else if (Lower == TEXT("expand"))
		{
			Type = ELandscapeImportTransformType::ExpandCentered;
		}
		else
		{
			OutError = FString::Printf(
				TEXT("unknown transform '%s' — use resample, original or expand"), *Transform);
			return false;
		}
		FLandscapeImportHelper::TransformHeightmapImportData(
			InRaw, OutRaw,
			FLandscapeImportResolution(InWidth, InHeight),
			FLandscapeImportResolution(OutWidth, OutHeight),
			Type);
		if (OutRaw.Num() != OutWidth * OutHeight)
		{
			OutError = FString::Printf(
				TEXT("fitting the %d x %d heightmap to %d x %d produced %d samples"),
				InWidth, InHeight, OutWidth, OutHeight, OutRaw.Num());
			return false;
		}
		return true;
	}
}
