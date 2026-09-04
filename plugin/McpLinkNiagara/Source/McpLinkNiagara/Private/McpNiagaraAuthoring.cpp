// Niagara authoring: build systems and emitters, not just drive live ones.
//
// A Niagara system is a set of emitter handles; each emitter carries four
// script stacks (emitter spawn/update, particle spawn/update) plus a list of
// renderers. Every stack is a chain of UNiagaraNodeFunctionCall nodes threaded
// through a parameter-map pin and terminated by a UNiagaraNodeOutput — that
// chain *is* what the Niagara editor's stack view draws.
//
// Everything here goes through the engine's own exported entry points where
// they exist (FNiagaraEditorUtilities::AddEmitterToSystem,
// FNiagaraStackGraphUtilities::AddScriptModuleToStack / SetModuleIsEnabled,
// FNiagaraStackFunctionInputBinder for typed input values,
// UNiagaraEmitter::AddRenderer). Module removal has no exported entry point,
// so it is done by relinking the parameter-map chain the way the engine does,
// and the route recompiles afterwards so a bad edit surfaces immediately.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterMapHistory.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraStackFunctionInputBinder.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"

namespace McpLink
{
	namespace NiagaraAuthoring
	{
		/// The script stacks addressable on the wire. `bEmitter` stages live on
		/// an emitter; the others on the system itself.
		struct FStage
		{
			const TCHAR* Name;
			ENiagaraScriptUsage Usage;
			bool bEmitter;
		};

		const FStage Stages[] = {
			{TEXT("system_spawn"), ENiagaraScriptUsage::SystemSpawnScript, false},
			{TEXT("system_update"), ENiagaraScriptUsage::SystemUpdateScript, false},
			{TEXT("emitter_spawn"), ENiagaraScriptUsage::EmitterSpawnScript, true},
			{TEXT("emitter_update"), ENiagaraScriptUsage::EmitterUpdateScript, true},
			{TEXT("particle_spawn"), ENiagaraScriptUsage::ParticleSpawnScript, true},
			{TEXT("particle_update"), ENiagaraScriptUsage::ParticleUpdateScript, true},
		};

		FString StageNames()
		{
			TArray<FString> Names;
			for (const FStage& Stage : Stages)
			{
				Names.Add(Stage.Name);
			}
			return FString::Join(Names, TEXT(", "));
		}

		const FStage* FindStage(const FString& Name)
		{
			for (const FStage& Stage : Stages)
			{
				if (Name.Equals(Stage.Name, ESearchCase::IgnoreCase))
				{
					return &Stage;
				}
			}
			return nullptr;
		}

		/// One resolved stack: the script whose parameters an input write lands
		/// in, and the output node the module chain terminates at.
		struct FStackTarget
		{
			UNiagaraSystem* System = nullptr;
			FVersionedNiagaraEmitter Emitter;
			UNiagaraScript* Script = nullptr;
			UNiagaraNodeOutput* OutputNode = nullptr;
			ENiagaraScriptUsage Usage = ENiagaraScriptUsage::Module;

			FCompileConstantResolver Resolver() const
			{
				return Emitter.Emitter != nullptr ? FCompileConstantResolver(Emitter, Usage)
												  : FCompileConstantResolver(System, Usage);
			}

			FString EmitterName() const
			{
				return Emitter.Emitter != nullptr ? Emitter.Emitter->GetUniqueEmitterName() : FString();
			}
		};

		bool IsParameterMapPin(const UEdGraphPin* Pin)
		{
			return Pin != nullptr
				&& Pin->PinType.PinSubCategoryObject == FNiagaraTypeDefinition::GetParameterMapDef().GetStruct();
		}

		/// UNiagaraNodeParameterMapSet — the node that carries a module's input
		/// overrides — lives in NiagaraEditor/Private, so it is matched by
		/// class path rather than by type.
		bool IsOverrideNode(const UEdGraphNode* Node)
		{
			const UClass* MapSetClass =
				FindObject<UClass>(nullptr, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"));
			return Node != nullptr && MapSetClass != nullptr && Node->IsA(MapSetClass);
		}

		UEdGraphPin* MapPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
		{
			if (Node == nullptr)
			{
				return nullptr;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin != nullptr && Pin->Direction == Direction && IsParameterMapPin(Pin))
				{
					return Pin;
				}
			}
			return nullptr;
		}

		/// The module chain feeding an output node, in execution order.
		///
		/// FNiagaraStackGraphUtilities::GetOrderedModuleNodes does this inside
		/// NiagaraEditor but is not exported, so the chain is walked directly:
		/// every stack node threads one parameter-map pin from the previous
		/// node to the next, ending at the output node.
		TArray<UNiagaraNodeFunctionCall*> OrderedModules(UNiagaraNodeOutput* OutputNode)
		{
			TArray<UNiagaraNodeFunctionCall*> Modules;
			UEdGraphNode* Node = OutputNode;
			// The chain is finite, but a corrupt graph must not hang the editor.
			for (int32 Guard = 0; Node != nullptr && Guard < 4096; ++Guard)
			{
				UEdGraphPin* Input = MapPin(Node, EGPD_Input);
				if (Input == nullptr || Input->LinkedTo.IsEmpty() || Input->LinkedTo[0] == nullptr)
				{
					break;
				}
				UEdGraphNode* Previous = Input->LinkedTo[0]->GetOwningNode();
				if (Previous == nullptr || Previous == Node)
				{
					break;
				}
				if (UNiagaraNodeFunctionCall* Call = Cast<UNiagaraNodeFunctionCall>(Previous))
				{
					Modules.Insert(Call, 0);
				}
				Node = Previous;
			}
			return Modules;
		}

