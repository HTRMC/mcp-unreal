#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterMaterialRoutes(FMcpLinkCoreModule& Core);
	void RegisterMaterialGraphRoutes(FMcpLinkCoreModule& Core);
	void RegisterDataRoutes(FMcpLinkCoreModule& Core);
	void RegisterInputAssetRoutes(FMcpLinkCoreModule& Core);
	void RegisterIsmRoutes(FMcpLinkCoreModule& Core);
	void RegisterSequenceRoutes(FMcpLinkCoreModule& Core);
	void RegisterAudioRoutes(FMcpLinkCoreModule& Core);
	void RegisterStaticMeshRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkContentModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FMcpLinkCoreModule& Core = FMcpLinkCoreModule::Get();
		McpLink::RegisterMaterialRoutes(Core);
		McpLink::RegisterMaterialGraphRoutes(Core);
		McpLink::RegisterDataRoutes(Core);
		McpLink::RegisterInputAssetRoutes(Core);
		McpLink::RegisterIsmRoutes(Core);
		McpLink::RegisterSequenceRoutes(Core);
		McpLink::RegisterAudioRoutes(Core);
		McpLink::RegisterStaticMeshRoutes(Core);
	}
};

IMPLEMENT_MODULE(FMcpLinkContentModule, McpLinkContent)
