// Movie Render Queue: turning an authored Level Sequence into files on disk.
//
// sequence_ops can build a sequence but not render one, which left the whole
// cinematic pipeline stopping one step short. A render needs three things: a
// primary config (which settings produce which files), a job (which sequence,
// on which map), and an executor to run it.
//
// Rendering runs PIE, so it needs a windowed editor and it is asynchronous —
// `render` starts it and `render_status` reports progress, rather than holding
// the HTTP request open for minutes.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphSchema.h"
#include "Editor.h"
#include "Graph/MovieGraphConfig.h"
#include "Graph/MovieGraphNode.h"
#include "Graph/MovieGraphPin.h"
#include "Graph/Nodes/MovieGraphInputNode.h"
#include "Graph/Nodes/MovieGraphOutputNode.h"
#include "JsonObjectConverter.h"
#include "LevelSequence.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelineOutputSetting.h"
#include "MoviePipelinePIEExecutor.h"
#include "MoviePipelinePrimaryConfig.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineQueueSubsystem.h"
#include "MoviePipelineSetting.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace McpLink
{
	namespace MovieRender
	{
		UMoviePipelineQueueSubsystem* QueueSubsystem()
		{
			return GEditor != nullptr ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>()
									  : nullptr;
		}

		UMoviePipelinePrimaryConfig* ConfigOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("config"), Path, Responder,
					TEXT("a Movie Pipeline config asset path — movie_render create_config makes one")))
			{
				return nullptr;
			}
			UMoviePipelinePrimaryConfig* Config =
				Cast<UMoviePipelinePrimaryConfig>(ResolveAsset(Path));
			if (Config == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("config_not_found"),
					FString::Printf(TEXT("no Movie Pipeline config at '%s'"), *Path));
			}
			return Config;
		}

		TSharedRef<FJsonObject> SettingToJson(UMoviePipelineSetting* Setting)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("class"), Setting->GetClass()->GetName());
			Object->SetBoolField(TEXT("enabled"), Setting->IsEnabled());
			// Every knob a setting has is a UPROPERTY, so set_property on this
			// path is how resolution, output directory and codecs are set.
			Object->SetStringField(TEXT("path"), Setting->GetPathName());
			return Object;
		}

		// ------------------------------------------------- Movie Render Graph

		// The graph the New Movie Render Graph asset starts from: input →
		// global output settings → deferred pass → PNG output → output.
		const TCHAR* const DefaultGraphPath = TEXT("/MovieRenderPipeline/DefaultRenderGraph.DefaultRenderGraph");

		UMovieGraphConfig* GraphOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("graph"), Path, Responder,
					TEXT("a Movie Render Graph asset path — movie_render create_graph makes one")))
			{
				return nullptr;
			}
			UMovieGraphConfig* Graph = Cast<UMovieGraphConfig>(ResolveAsset(Path));
			if (Graph == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("graph_not_found"),
					FString::Printf(TEXT("no Movie Render Graph at '%s'"), *Path));
			}
			return Graph;
		}

		FString ValueTypeName(EMovieGraphValueType Type)
		{
			const UEnum* Enum = StaticEnum<EMovieGraphValueType>();
			return Enum != nullptr ? Enum->GetNameStringByValue(static_cast<int64>(Type)) : FString(TEXT("?"));
		}

		/// "NodeName.PinLabel", the address a connection reports.
		FString PinRef(const UMovieGraphPin* Pin)
		{
			return FString::Printf(TEXT("%s.%s"),
				Pin->Node != nullptr ? *Pin->Node->GetName() : TEXT("?"), *Pin->Properties.Label.ToString());
		}

		TSharedRef<FJsonObject> GraphPinToJson(const UMovieGraphPin* Pin)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("label"), Pin->Properties.Label.ToString());
			Object->SetStringField(TEXT("type"), Pin->Properties.bIsBranch ? TEXT("Branch") : ValueTypeName(Pin->Properties.Type));
			TArray<TSharedPtr<FJsonValue>> Connected;
			for (const UMovieGraphPin* Other : Pin->GetAllConnectedPins())
			{
				if (Other != nullptr)
				{
					Connected.Add(MakeShared<FJsonValueString>(PinRef(Other)));
				}
			}
			Object->SetArrayField(TEXT("connected"), Connected);
			return Object;
		}

		/// A node's overridable settings: every property the graph editor's
		/// details panel shows with its override checkbox, the dynamic ones
		/// (render layer names, CVars) included.
		TArray<TSharedPtr<FJsonValue>> NodePropertiesJson(UMovieGraphNode* Node)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FMovieGraphPropertyInfo& Info : Node->GetOverrideablePropertyInfo())
			{
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Info.Name.ToString());
				Item->SetStringField(TEXT("type"), ValueTypeName(Info.ValueType));
				if (Info.ValueTypeObject != nullptr)
				{
					Item->SetStringField(TEXT("type_object"), Info.ValueTypeObject->GetName());
				}
				Item->SetBoolField(TEXT("dynamic"), Info.bIsDynamicProperty);
				FString Value;
				bool bOverridden = false;
				if (Info.bIsDynamicProperty)
				{
					Node->GetDynamicPropertyValue(Info.Name, Value);
					bOverridden = Node->IsDynamicPropertyOverridden(Info.Name);
				}
				else if (const FProperty* Property = Node->GetClass()->FindPropertyByName(Info.Name))
				{
					Property->ExportText_InContainer(0, Value, Node, Node, Node, PPF_None);
					const FString FlagName = TEXT("bOverride_") + Info.Name.ToString();
					if (const FBoolProperty* Flag = FindFProperty<FBoolProperty>(Node->GetClass(), *FlagName))
					{
						bOverridden = Flag->GetPropertyValue_InContainer(Node);
					}
				}
				Item->SetStringField(TEXT("value"), Value);
				Item->SetBoolField(TEXT("overridden"), bOverridden);
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}
			return Items;
		}

		TSharedRef<FJsonObject> GraphNodeToJson(const UMovieGraphConfig& Graph, UMovieGraphNode* Node, bool bIncludeProperties)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Node->GetName());
			Object->SetStringField(TEXT("guid"), Node->GetGuid().ToString());
			Object->SetStringField(TEXT("class"), Node->GetClass()->GetName());
			Object->SetStringField(TEXT("title"), Node->GetNodeTitle().ToString());
			if (const UMovieGraphSettingNode* Setting = Cast<UMovieGraphSettingNode>(Node))
			{
				const FString Instance = Setting->GetNodeInstanceName();
				if (!Instance.IsEmpty())
				{
					Object->SetStringField(TEXT("instance_name"), Instance);
				}
			}
			Object->SetStringField(TEXT("kind"),
				Node == Graph.GetInputNode() ? TEXT("input")
				: Node == Graph.GetOutputNode() ? TEXT("output") : TEXT("node"));
			Object->SetStringField(TEXT("path"), Node->GetPathName());
			Object->SetBoolField(TEXT("disabled"), Node->IsDisabled());
			Object->SetNumberField(TEXT("x"), Node->GetNodePosX());
			Object->SetNumberField(TEXT("y"), Node->GetNodePosY());
			TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
			for (const UMovieGraphPin* Pin : Node->GetInputPins())
			{
				if (Pin != nullptr) { Inputs.Add(MakeShared<FJsonValueObject>(GraphPinToJson(Pin))); }
			}
			for (const UMovieGraphPin* Pin : Node->GetOutputPins())
			{
				if (Pin != nullptr) { Outputs.Add(MakeShared<FJsonValueObject>(GraphPinToJson(Pin))); }
			}
			Object->SetArrayField(TEXT("inputs"), Inputs);
			Object->SetArrayField(TEXT("outputs"), Outputs);
			if (bIncludeProperties)
			{
				Object->SetArrayField(TEXT("properties"), NodePropertiesJson(Node));
			}
			return Object;
		}

		TSharedRef<FJsonObject> GraphToJson(UMovieGraphConfig& Graph, bool bIncludeProperties)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("graph"), Graph.GetPathName());
			TArray<TSharedPtr<FJsonValue>> Nodes;
			Nodes.Add(MakeShared<FJsonValueObject>(GraphNodeToJson(Graph, Graph.GetInputNode(), false)));
			for (const TObjectPtr<UMovieGraphNode>& Node : Graph.GetNodes())
			{
				if (Node != nullptr)
				{
					Nodes.Add(MakeShared<FJsonValueObject>(GraphNodeToJson(Graph, Node, bIncludeProperties)));
				}
			}
			Nodes.Add(MakeShared<FJsonValueObject>(GraphNodeToJson(Graph, Graph.GetOutputNode(), false)));
			Object->SetArrayField(TEXT("nodes"), Nodes);
			TArray<TSharedPtr<FJsonValue>> Variables;
			for (const UMovieGraphVariable* Variable : Graph.GetVariables(/*bIncludeGlobal*/ false))
			{
				if (Variable != nullptr)
				{
					Variables.Add(MakeShared<FJsonValueString>(Variable->GetMemberName()));
				}
			}
			Object->SetArrayField(TEXT("variables"), Variables);
			return Object;
		}

		/// A node by name, GUID, path, class name or title — "Input" and
		/// "Output" are the graph's own two.
		UMovieGraphNode* FindGraphNode(UMovieGraphConfig& Graph, const FString& Spec)
		{
			if (Spec.Equals(TEXT("Input"), ESearchCase::IgnoreCase) || Spec.Equals(TEXT("Inputs"), ESearchCase::IgnoreCase))
			{
				return Graph.GetInputNode();
			}
			if (Spec.Equals(TEXT("Output"), ESearchCase::IgnoreCase) || Spec.Equals(TEXT("Outputs"), ESearchCase::IgnoreCase))
			{
				return Graph.GetOutputNode();
			}
			TArray<UMovieGraphNode*> All;
			All.Add(Graph.GetInputNode());
			for (const TObjectPtr<UMovieGraphNode>& Node : Graph.GetNodes())
			{
				if (Node != nullptr) { All.Add(Node); }
			}
			All.Add(Graph.GetOutputNode());
			for (UMovieGraphNode* Node : All)
			{
				if (Node->GetName() == Spec || Node->GetGuid().ToString() == Spec || Node->GetPathName() == Spec)
				{
					return Node;
				}
			}
			UMovieGraphNode* Loose = nullptr;
			int32 LooseMatches = 0;
			for (UMovieGraphNode* Node : All)
			{
				if (Node->GetClass()->GetName().Equals(Spec, ESearchCase::IgnoreCase)
					|| Node->GetNodeTitle().ToString().Equals(Spec, ESearchCase::IgnoreCase))
				{
					Loose = Node;
					++LooseMatches;
				}
			}
			return LooseMatches == 1 ? Loose : nullptr;
		}

		UMovieGraphNode* GraphNodeOrError(
			UMovieGraphConfig& Graph, const TSharedRef<FJsonObject>& Body, const TCHAR* Field,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			if (!RequireString(Body, Field, Spec, Responder,
					TEXT("a node name, GUID, class or title from graph_info, or Input / Output")))
			{
				return nullptr;
			}
			UMovieGraphNode* Node = FindGraphNode(Graph, Spec);
			if (Node == nullptr)
			{
				TArray<FString> Names;
				for (const TObjectPtr<UMovieGraphNode>& Existing : Graph.GetNodes())
				{
					if (Existing != nullptr)
					{
						Names.Add(FString::Printf(TEXT("%s (%s)"), *Existing->GetName(), *Existing->GetNodeTitle().ToString()));
					}
				}
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"),
					FString::Printf(TEXT("no node '%s' in the graph — it has Input, Output, %s"), *Spec,
						*FString::Join(Names, TEXT(", "))));
			}
			return Node;
		}

		/// Node classes by the names an agent would use: the class, or a
		/// shorter form with the MovieGraph prefix and Node suffix dropped.
		UClass* ResolveGraphNodeClass(const FString& Spec)
		{
			const FString Candidates[] = {
				Spec,
				TEXT("MovieGraph") + Spec,
				TEXT("MovieGraph") + Spec + TEXT("Node"),
				Spec + TEXT("Node"),
			};
			for (const FString& Candidate : Candidates)
			{
				UClass* Class = ResolveClass(Candidate);
				if (Class != nullptr && Class->IsChildOf(UMovieGraphNode::StaticClass()))
				{
					return Class;
				}
			}
			return nullptr;
		}
	}

	using namespace MovieRender;

	void RegisterMovieRenderRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/render/movie"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_setting_classes"))
				{
					FString Contains;
					Body->TryGetStringField(TEXT("name_contains"), Contains);
					TArray<TSharedPtr<FJsonValue>> Classes;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						UClass* Class = *It;
						if (!Class->IsChildOf(UMoviePipelineSetting::StaticClass())
							|| Class->HasAnyClassFlags(
								CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
						{
							continue;
						}
						const FString Name = Class->GetName();
						if (!Contains.IsEmpty() && !Name.Contains(Contains))
						{
							continue;
						}
						Classes.Add(MakeShared<FJsonValueString>(Name));
					}
					Classes.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
						{ return A->AsString() < B->AsString(); });
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("count"), Classes.Num());
					Data->SetArrayField(TEXT("classes"), Classes);
					Data->SetStringField(TEXT("message"),
						TEXT("output types (DeferredPass, PNG, JPG, EXR, WAV, CommandLineEncoder), ")
						TEXT("anti-aliasing, burn-ins and console-variable overrides all live here"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("render_status"))
				{
					UMoviePipelineQueueSubsystem* Subsystem = QueueSubsystem();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					const bool bRendering = Subsystem != nullptr && Subsystem->IsRendering();
					// The executor exists from the moment `render` returns, but
					// it only reports rendering once PIE is up and the pipeline
					// has started, a few seconds later — and a short sequence
					// can be done before the first poll.
					const bool bExecutorActive = Subsystem != nullptr && Subsystem->GetActiveExecutor() != nullptr;
					Data->SetBoolField(TEXT("rendering"), bRendering);
					Data->SetBoolField(TEXT("executor_active"), bExecutorActive);
					if (Subsystem != nullptr)
					{
						Data->SetNumberField(TEXT("jobs"), Subsystem->GetQueue()->GetJobs().Num());
						TArray<TSharedPtr<FJsonValue>> Jobs;
						for (UMoviePipelineExecutorJob* Job : Subsystem->GetQueue()->GetJobs())
						{
							if (Job == nullptr)
							{
								continue;
							}
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("name"), Job->JobName);
							Item->SetStringField(TEXT("sequence"), Job->Sequence.ToString());
							Item->SetStringField(TEXT("status"), Job->GetStatusMessage());
							Item->SetNumberField(TEXT("progress"), Job->GetStatusProgress());
							Item->SetBoolField(TEXT("consumed"), Job->IsConsumed());
							Jobs.Add(MakeShared<FJsonValueObject>(Item));
						}
						Data->SetArrayField(TEXT("queue"), Jobs);
					}
					if (!bRendering)
					{
						Data->SetStringField(TEXT("message"),
							bExecutorActive
								? TEXT("the executor is up but not rendering yet (PIE is starting) or has just finished — ")
								  TEXT("poll again in a few seconds and check the output directory")
								: TEXT("nothing rendering — a finished job's files are under the output directory of ")
								  TEXT("its config's MoviePipelineOutputSetting or its graph's Global Output Settings"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_config"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/Cinematics/MRQ_Preview")))
					{
						return;
					}
					if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr, *Path) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateMoviePipelineConfig", "McpLink Create Render Config"));
					UPackage* Package = CreatePackage(*Path);
					UMoviePipelinePrimaryConfig* Config = NewObject<UMoviePipelinePrimaryConfig>(
						Package, FName(*FPackageName::GetShortName(Path)),
						RF_Public | RF_Standalone | RF_Transactional);
					// A config with no render pass and no output type produces
					// nothing at all, so seed the pair every render needs.
					Config->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass());
					FString PassName = TEXT("MoviePipelineDeferredPassBase");
					Body->TryGetStringField(TEXT("render_pass"), PassName);
					FString OutputName = TEXT("MoviePipelineImageSequenceOutput_PNG");
					Body->TryGetStringField(TEXT("output_type"), OutputName);
					TArray<FString> Missing;
					for (const FString& ClassName : {PassName, OutputName})
					{
						UClass* Class = ResolveClass(ClassName);
						if (Class != nullptr && Class->IsChildOf(UMoviePipelineSetting::StaticClass()))
						{
							Config->FindOrAddSettingByClass(Class);
						}
						else
						{
							Missing.Add(ClassName);
						}
					}
					FAssetRegistryModule::AssetCreated(Config);
					Package->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("config"), Config->GetPathName());
					TArray<TSharedPtr<FJsonValue>> Settings;
					for (UMoviePipelineSetting* Setting : Config->GetUserSettings())
					{
						if (Setting != nullptr)
						{
							Settings.Add(MakeShared<FJsonValueObject>(SettingToJson(Setting)));
						}
					}
					Data->SetArrayField(TEXT("settings"), Settings);
					if (!Missing.IsEmpty())
					{
						Data->SetStringField(TEXT("warning"),
							FString::Printf(
								TEXT("not a Movie Pipeline setting class: %s — see list_setting_classes"),
								*FString::Join(Missing, TEXT(", "))));
					}
					Data->SetStringField(TEXT("message"),
						TEXT("tune the settings with set_property on their paths (output directory, ")
						TEXT("resolution, frame range), then save and render"));
					Responder->Ok(Data);
					return;
				}

				// The graph operations, and a render or save given a graph,
				// have no legacy config to resolve.
				static const TSet<FString> GraphOperations = {
					TEXT("create_graph"), TEXT("list_graph_node_classes"), TEXT("graph_info"),
					TEXT("add_graph_node"), TEXT("remove_graph_node"), TEXT("connect_graph_nodes"),
					TEXT("disconnect_graph_nodes"), TEXT("set_graph_node_properties"),
				};
				const bool bGraphRenderOrSave =
					(Operation == TEXT("render") || Operation == TEXT("save")) && Body->HasField(TEXT("graph"));
				UMoviePipelinePrimaryConfig* Config = nullptr;
				if (!GraphOperations.Contains(Operation) && !bGraphRenderOrSave)
				{
					Config = ConfigOrError(Body, Responder);
					if (Config == nullptr)
					{
						return;
					}
				}

				if (Operation == TEXT("config_info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("config"), Config->GetPathName());
					TArray<TSharedPtr<FJsonValue>> Settings;
					for (UMoviePipelineSetting* Setting : Config->GetUserSettings())
					{
						if (Setting != nullptr)
						{
							Settings.Add(MakeShared<FJsonValueObject>(SettingToJson(Setting)));
						}
					}
					Data->SetArrayField(TEXT("settings"), Settings);
					if (UMoviePipelineOutputSetting* Output = Cast<UMoviePipelineOutputSetting>(
							Config->FindSettingByClass(UMoviePipelineOutputSetting::StaticClass())))
					{
						Data->SetStringField(TEXT("output_directory"), Output->OutputDirectory.Path);
						Data->SetStringField(TEXT("file_name_format"), Output->FileNameFormat);
						Data->SetArrayField(TEXT("resolution"),
							{MakeShared<FJsonValueNumber>(Output->OutputResolution.X),
								MakeShared<FJsonValueNumber>(Output->OutputResolution.Y)});
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					UObject* Asset = Config;
					if (Asset == nullptr)
					{
						Asset = GraphOrError(Body, Responder);
						if (Asset == nullptr) { return; }
					}
					FString Filename, Error;
					if (!SaveAsset(Asset, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(Config != nullptr ? TEXT("config") : TEXT("graph"), Asset->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_setting") || Operation == TEXT("remove_setting"))
				{
					FString ClassName;
					if (!RequireString(Body, TEXT("class"), ClassName, Responder,
							TEXT("a setting class from list_setting_classes")))
					{
						return;
					}
					UClass* Class = ResolveClass(ClassName);
					if (Class == nullptr || !Class->IsChildOf(UMoviePipelineSetting::StaticClass())
						|| Class->HasAnyClassFlags(CLASS_Abstract))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_class"),
							FString::Printf(
								TEXT("'%s' is not a concrete UMoviePipelineSetting — see ")
								TEXT("list_setting_classes"),
								*ClassName));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditMoviePipelineConfig", "McpLink Edit Render Config"));
					Config->Modify();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					if (Operation == TEXT("add_setting"))
					{
						UMoviePipelineSetting* Setting = Config->FindOrAddSettingByClass(Class);
						if (Setting == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_failed"),
								FString::Printf(TEXT("the config refused a %s"), *ClassName));
							return;
						}
						Data->SetObjectField(TEXT("setting"), SettingToJson(Setting));
					}
					else
					{
						UMoviePipelineSetting* Setting = Config->FindSettingByClass(Class);
						if (Setting == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("setting_not_found"),
								FString::Printf(TEXT("this config has no %s"), *ClassName));
							return;
						}
						Config->RemoveSetting(Setting);
						Data->SetStringField(TEXT("removed"), ClassName);
					}
					Responder->Ok(Data);
					return;
				}

				// ------------------------------------------- Movie Render Graph
				if (Operation == TEXT("create_graph"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("a package path for the new graph, e.g. /Game/Cinematics/MRG_Preview")))
					{
						return;
					}
					const FString AssetName = FPackageName::GetShortName(Path);
					if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}
					// Seeded from the engine's default graph, as the New Movie
					// Render Graph asset is, so it renders as-is; an empty graph
					// has only its Input and Output nodes.
					UMovieGraphConfig* Template = BoolOr(Body, TEXT("from_default"), true)
						? LoadObject<UMovieGraphConfig>(nullptr, DefaultGraphPath)
						: nullptr;
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateMovieGraph", "McpLink Create Movie Render Graph"));
					UPackage* Package = CreatePackage(*Path);
					UMovieGraphConfig* Graph = Template != nullptr
						? DuplicateObject<UMovieGraphConfig>(Template, Package, FName(*AssetName))
						: NewObject<UMovieGraphConfig>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
					if (Graph == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("create_failed"),
							TEXT("the graph could not be created"));
						return;
					}
					Graph->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
					FAssetRegistryModule::AssetCreated(Graph);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = GraphToJson(*Graph, false);
					Data->SetBoolField(TEXT("from_default"), Template != nullptr);
					Data->SetStringField(TEXT("message"),
						TEXT("created in memory — movie_render save writes it to disk; render with graph=<path>"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_graph_node_classes"))
				{
					FString Filter;
					Body->TryGetStringField(TEXT("name_contains"), Filter);
					TArray<TSharedPtr<FJsonValue>> Classes;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						UClass* Class = *It;
						if (!Class->IsChildOf(UMovieGraphNode::StaticClass())
							|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
						{
							continue;
						}
						const UMovieGraphNode* Default = Class->GetDefaultObject<UMovieGraphNode>();
						if (Default == nullptr || !Default->CanBeAddedByUser())
						{
							continue;
						}
						const FString Title = Default->GetNodeTitle().ToString();
						const FString Category = Default->GetMenuCategory().ToString();
						if (!Filter.IsEmpty() && !Class->GetName().Contains(Filter) && !Title.Contains(Filter)
							&& !Category.Contains(Filter))
						{
							continue;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("class"), Class->GetName());
						Item->SetStringField(TEXT("title"), Title);
						Item->SetStringField(TEXT("category"), Category);
						Classes.Add(MakeShared<FJsonValueObject>(Item));
					}
					Classes.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
					{
						return A->AsObject()->GetStringField(TEXT("class")) < B->AsObject()->GetStringField(TEXT("class"));
					});
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("classes"), Classes);
					Data->SetNumberField(TEXT("count"), Classes.Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("graph_info"))
				{
					UMovieGraphConfig* Graph = GraphOrError(Body, Responder);
					if (Graph == nullptr) { return; }
					Responder->Ok(GraphToJson(*Graph, BoolOr(Body, TEXT("include_properties"), true)));
					return;
				}

				if (Operation == TEXT("add_graph_node"))
				{
					UMovieGraphConfig* Graph = GraphOrError(Body, Responder);
					if (Graph == nullptr) { return; }
					FString ClassSpec;
					if (!RequireString(Body, TEXT("class"), ClassSpec, Responder,
							TEXT("a node class from list_graph_node_classes, e.g. DeferredRenderPass or MovieGraphGlobalOutputSettingNode")))
					{
						return;
					}
					UClass* Class = ResolveGraphNodeClass(ClassSpec);
					if (Class == nullptr || Class->HasAnyClassFlags(CLASS_Abstract)
						|| !Class->GetDefaultObject<UMovieGraphNode>()->CanBeAddedByUser())
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_class_not_found"),
							FString::Printf(TEXT("'%s' is not an addable Movie Render Graph node class — see list_graph_node_classes"),
								*ClassSpec));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddMovieGraphNode", "McpLink Add Movie Render Graph Node"));
					Graph->Modify();
					UMovieGraphNode* Node = Graph->CreateNodeByClass(Class);
					if (Node == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("add_node_failed"),
							TEXT("the graph refused the node"));
						return;
					}
					Node->SetNodePosX(IntOr(Body, TEXT("x"), 0));
					Node->SetNodePosY(IntOr(Body, TEXT("y"), 0));
					Graph->MarkPackageDirty();
					Responder->Ok(GraphNodeToJson(*Graph, Node, true));
					return;
				}

				if (Operation == TEXT("remove_graph_node"))
				{
					UMovieGraphConfig* Graph = GraphOrError(Body, Responder);
					if (Graph == nullptr) { return; }
					UMovieGraphNode* Node = GraphNodeOrError(*Graph, Body, TEXT("node"), Responder);
					if (Node == nullptr) { return; }
					if (Node == Graph->GetInputNode() || Node == Graph->GetOutputNode())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("cannot_remove"),
							TEXT("the graph's Input and Output nodes cannot be removed"));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "RemoveMovieGraphNode", "McpLink Remove Movie Render Graph Node"));
					Graph->Modify();
					const FString Removed = Node->GetName();
					Graph->RemoveNode(Node);
					Graph->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = GraphToJson(*Graph, false);
					Data->SetStringField(TEXT("removed"), Removed);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("connect_graph_nodes") || Operation == TEXT("disconnect_graph_nodes"))
				{
					UMovieGraphConfig* Graph = GraphOrError(Body, Responder);
					if (Graph == nullptr) { return; }
					UMovieGraphNode* From = GraphNodeOrError(*Graph, Body, TEXT("from"), Responder);
					if (From == nullptr) { return; }
					UMovieGraphNode* To = GraphNodeOrError(*Graph, Body, TEXT("to"), Responder);
					if (To == nullptr) { return; }
					// Pins default to the first of each side, which on a
					// single-branch node is the only one.
					FString FromPin, ToPin;
					Body->TryGetStringField(TEXT("from_pin"), FromPin);
					Body->TryGetStringField(TEXT("to_pin"), ToPin);
					if (FromPin.IsEmpty() && !From->GetOutputPins().IsEmpty() && From->GetOutputPins()[0] != nullptr)
					{
						FromPin = From->GetOutputPins()[0]->Properties.Label.ToString();
					}
					if (ToPin.IsEmpty() && !To->GetInputPins().IsEmpty() && To->GetInputPins()[0] != nullptr)
					{
						ToPin = To->GetInputPins()[0]->Properties.Label.ToString();
					}
					const auto PinNames = [](const TArray<UMovieGraphPin*>& Pins)
					{
						TArray<FString> Names;
						for (const UMovieGraphPin* Pin : Pins)
						{
							if (Pin != nullptr) { Names.Add(Pin->Properties.Label.ToString()); }
						}
						return FString::Join(Names, TEXT(", "));
					};
					UMovieGraphPin* FromPinObject = From->GetOutputPin(FName(*FromPin));
					UMovieGraphPin* ToPinObject = To->GetInputPin(FName(*ToPin));
					if (FromPinObject == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("pin_not_found"),
							FString::Printf(TEXT("'%s' has no output pin '%s' — it has: %s"),
								*From->GetName(), *FromPin, *PinNames(From->GetOutputPins())));
						return;
					}
					if (ToPinObject == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("pin_not_found"),
							FString::Printf(TEXT("'%s' has no input pin '%s' — it has: %s"),
								*To->GetName(), *ToPin, *PinNames(To->GetInputPins())));
						return;
					}
					const bool bConnect = Operation == TEXT("connect_graph_nodes");
					if (bConnect)
					{
						const FPinConnectionResponse Check = FromPinObject->CanCreateConnection_PinConnectionResponse(ToPinObject);
						if (Check.Response == CONNECT_RESPONSE_DISALLOW)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("connection_rejected"),
								FString::Printf(TEXT("%s -> %s: %s"), *PinRef(FromPinObject), *PinRef(ToPinObject),
									*Check.Message.ToString()));
							return;
						}
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "ConnectMovieGraph", "McpLink Connect Movie Render Graph"));
					Graph->Modify();
					// AddLabeledEdge's bool says whether an existing edge into
					// the target pin was replaced, not whether it succeeded;
					// the pins' own connection list is the outcome.
					bool bReplaced = false;
					if (bConnect)
					{
						bReplaced = Graph->AddLabeledEdge(From, FName(*FromPin), To, FName(*ToPin));
					}
					else
					{
						Graph->RemoveLabeledEdge(From, FName(*FromPin), To, FName(*ToPin));
					}
					const bool bNowConnected = FromPinObject->GetAllConnectedPins().Contains(ToPinObject);
					if (bNowConnected != bConnect)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest,
							bConnect ? TEXT("connection_failed") : TEXT("disconnect_failed"),
							FString::Printf(TEXT("the graph refused %s %s -> %s"),
								bConnect ? TEXT("connecting") : TEXT("disconnecting"),
								*PinRef(FromPinObject), *PinRef(ToPinObject)));
						return;
					}
					Graph->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("connected"), bConnect);
					Data->SetStringField(TEXT("from"), PinRef(FromPinObject));
					Data->SetStringField(TEXT("to"), PinRef(ToPinObject));
					if (bConnect)
					{
						Data->SetBoolField(TEXT("replaced_existing"), bReplaced);
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_graph_node_properties"))
				{
					UMovieGraphConfig* Graph = GraphOrError(Body, Responder);
					if (Graph == nullptr) { return; }
					UMovieGraphNode* Node = GraphNodeOrError(*Graph, Body, TEXT("node"), Responder);
					if (Node == nullptr) { return; }
					const TSharedPtr<FJsonObject>* Properties = nullptr;
					if (!Body->TryGetObjectField(TEXT("properties"), Properties) || !Properties->IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'properties' is required — values keyed by name, e.g. {\"OutputResolution\": {\"ProfileName\": \"1080p (FHD)\"}}; graph_info lists each node's properties"));
						return;
					}
					// Every name is checked before anything is written, so one
					// typo does not leave the node half-changed.
					for (const auto& Pair : (*Properties)->Values)
					{
						const FString Name(Pair.Key.ToView());
						if (Node->GetClass()->FindPropertyByName(FName(*Name)) == nullptr
							&& Node->FindOverridePropertyForDynamicProperty(FName(*Name)) == nullptr)
						{
							TArray<FString> Known;
							for (const FMovieGraphPropertyInfo& Info : Node->GetOverrideablePropertyInfo())
							{
								Known.Add(Info.Name.ToString());
							}
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("property_not_found"),
								FString::Printf(TEXT("%s has no property '%s' — it has: %s"),
									*Node->GetClass()->GetName(), *Name, *FString::Join(Known, TEXT(", "))));
							return;
						}
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetMovieGraphNodeProperties", "McpLink Set Movie Render Graph Node Properties"));
					Node->Modify();
					TArray<FString> Applied;
					for (const auto& Pair : (*Properties)->Values)
					{
						const FString Name(Pair.Key.ToView());
						if (FProperty* Property = Node->GetClass()->FindPropertyByName(FName(*Name)))
						{
							if (!Pair.Value.IsValid() || !FJsonObjectConverter::JsonValueToUProperty(
									Pair.Value, Property, Property->ContainerPtrToValuePtr<void>(Node), 0, 0))
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_value"),
									FString::Printf(TEXT("'%s' would not take that value — it is a %s"),
										*Name, *Property->GetCPPType()));
								return;
							}
							// Setting a value without its override flag changes
							// nothing at render time; the flag is the checkbox.
							const FString FlagName = TEXT("bOverride_") + Name;
							if (FBoolProperty* Flag = FindFProperty<FBoolProperty>(Node->GetClass(), *FlagName))
							{
								Flag->SetPropertyValue_InContainer(Node, true);
							}
							Applied.Add(Name);
							continue;
						}
						if (Node->FindOverridePropertyForDynamicProperty(FName(*Name)) != nullptr)
						{
							FString Text;
							if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Text))
							{
								TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
								FJsonSerializer::Serialize(Pair.Value.ToSharedRef(), FString(), Writer);
								Text.TrimQuotesInline();
							}
							if (!Node->SetDynamicPropertyValue(FName(*Name), Text))
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_value"),
									FString::Printf(TEXT("dynamic property '%s' would not take '%s'"), *Name, *Text));
								return;
							}
							Node->SetDynamicPropertyOverridden(FName(*Name), true);
							Applied.Add(Name);
							continue;
						}
						TArray<FString> Known;
						for (const FMovieGraphPropertyInfo& Info : Node->GetOverrideablePropertyInfo())
						{
							Known.Add(Info.Name.ToString());
						}
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("property_not_found"),
							FString::Printf(TEXT("%s has no property '%s' — it has: %s"),
								*Node->GetClass()->GetName(), *Name, *FString::Join(Known, TEXT(", "))));
						return;
					}
					Graph->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = GraphNodeToJson(*Graph, Node, true);
					TArray<TSharedPtr<FJsonValue>> AppliedJson;
					for (const FString& Name : Applied)
					{
						AppliedJson.Add(MakeShared<FJsonValueString>(Name));
					}
					Data->SetArrayField(TEXT("applied"), AppliedJson);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("render"))
				{
					// The PIE executor renders through a Play In Editor session,
					// so there has to be something to render into.
					if (!FApp::CanEverRender())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_rendering"),
							TEXT("Movie Render Queue renders through PIE, which an editor without ")
							TEXT("rendering (-nullrhi) cannot start — run a windowed editor"));
						return;
					}
					UMoviePipelineQueueSubsystem* Subsystem = QueueSubsystem();
					if (Subsystem == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_subsystem"),
							TEXT("the Movie Pipeline queue subsystem is unavailable"));
						return;
					}
					if (Subsystem->IsRendering())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_rendering"),
							TEXT("a render is already running — poll render_status"));
						return;
					}

					FString SequencePath;
					if (!RequireString(Body, TEXT("sequence"), SequencePath, Responder,
							TEXT("the Level Sequence to render")))
					{
						return;
					}
					ULevelSequence* Sequence = Cast<ULevelSequence>(ResolveAsset(SequencePath));
					if (Sequence == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("sequence_not_found"),
							FString::Printf(TEXT("no Level Sequence at '%s'"), *SequencePath));
						return;
					}

					FString MapPath;
					Body->TryGetStringField(TEXT("map"), MapPath);
					if (MapPath.IsEmpty())
					{
						UWorld* World =
							GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
						if (World == nullptr || World->GetOutermost() == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'map' is required — there is no open level to default to"));
							return;
						}
						MapPath = World->GetOutermost()->GetName();
					}

					UMoviePipelineQueue* Queue = Subsystem->GetQueue();
					if (BoolOr(Body, TEXT("clear_queue"), true))
					{
						for (UMoviePipelineExecutorJob* Existing : TArray<UMoviePipelineExecutorJob*>(
								 Queue->GetJobs()))
						{
							Queue->DeleteJob(Existing);
						}
					}
					UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(
						UMoviePipelineExecutorJob::StaticClass());
					if (Job == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("job_failed"),
							TEXT("the queue refused a new job"));
						return;
					}
					FString JobName;
					Body->TryGetStringField(TEXT("name"), JobName);
					Job->JobName = JobName.IsEmpty() ? Sequence->GetName() : JobName;
					Job->Sequence = FSoftObjectPath(Sequence);
					Job->Map = FSoftObjectPath(MapPath);
					// A job renders either a legacy config or a graph preset.
					UMovieGraphConfig* Graph = nullptr;
					if (Config != nullptr)
					{
						Job->SetConfiguration(Config);
					}
					else
					{
						Graph = GraphOrError(Body, Responder);
						if (Graph == nullptr)
						{
							Queue->DeleteJob(Job);
							return;
						}
						Job->SetGraphPreset(Graph);
					}

					UMoviePipelineExecutorBase* Executor = Subsystem->RenderQueueWithExecutor(
						UMoviePipelinePIEExecutor::StaticClass());
					if (Executor == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("render_failed"),
							TEXT("the queue subsystem returned no executor"));
						return;
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("job"), Job->JobName);
					Data->SetStringField(TEXT("sequence"), Job->Sequence.ToString());
					Data->SetStringField(TEXT("map"), Job->Map.ToString());
					if (Config != nullptr)
					{
						Data->SetStringField(TEXT("config"), Config->GetPathName());
					}
					else if (Graph != nullptr)
					{
						Data->SetStringField(TEXT("graph"), Graph->GetPathName());
					}
					Data->SetBoolField(TEXT("started"), true);
					// Where the files will land, so the caller does not have to
					// go back through the config to find them.
					if (UMoviePipelineOutputSetting* Output = Config != nullptr
							? Cast<UMoviePipelineOutputSetting>(
								Config->FindSettingByClass(UMoviePipelineOutputSetting::StaticClass()))
							: nullptr)
					{
						Data->SetStringField(TEXT("output_directory"), Output->OutputDirectory.Path);
						Data->SetStringField(TEXT("file_name_format"), Output->FileNameFormat);
					}
					else if (Graph != nullptr)
					{
						Data->SetStringField(TEXT("output_note"),
							TEXT("the output directory is the Global Output Settings node's OutputDirectory — graph_info shows it"));
					}
					Data->SetStringField(TEXT("message"),
						TEXT("rendering through PIE — poll render_status until `rendering` is false, ")
						TEXT("then look in output_directory ({project_dir} and the other tokens are ")
						TEXT("expanded at render time)"));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_setting_classes, create_config, ")
						TEXT("config_info, add_setting, remove_setting, create_graph, list_graph_node_classes, ")
						TEXT("graph_info, add_graph_node, remove_graph_node, connect_graph_nodes, ")
						TEXT("disconnect_graph_nodes, set_graph_node_properties, save, render, or render_status"),
						*Operation));
			});
	}
}
