#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterLevelSnapshotRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkLevelSnapshotsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterLevelSnapshotRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkLevelSnapshotsModule, McpLinkLevelSnapshots)
