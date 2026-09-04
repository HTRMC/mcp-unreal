// Blueprint inspection and editing.
//
// Unlike the reference implementation this spawns nodes through
// FGraphNodeCreator and configures them (so a CallFunction node actually
// targets a function), validates links through the schema instead of calling
// MakeLinkTo directly, and wraps every mutation in a transaction.

#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintEditorLibrary.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "McpBlueprintUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace McpLink
{
	namespace
	{
		UBlueprint* BlueprintOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!Body->TryGetStringField(TEXT("blueprint"), Path) || Path.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'blueprint' asset path is required, e.g. /Game/Blueprints/BP_Thing"));
				return nullptr;
			}
			UBlueprint* Blueprint = ResolveBlueprint(Path);
			if (Blueprint == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("blueprint_not_found"),
					FString::Printf(
						TEXT("no Blueprint asset at '%s' — use blueprint_query list to discover"), *Path));
			}
			return Blueprint;
		}

		UEdGraph* GraphOrError(
			UBlueprint* Blueprint,
			const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString GraphName;
			Body->TryGetStringField(TEXT("graph"), GraphName);
			UEdGraph* Graph = FindGraph(Blueprint, GraphName);
			if (Graph == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("graph_not_found"),
					FString::Printf(
						TEXT("no graph '%s' in %s — use blueprint_query inspect to list graphs"),
						*GraphName, *Blueprint->GetName()));
			}
			return Graph;
		}

		FString StringOr(
			const TSharedRef<FJsonObject>& Body, const TCHAR* Field, const TCHAR* Default)
		{
			FString Value;
			return Body->TryGetStringField(Field, Value) && !Value.IsEmpty() ? Value : FString(Default);
		}

		void MarkModified(UBlueprint* Blueprint, bool bStructural)
		{
			if (bStructural)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			}
			else
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			}
		}
	}

	void RegisterBlueprintRoutes(FMcpLinkCoreModule& Core)
	{
		// ---------------------------------------------------------------- query
		Core.RegisterRoute(TEXT("/api/blueprints/query"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list"))
				{
					FString PathPrefix = TEXT("/Game");
					Body->TryGetStringField(TEXT("path_prefix"), PathPrefix);
					const int32 MaxResults = FMath::Clamp(
						Body->HasTypedField<EJson::Number>(TEXT("max_results"))
							? static_cast<int32>(Body->GetNumberField(TEXT("max_results")))
							: 100,
						1, 500);

					IAssetRegistry& Registry =
						FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
					FARFilter Filter;
					Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
					Filter.PackagePaths.Add(FName(*PathPrefix));
					Filter.bRecursivePaths = true;
					// Anim / Widget Blueprints are UBlueprint subclasses.
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
						Item->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
						FString ParentClass;
						if (Asset.GetTagValue(TEXT("ParentClass"), ParentClass))
						{
							Item->SetStringField(TEXT("parent_class"),
								FPackageName::ExportTextPathToObjectPath(ParentClass));
						}
						Results.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Assets.Num());
					Data->SetArrayField(TEXT("blueprints"), Results);
					Responder->Ok(Data);
					return;
				}

				UBlueprint* Blueprint = BlueprintOrError(Body, Responder);
				if (!Blueprint) { return; }

				if (Operation == TEXT("inspect"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("name"), Blueprint->GetName());
					Data->SetStringField(TEXT("path"), Blueprint->GetPathName());
					Data->SetStringField(TEXT("parent_class"),
						Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));

					TArray<TSharedPtr<FJsonValue>> Variables;
					for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Variable.VarName.ToString());
						Item->SetStringField(TEXT("type"), Variable.VarType.PinCategory.ToString());
						if (const UObject* SubObject = Variable.VarType.PinSubCategoryObject.Get())
						{
							Item->SetStringField(TEXT("type_object"), SubObject->GetPathName());
						}
						Item->SetStringField(TEXT("default"), Variable.DefaultValue);
						Variables.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetArrayField(TEXT("variables"), Variables);

					TArray<FMcpGraphEntry> Graphs;
					CollectGraphs(Blueprint, Graphs);
					TArray<TSharedPtr<FJsonValue>> GraphValues;
					for (const FMcpGraphEntry& Entry : Graphs)
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Entry.Graph->GetName());
						// Sub-graphs (states, transition rules, collapsed graphs)
						// are addressed by this path in every 'graph' field.
						Item->SetStringField(TEXT("path"), Entry.Path);
						Item->SetStringField(TEXT("category"), Entry.Category);
						Item->SetNumberField(TEXT("node_count"), Entry.Graph->Nodes.Num());
						GraphValues.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetArrayField(TEXT("graphs"), GraphValues);

					TArray<TSharedPtr<FJsonValue>> Components;
					if (Blueprint->SimpleConstructionScript != nullptr)
					{
						for (const USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
						{
							if (Node == nullptr)
							{
								continue;
							}
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
							Item->SetStringField(TEXT("class"),
								Node->ComponentClass ? Node->ComponentClass->GetName() : TEXT(""));
							Components.Add(MakeShared<FJsonValueObject>(Item));
						}
					}
					Data->SetArrayField(TEXT("components"), Components);

					TArray<TSharedPtr<FJsonValue>> Interfaces;
					for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
					{
						if (const UClass* Class = Interface.Interface.Get())
						{
							Interfaces.Add(MakeShared<FJsonValueString>(Class->GetPathName()));
						}
					}
					Data->SetArrayField(TEXT("interfaces"), Interfaces);

					TArray<TSharedPtr<FJsonValue>> Dispatchers;
					for (const FName& Name : UBlueprintEditorLibrary::ListEventDispatchers(Blueprint))
					{
						Dispatchers.Add(MakeShared<FJsonValueString>(Name.ToString()));
					}
					Data->SetArrayField(TEXT("event_dispatchers"), Dispatchers);

					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("get_graph"))
				{
					UEdGraph* Graph = GraphOrError(Blueprint, Body, Responder);
					if (!Graph) { return; }

					TArray<TSharedPtr<FJsonValue>> Nodes;
					for (const UEdGraphNode* Node : Graph->Nodes)
					{
						if (Node != nullptr)
						{
							Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node)));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("graph"), Graph->GetName());
					Data->SetArrayField(TEXT("nodes"), Nodes);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list, inspect, or get_graph"), *Operation));
			});

		// --------------------------------------------------------------- modify
		Core.RegisterRoute(TEXT("/api/blueprints/modify"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				// ---- create is the one operation without an existing asset ----
				if (Operation == TEXT("create"))
				{
					FString Path, ParentSpec;
					if (!Body->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'path' is required, e.g. /Game/Blueprints/BP_Thing"));
						return;
					}
					Body->TryGetStringField(TEXT("parent_class"), ParentSpec);
					UClass* ParentClass = ParentSpec.IsEmpty()
						? AActor::StaticClass()
						: ResolveClass(ParentSpec);
					if (ParentClass == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_class"),
							FString::Printf(TEXT("parent class '%s' not found"), *ParentSpec));
						return;
					}
					if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_parent"),
							FString::Printf(
								TEXT("cannot create a Blueprint from '%s'"), *ParentClass->GetName()));
						return;
					}

					const FString PackageName = Path;
					const FString AssetName = FPackageName::GetShortName(PackageName);
					if (FindPackage(nullptr, *PackageName) != nullptr
						|| FPackageName::DoesPackageExist(PackageName))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *PackageName));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateBlueprint", "McpLink Create Blueprint"));
					UPackage* Package = CreatePackage(*PackageName);
					UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
						ParentClass, Package, FName(*AssetName), BPTYPE_Normal,
						UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
					if (Blueprint == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("create_failed"),
							TEXT("CreateBlueprint returned null"));
						return;
					}
					FAssetRegistryModule::AssetCreated(Blueprint);
					Package->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Blueprint->GetPathName());
					Data->SetStringField(TEXT("name"), Blueprint->GetName());
					Data->SetStringField(TEXT("parent_class"), ParentClass->GetPathName());
					Data->SetStringField(TEXT("message"),
						TEXT("created in memory — call blueprint_modify save to write it to disk"));
					Responder->Ok(Data);
					return;
				}

				UBlueprint* Blueprint = BlueprintOrError(Body, Responder);
				if (!Blueprint) { return; }

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "ModifyBlueprint", "McpLink Modify Blueprint"));
				Blueprint->Modify();

				// ---- variables ----
				if (Operation == TEXT("add_variable"))
				{
					FString VarName, TypeName;
					Body->TryGetStringField(TEXT("name"), VarName);
					Body->TryGetStringField(TEXT("type"), TypeName);
					if (VarName.IsEmpty() || TypeName.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'name' and 'type' are required"));
						return;
					}
					FEdGraphPinType PinType;
					FString TypeError;
					if (!MakePinType(TypeName, PinType, TypeError))
					{
						Responder->Error(
							EHttpServerResponseCodes::BadRequest, TEXT("unknown_type"), TypeError);
						return;
					}
					FString DefaultValue;
					Body->TryGetStringField(TEXT("default"), DefaultValue);
					if (!FBlueprintEditorUtils::AddMemberVariable(
						Blueprint, FName(*VarName), PinType, DefaultValue))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_failed"),
							FString::Printf(
								TEXT("could not add variable '%s' — a member with that name may already exist"),
								*VarName));
						return;
					}
					MarkModified(Blueprint, true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("variable"), VarName);
					Data->SetStringField(TEXT("type"), TypeName);
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("remove_variable"))
				{
					FString VarName;
					Body->TryGetStringField(TEXT("name"), VarName);
					if (VarName.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'name' is required"));
						return;
					}
					FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*VarName));
					MarkModified(Blueprint, true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), VarName);
					Responder->Ok(Data);
					return;
				}

				// ---- function graphs ----
				if (Operation == TEXT("add_function"))
				{
					FString FunctionName;
					Body->TryGetStringField(TEXT("name"), FunctionName);
					if (FunctionName.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'name' is required"));
						return;
					}
					UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
						Blueprint, FName(*FunctionName), UEdGraph::StaticClass(),
						UEdGraphSchema_K2::StaticClass());
					FBlueprintEditorUtils::AddFunctionGraph<UClass>(
						Blueprint, Graph, /*bIsUserCreated*/ true, nullptr);
					MarkModified(Blueprint, true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("graph"), Graph->GetName());
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("remove_function"))
				{
					UEdGraph* Graph = GraphOrError(Blueprint, Body, Responder);
					if (!Graph) { return; }
					const FString Name = Graph->GetName();
					FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph);
					MarkModified(Blueprint, true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), Name);
					Responder->Ok(Data);
					return;
				}

				// ---- nodes and pins ----
				if (Operation == TEXT("add_node"))
				{
					UEdGraph* Graph = GraphOrError(Blueprint, Body, Responder);
					if (!Graph) { return; }

					// The node vocabulary lives in McpBlueprintNodes.cpp.
					FString Failure;
					UEdGraphNode* Created = CreateGraphNode(Blueprint, Graph, Body, Failure);
					if (Created == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("node_not_created"),
							Failure.IsEmpty() ? TEXT("node creation failed") : Failure);
						return;
					}
					Created->NodePosX = IntOr(Body, TEXT("x"), 0);
					Created->NodePosY = IntOr(Body, TEXT("y"), 0);
					MarkModified(Blueprint, true);
					Responder->Ok(NodeToJson(Created));
					return;
				}

				if (Operation == TEXT("delete_node"))
				{
					UEdGraph* Graph = GraphOrError(Blueprint, Body, Responder);
					if (!Graph) { return; }
					FString NodeGuid;
					Body->TryGetStringField(TEXT("node"), NodeGuid);
					UEdGraphNode* Node = FindNodeByGuid(Graph, NodeGuid);
					if (Node == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"),
							FString::Printf(TEXT("no node with guid '%s' in graph '%s'"),
								*NodeGuid, *Graph->GetName()));
						return;
					}
					FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile*/ true);
					MarkModified(Blueprint, true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), NodeGuid);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("connect_pins") || Operation == TEXT("disconnect_pins"))
				{
					UEdGraph* Graph = GraphOrError(Blueprint, Body, Responder);
					if (!Graph) { return; }

					FString FromNode, FromPin, ToNode, ToPin;
					Body->TryGetStringField(TEXT("from_node"), FromNode);
					Body->TryGetStringField(TEXT("from_pin"), FromPin);
					Body->TryGetStringField(TEXT("to_node"), ToNode);
					Body->TryGetStringField(TEXT("to_pin"), ToPin);

					UEdGraphPin* SourcePin = FindPin(FindNodeByGuid(Graph, FromNode), FromPin);
					UEdGraphPin* TargetPin = FindPin(FindNodeByGuid(Graph, ToNode), ToPin);
					if (SourcePin == nullptr || TargetPin == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("pin_not_found"),
							TEXT("could not resolve both pins — check node guids and pin names from get_graph"));
						return;
					}

					const UEdGraphSchema* Schema = Graph->GetSchema();
					if (Operation == TEXT("disconnect_pins"))
					{
						SourcePin->BreakLinkTo(TargetPin);
					}
					else
					{
						// Schema validation is what stops type-incompatible links.
						const FPinConnectionResponse Response =
							Schema->CanCreateConnection(SourcePin, TargetPin);
						if (Response.Response == CONNECT_RESPONSE_DISALLOW)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_connection"),
								FString::Printf(TEXT("cannot connect these pins: %s"),
									*Response.Message.ToString()));
							return;
						}
						if (!Schema->TryCreateConnection(SourcePin, TargetPin))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("connect_failed"),
								TEXT("the schema refused the connection"));
							return;
						}
					}
					MarkModified(Blueprint, true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("connected"), Operation == TEXT("connect_pins"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_pin_value"))
				{
					UEdGraph* Graph = GraphOrError(Blueprint, Body, Responder);
					if (!Graph) { return; }
					FString NodeGuid, PinName, Value;
					Body->TryGetStringField(TEXT("node"), NodeGuid);
					Body->TryGetStringField(TEXT("pin"), PinName);
					Body->TryGetStringField(TEXT("value"), Value);
					UEdGraphPin* Pin = FindPin(FindNodeByGuid(Graph, NodeGuid), PinName);
					if (Pin == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("pin_not_found"),
							FString::Printf(TEXT("no pin '%s' on node '%s'"), *PinName, *NodeGuid));
						return;
					}
					// Through the schema so validation and callbacks run.
					Graph->GetSchema()->TrySetDefaultValue(*Pin, Value, /*bMarkAsModified*/ true);
					MarkModified(Blueprint, false);
					Responder->Ok(PinToJson(Pin));
					return;
				}

				// ---- compile and save ----
				if (Operation == TEXT("compile"))
				{
					FKismetEditorUtilities::CompileBlueprint(Blueprint);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					const bool bUpToDate = Blueprint->Status == BS_UpToDate
						|| Blueprint->Status == BS_UpToDateWithWarnings;
					Data->SetBoolField(TEXT("compiled"), bUpToDate);
					Data->SetNumberField(TEXT("status"), static_cast<int32>(Blueprint->Status));
					Data->SetStringField(TEXT("status_text"),
						Blueprint->Status == BS_Error ? TEXT("error")
						: Blueprint->Status == BS_UpToDateWithWarnings ? TEXT("warnings")
						: bUpToDate ? TEXT("up_to_date") : TEXT("dirty"));
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("save"))
				{
					UPackage* Package = Blueprint->GetOutermost();
					const FString Filename = FPackageName::LongPackageNameToFilename(
						Package->GetName(), FPackageName::GetAssetPackageExtension());
					FSavePackageArgs SaveArgs;
					SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
					const bool bSaved =
						UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("saved"), bSaved);
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				// ---- components (the Simple Construction Script tree) ----
				if (Operation == TEXT("add_component") || Operation == TEXT("remove_component"))
				{
					USimpleConstructionScript* Scs = Blueprint->SimpleConstructionScript;
					if (Scs == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_component_tree"),
							TEXT("only Actor Blueprints have a component tree"));
						return;
					}
					FString Name;
					if (!Body->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'name' is required — the component's variable name"));
						return;
					}

					if (Operation == TEXT("remove_component"))
					{
						USCS_Node* Node = Scs->FindSCSNode(FName(*Name));
						if (Node == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("component_not_found"),
								FString::Printf(TEXT("no component '%s' — blueprint_query inspect lists them"), *Name));
							return;
						}
						Scs->RemoveNodeAndPromoteChildren(Node);
						MarkModified(Blueprint, true);
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("removed"), Name);
						Responder->Ok(Data);
						return;
					}

					FString ClassSpec;
					if (!Body->TryGetStringField(TEXT("class"), ClassSpec) || ClassSpec.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'class' is required, e.g. StaticMeshComponent, /Script/Engine.PointLightComponent"));
						return;
					}
					UClass* ComponentClass = ResolveClass(ClassSpec);
					if (ComponentClass == nullptr
						|| !ComponentClass->IsChildOf(UActorComponent::StaticClass())
						|| ComponentClass->HasAnyClassFlags(CLASS_Abstract))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_class"),
							FString::Printf(
								TEXT("'%s' is not a concrete ActorComponent subclass"), *ClassSpec));
						return;
					}
					// Resolve the parent first: a failed lookup after CreateNode
					// would leave an orphaned node behind.
					USCS_Node* Parent = nullptr;
					FString ParentName;
					if (Body->TryGetStringField(TEXT("parent"), ParentName) && !ParentName.IsEmpty())
					{
						Parent = Scs->FindSCSNode(FName(*ParentName));
						if (Parent == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("component_not_found"),
								FString::Printf(TEXT("no parent component '%s'"), *ParentName));
							return;
						}
						if (!ComponentClass->IsChildOf(USceneComponent::StaticClass()))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_parent"),
								TEXT("only scene components can be attached to another component"));
							return;
						}
					}
					USCS_Node* NewNode = Scs->CreateNode(ComponentClass, FName(*Name));
					if (NewNode == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("add_failed"),
							FString::Printf(TEXT("could not create a %s component"), *ClassSpec));
						return;
					}
					if (Parent != nullptr)
					{
						Parent->AddChildNode(NewNode);
					}
					else
					{
						Scs->AddNode(NewNode);
					}
					MarkModified(Blueprint, true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					// CreateNode uniquifies the name, so report what it settled on.
					Data->SetStringField(TEXT("component"), NewNode->GetVariableName().ToString());
					Data->SetStringField(TEXT("class"), ComponentClass->GetPathName());
					Data->SetStringField(TEXT("template"),
						NewNode->ComponentTemplate ? NewNode->ComponentTemplate->GetPathName() : TEXT(""));
					if (Parent != nullptr)
					{
						Data->SetStringField(TEXT("parent"), Parent->GetVariableName().ToString());
					}
					Responder->Ok(Data);
					return;
				}

				// ---- parent class ----
				if (Operation == TEXT("set_parent_class"))
				{
					FString ClassSpec;
					if (!Body->TryGetStringField(TEXT("parent_class"), ClassSpec) || ClassSpec.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'parent_class' is required"));
						return;
					}
					UClass* NewParent = ResolveClass(ClassSpec);
					if (NewParent == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("unknown_class"),
							FString::Printf(TEXT("no class '%s'"), *ClassSpec));
						return;
					}
					if (Blueprint->GeneratedClass != nullptr
						&& NewParent->IsChildOf(Blueprint->GeneratedClass))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("circular_parent"),
							TEXT("a Blueprint cannot inherit from itself or one of its own children"));
						return;
					}
					if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(NewParent))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_parent"),
							FString::Printf(
								TEXT("'%s' cannot be a Blueprint parent"), *NewParent->GetName()));
						return;
					}
					UBlueprintEditorLibrary::ReparentBlueprint(Blueprint, NewParent);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("parent_class"),
						Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
					Responder->Ok(Data);
					return;
				}

				// ---- variable type ----
				if (Operation == TEXT("set_variable_type"))
				{
					FString VarName, TypeName;
					Body->TryGetStringField(TEXT("name"), VarName);
					Body->TryGetStringField(TEXT("type"), TypeName);
					if (VarName.IsEmpty() || TypeName.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'name' and 'type' are required"));
						return;
					}
					FEdGraphPinType PinType;
					FString TypeError;
					if (!MakePinType(TypeName, PinType, TypeError))
					{
						Responder->Error(
							EHttpServerResponseCodes::BadRequest, TEXT("unknown_type"), TypeError);
						return;
					}
					UBlueprintEditorLibrary::ChangeMemberVariableType(Blueprint, FName(*VarName), PinType);
					MarkModified(Blueprint, true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("variable"), VarName);
					Data->SetStringField(TEXT("type"), TypeName);
					Responder->Ok(Data);
					return;
				}

				// ---- interfaces ----
				if (Operation == TEXT("add_interface") || Operation == TEXT("remove_interface"))
				{
					FString ClassSpec;
					if (!Body->TryGetStringField(TEXT("interface"), ClassSpec) || ClassSpec.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'interface' is required, e.g. /Script/Engine.AbilitySystemInterface or a BPI asset path"));
						return;
					}
					UClass* Interface = ResolveClass(ClassSpec);
					if (Interface == nullptr || !Interface->HasAnyClassFlags(CLASS_Interface))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_interface"),
							FString::Printf(TEXT("'%s' is not an interface class"), *ClassSpec));
						return;
					}
					if (Operation == TEXT("remove_interface"))
					{
						const bool bPreserve = BoolOr(Body, TEXT("preserve_functions"), false);
						FBlueprintEditorUtils::RemoveInterface(
							Blueprint, Interface->GetClassPathName(), bPreserve);
						MarkModified(Blueprint, true);
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("removed"), Interface->GetPathName());
						Responder->Ok(Data);
						return;
					}
					if (!FBlueprintEditorUtils::ImplementNewInterface(
						Blueprint, Interface->GetClassPathName()))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_failed"),
							FString::Printf(
								TEXT("could not implement '%s' — it may already be implemented, or conflict with a member name"),
								*Interface->GetName()));
						return;
					}
					MarkModified(Blueprint, true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("interface"), Interface->GetPathName());
					Responder->Ok(Data);
					return;
				}

				// ---- event dispatchers (multicast delegates) ----
				if (Operation == TEXT("add_event_dispatcher")
					|| Operation == TEXT("remove_event_dispatcher"))
				{
					FString Name;
					if (!Body->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'name' is required"));
						return;
					}
					const bool bAdd = Operation == TEXT("add_event_dispatcher");
					const bool bOk = bAdd
						? UBlueprintEditorLibrary::AddEventDispatcher(Blueprint, FName(*Name))
						: UBlueprintEditorLibrary::RemoveEventDispatcher(Blueprint, FName(*Name));
					if (!bOk)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("dispatcher_failed"),
							bAdd
								? FString::Printf(TEXT("could not add '%s' — the name is already in use"), *Name)
								: FString::Printf(TEXT("no event dispatcher '%s'"), *Name));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(bAdd ? TEXT("dispatcher") : TEXT("removed"), Name);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_dispatcher_parameter")
					|| Operation == TEXT("remove_dispatcher_parameter"))
				{
					FString Dispatcher, Name;
					Body->TryGetStringField(TEXT("dispatcher"), Dispatcher);
					Body->TryGetStringField(TEXT("name"), Name);
					if (Dispatcher.IsEmpty() || Name.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'dispatcher' and 'name' are required"));
						return;
					}
					bool bOk = false;
					if (Operation == TEXT("remove_dispatcher_parameter"))
					{
						bOk = UBlueprintEditorLibrary::RemoveEventDispatcherParameter(
							Blueprint, FName(*Dispatcher), FName(*Name));
					}
					else
					{
						FString TypeName;
						Body->TryGetStringField(TEXT("type"), TypeName);
						FEdGraphPinType PinType;
						FString TypeError;
						if (!MakePinType(TypeName, PinType, TypeError))
						{
							Responder->Error(
								EHttpServerResponseCodes::BadRequest, TEXT("unknown_type"), TypeError);
							return;
						}
						bOk = UBlueprintEditorLibrary::AddEventDispatcherParameter(
							Blueprint, FName(*Dispatcher), FName(*Name), PinType);
					}
					if (!bOk)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("parameter_failed"),
							FString::Printf(
								TEXT("could not change '%s' on dispatcher '%s' — check both names"),
								*Name, *Dispatcher));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("dispatcher"), Dispatcher);
					Data->SetStringField(TEXT("parameter"), Name);
					Responder->Ok(Data);
					return;
				}

				// ---- function signature and locals ----
				if (Operation == TEXT("add_function_parameter")
					|| Operation == TEXT("remove_function_parameter")
					|| Operation == TEXT("add_local_variable")
					|| Operation == TEXT("remove_local_variable"))
				{
					UEdGraph* Graph = GraphOrError(Blueprint, Body, Responder);
					if (!Graph) { return; }
					FString Name;
					if (!Body->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'name' is required"));
						return;
					}

					if (Operation.EndsWith(TEXT("local_variable")))
					{
						if (Operation == TEXT("remove_local_variable"))
						{
							UStruct* Scope = Blueprint->SkeletonGeneratedClass != nullptr
								? Blueprint->SkeletonGeneratedClass->FindFunctionByName(Graph->GetFName())
								: nullptr;
							if (Scope == nullptr)
							{
								Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_scope"),
									TEXT("the function has not been compiled yet — call compile first"));
								return;
							}
							FBlueprintEditorUtils::RemoveLocalVariable(Blueprint, Scope, FName(*Name));
							MarkModified(Blueprint, true);
							const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
							Data->SetStringField(TEXT("removed"), Name);
							Responder->Ok(Data);
							return;
						}
						FString TypeName, DefaultValue;
						Body->TryGetStringField(TEXT("type"), TypeName);
						Body->TryGetStringField(TEXT("default"), DefaultValue);
						FEdGraphPinType PinType;
						FString TypeError;
						if (!MakePinType(TypeName, PinType, TypeError))
						{
							Responder->Error(
								EHttpServerResponseCodes::BadRequest, TEXT("unknown_type"), TypeError);
							return;
						}
						if (!FBlueprintEditorUtils::AddLocalVariable(
							Blueprint, Graph, FName(*Name), PinType, DefaultValue))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_failed"),
								FString::Printf(
									TEXT("could not add local '%s' to '%s' — locals only exist on function graphs, ")
									TEXT("and the name must be free"),
									*Name, *Graph->GetName()));
							return;
						}
						MarkModified(Blueprint, true);
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("local_variable"), Name);
						Data->SetStringField(TEXT("graph"), Graph->GetName());
						Responder->Ok(Data);
						return;
					}

					TArray<UK2Node_FunctionEntry*> Entries;
					Graph->GetNodesOfClass(Entries);
					if (Entries.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_a_function"),
							FString::Printf(
								TEXT("graph '%s' has no function entry node — parameters only exist on function graphs"),
								*Graph->GetName()));
						return;
					}
					UK2Node_FunctionEntry* Entry = Entries[0];
					const bool bOutput =
						StringOr(Body, TEXT("direction"), TEXT("input")).Equals(TEXT("output"), ESearchCase::IgnoreCase);

					if (Operation == TEXT("remove_function_parameter"))
					{
						Entry->Modify();
						Entry->RemoveUserDefinedPinByName(FName(*Name));
						TArray<UK2Node_FunctionResult*> Results;
						Graph->GetNodesOfClass(Results);
						for (UK2Node_FunctionResult* Result : Results)
						{
							Result->Modify();
							Result->RemoveUserDefinedPinByName(FName(*Name));
						}
						MarkModified(Blueprint, true);
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("removed"), Name);
						Responder->Ok(Data);
						return;
					}

					FString TypeName;
					Body->TryGetStringField(TEXT("type"), TypeName);
					FEdGraphPinType PinType;
					FString TypeError;
					if (!MakePinType(TypeName, PinType, TypeError))
					{
						Responder->Error(
							EHttpServerResponseCodes::BadRequest, TEXT("unknown_type"), TypeError);
						return;
					}
					UEdGraphPin* NewPin = nullptr;
					if (bOutput)
					{
						// The result node is created on demand, the way the
						// details panel's "New Parameter" button does it.
						UK2Node_FunctionResult* Result =
							FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry);
						if (Result == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_result_node"),
								TEXT("could not create the function's return node"));
							return;
						}
						Result->Modify();
						NewPin = Result->CreateUserDefinedPin(FName(*Name), PinType, EGPD_Input);
					}
					else
					{
						Entry->Modify();
						// An input parameter is an *output* pin of the entry node.
						NewPin = Entry->CreateUserDefinedPin(FName(*Name), PinType, EGPD_Output);
					}
					if (NewPin == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_failed"),
							FString::Printf(
								TEXT("could not add parameter '%s' — the name may already be taken"), *Name));
						return;
					}
					MarkModified(Blueprint, true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("parameter"), NewPin->PinName.ToString());
					Data->SetStringField(TEXT("direction"), bOutput ? TEXT("output") : TEXT("input"));
					Data->SetStringField(TEXT("graph"), Graph->GetName());
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, add_variable, remove_variable, ")
						TEXT("set_variable_type, add_function, remove_function, add_function_parameter, ")
						TEXT("remove_function_parameter, add_local_variable, remove_local_variable, ")
						TEXT("add_component, remove_component, set_parent_class, add_interface, ")
						TEXT("remove_interface, add_event_dispatcher, remove_event_dispatcher, ")
						TEXT("add_dispatcher_parameter, remove_dispatcher_parameter, add_node, ")
						TEXT("delete_node, connect_pins, disconnect_pins, set_pin_value, compile, or save"),
						*Operation));
			});
	}
}
