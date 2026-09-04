// Instanced-static-mesh routes: add an ISM/HISM component to an actor and
// manage its instances in bulk. This is how an agent scatters thousands of
// meshes without spawning thousands of actors.

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "ScopedTransaction.h"

namespace McpLink
{
	namespace
	{
		UInstancedStaticMeshComponent* ComponentOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			UWorld* World = ResolveWorld(Body);
			if (World == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("pie_not_running"),
					TEXT("world 'pie' requested but no PIE session is running"));
				return nullptr;
			}
			FString ActorSpec;
			if (!RequireString(Body, TEXT("actor"), ActorSpec, Responder,
				TEXT("an actor path or editor label")))
			{
				return nullptr;
			}
			AActor* Actor = ResolveActor(World, ActorSpec);
			if (Actor == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
					FString::Printf(TEXT("no actor '%s' in %s"), *ActorSpec, *World->GetMapName()));
				return nullptr;
			}

			FString ComponentName;
			Body->TryGetStringField(TEXT("component"), ComponentName);
			UInstancedStaticMeshComponent* Found = nullptr;
			for (UActorComponent* Component : Actor->GetComponents())
			{
				UInstancedStaticMeshComponent* Ism = Cast<UInstancedStaticMeshComponent>(Component);
				if (Ism == nullptr)
				{
					continue;
				}
				if (ComponentName.IsEmpty() || Ism->GetName() == ComponentName)
				{
					Found = Ism;
					break;
				}
			}
			if (Found == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("component_not_found"),
					ComponentName.IsEmpty()
						? FString::Printf(
							TEXT("actor '%s' has no instanced-static-mesh component — add one with operation create"),
							*ActorSpec)
						: FString::Printf(
							TEXT("actor '%s' has no ISM component named '%s'"), *ActorSpec, *ComponentName));
			}
			return Found;
		}

		bool ReadTransform(const TSharedPtr<FJsonObject>& Object, FTransform& OutTransform)
		{
			if (!Object.IsValid())
			{
				return false;
			}
			const TSharedRef<FJsonObject> Ref = Object.ToSharedRef();
			FVector Location = FVector::ZeroVector;
			FRotator Rotation = FRotator::ZeroRotator;
			FVector Scale = FVector::OneVector;
			GetVector(Ref, TEXT("location"), Location);
			GetRotator(Ref, TEXT("rotation"), Rotation);
			GetVector(Ref, TEXT("scale"), Scale);
			OutTransform = FTransform(Rotation, Location, Scale);
			return true;
		}
	}

	void RegisterIsmRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/ism/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				// ---- create attaches a new component to an existing actor ----
				if (Operation == TEXT("create"))
				{
					UWorld* World = ResolveWorld(Body);
					if (World == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("pie_not_running"),
							TEXT("world 'pie' requested but no PIE session is running"));
						return;
					}
					FString ActorSpec, MeshPath;
					if (!RequireString(Body, TEXT("actor"), ActorSpec, Responder,
							TEXT("an actor path or editor label"))
						|| !RequireString(Body, TEXT("mesh"), MeshPath, Responder,
							TEXT("a StaticMesh asset path, e.g. /Engine/BasicShapes/Cube.Cube")))
					{
						return;
					}
					AActor* Actor = ResolveActor(World, ActorSpec);
					if (Actor == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
							FString::Printf(TEXT("no actor '%s'"), *ActorSpec));
						return;
					}
					UStaticMesh* Mesh = Cast<UStaticMesh>(ResolveObject(MeshPath));
					if (Mesh == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("mesh_not_found"),
							FString::Printf(TEXT("no StaticMesh at '%s'"), *MeshPath));
						return;
					}
					bool bHierarchical = false;
					Body->TryGetBoolField(TEXT("hierarchical"), bHierarchical);

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateIsm", "McpLink Add Instanced Mesh Component"), ShouldTransact(Actor));
					Actor->Modify();
					UClass* ComponentClass = bHierarchical
						? UHierarchicalInstancedStaticMeshComponent::StaticClass()
						: UInstancedStaticMeshComponent::StaticClass();
					UInstancedStaticMeshComponent* Component =
						NewObject<UInstancedStaticMeshComponent>(Actor, ComponentClass);
					Component->SetStaticMesh(Mesh);
					Component->AttachToComponent(
						Actor->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
					Component->RegisterComponent();
					Actor->AddInstanceComponent(Component);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("component"), Component->GetName());
					Data->SetStringField(TEXT("class"), Component->GetClass()->GetName());
					Data->SetStringField(TEXT("mesh"), Mesh->GetPathName());
					Responder->Ok(Data);
					return;
				}

				UInstancedStaticMeshComponent* Component = ComponentOrError(Body, Responder);
				if (!Component) { return; }

				if (Operation == TEXT("get_info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("component"), Component->GetName());
					Data->SetStringField(TEXT("class"), Component->GetClass()->GetName());
					Data->SetStringField(TEXT("mesh"),
						Component->GetStaticMesh() ? Component->GetStaticMesh()->GetPathName() : TEXT(""));
					Data->SetNumberField(TEXT("instance_count"), Component->GetInstanceCount());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_instances"))
				{
					const TArray<TSharedPtr<FJsonValue>>* Instances = nullptr;
					if (!Body->TryGetArrayField(TEXT("instances"), Instances) || Instances == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'instances' must be an array of {location:[x,y,z], rotation:[p,y,r], scale:[x,y,z]}"));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddIsmInstances", "McpLink Add Mesh Instances"), ShouldTransact(Component));
					Component->Modify();

					TArray<TSharedPtr<FJsonValue>> AddedIndices;
					for (const TSharedPtr<FJsonValue>& Value : *Instances)
					{
						const TSharedPtr<FJsonObject>* Object = nullptr;
						if (!Value->TryGetObject(Object) || Object == nullptr)
						{
							continue;
						}
						FTransform Transform;
						ReadTransform(*Object, Transform);
						const int32 Index = Component->AddInstance(Transform, /*bWorldSpace*/ false);
						AddedIndices.Add(MakeShared<FJsonValueNumber>(Index));
					}
					Component->MarkRenderStateDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("added"), AddedIndices.Num());
					Data->SetArrayField(TEXT("indices"), AddedIndices);
					Data->SetNumberField(TEXT("instance_count"), Component->GetInstanceCount());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("update_instance"))
				{
					double IndexValue = -1.0;
					if (!Body->TryGetNumberField(TEXT("index"), IndexValue))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'index' is required"));
						return;
					}
					const int32 Index = static_cast<int32>(IndexValue);
					if (Index < 0 || Index >= Component->GetInstanceCount())
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("index_out_of_range"),
							FString::Printf(TEXT("instance %d does not exist (count is %d)"),
								Index, Component->GetInstanceCount()));
						return;
					}
					FTransform Current;
					Component->GetInstanceTransform(Index, Current, /*bWorldSpace*/ false);
					FVector Location = Current.GetLocation();
					FRotator Rotation = Current.Rotator();
					FVector Scale = Current.GetScale3D();
					GetVector(Body, TEXT("location"), Location);
					GetRotator(Body, TEXT("rotation"), Rotation);
					GetVector(Body, TEXT("scale"), Scale);

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "UpdateIsmInstance", "McpLink Update Mesh Instance"), ShouldTransact(Component));
					Component->Modify();
					Component->UpdateInstanceTransform(
						Index, FTransform(Rotation, Location, Scale),
						/*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ true);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("index"), Index);
					Data->SetArrayField(TEXT("location"), VectorToJson(Location));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("clear"))
				{
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "ClearIsmInstances", "McpLink Clear Mesh Instances"), ShouldTransact(Component));
					Component->Modify();
					const int32 Removed = Component->GetInstanceCount();
					Component->ClearInstances();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("removed"), Removed);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, get_info, add_instances, ")
						TEXT("update_instance, or clear"),
						*Operation));
			});
	}
}
