#include "McpLogCapture.h"

#include "HAL/PlatformTime.h"
#include "Misc/OutputDeviceRedirector.h"

namespace McpLink
{
	FMcpLogCapture::FMcpLogCapture()
	{
		Ring.Reserve(Capacity);
		if (GLog)
		{
			GLog->AddOutputDevice(this);
		}
	}

	FMcpLogCapture::~FMcpLogCapture()
	{
		// Explicit removal — the reference plugin leaked its device into GLog
		// across hot reloads, which crashes during iterative development.
		if (GLog)
		{
			GLog->RemoveOutputDevice(this);
		}
	}

	void FMcpLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
	{
		if (Verbosity == ELogVerbosity::SetColor)
		{
			return;
		}
		FMcpLogEntry Entry;
		Entry.TimeSeconds = FPlatformTime::Seconds();
		Entry.Category = Category;
		Entry.Verbosity = Verbosity;
		Entry.Message = V;

		FScopeLock Lock(&Mutex);
		Entry.Seq = NextSeq++;
		if (Ring.Num() < Capacity)
		{
			Ring.Add(MoveTemp(Entry));
		}
		else
		{
			Ring[Next] = MoveTemp(Entry);
			Next = (Next + 1) % Capacity;
		}
	}

	void FMcpLogCapture::Get(
		uint64 SinceSeq,
		int32 MaxCount,
		FName CategoryFilter,
		ELogVerbosity::Type MinVerbosity,
		TArray<FMcpLogEntry>& OutEntries,
		uint64& OutLastSeq) const
	{
		FScopeLock Lock(&Mutex);
		OutLastSeq = NextSeq - 1;

		// Collect oldest→newest: ring order starts at Next when full.
		const int32 Count = Ring.Num();
		const int32 Start = (Count < Capacity) ? 0 : Next;
		TArray<const FMcpLogEntry*> Matching;
		for (int32 i = 0; i < Count; ++i)
		{
			const FMcpLogEntry& Entry = Ring[(Start + i) % Count];
			if (Entry.Seq <= SinceSeq)
			{
				continue;
			}
			// Lower ELogVerbosity values are more severe.
			if (Entry.Verbosity > MinVerbosity)
			{
				continue;
			}
			if (!CategoryFilter.IsNone() && Entry.Category != CategoryFilter)
			{
				continue;
			}
			Matching.Add(&Entry);
		}
		const int32 First = FMath::Max(0, Matching.Num() - MaxCount);
		OutEntries.Reserve(Matching.Num() - First);
		for (int32 i = First; i < Matching.Num(); ++i)
		{
			OutEntries.Add(*Matching[i]);
		}
	}
}
