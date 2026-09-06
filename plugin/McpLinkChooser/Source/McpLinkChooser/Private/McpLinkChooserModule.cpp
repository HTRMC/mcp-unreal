#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterChooserRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkChooserModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterChooserRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkChooserModule, McpLinkChooser)
