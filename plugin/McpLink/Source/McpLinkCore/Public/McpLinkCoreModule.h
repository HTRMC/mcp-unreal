#pragma once

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

private:
	void StartHttpServer();
	void StopHttpServer();
	void BindRoute(const FString& Path, McpLink::FMcpHandler Handler, bool bAllowGet);

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
	uint32 Port = 8091;
	bool bServerStarted = false;
};
