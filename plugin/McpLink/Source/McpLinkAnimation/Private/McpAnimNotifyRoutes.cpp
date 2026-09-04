// Notifies, notify tracks, sync markers and curves on animation assets.
//
// UAnimationBlueprintLibrary is the engine's own scripting surface for exactly
// this and it handles the bookkeeping (track validation, curve name
// registration on the skeleton, marker sorting) that hand-editing the arrays
// would get wrong — so this route is a thin, typed shell over it.

#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "AnimationBlueprintLibrary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "ScopedTransaction.h"

namespace McpLink
{
	namespace AnimNotifies
	{
		UAnimSequenceBase* SequenceOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("animation"), Path, Responder,
					TEXT("an Anim Sequence or Montage path, e.g. /Game/Anims/A_Run")))
			{
				return nullptr;
			}
			UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(ResolveAsset(Path));
			if (Sequence == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("animation_not_found"),
					FString::Printf(TEXT("no Anim Sequence or Montage at '%s'"), *Path));
			}
			return Sequence;
		}

		FName TrackOrDefault(const TSharedRef<FJsonObject>& Body)
		{
			FString Track;
			Body->TryGetStringField(TEXT("track"), Track);
			// UAnimationBlueprintLibrary uses this name for the track every
			// animation starts with.
			return Track.IsEmpty() ? FName(TEXT("1")) : FName(*Track);
		}
	}

	using namespace AnimNotifies;

	void RegisterAnimNotifyRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/anim/notifies"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				UAnimSequenceBase* Sequence = SequenceOrError(Body, Responder);
				if (Sequence == nullptr)
				{
					return;
				}

				if (Operation == TEXT("list"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("animation"), Sequence->GetPathName());
					Data->SetNumberField(TEXT("length"), Sequence->GetPlayLength());

					TArray<FName> TrackNames;
					UAnimationBlueprintLibrary::GetAnimationNotifyTrackNames(Sequence, TrackNames);
					TArray<TSharedPtr<FJsonValue>> Tracks;
					for (const FName& Name : TrackNames)
					{
						Tracks.Add(MakeShared<FJsonValueString>(Name.ToString()));
					}
					Data->SetArrayField(TEXT("tracks"), Tracks);

					TArray<TSharedPtr<FJsonValue>> Notifies;
					for (const FAnimNotifyEvent& Event : Sequence->Notifies)
					{
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("name"), Event.NotifyName.ToString());
						Item->SetNumberField(TEXT("time"), Event.GetTriggerTime());
						Item->SetNumberField(TEXT("duration"), Event.GetDuration());
						Item->SetStringField(TEXT("kind"),
							Event.NotifyStateClass != nullptr ? TEXT("state") : TEXT("notify"));
						const UObject* Instance = Event.NotifyStateClass != nullptr
							? static_cast<UObject*>(Event.NotifyStateClass)
							: static_cast<UObject*>(Event.Notify);
						if (Instance != nullptr)
						{
							Item->SetStringField(TEXT("class"), Instance->GetClass()->GetName());
							// Notify settings are ordinary properties, so
							// set_property on this path tunes them.
							Item->SetStringField(TEXT("path"), Instance->GetPathName());
						}
						Item->SetNumberField(TEXT("track"), Event.TrackIndex);
						Notifies.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetArrayField(TEXT("notifies"), Notifies);

					TArray<TSharedPtr<FJsonValue>> MarkerJson;
					// Sync markers exist on Anim Sequences only, not on montages.
					if (const UAnimSequence* AsSequence = Cast<UAnimSequence>(Sequence))
					{
						TArray<FName> Markers;
						UAnimationBlueprintLibrary::GetUniqueMarkerNames(AsSequence, Markers);
						for (const FName& Name : Markers)
						{
							MarkerJson.Add(MakeShared<FJsonValueString>(Name.ToString()));
						}
					}
					Data->SetArrayField(TEXT("sync_markers"), MarkerJson);

					TArray<FName> CurveNames;
					UAnimationBlueprintLibrary::GetAnimationCurveNames(
						Sequence, ERawCurveTrackTypes::RCT_Float, CurveNames);
					TArray<TSharedPtr<FJsonValue>> Curves;
					for (const FName& Name : CurveNames)
					{
						Curves.Add(MakeShared<FJsonValueString>(Name.ToString()));
					}
					Data->SetArrayField(TEXT("float_curves"), Curves);
					Responder->Ok(Data);
					return;
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditAnimNotifies", "McpLink Edit Animation Notifies"));
				Sequence->Modify();

				if (Operation == TEXT("add_track") || Operation == TEXT("remove_track"))
				{
					FString Track;
					if (!RequireString(Body, TEXT("track"), Track, Responder, TEXT("the notify track name")))
					{
						return;
					}
					if (Operation == TEXT("add_track"))
					{
						UAnimationBlueprintLibrary::AddAnimationNotifyTrack(
							Sequence, FName(*Track), FLinearColor::White);
					}
					else
					{
						UAnimationBlueprintLibrary::RemoveAnimationNotifyTrack(Sequence, FName(*Track));
					}
					TArray<FName> TrackNames;
					UAnimationBlueprintLibrary::GetAnimationNotifyTrackNames(Sequence, TrackNames);
					TArray<TSharedPtr<FJsonValue>> Tracks;
					for (const FName& Name : TrackNames)
					{
						Tracks.Add(MakeShared<FJsonValueString>(Name.ToString()));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("tracks"), Tracks);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_notify"))
				{
					const double Time = DoubleOr(Body, TEXT("time"), 0.0);
					const double Duration = DoubleOr(Body, TEXT("duration"), 0.0);
					FString ClassName;
					Body->TryGetStringField(TEXT("class"), ClassName);
					const FName Track = TrackOrDefault(Body);

					UObject* Created = nullptr;
					if (!ClassName.IsEmpty())
					{
						UClass* Class = ResolveClass(ClassName);
						if (Class == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("class_not_found"),
								FString::Printf(TEXT("no class '%s'"), *ClassName));
							return;
						}
						if (Class->IsChildOf(UAnimNotifyState::StaticClass()))
						{
							Created = UAnimationBlueprintLibrary::AddAnimationNotifyStateEvent(
								Sequence, Track, Time, Duration, Class);
						}
						else if (Class->IsChildOf(UAnimNotify::StaticClass()))
						{
							Created = UAnimationBlueprintLibrary::AddAnimationNotifyEvent(
								Sequence, Track, Time, Class);
						}
						else
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_class"),
								FString::Printf(
									TEXT("'%s' is neither a UAnimNotify nor a UAnimNotifyState"), *ClassName));
							return;
						}
					}
					else
					{
						// No class: a bare named notify, which an Anim Blueprint
						// picks up as an AnimNotify_<Name> event.
						FString Name;
						if (!RequireString(Body, TEXT("name"), Name, Responder,
								TEXT("the notify name, or pass 'class' for a notify class")))
						{
							return;
						}
						UAnimationBlueprintLibrary::AddAnimationNotifyEvent(
							Sequence, Track, Time, nullptr);
						// AddAnimationNotifyEvent with no class leaves the name
						// unset, so name the event that was just appended.
						if (!Sequence->Notifies.IsEmpty())
						{
							Sequence->Notifies.Last().NotifyName = FName(*Name);
						}
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("animation"), Sequence->GetPathName());
					Data->SetStringField(TEXT("track"), Track.ToString());
					Data->SetNumberField(TEXT("time"), Time);
					Data->SetNumberField(TEXT("notify_count"), Sequence->Notifies.Num());
					if (Created != nullptr)
					{
						Data->SetStringField(TEXT("class"), Created->GetClass()->GetName());
						Data->SetStringField(TEXT("path"), Created->GetPathName());
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_notifies"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder,
							TEXT("the notify name to remove, from `list`")))
					{
						return;
					}
					const int32 Removed =
						UAnimationBlueprintLibrary::RemoveAnimationNotifyEventsByName(Sequence, FName(*Name));
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("removed"), Removed);
					Data->SetNumberField(TEXT("notify_count"), Sequence->Notifies.Num());
					Responder->Ok(Data);
					return;
				}

				// -------------------------------------- sync markers only ---
				UAnimSequence* AnimSequence = Cast<UAnimSequence>(Sequence);
				if (AnimSequence == nullptr
					&& (Operation == TEXT("add_sync_marker") || Operation == TEXT("remove_sync_markers")))
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("wrong_asset_type"),
						FString::Printf(TEXT("'%s' is a %s — sync markers need an Anim Sequence"),
							*Sequence->GetPathName(), *Sequence->GetClass()->GetName()));
					return;
				}

				if (Operation == TEXT("add_sync_marker"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the marker name")))
					{
						return;
					}
					UAnimationBlueprintLibrary::AddAnimationSyncMarker(
						AnimSequence, FName(*Name), DoubleOr(Body, TEXT("time"), 0.0), TrackOrDefault(Body));
					TArray<FName> Markers;
					UAnimationBlueprintLibrary::GetUniqueMarkerNames(AnimSequence, Markers);
					TArray<TSharedPtr<FJsonValue>> MarkerJson;
					for (const FName& Marker : Markers)
					{
						MarkerJson.Add(MakeShared<FJsonValueString>(Marker.ToString()));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("sync_markers"), MarkerJson);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_sync_markers"))
				{
					UAnimationBlueprintLibrary::RemoveAllAnimationSyncMarkers(AnimSequence);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("animation"), AnimSequence->GetPathName());
					Data->SetNumberField(TEXT("sync_markers"), 0);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_curve") || Operation == TEXT("remove_curve"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the curve name")))
					{
						return;
					}
					if (Operation == TEXT("add_curve"))
					{
						UAnimationBlueprintLibrary::AddCurve(Sequence, FName(*Name),
							ERawCurveTrackTypes::RCT_Float, /*bMetaDataCurve*/ false);
					}
					else
					{
						UAnimationBlueprintLibrary::RemoveCurve(
							Sequence, FName(*Name), /*bRemoveNameFromSkeleton*/ false);
					}
					TArray<FName> CurveNames;
					UAnimationBlueprintLibrary::GetAnimationCurveNames(
						Sequence, ERawCurveTrackTypes::RCT_Float, CurveNames);
					TArray<TSharedPtr<FJsonValue>> Curves;
					for (const FName& Curve : CurveNames)
					{
						Curves.Add(MakeShared<FJsonValueString>(Curve.ToString()));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("float_curves"), Curves);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_curve_keys") || Operation == TEXT("get_curve_keys"))
				{
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the curve name")))
					{
						return;
					}
					if (Operation == TEXT("get_curve_keys"))
					{
						TArray<float> Times, Values;
						UAnimationBlueprintLibrary::GetFloatKeys(Sequence, FName(*Name), Times, Values);
						TArray<TSharedPtr<FJsonValue>> Keys;
						for (int32 Index = 0; Index < Times.Num(); ++Index)
						{
							const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
							Item->SetNumberField(TEXT("time"), Times[Index]);
							Item->SetNumberField(TEXT("value"), Values.IsValidIndex(Index) ? Values[Index] : 0.f);
							Keys.Add(MakeShared<FJsonValueObject>(Item));
						}
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("curve"), Name);
						Data->SetArrayField(TEXT("keys"), Keys);
						Responder->Ok(Data);
						return;
					}

					const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
					if (!Body->TryGetArrayField(TEXT("keys"), Keys) || Keys == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'keys' must be an array of {\"time\": t, \"value\": v}"));
						return;
					}
					TArray<float> Times, Values;
					for (const TSharedPtr<FJsonValue>& Key : *Keys)
					{
						const TSharedPtr<FJsonObject>* Object = nullptr;
						if (!Key.IsValid() || !Key->TryGetObject(Object) || !Object->IsValid())
						{
							continue;
						}
						Times.Add(DoubleOr(Object->ToSharedRef(), TEXT("time"), 0.0));
						Values.Add(DoubleOr(Object->ToSharedRef(), TEXT("value"), 0.0));
					}
					if (Times.IsEmpty())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("no_keys"),
							TEXT("'keys' held no {\"time\", \"value\"} objects"));
						return;
					}
					UAnimationBlueprintLibrary::AddFloatCurveKeys(
						Sequence, FName(*Name), Times, Values);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("curve"), Name);
					Data->SetNumberField(TEXT("keys_added"), Times.Num());
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list, add_track, remove_track, add_notify, ")
						TEXT("remove_notifies, add_sync_marker, remove_sync_markers, add_curve, ")
						TEXT("remove_curve, add_curve_keys, or get_curve_keys"),
						*Operation));
			});
	}
}
