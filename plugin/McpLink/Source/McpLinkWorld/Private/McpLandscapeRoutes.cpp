// Landscape terrain: create a landscape, read and write heights, and manage
// the weightmap layers a landscape material paints with.
//
// Heights cross the wire in centimetres of world Z relative to the landscape
// actor, not as raw uint16 samples: the 0..65535 encoding with its 1/128 cm
// step and 32768 midpoint is an implementation detail no agent should carry.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EngineUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Landscape.h"
#include "LandscapeEditLayer.h"
#include "LandscapeEditTypes.h"
#include "LandscapeGrassType.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"
#include "LandscapeEdit.h"
#include "LandscapeImportHelper.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "Materials/MaterialInterface.h"
#include "JsonObjectConverter.h"
#include "McpAssetUtils.h"
#include "McpLandscapeUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace
	{
		using namespace McpLink::Landscapes;

		TSharedRef<FJsonObject> LandscapeEditLayerJson(const ALandscape& Landscape, int32 Index)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("index"), Index);
			const ULandscapeEditLayerBase* Layer = Landscape.GetEditLayerConst(Index);
			if (Layer == nullptr)
			{
				return Entry;
			}
			Entry->SetStringField(TEXT("name"), Layer->GetName().ToString());
			Entry->SetStringField(TEXT("guid"), Layer->GetGuid().ToString());
			Entry->SetStringField(TEXT("class"), Layer->GetClass()->GetName());
			Entry->SetBoolField(TEXT("visible"), Layer->IsVisible());
			Entry->SetBoolField(TEXT("locked"), Layer->IsLocked());
			Entry->SetNumberField(TEXT("heightmap_alpha"), Layer->GetAlphaForTargetType(ELandscapeToolTargetType::Heightmap));
			Entry->SetNumberField(TEXT("weightmap_alpha"), Layer->GetAlphaForTargetType(ELandscapeToolTargetType::Weightmap));
			Entry->SetBoolField(TEXT("editing"), Landscape.GetEditingLayer() == Layer->GetGuid());
			return Entry;
		}

		TSharedRef<FJsonObject> LandscapeEditLayersJson(ALandscape& Landscape)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("landscape"), Landscape.GetPathName());
			Data->SetBoolField(TEXT("has_edit_layers"), Landscape.HasLayersContent());
			Data->SetStringField(TEXT("editing_layer"), Landscape.GetEditingLayer().ToString());
			TArray<TSharedPtr<FJsonValue>> Layers;
			const int32 Count = Landscape.HasLayersContent() ? Landscape.GetLayersConst().Num() : 0;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Layers.Add(MakeShared<FJsonValueObject>(LandscapeEditLayerJson(Landscape, Index)));
			}
			Data->SetArrayField(TEXT("layers"), Layers);
			return Data;
		}

		/// The edit layer named (or indexed) by 'layer', or nullptr after responding.
		ULandscapeEditLayerBase* LandscapeEditLayerOrError(ALandscape& Landscape,
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder, int32& OutIndex)
		{
			if (!Landscape.HasLayersContent())
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_edit_layers"),
					TEXT("the landscape has no edit layers — enable_edit_layers turns them on"));
				return nullptr;
			}
			FString Spec;
			if (!RequireString(Body, TEXT("layer"), Spec, Responder, TEXT("an edit layer name (or index) from list_edit_layers")))
			{
				return nullptr;
			}
			OutIndex = Landscape.GetLayerIndex(FName(*Spec));
			if (OutIndex == INDEX_NONE && Spec.IsNumeric())
			{
				OutIndex = FCString::Atoi(*Spec);
			}
			ULandscapeEditLayerBase* Layer = Landscape.GetLayersConst().IsValidIndex(OutIndex) ? Landscape.GetEditLayer(OutIndex) : nullptr;
			if (Layer == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("layer_not_found"),
					FString::Printf(TEXT("no edit layer '%s' — list_edit_layers shows them"), *Spec));
			}
			return Layer;
		}

		/// Routes a height or weight write into an edit layer while the
		/// landscape has them ('edit_layer', default the first), and asks for the
		/// merged result to be rebuilt afterwards. Nothing happens on a
		/// landscape without edit layers.
		struct FLandscapeLayerWriteScope
		{
			TUniquePtr<FScopedSetLandscapeEditingLayer> Scope;
			ALandscape* Landscape = nullptr;
			bool bFailed = false;
			FString Note;

			FLandscapeLayerWriteScope(ALandscape* InLandscape, const TSharedRef<FJsonObject>& Body,
				const TSharedRef<FMcpResponder>& Responder)
			{
				if (InLandscape == nullptr || !InLandscape->HasLayersContent())
				{
					return;
				}
				FString Spec;
				Body->TryGetStringField(TEXT("edit_layer"), Spec);
				// The layers are merged into the final heightmap on the GPU,
				// which a -nullrhi editor never does: a layer write there is
				// kept but invisible to get_heights until the level is opened
				// with rendering. So headless, an unnamed write goes straight to
				// the final data, as before edit layers were addressable.
				if (!FApp::CanEverRender())
				{
					if (Spec.IsEmpty())
					{
						return;
					}
					Note = TEXT("written into the edit layer; this editor cannot render, so the merged heightmap ")
						TEXT("(what get_heights and collision see) only updates once the level is opened with an RHI");
				}
				int32 Index = Spec.IsEmpty() ? 0 : InLandscape->GetLayerIndex(FName(*Spec));
				if (Index == INDEX_NONE && Spec.IsNumeric())
				{
					Index = FCString::Atoi(*Spec);
				}
				const ULandscapeEditLayerBase* Layer =
					InLandscape->GetLayersConst().IsValidIndex(Index) ? InLandscape->GetEditLayerConst(Index) : nullptr;
				if (Layer == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("layer_not_found"),
						FString::Printf(TEXT("no edit layer '%s' — list_edit_layers shows them"), *Spec));
					bFailed = true;
					return;
				}
				if (Layer->IsLocked())
				{
					Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("layer_locked"),
						FString::Printf(TEXT("edit layer '%s' is locked — set_edit_layer locked=false first"), *Layer->GetName().ToString()));
					bFailed = true;
					return;
				}
				Landscape = InLandscape;
				Scope = MakeUnique<FScopedSetLandscapeEditingLayer>(InLandscape, Layer->GetGuid());
			}

			~FLandscapeLayerWriteScope()
			{
				Scope.Reset();
				if (Landscape != nullptr)
				{
					Landscape->RequestLayersContentUpdateForceAll();
				}
			}

			void Annotate(const TSharedRef<FJsonObject>& Result) const
			{
				if (!Note.IsEmpty())
				{
					Result->SetStringField(TEXT("note"), Note);
				}
			}
		};

		void LandscapeSplineNotify(UObject& Object)
		{
			// Not exported on these MinimalAPI classes, but their
			// PostEditChangeProperty override is what calls UpdateSplinePoints
			// (and, with ValueSet, the edit-layer splines update).
			FPropertyChangedEvent Event(nullptr, EPropertyChangeType::ValueSet);
			Object.PostEditChangeProperty(Event);
		}

		TSharedRef<FJsonObject> LandscapeSplinePointJson(const ULandscapeSplinesComponent& Component, int32 Index)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("index"), Index);
			const ULandscapeSplineControlPoint* Point = Component.GetControlPoints()[Index];
			if (Point == nullptr)
			{
				return Entry;
			}
			Entry->SetArrayField(TEXT("location"), VectorToJson(Component.GetComponentTransform().TransformPosition(Point->Location)));
			Entry->SetArrayField(TEXT("rotation"), RotatorToJson(Point->Rotation));
			Entry->SetNumberField(TEXT("width"), Point->Width);
			Entry->SetNumberField(TEXT("side_falloff"), Point->SideFalloff);
			Entry->SetNumberField(TEXT("end_falloff"), Point->EndFalloff);
			Entry->SetStringField(TEXT("layer_name"), Point->LayerName.ToString());
			Entry->SetStringField(TEXT("mesh"), Point->Mesh != nullptr ? Point->Mesh->GetPathName() : FString());
			TArray<TSharedPtr<FJsonValue>> Segments;
			for (const FLandscapeSplineConnection& Connection : Point->ConnectedSegments)
			{
				Segments.Add(MakeShared<FJsonValueNumber>(Component.GetSegments().IndexOfByKey(Connection.Segment)));
			}
			Entry->SetArrayField(TEXT("segments"), Segments);
			return Entry;
		}

		TSharedRef<FJsonObject> LandscapeSplineSegmentJson(const ULandscapeSplinesComponent& Component, int32 Index)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("index"), Index);
			const ULandscapeSplineSegment* Segment = Component.GetSegments()[Index];
			if (Segment == nullptr)
			{
				return Entry;
			}
			Entry->SetNumberField(TEXT("start"), Component.GetControlPoints().IndexOfByKey(Segment->Connections[0].ControlPoint));
			Entry->SetNumberField(TEXT("end"), Component.GetControlPoints().IndexOfByKey(Segment->Connections[1].ControlPoint));
			Entry->SetNumberField(TEXT("start_tangent"), Segment->Connections[0].TangentLen);
			Entry->SetNumberField(TEXT("end_tangent"), Segment->Connections[1].TangentLen);
			Entry->SetStringField(TEXT("layer_name"), Segment->LayerName.ToString());
			Entry->SetBoolField(TEXT("raise_terrain"), Segment->bRaiseTerrain);
			Entry->SetBoolField(TEXT("lower_terrain"), Segment->bLowerTerrain);
			TArray<TSharedPtr<FJsonValue>> Meshes;
			for (const FLandscapeSplineMeshEntry& Mesh : Segment->SplineMeshes)
			{
				Meshes.Add(MakeShared<FJsonValueString>(Mesh.Mesh != nullptr ? Mesh.Mesh->GetPathName() : FString()));
			}
			Entry->SetArrayField(TEXT("meshes"), Meshes);
			return Entry;
		}

		TSharedRef<FJsonObject> LandscapeSplinesJson(ALandscape& Landscape)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("landscape"), Landscape.GetPathName());
			TArray<TSharedPtr<FJsonValue>> Points, Segments;
			if (const ULandscapeSplinesComponent* Component = Landscape.GetSplinesComponent())
			{
				for (int32 Index = 0; Index < Component->GetControlPoints().Num(); ++Index)
				{
					Points.Add(MakeShared<FJsonValueObject>(LandscapeSplinePointJson(*Component, Index)));
				}
				for (int32 Index = 0; Index < Component->GetSegments().Num(); ++Index)
				{
					Segments.Add(MakeShared<FJsonValueObject>(LandscapeSplineSegmentJson(*Component, Index)));
				}
			}
			Data->SetArrayField(TEXT("control_points"), Points);
			Data->SetArrayField(TEXT("segments"), Segments);
			return Data;
		}

		/// Apply the optional point fields of a spline request to a control point.
		void ApplyLandscapeSplinePointFields(const TSharedRef<FJsonObject>& Body, ULandscapeSplineControlPoint& Point)
		{
			double Number = 0.0;
			if (Body->TryGetNumberField(TEXT("width"), Number))
			{
				Point.Width = static_cast<float>(Number);
			}
			if (Body->TryGetNumberField(TEXT("side_falloff"), Number))
			{
				Point.SideFalloff = static_cast<float>(Number);
			}
			if (Body->TryGetNumberField(TEXT("end_falloff"), Number))
			{
				Point.EndFalloff = static_cast<float>(Number);
			}
			FString Text;
			if (Body->TryGetStringField(TEXT("layer_name"), Text))
			{
				Point.LayerName = FName(*Text);
			}
			if (Body->TryGetStringField(TEXT("mesh"), Text))
			{
				Point.Mesh = Text.IsEmpty() ? nullptr : Cast<UStaticMesh>(ResolveAsset(Text));
			}
			FRotator Rotation;
			if (GetRotator(Body, TEXT("rotation"), Rotation))
			{
				Point.Rotation = Rotation;
			}
			bool bFlag = false;
			if (Body->TryGetBoolField(TEXT("raise_terrain"), bFlag))
			{
				Point.bRaiseTerrain = bFlag;
			}
			if (Body->TryGetBoolField(TEXT("lower_terrain"), bFlag))
			{
				Point.bLowerTerrain = bFlag;
			}
		}

		void RemoveLandscapeSplineSegment(ULandscapeSplinesComponent& Component, ULandscapeSplineSegment& Segment)
		{
			Segment.Modify();
			Segment.DeleteSplinePoints();
			for (int32 End = 0; End < 2; ++End)
			{
				if (ULandscapeSplineControlPoint* Point = Segment.Connections[End].ControlPoint)
				{
					Point->Modify();
					Point->ConnectedSegments.RemoveAll(
						[&Segment](const FLandscapeSplineConnection& Connection) { return Connection.Segment == &Segment; });
				}
			}
			Component.GetSegments().Remove(&Segment);
		}

		bool ReadGrassVariety(const TSharedPtr<FJsonObject>& Entry, FGrassVariety& Out, FString& Error)
		{
			FString MeshPath;
			if (!Entry->TryGetStringField(TEXT("mesh"), MeshPath))
			{
				Error = TEXT("each variety needs 'mesh' (a Static Mesh path)");
				return false;
			}
			Out.GrassMesh = Cast<UStaticMesh>(ResolveAsset(MeshPath));
			if (Out.GrassMesh == nullptr)
			{
				Error = FString::Printf(TEXT("no Static Mesh at '%s'"), *MeshPath);
				return false;
			}
			double Number = 0.0;
			if (Entry->TryGetNumberField(TEXT("density"), Number))
			{
				Out.GrassDensity.Default = static_cast<float>(Number);
			}
			double MinScale = Out.ScaleX.Min, MaxScale = Out.ScaleX.Max;
			Entry->TryGetNumberField(TEXT("min_scale"), MinScale);
			Entry->TryGetNumberField(TEXT("max_scale"), MaxScale);
			Out.ScaleX = Out.ScaleY = Out.ScaleZ = FFloatInterval(static_cast<float>(MinScale), static_cast<float>(MaxScale));
			if (Entry->TryGetNumberField(TEXT("start_cull_distance"), Number))
			{
				Out.StartCullDistance.Default = static_cast<int32>(Number);
			}
			if (Entry->TryGetNumberField(TEXT("end_cull_distance"), Number))
			{
				Out.EndCullDistance.Default = static_cast<int32>(Number);
			}
			bool bFlag = false;
			if (Entry->TryGetBoolField(TEXT("random_rotation"), bFlag))
			{
				Out.RandomRotation = bFlag;
			}
			if (Entry->TryGetBoolField(TEXT("align_to_surface"), bFlag))
			{
				Out.AlignToSurface = bFlag;
			}
			if (Entry->TryGetBoolField(TEXT("use_grid"), bFlag))
			{
				Out.bUseGrid = bFlag;
			}
			// Anything else by its UPROPERTY name (PlacementJitter, MinLOD, …).
			const TSharedPtr<FJsonObject>* Extra = nullptr;
			if (Entry->TryGetObjectField(TEXT("properties"), Extra)
				&& !FJsonObjectConverter::JsonObjectToUStruct(Extra->ToSharedRef(), FGrassVariety::StaticStruct(), &Out, 0, 0))
			{
				Error = TEXT("'properties' did not apply — keys are FGrassVariety UPROPERTY names");
				return false;
			}
			return true;
		}

		ALandscape* LandscapeOrError(
			UWorld* World, const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			if (World == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_world"),
					TEXT("no world — open a level or start PIE"));
				return nullptr;
			}
			FString Spec;
			Body->TryGetStringField(TEXT("landscape"), Spec);

			TArray<ALandscape*> Found;
			for (TActorIterator<ALandscape> It(World); It; ++It)
			{
				if (Spec.IsEmpty() || It->GetActorNameOrLabel() == Spec || It->GetPathName() == Spec)
				{
					Found.Add(*It);
				}
			}
			if (Found.Num() == 1)
			{
				return Found[0];
			}
			if (Found.Num() > 1)
			{
				// Only reachable when no landscape was named.
				TArray<FString> Labels;
				for (const ALandscape* Landscape : Found) { Labels.Add(Landscape->GetActorNameOrLabel()); }
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("ambiguous_landscape"),
					FString::Printf(TEXT("this level has %d landscapes — name one: %s"),
						Found.Num(), *FString::Join(Labels, TEXT(", "))));
				return nullptr;
			}
			Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("landscape_not_found"),
				Spec.IsEmpty()
					? FString(TEXT("this level has no landscape — landscape_ops create makes one"))
					: FString::Printf(TEXT("no landscape '%s' in this level"), *Spec));
			return nullptr;
		}

		/// The rect a request addresses, clamped to the landscape's extent.
		bool ReadRegion(
			const TSharedRef<FJsonObject>& Body, ULandscapeInfo& Info,
			int32& X1, int32& Y1, int32& X2, int32& Y2, FString& OutError)
		{
			int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
			if (!Info.GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
			{
				OutError = TEXT("the landscape has no components yet");
				return false;
			}
			double Value = 0.0;
			X1 = Body->TryGetNumberField(TEXT("min_x"), Value) ? static_cast<int32>(Value) : MinX;
			Y1 = Body->TryGetNumberField(TEXT("min_y"), Value) ? static_cast<int32>(Value) : MinY;
			X2 = Body->TryGetNumberField(TEXT("max_x"), Value) ? static_cast<int32>(Value) : MaxX;
			Y2 = Body->TryGetNumberField(TEXT("max_y"), Value) ? static_cast<int32>(Value) : MaxY;

			X1 = FMath::Clamp(X1, MinX, MaxX);
			Y1 = FMath::Clamp(Y1, MinY, MaxY);
			X2 = FMath::Clamp(X2, X1, MaxX);
			Y2 = FMath::Clamp(Y2, Y1, MaxY);
			return true;
		}

		/// An existing LayerInfo asset at a package path, without loading a package
		/// that is not there: FSoftObjectPath::TryLoad leaves an empty UPackage
		/// registered on a miss, which then reads as "an asset already exists".
		ULandscapeLayerInfoObject* FindLayerInfoAsset(const FString& PackagePath)
		{
			const FString ObjectPath = FString::Printf(
				TEXT("%s.%s"), *PackagePath, *FPackageName::GetShortName(PackagePath));
			if (UObject* Found = StaticFindObject(
				ULandscapeLayerInfoObject::StaticClass(), nullptr, *ObjectPath))
			{
				return Cast<ULandscapeLayerInfoObject>(Found);
			}
			if (FPackageName::DoesPackageExist(PackagePath))
			{
				return LoadObject<ULandscapeLayerInfoObject>(nullptr, *ObjectPath);
			}
			return nullptr;
		}

		ULandscapeLayerInfoObject* FindLayerInfo(const ULandscapeInfo& Info, const FString& Name)
		{
			for (const FLandscapeInfoLayerSettings& Layer : Info.Layers)
			{
				if (Layer.LayerInfoObj != nullptr && Layer.GetLayerName().ToString() == Name)
				{
					return Layer.LayerInfoObj;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> LandscapeToJson(ALandscape& Landscape)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("landscape"), Landscape.GetPathName());
			Json->SetStringField(TEXT("label"), Landscape.GetActorNameOrLabel());
			Json->SetArrayField(TEXT("location"), VectorToJson(Landscape.GetActorLocation()));
			Json->SetArrayField(TEXT("scale"), VectorToJson(Landscape.GetActorScale3D()));

			ULandscapeInfo* Info = Landscape.GetLandscapeInfo();
			if (Info == nullptr)
			{
				return Json;
			}
			int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
			if (Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
			{
				const TSharedRef<FJsonObject> Extent = MakeShared<FJsonObject>();
				Extent->SetNumberField(TEXT("min_x"), MinX);
				Extent->SetNumberField(TEXT("min_y"), MinY);
				Extent->SetNumberField(TEXT("max_x"), MaxX);
				Extent->SetNumberField(TEXT("max_y"), MaxY);
				Extent->SetNumberField(TEXT("vertices_x"), MaxX - MinX + 1);
				Extent->SetNumberField(TEXT("vertices_y"), MaxY - MinY + 1);
				Json->SetObjectField(TEXT("extent"), Extent);
			}
			Json->SetNumberField(TEXT("component_size_quads"), Info->ComponentSizeQuads);
			Json->SetNumberField(TEXT("subsection_size_quads"), Info->SubsectionSizeQuads);
			Json->SetNumberField(TEXT("sections_per_component"), Info->ComponentNumSubsections);

			TArray<TSharedPtr<FJsonValue>> Layers;
			for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
			{
				const TSharedRef<FJsonObject> LayerJson = MakeShared<FJsonObject>();
				LayerJson->SetStringField(TEXT("name"), Layer.GetLayerName().ToString());
				LayerJson->SetBoolField(TEXT("has_layer_info"), Layer.LayerInfoObj != nullptr);
				if (Layer.LayerInfoObj != nullptr)
				{
					LayerJson->SetStringField(TEXT("layer_info"), Layer.LayerInfoObj->GetPathName());
				}
				Layers.Add(MakeShared<FJsonValueObject>(LayerJson));
			}
			Json->SetArrayField(TEXT("layers"), Layers);
			return Json;
		}
	}

	void RegisterLandscapeRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/world/landscape"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);
				UWorld* World = ResolveWorld(Body);

				if (Operation == TEXT("list"))
				{
					if (World == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_world"),
							TEXT("no world — open a level or start PIE"));
						return;
					}
					TArray<TSharedPtr<FJsonValue>> Landscapes;
					for (TActorIterator<ALandscape> It(World); It; ++It)
					{
						Landscapes.Add(MakeShared<FJsonValueObject>(LandscapeToJson(**It)));
					}
					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetArrayField(TEXT("landscapes"), Landscapes);
					Responder->Ok(Result);
					return;
				}

				if (Operation == TEXT("create"))
				{
					if (World == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_world"),
							TEXT("no world — open a level or start PIE"));
						return;
					}
					// The landscape's resolution is quads per section x sections
					// per component x components; the defaults are the ones the
					// New Landscape panel opens with.
					FCreateParams Params;
					double Value = 0.0;
					if (Body->TryGetNumberField(TEXT("quads_per_section"), Value))
					{
						Params.QuadsPerSection = static_cast<int32>(Value);
					}
					if (Body->TryGetNumberField(TEXT("sections_per_component"), Value))
					{
						Params.SectionsPerComponent = static_cast<int32>(Value);
					}
					if (Body->TryGetNumberField(TEXT("components_x"), Value))
					{
						Params.ComponentsX = static_cast<int32>(Value);
					}
					if (Body->TryGetNumberField(TEXT("components_y"), Value))
					{
						Params.ComponentsY = static_cast<int32>(Value);
					}
					GetVector(Body, TEXT("location"), Params.Location);
					GetVector(Body, TEXT("scale"), Params.Scale);
					Body->TryGetNumberField(TEXT("height"), Params.Height);

					// A heightmap file sizes the landscape unless the caller
					// fixed the component layout, in which case the samples
					// are fitted to it.
					FString HeightmapFile;
					int32 FileWidth = 0, FileHeight = 0;
					if (Body->TryGetStringField(TEXT("heightmap_file"), HeightmapFile) && !HeightmapFile.IsEmpty())
					{
						TArray<uint16> FileRaw;
						FString Error;
						if (!ReadHeightmapFile(HeightmapFile, FileRaw, FileWidth, FileHeight, Error))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("heightmap_unreadable"), Error);
							return;
						}
						if (!Body->HasField(TEXT("components_x")) && !Body->HasField(TEXT("components_y")))
						{
							// The New Landscape panel's own choice of layout
							// for this many pixels.
							FIntPoint Components(1, 1);
							FLandscapeImportHelper::ChooseBestComponentSizeForImport(
								FileWidth, FileHeight, Params.QuadsPerSection, Params.SectionsPerComponent, Components);
							Params.ComponentsX = FMath::Max(1, Components.X);
							Params.ComponentsY = FMath::Max(1, Components.Y);
						}
						const int32 QuadsPerComponent = Params.QuadsPerSection * Params.SectionsPerComponent;
						const int32 SizeX = Params.ComponentsX * QuadsPerComponent + 1;
						const int32 SizeY = Params.ComponentsY * QuadsPerComponent + 1;
						FString Transform;
						Body->TryGetStringField(TEXT("transform"), Transform);
						if (!FitHeightmap(FileRaw, FileWidth, FileHeight, SizeX, SizeY, Transform, Params.RawHeights, Error))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("heightmap_fit_failed"), Error);
							return;
						}
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateLandscape", "McpLink Create Landscape"),
						ShouldTransact(World));

					FString Error;
					ALandscape* Landscape = Create(World, Params, Error);
					if (Landscape == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("create_failed"), Error);
						return;
					}

					FString MaterialPath;
					if (Body->TryGetStringField(TEXT("material"), MaterialPath) && !MaterialPath.IsEmpty())
					{
						Landscape->LandscapeMaterial = Cast<UMaterialInterface>(ResolveObject(MaterialPath));
						Landscape->UpdateAllComponentMaterialInstances();
					}

					FString Label;
					if (Body->TryGetStringField(TEXT("name"), Label) && !Label.IsEmpty())
					{
						Landscape->SetActorLabel(Label);
					}
					const TSharedRef<FJsonObject> Created = LandscapeToJson(*Landscape);
					if (FileWidth > 0)
					{
						Created->SetStringField(TEXT("heightmap_file"), HeightmapFile);
						Created->SetNumberField(TEXT("heightmap_width"), FileWidth);
						Created->SetNumberField(TEXT("heightmap_height"), FileHeight);
					}
					Responder->Ok(Created);
					return;
				}

				if (Operation == TEXT("create_grass_type"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Landscape/LG_Meadow")))
					{
						return;
					}
					if (!FPackageName::IsValidLongPackageName(Path))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_path"),
							FString::Printf(TEXT("'%s' is not a package path — use /Game/Folder/Name"), *Path));
						return;
					}
					if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}
					const TArray<TSharedPtr<FJsonValue>>* VarietyValues = nullptr;
					TArray<FGrassVariety> Varieties;
					if (Body->TryGetArrayField(TEXT("varieties"), VarietyValues))
					{
						for (const TSharedPtr<FJsonValue>& Value : *VarietyValues)
						{
							const TSharedPtr<FJsonObject>* Entry = nullptr;
							FGrassVariety Variety;
							FString Error;
							if (!Value->TryGetObject(Entry) || !ReadGrassVariety(*Entry, Variety, Error))
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_variety"),
									Error.IsEmpty() ? TEXT("each variety is an object") : *Error);
								return;
							}
							Varieties.Add(Variety);
						}
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateGrassType", "McpLink Create Landscape Grass Type"));
					UPackage* Package = CreatePackage(*Path);
					ULandscapeGrassType* GrassType = NewObject<ULandscapeGrassType>(Package,
						FName(*FPackageName::GetShortName(Path)), RF_Public | RF_Standalone | RF_Transactional);
					GrassType->GrassVarieties = Varieties;
					GrassType->PostEditChange();
					FAssetRegistryModule::AssetCreated(GrassType);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("grass_type"), GrassType->GetPathName());
					Data->SetNumberField(TEXT("varieties"), GrassType->GrassVarieties.Num());
					Data->SetStringField(TEXT("message"),
						TEXT("created — the landscape material spawns it: material_graph add_expression ")
						TEXT("MaterialExpressionLandscapeGrassOutput, set_property its GrassTypes to this asset, ")
						TEXT("and feed the layer's weight into that input"));
					Responder->Ok(Data);
					return;
				}

				ALandscape* Landscape = LandscapeOrError(World, Body, Responder);
				if (Landscape == nullptr)
				{
					return;
				}
				ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
				if (Info == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_landscape_info"),
						TEXT("this landscape has no landscape info — it may not be registered"));
					return;
				}

				if (Operation == TEXT("info"))
				{
					Responder->Ok(LandscapeToJson(*Landscape));
					return;
				}

				if (Operation == TEXT("get_heights"))
				{
					int32 X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
					FString Error;
					if (!ReadRegion(Body, *Info, X1, Y1, X2, Y2, Error))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_extent"), Error);
						return;
					}
					const int32 Width = X2 - X1 + 1;
					const int32 Rows = Y2 - Y1 + 1;
					if (static_cast<int64>(Width) * Rows > 65536)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("region_too_large"),
							FString::Printf(
								TEXT("%d x %d is %lld samples — ask for at most 65536 (min_x/min_y/max_x/max_y)"),
								Width, Rows, static_cast<int64>(Width) * Rows));
						return;
					}

					TArray<double> Sampled;
					if (!GetHeights(*Landscape, X1, Y1, X2, Y2, Sampled, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("read_failed"), Error);
						return;
					}

					TArray<TSharedPtr<FJsonValue>> Heights;
					double Lowest = TNumericLimits<double>::Max();
					double Highest = TNumericLimits<double>::Lowest();
					for (const double WorldZ : Sampled)
					{
						Lowest = FMath::Min(Lowest, WorldZ);
						Highest = FMath::Max(Highest, WorldZ);
						Heights.Add(MakeShared<FJsonValueNumber>(WorldZ));
					}

					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetNumberField(TEXT("min_x"), X1);
					Result->SetNumberField(TEXT("min_y"), Y1);
					Result->SetNumberField(TEXT("max_x"), X2);
					Result->SetNumberField(TEXT("max_y"), Y2);
					Result->SetNumberField(TEXT("width"), Width);
					Result->SetNumberField(TEXT("rows"), Rows);
					Result->SetNumberField(TEXT("lowest"), Lowest);
					Result->SetNumberField(TEXT("highest"), Highest);
					Result->SetArrayField(TEXT("heights"), Heights);
					Responder->Ok(Result);
					return;
				}

				if (Operation == TEXT("set_heights"))
				{
					int32 X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
					FString Error;
					if (!ReadRegion(Body, *Info, X1, Y1, X2, Y2, Error))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_extent"), Error);
						return;
					}
					const int32 Width = X2 - X1 + 1;
					const int32 Rows = Y2 - Y1 + 1;

					TArray<double> Heights;
					const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
					double Uniform = 0.0;
					if (Body->TryGetArrayField(TEXT("heights"), Samples))
					{
						Heights.Reserve(Samples->Num());
						for (const TSharedPtr<FJsonValue>& Sample : *Samples)
						{
							double WorldZ = 0.0;
							Sample->TryGetNumber(WorldZ);
							Heights.Add(WorldZ);
						}
					}
					else if (Body->TryGetNumberField(TEXT("height"), Uniform))
					{
						Heights.Init(Uniform, Width * Rows);
					}
					else
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'height' (flatten the region) or 'heights' (one value per vertex, ")
							TEXT("row-major from min_y) is required"));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetHeights", "McpLink Sculpt Landscape"),
						ShouldTransact(Landscape));
					FLandscapeLayerWriteScope LayerScope(Landscape, Body, Responder);
					if (LayerScope.bFailed)
					{
						return;
					}
					if (!SetHeights(*Landscape, X1, Y1, X2, Y2, Heights, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("wrong_sample_count"), Error);
						return;
					}

					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetNumberField(TEXT("min_x"), X1);
					Result->SetNumberField(TEXT("min_y"), Y1);
					Result->SetNumberField(TEXT("max_x"), X2);
					Result->SetNumberField(TEXT("max_y"), Y2);
					Result->SetNumberField(TEXT("vertices_written"), Width * Rows);
					LayerScope.Annotate(Result);
					Responder->Ok(Result);
					return;
				}

				if (Operation == TEXT("import_heightmap"))
				{
					FString File;
					if (!Body->TryGetStringField(TEXT("file"), File) || File.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'file' is required — a 16-bit greyscale PNG, .raw or .r16 heightmap"));
						return;
					}
					int32 X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
					FString Error;
					if (!ReadRegion(Body, *Info, X1, Y1, X2, Y2, Error))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_extent"), Error);
						return;
					}
					TArray<uint16> FileRaw;
					int32 FileWidth = 0, FileHeight = 0;
					if (!ReadHeightmapFile(File, FileRaw, FileWidth, FileHeight, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("heightmap_unreadable"), Error);
						return;
					}
					const int32 Width = X2 - X1 + 1;
					const int32 Rows = Y2 - Y1 + 1;
					FString Transform;
					Body->TryGetStringField(TEXT("transform"), Transform);
					TArray<uint16> Fitted;
					if (!FitHeightmap(FileRaw, FileWidth, FileHeight, Width, Rows, Transform, Fitted, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("heightmap_fit_failed"), Error);
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "ImportHeightmap", "McpLink Import Heightmap"),
						ShouldTransact(Landscape));
					FLandscapeLayerWriteScope LayerScope(Landscape, Body, Responder);
					if (LayerScope.bFailed)
					{
						return;
					}
					if (!SetRawHeights(*Landscape, X1, Y1, X2, Y2, Fitted, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("wrong_sample_count"), Error);
						return;
					}

					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetStringField(TEXT("file"), File);
					Result->SetNumberField(TEXT("heightmap_width"), FileWidth);
					Result->SetNumberField(TEXT("heightmap_height"), FileHeight);
					Result->SetNumberField(TEXT("min_x"), X1);
					Result->SetNumberField(TEXT("min_y"), Y1);
					Result->SetNumberField(TEXT("max_x"), X2);
					Result->SetNumberField(TEXT("max_y"), Y2);
					Result->SetNumberField(TEXT("vertices_written"), Width * Rows);
					Result->SetBoolField(TEXT("resized"), FileWidth != Width || FileHeight != Rows);
					LayerScope.Annotate(Result);
					Responder->Ok(Result);
					return;
				}

				if (Operation == TEXT("add_layer"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("layer"), Name, Responder,
						TEXT("the weightmap layer name the landscape material paints with")))
					{
						return;
					}
					if (FindLayerInfo(*Info, Name) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("layer_exists"),
							FString::Printf(TEXT("this landscape already has a '%s' layer"), *Name));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddLayer", "McpLink Add Landscape Layer"),
						ShouldTransact(Landscape));

					// A layer needs a LayerInfo asset to be paintable at all.
					FString AssetPath;
					if (!Body->TryGetStringField(TEXT("layer_info"), AssetPath) || AssetPath.IsEmpty())
					{
						AssetPath = FString::Printf(TEXT("/Game/Landscape/LayerInfo/%s_LayerInfo"), *Name);
					}
					ULandscapeLayerInfoObject* LayerInfo = FindLayerInfoAsset(AssetPath);
					if (LayerInfo == nullptr)
					{
						FString Error;
						LayerInfo = Cast<ULandscapeLayerInfoObject>(CreateAsset(
							AssetPath, ULandscapeLayerInfoObject::StaticClass(), nullptr, Error));
						if (LayerInfo == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("layer_info_failed"),
								Error);
							return;
						}
						LayerInfo->LayerName = FName(*Name);
						LayerInfo->MarkPackageDirty();
					}

					Landscape->Modify();
					Landscape->AddTargetLayer(LayerInfo->LayerName, FLandscapeTargetLayerSettings(LayerInfo));
					Info->UpdateLayerInfoMap(Landscape);
					const int32 Index = Info->GetLayerInfoIndex(FName(*Name));
					if (Info->Layers.IsValidIndex(Index))
					{
						Info->Layers[Index].LayerInfoObj = LayerInfo;
					}
					Responder->Ok(LandscapeToJson(*Landscape));
					return;
				}

				if (Operation == TEXT("paint_layer"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("layer"), Name, Responder,
						TEXT("a layer from info's layers list")))
					{
						return;
					}
					ULandscapeLayerInfoObject* LayerInfo = FindLayerInfo(*Info, Name);
					if (LayerInfo == nullptr)
					{
						TArray<FString> Names;
						for (const FLandscapeInfoLayerSettings& Layer : Info->Layers)
						{
							Names.Add(Layer.GetLayerName().ToString());
						}
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("layer_not_found"),
							Names.Num() > 0
								? FString::Printf(
									TEXT("no paintable layer '%s' — this landscape has: %s"),
									*Name, *FString::Join(Names, TEXT(", ")))
								: FString::Printf(
									TEXT("no paintable layer '%s' — add_layer makes one"), *Name));
						return;
					}

					int32 X1 = 0, Y1 = 0, X2 = 0, Y2 = 0;
					FString Error;
					if (!ReadRegion(Body, *Info, X1, Y1, X2, Y2, Error))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_extent"), Error);
						return;
					}
					const int32 Width = X2 - X1 + 1;
					const int32 Rows = Y2 - Y1 + 1;

					double Weight = 1.0;
					Body->TryGetNumberField(TEXT("weight"), Weight);
					TArray<uint8> Data;
					Data.Init(
						static_cast<uint8>(FMath::Clamp(FMath::RoundToDouble(Weight * 255.0), 0.0, 255.0)),
						Width * Rows);

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "PaintLayer", "McpLink Paint Landscape Layer"),
						ShouldTransact(Landscape));
					Landscape->Modify();
					FLandscapeLayerWriteScope LayerScope(Landscape, Body, Responder);
					if (LayerScope.bFailed)
					{
						return;
					}
					{
						FLandscapeEditDataInterface Edit(Info);
						Edit.SetAlphaData(LayerInfo, X1, Y1, X2, Y2, Data.GetData(), Width,
							ELandscapeLayerPaintingRestriction::None);
					}
					Info->ForceLayersFullUpdate();

					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetStringField(TEXT("layer"), Name);
					Result->SetNumberField(TEXT("weight"), Weight);
					Result->SetNumberField(TEXT("vertices_painted"), Width * Rows);
					LayerScope.Annotate(Result);
					Responder->Ok(Result);
					return;
				}

				if (Operation == TEXT("list_edit_layers"))
				{
					Responder->Ok(LandscapeEditLayersJson(*Landscape));
					return;
				}

				if (Operation == TEXT("enable_edit_layers"))
				{
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EnableEditLayers", "McpLink Enable Landscape Edit Layers"), ShouldTransact(Landscape));
					const bool bWasEnabled = Landscape->HasLayersContent();
					if (!bWasEnabled)
					{
						// Moves the existing height and weight data into a
						// first layer, the way the details panel's checkbox does.
						Landscape->Modify();
						Landscape->ToggleCanHaveLayersContent();
					}
					const TSharedRef<FJsonObject> Data = LandscapeEditLayersJson(*Landscape);
					Data->SetStringField(TEXT("message"), bWasEnabled
						? TEXT("edit layers were already on — every 5.8 landscape has them")
						: TEXT("edit layers enabled; the existing data is now the first layer"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_edit_layer"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the new layer's name")))
					{
						return;
					}
					if (!Landscape->HasLayersContent())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_edit_layers"),
							TEXT("the landscape has no edit layers — enable_edit_layers first"));
						return;
					}
					if (Landscape->GetLayerIndex(FName(*Name)) != INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an edit layer named '%s' already exists"), *Name));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddEditLayer", "McpLink Add Landscape Edit Layer"), ShouldTransact(Landscape));
					Landscape->Modify();
					const int32 Index = Landscape->CreateLayer(FName(*Name));
					if (Index == INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("layer_refused"),
							TEXT("the landscape refused a new layer — the edit layer limit is reached"));
						return;
					}
					Landscape->RequestLayersContentUpdateForceAll();
					const TSharedRef<FJsonObject> Data = LandscapeEditLayersJson(*Landscape);
					Data->SetObjectField(TEXT("layer"), LandscapeEditLayerJson(*Landscape, Index));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_edit_layer") || Operation == TEXT("clear_edit_layer")
					|| Operation == TEXT("set_editing_layer") || Operation == TEXT("reorder_edit_layer")
					|| Operation == TEXT("set_edit_layer"))
				{
					int32 Index = INDEX_NONE;
					ULandscapeEditLayerBase* Layer = LandscapeEditLayerOrError(*Landscape, Body, Responder, Index);
					if (Layer == nullptr)
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditLayer", "McpLink Edit Landscape Edit Layer"), ShouldTransact(Landscape));
					Landscape->Modify();
					const FString LayerName = Layer->GetName().ToString();
					if (Operation == TEXT("remove_edit_layer"))
					{
						if (!Landscape->DeleteLayer(Index))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("delete_refused"),
								FString::Printf(TEXT("the landscape refused to delete '%s' — the last layer cannot go"), *LayerName));
							return;
						}
					}
					else if (Operation == TEXT("clear_edit_layer"))
					{
						Landscape->ClearLayer(Index);
					}
					else if (Operation == TEXT("set_editing_layer"))
					{
						Landscape->SetEditingLayer(Layer->GetGuid());
					}
					else if (Operation == TEXT("reorder_edit_layer"))
					{
						const int32 Target = IntOr(Body, TEXT("index"), -1);
						if (!Landscape->GetLayersConst().IsValidIndex(Target))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_index"),
								FString::Printf(TEXT("'index' must be 0..%d"), Landscape->GetLayersConst().Num() - 1));
							return;
						}
						if (!Landscape->ReorderLayer(Index, Target))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("reorder_refused"),
								TEXT("the landscape refused the reorder"));
							return;
						}
						Index = Target;
					}
					else
					{
						FString NewName;
						if (Body->TryGetStringField(TEXT("name"), NewName) && !NewName.IsEmpty())
						{
							Layer->SetName(FName(*NewName), true);
						}
						bool bFlag = false;
						if (Body->TryGetBoolField(TEXT("visible"), bFlag))
						{
							Layer->SetVisible(bFlag, true);
						}
						if (Body->TryGetBoolField(TEXT("locked"), bFlag))
						{
							Layer->SetLocked(bFlag, true);
						}
						double Alpha = 0.0;
						if (Body->TryGetNumberField(TEXT("heightmap_alpha"), Alpha))
						{
							Layer->SetAlphaForTargetType(ELandscapeToolTargetType::Heightmap, static_cast<float>(Alpha), true, EPropertyChangeType::ValueSet);
						}
						if (Body->TryGetNumberField(TEXT("weightmap_alpha"), Alpha))
						{
							Layer->SetAlphaForTargetType(ELandscapeToolTargetType::Weightmap, static_cast<float>(Alpha), true, EPropertyChangeType::ValueSet);
						}
					}
					Landscape->RequestLayersContentUpdateForceAll();
					const TSharedRef<FJsonObject> Data = LandscapeEditLayersJson(*Landscape);
					Data->SetStringField(TEXT("applied"), Operation);
					if (Operation != TEXT("remove_edit_layer") && Landscape->GetLayersConst().IsValidIndex(Index))
					{
						Data->SetObjectField(TEXT("layer"), LandscapeEditLayerJson(*Landscape, Index));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_splines"))
				{
					Responder->Ok(LandscapeSplinesJson(*Landscape));
					return;
				}

				if (Operation == TEXT("add_spline_point"))
				{
					FVector WorldLocation;
					if (!GetVector(Body, TEXT("location"), WorldLocation))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'location' ([x, y, z] in world space) is required"));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddSplinePoint", "McpLink Add Landscape Spline Point"), ShouldTransact(Landscape));
					Landscape->Modify();
					if (Landscape->GetSplinesComponent() == nullptr)
					{
						Landscape->CreateSplineComponent();
					}
					ULandscapeSplinesComponent* Component = Landscape->GetSplinesComponent();
					Component->Modify();
					const int32 ConnectTo = IntOr(Body, TEXT("connect_to"), -1);
					ULandscapeSplineControlPoint* Previous =
						Component->GetControlPoints().IsValidIndex(ConnectTo) ? Component->GetControlPoints()[ConnectTo].Get() : nullptr;
					if (ConnectTo >= 0 && Previous == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("point_not_found"),
							FString::Printf(TEXT("no control point %d — list_splines shows them"), ConnectTo));
						return;
					}
					ULandscapeSplineControlPoint* Point = NewObject<ULandscapeSplineControlPoint>(Component, NAME_None, RF_Transactional);
					Point->Location = Component->GetComponentTransform().InverseTransformPosition(WorldLocation);
					if (Previous != nullptr)
					{
						Point->Rotation = (Point->Location - Previous->Location).Rotation();
						Point->Width = Previous->Width;
						Point->SideFalloff = Previous->SideFalloff;
						Point->EndFalloff = Previous->EndFalloff;
						Point->LayerName = Previous->LayerName;
					}
					ApplyLandscapeSplinePointFields(Body, *Point);
					Component->GetControlPoints().Add(Point);
					const int32 PointIndex = Component->GetControlPoints().Num() - 1;
					int32 SegmentIndex = INDEX_NONE;
					if (Previous != nullptr)
					{
						ULandscapeSplineSegment* Segment = NewObject<ULandscapeSplineSegment>(Component, NAME_None, RF_Transactional);
						Segment->Connections[0].ControlPoint = Previous;
						Segment->Connections[1].ControlPoint = Point;
						const float TangentLen = static_cast<float>((Point->Location - Previous->Location).Size());
						Segment->Connections[0].TangentLen = TangentLen;
						Segment->Connections[1].TangentLen = TangentLen;
						FString MeshPath;
						if (Body->TryGetStringField(TEXT("segment_mesh"), MeshPath) && !MeshPath.IsEmpty())
						{
							if (UStaticMesh* Mesh = Cast<UStaticMesh>(ResolveAsset(MeshPath)))
							{
								FLandscapeSplineMeshEntry Entry;
								Entry.Mesh = Mesh;
								Segment->SplineMeshes.Add(Entry);
							}
						}
						Previous->Modify();
						Previous->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, 0));
						Point->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, 1));
						Component->GetSegments().Add(Segment);
						SegmentIndex = Component->GetSegments().Num() - 1;
						LandscapeSplineNotify(*Previous);
						LandscapeSplineNotify(*Segment);
					}
					LandscapeSplineNotify(*Point);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("landscape"), Landscape->GetPathName());
					Data->SetObjectField(TEXT("point"), LandscapeSplinePointJson(*Component, PointIndex));
					if (SegmentIndex != INDEX_NONE)
					{
						Data->SetObjectField(TEXT("segment"), LandscapeSplineSegmentJson(*Component, SegmentIndex));
					}
					Data->SetNumberField(TEXT("control_points"), Component->GetControlPoints().Num());
					Data->SetNumberField(TEXT("segments"), Component->GetSegments().Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_spline_point") || Operation == TEXT("remove_spline_point")
					|| Operation == TEXT("set_spline_segment") || Operation == TEXT("remove_spline_segment"))
				{
					ULandscapeSplinesComponent* Component = Landscape->GetSplinesComponent();
					const int32 Index = IntOr(Body, TEXT("index"), -1);
					const bool bPoint = Operation.EndsWith(TEXT("_point"));
					const int32 Count = Component == nullptr ? 0 : (bPoint ? Component->GetControlPoints().Num() : Component->GetSegments().Num());
					if (Index < 0 || Index >= Count)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, bPoint ? TEXT("point_not_found") : TEXT("segment_not_found"),
							FString::Printf(TEXT("'index' must be 0..%d — list_splines shows them"), Count - 1));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditSpline", "McpLink Edit Landscape Spline"), ShouldTransact(Landscape));
					Landscape->Modify();
					Component->Modify();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("landscape"), Landscape->GetPathName());
					Data->SetStringField(TEXT("applied"), Operation);
					if (Operation == TEXT("set_spline_point"))
					{
						ULandscapeSplineControlPoint* Point = Component->GetControlPoints()[Index];
						Point->Modify();
						FVector WorldLocation;
						if (GetVector(Body, TEXT("location"), WorldLocation))
						{
							Point->Location = Component->GetComponentTransform().InverseTransformPosition(WorldLocation);
						}
						ApplyLandscapeSplinePointFields(Body, *Point);
						LandscapeSplineNotify(*Point);
						Data->SetObjectField(TEXT("point"), LandscapeSplinePointJson(*Component, Index));
					}
					else if (Operation == TEXT("remove_spline_point"))
					{
						ULandscapeSplineControlPoint* Point = Component->GetControlPoints()[Index];
						Point->Modify();
						for (const FLandscapeSplineConnection& Connection : TArray<FLandscapeSplineConnection>(Point->ConnectedSegments))
						{
							if (Connection.Segment != nullptr)
							{
								RemoveLandscapeSplineSegment(*Component, *Connection.Segment);
							}
						}
						Point->DeleteSplinePoints();
						Component->GetControlPoints().Remove(Point);
					}
					else if (Operation == TEXT("set_spline_segment"))
					{
						ULandscapeSplineSegment* Segment = Component->GetSegments()[Index];
						Segment->Modify();
						const TArray<TSharedPtr<FJsonValue>>* MeshValues = nullptr;
						FString MeshPath;
						if (Body->TryGetArrayField(TEXT("meshes"), MeshValues))
						{
							Segment->SplineMeshes.Empty();
							for (const TSharedPtr<FJsonValue>& Value : *MeshValues)
							{
								FString Path;
								UStaticMesh* Mesh = Value->TryGetString(Path) ? Cast<UStaticMesh>(ResolveAsset(Path)) : nullptr;
								if (Mesh == nullptr)
								{
									Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("mesh_not_found"),
										FString::Printf(TEXT("no Static Mesh at '%s'"), *Path));
									return;
								}
								FLandscapeSplineMeshEntry Entry;
								Entry.Mesh = Mesh;
								Segment->SplineMeshes.Add(Entry);
							}
						}
						else if (Body->TryGetStringField(TEXT("mesh"), MeshPath))
						{
							Segment->SplineMeshes.Empty();
							if (!MeshPath.IsEmpty())
							{
								UStaticMesh* Mesh = Cast<UStaticMesh>(ResolveAsset(MeshPath));
								if (Mesh == nullptr)
								{
									Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("mesh_not_found"),
										FString::Printf(TEXT("no Static Mesh at '%s'"), *MeshPath));
									return;
								}
								FLandscapeSplineMeshEntry Entry;
								Entry.Mesh = Mesh;
								Segment->SplineMeshes.Add(Entry);
							}
						}
						FString Text;
						if (Body->TryGetStringField(TEXT("layer_name"), Text))
						{
							Segment->LayerName = FName(*Text);
						}
						bool bFlag = false;
						if (Body->TryGetBoolField(TEXT("raise_terrain"), bFlag))
						{
							Segment->bRaiseTerrain = bFlag;
						}
						if (Body->TryGetBoolField(TEXT("lower_terrain"), bFlag))
						{
							Segment->bLowerTerrain = bFlag;
						}
						double Tangent = 0.0;
						if (Body->TryGetNumberField(TEXT("start_tangent"), Tangent))
						{
							Segment->Connections[0].TangentLen = static_cast<float>(Tangent);
						}
						if (Body->TryGetNumberField(TEXT("end_tangent"), Tangent))
						{
							Segment->Connections[1].TangentLen = static_cast<float>(Tangent);
						}
						LandscapeSplineNotify(*Segment);
						Data->SetObjectField(TEXT("segment"), LandscapeSplineSegmentJson(*Component, Index));
					}
					else
					{
						RemoveLandscapeSplineSegment(*Component, *Component->GetSegments()[Index]);
					}
					Data->SetNumberField(TEXT("control_points"), Component->GetControlPoints().Num());
					Data->SetNumberField(TEXT("segments"), Component->GetSegments().Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("apply_splines"))
				{
					if (Landscape->GetSplinesComponent() == nullptr || Landscape->GetSplinesComponent()->GetSegments().IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_splines"),
							TEXT("the landscape has no spline segments — add_spline_point with connect_to makes one"));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "ApplySplines", "McpLink Apply Landscape Splines"), ShouldTransact(Landscape));
					Landscape->Modify();
					FLandscapeLayerWriteScope LayerScope(Landscape, Body, Responder);
					if (LayerScope.bFailed)
					{
						return;
					}
					// The spline tool's Apply Splines: raise / lower the terrain
					// to the segments and paint their layer, per the segment and
					// point flags.
					const bool bApplied = Info->ApplySplines();
					const TSharedRef<FJsonObject> Data = LandscapeSplinesJson(*Landscape);
					Data->SetBoolField(TEXT("applied"), bApplied);
					LayerScope.Annotate(Data);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list, create, info, get_heights, set_heights, import_heightmap, ")
						TEXT("add_layer, paint_layer, list_edit_layers, enable_edit_layers, add_edit_layer, set_edit_layer, ")
						TEXT("remove_edit_layer, clear_edit_layer, reorder_edit_layer, set_editing_layer, list_splines, ")
						TEXT("add_spline_point, set_spline_point, remove_spline_point, set_spline_segment, ")
						TEXT("remove_spline_segment, apply_splines or create_grass_type"),
						*Operation));
			});
	}
}
