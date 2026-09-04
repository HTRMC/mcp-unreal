// JSON helpers shared by every McpLink route module.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HttpServerRequest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace McpLink
{
	// UTF-8 request body -> JSON object. Empty body yields an empty object;
	// malformed JSON yields nullptr (the route wrapper turns that into a 400).
	// The body buffer is copied and null-terminated before conversion.
	inline TSharedPtr<FJsonObject> ParseBody(const FHttpServerRequest& Request)
	{
		if (Request.Body.Num() == 0)
		{
			return MakeShared<FJsonObject>();
		}
		TArray<uint8> Bytes = Request.Body;
		Bytes.Add(0);
		const FString Text = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData()));
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
		{
			return Parsed;
		}
		return nullptr;
	}

	// Optional numeric and boolean body fields. JSON has a single number type,
	// so an integer field arrives as a double.
	inline int32 IntOr(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, int32 Default)
	{
		double Value = 0.0;
		return Body->TryGetNumberField(Field, Value) ? static_cast<int32>(Value) : Default;
	}

	inline double DoubleOr(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, double Default)
	{
		double Value = 0.0;
		return Body->TryGetNumberField(Field, Value) ? Value : Default;
	}

	inline bool BoolOr(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, bool Default)
	{
		bool Value = false;
		return Body->TryGetBoolField(Field, Value) ? Value : Default;
	}

	inline FString JsonToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}
}
