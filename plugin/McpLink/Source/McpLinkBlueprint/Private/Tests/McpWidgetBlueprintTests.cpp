// End-to-end authoring of a Widget Blueprint: tree building, JSON property
// application on widgets and slots, event binding, compile.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "McpWidgetBlueprintUtils.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintOperationUtils.h"

namespace
{
	constexpr EAutomationTestFlags McpTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpWidgetTreeTest, "McpLink.WidgetBlueprint.TreeAuthoring", McpTestFlags)
bool FMcpWidgetTreeTest::RunTest(const FString& Parameters)
{
	using namespace McpLink::Widgets;

	const FString PackagePath = FString::Printf(
		TEXT("/Temp/McpLinkTests/WBP_McpTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FString Error;
	UWidgetBlueprint* Blueprint = CreateWidgetBlueprint(PackagePath, nullptr, UCanvasPanel::StaticClass(), Error);
	if (!TestNotNull(*FString::Printf(TEXT("widget blueprint created: %s"), *Error), Blueprint))
	{
		return false;
	}
	UWidget* Root = Blueprint->WidgetTree->RootWidget;
	if (!TestNotNull(TEXT("root widget exists"), Root))
	{
		return false;
	}
	TestTrue(TEXT("root is a canvas panel"), Root->IsA<UCanvasPanel>());
	TestNull(TEXT("creating over an existing asset fails"),
		CreateWidgetBlueprint(PackagePath, nullptr, nullptr, Error));
	TestTrue(TEXT("conflict message"), Error.Contains(TEXT("already exists")));

	// ---- tree building
	UWidget* Menu = AddWidget(Blueprint, UVerticalBox::StaticClass(), TEXT("Menu"), nullptr, -1, {}, Error);
	if (!TestNotNull(*FString::Printf(TEXT("vertical box added: %s"), *Error), Menu))
	{
		return false;
	}
	TestEqual(TEXT("default parent is the root"), Menu->GetParent(), Cast<UPanelWidget>(Root));
	UWidget* Title = AddWidget(Blueprint, UTextBlock::StaticClass(), TEXT("Title"), Menu, -1, {}, Error);
	UWidget* Play = AddWidget(Blueprint, UButton::StaticClass(), TEXT("PlayButton"), Menu, -1, true, Error);
	if (!TestNotNull(TEXT("text block added"), Title) || !TestNotNull(TEXT("button added"), Play))
	{
		return false;
	}
	TestEqual(TEXT("menu has two children"), Cast<UPanelWidget>(Menu)->GetChildrenCount(), 2);
	TestTrue(TEXT("button flagged as variable"), Play->bIsVariable);
	TestEqual(TEXT("found by name"), FindWidget(Blueprint, TEXT("Title")), Title);
	TestEqual(TEXT("found by object path"), FindWidget(Blueprint, Title->GetPathName()), Title);

	TestNull(TEXT("duplicate name rejected"),
		AddWidget(Blueprint, UTextBlock::StaticClass(), TEXT("Title"), Menu, -1, {}, Error));
	TestTrue(TEXT("duplicate message"), Error.Contains(TEXT("already exists")));
	TestNull(TEXT("non-panel parent rejected"),
		AddWidget(Blueprint, UTextBlock::StaticClass(), FString(), Title, -1, {}, Error));
	TestTrue(TEXT("non-panel message"), Error.Contains(TEXT("not a panel")));
	TestNull(TEXT("bad name rejected"),
		AddWidget(Blueprint, UTextBlock::StaticClass(), TEXT("Bad Name!"), Menu, -1, {}, Error));

	// ---- JSON properties on the widget
	{
		const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetStringField(TEXT("Text"), TEXT("Hello"));
		TestTrue(*FString::Printf(TEXT("text applied: %s"), *Error), ApplyJsonProperties(Title, Properties, Error));
		TestEqual(TEXT("text value"), Cast<UTextBlock>(Title)->GetText().ToString(), FString(TEXT("Hello")));

		const TSharedRef<FJsonObject> Bogus = MakeShared<FJsonObject>();
		Bogus->SetStringField(TEXT("NoSuchProperty"), TEXT("x"));
		TestFalse(TEXT("unknown property rejected"), ApplyJsonProperties(Title, Bogus, Error));
		TestTrue(TEXT("unknown property lists editable ones"), Error.Contains(TEXT("Text")));

		const TSharedRef<FJsonObject> Read = PropertiesToJson(Title, { TEXT("Text") });
		TestEqual(TEXT("text read back"), Read->GetStringField(TEXT("Text")), FString(TEXT("Hello")));
	}

	// ---- JSON properties on a canvas slot (nested struct, partial update)
	{
		UCanvasPanelSlot* MenuSlot = Cast<UCanvasPanelSlot>(Menu->Slot);
		if (!TestNotNull(TEXT("menu sits in a canvas slot"), MenuSlot))
		{
			return false;
		}
		const TSharedRef<FJsonObject> Offsets = MakeShared<FJsonObject>();
		Offsets->SetNumberField(TEXT("Left"), 40);
		Offsets->SetNumberField(TEXT("Top"), 20);
		const TSharedRef<FJsonObject> Layout = MakeShared<FJsonObject>();
		Layout->SetObjectField(TEXT("Offsets"), Offsets);
		const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		Properties->SetObjectField(TEXT("LayoutData"), Layout);
		Properties->SetNumberField(TEXT("ZOrder"), 3);
		TestTrue(*FString::Printf(TEXT("slot applied: %s"), *Error), ApplyJsonProperties(MenuSlot, Properties, Error));
		TestEqual(TEXT("slot left offset"), MenuSlot->GetOffsets().Left, 40.f);
		TestEqual(TEXT("slot top offset"), MenuSlot->GetOffsets().Top, 20.f);
		TestEqual(TEXT("slot z order"), MenuSlot->GetZOrder(), 3);
	}

	// ---- event binding
	bool bAlreadyBound = false;
	UK2Node_ComponentBoundEvent* Clicked = BindEvent(Blueprint, Play, TEXT("OnClicked"), bAlreadyBound, Error);
	if (!TestNotNull(*FString::Printf(TEXT("OnClicked bound: %s"), *Error), Clicked))
	{
		return false;
	}
	TestFalse(TEXT("first bind is new"), bAlreadyBound);
	TestEqual(TEXT("bound widget"), Clicked->ComponentPropertyName, FName(TEXT("PlayButton")));
	TestEqual(TEXT("bound event"), Clicked->DelegatePropertyName, FName(TEXT("OnClicked")));
	TestEqual(TEXT("rebinding returns the same node"),
		BindEvent(Blueprint, Play, TEXT("OnClicked"), bAlreadyBound, Error), Clicked);
	TestTrue(TEXT("rebinding reports already bound"), bAlreadyBound);
	TestNull(TEXT("unknown event rejected"), BindEvent(Blueprint, Play, TEXT("OnExploded"), bAlreadyBound, Error));
	TestTrue(TEXT("unknown event lists real ones"), Error.Contains(TEXT("OnClicked")));
	// A widget that is not a variable becomes one when an event is bound.
	TestFalse(TEXT("text block starts as non-variable"), Title->bIsVariable);

	// ---- tree JSON
	const TSharedRef<FJsonObject> Tree = WidgetTreeToJson(Root, NAME_None);
	TestEqual(TEXT("tree root class"), Tree->GetStringField(TEXT("class")), FString(TEXT("CanvasPanel")));
	TestEqual(TEXT("tree root has one child"), Tree->GetArrayField(TEXT("children")).Num(), 1);
	const TSharedPtr<FJsonObject> MenuJson = Tree->GetArrayField(TEXT("children"))[0]->AsObject();
	TestEqual(TEXT("menu has two json children"), MenuJson->GetArrayField(TEXT("children")).Num(), 2);
	TestEqual(TEXT("menu slot class"),
		MenuJson->GetObjectField(TEXT("slot"))->GetStringField(TEXT("class")), FString(TEXT("CanvasPanelSlot")));

	// ---- compile
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("widget blueprint compiles"),
		Blueprint->Status == BS_UpToDate || Blueprint->Status == BS_UpToDateWithWarnings);
	TestNotNull(TEXT("generated class exposes the button"),
		FindFProperty<FObjectProperty>(Blueprint->GeneratedClass, TEXT("PlayButton")));

	// ---- removal
	FText RemoveError;
	TestTrue(TEXT("title removed"), FWidgetBlueprintOperationUtils::RemoveWidget(Blueprint, Title, RemoveError));
	TestNull(TEXT("title gone"), FindWidget(Blueprint, TEXT("Title")));
	TestEqual(TEXT("menu has one child left"), Cast<UPanelWidget>(Menu)->GetChildrenCount(), 1);

	// Let GC reclaim the transient asset.
	Blueprint->ClearFlags(RF_Standalone);
	Blueprint->GetOutermost()->ClearFlags(RF_Standalone);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
