// Desired-state transitions of the input injector. These run without PIE, so
// they cover the bookkeeping the applier relies on rather than the engine
// injection itself (that is verified end to end through the MCP tools).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "InputCoreTypes.h"
#include "McpInputState.h"
#include "McpTestFlags.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpInputKeysTest, "McpLink.Input.State.Keys", McpTestFlags)
bool FMcpInputKeysTest::RunTest(const FString& Parameters)
{
	McpLink::FMcpInputState State;

	State.PressKey(EKeys::W, 0.0);
	const McpLink::FHeldKey* Held = State.GetHeldKeys().Find(EKeys::W);
	if (TestNotNull(TEXT("pressed key is tracked"), Held))
	{
		TestEqual(TEXT("hold has no frame limit"), Held->FramesRemaining, -1);
		TestEqual(TEXT("no timeout when 0 passed"), Held->ExpiresAt, 0.0);
		TestFalse(TEXT("not yet applied to the engine"), Held->bPressed);
	}

	State.PressKey(EKeys::A, 2.5);
	const McpLink::FHeldKey* Timed = State.GetHeldKeys().Find(EKeys::A);
	if (TestNotNull(TEXT("timed key is tracked"), Timed))
	{
		TestTrue(TEXT("timeout is in the future"), Timed->ExpiresAt > FPlatformTime::Seconds());
	}

	State.TapKey(EKeys::SpaceBar, 3);
	const McpLink::FHeldKey* Tap = State.GetHeldKeys().Find(EKeys::SpaceBar);
	if (TestNotNull(TEXT("tap is tracked"), Tap))
	{
		TestEqual(TEXT("tap holds for the requested frames"), Tap->FramesRemaining, 3);
	}
	State.TapKey(EKeys::E, 0);
	if (const McpLink::FHeldKey* Clamped = State.GetHeldKeys().Find(EKeys::E))
	{
		TestEqual(TEXT("tap of 0 frames is clamped to 1"), Clamped->FramesRemaining, 1);
	}

	// Release marks the entry for the applier to send IE_Released and drop.
	State.ReleaseKey(EKeys::W);
	const McpLink::FHeldKey* Released = State.GetHeldKeys().Find(EKeys::W);
	if (TestNotNull(TEXT("released key stays until the applier runs"), Released))
	{
		TestEqual(TEXT("release zeroes frames"), Released->FramesRemaining, 0);
		TestTrue(TEXT("release flags expiry"), Released->ExpiresAt < 0.0);
	}
	// Releasing something never pressed is a no-op, not a crash.
	State.ReleaseKey(EKeys::Z);
	TestNull(TEXT("unknown release adds nothing"), State.GetHeldKeys().Find(EKeys::Z));

	State.ReleaseAll();
	TestEqual(TEXT("release_all clears keys"), State.GetHeldKeys().Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpInputAxesTest, "McpLink.Input.State.AxesAndMouse", McpTestFlags)
bool FMcpInputAxesTest::RunTest(const FString& Parameters)
{
	McpLink::FMcpInputState State;

	State.SetAxis(EKeys::Gamepad_LeftX, 0.75f, 0.0);
	const McpLink::FHeldAxis* Axis = State.GetAxes().Find(EKeys::Gamepad_LeftX);
	if (TestNotNull(TEXT("axis is tracked"), Axis))
	{
		TestEqual(TEXT("axis value"), Axis->Value, 0.75f);
	}
	// Zero clears rather than holding a dead axis.
	State.SetAxis(EKeys::Gamepad_LeftX, 0.0f, 0.0);
	TestNull(TEXT("zero clears the axis"), State.GetAxes().Find(EKeys::Gamepad_LeftX));

	// A one-shot move is spread evenly across the requested frames.
	State.MoveMouse(100.0, -40.0, 4, /*bContinuous*/ false);
	const McpLink::FMouseDelta& Once = State.GetMouseDelta();
	TestEqual(TEXT("dx per frame"), Once.X, 25.0);
	TestEqual(TEXT("dy per frame"), Once.Y, -10.0);
	TestEqual(TEXT("frames to deliver"), Once.FramesRemaining, 4);

	// Continuous keeps the raw per-frame delta until cleared.
	State.MoveMouse(3.0, 0.0, 1, /*bContinuous*/ true);
	const McpLink::FMouseDelta& Continuous = State.GetMouseDelta();
	TestEqual(TEXT("continuous dx"), Continuous.X, 3.0);
	TestEqual(TEXT("continuous never runs out"), Continuous.FramesRemaining, -1);

	State.MoveMouse(0.0, 0.0, 1, true);
	TestEqual(TEXT("zero delta clears mouse"), State.GetMouseDelta().FramesRemaining, 0);

	State.SetAxis(EKeys::Gamepad_RightY, -1.0f, 0.0);
	State.ReleaseAll();
	TestEqual(TEXT("release_all clears axes"), State.GetAxes().Num(), 0);
	TestEqual(TEXT("release_all leaves no actions"), State.GetActiveActionNames().Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
