#include "rom/title_screen_decoder.h"

#include "rom/compressed2_optimizer.h"

// tile.h must be complete before spinball_rom.h/tileset.h with GCC 15.
#include "rom/tile.h"
#include "rom/spinball_rom.h"
#include "rom/palette.h"
#include "rom/sprite.h"
#include "rom/sprite_tile.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

namespace spintool::rom
{
	namespace
	{
		constexpr Uint16 kTileIndexMask = 0x07FFU;
		constexpr Uint16 kHorizontalFlip = 0x0800U;
		constexpr Uint16 kVerticalFlip = 0x1000U;
		constexpr std::size_t kTileBytes = 32U;

		constexpr Uint32 kOriginalTitleArtHeaderOffset = 0x0009D102U;
		constexpr Uint32 kOriginalTitleBackgroundLayout = 0x0009C82EU;
		constexpr Uint32 kTitleArtPointerOperandOffset = 0x000F2E7AU;
		constexpr Uint32 kTitleBackgroundLayoutPointerOffset = 0x00099370U;

		// SpinTool permanently reserves a fixed area in an expanded 4 MiB ROM.
		// The game keeps using its original 68k loader; only its absolute data
		// pointers are redirected to these ROM addresses.
		constexpr std::size_t kExpandedROMSize = 0x00400000U;
		constexpr Uint32 kReservedMetadataOffset = 0x00300000U;
		constexpr std::size_t kReservedMetadataSize = 0x00001000U;
		constexpr Uint32 kReservedTitleArtHeaderOffset = 0x00301000U;
		constexpr std::size_t kReservedTitleArtRegionSize = 0x00010000U;
		constexpr Uint32 kReservedTitleBackgroundLayout = 0x00311000U;
		constexpr std::size_t kReservedTitleBackgroundLayoutSize = 0x00001000U;
		constexpr std::array<Uint8, 8> kReservedSignature
		{
			'S', 'P', 'T', 'L', 'B', 'G', '0', '1'
		};
		constexpr Uint16 kReservedFormatVersion = 4U;
		constexpr Uint16 kMinimumSupportedReservedFormatVersion = 1U;
		constexpr Uint16 kTitleBackgroundWidthTiles = 40U;
		constexpr Uint16 kTitleBackgroundHeightTiles = 28U;
		constexpr std::size_t kTitleBackgroundFrameId = 500U;
		// Plane A starts at VRAM $C000, leaving pattern indices 0-$5FF.
		constexpr std::size_t kMaximumTitlePatternTiles = 0x0600U;
		constexpr Uint16 kTitleBaseTile = 0x0000U;
		constexpr std::array<Uint32, 7> kTitleSonicObjectTables
		{
			0x00099440U,
			0x000994BAU,
			0x00099534U,
			0x000995AEU,
			0x00099628U,
			0x000996A2U,
			0x0009971CU,
		};
		// The title screen stores each major visual element in its own object table.
		// Their order in ROM does not match their visual/name order on screen.
		constexpr Uint32 kTitleLogoSonicObjectTable = 0x00099796U;
		constexpr Uint32 kTitleLogoTheHedgehogObjectTable = 0x00099816U;
		constexpr Uint32 kTitleLogoSpinballObjectTable = 0x00099848U;
		// This object table is another Sonic animation composition, not the
		// large pinball support visible underneath him.
		constexpr Uint32 kTitleAdditionalSonicObjectTable = 0x000998A4U;
		// The support/bumper itself is stored as the title-screen foreground
		// tile layout and uses the same shared title-screen graphics block.
		constexpr Uint32 kTitleBumperRingTileLayout = 0x0009C05AU;
		// The small "TM &" displayed immediately before "© 1993 SEGA" is not
		// part of either title tile plane.  The game creates it as one 3x1
		// runtime sprite from this descriptor.  SpinTool overlays it on the
		// exported 320x224 background and, on import, migrates its pixels into
		// Plane B before disabling the original sprite descriptor.
		constexpr Uint32 kTitleRuntimeTrademarkDescriptor = 0x0009D0FAU;
		constexpr int kTitleRuntimeTrademarkScreenX = 142;
		constexpr int kTitleRuntimeTrademarkScreenY = 192;
		constexpr std::size_t kTitleRuntimeTrademarkFirstTile = 0x0486U;
		constexpr std::size_t kTitleRuntimeTrademarkTileCount = 3U;
		constexpr Uint32 kTitlePaletteDataOffset = 0x0009BD3AU;
		constexpr Uint8 kTitleBackgroundPaletteLine = 1U;
		constexpr Uint8 kTitleBackgroundLightGreySlot = 9U;
		constexpr Uint8 kTitleBackgroundMidGreySlot = 10U;
		constexpr Uint16 kTitleBackgroundLightGrey = 0x0CCCU;
		constexpr Uint16 kTitleBackgroundMidGrey = 0x0666U;
		constexpr Uint32 kBlankDescriptor = 0x00099430U;
		constexpr int kMaximumFrameDimension = 2048;

		struct PieceDescriptor
		{
			Uint32 rom_offset = 0;
			Uint8 width_in_tiles = 0;
			Uint8 height_in_tiles = 0;
			Sint16 x = 0;
			Sint16 y = 0;
			Uint16 attributes = 0;
		};

		struct PieceInstance
		{
			PieceDescriptor descriptor;
			Uint16 effective_attributes = 0;
			int position_x_pixels = 0;
			int position_y_pixels = 0;
			bool writable = true;
		};

		struct FrameDefinition
		{
			TitleScreenCategory category = TitleScreenCategory::SONIC;
			std::string name;
			std::string usage;
			std::size_t frame_id = 0;
			std::vector<PieceInstance> pieces;
			Uint16 base_tile = kTitleBaseTile;
			Uint32 art_offset = kOriginalTitleArtHeaderOffset + 2U;
		};


		struct ObjectTableEntry
		{
			Uint8 slot = 0;
			Uint32 descriptor_offset = 0;
			Uint16 stored_flags = 0;
		};

		struct PixelBounds
		{
			int minimum_x = 0;
			int minimum_y = 0;
			int maximum_x = 0;
			int maximum_y = 0;

			[[nodiscard]] int Width() const { return maximum_x - minimum_x; }
			[[nodiscard]] int Height() const { return maximum_y - minimum_y; }
		};

		struct RasterImage
		{
			int width = 0;
			int height = 0;
			std::vector<Uint8> pixels;

			bool operator==(const RasterImage& rhs) const
			{
				return width == rhs.width && height == rhs.height && pixels == rhs.pixels;
			}
		};

		bool CanRead(const std::vector<Uint8>& data, const Uint32 offset, const std::size_t count)
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

		void WriteBE16(std::vector<Uint8>& data, const Uint32 offset, const Uint16 value)
		{
			data[offset] = static_cast<Uint8>((value >> 8U) & 0xFFU);
			data[offset + 1U] = static_cast<Uint8>(value & 0xFFU);
		}

		void WriteBE32(std::vector<Uint8>& data, const Uint32 offset, const Uint32 value)
		{
			data[offset] = static_cast<Uint8>((value >> 24U) & 0xFFU);
			data[offset + 1U] = static_cast<Uint8>((value >> 16U) & 0xFFU);
			data[offset + 2U] = static_cast<Uint8>((value >> 8U) & 0xFFU);
			data[offset + 3U] = static_cast<Uint8>(value & 0xFFU);
		}

		bool HasReservedSignature(const std::vector<Uint8>& data)
		{
			if (!CanRead(data, kReservedMetadataOffset, kReservedSignature.size()))
			{
				return false;
			}
			return std::equal(
				kReservedSignature.begin(),
				kReservedSignature.end(),
				data.begin() + static_cast<std::ptrdiff_t>(kReservedMetadataOffset)
			);
		}

		Uint32 ResolveTitleArtHeaderOffset(const std::vector<Uint8>& data)
		{
			if (CanRead(data, kTitleArtPointerOperandOffset, 4U))
			{
				const Uint32 candidate = ReadBE32(data, kTitleArtPointerOperandOffset);
				if (CanRead(data, candidate, 2U) && ReadBE16(data, candidate) == 0xFFFFU)
				{
					return candidate;
				}
			}
			return kOriginalTitleArtHeaderOffset;
		}

		Uint32 ResolveTitleBackgroundLayoutOffset(const std::vector<Uint8>& data)
		{
			if (CanRead(data, kTitleBackgroundLayoutPointerOffset, 4U))
			{
				const Uint32 candidate = ReadBE32(data, kTitleBackgroundLayoutPointerOffset);
				if (CanRead(data, candidate, 4U) &&
					ReadBE16(data, candidate) == kTitleBackgroundWidthTiles &&
					ReadBE16(data, candidate + 2U) == kTitleBackgroundHeightTiles)
				{
					return candidate;
				}
			}
			return kOriginalTitleBackgroundLayout;
		}

		bool IsReservedBackgroundInstalled(const std::vector<Uint8>& data)
		{
			if (!HasReservedSignature(data) ||
				!CanRead(data, kReservedMetadataOffset + 10U, 8U) ||
				ReadBE16(data, kReservedMetadataOffset + 8U) <
					kMinimumSupportedReservedFormatVersion ||
				ReadBE16(data, kReservedMetadataOffset + 8U) > kReservedFormatVersion ||
				ReadBE16(data, kReservedMetadataOffset + 10U) != kTitleBackgroundWidthTiles ||
				ReadBE16(data, kReservedMetadataOffset + 12U) != kTitleBackgroundHeightTiles)
			{
				return false;
			}
			return ResolveTitleArtHeaderOffset(data) == kReservedTitleArtHeaderOffset &&
				ResolveTitleBackgroundLayoutOffset(data) == kReservedTitleBackgroundLayout;
		}

		void ExpandROMForReservedTitleData(std::vector<Uint8>& data)
		{
			if (data.size() < kExpandedROMSize)
			{
				data.resize(kExpandedROMSize, 0xFFU);
			}
			// Mega Drive header: inclusive ROM end address.
			if (CanRead(data, 0x000001A4U, 4U))
			{
				WriteBE32(data, 0x000001A4U, static_cast<Uint32>(data.size() - 1U));
			}
		}

