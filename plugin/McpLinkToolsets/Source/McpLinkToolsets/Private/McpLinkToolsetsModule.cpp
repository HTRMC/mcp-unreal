#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterToolsetRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkToolsetsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// McpLinkCore queues routes registered after the server is up, so
		// plugin load order does not matter.
		McpLink::RegisterToolsetRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkToolsetsModule, McpLinkToolsets)
