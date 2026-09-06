// Material Parameter Collections: the global scalar and vector parameters a
// material reads without needing an instance, plus their per-world runtime
// values (the ones Blueprint's SetScalarParameterValue writes).

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace McpLink
{
	namespace ParameterCollections
	{
		UMaterialParameterCollection* CollectionOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("collection"), Path, Responder,
					TEXT("a Material Parameter Collection asset path")))
			{
				return nullptr;
			}
			UMaterialParameterCollection* Collection =
				Cast<UMaterialParameterCollection>(ResolveAsset(Path));
			if (Collection == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("collection_not_found"),
					FString::Printf(TEXT("no Material Parameter Collection at '%s'"), *Path));
			}
			return Collection;
		}

		bool ReadLinearColor(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, FLinearColor& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Body->TryGetArrayField(Field, Values) || Values->Num() < 3 || Values->Num() > 4)
			{
				return false;
			}
			float Channels[4] = {0.f, 0.f, 0.f, 1.f};
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				Channels[Index] = static_cast<float>((*Values)[Index]->AsNumber());
			}
			Out = FLinearColor(Channels[0], Channels[1], Channels[2], Channels[3]);
			return true;
		}

		TArray<TSharedPtr<FJsonValue>> ColorJson(const FLinearColor& Color)
		{
			return {
				MakeShared<FJsonValueNumber>(Color.R), MakeShared<FJsonValueNumber>(Color.G),
				MakeShared<FJsonValueNumber>(Color.B), MakeShared<FJsonValueNumber>(Color.A)};
		}

		int32 FindScalar(const UMaterialParameterCollection& Collection, FName Name)
		{
			return Collection.ScalarParameters.IndexOfByPredicate(
				[Name](const FCollectionScalarParameter& P) { return P.ParameterName == Name; });
		}

		int32 FindVector(const UMaterialParameterCollection& Collection, FName Name)
		{
			return Collection.VectorParameters.IndexOfByPredicate(
				[Name](const FCollectionVectorParameter& P) { return P.ParameterName == Name; });
		}

		/// The asset's parameters with their defaults, plus the live value in
		/// World's instance when a world is given.
		TSharedRef<FJsonObject> CollectionJson(UMaterialParameterCollection& Collection, UWorld* World)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("collection"), Collection.GetPathName());
			Data->SetStringField(TEXT("state_id"), Collection.StateId.ToString());
			if (World != nullptr)
			{
				Data->SetStringField(TEXT("world"), World->GetName());
			}
			UMaterialParameterCollectionInstance* Instance =
				World != nullptr ? World->GetParameterCollectionInstance(&Collection) : nullptr;

			TArray<TSharedPtr<FJsonValue>> Scalars;
			for (const FCollectionScalarParameter& Parameter : Collection.ScalarParameters)
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Parameter.ParameterName.ToString());
				Entry->SetStringField(TEXT("id"), Parameter.Id.ToString());
				Entry->SetNumberField(TEXT("default"), Parameter.DefaultValue);
				float Value = 0.f;
				if (Instance != nullptr && Instance->GetScalarParameterValue(Parameter.ParameterName, Value))
				{
					Entry->SetNumberField(TEXT("value"), Value);
				}
				Scalars.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Data->SetArrayField(TEXT("scalars"), Scalars);

			TArray<TSharedPtr<FJsonValue>> Vectors;
			for (const FCollectionVectorParameter& Parameter : Collection.VectorParameters)
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Parameter.ParameterName.ToString());
				Entry->SetStringField(TEXT("id"), Parameter.Id.ToString());
				Entry->SetArrayField(TEXT("default"), ColorJson(Parameter.DefaultValue));
				FLinearColor Value;
				if (Instance != nullptr && Instance->GetVectorParameterValue(Parameter.ParameterName, Value))
				{
					Entry->SetArrayField(TEXT("value"), ColorJson(Value));
				}
				Vectors.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Data->SetArrayField(TEXT("vectors"), Vectors);
			return Data;
		}

		/// The details panel's edit bracket. PreEditChange snapshots the
		/// storage size; PostEditChangeProperty regenerates parameter ids,
		/// dedups names, and — when a parameter was added or removed — mints a
		/// new StateId, rebuilds the uniform buffer layout and recompiles every
		/// material that reads the collection, then pushes the defaults into
		/// every world's instance. Neither is exported (the class is
		/// MinimalAPI); both are virtual overrides of UObject's, so the calls
		/// dispatch through the vtable.
		struct FCollectionEdit
		{
			UObject& Object;
			FProperty* Property;

			FCollectionEdit(UMaterialParameterCollection& Collection, FName PropertyName)
				: Object(Collection)
				, Property(FindFProperty<FProperty>(UMaterialParameterCollection::StaticClass(), PropertyName))
			{
				Object.Modify();
				Object.PreEditChange(Property);
			}

			~FCollectionEdit()
			{
				FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
				Object.PostEditChangeProperty(Event);
				Object.MarkPackageDirty();
			}
		};

		TSharedRef<FJsonObject> ParameterJson(
			const UMaterialParameterCollection& Collection, FName Name, bool bAdded)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("collection"), Collection.GetPathName());
			Data->SetStringField(TEXT("name"), Name.ToString());
			Data->SetBoolField(TEXT("added"), bAdded);
			const int32 ScalarIndex = FindScalar(Collection, Name);
			if (ScalarIndex != INDEX_NONE)
			{
				Data->SetStringField(TEXT("type"), TEXT("scalar"));
				Data->SetStringField(TEXT("id"), Collection.ScalarParameters[ScalarIndex].Id.ToString());
				Data->SetNumberField(TEXT("default"), Collection.ScalarParameters[ScalarIndex].DefaultValue);
			}
			const int32 VectorIndex = FindVector(Collection, Name);
			if (VectorIndex != INDEX_NONE)
			{
				Data->SetStringField(TEXT("type"), TEXT("vector"));
				Data->SetStringField(TEXT("id"), Collection.VectorParameters[VectorIndex].Id.ToString());
				Data->SetArrayField(TEXT("default"), ColorJson(Collection.VectorParameters[VectorIndex].DefaultValue));
			}
			Data->SetStringField(TEXT("state_id"), Collection.StateId.ToString());
			return Data;
		}
	}

	void RegisterMaterialCollectionRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace ParameterCollections;

		Core.RegisterRoute(TEXT("/api/materials/parameter_collection"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/Materials/MPC_Global")))
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
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink",
						"CreateParameterCollection", "McpLink Create Material Parameter Collection"));
					UPackage* Package = CreatePackage(*Path);
					UMaterialParameterCollection* Collection = NewObject<UMaterialParameterCollection>(
						Package, FName(*FPackageName::GetShortName(Path)),
						RF_Public | RF_Standalone | RF_Transactional);
					FAssetRegistryModule::AssetCreated(Collection);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = CollectionJson(*Collection, nullptr);
					Data->SetStringField(TEXT("message"),
						TEXT("created — set_scalar / set_vector add parameters, then save"));
					Responder->Ok(Data);
					return;
				}

				UMaterialParameterCollection* Collection = CollectionOrError(Body, Responder);
				if (Collection == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					Responder->Ok(CollectionJson(*Collection, ResolveWorld(Body)));
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

				if (Operation == TEXT("set_scalar") || Operation == TEXT("set_vector"))
				{
					FString NameString;
					if (!RequireString(Body, TEXT("name"), NameString, Responder, TEXT("the parameter name")))
					{
						return;
					}
					const FName Name(*NameString);
					const bool bScalar = Operation == TEXT("set_scalar");
					// A name is unique across both lists in the editor (the
					// collection compiles them into one uniform buffer), so a
					// scalar cannot be set where a vector already lives.
					if ((bScalar ? FindVector(*Collection, Name) : FindScalar(*Collection, Name)) != INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("type_mismatch"),
							FString::Printf(TEXT("'%s' is already a %s parameter — remove it first"),
								*NameString, bScalar ? TEXT("vector") : TEXT("scalar")));
						return;
					}
					float ScalarValue = 0.f;
					FLinearColor VectorValue = FLinearColor::Black;
					if (bScalar)
					{
						double Number = 0.0;
						if (!Body->TryGetNumberField(TEXT("value"), Number))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'value' is required: the scalar default"));
							return;
						}
						ScalarValue = static_cast<float>(Number);
					}
					else if (!ReadLinearColor(Body, TEXT("value"), VectorValue))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"),
							TEXT("'value' must be [r, g, b] or [r, g, b, a] as linear floats"));
						return;
					}

					const FScopedTransaction Transaction(NSLOCTEXT("McpLink",
						"SetCollectionParameter", "McpLink Set Collection Parameter"));
					bool bAdded = false;
					{
						FCollectionEdit Edit(*Collection, bScalar
							? GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, ScalarParameters)
							: GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, VectorParameters));
						if (bScalar)
						{
							int32 Index = FindScalar(*Collection, Name);
							if (Index == INDEX_NONE)
							{
								FCollectionScalarParameter Parameter;
								Parameter.ParameterName = Name;
								Parameter.Id = FGuid::NewGuid();
								Index = Collection->ScalarParameters.Add(Parameter);
								bAdded = true;
							}
							Collection->ScalarParameters[Index].DefaultValue = ScalarValue;
						}
						else
						{
							int32 Index = FindVector(*Collection, Name);
							if (Index == INDEX_NONE)
							{
								FCollectionVectorParameter Parameter;
								Parameter.ParameterName = Name;
								Parameter.Id = FGuid::NewGuid();
								Index = Collection->VectorParameters.Add(Parameter);
								bAdded = true;
							}
							Collection->VectorParameters[Index].DefaultValue = VectorValue;
						}
					}
					const TSharedRef<FJsonObject> Data = ParameterJson(*Collection, Name, bAdded);
					if (bAdded)
					{
						Data->SetStringField(TEXT("note"),
							TEXT("adding a parameter changes the collection's layout: every material ")
							TEXT("reading it was recompiled and gets a new StateId"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove") || Operation == TEXT("rename"))
				{
					FString NameString;
					if (!RequireString(Body, TEXT("name"), NameString, Responder, TEXT("the parameter name")))
					{
						return;
					}
					const FName Name(*NameString);
					const int32 ScalarIndex = FindScalar(*Collection, Name);
					const int32 VectorIndex = FindVector(*Collection, Name);
					if (ScalarIndex == INDEX_NONE && VectorIndex == INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("parameter_not_found"),
							FString::Printf(TEXT("'%s' has no parameter '%s'"),
								*Collection->GetPathName(), *NameString));
						return;
					}
					FString NewNameString;
					if (Operation == TEXT("rename") && !RequireString(Body, TEXT("new_name"), NewNameString,
							Responder, TEXT("the parameter's new name")))
					{
						return;
					}
					const FName NewName(*NewNameString);
					if (Operation == TEXT("rename")
						&& (FindScalar(*Collection, NewName) != INDEX_NONE || FindVector(*Collection, NewName) != INDEX_NONE))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("'%s' already has a parameter named '%s'"),
								*Collection->GetPathName(), *NewNameString));
						return;
					}

					const FScopedTransaction Transaction(NSLOCTEXT("McpLink",
						"EditCollectionParameter", "McpLink Edit Collection Parameter"));
					{
						FCollectionEdit Edit(*Collection, ScalarIndex != INDEX_NONE
							? GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, ScalarParameters)
							: GET_MEMBER_NAME_CHECKED(UMaterialParameterCollection, VectorParameters));
						if (Operation == TEXT("remove"))
						{
							if (ScalarIndex != INDEX_NONE)
							{
								Collection->ScalarParameters.RemoveAt(ScalarIndex);
							}
							else
							{
								Collection->VectorParameters.RemoveAt(VectorIndex);
							}
						}
						else if (ScalarIndex != INDEX_NONE)
						{
							Collection->ScalarParameters[ScalarIndex].ParameterName = NewName;
						}
						else
						{
							Collection->VectorParameters[VectorIndex].ParameterName = NewName;
						}
					}
					if (Operation == TEXT("remove"))
					{
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("collection"), Collection->GetPathName());
						Data->SetStringField(TEXT("removed"), NameString);
						Data->SetStringField(TEXT("state_id"), Collection->StateId.ToString());
						Data->SetNumberField(TEXT("scalars"), Collection->ScalarParameters.Num());
						Data->SetNumberField(TEXT("vectors"), Collection->VectorParameters.Num());
						Data->SetStringField(TEXT("note"),
							TEXT("materials that read the removed parameter now read zero; recompiled"));
						Responder->Ok(Data);
						return;
					}
					const TSharedRef<FJsonObject> Data = ParameterJson(*Collection, NewName, false);
					Data->SetStringField(TEXT("renamed_from"), NameString);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_value") || Operation == TEXT("get_values"))
				{
					// Runtime values live on the world's instance, not the
					// asset: they reset when the world does and are never
					// saved — the same thing Blueprint's SetScalarParameterValue
					// on a collection touches.
					UWorld* World = ResolveWorldOrError(Body, Responder);
					if (World == nullptr)
					{
						return;
					}
					UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(Collection);
					if (Instance == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_instance"),
							TEXT("the world has no instance of this collection"));
						return;
					}
					if (Operation == TEXT("get_values"))
					{
						Responder->Ok(CollectionJson(*Collection, World));
						return;
					}
					FString NameString;
					if (!RequireString(Body, TEXT("name"), NameString, Responder, TEXT("the parameter name")))
					{
						return;
					}
					const FName Name(*NameString);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("collection"), Collection->GetPathName());
					Data->SetStringField(TEXT("world"), World->GetName());
					Data->SetStringField(TEXT("name"), NameString);
					double Number = 0.0;
					FLinearColor Color;
					bool bSet = false;
					if (Body->TryGetNumberField(TEXT("value"), Number))
					{
						bSet = Instance->SetScalarParameterValue(Name, static_cast<float>(Number));
						Data->SetNumberField(TEXT("value"), Number);
					}
					else if (ReadLinearColor(Body, TEXT("value"), Color))
					{
						bSet = Instance->SetVectorParameterValue(Name, Color);
						Data->SetArrayField(TEXT("value"), ColorJson(Color));
					}
					else
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"),
							TEXT("'value' must be a number (scalar) or [r, g, b, a] (vector)"));
						return;
					}
					if (!bSet)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("parameter_not_found"),
							FString::Printf(TEXT("'%s' has no %s parameter '%s'"), *Collection->GetPathName(),
								Body->HasTypedField<EJson::Number>(TEXT("value")) ? TEXT("scalar") : TEXT("vector"),
								*NameString));
						return;
					}
					Data->SetStringField(TEXT("note"),
						TEXT("a runtime value; it lasts until the world is torn down and is never saved"));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected create, info, set_scalar, ")
						TEXT("set_vector, rename, remove, set_value, get_values or save"), *Operation));
			});
	}
}
