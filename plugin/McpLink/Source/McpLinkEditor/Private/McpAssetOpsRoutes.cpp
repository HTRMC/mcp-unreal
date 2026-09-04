// Asset lifecycle: create, import, reimport, rename/move, duplicate, delete,
// save, folders, redirector fixup, export and metadata tags.
//
// Everything here goes through IAssetTools / AssetViewUtils rather than raw
// package writes, so the asset registry, the content browser and soft-reference
// fixup all stay consistent — the same code paths the editor UI uses, with
// every confirmation dialog suppressed so the routes work headless.

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "AssetViewUtils.h"
#include "AutomatedAssetImportData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorReimportHandler.h"
#include "Factories/Factory.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "JsonObjectConverter.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/MetaData.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace
	{
		IAssetTools& GetAssetTools()
		{
			return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		}

		IAssetRegistry& GetRegistry()
		{
			return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		}

		/// "/Game/Foo/Bar" -> "/Game/Foo/Bar.Bar"; a path that already names an
		/// object is returned unchanged.
		FString ToObjectPath(const FString& Path)
		{
			return Path.Contains(TEXT("."))
				? Path
				: FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path));
		}

		UObject* LoadAsset(const FString& Path)
		{
			return ResolveObject(ToObjectPath(Path));
		}

		/// Body field that is either a single "path" string or a "paths" array.
		TArray<FString> GetPathList(const TSharedRef<FJsonObject>& Body)
		{
			TArray<FString> Paths;
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (Body->TryGetArrayField(TEXT("paths"), Array) && Array != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Array)
				{
					FString Path;
					if (Value.IsValid() && Value->TryGetString(Path) && !Path.IsEmpty())
					{
						Paths.Add(Path);
					}
				}
			}
			FString Single;
			if (Body->TryGetStringField(TEXT("path"), Single) && !Single.IsEmpty())
			{
				Paths.AddUnique(Single);
			}
			return Paths;
		}

		/// A newly constructed factory that creates `AssetClass`, or nullptr when
		/// the class has no "create new" factory (most imported types).
		UFactory* MakeFactoryFor(UClass* AssetClass)
		{
			UFactory* Fallback = nullptr;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Class = *It;
				if (!Class->IsChildOf(UFactory::StaticClass())
					|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
				{
					continue;
				}
				const UFactory* Cdo = Class->GetDefaultObject<UFactory>();
				if (!Cdo->CanCreateNew())
				{
					continue;
				}
				const UClass* Supported = Cdo->GetSupportedClass();
				if (Supported == AssetClass)
				{
					return NewObject<UFactory>(GetTransientPackage(), Class);
				}
				// A factory for a base class still works when the caller also
				// passes the concrete class through factory_properties.
				if (Fallback == nullptr && Supported != nullptr && AssetClass->IsChildOf(Supported))
				{
					Fallback = NewObject<UFactory>(GetTransientPackage(), Class);
				}
			}
			return Fallback;
		}

		/// Apply a JSON object to a factory's UPROPERTYs — how a caller sets
		/// UDataAssetFactory::DataAssetClass, UBlueprintFactory::ParentClass, ...
		bool ApplyFactoryProperties(
			UFactory* Factory, const TSharedRef<FJsonObject>& Body, FString& OutError)
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Body->TryGetObjectField(TEXT("factory_properties"), Properties)
				|| Properties == nullptr)
			{
				return true;
			}
			if (!FJsonObjectConverter::JsonObjectToUStruct(
				Properties->ToSharedRef(), Factory->GetClass(), Factory, 0, 0))
			{
				OutError = FString::Printf(
					TEXT("could not apply factory_properties to %s — use get_class_info on it to see its properties"),
					*Factory->GetClass()->GetName());
				return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> AssetSummary(const UObject* Asset)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("path"), Asset->GetPathName());
			Object->SetStringField(TEXT("name"), Asset->GetName());
			Object->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
			return Object;
		}

		/// A content-browser folder path, normalised and checked. Rejects paths
		/// outside a mounted root so a typo cannot escape the content tree.
		bool RequireFolder(
			const TSharedRef<FJsonObject>& Body,
			const TCHAR* Field,
			const TSharedRef<FMcpResponder>& Responder,
			FString& OutPath)
		{
			if (!RequireString(Body, Field, OutPath, Responder,
				TEXT("a content folder, e.g. /Game/Meshes")))
			{
				return false;
			}
			while (OutPath.Len() > 1 && OutPath.EndsWith(TEXT("/")))
			{
				OutPath.LeftChopInline(1);
			}
			if (!FPackageName::IsValidLongPackageName(OutPath / TEXT("Probe")))
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_path"),
					FString::Printf(
						TEXT("'%s' is not inside a mounted content root — use /Game/... or a plugin's mount point"),
						*OutPath));
				return false;
			}
			return true;
		}
	}

	void RegisterAssetOpsRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/assets/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				// ---------------------------------------------------------------
				if (Operation == TEXT("create"))
				{
					FString Path, ClassSpec;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/Data/DA_Weapon"))
						|| !RequireString(Body, TEXT("class"), ClassSpec, Responder,
							TEXT("the asset class, e.g. CurveFloat, DataAsset, UserDefinedEnum")))
					{
						return;
					}
					UClass* AssetClass = ResolveClass(ClassSpec);
					if (AssetClass == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("unknown_class"),
							FString::Printf(TEXT("no class '%s'"), *ClassSpec));
						return;
					}
					UFactory* Factory = MakeFactoryFor(AssetClass);
					if (Factory == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("no_factory"),
							FString::Printf(
								TEXT("no 'create new' factory makes a %s — assets of this type are imported ")
								TEXT("(use operation 'import') rather than created empty"),
								*AssetClass->GetName()));
						return;
					}
					FString Error;
					if (!ApplyFactoryProperties(Factory, Body, Error))
					{
						Responder->Error(
							EHttpServerResponseCodes::BadRequest, TEXT("invalid_factory_properties"), Error);
						return;
					}
					UObject* Asset = CreateAsset(Path, AssetClass, Factory, Error);
					if (Asset == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = AssetSummary(Asset);
					Data->SetStringField(TEXT("factory"), Factory->GetClass()->GetName());
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("import"))
				{
					const TArray<TSharedPtr<FJsonValue>>* FileValues = nullptr;
					TArray<FString> Files;
					if (Body->TryGetArrayField(TEXT("files"), FileValues) && FileValues != nullptr)
					{
						for (const TSharedPtr<FJsonValue>& Value : *FileValues)
						{
							FString File;
							if (Value.IsValid() && Value->TryGetString(File) && !File.IsEmpty())
							{
								Files.Add(File);
							}
						}
					}
					FString SingleFile;
					if (Body->TryGetStringField(TEXT("file"), SingleFile) && !SingleFile.IsEmpty())
					{
						Files.AddUnique(SingleFile);
					}
					if (Files.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'files' (or 'file') is required — absolute paths to the source files on disk"));
						return;
					}
					FString Destination;
					if (!RequireFolder(Body, TEXT("destination"), Responder, Destination))
					{
						return;
					}

					TArray<FString> Missing;
					for (const FString& File : Files)
					{
						if (!IFileManager::Get().FileExists(*File))
						{
							Missing.Add(File);
						}
					}
					if (!Missing.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("file_not_found"),
							FString::Printf(TEXT("no such file: %s"), *FString::Join(Missing, TEXT(", "))));
						return;
					}

					UFactory* Factory = nullptr;
					FString FactorySpec;
					if (Body->TryGetStringField(TEXT("factory"), FactorySpec) && !FactorySpec.IsEmpty())
					{
						UClass* FactoryClass = ResolveClass(FactorySpec);
						if (FactoryClass == nullptr || !FactoryClass->IsChildOf(UFactory::StaticClass()))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_factory"),
								FString::Printf(
									TEXT("'%s' is not a UFactory class — omit 'factory' to let the importer pick one"),
									*FactorySpec));
							return;
						}
						Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
						FString Error;
						if (!ApplyFactoryProperties(Factory, Body, Error))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest,
								TEXT("invalid_factory_properties"), Error);
							return;
						}
					}

					// The automated path is the one that never opens an import
					// dialog, which is what makes this usable headless.
					UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
					ImportData->Filenames = Files;
					ImportData->DestinationPath = Destination;
					ImportData->bReplaceExisting = true;
					Body->TryGetBoolField(TEXT("replace_existing"), ImportData->bReplaceExisting);
					ImportData->bSkipReadOnly = false;
					ImportData->Factory = Factory;
					ImportData->GroupName = TEXT("McpLink");

					const TArray<UObject*> Imported = GetAssetTools().ImportAssetsAutomated(ImportData);
					if (Imported.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("import_failed"),
							TEXT("nothing was imported — check get_log for the importer's errors; a source ")
							TEXT("format with no factory in this build (or an unsupported variant) fails this way"));
						return;
					}

					TArray<TSharedPtr<FJsonValue>> Assets;
					for (UObject* Asset : Imported)
					{
						if (Asset != nullptr)
						{
							Assets.Add(MakeShared<FJsonValueObject>(AssetSummary(Asset)));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("imported"), Assets.Num());
					Data->SetArrayField(TEXT("assets"), Assets);
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("reimport"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder))
					{
						return;
					}
					UObject* Asset = LoadAsset(Path);
					if (Asset == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
							FString::Printf(TEXT("no asset at '%s'"), *Path));
						return;
					}
					FString File;
					Body->TryGetStringField(TEXT("file"), File);
					const bool bReimported = FReimportManager::Instance()->Reimport(
						Asset,
						/*bAskForNewFileIfMissing*/ false,
						/*bShowNotification*/ false,
						File,
						/*SpecifiedReimportHandler*/ nullptr,
						/*SourceFileIndex*/ INDEX_NONE,
						/*bForceNewFile*/ !File.IsEmpty(),
						/*bAutomated*/ true);
					if (!bReimported)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("reimport_failed"),
							FString::Printf(
								TEXT("could not reimport '%s' — the recorded source file may be gone; pass 'file' ")
								TEXT("to point it at a new one"),
								*Path));
						return;
					}
					Responder->Ok(AssetSummary(Asset));
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("rename") || Operation == TEXT("move"))
				{
					const TArray<FString> Paths = GetPathList(Body);
					if (Paths.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'path' (or 'paths') is required"));
						return;
					}
					FString NewName, NewFolder;
					Body->TryGetStringField(TEXT("new_name"), NewName);
					Body->TryGetStringField(TEXT("destination"), NewFolder);
					if (NewName.IsEmpty() && NewFolder.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("pass 'new_name' to rename, 'destination' to move, or both"));
						return;
					}
					if (!NewName.IsEmpty() && Paths.Num() > 1)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_request"),
							TEXT("'new_name' renames one asset — move several at once with 'destination' only"));
						return;
					}
					while (NewFolder.Len() > 1 && NewFolder.EndsWith(TEXT("/")))
					{
						NewFolder.LeftChopInline(1);
					}

					TArray<FAssetRenameData> Renames;
					for (const FString& Path : Paths)
					{
						UObject* Asset = LoadAsset(Path);
						if (Asset == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
								FString::Printf(TEXT("no asset at '%s'"), *Path));
							return;
						}
						const FString Folder = NewFolder.IsEmpty()
							? FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetName())
							: NewFolder;
						Renames.Emplace(Asset, Folder, NewName.IsEmpty() ? Asset->GetName() : NewName);
					}

					if (!GetAssetTools().RenameAssets(Renames))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("rename_failed"),
							TEXT("the rename was rejected — the destination name may be taken, or an asset ")
							TEXT("is checked out by someone else"));
						return;
					}

					TArray<TSharedPtr<FJsonValue>> Results;
					for (const FAssetRenameData& Rename : Renames)
					{
						if (UObject* Asset = Rename.Asset.Get())
						{
							Results.Add(MakeShared<FJsonValueObject>(AssetSummary(Asset)));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("renamed"), Results.Num());
					Data->SetArrayField(TEXT("assets"), Results);
					// RenameAssets leaves a redirector behind at each old path.
					Data->SetBoolField(TEXT("redirectors_left"), true);
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("duplicate"))
				{
					FString Path, Destination;
					if (!RequireString(Body, TEXT("path"), Path, Responder)
						|| !RequireString(Body, TEXT("destination"), Destination, Responder,
							TEXT("the full path of the copy, e.g. /Game/Meshes/SM_Rock_B")))
					{
						return;
					}
					UObject* Asset = LoadAsset(Path);
					if (Asset == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
							FString::Printf(TEXT("no asset at '%s'"), *Path));
						return;
					}
					if (FPackageName::DoesPackageExist(Destination)
						|| FindPackage(nullptr, *Destination) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("destination_taken"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Destination));
						return;
					}
					UObject* Copy = GetAssetTools().DuplicateAsset(
						FPackageName::GetShortName(Destination),
						FPackageName::GetLongPackagePath(Destination),
						Asset);
					if (Copy == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("duplicate_failed"),
							FString::Printf(TEXT("could not duplicate '%s' to '%s'"), *Path, *Destination));
						return;
					}
					Responder->Ok(AssetSummary(Copy));
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("delete"))
				{
					const TArray<FString> Paths = GetPathList(Body);
					if (Paths.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'path' (or 'paths') is required"));
						return;
					}
					bool bForce = false;
					Body->TryGetBoolField(TEXT("force"), bForce);

					TArray<FAssetData> ToDelete;
					TArray<UObject*> Objects;
					for (const FString& Path : Paths)
					{
						const FAssetData Asset =
							GetRegistry().GetAssetByObjectPath(FSoftObjectPath(ToObjectPath(Path)));
						if (!Asset.IsValid())
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
								FString::Printf(TEXT("no asset at '%s'"), *Path));
							return;
						}
						ToDelete.Add(Asset);
						if (bForce)
						{
							if (UObject* Object = Asset.GetAsset())
							{
								Objects.Add(Object);
							}
						}
					}

					const int32 Deleted = bForce
						? ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation*/ false)
						: ObjectTools::DeleteAssets(ToDelete, /*bShowConfirmation*/ false);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("deleted"), Deleted);
					Data->SetNumberField(TEXT("requested"), Paths.Num());
					if (Deleted < Paths.Num())
					{
						Data->SetStringField(TEXT("note"),
							TEXT("some assets were kept because something still references them — ")
							TEXT("get_asset_info lists referencers, or pass force:true to null out the references"));
					}
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("save"))
				{
					const TArray<FString> Paths = GetPathList(Body);
					if (Paths.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'path' (or 'paths') is required — use save_all for every dirty package"));
						return;
					}
					TArray<TSharedPtr<FJsonValue>> Saved;
					for (const FString& Path : Paths)
					{
						UObject* Asset = LoadAsset(Path);
						if (Asset == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
								FString::Printf(TEXT("no asset at '%s'"), *Path));
							return;
						}
						FString Filename, Error;
						if (!SaveAsset(Asset, Filename, Error))
						{
							Responder->Error(
								EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
							return;
						}
						Saved.Add(MakeShared<FJsonValueString>(Filename));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("saved"), Saved.Num());
					Data->SetArrayField(TEXT("files"), Saved);
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("save_all"))
				{
					TArray<UPackage*> Dirty;
					FEditorFileUtils::GetDirtyContentPackages(Dirty);
					bool bIncludeMaps = true;
					Body->TryGetBoolField(TEXT("include_maps"), bIncludeMaps);
					if (bIncludeMaps)
					{
						FEditorFileUtils::GetDirtyWorldPackages(Dirty);
					}
					TArray<TSharedPtr<FJsonValue>> Names;
					for (const UPackage* Package : Dirty)
					{
						Names.Add(MakeShared<FJsonValueString>(Package->GetName()));
					}
					const bool bSaved = Dirty.IsEmpty() || AssetViewUtils::SavePackages(Dirty);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("saved"), bSaved);
					Data->SetNumberField(TEXT("package_count"), Names.Num());
					Data->SetArrayField(TEXT("packages"), Names);
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("create_folder"))
				{
					FString Path;
					if (!RequireFolder(Body, TEXT("path"), Responder, Path))
					{
						return;
					}
					const FString Directory = FPackageName::LongPackageNameToFilename(Path / TEXT(""));
					if (!IFileManager::Get().DirectoryExists(*Directory)
						&& !IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("mkdir_failed"),
							FString::Printf(TEXT("could not create '%s' on disk"), *Directory));
						return;
					}
					GetRegistry().AddPath(Path);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Path);
					Data->SetStringField(TEXT("directory"), Directory);
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("delete_folder"))
				{
					FString Path;
					if (!RequireFolder(Body, TEXT("path"), Responder, Path))
					{
						return;
					}
					// Delete the contents first: AssetViewUtils::DeleteFolders only
					// skips its confirmation dialog when the folder is already empty.
					TArray<FAssetData> Contents;
					AssetViewUtils::GetAssetsInPaths({Path}, Contents);
					int32 DeletedAssets = 0;
					if (!Contents.IsEmpty())
					{
						bool bRecursive = false;
						Body->TryGetBoolField(TEXT("delete_contents"), bRecursive);
						if (!bRecursive)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("folder_not_empty"),
								FString::Printf(
									TEXT("'%s' still holds %d asset(s) — pass delete_contents:true to delete them too"),
									*Path, Contents.Num()));
							return;
						}
						// A folder whose assets reference each other cannot be
						// emptied by the ordinary path: DeleteAssets refuses
						// anything with a referencer, even one inside the set.
						if (BoolOr(Body, TEXT("force"), false))
						{
							TArray<UObject*> Objects;
							for (const FAssetData& Asset : Contents)
							{
								if (UObject* Object = Asset.GetAsset())
								{
									Objects.Add(Object);
								}
							}
							DeletedAssets =
								ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation*/ false);
						}
						else
						{
							DeletedAssets =
								ObjectTools::DeleteAssets(Contents, /*bShowConfirmation*/ false);
						}
						if (DeletedAssets < Contents.Num())
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("delete_failed"),
								FString::Printf(
									TEXT("only %d of %d assets in '%s' could be deleted — they still have ")
									TEXT("referencers (assets in the same folder referencing each other ")
									TEXT("count). Pass force:true to null out the references and delete anyway"),
									DeletedAssets, Contents.Num(), *Path));
							return;
						}
					}
					AssetViewUtils::FDeleteFolderParameters Options;
					Options.PathsToDelete = {Path};
					Options.bShowConfirmationBeforeLoadingAssets = false;
					const bool bDeleted = AssetViewUtils::DeleteFolders(Options);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("deleted"), bDeleted);
					Data->SetNumberField(TEXT("assets_deleted"), DeletedAssets);
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("rename_folder"))
				{
					FString Path, Destination;
					if (!RequireFolder(Body, TEXT("path"), Responder, Path)
						|| !RequireFolder(Body, TEXT("destination"), Responder, Destination))
					{
						return;
					}
					if (!AssetViewUtils::RenameFolder(Destination, Path))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("rename_failed"),
							FString::Printf(TEXT("could not rename '%s' to '%s'"), *Path, *Destination));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Destination);
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("list_folders"))
				{
					FString Path = TEXT("/Game");
					Body->TryGetStringField(TEXT("path"), Path);
					bool bRecursive = false;
					Body->TryGetBoolField(TEXT("recursive"), bRecursive);

					TArray<FString> SubPaths;
					GetRegistry().GetSubPaths(Path, SubPaths, bRecursive);
					SubPaths.Sort();
					TArray<TSharedPtr<FJsonValue>> Values;
					for (const FString& SubPath : SubPaths)
					{
						Values.Add(MakeShared<FJsonValueString>(SubPath));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Path);
					Data->SetNumberField(TEXT("total"), Values.Num());
					Data->SetArrayField(TEXT("folders"), Values);
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("fixup_redirectors"))
				{
					FString Path = TEXT("/Game");
					Body->TryGetStringField(TEXT("path"), Path);

					FARFilter Filter;
					Filter.bRecursivePaths = true;
					Filter.PackagePaths.Add(FName(*Path));
					Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
					TArray<FAssetData> Found;
					GetRegistry().GetAssets(Filter, Found);

					TArray<UObjectRedirector*> Redirectors;
					for (const FAssetData& Asset : Found)
					{
						if (UObjectRedirector* Redirector = Cast<UObjectRedirector>(Asset.GetAsset()))
						{
							Redirectors.Add(Redirector);
						}
					}
					if (!Redirectors.IsEmpty())
					{
						GetAssetTools().FixupReferencers(
							Redirectors, /*bCheckoutDialogPrompt*/ false);
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("fixed"), Redirectors.Num());
					Data->SetStringField(TEXT("path"), Path);
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("export"))
				{
					const TArray<FString> Paths = GetPathList(Body);
					FString Directory;
					if (Paths.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'path' (or 'paths') is required"));
						return;
					}
					if (!RequireString(Body, TEXT("directory"), Directory, Responder,
						TEXT("an absolute directory on disk to write the exported files into")))
					{
						return;
					}
					TArray<UObject*> Assets;
					for (const FString& Path : Paths)
					{
						UObject* Asset = LoadAsset(Path);
						if (Asset == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
								FString::Printf(TEXT("no asset at '%s'"), *Path));
							return;
						}
						Assets.Add(Asset);
					}
					IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
					GetAssetTools().ExportAssetsWithCleanFilename(Assets, Directory);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("exported"), Assets.Num());
					Data->SetStringField(TEXT("directory"), Directory);
					Data->SetStringField(TEXT("note"),
						TEXT("an asset type with no exporter writes nothing — check the directory listing"));
					Responder->Ok(Data);
					return;
				}

				// ---------------------------------------------------------------
				if (Operation == TEXT("get_metadata") || Operation == TEXT("set_metadata"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder))
					{
						return;
					}
					UObject* Asset = LoadAsset(Path);
					if (Asset == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
							FString::Printf(TEXT("no asset at '%s'"), *Path));
						return;
					}
					FMetaData& MetaData = Asset->GetOutermost()->GetMetaData();

					if (Operation == TEXT("set_metadata"))
					{
						FString Key;
						if (!RequireString(Body, TEXT("key"), Key, Responder,
							TEXT("a metadata tag name")))
						{
							return;
						}
						FString Value;
						if (Body->TryGetStringField(TEXT("value"), Value))
						{
							MetaData.SetValue(Asset, *Key, *Value);
						}
						else
						{
							MetaData.RemoveValue(Asset, *Key);
						}
						Asset->GetOutermost()->MarkPackageDirty();
					}

					const TSharedRef<FJsonObject> Tags = MakeShared<FJsonObject>();
					if (const TMap<FName, FString>* Map = FMetaData::GetMapForObject(Asset))
					{
						for (const TPair<FName, FString>& Pair : *Map)
						{
							Tags->SetStringField(Pair.Key.ToString(), Pair.Value);
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Asset->GetPathName());
					Data->SetObjectField(TEXT("metadata"), Tags);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, import, reimport, rename, move, duplicate, ")
						TEXT("delete, save, save_all, create_folder, delete_folder, rename_folder, ")
						TEXT("list_folders, fixup_redirectors, export, get_metadata or set_metadata"),
						*Operation));
			});
	}
}
