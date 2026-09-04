// The Visual Logger, read back as data instead of drawn on a timeline.
//
// UE_VLOG and friends are how engine AI, navigation and movement explain
// themselves: per-actor, per-frame entries carrying log lines, named status
// blocks and the shapes the Visual Logger window draws. Normally the only way
// to see them is that window, or a .bvlog file opened in it.
//
// FVisualLogger dispatches every entry to its registered FVisualLogDevices, so
// a device that keeps entries in memory turns the whole stream into something
// queryable. That is all this is: one device, a bounded ring of entries, and
// filters over it. The entries are the engine's own — nothing here logs
// anything, it only listens.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResponder.h"
#include "Misc/ScopeLock.h"
#include "VisualLogger/VisualLogger.h"
#include "VisualLogger/VisualLoggerTypes.h"

namespace McpLink
{
	namespace VisualLog
	{
#if UE_DEBUG_RECORDING_USING_VLOG

		/// Keeps the most recent entries in memory. Serialize can be called from
		/// whichever thread logged, so everything goes through the lock.
		class FMcpVisualLogDevice : public FVisualLogDevice
		{
		public:
			/// A busy AI scene logs a few entries per actor per frame, so this
			/// is a few seconds of a handful of actors — enough to answer "what
			/// just happened", which is what an agent asks.
			static constexpr int32 MaxEntries = 4000;

			virtual void Serialize(const UObject* LogOwner, const FName& InOwnerName,
				const FName& InOwnerDisplayName, const FName& InOwnerClassName,
				const FVisualLogEntry& InLogEntry) override
			{
				FScopeLock Lock(&CriticalSection);
				if (Entries.Num() >= MaxEntries)
				{
					Entries.RemoveAt(0, Entries.Num() - MaxEntries + 1, EAllowShrinking::No);
					++Dropped;
				}
				Entries.Emplace(InOwnerName, InOwnerDisplayName, InOwnerClassName, InLogEntry);
			}

			virtual void Cleanup(bool bReleaseMemory = false) override
			{
				FScopeLock Lock(&CriticalSection);
				Entries.Reset();
			}

			virtual void GetRecordedLogs(TArray<FVisualLogEntryItem>& OutLogs) const override
			{
				FScopeLock Lock(&CriticalSection);
				OutLogs = Entries;
			}

			void Reset()
			{
				FScopeLock Lock(&CriticalSection);
				Entries.Reset();
				Dropped = 0;
			}

			int32 GetDropped() const
			{
				FScopeLock Lock(&CriticalSection);
				return Dropped;
			}

		private:
			mutable FCriticalSection CriticalSection;
			TArray<FVisualLogEntryItem> Entries;
			int32 Dropped = 0;
		};

		FMcpVisualLogDevice& Device()
		{
			static FMcpVisualLogDevice Instance;
			return Instance;
		}

		bool& DeviceAttached()
		{
			static bool bAttached = false;
			return bAttached;
		}

		/// A single VLOG line can be enormous — Mass logs its whole processor
		/// graph in one — so a line is cut here rather than blowing the caller's
		/// budget on one entry.
		constexpr int32 MaxLineChars = 1200;

		FString ClampLine(const FString& Line, bool& bOutTruncated)
		{
			if (Line.Len() <= MaxLineChars)
			{
				return Line;
			}
			bOutTruncated = true;
			return Line.Left(MaxLineChars)
				+ FString::Printf(TEXT("… [+%d chars]"), Line.Len() - MaxLineChars);
		}

