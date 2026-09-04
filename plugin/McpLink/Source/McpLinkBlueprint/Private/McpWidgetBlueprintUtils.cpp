#include "McpWidgetBlueprintUtils.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/NamedSlotInterface.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "McpResolve.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintOperationUtils.h"

namespace McpLink::Widgets
{
	namespace
	{
		FString JoinCapped(const TArray<FString>& Items, int32 Cap)
		{
			if (Items.IsEmpty())
			{
				return TEXT("(none)");
			}
			TArray<FString> Shown;
			for (int32 Index = 0; Index < Items.Num() && Index < Cap; ++Index)
			{
				Shown.Add(Items[Index]);
			}
			FString Joined = FString::Join(Shown, TEXT(", "));
			if (Items.Num() > Cap)
			{
				Joined += FString::Printf(TEXT(", … (%d more)"), Items.Num() - Cap);
			}
			return Joined;
		}

		TArray<FString> EditablePropertyNames(const UClass* Class)
		{
			TArray<FString> Names;
			for (TFieldIterator<FProperty> It(Class); It; ++It)
			{
				if (It->HasAnyPropertyFlags(CPF_Edit) && !It->HasAnyPropertyFlags(CPF_Deprecated))
				{
					Names.Add(It->GetName());
				}
			}
			return Names;
		}
	}

	UWidgetBlueprint* CreateWidgetBlueprint(
		const FString& PackagePath, UClass* ParentClass, UClass* RootClass, FString& OutError)
	{
		// Read-only roots included so tests can build under /Temp.
		if (!FPackageName::IsValidLongPackageName(PackagePath, /*bIncludeReadOnlyRoots=*/true))
		{
			OutError = FString::Printf(
				TEXT("'%s' is not a valid package path — expected something like /Game/UI/WBP_Hud"), *PackagePath);
			return nullptr;
		}
		if (FindPackage(nullptr, *PackagePath) != nullptr || FPackageName::DoesPackageExist(PackagePath))
		{
			OutError = FString::Printf(TEXT("an asset already exists at '%s'"), *PackagePath);
			return nullptr;
		}
		if (ParentClass == nullptr)
		{
			ParentClass = UUserWidget::StaticClass();
		}
		if (!ParentClass->IsChildOf(UUserWidget::StaticClass())
			|| !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
			OutError = FString::Printf(
				TEXT("parent class '%s' must be a blueprintable UserWidget subclass"), *ParentClass->GetName());
			return nullptr;
		}
		if (RootClass != nullptr && !RootClass->IsChildOf(UPanelWidget::StaticClass()))
		{
			OutError = FString::Printf(
				TEXT("root widget class '%s' must be a panel (CanvasPanel, VerticalBox, Overlay, ...) — ")
				TEXT("add other widgets with add_widget"),
				*RootClass->GetName());
			return nullptr;
		}

		UPackage* Package = CreatePackage(*PackagePath);
		if (Package == nullptr)
		{
			OutError = FString::Printf(TEXT("could not create package '%s'"), *PackagePath);
			return nullptr;
		}
		const FString Name = FPackageName::GetShortName(PackagePath);
		UWidgetBlueprint* Blueprint = FWidgetBlueprintOperationUtils::CreateWidgetBlueprint(
			Package, FName(*Name), BPTYPE_Normal, ParentClass, RootClass, NAME_None, /*bRegisterAndCompile=*/true);
		if (Blueprint == nullptr)
		{
			OutError = TEXT("the engine refused to create the Widget Blueprint (see the output log)");
		}
		return Blueprint;
	}

	UWidget* FindWidget(UWidgetBlueprint* Blueprint, const FString& Spec)
	{
		if (Blueprint == nullptr || Blueprint->WidgetTree == nullptr || Spec.IsEmpty())
		{
			return nullptr;
		}
		if (UWidget* ByName = Blueprint->WidgetTree->FindWidget(FName(*Spec)))
		{
			return ByName;
		}
		if (Spec.Contains(TEXT(":")) || Spec.StartsWith(TEXT("/")))
		{
			UWidget* ByPath = Cast<UWidget>(ResolveObject(Spec));
			if (ByPath != nullptr && ByPath->GetTypedOuter<UWidgetBlueprint>() == Blueprint)
			{
				return ByPath;
			}
		}
		UWidget* ByLabel = nullptr;
		Blueprint->WidgetTree->ForEachWidget([&ByLabel, &Spec](UWidget* Widget)
		{
			if (ByLabel == nullptr && Widget->GetDisplayLabel() == Spec)
			{
				ByLabel = Widget;
			}
		});
		return ByLabel;
	}

