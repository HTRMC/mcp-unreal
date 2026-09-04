// Exactly-once HTTP response completion with real status codes and the
// McpLink JSON envelope: {"ok":true,"data":...} / {"ok":false,"error":{...}}.
#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"
#include "HttpServerConstants.h"

class FJsonObject;

namespace McpLink
{
	// Handlers may complete synchronously or stash the shared responder and
	// complete later from a delegate (deferred response). If a responder is
	// dropped without completing, its destructor sends a 500 so the client
	// never hangs.
	class MCPLINKCORE_API FMcpResponder : public TSharedFromThis<FMcpResponder>
	{
	public:
		explicit FMcpResponder(FHttpResultCallback InCallback);
		~FMcpResponder();

		FMcpResponder(const FMcpResponder&) = delete;
		FMcpResponder& operator=(const FMcpResponder&) = delete;

		void Ok(const TSharedRef<FJsonObject>& Data);
		void Error(EHttpServerResponseCodes StatusCode, const FString& ErrorCode, const FString& Message);
		bool IsCompleted() const { return bCompleted; }

	private:
		void Send(EHttpServerResponseCodes StatusCode, const TSharedRef<FJsonObject>& Envelope);

		FHttpResultCallback Callback;
		bool bCompleted = false;
	};
}
