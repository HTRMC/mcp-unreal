// Python escape hatch: run editor Python through IPythonScriptPlugin and
// return its log output and (for expressions) the result. Covers everything
// the `unreal` module exposes that has no dedicated McpLink route yet.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "IPythonScriptPlugin.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResponder.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "PythonScriptTypes.h"

namespace McpLink
{
	void RegisterPythonRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/python/exec"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
				if (Python == nullptr || !Python->IsPythonAvailable())
				{
					Responder->Error(EHttpServerResponseCodes::NotSupported, TEXT("python_unavailable"),
						TEXT("the Python Editor Script Plugin is not available — enable PythonScriptPlugin in the project and restart the editor"));
					return;
				}

				FString Code, File, Mode = TEXT("exec");
				Body->TryGetStringField(TEXT("code"), Code);
				Body->TryGetStringField(TEXT("file"), File);
				Body->TryGetStringField(TEXT("mode"), Mode);
				if (Code.IsEmpty() && File.IsEmpty())
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
						TEXT("'code' (Python source) or 'file' (path to a .py file) is required"));
					return;
				}
				bool bPersist = false;
				Body->TryGetBoolField(TEXT("persist"), bPersist);
				// Public scope runs in __main__, so names survive between calls.
				const EPythonFileExecutionScope Scope =
					bPersist ? EPythonFileExecutionScope::Public : EPythonFileExecutionScope::Private;

				FPythonCommandEx Command;
				FString TempFile;
				if (!File.IsEmpty())
				{
					Command.Command = File;
					Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
					Command.FileExecutionScope = Scope;
				}
				else if (Mode.Equals(TEXT("eval"), ESearchCase::IgnoreCase))
				{
					Command.Command = Code;
					Command.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
				}
				else if (Code.Contains(TEXT("\n")))
				{
					// ExecuteStatement is Py_single_input (one interactive statement), so
					// multi-line source runs as a file (Py_file_input) from a temp path.
					TempFile = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("McpLink"), TEXT("Python"),
						FString::Printf(TEXT("exec_%s.py"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
					if (!FFileHelper::SaveStringToFile(
						Code, *TempFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("temp_file_failed"),
							FString::Printf(TEXT("could not write %s"), *TempFile));
						return;
					}
					Command.Command = TempFile;
					Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
					Command.FileExecutionScope = Scope;
				}
				else
				{
					Command.Command = Code;
					Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteStatement;
				}

				const bool bOk = Python->ExecPythonCommandEx(Command);
				if (!TempFile.IsEmpty())
				{
					IFileManager::Get().Delete(*TempFile, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
				}

				TArray<TSharedPtr<FJsonValue>> Output;
				TArray<FString> Errors;
				for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
				{
					const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("level"),
						Entry.Type == EPythonLogOutputType::Error ? TEXT("error")
						: Entry.Type == EPythonLogOutputType::Warning ? TEXT("warning") : TEXT("info"));
					Item->SetStringField(TEXT("text"), Entry.Output.TrimEnd());
					Output.Add(MakeShared<FJsonValueObject>(Item));
					if (Entry.Type == EPythonLogOutputType::Error)
					{
						Errors.Add(Entry.Output.TrimEnd());
					}
				}

				if (!bOk)
				{
					// On failure the plugin writes the traceback into CommandResult.
					FString Message = Command.CommandResult.TrimEnd();
					for (const FString& Error : Errors)
					{
						// The log usually repeats the traceback; only add what is new.
						if (!Message.Contains(Error))
						{
							Message += (Message.IsEmpty() ? TEXT("") : TEXT("\n")) + Error;
						}
					}
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("python_error"),
						Message.IsEmpty() ? TEXT("Python reported failure without output") : Message);
					return;
				}

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetBoolField(TEXT("ok"), true);
				Data->SetStringField(TEXT("result"), Command.CommandResult);
				Data->SetArrayField(TEXT("output"), Output);
				Responder->Ok(Data);
			});
	}
}
