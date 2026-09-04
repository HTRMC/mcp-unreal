#include "McpAiUtils.h"

#include "AIGraphTypes.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Composites/BTComposite_SimpleParallel.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Composite.h"
#include "BehaviorTreeGraphNode_Decorator.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "BehaviorTreeGraphNode_Service.h"
#include "BehaviorTreeGraphNode_SimpleParallel.h"
#include "BehaviorTreeGraphNode_SubtreeTask.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "McpResolve.h"

namespace McpLink::Ai
{
	UBlackboardKeyType* MakeKeyType(
		UBlackboardData* Owner, const FString& Spec, FString& OutError)
	{
		const FString Lower = Spec.ToLower();
		if (Lower == TEXT("bool")) { return NewObject<UBlackboardKeyType_Bool>(Owner); }
		if (Lower == TEXT("int") || Lower == TEXT("int32"))
		{
			return NewObject<UBlackboardKeyType_Int>(Owner);
		}
		if (Lower == TEXT("float") || Lower == TEXT("double"))
		{
			return NewObject<UBlackboardKeyType_Float>(Owner);
		}
		if (Lower == TEXT("string")) { return NewObject<UBlackboardKeyType_String>(Owner); }
		if (Lower == TEXT("name")) { return NewObject<UBlackboardKeyType_Name>(Owner); }
		if (Lower == TEXT("vector")) { return NewObject<UBlackboardKeyType_Vector>(Owner); }
		if (Lower == TEXT("rotator")) { return NewObject<UBlackboardKeyType_Rotator>(Owner); }

		FString Prefix, Rest;
		if (Spec.Split(TEXT(":"), &Prefix, &Rest))
		{
			const FString Kind = Prefix.ToLower();
			if (Kind == TEXT("object") || Kind == TEXT("class"))
			{
				UClass* Base = ResolveClass(Rest);
				if (Base == nullptr)
				{
					OutError = FString::Printf(TEXT("unknown class '%s'"), *Rest);
					return nullptr;
				}
				if (Kind == TEXT("object"))
				{
					UBlackboardKeyType_Object* Key = NewObject<UBlackboardKeyType_Object>(Owner);
					Key->BaseClass = Base;
					return Key;
				}
				UBlackboardKeyType_Class* Key = NewObject<UBlackboardKeyType_Class>(Owner);
				Key->BaseClass = Base;
				return Key;
			}
			if (Kind == TEXT("enum"))
			{
				UEnum* Enum = Cast<UEnum>(ResolveObject(Rest));
				if (Enum == nullptr)
				{
					Enum = FindFirstObject<UEnum>(*Rest, EFindFirstObjectOptions::None);
				}
				if (Enum == nullptr)
				{
					OutError = FString::Printf(TEXT("unknown enum '%s'"), *Rest);
					return nullptr;
				}
				UBlackboardKeyType_Enum* Key = NewObject<UBlackboardKeyType_Enum>(Owner);
				Key->EnumType = Enum;
				Key->EnumName = Enum->GetName();
				return Key;
			}
		}

		OutError = FString::Printf(
			TEXT("unknown key type '%s' — use bool, int, float, string, name, vector, rotator, ")
			TEXT("object:<Class>, class:<Class> or enum:<Enum>"),
			*Spec);
		return nullptr;
	}

	UBehaviorTreeGraph* EnsureGraph(UBehaviorTree* Tree)
	{
		if (Tree == nullptr)
		{
			return nullptr;
		}
		if (UBehaviorTreeGraph* Existing = Cast<UBehaviorTreeGraph>(Tree->BTGraph))
		{
			return Existing;
		}
		const TSubclassOf<UEdGraphSchema> SchemaClass = GetDefault<UBehaviorTreeGraph>()->Schema;
		Tree->BTGraph = FBlueprintEditorUtils::CreateNewGraph(
			Tree, TEXT("Behavior Tree"), UBehaviorTreeGraph::StaticClass(), SchemaClass);
		UBehaviorTreeGraph* Graph = Cast<UBehaviorTreeGraph>(Tree->BTGraph);
		if (Graph != nullptr)
		{
			// Creates the Root node. Without it the graph compiles to nothing.
			Graph->GetSchema()->CreateDefaultNodesForGraph(*Graph);
			Graph->OnCreated();
			Graph->Initialize();
		}
		return Graph;
	}

