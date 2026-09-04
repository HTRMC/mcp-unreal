// End-to-end authoring of a Blackboard and a Behavior Tree.
//
// The check that matters for the tree is the last one: a Behavior Tree that
// looks right in the graph but leaves RootNode null runs nothing at all, and
// that is exactly what happens when a task is wired where a composite belongs.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Composites/BTComposite_Selector.h"
#include "BehaviorTree/Decorators/BTDecorator_Loop.h"
#include "BehaviorTree/Services/BTService_DefaultFocus.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "EdGraph/EdGraph.h"
#include "GameFramework/Actor.h"
#include "McpAiUtils.h"
#include "McpTestFlags.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
	template <typename T>
	T* MakeScratchAsset(const TCHAR* Prefix)
	{
		const FString PackageName = FString::Printf(
			TEXT("/Temp/McpLinkTests/%s_%s"), Prefix,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		UPackage* Package = CreatePackage(*PackageName);
		return NewObject<T>(
			Package, FName(*FPackageName::GetShortName(PackageName)),
			RF_Public | RF_Standalone | RF_Transactional);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpBlackboardKeyTest, "McpLink.AI.BlackboardKeys", McpTestFlags)
bool FMcpBlackboardKeyTest::RunTest(const FString& Parameters)
{
	UBlackboardData* Blackboard = MakeScratchAsset<UBlackboardData>(TEXT("BB"));
	if (!TestNotNull(TEXT("blackboard created"), Blackboard))
	{
		return false;
	}
	FString Error;

	UBlackboardKeyType* Bool = McpLink::Ai::MakeKeyType(Blackboard, TEXT("bool"), Error);
	TestNotNull(TEXT("bool key"), Bool);
	TestTrue(TEXT("bool key type"), Bool != nullptr && Bool->IsA<UBlackboardKeyType_Bool>());

	UBlackboardKeyType* Vector = McpLink::Ai::MakeKeyType(Blackboard, TEXT("Vector"), Error);
	TestTrue(TEXT("vector key is case-insensitive"),
		Vector != nullptr && Vector->IsA<UBlackboardKeyType_Vector>());

	UBlackboardKeyType* Object =
		McpLink::Ai::MakeKeyType(Blackboard, TEXT("object:/Script/Engine.Actor"), Error);
	if (TestNotNull(TEXT("object key"), Object))
	{
		UBlackboardKeyType_Object* AsObject = Cast<UBlackboardKeyType_Object>(Object);
		if (TestNotNull(TEXT("object key type"), AsObject))
		{
			// Without a base class an object key accepts anything, which is the
			// silent-failure shape this carries the class for.
			TestEqual(TEXT("object key base class"), AsObject->BaseClass.Get(), AActor::StaticClass());
		}
	}

	TestNull(TEXT("unknown key type refused"),
		McpLink::Ai::MakeKeyType(Blackboard, TEXT("quaternion"), Error));
	TestTrue(TEXT("error lists valid key types"), Error.Contains(TEXT("rotator")));

	TestNull(TEXT("unknown class refused"),
		McpLink::Ai::MakeKeyType(Blackboard, TEXT("object:NoSuchClassQq"), Error));
	TestTrue(TEXT("error names the class"), Error.Contains(TEXT("NoSuchClassQq")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpBehaviorTreeTest, "McpLink.AI.BehaviorTreeAuthoring", McpTestFlags)
bool FMcpBehaviorTreeTest::RunTest(const FString& Parameters)
{
	using namespace McpLink;

	UBehaviorTree* Tree = MakeScratchAsset<UBehaviorTree>(TEXT("BT"));
	if (!TestNotNull(TEXT("behavior tree created"), Tree))
	{
		return false;
	}

	UBehaviorTreeGraph* Graph = Ai::EnsureGraph(Tree);
	if (!TestNotNull(TEXT("graph created"), Graph))
	{
		return false;
	}
	UBehaviorTreeGraphNode* Root = Ai::FindRoot(Graph);
	if (!TestNotNull(TEXT("root node created with the graph"), Root))
	{
		return false;
	}

	FString Error;

	// ---- a composite under the root ----
	UBehaviorTreeGraphNode* Selector = Ai::AddNode(
		Tree, Graph, UBTComposite_Selector::StaticClass(), nullptr, 0, 200, Error);
	if (!TestNotNull(TEXT("selector added under the root"), Selector))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("selector has a runtime instance"),
		Selector->NodeInstance != nullptr
			&& Selector->NodeInstance->IsA<UBTComposite_Selector>());

	// ---- a task under the composite ----
	UBehaviorTreeGraphNode* Wait = Ai::AddNode(
		Tree, Graph, UBTTask_Wait::StaticClass(), Selector, 0, 400, Error);
	if (!TestNotNull(TEXT("wait task added under the selector"), Wait))
	{
		AddError(Error);
		return false;
	}

	// ---- a decorator and a service attach to a node, not to a pin ----
	UBehaviorTreeGraphNode* Loop = Ai::AddNode(
		Tree, Graph, UBTDecorator_Loop::StaticClass(), Selector, 0, 0, Error);
	if (TestNotNull(TEXT("loop decorator attached"), Loop))
	{
		TestEqual(TEXT("decorator's parent is the selector"),
			Cast<UBehaviorTreeGraphNode>(Loop->ParentNode.Get()), Selector);
		TestTrue(TEXT("decorator listed on its parent"), Selector->SubNodes.Contains(Loop));
	}
	UBehaviorTreeGraphNode* Focus = Ai::AddNode(
		Tree, Graph, UBTService_DefaultFocus::StaticClass(), Selector, 0, 0, Error);
	TestNotNull(TEXT("service attached"), Focus);

	// A decorator with no parent has nothing to attach to.
	TestNull(TEXT("decorator without a parent refused"),
		Ai::AddNode(Tree, Graph, UBTDecorator_Loop::StaticClass(), nullptr, 0, 0, Error));
	TestTrue(TEXT("error explains the parent requirement"), Error.Contains(TEXT("parent")));

	// A task cannot take children; the schema is what catches this.
	TestNull(TEXT("task under a task refused"),
		Ai::AddNode(Tree, Graph, UBTTask_Wait::StaticClass(), Wait, 0, 0, Error));

	// ---- the graph compiles into a runtime tree ----
	Graph->UpdateAsset(UBehaviorTreeGraph::ClearDebuggerFlags);
	if (TestNotNull(TEXT("graph compiled into a runtime root"), Tree->RootNode.Get()))
	{
		TestTrue(TEXT("runtime root is the selector"),
			Tree->RootNode->IsA<UBTComposite_Selector>());
		TestEqual(TEXT("runtime selector has one child"), Tree->RootNode->GetChildrenNum(), 1);
	}

	// ---- removing the task leaves the tree valid ----
	const FString WaitId = Wait->NodeGuid.ToString();
	TestEqual(TEXT("node lookup by id"), Ai::FindNode(Graph, WaitId), Wait);
	Wait->DestroyNode();
	Graph->UpdateAsset(UBehaviorTreeGraph::ClearDebuggerFlags);
	TestNull(TEXT("removed node no longer found"), Ai::FindNode(Graph, WaitId));
	if (Tree->RootNode != nullptr)
	{
		TestEqual(TEXT("runtime selector has no children left"),
			Tree->RootNode->GetChildrenNum(), 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
