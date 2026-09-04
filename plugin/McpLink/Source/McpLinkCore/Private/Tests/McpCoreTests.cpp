// Automation tests for McpLinkCore. Run headless with
//   run_tests {"filter": "McpLink"}
// or from the editor's Session Frontend.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMeshActor.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "McpJson.h"
#include "McpLogCapture.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr EAutomationTestFlags McpTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	FHttpServerRequest RequestWithBody(const FString& Text)
	{
		FHttpServerRequest Request;
		const FTCHARToUTF8 Utf8(*Text);
		Request.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Request;
	}

	TSharedPtr<FJsonObject> ParseResponseBody(const FHttpServerResponse& Response)
	{
		TArray<uint8> Bytes = Response.Body;
		Bytes.Add(0);
		const FString Text = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData()));
		TSharedPtr<FJsonObject> Parsed;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Parsed);
		return Parsed;
	}
}

// ---------------------------------------------------------------- JSON body

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpParseBodyTest, "McpLink.Core.Json.ParseBody", McpTestFlags)
bool FMcpParseBodyTest::RunTest(const FString& Parameters)
{
	// Empty body is a valid, empty object (every route tolerates no body).
	const FHttpServerRequest Empty;
	const TSharedPtr<FJsonObject> EmptyResult = McpLink::ParseBody(Empty);
	TestTrue(TEXT("empty body parses"), EmptyResult.IsValid());
	if (EmptyResult.IsValid())
	{
		TestEqual(TEXT("empty body has no fields"), EmptyResult->Values.Num(), 0);
	}

	// The raw body is not null-terminated; ParseBody must copy and terminate it.
	const TSharedPtr<FJsonObject> Valid =
		McpLink::ParseBody(RequestWithBody(TEXT("{\"a\": 1, \"b\": \"x\", \"nested\": {\"c\": true}}")));
	TestTrue(TEXT("valid body parses"), Valid.IsValid());
	if (Valid.IsValid())
	{
		TestEqual(TEXT("number field"), Valid->GetNumberField(TEXT("a")), 1.0);
		TestEqual(TEXT("string field"), Valid->GetStringField(TEXT("b")), FString(TEXT("x")));
		TestTrue(TEXT("nested object"), Valid->HasTypedField<EJson::Object>(TEXT("nested")));
	}

	// Malformed JSON must be reported, not silently treated as empty.
	TestFalse(TEXT("malformed body is rejected"),
		McpLink::ParseBody(RequestWithBody(TEXT("{not json"))).IsValid());

	// Non-ASCII survives the UTF-8 round trip.
	const TSharedPtr<FJsonObject> Unicode =
		McpLink::ParseBody(RequestWithBody(TEXT("{\"name\": \"Zoë — ü\"}")));
	if (TestTrue(TEXT("unicode body parses"), Unicode.IsValid()))
	{
		TestEqual(TEXT("unicode preserved"),
			Unicode->GetStringField(TEXT("name")), FString(TEXT("Zoë — ü")));
	}
	return true;
}

