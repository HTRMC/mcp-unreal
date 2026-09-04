// End-to-end authoring of a Level Sequence: creation, time conversion, actor
// bindings, tracks picked from a property, sections, keys and the resolvers
// that address them.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Channels/MovieSceneChannel.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "LevelSequence.h"
#include "McpSequenceUtils.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequence.h"
#include "MovieSceneTimeHelpers.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneVectorTrack.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintOperationUtils.h"

namespace
{
	constexpr EAutomationTestFlags McpTestFlags =
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	TSharedPtr<FJsonValue> ArrayEntry(const TSharedRef<FJsonObject>& Object, const TCHAR* Field, int32 Index)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Object->TryGetArrayField(Field, Array) && Array->IsValidIndex(Index))
		{
			return (*Array)[Index];
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpSequenceAuthoringTest, "McpLink.Sequence.Authoring", McpTestFlags)
bool FMcpSequenceAuthoringTest::RunTest(const FString& Parameters)
{
	using namespace McpLink::Sequences;

	// IAssetTools refuses /Temp/ (a read-only mount root), so this has to live
	// under /Game — it is never saved, so nothing reaches the content folder.
	const FString PackagePath = FString::Printf(
		TEXT("/Game/McpLinkTests/LS_McpTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FString Error;
	ULevelSequence* Sequence = Create(PackagePath, FFrameRate(30, 1), FFrameRate(24000, 1), Error);
	if (!TestNotNull(*FString::Printf(TEXT("level sequence created: %s"), *Error), Sequence))
	{
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!TestNotNull(TEXT("movie scene exists"), MovieScene))
	{
		return false;
	}
	TestEqual(TEXT("display rate applied"), MovieScene->GetDisplayRate(), FFrameRate(30, 1));
	TestEqual(TEXT("tick resolution applied"), MovieScene->GetTickResolution(), FFrameRate(24000, 1));
	TestNull(TEXT("creating over an existing asset fails"),
		Create(PackagePath, FFrameRate(30, 1), FFrameRate(24000, 1), Error));
	TestTrue(TEXT("conflict message"), Error.Contains(TEXT("already exists")));
	TestEqual(TEXT("found by package path"), Find(PackagePath),
		static_cast<UMovieSceneSequence*>(Sequence));

	// ---- time: the wire speaks display-rate frames, MovieScene speaks ticks
	TestEqual(TEXT("frame 30 at 30fps is one second"), TickFromDisplayFrame(*MovieScene, 30.0).Value, 24000);
	TestEqual(TEXT("seconds convert to ticks"), TickFromSeconds(*MovieScene, 2.0).Value, 48000);
	TestEqual(TEXT("ticks convert back to frames"), DisplayFrameFromTick(*MovieScene, FFrameNumber(24000)), 30.0);
	TestEqual(TEXT("ticks convert back to seconds"), SecondsFromTick(*MovieScene, FFrameNumber(48000)), 2.0);

	const TSharedRef<FJsonObject> RangeBody = MakeShared<FJsonObject>();
	RangeBody->SetNumberField(TEXT("start_frame"), 0);
	RangeBody->SetNumberField(TEXT("end_seconds"), 3.0);
	const TRange<FFrameNumber> Range = ReadRange(RangeBody, *MovieScene);
	TestEqual(TEXT("range mixes frames and seconds"),
		UE::MovieScene::DiscreteExclusiveUpper(Range).Value, 72000);
	MovieScene->SetPlaybackRange(Range);

	// ---- bindings need a world to possess from
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("McpSequenceTestWorld"));
	if (!TestNotNull(TEXT("test world created"), World))
	{
		return false;
	}
	AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("actor spawned"), Actor))
	{
		World->DestroyWorld(false);
		return false;
	}

	UMovieSceneSequence* AsSequence = Sequence;
	const FGuid ActorGuid = AsSequence->CreatePossessable(Actor);
	TestTrue(TEXT("actor bound"), ActorGuid.IsValid());
	TestEqual(TEXT("binding found by guid"),
		FindBinding(*MovieScene, ActorGuid.ToString(EGuidFormats::DigitsWithHyphens)), ActorGuid);
	TestFalse(TEXT("unknown binding is invalid"), FindBinding(*MovieScene, TEXT("nope")).IsValid());

	const TSharedRef<FJsonObject> BindingJson = BindingToJson(*MovieScene, ActorGuid, false);
	TestEqual(TEXT("binding reports its kind"),
		BindingJson->GetStringField(TEXT("kind")), FString(TEXT("possessable")));
	TestTrue(TEXT("binding reports the class it animates"),
		BindingJson->GetStringField(TEXT("class")).Contains(TEXT("StaticMeshActor")));

	// ---- track classes
	TestEqual(TEXT("'Transform' means the actor transform track"),
		ResolveTrackClass(TEXT("Transform")), UMovieScene3DTransformTrack::StaticClass());
	TestEqual(TEXT("full class names resolve"),
		ResolveTrackClass(TEXT("MovieScene3DTransformTrack")), UMovieScene3DTransformTrack::StaticClass());
	TestNull(TEXT("unknown track class"), ResolveTrackClass(TEXT("NotATrack")));

	int32 NumChannels = 0;
	UEnum* Enum = nullptr;
	UClass* ObjectClass = nullptr;
	const FProperty* Location =
		FindFProperty<FProperty>(USceneComponent::StaticClass(), TEXT("RelativeLocation"));
	TestEqual(TEXT("a vector property animates as a double vector track"),
		PropertyTrackClass(Location, NumChannels, Enum, ObjectClass),
		UMovieSceneDoubleVectorTrack::StaticClass());
	TestEqual(TEXT("with three channels"), NumChannels, 3);
	const FProperty* Hidden = FindFProperty<FProperty>(AActor::StaticClass(), TEXT("bHidden"));
	TestNotNull(TEXT("a bool property is animatable"),
		PropertyTrackClass(Hidden, NumChannels, Enum, ObjectClass));
	TestNull(TEXT("an unsupported property reports no track"),
		PropertyTrackClass(FindFProperty<FProperty>(AActor::StaticClass(), TEXT("Tags")),
			NumChannels, Enum, ObjectClass));

	// ---- track, section, keys
	UMovieSceneTrack* Track = MovieScene->AddTrack(UMovieScene3DTransformTrack::StaticClass(), ActorGuid);
	if (!TestNotNull(TEXT("transform track added"), Track))
	{
		World->DestroyWorld(false);
		return false;
	}
	TestEqual(TEXT("track found by name"), FindTrack(*MovieScene, Track->GetName()), Track);
	TestEqual(TEXT("track found by path"), FindTrack(*MovieScene, Track->GetPathName()), Track);
	TestNull(TEXT("unknown track"), FindTrack(*MovieScene, TEXT("MovieSceneNoSuchTrack_0")));

	UMovieSceneSection* Section = Track->CreateNewSection();
	Section->SetRange(Range);
	Track->AddSection(*Section);

	TArray<FString> Ambiguous;
	TestEqual(TEXT("section found by path"),
		FindSection(*MovieScene, Section->GetPathName(), Ambiguous), Section);
	TestEqual(TEXT("section found by unique name"),
		FindSection(*MovieScene, Section->GetName(), Ambiguous), Section);

	FName ChannelType;
	FMovieSceneChannel* LocationX = FindChannel(*Section, TEXT("Location.X"), ChannelType);
	if (!TestNotNull(TEXT("Location.X channel exists"), LocationX))
	{
		World->DestroyWorld(false);
		return false;
	}
	TestEqual(TEXT("transform channels are doubles"),
		ChannelType, FMovieSceneDoubleChannel::StaticStruct()->GetFName());
	// A miss clears the out-param, so it must not scribble over ChannelType.
	FName MissedType;
	TestNull(TEXT("unknown channel"), FindChannel(*Section, TEXT("Nope"), MissedType));
	TestTrue(TEXT("a miss reports no type"), MissedType.IsNone());

	TestTrue(TEXT("first key set"),
		SetKey(LocationX, ChannelType, FFrameNumber(0), MakeShared<FJsonValueNumber>(0.0),
			TEXT("linear"), Error));
	TestTrue(TEXT("second key set"),
		SetKey(LocationX, ChannelType, TickFromDisplayFrame(*MovieScene, 30.0),
			MakeShared<FJsonValueNumber>(500.0), TEXT("linear"), Error));
	TestEqual(TEXT("two keys on the channel"), LocationX->GetNumKeys(), 2);

	double Midpoint = 0.0;
	TestTrue(TEXT("channel evaluates"),
		static_cast<FMovieSceneDoubleChannel*>(LocationX)
			->Evaluate(FFrameTime(TickFromDisplayFrame(*MovieScene, 15.0)), Midpoint));
	TestEqual(TEXT("linear keys interpolate halfway"), Midpoint, 250.0, 0.01);

	TestFalse(TEXT("a string is refused on a double channel"),
		SetKey(LocationX, ChannelType, FFrameNumber(0), MakeShared<FJsonValueString>(TEXT("nope")),
			TEXT("linear"), Error));
	TestTrue(TEXT("type message"), Error.Contains(TEXT("number")));
	TestFalse(TEXT("an unknown interpolation is refused"),
		SetKey(LocationX, ChannelType, FFrameNumber(0), MakeShared<FJsonValueNumber>(1.0),
			TEXT("sproing"), Error));
	TestTrue(TEXT("interpolation message"), Error.Contains(TEXT("linear")));

	// A bare section name is ambiguous once two tracks each hold one.
	UMovieSceneTrack* Second = MovieScene->AddTrack(UMovieScene3DTransformTrack::StaticClass(), ActorGuid);
	UMovieSceneSection* SecondSection = Second->CreateNewSection();
	SecondSection->SetRange(Range);
	Second->AddSection(*SecondSection);
	TestEqual(TEXT("duplicate section names collide"), SecondSection->GetName(), Section->GetName());
	TestNull(TEXT("an ambiguous section name resolves to nothing"),
		FindSection(*MovieScene, Section->GetName(), Ambiguous));
	TestEqual(TEXT("both candidates reported"), Ambiguous.Num(), 2);
	TestEqual(TEXT("paths still resolve"),
		FindSection(*MovieScene, Section->GetPathName(), Ambiguous), Section);

	// ---- reported shape
	const TSharedRef<FJsonObject> Json = SequenceToJson(*Sequence, true);
	TestEqual(TEXT("display rate reported"), Json->GetNumberField(TEXT("display_rate")), 30.0);
	TestEqual(TEXT("playback range reported in seconds"),
		Json->GetObjectField(TEXT("playback_range"))->GetNumberField(TEXT("end_seconds")), 3.0);
	const TSharedPtr<FJsonValue> FirstBinding = ArrayEntry(Json, TEXT("bindings"), 0);
	if (!TestTrue(TEXT("one binding reported"), FirstBinding.IsValid()))
	{
		World->DestroyWorld(false);
		return false;
	}
	const TSharedPtr<FJsonValue> FirstTrack =
		ArrayEntry(FirstBinding->AsObject().ToSharedRef(), TEXT("tracks"), 0);
	const TSharedPtr<FJsonValue> FirstSection = FirstTrack.IsValid()
		? ArrayEntry(FirstTrack->AsObject().ToSharedRef(), TEXT("sections"), 0)
		: nullptr;
	if (!TestTrue(TEXT("track and section reported"), FirstSection.IsValid()))
	{
		World->DestroyWorld(false);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
	TestTrue(TEXT("channels reported"),
		FirstSection->AsObject()->TryGetArrayField(TEXT("channels"), Channels));
	bool bFoundKeys = false;
	for (const TSharedPtr<FJsonValue>& Channel : *Channels)
	{
		const TSharedPtr<FJsonObject> ChannelJson = Channel->AsObject();
		if (ChannelJson->GetStringField(TEXT("name")) != TEXT("Location.X"))
		{
			continue;
		}
		bFoundKeys = true;
		TestEqual(TEXT("key count reported"), ChannelJson->GetNumberField(TEXT("num_keys")), 2.0);
		const TSharedPtr<FJsonValue> LastKey = ArrayEntry(ChannelJson.ToSharedRef(), TEXT("keys"), 1);
		if (TestTrue(TEXT("keys reported"), LastKey.IsValid()))
		{
			TestEqual(TEXT("key time in display frames"),
				LastKey->AsObject()->GetNumberField(TEXT("frame")), 30.0);
			TestEqual(TEXT("key value"), LastKey->AsObject()->GetNumberField(TEXT("value")), 500.0);
		}
	}
	TestTrue(TEXT("Location.X reported among the channels"), bFoundKeys);

	// ---- teardown
	Track->RemoveSection(*Section);
	TestEqual(TEXT("section removed"), Track->GetAllSections().Num(), 0);
	TestTrue(TEXT("track removed"), MovieScene->RemoveTrack(*Track));
	TestTrue(TEXT("binding removed"), MovieScene->RemovePossessable(ActorGuid));
	TestEqual(TEXT("no bindings left"),
		const_cast<const UMovieScene*>(MovieScene)->GetBindings().Num(), 0);

	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpWidgetAnimationTest, "McpLink.Sequence.WidgetAnimation", McpTestFlags)
