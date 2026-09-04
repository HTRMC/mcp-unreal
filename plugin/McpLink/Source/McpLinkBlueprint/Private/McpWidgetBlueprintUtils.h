// Widget Blueprint (UMG) helpers shared by the routes and the automation
// tests: creation, widget-tree edits, JSON property application, event
// binding and serialisation. Thin wrappers over FWidgetBlueprintOperationUtils
// (5.8's programmatic UMG API) adding name validation and agent-friendly
// error text.
#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

class FJsonObject;
class UK2Node_ComponentBoundEvent;
class UPanelSlot;
class UWidget;
class UWidgetBlueprint;

namespace McpLink::Widgets
{
	/// Create a Widget Blueprint asset in memory at "/Game/UI/WBP_Name".
	/// ParentClass defaults to UUserWidget; RootClass (a panel) may be null for
	/// an empty tree. The result is compiled and registered with the registry.
	UWidgetBlueprint* CreateWidgetBlueprint(
		const FString& PackagePath, UClass* ParentClass, UClass* RootClass, FString& OutError);

	/// Widget by name, display label, or full object path within the tree.
	UWidget* FindWidget(UWidgetBlueprint* Blueprint, const FString& Spec);

	/// Names of every widget in the tree (for error messages).
	TArray<FString> WidgetNames(UWidgetBlueprint* Blueprint);

	/// Construct a widget of Class and add it under Parent (a panel; defaults to
	/// the root), or as the root when the tree is empty. Empty Name takes the
	/// engine default ("TextBlock_0"). Index -1 appends.
	UWidget* AddWidget(
		UWidgetBlueprint* Blueprint,
		UClass* Class,
		const FString& Name,
		UWidget* Parent,
		int32 Index,
		TOptional<bool> bIsVariable,
		FString& OutError);

	/// Write JSON fields onto an object's UPROPERTYs (FJsonObjectConverter
	/// semantics: nested structs update only the fields present, FText from a
	/// plain string, enums by name). Unknown keys fail before anything is written.
	bool ApplyJsonProperties(UObject* Target, const TSharedRef<FJsonObject>& Properties, FString& OutError);

	/// The named properties of an object as JSON; every editable property when
	/// Keys is empty.
	TSharedRef<FJsonObject> PropertiesToJson(UObject* Target, const TArray<FString>& Keys);

	/// Bind a widget's multicast event (Button OnClicked, ...) to a new event
	/// node in the event graph, exposing the widget as a variable first. Returns
	/// the existing node (bOutAlreadyBound = true) when the event is bound already.
	UK2Node_ComponentBoundEvent* BindEvent(
		UWidgetBlueprint* Blueprint, UWidget* Widget, FName EventName, bool& bOutAlreadyBound, FString& OutError);

	/// Multicast delegate (event) names on a widget class.
	TArray<FString> EventNames(const UClass* WidgetClass);

	TSharedRef<FJsonObject> SlotToJson(const UWidget* Widget, bool bWithProperties);
	TSharedRef<FJsonObject> WidgetToJson(const UWidget* Widget, FName NamedSlot, bool bWithSlotProperties);
	/// Widget plus its descendants under "children", including named-slot content.
	TSharedRef<FJsonObject> WidgetTreeToJson(const UWidget* Widget, FName NamedSlot);
}
