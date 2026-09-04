// MetaSound authoring: build a MetaSound Source or Patch graph node by node.
//
// A MetaSound is a document, not an EdGraph, and the engine's builder API
// (UMetaSoundBuilderSubsystem / UMetaSoundBuilderBase) is the only supported
// way to write one — it keeps the document, the frontend registry and the
// editor graph in step. Every operation here is that API; the route's job is
// turning node and vertex GUIDs into strings an agent can hold onto.
//
// Nodes are addressed by their GUID (from add_node or info) and vertices by
// name, which is how the MetaSound editor labels them.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Interfaces/MetasoundOutputFormatInterfaces.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "MetasoundBuilderBase.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendDocumentBuilder.h"
#include "MetasoundFrontendLiteral.h"
#include "MetasoundFrontendQuery.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundSource.h"
#include "Misc/PackageName.h"

namespace McpLink
{
	namespace MetaSounds
	{
		const TCHAR* ResultName(EMetaSoundBuilderResult Result)
		{
			return Result == EMetaSoundBuilderResult::Succeeded ? TEXT("succeeded") : TEXT("failed");
		}

		bool Failed(EMetaSoundBuilderResult Result)
		{
			return Result != EMetaSoundBuilderResult::Succeeded;
		}

		/// "Namespace.Name" or "Namespace.Name.Variant" -> a class name.
		bool ParseClassName(const FString& Spec, FMetasoundFrontendClassName& Out)
		{
			TArray<FString> Parts;
			Spec.ParseIntoArray(Parts, TEXT("."), /*InCullEmpty*/ true);
			if (Parts.Num() == 2)
			{
				Out = FMetasoundFrontendClassName(FName(*Parts[0]), FName(*Parts[1]));
				return true;
			}
			if (Parts.Num() == 3)
			{
				Out = FMetasoundFrontendClassName(
					FName(*Parts[0]), FName(*Parts[1]), FName(*Parts[2]));
				return true;
			}
			return false;
		}

