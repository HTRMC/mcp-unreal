// UMG refuses to construct the base UUserWidget class directly ("Abstract,
// Deprecated or Replaced classes are not allowed..."), so the runtime HUD needs
// a concrete subclass. Its tree is built in C++ by AMcpTestCharacter.
#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"
#include "McpTestHudWidget.generated.h"

UCLASS()
class UMcpTestHudWidget : public UUserWidget
{
	GENERATED_BODY()
};
