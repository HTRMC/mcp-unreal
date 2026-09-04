#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterMovieRenderRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkMovieRenderModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// McpLinkCore queues routes registered after the server is up, so
		// plugin load order does not matter.
		McpLink::RegisterMovieRenderRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkMovieRenderModule, McpLinkMovieRender)
