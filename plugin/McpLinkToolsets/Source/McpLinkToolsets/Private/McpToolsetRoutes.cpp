// Toolset Registry interop: the engine's own catalogue of AI-callable tools
// (Epic's *Toolset plugins, project-authored UToolsetDefinition classes,
// Python toolsets and the Agent Skill toolset), listed, described and
// executed through one route.
//
// Everything goes through UToolsetRegistrySubsystem's FToolsetRegistry, which
// is what the engine's MCP server and AI Assistant call too: the same block
// and allow lists apply, a tool's arguments are the JSON its schema describes,
// and its return value comes back as the registry's "returnValue".

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResponder.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Templates/ValueOrError.h"
#include "ToolsetRegistry/Toolset.h"
#include "ToolsetRegistry/ToolsetRegistry.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"
#include "UObject/Class.h"

namespace McpLink
{
	namespace
	{
		using UE::ToolsetRegistry::FToolset;
		using UE::ToolsetRegistry::FToolsetRegistry;

		// The toolset the registry itself registers for Agent Skill assets.
		// Its class is not exported, so it is recognised by path.
		const TCHAR* const AgentSkillToolsetClassPath = TEXT("/Script/ToolsetRegistry.AgentSkillToolset");

		struct FToolEntry
		{
			// Full name, "Toolset.Tool" — what the registry executes.
			FString Name;
			FString Toolset;
			FString Description;
			// The registry's own entry: name, description, inputSchema, ...
			TSharedPtr<FJsonObject> Schema;

			FString ShortName() const
			{
				FString ToolsetPart, ToolPart;
				return Name.Split(TEXT("."), &ToolsetPart, &ToolPart, ESearchCase::IgnoreCase, ESearchDir::FromEnd)
					? ToolPart
					: Name;
			}
		};

		struct FToolsetSnapshot
		{
			FString Name;
			FString Version;
			FString Description;
			FString ClassPath;
			bool bEnabled = true;
			// Every tool the toolset defines, before the block/allow lists.
			int32 DefinedToolCount = 0;
			// The tools the registry will actually execute.
			TArray<FToolEntry> Tools;
		};

		TSharedPtr<FJsonObject> ParseJsonObject(const FString& Text)
		{
			TSharedPtr<FJsonObject> Parsed;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
			{
				return Parsed;
			}
			return nullptr;
		}

		FToolsetRegistry* RegistryOrError(const TSharedRef<FMcpResponder>& Responder)
		{
			TValueOrError<TObjectPtr<UToolsetRegistrySubsystem>, FString> Subsystem = UToolsetRegistrySubsystem::Get();
			if (Subsystem.HasError())
			{
				Responder->Error(EHttpServerResponseCodes::ServiceUnavail, TEXT("toolsets_unavailable"),
					FString::Printf(
						TEXT("the Toolset Registry is not available (%s) — enable the ToolsetRegistry plugin, plus "
							 "AllToolsets or the individual *Toolset plugins you want, and restart the editor"),
						*Subsystem.GetError()));
				return nullptr;
			}
			return &Subsystem.GetValue()->ToolsetRegistry;
		}

