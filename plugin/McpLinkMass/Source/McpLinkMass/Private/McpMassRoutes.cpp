// Mass Entity: entity config assets (a parent plus a list of trait objects),
// Mass Spawner actors carrying entity types and spawn-data generators, and
// spawning/despawning in a running world.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "JsonObjectConverter.h"
#include "MassEntityConfigAsset.h"
#include "MassEntityManager.h"
#include "MassEntitySpawnDataGeneratorBase.h"
#include "MassEntitySubsystem.h"
#include "MassEntityTraitBase.h"
#include "MassSpawner.h"
#include "MassSpawnerTypes.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace MassEntities
	{
		UClass* FindSubclass(UClass* Base, const FString& Spec)
		{
			if (UClass* Direct = ResolveClass(Spec); Direct != nullptr && Direct->IsChildOf(Base))
			{
				return Direct;
			}
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->IsChildOf(Base) && !It->HasAnyClassFlags(CLASS_Abstract) && It->GetName().Equals(Spec, ESearchCase::IgnoreCase))
				{
					return *It;
				}
			}
			return nullptr;
		}

		TArray<TSharedPtr<FJsonValue>> SubclassesJson(UClass* Base)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (!It->IsChildOf(Base) || It->HasAnyClassFlags(CLASS_Abstract) || *It == Base
					|| It->GetName().StartsWith(TEXT("SKEL_")) || It->GetName().StartsWith(TEXT("REINST_")))
				{
					continue;
				}
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("class"), It->GetName());
				Entry->SetStringField(TEXT("path"), It->GetPathName());
				Entry->SetStringField(TEXT("module"), It->GetOutermost()->GetName());
				Out.Add(MakeShared<FJsonValueObject>(Entry));
			}
			return Out;
		}

		TSharedRef<FJsonObject> TraitJson(const UMassEntityTraitBase* Trait)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("class"), Trait->GetClass()->GetName());
			Entry->SetStringField(TEXT("path"), Trait->GetPathName());
			const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			FJsonObjectConverter::UStructToJsonObject(Trait->GetClass(), Trait, Properties, CPF_Edit, CPF_InstancedReference,
				nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
			Entry->SetObjectField(TEXT("properties"), Properties);
			return Entry;
		}

		TSharedRef<FJsonObject> ConfigJson(UMassEntityConfigAsset* Asset)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("config"), Asset->GetPathName());
			const FMassEntityConfig& Config = Asset->GetConfig();
			Data->SetStringField(TEXT("parent"), Config.GetParent() != nullptr ? Config.GetParent()->GetPathName() : FString());
			TArray<TSharedPtr<FJsonValue>> Traits;
			for (const UMassEntityTraitBase* Trait : Config.GetTraits())
			{
				if (Trait != nullptr)
				{
					Traits.Add(MakeShared<FJsonValueObject>(TraitJson(Trait)));
				}
			}
			Data->SetArrayField(TEXT("traits"), Traits);
			// Traits inherited through the parent chain.
			TArray<TSharedPtr<FJsonValue>> Inherited;
			for (const UMassEntityConfigAsset* Parent = Config.GetParent(); Parent != nullptr; Parent = Parent->GetConfig().GetParent())
			{
				for (const UMassEntityTraitBase* Trait : Parent->GetConfig().GetTraits())
				{
					if (Trait != nullptr)
					{
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("class"), Trait->GetClass()->GetName());
						Entry->SetStringField(TEXT("from"), Parent->GetPathName());
						Inherited.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
			}
			Data->SetArrayField(TEXT("inherited_traits"), Inherited);
			return Data;
		}

		// The config and its trait list are protected UPROPERTYs; reach them
		// through reflection so a trait can be removed.
		FScriptArrayHelper* TraitArray(UMassEntityConfigAsset* Asset, FArrayProperty*& OutProperty, void*& OutConfig)
		{
			FStructProperty* ConfigProperty = CastField<FStructProperty>(UMassEntityConfigAsset::StaticClass()->FindPropertyByName(TEXT("Config")));
			if (ConfigProperty == nullptr)
			{
				return nullptr;
			}
			OutConfig = ConfigProperty->ContainerPtrToValuePtr<void>(Asset);
			OutProperty = CastField<FArrayProperty>(ConfigProperty->Struct->FindPropertyByName(TEXT("Traits")));
			if (OutProperty == nullptr)
			{
				return nullptr;
			}
			return new FScriptArrayHelper(OutProperty, OutProperty->ContainerPtrToValuePtr<void>(OutConfig));
		}

		UMassEntityConfigAsset* ConfigOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("config"), Path, Responder, TEXT("a Mass Entity Config asset path")))
			{
				return nullptr;
			}
			UMassEntityConfigAsset* Asset = Cast<UMassEntityConfigAsset>(ResolveAsset(Path));
			if (Asset == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("config_not_found"),
					FString::Printf(TEXT("no Mass Entity Config asset at '%s'"), *Path));
			}
			return Asset;
		}

		AMassSpawner* SpawnerOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder, UWorld*& OutWorld)
		{
			OutWorld = ResolveWorldOrError(Body, Responder);
			if (OutWorld == nullptr)
			{
				return nullptr;
			}
			FString Spec;
			if (!RequireString(Body, TEXT("actor"), Spec, Responder, TEXT("a Mass Spawner actor name or path")))
			{
				return nullptr;
			}
			AMassSpawner* Spawner = Cast<AMassSpawner>(ResolveActor(OutWorld, Spec));
			if (Spawner == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("spawner_not_found"),
					FString::Printf(TEXT("no Mass Spawner actor '%s' in that world"), *Spec));
			}
			return Spawner;
		}

		TSharedRef<FJsonObject> SpawnerJson(AMassSpawner* Spawner)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("actor"), Spawner->GetName());
			Data->SetStringField(TEXT("path"), Spawner->GetPathName());
			Data->SetArrayField(TEXT("location"), VectorToJson(Spawner->GetActorLocation()));
			const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			FJsonObjectConverter::UStructToJsonObject(AMassSpawner::StaticClass(), Spawner, Properties, CPF_Edit, CPF_InstancedReference,
				nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
			Data->SetObjectField(TEXT("properties"), Properties);
			// The instanced generators are skipped by the dump above; name them.
			TArray<TSharedPtr<FJsonValue>> Generators;
			if (FArrayProperty* Property = CastField<FArrayProperty>(AMassSpawner::StaticClass()->FindPropertyByName(TEXT("SpawnDataGenerators"))))
			{
				FScriptArrayHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(Spawner));
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					const FMassSpawnDataGenerator* Entry = reinterpret_cast<const FMassSpawnDataGenerator*>(Helper.GetRawPtr(Index));
					const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
					Json->SetStringField(TEXT("class"), Entry->GeneratorInstance != nullptr ? Entry->GeneratorInstance->GetClass()->GetName() : FString());
					Json->SetNumberField(TEXT("proportion"), Entry->Proportion);
					if (Entry->GeneratorInstance != nullptr)
					{
						const TSharedRef<FJsonObject> GenProps = MakeShared<FJsonObject>();
						FJsonObjectConverter::UStructToJsonObject(Entry->GeneratorInstance->GetClass(), Entry->GeneratorInstance, GenProps,
							CPF_Edit, CPF_InstancedReference, nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
						Json->SetObjectField(TEXT("properties"), GenProps);
					}
					Generators.Add(MakeShared<FJsonValueObject>(Json));
				}
			}
			Data->SetArrayField(TEXT("generators"), Generators);
			// The configured count; the live number is world_entities.
			Data->SetNumberField(TEXT("count"), Spawner->GetCount());
			return Data;
		}

		int32 EntityCount(UWorld* World)
		{
			UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
			if (Subsystem == nullptr)
			{
				return -1;
			}
#if WITH_MASSENTITY_DEBUG
			// Storage size minus the reserved slot and the free list; the
			// arithmetic dips below zero on an untouched manager.
			return FMath::Max(0, Subsystem->GetEntityManager().DebugGetEntityCount());
#else
			return -1;
#endif
		}

		// Applies a JSON object's fields to an object's editable properties.
		bool ApplyProperties(const TSharedRef<FJsonObject>& Body, UObject* Target, FString& OutError)
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Body->TryGetObjectField(TEXT("properties"), Properties) || !Properties->IsValid())
			{
				return true;
			}
			FText Reason;
			if (!FJsonObjectConverter::JsonObjectToUStruct((*Properties).ToSharedRef(), Target->GetClass(), Target, 0, 0, false, &Reason))
			{
				OutError = FString::Printf(TEXT("'properties' did not import onto %s: %s — an FInstancedStruct entry is {\"_structType\": \"/Script/Module.Struct\", ...fields}, e.g. /Script/MassCore.TransformFragment"),
					*Target->GetClass()->GetName(), *Reason.ToString());
				return false;
			}
			return true;
		}
	}

	void RegisterMassRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace MassEntities;

		Core.RegisterRoute(TEXT("/api/mass/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_traits") || Operation == TEXT("list_generators"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					if (Operation == TEXT("list_traits"))
					{
						Data->SetArrayField(TEXT("traits"), SubclassesJson(UMassEntityTraitBase::StaticClass()));
					}
					else
					{
						Data->SetArrayField(TEXT("generators"), SubclassesJson(UMassEntitySpawnDataGeneratorBase::StaticClass()));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_config"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("the new asset's package path, e.g. /Game/Mass/EC_Crowd")))
					{
						return;
					}
					FString Error;
					UMassEntityConfigAsset* Asset = Cast<UMassEntityConfigAsset>(CreateAsset(Path, UMassEntityConfigAsset::StaticClass(), nullptr, Error));
					if (Asset == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					FString ParentPath;
					if (Body->TryGetStringField(TEXT("parent"), ParentPath) && !ParentPath.IsEmpty())
					{
						UMassEntityConfigAsset* Parent = Cast<UMassEntityConfigAsset>(ResolveAsset(ParentPath));
						if (Parent == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("parent_not_found"),
								FString::Printf(TEXT("no Mass Entity Config asset at '%s' to use as the parent"), *ParentPath));
							return;
						}
						Asset->GetMutableConfig().SetParentAsset(*Parent);
					}
					Asset->MarkPackageDirty();
					Responder->Ok(ConfigJson(Asset));
					return;
				}

				if (Operation == TEXT("config_info"))
				{
					UMassEntityConfigAsset* Asset = ConfigOrError(Body, Responder);
					if (Asset == nullptr)
					{
						return;
					}
					Responder->Ok(ConfigJson(Asset));
					return;
				}

				if (Operation == TEXT("add_trait") || Operation == TEXT("set_trait"))
				{
					UMassEntityConfigAsset* Asset = ConfigOrError(Body, Responder);
					if (Asset == nullptr)
					{
						return;
					}
					FString TraitSpec;
					if (!RequireString(Body, TEXT("trait"), TraitSpec, Responder, TEXT("a trait class from list_traits")))
					{
						return;
					}
					UClass* TraitClass = FindSubclass(UMassEntityTraitBase::StaticClass(), TraitSpec);
					if (TraitClass == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("trait_not_found"),
							FString::Printf(TEXT("'%s' is not a Mass trait class — list_traits shows them"), *TraitSpec));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "MassTrait", "McpLink Mass Trait"));
					Asset->Modify();
					UMassEntityTraitBase* Trait = nullptr;
					if (Operation == TEXT("add_trait"))
					{
						// Returns the existing instance of that class, or adds one.
						Trait = Asset->AddTrait(TraitClass);
					}
					else
					{
						Trait = const_cast<UMassEntityTraitBase*>(Asset->GetConfig().FindTrait(TraitClass, true));
						if (Trait == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("trait_not_found"),
								FString::Printf(TEXT("the config has no %s trait — add_trait creates one"), *TraitClass->GetName()));
							return;
						}
					}
					if (Trait == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("trait_refused"),
							TEXT("the config did not create the trait"));
						return;
					}
					Trait->SetFlags(RF_Transactional);
					Trait->Modify();
					FString Error;
					if (!ApplyProperties(Body, Trait, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"), Error);
						return;
					}
					Asset->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = ConfigJson(Asset);
					Data->SetObjectField(TEXT("trait"), TraitJson(Trait));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_trait"))
				{
					UMassEntityConfigAsset* Asset = ConfigOrError(Body, Responder);
					if (Asset == nullptr)
					{
						return;
					}
					FString TraitSpec;
					if (!RequireString(Body, TEXT("trait"), TraitSpec, Responder, TEXT("the trait's class name")))
					{
						return;
					}
					FArrayProperty* Property = nullptr;
					void* ConfigPtr = nullptr;
					TUniquePtr<FScriptArrayHelper> Helper(TraitArray(Asset, Property, ConfigPtr));
					if (!Helper.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("reflection_failed"),
							TEXT("UMassEntityConfigAsset::Config.Traits is not where this build expects it"));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "MassRemoveTrait", "McpLink Mass Remove Trait"));
					Asset->Modify();
					FObjectProperty* Inner = CastField<FObjectProperty>(Property->Inner);
					int32 Removed = 0;
					for (int32 Index = Helper->Num() - 1; Index >= 0; --Index)
					{
						UObject* Trait = Inner != nullptr ? Inner->GetObjectPropertyValue(Helper->GetRawPtr(Index)) : nullptr;
						if (Trait != nullptr && (Trait->GetClass()->GetName().Equals(TraitSpec, ESearchCase::IgnoreCase)
							|| Trait->GetClass()->GetPathName() == TraitSpec || Trait->GetPathName() == TraitSpec))
						{
							Helper->RemoveValues(Index, 1);
							++Removed;
						}
					}
					if (Removed == 0)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("trait_not_found"),
							FString::Printf(TEXT("the config has no '%s' trait — config_info lists them"), *TraitSpec));
						return;
					}
					Asset->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = ConfigJson(Asset);
					Data->SetNumberField(TEXT("removed"), Removed);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("validate_config"))
				{
					UMassEntityConfigAsset* Asset = ConfigOrError(Body, Responder);
					if (Asset == nullptr)
					{
						return;
					}
					UWorld* World = ResolveWorldOrError(Body, Responder);
					if (World == nullptr)
					{
						return;
					}
					const bool bValid = Asset->GetMutableConfig().ValidateEntityTemplate(*World);
					const TSharedRef<FJsonObject> Data = ConfigJson(Asset);
					Data->SetBoolField(TEXT("valid"), bValid);
					if (!bValid)
					{
						Data->SetStringField(TEXT("note"), TEXT("the template did not validate — the reasons are in the log (get_logs, category LogMass)"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save_config"))
				{
					UMassEntityConfigAsset* Asset = ConfigOrError(Body, Responder);
					if (Asset == nullptr)
					{
						return;
					}
					FString Filename, Error;
					if (!SaveAsset(Asset, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("config"), Asset->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_spawner") || Operation == TEXT("configure_spawner"))
				{
					UWorld* World = nullptr;
					AMassSpawner* Spawner = nullptr;
					if (Operation == TEXT("create_spawner"))
					{
						World = ResolveWorldOrError(Body, Responder);
						if (World == nullptr)
						{
							return;
						}
					}
					else
					{
						Spawner = SpawnerOrError(Body, Responder, World);
						if (Spawner == nullptr)
						{
							return;
						}
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "MassSpawner", "McpLink Mass Spawner"), ShouldTransact(World));
					if (Spawner == nullptr)
					{
						FVector Location = FVector::ZeroVector;
						FRotator Rotation = FRotator::ZeroRotator;
						GetVector(Body, TEXT("location"), Location);
						GetRotator(Body, TEXT("rotation"), Rotation);
						FActorSpawnParameters Params;
						FString Name;
						if (Body->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
						{
							Params.Name = FName(*Name);
							Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
						}
						Spawner = World->SpawnActor<AMassSpawner>(AMassSpawner::StaticClass(), Location, Rotation, Params);
						if (Spawner == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("spawn_failed"),
								TEXT("the world refused to spawn a Mass Spawner (name taken?)"));
							return;
						}
						if (!Name.IsEmpty())
						{
							Spawner->SetActorLabel(Name);
						}
					}
					Spawner->Modify();

					UClass* SpawnerClass = AMassSpawner::StaticClass();
					if (Body->HasTypedField<EJson::Number>(TEXT("count")))
					{
						if (FIntProperty* Property = CastField<FIntProperty>(SpawnerClass->FindPropertyByName(TEXT("Count"))))
						{
							Property->SetPropertyValue_InContainer(Spawner, IntOr(Body, TEXT("count"), 0));
						}
					}
					if (Body->HasTypedField<EJson::Boolean>(TEXT("auto_spawn")))
					{
						if (FBoolProperty* Property = CastField<FBoolProperty>(SpawnerClass->FindPropertyByName(TEXT("bAutoSpawnOnBeginPlay"))))
						{
							Property->SetPropertyValue_InContainer(Spawner, BoolOr(Body, TEXT("auto_spawn"), true));
						}
					}
					const TArray<TSharedPtr<FJsonValue>>* Types = nullptr;
					if (Body->TryGetArrayField(TEXT("entity_types"), Types))
					{
						FArrayProperty* Property = CastField<FArrayProperty>(SpawnerClass->FindPropertyByName(TEXT("EntityTypes")));
						if (Property == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("reflection_failed"), TEXT("AMassSpawner::EntityTypes not found"));
							return;
						}
						FScriptArrayHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(Spawner));
						Helper.EmptyValues();
						for (const TSharedPtr<FJsonValue>& Value : *Types)
						{
							const TSharedPtr<FJsonObject>* Entry = nullptr;
							if (!Value.IsValid() || !Value->TryGetObject(Entry))
							{
								continue;
							}
							FString ConfigPath;
							(*Entry)->TryGetStringField(TEXT("config"), ConfigPath);
							UMassEntityConfigAsset* Config = Cast<UMassEntityConfigAsset>(ResolveAsset(ConfigPath));
							if (Config == nullptr)
							{
								Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("config_not_found"),
									FString::Printf(TEXT("entity_types[].config '%s' is not a Mass Entity Config asset"), *ConfigPath));
								return;
							}
							const int32 Index = Helper.AddValue();
							FMassSpawnedEntityType* Type = reinterpret_cast<FMassSpawnedEntityType*>(Helper.GetRawPtr(Index));
							Type->EntityConfig = Config;
							Type->Proportion = static_cast<float>(DoubleOr((*Entry).ToSharedRef(), TEXT("proportion"), 1.0));
						}
					}
					const TArray<TSharedPtr<FJsonValue>>* Generators = nullptr;
					if (Body->TryGetArrayField(TEXT("generators"), Generators))
					{
						FArrayProperty* Property = CastField<FArrayProperty>(SpawnerClass->FindPropertyByName(TEXT("SpawnDataGenerators")));
						if (Property == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("reflection_failed"), TEXT("AMassSpawner::SpawnDataGenerators not found"));
							return;
						}
						FScriptArrayHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(Spawner));
						Helper.EmptyValues();
						for (const TSharedPtr<FJsonValue>& Value : *Generators)
						{
							const TSharedPtr<FJsonObject>* Entry = nullptr;
							if (!Value.IsValid() || !Value->TryGetObject(Entry))
							{
								continue;
							}
							FString ClassSpec;
							(*Entry)->TryGetStringField(TEXT("class"), ClassSpec);
							UClass* GeneratorClass = FindSubclass(UMassEntitySpawnDataGeneratorBase::StaticClass(), ClassSpec);
							if (GeneratorClass == nullptr)
							{
								Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("generator_not_found"),
									FString::Printf(TEXT("generators[].class '%s' is not a spawn data generator — list_generators shows them"), *ClassSpec));
								return;
							}
							UMassEntitySpawnDataGeneratorBase* Instance = NewObject<UMassEntitySpawnDataGeneratorBase>(Spawner, GeneratorClass,
								NAME_None, RF_Transactional);
							FString Error;
							if (!ApplyProperties((*Entry).ToSharedRef(), Instance, Error))
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"), Error);
								return;
							}
							const int32 Index = Helper.AddValue();
							FMassSpawnDataGenerator* Generator = reinterpret_cast<FMassSpawnDataGenerator*>(Helper.GetRawPtr(Index));
							// GeneratorClass is the legacy upgrade path: AMassSpawner::PostLoad
							// replaces the instance with a fresh default of that class whenever
							// it is set, which is how a PIE copy came back with default values.
							Generator->GeneratorClass = nullptr;
							Generator->GeneratorInstance = Instance;
							Generator->Proportion = static_cast<float>(DoubleOr((*Entry).ToSharedRef(), TEXT("proportion"), 1.0));
						}
					}
					Spawner->PostEditChange();
					Spawner->MarkPackageDirty();
					Responder->Ok(SpawnerJson(Spawner));
					return;
				}

				if (Operation == TEXT("spawner_info"))
				{
					UWorld* World = nullptr;
					AMassSpawner* Spawner = SpawnerOrError(Body, Responder, World);
					if (Spawner == nullptr)
					{
						return;
					}
					const TSharedRef<FJsonObject> Data = SpawnerJson(Spawner);
					Data->SetNumberField(TEXT("world_entities"), EntityCount(World));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("do_spawning") || Operation == TEXT("do_despawning"))
				{
					UWorld* World = nullptr;
					AMassSpawner* Spawner = SpawnerOrError(Body, Responder, World);
					if (Spawner == nullptr)
					{
						return;
					}
					if (!World->HasBegunPlay())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_playing"),
							TEXT("Mass entities live in a running world — start PIE and address the spawner with world='pie'"));
						return;
					}
					if (Operation == TEXT("do_spawning"))
					{
						Spawner->DoSpawning();
					}
					else
					{
						Spawner->DoDespawning();
					}
					const TSharedRef<FJsonObject> Data = SpawnerJson(Spawner);
					Data->SetNumberField(TEXT("world_entities"), EntityCount(World));
					Data->SetStringField(TEXT("note"), TEXT("spawn data generators run asynchronously (EQS ones over frames) — read spawner_info after a tick for the final count"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("entity_stats"))
				{
					UWorld* World = ResolveWorldOrError(Body, Responder);
					if (World == nullptr)
					{
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					UMassEntitySubsystem* Subsystem = World->GetSubsystem<UMassEntitySubsystem>();
					Data->SetBoolField(TEXT("has_subsystem"), Subsystem != nullptr);
					Data->SetNumberField(TEXT("entities"), EntityCount(World));
					TArray<TSharedPtr<FJsonValue>> Spawners;
					for (TActorIterator<AMassSpawner> It(World); It; ++It)
					{
						Spawners.Add(MakeShared<FJsonValueObject>(SpawnerJson(*It)));
					}
					Data->SetArrayField(TEXT("spawners"), Spawners);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected list_traits, list_generators, create_config, config_info, add_trait, set_trait, ")
						TEXT("remove_trait, validate_config, save_config, create_spawner, configure_spawner, spawner_info, do_spawning, do_despawning or entity_stats"),
						*Operation));
			});
	}
}
