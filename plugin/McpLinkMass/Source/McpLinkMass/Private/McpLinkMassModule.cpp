#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterMassRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkMassModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterMassRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkMassModule, McpLinkMass)
