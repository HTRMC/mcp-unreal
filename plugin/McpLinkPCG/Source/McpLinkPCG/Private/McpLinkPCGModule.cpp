#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterPcgRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkPCGModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterPcgRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkPCGModule, McpLinkPCG)
