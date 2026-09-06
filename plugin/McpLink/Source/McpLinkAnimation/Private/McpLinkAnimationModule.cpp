#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterAnimAssetRoutes(FMcpLinkCoreModule& Core);
	void RegisterAnimNotifyRoutes(FMcpLinkCoreModule& Core);
	void RegisterSkeletonRoutes(FMcpLinkCoreModule& Core);
	void RegisterPhysicsAssetRoutes(FMcpLinkCoreModule& Core);
	void RegisterPoseAssetRoutes(FMcpLinkCoreModule& Core);
	void RegisterMirrorTableRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkAnimationModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FMcpLinkCoreModule& Core = FMcpLinkCoreModule::Get();
		McpLink::RegisterAnimAssetRoutes(Core);
		McpLink::RegisterAnimNotifyRoutes(Core);
		McpLink::RegisterSkeletonRoutes(Core);
		McpLink::RegisterPhysicsAssetRoutes(Core);
		McpLink::RegisterPoseAssetRoutes(Core);
		McpLink::RegisterMirrorTableRoutes(Core);
	}
};

IMPLEMENT_MODULE(FMcpLinkAnimationModule, McpLinkAnimation)
