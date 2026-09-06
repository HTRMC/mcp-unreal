#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UFunction;

namespace McpLink
{
	/// Call a UFunction with arguments taken from a JSON object (matched to
	/// parameter names case-insensitively; missing inputs keep their
	/// defaults), and collect every return and out parameter into OutOutputs.
	/// False with OutError set when an argument could not be converted; the
	/// call itself is ProcessEvent, so it has no failure of its own.
	MCPLINKCORE_API bool CallFunctionFromJson(UObject* Object, UFunction* Function,
		const TSharedPtr<FJsonObject>& Args, const TSharedRef<FJsonObject>& OutOutputs, FString& OutError);

	/// The function's parameters as JSON: name, C++ type, in/out/return and
	/// the default the reflection metadata records.
	MCPLINKCORE_API TSharedRef<FJsonObject> FunctionSignatureJson(const UFunction& Function);
}
