// Shared helpers for the Blueprint routes: asset lookup, graph/node/pin
// resolution, and JSON serialisation of graph structure.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UBlueprint;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UEnum;
class UScriptStruct;
struct FEdGraphPinType;

namespace McpLink
{
	/// One graph of a Blueprint, including nested sub-graphs (state machines,
	/// states, transition rules, collapsed graphs).
	struct FMcpGraphEntry
	{
		/// event, function, macro, delegate, anim_graph, anim_state_machine,
		/// anim_state, anim_transition, or sub.
		FString Category;
		/// Slash-separated names from the top-level graph down, e.g.
		/// "AnimGraph/Locomotion/Idle".
		FString Path;
		UEdGraph* Graph = nullptr;
	};

	/// Load a Blueprint by asset path ("/Game/BP_Thing", with or without the
	/// ".BP_Thing" suffix). Returns nullptr when the asset is missing or is
	/// not a Blueprint.
	UBlueprint* ResolveBlueprint(const FString& Path);

	/// Find a graph by name or slash path. A bare name matches the first graph
	/// with that name (depth-first from the top-level graphs); a path such as
	/// "Locomotion/Idle" or "AnimGraph/Locomotion/Idle" disambiguates. Empty
	/// name returns the first event graph.
	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName);

	/// All graphs of a Blueprint, top-level graphs first, each followed by its
	/// nested sub-graphs.
	void CollectGraphs(UBlueprint* Blueprint, TArray<FMcpGraphEntry>& OutGraphs);

	/// The slash path of a graph (see FMcpGraphEntry::Path).
	FString GraphPath(const UEdGraph* Graph);

	/// Node lookup by GUID string (as reported by get_graph).
	UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& NodeGuid);

	/// Pin lookup by name, then by pin id, on a node.
	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName);

	TSharedRef<FJsonObject> PinToJson(const UEdGraphPin* Pin);
	TSharedRef<FJsonObject> NodeToJson(const UEdGraphNode* Node);

	/// Map a type name onto a pin type. Scalars ("bool", "int", "float",
	/// "string", "name", "text"), common structs ("vector", "transform"),
	/// prefixed references ("object:<Class>", "class:<Class>",
	/// "softobject:<Class>", "softclass:<Class>", "interface:<Class>",
	/// "struct:<Struct>", "enum:<Enum>") and containers ("array<int>",
	/// "set<name>", "map<name,float>"). Returns false with a message for
	/// anything unrecognised.
	bool MakePinType(const FString& TypeName, FEdGraphPinType& OutType, FString& OutError);

	/// An enum by path ("/Script/Engine.ECollisionChannel"), by bare name, or
	/// a User-Defined Enum asset path.
	UEnum* ResolveEnum(const FString& Spec);

	/// A struct by path, by bare name, or with the C++ "F" prefix; also a
	/// User-Defined Struct asset path.
	UScriptStruct* ResolveStruct(const FString& Spec);

	/// Spawn and configure one node in `Graph` from an add_node request body
	/// (see McpBlueprintNodes.cpp for the vocabulary). Returns nullptr with
	/// OutError naming the fix.
	UEdGraphNode* CreateGraphNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TSharedRef<FJsonObject>& Body,
		FString& OutError);
}
