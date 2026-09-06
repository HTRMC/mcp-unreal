// Live Link: the client's sources and subjects, sources added from a factory
// (Message Bus, a device plugin, …) or as virtual-subject containers, virtual
// subjects, and presets that capture or restore the whole setup.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "LiveLinkPreset.h"
#include "LiveLinkPresetTypes.h"
#include "LiveLinkRole.h"
#include "LiveLinkSourceFactory.h"
#include "LiveLinkSourceSettings.h"
#include "LiveLinkTypes.h"
#include "LiveLinkVirtualSubject.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace LiveLinks
	{
		ILiveLinkClient* ClientOrError(const TSharedRef<FMcpResponder>& Responder)
		{
			IModularFeatures& Features = IModularFeatures::Get();
			if (!Features.IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_live_link"),
					TEXT("no Live Link client is running — is the LiveLink plugin enabled?"));
				return nullptr;
			}
			return &Features.GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
		}

		// GetSources() leaves out virtual-subject sources; the client keeps
		// those in a separate list.
		TArray<FGuid> AllSources(ILiveLinkClient& Client)
		{
			TArray<FGuid> Sources = Client.GetSources();
			Sources.Append(Client.GetVirtualSources());
			return Sources;
		}

		TSharedRef<FJsonObject> SourceJson(ILiveLinkClient& Client, const FGuid& Source)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("id"), Source.ToString());
			Entry->SetStringField(TEXT("type"), Client.GetSourceType(Source).ToString());
			Entry->SetStringField(TEXT("status"), Client.GetSourceStatus(Source).ToString());
			Entry->SetStringField(TEXT("machine"), Client.GetSourceMachineName(Source).ToString());
			Entry->SetBoolField(TEXT("virtual"), Client.GetVirtualSources().Contains(Source));
			return Entry;
		}

		TSharedRef<FJsonObject> ClientJson(ILiveLinkClient& Client)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Sources;
			for (const FGuid& Source : AllSources(Client))
			{
				Sources.Add(MakeShared<FJsonValueObject>(SourceJson(Client, Source)));
			}
			Data->SetArrayField(TEXT("sources"), Sources);
			TArray<TSharedPtr<FJsonValue>> Subjects;
			for (const FLiveLinkSubjectKey& Key : Client.GetSubjects(true, true))
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Key.SubjectName.ToString());
				Entry->SetStringField(TEXT("source"), Key.Source.ToString());
				const TSubclassOf<ULiveLinkRole> Role = Client.GetSubjectRole_AnyThread(Key);
				Entry->SetStringField(TEXT("role"), Role != nullptr ? Role->GetName() : FString());
				Entry->SetBoolField(TEXT("enabled"), Client.IsSubjectEnabled(Key, false));
				Subjects.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Data->SetArrayField(TEXT("subjects"), Subjects);
			return Data;
		}

		UClass* FindClassByShortName(UClass* Base, const FString& Spec)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (!It->IsChildOf(Base) || It->HasAnyClassFlags(CLASS_Abstract))
				{
					continue;
				}
				if (It->GetName() == Spec || It->GetPathName() == Spec || It->GetName().Equals(Spec, ESearchCase::IgnoreCase))
				{
					return *It;
				}
			}
			return nullptr;
		}

		bool ParseSourceGuid(const TSharedRef<FJsonObject>& Body, ILiveLinkClient& Client, FGuid& Out)
		{
			FString Spec;
			Body->TryGetStringField(TEXT("source"), Spec);
			if (FGuid::Parse(Spec, Out))
			{
				return true;
			}
			// A source may also be named by its type text (a virtual source's
			// type is its name) when there is one of it.
			for (const FGuid& Source : AllSources(Client))
			{
				if (Client.GetSourceType(Source).ToString().Equals(Spec, ESearchCase::IgnoreCase))
				{
					Out = Source;
					return true;
				}
			}
			return false;
		}

		// The settings class of a virtual-subject source lives in a private
		// engine header, so it is matched by name.
		bool IsVirtualSourcePreset(const FLiveLinkSourcePreset& Preset)
		{
			return Preset.Settings != nullptr && Preset.Settings->GetClass()->GetName() == TEXT("LiveLinkVirtualSubjectSourceSettings");
		}

		FName VirtualSourceName(const FLiveLinkSourcePreset& Preset)
		{
			if (const FNameProperty* Property = CastField<FNameProperty>(Preset.Settings->GetClass()->FindPropertyByName(TEXT("SourceName"))))
			{
				return Property->GetPropertyValue_InContainer(Preset.Settings);
			}
			return FName(*Preset.SourceType.ToString());
		}

		// Applies a preset without letting the engine recreate its virtual
		// sources: FLiveLinkVirtualSubjectSource does not override
		// GetSettingsClass, so FLiveLinkClient::CreateSource rebuilds one with
		// plain ULiveLinkSourceSettings and InitializeSettings check()-fails —
		// the editor dies. A transient copy stripped of them goes through the
		// engine; the virtual sources and their subjects are recreated by hand.
		bool ApplyPresetSafely(ULiveLinkPreset* Preset, ILiveLinkClient& Client, bool bReplace, int32& OutVirtualSources, int32& OutVirtualSubjects)
		{
			OutVirtualSources = 0;
			OutVirtualSubjects = 0;
			TArray<FLiveLinkSourcePreset> VirtualSources;
			TArray<FLiveLinkSubjectPreset> VirtualSubjects;
			for (const FLiveLinkSourcePreset& Source : Preset->GetSourcePresets())
			{
				if (IsVirtualSourcePreset(Source))
				{
					VirtualSources.Add(Source);
				}
			}
			ULiveLinkPreset* Working = Preset;
			if (VirtualSources.Num() > 0)
			{
				Working = DuplicateObject<ULiveLinkPreset>(Preset, GetTransientPackage());
				FArrayProperty* SourcesProperty = CastField<FArrayProperty>(ULiveLinkPreset::StaticClass()->FindPropertyByName(TEXT("Sources")));
				FArrayProperty* SubjectsProperty = CastField<FArrayProperty>(ULiveLinkPreset::StaticClass()->FindPropertyByName(TEXT("Subjects")));
				if (SourcesProperty == nullptr || SubjectsProperty == nullptr)
				{
					return false;
				}
				FScriptArrayHelper Sources(SourcesProperty, SourcesProperty->ContainerPtrToValuePtr<void>(Working));
				for (int32 Index = Sources.Num() - 1; Index >= 0; --Index)
				{
					if (IsVirtualSourcePreset(*reinterpret_cast<const FLiveLinkSourcePreset*>(Sources.GetRawPtr(Index))))
					{
						Sources.RemoveValues(Index, 1);
					}
				}
				FScriptArrayHelper Subjects(SubjectsProperty, SubjectsProperty->ContainerPtrToValuePtr<void>(Working));
				for (int32 Index = Subjects.Num() - 1; Index >= 0; --Index)
				{
					const FLiveLinkSubjectPreset& Subject = *reinterpret_cast<const FLiveLinkSubjectPreset*>(Subjects.GetRawPtr(Index));
					if (VirtualSources.ContainsByPredicate([&Subject](const FLiveLinkSourcePreset& S) { return S.Guid == Subject.Key.Source; }))
					{
						VirtualSubjects.Add(Subject);
						Subjects.RemoveValues(Index, 1);
					}
				}
			}
			if (bReplace)
			{
				// ApplyToClient clears the regular sources only; the virtual
				// ones (bar the engine's default container) go here.
				for (const FGuid& Existing : Client.GetVirtualSources())
				{
					if (Client.GetSourceType(Existing).ToString() != TEXT("DefaultVirtualSource"))
					{
						Client.RemoveSource(Existing);
					}
				}
			}
			const bool bApplied = bReplace ? Working->ApplyToClient() : Working->AddToClient(true);
			for (const FLiveLinkSourcePreset& Source : VirtualSources)
			{
				// AddVirtualSubjectSource answers an invalid id when a source of
				// that name already exists, so find it by name in that case.
				const FName Name = VirtualSourceName(Source);
				FGuid NewId = Client.AddVirtualSubjectSource(Name);
				if (!NewId.IsValid())
				{
					for (const FGuid& Existing : Client.GetVirtualSources())
					{
						if (Client.GetSourceType(Existing).ToString() == Name.ToString())
						{
							NewId = Existing;
							break;
						}
					}
				}
				if (!NewId.IsValid())
				{
					continue;
				}
				++OutVirtualSources;
				for (const FLiveLinkSubjectPreset& Subject : VirtualSubjects)
				{
					if (Subject.Key.Source != Source.Guid)
					{
						continue;
					}
					UClass* Class = Subject.VirtualSubject != nullptr ? Subject.VirtualSubject->GetClass() : ULiveLinkVirtualSubject::StaticClass();
					const FLiveLinkSubjectKey Key(NewId, Subject.Key.SubjectName);
					if (Client.AddVirtualSubject(Key, Class))
					{
						Client.SetSubjectEnabled(Key, Subject.bEnabled);
						++OutVirtualSubjects;
					}
				}
			}
			return bApplied;
		}
	}

	void RegisterLiveLinkRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace LiveLinks;

		Core.RegisterRoute(TEXT("/api/vp/live_link"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);
				ILiveLinkClient* Client = ClientOrError(Responder);
				if (Client == nullptr)
				{
					return;
				}

				if (Operation == TEXT("status"))
				{
					Responder->Ok(ClientJson(*Client));
					return;
				}

				if (Operation == TEXT("list_source_types"))
				{
					TArray<TSharedPtr<FJsonValue>> Factories;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						if (!It->IsChildOf(ULiveLinkSourceFactory::StaticClass()) || It->HasAnyClassFlags(CLASS_Abstract)
							|| *It == ULiveLinkSourceFactory::StaticClass())
						{
							continue;
						}
						const ULiveLinkSourceFactory* Factory = It->GetDefaultObject<ULiveLinkSourceFactory>();
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("factory"), It->GetName());
						Entry->SetStringField(TEXT("display_name"), Factory->GetSourceDisplayName().ToString());
						Entry->SetStringField(TEXT("tooltip"), Factory->GetSourceTooltip().ToString());
						Factories.Add(MakeShared<FJsonValueObject>(Entry));
					}
					TArray<TSharedPtr<FJsonValue>> Roles;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						if (It->IsChildOf(ULiveLinkRole::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract) && *It != ULiveLinkRole::StaticClass())
						{
							Roles.Add(MakeShared<FJsonValueString>(It->GetName()));
						}
					}
					TArray<TSharedPtr<FJsonValue>> VirtualClasses;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						if (It->IsChildOf(ULiveLinkVirtualSubject::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract) && *It != ULiveLinkVirtualSubject::StaticClass())
						{
							VirtualClasses.Add(MakeShared<FJsonValueString>(It->GetName()));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("source_factories"), Factories);
					Data->SetArrayField(TEXT("roles"), Roles);
					Data->SetArrayField(TEXT("virtual_subject_classes"), VirtualClasses);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_source"))
				{
					FString FactorySpec, ConnectionString;
					if (!RequireString(Body, TEXT("factory"), FactorySpec, Responder, TEXT("a source factory class from list_source_types")))
					{
						return;
					}
					Body->TryGetStringField(TEXT("connection"), ConnectionString);
					UClass* FactoryClass = FindClassByShortName(ULiveLinkSourceFactory::StaticClass(), FactorySpec);
					if (FactoryClass == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("factory_not_found"),
							FString::Printf(TEXT("'%s' is not a Live Link source factory — list_source_types shows them"), *FactorySpec));
						return;
					}
					// The factory's own connection-string constructor — what a
					// preset stores — so the same string a preset would hold works.
					const TSharedPtr<ILiveLinkSource> Source =
						FactoryClass->GetDefaultObject<ULiveLinkSourceFactory>()->CreateSource(ConnectionString);
					if (!Source.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("source_refused"),
							TEXT("the factory made no source from that connection string — a Message Bus source needs the ")
							TEXT("provider's address, which only the factory's picker discovers; the status route lists what exists"));
						return;
					}
					const FGuid Id = Client->AddSource(Source);
					const TSharedRef<FJsonObject> Data = SourceJson(*Client, Id);
					Data->SetStringField(TEXT("factory"), FactoryClass->GetName());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_virtual_source"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the virtual source's name")))
					{
						return;
					}
					const FGuid Id = Client->AddVirtualSubjectSource(FName(*Name));
					Responder->Ok(SourceJson(*Client, Id));
					return;
				}

				if (Operation == TEXT("remove_source"))
				{
					FGuid Id;
					if (!ParseSourceGuid(Body, *Client, Id) || !AllSources(*Client).Contains(Id))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("source_not_found"),
							TEXT("'source' must be a source id (or unique type) from status"));
						return;
					}
					Client->RemoveSource(Id);
					const TSharedRef<FJsonObject> Data = ClientJson(*Client);
					Data->SetStringField(TEXT("removed"), Id.ToString());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_virtual_subject") || Operation == TEXT("remove_virtual_subject")
					|| Operation == TEXT("set_subject_enabled"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the subject's name")))
					{
						return;
					}
					FGuid SourceId;
					if (!ParseSourceGuid(Body, *Client, SourceId))
					{
						// Without a source, find the subject by name.
						for (const FLiveLinkSubjectKey& Key : Client->GetSubjects(true, true))
						{
							if (Key.SubjectName.ToString() == Name)
							{
								SourceId = Key.Source;
								break;
							}
						}
					}
					const FLiveLinkSubjectKey Key(SourceId, FName(*Name));
					if (Operation == TEXT("add_virtual_subject"))
					{
						if (!SourceId.IsValid())
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'source' (a virtual source id from add_virtual_source) is required"));
							return;
						}
						FString ClassSpec = TEXT("LiveLinkAnimationVirtualSubject");
						Body->TryGetStringField(TEXT("class"), ClassSpec);
						UClass* Class = FindClassByShortName(ULiveLinkVirtualSubject::StaticClass(), ClassSpec);
						if (Class == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("class_not_found"),
								FString::Printf(TEXT("'%s' is not a virtual subject class — list_source_types shows them"), *ClassSpec));
							return;
						}
						if (!Client->AddVirtualSubject(Key, Class))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("subject_refused"),
								TEXT("the client refused the virtual subject — the source must be a virtual-subject source and the name free"));
							return;
						}
					}
					else if (Operation == TEXT("remove_virtual_subject"))
					{
						Client->RemoveVirtualSubject(Key);
					}
					else
					{
						Client->SetSubjectEnabled(Key, BoolOr(Body, TEXT("enabled"), true));
					}
					const TSharedRef<FJsonObject> Data = ClientJson(*Client);
					Data->SetStringField(TEXT("applied"), Operation);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save_preset") || Operation == TEXT("apply_preset"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("preset"), Path, Responder, TEXT("a Live Link Preset asset path")))
					{
						return;
					}
					ULiveLinkPreset* Preset = Cast<ULiveLinkPreset>(ResolveAsset(Path));
					if (Operation == TEXT("apply_preset"))
					{
						if (Preset == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("preset_not_found"),
								FString::Printf(TEXT("no Live Link Preset at '%s'"), *Path));
							return;
						}
						// Additive by default: the sources a preset holds are added
						// next to what runs; 'replace' clears the client first.
						int32 VirtualSources = 0, VirtualSubjects = 0;
						const bool bApplied = ApplyPresetSafely(Preset, *Client, BoolOr(Body, TEXT("replace"), false), VirtualSources, VirtualSubjects);
						const TSharedRef<FJsonObject> Data = ClientJson(*Client);
						Data->SetStringField(TEXT("preset"), Preset->GetPathName());
						Data->SetBoolField(TEXT("applied"), bApplied);
						Data->SetNumberField(TEXT("virtual_sources_recreated"), VirtualSources);
						Data->SetNumberField(TEXT("virtual_subjects_recreated"), VirtualSubjects);
						Responder->Ok(Data);
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "SaveLiveLinkPreset", "McpLink Save Live Link Preset"));
					if (Preset == nullptr)
					{
						if (!FPackageName::IsValidLongPackageName(Path))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_path"),
								FString::Printf(TEXT("'%s' is not a package path — use /Game/Folder/Name"), *Path));
							return;
						}
						UPackage* Package = CreatePackage(*Path);
						Preset = NewObject<ULiveLinkPreset>(Package, FName(*FPackageName::GetShortName(Path)),
							RF_Public | RF_Standalone | RF_Transactional);
						FAssetRegistryModule::AssetCreated(Preset);
					}
					Preset->Modify();
					Preset->BuildFromClient();
					Preset->MarkPackageDirty();
					FString Filename, SaveError;
					if (!SaveAsset(Preset, Filename, SaveError))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), SaveError);
						return;
					}
					const TSharedRef<FJsonObject> Data = ClientJson(*Client);
					Data->SetStringField(TEXT("preset"), Preset->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					TArray<TSharedPtr<FJsonValue>> SourcePresets;
					for (const FLiveLinkSourcePreset& Source : Preset->GetSourcePresets())
					{
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("id"), Source.Guid.ToString());
						Entry->SetStringField(TEXT("type"), Source.SourceType.ToString());
						Entry->SetStringField(TEXT("settings_class"), Source.Settings != nullptr ? Source.Settings->GetClass()->GetName() : FString());
						Entry->SetBoolField(TEXT("virtual"), IsVirtualSourcePreset(Source));
						SourcePresets.Add(MakeShared<FJsonValueObject>(Entry));
					}
					Data->SetArrayField(TEXT("source_presets"), SourcePresets);
					TArray<TSharedPtr<FJsonValue>> SubjectPresets;
					for (const FLiveLinkSubjectPreset& Subject : Preset->GetSubjectPresets())
					{
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("name"), Subject.Key.SubjectName.ToString());
						Entry->SetStringField(TEXT("source"), Subject.Key.Source.ToString());
						Entry->SetStringField(TEXT("role"), Subject.Role != nullptr ? Subject.Role->GetName() : FString());
						Entry->SetBoolField(TEXT("virtual"), Subject.VirtualSubject != nullptr);
						Entry->SetBoolField(TEXT("enabled"), Subject.bEnabled);
						SubjectPresets.Add(MakeShared<FJsonValueObject>(Entry));
					}
					Data->SetArrayField(TEXT("subject_presets"), SubjectPresets);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected status, list_source_types, add_source, add_virtual_source, ")
						TEXT("remove_source, add_virtual_subject, remove_virtual_subject, set_subject_enabled, save_preset or apply_preset"),
						*Operation));
			});
	}
}
