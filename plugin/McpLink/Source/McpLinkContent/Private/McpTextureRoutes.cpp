// Textures from raw pixels, with no source file to import.
//
// asset_ops import covers PNGs and the rest on disk; this is the other half —
// authoring a texture's bytes directly, and writing into an existing one. That
// is what masks, gradients, palettes, lookup tables and small procedural maps
// need, and none of them have a file anywhere.
//
// Pixels cross the wire as base64 BGRA8, four bytes per pixel, rows top to
// bottom — the layout FTextureSource uses for TSF_BGRA8, so a write is a
// straight memcpy per row. Every operation works on the *source* data (the
// editor-only, uncompressed copy), because that is what the asset stores and
// what survives a recompress; the platform data is rebuilt from it by
// PostEditChange.

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture2D.h"
#include "McpAssetUtils.h"
#include "McpJson.h"
#include "McpLinkCoreModule.h"
#include "McpResolve.h"
#include "McpResponder.h"
#include "Misc/Base64.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace McpLink
{
	namespace Textures
	{
		/// The largest region a single request will move, in pixels. 256x256 of
		/// BGRA8 is 256 KB, which is already 350 KB of base64.
		constexpr int64 MaxRegionPixels = 256 * 256;

		UTexture2D* TextureOrError(
			const TSharedRef<FJsonObject>& Body, const TSharedRef<FMcpResponder>& Responder)
		{
			FString Path;
			if (!RequireString(Body, TEXT("texture"), Path, Responder,
					TEXT("a Texture2D asset path, e.g. /Game/Textures/T_Mask")))
			{
				return nullptr;
			}
			UTexture2D* Texture = Cast<UTexture2D>(ResolveAsset(Path));
			if (Texture == nullptr)
			{
				Responder->Error(EHttpServerResponseCodes::NotFound, TEXT("texture_not_found"),
					FString::Printf(TEXT("no Texture2D at '%s' — texture_ops create makes one"),
						*Path));
			}
			return Texture;
		}

		/// Writes and reads only make sense on the uncompressed 8-bit layout the
		/// wire format matches; anything else is reported rather than guessed at.
		bool RequireBgra8(UTexture2D* Texture, const TSharedRef<FMcpResponder>& Responder)
		{
			if (Texture->Source.GetFormat() != TSF_BGRA8)
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("wrong_source_format"),
					FString::Printf(
						TEXT("'%s' stores its source as %s; pixel reads and writes need TSF_BGRA8. ")
						TEXT("texture_ops create makes BGRA8 textures"),
						*Texture->GetName(),
						*StaticEnum<ETextureSourceFormat>()->GetNameStringByValue(
							static_cast<int64>(Texture->Source.GetFormat()))));
				return false;
			}
			return true;
		}

		/// A rectangle from x/y/width/height, defaulting to the whole texture and
		/// clamped to it. False means the request named an empty or out-of-bounds
		/// region and the responder has already said so.
		bool ReadRegion(const TSharedRef<FJsonObject>& Body, const UTexture2D* Texture,
			const TSharedRef<FMcpResponder>& Responder,
			int32& OutX, int32& OutY, int32& OutWidth, int32& OutHeight)
		{
			const int32 SizeX = Texture->Source.GetSizeX();
			const int32 SizeY = Texture->Source.GetSizeY();
			OutX = IntOr(Body, TEXT("x"), 0);
			OutY = IntOr(Body, TEXT("y"), 0);
			OutWidth = IntOr(Body, TEXT("width"), SizeX - OutX);
			OutHeight = IntOr(Body, TEXT("height"), SizeY - OutY);

			if (OutX < 0 || OutY < 0 || OutWidth <= 0 || OutHeight <= 0
				|| OutX + OutWidth > SizeX || OutY + OutHeight > SizeY)
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_region"),
					FString::Printf(
						TEXT("region %dx%d at (%d, %d) does not fit in a %dx%d texture"),
						OutWidth, OutHeight, OutX, OutY, SizeX, SizeY));
				return false;
			}
			if (static_cast<int64>(OutWidth) * OutHeight > MaxRegionPixels)
			{
				Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("region_too_large"),
					FString::Printf(
						TEXT("%dx%d is %lld pixels; one request moves at most %lld (256x256). ")
						TEXT("Work in tiles, or use render_ops to draw a whole texture with a ")
						TEXT("material"),
						OutWidth, OutHeight, static_cast<int64>(OutWidth) * OutHeight,
						MaxRegionPixels));
				return false;
			}
			return true;
		}

		/// A colour as [r, g, b, a], 0-255, defaulting to opaque black. Stored
		/// BGRA, which is the byte order on the wire and in the source data.
		void ReadColorBgra(const TSharedRef<FJsonObject>& Body, const TCHAR* Field, uint8 OutBgra[4])
		{
			OutBgra[0] = 0;
			OutBgra[1] = 0;
			OutBgra[2] = 0;
			OutBgra[3] = 255;
			const TArray<TSharedPtr<FJsonValue>>* Color = nullptr;
			if (!Body->TryGetArrayField(Field, Color))
			{
				return;
			}
			auto Channel = [Color](int32 Index, uint8 Default) -> uint8
			{
				return Color->IsValidIndex(Index)
					? static_cast<uint8>(FMath::Clamp((*Color)[Index]->AsNumber(), 0.0, 255.0))
					: Default;
			};
			OutBgra[2] = Channel(0, 0);
			OutBgra[1] = Channel(1, 0);
			OutBgra[0] = Channel(2, 0);
			OutBgra[3] = Channel(3, 255);
		}

		/// Rebuild what the source data feeds: mips, compression, the render
		/// resource. PreEditChange/PostEditChange is the pair the texture editor
		/// itself brackets an edit with.
		void ApplySourceChange(UTexture2D* Texture)
		{
			Texture->UpdateResource();
			Texture->PostEditChange();
			Texture->MarkPackageDirty();
		}
	}

	using namespace Textures;

	void RegisterTextureRoutes(FMcpLinkCoreModule& Core)
	{
		Core.RegisterRoute(TEXT("/api/textures/ops"),
			[](const TSharedRef<FJsonObject>& Body, TSharedRef<FMcpResponder> Responder)
			{
				FString Operation;
				Body->TryGetStringField(TEXT("operation"), Operation);

				if (Operation == TEXT("create"))
				{
					FString Path;
					if (!RequireString(Body, TEXT("path"), Path, Responder,
							TEXT("e.g. /Game/Textures/T_Mask")))
					{
						return;
					}
					const int32 Width = IntOr(Body, TEXT("width"), 0);
					const int32 Height = IntOr(Body, TEXT("height"), 0);
					if (Width <= 0 || Height <= 0 || Width > 8192 || Height > 8192)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_size"),
							TEXT("'width' and 'height' are required, between 1 and 8192"));
						return;
					}
					if (FPackageName::DoesPackageExist(Path) || FindPackage(nullptr, *Path) != nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::Conflict, TEXT("already_exists"),
							FString::Printf(TEXT("an asset already exists at '%s'"), *Path));
						return;
					}

					const int64 Expected = static_cast<int64>(Width) * Height * 4;
					TArray<uint8> Pixels;
					FString Encoded;
					if (Body->TryGetStringField(TEXT("pixels"), Encoded) && !Encoded.IsEmpty())
					{
						if (!FBase64::Decode(Encoded, Pixels))
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest,
								TEXT("bad_base64"), TEXT("'pixels' is not valid base64"));
							return;
						}
						if (Pixels.Num() != Expected)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest,
								TEXT("wrong_pixel_count"),
								FString::Printf(
									TEXT("'pixels' decoded to %d bytes; a %dx%d BGRA8 texture needs ")
									TEXT("%lld (4 bytes per pixel, rows top to bottom)"),
									Pixels.Num(), Width, Height, Expected));
							return;
						}
					}
					else
					{
						// No pixels given: a solid fill, which is the usual
						// starting point for a mask you then paint into.
						uint8 Bgra[4];
						ReadColorBgra(Body, TEXT("fill"), Bgra);
						Pixels.SetNumUninitialized(Expected);
						for (int64 Offset = 0; Offset < Expected; Offset += 4)
						{
							FMemory::Memcpy(Pixels.GetData() + Offset, Bgra, 4);
						}
					}

					const FScopedTransaction Transaction(
						NSLOCTEXT("McpLink", "CreateTexture", "McpLink Create Texture"));
					UPackage* Package = CreatePackage(*Path);
					UTexture2D* Texture = NewObject<UTexture2D>(Package,
						FName(*FPackageName::GetShortName(Path)),
						RF_Public | RF_Standalone | RF_Transactional);
					Texture->Source.Init(Width, Height, 1, 1, TSF_BGRA8, Pixels.GetData());
					// Masks, gradients and lookup tables are data, not colour;
					// sRGB on them is the classic silent wrongness.
					Texture->SRGB = BoolOr(Body, TEXT("srgb"), true);
					FString Compression;
					if (Body->TryGetStringField(TEXT("compression"), Compression))
					{
						const UEnum* Enum = StaticEnum<TextureCompressionSettings>();
						const int64 Value = Enum->GetValueByNameString(Compression);
						if (Value == INDEX_NONE)
						{
							Responder->Error(EHttpServerResponseCodes::BadRequest,
								TEXT("bad_compression"),
								FString::Printf(
									TEXT("'%s' is not a TextureCompressionSettings — e.g. ")
									TEXT("TC_Default, TC_Masks, TC_Grayscale, TC_HDR, ")
									TEXT("TC_VectorDisplacementmap"),
									*Compression));
							return;
						}
						Texture->CompressionSettings =
							static_cast<TextureCompressionSettings>(Value);
					}
					Texture->UpdateResource();
					Texture->PostEditChange();
					FAssetRegistryModule::AssetCreated(Texture);
					Package->MarkPackageDirty();

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("texture"), Texture->GetPathName());
					Data->SetNumberField(TEXT("width"), Width);
					Data->SetNumberField(TEXT("height"), Height);
					Data->SetBoolField(TEXT("srgb"), Texture->SRGB);
					Data->SetStringField(TEXT("message"),
						TEXT("created — write_pixels and fill edit it, then save"));
					Responder->Ok(Data);
					return;
				}

				UTexture2D* Texture = TextureOrError(Body, Responder);
				if (Texture == nullptr)
				{
					return;
				}

				if (Operation == TEXT("save"))
				{
					FString Filename, Error;
					if (!SaveAsset(Texture, Filename, Error))
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("save_failed"), Error);
						return;
					}
					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("texture"), Texture->GetPathName());
					Data->SetStringField(TEXT("file"), Filename);
					Responder->Ok(Data);
					return;
				}

				if (Operation != TEXT("write_pixels") && Operation != TEXT("fill")
					&& Operation != TEXT("read_pixels"))
				{
					Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("unknown_operation"),
						FString::Printf(
							TEXT("unknown operation '%s' — use create, read_pixels, write_pixels, ")
							TEXT("fill or save (texture_info reports size and format; asset_ops ")
							TEXT("import brings in a file from disk)"),
							*Operation));
					return;
				}

				if (!RequireBgra8(Texture, Responder))
				{
					return;
				}
				int32 X = 0, Y = 0, Width = 0, Height = 0;
				if (!ReadRegion(Body, Texture, Responder, X, Y, Width, Height))
				{
					return;
				}
				const int32 SizeX = Texture->Source.GetSizeX();
				const int64 RowBytes = static_cast<int64>(Width) * 4;

				if (Operation == TEXT("read_pixels"))
				{
					const uint8* Source = Texture->Source.LockMipReadOnly(0);
					if (Source == nullptr)
					{
						Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_source_data"),
							TEXT("this texture has no editor source data to read — that happens ")
							TEXT("with a cooked or source-stripped asset"));
						return;
					}
					TArray<uint8> Out;
					Out.SetNumUninitialized(RowBytes * Height);
					for (int32 Row = 0; Row < Height; ++Row)
					{
						FMemory::Memcpy(Out.GetData() + Row * RowBytes,
							Source + ((static_cast<int64>(Y + Row) * SizeX) + X) * 4, RowBytes);
					}
					Texture->Source.UnlockMip(0);

					const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
					Data->SetStringField(TEXT("texture"), Texture->GetPathName());
					Data->SetNumberField(TEXT("x"), X);
					Data->SetNumberField(TEXT("y"), Y);
					Data->SetNumberField(TEXT("width"), Width);
					Data->SetNumberField(TEXT("height"), Height);
					Data->SetStringField(TEXT("format"), TEXT("BGRA8"));
					Data->SetStringField(TEXT("pixels"), FBase64::Encode(Out));
					Responder->Ok(Data);
					return;
				}

				TArray<uint8> Incoming;
				uint8 Bgra[4] = {0, 0, 0, 255};
				if (Operation == TEXT("write_pixels"))
				{
					FString Encoded;
					if (!RequireString(Body, TEXT("pixels"), Encoded, Responder,
							TEXT("base64 BGRA8, four bytes per pixel, rows top to bottom")))
					{
						return;
					}
					if (!FBase64::Decode(Encoded, Incoming))
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest, TEXT("bad_base64"),
							TEXT("'pixels' is not valid base64"));
						return;
					}
					if (Incoming.Num() != RowBytes * Height)
					{
						Responder->Error(EHttpServerResponseCodes::BadRequest,
							TEXT("wrong_pixel_count"),
							FString::Printf(
								TEXT("'pixels' decoded to %d bytes; a %dx%d region needs %lld"),
								Incoming.Num(), Width, Height, RowBytes * Height));
						return;
					}
				}
				else
				{
					ReadColorBgra(Body, TEXT("color"), Bgra);
				}

				const FScopedTransaction Transaction(
					NSLOCTEXT("McpLink", "EditTexturePixels", "McpLink Edit Texture Pixels"));
				Texture->Modify();
				Texture->PreEditChange(nullptr);

				uint8* Destination = Texture->Source.LockMip(0);
				if (Destination == nullptr)
				{
					Texture->PostEditChange();
					Responder->Error(EHttpServerResponseCodes::ServerError, TEXT("no_source_data"),
						TEXT("this texture has no editor source data to write into"));
					return;
				}
				for (int32 Row = 0; Row < Height; ++Row)
				{
					uint8* RowStart =
						Destination + ((static_cast<int64>(Y + Row) * SizeX) + X) * 4;
					if (Operation == TEXT("write_pixels"))
					{
						FMemory::Memcpy(RowStart, Incoming.GetData() + Row * RowBytes, RowBytes);
					}
					else
					{
						for (int32 Column = 0; Column < Width; ++Column)
						{
							FMemory::Memcpy(RowStart + Column * 4, Bgra, 4);
						}
					}
				}
				Texture->Source.UnlockMip(0);
				ApplySourceChange(Texture);

				const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("texture"), Texture->GetPathName());
				Data->SetNumberField(TEXT("x"), X);
				Data->SetNumberField(TEXT("y"), Y);
				Data->SetNumberField(TEXT("width"), Width);
				Data->SetNumberField(TEXT("height"), Height);
				Data->SetStringField(TEXT("message"),
					TEXT("source pixels updated and mips rebuilt — save to keep it"));
				Responder->Ok(Data);
			});
	}
}
