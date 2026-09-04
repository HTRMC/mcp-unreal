// Enhanced Input asset authoring: create Input Actions and Mapping Contexts,
// and bind keys. This is the design-time counterpart to /api/input/inject,
// which drives input at runtime.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EnhancedActionKeyMapping.h"
#include "InputAction.h"
#include "InputMappingContext.h"
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
		template <typename AssetType>
		AssetType* LoadAsset(const FString& Path)
		{
			if (AssetType* Direct = Cast<AssetType>(ResolveObject(Path)))
			{
				return Direct;
			}
			if (!Path.Contains(TEXT(".")))
			{
				return Cast<AssetType>(ResolveObject(
					FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path))));
			}
			return nullptr;
		}

		bool ParseValueType(const FString& Name, EInputActionValueType& OutType)
		{
			if (Name.IsEmpty() || Name.Equals(TEXT("bool"), ESearchCase::IgnoreCase)
				|| Name.Equals(TEXT("digital"), ESearchCase::IgnoreCase))
			{
				OutType = EInputActionValueType::Boolean;
				return true;
			}
			if (Name.Equals(TEXT("float"), ESearchCase::IgnoreCase)
				|| Name.Equals(TEXT("axis1d"), ESearchCase::IgnoreCase))
			{
				OutType = EInputActionValueType::Axis1D;
				return true;
			}
			if (Name.Equals(TEXT("vector2d"), ESearchCase::IgnoreCase)
				|| Name.Equals(TEXT("axis2d"), ESearchCase::IgnoreCase))
			{
				OutType = EInputActionValueType::Axis2D;
				return true;
			}
			if (Name.Equals(TEXT("vector"), ESearchCase::IgnoreCase)
				|| Name.Equals(TEXT("axis3d"), ESearchCase::IgnoreCase))
			{
				OutType = EInputActionValueType::Axis3D;
				return true;
			}
			return false;
		}

		const TCHAR* ValueTypeName(EInputActionValueType Type)
		{
			switch (Type)
			{
				case EInputActionValueType::Boolean: return TEXT("bool");
				case EInputActionValueType::Axis1D: return TEXT("float");
				case EInputActionValueType::Axis2D: return TEXT("vector2d");
				case EInputActionValueType::Axis3D: return TEXT("vector");
				default: return TEXT("unknown");
			}
		}
	}

	void RegisterInputAssetRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/input_assets/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create_action"))
				{
					FString Path, ValueTypeName;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
						TEXT("e.g. /Game/Input/IA_Move")))
					{
						return;
					}
					Body->TryGetStringField(TEXT("value_type"), ValueTypeName);
					EInputActionValueType ValueType;
					if (!ParseValueType(ValueTypeName, ValueType))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_value_type"),
							FString::Printf(
								TEXT("unknown value_type '%s' — use bool, float, vector2d, or vector"),
								*ValueTypeName));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateInputAction", "McpLink Create Input Action"));
					FString Error;
					UObject* Asset = CreateAsset(Path, UInputAction::StaticClass(), nullptr, Error);
					if (Asset == nullptr)
					{
						Responder->Error(
							EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					UInputAction* Action = CastChecked<UInputAction>(Asset);
					Action->ValueType = ValueType;
					Action->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Action->GetPathName());
					Data->SetStringField(TEXT("value_type"), ValueTypeName.IsEmpty() ? TEXT("bool") : ValueTypeName);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_context"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
						TEXT("e.g. /Game/Input/IMC_Default")))
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateInputContext", "McpLink Create Input Mapping Context"));
					FString Error;
					UObject* Asset =
						CreateAsset(Path, UInputMappingContext::StaticClass(), nullptr, Error);
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

				if (Operation == TEXT("map_key") || Operation == TEXT("unmap_key"))
				{
					FString ContextPath, ActionPath, KeyName;
					if (!RequireString(Body, TEXT("context"), ContextPath, Responder,
							TEXT("an Input Mapping Context asset path"))
						|| !RequireString(Body, TEXT("action"), ActionPath, Responder,
							TEXT("an Input Action asset path"))
						|| !RequireString(Body, TEXT("key"), KeyName, Responder,
							TEXT("a UE key name such as W, SpaceBar, Gamepad_LeftX")))
					{
						return;
					}
					UInputMappingContext* Context = LoadAsset<UInputMappingContext>(ContextPath);
					if (Context == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("context_not_found"),
							FString::Printf(TEXT("no Input Mapping Context at '%s'"), *ContextPath));
						return;
					}
					UInputAction* Action = LoadAsset<UInputAction>(ActionPath);
					if (Action == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("action_not_found"),
							FString::Printf(TEXT("no Input Action at '%s'"), *ActionPath));
						return;
					}
					const FKey Key(*KeyName);
					if (!Key.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_key"),
							FString::Printf(TEXT("'%s' is not a valid UE key name"), *KeyName));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "MapInputKey", "McpLink Map Input Key"));
					Context->Modify();
					if (Operation == TEXT("unmap_key"))
					{
						Context->UnmapKey(Action, Key);
					}
					else
					{
						Context->MapKey(Action, Key);
					}
					Context->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("context"), Context->GetPathName());
					Data->SetStringField(TEXT("action"), Action->GetPathName());
					Data->SetStringField(TEXT("key"), KeyName);
					Data->SetBoolField(TEXT("mapped"), Operation == TEXT("map_key"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("get_context"))
				{
					FString ContextPath;
					if (!RequireString(Body, TEXT("context"), ContextPath, Responder,
						TEXT("an Input Mapping Context asset path")))
					{
						return;
					}
					UInputMappingContext* Context = LoadAsset<UInputMappingContext>(ContextPath);
					if (Context == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("context_not_found"),
							FString::Printf(TEXT("no Input Mapping Context at '%s'"), *ContextPath));
						return;
					}

					TArray<TSharedPtr<FJsonValue>> Mappings;
					for (const FEnhancedActionKeyMapping& Mapping : Context->GetMappings())
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("action"),
							Mapping.Action ? Mapping.Action->GetPathName() : TEXT(""));
						Item->SetStringField(TEXT("key"), Mapping.Key.ToString());
						Item->SetNumberField(TEXT("modifiers"), Mapping.Modifiers.Num());
						Item->SetNumberField(TEXT("triggers"), Mapping.Triggers.Num());
						if (Mapping.Action != nullptr)
						{
							Item->SetStringField(
								TEXT("value_type"), ValueTypeName(Mapping.Action->ValueType));
						}
						Mappings.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("context"), Context->GetPathName());
					Data->SetArrayField(TEXT("mappings"), Mappings);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
						TEXT("the Input Action or Mapping Context asset to save")))
					{
						return;
					}
					UObject* Asset = LoadAsset<UObject>(Path);
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
						TEXT("unknown operation '%s' — use create_action, create_context, map_key, ")
						TEXT("unmap_key, get_context, or save"),
						*Operation));
			});
	}
}
