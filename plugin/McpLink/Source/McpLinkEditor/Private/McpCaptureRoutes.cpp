// Viewport capture. The screenshot arrives a frame or more after the request,
// so the responder is stashed and completed from the capture delegate (with a
// ticker timeout as a backstop).

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpLinkEditorRoutes.h"
#include "McpResponder.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "UnrealClient.h"

namespace McpLink
{
	namespace
	{
		constexpr float CaptureTimeoutSeconds = 10.0f;

		struct FCaptureRequest
		{
			TSharedPtr<FMcpResponder> Responder;
			FDelegateHandle DelegateHandle;
			FString OutputPath;
			FString Format;   // "jpg" or "png"
			int32 Quality = 85;
			int32 MaxDimension = 0;
			bool bDone = false;
		};

		void FinishCapture(
			const TSharedRef<FCaptureRequest>& Request, int32 Width, int32 Height, const TArray<FColor>& Pixels)
		{
			if (Request->bDone)
			{
				return;
			}
			Request->bDone = true;
			if (UGameViewportClient::OnScreenshotCaptured().IsBound())
			{
				UGameViewportClient::OnScreenshotCaptured().Remove(Request->DelegateHandle);
			}

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
			Data->SetStringField(TEXT("format"), Request->Format);
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
	}

	void RegisterCaptureRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/editor/capture_viewport"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
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
				double MaxDimension = 1280.0;
				Body->TryGetNumberField(TEXT("max_dimension"), MaxDimension);
				Request->MaxDimension = FMath::Max(0, static_cast<int32>(MaxDimension));

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

					float Elapsed = 0.0f;
					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda(
							[Request, Elapsed](float DeltaTime) mutable -> bool
							{
								if (Request->bDone)
								{
									return false;
								}
								Elapsed += DeltaTime;
								if (Elapsed < CaptureTimeoutSeconds)
								{
									return true;
								}
								Request->bDone = true;
								UGameViewportClient::OnScreenshotCaptured().Remove(Request->DelegateHandle);
								Request->Responder->Error(
									EHttpServerResponseCodes::ServerError, TEXT("capture_timeout"),
									TEXT("no screenshot arrived within 10s — is the viewport rendering?"));
								return false;
							}),
						0.0f);
					return;
				}

				FViewport* Viewport = nullptr;
				if (GEditor != nullptr && GEditor->IsPlayingSessionInEditor())
				{
					Viewport = GEditor->GetPIEViewport();
				}
				if (Viewport == nullptr && GEditor != nullptr && GEditor->GetActiveViewport() != nullptr)
				{
					Viewport = GEditor->GetActiveViewport();
				}
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