		void WriteReservedMetadata(
			std::vector<Uint8>& data,
			const Uint16 protected_tile_count,
			const Uint16 total_tile_count
		)
		{
			std::fill(
				data.begin() + static_cast<std::ptrdiff_t>(kReservedMetadataOffset),
				data.begin() + static_cast<std::ptrdiff_t>(
					kReservedMetadataOffset + kReservedMetadataSize
				),
				0U
			);
			std::copy(
				kReservedSignature.begin(),
				kReservedSignature.end(),
				data.begin() + static_cast<std::ptrdiff_t>(kReservedMetadataOffset)
			);
			WriteBE16(data, kReservedMetadataOffset + 8U, kReservedFormatVersion);
			WriteBE16(data, kReservedMetadataOffset + 10U, kTitleBackgroundWidthTiles);
			WriteBE16(data, kReservedMetadataOffset + 12U, kTitleBackgroundHeightTiles);
			WriteBE16(data, kReservedMetadataOffset + 14U, protected_tile_count);
			WriteBE16(data, kReservedMetadataOffset + 16U, total_tile_count);
			WriteBE32(data, kReservedMetadataOffset + 20U, kReservedTitleArtHeaderOffset);
			WriteBE32(data, kReservedMetadataOffset + 24U, kReservedTitleBackgroundLayout);
		}

		Sint16 ReadBE16Signed(const std::vector<Uint8>& data, const Uint32 offset)
		{
			return static_cast<Sint16>(ReadBE16(data, offset));
		}

		Uint16 ApplyObjectFlags(const Uint16 attributes, const Uint16 flags)
		{
			const Uint16 filtered = static_cast<Uint16>(flags & 0xF8F8U);
			const Uint16 transformed = static_cast<Uint16>(
				((filtered & 0x00F8U) << 8U) |
				(filtered & 0xF800U)
			);
			return static_cast<Uint16>(attributes ^ transformed);
		}

		int FixedPointToPixels(const Sint16 value)
		{
			const int expanded = static_cast<int>(value);
			if (expanded >= 0)
			{
				return expanded / 16;
			}
			return -((-expanded + 15) / 16);
		}

		Sint16 AddWord(const Sint16 lhs, const Sint16 rhs)
		{
			return static_cast<Sint16>(static_cast<Uint16>(lhs) + static_cast<Uint16>(rhs));
		}

		bool ParsePieceDescriptor(
			const std::vector<Uint8>& rom,
			const Uint32 offset,
			PieceDescriptor& output,
			std::string& error
		)
		{
			if (!CanRead(rom, offset, 8U))
			{
				error = "Title-screen object descriptor is outside the ROM";
				return false;
			}

			const Uint8 size_code = static_cast<Uint8>(rom[offset + 1U] & 0x0FU);
			output.rom_offset = offset;
			output.width_in_tiles = static_cast<Uint8>(((size_code >> 2U) & 3U) + 1U);
			output.height_in_tiles = static_cast<Uint8>((size_code & 3U) + 1U);
			output.x = ReadBE16Signed(rom, offset + 2U);
			output.y = ReadBE16Signed(rom, offset + 4U);
			output.attributes = ReadBE16(rom, offset + 6U);
			return true;
		}

		bool IsRawBlankDescriptor(const std::vector<Uint8>& rom, const Uint32 offset)
		{
			if (offset == 0U || offset == kBlankDescriptor)
			{
				return true;
			}
			if (!CanRead(rom, offset, 8U))
			{
				return false;
			}
			return std::all_of(
				rom.begin() + static_cast<std::ptrdiff_t>(offset),
				rom.begin() + static_cast<std::ptrdiff_t>(offset + 8U),
				[](const Uint8 value) { return value == 0U; }
			);
		}

		bool ReadObjectTableEntries(
			const std::vector<Uint8>& rom,
			const Uint32 table_offset,
			std::vector<ObjectTableEntry>& entries,
			std::string& error
		)
		{
			entries.clear();
			if (!CanRead(rom, table_offset, 2U))
			{
				error = "Title-screen object list is outside the ROM";
				return false;
			}
			const Uint8 first_slot = rom[table_offset];
			const Uint8 count = rom[table_offset + 1U];
			if (count == 0U || count > 96U ||
				!CanRead(rom, table_offset + 2U, static_cast<std::size_t>(count) * 6U))
			{
				error = "Title-screen object list has an invalid entry count";
				return false;
			}

			entries.reserve(count);
			Uint32 cursor = table_offset + 2U;
			for (Uint8 index = 0U; index < count; ++index, cursor += 6U)
			{
				ObjectTableEntry entry;
				entry.slot = static_cast<Uint8>(first_slot + index);
				entry.descriptor_offset = ReadBE32(rom, cursor);
				entry.stored_flags = ReadBE16(rom, cursor + 4U);
				entries.emplace_back(entry);
			}
			return true;
		}

		bool AppendObjectTableFrame(
			const std::vector<Uint8>& rom,
			const Uint32 table_offset,
			const TitleScreenCategory category,
			std::string name,
			std::string usage,
			const std::size_t frame_id,
			std::vector<FrameDefinition>& output,
			std::string& error,
			const Uint32 first_descriptor = 0U,
			const Uint32 last_descriptor = std::numeric_limits<Uint32>::max()
		)
		{
			std::vector<ObjectTableEntry> entries;
			if (!ReadObjectTableEntries(rom, table_offset, entries, error))
			{
				return false;
			}

			FrameDefinition definition;
			definition.category = category;
			definition.name = std::move(name);
			definition.usage = std::move(usage);
			definition.frame_id = frame_id;
			for (const ObjectTableEntry& entry : entries)
			{
				if (entry.descriptor_offset < first_descriptor ||
					entry.descriptor_offset > last_descriptor ||
					IsRawBlankDescriptor(rom, entry.descriptor_offset))
				{
					continue;
				}

				PieceDescriptor descriptor;
				if (!ParsePieceDescriptor(rom, entry.descriptor_offset, descriptor, error))
				{
					return false;
				}
				PieceInstance piece;
				piece.descriptor = descriptor;
				piece.effective_attributes = ApplyObjectFlags(
					descriptor.attributes,
					entry.stored_flags
				);
				piece.position_x_pixels = 0;
				piece.position_y_pixels = 0;
				piece.writable = true;
				definition.pieces.emplace_back(std::move(piece));
			}
			if (definition.pieces.empty())
			{
				error = definition.name + " contains no editable title-screen pieces";
				return false;
			}
			output.emplace_back(std::move(definition));
			return true;
		}

		bool AppendTileLayoutFrame(
			const std::vector<Uint8>& rom,
			const Uint32 layout_offset,
			const TitleScreenCategory category,
			std::string name,
			std::string usage,
			const std::size_t frame_id,
			std::vector<FrameDefinition>& output,
			std::string& error
		)
		{
			if (!CanRead(rom, layout_offset, 4U))
			{
				error = "Title-screen tile layout header is outside the ROM";
				return false;
			}

			const Uint16 width_in_tiles = ReadBE16(rom, layout_offset);
			const Uint16 height_in_tiles = ReadBE16(rom, layout_offset + 2U);
			if (width_in_tiles == 0U || height_in_tiles == 0U ||
				width_in_tiles > 128U || height_in_tiles > 128U)
			{
				error = "Title-screen bumper layout has invalid dimensions";
				return false;
			}

			const std::size_t cell_count =
				static_cast<std::size_t>(width_in_tiles) * height_in_tiles;
			const Uint32 cells_offset = layout_offset + 4U;
			if (!CanRead(rom, cells_offset, cell_count * sizeof(Uint16)))
			{
				error = "Title-screen bumper layout data is outside the ROM";
				return false;
			}

			FrameDefinition definition;
			definition.category = category;
			definition.name = std::move(name);
			definition.usage = std::move(usage);
			definition.frame_id = frame_id;
			definition.pieces.reserve(cell_count);

			for (Uint16 y = 0U; y < height_in_tiles; ++y)
			{
				for (Uint16 x = 0U; x < width_in_tiles; ++x)
				{
					const std::size_t cell_index =
						static_cast<std::size_t>(y) * width_in_tiles + x;
					const Uint32 cell_offset = cells_offset +
						static_cast<Uint32>(cell_index * sizeof(Uint16));
					const Uint16 attributes = ReadBE16(rom, cell_offset);

					// Tile zero is the transparent/empty cell in this foreground
					// layout. Skipping it also crops the assembled frame to the
					// actual support instead of the full 320x224 plane.
					if ((attributes & kTileIndexMask) == 0U)
					{
						continue;
					}

					PieceInstance piece;
					piece.descriptor.rom_offset = cell_offset;
					piece.descriptor.width_in_tiles = 1U;
					piece.descriptor.height_in_tiles = 1U;
					piece.descriptor.x =
						(attributes & kHorizontalFlip) != 0U ? -8 : 0;
					piece.descriptor.y =
						(attributes & kVerticalFlip) != 0U ? -8 : 0;
					piece.descriptor.attributes = attributes;
					piece.effective_attributes = attributes;
					piece.position_x_pixels = static_cast<int>(x) * 8;
					piece.position_y_pixels = static_cast<int>(y) * 8;
					piece.writable = true;
					definition.pieces.emplace_back(std::move(piece));
				}
			}

			if (definition.pieces.empty())
			{
				error = "Title-screen bumper layout contains no visible tiles";
				return false;
			}
			output.emplace_back(std::move(definition));
			return true;
		}

		bool IsRuntimeTrademarkTile(const std::size_t tile_index)
		{
			return tile_index >= kTitleRuntimeTrademarkFirstTile &&
				tile_index < kTitleRuntimeTrademarkFirstTile +
					kTitleRuntimeTrademarkTileCount;
		}

		bool AppendRuntimeTrademarkOverlay(
			const std::vector<Uint8>& rom,
			FrameDefinition& definition,
			std::string& error
		)
		{
			if (!CanRead(rom, kTitleRuntimeTrademarkDescriptor, 8U))
			{
				error = "Title-screen TM & descriptor is outside the ROM";
				return false;
			}

			// Once an image has been imported, this descriptor is deliberately
			// blank because the pixels are then owned by Plane B.
			if (IsRawBlankDescriptor(rom, kTitleRuntimeTrademarkDescriptor))
			{
				return true;
			}

			PieceDescriptor descriptor;
			if (!ParsePieceDescriptor(
				rom, kTitleRuntimeTrademarkDescriptor, descriptor, error
			))
			{
				return false;
			}
			if (descriptor.width_in_tiles != kTitleRuntimeTrademarkTileCount ||
				descriptor.height_in_tiles != 1U)
			{
				error = "Title-screen TM & descriptor has unexpected dimensions";
				return false;
			}

			PieceInstance piece;
			piece.descriptor = descriptor;
			piece.effective_attributes = descriptor.attributes;
			piece.position_x_pixels = kTitleRuntimeTrademarkScreenX;
			piece.position_y_pixels = kTitleRuntimeTrademarkScreenY;
			piece.writable = false;
			// Insert this before Plane B cells. RasteriseSprite draws the vector
			// in reverse order, so the runtime notice remains the final overlay.
			definition.pieces.emplace_back(std::move(piece));
			return true;
		}

