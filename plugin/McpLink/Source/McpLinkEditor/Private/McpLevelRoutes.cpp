#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterLevelRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/levels/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
				if (EditorWorld == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_editor_world"),
						TEXT("editor world unavailable"));
					return;
				}

				if (Operation == TEXT("get_current"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("map"), EditorWorld->GetMapName());
					Data->SetStringField(TEXT("package"), EditorWorld->GetOutermost()->GetName());
					Data->SetBoolField(TEXT("dirty"), EditorWorld->GetOutermost()->IsDirty());
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("load"))
				{
					FString Path;
					if (!Body->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'path' is required (e.g. /Game/Maps/TestMap)"));
						return;
					}
					const bool bLoaded = FEditorFileUtils::LoadMap(Path, false, true);
					if (!bLoaded)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("load_failed"),
							FString::Printf(TEXT("could not load map '%s'"), *Path));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					UWorld* NewWorld = GEditor->GetEditorWorldContext().World();
					Data->SetStringField(TEXT("map"), NewWorld ? NewWorld->GetMapName() : TEXT(""));
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("save_current"))
				{
					const bool bSaved = FEditorFileUtils::SaveCurrentLevel();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("saved"), bSaved);
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("save_as"))
				{
					FString Path;
					if (!Body->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'path' is required (e.g. /Game/Maps/TestMap)"));
						return;
					}
					FString Filename;
					if (!FPackageName::TryConvertLongPackageNameToFilename(
						Path, Filename, FPackageName::GetMapPackageExtension()))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_path"),
							FString::Printf(TEXT("'%s' is not a valid package path — use /Game/..."), *Path));
						return;
					}
					FString SavedAs;
					const bool bSaved = FEditorFileUtils::SaveLevel(
						EditorWorld->PersistentLevel, Filename, &SavedAs);
					if (!bSaved)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"),
							FString::Printf(TEXT("could not save the level to '%s'"), *Filename));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("saved"), true);
					Data->SetStringField(TEXT("file"), SavedAs);
					Data->SetStringField(TEXT("package"), Path);
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("save_all"))
				{
					const bool bSaved = FEditorFileUtils::SaveDirtyPackages(
						/*bPromptUserToSave*/ false, /*bSaveMapPackages*/ true, /*bSaveContentPackages*/ true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("saved"), bSaved);
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("new_level"))
				{
					UWorld* NewWorld = GEditor->NewMap();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("map"), NewWorld ? NewWorld->GetMapName() : TEXT(""));
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("list"))
				{
					FAssetRegistryModule& AssetRegistry =
						FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
					FARFilter Filter;
					Filter.ClassPaths.Add(UWorld::StaticClass()->GetClassPathName());
					Filter.PackagePaths.Add(TEXT("/Game"));
					Filter.bRecursivePaths = true;
					TArray<FAssetData> Assets;
					AssetRegistry.Get().GetAssets(Filter, Assets);
					TArray<TSharedPtr<FJsonValue>> Maps;
					for (const FAssetData& Asset : Assets)
					{
						Maps.Add(MakeShared<FJsonValueString>(Asset.PackageName.ToString()));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("maps"), Maps);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use get_current, load, save_current, save_as, save_all, new_level, or list"),
						*Operation));
			});
	}
}
