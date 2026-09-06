// Editor and pipeline systems that had no route: collections, Editor
// Utility Blueprints and Widgets, the Asset Manager's primary assets and
// labels, Live Coding / hot reload, and Derived Data Cache statistics.

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CollectionManagerModule.h"
#include "CollectionManagerTypes.h"
#include "DerivedDataCacheInterface.h"
#include "DerivedDataCacheUsageStats.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorUtilityBlueprint.h"
#include "EditorUtilitySubsystem.h"
#include "EditorUtilityWidget.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "Engine/AssetManager.h"
#include "Engine/PrimaryAssetLabel.h"
#include "ICollectionContainer.h"
#include "ICollectionManager.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/CompilationResult.h"
#include "Misc/CoreMisc.h"
#include "Misc/HotReloadInterface.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

namespace McpLink
{
	namespace Pipeline
	{
		// ---------------------------------------------------------- collections

		bool ParseShareType(const FString& Spec, ECollectionShareType::Type& OutType)
		{
			const FString Lower = Spec.ToLower();
			if (Lower.IsEmpty() || Lower == TEXT("local")) { OutType = ECollectionShareType::CST_Local; return true; }
			if (Lower == TEXT("private")) { OutType = ECollectionShareType::CST_Private; return true; }
			if (Lower == TEXT("shared")) { OutType = ECollectionShareType::CST_Shared; return true; }
			return false;
		}

		FString ShareTypeName(ECollectionShareType::Type Type)
		{
			switch (Type)
			{
			case ECollectionShareType::CST_Local: return TEXT("local");
			case ECollectionShareType::CST_Private: return TEXT("private");
			case ECollectionShareType::CST_Shared: return TEXT("shared");
			case ECollectionShareType::CST_System: return TEXT("system");
			default: return TEXT("all");
			}
		}

		/// The project's collection container, where the content browser's
		/// collections live.
		ICollectionContainer& ProjectCollections()
		{
			return *FCollectionManagerModule::GetModule().Get().GetProjectCollectionContainer();
		}

