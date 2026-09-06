// Animation Blueprint routes: create for a skeleton, author state machines
// (states, entry, transitions, rules), inspect the result.
//
// Anything not covered here (arbitrary anim nodes, complex transition rules)
// is reachable through the generic Blueprint routes, because FindGraph
// resolves state / transition graphs by path ("Locomotion/Idle").

#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimGraphNode_LinkedAnimGraph.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateMachine.h"
#include "Animation/AnimLayerInterface.h"
#include "AnimationGraphSchema.h"
#include "Engine/MemberReference.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "McpAssetUtils.h"
#include "UObject/UnrealType.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "McpAnimBlueprintUtils.h"
#include "McpBlueprintUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

// Linked anim layers: interface assets declaring layer graphs, Blueprints
// implementing them, and the nodes that run a layer or another anim class.
namespace McpLink::AnimLayers
{
	// A layer graph declared on this Blueprint (interface or self layer), or
	// one it received by implementing an interface.
	UEdGraph* FindLayerGraph(UAnimBlueprint* Blueprint, FName Name, TSubclassOf<UInterface>& OutInterface, FGuid& OutGuid)
	{
		OutInterface = nullptr;
		OutGuid = FGuid();
		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			for (UEdGraph* Graph : Description.Graphs)
			{
				if (Graph != nullptr && Graph->GetFName() == Name)
				{
					OutInterface = Description.Interface;
					OutGuid = Graph->InterfaceGuid;
					return Graph;
				}
			}
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph != nullptr && Graph->GetFName() == Name && Graph->IsA<UAnimationGraph>())
			{
				return Graph;
			}
		}
		return nullptr;
	}

	// The editor's New Animation Layer: an animation graph among the
	// function graphs, with the group as its category.
	UEdGraph* AddLayerGraph(UAnimBlueprint* Blueprint, const FString& Name, const FString& Group, FString& OutError)
	{
		if (!FName::IsValidXName(Name, INVALID_OBJECTNAME_CHARACTERS))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid layer name"), *Name);
			return nullptr;
		}
		TSubclassOf<UInterface> Interface;
		FGuid Guid;
		if (FindLayerGraph(Blueprint, FName(*Name), Interface, Guid) != nullptr || FindObject<UEdGraph>(Blueprint, *Name) != nullptr)
		{
			OutError = FString::Printf(TEXT("the Blueprint already has a graph named '%s'"), *Name);
			return nullptr;
		}
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FName(*Name), UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());
		FBlueprintEditorUtils::AddDomainSpecificGraph(Blueprint, Graph);
		if (!Group.IsEmpty())
		{
			// The group lives on the graph's root node and becomes the
			// function's category when the interface compiles.
			FBlueprintEditorUtils::SetAnimationGraphLayerGroup(Graph, FText::FromString(Group));
		}
		return Graph;
	}

	FString LayerGroupOf(UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UAnimGraphNode_Root* Root = Cast<UAnimGraphNode_Root>(Node))
			{
				return Root->Node.GetGroup().ToString();
			}
		}
		return FString();
	}

	TSharedRef<FJsonObject> LayersJson(UAnimBlueprint* Blueprint)
	{
		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Data->SetBoolField(TEXT("is_interface"), Blueprint->BlueprintType == BPTYPE_Interface);
		TArray<TSharedPtr<FJsonValue>> Own;
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph != nullptr && Graph->IsA<UAnimationGraph>())
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("layer"), Graph->GetName());
				Entry->SetStringField(TEXT("group"), LayerGroupOf(Graph));
				Entry->SetNumberField(TEXT("nodes"), Graph->Nodes.Num());
				Own.Add(MakeShared<FJsonValueObject>(Entry));
			}
		}
		Data->SetArrayField(TEXT("layers"), Own);
		TArray<TSharedPtr<FJsonValue>> Interfaces;
		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("interface"), Description.Interface != nullptr ? Description.Interface->GetPathName() : FString());
			TArray<TSharedPtr<FJsonValue>> Graphs;
			for (UEdGraph* Graph : Description.Graphs)
			{
				if (Graph != nullptr)
				{
					const TSharedRef<FJsonObject> GraphEntry = MakeShared<FJsonObject>();
					GraphEntry->SetStringField(TEXT("layer"), Graph->GetName());
					GraphEntry->SetNumberField(TEXT("nodes"), Graph->Nodes.Num());
					Graphs.Add(MakeShared<FJsonValueObject>(GraphEntry));
				}
			}
			Entry->SetArrayField(TEXT("layers"), Graphs);
			Interfaces.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Data->SetArrayField(TEXT("implemented_interfaces"), Interfaces);
		return Data;
	}

	bool ConnectPoseToGraphResult(UEdGraph* Graph, UEdGraphPin* PosePin, FString& OutError)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node != nullptr && Node->IsA<UAnimGraphNode_Root>())
			{
				UEdGraphPin* Result = Node->FindPin(TEXT("Result"), EGPD_Input);
				if (Result != nullptr && PosePin != nullptr && Graph->GetSchema()->TryCreateConnection(PosePin, Result))
				{
					return true;
				}
				OutError = TEXT("the graph's output pose did not accept the node's pose");
				return false;
			}
		}
		OutError = TEXT("the graph has no output pose node");
		return false;
	}

	UClass* ResolveAnimClass(const FString& Spec)
	{
		if (UBlueprint* AsBlueprint = Cast<UBlueprint>(ResolveAsset(Spec)))
		{
			return AsBlueprint->GeneratedClass;
		}
		return ResolveClass(Spec);
	}
}

