// Viewport capture. The screenshot arrives a frame or more after the request,
// so the responder is stashed and completed from the capture delegate (with a
// ticker timeout as a backstop).
//
// Three sources: the viewport as it is (its pixels read straight back), a
// high-resolution render of it (the engine's High Resolution Screenshot,
// which redraws the view at the requested size), and a camera placed
// anywhere (a transient scene capture into a render target).

#include "Components/SceneCaptureComponent2D.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HighResScreenshot.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UnrealClient.h"

namespace McpLink
{
	namespace
	{
		constexpr float CaptureTimeoutSeconds = 10.0f;
		constexpr float HighResTimeoutSeconds = 60.0f;

		struct FCaptureRequest
		{
			TSharedPtr<FMcpResponder> Responder;
			// The game viewport's delegate (PIE, and any capture with UI).
			FDelegateHandle DelegateHandle;
			// The editor viewport's delegate (high-resolution shots).
			FDelegateHandle EditorDelegateHandle;
			FString OutputPath;
			FString Format;   // "jpg" or "png"
			FString Note;
			int32 Quality = 85;
			int32 MaxDimension = 0;
			bool bDone = false;
		};

		void RemoveDelegates(const TSharedRef<FCaptureRequest>& Request)
		{
			if (Request->DelegateHandle.IsValid() && UGameViewportClient::OnScreenshotCaptured().IsBound())
			{
				UGameViewportClient::OnScreenshotCaptured().Remove(Request->DelegateHandle);
			}
			if (Request->EditorDelegateHandle.IsValid() && FScreenshotRequest::OnScreenshotCaptured().IsBound())
			{
				FScreenshotRequest::OnScreenshotCaptured().Remove(Request->EditorDelegateHandle);
			}
			Request->DelegateHandle.Reset();
			Request->EditorDelegateHandle.Reset();
		}

