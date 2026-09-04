// Environment Query System authoring.
//
// An EQS asset is a list of *options*, each one a generator (which produces
// candidate items — points on a grid, actors of a class) plus an ordered list
// of tests that score and filter them. At runtime that list, UEnvQuery::Options,
// is all there is. In the editor the same thing is drawn as a node graph, and
// the graph is the source of truth: UEnvironmentQueryGraph::UpdateAsset rebuilds
// Options from it, and opening an asset whose EdGraph is null creates an *empty*
// graph rather than reconstructing one from the options — so options written
// directly are discarded the next time a human opens the asset. Authoring EQS
// means authoring the graph.
//
// The graph classes live in the EnvironmentQueryEditor plugin and are plain
// UCLASS() with no API macro, so nothing in them can be linked against. None of
// it has to be. Their UClasses are in the reflection system like any other, so
// NewObject finds them by path; every entry point that matters — PostPlacedNewNode,
// AllocateDefaultPins, UpdateAsset, CreateDefaultNodesForGraph, TryCreateConnection
// — is a virtual override of an exported base in AIGraph, Engine or UnrealEd, so
// calling it through that base dispatches to the EQS implementation; and the node
// state that has to be set (ClassData, NodeInstance, SubNodes, bTestEnabled) is
// either on the exported UAIGraphNode or a UPROPERTY reachable by reflection.
//
// The one thing this depends on is the EnvironmentQueryEditor plugin being
// loaded. It is enabled by default, and McpLink.uplugin asks for it; if a
// project has turned it off, the class lookups fail and the route says so.

