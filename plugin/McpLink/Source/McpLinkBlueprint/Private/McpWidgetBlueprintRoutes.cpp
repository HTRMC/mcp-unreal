// Widget Blueprint (UMG) routes: create widget blueprints, build the widget
// tree (add / move / remove / rename / wrap / replace), edit widget and slot
// properties from JSON, bind widget events to event-graph nodes, inspect.
//
// Built on FWidgetBlueprintOperationUtils, the engine's programmatic UMG API
// in 5.8, so the designer's rules (single-child panels, cycles, BindWidget
// names) apply here too. The logic behind a bound event is wired with the
// generic Blueprint routes in the EventGraph.

#include "Animation/WidgetAnimation.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "K2Node_ComponentBoundEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "McpBlueprintUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "McpWidgetBlueprintUtils.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintOperationUtils.h"

namespace McpLink
{
	namespace
	{
		UWidgetBlueprint* WidgetBlueprintOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!Body->TryGetStringField(TEXT("blueprint"), Path) || Path.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'blueprint' asset path is required, e.g. /Game/UI/WBP_Hud"));
				return nullptr;
			}
			UBlueprint* Blueprint = ResolveBlueprint(Path);
			if (Blueprint == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("blueprint_not_found"),
					FString::Printf(TEXT("no Blueprint asset at '%s'"), *Path));
				return nullptr;
			}
			UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint);
			if (WidgetBlueprint == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("not_widget_blueprint"),
					FString::Printf(TEXT("'%s' is a %s, not a Widget Blueprint"),
						*Path, *Blueprint->GetClass()->GetName()));
			}
			return WidgetBlueprint;
		}

		UWidget* WidgetOrError(
			UWidgetBlueprint* Blueprint,
			const TSharedRef<FJsonObject>& Body,
			const TCHAR* Field,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			Body->TryGetStringField(Field, Spec);
			if (Spec.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					FString::Printf(TEXT("'%s' (widget name from inspect) is required"), Field));
				return nullptr;
			}
			UWidget* Widget = Widgets::FindWidget(Blueprint, Spec);
			if (Widget == nullptr)
			{
				TArray<FString> Names = Widgets::WidgetNames(Blueprint);
				if (Names.Num() > 30)
				{
					Names.SetNum(30);
					Names.Add(TEXT("…"));
				}
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("widget_not_found"),
					FString::Printf(TEXT("no widget '%s' in '%s' — widgets: %s"), *Spec, *Blueprint->GetName(),
						Names.IsEmpty() ? TEXT("(empty tree)") : *FString::Join(Names, TEXT(", "))));
			}
			return Widget;
		}

		UClass* WidgetClassOrError(
			const TSharedRef<FJsonObject>& Body, const TCHAR* Field, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			Body->TryGetStringField(Field, Spec);
			if (Spec.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					FString::Printf(TEXT("'%s' (widget class, e.g. TextBlock, Button, VerticalBox, or a ")
						TEXT("Widget Blueprint path) is required"), Field));
				return nullptr;
			}
			UClass* Class = ResolveClass(Spec);
			if (Class == nullptr || !Class->IsChildOf(UWidget::StaticClass()))
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_widget_class"),
					FString::Printf(TEXT("'%s' is not a widget class — see list_widget_classes"), *Spec));
				return nullptr;
			}
			return Class;
		}

		TSharedPtr<FJsonObject> PropertiesOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Body->TryGetObjectField(TEXT("properties"), Properties) || !Properties->IsValid())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'properties' object is required, e.g. {\"Text\": \"Play\", \"ColorAndOpacity\": ")
					TEXT("{\"SpecifiedColor\": {\"R\": 1, \"G\": 0.5, \"B\": 0, \"A\": 1}}}"));
				return nullptr;
			}
			return *Properties;
		}

		TArray<FString> KeysOf(const FJsonObject& Object)
		{
			TArray<FString> Keys;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
			{
				Keys.Add(Pair.Key);
			}
			return Keys;
		}

		FString PropertyTypeName(const FProperty* Property)
		{
			FString Extended;
			const FString Type = Property->GetCPPType(&Extended);
			return Type + Extended;
		}

		bool IsPlaceableWidgetClass(const UClass* Class)
		{
			if (!Class->IsChildOf(UWidget::StaticClass()) || Class == UUserWidget::StaticClass())
			{
				return false;
			}
			if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden))
			{
				return false;
			}
			const FString Name = Class->GetName();
			return !Name.StartsWith(TEXT("SKEL_")) && !Name.StartsWith(TEXT("REINST_"))
				&& !Name.StartsWith(TEXT("TRASHCLASS_")) && !Name.StartsWith(TEXT("PLACEHOLDER-CLASS"));
		}

		TSharedRef<FJsonObject> BoundEventToJson(const UK2Node_ComponentBoundEvent* Node)
		{
			const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("widget"), Node->ComponentPropertyName.ToString());
			Json->SetStringField(TEXT("event"), Node->DelegatePropertyName.ToString());
			Json->SetStringField(TEXT("graph"), Node->GetGraph() ? GraphPath(Node->GetGraph()) : TEXT(""));
			Json->SetObjectField(TEXT("node"), NodeToJson(Node));
			return Json;
		}
	}

	void RegisterWidgetBlueprintRoutes(FMcpLinkCoreModule& Core)
	{
		// ---------------------------------------------------------------- query
		Core.RegisterRoute(TEXT("/api/widget_blueprints/query"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list"))
				{
					FString PathPrefix = TEXT("/Game");
					Body->TryGetStringField(TEXT("path_prefix"), PathPrefix);
					const int32 MaxResults = FMath::Clamp(IntOr(Body, TEXT("max_results"), 100), 1, 500);

					IAssetRegistry& Registry =
						FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
					FARFilter Filter;
					Filter.ClassPaths.Add(UWidgetBlueprint::StaticClass()->GetClassPathName());
					Filter.PackagePaths.Add(FName(*PathPrefix));
					Filter.bRecursivePaths = true;
					Filter.bRecursiveClasses = true;
					TArray<FAssetData> Assets;
					Registry.GetAssets(Filter, Assets);

					TArray<TSharedPtr<FJsonValue>> Results;
					for (const FAssetData& Asset : Assets)
					{
						if (Results.Num() >= MaxResults)
						{
							break;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
						Item->SetStringField(TEXT("path"), Asset.PackageName.ToString());
						FString Tag;
						if (Asset.GetTagValue(TEXT("ParentClass"), Tag))
						{
							Item->SetStringField(TEXT("parent_class"), FPackageName::ExportTextPathToObjectPath(Tag));
						}
						Results.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Assets.Num());
					Data->SetArrayField(TEXT("widget_blueprints"), Results);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_widget_classes"))
				{
					FString FilterText;
					Body->TryGetStringField(TEXT("filter"), FilterText);
					const int32 MaxResults = FMath::Clamp(IntOr(Body, TEXT("max_results"), 200), 1, 1000);

					TArray<UClass*> Classes;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						UClass* Class = *It;
						if (!IsPlaceableWidgetClass(Class))
						{
							continue;
						}
						if (!FilterText.IsEmpty() && !Class->GetName().Contains(FilterText))
						{
							continue;
						}
						Classes.Add(Class);
					}
					Classes.Sort([](const UClass& A, const UClass& B) { return A.GetName() < B.GetName(); });

					TArray<TSharedPtr<FJsonValue>> Results;
					for (UClass* Class : Classes)
					{
						if (Results.Num() >= MaxResults)
						{
							break;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Class->GetName());
						Item->SetStringField(TEXT("path"), Class->GetPathName());
						if (Class->IsChildOf(UPanelWidget::StaticClass()))
						{
							Item->SetBoolField(TEXT("is_panel"), true);
							Item->SetBoolField(TEXT("multiple_children"),
								Class->GetDefaultObject<UPanelWidget>()->CanHaveMultipleChildren());
						}
						if (Class->IsChildOf(UUserWidget::StaticClass()))
						{
							Item->SetBoolField(TEXT("is_user_widget"), true);
						}
						Results.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Classes.Num());
					Data->SetArrayField(TEXT("classes"), Results);
					Data->SetStringField(TEXT("note"),
						TEXT("Blueprint widget classes appear once loaded — use their asset path in add_widget ")
						TEXT("(/Game/UI/WBP_Item) to load them on demand"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("widget_class_info"))
				{
					UClass* Class = WidgetClassOrError(Body, TEXT("class"), Responder);
					if (!Class) { return; }

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("name"), Class->GetName());
					Data->SetStringField(TEXT("path"), Class->GetPathName());
					Data->SetStringField(TEXT("parent_class"),
						Class->GetSuperClass() ? Class->GetSuperClass()->GetName() : TEXT(""));
					Data->SetBoolField(TEXT("placeable"), IsPlaceableWidgetClass(Class));
					if (Class->IsChildOf(UPanelWidget::StaticClass()))
					{
						Data->SetBoolField(TEXT("is_panel"), true);
						Data->SetBoolField(TEXT("multiple_children"),
							Class->GetDefaultObject<UPanelWidget>()->CanHaveMultipleChildren());
					}

					TArray<TSharedPtr<FJsonValue>> Events;
					for (TFieldIterator<FMulticastDelegateProperty> It(Class); It; ++It)
					{
						const TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
						Event->SetStringField(TEXT("name"), It->GetName());
						TArray<TSharedPtr<FJsonValue>> Parameters;
						if (const UFunction* Signature = It->SignatureFunction)
						{
							for (TFieldIterator<FProperty> Param(Signature); Param; ++Param)
							{
								if (!Param->HasAnyPropertyFlags(CPF_Parm) || Param->HasAnyPropertyFlags(CPF_ReturnParm))
								{
									continue;
								}
								const TSharedRef<FJsonObject> ParamJson = MakeShared<FJsonObject>();
								ParamJson->SetStringField(TEXT("name"), Param->GetName());
								ParamJson->SetStringField(TEXT("type"), PropertyTypeName(*Param));
								Parameters.Add(MakeShared<FJsonValueObject>(ParamJson));
							}
						}
						Event->SetArrayField(TEXT("parameters"), Parameters);
						Events.Add(MakeShared<FJsonValueObject>(Event));
					}
					Data->SetArrayField(TEXT("events"), Events);

					TArray<TSharedPtr<FJsonValue>> Properties;
					for (TFieldIterator<FProperty> It(Class); It; ++It)
					{
						if (!It->HasAnyPropertyFlags(CPF_Edit) || It->HasAnyPropertyFlags(CPF_Deprecated))
						{
							continue;
						}
						const TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
						Property->SetStringField(TEXT("name"), It->GetName());
						Property->SetStringField(TEXT("type"), PropertyTypeName(*It));
						const FString Category = It->GetMetaData(TEXT("Category"));
						if (!Category.IsEmpty())
						{
							Property->SetStringField(TEXT("category"), Category);
						}
						Properties.Add(MakeShared<FJsonValueObject>(Property));
					}
					Data->SetArrayField(TEXT("properties"), Properties);
					Data->SetStringField(TEXT("note"),
						TEXT("set these with widget_blueprint_modify set_widget {properties: {...}}; ")
						TEXT("slot (layout) properties live on the parent's slot — see set_slot"));
					Responder->Ok(Data);
					return;
				}

				UWidgetBlueprint* Blueprint = WidgetBlueprintOrError(Body, Responder);
				if (!Blueprint) { return; }

				if (Operation == TEXT("inspect"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("name"), Blueprint->GetName());
					Data->SetStringField(TEXT("path"), Blueprint->GetPathName());
					Data->SetStringField(TEXT("parent_class"),
						Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
					Data->SetStringField(TEXT("generated_class"),
						Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : TEXT(""));

					UWidget* Root = Blueprint->WidgetTree ? Blueprint->WidgetTree->RootWidget.Get() : nullptr;
					if (Root != nullptr)
					{
						Data->SetObjectField(TEXT("root"), Widgets::WidgetTreeToJson(Root, NAME_None));
					}
					else
					{
						Data->SetField(TEXT("root"), MakeShared<FJsonValueNull>());
					}
					Data->SetNumberField(TEXT("widget_count"), Widgets::WidgetNames(Blueprint).Num());

					TArray<TSharedPtr<FJsonValue>> Variables;
					for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Variable.VarName.ToString());
						Item->SetStringField(TEXT("type"), Variable.VarType.PinCategory.ToString());
						Variables.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetArrayField(TEXT("variables"), Variables);

					TArray<FMcpGraphEntry> Graphs;
					CollectGraphs(Blueprint, Graphs);
					TArray<TSharedPtr<FJsonValue>> GraphValues;
					TArray<TSharedPtr<FJsonValue>> BoundEvents;
					for (const FMcpGraphEntry& Entry : Graphs)
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Entry.Graph->GetName());
						Item->SetStringField(TEXT("path"), Entry.Path);
						Item->SetStringField(TEXT("category"), Entry.Category);
						Item->SetNumberField(TEXT("node_count"), Entry.Graph->Nodes.Num());
						GraphValues.Add(MakeShared<FJsonValueObject>(Item));

						TArray<UK2Node_ComponentBoundEvent*> Nodes;
						Entry.Graph->GetNodesOfClass(Nodes);
						for (const UK2Node_ComponentBoundEvent* Node : Nodes)
						{
							BoundEvents.Add(MakeShared<FJsonValueObject>(BoundEventToJson(Node)));
						}
					}
					Data->SetArrayField(TEXT("graphs"), GraphValues);
					Data->SetArrayField(TEXT("bound_events"), BoundEvents);

					TArray<TSharedPtr<FJsonValue>> Animations;
					for (const UWidgetAnimation* Animation : Blueprint->Animations)
					{
						if (Animation != nullptr)
						{
							Animations.Add(MakeShared<FJsonValueString>(Animation->GetDisplayName().ToString()));
						}
					}
					Data->SetArrayField(TEXT("animations"), Animations);

					TArray<TSharedPtr<FJsonValue>> Bindings;
					for (const FDelegateEditorBinding& Binding : Blueprint->Bindings)
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("widget"), Binding.ObjectName);
						Item->SetStringField(TEXT("property"), Binding.PropertyName.ToString());
						Item->SetStringField(TEXT("function"), Binding.FunctionName.ToString());
						Bindings.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetArrayField(TEXT("property_bindings"), Bindings);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("get_widget"))
				{
					UWidget* Widget = WidgetOrError(Blueprint, Body, TEXT("widget"), Responder);
					if (!Widget) { return; }
					const TSharedRef<FJsonObject> Data = Widgets::WidgetToJson(Widget, NAME_None, true);
					Data->SetObjectField(TEXT("properties"), Widgets::PropertiesToJson(Widget, {}));
					TArray<TSharedPtr<FJsonValue>> Events;
					for (const FString& Event : Widgets::EventNames(Widget->GetClass()))
					{
						Events.Add(MakeShared<FJsonValueString>(Event));
					}
					Data->SetArrayField(TEXT("events"), Events);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — use list, inspect, get_widget, ")
						TEXT("list_widget_classes, or widget_class_info"), *Operation));
			});

		// --------------------------------------------------------------- modify
		Core.RegisterRoute(TEXT("/api/widget_blueprints/modify"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path, ParentSpec, RootSpec = TEXT("CanvasPanel");
					Body->TryGetStringField(TEXT("path"), Path);
					Body->TryGetStringField(TEXT("parent_class"), ParentSpec);
					Body->TryGetStringField(TEXT("root_widget"), RootSpec);
					if (Path.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'path' is required, e.g. /Game/UI/WBP_Hud"));
						return;
					}
					UClass* ParentClass = nullptr;
					if (!ParentSpec.IsEmpty())
					{
						ParentClass = ResolveClass(ParentSpec);
						if (ParentClass == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_class"),
								FString::Printf(TEXT("parent class '%s' not found"), *ParentSpec));
							return;
						}
					}
					UClass* RootClass = nullptr;
					if (!RootSpec.IsEmpty() && RootSpec != TEXT("none"))
					{
						RootClass = ResolveClass(RootSpec);
						if (RootClass == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_widget_class"),
								FString::Printf(TEXT("root widget class '%s' not found — try CanvasPanel, ")
									TEXT("VerticalBox, HorizontalBox, Overlay, or \"none\""), *RootSpec));
							return;
						}
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateWidgetBlueprint", "McpLink Create Widget Blueprint"));
					FString Error;
					UWidgetBlueprint* Created = Widgets::CreateWidgetBlueprint(Path, ParentClass, RootClass, Error);
					if (Created == nullptr)
					{
						const bool bConflict = Error.Contains(TEXT("already exists"));
						Responder->Error(
							bConflict ? EHttpServerResponseCodes::Conflict : EHttpServerResponseCodes::BadRequest,
							bConflict ? TEXT("already_exists") : TEXT("create_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Created->GetPathName());
					Data->SetStringField(TEXT("name"), Created->GetName());
					Data->SetStringField(TEXT("parent_class"),
						Created->ParentClass ? Created->ParentClass->GetPathName() : TEXT(""));
					if (UWidget* Root = Created->WidgetTree ? Created->WidgetTree->RootWidget.Get() : nullptr)
					{
						Data->SetObjectField(TEXT("root"), Widgets::WidgetToJson(Root, NAME_None, false));
					}
					else
					{
						Data->SetField(TEXT("root"), MakeShared<FJsonValueNull>());
					}
					Data->SetStringField(TEXT("message"),
						TEXT("created in memory and compiled — add_widget next, then blueprint_modify save"));
					Responder->Ok(Data);
					return;
				}

				UWidgetBlueprint* Blueprint = WidgetBlueprintOrError(Body, Responder);
				if (!Blueprint) { return; }

				if (Operation == TEXT("add_widget"))
				{
					UClass* Class = WidgetClassOrError(Body, TEXT("class"), Responder);
					if (!Class) { return; }
					FString Name, ParentSpec;
					Body->TryGetStringField(TEXT("name"), Name);
					Body->TryGetStringField(TEXT("parent"), ParentSpec);
					UWidget* Parent = nullptr;
					if (!ParentSpec.IsEmpty())
					{
						Parent = WidgetOrError(Blueprint, Body, TEXT("parent"), Responder);
						if (!Parent) { return; }
					}
					TOptional<bool> bIsVariable;
					bool bVariableValue = false;
					if (Body->TryGetBoolField(TEXT("is_variable"), bVariableValue))
					{
						bIsVariable = bVariableValue;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddWidget", "McpLink Add Widget"));
					FString Error;
					UWidget* Widget = Widgets::AddWidget(
						Blueprint, Class, Name, Parent, IntOr(Body, TEXT("index"), -1), bIsVariable, Error);
					if (Widget == nullptr)
					{
						Responder->Error(
							Error.Contains(TEXT("already exists")) ? EHttpServerResponseCodes::Conflict
							                                        : EHttpServerResponseCodes::BadRequest,
							TEXT("add_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = Widgets::WidgetToJson(Widget, NAME_None, true);
					Data->SetStringField(TEXT("parent"),
						Widget->GetParent() ? Widget->GetParent()->GetName() : TEXT(""));
					Data->SetStringField(TEXT("message"),
						TEXT("configure with set_widget (appearance/behaviour) and set_slot (layout); ")
						TEXT("bind_event for OnClicked etc."));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_widget"))
				{
					UWidget* Widget = WidgetOrError(Blueprint, Body, TEXT("widget"), Responder);
					if (!Widget) { return; }
					const FString Name = Widget->GetName();
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "RemoveWidget", "McpLink Remove Widget"));
					FText Error;
					if (!FWidgetBlueprintOperationUtils::RemoveWidget(Blueprint, Widget, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("remove_failed"), Error.ToString());
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), Name);
					Data->SetNumberField(TEXT("widget_count"), Widgets::WidgetNames(Blueprint).Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("move_widget"))
				{
					UWidget* Widget = WidgetOrError(Blueprint, Body, TEXT("widget"), Responder);
					if (!Widget) { return; }
					UWidget* Parent = WidgetOrError(Blueprint, Body, TEXT("parent"), Responder);
					if (!Parent) { return; }
					UPanelWidget* Panel = Cast<UPanelWidget>(Parent);
					if (Panel == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("not_a_panel"),
							FString::Printf(TEXT("'%s' (%s) cannot hold children"),
								*Parent->GetName(), *Parent->GetClass()->GetName()));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "MoveWidget", "McpLink Move Widget"));
					FText Error;
					if (!FWidgetBlueprintOperationUtils::MoveWidget(
							Blueprint, Widget, Panel, IntOr(Body, TEXT("index"), -1), Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("move_failed"), Error.ToString());
						return;
					}
					Responder->Ok(Widgets::WidgetToJson(Widget, NAME_None, true));
					return;
				}

				if (Operation == TEXT("rename_widget"))
				{
					UWidget* Widget = WidgetOrError(Blueprint, Body, TEXT("widget"), Responder);
					if (!Widget) { return; }
					FString NewName;
					Body->TryGetStringField(TEXT("new_name"), NewName);
					if (NewName.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'new_name' is required"));
						return;
					}
					FText Error;
					if (!FWidgetBlueprintOperationUtils::VerifyWidgetRename(
							Blueprint, Widget, FText::FromString(NewName), Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_name"), Error.ToString());
						return;
					}
					if (!FWidgetBlueprintOperationUtils::RenameWidget(Blueprint, Widget, NewName))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("rename_failed"),
							FString::Printf(TEXT("could not rename to '%s' (name in use?)"), *NewName));
						return;
					}
					Responder->Ok(Widgets::WidgetToJson(Widget, NAME_None, false));
					return;
				}

				if (Operation == TEXT("set_widget") || Operation == TEXT("set_slot"))
				{
					UWidget* Widget = WidgetOrError(Blueprint, Body, TEXT("widget"), Responder);
					if (!Widget) { return; }
					const TSharedPtr<FJsonObject> Properties = PropertiesOrError(Body, Responder);
					if (!Properties.IsValid()) { return; }
					UObject* Target = Widget;
					if (Operation == TEXT("set_slot"))
					{
						Target = Widget->Slot.Get();
						if (Target == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("no_slot"),
								FString::Printf(TEXT("'%s' has no slot (it is the root or not in a panel) — ")
									TEXT("layout is set on children of panels"), *Widget->GetName()));
							return;
						}
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetWidgetProperties", "McpLink Set Widget Properties"));
					FString Error;
					if (!Widgets::ApplyJsonProperties(Target, Properties.ToSharedRef(), Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("set_failed"), Error);
						return;
					}
					FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("widget"), Widget->GetName());
					Data->SetStringField(TEXT("target"), Target->GetPathName());
					Data->SetObjectField(TEXT("properties"), Widgets::PropertiesToJson(Target, KeysOf(*Properties)));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_variable"))
				{
					UWidget* Widget = WidgetOrError(Blueprint, Body, TEXT("widget"), Responder);
					if (!Widget) { return; }
					bool bIsVariable = true;
					Body->TryGetBoolField(TEXT("is_variable"), bIsVariable);
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetWidgetVariable", "McpLink Set Widget Is Variable"));
					FWidgetBlueprintOperationUtils::ToggleWidgetAsVariable(Blueprint, Widget, bIsVariable, true);
					Responder->Ok(Widgets::WidgetToJson(Widget, NAME_None, false));
					return;
				}

				if (Operation == TEXT("bind_event"))
				{
					UWidget* Widget = WidgetOrError(Blueprint, Body, TEXT("widget"), Responder);
					if (!Widget) { return; }
					FString EventName;
					Body->TryGetStringField(TEXT("event"), EventName);
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "BindWidgetEvent", "McpLink Bind Widget Event"));
					bool bAlreadyBound = false;
					FString Error;
					UK2Node_ComponentBoundEvent* Node =
						Widgets::BindEvent(Blueprint, Widget, FName(*EventName), bAlreadyBound, Error);
					if (Node == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bind_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = BoundEventToJson(Node);
					Data->SetBoolField(TEXT("already_bound"), bAlreadyBound);
					Data->SetStringField(TEXT("message"),
						TEXT("wire logic from this node's exec pin with blueprint_modify add_node / connect_pins ")
						TEXT("in the returned graph, then blueprint_modify compile and save"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("wrap_widget"))
				{
					UWidget* Widget = WidgetOrError(Blueprint, Body, TEXT("widget"), Responder);
					if (!Widget) { return; }
					UClass* Class = WidgetClassOrError(Body, TEXT("class"), Responder);
					if (!Class) { return; }
					if (!Class->IsChildOf(UPanelWidget::StaticClass()))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("not_a_panel"),
							FString::Printf(TEXT("'%s' is not a panel class"), *Class->GetName()));
						return;
					}
					const TArray<UWidget*> Wrappers =
						FWidgetBlueprintOperationUtils::WrapWidgets(Blueprint, { Widget }, Class);
					if (Wrappers.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("wrap_failed"),
							FString::Printf(TEXT("could not wrap '%s' in a %s"), *Widget->GetName(), *Class->GetName()));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetObjectField(TEXT("wrapper"), Widgets::WidgetToJson(Wrappers[0], NAME_None, true));
					Data->SetObjectField(TEXT("widget"), Widgets::WidgetToJson(Widget, NAME_None, true));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("replace_widget"))
				{
					UWidget* Widget = WidgetOrError(Blueprint, Body, TEXT("widget"), Responder);
					if (!Widget) { return; }
					UClass* Class = WidgetClassOrError(Body, TEXT("class"), Responder);
					if (!Class) { return; }
					const FName Name = Widget->GetFName();
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "ReplaceWidget", "McpLink Replace Widget"));
					FText Error;
					if (!FWidgetBlueprintOperationUtils::ReplaceWidgetWithTemplate(Blueprint, Widget, Class, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("replace_failed"), Error.ToString());
						return;
					}
					UWidget* Replacement = Blueprint->WidgetTree ? Blueprint->WidgetTree->FindWidget(Name) : nullptr;
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					if (Replacement != nullptr)
					{
						Data->SetObjectField(TEXT("widget"), Widgets::WidgetToJson(Replacement, NAME_None, true));
					}
					Data->SetStringField(TEXT("replaced"), Name.ToString());
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, add_widget, remove_widget, move_widget, ")
						TEXT("rename_widget, set_widget, set_slot, set_variable, bind_event, wrap_widget, or ")
						TEXT("replace_widget (compile/save via blueprint_modify)"),
						*Operation));
			});
	}
}
