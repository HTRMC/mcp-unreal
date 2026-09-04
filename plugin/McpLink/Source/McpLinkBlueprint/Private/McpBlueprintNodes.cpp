// The add_node vocabulary: every node kind blueprint_modify can spawn.
//
// Split out of McpBlueprintRoutes.cpp because the node table is the part that
// keeps growing. Two rules hold throughout, both learned the hard way:
//   * configure a node BEFORE FGraphNodeCreator::Finalize(), so its pins are
//     allocated from the real signature / struct / enum rather than empty;
//   * anything that needs the node to already exist (SetPurity, which
//     reconstructs) runs after Finalize.

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/TimelineTemplate.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ClassDynamicCast.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EnumLiteral.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FormatText.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_Literal.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeMap.h"
#include "K2Node_MakeSet.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_Select.h"
#include "K2Node_Self.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchString.h"
#include "K2Node_Timeline.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "McpBlueprintUtils.h"
#include "McpJson.h"
#include "McpResolve.h"

namespace McpLink
{
	namespace
	{
		FString StringField(const TSharedRef<FJsonObject>& Body, const TCHAR* Field)
		{
			FString Value;
			Body->TryGetStringField(Field, Value);
			return Value;
		}

		/// The class a bare function/variable/event name is looked up on: the
		/// Blueprint's own generated class, falling back to its parent while it
		/// has never been compiled.
		UClass* SelfClass(UBlueprint* Blueprint)
		{
			if (Blueprint->SkeletonGeneratedClass != nullptr)
			{
				return Blueprint->SkeletonGeneratedClass;
			}
			return Blueprint->GeneratedClass != nullptr
				? Blueprint->GeneratedClass.Get()
				: Blueprint->ParentClass.Get();
		}

		/// The class named by `class`, or the Blueprint itself when absent.
		UClass* TargetClass(
			UBlueprint* Blueprint, const TSharedRef<FJsonObject>& Body, FString& OutError)
		{
			const FString Spec = StringField(Body, TEXT("class"));
			if (Spec.IsEmpty())
			{
				return SelfClass(Blueprint);
			}
			UClass* Class = ResolveClass(Spec);
			if (Class == nullptr)
			{
				OutError = FString::Printf(
					TEXT("no class '%s' — pass a name, a /Script path, or a Blueprint asset path"), *Spec);
			}
			return Class;
		}

		/// The standard macro library, plus whatever library the caller names.
		UBlueprint* MacroLibrary(const TSharedRef<FJsonObject>& Body)
		{
			FString Spec = StringField(Body, TEXT("library"));
			if (Spec.IsEmpty())
			{
				Spec = TEXT("/Engine/EditorBlueprintResources/StandardMacros");
			}
			if (UBlueprint* Direct = ResolveBlueprint(Spec))
			{
				return Direct;
			}
			return nullptr;
		}

		FString MacroNames(const UBlueprint* Library)
		{
			TArray<FString> Names;
			for (const UEdGraph* Graph : Library->MacroGraphs)
			{
				if (Graph != nullptr)
				{
					Names.Add(Graph->GetName());
				}
			}
			Names.Sort();
			return FString::Join(Names, TEXT(", "));
		}

		/// Both delegate-node families take a multicast delegate property that
		/// lives either on this Blueprint (an event dispatcher) or on a class.
		bool BindDelegateReference(
			UK2Node_BaseMCDelegate* Node,
			UBlueprint* Blueprint,
			const TSharedRef<FJsonObject>& Body,
			FString& OutError)
		{
			const FString Name = StringField(Body, TEXT("delegate"));
			if (Name.IsEmpty())
			{
				OutError = TEXT("'delegate' is required — the event dispatcher or delegate property name");
				return false;
			}
			UClass* Owner = TargetClass(Blueprint, Body, OutError);
			if (Owner == nullptr)
			{
				return false;
			}
			FMulticastDelegateProperty* Property =
				FindFProperty<FMulticastDelegateProperty>(Owner, FName(*Name));
			if (Property == nullptr)
			{
				OutError = FString::Printf(
					TEXT("'%s' has no multicast delegate '%s' — add_event_dispatcher creates one on this Blueprint"),
					*Owner->GetName(), *Name);
				return false;
			}
			Node->SetFromProperty(Property, Owner == SelfClass(Blueprint), Owner);
			return true;
		}
	}

