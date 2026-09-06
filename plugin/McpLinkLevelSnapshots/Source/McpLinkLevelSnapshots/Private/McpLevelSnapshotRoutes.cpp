// Level Snapshots: capture every actor's properties in a level into an
// asset, see what has changed since, and restore the level to it.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Data/LevelSnapshot.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Filtering/PropertySelectionMap.h"
#include "GameFramework/Actor.h"
#include "LevelSnapshotsFilteringLibrary.h"
#include "LevelSnapshotsFunctionLibrary.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace Snapshots
	{
		ULevelSnapshot* SnapshotOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("snapshot"), Path, Responder, TEXT("a Level Snapshot asset path")))
			{
				return nullptr;
			}
			ULevelSnapshot* Snapshot = Cast<ULevelSnapshot>(ResolveAsset(Path));
			if (Snapshot == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("snapshot_not_found"),
					FString::Printf(TEXT("no Level Snapshot at '%s'"), *Path));
			}
			return Snapshot;
		}

		TSharedRef<FJsonObject> SnapshotJson(ULevelSnapshot& Snapshot)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("snapshot"), Snapshot.GetPathName());
			Data->SetStringField(TEXT("name"), Snapshot.GetSnapshotName().ToString());
			Data->SetStringField(TEXT("description"), Snapshot.GetSnapshotDescription());
			Data->SetStringField(TEXT("map"), Snapshot.GetMapPath().ToString());
			Data->SetStringField(TEXT("captured"), Snapshot.GetCaptureTime().ToIso8601());
			Data->SetNumberField(TEXT("actors"), Snapshot.GetNumSavedActors());
			return Data;
		}

		/// The snapshot's map must be the world that is open; a snapshot of
		/// another level restores nothing useful.
		bool CheckSameMap(ULevelSnapshot& Snapshot, UWorld& World, const TSharedRef<FMcpResponder>& Responder)
		{
			const FString SnapshotMap = Snapshot.GetMapPath().GetLongPackageName();
			const FString WorldMap = World.GetOutermost()->GetName();
			if (!SnapshotMap.IsEmpty() && SnapshotMap != WorldMap)
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("different_map"),
					FString::Printf(TEXT("the snapshot was taken in '%s' but '%s' is open"), *SnapshotMap, *WorldMap));
				return false;
			}
			return true;
		}
	}

	void RegisterLevelSnapshotRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace Snapshots;

		Core.RegisterRoute(TEXT("/api/world/snapshot"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("take"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Snapshots/LS_Before")))
					{
						return;
					}
					UWorld* World = ResolveWorldOrError(Body, Responder);
					if (World == nullptr)
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
					FString Name, Description;
					Body->TryGetStringField(TEXT("name"), Name);
					Body->TryGetStringField(TEXT("description"), Description);
					if (Name.IsEmpty())
					{
						Name = FPackageName::GetShortName(Path);
					}
					UPackage* Package = CreatePackage(*Path);
					// The library's own path: a snapshot object in the package,
					// then every actor serialized into it.
					ULevelSnapshot* Snapshot = ULevelSnapshotsFunctionLibrary::TakeLevelSnapshot_Internal(
						World, FName(*Name), Package, Description);
					if (Snapshot == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("snapshot_failed"),
							TEXT("Level Snapshots refused to capture the world — see the log"));
						return;
					}
					Snapshot->SetFlags(RF_Public | RF_Standalone);
					FAssetRegistryModule::AssetCreated(Snapshot);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = SnapshotJson(*Snapshot);
					Data->SetStringField(TEXT("message"),
						TEXT("captured — diff shows what changes from here, apply restores it; save keeps the asset"));
					Responder->Ok(Data);
					return;
				}

				ULevelSnapshot* Snapshot = SnapshotOrError(Body, Responder);
				if (Snapshot == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					Responder->Ok(SnapshotJson(*Snapshot));
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Snapshot, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = SnapshotJson(*Snapshot);
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				UWorld* World = ResolveWorldOrError(Body, Responder);
				if (World == nullptr || !CheckSameMap(*Snapshot, *World, Responder))
				{
					return;
				}

				if (Operation == TEXT("diff"))
				{
					const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_results"), 200), 1, 5000);
					TArray<TSharedPtr<FJsonValue>> Changed, Unchanged, Removed, Added;
					int32 ChangedCount = 0, UnchangedCount = 0, RemovedCount = 0, AddedCount = 0;
					const bool bListUnchanged = BoolOr(Body, TEXT("include_unchanged"), false);
					Snapshot->DiffWorld(World,
						ULevelSnapshot::FActorConsumer::CreateLambda([&](AActor* Actor)
						{
							if (Actor == nullptr)
							{
								return;
							}
							if (Snapshot->HasChangedSinceSnapshotWasTaken(Actor))
							{
								++ChangedCount;
								if (Changed.Num() < Max)
								{
									Changed.Add(MakeShared<FJsonValueString>(Actor->GetPathName()));
								}
							}
							else
							{
								++UnchangedCount;
								if (bListUnchanged && Unchanged.Num() < Max)
								{
									Unchanged.Add(MakeShared<FJsonValueString>(Actor->GetPathName()));
								}
							}
						}),
						ULevelSnapshot::FActorPathConsumer::CreateLambda([&](const FSoftObjectPath& ActorPath)
						{
							++RemovedCount;
							if (Removed.Num() < Max)
							{
								Removed.Add(MakeShared<FJsonValueString>(ActorPath.ToString()));
							}
						}),
						ULevelSnapshot::FActorConsumer::CreateLambda([&](AActor* Actor)
						{
							++AddedCount;
							if (Actor != nullptr && Added.Num() < Max)
							{
								Added.Add(MakeShared<FJsonValueString>(Actor->GetPathName()));
							}
						}));
					const TSharedRef<FJsonObject> Data = SnapshotJson(*Snapshot);
					Data->SetStringField(TEXT("world"), World->GetName());
					Data->SetNumberField(TEXT("changed_count"), ChangedCount);
					Data->SetNumberField(TEXT("unchanged_count"), UnchangedCount);
					Data->SetNumberField(TEXT("removed_count"), RemovedCount);
					Data->SetNumberField(TEXT("added_count"), AddedCount);
					Data->SetArrayField(TEXT("changed"), Changed);
					if (bListUnchanged)
					{
						Data->SetArrayField(TEXT("unchanged"), Unchanged);
					}
					// Actors in the snapshot that the level no longer has, and
					// actors the level has that the snapshot never saw.
					Data->SetArrayField(TEXT("removed"), Removed);
					Data->SetArrayField(TEXT("added"), Added);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("apply"))
				{
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "ApplySnapshot", "McpLink Apply Level Snapshot"), ShouldTransact(World));
					// No filter: every changed property of every matched actor,
					// respawn what was deleted, despawn what was added — the
					// snapshot editor's Restore with nothing unticked.
					const FPropertySelectionMap Selection =
						ULevelSnapshotsFilteringLibrary::DiffAndFilterSnapshot(World, Snapshot, nullptr);
					Snapshot->ApplySnapshotToWorld(World, Selection);
					const TSharedRef<FJsonObject> Data = SnapshotJson(*Snapshot);
					Data->SetStringField(TEXT("world"), World->GetName());
					Data->SetBoolField(TEXT("applied"), true);
					Data->SetStringField(TEXT("message"),
						TEXT("restored — an undo reverses it; diff should now report nothing changed"));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected take, info, diff, apply or save"), *Operation));
			});
	}
}