#include "AIGraph.h"
#include "AIGraphNode.h"
#include "AIGraphTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Editor.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace Eqs
	{
		const TCHAR* GraphClassPath = TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryGraph");
		const TCHAR* SchemaClassPath = TEXT("/Script/EnvironmentQueryEditor.EdGraphSchema_EnvironmentQuery");
		const TCHAR* RootNodeClassPath = TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryGraphNode_Root");
		const TCHAR* OptionNodeClassPath = TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryGraphNode_Option");
		const TCHAR* TestNodeClassPath = TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryGraphNode_Test");

		/// Editor-plugin classes by reflection, since none of them export a
		/// symbol to link against.
		UClass* EditorClass(const TCHAR* Path)
		{
			return FindObject<UClass>(nullptr, Path);
		}

		bool RequireEditorClasses(const TSharedRef<FMcpResponder>& Responder)
		{
			if (EditorClass(GraphClassPath) != nullptr && EditorClass(OptionNodeClassPath) != nullptr)
			{
				return true;
			}
			Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("eqs_editor_missing"),
				TEXT("the EnvironmentQueryEditor plugin is not loaded, so the graph classes an EQS "
					 "asset is authored through do not exist — enable it under Edit > Plugins "
					 "(it is on by default) and restart the editor"));
			return false;
		}

		UEnvQuery* ResolveQuery(const FString& Path)
		{
			return Cast<UEnvQuery>(ResolveAsset(Path));
		}

		UEnvQuery* QueryOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("query"), Path, Responder,
					TEXT("an Environment Query asset path, e.g. /Game/AI/EQS_FindCover")))
			{
				return nullptr;
			}
			UEnvQuery* Query = ResolveQuery(Path);
			if (Query == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("query_not_found"),
					FString::Printf(TEXT("no Environment Query at '%s'"), *Path));
			}
			return Query;
		}

		/// The graph as UAIGraph, which is the exported base every call below
		/// goes through.
		UAIGraph* QueryGraph(UEnvQuery* Query)
		{
			return Cast<UAIGraph>(Query->EdGraph);
		}

		UEdGraphNode* FindNodeOfClass(UEdGraph* Graph, const TCHAR* ClassPath)
		{
			UClass* Class = EditorClass(ClassPath);
			if (Class == nullptr)
			{
				return nullptr;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr && Node->IsA(Class))
				{
					return Node;
				}
			}
			return nullptr;
		}

		TArray<UAIGraphNode*> OptionNodes(UEdGraph* Graph)
		{
			TArray<UAIGraphNode*> Out;
			UClass* Class = EditorClass(OptionNodeClassPath);
			if (Class == nullptr)
			{
				return Out;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node != nullptr && Node->IsA(Class))
				{
					Out.Add(CastChecked<UAIGraphNode>(Node));
				}
			}
			// Options run in the order the root's pin links them, which the
			// editor keeps sorted by node X. Mirror that so an index here means
			// the same thing as the position a human sees.
			Out.Sort([](const UAIGraphNode& A, const UAIGraphNode& B) { return A.NodePosX < B.NodePosX; });
			return Out;
		}

		/// What FGraphNodeCreator does, for a class only known at runtime.
		UAIGraphNode* SpawnNode(UEdGraph* Graph, UClass* NodeClass, UClass* InstanceClass, int32 PosX)
		{
			UAIGraphNode* Node = NewObject<UAIGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
			Node->CreateNewGuid();
			Node->NodePosX = PosX;
			if (InstanceClass != nullptr)
			{
				// PostPlacedNewNode reads ClassData to build NodeInstance, so it
				// has to be set first — the same ordering AddSubNode relies on.
				Node->ClassData = FGraphNodeClassData(InstanceClass, FString());
			}
			Node->PostPlacedNewNode();
			Node->AllocateDefaultPins();
			return Node;
		}

		/// Every concrete, non-deprecated subclass of Base.
		void CollectClasses(UClass* Base, const FString& NameFilter, TArray<TSharedPtr<FJsonValue>>& Out)
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Class = *It;
				if (!Class->IsChildOf(Base) || Class == Base)
				{
					continue;
				}
				if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
				{
					continue;
				}
				const FString Name = Class->GetName();
				if (!NameFilter.IsEmpty() && !Name.Contains(NameFilter))
				{
					continue;
				}
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("class"), Class->GetPathName());
				Item->SetStringField(TEXT("name"), Name);
				Out.Add(MakeShared<FJsonValueObject>(Item));
			}
			Out.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
			{
				return A->AsObject()->GetStringField(TEXT("name"))
					< B->AsObject()->GetStringField(TEXT("name"));
			});
		}

		/// A class named on the wire: a full path, or a bare class name.
		UClass* FindInstanceClass(const FString& Name, UClass* Base)
		{
			if (UClass* Direct = FindObject<UClass>(nullptr, *Name))
			{
				return Direct->IsChildOf(Base) ? Direct : nullptr;
			}
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->IsChildOf(Base) && It->GetName() == Name)
				{
					return *It;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> TestToJson(UAIGraphNode* TestNode, int32 Index)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			UEnvQueryTest* Test = Cast<UEnvQueryTest>(TestNode->NodeInstance);
			Item->SetStringField(TEXT("test"), Test != nullptr ? Test->GetClass()->GetName() : FString());
			// The instance path is the handle for set_property: test options are
			// UPROPERTYs on the test object, exactly as the details panel edits.
			Item->SetStringField(TEXT("object_path"),
				Test != nullptr ? Test->GetPathName() : FString());
			bool bEnabled = true;
			if (const FBoolProperty* Property =
					FindFProperty<FBoolProperty>(TestNode->GetClass(), TEXT("bTestEnabled")))
			{
				bEnabled = Property->GetPropertyValue_InContainer(TestNode);
			}
			Item->SetBoolField(TEXT("enabled"), bEnabled);
			return Item;
		}

		TSharedRef<FJsonObject> OptionToJson(UAIGraphNode* OptionNode, int32 Index)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			UEnvQueryOption* Option = Cast<UEnvQueryOption>(OptionNode->NodeInstance);
			UEnvQueryGenerator* Generator = Option != nullptr ? Option->Generator : nullptr;
			Item->SetStringField(TEXT("generator"),
				Generator != nullptr ? Generator->GetClass()->GetName() : FString());
			Item->SetStringField(TEXT("object_path"),
				Generator != nullptr ? Generator->GetPathName() : FString());
			TArray<TSharedPtr<FJsonValue>> Tests;
			for (int32 TestIndex = 0; TestIndex < OptionNode->SubNodes.Num(); ++TestIndex)
			{
				if (UAIGraphNode* TestNode = OptionNode->SubNodes[TestIndex])
				{
					Tests.Add(MakeShared<FJsonValueObject>(TestToJson(TestNode, TestIndex)));
				}
			}
			Item->SetArrayField(TEXT("tests"), Tests);
			return Item;
		}

		void AddQueryFields(const TSharedRef<FJsonObject>& Data, UEnvQuery* Query)
		{
			Data->SetStringField(TEXT("query"), Query->GetPathName());
			TArray<TSharedPtr<FJsonValue>> Options;
			if (UEdGraph* Graph = Query->EdGraph)
			{
				const TArray<UAIGraphNode*> Nodes = OptionNodes(Graph);
				for (int32 Index = 0; Index < Nodes.Num(); ++Index)
				{
					Options.Add(MakeShared<FJsonValueObject>(OptionToJson(Nodes[Index], Index)));
				}
			}
			Data->SetArrayField(TEXT("options"), Options);
			// What the runtime will actually see, which is only what the last
			// compile produced.
			Data->SetNumberField(TEXT("compiled_options"), Query->GetOptions().Num());
		}

		/// Rebuild UEnvQuery::Options from the graph. UpdateAsset is a virtual
		/// override of UAIGraph's, so this reaches the EQS implementation
		/// without linking to it.
		void Compile(UEnvQuery* Query)
		{
			if (UAIGraph* Graph = QueryGraph(Query))
			{
				Graph->UpdateAsset(0);
			}
			Query->MarkPackageDirty();
		}

		UAIGraphNode* OptionAtOrError(UEnvQuery* Query, const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder)
		{
			UEdGraph* Graph = Query->EdGraph;
			if (Graph == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_graph"),
					TEXT("this query has no editor graph — create it through this route, or open "
						 "it in the editor once so the graph is built"));
				return nullptr;
			}
			const TArray<UAIGraphNode*> Nodes = OptionNodes(Graph);
			const int32 Index = IntOr(Body, TEXT("option"), -1);
			if (!Nodes.IsValidIndex(Index))
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_option"),
					FString::Printf(
						TEXT("'option' must be 0..%d — info lists the options in that order"),
						Nodes.Num() - 1));
				return nullptr;
			}
			return Nodes[Index];
		}
	}

	using namespace Eqs;

	void RegisterEqsRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/ai/eqs"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_classes"))
				{
					FString NameFilter;
					Body->TryGetStringField(TEXT("name_filter"), NameFilter);
					TArray<TSharedPtr<FJsonValue>> Generators;
					TArray<TSharedPtr<FJsonValue>> Tests;
					CollectClasses(UEnvQueryGenerator::StaticClass(), NameFilter, Generators);
					CollectClasses(UEnvQueryTest::StaticClass(), NameFilter, Tests);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("generators"), Generators);
					Data->SetArrayField(TEXT("tests"), Tests);
					Responder->Ok(Data);
					return;
				}

				if (!RequireEditorClasses(Responder))
				{
					return;
				}

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("an asset path, e.g. /Game/AI/EQS_FindCover")))
					{
						return;
					}
					FString Error;
					UObject* Asset = CreateAsset(Path, UEnvQuery::StaticClass(), nullptr, Error);
					if (Asset == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					UEnvQuery* Query = CastChecked<UEnvQuery>(Asset);

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateEqs", "McpLink Create Environment Query"));
					Query->Modify();
					UEdGraph* Graph = NewObject<UEdGraph>(
						Query, EditorClass(GraphClassPath), TEXT("EQSGraph"), RF_Transactional);
					Graph->Schema = EditorClass(SchemaClassPath);
					Query->EdGraph = Graph;
					// CreateDefaultNodesForGraph is the schema's own virtual, and
					// for EQS it places the Root node the options hang off.
					if (const UEdGraphSchema* Schema = Graph->GetSchema())
					{
						Schema->CreateDefaultNodesForGraph(*Graph);
					}
					if (UAIGraph* AiGraph = Cast<UAIGraph>(Graph))
					{
						AiGraph->OnCreated();
					}
					Compile(Query);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddQueryFields(Data, Query);
					Data->SetStringField(TEXT("message"),
						TEXT("created with an empty graph — add_option gives it a generator, then save"));
					Responder->Ok(Data);
					return;
				}

				UEnvQuery* Query = QueryOrError(Body, Responder);
				if (Query == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddQueryFields(Data, Query);
					Data->SetBoolField(TEXT("has_graph"), Query->EdGraph != nullptr);
					Data->SetStringField(TEXT("note"),
						TEXT("set_property on an option's or test's object_path edits it, exactly as "
							 "the details panel does; call compile afterwards"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("compile"))
				{
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CompileEqs", "McpLink Compile Environment Query"));
					Query->Modify();
					Compile(Query);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddQueryFields(Data, Query);
					Data->SetStringField(TEXT("message"),
						TEXT("options rebuilt from the graph — save to keep it"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Error;
					FString Filename;
					if (!SaveAsset(Query, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddQueryFields(Data, Query);
					Data->SetStringField(TEXT("file"), Filename);
					Data->SetStringField(TEXT("message"), TEXT("saved"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_option"))
				{
					FString GeneratorName;
					if (!RequireString(Body, TEXT("generator"), GeneratorName, Responder,
							TEXT("a generator class, e.g. EnvQueryGenerator_SimpleGrid — "
								 "list_classes names them all")))
					{
						return;
					}
					UClass* GeneratorClass =
						FindInstanceClass(GeneratorName, UEnvQueryGenerator::StaticClass());
					if (GeneratorClass == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_generator"),
							FString::Printf(
								TEXT("'%s' is not an EnvQueryGenerator — list_classes names them"),
								*GeneratorName));
						return;
					}
					UEdGraph* Graph = Query->EdGraph;
					UEdGraphNode* RootNode =
						Graph != nullptr ? FindNodeOfClass(Graph, RootNodeClassPath) : nullptr;
					if (RootNode == nullptr || RootNode->Pins.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_root"),
							TEXT("this query's graph has no root node to attach an option to — "
								 "create the asset through this route, or open it once in the editor"));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddEqsOption", "McpLink Add EQS Option"));
					Query->Modify();
					Graph->Modify();
					const int32 PosX = OptionNodes(Graph).Num() * 300;
					UAIGraphNode* OptionNode =
						SpawnNode(Graph, EditorClass(OptionNodeClassPath), GeneratorClass, PosX);
					OptionNode->NodePosY = 200;
					Graph->AddNode(OptionNode, false, false);
					if (OptionNode->Pins.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_pins"),
							TEXT("the option node came back without pins"));
						return;
					}
					// Through the schema, so the link is made the way the editor
					// would make it rather than by poking pin arrays.
					if (!Graph->GetSchema()->TryCreateConnection(RootNode->Pins[0], OptionNode->Pins[0]))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("link_failed"),
							TEXT("the schema refused to link the option to the root node"));
						return;
					}
					Compile(Query);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddQueryFields(Data, Query);
					Data->SetStringField(TEXT("generator"), GeneratorClass->GetName());
					Data->SetStringField(TEXT("message"),
						TEXT("option added — add_test scores its items, save to keep it"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_option"))
				{
					UAIGraphNode* OptionNode = OptionAtOrError(Query, Body, Responder);
					if (OptionNode == nullptr)
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "RemoveEqsOption", "McpLink Remove EQS Option"));
					Query->Modify();
					Query->EdGraph->Modify();
					OptionNode->RemoveAllSubNodes();
					Query->EdGraph->RemoveNode(OptionNode);
					Compile(Query);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddQueryFields(Data, Query);
					Data->SetStringField(TEXT("message"), TEXT("option removed"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_test"))
				{
					UAIGraphNode* OptionNode = OptionAtOrError(Query, Body, Responder);
					if (OptionNode == nullptr)
					{
						return;
					}
					FString TestName;
					if (!RequireString(Body, TEXT("test"), TestName, Responder,
							TEXT("a test class, e.g. EnvQueryTest_Distance — list_classes names them")))
					{
						return;
					}
					UClass* TestClass = FindInstanceClass(TestName, UEnvQueryTest::StaticClass());
					if (TestClass == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_test"),
							FString::Printf(
								TEXT("'%s' is not an EnvQueryTest — list_classes names them"), *TestName));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddEqsTest", "McpLink Add EQS Test"));
					Query->Modify();
					Query->EdGraph->Modify();
					UAIGraphNode* TestNode = NewObject<UAIGraphNode>(
						Query->EdGraph, EditorClass(TestNodeClassPath), NAME_None, RF_Transactional);
					TestNode->CreateNewGuid();
					// AddSubNode runs PostPlacedNewNode, which builds NodeInstance
					// from ClassData — so ClassData has to be set before it.
					TestNode->ClassData = FGraphNodeClassData(TestClass, FString());
					OptionNode->AddSubNode(TestNode, Query->EdGraph);
					Compile(Query);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddQueryFields(Data, Query);
					Data->SetStringField(TEXT("test"), TestClass->GetName());
					Data->SetStringField(TEXT("message"),
						TEXT("test added — set_property on its object_path tunes it, save to keep it"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_test") || Operation == TEXT("set_test_enabled"))
				{
					UAIGraphNode* OptionNode = OptionAtOrError(Query, Body, Responder);
					if (OptionNode == nullptr)
					{
						return;
					}
					const int32 TestIndex = IntOr(Body, TEXT("test_index"), -1);
					if (!OptionNode->SubNodes.IsValidIndex(TestIndex))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_test_index"),
							FString::Printf(
								TEXT("'test_index' must be 0..%d — info lists this option's tests"),
								OptionNode->SubNodes.Num() - 1));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditEqsTest", "McpLink Edit EQS Test"));
					Query->Modify();
					Query->EdGraph->Modify();
					UAIGraphNode* TestNode = OptionNode->SubNodes[TestIndex];
					if (Operation == TEXT("remove_test"))
					{
						OptionNode->SubNodes.RemoveAt(TestIndex);
						Query->EdGraph->RemoveNode(TestNode);
					}
					else
					{
						const bool bEnabled = BoolOr(Body, TEXT("enabled"), true);
						const FBoolProperty* Property =
							FindFProperty<FBoolProperty>(TestNode->GetClass(), TEXT("bTestEnabled"));
						if (Property == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::ServerError,
								TEXT("no_enabled_property"),
								TEXT("this build's test node has no bTestEnabled property"));
							return;
						}
						TestNode->Modify();
						Property->SetPropertyValue_InContainer(TestNode, bEnabled);
					}
					Compile(Query);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddQueryFields(Data, Query);
					Data->SetStringField(TEXT("message"),
						Operation == TEXT("remove_test") ? TEXT("test removed") : TEXT("test updated"));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_classes, create, info, add_option, ")
						TEXT("remove_option, add_test, remove_test, set_test_enabled, compile or save"),
						*Operation));
			});
	}
}
