#include "rom/menu_background_decoder.h"

#include "rom/palette.h"
#include "rom/spinball_rom.h"
#include "rom/sprite.h"
#include "rom/sprite_tile.h"
#include "rom/ssc_compressor.h"
#include "rom/ssc_decompressor.h"
#include "rom/tile.h"
#include "rom/tileset.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <vector>

namespace spintool::rom
{
	namespace
	{
		constexpr Uint16 kTileIndexMask = 0x07FFU;
		constexpr Uint16 kHorizontalFlip = 0x0800U;
		constexpr Uint16 kVerticalFlip = 0x1000U;
		constexpr Uint16 kPaletteMask = 0x6000U;
		constexpr Uint16 kPriorityMask = 0x8000U;
		constexpr std::size_t kTileBytes = 32U;

		constexpr Uint32 kOriginalArtHeaderOffset = 0x000BDD2EU;
		constexpr Uint32 kOriginalBrushesOffset = 0x000BDFBCU;
		constexpr Uint32 kOriginalLayoutOffset = 0x000BE1BCU;
		constexpr std::size_t kOriginalBrushCount = 16U;
		constexpr std::size_t kBrushWidthTiles = 4U;
		constexpr std::size_t kBrushHeightTiles = 4U;
		constexpr std::size_t kBrushTileCount = 16U;
		constexpr std::size_t kBrushBytes = kBrushTileCount * 2U;
		constexpr std::size_t kLayoutWidthBrushes = 10U;
		constexpr std::size_t kVisibleLayoutHeightBrushes = 7U;
		constexpr std::size_t kVisibleLayoutCellCount =
			kLayoutWidthBrushes * kVisibleLayoutHeightBrushes;
		constexpr std::size_t kVisibleLayoutBytes = kVisibleLayoutCellCount * 2U;
		// sub_D8E12 draws 29 tile rows while preparing the 40-column menu plane.
		// The visible image is only 28 rows high, so the game reads the first tile
		// row of an eighth 4x4-brush row.  Keep an explicit padding row in the
		// relocated layout instead of letting that read fall into zero-filled or
		// unrelated ROM data.
		constexpr std::size_t kRuntimeLayoutHeightBrushes = 8U;
		constexpr std::size_t kRuntimeLayoutCellCount =
			kLayoutWidthBrushes * kRuntimeLayoutHeightBrushes;
		constexpr std::size_t kRuntimeLayoutBytes = kRuntimeLayoutCellCount * 2U;
		constexpr int kImageWidth = 320;
		constexpr int kImageHeight = 224;
		constexpr std::size_t kImageWidthTiles = 40U;
		constexpr std::size_t kImageHeightTiles = 28U;
		constexpr std::size_t kImageTileCount =
			kImageWidthTiles * kImageHeightTiles;
		constexpr std::size_t kMaximumPatternTiles = 0x0600U;
		// The menu graphics allocator's first free pattern depends on the screen
		// which was active before Options/Hi-Score/Credits was opened.  It can be
		// as low as $13E in menu mode or as high as $414 after a level.  Earlier
		// versions assumed a fixed start, so their guard still allowed the allocator
		// to run into the relocated background.  Version 5 computes the capacity at
		// runtime from the allocator's actual start and reserves the final 128 VRAM
		// patterns for the background.  Identical 8x8 cells are deduplicated; an
		// edited cell becomes independent whenever its pixels differ.
		constexpr Uint16 kMaximumOriginalAllocatorStart = 0x0414U;
		constexpr Uint16 kMinimumBackgroundPattern = 0x0580U;
		constexpr std::size_t kMaximumUniqueBackgroundTiles =
			kMaximumPatternTiles - kMinimumBackgroundPattern;

		// The title-background reservation ends at 0x312000. The shared menu
		// background receives its own non-overlapping fixed regions immediately
		// afterwards inside the same 4 MiB expanded ROM.
		constexpr std::size_t kExpandedROMSize = 0x00400000U;
		constexpr Uint32 kReservedMetadataOffset = 0x00312000U;
		constexpr std::size_t kReservedMetadataSize = 0x00001000U;
		constexpr Uint32 kAllocatorGuardCodeOffset = 0x00312100U;
		constexpr Uint32 kReservedArtHeaderOffset = 0x00313000U;
		constexpr std::size_t kReservedArtRegionSize = 0x00010000U;
		constexpr Uint32 kReservedBrushesOffset = 0x00323000U;
		constexpr std::size_t kReservedBrushesRegionSize = 0x00001000U;
		constexpr Uint32 kReservedLayoutOffset = 0x00324000U;
		constexpr std::size_t kReservedLayoutRegionSize = 0x00001000U;
		constexpr std::array<Uint8, 8> kReservedSignature
		{
			'S', 'P', 'M', 'E', 'N', 'U', 'B', '1'
		};
		constexpr Uint16 kReservedFormatVersion = 5U;
		constexpr Uint16 kMinimumSupportedReservedFormatVersion = 1U;

		// Only the three background loaders are redirected. The operand at
		// $DF826 is deliberately kept on the original $BDD2E header because the
		// game uses its word value ($2A) as the fixed HUD/font base. Redirecting
		// that operand caused the dynamic menu art to collide with the imported
		// background at runtime.
		constexpr Uint32 kHudBaseHeaderPointerOperand = 0x000DF826U;
		constexpr Uint32 kLoadSSCCompressedTilesAddress = 0x000D6B88U;
		constexpr std::array<Uint32, 3> kArtPointerOperands
		{
			0x000FEDB0U, // Options
			0x000FF19EU, // Credits
			0x000FF2E0U, // Hi-Score
		};
		// Absolute JSR target operands immediately following the three PEA art
		// operands above.  They are redirected to a tiny ROM trampoline which
		// limits the dynamic graphics allocator, then tail-jumps to the original
		// SSC loader without changing its stack layout.
		constexpr std::array<Uint32, 3> kArtLoaderCallTargetOperands
		{
			0x000FEDB6U, // Options
			0x000FF1A4U, // Credits
			0x000FF2E6U, // Hi-Score
		};
		constexpr Uint32 kLayoutPointerOperand = 0x000FF44AU;
		constexpr Uint32 kBrushesPointerOperand = 0x000FF454U;

