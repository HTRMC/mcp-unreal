#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterSourceControlRoutes(FMcpLinkCoreModule& Core);
	void RegisterValidationRoutes(FMcpLinkCoreModule& Core);
	void RegisterTagAndCurveRoutes(FMcpLinkCoreModule& Core);
	void RegisterReferenceRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkWorkflowModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FMcpLinkCoreModule& Core = FMcpLinkCoreModule::Get();
		McpLink::RegisterSourceControlRoutes(Core);
		McpLink::RegisterValidationRoutes(Core);
		McpLink::RegisterTagAndCurveRoutes(Core);
		McpLink::RegisterReferenceRoutes(Core);
	}
};

IMPLEMENT_MODULE(FMcpLinkWorkflowModule, McpLinkWorkflow)
