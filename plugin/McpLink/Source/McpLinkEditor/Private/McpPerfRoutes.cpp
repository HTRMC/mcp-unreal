// Performance readback and offline builds.
//
// `stat fps` writes to the viewport, which an agent cannot read. This samples
// the frame time itself over a window of real frames and returns the numbers,
// which is the difference between "you can issue stat fps" and "you can tell
// whether the change made it slower".
//
// The build operations go through FEditorBuildUtils, the same entry point the
// editor's Build menu uses. The console commands are not an equivalent: most of
// these builds have no Exec verb at all, so issuing one silently does nothing.

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorBuildUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMemory.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "ProfilingDebugging/TraceAuxiliary.h"

namespace McpLink
{
	namespace
	{
		/// Sample FApp::GetDeltaTime() once per frame for a window, then report
		/// the distribution. Frame 0 is skipped: the tick that starts sampling
		/// carries whatever hitch the HTTP request itself caused.
		class FFrameSampler : public TSharedFromThis<FFrameSampler>
		{
		public:
			FFrameSampler(TSharedRef<FMcpResponder> InResponder, int32 InFrames)
				: Responder(MoveTemp(InResponder))
				, FramesWanted(FMath::Clamp(InFrames, 2, 600))
			{
			}

			void Start()
			{
				const TSharedRef<FFrameSampler> Self = AsShared();
				TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
					FTickerDelegate::CreateLambda(
						[Self](float /*Unused*/)
						{
							return Self->Tick();
						}),
					0.0f);
			}

		private:
			bool Tick()
			{
				if (bSkippedFirst)
				{
					Samples.Add(FApp::GetDeltaTime());
				}
				bSkippedFirst = true;
				if (Samples.Num() < FramesWanted)
				{
					return true;
				}
				FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
				Complete();
				return false;
			}

			void Complete()
			{
				double Total = 0.0;
				double Worst = 0.0;
				double Best = TNumericLimits<double>::Max();
				for (const double Sample : Samples)
				{
					Total += Sample;
					Worst = FMath::Max(Worst, Sample);
					Best = FMath::Min(Best, Sample);
				}
				const double Average = Samples.IsEmpty() ? 0.0 : Total / Samples.Num();

				TArray<double> Sorted = Samples;
				Sorted.Sort();
				const auto Percentile = [&Sorted](double Fraction)
				{
					if (Sorted.IsEmpty())
					{
						return 0.0;
					}
					const int32 Index =
						FMath::Clamp(FMath::RoundToInt(Fraction * (Sorted.Num() - 1)), 0, Sorted.Num() - 1);
					return Sorted[Index];
				};

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetNumberField(TEXT("frames"), Samples.Num());
				Data->SetNumberField(TEXT("average_ms"), Average * 1000.0);
				Data->SetNumberField(TEXT("min_ms"), Best * 1000.0);
				Data->SetNumberField(TEXT("max_ms"), Worst * 1000.0);
				// The 99th percentile is where hitching shows up; an average
				// alone hides it.
				Data->SetNumberField(TEXT("p99_ms"), Percentile(0.99) * 1000.0);
				Data->SetNumberField(TEXT("median_ms"), Percentile(0.5) * 1000.0);
				Data->SetNumberField(TEXT("average_fps"), Average > 0.0 ? 1.0 / Average : 0.0);
				Data->SetBoolField(TEXT("pie_active"),
					GEditor != nullptr && GEditor->IsPlayingSessionInEditor());
				Data->SetStringField(TEXT("note"),
					TEXT("editor frame time, which includes the editor's own UI work; measure gameplay "
						 "cost with a PIE session running"));
				Responder->Ok(Data);
			}

			TSharedRef<FMcpResponder> Responder;
			TArray<double> Samples;
			FTSTicker::FDelegateHandle TickerHandle;
			int32 FramesWanted = 60;
			bool bSkippedFirst = false;
		};