		bool CanRead(const std::vector<Uint8>& data, const Uint32 offset,
			const std::size_t count)
		{
			return offset <= data.size() && count <= data.size() - offset;
		}

		Uint16 ReadBE16(const std::vector<Uint8>& data, const Uint32 offset)
		{
			return static_cast<Uint16>(
				(static_cast<Uint16>(data[offset]) << 8U) |
				static_cast<Uint16>(data[offset + 1U])
			);
		}

		Uint32 ReadBE32(const std::vector<Uint8>& data, const Uint32 offset)
		{
			return
				(static_cast<Uint32>(data[offset]) << 24U) |
				(static_cast<Uint32>(data[offset + 1U]) << 16U) |
				(static_cast<Uint32>(data[offset + 2U]) << 8U) |
				static_cast<Uint32>(data[offset + 3U]);
		}

		void WriteBE16(std::vector<Uint8>& data, const Uint32 offset,
			const Uint16 value)
		{
			data[offset] = static_cast<Uint8>((value >> 8U) & 0xFFU);
			data[offset + 1U] = static_cast<Uint8>(value & 0xFFU);
		}

		void WriteBE32(std::vector<Uint8>& data, const Uint32 offset,
			const Uint32 value)
		{
			data[offset] = static_cast<Uint8>((value >> 24U) & 0xFFU);
			data[offset + 1U] = static_cast<Uint8>((value >> 16U) & 0xFFU);
			data[offset + 2U] = static_cast<Uint8>((value >> 8U) & 0xFFU);
			data[offset + 3U] = static_cast<Uint8>(value & 0xFFU);
		}

		bool HasReservedSignature(const std::vector<Uint8>& data)
		{
			return CanRead(data, kReservedMetadataOffset, kReservedSignature.size()) &&
				std::equal(
					kReservedSignature.begin(), kReservedSignature.end(),
					data.begin() + static_cast<std::ptrdiff_t>(kReservedMetadataOffset)
				);
		}

		Uint16 ReservedFormatVersion(const std::vector<Uint8>& data)
		{
			if (!HasReservedSignature(data) ||
				!CanRead(data, kReservedMetadataOffset + 8U, 2U))
			{
				return 0U;
			}
			return ReadBE16(data, kReservedMetadataOffset + 8U);
		}

		Uint16 ReservedBackgroundPatternStart(const std::vector<Uint8>& data)
		{
			if (ReservedFormatVersion(data) < 5U ||
				!CanRead(data, kReservedMetadataOffset + 32U, 2U))
			{
				return 0U;
			}
			return ReadBE16(data, kReservedMetadataOffset + 32U);
		}

		Uint16 ReservedAllocatorCapacity(const std::vector<Uint8>& data)
		{
			if (ReservedFormatVersion(data) < 5U ||
				!CanRead(data, kReservedMetadataOffset + 34U, 2U))
			{
				return 0U;
			}
			return ReadBE16(data, kReservedMetadataOffset + 34U);
		}

		bool HasAllocatorGuardCode(const std::vector<Uint8>& data,
			const Uint16 background_pattern_start)
		{
			if (!CanRead(data, kAllocatorGuardCodeOffset, 26U)) return false;
			return ReadBE16(data, kAllocatorGuardCodeOffset) == 0x303CU &&
				ReadBE16(data, kAllocatorGuardCodeOffset + 2U) ==
					background_pattern_start &&
				ReadBE16(data, kAllocatorGuardCodeOffset + 4U) == 0x9079U &&
				ReadBE32(data, kAllocatorGuardCodeOffset + 6U) == 0x00FF4248U &&
				ReadBE16(data, kAllocatorGuardCodeOffset + 10U) == 0x6402U &&
				ReadBE16(data, kAllocatorGuardCodeOffset + 12U) == 0x4240U &&
				ReadBE16(data, kAllocatorGuardCodeOffset + 14U) == 0x33C0U &&
				ReadBE32(data, kAllocatorGuardCodeOffset + 16U) == 0x00FF424AU &&
				ReadBE16(data, kAllocatorGuardCodeOffset + 20U) == 0x4EF9U &&
				ReadBE32(data, kAllocatorGuardCodeOffset + 22U) ==
					kLoadSSCCompressedTilesAddress;
		}

		void WriteAllocatorGuardCode(std::vector<Uint8>& data,
			const Uint16 background_pattern_start)
		{
			// move.w  #background_pattern_start,d0
			// sub.w   ($00FF4248).l,d0 ; actual first-free pattern
			// bcc.s   capacity_ok
			// clr.w   d0               ; fail safely if the start is unexpected
			// capacity_ok:
			// move.w  d0,($00FF424A).l
			// jmp     LoadSSCCompressedTiles
			WriteBE16(data, kAllocatorGuardCodeOffset, 0x303CU);
			WriteBE16(data, kAllocatorGuardCodeOffset + 2U,
				background_pattern_start);
			WriteBE16(data, kAllocatorGuardCodeOffset + 4U, 0x9079U);
			WriteBE32(data, kAllocatorGuardCodeOffset + 6U, 0x00FF4248U);
			WriteBE16(data, kAllocatorGuardCodeOffset + 10U, 0x6402U);
			WriteBE16(data, kAllocatorGuardCodeOffset + 12U, 0x4240U);
			WriteBE16(data, kAllocatorGuardCodeOffset + 14U, 0x33C0U);
			WriteBE32(data, kAllocatorGuardCodeOffset + 16U, 0x00FF424AU);
			WriteBE16(data, kAllocatorGuardCodeOffset + 20U, 0x4EF9U);
			WriteBE32(data, kAllocatorGuardCodeOffset + 22U,
				kLoadSSCCompressedTilesAddress);
		}

		bool IsValidArtHeader(const std::vector<Uint8>& data, const Uint32 offset)
		{
			if (!CanRead(data, offset, 3U)) return false;
			const Uint16 count = ReadBE16(data, offset);
			return count != 0U && count <= kMaximumPatternTiles;
		}

		Uint32 ResolveArt(const std::vector<Uint8>& data)
		{
			if (CanRead(data, kArtPointerOperands[0U], 4U))
			{
				const Uint32 candidate = ReadBE32(data, kArtPointerOperands[0U]);
				if (IsValidArtHeader(data, candidate)) return candidate;
			}
			return kOriginalArtHeaderOffset;
		}

