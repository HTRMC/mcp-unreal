#include "McpLinkCoreModule.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "HttpPath.h"
#include "HttpRequestHandler.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "IHttpRouter.h"
#include "McpJson.h"
#include "McpLogCapture.h"
#include "McpResponder.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/CoreMisc.h"
#include "Misc/ScopeExit.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"

#define MCPLINK_VERSION TEXT("0.1.0")

DEFINE_LOG_CATEGORY_STATIC(LogMcpLink, Log, All);

static TAutoConsoleVariable<int32> CVarMcpLinkPort(
	TEXT("McpLink.Port"),
	8091,
	TEXT("Port for the McpLink HTTP control server (loopback). 0 disables. Read once at editor startup."),
	ECVF_ReadOnly);

namespace
{
	ELogVerbosity::Type ParseVerbosity(const FString& Name)
	{
		if (Name.Equals(TEXT("error"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Error; }
		if (Name.Equals(TEXT("warning"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Warning; }
		if (Name.Equals(TEXT("display"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Display; }
		if (Name.Equals(TEXT("verbose"), ESearchCase::IgnoreCase)) { return ELogVerbosity::Verbose; }
		if (Name.Equals(TEXT("veryverbose"), ESearchCase::IgnoreCase)) { return ELogVerbosity::VeryVerbose; }
		return ELogVerbosity::Log;
	}
}

void FMcpLinkCoreModule::StartupModule()
{
	LogCapture = MakeUnique<McpLink::FMcpLogCapture>();

	// --- /api/status ------------------------------------------------------
	RegisterRoute(
		TEXT("/api/status"),
		[this](const TSharedRef<FJsonObject>& /*Body*/, TSharedRef<McpLink::FMcpResponder> Responder)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("plugin_version"), MCPLINK_VERSION);
			const FEngineVersion& Ver = FEngineVersion::Current();
			Data->SetStringField(
				TEXT("engine_version"),
				FString::Printf(TEXT("%u.%u.%u"), Ver.GetMajor(), Ver.GetMinor(), Ver.GetPatch()));
			Data->SetStringField(TEXT("project"), FApp::GetProjectName());
			const bool bPieActive = GEditor != nullptr && GEditor->IsPlayingSessionInEditor();
			Data->SetBoolField(TEXT("pie_active"), bPieActive);
			if (bPieActive && GEditor->PlayWorld != nullptr)
			{
				Data->SetStringField(TEXT("pie_map"), GEditor->PlayWorld->GetMapName());
			}
			Data->SetNumberField(TEXT("port"), Port);
			TArray<TSharedPtr<FJsonValue>> Routes;
			for (const FString& Path : RoutePaths)
			{
				Routes.Add(MakeShared<FJsonValueString>(Path));
			}
			Data->SetArrayField(TEXT("routes"), Routes);
			Responder->Ok(Data);
		},
		/*bAllowGet*/ true);

	// --- /api/editor/output_log ------------------------------------------
	RegisterRoute(
		TEXT("/api/editor/output_log"),
		[this](const TSharedRef<FJsonObject>& Body, TSharedRef<McpLink::FMcpResponder> Responder)
		{
			const uint64 SinceSeq = static_cast<uint64>(Body->HasTypedField<EJson::Number>(TEXT("since_seq"))
				? Body->GetNumberField(TEXT("since_seq"))
				: 0.0);
			const int32 MaxLines = FMath::Clamp(
				Body->HasTypedField<EJson::Number>(TEXT("max_lines"))
					? static_cast<int32>(Body->GetNumberField(TEXT("max_lines")))
					: 100,
				1, 500);
			FString CategoryStr;
			Body->TryGetStringField(TEXT("category"), CategoryStr);
			FString VerbosityStr;
			Body->TryGetStringField(TEXT("min_verbosity"), VerbosityStr);
			const ELogVerbosity::Type MinVerbosity =
				VerbosityStr.IsEmpty() ? ELogVerbosity::Log : ParseVerbosity(VerbosityStr);

			TArray<McpLink::FMcpLogEntry> Entries;
			uint64 LastSeq = 0;
			LogCapture->Get(
				SinceSeq, MaxLines,
				CategoryStr.IsEmpty() ? NAME_None : FName(*CategoryStr),
				MinVerbosity, Entries, LastSeq);

			TArray<TSharedPtr<FJsonValue>> JsonEntries;
			JsonEntries.Reserve(Entries.Num());
			for (const McpLink::FMcpLogEntry& Entry : Entries)
			{
				const TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
				E->SetNumberField(TEXT("seq"), static_cast<double>(Entry.Seq));
				E->SetStringField(TEXT("category"), Entry.Category.ToString());
				E->SetStringField(TEXT("verbosity"), ::ToString(Entry.Verbosity));
				E->SetStringField(TEXT("message"), Entry.Message);
				JsonEntries.Add(MakeShared<FJsonValueObject>(E));
			}
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetArrayField(TEXT("entries"), JsonEntries);
			Data->SetNumberField(TEXT("last_seq"), static_cast<double>(LastSeq));
			Responder->Ok(Data);
		});

	StartHttpServer();
}

void FMcpLinkCoreModule::ShutdownModule()
{
	StopHttpServer();
	LogCapture.Reset();
}

McpLink::FMcpLogCapture* FMcpLinkCoreModule::GetLogCapture() const
{
	return LogCapture.Get();
}

void FMcpLinkCoreModule::RegisterRoute(const FString& Path, McpLink::FMcpHandler Handler, bool bAllowGet)
{
	RoutePaths.Add(Path);
	if (bServerStarted)
	{
		BindRoute(Path, MoveTemp(Handler), bAllowGet);
	}
	else
	{
		PendingRoutes.Add({Path, MoveTemp(Handler), bAllowGet});
	}
}

void FMcpLinkCoreModule::StartHttpServer()
{
	Port = static_cast<uint32>(FMath::Max(0, CVarMcpLinkPort.GetValueOnGameThread()));
	if (Port == 0)
	{
		UE_LOG(LogMcpLink, Display, TEXT("McpLink HTTP server disabled (McpLink.Port=0)"));
		return;
	}

	FHttpServerModule& HttpModule = FModuleManager::LoadModuleChecked<FHttpServerModule>(TEXT("HTTPServer"));
	Router = HttpModule.GetHttpRouter(Port, /*bFailOnBindFailure*/ true);
	if (!Router.IsValid())
	{
		UE_LOG(LogMcpLink, Error,
			TEXT("McpLink: failed to bind HTTP router on port %u — is another instance or plugin using it? ")
			TEXT("Set the McpLink.Port CVar (e.g. in DefaultEngine.ini [ConsoleVariables]) and restart."),
			Port);
		return;
	}
	bServerStarted = true;

	for (FPendingRoute& Pending : PendingRoutes)
	{
		BindRoute(Pending.Path, MoveTemp(Pending.Handler), Pending.bAllowGet);
	}
	PendingRoutes.Empty();

	HttpModule.StartAllListeners();

	// Keep answering while execution is halted on a Blueprint breakpoint. See
	// PumpWhileHalted: without this the resume request cannot be delivered,
	// because the thing that would deliver it is what the halt stopped.
	HttpTicker = &HttpModule;
	if (FSlateApplication::IsInitialized())
	{
		PreTickHandle = FSlateApplication::Get().OnPreTick().AddRaw(this, &FMcpLinkCoreModule::PumpWhileHalted);
	}

	UE_LOG(LogMcpLink, Display, TEXT("McpLink %s listening on http://127.0.0.1:%u"), MCPLINK_VERSION, Port);
}

bool FMcpLinkCoreModule::IsHaltedAtBreakpoint()
{
#if WITH_EDITORONLY_DATA
	return GIntraFrameDebuggingGameThread;
#else
	return false;
#endif
}

bool FMcpLinkCoreModule::IsServableWhileHalted(const FString& Path)
{
	return Path == TEXT("/api/blueprints/debug")
		|| Path == TEXT("/api/status")
		|| Path == TEXT("/api/editor/output_log");
}

// A Blueprint breakpoint parks the game thread in
// FSlateApplication::EnterDebuggingMode — a nested loop that keeps ticking
// Slate but never returns to FEngineLoop, so FTSTicker stops running. The HTTP
// server ticks on FTSTicker, which means a halted editor stops reading its
// socket entirely: the request that would resume execution cannot arrive,
// because delivering it is what the halt suspended. That is the deadlock.
//
// The loop does call FSlateApplication::Tick every iteration, though, and this
// fires from it. Ticking the HTTP server here is all it takes to make a halted
// editor answerable — the debugger's own toolbar is reachable the same way.
void FMcpLinkCoreModule::PumpWhileHalted(float DeltaTime)
{
	if (HttpTicker == nullptr || bDispatching || !IsHaltedAtBreakpoint())
	{
		return;
	}
	HttpTicker->Tick(DeltaTime);
}

void FMcpLinkCoreModule::StopHttpServer()
{
	if (PreTickHandle.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().OnPreTick().Remove(PreTickHandle);
	}
	PreTickHandle.Reset();
	HttpTicker = nullptr;

	if (Router.IsValid())
	{
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			Router->UnbindRoute(Handle);
		}
	}
	RouteHandles.Empty();
	Router.Reset();
	bServerStarted = false;
}

void FMcpLinkCoreModule::BindRoute(const FString& Path, McpLink::FMcpHandler Handler, bool bAllowGet)
{
	EHttpServerRequestVerbs Verbs = EHttpServerRequestVerbs::VERB_POST;
	if (bAllowGet)
	{
		Verbs |= EHttpServerRequestVerbs::VERB_GET;
	}

	const FHttpRouteHandle Handle = Router->BindRoute(
		FHttpPath(Path),
		Verbs,
		FHttpRequestHandler::CreateLambda(
			[this, Handler = MoveTemp(Handler), Path](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) -> bool
			{
				// FHttpServerModule ticks on the game thread; every handler
				// relies on that for GEditor/UWorld access.
				check(IsInGameThread());
				const TSharedRef<McpLink::FMcpResponder> Responder = MakeShared<McpLink::FMcpResponder>(OnComplete);
				const TSharedPtr<FJsonObject> Body = McpLink::ParseBody(Request);
				if (!Body.IsValid())
				{
					Responder->Error(
						EHttpServerResponseCodes::BadRequest,
						TEXT("invalid_json"),
						TEXT("request body is not valid JSON"));
					return true;
				}
				// While halted this request arrived from PumpWhileHalted, which
				// means the call stack still holds a paused Blueprint. Only the
				// routes that exist to inspect and end that state are safe to
				// run; anything else would re-enter the engine mid-frame.
				if (IsHaltedAtBreakpoint() && !IsServableWhileHalted(Path))
				{
					Responder->Error(
						EHttpServerResponseCodes::Conflict,
						TEXT("halted_at_breakpoint"),
						TEXT("execution is halted on a Blueprint breakpoint — only blueprint_debug, "
							 "status and output_log are served until it resumes. Use blueprint_debug "
							 "with operation 'resume', 'step_over', 'step_into' or 'step_out'"));
					return true;
				}
				TGuardValue<bool> InDispatch(bDispatching, true);
				// Nobody is going to click a dialog. FMessageDialog and friends
				// only skip the modal when the process is unattended, and a
				// modal here blocks the game thread forever: the responder
				// never fires, and the request that would cancel it cannot be
				// served either, because the HTTP server ticks on this thread.
				// Editor code that prompts (asset rename with referencers, save
				// prompts, "are you sure") then takes its default answer, so an
				// operation is refused rather than hanging the editor.
				TGuardValue<bool> Unattended(GIsRunningUnattendedScript, true);
				Handler(Body.ToSharedRef(), Responder);
				return true;
			}));

	if (Handle.IsValid())
	{
		RouteHandles.Add(Handle);
	}
	else
	{
		UE_LOG(LogMcpLink, Error, TEXT("McpLink: failed to bind route %s"), *Path);
	}
}

IMPLEMENT_MODULE(FMcpLinkCoreModule, McpLinkCore)
