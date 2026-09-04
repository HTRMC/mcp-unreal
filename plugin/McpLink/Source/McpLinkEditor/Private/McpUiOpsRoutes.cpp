// Asset editors: opening, closing and listing them.
//
// ui_query reads the editor's widget tree; this acts on the one part of it
// that has a real API. Opening an asset editor is not only about showing a
// window — some engine code only does its work when the editor constructs
// itself, and a fresh Material Layer getting its input and output nodes is
// exactly that.
//
// Clicking, typing and scrolling go through the engine's AutomationDriver,
// which locates Slate widgets by driver id or type path. Two things about it
// matter. Its synchronous API dispatches every step with AsyncTask(GameThread)
// and blocks on the future, so driving it *from* the game thread deadlocks on
// itself — the work runs on a worker thread and the response is deferred.
// And its path parser asserts rather than erroring on a malformed selector
// (a leading '/' reaches Matchers.Last() on an empty array), which takes the
// editor down with an array range check; ValidatePath rejects those first.

#include "AutomationDriverTypeDefs.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "IAutomationDriver.h"
#include "IAutomationDriverModule.h"
#include "IDriverElement.h"
#include "IElementLocator.h"
#include "LocateBy.h"
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

	namespace UiDrive
	{
		/// The engine's own path parser asserts instead of erroring on a
		/// malformed path: an empty leading piece reaches Matchers.Last() on an
		/// empty array, and a trailing slash reaches PathPiece[0] of an empty
		/// string. Both take the editor down with an array range check, which is
		/// what "the AutomationDriver crashes on the first call" turned out to
		/// be — a bad selector, not a headless editor. So the path is checked
		/// here before the driver ever sees it.
		bool ValidatePath(const FString& Path, FString& OutError)
		{
			if (Path.IsEmpty())
			{
				OutError = TEXT("give 'id' (a widget's driver tag) or 'path' (a widget path like ")
						   TEXT("\"#MainWindow/SButton\")");
				return false;
			}
			if (Path.StartsWith(TEXT("/")))
			{
				OutError = FString::Printf(
					TEXT("widget path '%s' starts with '/' — paths are relative and must begin ")
					TEXT("with a matcher, e.g. \"#SomeId/SButton\" or \"SWindow//SButton\""),
					*Path);
				return false;
			}
			if (Path.EndsWith(TEXT("/")))
			{
				OutError = FString::Printf(
					TEXT("widget path '%s' ends with '/' — it must end with a matcher"), *Path);
				return false;
			}
			return true;
		}

		/// "id" (a widget's driver tag) or "path". Ids are the safer handle and
		/// what the engine's own driver tests use.
		bool MakeLocator(const TSharedRef<FJsonObject>& Body,
			TSharedPtr<IElementLocator, ESPMode::ThreadSafe>& OutLocator, FString& OutError)
		{
			FString Id;
			if (Body->TryGetStringField(TEXT("id"), Id) && !Id.IsEmpty())
			{
				OutLocator = By::Id(Id);
				return true;
			}
			FString Path;
			Body->TryGetStringField(TEXT("path"), Path);
			if (!ValidatePath(Path, OutError))
			{
				return false;
			}
			OutLocator = By::Path(Path);
			return true;
		}

		EMouseButtons::Type MouseButtonFromName(const FString& Name)
		{
			if (Name == TEXT("right")) { return EMouseButtons::Right; }
			if (Name == TEXT("middle")) { return EMouseButtons::Middle; }
			return EMouseButtons::Left;
		}

		TSharedRef<FJsonObject> ElementToJson(const TSharedRef<IDriverElement, ESPMode::ThreadSafe>& Element)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("text"), Element->GetText().ToString());
			Item->SetBoolField(TEXT("visible"), Element->IsVisible());
			Item->SetBoolField(TEXT("interactable"), Element->IsInteractable());
			Item->SetBoolField(TEXT("hovered"), Element->IsHovered());
			Item->SetBoolField(TEXT("focused"), Element->IsFocused());
			Item->SetBoolField(TEXT("scrollable"), Element->IsScrollable());
			const FVector2D Position = Element->GetAbsolutePosition();
			const FVector2D Size = Element->GetSize();
			TArray<TSharedPtr<FJsonValue>> Rect;
			Rect.Add(MakeShared<FJsonValueNumber>(Position.X));
			Rect.Add(MakeShared<FJsonValueNumber>(Position.Y));
			Rect.Add(MakeShared<FJsonValueNumber>(Size.X));
			Rect.Add(MakeShared<FJsonValueNumber>(Size.Y));
			Item->SetArrayField(TEXT("rect"), Rect);
			return Item;
		}

		bool IsDriveOperation(const FString& Operation)
		{
			return Operation == TEXT("find_widgets")
				|| Operation == TEXT("click")
				|| Operation == TEXT("double_click")
				|| Operation == TEXT("hover")
				|| Operation == TEXT("focus")
				|| Operation == TEXT("type")
				|| Operation == TEXT("press_key")
				|| Operation == TEXT("scroll");
		}

		/// Runs one driver action on a worker thread and completes the response
		/// back on the game thread.
		///
		/// The synchronous driver dispatches every step with AsyncTask(GameThread)
		/// and blocks on the future, so calling it *from* the game thread would
		/// deadlock on itself. The route therefore hands off: this handler
		/// returns immediately, the worker drives, and the game thread stays free
		/// to run the steps the worker is waiting on. FMcpResponder already
		/// supports being completed later.
		void RunDriveOperation(const FString& Operation, const TSharedRef<FJsonObject>& Body,
			TSharedRef<FMcpResponder> Responder)
		{
			if (!FSlateApplication::IsInitialized())
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_slate"),
					TEXT("Slate is not initialised, so there is no widget tree to drive"));
				return;
			}

			TSharedPtr<IElementLocator, ESPMode::ThreadSafe> Locator;
			FString Error;
			if (!MakeLocator(Body, Locator, Error))
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_locator"), Error);
				return;
			}

			IAutomationDriverModule& Module = IAutomationDriverModule::Get();
			if (Module.IsEnabled())
			{
				Module.Disable();
			}
			Module.Enable();
			const TSharedRef<IAutomationDriver, ESPMode::ThreadSafe> Driver = Module.CreateDriver();

			const TSharedRef<IElementLocator, ESPMode::ThreadSafe> LocatorRef = Locator.ToSharedRef();
			FString Text;
			Body->TryGetStringField(TEXT("text"), Text);
			FString KeyName;
			Body->TryGetStringField(TEXT("key"), KeyName);
			FString ButtonName;
			Body->TryGetStringField(TEXT("button"), ButtonName);
			ButtonName = ButtonName.ToLower();
			const double ScrollDelta = DoubleOr(Body, TEXT("delta"), 1.0);
			// Which of several matches to act on, in find_widgets order.
			const int32 Index = FMath::Max(0, IntOr(Body, TEXT("index"), 0));

			AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask,
				[Operation, Driver, LocatorRef, Text, KeyName, ButtonName, ScrollDelta, Index, Responder]()
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					FString Failure;
					bool bOk = true;

					if (Operation == TEXT("find_widgets"))
					{
						const TArray<TSharedRef<IDriverElement, ESPMode::ThreadSafe>> Elements =
							Driver->FindElements(LocatorRef)->GetElements();
						TArray<TSharedPtr<FJsonValue>> Items;
						for (const TSharedRef<IDriverElement, ESPMode::ThreadSafe>& Element : Elements)
						{
							Items.Add(MakeShared<FJsonValueObject>(ElementToJson(Element)));
						}
						Data->SetNumberField(TEXT("count"), Items.Num());
						Data->SetArrayField(TEXT("widgets"), Items);
					}
					else
					{
						// FindElement resolves only when the locator matches
						// exactly one widget, and reports a match of nine the
						// same way as a match of none. Collecting first lets
						// "which of the nine" be an answerable question: pair
						// find_widgets with 'index'.
						const TArray<TSharedRef<IDriverElement, ESPMode::ThreadSafe>> Matches =
							Driver->FindElements(LocatorRef)->GetElements();
						Data->SetNumberField(TEXT("matched"), Matches.Num());
						if (Matches.IsEmpty())
						{
							bOk = false;
							Failure = TEXT("no widget matched — find_widgets lists what a locator ")
									  TEXT("resolves to, and ui_query dumps the tree");
						}
						else if (!Matches.IsValidIndex(Index))
						{
							bOk = false;
							Failure = FString::Printf(
								TEXT("this locator matched %d widgets, so 'index' must be 0..%d — ")
								TEXT("find_widgets reports them in the same order"),
								Matches.Num(), Matches.Num() - 1);
						}
						if (!bOk)
						{
							// fall through to the shared failure reply
						}
						else if (const TSharedRef<IDriverElement, ESPMode::ThreadSafe>& Element = Matches[Index];
							Operation == TEXT("click"))
						{
							bOk = ButtonName.IsEmpty()
								? Element->Click()
								: Element->Click(MouseButtonFromName(ButtonName));
						}
						else if (Operation == TEXT("double_click"))
						{
							bOk = ButtonName.IsEmpty()
								? Element->DoubleClick()
								: Element->DoubleClick(MouseButtonFromName(ButtonName));
						}
						else if (Operation == TEXT("hover"))
						{
							bOk = Element->Hover();
						}
						else if (Operation == TEXT("focus"))
						{
							bOk = Element->Focus();
						}
						else if (Operation == TEXT("scroll"))
						{
							bOk = Element->ScrollBy(static_cast<float>(ScrollDelta));
						}
						else if (Operation == TEXT("type"))
						{
							bOk = Element->Type(Text);
						}
						else
						{
							const FKey Key(*KeyName);
							if (!Key.IsValid())
							{
								bOk = false;
								Failure = FString::Printf(
									TEXT("'%s' is not a key name — use engine names like Enter, ")
									TEXT("Escape, Tab, A, LeftControl"), *KeyName);
							}
							else
							{
								bOk = Element->Type(Key);
							}
						}
						if (bOk && Matches.IsValidIndex(Index))
						{
							Data->SetObjectField(TEXT("widget"), ElementToJson(Matches[Index]));
						}
					}

					// Back to the game thread: the driver has to be switched off
					// there (it owns the application's message handler), and the
					// HTTP response has to complete there too.
					AsyncTask(ENamedThreads::GameThread,
						[Operation, Data, Responder, bOk, Failure]()
						{
							IAutomationDriverModule& Mod = IAutomationDriverModule::Get();
							if (Mod.IsEnabled())
							{
								Mod.Disable();
							}
							if (!bOk)
							{
								Responder->Error(EHttpServerResponseCodes::Conflict,
									TEXT("drive_failed"),
									Failure.IsEmpty()
										? FString::Printf(
											TEXT("the widget refused '%s' — it may be disabled, ")
											TEXT("hidden or not interactable"), *Operation)
										: Failure);
								return;
							}
							Data->SetStringField(TEXT("operation"), Operation);
							Responder->Ok(Data);
						});
				});
		}
	}

	using namespace UiOps;
	using namespace UiDrive;

	void RegisterUiOpsRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/editor/ui_ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (IsDriveOperation(Operation))
				{
					RunDriveOperation(Operation, Body, Responder);
					return;
				}

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
							TEXT("unknown operation '%s' — use open_asset, close_asset, close_all, ")
							TEXT("list_open, find_widgets, click, double_click, hover, focus, type, ")
							TEXT("press_key or scroll"),
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
