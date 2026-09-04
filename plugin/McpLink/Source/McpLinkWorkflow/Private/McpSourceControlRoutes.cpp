// Source control: what the Perforce/Git/Plastic provider knows about the
// project's files, and the operations the editor's own menus run on them.
//
// McpLink writes and saves assets directly, so without this an agent silently
// edits files it never checked out. Everything goes through
// ISourceControlProvider, so whichever provider the project configured is the
// one that answers — including "none", which is reported rather than hidden.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SourceControlHelpers.h"
#include "SourceControlOperations.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace SourceControl
	{
		/// Package paths, asset paths and plain filenames all become the
		/// absolute filenames the provider works in.
		TArray<FString> ResolveFiles(
			const TSharedRef<FJsonObject>& Body, TArray<FString>& OutUnresolved)
		{
			TArray<FString> Files;
			const TArray<TSharedPtr<FJsonValue>>* Specs = nullptr;
			if (!Body->TryGetArrayField(TEXT("files"), Specs) || Specs == nullptr)
			{
				return Files;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Specs)
			{
				FString Spec;
				if (!Value.IsValid() || !Value->TryGetString(Spec) || Spec.IsEmpty())
				{
					continue;
				}
				if (FPaths::FileExists(Spec))
				{
					Files.Add(FPaths::ConvertRelativePathToFull(Spec));
					continue;
				}
				// "/Game/Foo" or "/Game/Foo.Foo" -> the package's file on disk.
				const FString PackageName = Spec.Contains(TEXT("."))
					? FPackageName::ObjectPathToPackageName(Spec)
					: Spec;
				FString Filename;
				if (FPackageName::DoesPackageExist(PackageName, &Filename))
				{
					Files.Add(FPaths::ConvertRelativePathToFull(Filename));
				}
				else
				{
					OutUnresolved.Add(Spec);
				}
			}
			return Files;
		}

		TSharedRef<FJsonObject> StateToJson(const FSourceControlStateRef& State)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("file"), State->GetFilename());
			Object->SetStringField(TEXT("status"), State->GetDisplayName().ToString());
			Object->SetBoolField(TEXT("controlled"), State->IsSourceControlled());
			Object->SetBoolField(TEXT("checked_out"), State->IsCheckedOut());
			Object->SetBoolField(TEXT("checked_out_other"), State->IsCheckedOutOther());
			Object->SetBoolField(TEXT("added"), State->IsAdded());
			Object->SetBoolField(TEXT("deleted"), State->IsDeleted());
			Object->SetBoolField(TEXT("modified"), State->IsModified());
			Object->SetBoolField(TEXT("up_to_date"), State->IsCurrent());
			Object->SetBoolField(TEXT("can_check_out"), State->CanCheckout());
			Object->SetBoolField(TEXT("can_edit"), State->CanEdit());
			return Object;
		}

		/// The provider, or nullptr after responding with why source control is
		/// unusable. `bRequireEnabled` is false for `status`, which reports the
		/// disabled state rather than erroring on it.
		ISourceControlProvider* ProviderOrError(
			const TSharedRef<FMcpResponder>& Responder, bool bRequireEnabled = true)
		{
			ISourceControlModule& Module = ISourceControlModule::Get();
			if (bRequireEnabled && !Module.IsEnabled())
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_enabled"),
					TEXT("source control is not enabled for this project — connect a provider in the ")
					TEXT("editor (Revision Control > Connect), or set it in Saved/Config"));
				return nullptr;
			}
			return &Module.GetProvider();
		}

		/// Run a provider operation over files and report per-file results.
		void RunOperation(
			const TSharedRef<FSourceControlOperationBase, ESPMode::ThreadSafe>& Operation,
			const TArray<FString>& Files,
			ISourceControlProvider& Provider,
			const TArray<FString>& Unresolved,
			const TSharedRef<FMcpResponder>& Responder)
		{
			// Synchronous: the handler is on the game thread and the caller is
			// waiting on the HTTP response either way.
			const ECommandResult::Type Result =
				Provider.Execute(Operation, Files, EConcurrency::Synchronous);

			TArray<FSourceControlStateRef> States;
			Provider.GetState(Files, States, EStateCacheUsage::Use);
			TArray<TSharedPtr<FJsonValue>> StateJson;
			for (const FSourceControlStateRef& State : States)
			{
				StateJson.Add(MakeShared<FJsonValueObject>(StateToJson(State)));
			}

			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("operation"), Operation->GetName().ToString());
			Data->SetBoolField(TEXT("succeeded"), Result == ECommandResult::Succeeded);
			Data->SetNumberField(TEXT("files"), Files.Num());
			Data->SetArrayField(TEXT("states"), StateJson);
			if (!Unresolved.IsEmpty())
			{
				Data->SetStringField(TEXT("warning"),
					FString::Printf(TEXT("no file on disk for: %s"),
						*FString::Join(Unresolved, TEXT(", "))));
			}
			if (Result != ECommandResult::Succeeded)
			{
				Data->SetStringField(TEXT("message"),
					TEXT("the provider rejected the operation — check the editor's Revision Control log"));
			}
			Responder->Ok(Data);
		}
	}

	using namespace SourceControl;

	void RegisterSourceControlRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/workflow/source_control"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				ISourceControlModule& Module = ISourceControlModule::Get();

				if (Operation == TEXT("status"))
				{
					ISourceControlProvider& Provider = Module.GetProvider();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("enabled"), Module.IsEnabled());
					Data->SetStringField(TEXT("provider"), Provider.GetName().ToString());
					Data->SetBoolField(TEXT("available"), Provider.IsAvailable());
					Data->SetStringField(TEXT("status_text"), Provider.GetStatusText().ToString());

					TArray<FString> Unresolved;
					const TArray<FString> Files = ResolveFiles(Body, Unresolved);
					if (!Files.IsEmpty() && Module.IsEnabled())
					{
						// Force a real query rather than trusting a stale cache.
						Provider.Execute(ISourceControlOperation::Create<FUpdateStatus>(), Files,
							EConcurrency::Synchronous);
						TArray<FSourceControlStateRef> States;
						Provider.GetState(Files, States, EStateCacheUsage::Use);
						TArray<TSharedPtr<FJsonValue>> StateJson;
						for (const FSourceControlStateRef& State : States)
						{
							StateJson.Add(MakeShared<FJsonValueObject>(StateToJson(State)));
						}
						Data->SetArrayField(TEXT("states"), StateJson);
					}
					if (!Unresolved.IsEmpty())
					{
						Data->SetStringField(TEXT("warning"),
							FString::Printf(TEXT("no file on disk for: %s"),
								*FString::Join(Unresolved, TEXT(", "))));
					}
					if (!Module.IsEnabled())
					{
						Data->SetStringField(TEXT("message"),
							TEXT("source control is off for this project; every other operation here ")
							TEXT("will say so too"));
					}
					Responder->Ok(Data);
					return;
				}

				ISourceControlProvider* Provider = ProviderOrError(Responder);
				if (Provider == nullptr)
				{
					return;
				}

				if (Operation == TEXT("connect"))
				{
					const ECommandResult::Type Result = Provider->Execute(
						ISourceControlOperation::Create<FConnect>(), EConcurrency::Synchronous);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("provider"), Provider->GetName().ToString());
					Data->SetBoolField(TEXT("connected"), Result == ECommandResult::Succeeded);
					Data->SetStringField(TEXT("status_text"), Provider->GetStatusText().ToString());
					Responder->Ok(Data);
					return;
				}

				TArray<FString> Unresolved;
				TArray<FString> Files = ResolveFiles(Body, Unresolved);
				if (Files.IsEmpty())
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("no_files"),
						Unresolved.IsEmpty()
							? TEXT("'files' must be a non-empty array of package paths or filenames")
							: *FString::Printf(TEXT("no file on disk for: %s"),
								  *FString::Join(Unresolved, TEXT(", "))));
					return;
				}

				if (Operation == TEXT("check_out"))
				{
					RunOperation(ISourceControlOperation::Create<FCheckOut>(), Files, *Provider,
						Unresolved, Responder);
					return;
				}
				if (Operation == TEXT("mark_for_add"))
				{
					RunOperation(ISourceControlOperation::Create<FMarkForAdd>(), Files, *Provider,
						Unresolved, Responder);
					return;
				}
				if (Operation == TEXT("revert"))
				{
					RunOperation(ISourceControlOperation::Create<FRevert>(), Files, *Provider,
						Unresolved, Responder);
					return;
				}
				if (Operation == TEXT("sync"))
				{
					RunOperation(ISourceControlOperation::Create<FSync>(), Files, *Provider,
						Unresolved, Responder);
					return;
				}
				if (Operation == TEXT("mark_for_delete"))
				{
					RunOperation(ISourceControlOperation::Create<FDelete>(), Files, *Provider,
						Unresolved, Responder);
					return;
				}
				if (Operation == TEXT("submit"))
				{
					FString Description;
					if (!RequireString(Body, TEXT("description"), Description, Responder,
							TEXT("the changelist description — a submit without one is rejected")))
					{
						return;
					}
					TSharedRef<FCheckIn, ESPMode::ThreadSafe> CheckIn =
						ISourceControlOperation::Create<FCheckIn>();
					CheckIn->SetDescription(FText::FromString(Description));
					RunOperation(CheckIn, Files, *Provider, Unresolved, Responder);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use status, connect, check_out, mark_for_add, ")
						TEXT("mark_for_delete, revert, sync, or submit"),
						*Operation));
			});
	}
}
