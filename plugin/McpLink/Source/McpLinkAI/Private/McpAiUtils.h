// Shared authoring helpers for the Blackboard and Behavior Tree routes.
//
// The route handlers do request parsing and JSON; everything that touches
// engine state lives here so the automation tests can drive it directly.
#pragma once

#include "CoreMinimal.h"

class UBehaviorTree;
class UBehaviorTreeGraph;
class UBehaviorTreeGraphNode;
class UBlackboardData;
class UBlackboardKeyType;
class UClass;

namespace McpLink::Ai
{
	/// Build the key-type instance a wire "type" asks for ("bool", "vector",
	/// "object:<Class>", "enum:<Enum>", ...), owned by the blackboard asset.
	/// Returns nullptr with OutError naming the fix.
	UBlackboardKeyType* MakeKeyType(
		UBlackboardData* Owner, const FString& Spec, FString& OutError);

	/// The asset's editor graph, created (with its Root node) the way the
	/// Behavior Tree editor creates it the first time an asset is opened.
	UBehaviorTreeGraph* EnsureGraph(UBehaviorTree* Tree);

	/// The Root node of a behavior tree graph.
	UBehaviorTreeGraphNode* FindRoot(UBehaviorTreeGraph* Graph);

	/// Node lookup by GUID string, including sub-nodes (decorators/services),
	/// which do not live in Graph->Nodes.
	UBehaviorTreeGraphNode* FindNode(UBehaviorTreeGraph* Graph, const FString& Guid);

	/// The graph-node class the Behavior Tree editor uses for a runtime node
	/// class, and the node's kind ("task", "composite", "decorator",
	/// "service"). Returns nullptr when the class is none of those.
	UClass* GraphNodeClassFor(UClass* NodeClass, FString& OutKind);

	/// Add a node to the tree: composites and tasks are wired under `Parent`,
	/// decorators and services attach to it as sub-nodes. Passing no parent
	/// wires under the root. Rebuilds the runtime tree on success.
	UBehaviorTreeGraphNode* AddNode(
		UBehaviorTree* Tree,
		UBehaviorTreeGraph* Graph,
		UClass* NodeClass,
		UBehaviorTreeGraphNode* Parent,
		int32 PosX,
		int32 PosY,
		FString& OutError);
}
