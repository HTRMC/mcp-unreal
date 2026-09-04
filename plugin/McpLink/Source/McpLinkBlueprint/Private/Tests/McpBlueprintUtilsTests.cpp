// Pin-type mapping used by blueprint_modify add_variable.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "GameFramework/Actor.h"
#include "McpBlueprintUtils.h"

namespace
{
	constexpr EAutomationTestFlags McpTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpPinTypeTest, "McpLink.Blueprint.PinType", McpTestFlags)
bool FMcpPinTypeTest::RunTest(const FString& Parameters)
{
	FEdGraphPinType Type;
	FString Error;

	TestTrue(TEXT("bool"), McpLink::MakePinType(TEXT("bool"), Type, Error));
	TestEqual(TEXT("bool category"), Type.PinCategory, UEdGraphSchema_K2::PC_Boolean);

	TestTrue(TEXT("int"), McpLink::MakePinType(TEXT("int"), Type, Error));
	TestEqual(TEXT("int category"), Type.PinCategory, UEdGraphSchema_K2::PC_Int);

	// Blueprint floats are doubles in UE5.
	TestTrue(TEXT("float"), McpLink::MakePinType(TEXT("float"), Type, Error));
	TestEqual(TEXT("float category"), Type.PinCategory, UEdGraphSchema_K2::PC_Real);
	TestEqual(TEXT("float is double-precision"), Type.PinSubCategory, UEdGraphSchema_K2::PC_Double);

	TestTrue(TEXT("string"), McpLink::MakePinType(TEXT("String"), Type, Error));
	TestEqual(TEXT("case-insensitive"), Type.PinCategory, UEdGraphSchema_K2::PC_String);

	TestTrue(TEXT("vector"), McpLink::MakePinType(TEXT("vector"), Type, Error));
	TestEqual(TEXT("vector is a struct"), Type.PinCategory, UEdGraphSchema_K2::PC_Struct);
	TestEqual(TEXT("vector struct"), Type.PinSubCategoryObject.Get(),
		static_cast<UObject*>(TBaseStructure<FVector>::Get()));

	TestTrue(TEXT("object:Actor"), McpLink::MakePinType(TEXT("object:Actor"), Type, Error));
	TestEqual(TEXT("object category"), Type.PinCategory, UEdGraphSchema_K2::PC_Object);
	TestEqual(TEXT("object class"), Type.PinSubCategoryObject.Get(),
		static_cast<UObject*>(AActor::StaticClass()));

	TestTrue(TEXT("class:Actor"), McpLink::MakePinType(TEXT("class:/Script/Engine.Actor"), Type, Error));
	TestEqual(TEXT("class category"), Type.PinCategory, UEdGraphSchema_K2::PC_Class);

	TestFalse(TEXT("unknown type rejected"), McpLink::MakePinType(TEXT("nonsense_type"), Type, Error));
	TestTrue(TEXT("error names the type"), Error.Contains(TEXT("nonsense_type")));
	TestTrue(TEXT("error lists valid options"), Error.Contains(TEXT("vector")));

	TestFalse(TEXT("unknown class rejected"), McpLink::MakePinType(TEXT("object:NoSuchClassQq"), Type, Error));
	TestTrue(TEXT("class error names the class"), Error.Contains(TEXT("NoSuchClassQq")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
