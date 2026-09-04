// Navigation queries: the answers an AI programmer gets from the nav mesh.
//
// build_level navigation rebuilds the mesh; this reads it. Projecting a point,
// pathing between two points and testing reachability are the three questions
// that decide whether a level's navigation actually works, and none of them
// were answerable before — a rebuilt nav mesh with a hole in it looked exactly
// like a good one.

#include "AI/NavigationSystemBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationData.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"

namespace McpLink
{
	namespace Navigation
	{
		UNavigationSystemV1* NavSystemOrError(
			UWorld* World, const TSharedRef<FMcpResponder>& Responder)
		{
			UNavigationSystemV1* System = FNavigationSystem::GetCurrent<UNavigationSystemV1>(World);
			if (System == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_navigation"),
					TEXT("this world has no navigation system — add a Nav Mesh Bounds Volume, then ")
					TEXT("build_level navigation"));
			}
			return System;
		}

		/// Either an explicit `location` or an actor's position, so a query can
		/// be phrased against the things already in the level.
		bool ReadPoint(
			UWorld* World,
			const TSharedRef<FJsonObject>& Body,
			const TCHAR* PointField,
			const TCHAR* ActorField,
			FVector& Out,
			FString& OutError)
		{
			if (GetVector(Body, PointField, Out))
			{
				return true;
			}
			FString ActorSpec;
			if (Body->TryGetStringField(ActorField, ActorSpec) && !ActorSpec.IsEmpty())
			{
				if (AActor* Actor = ResolveActor(World, ActorSpec))
				{
					Out = Actor->GetActorLocation();
					return true;
				}
				OutError = FString::Printf(TEXT("no actor '%s' in this world"), *ActorSpec);
				return false;
			}
			OutError = FString::Printf(
				TEXT("'%s' ([X,Y,Z]) or '%s' (an actor path or label) is required"),
				PointField, ActorField);
			return false;
		}
	}

	using namespace Navigation;

	void RegisterNavigationRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/ai/navigation"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				UWorld* World = ResolveWorldOrError(Body, Responder);
				if (World == nullptr)
				{
					return;
				}
				UNavigationSystemV1* NavSystem = NavSystemOrError(World, Responder);
				if (NavSystem == nullptr)
				{
					return;
				}

				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("world"), World->GetName());
					Data->SetBoolField(TEXT("building"), NavSystem->IsNavigationBuildInProgress());
					Data->SetNumberField(TEXT("remaining_build_tasks"),
						NavSystem->GetNumRemainingBuildTasks());

					TArray<TSharedPtr<FJsonValue>> NavData;
					for (ANavigationData* Data_ : NavSystem->NavDataSet)
					{
						if (Data_ == nullptr)
						{
							continue;
						}
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("actor"), Data_->GetPathName());
						Item->SetStringField(TEXT("class"), Data_->GetClass()->GetName());
						Item->SetStringField(TEXT("agent"), Data_->GetConfig().Name.ToString());
						Item->SetNumberField(TEXT("agent_radius"), Data_->GetConfig().AgentRadius);
						Item->SetNumberField(TEXT("agent_height"), Data_->GetConfig().AgentHeight);
						const FBox Bounds = Data_->GetBounds();
						if (Bounds.IsValid)
						{
							Item->SetArrayField(TEXT("bounds_min"), VectorToJson(Bounds.Min));
							Item->SetArrayField(TEXT("bounds_max"), VectorToJson(Bounds.Max));
						}
						NavData.Add(MakeShared<FJsonValueObject>(Item));
					}
					Data->SetNumberField(TEXT("nav_data_count"), NavData.Num());
					Data->SetArrayField(TEXT("nav_data"), NavData);
					if (NavData.IsEmpty())
					{
						Data->SetStringField(TEXT("message"),
							TEXT("no navigation data — the level needs a Nav Mesh Bounds Volume, then ")
							TEXT("build_level navigation"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("project_point"))
				{
					FVector Point = FVector::ZeroVector;
					FString Error;
					if (!ReadPoint(World, Body, TEXT("location"), TEXT("actor"), Point, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"), Error);
						return;
					}
					FVector Extent = FVector::ZeroVector;
					GetVector(Body, TEXT("extent"), Extent);

					FVector Projected = FVector::ZeroVector;
					const bool bFound = UNavigationSystemV1::K2_ProjectPointToNavigation(
						World, Point, Projected, nullptr, nullptr, Extent);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("on_navmesh"), bFound);
					Data->SetArrayField(TEXT("query"), VectorToJson(Point));
					if (bFound)
					{
						Data->SetArrayField(TEXT("projected"), VectorToJson(Projected));
						Data->SetNumberField(TEXT("distance"), FVector::Dist(Point, Projected));
					}
					else
					{
						Data->SetStringField(TEXT("message"),
							TEXT("no navigable surface near that point — widen `extent`, or the nav mesh ")
							TEXT("does not cover it"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("find_path") || Operation == TEXT("path_length"))
				{
					FVector Start = FVector::ZeroVector;
					FVector End = FVector::ZeroVector;
					FString Error;
					if (!ReadPoint(World, Body, TEXT("start"), TEXT("start_actor"), Start, Error)
						|| !ReadPoint(World, Body, TEXT("end"), TEXT("end_actor"), End, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"), Error);
						return;
					}

					if (Operation == TEXT("path_length"))
					{
						double Length = 0.0;
						const ENavigationQueryResult::Type Result =
							UNavigationSystemV1::GetPathLength(World, Start, End, Length);
						const UEnum* ResultEnum = StaticEnum<ENavigationQueryResult::Type>();
						const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
						Data->SetStringField(TEXT("result"),
							ResultEnum != nullptr
								? ResultEnum->GetNameStringByValue(static_cast<int64>(Result))
								: FString::FromInt(static_cast<int32>(Result)));
						Data->SetBoolField(TEXT("reachable"), Result == ENavigationQueryResult::Success);
						Data->SetNumberField(TEXT("length"), Length);
						Data->SetNumberField(TEXT("straight_line"), FVector::Dist(Start, End));
						Responder->Ok(Data);
						return;
					}

					AActor* Context = nullptr;
					FString ContextSpec;
					if (Body->TryGetStringField(TEXT("context_actor"), ContextSpec)
						&& !ContextSpec.IsEmpty())
					{
						Context = ResolveActor(World, ContextSpec);
					}
					UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(
						World, Start, End, Context);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("start"), VectorToJson(Start));
					Data->SetArrayField(TEXT("end"), VectorToJson(End));
					const bool bValid = Path != nullptr && Path->IsValid();
					Data->SetBoolField(TEXT("valid"), bValid);
					if (!bValid)
					{
						Data->SetStringField(TEXT("message"),
							TEXT("no path — one end is probably off the nav mesh (check with ")
							TEXT("project_point) or the two are not connected"));
						Responder->Ok(Data);
						return;
					}
					// A partial path means the destination was unreachable and
					// the query stopped at the nearest point it could get to.
					Data->SetBoolField(TEXT("partial"), Path->IsPartial());
					Data->SetNumberField(TEXT("length"), Path->GetPathLength());
					Data->SetNumberField(TEXT("straight_line"), FVector::Dist(Start, End));
					TArray<TSharedPtr<FJsonValue>> Points;
					const int32 Max = FMath::Clamp(IntOr(Body, TEXT("max_points"), 100), 1, 2000);
					for (const FVector& Point : Path->PathPoints)
					{
						if (Points.Num() >= Max)
						{
							break;
						}
						Points.Add(MakeShared<FJsonValueArray>(VectorToJson(Point)));
					}
					Data->SetNumberField(TEXT("point_count"), Path->PathPoints.Num());
					Data->SetArrayField(TEXT("points"), Points);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("raycast"))
				{
					FVector Start = FVector::ZeroVector;
					FVector End = FVector::ZeroVector;
					FString Error;
					if (!ReadPoint(World, Body, TEXT("start"), TEXT("start_actor"), Start, Error)
						|| !ReadPoint(World, Body, TEXT("end"), TEXT("end_actor"), End, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"), Error);
						return;
					}
					FVector Hit = FVector::ZeroVector;
					// True means the ray hit a nav mesh edge, i.e. the straight
					// line is blocked.
					const bool bBlocked =
						UNavigationSystemV1::NavigationRaycast(World, Start, End, Hit);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("blocked"), bBlocked);
					Data->SetBoolField(TEXT("clear_line_of_walk"), !bBlocked);
					Data->SetArrayField(TEXT("hit"), VectorToJson(Hit));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("random_point"))
				{
					FVector Origin = FVector::ZeroVector;
					FString Error;
					if (!ReadPoint(World, Body, TEXT("origin"), TEXT("actor"), Origin, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"), Error);
						return;
					}
					FVector Random = FVector::ZeroVector;
					const double Radius = DoubleOr(Body, TEXT("radius"), 1000.0);
					const bool bFound = UNavigationSystemV1::K2_GetRandomReachablePointInRadius(
						World, Origin, Random, Radius);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("found"), bFound);
					Data->SetNumberField(TEXT("radius"), Radius);
					if (bFound)
					{
						Data->SetArrayField(TEXT("location"), VectorToJson(Random));
					}
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use info, project_point, find_path, ")
						TEXT("path_length, raycast, or random_point"),
						*Operation));
			});
	}
}
