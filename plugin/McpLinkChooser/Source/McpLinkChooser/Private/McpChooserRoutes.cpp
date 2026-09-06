// Chooser tables: a signature (result type, result class, context parameters),
// columns (filter, scoring, output and randomize structs, each bound to a
// context property), rows (result structs plus one cell per column), a
// fallback, and test evaluation against live context objects or structs.
//
// The editor's view model that adds columns and rows is private to
// ChooserEditor, so the same steps are replicated here: insert the column in
// category order, keep every column's row array the size of ResultsStructs,
// compile the bindings and mark the asset changed.

#include "Chooser.h"
#include "ChooserPropertyAccess.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "IChooserColumn.h"
#include "IObjectChooser.h"
#include "JsonObjectConverter.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "ObjectChooser_Asset.h"
#include "ObjectChooser_Class.h"
#include "ScopedTransaction.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/StructView.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace Choosers
	{
		bool NameMatches(const UScriptStruct* Struct, const FString& Spec)
		{
			const FString Name = Struct->GetName();
			return Name.Equals(Spec, ESearchCase::IgnoreCase) || (TEXT("F") + Name).Equals(Spec, ESearchCase::IgnoreCase)
				|| Struct->GetPathName() == Spec || Struct->GetStructCPPName() == Spec;
		}

		UScriptStruct* FindStruct(const UScriptStruct* Base, const FString& Spec, const TCHAR* Suffix = nullptr)
		{
			for (TObjectIterator<UScriptStruct> It; It; ++It)
			{
				if (*It == Base || !It->IsChildOf(Base) || It->HasMetaData(TEXT("Hidden")))
				{
					continue;
				}
				if (NameMatches(*It, Spec) || (Suffix != nullptr && NameMatches(*It, Spec + Suffix)))
				{
					return *It;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> InstancedStructJson(const FInstancedStruct& Value)
		{
			const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			if (Value.IsValid())
			{
				Out->SetStringField(TEXT("_structType"), Value.GetScriptStruct()->GetPathName());
				FJsonObjectConverter::UStructToJsonObject(Value.GetScriptStruct(), Value.GetMemory(), Out, 0, CPF_Deprecated, nullptr,
					EJsonObjectConversionFlags::SkipStandardizeCase);
			}
			return Out;
		}

		// Imports a JSON object into an FInstancedStruct through its owning
		// property, so `_structType` picks the type.
		bool ImportInstancedStruct(const TSharedPtr<FJsonValue>& Value, FStructProperty* Property, void* Container, FString& OutError)
		{
			FText Reason;
			if (!FJsonObjectConverter::JsonValueToUProperty(Value, Property, Property->ContainerPtrToValuePtr<void>(Container), 0, 0, false, &Reason))
			{
				OutError = Reason.ToString();
				return false;
			}
			return true;
		}

		FString ColumnCategory(const UScriptStruct* Struct)
		{
			return Struct->GetMetaData(TEXT("Category"));
		}

		int32 CategoryOrder(const FString& Category)
		{
			if (Category == TEXT("Filter")) return 1;
			if (Category == TEXT("Scoring")) return 2;
			if (Category == TEXT("Output")) return 3;
			if (Category == TEXT("Random")) return 4;
			return 100;
		}

		FChooserColumnBase& ColumnAt(UChooserTable* Table, int32 Index)
		{
			return Table->ColumnsStructs[Index].GetMutable<FChooserColumnBase>();
		}

		// The widget's UpdateTableRows: every column's cell array follows the
		// row count, as does the disabled-row list.
		void SyncRows(UChooserTable* Table)
		{
			const int32 Num = Table->ResultsStructs.Num();
			Table->DisabledRows.SetNum(Num);
			for (FInstancedStruct& Column : Table->ColumnsStructs)
			{
				if (Column.IsValid())
				{
					Column.GetMutable<FChooserColumnBase>().SetNumRows(Num);
				}
			}
		}

		FArrayProperty* CellArray(const FInstancedStruct& Column, FChooserColumnBase& Base)
		{
			const FName ArrayName = Base.RowValuesPropertyName();
			return ArrayName.IsNone() ? nullptr : CastField<FArrayProperty>(Column.GetScriptStruct()->FindPropertyByName(ArrayName));
		}

		TSharedRef<FJsonObject> ColumnJson(UChooserTable* Table, int32 Index)
		{
			const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			const FInstancedStruct& Column = Table->ColumnsStructs[Index];
			Out->SetNumberField(TEXT("index"), Index);
			if (!Column.IsValid())
			{
				return Out;
			}
			FChooserColumnBase& Base = Table->ColumnsStructs[Index].GetMutable<FChooserColumnBase>();
			Out->SetStringField(TEXT("column"), Column.GetScriptStruct()->GetName());
			Out->SetStringField(TEXT("category"), ColumnCategory(Column.GetScriptStruct()));
			Out->SetBoolField(TEXT("disabled"), Base.bDisabled);
			if (const FInstancedStruct* Input = Base.GetInputValuePtr())
			{
				Out->SetObjectField(TEXT("input"), InstancedStructJson(*Input));
			}
			const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			FJsonObjectConverter::UStructToJsonObject(Column.GetScriptStruct(), Column.GetMemory(), Properties, CPF_Edit, CPF_Deprecated, nullptr,
				EJsonObjectConversionFlags::SkipStandardizeCase);
			Properties->RemoveField(TEXT("InputValue"));
			Out->SetObjectField(TEXT("properties"), Properties);
			TArray<TSharedPtr<FJsonValue>> Cells;
			if (FArrayProperty* Array = CellArray(Column, Base))
			{
				FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Column.GetMemory()));
				for (int32 Row = 0; Row < Helper.Num(); ++Row)
				{
					TSharedPtr<FJsonValue> Cell = FJsonObjectConverter::UPropertyToJsonValue(Array->Inner, Helper.GetRawPtr(Row), 0, CPF_Deprecated,
						nullptr, nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
					Cells.Add(Cell.IsValid() ? Cell : MakeShared<FJsonValueNull>());
				}
				Out->SetStringField(TEXT("cell_property"), Array->GetName());
			}
			Out->SetArrayField(TEXT("cells"), Cells);
			return Out;
		}

		TSharedRef<FJsonObject> TableJson(UChooserTable* Table)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("chooser"), Table->GetPathName());
			Data->SetStringField(TEXT("result_type"), StaticEnum<EObjectChooserResultType>()->GetNameStringByValue(static_cast<int64>(Table->ResultType)));
			Data->SetStringField(TEXT("output_class"), Table->OutputObjectType != nullptr ? Table->OutputObjectType->GetPathName() : FString());
			TArray<TSharedPtr<FJsonValue>> Context;
			for (const FInstancedStruct& Entry : Table->ContextData)
			{
				Context.Add(MakeShared<FJsonValueObject>(InstancedStructJson(Entry)));
			}
			Data->SetArrayField(TEXT("context"), Context);
			TArray<TSharedPtr<FJsonValue>> Columns;
			for (int32 Index = 0; Index < Table->ColumnsStructs.Num(); ++Index)
			{
				Columns.Add(MakeShared<FJsonValueObject>(ColumnJson(Table, Index)));
			}
			Data->SetArrayField(TEXT("columns"), Columns);
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (int32 Row = 0; Row < Table->ResultsStructs.Num(); ++Row)
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetNumberField(TEXT("index"), Row);
				Entry->SetObjectField(TEXT("result"), InstancedStructJson(Table->ResultsStructs[Row]));
				Entry->SetBoolField(TEXT("disabled"), Table->DisabledRows.IsValidIndex(Row) && Table->DisabledRows[Row]);
				Rows.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Data->SetArrayField(TEXT("rows"), Rows);
			Data->SetObjectField(TEXT("fallback"), InstancedStructJson(Table->FallbackResult));
			return Data;
		}

		UChooserTable* TableOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("chooser"), Path, Responder, TEXT("a Chooser Table asset path")))
			{
				return nullptr;
			}
			UChooserTable* Table = Cast<UChooserTable>(ResolveAsset(Path));
			if (Table == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("chooser_not_found"),
					FString::Printf(TEXT("no Chooser Table at '%s'"), *Path));
			}
			return Table;
		}

		bool RowIndexOrError(const TSharedRef<FJsonObject>& Body, UChooserTable* Table, const TSharedRef<FMcpResponder>& Responder, int32& OutRow)
		{
			OutRow = IntOr(Body, TEXT("row"), -1);
			if (OutRow < 0 || OutRow >= Table->ResultsStructs.Num())
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("row_not_found"),
					FString::Printf(TEXT("'row' must be a row index from info (0..%d)"), Table->ResultsStructs.Num() - 1));
				return false;
			}
			return true;
		}

		// A column by index or by struct name (first match).
		bool ColumnIndexOrError(const TSharedPtr<FJsonValue>& Spec, UChooserTable* Table, const TSharedRef<FMcpResponder>& Responder, int32& OutIndex)
		{
			OutIndex = -1;
			if (Spec.IsValid() && Spec->Type == EJson::Number)
			{
				OutIndex = static_cast<int32>(Spec->AsNumber());
			}
			else if (Spec.IsValid() && Spec->Type == EJson::String)
			{
				const FString Name = Spec->AsString();
				if (!Name.IsNumeric())
				{
					for (int32 Index = 0; Index < Table->ColumnsStructs.Num(); ++Index)
					{
						if (Table->ColumnsStructs[Index].IsValid() && NameMatches(Table->ColumnsStructs[Index].GetScriptStruct(), Name))
						{
							OutIndex = Index;
							break;
						}
					}
				}
				else
				{
					OutIndex = FCString::Atoi(*Name);
				}
			}
			if (OutIndex < 0 || OutIndex >= Table->ColumnsStructs.Num() || !Table->ColumnsStructs[OutIndex].IsValid())
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("column_not_found"),
					FString::Printf(TEXT("'column' must be a column index or struct name from info (0..%d)"), Table->ColumnsStructs.Num() - 1));
				return false;
			}
			return true;
		}

		bool SetCell(UChooserTable* Table, int32 ColumnIndex, int32 Row, const TSharedPtr<FJsonValue>& Value, FString& OutError)
		{
			FInstancedStruct& Column = Table->ColumnsStructs[ColumnIndex];
			FChooserColumnBase& Base = Column.GetMutable<FChooserColumnBase>();
			FArrayProperty* Array = CellArray(Column, Base);
			if (Array == nullptr)
			{
				OutError = FString::Printf(TEXT("column %d (%s) has no per-row cells"), ColumnIndex, *Column.GetScriptStruct()->GetName());
				return false;
			}
			FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Column.GetMutableMemory()));
			if (Row < 0 || Row >= Helper.Num())
			{
				OutError = FString::Printf(TEXT("column %d has %d cells, no row %d"), ColumnIndex, Helper.Num(), Row);
				return false;
			}
			FText Reason;
			if (!FJsonObjectConverter::JsonValueToUProperty(Value, Array->Inner, Helper.GetRawPtr(Row), 0, 0, false, &Reason))
			{
				OutError = FString::Printf(TEXT("cell for column %d (%s) did not import: %s — the cell shape is one element of %s.%s"),
					ColumnIndex, *Column.GetScriptStruct()->GetName(), *Reason.ToString(), *Column.GetScriptStruct()->GetName(), *Array->GetName());
				return false;
			}
			return true;
		}

		// `cells` is an object keyed by column index or struct name.
		bool ApplyCells(const TSharedRef<FJsonObject>& Body, UChooserTable* Table, int32 Row, const TSharedRef<FMcpResponder>& Responder)
		{
			const TSharedPtr<FJsonObject>* Cells = nullptr;
			if (!Body->TryGetObjectField(TEXT("cells"), Cells) || !Cells->IsValid())
			{
				return true;
			}
			for (const auto& Pair : (*Cells)->Values)
			{
				int32 ColumnIndex = -1;
				if (!ColumnIndexOrError(MakeShared<FJsonValueString>(FString(Pair.Key.ToView())), Table, Responder, ColumnIndex))
				{
					return false;
				}
				FString Error;
				if (!SetCell(Table, ColumnIndex, Row, Pair.Value, Error))
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_cell"), Error);
					return false;
				}
			}
			return true;
		}

		// A result from `result` (an FObjectChooserBase JSON with _structType)
		// or the shortcuts `asset`, `class` and `nested_chooser`.
		bool BuildResult(const TSharedRef<FJsonObject>& Body, FInstancedStruct& Out, FString& OutError)
		{
			FString Spec;
			if (Body->TryGetStringField(TEXT("asset"), Spec))
			{
				UObject* Asset = ResolveAsset(Spec);
				if (Asset == nullptr)
				{
					OutError = FString::Printf(TEXT("no asset at '%s'"), *Spec);
					return false;
				}
				Out.InitializeAs<FAssetChooser>();
				Out.GetMutable<FAssetChooser>().Asset = Asset;
				return true;
			}
			if (Body->TryGetStringField(TEXT("class"), Spec))
			{
				UClass* Class = ResolveClass(Spec);
				if (Class == nullptr)
				{
					OutError = FString::Printf(TEXT("no class '%s'"), *Spec);
					return false;
				}
				Out.InitializeAs<FClassChooser>();
				Out.GetMutable<FClassChooser>().Class = Class;
				return true;
			}
			if (Body->TryGetStringField(TEXT("nested_chooser"), Spec))
			{
				UChooserTable* Nested = Cast<UChooserTable>(ResolveAsset(Spec));
				if (Nested == nullptr)
				{
					OutError = FString::Printf(TEXT("no Chooser Table at '%s'"), *Spec);
					return false;
				}
				Out.InitializeAs<FEvaluateChooser>();
				Out.GetMutable<FEvaluateChooser>().Chooser = Nested;
				return true;
			}
			const TSharedPtr<FJsonObject>* Result = nullptr;
			if (Body->TryGetObjectField(TEXT("result"), Result) && Result->IsValid())
			{
				FString TypePath;
				(*Result)->TryGetStringField(TEXT("_structType"), TypePath);
				UScriptStruct* Type = FindStruct(FObjectChooserBase::StaticStruct(), TypePath, TEXT("Chooser"));
				if (Type == nullptr)
				{
					OutError = FString::Printf(TEXT("result._structType '%s' is not an object chooser struct — list_types shows them"), *TypePath);
					return false;
				}
				Out.InitializeAs(Type);
				FText Reason;
				if (!FJsonObjectConverter::JsonObjectToUStruct((*Result).ToSharedRef(), Type, Out.GetMutableMemory(), 0, 0, false, &Reason))
				{
					OutError = FString::Printf(TEXT("result did not import onto %s: %s"), *Type->GetName(), *Reason.ToString());
					return false;
				}
				return true;
			}
			OutError = TEXT("a row needs 'asset', 'class', 'nested_chooser' or a 'result' object with _structType");
			return false;
		}

		bool Finish(UChooserTable* Table)
		{
			SyncRows(Table);
			Table->Compile(true);
			Table->PostEditChange();
			Table->MarkPackageDirty();
			return true;
		}
	}

	void RegisterChooserRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace Choosers;

		Core.RegisterRoute(TEXT("/api/data/chooser"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_types"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					TArray<TSharedPtr<FJsonValue>> Columns, Results, Parameters;
					for (TObjectIterator<UScriptStruct> It; It; ++It)
					{
						if (It->HasMetaData(TEXT("Hidden")))
						{
							continue;
						}
						if (It->IsChildOf(FChooserColumnBase::StaticStruct()) && *It != FChooserColumnBase::StaticStruct())
						{
							const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
							Entry->SetStringField(TEXT("column"), It->GetName());
							Entry->SetStringField(TEXT("path"), It->GetPathName());
							Entry->SetStringField(TEXT("category"), ColumnCategory(*It));
							Entry->SetStringField(TEXT("tooltip"), It->GetMetaData(TEXT("Tooltip")));
							FInstancedStruct Probe;
							Probe.InitializeAs(*It);
							FChooserColumnBase& Base = Probe.GetMutable<FChooserColumnBase>();
							Entry->SetStringField(TEXT("input_base"), Base.GetInputBaseType() != nullptr ? Base.GetInputBaseType()->GetName() : FString());
							if (FArrayProperty* Array = CellArray(Probe, Base))
							{
								Entry->SetStringField(TEXT("cell_property"), Array->GetName());
								Entry->SetStringField(TEXT("cell_type"), Array->Inner->GetCPPType());
							}
							Columns.Add(MakeShared<FJsonValueObject>(Entry));
						}
						else if (It->IsChildOf(FObjectChooserBase::StaticStruct()) && *It != FObjectChooserBase::StaticStruct())
						{
							const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
							Entry->SetStringField(TEXT("result"), It->GetName());
							Entry->SetStringField(TEXT("path"), It->GetPathName());
							Entry->SetStringField(TEXT("result_type"), It->GetMetaData(TEXT("ResultType")));
							Entry->SetStringField(TEXT("category"), It->GetMetaData(TEXT("Category")));
							Results.Add(MakeShared<FJsonValueObject>(Entry));
						}
						else if (It->IsChildOf(FChooserParameterBase::StaticStruct()) && *It != FChooserParameterBase::StaticStruct())
						{
							const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
							Entry->SetStringField(TEXT("parameter"), It->GetName());
							Entry->SetStringField(TEXT("path"), It->GetPathName());
							Entry->SetStringField(TEXT("base"), It->GetSuperStruct() != nullptr ? It->GetSuperStruct()->GetName() : FString());
							Parameters.Add(MakeShared<FJsonValueObject>(Entry));
						}
					}
					Data->SetArrayField(TEXT("columns"), Columns);
					Data->SetArrayField(TEXT("results"), Results);
					Data->SetArrayField(TEXT("parameters"), Parameters);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("the new asset's package path, e.g. /Game/Choosers/CHT_Locomotion")))
					{
						return;
					}
					FString ResultTypeSpec = TEXT("object");
					Body->TryGetStringField(TEXT("result_type"), ResultTypeSpec);
					EObjectChooserResultType ResultType = EObjectChooserResultType::ObjectResult;
					if (ResultTypeSpec.Equals(TEXT("class"), ESearchCase::IgnoreCase) || ResultTypeSpec == TEXT("ClassResult"))
					{
						ResultType = EObjectChooserResultType::ClassResult;
					}
					else if (ResultTypeSpec.Equals(TEXT("none"), ESearchCase::IgnoreCase) || ResultTypeSpec == TEXT("NoPrimaryResult"))
					{
						ResultType = EObjectChooserResultType::NoPrimaryResult;
					}
					UClass* OutputClass = nullptr;
					FString OutputSpec;
					if (Body->TryGetStringField(TEXT("output_class"), OutputSpec) && !OutputSpec.IsEmpty())
					{
						OutputClass = ResolveClass(OutputSpec);
						if (OutputClass == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("class_not_found"),
								FString::Printf(TEXT("no class '%s' for output_class"), *OutputSpec));
							return;
						}
					}
					// Context parameters: class names, or {"struct": path} entries.
					TArray<FInstancedStruct> Context;
					const TArray<TSharedPtr<FJsonValue>>* ContextSpecs = nullptr;
					if (Body->TryGetArrayField(TEXT("context"), ContextSpecs))
					{
						for (const TSharedPtr<FJsonValue>& Spec : *ContextSpecs)
						{
							FString ClassSpec, StructSpec;
							const TSharedPtr<FJsonObject>* Obj = nullptr;
							if (Spec.IsValid() && Spec->Type == EJson::String)
							{
								ClassSpec = Spec->AsString();
							}
							else if (Spec.IsValid() && Spec->TryGetObject(Obj))
							{
								(*Obj)->TryGetStringField(TEXT("class"), ClassSpec);
								(*Obj)->TryGetStringField(TEXT("struct"), StructSpec);
							}
							if (!ClassSpec.IsEmpty())
							{
								UClass* Class = ResolveClass(ClassSpec);
								if (Class == nullptr)
								{
									Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("class_not_found"),
										FString::Printf(TEXT("context class '%s' not found"), *ClassSpec));
									return;
								}
								FInstancedStruct& Entry = Context.AddDefaulted_GetRef();
								Entry.InitializeAs<FContextObjectTypeClass>();
								Entry.GetMutable<FContextObjectTypeClass>().Class = Class;
							}
							else if (!StructSpec.IsEmpty())
							{
								UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *StructSpec);
								if (Struct == nullptr)
								{
									Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("struct_not_found"),
										FString::Printf(TEXT("context struct '%s' not found — use a /Script/Module.Struct path"), *StructSpec));
									return;
								}
								FInstancedStruct& Entry = Context.AddDefaulted_GetRef();
								Entry.InitializeAs<FContextObjectTypeStruct>();
								Entry.GetMutable<FContextObjectTypeStruct>().Struct = Struct;
							}
						}
					}
					FString Error;
					UChooserTable* Table = Cast<UChooserTable>(CreateAsset(Path, UChooserTable::StaticClass(), nullptr, Error));
					if (Table == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					Table->ResultType = ResultType;
					Table->OutputObjectType = OutputClass;
					Table->ContextData = MoveTemp(Context);
					Table->Version = UChooserTable::CurrentVersion;
					Finish(Table);
					Responder->Ok(TableJson(Table));
					return;
				}

				if (Operation == TEXT("info"))
				{
					UChooserTable* Table = TableOrError(Body, Responder);
					if (Table == nullptr)
					{
						return;
					}
					Responder->Ok(TableJson(Table));
					return;
				}

				if (Operation == TEXT("add_column"))
				{
					UChooserTable* Table = TableOrError(Body, Responder);
					if (Table == nullptr)
					{
						return;
					}
					FString ColumnSpec;
					if (!RequireString(Body, TEXT("column"), ColumnSpec, Responder, TEXT("a column struct from list_types, e.g. FloatRangeColumn or OutputFloatColumn")))
					{
						return;
					}
					UScriptStruct* ColumnType = FindStruct(FChooserColumnBase::StaticStruct(), ColumnSpec, TEXT("Column"));
					if (ColumnType == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("column_type_not_found"),
							FString::Printf(TEXT("'%s' is not a chooser column struct — list_types shows them"), *ColumnSpec));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "ChooserAddColumn", "McpLink Chooser Add Column"));
					Table->Modify(true);
					FInstancedStruct NewColumn;
					NewColumn.InitializeAs(ColumnType);
					FChooserColumnBase& Base = NewColumn.GetMutable<FChooserColumnBase>();
					Base.Initialize(Table);
					// Column-wide properties first (bWrapInput, MaxDistance, TagMatchType, …).
					const TSharedPtr<FJsonObject>* Properties = nullptr;
					if (Body->TryGetObjectField(TEXT("properties"), Properties) && Properties->IsValid())
					{
						FText Reason;
						if (!FJsonObjectConverter::JsonObjectToUStruct((*Properties).ToSharedRef(), ColumnType, NewColumn.GetMutableMemory(), 0, 0, false, &Reason))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"),
								FString::Printf(TEXT("'properties' did not import onto %s: %s"), *ColumnType->GetName(), *Reason.ToString()));
							return;
						}
					}
					// The input binding: a full instanced struct, or a property chain
					// on a context entry through the column's own ContextProperty type.
					FString PropertyChain;
					const TSharedPtr<FJsonObject>* Input = nullptr;
					if (Body->TryGetObjectField(TEXT("input"), Input) && Input->IsValid())
					{
						FStructProperty* InputProperty = CastField<FStructProperty>(ColumnType->FindPropertyByName(TEXT("InputValue")));
						FString Error;
						if (InputProperty == nullptr || !ImportInstancedStruct(MakeShared<FJsonValueObject>(*Input), InputProperty, NewColumn.GetMutableMemory(), Error))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_input"),
								FString::Printf(TEXT("'input' did not import: %s — it is a %s struct with _structType"), *Error,
									Base.GetInputBaseType() != nullptr ? *Base.GetInputBaseType()->GetName() : TEXT("parameter")));
							return;
						}
					}
					else if (Body->TryGetStringField(TEXT("property"), PropertyChain) && Base.GetInputBaseType() != nullptr)
					{
						UScriptStruct* BindingType = nullptr;
						for (TObjectIterator<UScriptStruct> It; It; ++It)
						{
							if (It->IsChildOf(Base.GetInputBaseType()) && It->GetName().EndsWith(TEXT("ContextProperty")))
							{
								BindingType = *It;
								break;
							}
						}
						if (BindingType == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_binding_type"),
								FString::Printf(TEXT("%s has no context-property binding type; pass 'input' with a _structType from list_types"), *ColumnType->GetName()));
							return;
						}
						Base.SetInputType(BindingType);
						FInstancedStruct* InputValue = Base.GetInputValuePtr();
						FStructProperty* BindingProperty = CastField<FStructProperty>(BindingType->FindPropertyByName(TEXT("Binding")));
						if (InputValue == nullptr || !InputValue->IsValid() || BindingProperty == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("binding_failed"), TEXT("the binding struct is not where this build expects it"));
							return;
						}
						FChooserPropertyBinding* Binding = BindingProperty->ContainerPtrToValuePtr<FChooserPropertyBinding>(InputValue->GetMutableMemory());
						Binding->PropertyBindingChain.Reset();
						TArray<FString> Parts;
						PropertyChain.ParseIntoArray(Parts, TEXT("."));
						for (const FString& Part : Parts)
						{
							Binding->PropertyBindingChain.Add(FName(*Part));
						}
						Binding->ContextIndex = IntOr(Body, TEXT("context_index"), 0);
						Binding->IsBoundToRoot = BoolOr(Body, TEXT("bound_to_root"), Parts.Num() == 0);
