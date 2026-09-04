// Landscape terrain: create a landscape, read and write heights, and manage
// the weightmap layers a landscape material paints with.
//
// Heights cross the wire in centimetres of world Z relative to the landscape
// actor, not as raw uint16 samples: the 0..65535 encoding with its 1/128 cm
// step and 32768 midpoint is an implementation detail no agent should carry.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
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
					Responder->Ok(LandscapeToJson(*Landscape));
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
					Responder->Ok(Result);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list, create, info, get_heights, set_heights, ")
						TEXT("add_layer or paint_layer"),
						*Operation));
			});
	}
}