namespace McpLink
{
	namespace
	{
		/// Load an asset by "/Game/Path/Name" or "/Game/Path/Name.Name".
		UObject* LoadAssetFlexible(const FString& Path)
		{
			if (Path.IsEmpty())
			{
				return nullptr;
			}
			if (UObject* Direct = ResolveObject(Path))
			{
				return Direct;
			}
			if (!Path.Contains(TEXT(".")))
			{
				return ResolveObject(
					FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path)));
			}
			return nullptr;
		}

		UAnimBlueprint* AnimBlueprintOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!Body->TryGetStringField(TEXT("blueprint"), Path) || Path.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'blueprint' asset path is required, e.g. /Game/Anim/ABP_Hero"));
				return nullptr;
			}
			UBlueprint* Blueprint = ResolveBlueprint(Path);
			if (Blueprint == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("blueprint_not_found"),
					FString::Printf(TEXT("no Blueprint asset at '%s'"), *Path));
				return nullptr;
			}
			UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(Blueprint);
			if (AnimBlueprint == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("not_anim_blueprint"),
					FString::Printf(TEXT("'%s' is a %s, not an Animation Blueprint"),
						*Path, *Blueprint->GetClass()->GetName()));
			}
			return AnimBlueprint;
		}

		UAnimationStateMachineGraph* MachineOrError(
			UBlueprint* Blueprint,
			const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString Name;
			Body->TryGetStringField(TEXT("state_machine"), Name);
			FString Error;
			UAnimationStateMachineGraph* Machine = Anim::FindStateMachine(Blueprint, Name, Error);
			if (Machine == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("state_machine_not_found"), Error);
			}
			return Machine;
		}

		UAnimStateNode* StateOrError(
			UAnimationStateMachineGraph* Machine,
			const TSharedRef<FJsonObject>& Body,
			const TCHAR* Field,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString Name;
			Body->TryGetStringField(Field, Name);
			if (Name.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					FString::Printf(TEXT("'%s' (state name or guid) is required"), Field));
				return nullptr;
			}
			UAnimStateNode* State = Anim::FindState(Machine, Name);
			if (State == nullptr)
			{
				TArray<FString> Names;
				for (UEdGraphNode* Node : Machine->Nodes)
				{
					if (const UAnimStateNode* Candidate = Cast<UAnimStateNode>(Node))
					{
						Names.Add(Candidate->GetStateName());
					}
				}
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("state_not_found"),
					FString::Printf(TEXT("no state '%s' in '%s' — states: %s"), *Name, *Machine->GetName(),
						Names.IsEmpty() ? TEXT("(none)") : *FString::Join(Names, TEXT(", "))));
			}
			return State;
		}

		UAnimationAsset* AnimationOrError(
			const FString& Spec, const TSharedRef<FMcpResponder>& Responder)
		{
			UAnimationAsset* Asset = Cast<UAnimationAsset>(LoadAssetFlexible(Spec));
			if (Asset == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("animation_not_found"),
					FString::Printf(
						TEXT("no animation asset at '%s' — use search_assets with class AnimSequence"), *Spec));
			}
			return Asset;
		}

		TOptional<bool> OptionalBool(const TSharedRef<FJsonObject>& Body, const TCHAR* Field)
		{
			bool Value = false;
			return Body->TryGetBoolField(Field, Value) ? TOptional<bool>(Value) : TOptional<bool>();
		}

		TOptional<double> OptionalNumber(const TSharedRef<FJsonObject>& Body, const TCHAR* Field)
		{
			double Value = 0.0;
			return Body->TryGetNumberField(Field, Value) ? TOptional<double>(Value) : TOptional<double>();
		}

		/// Apply the optional transition settings shared by add_transition and
		/// set_transition. Returns false (and responds) when a rule binding fails.
		bool ApplyTransitionSettings(
			UBlueprint* Blueprint,
			UAnimStateTransitionNode* Transition,
			const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder)
		{
			Transition->Modify();
			if (const TOptional<double> Duration = OptionalNumber(Body, TEXT("crossfade_duration")))
			{
				Transition->CrossfadeDuration = FMath::Max(0.0f, static_cast<float>(*Duration));
			}
			if (const TOptional<double> Priority = OptionalNumber(Body, TEXT("priority")))
			{
				Transition->PriorityOrder = static_cast<int32>(*Priority);
			}
			if (const TOptional<bool> Automatic = OptionalBool(Body, TEXT("automatic_rule")))
			{
				Transition->bAutomaticRuleBasedOnSequencePlayerInState = *Automatic;
			}
			if (const TOptional<bool> Bidirectional = OptionalBool(Body, TEXT("bidirectional")))
			{
				Transition->Bidirectional = *Bidirectional;
			}
			if (const TOptional<bool> Disabled = OptionalBool(Body, TEXT("disabled")))
			{
				Transition->bDisabled = *Disabled;
			}
			FString RuleVariable;
			if (Body->TryGetStringField(TEXT("rule_variable"), RuleVariable) && !RuleVariable.IsEmpty())
			{
				FString Error;
				if (!Anim::BindTransitionRuleToVariable(Blueprint, Transition, RuleVariable, Error))
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("rule_not_bound"), Error);
					return false;
				}
			}
			return true;
		}
	}

	void RegisterAnimBlueprintRoutes(FMcpLinkCoreModule& Core)
	{
		// ---------------------------------------------------------------- query
		Core.RegisterRoute(TEXT("/api/anim_blueprints/query"),
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
					Filter.ClassPaths.Add(UAnimBlueprint::StaticClass()->GetClassPathName());
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
						// Registry tags hold export text ("/Script/Engine.Skeleton'/Game/X.X'").
						FString Tag;
						if (Asset.GetTagValue(TEXT("TargetSkeleton"), Tag))
						{
							Item->SetStringField(TEXT("skeleton"), FPackageName::ExportTextPathToObjectPath(Tag));
						}
						if (Asset.GetTagValue(TEXT("ParentClass"), Tag))
						{
							Item->SetStringField(TEXT("parent_class"), FPackageName::ExportTextPathToObjectPath(Tag));
						}
						Results.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Assets.Num());
					Data->SetArrayField(TEXT("anim_blueprints"), Results);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("inspect"))
				{
					UAnimBlueprint* Blueprint = AnimBlueprintOrError(Body, Responder);
					if (!Blueprint) { return; }

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("name"), Blueprint->GetName());
					Data->SetStringField(TEXT("path"), Blueprint->GetPathName());
					Data->SetStringField(TEXT("parent_class"),
						Blueprint->ParentClass ? Blueprint->ParentClass->GetPathName() : TEXT(""));
					Data->SetStringField(TEXT("skeleton"),
						Blueprint->TargetSkeleton ? Blueprint->TargetSkeleton->GetPathName() : TEXT(""));
					const USkeletalMesh* Preview = Blueprint->GetPreviewMesh();
					Data->SetStringField(TEXT("preview_mesh"), Preview ? Preview->GetPathName() : TEXT(""));
					Data->SetBoolField(TEXT("is_template"), Blueprint->bIsTemplate);

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
					TArray<TSharedPtr<FJsonValue>> AnimGraphs;
					for (const FMcpGraphEntry& Entry : Graphs)
					{
						if (Entry.Category == TEXT("anim_graph"))
						{
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("name"), Entry.Graph->GetName());
							Item->SetStringField(TEXT("path"), Entry.Path);
							Item->SetNumberField(TEXT("node_count"), Entry.Graph->Nodes.Num());
							AnimGraphs.Add(MakeShared<FJsonValueObject>(Item));
						}
					}
					Data->SetArrayField(TEXT("anim_graphs"), AnimGraphs);

					TArray<UAnimationStateMachineGraph*> Machines;
					Anim::CollectStateMachines(Blueprint, Machines);
					TArray<TSharedPtr<FJsonValue>> MachineValues;
					for (UAnimationStateMachineGraph* Machine : Machines)
					{
						MachineValues.Add(MakeShared<FJsonValueObject>(Anim::StateMachineToJson(Machine)));
					}
					Data->SetArrayField(TEXT("state_machines"), MachineValues);
					const TSharedRef<FJsonObject> Layers = AnimLayers::LayersJson(Blueprint);
					Data->SetArrayField(TEXT("layers"), Layers->GetArrayField(TEXT("layers")));
					Data->SetArrayField(TEXT("implemented_interfaces"), Layers->GetArrayField(TEXT("implemented_interfaces")));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — use list or inspect"), *Operation));
			});

		// --------------------------------------------------------------- modify
		Core.RegisterRoute(TEXT("/api/anim_blueprints/modify"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path, SkeletonSpec, ParentSpec, PreviewSpec;
					Body->TryGetStringField(TEXT("path"), Path);
					Body->TryGetStringField(TEXT("skeleton"), SkeletonSpec);
					Body->TryGetStringField(TEXT("parent_class"), ParentSpec);
					Body->TryGetStringField(TEXT("preview_mesh"), PreviewSpec);
					if (Path.IsEmpty() || SkeletonSpec.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'path' and 'skeleton' are required, e.g. /Game/Anim/ABP_Hero and /Game/Characters/SK_Hero_Skeleton"));
						return;
					}

					UObject* SkeletonObject = LoadAssetFlexible(SkeletonSpec);
					USkeleton* Skeleton = Cast<USkeleton>(SkeletonObject);
					USkeletalMesh* PreviewMesh = Cast<USkeletalMesh>(LoadAssetFlexible(PreviewSpec));
					if (USkeletalMesh* MeshAsSkeleton = Cast<USkeletalMesh>(SkeletonObject))
					{
						// A skeletal mesh is accepted in place of its skeleton.
						Skeleton = MeshAsSkeleton->GetSkeleton();
						if (PreviewMesh == nullptr)
						{
							PreviewMesh = MeshAsSkeleton;
						}
					}
					if (Skeleton == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("skeleton_not_found"),
							FString::Printf(TEXT("no Skeleton (or SkeletalMesh) at '%s'"), *SkeletonSpec));
						return;
					}
					UClass* ParentClass = ParentSpec.IsEmpty() ? nullptr : ResolveClass(ParentSpec);
					if (!ParentSpec.IsEmpty() && ParentClass == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_class"),
							FString::Printf(TEXT("parent class '%s' not found"), *ParentSpec));
						return;
					}
					if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateAnimBlueprint", "McpLink Create Animation Blueprint"));
					FString Error;
					UAnimBlueprint* Created =
						Anim::CreateAnimBlueprint(Path, Skeleton, ParentClass, PreviewMesh, Error);
					if (Created == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("create_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Created->GetPathName());
					Data->SetStringField(TEXT("name"), Created->GetName());
					Data->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
					Data->SetStringField(TEXT("parent_class"),
						Created->ParentClass ? Created->ParentClass->GetPathName() : TEXT(""));
					const UAnimationGraph* AnimGraph = Anim::FindAnimGraph(Created, FString());
					Data->SetStringField(TEXT("anim_graph"), AnimGraph ? AnimGraph->GetName() : TEXT(""));
					Data->SetStringField(TEXT("message"),
						TEXT("created in memory — add_state_machine next, then blueprint_modify compile and save"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_layer_interface"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("the interface's package path, e.g. /Game/Anim/ALI_Hero")))
					{
						return;
					}
					if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateAnimLayerInterface", "McpLink Create Animation Layer Interface"));
					UPackage* Package = CreatePackage(*Path);
					// What the Animation Layer Interface factory does: an interface-kind
					// Animation Blueprint whose parent is the marker interface.
					UAnimBlueprint* Interface = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(
						UAnimLayerInterface::StaticClass(), Package, FName(*FPackageName::GetShortName(Path)), BPTYPE_Interface,
						UAnimBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
					if (Interface == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("create_failed"), TEXT("CreateBlueprint returned null"));
						return;
					}
					FAssetRegistryModule::AssetCreated(Interface);
					Interface->MarkPackageDirty();
					FString Group;
					Body->TryGetStringField(TEXT("group"), Group);
					const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
					if (Body->TryGetArrayField(TEXT("layers"), Layers))
					{
						for (const TSharedPtr<FJsonValue>& Layer : *Layers)
						{
							FString Error;
							if (!Layer.IsValid() || AnimLayers::AddLayerGraph(Interface, Layer->AsString(), Group, Error) == nullptr)
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_layer"), Error);
								return;
							}
						}
					}
					// Compiled, so the interface class carries the layer functions
					// another Blueprint's implement_layer_interface looks for.
					FKismetEditorUtilities::CompileBlueprint(Interface);
					const TSharedRef<FJsonObject> Data = AnimLayers::LayersJson(Interface);
					Data->SetStringField(TEXT("message"), TEXT("created in memory — implement_layer_interface on an Animation Blueprint, then blueprint_modify save both"));
					Responder->Ok(Data);
					return;
				}

				UAnimBlueprint* Blueprint = AnimBlueprintOrError(Body, Responder);
				if (!Blueprint) { return; }

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "ModifyAnimBlueprint", "McpLink Modify Animation Blueprint"));
				Blueprint->Modify();

				if (Operation == TEXT("add_layer"))
				{
					FString Name, Group;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the layer's name")))
					{
						return;
					}
					Body->TryGetStringField(TEXT("group"), Group);
					if (Blueprint->BlueprintType != BPTYPE_Interface)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_an_interface"),
							TEXT("layers are declared on an Animation Layer Interface (create_layer_interface); a normal Animation Blueprint gets them through implement_layer_interface"));
						return;
					}
					FString Error;
					if (AnimLayers::AddLayerGraph(Blueprint, Name, Group, Error) == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("invalid_layer"), Error);
						return;
					}
					FKismetEditorUtilities::CompileBlueprint(Blueprint);
					Responder->Ok(AnimLayers::LayersJson(Blueprint));
					return;
				}

				if (Operation == TEXT("implement_layer_interface"))
				{
					FString InterfaceSpec;
					if (!RequireString(Body, TEXT("interface"), InterfaceSpec, Responder, TEXT("an Animation Layer Interface asset path")))
					{
						return;
					}
					UAnimBlueprint* Interface = Cast<UAnimBlueprint>(LoadAssetFlexible(InterfaceSpec));
					if (Interface == nullptr || Interface->BlueprintType != BPTYPE_Interface || Interface->GeneratedClass == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("interface_not_found"),
							FString::Printf(TEXT("'%s' is not a compiled Animation Layer Interface"), *InterfaceSpec));
						return;
					}
					if (!FBlueprintEditorUtils::ImplementNewInterface(Blueprint, FTopLevelAssetPath(Interface->GeneratedClass)))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("implement_refused"),
							TEXT("the Blueprint refused the interface — already implemented, or it is not an Animation Blueprint (see get_logs)"));
						return;
					}
					const TSharedRef<FJsonObject> Data = AnimLayers::LayersJson(Blueprint);
					Data->SetStringField(TEXT("message"), TEXT("each layer is now an animation graph named after it — build its pose with blueprint_modify, then add_linked_layer_node runs it in AnimGraph"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_linked_layer_node") || Operation == TEXT("add_linked_graph_node"))
				{
					FString GraphName;
					Body->TryGetStringField(TEXT("graph"), GraphName);
					UAnimationGraph* Graph = Anim::FindAnimGraph(Blueprint, GraphName);
					if (Graph == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("graph_not_found"),
							FString::Printf(TEXT("no animation graph '%s' in the Blueprint"), *GraphName));
						return;
					}
					const int32 X = IntOr(Body, TEXT("x"), 0);
					const int32 Y = IntOr(Body, TEXT("y"), 0);
					UAnimGraphNode_LinkedAnimGraphBase* Node = nullptr;
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					if (Operation == TEXT("add_linked_layer_node"))
					{
						FString Layer;
						if (!RequireString(Body, TEXT("layer"), Layer, Responder, TEXT("a layer name from an implemented interface, or a self layer")))
						{
							return;
						}
						TSubclassOf<UInterface> Interface;
						FGuid Guid;
						if (AnimLayers::FindLayerGraph(Blueprint, FName(*Layer), Interface, Guid) == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("layer_not_found"),
								FString::Printf(TEXT("the Blueprint has no layer '%s' — implement_layer_interface adds an interface's layers"), *Layer));
							return;
						}
						FGraphNodeCreator<UAnimGraphNode_LinkedAnimLayer> Creator(*Graph);
						UAnimGraphNode_LinkedAnimLayer* LayerNode = Creator.CreateNode();
						LayerNode->NodePosX = X;
						LayerNode->NodePosY = Y;
						// What SetupFromLayerId does (it is not exported): the layer name,
						// the interface and its graph guid, and the function reference
						// the pins are allocated from.
						LayerNode->Node.Layer = FName(*Layer);
						LayerNode->Node.Interface = Interface != nullptr && Interface->IsChildOf(UAnimLayerInterface::StaticClass())
							? TSubclassOf<UAnimLayerInterface>(*Interface) : nullptr;
						LayerNode->InterfaceGuid = Guid;
						if (FStructProperty* ReferenceProperty = CastField<FStructProperty>(UAnimGraphNode_LinkedAnimGraphBase::StaticClass()->FindPropertyByName(TEXT("FunctionReference"))))
						{
							FMemberReference* Reference = ReferenceProperty->ContainerPtrToValuePtr<FMemberReference>(LayerNode);
							if (Interface != nullptr)
							{
								Reference->SetExternalMember(FName(*Layer), *Interface, Guid);
							}
							else
							{
								Reference->SetSelfMember(FName(*Layer));
							}
						}
						Creator.Finalize();
						Node = LayerNode;
						Data->SetStringField(TEXT("layer"), Layer);
						Data->SetStringField(TEXT("interface"), Interface != nullptr ? Interface->GetPathName() : TEXT("self"));
					}
					else
					{
						FString ClassSpec;
						if (!RequireString(Body, TEXT("instance_class"), ClassSpec, Responder, TEXT("an Animation Blueprint (or AnimInstance class) to run")))
						{
							return;
						}
						UClass* InstanceClass = AnimLayers::ResolveAnimClass(ClassSpec);
						if (InstanceClass == nullptr || !InstanceClass->IsChildOf(UAnimInstance::StaticClass()))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("class_not_found"),
								FString::Printf(TEXT("'%s' is not an Animation Blueprint or AnimInstance class"), *ClassSpec));
							return;
						}
						FGraphNodeCreator<UAnimGraphNode_LinkedAnimGraph> Creator(*Graph);
						UAnimGraphNode_LinkedAnimGraph* GraphNode = Creator.CreateNode();
						GraphNode->NodePosX = X;
						GraphNode->NodePosY = Y;
						GraphNode->Node.InstanceClass = InstanceClass;
						Creator.Finalize();
						Node = GraphNode;
						Data->SetStringField(TEXT("instance_class"), InstanceClass->GetPathName());
					}
					Node->ReconstructNode();
					if (BoolOr(Body, TEXT("connect_to_output"), true))
					{
						FString Error;
						if (!AnimLayers::ConnectPoseToGraphResult(Graph, Node->FindPin(TEXT("Pose"), EGPD_Output), Error))
						{
							Data->SetStringField(TEXT("warning"), Error);
						}
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
					Data->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
					Data->SetStringField(TEXT("graph"), Graph->GetName());
					Data->SetStringField(TEXT("node"), Node->GetName());
					TArray<TSharedPtr<FJsonValue>> Pins;
					for (UEdGraphPin* Pin : Node->Pins)
					{
						Pins.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s (%s, %s)"), *Pin->PinName.ToString(),
							Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out"), *Pin->PinType.PinCategory.ToString())));
					}
					Data->SetArrayField(TEXT("pins"), Pins);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_state_machine"))
				{
					FString Name, GraphName;
					Body->TryGetStringField(TEXT("name"), Name);
					Body->TryGetStringField(TEXT("graph"), GraphName);
					UAnimationGraph* Graph = Anim::FindAnimGraph(Blueprint, GraphName);
					if (Graph == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("graph_not_found"),
							FString::Printf(TEXT("no animation graph '%s' — omit 'graph' for the main AnimGraph"),
								*GraphName));
						return;
					}
					const TOptional<bool> ConnectOpt = OptionalBool(Body, TEXT("connect_to_output"));
					FString Warning;
					UAnimGraphNode_StateMachine* Node = Anim::AddStateMachine(
						Graph, Name, ConnectOpt.Get(true), IntOr(Body, TEXT("x"), -300), IntOr(Body, TEXT("y"), 0), Warning);
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("state_machine"), Node->GetStateMachineName());
					Data->SetStringField(TEXT("node_guid"), Node->NodeGuid.ToString());
					Data->SetStringField(TEXT("graph"),
						Node->EditorStateMachineGraph ? GraphPath(Node->EditorStateMachineGraph) : TEXT(""));
					Data->SetBoolField(TEXT("connected_to_output"), ConnectOpt.Get(true) && Warning.IsEmpty());
					if (!Warning.IsEmpty())
					{
						Data->SetStringField(TEXT("warning"), Warning);
					}
					Responder->Ok(Data);
					return;
				}

				UAnimationStateMachineGraph* Machine = MachineOrError(Blueprint, Body, Responder);
				if (!Machine) { return; }

				if (Operation == TEXT("add_state"))
				{
					FString Name, Animation;
					Body->TryGetStringField(TEXT("name"), Name);
					Body->TryGetStringField(TEXT("animation"), Animation);
					if (Name.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'name' is required"));
						return;
					}
					UAnimationAsset* Asset = nullptr;
					if (!Animation.IsEmpty() && (Asset = AnimationOrError(Animation, Responder)) == nullptr)
					{
						return;
					}

					UAnimStateNode* State =
						Anim::AddState(Machine, Name, IntOr(Body, TEXT("x"), 0), IntOr(Body, TEXT("y"), 0));
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					FString Warning;
					if (Asset != nullptr)
					{
						const TOptional<double> PlayRate = OptionalNumber(Body, TEXT("play_rate"));
						FString Error;
						if (Anim::SetStateAnimation(Blueprint, State, Asset, OptionalBool(Body, TEXT("loop_animation")),
							PlayRate.IsSet() ? TOptional<float>(static_cast<float>(*PlayRate)) : TOptional<float>(),
							Error) == nullptr)
						{
							Warning = Error;
						}
						else if (!Error.IsEmpty())
						{
							Warning = Error;
						}
					}
					if (OptionalBool(Body, TEXT("entry")).Get(false))
					{
						FString Error;
						if (!Anim::SetEntryState(Machine, State, Error))
						{
							Warning += (Warning.IsEmpty() ? TEXT("") : TEXT("; ")) + Error;
						}
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

					Data->SetObjectField(TEXT("state"), Anim::StateToJson(State));
					Data->SetStringField(TEXT("state_machine"), Machine->GetName());
					if (!Warning.IsEmpty())
					{
						Data->SetStringField(TEXT("warning"), Warning);
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_state_animation"))
				{
					UAnimStateNode* State = StateOrError(Machine, Body, TEXT("state"), Responder);
					if (!State) { return; }
					FString Animation;
					Body->TryGetStringField(TEXT("animation"), Animation);
					UAnimationAsset* Asset = AnimationOrError(Animation, Responder);
					if (!Asset) { return; }
					const TOptional<double> PlayRate = OptionalNumber(Body, TEXT("play_rate"));
					FString Error;
					UAnimGraphNode_AssetPlayerBase* Player = Anim::SetStateAnimation(
						Blueprint, State, Asset, OptionalBool(Body, TEXT("loop_animation")),
						PlayRate.IsSet() ? TOptional<float>(static_cast<float>(*PlayRate)) : TOptional<float>(),
						Error);
					if (Player == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("animation_not_set"), Error);
						return;
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetObjectField(TEXT("state"), Anim::StateToJson(State));
					Data->SetObjectField(TEXT("player"), NodeToJson(Player));
					if (!Error.IsEmpty())
					{
						Data->SetStringField(TEXT("warning"), Error);
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_entry_state"))
				{
					UAnimStateNode* State = StateOrError(Machine, Body, TEXT("state"), Responder);
					if (!State) { return; }
					FString Error;
					if (!Anim::SetEntryState(Machine, State, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("entry_not_set"), Error);
						return;
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("entry_state"), State->GetStateName());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_transition"))
				{
					UAnimStateNode* From = StateOrError(Machine, Body, TEXT("from"), Responder);
					if (!From) { return; }
					UAnimStateNode* To = StateOrError(Machine, Body, TEXT("to"), Responder);
					if (!To) { return; }
					FString Error;
					UAnimStateTransitionNode* Transition = Anim::AddTransition(From, To, Error);
					if (Transition == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("transition_not_created"), Error);
						return;
					}
					if (!ApplyTransitionSettings(Blueprint, Transition, Body, Responder))
					{
						return;
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
					const TSharedRef<FJsonObject> Data = Anim::TransitionToJson(Transition);
					Data->SetStringField(TEXT("message"),
						TEXT("build custom rules with blueprint_modify add_node / connect_pins in rule_graph, ")
						TEXT("feeding the result node's bCanEnterTransition pin"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_transition"))
				{
					FString Guid;
					Body->TryGetStringField(TEXT("transition"), Guid);
					UAnimStateTransitionNode* Transition = Anim::FindTransition(Machine, Guid);
					if (Transition == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("transition_not_found"),
							FString::Printf(TEXT("no transition with guid '%s' in '%s' — see inspect"),
								*Guid, *Machine->GetName()));
						return;
					}
					if (!ApplyTransitionSettings(Blueprint, Transition, Body, Responder))
					{
						return;
					}
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
					Responder->Ok(Anim::TransitionToJson(Transition));
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, add_state_machine, add_state, ")
						TEXT("set_state_animation, set_entry_state, add_transition, set_transition, create_layer_interface, ")
						TEXT("add_layer, implement_layer_interface, add_linked_layer_node or add_linked_graph_node"),
						*Operation));
			});
	}
}
