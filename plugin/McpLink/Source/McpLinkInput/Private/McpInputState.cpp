#include "McpInputState.h"

#include "Editor.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputSubsystems.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "InputAction.h"
#include "InputKeyEventArgs.h"
#include "Kismet/GameplayStatics.h"

DEFINE_LOG_CATEGORY_STATIC(LogMcpInput, Log, All);

static TAutoConsoleVariable<float> CVarInputIdleTimeout(
	TEXT("McpLink.InputIdleTimeout"),
	30.0f,
	TEXT("Seconds without an input command before McpLink releases all injected input. 0 disables the watchdog."));

namespace McpLink
{
	namespace
	{
		// Injected events must carry a device belonging to the target player,
		// or UInputSettings::bFilterInputByPlatformUser silently drops them.
		FInputDeviceId DeviceForController(const APlayerController* PC)
		{
			IPlatformInputDeviceMapper& Mapper = IPlatformInputDeviceMapper::Get();
			if (PC != nullptr)
			{
				const FInputDeviceId Device = Mapper.GetPrimaryInputDeviceForUser(PC->GetPlatformUserId());
				if (Device.IsValid())
				{
					return Device;
				}
			}
			return Mapper.GetDefaultInputDevice();
		}

		UEnhancedInputLocalPlayerSubsystem* EnhancedSubsystem(const APlayerController* PC)
		{
			if (PC == nullptr)
			{
				return nullptr;
			}
			const ULocalPlayer* LocalPlayer = PC->GetLocalPlayer();
			return LocalPlayer ? LocalPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
		}
	}

	FMcpInputState& GetInputState()
	{
		static FMcpInputState State;
		return State;
	}