		Uint32 ResolveBrushes(const std::vector<Uint8>& data)
		{
			if (CanRead(data, kBrushesPointerOperand, 4U))
			{
				const Uint32 candidate = ReadBE32(data, kBrushesPointerOperand);
				if (CanRead(data, candidate, kBrushBytes)) return candidate;
			}
			return kOriginalBrushesOffset;
		}

		Uint32 ResolveLayout(const std::vector<Uint8>& data)
		{
			if (CanRead(data, kLayoutPointerOperand, 4U))
			{
				const Uint32 candidate = ReadBE32(data, kLayoutPointerOperand);
				if (CanRead(data, candidate, kVisibleLayoutBytes)) return candidate;
			}
			return kOriginalLayoutOffset;
		}

		bool IsReservedInstalled(const std::vector<Uint8>& data)
		{
			const Uint16 version = ReservedFormatVersion(data);
			if (version < kMinimumSupportedReservedFormatVersion ||
				version > kReservedFormatVersion ||
				!CanRead(data, kReservedMetadataOffset + 8U, 24U))
			{
				return false;
			}
			return ResolveArt(data) == kReservedArtHeaderOffset &&
				ResolveBrushes(data) == kReservedBrushesOffset &&
				ResolveLayout(data) == kReservedLayoutOffset;
		}

		bool IsCurrentReservedFormat(const std::vector<Uint8>& data)
		{
			if (!IsReservedInstalled(data) ||
				ReservedFormatVersion(data) != kReservedFormatVersion)
			{
				return false;
			}
			const Uint16 background_start = ReservedBackgroundPatternStart(data);
			const Uint16 allocator_capacity = ReservedAllocatorCapacity(data);
			if (background_start < kMinimumBackgroundPattern ||
				background_start > kMaximumPatternTiles ||
				allocator_capacity != static_cast<Uint16>(
					background_start - kMaximumOriginalAllocatorStart) ||
				!HasAllocatorGuardCode(data, background_start))
			{
				return false;
			}
			for (const Uint32 operand : kArtLoaderCallTargetOperands)
			{
				if (!CanRead(data, operand, 4U) ||
					ReadBE32(data, operand) != kAllocatorGuardCodeOffset)
				{
					return false;
				}
			}
			return true;
		}

		void ExpandROM(std::vector<Uint8>& data)
		{
			if (data.size() < kExpandedROMSize)
			{
				data.resize(kExpandedROMSize, 0xFFU);
			}
			if (CanRead(data, 0x000001A4U, 4U))
			{
				WriteBE32(data, 0x000001A4U,
					static_cast<Uint32>(data.size() - 1U));
			}
		}

		void UpdateMegaDriveChecksum(SpinballROM& rom)
		{
			if (rom.m_buffer.size() < 0x190U) return;
			Uint32 checksum = 0U;
			for (std::size_t offset = 0x200U; offset < rom.m_buffer.size(); offset += 2U)
			{
				Uint16 word = static_cast<Uint16>(rom.m_buffer[offset]) << 8U;
				if (offset + 1U < rom.m_buffer.size())
				{
					word = static_cast<Uint16>(word | rom.m_buffer[offset + 1U]);
				}
				checksum = (checksum + word) & 0xFFFFU;
			}
			rom.m_buffer[0x18EU] = static_cast<Uint8>((checksum >> 8U) & 0xFFU);
			rom.m_buffer[0x18FU] = static_cast<Uint8>(checksum & 0xFFU);
		}

		bool ValidatePatchOperands(const std::vector<Uint8>& data, std::string& error)
		{
			if (!CanRead(data, kHudBaseHeaderPointerOperand, 4U))
			{
				error = "The shared menu HUD-base pointer operand is outside the ROM";
				return false;
			}
			const Uint32 hud_base_header = ReadBE32(data, kHudBaseHeaderPointerOperand);
			if (hud_base_header != kOriginalArtHeaderOffset &&
				hud_base_header != kReservedArtHeaderOffset)
			{
				error = "Import refused: the shared menu HUD-base pointer was modified by another hack";
				return false;
			}

			for (const Uint32 operand : kArtPointerOperands)
			{
				if (!CanRead(data, operand, 4U))
				{
					error = "A shared menu-art pointer operand is outside the ROM";
					return false;
				}
				const Uint32 value = ReadBE32(data, operand);
				if (value != kOriginalArtHeaderOffset && value != kReservedArtHeaderOffset)
				{
					std::ostringstream message;
					message << "Import refused: menu-art pointer at 0x" << std::hex
						<< std::uppercase << operand << " contains unexpected address 0x"
						<< value;
					error = message.str();
					return false;
				}
			}
			for (const Uint32 operand : kArtLoaderCallTargetOperands)
			{
				if (!CanRead(data, operand, 4U))
				{
					error = "A shared menu SSC-loader call target is outside the ROM";
					return false;
				}
				const Uint32 value = ReadBE32(data, operand);
				if (value != kLoadSSCCompressedTilesAddress &&
					value != kAllocatorGuardCodeOffset)
				{
					std::ostringstream message;
					message << "Import refused: menu SSC-loader call at 0x"
						<< std::hex << std::uppercase << operand
						<< " contains unexpected target 0x" << value;
					error = message.str();
					return false;
				}
			}
			if (!CanRead(data, kLayoutPointerOperand, 4U) ||
				!CanRead(data, kBrushesPointerOperand, 4U))
			{
				error = "The shared menu layout/brush pointer operands are outside the ROM";
				return false;
			}
			const Uint32 layout = ReadBE32(data, kLayoutPointerOperand);
			const Uint32 brushes = ReadBE32(data, kBrushesPointerOperand);
			if ((layout != kOriginalLayoutOffset && layout != kReservedLayoutOffset) ||
				(brushes != kOriginalBrushesOffset && brushes != kReservedBrushesOffset))
			{
				error = "Import refused: the shared menu layout or brush pointer was modified by another hack";
				return false;
			}
			if (HasReservedSignature(data) && !IsReservedInstalled(data))
			{
				error = "Import refused: the reserved shared-menu signature exists but its pointers or version are inconsistent";
				return false;
			}
			if (ReservedFormatVersion(data) == kReservedFormatVersion &&
				!IsCurrentReservedFormat(data))
			{
				error = "Import refused: the shared-menu allocator guard is incomplete or damaged";
				return false;
			}
			return true;
		}