	TArray<FString> WidgetNames(UWidgetBlueprint* Blueprint)
	{
		TArray<FString> Names;
		if (Blueprint != nullptr && Blueprint->WidgetTree != nullptr)
		{
			Blueprint->WidgetTree->ForEachWidget([&Names](UWidget* Widget) { Names.Add(Widget->GetName()); });
		}
		return Names;
	}

	UWidget* AddWidget(
		UWidgetBlueprint* Blueprint,
		UClass* Class,
		const FString& Name,
		UWidget* Parent,
		int32 Index,
		TOptional<bool> bIsVariable,
		FString& OutError)
	{
		if (Blueprint == nullptr || Blueprint->WidgetTree == nullptr)
		{
			OutError = TEXT("the Widget Blueprint has no widget tree");
			return nullptr;
		}
		if (Class == nullptr || !Class->IsChildOf(UWidget::StaticClass()))
		{
			OutError = TEXT("'class' must be a UWidget subclass — see list_widget_classes");
			return nullptr;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated) || Class == UUserWidget::StaticClass())
		{
			OutError = FString::Printf(
				TEXT("'%s' cannot be placed (abstract, deprecated, or the bare UserWidget class)"), *Class->GetName());
			return nullptr;
		}

		FName WidgetName = NAME_None;
		if (!Name.IsEmpty())
		{
			if (Blueprint->WidgetTree->FindWidget(FName(*Name)) != nullptr)
			{
				OutError = FString::Printf(TEXT("a widget named '%s' already exists in this tree"), *Name);
				return nullptr;
			}
			// The name becomes the object name (and the variable name), so it has
			// to survive object paths: no spaces or punctuation.
			if (!FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS))
			{
				OutError = FString::Printf(
					TEXT("'%s' is not a valid widget name — use letters, digits and underscores"), *Name);
				return nullptr;
			}
			FKismetNameValidator Validator(Blueprint);
			const EValidatorResult Result = Validator.IsValid(Name);
			if (Result != EValidatorResult::Ok)
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid widget name: %s"), *Name,
					*INameValidatorInterface::GetErrorString(Name, Result));
				return nullptr;
			}
			WidgetName = FName(*Name);
		}

		if (Parent == nullptr && Blueprint->WidgetTree->RootWidget != nullptr)
		{
			Parent = Blueprint->WidgetTree->RootWidget;
		}
		if (Parent != nullptr && !Parent->IsA<UPanelWidget>())
		{
			OutError = FString::Printf(
				TEXT("parent '%s' (%s) is not a panel and cannot hold children — pick a panel or wrap_widget it"),
				*Parent->GetName(), *Parent->GetClass()->GetName());
			return nullptr;
		}
		if (const UPanelWidget* Panel = Cast<UPanelWidget>(Parent); Panel && !Panel->CanAddMoreChildren())
		{
			OutError = FString::Printf(
				TEXT("'%s' (%s) holds a single child and is full — use a VerticalBox/Overlay/CanvasPanel, or wrap_widget"),
				*Parent->GetName(), *Parent->GetClass()->GetName());
			return nullptr;
		}

		UWidget* Widget = Blueprint->WidgetTree->ConstructWidget<UWidget>(Class, WidgetName);
		if (Widget == nullptr)
		{
			OutError = FString::Printf(TEXT("constructing a '%s' failed"), *Class->GetName());
			return nullptr;
		}
		if (bIsVariable.IsSet())
		{
			Widget->bIsVariable = *bIsVariable;
		}
		FText Error;
		if (!FWidgetBlueprintOperationUtils::AddWidget(Blueprint, Widget, Parent, Index, Error))
		{
			OutError = Error.ToString();
			return nullptr;
		}
		return Widget;
	}

	bool ApplyJsonProperties(UObject* Target, const TSharedRef<FJsonObject>& Properties, FString& OutError)
	{
		if (Target == nullptr)
		{
			OutError = TEXT("no target object");
			return false;
		}
		if (Properties->Values.IsEmpty())
		{
			OutError = TEXT("'properties' must be a non-empty object, e.g. {\"Text\": \"Hello\"}");
			return false;
		}
		TArray<FString> Unknown;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
		{
			if (FindFProperty<FProperty>(Target->GetClass(), *Pair.Key) == nullptr)
			{
				Unknown.Add(Pair.Key);
			}
		}
		if (!Unknown.IsEmpty())
		{
			OutError = FString::Printf(TEXT("unknown propert%s on %s: %s — editable properties: %s"),
				Unknown.Num() == 1 ? TEXT("y") : TEXT("ies"), *Target->GetClass()->GetName(),
				*FString::Join(Unknown, TEXT(", ")), *JoinCapped(EditablePropertyNames(Target->GetClass()), 60));
			return false;
		}

		Target->SetFlags(RF_Transactional);
		Target->Modify();
		FText Fail;
		if (!FJsonObjectConverter::JsonObjectToUStruct(Properties, Target->GetClass(), Target, 0, 0, false, &Fail))
		{
			OutError = Fail.IsEmpty() ? TEXT("value conversion failed (check the value shapes against get_widget)")
			                          : Fail.ToString();
			return false;
		}
		return true;
	}

	TSharedRef<FJsonObject> PropertiesToJson(UObject* Target, const TArray<FString>& Keys)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		if (Target == nullptr)
		{
			return Json;
		}
		if (Keys.IsEmpty())
		{
			FJsonObjectConverter::UStructToJsonObject(Target->GetClass(), Target, Json, CPF_Edit,
				CPF_InstancedReference | CPF_Transient | CPF_Deprecated, nullptr,
				EJsonObjectConversionFlags::SkipStandardizeCase);
			return Json;
		}
		for (const FString& Key : Keys)
		{
			FProperty* Property = FindFProperty<FProperty>(Target->GetClass(), *Key);
			if (Property == nullptr)
			{
				continue;
			}
			const TSharedPtr<FJsonValue> Value = FJsonObjectConverter::UPropertyToJsonValue(
				Property, Property->ContainerPtrToValuePtr<void>(Target), 0, 0, nullptr, nullptr,
				EJsonObjectConversionFlags::SkipStandardizeCase);
			if (Value.IsValid())
			{
				Json->SetField(Property->GetName(), Value);
			}
		}
		return Json;
	}

	TArray<FString> EventNames(const UClass* WidgetClass)
	{
		TArray<FString> Names;
		for (TFieldIterator<FMulticastDelegateProperty> It(WidgetClass); It; ++It)
		{
			Names.Add(It->GetName());
		}
		return Names;
	}

	UK2Node_ComponentBoundEvent* BindEvent(
		UWidgetBlueprint* Blueprint, UWidget* Widget, FName EventName, bool& bOutAlreadyBound, FString& OutError)
	{
		bOutAlreadyBound = false;
		if (Blueprint == nullptr || Widget == nullptr)
		{
			OutError = TEXT("a Widget Blueprint and a widget are required");
			return nullptr;
		}
		if (EventName.IsNone())
		{
			OutError = FString::Printf(TEXT("'event' is required — events on %s: %s"),
				*Widget->GetClass()->GetName(), *JoinCapped(EventNames(Widget->GetClass()), 40));
			return nullptr;
		}
		if (FindFProperty<FMulticastDelegateProperty>(Widget->GetClass(), EventName) == nullptr)
		{
			OutError = FString::Printf(TEXT("no event '%s' on %s — available: %s"), *EventName.ToString(),
				*Widget->GetClass()->GetName(), *JoinCapped(EventNames(Widget->GetClass()), 40));
			return nullptr;
		}
		if (const UK2Node_ComponentBoundEvent* Existing =
				FKismetEditorUtilities::FindBoundEventForComponent(Blueprint, EventName, Widget->GetFName()))
		{
			bOutAlreadyBound = true;
			return const_cast<UK2Node_ComponentBoundEvent*>(Existing);
		}

		// The bound-event node references the widget through its variable on the
		// skeleton class, so the widget must be a variable and the skeleton current.
		FWidgetBlueprintOperationUtils::ToggleWidgetAsVariable(Blueprint, Widget, true, true);
		if (FindFProperty<FObjectProperty>(Blueprint->SkeletonGeneratedClass, Widget->GetFName()) == nullptr)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		FText Error;
		if (!FWidgetBlueprintOperationUtils::BindToEventProperty(
				Blueprint, EventName, Widget->GetFName(), Widget->GetClass(), /*bShouldJumpToNode=*/false, Error))
		{
			OutError = Error.ToString();
			return nullptr;
		}
		const UK2Node_ComponentBoundEvent* Node =
			FKismetEditorUtilities::FindBoundEventForComponent(Blueprint, EventName, Widget->GetFName());
		if (Node == nullptr)
		{
			OutError = TEXT("the event was bound but its node was not found in the event graph");
			return nullptr;
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		return const_cast<UK2Node_ComponentBoundEvent*>(Node);
	}

	TSharedRef<FJsonObject> SlotToJson(const UWidget* Widget, bool bWithProperties)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		UPanelSlot* Slot = Widget ? Widget->Slot.Get() : nullptr;
		if (Slot == nullptr)
		{
			return Json;
		}
		Json->SetStringField(TEXT("class"), Slot->GetClass()->GetName());
		Json->SetStringField(TEXT("path"), Slot->GetPathName());
		if (const UPanelWidget* Parent = Widget->GetParent())
		{
			Json->SetNumberField(TEXT("index"), Parent->GetChildIndex(Widget));
		}
		if (bWithProperties)
		{
			Json->SetObjectField(TEXT("properties"), PropertiesToJson(Slot, {}));
		}
		return Json;
	}

	TSharedRef<FJsonObject> WidgetToJson(const UWidget* Widget, FName NamedSlot, bool bWithSlotProperties)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		if (Widget == nullptr)
		{
			return Json;
		}
		Json->SetStringField(TEXT("name"), Widget->GetName());
		Json->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
		Json->SetStringField(TEXT("class_path"), Widget->GetClass()->GetPathName());
		Json->SetStringField(TEXT("path"), Widget->GetPathName());
		Json->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);
		if (!Widget->GetDisplayLabel().IsEmpty() && Widget->GetDisplayLabel() != Widget->GetName())
		{
			Json->SetStringField(TEXT("label"), Widget->GetDisplayLabel());
		}
		if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			Json->SetBoolField(TEXT("is_panel"), true);
			Json->SetBoolField(TEXT("multiple_children"), Panel->CanHaveMultipleChildren());
			Json->SetNumberField(TEXT("child_count"), Panel->GetChildrenCount());
		}
		if (Widget->IsA<UUserWidget>())
		{
			Json->SetBoolField(TEXT("is_user_widget"), true);
		}
		if (NamedSlot != NAME_None)
		{
			Json->SetStringField(TEXT("named_slot"), NamedSlot.ToString());
		}
		if (Widget->Slot != nullptr)
		{
			Json->SetObjectField(TEXT("slot"), SlotToJson(Widget, bWithSlotProperties));
		}
		return Json;
	}

	TSharedRef<FJsonObject> WidgetTreeToJson(const UWidget* Widget, FName NamedSlot)
	{
		const TSharedRef<FJsonObject> Json = WidgetToJson(Widget, NamedSlot, /*bWithSlotProperties=*/true);
		if (Widget == nullptr)
		{
			return Json;
		}
		TArray<TSharedPtr<FJsonValue>> Children;
		if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			for (UWidget* Child : Panel->GetAllChildren())
			{
				if (Child != nullptr)
				{
					Children.Add(MakeShared<FJsonValueObject>(WidgetTreeToJson(Child, NAME_None)));
				}
			}
		}
		if (const INamedSlotInterface* Host = Cast<INamedSlotInterface>(const_cast<UWidget*>(Widget)))
		{
			TArray<FName> SlotNames;
			Host->GetSlotNames(SlotNames);
			for (const FName SlotName : SlotNames)
			{
				if (UWidget* Content = Host->GetContentForSlot(SlotName))
				{
					Children.Add(MakeShared<FJsonValueObject>(WidgetTreeToJson(Content, SlotName)));
				}
			}
		}
		if (!Children.IsEmpty())
		{
			Json->SetArrayField(TEXT("children"), Children);
		}
		return Json;
	}
}
