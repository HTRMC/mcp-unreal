#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterCameraRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkCamerasModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		McpLink::RegisterCameraRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkCamerasModule, McpLinkCameras)
