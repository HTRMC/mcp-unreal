// Gameplay tags and curve assets — two small things with no other path.
//
// Tags live in the project's ini rather than in an asset, so config_ops could
// technically write them, but only IGameplayTagsEditorModule keeps the tag
// manager, the ini and the tag sources in step. Curve assets can be *created*
// by asset_ops; their keys are FRichCurve data with no tool until now.

#include "Curves/CurveBase.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "ScopedTransaction.h"

namespace McpLink
{
	namespace TagsAndCurves
	{
		/// The curves an asset exposes, in the order the curve editor shows
		/// them: one for a float curve, three for a vector, four for a colour.
		TArray<FRichCurve*> CurvesOf(UCurveBase* Curve)
		{
			TArray<FRichCurve*> Curves;
			if (UCurveFloat* Float = Cast<UCurveFloat>(Curve))
			{
				Curves.Add(&Float->FloatCurve);
			}
			else if (UCurveVector* Vector = Cast<UCurveVector>(Curve))
			{
				for (FRichCurve& Channel : Vector->FloatCurves)
				{
					Curves.Add(&Channel);
				}
			}
			else if (UCurveLinearColor* Color = Cast<UCurveLinearColor>(Curve))
			{
				for (FRichCurve& Channel : Color->FloatCurves)
				{
					Curves.Add(&Channel);
				}
			}
			return Curves;
		}

		const TCHAR* ChannelName(const UCurveBase* Curve, int32 Index)
		{
			if (Curve->IsA<UCurveLinearColor>())
			{
				static const TCHAR* Names[] = {TEXT("R"), TEXT("G"), TEXT("B"), TEXT("A")};
				return Names[FMath::Clamp(Index, 0, 3)];
			}
			if (Curve->IsA<UCurveVector>())
			{
				static const TCHAR* Names[] = {TEXT("X"), TEXT("Y"), TEXT("Z")};
				return Names[FMath::Clamp(Index, 0, 2)];
			}
			return TEXT("Value");
		}

		ERichCurveInterpMode InterpFromName(const FString& Name)
		{
			if (Name.Equals(TEXT("constant"), ESearchCase::IgnoreCase))
			{
				return RCIM_Constant;
			}
			if (Name.Equals(TEXT("linear"), ESearchCase::IgnoreCase))
			{
				return RCIM_Linear;
			}
			return RCIM_Cubic;
		}

		const TCHAR* InterpName(ERichCurveInterpMode Mode)
		{
			switch (Mode)
			{
				case RCIM_Constant:
					return TEXT("constant");
				case RCIM_Linear:
					return TEXT("linear");
				default:
					return TEXT("cubic");
			}
		}
	}

	using namespace TagsAndCurves;

