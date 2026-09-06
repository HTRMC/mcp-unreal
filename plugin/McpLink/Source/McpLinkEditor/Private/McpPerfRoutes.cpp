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
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "Stats/StatsCommand.h"
#include "Stats/StatsData.h"
#include "Stats/StatsSystemTypes.h"

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

#if STATS
		/// One aggregated stat as the overlay would print it: cycle stats as
		/// milliseconds with call counts, counters and memory as numbers.
		TSharedRef<FJsonObject> StatJson(const FComplexStatMessage& Stat)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Stat.GetShortName().ToString());
			const FString Description = Stat.GetDescription();
			if (!Description.IsEmpty())
			{
				Item->SetStringField(TEXT("description"), Description);
			}
			const bool bCycle = Stat.NameAndInfo.GetFlag(EStatMetaFlags::IsCycle);
			const bool bMemory = Stat.NameAndInfo.GetFlag(EStatMetaFlags::IsMemory);
			if (bCycle)
			{
				Item->SetNumberField(TEXT("inc_avg_ms"),
					FPlatformTime::ToMilliseconds(Stat.GetValue_Duration(EComplexStatField::IncAve)));
				Item->SetNumberField(TEXT("inc_max_ms"),
					FPlatformTime::ToMilliseconds(Stat.GetValue_Duration(EComplexStatField::IncMax)));
				Item->SetNumberField(TEXT("exc_avg_ms"),
					FPlatformTime::ToMilliseconds(Stat.GetValue_Duration(EComplexStatField::ExcAve)));
				Item->SetNumberField(TEXT("exc_max_ms"),
					FPlatformTime::ToMilliseconds(Stat.GetValue_Duration(EComplexStatField::ExcMax)));
				Item->SetNumberField(TEXT("calls"), Stat.GetValue_CallCount(EComplexStatField::IncAve));
			}
			else if (Stat.NameAndInfo.GetField<EStatDataType>() == EStatDataType::ST_double)
			{
				Item->SetNumberField(TEXT("avg"), Stat.GetValue_double(EComplexStatField::IncAve));
				Item->SetNumberField(TEXT("max"), Stat.GetValue_double(EComplexStatField::IncMax));
			}
			else
			{
				const double Average = static_cast<double>(Stat.GetValue_int64(EComplexStatField::IncAve));
				const double Max = static_cast<double>(Stat.GetValue_int64(EComplexStatField::IncMax));
				Item->SetNumberField(TEXT("avg"), Average);
				Item->SetNumberField(TEXT("max"), Max);
				if (bMemory)
				{
					Item->SetNumberField(TEXT("avg_mb"), Average / (1024.0 * 1024.0));
					Item->SetNumberField(TEXT("max_mb"), Max / (1024.0 * 1024.0));
				}
			}
			return Item;
		}

		/// A GPU profiler stat: the same name carries three slots — busy,
		/// wait and idle — told apart by the FName number, and the values
		/// are already milliseconds.
		TArray<TSharedPtr<FJsonValue>> GpuStatsJson(const TArray<FComplexStatMessage>& Stats, int32 MaxStats)
		{
			static const TCHAR* const Kinds[] = {TEXT("busy"), TEXT("wait"), TEXT("idle")};
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FComplexStatMessage& Stat : Stats)
			{
				if (Items.Num() >= MaxStats)
				{
					break;
				}
				FName Name = Stat.GetShortName();
				const int32 Slot = Name.GetNumber();
				Name.SetNumber(0);
				const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Name.ToString());
				const FString Description = Stat.GetDescription();
				if (!Description.IsEmpty())
				{
					Item->SetStringField(TEXT("description"), Description);
				}
				Item->SetStringField(TEXT("kind"), Slot >= 0 && Slot < 3 ? Kinds[Slot] : TEXT("unknown"));
				Item->SetNumberField(TEXT("avg_ms"), Stat.GetValue_double(EComplexStatField::IncAve));
				Item->SetNumberField(TEXT("max_ms"), Stat.GetValue_double(EComplexStatField::IncMax));
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}
			return Items;
		}

		TArray<TSharedPtr<FJsonValue>> StatsJson(const TArray<FComplexStatMessage>& Stats, int32 MaxStats)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const FComplexStatMessage& Stat : Stats)
			{
				if (Items.Num() >= MaxStats)
				{
					break;
				}
				Items.Add(MakeShared<FJsonValueObject>(StatJson(Stat)));
			}
			return Items;
		}
