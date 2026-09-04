#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterGasRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkGASModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterGasRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkGASModule, McpLinkGAS)
