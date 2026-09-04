// A small attribute set so gas_ops has real attributes, effects and abilities
// to drive in the test project.
#pragma once

#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "CoreMinimal.h"
#include "McpTestAttributeSet.generated.h"

#define MCPTEST_ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

UCLASS()
class UMcpTestAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UMcpTestAttributeSet();

	UPROPERTY(BlueprintReadOnly, Category = "McpTest")
	FGameplayAttributeData Health;
	MCPTEST_ATTRIBUTE_ACCESSORS(UMcpTestAttributeSet, Health)

	UPROPERTY(BlueprintReadOnly, Category = "McpTest")
	FGameplayAttributeData MaxHealth;
	MCPTEST_ATTRIBUTE_ACCESSORS(UMcpTestAttributeSet, MaxHealth)

	UPROPERTY(BlueprintReadOnly, Category = "McpTest")
	FGameplayAttributeData Speed;
	MCPTEST_ATTRIBUTE_ACCESSORS(UMcpTestAttributeSet, Speed)
};
