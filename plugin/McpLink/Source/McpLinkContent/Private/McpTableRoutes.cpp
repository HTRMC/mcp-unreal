// String Tables and Curve Tables: the two table assets data_table_ops does
// not cover. A String Table maps keys to source strings (and dev notes) for
// localised text; a Curve Table holds one named curve per row.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/RichCurve.h"
#include "Curves/SimpleCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/CurveTable.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace Tables
	{
		/// A new asset package, refusing paths that already hold something —
		/// including the empty package a failed TryLoad leaves behind.
		UPackage* NewAssetPackage(
			const FString& Path, const TSharedRef<FMcpResponder>& Responder, FString& OutAssetName)
		{
			if (!FPackageName::IsValidLongPackageName(Path))
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_path"),
					FString::Printf(TEXT("'%s' is not a package path — use /Game/Folder/Name"), *Path));
				return nullptr;
			}
			if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
					FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
				return nullptr;
			}
			OutAssetName = FPackageName::GetShortName(Path);
			return CreatePackage(*Path);
		}

		UStringTable* StringTableOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("table"), Path, Responder, TEXT("a String Table asset path")))
			{
				return nullptr;
			}
			UStringTable* Table = Cast<UStringTable>(ResolveAsset(Path));
			if (Table == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("table_not_found"),
					FString::Printf(TEXT("no String Table at '%s'"), *Path));
			}
			return Table;
		}

		UCurveTable* CurveTableOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("table"), Path, Responder, TEXT("a Curve Table asset path")))
			{
				return nullptr;
			}
			UCurveTable* Table = Cast<UCurveTable>(ResolveAsset(Path));
			if (Table == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("table_not_found"),
					FString::Printf(TEXT("no Curve Table at '%s'"), *Path));
			}
			return Table;
		}

		TSharedRef<FJsonObject> StringTableJson(UStringTable& Table, int32 MaxEntries)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("table"), Table.GetPathName());
			const FStringTableConstRef Strings = Table.GetStringTable();
			Data->SetStringField(TEXT("namespace"), Strings->GetNamespace());
			TArray<TSharedPtr<FJsonValue>> Entries;
			int32 Total = 0;
			Strings->EnumerateKeysAndSourceStrings(
				[&Entries, &Total, MaxEntries](const FTextKey& Key, const FString& Source, const FString& DevNotes) -> bool
				{
					++Total;
					if (Entries.Num() < MaxEntries)
					{
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("key"), Key.ToString());
						Entry->SetStringField(TEXT("text"), Source);
						if (!DevNotes.IsEmpty())
						{
							Entry->SetStringField(TEXT("notes"), DevNotes);
						}
						Entries.Add(MakeShared<FJsonValueObject>(Entry));
					}
					return true;
				});
			Data->SetArrayField(TEXT("strings"), Entries);
			Data->SetNumberField(TEXT("count"), Total);
			Data->SetBoolField(TEXT("truncated"), Total > Entries.Num());
			return Data;
		}

		FString InterpName(ERichCurveInterpMode Mode)
		{
			switch (Mode)
			{
			case RCIM_Linear: return TEXT("linear");
			case RCIM_Constant: return TEXT("constant");
			case RCIM_Cubic: return TEXT("cubic");
			default: return TEXT("none");
			}
		}

		bool ParseInterp(const FString& Spec, ERichCurveInterpMode& OutMode)
		{
			const FString Lower = Spec.ToLower();
			if (Lower.IsEmpty() || Lower == TEXT("linear")) { OutMode = RCIM_Linear; return true; }
			if (Lower == TEXT("constant")) { OutMode = RCIM_Constant; return true; }
			if (Lower == TEXT("cubic") || Lower == TEXT("auto")) { OutMode = RCIM_Cubic; return true; }
			return false;
		}

		/// The table's mode says what its rows are; RTTI is off, so the
		/// cast is by mode rather than dynamic.
		TSharedRef<FJsonObject> CurveRowJson(const FName& Row, const FRealCurve* Curve, bool bRich, bool bIncludeKeys)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("row"), Row.ToString());
			if (Curve == nullptr)
			{
				return Item;
			}
			Item->SetNumberField(TEXT("num_keys"), Curve->GetNumKeys());
			float Min = 0.f, Max = 0.f;
			Curve->GetTimeRange(Min, Max);
			Item->SetNumberField(TEXT("min_time"), Min);
			Item->SetNumberField(TEXT("max_time"), Max);
			if (bIncludeKeys)
			{
				TArray<TSharedPtr<FJsonValue>> Keys;
				if (bRich)
				{
					for (const FRichCurveKey& Key : static_cast<const FRichCurve*>(Curve)->GetConstRefOfKeys())
					{
						const TSharedRef<FJsonObject> KeyJson = MakeShared<FJsonObject>();
						KeyJson->SetNumberField(TEXT("time"), Key.Time);
						KeyJson->SetNumberField(TEXT("value"), Key.Value);
						KeyJson->SetStringField(TEXT("interp"), InterpName(Key.InterpMode));
						Keys.Add(MakeShared<FJsonValueObject>(KeyJson));
					}
				}
				else
				{
					for (const FSimpleCurveKey& Key : static_cast<const FSimpleCurve*>(Curve)->GetConstRefOfKeys())
					{
						const TSharedRef<FJsonObject> KeyJson = MakeShared<FJsonObject>();
						KeyJson->SetNumberField(TEXT("time"), Key.Time);
						KeyJson->SetNumberField(TEXT("value"), Key.Value);
						Keys.Add(MakeShared<FJsonValueObject>(KeyJson));
					}
				}
				Item->SetArrayField(TEXT("keys"), Keys);
			}
			return Item;
		}

		bool IsRich(const UCurveTable& Table)
		{
			return Table.GetCurveTableMode() != ECurveTableMode::SimpleCurves;
		}

		TSharedRef<FJsonObject> CurveTableJson(UCurveTable& Table, bool bIncludeKeys)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("table"), Table.GetPathName());
			const ECurveTableMode Mode = Table.GetCurveTableMode();
			Data->SetStringField(TEXT("mode"),
				Mode == ECurveTableMode::RichCurves ? TEXT("rich")
				: Mode == ECurveTableMode::SimpleCurves ? TEXT("simple") : TEXT("empty"));
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const auto& Pair : Table.GetRowMap())
			{
				Rows.Add(MakeShared<FJsonValueObject>(CurveRowJson(Pair.Key, Pair.Value, IsRich(Table), bIncludeKeys)));
			}
			Data->SetArrayField(TEXT("rows"), Rows);
			Data->SetNumberField(TEXT("count"), Rows.Num());
			return Data;
		}

		FString RowNames(const UCurveTable& Table)
		{
			TArray<FString> Names;
			for (const auto& Pair : Table.GetRowMap())
			{
				Names.Add(Pair.Key.ToString());
			}
			return Names.IsEmpty() ? FString(TEXT("(none)")) : FString::Join(Names, TEXT(", "));
		}
	}

	using namespace Tables;

	void RegisterTableRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/data/string_table"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Text/ST_Dialogue")))
					{
						return;
					}
					FString AssetName;
					UPackage* Package = NewAssetPackage(Path, Responder, AssetName);
					if (Package == nullptr) { return; }
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "CreateStringTable", "McpLink Create String Table"));
					UStringTable* Table = NewObject<UStringTable>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
					FString Namespace;
					if (Body->TryGetStringField(TEXT("namespace"), Namespace) && !Namespace.IsEmpty())
					{
						Table->GetMutableStringTable()->SetNamespace(Namespace);
					}
					FAssetRegistryModule::AssetCreated(Table);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = StringTableJson(*Table, 50);
					Data->SetStringField(TEXT("message"), TEXT("created in memory — string_table_ops save writes it to disk"));
					Responder->Ok(Data);
					return;
				}

				UStringTable* Table = StringTableOrError(Body, Responder);
				if (Table == nullptr) { return; }

				if (Operation == TEXT("info") || Operation == TEXT("get_strings"))
				{
					const int32 Max = Operation == TEXT("info") ? 0 : FMath::Clamp(IntOr(Body, TEXT("max_results"), 500), 1, 20000);
					Responder->Ok(StringTableJson(*Table, Max));
					return;
				}

				if (Operation == TEXT("set_string") || Operation == TEXT("set_strings"))
				{
					// One key, or a batch keyed by key.
					TMap<FString, FString> Values;
					TMap<FString, FString> Notes;
					if (Operation == TEXT("set_string"))
					{
						FString Key, Text;
						if (!RequireString(Body, TEXT("key"), Key, Responder, TEXT("the string's key")))
						{
							return;
						}
						Body->TryGetStringField(TEXT("text"), Text);
						Values.Add(Key, Text);
						FString Note;
						if (Body->TryGetStringField(TEXT("notes"), Note))
						{
							Notes.Add(Key, Note);
						}
					}
					else
					{
						const TSharedPtr<FJsonObject>* Strings = nullptr;
						if (!Body->TryGetObjectField(TEXT("strings"), Strings) || !Strings->IsValid())
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'strings' is required — an object of key to text"));
							return;
						}
						for (const auto& Pair : (*Strings)->Values)
						{
							FString Text;
							if (Pair.Value.IsValid())
							{
								Pair.Value->TryGetString(Text);
							}
							Values.Add(FString(Pair.Key.ToView()), Text);
						}
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "SetStringTableEntry", "McpLink Set String Table Entry"));
					Table->Modify();
					const FStringTableRef Strings = Table->GetMutableStringTable();
					for (const auto& Pair : Values)
					{
						const FString* Note = Notes.Find(Pair.Key);
						Strings->SetSourceString(Pair.Key, Pair.Value, Note != nullptr ? *Note : FString());
					}
					Table->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = StringTableJson(*Table, 50);
					Data->SetNumberField(TEXT("written"), Values.Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_string"))
				{
					FString Key;
					if (!RequireString(Body, TEXT("key"), Key, Responder, TEXT("the key to remove")))
					{
						return;
					}
					FString Existing;
					if (!Table->GetStringTable()->GetSourceString(Key, Existing))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("key_not_found"),
							FString::Printf(TEXT("'%s' has no key '%s'"), *Table->GetName(), *Key));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "RemoveStringTableEntry", "McpLink Remove String Table Entry"));
					Table->Modify();
					Table->GetMutableStringTable()->RemoveSourceString(Key);
					Table->MarkPackageDirty();
					Responder->Ok(StringTableJson(*Table, 50));
					return;
				}

				if (Operation == TEXT("set_namespace"))
				{
					FString Namespace;
					Body->TryGetStringField(TEXT("namespace"), Namespace);
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "SetStringTableNamespace", "McpLink Set String Table Namespace"));
					Table->Modify();
					Table->GetMutableStringTable()->SetNamespace(Namespace);
					Table->MarkPackageDirty();
					Responder->Ok(StringTableJson(*Table, 50));
					return;
				}

				if (Operation == TEXT("import_csv") || Operation == TEXT("export_csv"))
				{
					FString File;
					if (!RequireString(Body, TEXT("file"), File, Responder, TEXT("a CSV file path (Key,SourceString[,Comment])")))
					{
						return;
					}
					bool bOk = false;
					if (Operation == TEXT("import_csv"))
					{
						const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "ImportStringTable", "McpLink Import String Table"));
						Table->Modify();
						bOk = Table->GetMutableStringTable()->ImportStringsFromCSVFile(File);
						Table->MarkPackageDirty();
					}
					else
					{
						bOk = Table->GetStringTable()->ExportStringsToCSVFile(File);
					}
					if (!bOk)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("csv_failed"),
							FString::Printf(TEXT("could not %s '%s'"), Operation == TEXT("import_csv") ? TEXT("import") : TEXT("export"), *File));
						return;
					}
					const TSharedRef<FJsonObject> Data = StringTableJson(*Table, 50);
					Data->SetStringField(TEXT("file"), File);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Table, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("table"), Table->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, info, get_strings, set_string, set_strings, ")
						TEXT("remove_string, set_namespace, import_csv, export_csv or save"),
						*Operation));
			});

		Core.RegisterRoute(TEXT("/api/data/curve_table"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Data/CT_Damage")))
					{
						return;
					}
					FString AssetName;
					UPackage* Package = NewAssetPackage(Path, Responder, AssetName);
					if (Package == nullptr) { return; }
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "CreateCurveTable", "McpLink Create Curve Table"));
					UCurveTable* Table = NewObject<UCurveTable>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
					FAssetRegistryModule::AssetCreated(Table);
					Package->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = CurveTableJson(*Table, false);
					Data->SetStringField(TEXT("message"),
						TEXT("created empty in memory — set_row adds curves (the first decides rich vs simple); curve_table_ops save writes it to disk"));
					Responder->Ok(Data);
					return;
				}

				UCurveTable* Table = CurveTableOrError(Body, Responder);
				if (Table == nullptr) { return; }

				if (Operation == TEXT("info"))
				{
					Responder->Ok(CurveTableJson(*Table, BoolOr(Body, TEXT("include_keys"), false)));
					return;
				}

				if (Operation == TEXT("get_row") || Operation == TEXT("evaluate") || Operation == TEXT("remove_row"))
				{
					FString Row;
					if (!RequireString(Body, TEXT("row"), Row, Responder, TEXT("a row (curve) name")))
					{
						return;
					}
					FRealCurve* const* Found = Table->GetRowMap().Find(FName(*Row));
					if (Found == nullptr || *Found == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("row_not_found"),
							FString::Printf(TEXT("'%s' has no row '%s' — it has: %s"), *Table->GetName(), *Row, *RowNames(*Table)));
						return;
					}
					if (Operation == TEXT("get_row"))
					{
						Responder->Ok(CurveRowJson(FName(*Row), *Found, IsRich(*Table), true));
						return;
					}
					if (Operation == TEXT("evaluate"))
					{
						const double Time = DoubleOr(Body, TEXT("time"), 0.0);
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("row"), Row);
						Data->SetNumberField(TEXT("time"), Time);
						Data->SetNumberField(TEXT("value"), (*Found)->Eval(static_cast<float>(Time)));
						Responder->Ok(Data);
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "RemoveCurveTableRow", "McpLink Remove Curve Table Row"));
					Table->Modify();
					Table->RemoveRow(FName(*Row));
					Table->MarkPackageDirty();
					Responder->Ok(CurveTableJson(*Table, false));
					return;
				}

				if (Operation == TEXT("set_row"))
				{
					FString Row;
					if (!RequireString(Body, TEXT("row"), Row, Responder, TEXT("a row (curve) name")))
					{
						return;
					}
					const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
					if (!Body->TryGetArrayField(TEXT("keys"), Keys))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'keys' is required — [{\"time\": 0, \"value\": 1, \"interp\": \"linear\"}, ...]"));
						return;
					}
					// A fresh table takes its mode from the first row; after
					// that every row shares it.
					const bool bSimple = Table->GetCurveTableMode() == ECurveTableMode::SimpleCurves
						|| (Table->GetCurveTableMode() == ECurveTableMode::Empty
							&& Body->HasField(TEXT("simple")) && BoolOr(Body, TEXT("simple"), false));
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "SetCurveTableRow", "McpLink Set Curve Table Row"));
					Table->Modify();
					const FName RowName(*Row);
					if (Table->GetRowMap().Contains(RowName))
					{
						Table->RemoveRow(RowName);
					}
					FRealCurve* Curve = bSimple
						? static_cast<FRealCurve*>(&Table->AddSimpleCurve(RowName))
						: static_cast<FRealCurve*>(&Table->AddRichCurve(RowName));
					FRichCurve* Rich = bSimple ? nullptr : static_cast<FRichCurve*>(Curve);
					for (const TSharedPtr<FJsonValue>& Value : *Keys)
					{
						const TSharedPtr<FJsonObject>* KeyObject = nullptr;
						if (!Value.IsValid() || !Value->TryGetObject(KeyObject) || !KeyObject->IsValid())
						{
							continue;
						}
						const float Time = static_cast<float>(DoubleOr(KeyObject->ToSharedRef(), TEXT("time"), 0.0));
						const float KeyValue = static_cast<float>(DoubleOr(KeyObject->ToSharedRef(), TEXT("value"), 0.0));
						const FKeyHandle Handle = Curve->AddKey(Time, KeyValue);
						FString Interp;
						(*KeyObject)->TryGetStringField(TEXT("interp"), Interp);
						ERichCurveInterpMode Mode = RCIM_Linear;
						if (!ParseInterp(Interp, Mode))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_interp"),
								FString::Printf(TEXT("unknown interp '%s' — linear, constant or cubic"), *Interp));
							return;
						}
						if (Rich != nullptr)
						{
							Rich->SetKeyInterpMode(Handle, Mode);
						}
					}
					Table->MarkPackageDirty();
					Responder->Ok(CurveRowJson(RowName, Curve, !bSimple, true));
					return;
				}

				if (Operation == TEXT("import_csv"))
				{
					FString Csv, File;
					Body->TryGetStringField(TEXT("csv"), Csv);
					Body->TryGetStringField(TEXT("file"), File);
					if (Csv.IsEmpty() && (File.IsEmpty() || !FFileHelper::LoadFileToString(Csv, *File)))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'csv' (text) or 'file' (a readable CSV: first row the times, then one row per curve) is required"));
						return;
					}
					FString InterpSpec;
					Body->TryGetStringField(TEXT("interp"), InterpSpec);
					ERichCurveInterpMode Mode = RCIM_Linear;
					if (!ParseInterp(InterpSpec, Mode))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_interp"),
							FString::Printf(TEXT("unknown interp '%s' — linear, constant or cubic"), *InterpSpec));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "ImportCurveTable", "McpLink Import Curve Table"));
					Table->Modify();
					const TArray<FString> Problems = Table->CreateTableFromCSVString(Csv, Mode);
					Table->MarkPackageDirty();
					if (!Problems.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("csv_problems"),
							FString::Join(Problems, TEXT("; ")));
						return;
					}
					Responder->Ok(CurveTableJson(*Table, false));
					return;
				}

				if (Operation == TEXT("export_csv"))
				{
					const FString Csv = Table->GetTableAsCSV();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("table"), Table->GetPathName());
					FString File;
					if (Body->TryGetStringField(TEXT("file"), File) && !File.IsEmpty())
					{
						if (!FFileHelper::SaveStringToFile(Csv, *File))
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("write_failed"),
								FString::Printf(TEXT("could not write '%s'"), *File));
							return;
						}
						Data->SetStringField(TEXT("file"), File);
					}
					else
					{
						Data->SetStringField(TEXT("csv"), Csv);
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Table, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("table"), Table->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, info, get_row, set_row, remove_row, evaluate, ")
						TEXT("import_csv, export_csv or save"),
						*Operation));
			});
	}
}