// ---------------------------------------------------------------- resolvers

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpVectorParsingTest, "McpLink.Core.Resolve.Vectors", McpTestFlags)
bool FMcpVectorParsingTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetArrayField(TEXT("location"), {
		MakeShared<FJsonValueNumber>(1.0), MakeShared<FJsonValueNumber>(2.5), MakeShared<FJsonValueNumber>(-3.0)});
	Body->SetArrayField(TEXT("rotation"), {
		MakeShared<FJsonValueNumber>(10.0), MakeShared<FJsonValueNumber>(20.0), MakeShared<FJsonValueNumber>(30.0)});
	Body->SetArrayField(TEXT("short"), {MakeShared<FJsonValueNumber>(1.0), MakeShared<FJsonValueNumber>(2.0)});
	Body->SetStringField(TEXT("wrong_type"), TEXT("1,2,3"));

	FVector Location;
	TestTrue(TEXT("3-element array parses"), McpLink::GetVector(Body, TEXT("location"), Location));
	TestEqual(TEXT("vector value"), Location, FVector(1.0, 2.5, -3.0));

	FRotator Rotation;
	TestTrue(TEXT("rotator parses"), McpLink::GetRotator(Body, TEXT("rotation"), Rotation));
	TestEqual(TEXT("pitch"), Rotation.Pitch, 10.0);
	TestEqual(TEXT("yaw"), Rotation.Yaw, 20.0);
	TestEqual(TEXT("roll"), Rotation.Roll, 30.0);

	FVector Unused;
	TestFalse(TEXT("2-element array rejected"), McpLink::GetVector(Body, TEXT("short"), Unused));
	TestFalse(TEXT("string rejected"), McpLink::GetVector(Body, TEXT("wrong_type"), Unused));
	TestFalse(TEXT("missing field rejected"), McpLink::GetVector(Body, TEXT("nope"), Unused));

	// Round trip through the serialiser.
	const TArray<TSharedPtr<FJsonValue>> Json = McpLink::VectorToJson(FVector(4, 5, 6));
	TestEqual(TEXT("VectorToJson has 3 elements"), Json.Num(), 3);
	if (Json.Num() == 3)
	{
		TestEqual(TEXT("VectorToJson z"), Json[2]->AsNumber(), 6.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpResolveClassTest, "McpLink.Core.Resolve.Class", McpTestFlags)
bool FMcpResolveClassTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("bare class name"),
		McpLink::ResolveClass(TEXT("StaticMeshActor")), AStaticMeshActor::StaticClass());
	TestEqual(TEXT("script path"),
		McpLink::ResolveClass(TEXT("/Script/Engine.PointLight")), APointLight::StaticClass());
	TestNull(TEXT("unknown class is null"), McpLink::ResolveClass(TEXT("NoSuchClassXyz123")));
	TestNull(TEXT("empty spec is null"), McpLink::ResolveClass(TEXT("")));

	// The world selector defaults to the editor world when PIE is not running.
	const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	TestNotNull(TEXT("auto world resolves"), McpLink::ResolveWorld(Body));
	Body->SetStringField(TEXT("world"), TEXT("pie"));
	if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
	{
		TestNull(TEXT("pie world is null without a session — never a silent fallback"),
			McpLink::ResolveWorld(Body));
	}
	return true;
}

// ---------------------------------------------------------------- responder

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpResponderTest, "McpLink.Core.Responder.ExactlyOnce", McpTestFlags)
bool FMcpResponderTest::RunTest(const FString& Parameters)
{
	int32 Calls = 0;
	EHttpServerResponseCodes Code = EHttpServerResponseCodes::Unknown;
	TSharedPtr<FJsonObject> Envelope;
	const FHttpResultCallback Callback =
		[&](TUniquePtr<FHttpServerResponse>&& Response)
		{
			++Calls;
			Code = Response->Code;
			Envelope = ParseResponseBody(*Response);
		};

	// The double completion below is deliberate; the responder warns about it.
	AddExpectedMessagePlain(TEXT("attempted double response"), ELogVerbosity::Warning);
	{
		const TSharedRef<McpLink::FMcpResponder> Responder =
			MakeShared<McpLink::FMcpResponder>(Callback);
		Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("thing_missing"), TEXT("no thing"));
		// A second completion must be ignored, not sent.
		Responder->Ok(MakeShared<FJsonObject>());
		TestTrue(TEXT("completed after first send"), Responder->IsCompleted());
	}
	TestEqual(TEXT("exactly one response"), Calls, 1);
	TestEqual(TEXT("real HTTP status code"), static_cast<int32>(Code), 404);
	if (TestTrue(TEXT("envelope parses"), Envelope.IsValid()))
	{
		TestFalse(TEXT("ok is false"), Envelope->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* Error = nullptr;
		if (TestTrue(TEXT("error object present"), Envelope->TryGetObjectField(TEXT("error"), Error)))
		{
			TestEqual(TEXT("error code"), (*Error)->GetStringField(TEXT("code")), FString(TEXT("thing_missing")));
			TestEqual(TEXT("error message"), (*Error)->GetStringField(TEXT("message")), FString(TEXT("no thing")));
		}
	}

	// Success envelope.
	Calls = 0;
	{
		const TSharedRef<McpLink::FMcpResponder> Responder =
			MakeShared<McpLink::FMcpResponder>(Callback);
		const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("answer"), 42);
		Responder->Ok(Data);
	}
	TestEqual(TEXT("one success response"), Calls, 1);
	TestEqual(TEXT("200 on success"), static_cast<int32>(Code), 200);
	if (Envelope.IsValid())
	{
		TestTrue(TEXT("ok is true"), Envelope->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (TestTrue(TEXT("data present"), Envelope->TryGetObjectField(TEXT("data"), Data)))
		{
			TestEqual(TEXT("data payload"), (*Data)->GetNumberField(TEXT("answer")), 42.0);
		}
	}

	// A responder dropped without completing must still answer (500), so a
	// handler bug never leaves the client hanging.
	Calls = 0;
	{
		const TSharedRef<McpLink::FMcpResponder> Dropped =
			MakeShared<McpLink::FMcpResponder>(Callback);
		(void)Dropped;
	}
	TestEqual(TEXT("dropped responder still responds"), Calls, 1);
	TestEqual(TEXT("dropped responder is a 500"), static_cast<int32>(Code), 500);
	return true;
}

