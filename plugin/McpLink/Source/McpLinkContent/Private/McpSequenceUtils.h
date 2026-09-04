// MovieScene sequence authoring — Level Sequences and UMG widget animations,
// which are the same machinery: bindings, tracks, sections, channels and keys.
//
// Frame numbers crossing the wire are always in the sequence's *display rate*
// (what Sequencer's timeline shows); internally MovieScene stores everything in
// tick-resolution frames, so every conversion goes through the helpers here.
#pragma once

#include "CoreMinimal.h"
#include "Misc/FrameNumber.h"
#include "Misc/FrameRate.h"

class FJsonObject;
class FJsonValue;
class UEnum;
class ULevelSequence;
class UMovieScene;
class UMovieSceneSequence;
class UUserWidget;
class UWidgetAnimation;
class UWidgetBlueprint;
class UMovieSceneSection;
class UMovieSceneTrack;
struct FMovieSceneChannel;

namespace McpLink::Sequences
{
	/// A Level Sequence ("/Game/Cine/LS_Shot") or a widget animation
	/// ("/Game/UI/WBP_Menu:Anim_FadeIn"), by asset path or full object path.
	UMovieSceneSequence* Find(const FString& Spec);

	/// New Level Sequence asset, initialised the way ULevelSequenceFactoryNew does.
	ULevelSequence* Create(
		const FString& PackagePath, FFrameRate DisplayRate, FFrameRate TickResolution, FString& OutError);

	/// New animation on a Widget Blueprint, built the way the UMG animation
	/// panel builds one (movie scene, display rate, playback range, variable).
	UWidgetAnimation* CreateWidgetAnimation(
		UWidgetBlueprint* Blueprint,
		const FString& Name,
		FFrameRate DisplayRate,
		double DurationSeconds,
		FString& OutError);

	/// Bind a widget of the animation's own Widget Blueprint by name; "Self"
	/// binds the user widget itself. Invalid GUID when the widget is unknown.
	FGuid BindWidget(UWidgetAnimation& Animation, const FString& WidgetName, FString& OutError);

	// --- time ---------------------------------------------------------------

	FFrameNumber TickFromDisplayFrame(const UMovieScene& MovieScene, double DisplayFrame);
	FFrameNumber TickFromSeconds(const UMovieScene& MovieScene, double Seconds);
	double DisplayFrameFromTick(const UMovieScene& MovieScene, FFrameNumber Tick);
	double SecondsFromTick(const UMovieScene& MovieScene, FFrameNumber Tick);

	/// Read a time from `<FrameField>` (display-rate frames) or `<SecondsField>`.
	/// False when neither field is present.
	bool ReadTime(
		const TSharedRef<FJsonObject>& Body,
		const UMovieScene& MovieScene,
		const TCHAR* FrameField,
		const TCHAR* SecondsField,
		FFrameNumber& OutTick);

	/// Read `start_frame`/`start_seconds` + `end_frame`/`end_seconds`, falling
	/// back to the sequence's playback range for whichever end is absent.
	TRange<FFrameNumber> ReadRange(const TSharedRef<FJsonObject>& Body, const UMovieScene& MovieScene);

	// --- lookup -------------------------------------------------------------

	/// Binding by GUID string or by display name. Invalid GUID when not found.
	FGuid FindBinding(const UMovieScene& MovieScene, const FString& Spec);

	/// Track by full object path or by object name (names are unique per movie scene).
	UMovieSceneTrack* FindTrack(const UMovieScene& MovieScene, const FString& Spec);

	/// Section by full object path, or by object name when that name is unique
	/// across the sequence. OutAmbiguous lists the candidates when it is not.
	UMovieSceneSection* FindSection(
		const UMovieScene& MovieScene, const FString& Spec, TArray<FString>& OutAmbiguous);

	/// "Transform", "3DTransform", "MovieScene3DTransformTrack", "SkeletalAnimation",
	/// "CameraCut", or a full class path.
	UClass* ResolveTrackClass(const FString& Spec);

	/// The property track class that animates `Property`, plus the extra state
	/// such a track needs (vector channel count, enum, object class).
	UClass* PropertyTrackClass(
		const FProperty* Property, int32& OutNumChannels, UEnum*& OutEnum, UClass*& OutObjectClass);

	/// Channel by metadata name ("Location.X", "Intensity"); OutTypeName is the
	/// channel's struct name (e.g. "MovieSceneDoubleChannel").
	FMovieSceneChannel* FindChannel(UMovieSceneSection& Section, const FString& Name, FName& OutTypeName);

	// --- keys ---------------------------------------------------------------

	/// Add or replace the key at `Tick`. Interpolation ("auto", "linear",
	/// "constant", "cubic") only applies to float/double channels.
	bool SetKey(
		FMovieSceneChannel* Channel,
		FName TypeName,
		FFrameNumber Tick,
		const TSharedPtr<FJsonValue>& Value,
		const FString& Interpolation,
		FString& OutError);

	/// The value a channel falls back to outside its keys.
	bool SetChannelDefault(
		FMovieSceneChannel* Channel, FName TypeName, const TSharedPtr<FJsonValue>& Value, FString& OutError);

	// --- json ---------------------------------------------------------------

	TSharedRef<FJsonObject> RangeToJson(const UMovieScene& MovieScene, const TRange<FFrameNumber>& Range);
	TSharedRef<FJsonObject> ChannelToJson(
		const UMovieScene& MovieScene, const FMovieSceneChannel& Channel, FName TypeName,
		const FString& Name, bool bIncludeKeys);
	TSharedRef<FJsonObject> SectionToJson(
		const UMovieScene& MovieScene, UMovieSceneSection& Section, bool bIncludeKeys);
	TSharedRef<FJsonObject> TrackToJson(
		const UMovieScene& MovieScene, UMovieSceneTrack& Track, bool bIncludeKeys);
	TSharedRef<FJsonObject> BindingToJson(
		const UMovieScene& MovieScene, const FGuid& Guid, bool bIncludeKeys);
	TSharedRef<FJsonObject> SequenceToJson(UMovieSceneSequence& Sequence, bool bIncludeKeys);
}
