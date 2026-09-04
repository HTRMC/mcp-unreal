// Desired-input-state manager.
//
// HTTP handlers only ever mutate the desired state (on the game thread); a
// per-frame applier synthesizes the actual engine input events. This matches
// how UPlayerInput consumes input:
//   * digital keys latch down until an explicit release, so we press once and
//     re-assert only if the engine dropped the key (e.g. viewport focus loss
//     flushes pressed keys);
//   * analog axes are accumulated and consumed every frame, so held axis
//     values and mouse deltas must be re-injected each frame.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "InputCoreTypes.h"

class APlayerController;
class UInputAction;

namespace McpLink
{
	struct FHeldKey
	{
		// Absolute time (FPlatformTime::Seconds) after which the key auto-releases.
		double ExpiresAt = 0.0;
		// Frames remaining for a tap; -1 means "hold until released or expired".
		int32 FramesRemaining = -1;
		bool bPressed = false;
	};

	struct FHeldAxis
	{
		float Value = 0.0f;
		double ExpiresAt = 0.0;
	};

	struct FMouseDelta
	{
		double X = 0.0;
		double Y = 0.0;
		// Frames left to deliver (0 = inactive, <0 = continuous until cleared).
		int32 FramesRemaining = 0;
	};

	class FMcpInputState
	{
	public:
		FMcpInputState();
		~FMcpInputState();

		void PressKey(const FKey& Key, double TimeoutSeconds);
		void ReleaseKey(const FKey& Key);
		void TapKey(const FKey& Key, int32 Frames);
		void SetAxis(const FKey& Axis, float Value, double TimeoutSeconds);
		void MoveMouse(double DeltaX, double DeltaY, int32 Frames, bool bContinuous);
		void InjectAction(UInputAction* Action, const FVector& Value, bool bHold, double TimeoutSeconds);
		void ReleaseAction(UInputAction* Action);
		void ReleaseAll();

		void SetPlayerIndex(int32 InPlayerIndex) { PlayerIndex = InPlayerIndex; }
		int32 GetPlayerIndex() const { return PlayerIndex; }

		const TMap<FKey, FHeldKey>& GetHeldKeys() const { return HeldKeys; }
		const TMap<FKey, FHeldAxis>& GetAxes() const { return Axes; }
		const FMouseDelta& GetMouseDelta() const { return MouseDelta; }
		TArray<FString> GetActiveActionNames() const;

		// True when a PIE player controller is available to inject into.
		static APlayerController* FindPlayerController(int32 PlayerIndex);

	private:
		// Called every frame on the game thread; applies desired state.
		bool Tick(float DeltaSeconds);
		void Apply(APlayerController* PC);
		void ReleaseEverything(APlayerController* PC);
		void OnEndPie(const bool bIsSimulating);

		TMap<FKey, FHeldKey> HeldKeys;
		TMap<FKey, FHeldAxis> Axes;
		FMouseDelta MouseDelta;
		TMap<TWeakObjectPtr<UInputAction>, double> ContinuousActions;

		int32 PlayerIndex = 0;
		// Global watchdog: everything releases if no command arrives for a while.
		double LastCommandTime = 0.0;

		FTSTicker::FDelegateHandle TickHandle;
		FDelegateHandle EndPieHandle;
	};

	// Process-wide instance (the editor hosts one injector).
	FMcpInputState& GetInputState();
}
