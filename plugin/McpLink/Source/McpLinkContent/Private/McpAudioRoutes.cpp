// Sound Cue authoring: build the node tree that a cue plays.
//
// Like a Behavior Tree, a Sound Cue carries both a runtime tree (FirstNode and
// each node's ChildNodes) and an editor graph (SoundCueGraph). Here the
// runtime tree is the source of truth — USoundCue::LinkGraphNodesFromSoundNodes
// regenerates the graph from it — so every edit writes the tree and then
// re-links, which is the opposite of the Behavior Tree flow.
//
// Sound Classes, Submixes, Attenuation and Concurrency assets need no route of
// their own: they are plain assets with plain properties, so asset_ops create
// plus set_property covers them.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Factories/SoundCueFactoryNew.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "ScopedTransaction.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundWave.h"
#include "UObject/UObjectIterator.h"

namespace McpLink
{
	namespace
	{
		USoundCue* CueOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("cue"), Path, Responder,
				TEXT("a Sound Cue asset path, e.g. /Game/Audio/Cue_Footstep")))
			{
				return nullptr;
			}
			UObject* Object = ResolveObject(Path);
			if (Object == nullptr && !Path.Contains(TEXT(".")))
			{
				Object = ResolveObject(
					FString::Printf(TEXT("%s.%s"), *Path, *FPackageName::GetShortName(Path)));
			}
			USoundCue* Cue = Cast<USoundCue>(Object);
			if (Cue == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("cue_not_found"),
					FString::Printf(TEXT("no Sound Cue at '%s'"), *Path));
			}
			return Cue;
		}

		/// Nodes are addressed by their object name ("SoundNodeRandom_0"),
		/// which is unique inside the cue and readable in the response.
		USoundNode* FindNode(USoundCue* Cue, const FString& Name)
		{
			for (USoundNode* Node : Cue->AllNodes)
			{
				if (Node != nullptr && Node->GetName() == Name)
				{
					return Node;
				}
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> NodeToJson(const USoundCue* Cue, USoundNode* Node)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("id"), Node->GetName());
			Object->SetStringField(TEXT("class"), Node->GetClass()->GetPathName());
			// Node settings are UPROPERTYs, so set_property on this path is how
			// a caller tunes a node (a Random node's weights, a Modulator's
			// pitch range, a WavePlayer's SoundWave).
			Object->SetStringField(TEXT("path"), Node->GetPathName());
			Object->SetBoolField(TEXT("is_root"), Cue->FirstNode == Node);
			Object->SetNumberField(TEXT("max_children"), Node->GetMaxChildNodes());

			TArray<TSharedPtr<FJsonValue>> Children;
			for (const USoundNode* Child : Node->ChildNodes)
			{
				// A node with a minimum child count starts with empty slots;
				// null reads as "nothing wired here", "" reads as a node named "".
				Children.Add(Child != nullptr
					? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Child->GetName()))
					: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));
			}
			Object->SetArrayField(TEXT("children"), Children);
			return Object;
		}

		/// Attach Child under Parent at Index, growing the child array the way
		/// the editor's "add input" does. Returns false with OutError when the
		/// parent cannot take another child.
		bool AttachChild(USoundNode* Parent, USoundNode* Child, int32 Index, FString& OutError)
		{
			const int32 MaxChildren = Parent->GetMaxChildNodes();
			if (MaxChildren <= 0)
			{
				OutError = FString::Printf(
					TEXT("'%s' takes no child nodes"), *Parent->GetClass()->GetName());
				return false;
			}
			if (Index < 0)
			{
				Index = Parent->ChildNodes.Num();
			}
			if (Index >= MaxChildren)
			{
				OutError = FString::Printf(
					TEXT("'%s' takes at most %d child nodes"),
					*Parent->GetClass()->GetName(), MaxChildren);
				return false;
			}
			while (Parent->ChildNodes.Num() <= Index)
			{
				Parent->InsertChildNode(Parent->ChildNodes.Num());
			}
			Parent->ChildNodes[Index] = Child;
			return true;
		}
	}

	void RegisterAudioRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/audio/cue"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_node_classes"))
				{
					FString NameFilter;
					Body->TryGetStringField(TEXT("name_contains"), NameFilter);
					TArray<TSharedPtr<FJsonValue>> Classes;
					for (TObjectIterator<UClass> It; It; ++It)
					{
						UClass* Class = *It;
						if (!Class->IsChildOf(USoundNode::StaticClass())
							|| Class == USoundNode::StaticClass()
							|| Class->HasAnyClassFlags(
								CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
						{
							continue;
						}
						if (!NameFilter.IsEmpty() && !Class->GetName().Contains(NameFilter))
						{
							continue;
						}
						const USoundNode* Cdo = Class->GetDefaultObject<USoundNode>();
						const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
						Item->SetStringField(TEXT("class"), Class->GetPathName());
						Item->SetStringField(TEXT("name"), Class->GetName());
						Item->SetNumberField(TEXT("max_children"), Cdo->GetMaxChildNodes());
						Classes.Add(MakeShared<FJsonValueObject>(Item));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("total"), Classes.Num());
					Data->SetArrayField(TEXT("classes"), Classes);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
						TEXT("e.g. /Game/Audio/Cue_Footstep")))
					{
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateSoundCue", "McpLink Create Sound Cue"));
					USoundCueFactoryNew* Factory = NewObject<USoundCueFactoryNew>();
					FString WavePath;
					if (Body->TryGetStringField(TEXT("sound_wave"), WavePath) && !WavePath.IsEmpty())
					{
						USoundWave* Wave = Cast<USoundWave>(ResolveObject(
							WavePath.Contains(TEXT(".")) ? WavePath
								: FString::Printf(TEXT("%s.%s"), *WavePath,
									*FPackageName::GetShortName(WavePath))));
						if (Wave == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound,
								TEXT("sound_wave_not_found"),
								FString::Printf(TEXT("no Sound Wave at '%s'"), *WavePath));
							return;
						}
						// The factory wires a WavePlayer for it automatically.
						Factory->InitialSoundWave = Wave;
					}
					FString Error;
					UObject* Asset = CreateAsset(Path, USoundCue::StaticClass(), Factory, Error);
					if (Asset == nullptr)
					{
						Responder->Error(
							EHttpServerResponseCodes::Conflict, TEXT("create_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Asset->GetPathName());
					Responder->Ok(Data);
					return;
				}

				USoundCue* Cue = CueOrError(Body, Responder);
				if (!Cue) { return; }

				if (Operation == TEXT("get_graph"))
				{
					TArray<TSharedPtr<FJsonValue>> Nodes;
					for (USoundNode* Node : Cue->AllNodes)
					{
						if (Node != nullptr)
						{
							Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Cue, Node)));
						}
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("path"), Cue->GetPathName());
					Data->SetStringField(TEXT("root"),
						Cue->FirstNode ? Cue->FirstNode->GetName() : TEXT(""));
					Data->SetArrayField(TEXT("nodes"), Nodes);
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("add_node"))
				{
					FString ClassSpec;
					if (!RequireString(Body, TEXT("class"), ClassSpec, Responder,
						TEXT("a SoundNode class — list_node_classes enumerates them")))
					{
						return;
					}
					UClass* NodeClass = ResolveClass(ClassSpec);
					if (NodeClass == nullptr
						|| !NodeClass->IsChildOf(USoundNode::StaticClass())
						|| NodeClass->HasAnyClassFlags(CLASS_Abstract))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_class"),
							FString::Printf(
								TEXT("'%s' is not a concrete SoundNode class — use list_node_classes"),
								*ClassSpec));
						return;
					}

					// Resolve the parent before creating anything, so a bad
					// parent does not leave an orphan node in AllNodes.
					FString ParentName;
					USoundNode* Parent = nullptr;
					const bool bHasParent =
						Body->TryGetStringField(TEXT("parent"), ParentName) && !ParentName.IsEmpty();
					if (bHasParent)
					{
						Parent = FindNode(Cue, ParentName);
						if (Parent == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"),
								FString::Printf(
									TEXT("no node '%s' — get_graph lists node ids"), *ParentName));
							return;
						}
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "AddSoundNode", "McpLink Add Sound Node"));
					Cue->Modify();
					USoundNode* Node =
						Cue->ConstructSoundNode<USoundNode>(NodeClass, /*bSelectNewNode*/ false);
					Node->CreateStartingConnectors();

					FString Error;
					if (Parent != nullptr)
					{
						Parent->Modify();
						if (!AttachChild(Parent, Node, IntOr(Body, TEXT("index"), -1), Error))
						{
							Responder->Error(
								EHttpServerResponseCodes::BadRequest, TEXT("cannot_attach"), Error);
							return;
						}
					}
					else
					{
						// No parent means this is the cue's output node.
						Cue->FirstNode = Node;
					}

					FString WavePath;
					if (Body->TryGetStringField(TEXT("sound_wave"), WavePath) && !WavePath.IsEmpty())
					{
						USoundNodeWavePlayer* Player = Cast<USoundNodeWavePlayer>(Node);
						if (Player == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("not_a_wave_player"),
								TEXT("'sound_wave' only applies to a SoundNodeWavePlayer"));
							return;
						}
						USoundWave* Wave = Cast<USoundWave>(ResolveObject(
							WavePath.Contains(TEXT(".")) ? WavePath
								: FString::Printf(TEXT("%s.%s"), *WavePath,
									*FPackageName::GetShortName(WavePath))));
						if (Wave == nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::NotFound,
								TEXT("sound_wave_not_found"),
								FString::Printf(TEXT("no Sound Wave at '%s'"), *WavePath));
							return;
						}
						Player->SetSoundWave(Wave);
					}

					// Regenerate the editor graph from the tree we just changed.
					Cue->LinkGraphNodesFromSoundNodes();
					Cue->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = NodeToJson(Cue, Node);
					if (Parent != nullptr)
					{
						Data->SetStringField(TEXT("parent"), Parent->GetName());
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("connect"))
				{
					FString ParentName, ChildName;
					if (!RequireString(Body, TEXT("parent"), ParentName, Responder)
						|| !RequireString(Body, TEXT("child"), ChildName, Responder))
					{
						return;
					}
					USoundNode* Parent = FindNode(Cue, ParentName);
					USoundNode* Child = FindNode(Cue, ChildName);
					if (Parent == nullptr || Child == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"),
							TEXT("could not resolve both nodes — get_graph lists node ids"));
						return;
					}
					if (Parent == Child)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("invalid_connection"),
							TEXT("a node cannot be its own child"));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "ConnectSoundNodes", "McpLink Connect Sound Nodes"));
					Cue->Modify();
					Parent->Modify();
					FString Error;
					if (!AttachChild(Parent, Child, IntOr(Body, TEXT("index"), -1), Error))
					{
						Responder->Error(
							EHttpServerResponseCodes::BadRequest, TEXT("cannot_attach"), Error);
						return;
					}
					Cue->LinkGraphNodesFromSoundNodes();
					Cue->MarkPackageDirty();
					Responder->Ok(NodeToJson(Cue, Parent));
					return;
				}

				if (Operation == TEXT("set_root"))
				{
					FString NodeName;
					if (!RequireString(Body, TEXT("node"), NodeName, Responder))
					{
						return;
					}
					USoundNode* Node = FindNode(Cue, NodeName);
					if (Node == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"),
							FString::Printf(TEXT("no node '%s'"), *NodeName));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "SetSoundCueRoot", "McpLink Set Sound Cue Root"));
					Cue->Modify();
					Cue->FirstNode = Node;
					Cue->LinkGraphNodesFromSoundNodes();
					Cue->MarkPackageDirty();
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("root"), Node->GetName());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("remove_node"))
				{
					FString NodeName;
					if (!RequireString(Body, TEXT("node"), NodeName, Responder))
					{
						return;
					}
					USoundNode* Node = FindNode(Cue, NodeName);
					if (Node == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("node_not_found"),
							FString::Printf(TEXT("no node '%s'"), *NodeName));
						return;
					}
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "RemoveSoundNode", "McpLink Remove Sound Node"));
					Cue->Modify();
					// Unhook it everywhere before dropping it, or the cue keeps
					// a dangling child slot that silences the branch.
					for (USoundNode* Other : Cue->AllNodes)
					{
						if (Other == nullptr || Other == Node)
						{
							continue;
						}
						for (int32 Index = Other->ChildNodes.Num() - 1; Index >= 0; --Index)
						{
							if (Other->ChildNodes[Index] == Node)
							{
								Other->Modify();
								Other->ChildNodes[Index] = nullptr;
							}
						}
					}
					if (Cue->FirstNode == Node)
					{
						Cue->FirstNode = nullptr;
					}
					Cue->AllNodes.Remove(Node);
					Cue->LinkGraphNodesFromSoundNodes();
					Cue->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("removed"), NodeName);
					Data->SetStringField(TEXT("root"),
						Cue->FirstNode ? Cue->FirstNode->GetName() : TEXT(""));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Cue, Filename, Error))
					{
						Responder->Error(
							EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetBoolField(TEXT("saved"), true);
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use create, list_node_classes, get_graph, ")
						TEXT("add_node, connect, set_root, remove_node or save"),
						*Operation));
			});
	}
}