		std::size_t StoredLayoutCellCount(
			const std::vector<Uint8>& data,
			const Uint32 layout_offset)
		{
			return layout_offset == kReservedLayoutOffset &&
				IsCurrentReservedFormat(data)
				? kRuntimeLayoutCellCount
				: kVisibleLayoutCellCount;
		}

		std::size_t RequiredBrushCount(
			const std::vector<Uint8>& data,
			const Uint32 layout_offset,
			std::string& error
		)
		{
			const std::size_t layout_cell_count =
				StoredLayoutCellCount(data, layout_offset);
			const std::size_t layout_bytes = layout_cell_count * 2U;
			if (!CanRead(data, layout_offset, layout_bytes))
			{
				error = "The shared menu layout is outside the ROM";
				return 0U;
			}
			std::size_t maximum = 0U;
			for (std::size_t index = 0U; index < layout_cell_count; ++index)
			{
				const Uint16 attributes = ReadBE16(
					data, layout_offset + static_cast<Uint32>(index * 2U));
				maximum = std::max<std::size_t>(maximum,
					attributes & kTileIndexMask);
			}
			return maximum + 1U;
		}

		bool DecodeArt(
			const SpinballROM& rom,
			const Uint32 art_header_offset,
			std::vector<Uint8>& art,
			std::size_t& compressed_size,
			std::string& error
		)
		{
			if (!IsValidArtHeader(rom.m_buffer, art_header_offset))
			{
				error = "The shared menu SSC art header is invalid";
				return false;
			}
			const Uint16 tile_count = ReadBE16(rom.m_buffer, art_header_offset);
			const SSCDecompressionResult decompression = SSCDecompressor::DecompressData(
				rom.m_buffer,
				art_header_offset + 2U,
				static_cast<Uint32>(tile_count) * static_cast<Uint32>(kTileBytes)
			);
			if (decompression.error_msg.has_value())
			{
				error = "Could not decompress shared menu art: " +
					*decompression.error_msg;
				return false;
			}
			const std::size_t expected_size =
				static_cast<std::size_t>(tile_count) * kTileBytes;
			if (decompression.uncompressed_data.size() != expected_size)
			{
				std::ostringstream message;
				message << "Shared menu art decoded to "
					<< decompression.uncompressed_data.size() << " bytes; expected "
					<< expected_size;
				error = message.str();
				return false;
			}
			art = decompression.uncompressed_data;
			compressed_size = decompression.rom_data.real_size;
			return true;
		}

		bool ResolveDisplayedTile(
			const std::vector<Uint8>& data,
			const Uint32 brushes_offset,
			const Uint32 layout_offset,
			const std::size_t screen_tile_x,
			const std::size_t screen_tile_y,
			Uint16& output,
			std::string& error
		)
		{
			const std::size_t block_x = screen_tile_x / kBrushWidthTiles;
			const std::size_t block_y = screen_tile_y / kBrushHeightTiles;
			const std::size_t local_x = screen_tile_x % kBrushWidthTiles;
			const std::size_t local_y = screen_tile_y % kBrushHeightTiles;
			const std::size_t layout_index = block_y * kLayoutWidthBrushes + block_x;
			if (!CanRead(data, layout_offset + static_cast<Uint32>(layout_index * 2U), 2U))
			{
				error = "The shared menu layout cell is outside the ROM";
				return false;
			}
			const Uint16 layout_attributes = ReadBE16(
				data, layout_offset + static_cast<Uint32>(layout_index * 2U));
			const std::size_t brush_index = layout_attributes & kTileIndexMask;
			const bool layout_flip_x = (layout_attributes & kHorizontalFlip) != 0U;
			const bool layout_flip_y = (layout_attributes & kVerticalFlip) != 0U;
			const std::size_t source_x = layout_flip_x
				? kBrushWidthTiles - 1U - local_x : local_x;
			const std::size_t source_y = layout_flip_y
				? kBrushHeightTiles - 1U - local_y : local_y;
			const std::size_t brush_cell = source_y * kBrushWidthTiles + source_x;
			const Uint32 tile_offset = brushes_offset + static_cast<Uint32>(
				brush_index * kBrushBytes + brush_cell * 2U);
			if (!CanRead(data, tile_offset, 2U))
			{
				error = "A shared menu brush references data outside the ROM";
				return false;
			}
			output = static_cast<Uint16>(
				ReadBE16(data, tile_offset) ^
				(layout_attributes & (kHorizontalFlip | kVerticalFlip))
			);
			return true;
		}

		std::shared_ptr<const Sprite> BuildSprite(
			const std::vector<Uint8>& data,
			const Uint32 brushes_offset,
			const Uint32 layout_offset,
			const std::vector<Uint8>& art,
			std::vector<Uint8>& palette_line_map,
			std::string& error
		)
		{
			if (art.empty() || (art.size() % kTileBytes) != 0U)
			{
				error = "The shared menu tile payload is invalid";
				return nullptr;
			}
			const std::size_t tile_count = art.size() / kTileBytes;
			auto sprite = std::make_shared<Sprite>();
			sprite->sprite_tiles.reserve(kImageTileCount);
			palette_line_map.assign(
				static_cast<std::size_t>(kImageWidth) * kImageHeight, 0U);

			for (std::size_t tile_y = 0U; tile_y < kImageHeightTiles; ++tile_y)
			{
				for (std::size_t tile_x = 0U; tile_x < kImageWidthTiles; ++tile_x)
				{
					Uint16 attributes = 0U;
					if (!ResolveDisplayedTile(
						data, brushes_offset, layout_offset,
						tile_x, tile_y, attributes, error))
					{
						return nullptr;
					}
					const std::size_t art_tile = attributes & kTileIndexMask;
					if (art_tile >= tile_count)
					{
						std::ostringstream message;
						message << "Shared menu cell references tile " << art_tile
							<< " but the SSC art contains only " << tile_count;
						error = message.str();
						return nullptr;
					}
					const Uint8 palette_line = static_cast<Uint8>(
						(attributes >> 13U) & 3U);
					auto piece = std::make_shared<SpriteTile>();
					piece->x_offset = static_cast<Sint16>(tile_x * 8U);
					piece->y_offset = static_cast<Sint16>(tile_y * 8U);
					piece->x_size = 8U;
					piece->y_size = 8U;
					piece->palette_line = palette_line;
					piece->blit_settings.flip_horizontal =
						(attributes & kHorizontalFlip) != 0U;
					piece->blit_settings.flip_vertical =
						(attributes & kVerticalFlip) != 0U;
					piece->pixel_data.reserve(64U);
					const std::size_t art_offset = art_tile * kTileBytes;
					for (std::size_t byte_index = 0U; byte_index < kTileBytes; ++byte_index)
					{
						const Uint8 packed = art[art_offset + byte_index];
						piece->pixel_data.emplace_back(
							static_cast<Uint32>(palette_line * 16U + (packed >> 4U)));
						piece->pixel_data.emplace_back(
							static_cast<Uint32>(palette_line * 16U + (packed & 0x0FU)));
					}
					for (std::size_t pixel_y = 0U; pixel_y < 8U; ++pixel_y)
					{
						for (std::size_t pixel_x = 0U; pixel_x < 8U; ++pixel_x)
						{
							palette_line_map[(tile_y * 8U + pixel_y) * kImageWidth +
								(tile_x * 8U + pixel_x)] = palette_line;
						}
					}
					sprite->sprite_tiles.emplace_back(std::move(piece));
				}
			}
			sprite->num_tiles = static_cast<Uint16>(sprite->sprite_tiles.size());
			sprite->num_vdp_tiles = static_cast<Uint16>(sprite->sprite_tiles.size());
			sprite->rom_data.SetROMData(layout_offset,
				layout_offset + static_cast<Uint32>(kVisibleLayoutBytes));
			sprite->is_valid = true;
			return sprite;
		}

