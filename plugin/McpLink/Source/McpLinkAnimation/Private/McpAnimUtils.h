// Engine-facing helpers for animation asset authoring, split out so the
// automation tests can exercise them without going through HTTP.
#pragma once

#include "CoreMinimal.h"

class UAnimMontage;
class UBlendSpace;
struct FBlendParameter;
struct FSlotAnimationTrack;

namespace McpLink::Anim
{
	/// The montage's slot track of that name, or nullptr.
	///
	/// UAnimMontage::AddSlot always appends, and IsValidSlot answers "has
	/// segments" rather than "exists", so building on either alone happily
	/// produces two slots with the same name.
	FSlotAnimationTrack* FindSlot(UAnimMontage* Montage, FName SlotName);

	FSlotAnimationTrack& FindOrAddSlot(UAnimMontage* Montage, FName SlotName);

	/// Store the montage's length and let it re-derive its section links, the
	/// way the montage editor does after a structural edit.
	/// UAnimMontage::CalculateSequenceLength only computes; the caller stores.
	void RefreshMontage(UAnimMontage* Montage);

	/// UBlendSpace::BlendParameters is protected with only a const getter, so
	/// writes go through the reflected property — the same route the details
	/// panel takes when an axis is edited by hand. Null for an out-of-range
	/// axis, or if the property is ever renamed.
	FBlendParameter* MutableBlendParameter(UBlendSpace* BlendSpace, int32 Axis);
}