		// The registry as it stands: each toolset with the tools it will run.
		// Schemas are regenerated per call, which is what the registry does
		// for its own consumers too.
		TArray<FToolsetSnapshot> Snapshot(const FToolsetRegistry& Registry)
		{
			TArray<FToolsetSnapshot> Out;
			Registry.ForEachToolset([&Out](const FString& Name, const FToolset& Toolset)
			{
				FToolsetSnapshot& Item = Out.AddDefaulted_GetRef();
				Item.Name = Name;
				Item.Version = Toolset.GetToolsetVersion();
				Item.Description = Toolset.GetToolsetDescription();
				Item.bEnabled = Toolset.IsEnabled();
				if (const UClass* Class = Toolset.GetToolsetClass())
				{
					Item.ClassPath = Class->GetPathName();
				}
				Item.DefinedToolCount = Toolset.ListToolNames().Num();

				// Empty when the toolset is disabled or every tool is blocked.
				const FString SchemaText = Toolset.GetJsonSchema();
				if (SchemaText.IsEmpty())
				{
					return;
				}
				const TSharedPtr<FJsonObject> Schema = ParseJsonObject(SchemaText);
				const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
				if (!Schema.IsValid() || !Schema->TryGetArrayField(TEXT("tools"), Tools))
				{
					return;
				}
				for (const TSharedPtr<FJsonValue>& Value : *Tools)
				{
					const TSharedPtr<FJsonObject>* Object = nullptr;
					if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object->IsValid())
					{
						continue;
					}
					FToolEntry& Tool = Item.Tools.AddDefaulted_GetRef();
					Tool.Name = (*Object)->GetStringField(TEXT("name"));
					Tool.Toolset = Name;
					(*Object)->TryGetStringField(TEXT("description"), Tool.Description);
					Tool.Schema = *Object;
				}
				Item.Tools.Sort([](const FToolEntry& A, const FToolEntry& B) { return A.Name < B.Name; });
			});
			Out.Sort([](const FToolsetSnapshot& A, const FToolsetSnapshot& B) { return A.Name < B.Name; });
			return Out;
		}

		const FToolsetSnapshot* FindToolset(const TArray<FToolsetSnapshot>& Toolsets, const FString& Name)
		{
			return Toolsets.FindByPredicate([&Name](const FToolsetSnapshot& Item)
			{
				return Item.Name.Equals(Name, ESearchCase::IgnoreCase);
			});
		}