		bool BuildIndependentData(
			const std::vector<Uint8>& indexed_pixels,
			const int image_width,
			const int image_height,
			std::vector<Uint8>& art,
			std::vector<Uint8>& brushes,
			std::vector<Uint8>& layout,
			std::size_t& visible_tile_count,
			std::size_t& unique_tile_count,
			Uint16& background_pattern_start,
			Uint16& allocator_capacity,
			std::string& error
		)
		{
			if (image_width != kImageWidth || image_height != kImageHeight)
			{
				std::ostringstream message;
				message << "Shared menu background PNG must be exactly "
					<< kImageWidth << 'x' << kImageHeight << " pixels (received "
					<< image_width << 'x' << image_height << ')';
				error = message.str();
				return false;
			}
			const std::size_t pixel_count =
				static_cast<std::size_t>(image_width) * image_height;
			if (indexed_pixels.size() < pixel_count)
			{
				error = "Shared menu indexed-pixel buffer is too small";
				return false;
			}

			struct UniquePattern
			{
				std::array<Uint8, kTileBytes> packed{};
				Uint8 palette_line = 0U;
			};

			std::vector<UniquePattern> unique_patterns;
			unique_patterns.reserve(kMaximumUniqueBackgroundTiles);
			std::vector<int> cell_pattern(kImageTileCount, -1);
			std::vector<Uint8> cell_palette(kImageTileCount, 0U);
			visible_tile_count = 0U;

			for (std::size_t tile_y = 0U; tile_y < kImageHeightTiles; ++tile_y)
			{
				for (std::size_t tile_x = 0U; tile_x < kImageWidthTiles; ++tile_x)
				{
					UniquePattern candidate;
					bool visible = false;
					bool line_chosen = false;
					for (std::size_t pixel_y = 0U; pixel_y < 8U; ++pixel_y)
					{
						for (std::size_t pixel_x = 0U; pixel_x < 8U; ++pixel_x)
						{
							const Uint8 combined = indexed_pixels[
								(tile_y * 8U + pixel_y) * kImageWidth +
								(tile_x * 8U + pixel_x)];
							const Uint8 local = static_cast<Uint8>(combined & 0x0FU);
							if (local == 0U) continue;
							const Uint8 line = static_cast<Uint8>((combined >> 4U) & 3U);
							if (line_chosen && line != candidate.palette_line)
							{
								error = "A Mega Drive menu tile cannot mix palette lines inside one 8x8 cell";
								return false;
							}
							candidate.palette_line = line;
							line_chosen = true;
							visible = true;
							const std::size_t packed_offset =
								pixel_y * 4U + pixel_x / 2U;
							if ((pixel_x & 1U) == 0U)
							{
								candidate.packed[packed_offset] =
									static_cast<Uint8>(local << 4U);
							}
							else
							{
								candidate.packed[packed_offset] = static_cast<Uint8>(
									candidate.packed[packed_offset] | local);
							}
						}
					}

					const std::size_t cell = tile_y * kImageWidthTiles + tile_x;
					if (!visible) continue;
					++visible_tile_count;
					cell_palette[cell] = candidate.palette_line;
					auto existing = std::find_if(
						unique_patterns.begin(), unique_patterns.end(),
						[&candidate](const UniquePattern& pattern)
						{
							return pattern.palette_line == candidate.palette_line &&
								pattern.packed == candidate.packed;
						});
					if (existing == unique_patterns.end())
					{
						if (unique_patterns.size() >= kMaximumUniqueBackgroundTiles)
						{
							std::ostringstream message;
							message << "The imported shared menu background needs more than "
								<< kMaximumUniqueBackgroundTiles
								<< " unique non-empty 8x8 patterns. This limit reserves "
								<< (kMinimumBackgroundPattern - kMaximumOriginalAllocatorStart)
								<< " patterns even when the menu is opened directly after a level.";
							error = message.str();
							return false;
						}
						cell_pattern[cell] = static_cast<int>(unique_patterns.size());
						unique_patterns.emplace_back(candidate);
					}
					else
					{
						cell_pattern[cell] = static_cast<int>(
							std::distance(unique_patterns.begin(), existing));
					}
				}
			}

			unique_tile_count = unique_patterns.size();
			background_pattern_start = static_cast<Uint16>(
				kMaximumPatternTiles - unique_tile_count);
			if (background_pattern_start < kMinimumBackgroundPattern ||
				background_pattern_start < kMaximumOriginalAllocatorStart)
			{
				error = "The shared menu background leaves insufficient VRAM for runtime menu graphics";
				return false;
			}
			// This is the guaranteed minimum capacity. The injected guard calculates
			// the exact capacity from the allocator's actual start each time one of
			// the three screens is loaded.
			allocator_capacity = static_cast<Uint16>(
				background_pattern_start - kMaximumOriginalAllocatorStart);

			// The SSC loader always writes from pattern zero.  Pad up to the guarded
			// top range, then place the unique background patterns through $5FF.
			art.assign(static_cast<std::size_t>(background_pattern_start) *
				kTileBytes, 0U);
			for (const UniquePattern& pattern : unique_patterns)
			{
				art.insert(art.end(), pattern.packed.begin(), pattern.packed.end());
			}
			if (art.size() != kMaximumPatternTiles * kTileBytes)
			{
				error = "Internal error while building the guarded shared menu pattern table";
				return false;
			}

			brushes.assign(kRuntimeLayoutCellCount * kBrushBytes, 0U);
			layout.assign(kRuntimeLayoutBytes, 0U);
			for (std::size_t tile_y = 0U; tile_y < kImageHeightTiles; ++tile_y)
			{
				for (std::size_t tile_x = 0U; tile_x < kImageWidthTiles; ++tile_x)
				{
					const std::size_t cell = tile_y * kImageWidthTiles + tile_x;
					Uint16 tile_index = 0U;
					Uint8 palette_line = 0U;
					if (cell_pattern[cell] >= 0)
					{
						tile_index = static_cast<Uint16>(background_pattern_start +
							static_cast<Uint16>(cell_pattern[cell]));
						palette_line = cell_palette[cell];
					}
					const std::size_t block_x = tile_x / kBrushWidthTiles;
					const std::size_t block_y = tile_y / kBrushHeightTiles;
					const std::size_t block_index =
						block_y * kLayoutWidthBrushes + block_x;
					const std::size_t local_x = tile_x % kBrushWidthTiles;
					const std::size_t local_y = tile_y % kBrushHeightTiles;
					const std::size_t brush_cell =
						local_y * kBrushWidthTiles + local_x;
					const Uint16 attributes = static_cast<Uint16>(
						(static_cast<Uint16>(palette_line) << 13U) | tile_index);
					WriteBE16(brushes,
						static_cast<Uint32>(block_index * kBrushBytes +
							brush_cell * 2U), attributes);
				}
			}

			for (std::size_t block = 0U; block < kVisibleLayoutCellCount; ++block)
			{
				WriteBE16(layout, static_cast<Uint32>(block * 2U),
					static_cast<Uint16>(block));
			}

			// The plane builder reads one hidden tile row below the 224-pixel image.
			// Dedicated padding brushes repeat the final visible row.
			for (std::size_t block_x = 0U; block_x < kLayoutWidthBrushes; ++block_x)
			{
				const std::size_t source_brush =
					(kVisibleLayoutHeightBrushes - 1U) * kLayoutWidthBrushes + block_x;
				const std::size_t padding_brush = kVisibleLayoutCellCount + block_x;
				for (std::size_t local_y = 0U; local_y < kBrushHeightTiles; ++local_y)
				{
					for (std::size_t local_x = 0U; local_x < kBrushWidthTiles; ++local_x)
					{
						const Uint16 attributes = ReadBE16(
							brushes,
							static_cast<Uint32>(source_brush * kBrushBytes +
								(3U * kBrushWidthTiles + local_x) * 2U));
						WriteBE16(
							brushes,
							static_cast<Uint32>(padding_brush * kBrushBytes +
								(local_y * kBrushWidthTiles + local_x) * 2U),
							attributes);
					}
				}
				WriteBE16(layout,
					static_cast<Uint32>((kVisibleLayoutCellCount + block_x) * 2U),
					static_cast<Uint16>(padding_brush));
			}
			return true;
		}