		bool ReadCollection(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder,
			FName& OutName, ECollectionShareType::Type& OutType)
		{
			FString Name;
			if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the collection name")))
			{
				return false;
			}
			FString Share;
			Body->TryGetStringField(TEXT("share_type"), Share);
			if (!ParseShareType(Share, OutType))
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_share_type"),
					FString::Printf(TEXT("unknown share_type '%s' — local (default), private or shared"), *Share));
				return false;
			}
			OutName = FName(*Name);
			return true;
		}

		TArray<FSoftObjectPath> ReadAssetPaths(const TSharedRef<FJsonObject>& Body)
		{
			TArray<FSoftObjectPath> Paths;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Body->TryGetArrayField(TEXT("assets"), Values))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Values)
				{
					FString Path;
					if (Value.IsValid() && Value->TryGetString(Path) && !Path.IsEmpty())
					{
						// "/Game/Foo" names the package; the asset repeats the name.
						const FString Full = Path.Contains(TEXT("."))
							? Path
							: FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path));
						Paths.Add(FSoftObjectPath(Full));
					}
				}
			}
			return Paths;
		}

		TSharedRef<FJsonObject> CollectionsJson()
		{
			ICollectionContainer& Container = ProjectCollections();
			TArray<FCollectionNameType> Collections;
			Container.GetCollections(Collections);
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FCollectionNameType& Collection : Collections)
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Collection.Name.ToString());
				Item->SetStringField(TEXT("share_type"), ShareTypeName(Collection.Type));
				TArray<FSoftObjectPath> Assets;
				Container.GetAssetsInCollection(Collection.Name, Collection.Type, Assets);
				Item->SetNumberField(TEXT("asset_count"), Assets.Num());
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("collections"), Items);
			Data->SetNumberField(TEXT("count"), Items.Num());
			return Data;
		}

		// ---------------------------------------------------- editor utilities

		TSharedRef<FJsonObject> UtilityAssetJson(const FAssetData& Asset)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("path"), Asset.GetObjectPathString());
			Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
			Item->SetStringField(TEXT("kind"),
				Asset.GetClass() == UEditorUtilityWidgetBlueprint::StaticClass() ? TEXT("widget") : TEXT("blueprint"));
			return Item;
		}

		// ------------------------------------------------------- asset manager

		TSharedRef<FJsonObject> PrimaryAssetTypeJson(const FPrimaryAssetTypeInfo& Info)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("type"), Info.PrimaryAssetType.ToString());
			Item->SetStringField(TEXT("base_class"),
				Info.AssetBaseClassLoaded != nullptr ? Info.AssetBaseClassLoaded->GetPathName() : FString());
			Item->SetBoolField(TEXT("blueprint_classes"), Info.bHasBlueprintClasses);
			Item->SetBoolField(TEXT("editor_only"), Info.bIsEditorOnly);
			Item->SetBoolField(TEXT("dynamic"), Info.bIsDynamicAsset);
			Item->SetNumberField(TEXT("asset_count"), Info.NumberOfAssets);
			TArray<TSharedPtr<FJsonValue>> Paths;
			for (const FString& Path : Info.AssetScanPaths)
			{
				Paths.Add(MakeShared<FJsonValueString>(Path));
			}
			Item->SetArrayField(TEXT("scan_paths"), Paths);
			return Item;
		}

		FString CookRuleName(EPrimaryAssetCookRule Rule)
		{
			switch (Rule)
			{
			case EPrimaryAssetCookRule::NeverCook: return TEXT("never_cook");
			case EPrimaryAssetCookRule::ProductionNeverCook: return TEXT("production_never_cook");
			case EPrimaryAssetCookRule::DevelopmentAlwaysProductionUnknownCook: return TEXT("development_always_cook");
			case EPrimaryAssetCookRule::DevelopmentAlwaysProductionNeverCook: return TEXT("development_always_production_never_cook");
			case EPrimaryAssetCookRule::AlwaysCook: return TEXT("always_cook");
			default: return TEXT("unknown");
			}
		}

		bool ParseCookRule(const FString& Spec, EPrimaryAssetCookRule& OutRule)
		{
			const FString Lower = Spec.ToLower();
			if (Lower.IsEmpty() || Lower == TEXT("unknown")) { OutRule = EPrimaryAssetCookRule::Unknown; return true; }
			if (Lower == TEXT("never_cook")) { OutRule = EPrimaryAssetCookRule::NeverCook; return true; }
			if (Lower == TEXT("production_never_cook")) { OutRule = EPrimaryAssetCookRule::ProductionNeverCook; return true; }
			if (Lower == TEXT("development_always_cook")) { OutRule = EPrimaryAssetCookRule::DevelopmentAlwaysProductionUnknownCook; return true; }
			if (Lower == TEXT("development_always_production_never_cook")) { OutRule = EPrimaryAssetCookRule::DevelopmentAlwaysProductionNeverCook; return true; }
			if (Lower == TEXT("always_cook")) { OutRule = EPrimaryAssetCookRule::AlwaysCook; return true; }
			return false;
		}

		TSharedRef<FJsonObject> RulesJson(const FPrimaryAssetRules& Rules)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("priority"), Rules.Priority);
			Item->SetNumberField(TEXT("chunk_id"), Rules.ChunkId);
			Item->SetBoolField(TEXT("apply_recursively"), Rules.bApplyRecursively);
			Item->SetStringField(TEXT("cook_rule"), CookRuleName(Rules.CookRule));
			return Item;
		}

		// -------------------------------------------------------- live coding

		FString CompilationResultName(ECompilationResult::Type Result)
		{
			return ECompilationResult::ToString(Result);
		}
	}

	using namespace Pipeline;

	void RegisterPipelineRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/workflow/collections"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list"))
				{
					Responder->Ok(CollectionsJson());
					return;
				}

				FName Name;
				ECollectionShareType::Type ShareType = ECollectionShareType::CST_Local;
				if (!ReadCollection(Body, Responder, Name, ShareType))
				{
					return;
				}
				ICollectionContainer& Container = ProjectCollections();
				FText Error;

				if (Operation == TEXT("create"))
				{
					FString Storage;
					Body->TryGetStringField(TEXT("storage"), Storage);
					const ECollectionStorageMode::Type Mode = Storage.Equals(TEXT("dynamic"), ESearchCase::IgnoreCase)
						? ECollectionStorageMode::Dynamic
						: ECollectionStorageMode::Static;
					if (Container.CollectionExists(Name, ShareType))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("a %s collection '%s' already exists"), *ShareTypeName(ShareType), *Name.ToString()));
						return;
					}
					if (!Container.CreateCollection(Name, ShareType, Mode, &Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("create_failed"), Error.ToString());
						return;
					}
					const TArray<FSoftObjectPath> Assets = ReadAssetPaths(Body);
					int32 Added = 0;
					if (!Assets.IsEmpty())
					{
						Container.AddToCollection(Name, ShareType, Assets, &Added, &Error);
					}
					const TSharedRef<FJsonObject> Data = CollectionsJson();
					Data->SetStringField(TEXT("created"), Name.ToString());
					Data->SetNumberField(TEXT("added"), Added);
					Responder->Ok(Data);
					return;
				}

				if (!Container.CollectionExists(Name, ShareType))
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("collection_not_found"),
						FString::Printf(TEXT("no %s collection '%s' — collection_ops list names them"),
							*ShareTypeName(ShareType), *Name.ToString()));
					return;
				}

				if (Operation == TEXT("destroy"))
				{
					if (!Container.DestroyCollection(Name, ShareType, &Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("destroy_failed"), Error.ToString());
						return;
					}
					const TSharedRef<FJsonObject> Data = CollectionsJson();
					Data->SetStringField(TEXT("destroyed"), Name.ToString());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add") || Operation == TEXT("remove"))
				{
					const TArray<FSoftObjectPath> Assets = ReadAssetPaths(Body);
					if (Assets.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'assets' is required — asset paths such as /Game/Meshes/SM_Rock"));
						return;
					}
					int32 Count = 0;
					const bool bOk = Operation == TEXT("add")
						? Container.AddToCollection(Name, ShareType, Assets, &Count, &Error)
						: Container.RemoveFromCollection(Name, ShareType, Assets, &Count, &Error);
					if (!bOk)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("update_failed"), Error.ToString());
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("collection"), Name.ToString());
					Data->SetNumberField(Operation == TEXT("add") ? TEXT("added") : TEXT("removed"), Count);
					TArray<FSoftObjectPath> Now;
					Container.GetAssetsInCollection(Name, ShareType, Now);
					Data->SetNumberField(TEXT("asset_count"), Now.Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("assets"))
				{
					TArray<FSoftObjectPath> Assets;
					Container.GetAssetsInCollection(Name, ShareType, Assets,
						BoolOr(Body, TEXT("recursive"), false)
							? static_cast<ECollectionRecursionFlags::Flags>(ECollectionRecursionFlags::Self | ECollectionRecursionFlags::Children)
							: ECollectionRecursionFlags::Self);
					TArray<TSharedPtr<FJsonValue>> Items;
					for (const FSoftObjectPath& Path : Assets)
					{
						Items.Add(MakeShared<FJsonValueString>(Path.ToString()));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("collection"), Name.ToString());
					Data->SetArrayField(TEXT("assets"), Items);
					Data->SetNumberField(TEXT("count"), Items.Num());
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — use list, create, destroy, add, remove or assets"), *Operation));
			});

		Core.RegisterRoute(TEXT("/api/workflow/editor_utilities"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);
				UEditorUtilitySubsystem* Subsystem =
					GEditor != nullptr ? GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>() : nullptr;
				if (Subsystem == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_subsystem"),
						TEXT("the Editor Utility subsystem is unavailable"));
					return;
				}

				if (Operation == TEXT("list"))
				{
					FString Prefix = TEXT("/Game");
					Body->TryGetStringField(TEXT("path_prefix"), Prefix);
					IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
					FARFilter Filter;
					Filter.ClassPaths.Add(UEditorUtilityBlueprint::StaticClass()->GetClassPathName());
					Filter.ClassPaths.Add(UEditorUtilityWidgetBlueprint::StaticClass()->GetClassPathName());
					Filter.bRecursiveClasses = true;
					Filter.PackagePaths.Add(FName(*Prefix));
					Filter.bRecursivePaths = true;
					TArray<FAssetData> Assets;
					Registry.GetAssets(Filter, Assets);
					TArray<TSharedPtr<FJsonValue>> Items;
					for (const FAssetData& Asset : Assets)
					{
						Items.Add(MakeShared<FJsonValueObject>(UtilityAssetJson(Asset)));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("utilities"), Items);
					Data->SetNumberField(TEXT("count"), Items.Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("run"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("asset"), Path, Responder,
							TEXT("an Editor Utility Blueprint (or any asset with a Run method) path")))
					{
						return;
					}
					UObject* Asset = ResolveAsset(Path);
					if (Asset == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
							FString::Printf(TEXT("no asset at '%s'"), *Path));
						return;
					}
					// TryRun executes the utility's Run entry point (an Editor
					// Utility Blueprint's, or a Blutility's) — a widget asset is
					// opened instead, as the content browser does.
					const bool bRan = Subsystem->TryRun(Asset);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("asset"), Asset->GetPathName());
					Data->SetBoolField(TEXT("ran"), bRan);
					if (!bRan)
					{
						Data->SetStringField(TEXT("note"),
							TEXT("TryRun returned false — the asset has no Run entry point (an Editor Utility Blueprint ")
							TEXT("deriving from EditorUtilityObject or AssetActionUtility exposes one), or it is a widget: use open_widget"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("open_widget") || Operation == TEXT("close_widget"))
				{
					if (Operation == TEXT("close_widget"))
					{
						FString TabId;
						if (!RequireString(Body, TEXT("tab_id"), TabId, Responder, TEXT("the tab id open_widget reported")))
						{
							return;
						}
						const bool bClosed = Subsystem->CloseTabByID(FName(*TabId));
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("tab_id"), TabId);
						Data->SetBoolField(TEXT("closed"), bClosed);
						Responder->Ok(Data);
						return;
					}
					FString Path;
					if (!RequireString(Body, TEXT("asset"), Path, Responder, TEXT("an Editor Utility Widget Blueprint path")))
					{
						return;
					}
					UEditorUtilityWidgetBlueprint* Widget = Cast<UEditorUtilityWidgetBlueprint>(ResolveAsset(Path));
					if (Widget == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("widget_not_found"),
							FString::Printf(TEXT("no Editor Utility Widget Blueprint at '%s'"), *Path));
						return;
					}
					FString TabIdSpec;
					Body->TryGetStringField(TEXT("tab_id"), TabIdSpec);
					FName TabId = TabIdSpec.IsEmpty() ? NAME_None : FName(*TabIdSpec);
					UEditorUtilityWidget* Instance = TabIdSpec.IsEmpty()
						? Subsystem->SpawnAndRegisterTabAndGetID(Widget, TabId)
						: Subsystem->SpawnAndRegisterTabWithId(Widget, TabId);
					if (Instance == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("spawn_failed"),
							TEXT("the widget did not spawn — a headless editor has no level editor tab manager to dock it in"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("asset"), Widget->GetPathName());
					Data->SetStringField(TEXT("tab_id"), TabId.ToString());
					Data->SetStringField(TEXT("widget"), Instance->GetPathName());
					Data->SetStringField(TEXT("note"),
						TEXT("the live widget's properties and functions are reachable with get_property / call_function on `widget`"));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — use list, run, open_widget or close_widget"), *Operation));
			});

		Core.RegisterRoute(TEXT("/api/workflow/asset_manager"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);
				if (!UAssetManager::IsInitialized())
				{
					Responder->Error(EHttpServerResponseCodes::ServiceUnavail, TEXT("no_asset_manager"),
						TEXT("the Asset Manager is not initialised"));
					return;
				}
				UAssetManager& Manager = UAssetManager::Get();

				if (Operation == TEXT("types"))
				{
					TArray<FPrimaryAssetTypeInfo> Types;
					Manager.GetPrimaryAssetTypeInfoList(Types);
					TArray<TSharedPtr<FJsonValue>> Items;
					for (const FPrimaryAssetTypeInfo& Info : Types)
					{
						Items.Add(MakeShared<FJsonValueObject>(PrimaryAssetTypeJson(Info)));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("types"), Items);
					Data->SetNumberField(TEXT("count"), Items.Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list"))
				{
					FString Type;
					if (!RequireString(Body, TEXT("type"), Type, Responder, TEXT("a primary asset type from `types`, e.g. Map or PrimaryAssetLabel")))
					{
						return;
					}
					TArray<FPrimaryAssetId> Ids;
					Manager.GetPrimaryAssetIdList(FPrimaryAssetType(*Type), Ids);
					TArray<TSharedPtr<FJsonValue>> Items;
					for (const FPrimaryAssetId& Id : Ids)
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("id"), Id.ToString());
						Item->SetStringField(TEXT("path"), Manager.GetPrimaryAssetPath(Id).ToString());
						Items.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("type"), Type);
					Data->SetArrayField(TEXT("assets"), Items);
					Data->SetNumberField(TEXT("count"), Items.Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("info"))
				{
					FString IdSpec;
					if (!RequireString(Body, TEXT("id"), IdSpec, Responder, TEXT("a primary asset id, Type:Name, or an asset path")))
					{
						return;
					}
					FPrimaryAssetId Id = FPrimaryAssetId::FromString(IdSpec);
					if (!Id.IsValid() && IdSpec.StartsWith(TEXT("/")))
					{
						// "/Game/Maps/TestMap" names the package; the object repeats the name.
						const FString Full = IdSpec.Contains(TEXT("."))
							? IdSpec
							: FString::Printf(TEXT("%s.%s"), *IdSpec, *FPackageName::GetShortName(IdSpec));
						Id = Manager.GetPrimaryAssetIdForPath(FSoftObjectPath(Full));
					}
					if (!Id.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("id_not_found"),
							FString::Printf(TEXT("'%s' is not a primary asset id (Type:Name) or the path of a primary asset"), *IdSpec));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("id"), Id.ToString());
					Data->SetStringField(TEXT("path"), Manager.GetPrimaryAssetPath(Id).ToString());
					Data->SetObjectField(TEXT("rules"), RulesJson(Manager.GetPrimaryAssetRules(Id)));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_label"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Labels/PAL_Level1")))
					{
						return;
					}
					if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}
					EPrimaryAssetCookRule CookRule = EPrimaryAssetCookRule::Unknown;
					FString CookSpec;
					Body->TryGetStringField(TEXT("cook_rule"), CookSpec);
					if (!ParseCookRule(CookSpec, CookRule))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_cook_rule"),
							FString::Printf(TEXT("unknown cook_rule '%s' — never_cook, production_never_cook, development_always_cook, ")
											TEXT("development_always_production_never_cook or always_cook"), *CookSpec));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "CreatePrimaryAssetLabel", "McpLink Create Primary Asset Label"));
					UPackage* Package = CreatePackage(*Path);
					UPrimaryAssetLabel* Label = NewObject<UPrimaryAssetLabel>(
						Package, FName(*FPackageName::GetShortName(Path)), RF_Public | RF_Standalone | RF_Transactional);
					Label->bLabelAssetsInMyDirectory = BoolOr(Body, TEXT("label_directory"), false);
					Label->Rules.Priority = IntOr(Body, TEXT("priority"), Label->Rules.Priority);
					Label->Rules.ChunkId = IntOr(Body, TEXT("chunk_id"), Label->Rules.ChunkId);
					Label->Rules.bApplyRecursively = BoolOr(Body, TEXT("apply_recursively"), Label->Rules.bApplyRecursively);
					Label->Rules.CookRule = CookRule;
					for (const FSoftObjectPath& Asset : ReadAssetPaths(Body))
					{
						Label->ExplicitAssets.Add(TSoftObjectPtr<UObject>(Asset));
					}
					FAssetRegistryModule::AssetCreated(Label);
					Package->MarkPackageDirty();
					// The label only counts once the manager has seen it.
					Manager.RefreshPrimaryAssetDirectory(true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Label->GetPathName());
					Data->SetStringField(TEXT("id"), Label->GetPrimaryAssetId().ToString());
					Data->SetNumberField(TEXT("explicit_assets"), Label->ExplicitAssets.Num());
					Data->SetObjectField(TEXT("rules"), RulesJson(Label->Rules));
					Data->SetStringField(TEXT("message"), TEXT("created in memory — asset_ops save writes it to disk"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("refresh"))
				{
					Manager.RefreshPrimaryAssetDirectory(true);
					TArray<FPrimaryAssetTypeInfo> Types;
					Manager.GetPrimaryAssetTypeInfoList(Types);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("refreshed"), true);
					Data->SetNumberField(TEXT("types"), Types.Num());
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — use types, list, info, create_label or refresh"), *Operation));
			});

		Core.RegisterRoute(TEXT("/api/workflow/live_coding"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);
				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
#if WITH_LIVE_CODING
				ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>("LiveCoding");
#else
				void* LiveCoding = nullptr;
#endif
				IHotReloadInterface* HotReload = FModuleManager::GetModulePtr<IHotReloadInterface>("HotReload");

				if (Operation == TEXT("status"))
				{
#if WITH_LIVE_CODING
					Data->SetBoolField(TEXT("live_coding_available"), LiveCoding != nullptr);
					if (LiveCoding != nullptr)
					{
						Data->SetBoolField(TEXT("enabled"), LiveCoding->IsEnabledForSession());
						Data->SetBoolField(TEXT("enabled_by_default"), LiveCoding->IsEnabledByDefault());
						Data->SetBoolField(TEXT("can_enable"), LiveCoding->CanEnableForSession());
						Data->SetBoolField(TEXT("started"), LiveCoding->HasStarted());
						Data->SetBoolField(TEXT("compiling"), LiveCoding->IsCompiling());
						const FString EnableError = LiveCoding->GetEnableErrorText().ToString();
						if (!EnableError.IsEmpty())
						{
							Data->SetStringField(TEXT("enable_error"), EnableError);
						}
					}
#else
					Data->SetBoolField(TEXT("live_coding_available"), false);
#endif
					Data->SetBoolField(TEXT("hot_reload_available"), HotReload != nullptr);
					if (HotReload != nullptr)
					{
						Data->SetBoolField(TEXT("hot_reload_compiling"), HotReload->IsCurrentlyCompiling());
						Data->SetBoolField(TEXT("game_modules_loaded"), HotReload->IsAnyGameModuleLoaded());
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("enable"))
				{
#if WITH_LIVE_CODING
					if (LiveCoding == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotSupported, TEXT("live_coding_unavailable"),
							TEXT("the Live Coding module is not loaded on this platform"));
						return;
					}
					const bool bEnable = BoolOr(Body, TEXT("enabled"), true);
					LiveCoding->EnableForSession(bEnable);
					Data->SetBoolField(TEXT("enabled"), LiveCoding->IsEnabledForSession());
					Data->SetBoolField(TEXT("started"), LiveCoding->HasStarted());
					const FString EnableError = LiveCoding->GetEnableErrorText().ToString();
					if (bEnable && !LiveCoding->IsEnabledForSession() && !EnableError.IsEmpty())
					{
						Data->SetStringField(TEXT("enable_error"), EnableError);
					}
					Responder->Ok(Data);
#else
					Responder->Error(EHttpServerResponseCodes::NotSupported, TEXT("live_coding_unavailable"),
						TEXT("Live Coding is Windows-only"));
#endif
					return;
				}

				if (Operation == TEXT("compile"))
				{
#if WITH_LIVE_CODING
					if (LiveCoding == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotSupported, TEXT("live_coding_unavailable"),
							TEXT("the Live Coding module is not loaded on this platform"));
						return;
					}
					if (!LiveCoding->IsEnabledForSession())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("live_coding_disabled"),
							TEXT("Live Coding is not enabled for this session — live_coding_ops enable first"));
						return;
					}
					// Waiting keeps the answer meaningful; a patch takes seconds
					// to a minute, which the caller's timeout allows for.
					const bool bWait = BoolOr(Body, TEXT("wait"), true);
					ELiveCodingCompileResult Result = ELiveCodingCompileResult::NotStarted;
					const bool bStarted = LiveCoding->Compile(
						bWait ? ELiveCodingCompileFlags::WaitForCompletion : ELiveCodingCompileFlags::None, &Result);
					static const TCHAR* const Names[] = {
						TEXT("success"), TEXT("no_changes"), TEXT("in_progress"), TEXT("compile_still_active"),
						TEXT("not_started"), TEXT("failure"), TEXT("cancelled")};
					const int32 Index = static_cast<int32>(Result);
					Data->SetBoolField(TEXT("started"), bStarted);
					Data->SetStringField(TEXT("result"), Index >= 0 && Index < UE_ARRAY_COUNT(Names) ? Names[Index] : TEXT("unknown"));
					Data->SetStringField(TEXT("note"),
						TEXT("Live Coding patches the running editor's binaries; the compiler output is in the Live Coding console and the output log"));
					Responder->Ok(Data);
