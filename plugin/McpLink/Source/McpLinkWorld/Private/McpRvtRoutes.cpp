// Runtime Virtual Textures: the asset, the volume actor that places one in
// the world, the primitives and landscapes that render into it, and the
// streaming mips the editor's Build button bakes.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/PrimitiveComponent.h"
#include "Components/RuntimeVirtualTextureComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "JsonObjectConverter.h"
#include "LandscapeProxy.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "RHIDefinitions.h"
#include "RuntimeVirtualTextureSetBounds.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "VT/RuntimeVirtualTexture.h"
#include "VT/RuntimeVirtualTextureVolume.h"
#include "VT/VirtualTextureBuilder.h"
#include "VirtualTexturingEditorModule.h"

namespace McpLink
{
	namespace VirtualTextures
	{
		URuntimeVirtualTexture* RvtOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("virtual_texture"), Path, Responder,
					TEXT("a Runtime Virtual Texture asset path")))
			{
				return nullptr;
			}
			URuntimeVirtualTexture* Rvt = Cast<URuntimeVirtualTexture>(ResolveAsset(Path));
			if (Rvt == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("virtual_texture_not_found"),
					FString::Printf(TEXT("no Runtime Virtual Texture at '%s'"), *Path));
			}
			return Rvt;
		}

		URuntimeVirtualTextureComponent* VolumeComponentOrError(
			UWorld* World, const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			if (!RequireString(Body, TEXT("volume"), Spec, Responder,
					TEXT("a Runtime Virtual Texture Volume actor (label or path)")))
			{
				return nullptr;
			}
			AActor* Actor = ResolveActor(World, Spec);
			URuntimeVirtualTextureComponent* Component =
				Actor != nullptr ? Actor->FindComponentByClass<URuntimeVirtualTextureComponent>() : nullptr;
			if (Component == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("volume_not_found"),
					FString::Printf(TEXT("no actor with a Runtime Virtual Texture Component matches '%s'"), *Spec));
			}
			return Component;
		}

		/// Editable properties as JSON — the asset's size, layout and
		/// compression settings without listing them by hand.
		TSharedRef<FJsonObject> RvtJson(URuntimeVirtualTexture& Rvt)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("virtual_texture"), Rvt.GetPathName());
			const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			FJsonObjectConverter::UStructToJsonObject(Rvt.GetClass(), &Rvt, Properties, CPF_Edit, 0,
				nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
			Data->SetObjectField(TEXT("properties"), Properties);
			Data->SetNumberField(TEXT("size"), Rvt.GetSize());
			Data->SetNumberField(TEXT("tile_size"), Rvt.GetTileSize());
			Data->SetNumberField(TEXT("tile_border_size"), Rvt.GetTileBorderSize());
			Data->SetNumberField(TEXT("layer_count"), Rvt.GetLayerCount());
			return Data;
		}

		TSharedRef<FJsonObject> ComponentJson(URuntimeVirtualTextureComponent& Component)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			if (AActor* Owner = Component.GetOwner())
			{
				Data->SetStringField(TEXT("volume"), Owner->GetPathName());
				Data->SetStringField(TEXT("label"), Owner->GetActorLabel());
			}
			Data->SetStringField(TEXT("virtual_texture"),
				Component.GetVirtualTexture() != nullptr ? Component.GetVirtualTexture()->GetPathName() : FString());
			const FTransform& Transform = Component.GetComponentTransform();
			Data->SetArrayField(TEXT("location"), VectorToJson(Transform.GetLocation()));
			Data->SetArrayField(TEXT("rotation"), RotatorToJson(Transform.Rotator()));
			Data->SetArrayField(TEXT("scale"), VectorToJson(Transform.GetScale3D()));
			const AActor* AlignActor = Component.GetBoundsAlignActor().Get();
			Data->SetStringField(TEXT("bounds_align_actor"), AlignActor != nullptr ? AlignActor->GetPathName() : FString());
			Data->SetStringField(TEXT("streaming_texture"),
				Component.GetStreamingTexture() != nullptr ? Component.GetStreamingTexture()->GetPathName() : FString());
			Data->SetBoolField(TEXT("enabled_in_scene"), Component.IsEnabledInScene());
			return Data;
		}

		/// Append Value to the object-array UPROPERTY named PropertyName (the
		/// RuntimeVirtualTextures list on primitives and landscape proxies),
		/// once. False when the class has no such array.
		bool AddToObjectArray(UObject& Object, FName PropertyName, UObject* Value, bool& bOutAdded)
		{
			bOutAdded = false;
			FArrayProperty* Property = FindFProperty<FArrayProperty>(Object.GetClass(), PropertyName);
			FObjectPropertyBase* Inner = Property != nullptr ? CastField<FObjectPropertyBase>(Property->Inner) : nullptr;
			if (Inner == nullptr)
			{
				return false;
			}
			FScriptArrayHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(&Object));
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				if (Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)) == Value)
				{
					return true;
				}
			}
			const int32 Index = Helper.AddValue();
			Inner->SetObjectPropertyValue(Helper.GetRawPtr(Index), Value);
			bOutAdded = true;
			return true;
		}
	}

	void RegisterRvtRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace VirtualTextures;

		Core.RegisterRoute(TEXT("/api/world/virtual_texture"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/VT/RVT_Terrain")))
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
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateRvt", "McpLink Create Runtime Virtual Texture"));
					UPackage* Package = CreatePackage(*Path);
					URuntimeVirtualTexture* Rvt = NewObject<URuntimeVirtualTexture>(Package,
						FName(*FPackageName::GetShortName(Path)), RF_Public | RF_Standalone | RF_Transactional);
					const TSharedPtr<FJsonObject>* Properties = nullptr;
					if (Body->TryGetObjectField(TEXT("properties"), Properties))
					{
						// TileCount / TileSize / TileBorderSize are log2 values,
						// MaterialType an ERuntimeVirtualTextureMaterialType name.
						if (!FJsonObjectConverter::JsonObjectToUStruct(Properties->ToSharedRef(), Rvt->GetClass(), Rvt, 0, 0))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_properties"),
								TEXT("'properties' did not apply — keys are the asset's UPROPERTY names ")
								TEXT("(TileCount, TileSize, TileBorderSize, MaterialType, bCompressTextures, RemoveLowMips, LODGroup)"));
							return;
						}
					}
					Rvt->PostEditChange();
					FAssetRegistryModule::AssetCreated(Rvt);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = RvtJson(*Rvt);
					Data->SetStringField(TEXT("message"),
						TEXT("created — spawn_volume places it, assign makes primitives and landscapes render into it"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("info"))
				{
					URuntimeVirtualTexture* Rvt = RvtOrError(Body, Responder);
					if (Rvt != nullptr)
					{
						Responder->Ok(RvtJson(*Rvt));
					}
					return;
				}

				if (Operation == TEXT("save"))
				{
					URuntimeVirtualTexture* Rvt = RvtOrError(Body, Responder);
					if (Rvt == nullptr)
					{
						return;
					}
					FString Filename, Error;
					if (!SaveAsset(Rvt, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("virtual_texture"), Rvt->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				UWorld* World = ResolveWorldOrError(Body, Responder);
				if (World == nullptr)
				{
					return;
				}

				if (Operation == TEXT("list"))
				{
					IVirtualTexturingEditorModule& Module =
						FModuleManager::LoadModuleChecked<IVirtualTexturingEditorModule>("VirtualTexturingEditor");
					TArray<TSharedPtr<FJsonValue>> Volumes;
					for (URuntimeVirtualTextureComponent* Component : Module.GatherRuntimeVirtualTextureComponents(World))
					{
						if (Component != nullptr)
						{
							const TSharedRef<FJsonObject> Entry = ComponentJson(*Component);
							Entry->SetBoolField(TEXT("streaming_mips_built"),
								Module.HasStreamedMips(EShadingPath::Deferred, Component));
							Volumes.Add(MakeShared<FJsonValueObject>(Entry));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("world"), World->GetName());
					Data->SetArrayField(TEXT("volumes"), Volumes);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("spawn_volume"))
				{
					URuntimeVirtualTexture* Rvt = RvtOrError(Body, Responder);
					if (Rvt == nullptr)
					{
						return;
					}
					AActor* AlignActor = nullptr;
					FString AlignSpec;
					if (Body->TryGetStringField(TEXT("align_actor"), AlignSpec) && !AlignSpec.IsEmpty())
					{
						AlignActor = ResolveActor(World, AlignSpec);
						if (AlignActor == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
								FString::Printf(TEXT("no actor matches '%s'"), *AlignSpec));
							return;
						}
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SpawnRvtVolume", "McpLink Spawn Runtime Virtual Texture Volume"),
						ShouldTransact(World));
					FActorSpawnParameters Params;
					Params.ObjectFlags |= RF_Transactional;
					FVector Location = FVector::ZeroVector;
					GetVector(Body, TEXT("location"), Location);
					ARuntimeVirtualTextureVolume* Volume =
						World->SpawnActor<ARuntimeVirtualTextureVolume>(Location, FRotator::ZeroRotator, Params);
					if (Volume == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("spawn_failed"),
							TEXT("the world refused to spawn a Runtime Virtual Texture Volume"));
						return;
					}
					FString Label;
					if (Body->TryGetStringField(TEXT("name"), Label) && !Label.IsEmpty())
					{
						Volume->SetActorLabel(Label);
					}
					URuntimeVirtualTextureComponent* Component = Volume->FindComponentByClass<URuntimeVirtualTextureComponent>();
					Component->Modify();
					Component->SetVirtualTexture(Rvt);
					if (AlignActor != nullptr)
					{
						Component->SetBoundsAlignActor(AlignActor);
					}
					if (FBoolProperty* Snap = FindFProperty<FBoolProperty>(Component->GetClass(), TEXT("bSnapBoundsToLandscape")))
					{
						Snap->SetPropertyValue_InContainer(Component, BoolOr(Body, TEXT("snap_to_landscape"), AlignActor != nullptr && AlignActor->IsA<ALandscapeProxy>()));
					}
					// The details panel's Set Bounds button: size the volume to
					// the align actor, or to everything that renders into it.
					if (AlignActor != nullptr || BoolOr(Body, TEXT("fit_bounds"), true))
					{
						RuntimeVirtualTexture::SetBounds(Component);
					}
					Component->PostEditChange();
					const TSharedRef<FJsonObject> Data = ComponentJson(*Component);
					Data->SetStringField(TEXT("message"),
						TEXT("spawned — assign adds primitives or a landscape to the texture; ")
						TEXT("set_bounds refits after they move"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_bounds"))
				{
					URuntimeVirtualTextureComponent* Component = VolumeComponentOrError(World, Body, Responder);
					if (Component == nullptr)
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetRvtBounds", "McpLink Set Virtual Texture Bounds"), ShouldTransact(World));
					Component->Modify();
					FString AlignSpec;
					if (Body->TryGetStringField(TEXT("align_actor"), AlignSpec))
					{
						AActor* AlignActor = AlignSpec.IsEmpty() ? nullptr : ResolveActor(World, AlignSpec);
						if (!AlignSpec.IsEmpty() && AlignActor == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
								FString::Printf(TEXT("no actor matches '%s'"), *AlignSpec));
							return;
						}
						Component->SetBoundsAlignActor(AlignActor);
					}
					RuntimeVirtualTexture::SetBounds(Component);
					Component->PostEditChange();
					Responder->Ok(ComponentJson(*Component));
					return;
				}

				if (Operation == TEXT("assign") || Operation == TEXT("unassign"))
				{
					URuntimeVirtualTexture* Rvt = RvtOrError(Body, Responder);
					if (Rvt == nullptr)
					{
						return;
					}
					FString ActorSpec;
					if (!RequireString(Body, TEXT("actor"), ActorSpec, Responder,
							TEXT("the actor whose primitives (or landscape) render into the texture")))
					{
						return;
					}
					AActor* Actor = ResolveActor(World, ActorSpec);
					if (Actor == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
							FString::Printf(TEXT("no actor matches '%s'"), *ActorSpec));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AssignRvt", "McpLink Assign Runtime Virtual Texture"), ShouldTransact(World));
					const bool bAssign = Operation == TEXT("assign");
					TArray<TSharedPtr<FJsonValue>> Targets;
					auto Apply = [&](UObject& Target, const FString& Label)
					{
						FArrayProperty* Property = FindFProperty<FArrayProperty>(Target.GetClass(), TEXT("RuntimeVirtualTextures"));
						if (Property == nullptr)
						{
							return;
						}
						Target.Modify();
						bool bChanged = false;
						if (bAssign)
						{
							AddToObjectArray(Target, TEXT("RuntimeVirtualTextures"), Rvt, bChanged);
						}
						else
						{
							FScriptArrayHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(&Target));
							FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(Property->Inner);
							for (int32 Index = Helper.Num() - 1; Index >= 0; --Index)
							{
								if (Inner != nullptr && Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)) == Rvt)
								{
									Helper.RemoveValues(Index, 1);
									bChanged = true;
								}
							}
						}
						FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
						Target.PostEditChangeProperty(Event);
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("target"), Label);
						Entry->SetBoolField(TEXT("changed"), bChanged);
						Targets.Add(MakeShared<FJsonValueObject>(Entry));
					};
					if (ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(Actor))
					{
						// A landscape carries the list on the proxy, not on its
						// components.
						Apply(*Proxy, Proxy->GetPathName());
					}
					else
					{
						TInlineComponentArray<UPrimitiveComponent*> Primitives;
						Actor->GetComponents(Primitives);
						for (UPrimitiveComponent* Primitive : Primitives)
						{
							Apply(*Primitive, Primitive->GetPathName());
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("virtual_texture"), Rvt->GetPathName());
					Data->SetStringField(TEXT("actor"), Actor->GetPathName());
					Data->SetArrayField(TEXT("targets"), Targets);
					if (Targets.IsEmpty())
					{
						Data->SetStringField(TEXT("note"),
							TEXT("the actor has no primitive components or landscape proxy to render into a virtual texture"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("build_streaming_mips"))
				{
					URuntimeVirtualTextureComponent* Component = VolumeComponentOrError(World, Body, Responder);
					if (Component == nullptr)
					{
						return;
					}
					if (Component->GetStreamingTexture() == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_streaming_texture"),
							TEXT("the volume has no Streaming Texture — asset_ops create a VirtualTextureBuilder ")
							TEXT("and set_property StreamingTexture on the component first"));
						return;
					}
					if (!FApp::CanEverRender())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("headless"),
							TEXT("building streaming mips renders the virtual texture, which needs a windowed editor (no -nullrhi)"));
						return;
					}
					IVirtualTexturingEditorModule& Module =
						FModuleManager::LoadModuleChecked<IVirtualTexturingEditorModule>("VirtualTexturingEditor");
					const bool bBuilt = Module.BuildStreamedMips(EShadingPath::Deferred, Component);
					const TSharedRef<FJsonObject> Data = ComponentJson(*Component);
					Data->SetBoolField(TEXT("built"), bBuilt);
					Data->SetBoolField(TEXT("streaming_mips_built"), Module.HasStreamedMips(EShadingPath::Deferred, Component));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected create, info, save, list, spawn_volume, ")
						TEXT("set_bounds, assign, unassign or build_streaming_mips"), *Operation));
			});
	}
}
