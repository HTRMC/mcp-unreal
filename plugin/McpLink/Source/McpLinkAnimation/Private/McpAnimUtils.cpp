#include "McpAnimUtils.h"

#include "Animation/AnimMontage.h"
#include "Animation/BlendSpace.h"

namespace McpLink::Anim
{
	FSlotAnimationTrack* FindSlot(UAnimMontage* Montage, FName SlotName)
	{
		if (Montage == nullptr)
		{
			return nullptr;
		}
		for (FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
		{
			if (Slot.SlotName == SlotName)
			{
				return &Slot;
			}
		}
		return nullptr;
	}

	FSlotAnimationTrack& FindOrAddSlot(UAnimMontage* Montage, FName SlotName)
	{
		FSlotAnimationTrack* Existing = FindSlot(Montage, SlotName);
		return Existing != nullptr ? *Existing : Montage->AddSlot(SlotName);
	}

	void RefreshMontage(UAnimMontage* Montage)
	{
		if (Montage == nullptr)
		{
			return;
		}
		Montage->SetCompositeLength(Montage->CalculateSequenceLength());
		Montage->PostEditChange();
		Montage->MarkPackageDirty();
	}

	FBlendParameter* MutableBlendParameter(UBlendSpace* BlendSpace, int32 Axis)
	{
		if (BlendSpace == nullptr)
		{
			return nullptr;
		}
		FStructProperty* Property = CastField<FStructProperty>(
			UBlendSpace::StaticClass()->FindPropertyByName(TEXT("BlendParameters")));
		if (Property == nullptr || Axis < 0 || Axis >= Property->ArrayDim)
		{
			return nullptr;
		}
		return Property->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace, Axis);
	}
}
