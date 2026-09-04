// Localization targets and cultures.
//
// A localization target is the unit the Localization Dashboard manages: which
// source it gathers from, which cultures it ships, and where the manifest,
// archives and .locres go. The gather and compile *runs* are commandlets — the
// engine's own in-editor task wrappers all take a parent SWindow for their
// progress dialog, so they cannot run headless — and localization_ops runs
// those as a subprocess instead. This route is the target and culture half,
// plus writing the config scripts those commandlets read.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Culture.h"
#include "LocalizationConfigurationScript.h"
#include "LocalizationSettings.h"
#include "LocalizationTargetTypes.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Internationalization/InternationalizationArchive.h"
#include "Internationalization/InternationalizationManifest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Serialization/JsonInternationalizationArchiveSerializer.h"
#include "Serialization/JsonInternationalizationManifestSerializer.h"
#include "UObject/UnrealType.h"
#include "ScopedTransaction.h"

namespace McpLink
{
	namespace Localization
	{
		ULocalizationTargetSet* GameTargets()
		{
			return ULocalizationSettings::GetGameTargetSet();
		}

		ULocalizationTarget* FindTarget(const FString& Name)
		{
			ULocalizationTargetSet* Set = GameTargets();
			if (Set == nullptr)
			{
				return nullptr;
			}
			for (ULocalizationTarget* Target : Set->TargetObjects)
			{
				if (Target != nullptr && Target->Settings.Name.Equals(Name, ESearchCase::IgnoreCase))
				{
					return Target;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> TargetToJson(ULocalizationTarget* Target)
		{
			const FLocalizationTargetSettings& Settings = Target->Settings;
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Settings.Name);
			// Every gather/export setting is a UPROPERTY under here, so
			// set_property on this path configures the target in detail.
			Object->SetStringField(TEXT("path"), Target->GetPathName());
			Object->SetStringField(TEXT("config"),
				LocalizationConfigurationScript::GetGatherTextConfigPath(Target));
			Object->SetStringField(TEXT("data_directory"),
				LocalizationConfigurationScript::GetDataDirectory(Target));

			TArray<TSharedPtr<FJsonValue>> Cultures;
			for (int32 Index = 0; Index < Settings.SupportedCulturesStatistics.Num(); ++Index)
			{
				const FCultureStatistics& Culture = Settings.SupportedCulturesStatistics[Index];
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("culture"), Culture.CultureName);
				Item->SetNumberField(TEXT("words"), Culture.WordCount);
				Item->SetBoolField(TEXT("native"), Index == Settings.NativeCultureIndex);
				Cultures.Add(MakeShared<FJsonValueObject>(Item));
			}
			Object->SetArrayField(TEXT("cultures"), Cultures);

			// A target whose sources are all disabled gathers nothing and says
			// so nowhere, and the one the engine ships is exactly that, so the
			// switches are worth reporting next to the cultures.
			const TSharedRef<FJsonObject> Gather = MakeShared<FJsonObject>();
			Gather->SetBoolField(TEXT("from_text_files"), Settings.GatherFromTextFiles.IsEnabled);
			Gather->SetBoolField(TEXT("from_packages"), Settings.GatherFromPackages.IsEnabled);
			Gather->SetBoolField(TEXT("from_metadata"), Settings.GatherFromMetaData.IsEnabled);
			TArray<TSharedPtr<FJsonValue>> Directories;
			for (const FGatherTextSearchDirectory& Directory :
					Settings.GatherFromTextFiles.SearchDirectories)
			{
				Directories.Add(MakeShared<FJsonValueString>(Directory.Path));
			}
			Gather->SetArrayField(TEXT("text_search_directories"), Directories);
			TArray<TSharedPtr<FJsonValue>> Wildcards;
			for (const FGatherTextIncludePath& Include :
					Settings.GatherFromPackages.IncludePathWildcards)
			{
				Wildcards.Add(MakeShared<FJsonValueString>(Include.Pattern));
			}
			Gather->SetArrayField(TEXT("package_include_wildcards"), Wildcards);
			Object->SetObjectField(TEXT("gather"), Gather);
			Object->SetStringField(TEXT("native_culture"),
				Settings.SupportedCulturesStatistics.IsValidIndex(Settings.NativeCultureIndex)
					? Settings.SupportedCulturesStatistics[Settings.NativeCultureIndex].CultureName
					: FString());
			return Object;
		}

		/// One culture's archive: the source text gathered into the manifest,
		/// paired with its translation. Written by `gather` and read by
		/// `compile`, so editing it in between is what translating a project
		/// actually is.
		///
		/// Loading needs the manifest too: archives at format version AddedKeys
		/// or newer carry keys, older ones are matched by source text through
		/// the manifest, and the serializer refuses without it.
		bool LoadArchive(ULocalizationTarget* Target, const FString& Culture,
			TSharedRef<FInternationalizationArchive> Archive,
			const TSharedRef<FMcpResponder>& Responder, FString& OutPath)
		{
			OutPath = LocalizationConfigurationScript::GetArchivePath(Target, Culture);
			if (!FPaths::FileExists(OutPath))
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("no_archive"),
					FString::Printf(
						TEXT("no archive at %s — add_culture then localization_ops gather writes ")
						TEXT("one; nothing is translatable until the gather has run"),
						*OutPath));
				return false;
			}

