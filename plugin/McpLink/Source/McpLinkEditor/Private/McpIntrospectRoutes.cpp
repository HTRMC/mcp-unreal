// Read-only introspection: active subsystems, and the Slate / UMG widget trees.
// Lets an agent "read the screen" — find the HUD text, count buttons, check a
// menu is open — without a screenshot.

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/Children.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/SWidget.h"
#include "Widgets/SWindow.h"

namespace McpLink
{
	namespace
	{
		template <typename SubsystemType>
		void AppendSubsystems(
			const TCHAR* Kind, const TArray<SubsystemType*>& Subsystems, TArray<TSharedPtr<FJsonValue>>& Out)
		{
			for (const USubsystem* Subsystem : Subsystems)
			{
				if (Subsystem == nullptr)
				{
					continue;
				}
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("kind"), Kind);
				Item->SetStringField(TEXT("class"), Subsystem->GetClass()->GetName());
				Item->SetStringField(TEXT("path"), Subsystem->GetPathName());
				Item->SetStringField(TEXT("module"),
					Subsystem->GetClass()->GetOutermost()->GetName());
				Out.Add(MakeShared<FJsonValueObject>(Item));
			}
		}

		int32 GetInt(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, int32 Default, int32 Min, int32 Max)
		{
			double Value = Default;
			Body->TryGetNumberField(Field, Value);
			return FMath::Clamp(static_cast<int32>(Value), Min, Max);
		}

