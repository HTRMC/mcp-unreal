// The automation flags every McpLink test uses.
//
// One `inline constexpr` in a shared header rather than a copy per file: an
// adaptive-unity build merges several test .cpp files into one translation
// unit, and two anonymous-namespace constants of the same name in one TU is a
// redefinition error. Which files get merged depends on which ones were edited
// recently, so the copy-per-file version broke unpredictably.
#pragma once

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

inline constexpr EAutomationTestFlags McpTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

#endif
