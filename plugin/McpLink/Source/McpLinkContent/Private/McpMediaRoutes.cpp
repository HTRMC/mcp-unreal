// Media Framework assets: file sources, playlists, players and the media
// texture a material samples, plus playback control on a player.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FileMediaSource.h"
#include "HAL/FileManager.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "MediaPlayer.h"
#include "MediaPlaylist.h"
#include "MediaSource.h"
#include "MediaTexture.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace Media
	{
		template <typename T>
		T* NewMediaAsset(const FString& Path, const TSharedRef<FMcpResponder>& Responder)
		{
			if (!FPackageName::IsValidLongPackageName(Path))
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_path"),
					FString::Printf(TEXT("'%s' is not a package path — use /Game/Folder/Name"), *Path));
				return nullptr;
			}
			if (FindPackage(nullptr, *Path) != nullptr || FPackageName::DoesPackageExist(Path))
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
					FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
				return nullptr;
			}
			UPackage* Package = CreatePackage(*Path);
			T* Asset = NewObject<T>(Package, FName(*FPackageName::GetShortName(Path)),
				RF_Public | RF_Standalone | RF_Transactional);
			FAssetRegistryModule::AssetCreated(Asset);
			Package->MarkPackageDirty();
			return Asset;
		}

		UMediaPlayer* PlayerOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("player"), Path, Responder, TEXT("a Media Player asset path")))
			{
				return nullptr;
			}
			UMediaPlayer* Player = Cast<UMediaPlayer>(ResolveAsset(Path));
			if (Player == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("player_not_found"),
					FString::Printf(TEXT("no Media Player at '%s'"), *Path));
			}
			return Player;
		}

		double Seconds(const FTimespan& Span)
		{
			return Span.GetTotalSeconds();
		}

		TSharedRef<FJsonObject> PlayerJson(UMediaPlayer& Player)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("player"), Player.GetPathName());
			Data->SetStringField(TEXT("url"), Player.GetUrl());
			Data->SetStringField(TEXT("player_plugin"), Player.GetPlayerName().ToString());
			Data->SetBoolField(TEXT("closed"), Player.IsClosed());
			Data->SetBoolField(TEXT("preparing"), Player.IsPreparing());
			Data->SetBoolField(TEXT("ready"), Player.IsReady());
			Data->SetBoolField(TEXT("playing"), Player.IsPlaying());
			Data->SetBoolField(TEXT("paused"), Player.IsPaused());
			Data->SetBoolField(TEXT("looping"), Player.IsLooping());
			Data->SetBoolField(TEXT("error"), Player.HasError());
			Data->SetNumberField(TEXT("time"), Seconds(Player.GetTime()));
			Data->SetNumberField(TEXT("duration"), Seconds(Player.GetDuration()));
			Data->SetNumberField(TEXT("rate"), Player.GetRate());
			Data->SetNumberField(TEXT("video_tracks"), Player.GetNumTracks(EMediaPlayerTrack::Video));
			Data->SetNumberField(TEXT("audio_tracks"), Player.GetNumTracks(EMediaPlayerTrack::Audio));
			if (Player.GetNumTracks(EMediaPlayerTrack::Video) > 0)
			{
				const FIntPoint Dimensions = Player.GetVideoTrackDimensions(INDEX_NONE, INDEX_NONE);
				Data->SetNumberField(TEXT("video_width"), Dimensions.X);
				Data->SetNumberField(TEXT("video_height"), Dimensions.Y);
			}
			return Data;
		}

		/// A file for a media source: absolute, or relative to the project's
		/// Content directory as the Media Framework resolves it.
		bool ResolveMediaFile(const FString& In, FString& OutAbsolute)
		{
			OutAbsolute = FPaths::IsRelative(In) ? FPaths::Combine(FPaths::ProjectContentDir(), In) : In;
			OutAbsolute = FPaths::ConvertRelativePathToFull(OutAbsolute);
			return IFileManager::Get().FileExists(*OutAbsolute);
		}
	}

	void RegisterMediaRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace Media;

		Core.RegisterRoute(TEXT("/api/content/media"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create_file_source"))
				{
					FString Path, File;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Media/MS_Intro"))
						|| !RequireString(Body, TEXT("file"), File, Responder,
							TEXT("the media file — absolute, or relative to the project's Content folder")))
					{
						return;
					}
					FString Absolute;
					if (!ResolveMediaFile(File, Absolute))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("file_not_found"),
							FString::Printf(TEXT("no file at '%s'"), *Absolute));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateMediaSource", "McpLink Create Media Source"));
					UFileMediaSource* Source = NewMediaAsset<UFileMediaSource>(Path, Responder);
					if (Source == nullptr)
					{
						return;
					}
					// A file under Content is stored relative, so the asset
					// survives a project move; anything else stays absolute.
					FString Stored = Absolute;
					const FString ContentDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir());
					if (FPaths::MakePathRelativeTo(Stored, *ContentDir) && !Stored.StartsWith(TEXT("..")))
					{
						Stored = TEXT("./") + Stored;
					}
					else
					{
						Stored = Absolute;
					}
					Source->SetFilePath(Stored);
					Source->PrecacheFile = BoolOr(Body, TEXT("precache"), false);
					Source->PostEditChange();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("source"), Source->GetPathName());
					Data->SetStringField(TEXT("file"), Source->GetFilePath());
					Data->SetStringField(TEXT("url"), Source->GetUrl());
					Data->SetBoolField(TEXT("valid"), Source->Validate());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_player"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Media/MP_Intro")))
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateMediaPlayer", "McpLink Create Media Player"));
					UMediaPlayer* Player = NewMediaAsset<UMediaPlayer>(Path, Responder);
					if (Player == nullptr)
					{
						return;
					}
					Player->PlayOnOpen = BoolOr(Body, TEXT("play_on_open"), true);
					Player->SetLooping(BoolOr(Body, TEXT("loop"), false));
					Player->PostEditChange();
					const TSharedRef<FJsonObject> Data = PlayerJson(*Player);
					Data->SetStringField(TEXT("message"),
						TEXT("created — create_texture gives a material something to sample, open starts a source"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_texture"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Media/MT_Intro")))
					{
						return;
					}
					UMediaPlayer* Player = PlayerOrError(Body, Responder);
					if (Player == nullptr)
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateMediaTexture", "McpLink Create Media Texture"));
					UMediaTexture* Texture = NewMediaAsset<UMediaTexture>(Path, Responder);
					if (Texture == nullptr)
					{
						return;
					}
					Texture->SetMediaPlayer(Player);
					Texture->UpdateResource();
					Texture->PostEditChange();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("texture"), Texture->GetPathName());
					Data->SetStringField(TEXT("player"), Player->GetPathName());
					Data->SetStringField(TEXT("message"),
						TEXT("sample it from a material with a TextureSample expression whose Texture is this asset"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_playlist"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("e.g. /Game/Media/MPL_Menu")))
					{
						return;
					}
					const TArray<TSharedPtr<FJsonValue>>* Sources = nullptr;
					Body->TryGetArrayField(TEXT("sources"), Sources);
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateMediaPlaylist", "McpLink Create Media Playlist"));
					UMediaPlaylist* Playlist = NewMediaAsset<UMediaPlaylist>(Path, Responder);
					if (Playlist == nullptr)
					{
						return;
					}
					TArray<TSharedPtr<FJsonValue>> Added;
					if (Sources != nullptr)
					{
						for (const TSharedPtr<FJsonValue>& Value : *Sources)
						{
							FString Spec;
							if (!Value->TryGetString(Spec))
							{
								continue;
							}
							// An asset path is a media source asset; anything
							// else is a file (or URL) added directly.
							if (UMediaSource* Source = Cast<UMediaSource>(ResolveAsset(Spec)))
							{
								Playlist->Add(Source);
								Added.Add(MakeShared<FJsonValueString>(Source->GetPathName()));
							}
							else if (Spec.Contains(TEXT("://")) ? Playlist->AddUrl(Spec) : Playlist->AddFile(Spec))
							{
								Added.Add(MakeShared<FJsonValueString>(Spec));
							}
						}
					}
					Playlist->PostEditChange();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("playlist"), Playlist->GetPathName());
					Data->SetNumberField(TEXT("count"), Playlist->Num());
					Data->SetArrayField(TEXT("added"), Added);
					Responder->Ok(Data);
					return;
				}

				UMediaPlayer* Player = PlayerOrError(Body, Responder);
				if (Player == nullptr)
				{
					return;
				}

				if (Operation == TEXT("status"))
				{
					Responder->Ok(PlayerJson(*Player));
					return;
				}

				if (Operation == TEXT("open"))
				{
					FString SourcePath, File, Url;
					bool bOpened = false;
					FString Opened;
					if (Body->TryGetStringField(TEXT("source"), SourcePath) && !SourcePath.IsEmpty())
					{
						UMediaSource* Source = Cast<UMediaSource>(ResolveAsset(SourcePath));
						if (Source == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("source_not_found"),
								FString::Printf(TEXT("no Media Source at '%s'"), *SourcePath));
							return;
						}
						bOpened = Player->OpenSource(Source);
						Opened = Source->GetPathName();
					}
					else if (Body->TryGetStringField(TEXT("file"), File) && !File.IsEmpty())
					{
						FString Absolute;
						if (!ResolveMediaFile(File, Absolute))
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("file_not_found"),
								FString::Printf(TEXT("no file at '%s'"), *Absolute));
							return;
						}
						bOpened = Player->OpenFile(Absolute);
						Opened = Absolute;
					}
					else if (Body->TryGetStringField(TEXT("url"), Url) && !Url.IsEmpty())
					{
						bOpened = Player->OpenUrl(Url);
						Opened = Url;
					}
					else
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'source' (a Media Source asset), 'file' or 'url' is required"));
						return;
					}
					if (!bOpened)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("open_failed"),
							FString::Printf(TEXT("the player refused '%s' — no media plugin handles it"), *Opened));
						return;
					}
					const TSharedRef<FJsonObject> Data = PlayerJson(*Player);
					Data->SetStringField(TEXT("opened"), Opened);
					Data->SetStringField(TEXT("note"),
						TEXT("opening is asynchronous — poll status until ready is true; play_on_open then starts it"));
					Responder->Ok(Data);
					return;
				}

				bool bResult = true;
				if (Operation == TEXT("play"))
				{
					bResult = Player->Play();
				}
				else if (Operation == TEXT("pause"))
				{
					bResult = Player->Pause();
				}
				else if (Operation == TEXT("rewind"))
				{
					bResult = Player->Rewind();
				}
				else if (Operation == TEXT("close"))
				{
					Player->Close();
				}
				else if (Operation == TEXT("seek"))
				{
					double Time = 0.0;
					if (!Body->TryGetNumberField(TEXT("time"), Time) || Time < 0.0)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'time' (seconds) is required"));
						return;
					}
					bResult = Player->Seek(FTimespan::FromSeconds(Time));
				}
				else if (Operation == TEXT("set_rate"))
				{
					double Rate = 1.0;
					if (!Body->TryGetNumberField(TEXT("rate"), Rate))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'rate' (1 is normal speed, 0 pauses, negative plays backwards where supported) is required"));
						return;
					}
					bResult = Player->SetRate(static_cast<float>(Rate));
				}
				else if (Operation == TEXT("set_looping"))
				{
					bResult = Player->SetLooping(BoolOr(Body, TEXT("loop"), true));
				}
				else
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
						FString::Printf(TEXT("unknown operation '%s' — expected create_file_source, create_player, ")
							TEXT("create_texture, create_playlist, open, play, pause, rewind, seek, set_rate, ")
							TEXT("set_looping, close or status"), *Operation));
					return;
				}
				if (!bResult)
				{
					Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("refused"),
						FString::Printf(TEXT("the player refused '%s' — is a source open and ready? (status says)"), *Operation));
					return;
				}
				const TSharedRef<FJsonObject> Data = PlayerJson(*Player);
				Data->SetStringField(TEXT("applied"), Operation);
				Responder->Ok(Data);
			});
	}
}
