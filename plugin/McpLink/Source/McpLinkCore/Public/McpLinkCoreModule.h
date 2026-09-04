#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "HttpRouteHandle.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

class IHttpRouter;
class FJsonObject;

namespace McpLink
{
	class FMcpResponder;
	class FMcpLogCapture;

	// A game-thread JSON handler. Complete via the responder — synchronously,
	// or later from a delegate for long-running operations.
	using FMcpHandler = TFunction<void(const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)>;
}

class MCPLINKCORE_API FMcpLinkCoreModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FMcpLinkCoreModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FMcpLinkCoreModule>("McpLinkCore");
	}

	// Register a handler at Path (e.g. "/api/actors/list"). Safe to call from
	// any dependent module's StartupModule; routes registered before the HTTP
	// server starts are queued and bound at startup.
	void RegisterRoute(const FString& Path, McpLink::FMcpHandler Handler, bool bAllowGet = false);

	McpLink::FMcpLogCapture* GetLogCapture() const;
	uint32 GetPort() const { return Port; }
	TConstArrayView<FString> GetRoutePaths() const { return RoutePaths; }

	// True while the game thread is parked in FSlateApplication's intra-frame
	// debugging loop, i.e. execution is halted on a Blueprint breakpoint.
	static bool IsHaltedAtBreakpoint();

	// A route that stays answerable while halted. Everything else is refused
	// with an explanation, because a handler that ran there would be
	// re-entering the engine from inside a paused Blueprint's call stack.
	static bool IsServableWhileHalted(const FString& Path);

private:
	void StartHttpServer();
	void StopHttpServer();
	void BindRoute(const FString& Path, McpLink::FMcpHandler Handler, bool bAllowGet);
	void PumpWhileHalted(float DeltaTime);

	struct FPendingRoute
	{
		FString Path;
		McpLink::FMcpHandler Handler;
		bool bAllowGet = false;
	};

	TSharedPtr<IHttpRouter> Router;
	TArray<FHttpRouteHandle> RouteHandles;
	TArray<FPendingRoute> PendingRoutes;
	TArray<FString> RoutePaths;
	TUniquePtr<McpLink::FMcpLogCapture> LogCapture;
	// The HTTP server as its ticker base. Held as a pointer so the manual tick
	// below is a vtable call: FHttpServerModule::Tick is public but carries no
	// export macro, so it can only be reached virtually.
	FTSTickerObjectBase* HttpTicker = nullptr;
	FDelegateHandle PreTickHandle;
	uint32 Port = 8091;
	bool bServerStarted = false;
	// Set while a route handler is running, so the halted-editor pump below
	// never re-enters the HTTP server from inside one of its own handlers.
	bool bDispatching = false;
};
