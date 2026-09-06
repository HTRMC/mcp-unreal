#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterRemoteControlRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkRemoteControlModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterRemoteControlRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkRemoteControlModule, McpLinkRemoteControl)
