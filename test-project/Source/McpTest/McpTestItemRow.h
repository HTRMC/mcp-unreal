// A small DataTable row struct so the data_table_ops tool has something real to
// exercise in the test project.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "McpTestItemRow.generated.h"

USTRUCT(BlueprintType)
struct FMcpTestItemRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item")
	FString DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item")
	int32 Value = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item")
	float Weight = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Item")
	bool bStackable = false;
};
