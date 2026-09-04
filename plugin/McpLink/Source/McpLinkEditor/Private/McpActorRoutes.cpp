#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "ScopedTransaction.h"

namespace McpLink
{
	namespace
	{
		UWorld* WorldOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			UWorld* World = ResolveWorld(Body);
			if (World == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("pie_not_running"),
					TEXT("world 'pie' requested but no PIE session is running — start one with pie_control"));
			}
			return World;
		}

		AActor* ActorOrError(
			UWorld* World, const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder,
			const TCHAR* Field = TEXT("actor"))
		{
			FString Spec;
			if (!Body->TryGetStringField(Field, Spec) || Spec.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					FString::Printf(TEXT("'%s' is required (object path or editor label)"), Field));
				return nullptr;
			}
			AActor* Actor = ResolveActor(World, Spec);
			if (Actor == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
					FString::Printf(TEXT("no actor '%s' in %s — use /api/actors/list to discover"),
						*Spec, *World->GetMapName()));
			}
			return Actor;
		}

		TSharedRef<FJsonObject> ActorToJson(const AActor* Actor)
		{
			const TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Obj->SetStringField(TEXT("path"), Actor->GetPathName());
			Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
			Obj->SetArrayField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
			Obj->SetArrayField(TEXT("rotation"), RotatorToJson(Actor->GetActorRotation()));
			Obj->SetArrayField(TEXT("scale"), VectorToJson(Actor->GetActorScale3D()));
			return Obj;
		}
	}

	void RegisterActorRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/actors/list"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				UWorld* World = WorldOrError(Body, Responder);
				if (!World) { return; }

				FString ClassFilterName, NameFilter;
				Body->TryGetStringField(TEXT("class_filter"), ClassFilterName);
				Body->TryGetStringField(TEXT("name_filter"), NameFilter);
				UClass* ClassFilter = ClassFilterName.IsEmpty() ? nullptr : ResolveClass(ClassFilterName);
				if (!ClassFilterName.IsEmpty() && ClassFilter == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_class"),
						FString::Printf(TEXT("class '%s' not found"), *ClassFilterName));
					return;
				}
				const int32 MaxResults = FMath::Clamp(
					Body->HasTypedField<EJson::Number>(TEXT("max_results"))
						? static_cast<int32>(Body->GetNumberField(TEXT("max_results")))
						: 200,
					1, 1000);

				TArray<TSharedPtr<FJsonValue>> Actors;
				int32 Total = 0;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					const AActor* Actor = *It;
					if (ClassFilter && !Actor->GetClass()->IsChildOf(ClassFilter))
					{
						continue;
					}
					if (!NameFilter.IsEmpty() && !Actor->GetActorLabel().Contains(NameFilter)
						&& !Actor->GetName().Contains(NameFilter))
					{
						continue;
					}
					++Total;
					if (Actors.Num() < MaxResults)
					{
						Actors.Add(MakeShared<FJsonValueObject>(ActorToJson(Actor)));
					}
				}

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("map"), World->GetMapName());
				Data->SetNumberField(TEXT("total"), Total);
				Data->SetArrayField(TEXT("actors"), Actors);
				Responder->Ok(Data);
			});

		Core.RegisterRoute(TEXT("/api/actors/spawn"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				UWorld* World = WorldOrError(Body, Responder);
				if (!World) { return; }
				FString ClassSpec;
				if (!Body->TryGetStringField(TEXT("class"), ClassSpec) || ClassSpec.IsEmpty())
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
						TEXT("'class' is required (e.g. StaticMeshActor, /Script/Engine.PointLight, /Game/BP_Thing)"));
					return;
				}
				UClass* Class = ResolveClass(ClassSpec);
				if (Class == nullptr || !Class->IsChildOf(AActor::StaticClass()))
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_class"),
						FString::Printf(TEXT("'%s' is not a spawnable actor class"), *ClassSpec));
					return;
				}

				FVector Location = FVector::ZeroVector;
				FRotator Rotation = FRotator::ZeroRotator;
				FVector Scale = FVector::OneVector;
				GetVector(Body, TEXT("location"), Location);
				GetRotator(Body, TEXT("rotation"), Rotation);
				const bool bHasScale = GetVector(Body, TEXT("scale"), Scale);

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "SpawnActor", "McpLink Spawn Actor"), ShouldTransact(World));
				FActorSpawnParameters Params;
				Params.SpawnCollisionHandlingOverride =
					ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
				AActor* Actor = World->SpawnActor<AActor>(Class, Location, Rotation, Params);
				if (Actor == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("spawn_failed"),
						FString::Printf(TEXT("SpawnActor returned null for class %s"), *Class->GetName()));
					return;
				}
				if (bHasScale)
				{
					Actor->SetActorScale3D(Scale);
				}
				FString Label;
				if (Body->TryGetStringField(TEXT("name"), Label) && !Label.IsEmpty())
				{
					Actor->SetActorLabel(Label);
				}
				Responder->Ok(ActorToJson(Actor));
			});

		Core.RegisterRoute(TEXT("/api/actors/delete"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				UWorld* World = WorldOrError(Body, Responder);
				if (!World) { return; }
				const TArray<TSharedPtr<FJsonValue>>* Specs = nullptr;
				if (!Body->TryGetArrayField(TEXT("actors"), Specs) || Specs == nullptr || Specs->IsEmpty())
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
						TEXT("'actors' array of paths/labels is required"));
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "DeleteActors", "McpLink Delete Actors"), ShouldTransact(World));
				int32 Deleted = 0;
				TArray<TSharedPtr<FJsonValue>> NotFound;
				for (const TSharedPtr<FJsonValue>& SpecValue : *Specs)
				{
					FString Spec;
					if (!SpecValue->TryGetString(Spec))
					{
						continue;
					}
					AActor* Actor = ResolveActor(World, Spec);
					if (Actor == nullptr)
					{
						NotFound.Add(MakeShared<FJsonValueString>(Spec));
						continue;
					}
					const bool bIsEditorWorld = !World->IsPlayInEditor();
					const bool bDestroyed = bIsEditorWorld
						? World->EditorDestroyActor(Actor, /*bShouldModifyLevel*/ true)
						: Actor->Destroy();
					if (bDestroyed)
					{
						++Deleted;
					}
				}

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetNumberField(TEXT("deleted"), Deleted);
				Data->SetArrayField(TEXT("not_found"), NotFound);
				Responder->Ok(Data);
			});

		Core.RegisterRoute(TEXT("/api/actors/transform"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				UWorld* World = WorldOrError(Body, Responder);
				if (!World) { return; }
				AActor* Actor = ActorOrError(World, Body, Responder);
				if (!Actor) { return; }

				FVector Location;
				FRotator Rotation;
				FVector Scale;
				const bool bHasLocation = GetVector(Body, TEXT("location"), Location);
				const bool bHasRotation = GetRotator(Body, TEXT("rotation"), Rotation);
				const bool bHasScale = GetVector(Body, TEXT("scale"), Scale);
				if (!bHasLocation && !bHasRotation && !bHasScale)
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
						TEXT("at least one of 'location' [X,Y,Z], 'rotation' [Pitch,Yaw,Roll], 'scale' [X,Y,Z] is required"));
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "MoveActor", "McpLink Move Actor"), ShouldTransact(Actor));
				Actor->Modify();
				bool bOk = true;
				if (bHasLocation && bHasRotation)
				{
					bOk = Actor->TeleportTo(Location, Rotation, false, /*bNoCheck*/ true);
				}
				else if (bHasLocation)
				{
					bOk = Actor->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
				}
				else if (bHasRotation)
				{
					bOk = Actor->SetActorRotation(Rotation, ETeleportType::TeleportPhysics);
				}
				if (bHasScale)
				{
					Actor->SetActorScale3D(Scale);
				}

				const TSharedRef<FJsonObject> Data = ActorToJson(Actor);
				Data->SetBoolField(TEXT("moved"), bOk);
				Responder->Ok(Data);
			});

		Core.RegisterRoute(TEXT("/api/actors/components"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				UWorld* World = WorldOrError(Body, Responder);
				if (!World) { return; }
				AActor* Actor = ActorOrError(World, Body, Responder);
				if (!Actor) { return; }

				TArray<TSharedPtr<FJsonValue>> Components;
				for (UActorComponent* Component : Actor->GetComponents())
				{
					if (Component == nullptr)
					{
						continue;
					}
					const TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
					C->SetStringField(TEXT("name"), Component->GetName());
					C->SetStringField(TEXT("class"), Component->GetClass()->GetName());
					C->SetStringField(TEXT("path"), Component->GetPathName());
					if (const USceneComponent* Scene = Cast<USceneComponent>(Component))
					{
						C->SetArrayField(TEXT("relative_location"), VectorToJson(Scene->GetRelativeLocation()));
						C->SetArrayField(TEXT("relative_rotation"), RotatorToJson(Scene->GetRelativeRotation()));
						C->SetArrayField(TEXT("relative_scale"), VectorToJson(Scene->GetRelativeScale3D()));
						C->SetBoolField(TEXT("visible"), Scene->IsVisible());
						if (const USceneComponent* Parent = Scene->GetAttachParent())
						{
							C->SetStringField(TEXT("attach_parent"), Parent->GetName());
						}
					}
					Components.Add(MakeShared<FJsonValueObject>(C));
				}

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("actor"), Actor->GetPathName());
				Data->SetArrayField(TEXT("components"), Components);
				Responder->Ok(Data);
			});
	}
}
