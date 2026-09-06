#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterGeometryScriptRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkGeometryScriptModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterGeometryScriptRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkGeometryScriptModule, McpLinkGeometryScript)
