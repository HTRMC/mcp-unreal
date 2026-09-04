#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterPythonRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkPythonModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterPythonRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkPythonModule, McpLinkPython)
