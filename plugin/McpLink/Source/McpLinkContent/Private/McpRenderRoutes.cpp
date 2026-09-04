// Render targets, textures and thumbnails — the pixels an agent can produce
// and read back without a viewport.
//
// capture_viewport photographs what the editor is showing; this is the other
// direction: draw a material into a render target, read the result, bake it to
// a Texture2D asset, or ask the editor for an asset's thumbnail. All of it
// needs a real RHI, so a `-nullrhi` editor is refused rather than left
// returning black.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInterface.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace Rendering
	{
		/// Everything here draws, so a non-rendering editor gets told rather
		/// than handed a black image.
		bool RequireRendering(const TSharedRef<FMcpResponder>& Responder)
		{
			if (FApp::CanEverRender())
			{
				return true;
			}
			Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_rendering"),
				TEXT("this editor runs without rendering (-nullrhi), so there is no RHI to draw with — ")
				TEXT("run a windowed editor for render targets, textures and thumbnails"));
			return false;
		}

		UTextureRenderTarget2D* TargetOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("render_target"), Path, Responder,
					TEXT("a Render Target asset path, e.g. /Game/RT/RT_Mask")))
			{
				return nullptr;
			}
			UTextureRenderTarget2D* Target = Cast<UTextureRenderTarget2D>(ResolveAsset(Path));
			if (Target == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("render_target_not_found"),
					FString::Printf(
						TEXT("no Render Target 2D at '%s' — render_ops create_render_target makes one"),
						*Path));
			}
			return Target;
		}

		FLinearColor ReadColor(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, FLinearColor Default)
		{
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Body->TryGetArrayField(Field, Values) || Values == nullptr || Values->Num() < 3)
			{
				return Default;
			}
			const auto At = [Values](int32 Index, double Fallback)
			{
				double Number = Fallback;
				if (Values->IsValidIndex(Index))
				{
					(*Values)[Index]->TryGetNumber(Number);
				}
				return static_cast<float>(Number);
			};
			return FLinearColor(At(0, 0.0), At(1, 0.0), At(2, 0.0), At(3, 1.0));
		}
	}

	using namespace Rendering;

	void RegisterRenderRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/content/render"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("list_formats"))
				{
					const UEnum* FormatEnum = StaticEnum<ETextureRenderTargetFormat>();
					TArray<TSharedPtr<FJsonValue>> Formats;
					for (int32 Index = 0; FormatEnum != nullptr && Index < FormatEnum->NumEnums() - 1;
						++Index)
					{
						Formats.Add(MakeShared<FJsonValueString>(FormatEnum->GetNameStringByIndex(Index)));
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetArrayField(TEXT("formats"), Formats);
					Responder->Ok(Data);
					return;
				}

				if (!RequireRendering(Responder))
				{
					return;
				}

				if (Operation == TEXT("create_render_target"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/RT/RT_Mask")))
					{
						return;
					}
					if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr, *Path) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}
					FString FormatName = TEXT("RTF_RGBA16f");
					Body->TryGetStringField(TEXT("format"), FormatName);
					const UEnum* FormatEnum = StaticEnum<ETextureRenderTargetFormat>();
					const int64 Format =
						FormatEnum != nullptr ? FormatEnum->GetValueByNameString(FormatName) : INDEX_NONE;
					if (Format == INDEX_NONE)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_format"),
							TEXT("'format' must be an ETextureRenderTargetFormat name — see list_formats"));
						return;
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateRenderTarget", "McpLink Create Render Target"));
					UPackage* Package = CreatePackage(*Path);
					UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(
						Package, FName(*FPackageName::GetShortName(Path)),
						RF_Public | RF_Standalone | RF_Transactional);
					Target->RenderTargetFormat = static_cast<ETextureRenderTargetFormat>(Format);
					Target->ClearColor = ReadColor(Body, TEXT("clear_color"), FLinearColor::Black);
					Target->bAutoGenerateMips = BoolOr(Body, TEXT("auto_generate_mips"), false);
					Target->InitAutoFormat(
						FMath::Clamp(IntOr(Body, TEXT("width"), 256), 1, 8192),
						FMath::Clamp(IntOr(Body, TEXT("height"), 256), 1, 8192));
					Target->UpdateResourceImmediate(true);
					FAssetRegistryModule::AssetCreated(Target);
					Package->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("render_target"), Target->GetPathName());
					Data->SetNumberField(TEXT("width"), Target->SizeX);
					Data->SetNumberField(TEXT("height"), Target->SizeY);
					Data->SetStringField(TEXT("format"), FormatName);
					Data->SetStringField(TEXT("message"),
						TEXT("created in memory — draw into it, then save with asset_ops save"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("generate_thumbnail"))
				{
					FString AssetPath;
					if (!RequireString(Body, TEXT("asset"), AssetPath, Responder,
							TEXT("the asset to render a thumbnail for")))
					{
						return;
					}
					UObject* Asset = ResolveAsset(AssetPath);
					if (Asset == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("asset_not_found"),
							FString::Printf(TEXT("no asset at '%s'"), *AssetPath));
						return;
					}
					const int32 Size = FMath::Clamp(IntOr(Body, TEXT("size"), 256), 16, 2048);
					FObjectThumbnail Thumbnail;
					ThumbnailTools::RenderThumbnail(Asset, Size, Size,
						ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush, nullptr, &Thumbnail);
					if (Thumbnail.GetImageWidth() == 0)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("no_thumbnail"),
							FString::Printf(
								TEXT("nothing rendered for '%s' — not every asset class has a thumbnail ")
								TEXT("renderer"),
								*AssetPath));
						return;
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("asset"), Asset->GetPathName());
					Data->SetNumberField(TEXT("width"), Thumbnail.GetImageWidth());
					Data->SetNumberField(TEXT("height"), Thumbnail.GetImageHeight());

					FString FilePath;
					if (Body->TryGetStringField(TEXT("file"), FilePath) && !FilePath.IsEmpty())
					{
						// The thumbnail cache's own compressed form is JPEG, so
						// re-encode as PNG rather than write a .png that is not
						// one. The raw data is BGRA, one FColor per pixel.
						const TArray<uint8>& Raw = Thumbnail.GetUncompressedImageData();
						const int32 Width = Thumbnail.GetImageWidth();
						const int32 Height = Thumbnail.GetImageHeight();
						if (Raw.Num() < Width * Height * static_cast<int32>(sizeof(FColor)))
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_pixels"),
								TEXT("the thumbnail renderer produced no image data"));
							return;
						}
						const TArrayView64<const FColor> Pixels(
							reinterpret_cast<const FColor*>(Raw.GetData()),
							static_cast<int64>(Width) * Height);
						TArray64<uint8> Png;
						FImageUtils::PNGCompressImageArray(Width, Height, Pixels, Png);
						if (!FFileHelper::SaveArrayToFile(Png, *FilePath))
						{
							Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("write_failed"),
								FString::Printf(TEXT("could not write '%s'"), *FilePath));
							return;
						}
						Data->SetStringField(TEXT("file"), FPaths::ConvertRelativePathToFull(FilePath));
						Data->SetNumberField(TEXT("bytes"), Png.Num());
						Data->SetStringField(TEXT("format"), TEXT("png"));
					}
					else
					{
						// Otherwise cache it into the asset's package, which is
						// what the content browser shows.
						ThumbnailTools::CacheThumbnail(Asset->GetFullName(), &Thumbnail,
							Asset->GetOutermost());
						Asset->MarkPackageDirty();
						Data->SetStringField(TEXT("message"),
							TEXT("cached into the asset's package — save it to keep the thumbnail"));
					}
					Responder->Ok(Data);
					return;
				}

				// ---- everything below targets an existing render target -----
				UTextureRenderTarget2D* Target = TargetOrError(Body, Responder);
				if (Target == nullptr)
				{
					return;
				}
				UWorld* World = GEditor != nullptr ? GEditor->GetEditorWorldContext().World() : nullptr;

				if (Operation == TEXT("info"))
				{
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("render_target"), Target->GetPathName());
					Data->SetNumberField(TEXT("width"), Target->SizeX);
					Data->SetNumberField(TEXT("height"), Target->SizeY);
					if (const UEnum* FormatEnum = StaticEnum<ETextureRenderTargetFormat>())
					{
						Data->SetStringField(TEXT("format"),
							FormatEnum->GetNameStringByValue(
								static_cast<int64>(Target->RenderTargetFormat)));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("clear"))
				{
					UKismetRenderingLibrary::ClearRenderTarget2D(
						World, Target, ReadColor(Body, TEXT("color"), Target->ClearColor));
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("render_target"), Target->GetPathName());
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("draw_material"))
				{
					FString MaterialPath;
					if (!RequireString(Body, TEXT("material"), MaterialPath, Responder,
							TEXT("the Material or Material Instance to draw")))
					{
						return;
					}
					UMaterialInterface* Material =
						Cast<UMaterialInterface>(ResolveAsset(MaterialPath));
					if (Material == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("material_not_found"),
							FString::Printf(TEXT("no material at '%s'"), *MaterialPath));
						return;
					}
					UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, Target, Material);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("render_target"), Target->GetPathName());
					Data->SetStringField(TEXT("material"), Material->GetPathName());
					Data->SetStringField(TEXT("message"),
						TEXT("drawn — read_pixel checks the result, to_texture bakes it to an asset"));
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("read_pixel"))
				{
					const int32 X = FMath::Clamp(IntOr(Body, TEXT("x"), 0), 0, Target->SizeX - 1);
					const int32 Y = FMath::Clamp(IntOr(Body, TEXT("y"), 0), 0, Target->SizeY - 1);
					const FLinearColor Color =
						UKismetRenderingLibrary::ReadRenderTargetRawPixel(World, Target, X, Y, false);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetNumberField(TEXT("x"), X);
					Data->SetNumberField(TEXT("y"), Y);
					Data->SetArrayField(TEXT("color"),
						{MakeShared<FJsonValueNumber>(Color.R), MakeShared<FJsonValueNumber>(Color.G),
							MakeShared<FJsonValueNumber>(Color.B),
							MakeShared<FJsonValueNumber>(Color.A)});
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("export"))
				{
					FString FilePath;
					if (!RequireString(Body, TEXT("file"), FilePath, Responder,
							TEXT("where to write the image, e.g. C:/out/mask.png")))
					{
						return;
					}
					const FString Directory = FPaths::GetPath(FilePath);
					const FString Filename = FPaths::GetCleanFilename(FilePath);
					UKismetRenderingLibrary::ExportRenderTarget(World, Target, Directory, Filename);
					const bool bWritten = FPaths::FileExists(FilePath);
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("file"), FPaths::ConvertRelativePathToFull(FilePath));
					Data->SetBoolField(TEXT("written"), bWritten);
					if (!bWritten)
					{
						Data->SetStringField(TEXT("message"),
							TEXT("the export reported no error but no file appeared — check the ")
							TEXT("directory exists and the extension is .png, .exr or .hdr"));
					}
					Responder->Ok(Data);
					return;
				}

				if (Operation == TEXT("to_texture"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("where to create the Texture2D, e.g. /Game/Textures/T_Mask")))
					{
						return;
					}
					UTexture2D* Existing = Cast<UTexture2D>(ResolveAsset(Path));
					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "RenderTargetToTexture", "McpLink Bake Render Target"));
					UTexture2D* Texture = Existing;
					if (Texture != nullptr)
					{
						// Overwriting keeps every reference to the texture.
						UKismetRenderingLibrary::ConvertRenderTargetToTexture2DEditorOnly(
							World, Target, Texture);
					}
					else
					{
						if (FPackageName::DoesPackageExist(Path)
							|| FindPackage(nullptr, *Path) != nullptr)
						{
							Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
								FString::Printf(
									TEXT("'%s' exists but is not a Texture2D"), *Path));
							return;
						}
						// The library's own create names the asset inside the
						// render target's package, so the texture is built here
						// and filled from the target instead.
						UPackage* Package = CreatePackage(*Path);
						Texture = NewObject<UTexture2D>(Package,
							FName(*FPackageName::GetShortName(Path)),
							RF_Public | RF_Standalone | RF_Transactional);
						UKismetRenderingLibrary::ConvertRenderTargetToTexture2DEditorOnly(
							World, Target, Texture);
						FAssetRegistryModule::AssetCreated(Texture);
						Package->MarkPackageDirty();
					}

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("texture"), Texture->GetPathName());
					Data->SetNumberField(TEXT("width"), Texture->Source.GetSizeX());
					Data->SetNumberField(TEXT("height"), Texture->Source.GetSizeY());
					Data->SetBoolField(TEXT("overwrote"), Existing != nullptr);
					Data->SetStringField(TEXT("message"),
						TEXT("baked in memory — save it with asset_ops save"));
					Responder->Ok(Data);
					return;
				}

				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
					FString::Printf(
						TEXT("unknown operation '%s' — use list_formats, create_render_target, info, ")
						TEXT("clear, draw_material, read_pixel, export, to_texture, or ")
						TEXT("generate_thumbnail"),
						*Operation));
			});
	}
}
