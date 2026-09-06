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
// UNiagaraEmitter::AddRenderer). Module removal has no exported entry point,
// so it is done by relinking the parameter-map chain the way the engine does,
// and the route recompiles afterwards so a bad edit surfaces immediately.
//
// Module inputs are read and written through the Niagara editor's own stack
// view model (FNiagaraSystemViewModel + UNiagaraStackFunctionInput), built
// the way the engine builds one for its commandlets: data-processing only,
// so there is no preview component, no sequencer and no Slate involved. That
// is what gives every input the same vocabulary the stack panel has — a local
// value, a linked parameter, a dynamic input with nested inputs of its own,
// a data interface, an object asset or an HLSL expression — and lets an edit
// replace one with another exactly as typing into the panel would. The older
// FNiagaraStackFunctionInputBinder path could only write a local value, and
// refused any input a template had already handed to a dynamic input.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_Niagara.h"
#include "JsonObjectConverter.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraParameterMapHistory.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectIterator.h"
#include "ViewModels/NiagaraEmitterHandleViewModel.h"
#include "ViewModels/NiagaraSystemViewModel.h"
#include "ViewModels/Stack/NiagaraParameterHandle.h"
#include "ViewModels/Stack/NiagaraStackEntry.h"
#include "ViewModels/Stack/NiagaraStackFunctionInput.h"
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"
#include "ViewModels/Stack/NiagaraStackModuleItem.h"
#include "ViewModels/Stack/NiagaraStackModuleItemLinkedInputCollection.h"
#include "ViewModels/Stack/NiagaraStackModuleItemOutputCollection.h"
#include "ViewModels/Stack/NiagaraStackViewModel.h"

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

		// ------------------------------------------------------- values ---

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

		/// The name a type shows in the editor ("float", "Vector", "Static bool")
		/// rather than its struct name ("NiagaraFloat", "Vector3f").
		FString TypeName(const FNiagaraTypeDefinition& Type)
		{
			return Type.IsValid() ? Type.GetNameText().ToString() : FString();
		}

		bool TypeNameMatches(const FNiagaraTypeDefinition& Type, const FString& Name)
		{
			return Type.IsValid()
				&& (TypeName(Type).Equals(Name, ESearchCase::IgnoreCase)
					|| Type.GetName().Equals(Name, ESearchCase::IgnoreCase));
		}

		/// The names an enum-typed input accepts, in declaration order, without
		/// the trailing _MAX and anything the enum hides from the editor. These
		/// are display names: Niagara's own enums are user-defined enums whose
		/// entries are literally named NewEnumerator0, NewEnumerator1, ..., and
		/// the display name is the only thing the editor ever shows.
		TArray<FString> EnumOptions(const FNiagaraTypeDefinition& Type)
		{
			TArray<FString> Options;
			const UEnum* Enum = Type.GetEnum();
			if (Enum == nullptr)
			{
				return Options;
			}
			const int32 Count = Enum->ContainsExistingMax() ? Enum->NumEnums() - 1 : Enum->NumEnums();
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (!Enum->HasMetaData(TEXT("Hidden"), Index))
				{
					Options.Add(Enum->GetDisplayNameTextByIndex(Index).ToString());
				}
			}
			return Options;
		}

		/// An enum entry by display name, raw name ("NewEnumerator2",
		/// "EMyEnum::Two") or numeric value. INDEX_NONE when nothing matches.
		int64 EnumValueFor(const FNiagaraTypeDefinition& Type, const FString& Text)
		{
			const UEnum* Enum = Type.GetEnum();
			if (Enum == nullptr)
			{
				return INDEX_NONE;
			}
			for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
			{
				if (Enum->GetDisplayNameTextByIndex(Index).ToString().Equals(Text, ESearchCase::IgnoreCase)
					|| Enum->GetNameStringByIndex(Index).Equals(Text, ESearchCase::IgnoreCase))
				{
					return Enum->GetValueByIndex(Index);
				}
			}
			const int64 ByQualifiedName = Enum->GetValueByNameString(Text);
			if (ByQualifiedName != INDEX_NONE)
			{
				return ByQualifiedName;
			}
			return Text.IsNumeric() ? FCString::Atoi64(*Text) : INDEX_NONE;
		}

		/// JSON value -> the raw bytes a Niagara parameter of `Type` expects.
		///
		/// Static switch types carry a flag on top of the base type, so the
		/// comparisons ignore flags and match "static float" as float.
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
			const auto Is = [&Type](const FNiagaraTypeDefinition& Other)
			{
				return Type.IsSameBaseDefinition(Other);
			};
			double Number = 0.0;
			bool Flag = false;
			FString Text;
			TArray<double> Parts;
			if (!Value.IsValid())
			{
				OutError = TEXT("no value given");
				return false;
			}
			if (Type.IsEnum())
			{
				int64 EnumValue = INDEX_NONE;
				if (Value->TryGetString(Text))
				{
					EnumValue = EnumValueFor(Type, Text);
					if (EnumValue == INDEX_NONE)
					{
						OutError = FString::Printf(TEXT("'%s' is not a value of %s — it takes: %s"),
							*Text, *TypeName(Type), *FString::Join(EnumOptions(Type), TEXT(", ")));
						return false;
					}
				}
				else if (Value->TryGetNumber(Number))
				{
					EnumValue = static_cast<int64>(Number);
				}
				else
				{
					OutError = FString::Printf(TEXT("input is the enum %s — pass one of its names: %s"),
						*TypeName(Type), *FString::Join(EnumOptions(Type), TEXT(", ")));
					return false;
				}
				const int32 Int = static_cast<int32>(EnumValue);
				Append(&Int, sizeof(Int));
			}
			else if (Is(FNiagaraTypeDefinition::GetFloatDef()) && Value->TryGetNumber(Number))
			{
				const float Float = static_cast<float>(Number);
				Append(&Float, sizeof(Float));
			}
			else if (Is(FNiagaraTypeDefinition::GetIntDef()) && Value->TryGetNumber(Number))
			{
				const int32 Int = static_cast<int32>(Number);
				Append(&Int, sizeof(Int));
			}
			else if (Is(FNiagaraTypeDefinition::GetBoolDef()) && Value->TryGetBool(Flag))
			{
				// A Niagara bool is a 4-byte FNiagaraBool, not a C++ bool.
				const FNiagaraBool Bool(Flag);
				Append(&Bool, sizeof(Bool));
			}
			else if (Is(FNiagaraTypeDefinition::GetVec2Def()) && ReadNumberArray(Value, 2, Parts))
			{
				const FVector2f Vector(Parts[0], Parts[1]);
				Append(&Vector, sizeof(Vector));
			}
			else if ((Is(FNiagaraTypeDefinition::GetVec3Def()) || Is(FNiagaraTypeDefinition::GetPositionDef()))
				&& ReadNumberArray(Value, 3, Parts))
			{
				const FVector3f Vector(Parts[0], Parts[1], Parts[2]);
				Append(&Vector, sizeof(Vector));
			}
			else if ((Is(FNiagaraTypeDefinition::GetVec4Def()) || Is(FNiagaraTypeDefinition::GetColorDef())
						 || Is(FNiagaraTypeDefinition::GetQuatDef()))
				&& ReadNumberArray(Value, 4, Parts))
			{
				const FVector4f Vector(Parts[0], Parts[1], Parts[2], Parts[3]);
				Append(&Vector, sizeof(Vector));
			}
			else if (Is(FNiagaraTypeDefinition::GetColorDef()) && ReadNumberArray(Value, 3, Parts))
			{
				const FVector4f Vector(Parts[0], Parts[1], Parts[2], 1.0f);
				Append(&Vector, sizeof(Vector));
			}
			else
			{
				OutError = FString::Printf(
					TEXT("input is a %s — pass a number (float/int), bool, an enum name, or a [x,y] / ")
					TEXT("[x,y,z] / [x,y,z,w] array; data interfaces take `data_interface`, object ")
					TEXT("inputs take `object_asset`, and other structs have no literal form here"),
					*TypeName(Type));
				return false;
			}
			if (Out.Num() != Type.GetSize())
			{
				OutError = FString::Printf(TEXT("value is %d bytes but %s wants %d"),
					Out.Num(), *TypeName(Type), Type.GetSize());
				return false;
			}
			return true;
		}

		/// The raw bytes of a Niagara value of `Type` -> JSON, the inverse of
		/// ValueToBytes. Types with no literal form come back as null.
		TSharedPtr<FJsonValue> BytesToJsonValue(const FNiagaraTypeDefinition& Type, const uint8* Data)
		{
			if (Data == nullptr)
			{
				return MakeShared<FJsonValueNull>();
			}
			const auto Is = [&Type](const FNiagaraTypeDefinition& Other)
			{
				return Type.IsSameBaseDefinition(Other);
			};
			// A float widened to JSON's double shows its representation error
			// (0.1f prints as 0.10000000149011612); round-tripping through the
			// shortest text that reads back as the same float hides it.
			const auto Number = [](float Value)
			{
				double Rounded = Value;
				for (int32 Digits = 6; Digits <= 9; ++Digits)
				{
					Rounded = FCString::Atod(*FString::Printf(TEXT("%.*g"), Digits, Value));
					if (static_cast<float>(Rounded) == Value)
					{
						break;
					}
				}
				return MakeShared<FJsonValueNumber>(Rounded);
			};
			const auto Floats = [Data, &Number](int32 Count)
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				const float* Values = reinterpret_cast<const float*>(Data);
				for (int32 Index = 0; Index < Count; ++Index)
				{
					Items.Add(Number(Values[Index]));
				}
				return MakeShared<FJsonValueArray>(Items);
			};
			if (Type.IsEnum())
			{
				int32 Int = 0;
				FMemory::Memcpy(&Int, Data, sizeof(Int));
				if (const UEnum* Enum = Type.GetEnum())
				{
					const int32 Index = Enum->GetIndexByValue(Int);
					if (Index != INDEX_NONE)
					{
						return MakeShared<FJsonValueString>(Enum->GetDisplayNameTextByIndex(Index).ToString());
					}
				}
				return MakeShared<FJsonValueNumber>(Int);
			}
			if (Is(FNiagaraTypeDefinition::GetFloatDef()))
			{
				float Float = 0.0f;
				FMemory::Memcpy(&Float, Data, sizeof(Float));
				return Number(Float);
			}
			if (Is(FNiagaraTypeDefinition::GetIntDef()))
			{
				int32 Int = 0;
				FMemory::Memcpy(&Int, Data, sizeof(Int));
				return MakeShared<FJsonValueNumber>(Int);
			}
			if (Is(FNiagaraTypeDefinition::GetBoolDef()))
			{
				FNiagaraBool Bool;
				FMemory::Memcpy(&Bool, Data, sizeof(Bool));
				return MakeShared<FJsonValueBoolean>(Bool.GetValue());
			}
			if (Is(FNiagaraTypeDefinition::GetVec2Def()))
			{
				return Floats(2);
			}
			if (Is(FNiagaraTypeDefinition::GetVec3Def()) || Is(FNiagaraTypeDefinition::GetPositionDef()))
			{
				return Floats(3);
			}
			if (Is(FNiagaraTypeDefinition::GetVec4Def()) || Is(FNiagaraTypeDefinition::GetColorDef())
				|| Is(FNiagaraTypeDefinition::GetQuatDef()))
			{
				return Floats(4);
			}
			return MakeShared<FJsonValueNull>();
		}

		/// The type a dynamic input script produces: the one non-map pin on
		/// its single output node. Invalid when the script is not shaped like
		/// a dynamic input at all.
		FNiagaraTypeDefinition DynamicInputOutputType(UNiagaraScript* Script)
		{
			UNiagaraScriptSource* Source = SourceOf(Script);
			if (Source == nullptr || Source->NodeGraph == nullptr)
			{
				return FNiagaraTypeDefinition();
			}
			TArray<UNiagaraNodeOutput*> OutputNodes;
			Source->NodeGraph->GetNodesOfClass<UNiagaraNodeOutput>(OutputNodes);
			if (OutputNodes.Num() != 1)
			{
				return FNiagaraTypeDefinition();
			}
			const UEdGraphPin* ValuePin = nullptr;
			for (const UEdGraphPin* Pin : OutputNodes[0]->Pins)
			{
				if (Pin != nullptr && Pin->Direction == EGPD_Input && !IsParameterMapPin(Pin))
				{
					if (ValuePin != nullptr)
					{
						return FNiagaraTypeDefinition();
					}
					ValuePin = Pin;
				}
			}
			return ValuePin != nullptr ? UEdGraphSchema_Niagara::PinToTypeDefinition(ValuePin)
									   : FNiagaraTypeDefinition();
		}

		// --------------------------------------------------- stack view ---

		/// The Niagara editor's stack view model over one system, built the way
		/// the engine builds one for its audit commandlet and its user-parameter
		/// helpers: data-processing only, so no preview component, no sequencer,
		/// no undo registration and nothing drawn. It lives for one request.
		///
		/// Underneath it every module is a UNiagaraStackModuleItem whose
		/// descendants include a UNiagaraStackFunctionInput per input — the
		/// module's own inputs and, nested beneath any input handed to a
		/// dynamic input, that dynamic input's inputs, recursively. Ownership
		/// is by function-call node: an input entry belongs to the module node
		/// or to a dynamic input node, which is how the tree is rebuilt here.
		struct FStackView
		{
			TSharedPtr<FNiagaraSystemViewModel> ViewModel;

			explicit FStackView(UNiagaraSystem& System)
			{
				FNiagaraSystemViewModelOptions Options;
				Options.bCanModifyEmittersFromTimeline = false;
				Options.bCanAutoCompile = false;
				Options.bCanSimulate = false;
				Options.bCompileForEdit = false;
				Options.bIsForDataProcessingOnly = true;
				Options.EditMode = ENiagaraSystemViewModelEditMode::SystemAsset;
				Options.MessageLogGuid = System.GetAssetGuid();
				ViewModel = MakeShared<FNiagaraSystemViewModel>();
				ViewModel->Initialize(System, Options);
			}

			UNiagaraStackViewModel* StackFor(const FStackTarget& Target) const
			{
				if (Target.Emitter.Emitter == nullptr)
				{
					return ViewModel->GetSystemStackViewModel();
				}
				const TSharedPtr<FNiagaraEmitterHandleViewModel> Handle =
					ViewModel->GetEmitterHandleViewModelForEmitter(Target.Emitter);
				return Handle.IsValid() ? Handle->GetEmitterStackViewModel() : nullptr;
			}

			UNiagaraStackModuleItem* ModuleItem(const FStackTarget& Target, const UNiagaraNodeFunctionCall* Node) const
			{
				UNiagaraStackViewModel* Stack = StackFor(Target);
				UNiagaraStackEntry* Root = Stack != nullptr ? Stack->GetRootEntry() : nullptr;
				if (Root == nullptr)
				{
					return nullptr;
				}
				TArray<UNiagaraStackModuleItem*> Items;
				Root->GetUnfilteredChildrenOfType<UNiagaraStackModuleItem>(Items, /*bRecursive*/ true);
				for (UNiagaraStackModuleItem* Item : Items)
				{
					if (Item != nullptr && &Item->GetModuleNode() == Node)
					{
						return Item;
					}
				}
				return nullptr;
			}
		};

		/// Every input entry under a stack entry, at any depth. A module item
		/// also lists the inputs elsewhere that read its outputs and its own
		/// outputs; neither is an input of this module, so those subtrees are
		/// skipped.
		void CollectInputs(const UNiagaraStackEntry& Entry, TArray<UNiagaraStackFunctionInput*>& Out)
		{
			TArray<UNiagaraStackEntry*> Children;
			Entry.GetUnfilteredChildren(Children);
			for (UNiagaraStackEntry* Child : Children)
			{
				if (Child == nullptr || Child->IsA<UNiagaraStackModuleItemLinkedInputCollection>()
					|| Child->IsA<UNiagaraStackModuleItemOutputCollection>())
				{
					continue;
				}
				if (UNiagaraStackFunctionInput* Input = Cast<UNiagaraStackFunctionInput>(Child))
				{
					Out.AddUnique(Input);
				}
				CollectInputs(*Child, Out);
			}
		}

		/// The inputs a function call (a module, or a dynamic input) exposes,
		/// in stack order.
		TArray<UNiagaraStackFunctionInput*> InputsOf(
			const TArray<UNiagaraStackFunctionInput*>& All, const UNiagaraNodeFunctionCall* Node)
		{
			TArray<UNiagaraStackFunctionInput*> Result;
			for (UNiagaraStackFunctionInput* Input : All)
			{
				if (Input->GetInputFunctionCallNodePtr() == Node)
				{
					Result.Add(Input);
				}
			}
			return Result;
		}

		FString InputName(const UNiagaraStackFunctionInput* Input)
		{
			return Input->GetInputParameterHandle().GetName().ToString();
		}

		FString InputNames(const TArray<UNiagaraStackFunctionInput*>& Inputs)
		{
			TArray<FString> Names;
			for (const UNiagaraStackFunctionInput* Input : Inputs)
			{
				Names.Add(InputName(Input));
			}
			return Names.IsEmpty() ? FString(TEXT("(none)")) : FString::Join(Names, TEXT(", "));
		}

		/// Resolve an input path — "SpawnRate", or "Drag/Minimum" for the
		/// Minimum input of the dynamic input driving Drag, to any depth —
		/// against a module's inputs. Fills OutError and returns null when a
		/// segment is missing or sits on an input that is not a dynamic input.
		UNiagaraStackFunctionInput* ResolveInputPath(
			const TArray<UNiagaraStackFunctionInput*>& All,
			UNiagaraNodeFunctionCall* ModuleNode,
			const FString& Path,
			FString& OutError)
		{
			TArray<FString> Segments;
			Path.ParseIntoArray(Segments, TEXT("/"), /*bCullEmpty*/ true);
			if (Segments.IsEmpty())
			{
				OutError = TEXT("'input' is empty");
				return nullptr;
			}
			UNiagaraNodeFunctionCall* Node = ModuleNode;
			UNiagaraStackFunctionInput* Current = nullptr;
			FString Walked;
			for (int32 Index = 0; Index < Segments.Num(); ++Index)
			{
				const FString Segment = Segments[Index].TrimStartAndEnd();
				const TArray<UNiagaraStackFunctionInput*> Candidates = InputsOf(All, Node);
				UNiagaraStackFunctionInput* Match = nullptr;
				for (UNiagaraStackFunctionInput* Candidate : Candidates)
				{
					if (InputName(Candidate).Equals(Segment, ESearchCase::IgnoreCase)
						|| Candidate->GetDisplayName().ToString().Equals(Segment, ESearchCase::IgnoreCase))
					{
						Match = Candidate;
						break;
					}
				}
				if (Match == nullptr)
				{
					OutError = FString::Printf(TEXT("no input '%s' under %s — it has: %s"), *Segment,
						Walked.IsEmpty() ? *FString::Printf(TEXT("module '%s'"), *ModuleNode->GetFunctionName())
										 : *FString::Printf(TEXT("'%s'"), *Walked),
						*InputNames(Candidates));
					return nullptr;
				}
				Walked = Walked.IsEmpty() ? InputName(Match) : Walked + TEXT("/") + InputName(Match);
				Current = Match;
				if (Index + 1 < Segments.Num())
				{
					Node = Match->GetDynamicInputNode();
					if (Node == nullptr)
					{
						OutError = FString::Printf(
							TEXT("'%s' is not driven by a dynamic input, so it has no nested inputs — ")
							TEXT("set `dynamic_input` on it first, or set its `value` directly"),
							*Walked);
						return nullptr;
					}
				}
			}
			return Current;
		}

		/// Whether the stack panel would show this input: inputs behind an
		/// unmet visible condition (usually another branch of a static switch)
		/// exist but are hidden.
		bool IsVisibleInput(const UNiagaraStackFunctionInput* Input)
		{
			return Input->GetShouldPassFilterForVisibleCondition() && !Input->GetIsHidden();
		}

		const TCHAR* ModeName(UNiagaraStackFunctionInput::EValueMode Mode)
		{
			using EValueMode = UNiagaraStackFunctionInput::EValueMode;
			switch (Mode)
			{
				case EValueMode::Local: return TEXT("local");
				case EValueMode::Linked: return TEXT("linked");
				case EValueMode::LinkedShared: return TEXT("linked_shared");
				case EValueMode::Dynamic: return TEXT("dynamic");
				case EValueMode::Data: return TEXT("data_interface");
				case EValueMode::ObjectAsset: return TEXT("object_asset");
				case EValueMode::Expression: return TEXT("expression");
				case EValueMode::DefaultFunction: return TEXT("default_function");
				case EValueMode::InvalidOverride: return TEXT("invalid_override");
				case EValueMode::UnsupportedDefault: return TEXT("unsupported_default");
				default: return TEXT("none");
			}
		}

		/// The data interface an input's override actually compiles with. The
		/// view model hands out a transient placeholder it syncs from, so the
		/// asset-side object is read off the override graph: the override
		/// node's pin for this input links to a UNiagaraNodeInput carrying it.
		/// Null while the input still uses the module's default.
		UNiagaraDataInterface* OverrideDataInterface(UNiagaraStackFunctionInput* Input)
		{
			UNiagaraNodeFunctionCall* Call = Input->GetInputFunctionCallNodePtr();
			UEdGraphPin* MapInput = MapPin(Call, EGPD_Input);
			UEdGraphNode* Upstream = (MapInput != nullptr && !MapInput->LinkedTo.IsEmpty() && MapInput->LinkedTo[0])
				? MapInput->LinkedTo[0]->GetOwningNode()
				: nullptr;
			if (!IsOverrideNode(Upstream))
			{
				return nullptr;
			}
			const FName PinName = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
				Input->GetInputParameterHandle(), Call).GetParameterHandleString();
			for (const UEdGraphPin* Pin : Upstream->Pins)
			{
				if (Pin == nullptr || Pin->Direction != EGPD_Input || Pin->PinName != PinName
					|| Pin->LinkedTo.Num() != 1 || Pin->LinkedTo[0] == nullptr)
				{
					continue;
				}
				UNiagaraNodeInput* InputNode = Cast<UNiagaraNodeInput>(Pin->LinkedTo[0]->GetOwningNode());
				if (InputNode == nullptr)
				{
					return nullptr;
				}
				// UNiagaraNodeInput is MinimalAPI, so its accessor is not
				// callable from here; the property is.
				const FObjectProperty* Property =
					FindFProperty<FObjectProperty>(UNiagaraNodeInput::StaticClass(), TEXT("DataInterface"));
				return Property != nullptr
					? Cast<UNiagaraDataInterface>(Property->GetObjectPropertyValue_InContainer(InputNode))
					: nullptr;
			}
			return nullptr;
		}

		/// One input as JSON, with the inputs of the dynamic input driving it
		/// (if any) nested beneath. Hidden inputs are left out of the nesting
		/// unless asked for; the input itself is always reported.
		TSharedRef<FJsonObject> InputToJson(
			UNiagaraStackFunctionInput* Input,
			const TArray<UNiagaraStackFunctionInput*>& All,
			const FString& ParentPath,
			bool bIncludeHidden)
		{
			using EValueMode = UNiagaraStackFunctionInput::EValueMode;
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			const FString Name = InputName(Input);
			const FString Path = ParentPath.IsEmpty() ? Name : ParentPath + TEXT("/") + Name;
			const FNiagaraTypeDefinition& Type = Input->GetInputType();
			Object->SetStringField(TEXT("path"), Path);
			Object->SetStringField(TEXT("name"), Name);
			const FString DisplayName = Input->GetDisplayName().ToString();
			if (!DisplayName.Equals(Name))
			{
				Object->SetStringField(TEXT("display_name"), DisplayName);
			}
			Object->SetStringField(TEXT("type"), TypeName(Type));
			const EValueMode Mode = Input->GetValueMode();
			Object->SetStringField(TEXT("mode"), ModeName(Mode));

			switch (Mode)
			{
				case EValueMode::Local:
				{
					const TSharedPtr<const FStructOnScope> Local = Input->GetLocalValueStruct();
					Object->SetField(TEXT("value"),
						BytesToJsonValue(Type, Local.IsValid() ? Local->GetStructMemory() : nullptr));
					break;
				}
				case EValueMode::Linked:
				case EValueMode::LinkedShared:
				{
					const FNiagaraVariableBase& Linked = Input->GetLinkedParameterValue();
					Object->SetStringField(TEXT("linked_parameter"), Linked.GetName().ToString());
					if (Linked.GetType().IsValid() && !Linked.GetType().IsSameBaseDefinition(Type))
					{
						Object->SetStringField(TEXT("linked_type"), TypeName(Linked.GetType()));
					}
					break;
				}
				case EValueMode::Dynamic:
				{
					const TSharedRef<FJsonObject> Dynamic = MakeShared<FJsonObject>();
					if (UNiagaraNodeFunctionCall* Node = Input->GetDynamicInputNode())
					{
						Dynamic->SetStringField(TEXT("name"), Node->GetFunctionName());
						Dynamic->SetStringField(TEXT("script"),
							Node->FunctionScript != nullptr ? Node->FunctionScript->GetPathName() : FString());
						TArray<TSharedPtr<FJsonValue>> Children;
						for (UNiagaraStackFunctionInput* Child : InputsOf(All, Node))
						{
							if (bIncludeHidden || IsVisibleInput(Child))
							{
								Children.Add(MakeShared<FJsonValueObject>(
									InputToJson(Child, All, Path, bIncludeHidden)));
							}
						}
						Dynamic->SetArrayField(TEXT("inputs"), Children);
					}
					Object->SetObjectField(TEXT("dynamic_input"), Dynamic);
					break;
				}
				case EValueMode::Data:
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					UNiagaraDataInterface* Override = OverrideDataInterface(Input);
					UNiagaraDataInterface* Shown = Override != nullptr ? Override : Input->GetDataValueObject();
					if (Shown != nullptr)
					{
						Data->SetStringField(TEXT("class"), Shown->GetClass()->GetName());
						Data->SetStringField(TEXT("path"), Shown->GetPathName());
						// The editable properties, named as `properties` on
						// set_module_input takes them back. That is the write path:
						// the object may be a transient placeholder, and an
						// override's name can carry a '.', which no object path
						// can address.
						const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
						FJsonObjectConverter::UStructToJsonObject(Shown->GetClass(), Shown, Properties, CPF_Edit,
							CPF_InstancedReference | CPF_Transient | CPF_Deprecated, nullptr,
							EJsonObjectConversionFlags::SkipStandardizeCase);
						Data->SetObjectField(TEXT("properties"), Properties);
					}
					Data->SetBoolField(TEXT("overridden"), Override != nullptr);
					Object->SetObjectField(TEXT("data_interface"), Data);
					break;
				}
				case EValueMode::ObjectAsset:
				{
					UObject* Asset = Input->GetObjectAssetValue();
					Object->SetStringField(TEXT("object_asset"), Asset != nullptr ? Asset->GetPathName() : FString());
					break;
				}
				case EValueMode::Expression:
					Object->SetStringField(TEXT("expression"), Input->GetCustomExpressionText().ToString());
					break;
				case EValueMode::DefaultFunction:
				{
					UNiagaraNodeFunctionCall* Node = Input->GetDefaultFunctionNode();
					Object->SetStringField(TEXT("default_function"),
						Node != nullptr ? Node->GetFunctionName() : FString());
					break;
				}
				default:
					break;
			}

			if (Type.IsEnum())
			{
				TArray<TSharedPtr<FJsonValue>> Options;
				for (const FString& Option : EnumOptions(Type))
				{
					Options.Add(MakeShared<FJsonValueString>(Option));
				}
				Object->SetArrayField(TEXT("enum_options"), Options);
			}
			if (Input->IsStaticParameter())
			{
				Object->SetBoolField(TEXT("static"), true);
			}
			Object->SetBoolField(TEXT("can_reset"), Input->CanReset());
			if (Input->GetHasEditCondition())
			{
				Object->SetBoolField(TEXT("edit_condition_enabled"), Input->GetEditConditionEnabled());
			}
			if (Input->GetIsInlineEditConditionToggle())
			{
				Object->SetBoolField(TEXT("inline_edit_condition_toggle"), true);
			}
			if (!IsVisibleInput(Input))
			{
				Object->SetBoolField(TEXT("visible"), false);
			}
			if (Input->GetIsAdvanced())
			{
				Object->SetBoolField(TEXT("advanced"), true);
			}
			if (!Input->GetIsEnabled())
			{
				Object->SetBoolField(TEXT("enabled"), false);
			}
			return Object;
		}

		TArray<TSharedPtr<FJsonValue>> InputsToJson(
			const TArray<UNiagaraStackFunctionInput*>& Inputs,
			const TArray<UNiagaraStackFunctionInput*>& All,
			bool bIncludeHidden)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (UNiagaraStackFunctionInput* Input : Inputs)
			{
				if (bIncludeHidden || IsVisibleInput(Input))
				{
					Items.Add(MakeShared<FJsonValueObject>(InputToJson(Input, All, FString(), bIncludeHidden)));
				}
			}
			return Items;
		}

		// ------------------------------------------------------------- JSON ---

		TSharedRef<FJsonObject> ModuleToJson(
			UNiagaraNodeFunctionCall* Call,
			int32 Index,
			const FStackTarget& Target,
			bool bIncludeInputs,
			const FStackView* View,
			bool bIncludeHidden = false)
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
			if (!bIncludeInputs)
			{
				return Object;
			}
			UNiagaraStackModuleItem* Item = View != nullptr ? View->ModuleItem(Target, Call) : nullptr;
			if (Item != nullptr)
			{
				TArray<UNiagaraStackFunctionInput*> All;
				CollectInputs(*Item, All);
				Object->SetArrayField(TEXT("inputs"), InputsToJson(InputsOf(All, Call), All, bIncludeHidden));
				return Object;
			}
			// No stack entry for this node (a module the view model does not
			// show): fall back to the compiler's view — names and types only.
			TArray<FNiagaraVariable> Inputs;
			FNiagaraStackGraphUtilities::GetStackFunctionInputs(*Call, Inputs, Target.Resolver(),
				FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly,
				/*bIgnoreDisabled*/ false);
			TArray<TSharedPtr<FJsonValue>> InputJson;
			for (const FNiagaraVariable& Input : Inputs)
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				// Inputs are namespaced "Module.<Name>" inside the script.
				FString Name = Input.GetName().ToString();
				Name.RemoveFromStart(TEXT("Module."));
				Entry->SetStringField(TEXT("path"), Name);
				Entry->SetStringField(TEXT("name"), Name);
				Entry->SetStringField(TEXT("type"), TypeName(Input.GetType()));
				InputJson.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Object->SetArrayField(TEXT("inputs"), InputJson);
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

		TSharedRef<FJsonObject> ScriptToJson(UNiagaraScript* Script)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Script->GetName());
			Object->SetStringField(TEXT("path"), Script->GetPathName());
			const FNiagaraTypeDefinition Output = DynamicInputOutputType(Script);
			if (Output.IsValid())
			{
				Object->SetStringField(TEXT("output_type"), TypeName(Output));
			}
			return Object;
		}

		// ------------------------------------------------- input edits ---

		/// Write a literal onto an input, replacing whatever drives it now —
		/// a dynamic input, a link or an expression — the way typing a value
		/// into the stack panel does.
		bool ApplyLocalValue(
			UNiagaraStackFunctionInput* Input, const TSharedPtr<FJsonValue>& Value, FString& OutError)
		{
			const FNiagaraTypeDefinition& Type = Input->GetInputType();
			if (Type.IsDataInterface())
			{
				OutError = FString::Printf(
					TEXT("input is a %s data interface — pass `data_interface` (a class) and `properties`"),
					*TypeName(Type));
				return false;
			}
			if (Type.IsUObject())
			{
				OutError = FString::Printf(TEXT("input is a %s object — pass `object_asset`"), *TypeName(Type));
				return false;
			}
			TArray<uint8> Bytes;
			if (!ValueToBytes(Type, Value, Bytes, OutError))
			{
				return false;
			}
			UStruct* Struct = Type.GetStruct();
			if (Struct == nullptr)
			{
				OutError = FString::Printf(TEXT("%s has no struct to hold a value"), *TypeName(Type));
				return false;
			}
			const TSharedRef<FStructOnScope> Local = MakeShared<FStructOnScope>(Struct);
			if (Local->GetStructMemory() == nullptr || Bytes.Num() > Struct->GetStructureSize())
			{
				OutError = FString::Printf(TEXT("%s does not fit its %d-byte struct"), *TypeName(Type),
					Struct->GetStructureSize());
				return false;
			}
			FMemory::Memcpy(Local->GetStructMemory(), Bytes.GetData(), Bytes.Num());
			Input->SetLocalValue(Local);
			return true;
		}

		/// Link an input to a parameter by name. Known parameters (engine
		/// constants, user parameters, and everything written earlier in the
		/// stack) link directly, or through the conversion script the editor
		/// would pick; an unknown name in a readable namespace becomes a new
		/// parameter of the input's type, as dragging a new parameter onto the
		/// input would.
		bool ApplyLink(UNiagaraStackFunctionInput* Input, const FString& ParameterName, FString& OutError)
		{
			TSet<UNiagaraStackFunctionInput::FNiagaraAvailableParameterInfo> Available;
			UNiagaraStackFunctionInput::FGetAvailableParameterArgs Args;
			Args.bIncludeConversionScripts = true;
			Input->GetAvailableParameters(Available, Args);
			for (const UNiagaraStackFunctionInput::FNiagaraAvailableParameterInfo& Info : Available)
			{
				if (!Info.Variable.GetName().ToString().Equals(ParameterName, ESearchCase::IgnoreCase))
				{
					continue;
				}
				if (Info.ConversionScript != nullptr)
				{
					Input->SetLinkedParameterValueViaConversionScript(Info.Variable, *Info.ConversionScript);
				}
				else
				{
					Input->SetLinkedParameterValue(Info.Variable);
				}
				return true;
			}

			TArray<FName> Namespaces;
			Input->GetNamespacesForNewReadParameters(Namespaces);
			const FName NewName(*ParameterName);
			const FNiagaraParameterHandle Handle(NewName);
			const FName Namespace = Handle.GetNamespace();
			if (Handle.IsValid() && !Namespace.IsNone() && Namespaces.Contains(Namespace))
			{
				Input->SetLinkedParameterValue(FNiagaraVariableBase(Input->GetInputType(), NewName));
				return true;
			}

			TArray<FString> Names;
			for (const UNiagaraStackFunctionInput::FNiagaraAvailableParameterInfo& Info : Available)
			{
				Names.Add(Info.Variable.GetName().ToString());
			}
			Names.Sort();
			TArray<FString> NamespaceNames;
			for (const FName& Candidate : Namespaces)
			{
				NamespaceNames.Add(Candidate.ToString());
			}
			OutError = FString::Printf(
				TEXT("no parameter '%s' can feed this %s input — it can link to: %s; or name a new ")
				TEXT("parameter in one of the namespaces %s (see list_input_options)"),
				*ParameterName, *TypeName(Input->GetInputType()),
				Names.IsEmpty() ? TEXT("(nothing)") : *FString::Join(Names, TEXT(", ")),
				NamespaceNames.IsEmpty() ? TEXT("(none)") : *FString::Join(NamespaceNames, TEXT(", ")));
			return false;
		}

		FString Squashed(const FString& Text)
		{
			return Text.Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT(""));
		}

		/// Resolve a dynamic input by asset path, asset name or display name
		/// ("Random Range Float" finds RandomRangeFloat) among the scripts whose
		/// output the input's type accepts.
		UNiagaraScript* FindDynamicInput(
			UNiagaraStackFunctionInput* Input,
			const FString& Spec,
			TArray<UNiagaraScript*>& OutAvailable,
			FString& OutError)
		{
			Input->GetAvailableDynamicInputs(OutAvailable, /*bIncludeNonLibraryInputs*/ true);
			UNiagaraScript* ByPath = Cast<UNiagaraScript>(ResolveAsset(Spec));
			if (ByPath != nullptr)
			{
				if (OutAvailable.Contains(ByPath))
				{
					return ByPath;
				}
				const FNiagaraTypeDefinition Output = DynamicInputOutputType(ByPath);
				OutError = ByPath->GetUsage() != ENiagaraScriptUsage::DynamicInput
					? FString::Printf(TEXT("'%s' is not a dynamic input script"), *Spec)
					: FString::Printf(TEXT("'%s' produces a %s, which this %s input cannot take"), *Spec,
						Output.IsValid() ? *TypeName(Output) : TEXT("value of unknown type"),
						*TypeName(Input->GetInputType()));
				return nullptr;
			}
			const FString Wanted = Squashed(Spec);
			for (UNiagaraScript* Script : OutAvailable)
			{
				if (Script != nullptr
					&& (Script->GetName().Equals(Spec, ESearchCase::IgnoreCase)
						|| Squashed(Script->GetName()).Equals(Wanted, ESearchCase::IgnoreCase)))
				{
					return Script;
				}
			}
			TArray<FString> Names;
			for (UNiagaraScript* Script : OutAvailable)
			{
				if (Script != nullptr)
				{
					Names.Add(Script->GetName());
				}
			}
			Names.Sort();
			OutError = FString::Printf(
				TEXT("no dynamic input '%s' fits this %s input — %d do, e.g. %s (see list_input_options)"),
				*Spec, *TypeName(Input->GetInputType()), Names.Num(),
				Names.IsEmpty() ? TEXT("(none)") : *FString::Join(TArray<FString>(Names.GetData(), FMath::Min(Names.Num(), 12)), TEXT(", ")));
			return nullptr;
		}

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
				if (Operation == TEXT("list_module_scripts") || Operation == TEXT("list_dynamic_inputs"))
				{
					const bool bDynamic = Operation == TEXT("list_dynamic_inputs");
					FString Contains, StageName, OutputType;
					Body->TryGetStringField(TEXT("name_contains"), Contains);
					Body->TryGetStringField(TEXT("stage"), StageName);
					Body->TryGetStringField(TEXT("output_type"), OutputType);

					FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions Options;
					Options.ScriptUsageToInclude =
						bDynamic ? ENiagaraScriptUsage::DynamicInput : ENiagaraScriptUsage::Module;
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
						if (bDynamic)
						{
							// What a dynamic input produces is only known from its
							// graph, so the script is loaded — the editor's own
							// dynamic-input menu does the same.
							UNiagaraScript* Script = Cast<UNiagaraScript>(Asset.GetAsset());
							if (Script == nullptr)
							{
								continue;
							}
							if (!OutputType.IsEmpty() && !TypeNameMatches(DynamicInputOutputType(Script), OutputType))
							{
								continue;
							}
							const TSharedRef<FJsonObject> Item = ScriptToJson(Script);
							++Matched;
							if (Results.Num() < Max)
							{
								Results.Add(MakeShared<FJsonValueObject>(Item));
							}
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
					const bool bHidden = BoolOr(Body, TEXT("include_hidden"), false);
					TUniquePtr<FStackView> View;
					if (bInputs)
					{
						View = MakeUnique<FStackView>(*System);
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("system"), System->GetPathName());

					const auto StageJson = [&](const FStackTarget& Target)
					{
						TArray<TSharedPtr<FJsonValue>> Modules;
						int32 Index = 0;
						for (UNiagaraNodeFunctionCall* Call : OrderedModules(Target.OutputNode))
						{
							Modules.Add(MakeShared<FJsonValueObject>(
								ModuleToJson(Call, Index++, Target, bInputs, View.Get(), bHidden)));
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
				const bool bModuleOperation = Operation == TEXT("add_module") || Operation == TEXT("remove_module")
					|| Operation == TEXT("set_module_enabled") || Operation == TEXT("set_module_input")
					|| Operation == TEXT("get_module") || Operation == TEXT("list_input_options");
				FStackTarget Target;
				if (bModuleOperation && !ResolveStack(System, Body, Responder, Target))
				{
					return;
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
					const FStackView View(*System);
					Responder->Ok(ModuleToJson(Added, Modules.IndexOfByKey(Added), Target, true, &View,
						BoolOr(Body, TEXT("include_hidden"), false)));
					return;
				}

				if (bModuleOperation)
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
					const int32 ModuleIndex = Modules.IndexOfByKey(Call);

					if (Operation == TEXT("get_module"))
					{
						const FStackView View(*System);
						Responder->Ok(ModuleToJson(Call, ModuleIndex, Target, true, &View,
							BoolOr(Body, TEXT("include_hidden"), false)));
						return;
					}

					// ------------------------------------- input reads ---
					if (Operation == TEXT("list_input_options"))
					{
						FString InputPath;
						if (!RequireString(Body, TEXT("input"), InputPath, Responder,
								TEXT("an input path from niagara_author stack, e.g. SpawnRate or Drag/Minimum")))
						{
							return;
						}
						const FStackView View(*System);
						UNiagaraStackModuleItem* Item = View.ModuleItem(Target, Call);
						if (Item == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_stack_entry"),
								FString::Printf(TEXT("the stack view has no entry for module '%s'"),
									*Call->GetFunctionName()));
							return;
						}
						TArray<UNiagaraStackFunctionInput*> All;
						CollectInputs(*Item, All);
						FString Error;
						UNiagaraStackFunctionInput* Input = ResolveInputPath(All, Call, InputPath, Error);
						if (Input == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("input_not_found"), Error);
							return;
						}
						FString Contains;
						Body->TryGetStringField(TEXT("name_contains"), Contains);
						const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_results"), 100), 1, 500);
						const FNiagaraTypeDefinition& Type = Input->GetInputType();

						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("module"), Call->GetFunctionName());
						Data->SetObjectField(TEXT("input"), InputToJson(Input, All, FString(), /*bIncludeHidden*/ true));

						TArray<UNiagaraScript*> DynamicInputs;
						Input->GetAvailableDynamicInputs(DynamicInputs,
							BoolOr(Body, TEXT("include_non_library"), false));
						DynamicInputs.Sort([](const UNiagaraScript& A, const UNiagaraScript& B)
							{ return A.GetName() < B.GetName(); });
						TArray<TSharedPtr<FJsonValue>> DynamicJson;
						int32 DynamicTotal = 0;
						for (UNiagaraScript* Script : DynamicInputs)
						{
							if (Script == nullptr || (!Contains.IsEmpty() && !Script->GetName().Contains(Contains)))
							{
								continue;
							}
							++DynamicTotal;
							if (DynamicJson.Num() < Max)
							{
								DynamicJson.Add(MakeShared<FJsonValueObject>(ScriptToJson(Script)));
							}
						}
						Data->SetNumberField(TEXT("dynamic_inputs_total"), DynamicTotal);
						Data->SetArrayField(TEXT("dynamic_inputs"), DynamicJson);

						TSet<UNiagaraStackFunctionInput::FNiagaraAvailableParameterInfo> Available;
						UNiagaraStackFunctionInput::FGetAvailableParameterArgs Args;
						Args.bIncludeConversionScripts = true;
						Input->GetAvailableParameters(Available, Args);
						TArray<TSharedPtr<FJsonValue>> ParameterJson;
						TArray<UNiagaraStackFunctionInput::FNiagaraAvailableParameterInfo> Sorted = Available.Array();
						Sorted.Sort([](const UNiagaraStackFunctionInput::FNiagaraAvailableParameterInfo& A,
										const UNiagaraStackFunctionInput::FNiagaraAvailableParameterInfo& B)
							{ return A.Variable.GetName().LexicalLess(B.Variable.GetName()); });
						int32 ParameterTotal = 0;
						for (const UNiagaraStackFunctionInput::FNiagaraAvailableParameterInfo& Info : Sorted)
						{
							const FString Name = Info.Variable.GetName().ToString();
							if (!Contains.IsEmpty() && !Name.Contains(Contains))
							{
								continue;
							}
							++ParameterTotal;
							if (ParameterJson.Num() >= Max)
							{
								continue;
							}
							const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
							Entry->SetStringField(TEXT("name"), Name);
							Entry->SetStringField(TEXT("type"), Info.Variable.GetType().GetName());
							if (Info.ConversionScript != nullptr)
							{
								Entry->SetStringField(TEXT("via_conversion"), Info.ConversionScript->GetName());
							}
							ParameterJson.Add(MakeShared<FJsonValueObject>(Entry));
						}
						Data->SetNumberField(TEXT("parameters_total"), ParameterTotal);
						Data->SetArrayField(TEXT("parameters"), ParameterJson);

						TArray<FName> Namespaces;
						Input->GetNamespacesForNewReadParameters(Namespaces);
						TArray<TSharedPtr<FJsonValue>> NamespaceJson;
						for (const FName& Namespace : Namespaces)
						{
							NamespaceJson.Add(MakeShared<FJsonValueString>(Namespace.ToString()));
						}
						Data->SetArrayField(TEXT("new_parameter_namespaces"), NamespaceJson);

						if (Type.IsDataInterface())
						{
							TArray<FString> ClassNames;
							for (TObjectIterator<UClass> It; It; ++It)
							{
								if (It->IsChildOf(Type.GetClass()) && !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
								{
									ClassNames.Add(It->GetName());
								}
							}
							ClassNames.Sort();
							TArray<TSharedPtr<FJsonValue>> ClassJson;
							for (const FString& ClassName : ClassNames)
							{
								ClassJson.Add(MakeShared<FJsonValueString>(ClassName));
							}
							Data->SetArrayField(TEXT("data_interface_classes"), ClassJson);
						}
						Data->SetBoolField(TEXT("accepts_expression"), !Type.IsDataInterface() && !Type.IsUObject());
						Responder->Ok(Data);
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditNiagaraModule", "McpLink Edit Niagara Module"));
					System->Modify();

					if (Operation == TEXT("set_module_enabled"))
					{
						FNiagaraStackGraphUtilities::SetModuleIsEnabled(
							*Call, BoolOr(Body, TEXT("enabled"), true));
						Responder->Ok(ModuleToJson(Call, ModuleIndex, Target, false, nullptr));
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

					// ------------------------------------ set_module_input ---
					FString InputPath;
					if (!RequireString(Body, TEXT("input"), InputPath, Responder,
							TEXT("an input path from niagara_author stack, e.g. SpawnRate or Drag/Minimum")))
					{
						return;
					}
					const TSharedPtr<FJsonValue> Value = Body->TryGetField(TEXT("value"));
					FString DynamicSpec, LinkSpec, Expression, DataInterfaceSpec, ObjectAssetSpec;
					Body->TryGetStringField(TEXT("dynamic_input"), DynamicSpec);
					Body->TryGetStringField(TEXT("link"), LinkSpec);
					Body->TryGetStringField(TEXT("expression"), Expression);
					Body->TryGetStringField(TEXT("data_interface"), DataInterfaceSpec);
					Body->TryGetStringField(TEXT("object_asset"), ObjectAssetSpec);
					const TSharedPtr<FJsonObject>* Properties = nullptr;
					Body->TryGetObjectField(TEXT("properties"), Properties);
					const TSharedPtr<FJsonObject>* NestedInputs = nullptr;
					Body->TryGetObjectField(TEXT("inputs"), NestedInputs);
					const bool bReset = BoolOr(Body, TEXT("reset"), false);
					const bool bHasEditCondition = Body->HasTypedField<EJson::Boolean>(TEXT("edit_condition_enabled"));

					const int32 Actions = (Value.IsValid() ? 1 : 0) + (DynamicSpec.IsEmpty() ? 0 : 1)
						+ (LinkSpec.IsEmpty() ? 0 : 1) + (Body->HasField(TEXT("expression")) ? 1 : 0)
						+ (DataInterfaceSpec.IsEmpty() ? 0 : 1) + (ObjectAssetSpec.IsEmpty() ? 0 : 1)
						+ (bReset ? 1 : 0);
					if (Actions > 1)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("ambiguous"),
							TEXT("pass one of `value`, `dynamic_input`, `link`, `expression`, `data_interface`, ")
							TEXT("`object_asset` or `reset` per call"));
						return;
					}
					if (Actions == 0 && Properties == nullptr && !bHasEditCondition)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("nothing to set — pass `value` (a literal), `dynamic_input` (a script, plus ")
							TEXT("optional nested `inputs`), `link` (a parameter name), `expression` (HLSL), ")
							TEXT("`data_interface` (a class, plus `properties`), `object_asset`, `reset`, or ")
							TEXT("`edit_condition_enabled`"));
						return;
					}

					const FStackView View(*System);
					UNiagaraStackModuleItem* Item = View.ModuleItem(Target, Call);
					if (Item == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_stack_entry"),
							FString::Printf(TEXT("the stack view has no entry for module '%s'"),
								*Call->GetFunctionName()));
						return;
					}
					TArray<UNiagaraStackFunctionInput*> All;
					CollectInputs(*Item, All);
					FString Error;
					UNiagaraStackFunctionInput* Input = ResolveInputPath(All, Call, InputPath, Error);
					if (Input == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("input_not_found"), Error);
						return;
					}
					using EValueMode = UNiagaraStackFunctionInput::EValueMode;
					const FNiagaraTypeDefinition& Type = Input->GetInputType();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("module"), Call->GetFunctionName());
					Data->SetStringField(TEXT("input"), InputPath);
					Data->SetStringField(TEXT("was"), ModeName(Input->GetValueMode()));

					if (bReset)
					{
						if (!Input->CanReset())
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_default"),
								FString::Printf(TEXT("'%s' is already at the module's default"), *InputPath));
							return;
						}
						Input->Reset();
					}
					else if (Value.IsValid())
					{
						if (!ApplyLocalValue(Input, Value, Error))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"),
								FString::Printf(TEXT("'%s': %s"), *InputPath, *Error));
							return;
						}
					}
					else if (!DynamicSpec.IsEmpty())
					{
						TArray<UNiagaraScript*> Available;
						UNiagaraScript* Script = FindDynamicInput(Input, DynamicSpec, Available, Error);
						if (Script == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("dynamic_input_not_found"),
								FString::Printf(TEXT("'%s': %s"), *InputPath, *Error));
							return;
						}
						// The node names itself after the script (RandomRangeFloat,
						// RandomRangeFloat001, ...): the engine ignores a suggested
						// name once the node has one, and the override pins and
						// rapid-iteration parameters beneath embed that name.
						Input->SetDynamicInput(Script);
						UNiagaraNodeFunctionCall* Node = Input->GetDynamicInputNode();
						if (Node == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("set_failed"),
								FString::Printf(TEXT("Niagara did not attach '%s' to '%s'"), *Script->GetName(), *InputPath));
							return;
						}
						Data->SetStringField(TEXT("dynamic_input"), Node->GetFunctionName());

						if (NestedInputs != nullptr)
						{
							// Static switches change which inputs exist, so they go
							// first and the tree is re-read after each one — the
							// same order the editor pastes inputs in.
							TArray<TPair<FString, TSharedPtr<FJsonValue>>> Nested;
							for (const auto& Pair : (*NestedInputs)->Values)
							{
								Nested.Emplace(FString(Pair.Key.ToView()), Pair.Value);
							}
							TArray<TSharedPtr<FJsonValue>> Applied;
							for (int32 Pass = 0; Pass < 2; ++Pass)
							{
								for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Nested)
								{
									const FString& Key = Pair.Key;
									All.Reset();
									CollectInputs(*Item, All);
									FString ChildError;
									UNiagaraStackFunctionInput* Child =
										ResolveInputPath(All, Call, InputPath + TEXT("/") + Key, ChildError);
									if (Child == nullptr)
									{
										if (Pass == 1)
										{
											Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("input_not_found"),
												FString::Printf(TEXT("after attaching %s: %s"), *Node->GetFunctionName(), *ChildError));
											return;
										}
										continue;
									}
									if (Child->IsStaticParameter() != (Pass == 0))
									{
										continue;
									}
									if (!ApplyLocalValue(Child, Pair.Value, ChildError))
									{
										Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"),
											FString::Printf(TEXT("'%s/%s': %s"), *InputPath, *Key, *ChildError));
										return;
									}
									Applied.Add(MakeShared<FJsonValueString>(Key));
								}
							}
							Data->SetArrayField(TEXT("nested_inputs_set"), Applied);
						}
					}
					else if (!LinkSpec.IsEmpty())
					{
						if (!ApplyLink(Input, LinkSpec, Error))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("parameter_not_found"),
								FString::Printf(TEXT("'%s': %s"), *InputPath, *Error));
							return;
						}
					}
					else if (Body->HasField(TEXT("expression")))
					{
						if (Type.IsDataInterface() || Type.IsUObject())
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"),
								FString::Printf(TEXT("'%s' is a %s, which an expression cannot produce"),
									*InputPath, *TypeName(Type)));
							return;
						}
						Input->SetCustomExpression(Expression);
					}
					else if (!DataInterfaceSpec.IsEmpty())
					{
						UClass* Required = Type.GetClass();
						if (!Type.IsDataInterface() || Required == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"),
								FString::Printf(TEXT("'%s' is a %s, not a data interface input"),
									*InputPath, *TypeName(Type)));
							return;
						}
						UClass* Class = ResolveClass(DataInterfaceSpec);
						if (Class == nullptr || !Class->IsChildOf(Required) || Class->HasAnyClassFlags(CLASS_Abstract))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_class"),
								FString::Printf(
									TEXT("'%s' is not a concrete %s — list_input_options names the classes that fit"),
									*DataInterfaceSpec, *Required->GetName()));
							return;
						}
						UNiagaraDataInterface* Current = Input->GetDataValueObject();
						if (Input->GetValueMode() != EValueMode::Data || Current == nullptr
							|| Current->GetClass() != Class || !OverrideDataInterface(Input))
						{
							Input->SetDataInterfaceValue(Class);
						}
					}
					else if (!ObjectAssetSpec.IsEmpty())
					{
						UObject* Asset = ResolveAsset(ObjectAssetSpec);
						if (!Type.IsUObject() || Type.GetClass() == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"),
								FString::Printf(TEXT("'%s' is a %s, not an object input"), *InputPath, *TypeName(Type)));
							return;
						}
						if (Asset == nullptr || !Asset->IsA(Type.GetClass()))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
								FString::Printf(TEXT("'%s' is not a loadable %s"), *ObjectAssetSpec,
									*Type.GetClass()->GetName()));
							return;
						}
						if (Input->GetValueMode() != EValueMode::ObjectAsset)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_object_input"),
								FString::Printf(TEXT("'%s' is currently %s and cannot take an object asset"),
									*InputPath, ModeName(Input->GetValueMode())));
							return;
						}
						Input->SetObjectAssetValue(Asset);
					}

					if (Properties != nullptr)
					{
						UNiagaraDataInterface* Current = Input->GetDataValueObject();
						if (Input->GetValueMode() != EValueMode::Data || Current == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"),
								FString::Printf(TEXT("'%s' is %s, so `properties` has no data interface to edit"),
									*InputPath, ModeName(Input->GetValueMode())));
							return;
						}
						// Edited through the view model's placeholder, which is what
						// the details panel edits; the engine copies the change onto
						// the override in the graph.
						bool bApplied = false;
						Input->SetDataInterfaceValueExternal(Current->GetClass(),
							[&](UNiagaraDataInterface* DataInterface)
							{
								bApplied = FJsonObjectConverter::JsonObjectToUStruct(
									Properties->ToSharedRef(), DataInterface->GetClass(), DataInterface, 0, 0);
							});
						if (!bApplied)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_properties"),
								FString::Printf(TEXT("could not apply `properties` to the %s on '%s'"),
									*Current->GetClass()->GetName(), *InputPath));
							return;
						}
					}

					if (bHasEditCondition)
					{
						if (!Input->GetHasEditCondition())
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("no_edit_condition"),
								FString::Printf(TEXT("'%s' has no edit condition to toggle"), *InputPath));
							return;
						}
						Input->SetEditConditionEnabled(BoolOr(Body, TEXT("edit_condition_enabled"), true));
					}

					// Read the input back through a fresh walk: an edit can
					// rebuild the entries beneath it.
					All.Reset();
					CollectInputs(*Item, All);
					if (UNiagaraStackFunctionInput* After = ResolveInputPath(All, Call, InputPath, Error))
					{
						Data->SetObjectField(TEXT("result"),
							InputToJson(After, All, FString(), BoolOr(Body, TEXT("include_hidden"), false)));
					}
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_stages, list_module_scripts, ")
						TEXT("list_dynamic_inputs, list_renderer_classes, create_system, create_emitter, ")
						TEXT("stack, add_emitter, remove_emitter, rename_emitter, set_emitter_enabled, ")
						TEXT("add_module, get_module, remove_module, set_module_enabled, set_module_input, ")
						TEXT("list_input_options, add_renderer, remove_renderer, compile, compile_status, ")
						TEXT("or save"),
						*Operation));
			});
	}
}
