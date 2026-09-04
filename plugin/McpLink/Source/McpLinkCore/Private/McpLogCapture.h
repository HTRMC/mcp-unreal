// Thread-safe ring buffer capturing the global output log, with sequence
// numbers so clients can poll incrementally.
#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

namespace McpLink
{
	struct FMcpLogEntry
	{
		uint64 Seq = 0;
		double TimeSeconds = 0.0;
		FName Category;
		ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
		FString Message;
	};

	class FMcpLogCapture final : public FOutputDevice
	{
	public:
		FMcpLogCapture();
		virtual ~FMcpLogCapture() override;

		// FOutputDevice — called from arbitrary threads.
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		// Entries with Seq > SinceSeq (oldest first), newest MaxCount kept.
		// MinVerbosity: include entries at least this severe (e.g. Warning
		// includes Warning/Error/Fatal). NAME_None category matches all.
		void Get(
			uint64 SinceSeq,
			int32 MaxCount,
			FName CategoryFilter,
			ELogVerbosity::Type MinVerbosity,
			TArray<FMcpLogEntry>& OutEntries,
			uint64& OutLastSeq) const;

	private:
		static constexpr int32 Capacity = 10000;

		mutable FCriticalSection Mutex;
		TArray<FMcpLogEntry> Ring;
		int32 Next = 0; // insertion index once the ring is full
		uint64 NextSeq = 1;
	};
}