// ---------------------------------------------------------------- log ring

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpLogCaptureTest, "McpLink.Core.LogCapture.Ring", McpTestFlags)
bool FMcpLogCaptureTest::RunTest(const FString& Parameters)
{
	// The capture hooks GLog and sees every log line in the process, so isolate
	// this test's entries by category.
	const FName Category(TEXT("LogMcpLinkTestOnly"));
	McpLink::FMcpLogCapture Capture;
	Capture.Serialize(TEXT("first warning"), ELogVerbosity::Warning, Category);
	Capture.Serialize(TEXT("some verbose"), ELogVerbosity::Verbose, Category);
	Capture.Serialize(TEXT("an error"), ELogVerbosity::Error, Category);
	Capture.Serialize(TEXT("other category"), ELogVerbosity::Error, FName(TEXT("LogMcpLinkOther")));

	TArray<McpLink::FMcpLogEntry> Entries;
	uint64 LastSeq = 0;

	Capture.Get(0, 100, Category, ELogVerbosity::Log, Entries, LastSeq);
	TestEqual(TEXT("category filter: Log level excludes Verbose"), Entries.Num(), 2);
	TestTrue(TEXT("last_seq advances"), LastSeq >= 4);
	if (Entries.Num() == 2)
	{
		TestTrue(TEXT("oldest first"), Entries[0].Seq < Entries[1].Seq);
		TestEqual(TEXT("first message"), Entries[0].Message, FString(TEXT("first warning")));
	}

	Entries.Reset();
	Capture.Get(0, 100, Category, ELogVerbosity::Error, Entries, LastSeq);
	TestEqual(TEXT("Error level keeps only errors"), Entries.Num(), 1);

	Entries.Reset();
	Capture.Get(0, 100, Category, ELogVerbosity::VeryVerbose, Entries, LastSeq);
	TestEqual(TEXT("VeryVerbose keeps everything"), Entries.Num(), 3);

	// Incremental polling: nothing newer than the last sequence.
	Entries.Reset();
	Capture.Get(LastSeq, 100, Category, ELogVerbosity::VeryVerbose, Entries, LastSeq);
	TestEqual(TEXT("since_seq returns nothing new"), Entries.Num(), 0);

	// Cap keeps the newest.
	Entries.Reset();
	Capture.Get(0, 1, Category, ELogVerbosity::VeryVerbose, Entries, LastSeq);
	TestEqual(TEXT("cap to one"), Entries.Num(), 1);
	if (Entries.Num() == 1)
	{
		TestEqual(TEXT("cap keeps newest"), Entries[0].Message, FString(TEXT("an error")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
