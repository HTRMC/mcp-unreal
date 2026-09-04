// Sublevels: the streaming levels layered under the persistent level.
//
// level_ops handles the one level open in the editor; this handles the rest of
// the world. UEditorLevelUtils is the Levels panel's own back end, so adding,
// creating, removing, hiding and locking sublevels here does exactly what the
// panel does — including the actor moves and transaction bookkeeping.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LevelStreamingAlwaysLoaded.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/World.h"
#include "LevelUtils.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

namespace McpLink
{
	namespace Streaming
	{
		/// "always_loaded" or "dynamic" (the Levels panel's two choices).
		TSubclassOf<ULevelStreaming> StreamingClass(const TSharedRef<FJsonObject>& Body)
		{
			FString Kind;
			Body->TryGetStringField(TEXT("streaming"), Kind);
			return Kind.Equals(TEXT("dynamic"), ESearchCase::IgnoreCase)
				? TSubclassOf<ULevelStreaming>(ULevelStreamingDynamic::StaticClass())
				: TSubclassOf<ULevelStreaming>(ULevelStreamingAlwaysLoaded::StaticClass());
		}

		ULevelStreaming* FindStreamingLevel(UWorld* World, const FString& Spec)
		{
			for (ULevelStreaming* Level : World->GetStreamingLevels())
			{
				if (Level == nullptr)
				{
					continue;
				}
				const FString Package = Level->GetWorldAssetPackageName();
				if (Package == Spec || FPackageName::GetShortName(Package) == Spec)
				{
					return Level;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> StreamingToJson(ULevelStreaming* Streaming, UWorld* World)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			const FString Package = Streaming->GetWorldAssetPackageName();
			Object->SetStringField(TEXT("package"), Package);
			Object->SetStringField(TEXT("name"), FPackageName::GetShortName(Package));
			Object->SetStringField(TEXT("streaming_class"), Streaming->GetClass()->GetName());
			Object->SetBoolField(TEXT("visible"), Streaming->GetShouldBeVisibleInEditor());
			ULevel* Loaded = Streaming->GetLoadedLevel();
			Object->SetBoolField(TEXT("loaded"), Loaded != nullptr);
			if (Loaded != nullptr)
			{
				Object->SetBoolField(TEXT("locked"), Loaded->bLocked);
				Object->SetNumberField(TEXT("actors"), Loaded->Actors.Num());
				Object->SetBoolField(TEXT("current"), World->GetCurrentLevel() == Loaded);
			}
			const FTransform Transform = Streaming->LevelTransform;
			Object->SetArrayField(TEXT("location"), VectorToJson(Transform.GetLocation()));
			Object->SetArrayField(TEXT("rotation"), RotatorToJson(Transform.Rotator()));
			return Object;
		}
	}

	using namespace Streaming;

	void RegisterStreamingRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/world/streaming"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				UWorld* World = ResolveWorldOrError(Body, Responder);
				if (World == nullptr)
				{
					return;
				}

				if (Operation == TEXT("list"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("persistent_level"),
						World->PersistentLevel != nullptr
							? World->PersistentLevel->GetOutermost()->GetName()
							: FString());
					Data->SetBoolField(TEXT("world_partition"), World->IsPartitionedWorld());
					TArray<TSharedPtr<FJsonValue>> Levels;
					for (ULevelStreaming* Level : World->GetStreamingLevels())
					{
						if (Level != nullptr)
						{
							Levels.Add(MakeShared<FJsonValueObject>(StreamingToJson(Level, World)));
						}
					}
					Data->SetNumberField(TEXT("count"), Levels.Num());
					Data->SetArrayField(TEXT("levels"), Levels);
					if (World->IsPartitionedWorld())
					{
						Data->SetStringField(TEXT("note"),
							TEXT("this is a World Partition map — it streams by cell, not by sublevel; ")
							TEXT("see world_partition_ops"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("package path for the new sublevel, e.g. /Game/Maps/Sub_Props")))
					{
						return;
					}
					if (FPackageName::DoesPackageExist(Path))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("a level already exists at '%s' — use add instead"), *Path));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateSublevel", "McpLink Create Sublevel"));
					// DefaultFilename is a filesystem path, not a package path.
					UEditorLevelUtils::FCreateNewStreamingLevelForWorldParams Params(
						StreamingClass(Body),
						FPackageName::LongPackageNameToFilename(
							Path, FPackageName::GetMapPackageExtension()));
					// A Save As dialog would hang a headless editor, and the
					// destination is already known.
					Params.bUseSaveAs = false;
					FVector NewLocation = FVector::ZeroVector;
					FRotator NewRotation = FRotator::ZeroRotator;
					if (GetVector(Body, TEXT("location"), NewLocation)
						| GetRotator(Body, TEXT("rotation"), NewRotation))
					{
						Params.Transform = FTransform(NewRotation, NewLocation);
					}
					ULevelStreaming* Created =
						UEditorLevelUtils::CreateNewStreamingLevelForWorld(*World, Params);
					if (Created == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("create_failed"),
							FString::Printf(TEXT("could not create a sublevel at '%s'"), *Path));
						return;
					}
					Responder->Ok(StreamingToJson(Created, World));
					return;
				}

