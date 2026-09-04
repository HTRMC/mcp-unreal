#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterBlueprintRoutes(FMcpLinkCoreModule& Core);
	void RegisterAnimBlueprintRoutes(FMcpLinkCoreModule& Core);
	void RegisterWidgetBlueprintRoutes(FMcpLinkCoreModule& Core);
	void RegisterUserTypeRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkBlueprintModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FMcpLinkCoreModule& Core = FMcpLinkCoreModule::Get();
		McpLink::RegisterBlueprintRoutes(Core);
		McpLink::RegisterAnimBlueprintRoutes(Core);
		McpLink::RegisterWidgetBlueprintRoutes(Core);
		McpLink::RegisterUserTypeRoutes(Core);
	}
};

IMPLEMENT_MODULE(FMcpLinkBlueprintModule, McpLinkBlueprint)
