#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterMetaSoundRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkMetaSoundModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// McpLinkCore queues routes registered after the server is up, so
		// plugin load order does not matter.
		McpLink::RegisterMetaSoundRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkMetaSoundModule, McpLinkMetaSound)
