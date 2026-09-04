// Blueprint debugging: breakpoints, watched pins, and the debug object their
// values are read from.
//
// Halting works too. When a breakpoint hits, FKismetDebugUtilities calls
// FSlateApplication::EnterDebuggingMode, a nested loop that never returns to
// FEngineLoop — so FTSTicker stops running, and with it the HTTP server, which
// used to mean the request that would resume execution could not be delivered.
// The loop does keep ticking Slate, though, so McpLinkCore pumps the HTTP
// server from FSlateApplication::OnPreTick while halted; see PumpWhileHalted.
// Only this route, status and output_log are served in that state, because any
// other handler would be re-entering the engine from inside a paused
// Blueprint's call stack.
//
// resume, step_into, step_over, step_out and abort name no Blueprint: like the
// debugger toolbar they act on whatever is currently stopped.
//
// Watched pin values are still readable while PIE runs *without* halting, for
// any pin backed by a class property (variable gets, and node outputs the
// compiler promoted to properties). Locals of a function that is not currently
// on the stack read back as not-in-scope, which is what EWTR_NotInScope means
// and what this route reports.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/Blueprint.h"
#include "Kismet2/Breakpoint.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/WatchedPin.h"
#include "McpAssetUtils.h"
#include "McpBlueprintUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/App.h"
#include "UnrealEdGlobals.h"

