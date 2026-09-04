// User-Defined Structs and Enums: the members and enumerators inside them.
//
// asset_ops can create either asset, but an empty struct or a one-entry enum is
// no use on its own — these are the routes that fill them in. Both types keep
// editor-side descriptions alongside the generated UStruct/UEnum, so every edit
// goes through FStructureEditorUtils / FEnumEditorUtils, which recompile the
// type and fix up everything referencing it.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/UserDefinedEnum.h"
#include "Factories/EnumFactory.h"
#include "Factories/StructureFactory.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "McpAssetUtils.h"
#include "McpBlueprintUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace McpLink
{
	namespace
	{
		UObject* LoadAssetByPath(const FString& Path)
		{
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

		UUserDefinedStruct* StructOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("struct"), Path, Responder,
				TEXT("a User-Defined Struct asset path, e.g. /Game/Data/S_Item")))
			{
				return nullptr;
			}
			UUserDefinedStruct* Struct = Cast<UUserDefinedStruct>(LoadAssetByPath(Path));
			if (Struct == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("struct_not_found"),
					FString::Printf(
						TEXT("no User-Defined Struct at '%s' — this route edits Blueprint structs, ")
						TEXT("not C++ ones"),
						*Path));
			}
			return Struct;
		}

		UUserDefinedEnum* EnumOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("enum"), Path, Responder,
				TEXT("a User-Defined Enum asset path, e.g. /Game/Data/E_Team")))
			{
				return nullptr;
			}
			UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(LoadAssetByPath(Path));
			if (Enum == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("enum_not_found"),
					FString::Printf(
						TEXT("no User-Defined Enum at '%s' — this route edits Blueprint enums, ")
						TEXT("not C++ ones"),
						*Path));
			}
			return Enum;
		}

		/// Members are addressed by their friendly (display) name, which is what
		/// the editor shows and what Blueprint pins are labelled with; VarName
		/// carries a GUID suffix nobody would type.
		const FStructVariableDescription* FindMember(
			const UUserDefinedStruct* Struct, const FString& Name)
		{
			for (const FStructVariableDescription& Description :
				FStructureEditorUtils::GetVarDesc(Struct))
			{
				if (Description.FriendlyName == Name)
				{
					return &Description;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> MemberToJson(const FStructVariableDescription& Description)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Description.FriendlyName);
			Object->SetStringField(TEXT("type"), Description.Category.ToString());
			if (!Description.SubCategory.IsNone())
			{
				Object->SetStringField(TEXT("subtype"), Description.SubCategory.ToString());
			}
			if (!Description.SubCategoryObject.IsNull())
			{
				Object->SetStringField(TEXT("type_object"), Description.SubCategoryObject.ToString());
			}
			switch (Description.ContainerType)
			{
			case EPinContainerType::Array: Object->SetStringField(TEXT("container"), TEXT("array")); break;
			case EPinContainerType::Set: Object->SetStringField(TEXT("container"), TEXT("set")); break;
			case EPinContainerType::Map: Object->SetStringField(TEXT("container"), TEXT("map")); break;
			default: break;
			}
			Object->SetStringField(TEXT("default"), Description.DefaultValue);
			// The internal name, in case something needs to match a property.
			Object->SetStringField(TEXT("property"), Description.VarName.ToString());
			return Object;
		}

		/// User-defined enums always carry a trailing _MAX entry the editor hides.
		int32 VisibleEnumCount(const UUserDefinedEnum* Enum)
		{
			return FMath::Max(0, Enum->NumEnums() - 1);
		}
	}

	void RegisterUserTypeRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/types/user_defined"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				// ---- creation ----
				if (Operation == TEXT("create_struct") || Operation == TEXT("create_enum"))
				{
					const bool bStruct = Operation == TEXT("create_struct");
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
						bStruct ? TEXT("e.g. /Game/Data/S_Item") : TEXT("e.g. /Game/Data/E_Team")))
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateUserType", "McpLink Create User-Defined Type"));
					UFactory* Factory = bStruct
						? static_cast<UFactory*>(NewObject<UStructureFactory>())
						: static_cast<UFactory*>(NewObject<UEnumFactory>());
					FString Error;
					UObject* Asset = CreateAsset(
						Path,
						bStruct ? UUserDefinedStruct::StaticClass() : UUserDefinedEnum::StaticClass(),
						Factory, Error);
					if (Asset == nullptr)
					{
						Responder->Error(
							EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Asset->GetPathName());
					if (bStruct)
					{
						// A new struct comes with one placeholder member.
						Data->SetStringField(TEXT("note"),
							TEXT("a new struct starts with one default member — rename or remove it"));
					}
					Responder->Ok(Data);
					return;
				}

				// ---- structs ----
				if (Operation.Contains(TEXT("struct")) || Operation == TEXT("get_struct"))
				{
					UUserDefinedStruct* Struct = StructOrError(Body, Responder);
					if (!Struct) { return; }

					if (Operation == TEXT("get_struct"))
					{
						TArray<TSharedPtr<FJsonValue>> Members;
						for (const FStructVariableDescription& Description :
							FStructureEditorUtils::GetVarDesc(Struct))
						{
							Members.Add(MakeShared<FJsonValueObject>(MemberToJson(Description)));
						}
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("path"), Struct->GetPathName());
						Data->SetStringField(TEXT("tooltip"), FStructureEditorUtils::GetTooltip(Struct));
						Data->SetArrayField(TEXT("members"), Members);
						Responder->Ok(Data);
						return;
					}

					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder,
						TEXT("the member's name as get_struct reports it")))
					{
						return;
					}

					if (Operation == TEXT("add_struct_member"))
					{
						FString TypeName;
						if (!RequireString(Body, TEXT("type"), TypeName, Responder,
							TEXT("bool, int, float, string, name, text, vector, struct:<Struct>, ")
							TEXT("enum:<Enum>, object:<Class>, array<int>, ...")))
						{
							return;
						}
						FEdGraphPinType PinType;
						FString TypeError;
						if (!MakePinType(TypeName, PinType, TypeError))
						{
							Responder->Error(
								EHttpServerResponseCodes::BadRequest, TEXT("unknown_type"), TypeError);
							return;
						}
						if (FindMember(Struct, Name) != nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("member_exists"),
								FString::Printf(TEXT("'%s' already exists on %s"), *Name, *Struct->GetName()));
							return;
						}
						const FScopedTransaction Transaction(
							NSLOCTEXT("McpLink", "AddStructMember", "McpLink Add Struct Member"));
						// AddVariable names the new member itself, so it has to
						// be renamed afterwards; the new one is always last.
						if (!FStructureEditorUtils::AddVariable(Struct, PinType))
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("add_failed"),
								TEXT("the struct refused the new member"));
							return;
						}
						const TArray<FStructVariableDescription>& Members =
							FStructureEditorUtils::GetVarDesc(Struct);
						const FGuid NewGuid = Members.Last().VarGuid;
						if (!FStructureEditorUtils::RenameVariable(Struct, NewGuid, Name))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("rename_failed"),
								FString::Printf(
									TEXT("the member was added but could not be named '%s' — the name ")
									TEXT("may be reserved or already used"),
									*Name));
							return;
						}
						FString DefaultValue;
						if (Body->TryGetStringField(TEXT("default"), DefaultValue)
							&& !DefaultValue.IsEmpty())
						{
							FStructureEditorUtils::ChangeVariableDefaultValue(
								Struct, NewGuid, DefaultValue);
						}
						const FStructVariableDescription* Added = FindMember(Struct, Name);
						Responder->Ok(Added != nullptr ? MemberToJson(*Added) : MakeShared<FJsonObject>());
						return;
					}

					const FStructVariableDescription* Member = FindMember(Struct, Name);
					if (Member == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("member_not_found"),
							FString::Printf(
								TEXT("no member '%s' on %s — get_struct lists them"),
								*Name, *Struct->GetName()));
						return;
					}
					const FGuid MemberGuid = Member->VarGuid;

					if (Operation == TEXT("remove_struct_member"))
					{
						const FScopedTransaction Transaction(
							NSLOCTEXT("McpLink", "RemoveStructMember", "McpLink Remove Struct Member"));
						if (!FStructureEditorUtils::RemoveVariable(Struct, MemberGuid))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("remove_failed"),
								TEXT("a struct must keep at least one member"));
							return;
						}
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("removed"), Name);
						Responder->Ok(Data);
						return;
					}

					if (Operation == TEXT("rename_struct_member"))
					{
						FString NewName;
						if (!RequireString(Body, TEXT("new_name"), NewName, Responder))
						{
							return;
						}
						const FScopedTransaction Transaction(
							NSLOCTEXT("McpLink", "RenameStructMember", "McpLink Rename Struct Member"));
						if (!FStructureEditorUtils::RenameVariable(Struct, MemberGuid, NewName))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("rename_failed"),
								FString::Printf(TEXT("could not rename to '%s'"), *NewName));
							return;
						}
						const FStructVariableDescription* Renamed = FindMember(Struct, NewName);
						Responder->Ok(
							Renamed != nullptr ? MemberToJson(*Renamed) : MakeShared<FJsonObject>());
						return;
					}

					if (Operation == TEXT("set_struct_member_type"))
					{
						FString TypeName;
						if (!RequireString(Body, TEXT("type"), TypeName, Responder))
						{
							return;
						}
						FEdGraphPinType PinType;
						FString TypeError;
						if (!MakePinType(TypeName, PinType, TypeError))
						{
							Responder->Error(
								EHttpServerResponseCodes::BadRequest, TEXT("unknown_type"), TypeError);
							return;
						}
						const FScopedTransaction Transaction(
							NSLOCTEXT("McpLink", "SetStructMemberType", "McpLink Set Struct Member Type"));
						if (!FStructureEditorUtils::ChangeVariableType(Struct, MemberGuid, PinType))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("retype_failed"),
								TEXT("the struct refused the new type"));
							return;
						}
						const FStructVariableDescription* Changed = FindMember(Struct, Name);
						Responder->Ok(
							Changed != nullptr ? MemberToJson(*Changed) : MakeShared<FJsonObject>());
						return;
					}

					if (Operation == TEXT("set_struct_member_default"))
					{
						FString DefaultValue;
						Body->TryGetStringField(TEXT("default"), DefaultValue);
						const FScopedTransaction Transaction(
							NSLOCTEXT("McpLink", "SetStructMemberDefault",
								"McpLink Set Struct Member Default"));
						if (!FStructureEditorUtils::ChangeVariableDefaultValue(
							Struct, MemberGuid, DefaultValue))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_default"),
								FString::Printf(
									TEXT("'%s' is not a valid default for this member's type"),
									*DefaultValue));
							return;
						}
						const FStructVariableDescription* Changed = FindMember(Struct, Name);
						Responder->Ok(
							Changed != nullptr ? MemberToJson(*Changed) : MakeShared<FJsonObject>());
						return;
					}
				}

				// ---- enums ----
				if (Operation.Contains(TEXT("enum")))
				{
					UUserDefinedEnum* Enum = EnumOrError(Body, Responder);
					if (!Enum) { return; }

					const auto EntriesJson = [Enum]()
					{
						TArray<TSharedPtr<FJsonValue>> Entries;
						for (int32 Index = 0; Index < VisibleEnumCount(Enum); ++Index)
						{
							const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
							Entry->SetNumberField(TEXT("index"), Index);
							Entry->SetStringField(TEXT("name"),
								Enum->GetDisplayNameTextByIndex(Index).ToString());
							Entry->SetStringField(TEXT("internal_name"),
								Enum->GetNameStringByIndex(Index));
							Entries.Add(MakeShared<FJsonValueObject>(Entry));
						}
						return Entries;
					};

					if (Operation == TEXT("get_enum"))
					{
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("path"), Enum->GetPathName());
						Data->SetArrayField(TEXT("entries"), EntriesJson());
						Responder->Ok(Data);
						return;
					}

					if (Operation == TEXT("add_enum_entry"))
					{
						FString Name;
						if (!RequireString(Body, TEXT("name"), Name, Responder))
						{
							return;
						}
						const FScopedTransaction Transaction(
							NSLOCTEXT("McpLink", "AddEnumEntry", "McpLink Add Enum Entry"));
						// The new entry lands last with a generated name.
						FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
						const int32 NewIndex = VisibleEnumCount(Enum) - 1;
						if (NewIndex < 0
							|| !FEnumEditorUtils::SetEnumeratorDisplayName(
								Enum, NewIndex, FText::FromString(Name)))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("name_taken"),
								FString::Printf(
									TEXT("the entry was added but '%s' could not be used as its name — ")
									TEXT("display names must be unique within the enum"),
									*Name));
							return;
						}
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetNumberField(TEXT("index"), NewIndex);
						Data->SetStringField(TEXT("name"), Name);
						Data->SetArrayField(TEXT("entries"), EntriesJson());
						Responder->Ok(Data);
						return;
					}

					// The remaining operations address one existing entry.
					FString Name;
					Body->TryGetStringField(TEXT("name"), Name);
					int32 Index = IntOr(Body, TEXT("index"), INDEX_NONE);
					if (Index == INDEX_NONE && !Name.IsEmpty())
					{
						for (int32 Candidate = 0; Candidate < VisibleEnumCount(Enum); ++Candidate)
						{
							if (Enum->GetDisplayNameTextByIndex(Candidate).ToString() == Name)
							{
								Index = Candidate;
								break;
							}
						}
					}
					if (Index < 0 || Index >= VisibleEnumCount(Enum))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("entry_not_found"),
							FString::Printf(
								TEXT("no entry '%s' on %s — get_enum lists them"),
								*Name, *Enum->GetName()));
						return;
					}

					if (Operation == TEXT("remove_enum_entry"))
					{
						const FScopedTransaction Transaction(
							NSLOCTEXT("McpLink", "RemoveEnumEntry", "McpLink Remove Enum Entry"));
						FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum(Enum, Index);
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetNumberField(TEXT("removed_index"), Index);
						Data->SetArrayField(TEXT("entries"), EntriesJson());
						Responder->Ok(Data);
						return;
					}

					if (Operation == TEXT("rename_enum_entry"))
					{
						FString NewName;
						if (!RequireString(Body, TEXT("new_name"), NewName, Responder))
						{
							return;
						}
						const FScopedTransaction Transaction(
							NSLOCTEXT("McpLink", "RenameEnumEntry", "McpLink Rename Enum Entry"));
						if (!FEnumEditorUtils::SetEnumeratorDisplayName(
							Enum, Index, FText::FromString(NewName)))
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("name_taken"),
								FString::Printf(
									TEXT("'%s' is already used by another entry"), *NewName));
							return;
						}
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetNumberField(TEXT("index"), Index);
						Data->SetStringField(TEXT("name"), NewName);
						Data->SetArrayField(TEXT("entries"), EntriesJson());
						Responder->Ok(Data);
						return;
					}
				}

				if (Operation == TEXT("save"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
						TEXT("the struct or enum asset path")))
					{
						return;
					}
					UObject* Asset = LoadAssetByPath(Path);
					if (Asset == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
							FString::Printf(TEXT("no asset at '%s'"), *Path));
						return;
					}
					FString Filename, Error;
					if (!SaveAsset(Asset, Filename, Error))
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
						TEXT("unknown operation '%s' — use create_struct, get_struct, ")
						TEXT("add_struct_member, remove_struct_member, rename_struct_member, ")
						TEXT("set_struct_member_type, set_struct_member_default, create_enum, ")
						TEXT("get_enum, add_enum_entry, remove_enum_entry, rename_enum_entry or save"),
						*Operation));
			});
	}
}
