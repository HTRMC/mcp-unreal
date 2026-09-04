// Asset editors: opening, closing and listing them.
//
// ui_query reads the editor's widget tree; this acts on the one part of it
// that has a real API. Opening an asset editor is not only about showing a
// window — some engine code only does its work when the editor constructs
// itself, and a fresh Material Layer getting its input and output nodes is
// exactly that.
//
// Clicking widgets is deliberately not here. The engine's AutomationDriver is
// the right mechanism on paper — it locates Slate widgets by driver id, tag or
// type path and clicks, types and scrolls them — and its threading works out:
// it dispatches to the game thread and the synchronous API blocks, so it has
// to be driven from a worker thread with the response deferred, which
// FMcpResponder already supports. What does not work is the environment. In a
// headless editor the very first call, Exists(), asserts inside the driver
// ("Assertion failed: (Index >= 0) & (Index < ArrayNum)", Array.h:1339) and
// takes the editor down with it, because the driver resolves elements through
// platform windows that -nullrhi never creates. See incomplete.txt.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace McpLink
{
	namespace UiOps
	{
		UAssetEditorSubsystem* AssetEditors()
		{
			return GEditor != nullptr ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		}
	}

	using namespace UiOps;

	void RegisterUiOpsRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/editor/ui_ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				UAssetEditorSubsystem* Subsystem = AssetEditors();
				if (Subsystem == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_editor"),
						TEXT("the asset editor subsystem is unavailable"));
					return;
				}

				if (Operation == TEXT("list_open"))
				{
					TArray<TSharedPtr<FJsonValue>> Items;
					for (UObject* Asset : Subsystem->GetAllEditedAssets())
					{
						if (Asset == nullptr)
						{
							continue;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("asset"), Asset->GetPathName());
						Item->SetStringField(TEXT("class"), Asset->GetClass()->GetName());
						Items.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("count"), Items.Num());
					Data->SetArrayField(TEXT("open"), Items);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("close_all"))
				{
					Subsystem->CloseAllAssetEditors();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("message"), TEXT("every asset editor closed"));
					Responder->Ok(Data);
					return;
				}

				if (Operation != TEXT("open_asset") && Operation != TEXT("close_asset"))
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
						FString::Printf(
							TEXT("unknown operation '%s' — use open_asset, close_asset, close_all ")
							TEXT("or list_open. Clicking widgets is not available; ui_query reads ")
							TEXT("the widget tree, and the typed tools do the work buttons would"),
							*Operation));
					return;
				}

				FString Path;
				if (!RequireString(Body, TEXT("asset"), Path, Responder,
						TEXT("an asset path, e.g. /Game/Materials/M_Thing")))
				{
					return;
				}
				UObject* Asset = ResolveAsset(Path);
				if (Asset == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
						FString::Printf(TEXT("no asset at '%s'"), *Path));
					return;
				}

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("asset"), Asset->GetPathName());
				if (Operation == TEXT("close_asset"))
				{
					Subsystem->CloseAllEditorsForAsset(Asset);
					Data->SetStringField(TEXT("message"), TEXT("closed"));
				}
				else
				{
					const bool bOpened = Subsystem->OpenEditorForAsset(Asset);
					Data->SetBoolField(TEXT("opened"), bOpened);
					Data->SetStringField(TEXT("message"), bOpened
							? TEXT("editor opened — its transient preview objects show up in ")
							  TEXT("list_open alongside the asset")
							: TEXT("no editor opened — this asset type may have none"));
				}
				Responder->Ok(Data);
			});
	}
}