	UBehaviorTreeGraphNode* FindRoot(UBehaviorTreeGraph* Graph)
	{
		if (Graph == nullptr)
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node != nullptr && Node->IsA<UBehaviorTreeGraphNode_Root>())
			{
				return Cast<UBehaviorTreeGraphNode>(Node);
			}
		}
		return nullptr;
	}

	UBehaviorTreeGraphNode* FindNode(UBehaviorTreeGraph* Graph, const FString& Guid)
	{
		if (Graph == nullptr)
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			UBehaviorTreeGraphNode* BtNode = Cast<UBehaviorTreeGraphNode>(Node);
			if (BtNode == nullptr)
			{
				continue;
			}
			if (BtNode->NodeGuid.ToString() == Guid)
			{
				return BtNode;
			}
			// Decorators and services hang off a node, not off the graph.
			for (UAIGraphNode* Sub : BtNode->SubNodes)
			{
				if (Sub != nullptr && Sub->NodeGuid.ToString() == Guid)
				{
					return Cast<UBehaviorTreeGraphNode>(Sub);
				}
			}
		}
		return nullptr;
	}

	UClass* GraphNodeClassFor(UClass* NodeClass, FString& OutKind)
	{
		if (NodeClass == nullptr)
		{
			return nullptr;
		}
		if (NodeClass->IsChildOf(UBTTaskNode::StaticClass()))
		{
			OutKind = TEXT("task");
			return NodeClass->IsChildOf(UBTTask_RunBehavior::StaticClass())
				? UBehaviorTreeGraphNode_SubtreeTask::StaticClass()
				: UBehaviorTreeGraphNode_Task::StaticClass();
		}
		if (NodeClass->IsChildOf(UBTCompositeNode::StaticClass()))
		{
			OutKind = TEXT("composite");
			return NodeClass->IsChildOf(UBTComposite_SimpleParallel::StaticClass())
				? UBehaviorTreeGraphNode_SimpleParallel::StaticClass()
				: UBehaviorTreeGraphNode_Composite::StaticClass();
		}
		if (NodeClass->IsChildOf(UBTDecorator::StaticClass()))
		{
			OutKind = TEXT("decorator");
			return UBehaviorTreeGraphNode_Decorator::StaticClass();
		}
		if (NodeClass->IsChildOf(UBTService::StaticClass()))
		{
			OutKind = TEXT("service");
			return UBehaviorTreeGraphNode_Service::StaticClass();
		}
		return nullptr;
	}

	UBehaviorTreeGraphNode* AddNode(
		UBehaviorTree* Tree,
		UBehaviorTreeGraph* Graph,
		UClass* NodeClass,
		UBehaviorTreeGraphNode* Parent,
		int32 PosX,
		int32 PosY,
		FString& OutError)
	{
		if (Tree == nullptr || Graph == nullptr)
		{
			OutError = TEXT("no behavior tree graph");
			return nullptr;
		}
		FString Kind;
		UClass* GraphNodeClass = GraphNodeClassFor(NodeClass, Kind);
		if (GraphNodeClass == nullptr)
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a Behavior Tree task, composite, decorator or service"),
				NodeClass ? *NodeClass->GetName() : TEXT("(null)"));
			return nullptr;
		}

		const bool bSubNode = Kind == TEXT("decorator") || Kind == TEXT("service");
		if (bSubNode && Parent == nullptr)
		{
			OutError = TEXT("a decorator or service attaches to a node — pass 'parent'");
			return nullptr;
		}
		if (Parent == nullptr)
		{
			Parent = FindRoot(Graph);
		}

		Graph->Modify();

		if (bSubNode)
		{
			UBehaviorTreeGraphNode* SubNode = NewObject<UBehaviorTreeGraphNode>(Graph, GraphNodeClass);
			// ClassData is what PostPlacedNewNode builds the runtime instance
			// from, so it has to be set before AddSubNode runs it.
			SubNode->ClassData = FGraphNodeClassData(NodeClass, FString());
			Parent->AddSubNode(SubNode, Graph);
			Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
			Tree->MarkPackageDirty();
			return SubNode;
		}

		UBehaviorTreeGraphNode* NewNode = NewObject<UBehaviorTreeGraphNode>(Graph, GraphNodeClass);
		NewNode->ClassData = FGraphNodeClassData(NodeClass, FString());
		NewNode->SetFlags(RF_Transactional);
		Graph->AddNode(NewNode, /*bFromUI*/ true, /*bSelectNewNode*/ false);
		NewNode->CreateNewGuid();
		NewNode->PostPlacedNewNode();
		NewNode->AllocateDefaultPins();
		NewNode->NodePosX = PosX;
		NewNode->NodePosY = PosY;

		if (Parent != nullptr)
		{
			UEdGraphPin* From = Parent->GetOutputPin();
			UEdGraphPin* To = NewNode->GetInputPin();
			if (From == nullptr || To == nullptr)
			{
				OutError = FString::Printf(
					TEXT("'%s' has no child pin — only the root and composites take children"),
					*Parent->GetNodeTitle(ENodeTitleType::ListView).ToString());
				NewNode->DestroyNode();
				return nullptr;
			}
			// Through the schema, so a task under a task is refused here rather
			// than producing a graph that quietly compiles to nothing.
			const FPinConnectionResponse Allowed =
				Graph->GetSchema()->CanCreateConnection(From, To);
			if (Allowed.Response == CONNECT_RESPONSE_DISALLOW)
			{
				OutError = FString::Printf(
					TEXT("cannot attach here: %s"), *Allowed.Message.ToString());
				NewNode->DestroyNode();
				return nullptr;
			}
			Graph->GetSchema()->TryCreateConnection(From, To);
		}

		Graph->UpdateAsset(UBehaviorTreeGraph::KeepRebuildCounter);
		Tree->MarkPackageDirty();
		return NewNode;
	}
}
