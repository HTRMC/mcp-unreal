// World Partition: data layers, and loading regions of a partitioned world in
// the editor.
//
// A World Partition map has no sublevels — actors live in one partitioned
// level and stream by cell. Data layers are the authoring-time grouping that
// replaces sublevels, and the editor loads only the cells inside a loader
// adapter's shape, which is why a freshly opened partitioned map can look
// empty until a region is loaded.

#include "DataLayer/DataLayerEditorSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/DataLayerManager.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionEditorLoaderAdapter.h"

namespace McpLink
{
	namespace Partition
	{
		/// The world's partition, or nullptr after responding with why not.
		UWorldPartition* PartitionOrError(UWorld* World, const TSharedRef<FMcpResponder>& Responder)
		{
			UWorldPartition* Partition = World->GetWorldPartition();
			if (Partition == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_partitioned"),
					FString::Printf(
						TEXT("'%s' is not a World Partition map — its sublevels are the streaming unit, ")
						TEXT("see sublevel_ops"),
						*World->GetName()));
			}
			return Partition;
		}

		UDataLayerEditorSubsystem* DataLayers()
		{
			return GEditor != nullptr ? UDataLayerEditorSubsystem::Get() : nullptr;
		}

		TSharedRef<FJsonObject> DataLayerToJson(const UDataLayerInstance* Instance)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Instance->GetDataLayerShortName());
			Object->SetStringField(TEXT("instance"), Instance->GetDataLayerFName().ToString());
			Object->SetBoolField(TEXT("visible"), Instance->IsVisible());
			Object->SetBoolField(TEXT("loaded_in_editor"), Instance->IsLoadedInEditor());
			Object->SetBoolField(TEXT("is_runtime"), Instance->IsRuntime());
			if (const UEnum* StateEnum = StaticEnum<EDataLayerRuntimeState>())
			{
				Object->SetStringField(TEXT("initial_runtime_state"),
					StateEnum->GetNameStringByValue(
						static_cast<int64>(Instance->GetInitialRuntimeState())));
			}
			// The instance's own settings are ordinary properties on this path.
			Object->SetStringField(TEXT("path"), Instance->GetPathName());
			if (const UDataLayerAsset* Asset = Instance->GetAsset())
			{
				Object->SetStringField(TEXT("asset"), Asset->GetPathName());
			}
			return Object;
		}

		UDataLayerInstance* FindDataLayer(const FString& Spec)
		{
			UDataLayerEditorSubsystem* Subsystem = DataLayers();
			if (Subsystem == nullptr || Spec.IsEmpty())
			{
				return nullptr;
			}
			if (UDataLayerInstance* ByName = Subsystem->GetDataLayerInstance(FName(*Spec)))
			{
				return ByName;
			}
			// Also accept the asset path, which is what create returns.
			if (const UDataLayerAsset* Asset = Cast<UDataLayerAsset>(ResolveAsset(Spec)))
			{
				return Subsystem->GetDataLayerInstance(Asset);
			}
			return nullptr;
		}

		/// Actors named in `actors`, plus the specs that matched nothing.
		TArray<AActor*> ReadActors(
			UWorld* World, const TSharedRef<FJsonObject>& Body, TArray<FString>& OutMissing)
		{
			TArray<AActor*> Actors;
			const TArray<TSharedPtr<FJsonValue>>* Specs = nullptr;
			if (!Body->TryGetArrayField(TEXT("actors"), Specs) || Specs == nullptr)
			{
				return Actors;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Specs)
			{
				FString Spec;
				if (!Value.IsValid() || !Value->TryGetString(Spec))
				{
					continue;
				}
				if (AActor* Actor = ResolveActor(World, Spec))
				{
					Actors.Add(Actor);
				}
				else
				{
					OutMissing.Add(Spec);
				}
			}
			return Actors;
		}
	}

	using namespace Partition;

	void RegisterPartitionRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/world/partition"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				UWorld* World = ResolveWorldOrError(Body, Responder);
				if (World == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("world"), World->GetName());
					UWorldPartition* Partition = World->GetWorldPartition();
					Data->SetBoolField(TEXT("partitioned"), Partition != nullptr);
					if (Partition == nullptr)
					{
						Data->SetStringField(TEXT("note"),
							TEXT("not a World Partition map — sublevel_ops handles its streaming"));
						Responder->Ok(Data);
						return;
					}
					Data->SetBoolField(TEXT("initialized"), Partition->IsInitialized());
					Data->SetBoolField(TEXT("streaming_enabled"), Partition->IsStreamingEnabled());
					Data->SetBoolField(TEXT("streaming_enabled_in_editor"),
						Partition->IsStreamingEnabledInEditor());
					Data->SetStringField(TEXT("editor_name"),
						Partition->GetWorldPartitionEditorName().ToString());

					TArray<TSharedPtr<FJsonValue>> Layers;
					if (const UDataLayerManager* Manager = Partition->GetDataLayerManager())
					{
						Manager->ForEachDataLayerInstance(
							[&Layers](UDataLayerInstance* Instance)
							{
								if (Instance != nullptr)
								{
									Layers.Add(MakeShared<FJsonValueObject>(DataLayerToJson(Instance)));
								}
								return true;
							});
					}
					Data->SetArrayField(TEXT("data_layers"), Layers);
					Responder->Ok(Data);
					return;
				}

				UWorldPartition* Partition = PartitionOrError(World, Responder);
				if (Partition == nullptr)
				{
					return;
				}
				UDataLayerEditorSubsystem* Subsystem = DataLayers();
				if (Subsystem == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_subsystem"),
						TEXT("UDataLayerEditorSubsystem is unavailable"));
					return;
				}

				if (Operation == TEXT("create_data_layer"))
				{
					FString AssetPath;
					if (!RequireString(Body, TEXT("asset"), AssetPath, Responder,
							TEXT("package path for the Data Layer asset, e.g. /Game/DataLayers/DL_Props")))
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateDataLayer", "McpLink Create Data Layer"));
					UDataLayerAsset* Asset = Cast<UDataLayerAsset>(ResolveAsset(AssetPath));
					if (Asset == nullptr)
					{
						FString Error;
						Asset = Cast<UDataLayerAsset>(
							CreateAsset(AssetPath, UDataLayerAsset::StaticClass(), nullptr, Error));
						if (Asset == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
							return;
						}
					}
					if (Subsystem->GetDataLayerInstance(Asset) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("this world already has a data layer for '%s'"), *AssetPath));
						return;
					}
					FDataLayerCreationParameters Parameters;
					Parameters.DataLayerAsset = Asset;
					Parameters.WorldDataLayers = World->GetWorldDataLayers();
					UDataLayerInstance* Instance = Subsystem->CreateDataLayerInstance(Parameters);
					if (Instance == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("create_failed"),
							TEXT("the data layer subsystem refused to create the instance"));
						return;
					}
					Responder->Ok(DataLayerToJson(Instance));
					return;
				}

				if (Operation == TEXT("load_region") || Operation == TEXT("unload_region"))
				{
					FVector Min = FVector::ZeroVector;
					FVector Max = FVector::ZeroVector;
					if (!GetVector(Body, TEXT("min"), Min) || !GetVector(Body, TEXT("max"), Max))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'min' and 'max' are required, as [X,Y,Z] world-space corners"));
						return;
					}
					const FBox Region(Min.ComponentMin(Max), Min.ComponentMax(Max));
					// A loader adapter is what actually pulls cells into the
					// editor; the region stays loaded until the adapter is
					// released, so unload_region releases every adapter.
					if (Operation == TEXT("load_region"))
					{
						UWorldPartitionEditorLoaderAdapter* Adapter =
							Partition->CreateEditorLoaderAdapter<FLoaderAdapterShape>(
								World, Region, TEXT("McpLink Region"));
						if (Adapter == nullptr || Adapter->GetLoaderAdapter() == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("load_failed"),
								TEXT("could not create a loader adapter for that region"));
							return;
						}
						Adapter->GetLoaderAdapter()->Load();
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("adapter"), Adapter->GetPathName());
						Data->SetArrayField(TEXT("min"), VectorToJson(Region.Min));
						Data->SetArrayField(TEXT("max"), VectorToJson(Region.Max));
						Data->SetStringField(TEXT("message"),
							TEXT("region loaded — the actors in it are now reachable by the actor tools"));
						Responder->Ok(Data);
						return;
					}

					int32 Released = 0;
					for (TObjectIterator<UWorldPartitionEditorLoaderAdapter> It; It; ++It)
					{
						UWorldPartitionEditorLoaderAdapter* Adapter = *It;
						if (Adapter == nullptr || Adapter->GetWorld() != World
							|| Adapter->GetLoaderAdapter() == nullptr)
						{
							continue;
						}
						Adapter->GetLoaderAdapter()->Unload();
						++Released;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("unloaded_adapters"), Released);
					Responder->Ok(Data);
					return;
				}

				// ---- everything below names an existing data layer ---------
				FString LayerSpec;
				if (!RequireString(Body, TEXT("data_layer"), LayerSpec, Responder,
						TEXT("a data layer name or asset path, from `info`")))
				{
					return;
				}
				UDataLayerInstance* Layer = FindDataLayer(LayerSpec);
				if (Layer == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("data_layer_not_found"),
						FString::Printf(TEXT("this world has no data layer '%s' — see `info`"), *LayerSpec));
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditDataLayer", "McpLink Edit Data Layer"));

				if (Operation == TEXT("delete_data_layer"))
				{
					Subsystem->DeleteDataLayer(Layer);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), LayerSpec);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_data_layer_state"))
				{
					bool bValue = false;
					if (Body->TryGetBoolField(TEXT("visible"), bValue))
					{
						Subsystem->SetDataLayerVisibility(Layer, bValue);
					}
					if (Body->TryGetBoolField(TEXT("loaded_in_editor"), bValue))
					{
						Subsystem->SetDataLayerIsLoadedInEditor(
							Layer, bValue, /*bIsFromUserChange*/ true);
					}
					FString StateName;
					if (Body->TryGetStringField(TEXT("initial_runtime_state"), StateName)
						&& !StateName.IsEmpty())
					{
						const UEnum* StateEnum = StaticEnum<EDataLayerRuntimeState>();
						const int64 Value =
							StateEnum != nullptr ? StateEnum->GetValueByNameString(StateName) : INDEX_NONE;
						if (Value == INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_state"),
								TEXT("'initial_runtime_state' must be Unloaded, Loaded or Activated"));
							return;
						}
						Subsystem->SetDataLayerInitialRuntimeState(
							Layer, static_cast<EDataLayerRuntimeState>(Value));
					}
					Responder->Ok(DataLayerToJson(Layer));
					return;
				}

				if (Operation == TEXT("add_actors") || Operation == TEXT("remove_actors"))
				{
					TArray<FString> Missing;
					TArray<AActor*> Actors = ReadActors(World, Body, Missing);
					if (Actors.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("no_actors"),
							Missing.IsEmpty()
								? TEXT("'actors' must be a non-empty array of actor paths or labels")
								: *FString::Printf(TEXT("none of these actors were found: %s"),
									  *FString::Join(Missing, TEXT(", "))));
						return;
					}
					const bool bAdd = Operation == TEXT("add_actors");
					const bool bOk = bAdd ? Subsystem->AddActorsToDataLayer(Actors, Layer)
										  : Subsystem->RemoveActorsFromDataLayer(Actors, Layer);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("data_layer"), Layer->GetDataLayerShortName());
					Data->SetNumberField(TEXT("actors"), Actors.Num());
					Data->SetBoolField(TEXT("changed"), bOk);
					if (!Missing.IsEmpty())
					{
						Data->SetStringField(TEXT("warning"),
							FString::Printf(TEXT("not found: %s"), *FString::Join(Missing, TEXT(", "))));
					}
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use info, create_data_layer, delete_data_layer, ")
						TEXT("set_data_layer_state, add_actors, remove_actors, load_region, or ")
						TEXT("unload_region"),
						*Operation));
			});
	}
}
