#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterPoseSearchRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkPoseSearchModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterPoseSearchRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkPoseSearchModule, McpLinkPoseSearch)