		TSharedRef<FJsonObject> SlateWidgetToJson(const SWidget& Widget, int32 Depth)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("type"), Widget.GetTypeAsString());
			Item->SetNumberField(TEXT("depth"), Depth);
			Item->SetStringField(TEXT("visibility"), Widget.GetVisibility().ToString());
			Item->SetBoolField(TEXT("enabled"), Widget.IsEnabled());
			const FVector2D Size = Widget.GetCachedGeometry().GetLocalSize();
			Item->SetArrayField(TEXT("size"), {
				MakeShared<FJsonValueNumber>(Size.X), MakeShared<FJsonValueNumber>(Size.Y)});
			// Where the widget was constructed (file:line) — invaluable for
			// tracing UMG-generated Slate back to its source.
			const FString Location = Widget.GetReadableLocation();
			if (!Location.IsEmpty())
			{
				Item->SetStringField(TEXT("created_at"), Location);
			}
			return Item;
		}

		// Depth-first walk with a hard node cap so a busy editor can't blow up
		// the response.
		void WalkSlate(
			const TSharedRef<SWidget>& Widget,
			int32 Depth,
			int32 MaxDepth,
			int32 MaxNodes,
			const FString& TypeFilter,
			TArray<TSharedPtr<FJsonValue>>& Out,
			int32& Visited)
		{
			if (Visited >= MaxNodes)
			{
				return;
			}
			++Visited;
			if (TypeFilter.IsEmpty() || Widget->GetTypeAsString().Contains(TypeFilter))
			{
				Out.Add(MakeShared<FJsonValueObject>(SlateWidgetToJson(*Widget, Depth)));
			}
			if (Depth >= MaxDepth)
			{
				return;
			}
			FChildren* Children = Widget->GetChildren();
			if (Children == nullptr)
			{
				return;
			}
			for (int32 i = 0; i < Children->Num(); ++i)
			{
				WalkSlate(Children->GetChildAt(i), Depth + 1, MaxDepth, MaxNodes, TypeFilter, Out, Visited);
			}
		}

		TSharedPtr<SWindow> FindWindow(const TSharedRef<FJsonObject>& Body)
		{
			TArray<TSharedRef<SWindow>> Windows;
			FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
			if (Windows.IsEmpty())
			{
				return nullptr;
			}
			double Index = -1.0;
			if (Body->TryGetNumberField(TEXT("window"), Index))
			{
				const int32 I = static_cast<int32>(Index);
				return Windows.IsValidIndex(I) ? Windows[I].ToSharedPtr() : nullptr;
			}
			FString Title;
			if (Body->TryGetStringField(TEXT("window"), Title) && !Title.IsEmpty())
			{
				for (const TSharedRef<SWindow>& Window : Windows)
				{
					if (Window->GetTitle().ToString().Contains(Title))
					{
						return Window;
					}
				}
				return nullptr;
			}
			// Default: the game viewport window during PIE, else the first (main) window.
			for (const TSharedRef<SWindow>& Window : Windows)
			{
				if (Window->GetTitle().ToString().Contains(TEXT("Preview")))
				{
					return Window;
				}
			}
			return Windows[0];
		}
	}

	void RegisterIntrospectRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/subsystems/query"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Kind = TEXT("all");
				Body->TryGetStringField(TEXT("kind"), Kind);
				Kind = Kind.ToLower();
				const bool bAll = Kind == TEXT("all");

				TArray<TSharedPtr<FJsonValue>> Out;
				if ((bAll || Kind == TEXT("engine")) && GEngine != nullptr)
				{
					AppendSubsystems(TEXT("engine"),
						GEngine->GetEngineSubsystemArrayCopy<UEngineSubsystem>(), Out);
				}
				if ((bAll || Kind == TEXT("editor")) && GEditor != nullptr)
				{
					AppendSubsystems(TEXT("editor"),
						GEditor->GetEditorSubsystemArrayCopy<UEditorSubsystem>(), Out);
				}
				if (bAll || Kind == TEXT("world") || Kind == TEXT("game_instance") || Kind == TEXT("local_player"))
				{
					UWorld* World = ResolveWorld(Body);
					if (World == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("pie_not_running"),
							TEXT("world 'pie' requested but no PIE session is running"));
						return;
					}
					if (bAll || Kind == TEXT("world"))
					{
						AppendSubsystems(TEXT("world"),
							World->GetSubsystemArrayCopy<UWorldSubsystem>(), Out);
					}
					UGameInstance* GameInstance = World->GetGameInstance();
					if (GameInstance != nullptr && (bAll || Kind == TEXT("game_instance")))
					{
						AppendSubsystems(TEXT("game_instance"),
							GameInstance->GetSubsystemArrayCopy<UGameInstanceSubsystem>(), Out);
					}
					if (GameInstance != nullptr && (bAll || Kind == TEXT("local_player")))
					{
						for (const ULocalPlayer* LocalPlayer : GameInstance->GetLocalPlayers())
						{
							if (LocalPlayer != nullptr)
							{
								AppendSubsystems(TEXT("local_player"),
									LocalPlayer->GetSubsystemArrayCopy<ULocalPlayerSubsystem>(), Out);
							}
						}
					}
				}

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetNumberField(TEXT("total"), Out.Num());
				Data->SetArrayField(TEXT("subsystems"), Out);
				Responder->Ok(Data);
			});

		Core.RegisterRoute(TEXT("/api/ui/query"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				// UMG lives on UObjects and needs no Slate application.
				if (Operation == TEXT("umg"))
				{
					UWorld* World = ResolveWorld(Body);
					if (World == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("pie_not_running"),
							TEXT("world 'pie' requested but no PIE session is running"));
						return;
					}
					const int32 MaxWidgets = GetInt(Body, TEXT("max_widgets"), 200, 1, 2000);

					TArray<TSharedPtr<FJsonValue>> UserWidgets;
					int32 Emitted = 0;
					for (TObjectIterator<UUserWidget> It; It; ++It)
					{
						UUserWidget* UserWidget = *It;
						if (UserWidget == nullptr || UserWidget->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
							|| UserWidget->GetWorld() != World)
						{
							continue;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("class"), UserWidget->GetClass()->GetName());
						Item->SetStringField(TEXT("path"), UserWidget->GetPathName());
						Item->SetBoolField(TEXT("in_viewport"), UserWidget->IsInViewport());
						Item->SetStringField(TEXT("visibility"),
							StaticEnum<ESlateVisibility>()->GetNameStringByValue(
								static_cast<int64>(UserWidget->GetVisibility())));

						TArray<TSharedPtr<FJsonValue>> Children;
						if (UserWidget->WidgetTree != nullptr)
						{
							UserWidget->WidgetTree->ForEachWidget(
								[&Children, &Emitted, MaxWidgets](UWidget* Widget)
								{
									if (Widget == nullptr || Emitted >= MaxWidgets)
									{
										return;
									}
									++Emitted;
									const TSharedRef<FJsonObject> W = MakeShared<FJsonObject>();
									W->SetStringField(TEXT("name"), Widget->GetName());
									W->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
									W->SetStringField(TEXT("visibility"),
										StaticEnum<ESlateVisibility>()->GetNameStringByValue(
											static_cast<int64>(Widget->GetVisibility())));
									W->SetBoolField(TEXT("is_visible"), Widget->IsVisible());
									if (const UTextBlock* Text = Cast<UTextBlock>(Widget))
									{
										W->SetStringField(TEXT("text"), Text->GetText().ToString());
									}
									Children.Add(MakeShared<FJsonValueObject>(W));
								});
						}
						Item->SetArrayField(TEXT("widgets"), Children);
						UserWidgets.Add(MakeShared<FJsonValueObject>(Item));
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("world"), World->GetMapName());
					Data->SetNumberField(TEXT("user_widget_count"), UserWidgets.Num());
					Data->SetNumberField(TEXT("widget_count"), Emitted);
					Data->SetArrayField(TEXT("user_widgets"), UserWidgets);
					Responder->Ok(Data);
					return;
				}

				if (!FSlateApplication::IsInitialized())
				{
					Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_slate"),
						TEXT("Slate is not running — the editor is headless (-nullrhi). Only 'umg' works here."));
					return;
				}

				if (Operation == TEXT("windows"))
				{
					TArray<TSharedRef<SWindow>> Windows;
					FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
					TArray<TSharedPtr<FJsonValue>> Out;
					for (int32 i = 0; i < Windows.Num(); ++i)
					{
						const TSharedRef<SWindow>& Window = Windows[i];
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetNumberField(TEXT("index"), i);
						Item->SetStringField(TEXT("title"), Window->GetTitle().ToString());
						Item->SetStringField(TEXT("type"), Window->GetTypeAsString());
						const FVector2D Size = Window->GetSizeInScreen();
						Item->SetArrayField(TEXT("size"), {
							MakeShared<FJsonValueNumber>(Size.X), MakeShared<FJsonValueNumber>(Size.Y)});
						Item->SetBoolField(TEXT("is_active"), Window->IsActive());
						Out.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("windows"), Out);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("tree") || Operation == TEXT("find"))
				{
					const TSharedPtr<SWindow> Window = FindWindow(Body);
					if (!Window.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("window_not_found"),
							TEXT("no matching Slate window — use operation 'windows' to list them"));
						return;
					}
					FString TypeFilter;
					if (Operation == TEXT("find"))
					{
						Body->TryGetStringField(TEXT("type"), TypeFilter);
						if (TypeFilter.IsEmpty())
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'type' is required for find, e.g. SButton, STextBlock, SViewport"));
							return;
						}
					}
					const int32 MaxDepth = GetInt(Body, TEXT("max_depth"), Operation == TEXT("find") ? 40 : 8, 1, 100);
					const int32 MaxNodes = GetInt(Body, TEXT("max_nodes"), 300, 1, 5000);

					TArray<TSharedPtr<FJsonValue>> Out;
					int32 Visited = 0;
					WalkSlate(Window.ToSharedRef(), 0, MaxDepth, MaxNodes, TypeFilter, Out, Visited);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("window"), Window->GetTitle().ToString());
					Data->SetNumberField(TEXT("visited"), Visited);
					Data->SetBoolField(TEXT("truncated"), Visited >= MaxNodes);
					Data->SetArrayField(TEXT("widgets"), Out);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use windows, tree, find, or umg"), *Operation));
			});
	}
}
