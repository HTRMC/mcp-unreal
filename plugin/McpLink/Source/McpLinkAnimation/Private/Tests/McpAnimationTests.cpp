// Montage and Blend Space authoring, on scratch assets.
//
// Both checks here guard a trap the engine sets rather than anything McpLink
// invented: UAnimMontage::AddSlot appends unconditionally (so a second
// "DefaultSlot" is easy to create by accident) and CalculateSequenceLength
// returns the length without storing it (so a montage authored through it
// stays zero-length and plays nothing).

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/AnimMontage.h"
#include "Animation/BlendSpace.h"
#include "McpAnimUtils.h"
#include "McpTestFlags.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace
{
	template <typename T>
	T* MakeAnimScratchAsset(const TCHAR* Prefix)
	{
		const FString PackageName = FString::Printf(
			TEXT("/Temp/McpLinkAnimTests/%s_%s"), Prefix,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		UPackage* Package = CreatePackage(*PackageName);
		return NewObject<T>(
			Package, FName(*FPackageName::GetShortName(PackageName)),
			RF_Public | RF_Standalone | RF_Transactional);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpMontageSlotTest, "McpLink.Animation.MontageSlots", McpTestFlags)
bool FMcpMontageSlotTest::RunTest(const FString& Parameters)
{
	UAnimMontage* Montage = MakeAnimScratchAsset<UAnimMontage>(TEXT("AM"));
	if (!TestNotNull(TEXT("montage created"), Montage))
	{
		return false;
	}

	// A montage is born with a DefaultSlot, which is exactly why AddSlot alone
	// is the wrong call: it would append a second one of the same name.
	const int32 SlotsAtBirth = Montage->SlotAnimTracks.Num();
	FSlotAnimationTrack* Default = McpLink::Anim::FindSlot(Montage, TEXT("DefaultSlot"));
	if (!TestNotNull(TEXT("a fresh montage already has DefaultSlot"), Default))
	{
		return false;
	}

	FSlotAnimationTrack& Reused = McpLink::Anim::FindOrAddSlot(Montage, TEXT("DefaultSlot"));
	TestEqual(TEXT("asking for it again adds nothing"), Montage->SlotAnimTracks.Num(), SlotsAtBirth);
	TestTrue(TEXT("the existing slot is returned"), Default == &Reused);

	McpLink::Anim::FindOrAddSlot(Montage, TEXT("UpperBody"));
	TestEqual(TEXT("a new name does append"), Montage->SlotAnimTracks.Num(), SlotsAtBirth + 1);
	TestNotNull(TEXT("the new slot is findable"), McpLink::Anim::FindSlot(Montage, TEXT("UpperBody")));
	TestNull(TEXT("an absent slot is not"), McpLink::Anim::FindSlot(Montage, TEXT("Nope")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpMontageLengthTest, "McpLink.Animation.MontageLength", McpTestFlags)
bool FMcpMontageLengthTest::RunTest(const FString& Parameters)
{
	UAnimMontage* Montage = MakeAnimScratchAsset<UAnimMontage>(TEXT("AM"));
	if (Montage == nullptr)
	{
		return false;
	}

	FSlotAnimationTrack& Slot = McpLink::Anim::FindOrAddSlot(Montage, TEXT("DefaultSlot"));
	// Segment length comes from the times alone, so no source animation is
	// needed — and a scratch UAnimSequence would warn about its empty model.
	FAnimSegment Segment;
	Segment.StartPos = 0.f;
	Segment.AnimStartTime = 0.f;
	Segment.AnimEndTime = 2.f;
	Segment.AnimPlayRate = 1.f;
	Segment.LoopingCount = 1;
	Slot.AnimTrack.AnimSegments.Add(Segment);

	TestEqual(TEXT("track knows its own length"), Slot.AnimTrack.GetLength(), 2.f);
	TestEqual(TEXT("montage length is still unset"), Montage->GetPlayLength(), 0.f);

	McpLink::Anim::RefreshMontage(Montage);
	TestEqual(TEXT("refresh stores the calculated length"), Montage->GetPlayLength(), 2.f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMcpBlendSpaceAxisTest, "McpLink.Animation.BlendSpaceAxis", McpTestFlags)
bool FMcpBlendSpaceAxisTest::RunTest(const FString& Parameters)
{
	UBlendSpace* BlendSpace = MakeAnimScratchAsset<UBlendSpace>(TEXT("BS"));
	if (!TestNotNull(TEXT("blend space created"), BlendSpace))
	{
		return false;
	}

	FBlendParameter* Axis = McpLink::Anim::MutableBlendParameter(BlendSpace, 0);
	if (!TestNotNull(TEXT("BlendParameters is reachable by reflection"), Axis))
	{
		return false;
	}
	Axis->DisplayName = TEXT("Speed");
	Axis->Min = 0.f;
	Axis->Max = 600.f;
	Axis->GridNum = 4;

	// The write must land on the object the const getter reads, not a copy.
	const FBlendParameter& ReadBack = BlendSpace->GetBlendParameter(0);
	TestEqual(TEXT("axis name"), ReadBack.DisplayName, FString(TEXT("Speed")));
	TestEqual(TEXT("axis max"), ReadBack.Max, 600.f);
	TestEqual(TEXT("grid divisions"), ReadBack.GridNum, 4);

	TestTrue(TEXT("a value in range validates"),
		BlendSpace->ValidateSampleValue(FVector(300.f, 0.f, 0.f)));
	TestFalse(TEXT("a value past the max does not"),
		BlendSpace->ValidateSampleValue(FVector(9999.f, 0.f, 0.f)));

	TestNull(TEXT("axis 3 is out of range"), McpLink::Anim::MutableBlendParameter(BlendSpace, 3));
	TestNull(TEXT("a negative axis is out of range"),
		McpLink::Anim::MutableBlendParameter(BlendSpace, -1));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
