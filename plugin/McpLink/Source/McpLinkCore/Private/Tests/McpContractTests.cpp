// The plugin half of tests/fixtures/contract/: every fixture was captured from
// a real exchange with this plugin, so the routes it names must still exist and
// the envelopes it recorded must still be what FMcpResponder produces.
//
// The Rust server checks the same files from the other side
// (tests/contract_tests.rs), so a rename on either half breaks a test instead
// of breaking an agent at runtime.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HttpResultCallback.h"
#include "HttpServerResponse.h"
#include "Interfaces/IPluginManager.h"
#include "McpLinkCoreModule.h"
#include "McpLogCapture.h"
#include "McpResponder.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr EAutomationTestFlags McpTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	const TCHAR* ContractSubPath = TEXT("tests/fixtures/contract");

	/// Locate tests/fixtures/contract: the McpLink.ContractFixtures CVar wins,
	/// otherwise walk up from the plugin and the project (the plugin is
	/// junctioned into test-project/Plugins in this repo).
	FString FindContractDir()
	{
		if (const IConsoleVariable* CVar =
				IConsoleManager::Get().FindConsoleVariable(TEXT("McpLink.ContractFixtures")))
		{
			const FString Configured = CVar->GetString();
			if (!Configured.IsEmpty() && IFileManager::Get().DirectoryExists(*Configured))
			{
				return Configured;
			}
		}

		TArray<FString> Roots;
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("McpLink")))
		{
			Roots.Add(Plugin->GetBaseDir());
		}
		Roots.Add(FPaths::ProjectDir());

		for (const FString& Root : Roots)
		{
			FString Dir = FPaths::ConvertRelativePathToFull(Root);
			for (int32 Depth = 0; Depth < 6 && !Dir.IsEmpty(); ++Depth)
			{
				const FString Candidate = FPaths::Combine(Dir, ContractSubPath);
				if (IFileManager::Get().DirectoryExists(*Candidate))
				{
					return Candidate;
				}
				const FString Parent = FPaths::GetPath(Dir);
				if (Parent == Dir)
				{
					break;
				}
				Dir = Parent;
			}
		}
		return FString();
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> Parsed;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid() ? Parsed : nullptr;
	}

	/// The body bytes a responder handed back, parsed. The buffer is not
	/// null-terminated and the messages carry non-ASCII characters, so it must be
	/// terminated before the UTF-8 conversion rather than measured in bytes.
	TSharedPtr<FJsonObject> ResponseBody(const FHttpServerResponse& Response)
	{
		TArray<uint8> Bytes = Response.Body;
		Bytes.Add(0);
		return ParseObject(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData())));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpContractFixturesTest, "McpLink.Core.Contract.Fixtures", McpTestFlags)
bool FMcpContractFixturesTest::RunTest(const FString& Parameters)
{
	const FString Dir = FindContractDir();
	if (!TestFalse(
			TEXT("contract fixture directory found (set McpLink.ContractFixtures to override)"),
			Dir.IsEmpty()))
	{
		return false;
	}

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, TEXT("*.json")), true, false);
	if (!TestTrue(FString::Printf(TEXT("fixtures present in %s"), *Dir), Files.Num() > 0))
	{
		return false;
	}

	const TConstArrayView<FString> Routes = FMcpLinkCoreModule::Get().GetRoutePaths();

	for (const FString& File : Files)
	{
		const FString Path = FPaths::Combine(Dir, File);
		FString Text;
		if (!TestTrue(FString::Printf(TEXT("%s reads"), *File), FFileHelper::LoadFileToString(Text, *Path)))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Fixture = ParseObject(Text);
		if (!TestTrue(FString::Printf(TEXT("%s parses"), *File), Fixture.IsValid()))
		{
			continue;
		}

		const FString Name = Fixture->GetStringField(TEXT("name"));
		const FString Route = Fixture->GetStringField(TEXT("route"));
		const int32 Status = static_cast<int32>(Fixture->GetNumberField(TEXT("status")));

		// The request must survive the plugin's own parser.
		const TSharedPtr<FJsonObject>* Request = nullptr;
		TestTrue(
			FString::Printf(TEXT("%s: request is an object"), *Name),
			Fixture->TryGetObjectField(TEXT("request"), Request) && Request->IsValid());

		// The route it was captured against must still be registered.
		TestTrue(
			FString::Printf(TEXT("%s: route '%s' is still registered (routes: %d)"), *Name, *Route, Routes.Num()),
			Routes.Contains(Route));

		const TSharedPtr<FJsonObject>* Expected = nullptr;
		if (!Fixture->TryGetObjectField(TEXT("response"), Expected) || !Expected->IsValid())
		{
			AddError(FString::Printf(TEXT("%s: response is not an object"), *Name));
			continue;
		}

		// Rebuild the envelope through the real responder and compare it to
		// what the plugin actually sent when the fixture was captured.
		int32 Calls = 0;
		int32 SentCode = 0;
		TSharedPtr<FJsonObject> Sent;
		const FHttpResultCallback Callback = [&](TUniquePtr<FHttpServerResponse>&& Response)
		{
			++Calls;
			SentCode = static_cast<int32>(Response->Code);
			Sent = ResponseBody(*Response);
		};

		bool bOk = false;
		(*Expected)->TryGetBoolField(TEXT("ok"), bOk);
		{
			const TSharedRef<McpLink::FMcpResponder> Responder =
				MakeShared<McpLink::FMcpResponder>(Callback);
			if (bOk)
			{
				const TSharedPtr<FJsonObject>* Data = nullptr;
				if ((*Expected)->TryGetObjectField(TEXT("data"), Data) && Data->IsValid())
				{
					Responder->Ok(Data->ToSharedRef());
				}
				else
				{
					AddError(FString::Printf(TEXT("%s: ok envelope without a data object"), *Name));
					Responder->Ok(MakeShared<FJsonObject>());
				}
			}
			else
			{
				const TSharedPtr<FJsonObject>* Error = nullptr;
				if ((*Expected)->TryGetObjectField(TEXT("error"), Error) && Error->IsValid())
				{
					Responder->Error(
						static_cast<EHttpServerResponseCodes>(Status),
						(*Error)->GetStringField(TEXT("code")),
						(*Error)->GetStringField(TEXT("message")));
				}
				else
				{
					AddError(FString::Printf(TEXT("%s: error envelope without an error object"), *Name));
				}
			}
		}

		TestEqual(FString::Printf(TEXT("%s: exactly one response"), *Name), Calls, 1);
		TestEqual(FString::Printf(TEXT("%s: HTTP status"), *Name), SentCode, Status);
		if (TestTrue(FString::Printf(TEXT("%s: envelope parses"), *Name), Sent.IsValid()))
		{
			const FJsonValueObject SentValue(Sent);
			const FJsonValueObject ExpectedValue(*Expected);
			TestTrue(
				FString::Printf(
					TEXT("%s: the envelope FMcpResponder builds still matches the recorded one"), *Name),
				FJsonValue::CompareEqual(SentValue, ExpectedValue));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
