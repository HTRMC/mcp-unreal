// Blackboard assets: create one, and add, retype or remove its keys.
//
// A blackboard key is a UBlackboardKeyType instance owned by the asset, so the
// key "type" on the wire is a small vocabulary that maps onto those classes;
// the object/class/enum forms carry the extra property the type needs.

#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/BlackboardData.h"
#include "BlackboardDataFactory.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "McpAiUtils.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

namespace McpLink
{
	namespace
	{
		UBlackboardData* BlackboardOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("blackboard"), Path, Responder,
				TEXT("a Blackboard asset path, e.g. /Game/AI/BB_Guard")))
			{
				return nullptr;
			}
			UObject* Object = ResolveObject(Path);
			if (Object == nullptr && !Path.Contains(TEXT(".")))
			{
				Object = ResolveObject(
					FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path)));
			}
			UBlackboardData* Blackboard = Cast<UBlackboardData>(Object);
			if (Blackboard == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("blackboard_not_found"),
					FString::Printf(TEXT("no Blackboard asset at '%s'"), *Path));
			}
			return Blackboard;
		}

		TSharedRef<FJsonObject> KeyToJson(const FBlackboardEntry& Entry)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("name"), Entry.EntryName.ToString());
			Object->SetStringField(TEXT("type"),
				Entry.KeyType ? Entry.KeyType->GetClass()->GetName() : TEXT("(none)"));
			if (const UBlackboardKeyType_Object* AsObject =
				Cast<UBlackboardKeyType_Object>(Entry.KeyType))
			{
				Object->SetStringField(TEXT("base_class"),
					AsObject->BaseClass ? AsObject->BaseClass->GetPathName() : TEXT(""));
			}
			else if (const UBlackboardKeyType_Class* AsClass =
				Cast<UBlackboardKeyType_Class>(Entry.KeyType))
			{
				Object->SetStringField(TEXT("base_class"),
					AsClass->BaseClass ? AsClass->BaseClass->GetPathName() : TEXT(""));
			}
			else if (const UBlackboardKeyType_Enum* AsEnum =
				Cast<UBlackboardKeyType_Enum>(Entry.KeyType))
			{
				Object->SetStringField(TEXT("enum"),
					AsEnum->EnumType ? AsEnum->EnumType->GetPathName() : TEXT(""));
			}
			Object->SetBoolField(TEXT("instance_synced"), Entry.bInstanceSynced != 0);
			// The path a set_property call would use to reach the key's own
			// options (BaseClass, EnumType, ...).
			if (Entry.KeyType != nullptr)
			{
				Object->SetStringField(TEXT("key_type_path"), Entry.KeyType->GetPathName());
			}
			return Object;
		}
	}

	void RegisterBlackboardRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/ai/blackboard"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
						TEXT("e.g. /Game/AI/BB_Guard")))
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateBlackboard", "McpLink Create Blackboard"));
					UBlackboardDataFactory* Factory = NewObject<UBlackboardDataFactory>();
					FString Error;
					UObject* Asset =
						CreateAsset(Path, UBlackboardData::StaticClass(), Factory, Error);
					if (Asset == nullptr)
					{
						Responder->Error(
							EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Asset->GetPathName());
					Responder->Ok(Data);
					return;
				}

				UBlackboardData* Blackboard = BlackboardOrError(Body, Responder);
				if (!Blackboard) { return; }

				if (Operation == TEXT("get_keys"))
				{
					TArray<TSharedPtr<FJsonValue>> Keys;
					for (const FBlackboardEntry& Entry : Blackboard->Keys)
					{
						Keys.Add(MakeShared<FJsonValueObject>(KeyToJson(Entry)));
					}
					TArray<TSharedPtr<FJsonValue>> Inherited;
					for (const FBlackboardEntry& Entry : Blackboard->ParentKeys)
					{
						Inherited.Add(MakeShared<FJsonValueObject>(KeyToJson(Entry)));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Blackboard->GetPathName());
					Data->SetStringField(TEXT("parent"),
						Blackboard->Parent ? Blackboard->Parent->GetPathName() : TEXT(""));
					Data->SetArrayField(TEXT("keys"), Keys);
					Data->SetArrayField(TEXT("inherited_keys"), Inherited);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_key") || Operation == TEXT("set_key"))
				{
					FString Name, TypeSpec;
					if (!RequireString(Body, TEXT("name"), Name, Responder)
						|| !RequireString(Body, TEXT("type"), TypeSpec, Responder,
							TEXT("bool, int, float, string, name, vector, rotator, object:<Class>, class:<Class>, enum:<Enum>")))
					{
						return;
					}
					const int32 Existing = Blackboard->Keys.IndexOfByPredicate(
						[&Name](const FBlackboardEntry& Entry)
						{
							return Entry.EntryName == FName(*Name);
						});
					if (Existing != INDEX_NONE && Operation == TEXT("add_key"))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("key_exists"),
							FString::Printf(
								TEXT("'%s' already exists — use set_key to change its type"), *Name));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddBlackboardKey", "McpLink Add Blackboard Key"));
					Blackboard->Modify();

					FString Error;
					UBlackboardKeyType* KeyType = Ai::MakeKeyType(Blackboard, TypeSpec, Error);
					if (KeyType == nullptr)
					{
						Responder->Error(
							EHttpServerResponseCodes::BadRequest, TEXT("unknown_key_type"), Error);
						return;
					}
					FBlackboardEntry Entry;
					Entry.EntryName = FName(*Name);
					Entry.KeyType = KeyType;
					Entry.bInstanceSynced = BoolOr(Body, TEXT("instance_synced"), false) ? 1 : 0;
					FString Description;
					if (Body->TryGetStringField(TEXT("description"), Description))
					{
						Entry.EntryDescription = Description;
					}

					if (Existing != INDEX_NONE)
					{
						Blackboard->Keys[Existing] = Entry;
					}
					else
					{
						Blackboard->Keys.Add(Entry);
					}
					Blackboard->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data =
						KeyToJson(Blackboard->Keys[Existing != INDEX_NONE ? Existing : Blackboard->Keys.Num() - 1]);
					Data->SetBoolField(TEXT("created"), Existing == INDEX_NONE);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_key"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder))
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "RemoveBlackboardKey", "McpLink Remove Blackboard Key"));
					Blackboard->Modify();
					const int32 Removed = Blackboard->Keys.RemoveAll(
						[&Name](const FBlackboardEntry& Entry)
						{
							return Entry.EntryName == FName(*Name);
						});
					if (Removed == 0)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("key_not_found"),
							FString::Printf(TEXT("no key '%s' on %s"), *Name, *Blackboard->GetName()));
						return;
					}
					Blackboard->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), Name);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_parent"))
				{
					FString ParentPath;
					Body->TryGetStringField(TEXT("parent"), ParentPath);
					UBlackboardData* Parent = nullptr;
					if (!ParentPath.IsEmpty())
					{
						Parent = Cast<UBlackboardData>(ResolveObject(
							ParentPath.Contains(TEXT(".")) ? ParentPath
								: FString::Printf(TEXT("%s.%s"), *ParentPath,
									*FPackageName::GetShortName(ParentPath))));
						if (Parent == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound,
								TEXT("blackboard_not_found"),
								FString::Printf(TEXT("no Blackboard asset at '%s'"), *ParentPath));
							return;
						}
						if (Parent == Blackboard)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest,
								TEXT("circular_parent"),
								TEXT("a Blackboard cannot be its own parent"));
							return;
						}
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetBlackboardParent", "McpLink Set Blackboard Parent"));
					Blackboard->Modify();
					Blackboard->Parent = Parent;
					Blackboard->UpdateParentKeys();
					Blackboard->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("parent"), Parent ? Parent->GetPathName() : TEXT(""));
					Data->SetNumberField(TEXT("inherited_keys"), Blackboard->ParentKeys.Num());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Blackboard, Filename, Error))
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
						TEXT("unknown operation '%s' — use create, get_keys, add_key, set_key, ")
						TEXT("remove_key, set_parent or save"),
						*Operation));
			});
	}
}