#endif

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

				if (Operation == TEXT("memreport"))
				{
					const bool bFull = BoolOr(Body, TEXT("full"), true);
					const int32 MaxChars = FMath::Clamp(IntOr(Body, TEXT("max_chars"), 60000), 1000, 4000000);
					const FString Dir = FPaths::ProfilingDir() / TEXT("MemReports");
					// The report lands in a dated subfolder of this folder under
					// a name taken from the *session's* start time, so a second
					// report overwrites the first: the new one is told apart by
					// its timestamp, not its name.
					TArray<FString> Existing;
					IFileManager::Get().FindFilesRecursive(Existing, *Dir, TEXT("*.memreport"), true, false);
					const TSharedRef<TMap<FString, FDateTime>> Before = MakeShared<TMap<FString, FDateTime>>();
					for (const FString& File : Existing)
					{
						Before->Add(File, IFileManager::Get().GetTimeStamp(*File));
					}

					UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;
					if (GEditor != nullptr && GEditor->PlayWorld != nullptr)
					{
						World = GEditor->PlayWorld;
					}
					// MemReport defers itself to the end of the frame (it
					// collects garbage first), then writes the whole file in
					// one go on the game thread, so the next tick that sees
					// the file sees all of it.
					GEngine->Exec(World, bFull ? TEXT("MemReport -full") : TEXT("MemReport"));

					float Elapsed = 0.0f;
					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda(
							[Responder, Before, Dir, MaxChars, bFull, Elapsed](float DeltaTime) mutable -> bool
							{
								Elapsed += DeltaTime;
								TArray<FString> Now;
								IFileManager::Get().FindFilesRecursive(Now, *Dir, TEXT("*.memreport"), true, false);
								FString NewFile;
								for (const FString& File : Now)
								{
									const FDateTime* Seen = Before->Find(File);
									if (Seen == nullptr || IFileManager::Get().GetTimeStamp(*File) != *Seen)
									{
										NewFile = File;
									}
								}
								if (NewFile.IsEmpty())
								{
									if (Elapsed < 60.0f)
									{
										return true;
									}
									Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("memreport_timeout"),
										TEXT("no .memreport appeared under Saved/Profiling/MemReports within 60s"));
									return false;
								}
								const FString Path = FPaths::ConvertRelativePathToFull(NewFile);
								FString Text;
								FFileHelper::LoadFileToString(Text, *Path);
								const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
								Data->SetStringField(TEXT("file"), Path);
								Data->SetBoolField(TEXT("full"), bFull);
								Data->SetNumberField(TEXT("chars"), Text.Len());
								Data->SetBoolField(TEXT("truncated"), Text.Len() > MaxChars);
								Data->SetStringField(TEXT("text"), Text.Len() > MaxChars ? Text.Left(MaxChars) : Text);
								Responder->Ok(Data);
								return false;
							}),
						0.0f);
					return;
				}

				if (Operation == TEXT("stat_group"))
				{
#if STATS
					FString Group;
					Body->TryGetStringField(TEXT("group"), Group);
					Group.RemoveFromStart(TEXT("STATGROUP_"), ESearchCase::IgnoreCase);
					if (Group.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'group' is required — a stat group such as GPU, SceneRendering, Game, Engine, ")
							TEXT("Memory, Physics or Niagara"));
						return;
					}
					// The registry of groups the stats system has seen, which is
					// what `stat help` lists.
					const TSet<FName>& Known = FStatGroupGameThreadNotifier::Get().StatGroupNames;
					FName GroupName = NAME_None;
					const FString Wanted = TEXT("STATGROUP_") + Group;
					for (const FName& Name : Known)
					{
						if (Name.ToString().Equals(Wanted, ESearchCase::IgnoreCase))
						{
							GroupName = Name;
						}
					}
					// `stat gpu` is not a group of that name in 5.8: the GPU
					// profiler registers one per queue (GPU0_Graphics0, ...).
					// A bare "GPU" takes the first of them.
					if (GroupName.IsNone() && Group.Equals(TEXT("GPU"), ESearchCase::IgnoreCase))
					{
						TArray<FName> GpuGroups;
						for (const FName& Name : Known)
						{
							if (Name.ToString().StartsWith(TEXT("STATGROUP_GPU"), ESearchCase::IgnoreCase))
							{
								GpuGroups.Add(Name);
							}
						}
						GpuGroups.Sort(FNameLexicalLess());
						if (!GpuGroups.IsEmpty())
						{
							GroupName = GpuGroups[0];
						}
					}
					if (GroupName.IsNone())
					{
						TArray<FString> Names;
						for (const FName& Name : Known)
						{
							FString Short = Name.ToString();
							Short.RemoveFromStart(TEXT("STATGROUP_"));
							Names.Add(Short);
						}
						Names.Sort();
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("unknown_stat_group"),
							FString::Printf(TEXT("no stat group '%s' — known groups: %s"), *Group,
								*FString::Join(Names, TEXT(", "))));
						return;
					}
					const int32 Frames = FMath::Clamp(IntOr(Body, TEXT("frames"), 30), 2, 600);
					const int32 MaxStats = FMath::Clamp(IntOr(Body, TEXT("max_stats"), 100), 1, 5000);
					const bool bKeepEnabled = BoolOr(Body, TEXT("keep_enabled"), false);
					const FGameThreadStatsData* Latest = FLatestGameThreadStatsData::Get().Latest;
					const bool bWasEnabled = Latest != nullptr && Latest->GroupNames.Contains(GroupName);
					FString ShortGroup = GroupName.ToString();
					ShortGroup.RemoveFromStart(TEXT("STATGROUP_"));
					const FString ToggleCommand = TEXT("stat ") + ShortGroup;
					if (!bWasEnabled)
					{
						// The same toggle `stat <group>` runs, straight to the
						// stats thread; no viewport is needed to gather.
						UE::Stats::DirectStatsCommand(*ToggleCommand, /*bBlockForCompletion*/ true);
					}

					int32 Seen = 0;
					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda(
							[Responder, GroupName, ShortGroup, ToggleCommand, Frames, MaxStats, bWasEnabled, bKeepEnabled, Seen](
								float /*DeltaTime*/) mutable -> bool
							{
								if (++Seen < Frames)
								{
									return true;
								}
								const FGameThreadStatsData* Data = FLatestGameThreadStatsData::Get().Latest;
								const int32 Index = Data != nullptr ? Data->GroupNames.IndexOfByKey(GroupName) : INDEX_NONE;
								if (Index == INDEX_NONE || !Data->ActiveStatGroups.IsValidIndex(Index))
								{
									if (!bWasEnabled && !bKeepEnabled)
									{
										UE::Stats::DirectStatsCommand(*ToggleCommand, true);
									}
									Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_stat_data"),
										FString::Printf(
											TEXT("no data arrived for stat group '%s' in %d frames — the stats thread ")
											TEXT("may be disabled (-nostats) or the group may gather only while rendering"),
											*ShortGroup, Frames));
									return false;
								}
								const FActiveStatGroupInfo& Info = Data->ActiveStatGroups[Index];
								const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
								Result->SetStringField(TEXT("group"), ShortGroup);
								Result->SetNumberField(TEXT("frames"), Frames);
								Result->SetArrayField(TEXT("flat"), StatsJson(Info.FlatAggregate, MaxStats));
								// The hierarchy view, with each row's depth.
								TArray<TSharedPtr<FJsonValue>> Hierarchy;
								for (int32 I = 0; I < Info.HierAggregate.Num() && I < MaxStats; ++I)
								{
									const TSharedRef<FJsonObject> Item = StatJson(Info.HierAggregate[I]);
									Item->SetNumberField(TEXT("depth"),
										Info.Indentation.IsValidIndex(I) ? Info.Indentation[I] : 0);
									Hierarchy.Add(MakeShared<FJsonValueObject>(Item));
								}
								Result->SetArrayField(TEXT("hierarchy"), Hierarchy);
								Result->SetArrayField(TEXT("counters"), StatsJson(Info.CountersAggregate, MaxStats));
								Result->SetArrayField(TEXT("memory"), StatsJson(Info.MemoryAggregate, MaxStats));
								if (!Info.GpuStatsAggregate.IsEmpty())
								{
									Result->SetArrayField(TEXT("gpu"), GpuStatsJson(Info.GpuStatsAggregate, MaxStats));
								}
								Result->SetBoolField(TEXT("left_enabled"), bWasEnabled || bKeepEnabled);
								if (!bWasEnabled && !bKeepEnabled)
								{
									UE::Stats::DirectStatsCommand(*ToggleCommand, true);
								}
								Responder->Ok(Result);
								return false;
							}),
						0.0f);
					return;
