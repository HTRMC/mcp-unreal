#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterControlRigRoutes(FMcpLinkCoreModule& Core);
	void RegisterRigVmGraphRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkControlRigModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterControlRigRoutes(FMcpLinkCoreModule::Get());
		McpLink::RegisterRigVmGraphRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkControlRigModule, McpLinkControlRig)
