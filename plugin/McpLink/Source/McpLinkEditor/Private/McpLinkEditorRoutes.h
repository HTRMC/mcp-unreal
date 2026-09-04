#pragma once

class FMcpLinkCoreModule;

namespace McpLink
{
	void RegisterObjectRoutes(FMcpLinkCoreModule& Core);
	void RegisterActorRoutes(FMcpLinkCoreModule& Core);
	void RegisterLevelRoutes(FMcpLinkCoreModule& Core);
	void RegisterAssetRoutes(FMcpLinkCoreModule& Core);
	void RegisterAssetOpsRoutes(FMcpLinkCoreModule& Core);
	void RegisterConsoleRoutes(FMcpLinkCoreModule& Core);
	void RegisterEditorOpsRoutes(FMcpLinkCoreModule& Core);
	void RegisterPerfRoutes(FMcpLinkCoreModule& Core);
	void RegisterVisualLogRoutes(FMcpLinkCoreModule& Core);
	void RegisterPieRoutes(FMcpLinkCoreModule& Core);
	void RegisterCaptureRoutes(FMcpLinkCoreModule& Core);
	void RegisterIntrospectRoutes(FMcpLinkCoreModule& Core);
}
