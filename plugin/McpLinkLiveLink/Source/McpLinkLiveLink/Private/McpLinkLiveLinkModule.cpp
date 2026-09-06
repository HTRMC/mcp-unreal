#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterLiveLinkRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkLiveLinkModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterLiveLinkRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkLiveLinkModule, McpLinkLiveLink)
