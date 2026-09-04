#include "McpBlueprintUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "AnimationGraph.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationTransitionGraph.h"
#include "AnimStateTransitionNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "McpResolve.h"
#include "UObject/UnrealType.h"

namespace McpLink
{
	UBlueprint* ResolveBlueprint(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		if (UBlueprint* Direct = Cast<UBlueprint>(ResolveObject(Path)))
		{
			return Direct;
		}
		// Accept "/Game/BP_Thing" as well as "/Game/BP_Thing.BP_Thing".
		if (!Path.Contains(TEXT(".")))
		{
			const FString WithAsset =
				FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path));
			return Cast<UBlueprint>(ResolveObject(WithAsset));
		}
		return nullptr;
	}

	namespace
	{
		FString CategoryFor(const UEdGraph* Graph, const TCHAR* TopLevelCategory)
		{
			// Most specific first: state graphs derive from UAnimationGraph.
			if (Graph->IsA<UAnimationStateMachineGraph>()) { return TEXT("anim_state_machine"); }
			if (Graph->IsA<UAnimationTransitionGraph>()) { return TEXT("anim_transition"); }
			if (Graph->IsA<UAnimationStateGraph>()) { return TEXT("anim_state"); }
			if (Graph->IsA<UAnimationGraph>()) { return TEXT("anim_graph"); }
			return TopLevelCategory;
		}

		/// The path segment for a sub-graph. Every transition rule graph is
		/// literally named "Transition" (under its own node), so those are
		/// addressed as "<From> to <To>" the way the editor titles them.
		FString SegmentFor(const UEdGraph* Graph)
		{
			if (const UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Graph->GetOuter()))
			{
				const UAnimStateNodeBase* From = Transition->GetPreviousState();
				const UAnimStateNodeBase* To = Transition->GetNextState();
				if (From != nullptr && To != nullptr)
				{
					return FString::Printf(TEXT("%s to %s"), *From->GetStateName(), *To->GetStateName());
				}
			}
			return Graph->GetName();
		}

		void CollectSubGraphs(UEdGraph* Parent, const FString& ParentPath, TArray<FMcpGraphEntry>& Out)
		{
			TMap<FString, int32> SeenSegments;
			for (UEdGraph* Sub : Parent->SubGraphs)
			{
				if (Sub == nullptr)
				{
					continue;
				}
				FString Segment = SegmentFor(Sub);
				// Two transitions between the same states share a title; keep
				// paths unique by numbering the later ones.
				int32& Seen = SeenSegments.FindOrAdd(Segment);
				if (++Seen > 1)
				{
					Segment = FString::Printf(TEXT("%s #%d"), *Segment, Seen);
				}
				FMcpGraphEntry& Entry = Out.AddDefaulted_GetRef();
				Entry.Category = CategoryFor(Sub, TEXT("sub"));
				Entry.Path = ParentPath + TEXT("/") + Segment;
				Entry.Graph = Sub;
				CollectSubGraphs(Sub, Entry.Path, Out);
			}
		}

		void CollectTopLevel(
			const TArray<TObjectPtr<UEdGraph>>& Graphs, const TCHAR* Category, TArray<FMcpGraphEntry>& Out)
		{
			for (UEdGraph* Graph : Graphs)
			{
				if (Graph == nullptr)
				{
					continue;
				}
				FMcpGraphEntry& Entry = Out.AddDefaulted_GetRef();
				Entry.Category = CategoryFor(Graph, Category);
				Entry.Path = Graph->GetName();
				Entry.Graph = Graph;
				CollectSubGraphs(Graph, Entry.Path, Out);
			}
		}
	}

	void CollectGraphs(UBlueprint* Blueprint, TArray<FMcpGraphEntry>& OutGraphs)
	{
		if (Blueprint == nullptr)
		{
			return;
		}
		CollectTopLevel(Blueprint->UbergraphPages, TEXT("event"), OutGraphs);
		CollectTopLevel(Blueprint->FunctionGraphs, TEXT("function"), OutGraphs);
		CollectTopLevel(Blueprint->MacroGraphs, TEXT("macro"), OutGraphs);
		CollectTopLevel(Blueprint->DelegateSignatureGraphs, TEXT("delegate"), OutGraphs);
	}

	FString GraphPath(const UEdGraph* Graph)
	{
		// Derive it from the same walk FindGraph uses, so the two agree even
		// for numbered duplicates.
		if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph))
		{
			TArray<FMcpGraphEntry> Graphs;
			CollectGraphs(Blueprint, Graphs);
			for (const FMcpGraphEntry& Entry : Graphs)
			{
				if (Entry.Graph == Graph)
				{
					return Entry.Path;
				}
			}
		}
		// Not (yet) reachable from a Blueprint: fall back to the outer chain.
		FString Path;
		for (const UObject* Outer = Graph; Outer != nullptr; Outer = Outer->GetOuter())
		{
			if (const UEdGraph* AsGraph = Cast<UEdGraph>(Outer))
			{
				Path = Path.IsEmpty() ? SegmentFor(AsGraph) : SegmentFor(AsGraph) + TEXT("/") + Path;
			}
			else if (Cast<UEdGraphNode>(Outer) == nullptr)
			{
				break;
			}
		}
		return Path;
	}

	UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName)
	{
		TArray<FMcpGraphEntry> Graphs;
		CollectGraphs(Blueprint, Graphs);
		if (Graphs.IsEmpty())
		{
			return nullptr;
		}
		if (GraphName.IsEmpty())
		{
			return Graphs[0].Graph;
		}
		for (const FMcpGraphEntry& Entry : Graphs)
		{
			if (Entry.Path == GraphName)
			{
				return Entry.Graph;
			}
		}
		for (const FMcpGraphEntry& Entry : Graphs)
		{
			if (Entry.Graph->GetName() == GraphName)
			{
				return Entry.Graph;
			}
		}
		// Partial path from any depth: "Locomotion/Idle".
		const FString Suffix = TEXT("/") + GraphName;
		for (const FMcpGraphEntry& Entry : Graphs)
		{
			if (Entry.Path.EndsWith(Suffix))
			{
				return Entry.Graph;
			}
		}
		return nullptr;
	}

	UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& NodeGuid)
	{
		if (Graph == nullptr)
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node != nullptr && Node->NodeGuid.ToString() == NodeGuid)
			{
				return Node;
			}
		}
		return nullptr;
	}

	UEdGraphPin* FindPin(UEdGraphNode* Node, const FString& PinName)
	{
		if (Node == nullptr)
		{
			return nullptr;
		}
		if (UEdGraphPin* ByName = Node->FindPin(FName(*PinName)))
		{
			return ByName;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && Pin->PinId.ToString() == PinName)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	TSharedRef<FJsonObject> PinToJson(const UEdGraphPin* Pin)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Object->SetStringField(
			TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		Object->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());
		if (!Pin->PinType.PinSubCategory.IsNone())
		{
			Object->SetStringField(TEXT("subtype"), Pin->PinType.PinSubCategory.ToString());
		}
		if (const UObject* SubObject = Pin->PinType.PinSubCategoryObject.Get())
		{
			Object->SetStringField(TEXT("subtype_object"), SubObject->GetPathName());
		}
		if (!Pin->DefaultValue.IsEmpty())
		{
			Object->SetStringField(TEXT("default"), Pin->DefaultValue);
		}
		TArray<TSharedPtr<FJsonValue>> Links;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked == nullptr || Linked->GetOwningNode() == nullptr)
			{
				continue;
			}
			const TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
			Link->SetStringField(TEXT("node"), Linked->GetOwningNode()->NodeGuid.ToString());
			Link->SetStringField(TEXT("pin"), Linked->PinName.ToString());
			Links.Add(MakeShared<FJsonValueObject>(Link));
		}
		Object->SetArrayField(TEXT("linked_to"), Links);
		return Object;
	}

	TSharedRef<FJsonObject> NodeToJson(const UEdGraphNode* Node)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("guid"), Node->NodeGuid.ToString());
		Object->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		Object->SetStringField(
			TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
		Object->SetNumberField(TEXT("x"), Node->NodePosX);
		Object->SetNumberField(TEXT("y"), Node->NodePosY);
		TArray<TSharedPtr<FJsonValue>> Pins;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr)
			{
				Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin)));
			}
		}
		Object->SetArrayField(TEXT("pins"), Pins);
		return Object;
	}

	bool MakePinType(const FString& TypeName, FEdGraphPinType& OutType, FString& OutError)
	{
		OutType = FEdGraphPinType();
		const FString Lower = TypeName.ToLower();

		if (Lower == TEXT("bool")) { OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean; return true; }
		if (Lower == TEXT("byte")) { OutType.PinCategory = UEdGraphSchema_K2::PC_Byte; return true; }
		if (Lower == TEXT("int") || Lower == TEXT("int32"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
			return true;
		}
		if (Lower == TEXT("int64")) { OutType.PinCategory = UEdGraphSchema_K2::PC_Int64; return true; }
		if (Lower == TEXT("float") || Lower == TEXT("double") || Lower == TEXT("real"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			return true;
		}
		if (Lower == TEXT("string")) { OutType.PinCategory = UEdGraphSchema_K2::PC_String; return true; }
		if (Lower == TEXT("name")) { OutType.PinCategory = UEdGraphSchema_K2::PC_Name; return true; }
		if (Lower == TEXT("text")) { OutType.PinCategory = UEdGraphSchema_K2::PC_Text; return true; }

		// Common structs by friendly name.
		static const TMap<FString, FName> StructNames = {
			{TEXT("vector"), TEXT("Vector")},
			{TEXT("vector2d"), TEXT("Vector2D")},
			{TEXT("rotator"), TEXT("Rotator")},
			{TEXT("transform"), TEXT("Transform")},
			{TEXT("linearcolor"), TEXT("LinearColor")},
			{TEXT("color"), TEXT("Color")},
			{TEXT("hitresult"), TEXT("HitResult")},
		};
		if (const FName* StructName = StructNames.Find(Lower))
		{
			if (UScriptStruct* Struct = FindFirstObject<UScriptStruct>(
				*StructName->ToString(), EFindFirstObjectOptions::None))
			{
				OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutType.PinSubCategoryObject = Struct;
				return true;
			}
		}

		// "object:<class>" / "class:<class>" for references.
		FString Prefix, ClassSpec;
		if (TypeName.Split(TEXT(":"), &Prefix, &ClassSpec))
		{
			UClass* Class = ResolveClass(ClassSpec);
			if (Class == nullptr)
			{
				OutError = FString::Printf(TEXT("unknown class '%s'"), *ClassSpec);
				return false;
			}
			OutType.PinCategory = Prefix.Equals(TEXT("class"), ESearchCase::IgnoreCase)
				? UEdGraphSchema_K2::PC_Class
				: UEdGraphSchema_K2::PC_Object;
			OutType.PinSubCategoryObject = Class;
			return true;
		}

		// Bare struct or class name as a last resort.
		if (UScriptStruct* Struct = FindFirstObject<UScriptStruct>(
			*TypeName, EFindFirstObjectOptions::None))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = Struct;
			return true;
		}
		if (UClass* Class = ResolveClass(TypeName))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Object;
			OutType.PinSubCategoryObject = Class;
			return true;
		}

		OutError = FString::Printf(
			TEXT("unknown type '%s' — use bool, byte, int, int64, float, string, name, text, ")
			TEXT("vector, vector2d, rotator, transform, linearcolor, color, or object:<Class> / class:<Class>"),
			*TypeName);
		return false;
	}
}
