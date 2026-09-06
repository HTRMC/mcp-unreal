// Remote Control presets: the asset that lists what an external controller
// (the web app, a DMX desk, OSC) may read and write — actors, properties
// and functions exposed by label.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "RemoteControlActor.h"
#include "RemoteControlEntity.h"
#include "RemoteControlField.h"
#include "RemoteControlFieldPath.h"
#include "RemoteControlPreset.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace RemoteControlPresets
	{
		URemoteControlPreset* PresetOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("preset"), Path, Responder, TEXT("a Remote Control Preset asset path")))
			{
				return nullptr;
			}
			URemoteControlPreset* Preset = Cast<URemoteControlPreset>(ResolveAsset(Path));
			if (Preset == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("preset_not_found"),
					FString::Printf(TEXT("no Remote Control Preset at '%s'"), *Path));
			}
			return Preset;
		}

		/// An actor by label or path, or any object by path.
		UObject* TargetOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			if (!RequireString(Body, TEXT("object"), Spec, Responder, TEXT("an actor label or an object path")))
			{
				return nullptr;
			}
			UObject* Object = ResolveObject(Spec);
			if (Object == nullptr)
			{
				if (UWorld* World = ResolveWorld(Body))
				{
					Object = ResolveActor(World, Spec);
				}
			}
			if (Object == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("object_not_found"),
					FString::Printf(TEXT("nothing matches '%s'"), *Spec));
			}
			return Object;
		}

		TSharedRef<FJsonObject> EntityJson(const FRemoteControlEntity& Entity, const TCHAR* Kind)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("kind"), Kind);
			Data->SetStringField(TEXT("label"), Entity.GetLabel().ToString());
			Data->SetStringField(TEXT("id"), Entity.GetId().ToString());
			TArray<TSharedPtr<FJsonValue>> Objects;
			for (UObject* Bound : Entity.GetBoundObjects())
			{
				if (Bound != nullptr)
				{
					Objects.Add(MakeShared<FJsonValueString>(Bound->GetPathName()));
				}
			}
			Data->SetArrayField(TEXT("bound_objects"), Objects);
			return Data;
		}

		TSharedRef<FJsonObject> PresetJson(URemoteControlPreset& Preset)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("preset"), Preset.GetPathName());
			TArray<TSharedPtr<FJsonValue>> Entities;
			for (const TWeakPtr<FRemoteControlProperty>& Weak : Preset.GetExposedEntities<FRemoteControlProperty>())
			{
				if (const TSharedPtr<FRemoteControlProperty> Property = Weak.Pin())
				{
					const TSharedRef<FJsonObject> Entry = EntityJson(*Property, TEXT("property"));
					Entry->SetStringField(TEXT("field"), Property->FieldPathInfo.ToString());
					Entities.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			for (const TWeakPtr<FRemoteControlFunction>& Weak : Preset.GetExposedEntities<FRemoteControlFunction>())
			{
				if (const TSharedPtr<FRemoteControlFunction> Function = Weak.Pin())
				{
					const TSharedRef<FJsonObject> Entry = EntityJson(*Function, TEXT("function"));
					Entry->SetStringField(TEXT("function"), Function->GetFunction() != nullptr ? Function->GetFunction()->GetName() : FString());
					Entities.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			for (const TWeakPtr<FRemoteControlActor>& Weak : Preset.GetExposedEntities<FRemoteControlActor>())
			{
				if (const TSharedPtr<FRemoteControlActor> Actor = Weak.Pin())
				{
					const TSharedRef<FJsonObject> Entry = EntityJson(*Actor, TEXT("actor"));
					Entry->SetStringField(TEXT("actor"), Actor->GetActor() != nullptr ? Actor->GetActor()->GetPathName() : FString());
					Entities.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			Data->SetNumberField(TEXT("count"), Entities.Num());
			Data->SetArrayField(TEXT("exposed"), Entities);
			return Data;
		}
	}

	void RegisterRemoteControlRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace RemoteControlPresets;

		Core.RegisterRoute(TEXT("/api/vp/remote_control"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/RemoteControl/RCP_Stage")))
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
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "CreateRcPreset", "McpLink Create Remote Control Preset"));
					UPackage* Package = CreatePackage(*Path);
					URemoteControlPreset* Preset = NewObject<URemoteControlPreset>(Package,
						FName(*FPackageName::GetShortName(Path)), RF_Public | RF_Standalone | RF_Transactional);
					FAssetRegistryModule::AssetCreated(Preset);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = PresetJson(*Preset);
					Data->SetStringField(TEXT("message"),
						TEXT("created — expose_property / expose_function / expose_actor fill it; the Remote Control web app and protocols read it"));
					Responder->Ok(Data);
					return;
				}

				URemoteControlPreset* Preset = PresetOrError(Body, Responder);
				if (Preset == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					Responder->Ok(PresetJson(*Preset));
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Preset, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = PresetJson(*Preset);
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "EditRcPreset", "McpLink Edit Remote Control Preset"));
				Preset->Modify();

				if (Operation == TEXT("expose_property") || Operation == TEXT("expose_function") || Operation == TEXT("expose_actor"))
				{
					UObject* Object = TargetOrError(Body, Responder);
					if (Object == nullptr)
					{
						return;
					}
					FString Label;
					Body->TryGetStringField(TEXT("label"), Label);
					FRemoteControlPresetExposeArgs Args(Label, FGuid(), true);
					TSharedPtr<FRemoteControlEntity> Exposed;
					const TCHAR* Kind = TEXT("");
					if (Operation == TEXT("expose_actor"))
					{
						AActor* Actor = Cast<AActor>(Object);
						if (Actor == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("not_an_actor"),
								FString::Printf(TEXT("'%s' is not an actor"), *Object->GetPathName()));
							return;
						}
						Exposed = Preset->ExposeActor(Actor, Args).Pin();
						Kind = TEXT("actor");
					}
					else if (Operation == TEXT("expose_property"))
					{
						FString FieldPath;
						if (!RequireString(Body, TEXT("property"), FieldPath, Responder,
								TEXT("a property path on the object, e.g. RelativeLocation or LightComponent.Intensity")))
						{
							return;
						}
						// A path through components is resolved against the
						// actor the way the panel does when a component property
						// is dragged in.
						FRCFieldPathInfo PathInfo(FieldPath);
						if (!PathInfo.Resolve(Object))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("property_not_found"),
								FString::Printf(TEXT("'%s' does not resolve on '%s'"), *FieldPath, *Object->GetPathName()));
							return;
						}
						Exposed = Preset->ExposeProperty(Object, PathInfo, Args).Pin();
						Kind = TEXT("property");
					}
					else
					{
						FString FunctionName;
						if (!RequireString(Body, TEXT("function"), FunctionName, Responder, TEXT("a function of the object's class")))
						{
							return;
						}
						UFunction* Function = Object->FindFunction(FName(*FunctionName));
						if (Function == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("function_not_found"),
								FString::Printf(TEXT("'%s' has no function '%s'"), *Object->GetClass()->GetName(), *FunctionName));
							return;
						}
						Exposed = Preset->ExposeFunction(Object, Function, Args).Pin();
						Kind = TEXT("function");
					}
					if (!Exposed.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("expose_refused"),
							TEXT("the preset refused the entity — is it already exposed under that label?"));
						return;
					}
					Preset->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = EntityJson(*Exposed, Kind);
					Data->SetStringField(TEXT("preset"), Preset->GetPathName());
					Data->SetNumberField(TEXT("count"), PresetJson(*Preset)->GetNumberField(TEXT("count")));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("unexpose") || Operation == TEXT("rename"))
				{
					FString Label;
					if (!RequireString(Body, TEXT("label"), Label, Responder, TEXT("an exposed entity's label from info")))
					{
						return;
					}
					const FGuid Id = Preset->GetExposedEntityId(FName(*Label));
					if (!Id.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("entity_not_found"),
							FString::Printf(TEXT("nothing exposed as '%s' — info lists the labels"), *Label));
						return;
					}
					if (Operation == TEXT("unexpose"))
					{
						Preset->Unexpose(Id);
					}
					else
					{
						FString NewLabel;
						if (!RequireString(Body, TEXT("new_label"), NewLabel, Responder, TEXT("the new label")))
						{
							return;
						}
						Preset->RenameExposedEntity(Id, FName(*NewLabel));
					}
					Preset->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = PresetJson(*Preset);
					Data->SetStringField(TEXT("applied"), Operation);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected create, info, expose_property, expose_function, ")
						TEXT("expose_actor, rename, unexpose or save"), *Operation));
			});
	}
}
