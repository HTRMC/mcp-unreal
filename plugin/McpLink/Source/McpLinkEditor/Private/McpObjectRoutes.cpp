// Native reflection: get/set any UPROPERTY, call any UFUNCTION — the
// replacement for the Remote Control API dependency.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "JsonObjectConverter.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpReflection.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

namespace McpLink
{
	namespace
	{
		UObject* ObjectOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!Body->TryGetStringField(TEXT("object_path"), Path) || Path.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'object_path' is required (e.g. /Game/Maps/Map.Map:PersistentLevel.MyActor)"));
				return nullptr;
			}
			UObject* Object = ResolveObject(Path);
			if (Object == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("object_not_found"),
					FString::Printf(TEXT("no object at path '%s'"), *Path));
			}
			return Object;
		}

		FProperty* PropertyOrError(
			UObject* Object, const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Name;
			if (!Body->TryGetStringField(TEXT("property"), Name) || Name.IsEmpty())
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'property' is required"));
				return nullptr;
			}
			FProperty* Property = Object->GetClass()->FindPropertyByName(FName(*Name));
			if (Property == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("property_not_found"),
					FString::Printf(TEXT("class %s has no property '%s'"),
						*Object->GetClass()->GetName(), *Name));
			}
			return Property;
		}

	}

	void RegisterObjectRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/object/get_property"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				UObject* Object = ObjectOrError(Body, Responder);
				if (!Object) { return; }
				FProperty* Property = PropertyOrError(Object, Body, Responder);
				if (!Property) { return; }

				const TSharedPtr<FJsonValue> Value = FJsonObjectConverter::UPropertyToJsonValue(
					Property, Property->ContainerPtrToValuePtr<void>(Object),
					0, 0, nullptr, nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetField(TEXT("value"), Value);
				Data->SetStringField(TEXT("type"), Property->GetCPPType());
				Responder->Ok(Data);
			});

		Core.RegisterRoute(TEXT("/api/object/set_property"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				UObject* Object = ObjectOrError(Body, Responder);
				if (!Object) { return; }
				FProperty* Property = PropertyOrError(Object, Body, Responder);
				if (!Property) { return; }
				const TSharedPtr<FJsonValue> Value = Body->TryGetField(TEXT("value"));
				if (!Value.IsValid())
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
						TEXT("'value' is required"));
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "SetProperty", "McpLink Set Property"), ShouldTransact(Object));
				Object->Modify();
				Object->PreEditChange(Property);

				FText FailReason;
				const bool bImported = FJsonObjectConverter::JsonValueToUProperty(
					Value, Property, Property->ContainerPtrToValuePtr<void>(Object),
					0, 0, false, &FailReason);

				FPropertyChangedEvent ChangedEvent(Property);
				Object->PostEditChangeProperty(ChangedEvent);

				if (!bImported)
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_value"),
						FString::Printf(TEXT("cannot convert value to %s: %s"),
							*Property->GetCPPType(), *FailReason.ToString()));
					return;
				}
				Object->MarkPackageDirty();

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetField(TEXT("value"), FJsonObjectConverter::UPropertyToJsonValue(
					Property, Property->ContainerPtrToValuePtr<void>(Object),
					0, 0, nullptr, nullptr, EJsonObjectConversionFlags::SkipStandardizeCase));
				Responder->Ok(Data);
			});

		Core.RegisterRoute(TEXT("/api/object/call_function"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				UObject* Object = ObjectOrError(Body, Responder);
				if (!Object) { return; }
				FString FunctionName;
				if (!Body->TryGetStringField(TEXT("function"), FunctionName) || FunctionName.IsEmpty())
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
						TEXT("'function' is required"));
					return;
				}
				UFunction* Function = Object->FindFunction(FName(*FunctionName));
				if (Function == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("function_not_found"),
						FString::Printf(TEXT("class %s has no function '%s'"),
							*Object->GetClass()->GetName(), *FunctionName));
					return;
				}

				const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
				Body->TryGetObjectField(TEXT("args"), ArgsPtr);
				const TSharedPtr<FJsonObject> Args = ArgsPtr ? *ArgsPtr : nullptr;

				const TSharedRef<FJsonObject> Outputs = MakeShared<FJsonObject>();
				FString ImportError;
				{
					// Objects created while a transaction is open are recorded in the
					// undo buffer (StaticConstructObject_Internal), so a call that spawns
					// runtime objects during PIE (e.g. WidgetBlueprintLibrary::Create on
					// a CDO) would pin the PIE world — no transaction while PIE runs.
					const bool bPieRunning = GEditor != nullptr && GEditor->PlayWorld != nullptr;
					FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CallFunction", "McpLink Call Function"),
						ShouldTransact(Object) && !bPieRunning);
					Object->Modify();
					if (!CallFunctionFromJson(Object, Function, Args, Outputs, ImportError))
					{
						Transaction.Cancel();
					}
				}
				if (!ImportError.IsEmpty())
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_argument"), ImportError);
					return;
				}

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetObjectField(TEXT("outputs"), Outputs);
				Responder->Ok(Data);
			});
	}
}
