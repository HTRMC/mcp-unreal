// Control Rig graphs: RigVM node authoring through the Blueprint's graph
// controller — unit nodes by struct, template nodes by notation, variable,
// comment, branch, if and select nodes; links; pin defaults; the VM compile
// and its log.

#include "ControlRigBlueprintLegacy.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace RigVmGraphs
	{
		UControlRigBlueprint* RigOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("rig"), Path, Responder, TEXT("a Control Rig Blueprint path")))
			{
				return nullptr;
			}
			UControlRigBlueprint* Rig = Cast<UControlRigBlueprint>(ResolveAsset(Path));
			if (Rig == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("rig_not_found"),
					FString::Printf(TEXT("no Control Rig Blueprint at '%s'"), *Path));
			}
			return Rig;
		}

		// The default model, or a function / collapse graph by node path.
		URigVMGraph* GraphOrError(const TSharedRef<FJsonObject>& Body, UControlRigBlueprint* Rig, const TSharedRef<FMcpResponder>& Responder)
		{
			FString GraphPath;
			Body->TryGetStringField(TEXT("graph"), GraphPath);
			URigVMGraph* Graph = GraphPath.IsEmpty() ? Rig->GetDefaultModel() : Rig->GetModel(GraphPath);
			if (Graph == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("graph_not_found"),
					FString::Printf(TEXT("no graph '%s' in the rig — list_graphs shows them"), *GraphPath));
			}
			return Graph;
		}

		const TCHAR* DirectionName(ERigVMPinDirection Direction)
		{
			switch (Direction)
			{
			case ERigVMPinDirection::Input: return TEXT("input");
			case ERigVMPinDirection::Output: return TEXT("output");
			case ERigVMPinDirection::IO: return TEXT("io");
			case ERigVMPinDirection::Visible: return TEXT("visible");
			case ERigVMPinDirection::Hidden: return TEXT("hidden");
			default: return TEXT("invalid");
			}
		}

		TSharedRef<FJsonObject> PinJson(URigVMPin* Pin, bool bSubPins)
		{
			const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("pin"), Pin->GetName());
			Out->SetStringField(TEXT("path"), Pin->GetPinPath());
			Out->SetStringField(TEXT("direction"), DirectionName(Pin->GetDirection()));
			Out->SetStringField(TEXT("type"), Pin->GetCPPType());
			Out->SetBoolField(TEXT("execute"), Pin->IsExecuteContext());
			if (!Pin->IsExecuteContext())
			{
				Out->SetStringField(TEXT("default"), Pin->GetDefaultValue());
			}
			TArray<TSharedPtr<FJsonValue>> Sources, Targets;
			for (URigVMPin* Source : Pin->GetLinkedSourcePins())
			{
				Sources.Add(MakeShared<FJsonValueString>(Source->GetPinPath()));
			}
			for (URigVMPin* Target : Pin->GetLinkedTargetPins())
			{
				Targets.Add(MakeShared<FJsonValueString>(Target->GetPinPath()));
			}
			if (Sources.Num() > 0)
			{
				Out->SetArrayField(TEXT("linked_from"), Sources);
			}
			if (Targets.Num() > 0)
			{
				Out->SetArrayField(TEXT("linked_to"), Targets);
			}
			if (bSubPins && Pin->GetSubPins().Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> Subs;
				for (URigVMPin* Sub : Pin->GetSubPins())
				{
					Subs.Add(MakeShared<FJsonValueObject>(PinJson(Sub, false)));
				}
				Out->SetArrayField(TEXT("sub_pins"), Subs);
			}
			return Out;
		}

		TSharedRef<FJsonObject> NodeJson(URigVMNode* Node, bool bPins)
		{
			const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("node"), Node->GetName());
			Out->SetStringField(TEXT("title"), Node->GetNodeTitle());
			Out->SetStringField(TEXT("class"), Node->GetClass()->GetName());
			if (const URigVMUnitNode* Unit = Cast<URigVMUnitNode>(Node))
			{
				Out->SetStringField(TEXT("struct"), Unit->GetScriptStruct() != nullptr ? Unit->GetScriptStruct()->GetPathName() : FString());
			}
			if (Node->IsEvent())
			{
				Out->SetStringField(TEXT("event"), Node->GetEventName().ToString());
			}
			TArray<TSharedPtr<FJsonValue>> Position;
			Position.Add(MakeShared<FJsonValueNumber>(Node->GetPosition().X));
			Position.Add(MakeShared<FJsonValueNumber>(Node->GetPosition().Y));
			Out->SetArrayField(TEXT("position"), Position);
			if (bPins)
			{
				TArray<TSharedPtr<FJsonValue>> Pins;
				for (URigVMPin* Pin : Node->GetPins())
				{
					Pins.Add(MakeShared<FJsonValueObject>(PinJson(Pin, true)));
				}
				Out->SetArrayField(TEXT("pins"), Pins);
			}
			return Out;
		}

		TSharedRef<FJsonObject> GraphJson(UControlRigBlueprint* Rig, URigVMGraph* Graph, bool bPins)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("rig"), Rig->GetPathName());
			Data->SetStringField(TEXT("graph"), Graph->GetNodePath());
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (URigVMNode* Node : Graph->GetNodes())
			{
				Nodes.Add(MakeShared<FJsonValueObject>(NodeJson(Node, bPins)));
			}
			Data->SetArrayField(TEXT("nodes"), Nodes);
			TArray<TSharedPtr<FJsonValue>> Links;
			for (URigVMLink* Link : Graph->GetLinks())
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("from"), Link->GetSourcePin() != nullptr ? Link->GetSourcePin()->GetPinPath() : FString());
				Entry->SetStringField(TEXT("to"), Link->GetTargetPin() != nullptr ? Link->GetTargetPin()->GetPinPath() : FString());
				Links.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Data->SetArrayField(TEXT("links"), Links);
			return Data;
		}

		// A template by exact notation, or by its name ("Add" finds
		// "Add(in A,in B,out Result)", "Set Transform" its Execute notation).
		FName ResolveTemplateNotation(const FString& Spec)
		{
			const TArray<FString> Notations = URigVMController::GetRegisteredTemplates();
			if (Notations.Contains(Spec))
			{
				return FName(*Spec);
			}
			for (const FString& Notation : Notations)
			{
				FString Name = Notation;
				int32 Paren = INDEX_NONE;
				if (Name.FindChar(TEXT('('), Paren))
				{
					Name.LeftInline(Paren);
				}
				FString Left, Right;
				if (Name.Split(TEXT("::"), &Left, &Right))
				{
					Name = Left;
				}
				if (Name.Equals(Spec, ESearchCase::IgnoreCase))
				{
					return FName(*Notation);
				}
			}
			return NAME_None;
		}

		// A new rig has no event until its editor opens and adds Forwards
		// Solve; do the same before the first node lands in the main graph.
		bool EnsureForwardsSolve(URigVMController* Controller, URigVMGraph* Graph, UControlRigBlueprint* Rig)
		{
			if (Graph != Rig->GetDefaultModel() || Controller->GetAllEventNames().Num() > 0)
			{
				return false;
			}
			UScriptStruct* BeginExecution = FindObject<UScriptStruct>(nullptr, TEXT("/Script/ControlRig.RigUnit_BeginExecution"));
			return BeginExecution != nullptr && Controller->AddUnitNode(BeginExecution, TEXT("Execute"), FVector2D::ZeroVector, FString(), true, false) != nullptr;
		}

		FVector2D PositionOf(const TSharedRef<FJsonObject>& Body)
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (Body->TryGetArrayField(TEXT("position"), Array) && Array->Num() >= 2)
			{
				return FVector2D((*Array)[0]->AsNumber(), (*Array)[1]->AsNumber());
			}
			return FVector2D::ZeroVector;
		}

		TSharedRef<FJsonObject> CompileJson(UControlRigBlueprint* Rig)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("rig"), Rig->GetPathName());
			Data->SetStringField(TEXT("status"), StaticEnum<EBlueprintStatus>()->GetNameStringByValue(static_cast<int64>(Rig->Status.GetValue())));
			const FCompilerResultsLog& Log = Rig->GetCompileLog();
			Data->SetNumberField(TEXT("errors"), Log.NumErrors);
			Data->SetNumberField(TEXT("warnings"), Log.NumWarnings);
			TArray<TSharedPtr<FJsonValue>> Messages;
			for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("severity"), Message->GetSeverity() == EMessageSeverity::Error ? TEXT("error")
					: Message->GetSeverity() == EMessageSeverity::Warning ? TEXT("warning") : TEXT("info"));
				Entry->SetStringField(TEXT("message"), Message->ToText().ToString());
				Messages.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Data->SetArrayField(TEXT("messages"), Messages);
			return Data;
		}
	}

	void RegisterRigVmGraphRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace RigVmGraphs;

		Core.RegisterRoute(TEXT("/api/anim/rigvm_graph"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_node_types"))
				{
					FString Filter;
					Body->TryGetStringField(TEXT("filter"), Filter);
					const int32 Max = IntOr(Body, TEXT("max"), 200);
					TArray<TSharedPtr<FJsonValue>> Units;
					for (UScriptStruct* Struct : URigVMController::GetRegisteredUnitStructs())
					{
						if (Struct == nullptr || Struct->HasMetaData(TEXT("Deprecated")) || Struct->HasMetaData(TEXT("Hidden")))
						{
							continue;
						}
						const FString DisplayName = Struct->GetMetaData(TEXT("DisplayName"));
						const FString Category = Struct->GetMetaData(TEXT("Category"));
						if (!Filter.IsEmpty() && !Struct->GetName().Contains(Filter) && !DisplayName.Contains(Filter) && !Category.Contains(Filter)
							&& !Struct->GetMetaData(TEXT("Keywords")).Contains(Filter))
						{
							continue;
						}
						if (Units.Num() >= Max)
						{
							break;
						}
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("struct"), Struct->GetPathName());
						Entry->SetStringField(TEXT("display_name"), DisplayName);
						Entry->SetStringField(TEXT("category"), Category);
						const FString Template = URigVMController::GetTemplateForUnitStruct(Struct);
						if (!Template.IsEmpty())
						{
							Entry->SetStringField(TEXT("template"), Template);
						}
						Units.Add(MakeShared<FJsonValueObject>(Entry));
					}
					TArray<TSharedPtr<FJsonValue>> Templates;
					for (const FString& Notation : URigVMController::GetRegisteredTemplates())
					{
						if ((!Filter.IsEmpty() && !Notation.Contains(Filter)) || Templates.Num() >= Max)
						{
							continue;
						}
						Templates.Add(MakeShared<FJsonValueString>(Notation));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("units"), Units);
					Data->SetArrayField(TEXT("templates"), Templates);
					Data->SetStringField(TEXT("note"), TEXT("add_node takes 'struct' (a unit struct path) or 'template' (a notation such as Add(in A,in B,out Result)); pass 'filter' to narrow, 'max' to see more"));
					Responder->Ok(Data);
					return;
				}

				UControlRigBlueprint* Rig = RigOrError(Body, Responder);
				if (Rig == nullptr)
				{
					return;
				}

				if (Operation == TEXT("list_graphs"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("rig"), Rig->GetPathName());
					TArray<TSharedPtr<FJsonValue>> Graphs;
					for (URigVMGraph* Graph : Rig->GetAllModels())
					{
						if (Graph == nullptr)
						{
							continue;
						}
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("graph"), Graph->GetNodePath());
						Entry->SetBoolField(TEXT("default"), Graph == Rig->GetDefaultModel());
						Entry->SetNumberField(TEXT("nodes"), Graph->GetNodes().Num());
						Graphs.Add(MakeShared<FJsonValueObject>(Entry));
					}
					Data->SetArrayField(TEXT("graphs"), Graphs);
					TArray<TSharedPtr<FJsonValue>> Variables;
					for (const FRigVMGraphVariableDescription& Variable : Rig->GetAssetVariables())
					{
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("name"), Variable.Name.ToString());
						Entry->SetStringField(TEXT("type"), Variable.CPPType);
						Entry->SetStringField(TEXT("default"), Variable.DefaultValue);
						Variables.Add(MakeShared<FJsonValueObject>(Entry));
					}
					Data->SetArrayField(TEXT("variables"), Variables);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_nodes"))
				{
					URigVMGraph* Graph = GraphOrError(Body, Rig, Responder);
					if (Graph == nullptr)
					{
						return;
					}
					Responder->Ok(GraphJson(Rig, Graph, BoolOr(Body, TEXT("pins"), true)));
					return;
				}

				if (Operation == TEXT("node_info"))
				{
					URigVMGraph* Graph = GraphOrError(Body, Rig, Responder);
					FString NodeName;
					if (Graph == nullptr || !RequireString(Body, TEXT("node"), NodeName, Responder, TEXT("a node name from list_nodes")))
					{
						return;
					}
					URigVMNode* Node = Graph->FindNodeByName(FName(*NodeName));
					if (Node == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"), FString::Printf(TEXT("no node '%s' in the graph"), *NodeName));
						return;
					}
					Responder->Ok(NodeJson(Node, true));
					return;
				}

				if (Operation == TEXT("add_variable"))
				{
					FString Name, CppType, Default;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the variable's name"))
						|| !RequireString(Body, TEXT("cpp_type"), CppType, Responder, TEXT("a RigVM type such as float, bool, int32, FVector, FTransform, TArray<FVector>")))
					{
						return;
					}
					Body->TryGetStringField(TEXT("default"), Default);
					const FName Added = Rig->AddMemberVariable(FName(*Name), CppType, BoolOr(Body, TEXT("public"), false), BoolOr(Body, TEXT("read_only"), false), Default);
					if (Added.IsNone())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("variable_refused"),
							FString::Printf(TEXT("the rig refused variable '%s' of type '%s' (name taken or unknown type)"), *Name, *CppType));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("rig"), Rig->GetPathName());
					Data->SetStringField(TEXT("variable"), Added.ToString());
					Data->SetStringField(TEXT("cpp_type"), CppType);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("compile"))
				{
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Rig);
					Rig->RecompileVM();
					Responder->Ok(CompileJson(Rig));
					return;
				}

				URigVMGraph* Graph = GraphOrError(Body, Rig, Responder);
				if (Graph == nullptr)
				{
					return;
				}
				URigVMController* Controller = Rig->GetOrCreateController(Graph);
				if (Controller == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_controller"), TEXT("the rig gave no controller for that graph"));
					return;
				}

				if (Operation == TEXT("add_node"))
				{
					FString StructPath, Notation, Variable, Comment, Kind, NodeName, CppType, CppTypeObject;
					Body->TryGetStringField(TEXT("struct"), StructPath);
					Body->TryGetStringField(TEXT("template"), Notation);
					Body->TryGetStringField(TEXT("variable"), Variable);
					Body->TryGetStringField(TEXT("comment"), Comment);
					Body->TryGetStringField(TEXT("kind"), Kind);
					Body->TryGetStringField(TEXT("name"), NodeName);
					Body->TryGetStringField(TEXT("cpp_type"), CppType);
					Body->TryGetStringField(TEXT("cpp_type_object"), CppTypeObject);
					const FVector2D Position = PositionOf(Body);
					FString Method = TEXT("Execute");
					Body->TryGetStringField(TEXT("method"), Method);
					Controller->OpenUndoBracket(TEXT("McpLink Add Node"));
					const bool bEventAdded = StructPath != TEXT("RigUnit_BeginExecution") && !StructPath.EndsWith(TEXT(".RigUnit_BeginExecution"))
						&& EnsureForwardsSolve(Controller, Graph, Rig);
					URigVMNode* Node = nullptr;
					if (!StructPath.IsEmpty())
					{
						UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *StructPath);
						if (Struct == nullptr)
						{
							// A bare name such as RigUnit_SetTransform.
							for (UScriptStruct* Candidate : URigVMController::GetRegisteredUnitStructs())
							{
								if (Candidate != nullptr && (Candidate->GetName() == StructPath || Candidate->GetName() == TEXT("RigUnit_") + StructPath
									|| Candidate->GetMetaData(TEXT("DisplayName")) == StructPath))
								{
									Struct = Candidate;
									break;
								}
							}
						}
						if (Struct == nullptr)
						{
							Controller->CancelUndoBracket();
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("struct_not_found"),
								FString::Printf(TEXT("'%s' is not a registered unit struct — list_node_types shows them"), *StructPath));
							return;
						}
						Node = Controller->AddUnitNode(Struct, FName(*Method), Position, NodeName, true, false);
					}
					else if (!Notation.IsEmpty())
					{
						const FName Resolved = ResolveTemplateNotation(Notation);
						if (Resolved.IsNone())
						{
							Controller->CancelUndoBracket();
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("template_not_found"),
								FString::Printf(TEXT("'%s' is not a registered template notation or name — list_node_types with a filter shows them"), *Notation));
							return;
						}
						Node = Controller->AddTemplateNode(Resolved, Position, NodeName, true, false);
					}
					else if (!Variable.IsEmpty())
					{
						FString Default;
						Body->TryGetStringField(TEXT("default"), Default);
						if (CppType.IsEmpty())
						{
							for (const FRigVMGraphVariableDescription& Description : Rig->GetAssetVariables())
							{
								if (Description.Name.ToString() == Variable)
								{
									CppType = Description.CPPType;
									CppTypeObject = Description.CPPTypeObject != nullptr ? Description.CPPTypeObject->GetPathName() : FString();
									break;
								}
							}
						}
						if (CppType.IsEmpty())
						{
							Controller->CancelUndoBracket();
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("variable_not_found"),
								FString::Printf(TEXT("'%s' is not a rig variable — add_variable makes one, or pass cpp_type"), *Variable));
							return;
						}
						Node = Controller->AddVariableNodeFromObjectPath(FName(*Variable), CppType, CppTypeObject, BoolOr(Body, TEXT("getter"), true), Default, Position, NodeName, true, false);
					}
					else if (!Comment.IsEmpty())
					{
						Node = Controller->AddCommentNode(Comment, Position, FVector2D(400.f, 300.f), FLinearColor::Black, NodeName, true, false);
					}
					else if (Kind == TEXT("branch"))
					{
						Node = Controller->AddBranchNode(Position, NodeName, true, false);
					}
					else if (Kind == TEXT("if") || Kind == TEXT("select"))
					{
						if (CppType.IsEmpty())
						{
							Controller->CancelUndoBracket();
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("an if/select node needs 'cpp_type' (and 'cpp_type_object' for struct or enum types)"));
							return;
						}
						Node = Kind == TEXT("if")
							? Controller->AddIfNode(CppType, FName(*CppTypeObject), Position, NodeName, true, false)
							: Controller->AddSelectNode(CppType, FName(*CppTypeObject), Position, NodeName, true, false);
					}
					else
					{
						Controller->CancelUndoBracket();
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("add_node needs one of 'struct', 'template', 'variable', 'comment' or kind = branch | if | select"));
						return;
					}
					if (Node == nullptr)
					{
						Controller->CancelUndoBracket();
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("node_refused"),
							TEXT("the controller refused the node — the schema may not allow it in this graph, or a node of that name exists (see get_logs, LogRigVMDeveloper)"));
						return;
					}
					// Pin defaults by pin name, as the details panel would set them.
					const TSharedPtr<FJsonObject>* Defaults = nullptr;
					if (Body->TryGetObjectField(TEXT("defaults"), Defaults) && Defaults->IsValid())
					{
						for (const auto& Pair : (*Defaults)->Values)
						{
							const FString PinPath = Node->GetName() + TEXT(".") + FString(Pair.Key.ToView());
							const FString Value = Pair.Value->Type == EJson::String ? Pair.Value->AsString() : Pair.Value->AsString();
							if (!Controller->SetPinDefaultValue(PinPath, Value, true, true, false, false))
							{
								Controller->CancelUndoBracket();
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_default"),
									FString::Printf(TEXT("pin '%s' did not accept '%s' — node_info shows the pins and their types"), *PinPath, *Value));
								return;
							}
						}
					}
					Controller->CloseUndoBracket();
					Rig->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = NodeJson(Node, true);
					Data->SetStringField(TEXT("graph"), Graph->GetNodePath());
					if (bEventAdded)
					{
						Data->SetBoolField(TEXT("forwards_solve_added"), true);
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_node") || Operation == TEXT("rename_node") || Operation == TEXT("set_node_position"))
				{
					FString NodeName;
					if (!RequireString(Body, TEXT("node"), NodeName, Responder, TEXT("a node name from list_nodes")))
					{
						return;
					}
					URigVMNode* Node = Graph->FindNodeByName(FName(*NodeName));
					if (Node == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"), FString::Printf(TEXT("no node '%s' in the graph"), *NodeName));
						return;
					}
					bool bOk = false;
					if (Operation == TEXT("remove_node"))
					{
						bOk = Controller->RemoveNode(Node, true, false);
					}
					else if (Operation == TEXT("rename_node"))
					{
						FString NewName;
						if (!RequireString(Body, TEXT("new_name"), NewName, Responder, TEXT("the node's new name")))
						{
							return;
						}
						bOk = Controller->RenameNode(Node, FName(*NewName), true, false);
					}
					else
					{
						bOk = Controller->SetNodePosition(Node, PositionOf(Body), true, false, false);
					}
					if (!bOk)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("refused"),
							FString::Printf(TEXT("the controller refused %s on '%s'"), *Operation, *NodeName));
						return;
					}
					Rig->MarkPackageDirty();
					Responder->Ok(GraphJson(Rig, Graph, false));
					return;
				}

				if (Operation == TEXT("set_pin_default"))
				{
					FString PinPath, Value;
					if (!RequireString(Body, TEXT("pin"), PinPath, Responder, TEXT("a pin path such as SetTransform.Value.Translation.X")))
					{
						return;
					}
					const TSharedPtr<FJsonValue> Raw = Body->TryGetField(TEXT("value"));
					if (!Raw.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"), TEXT("'value' is required (RigVM text, e.g. 1.0, true, (X=1,Y=0,Z=0))"));
						return;
					}
					Value = Raw->AsString();
					URigVMPin* Pin = Graph->FindPin(PinPath);
					if (Pin == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("pin_not_found"), FString::Printf(TEXT("no pin '%s' — node_info shows a node's pin paths"), *PinPath));
						return;
					}
					if (!Controller->SetPinDefaultValue(PinPath, Value, true, true, false, false))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_default"),
							FString::Printf(TEXT("pin '%s' (%s) did not accept '%s'"), *PinPath, *Pin->GetCPPType(), *Value));
						return;
					}
					Rig->MarkPackageDirty();
					Responder->Ok(PinJson(Pin, true));
					return;
				}

				if (Operation == TEXT("link_pins") || Operation == TEXT("unlink_pins"))
				{
					FString From, To;
					if (!RequireString(Body, TEXT("from"), From, Responder, TEXT("the output pin path"))
						|| !RequireString(Body, TEXT("to"), To, Responder, TEXT("the input pin path")))
					{
						return;
					}
					const bool bOk = Operation == TEXT("link_pins") ? Controller->AddLink(From, To, true, false) : Controller->BreakLink(From, To, true, false);
					if (!bOk)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("link_refused"),
							FString::Printf(TEXT("could not %s %s -> %s — types must match (or a cast be possible), and execute pins link execute pins; see get_logs (LogRigVMDeveloper)"),
								Operation == TEXT("link_pins") ? TEXT("link") : TEXT("unlink"), *From, *To));
						return;
					}
					Rig->MarkPackageDirty();
					Responder->Ok(GraphJson(Rig, Graph, false));
					return;
				}

				if (Operation == TEXT("break_all_links"))
				{
					FString PinPath;
					if (!RequireString(Body, TEXT("pin"), PinPath, Responder, TEXT("a pin path")))
					{
						return;
					}
					Controller->BreakAllLinks(PinPath, BoolOr(Body, TEXT("as_input"), true), true, false);
					Rig->MarkPackageDirty();
					Responder->Ok(GraphJson(Rig, Graph, false));
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected list_node_types, list_graphs, list_nodes, node_info, add_node, remove_node, rename_node, ")
						TEXT("set_node_position, set_pin_default, link_pins, unlink_pins, break_all_links, add_variable or compile"), *Operation));
			});
	}
}
