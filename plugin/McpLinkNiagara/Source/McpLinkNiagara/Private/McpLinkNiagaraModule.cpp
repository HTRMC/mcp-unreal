#include "McpLinkCoreModule.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	void RegisterNiagaraRoutes(FMcpLinkCoreModule& Core);
	void RegisterNiagaraAuthoringRoutes(FMcpLinkCoreModule& Core);
}

class FMcpLinkNiagaraModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// McpLinkCore queues routes registered after the server is up, so
		// plugin load order does not matter.
		McpLink::RegisterNiagaraRoutes(FMcpLinkCoreModule::Get());
		McpLink::RegisterNiagaraAuthoringRoutes(FMcpLinkCoreModule::Get());
	}
};

IMPLEMENT_MODULE(FMcpLinkNiagaraModule, McpLinkNiagara)