#if WITH_EDITORONLY_DATA
						Binding->DisplayName = PropertyChain;
#endif
					}
					// Filter and scoring columns first, then outputs, randomize last.
					const int32 Order = CategoryOrder(ColumnCategory(ColumnType));
					int32 InsertIndex = Table->ColumnsStructs.Num();
					for (int32 Index = 0; Index < Table->ColumnsStructs.Num(); ++Index)
					{
						if (Table->ColumnsStructs[Index].IsValid() && CategoryOrder(ColumnCategory(Table->ColumnsStructs[Index].GetScriptStruct())) > Order)
						{
							InsertIndex = Index;
							break;
						}
					}
					Table->ColumnsStructs.Insert(MoveTemp(NewColumn), InsertIndex);
					Finish(Table);
					const TSharedRef<FJsonObject> Data = TableJson(Table);
					Data->SetNumberField(TEXT("added_index"), InsertIndex);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_column") || Operation == TEXT("remove_column"))
				{
					UChooserTable* Table = TableOrError(Body, Responder);
					if (Table == nullptr)
					{
						return;
					}
					int32 Index = -1;
					if (!ColumnIndexOrError(Body->TryGetField(TEXT("column")), Table, Responder, Index))
					{
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "ChooserColumn", "McpLink Chooser Column"));
					Table->Modify(true);
					if (Operation == TEXT("remove_column"))
					{
						Table->ColumnsStructs.RemoveAt(Index);
					}
					else
					{
						FInstancedStruct& Column = Table->ColumnsStructs[Index];
						const TSharedPtr<FJsonObject>* Properties = nullptr;
						if (Body->TryGetObjectField(TEXT("properties"), Properties) && Properties->IsValid())
						{
							FText Reason;
							if (!FJsonObjectConverter::JsonObjectToUStruct((*Properties).ToSharedRef(), Column.GetScriptStruct(), Column.GetMutableMemory(), 0, 0, false, &Reason))
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"),
									FString::Printf(TEXT("'properties' did not import onto %s: %s"), *Column.GetScriptStruct()->GetName(), *Reason.ToString()));
								return;
							}
						}
						const TSharedPtr<FJsonObject>* Input = nullptr;
						if (Body->TryGetObjectField(TEXT("input"), Input) && Input->IsValid())
						{
							FStructProperty* InputProperty = CastField<FStructProperty>(Column.GetScriptStruct()->FindPropertyByName(TEXT("InputValue")));
							FString Error;
							if (InputProperty == nullptr || !ImportInstancedStruct(MakeShared<FJsonValueObject>(*Input), InputProperty, Column.GetMutableMemory(), Error))
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_input"), FString::Printf(TEXT("'input' did not import: %s"), *Error));
								return;
							}
						}
						if (Body->HasTypedField<EJson::Boolean>(TEXT("disabled")))
						{
							ColumnAt(Table, Index).bDisabled = BoolOr(Body, TEXT("disabled"), false);
						}
					}
					Finish(Table);
					Responder->Ok(TableJson(Table));
					return;
				}

				if (Operation == TEXT("add_row"))
				{
					UChooserTable* Table = TableOrError(Body, Responder);
					if (Table == nullptr)
					{
						return;
					}
					FInstancedStruct Result;
					FString Error;
					if (!BuildResult(Body, Result, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_result"), Error);
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "ChooserAddRow", "McpLink Chooser Add Row"));
					Table->Modify(true);
					const int32 Row = IntOr(Body, TEXT("row"), Table->ResultsStructs.Num());
					const int32 Index = FMath::Clamp(Row, 0, Table->ResultsStructs.Num());
					Table->ResultsStructs.Insert(MoveTemp(Result), Index);
					Table->DisabledRows.SetNum(Table->ResultsStructs.Num() - 1);
					Table->DisabledRows.Insert(BoolOr(Body, TEXT("disabled"), false), Index);
					for (FInstancedStruct& Column : Table->ColumnsStructs)
					{
						if (Column.IsValid())
						{
							FChooserColumnBase& Base = Column.GetMutable<FChooserColumnBase>();
							Base.SetNumRows(Table->ResultsStructs.Num() - 1);
							Base.InsertRows(Index, 1);
						}
					}
					SyncRows(Table);
					if (!ApplyCells(Body, Table, Index, Responder))
					{
						return;
					}
					Finish(Table);
					const TSharedRef<FJsonObject> Data = TableJson(Table);
					Data->SetNumberField(TEXT("added_row"), Index);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_row") || Operation == TEXT("remove_row"))
				{
					UChooserTable* Table = TableOrError(Body, Responder);
					if (Table == nullptr)
					{
						return;
					}
					int32 Row = -1;
					if (!RowIndexOrError(Body, Table, Responder, Row))
					{
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "ChooserRow", "McpLink Chooser Row"));
					Table->Modify(true);
					if (Operation == TEXT("remove_row"))
					{
						Table->ResultsStructs.RemoveAt(Row);
						if (Table->DisabledRows.IsValidIndex(Row))
						{
							Table->DisabledRows.RemoveAt(Row);
						}
						TArray<int> Rows = { Row };
						for (FInstancedStruct& Column : Table->ColumnsStructs)
						{
							if (Column.IsValid())
							{
								Column.GetMutable<FChooserColumnBase>().DeleteRows(Rows);
							}
						}
					}
					else
					{
						if (Body->HasField(TEXT("asset")) || Body->HasField(TEXT("class")) || Body->HasField(TEXT("nested_chooser")) || Body->HasField(TEXT("result")))
						{
							FInstancedStruct Result;
							FString Error;
							if (!BuildResult(Body, Result, Error))
							{
								Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_result"), Error);
								return;
							}
							Table->ResultsStructs[Row] = MoveTemp(Result);
						}
						if (Body->HasTypedField<EJson::Boolean>(TEXT("disabled")))
						{
							SyncRows(Table);
							Table->DisabledRows[Row] = BoolOr(Body, TEXT("disabled"), false);
						}
						if (!ApplyCells(Body, Table, Row, Responder))
						{
							return;
						}
					}
					Finish(Table);
					Responder->Ok(TableJson(Table));
					return;
				}

				if (Operation == TEXT("set_fallback"))
				{
					UChooserTable* Table = TableOrError(Body, Responder);
					if (Table == nullptr)
					{
						return;
					}
					FInstancedStruct Result;
					FString Error;
					if (!BuildResult(Body, Result, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_result"), Error);
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "ChooserFallback", "McpLink Chooser Fallback"));
					Table->Modify(true);
					Table->FallbackResult = MoveTemp(Result);
					Finish(Table);
					Responder->Ok(TableJson(Table));
					return;
				}

				if (Operation == TEXT("evaluate"))
				{
					UChooserTable* Table = TableOrError(Body, Responder);
					if (Table == nullptr)
					{
						return;
					}
					// Context params in ContextData order: object paths or actor
					// names, or {"_structType": …} structs (kept alive here, since
					// the context holds views).
					const TArray<TSharedPtr<FJsonValue>>* ContextSpecs = nullptr;
					Body->TryGetArrayField(TEXT("context"), ContextSpecs);
					TArray<FInstancedStruct> Structs;
					Structs.Reserve(ContextSpecs != nullptr ? ContextSpecs->Num() : 0);
					FChooserEvaluationContext Context;
					if (ContextSpecs != nullptr)
					{
						for (const TSharedPtr<FJsonValue>& Spec : *ContextSpecs)
						{
							const TSharedPtr<FJsonObject>* Obj = nullptr;
							if (Spec.IsValid() && Spec->Type == EJson::String)
							{
								const FString Path = Spec->AsString();
								UObject* Object = ResolveObject(Path);
								if (Object == nullptr)
								{
									if (UWorld* World = ResolveWorld(Body))
									{
										Object = ResolveActor(World, Path);
									}
								}
								if (Object == nullptr)
								{
									Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("context_not_found"),
										FString::Printf(TEXT("context entry '%s' is neither an object path nor an actor in the world"), *Path));
									return;
								}
								Context.AddObjectParam(Object);
							}
							else if (Spec.IsValid() && Spec->TryGetObject(Obj))
							{
								FString TypePath;
								(*Obj)->TryGetStringField(TEXT("_structType"), TypePath);
								UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *TypePath);
								if (Struct == nullptr)
								{
									Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("struct_not_found"),
										FString::Printf(TEXT("context struct '%s' not found — use a /Script/Module.Struct path in _structType"), *TypePath));
									return;
								}
								FInstancedStruct& Entry = Structs.AddDefaulted_GetRef();
								Entry.InitializeAs(Struct);
								FText Reason;
								if (!FJsonObjectConverter::JsonObjectToUStruct((*Obj).ToSharedRef(), Struct, Entry.GetMutableMemory(), 0, 0, false, &Reason))
								{
									Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_context"),
										FString::Printf(TEXT("context struct did not import onto %s: %s"), *Struct->GetName(), *Reason.ToString()));
									return;
								}
								Context.AddStructViewParam(FStructView(Struct, Entry.GetMutableMemory()));
							}
						}
					}
					if (Context.Params.Num() != Table->GetContextData().Num())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("context_mismatch"),
							FString::Printf(TEXT("the chooser declares %d context parameter(s) and 'context' has %d — info shows the declared types"),
								Table->GetContextData().Num(), Context.Params.Num()));
						return;
					}
					Table->Compile(true);
					const bool bMulti = BoolOr(Body, TEXT("multi"), false);
					TArray<TSharedPtr<FJsonValue>> Results;
					const FObjectChooserBase::EIteratorStatus Status = UChooserTable::EvaluateChooser(Context, Table,
						FObjectChooserBase::FObjectChooserIteratorCallback::CreateLambda([&Results, bMulti](UObject* Result)
						{
							Results.Add(MakeShared<FJsonValueString>(Result != nullptr ? Result->GetPathName() : FString()));
							return bMulti ? FObjectChooserBase::EIteratorStatus::Continue : FObjectChooserBase::EIteratorStatus::Stop;
						}));
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("chooser"), Table->GetPathName());
					Data->SetStringField(TEXT("status"), Status == FObjectChooserBase::EIteratorStatus::Failed ? TEXT("failed")
						: Status == FObjectChooserBase::EIteratorStatus::Stop ? TEXT("stop") : TEXT("continue"));
					Data->SetArrayField(TEXT("results"), Results);
					// Output columns write into the struct params; hand them back.
					TArray<TSharedPtr<FJsonValue>> StructsOut;
					for (const FInstancedStruct& Entry : Structs)
					{
						StructsOut.Add(MakeShared<FJsonValueObject>(InstancedStructJson(Entry)));
					}
					Data->SetArrayField(TEXT("context_structs"), StructsOut);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					UChooserTable* Table = TableOrError(Body, Responder);
					if (Table == nullptr)
					{
						return;
					}
					FString Filename, Error;
					if (!SaveAsset(Table, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("chooser"), Table->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected list_types, create, info, add_column, set_column, remove_column, ")
						TEXT("add_row, set_row, remove_row, set_fallback, evaluate or save"), *Operation));
			});
	}
}