		std::vector<Uint8> RasteriseIndependentData(
			const std::vector<Uint8>& art,
			const std::vector<Uint8>& brushes,
			const std::vector<Uint8>& layout,
			std::string& error)
		{
			std::vector<Uint8> output(
				static_cast<std::size_t>(kImageWidth) * kImageHeight, 0U);
			if (art.empty() || (art.size() % kTileBytes) != 0U ||
				brushes.size() < kRuntimeLayoutCellCount * kBrushBytes ||
				layout.size() < kRuntimeLayoutBytes)
			{
				error = "The rebuilt shared menu data is incomplete";
				return {};
			}
			const std::size_t tile_count = art.size() / kTileBytes;
			for (std::size_t tile_y = 0U; tile_y < kImageHeightTiles; ++tile_y)
			{
				for (std::size_t tile_x = 0U; tile_x < kImageWidthTiles; ++tile_x)
				{
					const std::size_t block_index =
						(tile_y / 4U) * kLayoutWidthBrushes + tile_x / 4U;
					const Uint16 layout_attributes = ReadBE16(
						layout, static_cast<Uint32>(block_index * 2U));
					const std::size_t brush_index = layout_attributes & kTileIndexMask;
					const std::size_t brush_cell =
						(tile_y % 4U) * 4U + (tile_x % 4U);
					const Uint16 attributes = ReadBE16(
						brushes,
						static_cast<Uint32>(brush_index * kBrushBytes + brush_cell * 2U));
					const std::size_t art_tile = attributes & kTileIndexMask;
					if (art_tile >= tile_count)
					{
						error = "The rebuilt menu layout references a tile outside its art";
						return {};
					}
					const Uint8 line = static_cast<Uint8>((attributes >> 13U) & 3U);
					for (std::size_t pixel_y = 0U; pixel_y < 8U; ++pixel_y)
					{
						for (std::size_t pixel_x = 0U; pixel_x < 8U; ++pixel_x)
						{
							const Uint8 packed = art[art_tile * kTileBytes +
								pixel_y * 4U + pixel_x / 2U];
							const Uint8 local = (pixel_x & 1U) == 0U
								? static_cast<Uint8>(packed >> 4U)
								: static_cast<Uint8>(packed & 0x0FU);
							output[(tile_y * 8U + pixel_y) * kImageWidth +
								(tile_x * 8U + pixel_x)] = local == 0U
								? 0U : static_cast<Uint8>(line * 16U + local);
						}
					}
				}
			}
			return output;
		}

