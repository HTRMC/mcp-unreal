// Animation Blueprint authoring helpers: state machines, states, transitions.
// Everything here mirrors what the Animation Blueprint editor does on the
// user's behalf (FGraphNodeCreator + PostPlacedNewNode creates the bound
// graphs, links go through the graph schema), so the result is
// indistinguishable from a hand-authored asset.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UAnimBlueprint;
class UAnimGraphNode_AssetPlayerBase;
class UAnimGraphNode_StateMachine;
class UAnimStateNode;
class UAnimStateNodeBase;
class UAnimStateTransitionNode;
class UAnimationAsset;
class UAnimationGraph;
class UAnimationStateMachineGraph;
class UBlueprint;
class UClass;
class USkeletalMesh;
class USkeleton;

namespace McpLink::Anim
{
	/// Create an Animation Blueprint in memory for a skeleton (what
	/// UAnimBlueprintFactory does, minus the dialogs). ParentClass defaults to
	/// UAnimInstance. The caller checks the package does not exist yet.
	UAnimBlueprint* CreateAnimBlueprint(
		const FString& PackagePath,
		USkeleton* Skeleton,
		UClass* ParentClass,
		USkeletalMesh* PreviewMesh,
		FString& OutError);

	/// The animation graph named GraphName ("AnimGraph" when empty). A state's
	/// own graph is also an animation graph, so nested machines work too.
	UAnimationGraph* FindAnimGraph(UBlueprint* Blueprint, const FString& GraphName);

	void CollectStateMachines(UBlueprint* Blueprint, TArray<UAnimationStateMachineGraph*>& Out);

	/// State machine by name or graph path; an empty name resolves when the
	/// Blueprint has exactly one.
	UAnimationStateMachineGraph* FindStateMachine(
		UBlueprint* Blueprint, const FString& NameOrPath, FString& OutError);

	/// State by name or node GUID.
	UAnimStateNode* FindState(UAnimationStateMachineGraph* Machine, const FString& NameOrGuid);
	UAnimStateTransitionNode* FindTransition(UAnimationStateMachineGraph* Machine, const FString& Guid);

	/// Add a state machine node; optionally wire its pose into the graph's
	/// result. OutError is set when the node was created but not connected.
	UAnimGraphNode_StateMachine* AddStateMachine(
		UAnimationGraph* Graph, const FString& Name, bool bConnectToResult, int32 X, int32 Y, FString& OutError);

	UAnimStateNode* AddState(UAnimationStateMachineGraph* Machine, const FString& Name, int32 X, int32 Y);

	/// Make the state play an asset: a sequence/composite/montage gets a
	/// Sequence Player, a blend space a Blend Space Player. Reuses the player
	/// already feeding the state's result when it is of the right kind.
	UAnimGraphNode_AssetPlayerBase* SetStateAnimation(
		UBlueprint* Blueprint,
		UAnimStateNode* State,
		UAnimationAsset* Asset,
		TOptional<bool> bLoop,
		TOptional<float> PlayRate,
		FString& OutError);

	bool SetEntryState(UAnimationStateMachineGraph* Machine, UAnimStateNode* State, FString& OutError);

	UAnimStateTransitionNode* AddTransition(UAnimStateNodeBase* From, UAnimStateNodeBase* To, FString& OutError);

	/// Wire a bool member variable straight into the transition rule's result,
	/// replacing whatever fed it before.
	bool BindTransitionRuleToVariable(
		UBlueprint* Blueprint, UAnimStateTransitionNode* Transition, const FString& VariableName, FString& OutError);

	TSharedRef<FJsonObject> StateToJson(UAnimStateNode* State);
	TSharedRef<FJsonObject> TransitionToJson(UAnimStateTransitionNode* Transition);
	TSharedRef<FJsonObject> StateMachineToJson(UAnimationStateMachineGraph* Machine);
}