#else
					Responder->Error(EHttpServerResponseCodes::NotSupported, TEXT("stats_unavailable"),
						TEXT("this editor was built without the stats system"));
					return;
#endif
				}

				if (Operation == TEXT("csv_start"))
				{
#if CSV_PROFILER
					if (FCsvProfiler::IsCapturing())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("csv_running"),
							TEXT("a CSV capture is already running — csv_stop it first"));
						return;
					}
					const int32 Frames = IntOr(Body, TEXT("frames"), -1);
					FString Folder, File;
					Body->TryGetStringField(TEXT("folder"), Folder);
					Body->TryGetStringField(TEXT("file"), File);
					FCsvProfiler::Get()->BeginCapture(Frames, Folder, File);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("capturing"), true);
					Data->SetNumberField(TEXT("frames"), Frames);
					Data->SetStringField(TEXT("folder"),
						Folder.IsEmpty() ? FPaths::ConvertRelativePathToFull(FPaths::ProfilingDir() / TEXT("CSV")) : Folder);
					Responder->Ok(Data);
					return;
#else
					Responder->Error(EHttpServerResponseCodes::NotSupported, TEXT("csv_unavailable"),
						TEXT("this editor was built without the CSV profiler"));
					return;
#endif
				}

				if (Operation == TEXT("csv_stop"))
				{
#if CSV_PROFILER
					if (!FCsvProfiler::IsCapturing())
					{
						// A capture started with a frame count stops itself; the
						// file it wrote is still on record.
						const FString Last = FCsvProfiler::Get()->GetOutputFilename();
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("csv_not_running"),
							Last.IsEmpty()
								? FString(TEXT("no CSV capture is running — csv_start one first"))
								: FString::Printf(
									TEXT("no CSV capture is running — the last one already finished and wrote %s"),
									*FPaths::ConvertRelativePathToFull(Last)));
						return;
					}
					// The file is written asynchronously; the future carries its path.
					TSharedFuture<FString> Written = FCsvProfiler::Get()->EndCapture();
					float Elapsed = 0.0f;
					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda(
							[Responder, Written, Elapsed](float DeltaTime) mutable -> bool
							{
								Elapsed += DeltaTime;
								if (!Written.IsReady())
								{
									if (Elapsed < 60.0f)
									{
										return true;
									}
									Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("csv_timeout"),
										TEXT("the CSV file was not written within 60s"));
									return false;
								}
								const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
								Data->SetBoolField(TEXT("capturing"), false);
								Data->SetStringField(TEXT("file"), FPaths::ConvertRelativePathToFull(Written.Get()));
								Responder->Ok(Data);
								return false;
							}),
						0.0f);
					return;
#else
					Responder->Error(EHttpServerResponseCodes::NotSupported, TEXT("csv_unavailable"),
						TEXT("this editor was built without the CSV profiler"));
					return;
#endif
				}

				if (Operation == TEXT("csv_status"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
#if CSV_PROFILER
					Data->SetBoolField(TEXT("available"), true);
					Data->SetBoolField(TEXT("capturing"), FCsvProfiler::IsCapturing());
					Data->SetStringField(TEXT("output_filename"), FCsvProfiler::Get()->GetOutputFilename());
#else
					Data->SetBoolField(TEXT("available"), false);
					Data->SetBoolField(TEXT("capturing"), false);
#endif
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use frame_stats, memory, memreport, stat_group, ")
						TEXT("csv_start, csv_stop, csv_status, trace_start, trace_stop or trace_status"),
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
