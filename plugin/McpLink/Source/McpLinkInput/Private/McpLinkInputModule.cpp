// /api/input/inject — the AI's hands on the keyboard, mouse and gamepad.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "McpInputState.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Modules/ModuleManager.h"

namespace McpLink
{
	namespace
	{
		// Accepts FKey names as UE spells them ("W", "SpaceBar", "LeftMouseButton",
		// "Gamepad_LeftX"), rejecting unknown ones rather than silently no-oping.
		bool ParseKey(const FString& Name, FKey& OutKey, FString& OutError)
		{
			if (Name.IsEmpty())
			{
				OutError = TEXT("key name is empty");
				return false;
			}
			const FKey Key(*Name);
			if (!Key.IsValid())
			{
				OutError = FString::Printf(
					TEXT("unknown key '%s' — use UE key names such as W, SpaceBar, LeftShift, ")
					TEXT("LeftMouseButton, MouseWheelAxis, Gamepad_LeftX, Gamepad_FaceButton_Bottom"),
					*Name);
				return false;
			}
			OutKey = Key;
			return true;
		}

		FKey MouseButtonKey(const FString& Name)
		{
			if (Name.Equals(TEXT("right"), ESearchCase::IgnoreCase)) { return EKeys::RightMouseButton; }
			if (Name.Equals(TEXT("middle"), ESearchCase::IgnoreCase)) { return EKeys::MiddleMouseButton; }
			if (Name.Equals(TEXT("thumb1"), ESearchCase::IgnoreCase)) { return EKeys::ThumbMouseButton; }
			if (Name.Equals(TEXT("thumb2"), ESearchCase::IgnoreCase)) { return EKeys::ThumbMouseButton2; }
			return EKeys::LeftMouseButton;
		}

		double GetNumber(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, double Default)
		{
			double Value = Default;
			Body->TryGetNumberField(Field, Value);
			return Value;
		}

		// Requires a live PIE session; injection into the editor world is meaningless.
		bool RequirePie(const TSharedRef<FMcpResponder>& Responder, int32 PlayerIndex)
		{
			if (FMcpInputState::FindPlayerController(PlayerIndex) != nullptr)
			{
				return true;
			}
			Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("pie_not_running"),
				TEXT("input injection requires a running PIE session with a player controller — ")
				TEXT("start one with pie_control operation 'start'"));
			return false;
		}

		TSharedRef<FJsonObject> StateJson(const FMcpInputState& State)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			APlayerController* PC = FMcpInputState::FindPlayerController(State.GetPlayerIndex());
			Data->SetBoolField(TEXT("pie_active"), PC != nullptr);

			TArray<TSharedPtr<FJsonValue>> Keys;
			for (const auto& Pair : State.GetHeldKeys())
			{
				const TSharedRef<FJsonObject> K = MakeShared<FJsonObject>();
				K->SetStringField(TEXT("key"), Pair.Key.ToString());
				K->SetBoolField(TEXT("pressed"), Pair.Value.bPressed);
				// Ground truth from the engine, not just our intent.
				K->SetBoolField(TEXT("engine_reports_down"), PC != nullptr && PC->IsInputKeyDown(Pair.Key));
				Keys.Add(MakeShared<FJsonValueObject>(K));
			}
			Data->SetArrayField(TEXT("held_keys"), Keys);

			TArray<TSharedPtr<FJsonValue>> AxisValues;
			for (const auto& Pair : State.GetAxes())
			{
				const TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
				A->SetStringField(TEXT("axis"), Pair.Key.ToString());
				A->SetNumberField(TEXT("value"), Pair.Value.Value);
				AxisValues.Add(MakeShared<FJsonValueObject>(A));
			}
			Data->SetArrayField(TEXT("axes"), AxisValues);

			const FMouseDelta& Mouse = State.GetMouseDelta();
			const TSharedRef<FJsonObject> MouseObj = MakeShared<FJsonObject>();
			MouseObj->SetNumberField(TEXT("dx_per_frame"), Mouse.X);
			MouseObj->SetNumberField(TEXT("dy_per_frame"), Mouse.Y);
			MouseObj->SetNumberField(TEXT("frames_remaining"), Mouse.FramesRemaining);
			Data->SetObjectField(TEXT("mouse"), MouseObj);

			TArray<TSharedPtr<FJsonValue>> Actions;
			for (const FString& Name : State.GetActiveActionNames())
			{
				Actions.Add(MakeShared<FJsonValueString>(Name));
			}
			Data->SetArrayField(TEXT("injected_actions"), Actions);
			return Data;
		}
	}
}

class FMcpLinkInputModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		using namespace McpLink;

		FMcpLinkCoreModule::Get().RegisterRoute(TEXT("/api/input/inject"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FMcpInputState& State = GetInputState();
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				int32 PlayerIndex = 0;
				double PlayerIndexValue = 0.0;
				if (Body->TryGetNumberField(TEXT("player_index"), PlayerIndexValue))
				{
					PlayerIndex = static_cast<int32>(PlayerIndexValue);
				}
				State.SetPlayerIndex(PlayerIndex);

				// Always answerable, even without PIE.
				if (Operation == TEXT("get_state"))
				{
					Responder->Ok(StateJson(State));
					return;
				}
				if (Operation == TEXT("release_all"))
				{
					State.ReleaseAll();
					Responder->Ok(StateJson(State));
					return;
				}

				if (!RequirePie(Responder, PlayerIndex))
				{
					return;
				}

				const double TimeoutSeconds = GetNumber(Body, TEXT("timeout_ms"), 0.0) / 1000.0;

				if (Operation == TEXT("press_key") || Operation == TEXT("release_key")
					|| Operation == TEXT("tap_key"))
				{
					FString KeyName;
					Body->TryGetStringField(TEXT("key"), KeyName);
					FKey Key;
					FString Error;
					if (!ParseKey(KeyName, Key, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_key"), Error);
						return;
					}
					if (Operation == TEXT("press_key"))
					{
						State.PressKey(Key, TimeoutSeconds);
					}
					else if (Operation == TEXT("release_key"))
					{
						State.ReleaseKey(Key);
					}
					else
					{
						State.TapKey(Key, static_cast<int32>(GetNumber(Body, TEXT("hold_frames"), 2.0)));
					}
					Responder->Ok(StateJson(State));
					return;
				}
				if (Operation == TEXT("set_axis"))
				{
					FString AxisName;
					Body->TryGetStringField(TEXT("axis"), AxisName);
					FKey Axis;
					FString Error;
					if (!ParseKey(AxisName, Axis, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_key"), Error);
						return;
					}
					State.SetAxis(Axis, static_cast<float>(GetNumber(Body, TEXT("value"), 0.0)), TimeoutSeconds);
					Responder->Ok(StateJson(State));
					return;
				}
				if (Operation == TEXT("mouse_move"))
				{
					FString Mode;
					Body->TryGetStringField(TEXT("mode"), Mode);
					const bool bContinuous = Mode.Equals(TEXT("continuous"), ESearchCase::IgnoreCase);
					State.MoveMouse(
						GetNumber(Body, TEXT("dx"), 0.0),
						GetNumber(Body, TEXT("dy"), 0.0),
						static_cast<int32>(GetNumber(Body, TEXT("frames"), 1.0)),
						bContinuous);
					Responder->Ok(StateJson(State));
					return;
				}
				if (Operation == TEXT("mouse_wheel"))
				{
					State.SetAxis(
						EKeys::MouseWheelAxis, static_cast<float>(GetNumber(Body, TEXT("delta"), 1.0)), 0.1);
					Responder->Ok(StateJson(State));
					return;
				}
				if (Operation == TEXT("mouse_button"))
				{
					FString Button;
					Body->TryGetStringField(TEXT("button"), Button);
					FString Action;
					Body->TryGetStringField(TEXT("action"), Action);
					const FKey Key = MouseButtonKey(Button);
					if (Action.Equals(TEXT("release"), ESearchCase::IgnoreCase))
					{
						State.ReleaseKey(Key);
					}
					else if (Action.Equals(TEXT("click"), ESearchCase::IgnoreCase))
					{
						State.TapKey(Key, 2);
					}
					else
					{
						State.PressKey(Key, TimeoutSeconds);
					}
					Responder->Ok(StateJson(State));
					return;
				}
				if (Operation == TEXT("inject_action") || Operation == TEXT("release_action"))
				{
					FString ActionPath;
					Body->TryGetStringField(TEXT("action"), ActionPath);
					UInputAction* Action = Cast<UInputAction>(ResolveObject(ActionPath));
					if (Action == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("action_not_found"),
							FString::Printf(
								TEXT("no Input Action asset at '%s' — pass the asset path, e.g. /Game/Input/IA_Move"),
								*ActionPath));
						return;
					}
					if (Operation == TEXT("release_action"))
					{
						State.ReleaseAction(Action);
					}
					else
					{
						FVector Value(GetNumber(Body, TEXT("value"), 1.0), 0.0, 0.0);
						const TArray<TSharedPtr<FJsonValue>>* Vector = nullptr;
						if (Body->TryGetArrayField(TEXT("value"), Vector) && Vector != nullptr)
						{
							double X = 0.0, Y = 0.0, Z = 0.0;
							if (Vector->Num() > 0) { (*Vector)[0]->TryGetNumber(X); }
							if (Vector->Num() > 1) { (*Vector)[1]->TryGetNumber(Y); }
							if (Vector->Num() > 2) { (*Vector)[2]->TryGetNumber(Z); }
							Value = FVector(X, Y, Z);
						}
						FString Mode;
						Body->TryGetStringField(TEXT("mode"), Mode);
						const bool bHold = !Mode.Equals(TEXT("trigger"), ESearchCase::IgnoreCase);
						State.InjectAction(Action, Value, bHold, TimeoutSeconds);
					}
					Responder->Ok(StateJson(State));
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use press_key, release_key, tap_key, set_axis, ")
						TEXT("mouse_move, mouse_button, mouse_wheel, inject_action, release_action, ")
						TEXT("release_all, or get_state"),
						*Operation));
			});
	}
};

IMPLEMENT_MODULE(FMcpLinkInputModule, McpLinkInput)
