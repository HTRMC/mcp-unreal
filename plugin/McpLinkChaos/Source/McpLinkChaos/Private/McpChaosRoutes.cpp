// Chaos destruction: Geometry Collection assets and the fracture operations
// behind the Fracture editor mode.
//
// A Geometry Collection is a bone hierarchy of mesh pieces. It starts as one
// bone holding the source mesh, and each fracture splits selected bones into
// children — so fracturing twice gives a two-level cluster, which is what the
// solver breaks apart at runtime.
//
// The Fracture mode drives this through async ops on a component in the level;
// this route runs the same work synchronously on the *asset*, using the
// exported FFractureEngineFracturing entry points. What the mode's
// FGeometryCollectionEdit scope does on destruction, the asset's own
// InvalidateCollection / RebuildRenderData / CreateSimulationData do here.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dataflow/DataflowSelection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "FractureEngineFracturing.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionConversion.h"
#include "GeometryCollection/GeometryCollectionObject.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace Chaos
	{
		UGeometryCollection* CollectionOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("collection"), Path, Responder,
					TEXT("a Geometry Collection asset path, e.g. /Game/Destruction/GC_Wall")))
			{
				return nullptr;
			}
			UGeometryCollection* Collection = Cast<UGeometryCollection>(ResolveAsset(Path));
			if (Collection == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("collection_not_found"),
					FString::Printf(
						TEXT("no Geometry Collection at '%s' — chaos_ops create makes one"), *Path));
			}
			return Collection;
		}

		/// Bone levels, so a caller can see the cluster hierarchy a fracture
		/// produced without dumping every transform.
		void AddStructureFields(
			const TSharedRef<FJsonObject>& Data, const UGeometryCollection* Collection)
		{
			TSharedPtr<const FGeometryCollection> Geometry = Collection->GetGeometryCollection();
			if (!Geometry.IsValid())
			{
				Data->SetNumberField(TEXT("bone_count"), 0);
				return;
			}
			const int32 NumTransforms = Geometry->NumElements(FGeometryCollection::TransformGroup);
			Data->SetNumberField(TEXT("bone_count"), NumTransforms);
			Data->SetNumberField(TEXT("geometry_count"),
				Geometry->NumElements(FGeometryCollection::GeometryGroup));
			Data->SetNumberField(TEXT("vertex_count"),
				Geometry->NumElements(FGeometryCollection::VerticesGroup));
			Data->SetNumberField(TEXT("material_count"), Collection->Materials.Num());

			// Level 0 is the whole object; each fracture adds a level of children.
			int32 MaxLevel = 0;
			TArray<int32> PerLevel;
			if (Geometry->HasAttribute(TEXT("Level"), FGeometryCollection::TransformGroup))
			{
				const TManagedArray<int32>& Levels =
					Geometry->GetAttribute<int32>(TEXT("Level"), FGeometryCollection::TransformGroup);
				for (int32 Index = 0; Index < Levels.Num(); ++Index)
				{
					MaxLevel = FMath::Max(MaxLevel, Levels[Index]);
					PerLevel.SetNumZeroed(FMath::Max(PerLevel.Num(), Levels[Index] + 1));
					++PerLevel[Levels[Index]];
				}
			}
			Data->SetNumberField(TEXT("max_level"), MaxLevel);
			TArray<TSharedPtr<FJsonValue>> Counts;
			for (int32 Count : PerLevel)
			{
				Counts.Add(MakeShared<FJsonValueNumber>(Count));
			}
			Data->SetArrayField(TEXT("bones_per_level"), Counts);
		}

		/// Everything the Fracture mode's FGeometryCollectionEdit scope does when
		/// it closes, minus the parts that only apply to a live component.
		void FinalizeCollection(UGeometryCollection* Collection)
		{
			Collection->InitializeMaterials();
			Collection->UpdateGeometryDependentProperties();
			Collection->InvalidateCollection();
			Collection->RebuildRenderData();
			Collection->CreateSimulationData();
			Collection->MarkPackageDirty();
		}

		/// Which bones a fracture applies to: "bones" if given, else every leaf,
		/// which is what the mode does when you select the whole collection.
		FDataflowTransformSelection BuildSelection(
			const TSharedRef<FJsonObject>& Body, const FGeometryCollection& Geometry)
		{
			FDataflowTransformSelection Selection;
			Selection.Initialize(Geometry.NumElements(FGeometryCollection::TransformGroup), false);

			const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
			if (Body->TryGetArrayField(TEXT("bones"), Bones) && !Bones->IsEmpty())
			{
				for (const TSharedPtr<FJsonValue>& Value : *Bones)
				{
					const int32 Index = static_cast<int32>(Value->AsNumber());
					if (Index >= 0 && Index < Selection.Num())
					{
						Selection.SetSelected(Index);
					}
				}
				return Selection;
			}

			// A bone with no children is a piece you can still cut; a cluster
			// parent is not.
			const TManagedArray<TSet<int32>>& Children = Geometry.Children;
			for (int32 Index = 0; Index < Children.Num(); ++Index)
			{
				if (Children[Index].IsEmpty())
				{
					Selection.SetSelected(Index);
				}
			}
			return Selection;
		}

		/// PointSpacing is the target edge length the cut surfaces are
		/// resampled to before noise displaces them. Only PlaneCutter checks
		/// the amplitude before applying it — VoronoiFracture and
		/// UniformFracture pass the settings through unconditionally, so a flat
		/// cut with the engine default spacing of 1cm still tessellates every
		/// interior face (an 8-piece cube came out at 202k vertices). With no
		/// noise asked for, spacing wide enough to never subdivide is what
		/// "flat cut" should mean.
		FNoiseSettings ReadNoise(const TSharedRef<FJsonObject>& Body)
		{
			FNoiseSettings Noise;
			Noise.Amplitude = static_cast<float>(DoubleOr(Body, TEXT("noise_amplitude"), 0.0));
			Noise.Frequency = static_cast<float>(DoubleOr(Body, TEXT("noise_frequency"), 0.1));
			Noise.Persistence = static_cast<float>(DoubleOr(Body, TEXT("noise_persistence"), 0.5));
			Noise.Lacunarity = static_cast<float>(DoubleOr(Body, TEXT("noise_lacunarity"), 2.0));
			Noise.Octaves = IntOr(Body, TEXT("noise_octaves"), 4);
			const double DefaultSpacing = Noise.Amplitude > 0.0f ? 1.0 : 1.0e6;
			Noise.PointSpacing =
				static_cast<float>(DoubleOr(Body, TEXT("point_spacing"), DefaultSpacing));
			return Noise;
		}
	}

	using namespace Chaos;

	void RegisterChaosRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/chaos/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/Destruction/GC_Wall")))
					{
						return;
					}
					const TArray<TSharedPtr<FJsonValue>>* Sources = nullptr;
					if (!Body->TryGetArrayField(TEXT("source_meshes"), Sources) || Sources->IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'source_meshes' is required — one or more Static Mesh asset paths ")
							TEXT("to build the collection from"));
						return;
					}
					if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr, *Path) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}

					TArray<UStaticMesh*> Meshes;
					for (const TSharedPtr<FJsonValue>& Value : *Sources)
					{
						const FString MeshPath = Value->AsString();
						UStaticMesh* Mesh = Cast<UStaticMesh>(ResolveAsset(MeshPath));
						if (Mesh == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("mesh_not_found"),
								FString::Printf(TEXT("no Static Mesh at '%s'"), *MeshPath));
							return;
						}
						Meshes.Add(Mesh);
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateGeometryCollection",
							"McpLink Create Geometry Collection"));
					UPackage* Package = CreatePackage(*Path);
					UGeometryCollection* Collection = NewObject<UGeometryCollection>(Package,
						FName(*FPackageName::GetShortName(Path)),
						RF_Public | RF_Standalone | RF_Transactional);

					for (UStaticMesh* Mesh : Meshes)
					{
						TArray<UMaterialInterface*> Materials;
						for (const FStaticMaterial& Material : Mesh->GetStaticMaterials())
						{
							Materials.Add(Material.MaterialInterface);
						}
						FGeometryCollectionConversion::AppendStaticMesh(
							Mesh, Materials, FTransform::Identity, Collection,
							/*ReindexMaterials*/ false);
					}
					FinalizeCollection(Collection);
					FAssetRegistryModule::AssetCreated(Collection);
					Package->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("collection"), Collection->GetPathName());
					AddStructureFields(Data, Collection);
					Data->SetStringField(TEXT("message"),
						TEXT("created as a single unfractured bone — fracture_uniform, ")
						TEXT("fracture_voronoi or fracture_planar splits it, then save"));
					Responder->Ok(Data);
					return;
				}

				UGeometryCollection* Collection = CollectionOrError(Body, Responder);
				if (Collection == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("collection"), Collection->GetPathName());
					AddStructureFields(Data, Collection);
					TArray<TSharedPtr<FJsonValue>> Materials;
					for (const TObjectPtr<UMaterialInterface>& Material : Collection->Materials)
					{
						Materials.Add(MakeShared<FJsonValueString>(
							Material != nullptr ? Material->GetPathName() : FString()));
					}
					Data->SetArrayField(TEXT("materials"), Materials);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Collection, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("collection"), Collection->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				const bool bUniform = Operation == TEXT("fracture_uniform");
				const bool bVoronoi = Operation == TEXT("fracture_voronoi");
				const bool bPlanar = Operation == TEXT("fracture_planar");
				if (!bUniform && !bVoronoi && !bPlanar)
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
						FString::Printf(
							TEXT("unknown operation '%s' — use create, info, fracture_uniform, ")
							TEXT("fracture_voronoi, fracture_planar or save"),
							*Operation));
					return;
				}

				TSharedPtr<FGeometryCollection> Geometry = Collection->GetGeometryCollection();
				if (!Geometry.IsValid())
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_collection_data"),
						TEXT("this asset holds no collection data to fracture"));
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "FractureGeometryCollection", "McpLink Fracture"));
				Collection->Modify();

				const FDataflowTransformSelection Selection = BuildSelection(Body, *Geometry);
				if (Selection.AnySelected() == false)
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("nothing_selected"),
						TEXT("no bone to fracture — 'bones' selected nothing, and the collection ")
						TEXT("has no leaf bones"));
					return;
				}

				const int32 Seed = IntOr(Body, TEXT("seed"), 0);
				const float Grout = static_cast<float>(DoubleOr(Body, TEXT("grout"), 0.0));
				const float Chance = static_cast<float>(DoubleOr(Body, TEXT("chance_to_fracture"), 1.0));
				const float CollisionSampleSpacing =
					static_cast<float>(DoubleOr(Body, TEXT("collision_sample_spacing"), 50.0));
				const bool bAddCollisionSamples = BoolOr(Body, TEXT("add_collision_samples"), false);
				const FNoiseSettings Noise = ReadNoise(Body);
				FIslandSplitSettings IslandSplit;
				IslandSplit.bSplitIslands = BoolOr(Body, TEXT("split_islands"), false);

				const int32 BonesBefore = Geometry->NumElements(FGeometryCollection::TransformGroup);
				int32 FirstNewGeometry = INDEX_NONE;

				if (bUniform)
				{
					FUniformFractureSettings Settings;
					Settings.Transform = FTransform::Identity;
					Settings.MinVoronoiSites = FMath::Max(2, IntOr(Body, TEXT("min_sites"), 8));
					Settings.MaxVoronoiSites =
						FMath::Max(Settings.MinVoronoiSites, IntOr(Body, TEXT("max_sites"), 12));
					Settings.InternalMaterialID = IntOr(Body, TEXT("internal_material_id"), 0);
					Settings.RandomSeed = Seed;
					Settings.ChanceToFracture = Chance;
					// The mode's "Group Fracture": one site cloud over the whole
					// selection instead of one per bone.
					Settings.GroupFracture = BoolOr(Body, TEXT("group_fracture"), true);
					Settings.SplitIslands = IslandSplit.bSplitIslands;
					Settings.Grout = Grout;
					Settings.NoiseSettings = Noise;
					Settings.AddSamplesForCollision = bAddCollisionSamples;
					Settings.CollisionSampleSpacing = CollisionSampleSpacing;
					FirstNewGeometry =
						FFractureEngineFracturing::UniformFracture(*Geometry, Selection, Settings);
				}
				else if (bVoronoi)
				{
					TArray<FVector> Sites;
					const TArray<TSharedPtr<FJsonValue>>* SiteArray = nullptr;
					if (Body->TryGetArrayField(TEXT("sites"), SiteArray))
					{
						for (const TSharedPtr<FJsonValue>& Value : *SiteArray)
						{
							const TArray<TSharedPtr<FJsonValue>>* Triple = nullptr;
							if (Value->TryGetArray(Triple) && Triple->Num() == 3)
							{
								Sites.Emplace((*Triple)[0]->AsNumber(), (*Triple)[1]->AsNumber(),
									(*Triple)[2]->AsNumber());
							}
						}
					}
					if (Sites.Num() < 2)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("too_few_sites"),
							TEXT("'sites' needs at least two [x, y, z] points in the collection's ")
							TEXT("local space — each site becomes one piece. fracture_uniform ")
							TEXT("scatters them for you"));
						return;
					}
					FirstNewGeometry = FFractureEngineFracturing::VoronoiFracture(*Geometry, Selection,
						Sites, FTransform::Identity, Seed, Chance, IslandSplit, Grout,
						Noise.Amplitude, Noise.Frequency, Noise.Persistence, Noise.Lacunarity,
						Noise.Octaves, Noise.PointSpacing, bAddCollisionSamples,
						CollisionSampleSpacing);
				}
				else
				{
					FBox Bounds(ForceInit);
					const TManagedArray<FBox>& BoundingBoxes = Geometry->BoundingBox;
					for (int32 Index = 0; Index < BoundingBoxes.Num(); ++Index)
					{
						Bounds += BoundingBoxes[Index];
					}
					FirstNewGeometry = FFractureEngineFracturing::PlaneCutter(*Geometry, Selection,
						Bounds, FTransform::Identity, FMath::Max(1, IntOr(Body, TEXT("planes"), 3)),
						Seed, Chance, IslandSplit, Grout, Noise.Amplitude, Noise.Frequency,
						Noise.Persistence, Noise.Lacunarity, Noise.Octaves, Noise.PointSpacing,
						bAddCollisionSamples, CollisionSampleSpacing);
				}

				if (FirstNewGeometry <= INDEX_NONE)
				{
					Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("fracture_produced_nothing"),
						TEXT("the fracture produced no new pieces. Common causes: the selected bones ")
						TEXT("are already the smallest pieces, the grout is wider than the geometry, ")
						TEXT("or chance_to_fracture skipped every bone"));
					return;
				}

				FinalizeCollection(Collection);

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("collection"), Collection->GetPathName());
				Data->SetNumberField(TEXT("bones_before"), BonesBefore);
				Data->SetNumberField(TEXT("first_new_geometry"), FirstNewGeometry);
				AddStructureFields(Data, Collection);
				Data->SetStringField(TEXT("message"),
					TEXT("fractured — fracture again to add another cluster level, then save"));
				Responder->Ok(Data);
			});
	}
}
