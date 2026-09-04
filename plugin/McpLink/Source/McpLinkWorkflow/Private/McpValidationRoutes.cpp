// The checks the editor runs before you ship: Data Validation, Map Check, and
// compiling every Blueprint.
//
// An agent that authors assets has no equivalent of a human noticing a red
// message log, so these are the readback for "is what I built actually valid".

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorValidatorSubsystem.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/MessageLog.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/PackageName.h"
#include "Misc/UObjectToken.h"

namespace McpLink
{
	namespace Validation
	{
		/// Assets named in `assets`, or every asset under `path_prefix`.
		TArray<FAssetData> GatherAssets(const TSharedRef<FJsonObject>& Body, FString& OutScope)
		{
			IAssetRegistry& Registry =
				FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

			TArray<FAssetData> Assets;
			const TArray<TSharedPtr<FJsonValue>>* Specs = nullptr;
			if (Body->TryGetArrayField(TEXT("assets"), Specs) && Specs != nullptr && !Specs->IsEmpty())
			{
				for (const TSharedPtr<FJsonValue>& Value : *Specs)
				{
					FString Spec;
					if (!Value.IsValid() || !Value->TryGetString(Spec) || Spec.IsEmpty())
					{
						continue;
					}
					const FString ObjectPath = Spec.Contains(TEXT("."))
						? Spec
						: FString::Printf(TEXT("%s.%s"), *Spec, *FPackageName::GetShortName(Spec));
					const FAssetData Found =
						Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
					if (Found.IsValid())
					{
						Assets.Add(Found);
					}
				}
				OutScope = FString::Printf(TEXT("%d named assets"), Assets.Num());
				return Assets;
			}

			FString Prefix = TEXT("/Game");
			Body->TryGetStringField(TEXT("path_prefix"), Prefix);
			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*Prefix));
			Filter.bRecursivePaths = true;
			Registry.GetAssets(Filter, Assets);
			OutScope = Prefix;
			return Assets;
		}
	}

	using namespace Validation;

	void RegisterValidationRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/workflow/validate"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("assets"))
				{
					UEditorValidatorSubsystem* Subsystem =
						GEditor != nullptr ? GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>()
										   : nullptr;
					if (Subsystem == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_subsystem"),
							TEXT("the Data Validation plugin is not loaded"));
						return;
					}

					FString Scope;
					TArray<FAssetData> Assets = GatherAssets(Body, Scope);
					const int32 Limit = FMath::Clamp(IntOr(Body, TEXT("max_assets"), 500), 1, 20000);
					if (Assets.Num() > Limit)
					{
						Assets.SetNum(Limit);
					}
					if (Assets.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("no_assets"),
							FString::Printf(TEXT("no assets to validate under '%s'"), *Scope));
						return;
					}

					FValidateAssetsSettings Settings;
					Settings.bLoadAssetsForValidation = true;
					Settings.bCollectPerAssetDetails = true;
					// A toast would be pointless here and a modal would hang.
					Settings.bShowIfNoFailures = false;
					FValidateAssetsResults Results;
					const int32 Failures =
						Subsystem->ValidateAssetsWithSettings(Assets, Settings, Results);

					TArray<TSharedPtr<FJsonValue>> Problems;
					const int32 MaxReported = FMath::Clamp(IntOr(Body, TEXT("max_results"), 50), 1, 500);
					for (const TPair<FString, FValidateAssetsDetails>& Pair : Results.AssetsDetails)
					{
						if (Pair.Value.Result != EDataValidationResult::Invalid
							&& Pair.Value.ValidationMessages.IsEmpty())
						{
							continue;
						}
						if (Problems.Num() >= MaxReported)
						{
							break;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("asset"), Pair.Key);
						Item->SetStringField(TEXT("result"),
							Pair.Value.Result == EDataValidationResult::Invalid ? TEXT("invalid")
																				: TEXT("warnings"));
						TArray<TSharedPtr<FJsonValue>> Messages;
						for (const TSharedRef<FTokenizedMessage>& Message : Pair.Value.ValidationMessages)
						{
							Messages.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
						}
						Item->SetArrayField(TEXT("messages"), Messages);
						Problems.Add(MakeShared<FJsonValueObject>(Item));
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("scope"), Scope);
					Data->SetNumberField(TEXT("checked"), Results.NumChecked);
					Data->SetNumberField(TEXT("valid"), Results.NumValid);
					Data->SetNumberField(TEXT("invalid"), Results.NumInvalid);
					Data->SetNumberField(TEXT("warnings"), Results.NumWarnings);
					Data->SetNumberField(TEXT("skipped"), Results.NumSkipped);
					Data->SetNumberField(TEXT("failures"), Failures);
					Data->SetArrayField(TEXT("problems"), Problems);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("map_check"))
				{
					UWorld* World = ResolveWorldOrError(Body, Responder);
					if (World == nullptr)
					{
						return;
					}
					// MAP CHECK writes into the MapCheck message log, so clear it
					// first and read back exactly what this run produced.
					FMessageLog MapCheckLog(TEXT("MapCheck"));
					MapCheckLog.NewPage(FText::FromString(TEXT("McpLink Map Check")));
					GEditor->Exec(World, TEXT("MAP CHECK DONTDISPLAYDIALOG"));

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("world"), World->GetName());
					Data->SetNumberField(TEXT("errors"), MapCheckLog.NumMessages(EMessageSeverity::Error));
					Data->SetNumberField(TEXT("warnings"),
						MapCheckLog.NumMessages(EMessageSeverity::Warning));
					Data->SetStringField(TEXT("message"),
						TEXT("counts come from the MapCheck message log; get_output_log shows the text"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("compile_blueprints"))
				{
					IAssetRegistry& Registry =
						FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
							.Get();
					FString Prefix = TEXT("/Game");
					Body->TryGetStringField(TEXT("path_prefix"), Prefix);
					FARFilter Filter;
					Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
					Filter.bRecursiveClasses = true;
					Filter.PackagePaths.Add(FName(*Prefix));
					Filter.bRecursivePaths = true;
					TArray<FAssetData> Assets;
					Registry.GetAssets(Filter, Assets);

					const int32 Limit = FMath::Clamp(IntOr(Body, TEXT("max_blueprints"), 500), 1, 20000);
					const int32 MaxReported = FMath::Clamp(IntOr(Body, TEXT("max_results"), 50), 1, 500);
					int32 Compiled = 0;
					int32 Failed = 0;
					TArray<TSharedPtr<FJsonValue>> Problems;
					for (const FAssetData& Asset : Assets)
					{
						if (Compiled >= Limit)
						{
							break;
						}
						UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
						if (Blueprint == nullptr)
						{
							continue;
						}
						FCompilerResultsLog Log;
						Log.bSilentMode = true;
						FKismetEditorUtilities::CompileBlueprint(
							Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Log);
						++Compiled;
						if (Log.NumErrors == 0 && Log.NumWarnings == 0)
						{
							continue;
						}
						if (Log.NumErrors > 0)
						{
							++Failed;
						}
						if (Problems.Num() >= MaxReported)
						{
							continue;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
						Item->SetNumberField(TEXT("errors"), Log.NumErrors);
						Item->SetNumberField(TEXT("warnings"), Log.NumWarnings);
						TArray<TSharedPtr<FJsonValue>> Messages;
						for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
						{
							if (Messages.Num() >= 10)
							{
								break;
							}
							Messages.Add(MakeShared<FJsonValueString>(Message->ToText().ToString()));
						}
						Item->SetArrayField(TEXT("messages"), Messages);
						Problems.Add(MakeShared<FJsonValueObject>(Item));
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("scope"), Prefix);
					Data->SetNumberField(TEXT("found"), Assets.Num());
					Data->SetNumberField(TEXT("compiled"), Compiled);
					Data->SetNumberField(TEXT("with_errors"), Failed);
					Data->SetArrayField(TEXT("problems"), Problems);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use assets, map_check, or compile_blueprints"),
						*Operation));
			});
	}
}
