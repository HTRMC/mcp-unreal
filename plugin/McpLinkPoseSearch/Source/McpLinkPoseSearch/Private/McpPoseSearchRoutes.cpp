// Motion Matching: Pose Search schemas (a skeleton per role, a sample rate and
// a list of feature channels) and Pose Search databases (a schema plus
// animation entries, indexed through the derived-data build).

#include "Animation/MirrorDataTable.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchDerivedData.h"
#include "PoseSearch/PoseSearchFeatureChannel.h"
#include "PoseSearch/PoseSearchIndex.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace PoseSearches
	{
		UClass* FindChannelClass(const FString& Spec)
		{
			if (UClass* Direct = ResolveClass(Spec); Direct != nullptr && Direct->IsChildOf(UPoseSearchFeatureChannel::StaticClass()))
			{
				return Direct;
			}
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (!It->IsChildOf(UPoseSearchFeatureChannel::StaticClass()) || It->HasAnyClassFlags(CLASS_Abstract))
				{
					continue;
				}
				// "Position" matches UPoseSearchFeatureChannel_Position.
				if (It->GetName().Equals(Spec, ESearchCase::IgnoreCase)
					|| It->GetName().Equals(TEXT("PoseSearchFeatureChannel_") + Spec, ESearchCase::IgnoreCase))
				{
					return *It;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> ObjectPropertiesJson(const UObject* Object)
		{
			const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			FJsonObjectConverter::UStructToJsonObject(Object->GetClass(), Object, Properties, CPF_Edit, CPF_InstancedReference,
				nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
			return Properties;
		}

		bool ApplyProperties(const TSharedRef<FJsonObject>& Body, UObject* Target, FString& OutError)
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Body->TryGetObjectField(TEXT("properties"), Properties) || !Properties->IsValid())
			{
				return true;
			}
			FText Reason;
			if (!FJsonObjectConverter::JsonObjectToUStruct((*Properties).ToSharedRef(), Target->GetClass(), Target, 0, 0, false, &Reason))
			{
				OutError = FString::Printf(TEXT("'properties' did not import onto %s: %s"), *Target->GetClass()->GetName(), *Reason.ToString());
				return false;
			}
			return true;
		}

		FScriptArrayHelper* ChannelArray(UPoseSearchSchema* Schema, FArrayProperty*& OutProperty)
		{
			OutProperty = CastField<FArrayProperty>(UPoseSearchSchema::StaticClass()->FindPropertyByName(TEXT("Channels")));
			if (OutProperty == nullptr)
			{
				return nullptr;
			}
			return new FScriptArrayHelper(OutProperty, OutProperty->ContainerPtrToValuePtr<void>(Schema));
		}

		TSharedRef<FJsonObject> SchemaJson(UPoseSearchSchema* Schema)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("schema"), Schema->GetPathName());
			Data->SetNumberField(TEXT("sample_rate"), Schema->SampleRate);
			TArray<TSharedPtr<FJsonValue>> Skeletons;
			for (const FPoseSearchRoledSkeleton& Roled : Schema->GetRoledSkeletons())
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("skeleton"), Roled.Skeleton != nullptr ? Roled.Skeleton->GetPathName() : FString());
				Entry->SetStringField(TEXT("mirror_table"), Roled.MirrorDataTable != nullptr ? Roled.MirrorDataTable->GetPathName() : FString());
				Entry->SetStringField(TEXT("role"), Roled.Role.ToString());
				Skeletons.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Data->SetArrayField(TEXT("skeletons"), Skeletons);
			TArray<TSharedPtr<FJsonValue>> Channels;
			FArrayProperty* Property = nullptr;
			TUniquePtr<FScriptArrayHelper> Helper(ChannelArray(Schema, Property));
			if (Helper.IsValid())
			{
				FObjectProperty* Inner = CastField<FObjectProperty>(Property->Inner);
				for (int32 Index = 0; Index < Helper->Num(); ++Index)
				{
					const UObject* Channel = Inner != nullptr ? Inner->GetObjectPropertyValue(Helper->GetRawPtr(Index)) : nullptr;
					const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
					Entry->SetNumberField(TEXT("index"), Index);
					Entry->SetStringField(TEXT("class"), Channel != nullptr ? Channel->GetClass()->GetName() : FString());
					if (Channel != nullptr)
					{
						Entry->SetObjectField(TEXT("properties"), ObjectPropertiesJson(Channel));
					}
					Channels.Add(MakeShared<FJsonValueObject>(Entry));
				}
			}
			Data->SetArrayField(TEXT("channels"), Channels);
			Data->SetNumberField(TEXT("finalized_channels"), Schema->GetChannels().Num());
			return Data;
		}

		UPoseSearchSchema* SchemaOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("schema"), Path, Responder, TEXT("a Pose Search Schema asset path")))
			{
				return nullptr;
			}
			UPoseSearchSchema* Schema = Cast<UPoseSearchSchema>(ResolveAsset(Path));
			if (Schema == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("schema_not_found"),
					FString::Printf(TEXT("no Pose Search Schema at '%s'"), *Path));
			}
			return Schema;
		}

		UPoseSearchDatabase* DatabaseOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("database"), Path, Responder, TEXT("a Pose Search Database asset path")))
			{
				return nullptr;
			}
			UPoseSearchDatabase* Database = Cast<UPoseSearchDatabase>(ResolveAsset(Path));
			if (Database == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("database_not_found"),
					FString::Printf(TEXT("no Pose Search Database at '%s'"), *Path));
			}
			return Database;
		}

		const TCHAR* BuildResultName(UE::PoseSearch::EAsyncBuildIndexResult Result)
		{
			switch (Result)
			{
			case UE::PoseSearch::EAsyncBuildIndexResult::Success: return TEXT("built");
			case UE::PoseSearch::EAsyncBuildIndexResult::InProgress: return TEXT("in_progress");
			default: return TEXT("failed");
			}
		}

		TSharedRef<FJsonObject> DatabaseJson(UPoseSearchDatabase* Database)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("database"), Database->GetPathName());
			Data->SetStringField(TEXT("schema"), Database->Schema != nullptr ? Database->Schema->GetPathName() : FString());
			TArray<TSharedPtr<FJsonValue>> Assets;
			for (int32 Index = 0; Index < Database->GetNumAnimationAssets(); ++Index)
			{
				const FPoseSearchDatabaseAnimationAsset* Entry = Database->GetDatabaseAnimationAsset(Index);
				const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetNumberField(TEXT("index"), Index);
				if (Entry != nullptr)
				{
					FJsonObjectConverter::UStructToJsonObject(FPoseSearchDatabaseAnimationAsset::StaticStruct(), Entry, Json, 0, 0,
						nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
					Json->SetStringField(TEXT("animation"), Entry->AnimAsset != nullptr ? Entry->AnimAsset->GetPathName() : FString());
					Json->SetBoolField(TEXT("looping"), Entry->IsLooping());
				}
				Assets.Add(MakeShared<FJsonValueObject>(Json));
			}
			Data->SetArrayField(TEXT("animations"), Assets);
			// ContinueRequest answers whether an index exists without rebuilding.
			const UE::PoseSearch::EAsyncBuildIndexResult Result =
				UE::PoseSearch::FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(Database, UE::PoseSearch::ERequestAsyncBuildFlag::ContinueRequest);
			Data->SetStringField(TEXT("index"), BuildResultName(Result));
			if (Result == UE::PoseSearch::EAsyncBuildIndexResult::Success)
			{
				const UE::PoseSearch::FSearchIndex& Index = Database->GetSearchIndex();
				Data->SetNumberField(TEXT("poses"), Index.GetNumPoses());
				Data->SetNumberField(TEXT("index_assets"), Index.Assets.Num());
			}
			return Data;
		}
	}

	void RegisterPoseSearchRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace PoseSearches;

		Core.RegisterRoute(TEXT("/api/anim/pose_search"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_channels"))
				{
					TArray<TSharedPtr<FJsonValue>> Channels;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						if (!It->IsChildOf(UPoseSearchFeatureChannel::StaticClass()) || It->HasAnyClassFlags(CLASS_Abstract)
							|| *It == UPoseSearchFeatureChannel::StaticClass())
						{
							continue;
						}
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("class"), It->GetName());
						Entry->SetStringField(TEXT("path"), It->GetPathName());
						Entry->SetObjectField(TEXT("defaults"), ObjectPropertiesJson(It->GetDefaultObject()));
						Channels.Add(MakeShared<FJsonValueObject>(Entry));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("channels"), Channels);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_schema"))
				{
					FString Path, SkeletonPath;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("the new asset's package path, e.g. /Game/MM/PSS_Hero"))
						|| !RequireString(Body, TEXT("skeleton"), SkeletonPath, Responder, TEXT("a Skeleton asset path")))
					{
						return;
					}
					USkeleton* Skeleton = Cast<USkeleton>(ResolveAsset(SkeletonPath));
					if (Skeleton == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("skeleton_not_found"),
							FString::Printf(TEXT("no Skeleton at '%s'"), *SkeletonPath));
						return;
					}
					UMirrorDataTable* Mirror = nullptr;
					FString MirrorPath;
					if (Body->TryGetStringField(TEXT("mirror_table"), MirrorPath) && !MirrorPath.IsEmpty())
					{
						Mirror = Cast<UMirrorDataTable>(ResolveAsset(MirrorPath));
						if (Mirror == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("mirror_table_not_found"),
								FString::Printf(TEXT("no Mirror Data Table at '%s'"), *MirrorPath));
							return;
						}
					}
					FString Error;
					UPoseSearchSchema* Schema = Cast<UPoseSearchSchema>(CreateAsset(Path, UPoseSearchSchema::StaticClass(), nullptr, Error));
					if (Schema == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					FString Role;
					Body->TryGetStringField(TEXT("role"), Role);
					Schema->AddSkeleton(Skeleton, Mirror, Role.IsEmpty() ? UE::PoseSearch::DefaultRole : FName(*Role));
					Schema->SampleRate = IntOr(Body, TEXT("sample_rate"), Schema->SampleRate);
					if (BoolOr(Body, TEXT("default_channels"), true))
					{
						// The set the editor seeds a new schema with (pose + trajectory).
						Schema->AddDefaultChannels();
					}
					Schema->PostEditChange();
					Schema->MarkPackageDirty();
					Responder->Ok(SchemaJson(Schema));
					return;
				}

				if (Operation == TEXT("schema_info"))
				{
					UPoseSearchSchema* Schema = SchemaOrError(Body, Responder);
					if (Schema == nullptr)
					{
						return;
					}
					Responder->Ok(SchemaJson(Schema));
					return;
				}

				if (Operation == TEXT("add_channel"))
				{
					UPoseSearchSchema* Schema = SchemaOrError(Body, Responder);
					if (Schema == nullptr)
					{
						return;
					}
					FString ChannelSpec;
					if (!RequireString(Body, TEXT("channel"), ChannelSpec, Responder, TEXT("a channel class from list_channels, e.g. Position")))
					{
						return;
					}
					UClass* ChannelClass = FindChannelClass(ChannelSpec);
					if (ChannelClass == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("channel_not_found"),
							FString::Printf(TEXT("'%s' is not a Pose Search feature channel — list_channels shows them"), *ChannelSpec));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "PoseSearchAddChannel", "McpLink Pose Search Add Channel"));
					Schema->Modify();
					UPoseSearchFeatureChannel* Channel = NewObject<UPoseSearchFeatureChannel>(Schema, ChannelClass, NAME_None, RF_Transactional);
					FString Error;
					if (!ApplyProperties(Body, Channel, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"), Error);
						return;
					}
					Schema->AddChannel(Channel);
					Schema->PostEditChange();
					Schema->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = SchemaJson(Schema);
					Data->SetStringField(TEXT("added"), Channel->GetClass()->GetName());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_channel") || Operation == TEXT("remove_channel"))
				{
					UPoseSearchSchema* Schema = SchemaOrError(Body, Responder);
					if (Schema == nullptr)
					{
						return;
					}
					FArrayProperty* Property = nullptr;
					TUniquePtr<FScriptArrayHelper> Helper(ChannelArray(Schema, Property));
					const int32 Index = IntOr(Body, TEXT("index"), -1);
					if (!Helper.IsValid() || Index < 0 || Index >= Helper->Num())
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("channel_not_found"),
							FString::Printf(TEXT("'index' must be a channel index from schema_info (0..%d)"), Helper.IsValid() ? Helper->Num() - 1 : -1));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "PoseSearchChannel", "McpLink Pose Search Channel"));
					Schema->Modify();
					if (Operation == TEXT("set_channel"))
					{
						UObject* Channel = CastField<FObjectProperty>(Property->Inner)->GetObjectPropertyValue(Helper->GetRawPtr(Index));
						if (Channel == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("channel_not_found"), TEXT("that channel slot is empty"));
							return;
						}
						Channel->Modify();
						FString Error;
						if (!ApplyProperties(Body, Channel, Error))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"), Error);
							return;
						}
					}
					else
					{
						Helper->RemoveValues(Index, 1);
					}
					Schema->PostEditChange();
					Schema->MarkPackageDirty();
					Responder->Ok(SchemaJson(Schema));
					return;
				}

				if (Operation == TEXT("save_schema") || Operation == TEXT("save_database"))
				{
					UObject* Asset = Operation == TEXT("save_schema")
						? static_cast<UObject*>(SchemaOrError(Body, Responder))
						: static_cast<UObject*>(DatabaseOrError(Body, Responder));
					if (Asset == nullptr)
					{
						return;
					}
					FString Filename, Error;
					if (!SaveAsset(Asset, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("asset"), Asset->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_database"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("the new asset's package path, e.g. /Game/MM/PSD_Hero")))
					{
						return;
					}
					UPoseSearchSchema* Schema = SchemaOrError(Body, Responder);
					if (Schema == nullptr)
					{
						return;
					}
					FString Error;
					UPoseSearchDatabase* Database = Cast<UPoseSearchDatabase>(CreateAsset(Path, UPoseSearchDatabase::StaticClass(), nullptr, Error));
					if (Database == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					Database->Schema = Schema;
					FString PropError;
					if (!ApplyProperties(Body, Database, PropError))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"), PropError);
						return;
					}
					Database->MarkPackageDirty();
					Responder->Ok(DatabaseJson(Database));
					return;
				}

				if (Operation == TEXT("database_info"))
				{
					UPoseSearchDatabase* Database = DatabaseOrError(Body, Responder);
					if (Database == nullptr)
					{
						return;
					}
					Responder->Ok(DatabaseJson(Database));
					return;
				}

				if (Operation == TEXT("set_database"))
				{
					UPoseSearchDatabase* Database = DatabaseOrError(Body, Responder);
					if (Database == nullptr)
					{
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "PoseSearchDatabase", "McpLink Pose Search Database"));
					Database->Modify();
					FString SchemaPath;
					if (Body->TryGetStringField(TEXT("schema"), SchemaPath) && !SchemaPath.IsEmpty())
					{
						UPoseSearchSchema* Schema = Cast<UPoseSearchSchema>(ResolveAsset(SchemaPath));
						if (Schema == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("schema_not_found"),
								FString::Printf(TEXT("no Pose Search Schema at '%s'"), *SchemaPath));
							return;
						}
						Database->Schema = Schema;
					}
					FString Error;
					if (!ApplyProperties(Body, Database, Error))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"), Error);
						return;
					}
					Database->MarkPackageDirty();
					Responder->Ok(DatabaseJson(Database));
					return;
				}

				if (Operation == TEXT("add_animation"))
				{
					UPoseSearchDatabase* Database = DatabaseOrError(Body, Responder);
					if (Database == nullptr)
					{
						return;
					}
					FString AnimPath;
					if (!RequireString(Body, TEXT("animation"), AnimPath, Responder, TEXT("an Animation Sequence, Composite, Montage or Blend Space path")))
					{
						return;
					}
					UObject* Anim = ResolveAsset(AnimPath);
					if (Anim == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("animation_not_found"),
							FString::Printf(TEXT("no animation asset at '%s'"), *AnimPath));
						return;
					}
					FPoseSearchDatabaseAnimationAsset Entry;
					const TSharedPtr<FJsonObject>* Properties = nullptr;
					if (Body->TryGetObjectField(TEXT("properties"), Properties) && Properties->IsValid()
						&& !FJsonObjectConverter::JsonObjectToUStruct((*Properties).ToSharedRef(), FPoseSearchDatabaseAnimationAsset::StaticStruct(), &Entry))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"),
							TEXT("some property in 'properties' does not exist on FPoseSearchDatabaseAnimationAsset (bEnabled, MirrorOption, SamplingRange, bDisableReselection, …)"));
						return;
					}
					Entry.AnimAsset = Anim;
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "PoseSearchAddAnimation", "McpLink Pose Search Add Animation"));
					Database->Modify();
					Database->AddAnimationAsset(Entry);
					Database->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = DatabaseJson(Database);
					Data->SetNumberField(TEXT("added_index"), Database->GetNumAnimationAssets() - 1);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_animation") || Operation == TEXT("remove_animation"))
				{
					UPoseSearchDatabase* Database = DatabaseOrError(Body, Responder);
					if (Database == nullptr)
					{
						return;
					}
					const int32 Index = IntOr(Body, TEXT("index"), -1);
					if (Index < 0 || Index >= Database->GetNumAnimationAssets())
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("animation_not_found"),
							FString::Printf(TEXT("'index' must be an entry index from database_info (0..%d)"), Database->GetNumAnimationAssets() - 1));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "PoseSearchAnimation", "McpLink Pose Search Animation"));
					Database->Modify();
					if (Operation == TEXT("set_animation"))
					{
						FPoseSearchDatabaseAnimationAsset* Entry = Database->GetMutableDatabaseAnimationAsset(Index);
						const TSharedPtr<FJsonObject>* Properties = nullptr;
						if (Entry == nullptr || !Body->TryGetObjectField(TEXT("properties"), Properties) || !Properties->IsValid()
							|| !FJsonObjectConverter::JsonObjectToUStruct((*Properties).ToSharedRef(), FPoseSearchDatabaseAnimationAsset::StaticStruct(), Entry))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"),
								TEXT("'properties' must be an object of FPoseSearchDatabaseAnimationAsset fields (bEnabled, MirrorOption, SamplingRange, …)"));
							return;
						}
					}
					else
					{
						Database->RemoveAnimationAssetAt(Index);
					}
					Database->MarkPackageDirty();
					Responder->Ok(DatabaseJson(Database));
					return;
				}

				if (Operation == TEXT("build_index"))
				{
					UPoseSearchDatabase* Database = DatabaseOrError(Body, Responder);
					if (Database == nullptr)
					{
						return;
					}
					if (Database->Schema == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_schema"),
							TEXT("the database has no schema — set_database with 'schema' first"));
						return;
					}
					using namespace UE::PoseSearch;
					ERequestAsyncBuildFlag Flags = ERequestAsyncBuildFlag::NewRequest;
					if (BoolOr(Body, TEXT("wait"), true))
					{
						Flags |= ERequestAsyncBuildFlag::WaitForCompletion;
					}
					const EAsyncBuildIndexResult Result = FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(Database, Flags);
					const TSharedRef<FJsonObject> Data = DatabaseJson(Database);
					Data->SetStringField(TEXT("build"), BuildResultName(Result));
					if (Result == EAsyncBuildIndexResult::Failed)
					{
						Data->SetStringField(TEXT("note"), TEXT("indexing failed — the reasons are in the log (get_logs, category LogPoseSearch): usually an animation on a different skeleton, or a channel referencing a bone the skeleton lacks"));
					}
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected list_channels, create_schema, schema_info, add_channel, set_channel, remove_channel, save_schema, ")
						TEXT("create_database, database_info, set_database, add_animation, set_animation, remove_animation, build_index or save_database"),
						*Operation));
			});
	}
}
