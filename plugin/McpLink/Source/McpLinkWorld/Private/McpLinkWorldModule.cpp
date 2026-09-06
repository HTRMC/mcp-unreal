#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterLandscapeRoutes(FMcpLinkCoreModule& Core);
	void RegisterFoliageRoutes(FMcpLinkCoreModule& Core);
	void RegisterStreamingRoutes(FMcpLinkCoreModule& Core);
	void RegisterPartitionRoutes(FMcpLinkCoreModule& Core);
	void RegisterLevelInstanceRoutes(FMcpLinkCoreModule& Core);
	void RegisterRvtRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkWorldModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FMcpLinkCoreModule& Core = FMcpLinkCoreModule::Get();
		McpLink::RegisterLandscapeRoutes(Core);
		McpLink::RegisterFoliageRoutes(Core);
		McpLink::RegisterStreamingRoutes(Core);
		McpLink::RegisterPartitionRoutes(Core);
		McpLink::RegisterLevelInstanceRoutes(Core);
		McpLink::RegisterRvtRoutes(Core);
	}
};

IMPLEMENT_MODULE(FMcpLinkWorldModule, McpLinkWorld)
