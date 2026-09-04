// DataTable routes: inspect and edit rows, and import from CSV.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Factories/DataTableFactory.h"
#include "JsonObjectConverter.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "ScopedTransaction.h"

namespace McpLink
{
	namespace
	{
		UDataTable* TableOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("table"), Path, Responder,
				TEXT("a DataTable asset path, e.g. /Game/Data/DT_Items")))
			{
				return nullptr;
			}
			UObject* Object = ResolveObject(Path);
			if (Object == nullptr && !Path.Contains(TEXT(".")))
			{
				Object = ResolveObject(
					FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path)));
			}
			UDataTable* Table = Cast<UDataTable>(Object);
			if (Table == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("table_not_found"),
					FString::Printf(TEXT("no DataTable at '%s'"), *Path));
			}
			return Table;
		}

		/// Serialise one row using the table's row struct.
		TSharedRef<FJsonObject> RowToJson(const UDataTable* Table, const uint8* RowData)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			if (Table->RowStruct != nullptr && RowData != nullptr)
			{
				// Keep property names as declared (DisplayName, not displayName) so
				// what get_rows returns is what set_row and get_info use.
				FJsonObjectConverter::UStructToJsonObject(
					Table->RowStruct, RowData, Object, 0, 0, nullptr,
					EJsonObjectConversionFlags::SkipStandardizeCase);
			}
			return Object;
		}
	}

	void RegisterDataRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/data/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				// ---- create is the one operation without an existing table ----
				if (Operation == TEXT("create"))
				{
					FString Path, StructSpec;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/Data/DT_Items"))
						|| !RequireString(Body, TEXT("row_struct"), StructSpec, Responder,
							TEXT("a struct deriving from FTableRowBase, e.g. /Script/McpTest.McpTestItemRow or McpTestItemRow")))
					{
						return;
					}
					UScriptStruct* RowStruct = Cast<UScriptStruct>(ResolveObject(StructSpec));
					if (RowStruct == nullptr)
					{
						// Bare name, with or without the F prefix.
						RowStruct = FindFirstObject<UScriptStruct>(
							*StructSpec, EFindFirstObjectOptions::None);
						if (RowStruct == nullptr && StructSpec.StartsWith(TEXT("F")))
						{
							RowStruct = FindFirstObject<UScriptStruct>(
								*StructSpec.RightChop(1), EFindFirstObjectOptions::None);
						}
					}
					if (RowStruct == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("struct_not_found"),
							FString::Printf(TEXT("no struct '%s' — pass a /Script/Module.Name path or a bare struct name"),
								*StructSpec));
						return;
					}
					if (!RowStruct->IsChildOf(FTableRowBase::StaticStruct()))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_row_struct"),
							FString::Printf(TEXT("'%s' does not derive from FTableRowBase"), *RowStruct->GetName()));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateDataTable", "McpLink Create DataTable"));
					UDataTableFactory* Factory = NewObject<UDataTableFactory>();
					Factory->Struct = RowStruct;
					FString Error;
					UObject* Asset = CreateAsset(Path, UDataTable::StaticClass(), Factory, Error);
					if (Asset == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Asset->GetPathName());
					Data->SetStringField(TEXT("row_struct"), RowStruct->GetPathName());
					Responder->Ok(Data);
					return;
				}

				UDataTable* Table = TableOrError(Body, Responder);
				if (!Table) { return; }

				if (Operation == TEXT("get_info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Table->GetPathName());
					Data->SetStringField(TEXT("row_struct"),
						Table->RowStruct ? Table->RowStruct->GetPathName() : TEXT(""));
					Data->SetNumberField(TEXT("row_count"), Table->GetRowMap().Num());
					TArray<TSharedPtr<FJsonValue>> Columns;
					if (Table->RowStruct != nullptr)
					{
						for (TFieldIterator<FProperty> It(Table->RowStruct); It; ++It)
						{
							const TSharedRef<FJsonObject> Column = MakeShared<FJsonObject>();
							Column->SetStringField(TEXT("name"), It->GetName());
							Column->SetStringField(TEXT("type"), It->GetCPPType());
							Columns.Add(MakeShared<FJsonValueObject>(Column));
						}
					}
					Data->SetArrayField(TEXT("columns"), Columns);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("get_rows"))
				{
					const int32 MaxRows = FMath::Clamp(
						Body->HasTypedField<EJson::Number>(TEXT("max_rows"))
							? static_cast<int32>(Body->GetNumberField(TEXT("max_rows")))
							: 50,
						1, 500);
					FString RowFilter;
					Body->TryGetStringField(TEXT("row"), RowFilter);

					TArray<TSharedPtr<FJsonValue>> Rows;
					int32 Total = 0;
					for (const TPair<FName, uint8*>& Pair : Table->GetRowMap())
					{
						if (!RowFilter.IsEmpty() && Pair.Key.ToString() != RowFilter)
						{
							continue;
						}
						++Total;
						if (Rows.Num() >= MaxRows)
						{
							continue;
						}
						const TSharedRef<FJsonObject> Row = RowToJson(Table, Pair.Value);
						Row->SetStringField(TEXT("__row_name"), Pair.Key.ToString());
						Rows.Add(MakeShared<FJsonValueObject>(Row));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Total);
					Data->SetArrayField(TEXT("rows"), Rows);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_row"))
				{
					FString RowName;
					if (!RequireString(Body, TEXT("row"), RowName, Responder))
					{
						return;
					}
					const TSharedPtr<FJsonObject>* Values = nullptr;
					if (!Body->TryGetObjectField(TEXT("values"), Values) || Values == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'values' object is required — use get_info to see the columns"));
						return;
					}
					if (Table->RowStruct == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_row_struct"),
							TEXT("this DataTable has no row struct assigned"));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetDataTableRow", "McpLink Set DataTable Row"));
					Table->Modify();

					const FName Key(*RowName);
					uint8* Existing = Table->GetRowMap().FindRef(Key);
					// Start from the current row so a partial update keeps other columns.
					TArray<uint8> Buffer;
					Buffer.SetNumZeroed(Table->RowStruct->GetStructureSize());
					Table->RowStruct->InitializeStruct(Buffer.GetData());
					if (Existing != nullptr)
					{
						Table->RowStruct->CopyScriptStruct(Buffer.GetData(), Existing);
					}
					if (!FJsonObjectConverter::JsonObjectToUStruct(
						Values->ToSharedRef(), Table->RowStruct, Buffer.GetData(), 0, 0))
					{
						Table->RowStruct->DestroyStruct(Buffer.GetData());
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_values"),
							TEXT("could not apply 'values' to the row struct — check column names and types"));
						return;
					}
					Table->AddRow(Key, *reinterpret_cast<FTableRowBase*>(Buffer.GetData()));
					Table->RowStruct->DestroyStruct(Buffer.GetData());
					Table->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("row"), RowName);
					Data->SetBoolField(TEXT("created"), Existing == nullptr);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("delete_row"))
				{
					FString RowName;
					if (!RequireString(Body, TEXT("row"), RowName, Responder))
					{
						return;
					}
					const FName Key(*RowName);
					if (Table->GetRowMap().FindRef(Key) == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("row_not_found"),
							FString::Printf(TEXT("no row '%s' in %s"), *RowName, *Table->GetName()));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "DeleteDataTableRow", "McpLink Delete DataTable Row"));
					Table->Modify();
					Table->RemoveRow(Key);
					Table->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), RowName);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("import_csv"))
				{
					FString Csv;
					if (!RequireString(Body, TEXT("csv"), Csv, Responder,
						TEXT("CSV text whose first column is the row name")))
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "ImportDataTableCsv", "McpLink Import DataTable CSV"));
					Table->Modify();
					const TArray<FString> Problems = Table->CreateTableFromCSVString(Csv);
					Table->MarkPackageDirty();

					TArray<TSharedPtr<FJsonValue>> ProblemValues;
					for (const FString& Problem : Problems)
					{
						ProblemValues.Add(MakeShared<FJsonValueString>(Problem));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("row_count"), Table->GetRowMap().Num());
					Data->SetArrayField(TEXT("problems"), ProblemValues);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Table, Filename, Error))
					{
						Responder->Error(
							EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("saved"), true);
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, get_info, get_rows, set_row, delete_row, ")
						TEXT("import_csv, or save"),
						*Operation));
			});
	}
}