				if (Operation == TEXT("add"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("package path of an existing level, e.g. /Game/Maps/Sub_Props")))
					{
						return;
					}
					if (!FPackageName::DoesPackageExist(Path))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("level_not_found"),
							FString::Printf(TEXT("no level package at '%s' — use create for a new one"), *Path));
						return;
					}
					if (FindStreamingLevel(World, Path) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_added"),
							FString::Printf(TEXT("'%s' is already a sublevel of this world"), *Path));
						return;
					}
					FTransform Transform = FTransform::Identity;
					FVector Location = FVector::ZeroVector;
					FRotator Rotation = FRotator::ZeroRotator;
					if (GetVector(Body, TEXT("location"), Location)
						| GetRotator(Body, TEXT("rotation"), Rotation))
					{
						Transform = FTransform(Rotation, Location);
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddSublevel", "McpLink Add Sublevel"));
					ULevelStreaming* Added = UEditorLevelUtils::AddLevelToWorld(
						World, *Path, StreamingClass(Body), Transform);
					if (Added == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("add_failed"),
							FString::Printf(TEXT("could not add '%s' as a sublevel"), *Path));
						return;
					}
					Responder->Ok(StreamingToJson(Added, World));
					return;
				}

				// ---- everything below names an existing streaming level ----
				FString LevelSpec;
				if (!RequireString(Body, TEXT("level"), LevelSpec, Responder,
						TEXT("the sublevel's package path or short name, from `list`")))
				{
					return;
				}
				ULevelStreaming* Streaming = FindStreamingLevel(World, LevelSpec);
				if (Streaming == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("level_not_found"),
						FString::Printf(TEXT("world has no sublevel '%s' — see `list`"), *LevelSpec));
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditSublevel", "McpLink Edit Sublevel"));

				if (Operation == TEXT("remove"))
				{
					ULevel* Loaded = Streaming->GetLoadedLevel();
					if (Loaded == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_loaded"),
							TEXT("that sublevel is not loaded, so it cannot be removed"));
						return;
					}
					if (!UEditorLevelUtils::RemoveLevelFromWorld(Loaded))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("remove_failed"),
							TEXT("the editor refused to remove that level (is it the current level?)"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), LevelSpec);
					Data->SetNumberField(TEXT("count"), World->GetStreamingLevels().Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_visible"))
				{
					ULevel* Loaded = Streaming->GetLoadedLevel();
					const bool bVisible = BoolOr(Body, TEXT("visible"), true);
					if (Loaded != nullptr)
					{
						UEditorLevelUtils::SetLevelVisibility(
							Loaded, bVisible, /*bForceLayersVisible*/ true);
					}
					else
					{
						Streaming->SetShouldBeVisibleInEditor(bVisible);
					}
					Responder->Ok(StreamingToJson(Streaming, World));
					return;
				}

				if (Operation == TEXT("set_locked"))
				{
					ULevel* Loaded = Streaming->GetLoadedLevel();
					if (Loaded == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_loaded"),
							TEXT("that sublevel is not loaded, so it cannot be locked"));
						return;
					}
					Loaded->bLocked = BoolOr(Body, TEXT("locked"), true);
					Responder->Ok(StreamingToJson(Streaming, World));
					return;
				}

				if (Operation == TEXT("set_current"))
				{
					UEditorLevelUtils::MakeLevelCurrent(Streaming);
					Responder->Ok(StreamingToJson(Streaming, World));
					return;
				}

				if (Operation == TEXT("set_transform"))
				{
					FVector Location = Streaming->LevelTransform.GetLocation();
					FRotator Rotation = Streaming->LevelTransform.Rotator();
					GetVector(Body, TEXT("location"), Location);
					GetRotator(Body, TEXT("rotation"), Rotation);
					Streaming->Modify();
					// SetEditorTransform moves the loaded actors with the level,
					// which assigning LevelTransform on its own would not.
					FLevelUtils::SetEditorTransform(Streaming, FTransform(Rotation, Location));
					Responder->Ok(StreamingToJson(Streaming, World));
					return;
				}

				if (Operation == TEXT("move_actors"))
				{
					ULevel* Loaded = Streaming->GetLoadedLevel();
					if (Loaded == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_loaded"),
							TEXT("that sublevel is not loaded, so actors cannot move into it"));
						return;
					}
					const TArray<TSharedPtr<FJsonValue>>* Specs = nullptr;
					TArray<AActor*> Actors;
					TArray<FString> Missing;
					if (Body->TryGetArrayField(TEXT("actors"), Specs) && Specs != nullptr)
					{
						for (const TSharedPtr<FJsonValue>& Value : *Specs)
						{
							FString Spec;
							if (!Value.IsValid() || !Value->TryGetString(Spec))
							{
								continue;
							}
							if (AActor* Actor = ResolveActor(World, Spec))
							{
								Actors.Add(Actor);
							}
							else
							{
								Missing.Add(Spec);
							}
						}
					}
					if (Actors.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("no_actors"),
							Missing.IsEmpty()
								? TEXT("'actors' must be a non-empty array of actor paths or labels")
								: *FString::Printf(TEXT("none of these actors were found: %s"),
									  *FString::Join(Missing, TEXT(", "))));
						return;
					}
					const int32 Moved = UEditorLevelUtils::MoveActorsToLevel(Actors, Loaded);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("level"), LevelSpec);
					Data->SetNumberField(TEXT("moved"), Moved);
					if (!Missing.IsEmpty())
					{
						Data->SetStringField(TEXT("warning"),
							FString::Printf(TEXT("not found: %s"), *FString::Join(Missing, TEXT(", "))));
					}
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list, create, add, remove, set_visible, ")
						TEXT("set_locked, set_current, set_transform, or move_actors"),
						*Operation));
			});
	}
}
