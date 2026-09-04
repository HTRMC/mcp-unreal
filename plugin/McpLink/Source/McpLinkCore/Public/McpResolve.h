// Shared resolvers: world selector, actor/object/class lookup, vector parsing.
#pragma once

#include "CoreMinimal.h"

class AActor;
class FJsonObject;
class FJsonValue;
class UClass;
class UObject;
class UWorld;

namespace McpLink
{
	class FMcpResponder;

	// Tri-state "world" body field: "auto" (default: PIE if playing, else the
	// editor world), "pie" (nullptr when PIE is not running — callers must
	// error, never silently fall back), "editor".
	MCPLINKCORE_API UWorld* ResolveWorld(const TSharedRef<FJsonObject>& Body);

	// ResolveWorld plus the standard 409 when "pie" was asked for and no PIE
	// session is running. Returns nullptr *after* responding, so a handler can
	// simply bail out.
	MCPLINKCORE_API UWorld* ResolveWorldOrError(
		const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder);

	// Spec is an object path (…:PersistentLevel.Name) or an editor actor label.
	MCPLINKCORE_API AActor* ResolveActor(UWorld* World, const FString& Spec);

	// Full object path -> UObject (resolves in memory, then tries loading).
	MCPLINKCORE_API UObject* ResolveObject(const FString& Path);

	// Asset path -> UObject, accepting both "/Game/Foo" and "/Game/Foo.Foo".
	// Never loads a package that does not exist: a failed TryLoad leaves an
	// empty UPackage registered, after which IAssetTools::CreateAsset refuses
	// the path as already taken.
	MCPLINKCORE_API UObject* ResolveAsset(const FString& Path);

	// "StaticMeshActor", "/Script/Engine.StaticMeshActor", or a Blueprint
	// asset path ("/Game/BP_Thing" or "/Game/BP_Thing.BP_Thing_C").
	MCPLINKCORE_API UClass* ResolveClass(const FString& Spec);

	// Whether an edit to Object (or anything in World) may be recorded in the
	// undo buffer. False for game-world (PIE) objects and anything owned by a
	// GameInstance: a transaction record would keep the dead PIE world alive
	// and EndPIE asserts. Pass the result as FScopedTransaction's second arg.
	MCPLINKCORE_API bool ShouldTransact(const UObject* Object);
	MCPLINKCORE_API bool ShouldTransact(const UWorld* World);

	// [X,Y,Z] JSON array fields.
	MCPLINKCORE_API bool GetVector(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, FVector& Out);
	// [Pitch,Yaw,Roll] JSON array fields.
	MCPLINKCORE_API bool GetRotator(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, FRotator& Out);

	MCPLINKCORE_API TArray<TSharedPtr<FJsonValue>> VectorToJson(const FVector& V);
	MCPLINKCORE_API TArray<TSharedPtr<FJsonValue>> RotatorToJson(const FRotator& R);
}
