// The add_node vocabulary: every node kind spawns configured, not empty.
//
// The point of each check is the *configuration*: a MakeStruct with no struct
// or a SwitchEnum with no enum both construct fine and then have no pins, which
// is exactly the silent failure this route exists to avoid.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "K2Node_AddComponentByClass.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_ConstructObjectFromClass.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_GenericCreateObject.h"
#include "K2Node_GetSubsystem.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_InputAxisEvent.h"
#include "K2Node_InputKey.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_Self.h"
#include "K2Node_SetFieldsInStruct.h"
#include "K2Node_SwitchEnum.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "McpBlueprintUtils.h"
#include "McpTestFlags.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
	/// A throwaway Actor Blueprint under /Temp. Nothing here is ever saved, so
	/// the package never reaches the content folder.
	UBlueprint* MakeScratchBlueprint()
	{
		const FString PackageName = FString::Printf(
			TEXT("/Temp/McpLinkTests/BP_Nodes_%s"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		UPackage* Package = CreatePackage(*PackageName);
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(*FPackageName::GetShortName(PackageName)),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	}

	TSharedRef<FJsonObject> Request(const TCHAR* NodeType)
	{
		const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("node_type"), NodeType);
		return Body;
	}

	int32 CountPins(const UEdGraphNode* Node, EEdGraphPinDirection Direction, FName Category)
	{
		int32 Count = 0;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && Pin->Direction == Direction && Pin->PinType.PinCategory == Category)
			{
				++Count;
			}
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpNodeVocabularyTest, "McpLink.Blueprint.NodeVocabulary", McpTestFlags)
bool FMcpNodeVocabularyTest::RunTest(const FString& Parameters)
{
	using namespace McpLink;

	UBlueprint* Blueprint = MakeScratchBlueprint();
	if (!TestNotNull(TEXT("scratch blueprint created"), Blueprint))
	{
		return false;
	}
	UEdGraph* Graph = FindGraph(Blueprint, FString());
	if (!TestNotNull(TEXT("event graph found"), Graph))
	{
		return false;
	}

	FString Error;

	// ---- branch: two exec outputs and a bool condition ----
	{
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Request(TEXT("branch")), Error);
		if (TestNotNull(TEXT("branch created"), Node))
		{
			TestTrue(TEXT("branch is an IfThenElse"), Node->IsA<UK2Node_IfThenElse>());
			TestEqual(TEXT("branch has two exec outputs"),
				CountPins(Node, EGPD_Output, UEdGraphSchema_K2::PC_Exec), 2);
		}
	}

	// ---- sequence: the extra exec outputs the "Add pin" button makes ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("sequence"));
		Body->SetNumberField(TEXT("outputs"), 4);
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("sequence created"), Node))
		{
			TestTrue(TEXT("sequence is an ExecutionSequence"), Node->IsA<UK2Node_ExecutionSequence>());
			TestEqual(TEXT("sequence grew to four outputs"),
				CountPins(Node, EGPD_Output, UEdGraphSchema_K2::PC_Exec), 4);
		}
	}

	// ---- cast: the target type is what gives the node its output pin ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("cast"));
		Body->SetStringField(TEXT("class"), TEXT("/Script/Engine.Pawn"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("cast created"), Node))
		{
			UK2Node_DynamicCast* Cast = CastChecked<UK2Node_DynamicCast>(Node);
			TestEqual(TEXT("cast targets Pawn"), Cast->TargetType.Get(), APawn::StaticClass());
			TestNotNull(TEXT("cast has a result pin"), Cast->GetCastResultPin());
		}
	}

	// ---- make_struct: one input pin per struct member ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("make_struct"));
		Body->SetStringField(TEXT("struct"), TEXT("Vector"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("make_struct created"), Node))
		{
			UK2Node_MakeStruct* Make = CastChecked<UK2Node_MakeStruct>(Node);
			TestNotNull(TEXT("make_struct knows its struct"), Make->StructType.Get());
			TestTrue(TEXT("make_struct got member pins"), Make->Pins.Num() > 1);
		}
	}
	{
		// The failure this whole "configure before Finalize" rule exists for.
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Request(TEXT("make_struct")), Error);
		TestNull(TEXT("make_struct without a struct is refused"), Node);
		TestTrue(TEXT("and says which field is missing"), Error.Contains(TEXT("struct")));
	}

	// ---- switch_enum: one case pin per enumerator ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("switch_enum"));
		Body->SetStringField(TEXT("enum"), TEXT("/Script/Engine.ECollisionChannel"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("switch_enum created"), Node))
		{
			UK2Node_SwitchEnum* Switch = CastChecked<UK2Node_SwitchEnum>(Node);
			TestNotNull(TEXT("switch_enum knows its enum"), Switch->Enum.Get());
			TestTrue(TEXT("switch_enum got case pins"),
				CountPins(Switch, EGPD_Output, UEdGraphSchema_K2::PC_Exec) > 2);
		}
	}

	// ---- macro instance from the engine's standard library ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("macro"));
		Body->SetStringField(TEXT("macro"), TEXT("ForEachLoop"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("ForEachLoop macro created"), Node))
		{
			UK2Node_MacroInstance* Macro = CastChecked<UK2Node_MacroInstance>(Node);
			TestNotNull(TEXT("macro graph bound"), Macro->GetMacroGraph());
			TestTrue(TEXT("macro exposes its tunnel pins"), Macro->Pins.Num() > 1);
		}
	}
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("macro"));
		Body->SetStringField(TEXT("macro"), TEXT("NoSuchMacro"));
		TestNull(TEXT("unknown macro refused"),
			CreateGraphNode(Blueprint, Graph, Body, Error));
		// The error is the discovery mechanism, so it must list the real names.
		TestTrue(TEXT("error lists the available macros"), Error.Contains(TEXT("ForEachLoop")));
	}

	// ---- comment ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("comment"));
		Body->SetStringField(TEXT("text"), TEXT("Locomotion"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("comment created"), Node))
		{
			TestEqual(TEXT("comment text kept"),
				CastChecked<UEdGraphNode_Comment>(Node)->NodeComment, FString(TEXT("Locomotion")));
		}
	}

	// ---- self ----
	{
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Request(TEXT("self")), Error);
		if (TestNotNull(TEXT("self created"), Node))
		{
			TestTrue(TEXT("self is a Self node"), Node->IsA<UK2Node_Self>());
		}
	}

	// ---- call_function against another class ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("call_function"));
		Body->SetStringField(TEXT("class"), TEXT("/Script/Engine.KismetSystemLibrary"));
		Body->SetStringField(TEXT("function"), TEXT("PrintString"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("call_function created"), Node))
		{
			TestNotNull(TEXT("PrintString's InString pin exists"), Node->FindPin(TEXT("InString")));
		}
	}
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("call_function"));
		Body->SetStringField(TEXT("class"), TEXT("/Script/Engine.KismetSystemLibrary"));
		Body->SetStringField(TEXT("function"), TEXT("NoSuchFunction"));
		TestNull(TEXT("missing function refused"),
			CreateGraphNode(Blueprint, Graph, Body, Error));
		TestTrue(TEXT("error names the class"), Error.Contains(TEXT("KismetSystemLibrary")));
	}

	// ---- input_key: a key event with pressed and released exec outputs ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("input_key"));
		Body->SetStringField(TEXT("key"), TEXT("SpaceBar"));
		TArray<TSharedPtr<FJsonValue>> Modifiers;
		Modifiers.Add(MakeShared<FJsonValueString>(TEXT("shift")));
		Body->SetArrayField(TEXT("modifiers"), Modifiers);
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("input_key created"), Node))
		{
			TestTrue(TEXT("input_key is an InputKey node"), Node->IsA<UK2Node_InputKey>());
			TestEqual(TEXT("input_key has Pressed and Released"),
				CountPins(Node, EGPD_Output, UEdGraphSchema_K2::PC_Exec), 2);
			TestTrue(TEXT("input_key took the shift modifier"),
				Cast<UK2Node_InputKey>(Node)->bShift != 0);
		}
	}
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("input_key"));
		Body->SetStringField(TEXT("key"), TEXT("NotAKeyAnyoneHas"));
		TestNull(TEXT("unknown key refused"), CreateGraphNode(Blueprint, Graph, Body, Error));
	}

	// ---- input_axis: the legacy axis event carries its value pin ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("input_axis"));
		Body->SetStringField(TEXT("name"), TEXT("MoveForward"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("input_axis created"), Node))
		{
			TestTrue(TEXT("input_axis is an InputAxisEvent node"), Node->IsA<UK2Node_InputAxisEvent>());
			TestNotNull(TEXT("input_axis has AxisValue"), Node->FindPin(TEXT("AxisValue")));
		}
	}

	// ---- get_subsystem: the result pin is typed to the subsystem ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("get_subsystem"));
		Body->SetStringField(TEXT("class"), TEXT("/Script/EnhancedInput.EnhancedInputLocalPlayerSubsystem"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("get_subsystem created"), Node))
		{
			TestTrue(TEXT("get_subsystem is a GetSubsystem node"), Node->IsA<UK2Node_GetSubsystem>());
			const UEdGraphPin* Result = Node->FindPin(UEdGraphSchema_K2::PN_ReturnValue);
			if (TestNotNull(TEXT("get_subsystem has a result pin"), Result))
			{
				TestEqual(TEXT("result pin is the subsystem class"),
					Result->PinType.PinSubCategoryObject.IsValid()
						? Result->PinType.PinSubCategoryObject->GetName()
						: FString(),
					FString(TEXT("EnhancedInputLocalPlayerSubsystem")));
			}
		}
	}
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("get_subsystem"));
		Body->SetStringField(TEXT("class"), TEXT("Actor"));
		TestNull(TEXT("non-subsystem class refused"), CreateGraphNode(Blueprint, Graph, Body, Error));
	}

	// ---- operator: a promotable operator starts wildcard ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("operator"));
		Body->SetStringField(TEXT("operator"), TEXT("Add"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("operator created"), Node))
		{
			TestTrue(TEXT("operator is a PromotableOperator node"), Node->IsA<UK2Node_PromotableOperator>());
			const UEdGraphPin* A = Node->FindPin(TEXT("A"));
			if (TestNotNull(TEXT("operator has pin A"), A))
			{
				TestEqual(TEXT("pin A is wildcard"), A->PinType.PinCategory, UEdGraphSchema_K2::PC_Wildcard);
			}
		}
	}
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("operator"));
		Body->SetStringField(TEXT("operator"), TEXT("Teleport"));
		TestNull(TEXT("unknown operator refused"), CreateGraphNode(Blueprint, Graph, Body, Error));
		TestTrue(TEXT("error lists the operators"), Error.Contains(TEXT("Add")));
	}

	// ---- set_fields_in_struct: a struct reference input ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("set_fields_in_struct"));
		Body->SetStringField(TEXT("struct"), TEXT("HitResult"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("set_fields_in_struct created"), Node))
		{
			TestTrue(TEXT("set_fields_in_struct is a SetFieldsInStruct node"), Node->IsA<UK2Node_SetFieldsInStruct>());
			TestTrue(TEXT("set_fields_in_struct has a struct input"),
				CountPins(Node, EGPD_Input, UEdGraphSchema_K2::PC_Struct) >= 1);
		}
	}

	// ---- construct_object / add_component_by_class / create_widget: the class pin grows the node ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("construct_object"));
		Body->SetStringField(TEXT("class"), TEXT("/Script/Engine.CurveFloat"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(*FString::Printf(TEXT("construct_object created (%s)"), *Error), Node))
		{
			TestTrue(TEXT("construct_object is a GenericCreateObject node"), Node->IsA<UK2Node_GenericCreateObject>());
			const UEdGraphPin* ClassPin = Cast<UK2Node_ConstructObjectFromClass>(Node)->GetClassPin();
			TestEqual(TEXT("construct_object class pin is CurveFloat"),
				ClassPin && ClassPin->DefaultObject ? ClassPin->DefaultObject->GetName() : FString(),
				FString(TEXT("CurveFloat")));
		}
	}
	{
		// What the compiler would refuse is refused up front, naming the node to use.
		const TSharedRef<FJsonObject> Body = Request(TEXT("construct_object"));
		Body->SetStringField(TEXT("class"), TEXT("StaticMeshComponent"));
		const int32 Before = Graph->Nodes.Num();
		TestNull(TEXT("construct_object refuses a component"), CreateGraphNode(Blueprint, Graph, Body, Error));
		TestTrue(TEXT("error points at add_component_by_class"), Error.Contains(TEXT("add_component_by_class")));
		TestEqual(TEXT("no orphan node left behind"), Graph->Nodes.Num(), Before);
	}
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("add_component_by_class"));
		Body->SetStringField(TEXT("class"), TEXT("StaticMeshComponent"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("add_component_by_class created"), Node))
		{
			TestTrue(TEXT("add_component_by_class is an AddComponentByClass node"),
				Node->IsA<UK2Node_AddComponentByClass>());
			TestNotNull(TEXT("add_component_by_class has RelativeTransform"),
				Node->FindPin(TEXT("RelativeTransform")));
		}
	}
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("add_component_by_class"));
		Body->SetStringField(TEXT("class"), TEXT("Actor"));
		TestNull(TEXT("non-component class refused"), CreateGraphNode(Blueprint, Graph, Body, Error));
	}
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("create_widget"));
		Body->SetStringField(TEXT("class"), TEXT("UserWidget"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("create_widget created"), Node))
		{
			TestEqual(TEXT("create_widget is the UMG editor's node"),
				Node->GetClass()->GetName(), FString(TEXT("K2Node_CreateWidget")));
			TestNotNull(TEXT("create_widget has OwningPlayer"), Node->FindPin(TEXT("OwningPlayer")));
		}
	}

	// ---- async_action: the factory's proxy delegates become exec outputs ----
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("async_action"));
		Body->SetStringField(TEXT("class"), TEXT("/Script/Engine.AsyncActionLoadPrimaryAsset"));
		Body->SetStringField(TEXT("function"), TEXT("AsyncLoadPrimaryAsset"));
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Body, Error);
		if (TestNotNull(TEXT("async_action created"), Node))
		{
			TestTrue(TEXT("async_action is an AsyncAction node"), Node->IsA<UK2Node_AsyncAction>());
			TestNotNull(TEXT("async_action has the Completed output"), Node->FindPin(TEXT("Completed")));
		}
	}
	{
		const TSharedRef<FJsonObject> Body = Request(TEXT("async_action"));
		Body->SetStringField(TEXT("class"), TEXT("/Script/Engine.KismetSystemLibrary"));
		Body->SetStringField(TEXT("function"), TEXT("Delay"));
		TestNull(TEXT("a plain latent function is not an async action"),
			CreateGraphNode(Blueprint, Graph, Body, Error));
		TestTrue(TEXT("error points at call_function"), Error.Contains(TEXT("call_function")));
	}
	{
		UEdGraphNode* Node = CreateGraphNode(Blueprint, Graph, Request(TEXT("play_montage")), Error);
		if (TestNotNull(TEXT("play_montage created"), Node))
		{
			TestEqual(TEXT("play_montage is the AnimGraph node"),
				Node->GetClass()->GetName(), FString(TEXT("K2Node_PlayMontage")));
			TestNotNull(TEXT("play_montage has MontageToPlay"), Node->FindPin(TEXT("MontageToPlay")));
		}
	}

	// ---- unknown node type lists the vocabulary ----
	{
		TestNull(TEXT("unknown node type refused"),
			CreateGraphNode(Blueprint, Graph, Request(TEXT("teleporter")), Error));
		TestTrue(TEXT("error lists node types"), Error.Contains(TEXT("call_function")));
	}

	return true;
}

#endif
