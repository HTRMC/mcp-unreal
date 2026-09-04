// Level Instances, Packed Level Actors and actor merging.
//
// All three answer the same question — "these actors are one thing, treat them
// as one" — at different costs: a Level Instance keeps them editable in their
// own level, a Packed Level Actor bakes them into ISM components, and merging
// collapses them into a single Static Mesh asset.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "IMeshMergeUtilities.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceTypes.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "MeshMerge/MeshMergingSettings.h"
#include "MeshMergeModule.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "PackedLevelActor/PackedLevelActor.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace LevelInstances
	{
		ULevelInstanceSubsystem* SubsystemOf(UWorld* World)
		{
			return World != nullptr ? World->GetSubsystem<ULevelInstanceSubsystem>() : nullptr;
		}

		TSharedRef<FJsonObject> InstanceToJson(AActor* Actor, ILevelInstanceInterface* Instance)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("actor"), Actor->GetPathName());
			Object->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Object->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
			Object->SetStringField(TEXT("level"), Instance->GetWorldAssetPackage());
			Object->SetBoolField(TEXT("packed"), Actor->IsA<APackedLevelActor>());
			Object->SetArrayField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
			return Object;
		}

		/// Actors named in `actors`, plus the specs that matched nothing.
		TArray<AActor*> ReadInstanceActors(
			UWorld* World, const TSharedRef<FJsonObject>& Body, TArray<FString>& OutMissing)
		{
			TArray<AActor*> Actors;
			const TArray<TSharedPtr<FJsonValue>>* Specs = nullptr;
			if (!Body->TryGetArrayField(TEXT("actors"), Specs) || Specs == nullptr)
			{
				return Actors;
			}
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
					OutMissing.Add(Spec);
				}
			}
			return Actors;
		}
	}

	using namespace LevelInstances;

	void RegisterLevelInstanceRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/world/instances"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				UWorld* World = ResolveWorldOrError(Body, Responder);
				if (World == nullptr)
				{
					return;
				}
				ULevelInstanceSubsystem* Subsystem = SubsystemOf(World);
				if (Subsystem == nullptr && Operation != TEXT("merge_actors"))
				{
					Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_subsystem"),
						TEXT("this world has no Level Instance subsystem"));
					return;
				}

				if (Operation == TEXT("list"))
				{
					TArray<TSharedPtr<FJsonValue>> Instances;
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						if (ILevelInstanceInterface* Instance = Cast<ILevelInstanceInterface>(*It))
						{
							Instances.Add(MakeShared<FJsonValueObject>(InstanceToJson(*It, Instance)));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("count"), Instances.Num());
					Data->SetArrayField(TEXT("instances"), Instances);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("package path for the new level, e.g. /Game/Maps/LI_Shed")))
					{
						return;
					}
					TArray<FString> Missing;
					TArray<AActor*> Actors = ReadInstanceActors(World, Body, Missing);
					if (Actors.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("no_actors"),
							Missing.IsEmpty()
								? TEXT("'actors' must be a non-empty array of actor paths or labels")
								: *FString::Printf(TEXT("none of these actors were found: %s"),
									  *FString::Join(Missing, TEXT(", "))));
						return;
					}
					// ULevelInstanceSubsystem::CreateLevelInstanceFrom hardcodes
					// bUseSaveAs, so it always goes through a Save As dialog.
					// With no UI to host that dialog the editor exits, taking
					// the HTTP server with it — so refuse first and say why.
					if (!FApp::CanEverRender())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("needs_ui"),
							TEXT("creating a level instance opens a Save As dialog (the engine hardcodes it), ")
							TEXT("which an editor without rendering (-nullrhi) cannot show — it would exit. ")
							TEXT("Run a windowed editor, or group the actors with sublevel_ops create + ")
							TEXT("move_actors instead."));
						return;
					}
					FText Reason;
					if (!Subsystem->CanCreateLevelInstanceFrom(Actors, &Reason))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("cannot_create"),
							Reason.IsEmpty()
								? TEXT("those actors cannot become a level instance")
								: *Reason.ToString());
						return;
					}

					FNewLevelInstanceParams Params;
					FString Kind;
					Body->TryGetStringField(TEXT("kind"), Kind);
					Params.Type = Kind.Equals(TEXT("packed"), ESearchCase::IgnoreCase)
						? ELevelInstanceCreationType::PackedLevelActor
						: ELevelInstanceCreationType::LevelInstance;
					Params.LevelPackageName = Path;
					// Every dialog has to be off or a headless editor hangs.
					Params.bAlwaysShowDialog = false;
					Params.bPromptForSave = false;
					Params.HideCreationType();
					Params.SetExternalActors(BoolOr(Body, TEXT("external_actors"), false));

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateLevelInstance", "McpLink Create Level Instance"));
					ILevelInstanceInterface* Created = Subsystem->CreateLevelInstanceFrom(Actors, Params);
					AActor* CreatedActor = Cast<AActor>(Created);
					if (CreatedActor == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("create_failed"),
							FString::Printf(
								TEXT("the level instance subsystem returned nothing for '%s'"), *Path));
						return;
					}
					const TSharedRef<FJsonObject> Data = InstanceToJson(CreatedActor, Created);
					Data->SetStringField(TEXT("note"),
						TEXT("the engine showed a Save As dialog for the new level; it is saved where that ")
						TEXT("dialog confirmed, which may differ from `path`"));
					if (!Missing.IsEmpty())
					{
						Data->SetStringField(TEXT("warning"),
							FString::Printf(TEXT("not found: %s"), *FString::Join(Missing, TEXT(", "))));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("break"))
				{
					FString Spec;
					if (!RequireString(Body, TEXT("actor"), Spec, Responder,
							TEXT("the level instance actor, from `list`")))
					{
						return;
					}
					AActor* Actor = ResolveActor(World, Spec);
					ILevelInstanceInterface* Instance = Cast<ILevelInstanceInterface>(Actor);
					if (Instance == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("instance_not_found"),
							FString::Printf(TEXT("'%s' is not a level instance actor"), *Spec));
						return;
					}
					if (!Subsystem->CanBreakLevelInstance(Instance))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("cannot_break"),
							TEXT("that level instance cannot be broken (is it being edited?)"));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "BreakLevelInstance", "McpLink Break Level Instance"));
					TArray<AActor*> Moved;
					const uint32 Levels = FMath::Max(1, IntOr(Body, TEXT("levels"), 1));
					if (!Subsystem->BreakLevelInstance(Instance, Levels, &Moved))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("break_failed"),
							TEXT("the level instance subsystem refused to break it"));
						return;
					}
					TArray<TSharedPtr<FJsonValue>> Actors;
					for (AActor* Result : Moved)
					{
						if (Result != nullptr)
						{
							Actors.Add(MakeShared<FJsonValueString>(Result->GetPathName()));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("actors"), Actors.Num());
					Data->SetArrayField(TEXT("moved"), Actors);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("merge_actors"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("package path for the merged Static Mesh, e.g. /Game/Meshes/SM_Merged")))
					{
						return;
					}
					TArray<FString> Missing;
					TArray<AActor*> Actors = ReadInstanceActors(World, Body, Missing);
					TArray<UPrimitiveComponent*> Components;
					for (AActor* Actor : Actors)
					{
						// GetComponents resets its output array, so collect per
						// actor and append — passing the shared array would keep
						// only the last actor's components.
						TArray<UPrimitiveComponent*> Own;
						Actor->GetComponents<UPrimitiveComponent>(Own, /*bIncludeChildren*/ true);
						Components.Append(Own);
					}
					if (Components.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("no_components"),
							TEXT("'actors' held no primitive components to merge"));
						return;
					}
					if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr, *Path) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}

					FMeshMergingSettings Settings;
					Settings.bMergePhysicsData = BoolOr(Body, TEXT("merge_physics"), true);
					Settings.bMergeMaterials = BoolOr(Body, TEXT("merge_materials"), false);
					Settings.bMergeMeshSockets = BoolOr(Body, TEXT("merge_sockets"), false);
					Settings.bGenerateLightMapUV = BoolOr(Body, TEXT("generate_lightmap_uvs"), true);

					const IMeshMergeUtilities& Utilities =
						FModuleManager::Get()
							.LoadModuleChecked<IMeshMergeModule>("MeshMergeUtilities")
							.GetUtilities();
					TArray<UObject*> Created;
					FVector MergedLocation = FVector::ZeroVector;
					// The merge prefixes the asset name with "SM_", so strip one
					// the caller already wrote or the result is SM_SM_Whatever.
					FString BaseName = FPackageName::GetShortName(Path);
					BaseName.RemoveFromStart(TEXT("SM_"));
					const FString BasePackageName =
						FPackageName::GetLongPackagePath(Path) / BaseName;
					// bSilent, or the merge opens a progress dialog headless.
					Utilities.MergeComponentsToStaticMesh(Components, World, Settings, nullptr, nullptr,
						BasePackageName, Created, MergedLocation, TNumericLimits<float>::Max(),
						/*bSilent*/ true);

					UStaticMesh* Mesh = nullptr;
					for (UObject* Object : Created)
					{
						if (UStaticMesh* Candidate = Cast<UStaticMesh>(Object))
						{
							Mesh = Candidate;
						}
					}
					if (Mesh == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("merge_failed"),
							TEXT("the merge produced no Static Mesh — the components had no geometry to ")
							TEXT("merge (do the actors actually have meshes assigned?)"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
					Data->SetNumberField(TEXT("components"), Components.Num());
					Data->SetArrayField(TEXT("location"), VectorToJson(MergedLocation));
					Data->SetStringField(TEXT("message"),
						TEXT("merged in memory — save it with asset_ops save; the source actors are untouched"));
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
						TEXT("unknown operation '%s' — use list, create, break, or merge_actors"),
						*Operation));
			});
	}
}
