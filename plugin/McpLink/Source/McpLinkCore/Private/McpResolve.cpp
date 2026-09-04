#include "McpResolve.h"

#include "McpResponder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace McpLink
{
	bool ShouldTransact(const UWorld* World)
	{
		return World == nullptr || !World->IsGameWorld();
	}

	bool ShouldTransact(const UObject* Object)
	{
		if (Object == nullptr)
		{
			return true;
		}
		if (const UWorld* World = Object->GetWorld(); World != nullptr && World->IsGameWorld())
		{
			return false;
		}
		return Object->GetTypedOuter<UGameInstance>() == nullptr;
	}

	UWorld* ResolveWorld(const TSharedRef<FJsonObject>& Body)
	{
		FString Selector = TEXT("auto");
		Body->TryGetStringField(TEXT("world"), Selector);

		const bool bPieActive = GEditor != nullptr && GEditor->PlayWorld != nullptr;
		if (Selector.Equals(TEXT("pie"), ESearchCase::IgnoreCase))
		{
			return bPieActive ? GEditor->PlayWorld.Get() : nullptr;
		}
		if (Selector.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
		{
			return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		}
		// auto
		if (bPieActive)
		{
			return GEditor->PlayWorld.Get();
		}
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	UWorld* ResolveWorldOrError(
		const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
	{
		UWorld* World = ResolveWorld(Body);
		if (World == nullptr)
		{
			Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("pie_not_running"),
				TEXT("world 'pie' requested but no PIE session is running — start one with pie_control"));
		}
		return World;
	}

	AActor* ResolveActor(UWorld* World, const FString& Spec)
	{
		if (World == nullptr || Spec.IsEmpty())
		{
			return nullptr;
		}
		// Object path first (fast, unambiguous).
		if (Spec.Contains(TEXT("/")))
		{
			const FSoftObjectPath Path(Spec);
			if (AActor* Actor = Cast<AActor>(Path.ResolveObject()))
			{
				if (Actor->GetWorld() == World)
				{
					return Actor;
				}
			}
		}
		// Editor label or FName scan.
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor->GetActorLabel() == Spec || Actor->GetName() == Spec)
			{
				return Actor;
			}
		}
		return nullptr;
	}

	UObject* ResolveObject(const FString& Path)
	{
		const FSoftObjectPath SoftPath(Path);
		if (UObject* Found = SoftPath.ResolveObject())
		{
			return Found;
		}
		return SoftPath.TryLoad();
	}

	UObject* ResolveAsset(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}
		// "/Game/Foo" is the package; the asset inside it repeats the name.
		const FString Full = Path.Contains(TEXT("."))
			? Path
			: FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path));
		const FSoftObjectPath SoftPath(Full);
		if (UObject* Found = SoftPath.ResolveObject())
		{
			return Found;
		}
		if (!FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Full)))
		{
			return nullptr;
		}
		return SoftPath.TryLoad();
	}

	UClass* ResolveClass(const FString& Spec)
	{
		if (Spec.IsEmpty())
		{
			return nullptr;
		}
		if (Spec.StartsWith(TEXT("/")))
		{
			// /Script/Module.Class or /Game/Path.Asset[_C]
			if (UClass* Loaded = Cast<UClass>(ResolveObject(Spec)))
			{
				return Loaded;
			}
			// Blueprint asset path without the generated-class suffix.
			if (UBlueprint* Blueprint = Cast<UBlueprint>(ResolveObject(Spec)))
			{
				return Blueprint->GeneratedClass;
			}
			// Retry with an explicit _C generated-class path.
			if (!Spec.EndsWith(TEXT("_C")))
			{
				FString AssetName = FPackageName::GetShortName(Spec);
				AssetName.Split(TEXT("."), nullptr, &AssetName);
				const FString WithClass = FString::Printf(TEXT("%s.%s_C"), *Spec, *AssetName);
				return Cast<UClass>(ResolveObject(WithClass));
			}
			return nullptr;
		}
		// Bare class name, e.g. "StaticMeshActor".
		return FindFirstObject<UClass>(*Spec, EFindFirstObjectOptions::None,
			ELogVerbosity::Warning, TEXT("McpLink ResolveClass"));
	}

	static bool GetThreeNumbers(
		const TSharedRef<FJsonObject>& Body, const TCHAR* Field, double& A, double& B, double& C)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Body->TryGetArrayField(Field, Values) || Values == nullptr || Values->Num() != 3)
		{
			return false;
		}
		return (*Values)[0]->TryGetNumber(A) && (*Values)[1]->TryGetNumber(B) && (*Values)[2]->TryGetNumber(C);
	}

	bool GetVector(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, FVector& Out)
	{
		double X, Y, Z;
		if (!GetThreeNumbers(Body, Field, X, Y, Z))
		{
			return false;
		}
		Out = FVector(X, Y, Z);
		return true;
	}

	bool GetRotator(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, FRotator& Out)
	{
		double Pitch, Yaw, Roll;
		if (!GetThreeNumbers(Body, Field, Pitch, Yaw, Roll))
		{
			return false;
		}
		Out = FRotator(Pitch, Yaw, Roll);
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> VectorToJson(const FVector& V)
	{
		return {
			MakeShared<FJsonValueNumber>(V.X),
			MakeShared<FJsonValueNumber>(V.Y),
			MakeShared<FJsonValueNumber>(V.Z)};
	}

	TArray<TSharedPtr<FJsonValue>> RotatorToJson(const FRotator& R)
	{
		return {
			MakeShared<FJsonValueNumber>(R.Pitch),
			MakeShared<FJsonValueNumber>(R.Yaw),
			MakeShared<FJsonValueNumber>(R.Roll)};
	}
}