		const TCHAR* ShapeTypeName(EVisualLoggerShapeElement Type)
		{
			switch (Type)
			{
			case EVisualLoggerShapeElement::SinglePoint: return TEXT("point");
			case EVisualLoggerShapeElement::Sphere: return TEXT("sphere");
			case EVisualLoggerShapeElement::Segment: return TEXT("segment");
			case EVisualLoggerShapeElement::Path: return TEXT("path");
			case EVisualLoggerShapeElement::Box: return TEXT("box");
			case EVisualLoggerShapeElement::Cone: return TEXT("cone");
			case EVisualLoggerShapeElement::Cylinder: return TEXT("cylinder");
			case EVisualLoggerShapeElement::Capsule: return TEXT("capsule");
			case EVisualLoggerShapeElement::Polygon: return TEXT("polygon");
			case EVisualLoggerShapeElement::Mesh: return TEXT("mesh");
			case EVisualLoggerShapeElement::NavAreaMesh: return TEXT("nav_area_mesh");
			case EVisualLoggerShapeElement::Arrow: return TEXT("arrow");
			case EVisualLoggerShapeElement::Circle: return TEXT("circle");
			case EVisualLoggerShapeElement::CoordinateSystem: return TEXT("coordinate_system");
			default: return TEXT("invalid");
			}
		}

		TSharedRef<FJsonValue> VectorToJson(const FVector& Vector)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Add(MakeShared<FJsonValueNumber>(Vector.X));
			Values.Add(MakeShared<FJsonValueNumber>(Vector.Y));
			Values.Add(MakeShared<FJsonValueNumber>(Vector.Z));
			return MakeShared<FJsonValueArray>(Values);
		}