		/// The builder for an existing MetaSound asset, or nullptr after
		/// responding with why there is none.
		UMetaSoundBuilderBase* BuilderOrError(
			const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder,
			TScriptInterface<IMetaSoundDocumentInterface>& OutMetaSound)
		{
			FString Path;
			if (!RequireString(Body, TEXT("metasound"), Path, Responder,
					TEXT("a MetaSound asset path, e.g. /Game/Audio/MS_Beep")))
			{
				return nullptr;
			}
			UObject* Asset = ResolveAsset(Path);
			if (Asset == nullptr || !Asset->GetClass()->ImplementsInterface(
									   UMetaSoundDocumentInterface::StaticClass()))
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("metasound_not_found"),
					FString::Printf(TEXT("no MetaSound asset at '%s'"), *Path));
				return nullptr;
			}
			UMetaSoundEditorSubsystem* Editor = GEditor != nullptr
				? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()
				: nullptr;
			if (Editor == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_subsystem"),
					TEXT("the MetaSound editor subsystem is unavailable"));
				return nullptr;
			}
			OutMetaSound = TScriptInterface<IMetaSoundDocumentInterface>(Asset);
			EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
			UMetaSoundBuilderBase* Builder = Editor->FindOrBeginBuilding(OutMetaSound, Result);
			if (Builder == nullptr || Failed(Result))
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_builder"),
					FString::Printf(TEXT("could not begin building '%s'"), *Path));
				return nullptr;
			}
			return Builder;
		}

		FMetaSoundNodeHandle NodeHandleFrom(const FString& Spec)
		{
			FGuid Id;
			FGuid::Parse(Spec, Id);
			return FMetaSoundNodeHandle(Id);
		}

		/// One node as JSON: its id, class and vertex names.
		TSharedRef<FJsonObject> NodeToJson(
			const FMetasoundFrontendNode& Node, const FMetasoundFrontendDocument& Document)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("id"), Node.GetID().ToString());
			Object->SetStringField(TEXT("name"), Node.Name.ToString());
			for (const FMetasoundFrontendClass& Class : Document.Dependencies)
			{
				if (Class.ID == Node.ClassID)
				{
					Object->SetStringField(TEXT("class"), Class.Metadata.GetClassName().ToString());
					break;
				}
			}
			const auto Vertices = [](const TArray<FMetasoundFrontendVertex>& In)
			{
				TArray<TSharedPtr<FJsonValue>> Out;
				for (const FMetasoundFrontendVertex& Vertex : In)
				{
					const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), Vertex.Name.ToString());
					Item->SetStringField(TEXT("type"), Vertex.TypeName.ToString());
					Out.Add(MakeShared<FJsonValueObject>(Item));
				}
				return Out;
			};
			Object->SetArrayField(TEXT("inputs"), Vertices(Node.Interface.Inputs));
			Object->SetArrayField(TEXT("outputs"), Vertices(Node.Interface.Outputs));
			return Object;
		}

		/// JSON value -> the literal a graph input or node input default takes.
		bool ReadLiteral(const TSharedPtr<FJsonValue>& Value, FMetasoundFrontendLiteral& Out)
		{
			if (!Value.IsValid())
			{
				return false;
			}
			double Number = 0.0;
			bool Flag = false;
			FString Text;
			if (Value->Type == EJson::Boolean && Value->TryGetBool(Flag))
			{
				Out.Set(Flag);
				return true;
			}
			if (Value->Type == EJson::Number && Value->TryGetNumber(Number))
			{
				// A whole number could be either; float is the safer default
				// because every numeric MetaSound pin accepts it.
				Out.Set(static_cast<float>(Number));
				return true;
			}
			if (Value->Type == EJson::String && Value->TryGetString(Text))
			{
				if (UObject* Object = ResolveAsset(Text))
				{
					Out.Set(Object);
				}
				else
				{
					Out.Set(Text);
				}
				return true;
			}
			return false;
		}
	}

	using namespace MetaSounds;

	void RegisterMetaSoundRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/audio/metasound"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_node_classes"))
				{
					FString Contains;
					Body->TryGetStringField(TEXT("name_contains"), Contains);
					const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_results"), 100), 1, 2000);

					// ISearchEngine, not INodeClassRegistry::IterateRegistry —
					// the registry's own iteration is deprecated as unsafe to
					// walk from outside the frontend.
					using namespace Metasound::Frontend;
					TArray<FString> Names;
					for (const FMetaSoundClassInfo& Class : ISearchEngine::Get().FindAllClasses(
							 ISearchEngine::EResultVersion::Highest))
					{
						if (Class.ClassType != EMetasoundFrontendClassType::External)
						{
							continue;
						}
						const FString Name = Class.ClassName.ToString();
						if (Contains.IsEmpty() || Name.Contains(Contains))
						{
							Names.AddUnique(Name);
						}
					}
					Names.Sort();

					TArray<TSharedPtr<FJsonValue>> Classes;
					for (const FString& Name : Names)
					{
						if (Classes.Num() >= Max)
						{
							break;
						}
						Classes.Add(MakeShared<FJsonValueString>(Name));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Names.Num());
					Data->SetArrayField(TEXT("classes"), Classes);
					Data->SetStringField(TEXT("message"),
						TEXT("pass one of these as `class` to add_node — the form is Namespace.Name"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/Audio/MS_Beep")))
					{
						return;
					}
					if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr, *Path) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}
					UMetaSoundBuilderSubsystem* Subsystem = UMetaSoundBuilderSubsystem::Get();
					UMetaSoundEditorSubsystem* Editor = GEditor != nullptr
						? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>()
						: nullptr;
					if (Subsystem == nullptr || Editor == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_subsystem"),
							TEXT("the MetaSound builder subsystems are unavailable"));
						return;
					}

					FString Kind = TEXT("source");
					Body->TryGetStringField(TEXT("kind"), Kind);
					const FName BuilderName(*FString::Printf(
						TEXT("McpLink_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
					EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
					UMetaSoundBuilderBase* Builder = nullptr;

					if (Kind.Equals(TEXT("patch"), ESearchCase::IgnoreCase))
					{
						Builder = Subsystem->CreatePatchBuilder(BuilderName, Result);
					}
					else
					{
						FString FormatName = TEXT("Mono");
						Body->TryGetStringField(TEXT("output_format"), FormatName);
						const UEnum* FormatEnum = StaticEnum<EMetaSoundOutputAudioFormat>();
						const int64 FormatValue = FormatEnum != nullptr
							? FormatEnum->GetValueByNameString(FormatName)
							: INDEX_NONE;
						if (FormatValue == INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_format"),
								TEXT("'output_format' must be Mono, Stereo, Quad, FiveDotOne or "
									 "SevenDotOne"));
							return;
						}
						FMetaSoundBuilderNodeOutputHandle OnPlay;
						FMetaSoundBuilderNodeInputHandle OnFinished;
						TArray<FMetaSoundBuilderNodeInputHandle> AudioOuts;
						Builder = Subsystem->CreateSourceBuilder(BuilderName, OnPlay, OnFinished,
							AudioOuts, Result,
							static_cast<EMetaSoundOutputAudioFormat>(FormatValue),
							BoolOr(Body, TEXT("one_shot"), true));
					}
					if (Builder == nullptr || Failed(Result))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("create_failed"),
							TEXT("the builder subsystem refused to create a MetaSound"));
						return;
					}

					FString Author;
					Body->TryGetStringField(TEXT("author"), Author);
					EMetaSoundBuilderResult BuildResult = EMetaSoundBuilderResult::Failed;
					TScriptInterface<IMetaSoundDocumentInterface> Built =
						Editor->BuildToAsset(Builder, Author, FPackageName::GetShortName(Path),
							FPackageName::GetLongPackagePath(Path), BuildResult);
					if (Failed(BuildResult) || Built.GetObject() == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("build_failed"),
							FString::Printf(TEXT("could not build a MetaSound asset at '%s'"), *Path));
						return;
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("metasound"), Built.GetObject()->GetPathName());
					Data->SetStringField(TEXT("class"), Built.GetObject()->GetClass()->GetName());
					Data->SetStringField(TEXT("message"),
						TEXT("created with its default interface wired up — add nodes, then save"));
					Responder->Ok(Data);
					return;
				}

				// ---- everything below targets an existing MetaSound asset ---
				TScriptInterface<IMetaSoundDocumentInterface> MetaSound;
				UMetaSoundBuilderBase* Builder = BuilderOrError(Body, Responder, MetaSound);
				if (Builder == nullptr)
				{
					return;
				}
				EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;

				if (Operation == TEXT("info"))
				{
					const FMetaSoundFrontendDocumentBuilder& Document = Builder->GetConstBuilder();
					const FMetasoundFrontendDocument& Doc = Document.GetConstDocumentChecked();
					const FMetasoundFrontendGraph& Graph = Document.FindConstBuildGraphChecked();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("metasound"), MetaSound.GetObject()->GetPathName());
					Data->SetStringField(TEXT("class"), MetaSound.GetObject()->GetClass()->GetName());

					TArray<TSharedPtr<FJsonValue>> Nodes;
					const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_nodes"), 200), 1, 2000);
					for (const FMetasoundFrontendNode& Node : Graph.Nodes)
					{
						if (Nodes.Num() >= Max)
						{
							break;
						}
						Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Node, Doc)));
					}
					Data->SetNumberField(TEXT("node_count"), Graph.Nodes.Num());
					Data->SetArrayField(TEXT("nodes"), Nodes);

					// Graph inputs and outputs are different subclasses of
					// FMetasoundFrontendClassVertex, so this has to be generic.
					const auto VertexNames = [](const auto& In)
					{
						TArray<TSharedPtr<FJsonValue>> Out;
						for (const auto& Vertex : In)
						{
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("name"), Vertex.Name.ToString());
							Item->SetStringField(TEXT("type"), Vertex.TypeName.ToString());
							Out.Add(MakeShared<FJsonValueObject>(Item));
						}
						return Out;
					};
					Data->SetArrayField(TEXT("graph_inputs"),
						VertexNames(Doc.RootGraph.GetDefaultInterface().Inputs));
					Data->SetArrayField(TEXT("graph_outputs"),
						VertexNames(Doc.RootGraph.GetDefaultInterface().Outputs));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_node"))
				{
					FString ClassSpec;
					if (!RequireString(Body, TEXT("class"), ClassSpec, Responder,
							TEXT("a node class, e.g. UE.Sine — see list_node_classes")))
					{
						return;
					}
					FMetasoundFrontendClassName ClassName;
					if (!ParseClassName(ClassSpec, ClassName))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_class"),
							FString::Printf(
								TEXT("'%s' is not a MetaSound class name — the form is Namespace.Name ")
								TEXT("(or Namespace.Name.Variant)"),
								*ClassSpec));
						return;
					}
					const FMetaSoundNodeHandle Node = Builder->AddNodeByClassName(
						ClassName, Result, IntOr(Body, TEXT("major_version"), 1));
					if (Failed(Result) || !Node.IsSet())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_failed"),
							FString::Printf(
								TEXT("no registered MetaSound node class '%s' at that major version — ")
								TEXT("see list_node_classes"),
								*ClassSpec));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("node"), Node.NodeID.ToString());
					Data->SetStringField(TEXT("class"), ClassSpec);
					TArray<TSharedPtr<FJsonValue>> Inputs, Outputs;
					for (const FMetaSoundBuilderNodeInputHandle& Handle :
						Builder->FindNodeInputs(Node, Result))
					{
						FName Name, Type;
						EMetaSoundBuilderResult Ignored = EMetaSoundBuilderResult::Failed;
						Builder->GetNodeInputData(Handle, Name, Type, Ignored);
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Name.ToString());
						Item->SetStringField(TEXT("type"), Type.ToString());
						Inputs.Add(MakeShared<FJsonValueObject>(Item));
					}
					for (const FMetaSoundBuilderNodeOutputHandle& Handle :
						Builder->FindNodeOutputs(Node, Result))
					{
						FName Name, Type;
						EMetaSoundBuilderResult Ignored = EMetaSoundBuilderResult::Failed;
						Builder->GetNodeOutputData(Handle, Name, Type, Ignored);
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Name.ToString());
						Item->SetStringField(TEXT("type"), Type.ToString());
						Outputs.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetArrayField(TEXT("inputs"), Inputs);
					Data->SetArrayField(TEXT("outputs"), Outputs);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_node"))
				{
					FString NodeSpec;
					if (!RequireString(Body, TEXT("node"), NodeSpec, Responder,
							TEXT("a node id from add_node or info")))
					{
						return;
					}
					Builder->RemoveNode(NodeHandleFrom(NodeSpec), Result);
					if (Failed(Result))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"),
							FString::Printf(TEXT("no node '%s' in this MetaSound"), *NodeSpec));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), NodeSpec);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("connect"))
				{
					FString FromNode, FromOutput, ToNode, ToInput;
					if (!RequireString(Body, TEXT("from_node"), FromNode, Responder,
							TEXT("the source node id"))
						|| !RequireString(Body, TEXT("from_output"), FromOutput, Responder,
							TEXT("the source node's output name"))
						|| !RequireString(Body, TEXT("to_node"), ToNode, Responder,
							TEXT("the destination node id"))
						|| !RequireString(Body, TEXT("to_input"), ToInput, Responder,
							TEXT("the destination node's input name")))
					{
						return;
					}
					EMetaSoundBuilderResult OutputResult = EMetaSoundBuilderResult::Failed;
					EMetaSoundBuilderResult InputResult = EMetaSoundBuilderResult::Failed;
					const FMetaSoundBuilderNodeOutputHandle Output = Builder->FindNodeOutputByName(
						NodeHandleFrom(FromNode), FName(*FromOutput), OutputResult);
					const FMetaSoundBuilderNodeInputHandle Input = Builder->FindNodeInputByName(
						NodeHandleFrom(ToNode), FName(*ToInput), InputResult);
					if (Failed(OutputResult) || Failed(InputResult))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("vertex_not_found"),
							FString::Printf(
								TEXT("could not find %s — check the names against info"),
								Failed(OutputResult) ? *FString::Printf(TEXT("output '%s'"), *FromOutput)
													 : *FString::Printf(TEXT("input '%s'"), *ToInput)));
						return;
					}
					Builder->ConnectNodes(Output, Input, Result);
					if (Failed(Result))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("connect_failed"),
							TEXT("the builder refused the connection — the data types probably differ"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("from"),
						FString::Printf(TEXT("%s.%s"), *FromNode, *FromOutput));
					Data->SetStringField(TEXT("to"), FString::Printf(TEXT("%s.%s"), *ToNode, *ToInput));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("connect_to_graph_output")
					|| Operation == TEXT("connect_from_graph_input"))
				{
					const bool bToOutput = Operation == TEXT("connect_to_graph_output");
					FString NodeSpec, VertexName, GraphVertex;
					if (!RequireString(Body, TEXT("node"), NodeSpec, Responder, TEXT("the node id"))
						|| !RequireString(Body, bToOutput ? TEXT("output") : TEXT("input"), VertexName,
							Responder, TEXT("the node's vertex name"))
						|| !RequireString(Body, TEXT("graph_vertex"), GraphVertex, Responder,
							TEXT("the graph input/output name, e.g. \"Out Mono\"")))
					{
						return;
					}
					if (bToOutput)
					{
						Builder->ConnectNodeToGraphOutput(NodeHandleFrom(NodeSpec), FName(*VertexName),
							FName(*GraphVertex), Result);
					}
					else
					{
						EMetaSoundBuilderResult InputResult = EMetaSoundBuilderResult::Failed;
						const FMetaSoundBuilderNodeInputHandle Input = Builder->FindNodeInputByName(
							NodeHandleFrom(NodeSpec), FName(*VertexName), InputResult);
						if (Failed(InputResult))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("vertex_not_found"),
								FString::Printf(TEXT("node has no input '%s'"), *VertexName));
							return;
						}
						Builder->ConnectNodeInputToGraphInput(FName(*GraphVertex), Input, Result);
					}
					if (Failed(Result))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("connect_failed"),
							FString::Printf(
								TEXT("could not connect to graph vertex '%s' — check info for its name ")
								TEXT("and type"),
								*GraphVertex));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("node"), NodeSpec);
					Data->SetStringField(TEXT("graph_vertex"), GraphVertex);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_graph_input") || Operation == TEXT("add_graph_output"))
				{
					FString Name, DataType;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the vertex name"))
						|| !RequireString(Body, TEXT("data_type"), DataType, Responder,
							TEXT("the MetaSound data type, e.g. Float, Audio, Trigger")))
					{
						return;
					}
					FMetasoundFrontendLiteral Default;
					ReadLiteral(Body->TryGetField(TEXT("default")), Default);
					if (Operation == TEXT("add_graph_input"))
					{
						Builder->AddGraphInputNode(FName(*Name), FName(*DataType), Default, Result,
							BoolOr(Body, TEXT("constructor"), false));
					}
					else
					{
						Builder->AddGraphOutputNode(FName(*Name), FName(*DataType), Default, Result,
							BoolOr(Body, TEXT("constructor"), false));
					}
					if (Failed(Result))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_failed"),
							FString::Printf(
								TEXT("could not add '%s' as a %s — the name may be taken or the data ")
								TEXT("type unknown"),
								*Name, *DataType));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("name"), Name);
					Data->SetStringField(TEXT("data_type"), DataType);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_node_input_default"))
				{
					FString NodeSpec, InputName;
					if (!RequireString(Body, TEXT("node"), NodeSpec, Responder, TEXT("the node id"))
						|| !RequireString(Body, TEXT("input"), InputName, Responder,
							TEXT("the input's name")))
					{
						return;
					}
					FMetasoundFrontendLiteral Literal;
					if (!ReadLiteral(Body->TryGetField(TEXT("value")), Literal))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"),
							TEXT("'value' must be a number, a bool, a string, or an asset path"));
						return;
					}
					EMetaSoundBuilderResult InputResult = EMetaSoundBuilderResult::Failed;
					const FMetaSoundBuilderNodeInputHandle Input = Builder->FindNodeInputByName(
						NodeHandleFrom(NodeSpec), FName(*InputName), InputResult);
					if (Failed(InputResult))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("vertex_not_found"),
							FString::Printf(TEXT("node has no input '%s'"), *InputName));
						return;
					}
					Builder->SetNodeInputDefault(Input, Literal, Result);
					if (Failed(Result))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("set_failed"),
							FString::Printf(
								TEXT("the builder refused that value for '%s' — it is probably the wrong ")
								TEXT("data type, or the input is connected"),
								*InputName));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("node"), NodeSpec);
					Data->SetStringField(TEXT("input"), InputName);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					// Register first: an unregistered edit is not what the next
					// load of the asset would see.
					if (UMetaSoundEditorSubsystem* Editor =
							GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>())
					{
						Editor->RegisterGraphWithFrontend(*MetaSound.GetObject());
					}
					FString Filename, Error;
					if (!SaveAsset(MetaSound.GetObject(), Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("metasound"), MetaSound.GetObject()->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_node_classes, create, info, add_node, ")
						TEXT("remove_node, connect, connect_to_graph_output, connect_from_graph_input, ")
						TEXT("add_graph_input, add_graph_output, set_node_input_default, or save"),
						*Operation));
			});
	}
}