	void RegisterTagAndCurveRoutes(FMcpLinkCoreModule& Core)
	{
		// ---------------------------------------------------- gameplay tags ---
		Core.RegisterRoute(TEXT("/api/workflow/tags"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);
				UGameplayTagsManager& Manager = UGameplayTagsManager::Get();

				if (Operation == TEXT("list"))
				{
					FString Contains, Under;
					Body->TryGetStringField(TEXT("name_contains"), Contains);
					Body->TryGetStringField(TEXT("under"), Under);
					const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_results"), 200), 1, 5000);

					FGameplayTagContainer All;
					if (!Under.IsEmpty())
					{
						const FGameplayTag Parent =
							Manager.RequestGameplayTag(FName(*Under), /*ErrorIfNotFound*/ false);
						if (!Parent.IsValid())
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("tag_not_found"),
								FString::Printf(TEXT("no gameplay tag '%s'"), *Under));
							return;
						}
						All = Manager.RequestGameplayTagChildren(Parent);
						All.AddTag(Parent);
					}
					else
					{
						Manager.RequestAllGameplayTags(All, /*OnlyIncludeDictionaryTags*/ false);
					}

					TArray<TSharedPtr<FJsonValue>> Tags;
					int32 Matched = 0;
					for (const FGameplayTag& Tag : All)
					{
						const FString Name = Tag.ToString();
						if (!Contains.IsEmpty() && !Name.Contains(Contains))
						{
							continue;
						}
						++Matched;
						if (Tags.Num() < Max)
						{
							Tags.Add(MakeShared<FJsonValueString>(Name));
						}
					}
					Tags.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
						{ return A->AsString() < B->AsString(); });

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Matched);
					Data->SetArrayField(TEXT("tags"), Tags);
					Responder->Ok(Data);
					return;
				}

				FString TagName;
				if (!RequireString(Body, TEXT("tag"), TagName, Responder,
						TEXT("the tag, e.g. Ability.Melee.Heavy")))
				{
					return;
				}

				if (Operation == TEXT("add"))
				{
					FString Comment, Source;
					Body->TryGetStringField(TEXT("comment"), Comment);
					Body->TryGetStringField(TEXT("source"), Source);
					// A tag added to the ini is a project-config edit, not an
					// asset edit, so there is nothing to transact.
					const bool bAdded = IGameplayTagsEditorModule::Get().AddNewGameplayTagToINI(
						TagName, Comment, Source.IsEmpty() ? NAME_None : FName(*Source));
					if (!bAdded)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("add_failed"),
							FString::Printf(
								TEXT("could not add '%s' — it may already exist, or the name is invalid ")
								TEXT("(tags are dot-separated words)"),
								*TagName));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("tag"), TagName);
					Data->SetStringField(TEXT("message"),
						TEXT("written to the project's gameplay tag ini and registered with the manager"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("rename"))
				{
					FString NewName;
					if (!RequireString(Body, TEXT("new_tag"), NewName, Responder, TEXT("the new tag name")))
					{
						return;
					}
					if (!IGameplayTagsEditorModule::Get().RenameTagInINI(TagName, NewName))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("rename_failed"),
							FString::Printf(TEXT("could not rename '%s' to '%s'"), *TagName, *NewName));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("tag"), NewName);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove"))
				{
					TSharedPtr<FGameplayTagNode> Node =
						Manager.FindTagNode(FName(*TagName));
					if (!Node.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("tag_not_found"),
							FString::Printf(TEXT("no gameplay tag '%s'"), *TagName));
						return;
					}
					if (!IGameplayTagsEditorModule::Get().DeleteTagFromINI(Node))
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("remove_failed"),
							FString::Printf(
								TEXT("could not remove '%s' — it may come from a plugin's ini, or be ")
								TEXT("referenced by assets"),
								*TagName));
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), TagName);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — use list, add, rename, or remove"),
						*Operation));
			});

		// ---------------------------------------------------------- curves ---
		Core.RegisterRoute(TEXT("/api/workflow/curves"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				FString Path;
				if (!RequireString(Body, TEXT("curve"), Path, Responder,
						TEXT("a Curve asset path, e.g. /Game/Curves/C_Damage")))
				{
					return;
				}
				UCurveBase* Curve = Cast<UCurveBase>(ResolveAsset(Path));
				if (Curve == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("curve_not_found"),
						FString::Printf(
							TEXT("no Curve asset at '%s' — asset_ops create makes one (CurveFloat, ")
							TEXT("CurveVector, CurveLinearColor)"),
							*Path));
					return;
				}
				TArray<FRichCurve*> Curves = CurvesOf(Curve);
				if (Curves.IsEmpty())
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unsupported_curve"),
						FString::Printf(
							TEXT("'%s' is a %s — only CurveFloat, CurveVector and CurveLinearColor have ")
							TEXT("editable rich curves here"),
							*Path, *Curve->GetClass()->GetName()));
					return;
				}
				const int32 Channel = IntOr(Body, TEXT("channel"), 0);
				if (!Curves.IsValidIndex(Channel))
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_channel"),
						FString::Printf(TEXT("'channel' must be 0..%d for a %s"),
							Curves.Num() - 1, *Curve->GetClass()->GetName()));
					return;
				}

				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("curve"), Curve->GetPathName());
					Data->SetStringField(TEXT("class"), Curve->GetClass()->GetName());
					TArray<TSharedPtr<FJsonValue>> Channels;
					for (int32 Index = 0; Index < Curves.Num(); ++Index)
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetNumberField(TEXT("channel"), Index);
						Item->SetStringField(TEXT("name"), ChannelName(Curve, Index));
						TArray<TSharedPtr<FJsonValue>> Keys;
						for (auto It = Curves[Index]->GetKeyHandleIterator(); It; ++It)
						{
							const FRichCurveKey& Key = Curves[Index]->GetKey(*It);
							const TSharedRef<FJsonObject> KeyJson = MakeShared<FJsonObject>();
							KeyJson->SetNumberField(TEXT("time"), Key.Time);
							KeyJson->SetNumberField(TEXT("value"), Key.Value);
							KeyJson->SetStringField(TEXT("interp"), InterpName(Key.InterpMode));
							Keys.Add(MakeShared<FJsonValueObject>(KeyJson));
						}
						Item->SetArrayField(TEXT("keys"), Keys);
						Channels.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetArrayField(TEXT("channels"), Channels);
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditCurve", "McpLink Edit Curve"));
				Curve->Modify();
				FRichCurve& Target = *Curves[Channel];

				if (Operation == TEXT("set_keys") || Operation == TEXT("add_keys"))
				{
					const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
					if (!Body->TryGetArrayField(TEXT("keys"), Keys) || Keys == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'keys' must be an array of {\"time\": t, \"value\": v, \"interp\": ")
							TEXT("\"cubic\"|\"linear\"|\"constant\"}"));
						return;
					}
					if (Operation == TEXT("set_keys"))
					{
						Target.Reset();
					}
					FString DefaultInterp = TEXT("cubic");
					Body->TryGetStringField(TEXT("interp"), DefaultInterp);
					int32 Added = 0;
					for (const TSharedPtr<FJsonValue>& Value : *Keys)
					{
						const TSharedPtr<FJsonObject>* Object = nullptr;
						if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object->IsValid())
						{
							continue;
						}
						const TSharedRef<FJsonObject> Key = Object->ToSharedRef();
						const float Time = DoubleOr(Key, TEXT("time"), 0.0);
						const float KeyValue = DoubleOr(Key, TEXT("value"), 0.0);
						FString Interp = DefaultInterp;
						Key->TryGetStringField(TEXT("interp"), Interp);
						const FKeyHandle Handle = Target.AddKey(Time, KeyValue);
						Target.SetKeyInterpMode(Handle, InterpFromName(Interp));
						++Added;
					}
					if (Added == 0)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("no_keys"),
							TEXT("'keys' held no {\"time\", \"value\"} objects"));
						return;
					}
					Target.AutoSetTangents();
					Curve->PostEditChange();
					Curve->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("curve"), Curve->GetPathName());
					Data->SetNumberField(TEXT("channel"), Channel);
					Data->SetNumberField(TEXT("keys_added"), Added);
					Data->SetNumberField(TEXT("key_count"), Target.GetNumKeys());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("clear"))
				{
					const int32 Before = Target.GetNumKeys();
					Target.Reset();
					Curve->PostEditChange();
					Curve->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("removed"), Before);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Curve, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use info, set_keys, add_keys, clear, or save"),
						*Operation));
			});
	}
}
