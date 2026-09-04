#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterChaosRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkChaosModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterChaosRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkChaosModule, McpLinkChaos)