		bool DisableRuntimeTrademarkSprite(
			std::vector<Uint8>& rom,
			std::string& error
		)
		{
			if (!CanRead(rom, kTitleRuntimeTrademarkDescriptor, 8U))
			{
				error = "Title-screen TM & descriptor is outside the ROM";
				return false;
			}
			std::fill(
				rom.begin() + static_cast<std::ptrdiff_t>(
					kTitleRuntimeTrademarkDescriptor
				),
				rom.begin() + static_cast<std::ptrdiff_t>(
					kTitleRuntimeTrademarkDescriptor + 8U
				),
				0U
			);
			return true;
		}

		bool AppendBackgroundLayoutFrame(
			const std::vector<Uint8>& rom,
			const Uint32 layout_offset,
			std::vector<FrameDefinition>& output,
			std::string& error
		)
		{
			if (!CanRead(rom, layout_offset, 4U))
			{
				error = "Title-screen background layout header is outside the ROM";
				return false;
			}
			const Uint16 width_in_tiles = ReadBE16(rom, layout_offset);
			const Uint16 height_in_tiles = ReadBE16(rom, layout_offset + 2U);
			if (width_in_tiles != kTitleBackgroundWidthTiles ||
				height_in_tiles != kTitleBackgroundHeightTiles)
			{
				error = "Title-screen background must be exactly 40x28 tiles";
				return false;
			}

			const std::size_t cell_count =
				static_cast<std::size_t>(width_in_tiles) * height_in_tiles;
			const Uint32 cells_offset = layout_offset + 4U;
			if (!CanRead(rom, cells_offset, cell_count * sizeof(Uint16)))
			{
				error = "Title-screen background layout data is outside the ROM";
				return false;
			}

			FrameDefinition definition;
			definition.category = TitleScreenCategory::BACKGROUND;
			definition.name = "Title Screen Background";
			definition.usage =
				"Complete 320x224 title-screen background, including TM &";
			definition.frame_id = kTitleBackgroundFrameId;
			definition.pieces.reserve(cell_count + 1U);
			if (!AppendRuntimeTrademarkOverlay(rom, definition, error))
			{
				return false;
			}

			for (Uint16 y = 0U; y < height_in_tiles; ++y)
			{
				for (Uint16 x = 0U; x < width_in_tiles; ++x)
				{
					const std::size_t cell_index =
						static_cast<std::size_t>(y) * width_in_tiles + x;
					const Uint32 cell_offset = cells_offset +
						static_cast<Uint32>(cell_index * sizeof(Uint16));
					const Uint16 attributes = ReadBE16(rom, cell_offset);

					PieceInstance piece;
					piece.descriptor.rom_offset = cell_offset;
					piece.descriptor.width_in_tiles = 1U;
					piece.descriptor.height_in_tiles = 1U;
					piece.descriptor.x =
						(attributes & kHorizontalFlip) != 0U ? -8 : 0;
					piece.descriptor.y =
						(attributes & kVerticalFlip) != 0U ? -8 : 0;
					piece.descriptor.attributes = attributes;
					piece.effective_attributes = attributes;
					piece.position_x_pixels = static_cast<int>(x) * 8;
					piece.position_y_pixels = static_cast<int>(y) * 8;
					piece.writable = true;
					definition.pieces.emplace_back(std::move(piece));
				}
			}
			output.emplace_back(std::move(definition));
			return true;
		}

		bool BuildFrameDefinitions(
			const std::vector<Uint8>& rom,
			const Uint32 background_layout_offset,
			std::vector<FrameDefinition>& output,
			std::string& error
		)
		{
			output.clear();
			for (std::size_t index = 0U; index < kTitleSonicObjectTables.size(); ++index)
			{
				if (!AppendObjectTableFrame(
					rom,
					kTitleSonicObjectTables[index],
					TitleScreenCategory::SONIC,
					"Sonic - frame " + std::to_string(index + 1U),
					"Sonic title-screen animation",
					index,
					output,
					error
				))
				{
					return false;
				}
			}
			if (!AppendObjectTableFrame(
				rom,
				kTitleAdditionalSonicObjectTable,
				TitleScreenCategory::SONIC,
				"Sonic - frame 8",
				"Additional Sonic title-screen animation composition",
				7U,
				output,
				error
			))
			{
				return false;
			}
			if (!AppendTileLayoutFrame(
				rom,
				kTitleBumperRingTileLayout,
				TitleScreenCategory::BUMPER_RING,
				"Bumper / Ring",
				"Pinball support on which Sonic bounces",
				100U,
				output,
				error
			))
			{
				return false;
			}
			if (!AppendObjectTableFrame(
				rom,
				kTitleLogoSonicObjectTable,
				TitleScreenCategory::LOGO_SONIC,
				"Logo Sonic",
				"SONIC title logo",
				200U,
				output,
				error
			))
			{
				return false;
			}
			if (!AppendObjectTableFrame(
				rom,
				kTitleLogoTheHedgehogObjectTable,
				TitleScreenCategory::LOGO_THE_HEDGEHOG,
				"Logo The Hedgehog",
				"THE HEDGEHOG title logo",
				300U,
				output,
				error
			))
			{
				return false;
			}
			if (!AppendObjectTableFrame(
				rom,
				kTitleLogoSpinballObjectTable,
				TitleScreenCategory::LOGO_SPINBALL,
				"Logo Spinball",
				"SPINBALL title logo",
				400U,
				output,
				error
			))
			{
				return false;
			}
			if (!AppendBackgroundLayoutFrame(
				rom,
				background_layout_offset,
				output,
				error
			))
			{
				return false;
			}
			return true;
		}

		bool BuildProtectedTitleTileMask(
			const std::vector<FrameDefinition>& definitions,
			const std::size_t available_tile_count,
			std::vector<bool>& protected_tiles,
			std::vector<Uint8>& protected_palette_usage,
			std::size_t& protected_tile_count,
			std::string& error
		)
		{
			protected_tiles.assign(kMaximumTitlePatternTiles, false);
			protected_palette_usage.assign(kMaximumTitlePatternTiles, 0U);
			// Pattern 0 must stay permanently blank.  The title-screen Plane A
			// layout uses tile index 0 for every empty/transparent cell.  Reusing
			// this slot for the background makes that pattern repeat over the
			// whole foreground plane and corrupts the imported image in-game.
			protected_tiles[0U] = true;
			for (const FrameDefinition& definition : definitions)
			{
				if (definition.category == TitleScreenCategory::BACKGROUND)
				{
					continue;
				}
				for (const PieceInstance& piece : definition.pieces)
				{
					const std::size_t first_tile =
						piece.effective_attributes & kTileIndexMask;
					const Uint8 palette_line = static_cast<Uint8>(
						(piece.effective_attributes >> 13U) & 3U
					);
					const std::size_t tile_count =
						static_cast<std::size_t>(piece.descriptor.width_in_tiles) *
						piece.descriptor.height_in_tiles;
					if (first_tile >= kMaximumTitlePatternTiles ||
						tile_count > kMaximumTitlePatternTiles - first_tile ||
						first_tile >= available_tile_count ||
						tile_count > available_tile_count - first_tile)
					{
						error = "A non-background title element references graphics outside the loaded title art";
						return false;
					}
					for (std::size_t index = 0U; index < tile_count; ++index)
					{
						protected_tiles[first_tile + index] = true;
						protected_palette_usage[first_tile + index] = static_cast<Uint8>(
							protected_palette_usage[first_tile + index] |
							static_cast<Uint8>(1U << palette_line)
						);
					}
				}
			}
			protected_tile_count = static_cast<std::size_t>(std::count(
				protected_tiles.begin(), protected_tiles.end(), true
			));
			return true;
		}

		bool BuildBackgroundOwnedTileMask(
			const std::vector<Uint8>& rom,
			const Uint32 layout_offset,
			const std::size_t available_tile_count,
			std::vector<bool>& background_owned_tiles,
			std::string& error
		)
		{
			const std::size_t cell_count =
				static_cast<std::size_t>(kTitleBackgroundWidthTiles) *
				kTitleBackgroundHeightTiles;
			if (!CanRead(rom, layout_offset, 4U + cell_count * sizeof(Uint16)) ||
				ReadBE16(rom, layout_offset) != kTitleBackgroundWidthTiles ||
				ReadBE16(rom, layout_offset + 2U) != kTitleBackgroundHeightTiles)
			{
				error = "The reference title-screen background layout is invalid";
				return false;
			}

			background_owned_tiles.assign(kMaximumTitlePatternTiles, false);
			for (std::size_t cell_index = 0U; cell_index < cell_count; ++cell_index)
			{
				const Uint16 attributes = ReadBE16(
					rom,
					layout_offset + 4U +
						static_cast<Uint32>(cell_index * sizeof(Uint16))
				);
				const std::size_t tile_index = attributes & kTileIndexMask;
				if (tile_index == 0U)
				{
					continue;
				}
				if (tile_index >= kMaximumTitlePatternTiles ||
					tile_index >= available_tile_count)
				{
					error = "The reference title-screen background uses graphics outside the loaded title art";
					return false;
				}
				background_owned_tiles[tile_index] = true;
			}
			return true;
		}

		bool PieceFitsArt(const PieceInstance& piece, const std::vector<Uint8>& art)
		{
			const Uint16 absolute_tile = static_cast<Uint16>(
				piece.effective_attributes & kTileIndexMask
			);
			if (absolute_tile < kTitleBaseTile)
			{
				return false;
			}
			const std::size_t first_tile = absolute_tile - kTitleBaseTile;
			const std::size_t tile_count =
				static_cast<std::size_t>(piece.descriptor.width_in_tiles) *
				piece.descriptor.height_in_tiles;
			const std::size_t available_tiles = art.size() / kTileBytes;
			return first_tile <= available_tiles && tile_count <= available_tiles - first_tile;
		}

		std::size_t FilterPiecesForArt(FrameDefinition& definition, const std::vector<Uint8>& art)
		{
			const std::size_t before = definition.pieces.size();
			definition.pieces.erase(
				std::remove_if(
					definition.pieces.begin(),
					definition.pieces.end(),
					[&art](const PieceInstance& piece)
					{
						return !PieceFitsArt(piece, art);
					}
				),
				definition.pieces.end()
			);
			return before - definition.pieces.size();
		}