		/// Request is taken **by value**, and that is load-bearing. This runs
		/// inside OnScreenshotCaptured's broadcast, and unbinding our own
		/// delegate below destroys the bound lambda immediately —
		/// FDelegateBase::Unbind runs the instance's destructor there and then,
		/// only the invocation-list compaction is deferred while broadcasting.
		/// The lambda owns a TSharedRef<FCaptureRequest>, so a reference
		/// parameter would alias storage that Remove() has just freed and every
		/// line after it would be a use-after-free.
		void FinishCapture(
			TSharedRef<FCaptureRequest> Request, int32 Width, int32 Height, const TArray<FColor>& Pixels)
		{
			if (Request->bDone)
			{
				return;
			}
			Request->bDone = true;
			RemoveDelegates(Request);

			TArray<FColor> Working = Pixels;
			// ReadPixels can leave alpha at 0, which renders as fully transparent.
			for (FColor& Pixel : Working)
			{
				Pixel.A = 255;
			}

			FImage Image;
			Image.SizeX = Width;
			Image.SizeY = Height;
			Image.NumSlices = 1;
			Image.Format = ERawImageFormat::BGRA8;
			Image.GammaSpace = EGammaSpace::sRGB;
			Image.RawData.SetNumUninitialized(Working.Num() * sizeof(FColor));
			FMemory::Memcpy(Image.RawData.GetData(), Working.GetData(), Working.Num() * sizeof(FColor));

			// Downscale before encoding so payloads stay small by default.
			if (Request->MaxDimension > 0 && FMath::Max(Width, Height) > Request->MaxDimension)
			{
				const double Scale = static_cast<double>(Request->MaxDimension) / FMath::Max(Width, Height);
				const int32 NewWidth = FMath::Max(1, FMath::RoundToInt(Width * Scale));
				const int32 NewHeight = FMath::Max(1, FMath::RoundToInt(Height * Scale));
				FImage Resized;
				FImageCore::ResizeTo(Image, Resized, NewWidth, NewHeight, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
				Image = MoveTemp(Resized);
			}

			TArray64<uint8> Encoded;
			if (!FImageUtils::CompressImage(Encoded, *Request->Format, Image, Request->Quality))
			{
				Request->Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("encode_failed"),
					TEXT("failed to encode the captured image"));
				return;
			}

			const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("width"), Image.SizeX);
			Data->SetNumberField(TEXT("height"), Image.SizeY);
			Data->SetNumberField(TEXT("captured_width"), Width);
			Data->SetNumberField(TEXT("captured_height"), Height);
			Data->SetStringField(TEXT("format"), Request->Format);
			if (!Request->Note.IsEmpty())
			{
				Data->SetStringField(TEXT("note"), Request->Note);
			}
			if (!Request->OutputPath.IsEmpty())
			{
				if (!FFileHelper::SaveArrayToFile(Encoded, *Request->OutputPath))
				{
					Request->Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("write_failed"),
						FString::Printf(TEXT("could not write '%s'"), *Request->OutputPath));
					return;
				}
				Data->SetStringField(TEXT("file"), Request->OutputPath);
			}
			else
			{
				Data->SetStringField(TEXT("image_base64"),
					FBase64::Encode(Encoded.GetData(), Encoded.Num()));
			}
			Request->Responder->Ok(Data);
		}

		/// Fail the request if no capture has arrived after `Seconds`.
		void StartTimeout(const TSharedRef<FCaptureRequest>& Request, float Seconds, const FString& Message)
		{
			float Elapsed = 0.0f;
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda(
					[Request, Elapsed, Seconds, Message](float DeltaTime) mutable -> bool
					{
						if (Request->bDone)
						{
							return false;
						}
						Elapsed += DeltaTime;
						if (Elapsed < Seconds)
						{
							return true;
						}
						Request->bDone = true;
						RemoveDelegates(Request);
						Request->Responder->Error(
							EHttpServerResponseCodes::ServerError, TEXT("capture_timeout"), Message);
						return false;
					}),
				0.0f);
		}

		/// The PIE viewport while playing, else the active editor viewport.
		FViewport* PickViewport()
		{
			FViewport* Viewport = nullptr;
			if (GEditor != nullptr && GEditor->IsPlayingSessionInEditor())
			{
				Viewport = GEditor->GetPIEViewport();
			}
			if (Viewport == nullptr && GEditor != nullptr)
			{
				Viewport = GEditor->GetActiveViewport();
			}
			return Viewport;
		}

		/// Render the scene from a camera placed by the request into a
		/// transient render target and hand its pixels to FinishCapture.
		void CaptureFromCamera(const TSharedRef<FJsonObject>& Body, const TSharedRef<FCaptureRequest>& Request)
		{
			if (!FApp::CanEverRender())
			{
				Request->Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_renderer"),
					TEXT("a headless (-nullrhi) editor has no RHI to render a scene capture with"));
				return;
			}
			UWorld* World = ResolveWorld(Body);
			if (World == nullptr)
			{
				Request->Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_world"),
					TEXT("no world — open a level or start PIE"));
				return;
			}
			FVector Location = FVector::ZeroVector;
			if (!GetVector(Body, TEXT("location"), Location))
			{
				Request->Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("missing_field"),
					TEXT("'location' [X, Y, Z] is required for a camera capture"));
				return;
			}
			FRotator Rotation = FRotator::ZeroRotator;
			GetRotator(Body, TEXT("rotation"), Rotation);
			const int32 Width = FMath::Clamp(IntOr(Body, TEXT("width"), 1280), 16, 8192);
			const int32 Height = FMath::Clamp(IntOr(Body, TEXT("height"), 720), 16, 8192);
			const float Fov = static_cast<float>(FMath::Clamp(DoubleOr(Body, TEXT("fov"), 90.0), 5.0, 170.0));

			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transient;
			SpawnParams.bNoFail = true;
			ASceneCapture2D* Actor = World->SpawnActor<ASceneCapture2D>(Location, Rotation, SpawnParams);
			if (Actor == nullptr)
			{
				Request->Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("spawn_failed"),
					TEXT("could not spawn a scene capture actor"));
				return;
			}
			USceneCaptureComponent2D* Capture = Actor->GetCaptureComponent2D();
			UTextureRenderTarget2D* Target =
				NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
			Target->RenderTargetFormat = RTF_RGBA8;
			Target->ClearColor = FLinearColor::Black;
			Target->InitAutoFormat(Width, Height);
			Target->UpdateResourceImmediate(true);

			Capture->TextureTarget = Target;
			Capture->FOVAngle = Fov;
			// The tonemapped image, as the viewport would show it.
			Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
			Capture->bCaptureEveryFrame = false;
			Capture->bCaptureOnMovement = false;
			Capture->bAlwaysPersistRenderingState = true;
			Capture->CaptureScene();
			FlushRenderingCommands();

			TArray<FColor> Pixels;
			FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
			const bool bRead = Resource != nullptr && Resource->ReadPixels(Pixels);
			World->DestroyActor(Actor);
			if (!bRead || Pixels.Num() != Width * Height)
			{
				Request->Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("read_pixels_failed"),
					TEXT("the scene capture produced no pixels"));
				return;
			}
			FinishCapture(Request, Width, Height, Pixels);
		}
	}

	void RegisterCaptureRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/editor/capture_viewport"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Mode = TEXT("viewport");
				Body->TryGetStringField(TEXT("mode"), Mode);
				Mode = Mode.ToLower();
				bool bIncludeUi = false;
				Body->TryGetBoolField(TEXT("include_ui"), bIncludeUi);

				const TSharedRef<FCaptureRequest> Request = MakeShared<FCaptureRequest>();
				Request->Responder = Responder;
				Body->TryGetStringField(TEXT("output_path"), Request->OutputPath);
				FString Format;
				Body->TryGetStringField(TEXT("format"), Format);
				Request->Format = Format.Equals(TEXT("png"), ESearchCase::IgnoreCase)
					? TEXT("png") : TEXT("jpg");
				double Quality = 85.0;
				Body->TryGetNumberField(TEXT("quality"), Quality);
				Request->Quality = FMath::Clamp(static_cast<int32>(Quality), 1, 100);
				// A high-resolution shot is asked for at its size, so it is
				// only downscaled when the caller says so.
				double MaxDimension = Mode == TEXT("high_res") ? 0.0 : 1280.0;
				Body->TryGetNumberField(TEXT("max_dimension"), MaxDimension);
				Request->MaxDimension = FMath::Max(0, static_cast<int32>(MaxDimension));

				if (Mode == TEXT("camera"))
				{
					CaptureFromCamera(Body, Request);
					return;
				}

				if (Mode == TEXT("high_res"))
				{
					if (!FApp::CanEverRender())
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_viewport"),
							TEXT("no renderable viewport — the editor may be running headless (-nullrhi)"));
						return;
					}
					FViewport* Viewport = PickViewport();
					if (Viewport == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_viewport"),
							TEXT("no viewport to render — open a level viewport or start PIE"));
						return;
					}
					const FIntPoint Size = Viewport->GetSizeXY();
					const double Multiplier = FMath::Clamp(DoubleOr(Body, TEXT("multiplier"), 2.0), 1.0, 8.0);
					int32 Width = IntOr(Body, TEXT("width"), 0);
					int32 Height = IntOr(Body, TEXT("height"), 0);
					if (Width <= 0 || Height <= 0)
					{
						Width = FMath::RoundToInt(Size.X * Multiplier);
						Height = FMath::RoundToInt(Size.Y * Multiplier);
					}
					// The same globals the High Resolution Screenshot tool sets:
					// the next draw of this viewport happens at this size.
					FHighResScreenshotConfig& Config = GetHighResScreenshotConfig();
					if (!Config.SetResolution(Width, Height, 1.0f))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("too_large"),
							FString::Printf(
								TEXT("%d x %d exceeds the largest texture this GPU can render — ask for less"),
								Width, Height));
						return;
					}
					Request->Note = TEXT("the engine also wrote its own copy under Saved/Screenshots");
					// Editor viewports broadcast the bitmap through
					// FScreenshotRequest, the game viewport through
					// UGameViewportClient; bind both, FinishCapture runs once.
					Request->EditorDelegateHandle = FScreenshotRequest::OnScreenshotCaptured().AddLambda(
						[Request](int32 W, int32 H, const TArray<FColor>& Pixels)
						{
							FinishCapture(Request, W, H, Pixels);
						});
					Request->DelegateHandle = UGameViewportClient::OnScreenshotCaptured().AddLambda(
						[Request](int32 W, int32 H, const TArray<FColor>& Pixels)
						{
							FinishCapture(Request, W, H, Pixels);
						});
					FScreenshotRequest::RequestScreenshot(/*bShowUI*/ false, /*bRestrictToGameViewport*/ false);
					Viewport->TakeHighResScreenShot();
					// An editor viewport only redraws when told to.
					Viewport->Invalidate();
					StartTimeout(Request, HighResTimeoutSeconds,
						TEXT("no high-resolution screenshot arrived within 60s — is the viewport drawing?"));
					return;
				}

				if (Mode != TEXT("viewport"))
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_mode"),
						FString::Printf(TEXT("unknown mode '%s' — use viewport, high_res or camera"), *Mode));
					return;
				}

				// With UI, only the game viewport composites Slate/UMG, so PIE
				// must be running; without it we can read any viewport directly.
				if (bIncludeUi)
				{
					if (GEngine == nullptr || GEngine->GameViewport == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("pie_not_running"),
							TEXT("include_ui captures Slate/UMG overlays, which requires a running PIE session"));
						return;
					}
					Request->DelegateHandle = UGameViewportClient::OnScreenshotCaptured().AddLambda(
						[Request](int32 Width, int32 Height, const TArray<FColor>& Pixels)
						{
							FinishCapture(Request, Width, Height, Pixels);
						});
					FScreenshotRequest::RequestScreenshot(/*bShowUI*/ true, /*bRestrictToGameViewport*/ true);
					StartTimeout(Request, CaptureTimeoutSeconds,
						TEXT("no screenshot arrived within 10s — is the viewport rendering?"));
					return;
				}

				FViewport* Viewport = PickViewport();
				if (Viewport == nullptr)
				{
					Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_viewport"),
						TEXT("no renderable viewport — the editor may be running headless (-nullrhi)"));
					return;
				}

				TArray<FColor> Pixels;
				if (!Viewport->ReadPixels(Pixels))
				{
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("read_pixels_failed"),
						TEXT("could not read viewport pixels"));
					return;
				}
				const FIntPoint Size = Viewport->GetSizeXY();
				FinishCapture(Request, Size.X, Size.Y, Pixels);
			});
	}
}
