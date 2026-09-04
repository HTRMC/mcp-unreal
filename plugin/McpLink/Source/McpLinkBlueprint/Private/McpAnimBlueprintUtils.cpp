#include "McpAnimBlueprintUtils.h"

#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationTransitionGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SkeletalMesh.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "McpBlueprintUtils.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace McpLink::Anim
{
	namespace
	{
		/// The pose input of the graph's sink: the Output Pose node of an
		/// animation graph, or the result node of a state graph.
		UEdGraphPin* FindResultInputPin(UEdGraph* Graph)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr
					&& (Node->IsA<UAnimGraphNode_Root>() || Node->IsA<UAnimGraphNode_StateResult>()))
				{
					return Node->FindPin(TEXT("Result"), EGPD_Input);
				}
			}
			return nullptr;
		}

		UEdGraphPin* FirstOutputPin(UEdGraphNode* Node)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin != nullptr && Pin->Direction == EGPD_Output)
				{
					return Pin;
				}
			}
			return nullptr;
		}

		bool Connect(UEdGraph* Graph, UEdGraphPin* From, UEdGraphPin* To, FString& OutError)
		{
			if (From == nullptr || To == nullptr)
			{
				OutError = TEXT("pin not found");
				return false;
			}
			if (From->LinkedTo.Contains(To))
			{
				return true;
			}
			const UEdGraphSchema* Schema = Graph->GetSchema();
			const FPinConnectionResponse Response = Schema->CanCreateConnection(From, To);
			if (Response.Response == CONNECT_RESPONSE_DISALLOW)
			{
				OutError = Response.Message.ToString();
				return false;
			}
			if (!Schema->TryCreateConnection(From, To))
			{
				OutError = TEXT("the schema refused the connection");
				return false;
			}
			return true;
		}

		FString StateNameOf(const UAnimStateNodeBase* Node)
		{
			return Node != nullptr ? Node->GetStateName() : FString();
		}
	}

	UAnimBlueprint* CreateAnimBlueprint(
		const FString& PackagePath,
		USkeleton* Skeleton,
		UClass* ParentClass,
		USkeletalMesh* PreviewMesh,
		FString& OutError)
	{
		if (Skeleton == nullptr)
		{
			OutError = TEXT("a skeleton is required");
			return nullptr;
		}
		if (ParentClass == nullptr)
		{
			ParentClass = UAnimInstance::StaticClass();
		}
		if (!ParentClass->IsChildOf(UAnimInstance::StaticClass())
			|| !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a usable AnimInstance subclass"), *ParentClass->GetName());
			return nullptr;
		}

		const FString AssetName = FPackageName::GetShortName(PackagePath);
		UPackage* Package = CreatePackage(*PackagePath);
		// The factory passes the plain generated-class type; CreateBlueprint
		// picks the anim-specific one from the Blueprint class.
		UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
			ParentClass, Package, FName(*AssetName), BPTYPE_Normal,
			UAnimBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
		if (AnimBlueprint == nullptr)
		{
			OutError = TEXT("CreateBlueprint returned null");
			return nullptr;
		}

		AnimBlueprint->bIsTemplate = false;
		AnimBlueprint->TargetSkeleton = Skeleton;
		if (UAnimBlueprintGeneratedClass* Generated =
			Cast<UAnimBlueprintGeneratedClass>(AnimBlueprint->GeneratedClass))
		{
			Generated->TargetSkeleton = Skeleton;
		}
		if (UAnimBlueprintGeneratedClass* SkeletonClass =
			Cast<UAnimBlueprintGeneratedClass>(AnimBlueprint->SkeletonGeneratedClass))
		{
			SkeletonClass->TargetSkeleton = Skeleton;
		}
		if (PreviewMesh != nullptr)
		{
			AnimBlueprint->SetPreviewMesh(PreviewMesh);
		}

		// CreateBlueprint adds the AnimGraph for anim Blueprints; be defensive.
		if (FindAnimGraph(AnimBlueprint, FString()) == nullptr)
		{
			UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
				AnimBlueprint, UEdGraphSchema_K2::GN_AnimGraph,
				UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(
				AnimBlueprint, Graph, /*bIsUserCreated*/ false, nullptr);
		}

		FAssetRegistryModule::AssetCreated(AnimBlueprint);
		Package->MarkPackageDirty();
		return AnimBlueprint;
	}

	UAnimationGraph* FindAnimGraph(UBlueprint* Blueprint, const FString& GraphName)
	{
		const FString Name =
			GraphName.IsEmpty() ? UEdGraphSchema_K2::GN_AnimGraph.ToString() : GraphName;
		return Cast<UAnimationGraph>(FindGraph(Blueprint, Name));
	}

	void CollectStateMachines(UBlueprint* Blueprint, TArray<UAnimationStateMachineGraph*>& Out)
	{
		TArray<FMcpGraphEntry> Graphs;
		CollectGraphs(Blueprint, Graphs);
		for (const FMcpGraphEntry& Entry : Graphs)
		{
			if (UAnimationStateMachineGraph* Machine = Cast<UAnimationStateMachineGraph>(Entry.Graph))
			{
				Out.Add(Machine);
			}
		}
	}

	UAnimationStateMachineGraph* FindStateMachine(
		UBlueprint* Blueprint, const FString& NameOrPath, FString& OutError)
	{
		TArray<UAnimationStateMachineGraph*> Machines;
		CollectStateMachines(Blueprint, Machines);
		if (NameOrPath.IsEmpty())
		{
			if (Machines.Num() == 1)
			{
				return Machines[0];
			}
			OutError = Machines.IsEmpty()
				? TEXT("this Blueprint has no state machine — add one with add_state_machine")
				: TEXT("this Blueprint has several state machines — pass 'state_machine'");
			return nullptr;
		}
		if (UAnimationStateMachineGraph* Machine =
			Cast<UAnimationStateMachineGraph>(FindGraph(Blueprint, NameOrPath)))
		{
			return Machine;
		}
		// Also accept the owning node's GUID.
		for (UAnimationStateMachineGraph* Machine : Machines)
		{
			if (Machine->OwnerAnimGraphNode != nullptr
				&& Machine->OwnerAnimGraphNode->NodeGuid.ToString() == NameOrPath)
			{
				return Machine;
			}
		}
		TArray<FString> Names;
		for (UAnimationStateMachineGraph* Machine : Machines)
		{
			Names.Add(Machine->GetName());
		}
		OutError = FString::Printf(TEXT("no state machine '%s' — available: %s"), *NameOrPath,
			Names.IsEmpty() ? TEXT("(none)") : *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	UAnimStateNode* FindState(UAnimationStateMachineGraph* Machine, const FString& NameOrGuid)
	{
		if (Machine == nullptr)
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Machine->Nodes)
		{
			UAnimStateNode* State = Cast<UAnimStateNode>(Node);
			if (State != nullptr
				&& (State->GetStateName() == NameOrGuid || State->NodeGuid.ToString() == NameOrGuid))
			{
				return State;
			}
		}
		return nullptr;
	}

	UAnimStateTransitionNode* FindTransition(UAnimationStateMachineGraph* Machine, const FString& Guid)
	{
		if (Machine == nullptr)
		{
			return nullptr;
		}
		for (UEdGraphNode* Node : Machine->Nodes)
		{
			UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node);
			if (Transition != nullptr && Transition->NodeGuid.ToString() == Guid)
			{
				return Transition;
			}
		}
		return nullptr;
	}

	UAnimGraphNode_StateMachine* AddStateMachine(
		UAnimationGraph* Graph, const FString& Name, bool bConnectToResult, int32 X, int32 Y, FString& OutError)
	{
		Graph->Modify();
		FGraphNodeCreator<UAnimGraphNode_StateMachine> Creator(*Graph);
		UAnimGraphNode_StateMachine* Node = Creator.CreateNode();
		// PostPlacedNewNode (inside Finalize) creates the state machine graph
		// and its entry node.
		Creator.Finalize();
		Node->NodePosX = X;
		Node->NodePosY = Y;

		if (!Name.IsEmpty() && Node->EditorStateMachineGraph != nullptr)
		{
			FBlueprintEditorUtils::RenameGraphWithSuggestion(
				Node->EditorStateMachineGraph, FNameValidatorFactory::MakeValidator(Node), Name);
		}

		if (bConnectToResult)
		{
			FString ConnectError;
			if (!Connect(Graph, Node->FindPin(TEXT("Pose"), EGPD_Output), FindResultInputPin(Graph), ConnectError))
			{
				OutError = FString::Printf(
					TEXT("state machine created but not connected to the graph result: %s"), *ConnectError);
			}
		}
		return Node;
	}

	UAnimStateNode* AddState(UAnimationStateMachineGraph* Machine, const FString& Name, int32 X, int32 Y)
	{
		Machine->Modify();
		FGraphNodeCreator<UAnimStateNode> Creator(*Machine);
		UAnimStateNode* State = Creator.CreateNode();
		Creator.Finalize();
		State->NodePosX = X;
		State->NodePosY = Y;
		if (!Name.IsEmpty() && State->BoundGraph != nullptr)
		{
			// The validator keeps state names unique within the machine, so a
			// clashing name comes back as e.g. "Idle_1" — callers report
			// GetStateName() rather than the requested name.
			FBlueprintEditorUtils::RenameGraphWithSuggestion(
				State->BoundGraph, FNameValidatorFactory::MakeValidator(State), Name);
		}
		return State;
	}

	UAnimGraphNode_AssetPlayerBase* SetStateAnimation(
		UBlueprint* Blueprint,
		UAnimStateNode* State,
		UAnimationAsset* Asset,
		TOptional<bool> bLoop,
		TOptional<float> PlayRate,
		FString& OutError)
	{
		UAnimationStateGraph* StateGraph = Cast<UAnimationStateGraph>(State->BoundGraph);
		if (StateGraph == nullptr)
		{
			OutError = TEXT("state has no bound graph");
			return nullptr;
		}
		UAnimGraphNode_StateResult* Result = StateGraph->GetResultNode();
		UEdGraphPin* ResultIn = Result != nullptr ? Result->FindPin(TEXT("Result"), EGPD_Input) : nullptr;
		if (ResultIn == nullptr)
		{
			OutError = TEXT("state graph has no result node");
			return nullptr;
		}
		const bool bBlendSpace = Asset->IsA<UBlendSpace>();
		if (!bBlendSpace && !Asset->IsA<UAnimSequenceBase>())
		{
			OutError = FString::Printf(
				TEXT("'%s' is a %s — pass an AnimSequence, AnimComposite, AnimMontage or BlendSpace"),
				*Asset->GetName(), *Asset->GetClass()->GetName());
			return nullptr;
		}

		UAnimGraphNode_AssetPlayerBase* Player = nullptr;
		for (UEdGraphPin* Linked : ResultIn->LinkedTo)
		{
			if (UAnimGraphNode_AssetPlayerBase* Existing =
				Cast<UAnimGraphNode_AssetPlayerBase>(Linked ? Linked->GetOwningNode() : nullptr))
			{
				Player = Existing;
				break;
			}
		}
		if (Player != nullptr && Player->IsA<UAnimGraphNode_BlendSpacePlayer>() != bBlendSpace)
		{
			FBlueprintEditorUtils::RemoveNode(Blueprint, Player, /*bDontRecompile*/ true);
			Player = nullptr;
		}

		StateGraph->Modify();
		if (Player == nullptr)
		{
			if (bBlendSpace)
			{
				FGraphNodeCreator<UAnimGraphNode_BlendSpacePlayer> Creator(*StateGraph);
				UAnimGraphNode_BlendSpacePlayer* Node = Creator.CreateNode();
				Node->SetAnimationAsset(Asset);
				Creator.Finalize();
				Player = Node;
			}
			else
			{
				FGraphNodeCreator<UAnimGraphNode_SequencePlayer> Creator(*StateGraph);
				UAnimGraphNode_SequencePlayer* Node = Creator.CreateNode();
				Node->SetAnimationAsset(Asset);
				Creator.Finalize();
				Player = Node;
			}
			Player->NodePosX = Result->NodePosX - 320;
			Player->NodePosY = Result->NodePosY;
		}
		else
		{
			Player->Modify();
			Player->SetAnimationAsset(Asset);
		}

		if (UAnimGraphNode_SequencePlayer* Sequence = Cast<UAnimGraphNode_SequencePlayer>(Player))
		{
			if (bLoop.IsSet())
			{
				Sequence->Node.SetLoopAnimation(*bLoop);
			}
			if (PlayRate.IsSet())
			{
				Sequence->Node.SetPlayRate(*PlayRate);
			}
		}

		FString ConnectError;
		if (!Connect(StateGraph, Player->FindPin(TEXT("Pose"), EGPD_Output), ResultIn, ConnectError))
		{
			OutError = FString::Printf(
				TEXT("player created but not connected to the state result: %s"), *ConnectError);
		}
		return Player;
	}

	bool SetEntryState(UAnimationStateMachineGraph* Machine, UAnimStateNode* State, FString& OutError)
	{
		UAnimStateEntryNode* Entry = Machine->EntryNode;
		if (Entry == nullptr)
		{
			for (UEdGraphNode* Node : Machine->Nodes)
			{
				if ((Entry = Cast<UAnimStateEntryNode>(Node)) != nullptr)
				{
					break;
				}
			}
		}
		if (Entry == nullptr)
		{
			OutError = TEXT("state machine has no entry node");
			return false;
		}
		UEdGraphPin* Out = Entry->GetOutputPin();
		if (Out == nullptr)
		{
			OutError = TEXT("entry node has no output pin");
			return false;
		}
		Machine->Modify();
		Entry->Modify();
		Out->BreakAllPinLinks(/*bNotifyNodes*/ true);
		return Connect(Machine, Out, State->GetInputPin(), OutError);
	}

	UAnimStateTransitionNode* AddTransition(UAnimStateNodeBase* From, UAnimStateNodeBase* To, FString& OutError)
	{
		UEdGraph* Machine = From->GetGraph();
		if (Machine == nullptr || Machine != To->GetGraph())
		{
			OutError = TEXT("both states must belong to the same state machine");
			return nullptr;
		}
		if (From == To)
		{
			OutError = TEXT("a state cannot transition to itself");
			return nullptr;
		}
		Machine->Modify();
		FGraphNodeCreator<UAnimStateTransitionNode> Creator(*Machine);
		UAnimStateTransitionNode* Transition = Creator.CreateNode();
		// Finalize creates the rule graph (with its result node) and the pins
		// CreateConnections links.
		Creator.Finalize();
		Transition->CreateConnections(From, To);
		Transition->NodePosX = (From->NodePosX + To->NodePosX) / 2;
		Transition->NodePosY = (From->NodePosY + To->NodePosY) / 2;
		return Transition;
	}

	bool BindTransitionRuleToVariable(
		UBlueprint* Blueprint, UAnimStateTransitionNode* Transition, const FString& VariableName, FString& OutError)
	{
		UAnimationTransitionGraph* Rule = Cast<UAnimationTransitionGraph>(Transition->BoundGraph);
		UAnimGraphNode_TransitionResult* Result = Rule != nullptr ? Rule->GetResultNode() : nullptr;
		UEdGraphPin* CanEnter =
			Result != nullptr ? Result->FindPin(TEXT("bCanEnterTransition"), EGPD_Input) : nullptr;
		if (CanEnter == nullptr)
		{
			OutError = TEXT("transition has no rule result node");
			return false;
		}
		const FBPVariableDescription* Variable = Blueprint->NewVariables.FindByPredicate(
			[&VariableName](const FBPVariableDescription& Candidate)
			{
				return Candidate.VarName.ToString() == VariableName;
			});
		if (Variable == nullptr)
		{
			OutError = FString::Printf(
				TEXT("no member variable '%s' — add it with blueprint_modify add_variable (type bool)"),
				*VariableName);
			return false;
		}
		if (Variable->VarType.PinCategory != UEdGraphSchema_K2::PC_Boolean || Variable->VarType.IsContainer())
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a bool — build the rule with blueprint_modify add_node / connect_pins in graph '%s' ")
				TEXT("and feed the result node's bCanEnterTransition pin"),
				*VariableName, *GraphPath(Rule));
			return false;
		}

		Rule->Modify();
		Result->Modify();
		CanEnter->BreakAllPinLinks(/*bNotifyNodes*/ true);

		FGraphNodeCreator<UK2Node_VariableGet> Creator(*Rule);
		UK2Node_VariableGet* Getter = Creator.CreateNode();
		Getter->VariableReference.SetSelfMember(FName(*VariableName));
		Creator.Finalize();
		Getter->NodePosX = Result->NodePosX - 250;
		Getter->NodePosY = Result->NodePosY;

		UEdGraphPin* Out = Getter->FindPin(FName(*VariableName), EGPD_Output);
		if (Out == nullptr)
		{
			Out = FirstOutputPin(Getter);
		}
		return Connect(Rule, Out, CanEnter, OutError);
	}

	TSharedRef<FJsonObject> StateToJson(UAnimStateNode* State)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), State->GetStateName());
		Object->SetStringField(TEXT("guid"), State->NodeGuid.ToString());
		Object->SetNumberField(TEXT("x"), State->NodePosX);
		Object->SetNumberField(TEXT("y"), State->NodePosY);
		TArray<TSharedPtr<FJsonValue>> Animations;
		if (State->BoundGraph != nullptr)
		{
			Object->SetStringField(TEXT("graph"), GraphPath(State->BoundGraph));
			Object->SetNumberField(TEXT("node_count"), State->BoundGraph->Nodes.Num());
			for (UEdGraphNode* Node : State->BoundGraph->Nodes)
			{
				if (const UAnimGraphNode_AssetPlayerBase* Player = Cast<UAnimGraphNode_AssetPlayerBase>(Node))
				{
					const UAnimationAsset* Asset = Player->GetAnimationAsset();
					Animations.Add(MakeShared<FJsonValueString>(
						Asset != nullptr ? Asset->GetPathName() : TEXT("(none)")));
				}
			}
		}
		Object->SetArrayField(TEXT("animations"), Animations);
		return Object;
	}

	TSharedRef<FJsonObject> TransitionToJson(UAnimStateTransitionNode* Transition)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("guid"), Transition->NodeGuid.ToString());
		Object->SetStringField(TEXT("from"), StateNameOf(Transition->GetPreviousState()));
		Object->SetStringField(TEXT("to"), StateNameOf(Transition->GetNextState()));
		Object->SetNumberField(TEXT("crossfade_duration"), Transition->CrossfadeDuration);
		Object->SetNumberField(TEXT("priority"), Transition->PriorityOrder);
		Object->SetBoolField(TEXT("automatic_rule"), Transition->bAutomaticRuleBasedOnSequencePlayerInState);
		Object->SetBoolField(TEXT("bidirectional"), Transition->Bidirectional);
		Object->SetBoolField(TEXT("disabled"), Transition->bDisabled);
		if (UAnimationTransitionGraph* Rule = Cast<UAnimationTransitionGraph>(Transition->BoundGraph))
		{
			Object->SetStringField(TEXT("rule_graph"), GraphPath(Rule));
			if (UAnimGraphNode_TransitionResult* Result = Rule->GetResultNode())
			{
				if (const UEdGraphPin* CanEnter = Result->FindPin(TEXT("bCanEnterTransition"), EGPD_Input))
				{
					Object->SetBoolField(TEXT("rule_bound"), CanEnter->LinkedTo.Num() > 0);
					Object->SetStringField(TEXT("rule_default"), CanEnter->DefaultValue);
				}
			}
		}
		return Object;
	}

	TSharedRef<FJsonObject> StateMachineToJson(UAnimationStateMachineGraph* Machine)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), Machine->GetName());
		Object->SetStringField(TEXT("path"), GraphPath(Machine));
		if (Machine->OwnerAnimGraphNode != nullptr)
		{
			Object->SetStringField(TEXT("node_guid"), Machine->OwnerAnimGraphNode->NodeGuid.ToString());
			if (const UEdGraph* Owner = Machine->OwnerAnimGraphNode->GetGraph())
			{
				Object->SetStringField(TEXT("owner_graph"), GraphPath(Owner));
			}
		}
		FString EntryState;
		if (Machine->EntryNode != nullptr)
		{
			EntryState = StateNameOf(Cast<UAnimStateNodeBase>(Machine->EntryNode->GetOutputNode()));
		}
		Object->SetStringField(TEXT("entry_state"), EntryState);

		TArray<TSharedPtr<FJsonValue>> States, Transitions;
		for (UEdGraphNode* Node : Machine->Nodes)
		{
			if (UAnimStateNode* State = Cast<UAnimStateNode>(Node))
			{
				States.Add(MakeShared<FJsonValueObject>(StateToJson(State)));
			}
			else if (UAnimStateTransitionNode* Transition = Cast<UAnimStateTransitionNode>(Node))
			{
				Transitions.Add(MakeShared<FJsonValueObject>(TransitionToJson(Transition)));
			}
		}
		Object->SetArrayField(TEXT("states"), States);
		Object->SetArrayField(TEXT("transitions"), Transitions);
		return Object;
	}
}