		int DisplayX(const PieceInstance& piece)
		{
			const int width = static_cast<int>(piece.descriptor.width_in_tiles) * 8;
			const int local_x = (piece.effective_attributes & kHorizontalFlip) != 0U
				? -static_cast<int>(piece.descriptor.x) - width
				: static_cast<int>(piece.descriptor.x);
			return piece.position_x_pixels + local_x;
		}

		int DisplayY(const PieceInstance& piece)
		{
			const int height = static_cast<int>(piece.descriptor.height_in_tiles) * 8;
			const int local_y = (piece.effective_attributes & kVerticalFlip) != 0U
				? -static_cast<int>(piece.descriptor.y) - height
				: static_cast<int>(piece.descriptor.y);
			return piece.position_y_pixels + local_y;
		}

		PixelBounds ComputeBounds(const FrameDefinition& definition)
		{
			PixelBounds bounds;
			bounds.minimum_x = std::numeric_limits<int>::max();
			bounds.minimum_y = std::numeric_limits<int>::max();
			bounds.maximum_x = std::numeric_limits<int>::min();
			bounds.maximum_y = std::numeric_limits<int>::min();
			for (const PieceInstance& piece : definition.pieces)
			{
				const int x = DisplayX(piece);
				const int y = DisplayY(piece);
				const int width = piece.descriptor.width_in_tiles * 8;
				const int height = piece.descriptor.height_in_tiles * 8;
				bounds.minimum_x = std::min(bounds.minimum_x, x);
				bounds.minimum_y = std::min(bounds.minimum_y, y);
				bounds.maximum_x = std::max(bounds.maximum_x, x + width);
				bounds.maximum_y = std::max(bounds.maximum_y, y + height);
			}
			return bounds;
		}

		std::shared_ptr<SpriteTile> BuildSpriteTile(
			const PieceInstance& piece,
			const std::vector<Uint8>& art,
			std::string& error
		)
		{
			if (!PieceFitsArt(piece, art))
			{
				error = "Object piece references tiles outside the title-screen art";
				return nullptr;
			}
			const int width = static_cast<int>(piece.descriptor.width_in_tiles) * 8;
			const int height = static_cast<int>(piece.descriptor.height_in_tiles) * 8;
			const Uint16 absolute_tile = static_cast<Uint16>(
				piece.effective_attributes & kTileIndexMask
			);
			const std::size_t first_tile = absolute_tile - kTitleBaseTile;

			auto sprite_tile = std::make_shared<SpriteTile>();
			sprite_tile->x_size = static_cast<Uint8>(width);
			sprite_tile->y_size = static_cast<Uint8>(height);
			sprite_tile->x_offset = static_cast<Sint16>(DisplayX(piece));
			sprite_tile->y_offset = static_cast<Sint16>(DisplayY(piece));
			sprite_tile->blit_settings.flip_horizontal =
				(piece.effective_attributes & kHorizontalFlip) != 0U;
			sprite_tile->blit_settings.flip_vertical =
				(piece.effective_attributes & kVerticalFlip) != 0U;
			sprite_tile->palette_line = static_cast<Uint8>(
				(piece.effective_attributes >> 13U) & 3U
			);
			sprite_tile->pixel_data.assign(
				static_cast<std::size_t>(width) * height,
				0U
			);

			for (int tile_x = 0; tile_x < piece.descriptor.width_in_tiles; ++tile_x)
			{
				for (int tile_y = 0; tile_y < piece.descriptor.height_in_tiles; ++tile_y)
				{
					const std::size_t tile_index = first_tile +
						static_cast<std::size_t>(tile_x) * piece.descriptor.height_in_tiles +
						static_cast<std::size_t>(tile_y);
					const std::size_t tile_offset = tile_index * kTileBytes;
					for (int pixel_y = 0; pixel_y < 8; ++pixel_y)
					{
						for (int pixel_x = 0; pixel_x < 8; ++pixel_x)
						{
							const Uint8 packed = art[
								tile_offset + static_cast<std::size_t>(pixel_y) * 4U +
								static_cast<std::size_t>(pixel_x / 2)
							];
							const Uint8 colour = (pixel_x & 1) == 0
								? static_cast<Uint8>(packed >> 4U)
								: static_cast<Uint8>(packed & 0x0FU);
							const int destination_x = tile_x * 8 + pixel_x;
							const int destination_y = tile_y * 8 + pixel_y;
							const Uint8 palette_line = static_cast<Uint8>(
								(piece.effective_attributes >> 13U) & 3U
							);
							// Preserve the mapping attribute in the pixel index. Index zero
							// remains the universal transparent value; visible colours use
							// 1-15, 17-31, 33-47 or 49-63.
							const Uint8 combined_colour = colour == 0U
								? 0U
								: static_cast<Uint8>((palette_line * 16U) + colour);
							sprite_tile->pixel_data[
								static_cast<std::size_t>(destination_y) * width + destination_x
							] = combined_colour;
						}
					}
				}
			}
			sprite_tile->header_rom_data.SetROMData(
				piece.descriptor.rom_offset,
				piece.descriptor.rom_offset + 8U
			);
			return sprite_tile;
		}

		std::shared_ptr<const Sprite> BuildSprite(
			const FrameDefinition& definition,
			const std::vector<Uint8>& art,
			std::string& error
		)
		{
			if (definition.pieces.empty())
			{
				error = "Frame contains no title-screen art pieces";
				return nullptr;
			}
			const PixelBounds bounds = ComputeBounds(definition);
			if (bounds.Width() <= 0 || bounds.Height() <= 0 ||
				bounds.Width() > kMaximumFrameDimension ||
				bounds.Height() > kMaximumFrameDimension)
			{
				error = "Frame bounds are invalid or unexpectedly large";
				return nullptr;
			}

			auto sprite = std::make_shared<Sprite>();
			Uint32 total_vdp_tiles = 0U;
			Uint32 minimum_descriptor = std::numeric_limits<Uint32>::max();
			Uint32 maximum_descriptor = 0U;
			for (const PieceInstance& piece : definition.pieces)
			{
				std::shared_ptr<SpriteTile> tile = BuildSpriteTile(piece, art, error);
				if (!tile)
				{
					return nullptr;
				}
				sprite->sprite_tiles.emplace_back(std::move(tile));
				total_vdp_tiles += static_cast<Uint32>(piece.descriptor.width_in_tiles) *
					piece.descriptor.height_in_tiles;
				minimum_descriptor = std::min(minimum_descriptor, piece.descriptor.rom_offset);
				maximum_descriptor = std::max(maximum_descriptor, piece.descriptor.rom_offset + 8U);
			}
			if (sprite->sprite_tiles.empty() ||
				sprite->sprite_tiles.size() > std::numeric_limits<Uint16>::max() ||
				total_vdp_tiles > std::numeric_limits<Uint16>::max())
			{
				error = "Complete title-screen frame has an invalid tile count";
				return nullptr;
			}
			sprite->num_tiles = static_cast<Uint16>(sprite->sprite_tiles.size());
			sprite->num_vdp_tiles = static_cast<Uint16>(total_vdp_tiles);
			sprite->rom_data.SetROMData(minimum_descriptor, maximum_descriptor);
			sprite->is_valid = true;
			return sprite;
		}

		RasterImage RasteriseSprite(const Sprite& sprite)
		{
			RasterImage image;
			if (sprite.sprite_tiles.empty()) return image;
			const auto bounds = sprite.GetBoundingBox();
			image.width = bounds.Width();
			image.height = bounds.Height();
			if (image.width <= 0 || image.height <= 0) return {};
			image.pixels.assign(static_cast<std::size_t>(image.width) * image.height, 0U);
			for (auto piece_it = sprite.sprite_tiles.rbegin();
				piece_it != sprite.sprite_tiles.rend(); ++piece_it)
			{
				const std::shared_ptr<SpriteTile>& piece = *piece_it;
				if (!piece) continue;
				for (int displayed_y = 0; displayed_y < piece->y_size; ++displayed_y)
				{
					for (int displayed_x = 0; displayed_x < piece->x_size; ++displayed_x)
					{
						const int source_x = piece->blit_settings.flip_horizontal
							? piece->x_size - 1 - displayed_x : displayed_x;
						const int source_y = piece->blit_settings.flip_vertical
							? piece->y_size - 1 - displayed_y : displayed_y;
						const Uint8 colour = static_cast<Uint8>(piece->pixel_data[
							static_cast<std::size_t>(source_y) * piece->x_size + source_x
						] & 0x3FU);
						if ((colour & 0x0FU) == 0U) continue;
						const int destination_x = piece->x_offset - bounds.min.x + displayed_x;
						const int destination_y = piece->y_offset - bounds.min.y + displayed_y;
						image.pixels[
							static_cast<std::size_t>(destination_y) * image.width + destination_x
						] = colour;
					}
				}
			}
			return image;
		}

