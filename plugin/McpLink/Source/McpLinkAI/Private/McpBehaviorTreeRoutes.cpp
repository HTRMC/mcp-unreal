// Behavior Tree authoring: build the editor graph, and let UBehaviorTreeGraph
// compile it into the runtime tree.
//
// A Behavior Tree asset carries two representations: BTGraph (what the editor
// shows and what survives a save) and RootNode (what the runtime executes,
// regenerated from the graph by UpdateAsset). Authoring the runtime tree
// directly would be silently undone the next time the asset is opened, so
// everything here goes through the graph, exactly like the editor.
//
// The engine-facing half lives in McpAiUtils.cpp; this file is request
// parsing, JSON and error messages.

#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTreeFactory.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "McpAiUtils.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace
	{
		UObject* LoadByPath(const FString& Path)
		{
			if (UObject* Direct = ResolveObject(Path))
			{
				return Direct;
			}
			if (!Path.Contains(TEXT(".")))
			{
				return ResolveObject(
					FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path)));
			}
			return nullptr;
		}

		UBehaviorTree* TreeOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("tree"), Path, Responder,
				TEXT("a Behavior Tree asset path, e.g. /Game/AI/BT_Guard")))
			{
				return nullptr;
			}
			UBehaviorTree* Tree = Cast<UBehaviorTree>(LoadByPath(Path));
			if (Tree == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("tree_not_found"),
					FString::Printf(TEXT("no Behavior Tree asset at '%s'"), *Path));
			}
			return Tree;
		}

		const TCHAR* KindOf(const UEdGraphNode* Node)
		{
			if (Node->IsA<UBehaviorTreeGraphNode_Root>()) { return TEXT("root"); }
			if (Node->IsA<UBehaviorTreeGraphNode_Decorator>()) { return TEXT("decorator"); }
			if (Node->IsA<UBehaviorTreeGraphNode_Service>()) { return TEXT("service"); }
			if (Node->IsA<UBehaviorTreeGraphNode_Composite>()) { return TEXT("composite"); }
			if (Node->IsA<UBehaviorTreeGraphNode_Task>()) { return TEXT("task"); }
			return TEXT("node");
		}

		TSharedRef<FJsonObject> NodeToJson(const UBehaviorTreeGraphNode* Node)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("id"), Node->NodeGuid.ToString());
			Object->SetStringField(TEXT("kind"), KindOf(Node));
			Object->SetStringField(TEXT("class"),
				Node->NodeInstance ? Node->NodeInstance->GetClass()->GetPathName() : TEXT(""));
			Object->SetStringField(TEXT("title"),
				Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			// Node options are UPROPERTYs on the instance, so set_property on
			// this path is how a caller configures a task or decorator.
			if (Node->NodeInstance != nullptr)
			{
				Object->SetStringField(TEXT("instance"), Node->NodeInstance->GetPathName());
			}

			TArray<TSharedPtr<FJsonValue>> Children;
			if (const UEdGraphPin* Output = Node->GetOutputPin())
			{
				for (const UEdGraphPin* Linked : Output->LinkedTo)
				{
					if (Linked != nullptr && Linked->GetOwningNode() != nullptr)
					{
						Children.Add(MakeShared<FJsonValueString>(
							Linked->GetOwningNode()->NodeGuid.ToString()));
					}
				}
			}
			Object->SetArrayField(TEXT("children"), Children);

			TArray<TSharedPtr<FJsonValue>> SubNodes;
			for (const UAIGraphNode* Sub : Node->SubNodes)
			{
				if (Sub != nullptr)
				{
					SubNodes.Add(MakeShared<FJsonValueString>(Sub->NodeGuid.ToString()));
				}
			}
			Object->SetArrayField(TEXT("sub_nodes"), SubNodes);
			return Object;
		}

		/// Every concrete runtime node class of a kind, native or Blueprint.
		void CollectNodeClasses(
			UClass* Base, const TCHAR* Kind, const FString& NameFilter,
			TArray<TSharedPtr<FJsonValue>>& Out)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Class = *It;
				if (!Class->IsChildOf(Base) || Class == Base
					|| Class->HasAnyClassFlags(
						CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
				{
					continue;
				}
				if (!NameFilter.IsEmpty() && !Class->GetName().Contains(NameFilter))
				{
					continue;
				}
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("class"), Class->GetPathName());
				Item->SetStringField(TEXT("name"), Class->GetName());
				Item->SetStringField(TEXT("kind"), Kind);
				Out.Add(MakeShared<FJsonValueObject>(Item));
			}
		}
	}

	void RegisterBehaviorTreeRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/ai/behavior_tree"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				// ---- discovery needs no asset ----
				if (Operation == TEXT("list_node_classes"))
				{
					FString Kind, NameFilter;
					Body->TryGetStringField(TEXT("kind"), Kind);
					Body->TryGetStringField(TEXT("name_contains"), NameFilter);
					Kind = Kind.ToLower();

					TArray<TSharedPtr<FJsonValue>> Classes;
					if (Kind.IsEmpty() || Kind == TEXT("task"))
					{
						CollectNodeClasses(
							UBTTaskNode::StaticClass(), TEXT("task"), NameFilter, Classes);
					}
					if (Kind.IsEmpty() || Kind == TEXT("composite"))
					{
						CollectNodeClasses(
							UBTCompositeNode::StaticClass(), TEXT("composite"), NameFilter, Classes);
					}
					if (Kind.IsEmpty() || Kind == TEXT("decorator"))
					{
						CollectNodeClasses(
							UBTDecorator::StaticClass(), TEXT("decorator"), NameFilter, Classes);
					}
					if (Kind.IsEmpty() || Kind == TEXT("service"))
					{
						CollectNodeClasses(
							UBTService::StaticClass(), TEXT("service"), NameFilter, Classes);
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Classes.Num());
					Data->SetArrayField(TEXT("classes"), Classes);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
						TEXT("e.g. /Game/AI/BT_Guard")))
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateBehaviorTree", "McpLink Create Behavior Tree"));
					UBehaviorTreeFactory* Factory = NewObject<UBehaviorTreeFactory>();
					FString Error;
					UObject* Asset = CreateAsset(Path, UBehaviorTree::StaticClass(), Factory, Error);
					if (Asset == nullptr)
					{
						Responder->Error(
							EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					UBehaviorTree* Tree = CastChecked<UBehaviorTree>(Asset);
					FString BlackboardPath;
					if (Body->TryGetStringField(TEXT("blackboard"), BlackboardPath)
						&& !BlackboardPath.IsEmpty())
					{
						UBlackboardData* Blackboard =
							Cast<UBlackboardData>(LoadByPath(BlackboardPath));
						if (Blackboard == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound,
								TEXT("blackboard_not_found"),
								FString::Printf(
									TEXT("the tree was created, but no Blackboard asset exists at '%s'"),
									*BlackboardPath));
							return;
						}
						Tree->BlackboardAsset = Blackboard;
					}
					UBehaviorTreeGraph* Graph = Ai::EnsureGraph(Tree);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Tree->GetPathName());
					Data->SetStringField(TEXT("blackboard"),
						Tree->BlackboardAsset ? Tree->BlackboardAsset->GetPathName() : TEXT(""));
					if (UBehaviorTreeGraphNode* Root = Ai::FindRoot(Graph))
					{
						Data->SetStringField(TEXT("root"), Root->NodeGuid.ToString());
					}
					Responder->Ok(Data);
					return;
				}

				UBehaviorTree* Tree = TreeOrError(Body, Responder);
				if (!Tree) { return; }
				UBehaviorTreeGraph* Graph = Ai::EnsureGraph(Tree);
				if (Graph == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_graph"),
						TEXT("could not create the tree's editor graph"));
					return;
				}

				if (Operation == TEXT("get_tree"))
				{
					TArray<TSharedPtr<FJsonValue>> Nodes;
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						UBehaviorTreeGraphNode* BtNode = Cast<UBehaviorTreeGraphNode>(Node);
						if (BtNode == nullptr)
						{
							continue;
						}
						Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(BtNode)));
						for (UAIGraphNode* Sub : BtNode->SubNodes)
						{
							if (UBehaviorTreeGraphNode* SubBt = Cast<UBehaviorTreeGraphNode>(Sub))
							{
								const TSharedRef<FJsonObject> SubJson = NodeToJson(SubBt);
								SubJson->SetStringField(TEXT("parent"), BtNode->NodeGuid.ToString());
								Nodes.Add(MakeShared<FJsonValueObject>(SubJson));
							}
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Tree->GetPathName());
					Data->SetStringField(TEXT("blackboard"),
						Tree->BlackboardAsset ? Tree->BlackboardAsset->GetPathName() : TEXT(""));
					Data->SetBoolField(TEXT("compiled"), Tree->RootNode != nullptr);
					Data->SetArrayField(TEXT("nodes"), Nodes);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_blackboard"))
				{
					FString BlackboardPath;
					if (!RequireString(Body, TEXT("blackboard"), BlackboardPath, Responder))
					{
						return;
					}
					UBlackboardData* Blackboard = Cast<UBlackboardData>(LoadByPath(BlackboardPath));
					if (Blackboard == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound,
							TEXT("blackboard_not_found"),
							FString::Printf(TEXT("no Blackboard asset at '%s'"), *BlackboardPath));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetBlackboard", "McpLink Set Blackboard"));
					Tree->Modify();
					Tree->BlackboardAsset = Blackboard;
					// Refreshes every key selector on the tree's nodes.
					Graph->UpdateBlackboardChange();
					Tree->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("blackboard"), Blackboard->GetPathName());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_node"))
				{
					FString ClassSpec;
					if (!RequireString(Body, TEXT("class"), ClassSpec, Responder,
						TEXT("a BT node class — list_node_classes enumerates them")))
					{
						return;
					}
					UClass* NodeClass = ResolveClass(ClassSpec);
					if (NodeClass == nullptr || NodeClass->HasAnyClassFlags(CLASS_Abstract))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_class"),
							FString::Printf(
								TEXT("no concrete node class '%s' — use list_node_classes"), *ClassSpec));
						return;
					}

					FString ParentGuid;
					Body->TryGetStringField(TEXT("parent"), ParentGuid);
					UBehaviorTreeGraphNode* Parent = nullptr;
					if (!ParentGuid.IsEmpty())
					{
						Parent = Ai::FindNode(Graph, ParentGuid);
						if (Parent == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"),
								FString::Printf(
									TEXT("no node '%s' — get_tree lists node ids"), *ParentGuid));
							return;
						}
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddBtNode", "McpLink Add Behavior Tree Node"));
					FString Error;
					UBehaviorTreeGraphNode* NewNode = Ai::AddNode(
						Tree, Graph, NodeClass, Parent,
						IntOr(Body, TEXT("x"), 0), IntOr(Body, TEXT("y"), 0), Error);
					if (NewNode == nullptr)
					{
						Responder->Error(
							EHttpServerResponseCodes::BadRequest, TEXT("node_not_created"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = NodeToJson(NewNode);
					if (UAIGraphNode* Owner = NewNode->ParentNode)
					{
						Data->SetStringField(TEXT("parent"), Owner->NodeGuid.ToString());
					}
					else if (Parent != nullptr)
					{
						Data->SetStringField(TEXT("parent"), Parent->NodeGuid.ToString());
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_node"))
				{
					FString Guid;
					if (!RequireString(Body, TEXT("node"), Guid, Responder,
						TEXT("a node id from get_tree")))
					{
						return;
					}
					UBehaviorTreeGraphNode* Node = Ai::FindNode(Graph, Guid);
					if (Node == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"),
							FString::Printf(TEXT("no node '%s'"), *Guid));
						return;
					}
					if (Node->IsA<UBehaviorTreeGraphNode_Root>())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("cannot_remove"),
							TEXT("the root node cannot be removed"));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "RemoveBtNode", "McpLink Remove Behavior Tree Node"));
					Graph->Modify();
					if (UAIGraphNode* Parent = Node->ParentNode)
					{
						// A sub-node lives in its parent's list, not the graph's.
						Parent->RemoveSubNode(Node);
					}
					else
					{
						Node->Modify();
						Node->DestroyNode();
					}
					Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
					Tree->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), Guid);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("compile"))
				{
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CompileBt", "McpLink Compile Behavior Tree"));
					Graph->Modify();
					Graph->UpdateAsset(UBehaviorTreeGraph::ClearDebuggerFlags);
					Tree->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("compiled"), Tree->RootNode != nullptr);
					Data->SetStringField(TEXT("root_node"),
						Tree->RootNode ? Tree->RootNode->GetClass()->GetName() : TEXT(""));
					if (Tree->RootNode == nullptr)
					{
						Data->SetStringField(TEXT("note"),
							TEXT("the graph produced no runtime tree — the root needs a composite ")
							TEXT("child (Selector or Sequence) before any task will run"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					Graph->OnSave();
					FString Filename, Error;
					if (!SaveAsset(Tree, Filename, Error))
					{
						Responder->Error(
							EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("saved"), true);
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, list_node_classes, get_tree, ")
						TEXT("set_blackboard, add_node, remove_node, compile or save"),
						*Operation));
			});
	}
}
