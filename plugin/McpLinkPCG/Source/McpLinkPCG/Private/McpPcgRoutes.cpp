// Procedural Content Generation interop: author PCG graphs (nodes, edges,
// parameters), attach PCG components to actors, generate and inspect output.
//
// Node settings are ordinary UObjects, so the long tail of per-node options is
// reachable through get_property / set_property on the reported settings_path.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "PCGCommon.h"
#include "PCGComponent.h"
#include "PCGData.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "ScopedTransaction.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace
	{
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

		UPCGGraphInterface* GraphInterfaceOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			Body->TryGetStringField(TEXT("graph"), Spec);
			if (Spec.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'graph' asset path is required, e.g. /Game/PCG/PCG_Forest"));
				return nullptr;
			}
			UPCGGraphInterface* Graph = Cast<UPCGGraphInterface>(LoadAssetFlexible(Spec));
			if (Graph == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("graph_not_found"),
					FString::Printf(TEXT("no PCG graph at '%s' — see pcg_ops list_graphs"), *Spec));
			}
			return Graph;
		}

		UPCGGraph* GraphOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			UPCGGraphInterface* Interface = GraphInterfaceOrError(Body, Responder);
			if (Interface == nullptr)
			{
				return nullptr;
			}
			UPCGGraph* Graph = Interface->GetGraph();
			if (Graph == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("graph_not_found"),
					TEXT("graph instance has no underlying graph"));
			}
			return Graph;
		}

		UPCGComponent* ComponentOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString ComponentPath, ActorSpec;
			Body->TryGetStringField(TEXT("component"), ComponentPath);
			Body->TryGetStringField(TEXT("actor"), ActorSpec);
			if (!ComponentPath.IsEmpty())
			{
				if (UPCGComponent* Component = Cast<UPCGComponent>(ResolveObject(ComponentPath)))
				{
					return Component;
				}
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("component_not_found"),
					FString::Printf(TEXT("no PCGComponent at '%s' — see pcg_ops list_components"), *ComponentPath));
				return nullptr;
			}
			if (!ActorSpec.IsEmpty())
			{
				UWorld* World = ResolveWorld(Body);
				AActor* Actor = World ? ResolveActor(World, ActorSpec) : nullptr;
				UPCGComponent* Component = Actor ? Actor->FindComponentByClass<UPCGComponent>() : nullptr;
				if (Component == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("component_not_found"),
						FString::Printf(TEXT("actor '%s' not found or has no PCGComponent — use attach"), *ActorSpec));
				}
				return Component;
			}
			Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
				TEXT("'component' (object path) or 'actor' is required"));
			return nullptr;
		}

		UPCGNode* FindNode(UPCGGraph* Graph, const FString& Spec)
		{
			if (Spec.Equals(TEXT("Input"), ESearchCase::IgnoreCase))
			{
				return Graph->GetInputNode();
			}
			if (Spec.Equals(TEXT("Output"), ESearchCase::IgnoreCase))
			{
				return Graph->GetOutputNode();
			}
			for (UPCGNode* Node : Graph->GetNodes())
			{
				if (Node != nullptr
					&& (Node->GetName() == Spec
						|| Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString() == Spec))
				{
					return Node;
				}
			}
			return nullptr;
		}

		UPCGNode* NodeOrError(
			UPCGGraph* Graph, const TSharedRef<FJsonObject>& Body, const TCHAR* Field,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			Body->TryGetStringField(Field, Spec);
			if (Spec.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					FString::Printf(TEXT("'%s' (node id from graph_info, or Input / Output) is required"), Field));
				return nullptr;
			}
			UPCGNode* Node = FindNode(Graph, Spec);
			if (Node == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"),
					FString::Printf(TEXT("no node '%s' in %s — see graph_info"), *Spec, *Graph->GetName()));
			}
			return Node;
		}

		FString NodeId(const UPCGNode* Node, const UPCGGraph* Graph)
		{
			if (Node == Graph->GetInputNode()) { return TEXT("Input"); }
			if (Node == Graph->GetOutputNode()) { return TEXT("Output"); }
			return Node->GetName();
		}

		TArray<TSharedPtr<FJsonValue>> PinsToJson(
			const TArray<TObjectPtr<UPCGPin>>& Pins, const UPCGGraph* Graph, bool bInput)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const UPCGPin* Pin : Pins)
			{
				if (Pin == nullptr)
				{
					continue;
				}
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("label"), Pin->Properties.Label.ToString());
				TArray<TSharedPtr<FJsonValue>> Links;
				for (const UPCGEdge* Edge : Pin->Edges)
				{
					if (Edge == nullptr)
					{
						continue;
					}
					const UPCGPin* Other = bInput ? Edge->InputPin.Get() : Edge->OutputPin.Get();
					if (Other == nullptr || Other->Node == nullptr)
					{
						continue;
					}
					const TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
					Link->SetStringField(TEXT("node"), NodeId(Other->Node, Graph));
					Link->SetStringField(TEXT("pin"), Other->Properties.Label.ToString());
					Links.Add(MakeShared<FJsonValueObject>(Link));
				}
				Item->SetArrayField(TEXT("connected_to"), Links);
				Out.Add(MakeShared<FJsonValueObject>(Item));
			}
			return Out;
		}

		TSharedRef<FJsonObject> NodeToJson(UPCGNode* Node, const UPCGGraph* Graph)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("id"), NodeId(Node, Graph));
			Object->SetStringField(TEXT("title"), Node->GetNodeTitle(EPCGNodeTitleType::FullTitle).ToString());
			if (const UPCGSettings* Settings = Node->GetSettings())
			{
				Object->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
				// get_property / set_property on this path edits the node's options.
				Object->SetStringField(TEXT("settings_path"), Settings->GetPathName());
			}
			Object->SetNumberField(TEXT("x"), Node->PositionX);
			Object->SetNumberField(TEXT("y"), Node->PositionY);
			Object->SetArrayField(TEXT("inputs"), PinsToJson(Node->GetInputPins(), Graph, true));
			Object->SetArrayField(TEXT("outputs"), PinsToJson(Node->GetOutputPins(), Graph, false));
			return Object;
		}

		TArray<TSharedPtr<FJsonValue>> ParametersToJson(const FInstancedPropertyBag* Bag)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			if (Bag == nullptr || Bag->GetPropertyBagStruct() == nullptr)
			{
				return Out;
			}
			for (const FPropertyBagPropertyDesc& Desc : Bag->GetPropertyBagStruct()->GetPropertyDescs())
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Desc.Name.ToString());
				Item->SetStringField(TEXT("type"), UEnum::GetDisplayValueAsText(Desc.ValueType).ToString());
				if (Desc.ValueTypeObject != nullptr)
				{
					Item->SetStringField(TEXT("type_object"), Desc.ValueTypeObject->GetPathName());
				}
				const TValueOrError<FString, EPropertyBagResult> Value = Bag->GetValueSerializedString(Desc.Name);
				Item->SetStringField(TEXT("value"), Value.HasValue() ? Value.GetValue() : TEXT(""));
				Out.Add(MakeShared<FJsonValueObject>(Item));
			}
			return Out;
		}

		TSharedRef<FJsonObject> ComponentToJson(UPCGComponent* Component)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("component"), Component->GetPathName());
			if (const AActor* Owner = Component->GetOwner())
			{
				Object->SetStringField(TEXT("owner"), Owner->GetPathName());
				Object->SetStringField(TEXT("owner_label"), Owner->GetActorLabel());
			}
			const UPCGGraph* Graph = Component->GetGraph();
			Object->SetStringField(TEXT("graph"), Graph ? Graph->GetPathName() : TEXT(""));
			Object->SetBoolField(TEXT("generated"), Component->bGenerated);
			Object->SetBoolField(TEXT("generating"), Component->IsGenerating());
			Object->SetBoolField(TEXT("activated"), Component->bActivated);
			Object->SetBoolField(TEXT("partitioned"), Component->bIsComponentPartitioned);
			Object->SetNumberField(TEXT("seed"), Component->Seed);
			Object->SetStringField(TEXT("generation_trigger"),
				UEnum::GetDisplayValueAsText(Component->GenerationTrigger).ToString());
			Object->SetNumberField(TEXT("output_data_count"), Component->GetGeneratedGraphOutput().TaggedData.Num());
			if (const UPCGGraphInstance* Instance = Component->GetGraphInstance())
			{
				Object->SetArrayField(TEXT("parameters"), ParametersToJson(Instance->GetUserParametersStruct()));
			}
			return Object;
		}

		FString SerializedValue(const TSharedPtr<FJsonValue>& Value)
		{
			FString Text;
			double Number = 0.0;
			bool Flag = false;
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (!Value.IsValid())
			{
				return FString();
			}
			if (Value->TryGetString(Text))
			{
				return Text;
			}
			if (Value->TryGetBool(Flag))
			{
				return Flag ? TEXT("True") : TEXT("False");
			}
			if (Value->TryGetNumber(Number))
			{
				return FString::SanitizeFloat(Number);
			}
			if (Value->TryGetArray(Array))
			{
				static const TCHAR* Axes[] = {TEXT("X"), TEXT("Y"), TEXT("Z"), TEXT("W")};
				TArray<FString> Parts;
				for (int32 Index = 0; Index < Array->Num() && Index < 4; ++Index)
				{
					double Component = 0.0;
					(*Array)[Index]->TryGetNumber(Component);
					Parts.Add(FString::Printf(TEXT("%s=%s"), Axes[Index], *FString::SanitizeFloat(Component)));
				}
				return FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
			}
			return FString();
		}
	}

	void RegisterPcgRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/pcg/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				// ------------------------------------------------ discovery
				if (Operation == TEXT("list_graphs"))
				{
					FString PathPrefix = TEXT("/Game");
					Body->TryGetStringField(TEXT("path_prefix"), PathPrefix);
					IAssetRegistry& Registry =
						FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
					FARFilter Filter;
					Filter.ClassPaths.Add(UPCGGraphInterface::StaticClass()->GetClassPathName());
					Filter.PackagePaths.Add(FName(*PathPrefix));
					Filter.bRecursivePaths = true;
					Filter.bRecursiveClasses = true;
					TArray<FAssetData> Assets;
					Registry.GetAssets(Filter, Assets);
					TArray<TSharedPtr<FJsonValue>> Results;
					for (const FAssetData& Asset : Assets)
					{
						if (Results.Num() >= 300) { break; }
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Asset.AssetName.ToString());
						Item->SetStringField(TEXT("path"), Asset.PackageName.ToString());
						Item->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
						Results.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Assets.Num());
					Data->SetArrayField(TEXT("graphs"), Results);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_settings_classes"))
				{
					FString Filter;
					Body->TryGetStringField(TEXT("filter"), Filter);
					TArray<TSharedPtr<FJsonValue>> Classes;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						UClass* Class = *It;
						if (!Class->IsChildOf(UPCGSettings::StaticClass())
							|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
							|| Class->GetName().StartsWith(TEXT("SKEL_")) || Class->GetName().StartsWith(TEXT("REINST_")))
						{
							continue;
						}
						const UPCGSettings* Defaults = GetDefault<UPCGSettings>(Class);
						const FString Title = Defaults ? Defaults->GetDefaultNodeTitle().ToString() : FString();
						if (!Filter.IsEmpty() && !Class->GetName().Contains(Filter) && !Title.Contains(Filter))
						{
							continue;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("class"), Class->GetName());
						Item->SetStringField(TEXT("title"), Title);
						Classes.Add(MakeShared<FJsonValueObject>(Item));
						if (Classes.Num() >= 400) { break; }
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("count"), Classes.Num());
					Data->SetArrayField(TEXT("settings_classes"), Classes);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_components"))
				{
					UWorld* World = ResolveWorld(Body);
					if (World == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_world"),
							TEXT("no matching world — is PIE running?"));
						return;
					}
					TArray<TSharedPtr<FJsonValue>> Components;
					for (TObjectIterator<UPCGComponent> It; It; ++It)
					{
						UPCGComponent* Component = *It;
						if (Component == nullptr || Component->IsTemplate() || !IsValid(Component)
							|| Component->GetWorld() != World)
						{
							continue;
						}
						Components.Add(MakeShared<FJsonValueObject>(ComponentToJson(Component)));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("world"), World->GetName());
					Data->SetNumberField(TEXT("count"), Components.Num());
					Data->SetArrayField(TEXT("components"), Components);
					Responder->Ok(Data);
					return;
				}

				// ------------------------------------------------ graph authoring
				if (Operation == TEXT("create_graph"))
				{
					FString Path;
					Body->TryGetStringField(TEXT("path"), Path);
					if (Path.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'path' is required, e.g. /Game/PCG/PCG_Forest"));
						return;
					}
					if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "CreatePcgGraph", "McpLink Create PCG Graph"));
					UPackage* Package = CreatePackage(*Path);
					UPCGGraph* Graph = NewObject<UPCGGraph>(
						Package, FName(*FPackageName::GetShortName(Path)), RF_Public | RF_Standalone | RF_Transactional);
					FAssetRegistryModule::AssetCreated(Graph);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("graph"), Graph->GetPathName());
					Data->SetStringField(TEXT("message"),
						TEXT("created in memory — add_node / connect, then save; attach it to an actor and generate"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("graph_info"))
				{
					UPCGGraph* Graph = GraphOrError(Body, Responder);
					if (!Graph) { return; }
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("graph"), Graph->GetPathName());
					TArray<TSharedPtr<FJsonValue>> Nodes;
					Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Graph->GetInputNode(), Graph)));
					for (UPCGNode* Node : Graph->GetNodes())
					{
						if (Node != nullptr)
						{
							Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node, Graph)));
						}
					}
					Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Graph->GetOutputNode(), Graph)));
					Data->SetArrayField(TEXT("nodes"), Nodes);
					Data->SetArrayField(TEXT("parameters"), ParametersToJson(Graph->GetUserParametersStruct()));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_node") || Operation == TEXT("remove_node") || Operation == TEXT("connect")
					|| Operation == TEXT("disconnect") || Operation == TEXT("save"))
				{
					UPCGGraph* Graph = GraphOrError(Body, Responder);
					if (!Graph) { return; }

					if (Operation == TEXT("save"))
					{
						UPackage* Package = Graph->GetOutermost();
						const FString Filename = FPackageName::LongPackageNameToFilename(
							Package->GetName(), FPackageName::GetAssetPackageExtension());
						FSavePackageArgs SaveArgs;
						SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
						const bool bSaved = UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs);
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetBoolField(TEXT("saved"), bSaved);
						Data->SetStringField(TEXT("file"), Filename);
						Responder->Ok(Data);
						return;
					}

					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "EditPcgGraph", "McpLink Edit PCG Graph"));
					Graph->Modify();

					if (Operation == TEXT("add_node"))
					{
						FString ClassSpec;
						Body->TryGetStringField(TEXT("settings_class"), ClassSpec);
						UClass* Class = ClassSpec.IsEmpty() ? nullptr : ResolveClass(ClassSpec);
						if (Class == nullptr && !ClassSpec.IsEmpty() && !ClassSpec.StartsWith(TEXT("PCG")))
						{
							Class = ResolveClass(TEXT("PCG") + ClassSpec + TEXT("Settings"));
						}
						if (Class == nullptr || !Class->IsChildOf(UPCGSettings::StaticClass()))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("settings_class_not_found"),
								FString::Printf(TEXT("'%s' is not a PCGSettings class — see list_settings_classes"), *ClassSpec));
							return;
						}
						UPCGSettings* Settings = nullptr;
						UPCGNode* Node = Graph->AddNodeOfType(Class, Settings);
						if (Node == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("node_not_created"),
								TEXT("AddNodeOfType returned null"));
							return;
						}
						double X = 0, Y = 0;
						Body->TryGetNumberField(TEXT("x"), X);
						Body->TryGetNumberField(TEXT("y"), Y);
						Node->PositionX = static_cast<int32>(X);
						Node->PositionY = static_cast<int32>(Y);
						Responder->Ok(NodeToJson(Node, Graph));
						return;
					}

					if (Operation == TEXT("remove_node"))
					{
						UPCGNode* Node = NodeOrError(Graph, Body, TEXT("node"), Responder);
						if (!Node) { return; }
						if (Node == Graph->GetInputNode() || Node == Graph->GetOutputNode())
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("protected_node"),
								TEXT("the Input and Output nodes cannot be removed"));
							return;
						}
						const FString Id = NodeId(Node, Graph);
						Graph->RemoveNode(Node);
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("removed"), Id);
						Responder->Ok(Data);
						return;
					}

					// connect / disconnect
					UPCGNode* From = NodeOrError(Graph, Body, TEXT("from"), Responder);
					if (!From) { return; }
					UPCGNode* To = NodeOrError(Graph, Body, TEXT("to"), Responder);
					if (!To) { return; }
					FString FromPin, ToPin;
					Body->TryGetStringField(TEXT("from_pin"), FromPin);
					Body->TryGetStringField(TEXT("to_pin"), ToPin);
					if (FromPin.IsEmpty() && !From->GetOutputPins().IsEmpty() && From->GetOutputPins()[0])
					{
						FromPin = From->GetOutputPins()[0]->Properties.Label.ToString();
					}
					if (ToPin.IsEmpty() && !To->GetInputPins().IsEmpty() && To->GetInputPins()[0])
					{
						ToPin = To->GetInputPins()[0]->Properties.Label.ToString();
					}
					if (From->GetOutputPin(FName(*FromPin)) == nullptr || To->GetInputPin(FName(*ToPin)) == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("pin_not_found"),
							FString::Printf(TEXT("no output pin '%s' on %s or no input pin '%s' on %s — see graph_info"),
								*FromPin, *NodeId(From, Graph), *ToPin, *NodeId(To, Graph)));
						return;
					}
					bool bOk;
					if (Operation == TEXT("connect"))
					{
						bOk = Graph->AddEdge(From, FName(*FromPin), To, FName(*ToPin)) != nullptr;
					}
					else
					{
						bOk = Graph->RemoveEdge(From, FName(*FromPin), To, FName(*ToPin));
					}
					if (!bOk)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("edge_failed"),
							Operation == TEXT("connect")
								? TEXT("PCG refused the edge (incompatible pin types or already connected)")
								: TEXT("no such edge"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("connected"), Operation == TEXT("connect"));
					Data->SetObjectField(TEXT("to"), NodeToJson(To, Graph));
					Responder->Ok(Data);
					return;
				}

				// ------------------------------------------------ components
				if (Operation == TEXT("attach"))
				{
					FString ActorSpec;
					Body->TryGetStringField(TEXT("actor"), ActorSpec);
					UWorld* World = ResolveWorld(Body);
					AActor* Actor = (World && !ActorSpec.IsEmpty()) ? ResolveActor(World, ActorSpec) : nullptr;
					if (Actor == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
							FString::Printf(TEXT("no actor '%s'"), *ActorSpec));
						return;
					}
					UPCGGraphInterface* Graph = nullptr;
					FString GraphSpec;
					if (Body->TryGetStringField(TEXT("graph"), GraphSpec) && !GraphSpec.IsEmpty())
					{
						Graph = GraphInterfaceOrError(Body, Responder);
						if (!Graph) { return; }
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AttachPcg", "McpLink Attach PCG Component"), ShouldTransact(Actor));
					Actor->Modify();
					UPCGComponent* Component = NewObject<UPCGComponent>(Actor, NAME_None, RF_Transactional);
					Actor->AddInstanceComponent(Component);
					Component->OnComponentCreated();
					Component->RegisterComponent();
					if (Graph != nullptr)
					{
						Component->SetGraph(Graph);
					}
					double Seed = 0;
					if (Body->TryGetNumberField(TEXT("seed"), Seed))
					{
						Component->Seed = static_cast<int32>(Seed);
					}
					Component->GenerationTrigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
					Responder->Ok(ComponentToJson(Component));
					return;
				}

				UPCGComponent* Component = ComponentOrError(Body, Responder);
				if (!Component) { return; }

				if (Operation == TEXT("component_info"))
				{
					Responder->Ok(ComponentToJson(Component));
					return;
				}
				if (Operation == TEXT("generate"))
				{
					bool bForce = true;
					Body->TryGetBoolField(TEXT("force"), bForce);
					Component->Generate(bForce);
					const TSharedRef<FJsonObject> Data = ComponentToJson(Component);
					Data->SetStringField(TEXT("message"),
						TEXT("generation is scheduled asynchronously — poll component_info until generating is false"));
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("cleanup"))
				{
					bool bRemoveComponents = true;
					Body->TryGetBoolField(TEXT("remove_components"), bRemoveComponents);
					Component->Cleanup(bRemoveComponents);
					Responder->Ok(ComponentToJson(Component));
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditPcgComponent", "McpLink Edit PCG Component"), ShouldTransact(Component));
				Component->Modify();

				if (Operation == TEXT("set_graph"))
				{
					UPCGGraphInterface* Graph = GraphInterfaceOrError(Body, Responder);
					if (!Graph) { return; }
					Component->SetGraph(Graph);
					Responder->Ok(ComponentToJson(Component));
					return;
				}
				if (Operation == TEXT("set_seed"))
				{
					double Seed = 0;
					if (!Body->TryGetNumberField(TEXT("seed"), Seed))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"), TEXT("'seed' is required"));
						return;
					}
					Component->Seed = static_cast<int32>(Seed);
					Responder->Ok(ComponentToJson(Component));
					return;
				}
				if (Operation == TEXT("set_parameter"))
				{
					FString Name;
					Body->TryGetStringField(TEXT("name"), Name);
					const TSharedPtr<FJsonValue> Value = Body->TryGetField(TEXT("value"));
					UPCGGraphInstance* Instance = Component->GetGraphInstance();
					const UPCGGraph* Parent = Component->GetGraph();
					if (Name.IsEmpty() || !Value.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'name' and 'value' are required (value: number, bool, string, or [x,y,z] array)"));
						return;
					}
					if (Instance == nullptr || Parent == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_graph"),
							TEXT("component has no graph — set_graph first"));
						return;
					}
					FInstancedPropertyBag* Bag = Instance->GetMutableUserParametersStruct_Unsafe();
					const FPropertyBagPropertyDesc* Desc =
						Bag != nullptr ? Bag->FindPropertyDescByName(FName(*Name)) : nullptr;
					if (Desc == nullptr)
					{
						TArray<FString> Names;
						for (const TSharedPtr<FJsonValue>& Item : ParametersToJson(Bag))
						{
							Names.Add(Item->AsObject()->GetStringField(TEXT("name")));
						}
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("parameter_not_found"),
							FString::Printf(TEXT("graph has no parameter '%s' — available: %s"), *Name,
								Names.IsEmpty() ? TEXT("(none)") : *FString::Join(Names, TEXT(", "))));
						return;
					}
					Instance->Modify();
					Instance->UpdatePropertyOverride(Desc->CachedProperty, /*bMarkAsOverridden*/ true);
					const EPropertyBagResult Result = Bag->SetValueSerializedString(FName(*Name), SerializedValue(Value));
					if (Result != EPropertyBagResult::Success)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("value_rejected"),
							FString::Printf(TEXT("could not set '%s' from '%s' (property bag result %d)"),
								*Name, *SerializedValue(Value), static_cast<int32>(Result)));
						return;
					}
					const TSharedRef<FJsonObject> Data = ComponentToJson(Component);
					Data->SetStringField(TEXT("message"), TEXT("call generate {force:true} to regenerate with the new value"));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_graphs, list_settings_classes, list_components, create_graph, ")
						TEXT("graph_info, add_node, remove_node, connect, disconnect, save, attach, component_info, ")
						TEXT("generate, cleanup, set_graph, set_seed, or set_parameter"),
						*Operation));
			});
	}
}