bool FMcpWidgetAnimationTest::RunTest(const FString& Parameters)
{
	using namespace McpLink::Sequences;

	// A widget animation lives inside a Widget Blueprint, so this needs a real
	// compiled one. /Temp/ is fine here: nothing goes through IAssetTools.
	const FString AssetName = FString::Printf(
		TEXT("WBP_McpAnim_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString PackagePath = FString::Printf(TEXT("/Temp/McpLinkTests/%s"), *AssetName);
	UPackage* Package = CreatePackage(*PackagePath);
	UWidgetBlueprint* Blueprint = FWidgetBlueprintOperationUtils::CreateWidgetBlueprint(
		Package, FName(*AssetName), BPTYPE_Normal, UUserWidget::StaticClass(),
		UCanvasPanel::StaticClass());
	if (!TestNotNull(TEXT("widget blueprint created"), Blueprint))
	{
		return false;
	}
	FText AddError;
	UWidget* Title = Blueprint->WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(), TEXT("Title"));
	if (!TestTrue(TEXT("text block added"),
		FWidgetBlueprintOperationUtils::AddWidget(
			Blueprint, Title, Blueprint->WidgetTree->RootWidget, -1, AddError)))
	{
		return false;
	}
	FString Error;

	UWidgetAnimation* Animation =
		CreateWidgetAnimation(Blueprint, TEXT("Anim_FadeIn"), FFrameRate(60, 1), 1.0, Error);
	if (!TestNotNull(*FString::Printf(TEXT("animation created: %s"), *Error), Animation))
	{
		return false;
	}
	TestTrue(TEXT("animation registered on the blueprint"), Blueprint->Animations.Contains(Animation));
	TestNotNull(TEXT("animation has a movie scene"), Animation->GetMovieScene());
	TestEqual(TEXT("display rate applied"), Animation->GetMovieScene()->GetDisplayRate(), FFrameRate(60, 1));

	UMovieScene& MovieScene = *Animation->GetMovieScene();
	TestEqual(TEXT("playback range ends on a whole frame"),
		DisplayFrameFromTick(MovieScene, UE::MovieScene::DiscreteExclusiveUpper(MovieScene.GetPlaybackRange())),
		60.0);

	TestNull(TEXT("a duplicate name is refused"),
		CreateWidgetAnimation(Blueprint, TEXT("Anim_FadeIn"), FFrameRate(60, 1), 1.0, Error));
	TestTrue(TEXT("duplicate message"), Error.Contains(TEXT("already has an animation")));
	TestNull(TEXT("a bad name is refused"),
		CreateWidgetAnimation(Blueprint, TEXT("Bad Name!"), FFrameRate(60, 1), 1.0, Error));
	TestTrue(TEXT("bad name message"), Error.Contains(TEXT("not a valid animation name")));

	// The animation is addressable the way the tool addresses it.
	TestEqual(TEXT("found by <blueprint>:<animation>"),
		Find(FString::Printf(TEXT("%s:Anim_FadeIn"), *PackagePath)),
		static_cast<UMovieSceneSequence*>(Animation));
	TestEqual(TEXT("found by object path"),
		Find(Animation->GetPathName()), static_cast<UMovieSceneSequence*>(Animation));

	const FGuid Guid = BindWidget(*Animation, TEXT("Title"), Error);
	if (!TestTrue(*FString::Printf(TEXT("widget bound: %s"), *Error), Guid.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("binding is recorded against the widget name"),
		Animation->GetBindings().Num(), 1);
	TestEqual(TEXT("bound widget name"),
		Animation->GetBindings()[0].WidgetName, FName(TEXT("Title")));
	TestEqual(TEXT("binding it again reuses the same guid"),
		BindWidget(*Animation, TEXT("Title"), Error), Guid);
	TestFalse(TEXT("an unknown widget is refused"),
		BindWidget(*Animation, TEXT("NoSuchWidget"), Error).IsValid());
	TestTrue(TEXT("unknown widget message lists what exists"), Error.Contains(TEXT("Title")));

	const TSharedRef<FJsonObject> BindingJson = BindingToJson(MovieScene, Guid, false);
	TestEqual(TEXT("binding reports the widget"),
		BindingJson->GetStringField(TEXT("widget")), FString(TEXT("Title")));
	TestFalse(TEXT("not the root widget"), BindingJson->GetBoolField(TEXT("root_widget")));

	// ---- a property track keyed the way sequence_ops keys one
	int32 NumChannels = 0;
	UEnum* Enum = nullptr;
	UClass* ObjectClass = nullptr;
	const FProperty* Opacity = FindFProperty<FProperty>(UWidget::StaticClass(), TEXT("RenderOpacity"));
	TestEqual(TEXT("RenderOpacity animates as a float track"),
		PropertyTrackClass(Opacity, NumChannels, Enum, ObjectClass), UMovieSceneFloatTrack::StaticClass());

	UMovieSceneTrack* Track = MovieScene.AddTrack(UMovieSceneFloatTrack::StaticClass(), Guid);
	if (!TestNotNull(TEXT("float track added"), Track))
	{
		return false;
	}
	UMovieSceneSection* Section = Track->CreateNewSection();
	Section->SetRange(MovieScene.GetPlaybackRange());
	Track->AddSection(*Section);

	FName ChannelType;
	// A single-value section leaves its channel unnamed; the tool calls it "Value".
	FMovieSceneChannel* Channel = FindChannel(*Section, TEXT("Value"), ChannelType);
	if (!TestNotNull(TEXT("the single channel is addressable as \"Value\""), Channel))
	{
		return false;
	}
	FName EmptyType;
	TestEqual(TEXT("and as the only channel"), FindChannel(*Section, FString(), EmptyType), Channel);

	TestTrue(TEXT("opacity keyed at 0"),
		SetKey(Channel, ChannelType, FFrameNumber(0), MakeShared<FJsonValueNumber>(0.0), TEXT("linear"), Error));
	TestTrue(TEXT("opacity keyed at one second"),
		SetKey(Channel, ChannelType, TickFromSeconds(MovieScene, 1.0),
			MakeShared<FJsonValueNumber>(1.0), TEXT("linear"), Error));
	TestEqual(TEXT("two keys"), Channel->GetNumKeys(), 2);

	const TSharedRef<FJsonObject> Json = SequenceToJson(*Animation, false);
	TestEqual(TEXT("reported as a widget animation"),
		Json->GetStringField(TEXT("kind")), FString(TEXT("widget_animation")));
	TestEqual(TEXT("names its animation"), Json->GetStringField(TEXT("name")), FString(TEXT("Anim_FadeIn")));
	TestTrue(TEXT("names its widget blueprint"),
		Json->GetStringField(TEXT("widget_blueprint")).Contains(TEXT("WBP_McpAnim")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