		/// Status blocks nest, and the nesting is how a Behavior Tree or a
		/// movement component groups what it is reporting.
		TSharedRef<FJsonObject> StatusToJson(const FVisualLogStatusCategory& Status)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("category"), Status.Category);
			TArray<TSharedPtr<FJsonValue>> Lines;
			for (const FString& Line : Status.Data)
			{
				Lines.Add(MakeShared<FJsonValueString>(Line));
			}
			Object->SetArrayField(TEXT("data"), Lines);
			if (!Status.Children.IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> Children;
				for (const FVisualLogStatusCategory& Child : Status.Children)
				{
					Children.Add(MakeShared<FJsonValueObject>(StatusToJson(Child)));
				}
				Object->SetArrayField(TEXT("children"), Children);
			}
			return Object;
		}

		/// bOutMatchedCategory says whether anything survived the category
		/// filter, so an entry filtered down to nothing can be dropped rather
		/// than returned empty.
		TSharedRef<FJsonObject> EntryToJson(const FVisualLogDevice::FVisualLogEntryItem& Item,
			const FString& CategoryFilter, bool& bOutMatchedCategory)
		{
			bOutMatchedCategory = false;
			const FVisualLogEntry& Entry = Item.Entry;
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("owner"), Item.OwnerName.ToString());
			Object->SetStringField(TEXT("display_name"), Item.OwnerDisplayName.ToString());
			Object->SetStringField(TEXT("class"), Item.OwnerClassName.ToString());
			Object->SetNumberField(TEXT("time"), Entry.TimeStamp);
			Object->SetField(TEXT("location"), VectorToJson(Entry.Location));

			auto Matches = [&CategoryFilter](const FName& Category)
			{
				return CategoryFilter.IsEmpty()
					|| Category.ToString().Contains(CategoryFilter);
			};

			TArray<TSharedPtr<FJsonValue>> Lines;
			for (const FVisualLogLine& Line : Entry.LogLines)
			{
				if (!Matches(Line.Category))
				{
					continue;
				}
				const TSharedRef<FJsonObject> Item2 = MakeShared<FJsonObject>();
				Item2->SetStringField(TEXT("category"), Line.Category.ToString());
				Item2->SetStringField(TEXT("verbosity"),
					::ToString(static_cast<ELogVerbosity::Type>(Line.Verbosity)));
				bool bTruncated = false;
				Item2->SetStringField(TEXT("text"), ClampLine(Line.Line, bTruncated));
				if (bTruncated)
				{
					Item2->SetBoolField(TEXT("truncated"), true);
				}
				Lines.Add(MakeShared<FJsonValueObject>(Item2));
				bOutMatchedCategory = true;
			}
			Object->SetArrayField(TEXT("log_lines"), Lines);

			TArray<TSharedPtr<FJsonValue>> Shapes;
			for (const FVisualLogShapeElement& Shape : Entry.ElementsToDraw)
			{
				if (!Matches(Shape.Category))
				{
					continue;
				}
				const TSharedRef<FJsonObject> Item2 = MakeShared<FJsonObject>();
				Item2->SetStringField(TEXT("type"), ShapeTypeName(Shape.GetType()));
				Item2->SetStringField(TEXT("category"), Shape.Category.ToString());
				Item2->SetStringField(TEXT("description"), Shape.Description);
				// A path or polygon can carry hundreds of points; the shape is
				// identified by its ends and its size, which is what a caller
				// checks against.
				Item2->SetNumberField(TEXT("point_count"), Shape.Points.Num());
				TArray<TSharedPtr<FJsonValue>> Points;
				for (int32 Index = 0; Index < FMath::Min(Shape.Points.Num(), 8); ++Index)
				{
					Points.Add(VectorToJson(Shape.Points[Index]));
				}
				Item2->SetArrayField(TEXT("points"), Points);
				Shapes.Add(MakeShared<FJsonValueObject>(Item2));
				bOutMatchedCategory = true;
			}
			Object->SetArrayField(TEXT("shapes"), Shapes);

			TArray<TSharedPtr<FJsonValue>> Statuses;
			for (const FVisualLogStatusCategory& Status : Entry.Status)
			{
				Statuses.Add(MakeShared<FJsonValueObject>(StatusToJson(Status)));
			}
			Object->SetArrayField(TEXT("status"), Statuses);

			TArray<TSharedPtr<FJsonValue>> Events;
			for (const FVisualLogEvent& Event : Entry.Events)
			{
				const TSharedRef<FJsonObject> Item2 = MakeShared<FJsonObject>();
				Item2->SetStringField(TEXT("name"), Event.Name);
				Item2->SetStringField(TEXT("description"), Event.UserFriendlyDesc);
				Item2->SetNumberField(TEXT("counter"), Event.Counter);
				Events.Add(MakeShared<FJsonValueObject>(Item2));
			}
			Object->SetArrayField(TEXT("events"), Events);
			return Object;
		}

		void AddStatusFields(const TSharedRef<FJsonObject>& Data)
		{
			TArray<FVisualLogDevice::FVisualLogEntryItem> Entries;
			Device().GetRecordedLogs(Entries);
			Data->SetBoolField(TEXT("recording"), FVisualLogger::IsRecording());
			Data->SetBoolField(TEXT("listening"), DeviceAttached());
			Data->SetNumberField(TEXT("entry_count"), Entries.Num());
			Data->SetNumberField(TEXT("dropped"), Device().GetDropped());
			Data->SetNumberField(TEXT("capacity"), FMcpVisualLogDevice::MaxEntries);
			if (!Entries.IsEmpty())
			{
				Data->SetNumberField(TEXT("first_time"), Entries[0].Entry.TimeStamp);
				Data->SetNumberField(TEXT("last_time"), Entries.Last().Entry.TimeStamp);
			}
		}

#endif // UE_DEBUG_RECORDING_USING_VLOG
	}

	using namespace VisualLog;

	void RegisterVisualLogRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/debug/visual_log"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
#if !UE_DEBUG_RECORDING_USING_VLOG
				Responder->Error(EHttpServerResponseCodes::NotSupported, TEXT("vlog_compiled_out"),
					TEXT("this build has visual logging compiled out (UE_DEBUG_RECORDING_ENABLED ")
					TEXT("is off, or this is a Shipping/Test build), so there is nothing to record"));
