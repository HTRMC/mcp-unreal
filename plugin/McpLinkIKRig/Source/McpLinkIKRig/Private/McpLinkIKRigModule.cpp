#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterIkRigRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkIKRigModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterIkRigRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkIKRigModule, McpLinkIKRig)