		std::vector<Uint8> BuildPaletteLineMap(const Sprite& sprite)
		{
			std::vector<Uint8> palette_lines;
			if (sprite.sprite_tiles.empty()) return palette_lines;
			const BoundingBox bounds = sprite.GetBoundingBox();
			const int width = bounds.Width();
			const int height = bounds.Height();
			if (width <= 0 || height <= 0) return palette_lines;
			palette_lines.assign(static_cast<std::size_t>(width) * height, 0U);

			for (int image_y = 0; image_y < height; ++image_y)
			{
				for (int image_x = 0; image_x < width; ++image_x)
				{
					std::optional<Uint8> transparent_fallback;
					std::optional<Uint8> visible_owner;
					for (const std::shared_ptr<SpriteTile>& piece : sprite.sprite_tiles)
					{
						if (!piece) continue;
						const int displayed_x = image_x + bounds.min.x - piece->x_offset;
						const int displayed_y = image_y + bounds.min.y - piece->y_offset;
						if (displayed_x < 0 || displayed_x >= piece->x_size ||
							displayed_y < 0 || displayed_y >= piece->y_size)
						{
							continue;
						}

						const int source_x = piece->blit_settings.flip_horizontal
							? piece->x_size - 1 - displayed_x : displayed_x;
						const int source_y = piece->blit_settings.flip_vertical
							? piece->y_size - 1 - displayed_y : displayed_y;
						const Uint8 colour = static_cast<Uint8>(piece->pixel_data[
							static_cast<std::size_t>(source_y) * piece->x_size + source_x
						] & 0x3FU);
						if (!transparent_fallback.has_value())
						{
							transparent_fallback = piece->palette_line;
						}
						if ((colour & 0x0FU) != 0U)
						{
							visible_owner = piece->palette_line;
							break;
						}
					}

					palette_lines[static_cast<std::size_t>(image_y) * width + image_x] =
						visible_owner.value_or(transparent_fallback.value_or(0U));
				}
			}
			return palette_lines;
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

		bool ReadPiecePixel(
			const PieceInstance& piece,
			const std::vector<Uint8>& art,
			const int displayed_local_x,
			const int displayed_local_y,
			Uint8& colour,
			std::size_t& byte_offset,
			bool& high_nibble,
			std::string& error
		)
		{
			const int piece_width = piece.descriptor.width_in_tiles * 8;
			const int piece_height = piece.descriptor.height_in_tiles * 8;
			if (displayed_local_x < 0 || displayed_local_x >= piece_width ||
				displayed_local_y < 0 || displayed_local_y >= piece_height)
			{
				return false;
			}
			if (!PieceFitsArt(piece, art))
			{
				error = "Object piece references tiles outside the title-screen art";
				return false;
			}
			const bool horizontal_flip = (piece.effective_attributes & kHorizontalFlip) != 0U;
			const bool vertical_flip = (piece.effective_attributes & kVerticalFlip) != 0U;
			const int unflipped_x = horizontal_flip
				? piece_width - 1 - displayed_local_x : displayed_local_x;
			const int unflipped_y = vertical_flip
				? piece_height - 1 - displayed_local_y : displayed_local_y;
			const Uint16 absolute_tile = static_cast<Uint16>(
				piece.effective_attributes & kTileIndexMask
			);
			const std::size_t first_tile = absolute_tile - kTitleBaseTile;
			const int tile_x = unflipped_x / 8;
			const int tile_y = unflipped_y / 8;
			const int pixel_x = unflipped_x & 7;
			const int pixel_y = unflipped_y & 7;
			const std::size_t tile_index = first_tile +
				static_cast<std::size_t>(tile_x) * piece.descriptor.height_in_tiles +
				static_cast<std::size_t>(tile_y);
			byte_offset = tile_index * kTileBytes +
				static_cast<std::size_t>(pixel_y) * 4U +
				static_cast<std::size_t>(pixel_x / 2);
			high_nibble = (pixel_x & 1) == 0;
			colour = high_nibble
				? static_cast<Uint8>(art[byte_offset] >> 4U)
				: static_cast<Uint8>(art[byte_offset] & 0x0FU);
			return true;
		}

		void WritePackedPixel(
			std::vector<Uint8>& art,
			const std::size_t byte_offset,
			const bool high_nibble,
			const Uint8 colour
		)
		{
			if (high_nibble)
			{
				art[byte_offset] = static_cast<Uint8>(
					(art[byte_offset] & 0x0FU) | ((colour & 0x0FU) << 4U)
				);
			}
			else
			{
				art[byte_offset] = static_cast<Uint8>(
					(art[byte_offset] & 0xF0U) | (colour & 0x0FU)
				);
			}
		}

		bool WriteFramePixelsToArt(
			const FrameDefinition& definition,
			std::vector<Uint8>& art,
			const std::vector<Uint8>& indexed_pixels,
			const int image_width,
			const int image_height,
			std::size_t& changed_pixel_count,
			std::size_t& conflicting_shared_pixels,
			std::string& error
		)
		{
			changed_pixel_count = 0U;
			conflicting_shared_pixels = 0U;
			if (image_width <= 0 || image_height <= 0)
			{
				error = "PNG dimensions are invalid";
				return false;
			}
			if (indexed_pixels.size() < static_cast<std::size_t>(image_width) * image_height)
			{
				error = "PNG indexed-pixel buffer is smaller than its dimensions";
				return false;
			}
			const PixelBounds bounds = ComputeBounds(definition);
			if (image_width != bounds.Width() || image_height != bounds.Height())
			{
				std::ostringstream message;
				message << "PNG must be exactly " << bounds.Width() << 'x' << bounds.Height()
					<< " pixels for this assembled title frame (received "
					<< image_width << 'x' << image_height << ')';
				error = message.str();
				return false;
			}

			struct Candidate
			{
				std::size_t byte_offset = 0U;
				bool high_nibble = false;
				Uint8 current_colour = 0U;
			};
			struct PixelAssignment
			{
				std::size_t byte_offset = 0U;
				bool high_nibble = false;
				Uint8 colour = 0U;
			};
			std::map<std::size_t, PixelAssignment> changed_pixels;

			for (int image_y = 0; image_y < image_height; ++image_y)
			{
				for (int image_x = 0; image_x < image_width; ++image_x)
				{
					std::optional<Candidate> transparent_fallback;
					std::optional<Candidate> owner;
					for (const PieceInstance& piece : definition.pieces)
					{
						if (!piece.writable) continue;
						const int local_x = image_x + bounds.minimum_x - DisplayX(piece);
						const int local_y = image_y + bounds.minimum_y - DisplayY(piece);
						Uint8 current_colour = 0U;
						std::size_t byte_offset = 0U;
						bool high_nibble = false;
						if (!ReadPiecePixel(
							piece, art, local_x, local_y, current_colour,
							byte_offset, high_nibble, error
						))
						{
							if (!error.empty()) return false;
							continue;
						}
						const Candidate candidate{byte_offset, high_nibble, current_colour};
						if (!transparent_fallback.has_value()) transparent_fallback = candidate;
						if (current_colour != 0U)
						{
							owner = candidate;
							break;
						}
					}
					const std::optional<Candidate>& selected = owner.has_value()
						? owner : transparent_fallback;
					if (!selected.has_value()) continue;
					const Uint8 imported_colour = static_cast<Uint8>(indexed_pixels[
						static_cast<std::size_t>(image_y) * image_width + image_x
					] & 0x0FU);
					if (imported_colour == selected->current_colour) continue;
					const std::size_t source_key = selected->byte_offset * 2U +
						(selected->high_nibble ? 0U : 1U);
					const auto existing = changed_pixels.find(source_key);
					if (existing != changed_pixels.end())
					{
						if (existing->second.colour != imported_colour)
						{
							++conflicting_shared_pixels;
						}
						continue;
					}
					changed_pixels.emplace(source_key, PixelAssignment{
						selected->byte_offset, selected->high_nibble, imported_colour
					});
				}
			}
			for (const auto& [source_key, assignment] : changed_pixels)
			{
				(void)source_key;
				WritePackedPixel(art, assignment.byte_offset, assignment.high_nibble, assignment.colour);
			}
			changed_pixel_count = changed_pixels.size();
			return true;
		}

		bool BuildIndependentBackgroundData(
			const std::vector<Uint8>& current_art,
			const std::vector<Uint8>& reference_art,
			const std::vector<bool>& protected_tiles,
			const std::vector<Uint8>& protected_palette_usage,
			const std::vector<bool>& restore_from_reference,
			const std::vector<Uint8>& rom_data,
			const Uint32 current_layout_offset,
			const std::vector<Uint8>& indexed_pixels,
			const int image_width,
			const int image_height,
			std::vector<Uint8>& output_art,
			std::vector<Uint8>& output_layout,
			std::size_t& independent_tile_count,
			std::string& error
		)
		{
			const int expected_width = static_cast<int>(kTitleBackgroundWidthTiles) * 8;
			const int expected_height = static_cast<int>(kTitleBackgroundHeightTiles) * 8;
			if (image_width != expected_width || image_height != expected_height)
			{
				std::ostringstream message;
				message << "Title background PNG must be exactly "
					<< expected_width << 'x' << expected_height << " pixels (received "
					<< image_width << 'x' << image_height << ')';
				error = message.str();
				return false;
			}
			const std::size_t pixel_count =
				static_cast<std::size_t>(image_width) * image_height;
			if (indexed_pixels.size() < pixel_count)
			{
				error = "Title background indexed-pixel buffer is too small";
				return false;
			}
			const std::size_t current_tile_count = current_art.size() / kTileBytes;
			if (current_art.empty() || (current_art.size() % kTileBytes) != 0U ||
				current_tile_count > kMaximumTitlePatternTiles ||
				protected_tiles.size() != kMaximumTitlePatternTiles ||
				protected_palette_usage.size() != kMaximumTitlePatternTiles ||
				restore_from_reference.size() != kMaximumTitlePatternTiles ||
				reference_art.empty() || (reference_art.size() % kTileBytes) != 0U)
			{
				error = "The current title-screen graphics payload is invalid";
				return false;
			}

			const std::size_t cell_count =
				static_cast<std::size_t>(kTitleBackgroundWidthTiles) *
				kTitleBackgroundHeightTiles;
			if (!CanRead(rom_data, current_layout_offset, 4U + cell_count * 2U) ||
				ReadBE16(rom_data, current_layout_offset) != kTitleBackgroundWidthTiles ||
				ReadBE16(rom_data, current_layout_offset + 2U) != kTitleBackgroundHeightTiles)
			{
				error = "The current title-screen background layout is invalid";
				return false;
			}

			struct PreparedCell
			{
				std::array<Uint8, kTileBytes> packed_tile{};
				Uint8 palette_line = 0U;
				bool visible = false;
			};
			std::vector<PreparedCell> prepared_cells(cell_count);
			independent_tile_count = 0U;
			for (Uint16 tile_y = 0U; tile_y < kTitleBackgroundHeightTiles; ++tile_y)
			{
				for (Uint16 tile_x = 0U; tile_x < kTitleBackgroundWidthTiles; ++tile_x)
				{
					const std::size_t cell_index =
						static_cast<std::size_t>(tile_y) * kTitleBackgroundWidthTiles + tile_x;
					PreparedCell& cell = prepared_cells[cell_index];
					std::optional<Uint8> palette_line;
					for (int pixel_y = 0; pixel_y < 8; ++pixel_y)
					{
						for (int pixel_x = 0; pixel_x < 8; ++pixel_x)
						{
							const int image_x = static_cast<int>(tile_x) * 8 + pixel_x;
							const int image_y = static_cast<int>(tile_y) * 8 + pixel_y;
							const Uint8 combined = indexed_pixels[
								static_cast<std::size_t>(image_y) * image_width + image_x
							];
							const Uint8 local_colour = static_cast<Uint8>(combined & 0x0FU);
							if (local_colour == 0U)
							{
								continue;
							}
							cell.visible = true;
							const Uint8 pixel_line = static_cast<Uint8>((combined >> 4U) & 3U);
							if (palette_line.has_value() && *palette_line != pixel_line)
							{
								error = "A Mega Drive background tile cannot mix palette lines inside one 8x8 cell";
								return false;
							}
							palette_line = pixel_line;
							const std::size_t packed_offset =
								static_cast<std::size_t>(pixel_y) * 4U +
								static_cast<std::size_t>(pixel_x / 2);
							if ((pixel_x & 1) == 0)
							{
								cell.packed_tile[packed_offset] =
									static_cast<Uint8>(local_colour << 4U);
							}
							else
							{
								cell.packed_tile[packed_offset] = static_cast<Uint8>(
									cell.packed_tile[packed_offset] | local_colour
								);
							}
						}
					}
					if (cell.visible)
					{
						cell.palette_line = palette_line.value_or(0U);
						++independent_tile_count;
					}
				}
			}

			std::vector<Uint16> free_slots;
			free_slots.reserve(kMaximumTitlePatternTiles);
			for (std::size_t tile = 0U; tile < kMaximumTitlePatternTiles; ++tile)
			{
				if (!protected_tiles[tile])
				{
					free_slots.emplace_back(static_cast<Uint16>(tile));
				}
			}
			// Transparent background cells always use the permanently blank
			// pattern 0.  They therefore need no extra allocated pattern slot.
			const std::size_t required_slots = independent_tile_count;
			if (required_slots > free_slots.size())
			{
				std::ostringstream message;
				message << "The imported background needs " << required_slots
					<< " free VRAM tile slots, but only " << free_slots.size()
					<< " remain after protecting Sonic, the logos and the bumper. "
					<< "Use transparent cells to reduce the number of independent tiles.";
				error = message.str();
				return false;
			}

			output_art.assign(kMaximumTitlePatternTiles * kTileBytes, 0U);
			std::size_t highest_used_tile = 0U;
			bool has_any_tile = true; // tile 0 is deliberately present and blank
			for (std::size_t tile = 0U; tile < kMaximumTitlePatternTiles; ++tile)
			{
				if (!protected_tiles[tile])
				{
					continue;
				}
				if (tile == 0U)
				{
					// Never copy possibly stale data into the universal blank slot.
					continue;
				}
				const std::vector<Uint8>& source_art = restore_from_reference[tile]
					? reference_art
					: current_art;
				const std::size_t source_tile_count = source_art.size() / kTileBytes;
				if (tile >= source_tile_count)
				{
					error = "A protected title tile is outside the current graphics payload";
					return false;
				}
				std::copy_n(
					source_art.begin() + static_cast<std::ptrdiff_t>(tile * kTileBytes),
					kTileBytes,
					output_art.begin() + static_cast<std::ptrdiff_t>(tile * kTileBytes)
				);

				const Uint8 usage = protected_palette_usage[tile];
				const Uint8 background_line_bit = static_cast<Uint8>(
					1U << kTitleBackgroundPaletteLine
				);
				if ((usage & background_line_bit) != 0U)
				{
					const bool shared_with_other_lines =
						(usage & static_cast<Uint8>(~background_line_bit)) != 0U;
					for (std::size_t byte_index = 0U; byte_index < kTileBytes; ++byte_index)
					{
						Uint8& packed = output_art[tile * kTileBytes + byte_index];
						Uint8 high = static_cast<Uint8>(packed >> 4U);
						Uint8 low = static_cast<Uint8>(packed & 0x0FU);
						const bool uses_repurposed_slot =
							high == kTitleBackgroundLightGreySlot ||
							high == kTitleBackgroundMidGreySlot ||
							low == kTitleBackgroundLightGreySlot ||
							low == kTitleBackgroundMidGreySlot;
						if (shared_with_other_lines && uses_repurposed_slot)
						{
							error = "A protected title tile shares palette line 1 with another palette line while using duplicate colour slots 9 or 10";
							return false;
						}
						if (!shared_with_other_lines)
						{
							if (high == kTitleBackgroundLightGreySlot) high = 6U;
							if (high == kTitleBackgroundMidGreySlot) high = 7U;
							if (low == kTitleBackgroundLightGreySlot) low = 6U;
							if (low == kTitleBackgroundMidGreySlot) low = 7U;
							packed = static_cast<Uint8>((high << 4U) | low);
						}
					}
				}
				highest_used_tile = std::max(highest_used_tile, tile);
				has_any_tile = true;
			}

			std::size_t next_free_slot = 0U;
			constexpr Uint16 blank_tile_index = 0U;

			output_layout.assign(4U + cell_count * 2U, 0U);
			WriteBE16(output_layout, 0U, kTitleBackgroundWidthTiles);
			WriteBE16(output_layout, 2U, kTitleBackgroundHeightTiles);
			for (std::size_t cell_index = 0U; cell_index < cell_count; ++cell_index)
			{
				const Uint16 old_attributes = ReadBE16(
					rom_data,
					current_layout_offset + 4U + static_cast<Uint32>(cell_index * 2U)
				);
				const PreparedCell& cell = prepared_cells[cell_index];
				Uint16 tile_index = blank_tile_index;
				Uint16 palette_bits = static_cast<Uint16>(old_attributes & 0x6000U);
				if (cell.visible)
				{
					tile_index = free_slots[next_free_slot++];
					palette_bits = static_cast<Uint16>(cell.palette_line << 13U);
					std::copy(
						cell.packed_tile.begin(), cell.packed_tile.end(),
						output_art.begin() + static_cast<std::ptrdiff_t>(
							static_cast<std::size_t>(tile_index) * kTileBytes
						)
					);
					highest_used_tile = std::max<std::size_t>(highest_used_tile, tile_index);
					has_any_tile = true;
				}
				const Uint16 new_attributes = static_cast<Uint16>(
					(old_attributes & 0x8000U) | palette_bits | tile_index
				);
				WriteBE16(
					output_layout,
					4U + static_cast<Uint32>(cell_index * 2U),
					new_attributes
				);
			}

			if (!has_any_tile)
			{
				error = "The rebuilt title graphics payload contains no tiles";
				return false;
			}
			output_art.resize((highest_used_tile + 1U) * kTileBytes);
			return true;
		}

		bool ValidateIndependentBackgroundData(
			const std::vector<Uint8>& art,
			const std::vector<Uint8>& layout,
			const std::vector<Uint8>& expected_pixels,
			const int image_width,
			const int image_height,
			std::string& error
		)
		{
			const std::size_t cell_count =
				static_cast<std::size_t>(kTitleBackgroundWidthTiles) *
				kTitleBackgroundHeightTiles;
			if (image_width != static_cast<int>(kTitleBackgroundWidthTiles) * 8 ||
				image_height != static_cast<int>(kTitleBackgroundHeightTiles) * 8 ||
				expected_pixels.size() < static_cast<std::size_t>(image_width) * image_height ||
				layout.size() < 4U + cell_count * 2U ||
				art.empty() || (art.size() % kTileBytes) != 0U)
			{
				error = "The rebuilt title background cannot be validated because its buffers are invalid";
				return false;
			}
			if (ReadBE16(layout, 0U) != kTitleBackgroundWidthTiles ||
				ReadBE16(layout, 2U) != kTitleBackgroundHeightTiles)
			{
				error = "The rebuilt title background layout header changed unexpectedly";
				return false;
			}
			if (!std::all_of(
				art.begin(),
				art.begin() + static_cast<std::ptrdiff_t>(std::min(kTileBytes, art.size())),
				[](const Uint8 value) { return value == 0U; }
			))
			{
				error = "The universal blank title tile (pattern 0) is not empty";
				return false;
			}

			const std::size_t tile_count = art.size() / kTileBytes;
			for (Uint16 tile_y = 0U; tile_y < kTitleBackgroundHeightTiles; ++tile_y)
			{
				for (Uint16 tile_x = 0U; tile_x < kTitleBackgroundWidthTiles; ++tile_x)
				{
					const std::size_t cell_index =
						static_cast<std::size_t>(tile_y) * kTitleBackgroundWidthTiles + tile_x;
					const Uint16 attributes = ReadBE16(
						layout, 4U + static_cast<Uint32>(cell_index * 2U)
					);
					const std::size_t tile_index = attributes & kTileIndexMask;
					if (tile_index >= tile_count)
					{
						std::ostringstream message;
						message << "The rebuilt title layout references missing tile "
							<< tile_index << " at cell " << tile_x << ',' << tile_y;
						error = message.str();
						return false;
					}
					const bool horizontal_flip = (attributes & kHorizontalFlip) != 0U;
					const bool vertical_flip = (attributes & kVerticalFlip) != 0U;
					const Uint8 palette_line = static_cast<Uint8>((attributes >> 13U) & 3U);
					for (int pixel_y = 0; pixel_y < 8; ++pixel_y)
					{
						for (int pixel_x = 0; pixel_x < 8; ++pixel_x)
						{
							const int source_x = horizontal_flip ? 7 - pixel_x : pixel_x;
							const int source_y = vertical_flip ? 7 - pixel_y : pixel_y;
							const std::size_t byte_offset = tile_index * kTileBytes +
								static_cast<std::size_t>(source_y) * 4U +
								static_cast<std::size_t>(source_x / 2);
							const Uint8 packed = art[byte_offset];
							const Uint8 local_colour = (source_x & 1) == 0
								? static_cast<Uint8>(packed >> 4U)
								: static_cast<Uint8>(packed & 0x0FU);
							const Uint8 actual = local_colour == 0U
								? 0U
								: static_cast<Uint8>(palette_line * 16U + local_colour);
							const int image_x = static_cast<int>(tile_x) * 8 + pixel_x;
							const int image_y = static_cast<int>(tile_y) * 8 + pixel_y;
							const Uint8 expected = static_cast<Uint8>(expected_pixels[
								static_cast<std::size_t>(image_y) * image_width + image_x
							] & 0x3FU);
							if (actual != expected)
							{
								std::ostringstream message;
								message << "The rebuilt title background differs from the imported PNG at pixel "
									<< image_x << ',' << image_y << " (expected index "
									<< static_cast<unsigned int>(expected) << ", rebuilt index "
									<< static_cast<unsigned int>(actual) << ')';
								error = message.str();
								return false;
							}
						}
					}
				}
			}
			return true;
		}

		TitleScreenImportResult ImportBackgroundIndexedImage(
			SpinballROM& rom,
			const SpinballROM& reference_rom,
			const std::vector<Uint8>& indexed_pixels,
			const int image_width,
			const int image_height
		)
		{
			TitleScreenImportResult result;
			const Uint32 current_art_header = ResolveTitleArtHeaderOffset(rom.m_buffer);
			const Uint32 current_stream_offset = current_art_header + 2U;
			const Uint32 current_layout_offset =
				ResolveTitleBackgroundLayoutOffset(rom.m_buffer);
			if (!CanRead(rom.m_buffer, current_art_header, 2U) ||
				ReadBE16(rom.m_buffer, current_art_header) != 0xFFFFU)
			{
				result.message = "The current title-screen Compressed2 marker is missing";
				return result;
			}

			std::vector<Uint8> current_art;
			std::size_t current_stream_size = 0U;
			std::string error;
			if (!Compressed2Optimizer::Decode(
				rom.m_buffer, current_stream_offset, current_art, error,
				&current_stream_size, nullptr
			))
			{
				result.message = "Could not decompress the current title-screen art: " + error;
				return result;
			}
			if (current_art.empty() || (current_art.size() % kTileBytes) != 0U)
			{
				result.message = "The current title-screen art is not a complete tile payload";
				return result;
			}

			if (!CanRead(reference_rom.m_buffer, kOriginalTitleArtHeaderOffset, 2U) ||
				ReadBE16(reference_rom.m_buffer, kOriginalTitleArtHeaderOffset) != 0xFFFFU)
			{
				result.message = "Reference ROM has no Compressed2 marker at 0x9D102";
				return result;
			}
			std::vector<Uint8> reference_art;
			if (!Compressed2Optimizer::Decode(
				reference_rom.m_buffer,
				kOriginalTitleArtHeaderOffset + 2U,
				reference_art,
				error,
				nullptr,
				nullptr
			))
			{
				result.message = "Could not decompress the reference title-screen art: " + error;
				return result;
			}
			if (reference_art.empty() || (reference_art.size() % kTileBytes) != 0U ||
				reference_art.size() / kTileBytes > kMaximumTitlePatternTiles)
			{
				result.message = "The reference title-screen art is not a valid tile payload";
				return result;
			}

			if (current_art_header == kReservedTitleArtHeaderOffset &&
				!IsReservedBackgroundInstalled(rom.m_buffer))
			{
				result.message =
					"The fixed title-screen area is present, but its SpinTool metadata is invalid";
				return result;
			}

			std::vector<FrameDefinition> definitions;
			if (!BuildFrameDefinitions(
				rom.m_buffer, current_layout_offset, definitions, error
			))
			{
				result.message = error;
				return result;
			}
			std::vector<bool> protected_tiles;
			std::vector<Uint8> protected_palette_usage;
			std::size_t protected_tile_count = 0U;
			if (!BuildProtectedTitleTileMask(
				definitions, current_art.size() / kTileBytes, protected_tiles,
				protected_palette_usage,
				protected_tile_count, error
			))
			{
				result.message = error;
				return result;
			}

			std::vector<bool> reference_background_tiles;
			if (!BuildBackgroundOwnedTileMask(
				reference_rom.m_buffer,
				kOriginalTitleBackgroundLayout,
				reference_art.size() / kTileBytes,
				reference_background_tiles,
				error
			))
			{
				result.message = error;
				return result;
			}

			// A title tile is recyclable only when the original Plane B layout
			// actually owns it. Preserve every other original title pattern and,
			// when repairing a ROM prepared by an older SpinTool version, restore
			// those safety-only patterns from the clean reference ROM. The three
			// runtime "TM &" patterns are the sole exception: their visible pixels
			// are now imported into Plane B and their old sprite descriptor is
			// disabled, so those slots can safely be reused by the background.
			std::vector<bool> restore_from_reference(kMaximumTitlePatternTiles, false);
			const std::size_t reference_tile_count = reference_art.size() / kTileBytes;
			for (std::size_t tile = 1U; tile < reference_tile_count; ++tile)
			{
				if (!reference_background_tiles[tile] && !protected_tiles[tile] &&
					!IsRuntimeTrademarkTile(tile))
				{
					protected_tiles[tile] = true;
					restore_from_reference[tile] = true;
				}
			}
			protected_tile_count = static_cast<std::size_t>(std::count(
				protected_tiles.begin(), protected_tiles.end(), true
			));

			std::vector<Uint8> independent_art;
			std::vector<Uint8> independent_layout;
			std::size_t independent_tile_count = 0U;
			if (!BuildIndependentBackgroundData(
				current_art, reference_art, protected_tiles,
				protected_palette_usage, restore_from_reference,
				rom.m_buffer, current_layout_offset,
				indexed_pixels, image_width, image_height, independent_art,
				independent_layout, independent_tile_count, error
			))
			{
				result.message = error;
				return result;
			}
			if (!ValidateIndependentBackgroundData(
				independent_art, independent_layout, indexed_pixels,
				image_width, image_height, error
			))
			{
				result.message = "Internal title-background validation failed: " + error;
				return result;
			}

			const std::vector<Uint8> original_stream(
				rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(current_stream_offset),
				rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
					current_stream_offset + current_stream_size
				)
			);
			const Compressed2CompressionResult compression =
				Compressed2Optimizer::Compress(independent_art, original_stream);
			const std::size_t reserved_stream_capacity = kReservedTitleArtRegionSize - 2U;
			if (compression.data.size() > reserved_stream_capacity)
			{
				std::ostringstream message;
				message << "The relocated title art needs " << compression.data.size()
					<< " compressed bytes, but the fixed SpinTool area has "
					<< reserved_stream_capacity << " bytes.";
				result.message = message.str();
				return result;
			}
			if (independent_layout.size() > kReservedTitleBackgroundLayoutSize)
			{
				result.message = "The 40x28 title layout does not fit its fixed reserved area";
				return result;
			}

			std::vector<Uint8> verified_art;
			std::size_t consumed_size = 0U;
			if (!Compressed2Optimizer::Decode(
				compression.data, 0U, verified_art, error, &consumed_size, nullptr
			) || verified_art != independent_art ||
				consumed_size != compression.data.size())
			{
				result.message = "Relocated title-art compression validation failed: " + error;
				return result;
			}

			// Validate the original runtime sprite descriptor before changing any
			// ROM byte.  Its pixels have already been rebuilt into independent
			// Plane B tiles at this point.
			std::vector<Uint8> trademark_validation = rom.m_buffer;
			if (!DisableRuntimeTrademarkSprite(trademark_validation, error))
			{
				result.message = error;
				return result;
			}

			ExpandROMForReservedTitleData(rom.m_buffer);
			if (!CanRead(rom.m_buffer, kReservedTitleArtHeaderOffset,
				kReservedTitleArtRegionSize) ||
				!CanRead(rom.m_buffer, kReservedTitleBackgroundLayout,
					kReservedTitleBackgroundLayoutSize) ||
				!CanRead(rom.m_buffer, kTitleArtPointerOperandOffset, 4U) ||
				!CanRead(rom.m_buffer, kTitleBackgroundLayoutPointerOffset, 4U) ||
				!CanRead(
					rom.m_buffer,
					kTitlePaletteDataOffset +
						static_cast<Uint32>(kTitleBackgroundPaletteLine) *
						Palette::s_palette_size_on_rom,
					Palette::s_palette_size_on_rom
				))
			{
				result.message = "The expanded ROM does not contain the fixed title-screen areas";
				return result;
			}

			WriteBE16(rom.m_buffer, kReservedTitleArtHeaderOffset, 0xFFFFU);
			std::copy(
				compression.data.begin(), compression.data.end(),
				rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
					kReservedTitleArtHeaderOffset + 2U
				)
			);
			std::fill(
				rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
					kReservedTitleArtHeaderOffset + 2U + compression.data.size()
				),
				rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
					kReservedTitleArtHeaderOffset + kReservedTitleArtRegionSize
				),
				0U
			);
			std::copy(
				independent_layout.begin(), independent_layout.end(),
				rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
					kReservedTitleBackgroundLayout
				)
			);
			std::fill(
				rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
					kReservedTitleBackgroundLayout + independent_layout.size()
				),
				rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
					kReservedTitleBackgroundLayout + kReservedTitleBackgroundLayoutSize
				),
				0U
			);
			WriteBE32(rom.m_buffer, kTitleArtPointerOperandOffset,
				kReservedTitleArtHeaderOffset);
			WriteBE32(rom.m_buffer, kTitleBackgroundLayoutPointerOffset,
				kReservedTitleBackgroundLayout);

			const Uint32 background_palette_offset =
				kTitlePaletteDataOffset +
				static_cast<Uint32>(kTitleBackgroundPaletteLine) *
				Palette::s_palette_size_on_rom;
			// Palette line 1 originally contains duplicate copies of colours 6 and 7
			// in slots 9 and 10. Reuse those duplicate slots for the two greys shown
			// by SpinTool's complete title-background export. This preserves every
			// original unique colour while allowing an exported PNG to round-trip
			// without the dark grey becoming black or blue-grey becoming mid-grey.
			WriteBE16(
				rom.m_buffer,
				background_palette_offset +
					static_cast<Uint32>(kTitleBackgroundLightGreySlot) * 2U,
				kTitleBackgroundLightGrey
			);
			WriteBE16(
				rom.m_buffer,
				background_palette_offset +
					static_cast<Uint32>(kTitleBackgroundMidGreySlot) * 2U,
				kTitleBackgroundMidGrey
			);
			if (!DisableRuntimeTrademarkSprite(rom.m_buffer, error))
			{
				result.message = error;
				return result;
			}
			WriteReservedMetadata(
				rom.m_buffer, static_cast<Uint16>(protected_tile_count),
				static_cast<Uint16>(independent_art.size() / kTileBytes)
			);
			UpdateMegaDriveChecksum(rom);

			result.remaining_bytes = reserved_stream_capacity - compression.data.size();
			result.compression_saved_bytes = independent_art.size() > compression.data.size()
				? independent_art.size() - compression.data.size()
				: 0U;
			result.success = true;
			result.changed = true;
			std::ostringstream message;
			message << "Title background relocated to fixed ROM areas 0x"
				<< std::hex << std::uppercase << kReservedTitleArtHeaderOffset
				<< " (art) and 0x" << kReservedTitleBackgroundLayout
				<< " (layout); " << std::dec << independent_tile_count
				<< " visible 8x8 cells now have independent tiles; "
				<< protected_tile_count << " tile slots remain protected for the mandatory blank pattern and every original non-background title element, including Sonic, logos and bumper; TM & is now part of the editable Plane B background and its former runtime sprite is disabled; compressed art "
				<< compression.data.size() << '/' << reserved_stream_capacity
				<< " bytes; title palette line 1 duplicate slots 9 and 10 are reserved for the background greys; all four title palettes remain editable at their original ROM addresses.";
			result.message = message.str();
			return result;
		}

	}

	TitleScreenDecodeResult TitleScreenDecoder::Decode(const SpinballROM& rom)
	{
		TitleScreenDecodeResult result;
		const Uint32 art_header_offset = ResolveTitleArtHeaderOffset(rom.m_buffer);
		const Uint32 compressed_stream_offset = art_header_offset + 2U;
		const Uint32 background_layout_offset =
			ResolveTitleBackgroundLayoutOffset(rom.m_buffer);
		if (!CanRead(rom.m_buffer, art_header_offset, 2U) ||
			ReadBE16(rom.m_buffer, art_header_offset) != 0xFFFFU)
		{
			std::ostringstream message;
			message << "Title-screen Compressed2 marker FFFF is missing at 0x"
				<< std::hex << std::uppercase << art_header_offset;
			result.error = message.str();
			return result;
		}

		std::vector<Uint8> art;
		std::string error;
		if (!Compressed2Optimizer::Decode(
			rom.m_buffer,
			compressed_stream_offset,
			art,
			error,
			nullptr,
			nullptr
		))
		{
			std::ostringstream message;
			message << "Could not decompress title-screen art at 0x"
				<< std::hex << std::uppercase << compressed_stream_offset
				<< ": " << error;
			result.error = message.str();
			return result;
		}
		if (art.empty())
		{
			result.error = "Title-screen art decompressed to an empty tile payload";
			return result;
		}

		auto palette_set = std::make_shared<PaletteSet>();
		for (std::size_t line = 0U; line < palette_set->palette_lines.size(); ++line)
		{
			palette_set->palette_lines[line] = Palette::LoadFromROM(
				rom,
				kTitlePaletteDataOffset + static_cast<Uint32>(line * Palette::s_palette_size_on_rom)
			);
			if (!palette_set->palette_lines[line])
			{
				result.error = "Could not load the four title-screen palette lines";
				return result;
			}
		}
		result.palette_set = std::move(palette_set);

		std::vector<FrameDefinition> definitions;
		if (!BuildFrameDefinitions(rom.m_buffer, background_layout_offset, definitions, error))
		{
			result.error = error;
			return result;
		}

		std::vector<std::pair<TitleScreenCategory, RasterImage>> unique_rasters;
		for (FrameDefinition definition : definitions)
		{
			const std::size_t ignored_pieces = FilterPiecesForArt(definition, art);
			if (ignored_pieces != 0U)
			{
				std::ostringstream warning;
				warning << definition.name << ": ignored " << ignored_pieces
					<< " object piece(s) that use another graphics block";
				result.warnings.emplace_back(warning.str());
			}
			if (definition.pieces.empty()) continue;

			std::shared_ptr<const Sprite> sprite = BuildSprite(definition, art, error);
			if (!sprite)
			{
				result.warnings.emplace_back(definition.name + ": " + error);
				continue;
			}
			const RasterImage raster = RasteriseSprite(*sprite);
			const bool duplicate_in_category = std::any_of(
				unique_rasters.begin(),
				unique_rasters.end(),
				[&definition, &raster](
					const std::pair<TitleScreenCategory, RasterImage>& candidate
				)
				{
					return candidate.first == definition.category && candidate.second == raster;
				}
			);
			if (duplicate_in_category)
			{
				continue;
			}
			unique_rasters.emplace_back(definition.category, raster);
			TitleScreenFrame frame;
			frame.category = definition.category;
			frame.name = std::move(definition.name);
			frame.usage = std::move(definition.usage);
			frame.frame_id = definition.frame_id;
			frame.palette_line_map = BuildPaletteLineMap(*sprite);
			frame.sprite = std::move(sprite);
			result.frames.emplace_back(std::move(frame));
		}
		if (result.frames.empty())
		{
			result.error = "No complete title-screen frame could be assembled";
		}
		return result;
	}

	TitleScreenImportResult TitleScreenDecoder::ImportIndexedImage(
		SpinballROM& rom,
		const SpinballROM& reference_rom,
		const std::size_t frame_id,
		const std::vector<Uint8>& indexed_pixels,
		const int image_width,
		const int image_height
	)
	{
		TitleScreenImportResult result;
		if (frame_id == kTitleBackgroundFrameId)
		{
			return ImportBackgroundIndexedImage(
				rom, reference_rom, indexed_pixels, image_width, image_height
			);
		}

		const Uint32 art_header_offset = ResolveTitleArtHeaderOffset(rom.m_buffer);
		const Uint32 compressed_stream_offset = art_header_offset + 2U;
		const Uint32 background_layout_offset =
			ResolveTitleBackgroundLayoutOffset(rom.m_buffer);
		const bool uses_reserved_art =
			art_header_offset == kReservedTitleArtHeaderOffset &&
			IsReservedBackgroundInstalled(rom.m_buffer);
		if (!CanRead(rom.m_buffer, art_header_offset, 2U) ||
			ReadBE16(rom.m_buffer, art_header_offset) != 0xFFFFU)
		{
			result.message = "The working title-screen Compressed2 marker is missing";
			return result;
		}
		if (!uses_reserved_art &&
			(!CanRead(reference_rom.m_buffer, kOriginalTitleArtHeaderOffset, 2U) ||
			ReadBE16(reference_rom.m_buffer, kOriginalTitleArtHeaderOffset) != 0xFFFFU))
		{
			result.message = "Reference ROM has no Compressed2 marker at 0x9D102";
			return result;
		}

		std::vector<Uint8> tile_art;
		std::size_t current_stream_size = 0U;
		std::string error;
		if (!Compressed2Optimizer::Decode(
			rom.m_buffer,
			compressed_stream_offset,
			tile_art,
			error,
			&current_stream_size,
			nullptr
		))
		{
			result.message = "Could not decompress working title art: " + error;
			return result;
		}
		if (tile_art.empty())
		{
			result.message = "Working title art has no tile payload";
			return result;
		}

		std::vector<FrameDefinition> definitions;
		if (!BuildFrameDefinitions(rom.m_buffer, background_layout_offset, definitions, error))
		{
			result.message = error;
			return result;
		}
		auto definition_it = std::find_if(
			definitions.begin(), definitions.end(),
			[frame_id](const FrameDefinition& definition)
			{
				return definition.frame_id == frame_id;
			}
		);
		if (definition_it == definitions.end())
		{
			result.message = "Selected title-screen frame ID is not available";
			return result;
		}
		FrameDefinition definition = *definition_it;
		FilterPiecesForArt(definition, tile_art);
		if (definition.pieces.empty())
		{
			result.message = "Selected frame contains no piece from the title-screen art block";
			return result;
		}

		std::size_t capacity = 0U;
		if (uses_reserved_art)
		{
			capacity = kReservedTitleArtRegionSize - 2U;
		}
		else
		{
			std::vector<Uint8> reference_tile_art;
			if (!Compressed2Optimizer::Decode(
				reference_rom.m_buffer,
				kOriginalTitleArtHeaderOffset + 2U,
				reference_tile_art,
				error,
				&capacity,
				nullptr
			))
			{
				result.message = "Could not determine exact reference capacity: " + error;
				return result;
			}
			if (kOriginalTitleArtHeaderOffset + 2U > reference_rom.m_buffer.size() ||
				capacity > reference_rom.m_buffer.size() -
					(kOriginalTitleArtHeaderOffset + 2U))
			{
				result.message = "Reference title-art block extends outside the ROM";
				return result;
			}
		}
		if (compressed_stream_offset > rom.m_buffer.size() ||
			current_stream_size > rom.m_buffer.size() - compressed_stream_offset)
		{
			result.message = "Working title-art block extends outside the ROM";
			return result;
		}
		const std::vector<Uint8> original_stream(
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(compressed_stream_offset),
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
				compressed_stream_offset + current_stream_size
			)
		);

		std::size_t changed_pixel_count = 0U;
		std::size_t conflicting_shared_pixels = 0U;
		if (!WriteFramePixelsToArt(
			definition,
			tile_art,
			indexed_pixels,
			image_width,
			image_height,
			changed_pixel_count,
			conflicting_shared_pixels,
			error
		))
		{
			result.message = error;
			return result;
		}
		result.remaining_bytes = capacity >= current_stream_size
			? capacity - current_stream_size
			: 0U;
		result.compression_saved_bytes = tile_art.size() > current_stream_size
			? tile_art.size() - current_stream_size
			: 0U;
		if (changed_pixel_count == 0U)
		{
			result.success = true;
			result.changed = false;
			result.message = "The PNG is identical to the current title frame; the ROM was not modified.";
			return result;
		}

		const Compressed2CompressionResult compression =
			Compressed2Optimizer::Compress(tile_art, original_stream);
		std::vector<Uint8> verified_art;
		std::size_t consumed_size = 0U;
		if (!Compressed2Optimizer::Decode(
			compression.data,
			0U,
			verified_art,
			error,
			&consumed_size,
			nullptr
		))
		{
			result.message = "Optimized Compressed2 validation failed: " + error;
			return result;
		}
		if (verified_art != tile_art || consumed_size != compression.data.size())
		{
			result.message = "Optimized Compressed2 validation produced different title art";
			return result;
		}
		if (compression.data.size() > capacity)
		{
			std::ostringstream message;
			message << "Import refused before writing the ROM. Modified title art needs "
				<< compression.data.size() << " bytes after optimized compression, but "
				<< "the original block only has " << capacity << " bytes ("
				<< compression.data.size() - capacity << " bytes too large; basic "
				<< "compression was " << compression.baseline_size << " bytes).";
			result.message = message.str();
			return result;
		}
		if (compressed_stream_offset > rom.m_buffer.size() ||
			capacity > rom.m_buffer.size() - compressed_stream_offset)
		{
			result.message = "Compressed title-art block extends outside the working ROM";
			return result;
		}

		std::copy(
			compression.data.begin(), compression.data.end(),
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(compressed_stream_offset)
		);
		std::fill(
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
				compressed_stream_offset + compression.data.size()
			),
			rom.m_buffer.begin() + static_cast<std::ptrdiff_t>(
				compressed_stream_offset + capacity
			),
			0U
		);
		UpdateMegaDriveChecksum(rom);

		result.remaining_bytes = capacity - compression.data.size();
		result.compression_saved_bytes = tile_art.size() > compression.data.size()
			? tile_art.size() - compression.data.size()
			: 0U;
		result.success = true;
		result.changed = true;
		std::ostringstream message;
		message << changed_pixel_count
			<< " source pixels changed; optimized size " << compression.data.size()
			<< "/" << capacity << " bytes; strategy " << compression.strategy;
		if (compression.baseline_size > compression.data.size())
		{
			message << "; saved "
				<< compression.baseline_size - compression.data.size()
				<< " bytes versus basic compression";
		}
		if (conflicting_shared_pixels != 0U)
		{
			message << "; " << conflicting_shared_pixels
				<< " conflicting shared appearances kept their first edited colour";
		}
		message << ". All title frames referencing these source tiles use the change.";
		result.message = message.str();
		return result;
	}
}
