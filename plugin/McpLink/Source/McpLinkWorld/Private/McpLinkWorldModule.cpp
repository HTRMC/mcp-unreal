#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterLandscapeRoutes(FMcpLinkCoreModule& Core);
	void RegisterFoliageRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkWorldModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FMcpLinkCoreModule& Core = FMcpLinkCoreModule::Get();
		McpLink::RegisterLandscapeRoutes(Core);
		McpLink::RegisterFoliageRoutes(Core);
	}
};

IMPLEMENT_MODULE(FMcpLinkWorldModule, McpLinkWorld)