		void WriteMetadata(std::vector<Uint8>& data, const Uint16 tile_count,
			const Uint32 compressed_size,
			const Uint16 background_pattern_start,
			const Uint16 allocator_capacity,
			const Uint16 unique_tile_count,
			const Uint16 visible_tile_count)
		{
			std::fill(
				data.begin() + static_cast<std::ptrdiff_t>(kReservedMetadataOffset),
				data.begin() + static_cast<std::ptrdiff_t>(
					kReservedMetadataOffset + kReservedMetadataSize),
				0U);
			std::copy(
				kReservedSignature.begin(), kReservedSignature.end(),
				data.begin() + static_cast<std::ptrdiff_t>(kReservedMetadataOffset));
			WriteBE16(data, kReservedMetadataOffset + 8U, kReservedFormatVersion);
			WriteBE16(data, kReservedMetadataOffset + 10U,
				static_cast<Uint16>(kImageWidthTiles));
			WriteBE16(data, kReservedMetadataOffset + 12U,
				static_cast<Uint16>(kImageHeightTiles));
			WriteBE16(data, kReservedMetadataOffset + 14U, tile_count);
			WriteBE32(data, kReservedMetadataOffset + 16U, compressed_size);
			WriteBE32(data, kReservedMetadataOffset + 20U, kReservedArtHeaderOffset);
			WriteBE32(data, kReservedMetadataOffset + 24U, kReservedBrushesOffset);
			WriteBE32(data, kReservedMetadataOffset + 28U, kReservedLayoutOffset);
			WriteBE16(data, kReservedMetadataOffset + 32U, background_pattern_start);
			WriteBE16(data, kReservedMetadataOffset + 34U, allocator_capacity);
			WriteBE16(data, kReservedMetadataOffset + 36U, unique_tile_count);
			WriteBE16(data, kReservedMetadataOffset + 38U, visible_tile_count);
		}
	}

	Uint32 MenuBackgroundDecoder::ResolveArtHeaderOffset(const SpinballROM& rom)
	{
		return ResolveArt(rom.m_buffer);
	}

	Uint32 MenuBackgroundDecoder::ResolveBrushesOffset(const SpinballROM& rom)
	{
		return ResolveBrushes(rom.m_buffer);
	}

	Uint32 MenuBackgroundDecoder::ResolveLayoutOffset(const SpinballROM& rom)
	{
		return ResolveLayout(rom.m_buffer);
	}

	Uint32 MenuBackgroundDecoder::ResolveBrushesEndOffset(const SpinballROM& rom)
	{
		std::string error;
		const Uint32 layout = ResolveLayout(rom.m_buffer);
		const std::size_t brush_count = RequiredBrushCount(rom.m_buffer, layout, error);
		const Uint32 brushes = ResolveBrushes(rom.m_buffer);
		if (brush_count == 0U) return brushes;
		return brushes + static_cast<Uint32>(brush_count * kBrushBytes);
	}

	Uint32 MenuBackgroundDecoder::ResolveLayoutEndOffset(const SpinballROM& rom)
	{
		const Uint32 layout = ResolveLayout(rom.m_buffer);
		return layout + static_cast<Uint32>(
			StoredLayoutCellCount(rom.m_buffer, layout) * 2U);
	}

	MenuBackgroundDecodeResult MenuBackgroundDecoder::Decode(const SpinballROM& rom)
	{
		MenuBackgroundDecodeResult result;
		const Uint32 art_header = ResolveArt(rom.m_buffer);
		const Uint32 brushes = ResolveBrushes(rom.m_buffer);
		const Uint32 layout = ResolveLayout(rom.m_buffer);
		std::string error;
		const std::size_t brush_count = RequiredBrushCount(rom.m_buffer, layout, error);
		if (brush_count == 0U)
		{
			result.error = error;
			return result;
		}
		if (!CanRead(rom.m_buffer, brushes, brush_count * kBrushBytes))
		{
			result.error = "The shared menu brush table is outside the ROM";
			return result;
		}

		std::vector<Uint8> art;
		std::size_t compressed_size = 0U;
		if (!DecodeArt(rom, art_header, art, compressed_size, error))
		{
			result.error = error;
			return result;
		}
		result.palette_set = rom.GetOptionsScreenPaletteSet();
		if (!result.palette_set)
		{
			result.error = "Could not load the four Options/Hi-Score/Credits palette lines";
			return result;
		}
		for (const std::shared_ptr<Palette>& line : result.palette_set->palette_lines)
		{
			if (!line)
			{
				result.error = "One of the four shared menu palette lines could not be loaded";
				return result;
			}
		}

		std::vector<Uint8> palette_line_map;
		std::shared_ptr<const Sprite> sprite = BuildSprite(
			rom.m_buffer, brushes, layout, art, palette_line_map, error);
		if (!sprite)
		{
			result.error = error;
			return result;
		}

		MenuBackgroundFrame frame;
		frame.category = MenuBackgroundCategory::OPTIONS;
		frame.name = "Options / Hi-Score / Credits Background";
		frame.usage = "One shared 320x224 ROM image used by all three screens";
		frame.frame_id = 600U;
		frame.sprite = sprite;
		frame.palette_line_map = palette_line_map;
		result.frames.emplace_back(std::move(frame));
		if (IsReservedInstalled(rom.m_buffer))
		{
			result.warnings.emplace_back(IsCurrentReservedFormat(rom.m_buffer)
				? "The shared menu background uses fixed relocated ROM areas and a guarded top-of-VRAM pattern range which the runtime menu allocator cannot overwrite."
				: "The shared menu background uses an older SpinTool layout and will be upgraded on the next import.");
		}
		return result;
	}

