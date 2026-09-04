// Niagara interop: discover systems, spawn them into a world, drive user
// parameters, and inspect live components.
//
// Parameter values are typed from the system's exposed ("User.") parameter
// store, so a JSON number/bool/array/string lands in the right SetVariable*.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "NiagaraComponent.h"
#include "NiagaraComponentPoolMethodEnum.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace
	{
		UNiagaraSystem* SystemOrError(const FString& Spec, const TSharedRef<FMcpResponder>& Responder)
		{
			UNiagaraSystem* System = Cast<UNiagaraSystem>(ResolveAsset(Spec));
			if (System == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("system_not_found"),
					FString::Printf(
						TEXT("no Niagara system at '%s' — use niagara_ops list_systems (engine templates live under /Niagara)"),
						*Spec));
			}
			return System;
		}

		UNiagaraComponent* ComponentOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString ComponentPath, ActorSpec;
			Body->TryGetStringField(TEXT("component"), ComponentPath);
			Body->TryGetStringField(TEXT("actor"), ActorSpec);
			if (!ComponentPath.IsEmpty())
			{
				if (UNiagaraComponent* Component = Cast<UNiagaraComponent>(ResolveObject(ComponentPath)))
				{
					return Component;
				}
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("component_not_found"),
					FString::Printf(TEXT("no NiagaraComponent at '%s' — see niagara_ops list_components"), *ComponentPath));
				return nullptr;
			}
			if (!ActorSpec.IsEmpty())
			{
				UWorld* World = ResolveWorld(Body);
				AActor* Actor = World ? ResolveActor(World, ActorSpec) : nullptr;
				UNiagaraComponent* Component =
					Actor ? Actor->FindComponentByClass<UNiagaraComponent>() : nullptr;
				if (Component == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("component_not_found"),
						FString::Printf(TEXT("actor '%s' not found or has no NiagaraComponent"), *ActorSpec));
				}
				return Component;
			}
			Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
				TEXT("'component' (object path) or 'actor' is required"));
			return nullptr;
		}

		FString UserName(const FString& Name)
		{
			return Name.StartsWith(TEXT("User.")) ? Name : TEXT("User.") + Name;
		}

		TSharedPtr<FJsonValue> Numbers(std::initializer_list<double> Values)
		{
			TArray<TSharedPtr<FJsonValue>> Array;
			for (double Value : Values)
			{
				Array.Add(MakeShared<FJsonValueNumber>(Value));
			}
			return MakeShared<FJsonValueArray>(Array);
		}

		/// Current value of a store parameter as JSON, by Niagara type.
		TSharedPtr<FJsonValue> ParameterValue(const FNiagaraParameterStore& Store, const FNiagaraVariableBase& Var)
		{
			const FNiagaraTypeDefinition& Type = Var.GetType();
			if (Var.IsDataInterface() || Var.IsUObject())
			{
				return MakeShared<FJsonValueString>(FString::Printf(TEXT("<%s>"), *Type.GetName()));
			}
			const uint8* Data = Store.GetParameterData(Var);
			if (Data == nullptr)
			{
				return MakeShared<FJsonValueNull>();
			}
			if (Type == FNiagaraTypeDefinition::GetFloatDef())
			{
				return MakeShared<FJsonValueNumber>(*reinterpret_cast<const float*>(Data));
			}
			if (Type == FNiagaraTypeDefinition::GetIntDef())
			{
				return MakeShared<FJsonValueNumber>(*reinterpret_cast<const int32*>(Data));
			}
			if (Type == FNiagaraTypeDefinition::GetBoolDef())
			{
				return MakeShared<FJsonValueBoolean>(*reinterpret_cast<const int32*>(Data) != 0);
			}
			if (Type == FNiagaraTypeDefinition::GetVec2Def())
			{
				const FVector2f& V = *reinterpret_cast<const FVector2f*>(Data);
				return Numbers({V.X, V.Y});
			}
			if (Type == FNiagaraTypeDefinition::GetVec3Def() || Type == FNiagaraTypeDefinition::GetPositionDef())
			{
				const FVector3f& V = *reinterpret_cast<const FVector3f*>(Data);
				return Numbers({V.X, V.Y, V.Z});
			}
			if (Type == FNiagaraTypeDefinition::GetVec4Def() || Type == FNiagaraTypeDefinition::GetColorDef()
				|| Type == FNiagaraTypeDefinition::GetQuatDef())
			{
				const FVector4f& V = *reinterpret_cast<const FVector4f*>(Data);
				return Numbers({V.X, V.Y, V.Z, V.W});
			}
			return MakeShared<FJsonValueString>(FString::Printf(TEXT("<%s>"), *Type.GetName()));
		}

		TSharedRef<FJsonObject> ParametersToJson(const FNiagaraParameterStore& Store)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			TArray<FNiagaraVariable> Variables;
			Store.GetParameters(Variables);
			for (const FNiagaraVariable& Var : Variables)
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("type"), Var.GetType().GetName());
				Item->SetField(TEXT("value"), ParameterValue(Store, Var));
				Object->SetObjectField(Var.GetName().ToString(), Item);
			}
			return Object;
		}

		bool ReadNumbers(const TSharedPtr<FJsonValue>& Value, int32 Count, TArray<double>& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (!Value.IsValid() || !Value->TryGetArray(Array) || Array->Num() != Count)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Element : *Array)
			{
				double Number = 0.0;
				if (!Element.IsValid() || !Element->TryGetNumber(Number))
				{
					return false;
				}
				Out.Add(Number);
			}
			return true;
		}

		/// Apply one "name: value" pair to a component's override parameters.
		bool ApplyParameter(
			UNiagaraComponent* Component, const FString& RawName, const TSharedPtr<FJsonValue>& Value, FString& OutError)
		{
			UNiagaraSystem* System = Component->GetAsset();
			if (System == nullptr)
			{
				OutError = TEXT("component has no system asset");
				return false;
			}
			const FName Name(*UserName(RawName));
			TArray<FNiagaraVariable> Exposed;
			System->GetExposedParameters().GetParameters(Exposed);
			const FNiagaraVariable* Var = Exposed.FindByPredicate(
				[&Name](const FNiagaraVariable& Candidate) { return Candidate.GetName() == Name; });
			if (Var == nullptr)
			{
				TArray<FString> Names;
				for (const FNiagaraVariable& Candidate : Exposed)
				{
					Names.Add(Candidate.GetName().ToString());
				}
				OutError = FString::Printf(TEXT("'%s' is not a user parameter of %s — exposed: %s"),
					*RawName, *System->GetName(), Names.IsEmpty() ? TEXT("(none)") : *FString::Join(Names, TEXT(", ")));
				return false;
			}

			const FNiagaraTypeDefinition& Type = Var->GetType();
			double Number = 0.0;
			bool Flag = false;
			FString Text;
			TArray<double> Parts;
			if (Type == FNiagaraTypeDefinition::GetFloatDef() && Value->TryGetNumber(Number))
			{
				Component->SetVariableFloat(Name, static_cast<float>(Number));
			}
			else if (Type == FNiagaraTypeDefinition::GetIntDef() && Value->TryGetNumber(Number))
			{
				Component->SetVariableInt(Name, static_cast<int32>(Number));
			}
			else if (Type == FNiagaraTypeDefinition::GetBoolDef() && Value->TryGetBool(Flag))
			{
				Component->SetVariableBool(Name, Flag);
			}
			else if (Type == FNiagaraTypeDefinition::GetVec2Def() && ReadNumbers(Value, 2, Parts))
			{
				Component->SetVariableVec2(Name, FVector2D(Parts[0], Parts[1]));
			}
			else if (Type == FNiagaraTypeDefinition::GetVec3Def() && ReadNumbers(Value, 3, Parts))
			{
				Component->SetVariableVec3(Name, FVector(Parts[0], Parts[1], Parts[2]));
			}
			else if (Type == FNiagaraTypeDefinition::GetPositionDef() && ReadNumbers(Value, 3, Parts))
			{
				Component->SetVariablePosition(Name, FVector(Parts[0], Parts[1], Parts[2]));
			}
			else if (Type == FNiagaraTypeDefinition::GetVec4Def() && ReadNumbers(Value, 4, Parts))
			{
				Component->SetVariableVec4(Name, FVector4(Parts[0], Parts[1], Parts[2], Parts[3]));
			}
			else if (Type == FNiagaraTypeDefinition::GetColorDef() && ReadNumbers(Value, 4, Parts))
			{
				Component->SetVariableLinearColor(Name, FLinearColor(Parts[0], Parts[1], Parts[2], Parts[3]));
			}
			else if (Type == FNiagaraTypeDefinition::GetColorDef() && ReadNumbers(Value, 3, Parts))
			{
				Component->SetVariableLinearColor(Name, FLinearColor(Parts[0], Parts[1], Parts[2], 1.0f));
			}
			else if (Type == FNiagaraTypeDefinition::GetQuatDef() && ReadNumbers(Value, 4, Parts))
			{
				Component->SetVariableQuat(Name, FQuat(Parts[0], Parts[1], Parts[2], Parts[3]));
			}
			else if ((Var->IsUObject() || Var->IsDataInterface()) && Value->TryGetString(Text))
			{
				UObject* Object = ResolveAsset(Text);
				if (Object == nullptr)
				{
					OutError = FString::Printf(TEXT("object '%s' for parameter '%s' not found"), *Text, *RawName);
					return false;
				}
				Component->SetVariableObject(Name, Object);
			}
			else
			{
				OutError = FString::Printf(
					TEXT("parameter '%s' is a %s — pass a number (float/int), bool, [x,y] / [x,y,z] / [x,y,z,w] array, or object path"),
					*RawName, *Type.GetName());
				return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> ComponentToJson(UNiagaraComponent* Component, bool bIncludeParameters)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("component"), Component->GetPathName());
			if (const AActor* Owner = Component->GetOwner())
			{
				Object->SetStringField(TEXT("owner"), Owner->GetPathName());
				Object->SetStringField(TEXT("owner_label"), Owner->GetActorLabel());
			}
			Object->SetStringField(TEXT("system"),
				Component->GetAsset() ? Component->GetAsset()->GetPathName() : TEXT(""));
			Object->SetBoolField(TEXT("active"), Component->IsActive());
			Object->SetBoolField(TEXT("complete"), Component->IsComplete());
			Object->SetBoolField(TEXT("paused"), Component->IsPaused());
			// A system compiles on first use (seconds); until then the component
			// reports inactive/complete and activates itself once ready.
			const bool bReady = Component->GetAsset() != nullptr && Component->GetAsset()->IsReadyToRun();
			Object->SetBoolField(TEXT("system_ready"), bReady);
			if (!bReady)
			{
				Object->SetStringField(TEXT("note"),
					TEXT("system is still compiling — it activates automatically when ready; poll component_info"));
			}
			Object->SetArrayField(TEXT("location"), VectorToJson(Component->GetComponentLocation()));
			if (bIncludeParameters)
			{
				Object->SetObjectField(TEXT("override_parameters"),
					ParametersToJson(static_cast<const UNiagaraComponent*>(Component)->GetOverrideParameters()));
			}
			return Object;
		}
	}

	void RegisterNiagaraRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/niagara/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_systems"))
				{
					FString PathPrefix = TEXT("/Game");
					Body->TryGetStringField(TEXT("path_prefix"), PathPrefix);
					double MaxResults = 100;
					Body->TryGetNumberField(TEXT("max_results"), MaxResults);

					IAssetRegistry& Registry =
						FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
					FARFilter Filter;
					Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
					Filter.PackagePaths.Add(FName(*PathPrefix));
					Filter.bRecursivePaths = true;
					Filter.bRecursiveClasses = true;
					TArray<FAssetData> Assets;
					Registry.GetAssets(Filter, Assets);

					TArray<TSharedPtr<FJsonValue>> Results;
					for (const FAssetData& Asset : Assets)
					{
						if (Results.Num() >= FMath::Clamp(static_cast<int32>(MaxResults), 1, 500))
						{
							break;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
						Item->SetStringField(TEXT("path"), Asset.PackageName.ToString());
						Results.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Assets.Num());
					Data->SetArrayField(TEXT("systems"), Results);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("system_info"))
				{
					FString Spec;
					Body->TryGetStringField(TEXT("system"), Spec);
					UNiagaraSystem* System = SystemOrError(Spec, Responder);
					if (!System) { return; }

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("system"), System->GetPathName());
					TArray<TSharedPtr<FJsonValue>> Emitters;
					for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Handle.GetName().ToString());
						Item->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
						Emitters.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetArrayField(TEXT("emitters"), Emitters);
					Data->SetObjectField(TEXT("user_parameters"), ParametersToJson(System->GetExposedParameters()));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_components"))
				{
					UWorld* World = ResolveWorld(Body);
					if (World == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_world"),
							TEXT("no matching world — is PIE running?"));
						return;
					}
					TArray<TSharedPtr<FJsonValue>> Components;
					for (TObjectIterator<UNiagaraComponent> It; It; ++It)
					{
						UNiagaraComponent* Component = *It;
						if (Component == nullptr || Component->IsTemplate() || !IsValid(Component)
							|| Component->GetWorld() != World)
						{
							continue;
						}
						Components.Add(MakeShared<FJsonValueObject>(ComponentToJson(Component, false)));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("world"), World->GetName());
					Data->SetNumberField(TEXT("count"), Components.Num());
					Data->SetArrayField(TEXT("components"), Components);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("spawn"))
				{
					FString Spec;
					Body->TryGetStringField(TEXT("system"), Spec);
					UNiagaraSystem* System = SystemOrError(Spec, Responder);
					if (!System) { return; }
					UWorld* World = ResolveWorld(Body);
					if (World == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_world"),
							TEXT("no matching world — is PIE running?"));
						return;
					}
					FVector Location = FVector::ZeroVector;
					FRotator Rotation = FRotator::ZeroRotator;
					FVector Scale = FVector::OneVector;
					GetVector(Body, TEXT("location"), Location);
					GetRotator(Body, TEXT("rotation"), Rotation);
					GetVector(Body, TEXT("scale"), Scale);
					bool bAutoDestroy = false, bAutoActivate = true;
					Body->TryGetBoolField(TEXT("auto_destroy"), bAutoDestroy);
					Body->TryGetBoolField(TEXT("auto_activate"), bAutoActivate);

					UNiagaraComponent* Component = nullptr;
					FString AttachSpec;
					if (Body->TryGetStringField(TEXT("attach_to"), AttachSpec) && !AttachSpec.IsEmpty())
					{
						AActor* Target = ResolveActor(World, AttachSpec);
						if (Target == nullptr || Target->GetRootComponent() == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
								FString::Printf(TEXT("attach_to actor '%s' not found or has no root component"), *AttachSpec));
							return;
						}
						FString Socket;
						Body->TryGetStringField(TEXT("socket"), Socket);
						Component = UNiagaraFunctionLibrary::SpawnSystemAttached(
							System, Target->GetRootComponent(), FName(*Socket), Location, Rotation, Scale,
							EAttachLocation::KeepRelativeOffset, bAutoDestroy, ENCPoolMethod::None,
							bAutoActivate, /*bPreCullCheck*/ false);
					}
					else
					{
						Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
							World, System, Location, Rotation, Scale, bAutoDestroy, bAutoActivate,
							ENCPoolMethod::None, /*bPreCullCheck*/ false);
					}
					if (Component == nullptr)
					{
						// UNiagaraFunctionLibrary refuses to create components when the
						// app cannot render (-nullrhi) or on a dedicated server.
						if (!FApp::CanEverRender())
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_rendering"),
								TEXT("Niagara cannot spawn systems while the editor runs without rendering (-nullrhi); ")
								TEXT("use a windowed editor for Niagara spawning"));
							return;
						}
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("spawn_failed"),
							TEXT("Niagara returned no component (is the system valid for this world?)"));
						return;
					}

					TArray<FString> Warnings;
					const TSharedPtr<FJsonObject>* Parameters = nullptr;
					if (Body->TryGetObjectField(TEXT("parameters"), Parameters) && Parameters->IsValid())
					{
						for (const auto& Pair : (*Parameters)->Values)
						{
							FString Error;
							if (!ApplyParameter(Component, FString(Pair.Key.ToView()), Pair.Value, Error))
							{
								Warnings.Add(Error);
							}
						}
					}
					const TSharedRef<FJsonObject> Data = ComponentToJson(Component, true);
					if (!Warnings.IsEmpty())
					{
						Data->SetStringField(TEXT("warning"), FString::Join(Warnings, TEXT("; ")));
					}
					Responder->Ok(Data);
					return;
				}

				// ---- everything below targets an existing component ----
				UNiagaraComponent* Component = ComponentOrError(Body, Responder);
				if (!Component) { return; }

				if (Operation == TEXT("component_info"))
				{
					Responder->Ok(ComponentToJson(Component, true));
					return;
				}
				if (Operation == TEXT("set_parameters"))
				{
					const TSharedPtr<FJsonObject>* Parameters = nullptr;
					if (!Body->TryGetObjectField(TEXT("parameters"), Parameters) || !Parameters->IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'parameters' object is required, e.g. {\"SpawnRate\": 200, \"Color\": [1,0,0,1]}"));
						return;
					}
					TArray<FString> Errors;
					int32 Applied = 0;
					for (const auto& Pair : (*Parameters)->Values)
					{
						FString Error;
						if (ApplyParameter(Component, FString(Pair.Key.ToView()), Pair.Value, Error))
						{
							++Applied;
						}
						else
						{
							Errors.Add(Error);
						}
					}
					if (Applied == 0 && !Errors.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("parameters_rejected"),
							FString::Join(Errors, TEXT("; ")));
						return;
					}
					bool bReset = false;
					if (Body->TryGetBoolField(TEXT("reset"), bReset) && bReset)
					{
						Component->ResetSystem();
					}
					const TSharedRef<FJsonObject> Data = ComponentToJson(Component, true);
					Data->SetNumberField(TEXT("applied"), Applied);
					if (!Errors.IsEmpty())
					{
						Data->SetStringField(TEXT("warning"), FString::Join(Errors, TEXT("; ")));
					}
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("activate"))
				{
					bool bReset = false;
					Body->TryGetBoolField(TEXT("reset"), bReset);
					Component->Activate(bReset);
					Responder->Ok(ComponentToJson(Component, false));
					return;
				}
				if (Operation == TEXT("deactivate"))
				{
					Component->Deactivate();
					Responder->Ok(ComponentToJson(Component, false));
					return;
				}
				if (Operation == TEXT("set_paused"))
				{
					bool bPaused = true;
					Body->TryGetBoolField(TEXT("paused"), bPaused);
					Component->SetPaused(bPaused);
					Responder->Ok(ComponentToJson(Component, false));
					return;
				}
				if (Operation == TEXT("destroy"))
				{
					const FString Path = Component->GetPathName();
					Component->DestroyComponent();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("destroyed"), Path);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_systems, system_info, list_components, spawn, ")
						TEXT("component_info, set_parameters, activate, deactivate, set_paused, or destroy"),
						*Operation));
			});
	}
}
