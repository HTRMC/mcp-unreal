#include "Dom/JsonObject.h"
#include "Editor.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/StringOutputDevice.h"

namespace McpLink
{
	void RegisterConsoleRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/editor/console_command"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Command;
				if (!Body->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
						TEXT("'command' is required (e.g. \"stat fps\", \"obj list class=StaticMeshActor\")"));
					return;
				}
				UWorld* World = ResolveWorld(Body);

				FStringOutputDevice Output;
				const bool bHandled = GEditor != nullptr && GEditor->Exec(World, *Command, Output);

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetBoolField(TEXT("handled"), bHandled);
				Data->SetStringField(TEXT("output"), Output);
				Responder->Ok(Data);
			});
	}
}
