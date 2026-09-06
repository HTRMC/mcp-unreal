#include "McpReflection.h"

#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "UObject/UnrealType.h"

namespace McpLink
{
	namespace
	{
		TSharedPtr<FJsonValue> FindArgumentIgnoreCase(const TSharedPtr<FJsonObject>& Args, const FString& Name)
		{
			if (!Args.IsValid())
			{
				return nullptr;
			}
			for (const auto& Pair : Args->Values)
			{
				if (Pair.Key.ToView().Equals(Name, ESearchCase::IgnoreCase))
				{
					return Pair.Value;
				}
			}
			return nullptr;
		}
	}

	bool CallFunctionFromJson(UObject* Object, UFunction* Function,
		const TSharedPtr<FJsonObject>& Args, const TSharedRef<FJsonObject>& OutOutputs, FString& OutError)
	{
		OutError.Reset();
		if (Object == nullptr || Function == nullptr)
		{
			OutError = TEXT("no object or function");
			return false;
		}

		TArray<uint8> Parms;
		Parms.SetNumZeroed(Function->ParmsSize);
		TArray<FProperty*> ParamProps;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			ParamProps.Add(*It);
			It->InitializeValue_InContainer(Parms.GetData());
		}

		for (FProperty* Param : ParamProps)
		{
			if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			const TSharedPtr<FJsonValue> Arg = FindArgumentIgnoreCase(Args, Param->GetName());
			if (!Arg.IsValid())
			{
				// A missing input keeps the initialized default.
				continue;
			}
			FText FailReason;
			if (!FJsonObjectConverter::JsonValueToUProperty(
				Arg, Param, Param->ContainerPtrToValuePtr<void>(Parms.GetData()), 0, 0, false, &FailReason))
			{
				OutError = FString::Printf(TEXT("argument '%s': %s"), *Param->GetName(), *FailReason.ToString());
				break;
			}
		}

		if (OutError.IsEmpty())
		{
			Object->ProcessEvent(Function, Parms.GetData());
			for (FProperty* Param : ParamProps)
			{
				if (Param->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
				{
					OutOutputs->SetField(Param->GetName(), FJsonObjectConverter::UPropertyToJsonValue(
						Param, Param->ContainerPtrToValuePtr<void>(Parms.GetData()),
						0, 0, nullptr, nullptr, EJsonObjectConversionFlags::SkipStandardizeCase));
				}
			}
		}

		for (FProperty* Param : ParamProps)
		{
			Param->DestroyValue_InContainer(Parms.GetData());
		}
		return OutError.IsEmpty();
	}

	TSharedRef<FJsonObject> FunctionSignatureJson(const UFunction& Function)
	{
		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("function"), Function.GetName());
		if (const UClass* Owner = Function.GetOwnerClass())
		{
			Data->SetStringField(TEXT("class"), Owner->GetName());
		}
		Data->SetBoolField(TEXT("static"), Function.HasAnyFunctionFlags(FUNC_Static));
		TArray<TSharedPtr<FJsonValue>> Params;
		for (TFieldIterator<FProperty> It(&Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), It->GetName());
			Entry->SetStringField(TEXT("type"), It->GetCPPType());
			if (It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				Entry->SetStringField(TEXT("direction"), TEXT("return"));
			}
			else if (It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ConstParm))
			{
				Entry->SetStringField(TEXT("direction"), It->HasAnyPropertyFlags(CPF_ReferenceParm) ? TEXT("in_out") : TEXT("out"));
			}
			else
			{
				Entry->SetStringField(TEXT("direction"), TEXT("in"));
			}
			// Blueprint defaults live in metadata as CPP_Default_<Param>.
			const FString DefaultValue = Function.GetMetaData(*FString::Printf(TEXT("CPP_Default_%s"), *It->GetName()));
			if (!DefaultValue.IsEmpty())
			{
				Entry->SetStringField(TEXT("default"), DefaultValue);
			}
			Params.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Data->SetArrayField(TEXT("params"), Params);
		const FString Tooltip = Function.GetMetaData(TEXT("ToolTip"));
		if (!Tooltip.IsEmpty())
		{
			Data->SetStringField(TEXT("tooltip"), Tooltip.Left(300));
		}
		return Data;
	}
}