	FMcpInputState::FMcpInputState()
	{
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FMcpInputState::Tick), 0.0f);
		EndPieHandle = FEditorDelegates::EndPIE.AddRaw(this, &FMcpInputState::OnEndPie);
	}

	FMcpInputState::~FMcpInputState()
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		FEditorDelegates::EndPIE.Remove(EndPieHandle);
	}

	APlayerController* FMcpInputState::FindPlayerController(int32 PlayerIndex)
	{
		if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
		{
			return nullptr;
		}
		return UGameplayStatics::GetPlayerController(GEditor->PlayWorld, PlayerIndex);
	}

	void FMcpInputState::PressKey(const FKey& Key, double TimeoutSeconds)
	{
		LastCommandTime = FPlatformTime::Seconds();
		FHeldKey& Held = HeldKeys.FindOrAdd(Key);
		Held.FramesRemaining = -1;
		Held.ExpiresAt = TimeoutSeconds > 0.0 ? LastCommandTime + TimeoutSeconds : 0.0;
	}

	void FMcpInputState::ReleaseKey(const FKey& Key)
	{
		LastCommandTime = FPlatformTime::Seconds();
		if (FHeldKey* Held = HeldKeys.Find(Key))
		{
			// Mark for release; the applier sends the event and removes it.
			Held->FramesRemaining = 0;
			Held->ExpiresAt = -1.0;
		}
	}

	void FMcpInputState::TapKey(const FKey& Key, int32 Frames)
	{
		LastCommandTime = FPlatformTime::Seconds();
		FHeldKey& Held = HeldKeys.FindOrAdd(Key);
		Held.FramesRemaining = FMath::Max(1, Frames);
		Held.ExpiresAt = 0.0;
	}

	void FMcpInputState::SetAxis(const FKey& Axis, float Value, double TimeoutSeconds)
	{
		LastCommandTime = FPlatformTime::Seconds();
		if (FMath::IsNearlyZero(Value))
		{
			Axes.Remove(Axis);
			return;
		}
		FHeldAxis& Held = Axes.FindOrAdd(Axis);
		Held.Value = Value;
		Held.ExpiresAt = TimeoutSeconds > 0.0 ? LastCommandTime + TimeoutSeconds : 0.0;
	}

	void FMcpInputState::MoveMouse(double DeltaX, double DeltaY, int32 Frames, bool bContinuous)
	{
		LastCommandTime = FPlatformTime::Seconds();
		MouseDelta.X = DeltaX;
		MouseDelta.Y = DeltaY;
		if (bContinuous)
		{
			MouseDelta.FramesRemaining = -1;
		}
		else
		{
			MouseDelta.FramesRemaining = FMath::Max(1, Frames);
			// Spread the requested total across the delivery frames.
			MouseDelta.X = DeltaX / MouseDelta.FramesRemaining;
			MouseDelta.Y = DeltaY / MouseDelta.FramesRemaining;
		}
		if (FMath::IsNearlyZero(DeltaX) && FMath::IsNearlyZero(DeltaY))
		{
			MouseDelta = FMouseDelta{};
		}
	}

	void FMcpInputState::InjectAction(UInputAction* Action, const FVector& Value, bool bHold, double TimeoutSeconds)
	{
		LastCommandTime = FPlatformTime::Seconds();
		APlayerController* PC = FindPlayerController(PlayerIndex);
		UEnhancedInputLocalPlayerSubsystem* Subsystem = EnhancedSubsystem(PC);
		if (Subsystem == nullptr || Action == nullptr)
		{
			return;
		}
		const FInputActionValue ActionValue(Value);
		if (bHold)
		{
			// The Enhanced Input module pumps continuous injections every frame.
			Subsystem->StartContinuousInputInjectionForAction(Action, ActionValue, {}, {});
			ContinuousActions.Add(
				Action, TimeoutSeconds > 0.0 ? LastCommandTime + TimeoutSeconds : 0.0);
		}
		else
		{
			Subsystem->InjectInputForAction(Action, ActionValue, {}, {});
		}
	}

	void FMcpInputState::ReleaseAction(UInputAction* Action)
	{
		LastCommandTime = FPlatformTime::Seconds();
		if (Action == nullptr)
		{
			return;
		}
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = EnhancedSubsystem(FindPlayerController(PlayerIndex)))
		{
			Subsystem->StopContinuousInputInjectionForAction(Action);
		}
		ContinuousActions.Remove(Action);
	}

	void FMcpInputState::ReleaseAll()
	{
		LastCommandTime = FPlatformTime::Seconds();
		ReleaseEverything(FindPlayerController(PlayerIndex));
	}

	TArray<FString> FMcpInputState::GetActiveActionNames() const
	{
		TArray<FString> Names;
		for (const auto& Pair : ContinuousActions)
		{
			if (const UInputAction* Action = Pair.Key.Get())
			{
				Names.Add(Action->GetPathName());
			}
		}
		return Names;
	}

	void FMcpInputState::ReleaseEverything(APlayerController* PC)
	{
		if (PC != nullptr)
		{
			const FInputDeviceId Device = DeviceForController(PC);
			FViewport* Viewport = GEditor ? GEditor->GetPIEViewport() : nullptr;
			for (const auto& Pair : HeldKeys)
			{
				if (Pair.Value.bPressed)
				{
					PC->InputKey(FInputKeyEventArgs::CreateSimulated(
						Pair.Key, IE_Released, 0.0f, /*NumSamples*/ 0, Device, false, Viewport));
				}
			}
			if (UEnhancedInputLocalPlayerSubsystem* Subsystem = EnhancedSubsystem(PC))
			{
				for (const auto& Pair : ContinuousActions)
				{
					if (UInputAction* Action = Pair.Key.Get())
					{
						Subsystem->StopContinuousInputInjectionForAction(Action);
					}
				}
			}
		}
		HeldKeys.Empty();
		Axes.Empty();
		MouseDelta = FMouseDelta{};
		ContinuousActions.Empty();
	}

	void FMcpInputState::OnEndPie(const bool /*bIsSimulating*/)
	{
		// The PIE world is going away; drop state without touching stale objects.
		HeldKeys.Empty();
		Axes.Empty();
		MouseDelta = FMouseDelta{};
		ContinuousActions.Empty();
	}

	bool FMcpInputState::Tick(float DeltaSeconds)
	{
		const bool bIdle = HeldKeys.IsEmpty() && Axes.IsEmpty()
			&& MouseDelta.FramesRemaining == 0 && ContinuousActions.IsEmpty();
		if (bIdle)
		{
			return true;
		}

		APlayerController* PC = FindPlayerController(PlayerIndex);
		if (PC == nullptr)
		{
			// PIE ended or hasn't started: drop desired state so nothing
			// resurrects when the next session begins.
			HeldKeys.Empty();
			Axes.Empty();
			MouseDelta = FMouseDelta{};
			ContinuousActions.Empty();
			return true;
		}

		const float IdleTimeout = CVarInputIdleTimeout.GetValueOnGameThread();
		if (IdleTimeout > 0.0f && FPlatformTime::Seconds() - LastCommandTime > IdleTimeout)
		{
			UE_LOG(LogMcpInput, Warning,
				TEXT("McpLink: releasing injected input after %.0fs without a command (McpLink.InputIdleTimeout)"),
				IdleTimeout);
			ReleaseEverything(PC);
			return true;
		}

		Apply(PC);
		return true;
	}

	void FMcpInputState::Apply(APlayerController* PC)
	{
		const double Now = FPlatformTime::Seconds();
		const FInputDeviceId Device = DeviceForController(PC);
		FViewport* Viewport = GEditor ? GEditor->GetPIEViewport() : nullptr;

		// --- digital keys -------------------------------------------------
		TArray<FKey> Finished;
		for (auto& Pair : HeldKeys)
		{
			const FKey& Key = Pair.Key;
			FHeldKey& Held = Pair.Value;

			const bool bExpired = (Held.ExpiresAt > 0.0 && Now >= Held.ExpiresAt)
				|| Held.ExpiresAt < 0.0
				|| Held.FramesRemaining == 0;
			if (bExpired)
			{
				if (Held.bPressed)
				{
					PC->InputKey(FInputKeyEventArgs::CreateSimulated(
						Key, IE_Released, 0.0f, /*NumSamples*/ 0, Device, false, Viewport));
				}
				Finished.Add(Key);
				continue;
			}

			// Press, or re-assert if the engine dropped the key (focus loss
			// flushes pressed keys — this heals that automatically).
			if (!Held.bPressed || !PC->IsInputKeyDown(Key))
			{
				PC->InputKey(FInputKeyEventArgs::CreateSimulated(
					Key, IE_Pressed, 1.0f, /*NumSamples*/ 0, Device, false, Viewport));
				Held.bPressed = true;
			}
			if (Held.FramesRemaining > 0)
			{
				--Held.FramesRemaining;
			}
		}
		for (const FKey& Key : Finished)
		{
			HeldKeys.Remove(Key);
		}

		// --- analog axes (accumulated and consumed each frame) ------------
		TArray<FKey> ExpiredAxes;
		for (const auto& Pair : Axes)
		{
			if (Pair.Value.ExpiresAt > 0.0 && Now >= Pair.Value.ExpiresAt)
			{
				ExpiredAxes.Add(Pair.Key);
				continue;
			}
			PC->InputKey(FInputKeyEventArgs::CreateSimulated(
				Pair.Key, IE_Axis, Pair.Value.Value, /*NumSamples*/ 1, Device, false, Viewport));
		}
		for (const FKey& Key : ExpiredAxes)
		{
			Axes.Remove(Key);
		}

		// --- mouse look ---------------------------------------------------
		if (MouseDelta.FramesRemaining != 0)
		{
			if (!FMath::IsNearlyZero(MouseDelta.X))
			{
				PC->InputKey(FInputKeyEventArgs::CreateSimulated(
					EKeys::MouseX, IE_Axis, static_cast<float>(MouseDelta.X),
					/*NumSamples*/ 1, Device, false, Viewport));
			}
			if (!FMath::IsNearlyZero(MouseDelta.Y))
			{
				PC->InputKey(FInputKeyEventArgs::CreateSimulated(
					EKeys::MouseY, IE_Axis, static_cast<float>(MouseDelta.Y),
					/*NumSamples*/ 1, Device, false, Viewport));
			}
			if (MouseDelta.FramesRemaining > 0 && --MouseDelta.FramesRemaining == 0)
			{
				MouseDelta = FMouseDelta{};
			}
		}

		// --- Enhanced Input continuous injections (engine-pumped) ---------
		TArray<TWeakObjectPtr<UInputAction>> ExpiredActions;
		for (const auto& Pair : ContinuousActions)
		{
			if (!Pair.Key.IsValid() || (Pair.Value > 0.0 && Now >= Pair.Value))
			{
				ExpiredActions.Add(Pair.Key);
			}
		}
		if (!ExpiredActions.IsEmpty())
		{
			UEnhancedInputLocalPlayerSubsystem* Subsystem = EnhancedSubsystem(PC);
			for (const TWeakObjectPtr<UInputAction>& Weak : ExpiredActions)
			{
				if (Subsystem != nullptr)
				{
					if (UInputAction* Action = Weak.Get())
					{
						Subsystem->StopContinuousInputInjectionForAction(Action);
					}
				}
				ContinuousActions.Remove(Weak);
			}
		}
	}
}