		// A tool by full name, or — when the toolset half is left off — by its
		// short name if exactly one toolset defines it. `Candidates` collects
		// the near misses for the error message.
		const FToolEntry* FindTool(
			const TArray<FToolsetSnapshot>& Toolsets, const FString& Spec, TArray<FString>& Candidates)
		{
			for (const FToolsetSnapshot& Toolset : Toolsets)
			{
				for (const FToolEntry& Tool : Toolset.Tools)
				{
					if (Tool.Name.Equals(Spec, ESearchCase::IgnoreCase))
					{
						return &Tool;
					}
				}
			}
			FString SpecToolset, SpecTool;
			if (!Spec.Split(TEXT("."), &SpecToolset, &SpecTool, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			{
				SpecTool = Spec;
			}
			TArray<const FToolEntry*> ByShortName;
			for (const FToolsetSnapshot& Toolset : Toolsets)
			{
				for (const FToolEntry& Tool : Toolset.Tools)
				{
					const FString Short = Tool.ShortName();
					if (Short.Equals(SpecTool, ESearchCase::IgnoreCase))
					{
						ByShortName.Add(&Tool);
					}
					else if (Short.Contains(SpecTool) && Candidates.Num() < 8)
					{
						Candidates.Add(Tool.Name);
					}
				}
			}
			if (ByShortName.Num() == 1 && SpecToolset.IsEmpty())
			{
				return ByShortName[0];
			}
			for (const FToolEntry* Tool : ByShortName)
			{
				Candidates.Insert(Tool->Name, 0);
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> ToolSummary(const FToolEntry& Tool, bool bIncludeSchema)
		{
			if (bIncludeSchema && Tool.Schema.IsValid())
			{
				// The registry's entry, plus the toolset it belongs to.
				const TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>(*Tool.Schema);
				Copy->SetStringField(TEXT("toolset"), Tool.Toolset);
				return Copy;
			}
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Tool.Name);
			Item->SetStringField(TEXT("toolset"), Tool.Toolset);
			Item->SetStringField(TEXT("description"), Tool.Description);
			return Item;
		}

		TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const FString& Value : Values)
			{
				Out.Add(MakeShared<FJsonValueString>(Value));
			}
			return Out;
		}

		// Tool results and the responder both belong to the game thread. A
		// synchronous tool fulfils its future on the game thread before
		// ExecuteTool returns; an asynchronous one signals completion from
		// the main thread too, but this is cheap insurance against a tool
		// that fulfils from a worker.
		void OnGameThread(TUniqueFunction<void()> Work)
		{
			if (IsInGameThread())
			{
				Work();
			}
			else
			{
				AsyncTask(ENamedThreads::GameThread, MoveTemp(Work));
			}
		}

		// The registry wraps every return value as {"returnValue": ...}; hand
		// back the value itself.
		TSharedPtr<FJsonValue> UnwrapResult(const FString& ResultJson)
		{
			const TSharedPtr<FJsonObject> Object = ParseJsonObject(ResultJson);
			if (!Object.IsValid())
			{
				return MakeShared<FJsonValueString>(ResultJson);
			}
			if (const TSharedPtr<FJsonValue> ReturnValue = Object->TryGetField(TEXT("returnValue")))
			{
				return ReturnValue;
			}
			return MakeShared<FJsonValueObject>(Object);
		}

		// Run one tool and complete the responder when its future does.
		void RunTool(
			FToolsetRegistry& Registry, const FString& ToolName, const FString& ArgumentsJson,
			const TSharedRef<FMcpResponder>& Responder,
			TFunction<void(const TSharedRef<FJsonObject>& Data, const TSharedPtr<FJsonValue>& Result)> Shape)
		{
			const double Start = FPlatformTime::Seconds();
			Registry.ExecuteTool(ToolName, ArgumentsJson)
				.Next([Responder, ToolName, Start, Shape](TValueOrError<FString, FString> Result)
				{
					OnGameThread([Responder, ToolName, Start, Shape, Result = MoveTemp(Result)]() mutable
					{
						const double ElapsedMs = (FPlatformTime::Seconds() - Start) * 1000.0;
						if (Result.HasError())
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("tool_failed"),
								FString::Printf(TEXT("%s: %s"), *ToolName, *Result.GetError()));
							return;
						}
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("tool"), ToolName);
						Data->SetNumberField(TEXT("elapsed_ms"), FMath::RoundToDouble(ElapsedMs * 10.0) / 10.0);
						Shape(Data, UnwrapResult(Result.GetValue()));
						Responder->Ok(Data);
					});
				});
		}

		void ListToolsets(
			const FToolsetRegistry& Registry, const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString NameContains;
			Body->TryGetStringField(TEXT("name_contains"), NameContains);
			const bool bIncludeTools = BoolOr(Body, TEXT("include_tools"), false);

			TArray<TSharedPtr<FJsonValue>> Items;
			int32 ToolTotal = 0;
			for (const FToolsetSnapshot& Toolset : Snapshot(Registry))
			{
				if (!NameContains.IsEmpty() && !Toolset.Name.Contains(NameContains)
					&& !Toolset.Description.Contains(NameContains))
				{
					continue;
				}
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Toolset.Name);
				Item->SetStringField(TEXT("version"), Toolset.Version);
				Item->SetStringField(TEXT("description"), Toolset.Description);
				Item->SetBoolField(TEXT("enabled"), Toolset.bEnabled);
				Item->SetNumberField(TEXT("tool_count"), Toolset.Tools.Num());
				if (Toolset.DefinedToolCount > Toolset.Tools.Num())
				{
					Item->SetNumberField(TEXT("blocked_tool_count"), Toolset.DefinedToolCount - Toolset.Tools.Num());
				}
				if (!Toolset.ClassPath.IsEmpty())
				{
					Item->SetStringField(TEXT("class"), Toolset.ClassPath);
				}
				if (bIncludeTools)
				{
					TArray<TSharedPtr<FJsonValue>> Tools;
					for (const FToolEntry& Tool : Toolset.Tools)
					{
						Tools.Add(MakeShared<FJsonValueObject>(ToolSummary(Tool, false)));
					}
					Item->SetArrayField(TEXT("tools"), Tools);
				}
				ToolTotal += Toolset.Tools.Num();
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}

			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("toolsets"), Items);
			Data->SetNumberField(TEXT("count"), Items.Num());
			Data->SetNumberField(TEXT("tool_count"), ToolTotal);
			Data->SetArrayField(TEXT("blocked_names"), Strings(Registry.GetBlockedNames()));
			Data->SetArrayField(TEXT("allowed_names"), Strings(Registry.GetAllowedNames()));
			Responder->Ok(Data);
		}

		void ListTools(
			const FToolsetRegistry& Registry, const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString ToolsetName, NameContains;
			Body->TryGetStringField(TEXT("toolset"), ToolsetName);
			Body->TryGetStringField(TEXT("name_contains"), NameContains);
			const bool bIncludeSchemas = BoolOr(Body, TEXT("include_schemas"), false);
			const int32 MaxResults = FMath::Max(1, IntOr(Body, TEXT("max_results"), 500));

			const TArray<FToolsetSnapshot> Toolsets = Snapshot(Registry);
			if (!ToolsetName.IsEmpty() && FindToolset(Toolsets, ToolsetName) == nullptr)
			{
				TArray<FString> Names;
				for (const FToolsetSnapshot& Toolset : Toolsets)
				{
					Names.Add(Toolset.Name);
				}
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("toolset_not_found"),
					FString::Printf(TEXT("no toolset named '%s' is registered — registered: %s"), *ToolsetName,
						*FString::Join(Names, TEXT(", "))));
				return;
			}

			TArray<TSharedPtr<FJsonValue>> Items;
			int32 Total = 0;
			for (const FToolsetSnapshot& Toolset : Toolsets)
			{
				if (!ToolsetName.IsEmpty() && !Toolset.Name.Equals(ToolsetName, ESearchCase::IgnoreCase))
				{
					continue;
				}
				for (const FToolEntry& Tool : Toolset.Tools)
				{
					if (!NameContains.IsEmpty() && !Tool.Name.Contains(NameContains)
						&& !Tool.Description.Contains(NameContains))
					{
						continue;
					}
					++Total;
					if (Items.Num() < MaxResults)
					{
						Items.Add(MakeShared<FJsonValueObject>(ToolSummary(Tool, bIncludeSchemas)));
					}
				}
			}

			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("tools"), Items);
			Data->SetNumberField(TEXT("count"), Items.Num());
			Data->SetNumberField(TEXT("total"), Total);
			Data->SetBoolField(TEXT("truncated"), Total > Items.Num());
			Responder->Ok(Data);
		}

		void GetTool(
			const FToolsetRegistry& Registry, const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			if (!Body->TryGetStringField(TEXT("tool"), Spec) || Spec.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'tool' is required — a full name such as 'EditorApp.GetCameraTransform' from list_tools"));
				return;
			}
			const TArray<FToolsetSnapshot> Toolsets = Snapshot(Registry);
			TArray<FString> Candidates;
			const FToolEntry* Tool = FindTool(Toolsets, Spec, Candidates);
			if (Tool == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("tool_not_found"),
					Candidates.IsEmpty()
						? FString::Printf(TEXT("no registered tool matches '%s' — see list_tools"), *Spec)
						: FString::Printf(TEXT("no registered tool matches '%s' — did you mean: %s"), *Spec,
							*FString::Join(Candidates, TEXT(", "))));
				return;
			}
			Responder->Ok(ToolSummary(*Tool, true));
		}

		void Execute(
			FToolsetRegistry& Registry, const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			if (!Body->TryGetStringField(TEXT("tool"), Spec) || Spec.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'tool' is required — a full name such as 'EditorApp.GetCameraTransform' from list_tools"));
				return;
			}
			const TArray<FToolsetSnapshot> Toolsets = Snapshot(Registry);
			TArray<FString> Candidates;
			const FToolEntry* Tool = FindTool(Toolsets, Spec, Candidates);
			if (Tool == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("tool_not_found"),
					Candidates.IsEmpty()
						? FString::Printf(TEXT("no registered tool matches '%s' — see list_tools"), *Spec)
						: FString::Printf(TEXT("no registered tool matches '%s' — did you mean: %s"), *Spec,
							*FString::Join(Candidates, TEXT(", "))));
				return;
			}

			FString ArgumentsJson = TEXT("{}");
			const TSharedPtr<FJsonObject>* Arguments = nullptr;
			if (Body->HasField(TEXT("arguments")))
			{
				if (!Body->TryGetObjectField(TEXT("arguments"), Arguments) || !Arguments->IsValid())
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_field"),
						TEXT("'arguments' must be an object keyed by the tool's parameter names — get_tool reports its inputSchema"));
					return;
				}
				ArgumentsJson = JsonToString(Arguments->ToSharedRef());
			}

			RunTool(Registry, Tool->Name, ArgumentsJson, Responder,
				[](const TSharedRef<FJsonObject>& Data, const TSharedPtr<FJsonValue>& Result)
				{
					Data->SetField(TEXT("result"), Result);
				});
		}

		// The AgentSkill toolset's tool of a given short name, if that toolset
		// is registered and the tool survived the filters.
		const FToolEntry* FindSkillTool(
			const TArray<FToolsetSnapshot>& Toolsets, const TCHAR* ShortName, const TSharedRef<FMcpResponder>& Responder)
		{
			const FToolsetSnapshot* Skills = Toolsets.FindByPredicate([](const FToolsetSnapshot& Item)
			{
				return Item.ClassPath == AgentSkillToolsetClassPath;
			});
			if (Skills == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("skills_unavailable"),
					TEXT("the Agent Skill toolset is not registered — the ToolsetRegistry plugin registers it at startup, "
						 "unless the Toolset Registry settings block it"));
				return nullptr;
			}
			const FToolEntry* Tool = Skills->Tools.FindByPredicate([ShortName](const FToolEntry& Item)
			{
				return Item.ShortName().Equals(ShortName, ESearchCase::IgnoreCase);
			});
			if (Tool == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("skills_unavailable"),
					FString::Printf(TEXT("the Agent Skill toolset has no callable '%s' — the Toolset Registry settings block it"), ShortName));
			}
			return Tool;
		}

		// The name of a tool's first input parameter, from its schema, so the
		// skill helpers need not hardcode how the engine spells it.
		FString FirstParameterName(const FToolEntry& Tool, const TCHAR* Fallback)
		{
			const TSharedPtr<FJsonObject>* Input = nullptr;
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (Tool.Schema.IsValid() && Tool.Schema->TryGetObjectField(TEXT("inputSchema"), Input)
				&& (*Input)->TryGetObjectField(TEXT("properties"), Properties))
			{
				for (const auto& Pair : (*Properties)->Values)
				{
					return FString(Pair.Key.ToView());
				}
			}
			return Fallback;
		}

		void ListSkills(FToolsetRegistry& Registry, const TSharedRef<FMcpResponder>& Responder)
		{
			// The entry points into the snapshot, which has to outlive it.
			const TArray<FToolsetSnapshot> Toolsets = Snapshot(Registry);
			const FToolEntry* Tool = FindSkillTool(Toolsets, TEXT("ListSkills"), Responder);
			if (Tool == nullptr)
			{
				return;
			}
			RunTool(Registry, Tool->Name, TEXT("{}"), Responder,
				[](const TSharedRef<FJsonObject>& Data, const TSharedPtr<FJsonValue>& Result)
				{
					// A map of skill path to description.
					TArray<TSharedPtr<FJsonValue>> Skills;
					const TSharedPtr<FJsonObject>* Map = nullptr;
					if (Result.IsValid() && Result->TryGetObject(Map) && Map->IsValid())
					{
						for (const auto& Pair : (*Map)->Values)
						{
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("path"), FString(Pair.Key.ToView()));
							FString Description;
							if (Pair.Value.IsValid())
							{
								Pair.Value->TryGetString(Description);
							}
							Item->SetStringField(TEXT("description"), Description);
							Skills.Add(MakeShared<FJsonValueObject>(Item));
						}
					}
					Skills.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
					{
						return A->AsObject()->GetStringField(TEXT("path")) < B->AsObject()->GetStringField(TEXT("path"));
					});
					Data->SetArrayField(TEXT("skills"), Skills);
					Data->SetNumberField(TEXT("count"), Skills.Num());
				});
		}

		void GetSkill(
			FToolsetRegistry& Registry, const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			TArray<FString> Paths;
			FString Path;
			if (Body->TryGetStringField(TEXT("path"), Path) && !Path.IsEmpty())
			{
				Paths.Add(Path);
			}
			const TArray<TSharedPtr<FJsonValue>>* PathValues = nullptr;
			if (Body->TryGetArrayField(TEXT("paths"), PathValues))
			{
				for (const TSharedPtr<FJsonValue>& Value : *PathValues)
				{
					FString Item;
					if (Value.IsValid() && Value->TryGetString(Item) && !Item.IsEmpty())
					{
						Paths.Add(Item);
					}
				}
			}
			if (Paths.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'path' (a skill path from list_skills) or 'paths' is required"));
				return;
			}
			const TArray<FToolsetSnapshot> Toolsets = Snapshot(Registry);
			const FToolEntry* Tool = FindSkillTool(Toolsets, TEXT("GetSkills"), Responder);
			if (Tool == nullptr)
			{
				return;
			}
			const TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
			Arguments->SetArrayField(FirstParameterName(*Tool, TEXT("SkillPaths")), Strings(Paths));

			RunTool(Registry, Tool->Name, JsonToString(Arguments), Responder,
				[Paths](const TSharedRef<FJsonObject>& Data, const TSharedPtr<FJsonValue>& Result)
				{
					// A map of skill path to its details (the instructions).
					TArray<TSharedPtr<FJsonValue>> Skills;
					const TSharedPtr<FJsonObject>* Map = nullptr;
					if (Result.IsValid() && Result->TryGetObject(Map) && Map->IsValid())
					{
						for (const auto& Pair : (*Map)->Values)
						{
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("path"), FString(Pair.Key.ToView()));
							const TSharedPtr<FJsonObject>* Details = nullptr;
							FString Instructions;
							if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Details) && Details->IsValid())
							{
								for (const auto& Field : (*Details)->Values)
								{
									if (FString(Field.Key.ToView()).Equals(TEXT("Instructions"), ESearchCase::IgnoreCase)
										&& Field.Value.IsValid())
									{
										Field.Value->TryGetString(Instructions);
									}
								}
								Item->SetObjectField(TEXT("details"), *Details);
							}
							Item->SetStringField(TEXT("instructions"), Instructions);
							Skills.Add(MakeShared<FJsonValueObject>(Item));
						}
					}
					Data->SetArrayField(TEXT("skills"), Skills);
					Data->SetNumberField(TEXT("count"), Skills.Num());
					TArray<FString> Missing;
					for (const FString& Requested : Paths)
					{
						const bool bFound = Skills.ContainsByPredicate([&Requested](const TSharedPtr<FJsonValue>& Item)
						{
							return Item->AsObject()->GetStringField(TEXT("path")).Equals(Requested, ESearchCase::IgnoreCase);
						});
						if (!bFound)
						{
							Missing.Add(Requested);
						}
					}
					if (!Missing.IsEmpty())
					{
						Data->SetArrayField(TEXT("not_found"), Strings(Missing));
					}
				});
		}
	}

	void RegisterToolsetRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/toolsets/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				FToolsetRegistry* Registry = RegistryOrError(Responder);
				if (Registry == nullptr)
				{
					return;
				}

				if (Operation == TEXT("list_toolsets"))
				{
					ListToolsets(*Registry, Body, Responder);
				}
				else if (Operation == TEXT("list_tools"))
				{
					ListTools(*Registry, Body, Responder);
				}
				else if (Operation == TEXT("get_tool"))
				{
					GetTool(*Registry, Body, Responder);
				}
				else if (Operation == TEXT("execute"))
				{
					Execute(*Registry, Body, Responder);
				}
				else if (Operation == TEXT("list_skills"))
				{
					ListSkills(*Registry, Responder);
				}
				else if (Operation == TEXT("get_skill"))
				{
					GetSkill(*Registry, Body, Responder);
				}
				else
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
						FString::Printf(
							TEXT("unknown operation '%s' — expected list_toolsets, list_tools, get_tool, execute, list_skills or get_skill"),
							*Operation));
				}
			});
	}
}
