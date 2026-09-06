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
#include "Misc/PackageName.h"
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
		// Graphs an implemented interface brought in (anim layers among them).
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			CollectTopLevel(Interface.Graphs, TEXT("interface"), OutGraphs);
		}
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

	UEnum* ResolveEnum(const FString& Spec)
	{
		if (Spec.IsEmpty())
		{
			return nullptr;
		}
		if (UEnum* Direct = Cast<UEnum>(ResolveObject(Spec)))
		{
			return Direct;
		}
		// A User-Defined Enum asset: "/Game/Enums/E_Team" names the package.
		if (!Spec.Contains(TEXT(".")) && Spec.StartsWith(TEXT("/")))
		{
			if (UEnum* Asset = Cast<UEnum>(ResolveObject(
				FString::Printf(TEXT("%s.%s"), *Spec, *FPackageName::GetShortName(Spec)))))
			{
				return Asset;
			}
		}
		return FindFirstObject<UEnum>(*Spec, EFindFirstObjectOptions::None);
	}

	UScriptStruct* ResolveStruct(const FString& Spec)
	{
		if (Spec.IsEmpty())
		{
			return nullptr;
		}
		if (UScriptStruct* Direct = Cast<UScriptStruct>(ResolveObject(Spec)))
		{
			return Direct;
		}
		if (!Spec.Contains(TEXT(".")) && Spec.StartsWith(TEXT("/")))
		{
			if (UScriptStruct* Asset = Cast<UScriptStruct>(ResolveObject(
				FString::Printf(TEXT("%s.%s"), *Spec, *FPackageName::GetShortName(Spec)))))
			{
				return Asset;
			}
		}
		if (UScriptStruct* ByName =
			FindFirstObject<UScriptStruct>(*Spec, EFindFirstObjectOptions::None))
		{
			return ByName;
		}
		// "FHitResult" is how the type is spelled in C++.
		if (Spec.Len() > 1 && Spec[0] == TEXT('F'))
		{
			return FindFirstObject<UScriptStruct>(*Spec.RightChop(1), EFindFirstObjectOptions::None);
		}
		return nullptr;
	}

	namespace
	{
		/// Everything except the container wrapper: scalars, structs, enums and
		/// the "prefix:Class" reference forms.
		bool MakeTerminalPinType(const FString& TypeName, FEdGraphPinType& OutType, FString& OutError)
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

			// "<kind>:<name>" for references, structs and enums. Split on the
			// first colon only, so "/Script/..." paths survive on the right.
			FString Prefix, Rest;
			if (TypeName.Split(TEXT(":"), &Prefix, &Rest))
			{
				const FString Kind = Prefix.ToLower();
				if (Kind == TEXT("struct"))
				{
					UScriptStruct* Struct = ResolveStruct(Rest);
					if (Struct == nullptr)
					{
						OutError = FString::Printf(TEXT("unknown struct '%s'"), *Rest);
						return false;
					}
					OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
					OutType.PinSubCategoryObject = Struct;
					return true;
				}
				if (Kind == TEXT("enum"))
				{
					UEnum* Enum = ResolveEnum(Rest);
					if (Enum == nullptr)
					{
						OutError = FString::Printf(TEXT("unknown enum '%s'"), *Rest);
						return false;
					}
					OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
					OutType.PinSubCategoryObject = Enum;
					return true;
				}

				UClass* Class = ResolveClass(Rest);
				if (Class == nullptr)
				{
					OutError = FString::Printf(TEXT("unknown class '%s'"), *Rest);
					return false;
				}
				if (Kind == TEXT("class")) { OutType.PinCategory = UEdGraphSchema_K2::PC_Class; }
				else if (Kind == TEXT("softobject")) { OutType.PinCategory = UEdGraphSchema_K2::PC_SoftObject; }
				else if (Kind == TEXT("softclass")) { OutType.PinCategory = UEdGraphSchema_K2::PC_SoftClass; }
				else if (Kind == TEXT("interface")) { OutType.PinCategory = UEdGraphSchema_K2::PC_Interface; }
				else { OutType.PinCategory = UEdGraphSchema_K2::PC_Object; }
				OutType.PinSubCategoryObject = Class;
				return true;
			}

			// Bare struct, enum or class name as a last resort.
			if (UScriptStruct* Struct = ResolveStruct(TypeName))
			{
				OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				OutType.PinSubCategoryObject = Struct;
				return true;
			}
			if (UEnum* Enum = FindFirstObject<UEnum>(*TypeName, EFindFirstObjectOptions::None))
			{
				OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
				OutType.PinSubCategoryObject = Enum;
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
				TEXT("vector, vector2d, rotator, transform, linearcolor, color, hitresult, ")
				TEXT("object:<Class>, class:<Class>, softobject:<Class>, softclass:<Class>, ")
				TEXT("interface:<Class>, struct:<Struct>, enum:<Enum>, or a container such as ")
				TEXT("array<int>, set<name>, map<name,float>"),
				*TypeName);
			return false;
		}

		/// "array<int>" -> ("array", "int"). False when TypeName is not a
		/// container of this kind.
		bool SplitContainer(const FString& TypeName, const TCHAR* Keyword, FString& OutInner)
		{
			const FString Open = FString(Keyword) + TEXT("<");
			if (!TypeName.StartsWith(Open, ESearchCase::IgnoreCase) || !TypeName.EndsWith(TEXT(">")))
			{
				return false;
			}
			OutInner = TypeName.Mid(Open.Len(), TypeName.Len() - Open.Len() - 1).TrimStartAndEnd();
			return !OutInner.IsEmpty();
		}
	}

	bool MakePinType(const FString& TypeName, FEdGraphPinType& OutType, FString& OutError)
	{
		const FString Trimmed = TypeName.TrimStartAndEnd();
		FString Inner;

		if (SplitContainer(Trimmed, TEXT("array"), Inner))
		{
			if (!MakeTerminalPinType(Inner, OutType, OutError)) { return false; }
			OutType.ContainerType = EPinContainerType::Array;
			return true;
		}
		if (SplitContainer(Trimmed, TEXT("set"), Inner))
		{
			if (!MakeTerminalPinType(Inner, OutType, OutError)) { return false; }
			OutType.ContainerType = EPinContainerType::Set;
			return true;
		}
		if (SplitContainer(Trimmed, TEXT("map"), Inner))
		{
			FString KeySpec, ValueSpec;
			if (!Inner.Split(TEXT(","), &KeySpec, &ValueSpec))
			{
				OutError = FString::Printf(
					TEXT("map needs a key and a value type, e.g. map<name,float> (got '%s')"), *Inner);
				return false;
			}
			FEdGraphPinType ValueType;
			if (!MakeTerminalPinType(KeySpec.TrimStartAndEnd(), OutType, OutError)
				|| !MakeTerminalPinType(ValueSpec.TrimStartAndEnd(), ValueType, OutError))
			{
				return false;
			}
			OutType.ContainerType = EPinContainerType::Map;
			OutType.PinValueType = FEdGraphTerminalType::FromPinType(ValueType);
			return true;
		}

		return MakeTerminalPinType(Trimmed, OutType, OutError);
	}
}
