// Sequencer authoring for Level Sequences and UMG widget animations, which are
// the same MovieScene machinery: bindings, tracks, sections, keys.
//
// Everything a section or track exposes as a UPROPERTY (a skeletal animation
// section's Params.Animation, an audio section's Sound, easing) is edited with
// set_property on the reported `path` — this route only owns the structure and
// the keyframes, which have no reflection surface.

#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "McpSequenceUtils.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSection.h"
#include "MovieSceneTimeHelpers.h"
#include "MovieSceneTrack.h"
#include "ScopedTransaction.h"
#include "Sections/MovieSceneCameraCutSection.h"
#include "Sections/MovieSceneVectorSection.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneEnumTrack.h"
#include "Tracks/MovieSceneObjectPropertyTrack.h"
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "WidgetBlueprint.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace
	{
		using namespace McpLink::Sequences;

		UMovieSceneSequence* SequenceOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("sequence"), Path, Responder,
				TEXT("a Level Sequence path (/Game/Cine/LS_Shot) or a widget animation ")
				TEXT("(/Game/UI/WBP_Menu:Anim_FadeIn)")))
			{
				return nullptr;
			}
			UMovieSceneSequence* Sequence = Find(Path);
			if (Sequence == nullptr || Sequence->GetMovieScene() == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("sequence_not_found"),
					FString::Printf(
						TEXT("no sequence at '%s' — create makes a Level Sequence, ")
						TEXT("create_widget_animation makes a widget animation"), *Path));
				return nullptr;
			}
			return Sequence;
		}

		/// A Widget Blueprint for the widget-animation operations.
		UWidgetBlueprint* WidgetBlueprintOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("widget_blueprint"), Path, Responder,
				TEXT("a Widget Blueprint asset path, e.g. /Game/UI/WBP_Menu")))
			{
				return nullptr;
			}
			UObject* Object = ResolveObject(Path);
			if (Object == nullptr && !Path.Contains(TEXT(".")))
			{
				Object = ResolveObject(FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path)));
			}
			UWidgetBlueprint* Blueprint = Cast<UWidgetBlueprint>(Object);
			if (Blueprint == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("widget_blueprint_not_found"),
					FString::Printf(TEXT("no Widget Blueprint at '%s'"), *Path));
			}
			return Blueprint;
		}

		/// The animation named by `animation` on a Widget Blueprint.
		UWidgetAnimation* FindAnimation(const UWidgetBlueprint& Blueprint, const FString& Name)
		{
			for (UWidgetAnimation* Animation : Blueprint.Animations)
			{
				if (Animation != nullptr
					&& (Animation->GetName() == Name || Animation->GetDisplayLabel() == Name))
				{
					return Animation;
				}
			}
			return nullptr;
		}

		FGuid BindingOrError(
			const UMovieScene& MovieScene, const TSharedRef<FJsonObject>& Body, const TCHAR* Field,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			Body->TryGetStringField(Field, Spec);
			const FGuid Guid = Spec.IsEmpty() ? FGuid() : FindBinding(MovieScene, Spec);
			if (!Guid.IsValid())
			{
				TArray<FString> Names;
				for (const FMovieSceneBinding& Binding : MovieScene.GetBindings())
				{
					Names.Add(MovieScene.GetObjectDisplayName(Binding.GetObjectGuid()).ToString());
				}
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("binding_not_found"),
					Names.Num() > 0
						? FString::Printf(TEXT("'%s' is not a binding in this sequence — it has: %s"),
							*Spec, *FString::Join(Names, TEXT(", ")))
						: FString::Printf(
							TEXT("'%s' is not a binding — this sequence has none yet, add_binding makes one"),
							*Spec));
			}
			return Guid;
		}

		UMovieSceneTrack* TrackOrError(
			const UMovieScene& MovieScene, const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			Body->TryGetStringField(TEXT("track"), Spec);
			UMovieSceneTrack* Track = FindTrack(MovieScene, Spec);
			if (Track == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("track_not_found"),
					FString::Printf(TEXT("no track '%s' in this sequence — inspect reports each track's path"),
						*Spec));
			}
			return Track;
		}

		UMovieSceneSection* SectionOrError(
			const UMovieScene& MovieScene, const TSharedRef<FJsonObject>& Body,
			const TSharedRef<FMcpResponder>& Responder)
		{
			FString Spec;
			Body->TryGetStringField(TEXT("section"), Spec);
			TArray<FString> Ambiguous;
			UMovieSceneSection* Section = FindSection(MovieScene, Spec, Ambiguous);
			if (Section == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("section_not_found"),
					Ambiguous.Num() > 1
						? FString::Printf(TEXT("'%s' names %d sections — use one of these paths: %s"),
							*Spec, Ambiguous.Num(), *FString::Join(Ambiguous, TEXT(", ")))
						: FString::Printf(
							TEXT("no section '%s' in this sequence — inspect reports each section's path"),
							*Spec));
			}
			return Section;
		}

		/// Walk a dotted property path ("RelativeLocation.X") from a class.
		const FProperty* FindPropertyByPath(const UStruct* Owner, const FString& Path)
		{
			const FProperty* Property = nullptr;
			TArray<FString> Segments;
			Path.ParseIntoArray(Segments, TEXT("."));
			for (const FString& Segment : Segments)
			{
				if (Owner == nullptr)
				{
					return nullptr;
				}
				Property = FindFProperty<FProperty>(Owner, *Segment);
				if (Property == nullptr)
				{
					return nullptr;
				}
				const FStructProperty* AsStruct = CastField<FStructProperty>(Property);
				Owner = AsStruct != nullptr ? AsStruct->Struct : nullptr;
			}
			return Property;
		}

		/// The class a binding animates, for property lookup.
		const UClass* BoundClass(UMovieScene& MovieScene, const FGuid& Guid)
		{
			if (const FMovieScenePossessable* Possessable = MovieScene.FindPossessable(Guid))
			{
				return Possessable->GetPossessedObjectClass();
			}
			if (const FMovieSceneSpawnable* Spawnable = MovieScene.FindSpawnable(Guid))
			{
				return Spawnable->GetObjectTemplate() != nullptr
					? Spawnable->GetObjectTemplate()->GetClass()
					: nullptr;
			}
			return nullptr;
		}

		/// Attach a fresh section spanning `Range` to a track.
		UMovieSceneSection* AddSectionTo(UMovieSceneTrack& Track, const TRange<FFrameNumber>& Range, int32 RowIndex)
		{
			UMovieSceneSection* Section = Track.CreateNewSection();
			if (Section == nullptr)
			{
				return nullptr;
			}
			Section->SetRange(Range);
			Section->SetRowIndex(RowIndex);
			Track.AddSection(*Section);
			return Section;
		}

		void MarkSequenceChanged(UMovieSceneSequence& Sequence)
		{
			Sequence.GetMovieScene()->MarkAsChanged();
			Sequence.MarkPackageDirty();
		}
	}

	void RegisterSequenceRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/sequences/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_track_classes"))
				{
					FString Filter;
					Body->TryGetStringField(TEXT("filter"), Filter);
					TArray<TSharedPtr<FJsonValue>> Classes;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						UClass* Class = *It;
						if (!Class->IsChildOf(UMovieSceneTrack::StaticClass())
							|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
						{
							continue;
						}
						const FString Name = Class->GetName();
						if (!Filter.IsEmpty() && !Name.Contains(Filter))
						{
							continue;
						}
						const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
						Json->SetStringField(TEXT("name"), Name);
						Json->SetStringField(TEXT("path"), Class->GetPathName());
						Json->SetBoolField(TEXT("property_track"),
							Class->IsChildOf(UMovieScenePropertyTrack::StaticClass()));
						Classes.Add(MakeShared<FJsonValueObject>(Json));
					}
					Classes.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
					{
						return A->AsObject()->GetStringField(TEXT("name"))
							< B->AsObject()->GetStringField(TEXT("name"));
					});
					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetArrayField(TEXT("classes"), Classes);
					Responder->Ok(Result);
					return;
				}

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
						TEXT("where to put the asset, e.g. /Game/Cine/LS_Shot")))
					{
						return;
					}
					double DisplayFps = 30.0;
					Body->TryGetNumberField(TEXT("display_rate"), DisplayFps);
					double TickFps = 24000.0;
					Body->TryGetNumberField(TEXT("tick_resolution"), TickFps);
					if (DisplayFps <= 0.0 || TickFps <= 0.0)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_rate"),
							TEXT("display_rate and tick_resolution are frames per second and must be positive"));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateSequence", "McpLink Create Level Sequence"));
					FString Error;
					ULevelSequence* Sequence = Create(
						Path,
						FFrameRate(FMath::RoundToInt(DisplayFps * 1000.0), 1000),
						FFrameRate(FMath::RoundToInt(TickFps), 1),
						Error);
					if (Sequence == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("create_failed"), Error);
						return;
					}

					UMovieScene* MovieScene = Sequence->GetMovieScene();
					FFrameNumber Start(0);
					ReadTime(Body, *MovieScene, TEXT("start_frame"), TEXT("start_seconds"), Start);
					FFrameNumber End = TickFromSeconds(*MovieScene, 5.0) + Start;
					ReadTime(Body, *MovieScene, TEXT("end_frame"), TEXT("end_seconds"), End);
					double DurationSeconds = 0.0;
					if (Body->TryGetNumberField(TEXT("duration_seconds"), DurationSeconds))
					{
						End = Start + TickFromSeconds(*MovieScene, DurationSeconds);
					}
					MovieScene->SetPlaybackRange(TRange<FFrameNumber>(Start, FMath::Max(End, Start + 1)));
					MarkSequenceChanged(*Sequence);
					Responder->Ok(SequenceToJson(*Sequence, false));
					return;
				}

				if (Operation == TEXT("list_widget_animations")
					|| Operation == TEXT("create_widget_animation")
					|| Operation == TEXT("remove_widget_animation"))
				{
					UWidgetBlueprint* Blueprint = WidgetBlueprintOrError(Body, Responder);
					if (Blueprint == nullptr) { return; }

					if (Operation == TEXT("list_widget_animations"))
					{
						TArray<TSharedPtr<FJsonValue>> Animations;
						for (UWidgetAnimation* Animation : Blueprint->Animations)
						{
							if (Animation != nullptr)
							{
								Animations.Add(MakeShared<FJsonValueObject>(SequenceToJson(*Animation, false)));
							}
						}
						const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
						Result->SetStringField(TEXT("widget_blueprint"), Blueprint->GetPathName());
						Result->SetArrayField(TEXT("animations"), Animations);
						Responder->Ok(Result);
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "EditWidgetAnimations", "McpLink Edit Widget Animations"),
						ShouldTransact(Blueprint));

					FString Name;
					if (!RequireString(Body, Operation == TEXT("create_widget_animation")
							? TEXT("name") : TEXT("animation"),
						Name, Responder, TEXT("the animation's name")))
					{
						return;
					}

					if (Operation == TEXT("remove_widget_animation"))
					{
						UWidgetAnimation* Animation = FindAnimation(*Blueprint, Name);
						if (Animation == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("animation_not_found"),
								FString::Printf(TEXT("'%s' has no animation called '%s'"),
									*Blueprint->GetName(), *Name));
							return;
						}
						Blueprint->Modify();
						Blueprint->Animations.Remove(Animation);
						FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
						const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
						Result->SetStringField(TEXT("removed"), Name);
						Result->SetStringField(TEXT("widget_blueprint"), Blueprint->GetPathName());
						Responder->Ok(Result);
						return;
					}

					double Fps = 60.0;
					Body->TryGetNumberField(TEXT("display_rate"), Fps);
					double DurationSeconds = 5.0;
					Body->TryGetNumberField(TEXT("duration_seconds"), DurationSeconds);
					if (Fps <= 0.0)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_rate"),
							TEXT("display_rate is frames per second and must be positive"));
						return;
					}
					FString Error;
					UWidgetAnimation* Animation = CreateWidgetAnimation(
						Blueprint, Name, FFrameRate(FMath::RoundToInt(Fps * 1000.0), 1000),
						DurationSeconds, Error);
					if (Animation == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("create_failed"), Error);
						return;
					}
					Responder->Ok(SequenceToJson(*Animation, false));
					return;
				}

				UMovieSceneSequence* Sequence = SequenceOrError(Body, Responder);
				if (Sequence == nullptr)
				{
					return;
				}
				UMovieScene& MovieScene = *Sequence->GetMovieScene();
				bool bIncludeKeys = false;
				Body->TryGetBoolField(TEXT("include_keys"), bIncludeKeys);

				if (Operation == TEXT("inspect"))
				{
					Responder->Ok(SequenceToJson(*Sequence, bIncludeKeys));
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename;
					FString Error;
					if (!SaveAsset(Sequence, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetStringField(TEXT("sequence"), Sequence->GetPathName());
					Result->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Result);
					return;
				}

				// Everything below mutates the asset. Nothing transacts while PIE is
				// running: add_to_level spawns into the play world, and anything
				// from that world recorded in the undo buffer keeps it alive past
				// EndPlayMap, which asserts. Same rule as call_function.
				const bool bPieRunning = GEditor != nullptr && GEditor->PlayWorld != nullptr;
				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditSequence", "McpLink Edit Level Sequence"),
					ShouldTransact(Sequence) && !bPieRunning);
				Sequence->Modify();
				MovieScene.Modify();

				if (Operation == TEXT("set_playback_range"))
				{
					MovieScene.SetPlaybackRange(ReadRange(Body, MovieScene));
					MarkSequenceChanged(*Sequence);
					Responder->Ok(SequenceToJson(*Sequence, false));
					return;
				}

				if (Operation == TEXT("set_display_rate"))
				{
					double Fps = 0.0;
					if (!Body->TryGetNumberField(TEXT("display_rate"), Fps) || Fps <= 0.0)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'display_rate' is required (frames per second, e.g. 24 or 29.97)"));
						return;
					}
					MovieScene.SetDisplayRate(FFrameRate(FMath::RoundToInt(Fps * 1000.0), 1000));
					MarkSequenceChanged(*Sequence);
					Responder->Ok(SequenceToJson(*Sequence, false));
					return;
				}

				if (Operation == TEXT("add_marked_frame"))
				{
					FFrameNumber Tick(0);
					if (!ReadTime(Body, MovieScene, TEXT("frame"), TEXT("seconds"), Tick))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
							TEXT("'frame' (display-rate frames) or 'seconds' is required"));
						return;
					}
					FString Label;
					Body->TryGetStringField(TEXT("label"), Label);
					FMovieSceneMarkedFrame Mark(Tick);
					Mark.Label = Label;
					MovieScene.AddMarkedFrame(Mark);
					MarkSequenceChanged(*Sequence);
					Responder->Ok(SequenceToJson(*Sequence, false));
					return;
				}

				if (Operation == TEXT("add_widget_binding"))
				{
					UWidgetAnimation* Animation = Cast<UWidgetAnimation>(Sequence);
					if (Animation == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("not_a_widget_animation"),
							TEXT("add_widget_binding only applies to widget animations — use add_binding to ")
							TEXT("possess an actor in a Level Sequence"));
						return;
					}
					FString WidgetName;
					if (!RequireString(Body, TEXT("widget"), WidgetName, Responder,
						TEXT("a widget of the animation's own Widget Blueprint, or \"Self\"")))
					{
						return;
					}
					FString Error;
					const FGuid Guid = BindWidget(*Animation, WidgetName, Error);
					if (!Guid.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("widget_not_found"), Error);
						return;
					}
					MarkSequenceChanged(*Sequence);
					Responder->Ok(BindingToJson(MovieScene, Guid, false));
					return;
				}

				if (Operation == TEXT("add_binding") || Operation == TEXT("add_camera_cut")
					|| Operation == TEXT("add_to_level"))
				{
					if (Cast<UWidgetAnimation>(Sequence) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("not_a_level_sequence"),
							FString::Printf(
								TEXT("'%s' works on Level Sequences, not widget animations — bind widgets ")
								TEXT("with add_widget_binding"), *Operation));
						return;
					}
				}

				if (Operation == TEXT("add_binding"))
				{
					UWorld* World = ResolveWorld(Body);
					FString ActorSpec;
					if (!RequireString(Body, TEXT("actor"), ActorSpec, Responder,
						TEXT("an actor path or editor label to animate")))
					{
						return;
					}
					AActor* Actor = World != nullptr ? ResolveActor(World, ActorSpec) : nullptr;
					if (Actor == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
							FString::Printf(TEXT("no actor '%s' in the %s world"), *ActorSpec,
								World != nullptr ? TEXT("selected") : TEXT("requested")));
						return;
					}

					UObject* ToBind = Actor;
					FString ComponentName;
					if (Body->TryGetStringField(TEXT("component"), ComponentName) && !ComponentName.IsEmpty())
					{
						ToBind = nullptr;
						for (UActorComponent* Component : Actor->GetComponents())
						{
							if (Component != nullptr && Component->GetName() == ComponentName)
							{
								ToBind = Component;
								break;
							}
						}
						if (ToBind == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("component_not_found"),
								FString::Printf(TEXT("'%s' has no component named '%s'"),
									*Actor->GetActorNameOrLabel(), *ComponentName));
							return;
						}
					}

					bool bSpawnable = false;
					Body->TryGetBoolField(TEXT("spawnable"), bSpawnable);
					UMovieSceneSequence* AsSequence = Sequence;
					const FGuid Guid = bSpawnable
						? AsSequence->CreateSpawnable(ToBind)
						: AsSequence->CreatePossessable(ToBind);
					if (!Guid.IsValid())
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bind_failed"),
							FString::Printf(TEXT("could not bind '%s' into this sequence"), *ToBind->GetName()));
						return;
					}
					// A spawnable is named after its object template, which is not
					// the label the caller just used to find the actor — make both
					// paths addressable by the same name.
					if (bSpawnable)
					{
						const FString Label = ComponentName.IsEmpty()
							? Actor->GetActorNameOrLabel()
							: ComponentName;
						MovieScene.SetObjectDisplayName(Guid, FText::FromString(Label));
					}
					MarkSequenceChanged(*Sequence);
					Responder->Ok(BindingToJson(MovieScene, Guid, false));
					return;
				}

				if (Operation == TEXT("remove_binding"))
				{
					const FGuid Guid = BindingOrError(MovieScene, Body, TEXT("binding"), Responder);
					if (!Guid.IsValid()) { return; }
					if (!MovieScene.RemovePossessable(Guid))
					{
						MovieScene.RemoveSpawnable(Guid);
					}
					MarkSequenceChanged(*Sequence);
					Responder->Ok(SequenceToJson(*Sequence, false));
					return;
				}

				if (Operation == TEXT("set_binding_name"))
				{
					const FGuid Guid = BindingOrError(MovieScene, Body, TEXT("binding"), Responder);
					if (!Guid.IsValid()) { return; }
					FString Name;
					if (!RequireString(Body, TEXT("name"), Name, Responder, TEXT("the display name to use")))
					{
						return;
					}
					MovieScene.SetObjectDisplayName(Guid, FText::FromString(Name));
					if (FMovieScenePossessable* Possessable = MovieScene.FindPossessable(Guid))
					{
						Possessable->SetName(Name);
					}
					MarkSequenceChanged(*Sequence);
					Responder->Ok(BindingToJson(MovieScene, Guid, false));
					return;
				}

				if (Operation == TEXT("add_track"))
				{
					FString ClassSpec;
					if (!RequireString(Body, TEXT("track"), ClassSpec, Responder,
						TEXT("a track class such as Transform, SkeletalAnimation or CameraCut")))
					{
						return;
					}
					UClass* Class = ResolveTrackClass(ClassSpec);
					if (Class == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("track_class_not_found"),
							FString::Printf(
								TEXT("'%s' is not a concrete MovieSceneTrack class — see list_track_classes"),
								*ClassSpec));
						return;
					}

					if (Sequence->IsTrackSupported(Class) == ETrackSupport::NotSupported)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("track_not_supported"),
							FString::Printf(TEXT("%s does not support a %s"),
								*Sequence->GetClass()->GetName(), *Class->GetName()));
						return;
					}

					FString BindingSpec;
					Body->TryGetStringField(TEXT("binding"), BindingSpec);
					UMovieSceneTrack* Track = nullptr;
					if (Class->IsChildOf(UMovieSceneCameraCutTrack::StaticClass()))
					{
						Track = MovieScene.GetCameraCutTrack();
						if (Track == nullptr)
						{
							Track = MovieScene.AddCameraCutTrack(Class);
						}
					}
					else if (!BindingSpec.IsEmpty())
					{
						const FGuid Guid = BindingOrError(MovieScene, Body, TEXT("binding"), Responder);
						if (!Guid.IsValid()) { return; }
						Track = MovieScene.AddTrack(Class, Guid);
					}
					else
					{
						Track = MovieScene.AddTrack(Class);
					}
					if (Track == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("add_track_failed"),
							FString::Printf(TEXT("this sequence would not take a %s"), *Class->GetName()));
						return;
					}

					FString PropertyPath;
					if (Body->TryGetStringField(TEXT("property"), PropertyPath) && !PropertyPath.IsEmpty())
					{
						if (UMovieScenePropertyTrack* PropertyTrack = Cast<UMovieScenePropertyTrack>(Track))
						{
							FString Leaf = PropertyPath;
							PropertyPath.Split(TEXT("."), nullptr, &Leaf, ESearchCase::IgnoreCase,
								ESearchDir::FromEnd);
							PropertyTrack->SetPropertyNameAndPath(FName(*Leaf), PropertyPath);
						}
					}
					FString DisplayName;
					if (Body->TryGetStringField(TEXT("name"), DisplayName) && !DisplayName.IsEmpty())
					{
						if (UMovieSceneNameableTrack* Nameable = Cast<UMovieSceneNameableTrack>(Track))
						{
							Nameable->SetDisplayName(FText::FromString(DisplayName));
						}
					}

					bool bWithSection = true;
					Body->TryGetBoolField(TEXT("with_section"), bWithSection);
					if (bWithSection)
					{
						AddSectionTo(*Track, ReadRange(Body, MovieScene), 0);
					}
					MarkSequenceChanged(*Sequence);
					Responder->Ok(TrackToJson(MovieScene, *Track, false));
					return;
				}

				if (Operation == TEXT("add_property_track"))
				{
					const FGuid Guid = BindingOrError(MovieScene, Body, TEXT("binding"), Responder);
					if (!Guid.IsValid()) { return; }
					FString PropertyPath;
					if (!RequireString(Body, TEXT("property"), PropertyPath, Responder,
						TEXT("a property on the bound object, e.g. RelativeLocation or Intensity")))
					{
						return;
					}
					const UClass* Class = BoundClass(MovieScene, Guid);
					if (Class == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_bound_class"),
							TEXT("this binding does not record the class it animates — use add_track with an ")
							TEXT("explicit track class"));
						return;
					}
					const FProperty* Property = FindPropertyByPath(Class, PropertyPath);
					if (Property == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("property_not_found"),
							FString::Printf(TEXT("%s has no property '%s'"), *Class->GetName(), *PropertyPath));
						return;
					}
					int32 NumChannels = 0;
					UEnum* Enum = nullptr;
					UClass* ObjectClass = nullptr;
					UClass* TrackClass = PropertyTrackClass(Property, NumChannels, Enum, ObjectClass);
					if (TrackClass == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotSupported, TEXT("property_not_animatable"),
							FString::Printf(
								TEXT("'%s' is a %s, which Sequencer has no property track for"),
								*PropertyPath, *Property->GetClass()->GetName()));
						return;
					}

					UMovieSceneTrack* Track = MovieScene.AddTrack(TrackClass, Guid);
					if (Track == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("add_track_failed"),
							FString::Printf(TEXT("this sequence would not take a %s"), *TrackClass->GetName()));
						return;
					}
					FString Leaf = PropertyPath;
					PropertyPath.Split(TEXT("."), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
					Cast<UMovieScenePropertyTrack>(Track)->SetPropertyNameAndPath(FName(*Leaf), PropertyPath);
					if (UMovieSceneEnumTrack* EnumTrack = Cast<UMovieSceneEnumTrack>(Track))
					{
						EnumTrack->SetEnum(Enum);
					}
					if (UMovieSceneObjectPropertyTrack* ObjectTrack = Cast<UMovieSceneObjectPropertyTrack>(Track))
					{
						ObjectTrack->PropertyClass = ObjectClass;
					}
					if (UMovieSceneDoubleVectorTrack* Vector = Cast<UMovieSceneDoubleVectorTrack>(Track))
					{
						Vector->SetNumChannelsUsed(NumChannels);
					}
					if (UMovieSceneFloatVectorTrack* Vector = Cast<UMovieSceneFloatVectorTrack>(Track))
					{
						Vector->SetNumChannelsUsed(NumChannels);
					}

					if (UMovieSceneSection* Section = AddSectionTo(*Track, ReadRange(Body, MovieScene), 0))
					{
						if (UMovieSceneDoubleVectorSection* Vector = Cast<UMovieSceneDoubleVectorSection>(Section))
						{
							Vector->SetChannelsUsed(NumChannels);
						}
						if (UMovieSceneFloatVectorSection* Vector = Cast<UMovieSceneFloatVectorSection>(Section))
						{
							Vector->SetChannelsUsed(NumChannels);
						}
					}
					MarkSequenceChanged(*Sequence);
					Responder->Ok(TrackToJson(MovieScene, *Track, false));
					return;
				}

				if (Operation == TEXT("remove_track"))
				{
					UMovieSceneTrack* Track = TrackOrError(MovieScene, Body, Responder);
					if (Track == nullptr) { return; }
					if (Track == MovieScene.GetCameraCutTrack())
					{
						MovieScene.RemoveCameraCutTrack();
					}
					else
					{
						MovieScene.RemoveTrack(*Track);
					}
					MarkSequenceChanged(*Sequence);
					Responder->Ok(SequenceToJson(*Sequence, false));
					return;
				}

				if (Operation == TEXT("add_section"))
				{
					UMovieSceneTrack* Track = TrackOrError(MovieScene, Body, Responder);
					if (Track == nullptr) { return; }
					double RowIndex = 0.0;
					Body->TryGetNumberField(TEXT("row_index"), RowIndex);
					UMovieSceneSection* Section =
						AddSectionTo(*Track, ReadRange(Body, MovieScene), static_cast<int32>(RowIndex));
					if (Section == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("add_section_failed"),
							FString::Printf(TEXT("%s does not create sections"), *Track->GetClass()->GetName()));
						return;
					}
					MarkSequenceChanged(*Sequence);
					Responder->Ok(SectionToJson(MovieScene, *Section, false));
					return;
				}

				if (Operation == TEXT("set_section_range"))
				{
					UMovieSceneSection* Section = SectionOrError(MovieScene, Body, Responder);
					if (Section == nullptr) { return; }
					Section->Modify();
					Section->SetRange(ReadRange(Body, MovieScene));
					MarkSequenceChanged(*Sequence);
					Responder->Ok(SectionToJson(MovieScene, *Section, false));
					return;
				}

				if (Operation == TEXT("remove_section"))
				{
					UMovieSceneSection* Section = SectionOrError(MovieScene, Body, Responder);
					if (Section == nullptr) { return; }
					UMovieSceneTrack* Track = Section->GetTypedOuter<UMovieSceneTrack>();
					if (Track == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_track"),
							TEXT("that section is not owned by a track"));
						return;
					}
					Track->Modify();
					Track->RemoveSection(*Section);
					MarkSequenceChanged(*Sequence);
					Responder->Ok(TrackToJson(MovieScene, *Track, false));
					return;
				}

				if (Operation == TEXT("add_camera_cut"))
				{
					const FGuid Guid = BindingOrError(MovieScene, Body, TEXT("camera_binding"), Responder);
					if (!Guid.IsValid()) { return; }
					UMovieSceneTrack* Track = MovieScene.GetCameraCutTrack();
					if (Track == nullptr)
					{
						Track = MovieScene.AddCameraCutTrack(UMovieSceneCameraCutTrack::StaticClass());
					}
					UMovieSceneCameraCutSection* Section =
						Cast<UMovieSceneCameraCutSection>(AddSectionTo(*Track, ReadRange(Body, MovieScene), 0));
					if (Section == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("add_section_failed"),
							TEXT("the camera cut track would not create a section"));
						return;
					}
					Section->SetCameraGuid(Guid);
					MarkSequenceChanged(*Sequence);
					Responder->Ok(SectionToJson(MovieScene, *Section, false));
					return;
				}

				if (Operation == TEXT("add_key") || Operation == TEXT("set_channel_default")
					|| Operation == TEXT("remove_keys"))
				{
					UMovieSceneSection* Section = SectionOrError(MovieScene, Body, Responder);
					if (Section == nullptr) { return; }
					FString ChannelName;
					Body->TryGetStringField(TEXT("channel"), ChannelName);
					FName TypeName;
					FMovieSceneChannel* Channel = FindChannel(*Section, ChannelName, TypeName);
					if (Channel == nullptr)
					{
						TArray<FString> Names;
						for (const TSharedPtr<FJsonValue>& Entry :
							SectionToJson(MovieScene, *Section, false)->GetArrayField(TEXT("channels")))
						{
							Names.Add(Entry->AsObject()->GetStringField(TEXT("name")));
						}
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("channel_not_found"),
							FString::Printf(TEXT("'%s' is not a channel of this section — it has: %s"),
								*ChannelName, *FString::Join(Names, TEXT(", "))));
						return;
					}
					Section->Modify();

					if (Operation == TEXT("remove_keys"))
					{
						FFrameNumber Tick(0);
						if (ReadTime(Body, MovieScene, TEXT("frame"), TEXT("seconds"), Tick))
						{
							TArray<FFrameNumber> Times;
							TArray<FKeyHandle> Handles;
							Channel->GetKeys(TRange<FFrameNumber>(Tick, Tick + 1), &Times, &Handles);
							Channel->DeleteKeys(Handles);
						}
						else
						{
							Channel->Reset();
						}
						MarkSequenceChanged(*Sequence);
						Responder->Ok(SectionToJson(MovieScene, *Section, true));
						return;
					}

					if (Operation == TEXT("set_channel_default"))
					{
						FString Error;
						if (!SetChannelDefault(Channel, TypeName, Body->TryGetField(TEXT("value")), Error))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"), Error);
							return;
						}
						MarkSequenceChanged(*Sequence);
						Responder->Ok(SectionToJson(MovieScene, *Section, true));
						return;
					}

					// add_key: one key, or a batch under "keys".
					TArray<TSharedPtr<FJsonValue>> Keys;
					const TArray<TSharedPtr<FJsonValue>>* Batch = nullptr;
					if (Body->TryGetArrayField(TEXT("keys"), Batch))
					{
						Keys = *Batch;
					}
					else
					{
						Keys.Add(MakeShared<FJsonValueObject>(Body));
					}
					FString Interpolation;
					Body->TryGetStringField(TEXT("interpolation"), Interpolation);
					for (const TSharedPtr<FJsonValue>& Entry : Keys)
					{
						const TSharedPtr<FJsonObject> KeyBody = Entry.IsValid() ? Entry->AsObject() : nullptr;
						if (!KeyBody.IsValid())
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_key"),
								TEXT("each entry of 'keys' must be an object with a frame/seconds and a value"));
							return;
						}
						const TSharedRef<FJsonObject> KeyRef = KeyBody.ToSharedRef();
						FFrameNumber Tick(0);
						if (!ReadTime(KeyRef, MovieScene, TEXT("frame"), TEXT("seconds"), Tick))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
								TEXT("'frame' (display-rate frames) or 'seconds' is required for every key"));
							return;
						}
						FString KeyInterpolation = Interpolation;
						KeyRef->TryGetStringField(TEXT("interpolation"), KeyInterpolation);
						FString Error;
						if (!SetKey(Channel, TypeName, Tick, KeyRef->TryGetField(TEXT("value")),
							KeyInterpolation, Error))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_value"), Error);
							return;
						}
					}
					MarkSequenceChanged(*Sequence);
					Responder->Ok(SectionToJson(MovieScene, *Section, true));
					return;
				}

				if (Operation == TEXT("add_to_level"))
				{
					UWorld* World = ResolveWorld(Body);
					if (World == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_world"),
							TEXT("no world to spawn into — start PIE or open a level"));
						return;
					}
					FActorSpawnParameters Params;
					FString Name;
					if (Body->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
					{
						Params.Name = FName(*Name);
					}
					ALevelSequenceActor* Actor = World->SpawnActor<ALevelSequenceActor>(Params);
					if (Actor == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("spawn_failed"),
							TEXT("could not spawn a LevelSequenceActor"));
						return;
					}
					// The widget-animation guard above leaves only Level Sequences here.
					Actor->SetSequence(CastChecked<ULevelSequence>(Sequence));
					bool bAutoPlay = false;
					if (Body->TryGetBoolField(TEXT("auto_play"), bAutoPlay))
					{
						Actor->PlaybackSettings.bAutoPlay = bAutoPlay;
					}
					double LoopCount = 0.0;
					if (Body->TryGetNumberField(TEXT("loop_count"), LoopCount))
					{
						Actor->PlaybackSettings.LoopCount.Value = static_cast<int32>(LoopCount);
					}
					if (!Name.IsEmpty())
					{
						Actor->SetActorLabel(Name);
					}
					const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
					Result->SetStringField(TEXT("actor"), Actor->GetPathName());
					Result->SetStringField(TEXT("label"), Actor->GetActorNameOrLabel());
					Result->SetStringField(TEXT("sequence"), Sequence->GetPathName());
					Result->SetBoolField(TEXT("auto_play"), Actor->PlaybackSettings.bAutoPlay != 0);
					Responder->Ok(Result);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_track_classes, create, inspect, ")
						TEXT("set_playback_range, set_display_rate, add_marked_frame, add_binding, ")
						TEXT("remove_binding, set_binding_name, add_track, add_property_track, remove_track, ")
						TEXT("add_section, set_section_range, remove_section, add_camera_cut, add_key, ")
						TEXT("remove_keys, set_channel_default, add_to_level, save, ")
						TEXT("list_widget_animations, create_widget_animation, remove_widget_animation, ")
						TEXT("or add_widget_binding"),
						*Operation));
			});
	}
}