	UEdGraphNode* CreateGraphNode(
		UBlueprint* Blueprint,
		UEdGraph* Graph,
		const TSharedRef<FJsonObject>& Body,
		FString& OutError)
	{
		const FString NodeType = StringField(Body, TEXT("node_type"));

		// ------------------------------------------------------------ functions
		if (NodeType.Equals(TEXT("call_function"), ESearchCase::IgnoreCase))
		{
			const FString FunctionName = StringField(Body, TEXT("function"));
			UClass* Owner = TargetClass(Blueprint, Body, OutError);
			if (Owner == nullptr)
			{
				return nullptr;
			}
			UFunction* Function = Owner->FindFunctionByName(FName(*FunctionName));
			if (Function == nullptr)
			{
				OutError = FString::Printf(
					TEXT("'%s' has no function '%s' — get_class_info lists a class's functions"),
					*Owner->GetName(), *FunctionName);
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
			UK2Node_CallFunction* Node = Creator.CreateNode();
			Node->SetFromFunction(Function);
			Creator.Finalize();
			return Node;
		}

		if (NodeType.Equals(TEXT("call_parent_function"), ESearchCase::IgnoreCase))
		{
			const FString FunctionName = StringField(Body, TEXT("function"));
			UClass* Parent = Blueprint->ParentClass.Get();
			UFunction* Function = Parent ? Parent->FindFunctionByName(FName(*FunctionName)) : nullptr;
			if (Function == nullptr)
			{
				OutError = FString::Printf(
					TEXT("parent class '%s' has no function '%s'"),
					Parent ? *Parent->GetName() : TEXT("(none)"), *FunctionName);
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_CallParentFunction> Creator(*Graph);
			UK2Node_CallParentFunction* Node = Creator.CreateNode();
			Node->SetFromFunction(Function);
			Creator.Finalize();
			return Node;
		}

		// ------------------------------------------------------------ variables
		if (NodeType.Equals(TEXT("variable_get"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("variable_set"), ESearchCase::IgnoreCase))
		{
			const FString VarName = StringField(Body, TEXT("variable"));
			if (VarName.IsEmpty())
			{
				OutError = TEXT("'variable' is required for variable_get/variable_set");
				return nullptr;
			}
			const FString ClassSpec = StringField(Body, TEXT("class"));
			UClass* Owner = nullptr;
			if (!ClassSpec.IsEmpty())
			{
				Owner = TargetClass(Blueprint, Body, OutError);
				if (Owner == nullptr)
				{
					return nullptr;
				}
			}
			const bool bGet = NodeType.Equals(TEXT("variable_get"), ESearchCase::IgnoreCase);
			if (bGet)
			{
				FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
				UK2Node_VariableGet* Node = Creator.CreateNode();
				if (Owner != nullptr)
				{
					Node->VariableReference.SetExternalMember(FName(*VarName), Owner);
				}
				else
				{
					Node->VariableReference.SetSelfMember(FName(*VarName));
				}
				Creator.Finalize();
				return Node;
			}
			FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
			UK2Node_VariableSet* Node = Creator.CreateNode();
			if (Owner != nullptr)
			{
				Node->VariableReference.SetExternalMember(FName(*VarName), Owner);
			}
			else
			{
				Node->VariableReference.SetSelfMember(FName(*VarName));
			}
			Creator.Finalize();
			return Node;
		}

		// --------------------------------------------------------------- events
		if (NodeType.Equals(TEXT("custom_event"), ESearchCase::IgnoreCase))
		{
			const FString EventName = StringField(Body, TEXT("name"));
			FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
			UK2Node_CustomEvent* Node = Creator.CreateNode();
			Node->CustomFunctionName =
				FName(*(EventName.IsEmpty() ? FString(TEXT("NewEvent")) : EventName));
			Creator.Finalize();
			return Node;
		}

		if (NodeType.Equals(TEXT("event"), ESearchCase::IgnoreCase))
		{
			const FString FunctionName = StringField(Body, TEXT("function"));
			UClass* Owner = TargetClass(Blueprint, Body, OutError);
			if (Owner == nullptr)
			{
				return nullptr;
			}
			UFunction* Function = Owner->FindFunctionByName(FName(*FunctionName));
			if (Function == nullptr)
			{
				OutError = FString::Printf(
					TEXT("no overridable event '%s' on '%s' — it must be a BlueprintImplementableEvent ")
					TEXT("or BlueprintNativeEvent, e.g. ReceiveBeginPlay, ReceiveTick"),
					*FunctionName, *Owner->GetName());
				return nullptr;
			}
			if (!UEdGraphSchema_K2::CanKismetOverrideFunction(Function))
			{
				OutError = FString::Printf(
					TEXT("'%s' is not an overridable event — call it with call_function instead"),
					*FunctionName);
				return nullptr;
			}
			// A second node for the same event will not compile, so point the
			// caller at the one that exists.
			if (UK2Node_Event* Existing = FBlueprintEditorUtils::FindOverrideForFunction(
				Blueprint, Function->GetOwnerClass(), Function->GetFName()))
			{
				OutError = FString::Printf(
					TEXT("'%s' is already handled by node %s in graph '%s' — a Blueprint may hold one node per event"),
					*FunctionName, *Existing->NodeGuid.ToString(), *GraphPath(Existing->GetGraph()));
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_Event> Creator(*Graph);
			UK2Node_Event* Node = Creator.CreateNode();
			Node->EventReference.SetExternalMember(Function->GetFName(), Function->GetOwnerClass());
			Node->bOverrideFunction = true;
			Creator.Finalize();
			return Node;
		}

		if (NodeType.Equals(TEXT("component_bound_event"), ESearchCase::IgnoreCase))
		{
			const FString ComponentName = StringField(Body, TEXT("component"));
			const FString DelegateName = StringField(Body, TEXT("delegate"));
			if (ComponentName.IsEmpty() || DelegateName.IsEmpty())
			{
				OutError = TEXT("'component' and 'delegate' are required, e.g. component \"Box\", delegate \"OnComponentBeginOverlap\"");
				return nullptr;
			}
			UClass* Self = SelfClass(Blueprint);
			FObjectProperty* ComponentProperty =
				Self ? FindFProperty<FObjectProperty>(Self, FName(*ComponentName)) : nullptr;
			if (ComponentProperty == nullptr)
			{
				OutError = FString::Printf(
					TEXT("no component variable '%s' on this Blueprint — blueprint_query inspect lists components ")
					TEXT("(a component added this session needs a compile first)"),
					*ComponentName);
				return nullptr;
			}
			FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(
				ComponentProperty->PropertyClass, FName(*DelegateName));
			if (Delegate == nullptr)
			{
				OutError = FString::Printf(
					TEXT("'%s' (%s) has no delegate '%s'"),
					*ComponentName, *ComponentProperty->PropertyClass->GetName(), *DelegateName);
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_ComponentBoundEvent> Creator(*Graph);
			UK2Node_ComponentBoundEvent* Node = Creator.CreateNode();
			Node->InitializeComponentBoundEventParams(ComponentProperty, Delegate);
			Creator.Finalize();
			return Node;
		}

		// ------------------------------------------------------------ flow control
		if (NodeType.Equals(TEXT("branch"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
			UK2Node_IfThenElse* Node = Creator.CreateNode();
			Creator.Finalize();
			return Node;
		}

		if (NodeType.Equals(TEXT("sequence"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(*Graph);
			UK2Node_ExecutionSequence* Node = Creator.CreateNode();
			Creator.Finalize();
			// The node starts with two outputs; add the rest the way the "Add
			// pin" button does.
			const int32 Outputs = IntOr(Body, TEXT("outputs"), 2);
			for (int32 Index = 2; Index < FMath::Min(Outputs, 32); ++Index)
			{
				Node->AddInputPin();
			}
			return Node;
		}

		if (NodeType.Equals(TEXT("select"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_Select> Creator(*Graph);
			UK2Node_Select* Node = Creator.CreateNode();
			Creator.Finalize();
			return Node;
		}

		if (NodeType.StartsWith(TEXT("switch_"), ESearchCase::IgnoreCase))
		{
			const FString Kind = NodeType.RightChop(7).ToLower();
			if (Kind == TEXT("int") || Kind == TEXT("integer"))
			{
				FGraphNodeCreator<UK2Node_SwitchInteger> Creator(*Graph);
				UK2Node_SwitchInteger* Node = Creator.CreateNode();
				Creator.Finalize();
				return Node;
			}
			if (Kind == TEXT("string"))
			{
				FGraphNodeCreator<UK2Node_SwitchString> Creator(*Graph);
				UK2Node_SwitchString* Node = Creator.CreateNode();
				Creator.Finalize();
				return Node;
			}
			if (Kind == TEXT("name"))
			{
				FGraphNodeCreator<UK2Node_SwitchName> Creator(*Graph);
				UK2Node_SwitchName* Node = Creator.CreateNode();
				Creator.Finalize();
				return Node;
			}
			if (Kind == TEXT("enum"))
			{
				UEnum* Enum = ResolveEnum(StringField(Body, TEXT("enum")));
				if (Enum == nullptr)
				{
					OutError = TEXT("switch_enum needs 'enum' — an enum path or name, e.g. /Script/Engine.ECollisionChannel");
					return nullptr;
				}
				FGraphNodeCreator<UK2Node_SwitchEnum> Creator(*Graph);
				UK2Node_SwitchEnum* Node = Creator.CreateNode();
				// Before Finalize: AllocateDefaultPins builds one case pin per
				// enumerator from this.
				Node->SetEnum(Enum);
				Creator.Finalize();
				return Node;
			}
			OutError = FString::Printf(
				TEXT("unknown switch '%s' — use switch_int, switch_string, switch_name or switch_enum"),
				*NodeType);
			return nullptr;
		}

		if (NodeType.Equals(TEXT("macro"), ESearchCase::IgnoreCase))
		{
			UBlueprint* Library = MacroLibrary(Body);
			if (Library == nullptr)
			{
				OutError = TEXT("no macro library at 'library' — omit it for the engine's standard macros");
				return nullptr;
			}
			const FString MacroName = StringField(Body, TEXT("macro"));
			UEdGraph* MacroGraph = nullptr;
			for (UEdGraph* Candidate : Library->MacroGraphs)
			{
				if (Candidate != nullptr && Candidate->GetName() == MacroName)
				{
					MacroGraph = Candidate;
					break;
				}
			}
			if (MacroGraph == nullptr)
			{
				OutError = FString::Printf(
					TEXT("no macro '%s' in %s — available: %s"),
					*MacroName, *Library->GetName(), *MacroNames(Library));
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_MacroInstance> Creator(*Graph);
			UK2Node_MacroInstance* Node = Creator.CreateNode();
			Node->SetMacroGraph(MacroGraph);
			Creator.Finalize();
			return Node;
		}

		// ----------------------------------------------------------------- casts
		if (NodeType.Equals(TEXT("cast"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("class_cast"), ESearchCase::IgnoreCase))
		{
			UClass* Target = ResolveClass(StringField(Body, TEXT("class")));
			if (Target == nullptr)
			{
				OutError = TEXT("'class' is required — the type to cast to");
				return nullptr;
			}
			const bool bPure = BoolOr(Body, TEXT("pure"), false);
			if (NodeType.Equals(TEXT("class_cast"), ESearchCase::IgnoreCase))
			{
				FGraphNodeCreator<UK2Node_ClassDynamicCast> Creator(*Graph);
				UK2Node_ClassDynamicCast* Node = Creator.CreateNode();
				Node->TargetType = Target;
				Creator.Finalize();
				if (bPure)
				{
					Node->SetPurity(true);
				}
				return Node;
			}
			FGraphNodeCreator<UK2Node_DynamicCast> Creator(*Graph);
			UK2Node_DynamicCast* Node = Creator.CreateNode();
			Node->TargetType = Target;
			Creator.Finalize();
			if (bPure)
			{
				// Reconstructs the node, so it has to come after Finalize.
				Node->SetPurity(true);
			}
			return Node;
		}

		// ------------------------------------------------------- spawning / structs
		if (NodeType.Equals(TEXT("spawn_actor"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_SpawnActorFromClass> Creator(*Graph);
			UK2Node_SpawnActorFromClass* Node = Creator.CreateNode();
			Creator.Finalize();
			const FString ClassSpec = StringField(Body, TEXT("class"));
			if (!ClassSpec.IsEmpty())
			{
				UClass* Spawned = ResolveClass(ClassSpec);
				if (Spawned == nullptr)
				{
					OutError = FString::Printf(TEXT("no class '%s' to spawn"), *ClassSpec);
					return nullptr;
				}
				// Setting the class pin's default is what grows the node's
				// exposed-on-spawn pins.
				if (UEdGraphPin* ClassPin = Node->GetClassPin())
				{
					Graph->GetSchema()->TrySetDefaultObject(*ClassPin, Spawned);
					Node->ReconstructNode();
				}
			}
			return Node;
		}

		if (NodeType.Equals(TEXT("make_struct"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("break_struct"), ESearchCase::IgnoreCase))
		{
			UScriptStruct* Struct = ResolveStruct(StringField(Body, TEXT("struct")));
			if (Struct == nullptr)
			{
				OutError = TEXT("'struct' is required — a struct name or path, e.g. Vector, /Script/Engine.HitResult");
				return nullptr;
			}
			if (NodeType.Equals(TEXT("make_struct"), ESearchCase::IgnoreCase))
			{
				FGraphNodeCreator<UK2Node_MakeStruct> Creator(*Graph);
				UK2Node_MakeStruct* Node = Creator.CreateNode();
				Node->StructType = Struct;
				Creator.Finalize();
				return Node;
			}
			FGraphNodeCreator<UK2Node_BreakStruct> Creator(*Graph);
			UK2Node_BreakStruct* Node = Creator.CreateNode();
			Node->StructType = Struct;
			Creator.Finalize();
			return Node;
		}

		if (NodeType.Equals(TEXT("make_array"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_MakeArray> Creator(*Graph);
			UK2Node_MakeArray* Node = Creator.CreateNode();
			Creator.Finalize();
			for (int32 Index = 1; Index < FMath::Min(IntOr(Body, TEXT("entries"), 1), 64); ++Index)
			{
				Node->AddInputPin();
			}
			return Node;
		}
		if (NodeType.Equals(TEXT("make_set"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_MakeSet> Creator(*Graph);
			UK2Node_MakeSet* Node = Creator.CreateNode();
			Creator.Finalize();
			for (int32 Index = 1; Index < FMath::Min(IntOr(Body, TEXT("entries"), 1), 64); ++Index)
			{
				Node->AddInputPin();
			}
			return Node;
		}
		if (NodeType.Equals(TEXT("make_map"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_MakeMap> Creator(*Graph);
			UK2Node_MakeMap* Node = Creator.CreateNode();
			Creator.Finalize();
			for (int32 Index = 1; Index < FMath::Min(IntOr(Body, TEXT("entries"), 1), 64); ++Index)
			{
				Node->AddInputPin();
			}
			return Node;
		}

		if (NodeType.Equals(TEXT("get_array_item"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_GetArrayItem> Creator(*Graph);
			UK2Node_GetArrayItem* Node = Creator.CreateNode();
			Creator.Finalize();
			return Node;
		}

		if (NodeType.Equals(TEXT("format_text"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_FormatText> Creator(*Graph);
			UK2Node_FormatText* Node = Creator.CreateNode();
			Creator.Finalize();
			const FString Format = StringField(Body, TEXT("format"));
			if (!Format.IsEmpty())
			{
				// Argument pins come from the {tokens} in the format string.
				if (UEdGraphPin* FormatPin = Node->FindPin(TEXT("Format")))
				{
					Graph->GetSchema()->TrySetDefaultValue(*FormatPin, Format, true);
				}
			}
			return Node;
		}

		// -------------------------------------------------------------- literals
		if (NodeType.Equals(TEXT("self"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_Self> Creator(*Graph);
			UK2Node_Self* Node = Creator.CreateNode();
			Creator.Finalize();
			return Node;
		}

		if (NodeType.Equals(TEXT("literal"), ESearchCase::IgnoreCase))
		{
			const FString ObjectPath = StringField(Body, TEXT("object"));
			UObject* Referenced = ResolveObject(ObjectPath);
			if (Referenced == nullptr)
			{
				OutError = FString::Printf(
					TEXT("no object at '%s' — a literal node references an asset or a level actor by path"),
					*ObjectPath);
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_Literal> Creator(*Graph);
			UK2Node_Literal* Node = Creator.CreateNode();
			Creator.Finalize();
			Node->SetObjectRef(Referenced);
			return Node;
		}

		if (NodeType.Equals(TEXT("enum_literal"), ESearchCase::IgnoreCase))
		{
			UEnum* Enum = ResolveEnum(StringField(Body, TEXT("enum")));
			if (Enum == nullptr)
			{
				OutError = TEXT("'enum' is required — an enum path or name");
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_EnumLiteral> Creator(*Graph);
			UK2Node_EnumLiteral* Node = Creator.CreateNode();
			Node->Enum = Enum;
			Creator.Finalize();
			return Node;
		}

		if (NodeType.Equals(TEXT("reroute"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("knot"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UK2Node_Knot> Creator(*Graph);
			UK2Node_Knot* Node = Creator.CreateNode();
			Creator.Finalize();
			return Node;
		}

		if (NodeType.Equals(TEXT("comment"), ESearchCase::IgnoreCase))
		{
			FGraphNodeCreator<UEdGraphNode_Comment> Creator(*Graph);
			UEdGraphNode_Comment* Node = Creator.CreateNode();
			Creator.Finalize();
			// PostPlacedNewNode resets the comment to its default title, so
			// these have to be written after Finalize, unlike every other node.
			Node->NodeComment = StringField(Body, TEXT("text"));
			Node->NodeWidth = IntOr(Body, TEXT("width"), 400);
			Node->NodeHeight = IntOr(Body, TEXT("height"), 200);
			return Node;
		}

		// ------------------------------------------------------------- timelines
		if (NodeType.Equals(TEXT("timeline"), ESearchCase::IgnoreCase))
		{
			FString Name = StringField(Body, TEXT("name"));
			if (Name.IsEmpty())
			{
				Name = TEXT("Timeline");
			}
			if (!FBlueprintEditorUtils::DoesSupportTimelines(Blueprint))
			{
				OutError = TEXT("this Blueprint type does not support timelines (they need an Actor-derived parent)");
				return nullptr;
			}
			const FName TimelineName =
				FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, Name);
			if (FBlueprintEditorUtils::AddNewTimeline(Blueprint, TimelineName) == nullptr)
			{
				OutError = FString::Printf(TEXT("could not add a timeline named '%s'"), *Name);
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_Timeline> Creator(*Graph);
			UK2Node_Timeline* Node = Creator.CreateNode();
			Node->TimelineName = TimelineName;
			Creator.Finalize();
			return Node;
		}

		// ------------------------------------------------------------- delegates
		if (NodeType.Equals(TEXT("call_delegate"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("bind_delegate"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("unbind_delegate"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("clear_delegate"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("assign_delegate"), ESearchCase::IgnoreCase))
		{
			// One creator per concrete class, then the shared reference binding.
			UK2Node_BaseMCDelegate* Node = nullptr;
			if (NodeType.Equals(TEXT("call_delegate"), ESearchCase::IgnoreCase))
			{
				FGraphNodeCreator<UK2Node_CallDelegate> Creator(*Graph);
				UK2Node_CallDelegate* Typed = Creator.CreateNode();
				if (!BindDelegateReference(Typed, Blueprint, Body, OutError)) { return nullptr; }
				Creator.Finalize();
				Node = Typed;
			}
			else if (NodeType.Equals(TEXT("bind_delegate"), ESearchCase::IgnoreCase))
			{
				FGraphNodeCreator<UK2Node_AddDelegate> Creator(*Graph);
				UK2Node_AddDelegate* Typed = Creator.CreateNode();
				if (!BindDelegateReference(Typed, Blueprint, Body, OutError)) { return nullptr; }
				Creator.Finalize();
				Node = Typed;
			}
			else if (NodeType.Equals(TEXT("unbind_delegate"), ESearchCase::IgnoreCase))
			{
				FGraphNodeCreator<UK2Node_RemoveDelegate> Creator(*Graph);
				UK2Node_RemoveDelegate* Typed = Creator.CreateNode();
				if (!BindDelegateReference(Typed, Blueprint, Body, OutError)) { return nullptr; }
				Creator.Finalize();
				Node = Typed;
			}
			else if (NodeType.Equals(TEXT("clear_delegate"), ESearchCase::IgnoreCase))
			{
				FGraphNodeCreator<UK2Node_ClearDelegate> Creator(*Graph);
				UK2Node_ClearDelegate* Typed = Creator.CreateNode();
				if (!BindDelegateReference(Typed, Blueprint, Body, OutError)) { return nullptr; }
				Creator.Finalize();
				Node = Typed;
			}
			else
			{
				FGraphNodeCreator<UK2Node_AssignDelegate> Creator(*Graph);
				UK2Node_AssignDelegate* Typed = Creator.CreateNode();
				if (!BindDelegateReference(Typed, Blueprint, Body, OutError)) { return nullptr; }
				Creator.Finalize();
				Node = Typed;
			}
			return Node;
		}

		OutError = FString::Printf(
			TEXT("unknown node_type '%s' — use call_function, call_parent_function, variable_get, ")
			TEXT("variable_set, event, custom_event, component_bound_event, branch, sequence, select, ")
			TEXT("switch_int, switch_string, switch_name, switch_enum, macro, cast, class_cast, ")
			TEXT("spawn_actor, make_struct, break_struct, make_array, make_set, make_map, ")
			TEXT("get_array_item, format_text, self, literal, enum_literal, reroute, comment, ")
			TEXT("timeline, call_delegate, bind_delegate, unbind_delegate, clear_delegate or assign_delegate"),
			*NodeType);
		return nullptr;
	}
}
