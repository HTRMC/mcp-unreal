// Editor workflow that is not a property edit: undo/redo, selection, outliner
// folders, actor attachment, per-instance components, duplication and snapping.
//
// These are the operations a person performs with the mouse. Everything routes
// through the same editor subsystems the UI uses, so the results are what the
// editor would show and each one lands in the transaction buffer.

#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "ScopedTransaction.h"
#include "Subsystems/EditorActorSubsystem.h"

namespace McpLink
{
	namespace
	{
		/// Actors named by a "actors" array or a single "actor" string.
		bool CollectActors(
			UWorld* World,
			const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder,
			TArray<AActor*>& OutActors)
		{
			TArray<FString> Specs;
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (Body->TryGetArrayField(TEXT("actors"), Array) && Array != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Array)
				{
					FString Spec;
					if (Value.IsValid() && Value->TryGetString(Spec) && !Spec.IsEmpty())
					{
						Specs.Add(Spec);
					}
				}
			}
			FString Single;
			if (Body->TryGetStringField(TEXT("actor"), Single) && !Single.IsEmpty())
			{
				Specs.AddUnique(Single);
			}
			if (Specs.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'actor' (or 'actors') is required — an object path or editor label"));
				return false;
			}
			for (const FString& Spec : Specs)
			{
				AActor* Actor = ResolveActor(World, Spec);
				if (Actor == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
						FString::Printf(
							TEXT("no actor '%s' — get_level_actors lists them"), *Spec));
					return false;
				}
				OutActors.Add(Actor);
			}
			return true;
		}

		TSharedRef<FJsonObject> ActorSummary(const AActor* Actor)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Object->SetStringField(TEXT("path"), Actor->GetPathName());
			Object->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
			Object->SetArrayField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
			if (const AActor* Parent = Actor->GetAttachParentActor())
			{
				Object->SetStringField(TEXT("attached_to"), Parent->GetActorLabel());
			}
			const FName Folder = Actor->GetFolderPath();
			if (!Folder.IsNone())
			{
				Object->SetStringField(TEXT("folder"), Folder.ToString());
			}
			return Object;
		}

		UActorComponent* FindComponent(AActor* Actor, const FString& Name)
		{
			for (UActorComponent* Component : Actor->GetComponents())
			{
				if (Component != nullptr
					&& (Component->GetName() == Name || Component->GetPathName() == Name))
				{
					return Component;
				}
			}
			return nullptr;
		}

		/// Drop an actor onto whatever has collision beneath it, resting its
		/// bounds on the surface. Returns false when nothing was under it.
		bool SnapToFloor(AActor* Actor, double TraceDistance)
		{
			UWorld* World = Actor->GetWorld();
			if (World == nullptr)
			{
				return false;
			}
			FVector Origin, Extent;
			Actor->GetActorBounds(/*bOnlyCollidingComponents*/ false, Origin, Extent);
			const FVector Start = Actor->GetActorLocation();
			const FVector End = Start - FVector(0.0, 0.0, TraceDistance);

			FCollisionQueryParams Params(SCENE_QUERY_STAT(McpSnapToFloor), /*bTraceComplex*/ true);
			Params.AddIgnoredActor(Actor);
			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params))
			{
				return false;
			}
			// The pivot is not necessarily the bottom of the actor, so offset by
			// however far the bounds hang below it.
			const double BottomOffset = Start.Z - (Origin.Z - Extent.Z);
			Actor->SetActorLocation(Hit.Location + FVector(0.0, 0.0, BottomOffset));
			return true;
		}
	}

	void RegisterEditorOpsRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/editor/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				// ---- transaction buffer ----
				if (Operation == TEXT("undo") || Operation == TEXT("redo"))
				{
					if (GEditor == nullptr || GEditor->Trans == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_transactor"),
							TEXT("the editor has no transaction buffer"));
						return;
					}
					const bool bUndo = Operation == TEXT("undo");
					// CanUndo's out-parameter is the reason it *cannot*, not the
					// transaction's name; the name comes from the context.
					FText Reason;
					const bool bCan = bUndo
						? GEditor->Trans->CanUndo(&Reason)
						: GEditor->Trans->CanRedo(&Reason);
					if (!bCan)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("nothing_to_do"),
							Reason.IsEmpty()
								? (bUndo ? TEXT("nothing left to undo") : TEXT("nothing to redo"))
								: Reason.ToString());
						return;
					}
					const FString Name = bUndo
						? GEditor->Trans->GetUndoContext().Title.ToString()
						: GEditor->Trans->GetRedoContext().Title.ToString();
					const bool bDone = bUndo
						? GEditor->UndoTransaction()
						: GEditor->RedoTransaction();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(bUndo ? TEXT("undone") : TEXT("redone"), bDone);
					Data->SetStringField(TEXT("transaction"), Name);
					Data->SetNumberField(TEXT("queue_length"), GEditor->Trans->GetQueueLength());
					Data->SetNumberField(TEXT("undo_count"), GEditor->Trans->GetUndoCount());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("get_history"))
				{
					if (GEditor == nullptr || GEditor->Trans == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_transactor"),
							TEXT("the editor has no transaction buffer"));
						return;
					}
					const bool bCanUndo = GEditor->Trans->CanUndo();
					const bool bCanRedo = GEditor->Trans->CanRedo();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("can_undo"), bCanUndo);
					Data->SetBoolField(TEXT("can_redo"), bCanRedo);
					// The names come from the transaction contexts; CanUndo's
					// out-parameter carries the refusal reason instead.
					Data->SetStringField(TEXT("next_undo"),
						bCanUndo ? GEditor->Trans->GetUndoContext().Title.ToString() : FString());
					Data->SetStringField(TEXT("next_redo"),
						bCanRedo ? GEditor->Trans->GetRedoContext().Title.ToString() : FString());
					Data->SetNumberField(TEXT("queue_length"), GEditor->Trans->GetQueueLength());
					Data->SetNumberField(TEXT("undo_count"), GEditor->Trans->GetUndoCount());
					Responder->Ok(Data);
					return;
				}

				UEditorActorSubsystem* Actors =
					GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;

				// ---- selection ----
				if (Operation == TEXT("get_selection"))
				{
					if (Actors == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_editor"),
							TEXT("the editor actor subsystem is unavailable"));
						return;
					}
					TArray<TSharedPtr<FJsonValue>> Selected;
					for (const AActor* Actor : Actors->GetSelectedLevelActors())
					{
						if (Actor != nullptr)
						{
							Selected.Add(MakeShared<FJsonValueObject>(ActorSummary(Actor)));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("count"), Selected.Num());
					Data->SetArrayField(TEXT("actors"), Selected);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("clear_selection"))
				{
					if (Actors == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_editor"),
							TEXT("the editor actor subsystem is unavailable"));
						return;
					}
					Actors->SelectNothing();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("count"), 0);
					Responder->Ok(Data);
					return;
				}

				UWorld* World = ResolveWorldOrError(Body, Responder);
				if (!World) { return; }

				if (Operation == TEXT("select"))
				{
					if (Actors == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_editor"),
							TEXT("the editor actor subsystem is unavailable"));
						return;
					}
					TArray<AActor*> Targets;
					if (!CollectActors(World, Body, Responder, Targets))
					{
						return;
					}
					Actors->SetSelectedLevelActors(Targets);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("count"), Targets.Num());
					Responder->Ok(Data);
					return;
				}

				// ---- attachment ----
				if (Operation == TEXT("attach"))
				{
					FString ParentSpec;
					if (!Body->TryGetStringField(TEXT("parent"), ParentSpec) || ParentSpec.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'parent' is required — the actor to attach to"));
						return;
					}
					AActor* Parent = ResolveActor(World, ParentSpec);
					if (Parent == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
							FString::Printf(TEXT("no actor '%s'"), *ParentSpec));
						return;
					}
					TArray<AActor*> Targets;
					if (!CollectActors(World, Body, Responder, Targets))
					{
						return;
					}
					FString Socket;
					Body->TryGetStringField(TEXT("socket"), Socket);
					// Keeping world transform is what the editor's drag-in-outliner
					// does, so it is the default here too.
					const bool bKeepWorld = BoolOr(Body, TEXT("keep_world_transform"), true);
					const FAttachmentTransformRules Rules = bKeepWorld
						? FAttachmentTransformRules::KeepWorldTransform
						: FAttachmentTransformRules::KeepRelativeTransform;

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AttachActors", "McpLink Attach Actors"),
						ShouldTransact(World));
					TArray<TSharedPtr<FJsonValue>> Results;
					for (AActor* Actor : Targets)
					{
						if (Actor == Parent || Parent->IsAttachedTo(Actor))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest,
								TEXT("circular_attachment"),
								FString::Printf(
									TEXT("attaching '%s' to '%s' would make a cycle"),
									*Actor->GetActorLabel(), *Parent->GetActorLabel()));
							return;
						}
						Actor->Modify();
						Actor->AttachToActor(Parent, Rules, FName(*Socket));
						Results.Add(MakeShared<FJsonValueObject>(ActorSummary(Actor)));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("parent"), Parent->GetActorLabel());
					Data->SetArrayField(TEXT("actors"), Results);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("detach"))
				{
					TArray<AActor*> Targets;
					if (!CollectActors(World, Body, Responder, Targets))
					{
						return;
					}
					const bool bKeepWorld = BoolOr(Body, TEXT("keep_world_transform"), true);
					const FDetachmentTransformRules Rules = bKeepWorld
						? FDetachmentTransformRules::KeepWorldTransform
						: FDetachmentTransformRules::KeepRelativeTransform;
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "DetachActors", "McpLink Detach Actors"),
						ShouldTransact(World));
					TArray<TSharedPtr<FJsonValue>> Results;
					for (AActor* Actor : Targets)
					{
						Actor->Modify();
						Actor->DetachFromActor(Rules);
						Results.Add(MakeShared<FJsonValueObject>(ActorSummary(Actor)));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("actors"), Results);
					Responder->Ok(Data);
					return;
				}

				// ---- per-instance components ----
				if (Operation == TEXT("add_component"))
				{
					TArray<AActor*> Targets;
					if (!CollectActors(World, Body, Responder, Targets) || Targets.IsEmpty())
					{
						return;
					}
					AActor* Actor = Targets[0];
					FString ClassSpec, Name;
					if (!Body->TryGetStringField(TEXT("class"), ClassSpec) || ClassSpec.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'class' is required, e.g. StaticMeshComponent, PointLightComponent"));
						return;
					}
					Body->TryGetStringField(TEXT("name"), Name);
					UClass* ComponentClass = ResolveClass(ClassSpec);
					if (ComponentClass == nullptr
						|| !ComponentClass->IsChildOf(UActorComponent::StaticClass())
						|| ComponentClass->HasAnyClassFlags(CLASS_Abstract))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_class"),
							FString::Printf(
								TEXT("'%s' is not a concrete ActorComponent subclass"), *ClassSpec));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddComponent", "McpLink Add Component"),
						ShouldTransact(Actor));
					Actor->Modify();
					// MakeUniqueObjectName always appends a suffix, so only reach
					// for it when the requested name is actually taken.
					FName ComponentName = NAME_None;
					if (!Name.IsEmpty())
					{
						ComponentName = FName(*Name);
						if (StaticFindObjectFast(UObject::StaticClass(), Actor, ComponentName) != nullptr)
						{
							ComponentName = MakeUniqueObjectName(Actor, ComponentClass, ComponentName);
						}
					}
					UActorComponent* Component = NewObject<UActorComponent>(
						Actor, ComponentClass, ComponentName, RF_Transactional);
					// AddInstanceComponent is what makes it show in the details
					// panel and survive a save; RegisterComponent makes it live.
					Actor->AddInstanceComponent(Component);
					if (USceneComponent* Scene = Cast<USceneComponent>(Component))
					{
						USceneComponent* AttachTo = Actor->GetRootComponent();
						FString ParentName;
						if (Body->TryGetStringField(TEXT("attach_to"), ParentName)
							&& !ParentName.IsEmpty())
						{
							AttachTo = Cast<USceneComponent>(FindComponent(Actor, ParentName));
							if (AttachTo == nullptr)
							{
								Responder->Error(EHttpServerResponseCodes::NotFound,
									TEXT("component_not_found"),
									FString::Printf(
										TEXT("no scene component '%s' on %s"),
										*ParentName, *Actor->GetActorLabel()));
								return;
							}
						}
						if (AttachTo != nullptr)
						{
							Scene->AttachToComponent(
								AttachTo, FAttachmentTransformRules::KeepRelativeTransform);
						}
						else
						{
							Actor->SetRootComponent(Scene);
						}
					}
					Component->RegisterComponent();
					Actor->RerunConstructionScripts();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("component"), Component->GetName());
					Data->SetStringField(TEXT("path"), Component->GetPathName());
					Data->SetStringField(TEXT("class"), ComponentClass->GetPathName());
					Data->SetStringField(TEXT("note"),
						TEXT("this component belongs to the actor instance — to give every instance ")
						TEXT("one, use blueprint_modify add_component on its Blueprint"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_component"))
				{
					TArray<AActor*> Targets;
					if (!CollectActors(World, Body, Responder, Targets) || Targets.IsEmpty())
					{
						return;
					}
					AActor* Actor = Targets[0];
					FString Name;
					if (!Body->TryGetStringField(TEXT("component"), Name) || Name.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'component' is required — get_actor_components lists them"));
						return;
					}
					UActorComponent* Component = FindComponent(Actor, Name);
					if (Component == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("component_not_found"),
							FString::Printf(
								TEXT("no component '%s' on %s"), *Name, *Actor->GetActorLabel()));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "RemoveComponent", "McpLink Remove Component"),
						ShouldTransact(Actor));
					Actor->Modify();
					Actor->RemoveInstanceComponent(Component);
					Component->DestroyComponent();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), Name);
					Responder->Ok(Data);
					return;
				}

				// ---- duplication, folders, labels, snapping ----
				if (Operation == TEXT("duplicate"))
				{
					if (Actors == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_editor"),
							TEXT("the editor actor subsystem is unavailable"));
						return;
					}
					TArray<AActor*> Targets;
					if (!CollectActors(World, Body, Responder, Targets))
					{
						return;
					}
					FVector Offset = FVector::ZeroVector;
					GetVector(Body, TEXT("offset"), Offset);
					const int32 Count = FMath::Clamp(IntOr(Body, TEXT("count"), 1), 1, 256);

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "DuplicateActors", "McpLink Duplicate Actors"),
						ShouldTransact(World));
					TArray<TSharedPtr<FJsonValue>> Results;
					for (int32 Index = 1; Index <= Count; ++Index)
					{
						for (AActor* Actor : Actors->DuplicateActors(Targets, World, Offset * Index))
						{
							if (Actor != nullptr)
							{
								Results.Add(MakeShared<FJsonValueObject>(ActorSummary(Actor)));
							}
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("created"), Results.Num());
					Data->SetArrayField(TEXT("actors"), Results);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_folder"))
				{
					TArray<AActor*> Targets;
					if (!CollectActors(World, Body, Responder, Targets))
					{
						return;
					}
					FString Folder;
					Body->TryGetStringField(TEXT("folder"), Folder);
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetFolder", "McpLink Set Outliner Folder"),
						ShouldTransact(World));
					TArray<TSharedPtr<FJsonValue>> Results;
					for (AActor* Actor : Targets)
					{
						Actor->Modify();
						Actor->SetFolderPath(FName(*Folder));
						Results.Add(MakeShared<FJsonValueObject>(ActorSummary(Actor)));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("folder"), Folder);
					Data->SetArrayField(TEXT("actors"), Results);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_label"))
				{
					TArray<AActor*> Targets;
					if (!CollectActors(World, Body, Responder, Targets) || Targets.IsEmpty())
					{
						return;
					}
					FString Label;
					if (!Body->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'label' is required"));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetLabel", "McpLink Rename Actor"),
						ShouldTransact(Targets[0]));
					Targets[0]->Modify();
					Targets[0]->SetActorLabel(Label);
					Responder->Ok(ActorSummary(Targets[0]));
					return;
				}

				if (Operation == TEXT("snap_to_floor"))
				{
					TArray<AActor*> Targets;
					if (!CollectActors(World, Body, Responder, Targets))
					{
						return;
					}
					const double Distance = DoubleOr(Body, TEXT("trace_distance"), 100000.0);
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SnapToFloor", "McpLink Snap Actors To Floor"),
						ShouldTransact(World));
					TArray<TSharedPtr<FJsonValue>> Snapped;
					TArray<TSharedPtr<FJsonValue>> Missed;
					for (AActor* Actor : Targets)
					{
						Actor->Modify();
						if (SnapToFloor(Actor, Distance))
						{
							Snapped.Add(MakeShared<FJsonValueObject>(ActorSummary(Actor)));
						}
						else
						{
							Missed.Add(MakeShared<FJsonValueString>(Actor->GetActorLabel()));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("snapped"), Snapped);
					// Nothing beneath them is the usual cause, and silently
					// leaving those where they were would be confusing.
					Data->SetArrayField(TEXT("nothing_below"), Missed);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use undo, redo, get_history, get_selection, ")
						TEXT("select, clear_selection, attach, detach, add_component, ")
						TEXT("remove_component, duplicate, set_folder, set_label or snap_to_floor"),
						*Operation));
			});
	}
}