	MenuBackgroundImportResult MenuBackgroundDecoder::ImportIndexedImage(
		SpinballROM& rom,
		const SpinballROM& reference_rom,
		const std::vector<Uint8>& indexed_pixels,
		const int image_width,
		const int image_height)
	{
		MenuBackgroundImportResult result;
		std::string error;
		if (!ValidatePatchOperands(rom.m_buffer, error))
		{
			result.message = error;
			return result;
		}
		if (!IsValidArtHeader(reference_rom.m_buffer, kOriginalArtHeaderOffset) ||
			!CanRead(reference_rom.m_buffer, kOriginalBrushesOffset,
				kOriginalBrushCount * kBrushBytes) ||
			!CanRead(reference_rom.m_buffer, kOriginalLayoutOffset,
				kVisibleLayoutBytes))
		{
			result.message = "The reference ROM does not contain the expected original shared menu resources";
			return result;
		}

		std::vector<Uint8> art;
		std::vector<Uint8> brushes;
		std::vector<Uint8> layout;
		std::size_t visible_tile_count = 0U;
		std::size_t unique_tile_count = 0U;
		Uint16 background_pattern_start = 0U;
		Uint16 allocator_capacity = 0U;
		if (!BuildIndependentData(
			indexed_pixels, image_width, image_height,
			art, brushes, layout, visible_tile_count, unique_tile_count,
			background_pattern_start, allocator_capacity, error))
		{
			result.message = error;
			return result;
		}
		const std::vector<Uint8> verified_pixels = RasteriseIndependentData(
			art, brushes, layout, error);
		if (verified_pixels.empty() || verified_pixels != indexed_pixels)
		{
			result.message = error.empty()
				? "Import refused: rebuilt menu art/layout differs from the PNG"
				: error;
			return result;
		}

		const SSCCompressionResult compression = SSCCompressor::CompressData(
			art, 0U, static_cast<Uint32>(art.size()));
		const std::size_t stream_capacity = kReservedArtRegionSize - 2U;
		if (compression.empty() || compression.size() > stream_capacity)
		{
			std::ostringstream message;
			message << "Import refused: rebuilt menu SSC stream needs "
				<< compression.size() << " bytes, but the fixed art area has "
				<< stream_capacity;
			result.message = message.str();
			return result;
		}
		const SSCDecompressionResult verification = SSCDecompressor::DecompressData(
			compression, 0U, static_cast<Uint32>(art.size()));
		if (verification.error_msg.has_value() ||
			verification.uncompressed_data != art ||
			verification.rom_data.real_size != compression.size())
		{
			result.message = "Import refused: SSC verification produced different shared menu art";
			return result;
		}

		result.remaining_bytes = stream_capacity - compression.size();
		result.compression_saved_bytes = art.size() > compression.size()
			? art.size() - compression.size()
			: 0U;

		// Avoid rewriting a ROM that already contains exactly the same relocated
		// payload and pointer set.
		if (IsCurrentReservedFormat(rom.m_buffer) &&
			CanRead(rom.m_buffer, kReservedArtHeaderOffset, 2U + compression.size()) &&
			ReadBE16(rom.m_buffer, kReservedArtHeaderOffset) == art.size() / kTileBytes &&
			std::equal(compression.begin(), compression.end(),
				rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(kReservedArtHeaderOffset + 2U)) &&
			CanRead(rom.m_buffer, kReservedBrushesOffset, brushes.size()) &&
			std::equal(brushes.begin(), brushes.end(),
				rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(kReservedBrushesOffset)) &&
			CanRead(rom.m_buffer, kReservedLayoutOffset, layout.size()) &&
			std::equal(layout.begin(), layout.end(),
				rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(kReservedLayoutOffset)))
		{
			result.success = true;
			result.changed = false;
			result.message = "The PNG is identical to the current shared Options/Hi-Score/Credits background; the ROM was not modified.";
			return result;
		}

		ExpandROM(rom.m_buffer);
		WriteBE16(rom.m_buffer, kReservedArtHeaderOffset,
			static_cast<Uint16>(art.size() / kTileBytes));
		std::copy(compression.begin(), compression.end(),
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(kReservedArtHeaderOffset + 2U));
		std::fill(
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
				kReservedArtHeaderOffset + 2U + compression.size()),
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
				kReservedArtHeaderOffset + kReservedArtRegionSize),
			0U);
		std::copy(brushes.begin(), brushes.end(),
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(kReservedBrushesOffset));
		std::fill(
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
				kReservedBrushesOffset + brushes.size()),
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
				kReservedBrushesOffset + kReservedBrushesRegionSize),
			0U);
		std::copy(layout.begin(), layout.end(),
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(kReservedLayoutOffset));
		std::fill(
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
				kReservedLayoutOffset + layout.size()),
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
				kReservedLayoutOffset + kReservedLayoutRegionSize),
			0U);

		for (const Uint32 operand : kArtPointerOperands)
		{
			WriteBE32(rom.m_buffer, operand, kReservedArtHeaderOffset);
		}
		for (const Uint32 operand : kArtLoaderCallTargetOperands)
		{
			WriteBE32(rom.m_buffer, operand, kAllocatorGuardCodeOffset);
		}
		// Restore the original 42-pattern header as the HUD/font base even when
		// upgrading a ROM produced by SpinTool 15-17.
		WriteBE32(rom.m_buffer, kHudBaseHeaderPointerOperand,
			kOriginalArtHeaderOffset);
		WriteBE32(rom.m_buffer, kBrushesPointerOperand, kReservedBrushesOffset);
		WriteBE32(rom.m_buffer, kLayoutPointerOperand, kReservedLayoutOffset);
		WriteMetadata(
			rom.m_buffer,
			static_cast<Uint16>(art.size() / kTileBytes),
			static_cast<Uint32>(compression.size()),
			background_pattern_start,
			allocator_capacity,
			static_cast<Uint16>(unique_tile_count),
			static_cast<Uint16>(visible_tile_count));
		// Write this after metadata because metadata initialization clears the
		// complete 0x312000-0x312FFF reservation.
		WriteAllocatorGuardCode(rom.m_buffer, background_pattern_start);
		UpdateMegaDriveChecksum(rom);

		result.success = true;
		result.changed = true;
		std::ostringstream message;
		message << "Shared Options / Hi-Score / Credits background relocated to fixed ROM areas 0x"
			<< std::hex << std::uppercase << kReservedArtHeaderOffset
			<< " (art), 0x" << kReservedBrushesOffset << " (4x4 brushes) and 0x"
			<< kReservedLayoutOffset << " (10x7 layout). VRAM patterns $"
			<< background_pattern_start << "-$5FF are reserved for the background; "
			<< "the runtime graphics allocator is clipped to the actual free-pattern "
			<< "start and stopped below $" << background_pattern_start << ". "
			<< std::dec
			<< visible_tile_count << " visible cells use " << unique_tile_count
			<< " unique patterns; compressed art " << compression.size() << '/'
			<< stream_capacity << " bytes. The original $2A HUD/font base and the "
			<< "protected hidden row are preserved.";
		result.message = message.str();
		return result;
	}
}
