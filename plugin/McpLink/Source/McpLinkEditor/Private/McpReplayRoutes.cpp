// Replay recording and playback through the PIE session's Replay Subsystem
// (the DemoNetDriver behind the `demorec` / `demoplay` console commands).

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResponder.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "ReplaySubsystem.h"

namespace McpLink
{
	namespace Replays
	{
		// FLocalFileNetworkReplayStreamer's default save path and extension.
		// Its accessors need the streamer module linked in; the values are
		// constants there.
		FString DemoDirectory()
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Demos")));
		}

		FString ReplayFile(const FString& Name)
		{
			return FPaths::Combine(DemoDirectory(), Name + TEXT(".replay"));
		}

		TArray<TSharedPtr<FJsonValue>> ListJson()
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *FPaths::Combine(DemoDirectory(), TEXT("*.replay")), true, false);
			Files.Sort();
			for (const FString& File : Files)
			{
				const FString Full = FPaths::Combine(DemoDirectory(), File);
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), FPaths::GetBaseFilename(File));
				Entry->SetStringField(TEXT("file"), Full);
				Entry->SetNumberField(TEXT("size_bytes"), static_cast<double>(IFileManager::Get().FileSize(*Full)));
				Entry->SetStringField(TEXT("modified"), IFileManager::Get().GetTimeStamp(*Full).ToIso8601());
				Out.Add(MakeShared<FJsonValueObject>(Entry));
			}
			return Out;
		}

		/// The PIE world's replay subsystem, or nullptr after responding.
		UReplaySubsystem* SubsystemOrError(const TSharedRef<FMcpResponder>& Responder, UWorld*& OutWorld)
		{
			OutWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
			if (OutWorld == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_pie"),
					TEXT("replays record and play inside a PIE session — pie_control start first"));
				return nullptr;
			}
			UGameInstance* GameInstance = OutWorld->GetGameInstance();
			UReplaySubsystem* Subsystem =
				GameInstance != nullptr ? GameInstance->GetSubsystem<UReplaySubsystem>() : nullptr;
			if (Subsystem == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_replay_subsystem"),
					TEXT("the PIE game instance has no Replay Subsystem"));
			}
			return Subsystem;
		}

		bool IsPaused(const UWorld& World)
		{
			const AWorldSettings* Settings = World.GetWorldSettings();
			return Settings != nullptr && Settings->GetPauserPlayerState() != nullptr;
		}

		TSharedRef<FJsonObject> StatusJson(UReplaySubsystem& Subsystem, UWorld& World)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("pie_active"), true);
			Data->SetBoolField(TEXT("recording"), Subsystem.IsRecording());
			Data->SetBoolField(TEXT("playing"), Subsystem.IsPlaying());
			const FString Active = Subsystem.GetActiveReplayName();
			if (!Active.IsEmpty())
			{
				Data->SetStringField(TEXT("replay"), Active);
				Data->SetStringField(TEXT("file"), ReplayFile(Active));
			}
			Data->SetNumberField(TEXT("current_time"), Subsystem.GetReplayCurrentTime());
			Data->SetNumberField(TEXT("total_time"), Subsystem.GetReplayTotalTime());
			if (const UDemoNetDriver* Driver = World.GetDemoNetDriver())
			{
				Data->SetBoolField(TEXT("fast_forwarding"), Driver->IsFastForwarding());
				Data->SetBoolField(TEXT("recording_paused"), Driver->IsRecordingPaused());
			}
			Data->SetBoolField(TEXT("paused"), IsPaused(World));
			Data->SetStringField(TEXT("map"), World.GetMapName());
			Data->SetStringField(TEXT("demo_directory"), DemoDirectory());
			return Data;
		}
	}

	void RegisterReplayRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace Replays;

		Core.RegisterRoute(TEXT("/api/editor/replay"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("demo_directory"), DemoDirectory());
					Data->SetArrayField(TEXT("replays"), ListJson());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("delete"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("a replay name from list")))
					{
						return;
					}
					const FString File = ReplayFile(Name);
					if (!IFileManager::Get().FileExists(*File))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("replay_not_found"),
							FString::Printf(TEXT("no replay at '%s'"), *File));
						return;
					}
					if (!IFileManager::Get().Delete(*File))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("delete_failed"),
							FString::Printf(TEXT("could not delete '%s' — is it being recorded or played?"), *File));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("deleted"), File);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("status") && (GEditor == nullptr || GEditor->PlayWorld == nullptr))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("pie_active"), false);
					Data->SetBoolField(TEXT("recording"), false);
					Data->SetBoolField(TEXT("playing"), false);
					Data->SetStringField(TEXT("demo_directory"), DemoDirectory());
					Data->SetArrayField(TEXT("replays"), ListJson());
					Responder->Ok(Data);
					return;
				}

				UWorld* World = nullptr;
				UReplaySubsystem* Subsystem = SubsystemOrError(Responder, World);
				if (Subsystem == nullptr)
				{
					return;
				}

				if (Operation == TEXT("status"))
				{
					Responder->Ok(StatusJson(*Subsystem, *World));
					return;
				}

				if (Operation == TEXT("start_recording"))
				{
					if (Subsystem->IsRecording() || Subsystem->IsPlaying())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("replay_active"),
							FString::Printf(TEXT("a replay is already %s ('%s') — stop it first"),
								Subsystem->IsRecording() ? TEXT("recording") : TEXT("playing"),
								*Subsystem->GetActiveReplayName()));
						return;
					}
					FString Name;
					Body->TryGetStringField(TEXT("name"), Name);
					if (Name.IsEmpty())
					{
						Name = FString::Printf(TEXT("McpReplay_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
					}
					if (Name.Contains(TEXT("/")) || Name.Contains(TEXT("\\")) || Name.Contains(TEXT(".")))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_name"),
							TEXT("'name' is a bare file stem — no directories or extension"));
						return;
					}
					FString FriendlyName;
					Body->TryGetStringField(TEXT("friendly_name"), FriendlyName);
					// The subsystem creates the DemoNetDriver, opens the local
					// file streamer and starts writing frames on the next tick.
					Subsystem->RecordReplay(Name, FriendlyName, {}, nullptr);
					const TSharedRef<FJsonObject> Data = StatusJson(*Subsystem, *World);
					Data->SetStringField(TEXT("replay"), Name);
					Data->SetStringField(TEXT("file"), ReplayFile(Name));
					Data->SetStringField(TEXT("message"),
						TEXT("recording — play the session, then stop; the file is complete once stopped"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("stop"))
				{
					const bool bWasRecording = Subsystem->IsRecording();
					const bool bWasPlaying = Subsystem->IsPlaying();
					if (!bWasRecording && !bWasPlaying)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_active"),
							TEXT("nothing is recording or playing"));
						return;
					}
					const FString Name = Subsystem->GetActiveReplayName();
					// After playback the engine's default is to travel back to
					// the project's default map, which in PIE means leaving the
					// map the session was started on; stay put unless asked.
					Subsystem->bLoadDefaultMapOnStop = BoolOr(Body, TEXT("load_default_map"), false);
					Subsystem->StopReplay();
					const TSharedRef<FJsonObject> Data = StatusJson(*Subsystem, *World);
					Data->SetStringField(TEXT("stopped"), bWasRecording ? TEXT("recording") : TEXT("playback"));
					Data->SetStringField(TEXT("replay"), Name);
					Data->SetStringField(TEXT("file"), ReplayFile(Name));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("play"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("a replay name from list")))
					{
						return;
					}
					if (!IFileManager::Get().FileExists(*ReplayFile(Name)))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("replay_not_found"),
							FString::Printf(TEXT("no replay at '%s'"), *ReplayFile(Name)));
						return;
					}
					if (Subsystem->IsRecording())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("replay_active"),
							FString::Printf(TEXT("'%s' is recording — stop it first"), *Subsystem->GetActiveReplayName()));
						return;
					}
					if (!Subsystem->PlayReplay(Name, nullptr, {}))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("play_failed"),
							TEXT("the Replay Subsystem refused to start playback — see the log"));
						return;
					}
					// A replay whose whole stream fits in its first chunk is read
					// into the packet buffer in one tick, which leaves the
					// archive at its end before the first frame is due. The
					// driver then waits for "data on frame 0" before advancing
					// time, every tick, and no more data ever comes: playback
					// sits on the first frame for good. Once the driver is up,
					// give it half a second; if no frame has been processed by
					// then, a short seek runs the buffered packets through the
					// engine's own scrubbing path, after which it plays normally.
					const TSharedRef<double> Started = MakeShared<double>(FPlatformTime::Seconds());
					const TSharedRef<double> ReadyAt = MakeShared<double>(0.0);
					FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
						[Started, ReadyAt](float) -> bool
						{
							const double Now = FPlatformTime::Seconds();
							UWorld* PlayWorld = GEditor != nullptr ? GEditor->PlayWorld.Get() : nullptr;
							UDemoNetDriver* Driver = PlayWorld != nullptr ? PlayWorld->GetDemoNetDriver() : nullptr;
							if (Driver == nullptr || !Driver->IsPlaying() || Driver->GetDemoTotalTime() <= 0.f)
							{
								// Still travelling to the recorded map; give up after a while.
								return PlayWorld != nullptr && Now - *Started < 15.0;
							}
							if (Driver->GetDemoFrameNum() > 0 || Driver->IsFastForwarding())
							{
								return false;
							}
							if (*ReadyAt == 0.0)
							{
								*ReadyAt = Now;
								return true;
							}
							if (Now - *ReadyAt < 0.5)
							{
								return true;
							}
							Driver->GotoTimeInSeconds(FMath::Min(0.1f, Driver->GetDemoTotalTime()));
							return false;
						}), 0.f);
					const TSharedRef<FJsonObject> Data = StatusJson(*Subsystem, *World);
					Data->SetStringField(TEXT("replay"), Name);
					Data->SetStringField(TEXT("message"),
						TEXT("playback starts by travelling the PIE world to the recorded map; ")
						TEXT("poll status until playing is true and total_time is known"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("goto") || Operation == TEXT("pause") || Operation == TEXT("resume")
					|| Operation == TEXT("set_speed"))
				{
					if (!Subsystem->IsPlaying())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_playing"),
							TEXT("no replay is playing — play one first"));
						return;
					}
					// GEditor->PlayWorld can lag a replay's map travel; the
					// driver's own world is the one being played.
					UDemoNetDriver* Driver = World->GetDemoNetDriver();
					if (Driver == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_playing"),
							TEXT("the PIE world has no demo driver yet — playback is still loading; poll status"));
						return;
					}
					if (Operation == TEXT("goto"))
					{
						double Time = 0.0;
						if (!Body->TryGetNumberField(TEXT("time"), Time) || Time < 0.0)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'time' (seconds into the replay) is required"));
							return;
						}
						Driver->GotoTimeInSeconds(static_cast<float>(Time));
					}
					else if (Operation == TEXT("set_speed"))
					{
						double Speed = 1.0;
						if (!Body->TryGetNumberField(TEXT("speed"), Speed) || Speed <= 0.0)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'speed' (a positive playback multiplier) is required"));
							return;
						}
						World->GetWorldSettings()->DemoPlayTimeDilation = static_cast<float>(Speed);
					}
					else
					{
						// The demo driver stops advancing while the world settings
						// name any pauser. The spectator controller has no
						// PlayerState of its own (playback is a net client, and
						// clients never create one), so use a replicated one from
						// the recording — the demo driver only checks for non-null.
						APlayerState* Pauser = nullptr;
						if (Operation == TEXT("pause"))
						{
							for (TActorIterator<APlayerState> It(World); It; ++It)
							{
								Pauser = *It;
								break;
							}
							if (Pauser == nullptr)
							{
								Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_player_state"),
									TEXT("no player state has been replicated from the recording yet — ")
									TEXT("playback is still loading; poll status"));
								return;
							}
						}
						World->GetWorldSettings()->SetPauserPlayerState(Pauser);
					}
					const TSharedRef<FJsonObject> Data = StatusJson(*Subsystem, *World);
					Data->SetStringField(TEXT("applied"), Operation);
					Data->SetNumberField(TEXT("speed"), World->GetWorldSettings()->DemoPlayTimeDilation);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected status, list, start_recording, ")
						TEXT("stop, play, goto, pause, resume, set_speed or delete"), *Operation));
			});
	}
}
