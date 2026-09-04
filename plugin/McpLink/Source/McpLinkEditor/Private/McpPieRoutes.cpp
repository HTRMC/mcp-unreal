// PIE lifecycle and player control. PIE start/stop can optionally block until
// the engine reports the transition via FEditorDelegates, instead of making
// the client poll.

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "LevelEditorViewport.h"
#include "McpJson.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResolve.h"
#include "McpResponder.h"

namespace McpLink
{
	namespace
	{
		constexpr float PieWaitTimeoutSeconds = 30.0f;

		TSharedRef<FJsonObject> PieStatusJson()
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			const bool bActive = GEditor != nullptr && GEditor->IsPlayingSessionInEditor();
			Data->SetBoolField(TEXT("pie_active"), bActive);
			if (bActive && GEditor->PlayWorld != nullptr)
			{
				Data->SetStringField(TEXT("map"), GEditor->PlayWorld->GetMapName());
				Data->SetBoolField(TEXT("paused"), GEditor->PlayWorld->IsPaused());
			}
			return Data;
		}

		// Complete the responder when Delegate fires, or fail after a timeout.
		// The delegate handle and ticker are torn down exactly once.
		// bExpectedActive overrides the reported pie_active flag: EndPIE fires
		// while GEditor still reports a live session, so a snapshot taken inside
		// the delegate would tell the caller the opposite of what it just did.
		void CompleteOnPieEvent(
			FEditorDelegates::FOnPIEEvent& Delegate,
			TSharedRef<FMcpResponder> Responder,
			const FString& TimeoutMessage,
			bool bExpectedActive)
		{
			const TSharedRef<FDelegateHandle> Handle = MakeShared<FDelegateHandle>();
			const TSharedRef<bool> bDone = MakeShared<bool>(false);
			FEditorDelegates::FOnPIEEvent* DelegatePtr = &Delegate;

			*Handle = Delegate.AddLambda(
				[Responder, Handle, bDone, DelegatePtr, bExpectedActive](const bool /*bIsSimulating*/)
				{
					if (*bDone)
					{
						return;
					}
					*bDone = true;
					DelegatePtr->Remove(*Handle);
					const TSharedRef<FJsonObject> Data = PieStatusJson();
					Data->SetBoolField(TEXT("pie_active"), bExpectedActive);
					Responder->Ok(Data);
				});

			float Elapsed = 0.0f;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[Responder, Handle, bDone, DelegatePtr, TimeoutMessage, Elapsed](float DeltaTime) mutable -> bool
					{
						if (*bDone)
						{
							return false;
						}
						Elapsed += DeltaTime;
						if (Elapsed < PieWaitTimeoutSeconds)
						{
							return true;
						}
						*bDone = true;
						DelegatePtr->Remove(*Handle);
						Responder->Error(
							EHttpServerResponseCodes::ServerError, TEXT("pie_timeout"), TimeoutMessage);
						return false;
					}),
				0.0f);
		}

		APlayerController* PieControllerOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("pie_not_running"),
					TEXT("no PIE session — start one with pie_control operation 'start'"));
				return nullptr;
			}
			const int32 PlayerIndex = Body->HasTypedField<EJson::Number>(TEXT("player_index"))
				? static_cast<int32>(Body->GetNumberField(TEXT("player_index")))
				: 0;
			APlayerController* PC = UGameplayStatics::GetPlayerController(GEditor->PlayWorld, PlayerIndex);
			if (PC == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("no_player_controller"),
					FString::Printf(TEXT("no player controller at index %d"), PlayerIndex));
			}
			return PC;
		}
	}

	void RegisterPieRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/editor/pie_control"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				if (GEditor == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_editor"),
						TEXT("GEditor unavailable"));
					return;
				}
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);
				bool bWait = false;
				Body->TryGetBoolField(TEXT("wait"), bWait);
				const bool bActive = GEditor->IsPlayingSessionInEditor();

				if (Operation == TEXT("status"))
				{
					Responder->Ok(PieStatusJson());
					return;
				}
				if (Operation == TEXT("start"))
				{
					if (bActive)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("pie_already_running"),
							TEXT("a PIE session is already running — stop it first"));
						return;
					}
					bool bSimulate = false;
					Body->TryGetBoolField(TEXT("simulate"), bSimulate);

					FRequestPlaySessionParams Params;
					Params.SessionDestination = EPlaySessionDestinationType::InProcess;
					Params.WorldType = bSimulate
						? EPlaySessionWorldType::SimulateInEditor
						: EPlaySessionWorldType::PlayInEditor;
					FString MapOverride;
					if (Body->TryGetStringField(TEXT("map"), MapOverride) && !MapOverride.IsEmpty())
					{
						Params.GlobalMapOverride = MapOverride;
					}
					FVector StartLocation;
					if (GetVector(Body, TEXT("location"), StartLocation))
					{
						Params.StartLocation = StartLocation;
					}
					FRotator StartRotation;
					if (GetRotator(Body, TEXT("rotation"), StartRotation))
					{
						Params.StartRotation = StartRotation;
					}

					// Multiplayer options live on the play settings, not on the
					// request. Duplicating the defaults keeps a one-off net-mode
					// run from rewriting the user's editor preferences.
					const int32 Players = FMath::Clamp(IntOr(Body, TEXT("players"), 1), 1, 8);
					FString NetModeSpec;
					Body->TryGetStringField(TEXT("net_mode"), NetModeSpec);
					const bool bDedicated = BoolOr(Body, TEXT("dedicated_server"), false);
					if (Players > 1 || !NetModeSpec.IsEmpty() || bDedicated)
					{
						if (bSimulate)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest,
								TEXT("invalid_request"),
								TEXT("simulate has no players — drop 'simulate' to run a networked session"));
							return;
						}
						ULevelEditorPlaySettings* Settings = DuplicateObject<ULevelEditorPlaySettings>(
							GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage());
						const FString NetMode = NetModeSpec.ToLower();
						if (NetMode.IsEmpty() || NetMode == TEXT("standalone"))
						{
							Settings->SetPlayNetMode(EPlayNetMode::PIE_Standalone);
						}
						else if (NetMode == TEXT("listen_server") || NetMode == TEXT("listen"))
						{
							Settings->SetPlayNetMode(EPlayNetMode::PIE_ListenServer);
						}
						else if (NetMode == TEXT("client"))
						{
							Settings->SetPlayNetMode(EPlayNetMode::PIE_Client);
						}
						else
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest,
								TEXT("unknown_net_mode"),
								FString::Printf(
									TEXT("unknown net_mode '%s' — use standalone, listen_server or client"),
									*NetModeSpec));
							return;
						}
						Settings->SetPlayNumberOfClients(Players);
						Settings->bLaunchSeparateServer = bDedicated;
						// One process keeps every client inside this editor, which
						// is what makes the other tools able to reach them.
						Settings->SetRunUnderOneProcess(BoolOr(Body, TEXT("one_process"), true));
						Params.EditorPlaySettings = Settings;
					}

					if (bWait)
					{
						CompleteOnPieEvent(FEditorDelegates::PostPIEStarted, Responder,
							TEXT("PIE did not start within 30s — check the output log for load errors"),
							/*bExpectedActive*/ true);
					}
					GEditor->RequestPlaySession(Params);
					if (!bWait)
					{
						const TSharedRef<FJsonObject> Data = PieStatusJson();
						Data->SetStringField(TEXT("message"),
							TEXT("PIE start requested (async) — poll with operation 'status', or pass wait=true"));
						Responder->Ok(Data);
					}
					return;
				}
				if (Operation == TEXT("stop"))
				{
					if (!bActive)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("pie_not_running"),
							TEXT("no PIE session is running"));
						return;
					}
					if (bWait)
					{
						CompleteOnPieEvent(FEditorDelegates::EndPIE, Responder,
							TEXT("PIE did not stop within 30s"), /*bExpectedActive*/ false);
					}
					GEditor->RequestEndPlayMap();
					if (!bWait)
					{
						const TSharedRef<FJsonObject> Data = PieStatusJson();
						Data->SetStringField(TEXT("message"), TEXT("PIE stop requested (async)"));
						Responder->Ok(Data);
					}
					return;
				}
				if (Operation == TEXT("pause") || Operation == TEXT("resume"))
				{
					if (!bActive)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("pie_not_running"),
							TEXT("no PIE session is running"));
						return;
					}
					GEditor->SetPIEWorldsPaused(Operation == TEXT("pause"));
					Responder->Ok(PieStatusJson());
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use start, stop, status, pause, or resume"), *Operation));
			});

		Core.RegisterRoute(TEXT("/api/editor/player_control"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				// Editor viewport camera operations work without PIE.
				if (Operation == TEXT("get_camera") || Operation == TEXT("set_camera"))
				{
					FLevelEditorViewportClient* Viewport = GCurrentLevelEditingViewportClient;
					if (Viewport == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_viewport"),
							TEXT("no active level editor viewport (running headless?)"));
						return;
					}
					if (Operation == TEXT("set_camera"))
					{
						FVector Location;
						FRotator Rotation;
						if (GetVector(Body, TEXT("location"), Location))
						{
							Viewport->SetViewLocation(Location);
						}
						if (GetRotator(Body, TEXT("rotation"), Rotation))
						{
							Viewport->SetViewRotation(Rotation);
						}
						Viewport->Invalidate();
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("location"), VectorToJson(Viewport->GetViewLocation()));
					Data->SetArrayField(TEXT("rotation"), RotatorToJson(Viewport->GetViewRotation()));
					Responder->Ok(Data);
					return;
				}

				APlayerController* PC = PieControllerOrError(Body, Responder);
				if (!PC)
				{
					return;
				}
				APawn* Pawn = PC->GetPawn();

				if (Operation == TEXT("get_info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("controller"), PC->GetPathName());
					Data->SetArrayField(TEXT("control_rotation"), RotatorToJson(PC->GetControlRotation()));
					if (Pawn != nullptr)
					{
						Data->SetStringField(TEXT("pawn"), Pawn->GetPathName());
						Data->SetStringField(TEXT("pawn_class"), Pawn->GetClass()->GetName());
						Data->SetArrayField(TEXT("location"), VectorToJson(Pawn->GetActorLocation()));
						Data->SetArrayField(TEXT("rotation"), RotatorToJson(Pawn->GetActorRotation()));
						Data->SetArrayField(TEXT("velocity"), VectorToJson(Pawn->GetVelocity()));
					}
					else
					{
						Data->SetStringField(TEXT("pawn"), TEXT(""));
					}
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("teleport"))
				{
					if (Pawn == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_pawn"),
							TEXT("player controller has no pawn to teleport"));
						return;
					}
					FVector Location;
					if (!GetVector(Body, TEXT("location"), Location))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'location' [X,Y,Z] is required"));
						return;
					}
					FRotator Rotation = Pawn->GetActorRotation();
					GetRotator(Body, TEXT("rotation"), Rotation);
					const bool bMoved = Pawn->TeleportTo(Location, Rotation);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("teleported"), bMoved);
					Data->SetArrayField(TEXT("location"), VectorToJson(Pawn->GetActorLocation()));
					Data->SetArrayField(TEXT("rotation"), RotatorToJson(Pawn->GetActorRotation()));
					if (!bMoved)
					{
						Data->SetStringField(TEXT("message"),
							TEXT("teleport blocked — destination may be occupied"));
					}
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("set_control_rotation"))
				{
					FRotator Rotation;
					if (!GetRotator(Body, TEXT("rotation"), Rotation))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'rotation' [Pitch,Yaw,Roll] is required"));
						return;
					}
					PC->SetControlRotation(Rotation);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("control_rotation"), RotatorToJson(PC->GetControlRotation()));
					Responder->Ok(Data);
					return;
				}
				if (Operation == TEXT("possess"))
				{
					FString ActorSpec;
					if (!Body->TryGetStringField(TEXT("actor"), ActorSpec) || ActorSpec.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'actor' (a Pawn path or label) is required"));
						return;
					}
					APawn* Target = Cast<APawn>(ResolveActor(GEditor->PlayWorld, ActorSpec));
					if (Target == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("pawn_not_found"),
							FString::Printf(TEXT("no pawn '%s' in the PIE world"), *ActorSpec));
						return;
					}
					PC->Possess(Target);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("pawn"), Target->GetPathName());
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use get_info, teleport, set_control_rotation, possess, get_camera, or set_camera"),
						*Operation));
			});
	}
}