#else
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("start"))
				{
					if (!DeviceAttached())
					{
						FVisualLogger::Get().AddDevice(&Device());
						DeviceAttached() = true;
					}
					if (BoolOr(Body, TEXT("clear"), true))
					{
						Device().Reset();
					}
					FVisualLogger::Get().SetIsRecording(true);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddStatusFields(Data);
					Data->SetStringField(TEXT("message"),
						TEXT("recording — run PIE, then read what the engine logged with entries. ")
						TEXT("Nothing is recorded outside a play session, because that is when AI, ")
						TEXT("navigation and movement log"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("stop"))
				{
					FVisualLogger::Get().SetIsRecording(false);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddStatusFields(Data);
					Data->SetStringField(TEXT("message"),
						TEXT("stopped — the entries stay readable until clear or another start"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("clear"))
				{
					Device().Reset();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddStatusFields(Data);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("status"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddStatusFields(Data);
					Responder->Ok(Data);
					return;
				}

				TArray<FVisualLogDevice::FVisualLogEntryItem> Entries;
				Device().GetRecordedLogs(Entries);

				if (Operation == TEXT("owners"))
				{
					TMap<FName, int32> Counts;
					TMap<FName, FString> Classes;
					for (const FVisualLogDevice::FVisualLogEntryItem& Item : Entries)
					{
						++Counts.FindOrAdd(Item.OwnerName);
						Classes.FindOrAdd(Item.OwnerName) = Item.OwnerClassName.ToString();
					}
					TArray<TSharedPtr<FJsonValue>> Owners;
					for (const TPair<FName, int32>& Pair : Counts)
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("owner"), Pair.Key.ToString());
						Item->SetStringField(TEXT("class"), Classes[Pair.Key]);
						Item->SetNumberField(TEXT("entries"), Pair.Value);
						Owners.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					AddStatusFields(Data);
					Data->SetArrayField(TEXT("owners"), Owners);
					if (Owners.IsEmpty())
					{
						Data->SetStringField(TEXT("message"),
							TEXT("nothing logged yet — start recording before the play session, and ")
							TEXT("remember only code that calls UE_VLOG shows up here"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation != TEXT("entries"))
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
						FString::Printf(
							TEXT("unknown operation '%s' — use start, stop, status, owners, ")
							TEXT("entries or clear"),
							*Operation));
					return;
				}

				FString OwnerFilter, CategoryFilter;
				Body->TryGetStringField(TEXT("owner"), OwnerFilter);
				Body->TryGetStringField(TEXT("category"), CategoryFilter);
				const double Since = DoubleOr(Body, TEXT("since_time"), -1.0);
				const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_results"), 50), 1, 500);

				TArray<TSharedPtr<FJsonValue>> Items;
				int32 Matched = 0;
				// Newest first: what just happened is what is being asked about.
				for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
				{
					const FVisualLogDevice::FVisualLogEntryItem& Item = Entries[Index];
					if (!OwnerFilter.IsEmpty()
						&& !Item.OwnerName.ToString().Contains(OwnerFilter)
						&& !Item.OwnerDisplayName.ToString().Contains(OwnerFilter))
					{
						continue;
					}
					if (Item.Entry.TimeStamp < Since)
					{
						continue;
					}
					// Build it first: a category filter is only meaningful once
					// the entry's own lines and shapes have been tested against
					// it, and an entry filtered down to nothing is not a match.
					bool bMatchedCategory = false;
					const TSharedRef<FJsonObject> Json =
						EntryToJson(Item, CategoryFilter, bMatchedCategory);
					if (!CategoryFilter.IsEmpty() && !bMatchedCategory)
					{
						continue;
					}
					++Matched;
					if (Items.Num() < Max)
					{
						Items.Add(MakeShared<FJsonValueObject>(Json));
					}
				}

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				AddStatusFields(Data);
				Data->SetNumberField(TEXT("matched"), Matched);
				Data->SetNumberField(TEXT("returned"), Items.Num());
				Data->SetArrayField(TEXT("entries"), Items);
				Responder->Ok(Data);
#endif // UE_DEBUG_RECORDING_USING_VLOG
			});
	}
}
