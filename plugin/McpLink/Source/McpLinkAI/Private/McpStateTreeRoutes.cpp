// State Trees: the hierarchical state machine that is replacing Behavior Trees
// for a lot of AI and gameplay logic.
//
// Unlike a Behavior Tree there is no EdGraph — the editor draws UStateTreeState
// objects directly, so authoring is just building that tree and compiling it.
// A tree that is never compiled has empty runtime data and does nothing, which
// is why `compile` reports its log rather than a bare bool.
//
// Tasks, conditions and evaluators are instanced structs (FStateTreeEditorNode
// wrapping an FStateTreeNodeBase), added by struct name from list_node_structs.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "StateTree.h"
#include "StateTreeCompiler.h"
#include "Logging/TokenizedMessage.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeConditionBase.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTaskBase.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace StateTrees
	{
		/// The three node families a state can hold, and the base struct each
		/// one derives from.
		UScriptStruct* BaseStructFor(const FString& Kind)
		{
			if (Kind.Equals(TEXT("task"), ESearchCase::IgnoreCase))
			{
				return FStateTreeTaskBase::StaticStruct();
			}
			if (Kind.Equals(TEXT("condition"), ESearchCase::IgnoreCase))
			{
				return FStateTreeConditionBase::StaticStruct();
			}
			if (Kind.Equals(TEXT("evaluator"), ESearchCase::IgnoreCase))
			{
				return FStateTreeEvaluatorBase::StaticStruct();
			}
			return nullptr;
		}

		UStateTree* StateTreeOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("state_tree"), Path, Responder,
					TEXT("a State Tree asset path, e.g. /Game/AI/ST_Guard")))
			{
				return nullptr;
			}
			UStateTree* Tree = Cast<UStateTree>(ResolveAsset(Path));
			if (Tree == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("tree_not_found"),
					FString::Printf(TEXT("no State Tree asset at '%s'"), *Path));
			}
			return Tree;
		}

		UStateTreeEditorData* EditorDataOf(UStateTree* Tree)
		{
			return Cast<UStateTreeEditorData>(Tree->EditorData);
		}

		/// Depth-first walk of the tree's states, so a name or id resolves
		/// wherever it sits in the hierarchy.
		UStateTreeState* FindState(UStateTreeState* State, const FString& Spec)
		{
			if (State == nullptr)
			{
				return nullptr;
			}
			if (State->Name.ToString().Equals(Spec, ESearchCase::IgnoreCase)
				|| State->ID.ToString() == Spec)
			{
				return State;
			}
			for (UStateTreeState* Child : State->Children)
			{
				if (UStateTreeState* Found = FindState(Child, Spec))
				{
					return Found;
				}
			}
			return nullptr;
		}

		UStateTreeState* FindState(UStateTreeEditorData* EditorData, const FString& Spec)
		{
			for (UStateTreeState* Root : EditorData->SubTrees)
			{
				if (UStateTreeState* Found = FindState(Root, Spec))
				{
					return Found;
				}
			}
			return nullptr;
		}

		FString NodeStructName(const FStateTreeEditorNode& Node)
		{
			const UScriptStruct* Struct = Node.Node.GetScriptStruct();
			return Struct != nullptr ? Struct->GetName() : FString();
		}

		TSharedRef<FJsonObject> StateToJson(UStateTreeState* State)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), State->Name.ToString());
			Object->SetStringField(TEXT("id"), State->ID.ToString());
			if (const UEnum* TypeEnum = StaticEnum<EStateTreeStateType>())
			{
				Object->SetStringField(TEXT("type"),
					TypeEnum->GetNameStringByValue(static_cast<int64>(State->Type)));
			}
			// The state object itself is a UObject, so set_property reaches its
			// plain properties on this path.
			Object->SetStringField(TEXT("path"), State->GetPathName());

			const auto Nodes = [](const TArray<FStateTreeEditorNode>& In)
			{
				TArray<TSharedPtr<FJsonValue>> Out;
				for (const FStateTreeEditorNode& Node : In)
				{
					Out.Add(MakeShared<FJsonValueString>(NodeStructName(Node)));
				}
				return Out;
			};
			Object->SetArrayField(TEXT("tasks"), Nodes(State->Tasks));
			Object->SetArrayField(TEXT("enter_conditions"), Nodes(State->EnterConditions));

			TArray<TSharedPtr<FJsonValue>> Transitions;
			for (const FStateTreeTransition& Transition : State->Transitions)
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				if (const UEnum* TriggerEnum = StaticEnum<EStateTreeTransitionTrigger>())
				{
					Item->SetStringField(TEXT("trigger"),
						TriggerEnum->GetNameStringByValue(static_cast<int64>(Transition.Trigger)));
				}
				if (const UEnum* TypeEnum = StaticEnum<EStateTreeTransitionType>())
				{
					Item->SetStringField(TEXT("type"),
						TypeEnum->GetNameStringByValue(static_cast<int64>(Transition.State.LinkType)));
				}
				Item->SetStringField(TEXT("target"), Transition.State.Name.ToString());
				Transitions.Add(MakeShared<FJsonValueObject>(Item));
			}
			Object->SetArrayField(TEXT("transitions"), Transitions);

			TArray<TSharedPtr<FJsonValue>> Children;
			for (UStateTreeState* Child : State->Children)
			{
				if (Child != nullptr)
				{
					Children.Add(MakeShared<FJsonValueObject>(StateToJson(Child)));
				}
			}
			Object->SetArrayField(TEXT("children"), Children);
			return Object;
		}
	}

	using namespace StateTrees;

	void RegisterStateTreeRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/ai/statetree"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_node_structs") || Operation == TEXT("list_schemas"))
				{
					FString Contains;
					Body->TryGetStringField(TEXT("name_contains"), Contains);
					const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_results"), 200), 1, 2000);
					TArray<TSharedPtr<FJsonValue>> Results;
					int32 Matched = 0;

					if (Operation == TEXT("list_schemas"))
					{
						for (TObjectIterator<UClass> It; It; ++It)
						{
							UClass* Class = *It;
							if (!Class->IsChildOf(UStateTreeSchema::StaticClass())
								|| Class == UStateTreeSchema::StaticClass()
								|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
							{
								continue;
							}
							const FString Name = Class->GetName();
							if (!Contains.IsEmpty() && !Name.Contains(Contains))
							{
								continue;
							}
							++Matched;
							if (Results.Num() < Max)
							{
								Results.Add(MakeShared<FJsonValueString>(Name));
							}
						}
					}
					else
					{
						FString Kind;
						Body->TryGetStringField(TEXT("kind"), Kind);
						for (TObjectIterator<UScriptStruct> It; It; ++It)
						{
							UScriptStruct* Struct = *It;
							const bool bTask = Struct->IsChildOf(FStateTreeTaskBase::StaticStruct());
							const bool bCondition =
								Struct->IsChildOf(FStateTreeConditionBase::StaticStruct());
							const bool bEvaluator =
								Struct->IsChildOf(FStateTreeEvaluatorBase::StaticStruct());
							if (!bTask && !bCondition && !bEvaluator)
							{
								continue;
							}
							// The bases themselves are not addable.
							if (Struct == FStateTreeTaskBase::StaticStruct()
								|| Struct == FStateTreeConditionBase::StaticStruct()
								|| Struct == FStateTreeEvaluatorBase::StaticStruct()
								|| Struct->GetName().EndsWith(TEXT("Base")))
							{
								continue;
							}
							const FString ThisKind = bTask ? TEXT("task")
								: bCondition			   ? TEXT("condition")
														   : TEXT("evaluator");
							if (!Kind.IsEmpty() && !Kind.Equals(ThisKind, ESearchCase::IgnoreCase))
							{
								continue;
							}
							const FString Name = Struct->GetName();
							if (!Contains.IsEmpty() && !Name.Contains(Contains))
							{
								continue;
							}
							++Matched;
							if (Results.Num() >= Max)
							{
								continue;
							}
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("struct"), Name);
							Item->SetStringField(TEXT("kind"), ThisKind);
							Results.Add(MakeShared<FJsonValueObject>(Item));
						}
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Matched);
					Data->SetArrayField(
						Operation == TEXT("list_schemas") ? TEXT("schemas") : TEXT("structs"), Results);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create"))
				{
					FString Path, SchemaName;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/AI/ST_Guard")))
					{
						return;
					}
					Body->TryGetStringField(TEXT("schema"), SchemaName);
					if (SchemaName.IsEmpty())
					{
						// Without a schema the editor cannot decide which tasks
						// are legal, and compiling fails with no explanation.
						SchemaName = TEXT("StateTreeComponentSchema");
					}
					UClass* SchemaClass = ResolveClass(SchemaName);
					if (SchemaClass == nullptr || !SchemaClass->IsChildOf(UStateTreeSchema::StaticClass()))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_schema"),
							FString::Printf(
								TEXT("'%s' is not a UStateTreeSchema — see state_tree_ops list_schemas"),
								*SchemaName));
						return;
					}
					if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr, *Path) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateStateTree", "McpLink Create State Tree"));
					UPackage* Package = CreatePackage(*Path);
					UStateTree* Tree = NewObject<UStateTree>(Package,
						FName(*FPackageName::GetShortName(Path)),
						RF_Public | RF_Standalone | RF_Transactional);
					UStateTreeEditorData* EditorData = NewObject<UStateTreeEditorData>(
						Tree, FName(), RF_Transactional);
					EditorData->Schema = NewObject<UStateTreeSchema>(EditorData, SchemaClass);
					Tree->EditorData = EditorData;
					// Every tree needs one root state or there is nothing to
					// enter, and the compiler rejects it.
					EditorData->AddRootState();
					FAssetRegistryModule::AssetCreated(Tree);
					Package->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("state_tree"), Tree->GetPathName());
					Data->SetStringField(TEXT("schema"), SchemaClass->GetName());
					Data->SetStringField(TEXT("message"),
						TEXT("created with a root state — add states and tasks, then compile and save"));
					Responder->Ok(Data);
					return;
				}

				// ---- everything below targets an existing State Tree --------
				UStateTree* Tree = StateTreeOrError(Body, Responder);
				if (Tree == nullptr)
				{
					return;
				}
				UStateTreeEditorData* EditorData = EditorDataOf(Tree);
				if (EditorData == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_editor_data"),
						FString::Printf(
							TEXT("'%s' has no editor data — it was probably cooked, or created outside ")
							TEXT("the editor"),
							*Tree->GetPathName()));
					return;
				}

				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("state_tree"), Tree->GetPathName());
					Data->SetStringField(TEXT("schema"),
						EditorData->Schema != nullptr ? EditorData->Schema->GetClass()->GetName()
													  : FString());
					Data->SetBoolField(TEXT("compiled"), Tree->IsReadyToRun());
					TArray<TSharedPtr<FJsonValue>> Roots;
					for (UStateTreeState* Root : EditorData->SubTrees)
					{
						if (Root != nullptr)
						{
							Roots.Add(MakeShared<FJsonValueObject>(StateToJson(Root)));
						}
					}
					Data->SetArrayField(TEXT("states"), Roots);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("compile"))
				{
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CompileStateTree", "McpLink Compile State Tree"));
					Tree->Modify();
					FStateTreeCompilerLog Log;
					FStateTreeCompiler Compiler(Log);
					const bool bCompiled = Compiler.Compile(*Tree);

					// The log's own message array is protected; the tokenized
					// form is the exported way to read what it recorded.
					TArray<TSharedPtr<FJsonValue>> Messages;
					for (const TSharedRef<FTokenizedMessage>& Message : Log.ToTokenizedMessages())
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("severity"),
							Message->GetSeverity() == EMessageSeverity::Error ? TEXT("error")
																			  : TEXT("warning"));
						Item->SetStringField(TEXT("message"), Message->ToText().ToString());
						Messages.Add(MakeShared<FJsonValueObject>(Item));
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("state_tree"), Tree->GetPathName());
					Data->SetBoolField(TEXT("compiled"), bCompiled);
					Data->SetBoolField(TEXT("ready_to_run"), Tree->IsReadyToRun());
					Data->SetArrayField(TEXT("messages"), Messages);
					if (!bCompiled)
					{
						Data->SetStringField(TEXT("hint"),
							TEXT("an uncompiled State Tree has empty runtime data and does nothing — ")
							TEXT("fix the messages above and compile again"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Tree, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("state_tree"), Tree->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditStateTree", "McpLink Edit State Tree"));
				Tree->Modify();
				EditorData->Modify();

				if (Operation == TEXT("add_state"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the new state's name")))
					{
						return;
					}
					FString TypeName = TEXT("State");
					Body->TryGetStringField(TEXT("state_type"), TypeName);
					const UEnum* TypeEnum = StaticEnum<EStateTreeStateType>();
					const int64 TypeValue =
						TypeEnum != nullptr ? TypeEnum->GetValueByNameString(TypeName) : INDEX_NONE;
					if (TypeValue == INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_state_type"),
							TEXT("'state_type' must be State, Group, Linked, LinkedAsset or Subtree"));
						return;
					}

					FString ParentSpec;
					UStateTreeState* Parent = nullptr;
					if (Body->TryGetStringField(TEXT("parent"), ParentSpec) && !ParentSpec.IsEmpty())
					{
						Parent = FindState(EditorData, ParentSpec);
						if (Parent == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("state_not_found"),
								FString::Printf(TEXT("no state '%s' in this tree"), *ParentSpec));
							return;
						}
					}
					else if (!EditorData->SubTrees.IsEmpty())
					{
						Parent = EditorData->SubTrees[0];
					}
					if (Parent == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_root"),
							TEXT("this tree has no root state to attach to"));
						return;
					}
					Parent->Modify();
					UStateTreeState& Added = Parent->AddChildState(
						FName(*Name), static_cast<EStateTreeStateType>(TypeValue));
					Responder->Ok(StateToJson(&Added));
					return;
				}

				// ---- everything below names an existing state ---------------
				FString StateSpec;
				if (!RequireString(Body, TEXT("state"), StateSpec, Responder,
						TEXT("a state name or id from `info`")))
				{
					return;
				}
				UStateTreeState* State = FindState(EditorData, StateSpec);
				if (State == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("state_not_found"),
						FString::Printf(TEXT("no state '%s' in this tree — see `info`"), *StateSpec));
					return;
				}
				State->Modify();

				if (Operation == TEXT("rename_state"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the new name")))
					{
						return;
					}
					State->Name = FName(*Name);
					Responder->Ok(StateToJson(State));
					return;
				}

				if (Operation == TEXT("remove_state"))
				{
					UStateTreeState* Parent = State->Parent;
					if (Parent == nullptr)
					{
						const int32 Removed = EditorData->SubTrees.Remove(State);
						if (Removed == 0)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("remove_failed"),
								TEXT("that state is neither a root nor a child of one"));
							return;
						}
					}
					else
					{
						Parent->Modify();
						Parent->Children.Remove(State);
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), StateSpec);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_transition"))
				{
					FString TriggerName = TEXT("OnStateCompleted");
					FString TypeName = TEXT("GotoState");
					FString TargetSpec;
					Body->TryGetStringField(TEXT("trigger"), TriggerName);
					Body->TryGetStringField(TEXT("transition_type"), TypeName);
					Body->TryGetStringField(TEXT("target"), TargetSpec);

					const UEnum* TriggerEnum = StaticEnum<EStateTreeTransitionTrigger>();
					const UEnum* TypeEnum = StaticEnum<EStateTreeTransitionType>();
					const int64 Trigger =
						TriggerEnum != nullptr ? TriggerEnum->GetValueByNameString(TriggerName) : INDEX_NONE;
					const int64 Type =
						TypeEnum != nullptr ? TypeEnum->GetValueByNameString(TypeName) : INDEX_NONE;
					if (Trigger == INDEX_NONE || Type == INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_transition"),
							TEXT("'trigger' must be an EStateTreeTransitionTrigger name (OnStateCompleted, ")
							TEXT("OnStateSucceeded, OnStateFailed, OnTick, OnEvent) and 'transition_type' ")
							TEXT("an EStateTreeTransitionType name (GotoState, NextState, Succeeded, ")
							TEXT("Failed, None)"));
						return;
					}

					UStateTreeState* Target = nullptr;
					if (!TargetSpec.IsEmpty())
					{
						Target = FindState(EditorData, TargetSpec);
						if (Target == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("state_not_found"),
								FString::Printf(TEXT("no target state '%s'"), *TargetSpec));
							return;
						}
					}
					if (Type == static_cast<int64>(EStateTreeTransitionType::GotoState)
						&& Target == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'target' is required for a GotoState transition"));
						return;
					}
					State->AddTransition(static_cast<EStateTreeTransitionTrigger>(Trigger),
						static_cast<EStateTreeTransitionType>(Type), Target);
					Responder->Ok(StateToJson(State));
					return;
				}

				if (Operation == TEXT("add_node") || Operation == TEXT("remove_node"))
				{
					FString Kind;
					if (!RequireString(Body, TEXT("kind"), Kind, Responder,
							TEXT("\"task\", \"condition\" or \"evaluator\"")))
					{
						return;
					}
					UScriptStruct* Base = BaseStructFor(Kind);
					if (Base == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_kind"),
							TEXT("'kind' must be \"task\", \"condition\" or \"evaluator\""));
						return;
					}
					// Evaluators live on the tree, not on a state.
					TArray<FStateTreeEditorNode>* Target =
						Kind.Equals(TEXT("task"), ESearchCase::IgnoreCase) ? &State->Tasks
																		   : &State->EnterConditions;
					if (Kind.Equals(TEXT("evaluator"), ESearchCase::IgnoreCase))
					{
						Target = &EditorData->Evaluators;
					}

					if (Operation == TEXT("remove_node"))
					{
						const int32 Index = IntOr(Body, TEXT("index"), -1);
						if (!Target->IsValidIndex(Index))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_index"),
								FString::Printf(TEXT("'index' must be 0..%d"), Target->Num() - 1));
							return;
						}
						const FString Removed = NodeStructName((*Target)[Index]);
						Target->RemoveAt(Index);
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("removed"), Removed);
						Responder->Ok(Data);
						return;
					}

					FString StructName;
					if (!RequireString(Body, TEXT("struct"), StructName, Responder,
							TEXT("a node struct name from list_node_structs")))
					{
						return;
					}
					UScriptStruct* Struct = nullptr;
					for (TObjectIterator<UScriptStruct> It; It; ++It)
					{
						if (It->GetName() == StructName && It->IsChildOf(Base))
						{
							Struct = *It;
							break;
						}
					}
					if (Struct == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("struct_not_found"),
							FString::Printf(
								TEXT("no %s struct named '%s' — see state_tree_ops list_node_structs"),
								*Kind, *StructName));
						return;
					}
					FStateTreeEditorNode& Node = Target->AddDefaulted_GetRef();
					Node.InitializeAs(EditorData, Struct);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("state"), State->Name.ToString());
					Data->SetStringField(TEXT("kind"), Kind);
					Data->SetStringField(TEXT("struct"), StructName);
					Data->SetNumberField(TEXT("index"), Target->Num() - 1);
					Data->SetStringField(TEXT("message"),
						TEXT("added with its defaults — compile to check the schema allows it here"));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_schemas, list_node_structs, create, ")
						TEXT("info, add_state, rename_state, remove_state, add_transition, add_node, ")
						TEXT("remove_node, compile, or save"),
						*Operation));
			});
	}
}