#else
					Responder->Error(EHttpServerResponseCodes::NotSupported, TEXT("live_coding_unavailable"),
						TEXT("Live Coding is Windows-only"));
#endif
					return;
				}

				if (Operation == TEXT("hot_reload"))
				{
					if (HotReload == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotSupported, TEXT("hot_reload_unavailable"),
							TEXT("the Hot Reload module is not loaded"));
						return;
					}
					if (HotReload->IsCurrentlyCompiling())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_compiling"),
							TEXT("a hot reload compile is already running"));
						return;
					}
					const bool bWait = BoolOr(Body, TEXT("wait"), true);
					const ECompilationResult::Type Result = HotReload->DoHotReloadFromEditor(
						bWait ? EHotReloadFlags::WaitForCompletion : EHotReloadFlags::None);
					Data->SetStringField(TEXT("result"), CompilationResultName(Result));
					Data->SetBoolField(TEXT("succeeded"),
						Result == ECompilationResult::Succeeded || Result == ECompilationResult::UpToDate);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — use status, enable, compile or hot_reload"), *Operation));
			});

		Core.RegisterRoute(TEXT("/api/workflow/ddc"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);
				if (Operation != TEXT("stats"))
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
						FString::Printf(TEXT("unknown operation '%s' — use stats"), *Operation));
					return;
				}
				FDerivedDataCacheInterface& Cache = GetDerivedDataCacheRef();
				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("graph"), Cache.GetGraphName());
				Data->SetBoolField(TEXT("default_graph"), Cache.IsDefaultGraph());