		TSharedRef<FJsonObject> MemoryJson()
		{
			const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			constexpr double ToMb = 1.0 / (1024.0 * 1024.0);
			Object->SetNumberField(TEXT("used_physical_mb"), Stats.UsedPhysical * ToMb);
			Object->SetNumberField(TEXT("peak_used_physical_mb"), Stats.PeakUsedPhysical * ToMb);
			Object->SetNumberField(TEXT("used_virtual_mb"), Stats.UsedVirtual * ToMb);
			Object->SetNumberField(TEXT("peak_used_virtual_mb"), Stats.PeakUsedVirtual * ToMb);
			Object->SetNumberField(TEXT("available_physical_mb"), Stats.AvailablePhysical * ToMb);
			return Object;
		}
	}

	void RegisterPerfRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/editor/perf"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("frame_stats"))
				{
					// Completes from the ticker once the window has elapsed.
					MakeShared<FFrameSampler>(Responder, IntOr(Body, TEXT("frames"), 60))->Start();
					return;
				}

				if (Operation == TEXT("memory"))
				{
					const TSharedRef<FJsonObject> Data = MemoryJson();
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("trace_start"))
				{
					FString Target;
					Body->TryGetStringField(TEXT("file"), Target);
					FString Channels = TEXT("default");
					Body->TryGetStringField(TEXT("channels"), Channels);
					if (FTraceAuxiliary::IsConnected())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("trace_running"),
							TEXT("a trace is already running — stop it first"));
						return;
					}
					// A null target makes Insights name the file by date/time.
					const bool bStarted = FTraceAuxiliary::Start(
						FTraceAuxiliary::EConnectionType::File,
						Target.IsEmpty() ? nullptr : *Target,
						*Channels);
					if (!bStarted)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("trace_failed"),
							TEXT("could not start the trace — the editor may have been launched with ")
							TEXT("tracing disabled"));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("tracing"), true);
					Data->SetStringField(TEXT("channels"), Channels);
					Data->SetStringField(TEXT("file"),
						Target.IsEmpty() ? TEXT("(project Saved/Profiling, named by date)") : Target);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("trace_stop"))
				{
					const bool bStopped = FTraceAuxiliary::Stop();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("stopped"), bStopped);
					Data->SetBoolField(TEXT("tracing"), FTraceAuxiliary::IsConnected());
					Data->SetStringField(TEXT("note"),
						TEXT("open the .utrace in Unreal Insights to read it"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("trace_status"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("tracing"), FTraceAuxiliary::IsConnected());
					Data->SetBoolField(TEXT("paused"), FTraceAuxiliary::IsPaused());
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use frame_stats, memory, trace_start, ")
						TEXT("trace_stop or trace_status"),
						*Operation));
			});

		Core.RegisterRoute(TEXT("/api/editor/build"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (GEditor == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_editor"),
						TEXT("no editor engine"));
					return;
				}
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (World == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_world"),
						TEXT("no editor world is loaded"));
					return;
				}

				if (Operation == TEXT("reflection_captures"))
				{
					// This one is not an FBuildOptions id: the Build menu calls
					// the editor engine directly for it.
					GEditor->BuildReflectionCaptures(World);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("built"), true);
					Data->SetStringField(TEXT("build"), TEXT("reflection_captures"));
					Responder->Ok(Data);
					return;
				}

				// FEditorBuildUtils is what the editor's Build menu calls, so
				// each of these is exactly the menu item of the same name. The
				// console commands are not equivalent: most of them do not exist
				// as Exec verbs at all.
				static const TMap<FString, FName> Builds = {
					{TEXT("lighting"), FBuildOptions::BuildLighting},
					{TEXT("navigation"), FBuildOptions::BuildAIPaths},
					{TEXT("geometry"), FBuildOptions::BuildGeometry},
					{TEXT("visible_geometry"), FBuildOptions::BuildVisibleGeometry},
					{TEXT("hlod"), FBuildOptions::BuildHierarchicalLOD},
					{TEXT("texture_streaming"), FBuildOptions::BuildTextureStreaming},
					{TEXT("virtual_texture"), FBuildOptions::BuildVirtualTexture},
					{TEXT("landscape"), FBuildOptions::BuildAllLandscape},
					{TEXT("all"), FBuildOptions::BuildAll},
				};
				const FName* BuildId = Builds.Find(Operation.ToLower());
				if (BuildId == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
						FString::Printf(
							TEXT("unknown operation '%s' — use lighting, navigation, geometry, ")
							TEXT("visible_geometry, hlod, reflection_captures, texture_streaming, ")
							TEXT("virtual_texture, ")
							TEXT("landscape or all"),
							*Operation));
					return;
				}

				FString Quality;
				if (Body->TryGetStringField(TEXT("quality"), Quality) && !Quality.IsEmpty())
				{
					// EditorBuild reads the lighting quality back out of
					// GEditorPerProjectIni, so that is where it has to be set.
					static const TMap<FString, int32> Qualities = {
						{TEXT("preview"), 0}, {TEXT("medium"), 1},
						{TEXT("high"), 2}, {TEXT("production"), 3},
					};
					const int32* Level = Qualities.Find(Quality.ToLower());
					if (Level == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_quality"),
							TEXT("quality must be Preview, Medium, High or Production"));
						return;
					}
					GConfig->SetInt(TEXT("LightingBuildOptions"), TEXT("QualityLevel"),
						*Level, GEditorPerProjectIni);
				}

				// EditorBuild blocks the game thread until the build finishes,
				// so this response is the completion signal — except for
				// lighting, which hands off to a Lightmass process.
				const bool bBuilt = FEditorBuildUtils::EditorBuild(
					World, *BuildId, /*bAllowLightingDialog*/ false);

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetBoolField(TEXT("built"), bBuilt);
				Data->SetStringField(TEXT("build"), Operation);
				if (Operation.Equals(TEXT("lighting"), ESearchCase::IgnoreCase))
				{
					Data->SetStringField(TEXT("note"),
						TEXT("Lightmass runs out of process — poll get_log for 'Lighting build ")
						TEXT("completed'. A -nullrhi editor cannot build lighting at all"));
				}
				else if (!bBuilt)
				{
					Data->SetStringField(TEXT("note"),
						TEXT("the build reported failure — get_log has the reason"));
				}
				Responder->Ok(Data);
			});
	}
}