			const FString ManifestPath = LocalizationConfigurationScript::GetManifestPath(Target);
			TSharedPtr<FInternationalizationManifest> Manifest;
			if (FPaths::FileExists(ManifestPath))
			{
				Manifest = MakeShared<FInternationalizationManifest>();
				FJsonInternationalizationManifestSerializer::DeserializeManifestFromFile(
					ManifestPath, Manifest.ToSharedRef());
			}
			if (!FJsonInternationalizationArchiveSerializer::DeserializeArchiveFromFile(
					OutPath, Archive, Manifest, nullptr))
			{
				Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("archive_unreadable"),
					FString::Printf(TEXT("could not parse the archive at %s"), *OutPath));
				return false;
			}
			return true;
		}

		/// Target settings live in the project's ini, not in an asset, so a
		/// change is only real once it is written back.
		///
		/// The target *objects* are only a details-view model: what persists is
		/// ULocalizationSettings::GameTargetsSettings, a config array the
		/// settings object rebuilds the objects from on load. So SaveConfig on
		/// the set writes nothing at all (the set has no config properties) —
		/// the copy back into that array lives in the settings object's own
		/// PostEditChangeProperty, which then writes DefaultEditor.ini. Calling
		/// it is how the dashboard's details panel saves, too.
		void SaveTargets(ULocalizationTargetSet* Set)
		{
			for (ULocalizationTarget* Target : Set->TargetObjects)
			{
				if (Target != nullptr)
				{
					LocalizationConfigurationScript::GenerateAllConfigFiles(Target);
				}
			}
			if (UObject* Settings = Set->GetOuter())
			{
				FPropertyChangedEvent Event(nullptr);
				Settings->PostEditChangeProperty(Event);
			}
			GConfig->Flush(false, GEditorIni);
		}
	}

	using namespace Localization;

	void RegisterLocalizationRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/workflow/localization"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				ULocalizationTargetSet* Set = GameTargets();
				if (Set == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_target_set"),
						TEXT("the localization settings are unavailable"));
					return;
				}

				if (Operation == TEXT("list_targets"))
				{
					TArray<TSharedPtr<FJsonValue>> Targets;
					for (ULocalizationTarget* Target : Set->TargetObjects)
					{
						if (Target != nullptr)
						{
							Targets.Add(MakeShared<FJsonValueObject>(TargetToJson(Target)));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("count"), Targets.Num());
					Data->SetArrayField(TEXT("targets"), Targets);
					if (Targets.IsEmpty())
					{
						Data->SetStringField(TEXT("message"),
							TEXT("this project has no localization targets — create_target makes one"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_cultures"))
				{
					FString Contains;
					Body->TryGetStringField(TEXT("name_contains"), Contains);
					const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_results"), 100), 1, 2000);
					TArray<FString> Names;
					FInternationalization::Get().GetCultureNames(Names);
					TArray<TSharedPtr<FJsonValue>> Cultures;
					int32 Matched = 0;
					for (const FString& Name : Names)
					{
						if (!Contains.IsEmpty() && !Name.Contains(Contains))
						{
							continue;
						}
						++Matched;
						if (Cultures.Num() < Max)
						{
							Cultures.Add(MakeShared<FJsonValueString>(Name));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Matched);
					Data->SetArrayField(TEXT("cultures"), Cultures);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_target"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("target"), Name, Responder,
							TEXT("the target's name, e.g. Game")))
					{
						return;
					}
					if (FindTarget(Name) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(
								TEXT("this project already has a target '%s' — note that the engine ")
								TEXT("ships a 'Game' target in BaseEditor.ini with every gather source ")
								TEXT("disabled and no native culture, so an untouched project already ")
								TEXT("has one; configure_gather and set_native_culture make it usable"),
								*Name));
						return;
					}
					ULocalizationTarget* Target =
						NewObject<ULocalizationTarget>(Set, NAME_None, RF_Transactional);
					Target->Settings.Name = Name;
					Target->Settings.Guid = FGuid::NewGuid();
					// A target with no native culture gathers into nothing.
					FString Native = TEXT("en");
					Body->TryGetStringField(TEXT("native_culture"), Native);
					Target->Settings.SupportedCulturesStatistics.Add(FCultureStatistics(Native));
					Target->Settings.NativeCultureIndex = 0;
					// The defaults the dashboard uses for a new game target.
					Target->Settings.GatherFromTextFiles.IsEnabled = true;
					for (const TCHAR* Directory : {TEXT("Source"), TEXT("Config")})
					{
						FGatherTextSearchDirectory& Search =
							Target->Settings.GatherFromTextFiles.SearchDirectories.AddDefaulted_GetRef();
						Search.Path = Directory;
					}
					Target->Settings.GatherFromPackages.IsEnabled = true;
					FGatherTextIncludePath& Include =
						Target->Settings.GatherFromPackages.IncludePathWildcards.AddDefaulted_GetRef();
					Include.Pattern = TEXT("Content/*");
					Set->TargetObjects.Add(Target);
					SaveTargets(Set);

					const TSharedRef<FJsonObject> Data = TargetToJson(Target);
					Data->SetStringField(TEXT("message"),
						TEXT("target created and its commandlet config written — localization_ops ")
						TEXT("gather now has something to run"));
					Responder->Ok(Data);
					return;
				}

				// ---- everything below names an existing target --------------
				FString TargetName;
				if (!RequireString(Body, TEXT("target"), TargetName, Responder,
						TEXT("a target name from list_targets")))
				{
					return;
				}
				ULocalizationTarget* Target = FindTarget(TargetName);
				if (Target == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("target_not_found"),
						FString::Printf(TEXT("no localization target '%s' — see list_targets"),
							*TargetName));
					return;
				}

				if (Operation == TEXT("target_info"))
				{
					Responder->Ok(TargetToJson(Target));
					return;
				}

				if (Operation == TEXT("configure_gather"))
				{
					FLocalizationTargetSettings& Settings = Target->Settings;
					const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;

					if (Body->HasField(TEXT("from_text_files")))
					{
						Settings.GatherFromTextFiles.IsEnabled =
							BoolOr(Body, TEXT("from_text_files"), true);
					}
					if (Body->TryGetArrayField(TEXT("text_search_directories"), Values))
					{
						Settings.GatherFromTextFiles.SearchDirectories.Empty();
						for (const TSharedPtr<FJsonValue>& Value : *Values)
						{
							FGatherTextSearchDirectory& Directory =
								Settings.GatherFromTextFiles.SearchDirectories.AddDefaulted_GetRef();
							Directory.Path = Value->AsString();
						}
					}
					if (Body->HasField(TEXT("from_packages")))
					{
						Settings.GatherFromPackages.IsEnabled =
							BoolOr(Body, TEXT("from_packages"), true);
					}
					if (Body->TryGetArrayField(TEXT("package_include_wildcards"), Values))
					{
						Settings.GatherFromPackages.IncludePathWildcards.Empty();
						for (const TSharedPtr<FJsonValue>& Value : *Values)
						{
							FGatherTextIncludePath& Include =
								Settings.GatherFromPackages.IncludePathWildcards.AddDefaulted_GetRef();
							Include.Pattern = Value->AsString();
						}
					}
					if (Body->HasField(TEXT("from_metadata")))
					{
						Settings.GatherFromMetaData.IsEnabled =
							BoolOr(Body, TEXT("from_metadata"), true);
					}
					SaveTargets(Set);

					const TSharedRef<FJsonObject> Data = TargetToJson(Target);
					Data->SetStringField(TEXT("message"),
						TEXT("gather sources updated and the commandlet config rewritten"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("generate_configs"))
				{
					LocalizationConfigurationScript::GenerateAllConfigFiles(Target);
					const TSharedRef<FJsonObject> Data = TargetToJson(Target);
					Data->SetStringField(TEXT("message"),
						TEXT("commandlet config files rewritten from the target's current settings"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_culture") || Operation == TEXT("remove_culture")
					|| Operation == TEXT("set_native_culture"))
				{
					FString Culture;
					if (!RequireString(Body, TEXT("culture"), Culture, Responder,
							TEXT("a culture code, e.g. fr or pt-BR — see list_cultures")))
					{
						return;
					}
					FLocalizationTargetSettings& Settings = Target->Settings;
					const int32 Index = Settings.SupportedCulturesStatistics.IndexOfByPredicate(
						[&Culture](const FCultureStatistics& Candidate)
						{ return Candidate.CultureName == Culture; });

					if (Operation == TEXT("add_culture"))
					{
						if (Index != INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_supported"),
								FString::Printf(TEXT("'%s' already supports '%s'"), *TargetName, *Culture));
							return;
						}
						if (!FInternationalization::Get().GetCulture(Culture).IsValid())
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_culture"),
								FString::Printf(
									TEXT("'%s' is not a culture this engine knows — see list_cultures"),
									*Culture));
							return;
						}
						Settings.SupportedCulturesStatistics.Add(FCultureStatistics(Culture));
					}
					else if (Operation == TEXT("remove_culture"))
					{
						if (Index == INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("not_supported"),
								FString::Printf(TEXT("'%s' does not support '%s'"), *TargetName, *Culture));
							return;
						}
						if (Index == Settings.NativeCultureIndex)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("is_native"),
								TEXT("that is the native culture — set another one native first"));
							return;
						}
						Settings.SupportedCulturesStatistics.RemoveAt(Index);
						// The native index is a position in the array it just
						// shifted, so it has to move with it.
						if (Settings.NativeCultureIndex > Index)
						{
							--Settings.NativeCultureIndex;
						}
					}
					else
					{
						if (Index == INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("not_supported"),
								FString::Printf(
									TEXT("'%s' does not support '%s' — add_culture it first"),
									*TargetName, *Culture));
							return;
						}
						Settings.NativeCultureIndex = Index;
					}
					SaveTargets(Set);
					Responder->Ok(TargetToJson(Target));
					return;
				}

				if (Operation == TEXT("list_translations")
					|| Operation == TEXT("set_translation"))
				{
					FString Culture;
					if (!RequireString(Body, TEXT("culture"), Culture, Responder,
							TEXT("a culture this target supports — see target_info")))
					{
						return;
					}
					const TSharedRef<FInternationalizationArchive> Archive =
						MakeShared<FInternationalizationArchive>();
					FString ArchivePath;
					if (!LoadArchive(Target, Culture, Archive, Responder, ArchivePath))
					{
						return;
					}

					if (Operation == TEXT("list_translations"))
					{
						FString NamespaceFilter, TextFilter;
						Body->TryGetStringField(TEXT("namespace_contains"), NamespaceFilter);
						Body->TryGetStringField(TEXT("text_contains"), TextFilter);
						const bool bUntranslatedOnly =
							BoolOr(Body, TEXT("untranslated_only"), false);
						const int32 Max =
							FMath::Clamp(IntOr(Body, TEXT("max_results"), 100), 1, 1000);

						TArray<TSharedPtr<FJsonValue>> Items;
						int32 Total = 0;
						int32 Untranslated = 0;
						for (auto It = Archive->GetEntriesByKeyIterator(); It; ++It)
						{
							const TSharedRef<FArchiveEntry>& Entry = It.Value();
							const FString Source = Entry->Source.Text;
							const FString Translation = Entry->Translation.Text;
							// The gather seeds every entry with the source text,
							// so "same as source" is what untranslated looks like.
							const bool bIsUntranslated =
								Translation.IsEmpty() || Translation.Equals(Source);
							++Total;
							if (bIsUntranslated)
							{
								++Untranslated;
							}
							if (bUntranslatedOnly && !bIsUntranslated)
							{
								continue;
							}
							const FString Namespace = Entry->Namespace.GetString();
							if (!NamespaceFilter.IsEmpty() && !Namespace.Contains(NamespaceFilter))
							{
								continue;
							}
							if (!TextFilter.IsEmpty() && !Source.Contains(TextFilter))
							{
								continue;
							}
							if (Items.Num() >= Max)
							{
								continue;
							}
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("namespace"), Namespace);
							Item->SetStringField(TEXT("key"), Entry->Key.GetString());
							Item->SetStringField(TEXT("source"), Source);
							Item->SetStringField(TEXT("translation"), Translation);
							Item->SetBoolField(TEXT("untranslated"), bIsUntranslated);
							Items.Add(MakeShared<FJsonValueObject>(Item));
						}

						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("target"), Target->Settings.Name);
						Data->SetStringField(TEXT("culture"), Culture);
						Data->SetStringField(TEXT("archive"), ArchivePath);
						Data->SetNumberField(TEXT("total"), Total);
						Data->SetNumberField(TEXT("untranslated"), Untranslated);
						Data->SetNumberField(TEXT("returned"), Items.Num());
						Data->SetArrayField(TEXT("entries"), Items);
						Responder->Ok(Data);
						return;
					}

					FString Namespace, Key, Translation;
					Body->TryGetStringField(TEXT("namespace"), Namespace);
					if (!RequireString(Body, TEXT("key"), Key, Responder,
							TEXT("an entry key from list_translations")))
					{
						return;
					}
					if (!Body->TryGetStringField(TEXT("translation"), Translation))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'translation' is required — the translated text for this entry"));
						return;
					}
					const TSharedPtr<FArchiveEntry> Entry =
						Archive->FindEntryByKey(Namespace, Key, nullptr);
					if (!Entry.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("entry_not_found"),
							FString::Printf(
								TEXT("no entry '%s' in namespace '%s' — list_translations shows ")
								TEXT("what the gather found (an empty namespace is the usual one)"),
								*Key, *Namespace));
						return;
					}
					if (!Archive->SetTranslation(Namespace, Key, Entry->Source,
							FLocItem(Translation), nullptr))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError,
							TEXT("set_translation_failed"),
							TEXT("the archive refused the translation"));
						return;
					}
					if (!FJsonInternationalizationArchiveSerializer::SerializeArchiveToFile(
							Archive, ArchivePath))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError,
							TEXT("archive_not_written"),
							FString::Printf(TEXT("could not write %s"), *ArchivePath));
						return;
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("target"), Target->Settings.Name);
					Data->SetStringField(TEXT("culture"), Culture);
					Data->SetStringField(TEXT("namespace"), Namespace);
					Data->SetStringField(TEXT("key"), Key);
					Data->SetStringField(TEXT("source"), Entry->Source.Text);
					Data->SetStringField(TEXT("translation"), Translation);
					Data->SetStringField(TEXT("archive"), ArchivePath);
					Data->SetStringField(TEXT("message"),
						TEXT("archive updated — localization_ops compile turns it into the .locres ")
						TEXT("the game loads"));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_targets, list_cultures, create_target, ")
						TEXT("target_info, add_culture, remove_culture, set_native_culture, ")
						TEXT("configure_gather, generate_configs, list_translations or ")
						TEXT("set_translation (gather, compile, export_po and import_po are ")
						TEXT("localization_ops operations, run as commandlets)"),
						*Operation));
			});
	}
}