namespace McpLink
{
	namespace BlueprintDebug
	{
		UBlueprint* DebugBlueprintOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("blueprint"), Path, Responder,
					TEXT("a Blueprint asset path, e.g. /Game/BP_Player")))
			{
				return nullptr;
			}
			UBlueprint* Blueprint = ResolveBlueprint(Path);
			if (Blueprint == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("blueprint_not_found"),
					FString::Printf(TEXT("no Blueprint at '%s'"), *Path));
			}
			return Blueprint;
		}

		/// The node an operation acts on, from "graph" plus "node" (a GUID as
		/// reported by blueprint_query get_graph).
		UEdGraphNode* DebugNodeOrError(UBlueprint* Blueprint,
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString GraphName;
			Body->TryGetStringField(TEXT("graph"), GraphName);
			UEdGraph* Graph = FindGraph(Blueprint, GraphName);
			if (Graph == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("graph_not_found"),
					FString::Printf(TEXT("no graph '%s' in %s — blueprint_query inspect lists them"),
						*GraphName, *Blueprint->GetName()));
				return nullptr;
			}
			FString NodeGuid;
			if (!RequireString(Body, TEXT("node"), NodeGuid, Responder,
					TEXT("a node GUID from blueprint_query get_graph")))
			{
				return nullptr;
			}
			UEdGraphNode* Node = FindNodeByGuid(Graph, NodeGuid);
			if (Node == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"),
					FString::Printf(TEXT("no node '%s' in graph '%s'"), *NodeGuid, *GraphPath(Graph)));
			}
			return Node;
		}

		TSharedRef<FJsonObject> NodeSiteToJson(const UEdGraphNode* Node)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("node"), Node->NodeGuid.ToString());
			Object->SetStringField(TEXT("title"),
				Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			Object->SetStringField(TEXT("graph"), GraphPath(Node->GetGraph()));
			return Object;
		}

		const TCHAR* WatchResultName(FKismetDebugUtilities::EWatchTextResult Result)
		{
			switch (Result)
			{
			case FKismetDebugUtilities::EWTR_Valid: return TEXT("valid");
			case FKismetDebugUtilities::EWTR_NotInScope: return TEXT("not_in_scope");
			case FKismetDebugUtilities::EWTR_NoDebugObject: return TEXT("no_debug_object");
			default: return TEXT("no_property");
			}
		}

		/// Why a value is missing, phrased as what to do about it.
		const TCHAR* WatchResultRemedy(FKismetDebugUtilities::EWatchTextResult Result)
		{
			switch (Result)
			{
			case FKismetDebugUtilities::EWTR_NotInScope:
				return TEXT("this pin is a local of a function that is not on the stack right now — ")
					   TEXT("only pins backed by a class property read outside a halt");
			case FKismetDebugUtilities::EWTR_NoDebugObject:
				return TEXT("no instance is selected — run PIE and call set_debug_object, or pass ")
					   TEXT("'object'");
			case FKismetDebugUtilities::EWTR_NoProperty:
				return TEXT("the compiler kept no property for this pin, so it has no value to read");
			default:
				return TEXT("");
			}
		}

		/// The instance watches read from: an explicit "object", else whatever
		/// the Blueprint already has selected.
		UObject* DebugObject(UBlueprint* Blueprint, const TSharedRef<FJsonObject>& Body)
		{
			FString Path;
			if (Body->TryGetStringField(TEXT("object"), Path) && !Path.IsEmpty())
			{
				return FindObjectSafe<UObject>(nullptr, *Path);
			}
			return Blueprint->GetObjectBeingDebugged();
		}

		void AddDebugObjectFields(const TSharedRef<FJsonObject>& Data, UBlueprint* Blueprint)
		{
			UObject* Object = Blueprint->GetObjectBeingDebugged();
			Data->SetStringField(TEXT("debug_object"),
				Object != nullptr ? Object->GetPathName() : FString());
			Data->SetStringField(TEXT("debug_object_path_hint"), Blueprint->GetObjectPathToDebug());
		}

		bool IsHaltControl(const FString& Operation)
		{
			return Operation == TEXT("resume")
				|| Operation == TEXT("step_into")
				|| Operation == TEXT("step_over")
				|| Operation == TEXT("step_out")
				|| Operation == TEXT("abort");
		}

		/// The Blueprint debugger toolbar's own sequence, which is all these
		/// are: ask FKismetDebugUtilities for the step kind, unpause the PIE
		/// worlds, then tell Slate to leave its debugging loop. Passing "we are
		/// stopping again shortly" to LeaveDebuggingMode is what keeps the
		/// mouse out of the game viewport between steps.
		void HandleHaltControl(const FString& Operation, const TSharedRef<FMcpResponder>& Responder)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			const bool bHalted = FMcpLinkCoreModule::IsHaltedAtBreakpoint();
			Data->SetBoolField(TEXT("halted"), bHalted);
			Data->SetBoolField(TEXT("single_stepping"), FKismetDebugUtilities::IsSingleStepping());
			if (const UEdGraphNode* Node = FKismetDebugUtilities::GetCurrentInstruction())
			{
				Data->SetObjectField(TEXT("halted_at"), NodeSiteToJson(Node));
			}
			if (const UEdGraphNode* Node = FKismetDebugUtilities::GetMostRecentBreakpointHit())
			{
				Data->SetObjectField(TEXT("breakpoint_hit"), NodeSiteToJson(Node));
			}
			if (const UWorld* World = FKismetDebugUtilities::GetCurrentDebuggingWorld())
			{
				Data->SetStringField(TEXT("debugging_world"), World->GetPathName());
			}

			if (Operation == TEXT("halt_status"))
			{
				Data->SetStringField(TEXT("message"), bHalted
					? TEXT("halted — resume, step_into, step_over, step_out or abort to continue")
					: TEXT("not halted"));
				Responder->Ok(Data);
				return;
			}

			if (!bHalted)
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_halted"),
					TEXT("execution is not halted on a breakpoint, so there is nothing to resume "
						 "or step — halt_status reports whether it is"));
				return;
			}

			if (Operation == TEXT("step_into"))
			{
				FKismetDebugUtilities::RequestSingleStepIn();
			}
			else if (Operation == TEXT("step_over"))
			{
				FKismetDebugUtilities::RequestStepOver();
			}
			else if (Operation == TEXT("step_out"))
			{
				FKismetDebugUtilities::RequestStepOut();
			}
			else if (Operation == TEXT("abort"))
			{
				FKismetDebugUtilities::RequestAbortingExecution();
			}

			if (GUnrealEd != nullptr)
			{
				GUnrealEd->SetPIEWorldsPaused(false);
			}
			const bool bResuming = !FKismetDebugUtilities::IsSingleStepping();
			// Only sets a flag: the nested loop finishes its iteration and
			// unwinds, so this response is written once the editor is running
			// again. That is also why the reply says what *will* happen.
			FSlateApplication::Get().LeaveDebuggingMode(!bResuming);
			if (GUnrealEd != nullptr)
			{
				GUnrealEd->PlaySessionSingleStepped();
			}

			Data->SetBoolField(TEXT("resuming"), bResuming);
			Data->SetStringField(TEXT("message"), bResuming
				? TEXT("resuming — PIE runs on until the next breakpoint")
				: TEXT("stepping — execution halts again at the next node; poll halt_status"));
			Responder->Ok(Data);
		}
	}

	using namespace BlueprintDebug;

	void RegisterBlueprintDebugRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/blueprints/debug"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				// Halt control names no Blueprint: it acts on whatever is
				// currently stopped, exactly like the debugger's toolbar.
				if (Operation == TEXT("halt_status") || IsHaltControl(Operation))
				{
					HandleHaltControl(Operation, Responder);
					return;
				}

				UBlueprint* Blueprint = DebugBlueprintOrError(Body, Responder);
				if (Blueprint == nullptr)
				{
					return;
				}

				if (Operation == TEXT("status"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
					Data->SetBoolField(TEXT("has_breakpoints"),
						FKismetDebugUtilities::BlueprintHasBreakpoints(Blueprint));
					Data->SetBoolField(TEXT("has_watches"),
						FKismetDebugUtilities::BlueprintHasPinWatches(Blueprint));
					Data->SetBoolField(TEXT("has_debugging_data"),
						FKismetDebugUtilities::HasDebuggingData(Blueprint));
					Data->SetBoolField(TEXT("single_stepping"),
						FKismetDebugUtilities::IsSingleStepping());
					if (const UEdGraphNode* Node = FKismetDebugUtilities::GetCurrentInstruction())
					{
						Data->SetObjectField(TEXT("halted_at"), NodeSiteToJson(Node));
					}
					Data->SetBoolField(TEXT("in_pie"), GEditor != nullptr && GEditor->PlayWorld != nullptr);
					// Halting is answerable in any editor, windowed or not: the
					// HTTP server is pumped from Slate's tick while stopped.
					Data->SetBoolField(TEXT("halted"), FMcpLinkCoreModule::IsHaltedAtBreakpoint());
					AddDebugObjectFields(Data, Blueprint);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_breakpoints"))
				{
					TArray<TSharedPtr<FJsonValue>> Items;
					FKismetDebugUtilities::ForeachBreakpoint(Blueprint,
						[&Items](FBlueprintBreakpoint& Breakpoint)
						{
							if (UEdGraphNode* Node = Breakpoint.GetLocation())
							{
								const TSharedRef<FJsonObject> Item = NodeSiteToJson(Node);
								Item->SetBoolField(TEXT("enabled"), Breakpoint.IsEnabledByUser());
								Item->SetBoolField(TEXT("active"), Breakpoint.IsEnabled());
								Items.Add(MakeShared<FJsonValueObject>(Item));
							}
						});
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
					Data->SetNumberField(TEXT("count"), Items.Num());
					Data->SetArrayField(TEXT("breakpoints"), Items);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_watches"))
				{
					TArray<TSharedPtr<FJsonValue>> Items;
					UObject* Object = DebugObject(Blueprint, Body);
					FKismetDebugUtilities::ForeachPinWatch(Blueprint,
						[&Items, Blueprint, Object](UEdGraphPin* Pin)
						{
							if (Pin == nullptr || Pin->GetOwningNodeUnchecked() == nullptr)
							{
								return;
							}
							const TSharedRef<FJsonObject> Item = NodeSiteToJson(Pin->GetOwningNode());
							Item->SetStringField(TEXT("pin"), Pin->PinName.ToString());
							FString Text;
							const FKismetDebugUtilities::EWatchTextResult Result =
								FKismetDebugUtilities::GetWatchText(Text, Blueprint, Object, Pin);
							Item->SetStringField(TEXT("result"), WatchResultName(Result));
							if (Result == FKismetDebugUtilities::EWTR_Valid)
							{
								Item->SetStringField(TEXT("value"), Text);
							}
							else
							{
								Item->SetStringField(TEXT("reason"), WatchResultRemedy(Result));
							}
							Items.Add(MakeShared<FJsonValueObject>(Item));
						});
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
					Data->SetNumberField(TEXT("count"), Items.Num());
					Data->SetArrayField(TEXT("watches"), Items);
					AddDebugObjectFields(Data, Blueprint);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("clear_breakpoints"))
				{
					FKismetDebugUtilities::ClearBreakpoints(Blueprint);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
					Data->SetStringField(TEXT("message"), TEXT("all breakpoints removed"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("clear_watches"))
				{
					FKismetDebugUtilities::ClearPinWatches(Blueprint);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
					Data->SetStringField(TEXT("message"), TEXT("all watches removed"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_debug_object"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("object"), Path, Responder,
							TEXT("the full path of a spawned instance, e.g. the PIE actor from ")
							TEXT("find_actors — pass \"\" to clear it")))
					{
						return;
					}
					UObject* Object = FindObjectSafe<UObject>(nullptr, *Path);
					if (Object == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("object_not_found"),
							FString::Printf(
								TEXT("no object at '%s' — during PIE the instance lives under a ")
								TEXT("/Game/.../UEDPIE_ world, so use the path find_actors reports"),
								*Path));
						return;
					}
					if (Blueprint->GeneratedClass != nullptr
						&& !Object->IsA(Blueprint->GeneratedClass))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("wrong_class"),
							FString::Printf(TEXT("'%s' is a %s, not a %s"), *Path,
								*Object->GetClass()->GetName(),
								*Blueprint->GeneratedClass->GetName()));
						return;
					}
					Blueprint->SetObjectBeingDebugged(Object);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
					AddDebugObjectFields(Data, Blueprint);
					Data->SetStringField(TEXT("message"),
						TEXT("watch values now read from this instance"));
					Responder->Ok(Data);
					return;
				}

				// ---- everything below addresses one node -------------------
				UEdGraphNode* Node = DebugNodeOrError(Blueprint, Body, Responder);
				if (Node == nullptr)
				{
					return;
				}

				if (Operation == TEXT("set_breakpoint")
					|| Operation == TEXT("set_breakpoint_enabled"))
				{
					const bool bEnabled = BoolOr(Body, TEXT("enabled"), true);

					FBlueprintBreakpoint* Existing =
						FKismetDebugUtilities::FindBreakpointForNode(Node, Blueprint);
					if (Existing == nullptr)
					{
						if (Operation == TEXT("set_breakpoint_enabled"))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound,
								TEXT("breakpoint_not_found"),
								TEXT("no breakpoint on that node — set_breakpoint makes one"));
							return;
						}
						FKismetDebugUtilities::CreateBreakpoint(Blueprint, Node, bEnabled);
					}
					else
					{
						FKismetDebugUtilities::SetBreakpointEnabled(Node, Blueprint, bEnabled);
					}

					const TSharedRef<FJsonObject> Data = NodeSiteToJson(Node);
					Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
					Data->SetBoolField(TEXT("enabled"), bEnabled);
					Data->SetStringField(TEXT("message"), bEnabled
							? TEXT("armed — execution halts here; drive it from halt_status, ")
							  TEXT("step_into, step_over, step_out, abort and resume")
							: TEXT("set but disabled — it will not halt execution"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_breakpoint"))
				{
					if (FKismetDebugUtilities::FindBreakpointForNode(Node, Blueprint) == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound,
							TEXT("breakpoint_not_found"), TEXT("no breakpoint on that node"));
						return;
					}
					FKismetDebugUtilities::RemoveBreakpointFromNode(Node, Blueprint);
					const TSharedRef<FJsonObject> Data = NodeSiteToJson(Node);
					Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
					Data->SetStringField(TEXT("message"), TEXT("breakpoint removed"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_watch") || Operation == TEXT("remove_watch")
					|| Operation == TEXT("read_watch"))
				{
					FString PinName;
					if (!RequireString(Body, TEXT("pin"), PinName, Responder,
							TEXT("a pin name on that node, from blueprint_query get_graph")))
					{
						return;
					}
					UEdGraphPin* Pin = FindPin(Node, PinName);
					if (Pin == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("pin_not_found"),
							FString::Printf(TEXT("no pin '%s' on that node"), *PinName));
						return;
					}

					if (Operation == TEXT("add_watch"))
					{
						if (!FKismetDebugUtilities::CanWatchPin(Blueprint, Pin))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest,
								TEXT("cannot_watch"),
								TEXT("this pin cannot be watched — exec pins and pins the compiler ")
								TEXT("keeps no property for have no value to show"));
							return;
						}
						FKismetDebugUtilities::AddPinWatch(Blueprint, FBlueprintWatchedPin(Pin));
					}
					else if (Operation == TEXT("remove_watch"))
					{
						if (!FKismetDebugUtilities::RemovePinWatch(Blueprint, Pin))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound,
								TEXT("watch_not_found"), TEXT("that pin is not being watched"));
							return;
						}
					}

					const TSharedRef<FJsonObject> Data = NodeSiteToJson(Node);
					Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
					Data->SetStringField(TEXT("pin"), Pin->PinName.ToString());
					Data->SetBoolField(TEXT("watched"),
						FKismetDebugUtilities::IsPinBeingWatched(Blueprint, Pin));
					if (Operation != TEXT("remove_watch"))
					{
						FString Text;
						const FKismetDebugUtilities::EWatchTextResult Result =
							FKismetDebugUtilities::GetWatchText(
								Text, Blueprint, DebugObject(Blueprint, Body), Pin);
						Data->SetStringField(TEXT("result"), WatchResultName(Result));
						if (Result == FKismetDebugUtilities::EWTR_Valid)
						{
							Data->SetStringField(TEXT("value"), Text);
						}
						else
						{
							Data->SetStringField(TEXT("reason"), WatchResultRemedy(Result));
						}
						AddDebugObjectFields(Data, Blueprint);
					}
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use status, halt_status, resume, step_into, ")
						TEXT("step_over, step_out, abort, list_breakpoints, set_breakpoint, ")
						TEXT("set_breakpoint_enabled, remove_breakpoint, clear_breakpoints, ")
						TEXT("list_watches, add_watch, read_watch, remove_watch, clear_watches or ")
						TEXT("set_debug_object"),
						*Operation));
			});
	}
}
