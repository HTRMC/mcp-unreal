#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterBlackboardRoutes(FMcpLinkCoreModule& Core);
	void RegisterBehaviorTreeRoutes(FMcpLinkCoreModule& Core);
	void RegisterNavigationRoutes(FMcpLinkCoreModule& Core);
	void RegisterStateTreeRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkAIModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FMcpLinkCoreModule& Core = FMcpLinkCoreModule::Get();
		McpLink::RegisterBlackboardRoutes(Core);
		McpLink::RegisterBehaviorTreeRoutes(Core);
		McpLink::RegisterNavigationRoutes(Core);
		McpLink::RegisterStateTreeRoutes(Core);
	}
};

IMPLEMENT_MODULE(FMcpLinkAIModule, McpLinkAI)
