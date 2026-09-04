#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "Modules/ModuleManager.h"

class FMcpLinkEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FMcpLinkCoreModule& Core = FMcpLinkCoreModule::Get();
		McpLink::RegisterObjectRoutes(Core);
		McpLink::RegisterActorRoutes(Core);
		McpLink::RegisterLevelRoutes(Core);
		McpLink::RegisterAssetRoutes(Core);
		McpLink::RegisterAssetOpsRoutes(Core);
		McpLink::RegisterConsoleRoutes(Core);
		McpLink::RegisterEditorOpsRoutes(Core);
		McpLink::RegisterPerfRoutes(Core);
		McpLink::RegisterVisualLogRoutes(Core);
		McpLink::RegisterPieRoutes(Core);
		McpLink::RegisterCaptureRoutes(Core);
		McpLink::RegisterIntrospectRoutes(Core);
	}
};

IMPLEMENT_MODULE(FMcpLinkEditorModule, McpLinkEditor)
