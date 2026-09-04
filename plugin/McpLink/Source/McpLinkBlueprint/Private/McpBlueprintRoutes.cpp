// Blueprint inspection and editing.
//
// Unlike the reference implementation this spawns nodes through
// FGraphNodeCreator and configures them (so a CallFunction node actually
// targets a function), validates links through the schema instead of calling
// MakeLinkTo directly, and wraps every mutation in a transaction.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
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

					FString NodeType;
					Body->TryGetStringField(TEXT("node_type"), NodeType);
					const int32 PosX = static_cast<int32>(
						Body->HasTypedField<EJson::Number>(TEXT("x")) ? Body->GetNumberField(TEXT("x")) : 0.0);
					const int32 PosY = static_cast<int32>(
						Body->HasTypedField<EJson::Number>(TEXT("y")) ? Body->GetNumberField(TEXT("y")) : 0.0);

					UEdGraphNode* Created = nullptr;
					FString Failure;

					if (NodeType.Equals(TEXT("call_function"), ESearchCase::IgnoreCase))
					{
						FString FunctionName, ClassSpec;
						Body->TryGetStringField(TEXT("function"), FunctionName);
						Body->TryGetStringField(TEXT("class"), ClassSpec);
						UClass* OwnerClass = ClassSpec.IsEmpty()
							? (Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get()
														 : Blueprint->ParentClass.Get())
							: ResolveClass(ClassSpec);
						UFunction* Function =
							OwnerClass ? OwnerClass->FindFunctionByName(FName(*FunctionName)) : nullptr;
						if (Function == nullptr && !ClassSpec.IsEmpty())
						{
							Failure = FString::Printf(
								TEXT("class '%s' has no function '%s'"), *ClassSpec, *FunctionName);
						}
						else if (Function == nullptr)
						{
							Failure = FString::Printf(
								TEXT("no function '%s' on this Blueprint or its parent — pass 'class' to target another type"),
								*FunctionName);
						}
						else
						{
							FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
							UK2Node_CallFunction* Node = Creator.CreateNode();
							// Configure BEFORE Finalize so pins are allocated
							// from the real signature.
							Node->SetFromFunction(Function);
							Creator.Finalize();
							Created = Node;
						}
					}
					else if (NodeType.Equals(TEXT("variable_get"), ESearchCase::IgnoreCase)
						|| NodeType.Equals(TEXT("variable_set"), ESearchCase::IgnoreCase))
					{
						FString VarName;
						Body->TryGetStringField(TEXT("variable"), VarName);
						UClass* Scope = Blueprint->GeneratedClass
							? Blueprint->GeneratedClass.Get()
							: Blueprint->ParentClass.Get();
						if (VarName.IsEmpty())
						{
							Failure = TEXT("'variable' is required for variable_get/variable_set");
						}
						else if (NodeType.Equals(TEXT("variable_get"), ESearchCase::IgnoreCase))
						{
							FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
							UK2Node_VariableGet* Node = Creator.CreateNode();
							Node->VariableReference.SetSelfMember(FName(*VarName));
							Creator.Finalize();
							Created = Node;
						}
						else
						{
							FGraphNodeCreator<UK2Node_VariableSet> Creator(*Graph);
							UK2Node_VariableSet* Node = Creator.CreateNode();
							Node->VariableReference.SetSelfMember(FName(*VarName));
							Creator.Finalize();
							Created = Node;
						}
						(void)Scope;
					}
					else if (NodeType.Equals(TEXT("branch"), ESearchCase::IgnoreCase))
					{
						FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
						UK2Node_IfThenElse* Node = Creator.CreateNode();
						Creator.Finalize();
						Created = Node;
					}
					else if (NodeType.Equals(TEXT("custom_event"), ESearchCase::IgnoreCase))
					{
						FString EventName;
						Body->TryGetStringField(TEXT("name"), EventName);
						FGraphNodeCreator<UK2Node_CustomEvent> Creator(*Graph);
						UK2Node_CustomEvent* Node = Creator.CreateNode();
						Node->CustomFunctionName = FName(*(EventName.IsEmpty() ? TEXT("NewEvent") : EventName));
						Creator.Finalize();
						Created = Node;
					}
					else
					{
						Failure = FString::Printf(
							TEXT("unknown node_type '%s' — use call_function, variable_get, variable_set, branch, or custom_event"),
							*NodeType);
					}

					if (Created == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("node_not_created"),
							Failure.IsEmpty() ? TEXT("node creation failed") : Failure);
						return;
					}
					Created->NodePosX = PosX;
					Created->NodePosY = PosY;
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

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, add_variable, remove_variable, ")
						TEXT("add_function, remove_function, add_node, delete_node, connect_pins, ")
						TEXT("disconnect_pins, set_pin_value, compile, or save"),
						*Operation));
			});
	}
}
