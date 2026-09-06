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

// The gameplay-authoring kinds: input events, object construction, async
// actions, interface messages, data tables, subsystems, promotable operators
// and struct member writes.
#include "Blueprint/UserWidget.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintTypePromotion.h"
#include "Components/ActorComponent.h"
#include "Engine/DataTable.h"
#include "GameFramework/Actor.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "K2Node_AddComponentByClass.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_BaseAsyncTask.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_GenericCreateObject.h"
#include "K2Node_GetDataTableRow.h"
#include "K2Node_GetSubsystem.h"
#include "K2Node_InputAction.h"
#include "K2Node_InputAxisEvent.h"
#include "K2Node_InputKey.h"
#include "K2Node_Message.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_SetFieldsInStruct.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "Subsystems/Subsystem.h"
#include "UObject/UnrealType.h"

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

		/// The class a Construct-Object family node's `class` names, checked
		/// against the base its class pin accepts. Resolved before the node
		/// exists, so a bad class leaves no orphan behind. No `class` is
		/// fine: OutClass stays null and the pin is left for a wire.
		bool ResolveClassPin(
			const TSharedRef<FJsonObject>& Body,
			const UClass* Base,
			const TCHAR* What,
			UClass*& OutClass,
			FString& OutError)
		{
			OutClass = nullptr;
			const FString ClassSpec = StringField(Body, TEXT("class"));
			if (ClassSpec.IsEmpty())
			{
				return true;
			}
			UClass* Class = ResolveClass(ClassSpec);
			if (Class == nullptr)
			{
				OutError = FString::Printf(TEXT("no %s '%s'"), What, *ClassSpec);
				return false;
			}
			if (!Class->IsChildOf(Base))
			{
				OutError = FString::Printf(
					TEXT("'%s' is not a %s — it must derive from %s"), *Class->GetName(), What, *Base->GetName());
				return false;
			}
			OutClass = Class;
			return true;
		}

		/// Setting the class pin's default is what grows the node's
		/// exposed-on-spawn pins.
		void SetClassPin(UK2Node_ConstructObjectFromClass* Node, UEdGraph* Graph, UClass* Class)
		{
			if (Class == nullptr)
			{
				return;
			}
			if (UEdGraphPin* ClassPin = Node->GetClassPin())
			{
				Graph->GetSchema()->TrySetDefaultObject(*ClassPin, Class);
				Node->ReconstructNode();
			}
		}

		/// The Construct Object node's own compile-time rule (a BlueprintType
		/// class that is not abstract, deprecated, an Actor, a component or
		/// marked DontUseGenericSpawnObject), applied now so the refusal
		/// names the node to use instead of arriving as a compile error.
		bool CanConstructObject(const UClass* Class, FString& OutReason)
		{
			if (Class->IsChildOf(AActor::StaticClass()))
			{
				OutReason = TEXT("is an Actor — use spawn_actor");
				return false;
			}
			if (Class->IsChildOf(UActorComponent::StaticClass()))
			{
				OutReason = TEXT("is a component — use add_component_by_class");
				return false;
			}
			if (Class->IsChildOf(UUserWidget::StaticClass()))
			{
				OutReason = TEXT("is a widget — use create_widget");
				return false;
			}
			if (Class->HasAnyClassFlags(CLASS_Abstract))
			{
				OutReason = TEXT("is abstract");
				return false;
			}
			if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				OutReason = TEXT("is deprecated");
				return false;
			}
			static const FName BlueprintTypeName(TEXT("BlueprintType"));
			static const FName NotBlueprintTypeName(TEXT("NotBlueprintType"));
			static const FName DontUseGenericSpawnObjectName(TEXT("DontUseGenericSpawnObject"));
			for (const UClass* Current = Class; Current != nullptr; Current = Current->GetSuperClass())
			{
				if (Current->GetBoolMetaData(NotBlueprintTypeName))
				{
					break;
				}
				if (Current->GetBoolMetaData(BlueprintTypeName))
				{
					if (Current->GetBoolMetaData(DontUseGenericSpawnObjectName))
					{
						OutReason = TEXT("is created through its own function (DontUseGenericSpawnObject) — call that with call_function");
						return false;
					}
					return true;
				}
			}
			OutReason = TEXT("is not a BlueprintType class");
			return false;
		}

		/// The Play Montage node configures its proxy in its constructor; its
		/// class is unexported, so it is spawned by class object.
		UEdGraphNode* CreatePlayMontageNode(UEdGraph* Graph, FString& OutError)
		{
			UClass* NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.K2Node_PlayMontage"));
			if (NodeClass == nullptr)
			{
				OutError = TEXT("the Play Montage node class is not loaded");
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_BaseAsyncTask> Creator(*Graph);
			UK2Node_BaseAsyncTask* Node = Creator.CreateNode(true, NodeClass);
			Creator.Finalize();
			return Node;
		}

		bool SetReflectedName(UObject* Object, const TCHAR* PropertyName, FName Value, FString& OutError)
		{
			FNameProperty* Property = FindFProperty<FNameProperty>(Object->GetClass(), PropertyName);
			if (Property == nullptr)
			{
				OutError = FString::Printf(TEXT("%s has no '%s' property"), *Object->GetClass()->GetName(), PropertyName);
				return false;
			}
			Property->SetPropertyValue_InContainer(Object, Value);
			return true;
		}

		bool SetReflectedObject(UObject* Object, const TCHAR* PropertyName, UObject* Value, FString& OutError)
		{
			FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Object->GetClass(), PropertyName);
			if (Property == nullptr)
			{
				OutError = FString::Printf(TEXT("%s has no '%s' property"), *Object->GetClass()->GetName(), PropertyName);
				return false;
			}
			Property->SetObjectPropertyValue_InContainer(Object, Value);
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
			UClass* Spawned = nullptr;
			if (!ResolveClassPin(Body, AActor::StaticClass(), TEXT("actor class"), Spawned, OutError))
			{
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_SpawnActorFromClass> Creator(*Graph);
			UK2Node_SpawnActorFromClass* Node = Creator.CreateNode();
			Creator.Finalize();
			SetClassPin(Node, Graph, Spawned);
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

		// ------------------------------------------------------------ input events
		if (NodeType.Equals(TEXT("enhanced_input_action"), ESearchCase::IgnoreCase))
		{
			const FString AssetSpec = StringField(Body, TEXT("object"));
			if (AssetSpec.IsEmpty())
			{
				OutError = TEXT("'object' is required — the Input Action asset, e.g. /Game/Input/IA_Jump "
								"(input_asset_ops lists and creates them)");
				return nullptr;
			}
			const UInputAction* Action = Cast<UInputAction>(ResolveAsset(AssetSpec));
			if (Action == nullptr)
			{
				OutError = FString::Printf(
					TEXT("no Input Action asset at '%s' — input_asset_ops lists and creates them"), *AssetSpec);
				return nullptr;
			}
			// The editor places one event node per action and jumps to the
			// existing one after that, so a second is refused the same way a
			// second ReceiveBeginPlay is.
			TArray<UK2Node_EnhancedInputAction*> Existing;
			FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Existing);
			for (const UK2Node_EnhancedInputAction* Node : Existing)
			{
				if (Node->InputAction == Action)
				{
					OutError = FString::Printf(
						TEXT("'%s' already has an event node %s in graph '%s' — a Blueprint holds one per action"),
						*Action->GetName(), *Node->NodeGuid.ToString(), *GraphPath(Node->GetGraph()));
					return nullptr;
				}
			}
			FGraphNodeCreator<UK2Node_EnhancedInputAction> Creator(*Graph);
			UK2Node_EnhancedInputAction* Node = Creator.CreateNode();
			Node->InputAction = Action;
			Creator.Finalize();
			return Node;
		}

		if (NodeType.Equals(TEXT("input_key"), ESearchCase::IgnoreCase))
		{
			const FString KeyName = StringField(Body, TEXT("key"));
			if (KeyName.IsEmpty())
			{
				OutError = TEXT("'key' is required — a key name such as SpaceBar, E, LeftMouseButton or Gamepad_FaceButton_Bottom");
				return nullptr;
			}
			const FKey Key(*KeyName);
			if (!Key.IsValid())
			{
				OutError = FString::Printf(
					TEXT("'%s' is not a key name — use the engine's FKey names (SpaceBar, E, LeftMouseButton, ")
					TEXT("Gamepad_FaceButton_Bottom, ...)"),
					*KeyName);
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_InputKey> Creator(*Graph);
			UK2Node_InputKey* Node = Creator.CreateNode();
			Node->InputKey = Key;
			if (Body->HasField(TEXT("consume_input")))
			{
				Node->bConsumeInput = BoolOr(Body, TEXT("consume_input"), true);
			}
			const TArray<TSharedPtr<FJsonValue>>* Modifiers = nullptr;
			if (Body->TryGetArrayField(TEXT("modifiers"), Modifiers))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Modifiers)
				{
					FString Modifier;
					if (Value.IsValid())
					{
						Value->TryGetString(Modifier);
					}
					if (Modifier.Equals(TEXT("control"), ESearchCase::IgnoreCase) || Modifier.Equals(TEXT("ctrl"), ESearchCase::IgnoreCase))
					{
						Node->bControl = true;
					}
					else if (Modifier.Equals(TEXT("alt"), ESearchCase::IgnoreCase))
					{
						Node->bAlt = true;
					}
					else if (Modifier.Equals(TEXT("shift"), ESearchCase::IgnoreCase))
					{
						Node->bShift = true;
					}
					else if (Modifier.Equals(TEXT("command"), ESearchCase::IgnoreCase) || Modifier.Equals(TEXT("cmd"), ESearchCase::IgnoreCase))
					{
						Node->bCommand = true;
					}
					else
					{
						OutError = FString::Printf(
							TEXT("unknown modifier '%s' — modifiers are control, alt, shift and command"), *Modifier);
						return nullptr;
					}
				}
			}
			Creator.Finalize();
			return Node;
		}

		if (NodeType.Equals(TEXT("input_action"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("input_axis"), ESearchCase::IgnoreCase))
		{
			const bool bAxis = NodeType.Equals(TEXT("input_axis"), ESearchCase::IgnoreCase);
			const FString Name = StringField(Body, TEXT("name"));
			if (Name.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("'name' is required — the legacy %s mapping name from Project Settings > Input, e.g. %s"),
					bAxis ? TEXT("axis") : TEXT("action"), bAxis ? TEXT("MoveForward") : TEXT("Jump"));
				return nullptr;
			}
			if (bAxis)
			{
				FGraphNodeCreator<UK2Node_InputAxisEvent> Creator(*Graph);
				UK2Node_InputAxisEvent* Node = Creator.CreateNode();
				// Names the event function the node compiles to; must precede
				// pin allocation.
				Node->Initialize(FName(*Name));
				if (Body->HasField(TEXT("consume_input")))
				{
					Node->bConsumeInput = BoolOr(Body, TEXT("consume_input"), true);
				}
				Creator.Finalize();
				return Node;
			}
			FGraphNodeCreator<UK2Node_InputAction> Creator(*Graph);
			UK2Node_InputAction* Node = Creator.CreateNode();
			Node->InputActionName = FName(*Name);
			if (Body->HasField(TEXT("consume_input")))
			{
				Node->bConsumeInput = BoolOr(Body, TEXT("consume_input"), true);
			}
			Creator.Finalize();
			return Node;
		}

		// ----------------------------------------------------- object construction
		if (NodeType.Equals(TEXT("construct_object"), ESearchCase::IgnoreCase))
		{
			UClass* Class = nullptr;
			if (!ResolveClassPin(Body, UObject::StaticClass(), TEXT("class"), Class, OutError))
			{
				return nullptr;
			}
			FString Reason;
			if (Class != nullptr && !CanConstructObject(Class, Reason))
			{
				OutError = FString::Printf(TEXT("'%s' %s"), *Class->GetName(), *Reason);
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_GenericCreateObject> Creator(*Graph);
			UK2Node_GenericCreateObject* Node = Creator.CreateNode();
			Creator.Finalize();
			SetClassPin(Node, Graph, Class);
			return Node;
		}

		if (NodeType.Equals(TEXT("add_component_by_class"), ESearchCase::IgnoreCase))
		{
			UClass* Class = nullptr;
			if (!ResolveClassPin(Body, UActorComponent::StaticClass(), TEXT("component class"), Class, OutError))
			{
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_AddComponentByClass> Creator(*Graph);
			UK2Node_AddComponentByClass* Node = Creator.CreateNode();
			Creator.Finalize();
			SetClassPin(Node, Graph, Class);
			return Node;
		}

		if (NodeType.Equals(TEXT("create_widget"), ESearchCase::IgnoreCase))
		{
			UClass* Class = nullptr;
			if (!ResolveClassPin(Body, UUserWidget::StaticClass(), TEXT("widget class"), Class, OutError))
			{
				return nullptr;
			}
			// The node class lives in UMGEditor's private headers; the
			// Construct Object base it derives from is the exported surface.
			UClass* NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/UMGEditor.K2Node_CreateWidget"));
			if (NodeClass == nullptr)
			{
				OutError = TEXT("the UMG editor's Create Widget node class is not loaded");
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_ConstructObjectFromClass> Creator(*Graph);
			UK2Node_ConstructObjectFromClass* Node = Creator.CreateNode(true, NodeClass);
			Creator.Finalize();
			SetClassPin(Node, Graph, Class);
			return Node;
		}

		// ------------------------------------------------------------ async actions
		if (NodeType.Equals(TEXT("async_action"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("latent_action"), ESearchCase::IgnoreCase))
		{
			const FString FunctionName = StringField(Body, TEXT("function"));
			UClass* Owner = TargetClass(Blueprint, Body, OutError);
			if (Owner == nullptr)
			{
				return nullptr;
			}
			UFunction* Factory = Owner->FindFunctionByName(FName(*FunctionName));
			if (Factory == nullptr)
			{
				OutError = FString::Printf(
					TEXT("'%s' has no function '%s' — an async action is created by a static factory function on ")
					TEXT("its proxy class, e.g. class AsyncActionLoadPrimaryAsset, function AsyncLoadPrimaryAsset, ")
					TEXT("or class AbilityTask_WaitGameplayEvent, function WaitGameplayEvent"),
					*Owner->GetName(), *FunctionName);
				return nullptr;
			}
			const FObjectProperty* ReturnProperty = CastField<FObjectProperty>(Factory->GetReturnProperty());
			if (!Factory->HasAnyFunctionFlags(FUNC_Static) || ReturnProperty == nullptr)
			{
				OutError = FString::Printf(
					TEXT("'%s' is not an async action factory — it must be a static function returning the ")
					TEXT("proxy object; a plain latent function (Delay, LoadAsset) is a call_function node"),
					*FunctionName);
				return nullptr;
			}
			UClass* Proxy = ReturnProperty->PropertyClass;

			// Blueprint async actions have a node of their own that reads the
			// factory function directly.
			if (Proxy->IsChildOf(UBlueprintAsyncActionBase::StaticClass()))
			{
				FGraphNodeCreator<UK2Node_AsyncAction> Creator(*Graph);
				UK2Node_AsyncAction* Node = Creator.CreateNode();
				Node->InitializeProxyFromFunction(Factory);
				Creator.Finalize();
				return Node;
			}

			// Play Montage's node configures itself from its constructor.
			const UClass* MontageProxy = FindObject<UClass>(nullptr, TEXT("/Script/AnimGraphRuntime.PlayMontageCallbackProxy"));
			if (MontageProxy != nullptr && Proxy->IsChildOf(MontageProxy))
			{
				return CreatePlayMontageNode(Graph, OutError);
			}

			// Gameplay tasks (ability tasks included) use the latent task-call
			// nodes, whose classes export nothing: the proxy fields are the
			// same protected UPROPERTYs the engine's own menu spawner fills
			// in, so they are set by reflection.
			const UClass* GameplayTask = FindObject<UClass>(nullptr, TEXT("/Script/GameplayTasks.GameplayTask"));
			if (GameplayTask != nullptr && Proxy->IsChildOf(GameplayTask))
			{
				UClass* NodeClass = nullptr;
				const UClass* AbilityTask = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.AbilityTask"));
				if (AbilityTask != nullptr && Proxy->IsChildOf(AbilityTask))
				{
					NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilitiesEditor.K2Node_LatentAbilityCall"));
				}
				if (NodeClass == nullptr)
				{
					NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayTasksEditor.K2Node_LatentGameplayTaskCall"));
				}
				if (NodeClass == nullptr)
				{
					OutError = TEXT("the gameplay task node classes are not loaded — enable the GameplayTasks (and GameplayAbilities) plugins");
					return nullptr;
				}
				FGraphNodeCreator<UK2Node_BaseAsyncTask> Creator(*Graph);
				UK2Node_BaseAsyncTask* Node = Creator.CreateNode(true, NodeClass);
				if (!SetReflectedName(Node, TEXT("ProxyFactoryFunctionName"), Factory->GetFName(), OutError)
					|| !SetReflectedObject(Node, TEXT("ProxyFactoryClass"), Owner, OutError)
					|| !SetReflectedObject(Node, TEXT("ProxyClass"), Proxy, OutError))
				{
					return nullptr;
				}
				Creator.Finalize();
				return Node;
			}

			OutError = FString::Printf(
				TEXT("'%s' returns a %s, which is not an async action proxy — expected a BlueprintAsyncActionBase, ")
				TEXT("a GameplayTask (ability tasks included) or the Play Montage proxy"),
				*FunctionName, *Proxy->GetName());
			return nullptr;
		}

		if (NodeType.Equals(TEXT("play_montage"), ESearchCase::IgnoreCase))
		{
			return CreatePlayMontageNode(Graph, OutError);
		}

		// -------------------------------------------------------------- interfaces
		if (NodeType.Equals(TEXT("interface_message"), ESearchCase::IgnoreCase))
		{
			const FString FunctionName = StringField(Body, TEXT("function"));
			UClass* Interface = TargetClass(Blueprint, Body, OutError);
			if (Interface == nullptr)
			{
				return nullptr;
			}
			if (!Interface->HasAnyClassFlags(CLASS_Interface))
			{
				OutError = FString::Printf(
					TEXT("'%s' is not an interface — pass a Blueprint Interface asset path or a UInterface class as 'class'"),
					*Interface->GetName());
				return nullptr;
			}
			UFunction* Function = Interface->FindFunctionByName(FName(*FunctionName));
			if (Function == nullptr)
			{
				TArray<FString> Names;
				for (TFieldIterator<UFunction> It(Interface, EFieldIteratorFlags::ExcludeSuper); It; ++It)
				{
					Names.Add(It->GetName());
				}
				OutError = FString::Printf(
					TEXT("interface '%s' has no function '%s' — it has: %s"),
					*Interface->GetName(), *FunctionName,
					Names.IsEmpty() ? TEXT("(none)") : *FString::Join(Names, TEXT(", ")));
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_Message> Creator(*Graph);
			UK2Node_Message* Node = Creator.CreateNode();
			Node->SetFromFunction(Function);
			Creator.Finalize();
			return Node;
		}

		// ------------------------------------------------------- data and services
		if (NodeType.Equals(TEXT("get_data_table_row"), ESearchCase::IgnoreCase))
		{
			UDataTable* Table = nullptr;
			const FString TableSpec = StringField(Body, TEXT("object"));
			if (!TableSpec.IsEmpty())
			{
				Table = Cast<UDataTable>(ResolveAsset(TableSpec));
				if (Table == nullptr)
				{
					OutError = FString::Printf(
						TEXT("no DataTable asset at '%s' — data_table_ops lists and creates them"), *TableSpec);
					return nullptr;
				}
			}
			// Checked before the node exists, so a bad row leaves no orphan.
			const FString RowName = StringField(Body, TEXT("name"));
			if (!RowName.IsEmpty())
			{
				if (Table == nullptr)
				{
					OutError = TEXT("'name' (the row) needs 'object' (the DataTable it lives in)");
					return nullptr;
				}
				if (Table->GetRowMap().Find(FName(*RowName)) == nullptr)
				{
					OutError = FString::Printf(
						TEXT("'%s' has no row '%s' — data_table_ops get_rows names them"),
						*Table->GetName(), *RowName);
					return nullptr;
				}
			}
			FGraphNodeCreator<UK2Node_GetDataTableRow> Creator(*Graph);
			UK2Node_GetDataTableRow* Node = Creator.CreateNode();
			Creator.Finalize();
			if (Table != nullptr)
			{
				// The table pin's default is what types the output row struct
				// and offers the row names.
				if (UEdGraphPin* TablePin = Node->GetDataTablePin())
				{
					Graph->GetSchema()->TrySetDefaultObject(*TablePin, Table);
					Node->ReconstructNode();
				}
				if (!RowName.IsEmpty())
				{
					if (UEdGraphPin* RowPin = Node->GetRowNamePin())
					{
						Graph->GetSchema()->TrySetDefaultValue(*RowPin, RowName);
					}
				}
			}
			return Node;
		}

		if (NodeType.Equals(TEXT("get_subsystem"), ESearchCase::IgnoreCase))
		{
			const FString ClassSpec = StringField(Body, TEXT("class"));
			UClass* SubsystemClass = ClassSpec.IsEmpty() ? nullptr : ResolveClass(ClassSpec);
			if (SubsystemClass == nullptr || !SubsystemClass->IsChildOf(USubsystem::StaticClass()))
			{
				OutError = ClassSpec.IsEmpty()
					? FString(TEXT("'class' is required — the subsystem class, e.g. EnhancedInputLocalPlayerSubsystem; subsystem_query lists the live ones"))
					: FString::Printf(TEXT("'%s' is not a subsystem class — subsystem_query lists the live ones"), *ClassSpec);
				return nullptr;
			}
			// Engine and editor subsystems have nodes of their own; local
			// player subsystems can be fetched off a player controller.
			TSubclassOf<UK2Node_GetSubsystem> NodeClass = UK2Node_GetSubsystem::StaticClass();
			const UClass* EditorSubsystem = FindObject<UClass>(nullptr, TEXT("/Script/EditorSubsystem.EditorSubsystem"));
			if (SubsystemClass->IsChildOf(UEngineSubsystem::StaticClass()))
			{
				NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BlueprintGraph.K2Node_GetEngineSubsystem"));
			}
			else if (EditorSubsystem != nullptr && SubsystemClass->IsChildOf(EditorSubsystem))
			{
				NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BlueprintGraph.K2Node_GetEditorSubsystem"));
			}
			else if (SubsystemClass->IsChildOf(ULocalPlayerSubsystem::StaticClass())
				&& BoolOr(Body, TEXT("player_controller"), false))
			{
				NodeClass = FindObject<UClass>(nullptr, TEXT("/Script/BlueprintGraph.K2Node_GetSubsystemFromPC"));
			}
			if (NodeClass.Get() == nullptr)
			{
				OutError = TEXT("the subsystem node class is not loaded");
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_GetSubsystem> Creator(*Graph);
			UK2Node_GetSubsystem* Node = Creator.CreateNode(true, NodeClass);
			Node->Initialize(SubsystemClass);
			Creator.Finalize();
			return Node;
		}

		// ---------------------------------------------------- operators and structs
		if (NodeType.Equals(TEXT("operator"), ESearchCase::IgnoreCase)
			|| NodeType.Equals(TEXT("promotable_operator"), ESearchCase::IgnoreCase))
		{
			TArray<FName> OpNames = FTypePromotion::GetAllOpNames().Array();
			OpNames.Sort(FNameLexicalLess());
			TArray<FString> OpList;
			for (const FName& Name : OpNames)
			{
				OpList.Add(Name.ToString());
			}
			const FString OpSpec = StringField(Body, TEXT("operator"));
			FName Op = NAME_None;
			for (const FName& Name : OpNames)
			{
				if (Name.ToString().Equals(OpSpec, ESearchCase::IgnoreCase))
				{
					Op = Name;
				}
			}
			if (Op.IsNone())
			{
				OutError = OpSpec.IsEmpty()
					? FString::Printf(TEXT("'operator' is required — one of: %s"), *FString::Join(OpList, TEXT(", ")))
					: FString::Printf(TEXT("no promotable operator '%s' — one of: %s"), *OpSpec, *FString::Join(OpList, TEXT(", ")));
				return nullptr;
			}
			// The node goes wildcard and promotes to whatever is wired, so
			// the starting function only decides the title: the all-double
			// overload reads as "Multiply", the palette's registered entry
			// can be any overload ("Seconds * FrameRate").
			TArray<UFunction*> Candidates;
			FTypePromotion::GetAllFuncsForOp(Op, Candidates);
			const UFunction* Function = nullptr;
			for (const UFunction* Candidate : Candidates)
			{
				bool bAllDouble = true;
				for (TFieldIterator<FProperty> It(Candidate); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					if (!It->HasAnyPropertyFlags(CPF_ReturnParm) && !It->IsA<FDoubleProperty>())
					{
						bAllDouble = false;
					}
				}
				if (bAllDouble)
				{
					Function = Candidate;
					break;
				}
			}
			if (Function == nullptr)
			{
				if (const UBlueprintFunctionNodeSpawner* Spawner = FTypePromotion::GetOperatorSpawner(Op))
				{
					Function = Spawner->GetFunction();
				}
			}
			if (Function == nullptr && !Candidates.IsEmpty())
			{
				Function = Candidates[0];
			}
			if (Function == nullptr)
			{
				OutError = FString::Printf(TEXT("no operator function registered for '%s'"), *Op.ToString());
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_PromotableOperator> Creator(*Graph);
			UK2Node_PromotableOperator* Node = Creator.CreateNode();
			// The override that records the operation name is unexported;
			// the exported base entry point dispatches to it.
			static_cast<UK2Node_CallFunction*>(Node)->SetFromFunction(Function);
			Creator.Finalize();
			return Node;
		}

		if (NodeType.Equals(TEXT("set_fields_in_struct"), ESearchCase::IgnoreCase))
		{
			UScriptStruct* Struct = ResolveStruct(StringField(Body, TEXT("struct")));
			if (Struct == nullptr)
			{
				OutError = TEXT("'struct' is required — a struct name or path, e.g. Vector, /Script/Engine.HitResult");
				return nullptr;
			}
			FGraphNodeCreator<UK2Node_SetFieldsInStruct> Creator(*Graph);
			UK2Node_SetFieldsInStruct* Node = Creator.CreateNode();
			Node->StructType = Struct;
			Creator.Finalize();
			return Node;
		}

		OutError = FString::Printf(
			TEXT("unknown node_type '%s' — use call_function, call_parent_function, variable_get, ")
			TEXT("variable_set, event, custom_event, component_bound_event, enhanced_input_action, ")
			TEXT("input_key, input_action, input_axis, branch, sequence, select, ")
			TEXT("switch_int, switch_string, switch_name, switch_enum, macro, cast, class_cast, ")
			TEXT("spawn_actor, construct_object, add_component_by_class, create_widget, async_action, ")
			TEXT("play_montage, interface_message, get_data_table_row, get_subsystem, operator, ")
			TEXT("make_struct, break_struct, set_fields_in_struct, make_array, make_set, make_map, ")
			TEXT("get_array_item, format_text, self, literal, enum_literal, reroute, comment, ")
			TEXT("timeline, call_delegate, bind_delegate, unbind_delegate, clear_delegate or assign_delegate"),
			*NodeType);
		return nullptr;
	}
}