#if ENABLE_COOK_STATS
				// Per node of the cache graph (local, shared, Zen, ...): how
				// many gets and puts, how many hit, how many bytes moved.
				TMap<FString, FDerivedDataCacheUsageStats> Usage;
				Cache.GatherUsageStats(Usage);
				TArray<FString> Keys;
				Usage.GetKeys(Keys);
				Keys.Sort();
				TArray<TSharedPtr<FJsonValue>> Nodes;
				const auto CallJson = [](const FCookStats::CallStats& Stats)
				{
					using EHitOrMiss = FCookStats::CallStats::EHitOrMiss;
					using EStatType = FCookStats::CallStats::EStatType;
					const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetNumberField(TEXT("hits"), static_cast<double>(Stats.GetAccumulatedValueAnyThread(EHitOrMiss::Hit, EStatType::Counter)));
					Item->SetNumberField(TEXT("misses"), static_cast<double>(Stats.GetAccumulatedValueAnyThread(EHitOrMiss::Miss, EStatType::Counter)));
					Item->SetNumberField(TEXT("hit_bytes"), static_cast<double>(Stats.GetAccumulatedValueAnyThread(EHitOrMiss::Hit, EStatType::Bytes)));
					Item->SetNumberField(TEXT("miss_bytes"), static_cast<double>(Stats.GetAccumulatedValueAnyThread(EHitOrMiss::Miss, EStatType::Bytes)));
					Item->SetNumberField(TEXT("hit_seconds"),
						FPlatformTime::ToSeconds64(Stats.GetAccumulatedValueAnyThread(EHitOrMiss::Hit, EStatType::Cycles)));
					Item->SetNumberField(TEXT("miss_seconds"),
						FPlatformTime::ToSeconds64(Stats.GetAccumulatedValueAnyThread(EHitOrMiss::Miss, EStatType::Cycles)));
					return Item;
				};
				for (const FString& Key : Keys)
				{
					const FDerivedDataCacheUsageStats& Stats = Usage[Key];
					const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("node"), Key);
					Item->SetObjectField(TEXT("get"), CallJson(Stats.GetStats));
					Item->SetObjectField(TEXT("put"), CallJson(Stats.PutStats));
					Item->SetObjectField(TEXT("exists"), CallJson(Stats.ExistsStats));
					Item->SetObjectField(TEXT("prefetch"), CallJson(Stats.PrefetchStats));
					Nodes.Add(MakeShared<FJsonValueObject>(Item));
				}
				Data->SetArrayField(TEXT("nodes"), Nodes);
#else
				Data->SetStringField(TEXT("note"), TEXT("this editor was built without cook stats, so per-node usage is unavailable"));
#endif
				Responder->Ok(Data);
			});
	}
}
