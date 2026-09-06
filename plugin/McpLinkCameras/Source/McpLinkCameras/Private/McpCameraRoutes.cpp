// Gameplay Cameras: camera rig assets (a tree of camera nodes owned by the
// rig, connected through the nodes' own object properties), camera assets (a
// director pointing at rigs), the headless build, and activation on a player
// in PIE. The editor's graph is only a view of the object tree, so the tree is
// authored directly; nodes are also registered in the rig's connectable-object
// list so the graph shows them without a "please re-save" warning.

#include "Build/CameraAssetBuilder.h"
#include "Build/CameraBuildContext.h"
#include "Build/CameraBuildLog.h"
#include "Build/CameraBuildStatus.h"
#include "Build/CameraRigAssetBuilder.h"
#include "Core/CameraAsset.h"
#include "Core/CameraAssetReference.h"
#include "Core/CameraDirector.h"
#include "Core/CameraNode.h"
#include "Core/CameraRigAsset.h"
#include "Core/ObjectTreeGraphRootObject.h"
#include "Directors/SingleCameraDirector.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/GameplayCameraActor.h"
#include "GameFramework/GameplayCameraComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "JsonObjectConverter.h"
#include "Kismet/GameplayStatics.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace GameplayCameras
	{
		UClass* FindSubclass(UClass* Base, const FString& Spec)
		{
			if (UClass* Direct = ResolveClass(Spec); Direct != nullptr && Direct->IsChildOf(Base) && !Direct->HasAnyClassFlags(CLASS_Abstract))
			{
				return Direct;
			}
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (!It->IsChildOf(Base) || It->HasAnyClassFlags(CLASS_Abstract))
				{
					continue;
				}
				// "BoomArm" matches UBoomArmCameraNode, "Single" matches USingleCameraDirector.
				if (It->GetName().Equals(Spec, ESearchCase::IgnoreCase)
					|| It->GetName().Equals(Spec + TEXT("CameraNode"), ESearchCase::IgnoreCase)
					|| It->GetName().Equals(Spec + TEXT("CameraDirector"), ESearchCase::IgnoreCase)
					|| It->GetDisplayNameText().ToString().Equals(Spec, ESearchCase::IgnoreCase))
				{
					return *It;
				}
			}
			return nullptr;
		}

		const TCHAR* BuildStatusName(ECameraBuildStatus Status)
		{
			switch (Status)
			{
			case ECameraBuildStatus::Clean: return TEXT("clean");
			case ECameraBuildStatus::CleanWithWarnings: return TEXT("clean_with_warnings");
			case ECameraBuildStatus::WithErrors: return TEXT("errors");
			default: return TEXT("dirty");
			}
		}

		TArray<TSharedPtr<FJsonValue>> LogJson(const UE::Cameras::FCameraBuildLog& Log)
		{
			TArray<TSharedPtr<FJsonValue>> Out;
			for (const UE::Cameras::FCameraBuildLogMessage& Message : Log.GetMessages())
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("severity"), Message.Severity == EMessageSeverity::Error ? TEXT("error")
					: Message.Severity == EMessageSeverity::Warning || Message.Severity == EMessageSeverity::PerformanceWarning ? TEXT("warning") : TEXT("info"));
				Entry->SetStringField(TEXT("message"), Message.Text.ToString());
				Entry->SetStringField(TEXT("object"), Message.Object != nullptr ? Message.Object->GetName() : FString());
				Out.Add(MakeShared<FJsonValueObject>(Entry));
			}
			return Out;
		}

		// Object properties that hold camera nodes are the tree's edges.
		bool IsNodeSlot(const FProperty* Property)
		{
			if (const FObjectProperty* Object = CastField<FObjectProperty>(Property))
			{
				return Object->PropertyClass != nullptr && Object->PropertyClass->IsChildOf(UCameraNode::StaticClass());
			}
			if (const FArrayProperty* Array = CastField<FArrayProperty>(Property))
			{
				return IsNodeSlot(Array->Inner);
			}
			return false;
		}

		TSharedRef<FJsonObject> PropertiesJson(const UObject* Object)
		{
			const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
			FJsonObjectConverter::UStructToJsonObject(Object->GetClass(), Object, Properties, CPF_Edit, CPF_Deprecated, nullptr,
				EJsonObjectConversionFlags::SkipStandardizeCase);
			// Drop the child slots (they are edges, reported as children) and
			// the build-only bits of each camera parameter.
			for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
			{
				if (IsNodeSlot(*It))
				{
					Properties->RemoveField(It->GetName());
				}
			}
			for (auto& Pair : Properties->Values)
			{
				const TSharedPtr<FJsonObject>* Param = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Param) && (*Param)->HasField(TEXT("VariableID")))
				{
					(*Param)->RemoveField(TEXT("VariableID"));
				}
			}
			return Properties;
		}

		TSharedRef<FJsonObject> NodeJson(const UCameraNode* Node, int32 Depth)
		{
			const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("node"), Node->GetName());
			Out->SetStringField(TEXT("class"), Node->GetClass()->GetName());
			Out->SetStringField(TEXT("display_name"), Node->GetClass()->GetDisplayNameText().ToString());
			Out->SetObjectField(TEXT("properties"), PropertiesJson(Node));
			TArray<TSharedPtr<FJsonValue>> Children;
			for (TFieldIterator<FProperty> It(Node->GetClass()); It; ++It)
			{
				if (!IsNodeSlot(*It))
				{
					continue;
				}
				if (const FObjectProperty* Slot = CastField<FObjectProperty>(*It))
				{
					if (const UCameraNode* Child = Cast<UCameraNode>(Slot->GetObjectPropertyValue_InContainer(Node)))
					{
						const TSharedRef<FJsonObject> Entry = Depth < 12 ? NodeJson(Child, Depth + 1) : MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("slot"), Slot->GetName());
						Children.Add(MakeShared<FJsonValueObject>(Entry));
					}
					else
					{
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("slot"), Slot->GetName());
						Entry->SetStringField(TEXT("accepts"), Slot->PropertyClass->GetName());
						Children.Add(MakeShared<FJsonValueObject>(Entry));
					}
				}
				else if (const FArrayProperty* Array = CastField<FArrayProperty>(*It))
				{
					FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Node));
					for (int32 Index = 0; Index < Helper.Num(); ++Index)
					{
						if (const UCameraNode* Child = Cast<UCameraNode>(CastField<FObjectProperty>(Array->Inner)->GetObjectPropertyValue(Helper.GetRawPtr(Index))))
						{
							const TSharedRef<FJsonObject> Entry = Depth < 12 ? NodeJson(Child, Depth + 1) : MakeShared<FJsonObject>();
							Entry->SetStringField(TEXT("slot"), Array->GetName());
							Entry->SetNumberField(TEXT("slot_index"), Index);
							Children.Add(MakeShared<FJsonValueObject>(Entry));
						}
					}
				}
			}
			Out->SetArrayField(TEXT("children"), Children);
			return Out;
		}

		TSharedRef<FJsonObject> RigJson(UCameraRigAsset* Rig)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("rig"), Rig->GetPathName());
			Data->SetStringField(TEXT("build_status"), BuildStatusName(Rig->GetBuildStatus()));
			if (Rig->RootNode != nullptr)
			{
				Data->SetObjectField(TEXT("root"), NodeJson(Rig->RootNode, 0));
			}
			Data->SetNumberField(TEXT("enter_transitions"), Rig->EnterTransitions.Num());
			Data->SetNumberField(TEXT("exit_transitions"), Rig->ExitTransitions.Num());
			Data->SetNumberField(TEXT("blendable_parameters"), Rig->Interface.BlendableParameters.Num());
			return Data;
		}

		TSharedRef<FJsonObject> CameraJson(UCameraAsset* Camera)
		{
			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("camera"), Camera->GetPathName());
			Data->SetStringField(TEXT("build_status"), BuildStatusName(Camera->GetBuildStatus()));
			const UCameraDirector* Director = Camera->GetCameraDirector();
			Data->SetStringField(TEXT("director"), Director != nullptr ? Director->GetClass()->GetName() : FString());
			if (const USingleCameraDirector* Single = Cast<USingleCameraDirector>(Director))
			{
				Data->SetStringField(TEXT("rig"), Single->CameraRig != nullptr ? Single->CameraRig->GetPathName() : FString());
			}
			if (Director != nullptr)
			{
				Data->SetObjectField(TEXT("director_properties"), PropertiesJson(Director));
			}
			Data->SetNumberField(TEXT("enter_transitions"), Camera->GetEnterTransitions().Num());
			return Data;
		}

		UCameraRigAsset* RigOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("rig"), Path, Responder, TEXT("a Camera Rig asset path")))
			{
				return nullptr;
			}
			UCameraRigAsset* Rig = Cast<UCameraRigAsset>(ResolveAsset(Path));
			if (Rig == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("rig_not_found"),
					FString::Printf(TEXT("no Camera Rig asset at '%s'"), *Path));
			}
			return Rig;
		}

		UCameraAsset* CameraOrError(const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("camera"), Path, Responder, TEXT("a Camera asset path")))
			{
				return nullptr;
			}
			UCameraAsset* Camera = Cast<UCameraAsset>(ResolveAsset(Path));
			if (Camera == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("camera_not_found"),
					FString::Printf(TEXT("no Camera asset at '%s'"), *Path));
			}
			return Camera;
		}

		UCameraNode* FindNode(UCameraRigAsset* Rig, const FString& Name)
		{
			return FindObject<UCameraNode>(Rig, *Name);
		}

		// Camera parameters are {Value, Variable} structs; a bare number,
		// array or object in their place is wrapped as the literal value.
		void NormalizeParameters(const TSharedRef<FJsonObject>& Properties, const UStruct* Struct)
		{
			for (auto& Pair : Properties->Values)
			{
				const FStructProperty* Property = CastField<FStructProperty>(Struct->FindPropertyByName(FName(Pair.Key.ToView())));
				if (Property == nullptr || !Property->Struct->GetName().EndsWith(TEXT("CameraParameter")) || !Pair.Value.IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>* AsObject = nullptr;
				if (Pair.Value->TryGetObject(AsObject) && ((*AsObject)->HasField(TEXT("Value")) || (*AsObject)->HasField(TEXT("Variable"))))
				{
					continue;
				}
				TSharedPtr<FJsonValue> Literal = Pair.Value;
				const TArray<TSharedPtr<FJsonValue>>* AsArray = nullptr;
				if (Pair.Value->TryGetArray(AsArray))
				{
					const FStructProperty* ValueProperty = CastField<FStructProperty>(Property->Struct->FindPropertyByName(TEXT("Value")));
					const TSharedRef<FJsonObject> Composite = MakeShared<FJsonObject>();
					const bool bRotator = ValueProperty != nullptr && ValueProperty->Struct->GetName().StartsWith(TEXT("Rotator"));
					const TCHAR* Keys[3] = { bRotator ? TEXT("Pitch") : TEXT("X"), bRotator ? TEXT("Yaw") : TEXT("Y"), bRotator ? TEXT("Roll") : TEXT("Z") };
					for (int32 Index = 0; Index < AsArray->Num() && Index < 3; ++Index)
					{
						Composite->SetNumberField(Keys[Index], (*AsArray)[Index]->AsNumber());
					}
					Literal = MakeShared<FJsonValueObject>(Composite);
				}
				const TSharedRef<FJsonObject> Wrapped = MakeShared<FJsonObject>();
				Wrapped->SetField(TEXT("Value"), Literal);
				Pair.Value = MakeShared<FJsonValueObject>(Wrapped);
			}
		}

		bool ApplyProperties(const TSharedRef<FJsonObject>& Body, UObject* Target, FString& OutError)
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Body->TryGetObjectField(TEXT("properties"), Properties) || !Properties->IsValid())
			{
				return true;
			}
			NormalizeParameters((*Properties).ToSharedRef(), Target->GetClass());
			FText Reason;
			if (!FJsonObjectConverter::JsonObjectToUStruct((*Properties).ToSharedRef(), Target->GetClass(), Target, 0, 0, false, &Reason))
			{
				OutError = FString::Printf(TEXT("'properties' did not import onto %s: %s — camera parameters take a literal (75, [0,0,300]) or {\"Value\": …} / {\"Variable\": \"/Game/…\"}"),
					*Target->GetClass()->GetName(), *Reason.ToString());
				return false;
			}
			return true;
		}

		// Attaches Child to Parent through a slot: an explicit property name,
		// or the first array/object slot whose class accepts the child.
		bool Attach(UCameraNode* Parent, UCameraNode* Child, const FString& SlotSpec, FString& OutError)
		{
			for (TFieldIterator<FProperty> It(Parent->GetClass()); It; ++It)
			{
				if (!IsNodeSlot(*It) || (!SlotSpec.IsEmpty() && !It->GetName().Equals(SlotSpec, ESearchCase::IgnoreCase)))
				{
					continue;
				}
				if (const FArrayProperty* Array = CastField<FArrayProperty>(*It))
				{
					if (!Child->IsA(CastField<FObjectProperty>(Array->Inner)->PropertyClass))
					{
						continue;
					}
					FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Parent));
					const int32 Index = Helper.AddValue();
					CastField<FObjectProperty>(Array->Inner)->SetObjectPropertyValue(Helper.GetRawPtr(Index), Child);
					return true;
				}
				if (const FObjectProperty* Slot = CastField<FObjectProperty>(*It))
				{
					if (!Child->IsA(Slot->PropertyClass))
					{
						if (!SlotSpec.IsEmpty())
						{
							OutError = FString::Printf(TEXT("slot %s takes a %s, not a %s"), *Slot->GetName(), *Slot->PropertyClass->GetName(), *Child->GetClass()->GetName());
							return false;
						}
						continue;
					}
					Slot->SetObjectPropertyValue_InContainer(Parent, Child);
					return true;
				}
			}
			OutError = FString::Printf(TEXT("%s has no slot for a %s%s — an Array (Sequence) node takes any child in Children, other nodes have typed slots such as InputSlot"),
				*Parent->GetClass()->GetName(), *Child->GetClass()->GetName(), SlotSpec.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" named '%s'"), *SlotSpec));
			return false;
		}

		// Clears every reference to Node held by another node or the rig root.
		int32 Detach(UCameraRigAsset* Rig, UCameraNode* Node)
		{
			int32 Cleared = 0;
			if (Rig->RootNode == Node)
			{
				Rig->RootNode = nullptr;
				++Cleared;
			}
			for (TObjectIterator<UCameraNode> It; It; ++It)
			{
				if (!It->IsIn(Rig) || *It == Node)
				{
					continue;
				}
				for (TFieldIterator<FProperty> Field(It->GetClass()); Field; ++Field)
				{
					if (!IsNodeSlot(*Field))
					{
						continue;
					}
					if (const FArrayProperty* Array = CastField<FArrayProperty>(*Field))
					{
						FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(*It));
						for (int32 Index = Helper.Num() - 1; Index >= 0; --Index)
						{
							if (CastField<FObjectProperty>(Array->Inner)->GetObjectPropertyValue(Helper.GetRawPtr(Index)) == Node)
							{
								Helper.RemoveValues(Index, 1);
								++Cleared;
							}
						}
					}
					else if (const FObjectProperty* Slot = CastField<FObjectProperty>(*Field); Slot != nullptr && Slot->GetObjectPropertyValue_InContainer(*It) == Node)
					{
						Slot->SetObjectPropertyValue_InContainer(*It, nullptr);
						++Cleared;
					}
				}
			}
			return Cleared;
		}
	}

	void RegisterCameraRoutes(FMcpLinkCoreModule& Core)
	{
		using namespace GameplayCameras;

		Core.RegisterRoute(TEXT("/api/cameras/gameplay"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_node_types") || Operation == TEXT("list_director_types"))
				{
					UClass* Base = Operation == TEXT("list_node_types") ? UCameraNode::StaticClass() : UCameraDirector::StaticClass();
					FString Filter;
					Body->TryGetStringField(TEXT("filter"), Filter);
					TArray<TSharedPtr<FJsonValue>> Types;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						if (!It->IsChildOf(Base) || It->HasAnyClassFlags(CLASS_Abstract) || It->GetName().StartsWith(TEXT("SKEL_")) || It->GetName().StartsWith(TEXT("REINST_")))
						{
							continue;
						}
						const FString Category = It->GetMetaData(TEXT("CameraNodeCategories"));
						if (!Filter.IsEmpty() && !It->GetName().Contains(Filter) && !Category.Contains(Filter))
						{
							continue;
						}
						const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
						Entry->SetStringField(TEXT("class"), It->GetName());
						Entry->SetStringField(TEXT("path"), It->GetPathName());
						Entry->SetStringField(TEXT("display_name"), It->GetDisplayNameText().ToString());
						Entry->SetStringField(TEXT("categories"), Category);
						TArray<TSharedPtr<FJsonValue>> Slots;
						for (TFieldIterator<FProperty> Field(*It); Field; ++Field)
						{
							if (IsNodeSlot(*Field))
							{
								const FArrayProperty* Array = CastField<FArrayProperty>(*Field);
								const FObjectProperty* Object = CastField<FObjectProperty>(Array != nullptr ? Array->Inner : *Field);
								Slots.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s%s: %s"), *Field->GetName(), Array != nullptr ? TEXT("[]") : TEXT(""), *Object->PropertyClass->GetName())));
							}
						}
						Entry->SetArrayField(TEXT("slots"), Slots);
						Entry->SetObjectField(TEXT("defaults"), PropertiesJson(It->GetDefaultObject()));
						Types.Add(MakeShared<FJsonValueObject>(Entry));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("types"), Types);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_rig"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("the new asset's package path, e.g. /Game/Cameras/CR_ThirdPerson")))
					{
						return;
					}
					FString Error;
					UCameraRigAsset* Rig = Cast<UCameraRigAsset>(CreateAsset(Path, UCameraRigAsset::StaticClass(), nullptr, Error));
					if (Rig == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					Rig->MarkPackageDirty();
					Responder->Ok(RigJson(Rig));
					return;
				}

				if (Operation == TEXT("rig_info"))
				{
					UCameraRigAsset* Rig = RigOrError(Body, Responder);
					if (Rig == nullptr)
					{
						return;
					}
					Responder->Ok(RigJson(Rig));
					return;
				}

				if (Operation == TEXT("add_node"))
				{
					UCameraRigAsset* Rig = RigOrError(Body, Responder);
					if (Rig == nullptr)
					{
						return;
					}
					FString ClassSpec;
					if (!RequireString(Body, TEXT("node"), ClassSpec, Responder, TEXT("a camera node class from list_node_types, e.g. BoomArm, Offset, FieldOfView or Array")))
					{
						return;
					}
					UClass* NodeClass = FindSubclass(UCameraNode::StaticClass(), ClassSpec);
					if (NodeClass == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_type_not_found"),
							FString::Printf(TEXT("'%s' is not a camera node class — list_node_types shows them"), *ClassSpec));
						return;
					}
					FString ParentName, Slot, Name;
					Body->TryGetStringField(TEXT("parent"), ParentName);
					Body->TryGetStringField(TEXT("slot"), Slot);
					Body->TryGetStringField(TEXT("name"), Name);
					UCameraNode* Parent = nullptr;
					if (!ParentName.IsEmpty())
					{
						Parent = FindNode(Rig, ParentName);
						if (Parent == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("parent_not_found"),
								FString::Printf(TEXT("no node '%s' in the rig — rig_info lists them"), *ParentName));
							return;
						}
					}
					else if (Rig->RootNode != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("root_taken"),
							FString::Printf(TEXT("the rig already has a root node (%s) — pass 'parent' (an Array node's name to append to it)"), *Rig->RootNode->GetName()));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "CameraAddNode", "McpLink Camera Rig Add Node"));
					Rig->Modify();
					FName ObjectName = NAME_None;
					if (!Name.IsEmpty())
					{
						if (StaticFindObjectFast(nullptr, Rig, FName(*Name)) != nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("name_taken"),
								FString::Printf(TEXT("the rig already has an object named '%s'"), *Name));
							return;
						}
						ObjectName = FName(*Name);
					}
					UCameraNode* Node = NewObject<UCameraNode>(Rig, NodeClass, ObjectName, RF_Transactional);
					FString Error;
					if (!ApplyProperties(Body, Node, Error))
					{
						Node->MarkAsGarbage();
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"), Error);
						return;
					}
					if (Parent != nullptr)
					{
						Parent->Modify();
						if (!Attach(Parent, Node, Slot, Error))
						{
							Node->MarkAsGarbage();
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_slot"), Error);
							return;
						}
					}
					else
					{
						Rig->RootNode = Node;
					}
					static_cast<IObjectTreeGraphRootObject*>(Rig)->AddConnectableObject(UCameraRigAsset::NodeTreeGraphName, Node);
					Rig->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = RigJson(Rig);
					Data->SetStringField(TEXT("added"), Node->GetName());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("set_node") || Operation == TEXT("remove_node"))
				{
					UCameraRigAsset* Rig = RigOrError(Body, Responder);
					if (Rig == nullptr)
					{
						return;
					}
					FString NodeName;
					if (!RequireString(Body, TEXT("node"), NodeName, Responder, TEXT("a node name from rig_info")))
					{
						return;
					}
					UCameraNode* Node = FindNode(Rig, NodeName);
					if (Node == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"),
							FString::Printf(TEXT("no node '%s' in the rig — rig_info lists them"), *NodeName));
						return;
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "CameraNode", "McpLink Camera Rig Node"));
					Rig->Modify();
					Node->Modify();
					if (Operation == TEXT("set_node"))
					{
						FString Error;
						if (!ApplyProperties(Body, Node, Error))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"), Error);
							return;
						}
					}
					else
					{
						Detach(Rig, Node);
						static_cast<IObjectTreeGraphRootObject*>(Rig)->RemoveConnectableObject(UCameraRigAsset::NodeTreeGraphName, Node);
						Node->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
						Node->MarkAsGarbage();
					}
					Rig->MarkPackageDirty();
					Responder->Ok(RigJson(Rig));
					return;
				}

				if (Operation == TEXT("build_rig"))
				{
					UCameraRigAsset* Rig = RigOrError(Body, Responder);
					if (Rig == nullptr)
					{
						return;
					}
					UE::Cameras::FCameraBuildLog Log;
					UE::Cameras::FCameraBuildContext Context(Log, UE::Cameras::ECameraBuildReason::UserAction);
					UE::Cameras::FCameraRigAssetBuilder(Context).BuildCameraRig(Rig);
					const TSharedRef<FJsonObject> Data = RigJson(Rig);
					Data->SetArrayField(TEXT("messages"), LogJson(Log));
					Data->SetBoolField(TEXT("ok"), !Log.HasErrors());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create_camera") || Operation == TEXT("set_camera"))
				{
					UCameraAsset* Camera = nullptr;
					if (Operation == TEXT("create_camera"))
					{
						FString Path;
						if (!RequireString(Body, TEXT("path"), Path, Responder, TEXT("the new asset's package path, e.g. /Game/Cameras/CAM_ThirdPerson")))
						{
							return;
						}
						FString Error;
						Camera = Cast<UCameraAsset>(CreateAsset(Path, UCameraAsset::StaticClass(), nullptr, Error));
						if (Camera == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
							return;
						}
					}
					else
					{
						Camera = CameraOrError(Body, Responder);
						if (Camera == nullptr)
						{
							return;
						}
					}
					const FScopedTransaction Transaction(NSLOCTEXT("McpLink", "CameraAsset", "McpLink Camera Asset"));
					Camera->Modify();
					FString DirectorSpec;
					Body->TryGetStringField(TEXT("director"), DirectorSpec);
					UCameraDirector* Director = Camera->GetCameraDirector();
					if (!DirectorSpec.IsEmpty() || Director == nullptr)
					{
						UClass* DirectorClass = DirectorSpec.IsEmpty() ? USingleCameraDirector::StaticClass() : FindSubclass(UCameraDirector::StaticClass(), DirectorSpec);
						if (DirectorClass == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("director_not_found"),
								FString::Printf(TEXT("'%s' is not a camera director class — list_director_types shows them"), *DirectorSpec));
							return;
						}
						if (Director == nullptr || Director->GetClass() != DirectorClass)
						{
							// The director's outer is the camera asset, as the factory does it.
							Director = NewObject<UCameraDirector>(Camera, DirectorClass, NAME_None, RF_Transactional);
							Camera->SetCameraDirector(Director);
							FCameraDirectorFactoryCreateParams Params;
							Director->FactoryCreateAsset(Params);
						}
					}
					FString RigPath;
					if (Body->TryGetStringField(TEXT("rig"), RigPath) && !RigPath.IsEmpty())
					{
						UCameraRigAsset* Rig = Cast<UCameraRigAsset>(ResolveAsset(RigPath));
						if (Rig == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("rig_not_found"),
								FString::Printf(TEXT("no Camera Rig asset at '%s'"), *RigPath));
							return;
						}
						if (USingleCameraDirector* Single = Cast<USingleCameraDirector>(Director))
						{
							Single->Modify();
							Single->CameraRig = Rig;
						}
						else
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("director_takes_no_rig"),
								FString::Printf(TEXT("a %s does not take a single rig — set its properties with 'director_properties' instead"), *Director->GetClass()->GetName()));
							return;
						}
					}
					const TSharedPtr<FJsonObject>* DirectorProperties = nullptr;
					if (Body->TryGetObjectField(TEXT("director_properties"), DirectorProperties) && DirectorProperties->IsValid() && Director != nullptr)
					{
						FText Reason;
						if (!FJsonObjectConverter::JsonObjectToUStruct((*DirectorProperties).ToSharedRef(), Director->GetClass(), Director, 0, 0, false, &Reason))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_properties"),
								FString::Printf(TEXT("'director_properties' did not import onto %s: %s"), *Director->GetClass()->GetName(), *Reason.ToString()));
							return;
						}
					}
					Camera->MarkPackageDirty();
					Responder->Ok(CameraJson(Camera));
					return;
				}

				if (Operation == TEXT("camera_info"))
				{
					UCameraAsset* Camera = CameraOrError(Body, Responder);
					if (Camera == nullptr)
					{
						return;
					}
					Responder->Ok(CameraJson(Camera));
					return;
				}

				if (Operation == TEXT("build_camera"))
				{
					UCameraAsset* Camera = CameraOrError(Body, Responder);
					if (Camera == nullptr)
					{
						return;
					}
					UE::Cameras::FCameraBuildLog Log;
					UE::Cameras::FCameraBuildContext Context(Log, UE::Cameras::ECameraBuildReason::UserAction);
					// Builds the referenced rigs as well.
					UE::Cameras::FCameraAssetBuilder(Context).BuildCamera(Camera);
					const TSharedRef<FJsonObject> Data = CameraJson(Camera);
					Data->SetArrayField(TEXT("messages"), LogJson(Log));
					Data->SetBoolField(TEXT("ok"), !Log.HasErrors());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("asset"), Path, Responder, TEXT("a Camera or Camera Rig asset path")))
					{
						return;
					}
					UObject* Asset = ResolveAsset(Path);
					if (Asset == nullptr || (!Asset->IsA<UCameraAsset>() && !Asset->IsA<UCameraRigAsset>()))
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
							FString::Printf(TEXT("no Camera or Camera Rig asset at '%s'"), *Path));
						return;
					}
					// PreSave runs the build, so a saved asset is a built one.
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

				if (Operation == TEXT("activate"))
				{
					UCameraAsset* Camera = CameraOrError(Body, Responder);
					if (Camera == nullptr)
					{
						return;
					}
					UWorld* World = ResolveWorldOrError(Body, Responder);
					if (World == nullptr)
					{
						return;
					}
					if (!World->HasBegunPlay())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("not_playing"),
							TEXT("a camera is activated on a player controller in a running world — start PIE and pass world='pie'"));
						return;
					}
					APlayerController* Controller = UGameplayStatics::GetPlayerController(World, IntOr(Body, TEXT("player_index"), 0));
					if (Controller == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("no_player"), TEXT("that world has no player controller at 'player_index'"));
						return;
					}
					FVector Location = FVector::ZeroVector;
					FRotator Rotation = FRotator::ZeroRotator;
					if (!GetVector(Body, TEXT("location"), Location) && Controller->GetPawn() != nullptr)
					{
						Location = Controller->GetPawn()->GetActorLocation();
					}
					GetRotator(Body, TEXT("rotation"), Rotation);
					FActorSpawnParameters Params;
					FString Name;
					if (Body->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
					{
						Params.Name = FName(*Name);
						Params.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Requested;
					}
					AGameplayCameraActor* Actor = World->SpawnActor<AGameplayCameraActor>(AGameplayCameraActor::StaticClass(), Location, Rotation, Params);
					if (Actor == nullptr || Actor->GetCameraComponent() == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("spawn_failed"), TEXT("the world refused to spawn a Gameplay Camera actor"));
						return;
					}
					Actor->GetCameraComponent()->CameraReference.SetCameraAsset(Camera);
					Actor->GetCameraComponent()->ActivateCameraForPlayerController(Controller, true);
					const TSharedRef<FJsonObject> Data = CameraJson(Camera);
					Data->SetStringField(TEXT("actor"), Actor->GetPathName());
					Data->SetStringField(TEXT("view_target"), Controller->GetViewTarget() != nullptr ? Controller->GetViewTarget()->GetName() : FString());
					Data->SetArrayField(TEXT("location"), VectorToJson(Actor->GetActorLocation()));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("deactivate"))
				{
					UWorld* World = ResolveWorldOrError(Body, Responder);
					if (World == nullptr)
					{
						return;
					}
					FString ActorSpec;
					if (!RequireString(Body, TEXT("actor"), ActorSpec, Responder, TEXT("the Gameplay Camera actor from activate")))
					{
						return;
					}
					AGameplayCameraActor* Actor = Cast<AGameplayCameraActor>(ResolveActor(World, ActorSpec));
					if (Actor == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("actor_not_found"),
							FString::Printf(TEXT("no Gameplay Camera actor '%s' in that world"), *ActorSpec));
						return;
					}
					APlayerController* Controller = UGameplayStatics::GetPlayerController(World, IntOr(Body, TEXT("player_index"), 0));
					if (Controller != nullptr && Controller->GetPawn() != nullptr)
					{
						Controller->SetViewTarget(Controller->GetPawn());
					}
					Actor->Destroy();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("view_target"), Controller != nullptr && Controller->GetViewTarget() != nullptr ? Controller->GetViewTarget()->GetName() : FString());
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(TEXT("unknown operation '%s' — expected list_node_types, list_director_types, create_rig, rig_info, add_node, set_node, remove_node, ")
						TEXT("build_rig, create_camera, set_camera, camera_info, build_camera, save, activate or deactivate"), *Operation));
			});
	}
}