		UNiagaraScriptSource* SourceOf(UNiagaraScript* Script)
		{
			return Script != nullptr ? Cast<UNiagaraScriptSource>(Script->GetLatestSource()) : nullptr;
		}

		FNiagaraEmitterHandle* FindHandle(UNiagaraSystem* System, const FString& Spec)
		{
			for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				if (Handle.GetName().ToString().Equals(Spec, ESearchCase::IgnoreCase)
					|| Handle.GetId().ToString() == Spec)
				{
					return &Handle;
				}
			}
			return nullptr;
		}

		/// Resolve `stage` (+ `emitter` for emitter stages) into a stack.
		/// Responds and returns false on every failure, so callers just bail.
		bool ResolveStack(
			UNiagaraSystem* System,
			const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder,
			FStackTarget& Out)
		{
			FString StageName;
			Body->TryGetStringField(TEXT("stage"), StageName);
			const FStage* Stage = FindStage(StageName);
			if (Stage == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_stage"),
					FString::Printf(TEXT("'stage' must be one of %s (got '%s')"), *StageNames(), *StageName));
				return false;
			}

			Out.System = System;
			Out.Usage = Stage->Usage;
			if (Stage->bEmitter)
			{
				FString EmitterSpec;
				if (!RequireString(Body, TEXT("emitter"), EmitterSpec, Responder,
						TEXT("the emitter name in the system, from niagara_author stack")))
				{
					return false;
				}
				FNiagaraEmitterHandle* Handle = FindHandle(System, EmitterSpec);
				if (Handle == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("emitter_not_found"),
						FString::Printf(TEXT("system '%s' has no emitter '%s'"),
							*System->GetName(), *EmitterSpec));
					return false;
				}
				Out.Emitter = Handle->GetInstance();
				FVersionedNiagaraEmitterData* Data = Handle->GetEmitterData();
				if (Data == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_emitter_data"),
						FString::Printf(TEXT("emitter '%s' has no data for its version"), *EmitterSpec));
					return false;
				}
				switch (Stage->Usage)
				{
					case ENiagaraScriptUsage::EmitterSpawnScript:
						Out.Script = Data->EmitterSpawnScriptProps.Script;
						break;
					case ENiagaraScriptUsage::EmitterUpdateScript:
						Out.Script = Data->EmitterUpdateScriptProps.Script;
						break;
					case ENiagaraScriptUsage::ParticleSpawnScript:
						Out.Script = Data->SpawnScriptProps.Script;
						break;
					default:
						Out.Script = Data->UpdateScriptProps.Script;
						break;
				}
				// All four emitter scripts share the emitter's one graph; the
				// output node's usage is what separates the stacks.
				if (UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(Data->GraphSource))
				{
					Out.OutputNode = Source->NodeGraph != nullptr
						? Source->NodeGraph->FindEquivalentOutputNode(Stage->Usage, FGuid())
						: nullptr;
				}
			}
			else
			{
				Out.Script = Stage->Usage == ENiagaraScriptUsage::SystemSpawnScript
					? System->GetSystemSpawnScript()
					: System->GetSystemUpdateScript();
				if (UNiagaraScriptSource* Source = SourceOf(Out.Script))
				{
					Out.OutputNode = Source->NodeGraph != nullptr
						? Source->NodeGraph->FindEquivalentOutputNode(Stage->Usage, FGuid())
						: nullptr;
				}
			}

			if (Out.Script == nullptr || Out.OutputNode == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_stack"),
					FString::Printf(
						TEXT("stage '%s' has no script graph on this system — a system created outside ")
						TEXT("niagara_author may predate its default nodes"),
						*StageName));
				return false;
			}
			return true;
		}

		// ------------------------------------------------------------- JSON ---

		TSharedRef<FJsonObject> ModuleToJson(
			UNiagaraNodeFunctionCall* Call, int32 Index, const FStackTarget& Target, bool bIncludeInputs)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("id"), Call->NodeGuid.ToString());
			Object->SetNumberField(TEXT("index"), Index);
			Object->SetStringField(TEXT("name"), Call->GetFunctionName());
			Object->SetStringField(TEXT("script"),
				Call->FunctionScript != nullptr ? Call->FunctionScript->GetPathName() : FString());
			// SetModuleIsEnabled drives the node's enabled state, so that is
			// what reads it back (GetModuleIsEnabled is not exported).
			Object->SetBoolField(TEXT("enabled"), Call->IsNodeEnabled());
			if (bIncludeInputs)
			{
				TArray<FNiagaraVariable> Inputs;
				FNiagaraStackGraphUtilities::GetStackFunctionInputs(*Call, Inputs, Target.Resolver(),
					FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
					/*bIgnoreDisabled*/ false);
				TArray<TSharedPtr<FJsonValue>> InputJson;
				for (const FNiagaraVariable& Input : Inputs)
				{
					const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					// Inputs are namespaced "Module.<Name>" inside the script.
					FString Name = Input.GetName().ToString();
					Name.RemoveFromStart(TEXT("Module."));
					Item->SetStringField(TEXT("name"), Name);
					Item->SetStringField(TEXT("type"), Input.GetType().GetName());
					InputJson.Add(MakeShared<FJsonValueObject>(Item));
				}
				Object->SetArrayField(TEXT("inputs"), InputJson);
			}
			return Object;
		}

		TSharedRef<FJsonObject> RendererToJson(UNiagaraRendererProperties* Renderer, int32 Index)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetNumberField(TEXT("index"), Index);
			Object->SetStringField(TEXT("class"), Renderer->GetClass()->GetName());
			// Renderer options are ordinary UPROPERTYs, so set_property on this
			// path is how they are tuned.
			Object->SetStringField(TEXT("path"), Renderer->GetPathName());
			Object->SetBoolField(TEXT("enabled"), Renderer->GetIsEnabled());
			return Object;
		}

		// ------------------------------------------------------ input values ---

		bool ReadNumberArray(const TSharedPtr<FJsonValue>& Value, int32 Count, TArray<double>& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (!Value.IsValid() || !Value->TryGetArray(Array) || Array->Num() != Count)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Element : *Array)
			{
				double Number = 0.0;
				if (!Element.IsValid() || !Element->TryGetNumber(Number))
				{
					return false;
				}
				Out.Add(Number);
			}
			return true;
		}

		/// JSON value -> the raw bytes a Niagara parameter of `Type` expects.
		bool ValueToBytes(
			const FNiagaraTypeDefinition& Type,
			const TSharedPtr<FJsonValue>& Value,
			TArray<uint8>& Out,
			FString& OutError)
		{
			const auto Append = [&Out](const void* Data, int32 Size)
			{
				Out.Append(static_cast<const uint8*>(Data), Size);
			};
			double Number = 0.0;
			bool Flag = false;
			TArray<double> Parts;
			if (Type == FNiagaraTypeDefinition::GetFloatDef() && Value->TryGetNumber(Number))
			{
				const float Float = static_cast<float>(Number);
				Append(&Float, sizeof(Float));
			}
			else if (Type == FNiagaraTypeDefinition::GetIntDef() && Value->TryGetNumber(Number))
			{
				const int32 Int = static_cast<int32>(Number);
				Append(&Int, sizeof(Int));
			}
			else if (Type == FNiagaraTypeDefinition::GetBoolDef() && Value->TryGetBool(Flag))
			{
				// A Niagara bool is a 4-byte FNiagaraBool, not a C++ bool.
				const FNiagaraBool Bool(Flag);
				Append(&Bool, sizeof(Bool));
			}
			else if (Type == FNiagaraTypeDefinition::GetVec2Def() && ReadNumberArray(Value, 2, Parts))
			{
				const FVector2f Vector(Parts[0], Parts[1]);
				Append(&Vector, sizeof(Vector));
			}
			else if ((Type == FNiagaraTypeDefinition::GetVec3Def()
						 || Type == FNiagaraTypeDefinition::GetPositionDef())
				&& ReadNumberArray(Value, 3, Parts))
			{
				const FVector3f Vector(Parts[0], Parts[1], Parts[2]);
				Append(&Vector, sizeof(Vector));
			}
			else if ((Type == FNiagaraTypeDefinition::GetVec4Def()
						 || Type == FNiagaraTypeDefinition::GetColorDef()
						 || Type == FNiagaraTypeDefinition::GetQuatDef())
				&& ReadNumberArray(Value, 4, Parts))
			{
				const FVector4f Vector(Parts[0], Parts[1], Parts[2], Parts[3]);
				Append(&Vector, sizeof(Vector));
			}
			else if (Type == FNiagaraTypeDefinition::GetColorDef() && ReadNumberArray(Value, 3, Parts))
			{
				const FVector4f Vector(Parts[0], Parts[1], Parts[2], 1.0f);
				Append(&Vector, sizeof(Vector));
			}
			else
			{
				OutError = FString::Printf(
					TEXT("input is a %s — pass a number (float/int), bool, or a [x,y] / [x,y,z] / ")
					TEXT("[x,y,z,w] array; other types (data interfaces, enums, structs) have no ")
					TEXT("literal form here"),
					*Type.GetName());
				return false;
			}
			if (Out.Num() != Type.GetSize())
			{
				OutError = FString::Printf(TEXT("value is %d bytes but %s wants %d"),
					Out.Num(), *Type.GetName(), Type.GetSize());
				return false;
			}
			return true;
		}

		// ------------------------------------------------------------- edits ---

		/// Detach a module from its stack, stitching the parameter-map chain
		/// back together and dropping the override node that fed its inputs.
		/// Mirrors what FNiagaraStackGraphUtilities::RemoveModuleFromStack does
		/// inside NiagaraEditor, which is not exported.
		bool DetachModule(UNiagaraNodeFunctionCall* Call, FString& OutError)
		{
			UEdGraph* Graph = Call->GetGraph();
			if (Graph == nullptr)
			{
				OutError = TEXT("module node has no graph");
				return false;
			}
			UEdGraphPin* Input = MapPin(Call, EGPD_Input);
			UEdGraphPin* Output = MapPin(Call, EGPD_Output);
			if (Input == nullptr || Output == nullptr)
			{
				OutError = TEXT("module node has no parameter map pins to relink");
				return false;
			}
			UEdGraphPin* Upstream =
				Input->LinkedTo.IsEmpty() ? nullptr : Input->LinkedTo[0];
			const TArray<UEdGraphPin*> Downstream = Output->LinkedTo;
			if (Upstream == nullptr)
			{
				OutError = TEXT("module is not connected to the stack");
				return false;
			}

			// An override node sits immediately upstream and carries this
			// module's input values; it must go with the module.
			UEdGraphNode* UpstreamNode = Upstream->GetOwningNode();
			TArray<UEdGraphNode*> ToRemove;
			ToRemove.Add(Call);
			const FString Prefix = Call->GetFunctionName() + TEXT(".");
			if (IsOverrideNode(UpstreamNode))
			{
				bool bOnlyThisModule = true;
				for (const UEdGraphPin* Pin : UpstreamNode->Pins)
				{
					if (Pin->Direction == EGPD_Input && !IsParameterMapPin(Pin)
						&& !Pin->PinName.ToString().StartsWith(Prefix))
					{
						bOnlyThisModule = false;
						break;
					}
				}
				if (bOnlyThisModule)
				{
					UEdGraphPin* OverrideInput = MapPin(UpstreamNode, EGPD_Input);
					Upstream = (OverrideInput != nullptr && !OverrideInput->LinkedTo.IsEmpty())
						? OverrideInput->LinkedTo[0]
						: nullptr;
					ToRemove.Add(UpstreamNode);
				}
			}
			if (Upstream == nullptr)
			{
				OutError = TEXT("could not find the node feeding this module");
				return false;
			}

			for (UEdGraphPin* Next : Downstream)
			{
				Upstream->MakeLinkTo(Next);
			}
			for (UEdGraphNode* Node : ToRemove)
			{
				Node->Modify();
				Node->BreakAllNodeLinks();
				Graph->RemoveNode(Node);
			}
			Graph->NotifyGraphChanged();
			return true;
		}
	}

	using namespace NiagaraAuthoring;

	void RegisterNiagaraAuthoringRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/niagara/author"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				// ---------------------------------------------- discovery ---
				if (Operation == TEXT("list_module_scripts"))
				{
					FString Contains, StageName;
					Body->TryGetStringField(TEXT("name_contains"), Contains);
					Body->TryGetStringField(TEXT("stage"), StageName);

					FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions Options;
					Options.ScriptUsageToInclude = ENiagaraScriptUsage::Module;
					Options.bIncludeNonLibraryScripts = BoolOr(Body, TEXT("include_non_library"), false);
					if (const FStage* Stage = FindStage(StageName))
					{
						Options.TargetUsageToMatch = Stage->Usage;
					}
					TArray<FAssetData> Assets;
					FNiagaraEditorUtilities::GetFilteredScriptAssets(Options, Assets);

					const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_results"), 100), 1, 500);
					TArray<TSharedPtr<FJsonValue>> Results;
					int32 Matched = 0;
					for (const FAssetData& Asset : Assets)
					{
						const FString Name = Asset.AssetName.ToString();
						if (!Contains.IsEmpty() && !Name.Contains(Contains))
						{
							continue;
						}
						++Matched;
						if (Results.Num() >= Max)
						{
							continue;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Name);
						Item->SetStringField(TEXT("path"), Asset.PackageName.ToString());
						Results.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Matched);
					Data->SetArrayField(TEXT("scripts"), Results);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_renderer_classes"))
				{
					TArray<TSharedPtr<FJsonValue>> Classes;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						UClass* Class = *It;
						if (!Class->IsChildOf(UNiagaraRendererProperties::StaticClass())
							|| Class == UNiagaraRendererProperties::StaticClass()
							|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
						{
							continue;
						}
						Classes.Add(MakeShared<FJsonValueString>(Class->GetName()));
					}
					Classes.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
						{ return A->AsString() < B->AsString(); });
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("count"), Classes.Num());
					Data->SetArrayField(TEXT("classes"), Classes);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("list_stages"))
				{
					TArray<TSharedPtr<FJsonValue>> Names;
					for (const FStage& Stage : Stages)
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("stage"), Stage.Name);
						Item->SetStringField(TEXT("scope"), Stage.bEmitter ? TEXT("emitter") : TEXT("system"));
						Names.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("stages"), Names);
					Responder->Ok(Data);
					return;
				}

				// ------------------------------------------------ creation ---
				if (Operation == TEXT("create_system") || Operation == TEXT("create_emitter"))
				{
					const bool bSystem = Operation == TEXT("create_system");
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							bSystem ? TEXT("e.g. /Game/FX/NS_Sparks") : TEXT("e.g. /Game/FX/NE_Sparks")))
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
						NSLOCTEXT("McpLink", "CreateNiagaraAsset", "McpLink Create Niagara Asset"));
					UPackage* Package = CreatePackage(*Path);
					const FName AssetName(*FPackageName::GetShortName(Path));
					const EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transactional;

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					if (bSystem)
					{
						UNiagaraSystem* System = NewObject<UNiagaraSystem>(Package, AssetName, Flags);
						UNiagaraSystemFactoryNew::InitializeSystem(System, /*bCreateDefaultNodes*/ true);
						FAssetRegistryModule::AssetCreated(System);
						Data->SetStringField(TEXT("system"), System->GetPathName());
					}
					else
					{
						UNiagaraEmitter* Emitter = NewObject<UNiagaraEmitter>(Package, AssetName, Flags);
						UNiagaraEmitterFactoryNew::InitializeEmitter(
							Emitter, BoolOr(Body, TEXT("add_defaults"), true));
						FAssetRegistryModule::AssetCreated(Emitter);
						Data->SetStringField(TEXT("emitter"), Emitter->GetPathName());
					}
					Package->MarkPackageDirty();
					Data->SetStringField(TEXT("message"),
						TEXT("created in memory — author it, then save"));
					Responder->Ok(Data);
					return;
				}

				// ------- everything below operates on an existing system -----
				FString SystemPath;
				if (!RequireString(Body, TEXT("system"), SystemPath, Responder,
						TEXT("a Niagara system asset path, e.g. /Game/FX/NS_Sparks")))
				{
					return;
				}
				UNiagaraSystem* System = Cast<UNiagaraSystem>(ResolveAsset(SystemPath));
				if (System == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("system_not_found"),
						FString::Printf(TEXT("no Niagara system at '%s' — see niagara_ops list_systems"),
							*SystemPath));
					return;
				}

				if (Operation == TEXT("stack"))
				{
					const bool bInputs = BoolOr(Body, TEXT("include_inputs"), true);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("system"), System->GetPathName());

					const auto StageJson = [&](const FStackTarget& Target)
					{
						TArray<TSharedPtr<FJsonValue>> Modules;
						int32 Index = 0;
						for (UNiagaraNodeFunctionCall* Call : OrderedModules(Target.OutputNode))
						{
							Modules.Add(MakeShared<FJsonValueObject>(
								ModuleToJson(Call, Index++, Target, bInputs)));
						}
						return Modules;
					};

					// System-level stacks.
					const TSharedRef<FJsonObject> SystemStacks = MakeShared<FJsonObject>();
					for (const FStage& Stage : Stages)
					{
						if (Stage.bEmitter)
						{
							continue;
						}
						FStackTarget Target;
						Target.System = System;
						Target.Usage = Stage.Usage;
						Target.Script = Stage.Usage == ENiagaraScriptUsage::SystemSpawnScript
							? System->GetSystemSpawnScript()
							: System->GetSystemUpdateScript();
						if (UNiagaraScriptSource* Source = SourceOf(Target.Script))
						{
							Target.OutputNode = Source->NodeGraph != nullptr
								? Source->NodeGraph->FindEquivalentOutputNode(Stage.Usage, FGuid())
								: nullptr;
						}
						SystemStacks->SetArrayField(Stage.Name, StageJson(Target));
					}
					Data->SetObjectField(TEXT("system_stacks"), SystemStacks);

					TArray<TSharedPtr<FJsonValue>> Emitters;
					for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Handle.GetName().ToString());
						Item->SetStringField(TEXT("id"), Handle.GetId().ToString());
						Item->SetBoolField(TEXT("enabled"), Handle.GetIsEnabled());
						FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
						if (EmitterData != nullptr)
						{
							const TSharedRef<FJsonObject> Stacks = MakeShared<FJsonObject>();
							for (const FStage& Stage : Stages)
							{
								if (!Stage.bEmitter)
								{
									continue;
								}
								FStackTarget Target;
								Target.System = System;
								Target.Emitter = Handle.GetInstance();
								Target.Usage = Stage.Usage;
								if (UNiagaraScriptSource* Source =
										Cast<UNiagaraScriptSource>(EmitterData->GraphSource))
								{
									Target.OutputNode = Source->NodeGraph != nullptr
										? Source->NodeGraph->FindEquivalentOutputNode(Stage.Usage, FGuid())
										: nullptr;
								}
								Stacks->SetArrayField(Stage.Name, StageJson(Target));
							}
							Item->SetObjectField(TEXT("stacks"), Stacks);

							TArray<TSharedPtr<FJsonValue>> Renderers;
							int32 RendererIndex = 0;
							for (UNiagaraRendererProperties* Renderer : EmitterData->GetRenderers())
							{
								if (Renderer != nullptr)
								{
									Renderers.Add(MakeShared<FJsonValueObject>(
										RendererToJson(Renderer, RendererIndex)));
								}
								++RendererIndex;
							}
							Item->SetArrayField(TEXT("renderers"), Renderers);
						}
						Emitters.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetArrayField(TEXT("emitters"), Emitters);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_emitter"))
				{
					FString EmitterPath;
					if (!RequireString(Body, TEXT("emitter_asset"), EmitterPath, Responder,
							TEXT("a Niagara emitter asset path; engine templates live under /Niagara")))
					{
						return;
					}
					UNiagaraEmitter* Source = Cast<UNiagaraEmitter>(ResolveAsset(EmitterPath));
					if (Source == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("emitter_not_found"),
							FString::Printf(TEXT("no Niagara emitter asset at '%s'"), *EmitterPath));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddNiagaraEmitter", "McpLink Add Niagara Emitter"));
					System->Modify();
					const FGuid Added = FNiagaraEditorUtilities::AddEmitterToSystem(*System, *Source,
						Source->GetExposedVersion().VersionGuid,
						BoolOr(Body, TEXT("create_copy"), true));
					FNiagaraEmitterHandle* Handle = nullptr;
					for (FNiagaraEmitterHandle& Candidate : System->GetEmitterHandles())
					{
						if (Candidate.GetId() == Added)
						{
							Handle = &Candidate;
						}
					}
					if (Handle == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("add_failed"),
							TEXT("Niagara did not add the emitter to the system"));
						return;
					}
					FString Name;
					if (Body->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
					{
						Handle->SetName(FName(*Name), *System);
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
					Data->SetStringField(TEXT("id"), Handle->GetId().ToString());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_emitter") || Operation == TEXT("rename_emitter")
					|| Operation == TEXT("set_emitter_enabled"))
				{
					FString EmitterSpec;
					if (!RequireString(Body, TEXT("emitter"), EmitterSpec, Responder,
							TEXT("the emitter name in the system")))
					{
						return;
					}
					FNiagaraEmitterHandle* Handle = FindHandle(System, EmitterSpec);
					if (Handle == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("emitter_not_found"),
							FString::Printf(TEXT("system '%s' has no emitter '%s'"),
								*System->GetName(), *EmitterSpec));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditNiagaraEmitter", "McpLink Edit Niagara Emitter"));
					System->Modify();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					if (Operation == TEXT("remove_emitter"))
					{
						Data->SetStringField(TEXT("removed"), Handle->GetName().ToString());
						System->RemoveEmitterHandle(*Handle);
					}
					else if (Operation == TEXT("rename_emitter"))
					{
						FString Name;
						if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the new emitter name")))
						{
							return;
						}
						Handle->SetName(FName(*Name), *System);
						Data->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
					}
					else
					{
						const bool bEnabled = BoolOr(Body, TEXT("enabled"), true);
						Handle->SetIsEnabled(bEnabled, *System, /*bRecompileIfChanged*/ false);
						Data->SetStringField(TEXT("emitter"), Handle->GetName().ToString());
						Data->SetBoolField(TEXT("enabled"), Handle->GetIsEnabled());
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_renderer") || Operation == TEXT("remove_renderer"))
				{
					FString EmitterSpec;
					if (!RequireString(Body, TEXT("emitter"), EmitterSpec, Responder,
							TEXT("the emitter name in the system")))
					{
						return;
					}
					FNiagaraEmitterHandle* Handle = FindHandle(System, EmitterSpec);
					FVersionedNiagaraEmitterData* EmitterData = Handle ? Handle->GetEmitterData() : nullptr;
					if (EmitterData == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("emitter_not_found"),
							FString::Printf(TEXT("system '%s' has no emitter '%s'"),
								*System->GetName(), *EmitterSpec));
						return;
					}
					const FVersionedNiagaraEmitter Versioned = Handle->GetInstance();
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditNiagaraRenderer", "McpLink Edit Niagara Renderer"));
					Versioned.Emitter->Modify();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					if (Operation == TEXT("add_renderer"))
					{
						FString ClassName;
						if (!RequireString(Body, TEXT("class"), ClassName, Responder,
								TEXT("e.g. NiagaraSpriteRendererProperties — see list_renderer_classes")))
						{
							return;
						}
						UClass* Class = ResolveClass(ClassName);
						if (Class == nullptr || !Class->IsChildOf(UNiagaraRendererProperties::StaticClass())
							|| Class->HasAnyClassFlags(CLASS_Abstract))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_class"),
								FString::Printf(
									TEXT("'%s' is not a concrete UNiagaraRendererProperties class — see ")
									TEXT("niagara_author list_renderer_classes"),
									*ClassName));
							return;
						}
						UNiagaraRendererProperties* Renderer = NewObject<UNiagaraRendererProperties>(
							Versioned.Emitter, Class, NAME_None, RF_Transactional);
						Versioned.Emitter->AddRenderer(Renderer, Versioned.Version);
						Data->SetObjectField(TEXT("renderer"),
							RendererToJson(Renderer, EmitterData->GetRenderers().Num() - 1));
					}
					else
					{
						const int32 Index = IntOr(Body, TEXT("index"), -1);
						const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
						if (!Renderers.IsValidIndex(Index))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_index"),
								FString::Printf(TEXT("'index' must be 0..%d for emitter '%s'"),
									Renderers.Num() - 1, *EmitterSpec));
							return;
						}
						UNiagaraRendererProperties* Renderer = Renderers[Index];
						Data->SetStringField(TEXT("removed"), Renderer->GetClass()->GetName());
						Versioned.Emitter->RemoveRenderer(Renderer, Versioned.Version);
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("compile") || Operation == TEXT("compile_status"))
				{
					if (Operation == TEXT("compile"))
					{
						System->RequestCompile(BoolOr(Body, TEXT("force"), false));
					}
					// PollForCompilationComplete's bool cannot be the answer: it
					// returns false both while compiling and when there was
					// nothing to compile. The per-script status is the real one.
					System->PollForCompilationComplete(true);

					TArray<TSharedPtr<FJsonValue>> Scripts;
					TArray<TSharedPtr<FJsonValue>> Errors;
					bool bAllUpToDate = true;
					const UEnum* StatusEnum = StaticEnum<ENiagaraScriptCompileStatus>();

					const auto Report = [&](UNiagaraScript* Script, const FString& Owner)
					{
						if (Script == nullptr)
						{
							return;
						}
						const FNiagaraVMExecutableData& Data = Script->GetVMExecutableData();
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("owner"), Owner);
						Item->SetStringField(TEXT("script"), Script->GetName());
						const FString Status = StatusEnum != nullptr
							? StatusEnum->GetNameStringByValue(static_cast<int64>(Data.LastCompileStatus))
							: FString::FromInt(static_cast<int32>(Data.LastCompileStatus));
						Item->SetStringField(TEXT("status"), Status);
						if (Data.LastCompileStatus != ENiagaraScriptCompileStatus::NCS_UpToDate)
						{
							bAllUpToDate = false;
						}
						for (const FNiagaraCompileEvent& Event : Data.LastCompileEvents)
						{
							if (Event.Severity != FNiagaraCompileEventSeverity::Error)
							{
								continue;
							}
							Errors.Add(MakeShared<FJsonValueString>(
								FString::Printf(TEXT("%s/%s: %s"), *Owner, *Script->GetName(), *Event.Message)));
						}
						Scripts.Add(MakeShared<FJsonValueObject>(Item));
					};

					Report(System->GetSystemSpawnScript(), TEXT("system"));
					Report(System->GetSystemUpdateScript(), TEXT("system"));
					for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
					{
						FVersionedNiagaraEmitterData* EmitterData = Handle.GetEmitterData();
						if (EmitterData == nullptr)
						{
							continue;
						}
						TArray<UNiagaraScript*> EmitterScripts;
						EmitterData->GetScripts(EmitterScripts, /*bCompilableOnly*/ true);
						for (UNiagaraScript* Script : EmitterScripts)
						{
							Report(Script, Handle.GetName().ToString());
						}
					}

					const bool bPending = System->HasOutstandingCompilationRequests();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("system"), System->GetPathName());
					Data->SetBoolField(TEXT("compiling"), bPending);
					Data->SetBoolField(TEXT("compiled"), !bPending && bAllUpToDate);
					Data->SetBoolField(TEXT("ready_to_run"), System->IsReadyToRun());
					Data->SetArrayField(TEXT("scripts"), Scripts);
					Data->SetArrayField(TEXT("errors"), Errors);
					if (bPending)
					{
						Data->SetStringField(TEXT("message"),
							TEXT("compilation is still running — poll with compile_status"));
					}
					else if (!FApp::CanEverRender())
					{
						// UNiagaraSystem::IsReadyToRunInternal returns false
						// outright when the app cannot render, so the flag says
						// nothing about this system in a headless editor.
						Data->SetStringField(TEXT("message"),
							TEXT("ready_to_run is always false in an editor without rendering (-nullrhi); ")
							TEXT("`compiled` and `errors` are the signal here"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(System, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("system"), System->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				// ------------- everything below targets one script stack -----
				FStackTarget Target;
				if (Operation == TEXT("add_module") || Operation == TEXT("remove_module")
					|| Operation == TEXT("set_module_enabled") || Operation == TEXT("set_module_input"))
				{
					if (!ResolveStack(System, Body, Responder, Target))
					{
						return;
					}
				}

				if (Operation == TEXT("add_module"))
				{
					FString ScriptPath;
					if (!RequireString(Body, TEXT("module"), ScriptPath, Responder,
							TEXT("a Niagara module script path — see niagara_author list_module_scripts")))
					{
						return;
					}
					UNiagaraScript* ModuleScript = Cast<UNiagaraScript>(ResolveAsset(ScriptPath));
					if (ModuleScript == nullptr || ModuleScript->GetUsage() != ENiagaraScriptUsage::Module)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("module_not_found"),
							FString::Printf(
								TEXT("'%s' is not a Niagara module script — see niagara_author ")
								TEXT("list_module_scripts"),
								*ScriptPath));
						return;
					}
					FString Name;
					Body->TryGetStringField(TEXT("name"), Name);
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddNiagaraModule", "McpLink Add Niagara Module"));
					System->Modify();
					UNiagaraNodeFunctionCall* Added = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
						ModuleScript, *Target.OutputNode, IntOr(Body, TEXT("index"), INDEX_NONE), Name);
					if (Added == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("add_failed"),
							FString::Printf(TEXT("Niagara refused to add '%s' to this stack"), *ScriptPath));
						return;
					}
					const TArray<UNiagaraNodeFunctionCall*> Modules = OrderedModules(Target.OutputNode);
					Responder->Ok(ModuleToJson(Added, Modules.IndexOfByKey(Added), Target, true));
					return;
				}

				if (Operation == TEXT("remove_module") || Operation == TEXT("set_module_enabled")
					|| Operation == TEXT("set_module_input"))
				{
					FString ModuleSpec;
					if (!RequireString(Body, TEXT("module"), ModuleSpec, Responder,
							TEXT("a module id or name from niagara_author stack")))
					{
						return;
					}
					UNiagaraNodeFunctionCall* Call = nullptr;
					const TArray<UNiagaraNodeFunctionCall*> Modules = OrderedModules(Target.OutputNode);
					for (UNiagaraNodeFunctionCall* Candidate : Modules)
					{
						if (Candidate->NodeGuid.ToString() == ModuleSpec
							|| Candidate->GetFunctionName().Equals(ModuleSpec, ESearchCase::IgnoreCase))
						{
							Call = Candidate;
							break;
						}
					}
					if (Call == nullptr)
					{
						TArray<FString> Names;
						for (UNiagaraNodeFunctionCall* Candidate : Modules)
						{
							Names.Add(Candidate->GetFunctionName());
						}
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("module_not_found"),
							FString::Printf(TEXT("no module '%s' in this stack — it holds: %s"),
								*ModuleSpec,
								Names.IsEmpty() ? TEXT("(none)") : *FString::Join(Names, TEXT(", "))));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditNiagaraModule", "McpLink Edit Niagara Module"));
					System->Modify();

					if (Operation == TEXT("set_module_enabled"))
					{
						FNiagaraStackGraphUtilities::SetModuleIsEnabled(
							*Call, BoolOr(Body, TEXT("enabled"), true));
						Responder->Ok(ModuleToJson(Call, Modules.IndexOfByKey(Call), Target, false));
						return;
					}

					if (Operation == TEXT("remove_module"))
					{
						const FString Name = Call->GetFunctionName();
						FString Error;
						if (!DetachModule(Call, Error))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("remove_failed"), Error);
							return;
						}
						Target.Script->MarkScriptAndSourceDesynchronized(
							TEXT("McpLink removed a module"), FGuid());
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("removed"), Name);
						Data->SetStringField(TEXT("message"),
							TEXT("stack relinked — run compile to confirm the system still builds"));
						Responder->Ok(Data);
						return;
					}

					// set_module_input
					FString InputName;
					if (!RequireString(Body, TEXT("input"), InputName, Responder,
							TEXT("an input name from niagara_author stack, e.g. SpawnRate")))
					{
						return;
					}
					const TSharedPtr<FJsonValue> Value = Body->TryGetField(TEXT("value"));
					if (!Value.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'value' is required"));
						return;
					}

					TArray<FNiagaraVariable> Inputs;
					FNiagaraStackGraphUtilities::GetStackFunctionInputs(*Call, Inputs, Target.Resolver(),
						FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
						/*bIgnoreDisabled*/ false);
					const FNiagaraVariable* Input = Inputs.FindByPredicate(
						[&InputName](const FNiagaraVariable& Candidate)
						{
							FString Name = Candidate.GetName().ToString();
							Name.RemoveFromStart(TEXT("Module."));
							return Name.Equals(InputName, ESearchCase::IgnoreCase);
						});
					if (Input == nullptr)
					{
						TArray<FString> Names;
						for (const FNiagaraVariable& Candidate : Inputs)
						{
							FString Name = Candidate.GetName().ToString();
							Name.RemoveFromStart(TEXT("Module."));
							Names.Add(Name);
						}
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("input_not_found"),
							FString::Printf(TEXT("module '%s' has no input '%s' — it takes: %s"),
								*Call->GetFunctionName(), *InputName,
								Names.IsEmpty() ? TEXT("(none)") : *FString::Join(Names, TEXT(", "))));
						return;
					}

					TArray<uint8> Bytes;
					FString Error;
					if (!ValueToBytes(Input->GetType(), Value, Bytes, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"),
							FString::Printf(TEXT("'%s': %s"), *InputName, *Error));
						return;
					}

					FNiagaraStackFunctionInputBinder Binder;
					FText BindError;
					FString UnaliasedName = Input->GetName().ToString();
					UnaliasedName.RemoveFromStart(TEXT("Module."));
					if (!Binder.TryBind(Target.Script, {}, Target.Resolver(), Target.EmitterName(), Call,
							FName(*UnaliasedName), Input->GetType(), /*bIsRequired*/ true, BindError))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("bind_failed"),
							FString::Printf(TEXT("could not bind input '%s': %s"),
								*InputName, *BindError.ToString()));
						return;
					}
					Binder.SetData(Bytes.GetData(), Bytes.Num());

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("module"), Call->GetFunctionName());
					Data->SetStringField(TEXT("input"), InputName);
					Data->SetStringField(TEXT("type"), Input->GetType().GetName());
					Data->SetField(TEXT("value"), Value);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_stages, list_module_scripts, ")
						TEXT("list_renderer_classes, create_system, create_emitter, stack, add_emitter, ")
						TEXT("remove_emitter, rename_emitter, set_emitter_enabled, add_module, ")
						TEXT("remove_module, set_module_enabled, set_module_input, add_renderer, ")
						TEXT("remove_renderer, compile, compile_status, or save"),
						*Operation));
			});
	}
}
