#include "McpResponder.h"

#include "Dom/JsonObject.h"
#include "HttpServerResponse.h"
#include "McpJson.h"

namespace McpLink
{
	FMcpResponder::FMcpResponder(FHttpResultCallback InCallback)
		: Callback(MoveTemp(InCallback))
	{
	}

	FMcpResponder::~FMcpResponder()
	{
		if (!bCompleted)
		{
			Error(
				EHttpServerResponseCodes::ServerError,
				TEXT("handler_dropped"),
				TEXT("the route handler was destroyed without sending a response"));
		}
	}

	void FMcpResponder::Ok(const TSharedRef<FJsonObject>& Data)
	{
		const TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetBoolField(TEXT("ok"), true);
		Envelope->SetObjectField(TEXT("data"), Data);
		Send(EHttpServerResponseCodes::Ok, Envelope);
	}

	void FMcpResponder::Error(EHttpServerResponseCodes StatusCode, const FString& ErrorCode, const FString& Message)
	{
		const TSharedRef<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
		ErrorObj->SetStringField(TEXT("code"), ErrorCode);
		ErrorObj->SetStringField(TEXT("message"), Message);
		const TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetBoolField(TEXT("ok"), false);
		Envelope->SetObjectField(TEXT("error"), ErrorObj);
		Send(StatusCode, Envelope);
	}

	void FMcpResponder::Send(EHttpServerResponseCodes StatusCode, const TSharedRef<FJsonObject>& Envelope)
	{
		if (bCompleted)
		{
			UE_LOG(LogTemp, Warning, TEXT("McpLink: attempted double response — ignored"));
			return;
		}
		bCompleted = true;
		TUniquePtr<FHttpServerResponse> Response =
			FHttpServerResponse::Create(JsonToString(Envelope), TEXT("application/json"));
		Response->Code = StatusCode;
		Callback(MoveTemp(Response));
	}
}
