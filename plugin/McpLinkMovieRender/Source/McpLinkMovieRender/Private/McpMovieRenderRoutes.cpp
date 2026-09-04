// Movie Render Queue: turning an authored Level Sequence into files on disk.
//
// sequence_ops can build a sequence but not render one, which left the whole
// cinematic pipeline stopping one step short. A render needs three things: a
// primary config (which settings produce which files), a job (which sequence,
// on which map), and an executor to run it.
//
// Rendering runs PIE, so it needs a windowed editor and it is asynchronous —
// `render` starts it and `render_status` reports progress, rather than holding
// the HTTP request open for minutes.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "LevelSequence.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelineOutputSetting.h"
#include "MoviePipelinePIEExecutor.h"
#include "MoviePipelinePrimaryConfig.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineQueueSubsystem.h"
#include "MoviePipelineSetting.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace MovieRender
	{
		UMoviePipelineQueueSubsystem* QueueSubsystem()
		{
			return GEditor != nullptr ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>()
									  : nullptr;
		}

		UMoviePipelinePrimaryConfig* ConfigOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("config"), Path, Responder,
					TEXT("a Movie Pipeline config asset path — movie_render create_config makes one")))
			{
				return nullptr;
			}
			UMoviePipelinePrimaryConfig* Config =
				Cast<UMoviePipelinePrimaryConfig>(ResolveAsset(Path));
			if (Config == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("config_not_found"),
					FString::Printf(TEXT("no Movie Pipeline config at '%s'"), *Path));
			}
			return Config;
		}

		TSharedRef<FJsonObject> SettingToJson(UMoviePipelineSetting* Setting)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("class"), Setting->GetClass()->GetName());
			Object->SetBoolField(TEXT("enabled"), Setting->IsEnabled());
			// Every knob a setting has is a UPROPERTY, so set_property on this
			// path is how resolution, output directory and codecs are set.
			Object->SetStringField(TEXT("path"), Setting->GetPathName());
			return Object;
		}
	}

	using namespace MovieRender;

	void RegisterMovieRenderRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/render/movie"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_setting_classes"))
				{
					FString Contains;
					Body->TryGetStringField(TEXT("name_contains"), Contains);
					TArray<TSharedPtr<FJsonValue>> Classes;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						UClass* Class = *It;
						if (!Class->IsChildOf(UMoviePipelineSetting::StaticClass())
							|| Class->HasAnyClassFlags(
								CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
						{
							continue;
						}
						const FString Name = Class->GetName();
						if (!Contains.IsEmpty() && !Name.Contains(Contains))
						{
							continue;
						}
						Classes.Add(MakeShared<FJsonValueString>(Name));
					}
					Classes.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
						{ return A->AsString() < B->AsString(); });
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("count"), Classes.Num());
					Data->SetArrayField(TEXT("classes"), Classes);
					Data->SetStringField(TEXT("message"),
						TEXT("output types (DeferredPass, PNG, JPG, EXR, WAV, CommandLineEncoder), ")
						TEXT("anti-aliasing, burn-ins and console-variable overrides all live here"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("render_status"))
				{
					UMoviePipelineQueueSubsystem* Subsystem = QueueSubsystem();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					const bool bRendering = Subsystem != nullptr && Subsystem->IsRendering();
					Data->SetBoolField(TEXT("rendering"), bRendering);
					if (Subsystem != nullptr)
					{
						Data->SetNumberField(TEXT("jobs"), Subsystem->GetQueue()->GetJobs().Num());
						TArray<TSharedPtr<FJsonValue>> Jobs;
						for (UMoviePipelineExecutorJob* Job : Subsystem->GetQueue()->GetJobs())
						{
							if (Job == nullptr)
							{
								continue;
							}
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetStringField(TEXT("name"), Job->JobName);
							Item->SetStringField(TEXT("sequence"), Job->Sequence.ToString());
							Item->SetStringField(TEXT("status"), Job->GetStatusMessage());
							Item->SetNumberField(TEXT("progress"), Job->GetStatusProgress());
							Item->SetBoolField(TEXT("consumed"), Job->IsConsumed());
							Jobs.Add(MakeShared<FJsonValueObject>(Item));
						}
						Data->SetArrayField(TEXT("queue"), Jobs);
					}
					if (!bRendering)
					{
						Data->SetStringField(TEXT("message"),
							TEXT("nothing rendering — a finished job's files are under the output ")
							TEXT("directory of its config's MoviePipelineOutputSetting"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_config"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/Cinematics/MRQ_Preview")))
					{
						return;
					}
					if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr, *Path) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateMoviePipelineConfig", "McpLink Create Render Config"));
					UPackage* Package = CreatePackage(*Path);
					UMoviePipelinePrimaryConfig* Config = NewObject<UMoviePipelinePrimaryConfig>(
						Package, FName(*FPackageName::GetShortName(Path)),
						RF_Public | RF_Standalone | RF_Transactional);
					// A config with no render pass and no output type produces
					// nothing at all, so seed the pair every render needs.
					Config->FindOrAddSettingByClass(UMoviePipelineOutputSetting::StaticClass());
					FString PassName = TEXT("MoviePipelineDeferredPassBase");
					Body->TryGetStringField(TEXT("render_pass"), PassName);
					FString OutputName = TEXT("MoviePipelineImageSequenceOutput_PNG");
					Body->TryGetStringField(TEXT("output_type"), OutputName);
					TArray<FString> Missing;
					for (const FString& ClassName : {PassName, OutputName})
					{
						UClass* Class = ResolveClass(ClassName);
						if (Class != nullptr && Class->IsChildOf(UMoviePipelineSetting::StaticClass()))
						{
							Config->FindOrAddSettingByClass(Class);
						}
						else
						{
							Missing.Add(ClassName);
						}
					}
					FAssetRegistryModule::AssetCreated(Config);
					Package->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("config"), Config->GetPathName());
					TArray<TSharedPtr<FJsonValue>> Settings;
					for (UMoviePipelineSetting* Setting : Config->GetUserSettings())
					{
						if (Setting != nullptr)
						{
							Settings.Add(MakeShared<FJsonValueObject>(SettingToJson(Setting)));
						}
					}
					Data->SetArrayField(TEXT("settings"), Settings);
					if (!Missing.IsEmpty())
					{
						Data->SetStringField(TEXT("warning"),
							FString::Printf(
								TEXT("not a Movie Pipeline setting class: %s — see list_setting_classes"),
								*FString::Join(Missing, TEXT(", "))));
					}
					Data->SetStringField(TEXT("message"),
						TEXT("tune the settings with set_property on their paths (output directory, ")
						TEXT("resolution, frame range), then save and render"));
					Responder->Ok(Data);
					return;
				}

				UMoviePipelinePrimaryConfig* Config = ConfigOrError(Body, Responder);
				if (Config == nullptr)
				{
					return;
				}

				if (Operation == TEXT("config_info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("config"), Config->GetPathName());
					TArray<TSharedPtr<FJsonValue>> Settings;
					for (UMoviePipelineSetting* Setting : Config->GetUserSettings())
					{
						if (Setting != nullptr)
						{
							Settings.Add(MakeShared<FJsonValueObject>(SettingToJson(Setting)));
						}
					}
					Data->SetArrayField(TEXT("settings"), Settings);
					if (UMoviePipelineOutputSetting* Output = Cast<UMoviePipelineOutputSetting>(
							Config->FindSettingByClass(UMoviePipelineOutputSetting::StaticClass())))
					{
						Data->SetStringField(TEXT("output_directory"), Output->OutputDirectory.Path);
						Data->SetStringField(TEXT("file_name_format"), Output->FileNameFormat);
						Data->SetArrayField(TEXT("resolution"),
							{MakeShared<FJsonValueNumber>(Output->OutputResolution.X),
								MakeShared<FJsonValueNumber>(Output->OutputResolution.Y)});
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Config, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("config"), Config->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_setting") || Operation == TEXT("remove_setting"))
				{
					FString ClassName;
					if (!RequireString(Body, TEXT("class"), ClassName, Responder,
							TEXT("a setting class from list_setting_classes")))
					{
						return;
					}
					UClass* Class = ResolveClass(ClassName);
					if (Class == nullptr || !Class->IsChildOf(UMoviePipelineSetting::StaticClass())
						|| Class->HasAnyClassFlags(CLASS_Abstract))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_class"),
							FString::Printf(
								TEXT("'%s' is not a concrete UMoviePipelineSetting — see ")
								TEXT("list_setting_classes"),
								*ClassName));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditMoviePipelineConfig", "McpLink Edit Render Config"));
					Config->Modify();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					if (Operation == TEXT("add_setting"))
					{
						UMoviePipelineSetting* Setting = Config->FindOrAddSettingByClass(Class);
						if (Setting == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_failed"),
								FString::Printf(TEXT("the config refused a %s"), *ClassName));
							return;
						}
						Data->SetObjectField(TEXT("setting"), SettingToJson(Setting));
					}
					else
					{
						UMoviePipelineSetting* Setting = Config->FindSettingByClass(Class);
						if (Setting == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("setting_not_found"),
								FString::Printf(TEXT("this config has no %s"), *ClassName));
							return;
						}
						Config->RemoveSetting(Setting);
						Data->SetStringField(TEXT("removed"), ClassName);
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("render"))
				{
					// The PIE executor renders through a Play In Editor session,
					// so there has to be something to render into.
					if (!FApp::CanEverRender())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_rendering"),
							TEXT("Movie Render Queue renders through PIE, which an editor without ")
							TEXT("rendering (-nullrhi) cannot start — run a windowed editor"));
						return;
					}
					UMoviePipelineQueueSubsystem* Subsystem = QueueSubsystem();
					if (Subsystem == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_subsystem"),
							TEXT("the Movie Pipeline queue subsystem is unavailable"));
						return;
					}
					if (Subsystem->IsRendering())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_rendering"),
							TEXT("a render is already running — poll render_status"));
						return;
					}

					FString SequencePath;
					if (!RequireString(Body, TEXT("sequence"), SequencePath, Responder,
							TEXT("the Level Sequence to render")))
					{
						return;
					}
					ULevelSequence* Sequence = Cast<ULevelSequence>(ResolveAsset(SequencePath));
					if (Sequence == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("sequence_not_found"),
							FString::Printf(TEXT("no Level Sequence at '%s'"), *SequencePath));
						return;
					}

					FString MapPath;
					Body->TryGetStringField(TEXT("map"), MapPath);
					if (MapPath.IsEmpty())
					{
						UWorld* World =
							GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
						if (World == nullptr || World->GetOutermost() == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'map' is required — there is no open level to default to"));
							return;
						}
						MapPath = World->GetOutermost()->GetName();
					}

					UMoviePipelineQueue* Queue = Subsystem->GetQueue();
					if (BoolOr(Body, TEXT("clear_queue"), true))
					{
						for (UMoviePipelineExecutorJob* Existing : TArray<UMoviePipelineExecutorJob*>(
								 Queue->GetJobs()))
						{
							Queue->DeleteJob(Existing);
						}
					}
					UMoviePipelineExecutorJob* Job = Queue->AllocateNewJob(
						UMoviePipelineExecutorJob::StaticClass());
					if (Job == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("job_failed"),
							TEXT("the queue refused a new job"));
						return;
					}
					FString JobName;
					Body->TryGetStringField(TEXT("name"), JobName);
					Job->JobName = JobName.IsEmpty() ? Sequence->GetName() : JobName;
					Job->Sequence = FSoftObjectPath(Sequence);
					Job->Map = FSoftObjectPath(MapPath);
					Job->SetConfiguration(Config);

					UMoviePipelineExecutorBase* Executor = Subsystem->RenderQueueWithExecutor(
						UMoviePipelinePIEExecutor::StaticClass());
					if (Executor == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("render_failed"),
							TEXT("the queue subsystem returned no executor"));
						return;
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("job"), Job->JobName);
					Data->SetStringField(TEXT("sequence"), Job->Sequence.ToString());
					Data->SetStringField(TEXT("map"), Job->Map.ToString());
					Data->SetStringField(TEXT("config"), Config->GetPathName());
					Data->SetBoolField(TEXT("started"), true);
					// Where the files will land, so the caller does not have to
					// go back through the config to find them.
					if (UMoviePipelineOutputSetting* Output = Cast<UMoviePipelineOutputSetting>(
							Config->FindSettingByClass(UMoviePipelineOutputSetting::StaticClass())))
					{
						Data->SetStringField(TEXT("output_directory"), Output->OutputDirectory.Path);
						Data->SetStringField(TEXT("file_name_format"), Output->FileNameFormat);
					}
					Data->SetStringField(TEXT("message"),
						TEXT("rendering through PIE — poll render_status until `rendering` is false, ")
						TEXT("then look in output_directory ({project_dir} and the other tokens are ")
						TEXT("expanded at render time)"));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_setting_classes, create_config, ")
						TEXT("config_info, add_setting, remove_setting, save, render, or render_status"),
						*Operation));
			});
	}
}
