// End-to-end authoring of an Animation Blueprint state machine against the
// engine's own tutorial skeleton, compiled to make sure the graph we build is
// one the anim compiler accepts.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "McpAnimBlueprintUtils.h"
#include "McpBlueprintUtils.h"
#include "UObject/Package.h"
#include "McpTestFlags.h"

namespace
{

	const TCHAR* TutorialSkeleton =
		TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
	const TCHAR* TutorialIdle =
		TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
	const TCHAR* TutorialWalk =
		TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpAnimStateMachineTest, "McpLink.AnimBlueprint.StateMachineAuthoring", McpTestFlags)
bool FMcpAnimStateMachineTest::RunTest(const FString& Parameters)
{
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, TutorialSkeleton);
	UAnimSequenceBase* Idle = LoadObject<UAnimSequenceBase>(nullptr, TutorialIdle);
	UAnimSequenceBase* Walk = LoadObject<UAnimSequenceBase>(nullptr, TutorialWalk);
	if (!TestNotNull(TEXT("tutorial skeleton loads"), Skeleton)
		|| !TestNotNull(TEXT("tutorial idle loads"), Idle)
		|| !TestNotNull(TEXT("tutorial walk loads"), Walk))
	{
		return false;
	}

	const FString PackagePath = FString::Printf(
		TEXT("/Temp/McpLinkTests/ABP_McpTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FString Error;
	UAnimBlueprint* Blueprint = McpLink::Anim::CreateAnimBlueprint(PackagePath, Skeleton, nullptr, nullptr, Error);
	if (!TestNotNull(TEXT("anim blueprint created"), Blueprint))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("skeleton assigned"), Blueprint->TargetSkeleton.Get(), Skeleton);

	UAnimationGraph* AnimGraph = McpLink::Anim::FindAnimGraph(Blueprint, FString());
	if (!TestNotNull(TEXT("AnimGraph exists"), AnimGraph))
	{
		return false;
	}

	UAnimGraphNode_StateMachine* MachineNode =
		McpLink::Anim::AddStateMachine(AnimGraph, TEXT("Locomotion"), true, 0, 0, Error);
	if (!TestNotNull(TEXT("state machine added"), MachineNode))
	{
		return false;
	}
	TestTrue(TEXT("state machine connected to output pose"), Error.IsEmpty());
	TestEqual(TEXT("state machine named"), MachineNode->GetStateMachineName(), FString(TEXT("Locomotion")));

	UAnimationStateMachineGraph* Machine = McpLink::Anim::FindStateMachine(Blueprint, FString(), Error);
	if (!TestNotNull(TEXT("single state machine resolves without a name"), Machine))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("state machine by name"),
		McpLink::Anim::FindStateMachine(Blueprint, TEXT("Locomotion"), Error), Machine);

	UAnimStateNode* IdleState = McpLink::Anim::AddState(Machine, TEXT("Idle"), 0, 0);
	UAnimStateNode* WalkState = McpLink::Anim::AddState(Machine, TEXT("Walk"), 400, 0);
	UAnimStateNode* Duplicate = McpLink::Anim::AddState(Machine, TEXT("Idle"), 0, 200);
	TestEqual(TEXT("state named"), IdleState->GetStateName(), FString(TEXT("Idle")));
	TestNotEqual(TEXT("duplicate state names are made unique"), Duplicate->GetStateName(), FString(TEXT("Idle")));
	FBlueprintEditorUtils::RemoveNode(Blueprint, Duplicate, true);

	TestTrue(TEXT("entry state set"), McpLink::Anim::SetEntryState(Machine, IdleState, Error));
	TestNotNull(TEXT("idle animation set"),
		McpLink::Anim::SetStateAnimation(Blueprint, IdleState, Idle, true, TOptional<float>(), Error));
	TestTrue(TEXT("idle player connected"), Error.IsEmpty());
	TestNotNull(TEXT("walk animation set"),
		McpLink::Anim::SetStateAnimation(Blueprint, WalkState, Walk, true, 1.0f, Error));
	// Setting again must reuse the existing player rather than stack a second one.
	McpLink::Anim::SetStateAnimation(Blueprint, WalkState, Walk, TOptional<bool>(), TOptional<float>(), Error);
	int32 PlayerCount = 0;
	for (const UEdGraphNode* Node : WalkState->BoundGraph->Nodes)
	{
		PlayerCount += Node->GetClass()->GetName().Contains(TEXT("SequencePlayer")) ? 1 : 0;
	}
	TestEqual(TEXT("one sequence player per state"), PlayerCount, 1);

	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	TestTrue(TEXT("bool variable added"),
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, TEXT("IsMoving"), BoolType));

	UAnimStateTransitionNode* Transition = McpLink::Anim::AddTransition(IdleState, WalkState, Error);
	if (!TestNotNull(TEXT("transition added"), Transition))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("transition source"), Transition->GetPreviousState(), static_cast<UAnimStateNodeBase*>(IdleState));
	TestEqual(TEXT("transition target"), Transition->GetNextState(), static_cast<UAnimStateNodeBase*>(WalkState));
	TestNull(TEXT("self transition refused"), McpLink::Anim::AddTransition(IdleState, IdleState, Error));

	TestTrue(TEXT("rule bound to bool variable"),
		McpLink::Anim::BindTransitionRuleToVariable(Blueprint, Transition, TEXT("IsMoving"), Error));
	TestFalse(TEXT("unknown rule variable refused"),
		McpLink::Anim::BindTransitionRuleToVariable(Blueprint, Transition, TEXT("NoSuchVar"), Error));
	TestTrue(TEXT("error names the variable"), Error.Contains(TEXT("NoSuchVar")));

	// Graph paths reach into the state machine.
	TestEqual(TEXT("state graph by path"),
		McpLink::FindGraph(Blueprint, TEXT("Locomotion/Idle")), IdleState->BoundGraph.Get());
	TestEqual(TEXT("state graph by full path"),
		McpLink::FindGraph(Blueprint, TEXT("AnimGraph/Locomotion/Idle")), IdleState->BoundGraph.Get());
	TestEqual(TEXT("rule graph path is titled like the editor"),
		McpLink::GraphPath(Transition->BoundGraph), FString(TEXT("AnimGraph/Locomotion/Idle to Walk")));
	TestEqual(TEXT("rule graph by path"),
		McpLink::FindGraph(Blueprint, TEXT("Locomotion/Idle to Walk")), Transition->BoundGraph.Get());
	// A second transition between the same states must not shadow the first.
	UAnimStateTransitionNode* Second = McpLink::Anim::AddTransition(IdleState, WalkState, Error);
	TestEqual(TEXT("duplicate rule graph gets a numbered path"),
		McpLink::GraphPath(Second->BoundGraph), FString(TEXT("AnimGraph/Locomotion/Idle to Walk #2")));
	TestEqual(TEXT("numbered path resolves"),
		McpLink::FindGraph(Blueprint, TEXT("Locomotion/Idle to Walk #2")), Second->BoundGraph.Get());
	FBlueprintEditorUtils::RemoveNode(Blueprint, Second, true);

	const TSharedRef<FJsonObject> Json = McpLink::Anim::StateMachineToJson(Machine);
	TestEqual(TEXT("json entry state"), Json->GetStringField(TEXT("entry_state")), FString(TEXT("Idle")));
	TestEqual(TEXT("json state count"), Json->GetArrayField(TEXT("states")).Num(), 2);
	TestEqual(TEXT("json transition count"), Json->GetArrayField(TEXT("transitions")).Num(), 1);
	const TSharedPtr<FJsonObject> TransitionJson =
		Json->GetArrayField(TEXT("transitions"))[0]->AsObject();
	TestTrue(TEXT("json rule bound"), TransitionJson->GetBoolField(TEXT("rule_bound")));
	TestEqual(TEXT("json transition from"), TransitionJson->GetStringField(TEXT("from")), FString(TEXT("Idle")));

	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("anim blueprint compiles"),
		Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);

	// Let GC reclaim the transient asset.
	Blueprint->ClearFlags(RF_Standalone);
	Blueprint->GetOutermost()->ClearFlags(RF_Standalone);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
